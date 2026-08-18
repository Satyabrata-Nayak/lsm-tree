#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <vector>

#include "lsm/sstable.h"
#include "lsm/types.h"

namespace lsm {
namespace compaction {

std::shared_ptr<Table> merge_tables(
    const std::vector<std::shared_ptr<Table>>& tables,
    const std::filesystem::path& directory, std::uint64_t& next_generation,
    std::size_t bloom_bits_per_key);

}  // namespace compaction
}  // namespace lsm