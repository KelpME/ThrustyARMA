#include "live_monitor.hpp"

bool LiveMonitor::is_monitored(const std::shared_ptr<DeviceInfo>& dev) {
    return dev->online && dev->dev && !dev->roles.empty();
}

std::string LiveMonitor::friendly_button_name(const std::string& role, int btn) {
    const char* raw = libevdev_event_code_get_name(EV_KEY, btn);
    std::string name = raw ? raw : ("BTN_" + std::to_string(btn));
    
    if (role == "throttle") {
        // Left-hand device: finger positions are mirrored
        if (name == "BTN_THUMB")   return "BTN_PINKIE";
        if (name == "BTN_THUMB2")  return "BTN_PINKIE2";
        if (name == "BTN_PINKIE")  return "BTN_THUMB";
        if (name == "BTN_TOP")     return "BTN_RING";
        if (name == "BTN_TOP2")    return "BTN_MIDDLE";
    } else if (role == "stick") {
        // Stick: strip finger-based names, use positional labels
        if (name == "BTN_THUMB")   return "BTN_2";
        if (name == "BTN_THUMB2")  return "BTN_3";
        if (name == "BTN_TOP")     return "BTN_4";
        if (name == "BTN_TOP2")    return "BTN_5";
        if (name == "BTN_PINKIE")  return "BTN_6";
        if (name == "BTN_BASE")    return "BTN_7";
        if (name == "BTN_BASE2")   return "BTN_8";
        if (name == "BTN_BASE3")   return "BTN_9";
        if (name == "BTN_BASE4")   return "BTN_10";
        if (name == "BTN_BASE5")   return "BTN_11";
        if (name == "BTN_BASE6")   return "BTN_12";
    }
    
    return name;
}

void LiveMonitor::draw_axes_panel(Window* win, int start_col, int panel_width, int start_row, int max_row) {
    auto& devices = tui->get_devices();
    
    int label_width = std::max(14, (panel_width - 12) * 40 / 100);
    int col_bar = start_col + label_width;
    int bar_width = std::max(10, panel_width - label_width - 10);
    int col_value = col_bar + bar_width + 1;
    
    win->print(start_row - 1, start_col - 2, "Axes", A_BOLD | A_UNDERLINE);
    int row = start_row;
    
    for (const auto& dev : devices) {
        if (!is_monitored(dev) || dev->axes.empty()) continue;
        
        win->print(row++, start_col - 2, (get_role_icons(dev->roles) + " " + dev->roles_str() + ":").c_str(),
                   COLOR_PAIR(CP_HEADER));
        
        for (int axis : dev->axes) {
            if (row >= max_row) break;
            
            struct input_absinfo absinfo_buf;
            if (dev->fd >= 0 && ioctl(dev->fd, EVIOCGABS(axis), &absinfo_buf) == 0) {
                int value = absinfo_buf.value;
                last_values[dev->roles_str() + "." + std::to_string(axis)] = value;
                
                const char* name = libevdev_event_code_get_name(EV_ABS, axis);
                std::string axis_name = name ? name : ("ABS_" + std::to_string(axis));
                if (static_cast<int>(axis_name.length()) > label_width - 2)
                    axis_name = axis_name.substr(0, label_width - 5) + "...";
                
                mvwprintw(win->get(), row, start_col, "%-*s", label_width, axis_name.c_str());
                
                float percent = 0.0f;
                if (absinfo_buf.maximum > absinfo_buf.minimum) {
                    percent = std::clamp(static_cast<float>(value - absinfo_buf.minimum) /
                                         (absinfo_buf.maximum - absinfo_buf.minimum), 0.0f, 1.0f);
                }
                
                int filled = std::clamp(static_cast<int>(bar_width * percent), 0, bar_width);
                std::string bar(filled, '#');
                bar += std::string(bar_width - filled, '-');
                
                wattron(win->get(), COLOR_PAIR(CP_AXIS));
                mvwprintw(win->get(), row, col_bar, "%s", bar.c_str());
                wattroff(win->get(), COLOR_PAIR(CP_AXIS));
                
                mvwprintw(win->get(), row, col_value, "%6d", value);
                row++;
            }
        }
        row++;
    }
}

