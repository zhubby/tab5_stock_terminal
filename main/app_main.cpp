#include "longbridge/longbridge_client.hpp"
#include "quotes/quote_store.hpp"
#include "sd_file_manager/sd_file_manager.hpp"
#include "settings/settings_file.hpp"
#include "settings/settings_store.hpp"
#include "ui/terminal_ui.hpp"

#include <atomic>
#include <array>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cinttypes>
#include <ctime>
#include <algorithm>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_io_i2c.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_sntp.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "mdns.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"

#if __has_include("bsp/esp-bsp.h")
#include "bsp/esp-bsp.h"
#endif
#if __has_include("esp_lcd_st7123.h")
#include "esp_lcd_st7123.h"
#define TAB5_HAS_ST7123_DRIVER 1
#endif
#if __has_include("esp_lcd_st7121.h")
#include "esp_lcd_st7121.h"
#define TAB5_HAS_ST7121_DRIVER 1
#endif
#if __has_include("esp_lcd_touch_gt911.h") && __has_include("esp_lcd_touch_st7123.h")
#include "esp_lcd_touch_gt911.h"
#include "esp_lcd_touch_st7123.h"
#define TAB5_HAS_TOUCH_PROBE 1
#endif
#if __has_include("esp_hosted.h")
#include "esp_hosted.h"
#include "esp_hosted_event.h"
#define TAB5_HAS_ESP_HOSTED 1
#endif
#if __has_include("esp_lvgl_port.h")
#include "esp_lvgl_port.h"
#endif

namespace {

constexpr const char* kTag = "tab5-stock";
constexpr std::int64_t kQuoteStaleAfterMs = 20'000;
constexpr std::int64_t kLiveSnapshotRefreshMs = 60'000;
constexpr std::size_t kSdSettingsFileMaxBytes = 32 * 1024;
constexpr bool kForceSt7123DisplayDiagnostic = false;
constexpr bool kForceSt7121Display = true;
constexpr bool kHoldLvglDisplayDiagnostic = false;
constexpr bool kHoldBspHardwarePatternDiagnostic = false;
constexpr bool kHoldSt7123DriverPatternDiagnostic = false;
constexpr bool kHoldSt7121DriverPatternDiagnostic = false;
constexpr bool kShowSt7123HardwareColorBars = false;
constexpr bool kShowRawPanelDiagnostic = false;
constexpr bool kHoldRawPanelDiagnostic = false;
constexpr std::uint8_t kTab5KeyboardAddress = 0x6d;
constexpr i2c_port_num_t kTab5KeyboardI2CPort = I2C_NUM_1;
constexpr gpio_num_t kTab5KeyboardSda = GPIO_NUM_0;
constexpr gpio_num_t kTab5KeyboardScl = GPIO_NUM_1;
constexpr gpio_num_t kTab5KeyboardInt = GPIO_NUM_50;
constexpr std::uint32_t kTab5KeyboardI2CFrequencyHz = 400'000;
constexpr std::uint8_t kTab5KeyboardRegInterruptConfig = 0x00;
constexpr std::uint8_t kTab5KeyboardRegInterruptStatus = 0x01;
constexpr std::uint8_t kTab5KeyboardRegEventCount = 0x02;
constexpr std::uint8_t kTab5KeyboardRegMode = 0x10;
constexpr std::uint8_t kTab5KeyboardRegKeyEvent = 0x20;
constexpr std::uint8_t kTab5KeyboardRegHidEvent = 0x30;
constexpr std::uint8_t kTab5KeyboardRegCharEventLength = 0x40;
constexpr std::uint8_t kTab5KeyboardRegCharEvent = 0x50;
constexpr std::uint8_t kTab5KeyboardRegFirmwareVersion = 0xfe;
constexpr std::uint8_t kTab5KeyboardInterruptKey = 0x01;
constexpr std::uint8_t kTab5KeyboardInterruptChar = 0x04;
constexpr std::uint8_t kTab5KeyboardStatusKey = 0x01;
constexpr std::uint8_t kTab5KeyboardStatusHid = 0x02;
constexpr std::uint8_t kTab5KeyboardStatusChar = 0x04;
constexpr std::uint8_t kTab5KeyboardModeKey = 0x00;
constexpr std::uint8_t kTab5KeyboardModeHid = 0x01;
constexpr std::uint8_t kTab5KeyboardModeChar = 0x02;
constexpr std::uint8_t kTab5KeyboardEmptyEvent = 0xff;
constexpr std::uint8_t kTab5KeyboardMaxEvents = 32;
constexpr std::size_t kTab5KeyboardCharEventMaxBytes = 17;
constexpr std::size_t kTab5KeyboardQueueSize = 96;
constexpr EventBits_t kWifiConnectedBit = BIT0;
constexpr EventBits_t kWifiFailedBit = BIT1;
constexpr EventBits_t kHostedTransportUpBit = BIT2;

enum class KeyboardStatus : std::uint8_t {
    Wait,
    Ready,
    Input,
    Lost,
};

struct Tab5KeyboardInputContext {
    QueueHandle_t key_queue { nullptr };
    std::uint32_t last_key { 0 };
    bool release_pending { false };
    std::uint32_t logged_key_count { 0 };
    std::uint32_t logged_raw_event_count { 0 };
};

tab5::settings::SettingsStore g_settings_store;
tab5::settings::AppSettings g_settings;
tab5::quotes::QuoteStore g_quote_store;
tab5::ui::TerminalUi g_ui;
tab5::longbridge::LongbridgeClient g_longbridge;
tab5::sd_file_manager::SdFileManagerServer g_sd_file_manager_server;
EventGroupHandle_t g_wifi_events { nullptr };
SemaphoreHandle_t g_quote_delta_mutex { nullptr };
std::map<std::string, tab5::quotes::QuoteDelta> g_pending_quote_deltas;
std::uint32_t g_dropped_quote_delta_count { 0 };
bool g_netif_initialized { false };
bool g_wifi_initialized { false };
bool g_wifi_started { false };
bool g_sntp_started { false };
bool g_mdns_started { false };
bool g_hosted_started { false };
bool g_hosted_event_handler_registered { false };
lv_display_t* g_display { nullptr };
i2c_master_bus_handle_t g_tab5_keyboard_bus { nullptr };
i2c_master_dev_handle_t g_tab5_keyboard_device { nullptr };
lv_indev_t* g_tab5_keyboard_indev { nullptr };
QueueHandle_t g_tab5_keyboard_intr_queue { nullptr };
Tab5KeyboardInputContext g_tab5_keyboard_input;
bool g_tab5_keyboard_ready { false };
bool g_tab5_keyboard_absent_logged { false };
std::uint32_t g_tab5_keyboard_i2c_error_count { 0 };
std::uint32_t g_tab5_keyboard_poll_count { 0 };
std::uint32_t g_tab5_keyboard_irq_count { 0 };
std::int64_t g_tab5_keyboard_last_heartbeat_ms { 0 };
int g_tab5_keyboard_last_int_level { -1 };
std::uint8_t g_tab5_keyboard_last_status { 0xff };
bool g_tab5_keyboard_sym_active { false };
bool g_tab5_keyboard_aa_active { false };
std::string g_stream_watchlist_key;
tab5::longbridge::BackoffPolicy g_wifi_backoff { 2'000, 60'000, 2 };
tab5::longbridge::BackoffPolicy g_quote_retry_backoff { 3'000, 60'000, 2 };
std::int64_t g_next_wifi_reconnect_ms { 0 };
std::int64_t g_next_quote_refresh_ms { 0 };
bool g_wifi_online { false };
bool g_quote_stream_online { false };
std::string g_boot_warning;
std::string g_connection_status { "offline" };
std::string g_sd_file_manager_status;
bool g_sd_file_manager_mdns_advertised { false };
std::atomic<KeyboardStatus> g_keyboard_status { KeyboardStatus::Wait };
std::string g_rendered_status;

bool refresh_quotes();
void refresh_quotes_and_schedule();

void refresh_status_line()
{
    std::string status = g_connection_status;
    status += " | ";
    switch (g_keyboard_status.load(std::memory_order_relaxed)) {
    case KeyboardStatus::Ready:
        status += "kbd ready";
        break;
    case KeyboardStatus::Input:
        status += "kbd input";
        break;
    case KeyboardStatus::Lost:
        status += "kbd lost";
        break;
    case KeyboardStatus::Wait:
    default:
        status += "kbd wait";
        break;
    }
    if (!g_sd_file_manager_status.empty()) {
        status += " | ";
        status += g_sd_file_manager_status;
    }
    if (g_rendered_status == status) {
        return;
    }
    g_ui.set_connection_status(status);
    g_rendered_status = std::move(status);
}

void set_connection_status(std::string status)
{
    g_connection_status = std::move(status);
    refresh_status_line();
}

void set_keyboard_status(KeyboardStatus status)
{
    g_keyboard_status.store(status, std::memory_order_relaxed);
}

enum class Tab5PanelKind {
    Unknown,
    Ili9881c,
    St7121,
    St7123,
};

Tab5PanelKind detect_tab5_panel_kind()
{
#if defined(TAB5_HAS_TOUCH_PROBE) && __has_include("bsp/esp-bsp.h")
    const esp_err_t i2c_err = bsp_i2c_init();
    if (i2c_err != ESP_OK) {
        ESP_LOGW(kTag, "Tab5 panel probe skipped: I2C init failed: %s", esp_err_to_name(i2c_err));
        return Tab5PanelKind::Unknown;
    }

    const esp_err_t touch_power_err = bsp_feature_enable(BSP_FEATURE_TOUCH, true);
    if (touch_power_err != ESP_OK) {
        ESP_LOGW(kTag,
                 "Tab5 panel probe skipped: touch power enable failed: %s",
                 esp_err_to_name(touch_power_err));
        return Tab5PanelKind::Unknown;
    }

    vTaskDelay(pdMS_TO_TICKS(500));
    const esp_err_t st7123_probe =
        i2c_master_probe(bsp_i2c_get_handle(), ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS, 100);
    const esp_err_t gt911_probe =
        i2c_master_probe(bsp_i2c_get_handle(), ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 100);

    ESP_LOGI(kTag,
             "Tab5 touch probe: ST7123@0x55=%s, GT911@0x14=%s",
             esp_err_to_name(st7123_probe),
             esp_err_to_name(gt911_probe));

    if (st7123_probe == ESP_OK) {
        esp_lcd_panel_io_handle_t touch_io = nullptr;
        esp_lcd_panel_io_i2c_config_t touch_io_config {};
        touch_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_ST7123_ADDRESS;
        touch_io_config.control_phase_bytes = 1;
        touch_io_config.lcd_cmd_bits = 16;
        touch_io_config.flags.disable_control_phase = 1;
        touch_io_config.scl_speed_hz = 100000;
        const esp_err_t touch_io_err =
            esp_lcd_new_panel_io_i2c(bsp_i2c_get_handle(), &touch_io_config, &touch_io);
        if (touch_io_err == ESP_OK) {
            std::uint8_t fw_version = 0xff;
            const esp_err_t fw_err = esp_lcd_panel_io_rx_param(touch_io, 0x0000, &fw_version, 1);
            ESP_LOGI(kTag,
                     "Tab5 ST712x touch firmware probe: %s, version=%u",
                     esp_err_to_name(fw_err),
                     static_cast<unsigned>(fw_version));
            ESP_ERROR_CHECK_WITHOUT_ABORT(esp_lcd_panel_io_del(touch_io));
            if (fw_err == ESP_OK && fw_version == 1) {
                ESP_LOGI(kTag, "detected Tab5 ST7121 panel");
                return Tab5PanelKind::St7121;
            }
            if (fw_err == ESP_OK && fw_version == 3) {
                ESP_LOGI(kTag, "detected Tab5 ST7123 panel");
                return Tab5PanelKind::St7123;
            }
        } else {
            ESP_LOGW(kTag,
                     "Tab5 ST712x touch firmware probe failed: %s",
                     esp_err_to_name(touch_io_err));
        }

        ESP_LOGI(kTag, "detected Tab5 0x55 touch controller; falling back to ST7123 panel");
        return Tab5PanelKind::St7123;
    }
    if (gt911_probe == ESP_OK) {
        ESP_LOGI(kTag, "detected Tab5 ILI9881C panel");
        return Tab5PanelKind::Ili9881c;
    }
#endif

    ESP_LOGW(kTag, "Tab5 panel kind is unknown; falling back to BSP display path");
    return Tab5PanelKind::Unknown;
}

void pause_display_refresh()
{
    if (!g_display) {
        return;
    }
    lv_timer_t* refresh_timer = lv_display_get_refr_timer(g_display);
    if (refresh_timer) {
        lv_timer_pause(refresh_timer);
    }
    lv_display_enable_invalidation(g_display, false);
}

void resume_display_refresh(lv_obj_t* screen)
{
    if (!g_display) {
        return;
    }
    lv_display_enable_invalidation(g_display, true);
    if (screen) {
        lv_obj_invalidate(screen);
    }
    lv_timer_t* refresh_timer = lv_display_get_refr_timer(g_display);
    if (refresh_timer) {
        lv_timer_resume(refresh_timer);
        lv_timer_ready(refresh_timer);
    }
}

void hold_lvgl_display_diagnostic(lv_obj_t* screen)
{
    if (!screen) {
        return;
    }

    struct DiagnosticColor {
        const char* name;
        std::uint32_t hex;
    };
    constexpr DiagnosticColor colors[] = {
        { "white", 0xffffff },
        { "red", 0xff0000 },
        { "green", 0x00ff00 },
        { "blue", 0x0000ff },
        { "yellow", 0xffff00 },
        { "magenta", 0xff00ff },
        { "cyan", 0x00ffff },
    };

    ESP_LOGW(kTag, "holding LVGL full-screen diagnostic; firmware will not continue to UI");
    std::size_t color_index = 0;
    while (true) {
        const DiagnosticColor& color = colors[color_index % (sizeof(colors) / sizeof(colors[0]))];
        ESP_LOGI(kTag, "LVGL full-screen diagnostic fill: %s", color.name);
        if (bsp_display_lock(5000)) {
            lv_obj_clean(screen);
            lv_obj_set_style_bg_color(screen, lv_color_hex(color.hex), LV_PART_MAIN);
            lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, LV_PART_MAIN);
            lv_obj_invalidate(screen);
            bsp_display_unlock();
        } else {
            ESP_LOGW(kTag, "LVGL diagnostic could not lock display");
        }
        resume_display_refresh(screen);
        vTaskDelay(pdMS_TO_TICKS(2000));
        pause_display_refresh();
        ++color_index;
    }
}

