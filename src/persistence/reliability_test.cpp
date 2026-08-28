#include "sanguinius/reliability_test.hpp"

#include "sanguinius/ai_generation.hpp"
#include "sanguinius/ai_work_service.hpp"
#include "sanguinius/outbox.hpp"
#include "sanguinius/persistence/database.hpp"
#include "sanguinius/persistence/migrator.hpp"
#include "sanguinius/persistence/sqlite_ai_generation_repository.hpp"
#include "sanguinius/persistence/sqlite_durable_work_repository.hpp"
#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistent_id.hpp"

#include <array>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <system_error>
#include <unistd.h>
#include <utility>
#include <vector>

namespace sanguinius {
namespace {

using namespace std::chrono_literals;

class TemporaryProbeDatabase {
public:
  TemporaryProbeDatabase() {
    auto pattern = (std::filesystem::temp_directory_path() /
                    "sanguinius-reliability-XXXXXX")
                       .string();
    std::vector<char> mutable_pattern(pattern.begin(), pattern.end());
    mutable_pattern.push_back('\0');
    const auto descriptor = ::mkstemp(mutable_pattern.data());
    if (descriptor < 0)
      throw std::runtime_error{"Could not create reliability probe state."};
    static_cast<void>(::close(descriptor));
    path_ = mutable_pattern.data();
  }

  ~TemporaryProbeDatabase() {
    std::error_code ignored;
    for (const auto suffix : {"", "-wal", "-shm", "-journal", ".lock"})
      std::filesystem::remove(path_.string() + suffix, ignored);
  }

  TemporaryProbeDatabase(const TemporaryProbeDatabase &) = delete;
  TemporaryProbeDatabase &operator=(const TemporaryProbeDatabase &) = delete;

  [[nodiscard]] const std::filesystem::path &path() const noexcept {
    return path_;
  }

private:
  std::filesystem::path path_;
};

struct ProbePersistence {
  ProbePersistence() {
    {
      auto database = persistence::Database::open_migration(path.path(), 500ms);
      const persistence::Migrator migrator{
          persistence::production_migrations(),
          BuildInfo{.version = "reliability-probe",
                    .revision = "isolated",
                    .release_id = "reliability-probe"},
          clock};
      if (migrator.apply(database.connection()).current_version != 16)
        throw std::runtime_error{"Reliability probe migration failed."};
    }
    context = std::make_shared<persistence::SqliteRepositoryContext>(
        persistence::Database::open_runtime(path.path(), 500ms));
    persistence::SqliteCoreIdentityRepository identities{context};
    identities.initialize_or_validate_scope({10, 20, 30}, 1);
    identities.ensure_user({30, "Owner", "owner", false, 1});
  }

  TemporaryProbeDatabase path;
  SystemClock clock;
  std::shared_ptr<persistence::SqliteRepositoryContext> context;
};

class TimeoutProbeClient final : public AiClient {
public:
  explicit TimeoutProbeClient(const bool submitted) : submitted_{submitted} {}

