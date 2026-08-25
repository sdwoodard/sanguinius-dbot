#include "sanguinius/persistence/sqlite_tarot_house_repository.hpp"

#include "sanguinius/persistence/transaction.hpp"

#include "sqlite_durable_work_writes.hpp"

#include <nlohmann/json.hpp>

#include <mutex>
#include <sqlite3.h>
#include <stdexcept>

namespace sanguinius::persistence {
namespace {

[[nodiscard]] TarotVisibility draw_visibility(const std::string_view value) {
  if (value == "public")
    return TarotVisibility::public_result;
  if (value == "private")
    return TarotVisibility::private_result;
  throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                      SQLITE_SCHEMA,
                      "Stored Tarot draw visibility is invalid."};
}

[[nodiscard]] const char *draw_visibility_name(const TarotVisibility value) {
  return value == TarotVisibility::public_result ? "public" : "private";
}

[[nodiscard]] TarotDrawRecord draw_from(SqliteStatement &query) {
  const auto flavor_variant = query.column_int64(5);
  const auto flavors = nlohmann::json::parse(query.column_text(11));
  if (!flavors.is_array() || flavor_variant < 0 ||
      static_cast<std::size_t>(flavor_variant) >= flavors.size() ||
      !flavors.at(static_cast<std::size_t>(flavor_variant)).is_string())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Stored Tarot draw flavor is invalid."};
  return {.draw_id = query.column_text(0),
          .user_id = DiscordSnowflake::parse(query.column_text(1)),
          .visibility = draw_visibility(query.column_text(2)),
          .catalog_version = query.column_text(3),
          .card_ordinal = query.column_int64(4),
          .flavor_variant = flavor_variant,
          .drawn_at_ms = query.column_int64(6),
          .cooldown_until_ms = query.column_int64(7),
          .is_test = query.column_int64(8) != 0,
          .card_name = query.column_text(9),
          .card_meaning = query.column_text(10),
          .flavor_text = flavors.at(static_cast<std::size_t>(flavor_variant))
                             .get<std::string>()};
}

[[nodiscard]] std::optional<TarotDrawRecord>
load_draw(SqliteConnection &connection, const std::string_view id) {
  auto query = connection.prepare(
      "SELECT draw.draw_id,draw.user_id,draw.visibility,draw.catalog_version,"
      "draw.card_ordinal,draw.flavor_variant,draw.drawn_at_ms,"
      "draw.cooldown_until_ms,draw.is_test,card.name,"
      "card.meaning,card.flavor_json FROM tarot_card_draw draw JOIN "
      "tarot_card_definition card ON card.catalog_version=draw.catalog_version "
      "AND card.ordinal=draw.card_ordinal WHERE draw_id=?");
  query.bind(1, id);
  if (!query.step())
    return std::nullopt;
  auto result = draw_from(query);
  if (query.step())
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA, "Duplicate Tarot draw identity."};
  return result;
}

[[nodiscard]] bool enqueue_public_draw(SqliteConnection &connection,
                                       const TarotDrawRecord &draw,
                                       const TarotInvocation &invocation,
                                       const std::string_view outbox_id,
                                       const std::string_view event_id) {
  if (draw.visibility != TarotVisibility::public_result)
    return false;
  std::string content = draw.is_test ? "[TEST] " : "";
  content += draw.card_name + "\n" + draw.card_meaning + "\n" +
             draw.flavor_text + "\nReference: " + draw.draw_id;
  auto message = text_message(std::move(content));
  message.allowed_user_mentions.clear();
  const OutboxEnqueue outbox{
      .outbox_id = std::string{outbox_id},
      .kind = std::string{public_discord_outbox_kind},
      .aggregate_type = "tarot_draw",
      .aggregate_id = draw.draw_id,
      .target_guild_id = invocation.guild_id,
      .target_channel_id = invocation.channel_id,
      .target_user_id = std::nullopt,
      .available_at_ms = invocation.now_ms,
      .max_attempts = 5,
      .idempotency_key = "outbox:tarot-draw:" + draw.draw_id,
      .provider_nonce = discord_nonce_from_uuid(outbox_id),
      .created_at_ms = invocation.now_ms,
  };
  static_cast<void>(detail::insert_outbox_uncommitted(
      connection, outbox,
      detail::encode_public_payload(
          PublicOutboxPayload{
              .request =
                  PublicMessageRequest{.guild_id = invocation.guild_id,
                                       .channel_id = invocation.channel_id,
                                       .message = std::move(message)}},
          invocation.correlation_id, std::string{event_id})));
  auto link = connection.prepare(
      "INSERT INTO tarot_draw_public_delivery(draw_id,outbox_id,created_at_ms) "
      "VALUES(?,?,?)");
  link.bind(1, draw.draw_id);
  link.bind(2, outbox_id);
  link.bind(3, invocation.now_ms);
  link.execute();
  return true;
}

} // namespace