#if __has_include("bsp/esp-bsp.h")
void hold_bsp_hardware_pattern_diagnostic()
{
    bsp_lcd_handles_t handles {};
    bsp_display_config_t config {};
    config.dsi_bus.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    config.dsi_bus.lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS;

    ESP_LOGW(kTag, "holding BSP hardware DSI pattern diagnostic; firmware will not continue to UI");
    ESP_ERROR_CHECK(bsp_display_new_with_handles(&config, &handles));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(handles.panel, true));
    ESP_ERROR_CHECK(bsp_display_backlight_on());

    constexpr mipi_dsi_pattern_type_t patterns[] = {
        MIPI_DSI_PATTERN_BAR_VERTICAL,
        MIPI_DSI_PATTERN_BAR_HORIZONTAL,
        MIPI_DSI_PATTERN_BER_VERTICAL,
    };
    constexpr const char* names[] = {
        "vertical bars",
        "horizontal bars",
        "vertical BER",
    };

    std::size_t pattern_index = 0;
    while (true) {
        const std::size_t index = pattern_index % (sizeof(patterns) / sizeof(patterns[0]));
        ESP_LOGI(kTag, "BSP hardware DSI pattern: %s", names[index]);
        ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(handles.panel, patterns[index]));
        ++pattern_index;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
#endif

#if defined(TAB5_HAS_ST7121_DRIVER) && __has_include("bsp/esp-bsp.h")
void hold_st7121_driver_pattern_diagnostic()
{
    ESP_LOGW(kTag, "holding ST7121 hardware pattern diagnostic; firmware will not continue to UI");
    const Tab5PanelKind panel_kind = detect_tab5_panel_kind();
    if (panel_kind != Tab5PanelKind::St7121) {
        ESP_LOGW(kTag, "forcing ST7121 diagnostic despite panel probe result=%d", static_cast<int>(panel_kind));
    }

    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_LCD, true));
    ESP_ERROR_CHECK(bsp_display_brightness_init());

    esp_ldo_channel_handle_t dsi_phy_power = nullptr;
    esp_ldo_channel_config_t ldo_config {
        .chan_id = BSP_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
        .flags = {},
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_config, &dsi_phy_power));

    esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
    esp_lcd_panel_io_handle_t dbi_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;

    esp_lcd_dsi_bus_config_t bus_config {};
    bus_config.bus_id = 0;
    bus_config.num_data_lanes = 2;
    bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_config.lane_bit_rate_mbps = 965;
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus));

    esp_lcd_dbi_io_config_t dbi_config {};
    dbi_config.virtual_channel = 0;
    dbi_config.lcd_cmd_bits = 8;
    dbi_config.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &dbi_io));

    esp_lcd_dpi_panel_config_t dpi_config {};
    dpi_config.virtual_channel = 0;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = 70;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.num_fbs = 1;
    dpi_config.video_timing.h_size = 720;
    dpi_config.video_timing.v_size = 1280;
    dpi_config.video_timing.hsync_back_porch = 40;
    dpi_config.video_timing.hsync_pulse_width = 2;
    dpi_config.video_timing.hsync_front_porch = 40;
    dpi_config.video_timing.vsync_back_porch = 24;
    dpi_config.video_timing.vsync_pulse_width = 20;
    dpi_config.video_timing.vsync_front_porch = 200;
#if CONFIG_BSP_LCD_USE_DMA2D && (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0))
    dpi_config.flags.use_dma2d = true;
#endif

    st7121_vendor_config_t vendor_config {};
    vendor_config.init_cmds = nullptr;
    vendor_config.init_cmds_size = 0;
    vendor_config.mipi_config.dsi_bus = dsi_bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;

    esp_lcd_panel_dev_config_t panel_config {};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 24;
    panel_config.vendor_config = &vendor_config;

    ESP_LOGI(kTag, "initializing ST7121 panel with M5Stack UserDemo timing");
    ESP_ERROR_CHECK(esp_lcd_new_panel_st7121(dbi_io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ESP_ERROR_CHECK(bsp_display_backlight_on());

    constexpr mipi_dsi_pattern_type_t patterns[] = {
        MIPI_DSI_PATTERN_BAR_VERTICAL,
        MIPI_DSI_PATTERN_BAR_HORIZONTAL,
        MIPI_DSI_PATTERN_BER_VERTICAL,
    };
    constexpr const char* names[] = {
        "vertical bars",
        "horizontal bars",
        "vertical BER",
    };

    std::size_t pattern_index = 0;
    while (true) {
        const std::size_t index = pattern_index % (sizeof(patterns) / sizeof(patterns[0]));
        ESP_LOGI(kTag, "ST7121 hardware pattern: %s", names[index]);
        ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(panel, patterns[index]));
        ++pattern_index;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}
#endif

#if defined(TAB5_HAS_ST7123_DRIVER) && __has_include("bsp/esp-bsp.h")
void hold_st7123_driver_pattern_diagnostic()
{
    ESP_LOGW(kTag, "holding ST7123 driver default-pattern diagnostic; firmware will not continue to UI");
    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_LCD, true));
    ESP_ERROR_CHECK(bsp_display_brightness_init());

    esp_ldo_channel_handle_t dsi_phy_power = nullptr;
    esp_ldo_channel_config_t ldo_config {
        .chan_id = BSP_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
        .flags = {},
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_config, &dsi_phy_power));

    esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
    esp_lcd_panel_io_handle_t dbi_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;

    esp_lcd_dsi_bus_config_t bus_config = ST7123_PANEL_BUS_DSI_2CH_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus));

    esp_lcd_dbi_io_config_t dbi_config = ST7123_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &dbi_io));

    esp_lcd_dpi_panel_config_t dpi_config {};
    dpi_config.virtual_channel = 0;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = 78;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.num_fbs = 1;
    dpi_config.video_timing.h_size = 720;
    dpi_config.video_timing.v_size = 1560;
    dpi_config.video_timing.hsync_back_porch = 40;
    dpi_config.video_timing.hsync_pulse_width = 2;
    dpi_config.video_timing.hsync_front_porch = 40;
    dpi_config.video_timing.vsync_back_porch = 4;
    dpi_config.video_timing.vsync_pulse_width = 2;
    dpi_config.video_timing.vsync_front_porch = 320;
#if CONFIG_BSP_LCD_USE_DMA2D && (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0))
    dpi_config.flags.use_dma2d = true;
#endif

    st7123_vendor_config_t vendor_config {};
    vendor_config.init_cmds = nullptr;
    vendor_config.init_cmds_size = 0;
    vendor_config.mipi_config.dsi_bus = dsi_bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;

    esp_lcd_panel_dev_config_t panel_config {};
    panel_config.reset_gpio_num = BSP_LCD_RST;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 16;
    panel_config.vendor_config = &vendor_config;

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7123(dbi_io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ESP_ERROR_CHECK(bsp_display_backlight_on());

    constexpr mipi_dsi_pattern_type_t patterns[] = {
        MIPI_DSI_PATTERN_BAR_VERTICAL,
        MIPI_DSI_PATTERN_BAR_HORIZONTAL,
        MIPI_DSI_PATTERN_BER_VERTICAL,
    };
    constexpr const char* names[] = {
        "vertical bars",
        "horizontal bars",
        "vertical BER",
    };
    std::size_t pattern_index = 0;
    while (true) {
        const std::size_t index = pattern_index % (sizeof(patterns) / sizeof(patterns[0]));
        ESP_LOGI(kTag, "ST7123 driver default hardware pattern: %s", names[index]);
        ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(panel, patterns[index]));
        ++pattern_index;
        vTaskDelay(pdMS_TO_TICKS(3000));
    }
}

constexpr std::uint16_t rgb565(std::uint8_t red, std::uint8_t green, std::uint8_t blue)
{
    return static_cast<std::uint16_t>(((red & 0xf8) << 8) | ((green & 0xfc) << 3) | (blue >> 3));
}

std::uint16_t diagnostic_pixel(std::uint16_t x, std::uint16_t y)
{
    constexpr std::uint16_t h_res = BSP_LCD_H_RES;
    constexpr std::uint16_t v_res = BSP_LCD_V_RES;

    if (x < 12 || x >= h_res - 12 || y < 12 || y >= v_res - 12) {
        return rgb565(255, 255, 255);
    }
    if ((x / 24 + y / 24) % 2 == 0 && (x < 72 || y < 72 || x >= h_res - 72 || y >= v_res - 72)) {
        return rgb565(0, 0, 0);
    }

    const std::uint16_t band = static_cast<std::uint16_t>((x * 6) / h_res);
    switch (band) {
    case 0:
        return rgb565(230, static_cast<std::uint8_t>(y / 6), 36);
    case 1:
        return rgb565(12, 180, static_cast<std::uint8_t>(y / 6));
    case 2:
        return rgb565(static_cast<std::uint8_t>(y / 6), 112, 255);
    case 3:
        return rgb565(240, 210, static_cast<std::uint8_t>(x / 4));
    case 4:
        return rgb565(0, 210, 220);
    default:
        return rgb565(190, static_cast<std::uint8_t>(x / 5), 255);
    }
}

