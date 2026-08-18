// benchmarks/benchmark_driver.cpp
//
// Configurable benchmark driver for MiniLSM.
//
// Usage:
//   lsm_bench <workload.conf> <db_dir> [--key value ...]
//
// The workload file is a plain "key = value" file (see benchmarks/workloads/).
// Every knob can also be overridden on the command line:
//   --memtable <bytes>|K|M  memtable size (e.g. 262144, 256K, 1M)
//   --bloom <bits>          bloom filter bits per key
//   --compaction <n>        flush/compaction trigger
//   --sync <0|1>            synchronous WAL fsync on every write
//   --ops <n>               number of measured operations
//   --preload <n>           writes performed before the measured phase
//   --seed <n>              RNG seed (fixed => reproducible runs)
//   --output <path>         JSON report destination
//
// A run prints a one-line summary to stdout and writes a full JSON report
// with throughput, p50/p95/p99 latencies, SSTable count, Bloom statistics,
// and on-disk database size.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <random>
#include <string>
#include <vector>

#include "lsm/lsm.h"

namespace {

namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

struct Config {
  std::string workload = "workload";
  std::size_t ops = 20000;
  double write_ratio = 0.5;
  std::size_t key_space = 100000;
  std::size_t value_size = 100;
  std::size_t preload = 0;
  std::uint64_t seed = 20260728;
};

struct Override {
  std::optional<std::size_t> memtable_bytes;
  std::optional<std::size_t> bloom_bits;
  std::optional<std::size_t> compaction_trigger;
  std::optional<bool> sync_writes;
  std::optional<std::size_t> ops;
  std::optional<std::size_t> preload;
  std::optional<std::uint64_t> seed;
  std::optional<std::string> output;
};

std::string trim(const std::string& s) {
  const std::size_t b = s.find_first_not_of(" \t\r\n");
  if (b == std::string::npos) return "";
  const std::size_t e = s.find_last_not_of(" \t\r\n");
  return s.substr(b, e - b + 1);
}

// Parses "262144", "256K" or "1M" into a byte count.
std::size_t parse_size(const std::string& raw) {
  std::string s = trim(raw);
  std::uint64_t multiplier = 1;
  if (!s.empty() && (s.back() == 'K' || s.back() == 'k')) {
    multiplier = 1024;
    s.pop_back();
  } else if (!s.empty() && (s.back() == 'M' || s.back() == 'm')) {
    multiplier = 1024 * 1024;
    s.pop_back();
  }
  return static_cast<std::size_t>(
      static_cast<std::uint64_t>(std::strtoull(s.c_str(), nullptr, 10)) *
      multiplier);
}

Config read_config(const std::string& path) {
  Config config;
  std::ifstream in(path);
  if (!in) {
    std::cerr << "error: cannot open workload file: " << path << "\n";
    std::exit(2);
  }
  std::string line;
  std::size_t line_no = 0;
  while (std::getline(in, line)) {
    ++line_no;
    line = trim(line);
    if (line.empty() || line[0] == '#') continue;
    const std::size_t eq = line.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = trim(line.substr(0, eq));
    const std::string value = trim(line.substr(eq + 1));
    try {
      if (key == "workload") {
        config.workload = value;
      } else if (key == "ops") {
        config.ops = static_cast<std::size_t>(std::stoull(value));
      } else if (key == "write_ratio") {
        config.write_ratio = std::stod(value);
      } else if (key == "key_space") {
        config.key_space = static_cast<std::size_t>(std::stoull(value));
      } else if (key == "value_size") {
        config.value_size = static_cast<std::size_t>(std::stoull(value));
      } else if (key == "preload") {
        config.preload = static_cast<std::size_t>(std::stoull(value));
      } else if (key == "seed") {
        config.seed = std::stoull(value);
      }
    } catch (const std::exception& e) {
      std::cerr << "error: " << path << ":" << line_no << ": bad value '"
                << value << "' for '" << key << "': " << e.what() << "\n";
      std::exit(2);
    }
  }
  if (config.write_ratio < 0.0) config.write_ratio = 0.0;
  if (config.write_ratio > 1.0) config.write_ratio = 1.0;
  if (config.key_space == 0) config.key_space = 1;
  return config;
}

// Fixed-width decimal key so lexicographic order matches numeric order.
std::string make_key(std::uint64_t id) {
  std::string digits = std::to_string(id);
  if (digits.size() >= 20) return digits;
  return std::string(20 - digits.size(), '0') + digits;
}

// Nearest-rank percentile over ns samples; 0 for an empty sample set.
std::uint64_t percentile(std::vector<std::uint64_t>& samples, double p) {
  if (samples.empty()) return 0;
  std::sort(samples.begin(), samples.end());
  const std::size_t idx =
      static_cast<std::size_t>(std::ceil(p / 100.0 * samples.size())) - 1;
  return samples[idx];
}

std::string json_string(const std::string& s) {
  std::string out = "\"";
  for (const char ch : s) {
    if (ch == '"' || ch == '\\') {
      out.push_back('\\');
      out.push_back(ch);
    } else if (ch == '\n') {
      out += "\\n";
    } else {
      out.push_back(ch);
    }
  }
  out.push_back('"');
  return out;
}

void print_usage() {
  std::cerr << "usage: lsm_bench <workload.conf> <db_dir> [--key value ...]\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3) {
    print_usage();
    return 2;
  }

