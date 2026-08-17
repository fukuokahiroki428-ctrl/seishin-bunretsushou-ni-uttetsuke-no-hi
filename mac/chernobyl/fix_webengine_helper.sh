#!/bin/bash
# Fix QtWebEngineProcess helper's library paths after macdeployqt
# macdeployqt doesn't fix the helper binary's references to homebrew

BUNDLE_DIR="$1"
if [ -z "$BUNDLE_DIR" ]; then
    echo "Usage: $0 <bundle.app>"
    exit 1
fi

HELPER_BIN="$BUNDLE_DIR/Contents/Frameworks/QtWebEngineCore.framework/Versions/A/Helpers/QtWebEngineProcess.app/Contents/MacOS/QtWebEngineProcess"
HELPER_CONTENTS="$BUNDLE_DIR/Contents/Frameworks/QtWebEngineCore.framework/Versions/A/Helpers/QtWebEngineProcess.app/Contents"

if [ ! -f "$HELPER_BIN" ]; then
    echo "QtWebEngineProcess not found, skipping"
    exit 0
fi

# Create Frameworks symlink so @executable_path/../Frameworks resolves to main app's Frameworks
#
# ★ -n 과 rm -f 가 둘 다 필요하다. 이유:
#   이 링크는 본 번들의 Contents/Frameworks 를 가리키는 '디렉터리 심볼릭 링크' 다.
#   두 번째 빌드에서 링크가 이미 있는 채로 그냥 `ln -sf 대상 링크` 를 하면,
#   ln 은 링크를 갈아끼우는 대신 링크를 '따라 들어가' 그 안에 만든다. 즉
#       본번들/Contents/Frameworks/Frameworks -> ../../../../../../../Frameworks
#   가 생기고, 이건 그 위치 기준으로는 번들 밖을 가리켜 끊어진 링크가 된다.
#   끊어진 링크가 하나라도 있으면 codesign --verify --deep 이
#   "No such file or directory" 로 실패하고, macOS 는 그런 앱을 SIGKILL 로 죽인다.
#   실제로 이 저장소에서 두 번째 빌드마다 그 일이 벌어지고 있었다.
#   -n(--no-dereference) 은 대상이 디렉터리 링크여도 따라가지 않게 하고,
#   rm -f 는 -n 을 지원하지 않는 낡은 ln 에서도 같은 결과가 되게 한다.
rm -f "$HELPER_CONTENTS/Frameworks"
ln -sfn "../../../../../../../Frameworks" "$HELPER_CONTENTS/Frameworks"

# Add rpath to main app's Frameworks
install_name_tool -add_rpath "@loader_path/../../../../../../../../Frameworks" "$HELPER_BIN" 2>/dev/null || true

# Fix all homebrew absolute paths to @rpath
otool -L "$HELPER_BIN" | grep '/opt/homebrew/' | awk '{print $1}' | while read lib; do
    name=$(basename "$lib")
    case "$lib" in
        *.framework/*)
            # e.g. /opt/homebrew/.../QtCore.framework/Versions/A/QtCore → @rpath/QtCore.framework/Versions/A/QtCore
            fw=$(basename "$(dirname "$(dirname "$(dirname "$lib")")")")
            install_name_tool -change "$lib" "@rpath/$fw/Versions/A/$name" "$HELPER_BIN" 2>/dev/null || true
            ;;
        *)
            # ★ 일반 dylib(프레임워크 아님): /opt/homebrew/lib/libfoo.dylib → @rpath/libfoo.dylib
            #   (이전엔 basename(dirname×3)="opt" 라서 @rpath/opt/... 잘못된 경로를 만들었음)
            install_name_tool -change "$lib" "@rpath/$name" "$HELPER_BIN" 2>/dev/null || true
            ;;
    esac
done

echo "QtWebEngineProcess fixed"
