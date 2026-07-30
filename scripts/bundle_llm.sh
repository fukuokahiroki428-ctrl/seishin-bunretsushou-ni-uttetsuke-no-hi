#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════
# bundle_llm.sh — 로컬 LLM(llama-server + GGUF 모델)을 앱 "내부"에 배치한다.
#
# SelfRepair(src/utils/SelfRepair.h) 는 앱 시작 시 <Resources>/llm/ 에
# llama-server + *.gguf 가 있으면 자동으로 기동(포트 8737)해 자가진단
# 보고서의 원인 분석에 사용한다. 이 스크립트는 그 폴더를 채운다.
#
# 사용법:
#   mac:     bash scripts/bundle_llm.sh "path/to/Predormition.app"
#   windows: bash scripts/bundle_llm.sh "path/to/배포폴더"   (git-bash/WSL)
#
# 환경변수로 교체 가능:
#   LLM_MODEL_URL  — 기본: Qwen2.5-1.5B-Instruct Q4_K_M (~1.0GB, 한국어 양호)
#   LLAMA_ZIP_URL  — 기본: ggml-org/llama.cpp 최신 릴리스에서 플랫폼별 자동 선택
#
# 주의: 모델은 GB 단위라 git 에 커밋하지 않는다(GitHub 100MB 제한).
#       배포 패키징 단계에서 이 스크립트를 1회 실행하는 방식이다.
# ═══════════════════════════════════════════════════════════════════════════
set -euo pipefail

TARGET="${1:?사용법: bash scripts/bundle_llm.sh <App.app 또는 배포폴더>}"

# 기본 모델 세트 — 작은 것(빠른 응답)부터 코드 특화(수리 품질)까지.
#   LLM_MODEL_URL 하나만 주면 그것만, LLM_MODEL_URLS(공백/줄바꿈 구분)로 여러 개 지정 가능.
HF=https://huggingface.co/Qwen
DEFAULT_MODELS="\
$HF/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/qwen2.5-1.5b-instruct-q4_k_m.gguf \
$HF/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf \
$HF/Qwen2.5-Coder-3B-Instruct-GGUF/resolve/main/qwen2.5-coder-3b-instruct-q4_k_m.gguf \
$HF/Qwen2.5-Coder-7B-Instruct-GGUF/resolve/main/qwen2.5-coder-7b-instruct-q4_k_m-00001-of-00002.gguf \
$HF/Qwen2.5-Coder-7B-Instruct-GGUF/resolve/main/qwen2.5-coder-7b-instruct-q4_k_m-00002-of-00002.gguf"
MODEL_URLS="${LLM_MODEL_URLS:-${LLM_MODEL_URL:-$DEFAULT_MODELS}}"

# ── 대상 llm 폴더 결정 ─────────────────────────────────────────────────────
if [[ "$TARGET" == *.app ]]; then
    PLAT="mac"
    LLM_DIR="$TARGET/Contents/Resources/llm"
else
    PLAT="win"
    LLM_DIR="$TARGET/llm"
fi
mkdir -p "$LLM_DIR"
echo "▶ 대상: $LLM_DIR ($PLAT)"

# ── llama-server 바이너리 ──────────────────────────────────────────────────
if [ -z "${LLAMA_ZIP_URL:-}" ]; then
    if [ "$PLAT" = "mac" ]; then
        PAT="macos-arm64"
        [ "$(uname -m)" = "x86_64" ] && PAT="macos-x64"
    else
        PAT="win-avx2-x64"
    fi
    echo "▶ llama.cpp 최신 릴리스에서 '$PAT' 자산 검색..."
    LLAMA_ZIP_URL=$(curl -fsSL https://api.github.com/repos/ggml-org/llama.cpp/releases/latest \
        | grep -o '"browser_download_url": *"[^"]*"' | grep "$PAT" | head -1 \
        | sed 's/.*"\(https[^"]*\)"/\1/')
    [ -n "$LLAMA_ZIP_URL" ] || { echo "❌ '$PAT' 자산을 찾지 못함 — LLAMA_ZIP_URL 로 직접 지정하세요"; exit 1; }