  [[nodiscard]] AiResult generate(
      const AiRequest &, std::stop_token,
      const std::function<void()> &transmission_started = {}) const override {
    if (submitted_ && transmission_started)
      transmission_started();
    throw AiProviderError{AiProviderErrorCategory::timeout, {}};
  }

private:
  bool submitted_{};
};

[[nodiscard]] AiRequest timeout_request(std::string idempotency_key) {
  return {.instructions = "isolated reliability probe",
          .conversation = {},
          .max_output_tokens = 10,
          .json_schema = std::nullopt,
          .purpose = AiPurpose::direct,
          .priority = AiPriority::direct,
          .requester_user_id = std::string{"30"},
          .idempotency_key = std::move(idempotency_key)};
}

void expect_timeout(AiGenerationService &service, const AiRequest &request) {
  try {
    static_cast<void>(service.generate(request, {}));
  } catch (const AiProviderError &error) {
    if (error.category() == AiProviderErrorCategory::timeout)
      return;
    throw;
  }
  throw std::runtime_error{"Reliability timeout was not observed."};
}

void run_text_timeout_probe() {
  ProbePersistence probe;
  UuidV4Generator ids;
  const AiGenerationPolicy policy{.input_rate_micro_usd_per_million_tokens = 1,
                                  .output_rate_micro_usd_per_million_tokens = 1,
                                  .model = "isolated-reliability-probe"};
  {
    AiGenerationService service{
        std::make_unique<TimeoutProbeClient>(false),
        std::make_unique<persistence::SqliteAiGenerationRepository>(
            probe.context),
        probe.clock,
        ids,
        {10, 20, 30},
        policy};
    expect_timeout(service, timeout_request("reliability:timeout:unsent"));
  }
  {
    AiGenerationService service{
        std::make_unique<TimeoutProbeClient>(true),
        std::make_unique<persistence::SqliteAiGenerationRepository>(
            probe.context),
        probe.clock,
        ids,
        {10, 20, 30},
        policy};
    expect_timeout(service, timeout_request("reliability:timeout:submitted"));
  }

  auto query = probe.context->connection().prepare(
      "SELECT idempotency_key,state,result_code,provider_sent FROM "
      "ai_generation_attempt WHERE idempotency_key LIKE "
      "'reliability:timeout:%' ORDER BY idempotency_key");
  std::array<std::string, 2> states;
  std::array<std::string, 2> results;
  std::array<std::int64_t, 2> sent{};
  std::size_t count{};
  while (query.step() && count < states.size()) {
    states[count] = query.column_text(1);
    results[count] = query.column_text(2);
    sent[count] = query.column_int64(3);
    ++count;
  }
  if (count != 2 || states[0] != "failed" || results[0] != "timeout" ||
      sent[0] != 1 || states[1] != "cancelled" ||
      results[1] != "provider_not_sent" || sent[1] != 0) {
    throw std::runtime_error{"Text timeout accounting probe failed."};
  }
}

void run_ai_saturation_probe() {
  AiWorkService work{1, 1};
  std::mutex mutex;
  std::condition_variable_any changed;
  bool active{};
  bool release{};
  work.start();
  const auto first = work.submit([&](const std::stop_token stop_token) {
    std::unique_lock lock{mutex};
    active = true;
    changed.notify_all();
    changed.wait_for(lock, stop_token, 500ms, [&] { return release; });
  });
  {
    std::unique_lock lock{mutex};
    if (!changed.wait_for(lock, 500ms, [&] { return active; })) {
      work.stop();
      throw std::runtime_error{"AI saturation probe did not start."};
    }
  }
  const auto queued = work.submit([](std::stop_token) {});
  const auto rejected = work.submit([](std::stop_token) {});
  {
    const std::scoped_lock lock{mutex};
    release = true;
  }
  changed.notify_all();
  work.stop();
  if (first != SubmitResult::accepted || queued != SubmitResult::accepted ||
      rejected != SubmitResult::full)
    throw std::runtime_error{"AI saturation admission probe failed."};
}

void run_discord_unknown_probe() {
  ProbePersistence probe;
  persistence::SqliteDurableWorkRepository repository{probe.context};
  constexpr std::string_view event_id{"00000000-0000-4000-8000-000000000701"};
  constexpr std::string_view outbox_id{"00000000-0000-4000-8000-000000000702"};
  const EventJournalEntry event{.event_id = std::string{event_id},
                                .event_type = "owner.reliability_probe.v1",
                                .aggregate_type = "reliability_probe",
                                .aggregate_id = "discord_unknown",
                                .actor_user_id = DiscordSnowflake{30},
                                .guild_id = DiscordSnowflake{10},
                                .channel_id = DiscordSnowflake{20},
                                .source_message_id = std::nullopt,
                                .occurred_at_ms = 1'000,
                                .recorded_at_ms = 1'000,
                                .correlation_id = "reliability-probe",
                                .causation_id = std::nullopt,
                                .idempotency_key =
                                    "event:reliability:discord-unknown",
                                .payload_json = "{}"};
  const OutboxEnqueue outbox{
      .outbox_id = std::string{outbox_id},
      .kind = std::string{public_discord_outbox_kind},
      .aggregate_type = "reliability_probe",
      .aggregate_id = "discord_unknown",
      .target_guild_id = DiscordSnowflake{10},
      .target_channel_id = DiscordSnowflake{20},
      .target_user_id = std::nullopt,
      .available_at_ms = 1'000,
      .max_attempts = 5,
      .idempotency_key = "outbox:reliability:discord-unknown",
      .provider_nonce = discord_nonce_from_uuid(outbox_id),
      .created_at_ms = 1'000};
  const PublicOutboxPayload payload{
      .request = {.guild_id = DiscordSnowflake{10},
                  .channel_id = DiscordSnowflake{20},
                  .message = text_message("Isolated reliability probe.")},
      .fail_before_first_send = false};
  if (!repository.enqueue_public(event, outbox, payload))
    throw std::runtime_error{"Reliability outbox enqueue failed."};

  auto first =
      repository.claim_due_outbox(1'000, 61'000, "reliability-instance",
                                  "00000000-0000-4000-8000-000000000703", true);
  if (!first.has_value() || repository.mark_public_outbox_submitted(
                                *first,
                                {.wall_time_ms = 1'000,
                                 .elapsed_realtime_ms = 1'000,
                                 .boot_session_id = "reliability-boot"},
                                61'000) != WorkMutationStatus::applied)
    throw std::runtime_error{"Reliability outbox submission failed."};
  const auto retry =
      classify_discord_delivery_failure(DeliveryResult::unknown_outcome, true);
  if (!retry.retry || repository.fail_outbox(
                          *first, 1'000, 6'000, std::string{retry.error_code},
                          retry.mode) != WorkMutationStatus::applied)
    throw std::runtime_error{"Reliability outbox retry failed."};

  auto second =
      repository.claim_due_outbox(92'000, 152'000, "reliability-instance",
                                  "00000000-0000-4000-8000-000000000704", true);
  const auto quarantine =
      classify_discord_delivery_failure(DeliveryResult::unknown_outcome, false);
  if (!second.has_value() || quarantine.retry ||
      second->first_attempt_elapsed_ms != std::optional<std::int64_t>{1'000} ||
      repository.fail_outbox(*second, 92'000, 92'000,
                             std::string{quarantine.error_code},
                             quarantine.mode) != WorkMutationStatus::applied)
    throw std::runtime_error{"Discord unknown-outcome quarantine failed."};
  const auto health = repository.health(92'000);
  const auto dead = repository.dead(5);
  if (health.failed_outbox != 1 || dead.size() != 1 ||
      dead.front().state != "failed" ||
      dead.front().error_code !=
          std::optional<std::string>{"discord_unknown_outcome_stale"})
    throw std::runtime_error{"Discord quarantine state was not durable."};
}

class IsolatedReliabilityTestService final : public ReliabilityTestService {
public:
  [[nodiscard]] std::string
  run(const std::string_view scenario) const override {
    if (scenario == "text-timeout") {
      run_text_timeout_probe();
      return "Reliability probe passed: isolated SQLite reservation and "
             "submission timeout accounting completed without provider "
             "traffic.";
    }
    if (scenario == "ai-saturation") {
      run_ai_saturation_probe();
      return "Reliability probe passed: the bounded AI queue rejected excess "
             "work without provider traffic.";
    }
    if (scenario == "discord-unknown") {
      run_discord_unknown_probe();
      return "Reliability probe passed: an isolated durable outbox retried "
             "inside the nonce window and quarantined the stale unknown "
             "outcome without Discord traffic.";
    }
    throw std::invalid_argument{"Unknown reliability scenario."};
  }
};

} // namespace

std::unique_ptr<ReliabilityTestService>
make_isolated_reliability_test_service() {
  return std::make_unique<IsolatedReliabilityTestService>();
}

} // namespace sanguinius
