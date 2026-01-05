#include "config.hpp"
#include "bindings.hpp"
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <libevdev-1.0/libevdev/libevdev.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <iostream>
#include <vector>
#include <map>
#include <string>
#include <chrono>
#include <thread>
#include <algorithm>
#include <optional>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstring>
#include <termios.h>
#include <sys/select.h>
#include <poll.h>
#include <set>
#include <filesystem>

struct DeviceInfo {
    std::string path;
    std::string by_id;
    std::string role;
    std::string vendor;
    std::string product;
    int fd;
    struct libevdev* dev;
    std::map<int, int> abs_values;
    std::map<int, int> abs_variance;
    std::chrono::steady_clock::time_point last_update;
};

struct CapturedAxis {
    std::string role;
    int src;
    int dst;
    bool invert;
    int deadzone;
    float scale;
};

struct CaptureState {
    std::vector<DeviceInfo> devices;
    std::vector<std::pair<std::string, int>> captured_buttons;
    std::vector<CapturedAxis> captured_axes;
};

// Virtual button mappings in order
const std::vector<int> VIRTUAL_BUTTONS = {
    BTN_SOUTH,   // 1. A
    BTN_EAST,    // 2. B  
    BTN_WEST,    // 3. X
    BTN_NORTH,   // 4. Y
    BTN_TL,      // 5. Left Bumper
    BTN_TR,      // 6. Right Bumper
    BTN_SELECT,  // 7. Back
    BTN_START,   // 8. Start
    BTN_MODE,    // 9. Guide
    BTN_THUMBL,  // 10. L3
    BTN_THUMBR   // 11. R3
};

const std::vector<std::string> BUTTON_NAMES = {
    "A (South)", "B (East)", "X (West)", "Y (North)",
    "Left Bumper", "Right Bumper", "Back (Select)", "Start",
    "Guide", "L3 (Left Stick)", "R3 (Right Stick)"
};

void set_nonblocking(bool enable) {
    struct termios ttystate;
    tcgetattr(STDIN_FILENO, &ttystate);
    if (enable) {
        ttystate.c_lflag &= ~(ICANON | ECHO);
        ttystate.c_cc[VMIN] = 0;
        ttystate.c_cc[VTIME] = 0;
    } else {
        ttystate.c_lflag |= ICANON | ECHO;
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &ttystate);
}

bool key_pressed() {
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    return poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN);
}

char get_key_with_timeout(int timeout_ms) {
    struct pollfd pfd = {STDIN_FILENO, POLLIN, 0};
    int ret = poll(&pfd, 1, timeout_ms);
    
    if (ret > 0 && (pfd.revents & POLLIN)) {
        char c;
        if (read(STDIN_FILENO, &c, 1) == 1) {
            return c;
        }
    }
    return 0; // timeout or no input
}

std::string get_line_input() {
    std::string line;
    std::getline(std::cin, line);
    return line;
}

int get_user_choice(int max_index, int default_choice = 0) {
    while (true) {
        std::string input = get_line_input();
        if (input.empty()) {
            return default_choice;
        }
        
        try {
            int choice = std::stoi(input);
            if (choice >= 0 && choice <= max_index) {
                return choice;
            }
        } catch (...) {
            // Invalid input
        }
        
        std::cout << "Invalid choice. Please enter a number between 0 and " << max_index << ": ";
    }
}

std::string get_udev_property(const std::string& device_path, const std::string& property) {
    std::string cmd = "udevadm info -q property -n " + device_path + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    
    std::string output;
    char buffer[128];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
        output += buffer;
    }
    pclose(pipe);
    
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.substr(0, property.length() + 1) == property + "=") {
            return line.substr(property.length() + 1);
        }
    }
    return "";
}

std::string find_by_id_path(const std::string& device_path) {
    std::string cmd = "find /dev/input/by-id/ -lname '" + device_path + "' 2>/dev/null | head -1";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) return "";
    
    char result[256];
    if (fgets(result, sizeof(result), pipe)) {
        result[strcspn(result, "\n")] = 0;
        pclose(pipe);
        return std::string(result);
    }
    pclose(pipe);
    return "";
}

