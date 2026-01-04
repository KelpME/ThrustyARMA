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

void monitor_variance(DeviceInfo& device, std::chrono::milliseconds duration) {
    auto start = std::chrono::steady_clock::now();
    const int EPSILON = 16; // Noise gate threshold
    
    // Initialize baseline values from current device state
    for (int code = 0; code <= ABS_MAX; code++) {
        if (libevdev_has_event_code(device.dev, EV_ABS, code)) {
            const struct input_absinfo* absinfo = libevdev_get_abs_info(device.dev, code);
            if (absinfo) {
                device.abs_values[code] = absinfo->value;
                device.abs_variance[code] = 0;
            }
        }
    }
    
    device.last_update = std::chrono::steady_clock::now();
    
    while (std::chrono::steady_clock::now() - start < duration) {
        struct input_event ev;
        int rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_ABS) {
            int current = ev.value;
            int baseline = device.abs_values[ev.code];
            int delta = std::abs(current - baseline);
            
            // Apply noise gating: only count movement if delta exceeds epsilon
            if (delta >= EPSILON) {
                device.abs_variance[ev.code] = std::max(device.abs_variance[ev.code], delta);
                device.last_update = std::chrono::steady_clock::now();
            }
        } else if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
}

void capture_axes(CaptureState& state) {
    // Phase 1A: Stick axes (5 seconds)
    std::cout << "\n=== Phase 1: Axis Capture ===\n";
    std::cout << "Move the STICK left/right and up/down (5 seconds)\n";
    std::cout << "Starting in 3... ";
    std::cout.flush();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "2... ";
    std::cout.flush();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "1... ";
    std::cout.flush();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    std::cout << "GO!\n";
    
    // Find stick device
    DeviceInfo* stick = nullptr;
    for (auto& device : state.devices) {
        if (device.role == "stick") {
            stick = &device;
            break;
        }
    }
    
    if (stick) {
        monitor_variance(*stick, std::chrono::seconds(5));
        
        // Find top 2 axes with highest variance
        std::vector<std::pair<int, int>> axis_variance;
        for (const auto& [code, variance] : stick->abs_variance) {
            if (variance > 0) {
                axis_variance.push_back({code, variance});
            }
        }
        
        std::sort(axis_variance.begin(), axis_variance.end(), 
                 [](const auto& a, const auto& b) { return a.second > b.second; });
        
        if (axis_variance.size() >= 2) {
            state.captured_axes.push_back(CapturedAxis{"stick", axis_variance[0].first, ABS_X, false, 0, 1.0f});
            state.captured_axes.push_back(CapturedAxis{"stick", axis_variance[1].first, ABS_Y, false, 0, 1.0f});
            std::cout << "Captured X axis: " << axis_variance[0].first << " (variance: " << axis_variance[0].second << ")\n";
            std::cout << "Captured Y axis: " << axis_variance[1].first << " (variance: " << axis_variance[1].second << ")\n";
        }
        
        // Check for HAT
        bool has_hat_x = stick->abs_variance.count(ABS_HAT0X) && stick->abs_variance.at(ABS_HAT0X) > 0;
        bool has_hat_y = stick->abs_variance.count(ABS_HAT0Y) && stick->abs_variance.at(ABS_HAT0Y) > 0;
        if (has_hat_x) {
            state.captured_axes.push_back(CapturedAxis{"stick", ABS_HAT0X, ABS_HAT0X, false, 0, 1.0f});
            std::cout << "Captured HAT X axis\n";
        }
        if (has_hat_y) {
            state.captured_axes.push_back(CapturedAxis{"stick", ABS_HAT0Y, ABS_HAT0Y, false, 0, 1.0f});
            std::cout << "Captured HAT Y axis\n";
        }
    }
    
    // Phase 1B: Throttle axis (4 seconds, if present)
    DeviceInfo* throttle = nullptr;
    for (auto& device : state.devices) {
        if (device.role == "throttle") {
            throttle = &device;
            break;
        }
    }
    
    if (throttle) {
        std::cout << "\nMove the THROTTLE through its full range (4 seconds)\n";
        std::cout << "Starting in 2... ";
        std::cout.flush();
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::cout << "GO!\n";
        
        monitor_variance(*throttle, std::chrono::seconds(4));
        
        // Find axis with highest variance, prefer ABS_Z/ABS_THROTTLE
        std::vector<std::pair<int, int>> axis_variance;
        for (const auto& [code, variance] : throttle->abs_variance) {
            if (variance > 0) {
                int priority = (code == ABS_Z || code == ABS_THROTTLE) ? 1000 : 0;
                axis_variance.push_back({code, variance + priority});
            }
        }
        
        std::sort(axis_variance.begin(), axis_variance.end(), 
                 [](const auto& a, const auto& b) { return a.second > b.second; });
        
        if (!axis_variance.empty()) {
            state.captured_axes.push_back(CapturedAxis{"throttle", axis_variance[0].first, ABS_Z, false, 0, 1.0f});
            std::cout << "Captured throttle axis: " << axis_variance[0].first << " (variance: " << (axis_variance[0].second % 1000) << ")\n";
        }
    }
    
    // Phase 1C: Rudder axis (3 seconds, if present)
    DeviceInfo* rudder = nullptr;
    for (auto& device : state.devices) {
        if (device.role == "rudder") {
            rudder = &device;
            break;
        }
    }
    
    if (rudder) {
        std::cout << "\nMove the RUDDER pedals (3 seconds)\n";
        std::cout << "Starting in 2... ";
        std::cout.flush();
        std::this_thread::sleep_for(std::chrono::seconds(2));
        std::cout << "GO!\n";
        
        monitor_variance(*rudder, std::chrono::seconds(3));
        
        // Find axis with highest variance, prefer ABS_RZ
        std::vector<std::pair<int, int>> axis_variance;
        for (const auto& [code, variance] : rudder->abs_variance) {
            if (variance > 0) {
                int priority = (code == ABS_RZ) ? 1000 : 0;
                axis_variance.push_back({code, variance + priority});
            }
        }
        
        std::sort(axis_variance.begin(), axis_variance.end(), 
                 [](const auto& a, const auto& b) { return a.second > b.second; });
        
        if (!axis_variance.empty()) {
            state.captured_axes.push_back(CapturedAxis{"rudder", axis_variance[0].first, ABS_RZ, false, 0, 1.0f});
            std::cout << "Captured rudder axis: " << axis_variance[0].first << " (variance: " << (axis_variance[0].second % 1000) << ")\n";
        }
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

bool write_config(const CaptureState& state) {
    std::string config_dir = std::string(getenv("HOME")) + "/.config/twcs-mapper";
    std::string config_path = config_dir + "/config.json";
    
    // Load existing config to preserve non-binding settings
    Config config;
    auto existing = ConfigManager::load(config_path);
    if (existing) {
        config = *existing;
    } else {
        // Use default uinput_name for fresh config
        config.uinput_name = "Xbox 360 Controller (Virtual)";
    }
    
    // Update inputs
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
    std::cout << "\n=== Phase 3: Confirmation ===\n";
    std::cout << "Detected devices:\n";
    for (const auto& device : state.devices) {
        std::cout << "  " << device.role << ": " << device.by_id;
        if (!device.vendor.empty() && !device.product.empty()) {
            std::cout << " (vendor:" << device.vendor << " product:" << device.product << ")";
        }
        std::cout << "\n";
    }
    
    std::cout << "\nCaptured " << state.captured_axes.size() << " axis bindings\n";
    for (const auto& axis : state.captured_axes) {
        std::cout << "  " << axis.role << " " << axis.src << " -> virtual " << axis.dst << "\n";
    }
    
    std::cout << "\nCaptured " << state.captured_buttons.size() << " button bindings\n";
    for (size_t i = 0; i < state.captured_buttons.size(); i++) {
        std::cout << "  " << state.captured_buttons[i].first << " " << state.captured_buttons[i].second 
                 << " -> " << BUTTON_NAMES[i] << "\n";
    }
}

int main() {
    std::cout << "=== TWCS Quick Setup (30 seconds) ===\n";
    std::cout << "This will automatically detect your devices and configure them.\n\n";
    
    CaptureState state;
    
    // Phase 0: Device Detection
    std::cout << "Phase 0: Detecting devices...\n";
    state.devices = detect_devices();
    
    if (state.devices.empty()) {
        std::cerr << "Error: No joystick devices found!\n";
        return 1;
    }
    
    std::cout << "Found " << state.devices.size() << " devices:\n";
    for (const auto& device : state.devices) {
        std::cout << "  " << device.role << ": " << device.by_id << "\n";
    }
    
    // Phase 1: Axis Capture
    capture_axes(state);
    
    // Phase 2: Button Capture
    capture_buttons(state);
    
    // Phase 3: Confirmation with redo loop
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
    
    // Phase 4: Config Write
    std::cout << "\n=== Phase 4: Writing Configuration ===\n";
    if (!write_config(state)) {
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
    
    std::cout << "\n✓ Setup complete! ARMA should now see one virtual Xbox controller.\n";
    
    return 0;
}