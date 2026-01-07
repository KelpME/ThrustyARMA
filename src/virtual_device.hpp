#ifndef VIRTUAL_DEVICE_HPP
#define VIRTUAL_DEVICE_HPP

#include <string>
#include <linux/uinput.h>

class VirtualDevice {
public:
    VirtualDevice(const std::string& device_name);
    ~VirtualDevice();
    
    bool initialize();
    void cleanup();
    
    int get_fd() const { return uinput_fd; }
    bool is_ready() const { return ready; }
    
    bool write_event(const struct input_event& ev);
    bool emit_sync();

private:
    std::string device_name;
    int uinput_fd;
    bool ready;
    
    bool setup_uinput();
    bool enable_event_types();
    bool enable_buttons();
    bool enable_axes();
    bool create_device();
};

#endif // VIRTUAL_DEVICE_HPP