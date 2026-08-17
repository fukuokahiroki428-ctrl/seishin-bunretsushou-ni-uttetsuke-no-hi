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

# ★ 서명 직전: 끊어진 심볼릭 링크 제거.
#   codesign --verify --deep 는 끊어진 링크를 하나라도 만나면 그 위치가 아니라
#   앱 전체를 가리키며 "No such file or directory" 로 실패한다. 원인을 찾기가 아주 나쁘다.
#   그런 앱을 macOS 는 SIGKILL 로 죽인다.
#   같은 정리가 CMakeLists 의 POST_BUILD 에도 있지만 그건 '타겟이 다시 링크될 때만' 돈다.
#   build.sh 는 소스 변경 없이도 macdeployqt 와 헬퍼 수정을 다시 돌리는 별도 경로이므로
#   여기에도 있어야 한다(실제로 없어서 두 번째 빌드마다 서명이 깨졌다).
#   CMake 쪽처럼 링크를 몽땅 지우지는 않는다 — Frameworks 에는 libfoo.1.dylib 같은
#   정상 링크도 있다. 끊어진 것만 고른다.
BROKEN=0
while IFS= read -r link; do
    [ -z "$link" ] && continue
    echo "  끊어진 링크 제거: ${link#$APPDIR/}"
    rm -f "$link"; BROKEN=$((BROKEN + 1))
done < <(find "$APPDIR" -type l ! -exec test -e {} \; -print 2>/dev/null)
[ "$BROKEN" -gt 0 ] && echo "=== 끊어진 심볼릭 링크 $BROKEN 개 정리함 ==="

# ★ 번들 도구의 아키텍처 확인.
#   ffmpeg·deno·rclone 은 arm64 인데 ffprobe 만 x86_64 로 들어가 있었다.
#   이 맥에는 로제타가 깔려 있어 그냥 돌아버리는 바람에 아무도 몰랐다. 로제타가
#   없는 애플 실리콘 맥에서는 실행 자체가 안 된다("Bad CPU type in executable").
#   조용히 지나가지 않게 빌드가 대신 봐 준다. 빌드를 세우지는 않는다 —
#   로제타가 있으면 실제로 동작은 하므로, 크게 눈에 띄게 알리기만 한다.
echo "=== 번들 도구 아키텍처 확인 (호스트: $(uname -m)) ==="
HOST_ARCH="$(uname -m)"
MISMATCH=0
for T in "$APPDIR/Contents/MacOS/ffmpeg" \
         "$APPDIR/Contents/MacOS/ffprobe" \
         "$APPDIR/Contents/MacOS/deno" \
         "$APPDIR/Contents/Resources/tools/rclone"; do
    [ -f "$T" ] || continue
    ARCHS="$(lipo -archs "$T" 2>/dev/null)" || continue
    [ -z "$ARCHS" ] && continue
    case " $ARCHS " in
        *" $HOST_ARCH "*) ;;                       # 호스트 아키텍처 포함 → 정상
        *)
            echo "  ⚠ $(basename "$T"): $ARCHS — $HOST_ARCH 아님."
            echo "     로제타 없는 맥에서 실행 불가. 같은 아키텍처 빌드로 교체하십시오:"
            echo "     resources/tools/$(basename "$T")"
            MISMATCH=$((MISMATCH + 1))
            ;;
    esac
done
if [ "$MISMATCH" -gt 0 ]; then
    echo "  ⚠ 아키텍처가 어긋난 도구 $MISMATCH 개 — 위 안내를 보십시오."
else
    echo "  모든 번들 도구가 $HOST_ARCH 를 지원합니다."
fi

# ★ Info.plist 의 LSMinimumSystemVersion 을 '실제로 필요한 값' 으로 맞춘다.
#   Info.plist.in 에는 12.0 이라고 손으로 적혀 있었는데, 실측하면 26.0 이 필요하다
#   (실행 파일 minos 26.0, 그리고 QtWebChannel·libicu 등 Homebrew dylib 63개가 26.0).
#   그 값은 우리가 고른 게 아니라 Homebrew 병이 어떤 macOS 에서 만들어졌느냐로 정해진다.
#   손으로 적어 두면 Homebrew 를 올릴 때마다 조용히 어긋나고, 그러면 macOS 12~25
#   사용자는 설치는 되는데 실행만 안 되는(원인 안 보이는) 상태가 된다.
#   → 번들 안 Mach-O 들의 minos 최대값을 그때그때 계산해 적는다.
#   반드시 codesign 앞에서 — plist 를 서명 뒤에 고치면 봉인이 깨진다.
FLOOR="$(
  { vtool -show-build "$APPDIR/Contents/MacOS/$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$APPDIR/Contents/Info.plist")" 2>/dev/null
    find "$APPDIR/Contents/Frameworks" "$APPDIR/Contents/PlugIns" -type f 2>/dev/null | while IFS= read -r f; do
        file -b "$f" 2>/dev/null | grep -q "Mach-O" && vtool -show-build "$f" 2>/dev/null
    done
  } | awk '/minos/{print $2}' | sort -V | tail -1
)"
if [ -n "$FLOOR" ]; then
    DECLARED="$(/usr/libexec/PlistBuddy -c 'Print :LSMinimumSystemVersion' "$APPDIR/Contents/Info.plist" 2>/dev/null || echo '')"
    if [ "$DECLARED" != "$FLOOR" ]; then
        echo "=== 최소 macOS 정정: ${DECLARED:-(없음)} → $FLOOR (실측) ==="
        /usr/libexec/PlistBuddy -c "Set :LSMinimumSystemVersion $FLOOR" "$APPDIR/Contents/Info.plist" 2>/dev/null \
          || /usr/libexec/PlistBuddy -c "Add :LSMinimumSystemVersion string $FLOOR" "$APPDIR/Contents/Info.plist"
    else
        echo "=== 최소 macOS $FLOOR (선언과 실측 일치) ==="
    fi
fi

echo "=== Codesign (inside-out + --deep --strict verify) ==="
# 단일 서명 경로로 위임 — 서명/검증 실패 시 codesign_app.sh 가 exit 1 → set -e 로 중단.
bash "$(dirname "$0")/codesign_app.sh" "$APPDIR"

echo "=== Done! ==="
echo "Run: open \"$APPDIR\""
