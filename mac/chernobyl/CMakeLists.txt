cmake_minimum_required(VERSION 3.20)
project(Miyo VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Qt6
#   ★ GuiPrivate 명시 — QXlsx 가 Qt6::GuiPrivate 를 링크하므로 private 컴포넌트를 직접 요청해
#     Qt6::GuiPrivate 타깃을 생성해야 한다(Gui 만으론, transitive 만으론 미생성).
find_package(Qt6 REQUIRED COMPONENTS
    Core Gui GuiPrivate Widgets WebEngineWidgets WebChannel Network WebSockets)

# QXlsx
include(FetchContent)
FetchContent_Declare(QXlsx
    GIT_REPOSITORY https://github.com/QtExcel/QXlsx.git
    GIT_TAG v1.5.0   # ★ master 고정 금지 — 릴리스 태그로 핀 (재현 가능 빌드). commit 0333829.
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR QXlsx)
FetchContent_MakeAvailable(QXlsx)

# Sources
set(CORE_SOURCES
    src/core/MainWindow.cpp
    src/core/MiyoBackend.cpp
    src/core/Common.cpp
    src/core/Config.cpp
)

set(CORE_HEADERS
    src/core/MainWindow.h
    src/core/MiyoBackend.h
    src/core/WorkerSignals.h
    src/core/Common.h
    src/core/Config.h
)

set(UTILS_SOURCES
    src/utils/HttpClient.cpp
    src/utils/ExcelWriter.cpp
    src/utils/FileHelper.cpp
    src/utils/DiskJsonBuffer.cpp
    src/utils/ContentSecurityScanner.cpp
    src/utils/WebDavUploader.cpp
)

set(UTILS_HEADERS
    src/utils/HttpClient.h
    src/utils/ExcelWriter.h
    src/utils/FileHelper.h
    src/utils/DiskJsonBuffer.h
    src/utils/ContentSecurityScanner.h
    src/utils/WebDavUploader.h
)

set(PLATFORM_SOURCES
    src/platforms/DiscordCollector.cpp
    src/platforms/BlueskyCollector.cpp
    src/platforms/InstagramCollector.cpp
    src/platforms/YouTubeDownloader.cpp
    src/platforms/TwitterCollector.cpp
    src/platforms/SiteCrawler.cpp
    src/platforms/RealChromeCrawler.cpp
)

set(PLATFORM_HEADERS
    src/platforms/DiscordCollector.h
    src/platforms/BlueskyCollector.h
    src/platforms/InstagramCollector.h
    src/platforms/YouTubeDownloader.h
    src/platforms/TwitterCollector.h
    src/platforms/SiteCrawler.h
    src/platforms/RealChromeCrawler.h
)

# Resources
set(RESOURCES resources/miyo.qrc)

# ═══════════════════════════════════════════
# Platform-specific executable setup
# ═══════════════════════════════════════════

if(APPLE)
    # macOS bundle settings
    set(MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

    add_executable(Miyo MACOSX_BUNDLE
        src/main.cpp
        ${CORE_SOURCES} ${CORE_HEADERS}
        ${UTILS_SOURCES} ${UTILS_HEADERS}
        ${PLATFORM_SOURCES} ${PLATFORM_HEADERS}
        ${RESOURCES}
    )
elseif(WIN32)
    # Windows: WIN32 flag for GUI app (no console window)
    add_executable(Miyo WIN32
        src/main.cpp
        ${CORE_SOURCES} ${CORE_HEADERS}
        ${UTILS_SOURCES} ${UTILS_HEADERS}
        ${PLATFORM_SOURCES} ${PLATFORM_HEADERS}
        ${RESOURCES}
    )
else()
    add_executable(Miyo
        src/main.cpp
        ${CORE_SOURCES} ${CORE_HEADERS}
        ${UTILS_SOURCES} ${UTILS_HEADERS}
        ${PLATFORM_SOURCES} ${PLATFORM_HEADERS}
        ${RESOURCES}
    )
endif()

target_include_directories(Miyo PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/src/platforms
)

# ═══════════════════════════════════════════
# Platform-specific linking
# ═══════════════════════════════════════════

target_link_libraries(Miyo PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::WebEngineWidgets
    Qt6::WebChannel
    Qt6::Network
    Qt6::WebSockets
    QXlsx::QXlsx
)

if(APPLE)
    target_link_libraries(Miyo PRIVATE "-framework IOKit")
elseif(WIN32)
    target_link_libraries(Miyo PRIVATE dwmapi)
endif()

# ═══════════════════════════════════════════
# macOS-specific post-build steps
# ═══════════════════════════════════════════

if(APPLE)
    # App name / icon / identifier — override with -DAPP_NAME="..." -DAPP_ICON="..." -DAPP_ID="..."
    if(NOT APP_NAME)
        set(APP_NAME "ABIWA")
    endif()
    if(NOT APP_ICON)
        set(APP_ICON "abiwa.icns")
    endif()
    if(NOT APP_ID)
        set(APP_ID "com.miyo.app")
    endif()

    set_target_properties(Miyo PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/Info.plist.in
        MACOSX_BUNDLE_ICON_FILE "${APP_ICON}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${APP_ID}"
        MACOSX_BUNDLE_BUNDLE_NAME "${APP_NAME}"
        OUTPUT_NAME "${APP_NAME}"
    )

    # Post-build: copy yt-dlp and ffmpeg into the app bundle
    set(YTDLP_PATH "${CMAKE_SOURCE_DIR}/resources/tools/yt-dlp")
    set(FFMPEG_PATH "${CMAKE_SOURCE_DIR}/resources/tools/ffmpeg")
    set(FFPROBE_PATH "${CMAKE_SOURCE_DIR}/resources/tools/ffprobe")

    if(EXISTS "${YTDLP_PATH}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${YTDLP_PATH}" "$<TARGET_BUNDLE_DIR:Miyo>/Contents/MacOS/yt-dlp"
            COMMAND chmod +x "$<TARGET_BUNDLE_DIR:Miyo>/Contents/MacOS/yt-dlp"
            COMMENT "Bundling yt-dlp"
        )
    endif()

    if(EXISTS "${FFMPEG_PATH}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFMPEG_PATH}" "$<TARGET_BUNDLE_DIR:Miyo>/Contents/MacOS/ffmpeg"
            COMMAND chmod +x "$<TARGET_BUNDLE_DIR:Miyo>/Contents/MacOS/ffmpeg"
            COMMENT "Bundling ffmpeg"
        )
    endif()

    if(EXISTS "${FFPROBE_PATH}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFPROBE_PATH}" "$<TARGET_BUNDLE_DIR:Miyo>/Contents/MacOS/ffprobe"
            COMMAND chmod +x "$<TARGET_BUNDLE_DIR:Miyo>/Contents/MacOS/ffprobe"
            COMMENT "Bundling ffprobe"
        )
    endif()

    # ★ rclone — WebDAV/SFTP/S3/Mountain Duck 호환 (50+ protocol, MIT 라이센스)
    set(RCLONE_PATH "${CMAKE_SOURCE_DIR}/resources/tools/rclone")
    if(EXISTS "${RCLONE_PATH}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/tools"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RCLONE_PATH}" "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/tools/rclone"
            COMMAND chmod +x "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/tools/rclone"
            COMMENT "Bundling rclone (WebDAV/SFTP/Mountain Duck 호환 fast transfer)"
        )
    endif()

    # ★ HTML 외부화 — AUTORCC 의존성 추적 깨졌을 때 즉시 반영 위해 별도 복사
    add_custom_command(TARGET Miyo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources/html"
            "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/html"
        COMMENT "Bundling HTML (external — bypasses AUTORCC cache issues)"
    )

    # ★ Chromium 번들 — Chrome for Testing (~340MB, BSD)
    #   사용자 시스템 Chrome 의존성 제거. 캡쳐 전용 Chromium 자체 동봉.
    set(CHROMIUM_SRC "${CMAKE_SOURCE_DIR}/resources/chromium/Chromium.app")
    if(EXISTS "${CHROMIUM_SRC}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CHROMIUM_SRC}"
                "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/chromium/Chromium.app"
            COMMENT "Bundling Chromium"
        )
    endif()

    # exiftool (프로젝트 내 Perl script + libs → Resources/tools/exiftool/)
    set(EXIFTOOL_SRC "${CMAKE_SOURCE_DIR}/resources/tools/exiftool")
    if(EXISTS "${EXIFTOOL_SRC}/exiftool")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${EXIFTOOL_SRC}"
                "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/tools/exiftool"
            COMMAND chmod +x "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/tools/exiftool/exiftool"
            COMMENT "Bundling exiftool + Perl libs"
        )
    endif()

    # Python tool scripts
    set(TOOLS_DIR "${CMAKE_SOURCE_DIR}/resources/tools")
    add_custom_command(TARGET Miyo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/tools"
        COMMENT "Creating tools directory"
    )
    foreach(TOOL_SCRIPT twitter_tid.py twitter_api.py twitter_daemon.py bluesky_daemon.py email_watch.py)
        if(EXISTS "${TOOLS_DIR}/${TOOL_SCRIPT}")
            add_custom_command(TARGET Miyo POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${TOOLS_DIR}/${TOOL_SCRIPT}" "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/tools/${TOOL_SCRIPT}"
                COMMENT "Bundling ${TOOL_SCRIPT}"
            )
        endif()
    endforeach()

    # SingleFile Chrome 확장 — 진짜 페이지 캡쳐 시 인라인 저장에 사용
    if(EXISTS "${TOOLS_DIR}/singlefile_extension")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${TOOLS_DIR}/singlefile_extension"
                "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/tools/singlefile_extension"
            COMMENT "Bundling SingleFile extension"
        )
    endif()

    # Bundle Python venv
    add_custom_command(TARGET Miyo POST_BUILD
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/bundle_python.sh" "$<TARGET_BUNDLE_DIR:Miyo>"
        COMMENT "Bundling Python venv (twikit, httpx, atproto)"
    )

    # Chernobyl.png
    set(CHERNOBYL_PATH "${CMAKE_SOURCE_DIR}/../Chernobyl.png")
    if(EXISTS "${CHERNOBYL_PATH}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CHERNOBYL_PATH}" "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/Chernobyl.png"
            COMMENT "Bundling Chernobyl.png"
        )
    endif()

    # Fix QtWebEngineProcess
    add_custom_command(TARGET Miyo POST_BUILD
        COMMAND bash "${CMAKE_SOURCE_DIR}/fix_webengine_helper.sh" "$<TARGET_BUNDLE_DIR:Miyo>"
        COMMENT "Fixing QtWebEngineProcess library paths"
    )

    # Icon — APP_ICON_PATH can be overridden via -DAPP_ICON_PATH="..."
    if(NOT APP_ICON_PATH)
        set(APP_ICON_PATH "${CMAKE_SOURCE_DIR}/resources/abiwa.icns")
    endif()
    if(EXISTS "${APP_ICON_PATH}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${APP_ICON_PATH}" "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/${APP_ICON}"
            COMMENT "Bundling app icon: ${APP_ICON}"
        )
    endif()

    # Bundle fonts
    add_custom_command(TARGET Miyo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/fonts"
    )
    file(GLOB FONT_FILES "${CMAKE_SOURCE_DIR}/resources/fonts/*.ttf")
    foreach(FONT ${FONT_FILES})
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FONT}"
                "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/fonts/"
            COMMENT "Bundling font: ${FONT}"
        )
    endforeach()

    # ★ Companion PyInstaller apps (anipo, AINU)는 미사용 + codesign sealed resource missing 원인 →
    #   bundle 463MB 차지 + macOS launch SIGKILL 일으킴. 빌드에서 제외.
    #   필요해지면 별도 외부 폴더에서 실행하면 됨.

    # miyo_cli
    set(MIYO_CLI_PATH "${CMAKE_SOURCE_DIR}/../miyo_cli")
    if(EXISTS "${MIYO_CLI_PATH}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${MIYO_CLI_PATH}" "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/miyo_cli"
            COMMENT "Bundling miyo_cli"
        )
    endif()

    # ★ Codesign 직전: fix_webengine_helper.sh가 만든 잘못된 심볼릭 링크 정리
    #   "Frameworks/Frameworks -> ../../../../../Frameworks" 같은 dangling symlink가
    #   codesign sealed resource verify를 깨뜨려 macOS가 SIGKILL로 앱 죽임.
    add_custom_command(TARGET Miyo POST_BUILD
        COMMAND find "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Frameworks" -maxdepth 1 -type l -delete 2>/dev/null || true
        COMMENT "Cleaning broken Frameworks symlinks before codesign"
    )

    # Codesign
    add_custom_command(TARGET Miyo POST_BUILD
        COMMAND bash "${CMAKE_SOURCE_DIR}/codesign_app.sh" "$<TARGET_BUNDLE_DIR:Miyo>"
        COMMENT "Codesigning entire app bundle"
    )
