// Helper function to ONLY detect which axis is moving and measure its center
// Does NOT measure full range - that's done separately
std::pair<int, int> detect_axis_and_center(DeviceInfo& device, int capture_time_ms = 3000) {
    const int JITTER_THRESHOLD = 100;
    const int MIN_MOVEMENT = 3000;
    std::map<int, int> delta_sum;
    std::map<int, std::vector<int>> center_samples;
    std::map<int, int> baseline_values;
    struct input_event ev;
    
    // Drain pending events first
    while (libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev) == LIBEVDEV_READ_STATUS_SUCCESS) {
        // Just drain the queue
    }
    
    // Get initial values as baseline
    for (int code = 0; code <= ABS_MAX; code++) {
        if (libevdev_has_event_code(device.dev, EV_ABS, code)) {
            const struct input_absinfo* absinfo = libevdev_get_abs_info(device.dev, code);
            if (absinfo) {
                baseline_values[code] = absinfo->value;
                delta_sum[code] = 0;
            }
        }
    }
    
    // Sample center values at the beginning (first 1 second while user holds still)
    auto start = std::chrono::steady_clock::now();
    auto center_end = start + std::chrono::milliseconds(1000);
    
    while (std::chrono::steady_clock::now() < center_end) {
        int rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_ABS) {
            center_samples[ev.code].push_back(ev.value);
        } else if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    
    // Now capture movement for remaining time (user moves the axis)
    while (std::chrono::steady_clock::now() - start < std::chrono::milliseconds(capture_time_ms)) {
        int rc = libevdev_next_event(device.dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        
        if (rc == LIBEVDEV_READ_STATUS_SUCCESS && ev.type == EV_ABS) {
            int baseline = baseline_values[ev.code];
            int delta = std::abs(ev.value - baseline);
            
            if (delta >= JITTER_THRESHOLD) {
                delta_sum[ev.code] += delta;
            }
        } else if (rc == -EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    
    // Show all detected movement for debugging
    std::cout << "  Movement detected:\n";
    for (const auto& [code, delta] : delta_sum) {
        if (delta > 0) {
            const char* axis_name_str = libevdev_event_code_get_name(EV_ABS, code);
            std::cout << "    Axis " << code << " (" << (axis_name_str ? axis_name_str : "UNKNOWN") 
                     << "): " << delta << " units\n";
        }
    }
    
    // Find axis with highest delta
    int best_code = -1;
    int max_delta = 0;
    for (const auto& [code, delta] : delta_sum) {
        if (delta > max_delta) {
            max_delta = delta;
            best_code = code;
        }
    }
    
    // Require minimum movement
    if (max_delta < MIN_MOVEMENT) {
        std::cout << "  WARNING: Movement too small (" << max_delta << " < " << MIN_MOVEMENT << ")\n";
        return {-1, 0};
    }
    
    std::cout << "Detected axis code: " << best_code << "\n";
    
    // Calculate center value from initial samples (when user was holding still)
    int center_value = baseline_values[best_code];
    
    if (!center_samples[best_code].empty()) {
        long long sum = 0;
        for (int val : center_samples[best_code]) {
            sum += val;
        }
        center_value = sum / center_samples[best_code].size();
    }
    
    std::cout << "Center value: " << center_value << "\n";
    
    return {best_code, center_value};
}
