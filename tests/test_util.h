#pragma once

#include <gtest/gtest.h>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "kv/db.h"

namespace kv::test {

// A fresh, empty directory per test, removed afterwards.
class DbTest : public ::testing::Test {
 protected:
  void SetUp() override {
    const auto* info = ::testing::UnitTest::GetInstance()->current_test_info();
    dir_ = std::filesystem::temp_directory_path() /
           ("kvtest-" + std::to_string(::getpid()) + "-" + info->test_suite_name() + "-" +
            info->name());
    std::filesystem::remove_all(dir_);
  }
  void TearDown() override { std::filesystem::remove_all(dir_); }

  std::filesystem::path seg(uint32_t id) const { return dir_ / DB::segment_name(id); }

  std::filesystem::path dir_;
};

inline std::vector<char> read_bytes(const std::filesystem::path& p) {
  std::ifstream in(p, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

inline void write_bytes(const std::filesystem::path& p, const std::vector<char>& data) {
  std::ofstream out(p, std::ios::binary | std::ios::trunc);
  out.write(data.data(), static_cast<std::streamsize>(data.size()));
}

}  // namespace kv::test
