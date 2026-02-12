#include "rate_limiter.hpp"
#include <thread>
#include <algorithm>

namespace crawl {

RateLimiter::RateLimiter(double requests_per_second, size_t burst)
    : rate_(requests_per_second),
      burst_(burst == 0 ? static_cast<size_t>(requests_per_second) : burst),
      last_refill_(std::chrono::steady_clock::now()) {
    
    if (rate_ > 0) {
        interval_ = std::chrono::nanoseconds(
            static_cast<long long>(1'000'000'000.0 / rate_)
        );
    }
}

void RateLimiter::refill() {
    if (rate_ <= 0) return; // Unlimited
    
    auto now = std::chrono::steady_clock::now();
    auto elapsed = now - last_refill_;
    
    // Calculate how many tokens to add
    auto tokens_to_add = std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count() / interval_.count();
    
    if (tokens_to_add > 0) {
        // Add tokens up to burst limit
        while (tokens_.size() < burst_ && tokens_to_add > 0) {
            tokens_.push_back(now);
            tokens_to_add--;
        }
        
        last_refill_ = now;
    }
}

void RateLimiter::acquire() {
    if (rate_ <= 0) return; // Unlimited
    
    std::unique_lock<std::mutex> lock(mutex_);
    
    while (true) {
        refill();
        
        if (!tokens_.empty()) {
            tokens_.pop_front();
            return;
        }
        
        // Wait for next token
        lock.unlock();
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        lock.lock();
    }
}

bool RateLimiter::try_acquire() {
    if (rate_ <= 0) return true; // Unlimited
    
    std::lock_guard<std::mutex> lock(mutex_);
    
    refill();
    
    if (!tokens_.empty()) {
        tokens_.pop_front();
        return true;
    }
    
    return false;
}

void RateLimiter::set_rate(double requests_per_second, size_t burst) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    rate_ = requests_per_second;
    burst_ = burst == 0 ? static_cast<size_t>(requests_per_second) : burst;
    
    if (rate_ > 0) {
        interval_ = std::chrono::nanoseconds(
            static_cast<long long>(1'000'000'000.0 / rate_)
        );
    }
    
    tokens_.clear();
    last_refill_ = std::chrono::steady_clock::now();
}

} // namespace crawl
