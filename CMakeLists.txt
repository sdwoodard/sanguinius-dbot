cmake_minimum_required(VERSION 3.25)

project(sanguinius VERSION 2.1.0 LANGUAGES CXX)

include(CTest)

find_package(dpp CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Threads REQUIRED)

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
    src/clock.cpp
    src/diagnostics.cpp
    src/id_generator.cpp
    src/message_handler.cpp
    src/message_logger.cpp
    src/work_queue.cpp
)

target_compile_features(sanguinius_core PUBLIC cxx_std_20)
target_include_directories(sanguinius_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
sanguinius_enable_warnings(sanguinius_core)
target_link_libraries(sanguinius_core
    PUBLIC
        Threads::Threads
)

add_library(sanguinius_runtime STATIC
    src/composition_root.cpp
    src/config.cpp
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
        tests/unit/command_parse_tests.cpp
        tests/unit/mention_parse_tests.cpp
        tests/unit/message_logger_tests.cpp
        tests/unit/work_queue_tests.cpp
    )
    add_sanguinius_test(sanguinius_application_tests
        tests/application/application_flow_tests.cpp
        tests/application/failure_and_shutdown_tests.cpp
    )
endif()

install(TARGETS sanguinius RUNTIME DESTINATION bin)
