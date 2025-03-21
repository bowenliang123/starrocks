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

#include "block_cache/block_cache.h"

#include <gtest/gtest.h>

#include <cstring>

#include "common/logging.h"
#include "common/statusor.h"
#include "fs/fs_util.h"

namespace starrocks {

static const size_t block_size = 1024 * 1024;

class BlockCacheTest : public ::testing::Test {
protected:
    static void SetUpTestCase() {
        ASSERT_TRUE(fs::create_directories("./ut_dir/block_disk_cache").ok());
        BlockCache* cache = BlockCache::instance();

        CacheOptions options;
        options.mem_space_size = 20 * 1024 * 1024;
        size_t quota = 500 * 1024 * 1024;
        options.disk_spaces.push_back({.path = "./ut_dir/block_disk_cache", .size = quota});
        options.block_size = block_size;
        options.engine = "starcache";
        Status status = cache->init(options);
        ASSERT_TRUE(status.ok());
    }

    static void TearDownTestCase() {
        ASSERT_TRUE(fs::remove_all("./ut_dir").ok());
        BlockCache* cache = BlockCache::instance();
        cache->shutdown();
    }

    void SetUp() override {}
    void TearDown() override {}
};

TEST_F(BlockCacheTest, hybrid_cache) {
    std::unique_ptr<BlockCache> cache(new BlockCache);
    const size_t block_size = 1 * 1024 * 1024;

    CacheOptions options;
    options.mem_space_size = 20 * 1024 * 1024;
    size_t quota = 500 * 1024 * 1024;
    options.disk_spaces.push_back({.path = "./ut_dir/block_disk_cache", .size = quota});
    options.block_size = block_size;
    options.engine = "starcache";
    Status status = cache->init(options);
    ASSERT_TRUE(status.ok());

    const size_t batch_size = block_size - 1234;
    const size_t rounds = 20;
    const std::string cache_key = "test_file";

    // write cache
    off_t offset = 0;
    for (size_t i = 0; i < rounds; ++i) {
        char ch = 'a' + i % 26;
        std::string value(batch_size, ch);
        Status st = cache->write_cache(cache_key + std::to_string(i), 0, batch_size, value.c_str());
        ASSERT_TRUE(st.ok());
        offset += batch_size;
    }

    // read cache
    offset = 0;
    for (size_t i = 0; i < rounds; ++i) {
        char ch = 'a' + i % 26;
        std::string expect_value(batch_size, ch);
        char value[batch_size] = {0};
        auto res = cache->read_cache(cache_key + std::to_string(i), 0, batch_size, value);
        ASSERT_TRUE(res.status().ok());
        ASSERT_EQ(memcmp(value, expect_value.c_str(), batch_size), 0);
        offset += batch_size;
    }

    // remove cache
    char value[1024] = {0};
    status = cache->remove_cache(cache_key, 0, batch_size);
    ASSERT_TRUE(status.ok());

    auto res = cache->read_cache(cache_key, 0, batch_size, value);
    ASSERT_TRUE(res.status().is_not_found());

    // not found
    res = cache->read_cache(cache_key, block_size * 1000, batch_size, value);
    ASSERT_TRUE(res.status().is_not_found());
}

TEST_F(BlockCacheTest, write_with_overwrite_option) {
    BlockCache* cache = BlockCache::instance();

    const size_t cache_size = 1024;
    const std::string cache_key = "test_file";

    std::string value(cache_size, 'a');
    Status st = cache->write_cache(cache_key, 0, cache_size, value.c_str());
    ASSERT_TRUE(st.ok());

    std::string value2(cache_size, 'b');
    st = cache->write_cache(cache_key, 0, cache_size, value2.c_str(), 0, true);
    ASSERT_TRUE(st.ok());

    char rvalue[cache_size] = {0};
    auto res = cache->read_cache(cache_key, 0, cache_size, rvalue);
    ASSERT_TRUE(res.status().ok());
    std::string expect_value(cache_size, 'b');
    ASSERT_EQ(memcmp(rvalue, expect_value.c_str(), cache_size), 0);

    std::string value3(cache_size, 'c');
    st = cache->write_cache(cache_key, 0, cache_size, value3.c_str(), 0, false);
    ASSERT_TRUE(st.is_already_exist());
}

TEST_F(BlockCacheTest, auto_create_disk_cache_path) {
    std::unique_ptr<BlockCache> cache(new BlockCache);
    const size_t block_size = 1 * 1024 * 1024;

    CacheOptions options;
    options.mem_space_size = 20 * 1024 * 1024;
    size_t quota = 500 * 1024 * 1024;
    options.disk_spaces.push_back({.path = "./ut_dir/final_entry_not_exist", .size = quota});
    options.block_size = block_size;
    options.engine = "starcache";
    Status status = cache->init(options);
    ASSERT_TRUE(status.ok());

    const size_t batch_size = block_size - 1234;
    const size_t rounds = 3;
    const std::string cache_key = "test_file";

    // write cache
    off_t offset = 0;
    for (size_t i = 0; i < rounds; ++i) {
        char ch = 'a' + i % 26;
        std::string value(batch_size, ch);
        Status st = cache->write_cache(cache_key + std::to_string(i), 0, batch_size, value.c_str());
        ASSERT_TRUE(st.ok());
        offset += batch_size;
    }

    // read cache
    offset = 0;
    for (size_t i = 0; i < rounds; ++i) {
        char ch = 'a' + i % 26;
        std::string expect_value(batch_size, ch);
        char value[batch_size] = {0};
        auto res = cache->read_cache(cache_key + std::to_string(i), 0, batch_size, value);
        ASSERT_TRUE(res.status().ok());
        ASSERT_EQ(memcmp(value, expect_value.c_str(), batch_size), 0);
        offset += batch_size;
    }

    cache->shutdown();
}

} // namespace starrocks
