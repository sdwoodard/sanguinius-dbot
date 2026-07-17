#pragma once

#include "sanguinius/openai_client.hpp"

#include <dpp/dpp.h>

#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace sanguinius {

[[nodiscard]] std::optional<std::string>
prompt_after_bot_mention(std::string_view content, dpp::snowflake bot_id);

class AiResponder {
public:
  AiResponder(dpp::cluster &bot, const OpenAiClient &client,
              std::string persona, std::size_t worker_count = 2);
  ~AiResponder();

  AiResponder(const AiResponder &) = delete;
  AiResponder &operator=(const AiResponder &) = delete;
  AiResponder(AiResponder &&) = delete;
  AiResponder &operator=(AiResponder &&) = delete;

  [[nodiscard]] bool handles(const dpp::message &message) const;
  [[nodiscard]] bool enqueue(const dpp::message &message);

private:
  void run();
  void respond(const dpp::message &message) const;
  [[nodiscard]] std::vector<dpp::message>
  recent_messages(const dpp::message &message) const;
  [[nodiscard]] std::optional<dpp::message>
  replied_message(const dpp::message &message,
                  const std::vector<dpp::message> &recent) const;
  [[nodiscard]] std::string
  create_prompt(const dpp::message &message,
                const std::vector<dpp::message> &recent,
                const std::optional<dpp::message> &replied) const;

  dpp::cluster &bot_;
  const OpenAiClient &client_;
  std::string persona_;
  std::mutex queue_mutex_;
  std::condition_variable queue_ready_;
  std::queue<dpp::message> queue_;
  bool stopping_{false};
  std::vector<std::thread> workers_;
};

} // namespace sanguinius
