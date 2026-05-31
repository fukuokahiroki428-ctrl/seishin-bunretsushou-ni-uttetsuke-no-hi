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
ln -sf "../../../../../../../Frameworks" "$HELPER_CONTENTS/Frameworks"

# Add rpath to main app's Frameworks
install_name_tool -add_rpath "@loader_path/../../../../../../../../Frameworks" "$HELPER_BIN" 2>/dev/null || true

# Fix all homebrew absolute paths to @rpath
otool -L "$HELPER_BIN" | grep '/opt/homebrew/' | awk '{print $1}' | while read lib; do
    # Extract framework name: e.g. /opt/homebrew/.../QtCore.framework/Versions/A/QtCore → QtCore.framework
    fw=$(basename "$(dirname "$(dirname "$(dirname "$lib")")")")
    name=$(basename "$lib")
    install_name_tool -change "$lib" "@rpath/$fw/Versions/A/$name" "$HELPER_BIN" 2>/dev/null || true
done

echo "QtWebEngineProcess fixed"
