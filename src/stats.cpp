#include "stats.hpp"
#include <iostream>
#include <iomanip>
#include <sstream>

namespace crawl {

// ──────────────────────────────────────────────────────────────────────────────
// ANSI color codes
// ──────────────────────────────────────────────────────────────────────────────
static const char* C_GREY   = "\033[90m";     // box outline
static const char* C_CYAN   = "\033[36m";     // block letters (╟─ └─ ╙─)
static const char* C_GREEN  = "\033[92m";     // label names
static const char* C_PINK   = "\033[38;5;205m"; // values
static const char* C_RED    = "\033[31m";     // latency internal outline
static const char* C_RESET  = "\033[0m";

// ──────────────────────────────────────────────────────────────────────────────
// Compute DISPLAY width (columns) of a UTF-8 string.
// Box-drawing chars (╟, ─, ║, etc.) are 3 UTF-8 bytes but ONE terminal column.
// ──────────────────────────────────────────────────────────────────────────────
static int disp_w(const std::string& s) {
    int w = 0;
    size_t i = 0;
    while (i < s.size()) {
        unsigned char c = (unsigned char)s[i];
        if      (c < 0x80)             { w++;    i += 1; }  // ASCII
        else if ((c & 0xE0) == 0xC0)   { w++;    i += 2; }  // 2-byte
        else if ((c & 0xF0) == 0xE0)   { w++;    i += 3; }  // 3-byte (box chars)
        else if ((c & 0xF8) == 0xF0)   { w += 2; i += 4; }  // 4-byte (wide)
        else                           {          i += 1; }
    }
    return w;
}

// ──────────────────────────────────────────────────────────────────────────────
// Print a full-width box line.
//   uncolored_content  – the line content WITHOUT ANSI codes (used for width calc)
//   colored_content    – the same content WITH ANSI codes (used for output)
//   box_width          – total display columns (64)
// ──────────────────────────────────────────────────────────────────────────────
static void box_line(const std::string& uncolored, const std::string& colored,
                     int box_width = 64) {
    int content_cols = disp_w(uncolored); // visual cols used
    int spaces       = box_width - content_cols - 1; // remaining before closing ║
    if (spaces < 0) spaces = 0;

    std::cout << colored;
    for (int i = 0; i < spaces; ++i) std::cout << ' ';
    std::cout << C_GREY << "║" << C_RESET << "\n";
}

// Helper: format a stat value line
// connector = "╟─" or "╙─" (cyan)
// label = "Requests:", etc (green)
// label_pad = spaces after label: to align values
// value = the number/string (pink)
static void stat_line(const std::string& connector,
                      const std::string& label,
                      const std::string& label_pad,
                      const std::string& value,
                      int box_width = 64) {
    // Build uncolored version for width calculation
    std::string unc = "║  " + connector + " " + label + label_pad + value;
    // Build colored version
    std::string col = std::string(C_GREY) + "║" + C_RESET +
                      "  " +
                      C_CYAN + connector + C_RESET +
                      " " +
                      C_GREEN + label + C_RESET +
                      label_pad +
                      C_PINK + value + C_RESET;
    box_line(unc, col, box_width);
}

// Helper: format a timing line (uses └─)
static void timing_line(const std::string& label,
                        const std::string& label_pad,
                        const std::string& value,
                        int box_width = 64) {
    std::string unc = "║  └─ " + label + label_pad + value;
    std::string col = std::string(C_GREY) + "║" + C_RESET +
                      "  " +
                      C_CYAN + "└─" + C_RESET +
                      " " +
                      C_GREEN + label + C_RESET +
                      label_pad +
                      C_PINK + value + C_RESET;
    box_line(unc, col, box_width);
}

// ──────────────────────────────────────────────────────────────────────────────
Statistics::Statistics() {}

void Statistics::record_request(std::chrono::milliseconds latency, size_t bytes_received) {
    total_requests_++;
    total_bytes_received_ += bytes_received;
    uint64_t lat_ms = latency.count();
    total_latency_ms_ += lat_ms;
    uint64_t cur_min = min_latency_ms_.load();
    while (lat_ms < cur_min && !min_latency_ms_.compare_exchange_weak(cur_min, lat_ms));
    uint64_t cur_max = max_latency_ms_.load();
    while (lat_ms > cur_max && !max_latency_ms_.compare_exchange_weak(cur_max, lat_ms));
}

void Statistics::record_connection(bool reused) {
    if (reused) connections_reused_++;
    else        connections_created_++;
}

void Statistics::record_error(const std::string& error_type) {
    total_errors_++;
    std::lock_guard<std::mutex> lock(error_mutex_);
    error_counts_[error_type]++;
}

void Statistics::record_dns_lookup(std::chrono::milliseconds duration, bool cached) {
    dns_lookups_++;
    if (cached) dns_cache_hits_++;
    total_dns_ms_ += duration.count();
}

void Statistics::record_tcp_handshake(std::chrono::milliseconds duration) {
    tcp_handshake_count_++;
    total_tcp_ms_ += duration.count();
}

void Statistics::record_first_byte(std::chrono::milliseconds duration) {
    first_byte_count_++;
    total_first_byte_ms_ += duration.count();
}

void Statistics::set_current_ip(const std::string& ip) {
    std::lock_guard<std::mutex> lock(info_mutex_);
    current_ip_ = ip;
}
void Statistics::set_current_host(const std::string& host) {
    std::lock_guard<std::mutex> lock(info_mutex_);
    current_host_ = host;
}
void Statistics::set_is_secure(bool secure) {
    std::lock_guard<std::mutex> lock(info_mutex_);
    is_secure_ = secure;
}

Statistics::Stats Statistics::get_stats() const {
    Stats s;
    s.total_requests      = total_requests_;
    s.total_errors        = total_errors_;
    s.total_bytes_received= total_bytes_received_;
    s.total_bytes_sent    = total_bytes_sent_;
    s.connections_created = connections_created_;
    s.connections_reused  = connections_reused_;
    s.dns_lookups         = dns_lookups_;
    s.dns_cache_hits      = dns_cache_hits_;

    uint64_t req = total_requests_.load();
    s.avg_latency_ms = req > 0 ? static_cast<double>(total_latency_ms_) / req : 0;
    uint64_t mn = min_latency_ms_.load();
    s.min_latency_ms = (mn == 999999) ? 0 : mn;
    s.max_latency_ms = max_latency_ms_.load();

    uint64_t dns_c = dns_lookups_.load();
    s.avg_dns_ms = dns_c > 0 ? static_cast<double>(total_dns_ms_) / dns_c : 0;

    uint64_t tcp_c = tcp_handshake_count_.load();
    s.avg_tcp_handshake_ms = tcp_c > 0 ? static_cast<double>(total_tcp_ms_) / tcp_c : 0;

    uint64_t fb_c = first_byte_count_.load();
    s.avg_first_byte_ms = fb_c > 0 ? static_cast<double>(total_first_byte_ms_) / fb_c : 0;
    s.avg_last_byte_ms  = s.avg_latency_ms;

    {
        std::lock_guard<std::mutex> lock(info_mutex_);
        s.current_ip   = current_ip_.empty()   ? "N/A" : current_ip_;
        s.current_host = current_host_.empty() ? "N/A" : current_host_;
        s.is_secure    = is_secure_;
    }
    {
        std::lock_guard<std::mutex> lock(error_mutex_);
        s.error_counts = error_counts_;
    }
    return s;
}

void Statistics::reset() {
    total_requests_ = total_errors_ = total_bytes_received_ = total_bytes_sent_ = 0;
    connections_created_ = connections_reused_ = 0;
    dns_lookups_ = dns_cache_hits_ = 0;
    total_latency_ms_ = 0;
    min_latency_ms_  = 999999;
    max_latency_ms_  = 0;
    total_dns_ms_ = total_tcp_ms_ = total_first_byte_ms_ = 0;
    tcp_handshake_count_ = first_byte_count_ = 0;
    std::lock_guard<std::mutex> l1(info_mutex_);
    current_ip_.clear(); current_host_.clear(); is_secure_ = false;
    std::lock_guard<std::mutex> l2(error_mutex_);
    error_counts_.clear();
}

void Statistics::print_line(const std::string& label, const std::string& value,
                             int total_width) const {
    stat_line("╟─", label, ": ", value, total_width);
}

// ──────────────────────────────────────────────────────────────────────────────
// Main print function
// ──────────────────────────────────────────────────────────────────────────────
void Statistics::print(bool /*detailed*/) const {
    auto s = get_stats();

    // Lambda to format a double as "%.2f ms" or just "%.2f"
    auto fmt2 = [](double v, const std::string& suffix = "") -> std::string {
        std::ostringstream o;
        o << std::fixed << std::setprecision(2) << v << suffix;
        return o.str();
    };
    auto fmt1 = [](double v, const std::string& suffix = "") -> std::string {
        std::ostringstream o;
        o << std::fixed << std::setprecision(1) << v << suffix;
        return o.str();
    };
    auto fmtu = [](uint64_t v) -> std::string {
        return std::to_string(v);
    };

    const int W = 64;

    // ── TITLE ──────────────────────────────────────────────────────────────
    std::cout << "\n";
    std::cout << C_GREY
              << "╔══════════════════════════════════════════════════════════════╗\n"
              << "║                      CRAWL STATISTICS                        ║\n"
              << "╚══════════════════════════════════════════════════════════════╝\n"
              << C_RESET;

    // ── GENERAL INFO ───────────────────────────────────────────────────────
    // Header line – compute spacing with disp_w
    {
        std::string unc = "║  GENERAL INFO";
        std::string col = std::string(C_GREY) + "║" + C_RESET + "  " +
                          C_GREEN + "GENERAL INFO" + C_RESET;
        std::cout << C_GREY << "╔══════════════════════════════════════════════════════════════╗\n" << C_RESET;
        box_line(unc, col, W);
    }

    // Prefix display cols for GENERAL / CONNECTIONS value lines:
    // "║  ╟─ Requests:      " → display cols = 1+2+1+1+1+9+6 = 21
    // We want value to start at col 22, and closing ║ at col 64.
    // spaces_after_value = 64 - 21 - value_display_cols - 1
    // We bake the label_pad to reach the same column 22 for all labels:
    //   "Requests:"  (9)  → pad = "      " (6) → total prefix = 21
    //   "Errors:"    (7)  → pad = "        " (8)
    //   "Data Received:" (14) → pad = " " (1)
    stat_line("╟─", "Requests:",      "      ", fmtu(s.total_requests),        W);
    stat_line("╟─", "Errors:",        "        ", fmtu(s.total_errors),         W);
    stat_line("╙─", "Data Received:", " ",
              fmt2(s.total_bytes_received / 1024.0) + " KB", W);

    std::cout << C_GREY << "╚══════════════════════════════════════════════════════════════╝\n" << C_RESET;

    // ── LATENCY (ms) ───────────────────────────────────────────────────────
    std::cout << C_GREY << "╔══════════════════════════════════════════════════════════════╗\n" << C_RESET;
    {
        std::string unc = "║  LATENCY (ms)";
        std::string col = std::string(C_GREY) + "║" + C_RESET + "  " +
                          C_GREEN + "LATENCY (ms)" + C_RESET;
        box_line(unc, col, W);
    }

    // Inner table with RED outline
    std::cout << C_GREY << "║  " << C_RESET
              << C_RED
              << "╭──────────────────┬──────────────────┬──────────────────╮"
              << C_RESET
              << C_GREY << "  ║\n" << C_RESET;

    // Header row
    std::cout << C_GREY << "║  " << C_RESET
              << C_RED << "│" << C_RESET
              << C_GREEN << "      Average     " << C_RESET
              << C_RED << "│" << C_RESET
              << C_GREEN << "        Min       " << C_RESET
              << C_RED << "│" << C_RESET
              << C_GREEN << "        Max       " << C_RESET
              << C_RED << "│" << C_RESET
              << C_GREY << "  ║\n" << C_RESET;

    // Value row - center each value in its 18-char cell
    auto center18 = [&](const std::string& val) -> std::string {
        int vlen = val.length();
        int left  = (18 - vlen) / 2;
        int right = 18 - vlen - left;
        return std::string(left, ' ') + val + std::string(right, ' ');
    };
    std::string avg_s = center18(fmt2(s.avg_latency_ms));
    std::string min_s = center18(fmt2(s.min_latency_ms));
    std::string max_s = center18(fmt2(s.max_latency_ms));

    std::cout << C_GREY << "║  " << C_RESET
              << C_RED << "│" << C_RESET
              << C_PINK << avg_s << C_RESET
              << C_RED << "│" << C_RESET
              << C_PINK << min_s << C_RESET
              << C_RED << "│" << C_RESET
              << C_PINK << max_s << C_RESET
              << C_RED << "│" << C_RESET
              << C_GREY << "  ║\n" << C_RESET;

    std::cout << C_GREY << "║  " << C_RESET
              << C_RED
              << "╰──────────────────┴──────────────────┴──────────────────╯"
              << C_RESET
              << C_GREY << "  ║\n" << C_RESET;

    std::cout << C_GREY << "╚══════════════════════════════════════════════════════════════╝\n" << C_RESET;

    // ── CONNECTIONS ────────────────────────────────────────────────────────
    std::cout << C_GREY << "╔══════════════════════════════════════════════════════════════╗\n" << C_RESET;
    {
        std::string unc = "║  CONNECTIONS";
        std::string col = std::string(C_GREY) + "║" + C_RESET + "  " +
                          C_GREEN + "CONNECTIONS" + C_RESET;
        box_line(unc, col, W);
    }

    // Prefix cols for connections same as general (21):
    // "Created:"  (8) → pad = "       " (7) → 1+2+1+1+1+8+7 = 21
    // "Reused:"   (7) → pad = "        " (8) → 1+2+1+1+1+7+8 = 21
    // "Reuse Rate:" (11) → pad = "    " (4) → 1+2+1+1+1+11+4 = 21
    stat_line("╟─", "Created:",    "       ", fmtu(s.connections_created), W);
    stat_line("╟─", "Reused:",     "        ", fmtu(s.connections_reused),  W);
    {
        std::string rate;
        if (s.connections_created + s.connections_reused > 0) {
            double r = 100.0 * s.connections_reused /
                       (s.connections_created + s.connections_reused);
            rate = fmt1(r) + "%";
        } else {
            rate = "0.0%";
        }
        stat_line("╙─", "Reuse Rate:", "    ", rate, W);
    }

    std::cout << C_GREY << "╚══════════════════════════════════════════════════════════════╝\n" << C_RESET;

    // ── DETAILED TIMING ────────────────────────────────────────────────────
    std::cout << C_GREY << "╔══════════════════════════════════════════════════════════════╗\n" << C_RESET;
    {
        std::string unc = "║  DETAILED TIMING";
        std::string col = std::string(C_GREY) + "║" + C_RESET + "  " +
                          C_GREEN + "DETAILED TIMING" + C_RESET;
        box_line(unc, col, W);
    }

    // Timing line prefix cols = 23:
    // "║  └─ DNS Lookup:      " → 1+2+1+1+1+11+6 = 23
    // "║  └─ TCP Handshake:   " → 1+2+1+1+1+14+3 = 23
    // "║  └─ First Byte:      " → 1+2+1+1+1+11+6 = 23
    // "║  └─ Last Byte:       " → 1+2+1+1+1+10+7 = 23
    timing_line("DNS Lookup:",    "      ", fmt2(s.avg_dns_ms)            + " ms", W);
    timing_line("TCP Handshake:", "   ",    fmt2(s.avg_tcp_handshake_ms)  + " ms", W);
    timing_line("First Byte:",    "      ", fmt2(s.avg_first_byte_ms)     + " ms", W);
    timing_line("Last Byte:",     "       ", fmt2(s.avg_last_byte_ms)     + " ms", W);

    // Bottom border: ╚ + 5×═ + ╤ + 56×═ + ╝ = 64 cols exactly
    std::cout << C_GREY
              << "╚═════╤════════════════════════════════════════════════════════╝\n"
              << C_RESET;

    // Spider
    std::cout << C_GREY   << "      │\n" << C_RESET;
    std::cout << "    / " << C_CYAN << "┴" << C_RESET << " \\\n";
    std::cout << C_RED << "  \\_\\(_)/_/\n";
    std::cout << "  _//   \\\\_\n" << C_RESET;
    std::cout << "   /     \\\n\n";
}

} // namespace crawl
