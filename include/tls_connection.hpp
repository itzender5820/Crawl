#ifndef TLS_CONNECTION_HPP
#define TLS_CONNECTION_HPP

#include <string>
#include <vector>
#include <memory>

namespace crawl {

class TLSConnection {
public:
    TLSConnection(int socket_fd, const std::string& hostname);
    ~TLSConnection();
    
    bool handshake();
    ssize_t send(const void* data, size_t len);
    ssize_t recv(void* data, size_t len);
    void close();
    
    bool is_connected() const { return connected_; }
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
    int socket_fd_;
    std::string hostname_;
    bool connected_;
};

} // namespace crawl

#endif // TLS_CONNECTION_HPP
