#include "lsm/sstable.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <utility>
#include <vector>

#include "lsm/internal.h"

namespace lsm {
using namespace internal;

std::uint64_t generation_from_path(const std::filesystem::path& path) {
  const std::string name = path.filename().string();
  if (name.rfind("sst-", 0) != 0 || path.extension() != ".dat") {
    return 0;
  }
  const std::string number = name.substr(4, name.size() - 8);
  if (number.empty() ||
      !std::all_of(number.begin(), number.end(),
                   [](unsigned char character) {
                     return std::isdigit(character) != 0;
                   })) {
    return 0;
  }
  return std::stoull(number);
}

std::shared_ptr<Table> load_table(const std::filesystem::path& path) {
  const std::uint64_t generation = generation_from_path(path);
  const int fd = ::open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    system_error("open SSTable");
  }
  try {
    struct stat info {};
    if (::fstat(fd, &info) != 0) {
      system_error("fstat SSTable");
    }
    const std::size_t size = static_cast<std::size_t>(info.st_size);
    if (size < kTableHeader + kChecksum) {
      throw std::runtime_error("SSTable is too small");
    }
    // Transient whole-file read: validates the trailing checksum so corrupt
    // tables are rejected at open time. Only the header, bloom filter and
    // sparse index are retained; data blocks are read on demand later.
    std::vector<std::uint8_t> bytes(size);
    if (!read_exact(fd, 0, bytes.data(), bytes.size())) {
      throw std::runtime_error("short SSTable read");
    }
    if (read_u32(bytes.data()) != kTableMagic ||
        read_u32(bytes.data() + 4) != kVersion) {
      throw std::runtime_error("unsupported SSTable header");
    }
    const std::uint32_t stored_crc = read_u32(bytes.data() + size - kChecksum);
    if (crc32(bytes.data(), size - kChecksum) != stored_crc) {
      throw std::runtime_error("SSTable checksum mismatch");
    }

    const std::uint64_t entry_count = read_u64(bytes.data() + 8);
    const std::uint64_t max_sequence = read_u64(bytes.data() + 16);
    const std::uint32_t bloom_bits = read_u32(bytes.data() + 24);
    const std::uint32_t bloom_hashes = read_u32(bytes.data() + 28);
    const std::uint32_t num_blocks = read_u32(bytes.data() + 32);
    const std::size_t bloom_bytes = (bloom_bits + 7U) / 8U;
    if (entry_count == 0 || num_blocks == 0 || num_blocks > entry_count ||
        kTableHeader + bloom_bytes + kChecksum > size) {
      throw std::runtime_error("invalid SSTable Bloom filter length");
    }

    std::vector<IndexEntry> index;
    index.reserve(num_blocks);
    std::size_t offset = kTableHeader + bloom_bytes;
    for (std::uint32_t block = 0; block < num_blocks; ++block) {
      if (offset + 4 > size - kChecksum) {
        throw std::runtime_error("truncated SSTable sparse index");
      }
      const std::uint32_t key_length = read_u32(bytes.data() + offset);
      offset += 4;
      if (key_length == 0 || key_length > kMaxKey ||
          offset + key_length + 8 > size - kChecksum) {
        throw std::runtime_error("invalid SSTable sparse index entry");
      }
      std::string first_key(
          reinterpret_cast<const char*>(bytes.data() + offset), key_length);
      offset += key_length;
      const std::uint64_t block_offset = read_u64(bytes.data() + offset);
      offset += 8;
      if (block_offset > size - kChecksum) {
        throw std::runtime_error("invalid SSTable block offset");
      }
      if (!index.empty() && index.back().first_key >= first_key) {
        throw std::runtime_error("SSTable index keys are not strictly ordered");
      }
      index.push_back({std::move(first_key), block_offset});
    }
    for (std::size_t block = 0; block < index.size(); ++block) {
      const std::uint64_t end = block + 1 < index.size()
                                    ? index[block + 1].offset
                                    : size - kChecksum;
      if (index[block].offset >= end) {
        throw std::runtime_error("invalid SSTable block layout");
      }
    }
    if (index.front().offset != offset) {
      throw std::runtime_error("invalid SSTable data offset");
    }

    // Validate records from the transient checksum buffer without retaining
    // them.  This rejects a structurally inconsistent table whose checksum was
    // recomputed, while avoiding extra reads during open.
    std::uint64_t parsed_entries = 0;
    std::uint64_t parsed_max_sequence = 0;
    std::string previous_key;
    for (std::size_t block = 0; block < index.size(); ++block) {
      const std::size_t block_end = static_cast<std::size_t>(
          block + 1 < index.size() ? index[block + 1].offset
                                   : size - kChecksum);
      std::size_t record_offset =
          static_cast<std::size_t>(index[block].offset);
      while (record_offset < block_end) {
        if (record_offset + kEntryHeader > block_end) {
          throw std::runtime_error("truncated SSTable block entry");
        }
        const std::uint64_t sequence = read_u64(bytes.data() + record_offset);
        const bool tombstone = bytes[record_offset + 8] != 0;
        if (bytes[record_offset + 9] != 0 || bytes[record_offset + 10] != 0 ||
            bytes[record_offset + 11] != 0) {
          throw std::runtime_error("invalid SSTable entry flags");
        }
        const std::uint32_t key_length =
            read_u32(bytes.data() + record_offset + 12);
        const std::uint32_t value_length =
            read_u32(bytes.data() + record_offset + 16);
        const std::size_t record_size =
            kEntryHeader + static_cast<std::size_t>(key_length) +
            static_cast<std::size_t>(value_length);
        if (key_length == 0 || key_length > kMaxKey ||
            value_length > kMaxValue || record_offset + record_size > block_end) {
          throw std::runtime_error("invalid SSTable block entry length");
        }
        const char* payload = reinterpret_cast<const char*>(
            bytes.data() + record_offset + kEntryHeader);
        const std::string key(payload, key_length);
        if (tombstone && value_length != 0) {
          throw std::runtime_error("tombstone has a value");
        }
        if (!previous_key.empty() && previous_key >= key) {
          throw std::runtime_error("SSTable keys are not strictly ordered");
        }
        previous_key = key;
        ++parsed_entries;
        parsed_max_sequence = std::max(parsed_max_sequence, sequence);
        record_offset += record_size;
      }
    }
    if (parsed_entries != entry_count || parsed_max_sequence != max_sequence) {
      throw std::runtime_error("inconsistent SSTable metadata");
    }

    auto table = std::make_shared<Table>();
    table->path = path;
    table->generation = generation;
    table->bloom = Bloom(
        std::vector<std::uint8_t>(
            bytes.begin() + static_cast<std::ptrdiff_t>(kTableHeader),
            bytes.begin() +
                static_cast<std::ptrdiff_t>(kTableHeader + bloom_bytes)),
        bloom_bits, bloom_hashes);
    table->index = std::move(index);
    table->entry_count = entry_count;
    table->max_sequence = max_sequence;
    table->file_size = size;
    table->fd = fd;
    return table;
  } catch (...) {
    ::close(fd);
    throw;
  }
}

