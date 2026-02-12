// config.cpp - Unified config with profiles
#include "config.hpp"
#include <fstream>
#include <sstream>
#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <iostream>

// Get config path from environment or use default
std::string ConfigManager::get_config_path() {
    const char* env_path = getenv("TWCS_CONFIG");
    if (env_path) {
        return std::string(env_path);
    }
    
    const char* home = getenv("HOME");
    if (!home) {
        home = getenv("USERPROFILE");  // Windows fallback
    }
    if (!home) {
        return "/etc/twcs-mapper/config.json";
    }
    
    return std::string(home) + "/.config/twcs-mapper/config.json";
}

std::optional<Config> ConfigManager::load(const std::string& config_path) {
    std::ifstream file(config_path);
    if (!file.is_open()) {
        return std::nullopt;
    }

    std::string json;
    std::string line;
    while (std::getline(file, line)) {
        json += line + "\n";
    }

    Config config;
    
    // Check version
    auto version_opt = get_json_value(json, "version");
    if (version_opt) {
        config.version = std::stoi(*version_opt);
    }
    
    // Parse settings
    auto settings_opt = get_json_value(json, "settings");
    if (settings_opt) {
        auto uinput_opt = get_json_value(*settings_opt, "uinput_name");
        if (uinput_opt) config.uinput_name = unescape_json_string(*uinput_opt);
        
        auto grab_opt = get_json_value(*settings_opt, "grab");
        if (grab_opt) config.grab = (*grab_opt == "true");
    }
    
    // Parse devices
    config.devices = parse_devices(json);
    
    // Parse calibrations
    config.calibrations = parse_calibrations(json);
    
    // Parse profiles
    config.profiles = parse_profiles(json);
    
    // Get active profile
    auto active_opt = get_json_value(json, "active_profile");
    if (active_opt && config.profiles.count(*active_opt)) {
        config.active_profile = *active_opt;
    } else if (!config.profiles.empty()) {
        config.active_profile = config.profiles.begin()->first;
    }
    
    // Legacy migration: if old format with inputs/bindings at root, migrate calibrations + profile
    if (config.profiles.empty()) {
        auto inputs_opt = get_json_value(json, "inputs");
        if (inputs_opt) {
            // Migrate embedded calibrations from legacy inputs[] if not already loaded
            if (config.calibrations.empty()) {
                std::string inputs_str = *inputs_opt;
                if (!inputs_str.empty() && inputs_str.front() == '[' && inputs_str.back() == ']') {
                    inputs_str = inputs_str.substr(1, inputs_str.length() - 2);
                }
                
                size_t pos = 0;
                while (pos < inputs_str.length()) {
                    size_t obj_start = inputs_str.find('{', pos);
                    if (obj_start == std::string::npos) break;
                    
                    int brace_count = 1;
                    size_t obj_end = obj_start + 1;
                    while (obj_end < inputs_str.length() && brace_count > 0) {
                        if (inputs_str[obj_end] == '{') brace_count++;
                        else if (inputs_str[obj_end] == '}') brace_count--;
                        obj_end++;
                    }
                    
                    if (brace_count == 0) {
                        std::string obj_str = inputs_str.substr(obj_start, obj_end - obj_start);
                        std::string role = get_json_value(obj_str, "role").value_or("");
                        
                        if (!role.empty()) {
                            auto cals_opt = get_json_value(obj_str, "calibrations");
                            if (cals_opt) {
                                std::string cals_str = *cals_opt;
                                if (!cals_str.empty() && cals_str.front() == '[' && cals_str.back() == ']') {
                                    cals_str = cals_str.substr(1, cals_str.length() - 2);
                                }
                                
                                size_t cpos = 0;
                                while (cpos < cals_str.length()) {
                                    size_t cal_start = cals_str.find('{', cpos);
                                    if (cal_start == std::string::npos) break;
                                    
                                    int cb = 1;
                                    size_t cal_end = cal_start + 1;
                                    while (cal_end < cals_str.length() && cb > 0) {
                                        if (cals_str[cal_end] == '{') cb++;
                                        else if (cals_str[cal_end] == '}') cb--;
                                        cal_end++;
                                    }
                                    
                                    if (cb == 0) {
                                        std::string cal_obj = cals_str.substr(cal_start, cal_end - cal_start);
                                        AxisCalibration cal;
                                        cal.src_code = std::stoi(get_json_value(cal_obj, "src_code").value_or("0"));
                                        cal.observed_min = std::stoi(get_json_value(cal_obj, "observed_min").value_or("0"));
                                        cal.observed_max = std::stoi(get_json_value(cal_obj, "observed_max").value_or("65535"));
                                        cal.center_value = std::stoi(get_json_value(cal_obj, "center_value").value_or("32768"));
                                        cal.deadzone_radius = std::stoi(get_json_value(cal_obj, "deadzone_radius").value_or("0"));
                                        config.calibrations[role][cal.src_code] = cal;
                                    }
                                    cpos = cal_end;
                                }
                            }
                        }
                    }
                    pos = obj_end;
                }
            }
        }
        
        // Create default profile with legacy bindings
        Profile default_profile;
        default_profile.name = "Default";
        default_profile.description = "Migrated from legacy config";
        default_profile.bindings_keys = parse_bindings_keys(json);
        default_profile.bindings_abs = parse_bindings_abs(json);
        
        if (!default_profile.bindings_keys.empty() || !default_profile.bindings_abs.empty()) {
            config.profiles["default"] = default_profile;
            config.active_profile = "default";
        }
    }
    
    // Ensure at least one profile exists
    if (config.profiles.empty()) {
        Profile empty_profile;
        empty_profile.name = "Default";
        empty_profile.description = "Empty profile";
        config.profiles["default"] = empty_profile;
        config.active_profile = "default";
    }

    return config;
}

