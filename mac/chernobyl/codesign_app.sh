#!/bin/bash
# Comprehensive codesign script for Predormition.app
# 컴포넌트를 inside-out 으로 서명한 뒤 --deep --strict 로 검증한다.
# ★ set -e 안 씀: 일부 nested(companion) 서명 실패는 허용하되,
#   메인 번들 서명/검증은 반드시 성공해야 하며 실패 시 즉시 비정상 종료(exit 1)한다.

APP="$1"
if [ -z "$APP" ] || [ ! -d "$APP" ]; then
    echo "Usage: $0 <path-to-app-bundle>"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENTITLEMENTS="${ENTITLEMENTS:-$SCRIPT_DIR/predormition.entitlements}"

# ─────────────────────────────────────────────────────────────────────────────
# 서명 ID 선택 (우선순위)
#   1) Developer ID Application — 배포+공증 가능(Gatekeeper 통과). 하드닝 런타임+entitlements 적용.
#   2) Apple Development (사용자 개발자 ID) — 로컬 안정 식별자 → TCC 권한 영구 유지.
#        이전 버전 Predormition 이 이걸로 서명됨. 평범 서명(하드닝 런타임 X) — 이전과 동일 동작.
#   3) ad-hoc(-) — 인증서가 하나도 없을 때. 매 빌드 해시가 바뀌어 권한이 재설정됨.
#   SIGN_ID 환경변수로 강제 지정 가능.
# ─────────────────────────────────────────────────────────────────────────────
if [ -z "$SIGN_ID" ]; then
    SIGN_ID=$(security find-identity -v -p codesigning 2>/dev/null \
        | grep -oE '"Developer ID Application:[^"]+"' | head -1 | tr -d '"')
    [ -z "$SIGN_ID" ] && SIGN_ID=$(security find-identity -v -p codesigning 2>/dev/null \
        | grep -oE '"Apple Development:[^"]+"' | head -1 | tr -d '"')
    [ -z "$SIGN_ID" ] && SIGN_ID="-"
fi
case "$SIGN_ID" in
    "Developer ID Application:"*) IS_DEVID=1; echo "[codesign] Using Developer ID: $SIGN_ID" ;;
    "-")  IS_DEVID=0; echo "[codesign] 서명 인증서 없음 → ad-hoc(-)." ;;
    *)    IS_DEVID=0; echo "[codesign] Using identity: $SIGN_ID (개발자 ID — 로컬 안정 식별자)" ;;
esac

# 하드닝 런타임+타임스탬프+entitlements 는 Developer ID(공증 경로)에서만.
#   Apple Development/ad-hoc 은 이전 버전처럼 평범하게 서명(하드닝 런타임 켜면 entitlements 없이 막힐 위험).
SIGN_FLAGS=(--force --sign "$SIGN_ID")
RUNTIME_FLAGS=()
if [ "$IS_DEVID" = "1" ]; then
    RUNTIME_FLAGS=(--options runtime --timestamp)
    [ -f "$ENTITLEMENTS" ] && RUNTIME_FLAGS+=(--entitlements "$ENTITLEMENTS")
fi

# 실행 코드/번들용(하드닝 런타임 포함). nested 라이브러리에도 적용해도 무해.
sign_rt() { codesign "${SIGN_FLAGS[@]}" "${RUNTIME_FLAGS[@]}" "$@"; }

# ★ 예전에는 모든 서명 호출이 `2>/dev/null || true` 였다. 어디서 실패했는지 알 길이
#   없었고, 마지막 --deep 검증만 "깨졌다" 고 말해 원인을 손으로 찾아야 했다
#   (실제로 파일 54개가 서명이 안 된 채 남아 있었는데 로그에는 아무것도 없었다).
#   이제는 세고, 처음 몇 건은 이유까지 보여 준다.
#   ★ 실패 개수를 변수에 세면 안 된다. 아래 서명 루프들은
#       find ... | while read ...   /   while read ... < <(...)
#     형태라 서브셸에서 돌고, 서브셸에서 올린 변수는 밖으로 나오지 않는다
#     (그래서 처음엔 늘 0 으로 나왔다). 파일에 적어야 밖에서 셀 수 있다.
SIGN_FAIL_LOG="$(mktemp -t predormition_sign)"
trap 'rm -f "$SIGN_FAIL_LOG"' EXIT
try_sign() {
    local out target="${*: -1}"
    if out="$(sign_rt "$@" 2>&1)"; then return 0; fi
    printf '%s\t%s\n' "$target" "$(echo "$out" | tail -1)" >> "$SIGN_FAIL_LOG"
    return 1
}

echo "[codesign] Signing $APP ..."

# 0. 기존 _CodeSignature 제거
find "$APP" -name "_CodeSignature" -type d -exec rm -rf {} + 2>/dev/null || true

