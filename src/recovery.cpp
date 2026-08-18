#include "lsm/recovery.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lsm/internal.h"
#include "lsm/sstable.h"

namespace lsm {
namespace recovery {
using namespace internal;

void discover_tables(const std::filesystem::path& directory,
                     std::vector<std::shared_ptr<Table>>& tables,
                     std::uint64_t& next_generation, std::uint64_t& sequence) {
  tables.clear();
  for (const auto& item : std::filesystem::directory_iterator(directory)) {
    if (!item.is_regular_file()) {
      continue;
    }
    const std::uint64_t generation = generation_from_path(item.path());
    if (generation == 0) {
      continue;
    }
    auto table = load_table(item.path());
    tables.push_back(std::move(table));
    next_generation = std::max(next_generation, generation + 1);
  }
  std::sort(tables.begin(), tables.end(),
            [](const auto& left, const auto& right) {
              return left->generation > right->generation;
            });
  for (const auto& table : tables) {
      sequence = std::max(sequence, table->max_sequence);
    }
}

}  // namespace recovery
}  // namespace lsm