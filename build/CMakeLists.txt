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

# Add cURLpp library
find_library(CURLPP_LIBRARY curlpp)
find_path(CURLPP_INCLUDE_DIR curlpp/cURLpp.hpp)
find_package(CURL REQUIRED)

# Find RapidJSON package
find_package(RapidJSON REQUIRED)

# Link libraries and include directories
target_link_libraries(sanguinius_dbot PRIVATE ${DPP_LIBRARY} ${Boost_LIBRARIES} ${CURL_LIBRARIES} ${CURLPP_LIBRARY} -lboost_log_setup)
target_include_directories(sanguinius_dbot PRIVATE ${CURLPP_INCLUDE_DIR} ${CURL_INCLUDE_DIRS} ${RapidJSON_INCLUDE_DIRS})

# Set the output directory
set_target_properties(sanguinius_dbot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "../bin")
