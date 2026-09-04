# Third-party dependencies are fetched at configure time and pinned to exact
# tags so that every machine builds the same sources.
include(FetchContent)

set(FETCHCONTENT_QUIET OFF)

# cpp-httplib: header-only HTTP server/client used for the gateway listener and
# for the integration tests' client side.
set(HTTPLIB_COMPILE OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_OPENSSL OFF CACHE BOOL "" FORCE)
set(HTTPLIB_REQUIRE_ZLIB OFF CACHE BOOL "" FORCE)
set(HTTPLIB_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(httplib
    GIT_REPOSITORY https://github.com/yhirose/cpp-httplib.git
    GIT_TAG v0.18.7
    GIT_SHALLOW ON)

# hiredis: minimal C client for the Redis-backed rate limiter. Chosen over a
# heavier C++ wrapper because the gateway only needs EVALSHA/EVAL.
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(DISABLE_TESTS ON CACHE BOOL "" FORCE)
set(ENABLE_SSL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(hiredis
    GIT_REPOSITORY https://github.com/redis/hiredis.git
    GIT_TAG v1.4.1
    GIT_SHALLOW ON)

if(API_GATEWAY_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(httplib hiredis googletest)
else()
    FetchContent_MakeAvailable(httplib hiredis)
endif()

# hiredis headers are C and trip the project's strict warnings, so consumers see
# them as system headers.
get_target_property(hiredis_include_dirs hiredis INTERFACE_INCLUDE_DIRECTORIES)
set_target_properties(hiredis PROPERTIES
    INTERFACE_SYSTEM_INCLUDE_DIRECTORIES "${hiredis_include_dirs}")
