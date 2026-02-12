#include "tui.hpp"
#include "../version.hpp"
#include "device_dashboard.hpp"
#include "mappings_view.hpp"
#include "live_monitor.hpp"
#include "profile_manager.hpp"
#include "calibration_wizard.hpp"

TUI::TUI() : running(true), current_view(ViewType::DASHBOARD), 
        config_modified(false), screen_height(0), screen_width(0) {
    init_ncurses();
    load_config();
    scan_devices();
    refresh_bindings();
    create_views();
    create_windows();
}

TUI::~TUI() {
    endwin();
}

void TUI::init_ncurses() {
    initscr();
    cbreak();
    noecho();
    noqiflush();
    keypad(stdscr, TRUE);
    curs_set(0);
    timeout(100); // 100ms timeout for getch
    
    // Disable XON/XOFF flow control so Ctrl+S reaches the app
    struct termios term;
    tcgetattr(STDIN_FILENO, &term);
    term.c_iflag &= ~(IXON | IXOFF);
    tcsetattr(STDIN_FILENO, TCSANOW, &term);
    
    if (has_colors()) {
        start_color();
        use_default_colors();
        
        // Initialize color pairs
        init_pair(CP_DEFAULT, COLOR_WHITE, -1);
        init_pair(CP_HEADER, COLOR_CYAN, -1);
        init_pair(CP_HIGHLIGHT, COLOR_BLACK, COLOR_CYAN);
        init_pair(CP_ONLINE, COLOR_GREEN, -1);
        init_pair(CP_OFFLINE, COLOR_RED, -1);
        init_pair(CP_WARNING, COLOR_YELLOW, -1);
        init_pair(CP_ERROR, COLOR_RED, -1);
        init_pair(CP_SUCCESS, COLOR_GREEN, -1);
        init_pair(CP_BINDING, COLOR_MAGENTA, -1);
        init_pair(CP_AXIS, COLOR_BLUE, -1);
        init_pair(CP_BUTTON, COLOR_YELLOW, -1);
        init_pair(CP_SELECTED, COLOR_BLACK, COLOR_WHITE);
        init_pair(CP_BORDER, COLOR_WHITE, -1);
    }
    
    getmaxyx(stdscr, screen_height, screen_width);
}

void TUI::create_windows() {
    // Header window (top 3 rows)
    header_win = std::make_unique<Window>(3, screen_width, 0, 0, "", false);
    
    // Main window (middle area)
    main_win = std::make_unique<Window>(screen_height - 5, screen_width, 3, 0, "", false);
    
    // Status window (bottom 2 rows)
    status_win = std::make_unique<Window>(2, screen_width, screen_height - 2, 0, "", false);
}

void TUI::load_config() {
    std::string config_path = get_config_path();
    auto loaded = ConfigManager::load(config_path);
    if (loaded) {
        config = *loaded;
    } else {
        // Create default config
        config.uinput_name = "Thrustmaster ARMA Virtual";
        config.grab = true;
    }
}

void TUI::save_config() {
    std::string config_path = get_config_path();
    if (ConfigManager::save(config_path, config)) {
        config_modified = false;
    }
}

void TUI::scan_devices() {
    devices.clear();
    
    // Scan from config.devices (new format)
    // Merge entries with the same by_id into one DeviceInfo with multiple roles
    for (const auto& [role, input] : config.devices) {
        // Check if we already have a DeviceInfo for this by_id
        std::shared_ptr<DeviceInfo> existing;
        if (!input.by_id.empty()) {
            for (auto& dev : devices) {
                if (dev->by_id == input.by_id) {
                    existing = dev;
                    break;
                }
            }
        }
        
        if (existing) {
            // Just add the role to the existing device
            if (!existing->has_role(role)) {
                existing->roles.push_back(role);
            }
            continue;
        }
        
        auto dev = std::make_shared<DeviceInfo>();
        dev->roles.push_back(role);
        dev->by_id = input.by_id;
        dev->vendor = input.vendor;
        dev->product = input.product;
        dev->optional = input.optional;
        
        // Try to open and validate
        if (!input.by_id.empty()) {
            char real_path[PATH_MAX];
            if (realpath(input.by_id.c_str(), real_path) != nullptr) {
                int fd = open(real_path, O_RDONLY | O_NONBLOCK);
                if (fd >= 0) {
                    struct libevdev* d = nullptr;
                    if (libevdev_new_from_fd(fd, &d) == 0) {
                        dev->fd = fd;
                        dev->dev = d;
                        dev->path = real_path;
                        dev->name = libevdev_get_name(d) ? libevdev_get_name(d) : "Unknown";
                        dev->online = true;
                        
                        // Enumerate capabilities
                        for (int code = 0; code <= ABS_MAX; code++) {
                            if (libevdev_has_event_code(d, EV_ABS, code)) {
                                dev->axes.push_back(code);
                            }
                        }
                        for (int code = BTN_JOYSTICK; code < BTN_DIGI; code++) {
                            if (libevdev_has_event_code(d, EV_KEY, code)) {
                                dev->buttons.push_back(code);
                            }
                        }
                    } else {
                        close(fd);
                    }
                }
            }
        }
        
        devices.push_back(dev);
    }
    
    // Also scan /dev/input/by-id for new devices
    scan_new_devices();
}

