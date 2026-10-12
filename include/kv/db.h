#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include "kv/file.h"

namespace kv {

enum class SyncMode {
  // put()/del() return only after their record is fsynced. Concurrent writers
  // share one fdatasync (group commit).
  kEveryWrite,
  // fdatasync once batch_sync_bytes have been appended, on sync(), and on close.
  // A crash can lose the most recent unsynced writes.
  kBatch,
};

struct Options {
  SyncMode sync_mode = SyncMode::kEveryWrite;
  uint64_t batch_sync_bytes = 1 << 20;
  uint64_t max_segment_bytes = 64 << 20;
};

struct Stats {
  size_t live_keys = 0;
  size_t segments = 0;
  uint64_t disk_bytes = 0;
  uint64_t truncated_bytes = 0;  // torn/corrupt tail bytes dropped when opening
};

struct CompactionResult {
  size_t segments_compacted = 0;
  uint64_t bytes_before = 0;  // total size of the sealed segments that were rewritten
  uint64_t bytes_after = 0;   // size of the single segment that replaced them
};

// A log-structured (Bitcask-style) key-value store.
//
// Thread safety: every public method may be called concurrently. Reads run in
// parallel with each other, with the writer, and with compaction.
class DB {
 public:
  static std::unique_ptr<DB> open(const std::filesystem::path& dir, Options opts = {});
  ~DB();
  DB(const DB&) = delete;
  DB& operator=(const DB&) = delete;

  void put(std::string_view key, std::string_view value);
  std::optional<std::string> get(std::string_view key) const;
  // Returns false (and writes nothing) if the key does not exist.
  bool del(std::string_view key);
  void sync();
  CompactionResult compact();
  Stats stats() const;

  static std::string segment_name(uint32_t id);

 private:
  struct Location {
    uint32_t file_id;
    uint64_t offset;
    uint32_t size;  // whole record, header included
    bool operator==(const Location&) const = default;
  };

  DB(std::filesystem::path dir, Options opts);
  void recover();
  void replay_segment(uint32_t id, File& f, bool is_last);
  // Appends a record and updates the index. Returns the end position of the
  // record in the overall log (see appended_), for wait_durable().
  uint64_t append_locked(std::string_view key, std::string_view value, bool tombstone);
  void roll_segment_locked();
  void wait_durable(uint64_t end);
  void check_writable() const;

  const std::filesystem::path dir_;
  const Options opts_;
  std::unique_ptr<DirLock> lock_;  // stops a second process opening the same directory

  // Lock order: compact_mu_ -> write_mu_ -> index_mu_ / sync_mu_.
  std::mutex compact_mu_;               // one compaction at a time
  std::mutex write_mu_;                 // serializes appends to the active segment
  mutable std::shared_mutex index_mu_;  // guards index_ and files_

  std::unordered_map<std::string, Location> index_;
  std::map<uint32_t, std::shared_ptr<File>> files_;

  // Guarded by write_mu_.
  uint32_t active_id_ = 0;
  std::shared_ptr<File> active_;
  uint64_t active_size_ = 0;
  uint64_t appended_ = 0;  // bytes appended since open, across all segments
  uint64_t unsynced_ = 0;  // kBatch only

  // Group commit state, guarded by sync_mu_.
  std::mutex sync_mu_;
  std::condition_variable sync_cv_;
  uint64_t durable_ = 0;  // every byte below this position of appended_ is on disk
  bool syncing_ = false;

  // Set after a failed write or fsync; the log tail is then unknown, so the DB
  // refuses further writes until it is reopened (and recovered).
  std::atomic<bool> failed_{false};
  uint64_t truncated_bytes_ = 0;
};

}  // namespace kv
