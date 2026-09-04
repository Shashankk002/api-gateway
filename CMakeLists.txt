cmake_minimum_required(VERSION 3.20)

project(api_gateway
    VERSION 0.1.0
    DESCRIPTION "A minimal HTTP API gateway"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Only the top-level project decides on testing and export of compile commands.
if(PROJECT_IS_TOP_LEVEL)
    option(API_GATEWAY_BUILD_TESTS "Build the api-gateway test suite" ON)
    set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
else()
    option(API_GATEWAY_BUILD_TESTS "Build the api-gateway test suite" OFF)
endif()

include(cmake/Dependencies.cmake)

# Warning flags applied to first-party targets only, never to fetched sources.
add_library(api_gateway_warnings INTERFACE)
if(MSVC)
    target_compile_options(api_gateway_warnings INTERFACE /W4)
else()
    target_compile_options(api_gateway_warnings INTERFACE
        -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion)
endif()

# Core library: everything except the process entry point, so that tests can
# link the real server rather than a copy of it.
add_library(api_gateway_core
    src/config.cpp
    src/load_balancer.cpp
    src/proxy.cpp
    src/router.cpp
    src/server.cpp)
add_library(api_gateway::core ALIAS api_gateway_core)

target_include_directories(api_gateway_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_link_libraries(api_gateway_core
    PUBLIC httplib::httplib
    PRIVATE api_gateway_warnings)

add_executable(api_gateway src/main.cpp)
set_target_properties(api_gateway PROPERTIES OUTPUT_NAME api-gateway)
target_link_libraries(api_gateway PRIVATE api_gateway::core api_gateway_warnings)

if(API_GATEWAY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
