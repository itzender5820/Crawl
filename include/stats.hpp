#pragma once

#include <chrono>
#include <atomic>
#include <string>
#include <map>
#include <mutex>

namespace crawl {

class Statistics {
public:
    Statistics();
    
    // Request tracking
    void record_request(std::chrono::milliseconds latency, size_t bytes_received);
    void record_connection(bool reused);
    void record_error(const std::string& error_type);
    
    // DNS tracking
    void record_dns_lookup(std::chrono::milliseconds duration, bool cached);
    
    // Detailed timing
    void record_tcp_handshake(std::chrono::milliseconds duration);
    void record_first_byte(std::chrono::milliseconds duration);
    
    // Connection info
    void set_current_ip(const std::string& ip);
    void set_current_host(const std::string& host);
    void set_is_secure(bool secure);
    
    // Get statistics
    struct Stats {
        uint64_t total_requests;
        uint64_t total_errors;
        uint64_t total_bytes_received;
        uint64_t total_bytes_sent;
        
        uint64_t connections_created;
        uint64_t connections_reused;
        
        uint64_t dns_lookups;
        uint64_t dns_cache_hits;
        
        double avg_latency_ms;
        double min_latency_ms;
        double max_latency_ms;
        
        double avg_dns_ms;
        double avg_tcp_handshake_ms;
        double avg_first_byte_ms;
        double avg_last_byte_ms;
        
        std::string current_ip;
        std::string current_host;
        bool is_secure;
        
        std::map<std::string, uint64_t> error_counts;
    };
    
    Stats get_stats() const;
    void reset();
    
    // Print statistics
    void print(bool detailed = false) const;
    
private:
    std::atomic<uint64_t> total_requests_{0};
    std::atomic<uint64_t> total_errors_{0};
    std::atomic<uint64_t> total_bytes_received_{0};
    std::atomic<uint64_t> total_bytes_sent_{0};
    
    std::atomic<uint64_t> connections_created_{0};
    std::atomic<uint64_t> connections_reused_{0};
    
    std::atomic<uint64_t> dns_lookups_{0};
    std::atomic<uint64_t> dns_cache_hits_{0};
    
    std::atomic<uint64_t> total_latency_ms_{0};
    std::atomic<uint64_t> min_latency_ms_{999999};
    std::atomic<uint64_t> max_latency_ms_{0};
    
    std::atomic<uint64_t> total_dns_ms_{0};
    std::atomic<uint64_t> total_tcp_ms_{0};
    std::atomic<uint64_t> total_first_byte_ms_{0};
    
    std::atomic<uint64_t> tcp_handshake_count_{0};
    std::atomic<uint64_t> first_byte_count_{0};
    
    mutable std::mutex info_mutex_;
    std::string current_ip_;
    std::string current_host_;
    bool is_secure_ = false;
    
    mutable std::mutex error_mutex_;
    std::map<std::string, uint64_t> error_counts_;
    
    // Helper to print aligned line
    void print_line(const std::string& label, const std::string& value, int total_width = 64) const;
};

} // namespace crawl
