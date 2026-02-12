#pragma once

#include "tui.hpp"

class DeviceDashboard : public View {
private:
    int scroll_offset;
    
public:
    DeviceDashboard(TUI* parent);
    
    void draw() override;
    void handle_input(int ch) override;
    
private:
    void toggle_role(const std::shared_ptr<DeviceInfo>& dev, const std::string& role);
    void launch_full_setup_wizard();
    void show_device_details(const std::shared_ptr<DeviceInfo>& dev);
};
