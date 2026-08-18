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

    const std::uint64_t count = read_u64(bytes.data() + 8);
    const std::uint32_t bloom_bits = read_u32(bytes.data() + 16);
    const std::uint32_t bloom_hashes = read_u32(bytes.data() + 20);
    const std::size_t bloom_bytes = (bloom_bits + 7U) / 8U;
    if (kTableHeader + bloom_bytes + kChecksum > size) {
      throw std::runtime_error("invalid SSTable Bloom filter length");
    }
    std::vector<std::uint8_t> bloom(
        bytes.begin() + static_cast<std::ptrdiff_t>(kTableHeader),
        bytes.begin() +
            static_cast<std::ptrdiff_t>(kTableHeader + bloom_bytes));

    auto table = std::make_shared<Table>();
    table->path = path;
    table->generation = generation;
    table->bloom = Bloom(std::move(bloom), bloom_bits, bloom_hashes);
    std::size_t offset = kTableHeader + bloom_bytes;
    for (std::uint64_t index = 0; index < count; ++index) {
      if (offset + kEntryHeader > size - kChecksum) {
        throw std::runtime_error("truncated SSTable entry");
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
          value_length > kMaxValue ||
          offset + record_size > size - kChecksum) {
        throw std::runtime_error("invalid SSTable entry length");
      }
      const char* payload =
          reinterpret_cast<const char*>(bytes.data() + offset + kEntryHeader);
      std::string key(payload, key_length);
      std::string value(payload + key_length, value_length);
      if (tombstone && !value.empty()) {
        throw std::runtime_error("tombstone has a value");
      }
      if (!table->entries.empty() && table->entries.back().first >= key) {
        throw std::runtime_error("SSTable keys are not strictly ordered");
      }
      table->entries.push_back(
          {std::move(key), {sequence, tombstone, std::move(value)}});
      offset += record_size;
    }
    if (offset != size - kChecksum) {
      throw std::runtime_error("unexpected bytes after SSTable entries");
    }
    ::close(fd);
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
  for (const auto& [key, entry] : entries) {
    static_cast<void>(entry);
    bloom.add(key);
  }
  std::vector<std::uint8_t> bytes;
  append_u32(bytes, kTableMagic);
  append_u32(bytes, kVersion);
  append_u64(bytes, entries.size());
  append_u32(bytes, static_cast<std::uint32_t>(bloom.bit_count()));
  append_u32(bytes, bloom.hash_count());
  bytes.insert(bytes.end(), bloom.bytes().begin(), bloom.bytes().end());
  for (const auto& [key, entry] : entries) {
    append_u64(bytes, entry.sequence);
    bytes.push_back(entry.tombstone ? 1 : 0);
    bytes.insert(bytes.end(), 3, 0);
    append_u32(bytes, static_cast<std::uint32_t>(key.size()));
    append_u32(bytes, static_cast<std::uint32_t>(entry.value.size()));
    bytes.insert(bytes.end(), key.begin(), key.end());
    bytes.insert(bytes.end(), entry.value.begin(), entry.value.end());
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

}  // namespace lsm