std::string detect_device_role(const DeviceInfo& device) {
    // Look at device name for better detection
    const char* name = libevdev_get_name(device.dev);
    std::string device_name = name ? name : "";
    
    // Thrustmaster specific detection
    if (device_name.find("TWCS") != std::string::npos) {
        return "throttle";
    }
    if (device_name.find("T-Rudder") != std::string::npos) {
        return "rudder";
    }
    if (device_name.find("T.16000M") != std::string::npos) {
        return "stick";
    }
    
    // Count available axes and buttons
    bool has_xy = libevdev_has_event_code(device.dev, EV_ABS, ABS_X) && 
                  libevdev_has_event_code(device.dev, EV_ABS, ABS_Y);
    bool has_z = libevdev_has_event_code(device.dev, EV_ABS, ABS_Z) ||
                 libevdev_has_event_code(device.dev, EV_ABS, ABS_THROTTLE);
    bool has_rz = libevdev_has_event_code(device.dev, EV_ABS, ABS_RZ);
    bool has_hat = libevdev_has_event_code(device.dev, EV_ABS, ABS_HAT0X) ||
                   libevdev_has_event_code(device.dev, EV_ABS, ABS_HAT0Y);
    
    int button_count = 0;
    for (int code = BTN_JOYSTICK; code < BTN_DIGI; code++) {
        if (libevdev_has_event_code(device.dev, EV_KEY, code)) {
            button_count++;
        }
    }
    
    // Generic role detection logic
    if (has_xy && !has_z && !has_rz) {
        return "stick";
    } else if (!has_xy && has_z && !has_rz && button_count >= 8) {
        return "throttle";  
    } else if (!has_xy && !has_z && has_rz && button_count <= 4) {
        return "rudder";
    } else if (has_xy && (has_z || has_rz)) {
        // Complex device, prefer stick
        return "stick";
    }
    
    return has_xy ? "stick" : (has_z ? "throttle" : "rudder");
}

std::vector<DeviceInfo> detect_devices() {
    std::vector<DeviceInfo> devices;
    
    // Enumerate /dev/input/by-id/
    const char* by_id_dir = "/dev/input/by-id";
    DIR* dir = opendir(by_id_dir);
    if (!dir) {
        std::cerr << "Failed to open " << by_id_dir << "\n";
        return devices;
    }
    
    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strstr(entry->d_name, "event") == nullptr) continue;
        
        std::string by_id_path = std::string(by_id_dir) + "/" + entry->d_name;
        char real_path[PATH_MAX];
        if (realpath(by_id_path.c_str(), real_path) == nullptr) continue;
        
        int fd = open(real_path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) continue;
        
        struct libevdev* dev = nullptr;
        if (libevdev_new_from_fd(fd, &dev) != 0) {
            close(fd);
            continue;
        }
        
        // Skip keyboards and mice
        if (libevdev_has_event_type(dev, EV_KEY) && 
            (libevdev_has_event_code(dev, EV_KEY, KEY_A) || 
             libevdev_has_event_code(dev, EV_KEY, BTN_LEFT))) {
            libevdev_free(dev);
            close(fd);
            continue;
        }
        
        // Must have at least some absolute axes to be a joystick
        if (!libevdev_has_event_type(dev, EV_ABS)) {
            libevdev_free(dev);
            close(fd);
            continue;
        }
        
        DeviceInfo info;
        info.path = real_path;
        info.by_id = by_id_path;
        info.fd = fd;
        info.dev = dev;
        info.vendor = get_udev_property(real_path, "ID_VENDOR_ID");
        info.product = get_udev_property(real_path, "ID_MODEL_ID");
        info.role = detect_device_role(info);
        
        devices.push_back(info);
    }
    closedir(dir);
    
    // Sort by preference (Thrustmaster stick first, then by role priority)
    std::sort(devices.begin(), devices.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
        if (a.vendor == "044f" && a.role == "stick" && (b.vendor != "044f" || b.role != "stick")) return true;
        if (b.vendor == "044f" && b.role == "stick" && (a.vendor != "044f" || a.role != "stick")) return false;
        if (a.role == "stick" && b.role != "stick") return true;
        if (b.role == "stick" && a.role != "stick") return false;
        if (a.role == "throttle" && b.role != "throttle") return true;
        if (b.role == "throttle" && a.role != "throttle") return false;
        return a.role == "rudder" && b.role != "rudder";
    });
    
    return devices;
}

