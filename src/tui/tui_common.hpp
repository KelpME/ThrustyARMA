#pragma once

/**
 * TWCS Mapper - Comprehensive TUI Interface
 * 
 * Features:
 * - Device Dashboard: View and manage connected devices
 * - Mappings Matrix: Visual binding management
 * - Binding Editor: Edit transforms with live preview
 * - Calibration Wizard: Step-by-step axis calibration
 * - Live Monitor: Real-time input visualization
 */

#include "config.hpp"
#include "bindings.hpp"
#include <ncurses.h>
#include <fcntl.h>
#include <unistd.h>
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <libevdev-1.0/libevdev/libevdev.h>
#include <string>
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <dirent.h>
#include <sys/stat.h>
#include <climits>
#include <cstring>
#include <cstdlib>
#include <cstdio>
#include <signal.h>
#include <termios.h>
#include <iostream>

// Color pairs
enum ColorPairs {
    CP_DEFAULT = 1,
    CP_HEADER,
    CP_HIGHLIGHT,
    CP_ONLINE,
    CP_OFFLINE,
    CP_WARNING,
    CP_ERROR,
    CP_SUCCESS,
    CP_BINDING,
    CP_AXIS,
    CP_BUTTON,
    CP_SELECTED,
    CP_BORDER
};

// View types
enum class ViewType {
    DASHBOARD,
    MAPPINGS,
    CALIBRATION,
    PROFILES,
    MONITOR
};

// Forward declarations
class TUI;
class View;
class DeviceDashboard;
class MappingsView;
class CalibrationWizard;
class ProfileManager;
class LiveMonitor;

// Utility functions
inline std::string get_config_path() {
    return ConfigManager::get_config_path();
}

inline std::string format_axis_value(int value) {
    std::stringstream ss;
    ss << std::setw(6) << value;
    return ss.str();
}

inline std::string get_role_icon(const std::string& role) {
    if (role == "stick") return "[S]";
    if (role == "throttle") return "[T]";
    if (role == "rudder") return "[R]";
    return "[?]";
}

inline std::string get_role_icons(const std::vector<std::string>& roles) {
    if (roles.empty()) return "[?]";
    std::string s;
    for (const auto& r : roles) {
        if (!s.empty()) s += " ";
        s += get_role_icon(r);
    }
    return s;
}

inline std::string get_connection_status(bool online) {
    return online ? "● ONLINE" : "○ OFFLINE";
}

inline std::string get_xbox_axis_name(int abs_code) {
    switch (abs_code) {
        case ABS_X:     return "Left Stick X";
        case ABS_Y:     return "Left Stick Y";
        case ABS_RX:    return "Right Stick X";
        case ABS_RY:    return "Right Stick Y";
        case ABS_Z:     return "Left Trigger";
        case ABS_RZ:    return "Right Trigger";
        case ABS_HAT0X: return "D-Pad X";
        case ABS_HAT0Y: return "D-Pad Y";
        default:        return "Axis " + std::to_string(abs_code);
    }
}

// Window management helper
class Window {
private:
    WINDOW* win;
    int x, y, width, height;
    std::string title;
    bool has_border;

public:
    Window(int h, int w, int starty, int startx, const std::string& t = "", bool border = true)
        : win(newwin(h, w, starty, startx)), x(startx), y(starty), 
          width(w), height(h), title(t), has_border(border) {
        if (has_border) {
            box(win, 0, 0);
            if (!title.empty()) {
                wattron(win, COLOR_PAIR(CP_BORDER) | A_BOLD);
                mvwprintw(win, 0, 2, " %s ", title.c_str());
                wattroff(win, COLOR_PAIR(CP_BORDER) | A_BOLD);
            }
        }
    }
    
    ~Window() {
        if (win) delwin(win);
    }
    
    WINDOW* get() { return win; }
    
    void refresh() { wrefresh(win); }
    void clear() { werase(win); if (has_border) box(win, 0, 0); }
    
