#include "http_client.hpp"
#include "happy_eyeballs.hpp"
#include "tls_connection.hpp"
#include "connection_pool.hpp"
#include "dns_cache.hpp"
#include "compression.hpp"
#include "rate_limiter.hpp"
#include "stats.hpp"
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <fcntl.h>
#include <cstring>
#include <thread>
#include <future>
#include <queue>
#include <netdb.h>
#include <arpa/inet.h>

// MSG_NOSIGNAL doesn't exist on some systems
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

// Global atomic variables defined here
std::atomic<size_t> g_downloaded{0};
std::atomic<size_t> g_total{0};
std::atomic<bool> g_progress_thread_running{false};

namespace crawl {

std::optional<URL> URL::parse(const std::string& url) {
    // Optimized URL parser without regex
    URL result;
    
    // Find scheme
    size_t scheme_end = url.find("://");
    if (scheme_end == std::string::npos) {
        return std::nullopt;
    }
    
    result.scheme = url.substr(0, scheme_end);
    std::transform(result.scheme.begin(), result.scheme.end(), 
                   result.scheme.begin(), ::tolower);
    
    size_t pos = scheme_end + 3;
    
    // Find host/port boundary
    size_t path_start = url.find('/', pos);
    size_t query_start = url.find('?', pos);
    size_t host_end = std::min(path_start, query_start);
    if (host_end == std::string::npos) {
        host_end = url.length();
    }
    
    std::string host_port = url.substr(pos, host_end - pos);
    
    // Parse host and port
    size_t port_delim = host_port.find(':');
    if (port_delim != std::string::npos) {
        result.host = host_port.substr(0, port_delim);
        result.port = std::stoi(host_port.substr(port_delim + 1));
    } else {
        result.host = host_port;
        result.port = (result.scheme == "https") ? 443 : 80;
    }
    
    // Parse path and query
    if (path_start != std::string::npos) {
        if (query_start != std::string::npos && query_start > path_start) {
            result.path = url.substr(path_start, query_start - path_start);
            result.query = url.substr(query_start + 1);
        } else {
            result.path = url.substr(path_start);
        }
    } else {
        result.path = "/";
        if (query_start != std::string::npos) {
            result.query = url.substr(query_start + 1);
        }
    }
    
    return result;
}

std::string URL::to_string() const {
    std::string result = scheme + "://" + host;
    if ((scheme == "http" && port != 80) || (scheme == "https" && port != 443)) {
        result += ":" + std::to_string(port);
    }
    result += path;
    if (!query.empty()) {
        result += "?" + query;
    }
    return result;
}

class HTTPClient::Impl {
public:
    ConnectionPool pool_;
    std::unique_ptr<DNSCache> dns_cache_;
    std::unique_ptr<RateLimiter> rate_limiter_;
    Statistics stats_;
    
    std::string user_agent_;
    std::chrono::milliseconds default_timeout_;
    bool enable_http2_ = false;
    bool enable_compression_ = true;
    
    Impl() 
        : pool_(200, std::chrono::seconds(90)),
          rate_limiter_(std::make_unique<RateLimiter>(0, 0)),
          user_agent_("Crawl/1.0 (Ultra-Fast)"),
          default_timeout_(30000) {
    }
    
    Response execute_request(const Request& req);
    Response execute_with_retry(const Request& req);
    
private:
    std::string build_request(const Request& req);
    Response parse_response(const std::vector<uint8_t>& data, bool enable_decompression);
        std::vector<uint8_t> read_response(int socket_fd, TLSConnection* tls, std::chrono::milliseconds timeout, const std::string& method);
    