std::shared_ptr<Table> write_table(
    const std::filesystem::path& directory, std::uint64_t& next_generation,
    std::size_t bloom_bits_per_key,
    const std::map<std::string, Entry>& entries) {
  Bloom bloom(entries.size(), bloom_bits_per_key);
  std::uint64_t max_sequence = 0;
  for (const auto& [key, entry] : entries) {
    bloom.add(key);
    max_sequence = std::max(max_sequence, entry.sequence);
  }

  // Group entries into data blocks of at most kBlockTarget bytes. A single
  // oversized record still gets its own block.
  std::vector<std::vector<std::pair<std::string, Entry>>> blocks;
  std::size_t current_size = 0;
  for (const auto& [key, entry] : entries) {
    const std::size_t record_size =
        kEntryHeader + key.size() + entry.value.size();
    if (!blocks.empty() && current_size + record_size > kBlockTarget) {
      blocks.emplace_back();
      current_size = 0;
    } else if (blocks.empty()) {
      blocks.emplace_back();
    }
    blocks.back().emplace_back(key, entry);
    current_size += record_size;
  }

  std::vector<std::uint64_t> block_sizes;
  block_sizes.reserve(blocks.size());
  for (const auto& block : blocks) {
    std::uint64_t block_size = 0;
    for (const auto& [key, entry] : block) {
      block_size += kEntryHeader + key.size() + entry.value.size();
    }
    block_sizes.push_back(block_size);
  }

  const std::size_t bloom_bytes = (bloom.bit_count() + 7U) / 8U;
  std::size_t index_bytes = 0;
  for (const auto& block : blocks) {
    index_bytes += 4 + block.front().first.size() + 8;
  }
  std::uint64_t block_offset = kTableHeader + bloom_bytes + index_bytes;

  std::vector<std::uint8_t> bytes;
  append_u32(bytes, kTableMagic);
  append_u32(bytes, kVersion);
  append_u64(bytes, entries.size());
  append_u64(bytes, max_sequence);
  append_u32(bytes, static_cast<std::uint32_t>(bloom.bit_count()));
  append_u32(bytes, bloom.hash_count());
  append_u32(bytes, static_cast<std::uint32_t>(blocks.size()));
  append_u32(bytes, 0);
  bytes.insert(bytes.end(), bloom.bytes().begin(), bloom.bytes().end());
  for (std::size_t block = 0; block < blocks.size(); ++block) {
    const std::string& first_key = blocks[block].front().first;
    append_u32(bytes, static_cast<std::uint32_t>(first_key.size()));
    bytes.insert(bytes.end(), first_key.begin(), first_key.end());
    append_u64(bytes, block_offset);
    block_offset += block_sizes[block];
  }
  for (const auto& block : blocks) {
    for (const auto& [key, entry] : block) {
      append_u64(bytes, entry.sequence);
      bytes.push_back(entry.tombstone ? 1 : 0);
      bytes.insert(bytes.end(), 3, 0);
      append_u32(bytes, static_cast<std::uint32_t>(key.size()));
      append_u32(bytes, static_cast<std::uint32_t>(entry.value.size()));
      bytes.insert(bytes.end(), key.begin(), key.end());
      bytes.insert(bytes.end(), entry.value.begin(), entry.value.end());
    }
  }
  append_u32(bytes, crc32(bytes.data(), bytes.size()));

  const std::uint64_t generation = next_generation++;
  const std::string base = "sst-" + std::to_string(generation) + ".dat";
  const auto temporary = directory / (base + ".tmp");
  const auto destination = directory / base;
  const int fd = ::open(temporary.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0644);
  if (fd < 0) {
    system_error("create SSTable");
  }
  try {
    write_all(fd, bytes.data(), bytes.size());
    if (::fsync(fd) != 0) {
      system_error("fsync SSTable");
    }
  } catch (...) {
    ::close(fd);
    throw;
  }
  ::close(fd);
  if (::rename(temporary.c_str(), destination.c_str()) != 0) {
    system_error("rename SSTable");
  }
  sync_directory(directory);
  return load_table(destination);
}

