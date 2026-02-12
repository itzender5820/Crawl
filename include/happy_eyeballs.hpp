#ifndef HAPPY_EYEBALLS_HPP
#define HAPPY_EYEBALLS_HPP

#include <string>
#include <vector>
#include <chrono>
#include <sys/socket.h>
#include <netinet/in.h>

namespace crawl {

struct AddressInfo {
    int family;
    int socktype;
    int protocol;
    sockaddr_storage addr;
    socklen_t addrlen;
};

// RFC 8305 Happy Eyeballs v2 implementation
class HappyEyeballs {
public:
    static constexpr auto CONNECTION_ATTEMPT_DELAY = std::chrono::milliseconds(250);
    static constexpr auto RESOLUTION_DELAY = std::chrono::milliseconds(50);
    
    HappyEyeballs(const std::string& host, int port);
    ~HappyEyeballs();
    
    // Returns the first successfully connected socket
    int connect(std::chrono::milliseconds timeout);
    
private:
    std::string host_;
    int port_;
    std::vector<AddressInfo> ipv4_addrs_;
    std::vector<AddressInfo> ipv6_addrs_;
    
    bool resolve_addresses();
    int try_connect_parallel(const std::vector<AddressInfo>& addrs, 
                            std::chrono::milliseconds timeout);
    int attempt_connection(const AddressInfo& addr, 
                          std::chrono::milliseconds timeout);
    void set_nonblocking(int fd);
};

} // namespace crawl

#endif // HAPPY_EYEBALLS_HPP