bool ConfigManager::save(const std::string& config_path, const Config& config) {
    // Create directory if needed
    size_t last_slash = config_path.find_last_of('/');
    if (last_slash != std::string::npos) {
        std::string dir = config_path.substr(0, last_slash);
        struct stat st;
        if (stat(dir.c_str(), &st) != 0) {
            std::string mkdir_cmd = "mkdir -p \"" + dir + "\"";
            if (system(mkdir_cmd.c_str()) != 0) {
                return false;
            }
        }
    }

    std::ofstream file(config_path);
    if (!file.is_open()) {
        return false;
    }

    file << "{\n";
    file << "  \"version\": " << config.version << ",\n";
    
    // Settings
    file << "  \"settings\": {\n";
    file << "    \"uinput_name\": \"" << escape_json_string(config.uinput_name) << "\",\n";
    file << "    \"grab\": " << (config.grab ? "true" : "false") << "\n";
    file << "  },\n";
    
    // Devices
    file << "  \"devices\": {\n";
    bool first_device = true;
    for (const auto& [role, device] : config.devices) {
        if (!first_device) file << ",\n";
        first_device = false;
        
        file << "    \"" << escape_json_string(role) << "\": {\n";
        file << "      \"by_id\": \"" << escape_json_string(device.by_id) << "\",\n";
        file << "      \"vendor\": \"" << escape_json_string(device.vendor) << "\",\n";
        file << "      \"product\": \"" << escape_json_string(device.product) << "\",\n";
        file << "      \"optional\": " << (device.optional ? "true" : "false") << "\n";
        file << "    }";
    }
    file << "\n  },\n";
    
    // Calibrations
    file << "  \"calibrations\": {\n";
    bool first_cal_role = true;
    for (const auto& [role, axes] : config.calibrations) {
        if (!first_cal_role) file << ",\n";
        first_cal_role = false;
        
        file << "    \"" << escape_json_string(role) << "\": {\n";
        bool first_axis = true;
        for (const auto& [axis_code, cal] : axes) {
            if (!first_axis) file << ",\n";
            first_axis = false;
            
            file << "      \"" << axis_code << "\": {\n";
            file << "        \"src_code\": " << cal.src_code << ",\n";
            file << "        \"observed_min\": " << cal.observed_min << ",\n";
            file << "        \"observed_max\": " << cal.observed_max << ",\n";
            file << "        \"center_value\": " << cal.center_value << ",\n";
            file << "        \"deadzone_radius\": " << cal.deadzone_radius << "\n";
            file << "      }";
        }
        file << "\n    }";
    }
    file << "\n  },\n";
    
    // Profiles
    file << "  \"profiles\": {\n";
    bool first_profile = true;
    for (const auto& [id, profile] : config.profiles) {
        if (!first_profile) file << ",\n";
        first_profile = false;
        
        file << "    \"" << escape_json_string(id) << "\": {\n";
        file << "      \"name\": \"" << escape_json_string(profile.name) << "\",\n";
        file << "      \"description\": \"" << escape_json_string(profile.description) << "\",\n";
        
        // Keys
        file << "      \"bindings\": {\n";
        file << "        \"keys\": [\n";
        for (size_t i = 0; i < profile.bindings_keys.size(); i++) {
            const auto& binding = profile.bindings_keys[i];
            file << "          {\n";
            file << "            \"role\": \"" << escape_json_string(binding.role) << "\",\n";
            file << "            \"src\": " << binding.src << ",\n";
            file << "            \"dst\": " << binding.dst << "\n";
            file << "          }";
            file << (i < profile.bindings_keys.size() - 1 ? ",\n" : "\n");
        }
        file << "        ]";
        
        // ABS
        if (!profile.bindings_abs.empty()) {
            file << ",\n        \"abs\": [\n";
            for (size_t i = 0; i < profile.bindings_abs.size(); i++) {
                const auto& binding = profile.bindings_abs[i];
                file << "          {\n";
                file << "            \"role\": \"" << escape_json_string(binding.role) << "\",\n";
                file << "            \"src\": " << binding.src << ",\n";
                file << "            \"dst\": " << binding.dst << ",\n";
                file << "            \"invert\": " << (binding.invert ? "true" : "false") << ",\n";
                file << "            \"deadzone\": " << binding.deadzone << ",\n";
                
                std::ostringstream scale_stream;
                scale_stream << binding.scale;
                file << "            \"scale\": " << scale_stream.str() << "\n";
                file << "          }";
                file << (i < profile.bindings_abs.size() - 1 ? ",\n" : "\n");
            }
            file << "        ]\n";
        } else {
            file << "\n";
        }
        file << "      }\n";
        file << "    }";
    }
    file << "\n  },\n";
    
    // Active profile
    file << "  \"active_profile\": \"" << escape_json_string(config.active_profile) << "\"\n";
    
    file << "}";
    
    return file.good();
}

