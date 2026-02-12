#pragma once

#include <vector>
#include <cstdint>
#include <string>
#include <optional>

namespace crawl {

enum class CompressionType {
    None,
    Gzip,
    Deflate,
    Brotli
};

class Compression {
public:
    // Decompress data based on Content-Encoding header
    static std::optional<std::vector<uint8_t>> decompress(
        const std::vector<uint8_t>& compressed_data,
        CompressionType type
    );
    
    // Compress data
    static std::optional<std::vector<uint8_t>> compress(
        const std::vector<uint8_t>& data,
        CompressionType type,
        int level = 6
    );
    
    // Detect compression type from header
    static CompressionType detect_from_header(const std::string& content_encoding);
    
    // Get Accept-Encoding header value
    static std::string get_accept_encoding_header();

private:
#ifdef HAVE_ZLIB
    static std::optional<std::vector<uint8_t>> decompress_gzip(const std::vector<uint8_t>& data);
    static std::optional<std::vector<uint8_t>> decompress_deflate(const std::vector<uint8_t>& data);
    static std::optional<std::vector<uint8_t>> compress_gzip(const std::vector<uint8_t>& data, int level);
#endif
    
#ifdef HAVE_BROTLI
    static std::optional<std::vector<uint8_t>> decompress_brotli(const std::vector<uint8_t>& data);
    static std::optional<std::vector<uint8_t>> compress_brotli(const std::vector<uint8_t>& data, int level);
#endif
};

} // namespace crawl
