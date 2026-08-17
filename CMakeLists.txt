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
    src/config.cpp
    src/message_handler.cpp
    src/message_logger.cpp
    src/openai_client.cpp
)

target_compile_features(sanguinius_core PUBLIC cxx_std_20)
target_include_directories(sanguinius_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
sanguinius_enable_warnings(sanguinius_core)
target_link_libraries(sanguinius_core
    PUBLIC
        dpp::dpp
        Threads::Threads
    PRIVATE
        CURL::libcurl
        nlohmann_json::nlohmann_json
)

add_executable(sanguinius src/main.cpp)
sanguinius_enable_warnings(sanguinius)
target_link_libraries(sanguinius PRIVATE sanguinius_core)

if(BUILD_TESTING)
    add_executable(sanguinius_tests tests/sanguinius_tests.cpp)
    sanguinius_enable_warnings(sanguinius_tests)
    target_link_libraries(sanguinius_tests PRIVATE sanguinius_core)
    add_test(NAME sanguinius_tests COMMAND sanguinius_tests)
endif()

install(TARGETS sanguinius RUNTIME DESTINATION bin)
