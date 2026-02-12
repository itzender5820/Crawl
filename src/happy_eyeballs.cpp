#include "happy_eyeballs.hpp"
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <algorithm>
#include <vector>
#include <thread>

// TCP_NODELAY might not be defined on some systems
#ifndef TCP_NODELAY
#define TCP_NODELAY 1
#endif

namespace crawl {

HappyEyeballs::HappyEyeballs(const std::string& host, int port)
    : host_(host), port_(port) {
}

HappyEyeballs::~HappyEyeballs() = default;

bool HappyEyeballs::resolve_addresses() {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_ADDRCONFIG;
    
    struct addrinfo* result = nullptr;
    std::string port_str = std::to_string(port_);
    
    int ret = getaddrinfo(host_.c_str(), port_str.c_str(), &hints, &result);
    if (ret != 0) {
        return false;
    }
    
    // Separate IPv4 and IPv6 addresses
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        AddressInfo addr;
        addr.family = rp->ai_family;
        addr.socktype = rp->ai_socktype;
        addr.protocol = rp->ai_protocol;
        addr.addrlen = rp->ai_addrlen;
        std::memcpy(&addr.addr, rp->ai_addr, rp->ai_addrlen);
        
        if (rp->ai_family == AF_INET6) {
            ipv6_addrs_.push_back(addr);
        } else if (rp->ai_family == AF_INET) {
            ipv4_addrs_.push_back(addr);
        }
    }
    
    freeaddrinfo(result);
    return !ipv4_addrs_.empty() || !ipv6_addrs_.empty();
}

void HappyEyeballs::set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int HappyEyeballs::attempt_connection(const AddressInfo& addr, 
                                      std::chrono::milliseconds timeout) {
    int fd = socket(addr.family, addr.socktype | SOCK_NONBLOCK, addr.protocol);
    if (fd < 0) {
        return -1;
    }
    
    // Enable TCP_NODELAY for lower latency
    int flag = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
    
    // Enable SO_KEEPALIVE
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
    
    int ret = ::connect(fd, reinterpret_cast<const sockaddr*>(&addr.addr), addr.addrlen);
    
    if (ret == 0) {
        // Connected immediately (unlikely)
        return fd;
    }
    
    if (errno != EINPROGRESS) {
        ::close(fd);
        return -1;
    }
    
    // Wait for connection with timeout
    struct pollfd pfd;
    pfd.fd = fd;
    pfd.events = POLLOUT;
    
    int poll_ret = poll(&pfd, 1, timeout.count());
    
    if (poll_ret <= 0) {
        ::close(fd);
        return -1;
    }
    
    // Check if connection succeeded
    int error = 0;
    socklen_t len = sizeof(error);
    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &len);
    
    if (error != 0) {
        ::close(fd);
        return -1;
    }
    
    // Set back to blocking mode
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    
    return fd;
}

int HappyEyeballs::try_connect_parallel(const std::vector<AddressInfo>& addrs,
                                        std::chrono::milliseconds timeout) {
    if (addrs.empty()) {
        return -1;
    }
    
    std::vector<int> sockets;
    std::vector<struct pollfd> pfds;
    
    auto start_time = std::chrono::steady_clock::now();
    
    // Stagger connection attempts
    for (size_t i = 0; i < addrs.size(); ++i) {
        const auto& addr = addrs[i];
        
        int fd = socket(addr.family, addr.socktype | SOCK_NONBLOCK, addr.protocol);
        if (fd < 0) continue;
        
        // Set socket options
        int flag = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
        setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
        
        ::connect(fd, reinterpret_cast<const sockaddr*>(&addr.addr), addr.addrlen);
        
        sockets.push_back(fd);
        
        struct pollfd pfd;
        pfd.fd = fd;
        pfd.events = POLLOUT;
        pfds.push_back(pfd);
        
        // Delay between attempts (Happy Eyeballs staggering)
        if (i < addrs.size() - 1) {
            std::this_thread::sleep_for(CONNECTION_ATTEMPT_DELAY);
        }
        
        // Check if any connection succeeded
        int poll_ret = poll(pfds.data(), pfds.size(), 0);
        if (poll_ret > 0) {
            for (size_t j = 0; j < pfds.size(); ++j) {
                if (pfds[j].revents & POLLOUT) {
                    int error = 0;
                    socklen_t len = sizeof(error);
                    getsockopt(pfds[j].fd, SOL_SOCKET, SO_ERROR, &error, &len);
                    
                    if (error == 0) {
                        // Success! Close other sockets
                        int success_fd = pfds[j].fd;
                        for (size_t k = 0; k < sockets.size(); ++k) {
                            if (sockets[k] != success_fd) {
                                ::close(sockets[k]);
                            }
                        }
                        // Set back to blocking
                        int flags = fcntl(success_fd, F_GETFL, 0);
                        fcntl(success_fd, F_SETFL, flags & ~O_NONBLOCK);
                        return success_fd;
                    }
                }
            }
        }
        
        auto elapsed = std::chrono::steady_clock::now() - start_time;
        if (elapsed >= timeout) {
            break;
        }
    }
    
    // Wait for any connection to complete
    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto remaining = timeout - std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    
    if (remaining.count() > 0) {
        int poll_ret = poll(pfds.data(), pfds.size(), remaining.count());
        
        if (poll_ret > 0) {
            for (size_t i = 0; i < pfds.size(); ++i) {
                if (pfds[i].revents & POLLOUT) {
                    int error = 0;
                    socklen_t len = sizeof(error);
                    getsockopt(pfds[i].fd, SOL_SOCKET, SO_ERROR, &error, &len);
                    
                    if (error == 0) {
                        int success_fd = pfds[i].fd;
                        for (size_t k = 0; k < sockets.size(); ++k) {
                            if (sockets[k] != success_fd) {
                                ::close(sockets[k]);
                            }
                        }
                        int flags = fcntl(success_fd, F_GETFL, 0);
                        fcntl(success_fd, F_SETFL, flags & ~O_NONBLOCK);
                        return success_fd;
                    }
                }
            }
        }
    }
    
    // All failed, close all sockets
    for (int fd : sockets) {
        ::close(fd);
    }
    
    return -1;
}

int HappyEyeballs::connect(std::chrono::milliseconds timeout) {
    if (!resolve_addresses()) {
        return -1;
    }
    
    // RFC 8305: Prefer IPv6, but start IPv4 soon after
    // We'll try both in parallel with staggering
    
    auto start_time = std::chrono::steady_clock::now();
    
    // Start with IPv6 if available
    if (!ipv6_addrs_.empty()) {
        int fd = try_connect_parallel(ipv6_addrs_, RESOLUTION_DELAY);
        if (fd >= 0) {
            return fd;
        }
    }
    
    auto elapsed = std::chrono::steady_clock::now() - start_time;
    auto remaining = timeout - std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    
    // Try IPv4
    if (!ipv4_addrs_.empty() && remaining.count() > 0) {
        int fd = try_connect_parallel(ipv4_addrs_, remaining);
        if (fd >= 0) {
            return fd;
        }
    }
    
    // If IPv6 is still pending, give it more time
    elapsed = std::chrono::steady_clock::now() - start_time;
    remaining = timeout - std::chrono::duration_cast<std::chrono::milliseconds>(elapsed);
    
    if (!ipv6_addrs_.empty() && remaining.count() > 0) {
        return try_connect_parallel(ipv6_addrs_, remaining);
    }
    
    return -1;
}

} // namespace crawl
