#!/bin/bash
set -e
cd "$(dirname "$0")"

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

echo "=== Codesign (inside-out + --deep --strict verify) ==="
# 단일 서명 경로로 위임 — 서명/검증 실패 시 codesign_app.sh 가 exit 1 → set -e 로 중단.
bash "$(dirname "$0")/codesign_app.sh" "$APPDIR"

echo "=== Done! ==="
echo "Run: open \"$APPDIR\""
