#!/bin/bash
# ════════════════════════════════════════════════════════════════
# Windows .exe 도구 자동 다운로드
#   Mac/Linux 에서 실행 → windows/resources/tools/ 에 배치
#   사용자가 windows ZIP 받으면 도구 다 들어있게 만듦
# ════════════════════════════════════════════════════════════════

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TOOLS_DIR="$(cd "$SCRIPT_DIR/../resources/tools" && pwd)"
TMP_DIR="$(mktemp -d)"

# ANSI 색상
GREEN='\033[32m'
YELLOW='\033[33m'
CYAN='\033[36m'
RED='\033[31m'
BOLD='\033[1m'
RESET='\033[0m'

echo -e "${BOLD}${CYAN}═══════════════════════════════════════════════════════════════${RESET}"
echo -e "${BOLD}${CYAN}  📦 Windows 용 도구 자동 다운로드${RESET}"
echo -e "${BOLD}${CYAN}═══════════════════════════════════════════════════════════════${RESET}"
echo -e "${YELLOW}대상 폴더:${RESET} $TOOLS_DIR"
echo ""

cd "$TMP_DIR"

# ─── 1. yt-dlp.exe ──────────────────────────────────────────────
echo -e "${BOLD}[1/6] yt-dlp.exe${RESET}"
if [ -f "$TOOLS_DIR/yt-dlp.exe" ] && [ "$1" != "--force" ]; then
    echo -e "  ${GREEN}✓ 이미 있음${RESET} ($(ls -lh "$TOOLS_DIR/yt-dlp.exe" | awk '{print $5}'))"
else
    echo -e "  ${CYAN}→ GitHub releases 에서 다운로드 중...${RESET}"
    curl -L -sS -o "$TOOLS_DIR/yt-dlp.exe" \
        "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe"
    echo -e "  ${GREEN}✓ 완료${RESET} ($(ls -lh "$TOOLS_DIR/yt-dlp.exe" | awk '{print $5}'))"
fi

# ─── 2. ffmpeg.exe + ffprobe.exe (gyan.dev essentials) ──────────
echo -e "${BOLD}[2/6] ffmpeg.exe + ffprobe.exe${RESET}"
if [ -f "$TOOLS_DIR/ffmpeg.exe" ] && [ -f "$TOOLS_DIR/ffprobe.exe" ] && [ "$1" != "--force" ]; then
    echo -e "  ${GREEN}✓ 이미 있음${RESET}"
else
    echo -e "  ${CYAN}→ gyan.dev essentials 다운로드 (~100MB)...${RESET}"
    curl -L -sS -o "$TMP_DIR/ffmpeg.zip" \
        "https://www.gyan.dev/ffmpeg/builds/ffmpeg-release-essentials.zip"
    echo -e "  ${CYAN}→ 압축 풀기 + 추출...${RESET}"
    cd "$TMP_DIR"
    unzip -q ffmpeg.zip
    FFMPEG_DIR=$(ls -d ffmpeg-* | head -1)
    cp "$FFMPEG_DIR/bin/ffmpeg.exe" "$TOOLS_DIR/"
    cp "$FFMPEG_DIR/bin/ffprobe.exe" "$TOOLS_DIR/"
    rm -rf "$FFMPEG_DIR" "$TMP_DIR/ffmpeg.zip"
    echo -e "  ${GREEN}✓ 완료${RESET} (ffmpeg: $(ls -lh "$TOOLS_DIR/ffmpeg.exe" | awk '{print $5}'), ffprobe: $(ls -lh "$TOOLS_DIR/ffprobe.exe" | awk '{print $5}'))"
fi

# ─── 3. exiftool.exe ────────────────────────────────────────────
echo -e "${BOLD}[3/6] exiftool.exe${RESET}"
if [ -f "$TOOLS_DIR/exiftool.exe" ] && [ "$1" != "--force" ]; then
    echo -e "  ${GREEN}✓ 이미 있음${RESET} ($(ls -lh "$TOOLS_DIR/exiftool.exe" | awk '{print $5}'))"
