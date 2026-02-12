#include "tui.hpp"

// Main entry point
int main(int argc, char* argv[]) {
    // Check for command line options
    bool start_in_calibration = false;
    
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--calibrate") == 0 || strcmp(argv[i], "-c") == 0) {
            start_in_calibration = true;
        } else if (strcmp(argv[i], "--help") == 0 || strcmp(argv[i], "-h") == 0) {
            printf("TWCS Mapper TUI\n\n");
            printf("Usage: %s [options]\n\n", argv[0]);
            printf("Options:\n");
            printf("  -c, --calibrate    Start in calibration wizard view\n");
            printf("  -h, --help         Show this help message\n");
            printf("\nViews (switch with F1-F5 or Tab):\n");
            printf("  F1 - Device Dashboard\n");
            printf("  F2 - Mappings Editor\n");
            printf("  F3 - Calibration Wizard (quick)\n");
            printf("  F4 - Profile Manager\n");
            printf("  F5 - Live Monitor\n");
            printf("\nFor full device setup, run: twcs_setup\n");
            return 0;
        }
    }
    
    try {
        TUI tui;
        
        // Start in calibration view if requested
        if (start_in_calibration) {
            tui.set_view(ViewType::CALIBRATION);
        }
        
        tui.run();
    } catch (const std::exception& e) {
        endwin();
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
    
    return 0;
}