std::vector<DeviceInfo> build_devices_from_config_inputs(const Config& cfg) {
    std::vector<DeviceInfo> devices;
    
    for (const auto& input_config : cfg.inputs) {
        if (input_config.by_id.empty()) {
            std::cout << "Skipping " << input_config.role << " (no by_id path configured)\n";
            continue;
        }
        
        // Resolve by_id symlink to real event path
        char real_path[PATH_MAX];
        if (realpath(input_config.by_id.c_str(), real_path) == nullptr) {
            std::cerr << "ERROR: Failed to resolve " << input_config.role << " path: " << input_config.by_id << "\n";
            continue;
        }
        
        // Open device
        int fd = open(real_path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            std::cerr << "ERROR: Failed to open " << input_config.role << ": " << input_config.by_id << " (" << strerror(errno) << ")\n";
            continue;
        }
        
        // Initialize libevdev
        struct libevdev* dev = nullptr;
        if (libevdev_new_from_fd(fd, &dev) != 0) {
            std::cerr << "ERROR: Failed to initialize " << input_config.role << ": " << input_config.by_id << "\n";
            close(fd);
            continue;
        }
        
        // Create DeviceInfo from config (do NOT infer role)
        DeviceInfo info;
        info.path = real_path;
        info.by_id = input_config.by_id;
        info.fd = fd;
        info.dev = dev;
        
        // Use config values first, fallback to udev if needed
        if (!input_config.vendor.empty()) {
            info.vendor = input_config.vendor;
        } else {
            info.vendor = get_udev_property(real_path, "ID_VENDOR_ID");
        }
        
        if (!input_config.product.empty()) {
            info.product = input_config.product;
        } else {
            info.product = get_udev_property(real_path, "ID_MODEL_ID");
        }
        
        info.role = input_config.role; // Use config role, do NOT infer
        
        devices.push_back(info);
        std::cout << "Configured " << input_config.role << ": " << input_config.by_id;
        if (!info.vendor.empty() && !info.product.empty()) {
            std::cout << " (vendor:" << info.vendor << " product:" << info.product << ")";
        }
        std::cout << "\n";
    }
    
    return devices;
}

std::map<std::string, DeviceInfo> select_devices_per_role(const std::vector<DeviceInfo>& all_devices, const Config& cfg) {
    std::map<std::string, DeviceInfo> selected_devices;
    
    // Build per-role required flag from config
    std::map<std::string, bool> role_required;
    for (const auto& input : cfg.inputs) {
        role_required[input.role] = !input.optional;
    }
    
    std::vector<std::string> roles_to_check = {"stick", "throttle", "rudder"};
    
    for (const auto& role : roles_to_check) {
        // Find candidate devices for this role
        std::vector<DeviceInfo> candidates;
        for (const auto& device : all_devices) {
            if (device.role == role) {
                candidates.push_back(device);
            }
        }
        
        if (candidates.empty()) {
            bool required = role_required[role];
            if (required) {
                std::cerr << "ERROR: No " << role << " devices found! This device is required by config.\n";
                return {};
            } else {
                std::cout << "No " << role << " devices found (optional according to config).\n";
                continue;
            }
        }
        
        std::cout << "\nSelect " << role << " device:\n";
        for (size_t i = 0; i < candidates.size(); i++) {
            const auto& device = candidates[i];
            std::cout << "  [" << i << "] " << device.by_id;
            if (!device.vendor.empty() && !device.product.empty()) {
                std::cout << " (vendor:" << device.vendor << " product:" << device.product << ")";
            }
            std::cout << "\n";
        }
        
        if (candidates.size() == 1) {
            std::cout << "Enter choice [0-0] (default 0): ";
        } else {
            std::cout << "Enter choice [0-" << (candidates.size() - 1) << "] (default 0): ";
        }
        int choice = get_user_choice(static_cast<int>(candidates.size() - 1), 0);
        
        selected_devices[role] = candidates[choice];
        std::cout << "Selected: " << selected_devices[role].by_id << "\n";
    }
    
    return selected_devices;
}

