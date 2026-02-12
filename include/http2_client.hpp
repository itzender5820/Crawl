#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <chrono>
#include <memory>

#ifdef HAVE_NGHTTP2
#include <nghttp2/nghttp2.h>
#endif

namespace crawl {

struct HTTP2Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::vector<uint8_t> body;
};

struct HTTP2Response {
    int status_code;
    std::map<std::string, std::string> headers;
    std::vector<uint8_t> body;
};

#ifdef HAVE_NGHTTP2

class HTTP2Session {
public:
    HTTP2Session(int socket_fd, const std::string& host);
    ~HTTP2Session();
    
    // Initialize HTTP/2 session
    bool init();
    
    // Send request and get response
    std::optional<HTTP2Response> request(const HTTP2Request& req);
    
    // Send multiple requests (multiplexed)
    std::vector<HTTP2Response> batch_request(const std::vector<HTTP2Request>& requests);
    
    // Check if connection is still alive
    bool is_alive() const;
    
private:
    int socket_fd_;
    std::string host_;
    nghttp2_session* session_ = nullptr;
    
    bool send_connection_header();
    bool send_settings();
};

#else

// Stub implementation when nghttp2 is not available
class HTTP2Session {
public:
    HTTP2Session(int, const std::string&) {}
    bool init() { return false; }
    std::optional<HTTP2Response> request(const HTTP2Request&) { return std::nullopt; }
    std::vector<HTTP2Response> batch_request(const std::vector<HTTP2Request>&) { return {}; }
    bool is_alive() const { return false; }
};

#endif

} // namespace crawl