Table::~Table() {
  if (fd >= 0) {
    ::close(fd);
  }
}

std::ptrdiff_t find_block(const Table& table, const std::string& key) {
  const auto it = std::upper_bound(
      table.index.begin(), table.index.end(), key,
      [](const std::string& target, const IndexEntry& entry) {
        return target < entry.first_key;
      });
  return static_cast<std::ptrdiff_t>(it - table.index.begin()) - 1;
}

std::vector<std::pair<std::string, Entry>> read_block(const Table& table,
                                                      std::size_t block_index,
                                                      ReadStats* stats) {
  const std::uint64_t start = table.index[block_index].offset;
  const std::uint64_t end = block_index + 1 < table.index.size()
                                ? table.index[block_index + 1].offset
                                : table.file_size - kChecksum;
  if (start >= end || end > table.file_size - kChecksum) {
    throw std::runtime_error("invalid SSTable block range");
  }
  const std::size_t size = static_cast<std::size_t>(end - start);
  std::vector<std::uint8_t> bytes(size);
  if (!read_exact(table.fd, start, bytes.data(), size)) {
    throw std::runtime_error("short SSTable block read");
  }
  if (stats != nullptr) {
    ++stats->block_reads;
    stats->bytes_read += size;
  }

  std::vector<std::pair<std::string, Entry>> entries;
  std::size_t offset = 0;
  while (offset < size) {
    if (offset + kEntryHeader > size) {
      throw std::runtime_error("truncated SSTable block entry");
    }
    const std::uint64_t sequence = read_u64(bytes.data() + offset);
    const bool tombstone = bytes[offset + 8] != 0;
    if (bytes[offset + 9] != 0 || bytes[offset + 10] != 0 ||
        bytes[offset + 11] != 0) {
      throw std::runtime_error("invalid SSTable entry flags");
    }
    const std::uint32_t key_length = read_u32(bytes.data() + offset + 12);
    const std::uint32_t value_length = read_u32(bytes.data() + offset + 16);
    const std::size_t record_size =
        kEntryHeader + static_cast<std::size_t>(key_length) +
        static_cast<std::size_t>(value_length);
    if (key_length == 0 || key_length > kMaxKey ||
        value_length > kMaxValue || offset + record_size > size) {
      throw std::runtime_error("invalid SSTable block entry length");
    }
    const char* payload =
        reinterpret_cast<const char*>(bytes.data() + offset + kEntryHeader);
    std::string key(payload, key_length);
    std::string value(payload + key_length, value_length);
    if (tombstone && !value.empty()) {
      throw std::runtime_error("tombstone has a value");
    }
    if (!entries.empty() && entries.back().first >= key) {
      throw std::runtime_error("SSTable block keys are not strictly ordered");
    }
    entries.emplace_back(std::move(key),
                         Entry{sequence, tombstone, std::move(value)});
    offset += record_size;
  }
  return entries;
}

void for_each_entry(
    const Table& table,
    const std::function<void(const std::string&, const Entry&)>& visit,
    ReadStats* stats) {
  for (std::size_t block = 0; block < table.index.size(); ++block) {
    const auto entries = read_block(table, block, stats);
    for (const auto& [key, entry] : entries) {
      visit(key, entry);
    }
  }
}

std::uint64_t table_metadata_bytes(const Table& table) {
  std::uint64_t index_bytes = 0;
  for (const auto& entry : table.index) {
    index_bytes += 4 + entry.first_key.size() + 8;
  }
  return kTableHeader + table.bloom.bytes().size() + index_bytes;
}

}  // namespace lsm
