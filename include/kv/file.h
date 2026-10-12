#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>

namespace kv {

class IoError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

class CorruptionError : public std::runtime_error {
 public:
  using std::runtime_error::runtime_error;
};

// Thin RAII wrapper around a POSIX file descriptor. All I/O is positional
// (pread/pwrite), so concurrent readers never share or move a file offset.
class File {
 public:
  // create=true fails if the file already exists.
  static std::shared_ptr<File> open(const std::filesystem::path& path, bool create);
  ~File();
  File(const File&) = delete;
  File& operator=(const File&) = delete;

  void write_at(uint64_t offset, const char* data, size_t n);
  // Reads exactly n bytes. Returns false if the file ends first.
  bool read_at(uint64_t offset, char* data, size_t n) const;
  void sync();
  void truncate(uint64_t size);
  uint64_t size() const;

 private:
  explicit File(int fd) : fd_(fd) {}
  int fd_;
};

// Holds an exclusive flock() on `path` for its lifetime.
class DirLock {
 public:
  explicit DirLock(const std::filesystem::path& path);
  ~DirLock();
  DirLock(const DirLock&) = delete;
  DirLock& operator=(const DirLock&) = delete;

 private:
  int fd_;
};

// fsync a directory so that file creations, renames and deletions in it are durable.
void sync_dir(const std::filesystem::path& dir);

[[noreturn]] void throw_errno(const std::string& what);

}  // namespace kv
