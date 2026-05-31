#!/bin/bash
# ════════════════════════════════════════════════════════════════
# Windows .exe 도구 자동 다운로드
#   Mac/Linux 에서 실행 → windows/miyo_cpp/resources/tools/ 에 배치
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
echo -e "${BOLD}[1/4] yt-dlp.exe${RESET}"
if [ -f "$TOOLS_DIR/yt-dlp.exe" ] && [ "$1" != "--force" ]; then
    echo -e "  ${GREEN}✓ 이미 있음${RESET} ($(ls -lh "$TOOLS_DIR/yt-dlp.exe" | awk '{print $5}'))"
else
    echo -e "  ${CYAN}→ GitHub releases 에서 다운로드 중...${RESET}"
    curl -L -sS -o "$TOOLS_DIR/yt-dlp.exe" \
        "https://github.com/yt-dlp/yt-dlp/releases/latest/download/yt-dlp.exe"
    echo -e "  ${GREEN}✓ 완료${RESET} ($(ls -lh "$TOOLS_DIR/yt-dlp.exe" | awk '{print $5}'))"
fi

# ─── 2. ffmpeg.exe + ffprobe.exe (gyan.dev essentials) ──────────
echo -e "${BOLD}[2/4] ffmpeg.exe + ffprobe.exe${RESET}"
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
echo -e "${BOLD}[3/4] exiftool.exe${RESET}"
if [ -f "$TOOLS_DIR/exiftool.exe" ] && [ "$1" != "--force" ]; then
    echo -e "  ${GREEN}✓ 이미 있음${RESET} ($(ls -lh "$TOOLS_DIR/exiftool.exe" | awk '{print $5}'))"
else
    echo -e "  ${CYAN}→ exiftool.org 최신 버전 확인 중...${RESET}"
    # 최신 버전 number 자동 가져오기
    VER=$(curl -sS "https://exiftool.org/ver.txt" | tr -d '\n\r ')
    if [ -z "$VER" ]; then VER="12.97"; fi
    echo -e "  ${CYAN}→ exiftool ${VER} 다운로드 (~12MB)...${RESET}"
    curl -L -sS -o "$TMP_DIR/exiftool.zip" \
        "https://exiftool.org/exiftool-${VER}_64.zip"
    cd "$TMP_DIR"
    unzip -q exiftool.zip
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

# ─── 4. Python embed (선택, 없어도 시스템 Python 사용) ──────────
echo -e "${BOLD}[4/4] Python embed (twikit, atproto 등 위해)${RESET}"
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
echo ""
echo -e "${YELLOW}다음 단계:${RESET}"
echo -e "  1. windows/miyo_cpp/ 폴더를 Windows 머신으로 복사"
echo -e "  2. Qt 6.7+ + CMake 3.20+ 설치"
echo -e "  3. build_windows.bat 실행"
echo -e "  4. dist/Chernobyl_win/Chernobyl.exe 동작 확인"
echo ""
