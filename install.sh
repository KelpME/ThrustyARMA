#!/bin/bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

echo "=== ThrustyARMA Installation ==="
echo ""

# Check dependencies
echo "Checking dependencies..."
if ! pkg-config --exists libevdev; then
    echo "ERROR: libevdev not found"
    echo "Install with: sudo pacman -S libevdev"
    exit 1
fi

if ! pkg-config --exists ncurses; then
    echo "ERROR: ncurses not found"
    echo "Install with: sudo pacman -S ncurses"
    exit 1
fi

echo "✓ Dependencies OK"
echo ""

# Build
echo "Building ThrustyARMA..."
mkdir -p "${SCRIPT_DIR}/build"
cd "${SCRIPT_DIR}/build"

cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build . --config Release -j$(nproc)

echo "✓ Build complete"
echo ""

# Install binaries
echo "Installing to ~/.local/bin..."
mkdir -p ~/.local/bin
install -m 755 bin/twcs_mapper ~/.local/bin/twcs_mapper
install -m 755 bin/twcs_setup ~/.local/bin/twcs_setup
install -m 755 bin/twcs_config ~/.local/bin/twcs_config
install -m 755 bin/twcs_select ~/.local/bin/twcs_select

echo "✓ Binaries installed"
echo ""

# Optional systemd service installation
echo "=== Autostart Setup (Optional) ==="
echo "Would you like to install the systemd service for automatic startup?"
echo "This will start the mapper automatically when you log in."
read -p "Install autostart? [y/N]: " -n 1 -r
echo ""

if [[ $REPLY =~ ^[Yy]$ ]]; then
    echo "Installing systemd user service..."
    mkdir -p ~/.config/systemd/user
    cp "${SCRIPT_DIR}/twcs-mapper.service" ~/.config/systemd/user/
    
    systemctl --user daemon-reload
    systemctl --user enable twcs-mapper.service
    
    echo "✓ Autostart enabled"
    echo ""
    echo "Service commands:"
    echo "  Start:   systemctl --user start twcs-mapper.service"
    echo "  Stop:    systemctl --user stop twcs-mapper.service"
    echo "  Status:  systemctl --user status twcs-mapper.service"
    echo "  Logs:    journalctl --user -u twcs-mapper.service -f"
else
    echo "Skipped autostart installation"
    echo "You can manually install it later with:"
    echo "  cp twcs-mapper.service ~/.config/systemd/user/"
    echo "  systemctl --user enable --now twcs-mapper.service"
fi

echo ""
echo "=== Installation Complete! ==="
echo ""
echo "Next steps:"
echo "  1. Run 'twcs_setup' to configure your devices"
echo "  2. Run 'twcs_config' for advanced binding configuration"
echo "  3. Run 'twcs_mapper' to start the mapper manually"
echo ""