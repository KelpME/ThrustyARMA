#include "calibration_wizard.hpp"

std::vector<CalibrationWizard::CalibItem> CalibrationWizard::get_calibration_items() {
    std::vector<CalibItem> items;
    auto& devices = tui->get_devices();
    auto active_abs = tui->get_config().get_active_bindings_abs();
    
    for (const auto& binding : active_abs) {
        // Find the device for this binding's role (online or offline)
        bool found = false;
        for (const auto& dev : devices) {
            if (!dev->has_role(binding.role)) continue;
            
            // For online devices, verify the axis exists
            if (dev->online) {
                bool has_axis = false;
                for (int a : dev->axes) {
                    if (a == binding.src) { has_axis = true; break; }
                }
                if (!has_axis) continue;
            }
            
            CalibItem item;
            item.device_name = dev->name.empty() ? ("(" + binding.role + " device)") : dev->name;
            item.role = binding.role;
            item.src_axis = binding.src;
            item.dst_axis = binding.dst;
            item.xbox_name = get_xbox_axis_name(binding.dst);
            item.calibrated = tui->get_config().get_calibration(binding.role, binding.src).has_value();
            item.online = dev->online;
            items.push_back(item);
            found = true;
            break;
        }
        
        // Device not in device list at all — still show it
        if (!found) {
            CalibItem item;
            item.device_name = "(offline " + binding.role + " device)";
            item.role = binding.role;
            item.src_axis = binding.src;
            item.dst_axis = binding.dst;
            item.xbox_name = get_xbox_axis_name(binding.dst);
            item.calibrated = tui->get_config().get_calibration(binding.role, binding.src).has_value();
            item.online = false;
            items.push_back(item);
        }
    }
    return items;
}

CalibrationWizard::CalibrationWizard(TUI* parent) : View(parent, ViewType::CALIBRATION), 
                                 state(State::SELECT_DEVICE),
                                 selected_axis(-1),
                                 sample_duration_ms(5000) {
    reset_calibration();
}

void CalibrationWizard::reset_calibration() {
    state = State::SELECT_DEVICE;
    selected_role.clear();
    selected_axis = -1;
    center_samples.clear();
    range_samples.clear();
    current_calibration = AxisCalibration{0, 0, 65535, 32768, 0};
}

void CalibrationWizard::draw() {
    auto* main_win = tui->get_main_win();
    int height = main_win->get_height();
    
    main_win->clear();
    
    // Title
    main_win->print(1, 2, "Calibration Wizard", COLOR_PAIR(CP_HEADER) | A_BOLD);
    
    switch (state) {
        case State::SELECT_DEVICE:
            draw_device_selection(main_win);
            break;
        case State::READY_CENTER:
            draw_ready_center(main_win);
            break;
        case State::CENTER_SAMPLE:
            draw_center_sampling(main_win);
            break;
        case State::READY_RANGE:
            draw_ready_range(main_win);
            break;
        case State::RANGE_SAMPLE:
            draw_range_sampling(main_win);
            break;
        case State::REVIEW:
            draw_review(main_win);
            break;
        case State::COMPLETE:
            draw_complete(main_win);
            break;
    }
    
    // Status message
    if (!status_message.empty()) {
        main_win->print(height - 2, 2, status_message, COLOR_PAIR(CP_WARNING));
    }
    
    main_win->refresh();
    needs_redraw = true; // Always redraw for live updates
}

