include(GoogleTest)

add_executable(api_gateway_tests
    config_test.cpp
    health_endpoint_test.cpp
    load_balancer_test.cpp
    load_balancing_integration_test.cpp
    proxy_integration_test.cpp
    router_test.cpp
    routing_integration_test.cpp)

target_link_libraries(api_gateway_tests
    PRIVATE api_gateway::core api_gateway_warnings GTest::gtest_main)

gtest_discover_tests(api_gateway_tests)
