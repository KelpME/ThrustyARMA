#include "config.hpp"
#include "bindings.hpp"
#include <linux/input.h>
#include <linux/input-event-codes.h>
#include <linux/uinput.h>
#include <libevdev-1.0/libevdev/libevdev.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <sys/ioctl.h>
#include <sys/epoll.h>
#include <iostream>
#include <array>
#include <sstream>
#include <fstream>
#include <limits.h>
#include <map>
#include <vector>
#include <set>
#include <chrono>
#include <thread>

static volatile sig_atomic_t running = 1;

void signal_handler(int sig) {
    running = 0;
}

// Device mapping structures
struct InputDevice {
    std::string role;
    int fd;
    struct libevdev* dev;
    std::string path;
    std::string by_id_path;
    std::string vendor;
    std::string product;
    bool optional;
    bool online;
    int consecutive_read_failures;
    std::chrono::steady_clock::time_point last_reconnect_attempt;
    int reconnect_backoff_ms;
};

Role string_to_role(const std::string& role_str) {
    if (role_str == "stick") return Role::Stick;
    if (role_str == "throttle") return Role::Throttle;
    if (role_str == "rudder") return Role::Rudder;
    return Role::Stick; // fallback
}

std::string get_udev_property(const std::string& device_path, const std::string& property) {
    std::string cmd = "udevadm info -q property -n " + device_path + " 2>/dev/null";
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
        return "";
    }
    
    std::string output;
    std::array<char, 128> buffer;
    while (fgets(buffer.data(), buffer.size(), pipe) != nullptr) {
        output += buffer.data();
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

bool validate_device(const std::string& device_path, const std::string& expected_vendor, const std::string& expected_product) {
    std::string vendor_id = get_udev_property(device_path, "ID_VENDOR_ID");
    std::string product_id = get_udev_property(device_path, "ID_MODEL_ID");
    
    return (vendor_id == expected_vendor && product_id == expected_product);
}

bool reopen_device(InputDevice& device) {
    // Close existing fd if open
    if (device.fd >= 0) {
        if (device.dev) {
            libevdev_free(device.dev);
            device.dev = nullptr;
        }
        close(device.fd);
        device.fd = -1;
    }
    
    // Try to resolve the by-id path
    char real_path[PATH_MAX];
    if (realpath(device.by_id_path.c_str(), real_path) == nullptr) {
        return false;
    }
    
    // Try to open the device
    int fd = open(real_path, O_RDONLY | O_NONBLOCK);
    if (fd < 0) {
        return false;
    }
    
    // Initialize libevdev
    struct libevdev* dev = nullptr;
    int rc = libevdev_new_from_fd(fd, &dev);
    if (rc < 0) {
        close(fd);
        return false;
    }
    
    // Validate device
    if (!validate_device(real_path, device.vendor, device.product)) {
        libevdev_free(dev);
        close(fd);
        return false;
    }
    
    // Update device state
    device.fd = fd;
    device.dev = dev;
    device.path = real_path;
    device.online = true;
    device.consecutive_read_failures = 0;
    device.reconnect_backoff_ms = 500; // Reset to initial backoff
    
    std::cout << "Successfully reconnected " << device.role << ": " << real_path << "\n";
    return true;
}

void handle_device_error(InputDevice& device) {
    device.consecutive_read_failures++;
    
    // Mark offline if we get specific errors or repeated failures
    if (errno == ENODEV || errno == EIO || device.consecutive_read_failures >= 3) {
        if (device.online) {
            std::cout << device.role << " device disconnected (errno=" << errno 
                      << ", failures=" << device.consecutive_read_failures << ")\n";
            device.online = false;
        }
        
        // Close the device
        if (device.dev) {
            libevdev_free(device.dev);
            device.dev = nullptr;
        }
        if (device.fd >= 0) {
            close(device.fd);
            device.fd = -1;
        }
    }
}

void attempt_device_reconnection(InputDevice& device) {
    if (device.online || device.optional) {
        return;
    }
    
    auto now = std::chrono::steady_clock::now();
    auto time_since_attempt = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - device.last_reconnect_attempt).count();
    
    if (time_since_attempt >= device.reconnect_backoff_ms) {
        device.last_reconnect_attempt = now;
        
        if (reopen_device(device)) {
            // Successfully reconnected
            device.reconnect_backoff_ms = 500; // Reset backoff
        } else {
            // Failed to reconnect, increase backoff (max 2 seconds)
            device.reconnect_backoff_ms = std::min(2000, device.reconnect_backoff_ms * 2);
        }
    }
}

