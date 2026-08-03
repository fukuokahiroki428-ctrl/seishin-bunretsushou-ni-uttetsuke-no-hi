cmake_minimum_required(VERSION 3.20)
# ★ 버전 단일 출처 — 레포 루트의 VERSION 파일 하나만 고치면 exe 버전 리소스,
#   Info.plist, 설치본 버전이 전부 따라온다. 예전엔 .iss/.rc/CMake 가 제각각이라
#   설치본은 1.0, exe 는 3.0, 릴리즈는 3.9 로 표시돼 어느 빌드인지 알 수 없었다.
# ★ file(READ) 는 재구성 의존성을 만들지 않는다 — VERSION 만 올리고 빌드하면
#   옛 버전이 박힌 바이너리가 조용히 나온다. 명시적으로 의존성을 건다.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../VERSION")
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../VERSION" PREDORMITION_VERSION)
string(STRIP "${PREDORMITION_VERSION}" PREDORMITION_VERSION)
project(Miyo VERSION ${PREDORMITION_VERSION} LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Qt6
# ★ 최소 버전 명시 — 낮은 Qt 로 빌드하면 링크 단계에서야 알 수 없는 오류가 나서 원인 찾기 어렵다.
find_package(Qt6 6.7 REQUIRED COMPONENTS
    Core Gui Widgets WebEngineWidgets WebChannel Network WebSockets)
message(STATUS "Qt ${Qt6_VERSION} 사용")

# QXlsx 는 Qt6::GuiPrivate(QZipReader/QZipWriter) 를 링크한다. private 모듈은 Qt 가
#   "예고 없이 바뀌거나 사라질 수 있다"고 명시한 영역이라, 필수(REQUIRED)로 걸어두면
#   Qt 를 올리는 순간 빌드가 통째로 죽는다.
#   → 있으면 그대로 쓰고, 없으면 QZip 헤더 2개만 vendoring 한 shim 으로 대체한다.
#     구현은 Qt6Core 에 Q_CORE_EXPORT 로 있으므로 선언 + Qt6::Core 링크면 충분하다.
find_package(Qt6 QUIET OPTIONAL_COMPONENTS GuiPrivate)
if(NOT TARGET Qt6::GuiPrivate)
    message(STATUS "Qt6::GuiPrivate 없음 → vendored QZip shim 사용: ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qtzip")
    add_library(Qt6::GuiPrivate INTERFACE IMPORTED)
    set_target_properties(Qt6::GuiPrivate PROPERTIES
        INTERFACE_INCLUDE_DIRECTORIES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/qtzip"
        INTERFACE_LINK_LIBRARIES "Qt6::Core;Qt6::Gui")
endif()

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
    src/core/PenBackend.cpp
    src/core/Common.cpp
    src/core/Config.cpp
)

set(CORE_HEADERS
    src/core/MainWindow.h
    src/core/MiyoBackend.h
    src/core/PenBackend.h
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
    src/platforms/PenChromeCrawler.cpp
)

set(PLATFORM_HEADERS
    src/platforms/DiscordCollector.h
    src/platforms/BlueskyCollector.h
    src/platforms/InstagramCollector.h
    src/platforms/YouTubeDownloader.h
    src/platforms/TwitterCollector.h
    src/platforms/SiteCrawler.h
    src/platforms/RealChromeCrawler.h
    src/platforms/PenChromeCrawler.h
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
        set(APP_NAME "Predormition")
    endif()
    if(NOT APP_ICON)
        set(APP_ICON "chernobyl.icns")
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
        # ★ yt-dlp 는 셸 래퍼(python -m yt_dlp) → Resources/tools/ 에 둔다.
        #   Contents/MacOS/ 는 Mach-O 전용이라 스크립트를 두면 codesign --deep 이 실패한다.
        #   앱의 findBundledYtDlp 가 MacOS/ 다음 Resources/tools/ 를 확인하므로 정상 발견.
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_BUNDLE_DIR:Miyo>/Contents/MacOS/yt-dlp"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/tools"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${YTDLP_PATH}" "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/tools/yt-dlp"
            COMMAND chmod +x "$<TARGET_BUNDLE_DIR:Miyo>/Contents/Resources/tools/yt-dlp"
            COMMENT "Bundling yt-dlp wrapper (Resources/tools)"
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

    # ★ deno — yt-dlp 2025+ 가 YouTube nsig/서명 해독에 쓰는 JS 런타임.
    #   없으면 시스템 deno 로 폴백(없으면 yt-dlp 내장 JS, 덜 안정). MacOS/ 에 두면 yt-dlp 가 PATH 로 찾음.
    set(DENO_PATH "${CMAKE_SOURCE_DIR}/resources/tools/deno")
    if(EXISTS "${DENO_PATH}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DENO_PATH}" "$<TARGET_BUNDLE_DIR:Miyo>/Contents/MacOS/deno"
            COMMAND chmod +x "$<TARGET_BUNDLE_DIR:Miyo>/Contents/MacOS/deno"
            COMMENT "Bundling deno (yt-dlp JS runtime)"
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
        set(APP_ICON_PATH "${CMAKE_SOURCE_DIR}/resources/chernobyl.icns")
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

    # ★ 서명 정합성 보증 — 위 POST_BUILD 는 '타겟이 다시 링크될 때만' 돈다.
    #   Info.plist 는 cmake 재구성만으로도 새로 생성되므로(예: VERSION 만 변경),
    #   그때는 링크가 없어 서명 단계가 건너뛰어지고 서명이 plist 와 어긋난 채 남는다.
    #   → macOS 가 앱 실행을 거부한다("invalid Info.plist" / Launch failed).
    #   항상 도는 이 타겟이 확인하고, 어긋났을 때만 다시 서명한다.
    add_custom_target(codesign_verify ALL
        COMMAND bash "${CMAKE_SOURCE_DIR}/verify_or_resign.sh" "$<TARGET_BUNDLE_DIR:Miyo>"
        COMMENT "Verifying code signature (re-signs only when stale)"
        VERBATIM
    )
    add_dependencies(codesign_verify Miyo)
endif()

# ═══════════════════════════════════════════
# Windows-specific post-build steps
# ═══════════════════════════════════════════

if(WIN32)
    # ★ APP_NAME 미설정 시 Chernobyl 기본값 (이전 "ABIWA" 하드코딩 → -DAPP_NAME 무시됐음)
    if(NOT APP_NAME)
        set(APP_NAME "Predormition")
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
            COMMAND cmd /c call "${BUNDLE_PY_WIN}" "${WIN_OUTPUT_DIR}"
            COMMENT "Bundling Python for Windows"
        )
    endif()

    # Windows RC file for icon
    #   ★ 아이콘이 없으면 .rc 를 컴파일하지 않는다 — abiwa.rc 는 abiwa.ico 를 참조하는데
    #     이 트리엔 .ico 가 없어서, 그대로 넘기면 RC2135 로 빌드가 죽는다.
    set(RC_FILE "${CMAKE_SOURCE_DIR}/resources/abiwa.rc")
    if(EXISTS "${RC_FILE}" AND EXISTS "${CMAKE_SOURCE_DIR}/resources/abiwa.ico")
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
