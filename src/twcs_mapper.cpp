#include "config.hpp"
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
};

// Role-specific mapping tables
struct ButtonMapping {
    int input_code;
    int output_code;
};

struct AxisMapping {
    int input_code;
    int output_code;
};

// Stick mappings (X,Y, hat, buttons)
static const AxisMapping stick_axis_mappings[] = {
    {ABS_X, ABS_X},    // Stick X -> Virtual X
    {ABS_Y, ABS_Y},    // Stick Y -> Virtual Y
};

// Throttle mappings (main axis, hat, buttons)
static const AxisMapping throttle_axis_mappings[] = {
    {ABS_Z, ABS_RX},   // Throttle -> Virtual RX
    {ABS_HAT0X, ABS_HAT0X}, // Throttle hat -> Virtual hat X
    {ABS_HAT0Y, ABS_HAT0Y}, // Throttle hat -> Virtual hat Y
};

// Rudder mappings (main axis, buttons)
static const AxisMapping rudder_axis_mappings[] = {
    {ABS_RZ, ABS_RY},  // Rudder -> Virtual RY
};

// Combined priority button mapping (assign outputs to avoid conflicts)
// ARMA Reforger Gamepad button mapping (XInput-style)
static const std::map<int, int> gamepad_button_mapping = {
    {BTN_TRIGGER, BTN_SOUTH},    // A
    {BTN_THUMB, BTN_EAST},       // B  
    {BTN_THUMB2, BTN_WEST},      // X
    {BTN_TOP, BTN_NORTH},        // Y
    {BTN_TOP2, BTN_TL},          // LB
    {BTN_PINKIE, BTN_TR},        // RB
    {BTN_BASE, BTN_SELECT},      // Select
    {BTN_BASE2, BTN_START},      // Start
    {BTN_BASE3, BTN_THUMBL},     // LS
    {BTN_BASE4, BTN_THUMBR},     // RS
};

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

int main(int argc, char* argv[]) {
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
            
            struct input_event ev;
            int rc = libevdev_next_event(source_device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            
            if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
                events_written = false;
                
                switch (ev.type) {
                    case EV_ABS: {
                        // Virtual Controller Contract: Map to fixed 8-axis layout
                        if (source_device->role == "stick") {
                            // Stick -> Left stick (ABS_X, ABS_Y)
                            if (ev.code == ABS_X || ev.code == ABS_Y) {
                                int value = ev.value;
                                int min = libevdev_get_abs_minimum(source_device->dev, ev.code);
                                int max = libevdev_get_abs_maximum(source_device->dev, ev.code);
                                if (min != max) {
                                    value = (value - min) * 65535 / (max - min) - 32768;
                                    if (value > 32767) value = 32767;
                                    if (value < -32768) value = -32768;
                                }
                                // Keep same axis codes for left stick
                                ev.value = value;
                                if (write(uinput_fd, &ev, sizeof(ev)) >= 0) events_written = true;
                            }
                            // Stick hat -> D-pad (ABS_HAT0X, ABS_HAT0Y)
                            else if (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y) {
                                ev.value = (ev.value > 0) ? 1 : ((ev.value < 0) ? -1 : 0);
                                if (write(uinput_fd, &ev, sizeof(ev)) >= 0) events_written = true;
                            }
                        } else if (source_device->role == "throttle") {
                            // Throttle axis -> Left trigger (ABS_Z)
                            if (ev.code == ABS_Z || ev.code == ABS_THROTTLE) {
                                int min = libevdev_get_abs_minimum(source_device->dev, ev.code);
                                int max = libevdev_get_abs_maximum(source_device->dev, ev.code);
                                int value = ev.value;
                                if (min != max) {
                                    value = (value - min) * 255 / (max - min);
                                    if (value > 255) value = 255;
                                    if (value < 0) value = 0;
                                }
                                ev.code = ABS_Z;  // Left trigger
                                ev.value = value;
                                if (write(uinput_fd, &ev, sizeof(ev)) >= 0) events_written = true;
                            }
                            // Throttle hat -> D-pad (but left stick takes precedence if available)
                            else if (ev.code == ABS_HAT0X || ev.code == ABS_HAT0Y) {
                                ev.value = (ev.value > 0) ? 1 : ((ev.value < 0) ? -1 : 0);
                                if (write(uinput_fd, &ev, sizeof(ev)) >= 0) events_written = true;
                            }
                        } else if (source_device->role == "rudder") {
                            // Rudder -> Right trigger (ABS_RZ) or Right stick X (ABS_RX)
                            if (ev.code == ABS_RZ) {
                                int min = libevdev_get_abs_minimum(source_device->dev, ev.code);
                                int max = libevdev_get_abs_maximum(source_device->dev, ev.code);
                                int value = ev.value;
                                if (min != max) {
                                    value = (value - min) * 255 / (max - min);
                                    if (value > 255) value = 255;
                                    if (value < 0) value = 0;
                                }
                                ev.code = ABS_RZ;  // Right trigger
                                ev.value = value;
                                if (write(uinput_fd, &ev, sizeof(ev)) >= 0) events_written = true;
                            }
                        }
                        break;
                    }
                    
                    case EV_KEY: {
                        // Virtual Controller Contract: Map to fixed 11-button layout
                        auto mapping_it = gamepad_button_mapping.find(ev.code);
                        if (mapping_it != gamepad_button_mapping.end()) {
                            ev.code = mapping_it->second;
                            if (write(uinput_fd, &ev, sizeof(ev)) >= 0) events_written = true;
                        }
                        break;
                    }
                    
                    case EV_SYN:
                        // Always pass through sync events
                        if (write(uinput_fd, &ev, sizeof(ev)) >= 0) {
                            events_written = false;
                        }
                        break;
                }
                
                // Emit sync if we wrote events
                if (events_written) {
                    struct input_event sync_ev;
                    memset(&sync_ev, 0, sizeof(sync_ev));
                    sync_ev.type = EV_SYN;
                    sync_ev.code = SYN_REPORT;
                    sync_ev.value = 0;
                    
                    write(uinput_fd, &sync_ev, sizeof(sync_ev));
                    events_written = false;
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