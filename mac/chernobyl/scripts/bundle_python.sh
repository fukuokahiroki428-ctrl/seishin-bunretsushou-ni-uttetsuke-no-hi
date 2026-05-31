#!/bin/bash
# Bundle standalone Python 3.14 + packages into the app bundle
# No system Python dependency — fully self-contained
# Usage: bundle_python.sh <APP_BUNDLE_PATH>

set -e

APP_BUNDLE="$1"
if [ -z "$APP_BUNDLE" ]; then
    echo "Usage: $0 <path/to/ABIWA.app>"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
RESOURCES="$APP_BUNDLE/Contents/Resources"
PYTHON_DIR="$RESOURCES/python_env"
MARKER="$PYTHON_DIR/.bundled_ok"

# Standalone Python tarball (auto-downloaded on first build)
TARBALL="$SCRIPT_DIR/python_standalone/cpython-3.14-aarch64-apple-darwin.tar.gz"
DOWNLOAD_URL="https://github.com/astral-sh/python-build-standalone/releases/download/20260320/cpython-3.14.3%2B20260320-aarch64-apple-darwin-install_only.tar.gz"

# Check if already bundled and working
if [ -f "$MARKER" ]; then
    BUNDLED_PY="$PYTHON_DIR/bin/python3"
    if "$BUNDLED_PY" -c "import twikit; import httpx; import atproto; import browser_cookie3" 2>/dev/null; then
        echo "[bundle_python] Already bundled and OK, skipping"
        exit 0
    fi
    echo "[bundle_python] Packages missing, rebuilding..."
    rm -rf "$PYTHON_DIR"
fi

# Download tarball if not cached
if [ ! -f "$TARBALL" ]; then
    echo "[bundle_python] Downloading standalone Python 3.14..."
    mkdir -p "$(dirname "$TARBALL")"
    curl -L -o "$TARBALL" "$DOWNLOAD_URL"
fi

# Extract standalone Python
echo "[bundle_python] Extracting standalone Python 3.14..."
mkdir -p "$PYTHON_DIR"
# The tarball extracts to a 'python/' directory
tar xzf "$TARBALL" -C "$PYTHON_DIR" --strip-components=1

BUNDLED_PY="$PYTHON_DIR/bin/python3"

# Verify extraction
if [ ! -x "$BUNDLED_PY" ]; then
    echo "[bundle_python] ERROR: Python binary not found after extraction"
    ls -la "$PYTHON_DIR/bin/" 2>/dev/null || echo "(bin dir missing)"
    exit 1
fi

echo "[bundle_python] Python: $("$BUNDLED_PY" --version)"

# Install packages
echo "[bundle_python] Installing twikit, httpx, atproto, browser_cookie3..."
"$BUNDLED_PY" -m pip install --quiet --no-cache-dir twikit httpx atproto openpyxl Pillow piexif browser_cookie3 2>&1 | tail -5

# Verify
echo "[bundle_python] Verifying..."
"$BUNDLED_PY" -c "
import twikit, httpx, atproto, openpyxl, PIL, piexif, bs4
print(f'twikit {twikit.__version__}')
print(f'httpx {httpx.__version__}')
print(f'Pillow {PIL.__version__}')
print(f'piexif {piexif.VERSION}')
print(f'openpyxl {openpyxl.__version__}')
print(f'bs4 {bs4.__version__}')
print('atproto OK')
print('All packages verified!')
"

# Clean up to reduce size
echo "[bundle_python] Pre-compiling all .py files (prevent runtime .pyc generation)..."
"$BUNDLED_PY" -m compileall -q "$PYTHON_DIR/lib" 2>/dev/null || true

echo "[bundle_python] Cleaning up..."
find "$PYTHON_DIR" -name "tests" -type d -exec rm -rf {} + 2>/dev/null || true
find "$PYTHON_DIR" -name "test" -type d -exec rm -rf {} + 2>/dev/null || true
# Remove unnecessary large dirs
rm -rf "$PYTHON_DIR/share/man" 2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/python3.14/test" 2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/python3.14/unittest/test" 2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/python3.14/lib2to3" 2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/python3.14/idlelib" 2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/python3.14/tkinter" 2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/python3.14/turtledemo" 2>/dev/null || true
# ensurepip 유지 — pip 복구에 필요
rm -rf "$PYTHON_DIR/lib/python3.14/pydoc_data" 2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/python3.14/distutils" 2>/dev/null || true
rm -rf "$PYTHON_DIR/include" 2>/dev/null || true
rm -rf "$PYTHON_DIR/share" 2>/dev/null || true
# pip은 유지 — 앱 내 모듈 업데이트 기능에 필요
# setuptools는 삭제 가능
rm -rf "$PYTHON_DIR/lib/python3.14/site-packages/setuptools" 2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/python3.14/site-packages/setuptools-"* 2>/dev/null || true
rm -rf "$PYTHON_DIR/lib/python3.14/site-packages/_distutils_hack" 2>/dev/null || true
# .dist-info 유지 — importlib.metadata 로 버전 조회에 필요
# __pycache__ 정리 (이미 .pyc로 컴파일됨, 원본 .py 옆 캐시 불필요)
find "$PYTHON_DIR" -name "__pycache__" -type d -exec rm -rf {} + 2>/dev/null || true

# Write marker
echo "standalone python 3.14 bundled $(date)" > "$MARKER"

SIZE=$(du -sh "$PYTHON_DIR" | cut -f1)
echo "[bundle_python] Done! Standalone Python 3.14 size: $SIZE"