void show_raw_panel_diagnostic(esp_lcd_panel_handle_t panel)
{
    constexpr std::uint16_t h_res = BSP_LCD_H_RES;
    constexpr std::uint16_t v_res = BSP_LCD_V_RES;
    constexpr std::uint16_t rows_per_chunk = 40;
    constexpr std::size_t pixel_count = static_cast<std::size_t>(h_res) * rows_per_chunk;
    auto* pixels = static_cast<std::uint16_t*>(
        heap_caps_malloc(pixel_count * sizeof(std::uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (!pixels) {
        ESP_LOGW(kTag, "raw LCD diagnostic skipped: failed to allocate DMA buffer");
        return;
    }

    ESP_LOGI(kTag, "drawing raw RGB565 LCD diagnostic for 10 seconds");
    for (std::uint16_t y = 0; y < v_res; y = static_cast<std::uint16_t>(y + rows_per_chunk)) {
        const std::uint16_t rows = std::min<std::uint16_t>(rows_per_chunk, v_res - y);
        for (std::uint16_t row = 0; row < rows; ++row) {
            for (std::uint16_t x = 0; x < h_res; ++x) {
                pixels[static_cast<std::size_t>(row) * h_res + x] =
                    diagnostic_pixel(x, static_cast<std::uint16_t>(y + row));
            }
        }
        ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y, h_res, y + rows, pixels));
    }

    vTaskDelay(pdMS_TO_TICKS(10000));
    free(pixels);
    ESP_LOGI(kTag, "raw RGB565 LCD diagnostic complete");
}

void hold_raw_panel_diagnostic(esp_lcd_panel_handle_t panel)
{
    constexpr std::uint16_t h_res = BSP_LCD_H_RES;
    constexpr std::uint16_t v_res = BSP_LCD_V_RES;
    constexpr std::uint16_t rows_per_chunk = 40;
    constexpr std::size_t pixel_count = static_cast<std::size_t>(h_res) * rows_per_chunk;

    struct DiagnosticColor {
        const char* name;
        std::uint16_t rgb565;
    };
    constexpr DiagnosticColor colors[] = {
        { "white", 0xffff },
        { "red", rgb565(255, 0, 0) },
        { "green", rgb565(0, 255, 0) },
        { "blue", rgb565(0, 0, 255) },
        { "yellow", rgb565(255, 255, 0) },
        { "magenta", rgb565(255, 0, 255) },
        { "cyan", rgb565(0, 255, 255) },
    };

    auto* pixels = static_cast<std::uint16_t*>(
        heap_caps_malloc(pixel_count * sizeof(std::uint16_t), MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (!pixels) {
        ESP_LOGE(kTag, "persistent LCD diagnostic failed: could not allocate DMA buffer");
        return;
    }

    ESP_LOGW(kTag, "holding raw RGB565 LCD diagnostic; firmware will not continue to UI");
    std::size_t color_index = 0;
    while (true) {
        const DiagnosticColor& color = colors[color_index % (sizeof(colors) / sizeof(colors[0]))];
        ESP_LOGI(kTag, "raw RGB565 LCD diagnostic fill: %s", color.name);
        for (std::uint16_t y = 0; y < v_res; y = static_cast<std::uint16_t>(y + rows_per_chunk)) {
            const std::uint16_t rows = std::min<std::uint16_t>(rows_per_chunk, v_res - y);
            std::fill(pixels, pixels + static_cast<std::size_t>(h_res) * rows, color.rgb565);
            ESP_ERROR_CHECK(esp_lcd_panel_draw_bitmap(panel, 0, y, h_res, y + rows, pixels));
        }
        ++color_index;
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

static const st7123_lcd_init_cmd_t kTab5St7123InitCommands[] = {
    {0x60, (uint8_t[]){0x71, 0x23, 0xa2}, 3, 0},
    {0x60, (uint8_t[]){0x71, 0x23, 0xa3}, 3, 0},
    {0x60, (uint8_t[]){0x71, 0x23, 0xa4}, 3, 0},
    {0xA4, (uint8_t[]){0x31}, 1, 0},
    {0xD7, (uint8_t[]){0x10, 0x0A, 0x10, 0x2A, 0x80, 0x80}, 6, 0},
    {0x90, (uint8_t[]){0x71, 0x23, 0x5A, 0x20, 0x24, 0x09, 0x09}, 7, 0},
    {0xA3,
     (uint8_t[]){0x80, 0x01, 0x88, 0x30, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00,
                 0x46, 0x00, 0x00, 0x1E, 0x5C, 0x1E, 0x80, 0x00, 0x4F, 0x05,
                 0x00, 0x00, 0x00, 0x00, 0x00, 0x46, 0x00, 0x00, 0x1E, 0x5C,
                 0x1E, 0x80, 0x00, 0x6F, 0x58, 0x00, 0x00, 0x00, 0xFF},
     40,
     0},
    {0xA6,
     (uint8_t[]){0x03, 0x00, 0x24, 0x55, 0x36, 0x00, 0x39, 0x00, 0x6E, 0x6E,
                 0x91, 0xFF, 0x00, 0x24, 0x55, 0x38, 0x00, 0x37, 0x00, 0x6E,
                 0x6E, 0x91, 0xFF, 0x00, 0x24, 0x11, 0x00, 0x00, 0x00, 0x00,
                 0x6E, 0x6E, 0x91, 0xFF, 0x00, 0xEC, 0x11, 0x00, 0x03, 0x00,
                 0x03, 0x6E, 0x6E, 0xFF, 0xFF, 0x00, 0x08, 0x80, 0x08, 0x80,
                 0x06, 0x00, 0x00, 0x00, 0x00},
     55,
     0},
    {0xA7,
     (uint8_t[]){0x19, 0x19, 0x80, 0x64, 0x40, 0x07, 0x16, 0x40, 0x00, 0x44,
                 0x03, 0x6E, 0x6E, 0x91, 0xFF, 0x08, 0x80, 0x64, 0x40, 0x25,
                 0x34, 0x40, 0x00, 0x02, 0x01, 0x6E, 0x6E, 0x91, 0xFF, 0x08,
                 0x80, 0x64, 0x40, 0x00, 0x00, 0x40, 0x00, 0x00, 0x00, 0x6E,
                 0x6E, 0x91, 0xFF, 0x08, 0x80, 0x64, 0x40, 0x00, 0x00, 0x00,
                 0x00, 0x20, 0x00, 0x6E, 0x6E, 0x84, 0xFF, 0x08, 0x80, 0x44},
     60,
     0},
    {0xAC,
     (uint8_t[]){0x03, 0x19, 0x19, 0x18, 0x18, 0x06, 0x13, 0x13, 0x11, 0x11,
                 0x08, 0x08, 0x0A, 0x0A, 0x1C, 0x1C, 0x07, 0x07, 0x00, 0x00,
                 0x02, 0x02, 0x01, 0x19, 0x19, 0x18, 0x18, 0x06, 0x12, 0x12,
                 0x10, 0x10, 0x09, 0x09, 0x0B, 0x0B, 0x1C, 0x1C, 0x07, 0x07,
                 0x03, 0x03, 0x01, 0x01},
     44,
     0},
    {0xAD,
     (uint8_t[]){0xF0, 0x00, 0x46, 0x00, 0x03, 0x50, 0x50, 0xFF, 0xFF, 0xF0,
                 0x40, 0x06, 0x01, 0x07, 0x42, 0x42, 0xFF, 0xFF, 0x01, 0x00,
                 0x00, 0xFF, 0xFF, 0xFF, 0xFF},
     25,
     0},
    {0xAE, (uint8_t[]){0xFE, 0x3F, 0x3F, 0xFE, 0x3F, 0x3F, 0x00}, 7, 0},
    {0xB2,
     (uint8_t[]){0x15, 0x19, 0x05, 0x23, 0x49, 0xAF, 0x03, 0x2E, 0x5C,
                 0xD2, 0xFF, 0x10, 0x20, 0xFD, 0x20, 0xC0, 0x00},
     17,
     0},
    {0xE8,
     (uint8_t[]){0x20, 0x6F, 0x04, 0x97, 0x97, 0x3E, 0x04, 0xDC,
                 0xDC, 0x3E, 0x06, 0xFA, 0x26, 0x3E},
     15,
     0},
    {0x75, (uint8_t[]){0x03, 0x04}, 2, 0},
    {0xE7,
     (uint8_t[]){0x3B, 0x00, 0x00, 0x7C, 0xA1, 0x8C, 0x20, 0x1A, 0xF0, 0xB1,
                 0x50, 0x00, 0x50, 0xB1, 0x50, 0xB1, 0x50, 0xD8, 0x00, 0x55,
                 0x00, 0xB1, 0x00, 0x45, 0xC9, 0x6A, 0xFF, 0x5A, 0xD8, 0x18,
                 0x88, 0x15, 0xB1, 0x01, 0x01, 0x77},
     36,
     0},
    {0xEA, (uint8_t[]){0x13, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x2C}, 8, 0},
    {0xB0, (uint8_t[]){0x22, 0x43, 0x11, 0x61, 0x25, 0x43, 0x43}, 7, 0},
    {0xB7, (uint8_t[]){0x00, 0x00, 0x73, 0x73}, 4, 0},
    {0xBF, (uint8_t[]){0xA6, 0xAA}, 2, 0},
    {0xA9, (uint8_t[]){0x00, 0x00, 0x73, 0xFF, 0x00, 0x00, 0x03, 0x00, 0x00, 0x03}, 10, 0},
    {0xC8,
     (uint8_t[]){0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05,
                 0x10, 0xF2, 0x06, 0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01,
                 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32, 0xDC, 0x09, 0x33, 0x0F,
                 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF},
     37,
     0},
    {0xC9,
     (uint8_t[]){0x00, 0x00, 0x10, 0x1F, 0x36, 0x00, 0x5D, 0x04, 0x9D, 0x05,
                 0x10, 0xF2, 0x06, 0x60, 0x03, 0x11, 0xAD, 0x00, 0xEF, 0x01,
                 0x22, 0x2E, 0x0E, 0x74, 0x08, 0x32, 0xDC, 0x09, 0x33, 0x0F,
                 0xF3, 0x77, 0x0D, 0xB0, 0xDC, 0x03, 0xFF},
     37,
     0},
    {0x36, (uint8_t[]){0x03}, 1, 0},
    {0x11, (uint8_t[]){0x00}, 1, 120},
    {0x29, (uint8_t[]){0x00}, 1, 50},
    {0x35, (uint8_t[]){0x00}, 1, 0},
};

lv_obj_t* initialize_tab5_st7123_lvgl_display()
{
    ESP_LOGI(kTag, "initializing Tab5 ST7123 display with delayed sleep-out sequence");

    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_LCD, true));
    ESP_ERROR_CHECK(bsp_display_brightness_init());

    esp_ldo_channel_handle_t dsi_phy_power = nullptr;
    esp_ldo_channel_config_t ldo_config {
        .chan_id = BSP_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
        .flags = {},
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_config, &dsi_phy_power));

    esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
    esp_lcd_panel_io_handle_t dbi_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;

    esp_lcd_dsi_bus_config_t bus_config {};
    bus_config.bus_id = 0;
    bus_config.num_data_lanes = BSP_LCD_MIPI_DSI_LANE_NUM;
    bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_config.lane_bit_rate_mbps = BSP_LCD_MIPI_DSI_LANE_BITRATE_MBPS;
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus));

    esp_lcd_dbi_io_config_t dbi_config = ST7123_PANEL_IO_DBI_CONFIG();
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &dbi_io));

    esp_lcd_dpi_panel_config_t dpi_config {};
    dpi_config.virtual_channel = 0;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = 70;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.num_fbs = CONFIG_BSP_LCD_DPI_BUFFER_NUMS;
    dpi_config.video_timing.h_size = BSP_LCD_H_RES;
    dpi_config.video_timing.v_size = BSP_LCD_V_RES;
    dpi_config.video_timing.hsync_back_porch = 40;
    dpi_config.video_timing.hsync_pulse_width = 2;
    dpi_config.video_timing.hsync_front_porch = 40;
    dpi_config.video_timing.vsync_back_porch = 8;
    dpi_config.video_timing.vsync_pulse_width = 2;
    dpi_config.video_timing.vsync_front_porch = 220;
#if CONFIG_BSP_LCD_USE_DMA2D && (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0))
    dpi_config.flags.use_dma2d = true;
#endif

    st7123_vendor_config_t vendor_config {};
    vendor_config.init_cmds = kTab5St7123InitCommands;
    vendor_config.init_cmds_size =
        sizeof(kTab5St7123InitCommands) / sizeof(kTab5St7123InitCommands[0]);
    vendor_config.mipi_config.dsi_bus = dsi_bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;

    esp_lcd_panel_dev_config_t panel_config {};
    panel_config.reset_gpio_num = BSP_LCD_RST;
    panel_config.rgb_ele_order = BSP_LCD_COLOR_SPACE;
    panel_config.bits_per_pixel = BSP_LCD_BITS_PER_PIXEL;
    panel_config.vendor_config = &vendor_config;

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7123(dbi_io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    vTaskDelay(pdMS_TO_TICKS(50));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ESP_ERROR_CHECK(bsp_display_backlight_on());

    if (kShowSt7123HardwareColorBars) {
        ESP_LOGI(kTag, "showing ST7123 hardware color-bar diagnostic for 10 seconds");
        const esp_err_t pattern_err =
            esp_lcd_dpi_panel_set_pattern(panel, MIPI_DSI_PATTERN_BAR_VERTICAL);
        if (pattern_err == ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(10000));
            ESP_ERROR_CHECK(esp_lcd_dpi_panel_set_pattern(panel, MIPI_DSI_PATTERN_NONE));
            ESP_LOGI(kTag, "ST7123 hardware color-bar diagnostic complete");
        } else {
            ESP_LOGW(kTag,
                     "ST7123 hardware color-bar diagnostic unavailable: %s",
                     esp_err_to_name(pattern_err));
        }
    }
    if (kShowRawPanelDiagnostic) {
        if (kHoldRawPanelDiagnostic) {
            hold_raw_panel_diagnostic(panel);
        } else {
            show_raw_panel_diagnostic(panel);
        }
    }

    lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_config.task_stack = 10000;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_config));

    lvgl_port_display_cfg_t display_config {};
    display_config.io_handle = dbi_io;
    display_config.panel_handle = panel;
    display_config.control_handle = panel;
    display_config.buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT;
#if CONFIG_BSP_LCD_DRAW_BUF_DOUBLE
    display_config.double_buffer = true;
#else
    display_config.double_buffer = false;
#endif
    display_config.hres = BSP_LCD_H_RES;
    display_config.vres = BSP_LCD_V_RES;
#if LVGL_VERSION_MAJOR >= 9
    display_config.color_format = LV_COLOR_FORMAT_RGB565;
#endif
    display_config.flags.buff_dma = true;
    display_config.flags.buff_spiram = false;
    display_config.flags.sw_rotate = true;
#if LVGL_VERSION_MAJOR >= 9
    display_config.flags.swap_bytes = BSP_LCD_BIGENDIAN ? true : false;
#endif

    lvgl_port_display_dsi_cfg_t dsi_display_config {};

    g_display = lvgl_port_add_disp_dsi(&display_config, &dsi_display_config);
    ESP_ERROR_CHECK(g_display ? ESP_OK : ESP_FAIL);

    lv_obj_t* screen = nullptr;
    if (bsp_display_lock(0)) {
        lv_display_set_default(g_display);
        bsp_display_rotate(g_display, LV_DISPLAY_ROTATION_90);
        screen = lv_display_get_screen_active(g_display);
        bsp_display_unlock();
    }
    ESP_LOGI(kTag,
             "Tab5 ST7123 LVGL display ready: %dx%d",
             static_cast<int>(lv_display_get_horizontal_resolution(g_display)),
             static_cast<int>(lv_display_get_vertical_resolution(g_display)));
    pause_display_refresh();
    return screen ? screen : lv_screen_active();
}
#endif

