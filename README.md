# Thrustmaster ARMA Reforger Mapper

A Linux userspace tool that merges multiple Thrustmaster input devices into a single Xbox 360 Controller-compatible virtual joystick for ARMA Reforger.

## Overview

Supports merging these Thrustmaster devices:
- **T.16000M Joystick** (044f:b10a) - **required**
- **TWCS Throttle** (044f:b687) - optional
- **T-Rudder** (044f:b679) - optional

This creates an Xbox 360 Controller-compatible virtual joystick that ARMA Reforger recognizes as a gamepad, while preserving raw device access for other applications like DCS.

## Features

- **Multi-device merging**: Reads 1-3 evdev devices via epoll for efficient polling
- **Role-based mapping**: Deterministic mapping based on device role (stick/throttle/rudder)
- **Configurable device grabbing**: Per-device control via EVIOCGRAB
- **Discovery mode**: Shows event codes for each device role
- **TUI configuration tool**: Interactive terminal interface for binding management
- **Custom bindings**: Physical-to-virtual input remapping with transforms
- **Xbox 360 Controller compatibility**: Emulates Xbox 360 Controller (045e:028e) for ARMA Reforger
- **Mixed axis ranges**: 16-bit signed (-32768 to 32767) for sticks, 8-bit (0-255) for triggers
- **XInput-style buttons**: A, B, X, Y, LB, RB, Select, Start, LS, RS, Guide
- **Graceful failure handling**: Optional devices can be missing without breaking startup

## Virtual Controller Contract (Fixed)

**⚠️ This virtual device layout is fixed and cannot be changed.**  
ARMA binding stability depends on this contract remaining exactly as specified.

### Axes (8 total)
- **`ABS_X`, `ABS_Y`** - Left stick (-32768 to 32767)
- **`ABS_RX`, `ABS_RY`** - Right stick (-32768 to 32767) 
- **`ABS_Z`** - Left trigger analog (0-255) **(no digital click)**
- **`ABS_RZ`** - Right trigger analog (0-255) **(no digital click)**
- **`ABS_HAT0X`, `ABS_HAT0Y`** - D-pad hat (-1 to 1)

### Buttons (11 total)
- **Face buttons**: `BTN_SOUTH`, `BTN_EAST`, `BTN_WEST`, `BTN_NORTH`
- **Shoulder buttons**: `BTN_TL`, `BTN_TR` **(no digital triggers)**
- **System buttons**: `BTN_SELECT`, `BTN_START`, `BTN_MODE`
- **Stick buttons**: `BTN_THUMBL`, `BTN_THUMBR`

### Out of Scope
- **No digital trigger clicks** (BTN_TL2, BTN_TR2)
- **No macros or keyboard output** (future feature only after mapping TUI is stable)

## Device Mapping Strategy

### T.16000M Joystick (Role: stick)
**Virtual Device Mapping:**
- `ABS_X` → `ABS_X` (left stick X)
- `ABS_Y` → `ABS_Y` (left stick Y)

**Button Mapping:**
- `BTN_TRIGGER` → `BTN_SOUTH` (face button)
- `BTN_THUMB` → `BTN_EAST` (face button)
- `BTN_THUMB2` → `BTN_WEST` (face button)
- `BTN_TOP` → `BTN_NORTH` (face button)
- `BTN_TOP2` → `BTN_TL` (shoulder button)
- `BTN_PINKIE` → `BTN_TR` (shoulder button)
- `BTN_BASE` → `BTN_SELECT` (system button)
- `BTN_BASE2` → `BTN_START` (system button)
- `BTN_BASE3` → `BTN_THUMBL` (stick click)
- `BTN_BASE4` → `BTN_THUMBR` (stick click)

### TWCS Throttle (Role: throttle)
**Virtual Device Mapping:**
- `ABS_Z` → `ABS_Z` (left trigger analog)
- `ABS_HAT0X` → `ABS_HAT0X` (D-pad X)
- `ABS_HAT0Y` → `ABS_HAT0Y` (D-pad Y)

