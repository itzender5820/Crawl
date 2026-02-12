#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <chrono>
#include <mutex>
#include <netinet/in.h>

namespace crawl {

struct CachedAddress {
    std::vector<sockaddr_storage> addresses;
    std::chrono::steady_clock::time_point cached_at;
    std::chrono::seconds ttl;
};

class DNSCache {
public:
    DNSCache(std::chrono::seconds default_ttl = std::chrono::seconds(300));
    
    // Lookup cached address or resolve if not cached
    std::vector<sockaddr_storage> resolve(const std::string& host, int port);
    
    // Pre-populate cache
    void warmup(const std::string& host, int port);
    
    // Clear expired entries
    void cleanup();
    
    // Clear all entries
    void clear();
    
    // Get cache statistics
    struct Stats {
        size_t hits;
        size_t misses;
        size_t entries;
    };
    Stats get_stats() const;
    
private:
    std::unordered_map<std::string, CachedAddress> cache_;
    mutable std::mutex mutex_;
    std::chrono::seconds default_ttl_;
    
    mutable size_t hits_ = 0;
    mutable size_t misses_ = 0;
    
    std::string make_key(const std::string& host, int port) const;
    std::vector<sockaddr_storage> do_resolve(const std::string& host, int port);
};

} // namespace crawl