int capture_single_axis(DeviceInfo& device, int capture_time_ms = 1500) {
    const int JITTER_THRESHOLD = 32;
    std::map<int, int> delta_sum;
    
    // Drain pending events first
    struct input_event ev;
    while (libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS) {
        // Just drain the queue
    }
    
    // Get initial values as baseline
    std::map<int, int> baseline_values;
    for (int code = 0; code <= ABS_MAX; code++) {
        if (libevdev_has_event_code(device.dev, EV_ABS, code)) {
            const struct input_absinfo* absinfo = libevdev_get_abs_info(device.dev, code);
            if (absinfo) {
                baseline_values[code] = absinfo->value;
                delta_sum[code] = 0;
            }
        }
    }
    
    auto start = std::chrono::steady_clock::now();
    
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(capture_time_ms)) {
        int rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_ABS) {
            int baseline = baseline_values[ev.code];
            int delta = std::abs(ev.value - baseline);
            
            if (delta >= JITTER_THRESHOLD) {
                delta_sum[ev.code] += delta;
            }
        } else if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    // Find axis with highest delta
    int best_code = -1;
    int max_delta = 0;
    for (const auto& [code, delta] : delta_sum) {
        if (delta > max_delta) {
            max_delta = delta;
            best_code = code;
        }
    }
    
    return best_code;
}

bool get_invert_preference(const std::string& axis_name) {
    std::cout << "Invert " << axis_name << "? (y/n, default n): ";
    std::string input = get_line_input();
    return (!input.empty() && (input[0] == 'y' || input[0] == 'Y'));
}