// Profile management
bool ConfigManager::switch_profile(Config& config, const std::string& profile_name) {
    if (config.profiles.count(profile_name)) {
        config.active_profile = profile_name;
        return true;
    }
    return false;
}

bool ConfigManager::create_profile(Config& config, const std::string& name, 
                                   const std::string& display_name) {
    if (config.profiles.count(name)) {
        return false;  // Already exists
    }
    
    Profile new_profile;
    new_profile.name = display_name.empty() ? name : display_name;
    new_profile.description = "";
    
    // Copy bindings from current active profile as template
    if (config.profiles.count(config.active_profile)) {
        new_profile.bindings_keys = config.profiles[config.active_profile].bindings_keys;
        new_profile.bindings_abs = config.profiles[config.active_profile].bindings_abs;
    }
    
    config.profiles[name] = new_profile;
    return true;
}

bool ConfigManager::delete_profile(Config& config, const std::string& name) {
    if (name == "default" || !config.profiles.count(name)) {
        return false;
    }
    
    config.profiles.erase(name);
    
    // If we deleted the active profile, switch to default
    if (config.active_profile == name) {
        config.active_profile = config.profiles.count("default") ? "default" : 
                               config.profiles.begin()->first;
    }
    
    return true;
}

bool ConfigManager::duplicate_profile(Config& config, const std::string& source_name,
                                      const std::string& dest_name) {
    if (!config.profiles.count(source_name) || config.profiles.count(dest_name)) {
        return false;
    }
    
    config.profiles[dest_name] = config.profiles[source_name];
    config.profiles[dest_name].name = dest_name;
    return true;
}

// Helper methods
std::vector<BindingConfigKey> Config::get_active_bindings_keys() const {
    auto it = profiles.find(active_profile);
    if (it != profiles.end()) {
        return it->second.bindings_keys;
    }
    return {};
}

