#include "http_client.hpp"
#include "stats.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <cstring>
#include <getopt.h>
#include <iomanip>
#include <chrono>
#include <atomic>
#include <thread>
#include <vector>
#include <mutex>
#include <algorithm>
#include <sys/ioctl.h>
#include <unistd.h>
#include <cstdio>
#include <map>

// Define our Color Palette
#define RESET   "\033[0m"
#define CYAN    "\033[36m"
#define GREY    "\033[38;5;244m"
#define PINK    "\033[38;5;205m"
#define ORANGE  "\033[38;5;208m"
#define FLUORE  "\033[38;5;118m" // Fluorescent Green/Yellow
#define RED     "\033[31m"
#define YELLOW  "\033[33m"
#define GREEN   "\033[92m" // Added GREEN macro

using namespace crawl;

// ─────────────────────────────────────────────────────────────────────────────
// Terminal width via ioctl
// ─────────────────────────────────────────────────────────────────────────────
static int get_terminal_width() {
    struct winsize w;
    if (ioctl(STDERR_FILENO, TIOCGWINSZ, &w) == -1 || w.ws_col == 0)
        return 80;
    return (int)w.ws_col;
}

// ─────────────────────────────────────────────────────────────────────────────
// Smart size formatter  e.g. 102400 → "100.00KB"
// ─────────────────────────────────────────────────────────────────────────────
static std::string fmt_size(size_t bytes) {
    char val_buf[16]; // Buffer for the numeric value
    char unit_buf[8]; // Buffer for the unit (B, KB, MB, GB)

    if (bytes < 1024) {
        snprintf(val_buf, sizeof(val_buf), "%zu", bytes);
        snprintf(unit_buf, sizeof(unit_buf), "B");
    } else if (bytes < 1024*1024) {
        snprintf(val_buf, sizeof(val_buf), "%.2f", bytes/1024.0);
        snprintf(unit_buf, sizeof(unit_buf), "KB");
    } else if (bytes < (size_t)1024*1024*1024) {
        snprintf(val_buf, sizeof(val_buf), "%.2f", bytes/(1024.0*1024.0));
        snprintf(unit_buf, sizeof(unit_buf), "MB");
    } else {
        snprintf(val_buf, sizeof(val_buf), "%.2f", bytes/(1024.0*1024.0*1024.0));
        snprintf(unit_buf, sizeof(unit_buf), "GB");
    }
    return std::string(val_buf) + std::string(unit_buf);
}

static std::string get_plain_size_string(size_t bytes) {
    return fmt_size(bytes);
}

// ─────────────────────────────────────────────────────────────────────────────
// draw_progress — always overwrites the same line with \r
// ─────────────────────────────────────────────────────────────────────────────

