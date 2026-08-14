#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════
#  실행 중인 앱만 안전하게 종료한다.
#
#  왜 이 스크립트가 있나:
#    pkill -f "MacOS/Predormition" 처럼 명령줄 전체를 훑어 죽이면, 앱이 아니라
#    '앱 경로를 인자로 받은 다른 프로세스' 까지 잡힌다. 특히 codesign 은
#        codesign --force --sign ... /경로/Predormition.app/Contents/MacOS/Predormition
#    이라 그 패턴에 그대로 걸린다. 서명 도중에 죽이면 dylib 이 손상되고
#    (실제로 16개 → 54개까지 망가졌다) 앱은 아무 말 없이 안 뜬다.
#
#    여기서는 '실행 파일 경로가 정확히 그것인 프로세스' 만 고른다.
#    codesign 은 실행 파일이 /usr/bin/codesign 이므로 절대 걸리지 않는다.
#
#  사용:
#    ./kill_app.sh                      # /Applications 에 설치된 앱
#    ./kill_app.sh build/Predormition.app   # 특정 번들
# ═══════════════════════════════════════════════════════════════════════════
set -e
cd "$(dirname "$0")"

APP="${1:-/Applications/Predormition.app}"
[ -d "$APP" ] || { echo "번들이 없습니다: $APP"; exit 1; }

NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$APP/Contents/Info.plist" 2>/dev/null || basename "$APP" .app)"
EXE="$APP/Contents/MacOS/$NAME"
# 심볼릭 링크·상대 경로를 흡수해 실제 경로로 맞춘다.
EXE="$(cd "$(dirname "$EXE")" && pwd)/$(basename "$EXE")"

found=0
for pid in $(pgrep -x "$NAME" 2>/dev/null); do
    # ps 의 첫 인자(argv[0]) 가 그 실행 파일인 것만 — 인자로 경로를 받은 프로세스는 제외.
    argv0="$(ps -o command= -p "$pid" 2>/dev/null | awk '{print $1}')"
    [ "$argv0" = "$EXE" ] || continue
    kill "$pid" 2>/dev/null && found=$((found + 1))
done

if [ "$found" -eq 0 ]; then
    echo "실행 중인 앱이 없습니다: $EXE"
    exit 0
fi

# 얌전히 끝날 시간을 준다. 안 끝나면 그때만 강제 종료.
for _ in $(seq 1 20); do
    still=0
    for pid in $(pgrep -x "$NAME" 2>/dev/null); do
        argv0="$(ps -o command= -p "$pid" 2>/dev/null | awk '{print $1}')"
        [ "$argv0" = "$EXE" ] && still=1
    done
    [ "$still" -eq 0 ] && { echo "앱 $found 개를 종료했습니다."; exit 0; }
    sleep 0.5
done

for pid in $(pgrep -x "$NAME" 2>/dev/null); do
    argv0="$(ps -o command= -p "$pid" 2>/dev/null | awk '{print $1}')"
    [ "$argv0" = "$EXE" ] && kill -9 "$pid" 2>/dev/null
done
echo "앱을 강제 종료했습니다(10초 안에 안 끝남)."
