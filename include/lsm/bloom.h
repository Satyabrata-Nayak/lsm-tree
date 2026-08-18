#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace lsm {

class Bloom {
 public:
  Bloom() = default;

  Bloom(std::size_t key_count, std::size_t bits_per_key);
  Bloom(std::vector<std::uint8_t> bytes, std::size_t bit_count,
        std::uint32_t hash_count);

  void add(const std::string& key);
  bool maybe_contains(const std::string& key) const;

  const std::vector<std::uint8_t>& bytes() const { return bytes_; }
  std::size_t bit_count() const { return bit_count_; }
  std::uint32_t hash_count() const { return hash_count_; }

 private:
  std::vector<std::uint8_t> bytes_;
  std::size_t bit_count_ = 0;
  std::uint32_t hash_count_ = 0;
};

}  // namespace lsm