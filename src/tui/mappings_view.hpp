#pragma once

#include "tui.hpp"

class MappingsView : public View {
private:
    int scroll_offset;
    int selected_binding;
    
    // Display row: section header or Xbox control (possibly merged axis+button)
    struct DisplayRow {
        bool is_header;
        std::string header_text;
        std::string display_name;       // human-readable name for this control
        
        // Primary virtual output (always set for non-headers)
        int virtual_code;
        SrcKind virtual_kind;
        
        // Optional secondary virtual output (for merged controls like triggers/dpad)
        int alt_code;                   // -1 if no alt
        SrcKind alt_kind;
        
        BindingDisplay* binding;        // first binding found (for display)
        int source_count;               // total sources across both codes
    };
    
    std::vector<DisplayRow> build_display_list();
    int count_sources(int code, SrcKind kind);
    BindingDisplay* find_first_binding(int code, SrcKind kind);
    
public:
    MappingsView(TUI* parent);
    
    void draw() override;
    void handle_input(int ch) override;
    
private:
    std::string get_slot_name(int code, SrcKind kind);
    void show_add_binding_dialog();
    void show_add_axis_listen_dialog(int dst_code);
    void show_add_button_listen_dialog(int dst_code);
    void show_add_merged_listen_dialog(const DisplayRow& drow);
    void show_edit_binding_dialog();
    void show_edit_merged_sources_dialog(const DisplayRow& drow);
    void show_edit_axis_sources_dialog(int dst_code);
    void show_edit_button_sources_dialog(int dst_code);
    void delete_selected_binding();
};
