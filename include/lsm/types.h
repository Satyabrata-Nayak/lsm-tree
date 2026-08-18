#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace lsm {

struct Options {
  std::size_t memtable_bytes = 64 * 1024;
  std::size_t bloom_bits_per_key = 10;
  std::size_t compaction_trigger = 4;
  bool sync_writes = true;
};

struct Stats {
  std::size_t memtable_entries = 0;
  std::size_t sstable_count = 0;
  std::uint64_t sequence = 0;
  std::uint64_t bloom_checks = 0;
  std::uint64_t bloom_negative_hits = 0;
  std::uint64_t sstable_reads = 0;
  std::uint64_t sstable_bytes_read = 0;
  std::uint64_t table_metadata_bytes = 0;
};

struct Entry {
  std::uint64_t sequence = 0;
  bool tombstone = false;
  std::string value;
};

}  // namespace lsm