void TUI::scan_new_devices() {
    DIR* dir = opendir("/dev/input/by-id");
    if (!dir) return;
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strstr(entry->d_name, "event") == nullptr) continue;
        
        std::string path = std::string("/dev/input/by-id/") + entry->d_name;
        
        // Check if already known
        bool known = false;
        for (const auto& dev : devices) {
            if (dev->by_id == path) {
                known = true;
                break;
            }
        }
        
        if (!known) {
            // Add as unassigned device for user to configure
            auto dev = std::make_shared<DeviceInfo>();
            dev->by_id = path;
            
            char real_path[PATH_MAX];
            if (realpath(path.c_str(), real_path) != nullptr) {
                int fd = open(real_path, O_RDONLY | O_NONBLOCK);
                if (fd >= 0) {
                    struct libevdev* d = nullptr;
                    if (libevdev_new_from_fd(fd, &d) == 0) {
                        dev->fd = fd;
                        dev->dev = d;
                        dev->path = real_path;
                        dev->name = libevdev_get_name(d) ? libevdev_get_name(d) : "Unknown";
                        dev->online = true;
                        
                        for (int code = 0; code <= ABS_MAX; code++) {
                            if (libevdev_has_event_code(d, EV_ABS, code)) {
                                dev->axes.push_back(code);
                            }
                        }
                        for (int code = BTN_JOYSTICK; code < BTN_DIGI; code++) {
                            if (libevdev_has_event_code(d, EV_KEY, code)) {
                                dev->buttons.push_back(code);
                            }
                        }
                        
                        devices.push_back(dev);
                    } else {
                        close(fd);
                    }
                }
            }
        }
    }
    closedir(dir);
}

void TUI::refresh_bindings() {
    bindings.clear();
    
    // Get bindings from active profile
    auto active_keys = config.get_active_bindings_keys();
    auto active_abs = config.get_active_bindings_abs();
    
    // Convert config bindings to display format
    for (const auto& key_binding : active_keys) {
        BindingDisplay bd;
        bd.source_role = key_binding.role;
        bd.source_code = key_binding.src;
        bd.virtual_code = key_binding.dst;
        bd.virtual_kind = SrcKind::Key;
        const char* btn_name = libevdev_event_code_get_name(EV_KEY, key_binding.src);
        bd.source_name = btn_name ? btn_name : ("BTN_" + std::to_string(key_binding.src));
        bd.invert = false;
        bd.deadzone = 0;
        bd.scale = 1.0f;
        bd.is_valid = true;
        bindings.push_back(bd);
    }
    
    for (const auto& abs_binding : active_abs) {
        BindingDisplay bd;
        bd.source_role = abs_binding.role;
        bd.source_code = abs_binding.src;
        bd.virtual_code = abs_binding.dst;
        bd.virtual_kind = SrcKind::Abs;
        const char* abs_name = libevdev_event_code_get_name(EV_ABS, abs_binding.src);
        bd.source_name = abs_name ? abs_name : ("ABS_" + std::to_string(abs_binding.src));
        bd.invert = abs_binding.invert;
        bd.deadzone = abs_binding.deadzone;
        bd.scale = abs_binding.scale;
        bd.is_valid = true;
        bindings.push_back(bd);
    }
}

