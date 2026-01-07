#include "epoll_loop.hpp"
#include "input_source.hpp"
#include "bindings.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <cerrno>
#include <iostream>
#include <libevdev-1.0/libevdev/libevdev.h>
#include <algorithm>

EpollLoop::EpollLoop() : epoll_fd(-1) {
}

EpollLoop::~EpollLoop() {
    cleanup();
}

bool EpollLoop::initialize() {
    epoll_fd = epoll_create1(0);
    if (epoll_fd < 0) {
        perror("Failed to create epoll");
        return false;
    }
    return true;
}

void EpollLoop::cleanup() {
    if (epoll_fd >= 0) {
        close(epoll_fd);
        epoll_fd = -1;
    }
    active_devices.clear();
}

bool EpollLoop::add_device(InputSource* device) {
    if (!device || epoll_fd < 0 || device->fd < 0) {
        return false;
    }
    
    struct epoll_event event;
    event.events = EPOLLIN;
    event.data.fd = device->fd;
    
    if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, device->fd, &event) < 0) {
        perror("Failed to add device to epoll");
        return false;
    }
    
    // Remove if already present, then add to active devices
    active_devices.erase(
        std::remove(active_devices.begin(), active_devices.end(), device),
        active_devices.end()
    );
    active_devices.push_back(device);
    
    return true;
}

bool EpollLoop::remove_device(InputSource* device) {
    if (!device || epoll_fd < 0 || device->fd < 0) {
        return false;
    }
    
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, device->fd, nullptr);
    
    active_devices.erase(
        std::remove(active_devices.begin(), active_devices.end(), device),
        active_devices.end()
    );
    
    return true;
}

bool EpollLoop::rebuild_devices(const std::vector<InputSource*>& devices) {
    if (epoll_fd < 0) {
        return false;
    }
    
    // Clear all existing devices
    for (auto* device : active_devices) {
        if (device && device->fd >= 0) {
            epoll_ctl(epoll_fd, EPOLL_CTL_DEL, device->fd, nullptr);
        }
    }
    active_devices.clear();
    
    // Add all valid devices
    for (auto* device : devices) {
        if (device && device->fd >= 0) {
            add_device(device);
        }
    }
    
    return true;
}

int EpollLoop::run_once(int timeout_ms) {
    if (epoll_fd < 0) {
        return -1;
    }
    
    struct epoll_event events[8];
    int nfds = epoll_wait(epoll_fd, events, 8, timeout_ms);
    
    if (nfds < 0) {
        if (errno == EINTR) return 0;
        perror("epoll_wait failed");
        return -1;
    }
    
    for (int i = 0; i < nfds; i++) {
        InputSource* source_device = nullptr;
        for (auto* device : active_devices) {
            if (device && device->fd == events[i].data.fd) {
                source_device = device;
                break;
            }
        }
        
        if (!source_device) continue;
        
        // Handle disconnect events
        if (events[i].events & (EPOLLHUP | EPOLLERR)) {
            handle_disconnect(source_device);
            continue;
        }
        
        // Handle input events
        if (events[i].events & EPOLLIN) {
            handle_device_event(source_device);
        }
    }
    
    return nfds;
}

void EpollLoop::handle_device_event(InputSource* device) {
    if (!device || !device->dev || !event_callback) {
        return;
    }
    
    while (true) {
        struct input_event ev;
        int rc = libevdev_next_event(device->dev, LIBEVDEV_READ_FLAG_NORMAL, &ev);
        
        if (rc == -EAGAIN) {
            break; // No more events
        }
        
        if (rc == -ENODEV) {
            handle_disconnect(device);
            break;
        }
        
        if (rc != 0) {
            break; // Other error
        }
        
        // Call the event callback
        event_callback(device, ev);
    }
}

void EpollLoop::handle_disconnect(InputSource* device) {
    std::cout << "Disconnect " << device->role << ": " << device->resolved_path << "\n";
    device->close_and_free();
    
    if (disconnect_callback) {
        disconnect_callback(device);
    }
}