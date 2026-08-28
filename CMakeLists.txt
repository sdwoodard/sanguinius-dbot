cmake_minimum_required(VERSION 3.25)

project(sanguinius VERSION 2.2.0 LANGUAGES CXX)

include(CTest)

set(SANGUINIUS_REVISION_OVERRIDE "" CACHE STRING
    "Git revision to embed instead of detecting the current checkout")
set(SANGUINIUS_RELEASE_ID "development" CACHE STRING
    "Immutable release identifier embedded in the binary")
set(SANGUINIUS_BUILD_TIMESTAMP "unknown" CACHE STRING
    "UTC build timestamp embedded in the binary")
set(SANGUINIUS_TOOLCHAIN_ID "local" CACHE STRING
    "Pinned release-toolchain identity embedded in the binary")
if(SANGUINIUS_REVISION_OVERRIDE)
    set(sanguinius_build_revision "${SANGUINIUS_REVISION_OVERRIDE}")
else()
    execute_process(
        COMMAND git rev-parse HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE sanguinius_git_result
        OUTPUT_VARIABLE sanguinius_build_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
    )
    if(NOT sanguinius_git_result EQUAL 0 OR NOT sanguinius_build_revision)
        set(sanguinius_build_revision "unknown")
    endif()
endif()

find_package(dpp 10.1.7 CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)

include(CheckCXXSourceCompiles)
include(CheckCXXSourceRuns)
set(CMAKE_REQUIRED_LIBRARIES dpp::dpp)
check_cxx_source_compiles([[
#include <cstdint>
#include <dpp/dpp.h>
static_assert(DPP_VERSION_SHORT >= 100107);
void sanguinius_voice_api_probe(dpp::discord_client& shard,
                                dpp::discord_voice_client& voice) {
  shard.connect_voice(dpp::snowflake{1}, dpp::snowflake{2}, false, true, true);
  shard.queue_message(R"({"op":4,"d":{}})", true);
  static_cast<void>(voice.is_end_to_end_encrypted());
  std::uint16_t samples[2]{};
  voice.send_audio_raw(samples, sizeof(samples));
  voice.insert_marker("sanguinius-voice-api-probe");
  shard.disconnect_voice(dpp::snowflake{1});
  static_cast<void>(dpp::i_guild_voice_states);
}
int main() { return 0; }
]] SANGUINIUS_DPP_VOICE_API_AVAILABLE)
if(NOT SANGUINIUS_DPP_VOICE_API_AVAILABLE)
    message(FATAL_ERROR
        "D++ 10.1.7+ was found, but required voice/DAVE APIs did not compile "
        "and link. Rebuild D++ with BUILD_VOICE_SUPPORT=ON and integrated "
        "DAVE/MLS support.")
endif()

# Voice receive is an experimental, optional D++ surface. A build without the
# callback remains fully usable for text and voice output and reports
# unsupported_build at runtime.
check_cxx_source_compiles([[
#include <dpp/dpp.h>
void sanguinius_voice_receive_probe(dpp::cluster& cluster) {
  cluster.on_voice_receive_combined([](const dpp::voice_receive_t& event) {
    static_cast<void>(event.audio_data.size());
  });
}
int main() { return 0; }
]] SANGUINIUS_DPP_VOICE_RECEIVE_AVAILABLE)
if(DEFINED ENV{LSAN_OPTIONS})
    set(sanguinius_saved_lsan_options "$ENV{LSAN_OPTIONS}")
    set(sanguinius_had_lsan_options TRUE)
endif()
set(ENV{LSAN_OPTIONS} "detect_leaks=0")
check_cxx_source_runs([[
#include <dpp/dpp.h>
int main() { return dpp::utility::has_voice() ? 0 : 1; }
]] SANGUINIUS_DPP_VOICE_RUNTIME_AVAILABLE)
if(sanguinius_had_lsan_options)
    set(ENV{LSAN_OPTIONS} "${sanguinius_saved_lsan_options}")
else()
    unset(ENV{LSAN_OPTIONS})
endif()
unset(sanguinius_saved_lsan_options)
unset(sanguinius_had_lsan_options)
unset(CMAKE_REQUIRED_LIBRARIES)
if(NOT SANGUINIUS_DPP_VOICE_RUNTIME_AVAILABLE)
    message(FATAL_ERROR
        "D++ 10.1.7+ was found, but dpp::utility::has_voice() reports that this library "
        "was built without voice support. Rebuild D++ with "
        "BUILD_VOICE_SUPPORT=ON and integrated DAVE/MLS support.")
