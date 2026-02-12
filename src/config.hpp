#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <vector>
#include <map>

// Version marker for new config format
constexpr int CONFIG_VERSION = 2;

struct AxisCalibration {
    int src_code;
    int observed_min;
    int observed_max;
    int center_value;
    int deadzone_radius;
};

struct DeviceConfig {
    std::string role;
    std::string by_id;
    std::string vendor;
    std::string product;
    bool optional = false;
};

struct BindingConfigKey {
    std::string role;
    int src;
    int dst;
};

struct BindingConfigAbs {
    std::string role;
    int src;
    int dst;
    bool invert = false;
    int deadzone = 0;
    float scale = 1.0f;
};

struct Profile {
    std::string name;
    std::string description;
    std::vector<BindingConfigKey> bindings_keys;
    std::vector<BindingConfigAbs> bindings_abs;
};

// Single unified config structure
struct Config {
    int version = CONFIG_VERSION;
    
    // Settings
    std::string uinput_name = "Thrustmaster ARMA Virtual";
    bool grab = true;
    
    // Device paths (shared across all profiles)
    std::map<std::string, DeviceConfig> devices;  // role -> device
    
    // Calibrations (shared across all profiles, keyed by role)
    std::map<std::string, std::map<int, AxisCalibration>> calibrations;  // role -> (axis_code -> calibration)
    
    // Profiles
    std::map<std::string, Profile> profiles;
    std::string active_profile = "default";
    
    // Get active profile bindings
    std::vector<BindingConfigKey> get_active_bindings_keys() const;
    std::vector<BindingConfigAbs> get_active_bindings_abs() const;
    
    // Get calibration for a role/axis
    std::optional<AxisCalibration> get_calibration(const std::string& role, int axis_code) const;
    void set_calibration(const std::string& role, int axis_code, const AxisCalibration& cal);
};

class ConfigManager {
public:
    static std::string get_config_path();
    static std::optional<Config> load(const std::string& config_path);
    static bool save(const std::string& config_path, const Config& config);
    
    // Profile management
    static bool switch_profile(Config& config, const std::string& profile_name);
    static bool create_profile(Config& config, const std::string& name, 
                               const std::string& display_name = "");
    static bool delete_profile(Config& config, const std::string& name);
    static bool duplicate_profile(Config& config, const std::string& source_name,
                                  const std::string& dest_name);
    
private:
    static std::string escape_json_string(const std::string& str);
    static std::string unescape_json_string(const std::string& str);
    static std::optional<std::string> get_json_value(const std::string& json, std::string_view key);
    
    // New parsing functions
    static std::map<std::string, DeviceConfig> parse_devices(const std::string& json);
    static std::map<std::string, std::map<int, AxisCalibration>> parse_calibrations(const std::string& json);
    static std::map<std::string, Profile> parse_profiles(const std::string& json);
    static Profile parse_single_profile(const std::string& json);
    static std::vector<BindingConfigKey> parse_bindings_keys(const std::string& json);
    static std::vector<BindingConfigAbs> parse_bindings_abs(const std::string& json);
};
