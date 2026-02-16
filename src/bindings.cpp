#include "bindings.hpp"
#include "config.hpp"
#include <algorithm>
#include <unordered_set>
#include <set>
#include <iostream>

#ifdef DEBUG_BINDINGS
bool debug_bindings_enabled = false;
#endif

Role BindingResolver::get_role_priority(const VirtualSlot& dst) {
    return Role::Stick;
}

bool BindingResolver::is_virtual_slot_valid(const VirtualSlot& slot) const {
    static const std::unordered_set<uint16_t> valid_buttons = {
        BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH,
        BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
        BTN_SELECT, BTN_START, BTN_MODE,
        BTN_THUMBL, BTN_THUMBR,
        BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT
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

int BindingResolver::apply_axis_transform(int value, const AxisTransform& xform, Role role, int src_code) const {
    int input_value = value;
    
    // Check if we have calibration data for this axis
    auto role_it = calibrations.find(role);
    if (role_it != calibrations.end()) {
        auto cal_it = role_it->second.find(src_code);
        if (cal_it != role_it->second.end()) {
            const AxisCalibration& cal = cal_it->second;
            
            // TEMP DEBUG: log rudder transform
            if (role == Role::Rudder) {
                static int dbg_count = 0;
                if (dbg_count++ % 100 == 0) {
                    fprintf(stderr, "[RUDDER DBG] input=%d src_code=%d cal: min=%d max=%d center=%d dz=%d xform: min_out=%d max_out=%d invert=%d\n",
                            input_value, src_code, cal.observed_min, cal.observed_max, cal.center_value, cal.deadzone_radius,
                            xform.min_out, xform.max_out, xform.invert);
                }
            }
            
            // Determine axis type: Throttle is always unidirectional, Stick/Rudder are centered
            bool is_centered = (role == Role::Stick || role == Role::Rudder) && 
                               (cal.center_value > cal.observed_min + 10) &&
                               (cal.deadzone_radius > 0);
            
            int output_value;
            if (is_centered) {
                // Two-segment mapping for centered axes (rudder, stick)
                // Apply deadzone and rescale to maintain smooth output
                if (std::abs(input_value - cal.center_value) < cal.deadzone_radius) {
                    // Inside deadzone: output center (0)
                    output_value = 0;
                } else if (input_value < cal.center_value) {
                    // Left half: rescale from [observed_min, center-deadzone] to [min_out, 0]
                    int deadzone_edge = cal.center_value - cal.deadzone_radius;
                    float ratio = (input_value - cal.observed_min) / (float)(deadzone_edge - cal.observed_min);
                    output_value = static_cast<int>(ratio * (0 - xform.min_out) + xform.min_out);
                } else {
                    // Right half: rescale from [center+deadzone, observed_max] to [0, max_out]
                    int deadzone_edge = cal.center_value + cal.deadzone_radius;
                    float ratio = (input_value - deadzone_edge) / (float)(cal.observed_max - deadzone_edge);
                    output_value = static_cast<int>(ratio * xform.max_out);
                }
            } else {
                // Unidirectional mapping for throttle: full range maps to full output range
                // No deadzone for throttle (ARMA needs full precision)
                float ratio = (input_value - cal.observed_min) / (float)(cal.observed_max - cal.observed_min);
                output_value = static_cast<int>(ratio * (xform.max_out - xform.min_out) + xform.min_out);
            }
            
            // Apply inversion if needed
            if (xform.invert) {
                output_value = xform.max_out + xform.min_out - output_value;
            }
            
            // Final clamp to output range only (not input range)
            return std::max(xform.min_out, std::min(xform.max_out, output_value));
        }
    }
    
    // No calibration data - pass through with simple scaling
    if (role == Role::Rudder) {
        fprintf(stderr, "[RUDDER FALLBACK] NO CALIBRATION FOUND! role=%d src_code=%d value=%d\n",
                static_cast<int>(role), src_code, value);
    }
    float ratio = value / 65535.0f;
    if (xform.invert) {
        ratio = 1.0f - ratio;
    }
    int output_value = static_cast<int>(ratio * (xform.max_out - xform.min_out) + xform.min_out);
    return std::max(xform.min_out, std::min(xform.max_out, output_value));
}

void BindingResolver::set_calibration(Role role, int src_code, const AxisCalibration& cal) {
    calibrations[role][src_code] = cal;
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
                int transformed_value = apply_axis_transform(value, binding.xform, input.role, input.code);
                if (input.role == Role::Rudder) {
                    static int pdbg = 0;
                    if (pdbg++ % 50 == 0) {
                        fprintf(stderr, "[RUDDER PROC] raw=%d code=%d -> transformed=%d dst_code=%d\n",
                                value, input.code, transformed_value, binding.dst.code);
                    }
                }
                axis_values[binding.dst][input.role] = transformed_value;
                DEBUG_LOG("Axis value for role %d: %d\n", static_cast<int>(input.role), transformed_value);
            }
        }
    }
}

