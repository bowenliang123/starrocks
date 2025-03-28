// Copyright 2021-present StarRocks, Inc. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     https://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <gtest/gtest.h>

#include <random>

#include "column/chunk.h"
#include "column/datum_tuple.h"
#include "column/fixed_length_column.h"
#include "column/schema.h"
#include "column/vectorized_fwd.h"
#include "common/logging.h"
#include "fs/fs_util.h"
#include "storage/chunk_helper.h"
#include "storage/lake/delta_writer.h"
#include "storage/lake/fixed_location_provider.h"
#include "storage/lake/join_path.h"
#include "storage/lake/location_provider.h"
#include "storage/lake/meta_file.h"
#include "storage/lake/tablet.h"
#include "storage/lake/tablet_manager.h"
#include "storage/lake/tablet_reader.h"
#include "storage/lake/tablet_writer.h"
#include "storage/rowset/segment.h"
#include "storage/rowset/segment_iterator.h"
#include "storage/rowset/segment_options.h"
#include "storage/tablet_schema.h"
#include "testutil/assert.h"
#include "testutil/id_generator.h"

namespace starrocks::lake {

using VSchema = starrocks::Schema;
using VChunk = starrocks::Chunk;

class TestLocationProvider : public LocationProvider {
public:
    explicit TestLocationProvider(std::string dir) : _dir(dir) {}

    std::set<int64_t> owned_tablets() const override { return _owned_shards; }

    std::string root_location(int64_t tablet_id) const override { return _dir; }

    Status list_root_locations(std::set<std::string>* roots) const override {
        roots->insert(_dir);
        return Status::OK();
    }

    void set_failed(bool f) { _set_failed = f; }

    std::set<int64_t> _owned_shards;
    std::string _dir;
    bool _set_failed = false;
};

class PrimaryKeyPublishTest : public testing::Test {
public:
    PrimaryKeyPublishTest() {
        _location_provider = std::make_unique<TestLocationProvider>(kTestGroupPath);
        _update_manager = std::make_unique<UpdateManager>(_location_provider.get());
        _tablet_manager = std::make_unique<TabletManager>(_location_provider.get(), _update_manager.get(), 1024 * 1024);

        _tablet_metadata = std::make_unique<TabletMetadata>();
        _tablet_metadata->set_id(next_id());
        _tablet_metadata->set_version(1);
        _tablet_metadata->set_next_rowset_id(1);
        _location_provider->_owned_shards.insert(_tablet_metadata->id());

        _backup_location_provider = _tablet_manager->TEST_set_location_provider(_location_provider.get());

        _parent_mem_tracker = std::make_unique<MemTracker>(-1);
        _mem_tracker = std::make_unique<MemTracker>(-1, "", _parent_mem_tracker.get());
        //
        //  | column | type | KEY | NULL |
        //  +--------+------+-----+------+
        //  |   c0   |  INT | YES |  NO  |
        //  |   c1   |  INT | NO  |  NO  |
        auto schema = _tablet_metadata->mutable_schema();
        schema->set_id(next_id());
        schema->set_num_short_key_columns(1);
        schema->set_keys_type(PRIMARY_KEYS);
        schema->set_num_rows_per_row_block(65535);
        auto c0 = schema->add_column();
        {
            c0->set_unique_id(next_id());
            c0->set_name("c0");
            c0->set_type("INT");
            c0->set_is_key(true);
            c0->set_is_nullable(false);
        }
        auto c1 = schema->add_column();
        {
            c1->set_unique_id(next_id());
            c1->set_name("c1");
            c1->set_type("INT");
            c1->set_is_key(false);
            c1->set_is_nullable(false);
            c1->set_aggregation("REPLACE");
        }

        _tablet_schema = TabletSchema::create(*schema);
        _schema = std::make_shared<VSchema>(ChunkHelper::convert_schema(*_tablet_schema));
    }

    void SetUp() override {
        (void)fs::remove_all(kTestGroupPath);
        CHECK_OK(fs::create_directories(lake::join_path(kTestGroupPath, lake::kSegmentDirectoryName)));
        CHECK_OK(fs::create_directories(lake::join_path(kTestGroupPath, lake::kMetadataDirectoryName)));
        CHECK_OK(fs::create_directories(lake::join_path(kTestGroupPath, lake::kTxnLogDirectoryName)));
        CHECK_OK(_tablet_manager->put_tablet_metadata(*_tablet_metadata));
    }

