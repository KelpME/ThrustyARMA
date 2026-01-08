#include "config.hpp"
#include "bindings.hpp"
#include <ncurses.h>
#include <fcntl.h>
#include <unistd.h>
#include <string>
#include <vector>
#include <optional>
#include <algorithm>
#include <cstring>
#include <linux/input.h>
#include <linux/input-event-codes.h>

enum class InputType {
    KEY,
    ABS
};

struct ConfigVirtualSlot {
    int code;
    InputType type;
    std::string name;
};

struct SlotLabel {
    const char* short_name;
    const char* long_name;
};

static const std::map<std::pair<int, InputType>, SlotLabel> SLOT_LABELS = {
    // Axes
    {{ABS_X, InputType::ABS}, {"LS X", "Left Stick X"}},
    {{ABS_Y, InputType::ABS}, {"LS Y", "Left Stick Y"}},
    {{ABS_RX, InputType::ABS}, {"RS X", "Right Stick X"}},
    {{ABS_RY, InputType::ABS}, {"RS Y", "Right Stick Y"}},
    {{ABS_Z, InputType::ABS}, {"LT", "Left Trigger"}},
    {{ABS_RZ, InputType::ABS}, {"RT", "Right Trigger"}},
    {{ABS_HAT0X, InputType::ABS}, {"DPad X", "D-Pad X"}},
    {{ABS_HAT0Y, InputType::ABS}, {"DPad Y", "D-Pad Y"}},
    
    // Buttons
    {{BTN_SOUTH, InputType::KEY}, {"A", "A Button"}},
    {{BTN_EAST, InputType::KEY}, {"B", "B Button"}},
    {{BTN_WEST, InputType::KEY}, {"X", "X Button"}},
    {{BTN_NORTH, InputType::KEY}, {"Y", "Y Button"}},
    {{BTN_TL, InputType::KEY}, {"LB", "Left Bumper"}},
    {{BTN_TR, InputType::KEY}, {"RB", "Right Bumper"}},
    {{BTN_SELECT, InputType::KEY}, {"Back", "Back / Select"}},
    {{BTN_START, InputType::KEY}, {"Start", "Start"}},
    {{BTN_MODE, InputType::KEY}, {"Guide", "Guide"}},
    {{BTN_THUMBL, InputType::KEY}, {"L3", "Left Stick Click (L3)"}},
    {{BTN_THUMBR, InputType::KEY}, {"R3", "Right Stick Click (R3)"}}
};

std::string get_slot_label(int code, InputType type, bool use_long = false) {
    auto it = SLOT_LABELS.find({code, type});
    if (it != SLOT_LABELS.end()) {
        return use_long ? it->second.long_name : it->second.short_name;
    }
    return "Unknown Slot";
}

std::string get_slot_display_name(int code, InputType type, const std::string& kernel_name) {
    std::string label = get_slot_label(code, type, true);
    return label + " (" + kernel_name + ")";
}

struct UIState {
    int selected_slot = 0;
    int selected_binding = 0;
    int slot_scroll = 0;
    bool show_invalid = false;
    bool unsaved_changes = false;
    
    Config config;
    std::vector<ConfigVirtualSlot> slots;
    std::vector<BindingConfigKey> invalid_keys;
    std::vector<BindingConfigAbs> invalid_abs;
};