void TUI::draw_header() {
    header_win->clear();
    
    // Title
    wattron(header_win->get(), COLOR_PAIR(CP_HEADER) | A_BOLD);
    mvwprintw(header_win->get(), 0, 2, "TWCS Mapper - Thrustmaster ARMA Controller");
    wattroff(header_win->get(), COLOR_PAIR(CP_HEADER) | A_BOLD);
    
    // Status indicator
    std::string status = config_modified ? " [MODIFIED]" : "";
    wattron(header_win->get(), COLOR_PAIR(CP_WARNING));
    mvwprintw(header_win->get(), 0, screen_width - status.length() - 15, "%s", status.c_str());
    wattroff(header_win->get(), COLOR_PAIR(CP_WARNING));
    
    // Navigation tabs - F1-F5 for main views
    const char* tabs[] = {"[F1] Devices", "[F2] Mappings", "[F3] Calibrate", "[F4] Profiles", "[F5] Monitor"};
    ViewType tab_views[] = {ViewType::DASHBOARD, ViewType::MAPPINGS, ViewType::CALIBRATION, ViewType::PROFILES, ViewType::MONITOR};
    int x = 2;
    for (int i = 0; i < 5; i++) {
        if (tab_views[i] == current_view) {
            wattron(header_win->get(), COLOR_PAIR(CP_HIGHLIGHT));
            mvwprintw(header_win->get(), 2, x, "%s", tabs[i]);
            wattroff(header_win->get(), COLOR_PAIR(CP_HIGHLIGHT));
        } else {
            mvwprintw(header_win->get(), 2, x, "%s", tabs[i]);
        }
        x += strlen(tabs[i]) + 4;
    }
    
    // Horizontal line
    mvwhline(header_win->get(), 1, 0, ACS_HLINE, screen_width);
    
    header_win->refresh();
}

void TUI::draw_status() {
    status_win->clear();
    
    // Help text
    wattron(status_win->get(), COLOR_PAIR(CP_DEFAULT) | A_DIM);
    mvwprintw(status_win->get(), 0, 2, 
              "Tab: Switch Views | Enter: Select | q: Quit | h: Help | Ctrl+S: Save");
    wattroff(status_win->get(), COLOR_PAIR(CP_DEFAULT) | A_DIM);
    
    // Device count
    int online_count = 0;
    for (const auto& dev : devices) {
        if (dev->online) online_count++;
    }
    
    std::string dev_status = std::to_string(online_count) + "/" + std::to_string(devices.size()) + " devices online";
    mvwprintw(status_win->get(), 0, screen_width - dev_status.length() - 2, "%s", dev_status.c_str());
    
    // Version
    std::string ver = "v" + std::string(TWCS_VERSION);
    wattron(status_win->get(), COLOR_PAIR(CP_DEFAULT) | A_DIM);
    mvwprintw(status_win->get(), 1, screen_width - ver.length() - 2, "%s", ver.c_str());
    wattroff(status_win->get(), COLOR_PAIR(CP_DEFAULT) | A_DIM);
    
    status_win->refresh();
}

