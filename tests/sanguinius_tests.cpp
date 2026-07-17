#include "sanguinius/ai_responder.hpp"
#include "sanguinius/message_handler.hpp"
#include "sanguinius/message_logger.hpp"

#include <dpp/dpp.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>

namespace {

int failures = 0;

void expect(const bool condition, const std::string_view description) {
  if (!condition) {
    std::cerr << "FAIL: " << description << '\n';
    ++failures;
  }
}

[[nodiscard]] bool contains(const std::string_view text,
                            const std::string_view fragment) {
  return text.find(fragment) != std::string_view::npos;
}

void test_command_parser() {
  using enum sanguinius::Command;
  expect(sanguinius::parse_command("!help", "!") == help, "parses help");
  expect(sanguinius::parse_command("!HELP", "!") == help,
         "commands are case-insensitive");
  expect(sanguinius::parse_command("!repo extra", "!") == repo,
         "allows command arguments");
  expect(sanguinius::parse_command("?repo", "!") == none,
         "rejects another prefix");
  expect(sanguinius::parse_command("!join", "!") == none,
         "removed join command stays removed");
  expect(sanguinius::parse_command("!gpt question", "!") == none,
         "removed GPT command stays removed");
  expect(sanguinius::parse_command("ordinary message", "!") == none,
         "ignores regular messages");
}

void test_bot_mention_parser() {
  const dpp::snowflake bot_id{123};
  expect(sanguinius::prompt_after_bot_mention("<@123> hello", bot_id) ==
             "hello",
         "parses a leading bot mention");
  expect(sanguinius::prompt_after_bot_mention("  <@!123>: hello", bot_id) ==
             "hello",
         "parses a nickname mention and separator");
  expect(sanguinius::prompt_after_bot_mention("<@123>", bot_id) == "",
         "allows a mention with no explicit prompt");
  expect(!sanguinius::prompt_after_bot_mention("hello <@123>", bot_id),
         "requires the mention at the beginning");
  expect(!sanguinius::prompt_after_bot_mention("<@456> hello", bot_id),
         "rejects a mention of another user");
}

void test_message_logger() {
  const auto path =
      std::filesystem::current_path() / "sanguinius-test-messages.log";
  std::filesystem::remove(path);

  {
    sanguinius::MessageLogger logger{path};
    dpp::message message;
    message.author.username = "test-user";
    message.content = "hello\n\"Discord\"";
    logger.log(message);
  }

  std::ifstream stream{path};
  const std::string output{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
  expect(contains(output, "-04:00 author=") ||
             contains(output, "-05:00 author="),
         "uses the daylight-aware New York UTC offset");
  expect(contains(output, "author=\"test-user\""), "logs the author");
  expect(contains(output, "message=\"hello\\n\\\"Discord\\\"\""),
         "escapes message content");
  expect(!contains(output, "message_id="), "does not log the message ID");
  expect(!contains(output, "guild_id="), "does not log the guild ID");
  expect(!contains(output, "channel_id="), "does not log the channel ID");
  expect(!contains(output, "author_id="), "does not log the author ID");
  expect(!contains(output, "bot="), "does not log bot status");
  expect(output.find('\n') == output.size() - 1,
         "writes exactly one physical line per message");

  std::filesystem::remove(path);
}

} // namespace

int main() {
  test_command_parser();
  test_bot_mention_parser();
  test_message_logger();

  if (failures == 0) {
    std::cout << "All tests passed.\n";
  }
  return failures == 0 ? 0 : 1;
}
