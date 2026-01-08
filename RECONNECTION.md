# Device Reconnection & Hot-Plug Support

The TWCS mapper includes automatic device reconnection handling to maintain stability when devices are unplugged and reconnected during operation.

## How It Works

### Automatic Reconnection
The mapper continuously monitors device health and automatically attempts to reconnect disconnected devices:

- **Detection**: Devices are marked offline after 3 consecutive read failures or specific errors (ENODEV, EIO)
- **Backoff Strategy**: Exponential backoff starting at 500ms, doubling on each failure, capped at 2 seconds
- **Seamless Recovery**: When a device reconnects, it's automatically re-grabbed and added back to the event loop
- **Optional Devices**: Optional devices (throttle, rudder) can be missing without preventing mapper startup

### Device States

Each input device tracks:
- **online**: Current connection status
- **consecutive_read_failures**: Error counter for detecting disconnections
- **reconnect_backoff_ms**: Current retry delay
- **last_reconnect_attempt**: Timestamp of last reconnection attempt

### Reconnection Process

1. **Device Disconnection Detected**
   - Read errors trigger failure counter increment
   - After 3 failures or critical errors, device marked offline
   - File descriptor closed and libevdev freed

2. **Automatic Reconnection Attempts**
   - Every event loop iteration checks offline devices
   - Respects backoff delay to avoid hammering the system
   - Attempts to resolve by-id path and reopen device
   - Validates vendor/product IDs match configuration

3. **Successful Reconnection**
   - Device re-grabbed (if grab enabled)
   - Added back to epoll for event monitoring
   - Backoff reset to 500ms
   - Mapper continues without restart

## Behavior by Device Type

### Required Devices (stick)
- Mapper **will not start** if missing at startup
- If disconnected during operation, mapper continues running
- Automatically reconnects when device returns
- Virtual controller remains active

### Optional Devices (throttle, rudder)
- Mapper starts even if missing
- Can be hot-plugged after mapper starts
- Automatically detected and connected
- Bindings activate when device appears

## Testing Reconnection

To test device reconnection:

```bash
# Start the mapper
make start

# Verify it's running
make status

# Unplug a device (e.g., throttle)
# Watch the logs
journalctl --user -u twcs-mapper.service -f

# You should see:
# "throttle device disconnected (errno=..., failures=...)"

# Plug the device back in
# You should see:
# "Successfully reconnected throttle: /dev/input/eventX"
```

## Virtual Controller Stability

The virtual controller remains active even when physical devices disconnect:
- Virtual device persists throughout reconnection events
- ARMA continues to see a stable controller
- No game restart required
- Bindings reactivate automatically when devices return

## Limitations

### Current Implementation
- Only **required** devices trigger reconnection attempts
- Optional devices that disconnect stay offline (by design)
- Reconnection uses by-id paths (must remain stable)
- No notification system for reconnection events

### Future Enhancements
- Reconnection for optional devices
- Desktop notifications on disconnect/reconnect
- Configurable reconnection behavior per device
- Metrics tracking (reconnection count, uptime)

## Troubleshooting

### Device Won't Reconnect

**Check by-id path stability:**
```bash
ls -la /dev/input/by-id/ | grep Thrustmaster
```

**Verify device is detected:**
```bash
lsusb | grep Thrustmaster
```

**Check mapper logs:**
```bash
journalctl --user -u twcs-mapper.service -n 50
```

### Frequent Disconnections

If devices frequently disconnect:
1. Check USB cable quality
2. Try different USB port
3. Check USB power management settings
4. Review system logs for USB errors: `dmesg | grep -i usb`

### Manual Recovery

If automatic reconnection fails:
```bash
# Restart the mapper service
make restart

# Or reload the entire service
systemctl --user daemon-reload
systemctl --user restart twcs-mapper.service
```

## Implementation Details

### Code Location
- Main reconnection logic: `src/twcs_mapper.cpp`
  - `handle_device_error()`: Detects and marks devices offline
  - `attempt_device_reconnection()`: Tries to reconnect offline devices
  - `reopen_device()`: Reopens and validates device

### Event Loop Integration
The reconnection system integrates with the main epoll event loop:
1. epoll_wait() returns events from online devices
2. After processing events, check all offline devices
3. Attempt reconnection with backoff
4. Add successfully reconnected devices back to epoll

### Thread Safety
- Single-threaded design (no race conditions)
- All state changes happen in main event loop
- No locks or mutexes required
