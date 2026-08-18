#include "lsm/lsm.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "lsm/compaction.h"
#include "lsm/internal.h"
#include "lsm/recovery.h"
#include "lsm/sstable.h"
#include "lsm/wal.h"

namespace lsm {

LSMTree::LSMTree(std::filesystem::path directory, Options options)
    : directory_(std::move(directory)),
      wal_path_(directory_ / "active.wal"),
      options_(options) {
  if (options_.memtable_bytes == 0 || options_.bloom_bits_per_key == 0 ||
      options_.compaction_trigger < 2) {
    throw std::invalid_argument("invalid LSM options");
  }
  std::filesystem::create_directories(directory_);
  recovery::discover_tables(directory_, tables_, next_generation_, sequence_);
  wal_fd_ = ::open(wal_path_.c_str(), O_CREAT | O_RDWR | O_APPEND, 0644);
  if (wal_fd_ < 0) {
    internal::system_error("open WAL");
  }
  try {
    wal::recover(wal_fd_, memtable_, memtable_size_, sequence_);
  } catch (...) {
    ::close(wal_fd_);
    wal_fd_ = -1;
    throw;
  }
}

LSMTree::~LSMTree() {
  if (wal_fd_ >= 0) {
    ::close(wal_fd_);
  }
}

void LSMTree::put(const std::string& key, const std::string& value) {
  mutate(key, value, false);
}

void LSMTree::erase(const std::string& key) { mutate(key, {}, true); }

void LSMTree::mutate(const std::string& key, const std::string& value,
                     bool tombstone) {
  if (key.empty() || key.size() > internal::kMaxKey ||
      value.size() > internal::kMaxValue) {
    throw std::invalid_argument("invalid key or value length");
  }
  const std::uint64_t sequence = ++sequence_;
  wal::append(wal_fd_, sequence, key, value, tombstone,
              options_.sync_writes);

  const auto current = memtable_.find(key);
  if (current != memtable_.end()) {
    memtable_size_ -= current->first.size() + current->second.value.size();
  }
  memtable_[key] = {sequence, tombstone, value};
  memtable_size_ += key.size() + value.size();
  if (memtable_size_ >= options_.memtable_bytes) {
    flush();
  }
}

std::optional<std::string> LSMTree::get(const std::string& key) const {
  const auto memory = memtable_.find(key);
  if (memory != memtable_.end()) {
    return memory->second.tombstone
               ? std::nullopt
               : std::optional<std::string>(memory->second.value);
  }
  for (const auto& table : tables_) {
    ++bloom_checks_;
    if (!table->bloom.maybe_contains(key)) {
      ++bloom_negative_hits_;
      continue;
    }
    const auto found = std::lower_bound(
        table->entries.begin(), table->entries.end(), key,
        [](const auto& item, const std::string& target) {
          return item.first < target;
        });
    if (found != table->entries.end() && found->first == key) {
      return found->second.tombstone
                 ? std::nullopt
                 : std::optional<std::string>(found->second.value);
    }
  }
  return std::nullopt;
}

std::vector<std::pair<std::string, std::string>> LSMTree::scan(
    const std::string& begin, const std::string& end) const {
  if (end < begin) {
    throw std::invalid_argument("scan end precedes begin");
  }
  std::map<std::string, Entry> latest;
  for (const auto& table : tables_) {
    for (const auto& [key, entry] : table->entries) {
      if (key < begin || (!end.empty() && key >= end)) {
        continue;
      }
      const auto current = latest.find(key);
      if (current == latest.end() ||
          current->second.sequence < entry.sequence) {
        latest[key] = entry;
      }
    }
  }
  for (auto iterator = memtable_.lower_bound(begin);
       iterator != memtable_.end() && (end.empty() || iterator->first < end);
       ++iterator) {
    const auto current = latest.find(iterator->first);
    if (current == latest.end() ||
        current->second.sequence < iterator->second.sequence) {
      latest[iterator->first] = iterator->second;
    }
  }
  std::vector<std::pair<std::string, std::string>> output;
  output.reserve(latest.size());
  for (const auto& [key, entry] : latest) {
    if (!entry.tombstone) {
      output.push_back({key, entry.value});
    }
  }
  return output;
}

void LSMTree::flush() {
  if (memtable_.empty()) {
    return;
  }
  auto table = write_table(directory_, next_generation_,
                           options_.bloom_bits_per_key, memtable_);
  tables_.insert(tables_.begin(), std::move(table));
  wal::reset(wal_fd_);
  memtable_.clear();
  memtable_size_ = 0;
  if (tables_.size() >= options_.compaction_trigger) {
    compact();
  }
}

void LSMTree::compact() {
  if (tables_.size() < 2) {
    return;
  }
  auto replacement =
      compaction::merge_tables(tables_, directory_, next_generation_,
                               options_.bloom_bits_per_key);
  const auto old_tables = tables_;
  tables_.assign(1, replacement);
  for (const auto& table : old_tables) {
    std::error_code error;
    std::filesystem::remove(table->path, error);
    if (error) {
      throw std::runtime_error("failed to remove compacted SSTable");
    }
  }
  internal::sync_directory(directory_);
}

Stats LSMTree::stats() const {
  return {memtable_.size(), tables_.size(), sequence_, bloom_checks_,
          bloom_negative_hits_};
}

}  // namespace lsm