std::vector<std::pair<VirtualSlot, int>> BindingResolver::get_pending_events() {
    std::vector<std::pair<VirtualSlot, int>> events;
    std::set<VirtualSlot> emitted_slots; // Track to prevent duplicates
    
    auto should_suppress_button_output = [&](const VirtualSlot& slot) {
        // For Xbox-style controllers, triggers should be axes.
        // Some games (including ARMA) may interpret BTN_TL2/BTN_TR2 as system/menu buttons.
        // We still use these internally as "trigger clicks", but we suppress emitting them as EV_KEY.
        return slot.kind == SrcKind::Key && (slot.code == BTN_TL2 || slot.code == BTN_TR2);
    };

    for (const auto& [slot, refcount] : button_refcounts) {
        // Safety: Skip invalid slots
        if (!is_virtual_slot_valid(slot)) {
            DEBUG_LOG("Skipping invalid button slot: kind=%d code=%d\n", static_cast<int>(slot.kind), slot.code);
            continue;
        }

        // Safety: Skip duplicate slots
        if (emitted_slots.count(slot)) {
            DEBUG_LOG("Skipping duplicate button slot: kind=%d code=%d\n", static_cast<int>(slot.kind), slot.code);
            continue;
        }

        int last_value = last_output_values.count(slot) ? last_output_values[slot] : 0;
        int current_value = (refcount > 0) ? 1 : 0;

        if (last_value == current_value) {
            continue;
        }

        // Always update state so we don't repeatedly "re-detect" changes.
        last_output_values[slot] = current_value;

        if (should_suppress_button_output(slot)) {
            continue;
        }

        events.push_back({slot, current_value});
        emitted_slots.insert(slot);
        DEBUG_LOG("Button event: slot=%d value=%d\n", slot.code, current_value);
    }
    
    // Mirror button-style inputs into axis-style outputs for games expecting axes.
    //
    // This is especially important for:
    // - D-pad: some games only read the hat axes (ABS_HAT0X/ABS_HAT0Y)
    // - Triggers: some games only read the analog trigger axes (ABS_Z/ABS_RZ)

    auto get_button_pressed = [&](uint16_t btn_code) -> bool {
        VirtualSlot btn_slot{SrcKind::Key, btn_code};
        return button_refcounts.count(btn_slot) ? (button_refcounts[btn_slot] > 0) : false;
    };

    auto mirror_button_to_axis = [&](uint16_t btn_code, uint16_t axis_code, int pressed_value, int released_value) {
        if (!get_button_pressed(btn_code)) {
            pressed_value = released_value;
        }

        VirtualSlot axis_slot{SrcKind::Abs, axis_code};
        if (!is_virtual_slot_valid(axis_slot)) {
            return;
        }

        // Put mirrored values in the lowest-priority role so they never override
        // any real axis mappings coming from stick/throttle.
        auto& role_values = axis_values[axis_slot];
        role_values[Role::Rudder] = pressed_value;
    };

    // D-pad buttons -> hat axes
    // Linux hat convention: X left=-1 right=+1, Y up=-1 down=+1
    int hat_x = 0;
    if (get_button_pressed(BTN_DPAD_LEFT)) hat_x = -1;
    if (get_button_pressed(BTN_DPAD_RIGHT)) hat_x = 1;

    int hat_y = 0;
    if (get_button_pressed(BTN_DPAD_UP)) hat_y = -1;
    if (get_button_pressed(BTN_DPAD_DOWN)) hat_y = 1;

    {
        VirtualSlot axis_slot{SrcKind::Abs, ABS_HAT0X};
        if (is_virtual_slot_valid(axis_slot)) {
            auto& role_values = axis_values[axis_slot];
            role_values[Role::Rudder] = hat_x;
        }
    }

    {
        VirtualSlot axis_slot{SrcKind::Abs, ABS_HAT0Y};
        if (is_virtual_slot_valid(axis_slot)) {
            auto& role_values = axis_values[axis_slot];
            role_values[Role::Rudder] = hat_y;
        }
    }

    // Trigger click buttons -> trigger axes
    mirror_button_to_axis(BTN_TL2, ABS_Z, 255, 0);
    mirror_button_to_axis(BTN_TR2, ABS_RZ, 255, 0);
    
    for (const auto& [slot, role_values] : axis_values) {
        // Safety: Skip invalid slots
        if (!is_virtual_slot_valid(slot)) {
            DEBUG_LOG("Skipping invalid axis slot: kind=%d code=%d\n", static_cast<int>(slot.kind), slot.code);
            continue;
        }
        
        // Safety: Skip duplicate slots
        if (emitted_slots.count(slot)) {
            DEBUG_LOG("Skipping duplicate axis slot: kind=%d code=%d\n", static_cast<int>(slot.kind), slot.code);
            continue;
        }
        
        Role selected_role = Role::Stick;
        bool has_value = false;
        
        auto role_value = [&](Role role) -> const std::optional<int>* {
            auto it = role_values.find(role);
            if (it == role_values.end()) {
                return nullptr;
            }
            return &it->second;
        };

        // Priority: Stick > Throttle > Rudder
        if (const auto* v = role_value(Role::Stick); v && v->has_value()) {
            selected_role = Role::Stick;
            has_value = true;
        } else if (const auto* v = role_value(Role::Throttle); v && v->has_value()) {
            selected_role = Role::Throttle;
            has_value = true;
        } else if (const auto* v = role_value(Role::Rudder); v && v->has_value()) {
            selected_role = Role::Rudder;
            has_value = true;
        }

        int current_value = 0;
        if (has_value) {
            if (const auto* v = role_value(selected_role); v && v->has_value()) {
                current_value = **v;
            }
        }
        int last_value = last_output_values.count(slot) ? last_output_values[slot] : 0;
        
        if (last_value != current_value) {
            events.push_back({slot, current_value});
            last_output_values[slot] = current_value;
            emitted_slots.insert(slot);
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
    // Organized: Primary heli controls first, then triggers/auxiliary buttons
    const std::map<int, int> button_mappings = {
        // Primary heli controls (face buttons, shoulders, system)
        {BTN_TRIGGER, BTN_SOUTH},      // Primary trigger -> South button
        {BTN_THUMB, BTN_EAST},          // Thumb button -> East button
        {BTN_THUMB2, BTN_NORTH},        // Thumb 2 -> X button (left)
        {BTN_TOP, BTN_WEST},            // Top button -> Y button (top)
        {BTN_TOP2, BTN_TL},             // Top 2 -> Left shoulder
        {BTN_PINKIE, BTN_TR},           // Pinkie -> Right shoulder
        {BTN_BASE, BTN_SELECT},         // Base -> Select
        {BTN_BASE2, BTN_START},         // Base 2 -> Start
        
        // Triggers and auxiliary controls
        {BTN_BASE3, BTN_THUMBL},        // Base 3 -> Left stick button
        {BTN_BASE4, BTN_THUMBR},        // Base 4 -> Right stick button
        {BTN_BASE5, BTN_TL2},           // Base 5 -> Left trigger click
        {BTN_BASE6, BTN_TR2}            // Base 6 -> Right trigger click
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
            case ABS_RX:
                // Centered axes for stick X (yaw/anti-torque)
                min_out = -32768;
                max_out = 32767;
                break;
            case ABS_Y:
            case ABS_RY:
                // Full range for all roles
                // Throttle->ABS_Y (collective) needs full range for 0-100% in ARMA
                // Stick->ABS_Y/RY (pitch/roll) also needs full range
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
        BTN_TL, BTN_TR, BTN_TL2, BTN_TR2,
        BTN_SELECT, BTN_START, BTN_MODE,
        BTN_THUMBL, BTN_THUMBR,
        BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT
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