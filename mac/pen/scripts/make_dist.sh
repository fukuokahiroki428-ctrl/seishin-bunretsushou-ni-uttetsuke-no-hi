#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════
# make_dist.sh — Pen.app 을 "진짜 배포 가능한" self-contained .app 으로 변환.
#   build/Pen.app (시스템 Homebrew Qt 의존) → macdeployqt 로 Qt 프레임워크 번들 →
#   homebrew 잔재 경로 전부 @rpath/@executable_path 로 치환 → inside-out 코드사인.
#   결과: Homebrew Qt 없는 깨끗한 맥에서도 실행됨.
#
# 사용: scripts/make_dist.sh [출력_app_경로]
#   기본 출력: /tmp/Pen_dist.app
# ═══════════════════════════════════════════════════════════════════════════
set -e

BETA_DIR="$(cd "$(dirname "$0")/.." && pwd)"
SRC_APP="$BETA_DIR/build/Pen.app"
OUT_APP="${1:-/tmp/Pen_dist.app}"
MACDEPLOYQT="$(command -v macdeployqt || echo /opt/homebrew/bin/macdeployqt)"

SIGN_ID=$(security find-identity -v -p codesigning 2>/dev/null \
    | grep -m1 "Apple Development" | sed -E 's/.*\) ([0-9A-F]+) "(.*)"/\2/' )
[ -z "$SIGN_ID" ] && SIGN_ID="-"

echo "━━ 1) build/Pen.app → $OUT_APP 복사 ━━"
[ ! -d "$SRC_APP" ] && { echo "❌ $SRC_APP 없음 — 먼저 빌드하세요"; exit 1; }
rm -rf "$OUT_APP"
cp -R "$SRC_APP" "$OUT_APP"

echo "━━ 2) macdeployqt (Qt 프레임워크 번들 + 경로 재작성) ━━"
"$MACDEPLOYQT" "$OUT_APP" -no-strip 2>&1 | grep -vE "^ERROR: (Cannot resolve|using QList)" || true

echo "━━ 3) WebEngine 헬퍼 homebrew 경로 치환 ━━"
bash "$BETA_DIR/scripts/fix_webengine_helper.sh" "$OUT_APP" 2>&1 | tail -1 || true

echo "━━ 4) dangling Frameworks 심볼릭 정리 ━━"
find "$OUT_APP/Contents/Frameworks" -maxdepth 1 -type l -delete 2>/dev/null || true

echo "━━ 5) 남은 homebrew self-ID / 참조 전부 @rpath 치환 ━━"
FW="$OUT_APP/Contents/Frameworks"
# dylib + 프레임워크 바이너리의 self-ID 및 의존성 중 homebrew 절대경로 → @rpath
fix_macho() {
    local f="$1"
    # self-ID
    local id; id=$(otool -D "$f" 2>/dev/null | tail -1)
    if echo "$id" | grep -q "/opt/homebrew"; then
        local base; base=$(basename "$f")
        if [[ "$f" == *.framework/* ]]; then
            install_name_tool -id "@rpath/$base.framework/Versions/A/$base" "$f" 2>/dev/null || true
        else
            install_name_tool -id "@rpath/$base" "$f" 2>/dev/null || true
        fi
    fi
    # dependencies
    otool -L "$f" 2>/dev/null | grep "/opt/homebrew" | awk '{print $1}' | while read -r lib; do
        local nm; nm=$(basename "$lib")
        if [[ "$lib" == *.framework/* ]]; then
            install_name_tool -change "$lib" "@rpath/$nm.framework/Versions/A/$nm" "$f" 2>/dev/null || true
        else
            install_name_tool -change "$lib" "@rpath/$nm" "$f" 2>/dev/null || true
        fi
    done
}
while IFS= read -r f; do fix_macho "$f"; done < <(
    find "$FW" -type f -name "*.dylib"
    find "$FW" -type d -name "*.framework" | while read -r d; do b=$(basename "$d" .framework); echo "$d/Versions/A/$b"; done
)
# 메인 바이너리의 안 쓰이는 homebrew rpath 제거
otool -l "$OUT_APP/Contents/MacOS/Pen" | awk '/LC_RPATH/{g=1} g&&/path /{print $2; g=0}' | grep "/opt/homebrew" | while read -r rp; do
    install_name_tool -delete_rpath "$rp" "$OUT_APP/Contents/MacOS/Pen" 2>/dev/null || true
done

echo "━━ 6) homebrew 잔재 최종 스캔 ━━"
set +e  # grep 실패(=잔재 0개, 정상)가 set -e 로 스크립트를 죽이지 않게
LEFT=$(
    { find "$OUT_APP" -type f -name "*.dylib"
      find "$FW" -type d -name "*.framework" | while read -r d; do b=$(basename "$d" .framework); echo "$d/Versions/A/$b"; done
      echo "$OUT_APP/Contents/MacOS/Pen"
      echo "$OUT_APP/Contents/Frameworks/QtWebEngineCore.framework/Versions/A/Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess"
    } | while read -r f; do [ -f "$f" ] && otool -L "$f" 2>/dev/null | grep -q "/opt/homebrew" && echo "$f"; done
)
if [ -n "$LEFT" ]; then echo "⚠ homebrew 잔재 남음:"; echo "$LEFT"; else echo "✅ homebrew 의존 0 — 완전 자급자족"; fi
set -e

echo "━━ 7) inside-out 코드사인 (id: $SIGN_ID) ━━"
WEBENG="$FW/QtWebEngineCore.framework"
HELPER_APP="$WEBENG/Versions/A/Helpers/QtWebEngineProcess.app"
# dylib → framework → webengine helper → main exe → app
find "$OUT_APP" -type f -name "*.dylib" -print0 | xargs -0 -P8 -I{} codesign --force --sign "$SIGN_ID" {} 2>/dev/null || true
find "$FW" -maxdepth 1 -name "*.framework" -type d ! -name "QtWebEngineCore.framework" -print0 \
    | xargs -0 -I{} codesign --force --sign "$SIGN_ID" {} 2>/dev/null || true
[ -d "$HELPER_APP" ] && codesign --force --sign "$SIGN_ID" "$HELPER_APP" 2>/dev/null || true
codesign --force --sign "$SIGN_ID" "$WEBENG/Versions/A" 2>/dev/null || true
codesign --force --sign "$SIGN_ID" "$OUT_APP/Contents/MacOS/Pen" 2>/dev/null || true
codesign --force --sign "$SIGN_ID" "$OUT_APP" 2>/dev/null || true

echo "━━ 8) --deep --strict 검증 ━━"
codesign --verify --deep --strict --verbose=1 "$OUT_APP" 2>&1 | tail -2

echo ""
echo "✅ 완료: $OUT_APP ($(du -sh "$OUT_APP" | awk '{print $1}'))"
