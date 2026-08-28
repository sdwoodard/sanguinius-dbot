#include "sanguinius/health.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <fcntl.h>
#include <nlohmann/json.hpp>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

namespace sanguinius {
namespace {

constexpr std::size_t maximum_status_file_size = 16U * 1024U;
constexpr std::uintmax_t minimum_state_free_bytes = 512U * 1024U * 1024U;
constexpr std::uintmax_t minimum_backup_free_bytes = 1024U * 1024U * 1024U;

[[nodiscard]] std::optional<bool>
disk_warning(const std::filesystem::path &directory,
             const std::uintmax_t minimum_free_bytes) noexcept {
  std::error_code error;
  const auto space = std::filesystem::space(directory, error);
  if (error || space.capacity == 0)
    return std::nullopt;
  const auto threshold = std::max(minimum_free_bytes, space.capacity / 10U);
  return space.available < threshold;
}

[[nodiscard]] bool safe_result(const std::string_view value) noexcept {
  return value == "idle" || value == "running" || value == "succeeded" ||
         value == "failed" || value == "never";
}

[[nodiscard]] std::optional<std::string>
read_root_owned_file(const std::filesystem::path &path) {
  const int descriptor =
      ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0)
    return std::nullopt;
  struct DescriptorGuard {
    int descriptor;
    ~DescriptorGuard() { static_cast<void>(::close(descriptor)); }
  } guard{descriptor};
  struct stat status{};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_uid != 0 || (status.st_mode & (S_IWGRP | S_IWOTH)) != 0 ||
      status.st_size < 0 ||
      static_cast<std::uintmax_t>(status.st_size) > maximum_status_file_size)
    return std::nullopt;
  std::string content(static_cast<std::size_t>(status.st_size), '\0');
  std::size_t offset{};
  while (offset < content.size()) {
    const auto count =
        ::read(descriptor, content.data() + offset, content.size() - offset);
    if (count < 0 && errno == EINTR)
      continue;
    if (count <= 0)
      return std::nullopt;
    offset += static_cast<std::size_t>(count);
  }
  return content;
}

[[nodiscard]] std::optional<std::int64_t>
age_seconds(const nlohmann::json &object, const char *field,
            const std::int64_t now_ms) {
  const auto found = object.find(field);
  if (found == object.end() || !found->is_number_integer())
    return std::nullopt;
  const auto timestamp = found->get<std::int64_t>();
  if (timestamp <= 0 || timestamp > now_ms + 300'000)
    return std::nullopt;
  return std::max<std::int64_t>(0, now_ms - timestamp) / 1'000;
}

} // namespace

OperationsHealth
read_operations_health(const std::filesystem::path &status_file,
                       const std::filesystem::path &state_directory,
                       const std::filesystem::path &cache_directory,
                       const std::filesystem::path &backup_directory,
                       const std::int64_t now_ms) noexcept {
  OperationsHealth result{
      .status_available = false,
      .status_age_seconds = std::nullopt,
      .operation_result = "unknown",
      .backup_age_seconds = std::nullopt,
      .backup_schema = std::nullopt,
      .backup_result = "unknown",
      .state_disk_warning =
          disk_warning(state_directory, minimum_state_free_bytes),
      .cache_disk_warning =
          disk_warning(cache_directory, minimum_state_free_bytes),
      .backup_disk_warning =
          disk_warning(backup_directory, minimum_backup_free_bytes),
  };
  try {
    const auto content = read_root_owned_file(status_file);
    if (!content)
      return result;
    const auto document = nlohmann::json::parse(*content);
    if (!document.is_object())
      return result;
    const auto operation = document.find("result");
    const auto backup = document.find("backup_result");
    if (operation == document.end() || !operation->is_string() ||
        backup == document.end() || !backup->is_string() ||
        !safe_result(operation->get_ref<const std::string &>()) ||
        !safe_result(backup->get_ref<const std::string &>()))
      return result;
    result.status_age_seconds = age_seconds(document, "updated_at_ms", now_ms);
    result.backup_age_seconds = age_seconds(document, "backup_at_ms", now_ms);
    const auto schema = document.find("backup_schema");
    if (schema != document.end() && schema->is_number_integer()) {
      const auto value = schema->get<std::int64_t>();
      if (value >= 0 && value <= 16)
        result.backup_schema = value;
    }
    if (!result.status_age_seconds)
      return result;
    result.operation_result = operation->get<std::string>();
    result.backup_result = backup->get<std::string>();
    result.status_available = true;
  } catch (...) {
  }
  return result;
}

} // namespace sanguinius
