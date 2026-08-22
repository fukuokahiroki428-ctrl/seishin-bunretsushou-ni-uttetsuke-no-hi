#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════
#  소스의 자원을 번들에 다시 맞춘다.
#
#  왜 이 스크립트가 있나:
#    CMakeLists 의 자원 복사는 전부 add_custom_command(TARGET Miyo POST_BUILD) 라서
#    '타겟이 다시 링크될 때만' 돈다. C++ 을 건드리지 않고 자원만 바꾸면
#    (도구 교체·HTML 수정·파이썬 스크립트 수정) 빌드가 조용히 옛것을 그대로 둔다.
#    실제로 ffprobe 를 arm64 로 갈아 끼우고 빌드했는데 번들엔 옛 x86_64 가 남았고,
#    로그에 "Bundling" 줄이 하나도 안 찍혔다. '고쳤다고 믿고 넘어가기' 딱 좋다.
#
#    build.sh 와 CMake 의 항상 도는 타겟이 둘 다 이 스크립트를 부른다.
#    한 곳에만 두면 다른 경로(IDE 빌드, CI)에서 같은 문제가 되살아난다.
#
#  반드시 codesign 앞에서 실행해야 한다 — 서명 뒤에 파일을 바꾸면 봉인이 깨진다.
#
#  사용:  ./sync_resources.sh <bundle.app>
# ═══════════════════════════════════════════════════════════════════════════
set -e

APPDIR="$1"
[ -n "$APPDIR" ] || { echo "usage: $0 <bundle.app>"; exit 1; }
[ -d "$APPDIR" ] || { echo "[sync] 번들이 없습니다: $APPDIR"; exit 0; }

SRCROOT="$(cd "$(dirname "$0")" && pwd)"
SYNCED=0

sync_one() {   # $1=소스 $2=번들 안 상대경로
    [ -f "$1" ] || return 0
    local dst="$APPDIR/$2"
    # 내용 비교(cmp)를 쓰면 안 된다 — 번들 사본에는 codesign 이 서명을 붙여 두어서
    # 원본과 '언제나' 다르다. 그러면 매 빌드마다 전부 갱신으로 잡혀 경고가 무의미해진다.
    # 갱신 시각으로 본다. cp 는 대상 시각을 지금으로 만드니 다음 빌드에선 조용하다.
    if [ ! -f "$dst" ] || [ "$1" -nt "$dst" ]; then
        mkdir -p "$(dirname "$dst")"
        cp -f "$1" "$dst"
        chmod +x "$dst" 2>/dev/null || true
        echo "  갱신: $2"
        SYNCED=$((SYNCED + 1))
    fi
}

sync_one "$SRCROOT/resources/tools/ffmpeg"      "Contents/MacOS/ffmpeg"
sync_one "$SRCROOT/resources/tools/ffprobe"     "Contents/MacOS/ffprobe"
sync_one "$SRCROOT/resources/tools/deno"        "Contents/MacOS/deno"
sync_one "$SRCROOT/resources/tools/yt-dlp"      "Contents/Resources/tools/yt-dlp"
sync_one "$SRCROOT/resources/tools/rclone"      "Contents/Resources/tools/rclone"
sync_one "$SRCROOT/resources/html/index.html"   "Contents/Resources/html/index.html"

# 자동 재서명 도구 — 앱이 스스로 봉인을 복구할 때 쓴다(런타임이 이걸 부른다).
sync_one "$SRCROOT/codesign_app.sh"             "Contents/Resources/tools/codesign_app.sh"
sync_one "$SRCROOT/hanishiki.entitlements"   "Contents/Resources/tools/hanishiki.entitlements"

for _p in "$SRCROOT"/resources/tools/*.py; do
    [ -f "$_p" ] && sync_one "$_p" "Contents/Resources/tools/$(basename "$_p")"
done
for _p in "$SRCROOT"/../../scripts/archive_*.py; do
    [ -f "$_p" ] && sync_one "$_p" "Contents/Resources/tools/archive/$(basename "$_p")"
done

# 번들 Chromium — 폴더라 sync_one 으로 못 다룬다. 없을 때만 넣는다.
#   ★ cp -Rp 여야 한다. cmake -E copy_directory 나 cp -R 은 심볼릭 링크를 따라가
#     실체를 복제해서 356MB 가 1.0GB 로 불고, 프레임워크 구조가 깨져 codesign 이
#     실패한다(실제로 겪었다).
CHROMIUM_SRC="$SRCROOT/resources/chromium/Chromium.app"
CHROMIUM_DST="$APPDIR/Contents/Resources/chromium/Chromium.app"
if [ -d "$CHROMIUM_SRC" ] && [ ! -d "$CHROMIUM_DST" ]; then
    mkdir -p "$(dirname "$CHROMIUM_DST")"
    cp -Rp "$CHROMIUM_SRC" "$CHROMIUM_DST"
    echo "  갱신: Contents/Resources/chromium/Chromium.app"
    SYNCED=$((SYNCED + 1))
elif [ ! -d "$CHROMIUM_SRC" ]; then
    echo "  ⚠ 번들 Chromium 이 없습니다 — 캡쳐가 사용자 시스템 Chrome 에 의존합니다."
    echo "     README 의 '번들 Chromium 넣기' 참고."
fi

if [ "$SYNCED" -eq 0 ]; then
    echo "  번들이 이미 소스와 같습니다."
else
    echo "  $SYNCED 개 갱신됨(CMake POST_BUILD 가 안 돌았다는 뜻)."
    # 자원을 바꿨으면 서명이 어긋난다. 뒤이어 도는 codesign/verify_or_resign 이
    # 다시 서명하도록, 여기서는 알리기만 하고 손대지 않는다.
fi
exit 0
