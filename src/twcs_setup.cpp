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
    AxisCalibration calibration;
};

struct CaptureState {
    std::vector<DeviceInfo> devices;
    std::vector<std::pair<std::string, int>> captured_buttons;
    std::vector<CapturedAxis> captured_axes;
    bool abort = false;
    std::string abort_reason;
};

// Virtual button mappings in order
const std::vector<int> VIRTUAL_BUTTONS = {
    BTN_SOUTH,   // 1. South Button
    BTN_EAST,    // 2. East Button
    BTN_NORTH,   // 3. North Button
    BTN_WEST,    // 4. West Button
    BTN_TL,      // 5. Left Shoulder
    BTN_TR,      // 6. Right Shoulder
    BTN_TL2,     // 7. Left Trigger
    BTN_TR2,     // 8. Right Trigger
    BTN_SELECT,  // 9. Select
    BTN_START,   // 10. Start
    BTN_MODE,    // 11. Menu
    BTN_THUMBL,  // 12. Left Stick Button
    BTN_THUMBR,  // 13. Right Stick Button
    BTN_DPAD_UP,    // 14. D-pad Up
    BTN_DPAD_DOWN,  // 15. D-pad Down
    BTN_DPAD_LEFT,  // 16. D-pad Left
    BTN_DPAD_RIGHT  // 17. D-pad Right
};