void capture_axes(CaptureState& state) {
    std::cout << "\n=== Axis Capture ===\n";
    std::cout << "This will capture axes one at a time for precise mapping.\n\n";
    
    // CYCLIC X (right stick X -> ABS_RX)
    if (state.devices.empty()) {
        std::cerr << "No devices available for axis capture!\n";
        return;
    }
    
    // Find stick device for cyclic controls
    DeviceInfo* stick = nullptr;
    for (auto& device : state.devices) {
        if (device.role == "stick") {
            stick = &device;
            break;
        }
    }
    
    if (!stick) {
        std::cerr << "ERROR: No stick device found for cyclic controls!\n";
        return;
    }
    
    // Capture CYCLIC X
    std::cout << "A) CYCLIC X (right stick X axis)\n";
    std::cout << "Press ENTER and move stick left/right...";
    get_line_input();
    
    int cyclic_x_code = capture_single_axis(*stick);
    if (cyclic_x_code >= 0) {
        bool invert = get_invert_preference("Cyclic X");
        state.captured_axes.push_back(CapturedAxis{"stick", cyclic_x_code, ABS_RX, invert, 0, 1.0f});
        std::cout << "Captured CYCLIC X: code " << cyclic_x_code << " -> virtual ABS_RX(3) invert=" << invert << "\n\n";
    } else {
        std::cout << "No movement detected. Retrying...\n";
        // Retry once
        std::cout << "Press ENTER and move stick left/right...";
        get_line_input();
        cyclic_x_code = capture_single_axis(*stick);
        if (cyclic_x_code >= 0) {
            bool invert = get_invert_preference("Cyclic X");
            state.captured_axes.push_back(CapturedAxis{"stick", cyclic_x_code, ABS_RX, invert, 0, 1.0f});
            std::cout << "Captured CYCLIC X: code " << cyclic_x_code << " -> virtual ABS_RX(3) invert=" << invert << "\n\n";
        }
    }
    
    // Capture CYCLIC Y
    std::cout << "B) CYCLIC Y (right stick Y axis)\n";
    std::cout << "Press ENTER and move stick forward/back...";
    get_line_input();
    
    int cyclic_y_code = capture_single_axis(*stick);
    if (cyclic_y_code >= 0) {
        bool invert = get_invert_preference("Cyclic Y");
        state.captured_axes.push_back(CapturedAxis{"stick", cyclic_y_code, ABS_RY, invert, 0, 1.0f});
        std::cout << "Captured CYCLIC Y: code " << cyclic_y_code << " -> virtual ABS_RY(4) invert=" << invert << "\n\n";
    } else {
        std::cout << "No movement detected. Retrying...\n";
        std::cout << "Press ENTER and move stick forward/back...";
        get_line_input();
        cyclic_y_code = capture_single_axis(*stick);
        if (cyclic_y_code >= 0) {
            bool invert = get_invert_preference("Cyclic Y");
            state.captured_axes.push_back(CapturedAxis{"stick", cyclic_y_code, ABS_RY, invert, 0, 1.0f});
            std::cout << "Captured CYCLIC Y: code " << cyclic_y_code << " -> virtual ABS_RY(4) invert=" << invert << "\n\n";
        }
    }
    
    // Capture COLLECTIVE (left stick Y -> ABS_Y)
    DeviceInfo* throttle = nullptr;
    for (auto& device : state.devices) {
        if (device.role == "throttle") {
            throttle = &device;
            break;
        }
    }
    
    if (throttle) {
        std::cout << "C) COLLECTIVE (throttle/collective)\n";
        std::cout << "Press ENTER and move throttle through full range...";
        get_line_input();
        
        int collective_code = capture_single_axis(*throttle);
        if (collective_code >= 0) {
            bool invert = get_invert_preference("Collective");
            state.captured_axes.push_back(CapturedAxis{"throttle", collective_code, ABS_Y, invert, 0, 1.0f});
            std::cout << "Captured COLLECTIVE: code " << collective_code << " -> virtual ABS_Y(1) invert=" << invert << "\n\n";
        } else {
            std::cout << "No movement detected. Retrying...\n";
            std::cout << "Press ENTER and move throttle through full range...";
            get_line_input();
            collective_code = capture_single_axis(*throttle);
            if (collective_code >= 0) {
                bool invert = get_invert_preference("Collective");
                state.captured_axes.push_back(CapturedAxis{"throttle", collective_code, ABS_Y, invert, 0, 1.0f});
                std::cout << "Captured COLLECTIVE: code " << collective_code << " -> virtual ABS_Y(1) invert=" << invert << "\n\n";
            }
        }
    } else {
        std::cout << "C) COLLECTIVE - no throttle device found, skipping\n\n";
    }
    
    // Capture ANTI-TORQUE (left stick X -> ABS_X)
    DeviceInfo* rudder = nullptr;
    for (auto& device : state.devices) {
        if (device.role == "rudder") {
            rudder = &device;
            break;
        }
    }
    
    if (rudder) {
        std::cout << "D) ANTI-TORQUE (rudder pedals)\n";
        std::cout << "Press ENTER and move rudder pedals left/right...";
        get_line_input();
        
        int antitorque_code = capture_single_axis(*rudder);
        if (antitorque_code >= 0) {
            bool invert = get_invert_preference("Anti-torque");
            state.captured_axes.push_back(CapturedAxis{"rudder", antitorque_code, ABS_X, invert, 0, 1.0f});
            std::cout << "Captured ANTI-TORQUE: code " << antitorque_code << " -> virtual ABS_X(0) invert=" << invert << "\n\n";
        } else {
            std::cout << "No movement detected. Retrying...\n";
            std::cout << "Press ENTER and move rudder pedals left/right...";
            get_line_input();
            antitorque_code = capture_single_axis(*rudder);
            if (antitorque_code >= 0) {
                bool invert = get_invert_preference("Anti-torque");
                state.captured_axes.push_back(CapturedAxis{"rudder", antitorque_code, ABS_X, invert, 0, 1.0f});
                std::cout << "Captured ANTI-TORQUE: code " << antitorque_code << " -> virtual ABS_X(0) invert=" << invert << "\n\n";
            }
        }
    } else {
        std::cout << "D) ANTI-TORQUE - no rudder device found, skipping\n\n";
    }
}

