// config.cpp
#include "config.hpp"
#include <fstream>
#include <vector>
#include <cstdio>
#include <sys/stat.h>
#include <sstream>

std::optional<Config> ConfigManager::load(const std::string& config_path) {
    std::ifstream file(config_path);
    if (!file.is_open()) {
        return std::nullopt;
    }

    std::string json;
    std::string line;
    while (std::getline(file, line)) {
        json += line;
    }

    Config config;

    auto uinput_name_opt = get_json_value(json, "uinput_name");
    if (uinput_name_opt) {
        config.uinput_name = unescape_json_string(*uinput_name_opt);
    }

    auto grab_opt = get_json_value(json, "grab");
    if (grab_opt) {
        config.grab = (*grab_opt == "true");
    }

    config.inputs = parse_inputs(json);
    config.bindings_keys = parse_bindings_keys(json);
    config.bindings_abs = parse_bindings_abs(json);

    return config;
}

std::vector<InputConfig> ConfigManager::parse_inputs(const std::string& json) {
    std::vector<InputConfig> inputs;

    auto inputs_array_opt = get_json_value(json, "inputs");
    if (!inputs_array_opt) {
        return inputs;
    }

    std::string inputs_str = *inputs_array_opt;
    // Remove surrounding brackets if present
    if (!inputs_str.empty() && inputs_str.front() == '[' && inputs_str.back() == ']') {
        inputs_str = inputs_str.substr(1, inputs_str.length() - 2);
    }

    // Parse individual objects (simplified JSON parser for this specific case)
    size_t pos = 0;
    while (pos < inputs_str.length()) {
        // Find next object start
        size_t obj_start = inputs_str.find('{', pos);
        if (obj_start == std::string::npos) break;

        // Find matching brace
        int brace_count = 1;
        size_t obj_end = obj_start + 1;
        while (obj_end < inputs_str.length() && brace_count > 0) {
            if (inputs_str[obj_end] == '{') brace_count++;
            else if (inputs_str[obj_end] == '}') brace_count--;
            obj_end++;
        }

        if (brace_count == 0) {
            std::string obj_str = inputs_str.substr(obj_start, obj_end - obj_start);

            InputConfig input_config;
            input_config.role = get_json_value(obj_str, "role").value_or("");
            input_config.by_id = get_json_value(obj_str, "by_id").value_or("");
            input_config.vendor = get_json_value(obj_str, "vendor").value_or("");
            input_config.product = get_json_value(obj_str, "product").value_or("");
            input_config.optional = get_json_value(obj_str, "optional").value_or("false") == "true";
            
            // Parse calibrations array if present
            auto calibrations_opt = get_json_value(obj_str, "calibrations");
            if (calibrations_opt) {
                std::string cal_str = *calibrations_opt;
                if (!cal_str.empty() && cal_str.front() == '[' && cal_str.back() == ']') {
                    cal_str = cal_str.substr(1, cal_str.length() - 2);
                }
                
                size_t cal_pos = 0;
                while (cal_pos < cal_str.length()) {
                    size_t cal_obj_start = cal_str.find('{', cal_pos);
                    if (cal_obj_start == std::string::npos) break;
                    
                    int cal_brace_count = 1;
                    size_t cal_obj_end = cal_obj_start + 1;
                    while (cal_obj_end < cal_str.length() && cal_brace_count > 0) {
                        if (cal_str[cal_obj_end] == '{') cal_brace_count++;
                        else if (cal_str[cal_obj_end] == '}') cal_brace_count--;
                        cal_obj_end++;
                    }
                    
                    if (cal_brace_count == 0) {
                        std::string cal_obj_str = cal_str.substr(cal_obj_start, cal_obj_end - cal_obj_start);
                        
                        AxisCalibration cal;
                        cal.src_code = std::stoi(get_json_value(cal_obj_str, "src_code").value_or("0"));
                        cal.observed_min = std::stoi(get_json_value(cal_obj_str, "observed_min").value_or("0"));
                        cal.observed_max = std::stoi(get_json_value(cal_obj_str, "observed_max").value_or("65535"));
                        cal.center_value = std::stoi(get_json_value(cal_obj_str, "center_value").value_or("32768"));
                        cal.deadzone_radius = std::stoi(get_json_value(cal_obj_str, "deadzone_radius").value_or("0"));
                        
                        input_config.calibrations.push_back(cal);
                    }
                    
                    cal_pos = cal_obj_end;
                }
            }

            if (!input_config.role.empty()) {
                inputs.push_back(input_config);
            }
        }

        pos = obj_end;
    }

    return inputs;
}

