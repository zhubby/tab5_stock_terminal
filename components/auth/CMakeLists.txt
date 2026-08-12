idf_component_register(
    SRCS "oauth.cpp" "oauth_callback_server.cpp" "sha256.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_timer
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
