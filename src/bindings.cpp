#include "bindings.hpp"
#include "config.hpp"
#include <algorithm>
#include <unordered_set>

#ifdef DEBUG_BINDINGS
bool debug_bindings_enabled = false;
#endif

Role BindingResolver::get_role_priority(const VirtualSlot& dst) {
    return Role::Stick;
}

bool BindingResolver::is_virtual_slot_valid(const VirtualSlot& slot) const {
    static const std::unordered_set<uint16_t> valid_buttons = {
        BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH,
        BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE,
        BTN_THUMBL, BTN_THUMBR
    };
    
    static const std::unordered_set<uint16_t> valid_axes = {
        ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y
    };
    
    if (slot.kind == SrcKind::Key) {
        return valid_buttons.count(slot.code) > 0;
    } else {
        return valid_axes.count(slot.code) > 0;
    }
}

int BindingResolver::apply_axis_transform(int value, const AxisTransform& xform) const {
    if (xform.invert) {
        value = -value;
    }
    
    if (xform.deadzone > 0) {
        if (value < 0) {
            value = std::min(0, value + xform.deadzone);
        } else {
            value = std::max(0, value - xform.deadzone);
        }
    }
    
    value = static_cast<int>(value * xform.scale);
    
    value = std::max(xform.min_out, std::min(xform.max_out, value));
    
    return value;
}

BindingResolver::BindingResolver(const std::vector<Binding>& bindings) : bindings(bindings) {
    for (const auto& binding : bindings) {
        if (!is_virtual_slot_valid(binding.dst)) {
            continue;
        }
        
        if (binding.dst.kind == SrcKind::Key) {
            button_refcounts[binding.dst] = 0;
        } else {
            axis_values[binding.dst][Role::Stick] = std::nullopt;
            axis_values[binding.dst][Role::Throttle] = std::nullopt;
            axis_values[binding.dst][Role::Rudder] = std::nullopt;
            axis_selected_source[binding.dst] = std::nullopt;
            last_output_values[binding.dst] = 0;
        }
    }
}

void BindingResolver::process_input(const PhysicalInput& input, int value) {
    DEBUG_LOG("Processing input: role=%d kind=%d code=%d value=%d\n", 
              static_cast<int>(input.role), static_cast<int>(input.kind), input.code, value);
    
    for (const auto& binding : bindings) {
        if (binding.src.role == input.role && 
            binding.src.kind == input.kind && 
            binding.src.code == input.code) {
            
            DEBUG_LOG("Found binding to: kind=%d code=%d\n", 
                      static_cast<int>(binding.dst.kind), binding.dst.code);
            
            if (binding.dst.kind == SrcKind::Key) {
                button_pressed_sources[binding.dst][binding.src] = (value != 0);
                
                int refcount = 0;
                for (const auto& [source, pressed] : button_pressed_sources[binding.dst]) {
                    if (pressed) refcount++;
                }
                button_refcounts[binding.dst] = refcount;
                DEBUG_LOG("Button refcount: %d\n", refcount);
            } else {
                int transformed_value = apply_axis_transform(value, binding.xform);
                axis_values[binding.dst][input.role] = transformed_value;
                DEBUG_LOG("Axis value for role %d: %d\n", static_cast<int>(input.role), transformed_value);
            }
        }
    }
}

std::vector<std::pair<VirtualSlot, int>> BindingResolver::get_pending_events() {
    std::vector<std::pair<VirtualSlot, int>> events;
    
    for (const auto& [slot, refcount] : button_refcounts) {
        int last_value = last_output_values.count(slot) ? last_output_values[slot] : 0;
        int current_value = (refcount > 0) ? 1 : 0;
        
        if (last_value != current_value) {
            events.push_back({slot, current_value});
            last_output_values[slot] = current_value;
            DEBUG_LOG("Button event: slot=%d value=%d\n", slot.code, current_value);
        }
    }
    
    for (const auto& [slot, role_values] : axis_values) {
        Role selected_role = Role::Stick;
        bool has_value = false;
        
        // Priority: Stick > Throttle > Rudder
        if (role_values.at(Role::Stick).has_value()) {
            selected_role = Role::Stick;
            has_value = true;
        } else if (role_values.at(Role::Throttle).has_value()) {
            selected_role = Role::Throttle;
            has_value = true;
        } else if (role_values.at(Role::Rudder).has_value()) {
            selected_role = Role::Rudder;
            has_value = true;
        }
        
        int current_value = has_value ? *role_values.at(selected_role) : 0;
        int last_value = last_output_values.count(slot) ? last_output_values[slot] : 0;
        
        if (last_value != current_value) {
            events.push_back({slot, current_value});
            last_output_values[slot] = current_value;
            DEBUG_LOG("Axis event: slot=%d value=%d (role=%d)\n", slot.code, current_value, static_cast<int>(selected_role));
        }
    }
    
    return events;
}

void BindingResolver::clear_pending_events() {
}

