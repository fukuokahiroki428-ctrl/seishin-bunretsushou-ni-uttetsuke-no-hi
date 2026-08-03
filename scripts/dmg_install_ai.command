#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════
# AI 모델 설치 — DMG 안에 넣어 배포하는 사용자용 실행 파일(더블클릭).
#
#   배포본(DMG)에는 앱만 들어간다. 로컬 AI 모델은 1~9GB 라 GitHub 릴리즈
#   파일당 2GB 제한에 걸려 함께 넣을 수 없다. 이 파일이 모델을 받아
#   설치된 앱 "내부"(Predormition.app/Contents/Resources/llm)에 합쳐 넣는다.
#
#   - 모델은 공식 배포처(Hugging Face)에서 직접 받는다 → 용량 제한 없음.
#   - llama-server(추론 엔진)도 없으면 함께 설치.
#   - 앱 번들 안에 파일을 넣으면 codesign 봉인이 깨지므로 끝에 자동 재서명.
#   - 중간에 끊겨도 다시 실행하면 이어받는다(이미 받은 건 건너뜀).
# ═══════════════════════════════════════════════════════════════════════════
set -uo pipefail

APP_NAME="Predormition"
HF="https://huggingface.co/Qwen"

echo ""
echo "════════════════════════════════════════════"
echo "  $APP_NAME — 로컬 AI(오픈클로) 설치"
echo "════════════════════════════════════════════"
echo ""

# ── 1) 앱 찾기 ─────────────────────────────────────────────────────────────
APP=""
for c in "/Applications/$APP_NAME.app" "$HOME/Applications/$APP_NAME.app" \
         "$(cd "$(dirname "$0")" && pwd)/$APP_NAME.app"; do
    [ -d "$c" ] && { APP="$c"; break; }
done
if [ -z "$APP" ]; then
    echo "먼저 $APP_NAME.app 을 Applications 폴더로 끌어넣어 설치해 주세요."
    echo "설치한 뒤 이 파일을 다시 더블클릭하면 됩니다."
    echo ""
    read -n 1 -s -r -p "아무 키나 누르면 닫힙니다..."
    exit 1
fi
echo "앱 위치: $APP"

# 설치 위치에 쓸 수 있는지 (관리자 권한 없이 되는지) 확인
LLM_DIR="$APP/Contents/Resources/llm"
if ! mkdir -p "$LLM_DIR" 2>/dev/null; then
    echo ""
    echo "앱 폴더에 쓸 권한이 없습니다. $APP_NAME.app 을 사용자 폴더"
    echo "(~/Applications)로 옮긴 뒤 다시 실행해 주세요."
    echo ""
    read -n 1 -s -r -p "아무 키나 누르면 닫힙니다..."
    exit 1
fi

# ── 2) 무엇을 받을지 고르기 ────────────────────────────────────────────────
echo ""
echo "설치할 AI 를 고르세요. (숫자 입력 후 Enter)"
echo ""
echo "  1) 기본        1.0GB  — 대화용. 가볍고 빠름."
echo "  2) 코드 수리   3.0GB  — 기본 + 코드 자가수리용(권장)."
echo "  3) 전체        9.3GB  — 위 전부 + 최고 품질 7B."
echo ""
read -r -p "선택 [2]: " CHOICE
CHOICE="${CHOICE:-2}"

# ★ 1순위 = 이 프로젝트의 보관 릴리즈(ai-assets-v1).
#   허깅페이스나 원 배포처가 사라져도 설치할 수 있도록 모델 원본을 직접 보관해 둔다.
#   2GB 넘는 모델은 .partaa/.partab… 로 나눠 올려두고, 받은 뒤 합쳐서 쓴다.
MIRROR="https://github.com/fukuokahiroki428-ctrl/seishin-bunretsushou-ni-uttetsuke-no-hi/releases/download/ai-assets-v1"

M15="$HF/Qwen2.5-1.5B-Instruct-GGUF/resolve/main/qwen2.5-1.5b-instruct-q4_k_m.gguf"
M3="$HF/Qwen2.5-3B-Instruct-GGUF/resolve/main/qwen2.5-3b-instruct-q4_k_m.gguf"
MC3="$HF/Qwen2.5-Coder-3B-Instruct-GGUF/resolve/main/qwen2.5-coder-3b-instruct-q4_k_m.gguf"
MC7A="$HF/Qwen2.5-Coder-7B-Instruct-GGUF/resolve/main/qwen2.5-coder-7b-instruct-q4_k_m-00001-of-00002.gguf"
MC7B="$HF/Qwen2.5-Coder-7B-Instruct-GGUF/resolve/main/qwen2.5-coder-7b-instruct-q4_k_m-00002-of-00002.gguf"

