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

# ── yt-dlp 래퍼는 저장소에 있다(mac/chernobyl/resources/tools/yt-dlp — 우리가 쓴 셸 스크립트) ──
#   ★ 예전엔 여기서 heredoc 으로 늘 다시 썼다. 그 사본이 낡아(덧씌우기 pypkgs · HANISHIKI_PYTHON 을 모르고 옛 Miyo 경로를
#     봄) 새 맥에서 이 스크립트를 돌리면 저장소의 래퍼가 옛것으로 덮였다(장기 지원 점검). 이제 건드리지 않고 있는지만 본다.
if [ ! -x yt-dlp ]; then
    echo "⚠ yt-dlp 래퍼가 없습니다 — 저장소에서 되살리십시오: git checkout -- mac/chernobyl/resources/tools/yt-dlp"
fi

# ── 판 고정 — 어느 맥에서 받아도 같은 도구가 들어가게(지금 번들의 판). 고정한 주소가 없으면 latest 로 물러선다 ──
#   판을 올릴 때는 여기와 akashi/docs/정비수칙.md 의 '새 맥에서 빌드하기' 를 함께 고친다.
FFMPEG_TAG="b6.0"         # eugeneware/ffmpeg-static — ffmpeg 6.0
DENO_VER="v2.9.3"
RCLONE_VER="v1.74.4"
EXIFTOOL_VER="13.59"
dl_pinned() {   # dl_pinned <받을 자리> <고정 주소> <latest 주소>
    dl "$1" "$2" || { echo "  (고정 판 주소를 못 받아 latest 로 받습니다: $3)"; dl "$1" "$3"; }
}

# ── ffmpeg (static — dylib 의존 없음) ──────────────────────────────────────
if [ ! -x ffmpeg ]; then
    echo "▶ ffmpeg (static)..."
    FF="ffmpeg-darwin-arm64.gz"; [ "$ARCH" = "x86_64" ] && FF="ffmpeg-darwin-x64.gz"
    dl_pinned "$TMP/ff.gz" "https://github.com/eugeneware/ffmpeg-static/releases/download/$FFMPEG_TAG/$FF" \
              "https://github.com/eugeneware/ffmpeg-static/releases/latest/download/$FF"
    gunzip -c "$TMP/ff.gz" > ffmpeg
    chmod +x ffmpeg
fi

# ── ffprobe (static) — 예전엔 이 스크립트에 없어, 새 맥의 빌드에는 ffprobe 가 빠졌다 ──────────────
if [ ! -x ffprobe ]; then
    echo "▶ ffprobe (static)..."
    FP="ffprobe-darwin-arm64.gz"; [ "$ARCH" = "x86_64" ] && FP="ffprobe-darwin-x64.gz"
    if dl_pinned "$TMP/fp.gz" "https://github.com/eugeneware/ffmpeg-static/releases/download/$FFMPEG_TAG/$FP" \
                 "https://github.com/eugeneware/ffmpeg-static/releases/latest/download/$FP"; then
        gunzip -c "$TMP/fp.gz" > ffprobe
        chmod +x ffprobe
    else
        echo "⚠ ffprobe 를 받지 못했습니다 — 앱은 Homebrew 의 ffprobe 로 물러서거나 영상 길이 확인을 건너뜁니다"
    fi
fi

# ── deno (yt-dlp 2025+ YouTube nsig JS 런타임) ─────────────────────────────
if [ ! -x deno ]; then
    echo "▶ deno..."
    DN="deno-aarch64-apple-darwin.zip"; [ "$ARCH" = "x86_64" ] && DN="deno-x86_64-apple-darwin.zip"
    dl_pinned "$TMP/deno.zip" "https://github.com/denoland/deno/releases/download/$DENO_VER/$DN" \
              "https://github.com/denoland/deno/releases/latest/download/$DN"
    unzip -oq "$TMP/deno.zip" -d "$TMP/deno"
    cp "$TMP/deno/deno" deno
    chmod +x deno
fi

# ── rclone (WebDAV/SFTP/S3 백업 — MIT) ─────────────────────────────────────
if [ ! -x rclone ]; then
    echo "▶ rclone..."
    RA="osx-arm64"; [ "$ARCH" = "x86_64" ] && RA="osx-amd64"
    dl_pinned "$TMP/rclone.zip" "https://downloads.rclone.org/$RCLONE_VER/rclone-$RCLONE_VER-$RA.zip" \
              "https://downloads.rclone.org/rclone-current-$RA.zip"
    unzip -oq "$TMP/rclone.zip" -d "$TMP/rclone"
    cp "$(find "$TMP/rclone" -name rclone -type f | head -1)" rclone
    chmod +x rclone
fi

# ── exiftool (Perl 스크립트 + lib — 인터프리터는 OS perl /usr/bin/perl) ────
if [ ! -x exiftool/exiftool ]; then
    echo "▶ exiftool..."
    dl_pinned "$TMP/et.tar.gz" "https://github.com/exiftool/exiftool/archive/refs/tags/$EXIFTOOL_VER.tar.gz" \
              "https://github.com/exiftool/exiftool/archive/refs/heads/master.tar.gz"
    tar -xzf "$TMP/et.tar.gz" -C "$TMP"
    SRC="$(find "$TMP" -maxdepth 1 -type d -name 'exiftool-*' | head -1)"
    rm -rf exiftool && mkdir -p exiftool
    cp "$SRC/exiftool" exiftool/exiftool
    cp -R "$SRC/lib" exiftool/lib
    chmod +x exiftool/exiftool
fi

echo ""
echo "✅ 완료. 배치된 도구:"
for t in yt-dlp ffmpeg ffprobe deno rclone exiftool/exiftool; do
    if [ -x "$t" ]; then printf "  %-18s %s\n" "$t" "$(du -h "$t" | cut -f1)"; else echo "  $t  ❌ 없음"; fi
done
echo ""
echo "→ 이제 재빌드하면 POST_BUILD 가 번들에 복사한다 (앱 내부 실행)."