endif()

if(NOT SQLite3_VERSION)
    message(FATAL_ERROR "SQLite version could not be determined")
endif()
if(SQLite3_VERSION VERSION_LESS "3.51.3"
   AND NOT SQLite3_VERSION VERSION_EQUAL "3.50.7"
   AND NOT SQLite3_VERSION VERSION_EQUAL "3.44.6")
    message(FATAL_ERROR
        "SQLite ${SQLite3_VERSION} lacks the required WAL-reset fix; "
        "use SQLite 3.51.3+ or an approved fixed backport")
endif()
if(NOT TARGET SQLite3::SQLite3)
    if(TARGET SQLite::SQLite3)
        add_library(SQLite3::SQLite3 ALIAS SQLite::SQLite3)
    else()
        message(FATAL_ERROR "FindSQLite3 did not provide an imported target")
    endif()
endif()

include(cmake/EmbedMigrations.cmake)
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
sanguinius_embed_migrations(
    "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_migrations.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embedded_migration.cpp.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0001_core_foundation.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0002_discord_interactions.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0003_durable_work.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0004_chronicle.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0005_relationships.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0006_chronicle_sessions.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0007_appearance_dry_run.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0008_appearance_live.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0009_tarot_ledger.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0010_peer_wagers.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0011_tarot_house_integration.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0012_vox_foundation.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0013_vox_tts.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0014_vox_narration.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0015_voice_input.sql"
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0016_cross_feature_reliability.sql"
)

