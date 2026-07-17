cmake_minimum_required(VERSION 3.25)

project(sanguinius VERSION 2.0.0 LANGUAGES CXX)

include(CTest)

find_package(dpp CONFIG REQUIRED)
find_package(Threads REQUIRED)

add_library(sanguinius_core STATIC
    src/config.cpp
    src/message_handler.cpp
    src/message_logger.cpp
)

target_compile_features(sanguinius_core PUBLIC cxx_std_20)
target_include_directories(sanguinius_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(sanguinius_core PUBLIC dpp::dpp Threads::Threads)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(sanguinius_core PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wshadow)
elseif(MSVC)
    target_compile_options(sanguinius_core PRIVATE /W4 /permissive-)
endif()

add_executable(sanguinius src/main.cpp)
target_link_libraries(sanguinius PRIVATE sanguinius_core)

if(BUILD_TESTING)
    add_executable(sanguinius_tests tests/sanguinius_tests.cpp)
    target_link_libraries(sanguinius_tests PRIVATE sanguinius_core)
    add_test(NAME sanguinius_tests COMMAND sanguinius_tests)
endif()

install(TARGETS sanguinius RUNTIME DESTINATION bin)
