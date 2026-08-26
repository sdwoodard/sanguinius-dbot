#include "sanguinius/tts_cache.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <fcntl.h>
#include <limits>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace sanguinius {
namespace {

std::atomic<std::uint64_t> temporary_sequence{};
constexpr std::string_view ownership_filename{
    ".sanguinius-tts-cache-v1"};
constexpr std::string_view ownership_contents{
    "sanguinius dedicated TTS cache v1\n"};
constexpr std::string_view temporary_prefix{
    ".sanguinius-tts-tmp-"};

[[nodiscard]] bool valid_key(const std::string_view key) noexcept {
  return key.size() == 64 &&
         std::all_of(key.begin(), key.end(), [](const unsigned char character) {
           return (character >= '0' && character <= '9') ||
                  (character >= 'a' && character <= 'f');
         });
}

[[nodiscard]] std::string filename(const std::string_view key) {
  if (!valid_key(key))
    throw TtsError{TtsFailureCategory::cache_failed,
                   "TTS cache key is invalid."};
  return std::string{key} + ".pcm";
}

[[nodiscard]] bool valid_temporary_name(const std::string_view name) noexcept {
  if (!name.starts_with(temporary_prefix))
    return false;
  const auto suffix = name.substr(temporary_prefix.size());
  const auto separator = suffix.find('-');
  if (separator == std::string_view::npos || separator == 0 ||
      separator + 1 == suffix.size())
    return false;
  const auto digits = [](const std::string_view value) {
    return std::all_of(value.begin(), value.end(), [](const char character) {
      return character >= '0' && character <= '9';
    });
  };
  return digits(suffix.substr(0, separator)) &&
         digits(suffix.substr(separator + 1));
}

[[nodiscard]] std::vector<std::byte> pcm_bytes(const PcmAudio &audio) {
  if (audio.sample_rate != 48'000 || audio.channels != 2 ||
      audio.bits_per_sample != 16 || audio.samples.empty() ||
      audio.samples.size() * sizeof(std::int16_t) > maximum_tts_pcm_bytes ||
      audio.samples.size() % 2 != 0)
    throw TtsError{TtsFailureCategory::cache_failed,
                   "Only bounded D++ PCM can be cached."};
  std::vector<std::byte> result;
  result.reserve(audio.samples.size() * 2);
  for (const auto sample : audio.samples) {
    const auto bits = static_cast<std::uint16_t>(sample);
    result.push_back(static_cast<std::byte>(bits & 0xFFU));
    result.push_back(static_cast<std::byte>((bits >> 8U) & 0xFFU));
  }
  return result;
}

[[nodiscard]] PcmAudio decode_pcm(const std::span<const std::byte> bytes) {
  if (bytes.empty() || bytes.size() > maximum_tts_pcm_bytes ||
      bytes.size() % 4 != 0)
    throw TtsError{TtsFailureCategory::cache_failed,
                   "Cached PCM has an invalid size."};
  PcmAudio audio;
  audio.samples.reserve(bytes.size() / 2);
  for (std::size_t offset = 0; offset < bytes.size(); offset += 2) {
    const auto low = std::to_integer<std::uint16_t>(bytes[offset]);
    const auto high = std::to_integer<std::uint16_t>(bytes[offset + 1]);
    audio.samples.push_back(static_cast<std::int16_t>(low | (high << 8U)));
  }
  return audio;
}

void write_all(const int descriptor, const std::span<const std::byte> bytes) {
  std::size_t offset{};
  while (offset < bytes.size()) {
    const auto written = ::write(
        descriptor, reinterpret_cast<const char *>(bytes.data()) + offset,
        bytes.size() - offset);
    if (written > 0) {
      offset += static_cast<std::size_t>(written);
      continue;
    }
    if (written < 0 && errno == EINTR)
      continue;
    throw TtsError{TtsFailureCategory::cache_failed,
                   "Unable to write a TTS cache entry."};
  }
}

[[nodiscard]] std::vector<std::byte> read_all(const int descriptor,
                                              const std::size_t size) {
  std::vector<std::byte> bytes(size);
  std::size_t offset{};
  while (offset < size) {
    const auto received = ::read(
        descriptor, reinterpret_cast<char *>(bytes.data()) + offset,
        size - offset);
    if (received > 0) {
      offset += static_cast<std::size_t>(received);
      continue;
    }
    if (received < 0 && errno == EINTR)
      continue;
    throw TtsError{TtsFailureCategory::cache_failed,
                   "Unable to read a complete TTS cache entry."};
  }
  return bytes;
}

[[nodiscard]] std::int64_t timespec_nanoseconds(const timespec value) noexcept {
  constexpr std::int64_t nanoseconds_per_second = 1'000'000'000;
  if (value.tv_sec < 0 ||
      value.tv_sec > std::numeric_limits<std::int64_t>::max() /
                         nanoseconds_per_second)
    return 0;
  return static_cast<std::int64_t>(value.tv_sec) * nanoseconds_per_second +
         value.tv_nsec;
}

void touch_access_time(const int descriptor, const struct stat &status) noexcept {
  timespec now{};
  if (::clock_gettime(CLOCK_REALTIME, &now) != 0)
    return;
  const timespec times[]{now, status.st_mtim};
  static_cast<void>(::futimens(descriptor, times));
}

} // namespace

