#pragma once

#include "sanguinius/application.hpp"
#include "sanguinius/config.hpp"

#include <memory>

namespace sanguinius {

[[nodiscard]] std::unique_ptr<Application>
make_application(const Config &config);

} // namespace sanguinius