# 1. companion app 안의 Python.framework 구조 정상화(있을 때만 — 현재 빌드엔 미포함)
find "$APP/Contents/Resources" -maxdepth 1 -name "*.app" -type d 2>/dev/null | while IFS= read -r capp; do
    for loc in "Contents/Resources" "Contents/Frameworks"; do
        fw="$capp/$loc/Python.framework"
        if [ -d "$fw/Versions/3.14" ]; then
            if [ -d "$fw/Versions/Current" ] && [ ! -L "$fw/Versions/Current" ]; then
                rm -rf "$fw/Versions/Current"; ln -sf 3.14 "$fw/Versions/Current"
            fi
            if [ -f "$fw/Python" ] && [ ! -L "$fw/Python" ]; then
                rm -f "$fw/Python"; ln -sf Versions/Current/Python "$fw/Python"
            fi
            if [ -d "$fw/Resources" ] && [ ! -L "$fw/Resources" ]; then
                rm -rf "$fw/Resources"; ln -sf Versions/Current/Resources "$fw/Resources"
            fi
        fi
    done
    find "$capp" -name "*.dist-info" -type d -exec rm -rf {} + 2>/dev/null || true
    find "$capp" -name "*.egg-info" -type d -exec rm -rf {} + 2>/dev/null || true
done

# 2. 모든 Mach-O 바이너리를 개별 서명(가장 안쪽 먼저)
find "$APP" -type f -print0 2>/dev/null | while IFS= read -r -d $'\0' f; do
    if file "$f" 2>/dev/null | grep -q "Mach-O"; then
        try_sign "$f" || true
    fi
done
echo "[codesign] All Mach-O binaries signed"

# 3. Qt 프레임워크
find "$APP/Contents/Frameworks" -maxdepth 1 -name "*.framework" -type d 2>/dev/null | while IFS= read -r fw; do
    try_sign "$fw" || true
done
echo "[codesign] Qt frameworks signed"

# 4. WebEngine 헬퍼(.app)
find "$APP" -name "QtWebEngineProcess.app" -type d 2>/dev/null | while IFS= read -r helper; do
    try_sign "$helper" || true
done
echo "[codesign] WebEngine helper signed"

# 5. Python 프레임워크
find "$APP" -path "*/Python.framework" -not -path "*/Versions/*" -type d 2>/dev/null | while IFS= read -r pfw; do
    try_sign "$pfw" || true
done

# 6. companion app(있을 때만 — 실패해도 진행)
find "$APP/Contents/Resources" -maxdepth 1 -name "*.app" -type d 2>/dev/null | while IFS= read -r capp; do
    try_sign "$capp" || true
    echo "[codesign] Signed companion: $(basename "$capp") (may have warnings)"
done

# 7. Contents/MacOS 의 실행 파일(yt-dlp/ffmpeg/ffprobe/main exe 등)
find "$APP/Contents/MacOS" -type f -print0 2>/dev/null | while IFS= read -r -d $'\0' f; do
    file "$f" 2>/dev/null | grep -q "Mach-O" && { try_sign "$f" || true; }
done
echo "[codesign] Main executables signed"

# 7.5 개별 서명 결과 보고 — 예전엔 전부 2>/dev/null 로 삼켜서, 마지막 --deep 검증이
#     "깨졌다" 고만 말했고 어디가 문제인지는 손으로 찾아야 했다(파일 54개가 서명이
#     안 된 채 남아 있었는데 로그에는 아무것도 없었다).
if [ -s "$SIGN_FAIL_LOG" ]; then
    nfail=$(wc -l < "$SIGN_FAIL_LOG" | tr -d ' ')
    echo "[codesign] ⚠ 개별 서명 실패 ${nfail} 건 — 아래 검증에서 걸릴 수 있습니다:"
    head -5 "$SIGN_FAIL_LOG" | while IFS=$'\t' read -r f why; do
        echo "[codesign]    ${f#$APP/}"
        echo "[codesign]      $why"
    done
    [ "$nfail" -gt 5 ] && echo "[codesign]    … 외 $((nfail - 5)) 건"
else
    echo "[codesign] 개별 서명 실패 없음"
fi

# 8. ★ 메인 번들 — 반드시 성공해야 함(실패 시 즉시 중단).
#    일본어/유니코드 앱 이름의 NFC/NFD 충돌 회피: 부모 dir 로 cd 후 상대 경로 사용.
rm -rf "$APP/Contents/_CodeSignature" 2>/dev/null || true
APP_NAME=$(basename "$APP")
APP_PARENT=$(dirname "$APP")
if ! ( cd "$APP_PARENT" && codesign "${SIGN_FLAGS[@]}" "${RUNTIME_FLAGS[@]}" "$APP_NAME" ); then
    echo "[codesign] ❌ 메인 번들 서명 실패 — 중단."
    exit 1
fi
echo "[codesign] Main bundle signed"

# 9. quarantine 제거(로컬 실행 편의 — 다운로드본엔 영향 없음)
xattr -cr "$APP" 2>/dev/null || true
echo "[codesign] Quarantine removed"

# 10. ★ 엄격 검증 — nested 까지(--deep --strict). 실패 시 비정상 종료(거짓 'OK' 방지).
echo "[codesign] Verifying (--deep --strict) ..."
if ! codesign --verify --deep --strict --verbose=2 "$APP"; then
    echo "[codesign] ❌ VERIFY FAILED — 서명이 깨졌습니다(이대로면 실행 시 SIGKILL 위험)."
    exit 1
fi
echo "[codesign] ✅ VERIFIED OK"

# Developer ID 서명일 때만 Gatekeeper 평가 미리보기(공증 전이면 reject 가 정상 — 정보용).
if [ "$SIGN_ID" != "-" ]; then
    echo "[codesign] Gatekeeper 평가(spctl):"
    spctl -a -vv --type execute "$APP" 2>&1 || true
    echo "[codesign] 배포하려면: notarytool submit + stapler staple (README 참고)."
fi