function(sanguinius_enable_warnings target)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE
            -Wall
            -Wextra
            -Wpedantic
            -Wconversion
            -Wshadow
        )
    elseif(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    endif()
endfunction()

add_library(sanguinius_core STATIC
    src/ai_client.cpp
    src/ai_generation.cpp
    src/ai_responder.cpp
    src/ai_work_service.cpp
    src/appearance_policy.cpp
    src/appearances.cpp
    src/application.cpp
    src/build_info.cpp
    src/callback_fence.cpp
    src/chronicle.cpp
    src/chronicle_sessions.cpp
    src/clock.cpp
    src/command_registry.cpp
    src/config.cpp
    src/cross_feature_orchestrator.cpp
    src/diagnostics.cpp
    src/durable_work.cpp
    src/durable_work_controls.cpp
    src/discord_command_cli.cpp
    src/feature_config.cpp
    src/health.cpp
    src/id_generator.cpp
    src/interaction_handler.cpp
    src/interaction_response_state.cpp
    src/interaction_router.cpp
    src/message_handler.cpp
    src/message_logger.cpp
    src/owner_admin.cpp
    src/outbox.cpp
    src/operations_health.cpp
    src/pending_notice.cpp
    src/persistent_id.cpp
    src/presentation.cpp
    src/provider_circuit.cpp
    src/prompt_compiler.cpp
    src/random.cpp
    src/retention.cpp
    src/relationships.cpp
    src/safety_controls.cpp
    src/sanguinius_overview.cpp
    src/repositories.cpp
    src/scheduler.cpp
    src/service_notifier.cpp
    src/server_scope_policy.cpp
    src/speech.cpp
    src/speech_service.cpp
    src/static_speech_assets.cpp
    src/snowflake.cpp
    src/tarot.cpp
    src/tarot_catalog.cpp
    src/tarot_house.cpp
    src/tarot_integration.cpp
    src/tts.cpp
    src/transcription.cpp
    src/wagers.cpp
    src/vox.cpp
    src/vox_narration.cpp
    src/voice_input.cpp
    src/work_queue.cpp
)

target_compile_features(sanguinius_core PUBLIC cxx_std_20)
target_include_directories(sanguinius_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(sanguinius_core PUBLIC
    SANGUINIUS_BUILD_VERSION="${PROJECT_VERSION}"
    SANGUINIUS_BUILD_REVISION="${sanguinius_build_revision}"
    SANGUINIUS_RELEASE_ID="${SANGUINIUS_RELEASE_ID}"
    SANGUINIUS_BUILD_TIMESTAMP="${SANGUINIUS_BUILD_TIMESTAMP}"
    SANGUINIUS_TOOLCHAIN_ID="${SANGUINIUS_TOOLCHAIN_ID}"
)
sanguinius_enable_warnings(sanguinius_core)
target_link_libraries(sanguinius_core
    PUBLIC
        Threads::Threads
        OpenSSL::Crypto
    PRIVATE
        nlohmann_json::nlohmann_json
        PkgConfig::SYSTEMD
)

add_library(sanguinius_persistence STATIC
    "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_migrations.cpp"
    src/database_cli.cpp
    src/persistence/backup.cpp
    src/persistence/database.cpp
    src/persistence/migrator.cpp
    src/persistence/reliability_test.cpp
    src/persistence/sqlite.cpp
    src/persistence/sqlite_ai_generation_repository.cpp
    src/persistence/sqlite_provider_circuit_repository.cpp
    src/persistence/sqlite_chronicle_repository.cpp
    src/persistence/sqlite_chronicle_session_repository.cpp
    src/persistence/sqlite_appearance_repository.cpp
    src/persistence/sqlite_durable_work_repository.cpp
    src/persistence/sqlite_relationship_repository.cpp
    src/persistence/sqlite_retention_repository.cpp
    src/persistence/sqlite_speech_repository.cpp
    src/persistence/sqlite_repositories.cpp
    src/persistence/sqlite_safety_control_repository.cpp
    src/persistence/sqlite_tarot_repository.cpp
    src/persistence/sqlite_tarot_catalog_repository.cpp
    src/persistence/sqlite_tarot_draw_repository.cpp
    src/persistence/sqlite_tarot_house_repository.cpp
    src/persistence/sqlite_tarot_integration_repository.cpp
    src/persistence/sqlite_wager_repository.cpp
    src/persistence/sqlite_vox_repository.cpp
    src/persistence/sqlite_vox_narration_repository.cpp
    src/persistence/sqlite_voice_input_repository.cpp
    src/persistence/transaction.cpp
)

target_compile_features(sanguinius_persistence PUBLIC cxx_std_20)
target_include_directories(sanguinius_persistence PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
)
sanguinius_enable_warnings(sanguinius_persistence)
target_link_libraries(sanguinius_persistence
    PUBLIC
        sanguinius_core
    PRIVATE
        SQLite3::SQLite3
        nlohmann_json::nlohmann_json
)

add_library(sanguinius_runtime STATIC
    src/composition_root.cpp
    src/dpp_cluster_host.cpp
    src/dpp_command_registry.cpp
    src/dpp_discord_adapter.cpp
    src/dpp_voice_gateway.cpp
    src/dpp_voice_input_adapter.cpp
    src/ffmpeg_audio_normalizer.cpp
    src/openai_client.cpp
    src/openai_tts_client.cpp
    src/openai_transcription_client.cpp
    src/process_signals.cpp
    src/tts_cache.cpp
)

target_compile_features(sanguinius_runtime PUBLIC cxx_std_20)
target_include_directories(sanguinius_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
if(SANGUINIUS_DPP_VOICE_RECEIVE_AVAILABLE)
    target_compile_definitions(sanguinius_runtime PRIVATE
        SANGUINIUS_HAVE_DPP_VOICE_RECEIVE=1)
else()
    target_compile_definitions(sanguinius_runtime PRIVATE
        SANGUINIUS_HAVE_DPP_VOICE_RECEIVE=0)
    message(STATUS
        "D++ combined voice receive callback is unavailable; voice input will "
        "build as an unsupported stub")
endif()
sanguinius_enable_warnings(sanguinius_runtime)
target_link_libraries(sanguinius_runtime
    PUBLIC
        sanguinius_core
        sanguinius_persistence
        dpp::dpp
    PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
)

add_executable(sanguinius src/main.cpp)
sanguinius_enable_warnings(sanguinius)
target_link_libraries(sanguinius PRIVATE sanguinius_runtime)
set_target_properties(sanguinius PROPERTIES
    INSTALL_RPATH "$ORIGIN/../lib"
    INSTALL_RPATH_USE_LINK_PATH FALSE
)

if(BUILD_TESTING)
    find_package(Catch2 3 CONFIG REQUIRED)
    include(Catch)

    set(SANGUINIUS_TEST_CASE_TIMEOUT_SECONDS 10 CACHE STRING
        "Per-case CTest timeout; sanitizer builds may raise this bounded limit"
    )
    set(SANGUINIUS_MIGRATION_MATRIX_TIMEOUT_SECONDS 30 CACHE STRING
        "Bounded timeout for the complete accepted-schema upgrade matrix"
    )
    set(SANGUINIUS_PROPERTY_SEQUENCE_TIMEOUT_SECONDS 30 CACHE STRING
        "Bounded timeout for the deterministic mixed-domain property sequence"
    )

    function(add_sanguinius_test target)
        add_executable(${target} ${ARGN})
        target_compile_features(${target} PRIVATE cxx_std_20)
        target_include_directories(${target} PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/tests"
        )
        sanguinius_enable_warnings(${target})
        target_link_libraries(${target} PRIVATE
            sanguinius_core
            Catch2::Catch2WithMain
        )
        catch_discover_tests(${target}
            PROPERTIES TIMEOUT ${SANGUINIUS_TEST_CASE_TIMEOUT_SECONDS}
        )
    endfunction()

    add_sanguinius_test(sanguinius_unit_tests
        tests/unit/appearance_policy_tests.cpp
        tests/unit/appearance_flow_tests.cpp
        tests/unit/chronicle_tests.cpp
        tests/unit/chronicle_session_tests.cpp
        tests/unit/config_tests.cpp
        tests/unit/command_parse_tests.cpp
        tests/unit/durable_work_service_tests.cpp
        tests/unit/health_tests.cpp
        tests/unit/interaction_core_tests.cpp
        tests/unit/mention_parse_tests.cpp
        tests/unit/message_logger_tests.cpp
        tests/unit/m18_reliability_tests.cpp
        tests/unit/m19_operations_tests.cpp
        tests/unit/owner_admin_tests.cpp
        tests/unit/relationship_tests.cpp
        tests/unit/server_scope_policy_tests.cpp
        tests/unit/snowflake_tests.cpp
        tests/unit/tarot_tests.cpp
        tests/unit/tarot_catalog_tests.cpp
        tests/unit/tts_tests.cpp
        tests/unit/wager_tests.cpp
        tests/unit/vox_tests.cpp
        tests/unit/vox_narration_tests.cpp
        tests/unit/voice_input_tests.cpp
        tests/unit/work_queue_tests.cpp
    )
    add_sanguinius_test(sanguinius_application_tests
        tests/application/application_flow_tests.cpp
        tests/application/failure_and_shutdown_tests.cpp
        tests/application/interaction_flow_tests.cpp
    )

    add_executable(sanguinius_persistence_tests
        tests/integration/appearance_repository_tests.cpp
        tests/integration/backup_tests.cpp
        tests/integration/chronicle_repository_tests.cpp
        tests/integration/chronicle_session_repository_tests.cpp
        tests/integration/database_cli_tests.cpp
        tests/integration/durable_work_tests.cpp
        tests/integration/migration_tests.cpp
        tests/integration/m18_reliability_repository_tests.cpp
        tests/integration/m19_reliability_tests.cpp
        tests/integration/repository_tests.cpp
        tests/integration/relationship_repository_tests.cpp
        tests/integration/speech_repository_tests.cpp
        tests/integration/sqlite_tests.cpp
        tests/integration/tarot_repository_tests.cpp
        tests/integration/tarot_house_repository_tests.cpp
        tests/integration/wager_repository_tests.cpp
        tests/integration/vox_repository_tests.cpp
        tests/integration/vox_narration_repository_tests.cpp
        tests/integration/voice_input_repository_tests.cpp
        tests/unit/persistent_id_tests.cpp
    )
    target_compile_features(sanguinius_persistence_tests PRIVATE cxx_std_20)
    target_include_directories(sanguinius_persistence_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    sanguinius_enable_warnings(sanguinius_persistence_tests)
    target_link_libraries(sanguinius_persistence_tests PRIVATE
        sanguinius_persistence
        Catch2::Catch2WithMain
    )
    catch_discover_tests(sanguinius_persistence_tests
        TEST_SPEC "~[upgrade-matrix]~[bounded-long]"
        PROPERTIES TIMEOUT ${SANGUINIUS_TEST_CASE_TIMEOUT_SECONDS}
    )
    add_test(
        NAME "production migration upgrades every accepted prior schema to version sixteen"
        COMMAND "$<TARGET_FILE:sanguinius_persistence_tests>" "[upgrade-matrix]"
    )
    set_tests_properties(
        "production migration upgrades every accepted prior schema to version sixteen"
        PROPERTIES TIMEOUT ${SANGUINIUS_MIGRATION_MATRIX_TIMEOUT_SECONDS}
    )
    add_test(
        NAME "mixed Tarot property sequences preserve the reference ledger model"
        COMMAND "$<TARGET_FILE:sanguinius_persistence_tests>" "[bounded-long]"
    )
    set_tests_properties(
        "mixed Tarot property sequences preserve the reference ledger model"
        PROPERTIES TIMEOUT ${SANGUINIUS_PROPERTY_SEQUENCE_TIMEOUT_SECONDS}
    )

    add_executable(sanguinius_runtime_tests
        tests/runtime/dpp_command_registration_tests.cpp
        tests/runtime/openai_runtime_tests.cpp
        tests/runtime/tts_runtime_tests.cpp
        tests/runtime/transcription_runtime_tests.cpp
    )
    target_compile_features(sanguinius_runtime_tests PRIVATE cxx_std_20)
    target_include_directories(sanguinius_runtime_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/tests"
    )
    target_compile_definitions(sanguinius_runtime_tests PRIVATE
        SANGUINIUS_TEST_VOX_ASSET_DIRECTORY="${CMAKE_CURRENT_SOURCE_DIR}/assets/vox"
    )
    sanguinius_enable_warnings(sanguinius_runtime_tests)
    target_link_libraries(sanguinius_runtime_tests PRIVATE
        sanguinius_runtime
        dpp::dpp
        Catch2::Catch2WithMain
    )
    catch_discover_tests(sanguinius_runtime_tests
        PROPERTIES TIMEOUT ${SANGUINIUS_TEST_CASE_TIMEOUT_SECONDS}
    )

    set(sanguinius_config_environment_unsets
        --unset=SANGUINIUS_TOKEN
        --unset=SANGUINIUS_TOKEN_FILE
        --unset=OPENAI_API_KEY
        --unset=SANGUINIUS_OPENAI_API_KEY_FILE
        --unset=SANGUINIUS_LOG_FILE
        --unset=SANGUINIUS_COMMAND_PREFIX
        --unset=SANGUINIUS_OPENAI_MODEL
        --unset=SANGUINIUS_OPENAI_INPUT_MICRO_USD_PER_MILLION_TOKENS
        --unset=SANGUINIUS_OPENAI_OUTPUT_MICRO_USD_PER_MILLION_TOKENS
        --unset=SANGUINIUS_PERSONA_FILE
        --unset=SANGUINIUS_APPEARANCE_POLICY_FILE
        --unset=SANGUINIUS_TAROT_DECK_FILE
        --unset=SANGUINIUS_TAROT_HOUSE_FILE
        --unset=SANGUINIUS_TAROT_STARTING_FATE
        --unset=SANGUINIUS_TAROT_GRACE_THRESHOLD
        --unset=SANGUINIUS_TAROT_GRACE_TARGET
        --unset=SANGUINIUS_TAROT_GRACE_COOLDOWN_HOURS
        --unset=SANGUINIUS_TAROT_TRIAL_THRESHOLD
        --unset=SANGUINIUS_TAROT_TRIAL_REWARD_MIN
        --unset=SANGUINIUS_TAROT_TRIAL_REWARD_MAX
        --unset=SANGUINIUS_TAROT_TRIAL_COOLDOWN_HOURS
        --unset=SANGUINIUS_TAROT_DRAW_COOLDOWN_HOURS
        --unset=SANGUINIUS_TAROT_HOUSE_ENABLED
        --unset=SANGUINIUS_TAROT_HOUSE_EXPOSURE_CAP
        --unset=SANGUINIUS_TAROT_HOUSE_PROFIT_CAP
        --unset=SANGUINIUS_TAROT_INTEGRATION_ENABLED
        --unset=SANGUINIUS_DATABASE_FILE
        --unset=SANGUINIUS_OPERATIONS_STATUS_FILE
        --unset=SANGUINIUS_BACKUP_DIRECTORY
        --unset=SANGUINIUS_ADMIN_COMMANDS_ENABLED
        --unset=SANGUINIUS_TEST_MODE
        --unset=SANGUINIUS_CHRONICLE_ENABLED
        --unset=SANGUINIUS_TAROT_ENABLED
        --unset=SANGUINIUS_APPEARANCES_MODE
        --unset=SANGUINIUS_VOX_ENABLED
        --unset=SANGUINIUS_VOX_NARRATION_ENABLED
        --unset=SANGUINIUS_VOICE_INPUT_ENABLED
        --unset=SANGUINIUS_VOICE_INPUT_GUILD_CONSENT_ATTESTED
        --unset=SANGUINIUS_TRANSCRIPTION_PROVIDER
        --unset=SANGUINIUS_TRANSCRIPTION_MODEL
        --unset=SANGUINIUS_TRANSCRIPTION_REQUEST_TIMEOUT_MS
        --unset=SANGUINIUS_VOICE_INPUT_ROLLING_DAY_WINDOWS
        --unset=SANGUINIUS_VOICE_INPUT_ROLLING_DAY_MICRO_USD
        --unset=SANGUINIUS_VOICE_INPUT_MONTHLY_MICRO_USD
        --unset=SANGUINIUS_TTS_PROVIDER
        --unset=SANGUINIUS_TTS_MODEL
        --unset=SANGUINIUS_TTS_VOICE
        --unset=SANGUINIUS_TTS_CACHE_DIRECTORY
        --unset=SANGUINIUS_FFMPEG_PATH
        --unset=SANGUINIUS_FFPROBE_PATH
        --unset=SANGUINIUS_TTS_FALLBACK_DIRECTORY
        --unset=SANGUINIUS_TTS_MAXIMUM_TEXT_SCALARS
        --unset=SANGUINIUS_TTS_ROLLING_DAY_ATTEMPTS
        --unset=SANGUINIUS_TTS_ROLLING_DAY_MICRO_USD
        --unset=SANGUINIUS_TTS_MONTHLY_MICRO_USD
        --unset=SANGUINIUS_TTS_CACHE_MAXIMUM_MIB
        --unset=SANGUINIUS_TTS_CACHE_MAXIMUM_DAYS
        --unset=SANGUINIUS_TTS_MAXIMUM_DURATION_SECONDS
        --unset=SANGUINIUS_TTS_CONNECT_TIMEOUT_MS
        --unset=SANGUINIUS_TTS_REQUEST_TIMEOUT_MS
        --unset=SANGUINIUS_FFPROBE_TIMEOUT_MS
        --unset=SANGUINIUS_FFMPEG_TIMEOUT_MS
        --unset=SANGUINIUS_TIMEZONE
        --unset=SANGUINIUS_DISCORD_REQUEST_TIMEOUT_SECONDS
    )

    add_test(NAME sanguinius_check_config
        COMMAND "${CMAKE_COMMAND}" -E env
            ${sanguinius_config_environment_unsets}
            SANGUINIUS_TOKEN=DISCORD_SECRET_SENTINEL
            OPENAI_API_KEY=OPENAI_SECRET_SENTINEL
            SANGUINIUS_GUILD_ID=18446744073709551615
            SANGUINIUS_PRIMARY_CHANNEL_ID=9223372036854775808
            SANGUINIUS_OWNER_USER_ID=123456789012345678
            SANGUINIUS_DATABASE_FILE=DATABASE_PATH_SENTINEL
            SANGUINIUS_TTS_CACHE_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/check-config-tts-cache
            SANGUINIUS_PERSONA_FILE=${CMAKE_CURRENT_SOURCE_DIR}/config/persona.txt
            SANGUINIUS_APPEARANCE_POLICY_FILE=${CMAKE_CURRENT_SOURCE_DIR}/config/appearance-policy-v2.json
            SANGUINIUS_DISCORD_REQUEST_TIMEOUT_SECONDS=1
            "$<TARGET_FILE:sanguinius>" --check-config
    )

    add_test(NAME sanguinius_version_json
        COMMAND "$<TARGET_FILE:sanguinius>" --version --json
    )
    set_tests_properties(sanguinius_version_json PROPERTIES
        TIMEOUT 3
        PASS_REGULAR_EXPRESSION "\"version\":\"2.2.0\""
        FAIL_REGULAR_EXPRESSION "DISCORD;OPENAI;token;api_key"
    )
    set_tests_properties(sanguinius_check_config PROPERTIES
        TIMEOUT 3
        PASS_REGULAR_EXPRESSION "Configuration valid"
        FAIL_REGULAR_EXPRESSION
            "DISCORD_SECRET_SENTINEL;OPENAI_SECRET_SENTINEL;DATABASE_PATH_SENTINEL;18446744073709551615;9223372036854775808;123456789012345678"
    )

    add_test(NAME sanguinius_check_config_secret_file_error
        COMMAND "${CMAKE_COMMAND}" -E env
            ${sanguinius_config_environment_unsets}
            SANGUINIUS_TOKEN_FILE=${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt/TOKEN_FILE_PATH_SECRET_SENTINEL
            OPENAI_API_KEY=OPENAI_SECRET_SENTINEL
            SANGUINIUS_GUILD_ID=10
            SANGUINIUS_PRIMARY_CHANNEL_ID=20
            SANGUINIUS_OWNER_USER_ID=30
            SANGUINIUS_PERSONA_FILE=${CMAKE_CURRENT_SOURCE_DIR}/config/persona.txt
            SANGUINIUS_APPEARANCE_POLICY_FILE=${CMAKE_CURRENT_SOURCE_DIR}/config/appearance-policy-v2.json
            "$<TARGET_FILE:sanguinius>" --check-config
    )
    set_tests_properties(sanguinius_check_config_secret_file_error PROPERTIES
        TIMEOUT 3
        PASS_REGULAR_EXPRESSION "SANGUINIUS_TOKEN_FILE"
        FAIL_REGULAR_EXPRESSION
            "TOKEN_FILE_PATH_SECRET_SENTINEL;OPENAI_SECRET_SENTINEL"
    )

    add_test(NAME sanguinius_startup_path_redaction
        COMMAND "${CMAKE_COMMAND}" -E env
            ${sanguinius_config_environment_unsets}
            SANGUINIUS_TOKEN=DISCORD_SECRET_SENTINEL
            OPENAI_API_KEY=OPENAI_SECRET_SENTINEL
            SANGUINIUS_GUILD_ID=10
            SANGUINIUS_PRIMARY_CHANNEL_ID=20
            SANGUINIUS_OWNER_USER_ID=30
            SANGUINIUS_DATABASE_FILE=${CMAKE_CURRENT_BINARY_DIR}/startup-test.sqlite3
            SANGUINIUS_LOG_FILE=${CMAKE_CURRENT_SOURCE_DIR}/CMakeLists.txt/LOG_PATH_SECRET_SENTINEL/messages.log
            SANGUINIUS_TTS_CACHE_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/startup-path-redaction-tts-cache
            SANGUINIUS_PERSONA_FILE=${CMAKE_CURRENT_SOURCE_DIR}/config/persona.txt
            SANGUINIUS_APPEARANCE_POLICY_FILE=${CMAKE_CURRENT_SOURCE_DIR}/config/appearance-policy-v2.json
            "$<TARGET_FILE:sanguinius>"
    )
    set_tests_properties(sanguinius_startup_path_redaction PROPERTIES
        TIMEOUT 3
        FIXTURES_REQUIRED sanguinius_startup_database
        PASS_REGULAR_EXPRESSION "SANGUINIUS_LOG_FILE"
        FAIL_REGULAR_EXPRESSION
            "LOG_PATH_SECRET_SENTINEL;DISCORD_SECRET_SENTINEL;OPENAI_SECRET_SENTINEL"
    )

    add_test(NAME sanguinius_startup_requires_existing_database
        COMMAND "${CMAKE_COMMAND}" -E env
            ${sanguinius_config_environment_unsets}
            SANGUINIUS_TOKEN=DISCORD_SECRET_SENTINEL
            OPENAI_API_KEY=OPENAI_SECRET_SENTINEL
            SANGUINIUS_GUILD_ID=10
            SANGUINIUS_PRIMARY_CHANNEL_ID=20
            SANGUINIUS_OWNER_USER_ID=30
            SANGUINIUS_DATABASE_FILE=${CMAKE_CURRENT_BINARY_DIR}/missing-startup-test.sqlite3
            SANGUINIUS_LOG_FILE=${CMAKE_CURRENT_BINARY_DIR}/messages.log
            SANGUINIUS_TTS_CACHE_DIRECTORY=${CMAKE_CURRENT_BINARY_DIR}/startup-missing-tts-cache
            SANGUINIUS_PERSONA_FILE=${CMAKE_CURRENT_SOURCE_DIR}/config/persona.txt
            SANGUINIUS_APPEARANCE_POLICY_FILE=${CMAKE_CURRENT_SOURCE_DIR}/config/appearance-policy-v2.json
            "$<TARGET_FILE:sanguinius>"
    )
    set_tests_properties(sanguinius_startup_requires_existing_database PROPERTIES
        TIMEOUT 3
        FIXTURES_REQUIRED sanguinius_missing_database_reset
        PASS_REGULAR_EXPRESSION "run db migrate"
        FAIL_REGULAR_EXPRESSION
            "missing-startup-test;DISCORD_SECRET_SENTINEL;OPENAI_SECRET_SENTINEL"
    )

    add_test(NAME sanguinius_release_shell_tests
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/release_tests.bash)
    add_test(NAME sanguinius_deployment_shell_tests
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/deployment_tests.bash)
    add_test(NAME sanguinius_backup_restore_shell_tests
        COMMAND bash ${CMAKE_CURRENT_SOURCE_DIR}/tests/scripts/backup_restore_tests.bash)
    set_tests_properties(
        sanguinius_release_shell_tests
        sanguinius_deployment_shell_tests
        sanguinius_backup_restore_shell_tests
        PROPERTIES TIMEOUT 30)

    add_test(NAME sanguinius_prepare_startup_database
        COMMAND "${CMAKE_COMMAND}" -E env
            ${sanguinius_config_environment_unsets}
            SANGUINIUS_DATABASE_FILE=${CMAKE_CURRENT_BINARY_DIR}/startup-test.sqlite3
            "$<TARGET_FILE:sanguinius>" db migrate
    )
    set_tests_properties(sanguinius_prepare_startup_database PROPERTIES
        TIMEOUT 3
        FIXTURES_REQUIRED sanguinius_startup_database_reset
        FIXTURES_SETUP sanguinius_startup_database
        PASS_REGULAR_EXPRESSION "database=current"
    )

    add_test(NAME sanguinius_reset_startup_database
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            ${CMAKE_CURRENT_BINARY_DIR}/startup-test.sqlite3
            ${CMAKE_CURRENT_BINARY_DIR}/startup-test.sqlite3-wal
            ${CMAKE_CURRENT_BINARY_DIR}/startup-test.sqlite3-shm
            ${CMAKE_CURRENT_BINARY_DIR}/startup-test.sqlite3-journal
            ${CMAKE_CURRENT_BINARY_DIR}/startup-test.sqlite3.lock
    )
    set_tests_properties(sanguinius_reset_startup_database PROPERTIES
        TIMEOUT 3
        FIXTURES_SETUP sanguinius_startup_database_reset
    )

    add_test(NAME sanguinius_reset_missing_database
        COMMAND "${CMAKE_COMMAND}" -E rm -f
            ${CMAKE_CURRENT_BINARY_DIR}/missing-startup-test.sqlite3
            ${CMAKE_CURRENT_BINARY_DIR}/missing-startup-test.sqlite3-wal
            ${CMAKE_CURRENT_BINARY_DIR}/missing-startup-test.sqlite3-shm
            ${CMAKE_CURRENT_BINARY_DIR}/missing-startup-test.sqlite3-journal
            ${CMAKE_CURRENT_BINARY_DIR}/missing-startup-test.sqlite3.lock
    )
    set_tests_properties(sanguinius_reset_missing_database PROPERTIES
        TIMEOUT 3
        FIXTURES_SETUP sanguinius_missing_database_reset
    )
endif()

install(TARGETS sanguinius RUNTIME DESTINATION bin)
install(DIRECTORY assets/vox/
    DESTINATION assets/vox
    FILES_MATCHING
    PATTERN "*.pcm"
    PATTERN "fallbacks-v1.json"
)
install(FILES
    config/persona.txt
    config/appearance-policy-v2.json
    config/emperor-tarot-v1.json
    config/tarot-house-v1.json
    DESTINATION config
)
install(DIRECTORY migrations/
    DESTINATION migrations
    FILES_MATCHING PATTERN "*.sql"
)
install(PROGRAMS
    scripts/sanguinius-backup.bash
    scripts/sanguinius-restore.bash
    DESTINATION libexec
)
install(FILES
    packaging/systemd/sanguinius.service
    packaging/systemd/sanguinius-compat.service
    packaging/systemd/sanguinius-backup.service
    packaging/systemd/sanguinius-backup.timer
    DESTINATION systemd
)
install(FILES packaging/sysusers.d/sanguinius.conf DESTINATION sysusers.d)
install(FILES packaging/tmpfiles.d/sanguinius.conf DESTINATION tmpfiles.d)
install(FILES docs/OPERATIONS.md DESTINATION docs)
