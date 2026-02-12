#include "device_dashboard.hpp"

DeviceDashboard::DeviceDashboard(TUI* parent) : View(parent, ViewType::DASHBOARD), scroll_offset(0) {}

void DeviceDashboard::draw() {
    if (!needs_redraw) return;
    
    auto& devices = tui->get_devices();
    auto* main_win = tui->get_main_win();
    int height = main_win->get_height();
    int width = main_win->get_width();
    
    main_win->clear();
    
    // Title
    main_win->print(1, 2, "Connected Devices", COLOR_PAIR(CP_HEADER) | A_BOLD);
    
    // Dynamic column layout based on terminal width
    int usable = width - 4; // 2px padding each side
    int col_role = 2;
    int col_name = col_role + std::max(24, usable * 35 / 100);
    int col_status = col_name + std::max(20, usable * 40 / 100);
    int name_width = col_status - col_name - 2;
    
    // Column headers
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "%-*s%-*s%s",
             col_name - col_role, "Role",
             col_status - col_name, "Name",
             "Status");
    main_win->print(3, 2, hdr);
    main_win->print(4, 2, std::string(width - 4, '-'));
    
    // Device list
    int row = 5;
    int visible_items = height - 10;
    
    for (size_t i = scroll_offset; i < devices.size() && row < height - 4; i++) {
        const auto& dev = devices[i];
        int attrs = (i == static_cast<size_t>(selected_item)) ? COLOR_PAIR(CP_SELECTED) : 0;
        
        if (attrs) wattron(main_win->get(), attrs);
        
        // Role icons and names
        std::string role_str = get_role_icons(dev->roles) + " " + dev->roles_str();
        mvwprintw(main_win->get(), row, col_role, "%-*s", col_name - col_role, role_str.c_str());
        
        // Device name (truncate to fit column)
        std::string name = dev->name;
        if (static_cast<int>(name.length()) > name_width) {
            name = name.substr(0, name_width - 3) + "...";
        }
        mvwprintw(main_win->get(), row, col_name, "%-*s", col_status - col_name, name.c_str());
        
        // Status
        if (dev->online) {
            wattron(main_win->get(), COLOR_PAIR(CP_ONLINE));
            mvwprintw(main_win->get(), row, col_status, "● ONLINE ");
            wattroff(main_win->get(), COLOR_PAIR(CP_ONLINE));
        } else {
            wattron(main_win->get(), COLOR_PAIR(CP_OFFLINE));
            mvwprintw(main_win->get(), row, col_status, "○ OFFLINE");
            wattroff(main_win->get(), COLOR_PAIR(CP_OFFLINE));
        }
        
        if (attrs) wattroff(main_win->get(), attrs);
        
        row++;
    }
    
    // Actions section
    main_win->print(height - 3, 2, "Actions:", COLOR_PAIR(CP_HEADER) | A_BOLD);
    main_win->print(height - 2, 4, "[s] Stick  [t] Throttle  [r] Rudder  (toggle type)  [d] Detect  [w] Wizard  [Enter] Details");
    
    main_win->refresh();
    needs_redraw = false;
}

void DeviceDashboard::handle_input(int ch) {
    auto& devices = tui->get_devices();
    
    switch (ch) {
        case KEY_UP:
        case 'k':
            if (selected_item > 0) {
                selected_item--;
                if (selected_item < scroll_offset) scroll_offset = selected_item;
                needs_redraw = true;
            }
            break;
            
        case KEY_DOWN:
        case 'j':
            if (selected_item < static_cast<int>(devices.size()) - 1) {
                selected_item++;
                int visible = tui->get_screen_height() - 14;
                if (selected_item >= scroll_offset + visible) {
                    scroll_offset = selected_item - visible + 1;
                }
                needs_redraw = true;
            }
            break;
            
        case 'd':
        case 'D':
            tui->scan_devices();
            needs_redraw = true;
            break;
            
        case '\n':
        case '\r':
        case KEY_ENTER:
            if (selected_item >= 0 && selected_item < static_cast<int>(devices.size())) {
                show_device_details(devices[selected_item]);
            }
            break;
            
        case 'w':
        case 'W':
            launch_full_setup_wizard();
            break;
            
        case 's':
        case 'S':
            if (selected_item >= 0 && selected_item < static_cast<int>(devices.size())) {
                toggle_role(devices[selected_item], "stick");
            }
            break;
            
        case 't':
        case 'T':
            if (selected_item >= 0 && selected_item < static_cast<int>(devices.size())) {
                toggle_role(devices[selected_item], "throttle");
            }
            break;
            
        case 'r':
        case 'R':
            if (selected_item >= 0 && selected_item < static_cast<int>(devices.size())) {
                toggle_role(devices[selected_item], "rudder");
            }
            break;
    }
}

