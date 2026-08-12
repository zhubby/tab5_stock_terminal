idf_component_register(
    SRCS "settings_store.cpp"
    INCLUDE_DIRS "include"
    REQUIRES auth longbridge quotes nvs_flash
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
