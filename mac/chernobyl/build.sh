#!/bin/bash
set -e
cd "$(dirname "$0")"

# ═══ 빌드 잠금 ═══════════════════════════════════════════════════════════
#  같은 build/ 에서 빌드를 두 개 겹쳐 돌리면 서로의 산출물을 지운다.
#  make/ninja 는 링크 전에 대상 파일을 지우므로, 뒤에 시작한 쪽이 앞선 쪽이 막
#  만들어 둔 실행 파일을 없애 버린다. 그러면 빌드는 '성공' 이라고 하는데
#  Contents/MacOS 에 실행 파일이 없어 앱이 조용히 안 뜬다(실제로 두 번 겪었다).
#
#  mkdir 은 원자적이라 잠금으로 쓸 수 있다(flock 은 맥 기본에 없다).
LOCK="build/.build.lock"
mkdir -p build
if ! mkdir "$LOCK" 2>/dev/null; then
    OWNER="$(cat "$LOCK/pid" 2>/dev/null || echo '?')"
    if [ "$OWNER" != "?" ] && kill -0 "$OWNER" 2>/dev/null; then
        echo "❌ 이미 빌드가 돌고 있습니다 (PID $OWNER). 끝난 뒤에 다시 실행하세요."
        echo "   강제로 풀려면:  rm -rf '$PWD/$LOCK'"
        exit 1
    fi
    echo "⚠ 남아 있던 잠금을 정리합니다(PID $OWNER 는 이미 없음)."
    rm -rf "$LOCK"; mkdir "$LOCK"
fi
echo $$ > "$LOCK/pid"
trap 'rm -rf "$LOCK"' EXIT INT TERM

echo "=== Building ==="
cmake --build build --target Miyo

# 빌드 산출 .app 자동 탐색(이름은 -DAPP_NAME 옵션에 따라 달라짐 — 하드코딩 금지). 인자/ENV 로 지정 가능.
APPDIR="${1:-${APPDIR:-$(ls -d build/*.app 2>/dev/null | head -1)}}"
if [ -z "$APPDIR" ] || [ ! -d "$APPDIR" ]; then
    echo "❌ build/ 에 .app 이 없습니다. cmake 구성/빌드를 확인하세요."
    exit 1
fi
FWDIR="$APPDIR/Contents/Frameworks"

echo "=== macdeployqt ($APPDIR) ==="
MACDEPLOYQT="$(command -v macdeployqt || echo /opt/homebrew/opt/qtbase/bin/macdeployqt)"
"$MACDEPLOYQT" "$APPDIR" 2>&1 | grep -v "^$\|File exists" || true

echo "=== Fixing framework paths ==="
for fwbin in "$FWDIR"/Qt*.framework/Versions/A/Qt*; do
  [ -f "$fwbin" ] || continue; file "$fwbin" 2>/dev/null | grep -q "Mach-O" || continue
  ARGS=""
  while IFS= read -r dep; do
    [ -z "$dep" ] && continue; R=${dep#@executable_path/../Frameworks/}
    [ "$R" = "$dep" ] && continue; ARGS="$ARGS -change $dep @loader_path/../../../$R"
  done < <(otool -L "$fwbin" 2>/dev/null | awk 'NR>1{print $1}' | grep "@executable_path")
  [ -n "$ARGS" ] && eval install_name_tool $ARGS "\"$fwbin\"" 2>/dev/null
done
for dylib in "$FWDIR"/*.dylib; do
  [ -f "$dylib" ] || continue; file "$dylib" 2>/dev/null | grep -q "Mach-O" || continue
  ARGS=""
  while IFS= read -r dep; do
    [ -z "$dep" ] && continue; R=${dep#@executable_path/../Frameworks/}
    [ "$R" = "$dep" ] && continue; ARGS="$ARGS -change $dep @loader_path/$R"
  done < <(otool -L "$dylib" 2>/dev/null | awk 'NR>1{print $1}' | grep "@executable_path")
  [ -n "$ARGS" ] && eval install_name_tool $ARGS "\"$dylib\"" 2>/dev/null
done

PROC="$FWDIR/QtWebEngineCore.framework/Versions/A/Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess"
if [ -f "$PROC" ]; then
  ARGS=""
  while IFS= read -r dep; do
    [ -z "$dep" ] && continue; R=${dep#@executable_path/../Frameworks/}
    [ "$R" = "$dep" ] && continue; ARGS="$ARGS -change $dep @loader_path/../../../../../../../$R"
  done < <(otool -L "$PROC" 2>/dev/null | awk 'NR>1{print $1}' | grep "@executable_path")
  [ -n "$ARGS" ] && eval install_name_tool $ARGS "\"$PROC\"" 2>/dev/null
fi

# ★ companion sub-app(anipo/AINU) 재복사 제거 — CMakeLists 가 의도적으로 제외한 것.
#   이들은 codesign "sealed resource missing" → 번들이 macOS 에서 SIGKILL 되는 원인이었다.
#   (이전 build.sh 가 이걸 다시 넣어 CMake 가 고친 버그를 재현하고 있었음)

# ★ QtWebEngineProcess 헬퍼의 /opt/homebrew 절대경로 → @rpath 정정 (macdeployqt 가 헬퍼는 안 고침).
#   안 하면 배포본에서 렌더러가 못 떠 '백지 창'이 된다. (위 @executable_path 처리와 상보적)
bash "$(dirname "$0")/fix_webengine_helper.sh" "$APPDIR" || true

echo "=== Codesign (inside-out + --deep --strict verify) ==="
# 단일 서명 경로로 위임 — 서명/검증 실패 시 codesign_app.sh 가 exit 1 → set -e 로 중단.
bash "$(dirname "$0")/codesign_app.sh" "$APPDIR"

echo "=== Done! ==="
echo "Run: open \"$APPDIR\""
