#include "kv/db.h"

#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <tuple>
#include <vector>

#include "kv/record.h"

namespace fs = std::filesystem;

namespace kv {
namespace {

// File names: "0000000007.data" is segment 7. "0000000007.compact" is a finished
// compaction that replaces every segment with id <= 7. "*.tmp" is scratch.
struct DirEntry {
  uint32_t id;
  std::string suffix;
};

std::optional<DirEntry> parse_name(const std::string& name) {
  if (name.size() < 11 || name[10] != '.') return std::nullopt;
  for (int i = 0; i < 10; ++i) {
    if (name[i] < '0' || name[i] > '9') return std::nullopt;
  }
  return DirEntry{static_cast<uint32_t>(std::stoul(name.substr(0, 10))), name.substr(11)};
}

}  // namespace

std::string DB::segment_name(uint32_t id) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "%010u.data", id);
  return buf;
}

DB::DB(fs::path dir, Options opts) : dir_(std::move(dir)), opts_(opts) {}

std::unique_ptr<DB> DB::open(const fs::path& dir, Options opts) {
  fs::create_directories(dir);
  std::unique_ptr<DB> db(new DB(dir, opts));
  db->lock_ = std::make_unique<DirLock>(dir / "LOCK");
  db->recover();
  return db;
}

DB::~DB() {
  try {
    std::lock_guard w(write_mu_);
    if (!failed_ && active_) active_->sync();
  } catch (...) {
    // Destructors must not throw; unsynced batch-mode writes may be lost.
  }
}

void DB::recover() {
  std::vector<uint32_t> segments;
  std::vector<uint32_t> compacted;
  for (const auto& e : fs::directory_iterator(dir_)) {
    const std::string name = e.path().filename().string();
    auto parsed = parse_name(name);
    if (name.ends_with(".tmp")) {
      fs::remove(e.path());  // an unfinished compaction; the old segments are intact
    } else if (parsed && parsed->suffix == "data") {
      segments.push_back(parsed->id);
    } else if (parsed && parsed->suffix == "compact") {
      compacted.push_back(parsed->id);
    }
  }

  // A compaction crashed after its commit point (the rename to .compact) but
  // before cleanup finished. Roll it forward.
  if (!compacted.empty()) {
    const uint32_t b = *std::max_element(compacted.begin(), compacted.end());
    std::erase_if(segments, [&](uint32_t id) {
      if (id > b) return false;
      fs::remove(dir_ / segment_name(id));
      return true;
    });
    for (uint32_t id : compacted) {
      const fs::path src = dir_ / (segment_name(id).substr(0, 10) + ".compact");
      if (id == b) {
        fs::rename(src, dir_ / segment_name(id));
      } else {
        fs::remove(src);
      }
    }
    segments.push_back(b);
    sync_dir(dir_);
  }

  std::sort(segments.begin(), segments.end());
  for (size_t i = 0; i < segments.size(); ++i) {
    auto f = File::open(dir_ / segment_name(segments[i]), false);
    replay_segment(segments[i], *f, i + 1 == segments.size());
    files_[segments[i]] = std::move(f);
  }

  if (files_.empty()) {
    active_id_ = 1;
    active_ = File::open(dir_ / segment_name(active_id_), true);
    sync_dir(dir_);
    files_[active_id_] = active_;
  } else {
    active_id_ = files_.rbegin()->first;
    active_ = files_.rbegin()->second;
  }
  active_size_ = active_->size();
}

// Rebuilds the index from one segment. A bad record in the newest segment is
// treated as a torn write from a crash: everything from it onwards is cut off.
// A bad record in an older segment cannot be a torn write (segments are fsynced
// before they are sealed), so it is reported as corruption.
void DB::replay_segment(uint32_t id, File& f, bool is_last) {
  const uint64_t size = f.size();
  std::string buf(size, '\0');
  if (size > 0 && !f.read_at(0, buf.data(), size)) {
    throw IoError("short read replaying " + segment_name(id));
  }

  uint64_t off = 0;
  while (off < size) {
    RecordHeader h;
    const bool ok = size - off >= kHeaderSize && decode_header(buf.data() + off, h) &&
                    h.record_size() <= size - off && verify_record(h, buf.data() + off);
    if (!ok) break;
    std::string key(buf.data() + off + kHeaderSize, h.key_len);
    if (h.tombstone()) {
      index_.erase(key);
    } else {
      index_[std::move(key)] = Location{id, off, static_cast<uint32_t>(h.record_size())};
    }
    off += h.record_size();
  }

  if (off < size) {
    if (!is_last) {
      throw CorruptionError("corrupt record in " + segment_name(id) + " at offset " +
                            std::to_string(off));
    }
    f.truncate(off);
    f.sync();
    truncated_bytes_ += size - off;
  }
}