#if defined(TAB5_HAS_ST7121_DRIVER) && __has_include("bsp/esp-bsp.h")
lv_obj_t* initialize_tab5_st7121_lvgl_display()
{
    ESP_LOGI(kTag, "initializing Tab5 ST7121 LVGL display");

    ESP_ERROR_CHECK(bsp_feature_enable(BSP_FEATURE_LCD, true));
    ESP_ERROR_CHECK(bsp_display_brightness_init());

    esp_ldo_channel_handle_t dsi_phy_power = nullptr;
    esp_ldo_channel_config_t ldo_config {
        .chan_id = BSP_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = BSP_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
        .flags = {},
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_config, &dsi_phy_power));

    esp_lcd_dsi_bus_handle_t dsi_bus = nullptr;
    esp_lcd_panel_io_handle_t dbi_io = nullptr;
    esp_lcd_panel_handle_t panel = nullptr;

    esp_lcd_dsi_bus_config_t bus_config {};
    bus_config.bus_id = 0;
    bus_config.num_data_lanes = 2;
    bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
    bus_config.lane_bit_rate_mbps = 965;
    ESP_ERROR_CHECK(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus));

    esp_lcd_dbi_io_config_t dbi_config {};
    dbi_config.virtual_channel = 0;
    dbi_config.lcd_cmd_bits = 8;
    dbi_config.lcd_param_bits = 8;
    ESP_ERROR_CHECK(esp_lcd_new_panel_io_dbi(dsi_bus, &dbi_config, &dbi_io));

    esp_lcd_dpi_panel_config_t dpi_config {};
    dpi_config.virtual_channel = 0;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = 70;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB565;
    dpi_config.num_fbs = CONFIG_BSP_LCD_DPI_BUFFER_NUMS;
    dpi_config.video_timing.h_size = BSP_LCD_H_RES;
    dpi_config.video_timing.v_size = BSP_LCD_V_RES;
    dpi_config.video_timing.hsync_back_porch = 40;
    dpi_config.video_timing.hsync_pulse_width = 2;
    dpi_config.video_timing.hsync_front_porch = 40;
    dpi_config.video_timing.vsync_back_porch = 24;
    dpi_config.video_timing.vsync_pulse_width = 20;
    dpi_config.video_timing.vsync_front_porch = 200;
#if CONFIG_BSP_LCD_USE_DMA2D && (ESP_IDF_VERSION < ESP_IDF_VERSION_VAL(6, 0, 0))
    dpi_config.flags.use_dma2d = true;
#endif

    st7121_vendor_config_t vendor_config {};
    vendor_config.init_cmds = nullptr;
    vendor_config.init_cmds_size = 0;
    vendor_config.mipi_config.dsi_bus = dsi_bus;
    vendor_config.mipi_config.dpi_config = &dpi_config;

    esp_lcd_panel_dev_config_t panel_config {};
    panel_config.reset_gpio_num = GPIO_NUM_NC;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.bits_per_pixel = 24;
    panel_config.vendor_config = &vendor_config;

    ESP_ERROR_CHECK(esp_lcd_new_panel_st7121(dbi_io, &panel_config, &panel));
    ESP_ERROR_CHECK(esp_lcd_panel_reset(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_init(panel));
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(panel, true));
    ESP_ERROR_CHECK(bsp_display_backlight_on());

    lvgl_port_cfg_t lvgl_config = ESP_LVGL_PORT_INIT_CONFIG();
    lvgl_config.task_stack = 10000;
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_config));

    lvgl_port_display_cfg_t display_config {};
    display_config.io_handle = dbi_io;
    display_config.panel_handle = panel;
    display_config.control_handle = panel;
    display_config.buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT;
#if CONFIG_BSP_LCD_DRAW_BUF_DOUBLE
    display_config.double_buffer = true;
#else
    display_config.double_buffer = false;
#endif
    display_config.hres = BSP_LCD_H_RES;
    display_config.vres = BSP_LCD_V_RES;
#if LVGL_VERSION_MAJOR >= 9
    display_config.color_format = LV_COLOR_FORMAT_RGB565;
#endif
    display_config.flags.buff_dma = true;
    display_config.flags.buff_spiram = false;
    display_config.flags.sw_rotate = true;
#if LVGL_VERSION_MAJOR >= 9
    display_config.flags.swap_bytes = BSP_LCD_BIGENDIAN ? true : false;
#endif

    lvgl_port_display_dsi_cfg_t dsi_display_config {};
    g_display = lvgl_port_add_disp_dsi(&display_config, &dsi_display_config);
    ESP_ERROR_CHECK(g_display ? ESP_OK : ESP_FAIL);

    lv_obj_t* screen = nullptr;
    if (bsp_display_lock(5000)) {
        lv_display_set_default(g_display);
        bsp_display_rotate(g_display, LV_DISPLAY_ROTATION_90);
        screen = lv_display_get_screen_active(g_display);
        bsp_display_unlock();
    } else {
        ESP_LOGW(kTag, "failed to lock LVGL while applying ST7121 display rotation");
    }

    ESP_LOGI(kTag,
             "Tab5 ST7121 LVGL display ready: %dx%d",
             static_cast<int>(lv_display_get_horizontal_resolution(g_display)),
             static_cast<int>(lv_display_get_vertical_resolution(g_display)));
    pause_display_refresh();
    return screen ? screen : lv_screen_active();
}
#endif

#if __has_include("bsp/esp-bsp.h")
lv_obj_t* initialize_bsp_lvgl_display()
{
    ESP_LOGI(kTag, "initializing Tab5 display through BSP path");
    g_display = bsp_display_start();
    ESP_ERROR_CHECK(g_display ? ESP_OK : ESP_FAIL);

    lv_obj_t* screen = nullptr;
    if (bsp_display_lock(5000)) {
        lv_display_set_default(g_display);
        bsp_display_rotate(g_display, LV_DISPLAY_ROTATION_90);
        screen = lv_display_get_screen_active(g_display);
        bsp_display_unlock();
    } else {
        ESP_LOGW(kTag, "failed to lock LVGL while applying BSP display rotation");
    }

    ESP_ERROR_CHECK(bsp_display_backlight_on());
    ESP_LOGI(kTag,
             "Tab5 BSP LVGL display ready: %dx%d",
             static_cast<int>(lv_display_get_horizontal_resolution(g_display)),
             static_cast<int>(lv_display_get_vertical_resolution(g_display)));
    pause_display_refresh();
    return screen ? screen : lv_screen_active();
}
#endif

struct PendingSetup {
    tab5::settings::WifiCredentials wifi;
    tab5::longbridge::EndpointRegion endpoint { tab5::longbridge::EndpointRegion::Global };
    tab5::settings::LongbridgeApiKeyCredentials api_key;
};

SemaphoreHandle_t g_action_mutex { nullptr };
std::optional<PendingSetup> g_pending_setup;
std::optional<std::string> g_pending_add_symbol;
std::optional<std::size_t> g_pending_remove_symbol;
bool g_pending_refresh_quotes { false };
bool g_pending_reset_settings { false };

void seed_default_watchlist()
{
    if (!g_settings.watchlist.empty()) {
        return;
    }

    for (const char* symbol : { "AAPL.US", "700.HK", "600519.SH", "000001.SZ" }) {
        if (auto parsed = tab5::quotes::SecuritySymbol::parse(symbol)) {
            g_settings.watchlist.add(*parsed);
        }
    }
}

void set_boot_warning(std::string warning)
{
    if (g_boot_warning.empty()) {
        g_boot_warning = std::move(warning);
    }
}

std::optional<std::string> read_text_file(const char* path)
{
    FILE* file = std::fopen(path, "rb");
    if (!file) {
        return std::nullopt;
    }

    std::string content;
    std::array<char, 512> buffer {};
    while (true) {
        const std::size_t read = std::fread(buffer.data(), 1, buffer.size(), file);
        if (read > 0) {
            if (content.size() + read > kSdSettingsFileMaxBytes) {
                std::fclose(file);
                ESP_LOGW(kTag, "SD settings file is larger than %u bytes: %s",
                         static_cast<unsigned>(kSdSettingsFileMaxBytes),
                         path);
                return std::nullopt;
            }
            content.append(buffer.data(), read);
        }
        if (read < buffer.size()) {
            if (std::ferror(file)) {
                std::fclose(file);
                ESP_LOGW(kTag, "failed reading SD settings file: %s", path);
                return std::nullopt;
            }
            break;
        }
    }

    std::fclose(file);
    return content;
}

void preserve_existing_api_key_if_safe(tab5::settings::SettingsFileResult& parsed,
                                       const tab5::settings::AppSettings& existing)
{
    if (!parsed.touched_api_key_credentials && existing.api_key.complete()) {
        parsed.settings.api_key = existing.api_key;
    }
}

bool import_settings_from_sd_card()
{
#if __has_include("bsp/esp-bsp.h")
    const esp_err_t mount_err = bsp_sdcard_mount();
    const bool mounted_here = mount_err == ESP_OK;
    if (mount_err != ESP_OK && mount_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGI(kTag, "SD card settings import skipped: mount failed: %s", esp_err_to_name(mount_err));
        return false;
    }

    const std::array<const char*, 4> paths {
        BSP_SD_MOUNT_POINT "/TAB5.CFG",
        BSP_SD_MOUNT_POINT "/TAB5.INI",
        BSP_SD_MOUNT_POINT "/tab5_stock_terminal.conf",
        BSP_SD_MOUNT_POINT "/tab5_stock_terminal.ini",
    };

    bool saw_file = false;
    bool imported = false;
    for (const char* path : paths) {
        auto content = read_text_file(path);
        if (!content.has_value()) {
            continue;
        }
        saw_file = true;
        auto parsed = tab5::settings::parse_settings_file(*content);
        for (const auto& warning : parsed.warnings) {
            ESP_LOGW(kTag, "SD settings warning: %s", warning.c_str());
        }
        preserve_existing_api_key_if_safe(parsed, g_settings);
        if (parsed.status == tab5::settings::SettingsFileStatus::MissingRequired
            && parsed.settings.onboarding_complete()) {
            parsed.status = tab5::settings::SettingsFileStatus::Ok;
        }
        if (!parsed.ok()) {
            const std::string warning = std::string("SD config rejected: ")
                + tab5::settings::settings_file_status_text(parsed.status);
            ESP_LOGW(kTag, "%s: %s", warning.c_str(), path);
            set_boot_warning(warning);
            break;
        }

        const esp_err_t save_err = g_settings_store.save(parsed.settings);
        if (save_err != ESP_OK) {
            const std::string warning = std::string("SD config save failed: ") + esp_err_to_name(save_err);
            ESP_LOGW(kTag, "%s", warning.c_str());
            set_boot_warning(warning);
            break;
        }

        g_settings = std::move(parsed.settings);
        ESP_LOGI(kTag, "loaded settings from SD card: %s", path);
        imported = true;
        break;
    }

    if (!saw_file) {
        ESP_LOGI(kTag, "no SD settings file found");
    }
    if (mounted_here) {
        const esp_err_t unmount_err = bsp_sdcard_unmount();
        if (unmount_err != ESP_OK) {
            ESP_LOGW(kTag, "SD card unmount failed after settings import: %s", esp_err_to_name(unmount_err));
        }
    }
    return imported;
#else
    return false;
#endif
}

void redraw_watchlist()
{
    g_quote_store.set_watchlist(g_settings.watchlist);
    g_ui.show_watchlist(g_quote_store.ordered_snapshots());
    g_rendered_status.clear();
    refresh_status_line();
}

void save_settings_and_redraw()
{
    const esp_err_t err = g_settings_store.save(g_settings);
    if (err != ESP_OK) {
        g_ui.set_error(std::string("failed to save settings: ") + esp_err_to_name(err));
    } else {
        g_ui.clear_error();
    }
    redraw_watchlist();
}

bool wifi_is_connected()
{
    if (!g_wifi_events) {
        return false;
    }
    return (xEventGroupGetBits(g_wifi_events) & kWifiConnectedBit) != 0;
}

void wifi_event_handler(void*,
                        esp_event_base_t event_base,
                        std::int32_t event_id,
                        void* event_data)
{
    if (!g_wifi_events) {
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_wifi_events, kWifiConnectedBit);
        xEventGroupSetBits(g_wifi_events, kWifiFailedBit);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        auto* event = static_cast<ip_event_got_ip_t*>(event_data);
        ESP_LOGI(kTag, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
        xEventGroupClearBits(g_wifi_events, kWifiFailedBit);
        xEventGroupSetBits(g_wifi_events, kWifiConnectedBit);
    }
}

#if defined(TAB5_HAS_ESP_HOSTED)
void hosted_event_handler(void*,
                          esp_event_base_t event_base,
                          std::int32_t event_id,
                          void*)
{
    if (event_base != ESP_HOSTED_EVENT || !g_wifi_events) {
        return;
    }

    if (event_id == ESP_HOSTED_EVENT_TRANSPORT_UP) {
        ESP_LOGI(kTag, "ESP-Hosted transport is up");
        xEventGroupSetBits(g_wifi_events, kHostedTransportUpBit);
    } else if (event_id == ESP_HOSTED_EVENT_TRANSPORT_DOWN
               || event_id == ESP_HOSTED_EVENT_TRANSPORT_FAILURE) {
        ESP_LOGW(kTag, "ESP-Hosted transport is down or failed");
        xEventGroupClearBits(g_wifi_events, kHostedTransportUpBit);
    }
}
#endif

