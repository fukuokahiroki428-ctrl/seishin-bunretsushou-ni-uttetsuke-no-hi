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

# 대상 고르기 — 두 조건을 모두 만족하는 프로세스만.
#   (1) pgrep -x "$NAME"  : 프로세스 '이름' 이 정확히 그 실행 파일 이름.
#       codesign 은 이름이 codesign 이라 여기서 걸러진다(인자로 앱 경로를 받아도 무관).
#   (2) pgrep -f "$EXE"   : 명령줄에 그 번들의 실행 경로가 들어 있음.
#       같은 이름의 다른 번들(예: build/ 와 /Applications/)을 구분한다.
#
# ★ 예전에는 ps -o command= 의 첫 낱말과 문자열 비교를 했는데, ps 는 ASCII 가 아닌
#   글자를 8진 이스케이프로 바꿔서 찍는다(예: 무제 폴더 → M-kM-,M-4M-l\240M^\ ...).
#   그래서 경로에 한글이 들어가면 비교가 '언제나' 실패했고, 스크립트는 아무도 죽이지
#   않은 채 "실행 중인 앱이 없습니다" 라고만 하고 0 으로 끝났다. 조용한 실패였다.
#   pgrep 은 커널이 가진 원본 명령줄로 맞추므로 그 문제가 없다.
pids_of_app() {
    comm -12 <(pgrep -x "$NAME" 2>/dev/null | sort) \
             <(pgrep -f "$EXE"  2>/dev/null | sort)
}

found=0
for pid in $(pids_of_app); do
    kill "$pid" 2>/dev/null && found=$((found + 1))
done

if [ "$found" -eq 0 ]; then
    echo "실행 중인 앱이 없습니다: $EXE"
    exit 0
fi

# 얌전히 끝날 시간을 준다. 안 끝나면 그때만 강제 종료.
for _ in $(seq 1 20); do
    [ -z "$(pids_of_app)" ] && { echo "앱 $found 개를 종료했습니다."; exit 0; }
    sleep 0.5
done

for pid in $(pids_of_app); do
    kill -9 "$pid" 2>/dev/null
done
echo "앱을 강제 종료했습니다(10초 안에 안 끝남)."
