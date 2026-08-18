#pragma once

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <unistd.h>
#include <vector>

namespace lsm {
namespace internal {

constexpr std::uint32_t kWalMagic = 0x314C4157U;    // WAL1
constexpr std::uint32_t kTableMagic = 0x31545353U;  // SST1
constexpr std::uint32_t kVersion = 1;
constexpr std::size_t kWalHeader = 24;
constexpr std::size_t kTableHeader = 24;
constexpr std::size_t kEntryHeader = 20;
constexpr std::size_t kChecksum = 4;
constexpr std::uint32_t kMaxKey = 1U << 20;
constexpr std::uint32_t kMaxValue = 16U << 20;

[[noreturn]] inline void system_error(const std::string& operation) {
  throw std::runtime_error(operation + ": " + std::strerror(errno));
}

inline void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

inline void append_u64(std::vector<std::uint8_t>& out, std::uint64_t value) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<std::uint8_t>(value >> shift));
  }
}

inline std::uint32_t read_u32(const std::uint8_t* data) {
  std::uint32_t value = 0;
  for (int shift = 0; shift < 32; shift += 8) {
    value |= static_cast<std::uint32_t>(*data++) << shift;
  }
  return value;
}

inline std::uint64_t read_u64(const std::uint8_t* data) {
  std::uint64_t value = 0;
  for (int shift = 0; shift < 64; shift += 8) {
    value |= static_cast<std::uint64_t>(*data++) << shift;
  }
  return value;
}

inline void write_all(int fd, const std::uint8_t* data, std::size_t size) {
  const char* split = std::getenv("LSM_SPLIT_WRITES");
  const bool split_writes = split != nullptr && std::string(split) == "1";
  const std::size_t chunk_size = split_writes ? 7 : size;
  std::size_t offset = 0;
  while (offset < size) {
    const std::size_t requested = std::min(chunk_size, size - offset);
    const ssize_t count = ::write(fd, data + offset, requested);
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      system_error("write");
    }
    if (count == 0) {
      throw std::runtime_error("write returned zero bytes");
    }
    offset += static_cast<std::size_t>(count);
    if (split_writes) {
      ::usleep(100);
    }
  }
}

inline bool read_exact(int fd, std::uint64_t offset, std::uint8_t* data,
                       std::size_t size) {
  std::size_t complete = 0;
  while (complete < size) {
    const ssize_t count =
        ::pread(fd, data + complete, size - complete,
                static_cast<off_t>(offset + complete));
    if (count < 0) {
      if (errno == EINTR) {
        continue;
      }
      system_error("pread");
    }
    if (count == 0) {
      return false;
    }
    complete += static_cast<std::size_t>(count);
  }
  return true;
}

inline void sync_directory(const std::filesystem::path& directory) {
  const int fd = ::open(directory.c_str(), O_RDONLY);
  if (fd < 0) {
    system_error("open database directory");
  }
  if (::fsync(fd) != 0) {
    const int saved = errno;
    ::close(fd);
    errno = saved;
    system_error("fsync database directory");
  }
  ::close(fd);
}

}  // namespace internal
}  // namespace lsm

namespace lsm {

inline std::uint32_t crc32(const std::uint8_t* data, std::size_t size) {
  std::uint32_t crc = 0xFFFFFFFFU;
  for (std::size_t i = 0; i < size; ++i) {
    crc ^= data[i];
    for (int bit = 0; bit < 8; ++bit) {
      const std::uint32_t mask =
          static_cast<std::uint32_t>(-static_cast<std::int32_t>(crc & 1U));
      crc = (crc >> 1) ^ (0xEDB88320U & mask);
    }
  }
  return ~crc;
}

}  // namespace lsm