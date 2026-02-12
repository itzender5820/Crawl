#include "tls_connection.hpp"
#include <mbedtls/net_sockets.h>
#include <mbedtls/ssl.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/error.h>
#include <mbedtls/x509_crt.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdexcept>

// MSG_NOSIGNAL doesn't exist on Android/Termux
#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

namespace crawl {

class TLSConnection::Impl {
public:
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_x509_crt cacert;
    
    Impl() {
        mbedtls_ssl_init(&ssl);
        mbedtls_ssl_config_init(&conf);
        mbedtls_entropy_init(&entropy);
        mbedtls_ctr_drbg_init(&ctr_drbg);
        mbedtls_x509_crt_init(&cacert);
    }
    
    ~Impl() {
        mbedtls_ssl_free(&ssl);
        mbedtls_ssl_config_free(&conf);
        mbedtls_entropy_free(&entropy);
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_x509_crt_free(&cacert);
    }
};

// Custom BIO functions for using our socket
static int ssl_send(void* ctx, const unsigned char* buf, size_t len) {
    int fd = *static_cast<int*>(ctx);
    ssize_t ret = ::send(fd, buf, len, MSG_NOSIGNAL);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_WRITE;
        }
        return MBEDTLS_ERR_NET_SEND_FAILED;
    }
    return ret;
}

static int ssl_recv(void* ctx, unsigned char* buf, size_t len) {
    int fd = *static_cast<int*>(ctx);
    ssize_t ret = ::recv(fd, buf, len, 0);
    if (ret < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            return MBEDTLS_ERR_SSL_WANT_READ;
        }
        return MBEDTLS_ERR_NET_RECV_FAILED;
    }
    if (ret == 0) {
        return MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY;
    }
    return ret;
}

TLSConnection::TLSConnection(int socket_fd, const std::string& hostname)
    : impl_(std::make_unique<Impl>()), 
      socket_fd_(socket_fd),
      hostname_(hostname),
      connected_(false) {
}

TLSConnection::~TLSConnection() {
    close();
}

bool TLSConnection::handshake() {
    const char* pers = "httpclient";
    
    // Seed the RNG
    int ret = mbedtls_ctr_drbg_seed(&impl_->ctr_drbg, mbedtls_entropy_func,
                                     &impl_->entropy,
                                     reinterpret_cast<const unsigned char*>(pers),
                                     strlen(pers));
    if (ret != 0) {
        return false;
    }
    
    // Load CA certificates (system default)
    // Try multiple common paths for different systems
    const char* ca_paths[] = {
        "/etc/ssl/certs",                    // Debian/Ubuntu
        "/etc/pki/tls/certs",                // RHEL/CentOS
        "/usr/local/share/certs",            // FreeBSD
        "/etc/ssl",                          // OpenBSD
        "/data/data/com.termux/files/usr/etc/tls/certs", // Termux
        "/system/etc/security/cacerts",      // Android
        nullptr
    };
    
    bool ca_loaded = false;
    for (int i = 0; ca_paths[i] != nullptr; i++) {
        ret = mbedtls_x509_crt_parse_path(&impl_->cacert, ca_paths[i]);
        if (ret >= 0) {
            ca_loaded = true;
            break;
        }
    }
    
    // If no path worked, try parsing the default cert file
    if (!ca_loaded) {
        const char* ca_files[] = {
            "/etc/ssl/certs/ca-certificates.crt",
            "/etc/pki/tls/certs/ca-bundle.crt",
            "/data/data/com.termux/files/usr/etc/tls/cert.pem",
            nullptr
        };
        
        for (int i = 0; ca_files[i] != nullptr; i++) {
            ret = mbedtls_x509_crt_parse_file(&impl_->cacert, ca_files[i]);
            if (ret == 0) {
                ca_loaded = true;
                break;
            }
        }
    }
    
    // Setup SSL/TLS structure
    ret = mbedtls_ssl_config_defaults(&impl_->conf,
                                       MBEDTLS_SSL_IS_CLIENT,
                                       MBEDTLS_SSL_TRANSPORT_STREAM,
                                       MBEDTLS_SSL_PRESET_DEFAULT);
    if (ret != 0) {
        return false;
    }
    
    // Set minimum TLS version to 1.2
    mbedtls_ssl_conf_min_version(&impl_->conf, MBEDTLS_SSL_MAJOR_VERSION_3,
                                  MBEDTLS_SSL_MINOR_VERSION_3);
    
    mbedtls_ssl_conf_authmode(&impl_->conf, MBEDTLS_SSL_VERIFY_OPTIONAL);
    mbedtls_ssl_conf_ca_chain(&impl_->conf, &impl_->cacert, nullptr);
    mbedtls_ssl_conf_rng(&impl_->conf, mbedtls_ctr_drbg_random, &impl_->ctr_drbg);
    
    ret = mbedtls_ssl_setup(&impl_->ssl, &impl_->conf);
    if (ret != 0) {
        return false;
    }
    
    // Set hostname for SNI
    mbedtls_ssl_set_hostname(&impl_->ssl, hostname_.c_str());
    
    // Set custom BIO functions
    mbedtls_ssl_set_bio(&impl_->ssl, &socket_fd_, ssl_send, ssl_recv, nullptr);
    
    // Perform handshake
    while ((ret = mbedtls_ssl_handshake(&impl_->ssl)) != 0) {
        if (ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            return false;
        }
    }
    
    connected_ = true;
    return true;
}

ssize_t TLSConnection::send(const void* data, size_t len) {
    if (!connected_) {
        return -1;
    }
    
    const unsigned char* buf = static_cast<const unsigned char*>(data);
    size_t written = 0;
    
    while (written < len) {
        int ret = mbedtls_ssl_write(&impl_->ssl, buf + written, len - written);
        if (ret < 0) {
            if (ret == MBEDTLS_ERR_SSL_WANT_WRITE || ret == MBEDTLS_ERR_SSL_WANT_READ) {
                continue;
            }
            return -1;
        }
        written += ret;
    }
    
    return written;
}

ssize_t TLSConnection::recv(void* data, size_t len) {
    if (!connected_) {
        return -1;
    }
    
    unsigned char* buf = static_cast<unsigned char*>(data);
    int ret = mbedtls_ssl_read(&impl_->ssl, buf, len);
    
    if (ret < 0) {
        if (ret == MBEDTLS_ERR_SSL_WANT_READ || ret == MBEDTLS_ERR_SSL_WANT_WRITE) {
            return 0;
        }
        return -1;
    }
    
    return ret;
}

void TLSConnection::close() {
    if (connected_) {
        mbedtls_ssl_close_notify(&impl_->ssl);
        connected_ = false;
    }
}

} // namespace crawl
