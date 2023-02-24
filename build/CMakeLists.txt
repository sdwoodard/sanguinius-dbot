cmake_minimum_required(VERSION 3.25)

set(CMAKE_CXX_STANDARD 20)

project(sanguinius_dbot)

# Add local dpp library
find_library(DPP_LIBRARY NAMES dpp PATHS /usr/local/lib)
include_directories(/usr/local/include)

# Add boost library
find_package(Boost REQUIRED COMPONENTS log system)
include_directories(${Boost_INCLUDE_DIRS})
add_definitions(-DBOOST_LOG_DYN_LINK)
add_definitions(-DBOOST_LOG_USE_CHAR)

# Add project sources
file(GLOB SOURCES "../src/*.cpp")

# Add project include directories
include_directories(../inc)

# Create executable
add_executable(sanguinius_dbot ${SOURCES})
target_link_libraries(sanguinius_dbot ${DPP_LIBRARY} ${Boost_LIBRARIES} -lboost_log_setup)

# Set the output directory
set_target_properties(sanguinius_dbot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "../bin")