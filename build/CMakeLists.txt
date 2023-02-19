cmake_minimum_required(VERSION 3.25)

set(CMAKE_CXX_STANDARD 20)

project(sanguinius_dbot)

# Add dpp library
find_library(DPP_LIBRARY NAMES dpp PATHS /usr/local/lib)
include_directories(/usr/local/include)

# Add sources
file(GLOB SOURCES "../src/*.cpp")

# Include directories
include_directories(../inc)

# Create executable
add_executable(sanguinius_dbot ${SOURCES})
target_link_libraries(sanguinius_dbot ${DPP_LIBRARY})

# Set the output directory
set_target_properties(sanguinius_dbot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "../bin")