#include "lsm/lsm.h"

#include <cstdio>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <optional>
#include <random>
#include <string>
#include <thread>
#include <unistd.h>

namespace {

int assertions = 0;
int failures = 0;

#define CHECK(condition)                                                     \
  do {                                                                       \
    ++assertions;                                                            \
    if (!(condition)) {                                                      \
      ++failures;                                                            \
      std::cerr << __FILE__ << ":" << __LINE__ << ": CHECK failed: "       \
                << #condition << "\n";                                      \
    }                                                                        \
  } while (false)

std::filesystem::path test_directory(const std::string& name) {
  return std::filesystem::temp_directory_path() /
         ("lsm-tree-" + name + "-" +
          std::to_string(static_cast<long long>(::getpid())));
}

void reset(const std::filesystem::path& directory) {
  std::error_code error;
  std::filesystem::remove_all(directory, error);
}

lsm::Options small_options() {
  lsm::Options options;
  options.memtable_bytes = 90;
  options.compaction_trigger = 4;
  return options;
}

void test_put_get_delete() {
  const auto directory = test_directory("basic");
  reset(directory);
  {
    lsm::LSMTree database(directory);
    database.put("alpha", "one");
    database.put("beta", "two");
    CHECK(database.get("alpha") == std::optional<std::string>("one"));
    CHECK(database.get("beta") == std::optional<std::string>("two"));
    CHECK(!database.get("missing"));
    database.erase("alpha");
    CHECK(!database.get("alpha"));
  }
  reset(directory);
}

void test_wal_recovers_without_flush() {
  const auto directory = test_directory("wal");
  reset(directory);
  {
    lsm::LSMTree database(directory);
    database.put("survives", "restart");
  }
  {
    lsm::LSMTree database(directory);
    CHECK(database.get("survives") ==
          std::optional<std::string>("restart"));
    CHECK(database.stats().memtable_entries == 1);
  }
  reset(directory);
}

void test_flush_and_reopen() {
  const auto directory = test_directory("flush");
  reset(directory);
  {
    lsm::LSMTree database(directory, small_options());
    for (int index = 0; index < 40; ++index) {
      database.put("key-" + std::to_string(index),
                   "value-" + std::to_string(index));
    }
    database.flush();
    CHECK(database.stats().sstable_count >= 1);
  }
  {
    lsm::LSMTree database(directory, small_options());
    for (int index = 0; index < 40; ++index) {
      CHECK(database.get("key-" + std::to_string(index)) ==
            std::optional<std::string>("value-" + std::to_string(index)));
    }
  }
  reset(directory);
}

void test_newest_value_and_tombstone_win() {
  const auto directory = test_directory("versions");
  reset(directory);
  lsm::Options options;
  options.memtable_bytes = 1024;
  options.compaction_trigger = 10;
  {
    lsm::LSMTree database(directory, options);
    database.put("shared", "old");
    database.flush();
    database.put("shared", "new");
    database.flush();
    CHECK(database.get("shared") == std::optional<std::string>("new"));
    database.erase("shared");
    database.flush();
    CHECK(!database.get("shared"));
  }
  {
    lsm::LSMTree database(directory, options);
    CHECK(!database.get("shared"));
    database.compact();
    CHECK(!database.get("shared"));
    CHECK(database.stats().sstable_count == 1);
  }
  reset(directory);
}

void test_torn_wal_tail_is_repaired() {
  const auto directory = test_directory("torn-wal");
  reset(directory);
  {
    lsm::LSMTree database(directory);
    database.put("complete", "record");
  }
  const auto wal = directory / "active.wal";
  {
    std::ofstream output(wal, std::ios::binary | std::ios::app);
    output.write("WAL1broken-tail", 15);
  }
  const auto damaged_size = std::filesystem::file_size(wal);
  {
    lsm::LSMTree database(directory);
    CHECK(database.get("complete") ==
          std::optional<std::string>("record"));
  }
  CHECK(std::filesystem::file_size(wal) < damaged_size);
  reset(directory);
}

void test_bloom_filter_has_no_false_negatives() {
  const auto directory = test_directory("bloom");
  reset(directory);
  lsm::Options options;
  options.memtable_bytes = 4096;
  options.compaction_trigger = 10;
  lsm::LSMTree database(directory, options);
  for (int index = 0; index < 100; ++index) {
    database.put("present-" + std::to_string(index), "value");
  }
  database.flush();
  for (int index = 0; index < 100; ++index) {
    CHECK(database.get("present-" + std::to_string(index)).has_value());
  }
  for (int index = 0; index < 100; ++index) {
    CHECK(!database.get("absent-" + std::to_string(index)).has_value());
  }
  const auto stats = database.stats();
  CHECK(stats.bloom_checks >= 200);
  CHECK(stats.bloom_negative_hits > 0);
  reset(directory);
}

void test_range_scan_reconciles_versions_and_tombstones() {
  const auto directory = test_directory("scan");
  reset(directory);
  lsm::Options options;
  options.memtable_bytes = 4096;
  options.compaction_trigger = 10;
  {
    lsm::LSMTree database(directory, options);
    database.put("a", "old-a");
    database.put("b", "old-b");
    database.put("c", "old-c");
    database.put("d", "old-d");
    database.flush();
    database.put("b", "new-b");
    database.erase("c");
    database.put("e", "new-e");
    const auto result = database.scan("b", "e");
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"b", "new-b"}, {"d", "old-d"}};
    CHECK(result == expected);
    database.flush();
  }
  {
    lsm::LSMTree database(directory, options);
    const auto result = database.scan("", "");
    const std::vector<std::pair<std::string, std::string>> expected = {
        {"a", "old-a"}, {"b", "new-b"}, {"d", "old-d"}, {"e", "new-e"}};
    CHECK(result == expected);
    database.compact();
    CHECK(database.scan("", "") == expected);
  }
  reset(directory);
}