  const std::string config_path = argv[1];
  const fs::path db_dir = argv[2];
  Config config = read_config(config_path);
  Override over;

  for (int i = 3; i + 1 < argc; i += 2) {
    const std::string key = argv[i];
    const std::string value = argv[i + 1];
    try {
      if (key == "--memtable") {
        over.memtable_bytes = parse_size(value);
      } else if (key == "--bloom") {
        over.bloom_bits = static_cast<std::size_t>(std::stoull(value));
      } else if (key == "--compaction") {
        over.compaction_trigger = static_cast<std::size_t>(std::stoull(value));
      } else if (key == "--sync") {
        over.sync_writes = (value != "0");
      } else if (key == "--ops") {
        over.ops = static_cast<std::size_t>(std::stoull(value));
      } else if (key == "--preload") {
        over.preload = static_cast<std::size_t>(std::stoull(value));
      } else if (key == "--seed") {
        over.seed = std::stoull(value);
      } else if (key == "--output") {
        over.output = value;
      } else {
        std::cerr << "error: unknown option: " << key << "\n";
        print_usage();
        return 2;
      }
    } catch (const std::exception& e) {
      std::cerr << "error: bad value '" << value << "' for " << key << ": "
                << e.what() << "\n";
      return 2;
    }
  }

  if (over.ops) config.ops = *over.ops;
  if (over.preload) config.preload = *over.preload;
  if (over.seed) config.seed = *over.seed;

  lsm::Options options;
  if (over.memtable_bytes) options.memtable_bytes = *over.memtable_bytes;
  if (over.bloom_bits) options.bloom_bits_per_key = *over.bloom_bits;
  if (over.compaction_trigger) {
    options.compaction_trigger = *over.compaction_trigger;
  }
  if (over.sync_writes) options.sync_writes = *over.sync_writes;

  if (fs::exists(db_dir)) fs::remove_all(db_dir);
  fs::create_directories(db_dir);

  lsm::LSMTree db(db_dir, options);

  std::mt19937_64 rng(config.seed);
  std::uniform_int_distribution<std::uint64_t> key_dist(
      0, static_cast<std::uint64_t>(config.key_space - 1));
  std::uniform_real_distribution<double> op_dist(0.0, 1.0);

  const std::string value(config.value_size, 'x');

  // Preload phase: populate the database without measuring it.
  for (std::size_t i = 0; i < config.preload; ++i) {
    db.put(make_key(key_dist(rng)), value);
  }

  std::vector<std::uint64_t> write_lat_ns;
  std::vector<std::uint64_t> read_lat_ns;
  write_lat_ns.reserve(config.ops);
  read_lat_ns.reserve(config.ops);

  std::size_t writes = 0;
  std::size_t reads = 0;
  std::size_t found = 0;

  const auto start = Clock::now();
  for (std::size_t i = 0; i < config.ops; ++i) {
    if (op_dist(rng) < config.write_ratio) {
      const auto t0 = Clock::now();
      db.put(make_key(key_dist(rng)), value);
      const auto t1 = Clock::now();
      write_lat_ns.push_back(
          static_cast<std::uint64_t>((t1 - t0).count()));
      ++writes;
    } else {
      const auto t0 = Clock::now();
      const auto result = db.get(make_key(key_dist(rng)));
      const auto t1 = Clock::now();
      read_lat_ns.push_back(
          static_cast<std::uint64_t>((t1 - t0).count()));
      ++reads;
      if (result.has_value()) ++found;
    }
  }
  const auto end = Clock::now();
  const double elapsed_s =
      std::chrono::duration<double>(end - start).count();

  const lsm::Stats stats = db.stats();

  std::uint64_t db_size = 0;
  std::size_t file_count = 0;
  for (const auto& entry : fs::directory_iterator(db_dir)) {
    if (entry.is_regular_file()) {
      db_size += entry.file_size();
      ++file_count;
    }
  }

  const double write_p50 = percentile(write_lat_ns, 50.0) / 1000.0;
  const double write_p95 = percentile(write_lat_ns, 95.0) / 1000.0;
  const double write_p99 = percentile(write_lat_ns, 99.0) / 1000.0;
  const double read_p50 = percentile(read_lat_ns, 50.0) / 1000.0;
  const double read_p95 = percentile(read_lat_ns, 95.0) / 1000.0;
  const double read_p99 = percentile(read_lat_ns, 99.0) / 1000.0;

