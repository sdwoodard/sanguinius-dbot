#include "sanguinius/build_info.hpp"
#include "sanguinius/composition_root.hpp"
#include "sanguinius/config.hpp"
#include "sanguinius/process_signals.hpp"

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string_view>

namespace {

void print_usage(std::ostream &stream, const std::string_view executable) {
  stream << "Usage: " << executable << " [--check-config|--help]\n";
}

} // namespace

int main(const int argc, char **argv) {
  try {
    if (argc > 2) {
      print_usage(std::cerr, argv[0]);
      return 2;
    }
    if (argc == 2 && std::string_view{argv[1]} == "--help") {
      print_usage(std::cout, argv[0]);
      return EXIT_SUCCESS;
    }
    if (argc == 2 && std::string_view{argv[1]} != "--check-config") {
      print_usage(std::cerr, argv[0]);
      return 2;
    }

    const auto config = sanguinius::Config::from_environment();
    if (argc == 2) {
      std::cout << sanguinius::redacted_config_summary(
          config, sanguinius::current_build_info());
      return EXIT_SUCCESS;
    }

    sanguinius::ProcessSignals signals;
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
