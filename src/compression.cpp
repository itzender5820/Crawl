#include "compression.hpp"
#include <algorithm>
#include <cstring>

#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

#ifdef HAVE_BROTLI
#include <brotli/decode.h>
#include <brotli/encode.h>
#endif

namespace crawl {

CompressionType Compression::detect_from_header(const std::string& content_encoding) {
    std::string lower = content_encoding;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    
    if (lower.find("br") != std::string::npos) {
        return CompressionType::Brotli;
    } else if (lower.find("gzip") != std::string::npos) {
        return CompressionType::Gzip;
    } else if (lower.find("deflate") != std::string::npos) {
        return CompressionType::Deflate;
    }
    
    return CompressionType::None;
}

std::string Compression::get_accept_encoding_header() {
    std::string encodings;
    
#ifdef HAVE_BROTLI
    encodings += "br, ";
#endif
    
#ifdef HAVE_ZLIB
    encodings += "gzip, deflate";
#endif
    
    if (!encodings.empty() && encodings.back() == ' ') {
        encodings.pop_back();
        encodings.pop_back();
    }
    
    return encodings.empty() ? "identity" : encodings;
}

#ifdef HAVE_ZLIB

std::optional<std::vector<uint8_t>> Compression::decompress_gzip(const std::vector<uint8_t>& data) {
    if (data.empty()) return std::vector<uint8_t>();
    
    z_stream stream{};
    stream.next_in = const_cast<uint8_t*>(data.data());
    stream.avail_in = data.size();
    
    // 15 + 16 for gzip format
    if (inflateInit2(&stream, 15 + 16) != Z_OK) {
        return std::nullopt;
    }
    
    std::vector<uint8_t> output;
    output.reserve(data.size() * 3); // Estimate 3x expansion
    
    const size_t chunk_size = 32768;
    std::vector<uint8_t> temp(chunk_size);
    
    int ret;
    do {
        stream.next_out = temp.data();
        stream.avail_out = chunk_size;
        
        ret = inflate(&stream, Z_NO_FLUSH);
        
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&stream);
            return std::nullopt;
        }
        
        size_t have = chunk_size - stream.avail_out;
        output.insert(output.end(), temp.begin(), temp.begin() + have);
        
    } while (ret != Z_STREAM_END);
    
    inflateEnd(&stream);
    return output;
}

std::optional<std::vector<uint8_t>> Compression::decompress_deflate(const std::vector<uint8_t>& data) {
    if (data.empty()) return std::vector<uint8_t>();
    
    z_stream stream{};
    stream.next_in = const_cast<uint8_t*>(data.data());
    stream.avail_in = data.size();
    
    // Try raw deflate first
    if (inflateInit2(&stream, -15) != Z_OK) {
        return std::nullopt;
    }
    
    std::vector<uint8_t> output;
    output.reserve(data.size() * 3);
    
    const size_t chunk_size = 32768;
    std::vector<uint8_t> temp(chunk_size);
    
    int ret;
    do {
        stream.next_out = temp.data();
        stream.avail_out = chunk_size;
        
        ret = inflate(&stream, Z_NO_FLUSH);
        
        if (ret != Z_OK && ret != Z_STREAM_END) {
            inflateEnd(&stream);
            return std::nullopt;
        }
        
        size_t have = chunk_size - stream.avail_out;
        output.insert(output.end(), temp.begin(), temp.begin() + have);
        
    } while (ret != Z_STREAM_END);
    
    inflateEnd(&stream);
    return output;
}

std::optional<std::vector<uint8_t>> Compression::compress_gzip(const std::vector<uint8_t>& data, int level) {
    if (data.empty()) return std::vector<uint8_t>();
    
    z_stream stream{};
    stream.next_in = const_cast<uint8_t*>(data.data());
    stream.avail_in = data.size();
    
    if (deflateInit2(&stream, level, Z_DEFLATED, 15 + 16, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
        return std::nullopt;
    }
    
    std::vector<uint8_t> output;
    output.reserve(deflateBound(&stream, data.size()));
    
    const size_t chunk_size = 32768;
    std::vector<uint8_t> temp(chunk_size);
    
    int ret;
    do {
        stream.next_out = temp.data();
        stream.avail_out = chunk_size;
        
        ret = deflate(&stream, Z_FINISH);
        
        if (ret != Z_OK && ret != Z_STREAM_END) {
            deflateEnd(&stream);
            return std::nullopt;
        }
        
        size_t have = chunk_size - stream.avail_out;
        output.insert(output.end(), temp.begin(), temp.begin() + have);
        
    } while (ret != Z_STREAM_END);
    
    deflateEnd(&stream);
    return output;
}

#endif // HAVE_ZLIB

#ifdef HAVE_BROTLI

std::optional<std::vector<uint8_t>> Compression::decompress_brotli(const std::vector<uint8_t>& data) {
    if (data.empty()) return std::vector<uint8_t>();
    
    size_t output_size = data.size() * 3;
    std::vector<uint8_t> output(output_size);
    
    BrotliDecoderResult result = BrotliDecoderDecompress(
        data.size(), data.data(),
        &output_size, output.data()
    );
    
    if (result != BROTLI_DECODER_RESULT_SUCCESS) {
        return std::nullopt;
    }
    
    output.resize(output_size);
    return output;
}

std::optional<std::vector<uint8_t>> Compression::compress_brotli(const std::vector<uint8_t>& data, int level) {
    if (data.empty()) return std::vector<uint8_t>();
    
    size_t output_size = BrotliEncoderMaxCompressedSize(data.size());
    std::vector<uint8_t> output(output_size);
    
    BROTLI_BOOL result = BrotliEncoderCompress(
        level,
        BROTLI_DEFAULT_WINDOW,
        BROTLI_DEFAULT_MODE,
        data.size(), data.data(),
        &output_size, output.data()
    );
    
    if (result != BROTLI_TRUE) {
        return std::nullopt;
    }
    
    output.resize(output_size);
    return output;
}

#endif // HAVE_BROTLI

std::optional<std::vector<uint8_t>> Compression::decompress(
    const std::vector<uint8_t>& compressed_data,
    CompressionType type) {
    
    switch (type) {
#ifdef HAVE_ZLIB
        case CompressionType::Gzip:
            return decompress_gzip(compressed_data);
        case CompressionType::Deflate:
            return decompress_deflate(compressed_data);
#endif
            
#ifdef HAVE_BROTLI
        case CompressionType::Brotli:
            return decompress_brotli(compressed_data);
#endif
            
        case CompressionType::None:
        default:
            return compressed_data;
    }
}

std::optional<std::vector<uint8_t>> Compression::compress(
    const std::vector<uint8_t>& data,
    CompressionType type,
    int level) {
    
    switch (type) {
#ifdef HAVE_ZLIB
        case CompressionType::Gzip:
            return compress_gzip(data, level);
#endif
            
#ifdef HAVE_BROTLI
        case CompressionType::Brotli:
            return compress_brotli(data, level);
#endif
            
        case CompressionType::None:
        case CompressionType::Deflate:
        default:
            return data;
    }
}

} // namespace crawl