    int connect_with_dns_cache(const std::string& host, int port);
};

int HTTPClient::Impl::connect_with_dns_cache(const std::string& host, int port) {
    if (dns_cache_) {
        auto start = std::chrono::steady_clock::now();
        auto addrs = dns_cache_->resolve(host, port);
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        
        stats_.record_dns_lookup(elapsed, !addrs.empty());
        
        if (!addrs.empty()) {
            // Try to connect to cached addresses
            for (const auto& addr : addrs) {
                int fd = socket(addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
                if (fd < 0) continue;
                
                int flag = 1;
                setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
                setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &flag, sizeof(flag));
                
                socklen_t len = (addr.ss_family == AF_INET) ? sizeof(sockaddr_in) : sizeof(sockaddr_in6);
                ::connect(fd, reinterpret_cast<const sockaddr*>(&addr), len);
                
                // Quick timeout check
                struct pollfd pfd;
                pfd.fd = fd;
                pfd.events = POLLOUT;
                
                if (poll(&pfd, 1, 1000) > 0) {
                    int error = 0;
                    socklen_t err_len = sizeof(error);
                    getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &err_len);
                    
                    if (error == 0) {
                        // Set back to blocking
                        int flags = fcntl(fd, F_GETFL, 0);
                        fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
                        return fd;
                    }
                }
                
                ::close(fd);
            }
        }
    }
    
    // Fallback to Happy Eyeballs
    HappyEyeballs he(host, port);
    return he.connect(std::chrono::milliseconds(5000));
}

std::string HTTPClient::Impl::build_request(const Request& req) {
    std::string result;
    result.reserve(512); // Pre-allocate
    
    // Request line
    result += req.method;
    result += " ";
    result += req.url.path;
    if (!req.url.query.empty()) {
        result += "?";
        result += req.url.query;
    }
    result += " HTTP/1.1\r\n";
    
    // Host header
    result += "Host: ";
    result += req.url.host;
    if ((req.url.scheme == "http" && req.url.port != 80) ||
        (req.url.scheme == "https" && req.url.port != 443)) {
        result += ":";
        result += std::to_string(req.url.port);
    }
    result += "\r\n";
    
    // Check which headers are already set
    bool has_user_agent = false;
    bool has_connection = false;
    bool has_accept = false;
    bool has_accept_encoding = false;
    
    for (const auto& [key, value] : req.headers) {
        std::string lower_key = key;
        std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
        
        if (lower_key == "user-agent") has_user_agent = true;
        if (lower_key == "connection") has_connection = true;
        if (lower_key == "accept") has_accept = true;
        if (lower_key == "accept-encoding") has_accept_encoding = true;
        
        result += key;
        result += ": ";
        result += value;
        result += "\r\n";
    }
    
    // Add default headers
    if (!has_user_agent) {
        result += "User-Agent: ";
        result += user_agent_;
        result += "\r\n";
    }
    
    if (!has_connection) {
        result += "Connection: keep-alive\r\n";
    }
    
    if (!has_accept) {
        result += "Accept: */*\r\n";
    }
    
    if (!has_accept_encoding && req.enable_compression && enable_compression_) {
        result += "Accept-Encoding: ";
        result += Compression::get_accept_encoding_header();
        result += "\r\n";
    }
    
    if (!req.body.empty()) {
        result += "Content-Length: ";
        result += std::to_string(req.body.size());
        result += "\r\n";
    }
    
    result += "\r\n";
    
    return result;
}

