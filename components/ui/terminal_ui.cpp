#include "ui/terminal_ui.hpp"

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <algorithm>
#include <utility>

#if __has_include("bsp/esp-bsp.h")
#include "bsp/esp-bsp.h"
#endif

namespace tab5::ui {
namespace {

constexpr std::uint32_t kPageBg = 0xf2f5f8;
constexpr std::uint32_t kPanelBg = 0xffffff;
constexpr std::uint32_t kSubtleBg = 0xe9eef3;
constexpr std::uint32_t kText = 0x17202a;
constexpr std::uint32_t kMutedText = 0x52606d;
constexpr std::uint32_t kBorder = 0xb9c5d0;
constexpr std::uint32_t kAccent = 0x1565c0;
constexpr std::uint32_t kAccentDark = 0x0d47a1;
constexpr std::uint32_t kError = 0xb42318;

std::string money(const std::optional<double>& value)
{
    if (!value.has_value()) {
        return "--";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.3f", *value);
    return buffer;
}

std::string number(const std::optional<double>& value)
{
    if (!value.has_value()) {
        return "--";
    }
    char buffer[32];
    if (std::fabs(*value) >= 1'000'000.0) {
        std::snprintf(buffer, sizeof(buffer), "%.2fm", *value / 1'000'000.0);
    } else {
        std::snprintf(buffer, sizeof(buffer), "%.0f", *value);
    }
    return buffer;
}

std::string percent(const std::optional<double>& value)
{
    if (!value.has_value()) {
        return "--";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%+.2f%%", *value);
    return buffer;
}

void set_text(lv_obj_t* label, const std::string& text)
{
    if (label) {
        lv_label_set_text(label, text.c_str());
    }
}

lv_color_t color_hex(std::uint32_t hex)
{
    return lv_color_hex(hex);
}

lv_obj_t* make_label(lv_obj_t* parent, const char* text, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, color_hex(kMutedText), LV_PART_MAIN);
    lv_obj_align(label, LV_ALIGN_TOP_LEFT, x, y);
    return label;
}

lv_obj_t* make_textarea(lv_obj_t* parent,
                        const char* placeholder,
                        const std::string& value,
                        lv_coord_t x,
                        lv_coord_t y,
                        lv_coord_t width,
                        bool password = false)
{
    lv_obj_t* textarea = lv_textarea_create(parent);
    lv_obj_set_size(textarea, width, 40);
    lv_textarea_set_one_line(textarea, true);
    lv_textarea_set_placeholder_text(textarea, placeholder);
    lv_textarea_set_text(textarea, value.c_str());
    lv_textarea_set_password_mode(textarea, password);
    lv_obj_set_style_bg_color(textarea, color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_color(textarea, color_hex(kText), LV_PART_MAIN);
    lv_obj_set_style_border_color(textarea, color_hex(kBorder), LV_PART_MAIN);
    lv_obj_set_style_border_width(textarea, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(textarea, 3, LV_PART_MAIN);
    lv_obj_align(textarea, LV_ALIGN_TOP_LEFT, x, y);
    return textarea;
}

lv_obj_t* make_button(lv_obj_t* parent, const char* text, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t* button = lv_button_create(parent);
    lv_obj_set_size(button, 150, 42);
    lv_obj_set_style_bg_color(button, color_hex(kAccent), LV_PART_MAIN);
    lv_obj_set_style_text_color(button, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_border_width(button, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(button, 3, LV_PART_MAIN);
    lv_obj_align(button, LV_ALIGN_TOP_LEFT, x, y);
    lv_obj_t* label = lv_label_create(button);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_white(), LV_PART_MAIN);
    lv_obj_center(label);
    return button;
}

class DisplayLock {
public:
    DisplayLock()
    {
#if __has_include("bsp/esp-bsp.h")
        locked_ = bsp_display_lock(5000);
#endif
    }

    ~DisplayLock()
    {
#if __has_include("bsp/esp-bsp.h")
        if (locked_) {
            bsp_display_unlock();
        }
#endif
    }

private:
    bool locked_ { true };
};

} // namespace

void TerminalUi::init(lv_obj_t* screen, TerminalUiCallbacks callbacks)
{
    DisplayLock lock;
    callbacks_ = std::move(callbacks);
    root_ = screen;
    focus_group_ = lv_group_create();
    lv_group_set_default(focus_group_);
    build_shell();
}

void TerminalUi::build_shell()
{
    if (!root_) {
        return;
    }

    lv_obj_clean(root_);
    lv_obj_set_style_bg_color(root_, color_hex(kPageBg), LV_PART_MAIN);
    lv_obj_set_style_text_color(root_, color_hex(kText), LV_PART_MAIN);
    lv_obj_set_style_pad_all(root_, 10, LV_PART_MAIN);

    title_ = lv_label_create(root_);
    lv_label_set_text(title_, "Tab5 Longbridge Terminal");
    lv_obj_set_style_text_font(title_, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(title_, color_hex(0x0f1720), LV_PART_MAIN);
    lv_obj_align(title_, LV_ALIGN_TOP_LEFT, 0, 0);

    status_ = lv_label_create(root_);
    lv_label_set_text(status_, "offline");
    lv_obj_set_style_text_color(status_, color_hex(kAccentDark), LV_PART_MAIN);
    lv_obj_align(status_, LV_ALIGN_TOP_RIGHT, 0, 2);

    error_ = lv_label_create(root_);
    lv_label_set_text(error_, "");
    lv_obj_set_width(error_, 1260);
    lv_obj_set_style_text_color(error_, color_hex(kError), LV_PART_MAIN);
    lv_obj_align(error_, LV_ALIGN_TOP_LEFT, 0, 30);

    content_ = lv_obj_create(root_);
    lv_obj_set_size(content_, 1260, 650);
    lv_obj_align(content_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(content_, color_hex(kPanelBg), LV_PART_MAIN);
    lv_obj_set_style_text_color(content_, color_hex(kText), LV_PART_MAIN);
    lv_obj_set_style_border_color(content_, color_hex(kBorder), LV_PART_MAIN);
    lv_obj_set_style_border_width(content_, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(content_, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(content_, 8, LV_PART_MAIN);
}

void TerminalUi::reset_content()
{
    if (!content_) {
        return;
    }
    lv_obj_clean(content_);
    table_ = nullptr;
    detail_ = nullptr;
    detail_text_ = nullptr;
    input_ = nullptr;
    ssid_input_ = nullptr;
    password_input_ = nullptr;
    client_id_input_ = nullptr;
    redirect_uri_input_ = nullptr;
    endpoint_dropdown_ = nullptr;
}

void TerminalUi::show_setup(const settings::AppSettings& settings)
{
    DisplayLock lock;
    mode_ = UiMode::Setup;
    reset_content();
    set_text(status_, "setup");

    lv_obj_t* heading = lv_label_create(content_);
    lv_label_set_text(heading, "Device setup");
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(heading, color_hex(kText), LV_PART_MAIN);
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 4, 4);

    make_label(content_, "Wi-Fi SSID", 4, 54);
    ssid_input_ = make_textarea(content_, "Office Wi-Fi", settings.wifi.ssid, 170, 42, 420);
    add_focus(ssid_input_);

    make_label(content_, "Wi-Fi password", 4, 108);
    password_input_ =
        make_textarea(content_, "password", settings.wifi.password, 170, 96, 420, true);
    add_focus(password_input_);

    make_label(content_, "Endpoint", 4, 162);
    endpoint_dropdown_ = lv_dropdown_create(content_);
    lv_dropdown_set_options(endpoint_dropdown_, "global\ncn");
    lv_dropdown_set_selected(endpoint_dropdown_,
                             settings.endpoint_region == longbridge::EndpointRegion::MainlandChina ? 1 : 0);
    lv_obj_set_size(endpoint_dropdown_, 180, 40);
    lv_obj_set_style_bg_color(endpoint_dropdown_, color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_color(endpoint_dropdown_, color_hex(kText), LV_PART_MAIN);
    lv_obj_set_style_border_color(endpoint_dropdown_, color_hex(kBorder), LV_PART_MAIN);
    lv_obj_set_style_border_width(endpoint_dropdown_, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(endpoint_dropdown_, 3, LV_PART_MAIN);
    lv_obj_align(endpoint_dropdown_, LV_ALIGN_TOP_LEFT, 170, 150);
    add_focus(endpoint_dropdown_);

    make_label(content_, "Longbridge client_id", 4, 216);
    client_id_input_ = make_textarea(content_, "OAuth client id", settings.longbridge_client_id, 170, 204, 620);
    add_focus(client_id_input_);

    make_label(content_, "Callback URI", 4, 270);
    redirect_uri_input_ =
        make_textarea(content_, "http://tab5-stock.local/oauth/callback", settings.oauth_redirect_uri, 170, 258, 760);
    add_focus(redirect_uri_input_);

    lv_obj_t* save = make_button(content_, "Save", 170, 324);
    add_focus(save);
    lv_obj_add_event_cb(
        save,
        [](lv_event_t* event) {
            auto* self = static_cast<TerminalUi*>(lv_event_get_user_data(event));
            self->save_setup_from_controls();
        },
        LV_EVENT_CLICKED,
        this);

    lv_obj_t* oauth = make_button(content_, "OAuth", 338, 324);
    add_focus(oauth);
    lv_obj_add_event_cb(
        oauth,
        [](lv_event_t* event) {
            auto* self = static_cast<TerminalUi*>(lv_event_get_user_data(event));
            if (self->callbacks_.start_oauth) {
                self->callbacks_.start_oauth();
            }
        },
        LV_EVENT_CLICKED,
        this);

    lv_obj_t* reset = make_button(content_, "Reset", 506, 324);
    add_focus(reset);
    lv_obj_add_event_cb(
        reset,
        [](lv_event_t* event) {
            auto* self = static_cast<TerminalUi*>(lv_event_get_user_data(event));
            if (self->callbacks_.reset_settings) {
                self->callbacks_.reset_settings();
            }
        },
        LV_EVENT_CLICKED,
        this);

    lv_obj_t* summary = lv_label_create(content_);
    std::string text = "OAuth: ";
    text += settings.oauth_tokens.empty() ? "not connected" : "connected";
    text += "\nWatchlist: ";
    text += std::to_string(settings.watchlist.size());
    text += " symbols\n\nUse Tab/Enter or touch to move through controls.";
    lv_label_set_text(summary, text.c_str());
    lv_obj_set_width(summary, 540);
    lv_obj_set_style_text_color(summary, color_hex(kMutedText), LV_PART_MAIN);
    lv_obj_align(summary, LV_ALIGN_TOP_LEFT, 4, 390);
}

void TerminalUi::show_oauth_url(const std::string& url)
{
    DisplayLock lock;
    mode_ = UiMode::OAuth;
    reset_content();
    set_text(status_, "oauth");

    lv_obj_t* heading = lv_label_create(content_);
    lv_label_set_text(heading, "Longbridge OAuth");
    lv_obj_set_style_text_font(heading, &lv_font_montserrat_18, LV_PART_MAIN);
    lv_obj_set_style_text_color(heading, color_hex(kText), LV_PART_MAIN);
    lv_obj_align(heading, LV_ALIGN_TOP_LEFT, 4, 4);

    lv_obj_t* instructions = lv_label_create(content_);
    lv_label_set_text(instructions, "Scan the QR code or open the URL to authorize this Tab5.");
    lv_obj_set_style_text_color(instructions, color_hex(kMutedText), LV_PART_MAIN);
    lv_obj_align(instructions, LV_ALIGN_TOP_LEFT, 4, 42);

#if defined(LV_USE_QRCODE) && LV_USE_QRCODE
#if defined(LVGL_VERSION_MAJOR) && LVGL_VERSION_MAJOR >= 9
    lv_obj_t* qr = lv_qrcode_create(content_);
    lv_qrcode_set_size(qr, 220);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, url.c_str(), url.size());
#else
    lv_obj_t* qr = lv_qrcode_create(content_, 220, lv_color_black(), lv_color_white());
    lv_qrcode_update(qr, url.c_str(), url.size());
#endif
    lv_obj_align(qr, LV_ALIGN_TOP_LEFT, 4, 82);
#endif

    lv_obj_t* url_label = lv_label_create(content_);
    lv_label_set_long_mode(url_label, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(url_label, 940);
    lv_label_set_text(url_label, url.c_str());
    lv_obj_set_style_text_color(url_label, color_hex(kText), LV_PART_MAIN);
    lv_obj_align(url_label, LV_ALIGN_TOP_LEFT, 280, 92);
}

void TerminalUi::show_watchlist(const std::vector<quotes::QuoteSnapshot>& rows)
{
    DisplayLock lock;
    mode_ = UiMode::Watchlist;
    rows_ = rows;
    if (selected_row_index_ >= rows_.size()) {
        selected_row_index_ = rows_.empty() ? 0 : rows_.size() - 1;
    }
    reset_content();
    set_text(status_, "watchlist");
    build_watchlist_table(rows_);
}

void TerminalUi::build_watchlist_table(const std::vector<quotes::QuoteSnapshot>& rows)
{
    table_ = lv_table_create(content_);
    lv_obj_set_size(table_, 900, 620);
    lv_obj_align(table_, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_color(table_, color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_border_color(table_, color_hex(kBorder), LV_PART_MAIN);
    lv_obj_set_style_border_width(table_, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(table_, 3, LV_PART_MAIN);
    lv_obj_set_style_text_color(table_, color_hex(kText), LV_PART_MAIN);
    lv_obj_set_style_text_color(table_, color_hex(kText), LV_PART_ITEMS);
    lv_obj_set_style_bg_color(table_, color_hex(0xffffff), LV_PART_ITEMS);
    lv_obj_set_style_border_color(table_, color_hex(0xd7dee6), LV_PART_ITEMS);
    lv_obj_set_style_border_width(table_, 1, LV_PART_ITEMS);
    lv_obj_set_style_pad_left(table_, 8, LV_PART_ITEMS);
    lv_obj_set_style_pad_right(table_, 8, LV_PART_ITEMS);
    lv_obj_set_style_pad_top(table_, 5, LV_PART_ITEMS);
    lv_obj_set_style_pad_bottom(table_, 5, LV_PART_ITEMS);
    lv_table_set_column_count(table_, 8);
    lv_table_set_row_count(table_, static_cast<std::uint32_t>(rows.size() + 1));
    lv_table_set_column_width(table_, 0, 128);
    lv_table_set_column_width(table_, 1, 110);
    lv_table_set_column_width(table_, 2, 100);
    lv_table_set_column_width(table_, 3, 100);
    lv_table_set_column_width(table_, 4, 118);
    lv_table_set_column_width(table_, 5, 105);
    lv_table_set_column_width(table_, 6, 105);
    lv_table_set_column_width(table_, 7, 105);
    add_focus(table_);
    lv_table_set_cell_value(table_, 0, 0, "Symbol");
    lv_table_set_cell_value(table_, 0, 1, "Last");
    lv_table_set_cell_value(table_, 0, 2, "Chg");
    lv_table_set_cell_value(table_, 0, 3, "Chg%");
    lv_table_set_cell_value(table_, 0, 4, "Vol");
    lv_table_set_cell_value(table_, 0, 5, "Open");
    lv_table_set_cell_value(table_, 0, 6, "High");
    lv_table_set_cell_value(table_, 0, 7, "Low");

    for (std::size_t i = 0; i < rows.size(); ++i) {
        const auto row = static_cast<std::uint32_t>(i + 1);
        const auto& quote = rows[i];
        lv_table_set_cell_value(table_, row, 0, quote.symbol.value().c_str());
        lv_table_set_cell_value(table_, row, 1, money(quote.last_price).c_str());
        lv_table_set_cell_value(table_, row, 2, money(quote.change()).c_str());
        lv_table_set_cell_value(table_, row, 3, percent(quote.change_percent()).c_str());
        lv_table_set_cell_value(table_, row, 4, number(quote.volume).c_str());
        lv_table_set_cell_value(table_, row, 5, money(quote.open).c_str());
        lv_table_set_cell_value(table_, row, 6, money(quote.high).c_str());
        lv_table_set_cell_value(table_, row, 7, money(quote.low).c_str());
    }
    if (!rows.empty()) {
        lv_table_set_selected_cell(table_, static_cast<std::uint16_t>(selected_row_index_ + 1), 0);
    }
    lv_obj_add_event_cb(
        table_,
        [](lv_event_t* event) {
            auto* self = static_cast<TerminalUi*>(lv_event_get_user_data(event));
            self->update_selected_row_from_table();
        },
        LV_EVENT_VALUE_CHANGED,
        this);

    detail_ = lv_obj_create(content_);
    lv_obj_set_size(detail_, 330, 620);
    lv_obj_align(detail_, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_color(detail_, color_hex(0xf7f9fc), LV_PART_MAIN);
    lv_obj_set_style_text_color(detail_, color_hex(kText), LV_PART_MAIN);
    lv_obj_set_style_border_color(detail_, color_hex(kBorder), LV_PART_MAIN);
    lv_obj_set_style_border_width(detail_, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(detail_, 4, LV_PART_MAIN);

    detail_text_ = lv_label_create(detail_);
    lv_label_set_long_mode(detail_text_, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(detail_text_, 300);
    lv_obj_set_style_text_color(detail_text_, color_hex(kText), LV_PART_MAIN);
    update_detail_text();
    lv_obj_align(detail_text_, LV_ALIGN_TOP_LEFT, 12, 12);

    lv_obj_t* refresh = make_button(detail_, "Refresh", 12, 486);
    lv_obj_set_size(refresh, 138, 38);
    add_focus(refresh);
    lv_obj_add_event_cb(
        refresh,
        [](lv_event_t* event) {
            auto* self = static_cast<TerminalUi*>(lv_event_get_user_data(event));
            if (self->callbacks_.refresh_quotes) {
                self->callbacks_.refresh_quotes();
            }
        },
        LV_EVENT_CLICKED,
        this);

    lv_obj_t* remove = make_button(detail_, "Remove", 168, 486);
    lv_obj_set_size(remove, 138, 38);
    add_focus(remove);
    lv_obj_add_event_cb(
        remove,
        [](lv_event_t* event) {
            auto* self = static_cast<TerminalUi*>(lv_event_get_user_data(event));
            if (self->callbacks_.remove_symbol_at) {
                self->callbacks_.remove_symbol_at(self->selected_row_index_);
            }
        },
        LV_EVENT_CLICKED,
        this);

    lv_obj_t* reset = make_button(detail_, "Reset", 12, 532);
    lv_obj_set_size(reset, 294, 36);
    add_focus(reset);
    lv_obj_add_event_cb(
        reset,
        [](lv_event_t* event) {
            auto* self = static_cast<TerminalUi*>(lv_event_get_user_data(event));
            if (self->callbacks_.reset_settings) {
                self->callbacks_.reset_settings();
            }
        },
        LV_EVENT_CLICKED,
        this);

    input_ = lv_textarea_create(detail_);
    lv_obj_set_size(input_, 210, 40);
    lv_textarea_set_one_line(input_, true);
    lv_textarea_set_placeholder_text(input_, "AAPL.US");
    lv_obj_set_style_bg_color(input_, color_hex(0xffffff), LV_PART_MAIN);
    lv_obj_set_style_text_color(input_, color_hex(kText), LV_PART_MAIN);
    lv_obj_set_style_border_color(input_, color_hex(kBorder), LV_PART_MAIN);
    lv_obj_set_style_border_width(input_, 1, LV_PART_MAIN);
    lv_obj_set_style_radius(input_, 3, LV_PART_MAIN);
    lv_obj_align(input_, LV_ALIGN_BOTTOM_LEFT, 12, -8);
    add_focus(input_);

    lv_obj_t* add = make_button(detail_, "Add", 232, 572);
    lv_obj_set_size(add, 74, 40);
    add_focus(add);
    lv_obj_add_event_cb(
        add,
        [](lv_event_t* event) {
            auto* self = static_cast<TerminalUi*>(lv_event_get_user_data(event));
            self->add_symbol_from_input();
        },
        LV_EVENT_CLICKED,
        this);
}

void TerminalUi::update_selected_row_from_table()
{
    if (!table_ || rows_.empty()) {
        selected_row_index_ = 0;
        update_detail_text();
        return;
    }

    std::uint32_t row = LV_TABLE_CELL_NONE;
    std::uint32_t col = LV_TABLE_CELL_NONE;
    lv_table_get_selected_cell(table_, &row, &col);
    if (row == LV_TABLE_CELL_NONE || row == 0) {
        return;
    }

    selected_row_index_ = std::min<std::size_t>(row - 1, rows_.size() - 1);
    update_detail_text();
}

void TerminalUi::update_detail_text()
{
    if (!detail_text_) {
        return;
    }

    if (rows_.empty()) {
        lv_label_set_text(
            detail_text_,
            "No symbols yet.\n\nType a ticker such as AAPL.US, 700.HK, 600519.SH, or 000001.SZ.");
        return;
    }

    const auto& quote = rows_[std::min(selected_row_index_, rows_.size() - 1)];
    std::string text = quote.symbol.value();
    text += "\nStatus: ";
    text += quote.trade_status.empty() ? "--" : quote.trade_status;
    text += "\nSession: ";
    text += quote.session.empty() ? "--" : quote.session;
    text += "\nTurnover: ";
    text += number(quote.turnover);
    text += "\nFreshness: ";
    text += quote.stale ? "stale" : "live";
    lv_label_set_text(detail_text_, text.c_str());
}

void TerminalUi::set_connection_status(const std::string& status)
{
    DisplayLock lock;
    set_text(status_, status);
}

void TerminalUi::set_error(const std::string& error)
{
    DisplayLock lock;
    set_text(error_, error);
}

void TerminalUi::clear_error()
{
    DisplayLock lock;
    set_text(error_, "");
}

void TerminalUi::add_focus(lv_obj_t* object)
{
    if (focus_group_ && object) {
        lv_group_add_obj(focus_group_, object);
    }
}

void TerminalUi::save_setup_from_controls()
{
    if (!callbacks_.save_setup || !ssid_input_ || !password_input_ || !client_id_input_
        || !redirect_uri_input_ || !endpoint_dropdown_) {
        return;
    }

    settings::WifiCredentials wifi;
    wifi.ssid = lv_textarea_get_text(ssid_input_);
    wifi.password = lv_textarea_get_text(password_input_);

    const auto endpoint = lv_dropdown_get_selected(endpoint_dropdown_) == 1
        ? longbridge::EndpointRegion::MainlandChina
        : longbridge::EndpointRegion::Global;

    callbacks_.save_setup(wifi,
                          endpoint,
                          lv_textarea_get_text(client_id_input_),
                          lv_textarea_get_text(redirect_uri_input_));
}

void TerminalUi::add_symbol_from_input()
{
    if (!callbacks_.add_symbol || !input_) {
        return;
    }
    callbacks_.add_symbol(lv_textarea_get_text(input_));
    lv_textarea_set_text(input_, "");
}

} // namespace tab5::ui
