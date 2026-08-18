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

find_package(dpp CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(Threads REQUIRED)

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
sanguinius_embed_migration(
    "${CMAKE_CURRENT_SOURCE_DIR}/migrations/0001_core_foundation.sql"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_migrations.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embedded_migration.cpp.in"
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
    src/application.cpp
    src/build_info.cpp
    src/clock.cpp
    src/config.cpp
    src/diagnostics.cpp
    src/feature_config.cpp
    src/health.cpp
    src/id_generator.cpp
    src/message_handler.cpp
    src/message_logger.cpp
    src/owner_admin.cpp
    src/persistent_id.cpp
    src/repositories.cpp
    src/server_scope_policy.cpp
    src/snowflake.cpp
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
)

add_library(sanguinius_persistence STATIC
    "${CMAKE_CURRENT_BINARY_DIR}/generated/embedded_migrations.cpp"
    src/database_cli.cpp
    src/persistence/backup.cpp
    src/persistence/database.cpp
    src/persistence/migrator.cpp
    src/persistence/sqlite.cpp
    src/persistence/sqlite_repositories.cpp
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
)

add_library(sanguinius_runtime STATIC
    src/composition_root.cpp
    src/dpp_discord_adapter.cpp
    src/openai_client.cpp
    src/process_signals.cpp
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
        tests/unit/config_tests.cpp
        tests/unit/command_parse_tests.cpp
        tests/unit/health_tests.cpp
        tests/unit/mention_parse_tests.cpp
        tests/unit/message_logger_tests.cpp
        tests/unit/owner_admin_tests.cpp
        tests/unit/server_scope_policy_tests.cpp
        tests/unit/snowflake_tests.cpp
        tests/unit/work_queue_tests.cpp
    )
    add_sanguinius_test(sanguinius_application_tests
        tests/application/application_flow_tests.cpp
        tests/application/failure_and_shutdown_tests.cpp
    )

    add_executable(sanguinius_persistence_tests
        tests/integration/backup_tests.cpp
        tests/integration/database_cli_tests.cpp
        tests/integration/migration_tests.cpp
        tests/integration/repository_tests.cpp
        tests/integration/sqlite_tests.cpp
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

    set(sanguinius_config_environment_unsets
        --unset=SANGUINIUS_TOKEN
        --unset=SANGUINIUS_TOKEN_FILE
        --unset=OPENAI_API_KEY
        --unset=SANGUINIUS_OPENAI_API_KEY_FILE
        --unset=SANGUINIUS_LOG_FILE
        --unset=SANGUINIUS_COMMAND_PREFIX
        --unset=SANGUINIUS_OPENAI_MODEL
        --unset=SANGUINIUS_PERSONA_FILE
        --unset=SANGUINIUS_DATABASE_FILE
        --unset=SANGUINIUS_ADMIN_COMMANDS_ENABLED
        --unset=SANGUINIUS_TEST_MODE
        --unset=SANGUINIUS_CHRONICLE_ENABLED
        --unset=SANGUINIUS_TAROT_ENABLED
        --unset=SANGUINIUS_APPEARANCES_MODE
        --unset=SANGUINIUS_VOX_ENABLED
        --unset=SANGUINIUS_VOICE_INPUT_ENABLED
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
