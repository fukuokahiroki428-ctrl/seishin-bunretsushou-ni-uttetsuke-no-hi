cmake_minimum_required(VERSION 3.20)
# ★ 버전 단일 출처 — 레포 루트의 VERSION 파일 하나만 고치면 exe 버전 리소스,
#   Info.plist, 설치본 버전이 전부 따라온다. 예전엔 .iss/.rc/CMake 가 제각각이라
#   설치본은 1.0, exe 는 3.0, 릴리즈는 3.9 로 표시돼 어느 빌드인지 알 수 없었다.
# ★ file(READ) 는 재구성 의존성을 만들지 않는다 — VERSION 만 올리고 빌드하면
#   옛 버전이 박힌 바이너리가 조용히 나온다. 명시적으로 의존성을 건다.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../VERSION")
file(READ "${CMAKE_CURRENT_LIST_DIR}/../VERSION" PREDORMITION_VERSION)
string(STRIP "${PREDORMITION_VERSION}" PREDORMITION_VERSION)
project(Miyo VERSION ${PREDORMITION_VERSION} LANGUAGES CXX)

# ★ UTF-8 소스 — MSVC 가 narrow/wide 문자열 리터럴(일본어 カメラ, 한글)을 올바르게 컴파일.
#   이게 없으면 시스템 ACP 로 해석돼 제목/메뉴가 깨짐.
if(MSVC)
    add_compile_options(/utf-8)
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Qt6
# ★ 최소 버전 명시 — 낮은 Qt 로 빌드하면 링크 단계에서야 알 수 없는 오류가 나서 원인 찾기 어렵다.
#   6.7 은 WebEngine/WebChannel API 가 현재 코드와 맞는 하한선이다.
find_package(Qt6 6.7 REQUIRED COMPONENTS
    Core Gui Widgets WebEngineWidgets WebChannel Network WebSockets)
message(STATUS "Qt ${Qt6_VERSION} 사용")

# QXlsx 는 Qt6::GuiPrivate(QZipReader/QZipWriter, QtCore private)를 링크한다.
#   mac/공식 Qt 는 private 를 포함하지만, CI 의 aqt(install-qt-action) Qt 6.7.3 는 Qt 모듈
#   private(헤더+CMake config)를 담지 않는다(Freetype/Png 등 3rd-party private 만 있음).
#   → 실제 GuiPrivate 가 있으면(mac 등) 그걸 쓰고, 없으면(CI) QZip 헤더 2개만 vendoring 한
#     shim 타깃을 만든다. QZipReader/QZipWriter 구현은 Qt6Core.dll 에 Q_CORE_EXPORT 로 존재하므로
#     헤더(선언) + Qt6::Core 링크만으로 해결된다. (third_party/qtzip/private/*.h)
find_package(Qt6 QUIET OPTIONAL_COMPONENTS GuiPrivate)
if(NOT TARGET Qt6::GuiPrivate)
    message(STATUS "Qt6::GuiPrivate 없음(aqt) → vendored QZip shim 사용: ${CMAKE_CURRENT_SOURCE_DIR}/third_party/qtzip")
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
    src/core/TerminalWindow.cpp
    src/core/MiyoBackend.cpp
    src/core/PenBackend.cpp
    src/core/Common.cpp
    src/core/Config.cpp
)