std::vector<BindingConfigAbs> Config::get_active_bindings_abs() const {
    auto it = profiles.find(active_profile);
    if (it != profiles.end()) {
        return it->second.bindings_abs;
    }
    return {};
}

std::optional<AxisCalibration> Config::get_calibration(const std::string& role, int axis_code) const {
    auto role_it = calibrations.find(role);
    if (role_it != calibrations.end()) {
        auto axis_it = role_it->second.find(axis_code);
        if (axis_it != role_it->second.end()) {
            return axis_it->second;
        }
    }
    return std::nullopt;
}

void Config::set_calibration(const std::string& role, int axis_code, const AxisCalibration& cal) {
    calibrations[role][axis_code] = cal;
}

// JSON parsing helpers
std::map<std::string, DeviceConfig> ConfigManager::parse_devices(const std::string& json) {
    std::map<std::string, DeviceConfig> devices;
    
    auto devices_opt = get_json_value(json, "devices");
    if (!devices_opt) {
        // Try legacy format
        auto inputs_opt = get_json_value(json, "inputs");
        if (!inputs_opt) return devices;
        
        // Parse legacy inputs array
        std::string inputs_str = *inputs_opt;
        if (!inputs_str.empty() && inputs_str.front() == '[' && inputs_str.back() == ']') {
            inputs_str = inputs_str.substr(1, inputs_str.length() - 2);
        }
        
        size_t pos = 0;
        while (pos < inputs_str.length()) {
            size_t obj_start = inputs_str.find('{', pos);
            if (obj_start == std::string::npos) break;
            
            int brace_count = 1;
            size_t obj_end = obj_start + 1;
            while (obj_end < inputs_str.length() && brace_count > 0) {
                if (inputs_str[obj_end] == '{') brace_count++;
                else if (inputs_str[obj_end] == '}') brace_count--;
                obj_end++;
            }
            
            if (brace_count == 0) {
                std::string obj_str = inputs_str.substr(obj_start, obj_end - obj_start);
                
                DeviceConfig device;
                device.role = get_json_value(obj_str, "role").value_or("");
                device.by_id = get_json_value(obj_str, "by_id").value_or("");
                device.vendor = get_json_value(obj_str, "vendor").value_or("");
                device.product = get_json_value(obj_str, "product").value_or("");
                device.optional = get_json_value(obj_str, "optional").value_or("false") == "true";
                
                if (!device.role.empty()) {
                    devices[device.role] = device;
                }
            }
            pos = obj_end;
        }
        return devices;
    }
    
    // Parse new format
    std::string devices_str = *devices_opt;
    if (devices_str.empty() || devices_str.front() != '{' || devices_str.back() != '}') {
        return devices;
    }
    
    devices_str = devices_str.substr(1, devices_str.length() - 2);
    
    // Parse key-value pairs
    size_t pos = 0;
    while (pos < devices_str.length()) {
        // Find key
        size_t key_start = devices_str.find('"', pos);
        if (key_start == std::string::npos) break;
        
        size_t key_end = devices_str.find('"', key_start + 1);
        if (key_end == std::string::npos) break;
        
        std::string role = unescape_json_string(devices_str.substr(key_start + 1, key_end - key_start - 1));
        
        // Find value object
        size_t obj_start = devices_str.find('{', key_end);
        if (obj_start == std::string::npos) break;
        
        int brace_count = 1;
        size_t obj_end = obj_start + 1;
        while (obj_end < devices_str.length() && brace_count > 0) {
            if (devices_str[obj_end] == '{') brace_count++;
            else if (devices_str[obj_end] == '}') brace_count--;
            obj_end++;
        }
        
        if (brace_count == 0) {
            std::string obj_str = devices_str.substr(obj_start, obj_end - obj_start);
            
            DeviceConfig device;
            device.role = role;
            device.by_id = get_json_value(obj_str, "by_id").value_or("");
            device.vendor = get_json_value(obj_str, "vendor").value_or("");
            device.product = get_json_value(obj_str, "product").value_or("");
            device.optional = get_json_value(obj_str, "optional").value_or("false") == "true";
            
            devices[role] = device;
        }
        
        pos = obj_end;
    }
    
    return devices;
}

