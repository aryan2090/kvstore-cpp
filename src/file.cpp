#include "kv/file.h"

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>

namespace kv {

void throw_errno(const std::string& what) {
  throw IoError(what + ": " + std::strerror(errno));
}

std::shared_ptr<File> File::open(const std::filesystem::path& path, bool create) {
  const int flags = O_RDWR | O_CLOEXEC | (create ? O_CREAT | O_EXCL : 0);
  const int fd = ::open(path.c_str(), flags, 0644);
  if (fd < 0) throw_errno("open " + path.string());
  return std::shared_ptr<File>(new File(fd));
}

File::~File() { ::close(fd_); }

void File::write_at(uint64_t offset, const char* data, size_t n) {
  while (n > 0) {
    const ssize_t w = ::pwrite(fd_, data, n, static_cast<off_t>(offset));
    if (w < 0) {
      if (errno == EINTR) continue;
      throw_errno("pwrite");
    }
    data += w;
    offset += static_cast<uint64_t>(w);
    n -= static_cast<size_t>(w);
  }
}

bool File::read_at(uint64_t offset, char* data, size_t n) const {
  while (n > 0) {
    const ssize_t r = ::pread(fd_, data, n, static_cast<off_t>(offset));
    if (r < 0) {
      if (errno == EINTR) continue;
      throw_errno("pread");
    }
    if (r == 0) return false;
    data += r;
    offset += static_cast<uint64_t>(r);
    n -= static_cast<size_t>(r);
  }
  return true;
}

// fdatasync is enough: it still flushes the file size, which appends change.
void File::sync() {
  if (::fdatasync(fd_) != 0) throw_errno("fdatasync");
}

void File::truncate(uint64_t size) {
  if (::ftruncate(fd_, static_cast<off_t>(size)) != 0) throw_errno("ftruncate");
}

uint64_t File::size() const {
  struct stat st {};
  if (::fstat(fd_, &st) != 0) throw_errno("fstat");
  return static_cast<uint64_t>(st.st_size);
}

DirLock::DirLock(const std::filesystem::path& path) {
  fd_ = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
  if (fd_ < 0) throw_errno("open " + path.string());
  if (::flock(fd_, LOCK_EX | LOCK_NB) != 0) {
    ::close(fd_);
    throw IoError("database directory is already open in another process: " +
                  path.parent_path().string());
  }
}

DirLock::~DirLock() { ::close(fd_); }

void sync_dir(const std::filesystem::path& dir) {
  const int fd = ::open(dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) throw_errno("open dir " + dir.string());
  const int rc = ::fsync(fd);
  ::close(fd);
  if (rc != 0) throw_errno("fsync dir " + dir.string());
}

}  // namespace kv
