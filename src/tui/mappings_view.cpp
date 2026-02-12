#include "mappings_view.hpp"

int MappingsView::count_sources(int code, SrcKind kind) {
    int count = 0;
    for (auto& bd : tui->get_bindings()) {
        if (bd.virtual_kind == kind && bd.virtual_code == code) count++;
    }
    return count;
}

BindingDisplay* MappingsView::find_first_binding(int code, SrcKind kind) {
    for (auto& bd : tui->get_bindings()) {
        if (bd.virtual_kind == kind && bd.virtual_code == code) return &bd;
    }
    return nullptr;
}

std::vector<MappingsView::DisplayRow> MappingsView::build_display_list() {
    std::vector<DisplayRow> rows;
    
    // Helper: simple single-type control row
    auto add_simple = [&](const std::string& name, int code, SrcKind kind) {
        int cnt = count_sources(code, kind);
        BindingDisplay* bd = find_first_binding(code, kind);
        rows.push_back({false, "", name, code, kind, -1, kind, bd, cnt});
    };
    
    // Helper: merged control row (axis + button behind one row)
    auto add_merged = [&](const std::string& name, int axis_code, int btn_code) {
        int cnt_abs = count_sources(axis_code, SrcKind::Abs);
        int cnt_key = count_sources(btn_code, SrcKind::Key);
        int total = cnt_abs + cnt_key;
        BindingDisplay* bd = find_first_binding(axis_code, SrcKind::Abs);
        if (!bd) bd = find_first_binding(btn_code, SrcKind::Key);
        rows.push_back({false, "", name, axis_code, SrcKind::Abs, btn_code, SrcKind::Key, bd, total});
    };
    
    auto add_header = [&](const std::string& text) {
        rows.push_back({true, text, "", 0, SrcKind::Abs, -1, SrcKind::Abs, nullptr, 0});
    };
    
    // -- Sticks --
    add_header("-- Sticks --");
    add_simple("Left Stick X", ABS_X, SrcKind::Abs);
    add_simple("Left Stick Y", ABS_Y, SrcKind::Abs);
    add_simple("Right Stick X", ABS_RX, SrcKind::Abs);
    add_simple("Right Stick Y", ABS_RY, SrcKind::Abs);
    
    // -- Triggers (merged: axis + button behind one row each) --
    add_header("-- Triggers --");
    add_merged("Left Trigger", ABS_Z, BTN_TL2);
    add_merged("Right Trigger", ABS_RZ, BTN_TR2);
    
    // -- D-Pad (individual button rows + hat axes merged per axis) --
    add_header("-- D-Pad --");
    add_simple("D-Pad Up", BTN_DPAD_UP, SrcKind::Key);
    add_simple("D-Pad Down", BTN_DPAD_DOWN, SrcKind::Key);
    add_simple("D-Pad Left", BTN_DPAD_LEFT, SrcKind::Key);
    add_simple("D-Pad Right", BTN_DPAD_RIGHT, SrcKind::Key);
    
    // -- Buttons --
    add_header("-- Buttons --");
    add_simple("A (South)", BTN_SOUTH, SrcKind::Key);
    add_simple("B (East)", BTN_EAST, SrcKind::Key);
    add_simple("X (West)", BTN_WEST, SrcKind::Key);
    add_simple("Y (North)", BTN_NORTH, SrcKind::Key);
    add_simple("Left Shoulder", BTN_TL, SrcKind::Key);
    add_simple("Right Shoulder", BTN_TR, SrcKind::Key);
    add_simple("Select", BTN_SELECT, SrcKind::Key);
    add_simple("Start", BTN_START, SrcKind::Key);
    add_simple("Menu", BTN_MODE, SrcKind::Key);
    add_simple("Left Stick", BTN_THUMBL, SrcKind::Key);
    add_simple("Right Stick", BTN_THUMBR, SrcKind::Key);
    
    return rows;
}

MappingsView::MappingsView(TUI* parent) : View(parent, ViewType::MAPPINGS), 
                             scroll_offset(0), selected_binding(1) {}