static void draw_progress(size_t downloaded, size_t total) {
    int term_w = get_terminal_width();

    // --- Non-colored versions for visible length calculation ---
    std::string size_info_plain_downloaded = get_plain_size_string(downloaded);
    std::string size_info_plain_total = (total > 0 ? get_plain_size_string(total) : "--b");

    char perc_buf_plain[32];
    if (total > 0) {
        double percentage = (double)downloaded / total * 100.0;
        snprintf(perc_buf_plain, sizeof(perc_buf_plain), "%.1f%%", percentage);
    } else {
        snprintf(perc_buf_plain, sizeof(perc_buf_plain), "--%%");
    }
    std::string perc_info_plain = perc_buf_plain;

    // --- Calculate reserved space based on visible lengths ---
    // Target format: "Progress:[bar] [XX.X%] [Current/Total]"
    // Fixed characters: "Progress:[" (9) + "] [" (3) + "]" (1) + " [" (2) + "/" (1) + "]" (1) = 17
    size_t reserved_visible_len = std::strlen("Progress:[") + std::strlen("] [") + perc_info_plain.length() +
                                  std::strlen("] [") + size_info_plain_downloaded.length() +
                                  std::strlen("/") + size_info_plain_total.length() + std::strlen("]");
    
    int bar_width = term_w - (int)reserved_visible_len - 1; // -1 for a final space after ]
    if (bar_width < 0) bar_width = 0;
    if (bar_width < 10) bar_width = 10; // Minimum bar width

    // Start printing the bar
    printf("\r%sProgress:%s%s[%s", CYAN, RESET, GREY, RESET); // Progress:[

    if (total > 0) { // Known-size bar
        double percentage = (double)downloaded / total * 100.0;
        int hashes_to_fill = (int)((percentage / 100.0) * bar_width);
        if (hashes_to_fill < 0) hashes_to_fill = 0; 
        
        printf("%s", FLUORE);
        for(int i = 0; i < hashes_to_fill; ++i) printf("#");
        
        printf("%s", RED); // Empty space color
        for(int i = 0; i < (bar_width - hashes_to_fill); ++i) printf("-");
        
    } else { // Blind bar
        std::string msg = "content length not provided by site";
        size_t msg_visible_len = msg.length();

        if (bar_width < (int)msg_visible_len) {
             printf("%s%s%s", YELLOW, msg.substr(0, bar_width).c_str(), RED); // Truncate, use RED for rest
             for(int i=0; i< (bar_width - (int)msg_visible_len); ++i) printf("-");
        } else {
            int padding_left = (bar_width - (int)msg_visible_len) / 2;
            int padding_right = bar_width - (int)msg_visible_len - padding_left;

            printf("%s", RED); for(int i=0; i<padding_left; ++i) printf("-");
            printf("%s%s%s", YELLOW, msg.c_str(), RED);
            for(int i=0; i<padding_right; ++i) printf("-");
        }
    }
    
    printf("%s]%s", GREY, RESET); // Closing bracket for the bar

    // Print percentage and size info
    printf(" %s[%s%s]%s %s[%s%s%s/%s%s]%s%s\033[K",
        GREY, // Color for first [
        PINK, perc_info_plain.c_str(), RESET, // XX.X%
        GREY, // Color for next ] and [
        PINK, size_info_plain_downloaded.c_str(), // Current
        ORANGE, size_info_plain_total.c_str(), RESET, // /Total
        GREY, RESET); // Final ] and reset

    fflush(stderr);
}

// New function for the progress bar thread
static void progress_thread_loop() {
    while (g_progress_thread_running) {
        draw_progress(g_downloaded, g_total);
        // Sleep for a short duration to control update rate
        std::this_thread::sleep_for(std::chrono::milliseconds(100)); 
    }
    // Final draw to ensure 100% is displayed cleanly
    draw_progress(g_downloaded, g_total);
    fprintf(stderr, "\n"); // Newline after the final bar
    fflush(stderr); // Ensure it's printed
}

// ─────────────────────────────────────────────────────────────────────────────
// Parallel range download
//   Splits file into `num_pipes` segments using HTTP Range requests.
//   Falls back to single-threaded if server doesn't support Accept-Ranges.
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<uint8_t> parallel_download(
        const std::string& url_str,
        size_t content_length,
        int num_pipes,
        bool show_progress,
        HTTPClient& client,
        std::string method,
        std::map<std::string,std::string> extra_headers,
        int max_time,
        bool no_compress) {

    if (num_pipes < 2 || content_length == 0) {
        return {}; // caller handles single-pipe
    }

    // Split into equal chunks
    size_t segment_size = content_length / num_pipes;

    if (show_progress) {
        g_total = content_length;
        fprintf(stderr, "Parallel download: %d pipes\n", num_pipes);
    }

    std::vector<std::vector<uint8_t>> parts(num_pipes);
    std::mutex err_mtx;
    bool any_failed = false;

    std::vector<std::thread> threads;
    threads.reserve(num_pipes);

    for (int i = 0; i < num_pipes; ++i) {
        threads.emplace_back([&, i, segment_size]() {
            HTTPClient pipe_client;
            auto parsed = URL::parse(url_str);
            if (!parsed) return;

            Request req;
            req.method  = method;
            req.url     = *parsed;
            req.headers = extra_headers;
            req.timeout = std::chrono::seconds(max_time);
            req.enable_compression = !no_compress;

                                    // Set Range header

                                    size_t start_byte = i * segment_size;

                                    char range_hdr[64];

                                    std::string range_debug_str; // For debug message - KEEP as it's part of the new range logic

                                    if (i == num_pipes - 1) { // Last pipe

                                        snprintf(range_hdr, sizeof(range_hdr), "bytes=%zu-", start_byte);

                                        range_debug_str = std::to_string(start_byte) + "-";

                                    } else {

                                        size_t end_byte = (i + 1) * segment_size - 1;

                                        snprintf(range_hdr, sizeof(range_hdr), "bytes=%zu-%zu", start_byte, end_byte);

                                        range_debug_str = std::to_string(start_byte) + "-" + std::to_string(end_byte);

                                    }

                                    req.headers["Range"] = range_hdr;

                        

                                    Response resp;

                                    int retries = 0;

                                    const int MAX_RETRIES = 3;

                                    do {

                                        if (retries > 0) {

                                            std::this_thread::sleep_for(std::chrono::seconds(1)); // Simple delay

                                        }

                                        resp = pipe_client.request(req);

                                        retries++;

                                    } while (resp.status_code != 206 && retries < MAX_RETRIES);

                        

                                    if (resp.status_code == 206) {

                                        parts[i] = std::move(resp.body);

                                        // g_downloaded.fetch_add(resp.bytes_received); // This was redundant

                                    } else {

                                        std::lock_guard<std::mutex> lk(err_mtx);

                                        any_failed = true;

                                    }
        });
    }

    for (auto& t : threads) t.join();

    if (any_failed || show_progress) {
        // check if we actually got parts
    }

    // Assemble in order
    std::vector<uint8_t> result;
    result.reserve(content_length);
    for (int i = 0; i < num_pipes; ++i) {
        result.insert(result.end(), parts[i].begin(), parts[i].end());
    }

    return result;
}

