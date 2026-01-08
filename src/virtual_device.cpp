#include "virtual_device.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cstdio>
#include <sys/ioctl.h>

VirtualDevice::VirtualDevice(const std::string& device_name) 
    : device_name(device_name), uinput_fd(-1), ready(false) {
}

VirtualDevice::~VirtualDevice() {
    cleanup();
}

bool VirtualDevice::initialize() {
    if (ready) {
        return true;
    }
    
    if (!setup_uinput()) {
        return false;
    }
    
    if (!enable_event_types()) {
        cleanup();
        return false;
    }
    
    if (!enable_buttons()) {
        cleanup();
        return false;
    }
    
    if (!enable_axes()) {
        cleanup();
        return false;
    }
    
    if (!create_device()) {
        cleanup();
        return false;
    }
    
    ready = true;
    return true;
}

void VirtualDevice::cleanup() {
    if (uinput_fd >= 0) {
        if (ready) {
            ioctl(uinput_fd, UI_DEV_DESTROY);
        }
        close(uinput_fd);
        uinput_fd = -1;
    }
    ready = false;
}

bool VirtualDevice::setup_uinput() {
    uinput_fd = open("/dev/uinput", O_RDWR | O_NONBLOCK);
    if (uinput_fd < 0) {
        perror("Failed to open uinput device");
        return false;
    }
    return true;
}

bool VirtualDevice::enable_event_types() {
    if (ioctl(uinput_fd, UI_SET_EVBIT, EV_KEY) < 0 ||
        ioctl(uinput_fd, UI_SET_EVBIT, EV_ABS) < 0) {
        perror("Failed to enable event types");
        return false;
    }
    return true;
}

bool VirtualDevice::enable_buttons() {
    // Virtual Controller Contract: Fixed 17 buttons for ARMA stability
    const int virtual_buttons[] = {
        BTN_SOUTH, BTN_EAST, BTN_WEST, BTN_NORTH,  // Face buttons (South/East/West/North)
        BTN_TL, BTN_TR,                              // Shoulder buttons
        BTN_TL2, BTN_TR2,                            // Trigger buttons (digital clicks)
        BTN_SELECT, BTN_START, BTN_MODE,             // System buttons
        BTN_THUMBL, BTN_THUMBR,                      // Stick buttons
        BTN_DPAD_UP, BTN_DPAD_DOWN, BTN_DPAD_LEFT, BTN_DPAD_RIGHT  // D-pad buttons
    };
    
    for (int btn : virtual_buttons) {
        if (ioctl(uinput_fd, UI_SET_KEYBIT, btn) < 0) {
            perror("Failed to enable virtual button");
            return false;
        }
    }
    return true;
}

bool VirtualDevice::enable_axes() {
    // Virtual Controller Contract: Fixed 8 axes for ARMA stability
    const int virtual_axes[] = {
        ABS_X, ABS_Y,        // Left stick
        ABS_RX, ABS_RY,      // Right stick  
        ABS_Z, ABS_RZ,       // Analog triggers (no digital clicks)
        ABS_HAT0X, ABS_HAT0Y // D-pad hat
    };
    
    for (int axis : virtual_axes) {
        if (ioctl(uinput_fd, UI_SET_ABSBIT, axis) < 0) {
            perror("Failed to enable virtual axis");
            return false;
        }
    }
    return true;
}

bool VirtualDevice::create_device() {
    // Set up uinput device for Virtual Controller (fixed contract for ARMA stability)
    struct uinput_user_dev uidev;
    memset(&uidev, 0, sizeof(uidev));
    
    strncpy(uidev.name, device_name.c_str(), UINPUT_MAX_NAME_SIZE);
    uidev.id.bustype = BUS_USB;
    uidev.id.vendor = 0x045e;  // Microsoft
    uidev.id.product = 0x028e; // Xbox 360 Controller
    uidev.id.version = 1;

    // Virtual Controller Contract: Fixed axis ranges for ARMA stability
    uidev.absmin[ABS_X] = -32768; uidev.absmax[ABS_X] = 32767;  // Left stick X
    uidev.absmin[ABS_Y] = -32768; uidev.absmax[ABS_Y] = 32767;  // Left stick Y
    uidev.absmin[ABS_RX] = -32768; uidev.absmax[ABS_RX] = 32767; // Right stick X
    uidev.absmin[ABS_RY] = -32768; uidev.absmax[ABS_RY] = 32767; // Right stick Y
    uidev.absmin[ABS_Z] = 0; uidev.absmax[ABS_Z] = 255;          // Left trigger
    uidev.absmin[ABS_RZ] = 0; uidev.absmax[ABS_RZ] = 255;        // Right trigger
    uidev.absmin[ABS_HAT0X] = -1; uidev.absmax[ABS_HAT0X] = 1;   // D-pad X
    uidev.absmin[ABS_HAT0Y] = -1; uidev.absmax[ABS_HAT0Y] = 1;   // D-pad Y

    // Create device
    if (write(uinput_fd, &uidev, sizeof(uidev)) < 0) {
        perror("Failed to write uinput device");
        return false;
    }

    if (ioctl(uinput_fd, UI_DEV_CREATE) < 0) {
        perror("Failed to create uinput device");
        return false;
    }

    return true;
}

bool VirtualDevice::write_event(const struct input_event& ev) {
    if (uinput_fd < 0 || !ready) {
        return false;
    }
    
    return write(uinput_fd, &ev, sizeof(ev)) >= 0;
}

bool VirtualDevice::emit_sync() {
    if (uinput_fd < 0 || !ready) {
        return false;
    }
    
    struct input_event sync_ev;
    memset(&sync_ev, 0, sizeof(sync_ev));
    sync_ev.type = EV_SYN;
    sync_ev.code = SYN_REPORT;
    sync_ev.value = 0;
    
    return write(uinput_fd, &sync_ev, sizeof(sync_ev)) >= 0;
}