std::map<std::string, std::map<int, AxisCalibration>> ConfigManager::parse_calibrations(const std::string& json) {
    std::map<std::string, std::map<int, AxisCalibration>> calibrations;
    
    auto cal_opt = get_json_value(json, "calibrations");
    if (!cal_opt) return calibrations;
    
    std::string cal_str = *cal_opt;
    if (cal_str.empty() || cal_str.front() != '{' || cal_str.back() != '}') {
        return calibrations;
    }
    
    cal_str = cal_str.substr(1, cal_str.length() - 2);
    
    size_t pos = 0;
    while (pos < cal_str.length()) {
        size_t key_start = cal_str.find('"', pos);
        if (key_start == std::string::npos) break;
        
        size_t key_end = cal_str.find('"', key_start + 1);
        if (key_end == std::string::npos) break;
        
        std::string role = unescape_json_string(cal_str.substr(key_start + 1, key_end - key_start - 1));
        
        size_t obj_start = cal_str.find('{', key_end);
        if (obj_start == std::string::npos) break;
        
        int brace_count = 1;
        size_t obj_end = obj_start + 1;
        while (obj_end < cal_str.length() && brace_count > 0) {
            if (cal_str[obj_end] == '{') brace_count++;
            else if (cal_str[obj_end] == '}') brace_count--;
            obj_end++;
        }
        
        if (brace_count == 0) {
            std::string axes_str = cal_str.substr(obj_start, obj_end - obj_start);
            
            // Parse axis calibrations
            size_t axis_pos = 0;
            while (axis_pos < axes_str.length()) {
                size_t axis_key_start = axes_str.find('"', axis_pos);
                if (axis_key_start == std::string::npos) break;
                
                size_t axis_key_end = axes_str.find('"', axis_key_start + 1);
                if (axis_key_end == std::string::npos) break;
                
                int axis_code = std::stoi(axes_str.substr(axis_key_start + 1, axis_key_end - axis_key_start - 1));
                
                size_t cal_obj_start = axes_str.find('{', axis_key_end);
                if (cal_obj_start == std::string::npos) break;
                
                int cal_brace_count = 1;
                size_t cal_obj_end = cal_obj_start + 1;
                while (cal_obj_end < axes_str.length() && cal_brace_count > 0) {
                    if (axes_str[cal_obj_end] == '{') cal_brace_count++;
                    else if (axes_str[cal_obj_end] == '}') cal_brace_count--;
                    cal_obj_end++;
                }
                
                if (cal_brace_count == 0) {
                    std::string cal_obj = axes_str.substr(cal_obj_start, cal_obj_end - cal_obj_start);
                    
                    AxisCalibration cal;
                    cal.src_code = std::stoi(get_json_value(cal_obj, "src_code").value_or("0"));
                    cal.observed_min = std::stoi(get_json_value(cal_obj, "observed_min").value_or("0"));
                    cal.observed_max = std::stoi(get_json_value(cal_obj, "observed_max").value_or("65535"));
                    cal.center_value = std::stoi(get_json_value(cal_obj, "center_value").value_or("32768"));
                    cal.deadzone_radius = std::stoi(get_json_value(cal_obj, "deadzone_radius").value_or("0"));
                    
                    calibrations[role][axis_code] = cal;
                }
                
                axis_pos = cal_obj_end;
            }
        }
        
        pos = obj_end;
    }
    
    return calibrations;
}