else
    echo -e "  ${CYAN}→ exiftool.org 최신 버전 확인 중...${RESET}"
    # 최신 버전 number 자동 가져오기
    VER=$(curl -sS "https://exiftool.org/ver.txt" | tr -d '\n\r ')
    if [ -z "$VER" ]; then VER="13.59"; fi
    echo -e "  ${CYAN}→ exiftool ${VER} 다운로드 (~12MB)...${RESET}"
    # ★ exiftool.org 는 버전별 zip 을 직접 두지 않는다(구버전 링크는 404). CI 와 같은
    #   순서로 미러를 훑는다 — oliverbetz.de 가 실제 ZIP 을 주고, SourceForge 는 폴백.
    #   예전에는 SourceForge 만 봤는데 HTML 이 내려오는 날이 있었고, 그러면 아래 unzip 이
    #   실패하면서 set -e 가 스크립트를 통째로 끝냈다 — 뒤의 rclone·deno 는 실행조차
    #   안 됐다. deno 는 yt-dlp 의 JS 런타임이라 없으면 YouTube 추출에서 포맷이 빠진다.
    EXIF_OK=0
    for U in \
        "https://oliverbetz.de/cms/files/Artikel/ExifTool-for-Windows/exiftool-${VER}_64.zip" \
        "https://sourceforge.net/projects/exiftool/files/exiftool-${VER}_64.zip/download"
    do
        curl -L -sS -A "Mozilla/5.0" -o "$TMP_DIR/exiftool.zip" "$U" || continue
        # 진짜 ZIP 인지 본다 (HTML 을 받아 놓고 압축을 푸는 사고 방지) — PK\x03\x04
        if [ "$(head -c 2 "$TMP_DIR/exiftool.zip" 2>/dev/null)" = "PK" ]; then
            EXIF_OK=1; break
        fi
        echo -e "  ${YELLOW}  · ZIP 이 아님 (다음 미러 시도): ${U}${RESET}"
    done
    cd "$TMP_DIR"
    if [ "$EXIF_OK" != "1" ] || ! unzip -q exiftool.zip; then
        echo -e "  ${RED}✗ exiftool 내려받기 실패 — 건너뛰고 계속합니다${RESET}"
        rm -f exiftool.zip
        cd "$SCRIPT_DIR" 2>/dev/null || cd - >/dev/null
    else
        # 파일명이 exiftool(-k).exe 인 경우 rename
        if [ -f "exiftool(-k).exe" ]; then
            mv "exiftool(-k).exe" exiftool.exe
        fi
        if [ -f "exiftool.exe" ]; then
            cp exiftool.exe "$TOOLS_DIR/"
            # exiftool_files 폴더도 함께 (Perl runtime + 라이브러리)
            if [ -d "exiftool_files" ]; then
                cp -R exiftool_files "$TOOLS_DIR/"
            fi
            echo -e "  ${GREEN}✓ 완료${RESET} ($(ls -lh "$TOOLS_DIR/exiftool.exe" | awk '{print $5}'))"
        else
            echo -e "  ${RED}✗ 실패 — exiftool.exe 추출 못함${RESET}"
        fi
        rm -rf exiftool.exe exiftool_files exiftool.zip
    fi
fi

# ─── 4. rclone.exe (WebDAV/SFTP/S3 고속 백업) ───────────────────
echo -e "${BOLD}[4/6] rclone.exe${RESET}"
if [ -f "$TOOLS_DIR/rclone.exe" ] && [ "$1" != "--force" ]; then
    echo -e "  ${GREEN}✓ 이미 있음${RESET} ($(ls -lh "$TOOLS_DIR/rclone.exe" | awk '{print $5}'))"
else
    echo -e "  ${CYAN}→ rclone.org 최신 windows-amd64 다운로드...${RESET}"
    curl -L -sS -o "$TMP_DIR/rclone.zip" \
        "https://downloads.rclone.org/rclone-current-windows-amd64.zip"
    cd "$TMP_DIR"
    if unzip -q rclone.zip; then
        RCLONE_DIR=$(ls -d rclone-*-windows-amd64 2>/dev/null | head -1)
        if [ -n "$RCLONE_DIR" ] && [ -f "$RCLONE_DIR/rclone.exe" ]; then
            cp "$RCLONE_DIR/rclone.exe" "$TOOLS_DIR/"
            echo -e "  ${GREEN}✓ 완료${RESET} ($(ls -lh "$TOOLS_DIR/rclone.exe" | awk '{print $5}'))"
        else
            echo -e "  ${RED}✗ 실패 — rclone.exe 추출 못함 (고속 백업 비활성 — 일반 백업으로 동작)${RESET}"
        fi
        rm -rf "$RCLONE_DIR"
    else
        echo -e "  ${RED}✗ 실패 — 다운로드/압축해제 못함 (고속 백업 비활성 — 일반 백업으로 동작)${RESET}"
    fi
    rm -f "$TMP_DIR/rclone.zip"