case "$CHOICE" in
    1) MODELS="$M15" ;;
    3) MODELS="$M15 $M3 $MC3 $MC7A $MC7B" ;;
    *) MODELS="$M15 $MC3" ;;
esac

# ── 3) llama-server(추론 엔진) ─────────────────────────────────────────────
SRV="$LLM_DIR/llama-server"
if [ ! -x "$SRV" ]; then
    echo ""
    echo "▶ 추론 엔진(llama-server) 설치 중..."
    case "$(uname -m)" in
        arm64) PAT="macos-arm64" ;;
        *)     PAT="macos-x64"   ;;
    esac
    ZIP_URL=$(curl -fsSL https://api.github.com/repos/ggml-org/llama.cpp/releases/latest \
        | grep -o '"browser_download_url": *"[^"]*"' | grep "$PAT" | head -1 \
        | sed 's/.*": *"//; s/"$//')
    if [ -z "$ZIP_URL" ]; then
        echo "  ✗ 엔진 다운로드 주소를 찾지 못했습니다. 인터넷 연결을 확인해 주세요."
        read -n 1 -s -r -p "아무 키나 누르면 닫힙니다..."; exit 1
    fi
    TMP=$(mktemp -d)
    ARCHIVE="$TMP/llama_archive"
    OK=0
    if curl -fL --retry 3 -o "$ARCHIVE" "$ZIP_URL"; then
        mkdir -p "$TMP/x"
        # ★ mac/linux 자산은 .tar.gz, windows 는 .zip — 확장자로 분기(unzip 으로 tar.gz 를 풀면 실패).
        case "$ZIP_URL" in
            *.tar.gz|*.tgz) tar -xzf "$ARCHIVE" -C "$TMP/x" 2>/dev/null && OK=1 ;;
            *.zip)          unzip -oq "$ARCHIVE" -d "$TMP/x" 2>/dev/null && OK=1 ;;
            *)              tar -xzf "$ARCHIVE" -C "$TMP/x" 2>/dev/null && OK=1 || \
                            { unzip -oq "$ARCHIVE" -d "$TMP/x" 2>/dev/null && OK=1; } ;;
        esac
    fi
    if [ "$OK" = "1" ]; then
        find "$TMP/x" -name "llama-server" -type f -exec cp {} "$LLM_DIR/" \; 2>/dev/null
        find "$TMP/x" -name "*.dylib" -exec cp {} "$LLM_DIR/" \; 2>/dev/null
        chmod +x "$SRV" 2>/dev/null
        if [ -x "$SRV" ]; then echo "  ✔ 엔진 설치 완료"; else echo "  ✗ 엔진 파일을 찾지 못했습니다"; fi
    else
        echo "  ✗ 엔진 설치 실패 (인터넷 연결을 확인해 주세요)"
        FAIL_ENGINE=1
    fi
    rm -rf "$TMP"
else
    echo "▶ 추론 엔진 이미 설치됨 — 건너뜀"
fi