bool device_supports_code(const InputDevice& device, SrcKind kind, uint16_t code) {
    if (!device.dev) return false;
    
    if (kind == SrcKind::Key) {
        return libevdev_has_event_code(device.dev, EV_KEY, code);
    } else {
        return libevdev_has_event_code(device.dev, EV_ABS, code);
    }
}

void validate_and_filter_bindings(std::vector<Binding>& bindings, const std::vector<InputDevice>& devices) {
    std::set<std::tuple<std::string, SrcKind, uint16_t>> logged_missing_codes;
    
    for (auto it = bindings.begin(); it != bindings.end();) {
        bool binding_valid = true;
        
        // Find the source device for this binding
        const InputDevice* source_device = nullptr;
        for (const auto& device : devices) {
            if (device.role == (it->src.role == Role::Stick ? "stick" : 
                               it->src.role == Role::Throttle ? "throttle" : "rudder")) {
                source_device = &device;
                break;
            }
        }
        
        if (source_device && source_device->dev) {
            // Check if device supports the source code
            if (!device_supports_code(*source_device, it->src.kind, it->src.code)) {
                auto missing_key = std::make_tuple(source_device->role, it->src.kind, it->src.code);
                
                // Log warning once per missing code
                if (logged_missing_codes.find(missing_key) == logged_missing_codes.end()) {
                    const char* type_name = (it->src.kind == SrcKind::Key) ? "KEY" : "ABS";
                    const char* code_name = libevdev_event_code_get_name(
                        it->src.kind == SrcKind::Key ? EV_KEY : EV_ABS, it->src.code);
                    
                    std::cout << "WARNING: " << source_device->role << " device does not support " 
                              << type_name << " " << (code_name ? code_name : "UNKNOWN") 
                              << " (" << it->src.code << "). Skipping binding.\n";
                    
                    logged_missing_codes.insert(missing_key);
                }
                
                binding_valid = false;
            }
        } else {
            // Device not available - skip binding but don't log (device validation handles this)
            binding_valid = false;
        }
        
        if (!binding_valid) {
            it = bindings.erase(it);
        } else {
            ++it;
        }
    }
}

int discovery_mode(const Config& config) {
    std::vector<InputDevice> devices;
    
    // Open all configured devices
    for (const auto& input_config : config.inputs) {
        if (input_config.by_id.empty()) {
            std::cout << "Skipping " << input_config.role << " (not present)\n";
            continue;
        }
        
        char real_path[PATH_MAX];
        if (realpath(input_config.by_id.c_str(), real_path) == nullptr) {
            std::cerr << "Failed to resolve " << input_config.role << " path: " << input_config.by_id << "\n";
            continue;
        }
        
        int fd = open(real_path, O_RDONLY);
        if (fd < 0) {
            std::cerr << "Failed to open " << input_config.role << ": " << input_config.by_id << "\n";
            continue;
        }

        struct libevdev* dev = nullptr;
        int rc = libevdev_new_from_fd(fd, &dev);
        if (rc < 0) {
            std::cerr << "Failed to init " << input_config.role << "\n";
            close(fd);
            continue;
        }
        
        if (!validate_device(real_path, input_config.vendor, input_config.product)) {
            std::cerr << "Device validation failed for " << input_config.role << "\n";
            libevdev_free(dev);
            close(fd);
            continue;
        }
        
        InputDevice device;
        device.role = input_config.role;
        device.fd = fd;
        device.dev = dev;
        device.path = real_path;
        device.by_id_path = input_config.by_id;
        device.vendor = input_config.vendor;
        device.product = input_config.product;
        device.optional = input_config.optional;
        device.online = true;
        device.consecutive_read_failures = 0;
        device.reconnect_backoff_ms = 500;
        device.last_reconnect_attempt = std::chrono::steady_clock::now();
        devices.push_back(device);
        
        std::cout << "\n=== " << input_config.role << " Discovery ===\n";
        std::cout << "Device: " << input_config.by_id << "\n";
        std::cout << "Observing events for 10 seconds...\n";
        std::cout << "Move all controls:\n\n";

        std::set<std::pair<int, int>> observed_codes;
        auto start_time = std::chrono::steady_clock::now();
        int elapsed_seconds = 0;
        
        while (running && elapsed_seconds < 10) {
            struct input_event ev;
            rc = libevdev_next_event(dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            
            if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
                if (ev.type == EV_KEY || ev.type == EV_ABS) {
                    auto code_pair = std::make_pair(ev.type, ev.code);
                    if (observed_codes.insert(code_pair).second) {
                        const char* type_name = libevdev_event_type_get_name(ev.type);
                        const char* code_name = libevdev_event_code_get_name(ev.type, ev.code);
                        std::cout << "  " << (type_name ? type_name : "UNKNOWN") 
                                  << " " << (code_name ? code_name : "UNKNOWN") 
                                  << " (type=" << ev.type << ", code=" << ev.code << ")\n";
                    }
                }
            } else if (rc == -EAGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                auto now = std::chrono::steady_clock::now();
                elapsed_seconds = std::chrono::duration_cast<std::chrono::seconds>(now - start_time).count();
            } else {
                break;
            }
        }
        
        libevdev_free(dev);
        close(fd);
    }
    
    std::cout << "\nDiscovery complete.\n";
    return 0;
}

