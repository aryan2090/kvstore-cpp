// Throughput and latency benchmark.
//
//   kvbench [--dir D] [--threads N] [--ops N] [--value-size B] [--keys N] [--sync every|batch]
//
// Write phase: each thread puts --ops values into its own slice of the key space.
// Read phase: each thread gets --ops random keys from the whole key space.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "kv/db.h"

namespace {

using Clock = std::chrono::steady_clock;

struct Config {
  std::string dir = "kvbench-data";
  int threads = 4;
  int ops = 100000;  // per thread, per phase
  size_t value_size = 100;
  int keys = 100000;
  kv::SyncMode sync = kv::SyncMode::kBatch;
};

std::string key_of(int i) {
  char buf[32];
  std::snprintf(buf, sizeof(buf), "key%012d", i);
  return buf;
}

template <typename Op>
void run_phase(const char* name, const Config& cfg, Op op) {
  std::vector<std::vector<double>> lat(cfg.threads);
  std::vector<std::thread> workers;
  const auto start = Clock::now();
  for (int t = 0; t < cfg.threads; ++t) {
    workers.emplace_back([&, t] {
      std::mt19937 rng(static_cast<unsigned>(t) + 1);
      lat[t].reserve(cfg.ops);
      for (int i = 0; i < cfg.ops; ++i) {
        const auto t0 = Clock::now();
        op(t, i, rng);
        lat[t].push_back(std::chrono::duration<double, std::micro>(Clock::now() - t0).count());
      }
    });
  }
  for (auto& w : workers) w.join();
  const double secs = std::chrono::duration<double>(Clock::now() - start).count();

  std::vector<double> all;
  for (auto& v : lat) all.insert(all.end(), v.begin(), v.end());
  std::sort(all.begin(), all.end());
  auto pct = [&](double p) { return all[static_cast<size_t>(p * (all.size() - 1))]; };
  std::printf("%-6s %9zu ops  %10.0f ops/sec  p50 %8.1f us  p99 %8.1f us\n", name, all.size(),
              all.size() / secs, pct(0.50), pct(0.99));
}

}  // namespace

int main(int argc, char** argv) {
  Config cfg;
  for (int i = 1; i + 1 < argc; i += 2) {
    const std::string flag = argv[i];
    const char* val = argv[i + 1];
    if (flag == "--dir") cfg.dir = val;
    else if (flag == "--threads") cfg.threads = std::atoi(val);
    else if (flag == "--ops") cfg.ops = std::atoi(val);
    else if (flag == "--value-size") cfg.value_size = std::strtoul(val, nullptr, 10);
    else if (flag == "--keys") cfg.keys = std::atoi(val);
    else if (flag == "--sync") cfg.sync = std::string(val) == "every" ? kv::SyncMode::kEveryWrite : kv::SyncMode::kBatch;
    else {
      std::fprintf(stderr, "unknown flag %s\n", flag.c_str());
      return 2;
    }
  }
  if (cfg.threads < 1 || cfg.ops < 1 || cfg.keys < cfg.threads) {
    std::fprintf(stderr, "invalid arguments\n");
    return 2;
  }

  std::filesystem::remove_all(cfg.dir);
  kv::Options opts;
  opts.sync_mode = cfg.sync;
  auto db = kv::DB::open(cfg.dir, opts);
  const std::string value(cfg.value_size, 'v');
  std::printf("threads=%d ops/thread=%d value=%zuB keys=%d sync=%s\n", cfg.threads, cfg.ops,
              cfg.value_size, cfg.keys, cfg.sync == kv::SyncMode::kEveryWrite ? "every" : "batch");
  std::fflush(stdout);

  const int slice = cfg.keys / cfg.threads;
  run_phase("put", cfg, [&](int t, int i, std::mt19937&) {
    db->put(key_of(t * slice + i % slice), value);
  });
  db->sync();
  const int written = std::min(slice, cfg.ops);  // keys per slice that exist now
  run_phase("get", cfg, [&](int, int, std::mt19937& rng) {
    const int t = static_cast<int>(rng() % static_cast<unsigned>(cfg.threads));
    const int k = t * slice + static_cast<int>(rng() % static_cast<unsigned>(written));
    if (!db->get(key_of(k))) std::abort();
  });

  db.reset();
  std::filesystem::remove_all(cfg.dir);
  return 0;
}
