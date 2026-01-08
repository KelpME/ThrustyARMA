# Button Signal Fix - Event Draining

## Problem
Button presses were being missed because the event loop only read **one event per device per epoll iteration**. When buttons were pressed/released quickly, or when multiple events were buffered by the kernel, only the first event was processed before moving to the next device or waiting for the next epoll cycle.

## Root Cause
The original code structure:
```cpp
int rc = libevdev_next_event(source_device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
if (rc == LIBEVDEV_READ_STATUS_SUCCESS) {
    // Process single event
}
```

This reads only ONE event, even if multiple events are buffered and ready to read.

## Solution
Changed to drain ALL available events from each device before moving on:
```cpp
while ((rc = libevdev_next_event(source_device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev)) == LIBEVDEV_READ_STATUS_SUCCESS) {
    // Process all available events
}
```

## Files Modified
- `src/twcs_mapper.cpp` - Main event loop (line ~1256)
- `src/twcs_mapper.cpp` - discovery_mode function (line ~307)
- `src/twcs_mapper.cpp` - diag_axes_mode function (line ~852)

## Benefits
1. **Zero missed signals** - All buffered events are processed immediately
2. **Lower latency** - Events don't wait for next epoll cycle
3. **Better responsiveness** - Rapid button presses are captured correctly
4. **Consistent behavior** - All code paths now use the same event draining pattern

## Technical Details
- `libevdev_next_event()` returns `LIBEVDEV_READ_STATUS_SUCCESS` when an event is available
- Returns `-EAGAIN` when no more events are buffered (normal condition)
- The while loop continues until the buffer is empty
- Each device's buffer is fully drained before processing the next device
- epoll still controls which devices have data ready, preventing busy-waiting

## Testing
After rebuilding with `make`, restart the mapper service:
```bash
make restart
```

Test by rapidly pressing buttons - all presses and releases should now be captured.