bool start_hosted_transport()
{
#if defined(TAB5_HAS_ESP_HOSTED) && defined(CONFIG_ESP_WIFI_REMOTE_ENABLED) \
    && CONFIG_ESP_WIFI_REMOTE_ENABLED && defined(CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED) \
    && CONFIG_ESP_WIFI_REMOTE_LIBRARY_HOSTED
    if (g_hosted_started) {
        return true;
    }
    if (!g_wifi_events) {
        return false;
    }

    xEventGroupClearBits(g_wifi_events, kHostedTransportUpBit);
    if (!g_hosted_event_handler_registered) {
        const esp_err_t event_err = esp_event_handler_register(
            ESP_HOSTED_EVENT,
            ESP_EVENT_ANY_ID,
            hosted_event_handler,
            nullptr);
        if (event_err != ESP_OK && event_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(kTag, "ESP-Hosted event handler failed: %s", esp_err_to_name(event_err));
            return false;
        }
        g_hosted_event_handler_registered = true;
    }

    esp_err_t err = static_cast<esp_err_t>(esp_hosted_init());
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "ESP-Hosted init failed: %s", esp_err_to_name(err));
        return false;
    }
    err = static_cast<esp_err_t>(esp_hosted_connect_to_slave());
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "ESP-Hosted connect failed: %s", esp_err_to_name(err));
        return false;
    }

    const EventBits_t bits = xEventGroupWaitBits(g_wifi_events,
                                                 kHostedTransportUpBit,
                                                 pdFALSE,
                                                 pdFALSE,
                                                 pdMS_TO_TICKS(30000));
    if ((bits & kHostedTransportUpBit) == 0) {
        ESP_LOGW(kTag, "ESP-Hosted transport did not come up before timeout");
        return false;
    }
    g_hosted_started = true;
#endif
    return true;
}

void start_mdns()
{
    if (g_mdns_started) {
        return;
    }
    if (mdns_init() != ESP_OK) {
        ESP_LOGW(kTag, "mDNS init failed");
        return;
    }
    mdns_hostname_set("tab5-stock");
    mdns_instance_name_set("Tab5 Stock Terminal");
    g_mdns_started = true;
}

void advertise_sd_file_manager_mdns()
{
    if (!g_mdns_started || !g_sd_file_manager_server.running() || g_sd_file_manager_mdns_advertised) {
        return;
    }

    const esp_err_t err = mdns_service_add("Tab5 SD Files",
                                           "_http",
                                           "_tcp",
                                           g_sd_file_manager_server.port(),
                                           nullptr,
                                           0);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "SD web mDNS service registration failed: %s", esp_err_to_name(err));
        return;
    }
    g_sd_file_manager_mdns_advertised = true;
}

void start_sd_file_manager_once()
{
    if (g_sd_file_manager_server.running()) {
        advertise_sd_file_manager_mdns();
        return;
    }
    g_sd_file_manager_server.on_status([](const std::string& status) {
        g_sd_file_manager_status = status;
        refresh_status_line();
    });
    if (!g_sd_file_manager_server.start()) {
        ESP_LOGW(kTag, "SD web file manager failed to start");
        return;
    }

    advertise_sd_file_manager_mdns();
}

void start_sntp_once()
{
    if (g_sntp_started) {
        return;
    }
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_init();
    g_sntp_started = true;
}

bool wait_for_time_sync()
{
    start_sntp_once();
    for (int attempt = 0; attempt < 20; ++attempt) {
        const std::time_t now = std::time(nullptr);
        if (now > 1'700'000'000) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    return false;
}

bool start_wifi_sta(const tab5::settings::WifiCredentials& wifi)
{
    if (!wifi.complete()) {
        return false;
    }

#if __has_include("bsp/esp-bsp.h")
    const esp_err_t power_err = bsp_feature_enable(BSP_FEATURE_WIFI, true);
    if (power_err != ESP_OK) {
        ESP_LOGW(kTag, "Wi-Fi power enable failed: %s", esp_err_to_name(power_err));
    }
#endif

    if (!g_netif_initialized) {
        ESP_ERROR_CHECK(esp_netif_init());
        const esp_err_t event_loop_err = esp_event_loop_create_default();
        if (event_loop_err != ESP_OK && event_loop_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(kTag, "event loop init failed: %s", esp_err_to_name(event_loop_err));
        }
        if (!start_hosted_transport()) {
            return false;
        }
        esp_netif_create_default_wifi_sta();
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event_handler, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, wifi_event_handler, nullptr));
        start_mdns();
        g_netif_initialized = true;
    }

    if (!g_wifi_initialized) {
        wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
        ESP_ERROR_CHECK(esp_wifi_init(&init_config));
        g_wifi_initialized = true;
    }

    if (g_wifi_events) {
        xEventGroupClearBits(g_wifi_events, kWifiConnectedBit | kWifiFailedBit);
    }
    g_wifi_online = false;
    if (g_wifi_started) {
        esp_wifi_disconnect();
        esp_wifi_stop();
        g_wifi_started = false;
    }

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    wifi_config_t wifi_config {};
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.ssid),
                 wifi.ssid.c_str(),
                 sizeof(wifi_config.sta.ssid) - 1);
    std::strncpy(reinterpret_cast<char*>(wifi_config.sta.password),
                 wifi.password.c_str(),
                 sizeof(wifi_config.sta.password) - 1);
    wifi_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    wifi_config.sta.sae_pwe_h2e = WPA3_SAE_PWE_BOTH;
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    g_wifi_started = true;

    const esp_err_t connect_err = esp_wifi_connect();
    if (connect_err != ESP_OK) {
        ESP_LOGW(kTag, "wifi connect failed: %s", esp_err_to_name(connect_err));
        if (g_wifi_events) {
            xEventGroupSetBits(g_wifi_events, kWifiFailedBit);
        }
        return false;
    }

    if (!g_wifi_events) {
        return true;
    }

    const EventBits_t bits = xEventGroupWaitBits(g_wifi_events,
                                                 kWifiConnectedBit | kWifiFailedBit,
                                                 pdFALSE,
                                                 pdFALSE,
                                                 pdMS_TO_TICKS(15000));
    if ((bits & kWifiConnectedBit) == 0) {
        ESP_LOGW(kTag, "wifi did not connect before timeout");
        xEventGroupSetBits(g_wifi_events, kWifiFailedBit);
        return false;
    }

    g_wifi_online = true;
    g_wifi_backoff.reset();
    g_next_wifi_reconnect_ms = 0;
    start_sd_file_manager_once();
    if (!wait_for_time_sync()) {
        ESP_LOGW(kTag, "SNTP time sync did not complete before timeout");
    }
    return true;
}

lv_obj_t* initialize_display()
{
    lv_obj_t* screen = nullptr;
#if __has_include("bsp/esp-bsp.h")
    if (kHoldBspHardwarePatternDiagnostic) {
        hold_bsp_hardware_pattern_diagnostic();
    }
#if defined(TAB5_HAS_ST7121_DRIVER)
    if (kHoldSt7121DriverPatternDiagnostic) {
        hold_st7121_driver_pattern_diagnostic();
    }
#endif
#if defined(TAB5_HAS_ST7123_DRIVER)
    if (kHoldSt7123DriverPatternDiagnostic) {
        hold_st7123_driver_pattern_diagnostic();
    }
#endif
#if defined(TAB5_HAS_ST7121_DRIVER)
    if (kForceSt7121Display) {
        ESP_LOGW(kTag, "forcing Tab5 ST7121 LVGL display path");
        screen = initialize_tab5_st7121_lvgl_display();
    } else
#endif
#if defined(TAB5_HAS_ST7123_DRIVER)
    if (kForceSt7123DisplayDiagnostic) {
        ESP_LOGW(kTag, "forcing BSP-backed ST7123 display diagnostic path");
        screen = initialize_tab5_st7123_lvgl_display();
    } else
#endif
    {
    const Tab5PanelKind panel_kind = detect_tab5_panel_kind();
#if defined(TAB5_HAS_ST7123_DRIVER)
    if (panel_kind == Tab5PanelKind::St7123) {
        screen = initialize_tab5_st7123_lvgl_display();
    } else
#endif
    {
        screen = initialize_bsp_lvgl_display();
    }
    }
#else
    lv_init();
    screen = lv_screen_active();
#endif
    if (g_display) {
        ESP_LOGI(kTag,
                 "LVGL display ready: %dx%d",
                 static_cast<int>(lv_display_get_horizontal_resolution(g_display)),
                 static_cast<int>(lv_display_get_vertical_resolution(g_display)));
    }
    if (kHoldLvglDisplayDiagnostic) {
        hold_lvgl_display_diagnostic(screen ? screen : lv_screen_active());
    }
    return screen ? screen : lv_screen_active();
}

void initialize_keyboard()
{
#if __has_include("bsp/esp-bsp.h") && __has_include("esp_lvgl_port.h")
    const esp_err_t usb_err = bsp_usb_host_start(BSP_USB_HOST_POWER_MODE_USB_DEV, true);
    if (usb_err != ESP_OK && usb_err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(kTag, "USB host start failed: %s", esp_err_to_name(usb_err));
        return;
    }

    lvgl_port_hid_keyboard_cfg_t keyboard_config {};
    keyboard_config.disp = g_display;
    lv_indev_t* keyboard = lvgl_port_add_usb_hid_keyboard_input(&keyboard_config);
    if (!keyboard) {
        ESP_LOGW(kTag, "USB HID keyboard input was not created");
        return;
    }
    lv_indev_set_group(keyboard, g_ui.focus_group());
    ESP_LOGI(kTag, "USB HID keyboard input initialized");
#else
    ESP_LOGW(kTag, "USB HID keyboard support is unavailable in this build");
#endif
}

esp_err_t tab5_keyboard_write_register(std::uint8_t reg, std::uint8_t value)
{
    if (!g_tab5_keyboard_device) {
        return ESP_ERR_INVALID_STATE;
    }
    const std::uint8_t data[] = { reg, value };
    return i2c_master_transmit(g_tab5_keyboard_device, data, sizeof(data), 100);
}

esp_err_t tab5_keyboard_read_register(std::uint8_t reg, std::uint8_t* data, std::size_t length)
{
    if (!g_tab5_keyboard_device) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!data || length == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    return i2c_master_transmit_receive(g_tab5_keyboard_device, &reg, 1, data, length, 100);
}

esp_err_t tab5_keyboard_clear_interrupt_status()
{
    return tab5_keyboard_write_register(kTab5KeyboardRegInterruptStatus, 0);
}

std::uint32_t hid_key_to_lvgl_key(std::uint8_t modifier, std::uint8_t keycode)
{
    const bool shifted = (modifier & 0x22U) != 0;
    if (keycode >= 0x04 && keycode <= 0x1d) {
        const char base = static_cast<char>('a' + (keycode - 0x04));
        return shifted ? static_cast<std::uint32_t>(base - 'a' + 'A')
                       : static_cast<std::uint32_t>(base);
    }
    if (keycode >= 0x1e && keycode <= 0x27) {
        constexpr char normal[] = "1234567890";
        constexpr char shifted_chars[] = "!@#$%^&*()";
        const auto index = static_cast<std::size_t>(keycode - 0x1e);
        return static_cast<std::uint32_t>(shifted ? shifted_chars[index] : normal[index]);
    }

    switch (keycode) {
    case 0x28:
        return LV_KEY_ENTER;
    case 0x29:
        return LV_KEY_ESC;
    case 0x2a:
        return LV_KEY_BACKSPACE;
    case 0x2b:
        return shifted ? LV_KEY_PREV : LV_KEY_NEXT;
    case 0x2c:
        return ' ';
    case 0x2d:
        return shifted ? '_' : '-';
    case 0x2e:
        return shifted ? '+' : '=';
    case 0x2f:
        return shifted ? '{' : '[';
    case 0x30:
        return shifted ? '}' : ']';
    case 0x31:
        return shifted ? '|' : '\\';
    case 0x32:
        return shifted ? '|' : '\\';
    case 0x33:
        return shifted ? ':' : ';';
    case 0x34:
        return shifted ? '"' : '\'';
    case 0x35:
        return shifted ? '~' : '`';
    case 0x36:
        return shifted ? '<' : ',';
    case 0x37:
        return shifted ? '>' : '.';
    case 0x38:
        return shifted ? '?' : '/';
    case 0x4a:
        return LV_KEY_HOME;
    case 0x4c:
        return LV_KEY_DEL;
    case 0x4d:
        return LV_KEY_END;
    case 0x4f:
        return LV_KEY_RIGHT;
    case 0x50:
        return LV_KEY_LEFT;
    case 0x51:
        return LV_KEY_DOWN;
    case 0x52:
        return LV_KEY_UP;
    default:
        return 0;
    }
}

