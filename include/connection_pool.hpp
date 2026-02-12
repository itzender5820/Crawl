#ifndef CONNECTION_POOL_HPP
#define CONNECTION_POOL_HPP

#include <string>
#include <map>
#include <vector>
#include <memory>
#include <mutex>
#include <chrono>

namespace crawl {

class TLSConnection;

struct PooledConnection {
    int socket_fd;
    std::unique_ptr<TLSConnection> tls;
    std::chrono::steady_clock::time_point last_used;
    bool in_use;
    
    PooledConnection() : socket_fd(-1), in_use(false) {}
};

class ConnectionPool {
public:
    ConnectionPool(int max_connections = 100, 
                   std::chrono::seconds idle_timeout = std::chrono::seconds(60));
    ~ConnectionPool();
    
    // Get a connection from pool or create new one
    std::shared_ptr<PooledConnection> acquire(const std::string& host, 
                                               int port, 
                                               bool use_tls);
    
    // Return connection to pool
    void release(const std::string& host, int port, 
                 std::shared_ptr<PooledConnection> conn);
    
    // Clean up idle connections
    void cleanup_idle();
    
private:
    struct PoolKey {
        std::string host;
        int port;
        bool use_tls;
        
        bool operator<(const PoolKey& other) const {
            if (host != other.host) return host < other.host;
            if (port != other.port) return port < other.port;
            return use_tls < other.use_tls;
        }
    };
    
    int max_connections_;
    std::chrono::seconds idle_timeout_;
    std::map<PoolKey, std::vector<std::shared_ptr<PooledConnection>>> pools_;
    std::mutex mutex_;
    
    std::shared_ptr<PooledConnection> create_connection(const std::string& host,
                                                         int port,
                                                         bool use_tls);
};

} // namespace crawl

#endif // CONNECTION_POOL_HPP