void DB::check_writable() const {
  if (failed_) throw IoError("database is in a failed state after an I/O error; reopen it");
}

void DB::roll_segment_locked() {
  active_->sync();  // a sealed segment is always fully durable
  const uint32_t id = active_id_ + 1;
  auto f = File::open(dir_ / segment_name(id), true);
  sync_dir(dir_);
  {
    std::unique_lock lk(index_mu_);
    files_[id] = f;
  }
  active_id_ = id;
  active_ = std::move(f);
  active_size_ = 0;
  unsynced_ = 0;
}

uint64_t DB::append_locked(std::string_view key, std::string_view value, bool tombstone) {
  check_writable();
  std::string rec;
  encode_record(rec, key, value, tombstone);
  try {
    if (active_size_ > 0 && active_size_ + rec.size() > opts_.max_segment_bytes) {
      roll_segment_locked();
    }
    active_->write_at(active_size_, rec.data(), rec.size());
    if (opts_.sync_mode == SyncMode::kBatch) {
      unsynced_ += rec.size();
      if (unsynced_ >= opts_.batch_sync_bytes) {
        active_->sync();
        unsynced_ = 0;
      }
    }
  } catch (...) {
    failed_ = true;
    throw;
  }

  {
    std::unique_lock lk(index_mu_);
    if (tombstone) {
      index_.erase(std::string(key));
    } else {
      index_.insert_or_assign(std::string(key),
                              Location{active_id_, active_size_, static_cast<uint32_t>(rec.size())});
    }
  }
  active_size_ += rec.size();
  appended_ += rec.size();
  return appended_;
}

// Group commit: the first waiter becomes the leader and runs one fdatasync that
// covers every record appended so far; the others just wait for it.
void DB::wait_durable(uint64_t end) {
  std::unique_lock lk(sync_mu_);
  while (durable_ < end) {
    check_writable();
    if (syncing_) {
      sync_cv_.wait(lk);
      continue;
    }
    syncing_ = true;
    lk.unlock();

    uint64_t target;
    std::shared_ptr<File> f;
    {
      std::lock_guard w(write_mu_);
      target = appended_;
      f = active_;  // older segments were synced when they were sealed
    }
    bool ok = true;
    try {
      f->sync();
    } catch (...) {
      ok = false;
    }

    lk.lock();
    syncing_ = false;
    if (ok) {
      durable_ = std::max(durable_, target);
    } else {
      failed_ = true;
    }
    sync_cv_.notify_all();
  }
}

void DB::put(std::string_view key, std::string_view value) {
  if (key.size() > kMaxKeySize) throw std::invalid_argument("key too large");
  if (value.size() > kMaxValueSize) throw std::invalid_argument("value too large");
  uint64_t end;
  {
    std::lock_guard w(write_mu_);
    end = append_locked(key, value, false);
  }
  if (opts_.sync_mode == SyncMode::kEveryWrite) wait_durable(end);
}

bool DB::del(std::string_view key) {
  if (key.size() > kMaxKeySize) return false;
  uint64_t end;
  {
    std::lock_guard w(write_mu_);
    {
      std::shared_lock lk(index_mu_);
      if (!index_.contains(std::string(key))) return false;
    }
    end = append_locked(key, {}, true);
  }
  if (opts_.sync_mode == SyncMode::kEveryWrite) wait_durable(end);
  return true;
}

std::optional<std::string> DB::get(std::string_view key) const {
  Location loc;
  std::shared_ptr<File> f;
  {
    std::shared_lock lk(index_mu_);
    auto it = index_.find(std::string(key));
    if (it == index_.end()) return std::nullopt;
    loc = it->second;
    f = files_.at(loc.file_id);
  }
  // The lock is released before the disk read. Holding the shared_ptr keeps the
  // fd open even if compaction unlinks this segment in the meantime.
  std::string rec(loc.size, '\0');
  RecordHeader h;
  if (!f->read_at(loc.offset, rec.data(), rec.size()) || !decode_header(rec.data(), h) ||
      h.record_size() != loc.size || !verify_record(h, rec.data()) ||
      std::string_view(rec).substr(kHeaderSize, h.key_len) != key) {
    throw CorruptionError("bad record for key in " + segment_name(loc.file_id) +
                          " at offset " + std::to_string(loc.offset));
  }
  return rec.substr(kHeaderSize + h.key_len);
}