std::vector<uint8_t> HTTPClient::Impl::read_response(
    int socket_fd, TLSConnection* tls, std::chrono::milliseconds inactivity_timeout, const std::string& method) {
    
    std::vector<uint8_t> response;
    response.reserve(65536);
    
    uint8_t buffer[131072]; // 128KB read buffer for large downloads
    bool headers_complete = false;
    size_t content_length = 0;
    bool chunked = false;
    size_t headers_end = 0;
    bool first_byte_received = false;
    
    auto request_start  = std::chrono::steady_clock::now();
    auto last_data_time = request_start; // reset every time data arrives
    
    while (true) {
        // INACTIVITY timeout: abort only if no data arrives for inactivity_timeout ms
        // This allows unlimited total download time as long as data keeps flowing
        auto now  = std::chrono::steady_clock::now();
        auto idle = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_data_time);
        if (idle >= inactivity_timeout) {
            break; // stalled / dead connection
        }
        
        ssize_t n;
        if (tls) {
            n = tls->recv(buffer, sizeof(buffer));
        } else {
            n = ::recv(socket_fd, buffer, sizeof(buffer), 0);
        }
        
        if (n < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
                continue;
            }
            break; // real error
        }
        
        if (n == 0) {
            break; // server closed connection cleanly — download complete
        }
        
        // Data received — reset inactivity timer
        last_data_time = std::chrono::steady_clock::now();
        
        // Record first byte timing once
        if (!first_byte_received) {
            first_byte_received = true;
            auto fb = std::chrono::duration_cast<std::chrono::milliseconds>(
                last_data_time - request_start);
            stats_.record_first_byte(fb);
        }
        
        response.insert(response.end(), buffer, buffer + n);
        g_downloaded.fetch_add(n); // Directly update global atomic
        
        // Parse headers on the first pass
        if (!headers_complete) {
            for (size_t i = 0; i + 3 < response.size(); ++i) {
                if (response[i] == '\r' && response[i+1] == '\n' &&
                    response[i+2] == '\r' && response[i+3] == '\n') {
                    headers_complete = true;
                    headers_end = i + 4;
                    
                    size_t pos = 0;
                    while (pos < headers_end && response[pos] != '\n') pos++;
                    pos++;
                    
                    while (pos < headers_end) {
                        size_t line_end = pos;
                        while (line_end < headers_end && response[line_end] != '\n') line_end++;
                        if (line_end == pos || response[pos] == '\r') break;
                        
                        size_t colon = pos;
                        while (colon < line_end && response[colon] != ':') colon++;
                        
                        if (colon < line_end) {
                            std::string key(response.begin() + pos, response.begin() + colon);
                            std::transform(key.begin(), key.end(), key.begin(), ::tolower);
                            
                            size_t vs = colon + 1;
                            while (vs < line_end && (response[vs]==' '||response[vs]=='\t')) vs++;
                            size_t ve = line_end;
                            if (ve > 0 && response[ve-1] == '\r') ve--;
                            
                            std::string value(response.begin() + vs, response.begin() + ve);
                            
                            if (key == "content-length") {
                                content_length = 0; for (char c : value) { if (c >= '0' && c <= '9') content_length = content_length * 10 + (c - '0'); else break; }
                            } else if (key == "transfer-encoding" &&
                                       value.find("chunked") != std::string::npos) {
                                chunked = true;
                            }
                        }
                        pos = line_end + 1;
                    }
                    break;
                }
            }
        }
        
        if (headers_complete) {
            // If method is HEAD, stop reading after headers
            if (method == "HEAD") {
                goto read_done;
            }
            
            if (chunked) {
                // Search for terminal chunk pattern: "0\r\n\r\n"
                for (size_t i = headers_end; i + 4 < response.size(); ++i) {
                    if (response[i]   == '0' && response[i+1] == '\r' &&
                        response[i+2] == '\n' && response[i+3] == '\r' &&
                        response[i+4] == '\n') {
                        goto read_done;
                    }
                }
            } else if (content_length > 0) {
                if (response.size() >= headers_end + content_length) {
                    goto read_done;
                }
            }
            // else: unknown length, keep reading until server closes or inactivity
        }
    }
    read_done:
    
    return response;
}