SqliteTarotDrawRepository::SqliteTarotDrawRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"SQLite Tarot draw context is required."};
}

TarotDrawResult
SqliteTarotDrawRepository::draw(const TarotDrawRequest &request) {
  if (!request.sample || request.cooldown_ms <= 0)
    throw std::invalid_argument{"Tarot draw request is invalid."};
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};

  auto receipt = connection.prepare(
      "SELECT status,draw_id,cooldown_until_ms,user_id,visibility "
      "FROM tarot_draw_receipt WHERE idempotency_key=?");
  receipt.bind(1, request.invocation.interaction_idempotency_key);
  if (receipt.step()) {
    if (receipt.column_text(3) != request.invocation.user_id.str() ||
        receipt.column_text(4) != draw_visibility_name(request.visibility))
      throw std::invalid_argument{"Tarot draw idempotency key was reused."};
    TarotDrawResult result;
    result.cooldown_until_ms = receipt.column_int64(2);
    if (receipt.column_text(0) == "drawn") {
      result.status = TarotDrawStatus::replay;
      result.draw = load_draw(connection, receipt.column_text(1));
      if (result.draw &&
          result.draw->visibility == TarotVisibility::public_result) {
        auto delivery = connection.prepare(
            "SELECT 1 FROM tarot_draw_public_delivery WHERE draw_id=?");
        delivery.bind(1, result.draw->draw_id);
        result.public_delivery_created = delivery.step();
      }
    } else {
      result.status = TarotDrawStatus::cooldown;
    }
    transaction.commit();
    return result;
  }

  if (!request.bypass_cooldown) {
    auto active = connection.prepare(
        "SELECT cooldown_until_ms FROM tarot_card_draw WHERE user_id=? AND "
        "is_test=0 "
        "AND cooldown_until_ms>? ORDER BY drawn_at_ms DESC LIMIT 1");
    active.bind(1, request.invocation.user_id.str());
    active.bind(2, request.invocation.now_ms);
    if (active.step()) {
      const auto until = active.column_int64(0);
      auto insert = connection.prepare(
          "INSERT INTO tarot_draw_receipt(idempotency_key,draw_id,user_id,"
          "visibility,status,cooldown_until_ms,created_at_ms) "
          "VALUES(?,NULL,?,?,'cooldown',?,?)");
      insert.bind(1, request.invocation.interaction_idempotency_key);
      insert.bind(2, request.invocation.user_id.str());
      insert.bind(3, draw_visibility_name(request.visibility));
      insert.bind(4, until);
      insert.bind(5, request.invocation.now_ms);
      insert.execute();
      transaction.commit();
      return {.status = TarotDrawStatus::cooldown,
              .draw = std::nullopt,
              .cooldown_until_ms = until,
              .event_created = false};
    }
  }

  const auto [card, flavor] = request.sample();
  if (card < 0 || card >= 22 || flavor < 0 || flavor >= 4)
    throw std::invalid_argument{"Tarot draw sampler returned invalid bounds."};
  const auto cooldown_until = request.invocation.now_ms + request.cooldown_ms;
  const auto payload =
      nlohmann::json{{"draw_id", request.draw_id},
                     {"visibility", draw_visibility_name(request.visibility)},
                     {"catalog_version", emperor_tarot_catalog_version},
                     {"card_ordinal", card},
                     {"is_test", request.is_test}};
  auto event = connection.prepare(
      "INSERT INTO "
      "event_journal(event_id,event_type,aggregate_type,aggregate_id,"
      "actor_user_id,guild_id,channel_id,source_message_id,occurred_at_ms,"
      "recorded_at_ms,correlation_id,causation_id,idempotency_key,payload_json)"
      " "
      "VALUES(?,'tarot.draw_created.v1','tarot_draw',?,?,?,?,NULL,?,?,?,NULL,?,"
      "?)");
  event.bind(1, request.event_id);
  event.bind(2, request.draw_id);
  event.bind(3, request.invocation.user_id.str());
  event.bind(4, request.invocation.guild_id.str());
  event.bind(5, request.invocation.channel_id.str());
  event.bind(6, request.invocation.now_ms);
  event.bind(7, request.invocation.now_ms);
  event.bind(8, request.invocation.correlation_id);
  event.bind(9, request.invocation.interaction_idempotency_key + ":event");
  event.bind(10, payload.dump());
  event.execute();

  auto insert = connection.prepare(
      "INSERT INTO "
      "tarot_card_draw(draw_id,user_id,guild_id,channel_id,visibility,"
      "catalog_version,card_ordinal,flavor_variant,drawn_at_ms,"
      "cooldown_until_ms,is_test,event_id) VALUES(?,?,?,?,?,?,?,?,?,?,?,?)");
  insert.bind(1, request.draw_id);
  insert.bind(2, request.invocation.user_id.str());
  insert.bind(3, request.invocation.guild_id.str());
  insert.bind(4, request.invocation.channel_id.str());
  insert.bind(5, draw_visibility_name(request.visibility));
  insert.bind(6, emperor_tarot_catalog_version);
  insert.bind(7, card);
  insert.bind(8, flavor);
  insert.bind(9, request.invocation.now_ms);
  insert.bind(10, cooldown_until);
  insert.bind(11, request.is_test ? 1LL : 0LL);
  insert.bind(12, request.event_id);
  insert.execute();

  auto observation = connection.prepare(
      "INSERT INTO tarot_integration_observation(source_event_id,event_type,"
      "visibility,is_test,state,attempts,next_attempt_at_ms,last_error,"
      "created_at_ms,processed_at_ms) "
      "VALUES(?,'tarot.draw_created.v1',?,?,'pending',0,?,NULL,?,NULL)");
  observation.bind(1, request.event_id);
  observation.bind(2, draw_visibility_name(request.visibility));
  observation.bind(3, request.is_test ? 1LL : 0LL);
  observation.bind(4, request.invocation.now_ms);
  observation.bind(5, request.invocation.now_ms);
  observation.execute();

  auto save_receipt = connection.prepare(
      "INSERT INTO tarot_draw_receipt(idempotency_key,draw_id,user_id,"
      "visibility,status,cooldown_until_ms,created_at_ms) "
      "VALUES(?,?,?,?,'drawn',?,?)");
  save_receipt.bind(1, request.invocation.interaction_idempotency_key);
  save_receipt.bind(2, request.draw_id);
  save_receipt.bind(3, request.invocation.user_id.str());
  save_receipt.bind(4, draw_visibility_name(request.visibility));
  save_receipt.bind(5, cooldown_until);
  save_receipt.bind(6, request.invocation.now_ms);
  save_receipt.execute();
  const auto persisted = load_draw(connection, request.draw_id);
  if (!persisted)
    throw DatabaseError{DatabaseErrorCategory::schema, SQLITE_SCHEMA,
                        SQLITE_SCHEMA,
                        "Persisted Tarot draw could not be reloaded."};
  bool public_delivery_created{};
  if (request.visibility == TarotVisibility::public_result) {
    if (request.public_outbox_id.empty())
      throw std::invalid_argument{"Public Tarot draw outbox ID is required."};
    public_delivery_created =
        enqueue_public_draw(connection, *persisted, request.invocation,
                            request.public_outbox_id, request.event_id);
  }
  transaction.commit();
  return {.status = TarotDrawStatus::drawn,
          .draw = persisted,
          .cooldown_until_ms = cooldown_until,
          .event_created = true,
          .public_delivery_created = public_delivery_created};
}

std::optional<TarotDrawRecord>
SqliteTarotDrawRepository::find(const std::string_view draw_id,
                                const DiscordSnowflake &requester) {
  std::scoped_lock lock{context_->mutex()};
  auto result = load_draw(context_->connection(), draw_id);
  if (!result || result->user_id != requester)
    return std::nullopt;
  return result;
}

} // namespace sanguinius::persistence
