#!/bin/bash
# Comprehensive codesign script for 見よ.app
# Signs all components from innermost to outermost
# ★ set -e 제거 — fail-tolerant (companion app sign 실패 시에도 main exe sign 끝까지)

APP="$1"
if [ -z "$APP" ] || [ ! -d "$APP" ]; then
    echo "Usage: $0 <path-to-app-bundle>"
    exit 1
fi

# ★ 서명 ID 자동 감지 — Apple Development 인증서가 있으면 그걸로, 없으면 ad-hoc
#   ad-hoc(-)는 매 빌드마다 해시 바뀌어서 macOS가 매번 새 앱으로 취급 → 권한 매번 재설정.
#   본인 Apple Development 인증서로 서명하면 ID 안정 → 권한 영구 유지.
if [ -z "$SIGN_ID" ]; then
    SIGN_ID=$(security find-identity -v -p codesigning 2>/dev/null \
        | grep -oE '"Apple Development:[^"]+"' | head -1 | tr -d '"')
    if [ -z "$SIGN_ID" ]; then
        SIGN_ID="-"
        echo "[codesign] No Apple Development cert found, using ad-hoc (-)"
    else
        echo "[codesign] Using identity: $SIGN_ID"
    fi
fi

echo "[codesign] Signing $APP ..."

# 1. Remove all existing _CodeSignature directories
find "$APP" -name "_CodeSignature" -type d -exec rm -rf {} + 2>/dev/null || true

# 2. Sign ALL Mach-O files (dylib, so, executables) individually
# This handles install_name_tool-invalidated signatures
find "$APP" \( -name "*.dylib" -o -name "*.so" \) -type f -print0 | xargs -0 -P 8 -I {} codesign --force --sign "$SIGN_ID" {} 2>/dev/null || true
echo "[codesign] All dylibs/so signed"

# 3. Sign Qt frameworks (inner binary + framework bundle)
find "$APP/Contents/Frameworks" -maxdepth 1 -name "*.framework" -type d | while IFS= read -r fw; do
    codesign --force --sign "$SIGN_ID" "$fw" 2>/dev/null || true
done
echo "[codesign] Qt frameworks signed"

# 4. Sign QtWebEngineProcess helper app
find "$APP" -name "QtWebEngineProcess.app" -type d | while IFS= read -r helper; do
    codesign --force --sign "$SIGN_ID" "$helper" 2>/dev/null || true
done
echo "[codesign] WebEngine helper signed"

# 5. Sign companion apps (anipo.app, anipo-2.app, AINU.app)
find "$APP/Contents/Resources" -maxdepth 1 -name "*.app" -type d | while IFS= read -r capp; do
    # Sign companion executables
    find "$capp/Contents/MacOS" -type f -perm +111 -print0 2>/dev/null | xargs -0 -I {} codesign --force --sign "$SIGN_ID" {} 2>/dev/null || true
    # Sign the companion app bundle itself
    codesign --force --sign "$SIGN_ID" "$capp" 2>/dev/null || true
    echo "[codesign] Signed companion: $(basename "$capp")"
done

# 6. Sign main executables
find "$APP/Contents/MacOS" -type f -perm +111 -print0 | xargs -0 -I {} codesign --force --sign "$SIGN_ID" {} 2>/dev/null || true
echo "[codesign] Main executables signed"

# 7. Final: sign the main app bundle (NFC/NFD 충돌 회피 — cd 후 상대 경로)
APP_NAME=$(basename "$APP")
APP_PARENT=$(dirname "$APP")
(cd "$APP_PARENT" && codesign --force --sign "$SIGN_ID" "$APP_NAME") 2>&1 \
    || echo "[codesign] Main bundle sign warning (main exe signed — launch OK)"
echo "[codesign] App bundle signed"

# 7.5. Main exe deterministic last-write
APP_BASENAME=$(basename "$APP" .app)
(cd "$APP/Contents/MacOS" && [ -f "$APP_BASENAME" ] && codesign --force --sign "$SIGN_ID" "./$APP_BASENAME") 2>/dev/null || true

# 8. xattr 정리 (quarantine 제거 — Finder 더블클릭 시 macOS 차단 방지)
xattr -cr "$APP" 2>/dev/null || true

# 9. Verify
if codesign --verify --verbose=1 "$APP" 2>&1; then
    echo "[codesign] VERIFIED OK"
else
    echo "[codesign] WARNING: Verification has issues (may still run fine — quarantine removed)"
fi
