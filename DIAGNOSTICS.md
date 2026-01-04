# TWCS Mapper Diagnostics Mode

The `--diagnostics` option provides comprehensive, non-interactive system health reporting for the TWCS mapper without modifying mapper behavior or configuration schema.

## Usage

```bash
twcs_mapper --diagnostics
```

## What It Reports

### Configuration
- uinput device name
- Device grab setting (enabled/disabled)
- Number of configured input devices

### Device Detection
For each configured device:
- Configuration details (role, path, expected vendor/product IDs, optional status)
- Path resolution status
- Actual device properties (name, vendor/product IDs)
- Validation results (detected/validation failed)
- Summary of detected vs required devices

### Bindings
- Number of bindings loaded from configuration vs defaults
- Active binding count
- Detailed binding mappings grouped by device role:
  - Source to destination mappings
  - Event type and code names
  - Transform parameters (invert, deadzone, scale) for axes

### Service State
- Service file existence check
- Current service status (active/inactive/failed)
- Service enabled status

### System Checks
- /dev/uinput accessibility
- User group membership (input group presence)
- Overall health status

## Exit Codes

- `0` - All checks pass, system is healthy
- `1` - Issues detected (missing required devices, no bindings, /dev/uinput inaccessible, etc.)

## Examples

### Healthy System
```
=== TWCS Mapper Diagnostics ===

CONFIGURATION:
  uinput_name: Thrustmaster ARMA Virtual
  device_grab: enabled
  configured_inputs: 5

DEVICE DETECTION:
  stick:
    configured_path: /dev/input/by-id/usb-Thrustmaster_TWCS_Throttle-event-joystick
    expected_vendor: 044f
    expected_product: b687
    optional: no
    resolved_path: /dev/input/event8
    device_name: Thrustmaster TWCS Throttle
    actual_vendor: 044f
    actual_product: b687
    status: DETECTED_OK

  Summary: 5/5 devices detected

BINDINGS:
  config_bindings: 2 loaded from config
  active_bindings: 2
    stick (2 bindings):
      ABS ABS_Z (2) -> ABS ABS_X (0)
      ABS ABS_RUDDER (7) -> ABS ABS_Y (1)

SERVICE STATE:
  service_file: EXISTS (/home/user/.config/systemd/user/twcs-mapper.service)
  service_status: active
  service_enabled: enabled

SYSTEM CHECKS:
  /dev/uinput: ACCESSIBLE
  user_groups: user input wheel (input group present)

HEALTH SUMMARY:
  STATUS: HEALTHY
```

### Issues Detected
```
HEALTH SUMMARY:
  ERROR: Required devices missing
  ERROR: Cannot access /dev/uinput
  STATUS: ISSUES_DETECTED
```

## Benefits

- **Non-interactive**: No user input required, fully automated
- **Comprehensive**: Covers all aspects of the mapper system
- **Machine-readable**: Clear status indicators and exit codes
- **Troubleshooting**: Detailed information for debugging
- **Production-safe**: No modifications to configuration or behavior
- **CI/CD ready**: Suitable for automated health checks and monitoring