    void print(int row, int col, const std::string& text, int attrs = 0) {
        if (attrs) wattron(win, attrs);
        mvwprintw(win, row, col, "%s", text.c_str());
        if (attrs) wattroff(win, attrs);
    }
    
    void print_center(int row, const std::string& text, int attrs = 0) {
        int col = (width - text.length()) / 2;
        print(row, col, text, attrs);
    }
    
    void draw_bar(int row, int col, int width, float percent, bool vertical = false) {
        int filled = std::clamp(static_cast<int>(width * percent), 0, width);
        std::string bar(filled, '#');
        bar += std::string(width - filled, '-');
        
        wattron(win, COLOR_PAIR(CP_AXIS));
        mvwprintw(win, row, col, "%s", bar.c_str());
        wattroff(win, COLOR_PAIR(CP_AXIS));
    }
    
    int get_height() const { return height; }
    int get_width() const { return width; }
};

// Device information structure
struct DeviceInfo {
    std::vector<std::string> roles;
    std::string name;
    std::string path;
    std::string by_id;
    std::string vendor;
    std::string product;
    bool online;
    bool optional;
    int fd;
    struct libevdev* dev;
    std::vector<int> axes;
    std::vector<int> buttons;
    
    DeviceInfo() : online(false), optional(true), fd(-1), dev(nullptr) {}
    
    bool has_role(const std::string& r) const {
        return std::find(roles.begin(), roles.end(), r) != roles.end();
    }
    
    std::string roles_str() const {
        if (roles.empty()) return "(unassigned)";
        std::string s;
        for (size_t i = 0; i < roles.size(); i++) {
            if (i > 0) s += ",";
            s += roles[i];
        }
        return s;
    }
    
    ~DeviceInfo() {
        if (dev) {
            libevdev_free(dev);
            dev = nullptr;
        }
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
    }
};

// Binding display structure  
struct BindingDisplay {
    std::string virtual_slot;
    int virtual_code;
    SrcKind virtual_kind;
    std::string source_role;
    int source_code;
    std::string source_name;
    bool invert;
    int deadzone;
    float scale;
    bool is_valid;
};

// Base View class
class View {
protected:
    TUI* tui;
    ViewType type;
    int selected_item;
    bool needs_redraw;
    
public:
    View(TUI* parent, ViewType t) : tui(parent), type(t), selected_item(0), needs_redraw(true) {}
    virtual ~View() = default;
    
    virtual void draw() = 0;
    virtual void handle_input(int ch) = 0;
    virtual void refresh() { needs_redraw = true; }
    
    ViewType get_type() const { return type; }
    bool get_needs_redraw() const { return needs_redraw; }
    void clear_redraw() { needs_redraw = false; }
};

// Helper to check if mapper service is running
inline bool is_mapper_running() {
    return system("systemctl --user is-active --quiet twcs-mapper.service 2>/dev/null") == 0;
}

// Helper to stop mapper service (returns true if it was running)
inline bool stop_mapper_service() {
    if (!is_mapper_running()) return false;
    system("systemctl --user stop twcs-mapper.service 2>/dev/null");
    // Also kill any stray processes
    system("pkill -INT twcs_mapper 2>/dev/null");
    // Brief wait for grab to release
    usleep(300000);
    return true;
}

// Helper to start mapper service
inline void start_mapper_service() {
    system("systemctl --user start twcs-mapper.service 2>/dev/null");
}

// Helper to send SIGHUP to mapper
inline bool signal_mapper_reload() {
    FILE* pipe = popen("pgrep -x twcs_mapper", "r");
    if (!pipe) return false;
    
    char pid_str[32];
    bool found = false;
    while (fgets(pid_str, sizeof(pid_str), pipe) != nullptr) {
        pid_t pid = atoi(pid_str);
        if (pid > 0) {
            if (kill(pid, SIGHUP) == 0) {
                found = true;
            }
        }
    }
    pclose(pipe);
    return found;
}
