#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════
#  ハンイシキ — 앱 밖에 설치된 것들 지우기
#
#  앱(.app)을 휴지통에 버려도 남는 것들이 있습니다. 앱이 사용자 폴더에 따로
#  설치·저장하는 것들입니다:
#
#      ~/Library/Application Support/Miyo/<앱>/
#        llm/                  AI 엔진·모델 (최대 9GB 넘음)
#        archive_index.db      산출물 색인 (수백 MB)
#        hanishiki_config.json 설정·계정 토큰 (옛 이름: miyo_config.json)
#        chrome_capture_profile*/  캡쳐용 크롬 프로필(로그인 상태)
#        script_overrides/     AI 가 고친 스크립트
#        tools/                yt-dlp 자동 갱신본
#
#  용량 때문에 지우고 싶거나, 깨끗한 상태로 다시 시험하고 싶을 때 씁니다.
#
#  ★ 영구 삭제하지 않습니다 — 전부 휴지통으로 옮깁니다.
#    잘못 지웠으면 휴지통에서 되돌릴 수 있습니다.
#    (휴지통 비우기는 직접 하십시오. 그건 되돌릴 수 없습니다.)
#
#  ★ 앱(.app) 자체는 건드리지 않습니다. 그건 Finder 에서 버리시면 됩니다.
# ═══════════════════════════════════════════════════════════════════════════
set -uo pipefail

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"

