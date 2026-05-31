#!/bin/bash
# DMG 생성 스크립트 — Chernobyl.app + Pen.app 둘 다
# 사용: ./make_dmg.sh

set -e

DIST_DIR="/Users/shio/Downloads/무제폴더/귀찮아/dist"
mkdir -p "$DIST_DIR"

SIGN_ID="Apple Development: seonghyun298@gmail.com (748FMV6UG5)"

make_dmg() {
    local APP_PATH="$1"
    local APP_NAME="$2"
    local DMG_NAME="$3"

    if [ ! -d "$APP_PATH" ]; then
        echo "❌ 앱이 없습니다: $APP_PATH"
        return 1
    fi

    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "📦 $APP_NAME → $DMG_NAME"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    # 임시 staging dir
    local STAGE_DIR=$(mktemp -d)
    trap "rm -rf $STAGE_DIR" EXIT

    # 앱 복사
    echo "  복사 중..."
    cp -R "$APP_PATH" "$STAGE_DIR/"

    # Applications 심볼릭 링크 (드래그 설치 UX)
    ln -s /Applications "$STAGE_DIR/Applications"

    # 임시 DMG 만들고 압축
    local OUT_DMG="$DIST_DIR/$DMG_NAME.dmg"
    rm -f "$OUT_DMG"

    echo "  DMG 생성..."
    hdiutil create -volname "$APP_NAME" \
        -srcfolder "$STAGE_DIR" \
        -ov \
        -format UDZO \
        -imagekey zlib-level=9 \
        "$OUT_DMG" 2>&1 | tail -3

    # DMG 서명
    echo "  서명..."
    codesign --force --sign "$SIGN_ID" "$OUT_DMG" 2>&1 | tail -2

    # quarantine 제거
    xattr -cr "$OUT_DMG" 2>/dev/null || true

    # 검증
    echo "  검증..."
    codesign --verify --verbose=1 "$OUT_DMG" 2>&1 | tail -2

    local SIZE=$(du -sh "$OUT_DMG" | awk '{print $1}')
    echo "  ✅ $OUT_DMG ($SIZE)"

    rm -rf "$STAGE_DIR"
    trap - EXIT
}

# 1) Chernobyl (메인 앱)
CHERN_APP="/Users/shio/Downloads/무제폴더/귀찮아/mac/miyo_cpp/build/Chernobyl.app"
make_dmg "$CHERN_APP" "Chernobyl" "Chernobyl"

# 2) Pen (beta 크롤러) — 배포명 "팬을 잘 쓰고 싶다"
#    ★ build/Pen.app 은 Homebrew Qt 의존 → 그대로 배포하면 Qt 없는 맥에서 안 켜짐.
#      make_dist.sh 로 Qt 프레임워크를 .app 안에 번들(self-contained)한 뒤 DMG 화.
PEN_DIST_APP="/tmp/Pen_dist.app"
echo "  Pen self-contained 번들 생성 (macdeployqt)..."
bash "/Users/shio/Downloads/무제폴더/귀찮아/beta/scripts/make_dist.sh" "$PEN_DIST_APP" 2>&1 \
    | grep -E "✅|⚠|homebrew|valid on disk|완료" | sed 's/^/    /'
make_dmg "$PEN_DIST_APP" "팬을 잘 쓰고 싶다" "팬을 잘 쓰고 싶다"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ 완료"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
ls -lh "$DIST_DIR"/*.dmg 2>/dev/null