void capture_buttons(CaptureState& state) {
    std::cout << "\n=== Phase 2: Button Capture ===\n";
    std::cout << "Buttons will be captured one at a time in the following order:\n";
    for (size_t i = 0; i < BUTTON_NAMES.size(); i++) {
        std::cout << "  " << (i + 1) << ". " << BUTTON_NAMES[i] << "\n";
    }
    std::cout << "\nControls: 's' to skip current button, 'r' to restart Phase 2, ENTER to accept detected button\n\n";
    
    state.captured_buttons.clear();
    
    for (size_t i = 0; i < VIRTUAL_BUTTONS.size(); i++) {
        bool restart_phase = false;
        std::string captured_device;
        int captured_code = 0;
        bool has_detection = false;
        
        while (true) {
            if (!has_detection) {
                std::cout << "Press button for: " << BUTTON_NAMES[i] << " (" << VIRTUAL_BUTTONS[i] << ")\n";
                std::cout << "Waiting for input... ";
                std::cout.flush();
            }
            
            set_nonblocking(true);
            
            while (true) {
                // Check for keyboard controls
                char c = get_key_with_timeout(0);
                if (c == 's' || c == 'S') {
                    std::cout << "SKIPPED\n";
                    set_nonblocking(false);
                    has_detection = false;
                    break;
                } else if (c == 'r' || c == 'R') {
                    std::cout << "RESTARTING\n";
                    set_nonblocking(false);
                    restart_phase = true;
                    has_detection = false;
                    break;
                } else if (c == '\r' || c == '\n') {
                    if (has_detection) {
                        std::cout << "ACCEPTED\n";
                        state.captured_buttons.push_back({captured_device, captured_code});
                        set_nonblocking(false);
                        goto next_button;
                    }
                }
                
                // Listen to all devices for button press
                for (auto& device : state.devices) {
                    struct input_event ev;
                    int rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                    
                    if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_KEY && ev.value == 1) {
                        // Clear the "Waiting for input..." line
                        std::cout << "\r" << std::string(50, ' ') << "\r";
                        std::cout << "Detected: " << device.role << " " << ev.code << "\n";
                        std::cout << "Press ENTER to accept, or press another button to override\n";
                        std::cout.flush();
                        
                        captured_device = device.role;
                        captured_code = ev.code;
                        has_detection = true;
                        break;
                    }
                }
                
                if (restart_phase || has_detection) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            
            set_nonblocking(false);
            
            if (restart_phase) {
                state.captured_buttons.clear();
                i = -1; // Will increment to 0 in outer loop
                break;
            }
            
            if (!has_detection) {
                break; // Skipped, move to next button
            }
        }
        
        next_button:
        if (restart_phase) {
            continue; // Restart the outer loop
        }
    }
    
    std::cout << "\nCaptured " << state.captured_buttons.size() << " out of " << VIRTUAL_BUTTONS.size() << " buttons\n";
}

bool write_config(const CaptureState& state, const Config& existing_config) {
    std::string config_dir = std::string(getenv("HOME")) + "/.config/twcs-mapper";
    std::string config_path = config_dir + "/config.json";
    
    // Use existing config and only update what's necessary
    Config config = existing_config;
    
    // Update inputs based on selected devices
    config.inputs.clear();
    for (const auto& device : state.devices) {
        InputConfig input;
        input.role = device.role;
        input.by_id = device.by_id;
        input.vendor = device.vendor;
        input.product = device.product;
        input.optional = (device.role != "stick"); // Stick is required, others optional
        
        config.inputs.push_back(input);
    }
    
    // Update bindings
    config.bindings_keys.clear();
    for (size_t i = 0; i < state.captured_buttons.size() && i < VIRTUAL_BUTTONS.size(); i++) {
        BindingConfigKey binding;
        binding.role = state.captured_buttons[i].first;
        binding.src = state.captured_buttons[i].second;
        binding.dst = VIRTUAL_BUTTONS[i];
        
        config.bindings_keys.push_back(binding);
    }
    
    config.bindings_abs.clear();
    for (const auto& axis : state.captured_axes) {
        BindingConfigAbs binding;
        binding.role = axis.role;
        binding.src = axis.src;
        binding.dst = axis.dst;
        binding.invert = axis.invert;
        binding.deadzone = axis.deadzone;
        binding.scale = axis.scale;
        
        config.bindings_abs.push_back(binding);
    }
    
    // Save config
    return ConfigManager::save(config_path, config);
}

