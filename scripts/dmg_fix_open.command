#!/bin/bash
# ════════════════════════════════════════════════════════════════════════════
#  Chernobyl 실행 허용 / 서명 수리 도구  (더블클릭)
#  - macOS Gatekeeper 가 "손상됨/확인 불가" 로 막을 때 풀어줍니다.
#  - 검역(quarantine) 속성 제거 + 코드사인 재서명(개발자 인증서 있으면 그걸로, 없으면 ad-hoc).
#  ※ 본인이 받은 앱에만 사용하세요.
# ════════════════════════════════════════════════════════════════════════════
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"

# 대상 앱 탐색: 같은 DMG/폴더 → Applications → 상위 폴더
APP=""
for c in "$DIR/Chernobyl.app" "/Applications/Chernobyl.app" "$DIR/../Chernobyl.app"; do
    [ -d "$c" ] && { APP="$c"; break; }
done
if [ -z "$APP" ]; then
    echo "❌ Chernobyl.app 을 찾지 못했습니다."
    echo "   먼저 Chernobyl.app 을 Applications 폴더로 드래그한 뒤 다시 실행하세요."
    echo ""; read -r -p "엔터를 눌러 닫기..."; exit 1
fi

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo " 대상: $APP"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

echo "[1/3] 검역(quarantine) 속성 제거..."
xattr -cr "$APP" 2>/dev/null && echo "      ✓ 완료"

echo "[2/3] 코드사인 재서명..."
DEVID=$(security find-identity -v -p codesigning 2>/dev/null \
    | grep -oE '"Developer ID Application:[^"]+"' | head -1 | tr -d '"')
[ -z "$DEVID" ] && DEVID=$(security find-identity -v -p codesigning 2>/dev/null \
    | grep -oE '"Apple Development:[^"]+"' | head -1 | tr -d '"')
if [ -n "$DEVID" ]; then
    echo "      개발자 인증서로 서명: $DEVID"
    codesign --force --deep --sign "$DEVID" "$APP" 2>/dev/null && echo "      ✓ 서명 완료"
else
    echo "      인증서 없음 → ad-hoc 재서명(서명 제거 효과 — Gatekeeper 우회)"
    codesign --force --deep --sign - "$APP" 2>/dev/null && echo "      ✓ ad-hoc 서명 완료"
fi

echo "[3/3] 검증..."
if codesign --verify --deep --strict "$APP" 2>/dev/null; then
    echo "      ✓ 서명 유효"
else
    echo "      ⚠ 검증 경고(보통 실행엔 문제 없음 — 검역은 제거됨)"
fi

echo ""
echo "✅ 완료! 이제 Chernobyl 을 더블클릭해서 실행하세요."
echo "   (그래도 막히면: 우클릭 → 열기 → 열기)"
echo ""
read -r -p "엔터를 눌러 닫기..."
