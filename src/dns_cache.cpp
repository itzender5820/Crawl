#include "dns_cache.hpp"
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <cstring>
#include <algorithm>

namespace crawl {

DNSCache::DNSCache(std::chrono::seconds default_ttl)
    : default_ttl_(default_ttl) {
}

std::string DNSCache::make_key(const std::string& host, int port) const {
    return host + ":" + std::to_string(port);
}

std::vector<sockaddr_storage> DNSCache::do_resolve(const std::string& host, int port) {
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_ADDRCONFIG;
    
    struct addrinfo* result = nullptr;
    std::string port_str = std::to_string(port);
    
    int ret = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (ret != 0) {
        return {};
    }
    
    std::vector<sockaddr_storage> addresses;
    for (struct addrinfo* rp = result; rp != nullptr; rp = rp->ai_next) {
        sockaddr_storage addr{};
        std::memcpy(&addr, rp->ai_addr, rp->ai_addrlen);
        addresses.push_back(addr);
    }
    
    freeaddrinfo(result);
    return addresses;
}

std::vector<sockaddr_storage> DNSCache::resolve(const std::string& host, int port) {
    std::string key = make_key(host, port);
    
    {
        std::lock_guard<std::mutex> lock(mutex_);
        
        auto it = cache_.find(key);
        if (it != cache_.end()) {
            auto now = std::chrono::steady_clock::now();
            auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.cached_at);
            
            if (age < it->second.ttl) {
                hits_++;
                return it->second.addresses;
            } else {
                // Entry expired
                cache_.erase(it);
            }
        }
        
        misses_++;
    }
    
    // Resolve DNS
    auto addresses = do_resolve(host, port);
    
    if (!addresses.empty()) {
        std::lock_guard<std::mutex> lock(mutex_);
        CachedAddress cached;
        cached.addresses = addresses;
        cached.cached_at = std::chrono::steady_clock::now();
        cached.ttl = default_ttl_;
        cache_[key] = std::move(cached);
    }
    
    return addresses;
}

void DNSCache::warmup(const std::string& host, int port) {
    resolve(host, port);
}

void DNSCache::cleanup() {
    std::lock_guard<std::mutex> lock(mutex_);
    
    auto now = std::chrono::steady_clock::now();
    
    for (auto it = cache_.begin(); it != cache_.end(); ) {
        auto age = std::chrono::duration_cast<std::chrono::seconds>(now - it->second.cached_at);
        if (age >= it->second.ttl) {
            it = cache_.erase(it);
        } else {
            ++it;
        }
    }
}

void DNSCache::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.clear();
}

DNSCache::Stats DNSCache::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return Stats{hits_, misses_, cache_.size()};
}

} // namespace crawl