void CalibrationWizard::draw_device_selection(Window* main_win) {
    int width = main_win->get_width();
    int height = main_win->get_height();
    auto items = get_calibration_items();
    
    // Dynamic column layout
    int usable = width - 4;
    int col_dev = 2;
    int col_ctrl = col_dev + std::max(28, usable * 35 / 100);
    int col_dz = col_ctrl + std::max(16, usable * 20 / 100);
    int col_cal = col_dz + std::max(12, usable * 15 / 100);
    
    main_win->print(3, 2, "Select a mapped axis to calibrate:", COLOR_PAIR(CP_HEADER));
    
    char hdr[256];
    snprintf(hdr, sizeof(hdr), "%-*s%-*s%-*s%s",
             col_ctrl - col_dev, "Device",
             col_dz - col_ctrl, "Xbox Control",
             col_cal - col_dz, "Deadzone",
             "Status");
    main_win->print(5, 2, hdr, A_BOLD);
    main_win->print(6, 2, std::string(width - 4, '-'));
    
    if (items.empty()) {
        main_win->print(8, 4, "No mapped axes found.", COLOR_PAIR(CP_WARNING));
        main_win->print(9, 4, "Add axis bindings in the Mappings tab first.");
        main_win->print(11, 2, "[ESC] Back");
        return;
    }
    
    int row = 7;
    for (size_t i = 0; i < items.size() && row < height - 4; i++) {
        const auto& item = items[i];
        
        // Look up current deadzone from calibration
        auto cal = tui->get_config().get_calibration(item.role, item.src_axis);
        std::string dz_str = cal.has_value() ? std::to_string(cal->deadzone_radius) : "-";
        
        if (!item.online) {
            // Offline device — dim the whole row
            wattron(main_win->get(), A_DIM);
            
            std::string dev_label = item.device_name + " (" + item.role + ")";
            int dev_width = col_ctrl - col_dev - 2;
            if (static_cast<int>(dev_label.length()) > dev_width)
                dev_label = dev_label.substr(0, dev_width - 3) + "...";
            mvwprintw(main_win->get(), row, col_dev, "%-*s", col_ctrl - col_dev, dev_label.c_str());
            mvwprintw(main_win->get(), row, col_ctrl, "%-*s", col_dz - col_ctrl, item.xbox_name.c_str());
            mvwprintw(main_win->get(), row, col_dz, "%-*s", col_cal - col_dz, dz_str.c_str());
            
            wattroff(main_win->get(), A_DIM);
            
            wattron(main_win->get(), COLOR_PAIR(CP_OFFLINE));
            mvwprintw(main_win->get(), row, col_cal, "OFFLINE");
            wattroff(main_win->get(), COLOR_PAIR(CP_OFFLINE));
        } else {
            int attrs = (static_cast<int>(i) == selected_item) ? COLOR_PAIR(CP_SELECTED) : 0;
            if (attrs) wattron(main_win->get(), attrs);
            
            // Device name with role
            std::string dev_label = item.device_name + " (" + item.role + ")";
            int dev_width = col_ctrl - col_dev - 2;
            if (static_cast<int>(dev_label.length()) > dev_width)
                dev_label = dev_label.substr(0, dev_width - 3) + "...";
            mvwprintw(main_win->get(), row, col_dev, "%-*s", col_ctrl - col_dev, dev_label.c_str());
            
            // Xbox control name
            mvwprintw(main_win->get(), row, col_ctrl, "%-*s", col_dz - col_ctrl, item.xbox_name.c_str());
            
            // Deadzone value
            mvwprintw(main_win->get(), row, col_dz, "%-*s", col_cal - col_dz, dz_str.c_str());
            
            if (attrs) wattroff(main_win->get(), attrs);
            
            // Calibration status
            if (item.calibrated) {
                wattron(main_win->get(), COLOR_PAIR(CP_SUCCESS));
                mvwprintw(main_win->get(), row, col_cal, "Calibrated");
                wattroff(main_win->get(), COLOR_PAIR(CP_SUCCESS));
            } else {
                wattron(main_win->get(), COLOR_PAIR(CP_WARNING));
                mvwprintw(main_win->get(), row, col_cal, "Not calibrated");
                wattroff(main_win->get(), COLOR_PAIR(CP_WARNING));
            }
        }
        
        row++;
    }
    
    main_win->print(row + 2, 2, "[ENTER] Calibrate  [+/-] Adjust deadzone  [ESC] Cancel");
}

void CalibrationWizard::draw_ready_center(Window* main_win) {
    main_win->print(3, 2, "Step 1/3: Center Position Calibration");
    main_win->print(5, 2, "Leave the axis at its center (resting) position.");
    main_win->print(6, 2, "Do not touch the axis during sampling.");
    main_win->print(8, 2, "When you are ready, press [ENTER] to begin.", COLOR_PAIR(CP_HEADER) | A_BOLD);
    main_win->print(11, 2, "[ENTER] Start  [ESC] Cancel");
}

