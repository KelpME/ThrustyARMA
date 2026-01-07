#ifndef INPUT_SOURCE_HPP
#define INPUT_SOURCE_HPP

#include <string>
#include <linux/input.h>
#include <libevdev-1.0/libevdev/libevdev.h>
#include <fcntl.h>
#include <unistd.h>
#include <limits.h>
#include <sys/ioctl.h>

struct InputSource {
    std::string role;
    std::string by_id;
    std::string resolved_path;
    int fd;
    struct libevdev* dev;
    bool grabbed;
    
    InputSource() : fd(-1), dev(nullptr), grabbed(false) {}
    
    int open_and_init(bool grab_enabled) {
        // Call close_and_free() first
        close_and_free();
        
        // Resolve by_id via realpath() to resolved_path
        char real_path[PATH_MAX];
        if (realpath(by_id.c_str(), real_path) == nullptr) {
            return -1;
        }
        resolved_path = real_path;
        
        // Open the device
        fd = open(resolved_path.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            return -1;
        }
        
        // Initialize libevdev
        int rc = libevdev_new_from_fd(fd, &dev);
        if (rc < 0) {
            close(fd);
            fd = -1;
            return -1;
        }
        
        // Grab if enabled
        if (grab_enabled) {
            if (ioctl(fd, EVIOCGRAB, 1) == 0) {
                grabbed = true;
            } else {
                grabbed = false;
                // Do NOT fail open
            }
        }
        
        return 0;
    }
    
    void close_and_free() {
        if (grabbed && fd >= 0) {
            ioctl(fd, EVIOCGRAB, 0);
        }
        if (dev) {
            libevdev_free(dev);
            dev = nullptr;
        }
        if (fd >= 0) {
            close(fd);
            fd = -1;
        }
        grabbed = false;
        resolved_path.clear();
    }
};

#endif // INPUT_SOURCE_HPP