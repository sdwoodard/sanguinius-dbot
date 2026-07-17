#include "sanguinius/config.hpp"
#include "sanguinius/message_handler.hpp"
#include "sanguinius/message_logger.hpp"

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
    sanguinius::MessageHandler handler{logger, config.command_prefix};

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
