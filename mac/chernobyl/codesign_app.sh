#!/bin/bash
# Comprehensive codesign script for Chernobyl.app
# 컴포넌트를 inside-out 으로 서명한 뒤 --deep --strict 로 검증한다.
# ★ set -e 안 씀: 일부 nested(companion) 서명 실패는 허용하되,
#   메인 번들 서명/검증은 반드시 성공해야 하며 실패 시 즉시 비정상 종료(exit 1)한다.

APP="$1"
if [ -z "$APP" ] || [ ! -d "$APP" ]; then
    echo "Usage: $0 <path-to-app-bundle>"
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ENTITLEMENTS="${ENTITLEMENTS:-$SCRIPT_DIR/chernobyl.entitlements}"

# ─────────────────────────────────────────────────────────────────────────────
# 서명 ID 선택
#   배포에 유효한 건 "Developer ID Application" 인증서뿐이다(Gatekeeper 통과 + 공증 가능).
#   "Apple Development" 인증서는 배포에 무효 — 오히려 ad-hoc 보다 Gatekeeper 차단이 심하다.
#     (이전 버전이 Apple Development 로 서명한 게 바로 "앱 서명 문제"의 원인이었음)
#   적합한 인증서가 없으면 ad-hoc(-): Apple Silicon 실행에 필요한 최소 서명(로컬 실행용).
#   SIGN_ID 환경변수로 강제 지정 가능.
# ─────────────────────────────────────────────────────────────────────────────
if [ -z "$SIGN_ID" ]; then
    SIGN_ID=$(security find-identity -v -p codesigning 2>/dev/null \
        | grep -oE '"Developer ID Application:[^"]+"' | head -1 | tr -d '"')
fi
if [ -z "$SIGN_ID" ] || [ "$SIGN_ID" = "-" ]; then
    SIGN_ID="-"
    echo "[codesign] Developer ID Application 인증서 없음 → ad-hoc(-) 서명."
    echo "[codesign]   로컬 실행은 되지만 배포본은 첫 실행 시 우클릭→'열기' 필요."
    echo "[codesign]   경고 없는 배포는 Developer ID + 공증(notarytool)이 필요함."
else
    echo "[codesign] Using identity: $SIGN_ID"
fi

# Developer ID 로 서명할 때만 하드닝 런타임 + 타임스탬프 + entitlements(공증 준비).
#   ad-hoc 에 하드닝 런타임을 켜면 의미도 없고 오히려 실행이 막힐 수 있어 적용하지 않는다.
SIGN_FLAGS=(--force --sign "$SIGN_ID")
RUNTIME_FLAGS=()
if [ "$SIGN_ID" != "-" ]; then
    RUNTIME_FLAGS=(--options runtime --timestamp)
    if [ -f "$ENTITLEMENTS" ]; then
        RUNTIME_FLAGS+=(--entitlements "$ENTITLEMENTS")
        echo "[codesign] entitlements: $ENTITLEMENTS"
    else
        echo "[codesign] ⚠ entitlements 파일 없음($ENTITLEMENTS) — WebEngine/python 이 공증 후 막힐 수 있음."
    fi
fi

# 실행 코드/번들용(하드닝 런타임 포함). nested 라이브러리에도 적용해도 무해.
sign_rt() { codesign "${SIGN_FLAGS[@]}" "${RUNTIME_FLAGS[@]}" "$@"; }

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
        sign_rt "$f" 2>/dev/null || true
    fi
done
echo "[codesign] All Mach-O binaries signed"

# 3. Qt 프레임워크
find "$APP/Contents/Frameworks" -maxdepth 1 -name "*.framework" -type d 2>/dev/null | while IFS= read -r fw; do
    sign_rt "$fw" 2>/dev/null || true
done
echo "[codesign] Qt frameworks signed"

# 4. WebEngine 헬퍼(.app)
find "$APP" -name "QtWebEngineProcess.app" -type d 2>/dev/null | while IFS= read -r helper; do
    sign_rt "$helper" 2>/dev/null || true
done
echo "[codesign] WebEngine helper signed"

# 5. Python 프레임워크
find "$APP" -path "*/Python.framework" -not -path "*/Versions/*" -type d 2>/dev/null | while IFS= read -r pfw; do
    sign_rt "$pfw" 2>/dev/null || true
done

# 6. companion app(있을 때만 — 실패해도 진행)
find "$APP/Contents/Resources" -maxdepth 1 -name "*.app" -type d 2>/dev/null | while IFS= read -r capp; do
    sign_rt "$capp" 2>/dev/null || true
    echo "[codesign] Signed companion: $(basename "$capp") (may have warnings)"
done

# 7. Contents/MacOS 의 실행 파일(yt-dlp/ffmpeg/ffprobe/main exe 등)
find "$APP/Contents/MacOS" -type f -print0 2>/dev/null | while IFS= read -r -d $'\0' f; do
    file "$f" 2>/dev/null | grep -q "Mach-O" && { sign_rt "$f" 2>/dev/null || true; }
done
echo "[codesign] Main executables signed"

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