void CalibrationWizard::draw_center_sampling(Window* main_win) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sample_start).count();
    float progress = std::min(1.0f, static_cast<float>(elapsed) / sample_duration_ms);
    int width = main_win->get_width();
    
    main_win->print(3, 2, "Step 1/3: Center Position Calibration");
    main_win->print(5, 2, "Leave the axis centered and still...");
    
    // Progress bar - fill most of the width
    int bar_width = std::max(40, width - 8);
    int filled = static_cast<int>(bar_width * progress);
    std::string bar(filled, '#');
    bar += std::string(bar_width - filled, '-');
    
    wattron(main_win->get(), COLOR_PAIR(CP_AXIS));
    mvwprintw(main_win->get(), 7, 2, "[%s]", bar.c_str());
    wattroff(main_win->get(), COLOR_PAIR(CP_AXIS));
    
    mvwprintw(main_win->get(), 8, 2, "Time: %.1fs / %.1fs", 
              elapsed / 1000.0f, sample_duration_ms / 1000.0f);
    
    // Live samples
    if (!center_samples.empty()) {
        int current = center_samples.back();
        double sum = 0;
        for (int v : center_samples) sum += v;
        int avg = static_cast<int>(sum / center_samples.size());
        
        mvwprintw(main_win->get(), 10, 2, "Current: %6d  Average: %6d  Samples: %zu",
                  current, avg, center_samples.size());
    }
    
    // Auto-advance when done
    if (progress >= 1.0f) {
        finish_center_sampling();
    } else {
        // Continue sampling
        sample_axis_value();
    }
    
    main_win->print(13, 2, "[S]kip  [R]estart  [ESC] Cancel");
}

void CalibrationWizard::draw_ready_range(Window* main_win) {
    main_win->print(3, 2, "Step 2/3: Full Range Calibration");
    main_win->print(5, 2, "You will need to move the axis through its entire range.");
    main_win->print(6, 2, "Push it all the way in both directions during sampling.");
    main_win->print(8, 2, "When you are ready, press [ENTER] to begin.", COLOR_PAIR(CP_HEADER) | A_BOLD);
    main_win->print(11, 2, "[ENTER] Start  [S]kip  [ESC] Cancel");
}

void CalibrationWizard::draw_range_sampling(Window* main_win) {
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - sample_start).count();
    float progress = std::min(1.0f, static_cast<float>(elapsed) / sample_duration_ms);
    int width = main_win->get_width();
    
    main_win->print(3, 2, "Step 2/3: Full Range Calibration");
    main_win->print(5, 2, "Move the axis through its full range...");
    
    // Progress bar - fill most of the width
    int bar_width = std::max(40, width - 8);
    int filled = static_cast<int>(bar_width * progress);
    std::string bar(filled, '#');
    bar += std::string(bar_width - filled, '-');
    
    wattron(main_win->get(), COLOR_PAIR(CP_AXIS));
    mvwprintw(main_win->get(), 7, 2, "[%s]", bar.c_str());
    wattroff(main_win->get(), COLOR_PAIR(CP_AXIS));
    
    mvwprintw(main_win->get(), 8, 2, "Time: %.1fs / %.1fs", 
              elapsed / 1000.0f, sample_duration_ms / 1000.0f);
    
    // Show current range
    if (!range_samples.empty()) {
        int current = range_samples.back();
        int min_val = *std::min_element(range_samples.begin(), range_samples.end());
        int max_val = *std::max_element(range_samples.begin(), range_samples.end());
        
        mvwprintw(main_win->get(), 10, 2, "Current: %6d  Min: %6d  Max: %6d",
                  current, min_val, max_val);
    }
    
    // Auto-advance when done
    if (progress >= 1.0f) {
        finish_range_sampling();
    } else {
        // Continue sampling
        sample_axis_value();
    }
    
    main_win->print(13, 2, "[S]kip  [R]estart  [ESC] Cancel");
}