const std::vector<ConfigVirtualSlot> VALID_SLOTS = {
    {ABS_X, InputType::ABS, "ABS_X"},
    {ABS_Y, InputType::ABS, "ABS_Y"},
    {ABS_RX, InputType::ABS, "ABS_RX"},
    {ABS_RY, InputType::ABS, "ABS_RY"},
    {ABS_Z, InputType::ABS, "ABS_Z"},
    {ABS_RZ, InputType::ABS, "ABS_RZ"},
    {ABS_HAT0X, InputType::ABS, "ABS_HAT0X"},
    {ABS_HAT0Y, InputType::ABS, "ABS_HAT0Y"},
    {BTN_SOUTH, InputType::KEY, "BTN_SOUTH"},
    {BTN_EAST, InputType::KEY, "BTN_EAST"},
    {BTN_WEST, InputType::KEY, "BTN_WEST"},
    {BTN_NORTH, InputType::KEY, "BTN_NORTH"},
    {BTN_TL, InputType::KEY, "BTN_TL"},
    {BTN_TR, InputType::KEY, "BTN_TR"},
    {BTN_TL2, InputType::KEY, "BTN_TL2"},
    {BTN_TR2, InputType::KEY, "BTN_TR2"},
    {BTN_SELECT, InputType::KEY, "BTN_SELECT"},
    {BTN_START, InputType::KEY, "BTN_START"},
    {BTN_MODE, InputType::KEY, "BTN_MODE"},
    {BTN_THUMBL, InputType::KEY, "BTN_THUMBL"},
    {BTN_THUMBR, InputType::KEY, "BTN_THUMBR"},
    {BTN_DPAD_UP, InputType::KEY, "BTN_DPAD_UP"},
    {BTN_DPAD_DOWN, InputType::KEY, "BTN_DPAD_DOWN"},
    {BTN_DPAD_LEFT, InputType::KEY, "BTN_DPAD_LEFT"},
    {BTN_DPAD_RIGHT, InputType::KEY, "BTN_DPAD_RIGHT"}
};

std::string get_config_path() {
    const char* home = getenv("HOME");
    if (!home) return "";
    return std::string(home) + "/.config/twcs-mapper/config.json";
}

std::optional<std::string> get_device_path(const Config& config, const std::string& role) {
    for (const auto& input : config.inputs) {
        if (input.role == role) {
            return input.by_id;
        }
    }
    return std::nullopt;
}

bool is_valid_dst(int code) {
    return std::any_of(VALID_SLOTS.begin(), VALID_SLOTS.end(),
                      [code](const ConfigVirtualSlot& slot) { return slot.code == code; });
}

void load_invalid_bindings(UIState& state) {
    state.invalid_keys.clear();
    state.invalid_abs.clear();
    
    for (const auto& binding : state.config.bindings_keys) {
        if (!is_valid_dst(binding.dst)) {
            state.invalid_keys.push_back(binding);
        }
    }
    
    for (const auto& binding : state.config.bindings_abs) {
        if (!is_valid_dst(binding.dst)) {
            state.invalid_abs.push_back(binding);
        }
    }
}

std::vector<BindingConfigKey> get_bindings_for_slot(const UIState& state, int slot_code) {
    std::vector<BindingConfigKey> result;
    for (const auto& binding : state.config.bindings_keys) {
        if (binding.dst == slot_code) {
            result.push_back(binding);
        }
    }
    return result;
}

std::vector<BindingConfigAbs> get_abs_bindings_for_slot(const UIState& state, int slot_code) {
    std::vector<BindingConfigAbs> result;
    for (const auto& binding : state.config.bindings_abs) {
        if (binding.dst == slot_code) {
            result.push_back(binding);
        }
    }
    return result;
}