    void TearDown() override {
        // check primary index cache's ref
        EXPECT_TRUE(_update_manager->TEST_check_primary_index_cache_ref(_tablet_metadata->id(), 1));
        ASSIGN_OR_ABORT(auto tablet, _tablet_manager->get_tablet(_tablet_metadata->id()));
        tablet.delete_txn_log(_txn_id);
        _txn_id++;
        (void)ExecEnv::GetInstance()->lake_tablet_manager()->TEST_set_location_provider(_backup_location_provider);
        (void)fs::remove_all(kTestGroupPath);
    }

    VChunk generate_data(int64_t chunk_size, int shift) {
        std::vector<int> v0(chunk_size);
        std::vector<int> v1(chunk_size);
        for (int i = 0; i < chunk_size; i++) {
            v0[i] = i + shift * chunk_size;
        }
        auto rng = std::default_random_engine{};
        std::shuffle(v0.begin(), v0.end(), rng);
        for (int i = 0; i < chunk_size; i++) {
            v1[i] = v0[i] * 3;
        }

        auto c0 = Int32Column::create();
        auto c1 = Int32Column::create();
        c0->append_numbers(v0.data(), v0.size() * sizeof(int));
        c1->append_numbers(v1.data(), v1.size() * sizeof(int));
        return VChunk({c0, c1}, _schema);
    }

    int64_t read(int64_t version) {
        ASSIGN_OR_ABORT(auto tablet, _tablet_manager->get_tablet(_tablet_metadata->id()));
        ASSIGN_OR_ABORT(auto reader, tablet.new_reader(version, *_schema));
        CHECK_OK(reader->prepare());
        CHECK_OK(reader->open(TabletReaderParams()));
        auto chunk = ChunkHelper::new_chunk(*_schema, 128);
        int64_t ret = 0;
        while (true) {
            auto st = reader->get_next(chunk.get());
            if (st.is_end_of_file()) {
                break;
            }
            CHECK_OK(st);
            ret += chunk->num_rows();
            chunk->reset();
        }
        return ret;
    }

protected:
    constexpr static const char* const kTestGroupPath = "test_lake_primary_key";
    constexpr static const int kChunkSize = 12;

    std::unique_ptr<TestLocationProvider> _location_provider;
    LocationProvider* _backup_location_provider;
    std::unique_ptr<TabletManager> _tablet_manager;
    std::unique_ptr<UpdateManager> _update_manager;
    std::unique_ptr<TabletMetadata> _tablet_metadata;
    std::shared_ptr<TabletSchema> _tablet_schema;
    std::unique_ptr<MemTracker> _parent_mem_tracker;
    std::unique_ptr<MemTracker> _mem_tracker;
    std::shared_ptr<VSchema> _schema;
    int64_t _txn_id = 1231;
    int64_t _partition_id = 4561;
};

TEST_F(PrimaryKeyPublishTest, test_write_read_success) {
    std::vector<int> k0{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};
    std::vector<int> v0{2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24, 26, 28, 30, 32, 34, 36, 38, 40, 41, 44};

    auto c0 = Int32Column::create();
    auto c1 = Int32Column::create();
    c0->append_numbers(k0.data(), k0.size() * sizeof(int));
    c1->append_numbers(v0.data(), v0.size() * sizeof(int));

    VChunk chunk0({c0, c1}, _schema);
    auto rowset_txn_meta = std::make_unique<RowsetTxnMetaPB>();

    ASSIGN_OR_ABORT(auto tablet, _tablet_manager->get_tablet(_tablet_metadata->id()));
    std::shared_ptr<const TabletSchema> const_schema = _tablet_schema;
    ASSIGN_OR_ABORT(auto writer, tablet.new_writer(kHorizontal));
    ASSERT_OK(writer->open());

    // write segment #1
    ASSERT_OK(writer->write(chunk0));
    ASSERT_OK(writer->finish());

    // write txnlog
    int64_t logs[1];
    logs[0] = _txn_id;
    auto txn_log = std::make_shared<TxnLog>();
    txn_log->set_tablet_id(_tablet_metadata->id());
    txn_log->set_txn_id(_txn_id++);
    auto op_write = txn_log->mutable_op_write();
    for (auto& f : writer->files()) {
        op_write->mutable_rowset()->add_segments(std::move(f));
    }
    op_write->mutable_rowset()->set_num_rows(writer->num_rows());
    op_write->mutable_rowset()->set_data_size(writer->data_size());
    op_write->mutable_rowset()->set_overlapped(false);

    ASSERT_OK(_tablet_manager->put_txn_log(txn_log));

    writer->close();

    ASSIGN_OR_ABORT(auto score, _tablet_manager->publish_version(_tablet_metadata->id(), 1, 2, logs, 1));
    EXPECT_TRUE(score > 0.0);

    // read at version 2
    ASSIGN_OR_ABORT(auto reader, tablet.new_reader(2, *_schema));
    ASSERT_OK(reader->prepare());
    TabletReaderParams params;
    ASSERT_OK(reader->open(params));

    auto read_chunk_ptr = ChunkHelper::new_chunk(*_schema, 1024);
    ASSERT_OK(reader->get_next(read_chunk_ptr.get()));
    ASSERT_EQ(k0.size(), read_chunk_ptr->num_rows());

    for (int i = 0, sz = k0.size(); i < sz; i++) {
        EXPECT_EQ(k0[i], read_chunk_ptr->get(i)[0].get_int32());
        EXPECT_EQ(v0[i], read_chunk_ptr->get(i)[1].get_int32());
    }
}

TEST_F(PrimaryKeyPublishTest, test_write_multitime_check_result) {
    auto chunk0 = generate_data(kChunkSize, 0);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    for (int i = 0; i < 3; i++) {
        _txn_id++;
        auto delta_writer = DeltaWriter::create(_tablet_manager.get(), tablet_id, _txn_id, _partition_id, nullptr,
                                                _mem_tracker.get());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish());
        delta_writer->close();
        // Publish version
        ASSERT_OK(_tablet_manager->publish_version(tablet_id, version, version + 1, &_txn_id, 1).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, read(version));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_manager->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);
}