void CalibrationWizard::draw_review(Window* main_win) {
    int width = main_win->get_width();
    
    main_win->print(3, 2, "Step 3/3: Review Calibration");
    
    int min_val = current_calibration.observed_min;
    int max_val = current_calibration.observed_max;
    int center = current_calibration.center_value;
    int range = max_val - min_val;
    
    main_win->print(5, 2, "Calibration Results:");
    // Find the Xbox control name for this axis from active bindings
    std::string ctrl_name = "Unknown";
    std::string dev_name = selected_role;
    auto items = get_calibration_items();
    for (const auto& item : items) {
        if (item.role == selected_role && item.src_axis == selected_axis) {
            ctrl_name = item.xbox_name;
            dev_name = item.device_name + " (" + item.role + ")";
            break;
        }
    }
    mvwprintw(main_win->get(), 7, 4, "Device:  %s", dev_name.c_str());
    mvwprintw(main_win->get(), 8, 4, "Control: %s", ctrl_name.c_str());
    mvwprintw(main_win->get(), 10, 4, "Min:     %6d", min_val);
    mvwprintw(main_win->get(), 11, 4, "Max:     %6d", max_val);
    mvwprintw(main_win->get(), 12, 4, "Center:  %6d", center);
    mvwprintw(main_win->get(), 13, 4, "Range:   %6d", range);
    mvwprintw(main_win->get(), 14, 4, "Deadzone: %5d", current_calibration.deadzone_radius);
    
    // Visual bar - fill available width
    int bar_width = std::max(40, width - 16);
    int center_pos = (range > 0) ? ((center - min_val) * bar_width / range) : (bar_width / 2);
    
    std::string bar(bar_width, '-');
    if (center_pos >= 0 && center_pos < bar_width) {
        bar[center_pos] = '|';
    }
    
    main_win->print(16, 4, "Visual: [" + bar + "]");
    
    main_win->print(19, 2, "[A]ccept  [R]etry  [ESC] Cancel");
}

void CalibrationWizard::draw_complete(Window* main_win) {
    main_win->print(8, 2, "✓ Calibration Complete!", COLOR_PAIR(CP_SUCCESS) | A_BOLD);
    main_win->print(10, 2, "The calibration has been saved and will be");
    main_win->print(11, 2, "used across all profiles.");
    main_win->print(13, 2, "Press any key to continue...", A_DIM);
}

void CalibrationWizard::handle_input(int ch) {
    switch (state) {
        case State::SELECT_DEVICE:
            handle_select_device(ch);
            break;
        case State::READY_CENTER:
            handle_ready_center(ch);
            break;
        case State::CENTER_SAMPLE:
            handle_center_sampling(ch);
            break;
        case State::READY_RANGE:
            handle_ready_range(ch);
            break;
        case State::RANGE_SAMPLE:
            handle_range_sampling(ch);
            break;
        case State::REVIEW:
            handle_review(ch);
            break;
        case State::COMPLETE:
            reset_calibration();
            break;
    }
}

void CalibrationWizard::handle_select_device(int ch) {
    auto items = get_calibration_items();
    int total_options = static_cast<int>(items.size());
    
    // Helper to find next/prev online item
    auto find_online = [&](int from, int dir) -> int {
        int pos = from + dir;
        while (pos >= 0 && pos < total_options) {
            if (items[pos].online) return pos;
            pos += dir;
        }
        return from; // stay put if nothing found
    };
    
    switch (ch) {
        case KEY_UP:
        case 'k':
            if (selected_item > 0) {
                selected_item = find_online(selected_item, -1);
                needs_redraw = true;
            }
            break;
        case KEY_DOWN:
        case 'j':
            if (selected_item < total_options - 1) {
                selected_item = find_online(selected_item, 1);
                needs_redraw = true;
            }
            break;
        case '\n':
        case '\r':
        case KEY_ENTER:
            if (total_options > 0 && selected_item < total_options && items[selected_item].online) {
                start_calibration();
            }
            break;
        case '+':
        case '=':
            if (total_options > 0 && selected_item < total_options && items[selected_item].online) {
                auto& item = items[selected_item];
                auto cal = tui->get_config().get_calibration(item.role, item.src_axis);
                if (cal.has_value()) {
                    cal->deadzone_radius += 1;
                    tui->get_config().set_calibration(item.role, item.src_axis, *cal);
                    tui->mark_modified();
                    needs_redraw = true;
                }
            }
            break;
        case '-':
            if (total_options > 0 && selected_item < total_options && items[selected_item].online) {
                auto& item = items[selected_item];
                auto cal = tui->get_config().get_calibration(item.role, item.src_axis);
                if (cal.has_value()) {
                    cal->deadzone_radius = std::max(0, cal->deadzone_radius - 1);
                    tui->get_config().set_calibration(item.role, item.src_axis, *cal);
                    tui->mark_modified();
                    needs_redraw = true;
                }
            }
            break;
        case 27: // ESC
            tui->set_view(ViewType::DASHBOARD);
            break;
    }
}

void CalibrationWizard::handle_ready_center(int ch) {
    switch (ch) {
        case '\n':
        case '\r':
        case KEY_ENTER:
            start_center_sampling();
            break;
        case 27: // ESC
            reset_calibration();
            break;
    }
}

