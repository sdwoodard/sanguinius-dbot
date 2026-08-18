#pragma once

#include <atomic>
#include <chrono>
#include <filesystem>

namespace sanguinius::test {

class TemporaryDatabase {
public:
  TemporaryDatabase() {
    static std::atomic<unsigned long long> sequence{0};
    root_ = std::filesystem::temp_directory_path() /
            ("sanguinius-database-test-" +
             std::to_string(sequence.fetch_add(1, std::memory_order_relaxed)) +
             "-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    database_ = root_ / "sanguinius.sqlite3";
  }

  ~TemporaryDatabase() {
    std::error_code ignored;
    std::filesystem::remove_all(root_, ignored);
  }

  TemporaryDatabase(const TemporaryDatabase &) = delete;
  TemporaryDatabase &operator=(const TemporaryDatabase &) = delete;

  [[nodiscard]] const std::filesystem::path &root() const noexcept {
    return root_;
  }
  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return database_;
  }

private:
  std::filesystem::path root_;
  std::filesystem::path database_;
};

} // namespace sanguinius::test