**Button Mapping:**
- `BTN_TRIGGER` → `BTN_SOUTH` (fallback if stick missing)
- `BTN_THUMB` → `BTN_EAST` (fallback if stick missing)
- `BTN_THUMB2` → `BTN_WEST` (fallback if stick missing)
- `BTN_TOP` → `BTN_NORTH` (fallback if stick missing)

### T-Rudder (Role: rudder)
**Virtual Device Mapping:**
- `ABS_RZ` → `ABS_RZ` (right trigger analog)

**Button Mapping:**
- `BTN_TRIGGER` → `BTN_SOUTH` (fallback if stick/throttle missing)

**Priority System:** Stick has highest priority, then throttle, then rudder. Ensures consistent mapping across different device combinations.

## Configuration

### New Schema
`~/.config/twcs-mapper/config.json`:
```json
{
  "uinput_name": "Thrustmaster ARMA Virtual",
  "grab": true,
  "inputs": [
    {
      "role": "stick",
      "by_id": "/dev/input/by-id/usb-Thrustmaster_T.16000M-event-joystick",
      "vendor": "044f",
      "product": "b10a",
      "optional": false
    },
    {
      "role": "throttle", 
      "by_id": "/dev/input/by-id/usb-Thrustmaster_Throttle_TWCS_Throttle-event-joystick",
      "vendor": "044f",
      "product": "b687",
      "optional": true
    },
    {
      "role": "rudder",
      "by_id": "",
      "vendor": "044f", 
      "product": "b679",
      "optional": true
    }
  ],
  "bindings": {
    "keys": [
      {
        "role": "stick",
        "src": 288,
        "dst": 304
      }
    ],
    "abs": [
      {
        "role": "throttle",
        "src": 0,
        "dst": 2,
        "invert": true,
        "deadzone": 5,
        "scale": 1.5
      }
    ]
  }
}
```

**Fields:**
- `uinput_name`: Virtual device name
- `grab`: Enable/disable EVIOCGRAB device grabbing
- `inputs`: Array of device configurations
  - `role`: Device role (stick/throttle/rudder)
  - `by_id`: Device path (empty = not available)
  - `vendor`: Expected USB vendor ID
  - `product`: Expected USB product ID  
  - `optional`: Whether device can be missing

### Custom Bindings Configuration

You can customize physical-to-virtual mappings by adding a `bindings` section to your config. If no bindings are specified, the mapper uses default mappings.

**Keys Bindings (EV_KEY):**
```json
"keys": [
  {
    "role": "stick",      // Device role: "stick", "throttle", or "rudder"
    "src": 288,          // Input event code (BTN_TRIGGER)
    "dst": 304           // Output event code (BTN_SOUTH)
  }
]
```

**Absolute Bindings (EV_ABS):**
```json
"abs": [
  {
    "role": "throttle",   // Device role: "stick", "throttle", or "rudder"
    "src": 0,            // Input event code (ABS_X)
    "dst": 2,            // Output event code (ABS_Z)
    "invert": true,       // Optional: invert the axis (default: false)
    "deadzone": 5,       // Optional: deadzone to apply (default: 0)
    "scale": 1.5         // Optional: scaling factor (default: 1.0)
  }
]
```

**Binding Rules:**
- Output ranges are automatically derived from destination axis code to match the frozen virtual controller contract:
  - `ABS_X`, `ABS_Y`, `ABS_RX`, `ABS_RY` → `[-32768, 32767]`
  - `ABS_Z`, `ABS_RZ` → `[0, 255]`
  - `ABS_HAT0X`, `ABS_HAT0Y` → `[-1, 1]`
- Invalid destination codes (outside frozen contract) are ignored with warnings
- Multiple physical inputs can map to the same virtual button (OR semantics)
- Axis priority: Stick > Throttle > Rudder for conflicting mappings

## Dependencies

- **libevdev** - Linux input device handling
- **ncurses** - Terminal UI for configuration tool
- **CMake 3.16+** - Build system
- **C++20** compiler

### Arch Linux Installation

```bash
# Install dependencies
sudo pacman -S libevdev ncurses cmake base-devel
```

## Building

