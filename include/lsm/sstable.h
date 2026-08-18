#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lsm/bloom.h"
#include "lsm/types.h"

namespace lsm {

// One entry of the sparse index: the first key of a data block and the file
// offset at which that block starts.
struct IndexEntry {
  std::string first_key;
  std::uint64_t offset = 0;
};

// An open SSTable. Only the header, bloom filter and sparse index are kept in
// memory; data blocks are read on demand with pread() through the open fd.
struct Table {
  std::filesystem::path path;
  std::uint64_t generation = 0;
  Bloom bloom;
  std::vector<IndexEntry> index;
  std::uint64_t entry_count = 0;
  std::uint64_t max_sequence = 0;
  std::uint64_t file_size = 0;
  int fd = -1;

  Table() = default;
  Table(const Table&) = delete;
  Table& operator=(const Table&) = delete;
  ~Table();
};

// Accumulates on-disk read activity so callers can report it in stats.
struct ReadStats {
  std::uint64_t block_reads = 0;
  std::uint64_t bytes_read = 0;
};

std::uint64_t generation_from_path(const std::filesystem::path& path);
std::shared_ptr<Table> load_table(const std::filesystem::path& path);
std::shared_ptr<Table> write_table(const std::filesystem::path& directory,
                                   std::uint64_t& next_generation,
                                   std::size_t bloom_bits_per_key,
                                   const std::map<std::string, Entry>& entries);

// Index of the block that could contain `key`, or -1 when `key` sorts before
// the first block's first key.
std::ptrdiff_t find_block(const Table& table, const std::string& key);

// Reads and parses one data block. `stats` is optional.
std::vector<std::pair<std::string, Entry>> read_block(const Table& table,
                                                      std::size_t block_index,
                                                      ReadStats* stats = nullptr);

// Visits every entry of the table in key order. `stats` is optional.
void for_each_entry(
    const Table& table,
    const std::function<void(const std::string&, const Entry&)>& visit,
    ReadStats* stats = nullptr);

// Bytes of header + bloom filter + sparse index retained in memory.
std::uint64_t table_metadata_bytes(const Table& table);

}  // namespace lsm
