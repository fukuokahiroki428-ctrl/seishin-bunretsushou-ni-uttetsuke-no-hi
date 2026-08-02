#!/bin/bash
# ════════════════════════════════════════════════════════════════════════════
#  Predormition 실행 허용 / 서명 수리 도구  (더블클릭)
#  - macOS Gatekeeper 가 "손상됨/확인 불가" 로 막을 때 풀어줍니다.
#  - 검역(quarantine) 속성 제거 + 코드사인 재서명.
#  ※ 본인이 받은 앱에만 사용하세요.
# ════════════════════════════════════════════════════════════════════════════
set -u
DIR="$(cd "$(dirname "$0")" && pwd)"
APP_NAME="Predormition"

fail() { echo ""; echo "$1"; echo ""; read -r -p "엔터를 눌러 닫기..."; exit 1; }

# ── 대상 앱 찾기 ───────────────────────────────────────────────────────────
# ★ '설치된' 앱을 먼저 찾는다. 예전엔 스크립트와 같은 폴더($DIR)를 1순위로 봐서,
#   DMG 안에서 더블클릭하면 읽기 전용 DMG 사본을 대상으로 잡았다. 그러면 xattr /
#   codesign 이 조용히 실패하는데도 마지막에 "완료" 를 찍어, 사용자는 고쳐진 줄
#   알지만 정작 설치된 앱은 그대로 막혀 있었다.
APP=""
for c in "/Applications/$APP_NAME.app" "$HOME/Applications/$APP_NAME.app" \
         "$DIR/../$APP_NAME.app" "$DIR/$APP_NAME.app"; do
    [ -d "$c" ] && { APP="$c"; break; }
done
[ -z "$APP" ] && fail "❌ $APP_NAME.app 을 찾지 못했습니다.
   먼저 $APP_NAME.app 을 Applications 폴더로 드래그해 설치한 뒤 다시 실행하세요."

# ★ 읽기 전용 볼륨(마운트된 DMG) 위의 앱은 고칠 수 없다 — 여기서 분명히 알린다.
if ! touch "$APP/.__wtest" 2>/dev/null; then
    fail "❌ 이 앱은 읽기 전용 위치에 있어 수리할 수 없습니다:
     $APP

   DMG 창의 $APP_NAME 을 왼쪽 Applications 폴더로 드래그해 설치한 뒤,
   이 파일을 다시 더블클릭해 주세요."
fi
rm -f "$APP/.__wtest" 2>/dev/null

echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"
echo " 대상: $APP"
echo "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━"

echo "[1/3] 검역(quarantine) 속성 제거..."
if xattr -cr "$APP" 2>/dev/null; then echo "      ✓ 완료"; else echo "      ⚠ 일부 속성을 지우지 못했습니다"; fi

echo "[2/3] 코드사인 재서명..."
DEVID=$(security find-identity -v -p codesigning 2>/dev/null \
    | grep -oE '"Developer ID Application:[^"]+"' | head -1 | tr -d '"')
[ -z "$DEVID" ] && DEVID=$(security find-identity -v -p codesigning 2>/dev/null \
    | grep -oE '"Apple Development:[^"]+"' | head -1 | tr -d '"')

# 앱 안에 동봉된 정식 서명 스크립트가 있으면 그걸 쓴다 — entitlements(JIT/라이브러리 검증
# 해제 등)와 하드닝 런타임을 보존해 재서명한다. 손으로 codesign --deep 만 하면 이것들이
# 벗겨져 WebEngine 캡쳐가 죽거나 공증이 불가능해진다.
# DMG 에 함께 담긴 것 → 앱 번들 안 → 없으면 직접 codesign 순.
SIGNER=""
for cand in "$DIR/codesign_app.sh" "$APP/Contents/Resources/codesign_app.sh"; do
    [ -f "$cand" ] && { SIGNER="$cand"; break; }
done
if [ -n "$SIGNER" ]; then
    echo "      동봉된 서명 스크립트 사용"
    SIGN_ID="${DEVID:--}" bash "$SIGNER" "$APP" 2>&1 | sed 's/^/      /'
    SIGN_RC=${PIPESTATUS[0]}
else
    if [ -n "$DEVID" ]; then
        echo "      개발자 인증서로 서명: $DEVID"
        codesign --force --deep --sign "$DEVID" "$APP" 2>&1 | sed 's/^/      /'
    else
        echo "      인증서 없음 → ad-hoc 재서명"
        codesign --force --deep --sign - "$APP" 2>&1 | sed 's/^/      /'
    fi
    SIGN_RC=${PIPESTATUS[0]}
fi
[ "$SIGN_RC" = "0" ] && echo "      ✓ 서명 완료" || echo "      ⚠ 서명 중 오류 (코드 $SIGN_RC)"

echo "[3/3] 검증..."
if codesign --verify --deep --strict "$APP" 2>/dev/null; then
    VERIFIED=1; echo "      ✓ 서명 유효"
else
    VERIFIED=0; echo "      ✗ 서명 검증 실패"
fi

echo ""
# ★ 실제로 고쳐졌을 때만 성공이라고 말한다. 예전엔 아무것도 못 고쳐도 "✅ 완료" 를 찍었다.
if [ "$VERIFIED" = "1" ]; then
    echo "✅ 완료! 이제 $APP_NAME 을 더블클릭해서 실행하세요."
    echo "   (그래도 막히면: 우클릭 → 열기 → 열기)"
else
    echo "❌ 수리하지 못했습니다."
    echo "   앱을 다시 내려받아 설치해 보세요. 그래도 안 되면 이 창의 내용을 알려주세요."
fi
echo ""
read -r -p "엔터를 눌러 닫기..."