std::vector<Binding> make_default_bindings() {
    std::vector<Binding> bindings;
    
    // Stick mappings
    bindings.push_back({{Role::Stick, SrcKind::Abs, ABS_X}, {SrcKind::Abs, ABS_X}, {false, 0, 1.0f, -32768, 32767}});
    bindings.push_back({{Role::Stick, SrcKind::Abs, ABS_Y}, {SrcKind::Abs, ABS_Y}, {false, 0, 1.0f, -32768, 32767}});
    bindings.push_back({{Role::Stick, SrcKind::Abs, ABS_HAT0X}, {SrcKind::Abs, ABS_HAT0X}, {false, 0, 1.0f, -1, 1}});
    bindings.push_back({{Role::Stick, SrcKind::Abs, ABS_HAT0Y}, {SrcKind::Abs, ABS_HAT0Y}, {false, 0, 1.0f, -1, 1}});
    
    // Throttle mappings
    bindings.push_back({{Role::Throttle, SrcKind::Abs, ABS_Z}, {SrcKind::Abs, ABS_Z}, {false, 0, 1.0f, 0, 255}});
    bindings.push_back({{Role::Throttle, SrcKind::Abs, ABS_THROTTLE}, {SrcKind::Abs, ABS_Z}, {false, 0, 1.0f, 0, 255}});
    bindings.push_back({{Role::Throttle, SrcKind::Abs, ABS_HAT0X}, {SrcKind::Abs, ABS_HAT0X}, {false, 0, 1.0f, -1, 1}});
    bindings.push_back({{Role::Throttle, SrcKind::Abs, ABS_HAT0Y}, {SrcKind::Abs, ABS_HAT0Y}, {false, 0, 1.0f, -1, 1}});
    
    // Rudder mappings
    bindings.push_back({{Role::Rudder, SrcKind::Abs, ABS_RZ}, {SrcKind::Abs, ABS_RZ}, {false, 0, 1.0f, 0, 255}});
    
    // Button mappings - all roles map to the same virtual buttons
    const std::map<int, int> button_mappings = {
        {BTN_TRIGGER, BTN_SOUTH},
        {BTN_THUMB, BTN_EAST},
        {BTN_THUMB2, BTN_WEST},
        {BTN_TOP, BTN_NORTH},
        {BTN_TOP2, BTN_TL},
        {BTN_PINKIE, BTN_TR},
        {BTN_BASE, BTN_SELECT},
        {BTN_BASE2, BTN_START},
        {BTN_BASE3, BTN_THUMBL},
        {BTN_BASE4, BTN_THUMBR}
    };
    
    for (const auto& [src_btn, dst_btn] : button_mappings) {
        bindings.push_back({{Role::Stick, SrcKind::Key, static_cast<uint16_t>(src_btn)}, {SrcKind::Key, static_cast<uint16_t>(dst_btn)}, {}});
        bindings.push_back({{Role::Throttle, SrcKind::Key, static_cast<uint16_t>(src_btn)}, {SrcKind::Key, static_cast<uint16_t>(dst_btn)}, {}});
        bindings.push_back({{Role::Rudder, SrcKind::Key, static_cast<uint16_t>(src_btn)}, {SrcKind::Key, static_cast<uint16_t>(dst_btn)}, {}});
    }
    
    return bindings;
}

std::vector<Binding> make_bindings_from_config(const std::vector<BindingConfigKey>& config_keys, const std::vector<BindingConfigAbs>& config_abs) {
    std::vector<Binding> bindings;
    
    // Convert key bindings
    for (const auto& config_key : config_keys) {
        Role role;
        if (config_key.role == "stick") role = Role::Stick;
        else if (config_key.role == "throttle") role = Role::Throttle;
        else if (config_key.role == "rudder") role = Role::Rudder;
        else continue; // Skip invalid role
        
        bindings.push_back({
            {role, SrcKind::Key, static_cast<uint16_t>(config_key.src)},
            {SrcKind::Key, static_cast<uint16_t>(config_key.dst)},
            {} // Empty AxisTransform for keys
        });
    }
    
    // Convert abs bindings
    for (const auto& config_abs : config_abs) {
        Role role;
        if (config_abs.role == "stick") role = Role::Stick;
        else if (config_abs.role == "throttle") role = Role::Throttle;
        else if (config_abs.role == "rudder") role = Role::Rudder;
        else continue; // Skip invalid role
        
        // Determine output range based on destination axis
        int min_out, max_out;
        switch (config_abs.dst) {
            case ABS_X:
            case ABS_Y:
            case ABS_RX:
            case ABS_RY:
                min_out = -32768;
                max_out = 32767;
                break;
            case ABS_Z:
            case ABS_RZ:
                min_out = 0;
                max_out = 255;
                break;
            case ABS_HAT0X:
            case ABS_HAT0Y:
                min_out = -1;
                max_out = 1;
                break;
            default:
                // Invalid destination axis - skip this binding
                continue;
        }
        
        bindings.push_back({
            {role, SrcKind::Abs, static_cast<uint16_t>(config_abs.src)},
            {SrcKind::Abs, static_cast<uint16_t>(config_abs.dst)},
            {config_abs.invert, config_abs.deadzone, config_abs.scale, min_out, max_out}
        });
    }
    
    return bindings;
}

bool validate_bindings(const std::vector<Binding>& bindings) {
    static const std::unordered_set<uint16_t> valid_buttons = {
        BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH,
        BTN_TL, BTN_TR, BTN_SELECT, BTN_START, BTN_MODE,
        BTN_THUMBL, BTN_THUMBR
    };
    
    static const std::unordered_set<uint16_t> valid_axes = {
        ABS_X, ABS_Y, ABS_RX, ABS_RY, ABS_Z, ABS_RZ, ABS_HAT0X, ABS_HAT0Y
    };
    
    for (const auto& binding : bindings) {
        if (binding.dst.kind == SrcKind::Key) {
            if (valid_buttons.find(binding.dst.code) == valid_buttons.end()) {
                return false;
            }
        } else {
            if (valid_axes.find(binding.dst.code) == valid_axes.end()) {
                return false;
            }
        }
    }
    
    return true;
}