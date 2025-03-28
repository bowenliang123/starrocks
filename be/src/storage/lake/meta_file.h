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

#pragma once

#include <string>
#include <unordered_map>

#include "column/vectorized_fwd.h"
#include "fs/fs.h"
#include "storage/lake/tablet.h"
#include "storage/lake/tablet_metadata.h"
#include "storage/lake/types_fwd.h"
#include "storage/olap_common.h"

namespace starrocks {

class DelVector;

namespace lake {

class UpdateManager;

class MetaFileBuilder {
public:
    explicit MetaFileBuilder(Tablet tablet, std::shared_ptr<TabletMetadata> metadata_ptr);
    // append delvec to builder's buffer
    void append_delvec(DelVectorPtr delvec, uint32_t segment_id);
    // handle txn log
    void apply_opwrite(const TxnLogPB_OpWrite& op_write);
    void apply_opcompaction(const TxnLogPB_OpCompaction& op_compaction);
    // finalize will generate and sync final meta state to storage.
    Status finalize();
    // find delvec in builder's buffer, used for batch txn log precess.
    StatusOr<bool> find_delvec(const TabletSegmentId& tsid, DelVectorPtr* pdelvec) const;
    // when apply or finalize fail, need to clear primary index cache
    void handle_failure();
    bool has_update_index() const { return _has_update_index; }

private:
    Status _finalize_delvec(int64_t version);

private:
    Tablet _tablet;
    std::shared_ptr<TabletMetadata> _tablet_meta;
    UpdateManager* _update_mgr;
    Buffer<uint8_t> _buf;
    std::unordered_map<uint32_t, DelvecPagePB> _delvecs;
    // whether finalize meta file success.
    bool _has_finalized = false;
    // whether update the state of pk index.
    bool _has_update_index = false;
};

class MetaFileReader {
public:
    explicit MetaFileReader(const std::string& filepath, bool fill_cache);
    ~MetaFileReader() {}
    Status load();
    Status get_del_vec(TabletManager* tablet_mgr, uint32_t segment_id, DelVector* delvec);
    StatusOr<TabletMetadataPtr> get_meta();

private:
    std::unique_ptr<RandomAccessFile> _access_file;
    std::shared_ptr<TabletMetadata> _tablet_meta;
    Status _err_status;
    bool _load;
};

bool is_primary_key(TabletMetadata* metadata);
bool is_primary_key(const TabletMetadata& metadata);

// TODO(yixin): cache rowset_rssid_to_path
void rowset_rssid_to_path(const TabletMetadata& metadata, const TxnLogPB_OpWrite& op_write,
                          std::unordered_map<uint32_t, std::string>& rssid_to_path);

} // namespace lake
} // namespace starrocks
