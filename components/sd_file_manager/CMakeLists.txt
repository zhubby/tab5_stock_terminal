idf_component_register(
    SRCS "sd_file_manager.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server esp_hw_support m5stack_tab5
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
