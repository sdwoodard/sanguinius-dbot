#include "sanguinius/persistence/sqlite_safety_control_repository.hpp"

#include "sanguinius/persistence/sqlite_repositories.hpp"
#include "sanguinius/persistence/transaction.hpp"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace sanguinius::persistence {

SqliteRuntimeFeatureControlRepository::SqliteRuntimeFeatureControlRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"SQLite repository context is required."};
}

std::vector<RuntimeFeatureControl>
SqliteRuntimeFeatureControlRepository::snapshot() {
  std::scoped_lock lock{context_->mutex()};
  auto query = context_->connection().prepare(
      "SELECT feature,disabled,revision,changed_at_ms FROM "
      "runtime_feature_control ORDER BY feature");
  std::vector<RuntimeFeatureControl> result;
  while (query.step()) {
    result.push_back(
        {.feature = query.column_text(0),
         .disabled = query.column_int64(1) != 0,
         .revision = static_cast<std::size_t>(query.column_int64(2)),
         .changed_at_ms = query.column_int64(3)});
  }
  if (result.size() != 3)
    throw std::runtime_error{"Runtime safety controls are incomplete."};
  return result;
}

RuntimeControlMutation SqliteRuntimeFeatureControlRepository::set(
    const std::string_view feature, const bool disabled,
    const DiscordSnowflake actor_user_id, std::string transition_id,
    std::string idempotency_key, const std::int64_t now_ms) {
  if ((feature != "text-ai" && feature != "tts" && feature != "vox-output") ||
      !actor_user_id.is_set() || transition_id.size() != 36 ||
      idempotency_key.empty() || idempotency_key.size() > 160 || now_ms < 0)
    throw std::invalid_argument{"Runtime safety mutation is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  auto duplicate = connection.prepare(
      "SELECT 1 FROM runtime_feature_control_transition WHERE "
      "idempotency_key=?");
  duplicate.bind(1, idempotency_key);
  if (duplicate.step()) {
    transaction.commit();
    return RuntimeControlMutation::duplicate;
  }
  auto current = connection.prepare(
      "SELECT disabled,revision FROM runtime_feature_control WHERE feature=?");
  current.bind(1, feature);
  if (!current.step())
    throw std::runtime_error{"Runtime safety target is missing."};
  const auto before = current.column_int64(0) != 0;
  const auto revision = current.column_int64(1);
  auto update = connection.prepare(
      "UPDATE runtime_feature_control SET disabled=?,revision=revision+1,"
      "actor_user_id=?,changed_at_ms=? WHERE feature=? AND revision=?");
  update.bind(1, disabled ? std::int64_t{1} : std::int64_t{0});
  update.bind(2, actor_user_id.str());
  update.bind(3, now_ms);
  update.bind(4, feature);
  update.bind(5, revision);
  update.execute();
  if (connection.changes() != 1)
    throw std::runtime_error{"Runtime safety mutation was stale."};
  auto transition = connection.prepare(
      "INSERT INTO runtime_feature_control_transition(transition_id,feature,"
      "from_disabled,to_disabled,actor_user_id,from_revision,to_revision,"
      "occurred_at_ms,idempotency_key) VALUES(?,?,?,?,?,?,?,?,?)");
  transition.bind(1, transition_id);
  transition.bind(2, feature);
  transition.bind(3, before ? std::int64_t{1} : std::int64_t{0});
  transition.bind(4, disabled ? std::int64_t{1} : std::int64_t{0});
  transition.bind(5, actor_user_id.str());
  transition.bind(6, revision);
  transition.bind(7, revision + 1);
  transition.bind(8, now_ms);
  transition.bind(9, idempotency_key);
  transition.execute();
  transaction.commit();
  return before == disabled ? RuntimeControlMutation::unchanged
                            : RuntimeControlMutation::applied;
}

} // namespace sanguinius::persistence
