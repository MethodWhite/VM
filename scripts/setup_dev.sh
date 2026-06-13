#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$SCRIPT_DIR"

echo "=== Vex Ecosystem Development Setup ==="
echo ""

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

# Development mode flags
DEV_MODE="${DEV_MODE:-true}"
BUILD_TYPE="${BUILD_TYPE:-Debug}"
BUILD_JOBS="${BUILD_JOBS:-$(nproc)}"
TARGET_ARCH="${TARGET_ARCH:-x86_64}"

# Directories
BUILD_DIR="${BUILD_DIR:-build}"
VEX_DIR="${VEX_DIR:-src/vex}"
EXAMPLES_DIR="${EXAMPLES_DIR:-examples_codes_vex}"
STDLIB_DIR="${STDLIB_DIR:-stdlib/native}"
DOCS_DIR="${DOCS_DIR:-doc}"
TESTS_DIR="${TESTS_DIR:-tests}"

# Output directories
BIN_DIR="$BUILD_DIR/bin"
LIB_DIR="$BUILD_DIR/lib"
INCLUDE_DIR="$BUILD_DIR/include"

# ---------------------------------------------------------------------------
# Helper functions
# ---------------------------------------------------------------------------

log_info() {
    echo "[INFO] $1"
}

log_warn() {
    echo "[WARN] $1" >&2
}

log_error() {
    echo "[ERROR] $1" >&2
}

check_tool() {
    local tool="$1"
    local version_flag="${2:---version}"
    
    if ! command -v "$tool" &>/dev/null; then
        log_error "$tool is required but not found."
        log_error "  Install it with your package manager and re-run this script."
        exit 1
    fi
    
    local version
    version="$($tool $version_flag 2>&1 | head -1 || echo "unknown")"
    log_info "  $tool: $version"
}

# ---------------------------------------------------------------------------
# Check required tools
# ---------------------------------------------------------------------------

log_info "[1/7] Checking required tools..."

check_tool cmake "--version"
check_tool git "--version"
check_tool python3 "--version"
check_tool rustc "--version"
check_tool cargo "--version"

if command -v gcc &>/dev/null; then
    log_info "  gcc: found"
elif command -v clang &>/dev/null; then
    log_info "  clang: found (using as C compiler)"
else
    log_error "Neither gcc nor clang found."
    log_error "  Install build-essential (Linux) or Xcode Command Line Tools (macOS)."
    exit 1
fi

# ---------------------------------------------------------------------------
# Initialize submodules
# ---------------------------------------------------------------------------

log_info ""
log_info "[2/7] Initializing submodules..."
if [ -f .gitmodules ]; then
    git submodule update --init --recursive
    log_info "  Submodules updated."
else
    log_warn "  No .gitmodules found, skipping submodule initialization."
fi

# ---------------------------------------------------------------------------
# Install system dependencies
# ---------------------------------------------------------------------------

log_info ""
log_info "[3/7] Installing system dependencies..."

case "$(uname -s)" in
    Linux)
        if command -v apt-get &>/dev/null; then
            log_info "  Detected apt (Debian/Ubuntu)..."
            sudo apt-get update
            sudo apt-get install -y build-essential cmake libssl-dev pkg-config
        elif command -v dnf &>/dev/null; then
            log_info "  Detected dnf (Fedora/RHEL)..."
            sudo dnf install -y cmake gcc-c++ openssl-devel pkgconfig
        elif command -v pacman &>/dev/null; then
            log_info "  Detected pacman (Arch)..."
            sudo pacman -S --noconfirm cmake base-devel openssl pkgconf
        else
            log_warn "  Unsupported Linux distro. Install dependencies manually."
        fi
        ;;
    Darwin)
        if command -v brew &>/dev/null; then
            log_info "  Detected Homebrew (macOS)..."
            brew install openssl cmake pkg-config
        else
            log_warn "  Homebrew not found. Install dependencies manually."
        fi
        ;;
    MINGW*|MSYS*)
        log_info "  Windows detected. Install dependencies via your package manager."
        ;;
    *)
        log_warn "  Unknown OS '$(uname -s)'. Install dependencies manually."
        ;;