void test_randomized_differential_replay() {
  const auto directory = test_directory("differential");
  reset(directory);
  auto options = small_options();
  options.sync_writes = false;
  std::map<std::string, std::optional<std::string>> oracle;
  std::mt19937 random(20260728);

  for (int phase = 0; phase < 5; ++phase) {
    {
      lsm::LSMTree database(directory, options);
      for (int step = 0; step < 400; ++step) {
        const std::string key = "key-" + std::to_string(random() % 80);
        if (random() % 5 == 0) {
          database.erase(key);
          oracle[key] = std::nullopt;
        } else {
          const std::string value = "value-" + std::to_string(random());
          database.put(key, value);
          oracle[key] = value;
        }
        if (step % 47 == 0) {
          for (const auto& [expected_key, expected_value] : oracle) {
            CHECK(database.get(expected_key) == expected_value);
          }
        }
      }
    }
    lsm::LSMTree reopened(directory, options);
    for (const auto& [key, value] : oracle) {
      CHECK(reopened.get(key) == value);
    }
  }
  reset(directory);
}

void test_corrupt_sstable_is_rejected() {
  const auto directory = test_directory("corrupt-table");
  reset(directory);
  {
    lsm::LSMTree database(directory);
    database.put("key", "value");
    database.flush();
  }
  std::filesystem::path table_path;
  for (const auto& item : std::filesystem::directory_iterator(directory)) {
    if (item.path().extension() == ".dat") {
      table_path = item.path();
    }
  }
  CHECK(!table_path.empty());
  {
    std::fstream file(table_path,
                      std::ios::binary | std::ios::in | std::ios::out);
    file.seekg(-1, std::ios::end);
    char byte = 0;
    file.read(&byte, 1);
    byte ^= 0x5A;
    file.seekp(-1, std::ios::end);
    file.write(&byte, 1);
  }
  bool rejected = false;
  try {
    lsm::LSMTree database(directory);
  } catch (const std::runtime_error&) {
    rejected = true;
  }
  CHECK(rejected);
  reset(directory);
}

