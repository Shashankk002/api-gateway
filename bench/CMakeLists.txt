# Benchmark-only targets. Nothing here is linked into api_gateway_core or the
# api-gateway binary.
add_executable(bench_backend bench_backend.cpp)
set_target_properties(bench_backend PROPERTIES OUTPUT_NAME bench-backend)
target_link_libraries(bench_backend PRIVATE httplib::httplib api_gateway_warnings)