int diagnostics_mode(const Config& config) {
    std::cout << "=== TWCS Mapper Diagnostics ===\n\n";
    
    // Configuration report
    std::cout << "CONFIGURATION:\n";
    std::cout << "  uinput_name: " << config.uinput_name << "\n";
    std::cout << "  device_grab: " << (config.grab ? "enabled" : "disabled") << "\n";
    std::cout << "  configured_inputs: " << config.inputs.size() << "\n";
    
    // Device detection report
    std::cout << "\nDEVICE DETECTION:\n";
    int detected_devices = 0;
    int required_devices = 0;
    int failed_required = 0;
    
    for (const auto& input_config : config.inputs) {
        if (!input_config.optional) {
            required_devices++;
        }
        
        std::cout << "  " << input_config.role << ":\n";
        std::cout << "    configured_path: " << input_config.by_id << "\n";
        std::cout << "    expected_vendor: " << input_config.vendor << "\n";
        std::cout << "    expected_product: " << input_config.product << "\n";
        std::cout << "    optional: " << (input_config.optional ? "yes" : "no") << "\n";
        
        if (input_config.by_id.empty()) {
            std::cout << "    status: NOT_CONFIGURED\n";
            continue;
        }
        
        char real_path[PATH_MAX];
        if (realpath(input_config.by_id.c_str(), real_path) == nullptr) {
            std::cout << "    status: PATH_RESOLUTION_FAILED\n";
            if (!input_config.optional) {
                failed_required++;
            }
            continue;
        }
        
        std::cout << "    resolved_path: " << real_path << "\n";
        
        int fd = open(real_path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            std::cout << "    status: ACCESS_FAILED (" << strerror(errno) << ")\n";
            if (!input_config.optional) {
                failed_required++;
            }
            continue;
        }

        struct libevdev* dev = nullptr;
        int rc = libevdev_new_from_fd(fd, &dev);
        if (rc < 0) {
            std::cout << "    status: INITIALIZATION_FAILED (" << strerror(-rc) << ")\n";
            close(fd);
            if (!input_config.optional) {
                failed_required++;
            }
            continue;
        }
        
        // Get actual device properties
        std::string actual_vendor = get_udev_property(real_path, "ID_VENDOR_ID");
        std::string actual_product = get_udev_property(real_path, "ID_MODEL_ID");
        const char* device_name = libevdev_get_name(dev);
        
        std::cout << "    device_name: " << (device_name ? device_name : "UNKNOWN") << "\n";
        std::cout << "    actual_vendor: " << actual_vendor << "\n";
        std::cout << "    actual_product: " << actual_product << "\n";
        
        bool validation_ok = validate_device(real_path, input_config.vendor, input_config.product);
        if (validation_ok) {
            std::cout << "    status: DETECTED_OK\n";
            detected_devices++;
        } else {
            std::cout << "    status: VALIDATION_FAILED\n";
            if (!input_config.optional) {
                failed_required++;
            }
        }
        
        libevdev_free(dev);
        close(fd);
    }
    
    std::cout << "\n  Summary: " << detected_devices << "/" << config.inputs.size() << " devices detected";
    if (failed_required > 0) {
        std::cout << " (" << failed_required << " required devices missing)";
    }
    std::cout << "\n";
    
    // Bindings report
    std::cout << "\nBINDINGS:\n";
    
    std::vector<Binding> bindings;
    if (!config.bindings_keys.empty() || !config.bindings_abs.empty()) {
        auto config_bindings = make_bindings_from_config(config.bindings_keys, config.bindings_abs);
        std::vector<Binding> valid_config_bindings;
        for (const auto& binding : config_bindings) {
            if (validate_bindings({binding})) {
                valid_config_bindings.push_back(binding);
            }
        }
        
        // Create temporary devices for validation (without opening them)
        std::vector<InputDevice> temp_devices;
        for (const auto& input_config : config.inputs) {
            InputDevice device;
            device.role = input_config.role;
            device.by_id_path = input_config.by_id;
            device.vendor = input_config.vendor;
            device.product = input_config.product;
            device.optional = input_config.optional;
            device.online = false;
            device.fd = -1;
            device.dev = nullptr;
            temp_devices.push_back(device);
        }
        
        // Validate source codes and filter bindings
        validate_and_filter_bindings(valid_config_bindings, temp_devices);
        
        if (!valid_config_bindings.empty()) {
            bindings = valid_config_bindings;
            std::cout << "  config_bindings: " << bindings.size() << " loaded from config\n";
        } else {
            bindings = make_default_bindings();
            std::cout << "  config_bindings: ALL INVALID (using defaults)\n";
        }
    } else {
        bindings = make_default_bindings();
        std::cout << "  config_bindings: none configured (using defaults)\n";
    }
    
    std::cout << "  active_bindings: " << bindings.size() << "\n";
    
    // Group bindings by type for clearer reporting
    std::map<std::string, std::vector<const Binding*>> role_bindings;
    for (const auto& binding : bindings) {
        std::string role_name;
        switch (binding.src.role) {
            case Role::Stick: role_name = "stick"; break;
            case Role::Throttle: role_name = "throttle"; break;
            case Role::Rudder: role_name = "rudder"; break;
        }
        role_bindings[role_name].push_back(&binding);
    }
    
    for (const auto& [role_name, role_binding_list] : role_bindings) {
        std::cout << "    " << role_name << " (" << role_binding_list.size() << " bindings):\n";
        for (const auto* binding : role_binding_list) {
            std::cout << "      ";
            if (binding->src.kind == SrcKind::Key) {
                const char* btn_name = libevdev_event_code_get_name(EV_KEY, binding->src.code);
                std::cout << "KEY " << (btn_name ? btn_name : "UNKNOWN") << " (" << binding->src.code << ")";
            } else {
                const char* axis_name = libevdev_event_code_get_name(EV_ABS, binding->src.code);
                std::cout << "ABS " << (axis_name ? axis_name : "UNKNOWN") << " (" << binding->src.code << ")";
            }
            
            std::cout << " -> ";
            if (binding->dst.kind == SrcKind::Key) {
                const char* btn_name = libevdev_event_code_get_name(EV_KEY, binding->dst.code);
                std::cout << "BTN " << (btn_name ? btn_name : "UNKNOWN") << " (" << binding->dst.code << ")";
            } else {
                const char* axis_name = libevdev_event_code_get_name(EV_ABS, binding->dst.code);
                std::cout << "ABS " << (axis_name ? axis_name : "UNKNOWN") << " (" << binding->dst.code << ")";
                
                if (binding->xform.invert || binding->xform.deadzone > 0 || binding->xform.scale != 1.0f) {
                    std::cout << " [";
                    if (binding->xform.invert) std::cout << "invert ";
                    if (binding->xform.deadzone > 0) std::cout << "deadzone=" << binding->xform.deadzone << " ";
                    if (binding->xform.scale != 1.0f) std::cout << "scale=" << binding->xform.scale << " ";
                    std::cout << "\b]";
                }
            }
            std::cout << "\n";
        }
    }
    
    // Service state check
    std::cout << "\nSERVICE STATE:\n";
    
    // Check if service file exists
    std::string service_path = std::string(getenv("HOME")) + "/.config/systemd/user/twcs-mapper.service";
    std::ifstream service_file(service_path);
    if (service_file.good()) {
        std::cout << "  service_file: EXISTS (" << service_path << ")\n";
        
        // Try to get service status via systemctl
        FILE* pipe = popen("systemctl --user is-active twcs-mapper.service 2>/dev/null", "r");
        if (pipe) {
            char buffer[128];
            std::string result;
            if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                result = buffer;
                result.erase(result.find_last_not_of("\n\r") + 1);
                std::cout << "  service_status: " << result << "\n";
            } else {
                std::cout << "  service_status: UNKNOWN\n";
            }
            pclose(pipe);
        }
        
        // Check if service is enabled
        pipe = popen("systemctl --user is-enabled twcs-mapper.service 2>/dev/null", "r");
        if (pipe) {
            char buffer[128];
            std::string result;
            if (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
                result = buffer;
                result.erase(result.find_last_not_of("\n\r") + 1);
                std::cout << "  service_enabled: " << result << "\n";
            } else {
                std::cout << "  service_enabled: UNKNOWN\n";
            }
            pclose(pipe);
        }
    } else {
        std::cout << "  service_file: NOT_FOUND\n";
    }
    
    // uinput availability check
    std::cout << "\nSYSTEM CHECKS:\n";
    int uinput_check = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (uinput_check >= 0) {
        std::cout << "  /dev/uinput: ACCESSIBLE\n";
        close(uinput_check);
    } else {
        std::cout << "  /dev/uinput: NOT_ACCESSIBLE (" << strerror(errno) << ")\n";
    }
    
    // Check user in input group
    std::cout << "  user_groups: ";
    FILE* groups_pipe = popen("groups", "r");
    if (groups_pipe) {
        char buffer[256];
        if (fgets(buffer, sizeof(buffer), groups_pipe) != nullptr) {
            std::string groups = buffer;
            groups.erase(groups.find_last_not_of("\n\r") + 1);
            std::cout << groups;
            if (groups.find("input") != std::string::npos) {
                std::cout << " (input group present)";
            } else {
                std::cout << " (input group missing - may affect device access)";
            }
        } else {
            std::cout << "UNKNOWN";
        }
        pclose(groups_pipe);
    }
    std::cout << "\n";
    
    // Overall health summary
    std::cout << "\nHEALTH SUMMARY:\n";
    bool overall_healthy = true;
    
    if (detected_devices < required_devices) {
        std::cout << "  ERROR: Required devices missing\n";
        overall_healthy = false;
    }
    
    if (bindings.empty()) {
        std::cout << "  ERROR: No active bindings\n";
        overall_healthy = false;
    }
    
    if (uinput_check < 0) {
        std::cout << "  ERROR: Cannot access /dev/uinput\n";
        overall_healthy = false;
    }
    
    if (overall_healthy) {
        std::cout << "  STATUS: HEALTHY\n";
    } else {
        std::cout << "  STATUS: ISSUES_DETECTED\n";
    }
    
    return overall_healthy ? 0 : 1;
}

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTION]\n";
    std::cout << "TWCS ARMA Mapper - Virtual controller mapping for flight controls\n\n";
    std::cout << "Options:\n";
    std::cout << "  --print-map     Interactive discovery mode to map device controls\n";
    std::cout << "  --diagnostics   Non-interactive diagnostics reporting device detection, bindings, and service state\n";
    std::cout << "  --help          Show this help message\n\n";
    std::cout << "When run without options, the mapper starts in normal mode, creating and managing the virtual controller.\n";
}

