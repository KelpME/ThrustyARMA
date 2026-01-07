#ifndef EPOLL_LOOP_HPP
#define EPOLL_LOOP_HPP

#include <vector>
#include <functional>
#include <sys/epoll.h>

#include "input_source.hpp"

class EpollLoop {
public:
    using EventCallback = std::function<void(InputSource*, const struct input_event&)>;
    using DisconnectCallback = std::function<void(InputSource*)>;
    
    EpollLoop();
    ~EpollLoop();
    
    bool initialize();
    void cleanup();
    
    bool add_device(InputSource* device);
    bool remove_device(InputSource* device);
    bool rebuild_devices(const std::vector<InputSource*>& devices);
    
    int run_once(int timeout_ms = 250);
    
    void set_event_callback(EventCallback callback) { event_callback = callback; }
    void set_disconnect_callback(DisconnectCallback callback) { disconnect_callback = callback; }
    
    bool is_running() const { return epoll_fd >= 0; }

private:
    int epoll_fd;
    std::vector<InputSource*> active_devices;
    EventCallback event_callback;
    DisconnectCallback disconnect_callback;
    
    void handle_device_event(InputSource* device);
    void handle_disconnect(InputSource* device);
};

#endif // EPOLL_LOOP_HPP