TEST_F(PrimaryKeyPublishTest, test_write_fail_retry) {
    std::vector<Chunk> chunks;
    for (int i = 0; i < 5; i++) {
        chunks.push_back(generate_data(kChunkSize, i));
    }
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    // write success
    for (int i = 0; i < 3; i++) {
        _txn_id++;
        auto delta_writer = DeltaWriter::create(_tablet_manager.get(), tablet_id, _txn_id, _partition_id, nullptr,
                                                _mem_tracker.get());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunks[i], indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish());
        delta_writer->close();
        // Publish version
        ASSERT_OK(_tablet_manager->publish_version(tablet_id, version, version + 1, &_txn_id, 1).status());
        version++;
    }
    // write failed
    for (int i = 3; i < 5; i++) {
        _txn_id++;
        auto delta_writer = DeltaWriter::create(_tablet_manager.get(), tablet_id, _txn_id, _partition_id, nullptr,
                                                _mem_tracker.get());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunks[i], indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish());
        delta_writer->close();
        ASSIGN_OR_ABORT(auto tablet, _tablet_manager->get_tablet(tablet_id));
        auto txn_log_st = tablet.get_txn_log(_txn_id);
        EXPECT_TRUE(txn_log_st.ok());
        auto& txn_log = txn_log_st.value();
        ASSIGN_OR_ABORT(auto base_metadata, tablet.get_metadata(version));
        auto new_metadata = std::make_shared<TabletMetadata>(*base_metadata);
        new_metadata->set_version(version + 1);
        std::unique_ptr<MetaFileBuilder> builder = std::make_unique<MetaFileBuilder>(tablet, new_metadata);
        // update primary table state, such as primary index
        ASSERT_OK(tablet.update_mgr()->publish_primary_key_tablet(txn_log->op_write(), txn_log->txn_id(), *new_metadata,
                                                                  &tablet, builder.get(), version));
        // if builder.finalize fail, remove primary index cache and retry
        builder->handle_failure();
    }
    // write success
    for (int i = 3; i < 5; i++) {
        _txn_id++;
        auto delta_writer = DeltaWriter::create(_tablet_manager.get(), tablet_id, _txn_id, _partition_id, nullptr,
                                                _mem_tracker.get());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunks[i], indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish());
        delta_writer->close();
        // Publish version
        ASSERT_OK(_tablet_manager->publish_version(tablet_id, version, version + 1, &_txn_id, 1).status());
        version++;
    }
    ASSERT_EQ(kChunkSize * 5, read(version));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_manager->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 5);
}

