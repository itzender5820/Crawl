#!/bin/bash

set -e

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m' # No Color

echo -e "${GREEN}Crawl - Ultra-Fast HTTP Client Builder${NC}"
echo ""

# Parse arguments
BUILD_TYPE="Release"
ENABLE_PGO=false
USE_PGO=false
CLEAN=false

while [[ $# -gt 0 ]]; do
    case $1 in
        --debug)
            BUILD_TYPE="Debug"
            shift
            ;;
        --pgo-gen)
            ENABLE_PGO=true
            shift
            ;;
        --pgo-use)
            USE_PGO=true
            shift
            ;;
        --clean)
            CLEAN=true
            shift
            ;;
        *)
            echo -e "${RED}Unknown option: $1${NC}"
            exit 1
            ;;
    esac
done

# Clean if requested
if [ "$CLEAN" = true ]; then
    echo -e "${YELLOW}Cleaning build directory...${NC}"
    rm -rf build
fi

# Create build directory
mkdir -p build
cd build

# Configure
echo -e "${YELLOW}Configuring build...${NC}"
CMAKE_FLAGS="-DCMAKE_BUILD_TYPE=$BUILD_TYPE"

if [ "$ENABLE_PGO" = true ]; then
    CMAKE_FLAGS="$CMAKE_FLAGS -DENABLE_PGO_GEN=ON"
    echo -e "${YELLOW}Profile-Guided Optimization: Generating profile data${NC}"
fi

if [ "$USE_PGO" = true ]; then
    CMAKE_FLAGS="$CMAKE_FLAGS -DENABLE_PGO_USE=ON"
    echo -e "${YELLOW}Profile-Guided Optimization: Using profile data${NC}"
fi

cmake $CMAKE_FLAGS ..

# Build
echo -e "${YELLOW}Building...${NC}"
CORES=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
make -j$CORES

echo ""
echo -e "${GREEN}Build complete!${NC}"
echo ""
echo "Executables:"
echo "  ./crawl         - Main HTTP client"
echo "  ./crawl-bench   - Benchmark tool"
echo ""

if [ "$ENABLE_PGO" = true ]; then
    echo -e "${YELLOW}PGO Training:${NC}"
    echo "Run your typical workload to generate profile data:"
    echo "  ./crawl https://example.com"
    echo "  ./crawl-bench https://example.com 100 10"
    echo ""
    echo "Then rebuild with --pgo-use flag:"
    echo "  ../build.sh --pgo-use"
    echo ""
fi

# Run quick test
if [ "$BUILD_TYPE" = "Release" ] && [ "$ENABLE_PGO" = false ]; then
    echo -e "${YELLOW}Running quick test...${NC}"
    if ./crawl --help > /dev/null 2>&1; then
        echo -e "${GREEN}✓ Basic test passed${NC}"
    else
        echo -e "${RED}✗ Basic test failed${NC}"
        exit 1
    fi
fi

echo ""
echo -e "${GREEN}Ready to crawl the web at ultra-fast speeds!${NC}"
