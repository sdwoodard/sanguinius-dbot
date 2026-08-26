#include "sanguinius/ffmpeg_audio_normalizer.hpp"

#include <nlohmann/json.hpp>

#include <cerrno>
#include <chrono>
#include <cmath>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <limits>
#include <pthread.h>
#include <poll.h>
#include <spawn.h>
#include <stdexcept>
#include <string>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <utility>
#include <vector>

namespace sanguinius {
namespace {

constexpr std::size_t maximum_process_stderr_bytes = 16U * 1024U;

class FileDescriptor final {
public:
  explicit FileDescriptor(const int value = -1) noexcept : value_{value} {}
  ~FileDescriptor() { reset(); }
  FileDescriptor(const FileDescriptor &) = delete;
  FileDescriptor &operator=(const FileDescriptor &) = delete;
  FileDescriptor(FileDescriptor &&other) noexcept
      : value_{std::exchange(other.value_, -1)} {}
  FileDescriptor &operator=(FileDescriptor &&other) noexcept {
    if (this != &other) {
      reset();
      value_ = std::exchange(other.value_, -1);
    }
    return *this;
  }
  [[nodiscard]] int get() const noexcept { return value_; }
  [[nodiscard]] bool valid() const noexcept { return value_ >= 0; }
  int release() noexcept { return std::exchange(value_, -1); }
  void reset(const int replacement = -1) noexcept {
    if (value_ >= 0)
      static_cast<void>(::close(value_));
    value_ = replacement;
  }

private:
  int value_;
};

class ScopedSigpipeBlock final {
public:
  ScopedSigpipeBlock() {
    sigset_t blocked{};
    if (::sigemptyset(&blocked) != 0 || ::sigaddset(&blocked, SIGPIPE) != 0)
      throw TtsError{TtsFailureCategory::decoder_failed,
                     "Unable to prepare the audio decoder pipe signal mask."};
    if (::sigpending(&pending_before_) != 0)
      throw TtsError{TtsFailureCategory::decoder_failed,
                     "Unable to secure the audio decoder pipe signal mask."};
    if (::pthread_sigmask(SIG_BLOCK, &blocked, &previous_mask_) != 0)
      throw TtsError{TtsFailureCategory::decoder_failed,
                     "Unable to block the audio decoder pipe signal."};
    installed_ = true;
  }

  ~ScopedSigpipeBlock() {
    if (!installed_)
      return;
    sigset_t pending{};
    if (::sigpending(&pending) == 0 &&
        ::sigismember(&pending_before_, SIGPIPE) == 0 &&
        ::sigismember(&pending, SIGPIPE) == 1) {
      sigset_t pipe_signal{};
      static_cast<void>(::sigemptyset(&pipe_signal));
      static_cast<void>(::sigaddset(&pipe_signal, SIGPIPE));
      const timespec no_wait{};
      while (::sigtimedwait(&pipe_signal, nullptr, &no_wait) < 0 &&
             errno == EINTR) {
      }
    }
    static_cast<void>(::pthread_sigmask(SIG_SETMASK, &previous_mask_, nullptr));
  }