void draw_ui(UIState& state) {
    clear();
    
    // Title
    attron(A_BOLD);
    mvprintw(0, 2, "TWCS Mapper Configuration Tool");
    attroff(A_BOLD);
    
    if (state.unsaved_changes) {
        mvprintw(0, 40, "[MODIFIED]");
    }
    
    // Virtual slots list
    mvprintw(2, 2, "Virtual Slots:");
    mvprintw(3, 2, "--------------");
    
    // Calculate visible slots (leave room for help at bottom)
    int max_visible_slots = LINES - 9;
    int slot_start = 4;
    
    // Update scroll position if needed
    if (state.selected_slot >= state.slot_scroll + max_visible_slots) {
        state.slot_scroll = state.selected_slot - max_visible_slots + 1;
    } else if (state.selected_slot < state.slot_scroll) {
        state.slot_scroll = state.selected_slot;
    }
    
    // Draw visible slots
    int end_slot = std::min(static_cast<int>(VALID_SLOTS.size()), state.slot_scroll + max_visible_slots);
    for (int i = state.slot_scroll; i < end_slot; i++) {
        const auto& slot = VALID_SLOTS[i];
        int display_line = slot_start + (i - state.slot_scroll);
        
        if (i == state.selected_slot) {
            attron(A_REVERSE);
        }
        std::string display_name = get_slot_display_name(slot.code, slot.type, slot.name);
        mvprintw(display_line, 4, "%-25s (%s)", display_name.c_str(), 
                 slot.type == InputType::KEY ? "KEY" : "ABS");
        
        // Show bindings inline for this slot
        auto key_bindings = get_bindings_for_slot(state, slot.code);
        auto abs_bindings = get_bindings_for_slot(state, slot.code);
        if (!key_bindings.empty() || !abs_bindings.empty()) {
            std::string binding_info;
            for (size_t j = 0; j < key_bindings.size(); j++) {
                if (j > 0) binding_info += ", ";
                binding_info += key_bindings[j].role;
            }
            for (size_t j = 0; j < abs_bindings.size(); j++) {
                if (!binding_info.empty()) binding_info += ", ";
                binding_info += abs_bindings[j].role;
            }
            mvprintw(display_line, 35, "[%s]", binding_info.c_str());
        }
        
        if (i == state.selected_slot) {
            attroff(A_REVERSE);
        }
    }
    
    // Show scroll indicator if needed
    if (VALID_SLOTS.size() > static_cast<size_t>(max_visible_slots)) {
        mvprintw(slot_start + max_visible_slots, 4, "[%d-%d of %zu]", 
                 state.slot_scroll + 1, end_slot, VALID_SLOTS.size());
    }
    
    // Detailed bindings for selected slot (right panel)
    int bindings_x = 50;
    int bindings_y = 4;
    
    const auto& selected_slot = VALID_SLOTS[state.selected_slot];
    std::string slot_display = get_slot_display_name(selected_slot.code, selected_slot.type, selected_slot.name);
    mvprintw(2, bindings_x, "Bindings for %s:", slot_display.c_str());
    mvprintw(3, bindings_x, "----------------------------");
    
    auto key_bindings = get_bindings_for_slot(state, selected_slot.code);
    auto abs_bindings = get_abs_bindings_for_slot(state, selected_slot.code);
    
    int binding_idx = 0;
    
    for (size_t i = 0; i < key_bindings.size(); i++) {
        const auto& binding = key_bindings[i];
        if (binding_idx == state.selected_binding) {
            attron(A_REVERSE);
        }
        mvprintw(bindings_y + binding_idx, bindings_x, "KEY  %-6s -> %d (src=%d)", 
                 binding.role.c_str(), binding.dst, binding.src);
        if (binding_idx == state.selected_binding) {
            attroff(A_REVERSE);
        }
        binding_idx++;
    }
    
    for (size_t i = 0; i < abs_bindings.size(); i++) {
        const auto& binding = abs_bindings[i];
        if (binding_idx == state.selected_binding) {
            attron(A_REVERSE);
        }
        mvprintw(bindings_y + binding_idx, bindings_x, "ABS  %-6s -> %d (src=%d inv=%s dz=%d s=%.1f)", 
                 binding.role.c_str(), binding.dst, binding.src,
                 binding.invert ? "Y" : "N", binding.deadzone, binding.scale);
        if (binding_idx == state.selected_binding) {
            attroff(A_REVERSE);
        }
        binding_idx++;
    }
    
    if (binding_idx == 0) {
        mvprintw(bindings_y, bindings_x, "No bindings");
    }
    
    // Invalid bindings section
    if (!state.invalid_keys.empty() || !state.invalid_abs.empty()) {
        int invalid_y = bindings_y + 10;
        mvprintw(invalid_y, 2, "Invalid Bindings:");
        mvprintw(invalid_y + 1, 2, "------------------");
        
        for (size_t i = 0; i < state.invalid_keys.size(); i++) {
            mvprintw(invalid_y + 2 + i, 4, "KEY  %-6s -> %d (src=%d) [INVALID DST]", 
                     state.invalid_keys[i].role.c_str(), state.invalid_keys[i].dst, state.invalid_keys[i].src);
        }
        
        for (size_t i = 0; i < state.invalid_abs.size(); i++) {
            mvprintw(invalid_y + 2 + state.invalid_keys.size() + i, 4, "ABS  %-6s -> %d (src=%d) [INVALID DST]", 
                     state.invalid_abs[i].role.c_str(), state.invalid_abs[i].dst, state.invalid_abs[i].src);
        }
    }
    
    // Help
    int help_start = LINES - 8;
    mvprintw(help_start, 2, "Keys:");
    mvprintw(help_start + 1, 2, "  Up/Down or j/k  Move selection");
    mvprintw(help_start + 2, 2, "  Enter       Select slot / binding");
    mvprintw(help_start + 3, 2, "  a           Add binding");
    mvprintw(help_start + 4, 2, "  d           Delete binding");
    mvprintw(help_start + 5, 2, "  s           Save");
    mvprintw(help_start + 6, 2, "  q           Quit");
    mvprintw(help_start + 7, 2, "  i           Toggle invalid bindings");
}