# ── 어느 앱의 데이터인가 ──────────────────────────────────────────────────
#   ★ 폴더 이름은 .app 파일명이 아니라 '실행 파일 이름'(CFBundleExecutable)이다.
#     앱이 app.setApplicationName(ASCII) 로 데이터 폴더를 정하기 때문이다.
#     .app 파일명은 Finder 표시용이라 카타카나(ハンイシキ.app)다. 그걸 그대로 쓰면
#     엉뚱한 폴더를 보게 된다(실제로 AI 설치기가 그 실수를 해서 모델을 넣어 놓고도
#     앱이 못 찾는 고장이 있었다).
DATA_NAME=""
for CAND in "$SELF_DIR"/*.app "/Applications"/*.app "$HOME/Applications"/*.app; do
    [ -d "$CAND" ] || continue
    ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$CAND/Contents/Info.plist" 2>/dev/null || echo "")"
    case "$ID" in
        com.hanishiki.*|com.miyo.*|com.predormition.*)
            DATA_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$CAND/Contents/Info.plist" 2>/dev/null || echo "")"
            [ -n "$DATA_NAME" ] && break ;;
    esac
done

# ★ 데이터 폴더 위치.
#   지금은 조직 폴더 없이 …/Application Support/<앱> 을 쓴다.
#   예전엔 …/Application Support/Miyo/<앱> 이었다('Miyo' 는 카메라 시절 흔적).
#   앱이 기동할 때 옛 위치를 새 위치로 옮기지만, 아직 한 번도 안 띄웠으면
#   옛 위치에 그대로 있다. 둘 다 본다 — 새 것을 먼저.
data_dir_for() {   # $1 = 앱 이름(ASCII)
    local sup="$HOME/Library/Application Support"
    if [ -d "$sup/$1" ]; then echo "$sup/$1"; else echo "$sup/Miyo/$1"; fi
}
BASE="$HOME/Library/Application Support"

echo ""
echo "  ハンイシキ — 앱 밖에 설치된 것들 지우기"
echo "  ═════════════════════════════════════════════"
echo ""

# 앱을 못 찾아도(이미 버렸어도) 폴더가 하나뿐이면 그걸 쓴다 — 흔한 경우다.
if [ -z "$DATA_NAME" ]; then
    N=$(ls -1d "$BASE"/*/ 2>/dev/null | wc -l | tr -d ' ')
    if [ "$N" = "1" ]; then
        DATA_NAME="$(basename "$(ls -1d "$BASE"/*/ | head -1)")"
        echo "  앱을 못 찾았지만 데이터 폴더가 하나뿐이라 그것을 씁니다."
    elif [ "$N" = "0" ]; then
        echo "  지울 것이 없습니다 — 앱 밖에 설치된 것이 이미 없습니다."
        echo ""; echo "  창을 닫으셔도 됩니다."; echo ""
        exit 0
    else
        echo "  ✘ 어느 것을 지울지 정할 수 없습니다. 데이터 폴더가 여러 개입니다:"
        ls -1d "$BASE"/*/ | sed 's|.*/Miyo/|      |'
        echo ""
        echo "    앱을 먼저 설치하신 뒤 다시 실행해 주십시오(앱 옆에서 이 파일을 실행하면"
        echo "    어느 앱의 것인지 알 수 있습니다)."
        echo ""
        exit 1
    fi
fi

DIR="$(data_dir_for "$DATA_NAME")"
if [ ! -d "$DIR" ]; then
    echo "  지울 것이 없습니다 — $DIR 가 없습니다."
    echo ""; echo "  창을 닫으셔도 됩니다."; echo ""
    exit 0
fi

# ── 무엇이 얼마나 있는지 먼저 보여준다 ────────────────────────────────────
echo "  위치: $DIR"
echo ""
echo "  들어 있는 것:"
du -sh "$DIR"/* 2>/dev/null | sort -rh | head -12 | while read -r sz path; do
    printf "    %-8s %s\n" "$sz" "$(basename "$path")"
done
TOTAL="$(du -sh "$DIR" 2>/dev/null | cut -f1)"
echo ""
echo "  합계: ${TOTAL:-?}"
echo ""

# ── 무엇을 지울지 고르게 한다 ─────────────────────────────────────────────
echo "  무엇을 지울까요?"
echo ""
echo "    1) AI 모델만        llm/ 만 치웁니다. 설정·토큰·색인은 그대로 둡니다."
echo "                        (용량만 비우고 계속 쓰실 때)"
echo "    2) 설정만 남기고    설정 파일만 남기고 나머지를 치웁니다."
echo "                        (토큰을 다시 넣지 않아도 되게)"
echo "    3) 전부            폴더째 치웁니다. 완전히 새로 설치한 상태가 됩니다."
echo "    0) 그만둔다"
echo ""
printf "  번호를 입력하고 Enter: "
# 더블클릭하면 Terminal 이 붙어 /dev/tty 가 열린다. 하지만 스크립트로 파이프해서
#   실행하거나 tty 가 없는 환경이면 /dev/tty 가 안 열린다. 그때 조용히 '그만둠' 으로
#   빠지면 "눌렀는데 아무 일도 안 일어난다" 가 된다 — 표준입력으로 물러선다.
#   [ -r /dev/tty ] 는 참인데 실제로 열면 실패하는 경우가 있다(Device not configured).
#   존재 여부가 아니라 '열리는지' 로 판단한다.
CHOICE=""
if { : </dev/tty; } 2>/dev/null; then
    read -r CHOICE </dev/tty || CHOICE=""
fi
[ -z "$CHOICE" ] && { read -r CHOICE || CHOICE=0; }
CHOICE="$(printf '%s' "${CHOICE:-0}" | tr -d '[:space:]')"

TRASH="$HOME/.Trash"
STAMP="$(date +%Y%m%d_%H%M%S)"
MOVED=0

move_to_trash() {   # $1 = 옮길 경로
    [ -e "$1" ] || return 0
    local dst="$TRASH/$(basename "$1")_$STAMP"
    if mv "$1" "$dst" 2>/dev/null; then
        echo "    ✔ $(basename "$1")  → 휴지통"
        MOVED=$((MOVED + 1))
    else
        echo "    ✘ $(basename "$1") 를 옮기지 못했습니다"
    fi
}

echo ""
case "$CHOICE" in
    1)
        echo "  AI 모델을 치웁니다…"
        move_to_trash "$DIR/llm"
        ;;
    2)
        echo "  설정만 남기고 치웁니다…"
        for item in "$DIR"/*; do
            [ -e "$item" ] || continue
            case "$(basename "$item")" in hanishiki_config.json|miyo_config.json) continue ;; esac
            move_to_trash "$item"
        done
        ;;
    3)
        echo "  전부 치웁니다…"
        move_to_trash "$DIR"
        ;;
    *)
        echo "  그만두었습니다. 아무것도 지우지 않았습니다."
        echo ""; echo "  창을 닫으셔도 됩니다."; echo ""
        exit 0
        ;;
esac

echo ""
echo "  ═════════════════════════════════════════════"
if [ "$MOVED" -eq 0 ]; then
    echo "  옮긴 것이 없습니다."
else
    echo "  $MOVED 개를 휴지통으로 옮겼습니다."
    echo ""
    echo "  ※ 아직 영구 삭제된 것이 아닙니다. 잘못 지우셨으면 휴지통에서"
    echo "     되돌릴 수 있습니다. 확인하신 뒤 휴지통을 비우시면 용량이 실제로 빕니다."
fi
echo ""
echo "  앱(.app) 자체는 건드리지 않았습니다 — 지우시려면 Finder 에서 버리십시오."
echo ""
echo "  창을 닫으셔도 됩니다."
echo ""
