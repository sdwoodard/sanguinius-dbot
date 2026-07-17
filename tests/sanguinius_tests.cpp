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

void test_message_logger() {
  const auto path =
      std::filesystem::current_path() / "sanguinius-test-messages.log";
  std::filesystem::remove(path);

  {
    sanguinius::MessageLogger logger{path};
    dpp::message message;
    message.id = 123;
    message.guild_id = 456;
    message.channel_id = 789;
    message.author.id = 42;
    message.author.username = "test-user";
    message.content = "hello\n\"Discord\"";

    dpp::attachment attachment{&message};
    attachment.filename = "picture.png";
    attachment.url = "https://cdn.example/picture.png";
    message.attachments.push_back(attachment);
    logger.log(message);
  }

  std::ifstream stream{path};
  const std::string output{std::istreambuf_iterator<char>{stream},
                           std::istreambuf_iterator<char>{}};
  expect(contains(output, "message_id=123"), "logs the message ID");
  expect(contains(output, "guild_id=456"), "logs the guild ID");
  expect(contains(output, "channel_id=789"), "logs the channel ID");
  expect(contains(output, "author_id=42"), "logs the author ID");
  expect(contains(output, "content=\"hello\\n\\\"Discord\\\"\""),
         "escapes message content");
  expect(contains(output,
                  "attachment=\"picture.png|https://cdn.example/picture.png\""),
         "logs attachments");
  expect(output.find('\n') == output.size() - 1,
         "writes exactly one physical line per message");

  std::filesystem::remove(path);
}

} // namespace

int main() {
  test_command_parser();
  test_message_logger();

  if (failures == 0) {
    std::cout << "All tests passed.\n";
  }
  return failures == 0 ? 0 : 1;
}