int main(int argc, char* argv[]) {
    // Check for help option first, before any other operations
    if (argc >= 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage(argv[0]);
        return 0;
    }

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    // Load config
    std::string config_dir = std::string(getenv("HOME")) + "/.config/twcs-mapper";
    std::string config_path = config_dir + "/config.json";
    
    auto config_opt = ConfigManager::load(config_path);
    if (!config_opt) {
        std::cerr << "No configuration found. Run twcs_select first.\n";
        return 1;
    }
    
    Config config = *config_opt;
    
    std::cout << "Loaded inputs: " << config.inputs.size() << "\n";
    for (const auto& in : config.inputs) {
        std::cout << "  role=" << in.role
                  << " optional=" << (in.optional ? "true" : "false")
                  << " by_id=" << in.by_id
                  << " vendor=" << in.vendor
                  << " product=" << in.product << "\n";
    }

    // Check for discovery mode
    if (argc >= 2 && strcmp(argv[1], "--print-map") == 0) {
        return discovery_mode(config);
    }

    // Check for diagnostics mode
    if (argc >= 2 && strcmp(argv[1], "--diagnostics") == 0) {
        return diagnostics_mode(config);
    }

    // Open and validate all devices
    std::vector<InputDevice> input_devices;
    
    for (const auto& input_config : config.inputs) {
        if (input_config.by_id.empty()) {
            std::cout << "Skipping " << input_config.role << " (not configured)\n";
            if (!input_config.optional) {
                std::cerr << "ERROR: Required device " << input_config.role << " is missing!\n";
                return 1;
            }
            continue;
        }
        
        char real_path[PATH_MAX];
        if (realpath(input_config.by_id.c_str(), real_path) == nullptr) {
            std::cerr << "Failed to resolve " << input_config.role << " path: " << input_config.by_id << "\n";
            if (!input_config.optional) {
                return 1;
            }
            continue;
        }
        
        int fd = open(real_path, O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            perror(("Failed to open " + input_config.role).c_str());
            if (!input_config.optional) {
                return 1;
            }
            continue;
        }

        struct libevdev* dev = nullptr;
        int rc = libevdev_new_from_fd(fd, &dev);
        if (rc < 0) {
            perror(("Failed to init " + input_config.role).c_str());
            close(fd);
            if (!input_config.optional) {
                return 1;
            }
            continue;
        }
        
        if (!validate_device(real_path, input_config.vendor, input_config.product)) {
            std::cerr << "Device validation failed for " << input_config.role << "\n";
            libevdev_free(dev);
            close(fd);
            if (!input_config.optional) {
                return 1;
            }
            continue;
        }
        
        // Grab device if configured
        if (config.grab) {
            rc = ioctl(fd, EVIOCGRAB, 1);
            if (rc < 0) {
                perror(("Failed to grab " + input_config.role).c_str());
                // Continue anyway - this might not be fatal
            } else {
                std::cout << "Grabbed " << input_config.role << ": " << real_path << "\n";
            }
        } else {
            std::cout << "Opened " << input_config.role << ": " << real_path << " (no grab)\n";
        }
        
        InputDevice device;
        device.role = input_config.role;
        device.fd = fd;
        device.dev = dev;
        device.path = real_path;
        device.by_id_path = input_config.by_id;
        device.vendor = input_config.vendor;
        device.product = input_config.product;
        device.optional = input_config.optional;
        device.online = true;
        device.consecutive_read_failures = 0;
        device.reconnect_backoff_ms = 500;
        device.last_reconnect_attempt = std::chrono::steady_clock::now();
        input_devices.push_back(device);
    }

    if (input_devices.empty()) {
        std::cerr << "No input devices available\n";
        return 1;
    }

    // Create uinput device
    int uinput_fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (uinput_fd < 0) {
        perror("Failed to open uinput device");
        for (auto& dev : input_devices) {
            libevdev_free(dev.dev);
            close(dev.fd);
        }
        return 1;
    }

    // Enable event types
    if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(uinput_fd, UI_SET_EVBIT, EV_ABS) < 0) {
        perror("Failed to enable event types");
        close(uinput_fd);
        for (auto& dev : input_devices) {
            libevdev_free(dev.dev);
            close(dev.fd);
        }
        return 1;
    }

    // Virtual Controller Contract: Fixed 11 buttons for ARMA stability
    const int virtual_buttons[] = {
        BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH,  // Face buttons
        BTN_TL, BTN_TR,                              // Shoulder buttons (no digital triggers)
        BTN_SELECT, BTN_START, BTN_MODE,             // System buttons
        BTN_THUMBL, BTN_THUMBR                       // Stick clicks
    };
    for (int btn : virtual_buttons) {
        if (ioctl(uinput_fd, UI_SET_KEYBIT, btn) < 0) {
            perror("Failed to enable virtual button");
            close(uinput_fd);
            for (auto& dev : input_devices) {
                libevdev_free(dev.dev);
                close(dev.fd);
            }
            return 1;
        }
    }

    // Virtual Controller Contract: Fixed 8 axes for ARMA stability
    const int virtual_axes[] = {
        ABS_X, ABS_Y,        // Left stick
        ABS_RX, ABS_RY,      // Right stick  
        ABS_Z, ABS_RZ,       // Analog triggers (no digital clicks)
        ABS_HAT0X, ABS_HAT0Y // D-pad hat
    };
    for (int axis : virtual_axes) {
        if (ioctl(uinput_fd, UI_SET_ABSBIT, axis) < 0) {
            perror("Failed to enable virtual axis");
            close(uinput_fd);
            for (auto& dev : input_devices) {
                libevdev_free(dev.dev);
                close(dev.fd);
            }
            return 1;
        }
    }

    // Set up uinput device for Virtual Controller (fixed contract for ARMA stability)
    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    
    strncpy(uidev.name, config.uinput_name.c_str(), UINPUT_MAX_NAME_SIZE);
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor = 0x045e;  // Microsoft
    uidev.id.product = 0x028e; // Xbox 360 Controller
    uidev.id.version = 1;

    // Virtual Controller Contract: Fixed axis ranges for ARMA stability
    uidev.absmin[ABS_X] = -32768; uidev.absmax[ABS_X] = 32767;  // Left stick X
    uidev.absmin[ABS_Y] = -32768; uidev.absmax[ABS_Y] = 32767;  // Left stick Y
    uidev.absmin[ABS_RX] = -32768; uidev.absmax[ABS_RX] = 32767; // Right stick X
    uidev.absmin[ABS_RY] = -32768; uidev.absmax[ABS_RY] = 32767; // Right stick Y
    uidev.absmin[ABS_Z] = 0; uidev.absmax[ABS_Z] = 255;          // Left trigger
    uidev.absmin[ABS_RZ] = 0; uidev.absmax[ABS_RZ] = 255;        // Right trigger
    uidev.absmin[ABS_HAT0X] = -1; uidev.absmax[ABS_HAT0X] = 1;   // D-pad X
    uidev.absmin[ABS_HAT0Y] = -1; uidev.absmax[ABS_HAT0Y] = 1;   // D-pad Y

    // Create device
    if (write(uinput_fd, &uidev, sizeof(uidev)) < 0) {
        perror("Failed to write uinput device");
        close(uinput_fd);
        for (auto& dev : input_devices) {
            libevdev_free(dev.dev);
            close(dev.fd);
        }
        return 1;
    }

    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
        perror("Failed to create uinput device");
        close(uinput_fd);
        for (auto& dev : input_devices) {
            libevdev_free(dev.dev);
            close(dev.fd);
        }
        return 1;
    }

    std::cout << "Created uinput device: " << config.uinput_name << "\n";

    // Set up epoll
    int epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("Failed to create epoll");
        ioctl(uinput_fd, UI_DEV_DESTROY);
        close(uinput_fd);
        for (auto& dev : input_devices) {
            libevdev_free(dev.dev);
            close(dev.fd);
        }
        return 1;
    }

    // Add all input devices to epoll
    for (auto& dev : input_devices) {
        struct epoll_event event;
        event.events = EPOLLIN;
        event.data.fd = dev.fd;
        if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, dev.fd, &event) < 0) {
            perror("Failed to add device to epoll");
            ioctl(uinput_fd, UI_DEV_DESTROY);
            close(uinput_fd);
            for (auto& d : input_devices) {
                libevdev_free(d.dev);
                close(d.fd);
            }
            close(epoll_fd);
            return 1;
        }
    }

    // Initialize binding resolver
    std::vector<Binding> bindings;
    
    // Try to load bindings from config, fall back to defaults if none or invalid
    if (!config.bindings_keys.empty() || !config.bindings_abs.empty()) {
        auto config_bindings = make_bindings_from_config(config.bindings_keys, config.bindings_abs);
        
        // Validate config bindings and filter out invalid ones
        std::vector<Binding> valid_config_bindings;
        for (const auto& binding : config_bindings) {
            if (validate_bindings({binding})) {
                valid_config_bindings.push_back(binding);
            } else {
                std::cout << "WARNING: Ignored invalid binding targeting virtual controller contract violation\n";
            }
        }
        
        if (!valid_config_bindings.empty()) {
            bindings = valid_config_bindings;
            std::cout << "Loaded " << bindings.size() << " bindings from config\n";
        } else {
            std::cout << "WARNING: All config bindings were invalid, falling back to defaults\n";
            bindings = make_default_bindings();
        }
    } else {
        bindings = make_default_bindings();
        std::cout << "Loaded " << bindings.size() << " default bindings\n";
    }
    
    // Validate source codes and filter out invalid bindings
    validate_and_filter_bindings(bindings, input_devices);
    
    BindingResolver resolver(bindings);
    