// ─────────────────────────────────────────────────────────────────────────────
// format_size for verbose output (uses same logic)
// ─────────────────────────────────────────────────────────────────────────────
static std::string format_size(size_t bytes) {
    return fmt_size(bytes);
}

void print_usage(const char* prog_name) {
    // ANSI color codes are now defined globally as macros, use them directly
    
    std::cout << "\n";
    std::cout << CYAN;
    std::cout << "                                                    /$$\n";
    std::cout << "                                                   | $$\n";
    std::cout << "          /$$$$$$$  /$$$$$$  /$$$$$$  /$$  /$$  /$$| $$\n";
    std::cout << "         /$$_____/ /$$__  $$|____  $$| $$ | $$ | $$| $$\n";
    std::cout << "        | $$      | $$  \\__/ /$$$$$$$| $$ | $$ | $$| $$\n";
    std::cout << "        | $$      | $$      /$$__  $$| $$ | $$ | $$| $$\n";
    std::cout << "        |  $$$$$$$| $$     |  $$$$$$$|  $$$$$/$$$$/| $$\n";
    std::cout << "         \\_______/|__/      \\_______/ \\_____/\\___/ |__/\n";
    std::cout << RESET;
    std::cout << "                                │\n";
    std::cout << GREY << "                   ​╔​═​═​═​═​═​═​═​═​═​═​​═​═" << RESET << "╪" << GREY << "​═​═​═​═​═​═​═​═​═​═​​══╗\n";
    std::cout << "                   ​║            " << RESET << "│" << GREY << "            ​║\n";
    std::cout << YELLOW << "      ╭──────╮     " << GREY << "​║          " << RESET << "/ ​┴ \\" << GREY << "          ​║     " << YELLOW << "╭──────╮\n";
    std::cout << "      ├──────┤     " << GREY << "​║        " << RED << "\\_\\(_)/_/" << GREY << "       ​ ║     " << YELLOW << "├──────┤\n";
    std::cout << "      ├──────┾​═​═​═​═​═​╝        " << RED << "_//   \\\\_" << GREY << "        ╚​═​═​═​═​═" << YELLOW << "┽──────┤\n";
    std::cout << "      ╰──────╯               " << RED << "/     \\" << GREY << "               " << YELLOW << "╰──────╯\n";
    std::cout << RESET << "\n\n";
    
    std::cout << GREY << "╭───────────────────────────────────────────────────────────────╮\n";
    std::cout << "│" << RESET << "                   Crawl - HTTP Client                         " << GREY << "│\n";
    std::cout << "├───────────────────────────────────────────────────────────────┤\n";
    std::cout << "│" << RESET << "                 Usage: crawl [options] <URL>                  " << GREY << "│\n";
    std::cout << "├───────────────────────────────────────────────────────────────┤\n";
    std::cout << "│  " << RESET << "BASIC OPTIONS" << GREY << "                                                │\n";
    std::cout << "│  " << GREEN << "-X, --request <method>    " << PINK << "HTTP method (GET, POST, etc.)      " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-H, --header <header>     " << PINK << "Add custom header (repeatable)     " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-d, --data <data>         " << PINK << "HTTP POST data                     " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-o, --output <file>       " << PINK << "Write output to file               " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-i, --include             " << PINK << "Include headers in output          " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-v, --verbose             " << PINK << "Verbose output with timing         " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-L, --location            " << PINK << "Follow redirects (default: off)    " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-m, --max-time <sec>      " << PINK << "Max request time (default: 30)     " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-A, --user-agent <ua>     " << PINK << "Custom User-Agent string           " << GREY << "│\n";
    std::cout << "├───────────────────────────────────────────────────────────────┤\n";
    std::cout << "│  " << RESET << "ADVANCED OPTIONS" << GREY << "                                             │\n";
    std::cout << "│  " << GREEN << "-r, --retry <count>       " << PINK << "Retry failed requests N times      " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-R, --rate-limit <rps>    " << PINK << "Rate limit (requests per second)   " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-p, --progress            " << PINK << "Show progress bar for downloads    " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-2, --http2               " << PINK << "Prefer HTTP/2 (if available)       " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-C, --no-compress         " << PINK << "Disable compression                " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-D, --dns-cache           " << PINK << "Enable DNS caching                 " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-S, --stats               " << PINK << "Show detailed statistics           " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-B, --batch <file>        " << PINK << "Batch mode: read URLs from file    " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-P, --parallel <num>      " << PINK << "Parallel requests (default: 10)    " << GREY << "│\n";
    std::cout << "│  " << GREEN << "-J, --json                " << PINK << "Output response as JSON            " << GREY << "│\n";
    std::cout << "├───────────────────────────────────────────────────────────────┤\n";
    std::cout << "│  " << RESET << "PERFORMANCE" << GREY << "                                                  │\n";
    std::cout << "│  " << GREEN << "--warmup <host>           " << PINK << "Pre-warm DNS cache for host        " << GREY << "│\n";
    std::cout << "│  " << GREEN << "--max-conn <num>          " << PINK << "Max concurrent connections         " << GREY << "│\n";
    std::cout << "├───────────────────────────────────────────────────────────────┤\n";
    std::cout << "│  " << RESET << "EXAMPLES" << GREY << "                                                     │\n";
    std::cout << "│  " << PINK << "└─ crawl https://example.com                                 " << GREY << "│\n";
    std::cout << "│  " << PINK << "└─ crawl -v -L https://google.com                            " << GREY << "│\n";
    std::cout << "│  " << PINK << "└─ crawl -X POST -d \"data\" https://api.example.com           " << GREY << "│\n";
    std::cout << "│  " << PINK << "└─ crawl -B urls.txt -P 20 -S                                " << GREY << "│\n";
    std::cout << "│  " << PINK << "└─ crawl -p -o file.zip https://example.com/large.zip        " << GREY << "│\n";
    std::cout << "╰───────────────────────────────────────────────────────────────╯\n";
    std::cout << RESET << "\n";
}


