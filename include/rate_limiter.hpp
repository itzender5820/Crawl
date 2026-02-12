#pragma once

#include <chrono>
#include <mutex>
#include <deque>

namespace crawl {

class RateLimiter {
public:
    // requests_per_second: max requests per second (0 = unlimited)
    // burst: max burst size
    RateLimiter(double requests_per_second = 0, size_t burst = 0);
    
    // Wait until a request can be made (respecting rate limit)
    void acquire();
    
    // Try to acquire without blocking
    bool try_acquire();
    
    // Update rate limit
    void set_rate(double requests_per_second, size_t burst = 0);
    
    // Get current rate
    double get_rate() const { return rate_; }
    
private:
    double rate_;  // requests per second
    size_t burst_; // max burst size
    std::chrono::nanoseconds interval_;
    
    std::mutex mutex_;
    std::deque<std::chrono::steady_clock::time_point> tokens_;
    std::chrono::steady_clock::time_point last_refill_;
    
    void refill();
};

} // namespace crawl