FilesystemTtsCache::FilesystemTtsCache(std::filesystem::path root,
                                       const TtsCachePolicy policy)
    : root_{std::move(root).lexically_normal()}, policy_{policy} {
  if (!root_.is_absolute() || policy_.maximum_bytes == 0 ||
      policy_.maximum_bytes > 128U * 1024U * 1024U ||
      policy_.maximum_age <= std::chrono::hours{0} ||
      policy_.maximum_age > std::chrono::hours{24 * 30})
    throw std::invalid_argument{"TTS cache configuration is invalid."};
  std::error_code error;
  std::filesystem::create_directories(root_, error);
  struct stat root_status {};
  if (error || ::lstat(root_.c_str(), &root_status) != 0 ||
      !S_ISDIR(root_status.st_mode) ||
      root_status.st_uid != ::geteuid())
    throw TtsError{TtsFailureCategory::cache_failed,
                   "Unable to create a secure TTS cache directory."};
  std::filesystem::permissions(
      root_, std::filesystem::perms::owner_all,
      std::filesystem::perm_options::replace, error);
  if (error)
    throw TtsError{TtsFailureCategory::cache_failed,
                   "Unable to secure the TTS cache directory."};

  const auto ownership_path = root_ / ownership_filename;
  struct stat ownership_status {};
  if (::lstat(ownership_path.c_str(), &ownership_status) != 0) {
    if (errno != ENOENT)
      throw TtsError{TtsFailureCategory::cache_failed,
                     "Unable to inspect TTS cache ownership."};
    for (std::filesystem::directory_iterator iterator{root_, error}, end;
         !error && iterator != end; iterator.increment(error))
      throw TtsError{
          TtsFailureCategory::cache_failed,
          "Refusing to claim a nonempty directory as the TTS cache."};
    if (error)
      throw TtsError{TtsFailureCategory::cache_failed,
                     "Unable to inspect TTS cache ownership."};
    const int descriptor = ::open(ownership_path.c_str(),
                                  O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC |
                                      O_NOFOLLOW,
                                  S_IRUSR | S_IWUSR);
    if (descriptor < 0)
      throw TtsError{TtsFailureCategory::cache_failed,
                     "Unable to claim the TTS cache directory."};
    try {
      write_all(descriptor,
                std::as_bytes(std::span{ownership_contents.data(),
                                        ownership_contents.size()}));
      if (::fsync(descriptor) != 0 || ::close(descriptor) != 0)
        throw TtsError{TtsFailureCategory::cache_failed,
                       "Unable to commit TTS cache ownership."};
      const int directory = ::open(
          root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
      if (directory < 0 || ::fsync(directory) != 0) {
        if (directory >= 0)
          static_cast<void>(::close(directory));
        throw TtsError{TtsFailureCategory::cache_failed,
                       "Unable to commit TTS cache ownership."};
      }
      static_cast<void>(::close(directory));
    } catch (...) {
      static_cast<void>(::close(descriptor));
      static_cast<void>(::unlink(ownership_path.c_str()));
      throw;
    }
    if (::lstat(ownership_path.c_str(), &ownership_status) != 0)
      throw TtsError{TtsFailureCategory::cache_failed,
                     "Unable to verify TTS cache ownership."};
  }
  if (!S_ISREG(ownership_status.st_mode) ||
      ownership_status.st_uid != ::geteuid() ||
      (ownership_status.st_mode & (S_IRWXG | S_IRWXO)) != 0 ||
      ownership_status.st_size !=
          static_cast<off_t>(ownership_contents.size()))
    throw TtsError{TtsFailureCategory::cache_failed,
                   "The TTS cache ownership marker is invalid."};
  const int marker =
      ::open(ownership_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (marker < 0)
    throw TtsError{TtsFailureCategory::cache_failed,
                   "Unable to verify TTS cache ownership."};
  std::vector<std::byte> marker_bytes;
  try {
    marker_bytes = read_all(marker, ownership_contents.size());
  } catch (...) {
    static_cast<void>(::close(marker));
    throw;
  }
  static_cast<void>(::close(marker));
  const auto expected = std::as_bytes(std::span{ownership_contents.data(),
                                                ownership_contents.size()});
  if (!std::equal(marker_bytes.begin(), marker_bytes.end(), expected.begin(),
                  expected.end()))
    throw TtsError{TtsFailureCategory::cache_failed,
                   "The TTS cache ownership marker is invalid."};
}

std::optional<PcmAudio>
FilesystemTtsCache::read(const std::string_view key,
                         const std::string_view expected_checksum) {
  const auto name = filename(key);
  if (!valid_key(expected_checksum))
    throw TtsError{TtsFailureCategory::cache_failed,
                   "TTS cache checksum is invalid."};
  const std::scoped_lock lock{mutex_};
  const auto path = root_ / name;
  const int descriptor = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (descriptor < 0) {
    struct stat link_status {};
    if (::lstat(path.c_str(), &link_status) == 0 &&
        S_ISLNK(link_status.st_mode))
      static_cast<void>(::unlink(path.c_str()));
    ++health_.misses;
    return std::nullopt;
  }
  struct stat status {};
  if (::fstat(descriptor, &status) != 0 || !S_ISREG(status.st_mode) ||
      status.st_size <= 0 ||
      static_cast<std::uintmax_t>(status.st_size) > maximum_tts_pcm_bytes ||
      status.st_size % 4 != 0) {
    static_cast<void>(::close(descriptor));
    ++health_.corruptions;
    ++health_.misses;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return std::nullopt;
  }
  try {
    auto bytes = read_all(descriptor, static_cast<std::size_t>(status.st_size));
    if (sha256_hex(bytes) != expected_checksum) {
      static_cast<void>(::close(descriptor));
      ++health_.corruptions;
      ++health_.misses;
      std::error_code ignored;
      std::filesystem::remove(path, ignored);
      return std::nullopt;
    }
    touch_access_time(descriptor, status);
    static_cast<void>(::close(descriptor));
    ++health_.hits;
    return decode_pcm(bytes);
  } catch (...) {
    static_cast<void>(::close(descriptor));
    ++health_.corruptions;
    ++health_.misses;
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    return std::nullopt;
  }
}

TtsCacheMutationResult FilesystemTtsCache::write(const std::string_view key,
                                                 const PcmAudio &audio) {
  const auto name = filename(key);
  const auto bytes = pcm_bytes(audio);
  if (bytes.size() > policy_.maximum_bytes)
    throw TtsError{TtsFailureCategory::cache_failed,
                   "A normalized line exceeds the configured cache limit."};
  const auto temporary = std::string{temporary_prefix} +
                         std::to_string(::getpid()) + "-" +
                         std::to_string(++temporary_sequence);
  const std::scoped_lock lock{mutex_};
  const auto temporary_path = root_ / temporary;
  const auto final_path = root_ / name;
  int descriptor = ::open(temporary_path.c_str(),
                          O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                          S_IRUSR | S_IWUSR);
  if (descriptor < 0)
    throw TtsError{TtsFailureCategory::cache_failed,
                   "Unable to create a TTS cache entry."};
  try {
    write_all(descriptor, bytes);
    if (::fsync(descriptor) != 0 || ::close(descriptor) != 0)
      throw TtsError{TtsFailureCategory::cache_failed,
                     "Unable to commit a TTS cache entry."};
    descriptor = -1;
    if (::rename(temporary_path.c_str(), final_path.c_str()) != 0)
      throw TtsError{TtsFailureCategory::cache_failed,
                     "Unable to publish a TTS cache entry."};
    const int directory =
        ::open(root_.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (directory < 0 || ::fsync(directory) != 0) {
      if (directory >= 0)
        static_cast<void>(::close(directory));
      throw TtsError{TtsFailureCategory::cache_failed,
                     "Unable to commit the TTS cache directory."};
    }
    static_cast<void>(::close(directory));
    return purge_locked(key);
  } catch (...) {
    if (descriptor >= 0)
      static_cast<void>(::close(descriptor));
    std::error_code ignored;
    std::filesystem::remove(temporary_path, ignored);
    throw;
  }
}

void FilesystemTtsCache::record_miss() noexcept {
  const std::scoped_lock lock{mutex_};
  ++health_.misses;
}

void FilesystemTtsCache::erase(const std::string_view key) noexcept {
  try {
    const auto name = filename(key);
    const std::scoped_lock lock{mutex_};
    const auto path = root_ / name;
    struct stat status {};
    const bool existed = ::lstat(path.c_str(), &status) == 0;
    if (::unlink(path.c_str()) == 0 && existed) {
      if (health_.entries > 0)
        --health_.entries;
      if (S_ISREG(status.st_mode) && status.st_size > 0 &&
          health_.bytes >= static_cast<std::uintmax_t>(status.st_size))
        health_.bytes -= static_cast<std::uintmax_t>(status.st_size);
    }
  } catch (...) {
  }
}

TtsCacheMutationResult FilesystemTtsCache::purge() {
  const std::scoped_lock lock{mutex_};
  return purge_locked();
}

TtsCacheMutationResult
FilesystemTtsCache::purge_locked(const std::string_view protected_key) {
  struct Candidate {
    std::filesystem::path path;
    std::string key;
    std::int64_t created_ns{};
    std::int64_t accessed_ns{};
    std::uintmax_t size{};
    bool protected_entry{};
  };
  TtsCacheMutationResult result;
  std::vector<Candidate> candidates;
  std::uintmax_t total{};
  const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  const auto maximum_age =
      std::chrono::duration_cast<std::chrono::nanoseconds>(policy_.maximum_age)
          .count();
  std::error_code error;
  for (std::filesystem::directory_iterator iterator{root_, error}, end;
       !error && iterator != end; iterator.increment(error)) {
    const auto &entry = *iterator;
    const auto name = entry.path().filename().string();
    const bool named_entry =
        name.size() == 68 && name.ends_with(".pcm") &&
        valid_key(std::string_view{name}.substr(0, 64));
    if (name == ownership_filename)
      continue;
    const bool temporary_entry = valid_temporary_name(name);
    if (!temporary_entry && !named_entry)
      throw TtsError{TtsFailureCategory::cache_failed,
                     "The TTS cache contains an unowned entry."};
    struct stat status {};
    if (::lstat(entry.path().c_str(), &status) != 0 ||
        !S_ISREG(status.st_mode) || status.st_size <= 0 ||
        static_cast<std::uintmax_t>(status.st_size) > maximum_tts_pcm_bytes ||
        status.st_size % 4 != 0 || temporary_entry) {
      std::filesystem::remove(entry.path(), error);
      if (named_entry)
        result.removed_keys.push_back(name.substr(0, 64));
      error.clear();
      continue;
    }
    const auto created_ns = timespec_nanoseconds(status.st_mtim);
    const auto accessed_ns = timespec_nanoseconds(status.st_atim);
    const auto size = static_cast<std::uintmax_t>(status.st_size);
    if (created_ns == 0 || created_ns <= now - maximum_age) {
      std::filesystem::remove(entry.path(), error);
      if (!error)
        result.removed_keys.push_back(name.substr(0, 64));
      error.clear();
      continue;
    }
    total += size;
    const auto key = name.substr(0, 64);
    candidates.push_back({entry.path(), key, created_ns, accessed_ns, size,
                          key == protected_key});
  }
  if (error)
    throw TtsError{TtsFailureCategory::cache_failed,
                   "Unable to enumerate the TTS cache directory."};
  std::sort(candidates.begin(), candidates.end(),
            [](const Candidate &left, const Candidate &right) {
              if (left.protected_entry != right.protected_entry)
                return !left.protected_entry;
              if (left.accessed_ns != right.accessed_ns)
                return left.accessed_ns < right.accessed_ns;
              if (left.created_ns != right.created_ns)
                return left.created_ns < right.created_ns;
              return left.key < right.key;
            });
  for (const auto &candidate : candidates) {
    if (total <= policy_.maximum_bytes)
      break;
    std::filesystem::remove(candidate.path, error);
    if (!error) {
      total -= candidate.size;
      result.removed_keys.push_back(candidate.key);
    }
    error.clear();
  }
  health_.entries = 0;
  health_.bytes = 0;
  for (const auto &candidate : candidates) {
    if (std::filesystem::exists(candidate.path, error) && !error) {
      ++health_.entries;
      health_.bytes += candidate.size;
    }
    error.clear();
  }
  if (health_.bytes > policy_.maximum_bytes)
    throw TtsError{TtsFailureCategory::cache_failed,
                   "Unable to enforce the configured TTS cache limit."};
  std::sort(result.removed_keys.begin(), result.removed_keys.end());
  result.removed_keys.erase(
      std::unique(result.removed_keys.begin(), result.removed_keys.end()),
      result.removed_keys.end());
  return result;
}

std::vector<std::string> FilesystemTtsCache::keys() const {
  const std::scoped_lock lock{mutex_};
  std::vector<std::string> result;
  std::error_code error;
  for (std::filesystem::directory_iterator iterator{root_, error}, end;
       !error && iterator != end; iterator.increment(error)) {
    const auto name = iterator->path().filename().string();
    if (name.size() == 68 && name.ends_with(".pcm") &&
        valid_key(std::string_view{name}.substr(0, 64)))
      result.push_back(name.substr(0, 64));
  }
  if (error)
    throw TtsError{TtsFailureCategory::cache_failed,
                   "Unable to enumerate the TTS cache directory."};
  std::sort(result.begin(), result.end());
  return result;
}

TtsCacheHealth FilesystemTtsCache::health() const {
  const std::scoped_lock lock{mutex_};
  return health_;
}

} // namespace sanguinius
