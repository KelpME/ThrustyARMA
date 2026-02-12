#include "config.hpp"
#include <iostream>
#include <vector>
#include <string>
#include <filesystem>
#include <cstdio>
#include <memory>
#include <array>
#include <sstream>
#include <limits.h>

struct DeviceInfo {
    std::string by_id_path;
    std::string event_path;
    std::string vendor_id;
    std::string model_id;
    std::string name;
    std::string path;
};

std::string exec_command(const std::string& cmd) {
    std::array<char, 128> buffer;
    std::string result;
    std::unique_ptr<FILE, decltype(&pclose)> pipe(popen(cmd.c_str(), "r"), pclose);
    if (!pipe) {
        return "";
    }
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
        result += buffer.data();
    }
    return result;
}

std::string get_udev_property(const std::string& device_path, const std::string& property) {
    std::string cmd = "udevadm info -q property -n " + device_path + " 2>/dev/null";
    std::string output = exec_command(cmd);
    
    std::istringstream iss(output);
    std::string line;
    while (std::getline(iss, line)) {
        if (line.substr(0, property.length() + 1) == property + "=") {
            return line.substr(property.length() + 1);
        }
    }
    return "";
}

std::vector<DeviceInfo> enumerate_devices() {
    std::vector<DeviceInfo> devices;
    
    if (!std::filesystem::exists("/dev/input/by-id")) {
        std::cerr << "Error: /dev/input/by-id not found\n";
        return devices;
    }
    
    for (const auto& entry : std::filesystem::directory_iterator("/dev/input/by-id")) {
        if (entry.path().string().find("event") != std::string::npos) {
            DeviceInfo info;
            info.by_id_path = entry.path().string();
            
            // Resolve symlink to get the actual event device
            char real_path[PATH_MAX];
            if (realpath(entry.path().c_str(), real_path) != nullptr) {
                info.event_path = real_path;
            } else {
                continue;
            }
            
            // Get udev properties
            info.vendor_id = get_udev_property(info.event_path, "ID_VENDOR_ID");
            info.model_id = get_udev_property(info.event_path, "ID_MODEL_ID");
            info.name = get_udev_property(info.event_path, "NAME");
            info.path = get_udev_property(info.event_path, "ID_PATH");
            
            // Clean up the name (remove quotes if present)
            if (!info.name.empty() && info.name.front() == '"' && info.name.back() == '"') {
                info.name = info.name.substr(1, info.name.length() - 2);
            }
            
            devices.push_back(info);
        }
    }
    
    return devices;
}

DeviceConfig detect_device(const std::vector<DeviceInfo>& devices, 
                      const std::string& role, 
                      const std::string& vendor, 
                      const std::string& product, 
                      bool optional) {
    DeviceConfig config;
    config.role = role;
    config.vendor = vendor;
    config.product = product;
    config.optional = optional;
    
    for (const auto& dev : devices) {
        if (dev.vendor_id == vendor && dev.model_id == product) {
            config.by_id = dev.by_id_path;
            std::cout << "Detected " << role << ": " << dev.name << " (" << vendor << ":" << product << ")\n";
            std::cout << "  Path: " << dev.by_id_path << "\n";
            return config;
        }
    }
    
    if (!optional) {
        std::cerr << "ERROR: Required device " << role << " (" << vendor << ":" << product << ") not found!\n";
        exit(1);
    } else {
        std::cout << "Optional device " << role << " (" << vendor << ":" << product << ") not found\n";
        config.by_id = "";  // Empty path means not available
        return config;
    }
}

int main() {
    std::cout << "Scanning for Thrustmaster devices...\n\n";
    
    auto devices = enumerate_devices();
    if (devices.empty()) {
        std::cerr << "No input devices found in /dev/input/by-id\n";
        return 1;
    }
    
    Config config;
    config.uinput_name = "Thrustmaster ARMA Virtual";
    config.grab = true;
    
    // Detect all three devices
    std::cout << "=== Device Detection ===\n";
    
    // Stick (required)
    config.devices["stick"] = detect_device(devices, "stick", "044f", "b10a", false);
    
    // Throttle (optional)
    config.devices["throttle"] = detect_device(devices, "throttle", "044f", "b687", true);
    
    // Rudder (optional)
    config.devices["rudder"] = detect_device(devices, "rudder", "044f", "b679", true);
    
    // Create default profile with empty bindings
    Profile default_profile;
    default_profile.name = "Default";
    default_profile.description = "Default profile";
    config.profiles["default"] = default_profile;
    config.active_profile = "default";
    
    std::cout << "\n=== Configuration ===\n";
    std::cout << "Detected " << config.devices.size() << " input devices:\n";
    for (const auto& [role, device] : config.devices) {
        std::cout << "  " << role << ": " 
                  << (device.by_id.empty() ? "not present" : device.by_id) 
                  << " (optional: " << (device.optional ? "yes" : "no") << ")\n";
    }
    
    std::string config_path = ConfigManager::get_config_path();
    
    if (ConfigManager::save(config_path, config)) {
        std::cout << "\nConfiguration saved to: " << config_path << "\n";
        std::cout << "You can now run: ./build.sh run\n";
    } else {
        std::cerr << "Failed to save configuration\n";
        return 1;
    }
    
    return 0;
}