void CalibrationWizard::handle_center_sampling(int ch) {
    switch (ch) {
        case 's':
        case 'S':
            finish_center_sampling();
            break;
        case 'r':
        case 'R':
            state = State::READY_CENTER;
            needs_redraw = true;
            break;
        case 27: // ESC
            reset_calibration();
            break;
    }
}

void CalibrationWizard::handle_ready_range(int ch) {
    switch (ch) {
        case '\n':
        case '\r':
        case KEY_ENTER:
            start_range_sampling();
            break;
        case 's':
        case 'S':
            finish_range_sampling();
            break;
        case 27: // ESC
            reset_calibration();
            break;
    }
}

void CalibrationWizard::handle_range_sampling(int ch) {
    switch (ch) {
        case 's':
        case 'S':
            finish_range_sampling();
            break;
        case 'r':
        case 'R':
            state = State::READY_RANGE;
            needs_redraw = true;
            break;
        case 27: // ESC
            reset_calibration();
            break;
    }
}

void CalibrationWizard::handle_review(int ch) {
    switch (ch) {
        case 'a':
        case 'A':
            save_calibration();
            state = State::COMPLETE;
            needs_redraw = true;
            break;
        case 'r':
        case 'R':
            // Retry goes back through ready gates
            if (selected_item >= 0 && selected_item < static_cast<int>(get_calibration_items().size())) {
                state = State::READY_CENTER;
                center_samples.clear();
                range_samples.clear();
                needs_redraw = true;
            }
            break;
        case 27: // ESC
            reset_calibration();
            break;
    }
}

void CalibrationWizard::start_calibration() {
    auto items = get_calibration_items();
    if (selected_item >= 0 && selected_item < static_cast<int>(items.size())) {
        selected_role = items[selected_item].role;
        selected_axis = items[selected_item].src_axis;
        state = State::READY_CENTER;
        needs_redraw = true;
    }
}

void CalibrationWizard::start_center_sampling() {
    state = State::CENTER_SAMPLE;
    center_samples.clear();
    sample_start = std::chrono::steady_clock::now();
    needs_redraw = true;
}

void CalibrationWizard::start_range_sampling() {
    state = State::RANGE_SAMPLE;
    range_samples.clear();
    sample_start = std::chrono::steady_clock::now();
    needs_redraw = true;
}

void CalibrationWizard::sample_axis_value() {
    // Find device and read axis
    auto& devices = tui->get_devices();
    for (const auto& dev : devices) {
        if (dev->has_role(selected_role) && dev->online && dev->fd >= 0) {
            struct input_absinfo absinfo;
            if (ioctl(dev->fd, EVIOCGABS(selected_axis), &absinfo) == 0) {
                if (state == State::CENTER_SAMPLE) {
                    center_samples.push_back(absinfo.value);
                } else if (state == State::RANGE_SAMPLE) {
                    range_samples.push_back(absinfo.value);
                }
            }
            break;
        }
    }
}

void CalibrationWizard::finish_center_sampling() {
    // Calculate center from samples
    if (!center_samples.empty()) {
        long long sum = 0;
        for (int v : center_samples) sum += v;
        current_calibration.center_value = static_cast<int>(sum / center_samples.size());
        current_calibration.src_code = selected_axis;
        
        // Calculate deadzone from variance
        int min_sample = *std::min_element(center_samples.begin(), center_samples.end());
        int max_sample = *std::max_element(center_samples.begin(), center_samples.end());
        current_calibration.deadzone_radius = (max_sample - min_sample) / 2 + 10;
    }
    
    state = State::READY_RANGE;
    needs_redraw = true;
}

void CalibrationWizard::finish_range_sampling() {
    // Calculate range from samples
    if (!range_samples.empty()) {
        current_calibration.observed_min = *std::min_element(range_samples.begin(), range_samples.end());
        current_calibration.observed_max = *std::max_element(range_samples.begin(), range_samples.end());
    }
    
    state = State::REVIEW;
    needs_redraw = true;
}

void CalibrationWizard::save_calibration() {
    auto& config = tui->get_config();
    config.set_calibration(selected_role, selected_axis, current_calibration);
    
    // Auto-save config
    std::string config_path = get_config_path();
    ConfigManager::save(config_path, config);
}