bool select_role_dialog(std::string& role) {
    const std::vector<std::string> roles = {"stick", "throttle", "rudder"};
    int selected = 0;
    
    while (true) {
        clear();
        mvprintw(0, 0, "Select role for binding:");
        mvprintw(2, 0, "");
        for (size_t i = 0; i < roles.size(); i++) {
            if (i == static_cast<size_t>(selected)) {
                attron(A_REVERSE);
            }
            printw("%s", roles[i].c_str());
            if (i == static_cast<size_t>(selected)) {
                attroff(A_REVERSE);
            }
            printw("\n");
        }
        mvprintw(10, 0, "Enter to select, q to cancel");
        
        int ch = getch();
        switch (ch) {
            case KEY_UP:
            case 'k':
                selected = (selected - 1 + roles.size()) % roles.size();
                break;
            case KEY_DOWN:
            case 'j':
                selected = (selected + 1) % roles.size();
                break;
            case '\n':
            case '\r':
                role = roles[selected];
                return true;
            case 'q':
            case 'Q':
                return false;
        }
    }
}

bool confirm_quit_dialog() {
    clear();
    mvprintw(0, 0, "You have unsaved changes. Quit anyway?");
    mvprintw(2, 0, "y = Yes, quit without saving");
    mvprintw(3, 0, "n = No, continue editing");
    
    while (true) {
        int ch = getch();
        switch (ch) {
            case 'y':
            case 'Y':
                return true;
            case 'n':
            case 'N':
            case 27: // ESC
                return false;
        }
    }
}

bool confirm_delete_dialog() {
    clear();
    mvprintw(0, 0, "Delete selected binding?");
    mvprintw(2, 0, "y = Yes, delete binding");
    mvprintw(3, 0, "n = No, keep binding");
    
    while (true) {
        int ch = getch();
        switch (ch) {
            case 'y':
            case 'Y':
                return true;
            case 'n':
            case 'N':
            case 27: // ESC
                return false;
        }
    }
}

