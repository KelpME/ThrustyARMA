#include "profile_manager.hpp"
#include <sys/stat.h>
#include <ctime>

ProfileManager::ProfileManager(TUI* parent) : View(parent, ViewType::PROFILES), 
                               scroll_offset(0), selected_idx(0), message_timer(0) {}

void ProfileManager::draw() {
    if (!needs_redraw) return;
    
    auto& config = tui->get_config();
    auto* main_win = tui->get_main_win();
    int height = main_win->get_height();
    int width = main_win->get_width();
    
    main_win->clear();
    
    // Title
    main_win->print(1, 2, "Profile Manager", COLOR_PAIR(CP_HEADER) | A_BOLD);
    std::string config_path = ConfigManager::get_config_path();
    main_win->print(2, 2, "Config: " + config_path, COLOR_PAIR(CP_DEFAULT) | A_DIM);
    
    // Show last-saved date from config file mtime
    struct stat st;
    std::string saved_str = "never";
    if (stat(config_path.c_str(), &st) == 0) {
        char timebuf[64];
        struct tm* tm_info = localtime(&st.st_mtime);
        strftime(timebuf, sizeof(timebuf), "%Y-%m-%d %H:%M", tm_info);
        saved_str = timebuf;
    }
    main_win->print(3, 2, "Last saved: " + saved_str, COLOR_PAIR(CP_DEFAULT) | A_DIM);
    main_win->print(4, 2, "Active: " + config.active_profile, COLOR_PAIR(CP_SUCCESS));
    
    // Dynamic column layout
    int usable = width - 4;
    int col_indicator = 2;
    int col_name = 4;
    int col_bindings = col_name + std::max(24, usable * 35 / 100);
    int col_desc = col_bindings + 12;
    int name_width = col_bindings - col_name - 2;
    int desc_width = std::max(10, width - col_desc - 2);
    
    // Column headers
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "  %-*s%-12s%s",
             col_bindings - col_name, "Profile Name",
             "Bindings", "Description");
    main_win->print(6, 2, hdr);
    main_win->print(7, 2, std::string(width - 4, '-'));
    
    // Convert profiles map to vector for indexed access
    std::vector<std::pair<std::string, Profile>> profile_list;
    for (const auto& [id, profile] : config.profiles) {
        profile_list.push_back({id, profile});
    }
    
    // Profile list
    int row = 8;
    int visible_items = height - 12;
    
    for (size_t i = scroll_offset; i < profile_list.size() && row < height - 4; i++) {
        const auto& [id, profile] = profile_list[i];
        bool is_active = (id == config.active_profile);
        int attrs = (i == static_cast<size_t>(selected_idx)) ? COLOR_PAIR(CP_SELECTED) : 0;
        
        if (attrs) wattron(main_win->get(), attrs);
        
        // Active indicator
        if (is_active) {
            wattron(main_win->get(), COLOR_PAIR(CP_ONLINE));
            mvwprintw(main_win->get(), row, col_indicator, "●");
            wattroff(main_win->get(), COLOR_PAIR(CP_ONLINE));
        } else {
            mvwprintw(main_win->get(), row, col_indicator, " ");
        }
        
        // Profile ID
        std::string display_id = id;
        if (static_cast<int>(display_id.length()) > name_width)
            display_id = display_id.substr(0, name_width - 3) + "...";
        mvwprintw(main_win->get(), row, col_name, "%-*s", col_bindings - col_name, display_id.c_str());
        
        // Binding count
        int binding_count = profile.bindings_keys.size() + profile.bindings_abs.size();
        mvwprintw(main_win->get(), row, col_bindings, "%3d", binding_count);
        
        // Description
        std::string desc = profile.description;
        if (static_cast<int>(desc.length()) > desc_width)
            desc = desc.substr(0, desc_width - 3) + "...";
        mvwprintw(main_win->get(), row, col_desc, "%s", desc.c_str());
        
        if (attrs) wattroff(main_win->get(), attrs);
        
        row++;
    }
    
    // Show message if any
    if (message_timer > 0 && !message.empty()) {
        message_timer--;
        int msg_color = (message.find("Error") != std::string::npos) ? CP_ERROR : CP_SUCCESS;
        main_win->print(height - 5, 2, message, COLOR_PAIR(msg_color));
    }
    
    // Actions
    main_win->print(height - 3, 2, "Actions:", COLOR_PAIR(CP_HEADER) | A_BOLD);
    main_win->print(height - 2, 4, "[A]ctivate [N]ew [D]uplicate [R]ename [Del]ete [S]ave");
    
    main_win->refresh();
    needs_redraw = false;
}

