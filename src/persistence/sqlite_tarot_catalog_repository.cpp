#include "sanguinius/persistence/sqlite_tarot_house_repository.hpp"

#include "sanguinius/persistence/transaction.hpp"

#include <nlohmann/json.hpp>

#include <mutex>
#include <stdexcept>

namespace sanguinius::persistence {
namespace {

void install_snapshot(SqliteConnection &connection, const std::string_view version,
                      const std::string_view kind,
                      const std::string_view canonical,
                      const std::string_view checksum,
                      const std::int64_t installed_at_ms) {
  auto existing = connection.prepare(
      "SELECT catalog_kind,canonical_json,checksum FROM tarot_catalog_snapshot "
      "WHERE catalog_version=?");
  existing.bind(1, version);
  if (existing.step()) {
    const auto matches = existing.column_text(0) == kind &&
                         existing.column_text(1) == canonical &&
                         existing.column_text(2) == checksum;
    if (existing.step() || !matches)
      throw std::runtime_error{"Tarot catalog version checksum collision."};
    return;
  }
  auto insert = connection.prepare(
      "INSERT INTO tarot_catalog_snapshot(catalog_version,catalog_kind,"
      "canonical_json,checksum,installed_at_ms) VALUES(?,?,?,?,?)");
  insert.bind(1, version);
  insert.bind(2, kind);
  insert.bind(3, canonical);
  insert.bind(4, checksum);
  insert.bind(5, installed_at_ms);
  insert.execute();
}

} // namespace

SqliteTarotCatalogRepository::SqliteTarotCatalogRepository(
    std::shared_ptr<SqliteRepositoryContext> context)
    : context_{std::move(context)} {
  if (!context_)
    throw std::invalid_argument{"SQLite Tarot catalog context is required."};
}

void SqliteTarotCatalogRepository::install(const TarotDeckCatalog &deck,
                                           const TarotHouseCatalog &house,
                                           const std::int64_t installed_at_ms) {
  std::scoped_lock lock{context_->mutex()};
  auto &connection = context_->connection();
  Transaction transaction{connection, TransactionMode::immediate};
  install_snapshot(connection, deck.version, "deck", deck.canonical_json,
                   deck.checksum, installed_at_ms);
  for (const auto &card : deck.cards) {
    auto flavors = nlohmann::json::array();
    for (const auto &variant : card.flavor_variants)
      flavors.push_back(variant);
    auto insert = connection.prepare(
        "INSERT OR IGNORE INTO tarot_card_definition(catalog_version,ordinal,"
        "slug,name,meaning,theme_tag,safety_prompt,flavor_json) "
        "VALUES(?,?,?,?,?,?,?,?)");
    insert.bind(1, deck.version);
    insert.bind(2, card.ordinal);
    insert.bind(3, card.slug);
    insert.bind(4, card.name);
    insert.bind(5, card.meaning);
    insert.bind(6, card.theme_tag);
    insert.bind(7, card.safety_prompt);
    insert.bind(8, flavors.dump());
    insert.execute();
  }
  install_snapshot(connection, house.version, "house", house.canonical_json,
                   house.checksum, installed_at_ms);
  const auto parsed = nlohmann::json::parse(house.canonical_json);
  for (const auto &definition : parsed.at("templates")) {
    auto insert = connection.prepare(
        "INSERT OR IGNORE INTO tarot_house_template_definition("
        "catalog_version,template_slug,canonical_json) VALUES(?,?,?)");
    insert.bind(1, house.version);
    insert.bind(2, definition.at("slug").get<std::string>());
    insert.bind(3, definition.dump());
    insert.execute();
  }
  transaction.commit();
}

} // namespace sanguinius::persistence