#ifdef DEBUG_BINDINGS
    const char* debug_env = getenv("TWCS_DEBUG_BINDINGS");
    debug_bindings_enabled = (debug_env && strcmp(debug_env, "1") == 0);
    if (debug_bindings_enabled) {
        printf("Debug bindings enabled\n");
    }
#endif

    // Main event loop - Virtual Controller Contract must remain fixed
    // Axes: 8 (ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y)
    // Buttons: 11 (Face 4, Shoulders 2, System 3, Stick clicks 2)
    // NO digital triggers - analog only
    struct epoll_event events[8];
    bool events_written = false;
    
    while (running) {
        int nfds = epoll_wait(epoll_fd, events, 8, 100);  // 100ms timeout
        
        if (nfds < 0) {
            if (errno == EINTR) continue;
            perror("epoll_wait failed");
            break;
        }
        
        for (int i = 0; i < nfds; i++) {
            // Find the device that triggered this event
            InputDevice* source_device = nullptr;
            for (auto& dev : input_devices) {
                if (dev.fd == events[i].data.fd) {
                    source_device = &dev;
                    break;
                }
            }
            
            if (!source_device) continue;
            
            // Skip offline devices
            if (!source_device->online || source_device->fd < 0 || !source_device->dev) {
                continue;
            }
            
            struct input_event ev;
            int rc = libevdev_next_event(source_device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            
            if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
                // Reset failure counter on successful read
                source_device->consecutive_read_failures = 0;
                events_written = false;
                
                switch (ev.type) {
                    case EV_ABS: {
                        // Apply axis transformation based on original logic
                        int transformed_value = ev.value;
                        Role role = string_to_role(source_device->role);
                        
                        // Apply range conversion as in original code
                        if (ev.code == ABS_X || ev.code == ABS_Y) {
                            // Stick axes: convert to 16-bit signed
                            int min = libevdev_get_abs_minimum(source_device->dev, ev.code);
                            int max = libevdev_get_abs_maximum(source_device->dev, ev.code);
                            if (min != max) {
                                transformed_value = (transformed_value - min) * 65535 / (max - min) - 32768;
                                if (transformed_value > 32767) transformed_value = 32767;
                                if (transformed_value < -32768) transformed_value = -32768;
                            }
                        } else if (ev.code == ABS_Z || ev.code == ABS_RZ || ev.code == ABS_THROTTLE) {
                            // Trigger axes: convert to 0-255
                            int min = libevdev_get_abs_minimum(source_device->dev, ev.code);
                            int max = libevdev_get_abs_maximum(source_device->dev, ev.code);
                            if (min != max) {
                                transformed_value = (transformed_value - min) * 255 / (max - min);
                                if (transformed_value > 255) transformed_value = 255;
                                if (transformed_value < 0) transformed_value = 0;
                            }
                        } else if (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y) {
                            // Hat axes: convert to -1, 0, 1
                            transformed_value = (transformed_value > 0) ? 1 : ((transformed_value < 0) ? -1 : 0);
                        }
                        
                        // Process through binding resolver
                        PhysicalInput input{role, SrcKind::Abs, static_cast<uint16_t>(ev.code)};
                        resolver.process_input(input, transformed_value);
                        break;
                    }
                    
                    case EV_KEY: {
                        // Process through binding resolver
                        Role role = string_to_role(source_device->role);
                        PhysicalInput input{role, SrcKind::Key, static_cast<uint16_t>(ev.code)};
                        resolver.process_input(input, ev.value);
                        break;
                    }
                    
                    case EV_SYN:
                        // Always pass through sync events
                        if (write(uinput_fd, &ev, sizeof(ev)) >= 0) {
                            events_written = false;
                        }
                        break;
                }
                
                // Emit any pending virtual events
                auto pending_events = resolver.get_pending_events();
                bool events_emitted = false;
                
                for (const auto& [slot, value] : pending_events) {
                    struct input_event out_ev;
                    memset(&out_ev, 0, sizeof(out_ev));
                    out_ev.type = (slot.kind == SrcKind::Key) ? EV_KEY : EV_ABS;
                    out_ev.code = slot.code;
                    out_ev.value = value;
                    
                    if (write(uinput_fd, &out_ev, sizeof(out_ev)) >= 0) {
                        events_emitted = true;
                    }
                }
                
                resolver.clear_pending_events();
                
                // Emit sync only if we actually emitted events (safety check)
                if (events_emitted) {
                    struct input_event sync_ev;
                    memset(&sync_ev, 0, sizeof(sync_ev));
                    sync_ev.type = EV_SYN;
                    sync_ev.code = SYN_REPORT;
                    sync_ev.value = 0;
                    
                    write(uinput_fd, &sync_ev, sizeof(sync_ev));
                }
            } else if (rc == -EAGAIN) {
                // No data available, reset failure counter
                source_device->consecutive_read_failures = 0;
            } else {
                // Read error - handle device disconnection
                handle_device_error(*source_device);
            }
        }
        
        // Try to reconnect any offline devices
        for (auto& device : input_devices) {
            attempt_device_reconnection(device);
            
            // If device came back online, add it to epoll
            if (device.online && device.fd >= 0) {
                struct epoll_event event;
                event.events = EPOLLIN;
                event.data.fd = device.fd;
                if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, device.fd, &event) < 0) {
                    if (errno != EEXIST) { // EEXIST means already added
                        perror("Failed to add reconnected device to epoll");
                    }
                }
            }
        }
    }

    std::cout << "Exiting...\n";
    
    // Clean up
    ioctl(uinput_fd, UI_DEV_DESTROY);
    close(uinput_fd);
    close(epoll_fd);
    for (auto& dev : input_devices) {
        libevdev_free(dev.dev);
        close(dev.fd);
    }
    
    return 0;
}