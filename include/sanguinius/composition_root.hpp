#pragma once

#include "sanguinius/application.hpp"
#include "sanguinius/config.hpp"

#include <memory>

namespace sanguinius {

void validate_runtime_configuration(const Config &config);

[[nodiscard]] std::unique_ptr<Application>
make_application(const Config &config);

} // namespace sanguinius
