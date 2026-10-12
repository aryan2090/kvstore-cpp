#include <string>

#include "kv/record.h"

#include "test_util.h"

namespace kv::test {
namespace {

TEST_F(DbTest, PutGetRoundTrip) {
  auto db = DB::open(dir_);
  db->put("a", "1");
  db->put("b", std::string(10000, 'x'));
  db->put("empty", "");
  EXPECT_EQ(db->get("a"), "1");
  EXPECT_EQ(db->get("b"), std::string(10000, 'x'));
  EXPECT_EQ(db->get("empty"), "");
  EXPECT_EQ(db->get("missing"), std::nullopt);
}

TEST_F(DbTest, BinaryKeysAndValues) {
  auto db = DB::open(dir_);
  const std::string key("k\0e\xffy", 5);
  const std::string value("\0\1\2\0", 4);
  db->put(key, value);
  EXPECT_EQ(db->get(key), value);
  EXPECT_EQ(db->get("k"), std::nullopt);
}

TEST_F(DbTest, OverwriteReturnsLatest) {
  auto db = DB::open(dir_);
  db->put("k", "v1");
  db->put("k", "v2");
  db->put("k", "v3");
  EXPECT_EQ(db->get("k"), "v3");
  EXPECT_EQ(db->stats().live_keys, 1u);
}

TEST_F(DbTest, DeleteRemovesKey) {
  auto db = DB::open(dir_);
  db->put("k", "v");
  EXPECT_TRUE(db->del("k"));
  EXPECT_EQ(db->get("k"), std::nullopt);
  EXPECT_FALSE(db->del("k"));
  EXPECT_FALSE(db->del("never-existed"));
  db->put("k", "again");
  EXPECT_EQ(db->get("k"), "again");
}

TEST_F(DbTest, ReopenPersistsPutsOverwritesAndDeletes) {
  {
    auto db = DB::open(dir_);
    for (int i = 0; i < 100; ++i) db->put("key" + std::to_string(i), "v" + std::to_string(i));
    db->put("key5", "overwritten");
    db->del("key7");
  }
  auto db = DB::open(dir_);
  EXPECT_EQ(db->stats().live_keys, 99u);
  EXPECT_EQ(db->get("key0"), "v0");
  EXPECT_EQ(db->get("key99"), "v99");
  EXPECT_EQ(db->get("key5"), "overwritten");
  EXPECT_EQ(db->get("key7"), std::nullopt);
  EXPECT_EQ(db->stats().truncated_bytes, 0u);
}

TEST_F(DbTest, BatchModePersistsAfterCleanClose) {
  Options opts;
  opts.sync_mode = SyncMode::kBatch;
  {
    auto db = DB::open(dir_, opts);
    for (int i = 0; i < 1000; ++i) db->put("k" + std::to_string(i), "v");
  }
  auto db = DB::open(dir_, opts);
  EXPECT_EQ(db->stats().live_keys, 1000u);
}

TEST_F(DbTest, SegmentsRollOverAndSurviveReopen) {
  Options opts;
  opts.max_segment_bytes = 1024;
  {
    auto db = DB::open(dir_, opts);
    for (int i = 0; i < 200; ++i) db->put("key" + std::to_string(i), std::string(50, 'a' + i % 26));
    EXPECT_GT(db->stats().segments, 5u);
  }
  auto db = DB::open(dir_, opts);
  EXPECT_GT(db->stats().segments, 5u);
  for (int i = 0; i < 200; ++i) {
    EXPECT_EQ(db->get("key" + std::to_string(i)), std::string(50, 'a' + i % 26));
  }
  for (const auto& e : std::filesystem::directory_iterator(dir_)) {
    if (e.path().extension() == ".data") {
      EXPECT_LE(e.file_size(), 1024u);
    }
  }
}

TEST_F(DbTest, SecondOpenOfSameDirectoryFails) {
  auto db = DB::open(dir_);
  EXPECT_THROW(DB::open(dir_), IoError);
}

TEST_F(DbTest, RejectsOversizedKey) {
  auto db = DB::open(dir_);
  EXPECT_THROW(db->put(std::string(kMaxKeySize + 1, 'k'), "v"), std::invalid_argument);
}

}  // namespace
}  // namespace kv::test