endif()

# ═══════════════════════════════════════════
# Windows-specific post-build steps
# ═══════════════════════════════════════════

if(WIN32)
    # ★ APP_NAME 미설정 시 Chernobyl 기본값 (이전 "ABIWA" 하드코딩 → -DAPP_NAME 무시됐음)
    if(NOT APP_NAME)
        set(APP_NAME "Chernobyl")
    endif()
    set_target_properties(Miyo PROPERTIES
        OUTPUT_NAME "${APP_NAME}"
        WIN32_EXECUTABLE TRUE
    )

    set(WIN_OUTPUT_DIR "$<TARGET_FILE_DIR:Miyo>")

    # Python tool scripts → <exe_dir>/tools/
    set(TOOLS_DIR "${CMAKE_SOURCE_DIR}/resources/tools")
    add_custom_command(TARGET Miyo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WIN_OUTPUT_DIR}/tools"
        COMMENT "Creating tools directory"
    )
    foreach(TOOL_SCRIPT twitter_tid.py twitter_api.py twitter_daemon.py bluesky_daemon.py email_watch.py)
        if(EXISTS "${TOOLS_DIR}/${TOOL_SCRIPT}")
            add_custom_command(TARGET Miyo POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${TOOLS_DIR}/${TOOL_SCRIPT}" "${WIN_OUTPUT_DIR}/tools/${TOOL_SCRIPT}"
                COMMENT "Bundling ${TOOL_SCRIPT}"
            )
        endif()
    endforeach()

    # yt-dlp.exe, ffmpeg.exe → <exe_dir>/
    set(YTDLP_WIN "${CMAKE_SOURCE_DIR}/resources/tools/yt-dlp.exe")
    set(FFMPEG_WIN "${CMAKE_SOURCE_DIR}/resources/tools/ffmpeg.exe")
    set(FFPROBE_WIN "${CMAKE_SOURCE_DIR}/resources/tools/ffprobe.exe")

    if(EXISTS "${YTDLP_WIN}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${YTDLP_WIN}" "${WIN_OUTPUT_DIR}/yt-dlp.exe"
            COMMENT "Bundling yt-dlp.exe"
        )
    endif()
    if(EXISTS "${FFMPEG_WIN}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FFMPEG_WIN}" "${WIN_OUTPUT_DIR}/ffmpeg.exe"
            COMMENT "Bundling ffmpeg.exe"
        )
    endif()
    if(EXISTS "${FFPROBE_WIN}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FFPROBE_WIN}" "${WIN_OUTPUT_DIR}/ffprobe.exe"
            COMMENT "Bundling ffprobe.exe"
        )
    endif()

    # Fonts → <exe_dir>/fonts/
    add_custom_command(TARGET Miyo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WIN_OUTPUT_DIR}/fonts"
    )
    file(GLOB FONT_FILES "${CMAKE_SOURCE_DIR}/resources/fonts/*.ttf")
    foreach(FONT ${FONT_FILES})
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FONT}" "${WIN_OUTPUT_DIR}/fonts/"
            COMMENT "Bundling font: ${FONT}"
        )
    endforeach()

    # Chernobyl.png
    set(CHERNOBYL_PATH "${CMAKE_SOURCE_DIR}/../Chernobyl.png")
    if(EXISTS "${CHERNOBYL_PATH}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CHERNOBYL_PATH}" "${WIN_OUTPUT_DIR}/Chernobyl.png"
            COMMENT "Bundling Chernobyl.png"
        )
    endif()

    # exiftool.exe → <exe_dir>/
    set(EXIFTOOL_WIN "${CMAKE_SOURCE_DIR}/resources/tools/exiftool.exe")
    if(EXISTS "${EXIFTOOL_WIN}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${EXIFTOOL_WIN}" "${WIN_OUTPUT_DIR}/exiftool.exe"
            COMMENT "Bundling exiftool.exe"
        )
    endif()

    # miyo_cli → <exe_dir>/miyo_cli/
    set(MIYO_CLI_WIN "${CMAKE_SOURCE_DIR}/../miyo_cli")
    if(EXISTS "${MIYO_CLI_WIN}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${MIYO_CLI_WIN}" "${WIN_OUTPUT_DIR}/miyo_cli"
            COMMENT "Bundling miyo_cli"
        )
    endif()

    # Icon (.ico for Windows)
    set(APP_ICON_WIN "${CMAKE_SOURCE_DIR}/resources/abiwa.ico")
    if(EXISTS "${APP_ICON_WIN}")
        # Windows icon is embedded via .rc file (see below)
    endif()

    # Bundle Python (run bundle_python_win.bat post-build)
    set(BUNDLE_PY_WIN "${CMAKE_SOURCE_DIR}/scripts/bundle_python_win.bat")
    if(EXISTS "${BUNDLE_PY_WIN}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND cmd /c "${BUNDLE_PY_WIN}" "${WIN_OUTPUT_DIR}"
            COMMENT "Bundling Python for Windows"
        )
    endif()

    # Windows RC file for icon
    set(RC_FILE "${CMAKE_SOURCE_DIR}/resources/abiwa.rc")
    if(EXISTS "${RC_FILE}")
        target_sources(Miyo PRIVATE "${RC_FILE}")
    endif()

    # windeployqt: deploy all Qt DLLs and plugins
    find_program(WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(WINDEPLOYQT)
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND "${WINDEPLOYQT}" --no-translations "$<TARGET_FILE:Miyo>"
            COMMENT "Running windeployqt (bundling Qt DLLs)"
        )
    endif()
endif()