std::uint32_t raw_key_to_lvgl_key(std::uint8_t row, std::uint8_t col, bool shifted, bool symbol_layer)
{
    // Official keyboard firmware exposes a 5x14 matrix in normal mode.
    // This maps the common character layers plus navigation/editing keys.
    static constexpr std::uint32_t kBaseMap[5][14] = {
        { LV_KEY_ESC, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '+', LV_KEY_DEL },
        { '`', '!', '@', '#', '$', '%', '^', '&', '*', '(', ')', '[', ']', '\\' },
        { LV_KEY_NEXT, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', ';', '\'', LV_KEY_BACKSPACE },
        { 0, 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', LV_KEY_UP, '_', LV_KEY_ENTER },
        { 0, 0, 'z', 'x', 'c', 'v', 'b', 'n', 'm', '.', LV_KEY_LEFT, LV_KEY_DOWN, LV_KEY_RIGHT, ' ' },
    };
    static constexpr std::uint32_t kSymbolMap[5][14] = {
        { LV_KEY_ESC, '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '+', LV_KEY_DEL },
        { '~', '?', '@', '#', '$', '%', '^', '&', '/', '<', '>', '{', '}', '|' },
        { LV_KEY_NEXT, 'q', 'w', 'e', 'r', 't', 'y', 'u', 'i', 'o', 'p', ':', '"', LV_KEY_BACKSPACE },
        { 0, 0, 'a', 's', 'd', 'f', 'g', 'h', 'j', 'k', 'l', LV_KEY_UP, '=', LV_KEY_ENTER },
        { 0, 0, 'z', 'x', 'c', 'v', 'b', 'n', 'm', ',', LV_KEY_LEFT, LV_KEY_DOWN, LV_KEY_RIGHT, ' ' },
    };
    if (row >= 5 || col >= 14) {
        return 0;
    }
    std::uint32_t key = symbol_layer ? kSymbolMap[row][col] : kBaseMap[row][col];
    if (shifted && key >= 'a' && key <= 'z') {
        key = static_cast<std::uint32_t>(key - 'a' + 'A');
    }
    return key;
}

std::string uppercase_ascii(std::string text)
{
    for (char& ch : text) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    }
    return text;
}

std::uint32_t char_event_to_lvgl_key(const std::uint8_t* data, std::size_t length)
{
    if (!data || length == 0) {
        return 0;
    }
    if (length == 1) {
        return static_cast<std::uint32_t>(data[0]);
    }

    const std::string text(reinterpret_cast<const char*>(data),
                           reinterpret_cast<const char*>(data + length));
    const std::string upper = uppercase_ascii(text);
    if (upper == "ESC") {
        return LV_KEY_ESC;
    }
    if (upper == "DEL") {
        return LV_KEY_DEL;
    }
    if (upper == "BACKSPACE") {
        return LV_KEY_BACKSPACE;
    }
    if (upper == "TAB") {
        return LV_KEY_NEXT;
    }
    if (upper == "ENTER") {
        return LV_KEY_ENTER;
    }
    if (upper == "UP") {
        return LV_KEY_UP;
    }
    if (upper == "DOWN") {
        return LV_KEY_DOWN;
    }
    if (upper == "LEFT") {
        return LV_KEY_LEFT;
    }
    if (upper == "RIGHT") {
        return LV_KEY_RIGHT;
    }
    return 0;
}

void tab5_keyboard_read_cb(lv_indev_t* indev, lv_indev_data_t* data)
{
    auto* ctx = static_cast<Tab5KeyboardInputContext*>(lv_indev_get_driver_data(indev));
    if (!ctx) {
        data->state = LV_INDEV_STATE_RELEASED;
        data->key = 0;
        return;
    }

    if (ctx->release_pending) {
        data->key = ctx->last_key;
        data->state = LV_INDEV_STATE_RELEASED;
        ctx->release_pending = false;
        data->continue_reading =
            ctx->key_queue && uxQueueMessagesWaiting(ctx->key_queue) > 0;
        return;
    }

    std::uint32_t key = 0;
    if (ctx->key_queue && xQueueReceive(ctx->key_queue, &key, 0) == pdTRUE) {
        ctx->last_key = key;
        ctx->release_pending = true;
        data->key = key;
        data->state = LV_INDEV_STATE_PRESSED;
        data->continue_reading = true;
        if (ctx->logged_key_count < 20) {
            ESP_LOGI(kTag, "Tab5 keyboard input: key=0x%02" PRIx32, key);
            ++ctx->logged_key_count;
        }
        return;
    }

    data->key = ctx->last_key;
    data->state = LV_INDEV_STATE_RELEASED;
}

void enqueue_tab5_keyboard_key(std::uint32_t key)
{
    if (!key || !g_tab5_keyboard_input.key_queue || !g_tab5_keyboard_indev) {
        return;
    }
    if (xQueueSend(g_tab5_keyboard_input.key_queue, &key, 0) != pdTRUE) {
        std::uint32_t dropped_key = 0;
        (void)xQueueReceive(g_tab5_keyboard_input.key_queue, &dropped_key, 0);
        if (xQueueSend(g_tab5_keyboard_input.key_queue, &key, 0) != pdTRUE) {
            ESP_LOGW(kTag, "Tab5 keyboard LVGL queue is full; dropping key=0x%02" PRIx32, key);
            return;
        }
    }
    (void)lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, g_tab5_keyboard_indev);
}

void IRAM_ATTR tab5_keyboard_gpio_isr(void*)
{
    if (!g_tab5_keyboard_intr_queue) {
        return;
    }
    const std::uint32_t gpio = static_cast<std::uint32_t>(kTab5KeyboardInt);
    BaseType_t higher_priority_task_woken = pdFALSE;
    (void)xQueueSendFromISR(g_tab5_keyboard_intr_queue, &gpio, &higher_priority_task_woken);
    if (higher_priority_task_woken == pdTRUE) {
        portYIELD_FROM_ISR();
    }
}

bool drain_tab5_keyboard_events()
{
    std::uint8_t status = 0;
    esp_err_t err = tab5_keyboard_read_register(kTab5KeyboardRegInterruptStatus, &status, 1);
    std::uint8_t event_count = 0;
    const int int_level = gpio_get_level(kTab5KeyboardInt);
    ++g_tab5_keyboard_poll_count;
    if (err != ESP_OK) {
        ++g_tab5_keyboard_i2c_error_count;
        if (g_tab5_keyboard_ready && g_tab5_keyboard_i2c_error_count >= 5) {
            ESP_LOGW(kTag, "Tab5 keyboard disconnected: %s", esp_err_to_name(err));
            g_tab5_keyboard_ready = false;
            g_tab5_keyboard_absent_logged = false;
            set_keyboard_status(KeyboardStatus::Lost);
        }
        return false;
    }

    g_tab5_keyboard_i2c_error_count = 0;
    err = tab5_keyboard_read_register(kTab5KeyboardRegEventCount, &event_count, 1);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Tab5 keyboard event-count read failed: %s", esp_err_to_name(err));
        return false;
    }

    if ((status & kTab5KeyboardStatusChar) != 0) {
        err = tab5_keyboard_read_register(kTab5KeyboardRegEventCount, &event_count, 1);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "Tab5 keyboard event-count read failed: %s", esp_err_to_name(err));
            return false;
        }
    } else if ((status & kTab5KeyboardStatusHid) != 0) {
        err = tab5_keyboard_read_register(kTab5KeyboardRegEventCount, &event_count, 1);
        if (err != ESP_OK) {
            ESP_LOGW(kTag, "Tab5 keyboard HID event-count read failed: %s", esp_err_to_name(err));
            return false;
        }
    }
    if (event_count > kTab5KeyboardMaxEvents) {
        event_count = kTab5KeyboardMaxEvents;
    }
    const std::int64_t now_ms = esp_timer_get_time() / 1000;
    const bool should_log_heartbeat =
        now_ms - g_tab5_keyboard_last_heartbeat_ms >= 5000;
    if (event_count > 0 || status != g_tab5_keyboard_last_status
        || int_level != g_tab5_keyboard_last_int_level || g_tab5_keyboard_poll_count <= 3) {
        ESP_LOGI(kTag,
                 "Tab5 keyboard poll: status=0x%02x event_count=%u int=%d irq=%" PRIu32,
                 status,
                 event_count,
                 int_level,
                 static_cast<std::uint32_t>(g_tab5_keyboard_irq_count));
        g_tab5_keyboard_last_status = status;
        g_tab5_keyboard_last_int_level = int_level;
    }
    if (should_log_heartbeat) {
        std::uint8_t mode = 0xff;
        std::uint8_t interrupt_config = 0xff;
        (void)tab5_keyboard_read_register(kTab5KeyboardRegMode, &mode, 1);
        (void)tab5_keyboard_read_register(kTab5KeyboardRegInterruptConfig, &interrupt_config, 1);
        ESP_LOGI(kTag,
                 "Tab5 keyboard heartbeat: status=0x%02x event_count=%u int=%d irq=%" PRIu32
                 " polls=%" PRIu32 " mode=%u INT_CFG=0x%02x",
                 status,
                 event_count,
                 int_level,
                 static_cast<std::uint32_t>(g_tab5_keyboard_irq_count),
                 g_tab5_keyboard_poll_count,
                 mode,
                 interrupt_config);
        g_tab5_keyboard_last_heartbeat_ms = now_ms;
    }
    if (event_count > 0 && g_tab5_keyboard_input.logged_raw_event_count < 40) {
        ESP_LOGI(kTag, "Tab5 keyboard queued decoded events: %u", event_count);
    }
    if (event_count > 0) {
        set_keyboard_status(KeyboardStatus::Input);
    }

    if ((status & kTab5KeyboardStatusChar) != 0) {
        for (std::uint8_t i = 0; i < event_count; ++i) {
            std::uint8_t event_length = 0;
            err = tab5_keyboard_read_register(kTab5KeyboardRegCharEventLength, &event_length, 1);
            if (err != ESP_OK) {
                ESP_LOGW(kTag, "Tab5 keyboard char-event length read failed: %s", esp_err_to_name(err));
                return false;
            }
            if (event_length == 0) {
                break;
            }
            const std::size_t bytes_to_read = static_cast<std::size_t>(event_length) + 1U;
            if (bytes_to_read > kTab5KeyboardCharEventMaxBytes) {
                ESP_LOGW(kTag, "Tab5 keyboard char-event length out of range: %u", event_length);
                (void)tab5_keyboard_clear_interrupt_status();
                return false;
            }

            std::array<std::uint8_t, kTab5KeyboardCharEventMaxBytes> buffer {};
            err = tab5_keyboard_read_register(kTab5KeyboardRegCharEvent, buffer.data(), bytes_to_read);
            if (err != ESP_OK) {
                ESP_LOGW(kTag, "Tab5 keyboard char-event read failed: %s", esp_err_to_name(err));
                return false;
            }

            const std::uint8_t modifier = buffer[0];
            const std::size_t text_length = static_cast<std::size_t>(event_length);
            const std::uint32_t lv_key = char_event_to_lvgl_key(&buffer[1], text_length);
            if (g_tab5_keyboard_input.logged_raw_event_count < 40) {
                const std::string text(reinterpret_cast<const char*>(&buffer[1]),
                                       reinterpret_cast<const char*>(&buffer[1] + text_length));
                ESP_LOGI(kTag,
                         "Tab5 keyboard char event: mod=0x%02x len=%u text='%s' key=0x%02" PRIx32,
                         modifier,
                         static_cast<unsigned>(text_length),
                         text.c_str(),
                         lv_key);
                ++g_tab5_keyboard_input.logged_raw_event_count;
            }
            if (lv_key != 0) {
                enqueue_tab5_keyboard_key(lv_key);
            }
        }
    } else if ((status & kTab5KeyboardStatusHid) != 0) {
        for (std::uint8_t i = 0; i < event_count; ++i) {
            std::uint8_t buffer[2] {};
            err = tab5_keyboard_read_register(kTab5KeyboardRegHidEvent, buffer, sizeof(buffer));
            if (err != ESP_OK) {
                ESP_LOGW(kTag, "Tab5 keyboard HID-event read failed: %s", esp_err_to_name(err));
                return false;
            }
            if (buffer[0] == kTab5KeyboardEmptyEvent && buffer[1] == kTab5KeyboardEmptyEvent) {
                break;
            }
            const std::uint32_t lv_key = hid_key_to_lvgl_key(buffer[0], buffer[1]);
            if (g_tab5_keyboard_input.logged_raw_event_count < 40) {
                ESP_LOGI(kTag,
                         "Tab5 keyboard HID event: mod=0x%02x keycode=0x%02x key=0x%02" PRIx32,
                         buffer[0],
                         buffer[1],
                         lv_key);
                ++g_tab5_keyboard_input.logged_raw_event_count;
            }
            if (buffer[1] != 0 && lv_key != 0) {
                enqueue_tab5_keyboard_key(lv_key);
            }
        }
    } else if ((status & kTab5KeyboardStatusKey) != 0 || event_count > 0) {
        for (std::uint8_t i = 0; i < event_count; ++i) {
            std::uint8_t raw_event = 0xff;
            err = tab5_keyboard_read_register(kTab5KeyboardRegKeyEvent, &raw_event, 1);
            if (err != ESP_OK) {
                ESP_LOGW(kTag, "Tab5 keyboard key-event read failed: %s", esp_err_to_name(err));
                return false;
            }
            if (raw_event == kTab5KeyboardEmptyEvent) {
                break;
            }
            const bool pressed = (raw_event & 0x80U) != 0;
            const std::uint8_t row = (raw_event >> 4U) & 0x07U;
            const std::uint8_t col = raw_event & 0x0fU;
            if (row == 3 && col == 0) {
                g_tab5_keyboard_sym_active = pressed;
                continue;
            }
            if (row == 3 && col == 1) {
                g_tab5_keyboard_aa_active = pressed;
                continue;
            }
            const std::uint32_t lv_key =
                raw_key_to_lvgl_key(row, col, g_tab5_keyboard_aa_active, g_tab5_keyboard_sym_active);
            if (g_tab5_keyboard_input.logged_raw_event_count < 40) {
                ESP_LOGI(kTag,
                         "Tab5 keyboard raw event: raw=0x%02x pressed=%d row=%u col=%u key=0x%02" PRIx32,
                         raw_event,
                         pressed ? 1 : 0,
                         row,
                         col,
                         lv_key);
                ++g_tab5_keyboard_input.logged_raw_event_count;
            }
            if (pressed && lv_key != 0) {
                enqueue_tab5_keyboard_key(lv_key);
            }
        }
    }
    if (status != 0) {
        const esp_err_t clear_err = tab5_keyboard_clear_interrupt_status();
        if (clear_err != ESP_OK) {
            ESP_LOGW(kTag, "Tab5 keyboard interrupt-status clear failed: %s", esp_err_to_name(clear_err));
        }
    }
    return true;
}

