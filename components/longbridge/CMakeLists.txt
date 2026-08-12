idf_component_register(
    SRCS "endpoint.cpp" "api_auth.cpp" "protocol.cpp" "quote_codec.cpp" "longbridge_client.cpp"
    INCLUDE_DIRS "include"
    REQUIRES auth quotes esp_http_client esp_websocket_client json esp_timer esp-tls
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
