#pragma once

#include "quotes/quote_store.hpp"
#include "settings/app_settings.hpp"

#include <cstdint>
#include <cstddef>
#include <functional>
#include <string>
#include <vector>

#include "lvgl.h"

namespace tab5::ui {

enum class UiMode {
    Setup,
    Watchlist,
};

struct TerminalUiCallbacks {
    std::function<void(settings::WifiCredentials,
                       longbridge::EndpointRegion,
                       settings::LongbridgeApiKeyCredentials)>
        save_setup;
    std::function<void(const std::string&)> add_symbol;
    std::function<void(std::size_t)> remove_symbol_at;
    std::function<void()> refresh_quotes;
    std::function<void()> reset_settings;
};

class TerminalUi {
public:
    void init(lv_obj_t* screen, TerminalUiCallbacks callbacks = {});
    void show_setup(const settings::AppSettings& settings);
    void show_watchlist(const std::vector<quotes::QuoteSnapshot>& rows);
    void set_connection_status(const std::string& status);
    void set_error(const std::string& error);
    void clear_error();
    lv_group_t* focus_group() const { return focus_group_; }

private:
    TerminalUiCallbacks callbacks_;
    lv_obj_t* root_ { nullptr };
    lv_obj_t* title_ { nullptr };
    lv_obj_t* status_ { nullptr };
    lv_obj_t* error_ { nullptr };
    lv_obj_t* content_ { nullptr };
    lv_obj_t* table_ { nullptr };
    lv_obj_t* detail_ { nullptr };
    lv_obj_t* detail_text_ { nullptr };
    lv_obj_t* input_ { nullptr };
    lv_obj_t* ssid_input_ { nullptr };
    lv_obj_t* password_input_ { nullptr };
    lv_obj_t* api_app_key_input_ { nullptr };
    lv_obj_t* api_app_secret_input_ { nullptr };
    lv_obj_t* api_access_token_input_ { nullptr };
    lv_obj_t* endpoint_dropdown_ { nullptr };
    lv_group_t* focus_group_ { nullptr };
    std::vector<quotes::QuoteSnapshot> rows_;
    std::size_t selected_row_index_ { 0 };
    UiMode mode_ { UiMode::Setup };

    void reset_content();
    void build_shell();
    void build_watchlist_table(const std::vector<quotes::QuoteSnapshot>& rows);
    void update_selected_row_from_table();
    void update_detail_text();
    lv_obj_t* create_input(lv_obj_t* parent,
                           const char* placeholder,
                           const std::string& value,
                           lv_coord_t x,
                           lv_coord_t y,
                           lv_coord_t width,
                           lv_obj_t** object_slot,
                           lv_obj_t** label_slot,
                           std::string* text_slot,
                           bool password = false);
    void add_focus(lv_obj_t* object);
    void handle_input_key(lv_obj_t* object, std::uint32_t key);
    void refresh_input(lv_obj_t* object);
    std::string* input_text_for(lv_obj_t* object);
    lv_obj_t* input_label_for(lv_obj_t* object) const;
    const char* input_placeholder_for(lv_obj_t* object) const;
    bool input_is_password(lv_obj_t* object) const;
    std::size_t input_max_length_for(lv_obj_t* object) const;
    void save_setup_from_controls();
    void add_symbol_from_input();

    lv_obj_t* input_label_ { nullptr };
    lv_obj_t* ssid_input_label_ { nullptr };
    lv_obj_t* password_input_label_ { nullptr };
    lv_obj_t* api_app_key_input_label_ { nullptr };
    lv_obj_t* api_app_secret_input_label_ { nullptr };
    lv_obj_t* api_access_token_input_label_ { nullptr };
    std::string input_text_;
    std::string ssid_input_text_;
    std::string password_input_text_;
    std::string api_app_key_input_text_;
    std::string api_app_secret_input_text_;
    std::string api_access_token_input_text_;
};

} // namespace tab5::ui