void DeviceDashboard::toggle_role(const std::shared_ptr<DeviceInfo>& dev, const std::string& role) {
    if (dev->by_id.empty()) return;
    
    auto& config = tui->get_config();
    auto& devices = tui->get_devices();
    
    if (dev->has_role(role)) {
        // Remove this role from the device and config
        config.devices.erase(role);
        dev->roles.erase(std::remove(dev->roles.begin(), dev->roles.end(), role), dev->roles.end());
    } else {
        // Remove this role from any other device that currently has it
        for (auto& other : devices) {
            if (other != dev && other->has_role(role)) {
                other->roles.erase(std::remove(other->roles.begin(), other->roles.end(), role), other->roles.end());
                break;
            }
        }
        
        // Add this role to the device and config
        dev->roles.push_back(role);
        DeviceConfig dc;
        dc.role = role;
        dc.by_id = dev->by_id;
        dc.vendor = dev->vendor;
        dc.product = dev->product;
        dc.optional = (role != "stick");
        config.devices[role] = dc;
    }
    
    tui->mark_modified();
    tui->refresh_bindings();
    needs_redraw = true;
}

void DeviceDashboard::launch_full_setup_wizard() {
    // End ncurses mode completely
    endwin();
    
    // Clear the screen
    std::cout << "\033[2J\033[H";
    std::cout << "\n=== Launching Full Setup Wizard ===\n";
    std::cout << "This will run the original twcs_setup calibration wizard.\n\n";
    std::cout.flush();
    
    // Launch twcs_setup - use full path
    std::string setup_cmd = std::string(getenv("HOME")) + "/.local/bin/twcs_setup";
    int result = system(setup_cmd.c_str());
    
    std::cout << "\nPress Enter to return to TUI...";
    std::cout.flush();
    std::cin.get();
    
    // Full ncurses reinitialization
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(100);
    
    // Clear and redraw everything
    clear();
    refresh();
    
    // Rescan devices since config may have changed
    tui->scan_devices();
    tui->refresh_bindings();
    
    // Mark all views for redraw on next loop
    needs_redraw = true;
}

void DeviceDashboard::show_device_details(const std::shared_ptr<DeviceInfo>& dev) {
    // Create detail dialog
    int h = 15, w = 50;
    int starty = (tui->get_screen_height() - h) / 2;
    int startx = (tui->get_screen_width() - w) / 2;
    
    Window detail_win(h, w, starty, startx, " Device Details ");
    
    detail_win.print(2, 2, ("Roles: " + dev->roles_str()).c_str());
    detail_win.print(3, 2, ("Name: " + dev->name).c_str());
    detail_win.print(4, 2, ("Path: " + dev->path).c_str());
    detail_win.print(5, 2, ("By ID: " + dev->by_id).c_str());
    detail_win.print(6, 2, ("Vendor: " + dev->vendor).c_str());
    detail_win.print(7, 2, ("Product: " + dev->product).c_str());
    
    std::string status = dev->online ? "ONLINE" : "OFFLINE";
    int color = dev->online ? CP_ONLINE : CP_OFFLINE;
    detail_win.print(8, 2, "Status: ", 0);
    wattron(detail_win.get(), COLOR_PAIR(color));
    wprintw(detail_win.get(), "%s", status.c_str());
    wattroff(detail_win.get(), COLOR_PAIR(color));
    
    detail_win.print(9, 2, ("Axes: " + std::to_string(dev->axes.size())).c_str());
    detail_win.print(10, 2, ("Buttons: " + std::to_string(dev->buttons.size())).c_str());
    
    detail_win.print(13, 2, "Press any key to close...", COLOR_PAIR(CP_WARNING));
    
    detail_win.refresh();
    flushinp();
    int ch;
    while ((ch = getch()) == ERR) {}
    needs_redraw = true;
}