bool install_service() {
    std::string service_dir = std::string(getenv("HOME")) + "/.config/systemd/user";
    std::string service_file = service_dir + "/twcs-mapper.service";
    
    // Create directory if needed
    std::string mkdir_cmd = "mkdir -p \"" + service_dir + "\"";
    if (system(mkdir_cmd.c_str()) != 0) {
        return false;
    }
    
    // Write service file
    std::ofstream file(service_file);
    if (!file.is_open()) {
        return false;
    }
    
    file << "[Unit]\n";
    file << "Description=TWCS ARMA Mapper\n\n";
    file << "[Service]\n";
    file << "Type=simple\n";
    file << "ExecStart=" << getenv("HOME") << "/.local/bin/twcs_mapper\n";
    file << "Restart=on-failure\n";
    file << "RestartSec=1\n";
    file << "WorkingDirectory=" << getenv("HOME") << "\n\n";
    file << "[Install]\n";
    file << "WantedBy=default.target\n";
    
    file.close();
    
    // Reload and enable service
    system("systemctl --user daemon-reload");
    system("systemctl --user enable --now twcs-mapper.service");
    
    return true;
}

void print_summary(const CaptureState& state) {
    std::cout << "\n=== Confirmation ===\n";
    std::cout << "Selected devices:\n";
    for (const auto& device : state.devices) {
        std::cout << "  " << device.role << ": " << device.by_id;
        if (!device.vendor.empty() && !device.product.empty()) {
            std::cout << " (vendor:" << device.vendor << " product:" << device.product << ")";
        }
        std::cout << "\n";
    }
    
    std::cout << "\nARMA Helicopter Axis Mappings:\n";
    for (const auto& axis : state.captured_axes) {
        std::string dst_name;
        if (axis.dst == ABS_RX) dst_name = "ABS_RX (Cyclic X - right stick X)";
        else if (axis.dst == ABS_RY) dst_name = "ABS_RY (Cyclic Y - right stick Y)";
        else if (axis.dst == ABS_X) dst_name = "ABS_X (Anti-torque - left stick X)";
        else if (axis.dst == ABS_Y) dst_name = "ABS_Y (Collective - left stick Y)";
        else dst_name = "ABS_" + std::to_string(axis.dst);
        
        std::cout << "  " << axis.role << " code " << axis.src 
                 << " -> " << dst_name;
        if (axis.invert) std::cout << " [INVERTED]";
        std::cout << "\n";
    }
    
    std::cout << "\nCaptured " << state.captured_buttons.size() << " button bindings\n";
    for (size_t i = 0; i < state.captured_buttons.size() && i < BUTTON_NAMES.size(); i++) {
        std::cout << "  " << state.captured_buttons[i].first << " " << state.captured_buttons[i].second 
                 << " -> " << BUTTON_NAMES[i] << "\n";
    }
}

