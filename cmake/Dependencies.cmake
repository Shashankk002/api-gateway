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

if(API_GATEWAY_BUILD_TESTS)
    set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.15.2
        GIT_SHALLOW ON)
    FetchContent_MakeAvailable(httplib googletest)
else()
    FetchContent_MakeAvailable(httplib)
endif()
