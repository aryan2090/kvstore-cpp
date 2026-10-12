#include <filesystem>
#include <string>

#include "kv/record.h"
#include "test_util.h"

namespace kv::test {
namespace {

namespace fs = std::filesystem;

// Size of the record put(key, value) writes.
size_t record_size(const std::string& key, const std::string& value) {
  return kHeaderSize + key.size() + value.size();
}

TEST_F(DbTest, TornWriteAtEveryCutPointIsTruncated) {
  {
    auto db = DB::open(dir_);
    db->put("a", "first");
    db->put("b", "second");
  }
  const auto good = read_bytes(seg(1));
  const size_t last = record_size("b", "second");
  ASSERT_EQ(good.size(), record_size("a", "first") + last);

  // Simulate a crash part-way through writing record "b": keep 1..last-1 of its bytes.
  for (size_t keep = 1; keep < last; ++keep) {
    std::vector<char> torn(good.begin(), good.end() - static_cast<long>(last - keep));
    write_bytes(seg(1), torn);
    {
      auto db = DB::open(dir_);
      EXPECT_EQ(db->get("a"), "first") << "keep=" << keep;
      EXPECT_EQ(db->get("b"), std::nullopt) << "keep=" << keep;
      EXPECT_EQ(db->stats().truncated_bytes, keep);
      // Appending after recovery must produce a clean log again.
      db->put("c", "third");
    }
    auto db = DB::open(dir_);
    EXPECT_EQ(db->stats().truncated_bytes, 0u);
    EXPECT_EQ(db->get("a"), "first");
    EXPECT_EQ(db->get("c"), "third");
  }
}

TEST_F(DbTest, ZeroFilledTailIsTruncated) {
  // A crash can leave the file extended with zeros (size updated, data not written).
  {
    auto db = DB::open(dir_);
    db->put("a", "1");
  }
  auto bytes = read_bytes(seg(1));
  const size_t good = bytes.size();
  bytes.resize(good + 4096, '\0');
  write_bytes(seg(1), bytes);

  auto db = DB::open(dir_);
  EXPECT_EQ(db->get("a"), "1");
  EXPECT_EQ(db->stats().truncated_bytes, 4096u);
  EXPECT_EQ(fs::file_size(seg(1)), good);
}

TEST_F(DbTest, FlippedByteInLastRecordDropsOnlyTheTail) {
  {
    auto db = DB::open(dir_);
    db->put("a", "first");
    db->put("b", "second");
  }
  auto bytes = read_bytes(seg(1));
  bytes[bytes.size() - 2] ^= 0x40;  // inside the value of "b"
  write_bytes(seg(1), bytes);

  auto db = DB::open(dir_);
  EXPECT_EQ(db->get("a"), "first");
  EXPECT_EQ(db->get("b"), std::nullopt);
  EXPECT_EQ(db->stats().truncated_bytes, record_size("b", "second"));
}

TEST_F(DbTest, CorruptionInSealedSegmentFailsOpen) {
  Options opts;
  opts.max_segment_bytes = 64;
  {
    auto db = DB::open(dir_, opts);
    for (int i = 0; i < 10; ++i) db->put("key" + std::to_string(i), std::string(30, 'v'));
  }
  ASSERT_TRUE(fs::exists(seg(2)));
  auto bytes = read_bytes(seg(1));
  bytes[kHeaderSize + 1] ^= 0x01;
  write_bytes(seg(1), bytes);
  // Silently dropping data from the middle of the log would be wrong; refuse instead.
  EXPECT_THROW(DB::open(dir_, opts), CorruptionError);
}

TEST_F(DbTest, CorruptionAfterOpenIsDetectedOnRead) {
  auto db = DB::open(dir_);
  db->put("a", "value-a");
  db->put("b", "value-b");
  auto bytes = read_bytes(seg(1));
  bytes[kHeaderSize + 1 + 2] ^= 0x01;  // inside "value-a"
  write_bytes(seg(1), bytes);

  EXPECT_THROW(db->get("a"), CorruptionError);
  EXPECT_EQ(db->get("b"), "value-b");
}

}  // namespace
}  // namespace kv::test