```bash
# Clone or extract the source
cd ThrustyARMA

# Using build script (recommended)
./build.sh build

# Or manual CMake
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build . --config Release
```

## Usage

### 1. Detect and Configure Devices
```bash
./build/bin/twcs_select
```

This will:
- Scan `/dev/input/by-id` for Thrustmaster devices
- Auto-detect T.16000M (required), TWCS, and T-Rudder (optional)
- Show which devices were found
- Save configuration with detected device paths

### 2. Configuration Editor (TUI)
```bash
./build/bin/twcs_config
```

Launch the interactive terminal UI to manage device bindings:
- View and edit virtual slot mappings
- Add/delete bindings for keys and axes
- Configure transforms (invert, deadzone, scale)
- Save configuration to config.json
- Validate bindings against virtual controller contract

**TUI Controls:**
- **↑↓/jk**: Select virtual slot
- **←→**: Select binding within slot
- **a**: Add new binding (select role: stick/throttle/rudder)
- **d**: Delete selected binding
- **s**: Save configuration
- **q**: Quit (with unsaved changes prompt)
- **i**: Toggle invalid bindings view
- **?/h**: Show help

**Note**: The TUI creates placeholder bindings with src=0. Edit `config.json` manually to set correct source event codes.

### 3. Discovery Mode (Optional)
```bash
TWCS_EVENT=/dev/input/by-id/usb-Thrustmaster_...-event-joystick ./build.sh discovery
```

Shows event codes from the specified device for 10 seconds, useful for finding correct source codes for bindings. **Note:** TWCS_EVENT environment variable is required for discovery mode.

### 4. Run Mapper
```bash
# Load from config (recommended after running twcs_select)
./build.sh run

# Or override with specific device
TWCS_EVENT=/dev/input/by-id/...event-joystick ./build.sh run
```

The mapper will:
- Load config or use default Xbox 360 Controller settings
- If no config exists: Creates "Xbox 360 Controller (Virtual)" with default device paths
- If config exists (from twcs_select): Creates "Thrustmaster ARMA Virtual" with detected paths
- Validate all configured devices
- Exit if required T.16000M is missing or invalid
- Skip optional devices that aren't present
- Open all valid devices with optional grabbing
- Create Xbox 360 Controller-compatible virtual device
- Merge events using epoll for efficiency
- Log device statuses and virtual device creation

### 5. Configure ARMA Reforger

In ARMA Reforger, bind controls to the **virtual Xbox 360 Controller**. The virtual device will appear as:
- "Xbox 360 Controller (Virtual)" if running without twcs_select
- "Thrustmaster ARMA Virtual" if running after twcs_select

ARMA Reforger will automatically recognize the virtual device as a gamepad. Use the gamepad control scheme, not individual joystick bindings.

## Custom Bindings Guide

### Finding Event Codes

Use discovery mode to identify physical input event codes:
```bash
TWCS_EVENT=/dev/input/by-id/your-device-event-joystick ./build.sh discovery
```

Move your joystick/throttle buttons and axes to see their event codes during the 10-second capture window.

### Manual Configuration

You can also edit `~/.config/twcs-mapper/config.json` directly:

**Key Binding Example:**
```json
{
  "role": "stick",
  "src": 288,        // BTN_TRIGGER from discovery
  "dst": 304         // BTN_SOUTH (A button)
}
```

**Axis Binding Example:**
```json
{
  "role": "throttle",
  "src": 1,          // ABS_Y from discovery  
  "dst": 2,          // ABS_Z (left trigger)
  "invert": true,     // Invert axis direction
  "deadzone": 5,     // Deadzone before input registers
  "scale": 1.5       // Sensitivity multiplier
}
```

**Binding Priority System:**
- Multiple physical inputs can map to the same virtual slot (OR semantics for buttons)
- Axis priority: Stick > Throttle > Rudder when roles conflict
- Invalid destination codes are ignored with warnings

## Device Selection Logic

