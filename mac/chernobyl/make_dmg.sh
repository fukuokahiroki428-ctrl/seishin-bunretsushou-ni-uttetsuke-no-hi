#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════
#  배포용 DMG 를 만든다.
#
#  사용:  ./make_dmg.sh [bundle.app]
#         (인자 없으면 build/ 의 .app 을 자동으로 찾는다)
#
#  주의할 점들 — 전에 물린 적이 있는 것들이다:
#   · 파일 이름은 ASCII 로만 만든다. 앱 표시 이름은 ハンイシキ 지만, GitHub 릴리즈
#     자산 이름이나 다른 도구가 비ASCII 에서 깨지는 일이 있어 Hanishiki 를 쓴다.
#   · DMG 에 넣기 전에 서명을 검증한다. 깨진 앱을 담아 배포하면 받는 쪽에서
#     macOS 가 SIGKILL 로 죽인다(원인이 안 보인다).
#   · 이 앱은 공증(notarize)되어 있지 않다. 받는 사람은 Gatekeeper 경고를 본다.
#     공증하려면 notarytool submit + stapler staple 이 따로 필요하다.
# ═══════════════════════════════════════════════════════════════════════════
set -e
cd "$(dirname "$0")"

APP="${1:-$(ls -d build/*.app 2>/dev/null | head -1)}"
[ -n "$APP" ] && [ -d "$APP" ] || { echo "❌ .app 을 찾지 못했습니다."; exit 1; }
APP="$(cd "$(dirname "$APP")" && pwd)/$(basename "$APP")"

VERSION="$(cat ../../VERSION 2>/dev/null || echo 0.0.0)"
# ★ DMG 파일 이름은 ASCII 로 고정한다.
#   번들 폴더 이름은 이제 카타카나(ハンイシキ.app)라 basename 을 쓰면 DMG 이름까지
#   비ASCII 가 된다. GitHub 릴리즈 자산 이름이나 다른 도구에서 깨질 수 있다.
#   실행 파일 이름(CFBundleExecutable)은 ASCII 로 유지되므로 그것을 쓴다.
NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$APP/Contents/Info.plist" 2>/dev/null \
        || basename "$APP" .app)"
# 볼륨 이름(창 제목)은 보이는 것이므로 표시 이름을 쓴다.
VOLNAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleName' "$APP/Contents/Info.plist" 2>/dev/null || echo "$NAME")"
OUT="build/${NAME}-${VERSION}.dmg"

# ── 서명 확인 — 깨진 걸 담아 배포하지 않는다 ──────────────────────────────
echo "=== 서명 확인 ==="
if ! codesign --verify --deep --strict "$APP" 2>&1; then
    # ★ '무효' 라고만 하면 원인을 못 찾는다. 실제로 겪은 경우는 번들이 망가진 게
    #   아니라 '고치는 중' 이었다 — 앱은 기동 9초 뒤 봉인이 깨진 걸 알면 스스로
    #   재서명을 시작하고, 그 일은 1분쯤 걸린다. 그 사이에 여기서 검사하면
    #   안쪽만 새로 서명되고 바깥 봉인은 옛것이라 반드시 무효로 나온다.
    #   그러니 진행 중인 재서명이 있는지 먼저 보고 다르게 안내한다.
    if pgrep -f "codesign_app.sh" >/dev/null 2>&1; then
        echo "⏳ 지금 앱이 스스로 서명을 복구하는 중입니다(1분쯤)."
        echo "   그것이 끝난 뒤에 다시 실행하십시오. 번들이 망가진 것이 아닙니다."
    else
        echo "❌ 서명이 유효하지 않습니다. DMG 를 만들지 않습니다."
        echo "   (이대로 배포하면 받는 쪽에서 macOS 가 앱을 죽입니다)"
        echo "   고치려면: ./codesign_app.sh \"$APP\""
    fi
    exit 1
fi
echo "  ✔ 유효"

# ── 담을 것만 모은다 ──────────────────────────────────────────────────────
STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT INT TERM
# -R 이 아니라 -Rp: 심볼릭 링크·권한을 그대로 옮겨야 서명 봉인이 유지된다.
cp -Rp "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"      # 끌어다 놓기용

# ── 받는 사람용 안내와 준비 스크립트 ──────────────────────────────────────
#   공증을 안 했으므로 받는 쪽에서 첫 실행이 막힌다. 매번 xattr 명령을 알려 주는
#   대신 더블클릭용 .command 를 같이 넣는다. 그 스크립트는 이 앱 하나의 격리
#   표시만 뗀다 — 시스템 Gatekeeper 는 절대 건드리지 않는다.
for _f in "$(dirname "$0")"/resources/dmg/*; do
    [ -e "$_f" ] || continue
    cp -p "$_f" "$STAGE/"
    case "$_f" in *.command) chmod +x "$STAGE/$(basename "$_f")" ;; esac
    echo "  동봉: $(basename "$_f")"
done

echo "=== DMG 만드는 중 ($OUT) ==="
rm -f "$OUT"
hdiutil create -volname "$VOLNAME $VERSION" \
               -srcfolder "$STAGE" \
               -ov -format UDZO \
               -quiet \
               "$OUT"

# ── 담긴 것이 성한지 되확인 ───────────────────────────────────────────────
echo "=== 결과 확인 ==="
MNT="$(mktemp -d)"
hdiutil attach "$OUT" -nobrowse -quiet -mountpoint "$MNT"
if codesign --verify --deep --strict "$MNT/$(basename "$APP")" 2>&1; then
    echo "  ✔ DMG 안 앱의 서명 정상"
else
    echo "  ❌ DMG 안에서 서명이 깨졌습니다"
    hdiutil detach "$MNT" -quiet; rmdir "$MNT"; exit 1
fi
MINOS="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' \
         "$MNT/$(basename "$APP")/Contents/Info.plist" 2>/dev/null || echo '?')"
hdiutil detach "$MNT" -quiet; rmdir "$MNT"

echo ""
echo "=== 완료 ==="
echo "  파일     : $(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
echo "  크기     : $(du -h "$OUT" | cut -f1)"
echo "  최소 macOS: $MINOS"
echo "  ※ 공증 안 됨 — 받는 사람은 Gatekeeper 경고를 봅니다."
