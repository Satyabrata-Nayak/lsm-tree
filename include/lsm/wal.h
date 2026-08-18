#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>

#include "lsm/types.h"

namespace lsm {
namespace wal {

void append(int fd, std::uint64_t sequence, const std::string& key,
            const std::string& value, bool tombstone, bool sync);
void recover(int fd, std::map<std::string, Entry>& memtable,
             std::size_t& memtable_size, std::uint64_t& sequence);
void reset(int fd);

}  // namespace wal
}  // namespace lsm