int main() {
    std::string config_path = get_config_path();
    if (config_path.empty()) {
        fprintf(stderr, "Could not determine config path\n");
        return 1;
    }
    
    UIState state;
    state.slots = VALID_SLOTS;
    
    auto config = ConfigManager::load(config_path);
    if (!config) {
        // Start with empty config
        state.config = Config{};
    } else {
        state.config = *config;
    }
    
    load_invalid_bindings(state);
    
    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);
    
    bool running = true;
    
    while (running) {
        draw_ui(state);
        
        int ch = getch();
        
        switch (ch) {
            case KEY_UP:
            case 'k':
                if (state.selected_slot > 0) {
                    state.selected_slot--;
                    state.selected_binding = 0;
                }
                break;
                
            case KEY_DOWN:
            case 'j':
                if (state.selected_slot < static_cast<int>(VALID_SLOTS.size()) - 1) {
                    state.selected_slot++;
                    state.selected_binding = 0;
                }
                break;
                
            case KEY_LEFT:
                if (state.selected_binding > 0) {
                    state.selected_binding--;
                }
                break;
                
            case KEY_RIGHT: {
                const auto& slot = VALID_SLOTS[state.selected_slot];
                auto key_bindings = get_bindings_for_slot(state, slot.code);
                auto abs_bindings = get_abs_bindings_for_slot(state, slot.code);
                int total_bindings = key_bindings.size() + abs_bindings.size();
                
                if (state.selected_binding < total_bindings - 1) {
                    state.selected_binding++;
                }
                break;
            }
                
            case 'a': {
                std::string role;
                if (select_role_dialog(role)) {
                    const auto& slot = VALID_SLOTS[state.selected_slot];
                    
                    // Create a placeholder binding that user can manually edit
                    if (slot.type == InputType::KEY) {
                        BindingConfigKey binding;
                        binding.role = role;
                        binding.src = 0; // Placeholder - user should edit config manually
                        binding.dst = slot.code;
                        state.config.bindings_keys.push_back(binding);
                    } else {
                        BindingConfigAbs binding;
                        binding.role = role;
                        binding.src = 0; // Placeholder - user should edit config manually
                        binding.dst = slot.code;
                        binding.invert = false;
                        binding.deadzone = 0;
                        binding.scale = 1.0f;
                        state.config.bindings_abs.push_back(binding);
                    }
                    
                    state.unsaved_changes = true;
                    
                    // Show info message
                    clear();
                    mvprintw(0, 0, "Binding added with placeholder source code (0)");
                    mvprintw(1, 0, "Edit config.json manually to set the correct source code");
                    mvprintw(3, 0, "Press any key to continue...");
                    getch();
                }
                break;
            }
                
            case 'd': {
                const auto& slot = VALID_SLOTS[state.selected_slot];
                auto key_bindings = get_bindings_for_slot(state, slot.code);
                auto abs_bindings = get_abs_bindings_for_slot(state, slot.code);
                int total_bindings = key_bindings.size() + abs_bindings.size();
                
                if (state.selected_binding < total_bindings && state.selected_binding >= 0) {
                    if (confirm_delete_dialog()) {
                        if (state.selected_binding < static_cast<int>(key_bindings.size())) {
                            // Delete key binding
                            auto it = state.config.bindings_keys.begin();
                            for (const auto& binding : key_bindings) {
                                if (it->role == binding.role && it->src == binding.src && it->dst == binding.dst) {
                                    state.config.bindings_keys.erase(it);
                                    state.unsaved_changes = true;
                                    break;
                                }
                                ++it;
                            }
                        } else {
                            // Delete abs binding
                            int abs_index = state.selected_binding - key_bindings.size();
                            auto it = state.config.bindings_abs.begin();
                            for (size_t i = 0; i < state.config.bindings_abs.size(); i++) {
                                if (i == static_cast<size_t>(abs_index)) {
                                    state.config.bindings_abs.erase(it);
                                    state.unsaved_changes = true;
                                    break;
                                }
                                ++it;
                            }
                        }
                        
                        if (state.selected_binding >= total_bindings - 1) {
                            state.selected_binding = std::max(0, total_bindings - 2);
                        }
                    }
                }
                break;
            }
                
            case 's':
                if (ConfigManager::save(config_path, state.config)) {
                    state.unsaved_changes = false;
                    load_invalid_bindings(state);
                    
                    // Show success message
                    clear();
                    mvprintw(0, 0, "Configuration saved successfully!");
                    mvprintw(2, 0, "Press any key to continue...");
                    getch();
                } else {
                    // Show error message
                    clear();
                    mvprintw(0, 0, "Failed to save configuration!");
                    mvprintw(2, 0, "Press any key to continue...");
                    getch();
                }
                break;
                
            case 'q':
                if (state.unsaved_changes) {
                    if (confirm_quit_dialog()) {
                        running = false;
                    }
                } else {
                    running = false;
                }
                break;
                
            case 'i':
                state.show_invalid = !state.show_invalid;
                break;
                
            case '?':
            case 'h':
                clear();
                mvprintw(0, 0, "TWCS Mapper Configuration Tool - Help");
                mvprintw(2, 0, "This tool allows you to manage device bindings for the TWCS mapper.");
                mvprintw(4, 0, "Controls:");
                mvprintw(5, 0, "  Up/Down or j/k - Select virtual slot");
                mvprintw(6, 0, "  Left/Right     - Select binding within slot");
                mvprintw(7, 0, "  a              - Add new binding (creates placeholder)");
                mvprintw(8, 0, "  d              - Delete selected binding");
                mvprintw(9, 0, "  s              - Save configuration");
                mvprintw(10, 0, "  q              - Quit");
                mvprintw(11, 0, "  i              - Toggle invalid bindings view");
                mvprintw(12, 0, "  ?/h            - Show this help");
                mvprintw(14, 0, "Note: This creates basic bindings. For advanced mappings,");
                mvprintw(15, 0, "      edit config.json manually or use external tools.");
                mvprintw(17, 0, "Press any key to continue...");
                getch();
                break;
        }
    }
    
    endwin();
    return 0;
}