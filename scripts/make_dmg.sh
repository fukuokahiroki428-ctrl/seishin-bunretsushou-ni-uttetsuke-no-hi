#!/bin/bash
# DMG 생성 스크립트 — Chernobyl.app + Pen.app 둘 다
# 사용: ./make_dmg.sh
#   경로/서명ID 는 환경변수로 덮어쓸 수 있음 (DIST_DIR, CHERN_APP, MAKE_DIST, SIGN_ID).

set -e

DIST_DIR="${DIST_DIR:-/Users/shio/Downloads/무제폴더/귀찮아/dist}"
mkdir -p "$DIST_DIR"

# ─────────────────────────────────────────────────────────────────────────────
# 서명 ID — 배포에 유효한 "Developer ID Application" 우선, 없으면 ad-hoc(-).
#   ★ 이전엔 "Apple Development: ..." 인증서를 하드코딩했는데, 이건 배포에 무효라
#     Gatekeeper 가 차단한다(앱 서명 문제의 원인). 개발 인증서는 절대 쓰지 않는다.
#   SIGN_ID 환경변수로 강제 지정 가능.
# ─────────────────────────────────────────────────────────────────────────────
if [ -z "$SIGN_ID" ]; then
    SIGN_ID=$(security find-identity -v -p codesigning 2>/dev/null \
        | grep -oE '"Developer ID Application:[^"]+"' | head -1 | tr -d '"')
    [ -z "$SIGN_ID" ] && SIGN_ID="-"
fi
if [ "$SIGN_ID" = "-" ]; then
    echo "서명 ID: ad-hoc(-) — 배포본은 첫 실행 시 우클릭→'열기' 필요(공증 안 됨)."
else
    echo "서명 ID: $SIGN_ID"
fi

make_dmg() {
    local APP_PATH="$1"     # 소스 .app
    local VOL_NAME="$2"     # 마운트 시 보일 볼륨 이름(한글 OK)
    local DMG_NAME="$3"     # 출력 파일 이름(★ GitHub 릴리스용으로 ASCII 만)

    if [ ! -d "$APP_PATH" ]; then
        echo "❌ 앱이 없습니다: $APP_PATH"
        return 1
    fi

    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
    echo "📦 $VOL_NAME → $DMG_NAME.dmg"
    echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

    local STAGE_DIR
    STAGE_DIR=$(mktemp -d)
    trap 'rm -rf "$STAGE_DIR"' RETURN

    echo "  복사 중..."
    cp -R "$APP_PATH" "$STAGE_DIR/"
    ln -s /Applications "$STAGE_DIR/Applications"

    local OUT_DMG="$DIST_DIR/$DMG_NAME.dmg"
    rm -f "$OUT_DMG"

    echo "  DMG 생성..."
    hdiutil create -volname "$VOL_NAME" \
        -srcfolder "$STAGE_DIR" \
        -ov -format UDZO -imagekey zlib-level=9 \
        "$OUT_DMG" 2>&1 | tail -3

    # DMG 서명/검증 — Developer ID 일 때만(공증 대상). ad-hoc 은 의미 없어 생략.
    if [ "$SIGN_ID" != "-" ]; then
        echo "  서명..."
        codesign --force --sign "$SIGN_ID" --timestamp "$OUT_DMG" \
            || { echo "  ❌ DMG 서명 실패"; return 1; }
        echo "  검증..."
        codesign --verify --verbose=2 "$OUT_DMG" \
            || { echo "  ❌ DMG 검증 실패"; return 1; }
    else
        echo "  (ad-hoc — DMG 서명 생략)"
    fi
    xattr -cr "$OUT_DMG" 2>/dev/null || true

    local SIZE
    SIZE=$(du -sh "$OUT_DMG" | awk '{print $1}')
    echo "  ✅ $OUT_DMG ($SIZE)"
}

# 1) Chernobyl (메인 앱)
CHERN_APP="${CHERN_APP:-/Users/shio/Downloads/무제폴더/귀찮아/mac/miyo_cpp/build/Chernobyl.app}"
make_dmg "$CHERN_APP" "Chernobyl" "Chernobyl"

# 2) Pen (배포명 "팬을 잘 쓰고 싶다") — self-contained 번들 생성 후 DMG
#    ★ 파일명은 ASCII "Pen" (GitHub 릴리스 자산은 ASCII 만 허용). 볼륨명만 한글.
PEN_DIST_APP="${PEN_DIST_APP:-/tmp/Pen_dist.app}"
MAKE_DIST="${MAKE_DIST:-/Users/shio/Downloads/무제폴더/귀찮아/beta/scripts/make_dist.sh}"
if [ -f "$MAKE_DIST" ]; then
    echo "  Pen self-contained 번들 생성 (macdeployqt)..."
    bash "$MAKE_DIST" "$PEN_DIST_APP" 2>&1 \
        | grep -E "✅|⚠|homebrew|valid on disk|완료" | sed 's/^/    /' || true
fi
make_dmg "$PEN_DIST_APP" "팬을 잘 쓰고 싶다" "Pen"

echo ""
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo "✅ 완료"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
ls -lh "$DIST_DIR"/*.dmg 2>/dev/null
