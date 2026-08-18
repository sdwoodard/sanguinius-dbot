#include "sanguinius/composition_root.hpp"
#include "sanguinius/config.hpp"
#include "sanguinius/process_signals.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
  try {
    sanguinius::ProcessSignals signals;
    const auto config = sanguinius::Config::from_environment();
    auto application = sanguinius::make_application(config);
    application->start();
    static_cast<void>(signals.wait());
    application->stop();
    return EXIT_SUCCESS;
  } catch (const std::exception &error) {
    std::cerr << "Fatal error: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
}