fi

SRV="$LLM_DIR/llama-server"; [ "$PLAT" = "win" ] && SRV="$SRV.exe"
if [ ! -f "$SRV" ]; then
    echo "▶ 다운로드: $LLAMA_ZIP_URL"
    TMP=$(mktemp -d)
    ARCHIVE="$TMP/llama_archive"
    curl -fL --retry 3 -o "$ARCHIVE" "$LLAMA_ZIP_URL"
    mkdir -p "$TMP/llama"
    # ★ 릴리스 자산: mac/linux=.tar.gz, windows=.zip → 확장자로 분기 (unzip 이 tar.gz 못 풀던 버그 수정)
    case "$LLAMA_ZIP_URL" in
        *.tar.gz|*.tgz) tar -xzf "$ARCHIVE" -C "$TMP/llama" ;;
        *.zip)          unzip -oq "$ARCHIVE" -d "$TMP/llama" ;;
        *)              tar -xzf "$ARCHIVE" -C "$TMP/llama" 2>/dev/null || unzip -oq "$ARCHIVE" -d "$TMP/llama" ;;
    esac
    # 구조가 릴리스마다 다름 — llama-server 실행파일과 동반 dylib/dll 을 찾아 복사
    FOUND=$(find "$TMP/llama" -name "llama-server*" -type f | head -1)
    [ -n "$FOUND" ] || { echo "❌ 아카이브 안에 llama-server 없음"; exit 1; }
    cp "$FOUND" "$SRV"
    find "$(dirname "$FOUND")" -maxdepth 1 \( -name "*.dylib" -o -name "*.dll" -o -name "*.metal" \) \
        -exec cp {} "$LLM_DIR/" \; 2>/dev/null || true
    chmod +x "$SRV"
    rm -rf "$TMP"
else
    echo "✔ llama-server 이미 존재 — 건너뜀"
fi

# ── 모델(여러 개) ─────────────────────────────────────────────────────────
for MODEL_URL in $MODEL_URLS; do
    MODEL_FILE="$LLM_DIR/$(basename "$MODEL_URL")"
    if [ ! -f "$MODEL_FILE" ]; then
        echo "▶ 모델 다운로드: $(basename "$MODEL_URL")"
        curl -fL --retry 3 -C - -o "$MODEL_FILE" "$MODEL_URL"
    else
        echo "✔ $(basename "$MODEL_URL") 이미 존재 — 건너뜀"
    fi
done

# ── 검증 ──────────────────────────────────────────────────────────────────
if [ "$PLAT" = "mac" ]; then
    "$SRV" --version >/dev/null 2>&1 && echo "✔ llama-server 실행 확인" \
        || echo "⚠ llama-server 실행 실패 — 아키텍처/서명 확인 필요 (앱은 LLM 없이도 정상 동작)"
fi
# ── mac 재서명 ────────────────────────────────────────────────────────────
#   앱 번들 안에 파일을 넣으면 codesign 봉인이 깨져 macOS 가 앱을 죽일 수 있다 → 즉시 재서명.
if [ "$PLAT" = "mac" ]; then
    echo "▶ 앱 재서명(번들에 모델을 넣었으므로 봉인 복구)..."
    SIGN_ID=$(security find-identity -v -p codesigning 2>/dev/null | grep -m1 -oE "[0-9A-F]{40}" || true)
    [ -z "$SIGN_ID" ] && SIGN_ID="-"
    codesign -f -s "$SIGN_ID" --deep "$TARGET" 2>/dev/null \
        && codesign --verify --deep --strict "$TARGET" 2>/dev/null \
        && echo "✔ 재서명·검증 통과" \
        || echo "⚠ 재서명 실패 — 앱이 실행 안 되면 다시 빌드하세요"
fi

echo ""
echo "✅ 완료. 앱 시작 시 SelfRepair 가 자동 감지·기동한다 (포트 8737)."
echo "   확인: 앱 실행 후 AppData(Application Support)/…/selfrepair/last_report.txt"