TEST_F(PrimaryKeyPublishTest, test_publish_multi_times) {
    auto chunk0 = generate_data(kChunkSize, 0);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    for (int i = 0; i < 3; i++) {
        _txn_id++;
        auto delta_writer = DeltaWriter::create(_tablet_manager.get(), tablet_id, _txn_id, _partition_id, nullptr,
                                                _mem_tracker.get());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish());
        delta_writer->close();
        // Publish version
        ASSERT_OK(_tablet_manager->publish_version(tablet_id, version, version + 1, &_txn_id, 1).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, read(version));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_manager->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);
    // duplicate publish
    ASSERT_OK(_tablet_manager->publish_version(tablet_id, version - 1, version, &_txn_id, 1).status());
    // publish using old version
    ASSERT_OK(_tablet_manager->publish_version(tablet_id, version - 2, version - 1, &_txn_id, 1).status());
    // advince publish should fail, because version + 1 don't exist
    ASSERT_ERROR(_tablet_manager->publish_version(tablet_id, version + 1, version + 2, &_txn_id, 1).status());
    ASSERT_EQ(kChunkSize, read(version));
}

TEST_F(PrimaryKeyPublishTest, test_publish_concurrent) {
    auto chunk0 = generate_data(kChunkSize, 0);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    for (int i = 0; i < 3; i++) {
        _txn_id++;
        auto delta_writer = DeltaWriter::create(_tablet_manager.get(), tablet_id, _txn_id, _partition_id, nullptr,
                                                _mem_tracker.get());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish());
        delta_writer->close();
        // start to publish using multi thread
        std::vector<std::thread> workers;
        for (int i = 0; i < 5; i++) {
            workers.emplace_back(
                    [&]() { (void)_tablet_manager->publish_version(tablet_id, version, version + 1, &_txn_id, 1); });
        }
        for (auto& t : workers) {
            t.join();
        }
        version++;
    }
    ASSERT_EQ(kChunkSize, read(version));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_manager->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);
}

TEST_F(PrimaryKeyPublishTest, test_resolve_conflict) {
    auto chunk0 = generate_data(kChunkSize, 0);
    auto indexes = std::vector<uint32_t>(kChunkSize);
    for (int i = 0; i < kChunkSize; i++) {
        indexes[i] = i;
    }

    auto version = 1;
    auto tablet_id = _tablet_metadata->id();
    for (int i = 0; i < 3; i++) {
        _txn_id++;
        auto delta_writer = DeltaWriter::create(_tablet_manager.get(), tablet_id, _txn_id, _partition_id, nullptr,
                                                _mem_tracker.get());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        ASSERT_OK(delta_writer->finish());
        delta_writer->close();
        // Publish version
        ASSERT_OK(_tablet_manager->publish_version(tablet_id, version, version + 1, &_txn_id, 1).status());
        version++;
    }
    ASSERT_EQ(kChunkSize, read(version));
    ASSIGN_OR_ABORT(auto new_tablet_metadata, _tablet_manager->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 3);

    // concurrent write
    for (int i = 0; i < 3; i++) {
        _txn_id++;
        auto delta_writer = DeltaWriter::create(_tablet_manager.get(), tablet_id, _txn_id, _partition_id, nullptr,
                                                _mem_tracker.get());
        ASSERT_OK(delta_writer->open());
        ASSERT_OK(delta_writer->write(chunk0, indexes.data(), indexes.size()));
        // will preload update state here.
        ASSERT_OK(delta_writer->finish());
        delta_writer->close();
    }
    // publish in order
    for (int i = _txn_id - 2; i <= _txn_id; i++) {
        // Publish version
        const int64_t ctxnid = i;
        ASSERT_OK(_tablet_manager->publish_version(tablet_id, version, version + 1, &ctxnid, 1).status());
        version++;
    }
    // check result
    ASSERT_EQ(kChunkSize, read(version));
    ASSIGN_OR_ABORT(new_tablet_metadata, _tablet_manager->get_tablet_metadata(tablet_id, version));
    EXPECT_EQ(new_tablet_metadata->rowsets_size(), 6);
}

} // namespace starrocks::lake