bool ConfigManager::save(const std::string& config_path, const Config& config) {
    // Create directory if it doesn't exist
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
    file << "  \"uinput_name\": \"" << escape_json_string(config.uinput_name) << "\",\n";
    file << "  \"grab\": " << (config.grab ? "true" : "false") << ",\n";
    file << "  \"inputs\": [\n";

    for (size_t i = 0; i < config.inputs.size(); i++) {
        const auto& input = config.inputs[i];
        file << "    {\n";
        file << "      \"role\": \"" << escape_json_string(input.role) << "\",\n";
        file << "      \"by_id\": \"" << escape_json_string(input.by_id) << "\",\n";
        file << "      \"vendor\": \"" << escape_json_string(input.vendor) << "\",\n";
        file << "      \"product\": \"" << escape_json_string(input.product) << "\",\n";
        file << "      \"optional\": " << (input.optional ? "true" : "false");
        
        if (!input.calibrations.empty()) {
            file << ",\n";
            file << "      \"calibrations\": [\n";
            for (size_t j = 0; j < input.calibrations.size(); j++) {
                const auto& cal = input.calibrations[j];
                file << "        {\n";
                file << "          \"src_code\": " << cal.src_code << ",\n";
                file << "          \"observed_min\": " << cal.observed_min << ",\n";
                file << "          \"observed_max\": " << cal.observed_max << ",\n";
                file << "          \"center_value\": " << cal.center_value << ",\n";
                file << "          \"deadzone_radius\": " << cal.deadzone_radius << "\n";
                file << "        }";
                file << (j < input.calibrations.size() - 1 ? ",\n" : "\n");
            }
            file << "      ]\n";
        } else {
            file << "\n";
        }
        
        file << (i < config.inputs.size() - 1 ? "    },\n" : "    }\n");
    }

    file << "  ]";
    
    // Add bindings if present
    if (!config.bindings_keys.empty() || !config.bindings_abs.empty()) {
        file << ",\n";
        file << "  \"bindings\": {\n";
        
        // Keys
        file << "    \"keys\": [\n";
        for (size_t i = 0; i < config.bindings_keys.size(); i++) {
            const auto& binding = config.bindings_keys[i];
            file << "      {\n";
            file << "        \"role\": \"" << escape_json_string(binding.role) << "\",\n";
            file << "        \"src\": " << binding.src << ",\n";
            file << "        \"dst\": " << binding.dst << "\n";
            file << "      }";
            file << (i < config.bindings_keys.size() - 1 ? ",\n" : "\n");
        }
        file << "    ]";
        
        // ABS
        if (!config.bindings_abs.empty()) {
            file << ",\n";
            file << "    \"abs\": [\n";
            for (size_t i = 0; i < config.bindings_abs.size(); i++) {
                const auto& binding = config.bindings_abs[i];
                file << "      {\n";
                file << "        \"role\": \"" << escape_json_string(binding.role) << "\",\n";
                file << "        \"src\": " << binding.src << ",\n";
                file << "        \"dst\": " << binding.dst << ",\n";
                file << "        \"invert\": " << (binding.invert ? "true" : "false") << ",\n";
                file << "        \"deadzone\": " << binding.deadzone << ",\n";
                
                // Use ostringstream for minimal decimal representation
                std::ostringstream scale_stream;
                scale_stream << binding.scale;
                file << "        \"scale\": " << scale_stream.str() << "\n";
                file << "      }";
                file << (i < config.bindings_abs.size() - 1 ? ",\n" : "\n");
            }
            file << "    ]\n";
        } else {
            file << "\n";
        }
        
        file << "  }\n";
    } else {
        file << "\n";
    }
    
    file << "}";

    return file.good();
}

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

    // Handle object/array values (needed for "inputs": [ ... ])
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
        // String value
        size_t value_end = json.find('"', value_start + 1);
        if (value_end == std::string::npos) {
            return std::nullopt;
        }

        // Handle escaped quotes
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
        // Non-string value (boolean, number)
        size_t value_end = json.find_first_of(",}\n", value_start);
        if (value_end == std::string::npos) {
            value_end = json.size();
        }

        std::string value = json.substr(value_start, value_end - value_start);
        // Trim whitespace
        value.erase(0, value.find_first_not_of(" \t\r\n"));
        value.erase(value.find_last_not_of(" \t\r\n") + 1);
        return value;
    }
}

std::vector<BindingConfigKey> ConfigManager::parse_bindings_keys(const std::string& json) {
    std::vector<BindingConfigKey> bindings;

    auto bindings_array_opt = get_json_value(json, "bindings");
    if (!bindings_array_opt) {
        return bindings;
    }

    std::string bindings_str = *bindings_array_opt;
    auto keys_array_opt = get_json_value(bindings_str, "keys");
    if (!keys_array_opt) {
        return bindings;
    }

    std::string keys_str = *keys_array_opt;
    // Remove surrounding brackets if present
    if (!keys_str.empty() && keys_str.front() == '[' && keys_str.back() == ']') {
        keys_str = keys_str.substr(1, keys_str.length() - 2);
    }

    // Parse individual objects
    size_t pos = 0;
    while (pos < keys_str.length()) {
        size_t obj_start = keys_str.find('{', pos);
        if (obj_start == std::string::npos) break;

        // Find matching brace
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

    auto bindings_array_opt = get_json_value(json, "bindings");
    if (!bindings_array_opt) {
        return bindings;
    }

    std::string bindings_str = *bindings_array_opt;
    auto abs_array_opt = get_json_value(bindings_str, "abs");
    if (!abs_array_opt) {
        return bindings;
    }

    std::string abs_str = *abs_array_opt;
    // Remove surrounding brackets if present
    if (!abs_str.empty() && abs_str.front() == '[' && abs_str.back() == ']') {
        abs_str = abs_str.substr(1, abs_str.length() - 2);
    }

    // Parse individual objects
    size_t pos = 0;
    while (pos < abs_str.length()) {
        size_t obj_start = abs_str.find('{', pos);
        if (obj_start == std::string::npos) break;

        // Find matching brace
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
                if (invert_opt) {
                    binding.invert = (*invert_opt == "true");
                }

                auto deadzone_opt = get_json_value(obj_str, "deadzone");
                if (deadzone_opt) {
                    binding.deadzone = std::stoi(*deadzone_opt);
                }

                auto scale_opt = get_json_value(obj_str, "scale");
                if (scale_opt) {
                    binding.scale = std::stof(*scale_opt);
                }

                bindings.push_back(binding);
            }
        }

        pos = obj_end;
    }

    return bindings;
}