void LiveMonitor::draw_buttons_panel(Window* win, int start_col, int panel_width, int start_row, int max_row) {
    auto& devices = tui->get_devices();
    
    const int state_width = 9; // "[PRESSED]"
    // Cap usable width so dots don't stretch on wide terminals
    int usable = std::min(panel_width, 40);
    int col_state = start_col + usable - state_width;
    int name_max = col_state - start_col - 1;
    if (name_max < 6) name_max = 6;
    
    win->print(start_row - 1, start_col - 1, "Buttons", A_BOLD | A_UNDERLINE);
    int row = start_row;
    
    unsigned char key_state[KEY_MAX / 8 + 1];
    
    for (const auto& dev : devices) {
        if (!is_monitored(dev) || dev->buttons.empty()) continue;
        
        win->print(row++, start_col - 1, (get_role_icons(dev->roles) + " " + dev->roles_str() + ":").c_str(),
                   COLOR_PAIR(CP_HEADER));
        
        memset(key_state, 0, sizeof(key_state));
        bool have_state = (dev->fd >= 0 && ioctl(dev->fd, EVIOCGKEY(sizeof(key_state)), key_state) >= 0);
        
        for (int btn : dev->buttons) {
            if (row >= max_row) break;
            
            std::string btn_name = friendly_button_name(dev->roles.empty() ? "" : dev->roles[0], btn);
            int blen = static_cast<int>(btn_name.length());
            if (blen > name_max - 2)
                btn_name = btn_name.substr(0, name_max - 5) + "...";
            blen = static_cast<int>(btn_name.length());
            
            bool pressed = have_state && (key_state[btn / 8] & (1 << (btn % 8)));
            
            // Name
            mvwprintw(win->get(), row, start_col, "%s", btn_name.c_str());
            
            // Dot leaders filling gap to state column
            int dots = name_max - blen;
            if (dots > 0) {
                std::string dots_str(dots, '.');
                wattron(win->get(), A_DIM);
                mvwprintw(win->get(), row, start_col + blen, "%s", dots_str.c_str());
                wattroff(win->get(), A_DIM);
            }
            
            // State at fixed column
            if (pressed) {
                wattron(win->get(), COLOR_PAIR(CP_ONLINE) | A_BOLD);
                mvwprintw(win->get(), row, col_state, "[PRESSED]");
                wattroff(win->get(), COLOR_PAIR(CP_ONLINE) | A_BOLD);
            } else {
                wattron(win->get(), A_DIM);
                mvwprintw(win->get(), row, col_state, "    -    ");
                wattroff(win->get(), A_DIM);
            }
            
            row++;
        }
        row++;
    }
}

LiveMonitor::LiveMonitor(TUI* parent) : View(parent, ViewType::MONITOR), monitoring(false) {}

LiveMonitor::~LiveMonitor() {
    stop_monitoring();
}

void LiveMonitor::draw() {
    auto* main_win = tui->get_main_win();
    int height = main_win->get_height();
    int width = main_win->get_width();
    
    main_win->clear();
    
    main_win->print(1, 2, "Live Input Monitor", COLOR_PAIR(CP_HEADER) | A_BOLD);
    
    if (monitoring) {
        main_win->print(2, 2, "Status: ", 0);
        wattron(main_win->get(), COLOR_PAIR(CP_ONLINE));
        wprintw(main_win->get(), "MONITORING");
        wattroff(main_win->get(), COLOR_PAIR(CP_ONLINE));
        
        int divider = width / 2;
        int max_row = height - 4;
        
        draw_axes_panel(main_win, 4, divider - 6, 4, max_row);
        
        // Divider line
        for (int r = 3; r < height - 3; r++)
            mvwaddch(main_win->get(), r, divider - 1, ACS_VLINE | COLOR_PAIR(CP_BORDER) | A_DIM);
        
        draw_buttons_panel(main_win, divider + 2, width - divider - 4, 4, max_row);
    } else {
        main_win->print(2, 2, "Status: ", 0);
        wattron(main_win->get(), COLOR_PAIR(CP_OFFLINE));
        wprintw(main_win->get(), "STOPPED");
        wattroff(main_win->get(), COLOR_PAIR(CP_OFFLINE));
        
        main_win->print(height / 2, 2, "Press [SPACE] to start monitoring", COLOR_PAIR(CP_WARNING));
    }
    
    main_win->print(height - 2, 2, monitoring ? "[SPACE] Stop  [r] Refresh" : "[SPACE] Start", A_DIM);
    
    main_win->refresh();
    needs_redraw = true;
}

void LiveMonitor::handle_input(int ch) {
    switch (ch) {
        case ' ':
            if (monitoring) {
                stop_monitoring();
            } else {
                start_monitoring();
            }
            needs_redraw = true;
            break;
            
        case 'r':
        case 'R':
            needs_redraw = true;
            break;
    }
}

void LiveMonitor::start_monitoring() {
    monitoring = true;
}

void LiveMonitor::stop_monitoring() {
    monitoring = false;
}
