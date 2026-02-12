cmake_minimum_required(VERSION 3.15)
project(Crawl VERSION 1.0 LANGUAGES CXX C)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Ultra-aggressive optimization flags
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -ffast-math -march=native -mtune=native -flto -funroll-loops -ftree-vectorize -fomit-frame-pointer -DNDEBUG")
set(CMAKE_CXX_FLAGS_DEBUG "-g -O0")

# Additional performance flags
add_compile_options(
    -fno-exceptions          # Disable exceptions for speed
    -fno-rtti               # Disable RTTI
    -finline-functions      # Aggressive inlining
    -fmerge-all-constants   # Merge constants
)

# Enable Link Time Optimization
if(CMAKE_BUILD_TYPE STREQUAL "Release")
    include(CheckIPOSupported)
    check_ipo_supported(RESULT ipo_supported OUTPUT ipo_error)
    if(ipo_supported)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "LTO enabled")
    endif()
endif()

# Profile Guided Optimization support
option(ENABLE_PGO_GEN "Generate profile data" OFF)
option(ENABLE_PGO_USE "Use profile data" OFF)

if(ENABLE_PGO_GEN)
    add_compile_options(-fprofile-generate)
    add_link_options(-fprofile-generate)
    message(STATUS "PGO: Generating profile data")
endif()

if(ENABLE_PGO_USE)
    add_compile_options(-fprofile-use -fprofile-correction)
    add_link_options(-fprofile-use)
    message(STATUS "PGO: Using profile data")
endif()

# Find packages
find_package(Threads REQUIRED)

# Optional: zlib for compression
find_package(ZLIB)
if(ZLIB_FOUND)
    add_compile_definitions(HAVE_ZLIB)
    message(STATUS "zlib found - compression support enabled")
endif()

# Optional: brotli for compression
find_path(BROTLI_INCLUDE_DIR brotli/decode.h)
find_library(BROTLI_DEC_LIBRARY brotlidec)
find_library(BROTLI_ENC_LIBRARY brotlienc)
find_library(BROTLI_COMMON_LIBRARY brotlicommon)

if(BROTLI_INCLUDE_DIR AND BROTLI_DEC_LIBRARY)
    add_compile_definitions(HAVE_BROTLI)
    set(BROTLI_LIBRARIES ${BROTLI_DEC_LIBRARY} ${BROTLI_ENC_LIBRARY} ${BROTLI_COMMON_LIBRARY})
    message(STATUS "brotli found - brotli compression support enabled")
endif()

# Optional: nghttp2 for HTTP/2
find_path(NGHTTP2_INCLUDE_DIR nghttp2/nghttp2.h)
find_library(NGHTTP2_LIBRARY nghttp2)

if(NGHTTP2_INCLUDE_DIR AND NGHTTP2_LIBRARY)
    add_compile_definitions(HAVE_NGHTTP2)
    message(STATUS "nghttp2 found - HTTP/2 support enabled")
endif()

# Check for mbedTLS
find_path(MBEDTLS_INCLUDE_DIR mbedtls/ssl.h)
find_library(MBEDTLS_LIBRARY mbedtls)
find_library(MBEDX509_LIBRARY mbedx509)
find_library(MBEDCRYPTO_LIBRARY mbedcrypto)

if(NOT MBEDTLS_INCLUDE_DIR OR NOT MBEDTLS_LIBRARY)
    message(STATUS "mbedTLS not found, downloading...")
    
    include(FetchContent)
    FetchContent_Declare(
        mbedtls
        GIT_REPOSITORY https://github.com/Mbed-TLS/mbedtls.git
        GIT_TAG v3.6.2
        GIT_SHALLOW TRUE
    )
    
    set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
    set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
    set(MBEDTLS_AS_SUBPROJECT ON CACHE BOOL "" FORCE)
    
    FetchContent_MakeAvailable(mbedtls)
    
    set(MBEDTLS_LIBRARIES mbedtls mbedx509 mbedcrypto)
    set(MBEDTLS_INCLUDE_DIRS ${mbedtls_SOURCE_DIR}/include)
else()
    set(MBEDTLS_LIBRARIES ${MBEDTLS_LIBRARY} ${MBEDX509_LIBRARY} ${MBEDCRYPTO_LIBRARY})
    set(MBEDTLS_INCLUDE_DIRS ${MBEDTLS_INCLUDE_DIR})
endif()

# Core library sources
set(CRAWL_SOURCES
    src/http_client.cpp
    src/happy_eyeballs.cpp
    src/tls_connection.cpp
    src/connection_pool.cpp
    src/dns_cache.cpp
    src/compression.cpp
    src/http2_client.cpp
    src/rate_limiter.cpp
    src/stats.cpp
)

# Build library
add_library(crawllib STATIC ${CRAWL_SOURCES})

target_include_directories(crawllib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${MBEDTLS_INCLUDE_DIRS}
)

target_link_libraries(crawllib PUBLIC
    ${MBEDTLS_LIBRARIES}
    Threads::Threads
)

if(ZLIB_FOUND)
    target_link_libraries(crawllib PUBLIC ZLIB::ZLIB)
endif()

if(BROTLI_INCLUDE_DIR AND BROTLI_DEC_LIBRARY)
    target_include_directories(crawllib PUBLIC ${BROTLI_INCLUDE_DIR})
    target_link_libraries(crawllib PUBLIC ${BROTLI_LIBRARIES})
endif()

if(NGHTTP2_INCLUDE_DIR AND NGHTTP2_LIBRARY)
    target_include_directories(crawllib PUBLIC ${NGHTTP2_INCLUDE_DIR})
    target_link_libraries(crawllib PUBLIC ${NGHTTP2_LIBRARY})
endif()

# Main executable
add_executable(crawl src/main.cpp)
target_link_libraries(crawl PRIVATE crawllib)

# Benchmark tool
add_executable(crawl-bench src/benchmark.cpp)
target_link_libraries(crawl-bench PRIVATE crawllib)

# Install
install(TARGETS crawl DESTINATION bin)
install(TARGETS crawllib DESTINATION lib)
install(DIRECTORY include/ DESTINATION include)

# Enable aggressive warnings
target_compile_options(crawllib PRIVATE
    -Wall -Wextra
    -Wno-unused-parameter
    -Wno-missing-field-initializers
)

message(STATUS "Crawl - Ultra-Fast HTTP Client")
message(STATUS "  Optimizations: ${CMAKE_CXX_FLAGS_RELEASE}")
message(STATUS "  C++ Standard: C++${CMAKE_CXX_STANDARD}")
