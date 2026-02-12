#pragma once

#include "tui_common.hpp"

// Main TUI class
class TUI {
private:
    bool running;
    ViewType current_view;
    std::vector<std::unique_ptr<View>> views;
    Config config;
    std::vector<std::shared_ptr<DeviceInfo>> devices;
    std::vector<BindingDisplay> bindings;
    bool config_modified;
    
    // UI state
    int screen_height, screen_width;
    std::unique_ptr<Window> header_win;
    std::unique_ptr<Window> main_win;
    std::unique_ptr<Window> status_win;
    
public:
    TUI();
    ~TUI();
    
    void init_ncurses();
    void create_windows();
    void load_config();
    void save_config();
    void create_views();
    void scan_devices();
    void scan_new_devices();
    void refresh_bindings();
    void draw_header();
    void draw_status();
    void run();
    void handle_global_input(int ch);
    void show_help();
    
    // Getters
    Config& get_config() { return config; }
    std::vector<std::shared_ptr<DeviceInfo>>& get_devices() { return devices; }
    std::vector<BindingDisplay>& get_bindings() { return bindings; }
    int get_screen_width() const { return screen_width; }
    int get_screen_height() const { return screen_height; }
    Window* get_main_win() { return main_win.get(); }
    void mark_modified() { config_modified = true; }
    bool is_modified() const { return config_modified; }
    void set_view(ViewType view) { current_view = view; }
};
