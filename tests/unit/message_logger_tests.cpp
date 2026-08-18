#include "sanguinius/message_logger.hpp"

#include "support/fake_clock.hpp"

#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace {

class TemporaryDirectory {
public:
  TemporaryDirectory() {
    static std::atomic<unsigned long long> sequence{0};
    const auto unique = sequence.fetch_add(1, std::memory_order_relaxed);
    path_ = std::filesystem::temp_directory_path() /
            ("sanguinius-logger-test-" + std::to_string(unique) + "-" +
             std::to_string(
                 std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(path_);
  }

  ~TemporaryDirectory() { std::filesystem::remove_all(path_); }

  [[nodiscard]] const std::filesystem::path &path() const { return path_; }

private:
  std::filesystem::path path_;
};

[[nodiscard]] bool contains(const std::string_view text,
                            const std::string_view fragment) {
  return text.find(fragment) != std::string_view::npos;
}

} // namespace

TEST_CASE("plaintext message log preserves format and privacy", "[logger]") {
  using namespace std::chrono;
  const auto instant =
      sys_days{year{2026} / July / 17} + hours{14} + minutes{22} + seconds{5};
  sanguinius::test::FakeClock clock{instant};
  TemporaryDirectory directory;
  const auto path = directory.path() / "messages.log";

  {
    sanguinius::MessageLogger logger{path, clock};
    logger.append({"test-user", "hello\n\"Discord\""});
  }

  std::ifstream stream{path};
  const std::string output{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
  REQUIRE(contains(output, "2026-07-17T10:22:05-04:00 author="));
  REQUIRE(contains(output, "author=\"test-user\""));
  REQUIRE(contains(output, "message=\"hello\\n\\\"Discord\\\"\""));
  REQUIRE_FALSE(contains(output, "message_id="));
  REQUIRE_FALSE(contains(output, "guild_id="));
  REQUIRE_FALSE(contains(output, "channel_id="));
  REQUIRE_FALSE(contains(output, "author_id="));
  REQUIRE_FALSE(contains(output, "bot="));
  REQUIRE(output.find('\n') == output.size() - 1);

  const auto permissions = std::filesystem::status(path).permissions();
  const auto expected =
      std::filesystem::perms::owner_read | std::filesystem::perms::owner_write;
  REQUIRE((permissions & std::filesystem::perms::mask) == expected);

  const auto secret_path = directory.path() / "LOG_PATH_SECRET_SENTINEL";
  {
    std::ofstream blocking_file{secret_path};
    REQUIRE(blocking_file.good());
  }

  std::string startup_error;
  try {
    sanguinius::MessageLogger invalid_logger{secret_path / "messages.log",
                                             clock};
  } catch (const std::exception &error) {
    startup_error = error.what();
  }
  REQUIRE(startup_error == "Unable to initialize the message log configured by "
                           "SANGUINIUS_LOG_FILE.");
  REQUIRE_FALSE(contains(startup_error, "LOG_PATH_SECRET_SENTINEL"));
}