void ProfileManager::handle_input(int ch) {
    auto& config = tui->get_config();
    int profile_count = config.profiles.size();
    
    switch (ch) {
        case KEY_UP:
        case 'k':
            if (selected_idx > 0) {
                selected_idx--;
                if (selected_idx < scroll_offset) scroll_offset = selected_idx;
                needs_redraw = true;
            }
            break;
            
        case KEY_DOWN:
        case 'j':
            if (selected_idx < profile_count - 1) {
                selected_idx++;
                int visible = tui->get_screen_height() - 16;
                if (selected_idx >= scroll_offset + visible) {
                    scroll_offset = selected_idx - visible + 1;
                }
                needs_redraw = true;
            }
            break;
            
        case 'a':
        case 'A':
            activate_selected_profile();
            break;
            
        case 'n':
        case 'N':
            show_new_profile_dialog();
            break;
            
        case 'd':
        case 'D':
            duplicate_selected_profile();
            break;
            
        case 'r':
        case 'R':
            rename_selected_profile();
            break;
            
        case KEY_DC: // Delete key
            delete_selected_profile();
            break;
    }
}

void ProfileManager::activate_selected_profile() {
    auto& config = tui->get_config();
    
    // Convert to vector for indexed access
    std::vector<std::string> profile_ids;
    for (const auto& [id, _] : config.profiles) {
        profile_ids.push_back(id);
    }
    
    if (selected_idx >= 0 && selected_idx < static_cast<int>(profile_ids.size())) {
        const std::string& profile_id = profile_ids[selected_idx];
        
        if (ConfigManager::switch_profile(config, profile_id)) {
            tui->mark_modified();
            
            // Signal mapper to reload
            if (signal_mapper_reload()) {
                show_message("Profile activated and mapper notified");
            } else {
                show_message("Profile activated (mapper not running)");
            }
            
            needs_redraw = true;
        }
    }
}

void ProfileManager::show_new_profile_dialog() {
    // Simple dialog - just ask for name
    int h = 8, w = 50;
    int starty = (tui->get_screen_height() - h) / 2;
    int startx = (tui->get_screen_width() - w) / 2;
    
    Window dialog(h, w, starty, startx, " New Profile ");
    dialog.print(2, 2, "Profile ID (lowercase, no spaces):");
    dialog.print(4, 2, "[ESC] Cancel  [ENTER] Confirm", A_DIM);
    dialog.refresh();
    
    // Enable cursor for input
    curs_set(1);
    echo();
    
    char input[32] = {0};
    mvwgetnstr(dialog.get(), 3, 2, input, 31);
    
    noecho();
    curs_set(0);
    
    if (input[0] != '\0') {
        auto& config = tui->get_config();
        if (ConfigManager::create_profile(config, input)) {
            tui->mark_modified();
            show_message("Profile created: " + std::string(input));
        } else {
            show_message("Error: Profile already exists");
        }
    }
    
    needs_redraw = true;
}