  const double read_hit_rate =
      reads == 0 ? 0.0 : static_cast<double>(found) / static_cast<double>(reads);

  std::cout << "workload=" << config.workload
            << " memtable=" << options.memtable_bytes
            << " bloom=" << options.bloom_bits_per_key
            << " compaction=" << options.compaction_trigger
            << " sync=" << (options.sync_writes ? 1 : 0)
            << " ops=" << config.ops << " preload=" << config.preload
            << " elapsed_s=" << elapsed_s
            << " total_ops_per_second=" << (config.ops / elapsed_s)
            << " writes=" << writes
            << " write_ops_per_second=" << (writes / elapsed_s)
            << " write_p99_us=" << write_p99
            << " reads=" << reads
            << " read_ops_per_second=" << (reads / elapsed_s)
            << " read_hit_rate=" << read_hit_rate
            << " read_p99_us=" << read_p99
            << " sstables=" << stats.sstable_count
            << " bloom_checks=" << stats.bloom_checks
            << " bloom_negative_hits=" << stats.bloom_negative_hits
            << " db_size_bytes=" << db_size << " files=" << file_count << "\n";

  std::string json;
  json += "{\n";
  json += "  \"workload\": " + json_string(config.workload) + ",\n";
  json += "  \"config\": {\n";
  json += "    \"ops\": " + std::to_string(config.ops) + ",\n";
  json += "    \"write_ratio\": " + std::to_string(config.write_ratio) + ",\n";
  json += "    \"key_space\": " + std::to_string(config.key_space) + ",\n";
  json += "    \"value_size\": " + std::to_string(config.value_size) + ",\n";
  json += "    \"preload\": " + std::to_string(config.preload) + ",\n";
  json += "    \"seed\": " + std::to_string(config.seed) + "\n";
  json += "  },\n";
  json += "  \"options\": {\n";
  json += "    \"memtable_bytes\": " + std::to_string(options.memtable_bytes) +
          ",\n";
  json += "    \"bloom_bits_per_key\": " +
          std::to_string(options.bloom_bits_per_key) + ",\n";
  json += "    \"compaction_trigger\": " +
          std::to_string(options.compaction_trigger) + ",\n";
  json += std::string("    \"sync_writes\": ") +
          (options.sync_writes ? "true" : "false") + "\n";
  json += "  },\n";
  json += "  \"results\": {\n";
  json += "    \"elapsed_seconds\": " + std::to_string(elapsed_s) + ",\n";
  json += "    \"total_ops_per_second\": " +
          std::to_string(config.ops / elapsed_s) + ",\n";
  json += "    \"writes\": " + std::to_string(writes) + ",\n";
  json += "    \"write_ops_per_second\": " + std::to_string(writes / elapsed_s) +
          ",\n";
  json += "    \"reads\": " + std::to_string(reads) + ",\n";
  json += "    \"read_ops_per_second\": " + std::to_string(reads / elapsed_s) +
          ",\n";
  json += "    \"read_hit_rate\": " + std::to_string(read_hit_rate) + ",\n";
  json += "    \"write_latency_us\": { \"p50\": " + std::to_string(write_p50) +
          ", \"p95\": " + std::to_string(write_p95) +
          ", \"p99\": " + std::to_string(write_p99) + " },\n";
  json += "    \"read_latency_us\": { \"p50\": " + std::to_string(read_p50) +
          ", \"p95\": " + std::to_string(read_p95) +
          ", \"p99\": " + std::to_string(read_p99) + " },\n";
  json += "    \"sstable_count\": " + std::to_string(stats.sstable_count) +
          ",\n";
  json += "    \"bloom_checks\": " + std::to_string(stats.bloom_checks) + ",\n";
  json += "    \"bloom_negative_hits\": " +
          std::to_string(stats.bloom_negative_hits) + ",\n";
  json += "    \"database_size_bytes\": " + std::to_string(db_size) + ",\n";
  json += "    \"file_count\": " + std::to_string(file_count) + "\n";
  json += "  }\n";
  json += "}\n";

  const fs::path out_path =
      over.output
          ? fs::path(*over.output)
          : fs::path("benchmarks/results") /
                (config.workload + "__memtable_" +
                 std::to_string(options.memtable_bytes) + "__bloom_" +
                 std::to_string(options.bloom_bits_per_key) + "__compaction_" +
                 std::to_string(options.compaction_trigger) + ".json");
  fs::create_directories(out_path.parent_path());
  std::ofstream out(out_path);
  out << json;
  std::cout << "report written to " << out_path.string() << "\n";

  return 0;
}
