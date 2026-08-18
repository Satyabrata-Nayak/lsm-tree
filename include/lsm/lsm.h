#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <vector>

#include "lsm/sstable.h"
#include "lsm/types.h"

namespace lsm {

class LSMTree {
 public:
  explicit LSMTree(std::filesystem::path directory,
                   Options options = Options{});
  ~LSMTree();

  LSMTree(const LSMTree&) = delete;
  LSMTree& operator=(const LSMTree&) = delete;

  void put(const std::string& key, const std::string& value);
  void erase(const std::string& key);
  std::optional<std::string> get(const std::string& key) const;
  std::vector<std::pair<std::string, std::string>> scan(
      const std::string& begin, const std::string& end) const;

  void flush();
  void compact();
  Stats stats() const;

 private:
  void mutate(const std::string& key, const std::string& value,
              bool tombstone);
  void flush_unlocked();
  void compact_unlocked();
  void record_read_stats(const ReadStats& stats) const;

  std::filesystem::path directory_;
  std::filesystem::path wal_path_;
  Options options_;
  int wal_fd_ = -1;
  std::uint64_t sequence_ = 0;
  std::uint64_t next_generation_ = 1;
  std::size_t memtable_size_ = 0;
  std::map<std::string, Entry> memtable_;
  std::vector<std::shared_ptr<Table>> tables_;
  mutable std::shared_mutex mutex_;
  mutable std::mutex stats_mutex_;
  mutable std::uint64_t bloom_checks_ = 0;
  mutable std::uint64_t bloom_negative_hits_ = 0;
  mutable ReadStats sstable_read_stats_;
};

}  // namespace lsm