bool configure_tab5_keyboard_device()
{
    std::uint8_t version = 0;
    const esp_err_t version_err =
        tab5_keyboard_read_register(kTab5KeyboardRegFirmwareVersion, &version, 1);
    if (version_err != ESP_OK) {
        return false;
    }
    (void)tab5_keyboard_write_register(kTab5KeyboardRegMode, kTab5KeyboardModeHid);
    vTaskDelay(pdMS_TO_TICKS(20));
    (void)tab5_keyboard_write_register(kTab5KeyboardRegEventCount, 0);
    (void)tab5_keyboard_clear_interrupt_status();
    const esp_err_t mode_err =
        tab5_keyboard_write_register(kTab5KeyboardRegMode, kTab5KeyboardModeKey);
    if (mode_err != ESP_OK) {
        ESP_LOGW(kTag, "Tab5 keyboard mode setup failed: %s", esp_err_to_name(mode_err));
        return false;
    }
    const esp_err_t interrupt_err =
        tab5_keyboard_write_register(kTab5KeyboardRegInterruptConfig, kTab5KeyboardInterruptKey);
    if (interrupt_err != ESP_OK) {
        ESP_LOGW(kTag, "Tab5 keyboard interrupt setup failed: %s", esp_err_to_name(interrupt_err));
        return false;
    }
    std::uint8_t ignored_status = 0;
    (void)tab5_keyboard_read_register(kTab5KeyboardRegInterruptStatus, &ignored_status, 1);
    (void)tab5_keyboard_write_register(kTab5KeyboardRegEventCount, 0);
    (void)tab5_keyboard_clear_interrupt_status();
    std::uint8_t actual_mode = 0xff;
    std::uint8_t actual_interrupt_config = 0xff;
    (void)tab5_keyboard_read_register(kTab5KeyboardRegMode, &actual_mode, 1);
    (void)tab5_keyboard_read_register(kTab5KeyboardRegInterruptConfig, &actual_interrupt_config, 1);
    ESP_LOGI(kTag,
             "Tab5 keyboard detected at 0x%02x on I2C%d (fw=0x%02x, mode=%u, INT_CFG=0x%02x, speed=%u)",
             kTab5KeyboardAddress,
             static_cast<int>(kTab5KeyboardI2CPort),
             version,
             actual_mode,
             actual_interrupt_config,
             static_cast<unsigned>(kTab5KeyboardI2CFrequencyHz));
    g_tab5_keyboard_ready = true;
    g_tab5_keyboard_absent_logged = false;
    g_tab5_keyboard_i2c_error_count = 0;
    g_tab5_keyboard_poll_count = 0;
    g_tab5_keyboard_irq_count = 0;
    g_tab5_keyboard_last_heartbeat_ms = 0;
    g_tab5_keyboard_last_int_level = -1;
    g_tab5_keyboard_last_status = 0xff;
    g_tab5_keyboard_sym_active = false;
    g_tab5_keyboard_aa_active = false;
    set_keyboard_status(KeyboardStatus::Ready);
    return true;
}

void tab5_keyboard_task(void*)
{
    while (true) {
        if (!g_tab5_keyboard_device) {
            vTaskDelay(pdMS_TO_TICKS(1000));
            continue;
        }
        if (!g_tab5_keyboard_ready) {
            if (!configure_tab5_keyboard_device()) {
                if (!g_tab5_keyboard_absent_logged) {
                    ESP_LOGW(kTag,
                             "Tab5 keyboard not detected at 0x%02x on ExtPort1 I2C%d; retrying",
                             kTab5KeyboardAddress,
                             static_cast<int>(kTab5KeyboardI2CPort));
                    g_tab5_keyboard_absent_logged = true;
                }
                set_keyboard_status(KeyboardStatus::Wait);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
        }
        if (g_tab5_keyboard_intr_queue) {
            std::uint32_t gpio = 0;
            if (xQueueReceive(g_tab5_keyboard_intr_queue, &gpio, pdMS_TO_TICKS(20)) == pdTRUE) {
                ++g_tab5_keyboard_irq_count;
                while (xQueueReceive(g_tab5_keyboard_intr_queue, &gpio, 0) == pdTRUE) {
                    ++g_tab5_keyboard_irq_count;
                }
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(20));
        }
        (void)drain_tab5_keyboard_events();
    }
}

void initialize_tab5_keyboard_i2c_input()
{
#if __has_include("esp_lvgl_port.h")
    if (!g_display || !g_ui.focus_group()) {
        ESP_LOGW(kTag, "Tab5 keyboard input skipped: display or focus group is not ready");
        return;
    }

    g_tab5_keyboard_input.key_queue =
        xQueueCreate(kTab5KeyboardQueueSize, sizeof(std::uint32_t));
    if (!g_tab5_keyboard_input.key_queue) {
        ESP_LOGW(kTag, "Tab5 keyboard queue allocation failed");
        return;
    }

    lvgl_port_lock(0);
    g_tab5_keyboard_indev = lv_indev_create();
    if (g_tab5_keyboard_indev) {
        lv_indev_set_type(g_tab5_keyboard_indev, LV_INDEV_TYPE_KEYPAD);
        lv_indev_set_mode(g_tab5_keyboard_indev, LV_INDEV_MODE_TIMER);
        lv_indev_set_read_cb(g_tab5_keyboard_indev, tab5_keyboard_read_cb);
        lv_indev_set_disp(g_tab5_keyboard_indev, g_display);
        lv_indev_set_driver_data(g_tab5_keyboard_indev, &g_tab5_keyboard_input);
        lv_indev_set_group(g_tab5_keyboard_indev, g_ui.focus_group());
    }
    lvgl_port_unlock();

    if (!g_tab5_keyboard_indev) {
        ESP_LOGW(kTag, "Tab5 keyboard LVGL input device was not created");
        vQueueDelete(g_tab5_keyboard_input.key_queue);
        g_tab5_keyboard_input.key_queue = nullptr;
        return;
    }

    gpio_config_t int_config {};
    int_config.pin_bit_mask = 1ULL << kTab5KeyboardInt;
    int_config.mode = GPIO_MODE_INPUT;
    int_config.pull_up_en = GPIO_PULLUP_ENABLE;
    int_config.pull_down_en = GPIO_PULLDOWN_DISABLE;
    int_config.intr_type = GPIO_INTR_NEGEDGE;
    ESP_ERROR_CHECK_WITHOUT_ABORT(gpio_config(&int_config));

    g_tab5_keyboard_intr_queue = xQueueCreate(8, sizeof(std::uint32_t));
    if (!g_tab5_keyboard_intr_queue) {
        ESP_LOGW(kTag, "Tab5 keyboard interrupt queue allocation failed; polling only");
    } else {
        const esp_err_t isr_service_err = gpio_install_isr_service(0);
        if (isr_service_err != ESP_OK && isr_service_err != ESP_ERR_INVALID_STATE) {
            ESP_LOGW(kTag,
                     "Tab5 keyboard GPIO ISR service setup failed: %s; polling only",
                     esp_err_to_name(isr_service_err));
            vQueueDelete(g_tab5_keyboard_intr_queue);
            g_tab5_keyboard_intr_queue = nullptr;
        } else {
            const esp_err_t isr_err =
                gpio_isr_handler_add(kTab5KeyboardInt, tab5_keyboard_gpio_isr, nullptr);
            if (isr_err != ESP_OK && isr_err != ESP_ERR_INVALID_STATE) {
                ESP_LOGW(kTag,
                         "Tab5 keyboard GPIO ISR setup failed: %s; polling only",
                         esp_err_to_name(isr_err));
                vQueueDelete(g_tab5_keyboard_intr_queue);
                g_tab5_keyboard_intr_queue = nullptr;
            }
        }
    }

    i2c_master_bus_config_t bus_config {};
    bus_config.i2c_port = kTab5KeyboardI2CPort;
    bus_config.sda_io_num = kTab5KeyboardSda;
    bus_config.scl_io_num = kTab5KeyboardScl;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7;
    bus_config.flags.enable_internal_pullup = true;

    esp_err_t err = i2c_new_master_bus(&bus_config, &g_tab5_keyboard_bus);
    if (err == ESP_ERR_INVALID_STATE) {
        err = i2c_master_get_bus_handle(kTab5KeyboardI2CPort, &g_tab5_keyboard_bus);
    }
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Tab5 keyboard I2C bus init failed: %s", esp_err_to_name(err));
        return;
    }

    i2c_device_config_t device_config {};
    device_config.dev_addr_length = I2C_ADDR_BIT_LEN_7;
    device_config.device_address = kTab5KeyboardAddress;
    device_config.scl_speed_hz = kTab5KeyboardI2CFrequencyHz;

    err = i2c_master_bus_add_device(g_tab5_keyboard_bus, &device_config, &g_tab5_keyboard_device);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "Tab5 keyboard I2C device add failed: %s", esp_err_to_name(err));
        return;
    }

    (void)configure_tab5_keyboard_device();
    BaseType_t task_created =
        xTaskCreate(tab5_keyboard_task, "tab5_kbd", 4096, nullptr, 5, nullptr);
    if (task_created != pdPASS) {
        ESP_LOGW(kTag, "Tab5 keyboard polling task was not created");
        return;
    }
    ESP_LOGI(kTag,
             "Tab5 keyboard I2C input initialized on SDA=%d SCL=%d INT=%d addr=0x%02x",
             static_cast<int>(kTab5KeyboardSda),
             static_cast<int>(kTab5KeyboardScl),
             static_cast<int>(kTab5KeyboardInt),
             kTab5KeyboardAddress);
#else
    ESP_LOGW(kTag, "Tab5 keyboard input support is unavailable in this build");
#endif
}

