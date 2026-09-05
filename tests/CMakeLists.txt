include(GoogleTest)

add_executable(api_gateway_tests
    circuit_breaker_test.cpp
    config_test.cpp
    health_check_integration_test.cpp
    health_check_test.cpp
    health_endpoint_test.cpp
    load_balancer_test.cpp
    lifecycle_stress_test.cpp
    load_balancing_integration_test.cpp
    metrics_integration_test.cpp
    metrics_test.cpp
    middleware_test.cpp
    pipeline_integration_test.cpp
    proxy_integration_test.cpp
    rate_limit_integration_test.cpp
    rate_limiter_test.cpp
    redis_rate_limit_test.cpp
    reliability_integration_test.cpp
    router_test.cpp
    system_failure_test.cpp
    routing_integration_test.cpp)

target_link_libraries(api_gateway_tests
    PRIVATE api_gateway::core api_gateway_warnings GTest::gtest_main)

gtest_discover_tests(api_gateway_tests)
