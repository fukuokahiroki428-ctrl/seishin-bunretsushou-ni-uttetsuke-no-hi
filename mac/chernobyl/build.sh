#!/bin/bash
set -e
cd "$(dirname "$0")"

APPDIR="build/見よ.app"
FWDIR="$APPDIR/Contents/Frameworks"
RESDIR="$APPDIR/Contents/Resources"

echo "=== Building ==="
cmake --build build --target Miyo

echo "=== macdeployqt ==="
/opt/homebrew/opt/qtbase/bin/macdeployqt "$APPDIR" 2>&1 | grep -v "^$\|File exists"

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

echo "=== Restoring sub-apps ==="
rm -rf "$RESDIR/anipo.app" "$RESDIR/anipo-2.app" "$RESDIR/AINU.app"
cp -a "$(dirname "$0")/../anipo 원본/dist/anipo.app" "$RESDIR/anipo.app"
cp -a "$(dirname "$0")/../anipo/dist/anipo-2.app" "$RESDIR/anipo-2.app"
cp -a "$(dirname "$0")/../ainu/dist/AINU.app" "$RESDIR/AINU.app"

echo "=== Signing ==="
for fw in "$FWDIR"/Qt*.framework; do codesign --force --sign - "$fw" 2>/dev/null; done
for dl in "$FWDIR"/*.dylib; do codesign --force --sign - "$dl" 2>/dev/null; done
find "$APPDIR/Contents/PlugIns" -name "*.dylib" | while read pl; do codesign --force --sign - "$pl" 2>/dev/null; done
codesign --force --sign - "$FWDIR/QtWebEngineCore.framework/Versions/A/Helpers/QtWebEngineProcess.app" 2>/dev/null
codesign --force --sign - "$APPDIR/Contents/MacOS/ffprobe" 2>/dev/null
codesign --force --sign - "$APPDIR/Contents/MacOS/ffmpeg" 2>/dev/null
codesign --force --sign - "$APPDIR/Contents/MacOS/yt-dlp" 2>/dev/null
codesign --force --sign - "$APPDIR" 2>/dev/null

echo "=== Done! ==="
echo "Run: open \"$APPDIR\""
