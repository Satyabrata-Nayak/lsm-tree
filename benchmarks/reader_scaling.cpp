// Measures shared-reader scaling against one immutable SSTable.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "lsm/lsm.h"

namespace {

using Clock = std::chrono::steady_clock;

std::string key_for(std::size_t index) {
  std::string key = std::to_string(index);
  return std::string(12 - key.size(), '0') + key;
}

std::uint64_t percentile(std::vector<std::uint64_t> samples, double p) {
  if (samples.empty()) {
    return 0;
  }
  std::sort(samples.begin(), samples.end());
  const std::size_t index = static_cast<std::size_t>(
      p * static_cast<double>(samples.size() - 1));
  return samples[index];
}

}  // namespace

int main(int argc, char** argv) {
  const std::filesystem::path directory =
      argc >= 2 ? argv[1] : "build/reader-bench.db";
  const std::size_t reads_per_reader =
      argc >= 3 ? static_cast<std::size_t>(std::stoull(argv[2])) : 10000;
  constexpr std::size_t kKeys = 20000;

  std::filesystem::remove_all(directory);
  lsm::Options options;
  options.memtable_bytes = 4 * 1024 * 1024;
  options.sync_writes = false;

  lsm::LSMTree database(directory, options);
  const std::string value(64, 'x');
  for (std::size_t key = 0; key < kKeys; ++key) {
    database.put(key_for(key), value);
  }
  database.flush();

  std::cout << "readers,total_reads,ops_per_second,p50_us,p95_us,p99_us\n";
  for (const std::size_t reader_count : {1U, 2U, 4U, 8U, 16U}) {
    std::atomic<bool> start{false};
    std::atomic<std::size_t> found{0};
    std::vector<std::vector<std::uint64_t>> latencies(reader_count);
    std::vector<std::thread> readers;
    readers.reserve(reader_count);
    for (std::size_t reader = 0; reader < reader_count; ++reader) {
      readers.emplace_back([&, reader] {
        std::mt19937_64 random(20260728 + reader);
        std::uniform_int_distribution<std::size_t> key(0, kKeys - 1);
        auto& samples = latencies[reader];
        samples.reserve(reads_per_reader);
        while (!start.load(std::memory_order_acquire)) {
        }
        for (std::size_t read = 0; read < reads_per_reader; ++read) {
          const auto before = Clock::now();
          const auto result = database.get(key_for(key(random)));
          const auto after = Clock::now();
          samples.push_back(static_cast<std::uint64_t>((after - before).count()));
          if (result == value) {
            found.fetch_add(1, std::memory_order_relaxed);
          }
        }
      });
    }
    const auto before = Clock::now();
    start.store(true, std::memory_order_release);
    for (auto& reader : readers) {
      reader.join();
    }
    const auto after = Clock::now();

    const std::size_t total_reads = reader_count * reads_per_reader;
    if (found.load(std::memory_order_relaxed) != total_reads) {
      std::cerr << "read verification failed\n";
      return 1;
    }
    std::vector<std::uint64_t> samples;
    samples.reserve(total_reads);
    for (const auto& per_reader : latencies) {
      samples.insert(samples.end(), per_reader.begin(), per_reader.end());
    }
    const double elapsed = std::chrono::duration<double>(after - before).count();
    std::cout << reader_count << ',' << total_reads << ','
              << total_reads / elapsed << ',' << percentile(samples, 0.50) / 1000.0
              << ',' << percentile(samples, 0.95) / 1000.0 << ','
              << percentile(samples, 0.99) / 1000.0 << '\n';
  }
  return 0;
}