std::map<std::string, Profile> ConfigManager::parse_profiles(const std::string& json) {
    std::map<std::string, Profile> profiles;
    
    auto profiles_opt = get_json_value(json, "profiles");
    if (!profiles_opt) return profiles;
    
    std::string profiles_str = *profiles_opt;
    if (profiles_str.empty() || profiles_str.front() != '{' || profiles_str.back() != '}') {
        return profiles;
    }
    
    profiles_str = profiles_str.substr(1, profiles_str.length() - 2);
    
    size_t pos = 0;
    while (pos < profiles_str.length()) {
        size_t key_start = profiles_str.find('"', pos);
        if (key_start == std::string::npos) break;
        
        size_t key_end = profiles_str.find('"', key_start + 1);
        if (key_end == std::string::npos) break;
        
        std::string profile_id = unescape_json_string(profiles_str.substr(key_start + 1, key_end - key_start - 1));
        
        size_t obj_start = profiles_str.find('{', key_end);
        if (obj_start == std::string::npos) break;
        
        int brace_count = 1;
        size_t obj_end = obj_start + 1;
        while (obj_end < profiles_str.length() && brace_count > 0) {
            if (profiles_str[obj_end] == '{') brace_count++;
            else if (profiles_str[obj_end] == '}') brace_count--;
            obj_end++;
        }
        
        if (brace_count == 0) {
            std::string profile_obj = profiles_str.substr(obj_start, obj_end - obj_start);
            Profile profile;
            profile.name = unescape_json_string(get_json_value(profile_obj, "name").value_or(profile_id));
            profile.description = unescape_json_string(get_json_value(profile_obj, "description").value_or(""));
            profile.bindings_keys = parse_bindings_keys(profile_obj);
            profile.bindings_abs = parse_bindings_abs(profile_obj);
            
            profiles[profile_id] = profile;
        }
        
        pos = obj_end;
    }
    
    return profiles;
}

// Legacy binding parsers (reuse existing)
std::vector<BindingConfigKey> ConfigManager::parse_bindings_keys(const std::string& json) {
    std::vector<BindingConfigKey> bindings;

    auto bindings_obj_opt = get_json_value(json, "bindings");
    if (!bindings_obj_opt) return bindings;

    auto keys_array_opt = get_json_value(*bindings_obj_opt, "keys");
    if (!keys_array_opt) return bindings;

    std::string keys_str = *keys_array_opt;
    if (!keys_str.empty() && keys_str.front() == '[' && keys_str.back() == ']') {
        keys_str = keys_str.substr(1, keys_str.length() - 2);
    }

    size_t pos = 0;
    while (pos < keys_str.length()) {
        size_t obj_start = keys_str.find('{', pos);
        if (obj_start == std::string::npos) break;

        int brace_count = 1;
        size_t obj_end = obj_start + 1;
        while (obj_end < keys_str.length() && brace_count > 0) {
            if (keys_str[obj_end] == '{') brace_count++;
            else if (keys_str[obj_end] == '}') brace_count--;
            obj_end++;
        }

        if (brace_count == 0) {
            std::string obj_str = keys_str.substr(obj_start, obj_end - obj_start);

            BindingConfigKey binding;
            auto role_opt = get_json_value(obj_str, "role");
            auto src_opt = get_json_value(obj_str, "src");
            auto dst_opt = get_json_value(obj_str, "dst");

            if (role_opt && src_opt && dst_opt) {
                binding.role = *role_opt;
                binding.src = std::stoi(*src_opt);
                binding.dst = std::stoi(*dst_opt);
                bindings.push_back(binding);
            }
        }

        pos = obj_end;
    }

    return bindings;
}

std::vector<BindingConfigAbs> ConfigManager::parse_bindings_abs(const std::string& json) {
    std::vector<BindingConfigAbs> bindings;

    auto bindings_obj_opt = get_json_value(json, "bindings");
    if (!bindings_obj_opt) return bindings;

    auto abs_array_opt = get_json_value(*bindings_obj_opt, "abs");
    if (!abs_array_opt) return bindings;

    std::string abs_str = *abs_array_opt;
    if (!abs_str.empty() && abs_str.front() == '[' && abs_str.back() == ']') {
        abs_str = abs_str.substr(1, abs_str.length() - 2);
    }

    size_t pos = 0;
    while (pos < abs_str.length()) {
        size_t obj_start = abs_str.find('{', pos);
        if (obj_start == std::string::npos) break;

        int brace_count = 1;
        size_t obj_end = obj_start + 1;
        while (obj_end < abs_str.length() && brace_count > 0) {
            if (abs_str[obj_end] == '{') brace_count++;
            else if (abs_str[obj_end] == '}') brace_count--;
            obj_end++;
        }

        if (brace_count == 0) {
            std::string obj_str = abs_str.substr(obj_start, obj_end - obj_start);

            BindingConfigAbs binding;
            auto role_opt = get_json_value(obj_str, "role");
            auto src_opt = get_json_value(obj_str, "src");
            auto dst_opt = get_json_value(obj_str, "dst");

            if (role_opt && src_opt && dst_opt) {
                binding.role = *role_opt;
                binding.src = std::stoi(*src_opt);
                binding.dst = std::stoi(*dst_opt);

                auto invert_opt = get_json_value(obj_str, "invert");
                if (invert_opt) binding.invert = (*invert_opt == "true");

                auto deadzone_opt = get_json_value(obj_str, "deadzone");
                if (deadzone_opt) binding.deadzone = std::stoi(*deadzone_opt);

                auto scale_opt = get_json_value(obj_str, "scale");
                if (scale_opt) binding.scale = std::stof(*scale_opt);

                bindings.push_back(binding);
            }
        }

        pos = obj_end;
    }

    return bindings;
}