void output_json(const Response& resp, const std::string& url) {
    std::cout << "{\n";
    std::cout << "  \"url\": \"" << url << "\",\n";
    std::cout << "  \"status\": " << resp.status_code << ",\n";
    std::cout << "  \"status_message\": \"" << resp.status_message << "\",\n";
    std::cout << "  \"elapsed_ms\": " << resp.elapsed_time.count() << ",\n";
    std::cout << "  \"bytes_received\": " << resp.bytes_received << ",\n";
    std::cout << "  \"compressed\": " << (resp.was_compressed ? "true" : "false") << ",\n";
    std::cout << "  \"http2\": " << (resp.used_http2 ? "true" : "false") << ",\n";
    std::cout << "  \"headers\": {\n";
    bool first = true;
    for (const auto& [key, value] : resp.headers) {
        if (!first) std::cout << ",\n";
        std::cout << "    \"" << key << "\": \"" << value << "\"";
        first = false;
    }
    std::cout << "\n  },\n";
    std::cout << "  \"body_length\": " << resp.body.size() << "\n";
    std::cout << "}\n";
}

int main(int argc, char* argv[]) {
    std::string method = "GET";
    std::string output_file;
    std::string data;
    std::string user_agent;
    std::string batch_file;
    bool include_headers = false;
    bool verbose = false;
    bool follow_redirects = false;
    bool show_progress = false;
    bool use_http2 = false;
    bool no_compress = false;
    bool use_dns_cache = false;
    bool show_stats = false;
    bool json_output = false;
    int max_time = 30;
    int retry_count = 0;
    double rate_limit = 0;
    int parallel = 10;
    int max_conn = 200;
    std::vector<std::string> warmup_hosts;
    std::map<std::string, std::string> headers;
    std::thread progress_updater; // Correct placement

    static struct option long_options[] = {
        {"request", required_argument, 0, 'X'},
        {"header", required_argument, 0, 'H'},
        {"data", required_argument, 0, 'd'},
        {"output", required_argument, 0, 'o'},
        {"include", no_argument, 0, 'i'},
        {"verbose", no_argument, 0, 'v'},
        {"location", no_argument, 0, 'L'},
        {"max-time", required_argument, 0, 'm'},
        {"user-agent", required_argument, 0, 'A'},
        {"retry", required_argument, 0, 'r'},
        {"rate-limit", required_argument, 0, 'R'},
        {"progress", no_argument, 0, 'p'},
        {"http2", no_argument, 0, '2'},
        {"no-compress", no_argument, 0, 'C'},
        {"dns-cache", no_argument, 0, 'D'},
        {"stats", no_argument, 0, 'S'},
        {"batch", required_argument, 0, 'B'},
        {"parallel", required_argument, 0, 'P'},
        {"json", no_argument, 0, 'J'},
        {"warmup", required_argument, 0, 1000},
        {"max-conn", required_argument, 0, 1001},
        {"help", no_argument, 0, 'h'},
        {0, 0, 0, 0}
    };
    
    int opt;
    while ((opt = getopt_long(argc, argv, "X:H:d:o:ivLm:A:r:R:p2CDSB:P:Jh", long_options, nullptr)) != -1) {
        switch (opt) {
            case 'X': method = optarg; break;
            case 'H': {
                std::string header_str(optarg);
                size_t colon = header_str.find(':');
                if (colon != std::string::npos) {
                    std::string key = header_str.substr(0, colon);
                    std::string value = header_str.substr(colon + 1);
                    value.erase(0, value.find_first_not_of(" \t"));
                    headers[key] = value;
                }
                break;
            }
            case 'd':
                data = optarg;
                if (method == "GET") method = "POST";
                break;
            case 'o': output_file = optarg; break;
            case 'i': include_headers = true; break;
            case 'v': verbose = true; break;
            case 'L': follow_redirects = true; break;
            case 'm': max_time = std::atoi(optarg); break;
            case 'A': user_agent = optarg; break;
            case 'r': retry_count = std::atoi(optarg); break;
            case 'R': rate_limit = std::atof(optarg); break;
            case 'p': show_progress = true; break;
            case '2': use_http2 = true; break;
            case 'C': no_compress = true; break;
            case 'D': use_dns_cache = true; break;
            case 'S': show_stats = true; break;
            case 'B': batch_file = optarg; break;
            case 'P': parallel = std::atoi(optarg); break;
            case 'J': json_output = true; break;
            case 1000: warmup_hosts.push_back(optarg); break;
            case 1001: max_conn = std::atoi(optarg); break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            default:
                print_usage(argv[0]);
                return 1;
        }
    }
    
    HTTPClient client;
    
    // Configure client
    if (!user_agent.empty()) {
        client.set_user_agent(user_agent);
    }
    
    client.set_timeout(std::chrono::seconds(max_time));
    client.enable_http2(use_http2);
    client.enable_compression(!no_compress);
    client.set_max_connections(max_conn);
    
    if (rate_limit > 0) {
        client.set_rate_limit(rate_limit, static_cast<size_t>(rate_limit * 2));
    }
    
    if (use_dns_cache) {
        client.enable_dns_cache(true);
    }
    
    // Warmup DNS
    for (const auto& host : warmup_hosts) {
        if (verbose) {
            std::cerr << "* Warming up DNS for " << host << "...\n";
        }
        client.warmup_dns({host});
    }
    
    // Batch mode
    if (!batch_file.empty()) {
        std::ifstream file(batch_file);
        if (!file) {
            std::cerr << "Error: Cannot open batch file: " << batch_file << "\n";
            return 1;
        }
        
        std::vector<Request> requests;
        std::string url;
        
        while (std::getline(file, url)) {
            if (url.empty() || url[0] == '#') continue;
            
            auto parsed_url = URL::parse(url);
            if (!parsed_url) {
                std::cerr << "Warning: Invalid URL: " << url << "\n";
                continue;
            }
            
            Request req;
            req.method = method;
            req.url = *parsed_url;
            req.headers = headers;
            req.follow_redirects = follow_redirects;
            req.timeout = std::chrono::seconds(max_time);
            req.max_retries = retry_count;
            req.enable_compression = !no_compress;
            req.prefer_http2 = use_http2;
            
            requests.push_back(req);
        }
        
        if (verbose) {
            std::cerr << "* Processing " << requests.size() << " URLs with " 
                      << parallel << " parallel connections...\n";
        }
        
        auto start = std::chrono::steady_clock::now();
        auto responses = client.batch_request(requests, parallel);
        auto elapsed = std::chrono::steady_clock::now() - start;
        
        // Output results
        size_t success = 0;
        for (const auto& resp : responses) {
            if (resp.status_code >= 200 && resp.status_code < 400) {
                success++;
            }
        }
        
        if (verbose) {
            auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
            std::cerr << "* Completed in " << total_ms << " ms\n";
            std::cerr << "* Success: " << success << "/" << responses.size() << "\n";
        }
        
        if (show_stats) {
            client.get_stats()->print(true);
        }
        
        return (success == responses.size()) ? 0 : 1;
    }
    
    // Single URL mode
    if (optind >= argc) {
        std::cerr << "Error: URL required\n";
        print_usage(argv[0]);
        return 1;
    }
    
    std::string url = argv[optind];
    
    auto parsed_url = URL::parse(url);
    if (!parsed_url) {
        std::cerr << "Error: Invalid URL\n";
        return 1;
    }
    
    Request req;
    req.method = method;
    req.url = *parsed_url;
    req.headers = headers;
    req.follow_redirects = follow_redirects;
    req.timeout = std::chrono::seconds(max_time);
    req.max_retries = retry_count;
    req.enable_compression = !no_compress;
    req.prefer_http2 = use_http2;
    
    if (!data.empty()) {
        req.body.assign(data.begin(), data.end());
        if (req.headers.find("Content-Type") == req.headers.end()) {
            req.headers["Content-Type"] = "application/x-www-form-urlencoded";
        }
    }
    
    if (verbose) {
        std::cerr << "* Crawl - Ultra-Fast HTTP Client\n";
        std::cerr << "* Connecting to " << parsed_url->host << ":" << parsed_url->port << "...\n";
        if (use_dns_cache) std::cerr << "* DNS caching enabled\n";
        if (use_http2) std::cerr << "* HTTP/2 preferred\n";
        if (!no_compress) std::cerr << "* Compression enabled\n";
        if (rate_limit > 0) std::cerr << "* Rate limit: " << rate_limit << " req/s\n";
    }
    
    // Reset global progress counters before starting any request
    g_downloaded = 0;
    g_total = 0;
    
    // ── HEAD request to check content-length and Accept-Ranges support ──
    size_t content_length = 0;
    bool supports_ranges  = false;
    // head_resp object declaration needs to be outside the if block if content_length/supports_ranges are to be used outside
    Response head_resp_for_cl; 
    if (parallel > 1 && !output_file.empty() && show_progress) {
        Request head_req  = req;
        head_req.method   = "HEAD";
        head_req.timeout  = std::chrono::seconds(5); // Shorter timeout for HEAD request
        head_resp_for_cl = client.request(head_req); // Store response
        auto it_cl = head_resp_for_cl.headers.find("Content-Length");
        if (it_cl == head_resp_for_cl.headers.end()) it_cl = head_resp_for_cl.headers.find("content-length");
        if (it_cl != head_resp_for_cl.headers.end()) {
            content_length = 0;
            for (char c : it_cl->second)
                if (c>='0' && c<='9') content_length = content_length*10+(c-'0');
        }
        auto it_ar = head_resp_for_cl.headers.find("Accept-Ranges");
        if (it_ar == head_resp_for_cl.headers.end()) it_ar = head_resp_for_cl.headers.find("accept-ranges");
        if (it_ar != head_resp_for_cl.headers.end() && it_ar->second.find("bytes") != std::string::npos)
            supports_ranges = true;
    }

    // Reset g_downloaded after HEAD request processing
    g_downloaded = 0;

    // Update g_total based on content_length if found
    if (content_length > 0) {
        g_total = content_length;
    } else {
        g_total = 0; // Explicitly set g_total to 0 if content_length not found
    }

    // Start progress thread only after g_total is initialized
    // std::thread progress_updater; // Declared earlier.
    if (show_progress && !output_file.empty()) {
        g_progress_thread_running = true;
        progress_updater = std::thread(progress_thread_loop);
    }

    auto start = std::chrono::steady_clock::now();
    Response resp;
    std::vector<uint8_t> parallel_body;
    bool parallel_download_performed = false; // Flag to track if parallel download happened

    if (parallel > 1 && content_length > 0 && supports_ranges && !output_file.empty()) {
        // ── Parallel range download ────────────────────────────────────────
        // if (verbose)
        //     std::cerr << "* Parallel download: " << parallel << " pipes, "
        //               << format_size(content_length) << " total\n";
        parallel_body = parallel_download(url, content_length, parallel,
                                          show_progress, client, method, headers,
                                          max_time, no_compress);
        resp.status_code = parallel_body.empty() ? 0 : 206;
        resp.body        = parallel_body;
        resp.bytes_received = resp.body.size();
        parallel_download_performed = true; // Set flag
    } else {
        // ── Single pipe download (progress callback already set) ───────────
        resp = client.request(req);

        // For single downloads, set g_total if content_length is available
        if (!parallel_download_performed && show_progress && !output_file.empty()) {
            auto it_cl = resp.headers.find("Content-Length");
            if (it_cl == resp.headers.end()) it_cl = resp.headers.find("content-length");
            if (it_cl != resp.headers.end()) {
                size_t cl_val = 0;
                for (char c : it_cl->second)
                    if (c>='0' && c<='9') cl_val = cl_val*10+(c-'0');
                g_total = cl_val;
            } else {
                g_total = 0; // Explicitly set to 0 for blind bar
            }
        }
    }

    auto elapsed = std::chrono::steady_clock::now() - start;

    // Manually record stats for parallel download if it occurred
    if (parallel_download_performed && client.get_stats()) {
        client.get_stats()->record_request(
            std::chrono::duration_cast<std::chrono::milliseconds>(elapsed),
            resp.bytes_received
        );
        // Record a connection as well, assuming one logical "request" to the main client
        client.get_stats()->record_connection(false); 
    }

    // Stop and join the progress update thread if it was started
    if (g_progress_thread_running) {
        g_progress_thread_running = false; // Signal the thread to stop
        if (progress_updater.joinable()) {
            progress_updater.join(); // Wait for the thread to finish
        }
    }
    
    if (verbose) {
        auto total_ms = std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
        std::cerr << "* Request completed in " << total_ms << " ms\n";
        std::cerr << "* Status: " << resp.status_code << " " << resp.status_message << "\n";
        std::cerr << "* Received: " << format_size(resp.bytes_received) << "\n";
        if (resp.was_compressed) std::cerr << "* Decompressed from " << format_size(resp.bytes_received) << "\n";
        if (resp.used_http2) std::cerr << "* Used HTTP/2\n";
        if (resp.redirect_count > 0) std::cerr << "* Redirects: " << resp.redirect_count << "\n";
    }
    
    if (resp.status_code == 0) {
        std::cerr << "Error: Connection failed\n";
        return 1;
    }
    
    if (json_output) {
        output_json(resp, url);
        return 0;
    }
    
    // Output response
    std::ostream* out = &std::cout;
    std::ofstream file_out;
    
    if (!output_file.empty()) {
        file_out.open(output_file, std::ios::binary);
        if (!file_out) {
            std::cerr << "Error: Cannot open output file\n";
            return 1;
        }
        out = &file_out;
    }
    
    if (include_headers) {
        *out << "HTTP/1.1 " << resp.status_code << " " << resp.status_message << "\n";
        for (const auto& [key, value] : resp.headers) {
            *out << key << ": " << value << "\n";
        }
        *out << "\n";
    }
    
    out->write(reinterpret_cast<const char*>(resp.body.data()), resp.body.size());
    
    if (verbose && !output_file.empty()) {
        std::cerr << "* Saved to " << output_file << " (" << format_size(resp.body.size()) << ")\n";
    }
    
    if (show_stats) {
        client.get_stats()->print(true);
    }
    
    return (resp.status_code >= 200 && resp.status_code < 400) ? 0 : 1;
}