void TUI::run() {
    // Ensure initial screen is drawn
    clear();
    ::refresh();
    if (static_cast<size_t>(current_view) < views.size()) {
        views[static_cast<int>(current_view)]->refresh();
    }
    
    ViewType last_view = current_view;
    
    while (running) {
        // Check if view changed and mark for redraw
        if (current_view != last_view) {
            if (static_cast<size_t>(current_view) < views.size()) {
                views[static_cast<int>(current_view)]->refresh();
            }
            last_view = current_view;
        }
        
        draw_header();
        draw_status();
        
        // Draw current view
        if (static_cast<size_t>(current_view) < views.size()) {
            views[static_cast<int>(current_view)]->draw();
        }
        
        // Handle input
        int ch = getch();
        if (ch != ERR) {
            handle_global_input(ch);
        }
        
        // Small delay to prevent CPU spinning
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void TUI::handle_global_input(int ch) {
    // Define the navigation order for views
    const std::vector<ViewType> view_order = {
        ViewType::DASHBOARD,
        ViewType::MAPPINGS,
        ViewType::CALIBRATION,
        ViewType::PROFILES,
        ViewType::MONITOR
    };
    
    switch (ch) {
        case KEY_F(1): current_view = ViewType::DASHBOARD; break;
        case KEY_F(2): current_view = ViewType::MAPPINGS; break;
        case KEY_F(3): current_view = ViewType::CALIBRATION; break;
        case KEY_F(4): current_view = ViewType::PROFILES; break;
        case KEY_F(5): current_view = ViewType::MONITOR; break;
        case 9: // Tab
        case KEY_RIGHT: {
            // Find current position in view_order
            int current_idx = 0;
            for (size_t i = 0; i < view_order.size(); i++) {
                if (view_order[i] == current_view) {
                    current_idx = i;
                    break;
                }
            }
            int next_idx = (current_idx + 1) % view_order.size();
            current_view = view_order[next_idx];
            break;
        }
        case KEY_BTAB:
        case KEY_LEFT: {
            // Find current position in view_order
            int current_idx = 0;
            for (size_t i = 0; i < view_order.size(); i++) {
                if (view_order[i] == current_view) {
                    current_idx = i;
                    break;
                }
            }
            int prev_idx = (current_idx - 1 + view_order.size()) % view_order.size();
            current_view = view_order[prev_idx];
            break;
        }
        case 'q':
        case 'Q':
            if (config_modified) {
                int h = 7, w = 45;
                int starty = (screen_height - h) / 2;
                int startx = (screen_width - w) / 2;
                
                Window dialog(h, w, starty, startx, " Unsaved Changes ");
                dialog.print(2, 2, "Save changes before quitting?");
                dialog.print(4, 2, "[y] Save & Quit  [n] Discard  [ESC] Cancel", A_DIM);
                dialog.refresh();
                
                int confirm;
                while ((confirm = getch()) != 'y' && confirm != 'Y' && 
                       confirm != 'n' && confirm != 'N' && confirm != 27) {}
                
                if (confirm == 'y' || confirm == 'Y') {
                    save_config();
                    running = false;
                } else if (confirm == 'n' || confirm == 'N') {
                    running = false;
                }
                // ESC = cancel, stay in TUI
            } else {
                running = false;
            }
            break;
        case 0x13: // Ctrl+S
            save_config();
            break;
        case 'h':
        case 'H':
        case '?':
            show_help();
            break;
        default:
            // Pass to current view
            if (static_cast<size_t>(current_view) < views.size()) {
                views[static_cast<int>(current_view)]->handle_input(ch);
            }
            break;
    }
}

void TUI::show_help() {
    // Create help dialog
    int h = 24, w = 60;
    int starty = (screen_height - h) / 2;
    int startx = (screen_width - w) / 2;
    
    Window help_win(h, w, starty, startx, " Help ");
    
    help_win.print(2, 2, "Global Keys:", COLOR_PAIR(CP_HEADER) | A_BOLD);
    help_win.print(3, 4, "F1-F5       Switch between views");
    help_win.print(4, 4, "Tab/←→      Next/Previous view");
    help_win.print(5, 4, "Ctrl+S      Save configuration");
    help_win.print(6, 4, "q           Quit application");
    help_win.print(7, 4, "h/?         Show this help");
    
    help_win.print(9, 2, "Device View:", COLOR_PAIR(CP_HEADER) | A_BOLD);
    help_win.print(10, 4, "s/t/r       Toggle Stick/Throttle/Rudder");
    help_win.print(11, 4, "d           Detect devices");
    help_win.print(12, 4, "w           Run setup wizard");
    
    help_win.print(14, 2, "Mappings View:", COLOR_PAIR(CP_HEADER) | A_BOLD);
    help_win.print(15, 4, "a           Add binding");
    help_win.print(16, 4, "e           Edit binding");
    help_win.print(17, 4, "d           Delete binding");
    
    help_win.print(19, 2, "Full Setup:", COLOR_PAIR(CP_HEADER) | A_BOLD);
    help_win.print(20, 4, "w           Run original setup wizard (twcs_setup)");
    
    help_win.print(22, 2, "Press any key to close...", COLOR_PAIR(CP_WARNING));
    
    help_win.refresh();
    getch();
}

// Implementation of TUI::create_views() - must be after View classes are defined
void TUI::create_views() {
    views.push_back(std::make_unique<DeviceDashboard>(this));
    views.push_back(std::make_unique<MappingsView>(this));
    views.push_back(std::make_unique<CalibrationWizard>(this));
    views.push_back(std::make_unique<ProfileManager>(this));
    views.push_back(std::make_unique<LiveMonitor>(this));
}
