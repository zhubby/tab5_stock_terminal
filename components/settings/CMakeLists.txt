idf_component_register(
    SRCS "settings_store.cpp" "settings_file.cpp"
    INCLUDE_DIRS "include"
    REQUIRES longbridge quotes nvs_flash
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
