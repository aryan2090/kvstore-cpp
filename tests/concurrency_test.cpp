#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "test_util.h"

namespace kv::test {
namespace {

// Values encode their own key and a version: "<key>:<version>". Each writer owns a
// disjoint set of keys and writes increasing versions, so every reader can check:
//   - a value always belongs to the key it was read from (no torn/mixed records),
//   - versions seen for one key never go backwards.
std::string make_value(const std::string& key, int version) {
  return key + ":" + std::to_string(version) + ":" + std::string(static_cast<size_t>(version % 50), 'p');
}

int parse_version(const std::string& key, const std::string& value) {
  if (value.compare(0, key.size() + 1, key + ":") != 0) return -1;
  return std::stoi(value.substr(key.size() + 1));
}

void stress(const std::filesystem::path& dir, SyncMode mode, bool with_compaction) {
  constexpr int kWriters = 4;
  constexpr int kReaders = 4;
  constexpr int kKeysPerWriter = 20;
  // Must be even (see check_final). Fewer versions with a real fsync per write.
  const int kVersions = mode == SyncMode::kEveryWrite ? 40 : 150;

  Options opts;
  opts.sync_mode = mode;
  opts.max_segment_bytes = 16 * 1024;  // force many rollovers
  auto db = DB::open(dir, opts);

  auto key_of = [](int w, int k) { return "w" + std::to_string(w) + "-k" + std::to_string(k); };
  std::atomic<int> writers_done{0};
  std::atomic<int> errors{0};
  std::vector<std::thread> threads;

  for (int w = 0; w < kWriters; ++w) {
    threads.emplace_back([&, w] {
      for (int v = 1; v <= kVersions; ++v) {
        for (int k = 0; k < kKeysPerWriter; ++k) {
          // Key 0 of each writer is deleted on odd versions to exercise tombstones.
          if (k == 0 && v % 2 == 1 && v > 1) {
            if (!db->del(key_of(w, k))) errors++;
          } else {
            db->put(key_of(w, k), make_value(key_of(w, k), v));
          }
        }
      }
      writers_done++;
    });
  }
  for (int r = 0; r < kReaders; ++r) {
    threads.emplace_back([&, r] {
      std::vector<int> last_seen(kWriters * kKeysPerWriter, 0);
      unsigned i = static_cast<unsigned>(r);
      while (writers_done.load() < kWriters) {
        i = i * 1103515245u + 12345u;
        const int w = static_cast<int>((i >> 8) % kWriters);
        const int k = static_cast<int>((i >> 16) % kKeysPerWriter);
        auto v = db->get(key_of(w, k));
        if (!v) continue;
        const int version = parse_version(key_of(w, k), *v);
        int& last = last_seen[w * kKeysPerWriter + k];
        if (version < 0 || version < last) errors++;
        last = version;
      }
    });
  }
  std::thread compactor;
  if (with_compaction) {
    compactor = std::thread([&] {
      while (writers_done.load() < kWriters) {
        db->compact();
        std::this_thread::yield();
      }
    });
  }
  for (auto& t : threads) t.join();
  if (compactor.joinable()) compactor.join();
  EXPECT_EQ(errors.load(), 0);

  auto check_final = [&](DB& d) {
    for (int w = 0; w < kWriters; ++w) {
      for (int k = 0; k < kKeysPerWriter; ++k) {
        // kVersions is even, so key 0 was re-put after its last delete.
        EXPECT_EQ(d.get(key_of(w, k)), make_value(key_of(w, k), kVersions));
      }
    }
    EXPECT_EQ(d.stats().live_keys, static_cast<size_t>(kWriters * kKeysPerWriter));
  };
  check_final(*db);
  db.reset();
  auto reopened = DB::open(dir, opts);
  check_final(*reopened);
}

TEST_F(DbTest, ConcurrentReadersAndWritersBatchSync) { stress(dir_, SyncMode::kBatch, false); }

TEST_F(DbTest, ConcurrentReadersAndWritersGroupCommit) {
  stress(dir_, SyncMode::kEveryWrite, false);
}

TEST_F(DbTest, ConcurrentReadersWritersAndCompaction) { stress(dir_, SyncMode::kBatch, true); }

}  // namespace
}  // namespace kv::test
