#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lsm/bloom.h"
#include "lsm/types.h"

namespace lsm {

struct Table {
  std::filesystem::path path;
  std::uint64_t generation = 0;
  Bloom bloom;
  std::vector<std::pair<std::string, Entry>> entries;
};

std::uint64_t generation_from_path(const std::filesystem::path& path);
std::shared_ptr<Table> load_table(const std::filesystem::path& path);
std::shared_ptr<Table> write_table(const std::filesystem::path& directory,
                                   std::uint64_t& next_generation,
                                   std::size_t bloom_bits_per_key,
                                   const std::map<std::string, Entry>& entries);

}  // namespace lsm