int main() {
    std::cout << "=== TWCS ARMA Setup ===\n";
    std::cout << "This will help you select devices and capture controls for ARMA helicopter mapping.\n\n";
    
    CaptureState state;
    
    // Phase 0: Load Config and Handle Reset
    std::string config_dir = std::string(getenv("HOME")) + "/.config/twcs-mapper";
    std::string config_path = config_dir + "/config.json";
    
    Config config;
    auto config_opt = ConfigManager::load(config_path);
    
    if (config_opt) {
        std::cout << "Existing config detected at " << config_path << "\n";
        std::cout << "Delete and regenerate? [y/N]: ";
        std::string resp;
        std::getline(std::cin, resp);
        bool should_delete = !resp.empty() && (resp[0] == 'y' || resp[0] == 'Y');
        
        if (should_delete) {
            if (std::filesystem::remove(config_path)) {
                std::cout << "Config deleted\n";
                config_opt = std::nullopt;
            } else {
                std::cerr << "Failed to delete config file\n";
            }
        }
    }
    
    if (!config_opt) {
        std::cout << "Creating new setup...\n";
        config.uinput_name = "Thrustmaster ARMA Virtual";
        config.grab = true;
    } else {
        config = *config_opt;
        std::cout << "Using existing config\n";
    }
    
    std::vector<DeviceInfo> all_devices;
    
    // Try config-driven device building first
    if (!config.inputs.empty()) {
        std::cout << "\nPhase 0: Building devices from config...\n";
        all_devices = build_devices_from_config_inputs(config);
    }
    
    // Fallback to heuristic scanning if no config devices
    if (all_devices.empty()) {
        std::cout << "\nPhase 0: No valid config devices, scanning all devices...\n";
        all_devices = detect_devices();
    }
    
    if (all_devices.empty()) {
        std::cerr << "Error: No joystick devices available!\n";
        return 1;
    }
    
    std::cout << "\nAvailable devices for selection:\n";
    for (const auto& device : all_devices) {
        std::cout << "  " << device.role << ": " << device.by_id;
        if (!device.vendor.empty() && !device.product.empty()) {
            std::cout << " (vendor:" << device.vendor << " product:" << device.product << ")";
        }
        std::cout << "\n";
    }
    
    // Phase 1: Device Selection per role
    std::cout << "\nPhase 1: Device Selection\n";
    auto selected_devices = select_devices_per_role(all_devices, config);
    
    if (selected_devices.empty()) {
        std::cerr << "ERROR: Required device(s) not selected!\n";
        return 1;
    }
    
    // Close and free unselected devices to avoid resource leaks
    std::set<std::string> selected_roles;
    for (const auto& [role, device] : selected_devices) {
        selected_roles.insert(role);
    }
    
    for (auto& device : all_devices) {
        if (selected_roles.find(device.role) == selected_roles.end()) {
            std::cout << "Closing unselected " << device.role << ": " << device.by_id << "\n";
            libevdev_free(device.dev);
            close(device.fd);
        }
    }
    
    // Convert selected devices to state.devices format
    for (const auto& [role, device] : selected_devices) {
        state.devices.push_back(device);
    }
    
    // Phase 2: Axis Capture
    capture_axes(state);
    
    // Phase 3: Button Capture
    capture_buttons(state);
    
    // Phase 4: Confirmation with redo loop
    while (true) {
        print_summary(state);
        
        std::cout << "\nPress ENTER to accept, or wait 5 seconds to accept automatically.\n";
        std::cout << "Press 'r' to redo capture.\n";
        
        set_nonblocking(true);
        char c = get_key_with_timeout(5000); // 5 second timeout
        set_nonblocking(false);
        
        if (c == '\r' || c == '\n' || c == 0) { // ENTER or timeout
            break; // accept and continue
        } else if (c == 'r' || c == 'R') {
            std::cout << "\n=== Redoing Capture ===\n";
            
            // Reopen all devices to reset state
            for (auto& device : state.devices) {
                libevdev_free(device.dev);
                close(device.fd);
                
                device.fd = open(device.path.c_str(), O_RDONLY | O_NONBLOCK);
                if (device.fd >= 0) {
                    libevdev_new_from_fd(device.fd, &device.dev);
                }
            }
            
            // Clear previous captures
            state.captured_axes.clear();
            state.captured_buttons.clear();
            
            // Redo capture phases
            capture_axes(state);
            capture_buttons(state);
            
            continue; // loop back to confirmation
        }
    }
    
// Phase 5: Config Write
    std::cout << "\n=== Phase 5: Writing Configuration ===\n";
    if (!write_config(state, config)) {
        std::cerr << "Error: Failed to write config file!\n";
        return 1;
    }
    std::cout << "Configuration written to ~/.config/twcs-mapper/config.json\n";
    
    // Phase 5: Service Install & Start
    std::cout << "\n=== Phase 5: Installing and Starting Service ===\n";
    if (!install_service()) {
        std::cerr << "Error: Failed to install service!\n";
        return 1;
    }
    
    std::cout << "Service installed and started.\n\n";
    
    // Show status
    std::cout << "Service status:\n";
    system("systemctl --user status twcs-mapper.service --no-pager -l");
    
    std::cout << "\nLast 10 journal lines:\n";
    system("journalctl --user -u twcs-mapper.service -n 10 --no-pager");
    
    std::cout << "\n✓ Setup complete! ARMA should now see 'Thrustmaster ARMA Virtual' controller.\n";
    std::cout << "\nExpected ARMA helicopter behavior:\n";
    std::cout << "  - Physical stick X/Y -> Right stick (cyclic)\n";
    std::cout << "  - Physical rudder -> Left stick X (anti-torque)\n";
    std::cout << "  - Physical throttle -> Left stick Y (collective)\n";
    
    return 0;
}