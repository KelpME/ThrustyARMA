#pragma once

#include <string>
#include <string_view>
#include <optional>
#include <vector>

struct InputConfig {
    std::string role;
    std::string by_id;
    std::string vendor;
    std::string product;
    bool optional;
};

struct Config {
    std::string uinput_name = "Xbox 360 Controller (Virtual)";
    bool grab = true;
    std::vector<InputConfig> inputs;
};

class ConfigManager {
public:
    static std::optional<Config> load(const std::string& config_path);
    static bool save(const std::string& config_path, const Config& config);
    
private:
    static std::string escape_json_string(const std::string& str);
    static std::string unescape_json_string(const std::string& str);
    static std::optional<std::string> get_json_value(const std::string& json, std::string_view key);
    static std::vector<InputConfig> parse_inputs(const std::string& json);
};