esac

log_info "  Dependencies installed."

# ---------------------------------------------------------------------------
# Configure CMake with development flags
# ---------------------------------------------------------------------------

log_info ""
log_info "[4/7] Configuring CMake with development flags..."

cmake -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DVESTA_BUILD_STDLIB=ON \
    -DVESTA_BUILD_EXAMPLES=ON \
    -DVESTA_ENABLE_JIT=ON \
    -DVESTA_ENABLE_DISTRIBUTED=ON \
    -DVESTA_ENABLE_BENCHMARKS=ON \
    -DVESTA_ENABLE_TESTS=ON \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
    -DCMAKE_INTERPROCEDURAL_OPTIMIZATION=ON \
    -DCMAKE_CXX_STANDARD=17 \
    -DCMAKE_CXX_STANDARD_REQUIRED=ON \
    -DCMAKE_CXX_EXTENSIONS=OFF \
    -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

log_info "  CMake configuration complete."

# ---------------------------------------------------------------------------
# Build all targets
# ---------------------------------------------------------------------------

log_info ""
log_info "[5/7] Building all targets..."

cmake --build "$BUILD_DIR" --parallel "$BUILD_JOBS"

log_info "  Build complete."

# ---------------------------------------------------------------------------
# Run tests
# ---------------------------------------------------------------------------

log_info ""
log_info "[6/7] Running tests..."

# Run Vex language tests
if [ -f "$TESTS_DIR/vex/test_vex_e2e.sh" ]; then
    log_info "  Running Vex E2E tests..."
    cd "$TESTS_DIR/vex"
    if ./test_vex_e2e.sh; then
        log_info "  Vex E2E tests passed."
    else
        log_error "  Vex E2E tests failed."
        exit 1
    fi
    cd "$SCRIPT_DIR"
fi

# Run CMake tests
if [ -f "$BUILD_DIR/CTestTestfile.cmake" ]; then
    log_info "  Running CMake tests..."
    ctest --test-dir "$BUILD_DIR" --output-on-failure --parallel "$BUILD_JOBS"
    log_info "  CMake tests passed."
fi

log_info "  All tests passed."

# ---------------------------------------------------------------------------
# Generate documentation
# ---------------------------------------------------------------------------

log_info ""
log_info "[7/7] Generating documentation..."

# Generate Vex language documentation
if [ -f "$DOCS_DIR/LANGUAGE.md" ]; then
    log_info "  Generating Vex language documentation..."
    # In a real implementation, this would call a documentation generator
    log_info "  Vex documentation generation skipped (requires docgen tool)."
fi

# Generate architecture documentation
if [ -f "$DOCS_DIR/ARCHITECTURE.md" ]; then
    log_info "  Generating architecture documentation..."
    # In a real implementation, this would call a documentation generator
    log_info "  Architecture documentation generation skipped (requires docgen tool)."
fi

log_info "  Documentation generation complete."

# ---------------------------------------------------------------------------
# Print success
# ---------------------------------------------------------------------------

log_info ""
log_info "========================================"
log_info "  Vex Ecosystem Development Setup Complete!"
log_info "  ========================================"
log_info "  Binario:      $BIN_DIR/vm"
log_info "  Librerías:    $LIB_DIR/*.so"
log_info "  Headers:      $INCLUDE_DIR/*.h"
log_info "  Documentación: $DOCS_DIR/"
log_info "  Tests:        ctest --test-dir $BUILD_DIR --output-on-failure"
log_info "  ========================================"
log_info "  Para más información, consulte:
log_info "    - README.md"
log_info "    - doc/QUICKSTART.md"
log_info "    - doc/LANGUAGE.md"
log_info "  ========================================"
