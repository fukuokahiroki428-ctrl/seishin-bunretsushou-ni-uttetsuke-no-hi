#!/bin/bash
# Comprehensive codesign script for Pen.app
# 컴포넌트를 inside-out 으로 서명한 뒤 --deep --strict 로 검증한다.
# ★ set -e 안 씀: 일부 nested 서명 실패는 허용하되, 메인 번들 서명/검증은 반드시 성공해야 함(실패 시 exit 1).

APP="$1"
if [ -z "$APP" ] || [ ! -d "$APP" ]; then
    echo "Usage: $0 <path-to-app-bundle>"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENTITLEMENTS="${ENTITLEMENTS:-$SCRIPT_DIR/pen.entitlements}"

# ─────────────────────────────────────────────────────────────────────────────
# 서명 ID 선택: 1) Developer ID Application(배포+공증) 2) Apple Development(사용자 개발자 ID —
#   로컬 안정 식별자, 이전 버전과 동일) 3) ad-hoc(-). SIGN_ID 로 강제 지정 가능.
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
    *)    IS_DEVID=0; echo "[codesign] Using identity: $SIGN_ID (개발자 ID)" ;;
esac

# 하드닝 런타임+entitlements 는 Developer ID(공증 경로)에서만. Apple Development/ad-hoc 은 평범 서명.
SIGN_FLAGS=(--force --sign "$SIGN_ID")
RUNTIME_FLAGS=()
if [ "$IS_DEVID" = "1" ]; then
    RUNTIME_FLAGS=(--options runtime --timestamp)
    [ -f "$ENTITLEMENTS" ] && RUNTIME_FLAGS+=(--entitlements "$ENTITLEMENTS")
fi
sign_rt() { codesign "${SIGN_FLAGS[@]}" "${RUNTIME_FLAGS[@]}" "$@"; }

echo "[codesign] Signing $APP ..."

# 1. 기존 _CodeSignature 제거
find "$APP" -name "_CodeSignature" -type d -exec rm -rf {} + 2>/dev/null || true

# 2. 모든 dylib/so 개별 서명 (install_name_tool 로 무효화된 서명 복구)
find "$APP" \( -name "*.dylib" -o -name "*.so" \) -type f -print0 \
    | while IFS= read -r -d $'\0' lib; do sign_rt "$lib" 2>/dev/null || true; done
echo "[codesign] All dylibs/so signed"

# 3. Qt 프레임워크
find "$APP/Contents/Frameworks" -maxdepth 1 -name "*.framework" -type d 2>/dev/null | while IFS= read -r fw; do
    sign_rt "$fw" 2>/dev/null || true
done
echo "[codesign] Qt frameworks signed"

# 4. WebEngine 헬퍼
find "$APP" -name "QtWebEngineProcess.app" -type d 2>/dev/null | while IFS= read -r helper; do
    sign_rt "$helper" 2>/dev/null || true
done
echo "[codesign] WebEngine helper signed"

# 5. companion app(있을 때만 — 실패해도 진행)
find "$APP/Contents/Resources" -maxdepth 1 -name "*.app" -type d 2>/dev/null | while IFS= read -r capp; do
    find "$capp/Contents/MacOS" -type f -perm +111 -print0 2>/dev/null \
        | while IFS= read -r -d $'\0' ex; do sign_rt "$ex" 2>/dev/null || true; done
    sign_rt "$capp" 2>/dev/null || true
    echo "[codesign] Signed companion: $(basename "$capp")"
done

# 6. Contents/MacOS 실행 파일
find "$APP/Contents/MacOS" -type f -perm +111 -print0 2>/dev/null \
    | while IFS= read -r -d $'\0' f; do sign_rt "$f" 2>/dev/null || true; done
echo "[codesign] Main executables signed"

# 7. ★ 메인 번들 — 반드시 성공(실패 시 중단). NFC/NFD 충돌 회피: cd 후 상대 경로.
APP_NAME=$(basename "$APP")
APP_PARENT=$(dirname "$APP")
if ! ( cd "$APP_PARENT" && codesign "${SIGN_FLAGS[@]}" "${RUNTIME_FLAGS[@]}" "$APP_NAME" ); then
    echo "[codesign] ❌ 메인 번들 서명 실패 — 중단."
    exit 1
fi
echo "[codesign] App bundle signed"

# 8. quarantine 제거
xattr -cr "$APP" 2>/dev/null || true

# 9. ★ 엄격 검증 — 실패 시 비정상 종료(거짓 'OK' 방지).
echo "[codesign] Verifying (--deep --strict) ..."
if ! codesign --verify --deep --strict --verbose=2 "$APP"; then
    echo "[codesign] ❌ VERIFY FAILED — 서명이 깨졌습니다(실행 시 SIGKILL 위험)."
    exit 1
fi
echo "[codesign] ✅ VERIFIED OK"
