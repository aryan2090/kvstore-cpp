#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>

#include "test_util.h"

namespace kv::test {
namespace {

namespace fs = std::filesystem;

uint64_t data_bytes(const fs::path& dir) {
  uint64_t total = 0;
  for (const auto& e : fs::directory_iterator(dir)) {
    if (e.path().extension() == ".data") total += e.file_size();
  }
  return total;
}

// Writes 50 keys x 20 versions, then deletes every 5th key. Returns the expected state.
std::map<std::string, std::string> write_churn(DB& db) {
  std::map<std::string, std::string> expected;
  for (int round = 0; round < 20; ++round) {
    for (int k = 0; k < 50; ++k) {
      const std::string key = "key" + std::to_string(k);
      const std::string value = "value-" + std::to_string(k) + "-" + std::to_string(round);
      db.put(key, value);
      expected[key] = value;
    }
  }
  for (int k = 0; k < 50; k += 5) {
    db.del("key" + std::to_string(k));
    expected.erase("key" + std::to_string(k));
  }
  return expected;
}

void expect_state(DB& db, const std::map<std::string, std::string>& expected) {
  EXPECT_EQ(db.stats().live_keys, expected.size());
  for (int k = 0; k < 50; ++k) {
    const std::string key = "key" + std::to_string(k);
    auto it = expected.find(key);
    if (it == expected.end()) {
      EXPECT_EQ(db.get(key), std::nullopt) << key;
    } else {
      EXPECT_EQ(db.get(key), it->second) << key;
    }
  }
}

Options small_segments() {
  Options opts;
  opts.max_segment_bytes = 4096;
  opts.sync_mode = SyncMode::kBatch;  // a clean close still syncs; keeps the tests fast
  return opts;
}

TEST_F(DbTest, CompactionKeepsLatestValuesAndShrinksLog) {
  std::map<std::string, std::string> expected;
  {
    auto db = DB::open(dir_, small_segments());
    expected = write_churn(*db);
    const uint64_t before = data_bytes(dir_);
    const size_t segs_before = db->stats().segments;

    const auto r = db->compact();
    EXPECT_EQ(r.segments_compacted, segs_before);
    EXPECT_EQ(r.bytes_before, before);
    EXPECT_LT(r.bytes_after * 10, r.bytes_before);  // 40 live of ~1000 records
    EXPECT_LT(data_bytes(dir_) * 10, before);
    EXPECT_EQ(db->stats().segments, 2u);  // compacted + new active
    expect_state(*db, expected);

    // Writes after compaction land in the new active segment and win on reopen.
    db->put("key1", "after-compaction");
    expected["key1"] = "after-compaction";
  }
  auto db = DB::open(dir_, small_segments());
  expect_state(*db, expected);
}

TEST_F(DbTest, CompactingTwiceIsStable) {
  auto db = DB::open(dir_, small_segments());
  const auto expected = write_churn(*db);
  db->compact();
  const uint64_t once = data_bytes(dir_);
  const auto r = db->compact();
  EXPECT_EQ(r.bytes_after, r.bytes_before);
  EXPECT_EQ(data_bytes(dir_), once);
  expect_state(*db, expected);
}

TEST_F(DbTest, CompactionOfEmptyStoreIsNoOp) {
  auto db = DB::open(dir_);
  const auto r = db->compact();
  EXPECT_EQ(r.segments_compacted, 0u);
  db->put("a", "1");
  EXPECT_EQ(db->get("a"), "1");
}

TEST_F(DbTest, CrashBeforeCommitLeavesOldDataIntact) {
  std::map<std::string, std::string> expected;
  {
    auto db = DB::open(dir_, small_segments());
    expected = write_churn(*db);
  }
  // A compaction that died while writing its output leaves a partial temp file.
  const fs::path tmp = dir_ / "0000000003.compact.tmp";
  write_bytes(tmp, std::vector<char>(1000, 'x'));

  auto db = DB::open(dir_, small_segments());
  EXPECT_FALSE(fs::exists(tmp));
  expect_state(*db, expected);
}

// Builds the on-disk state of a compaction that crashed after its commit point:
// all old segments still present plus the finished NNN.compact file. Returns the
// compacted segment id.
uint32_t simulate_crash_after_commit(const fs::path& dir, const fs::path& scratch) {
  fs::remove_all(scratch);
  fs::copy(dir, scratch);
  {
    auto db = DB::open(scratch, small_segments());
    db->compact();
  }
  // The compacted segment is the lowest-numbered .data file left in scratch.
  uint32_t out_id = UINT32_MAX;
  for (const auto& e : fs::directory_iterator(scratch)) {
    if (e.path().extension() == ".data") {
      out_id = std::min(out_id, static_cast<uint32_t>(std::stoul(e.path().stem().string())));
    }
  }
  const std::string stem = DB::segment_name(out_id).substr(0, 10);
  fs::copy_file(scratch / DB::segment_name(out_id), dir / (stem + ".compact"));
  fs::remove_all(scratch);
  return out_id;
}

TEST_F(DbTest, CrashAfterCommitIsRolledForward) {
  std::map<std::string, std::string> expected;
  {
    auto db = DB::open(dir_, small_segments());
    expected = write_churn(*db);
  }
  const uint64_t before = data_bytes(dir_);
  const uint32_t out_id = simulate_crash_after_commit(dir_, dir_.string() + "-scratch");

  {
    auto db = DB::open(dir_, small_segments());
    expect_state(*db, expected);
  }
  EXPECT_LT(data_bytes(dir_) * 10, before);
  EXPECT_FALSE(fs::exists(dir_ / (DB::segment_name(out_id).substr(0, 10) + ".compact")));
  for (uint32_t id = 1; id < out_id; ++id) EXPECT_FALSE(fs::exists(seg(id)));
}

TEST_F(DbTest, CrashDuringCleanupIsRolledForward) {
  std::map<std::string, std::string> expected;
  {
    auto db = DB::open(dir_, small_segments());
    expected = write_churn(*db);
  }
  const uint32_t out_id = simulate_crash_after_commit(dir_, dir_.string() + "-scratch");
  // Cleanup had already deleted some old segments, including the one being replaced.
  fs::remove(seg(1));
  fs::remove(seg(out_id));

  auto db = DB::open(dir_, small_segments());
  expect_state(*db, expected);
}

}  // namespace
}  // namespace kv::test
