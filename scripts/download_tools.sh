#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════
# download_tools.sh — 앱이 '내부에서' 실행할 자체완결 외부 도구를 받아
#   mac/chernobyl/resources/tools/ 에 배치한다. 빌드(POST_BUILD)가 이걸 번들에 복사.
#
# 이 파일들이 없으면 CMakeLists 의 if(EXISTS) 가 스킵 → 앱이 시스템(/opt/homebrew)
# 도구로 폴백한다("전부 앱 내부" 위반). 빌드 전에 1회 실행.
#
# 산출물(다운로드 결과물)·임시 저장공간은 대상 아님 — 도구/라이브러리만 내부화.
#
# 사용법:  bash scripts/download_tools.sh            # 현재 repo 의 mac 트리
#          TOOLS_DIR=<경로> bash scripts/download_tools.sh
# ═══════════════════════════════════════════════════════════════════════════
set -euo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
TOOLS_DIR="${TOOLS_DIR:-$HERE/../mac/chernobyl/resources/tools}"
mkdir -p "$TOOLS_DIR"
cd "$TOOLS_DIR"
echo "▶ 도구 배치 위치: $TOOLS_DIR"

ARCH="$(uname -m)"   # arm64 / x86_64
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT

dl() { curl -fL --retry 3 -s -o "$1" "$2"; }

# ── yt-dlp (번들 python 모듈 래퍼 — PyInstaller onefile 은 매 실행 ~18s 라 안 씀) ──
#   실제 yt_dlp 모듈은 bundle_python.sh 가 python_env 에 pip install 한다.
#   이 래퍼는 그 모듈을 호출(~0.5s). 위치 무관하게 python_env 후보를 순회.
echo "▶ yt-dlp (python 모듈 래퍼)..."
cat > yt-dlp <<'WRAP'
#!/bin/sh
# ★ .pyc 를 서명된 앱 번들 안에 쓰면 codesign 봉인이 깨져 macOS 가 앱을 죽인다.
#   → 쓰기가능 python_env 를 '먼저' 쓰고, 어떤 경우에도 bytecode 를 남기지 않는다.
export PYTHONDONTWRITEBYTECODE=1
DIR="$(cd "$(dirname "$0")" && pwd)"
for P in \
  "$HOME/Library/Application Support/Miyo/Predormition/python_env_arm64/bin/python3" \
  "$HOME/Library/Application Support/Miyo/Chernobyl/python_env_arm64/bin/python3" \
  "$HOME/Library/Application Support/Miyo/Chernobyl/python_env/bin/python3" \
  "$DIR/../Resources/python_env/bin/python3" \
  "$DIR/../python_env/bin/python3"; do
  [ -x "$P" ] && exec "$P" -m yt_dlp "$@"
done
exec python3 -m yt_dlp "$@"
WRAP
chmod +x yt-dlp

# ── ffmpeg (static — dylib 의존 없음) ──────────────────────────────────────
if [ ! -x ffmpeg ]; then
    echo "▶ ffmpeg (static)..."
    FF="ffmpeg-darwin-arm64.gz"; [ "$ARCH" = "x86_64" ] && FF="ffmpeg-darwin-x64.gz"
    dl "$TMP/ff.gz" "https://github.com/eugeneware/ffmpeg-static/releases/latest/download/$FF"
    gunzip -c "$TMP/ff.gz" > ffmpeg
    chmod +x ffmpeg
fi

# ── deno (yt-dlp 2025+ YouTube nsig JS 런타임) ─────────────────────────────
if [ ! -x deno ]; then
    echo "▶ deno..."
    DN="deno-aarch64-apple-darwin.zip"; [ "$ARCH" = "x86_64" ] && DN="deno-x86_64-apple-darwin.zip"
    dl "$TMP/deno.zip" "https://github.com/denoland/deno/releases/latest/download/$DN"
    unzip -oq "$TMP/deno.zip" -d "$TMP/deno"
    cp "$TMP/deno/deno" deno
    chmod +x deno
fi

# ── rclone (WebDAV/SFTP/S3 백업 — MIT) ─────────────────────────────────────
if [ ! -x rclone ]; then
    echo "▶ rclone..."
    RC="rclone-current-osx-arm64.zip"; [ "$ARCH" = "x86_64" ] && RC="rclone-current-osx-amd64.zip"
    dl "$TMP/rclone.zip" "https://downloads.rclone.org/$RC"
    unzip -oq "$TMP/rclone.zip" -d "$TMP/rclone"
    cp "$(find "$TMP/rclone" -name rclone -type f | head -1)" rclone
    chmod +x rclone
fi

# ── exiftool (Perl 스크립트 + lib — 인터프리터는 OS perl /usr/bin/perl) ────
if [ ! -x exiftool/exiftool ]; then
    echo "▶ exiftool..."
    dl "$TMP/et.tar.gz" "https://github.com/exiftool/exiftool/archive/refs/heads/master.tar.gz"
    tar -xzf "$TMP/et.tar.gz" -C "$TMP"
    SRC="$(find "$TMP" -maxdepth 1 -type d -name 'exiftool-*' | head -1)"
    rm -rf exiftool && mkdir -p exiftool
    cp "$SRC/exiftool" exiftool/exiftool
    cp -R "$SRC/lib" exiftool/lib
    chmod +x exiftool/exiftool
fi

echo ""
echo "✅ 완료. 배치된 도구:"
for t in yt-dlp ffmpeg deno rclone exiftool/exiftool; do
    if [ -x "$t" ]; then printf "  %-18s %s\n" "$t" "$(du -h "$t" | cut -f1)"; else echo "  $t  ❌ 없음"; fi
done
echo ""
echo "→ 이제 재빌드하면 POST_BUILD 가 번들에 복사한다 (앱 내부 실행)."
