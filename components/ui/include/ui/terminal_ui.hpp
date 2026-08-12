#pragma once

#include "quotes/quote_store.hpp"
#include "settings/app_settings.hpp"

#include <functional>
#include <string>
#include <vector>

#include "lvgl.h"

namespace tab5::ui {

enum class UiMode {
    Setup,
    OAuth,
    Watchlist,
};

struct TerminalUiCallbacks {
    std::function<void(settings::WifiCredentials,
                       longbridge::EndpointRegion,
                       std::string,
                       std::string)>
        save_setup;
    std::function<void()> start_oauth;
    std::function<void(const std::string&)> add_symbol;
    std::function<void(std::size_t)> remove_symbol_at;
    std::function<void()> refresh_quotes;
    std::function<void()> reset_settings;
};

class TerminalUi {
public:
    void init(lv_obj_t* screen, TerminalUiCallbacks callbacks = {});
    void show_setup(const settings::AppSettings& settings);
    void show_oauth_url(const std::string& url);
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
    lv_obj_t* client_id_input_ { nullptr };
    lv_obj_t* redirect_uri_input_ { nullptr };
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
    void add_focus(lv_obj_t* object);
    void save_setup_from_controls();
    void add_symbol_from_input();
};

} // namespace tab5::ui
