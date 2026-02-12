#pragma once

#include "tui.hpp"

class ProfileManager : public View {
private:
    int scroll_offset;
    int selected_idx;
    std::string message;
    int message_timer;
    
public:
    ProfileManager(TUI* parent);
    
    void draw() override;
    void handle_input(int ch) override;
    
private:
    void activate_selected_profile();
    void show_new_profile_dialog();
    void duplicate_selected_profile();
    void rename_selected_profile();
    void delete_selected_profile();
    void show_message(const std::string& msg);
};