void DB::sync() {
  std::lock_guard w(write_mu_);
  check_writable();
  active_->sync();
  unsynced_ = 0;
}

// Rewrites the live records of every sealed segment into one new segment that
// takes the id of the newest sealed segment. Crash safety comes from file names:
//   1. write NNN.compact.tmp and fsync it   (crash: tmp is deleted on open)
//   2. rename to NNN.compact, fsync dir     (commit point)
//   3. delete old segments <= NNN, rename NNN.compact -> NNN.data, fsync dir
//      (crash: open finishes step 3 itself)
// Writers keep appending to the active segment, and readers keep reading, while
// the new segment is being written.
CompactionResult DB::compact() {
  std::lock_guard c(compact_mu_);
  uint32_t active_id;
  {
    std::lock_guard w(write_mu_);
    check_writable();
    if (active_size_ > 0) {
      try {
        roll_segment_locked();
      } catch (...) {
        failed_ = true;
        throw;
      }
    }
    active_id = active_id_;
  }

  std::map<uint32_t, std::shared_ptr<File>> sealed;
  std::vector<std::pair<std::string, Location>> live;
  {
    std::shared_lock lk(index_mu_);
    for (const auto& [id, f] : files_) {
      if (id < active_id) sealed[id] = f;
    }
    for (const auto& [key, loc] : index_) {
      if (loc.file_id < active_id) live.emplace_back(key, loc);
    }
  }
  CompactionResult result;
  if (sealed.empty()) return result;
  result.segments_compacted = sealed.size();
  for (const auto& [id, f] : sealed) result.bytes_before += f->size();

  // Copying in file order turns the reads into a mostly sequential scan.
  std::sort(live.begin(), live.end(), [](const auto& a, const auto& b) {
    return std::tie(a.second.file_id, a.second.offset) <
           std::tie(b.second.file_id, b.second.offset);
  });

  const uint32_t out_id = sealed.rbegin()->first;
  const std::string stem = segment_name(out_id).substr(0, 10);
  const fs::path tmp_path = dir_ / (stem + ".compact.tmp");
  const fs::path commit_path = dir_ / (stem + ".compact");

  std::shared_ptr<File> out;
  std::vector<Location> moved(live.size());
  try {
    out = File::open(tmp_path, true);
    std::string buf;
    uint64_t out_size = 0;
    std::string rec;
    for (size_t i = 0; i < live.size(); ++i) {
      const Location& loc = live[i].second;
      rec.resize(loc.size);
      RecordHeader h;
      if (!sealed.at(loc.file_id)->read_at(loc.offset, rec.data(), rec.size()) ||
          !decode_header(rec.data(), h) || !verify_record(h, rec.data())) {
        throw CorruptionError("bad record during compaction in " + segment_name(loc.file_id));
      }
      moved[i] = Location{out_id, out_size + buf.size(), loc.size};
      buf += rec;
      if (buf.size() >= (1 << 20)) {
        out->write_at(out_size, buf.data(), buf.size());
        out_size += buf.size();
        buf.clear();
      }
    }
    out->write_at(out_size, buf.data(), buf.size());
    out->sync();
    fs::rename(tmp_path, commit_path);
    sync_dir(dir_);
  } catch (...) {
    std::error_code ec;
    fs::remove(tmp_path, ec);
    throw;
  }

  {
    std::unique_lock lk(index_mu_);
    for (size_t i = 0; i < live.size(); ++i) {
      auto it = index_.find(live[i].first);
      // Skip keys that were overwritten or deleted while we were copying.
      if (it != index_.end() && it->second == live[i].second) it->second = moved[i];
    }
    for (const auto& [id, f] : sealed) files_.erase(id);
    files_[out_id] = out;
  }

  for (const auto& [id, f] : sealed) fs::remove(dir_ / segment_name(id));
  fs::rename(commit_path, dir_ / segment_name(out_id));
  sync_dir(dir_);
  result.bytes_after = out->size();
  return result;
}

Stats DB::stats() const {
  std::shared_lock lk(index_mu_);
  Stats s;
  s.live_keys = index_.size();
  s.segments = files_.size();
  for (const auto& [id, f] : files_) s.disk_bytes += f->size();
  s.truncated_bytes = truncated_bytes_;
  return s;
}

}  // namespace kv