1. **T.16000M** must be present and validate (044f:b10a)
2. **TWCS Throttle** is optional - skipped if not found
3. **T-Rudder** is optional - skipped if not found
4. All devices are validated against their expected vendor:product IDs
5. Failed validation of required device → exit with error
6. Failed validation of optional device → skip with warning

## Event Processing

- **epoll loop**: Efficiently polls all open input device file descriptors
- **XInput mapping**: Converts Thrustmaster inputs to Xbox 360 Controller events
- **Device role mapping**: Each device applies its specific mapping (stick, throttle, rudder)
- **Mixed axis ranges**: 16-bit signed (-32768 to 32767) for sticks, 8-bit (0-255) for triggers
- **Sync emission**: SYN_REPORT sent after each event batch

## Troubleshooting

### Device Not Detected
```bash
# Check available devices
ls -la /dev/input/by-id/*event*

# Verify device properties
udevadm info -q property -n /dev/input/eventX | grep -E "(ID_VENDOR_ID|ID_MODEL_ID)"
```

### Missing Required Device
- Ensure T.16000M is connected and recognized by Linux
- Check `dmesg` for USB device detection messages
- Reconnect joystick if necessary

### Virtual Device Issues
```bash
# Check uinput module
lsmod | grep uinput

# Load if needed
sudo modprobe uinput

# Check permissions
ls -la /dev/uinput
```

### Permission Problems
```bash
# Add to input group
sudo usermod -a -G input $USER

# Test device access
ls -la /dev/input/by-id/*event*
```

## systemd Integration

The `twcs-mapper.service` template works for user services:

### Install
```bash
# Copy to user services
mkdir -p ~/.config/systemd/user
cp twcs-mapper.service ~/.config/systemd/user/

# Enable and start
systemctl --user daemon-reload
systemctl --user enable twcs-mapper.service
systemctl --user start twcs-mapper.service
```

### Management
```bash
# Check status
systemctl --user status twcs-mapper.service

# Stop manually
systemctl --user stop twcs-mapper.service

# View logs
journalctl --user -u twcs-mapper.service
```

## Project Structure

```
ThrustyARMA/
├── src/
│   ├── config.hpp         # Multi-device config interface
│   ├── config.cpp         # JSON parsing/writing
│   ├── bindings.hpp        # Binding resolution engine
│   ├── bindings.cpp        # Binding implementation
│   ├── twcs_select.cpp    # Multi-device detection
│   ├── twcs_mapper.cpp   # Multi-device merging daemon
│   └── twcs_config.cpp   # TUI configuration editor
├── CMakeLists.txt        # Build system
├── build.sh              # Build script
├── twcs-mapper.service   # systemd user unit template
└── README.md             # This file
```

## Runtime Behavior

- **Startup**: Load config or use defaults, validate devices, create Xbox 360 virtual device
- **Merging**: Continuously merge Thrustmaster events into Xbox 360 Controller output
- **Binding resolution**: Resolves conflicts using role-priority and OR semantics
- **Graceful shutdown**: Clean up on SIGINT/SIGTERM, release grabs, destroy virtual device
- **Resilience**: Optional device failures don't stop the daemon
- **Fixed virtual controller**: Always creates 8 axes + 11 buttons contract for ARMA stability
- **Configurable mappings**: Physical→virtual mappings customizable via TUI or JSON
- **Device priority**: Stick > throttle > rudder for button mappings

### Binding Resolution System

The mapper uses a sophisticated binding resolver with deterministic behavior:

**Button Semantics (OR Logic):**
- Multiple physical inputs can map to the same virtual button
- Button is pressed if any mapped physical input is pressed
- Each physical input tracked separately for accurate state

**Axis Semantics (Priority-based):**
- Lower priority axes are masked when higher priority axes are active
- Priority order: Stick > Throttle > Rudder
- Unset vs zero distinction prevents idle devices from masking active ones

**Transform Pipeline:**
- Invert: Reverse axis direction
- Deadzone: Ignore small movements around center
- Scale: Adjust sensitivity
- Range clamping: Enforce output bounds for destination type

## License

This project is licensed under the MIT License. See the [LICENSE](LICENSE) file for details.

Copyright (c) 2026 KelpMe