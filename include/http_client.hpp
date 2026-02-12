#pragma once

#include <string>
#include <vector>
#include <map>
#include <optional>
#include <chrono>
#include <memory>
#include <functional>
#include <atomic> // Added for std::atomic

extern std::atomic<size_t> g_downloaded;
extern std::atomic<size_t> g_total;
extern std::atomic<bool> g_progress_thread_running;

namespace crawl {

struct URL {
    std::string scheme;
    std::string host;
    int port;
    std::string path;
    std::string query;
    
    static std::optional<URL> parse(const std::string& url);
    std::string to_string() const;
};

struct Request {
    std::string method = "GET";
    URL url;
    std::map<std::string, std::string> headers;
    std::vector<uint8_t> body;
    std::chrono::milliseconds timeout{30000};
    bool follow_redirects = false;
    int max_redirects = 10;
    bool enable_compression = true;
    bool prefer_http2 = true;
    
    // Retry settings
    int max_retries = 0;
    std::chrono::milliseconds retry_delay{1000};
    bool exponential_backoff = true;
};

struct Response {
    int status_code;
    std::string status_message;
    std::map<std::string, std::string> headers;
    std::vector<uint8_t> body;
    std::chrono::milliseconds elapsed_time;
    
    // Extra metadata
    bool from_cache = false;
    bool used_http2 = false;
    bool was_compressed = false;
    int redirect_count = 0;
    size_t bytes_received = 0;
};

// Forward declarations
class ConnectionPool;
class DNSCache;
class RateLimiter;
class Statistics;

class HTTPClient {
public:
    HTTPClient();
    ~HTTPClient();
    
    // Simple GET request
    Response get(const std::string& url);
    
    // Simple POST request
    Response post(const std::string& url, const std::vector<uint8_t>& data);
    
    // Custom request
    Response request(const Request& req);
    
    // Batch requests (parallel execution)
    std::vector<Response> batch_request(const std::vector<Request>& requests, int max_parallel = 10);
    
    // Configuration
    void set_timeout(std::chrono::milliseconds timeout);
    void set_user_agent(const std::string& ua);
    void set_max_connections(int max);
    void enable_http2(bool enable);
    void enable_compression(bool enable);
    void set_rate_limit(double requests_per_second, size_t burst = 0);
    
    // Advanced features
    void enable_dns_cache(bool enable, std::chrono::seconds ttl = std::chrono::seconds(300));
    void warmup_dns(const std::vector<std::string>& hosts);
    
    // Statistics
    Statistics* get_stats();
    
private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace crawl