fi

# ─── 5. deno.exe (yt-dlp JS 런타임 — YouTube 추출에 필수) ───────
echo -e "${BOLD}[5/6] deno.exe (yt-dlp JS 런타임)${RESET}"
if [ -f "$TOOLS_DIR/deno.exe" ] && [ "$1" != "--force" ]; then
    echo -e "  ${GREEN}✓ 이미 있음${RESET} ($(ls -lh "$TOOLS_DIR/deno.exe" | awk '{print $5}'))"
else
    echo -e "  ${CYAN}→ denoland/deno releases 에서 windows-amd64 다운로드 (~40MB)...${RESET}"
    curl -L -sS -o "$TMP_DIR/deno.zip" \
        "https://github.com/denoland/deno/releases/latest/download/deno-x86_64-pc-windows-msvc.zip"
    cd "$TMP_DIR"
    if unzip -q deno.zip && [ -f "deno.exe" ]; then
        cp deno.exe "$TOOLS_DIR/"
        echo -e "  ${GREEN}✓ 완료${RESET} ($(ls -lh "$TOOLS_DIR/deno.exe" | awk '{print $5}'))"
    else
        echo -e "  ${RED}✗ 실패 — deno.exe 추출 못함 (YouTube 는 JS 런타임 없이 동작 — 일부 포맷 누락)${RESET}"
    fi
    rm -f "$TMP_DIR/deno.zip" "$TMP_DIR/deno.exe"
fi

# ─── 6. Python embed (선택, 없어도 시스템 Python 사용) ──────────
echo -e "${BOLD}[6/6] Python embed (twikit, atproto 등 위해)${RESET}"
if [ -d "$TOOLS_DIR/python_embed" ] && [ "$1" != "--force" ]; then
    echo -e "  ${GREEN}✓ 이미 있음${RESET}"
else
    echo -e "  ${YELLOW}⚠ Python embed 자동 다운로드 skip — Windows 빌드 시 bundle_python_win.bat 가 처리${RESET}"
    echo -e "  ${YELLOW}   (Windows 머신에서 빌드해야 venv 가 OS-specific 으로 만들어짐)${RESET}"
fi

# 정리
rm -rf "$TMP_DIR"

echo ""
echo -e "${BOLD}${CYAN}═══════════════════════════════════════════════════════════════${RESET}"
echo -e "${BOLD}${GREEN}  ✅ 완료 — 다운로드된 도구:${RESET}"
echo -e "${BOLD}${CYAN}═══════════════════════════════════════════════════════════════${RESET}"
ls -lh "$TOOLS_DIR"/*.exe 2>/dev/null
# ★ 한 단계가 실패해도 나머지는 계속 받으므로, 무엇이 빠졌는지 분명히 말해 준다.
#   "완료" 만 찍고 끝나면 사용자는 deno 가 없는 줄 모른 채 YouTube 가 반만 되는 것을 겪는다.
MISSING=""
for T in yt-dlp.exe ffmpeg.exe ffprobe.exe exiftool.exe rclone.exe deno.exe; do
    [ -f "$TOOLS_DIR/$T" ] || MISSING="$MISSING $T"
done
if [ -n "$MISSING" ]; then
    echo ""
    echo -e "${YELLOW}⚠ 빠진 도구:${RESET}$MISSING"
    echo -e "   다시 실행하거나(--force) 해당 배포처에서 직접 받아 $TOOLS_DIR 에 두세요."
    echo -e "   (deno 가 없으면 yt-dlp 의 YouTube 포맷 추출이 일부 빠집니다)"
fi
echo ""
echo -e "${YELLOW}다음 단계:${RESET}"
echo -e "  1. windows/ 폴더를 Windows 머신으로 복사"
echo -e "  2. Qt 6.7+ (MSVC 빌드) + CMake 3.20+ 설치"
echo -e "     ※ MinGW Qt 로는 안 됩니다 — Qt 가 MinGW 용 QtWebEngine 을 배포하지 않습니다."
echo -e "  3. windows/build_windows.bat 실행"
echo -e "  4. dist/win/Predormition.exe 동작 확인 (predormition.iss 가 이 폴더를 패키징)"
echo ""
