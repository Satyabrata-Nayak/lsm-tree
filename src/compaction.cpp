#include "lsm/compaction.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lsm/sstable.h"

namespace lsm {
namespace compaction {

std::shared_ptr<Table> merge_tables(
    const std::vector<std::shared_ptr<Table>>& tables,
    const std::filesystem::path& directory, std::uint64_t& next_generation,
    std::size_t bloom_bits_per_key) {
  std::map<std::string, Entry> merged;
  for (const auto& table : tables) {
    for (const auto& [key, entry] : table->entries) {
      const auto current = merged.find(key);
      if (current == merged.end() ||
          current->second.sequence < entry.sequence) {
        merged[key] = entry;
      }
    }
  }
  return write_table(directory, next_generation, bloom_bits_per_key, merged);
}

}  // namespace compaction
}  // namespace lsm