set(CORE_HEADERS
    src/core/MainWindow.h
    src/core/TerminalWindow.h
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
    # ★ 새 도구 스크립트를 만들면 반드시 여기에도 넣어야 한다. 안 넣으면 개발 트리에서만
    #   돌고 배포본에는 없어서, '내 기계에선 되는데' 가 된다.
    foreach(TOOL_SCRIPT twitter_tid.py twitter_api.py twitter_daemon.py bluesky_daemon.py
                        email_watch.py x_session.py)
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
        set(APP_NAME "Predormition")
    endif()
    set_target_properties(Miyo PROPERTIES
        OUTPUT_NAME "${APP_NAME}"
        WIN32_EXECUTABLE TRUE
    )

    set(WIN_OUTPUT_DIR "$<TARGET_FILE_DIR:Miyo>")

    # ★ UI(html) 를 exe 옆으로 — 이게 없으면 앱이 qrc:/html/index.html 로 폴백하는데,
    #   그때는 WebChannel transport 가 origin 차이로 안 붙어서 "화면은 뜨는데 버튼이
    #   전부 죽는" 상태가 된다 (MainWindow.cpp 112~114 주석이 적어 둔 그 증상).
    #   지금까지는 GitHub Actions 의 Deploy 단계(build.yml:284)만 이 복사를 했다.
    #   그래서 WINDOWS_BUILD.txt 가 안내하는 로컬 cmake 빌드는 만들자마자 깨져 있었다.
    #   맥 분기는 121행에서 이미 같은 복사를 하고 있다 — 윈도우만 빠져 있었다.
    add_custom_command(TARGET Miyo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources/html" "${WIN_OUTPUT_DIR}/html"
        COMMENT "Bundling UI html (없으면 모든 버튼이 죽는다)"
    )

    # requirements.txt — 파이썬 패키지 버전 고정. 없으면 코드 안 최소 목록으로 폴백해
    # 버전 고정이 사라진다 (Common.cpp 의 qWarning 경로).
    # 페이지 통째 캡쳐용 SingleFile 확장 — 맥 분기(297행)에만 있어서 로컬 윈도우 빌드는
    # 이 기능이 조용히 빠졌다. CI 의 Deploy 단계는 넣어 주므로 설치본은 영향 없었다.
    #   실제 파일은 맥 트리에만 있다 — CI 의 Deploy 단계와 같은 폴백을 쓴다.
    set(SF_SRC "${TOOLS_DIR}/singlefile_extension")
    if(NOT EXISTS "${SF_SRC}")
        set(SF_SRC "${CMAKE_SOURCE_DIR}/../mac/chernobyl/resources/tools/singlefile_extension")
    endif()
    if(EXISTS "${SF_SRC}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${SF_SRC}" "${WIN_OUTPUT_DIR}/tools/singlefile_extension"
            COMMENT "Bundling SingleFile extension"
        )
    endif()

    # 보관함 색인·질의응답 스크립트
    foreach(ARCHIVE_SCRIPT archive_index.py archive_ask.py)
        if(EXISTS "${CMAKE_SOURCE_DIR}/../scripts/${ARCHIVE_SCRIPT}")
            add_custom_command(TARGET Miyo POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/../scripts/${ARCHIVE_SCRIPT}"
                    "${WIN_OUTPUT_DIR}/tools/${ARCHIVE_SCRIPT}"
                COMMENT "Bundling ${ARCHIVE_SCRIPT}"
            )
        endif()
    endforeach()

    # ★ 로컬 AI 설치 스크립트 — 앱이 "AI_설치_더블클릭.bat 을 실행하세요" 라고 안내하는데
    #   그 파일이 배포본 어디에도 없었다. 저장소를 clone 할 줄 모르는 사용자에게는
    #   로컬 AI 를 설치할 길이 사실상 없었다.
    #   파일 이름에 한글이 있어 CMake 에 개별 경로로 적으면 시스템 코드페이지로 읽혀
    #   복사가 실패한다(실측: GLOB 도 마찬가지). copy_directory 는 제대로 처리한다.
    #   .bat 은 %~dp0 로 옆의 ps1 을 부르므로 폴더째 두면 그대로 동작한다.
    if(EXISTS "${CMAKE_SOURCE_DIR}/../scripts")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CMAKE_SOURCE_DIR}/../scripts" "${WIN_OUTPUT_DIR}/scripts"
            COMMENT "Bundling scripts (로컬 AI 설치 등)"
        )
    endif()

    if(EXISTS "${CMAKE_SOURCE_DIR}/../requirements.txt")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${CMAKE_SOURCE_DIR}/../requirements.txt" "${WIN_OUTPUT_DIR}/requirements.txt"
            COMMENT "Bundling requirements.txt"
        )
    endif()

    # Python tool scripts → <exe_dir>/tools/
    set(TOOLS_DIR "${CMAKE_SOURCE_DIR}/resources/tools")
    add_custom_command(TARGET Miyo POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WIN_OUTPUT_DIR}/tools"
        COMMENT "Creating tools directory"
    )
    # ★ 새 도구 스크립트를 만들면 반드시 여기에도 넣어야 한다. 안 넣으면 개발 트리에서만
    #   돌고 배포본에는 없어서, '내 기계에선 되는데' 가 된다.
    foreach(TOOL_SCRIPT twitter_tid.py twitter_api.py twitter_daemon.py bluesky_daemon.py
                        email_watch.py x_session.py)
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
    # ★ exiftool.exe 는 PAR 로 묶인 Perl 이라 옆의 exiftool_files\perl5*.dll 이 반드시 있어야
    #   한다. exe 만 복사하면 Common.cpp 의 탐색이 그것을 채택한 뒤 실행에 실패해서,
    #   exiftool 이 아예 없을 때보다 원인 찾기가 어려워진다.
    set(EXIFTOOL_FILES_WIN "${CMAKE_SOURCE_DIR}/resources/tools/exiftool_files")
    if(EXISTS "${EXIFTOOL_FILES_WIN}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${EXIFTOOL_FILES_WIN}" "${WIN_OUTPUT_DIR}/exiftool_files"
            COMMENT "Bundling exiftool_files (Perl 런타임 — 없으면 exiftool 이 안 뜬다)"
        )
    endif()

    # 번들 Chromium — 있으면 exe 옆으로. RealChromeCrawler 가 첫 후보로 여기를 본다.
    set(CHROMIUM_WIN "${CMAKE_SOURCE_DIR}/resources/chromium")
    if(EXISTS "${CHROMIUM_WIN}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${CHROMIUM_WIN}" "${WIN_OUTPUT_DIR}/chromium"
            COMMENT "Bundling Chromium"
        )
    endif()

    # rclone.exe → <exe_dir>/  (WebDAV/SFTP/S3 고속 백업 — 없으면 일반 백업으로 fallback)
    set(RCLONE_WIN "${CMAKE_SOURCE_DIR}/resources/tools/rclone.exe")
    if(EXISTS "${RCLONE_WIN}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RCLONE_WIN}" "${WIN_OUTPUT_DIR}/rclone.exe"
            COMMENT "Bundling rclone.exe (WebDAV/SFTP/S3 fast transfer)"
        )
    endif()

    # deno.exe → <exe_dir>/  (yt-dlp JS 런타임 — YouTube 추출에 필수, PATH 통해 자동 탐지)
    set(DENO_WIN "${CMAKE_SOURCE_DIR}/resources/tools/deno.exe")
    if(EXISTS "${DENO_WIN}")
        add_custom_command(TARGET Miyo POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DENO_WIN}" "${WIN_OUTPUT_DIR}/deno.exe"
            COMMENT "Bundling deno.exe (yt-dlp JS runtime for YouTube)"
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

    # Windows RC file for icon — .rc 는 abiwa.ico 가 있을 때만 컴파일(없으면 RC2135 로 빌드 실패).
    #   아이콘 에셋이 없어도 빌드는 성공하고, 기본 아이콘으로 동작.
    # ★ .rc 는 템플릿에서 생성 — 앱 이름·버전이 CMake(=VERSION 파일)와 항상 일치한다.
    #   예전엔 .rc 를 손으로 관리해 exe 속성이 "カメラ / Chernobyl.exe / 3.0.0.0" 인
    #   채로 배포됐다(설치본·릴리즈 버전과 불일치).
    set(RC_TEMPLATE "${CMAKE_SOURCE_DIR}/resources/abiwa.rc.in")
    set(RC_FILE "${CMAKE_BINARY_DIR}/abiwa.rc")
    if(EXISTS "${RC_TEMPLATE}" AND EXISTS "${APP_ICON_WIN}")
        # RC 의 FILEVERSION 은 쉼표 4자리(a,b,c,0) 형식이어야 한다.
        string(REPLACE "." "," PREDORMITION_VERSION_COMMA "${PREDORMITION_VERSION}")
        set(PREDORMITION_VERSION_COMMA "${PREDORMITION_VERSION_COMMA},0")
        # 아이콘은 .rc 와 같은 폴더 기준으로 찾으므로 생성 위치로 복사.
        configure_file("${APP_ICON_WIN}" "${CMAKE_BINARY_DIR}/abiwa.ico" COPYONLY)
        configure_file("${RC_TEMPLATE}" "${RC_FILE}" @ONLY)
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