void queue_quote_delta(const tab5::quotes::QuoteDelta& delta)
{
    if (!g_quote_delta_mutex) {
        return;
    }
    if (xSemaphoreTake(g_quote_delta_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        const auto key = delta.symbol.value();
        if (key.empty()) {
            xSemaphoreGive(g_quote_delta_mutex);
            return;
        }
        auto found = g_pending_quote_deltas.find(key);
        if (found == g_pending_quote_deltas.end()) {
            if (g_pending_quote_deltas.size() >= tab5::quotes::Watchlist::kMaxSymbols) {
                ++g_dropped_quote_delta_count;
                xSemaphoreGive(g_quote_delta_mutex);
                return;
            }
            g_pending_quote_deltas[key] = delta;
        } else {
            found->second = tab5::quotes::merge_delta(found->second, delta);
        }
        xSemaphoreGive(g_quote_delta_mutex);
    }
}

std::pair<std::vector<tab5::quotes::QuoteDelta>, std::uint32_t> take_quote_deltas()
{
    std::vector<tab5::quotes::QuoteDelta> deltas;
    std::uint32_t dropped = 0;
    if (!g_quote_delta_mutex) {
        return { deltas, dropped };
    }
    if (xSemaphoreTake(g_quote_delta_mutex, pdMS_TO_TICKS(20)) == pdTRUE) {
        deltas.reserve(g_pending_quote_deltas.size());
        for (auto& entry : g_pending_quote_deltas) {
            deltas.push_back(std::move(entry.second));
        }
        g_pending_quote_deltas.clear();
        dropped = g_dropped_quote_delta_count;
        g_dropped_quote_delta_count = 0;
        xSemaphoreGive(g_quote_delta_mutex);
    }
    return { deltas, dropped };
}

bool quote_auth_configured()
{
    return g_settings.quote_auth_configured();
}

tab5::longbridge::ApiKeyCredentials longbridge_api_key_credentials()
{
    return {
        g_settings.api_key.app_key,
        g_settings.api_key.app_secret,
        g_settings.api_key.access_token,
    };
}

void configure_longbridge()
{
    g_longbridge.configure({
        tab5::longbridge::default_endpoints(g_settings.endpoint_region),
        longbridge_api_key_credentials(),
    });
    g_longbridge.on_state([](const std::string& state) {
        set_connection_status(state);
    });
    g_longbridge.on_quote_delta([](const tab5::quotes::QuoteDelta& delta) {
        queue_quote_delta(delta);
    });
}

void queue_setup_save(PendingSetup setup)
{
    if (g_action_mutex && xSemaphoreTake(g_action_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_pending_setup = std::move(setup);
        xSemaphoreGive(g_action_mutex);
    }
}

void queue_add_symbol(std::string symbol)
{
    if (g_action_mutex && xSemaphoreTake(g_action_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_pending_add_symbol = std::move(symbol);
        xSemaphoreGive(g_action_mutex);
    }
}

void queue_remove_symbol(std::size_t index)
{
    if (g_action_mutex && xSemaphoreTake(g_action_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        g_pending_remove_symbol = index;
        xSemaphoreGive(g_action_mutex);
    }
}

void queue_flag(bool& flag)
{
    if (g_action_mutex && xSemaphoreTake(g_action_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        flag = true;
        xSemaphoreGive(g_action_mutex);
    }
}

bool ensure_api_token_ready()
{
    if (std::time(nullptr) <= 1'700'000'000) {
        wait_for_time_sync();
    }
    return g_settings.api_key.complete();
}

std::string watchlist_stream_key()
{
    return g_settings.watchlist.serialize();
}

bool ensure_quote_stream()
{
    if (g_settings.watchlist.empty()) {
        return true;
    }

    const std::string key = watchlist_stream_key();
    if (g_longbridge.quote_stream_connected() && key == g_stream_watchlist_key) {
        return true;
    }

    if (!g_longbridge.quote_stream_connected()) {
        g_longbridge.disconnect();
        g_stream_watchlist_key.clear();

        tab5::longbridge::SocketToken socket_token;
        auto result = g_longbridge.fetch_socket_token(socket_token);
        if (!result.ok()) {
            g_ui.set_error(std::string("socket token failed: ")
                           + tab5::longbridge::client_error_text(result.error) + " " + result.message);
            return false;
        }

        result = g_longbridge.connect_quote_stream(socket_token);
        if (!result.ok()) {
            g_ui.set_error(std::string("quote stream connect failed: ")
                           + tab5::longbridge::client_error_text(result.error) + " " + result.message);
            return false;
        }
    }

    auto result = g_longbridge.subscribe_quotes(g_settings.watchlist.symbols());
    if (!result.ok()) {
        g_longbridge.disconnect();
        g_quote_stream_online = false;
        g_stream_watchlist_key.clear();
        g_ui.set_error(std::string("quote subscribe failed: ")
                       + tab5::longbridge::client_error_text(result.error) + " " + result.message);
        return false;
    }

    g_stream_watchlist_key = key;
    g_quote_stream_online = true;
    set_connection_status("quote stream live");
    return true;
}

void process_quote_deltas()
{
    auto [deltas, dropped] = take_quote_deltas();
    if (dropped > 0) {
        g_quote_store.mark_all_stale();
        g_ui.set_error("quote push queue overflow; data marked stale");
        redraw_watchlist();
    }
    if (deltas.empty()) {
        return;
    }

    bool changed = false;
    for (const auto& delta : deltas) {
        changed = g_quote_store.apply_delta(delta) || changed;
    }
    if (changed) {
        if (dropped == 0) {
            g_ui.clear_error();
        }
        redraw_watchlist();
    }
}

bool refresh_quotes()
{
    if (!quote_auth_configured()) {
        g_ui.set_error("Longbridge API key credentials are incomplete");
        return false;
    }
    if (!wifi_is_connected()) {
        g_quote_store.mark_all_stale();
        g_ui.set_error("Wi-Fi offline; quote refresh paused");
        redraw_watchlist();
        return false;
    }
    if (!ensure_api_token_ready()) {
        return false;
    }

    configure_longbridge();
    std::vector<tab5::quotes::QuoteSnapshot> snapshots;
    const auto result =
        g_longbridge.fetch_quote_snapshots(g_settings.watchlist.symbols(), snapshots);
    if (!result.ok()) {
        g_quote_store.mark_all_stale();
        g_ui.set_error(std::string("quote refresh failed: ")
                       + tab5::longbridge::client_error_text(result.error) + " " + result.message);
        redraw_watchlist();
        return false;
    }

    const std::int64_t now_ms = esp_timer_get_time() / 1000;
    for (auto snapshot : snapshots) {
        snapshot.received_at_ms = now_ms;
        g_quote_store.apply_snapshot(snapshot);
    }
    redraw_watchlist();
    if (!ensure_quote_stream()) {
        return false;
    }
    g_quote_retry_backoff.reset();
    g_ui.clear_error();
    return true;
}

void schedule_next_quote_refresh(bool last_attempt_ok, std::int64_t now_ms)
{
    if (last_attempt_ok) {
        g_quote_retry_backoff.reset();
        g_next_quote_refresh_ms = now_ms + kLiveSnapshotRefreshMs;
        return;
    }

    if (!quote_auth_configured() || !wifi_is_connected()) {
        g_next_quote_refresh_ms = 0;
        return;
    }

    g_next_quote_refresh_ms = now_ms + g_quote_retry_backoff.next_delay_ms();
}

void refresh_quotes_and_schedule()
{
    const std::int64_t now_ms = esp_timer_get_time() / 1000;
    schedule_next_quote_refresh(refresh_quotes(), now_ms);
}

void handle_wifi_state(std::int64_t now_ms)
{
    if (!g_wifi_events) {
        return;
    }

    const bool connected = wifi_is_connected();
    if (connected) {
        if (!g_wifi_online) {
            g_wifi_online = true;
            g_wifi_backoff.reset();
            g_next_wifi_reconnect_ms = 0;
            g_next_quote_refresh_ms = 0;
            set_connection_status("wifi connected");
            start_sd_file_manager_once();
        }
        return;
    }

    if (g_wifi_online) {
        g_wifi_online = false;
        g_quote_stream_online = false;
        g_longbridge.disconnect();
        g_stream_watchlist_key.clear();
        g_quote_store.mark_all_stale();
        g_ui.set_error("Wi-Fi disconnected; reconnecting");
        redraw_watchlist();
    }

    if (!g_settings.wifi.complete() || !g_wifi_started) {
        return;
    }

    const EventBits_t bits = xEventGroupGetBits(g_wifi_events);
    if ((bits & kWifiFailedBit) == 0 || now_ms < g_next_wifi_reconnect_ms) {
        return;
    }

    xEventGroupClearBits(g_wifi_events, kWifiFailedBit);
    set_connection_status("wifi reconnecting");
    const esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
        ESP_LOGW(kTag, "wifi reconnect failed to start: %s", esp_err_to_name(err));
        xEventGroupSetBits(g_wifi_events, kWifiFailedBit);
    }
    g_next_wifi_reconnect_ms = now_ms + g_wifi_backoff.next_delay_ms();
}

void handle_quote_stream_state(std::int64_t now_ms)
{
    const bool connected = g_longbridge.quote_stream_connected();
    if (connected) {
        g_quote_stream_online = true;
        return;
    }

    if (!g_quote_stream_online) {
        return;
    }

    g_quote_stream_online = false;
    g_stream_watchlist_key.clear();
    g_quote_store.mark_all_stale();
    g_ui.set_error("quote stream disconnected; reconnecting");
    redraw_watchlist();
    if (wifi_is_connected() && quote_auth_configured()) {
        g_next_quote_refresh_ms = now_ms;
    }
}

void run_scheduled_quote_refresh(std::int64_t now_ms)
{
    if (!quote_auth_configured() || !wifi_is_connected()) {
        return;
    }
    if (g_next_quote_refresh_ms == 0) {
        g_next_quote_refresh_ms = now_ms;
    }
    if (now_ms < g_next_quote_refresh_ms) {
        return;
    }
    schedule_next_quote_refresh(refresh_quotes(), now_ms);
}

void handle_setup_save(PendingSetup setup)
{
    const bool endpoint_changed = setup.endpoint != g_settings.endpoint_region;
    const bool api_key_config_changed =
        setup.api_key.app_key != g_settings.api_key.app_key
        || setup.api_key.app_secret != g_settings.api_key.app_secret
        || setup.api_key.access_token != g_settings.api_key.access_token;
    const bool auth_config_changed = endpoint_changed || api_key_config_changed;

    g_settings.wifi = std::move(setup.wifi);
    g_settings.endpoint_region = setup.endpoint;
    g_settings.api_key = std::move(setup.api_key);
    if (auth_config_changed) {
        g_longbridge.disconnect();
        g_quote_stream_online = false;
        g_stream_watchlist_key.clear();
    }
    const esp_err_t err = g_settings_store.save(g_settings);
    if (err != ESP_OK) {
        g_ui.set_error(std::string("failed to save setup: ") + esp_err_to_name(err));
        return;
    }
    g_ui.clear_error();
    if (g_settings.wifi.complete()) {
        if (start_wifi_sta(g_settings.wifi)) {
            set_connection_status("wifi connected");
            if (quote_auth_configured()) {
                refresh_quotes_and_schedule();
            }
        } else {
            g_ui.set_error("Wi-Fi did not connect; check SSID/password");
        }
    }
    g_ui.show_setup(g_settings);
    g_rendered_status.clear();
    refresh_status_line();
}

void handle_add_symbol(const std::string& text)
{
    const auto symbol = tab5::quotes::SecuritySymbol::parse(text);
    if (!symbol) {
        g_ui.set_error("invalid symbol; use AAPL.US, 700.HK, 600519.SH, or 000001.SZ");
        return;
    }
    const auto result = g_settings.watchlist.add(*symbol);
    if (result == tab5::quotes::WatchlistAddResult::Full) {
        g_ui.set_error("watchlist is full; Longbridge quote subscription max is 500");
        return;
    }
    if (result == tab5::quotes::WatchlistAddResult::AlreadyExists) {
        g_ui.set_error("symbol already exists");
        return;
    }
    save_settings_and_redraw();
    refresh_quotes_and_schedule();
}

void handle_remove_symbol(std::size_t index)
{
    if (index >= g_settings.watchlist.symbols().size()) {
        return;
    }
    const auto symbol = g_settings.watchlist.symbols()[index];
    g_settings.watchlist.remove(symbol);
    g_quote_store.clear();
    g_longbridge.disconnect();
    g_quote_stream_online = false;
    g_stream_watchlist_key.clear();
    save_settings_and_redraw();
    refresh_quotes_and_schedule();
}

void handle_reset_settings()
{
    g_longbridge.disconnect();
    g_quote_stream_online = false;
    g_stream_watchlist_key.clear();
    g_next_quote_refresh_ms = 0;
    const esp_err_t err = g_settings_store.reset();
    if (err != ESP_OK) {
        g_ui.set_error(std::string("failed to reset settings: ") + esp_err_to_name(err));
        return;
    }
    g_settings = tab5::settings::AppSettings {};
    seed_default_watchlist();
    g_connection_status = "setup";
    g_ui.show_setup(g_settings);
    g_rendered_status.clear();
    refresh_status_line();
}

void process_pending_actions()
{
    std::optional<PendingSetup> setup;
    std::optional<std::string> add_symbol;
    std::optional<std::size_t> remove_symbol;
    bool refresh = false;
    bool reset = false;

    if (g_action_mutex && xSemaphoreTake(g_action_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        setup = std::move(g_pending_setup);
        g_pending_setup.reset();
        add_symbol = std::move(g_pending_add_symbol);
        g_pending_add_symbol.reset();
        remove_symbol = std::move(g_pending_remove_symbol);
        g_pending_remove_symbol.reset();
        refresh = g_pending_refresh_quotes;
        reset = g_pending_reset_settings;
        g_pending_refresh_quotes = false;
        g_pending_reset_settings = false;
        xSemaphoreGive(g_action_mutex);
    }

    if (setup) {
        handle_setup_save(std::move(*setup));
    }
    if (add_symbol) {
        handle_add_symbol(*add_symbol);
    }
    if (remove_symbol) {
        handle_remove_symbol(*remove_symbol);
    }
    if (refresh) {
        refresh_quotes_and_schedule();
    }
    if (reset) {
        handle_reset_settings();
    }
}

tab5::ui::TerminalUiCallbacks callbacks()
{
    tab5::ui::TerminalUiCallbacks callbacks;
    callbacks.save_setup =
        [](tab5::settings::WifiCredentials wifi,
           tab5::longbridge::EndpointRegion endpoint,
           tab5::settings::LongbridgeApiKeyCredentials api_key) {
            queue_setup_save({
                std::move(wifi),
                endpoint,
                std::move(api_key),
            });
        };

    callbacks.add_symbol = [](const std::string& text) {
        queue_add_symbol(text);
    };

    callbacks.remove_symbol_at = [](std::size_t index) {
        queue_remove_symbol(index);
    };

    callbacks.refresh_quotes = []() {
        queue_flag(g_pending_refresh_quotes);
    };

    callbacks.reset_settings = []() {
        queue_flag(g_pending_reset_settings);
    };

    return callbacks;
}

} // namespace

extern "C" void app_main(void)
{
    ESP_LOGI(kTag, "booting Tab5 Longbridge stock terminal");
    g_wifi_events = xEventGroupCreate();
    g_quote_delta_mutex = xSemaphoreCreateMutex();
    g_action_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(g_settings_store.initialize());

    if (g_settings_store.load(g_settings) != ESP_OK) {
        g_settings = tab5::settings::AppSettings {};
    }
    const bool imported_sd_settings = import_settings_from_sd_card();
    seed_default_watchlist();
    g_quote_store.set_watchlist(g_settings.watchlist);

    lv_obj_t* screen = initialize_display();
    ESP_LOGI(kTag, "initializing terminal UI");
    g_ui.init(screen, callbacks());
    initialize_keyboard();
    initialize_tab5_keyboard_i2c_input();
    if (!g_boot_warning.empty()) {
        g_ui.set_error(g_boot_warning);
    }

    if (!g_settings.onboarding_complete()) {
        ESP_LOGI(kTag, "showing setup screen; onboarding settings are incomplete");
        g_connection_status = "setup";
        g_ui.show_setup(g_settings);
        resume_display_refresh(screen);
    } else {
        ESP_LOGI(kTag,
                 "showing watchlist before network startup%s",
                 imported_sd_settings ? " after SD settings import" : "");
        redraw_watchlist();
        resume_display_refresh(screen);
        set_connection_status("wifi connecting");
        start_wifi_sta(g_settings.wifi);
        refresh_quotes_and_schedule();
    }

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        process_pending_actions();
        process_quote_deltas();
        const std::int64_t now_ms = esp_timer_get_time() / 1000;
        handle_wifi_state(now_ms);
        handle_quote_stream_state(now_ms);
        refresh_status_line();
        if (g_quote_store.mark_stale_older_than(now_ms, kQuoteStaleAfterMs)) {
            redraw_watchlist();
        }
        run_scheduled_quote_refresh(now_ms);
    }
}
