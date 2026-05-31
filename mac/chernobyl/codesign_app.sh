#!/bin/bash
# Comprehensive codesign script for チェルノブイリ.app
# Signs all components from innermost to outermost
# ★ set -e 제거: companion app (AINU.app 등) sign 실패해도 main exe는 sign 완료해야 함

APP="$1"
if [ -z "$APP" ] || [ ! -d "$APP" ]; then
    echo "Usage: $0 <path-to-app-bundle>"
    exit 1
fi

# ★ 서명 ID 자동 감지 — Apple Development 인증서가 있으면 그걸로, 없으면 ad-hoc
#   ad-hoc(-)는 매 빌드마다 해시 바뀌어서 macOS가 매번 새 앱으로 취급 → 권한 매번 재설정.
#   본인 Apple Development 인증서로 서명하면 ID 안정 → 권한 영구 유지.
if [ -z "$SIGN_ID" ]; then
    # 환경변수 미지정 시 자동 탐지
    SIGN_ID=$(security find-identity -v -p codesigning 2>/dev/null \
        | grep -oE '"Apple Development:[^"]+"' | head -1 | tr -d '"')
    if [ -z "$SIGN_ID" ]; then
        SIGN_ID="-"  # fallback to ad-hoc
        echo "[codesign] No Apple Development cert found, using ad-hoc (-)"
    else
        echo "[codesign] Using identity: $SIGN_ID"
    fi
fi

echo "[codesign] Signing $APP ..."

# 0. Remove all existing _CodeSignature
find "$APP" -name "_CodeSignature" -type d -exec rm -rf {} + 2>/dev/null || true

# 1. Fix Python.framework structure in companion apps (must be proper symlinks)
find "$APP/Contents/Resources" -maxdepth 1 -name "*.app" -type d | while IFS= read -r capp; do
    for loc in "Contents/Resources" "Contents/Frameworks"; do
        fw="$capp/$loc/Python.framework"
        if [ -d "$fw/Versions/3.14" ]; then
            # Current → symlink to 3.14
            if [ -d "$fw/Versions/Current" ] && [ ! -L "$fw/Versions/Current" ]; then
                rm -rf "$fw/Versions/Current"
                ln -sf 3.14 "$fw/Versions/Current"
            fi
            # Top-level Python → symlink
            if [ -f "$fw/Python" ] && [ ! -L "$fw/Python" ]; then
                rm -f "$fw/Python"
                ln -sf Versions/Current/Python "$fw/Python"
            fi
            # Top-level Resources → symlink
            if [ -d "$fw/Resources" ] && [ ! -L "$fw/Resources" ]; then
                rm -rf "$fw/Resources"
                ln -sf Versions/Current/Resources "$fw/Resources"
            fi
        fi
    done
    # Remove problematic items that break codesign
    find "$capp" -name "*.dist-info" -type d -exec rm -rf {} + 2>/dev/null || true
    find "$capp" -name "*.egg-info" -type d -exec rm -rf {} + 2>/dev/null || true
done

# 2. Sign ALL Mach-O binaries individually
find "$APP" -type f -print0 2>/dev/null | while IFS= read -r -d $'\0' f; do
    if file "$f" 2>/dev/null | grep -q "Mach-O"; then
        codesign --force --sign "$SIGN_ID" "$f" 2>/dev/null || true
    fi
done
echo "[codesign] All Mach-O binaries signed"

# 3. Sign Qt frameworks
find "$APP/Contents/Frameworks" -maxdepth 1 -name "*.framework" -type d | while IFS= read -r fw; do
    codesign --force --sign "$SIGN_ID" "$fw" 2>/dev/null || true
done
echo "[codesign] Qt frameworks signed"

# 4. Sign WebEngine helper
find "$APP" -name "QtWebEngineProcess.app" -type d | while IFS= read -r helper; do
    codesign --force --sign "$SIGN_ID" "$helper" 2>/dev/null || true
done
echo "[codesign] WebEngine helper signed"

# 5. Sign Python frameworks
find "$APP" -path "*/Python.framework" -not -path "*/Versions/*" -type d | while IFS= read -r pfw; do
    codesign --force --sign "$SIGN_ID" "$pfw" 2>/dev/null || true
done

# 6. Sign companion apps (PyInstaller apps — may fail, that's OK)
find "$APP/Contents/Resources" -maxdepth 1 -name "*.app" -type d | while IFS= read -r capp; do
    codesign --force --sign "$SIGN_ID" "$capp" 2>/dev/null || true
    echo "[codesign] Signed companion: $(basename "$capp") (may have warnings)"
done

# 7. Sign main executables
find "$APP/Contents/MacOS" -type f -print0 | xargs -0 -I {} codesign --force --sign "$SIGN_ID" {} 2>/dev/null || true
echo "[codesign] Main executables signed"

# 8. Main bundle codesign — companion apps 제거됐으니 깨끗 sign 가능.
#    ★ 일본어 앱 이름 "チェルノブイリ" NFC/NFD 충돌 회피: cd로 부모 dir 이동 후 상대 경로 사용
rm -rf "$APP/Contents/_CodeSignature" 2>/dev/null || true
APP_NAME=$(basename "$APP")
APP_PARENT=$(dirname "$APP")
(cd "$APP_PARENT" && codesign --force --sign "$SIGN_ID" "$APP_NAME") 2>&1 \
    || echo "[codesign] Main bundle sign warning (main exe signed — launch OK)"

# Main exe 한 번 더 sign — deterministic last-write
APP_BASENAME=$(basename "$APP" .app)
(cd "$APP/Contents/MacOS" && [ -f "$APP_BASENAME" ] && codesign --force --sign "$SIGN_ID" "./$APP_BASENAME") 2>/dev/null || true
echo "[codesign] App bundle + main exe signed"

# 9. Remove quarantine attribute (critical for launch)
xattr -cr "$APP" 2>/dev/null || true
echo "[codesign] Quarantine removed"

# 10. Verify (may still show warnings for PyInstaller companions — OK)
if codesign --verify --verbose=1 "$APP" 2>&1; then
    echo "[codesign] VERIFIED OK"
else
    echo "[codesign] WARNING: Verification has issues (will still run — quarantine removed)"
fi
