#!/bin/bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${SCRIPT_DIR}/build"

case "${1:-build}" in
    "build")
        echo "Building twcs_mapper..."
        if ! pkg-config --exists libevdev; then
            echo "libevdev not found"
            echo "Arch: sudo pacman -S libevdev"
            exit 1
        fi

        mkdir -p "${BUILD_DIR}"
        cd "${BUILD_DIR}"
        
        cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$HOME/.local" -DCMAKE_RUNTIME_OUTPUT_DIRECTORY=bin
        cmake --build . --config Release
        
        echo "Built: ${BUILD_DIR}/bin/twcs_select"
        echo "Built: ${BUILD_DIR}/bin/twcs_mapper"
        ;;
    
    "run")
        "${SCRIPT_DIR}/build.sh" build
        
        bin="${BUILD_DIR}/bin/twcs_mapper"
        if [[ ! -x "$bin" ]]; then
          echo "Error: twcs_mapper not found. Run './build.sh build' first."
          exit 1
        fi

        if [[ -n "${TWCS_EVENT:-}" ]]; then
            echo "Using TWCS_EVENT: ${TWCS_EVENT}"
            exec "${bin}" "${TWCS_EVENT}"
        else
            echo "Using config file"
            exec "${bin}"
        fi
        ;;
    
    "clean")
        rm -rf "${BUILD_DIR}"
        echo "Cleaned"
        ;;
    
    "discovery")
        "${SCRIPT_DIR}/build.sh" build
        
        bin="${BUILD_DIR}/bin/twcs_mapper"
        if [[ ! -x "$bin" ]]; then
          echo "Error: twcs_mapper not found. Run './build.sh build' first."
          exit 1
        fi
        
        if [[ -n "${TWCS_EVENT:-}" ]]; then
            echo "Discovery mode for: ${TWCS_EVENT}"
            exec "${bin}" --print-map "${TWCS_EVENT}"
        else
            echo "Set TWCS_EVENT to device path for discovery mode"
            echo "Example: TWCS_EVENT=/dev/input/by-id/...event-joystick ${0} discovery"
            exit 1
        fi
        ;;

    "install")
        "${SCRIPT_DIR}/build.sh" build
        cd "${BUILD_DIR}"
        cmake --install .
        echo "Installed to ~/.local/bin"
        ;;

    "help"|*)
        echo "Usage: $0 build|run|clean|discovery|install|help"
        echo "  build     - Compile the mapper (default)"
        echo "  run       - Build and run the mapper"
        echo "  discovery - Build and run discovery mode"
        echo "  install   - Build and install to ~/.local/bin"
        echo "  clean     - Remove build artifacts"
        echo "  help      - Show this help"
        echo ""
        echo "Environment:"
        echo "  TWCS_EVENT=/dev/input/eventX  Override input device for run/discovery"
        ;;

    *)
esac