Response HTTPClient::Impl::parse_response(const std::vector<uint8_t>& data, bool enable_decompression) {
    Response resp;
    
    if (data.empty()) {
        resp.status_code = 0;
        return resp;
    }
    
    // Find end of headers
    size_t headers_end = 0;
    for (size_t i = 0; i + 3 < data.size(); ++i) {
        if (data[i] == '\r' && data[i+1] == '\n' &&
            data[i+2] == '\r' && data[i+3] == '\n') {
            headers_end = i + 4;
            break;
        }
    }
    
    if (headers_end == 0) {
        resp.status_code = 0;
        return resp;
    }
    
    // Parse status line
    size_t pos = 0;
    while (pos < headers_end && data[pos] != ' ') pos++; // Skip HTTP version
    pos++;
    
    // Parse status code
    size_t code_start = pos;
    while (pos < headers_end && data[pos] >= '0' && data[pos] <= '9') pos++;
    resp.status_code = std::stoi(std::string(data.begin() + code_start, data.begin() + pos));
    
    // Parse status message
    pos++; // Skip space
    size_t msg_start = pos;
    while (pos < headers_end && data[pos] != '\r') pos++;
    resp.status_message = std::string(data.begin() + msg_start, data.begin() + pos);
    pos += 2; // Skip \r\n
    
    // Parse headers
    CompressionType compression_type = CompressionType::None;
    
    while (pos < headers_end) {
        size_t line_end = pos;
        while (line_end < headers_end && data[line_end] != '\n') line_end++;
        
        if (line_end == pos || data[pos] == '\r') break;
        
        size_t colon = pos;
        while (colon < line_end && data[colon] != ':') colon++;
        
        if (colon < line_end) {
            std::string key(data.begin() + pos, data.begin() + colon);
            
            size_t val_start = colon + 1;
            while (val_start < line_end && (data[val_start] == ' ' || data[val_start] == '\t')) {
                val_start++;
            }
            size_t val_end = line_end;
            if (val_end > 0 && data[val_end - 1] == '\r') val_end--;
            
            std::string value(data.begin() + val_start, data.begin() + val_end);
            resp.headers[key] = value;
            
            // Check for compression
            if (enable_decompression) {
                std::string lower_key = key;
                std::transform(lower_key.begin(), lower_key.end(), lower_key.begin(), ::tolower);
                if (lower_key == "content-encoding") {
                    compression_type = Compression::detect_from_header(value);
                }
            }
        }
        
        pos = line_end + 1;
    }
    
    // Extract body
    if (headers_end < data.size()) {
        auto it = resp.headers.find("Transfer-Encoding");
        if (it != resp.headers.end() && it->second.find("chunked") != std::string::npos) {
            // Decode chunked encoding
            size_t chunk_pos = headers_end;
            while (chunk_pos < data.size()) {
                size_t line_end = chunk_pos;
                while (line_end + 1 < data.size() && 
                       !(data[line_end] == '\r' && data[line_end+1] == '\n')) {
                    line_end++;
                }
                
                if (line_end + 1 >= data.size()) break;
                
                std::string chunk_size_str(data.begin() + chunk_pos, data.begin() + line_end);
                size_t chunk_size = std::stoull(chunk_size_str, nullptr, 16);
                
                if (chunk_size == 0) break;
                
                chunk_pos = line_end + 2;
                
                if (chunk_pos + chunk_size <= data.size()) {
                    resp.body.insert(resp.body.end(), 
                                    data.begin() + chunk_pos,
                                    data.begin() + chunk_pos + chunk_size);
                }
                
                chunk_pos += chunk_size + 2;
            }
        } else {
            resp.body.assign(data.begin() + headers_end, data.end());
        }
    }
    
    resp.bytes_received = resp.body.size();
    
    // Decompress if needed
    if (enable_decompression && compression_type != CompressionType::None && !resp.body.empty()) {
        auto decompressed = Compression::decompress(resp.body, compression_type);
        if (decompressed) {
            resp.body = *decompressed;
            resp.was_compressed = true;
        }
    }
    
    return resp;
}

Response HTTPClient::Impl::execute_with_retry(const Request& req) {
    int attempts = 0;
    int max_attempts = req.max_retries + 1;
    
    while (attempts < max_attempts) {
        auto resp = execute_request(req);
        
        // Check if retry is needed
        if (resp.status_code > 0 && resp.status_code < 500) {
            return resp; // Success or client error (don't retry)
        }
        
        attempts++;
        
        if (attempts < max_attempts) {
            // Calculate backoff delay
            auto delay = req.retry_delay;
            if (req.exponential_backoff) {
                delay *= (1 << (attempts - 1)); // 2^(attempts-1)
            }
            
            std::this_thread::sleep_for(delay);
            stats_.record_error("retry");
        }
    }
    
    Response resp;
    resp.status_code = 0;
    stats_.record_error("max_retries_exceeded");
    return resp;
}