void ProfileManager::duplicate_selected_profile() {
    auto& config = tui->get_config();
    
    std::vector<std::string> profile_ids;
    for (const auto& [id, _] : config.profiles) {
        profile_ids.push_back(id);
    }
    
    if (selected_idx >= 0 && selected_idx < static_cast<int>(profile_ids.size())) {
        const std::string& source_id = profile_ids[selected_idx];
        std::string dest_id = source_id + "_copy";
        
        // Find unique name
        int suffix = 1;
        while (config.profiles.count(dest_id)) {
            dest_id = source_id + "_copy" + std::to_string(suffix++);
        }
        
        if (ConfigManager::duplicate_profile(config, source_id, dest_id)) {
            tui->mark_modified();
            show_message("Profile duplicated: " + dest_id);
            needs_redraw = true;
        }
    }
}

void ProfileManager::rename_selected_profile() {
    auto& config = tui->get_config();
    
    std::vector<std::string> profile_ids;
    for (const auto& [id, _] : config.profiles) {
        profile_ids.push_back(id);
    }
    
    if (selected_idx < 0 || selected_idx >= static_cast<int>(profile_ids.size())) return;
    
    const std::string old_id = profile_ids[selected_idx];
    
    if (old_id == "default") {
        show_message("Cannot rename default profile");
        return;
    }
    
    int h = 8, w = 50;
    int starty = (tui->get_screen_height() - h) / 2;
    int startx = (tui->get_screen_width() - w) / 2;
    
    Window dialog(h, w, starty, startx, " Rename Profile ");
    dialog.print(2, 2, "Current: " + old_id);
    dialog.print(3, 2, "New name (lowercase, no spaces):");
    dialog.print(6, 2, "[ESC] Cancel  [ENTER] Confirm", A_DIM);
    dialog.refresh();
    
    curs_set(1);
    echo();
    
    char input[32] = {0};
    mvwgetnstr(dialog.get(), 4, 2, input, 31);
    
    noecho();
    curs_set(0);
    
    std::string new_id(input);
    if (!new_id.empty() && new_id != old_id) {
        if (config.profiles.count(new_id)) {
            show_message("Error: Profile '" + new_id + "' already exists");
        } else {
            // Move the profile to the new key
            config.profiles[new_id] = std::move(config.profiles[old_id]);
            config.profiles[new_id].name = new_id;
            config.profiles.erase(old_id);
            
            // Update active profile reference if needed
            if (config.active_profile == old_id) {
                config.active_profile = new_id;
            }
            
            tui->mark_modified();
            show_message("Renamed '" + old_id + "' -> '" + new_id + "'");
        }
    }
    
    needs_redraw = true;
}

void ProfileManager::delete_selected_profile() {
    auto& config = tui->get_config();
    
    std::vector<std::string> profile_ids;
    for (const auto& [id, _] : config.profiles) {
        profile_ids.push_back(id);
    }
    
    if (selected_idx >= 0 && selected_idx < static_cast<int>(profile_ids.size())) {
        const std::string& profile_id = profile_ids[selected_idx];
        
        if (profile_id == "default") {
            show_message("Cannot delete default profile");
            return;
        }
        
        // Confirm dialog
        int h = 7, w = 40;
        int starty = (tui->get_screen_height() - h) / 2;
        int startx = (tui->get_screen_width() - w) / 2;
        
        Window dialog(h, w, starty, startx, " Confirm Delete ");
        dialog.print(2, 2, "Delete profile '" + profile_id + "'?");
        dialog.print(4, 2, "[y] Yes  [n] No", A_DIM);
        dialog.refresh();
        
        int ch;
        while ((ch = getch()) != 'y' && ch != 'Y' && ch != 'n' && ch != 'N' && ch != 27) {
            // Wait for valid input
        }
        
        if (ch == 'y' || ch == 'Y') {
            if (ConfigManager::delete_profile(config, profile_id)) {
                tui->mark_modified();
                if (selected_idx >= static_cast<int>(profile_ids.size()) - 1) {
                    selected_idx = std::max(0, selected_idx - 1);
                }
                show_message("Profile deleted: " + profile_id);
            }
        }
    }
    
    needs_redraw = true;
}

void ProfileManager::show_message(const std::string& msg) {
    message = msg;
    message_timer = 50; // Show for ~5 seconds
    needs_redraw = true;
}
