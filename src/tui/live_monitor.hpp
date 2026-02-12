#pragma once

#include "tui.hpp"

class LiveMonitor : public View {
private:
    bool monitoring;
    std::thread monitor_thread;
    std::map<std::string, int> last_values;
    
    static bool is_monitored(const std::shared_ptr<DeviceInfo>& dev);
    static std::string friendly_button_name(const std::string& role, int btn);
    void draw_axes_panel(Window* win, int start_col, int panel_width, int start_row, int max_row);
    void draw_buttons_panel(Window* win, int start_col, int panel_width, int start_row, int max_row);
    
public:
    LiveMonitor(TUI* parent);
    ~LiveMonitor();
    
    void draw() override;
    void handle_input(int ch) override;
    void start_monitoring();
    void stop_monitoring();
};
