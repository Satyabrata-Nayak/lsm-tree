#include "lsm/bloom.h"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace lsm {
namespace {

std::uint64_t fnv1a(const std::string& value) {
  std::uint64_t hash = 1469598103934665603ULL;
  for (const unsigned char byte : value) {
    hash ^= byte;
    hash *= 1099511628211ULL;
  }
  return hash;
}

std::uint64_t mix(std::uint64_t value) {
  value ^= value >> 30;
  value *= 0xbf58476d1ce4e5b9ULL;
  value ^= value >> 27;
  value *= 0x94d049bb133111ebULL;
  return value ^ (value >> 31);
}

}  // namespace

Bloom::Bloom(std::size_t key_count, std::size_t bits_per_key) {
  bit_count_ = std::max<std::size_t>(64, key_count * bits_per_key);
  bytes_.assign((bit_count_ + 7) / 8, 0);
  hash_count_ = static_cast<std::uint32_t>(
      std::clamp<std::size_t>(bits_per_key * 69 / 100, 1, 12));
}

Bloom::Bloom(std::vector<std::uint8_t> bytes, std::size_t bit_count,
             std::uint32_t hash_count)
    : bytes_(std::move(bytes)),
      bit_count_(bit_count),
      hash_count_(hash_count) {}

void Bloom::add(const std::string& key) {
  const std::uint64_t first = fnv1a(key);
  const std::uint64_t second = mix(first) | 1ULL;
  for (std::uint32_t i = 0; i < hash_count_; ++i) {
    const std::size_t bit =
        static_cast<std::size_t>((first + i * second) % bit_count_);
    bytes_[bit / 8] |= static_cast<std::uint8_t>(1U << (bit % 8));
  }
}

bool Bloom::maybe_contains(const std::string& key) const {
  if (bytes_.empty()) {
    return false;
  }
  const std::uint64_t first = fnv1a(key);
  const std::uint64_t second = mix(first) | 1ULL;
  for (std::uint32_t i = 0; i < hash_count_; ++i) {
    const std::size_t bit =
        static_cast<std::size_t>((first + i * second) % bit_count_);
    if ((bytes_[bit / 8] & static_cast<std::uint8_t>(1U << (bit % 8))) == 0) {
      return false;
    }
  }
  return true;
}

}  // namespace lsm