// JSON helpers (copied from original)
std::string ConfigManager::escape_json_string(const std::string& str) {
    std::string result;
    result.reserve(str.size() + 10);

    for (char c : str) {
        switch (c) {
            case '"': result += "\\\""; break;
            case '\\': result += "\\\\"; break;
            case '\n': result += "\\n"; break;
            case '\r': result += "\\r"; break;
            case '\t': result += "\\t"; break;
            default: result += c; break;
        }
    }

    return result;
}

std::string ConfigManager::unescape_json_string(const std::string& str) {
    std::string result;
    result.reserve(str.size());

    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == '\\' && i + 1 < str.size()) {
            switch (str[i + 1]) {
                case '"': result += '"'; ++i; break;
                case '\\': result += '\\'; ++i; break;
                case 'n': result += '\n'; ++i; break;
                case 'r': result += '\r'; ++i; break;
                case 't': result += '\t'; ++i; break;
                default: result += str[i]; break;
            }
        } else {
            result += str[i];
        }
    }

    return result;
}

std::optional<std::string> ConfigManager::get_json_value(const std::string& json, std::string_view key) {
    std::string search_key = "\"";
    search_key += key;
    search_key += "\"";

    size_t key_pos = json.find(search_key);
    if (key_pos == std::string::npos) {
        return std::nullopt;
    }

    size_t colon_pos = json.find(':', key_pos);
    if (colon_pos == std::string::npos) {
        return std::nullopt;
    }

    size_t value_start = json.find_first_not_of(" \t", colon_pos + 1);
    if (value_start == std::string::npos) {
        return std::nullopt;
    }

    // Handle object/array values
    if (value_start < json.size() && (json[value_start] == '[' || json[value_start] == '{')) {
        const char open = json[value_start];
        const char close = (open == '[') ? ']' : '}';
        int depth = 0;
        bool in_string = false;
        bool escape = false;

        for (size_t i = value_start; i < json.size(); ++i) {
            const char c = json[i];

            if (in_string) {
                if (escape) { escape = false; continue; }
                if (c == '\\') { escape = true; continue; }
                if (c == '"') in_string = false;
                continue;
            }

            if (c == '"') { in_string = true; continue; }
            if (c == open) { depth++; continue; }
            if (c == close) {
                depth--;
                if (depth == 0) {
                    return json.substr(value_start, (i - value_start) + 1);
                }
            }
        }
        return std::nullopt;
    }

    if (json[value_start] == '"') {
        size_t value_end = json.find('"', value_start + 1);
        if (value_end == std::string::npos) {
            return std::nullopt;
        }

        size_t end = value_end;
        while (end + 1 < json.size() && json[end] == '"' && json[end - 1] == '\\') {
            end = json.find('"', end + 1);
            if (end == std::string::npos) break;
        }
        if (end == std::string::npos) {
            end = value_end;
        }

        return json.substr(value_start + 1, end - (value_start + 1));
    } else {
        size_t value_end = json.find_first_of(",}\n", value_start);
        if (value_end == std::string::npos) {
            value_end = json.size();
        }

        std::string value = json.substr(value_start, value_end - value_start);
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);
        return value;
    }
}