Response HTTPClient::Impl::execute_request(const Request& req) {
    auto start = std::chrono::steady_clock::now();
    
    // Apply rate limiting
    rate_limiter_->acquire();
    
    bool use_tls = (req.url.scheme == "https");
    
    auto conn = pool_.acquire(req.url.host, req.url.port, use_tls);
    if (!conn) {
        // ── NEW connection ─────────────────────────────────────────────────
        // 1. DNS + connect timing
        
        int fd = -1;
        
        // ── Step 1: DNS resolution (timed separately) ──────────────────────
        struct addrinfo hints{}, *res = nullptr;
        hints.ai_family   = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        
        auto dns_start = std::chrono::steady_clock::now();
        int gai_ret = getaddrinfo(req.url.host.c_str(),
                                  std::to_string(req.url.port).c_str(),
                                  &hints, &res);
        auto dns_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - dns_start);
        stats_.record_dns_lookup(dns_elapsed, false);
        
        // ── Step 2: TCP connect (timed separately, starts after DNS done) ──
        auto tcp_start = std::chrono::steady_clock::now();
        
        if (gai_ret == 0 && res != nullptr) {
            for (struct addrinfo* ai = res; ai != nullptr; ai = ai->ai_next) {
                int sfd = socket(ai->ai_family, SOCK_STREAM | SOCK_NONBLOCK, 0);
                if (sfd < 0) continue;
                
                int flag = 1;
                setsockopt(sfd, IPPROTO_TCP, TCP_NODELAY, &flag, sizeof(flag));
                setsockopt(sfd, SOL_SOCKET,  SO_KEEPALIVE, &flag, sizeof(flag));
                
                ::connect(sfd, ai->ai_addr, ai->ai_addrlen);
                
                struct pollfd pfd; pfd.fd = sfd; pfd.events = POLLOUT;
                if (poll(&pfd, 1, 10000) > 0) {
                    int err = 0; socklen_t el = sizeof(err);
                    getsockopt(sfd, SOL_SOCKET, SO_ERROR, &err, &el);
                    if (err == 0) {
                        // Back to blocking
                        int fl = fcntl(sfd, F_GETFL, 0);
                        fcntl(sfd, F_SETFL, fl & ~O_NONBLOCK);
                        fd = sfd;
                        break;
                    }
                }
                ::close(sfd);
            }
            freeaddrinfo(res);
        }
        
        auto tcp_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - tcp_start);
        stats_.record_tcp_handshake(tcp_elapsed);
        
        if (fd < 0) {
            Response resp;
            resp.status_code = 0;
            resp.elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start);
            stats_.record_error("connection_failed");
            return resp;
        }
        conn = std::make_shared<PooledConnection>();
        conn->socket_fd = fd;
        conn->in_use = true;
        
        if (use_tls) {
            conn->tls = std::make_unique<TLSConnection>(fd, req.url.host);
            if (!conn->tls->handshake()) {
                ::close(fd);
                Response resp;
                resp.status_code = 0;
                resp.elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);
                stats_.record_error("tls_handshake_failed");
                return resp;
            }
        }
        
        stats_.record_connection(false); // Created++
    } else {
        // ── REUSED connection ──────────────────────────────────────────────
        // DNS and TCP already happened on a previous request — record 0ms
        stats_.record_dns_lookup(std::chrono::milliseconds(0), true);   // cached
        stats_.record_tcp_handshake(std::chrono::milliseconds(0));       // already established
        stats_.record_connection(true); // Reused++
    }
    
    // Build and send request
    std::string request_str = build_request(req);
    
    ssize_t sent;
    if (conn->tls) {
        sent = conn->tls->send(request_str.data(), request_str.size());
    } else {
        sent = ::send(conn->socket_fd, request_str.data(), request_str.size(), MSG_NOSIGNAL);
    }
    
    if (sent < 0) {
        ::close(conn->socket_fd);
        Response resp;
        resp.status_code = 0;
        resp.elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start);
        stats_.record_error("send_failed");
        return resp;
    }
    
    // Send body if present
    if (!req.body.empty()) {
        if (conn->tls) {
            conn->tls->send(req.body.data(), req.body.size());
        } else {
            ::send(conn->socket_fd, req.body.data(), req.body.size(), MSG_NOSIGNAL);
        }
    }
    
    // Read response
    auto response_data = read_response(conn->socket_fd, conn->tls.get(), req.timeout, req.method);
    
    Response resp = parse_response(response_data, req.enable_compression);
    resp.elapsed_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start);

    // Return connection to pool
    pool_.release(req.url.host, req.url.port, conn);
    
    // Record statistics
    stats_.record_request(resp.elapsed_time, resp.bytes_received);
    
    // Handle redirects
    if (req.follow_redirects && resp.status_code >= 300 && resp.status_code < 400) {
        auto it = resp.headers.find("Location");
        if (it != resp.headers.end() && req.max_redirects > 0) {
            auto new_url = URL::parse(it->second);
            if (new_url) {
                Request new_req = req;
                new_req.url = *new_url;
                new_req.max_redirects = req.max_redirects - 1;
                resp = execute_request(new_req);
                resp.redirect_count++;
            }
        }
    }
    
    return resp;
}

