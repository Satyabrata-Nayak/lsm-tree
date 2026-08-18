#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "lsm/sstable.h"

namespace lsm {
namespace recovery {

void discover_tables(const std::filesystem::path& directory,
                     std::vector<std::shared_ptr<Table>>& tables,
                     std::uint64_t& next_generation, std::uint64_t& sequence);

}  // namespace recovery
}  // namespace lsm