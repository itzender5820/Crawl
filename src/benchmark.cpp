#include "http_client.hpp"
#include "stats.hpp"
#include <iostream>
#include <chrono>
#include <iomanip>
#include <vector>
#include <thread>

using namespace crawl;

void run_benchmark(const std::string& url, int num_requests, int concurrency) {
    std::cout << "\n=== Crawl Benchmark ===\n";
    std::cout << "URL:         " << url << "\n";
    std::cout << "Requests:    " << num_requests << "\n";
    std::cout << "Concurrency: " << concurrency << "\n";
    std::cout << "\n";
    
    HTTPClient client;
    client.enable_dns_cache(true);
    client.set_max_connections(concurrency * 2);
    
    // Warmup
    std::cout << "Warming up...\n";
    auto parsed_url = URL::parse(url);
    if (!parsed_url) {
        std::cerr << "Error: Invalid URL\n";
        return;
    }
    
    client.warmup_dns({parsed_url->host});
    
    // Run benchmark
    std::cout << "Running benchmark...\n";
    auto start = std::chrono::steady_clock::now();
    
    std::vector<Request> requests;
    for (int i = 0; i < num_requests; ++i) {
        Request req;
        req.method = "GET";
        req.url = *parsed_url;
        req.timeout = std::chrono::seconds(30);
        req.follow_redirects = true;
        requests.push_back(req);
    }
    
    auto responses = client.batch_request(requests, concurrency);
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    // Calculate statistics
    int success = 0;
    int errors = 0;
    size_t total_bytes = 0;
    double total_latency = 0;
    
    for (const auto& resp : responses) {
        if (resp.status_code >= 200 && resp.status_code < 400) {
            success++;
            total_bytes += resp.bytes_received;
            total_latency += resp.elapsed_time.count();
        } else {
            errors++;
        }
    }
    
    double duration_sec = duration.count() / 1000.0;
    double rps = num_requests / duration_sec;
    double avg_latency = total_latency / num_requests;
    double throughput_mbps = (total_bytes / duration_sec) / (1024 * 1024);
    
    std::cout << "\n=== Results ===\n";
    std::cout << std::fixed << std::setprecision(2);
    std::cout << "Total time:       " << duration_sec << " seconds\n";
    std::cout << "Requests/sec:     " << rps << "\n";
    std::cout << "Avg latency:      " << avg_latency << " ms\n";
    std::cout << "Success:          " << success << " (" << (success * 100.0 / num_requests) << "%)\n";
    std::cout << "Errors:           " << errors << "\n";
    std::cout << "Total data:       " << (total_bytes / (1024.0 * 1024.0)) << " MB\n";
    std::cout << "Throughput:       " << throughput_mbps << " MB/s\n";
    std::cout << "\n";
    
    client.get_stats()->print(false);
}

int main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cout << "Usage: " << argv[0] << " <URL> [requests] [concurrency]\n";
        std::cout << "Example: " << argv[0] << " https://example.com 1000 10\n";
        return 1;
    }
    
    std::string url = argv[1];
    int requests = (argc > 2) ? std::atoi(argv[2]) : 100;
    int concurrency = (argc > 3) ? std::atoi(argv[3]) : 10;
    
    run_benchmark(url, requests, concurrency);
    
    return 0;
}