void MappingsView::draw() {
    if (!needs_redraw) return;
    
    auto* main_win = tui->get_main_win();
    int height = main_win->get_height();
    int width = main_win->get_width();
    auto display = build_display_list();
    
    main_win->clear();
    
    // Title
    main_win->print(1, 2, "Virtual Controller Mappings", COLOR_PAIR(CP_HEADER) | A_BOLD);
    main_win->print(2, 2, "(Xbox 360 Controller Layout)", COLOR_PAIR(CP_DEFAULT) | A_DIM);
    
    // Dynamic column layout
    int usable = width - 4;
    int col_slot = 2;
    int col_source = col_slot + std::max(22, usable * 30 / 100);
    int col_xform = col_source + std::max(24, usable * 35 / 100);
    int slot_width = col_source - col_slot - 2;
    int source_width = col_xform - col_source - 2;
    
    // Column headers
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "%-*s%-*s%s",
             col_source - col_slot, "Xbox Control",
             col_xform - col_source, "Source",
             "Settings");
    main_win->print(4, 2, hdr, A_BOLD);
    main_win->print(5, 2, std::string(width - 4, '-'));
    
    // Unified binding list
    int row = 6;
    for (size_t i = scroll_offset; i < display.size() && row < height - 4; i++) {
        const auto& drow = display[i];
        
        if (drow.is_header) {
            // Section header
            main_win->print(row, 2, drow.header_text, COLOR_PAIR(CP_HEADER) | A_BOLD);
        } else {
            int attrs = (static_cast<int>(i) == selected_binding) ? COLOR_PAIR(CP_SELECTED) : 0;
            if (attrs) wattron(main_win->get(), attrs);
            
            // Control name from DisplayRow
            std::string slot_name = drow.display_name;
            if (static_cast<int>(slot_name.length()) > slot_width)
                slot_name = slot_name.substr(0, slot_width - 3) + "...";
            mvwprintw(main_win->get(), row, col_slot + 2, "%-*s", col_source - col_slot - 2, slot_name.c_str());
            
            if (drow.source_count == 0) {
                // Unmapped control
                if (!attrs) wattron(main_win->get(), A_DIM);
                mvwprintw(main_win->get(), row, col_source, "(unmapped)");
                if (!attrs) wattroff(main_win->get(), A_DIM);
            } else {
                const auto* bd = drow.binding;
                
                // Source summary
                std::string source;
                if (drow.source_count == 1) {
                    source = bd->source_role + " : " + bd->source_name;
                } else {
                    source = bd->source_role + " : " + bd->source_name +
                             " (+" + std::to_string(drow.source_count - 1) + " more)";
                }
                if (static_cast<int>(source.length()) > source_width)
                    source = source.substr(0, source_width - 3) + "...";
                mvwprintw(main_win->get(), row, col_source, "%-*s", col_xform - col_source, source.c_str());
                
                // Settings (only for single-source axis bindings)
                if (drow.alt_code == -1 && drow.virtual_kind == SrcKind::Abs && drow.source_count == 1) {
                    std::string info;
                    if (bd->invert) info += "Inverted";
                    if (bd->scale != 1.0f) {
                        if (!info.empty()) info += ", ";
                        std::ostringstream ss;
                        ss << "Scale: " << bd->scale << "x";
                        info += ss.str();
                    }
                    mvwprintw(main_win->get(), row, col_xform, "%s", info.c_str());
                }
            }
            
            if (attrs) wattroff(main_win->get(), attrs);
        }
        
        row++;
    }
    
    // Actions
    main_win->print(height - 3, 2, "Actions:", COLOR_PAIR(CP_HEADER) | A_BOLD);
    main_win->print(height - 2, 4, "[a] Add  [e] Edit  [d] Delete");
    
    main_win->refresh();
    needs_redraw = false;
}

void MappingsView::handle_input(int ch) {
    auto display = build_display_list();
    int total = static_cast<int>(display.size());
    
    // Helper to skip section headers when navigating
    auto find_selectable = [&](int from, int dir) -> int {
        int pos = from + dir;
        while (pos >= 0 && pos < total) {
            if (!display[pos].is_header) return pos;
            pos += dir;
        }
        return from;
    };
    
    switch (ch) {
        case KEY_UP:
        case 'k':
            if (selected_binding > 0) {
                selected_binding = find_selectable(selected_binding, -1);
                if (selected_binding < scroll_offset) scroll_offset = selected_binding;
                needs_redraw = true;
            }
            break;
            
        case KEY_DOWN:
        case 'j':
            if (selected_binding < total - 1) {
                selected_binding = find_selectable(selected_binding, 1);
                int visible = tui->get_screen_height() - 14;
                if (selected_binding >= scroll_offset + visible) {
                    scroll_offset = selected_binding - visible + 1;
                }
                needs_redraw = true;
            }
            break;
            
        case 'a':
        case 'A':
            show_add_binding_dialog();
            break;
            
        case 'e':
        case 'E':
            show_edit_binding_dialog();
            break;
            
        case 'd':
        case 'D':
            delete_selected_binding();
            break;
    }
}

std::string MappingsView::get_slot_name(int code, SrcKind kind) {
    if (kind == SrcKind::Abs) {
        switch (code) {
            case ABS_X: return "Left Stick X";
            case ABS_Y: return "Left Stick Y";
            case ABS_RX: return "Right Stick X";
            case ABS_RY: return "Right Stick Y";
            case ABS_Z: return "Left Trigger";
            case ABS_RZ: return "Right Trigger";
            case ABS_HAT0X: return "D-Pad X";
            case ABS_HAT0Y: return "D-Pad Y";
            default: return "ABS_" + std::to_string(code);
        }
    } else {
        switch (code) {
            case BTN_SOUTH: return "A (South)";
            case BTN_EAST: return "B (East)";
            case BTN_WEST: return "X (West)";
            case BTN_NORTH: return "Y (North)";
            case BTN_TL: return "Left Shoulder";
            case BTN_TR: return "Right Shoulder";
            case BTN_TL2: return "Left Trigger";
            case BTN_TR2: return "Right Trigger";
            case BTN_SELECT: return "Select";
            case BTN_START: return "Start";
            case BTN_MODE: return "Menu";
            case BTN_THUMBL: return "Left Stick";
            case BTN_THUMBR: return "Right Stick";
            case BTN_DPAD_UP: return "D-Pad Up";
            case BTN_DPAD_DOWN: return "D-Pad Down";
            case BTN_DPAD_LEFT: return "D-Pad Left";
            case BTN_DPAD_RIGHT: return "D-Pad Right";
            default: return "BTN_" + std::to_string(code);
        }
    }
}

void MappingsView::show_add_binding_dialog() {
    auto display = build_display_list();
    
    if (selected_binding < 0 || selected_binding >= static_cast<int>(display.size())) return;
    if (display[selected_binding].is_header) return;
    
    const auto& drow = display[selected_binding];
    
    // Merged control (e.g. triggers): listen for any input, auto-detect type
    if (drow.alt_code != -1) {
        show_add_merged_listen_dialog(drow);
        return;
    }
    
    // Simple control
    if (drow.virtual_kind == SrcKind::Abs) {
        show_add_axis_listen_dialog(drow.virtual_code);
    } else {
        show_add_button_listen_dialog(drow.virtual_code);
    }
}

