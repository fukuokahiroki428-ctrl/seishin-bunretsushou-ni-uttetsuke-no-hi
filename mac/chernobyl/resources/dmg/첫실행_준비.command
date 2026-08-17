#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════
#  ハンイシキ — 첫 실행 준비
#
#  이 앱은 Apple 공증(notarization)을 받지 않았습니다. 그래서 인터넷에서 받은
#  파일에 macOS 가 붙이는 '격리(quarantine)' 표시 때문에 그냥 더블클릭하면
#  "확인되지 않은 개발자" 라며 열리지 않습니다.
#
#  이 스크립트가 하는 일은 딱 하나입니다:
#      설치된 ハンイシキ.app 한 개에서 격리 표시를 뗀다.
#
#  하지 않는 일:
#      · 시스템 Gatekeeper 를 끄지 않습니다(spctl --master-disable 같은 건
#        맥 전체를 무방비로 만들기 때문에 절대 쓰지 않습니다).
#      · 다른 앱은 건드리지 않습니다.
#      · 관리자 권한을 요구하지 않습니다.
#
#  떼기 전에 서명이 성한지 먼저 확인합니다 — 서명이 깨진 앱이라면 격리를 떼도
#  macOS 가 실행 중에 죽이므로, 그때는 멈추고 알려 드립니다.
# ═══════════════════════════════════════════════════════════════════════════

APP_NAME="Hanishiki.app"
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"

echo ""
echo "  ハンイシキ — 첫 실행 준비"
echo "  ─────────────────────────────────────────────"
echo ""

# ── 어디에 설치돼 있나 ────────────────────────────────────────────────────
APP=""
for CAND in "/Applications/$APP_NAME" "$HOME/Applications/$APP_NAME"; do
    [ -d "$CAND" ] && { APP="$CAND"; break; }
done

if [ -z "$APP" ]; then
    echo "  ✘ 아직 설치되지 않았습니다."
    echo ""
    echo "    먼저 이 창(DMG)에서 ハンイシキ 를 옆의 Applications 폴더로"
    echo "    끌어다 놓으신 다음, 이 파일을 다시 실행해 주십시오."
    echo ""
    if [ -d "$SELF_DIR/$APP_NAME" ]; then
        echo "    (지금은 DMG 안의 것만 보입니다 — DMG 안에서는 고칠 수 없습니다.)"
        echo ""
    fi
    echo "  창을 닫으셔도 됩니다."
    exit 1
fi

echo "  찾았습니다: $APP"
echo ""

# ── 서명부터 확인 ─────────────────────────────────────────────────────────
echo "  1/2  서명을 확인합니다…"
if ! codesign --verify --deep --strict "$APP" >/dev/null 2>&1; then
    echo "       ✘ 서명이 유효하지 않습니다."
    echo ""
    echo "         옮기는 중에 파일이 상했을 수 있습니다. 격리만 떼도 macOS 가"
    echo "         실행 중에 앱을 죽이므로 여기서 멈춥니다."
    echo "         DMG 에서 다시 설치해 보시고, 그래도 같으면 알려 주십시오."
    echo ""
    exit 1
fi
echo "       ✔ 정상"

# ── 격리 표시 떼기 ────────────────────────────────────────────────────────
echo "  2/2  격리 표시를 뗍니다…"
if ! xattr -dr com.apple.quarantine "$APP" 2>/dev/null; then
    # 이미 없으면 xattr 이 실패로 끝난다 — 실제로 남아 있는지 보고 판단한다.
    if xattr -pr com.apple.quarantine "$APP" >/dev/null 2>&1; then
        echo "       ✘ 떼지 못했습니다(권한 문제일 수 있습니다)."
        echo ""
        echo "         터미널에서 아래를 실행해 보십시오:"
        echo "         sudo xattr -dr com.apple.quarantine \"$APP\""
        echo ""
        exit 1
    fi
fi
echo "       ✔ 완료"

echo ""
echo "  ─────────────────────────────────────────────"
echo "  이제 ハンイシキ 를 그냥 실행하시면 됩니다."
echo "  이 준비는 한 번만 하면 되고, 다음 판을 새로 받으시면 다시 하시면 됩니다."
echo ""
echo "  창을 닫으셔도 됩니다."
echo ""