  ScopedSigpipeBlock(const ScopedSigpipeBlock &) = delete;
  ScopedSigpipeBlock &operator=(const ScopedSigpipeBlock &) = delete;

private:
  sigset_t previous_mask_{};
  sigset_t pending_before_{};
  bool installed_{};
};

struct PipePair {
  FileDescriptor read;
  FileDescriptor write;
};

[[nodiscard]] PipePair make_pipe() {
  int values[2]{};
  if (::pipe(values) != 0)
    throw TtsError{TtsFailureCategory::decoder_failed,
                   "Unable to create an audio decoder pipe."};
  for (const int value : values) {
    const auto flags = ::fcntl(value, F_GETFD);
    if (flags < 0 || ::fcntl(value, F_SETFD, flags | FD_CLOEXEC) != 0) {
      static_cast<void>(::close(values[0]));
      static_cast<void>(::close(values[1]));
      throw TtsError{TtsFailureCategory::decoder_failed,
                     "Unable to secure an audio decoder pipe."};
    }
  }
  return {FileDescriptor{values[0]}, FileDescriptor{values[1]}};
}

void set_nonblocking(const int descriptor) {
  const auto flags = ::fcntl(descriptor, F_GETFL);
  if (flags < 0 || ::fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) != 0)
    throw TtsError{TtsFailureCategory::decoder_failed,
                   "Unable to bound an audio decoder pipe."};
}

struct SpawnActions final {
  posix_spawn_file_actions_t value{};
  SpawnActions() {
    if (posix_spawn_file_actions_init(&value) != 0)
      throw TtsError{TtsFailureCategory::decoder_failed,
                     "Unable to initialize audio decoder actions."};
  }
  ~SpawnActions() { posix_spawn_file_actions_destroy(&value); }
  SpawnActions(const SpawnActions &) = delete;
  SpawnActions &operator=(const SpawnActions &) = delete;
};

struct SpawnAttributes final {
  posix_spawnattr_t value{};
  SpawnAttributes() {
    if (posix_spawnattr_init(&value) != 0)
      throw TtsError{TtsFailureCategory::decoder_failed,
                     "Unable to initialize audio decoder attributes."};
  }
  ~SpawnAttributes() { posix_spawnattr_destroy(&value); }
  SpawnAttributes(const SpawnAttributes &) = delete;
  SpawnAttributes &operator=(const SpawnAttributes &) = delete;
};

struct ProcessResult {
  std::vector<std::byte> output;
  std::string error;
};

void close_if_open(FileDescriptor &descriptor) noexcept { descriptor.reset(); }

void terminate_process(const pid_t process) noexcept {
  if (process <= 0)
    return;
  static_cast<void>(::kill(-process, SIGTERM));
  const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds{500};
  int status{};
  bool leader_reaped{};
  while (std::chrono::steady_clock::now() < deadline) {
    if (!leader_reaped) {
      const auto waited = ::waitpid(process, &status, WNOHANG);
      if (waited == process || (waited < 0 && errno == ECHILD))
        leader_reaped = true;
    }
    if (::kill(-process, 0) < 0 && errno == ESRCH)
      return;
    std::this_thread::sleep_for(std::chrono::milliseconds{10});
  }
  static_cast<void>(::kill(-process, SIGKILL));
  if (!leader_reaped)
    while (::waitpid(process, &status, 0) < 0 && errno == EINTR) {
    }
}

void append_bounded(std::vector<std::byte> &output, const char *data,
                    const std::size_t bytes, const std::size_t limit) {
  if (bytes > limit || output.size() > limit - bytes)
    throw TtsError{TtsFailureCategory::oversized_media,
                   "Decoded audio exceeded its output limit."};
  const auto *begin = reinterpret_cast<const std::byte *>(data);
  output.insert(output.end(), begin, begin + bytes);
}

void append_bounded(std::string &output, const char *data,
                    const std::size_t bytes, const std::size_t limit) {
  if (output.size() >= limit)
    return;
  output.append(data, std::min(bytes, limit - output.size()));
}

template <typename Output>
void drain_descriptor(FileDescriptor &descriptor, Output &output,
                      const std::size_t limit) {
  char buffer[8'192];
  while (descriptor.valid()) {
    const auto bytes = ::read(descriptor.get(), buffer, sizeof(buffer));
    if (bytes > 0) {
      append_bounded(output, buffer, static_cast<std::size_t>(bytes), limit);
      continue;
    }
    if (bytes == 0) {
      descriptor.reset();
      return;
    }
    if (errno == EINTR)
      continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK)
      return;
    descriptor.reset();
    return;
  }
}

[[nodiscard]] ProcessResult run_process(
    const std::filesystem::path &executable,
    const std::vector<std::string> &arguments,
    const std::span<const std::byte> input, const std::size_t maximum_output,
    const std::chrono::milliseconds timeout,
    const std::stop_token stop_token) {
  auto standard_input = make_pipe();
  auto standard_output = make_pipe();
  auto standard_error = make_pipe();
  SpawnActions actions;
  const auto add_dup = [&actions](const int from, const int to) {
    if (posix_spawn_file_actions_adddup2(&actions.value, from, to) != 0)
      throw TtsError{TtsFailureCategory::decoder_failed,
                     "Unable to configure audio decoder pipes."};
  };
  add_dup(standard_input.read.get(), STDIN_FILENO);
  add_dup(standard_output.write.get(), STDOUT_FILENO);
  add_dup(standard_error.write.get(), STDERR_FILENO);
  if (::posix_spawn_file_actions_addclosefrom_np(&actions.value,
                                                  STDERR_FILENO + 1) != 0)
    throw TtsError{TtsFailureCategory::decoder_failed,
                   "Unable to close inherited audio decoder descriptors."};
  SpawnAttributes attributes;
  sigset_t empty_mask{};
  sigset_t default_signals{};
  if (::sigemptyset(&empty_mask) != 0 ||
      ::sigemptyset(&default_signals) != 0 ||
      ::sigaddset(&default_signals, SIGINT) != 0 ||
      ::sigaddset(&default_signals, SIGTERM) != 0 ||
      ::sigaddset(&default_signals, SIGPIPE) != 0 ||
      posix_spawnattr_setsigmask(&attributes.value, &empty_mask) != 0 ||
      posix_spawnattr_setsigdefault(&attributes.value, &default_signals) != 0 ||
      posix_spawnattr_setflags(&attributes.value,
                              POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK |
                                  POSIX_SPAWN_SETSIGDEF) != 0 ||
      posix_spawnattr_setpgroup(&attributes.value, 0) != 0)
    throw TtsError{TtsFailureCategory::decoder_failed,
                   "Unable to isolate the audio decoder process."};

  std::vector<std::string> owned_arguments;
  owned_arguments.reserve(arguments.size() + 1);
  owned_arguments.push_back(executable.string());
  owned_arguments.insert(owned_arguments.end(), arguments.begin(),
                         arguments.end());
  std::vector<char *> argument_vector;
  argument_vector.reserve(owned_arguments.size() + 1);
  for (auto &argument : owned_arguments)
    argument_vector.push_back(argument.data());
  argument_vector.push_back(nullptr);
  std::string locale{"LC_ALL=C"};
  std::string path{"PATH=/usr/bin:/bin"};
  char *environment[]{locale.data(), path.data(), nullptr};
  pid_t process{};
  const auto spawned = posix_spawn(&process, executable.c_str(), &actions.value,
                                   &attributes.value, argument_vector.data(),
                                   environment);
  if (spawned != 0)
    throw TtsError{TtsFailureCategory::decoder_failed,
                   "Unable to start the configured audio decoder."};

  standard_input.read.reset();
  standard_output.write.reset();
  standard_error.write.reset();
  try {
    ScopedSigpipeBlock sigpipe_block;
    set_nonblocking(standard_input.write.get());
    set_nonblocking(standard_output.read.get());
    set_nonblocking(standard_error.read.get());
    ProcessResult result;
    std::size_t input_offset{};
    bool process_exited{};
    int process_status{};
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!process_exited || standard_output.read.valid() ||
           standard_error.read.valid()) {
      if (stop_token.stop_requested()) {
        terminate_process(process);
        throw TtsError{TtsFailureCategory::cancelled,
                       "Audio normalization was cancelled."};
      }
      if (std::chrono::steady_clock::now() >= deadline) {
        terminate_process(process);
        throw TtsError{TtsFailureCategory::timeout,
                       "Audio normalization timed out."};
      }
      std::vector<pollfd> descriptors;
      if (standard_input.write.valid())
        descriptors.push_back({standard_input.write.get(), POLLOUT, 0});
      if (standard_output.read.valid())
        descriptors.push_back({standard_output.read.get(), POLLIN, 0});
      if (standard_error.read.valid())
        descriptors.push_back({standard_error.read.get(), POLLIN, 0});
      const auto polled = ::poll(descriptors.data(), descriptors.size(), 25);
      if (polled < 0 && errno != EINTR)
        throw TtsError{TtsFailureCategory::decoder_failed,
                       "Audio decoder polling failed."};
      for (const auto &descriptor : descriptors) {
        if (standard_input.write.valid() &&
            descriptor.fd == standard_input.write.get() &&
            (descriptor.revents & (POLLOUT | POLLHUP | POLLERR)) != 0) {
          if (input_offset == input.size()) {
            standard_input.write.reset();
          } else {
            const auto remaining = input.size() - input_offset;
            const auto *data = reinterpret_cast<const char *>(input.data()) +
                               input_offset;
            const auto written = ::write(standard_input.write.get(), data,
                                         std::min<std::size_t>(remaining, 65'536));
            if (written > 0) {
              input_offset += static_cast<std::size_t>(written);
              if (input_offset == input.size())
                standard_input.write.reset();
            } else if (written < 0 && errno != EINTR && errno != EAGAIN &&
                       errno != EWOULDBLOCK) {
              standard_input.write.reset();
            }
          }
        }
        if (standard_output.read.valid() &&
            descriptor.fd == standard_output.read.get() &&
            (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0)
          drain_descriptor(standard_output.read, result.output, maximum_output);
        if (standard_error.read.valid() &&
            descriptor.fd == standard_error.read.get() &&
            (descriptor.revents & (POLLIN | POLLHUP | POLLERR)) != 0)
          drain_descriptor(standard_error.read, result.error,
                           maximum_process_stderr_bytes);
      }
      if (!process_exited) {
        const auto waited = ::waitpid(process, &process_status, WNOHANG);
        if (waited == process)
          process_exited = true;
        else if (waited < 0 && errno != EINTR)
          throw TtsError{TtsFailureCategory::decoder_failed,
                         "Unable to reap the audio decoder process."};
      }
    }
    standard_input.write.reset();
    if (!process_exited) {
      while (::waitpid(process, &process_status, 0) < 0 && errno == EINTR) {
      }
    }
    if (!WIFEXITED(process_status) || WEXITSTATUS(process_status) != 0)
      throw TtsError{TtsFailureCategory::decoder_failed,
                     "The configured audio decoder rejected provider media."};
    return result;
  } catch (...) {
    close_if_open(standard_input.write);
    close_if_open(standard_output.read);
    close_if_open(standard_error.read);
    terminate_process(process);
    throw;
  }
}

[[nodiscard]] std::optional<double>
parse_duration(const std::vector<std::byte> &bytes) {
  try {
    const std::string json{reinterpret_cast<const char *>(bytes.data()),
                           bytes.size()};
    const auto parsed = nlohmann::json::parse(json);
    if (!parsed.contains("streams") || !parsed.at("streams").is_array() ||
        parsed.at("streams").size() != 1 ||
        parsed.at("streams").front().value("codec_type", "") != "audio")
      throw std::runtime_error{"shape"};
    auto duration_text = parsed.at("streams").front().value("duration", "");
    if (duration_text.empty() && parsed.contains("format") &&
        parsed.at("format").is_object())
      duration_text = parsed.at("format").value("duration", "");
    if (!duration_text.empty() && duration_text != "N/A") {
      std::size_t consumed{};
      const auto duration = std::stod(duration_text, &consumed);
      if (consumed != duration_text.size() || !std::isfinite(duration) ||
          duration <= 0.0)
        throw std::runtime_error{"duration"};
      return std::optional{duration};
    }
    if (!parsed.contains("packets") || !parsed.at("packets").is_array() ||
        parsed.at("packets").empty())
      throw std::runtime_error{"packets"};
    double duration{};
    for (const auto &packet : parsed.at("packets")) {
      const auto packet_duration = packet.value("duration_time", "");
      if (packet_duration.empty() || packet_duration == "N/A")
        throw std::runtime_error{"packet duration"};
      std::size_t consumed{};
      const auto value = std::stod(packet_duration, &consumed);
      if (consumed != packet_duration.size() || !std::isfinite(value) ||
          value <= 0.0 || duration > maximum_tts_duration_ms / 1'000.0 + 1.0 -
                                      value)
        throw std::runtime_error{"packet duration"};
      duration += value;
    }
    return std::optional{duration};
  } catch (const std::exception &) {
    throw TtsError{TtsFailureCategory::invalid_media,
                   "FFprobe returned invalid audio metadata."};
  }
}

void validate_executable(const std::filesystem::path &path,
                         const std::string_view expected_name) {
  if (!path.is_absolute() || path.filename() != expected_name)
    throw std::invalid_argument{
        "Audio decoder paths must be absolute fixed executable names."};
}

} // namespace

void validate_ffmpeg_runtime(const std::filesystem::path &ffprobe_path,
                             const std::filesystem::path &ffmpeg_path,
                             const unsigned expected_major_version) {
  validate_executable(ffprobe_path, "ffprobe");
  validate_executable(ffmpeg_path, "ffmpeg");
  if (expected_major_version == 0)
    throw std::invalid_argument{"Expected FFmpeg major version is invalid."};
  const auto verify = [expected_major_version](
                          const std::filesystem::path &path,
                          const std::string_view program) {
    const auto result = run_process(path, {"-version"}, {}, 16U * 1024U,
                                    std::chrono::seconds{5}, {});
    const std::string output{
        reinterpret_cast<const char *>(result.output.data()),
        result.output.size()};
    const auto prefix = std::string{program} + " version ";
    if (!output.starts_with(prefix))
      throw std::runtime_error{"Configured " + std::string{program} +
                               " did not report a supported version."};
    auto version = std::string_view{output}.substr(prefix.size());
    if (version.starts_with('n'))
      version.remove_prefix(1);
    const auto expected = std::to_string(expected_major_version) + ".";
    if (!version.starts_with(expected))
      throw std::runtime_error{"Configured " + std::string{program} +
                               " is not the tested major version " +
                               std::to_string(expected_major_version) + "."};
  };
  verify(ffprobe_path, "ffprobe");
  verify(ffmpeg_path, "ffmpeg");
}

FfmpegAudioNormalizer::FfmpegAudioNormalizer(
    std::filesystem::path ffprobe_path, std::filesystem::path ffmpeg_path)
    : ffprobe_path_{std::move(ffprobe_path)},
      ffmpeg_path_{std::move(ffmpeg_path)} {
  validate_executable(ffprobe_path_, "ffprobe");
  validate_executable(ffmpeg_path_, "ffmpeg");
}

NormalizedAudio FfmpegAudioNormalizer::normalize(
    const SynthesizedAudio &audio, const AudioNormalizationLimits &limits,
    const std::stop_token stop_token) const {
  if (audio.format != AudioFormat::wav ||
      !wav_media_signature(audio.bytes) || audio.bytes.empty() ||
      audio.bytes.size() > maximum_tts_encoded_bytes)
    throw TtsError{TtsFailureCategory::invalid_media,
                   "Only bounded RIFF/WAVE provider media can be normalized."};
  if (limits.maximum_duration_ms <= 0 ||
      limits.maximum_duration_ms > maximum_tts_duration_ms ||
      limits.maximum_output_bytes == 0 ||
      limits.maximum_output_bytes > maximum_tts_pcm_bytes)
    throw TtsError{TtsFailureCategory::invalid_request,
                   "Audio normalization limits are invalid."};

  const auto probe = run_process(
      ffprobe_path_,
      {"-v", "error", "-show_entries",
       "stream=index,codec_type,duration:format=duration:packet=duration_time",
       "-of", "json", "-f", "wav", "pipe:0"},
      audio.bytes, 512U * 1024U, limits.probe_timeout, stop_token);
  const auto probed_duration = parse_duration(probe.output);
  if (!probed_duration)
    throw TtsError{TtsFailureCategory::invalid_media,
                   "FFprobe did not report a finite audio duration."};
  if (*probed_duration * 1'000.0 >
      static_cast<double>(limits.maximum_duration_ms) + 0.5)
    throw TtsError{TtsFailureCategory::invalid_media,
                   "Provider audio exceeds the maximum speech duration."};

  const auto decoded = run_process(
      ffmpeg_path_,
      {"-nostdin", "-v", "error", "-threads", "1", "-f", "wav", "-i",
       "pipe:0", "-map", "0:a:0", "-vn", "-sn", "-dn", "-ac", "2",
       "-ar", "48000", "-sample_fmt", "s16", "-f", "s16le", "pipe:1"},
      audio.bytes, limits.maximum_output_bytes, limits.decode_timeout,
      stop_token);
  if (decoded.output.empty() || decoded.output.size() % 4 != 0)
    throw TtsError{TtsFailureCategory::invalid_media,
                   "Decoded PCM is empty or not stereo-frame aligned."};
  const auto frames = decoded.output.size() / 4;
  const auto duration_ms = static_cast<std::int64_t>(
      (frames * std::size_t{1'000}) / std::size_t{48'000});
  if (duration_ms <= 0 || duration_ms > limits.maximum_duration_ms)
    throw TtsError{TtsFailureCategory::invalid_media,
                   "Decoded PCM exceeds the maximum speech duration."};
  PcmAudio pcm;
  pcm.samples.reserve(decoded.output.size() / 2);
  for (std::size_t offset = 0; offset < decoded.output.size(); offset += 2) {
    const auto low = std::to_integer<std::uint16_t>(decoded.output[offset]);
    const auto high =
        std::to_integer<std::uint16_t>(decoded.output[offset + 1]);
    pcm.samples.push_back(static_cast<std::int16_t>(low | (high << 8U)));
  }
  return {.pcm = std::move(pcm), .duration_ms = duration_ms};
}

} // namespace sanguinius
