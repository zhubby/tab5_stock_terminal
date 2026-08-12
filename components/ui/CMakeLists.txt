idf_component_register(
    SRCS "terminal_ui.cpp"
    INCLUDE_DIRS "include"
    REQUIRES auth longbridge quotes settings lvgl
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
