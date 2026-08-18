#include "sanguinius/persistent_id.hpp"

#include "support/fake_persistent_id_generator.hpp"

#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>

TEST_CASE("persistent IDs are canonical UUID version four values", "[id]") {
  sanguinius::UuidV4Generator generator;
  std::set<std::string> generated;
  for (int index = 0; index < 64; ++index) {
    const auto value = generator.next_id();
    REQUIRE(sanguinius::valid_uuid_v4(value));
    generated.insert(value);
  }
  REQUIRE(generated.size() == 64);
}

TEST_CASE("persistent ID validation rejects malformed variants", "[id]") {
  REQUIRE(sanguinius::valid_uuid_v4("00000000-0000-4000-8000-000000000000"));
  REQUIRE_FALSE(
      sanguinius::valid_uuid_v4("00000000-0000-3000-8000-000000000000"));
  REQUIRE_FALSE(
      sanguinius::valid_uuid_v4("00000000-0000-4000-7000-000000000000"));
  REQUIRE_FALSE(
      sanguinius::valid_uuid_v4("00000000-0000-4000-8000-00000000000Z"));
}

TEST_CASE("persistent ID generation is injectable and deterministic", "[id]") {
  sanguinius::test::FakePersistentIdGenerator generator{
      {"00000000-0000-4000-8000-000000000001",
       "00000000-0000-4000-8000-000000000002"}};
  REQUIRE(generator.next_id() == "00000000-0000-4000-8000-000000000001");
  REQUIRE(generator.next_id() == "00000000-0000-4000-8000-000000000002");
  REQUIRE_THROWS_AS(generator.next_id(), std::runtime_error);
}
