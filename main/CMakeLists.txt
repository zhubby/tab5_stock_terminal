idf_component_register(
    SRCS "app_main.cpp"
    INCLUDE_DIRS "."
    REQUIRES auth longbridge quotes settings ui esp_event esp_netif esp_wifi esp_timer lwip mdns nvs_flash
             m5stack_tab5 esp_lcd esp_lcd_st7123 esp_lcd_st7121
             esp_lcd_touch_gt911 esp_lcd_touch_st7123 esp_lvgl_port esp_hosted
)

target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_17)
