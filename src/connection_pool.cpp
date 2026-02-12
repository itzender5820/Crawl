#include "connection_pool.hpp"
#include "happy_eyeballs.hpp"
#include "tls_connection.hpp"
#include <sys/socket.h>
#include <unistd.h>
#include <algorithm>
#include <errno.h>

namespace crawl {

ConnectionPool::ConnectionPool(int max_connections, std::chrono::seconds idle_timeout)
    : max_connections_(max_connections), idle_timeout_(idle_timeout) {
}

ConnectionPool::~ConnectionPool() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    for (auto& pool_pair : pools_) {
        for (auto& conn : pool_pair.second) {
            if (conn->socket_fd >= 0) {
                ::close(conn->socket_fd);
            }
        }
    }
}

std::shared_ptr<PooledConnection> ConnectionPool::create_connection(
    const std::string& host, int port, bool use_tls) {
    
    HappyEyeballs he(host, port);
    int fd = he.connect(std::chrono::milliseconds(10000));
    
    if (fd < 0) {
        return nullptr;
    }
    
    auto conn = std::make_shared<PooledConnection>();
    conn->socket_fd = fd;
    conn->in_use = true;
    conn->last_used = std::chrono::steady_clock::now();
    
    if (use_tls) {
        conn->tls = std::make_unique<TLSConnection>(fd, host);
        if (!conn->tls->handshake()) {
            ::close(fd);
            return nullptr;
        }
    }
    
    return conn;
}

std::shared_ptr<PooledConnection> ConnectionPool::acquire(
    const std::string& host, int port, bool use_tls) {
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    PoolKey key{host, port, use_tls};
    
    // Only return an EXISTING idle connection — never create a new one here.
    // New connections must be created by the caller so timing can be recorded.
    auto it = pools_.find(key);
    if (it != pools_.end()) {
        auto& pool = it->second;
        for (int i = (int)pool.size() - 1; i >= 0; --i) {
            auto& conn = pool[i];
            if (!conn || conn->in_use) continue;
            
            // Check if connection is still alive
            char buf[1];
            ssize_t ret = ::recv(conn->socket_fd, buf, 1, MSG_PEEK | MSG_DONTWAIT);
            if (ret == 0 || (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK)) {
                // Dead — close and remove
                ::close(conn->socket_fd);
                pool.erase(pool.begin() + i);
                continue;
            }
            
            conn->in_use = true;
            conn->last_used = std::chrono::steady_clock::now();
            return conn;
        }
    }
    
    // No existing connection available — caller creates a new one with timing
    return nullptr;
}

void ConnectionPool::release(const std::string& host, int port,
                             std::shared_ptr<PooledConnection> conn) {
    if (!conn || conn->socket_fd < 0) {
        return;
    }
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    PoolKey key{host, port, conn->tls != nullptr};
    
    conn->in_use = false;
    conn->last_used = std::chrono::steady_clock::now();
    
    // Add to pool
    auto& pool = pools_[key];
    
    // Check if we're at capacity
    int total_conns = 0;
    for (const auto& p : pools_) {
        total_conns += p.second.size();
    }
    
    if (total_conns >= max_connections_) {
        // Close the oldest idle connection
        ::close(conn->socket_fd);
        return;
    }
    
    pool.push_back(conn);
}

void ConnectionPool::cleanup_idle() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto now = std::chrono::steady_clock::now();
    
    for (auto& pool_pair : pools_) {
        auto& pool = pool_pair.second;
        
        pool.erase(
            std::remove_if(pool.begin(), pool.end(),
                [&](const std::shared_ptr<PooledConnection>& conn) {
                    if (!conn->in_use) {
                        auto idle_time = std::chrono::duration_cast<std::chrono::seconds>(
                            now - conn->last_used);
                        
                        if (idle_time >= idle_timeout_) {
                            ::close(conn->socket_fd);
                            return true;
                        }
                    }
                    return false;
                }),
            pool.end());
    }
}

} // namespace crawl
