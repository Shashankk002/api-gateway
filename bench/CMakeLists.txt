# Benchmark-only targets. Nothing here is linked into api_gateway_core or the
# api-gateway binary.
add_executable(bench_backend bench_backend.cpp)
set_target_properties(bench_backend PROPERTIES OUTPUT_NAME bench-backend)
target_link_libraries(bench_backend PRIVATE httplib::httplib api_gateway_warnings)

# Latency attribution probe. Links the gateway library so it can time the real
# ReverseProxy::forward; benchmark-only, never part of the gateway binary.
add_executable(proxy_probe proxy_probe.cpp)
set_target_properties(proxy_probe PROPERTIES OUTPUT_NAME proxy-probe)
target_link_libraries(proxy_probe PRIVATE api_gateway::core api_gateway_warnings)
