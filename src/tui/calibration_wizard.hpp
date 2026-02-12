#pragma once

#include "tui.hpp"

class CalibrationWizard : public View {
private:
    enum class State { SELECT_DEVICE, READY_CENTER, CENTER_SAMPLE, READY_RANGE, RANGE_SAMPLE, REVIEW, COMPLETE };
    
    struct CalibItem {
        std::string device_name;
        std::string role;
        int src_axis;
        int dst_axis;
        std::string xbox_name;
        bool calibrated;
        bool online;
    };
    
    State state;
    std::string selected_role;
    int selected_axis;
    AxisCalibration current_calibration;
    std::vector<int> center_samples;
    std::vector<int> range_samples;
    std::chrono::steady_clock::time_point sample_start;
    int sample_duration_ms;
    std::string status_message;
    
    std::vector<CalibItem> get_calibration_items();
    
public:
    CalibrationWizard(TUI* parent);
    
    void reset_calibration();
    void draw() override;
    void handle_input(int ch) override;
    
private:
    void draw_device_selection(Window* main_win);
    void draw_ready_center(Window* main_win);
    void draw_center_sampling(Window* main_win);
    void draw_ready_range(Window* main_win);
    void draw_range_sampling(Window* main_win);
    void draw_review(Window* main_win);
    void draw_complete(Window* main_win);
    
    void handle_select_device(int ch);
    void handle_ready_center(int ch);
    void handle_center_sampling(int ch);
    void handle_ready_range(int ch);
    void handle_range_sampling(int ch);
    void handle_review(int ch);
    
    void start_calibration();
    void start_center_sampling();
    void start_range_sampling();
    void sample_axis_value();
    void finish_center_sampling();
    void finish_range_sampling();
    void save_calibration();
};