# ── 4) 모델 받기 (이어받기 지원) ───────────────────────────────────────────
FAIL=${FAIL_ENGINE:-0}
for URL in $MODELS; do
    NAME=$(basename "$URL")
    OUT="$LLM_DIR/$NAME"
    if [ -f "$OUT" ]; then
        echo "▶ $NAME — 이미 있음, 건너뜀"
        continue
    fi
    echo ""
    echo "▶ 다운로드: $NAME"

    # ★ 받는 동안에는 항상 "$OUT.part" 에 쓰고, 다 받은 뒤에만 최종 이름으로 옮긴다.
    #   예전에는 최종 이름에 바로 받아서 두 가지 문제가 있었다:
    #     - 창을 닫거나 Ctrl-C 로 끊으면 '잘린 파일' 이 최종 이름으로 남고, 다음 실행에서
    #       "이미 있음, 건너뜀" 으로 처리돼 손상된 모델이 설치 완료로 보고됐다.
    #     - 실패하면 부분 파일을 지워버려서, 안내문과 달리 이어받기가 실제로는 안 됐다.
    # ★ 받는 곳마다 임시 파일을 따로 쓴다. 예전엔 셋이 같은 .part 를 공유해서,
    #   보관본 시도가 실패하며 그 파일을 지울 때 원 배포처에서 받다 만 것까지 같이
    #   지워졌다 → 안내와 달리 폴백 경로는 매번 0부터 다시 받았다.
    MIR_OUT="$OUT.mirror.part"
    MRG_OUT="$OUT.merge.part"
    TMP_OUT="$OUT.part"

    # ① 보관 릴리즈에서 통째로 (-C - 로 이어받기)
    if curl -fL --retry 3 -C - --progress-bar -o "$MIR_OUT" "$MIRROR/$NAME"; then
        mv -f "$MIR_OUT" "$OUT"
        echo "  ✔ 완료(보관본)"
        continue
    fi

    # ② 보관 릴리즈의 분할본 (2GB 넘는 모델은 .partaa/.partab… 로 올려둠)
    #   ★ 경로에 공백이 있어도 안전하도록 배열로 모은다(예전엔 따옴표 없는 $PARTS 라
    #     공백 있는 경로에서 병합이 통째로 실패했다).
    PARTS=(); OK=1
    for SFX in aa ab ac ad ae af; do
        if curl -fsIL "$MIRROR/$NAME.part$SFX" >/dev/null 2>&1; then
            echo "   조각 part$SFX 받는 중..."
            if curl -fL --retry 3 -C - --progress-bar -o "$OUT.chunk$SFX" "$MIRROR/$NAME.part$SFX"; then
                PARTS+=("$OUT.chunk$SFX")
            else OK=0; break; fi
        else
            break
        fi
    done
    if [ "${#PARTS[@]}" -gt 0 ] && [ "$OK" = "1" ]; then
        if cat "${PARTS[@]}" > "$MRG_OUT"; then
            rm -f "${PARTS[@]}"
            mv -f "$MRG_OUT" "$OUT"
            echo "  ✔ 완료(보관본 조각 합침)"
            continue
        fi
    fi
    [ "${#PARTS[@]}" -gt 0 ] && rm -f "${PARTS[@]}"
    # ★ 여기서 지우는 건 이 경로가 만든 것들뿐 — $TMP_OUT(원 배포처 이어받기용)은 건드리지 않는다.
    rm -f "$MIR_OUT" "$MRG_OUT" 2>/dev/null

    # ③ 원 배포처(Hugging Face)
    echo "   보관본이 없어 원 배포처에서 받습니다..."
    if curl -fL --retry 3 -C - --progress-bar -o "$TMP_OUT" "$URL"; then
        mv -f "$TMP_OUT" "$OUT"
        echo "  ✔ 완료"
    else
        # ★ 부분 파일(.part)은 남겨둔다 — 다시 실행하면 -C - 가 이어받는다.
        echo "  ✗ 실패 — 다시 실행하면 받다 만 지점부터 이어받습니다"
        FAIL=1
    fi
done

# ── 5) 재서명 (번들에 파일을 넣었으므로 봉인 복구) ─────────────────────────
echo ""
echo "▶ 앱 서명 복구 중... (용량이 커서 수 분 걸릴 수 있습니다)"
SIGN_ID=$(security find-identity -v -p codesigning 2>/dev/null | grep -m1 -oE "[0-9A-F]{40}" || true)
[ -z "$SIGN_ID" ] && SIGN_ID="-"
find "$APP/Contents/Resources" -name "__pycache__" -type d -exec rm -rf {} + 2>/dev/null
if codesign -f -s "$SIGN_ID" --deep "$APP" 2>/dev/null && \
   codesign --verify --deep --strict "$APP" 2>/dev/null; then
    echo "  ✔ 서명 복구 완료"
else
    echo "  ⚠ 서명 복구에 실패했습니다. 앱이 실행되지 않으면 다시 내려받아 설치해 주세요."
fi

echo ""
echo "════════════════════════════════════════════"
if [ "$FAIL" = "0" ]; then
    echo "  ✅ 설치 완료!"
    echo "  앱을 열고 설정 → 로컬 AI 에서 'AI 켜기' 를 누르세요."
    echo "  대화창에 '코드 고쳐줘' 라고 하면 스스로 점검·수리합니다."
else
    echo "  ⚠ 일부 파일을 받지 못했습니다."
    echo "  이 파일을 다시 더블클릭하면 못 받은 것만 이어받습니다."
fi
echo "════════════════════════════════════════════"
echo ""
read -n 1 -s -r -p "아무 키나 누르면 닫힙니다..."
echo ""