void test_sparse_index_across_blocks() {
  const auto directory = test_directory("sparse-index");
  reset(directory);
  lsm::Options options;
  options.memtable_bytes = 4 * 1024 * 1024;
  options.bloom_bits_per_key = 10;
  options.compaction_trigger = 4;
  {
    lsm::LSMTree database(directory, options);
    // 500 zero-padded keys with 64-byte values: ~92 bytes per record, so the
    // 4096-byte block target yields ~44 records per block and ~12 blocks.
    char key[16];
    std::string value(64, 'x');
    for (int i = 0; i < 500; ++i) {
      std::snprintf(key, sizeof(key), "key-%04d", i);
      database.put(key, value);
    }
    database.flush();
    CHECK(database.stats().sstable_count == 1);

    // Reads that cross block boundaries.
    for (int i = 0; i < 500; ++i) {
      std::snprintf(key, sizeof(key), "key-%04d", i);
      CHECK(database.get(key) == value);
    }
    // Misses before, between and after the key range.
    CHECK(database.get("key-0000") == value);
    CHECK(database.get("key-0499") == value);
    CHECK(!database.get("key-0500").has_value());
    CHECK(!database.get("aaa").has_value());
    CHECK(!database.get("zzz").has_value());

    // Range scan spanning several blocks.
    const auto range = database.scan("key-0100", "key-0200");
    CHECK(range.size() == 100);
    CHECK(range.front().first == "key-0100");
    CHECK(range.back().first == "key-0199");

    // Tombstone in a later block hides an earlier value.
    database.erase("key-0250");
    database.flush();
    CHECK(!database.get("key-0250").has_value());
    CHECK(database.get("key-0249") == value);
    CHECK(database.get("key-0251") == value);

    // On-demand block reads were actually exercised.
    CHECK(database.stats().sstable_reads > 0);
    CHECK(database.stats().sstable_bytes_read > 0);
    const auto stats = database.stats();
    CHECK(stats.table_metadata_bytes > 0);
    const auto table_path = directory / "sst-1.dat";
    CHECK(stats.table_metadata_bytes < std::filesystem::file_size(table_path));
  }
  {
    // Reopen: the sparse index is rebuilt from disk and still works.
    lsm::LSMTree reopened(directory, options);
    CHECK(reopened.get("key-0000") == std::string(64, 'x'));
    CHECK(!reopened.get("key-0250").has_value());
    CHECK(reopened.get("key-0499") == std::string(64, 'x'));
    CHECK(reopened.scan("key-0100", "key-0200").size() == 100);
  }
  reset(directory);
}

void test_concurrent_readers_with_writer() {
  const auto directory = test_directory("concurrent-readers");
  reset(directory);
  lsm::Options options;
  options.memtable_bytes = 2048;
  options.compaction_trigger = 4;
  options.sync_writes = false;
  lsm::LSMTree database(directory, options);
  for (int index = 0; index < 200; ++index) {
    const std::string key = "stable-" + std::to_string(index);
    database.put(key, "value-" + std::to_string(index));
  }
  database.flush();

  std::atomic<bool> start{false};
  std::atomic<bool> failed{false};
  std::vector<std::thread> readers;
  for (int reader = 0; reader < 4; ++reader) {
    readers.emplace_back([&database, &failed, &start, reader] {
      while (!start.load(std::memory_order_acquire)) {
      }
      for (int iteration = 0; iteration < 300; ++iteration) {
        const int index = (reader * 37 + iteration) % 200;
        const std::string key = "stable-" + std::to_string(index);
        if (database.get(key) != "value-" + std::to_string(index)) {
          failed.store(true, std::memory_order_release);
        }
        if (iteration % 25 == 0) {
          const auto range = database.scan("stable-", "stable.");
          if (range.size() != 200 || range.front().first != "stable-0" ||
              range.back().first != "stable-99") {
            failed.store(true, std::memory_order_release);
          }
        }
      }
    });
  }
  std::thread writer([&database, &start] {
    while (!start.load(std::memory_order_acquire)) {
    }
    for (int iteration = 0; iteration < 600; ++iteration) {
      database.put("mutable-" + std::to_string(iteration % 50),
                   "value-" + std::to_string(iteration));
    }
    database.flush();
  });

  start.store(true, std::memory_order_release);
  writer.join();
  for (auto& reader : readers) {
    reader.join();
  }
  CHECK(!failed.load(std::memory_order_acquire));
  CHECK(database.get("mutable-49") == std::optional<std::string>("value-599"));
  CHECK(database.stats().sstable_reads > 0);
  reset(directory);
}

}  // namespace

int main() {
  test_put_get_delete();
  test_wal_recovers_without_flush();
  test_flush_and_reopen();
  test_newest_value_and_tombstone_win();
  test_torn_wal_tail_is_repaired();
  test_bloom_filter_has_no_false_negatives();
  test_range_scan_reconciles_versions_and_tombstones();
  test_randomized_differential_replay();
  test_corrupt_sstable_is_rejected();
  test_sparse_index_across_blocks();
  test_concurrent_readers_with_writer();
  if (failures != 0) {
    std::cerr << failures << " of " << assertions << " assertions failed\n";
    return 1;
  }
  std::cout << assertions << " assertions passed\n";
  return 0;
}
