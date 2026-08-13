cmake_minimum_required(VERSION 3.16)
project(tab5_stock_terminal_host_tests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(host_tests
    ../../components/auth/sha256.cpp
    ../../components/longbridge/endpoint.cpp
    ../../components/longbridge/api_auth.cpp
    ../../components/longbridge/protocol.cpp
    ../../components/longbridge/quote_codec.cpp
    ../../components/quotes/quote_store.cpp
    ../../components/quotes/symbol.cpp
    ../../components/sd_file_manager/sd_file_manager.cpp
    ../../components/settings/settings_file.cpp
    test_main.cpp
)

target_compile_definitions(host_tests PRIVATE TAB5_HOST_TEST=1)
target_include_directories(host_tests PRIVATE
    ../../components/auth/include
    ../../components/longbridge/include
    ../../components/quotes/include
    ../../components/sd_file_manager/include
    ../../components/settings/include
)
