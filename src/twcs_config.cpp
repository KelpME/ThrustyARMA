// twcs_config.cpp - Now redirects to the comprehensive TUI
#include <iostream>
#include <cstdlib>

int main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;
    
    std::cout << "The configuration tool has been upgraded!\n\n";
    std::cout << "Please use the new comprehensive TUI instead:\n";
    std::cout << "  twcs_tui\n\n";
    std::cout << "Or use the Makefile:\n";
    std::cout << "  make tui\n\n";
    std::cout << "The new TUI includes:\n";
    std::cout << "  - Device Dashboard (F1)\n";
    std::cout << "  - Mappings Editor (F2)\n";
    std::cout << "  - Calibration Wizard (F3)\n";
    std::cout << "  - Profile Manager (F4)\n";
    std::cout << "  - Live Input Monitor\n\n";
    
    return 0;
}