void MappingsView::show_add_axis_listen_dialog(int dst_code) {
    auto& config = tui->get_config();
    
    std::string dst_name = get_slot_name(dst_code, SrcKind::Abs);
    
    int h = 10, w = 52;
    int starty = (tui->get_screen_height() - h) / 2;
    int startx = (tui->get_screen_width() - w) / 2;
    
    // Stop mapper so we can read from devices it had grabbed
    bool mapper_was_running = stop_mapper_service();
    if (mapper_was_running) {
        tui->scan_devices();
    }
    auto& devices = tui->get_devices();
    
    // Record baseline axis values so we can detect significant movement
    struct AxisBaseline {
        std::shared_ptr<DeviceInfo> dev;
        int code;
        int baseline;
        int threshold;
    };
    std::vector<AxisBaseline> baselines;
    
    // Drain pending events and record current positions (only assigned devices)
    for (const auto& dev : devices) {
        if (!dev->online || !dev->dev || dev->roles.empty()) continue;
        struct input_event ev;
        while (libevdev_next_event(dev->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS) {}
        for (int axis : dev->axes) {
            int val = libevdev_get_event_value(dev->dev, EV_ABS, axis);
            // Use 25% of axis range as threshold
            const struct input_absinfo* info = libevdev_get_abs_info(dev->dev, axis);
            int range = info ? (info->maximum - info->minimum) : 65535;
            int thresh = std::max(range / 4, 50);
            baselines.push_back({dev, axis, val, thresh});
        }
    }
    
    nodelay(stdscr, TRUE);
    
    bool captured = false;
    std::string captured_role;
    int captured_code = 0;
    std::string captured_name;
    
    while (!captured) {
        Window dialog(h, w, starty, startx, " Add to " + dst_name + " ");
        dialog.print(2, 2, "Move an axis on any device...", COLOR_PAIR(CP_HEADER) | A_BOLD);
        dialog.print(4, 2, "The axis will be mapped to:", A_DIM);
        dialog.print(5, 4, dst_name, COLOR_PAIR(CP_SUCCESS) | A_BOLD);
        dialog.print(h - 2, 2, "[ESC] Cancel", A_DIM);
        dialog.refresh();
        
        int ch = getch();
        if (ch == 27) {
            nodelay(stdscr, TRUE);
            needs_redraw = true;
            return;
        }
        
        // Poll assigned devices for axis movement
        for (const auto& dev : devices) {
            if (!dev->online || !dev->dev || dev->roles.empty()) continue;
            struct input_event ev;
            int rc = libevdev_next_event(dev->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            while (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
                if (ev.type == EV_ABS) {
                    // Check if this axis moved significantly from baseline
                    for (const auto& bl : baselines) {
                        if (bl.dev == dev && bl.code == ev.code) {
                            int delta = abs(ev.value - bl.baseline);
                            if (delta > bl.threshold) {
                                captured_code = ev.code;
                                const char* abs_name = libevdev_event_code_get_name(EV_ABS, ev.code);
                                captured_name = abs_name ? abs_name : ("ABS_" + std::to_string(ev.code));
                                if (!dev->roles.empty()) {
                                    captured_role = dev->roles[0];
                                }
                                captured = true;
                            }
                            break;
                        }
                    }
                    if (captured) break;
                }
                rc = libevdev_next_event(dev->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            }
            if (captured) break;
        }
        
        if (!captured) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    
    nodelay(stdscr, TRUE);
    
    // Check for duplicate
    auto active_profile = config.active_profile;
    if (config.profiles.count(active_profile)) {
        auto& profile = config.profiles[active_profile];
        bool duplicate = false;
        for (const auto& ab : profile.bindings_abs) {
            if (ab.role == captured_role && ab.src == captured_code && ab.dst == dst_code) {
                duplicate = true;
                break;
            }
        }
        
        if (duplicate) {
            Window dialog(8, 50, starty, startx, " Already Mapped ");
            dialog.print(2, 2, captured_role + " : " + captured_name, COLOR_PAIR(CP_WARNING));
            dialog.print(3, 2, "is already mapped to " + dst_name, COLOR_PAIR(CP_WARNING));
            dialog.print(5, 2, "Press any key to close...", A_DIM);
            dialog.refresh();
            flushinp();
            nodelay(stdscr, FALSE);
            getch();
            nodelay(stdscr, TRUE);
        } else {
            Window dialog(9, 52, starty, startx, " Axis Captured ");
            dialog.print(2, 2, "Captured: " + captured_role + " : " + captured_name, COLOR_PAIR(CP_SUCCESS) | A_BOLD);
            dialog.print(3, 2, "Mapped to: " + dst_name);
            dialog.print(5, 2, "Add this mapping? [y/n]", A_DIM);
            dialog.refresh();
            
            flushinp();
            nodelay(stdscr, FALSE);
            int ch;
            while ((ch = getch()) != 'y' && ch != 'Y' && ch != 'n' && ch != 'N' && ch != 27) {}
            nodelay(stdscr, TRUE);
            
            if (ch == 'y' || ch == 'Y') {
                BindingConfigAbs b;
                b.role = captured_role;
                b.src = captured_code;
                b.dst = dst_code;
                profile.bindings_abs.push_back(b);
                tui->mark_modified();
                tui->refresh_bindings();
            }
        }
    }
    
    // Restart mapper if it was running
    if (mapper_was_running) {
        start_mapper_service();
    }
    
    needs_redraw = true;
}

void MappingsView::show_add_button_listen_dialog(int dst_code) {
    auto& config = tui->get_config();
    
    std::string dst_name = get_slot_name(dst_code, SrcKind::Key);
    
    int h = 10, w = 52;
    int starty = (tui->get_screen_height() - h) / 2;
    int startx = (tui->get_screen_width() - w) / 2;
    
    // Stop mapper so we can read from devices it had grabbed
    bool mapper_was_running = stop_mapper_service();
    if (mapper_was_running) {
        tui->scan_devices();
    }
    auto& devices = tui->get_devices();
    
    // Drain any pending events from assigned devices before listening
    for (const auto& dev : devices) {
        if (!dev->online || !dev->dev || dev->roles.empty()) continue;
        struct input_event ev;
        while (libevdev_next_event(dev->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS) {}
    }
    
    // Switch ncurses to non-blocking so we can poll devices
    nodelay(stdscr, TRUE);
    
    bool captured = false;
    std::string captured_role;
    int captured_code = 0;
    std::string captured_name;
    
    while (!captured) {
        Window dialog(h, w, starty, startx, " Add to " + dst_name + " ");
        dialog.print(2, 2, "Press a button on any device...", COLOR_PAIR(CP_HEADER) | A_BOLD);
        dialog.print(4, 2, "The button press will be mapped to:", A_DIM);
        dialog.print(5, 4, dst_name, COLOR_PAIR(CP_SUCCESS) | A_BOLD);
        dialog.print(h - 2, 2, "[ESC] Cancel", A_DIM);
        dialog.refresh();
        
        // Check for ESC
        int ch = getch();
        if (ch == 27) {
            nodelay(stdscr, TRUE);
            needs_redraw = true;
            return;
        }
        
        // Poll assigned devices for button press events
        for (const auto& dev : devices) {
            if (!dev->online || !dev->dev || dev->roles.empty()) continue;
            struct input_event ev;
            int rc = libevdev_next_event(dev->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            while (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
                if (ev.type == EV_KEY && ev.value == 1) {
                    captured_code = ev.code;
                    const char* btn_name = libevdev_event_code_get_name(EV_KEY, ev.code);
                    captured_name = btn_name ? btn_name : ("BTN_" + std::to_string(ev.code));
                    if (!dev->roles.empty()) {
                        captured_role = dev->roles[0];
                    }
                    captured = true;
                    break;
                }
                rc = libevdev_next_event(dev->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            }
            if (captured) break;
        }
        
        if (!captured) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    
    nodelay(stdscr, TRUE);
    
    // Check if this exact binding already exists
    auto active_profile = config.active_profile;
    if (config.profiles.count(active_profile)) {
        auto& profile = config.profiles[active_profile];
        bool duplicate = false;
        for (const auto& kb : profile.bindings_keys) {
            if (kb.role == captured_role && kb.src == captured_code && kb.dst == dst_code) {
                duplicate = true;
                break;
            }
        }
        
        if (duplicate) {
            Window dialog(8, 50, starty, startx, " Already Mapped ");
            dialog.print(2, 2, captured_role + " : " + captured_name, COLOR_PAIR(CP_WARNING));
            dialog.print(3, 2, "is already mapped to " + dst_name, COLOR_PAIR(CP_WARNING));
            dialog.print(5, 2, "Press any key to close...", A_DIM);
            dialog.refresh();
            flushinp();
            nodelay(stdscr, FALSE);
            getch();
            nodelay(stdscr, TRUE);
        } else {
            // Show confirmation
            Window dialog(9, 52, starty, startx, " Button Captured ");
            dialog.print(2, 2, "Captured: " + captured_role + " : " + captured_name, COLOR_PAIR(CP_SUCCESS) | A_BOLD);
            dialog.print(3, 2, "Mapped to: " + dst_name);
            dialog.print(5, 2, "Add this mapping? [y/n]", A_DIM);
            dialog.refresh();
            
            flushinp();
            nodelay(stdscr, FALSE);
            int ch;
            while ((ch = getch()) != 'y' && ch != 'Y' && ch != 'n' && ch != 'N' && ch != 27) {}
            nodelay(stdscr, TRUE);
            
            if (ch == 'y' || ch == 'Y') {
                BindingConfigKey b;
                b.role = captured_role;
                b.src = captured_code;
                b.dst = dst_code;
                profile.bindings_keys.push_back(b);
                tui->mark_modified();
                tui->refresh_bindings();
            }
        }
    }
    
    // Restart mapper if it was running
    if (mapper_was_running) {
        start_mapper_service();
    }
    
    needs_redraw = true;
}

void MappingsView::show_add_merged_listen_dialog(const DisplayRow& drow) {
    auto& config = tui->get_config();
    
    std::string dst_name = drow.display_name;
    
    int h = 10, w = 52;
    int starty = (tui->get_screen_height() - h) / 2;
    int startx = (tui->get_screen_width() - w) / 2;
    
    // Stop mapper so we can read from devices
    bool mapper_was_running = stop_mapper_service();
    if (mapper_was_running) {
        tui->scan_devices();
    }
    auto& devices = tui->get_devices();
    
    // Record baseline axis values
    struct AxisBaseline {
        std::shared_ptr<DeviceInfo> dev;
        int code;
        int baseline;
        int threshold;
    };
    std::vector<AxisBaseline> baselines;
    
    for (const auto& dev : devices) {
        if (!dev->online || !dev->dev || dev->roles.empty()) continue;
        struct input_event ev;
        while (libevdev_next_event(dev->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS) {}
        for (int axis : dev->axes) {
            int val = libevdev_get_event_value(dev->dev, EV_ABS, axis);
            const struct input_absinfo* info = libevdev_get_abs_info(dev->dev, axis);
            int range = info ? (info->maximum - info->minimum) : 65535;
            int thresh = std::max(range / 4, 50);
            baselines.push_back({dev, axis, val, thresh});
        }
    }
    
    nodelay(stdscr, TRUE);
    
    bool captured = false;
    std::string captured_role;
    int captured_code = 0;
    std::string captured_name;
    SrcKind captured_kind = SrcKind::Key;
    
    while (!captured) {
        Window dialog(h, w, starty, startx, " Add to " + dst_name + " ");
        dialog.print(2, 2, "Press a button or move an axis...", COLOR_PAIR(CP_HEADER) | A_BOLD);
        dialog.print(4, 2, "Input will be mapped to:", A_DIM);
        dialog.print(5, 4, dst_name, COLOR_PAIR(CP_SUCCESS) | A_BOLD);
        dialog.print(h - 2, 2, "[ESC] Cancel", A_DIM);
        dialog.refresh();
        
        int ch = getch();
        if (ch == 27) {
            nodelay(stdscr, TRUE);
            if (mapper_was_running) start_mapper_service();
            needs_redraw = true;
            return;
        }
        
        for (const auto& dev : devices) {
            if (!dev->online || !dev->dev || dev->roles.empty()) continue;
            struct input_event ev;
            int rc = libevdev_next_event(dev->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            while (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
                // Button press
                if (ev.type == EV_KEY && ev.value == 1) {
                    captured_code = ev.code;
                    const char* btn_name = libevdev_event_code_get_name(EV_KEY, ev.code);
                    captured_name = btn_name ? btn_name : ("BTN_" + std::to_string(ev.code));
                    if (!dev->roles.empty()) captured_role = dev->roles[0];
                    captured_kind = SrcKind::Key;
                    captured = true;
                    break;
                }
                // Axis movement
                if (ev.type == EV_ABS) {
                    for (const auto& bl : baselines) {
                        if (bl.dev == dev && bl.code == ev.code) {
                            int delta = abs(ev.value - bl.baseline);
                            if (delta > bl.threshold) {
                                captured_code = ev.code;
                                const char* abs_name = libevdev_event_code_get_name(EV_ABS, ev.code);
                                captured_name = abs_name ? abs_name : ("ABS_" + std::to_string(ev.code));
                                if (!dev->roles.empty()) captured_role = dev->roles[0];
                                captured_kind = SrcKind::Abs;
                                captured = true;
                            }
                            break;
                        }
                    }
                    if (captured) break;
                }
                rc = libevdev_next_event(dev->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
            }
            if (captured) break;
        }
        
        if (!captured) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    
    nodelay(stdscr, TRUE);
    
    // Determine which virtual code to use based on captured input type
    int dst_code = (captured_kind == SrcKind::Abs) ? drow.virtual_code : drow.alt_code;
    std::string type_label = (captured_kind == SrcKind::Abs) ? " (axis)" : " (button)";
    
    auto active_profile = config.active_profile;
    if (config.profiles.count(active_profile)) {
        auto& profile = config.profiles[active_profile];
        
        // Check for duplicate
        bool duplicate = false;
        if (captured_kind == SrcKind::Abs) {
            for (const auto& ab : profile.bindings_abs) {
                if (ab.role == captured_role && ab.src == captured_code && ab.dst == dst_code) {
                    duplicate = true;
                    break;
                }
            }
        } else {
            for (const auto& kb : profile.bindings_keys) {
                if (kb.role == captured_role && kb.src == captured_code && kb.dst == dst_code) {
                    duplicate = true;
                    break;
                }
            }
        }
        
        if (duplicate) {
            Window dialog(8, 50, starty, startx, " Already Mapped ");
            dialog.print(2, 2, captured_role + " : " + captured_name, COLOR_PAIR(CP_WARNING));
            dialog.print(3, 2, "is already mapped to " + dst_name, COLOR_PAIR(CP_WARNING));
            dialog.print(5, 2, "Press any key to close...", A_DIM);
            dialog.refresh();
            flushinp();
            nodelay(stdscr, FALSE);
            getch();
            nodelay(stdscr, TRUE);
        } else {
            Window dialog(9, 52, starty, startx, " Input Captured ");
            dialog.print(2, 2, "Captured: " + captured_role + " : " + captured_name + type_label, COLOR_PAIR(CP_SUCCESS) | A_BOLD);
            dialog.print(3, 2, "Mapped to: " + dst_name);
            dialog.print(5, 2, "Add this mapping? [y/n]", A_DIM);
            dialog.refresh();
            
            flushinp();
            nodelay(stdscr, FALSE);
            int ch;
            while ((ch = getch()) != 'y' && ch != 'Y' && ch != 'n' && ch != 'N' && ch != 27) {}
            nodelay(stdscr, TRUE);
            
            if (ch == 'y' || ch == 'Y') {
                if (captured_kind == SrcKind::Abs) {
                    BindingConfigAbs b;
                    b.role = captured_role;
                    b.src = captured_code;
                    b.dst = dst_code;
                    profile.bindings_abs.push_back(b);
                } else {
                    BindingConfigKey b;
                    b.role = captured_role;
                    b.src = captured_code;
                    b.dst = dst_code;
                    profile.bindings_keys.push_back(b);
                }
                tui->mark_modified();
                tui->refresh_bindings();
            }
        }
    }
    
    if (mapper_was_running) {
        start_mapper_service();
    }
    
    needs_redraw = true;
}

void MappingsView::show_edit_binding_dialog() {
    auto& config = tui->get_config();
    auto display = build_display_list();
    
    if (selected_binding < 0 || selected_binding >= static_cast<int>(display.size())) return;
    if (display[selected_binding].is_header) return;
    
    const auto& drow = display[selected_binding];
    
    if (drow.source_count == 0) return; // nothing to edit on unmapped control
    
    // Merged control (triggers): show combined sources dialog
    if (drow.alt_code != -1) {
        show_edit_merged_sources_dialog(drow);
        return;
    }
    
    if (drow.virtual_kind == SrcKind::Key) {
        show_edit_button_sources_dialog(drow.virtual_code);
        return;
    }
    
    // For axes with multiple sources, show sources list first
    if (drow.source_count > 1) {
        show_edit_axis_sources_dialog(drow.virtual_code);
        return;
    }
    
    // Single axis source: edit transform directly
    auto* bd = drow.binding;
    auto active_profile = config.active_profile;
    if (!config.profiles.count(active_profile)) return;
    auto& profile = config.profiles[active_profile];
    
    BindingConfigAbs* target = nullptr;
    for (auto& ab : profile.bindings_abs) {
        if (ab.role == bd->source_role && ab.src == bd->source_code && ab.dst == bd->virtual_code) {
            target = &ab;
            break;
        }
    }
    if (!target) return;
    
    // Edit dialog with fields
    int h = 14, w = 50;
    int starty = (tui->get_screen_height() - h) / 2;
    int startx = (tui->get_screen_width() - w) / 2;
    
    int field = 0; // 0=invert, 1=scale
    
    while (true) {
        Window dialog(h, w, starty, startx, " Edit Axis Transform ");
        
        std::string slot_name = get_slot_name(target->dst, SrcKind::Abs);
        dialog.print(2, 2, "Slot: " + slot_name, COLOR_PAIR(CP_HEADER));
        dialog.print(3, 2, "Source: " + target->role + ".ABS_" + std::to_string(target->src));
        
        int attr0 = (field == 0) ? COLOR_PAIR(CP_SELECTED) : 0;
        int attr1 = (field == 1) ? COLOR_PAIR(CP_SELECTED) : 0;
        
        dialog.print(5, 2, "Invert:   " + std::string(target->invert ? "YES" : "NO "), attr0);
        
        char scale_buf[32];
        snprintf(scale_buf, sizeof(scale_buf), "%.2f", target->scale);
        dialog.print(6, 2, "Scale:    " + std::string(scale_buf), attr1);
        
        dialog.print(8, 2, "[Up/Down] Select  [+/-] Adjust  [ESC] Done", A_DIM);
        
        dialog.refresh();
        
        int ch = getch();
        if (ch == 27) break;
        if (ch == KEY_UP || ch == 'k') { if (field > 0) field--; }
        if (ch == KEY_DOWN || ch == 'j') { if (field < 1) field++; }
        if (ch == '+' || ch == '=' || ch == '\n' || ch == '\r' || ch == KEY_ENTER) {
            if (field == 0) { target->invert = !target->invert; tui->mark_modified(); }
            if (field == 1) { target->scale += 0.1f; tui->mark_modified(); }
        }
        if (ch == '-') {
            if (field == 0) { target->invert = !target->invert; tui->mark_modified(); }
            if (field == 1) { target->scale = std::max(0.1f, target->scale - 0.1f); tui->mark_modified(); }
        }
    }
    
    tui->refresh_bindings();
    needs_redraw = true;
}

void MappingsView::show_edit_merged_sources_dialog(const DisplayRow& drow) {
    auto& config = tui->get_config();
    std::string dst_name = drow.display_name;
    
    auto active_profile = config.active_profile;
    if (!config.profiles.count(active_profile)) return;
    
    int h = 18, w = 55;
    int starty = (tui->get_screen_height() - h) / 2;
    int startx = (tui->get_screen_width() - w) / 2;
    
    int sel = 0;
    
    while (true) {
        auto& profile = config.profiles[active_profile];
        
        // Collect all sources from both axis and button codes
        struct SourceEntry {
            std::string role;
            int src;
            std::string label;
            SrcKind kind; // which type this source maps to
            int dst_code;
        };
        std::vector<SourceEntry> sources;
        
        // Axis sources
        for (const auto& ab : profile.bindings_abs) {
            if (ab.dst == drow.virtual_code) {
                const char* name = libevdev_event_code_get_name(EV_ABS, ab.src);
                std::string sname = name ? name : ("ABS_" + std::to_string(ab.src));
                sources.push_back({ab.role, ab.src, ab.role + " : " + sname + " (axis)", SrcKind::Abs, drow.virtual_code});
            }
        }
        // Button sources
        if (drow.alt_code != -1) {
            for (const auto& kb : profile.bindings_keys) {
                if (kb.dst == drow.alt_code) {
                    const char* name = libevdev_event_code_get_name(EV_KEY, kb.src);
                    std::string sname = name ? name : ("BTN_" + std::to_string(kb.src));
                    sources.push_back({kb.role, kb.src, kb.role + " : " + sname + " (button)", SrcKind::Key, drow.alt_code});
                }
            }
        }
        
        if (sources.empty()) {
            Window dialog(8, 50, starty, startx, " Edit " + dst_name + " ");
            dialog.print(2, 2, "No sources mapped.", COLOR_PAIR(CP_WARNING));
            dialog.print(4, 2, "Press any key to close...", A_DIM);
            dialog.refresh();
            flushinp();
            nodelay(stdscr, FALSE);
            getch();
            nodelay(stdscr, TRUE);
            break;
        }
        
        if (sel >= static_cast<int>(sources.size())) sel = static_cast<int>(sources.size()) - 1;
        if (sel < 0) sel = 0;
        
        int visible = h - 8;
        int scroll = 0;
        if (sel >= visible) scroll = sel - visible + 1;
        
        Window dialog(h, w, starty, startx, " Edit " + dst_name + " ");
        dialog.print(2, 2, "Sources mapped to " + dst_name + ":", COLOR_PAIR(CP_HEADER) | A_BOLD);
        
        for (int i = scroll; i < static_cast<int>(sources.size()) && (i - scroll) < visible; i++) {
            int attrs = (i == sel) ? COLOR_PAIR(CP_SELECTED) : 0;
            if (attrs) wattron(dialog.get(), attrs);
            mvwprintw(dialog.get(), 4 + (i - scroll), 4, "%-48s", sources[i].label.c_str());
            if (attrs) wattroff(dialog.get(), attrs);
        }
        
        char count_buf[32];
        snprintf(count_buf, sizeof(count_buf), "%d mapping(s)", static_cast<int>(sources.size()));
        dialog.print(h - 3, 2, count_buf, COLOR_PAIR(CP_HEADER));
        dialog.print(h - 2, 2, "[d] Delete selected  [ESC] Done", A_DIM);
        dialog.refresh();
        
        int ch = getch();
        if (ch == 27) break;
        if (ch == KEY_UP || ch == 'k') { if (sel > 0) sel--; }
        if (ch == KEY_DOWN || ch == 'j') { if (sel < static_cast<int>(sources.size()) - 1) sel++; }
        if ((ch == 'd' || ch == 'D') && !sources.empty()) {
            auto& entry = sources[sel];
            if (entry.kind == SrcKind::Abs) {
                auto& abs_vec = profile.bindings_abs;
                abs_vec.erase(
                    std::remove_if(abs_vec.begin(), abs_vec.end(),
                        [&](const BindingConfigAbs& b) {
                            return b.role == entry.role && b.src == entry.src && b.dst == entry.dst_code;
                        }),
                    abs_vec.end());
            } else {
                auto& key_vec = profile.bindings_keys;
                key_vec.erase(
                    std::remove_if(key_vec.begin(), key_vec.end(),
                        [&](const BindingConfigKey& b) {
                            return b.role == entry.role && b.src == entry.src && b.dst == entry.dst_code;
                        }),
                    key_vec.end());
            }
            tui->mark_modified();
            tui->refresh_bindings();
        }
    }
    
    needs_redraw = true;
}

void MappingsView::show_edit_axis_sources_dialog(int dst_code) {
    auto& config = tui->get_config();
    std::string dst_name = get_slot_name(dst_code, SrcKind::Abs);
    
    auto active_profile = config.active_profile;
    if (!config.profiles.count(active_profile)) return;
    
    int h = 18, w = 55;
    int starty = (tui->get_screen_height() - h) / 2;
    int startx = (tui->get_screen_width() - w) / 2;
    
    int sel = 0;
    
    while (true) {
        auto& profile = config.profiles[active_profile];
        
        // Collect all sources mapped to this Xbox axis
        struct SourceEntry {
            std::string role;
            int src;
            std::string label;
        };
        std::vector<SourceEntry> sources;
        for (const auto& ab : profile.bindings_abs) {
            if (ab.dst == dst_code) {
                const char* abs_name = libevdev_event_code_get_name(EV_ABS, ab.src);
                std::string name = abs_name ? abs_name : ("ABS_" + std::to_string(ab.src));
                sources.push_back({ab.role, ab.src, ab.role + " : " + name});
            }
        }
        
        if (sources.empty()) {
            Window dialog(8, 50, starty, startx, " Edit " + dst_name + " ");
            dialog.print(2, 2, "No sources mapped to this axis.", COLOR_PAIR(CP_WARNING));
            dialog.print(4, 2, "Press any key to close...", A_DIM);
            dialog.refresh();
            flushinp();
            nodelay(stdscr, FALSE);
            getch();
            nodelay(stdscr, TRUE);
            break;
        }
        
        if (sel >= static_cast<int>(sources.size())) sel = static_cast<int>(sources.size()) - 1;
        if (sel < 0) sel = 0;
        
        int visible = h - 8;
        int scroll = 0;
        if (sel >= visible) scroll = sel - visible + 1;
        
        Window dialog(h, w, starty, startx, " Edit " + dst_name + " ");
        dialog.print(2, 2, "Sources mapped to " + dst_name + ":", COLOR_PAIR(CP_HEADER) | A_BOLD);
        
        for (int i = scroll; i < static_cast<int>(sources.size()) && (i - scroll) < visible; i++) {
            int attrs = (i == sel) ? COLOR_PAIR(CP_SELECTED) : 0;
            if (attrs) wattron(dialog.get(), attrs);
            mvwprintw(dialog.get(), 4 + (i - scroll), 4, "%-48s", sources[i].label.c_str());
            if (attrs) wattroff(dialog.get(), attrs);
        }
        
        char count_buf[32];
        snprintf(count_buf, sizeof(count_buf), "%d mapping(s)", static_cast<int>(sources.size()));
        dialog.print(h - 3, 2, count_buf, COLOR_PAIR(CP_HEADER));
        dialog.print(h - 2, 2, "[d] Delete selected  [ESC] Done", A_DIM);
        dialog.refresh();
        
        int ch = getch();
        if (ch == 27) break;
        if (ch == KEY_UP || ch == 'k') { if (sel > 0) sel--; }
        if (ch == KEY_DOWN || ch == 'j') { if (sel < static_cast<int>(sources.size()) - 1) sel++; }
        if ((ch == 'd' || ch == 'D') && !sources.empty()) {
            auto& entry = sources[sel];
            auto& abs_vec = profile.bindings_abs;
            abs_vec.erase(
                std::remove_if(abs_vec.begin(), abs_vec.end(),
                    [&](const BindingConfigAbs& b) {
                        return b.role == entry.role && b.src == entry.src && b.dst == dst_code;
                    }),
                abs_vec.end());
            tui->mark_modified();
            tui->refresh_bindings();
        }
    }
    
    needs_redraw = true;
}

void MappingsView::show_edit_button_sources_dialog(int dst_code) {
    auto& config = tui->get_config();
    std::string dst_name = get_slot_name(dst_code, SrcKind::Key);
    
    auto active_profile = config.active_profile;
    if (!config.profiles.count(active_profile)) return;
    
    int h = 18, w = 55;
    int starty = (tui->get_screen_height() - h) / 2;
    int startx = (tui->get_screen_width() - w) / 2;
    
    int sel = 0;
    
    while (true) {
        auto& profile = config.profiles[active_profile];
        
        // Collect all sources mapped to this Xbox button
        struct SourceEntry {
            std::string role;
            int src;
            std::string label;
        };
        std::vector<SourceEntry> sources;
        for (const auto& kb : profile.bindings_keys) {
            if (kb.dst == dst_code) {
                const char* btn_name = libevdev_event_code_get_name(EV_KEY, kb.src);
                std::string name = btn_name ? btn_name : ("BTN_" + std::to_string(kb.src));
                sources.push_back({kb.role, kb.src, kb.role + " : " + name});
            }
        }
        
        if (sources.empty()) {
            Window dialog(8, 50, starty, startx, " Edit " + dst_name + " ");
            dialog.print(2, 2, "No sources mapped to this button.", COLOR_PAIR(CP_WARNING));
            dialog.print(4, 2, "Press any key to close...", A_DIM);
            dialog.refresh();
            flushinp();
            nodelay(stdscr, FALSE);
            getch();
            nodelay(stdscr, TRUE);
            break;
        }
        
        if (sel >= static_cast<int>(sources.size())) sel = static_cast<int>(sources.size()) - 1;
        if (sel < 0) sel = 0;
        
        int visible = h - 8;
        int scroll = 0;
        if (sel >= visible) scroll = sel - visible + 1;
        
        Window dialog(h, w, starty, startx, " Edit " + dst_name + " ");
        dialog.print(2, 2, "Sources mapped to " + dst_name + ":", COLOR_PAIR(CP_HEADER) | A_BOLD);
        
        for (int i = scroll; i < static_cast<int>(sources.size()) && (i - scroll) < visible; i++) {
            int attrs = (i == sel) ? COLOR_PAIR(CP_SELECTED) : 0;
            if (attrs) wattron(dialog.get(), attrs);
            mvwprintw(dialog.get(), 4 + (i - scroll), 4, "%-48s", sources[i].label.c_str());
            if (attrs) wattroff(dialog.get(), attrs);
        }
        
        char count_buf[32];
        snprintf(count_buf, sizeof(count_buf), "%d mapping(s)", static_cast<int>(sources.size()));
        dialog.print(h - 3, 2, count_buf, COLOR_PAIR(CP_HEADER));
        dialog.print(h - 2, 2, "[d] Delete selected  [ESC] Done", A_DIM);
        dialog.refresh();
        
        int ch = getch();
        if (ch == 27) break;
        if (ch == KEY_UP || ch == 'k') { if (sel > 0) sel--; }
        if (ch == KEY_DOWN || ch == 'j') { if (sel < static_cast<int>(sources.size()) - 1) sel++; }
        if ((ch == 'd' || ch == 'D') && !sources.empty()) {
            auto& entry = sources[sel];
            auto& key_vec = profile.bindings_keys;
            key_vec.erase(
                std::remove_if(key_vec.begin(), key_vec.end(),
                    [&](const BindingConfigKey& b) {
                        return b.role == entry.role && b.src == entry.src && b.dst == dst_code;
                    }),
                key_vec.end());
            tui->mark_modified();
            tui->refresh_bindings();
        }
    }
    
    needs_redraw = true;
}

void MappingsView::delete_selected_binding() {
    auto& config = tui->get_config();
    auto display = build_display_list();
    
    if (selected_binding < 0 || selected_binding >= static_cast<int>(display.size())) return;
    if (display[selected_binding].is_header) return;
    
    const auto& drow = display[selected_binding];
    if (drow.source_count == 0) return; // nothing to delete on unmapped control
    
    // For merged controls or multiple sources, use the appropriate sources dialog
    if (drow.alt_code != -1) {
        show_edit_merged_sources_dialog(drow);
        return;
    }
    if (drow.source_count > 1) {
        if (drow.virtual_kind == SrcKind::Key) {
            show_edit_button_sources_dialog(drow.virtual_code);
        } else {
            show_edit_axis_sources_dialog(drow.virtual_code);
        }
        return;
    }
    
    // Confirm deletion of single source
    int h = 7, w = 50;
    int starty = (tui->get_screen_height() - h) / 2;
    int startx = (tui->get_screen_width() - w) / 2;
    
    std::string slot_name = drow.display_name;
    auto* bd = drow.binding;
    std::string source_desc = bd->source_role + " : " + bd->source_name;
    
    Window dialog(h, w, starty, startx, " Delete Binding ");
    dialog.print(2, 2, "Delete " + source_desc + " from " + slot_name + "?");
    dialog.print(4, 2, "[y] Yes  [n] No", A_DIM);
    dialog.refresh();
    
    int ch;
    while ((ch = getch()) != 'y' && ch != 'Y' && ch != 'n' && ch != 'N' && ch != 27) {}
    
    if (ch == 'y' || ch == 'Y') {
        auto active_profile = config.active_profile;
        if (config.profiles.count(active_profile)) {
            auto& profile = config.profiles[active_profile];
            
            if (drow.virtual_kind == SrcKind::Abs) {
                auto& abs_vec = profile.bindings_abs;
                abs_vec.erase(
                    std::remove_if(abs_vec.begin(), abs_vec.end(),
                        [&](const BindingConfigAbs& b) {
                            return b.role == bd->source_role && b.src == bd->source_code && b.dst == bd->virtual_code;
                        }),
                    abs_vec.end());
            } else {
                auto& key_vec = profile.bindings_keys;
                key_vec.erase(
                    std::remove_if(key_vec.begin(), key_vec.end(),
                        [&](const BindingConfigKey& b) {
                            return b.role == bd->source_role && b.src == bd->source_code && b.dst == bd->virtual_code;
                        }),
                    key_vec.end());
            }
            
            tui->mark_modified();
            tui->refresh_bindings();
        }
    }
    
    needs_redraw = true;
}
