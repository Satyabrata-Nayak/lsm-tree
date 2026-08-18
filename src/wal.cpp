#include "lsm/wal.h"

#include <cstdint>
#include <limits>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <vector>

#include "lsm/internal.h"

namespace lsm {
namespace wal {
using namespace internal;

void append(int fd, std::uint64_t sequence, const std::string& key,
            const std::string& value, bool tombstone, bool sync) {
  std::vector<std::uint8_t> record;
  record.reserve(kWalHeader + key.size() + value.size() + kChecksum);
  append_u32(record, kWalMagic);
  append_u64(record, sequence);
  record.push_back(tombstone ? 2 : 1);
  record.insert(record.end(), 3, 0);
  append_u32(record, static_cast<std::uint32_t>(key.size()));
  append_u32(record, static_cast<std::uint32_t>(value.size()));
  record.insert(record.end(), key.begin(), key.end());
  record.insert(record.end(), value.begin(), value.end());
  append_u32(record, crc32(record.data(), record.size()));
  write_all(fd, record.data(), record.size());
  if (sync && ::fsync(fd) != 0) {
    system_error("fsync WAL");
  }
}

void recover(int fd, std::map<std::string, Entry>& memtable,
             std::size_t& memtable_size, std::uint64_t& sequence) {
  struct stat info {};
  if (::fstat(fd, &info) != 0) {
    system_error("fstat WAL");
  }
  const std::uint64_t file_size = static_cast<std::uint64_t>(info.st_size);
  std::uint64_t offset = 0;
  while (offset < file_size) {
    if (file_size - offset < kWalHeader) {
      break;
    }
    std::vector<std::uint8_t> header(kWalHeader);
    if (!read_exact(fd, offset, header.data(), header.size())) {
      break;
    }
    const std::uint32_t magic = read_u32(header.data());
    const std::uint64_t record_sequence = read_u64(header.data() + 4);
    const std::uint8_t type = header[12];
    const std::uint32_t key_length = read_u32(header.data() + 16);
    const std::uint32_t value_length = read_u32(header.data() + 20);
    const std::uint64_t record_size =
        kWalHeader + static_cast<std::uint64_t>(key_length) +
        static_cast<std::uint64_t>(value_length) + kChecksum;
    if (magic != kWalMagic || record_sequence == 0 ||
        (type != 1 && type != 2) || header[13] != 0 || header[14] != 0 ||
        header[15] != 0 || key_length == 0 || key_length > kMaxKey ||
        value_length > kMaxValue || (type == 2 && value_length != 0) ||
        record_size > file_size - offset ||
        record_size > std::numeric_limits<std::size_t>::max()) {
      break;
    }
    std::vector<std::uint8_t> record(static_cast<std::size_t>(record_size));
    if (!read_exact(fd, offset, record.data(), record.size())) {
      break;
    }
    const std::uint32_t stored =
        read_u32(record.data() + record.size() - kChecksum);
    if (crc32(record.data(), record.size() - kChecksum) != stored) {
      break;
    }
    const char* payload =
        reinterpret_cast<const char*>(record.data() + kWalHeader);
    std::string key(payload, key_length);
    std::string value(payload + key_length, value_length);
    const Entry entry{record_sequence, type == 2, std::move(value)};
    const auto current = memtable.find(key);
    if (current == memtable.end() ||
        current->second.sequence <= entry.sequence) {
      if (current != memtable.end()) {
        memtable_size -= current->first.size() + current->second.value.size();
      }
      memtable_size += key.size() + entry.value.size();
      memtable[std::move(key)] = entry;
    }
    sequence = std::max(sequence, record_sequence);
    offset += record_size;
  }
  if (offset != file_size) {
    if (::ftruncate(fd, static_cast<off_t>(offset)) != 0 ||
        ::fsync(fd) != 0) {
      system_error("repair WAL");
    }
  }
}

void reset(int fd) {
  if (::ftruncate(fd, 0) != 0 || ::fsync(fd) != 0) {
    system_error("reset WAL");
  }
}

}  // namespace wal
}  // namespace lsm