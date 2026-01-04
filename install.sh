#!/bin/bash

# Build and install script for ThrustyARMA

set -e

echo "Building ThrustyARMA..."

# Create build directory
mkdir -p build
cd build

# Configure and build
cmake ..
make -j$(nproc)

echo "Installing to ~/.local/bin..."

# Install executables
install -D bin/twcs_mapper ~/.local/bin/twcs_mapper
install -D bin/twcs_setup ~/.local/bin/twcs_setup
install -D bin/twcs_config ~/.local/bin/twcs_config
install -D bin/twcs_select ~/.local/bin/twcs_select

echo "Installation complete!"
echo ""
echo "Run 'twcs_setup' to configure your devices (30-second guided setup)."
echo "Run 'twcs_config' for manual configuration."
echo ""
echo "The mapper will be installed as a user service and start automatically."