const std::vector<std::string> BUTTON_NAMES = {
    "South Button", "East Button", "North Button", "West Button",
    "Left Shoulder", "Right Shoulder", "Left Trigger", "Right Trigger",
    "Select", "Start", "Menu", "Left Stick Button", "Right Stick Button",
    "D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right"
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

static bool check_fd_not_grabbed(int fd, const std::string& path, std::string& err) {
    errno = 0;
    if (ioctl(fd, EVIOCGRAB, 1) == 0) {
        ioctl(fd, EVIOCGRAB, 0);
        return true;
    }
    if (errno == EBUSY) {
        err = "Device '" + path + "' is exclusively grabbed via EVIOCGRAB by another process.\n"
              "Likely culprit: twcs_mapper with grab=true.\n"
              "Fix: Run 'make stop' to stop the mapper, then rerun 'make setup'.\n"
              "Alternative: set \"grab\": false in config.json for mapper, restart mapper later.";
        return false;
    }
    err = "Failed to test EVIOCGRAB on '" + path + "': " + std::string(strerror(errno));
    return false;
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
        
        // No filtering - detect all devices with event interface
        
        DeviceInfo info;
        info.path = real_path;
        info.by_id = by_id_path;
        info.fd = fd;
        info.dev = dev;
        info.vendor = get_udev_property(real_path, "ID_VENDOR_ID");
        info.product = get_udev_property(real_path, "ID_MODEL_ID");
        info.role = "";  // No role assigned yet
        
        devices.push_back(info);
    }
    closedir(dir);
    
    // Sort by preference (Thrustmaster devices first, keyboards/mice last)
    std::sort(devices.begin(), devices.end(), [](const DeviceInfo& a, const DeviceInfo& b) {
        // Check if device name contains "keyboard" or "mouse" (case insensitive)
        const char* name_a = libevdev_get_name(a.dev);
        const char* name_b = libevdev_get_name(b.dev);
        std::string str_a = name_a ? name_a : "";
        std::string str_b = name_b ? name_b : "";
        
        // Convert to lowercase for comparison
        std::transform(str_a.begin(), str_a.end(), str_a.begin(), ::tolower);
        std::transform(str_b.begin(), str_b.end(), str_b.begin(), ::tolower);
        
        bool a_is_kbd_mouse = (str_a.find("keyboard") != std::string::npos || 
                               str_a.find("mouse") != std::string::npos);
        bool b_is_kbd_mouse = (str_b.find("keyboard") != std::string::npos || 
                               str_b.find("mouse") != std::string::npos);
        
        // Push keyboards/mice to the end
        if (a_is_kbd_mouse && !b_is_kbd_mouse) return false;
        if (!a_is_kbd_mouse && b_is_kbd_mouse) return true;
        
        // Prioritize Thrustmaster devices (vendor 044f)
        if (a.vendor == "044f" && b.vendor != "044f") return true;
        if (b.vendor == "044f" && a.vendor != "044f") return false;
        
        return false;  // Keep original order otherwise
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

int get_smart_default_for_role(const std::vector<DeviceInfo>& devices, const std::string& role) {
    // Smart defaults based on device names
    for (size_t i = 0; i < devices.size(); i++) {
        const char* name = libevdev_get_name(devices[i].dev);
        std::string device_name = name ? name : "";
        
        if (role == "stick" && device_name.find("T.16000M") != std::string::npos) {
            return i;
        }
        if (role == "throttle" && device_name.find("TWCS") != std::string::npos) {
            return i;
        }
        if (role == "rudder" && device_name.find("T-Rudder") != std::string::npos) {
            return i;
        }
    }
    return 0;  // Default to first device
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
        bool required = role_required[role];
        
        if (all_devices.empty()) {
            if (required) {
                std::cerr << "ERROR: No devices available for " << role << "! This device is required by config.\n";
                return {};
            } else {
                std::cout << "No devices available for " << role << " (optional according to config).\n";
                continue;
            }
        }
        
        std::cout << "\nSelect " << role << " device:\n";
        for (size_t i = 0; i < all_devices.size(); i++) {
            const auto& device = all_devices[i];
            const char* name = libevdev_get_name(device.dev);
            std::cout << "  [" << i << "] " << device.by_id;
            if (name) {
                std::cout << " (" << name << ")";
            }
            std::cout << "\n";
        }
        
        int smart_default = get_smart_default_for_role(all_devices, role);
        std::cout << "Enter choice [0-" << (all_devices.size() - 1) << "] (default " << smart_default << "): ";
        int choice = get_user_choice(static_cast<int>(all_devices.size() - 1), smart_default);
        
        DeviceInfo selected = all_devices[choice];
        selected.role = role;  // Assign role at selection time
        selected_devices[role] = selected;
        std::cout << "Selected: " << selected_devices[role].by_id << "\n";
    }
    
    return selected_devices;
}

int detect_axis(DeviceInfo& device, const std::string& axis_name, int capture_time_ms = 4000) {
    const int JITTER_THRESHOLD = 100;  // Increased from 32 to reduce false detections
    const int MIN_MOVEMENT = 5000;      // Require significant movement
    std::map<int, int> delta_sum;
    struct input_event ev;
    
    // Drain pending events first
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
    
    // Show all detected movement for debugging
    std::cout << "  Movement detected:\n";
    for (const auto& [code, delta] : delta_sum) {
        if (delta > 0) {
            const char* axis_name = libevdev_event_code_get_name(EV_ABS, code);
            std::cout << "    Axis " << code << " (" << (axis_name ? axis_name : "UNKNOWN") 
                     << "): " << delta << " units\n";
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
    
    // Require minimum movement
    if (max_delta < MIN_MOVEMENT) {
        std::cout << "  WARNING: Movement too small (" << max_delta << " < " << MIN_MOVEMENT << ")\n";
        return -1;
    }
    
    return best_code;
}

std::pair<int, int> detect_two_axes(DeviceInfo& device, const std::string& description, int capture_time_ms = 2000) {
    const int JITTER_THRESHOLD = 32;
    std::map<int, int> delta_sum;
    struct input_event ev;
    
    // Drain pending events first
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
    
    // Find two axes with highest deltas
    int first_code = -1, second_code = -1;
    int first_delta = 0, second_delta = 0;
    
    for (const auto& [code, delta] : delta_sum) {
        if (delta > first_delta) {
            second_delta = first_delta;
            second_code = first_code;
            first_delta = delta;
            first_code = code;
        } else if (delta > second_delta) {
            second_delta = delta;
            second_code = code;
        }
    }
    
    return {first_code, second_code};
}

std::optional<AxisCalibration> calibrate_detected_axis(DeviceInfo& device, int axis_code, const std::string& axis_name) {
    struct input_event ev;
    
    // Drain pending events first
    while (libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS) {
        // Just drain the queue
    }
    
    std::cout << "  Calibrating axis " << axis_code << " (" << axis_name << ")\n";
    
    // Step 1: Measure center position and deadzone
    std::cout << "  Step 1: Leave " << axis_name << " centered and don't touch it for 5 seconds...\n";
    std::cout << "  Press ENTER when ready...";
    get_line_input();
    
    std::vector<int> center_samples;
    int center_min = INT_MAX;
    int center_max = INT_MIN;
    
    // Get initial value
    const struct input_absinfo* absinfo = libevdev_get_abs_info(device.dev, axis_code);
    if (absinfo) {
        center_samples.push_back(absinfo->value);
        center_min = absinfo->value;
        center_max = absinfo->value;
    }
    
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(5000)) {
        int rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_ABS && ev.code == axis_code) {
            center_samples.push_back(ev.value);
            center_min = std::min(center_min, ev.value);
            center_max = std::max(center_max, ev.value);
        } else if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    // Calculate center value and deadzone
    long long sum = 0;
    for (int val : center_samples) {
        sum += val;
    }
    int center_value = center_samples.empty() ? 0 : sum / center_samples.size();
    int deadzone_radius = 10;  // Fixed deadzone for centered axes
    
    // Step 2: Measure full range
    std::cout << "  Step 2: Move " << axis_name << " through FULL range in all directions for 10 seconds...\n";
    std::cout << "  Press ENTER when ready...";
    get_line_input();
    
    int range_min = center_value;
    int range_max = center_value;
    
    // Get current value as starting point
    absinfo = libevdev_get_abs_info(device.dev, axis_code);
    if (absinfo) {
        range_min = absinfo->value;
        range_max = absinfo->value;
    }
    
    start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(10000)) {
        int rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_ABS && ev.code == axis_code) {
            range_min = std::min(range_min, ev.value);
            range_max = std::max(range_max, ev.value);
        } else if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    // Show results to user
    std::cout << "  Observed MIN: " << range_min << "\n";
    std::cout << "  Observed MAX: " << range_max << "\n";
    std::cout << "  Center value: " << center_value << "\n";
    std::cout << "  Deadzone radius: " << deadzone_radius << "\n";
    
    // Validate range
    if (range_max - range_min < 100) {
        std::cout << "  ERROR: Range too small (" << (range_max - range_min) << " units). Did you move the axis?\n";
        return std::nullopt;
    }
    
    AxisCalibration cal;
    cal.src_code = axis_code;
    cal.observed_min = range_min;
    cal.observed_max = range_max;
    cal.center_value = center_value;
    cal.deadzone_radius = deadzone_radius;
    
    return cal;
}

std::pair<int, std::optional<AxisCalibration>> detect_and_calibrate_throttle_axis(DeviceInfo& device, const std::string& axis_name, int capture_time_ms = 6000) {
    const int JITTER_THRESHOLD = 100;
    const int MIN_MOVEMENT = 5000;
    std::map<int, int> delta_sum;
    std::map<int, int> range_min;
    std::map<int, int> range_max;
    struct input_event ev;
    
    // Drain pending events first
    while (libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS) {
        // Just drain the queue
    }
    
    // Get initial values as baseline and initialize min/max tracking
    std::map<int, int> baseline_values;
    for (int code = 0; code <= ABS_MAX; code++) {
        if (libevdev_has_event_code(device.dev, EV_ABS, code)) {
            const struct input_absinfo* absinfo = libevdev_get_abs_info(device.dev, code);
            if (absinfo) {
                baseline_values[code] = absinfo->value;
                delta_sum[code] = 0;
                range_min[code] = absinfo->value;
                range_max[code] = absinfo->value;
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
            
            // Track min/max for all axes
            range_min[ev.code] = std::min(range_min[ev.code], ev.value);
            range_max[ev.code] = std::max(range_max[ev.code], ev.value);
        } else if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    // Show all detected movement for debugging
    std::cout << "  Movement detected:\n";
    for (const auto& [code, delta] : delta_sum) {
        if (delta > 0) {
            const char* axis_name_str = libevdev_event_code_get_name(EV_ABS, code);
            std::cout << "    Axis " << code << " (" << (axis_name_str ? axis_name_str : "UNKNOWN") 
                     << "): " << delta << " units\n";
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
    
    // Require minimum movement
    if (max_delta < MIN_MOVEMENT) {
        std::cout << "  WARNING: Movement too small (" << max_delta << " < " << MIN_MOVEMENT << ")\n";
        return {-1, std::nullopt};
    }
    
    std::cout << "Detected axis code: " << best_code << "\n";
    
    // Use the captured min/max from the detection phase
    int obs_min = range_min[best_code];
    int obs_max = range_max[best_code];
    
    std::cout << "  Observed MIN (0%): " << obs_min << "\n";
    std::cout << "  Observed MAX (100%): " << obs_max << "\n";
    
    // Validate range
    if (obs_max - obs_min < 100) {
        std::cout << "  ERROR: Range too small (" << (obs_max - obs_min) << " units). Did you move the throttle through full range?\n";
        return {best_code, std::nullopt};
    }
    
    // For throttle: center is calculated as midpoint (not measured)
    AxisCalibration cal;
    cal.src_code = best_code;
    cal.observed_min = obs_min;
    cal.observed_max = obs_max;
    cal.center_value = (obs_min + obs_max) / 2;  // Calculate midpoint
    cal.deadzone_radius = 0;       // No deadzone for throttle
    
    return {best_code, cal};
}

// Detect which axis is moving and measure ONLY its center position
// Does NOT measure full range - that's done separately
std::pair<int, int> detect_axis_and_center_only(DeviceInfo& device, int capture_time_ms = 3000) {
    const int JITTER_THRESHOLD = 100;
    const int MIN_MOVEMENT = 3000;
    std::map<int, int> delta_sum;
    std::map<int, std::vector<int>> center_samples;
    std::map<int, int> baseline_values;
    struct input_event ev;
    
    // Drain pending events first
    while (libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS) {
        // Just drain the queue
    }
    
    // Get initial values as baseline
    for (int code = 0; code <= ABS_MAX; code++) {
        if (libevdev_has_event_code(device.dev, EV_ABS, code)) {
            const struct input_absinfo* absinfo = libevdev_get_abs_info(device.dev, code);
            if (absinfo) {
                baseline_values[code] = absinfo->value;
                delta_sum[code] = 0;
            }
        }
    }
    
    // Sample center values at the beginning (first 1 second while user holds still)
    auto start = std::chrono::steady_clock::now();
    auto center_end = start + std::chrono::milliseconds(1000);
    
    std::cout << "  Sampling center position (hold still)...\n";
    while (std::chrono::steady_clock::now() < center_end) {
        int rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_ABS) {
            center_samples[ev.code].push_back(ev.value);
        } else if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    std::cout << "  Now move the axis to detect it...\n";
    
    // Now capture movement for remaining time (user moves the axis)
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
    
    // Show all detected movement for debugging
    std::cout << "  Movement detected:\n";
    for (const auto& [code, delta] : delta_sum) {
        if (delta > 0) {
            const char* axis_name_str = libevdev_event_code_get_name(EV_ABS, code);
            std::cout << "    Axis " << code << " (" << (axis_name_str ? axis_name_str : "UNKNOWN") 
                     << "): " << delta << " units\n";
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
    
    // Require minimum movement
    if (max_delta < MIN_MOVEMENT) {
        std::cout << "  WARNING: Movement too small (" << max_delta << " < " << MIN_MOVEMENT << ")\n";
        return {-1, 0};
    }
    
    std::cout << "Detected axis code: " << best_code << "\n";
    
    // Calculate center value from initial samples (when user was holding still)
    int center_value = baseline_values[best_code];
    
    if (!center_samples[best_code].empty()) {
        long long sum = 0;
        for (int val : center_samples[best_code]) {
            sum += val;
        }
        center_value = sum / center_samples[best_code].size();
    }
    
    std::cout << "Center value: " << center_value << "\n";
    
    return {best_code, center_value};
}

std::pair<int, std::optional<AxisCalibration>> detect_and_calibrate_centered_axis(DeviceInfo& device, const std::string& axis_name, int capture_time_ms = 6000, bool offer_midpoint = false) {
    const int JITTER_THRESHOLD = 100;
    const int MIN_MOVEMENT = 5000;
    std::map<int, int> delta_sum;
    std::map<int, int> range_min;
    std::map<int, int> range_max;
    std::vector<std::map<int, int>> center_samples_map;
    struct input_event ev;
    
    // Drain pending events first
    while (libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS) {
        // Just drain the queue
    }
    
    // Get initial values as baseline and initialize tracking
    std::map<int, int> baseline_values;
    for (int code = 0; code <= ABS_MAX; code++) {
        if (libevdev_has_event_code(device.dev, EV_ABS, code)) {
            const struct input_absinfo* absinfo = libevdev_get_abs_info(device.dev, code);
            if (absinfo) {
                baseline_values[code] = absinfo->value;
                delta_sum[code] = 0;
                range_min[code] = absinfo->value;
                range_max[code] = absinfo->value;
            }
        }
    }
    
    // Sample center values at the beginning (first 1 second)
    auto start = std::chrono::steady_clock::now();
    auto center_end = start + std::chrono::milliseconds(1000);
    std::map<int, std::vector<int>> center_samples;
    
    while (std::chrono::steady_clock::now() < center_end) {
        int rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_ABS) {
            center_samples[ev.code].push_back(ev.value);
        } else if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    // Now capture movement for remaining time
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(capture_time_ms)) {
        int rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_ABS) {
            int baseline = baseline_values[ev.code];
            int delta = std::abs(ev.value - baseline);
            
            if (delta >= JITTER_THRESHOLD) {
                delta_sum[ev.code] += delta;
            }
            
            // Track min/max for all axes
            range_min[ev.code] = std::min(range_min[ev.code], ev.value);
            range_max[ev.code] = std::max(range_max[ev.code], ev.value);
        } else if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    // Show all detected movement for debugging
    std::cout << "  Movement detected:\n";
    for (const auto& [code, delta] : delta_sum) {
        if (delta > 0) {
            const char* axis_name_str = libevdev_event_code_get_name(EV_ABS, code);
            std::cout << "    Axis " << code << " (" << (axis_name_str ? axis_name_str : "UNKNOWN") 
                     << "): " << delta << " units\n";
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
    
    // Require minimum movement
    if (max_delta < MIN_MOVEMENT) {
        std::cout << "  WARNING: Movement too small (" << max_delta << " < " << MIN_MOVEMENT << ")\n";
        return {-1, std::nullopt};
    }
    
    std::cout << "Detected axis code: " << best_code << "\n";
    
    // Calculate center value from initial samples
    int center_value = baseline_values[best_code];
    int center_min = center_value;
    int center_max = center_value;
    
    if (!center_samples[best_code].empty()) {
        long long sum = 0;
        for (int val : center_samples[best_code]) {
            sum += val;
            center_min = std::min(center_min, val);
            center_max = std::max(center_max, val);
        }
        center_value = sum / center_samples[best_code].size();
    }
    
    int deadzone_radius = 10;  // Fixed deadzone for centered axes
    
    // Use the captured min/max from the detection phase
    int obs_min = range_min[best_code];
    int obs_max = range_max[best_code];
    
    std::cout << "  Observed MIN: " << obs_min << "\n";
    std::cout << "  Observed MAX: " << obs_max << "\n";
    std::cout << "  Center value: " << center_value << "\n";
    std::cout << "  Deadzone radius: " << deadzone_radius << "\n";
    
    // Validate range
    if (obs_max - obs_min < 100) {
        std::cout << "  ERROR: Range too small (" << (obs_max - obs_min) << " units). Did you move through full range?\n";
        return {best_code, std::nullopt};
    }
    
    // Sanity check center value
    int expected_center = (obs_min + obs_max) / 2;
    int center_error = std::abs(center_value - expected_center);
    int range = obs_max - obs_min;
    
    if (range > 0 && center_error > range * 0.05) {
        std::cout << "\n  ⚠ WARNING: Center (" << center_value 
                  << ") is " << center_error << " units off from expected midpoint (" 
                  << expected_center << ")\n";
        std::cout << "  This suggests the axis wasn't centered at the start.\n";
        
        if (offer_midpoint) {
            std::cout << "\n  Options:\n";
            std::cout << "    [a] Accept measured center (" << center_value << ")\n";
            std::cout << "    [m] Use calculated midpoint (" << expected_center << ")\n";
            std::cout << "    [r] Retry calibration (default)\n";
            std::cout << "  Choice (a/m/r, default r): ";
            std::string accept_input;
            std::getline(std::cin, accept_input);
            if (!accept_input.empty() && (accept_input[0] == 'a' || accept_input[0] == 'A')) {
                // Accept measured center - continue with current center_value
            } else if (!accept_input.empty() && (accept_input[0] == 'm' || accept_input[0] == 'M')) {
                // Use calculated midpoint
                center_value = expected_center;
                std::cout << "  Using calculated midpoint: " << center_value << "\n";
            } else {
                // Default: retry - return -1 to signal retry needed
                return {-1, std::nullopt};
            }
        } else {
            std::cout << "\n  Retry calibration? (y/n, default y): ";
            std::string accept_input;
            std::getline(std::cin, accept_input);
            bool retry = accept_input.empty() || !(accept_input[0] == 'n' || accept_input[0] == 'N');
            std::cout << "  → " << (retry ? "Yes" : "No") << "\n";
            if (!retry) {
                // Accept measured center despite warning
            } else {
                // Default: retry - return -1 to signal retry needed
                return {-1, std::nullopt};
            }
        }
    }
    
    AxisCalibration cal;
    cal.src_code = best_code;
    cal.observed_min = obs_min;
    cal.observed_max = obs_max;
    cal.center_value = center_value;
    cal.deadzone_radius = deadzone_radius;
    
    return {best_code, cal};
}

std::optional<AxisCalibration> calibrate_throttle_axis(DeviceInfo& device, int axis_code, const std::string& axis_name) {
    struct input_event ev;
    
    // Drain pending events first
    while (libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS) {
        // Just drain the queue
    }
    
    std::cout << "  Calibrating axis " << axis_code << " (" << axis_name << ")\n";
    std::cout << "  Move " << axis_name << " through FULL range (0% to 100%) for 10 seconds...\n";
    std::cout << "  Press ENTER when ready...";
    get_line_input();
    
    int range_min = INT_MAX;
    int range_max = INT_MIN;
    
    // Get current value as starting point
    const struct input_absinfo* absinfo = libevdev_get_abs_info(device.dev, axis_code);
    if (absinfo) {
        range_min = absinfo->value;
        range_max = absinfo->value;
    }
    
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(10000)) {
        int rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_ABS && ev.code == axis_code) {
            range_min = std::min(range_min, ev.value);
            range_max = std::max(range_max, ev.value);
        } else if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    // Show results to user
    std::cout << "  Observed MIN (0%): " << range_min << "\n";
    std::cout << "  Observed MAX (100%): " << range_max << "\n";
    
    // Validate range
    if (range_max - range_min < 100) {
        std::cout << "  ERROR: Range too small (" << (range_max - range_min) << " units). Did you move the throttle?\n";
        return std::nullopt;
    }
    
    // For throttle: center is at min position, no deadzone needed
    AxisCalibration cal;
    cal.src_code = axis_code;
    cal.observed_min = range_min;
    cal.observed_max = range_max;
    cal.center_value = range_min;  // Throttle "center" is at minimum position
    cal.deadzone_radius = 0;       // No deadzone for throttle
    
    return cal;
}

std::pair<std::optional<AxisCalibration>, std::optional<AxisCalibration>> calibrate_two_axes(
    DeviceInfo& device, int axis1_code, int axis2_code, const std::string& description) {
    
    struct input_event ev;
    
    // Drain pending events first
    while (libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS) {
        // Just drain the queue
    }
    
    std::cout << "  Calibrating axes " << axis1_code << " and " << axis2_code << " (" << description << ")\n";
    
    // Step 1: Measure center position and deadzone for both axes
    std::cout << "  Step 1: Leave stick centered and don't touch it for 5 seconds...\n";
    std::cout << "  Press ENTER when ready...";
    get_line_input();
    
    std::vector<int> center_samples1, center_samples2;
    int center_min1 = INT_MAX, center_max1 = INT_MIN;
    int center_min2 = INT_MAX, center_max2 = INT_MIN;
    
    // Get initial values
    const struct input_absinfo* absinfo1 = libevdev_get_abs_info(device.dev, axis1_code);
    const struct input_absinfo* absinfo2 = libevdev_get_abs_info(device.dev, axis2_code);
    if (absinfo1) {
        center_samples1.push_back(absinfo1->value);
        center_min1 = absinfo1->value;
        center_max1 = absinfo1->value;
    }
    if (absinfo2) {
        center_samples2.push_back(absinfo2->value);
        center_min2 = absinfo2->value;
        center_max2 = absinfo2->value;
    }
    
    auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(5000)) {
        int rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_ABS) {
            if (ev.code == axis1_code) {
                center_samples1.push_back(ev.value);
                center_min1 = std::min(center_min1, ev.value);
                center_max1 = std::max(center_max1, ev.value);
            } else if (ev.code == axis2_code) {
                center_samples2.push_back(ev.value);
                center_min2 = std::min(center_min2, ev.value);
                center_max2 = std::max(center_max2, ev.value);
            }
        } else if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    // Calculate center values and deadzones
    long long sum1 = 0, sum2 = 0;
    for (int val : center_samples1) sum1 += val;
    for (int val : center_samples2) sum2 += val;
    
    int center_value1 = center_samples1.empty() ? 0 : sum1 / center_samples1.size();
    int center_value2 = center_samples2.empty() ? 0 : sum2 / center_samples2.size();
    int deadzone_radius1 = (center_max1 - center_min1) / 2 + 10;
    int deadzone_radius2 = (center_max2 - center_min2) / 2 + 10;
    
    // Step 2: Measure full range for both axes
    std::cout << "  Step 2: Move stick in full circles for 10 seconds...\n";
    std::cout << "  Press ENTER when ready...";
    get_line_input();
    
    // Initialize range with center values, not current position
    // This prevents contamination if stick isn't centered when ENTER is pressed
    int range_min1 = center_value1, range_max1 = center_value1;
    int range_min2 = center_value2, range_max2 = center_value2;
    
    start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(10000)) {
        int rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_ABS) {
            if (ev.code == axis1_code) {
                range_min1 = std::min(range_min1, ev.value);
                range_max1 = std::max(range_max1, ev.value);
            } else if (ev.code == axis2_code) {
                range_min2 = std::min(range_min2, ev.value);
                range_max2 = std::max(range_max2, ev.value);
            }
        } else if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    // Show results for axis 1
    std::cout << "  Axis " << axis1_code << ":\n";
    std::cout << "    Observed MIN: " << range_min1 << "\n";
    std::cout << "    Observed MAX: " << range_max1 << "\n";
    std::cout << "    Center value: " << center_value1 << "\n";
    std::cout << "    Deadzone radius: " << deadzone_radius1 << "\n";
    
    // Show results for axis 2
    std::cout << "  Axis " << axis2_code << ":\n";
    std::cout << "    Observed MIN: " << range_min2 << "\n";
    std::cout << "    Observed MAX: " << range_max2 << "\n";
    std::cout << "    Center value: " << center_value2 << "\n";
    std::cout << "    Deadzone radius: " << deadzone_radius2 << "\n";
    
    // Sanity check: warn if center is way off from expected midpoint
    int expected_center1 = (range_min1 + range_max1) / 2;
    int expected_center2 = (range_min2 + range_max2) / 2;
    int center_error1 = std::abs(center_value1 - expected_center1);
    int center_error2 = std::abs(center_value2 - expected_center2);
    int range1 = range_max1 - range_min1;
    int range2 = range_max2 - range_min2;
    
    // Warn if center is off by more than 5% of the total range
    bool center_warning = false;
    if (range1 > 0 && center_error1 > range1 * 0.05) {
        std::cout << "\n  ⚠ WARNING: Axis " << axis1_code << " center (" << center_value1 
                  << ") is " << center_error1 << " units off from expected midpoint (" 
                  << expected_center1 << ")\n";
        std::cout << "  This suggests the stick wasn't centered during Step 1.\n";
        center_warning = true;
    }
    if (range2 > 0 && center_error2 > range2 * 0.05) {
        std::cout << "\n  ⚠ WARNING: Axis " << axis2_code << " center (" << center_value2 
                  << ") is " << center_error2 << " units off from expected midpoint (" 
                  << expected_center2 << ")\n";
        std::cout << "  This suggests the stick wasn't centered during Step 1.\n";
        center_warning = true;
    }
    
    if (center_warning) {
        std::cout << "\n  Retry calibration? (y/n, default y): ";
        std::string retry_input = get_line_input();
        bool retry = retry_input.empty() || retry_input[0] == 'y' || retry_input[0] == 'Y';
        std::cout << "  → " << (retry ? "Yes" : "No") << "\n";
        if (retry) {
            std::cout << "  Retrying calibration...\n\n";
            return calibrate_two_axes(device, axis1_code, axis2_code, description);
        }
        // If 'n', accept measured centers and continue
    } else {
        // Ask if user wants to retry
        std::cout << "\n  Accept these calibration values? (y/n, default y): ";
        std::string retry_input = get_line_input();
        bool accept = retry_input.empty() || !(retry_input[0] == 'n' || retry_input[0] == 'N');
        std::cout << "  → " << (accept ? "Yes" : "No") << "\n";
        if (!accept) {
            std::cout << "  Retrying calibration...\n\n";
            return calibrate_two_axes(device, axis1_code, axis2_code, description);
        }
    }
    
    // Validate ranges
    std::optional<AxisCalibration> cal1, cal2;
    
    if (range_max1 - range_min1 >= 100) {
        AxisCalibration c1;
        c1.src_code = axis1_code;
        c1.observed_min = range_min1;
        c1.observed_max = range_max1;
        c1.center_value = center_value1;
        c1.deadzone_radius = deadzone_radius1;
        cal1 = c1;
    } else {
        std::cout << "  ERROR: Axis " << axis1_code << " range too small (" << (range_max1 - range_min1) << " units)\n";
    }
    
    if (range_max2 - range_min2 >= 100) {
        AxisCalibration c2;
        c2.src_code = axis2_code;
        c2.observed_min = range_min2;
        c2.observed_max = range_max2;
        c2.center_value = center_value2;
        c2.deadzone_radius = deadzone_radius2;
        cal2 = c2;
    } else {
        std::cout << "  ERROR: Axis " << axis2_code << " range too small (" << (range_max2 - range_min2) << " units)\n";
    }
    
    return {cal1, cal2};
}

bool get_invert_preference(const std::string& axis_name) {
    std::cout << "Invert " << axis_name << "? (y/n, default n): ";
    std::string input = get_line_input();
    bool invert = (!input.empty() && (input[0] == 'y' || input[0] == 'Y'));
    std::cout << "  → " << (invert ? "Yes" : "No") << "\n";
    return invert;
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
    
    // Preflight EVIOCGRAB check for required devices
    std::string grab_err;
    if (!check_fd_not_grabbed(stick->fd, stick->path, grab_err)) {
        state.abort = true;
        state.abort_reason = "=== EVIOCGRAB CONFLICT ===\n" + grab_err + "\n\nRecovery: Run 'make stop' then rerun 'make setup'\n=========================";
        return;
    }
    
    // Check throttle device if available
    for (auto& device : state.devices) {
        if (device.role == "throttle") {
            if (!check_fd_not_grabbed(device.fd, device.path, grab_err)) {
                state.abort = true;
                state.abort_reason = "=== EVIOCGRAB CONFLICT ===\n" + grab_err + "\n\nRecovery: Run 'make stop' then rerun 'make setup'\n=========================";
                return;
            }
            break;
        }
    }
    
    // Check rudder device if available
    for (auto& device : state.devices) {
        if (device.role == "rudder") {
            if (!check_fd_not_grabbed(device.fd, device.path, grab_err)) {
                state.abort = true;
                state.abort_reason = "=== EVIOCGRAB CONFLICT ===\n" + grab_err + "\n\nRecovery: Run 'make stop' then rerun 'make setup'\n=========================";
                return;
            }
            break;
        }
    }
    
    // Capture CYCLIC X and CYCLIC Y together in one step
    int cyclic_x_code = -1;
    int cyclic_y_code = -1;
    bool cyclics_captured = false;
    
    while (!cyclics_captured) {
        std::cout << "A+B) CYCLIC X & Y (right stick both axes)\n";
        std::cout << "Press ENTER and move stick left/right...";
        get_line_input();
        
        cyclic_x_code = detect_axis(*stick, "CYCLIC X (stick left/right)");
        if (cyclic_x_code >= 0) {
            std::cout << "Detected CYCLIC X axis code: " << cyclic_x_code << "\n";
            
            // Automatically determine the other axis for CYCLIC Y
            // For a 2-axis stick: if X is axis 0 (ABS_X), Y is axis 1 (ABS_Y), and vice versa
            if (cyclic_x_code == ABS_X) {
                cyclic_y_code = ABS_Y;
            } else if (cyclic_x_code == ABS_Y) {
                cyclic_y_code = ABS_X;
            } else {
                std::cout << "WARNING: Unexpected axis code " << cyclic_x_code << ", cannot auto-determine Y axis\n";
                cyclic_y_code = -1;
            }
            
            if (cyclic_y_code >= 0) {
                std::cout << "Auto-detected CYCLIC Y axis code: " << cyclic_y_code << " (complement of CYCLIC X)\n\n";
                
                // Calibrate both axes together
                auto [cyclic_x_cal, cyclic_y_cal] = calibrate_two_axes(*stick, cyclic_x_code, cyclic_y_code, "cyclic stick");
                
                if (cyclic_x_cal.has_value() && cyclic_y_cal.has_value()) {
                    // Capture CYCLIC X
                    bool invert_x = get_invert_preference("Cyclic X");
                    CapturedAxis axis_x;
                    axis_x.role = "stick";
                    axis_x.src = cyclic_x_cal->src_code;
                    axis_x.dst = ABS_RX;
                    axis_x.invert = invert_x;
                    axis_x.deadzone = cyclic_x_cal->deadzone_radius;
                    axis_x.scale = 1.0f;
                    axis_x.calibration = *cyclic_x_cal;
                    state.captured_axes.push_back(axis_x);
                    std::cout << "Captured CYCLIC X -> virtual ABS_RX(3) invert=" << invert_x << "\n";
                    
                    // Capture CYCLIC Y
                    bool invert_y = get_invert_preference("Cyclic Y");
                    CapturedAxis axis_y;
                    axis_y.role = "stick";
                    axis_y.src = cyclic_y_cal->src_code;
                    axis_y.dst = ABS_RY;
                    axis_y.invert = invert_y;
                    axis_y.deadzone = cyclic_y_cal->deadzone_radius;
                    axis_y.scale = 1.0f;
                    axis_y.calibration = *cyclic_y_cal;
                    state.captured_axes.push_back(axis_y);
                    std::cout << "Captured CYCLIC Y -> virtual ABS_RY(4) invert=" << invert_y << "\n\n";
                    
                    cyclics_captured = true;
                } else {
                    std::cout << "Failed to calibrate cyclic axes\n";
                    std::cout << "Skip or restart? (s/r, default r): ";
                    std::string input = get_line_input();
                    bool skip = !input.empty() && (input[0] == 's' || input[0] == 'S');
                    std::cout << "  → " << (skip ? "Skip" : "Restart") << "\n";
                    if (skip) {
                        std::cout << "Skipping cyclic axes\n\n";
                        cyclics_captured = true;
                    } else {
                        std::cout << "Restarting cyclic capture...\n\n";
                        continue;
                    }
                }
            } else {
                std::cout << "Failed to auto-determine Y axis. Skipping cyclic axes\n\n";
                cyclics_captured = true;
            }
        } else {
            std::cout << "No movement detected. Skipping cyclic axes\n";
            std::cout << "Skip or restart? (s/r, default r): ";
            std::string input = get_line_input();
            bool skip = !input.empty() && (input[0] == 's' || input[0] == 'S');
            std::cout << "  → " << (skip ? "Skip" : "Restart") << "\n";
            if (skip) {
                std::cout << "Skipping cyclic axes\n\n";
                cyclics_captured = true;
            } else {
                std::cout << "Restarting cyclic capture...\n\n";
                continue;
            }
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
        bool collective_captured = false;
        while (!collective_captured) {
            std::cout << "C) COLLECTIVE (throttle/collective)\n";
            std::cout << "Press ENTER and move throttle through full range...";
            get_line_input();
            
            auto [collective_code, collective_cal] = detect_and_calibrate_throttle_axis(*throttle, "COLLECTIVE (throttle)", 6000);
            if (collective_code >= 0) {
                if (collective_cal.has_value()) {
                    bool invert = get_invert_preference("Collective");
                    CapturedAxis axis;
                    axis.role = "throttle";
                    axis.src = collective_cal->src_code;
                    axis.dst = ABS_Y;
                    axis.invert = invert;
                    axis.deadzone = 0;  // No deadzone for throttle
                    axis.scale = 1.0f;
                    axis.calibration = *collective_cal;
                    state.captured_axes.push_back(axis);
                    std::cout << "Captured COLLECTIVE -> virtual ABS_Y(1) invert=" << invert << "\n\n";
                    collective_captured = true;
                } else {
                    std::cout << "Failed to calibrate COLLECTIVE\n";
                    std::cout << "Skip or restart? (s/r, default r): ";
                    std::string input = get_line_input();
                    bool skip = !input.empty() && (input[0] == 's' || input[0] == 'S');
                    std::cout << "  → " << (skip ? "Skip" : "Restart") << "\n";
                    if (skip) {
                        std::cout << "Skipping COLLECTIVE\n\n";
                        collective_captured = true;
                    } else {
                        std::cout << "Restarting COLLECTIVE capture...\n\n";
                        continue;
                    }
                }
            } else {
                std::cout << "No movement detected. Skipping COLLECTIVE\n";
                std::cout << "Skip or restart? (s/r, default r): ";
                std::string input = get_line_input();
                bool skip = !input.empty() && (input[0] == 's' || input[0] == 'S');
                std::cout << "  → " << (skip ? "Skip" : "Restart") << "\n";
                if (skip) {
                    std::cout << "Skipping COLLECTIVE\n\n";
                    collective_captured = true;
                } else {
                    std::cout << "Restarting COLLECTIVE capture...\n\n";
                    continue;
                }
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
        bool antitorque_captured = false;
        while (!antitorque_captured) {
            std::cout << "D) ANTI-TORQUE (rudder pedals)\n";
            std::cout << "⚠ IMPORTANT: Keep rudder pedals COMPLETELY CENTERED!\n";
            std::cout << "⚠ Do NOT touch the brake pedals during calibration - only move the rudder left/right!\n\n";
            
            // Step 1: Detect axis and measure full range
            std::cout << "Step 1: Detecting rudder axis and measuring range\n";
            std::cout << "Press ENTER, then move rudder pedals through FULL range (left and right)...";
            get_line_input();
            
            auto [antitorque_code, temp_cal] = detect_and_calibrate_throttle_axis(*rudder, "ANTI-TORQUE", 6000);
            
            if (antitorque_code < 0) {
                std::cout << "No movement detected. Skipping ANTI-TORQUE\n";
                std::cout << "Skip or restart? (s/r, default r): ";
                std::string input = get_line_input();
                bool skip = !input.empty() && (input[0] == 's' || input[0] == 'S');
                std::cout << "  → " << (skip ? "Skip" : "Restart") << "\n";
                if (skip) {
                    std::cout << "Skipping ANTI-TORQUE\n\n";
                    antitorque_captured = true;
                } else {
                    std::cout << "Restarting ANTI-TORQUE capture...\n\n";
                    continue;
                }
                continue;
            }
            
            if (!temp_cal.has_value()) {
                std::cout << "Failed to detect axis. Restarting...\n\n";
                continue;
            }
            
            std::cout << "\nDetected axis: " << antitorque_code << "\n";
            std::cout << "Observed MIN: " << temp_cal->observed_min << "\n";
            std::cout << "Observed MAX: " << temp_cal->observed_max << "\n";
            
            // Step 2: Measure center with known axis
            std::cout << "\nStep 2: Measuring center position\n";
            std::cout << "CENTER the rudder pedals and hold still for 5 seconds...\n";
            std::cout << "Press ENTER when ready...";
            get_line_input();
            
            // Sample center for 5 seconds
            struct input_event ev;
            while (libevdev_next_event(rudder->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS) {
                // Drain queue
            }
            
            std::vector<int> center_samples;
            auto start = std::chrono::steady_clock::now();
            while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(5000)) {
                int rc = libevdev_next_event(rudder->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
                if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_ABS && ev.code == antitorque_code) {
                    center_samples.push_back(ev.value);
                } else if (rc == -EAGAIN) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
            
            int center_value = temp_cal->center_value;
            if (!center_samples.empty()) {
                long long sum = 0;
                for (int val : center_samples) {
                    sum += val;
                }
                center_value = sum / center_samples.size();
            }
            
            std::cout << "Center value: " << center_value << "\n";
            std::cout << "\nAccept these values? (y/n, default y): ";
            std::string accept = get_line_input();
            bool accept_ok = accept.empty() || !(accept[0] == 'n' || accept[0] == 'N');
            std::cout << "  → " << (accept_ok ? "Yes" : "No") << "\n";
            
            if (!accept_ok) {
                std::cout << "Restarting rudder calibration...\n\n";
                continue;
            }
            
            // Build final calibration
            AxisCalibration antitorque_cal = *temp_cal;
            antitorque_cal.center_value = center_value;
            antitorque_cal.deadzone_radius = 10;
            
            bool invert = get_invert_preference("Anti-torque");
            CapturedAxis axis;
            axis.role = "rudder";
            axis.src = antitorque_cal.src_code;
            axis.dst = ABS_X;
            axis.invert = invert;
            axis.deadzone = antitorque_cal.deadzone_radius;
            axis.scale = 1.0f;
            axis.calibration = antitorque_cal;
            state.captured_axes.push_back(axis);
            std::cout << "Captured ANTI-TORQUE -> virtual ABS_X(0) invert=" << invert << "\n\n";
            antitorque_captured = true;
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
                        
                        // Drain all pending events from all devices before moving to next button
                        for (auto& dev : state.devices) {
                            struct input_event drain_ev;
                            while (libevdev_next_event(dev.dev, LIBEVDEV_READ_FLAG_NORMAL, &drain_ev) == LIBEVDEV_READ_STATUS_SUCCESS) {
                                // Just drain
                            }
                        }
                        
                        set_nonblocking(false);
                        goto next_button;
                    }
                }
                
                // Listen to all devices for button press - check ALL devices, don't break early
                bool new_detection = false;
                for (auto& device : state.devices) {
                    struct input_event ev;
                    int rc;
                    
                    // Drain all events from this device to ensure we don't miss anything
                    while ((rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) == LIBEVDEV_READ_STATUS_SUCCESS) {
                        if (ev.type == EV_KEY && ev.value == 1) {
                            // Clear the "Waiting for input..." line
                            std::cout << "\r" << std::string(50, ' ') << "\r";
                            std::cout << "Detected: " << device.role << " " << ev.code << "\n";
                            std::cout << "Press ENTER to accept, or press another button to override\n";
                            std::cout.flush();
                            
                            captured_device = device.role;
                            captured_code = ev.code;
                            has_detection = true;
                            new_detection = true;
                        }
                    }
                    
                    // Handle SYN_DROPPED to resync device state
                    if (rc == LIBEVDEV_READ_STATUS_SYNC) {
                        while (libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_SYNC, &ev) == LIBEVDEV_READ_STATUS_SYNC) {
                            // Resync
                        }
                    }
                }
                
                if (restart_phase) break;
                if (new_detection) continue; // Don't sleep if we just detected something
                
                // Very short sleep to reduce CPU usage but minimize missed events
                std::this_thread::sleep_for(std::chrono::microseconds(100));
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
        
        // Add calibrations for this device from captured axes
        for (const auto& axis : state.captured_axes) {
            if (axis.role == device.role) {
                input.calibrations.push_back(axis.calibration);
            }
        }
        
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
    
    std::cout << "\nDetected " << all_devices.size() << " device(s):\n";
    for (size_t i = 0; i < all_devices.size(); i++) {
        const auto& device = all_devices[i];
        const char* name = libevdev_get_name(device.dev);
        std::cout << "  [" << i << "] " << device.by_id;
        if (name) {
            std::cout << " (" << name << ")";
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
    std::set<std::string> selected_paths;
    for (const auto& [role, device] : selected_devices) {
        selected_paths.insert(device.path);
    }
    
    for (auto& device : all_devices) {
        if (selected_paths.find(device.path) == selected_paths.end()) {
            std::cout << "Closing unselected: " << device.by_id << "\n";
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
    
    if (state.abort) {
        std::cerr << state.abort_reason << "\n";
        return 1;
    }
    
    // Phase 3: Button Capture
    capture_buttons(state);
    
    if (state.abort) {
        std::cerr << state.abort_reason << "\n";
        return 1;
    }
    
    // Phase 4: Confirmation with redo loop
    while (true) {
        print_summary(state);
        
        std::cout << "\nPress ENTER to accept, or wait 10 seconds to accept automatically.\n";
        std::cout << "Press 'r' to redo capture.\n";
        
        set_nonblocking(true);
        char c = get_key_with_timeout(10000); // 5 second timeout
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
            
            // Clear only button captures (keep axes)
            state.captured_buttons.clear();
            
            // Redo only button capture
            capture_buttons(state);
            if (state.abort) {
                std::cerr << state.abort_reason << "\n";
                return 1;
            }
            
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