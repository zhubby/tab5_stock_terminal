idf_component_register(
    SRCS "symbol.cpp" "quote_store.cpp"
    INCLUDE_DIRS "include"
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
