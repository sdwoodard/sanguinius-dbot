cmake_minimum_required(VERSION 3.25)

project(sanguinius VERSION 2.1.0 LANGUAGES CXX)

include(CTest)

set(SANGUINIUS_REVISION_OVERRIDE "" CACHE STRING
    "Git revision to embed instead of detecting the current checkout")
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
    src/pending_notice.cpp
    src/persistent_id.cpp
    src/prompt_compiler.cpp
    src/random.cpp
    src/relationships.cpp
    src/repositories.cpp
    src/scheduler.cpp
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
    src/wagers.cpp
    src/vox.cpp
    src/vox_narration.cpp
    src/work_queue.cpp
)

target_compile_features(sanguinius_core PUBLIC cxx_std_20)
target_include_directories(sanguinius_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_compile_definitions(sanguinius_core PUBLIC
    SANGUINIUS_BUILD_VERSION="${PROJECT_VERSION}"
    SANGUINIUS_BUILD_REVISION="${sanguinius_build_revision}"
)
sanguinius_enable_warnings(sanguinius_core)
target_link_libraries(sanguinius_core
    PUBLIC
        Threads::Threads
        OpenSSL::Crypto
    PRIVATE
        nlohmann_json::nlohmann_json
)

add_library(sanguinius_persistence STATIC
    "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_migrations.cpp"
    src/database_cli.cpp
    src/persistence/backup.cpp
    src/persistence/database.cpp
    src/persistence/migrator.cpp
    src/persistence/sqlite.cpp
    src/persistence/sqlite_chronicle_repository.cpp
    src/persistence/sqlite_chronicle_session_repository.cpp
    src/persistence/sqlite_appearance_repository.cpp
    src/persistence/sqlite_durable_work_repository.cpp
    src/persistence/sqlite_relationship_repository.cpp
    src/persistence/sqlite_speech_repository.cpp
    src/persistence/sqlite_repositories.cpp
    src/persistence/sqlite_tarot_repository.cpp
    src/persistence/sqlite_tarot_catalog_repository.cpp
    src/persistence/sqlite_tarot_draw_repository.cpp
    src/persistence/sqlite_tarot_house_repository.cpp
    src/persistence/sqlite_tarot_integration_repository.cpp
    src/persistence/sqlite_wager_repository.cpp
    src/persistence/sqlite_vox_repository.cpp
    src/persistence/sqlite_vox_narration_repository.cpp
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
    src/ffmpeg_audio_normalizer.cpp
    src/openai_client.cpp
    src/openai_tts_client.cpp
    src/process_signals.cpp
    src/tts_cache.cpp
)

target_compile_features(sanguinius_runtime PUBLIC cxx_std_20)
target_include_directories(sanguinius_runtime PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
sanguinius_enable_warnings(sanguinius_runtime)
target_link_libraries(sanguinius_runtime
    PUBLIC
        sanguinius_core
        sanguinius_persistence
    PRIVATE
        dpp::dpp
        CURL::libcurl
        nlohmann_json::nlohmann_json
)

add_executable(sanguinius src/main.cpp)
sanguinius_enable_warnings(sanguinius)
target_link_libraries(sanguinius PRIVATE sanguinius_runtime)
set_target_properties(sanguinius PROPERTIES INSTALL_RPATH_USE_LINK_PATH TRUE)

if(BUILD_TESTING)
    find_package(Catch2 3 CONFIG REQUIRED)
    include(Catch)

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
            PROPERTIES TIMEOUT 10
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
        tests/integration/repository_tests.cpp
        tests/integration/relationship_repository_tests.cpp
        tests/integration/speech_repository_tests.cpp
        tests/integration/sqlite_tests.cpp
        tests/integration/tarot_repository_tests.cpp
        tests/integration/tarot_house_repository_tests.cpp
        tests/integration/wager_repository_tests.cpp
        tests/integration/vox_repository_tests.cpp
        tests/integration/vox_narration_repository_tests.cpp
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
        PROPERTIES TIMEOUT 10
    )

    add_executable(sanguinius_runtime_tests
        tests/runtime/dpp_command_registration_tests.cpp
        tests/runtime/tts_runtime_tests.cpp
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
        PROPERTIES TIMEOUT 10
    )

    set(sanguinius_config_environment_unsets
        --unset=SANGUINIUS_TOKEN
        --unset=SANGUINIUS_TOKEN_FILE
        --unset=OPENAI_API_KEY
        --unset=SANGUINIUS_OPENAI_API_KEY_FILE
        --unset=SANGUINIUS_LOG_FILE
        --unset=SANGUINIUS_COMMAND_PREFIX
        --unset=SANGUINIUS_OPENAI_MODEL
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
        --unset=SANGUINIUS_ADMIN_COMMANDS_ENABLED
        --unset=SANGUINIUS_TEST_MODE
        --unset=SANGUINIUS_CHRONICLE_ENABLED
        --unset=SANGUINIUS_TAROT_ENABLED
        --unset=SANGUINIUS_APPEARANCES_MODE
        --unset=SANGUINIUS_VOX_ENABLED
        --unset=SANGUINIUS_VOX_NARRATION_ENABLED
        --unset=SANGUINIUS_VOICE_INPUT_ENABLED
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
            SANGUINIUS_PERSONA_FILE=${CMAKE_CURRENT_SOURCE_DIR}/config/persona.txt
            SANGUINIUS_APPEARANCE_POLICY_FILE=${CMAKE_CURRENT_SOURCE_DIR}/config/appearance-policy-v2.json
            SANGUINIUS_DISCORD_REQUEST_TIMEOUT_SECONDS=1
            "$<TARGET_FILE:sanguinius>" --check-config
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
    DESTINATION share/sanguinius/vox
    FILES_MATCHING
    PATTERN "*.pcm"
    PATTERN "fallbacks-v1.json"
)