// Public API implementation

HTTPClient::HTTPClient() : impl_(std::make_unique<Impl>()) {
}

HTTPClient::~HTTPClient() = default;

Response HTTPClient::get(const std::string& url) {
    auto parsed_url = URL::parse(url);
    if (!parsed_url) {
        Response resp;
        resp.status_code = 0;
        return resp;
    }
    
    Request req;
    req.method = "GET";
    req.url = *parsed_url;
    req.timeout = impl_->default_timeout_;
    
    return impl_->execute_request(req);
}

Response HTTPClient::post(const std::string& url, const std::vector<uint8_t>& data) {
    auto parsed_url = URL::parse(url);
    if (!parsed_url) {
        Response resp;
        resp.status_code = 0;
        return resp;
    }
    
    Request req;
    req.method = "POST";
    req.url = *parsed_url;
    req.body = data;
    req.timeout = impl_->default_timeout_;
    req.headers["Content-Type"] = "application/octet-stream";
    
    return impl_->execute_request(req);
}

Response HTTPClient::request(const Request& req) {
    if (req.max_retries > 0) {
        return impl_->execute_with_retry(req);
    }
    return impl_->execute_request(req);
}

std::vector<Response> HTTPClient::batch_request(const std::vector<Request>& requests, int max_parallel) {
    std::vector<Response> responses(requests.size());
    std::vector<std::future<Response>> futures;
    
    size_t idx = 0;
    while (idx < requests.size()) {
        // Launch parallel requests
        while (futures.size() < static_cast<size_t>(max_parallel) && idx < requests.size()) {
            size_t current_idx = idx;
            futures.push_back(std::async(std::launch::async, [this, &requests, current_idx]() {
                return this->request(requests[current_idx]);
            }));
            idx++;
        }
        
        // Wait for first completed
        if (!futures.empty()) {
            size_t completed_idx = responses.size() - futures.size();
            responses[completed_idx] = futures.front().get();
            futures.erase(futures.begin());
        }
    }
    
    // Wait for remaining
    for (auto& future : futures) {
        size_t completed_idx = responses.size() - futures.size();
        responses[completed_idx] = future.get();
    }
    
    return responses;
}

void HTTPClient::set_timeout(std::chrono::milliseconds timeout) {
    impl_->default_timeout_ = timeout;
}

void HTTPClient::set_user_agent(const std::string& ua) {
    impl_->user_agent_ = ua;
}

void HTTPClient::set_max_connections(int max) {
    // Update pool size (implementation in ConnectionPool would need this)
}

void HTTPClient::enable_http2(bool enable) {
    impl_->enable_http2_ = enable;
}

void HTTPClient::enable_compression(bool enable) {
    impl_->enable_compression_ = enable;
}

void HTTPClient::set_rate_limit(double requests_per_second, size_t burst) {
    impl_->rate_limiter_->set_rate(requests_per_second, burst);
}

void HTTPClient::enable_dns_cache(bool enable, std::chrono::seconds ttl) {
    if (enable) {
        impl_->dns_cache_ = std::make_unique<DNSCache>(ttl);
    } else {
        impl_->dns_cache_.reset();
    }
}

void HTTPClient::warmup_dns(const std::vector<std::string>& hosts) {
    if (impl_->dns_cache_) {
        for (const auto& host : hosts) {
            impl_->dns_cache_->warmup(host, 443);
            impl_->dns_cache_->warmup(host, 80);
        }
    }
}

Statistics* HTTPClient::get_stats() {
    return &impl_->stats_;
}

} // namespace crawl
