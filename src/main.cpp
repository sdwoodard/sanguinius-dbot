#include "sanguinius/ai_responder.hpp"
#include "sanguinius/config.hpp"
#include "sanguinius/message_handler.hpp"
#include "sanguinius/message_logger.hpp"
#include "sanguinius/openai_client.hpp"

#include <dpp/dpp.h>

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
  try {
    const auto config = sanguinius::Config::from_environment();
    dpp::cluster bot{config.token,
                     dpp::i_default_intents | dpp::i_message_content};
    sanguinius::MessageLogger logger{config.message_log};
    sanguinius::OpenAiClient openai{config.openai_api_key, config.openai_model};
    sanguinius::AiResponder ai_responder{bot, openai, config.persona};
    sanguinius::MessageHandler handler{logger, ai_responder,
                                       config.command_prefix};

    bot.on_log(dpp::utility::cout_logger());
    bot.on_ready([&bot](const dpp::ready_t &) {
      std::cout << "Sanguinius is connected as " << bot.me.username << ".\n";
    });
    bot.on_message_create(
        [&handler](const dpp::message_create_t &event) { handler(event); });

    bot.start(dpp::st_wait);
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "Fatal error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
