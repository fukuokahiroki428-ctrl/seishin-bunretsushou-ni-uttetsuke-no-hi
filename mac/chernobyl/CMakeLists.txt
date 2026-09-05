cmake_minimum_required(VERSION 3.20)
# ★ 버전 단일 출처 — 레포 루트의 VERSION 파일 하나만 고치면 exe 버전 리소스,
#   Info.plist, 설치본 버전이 전부 따라온다. 예전엔 .iss/.rc/CMake 가 제각각이라
#   설치본은 1.0, exe 는 3.0, 릴리즈는 3.9 로 표시돼 어느 빌드인지 알 수 없었다.
# ★ file(READ) 는 재구성 의존성을 만들지 않는다 — VERSION 만 올리고 빌드하면
#   옛 버전이 박힌 바이너리가 조용히 나온다. 명시적으로 의존성을 건다.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../VERSION")
file(READ "${CMAKE_CURRENT_LIST_DIR}/../../VERSION" PREDORMITION_VERSION)
string(STRIP "${PREDORMITION_VERSION}" PREDORMITION_VERSION)
project(Hanishiki VERSION ${PREDORMITION_VERSION} LANGUAGES CXX)

# ── 판 이름(코드네임) ──────────────────────────────────────────────────────
#   사용자에게 보이는 이름은 '上野' 다. 숫자(VERSION)는 기계용으로만 남긴다.
#   왜 나누나:
#     - CMake 의 project(VERSION) 은 숫자만 받는다. 上野 를 넣으면 configure 가 실패한다.
#     - macOS 는 CFBundleShortVersionString 을 숫자로 비교해 판올림을 판단한다.
#       문자열이면 업데이트 감지·Gatekeeper 가 어긋난다.
#     - GitHub 릴리즈 자산 이름은 ASCII 만 허용된다.
#   그래서 숫자는 조용히 두고, 창 제목·정보 화면에는 이름을 쓴다.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/../../CODENAME")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../../CODENAME")
    file(READ "${CMAKE_CURRENT_LIST_DIR}/../../CODENAME" PREDORMITION_CODENAME)
    string(STRIP "${PREDORMITION_CODENAME}" PREDORMITION_CODENAME)
else()
    set(PREDORMITION_CODENAME "")
endif()
message(STATUS "판 이름: ${PREDORMITION_CODENAME} (내부 ${PREDORMITION_VERSION})")

# ── 만든 곳 ────────────────────────────────────────────────────────────────
#   여기 한 줄이 단일 출처다. 앱 정보 화면·Info.plist(저작권/정보 문자열)가
#   모두 이 값을 쓴다. 두 군데에 적으면 한쪽만 바뀌는 일이 또 생긴다.
set(APP_VENDOR "ishikawa-Illusion")

add_compile_definitions(
    PREDORMITION_CODENAME="${PREDORMITION_CODENAME}"
    PREDORMITION_VERSION_STR="${PREDORMITION_VERSION}"
    APP_VENDOR_NAME="${APP_VENDOR}")

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
    src/core/HanishikiBackend.cpp
    src/core/PenBackend.cpp
    src/core/Common.cpp
    src/core/Config.cpp
)

set(CORE_HEADERS
    src/core/MainWindow.h
    src/core/HanishikiBackend.h
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
    src/platforms/BlueskyCollector.cpp
    src/platforms/TwitterCollector.cpp
    src/platforms/SiteCrawler.cpp
    src/platforms/RealChromeCrawler.cpp
    src/platforms/PenChromeCrawler.cpp
)

set(PLATFORM_HEADERS
    src/platforms/BlueskyCollector.h
    src/platforms/TwitterCollector.h
    src/platforms/SiteCrawler.h
    src/platforms/DiscordError.h
    src/platforms/RealChromeCrawler.h
    src/platforms/PenChromeCrawler.h
)

# Resources
set(RESOURCES resources/hanishiki.qrc)

# ═══════════════════════════════════════════
# Platform-specific executable setup
# ═══════════════════════════════════════════

if(APPLE)
    # macOS bundle settings
    # ★ 두 값은 쓰임이 다르다. 예전에 나는 이 둘을 뭉뚱그려 "숫자를 바꾸면 판올림
    #   감지가 깨진다" 고 적었는데, 정확하지 않았다.
    #     CFBundleVersion             — 기계용. 같은 번들 ID 가 여러 벌 있을 때
    #                                   Launch Services 가 이 값으로 어느 것을 쓸지 고른다.
    #                                   그러니 반드시 숫자로 둔다.
    #     CFBundleShortVersionString  — 사람용. Finder 의 '정보 가져오기' 에 그대로 뜬다.
    #                                   비교에 쓰이지 않으므로 판 이름(한자)을 넣어도 된다.
    #   이 앱의 판 이름은 上野 다. 사람이 보는 자리에는 그 이름이 뜨는 것이 맞다.
    set(MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    if(PREDORMITION_CODENAME)
        set(MACOSX_BUNDLE_SHORT_VERSION_STRING "${PREDORMITION_CODENAME}")
    else()
        set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})
    endif()

    add_executable(Hanishiki MACOSX_BUNDLE
        src/main.cpp
        ${CORE_SOURCES} ${CORE_HEADERS}
        ${UTILS_SOURCES} ${UTILS_HEADERS}
        ${PLATFORM_SOURCES} ${PLATFORM_HEADERS}
        ${RESOURCES}
    )
elseif(WIN32)
    # Windows: WIN32 flag for GUI app (no console window)
    add_executable(Hanishiki WIN32
        src/main.cpp
        ${CORE_SOURCES} ${CORE_HEADERS}
        ${UTILS_SOURCES} ${UTILS_HEADERS}
        ${PLATFORM_SOURCES} ${PLATFORM_HEADERS}
        ${RESOURCES}
    )
else()
    add_executable(Hanishiki
        src/main.cpp
        ${CORE_SOURCES} ${CORE_HEADERS}
        ${UTILS_SOURCES} ${UTILS_HEADERS}
        ${PLATFORM_SOURCES} ${PLATFORM_HEADERS}
        ${RESOURCES}
    )
endif()

target_include_directories(Hanishiki PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/src/platforms
)

# ═══════════════════════════════════════════
# Platform-specific linking
# ═══════════════════════════════════════════

target_link_libraries(Hanishiki PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::WebEngineWidgets
    Qt6::WebChannel
    Qt6::Network
    Qt6::WebSockets
    QXlsx::QXlsx
)

if(APPLE)
    target_link_libraries(Hanishiki PRIVATE "-framework IOKit")
elseif(WIN32)
    target_link_libraries(Hanishiki PRIVATE dwmapi)
endif()

# ═══════════════════════════════════════════
# macOS-specific post-build steps
# ═══════════════════════════════════════════

if(APPLE)
    # App name / icon / identifier — override with -DAPP_NAME="..." -DAPP_ICON="..." -DAPP_ID="..."
    #
    #   ★ APP_NAME(ASCII)은 '실행 파일 이름' 과 '사용자 데이터 폴더 이름' 을 정한다.
    #     - 실행 파일: kill_app.sh 의 pgrep -x 가 이 이름으로 프로세스를 찾는다.
    #     - 데이터 폴더: app.setApplicationName(ASCII) → ~/…/Hanishiki/<APP_NAME>.
    #       여기를 바꾸면 설정·수집물이 갈라진다(실제로 잃은 적이 있다). 건드리지 말 것.
    #     .app 폴더 이름은 build.sh 가 codesign 직전에 APP_DISPLAY_NAME 으로 바꾼다.
    #
    #   ★ 옛 주석 정정(실측으로 확인함):
    #     - "Finder 에 CFBundleDisplayName 이 뜬다" → 틀렸다. Finder 는 .app 파일명을
    #       그대로 보여준다(kMDItemDisplayName). 메뉴 막대·Dock 만 표시 이름을 쓴다.
    #     - "비ASCII .app 이름은 codesign 이 실패한다" → 틀렸다. ハンイシキ.app 으로
    #       서명·--deep --strict 검증·실행까지 모두 통과하는 것을 확인했다.
    if(NOT APP_NAME)
        set(APP_NAME "Hanishiki")
    endif()
    if(NOT APP_DISPLAY_NAME)
        set(APP_DISPLAY_NAME "ハンイシキ")
    endif()
    if(NOT APP_ICON)
        set(APP_ICON "app.icns")
    endif()
    if(NOT APP_ID)
        # ★ 번들 ID 는 macOS 가 이 앱을 알아보는 이름이다. 디스크·자동화·화면녹화
        #   같은 허가가 전부 이 값에 매여 있어서, 바꾸면 허가가 초기화되고 사용자가
        #   다시 허용해야 한다. 그래서 이름이 네 번 바뀌는 동안에도 com.miyo.app 에
        #   머물러 있었다. 2026-08-25 사용자 결정으로 이름을 맞춘다.
        #   (옛 ID 로 남은 허가 항목은 시스템 설정에 따로 남을 수 있다)
        set(APP_ID "com.hanishiki.app")
    endif()

    # ★ 앱 이름을 코드에 박지 않는다 — 지금까지 main.cpp·백엔드·설치기에 각각
    #   "Predormition" 이 흩어져 있어서, 이름을 바꿀 때마다 일부만 따라오고
    #   나머지는 옛 이름을 가리켰다(데이터 폴더가 갈라지는 원인).
    target_compile_definitions(Hanishiki PRIVATE
        APP_NAME_ASCII="${APP_NAME}"
        APP_NAME_DISPLAY="${APP_DISPLAY_NAME}")

    set_target_properties(Hanishiki PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/Info.plist.in
        MACOSX_BUNDLE_ICON_FILE "${APP_ICON}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${APP_ID}"
        MACOSX_BUNDLE_BUNDLE_NAME "${APP_DISPLAY_NAME}"
        # ★ 이 두 값은 Info.plist.in 의 ${MACOSX_BUNDLE_COPYRIGHT} /
        #   ${MACOSX_BUNDLE_INFO_STRING} 로 치환된다(CMake 가 대상 속성에서 읽는다).
        #   Finder 의 '정보 가져오기' 와 앱 정보 화면에 만든 곳이 보이게 하는 자리.
        MACOSX_BUNDLE_COPYRIGHT "${APP_VENDOR}"
        MACOSX_BUNDLE_INFO_STRING "${APP_DISPLAY_NAME} ${PREDORMITION_CODENAME} — ${APP_VENDOR}"
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
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -f "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/MacOS/yt-dlp"
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/tools"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${YTDLP_PATH}" "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/tools/yt-dlp"
            COMMAND chmod +x "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/tools/yt-dlp"
            COMMENT "Bundling yt-dlp wrapper (Resources/tools)"
        )
    endif()

    if(EXISTS "${FFMPEG_PATH}")
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFMPEG_PATH}" "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/MacOS/ffmpeg"
            COMMAND chmod +x "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/MacOS/ffmpeg"
            COMMENT "Bundling ffmpeg"
        )
    endif()

    if(EXISTS "${FFPROBE_PATH}")
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${FFPROBE_PATH}" "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/MacOS/ffprobe"
            COMMAND chmod +x "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/MacOS/ffprobe"
            COMMENT "Bundling ffprobe"
        )
    endif()

    # ★ deno — yt-dlp 2025+ 가 YouTube nsig/서명 해독에 쓰는 JS 런타임.
    #   없으면 시스템 deno 로 폴백(없으면 yt-dlp 내장 JS, 덜 안정). MacOS/ 에 두면 yt-dlp 가 PATH 로 찾음.
    set(DENO_PATH "${CMAKE_SOURCE_DIR}/resources/tools/deno")
    if(EXISTS "${DENO_PATH}")
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${DENO_PATH}" "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/MacOS/deno"
            COMMAND chmod +x "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/MacOS/deno"
            COMMENT "Bundling deno (yt-dlp JS runtime)"
        )
    endif()

    # ★ rclone — WebDAV/SFTP/S3/Mountain Duck 호환 (50+ protocol, MIT 라이센스)
    set(RCLONE_PATH "${CMAKE_SOURCE_DIR}/resources/tools/rclone")
    if(EXISTS "${RCLONE_PATH}")
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/tools"
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${RCLONE_PATH}" "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/tools/rclone"
            COMMAND chmod +x "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/tools/rclone"
            COMMENT "Bundling rclone (WebDAV/SFTP/Mountain Duck 호환 fast transfer)"
        )
    endif()

    # ★ HTML 외부화 — AUTORCC 의존성 추적 깨졌을 때 즉시 반영 위해 별도 복사
    add_custom_command(TARGET Hanishiki POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources/html"
            "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/html"
        COMMENT "Bundling HTML (external — bypasses AUTORCC cache issues)"
    )

    # ★ Chromium 번들 — Chrome for Testing (~340MB, BSD)
    #   사용자 시스템 Chrome 의존성 제거. 캡쳐 전용 Chromium 자체 동봉.
    set(CHROMIUM_SRC "${CMAKE_SOURCE_DIR}/resources/chromium/Chromium.app")
    # ★ 없으면 조용히 건너뛰지 않는다.
    #   예전엔 if(EXISTS) 가 거짓이면 아무 말이 없어서, 번들에 Chromium 이 들어간 줄
    #   알고 넘어갔다. 실제로는 빠진 채 배포됐고 앱은 시스템 Chrome 으로 떨어졌다.
    #   크롬이 없는 맥에서는 캡쳐가 통째로 안 된다. 빠졌으면 반드시 알린다.
    if(NOT EXISTS "${CHROMIUM_SRC}")
        message(WARNING
            "번들 Chromium 이 없습니다: ${CHROMIUM_SRC}\n"
            "   → 캡쳐 기능이 사용자의 시스템 Chrome 에 의존하게 됩니다"
            "(크롬이 없는 맥에서는 동작하지 않습니다).\n"
            "   README 의 '번들 Chromium 넣기' 를 보고 내려받아 두십시오.")
    endif()
    if(EXISTS "${CHROMIUM_SRC}")
        # ★ cmake -E copy_directory 를 쓰면 안 된다 — 심볼릭 링크를 따라가 실체를
        #   복제한다. Chromium.app 은 프레임워크 링크(Versions/Current 등) 5개를 갖고
        #   있어서, 그렇게 복사하면 356MB 가 1.0GB 로 불어난다(실측). 게다가 링크가
        #   사라지면 프레임워크 구조가 깨져 codesign 이 그 프레임워크를 서명하지 못한다.
        #   cp -Rp 로 링크와 권한을 그대로 옮긴다.
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E rm -rf
                "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/chromium/Chromium.app"
            COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/chromium"
            COMMAND cp -Rp "${CHROMIUM_SRC}"
                "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/chromium/Chromium.app"
            COMMENT "Bundling Chromium (심볼릭 링크 보존)"
        )
    endif()

    # ★ 자동 재서명 도구 — 앱이 스스로 봉인을 복구할 때 쓴다.
    #   앱 내부(번들)에 모듈/스크립트를 설치하면 codesign 봉인이 깨진다. 예전에는
    #   런타임이 `codesign -f -s <id> --deep` 로 다시 서명했는데, 이 저장소의 빌드는
    #   --deep 서명을 일부러 피하고 inside-out 으로 서명한다(애플도 --deep 은 배포용이
    #   아니라고 한다). 두 방식이 갈려 있으면 런타임 재서명만 미묘하게 다른 결과를 낸다.
    #   → 빌드가 쓰는 바로 그 스크립트를 번들에 넣고, 런타임도 같은 것을 부른다.
    foreach(_sf codesign_app.sh hanishiki.entitlements)
        if(EXISTS "${CMAKE_SOURCE_DIR}/${_sf}")
            add_custom_command(TARGET Hanishiki POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/${_sf}"
                    "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/tools/${_sf}"
                COMMENT "Bundling ${_sf} (자동 재서명용)"
            )
        else()
            message(WARNING "자동 재서명 도구가 없습니다: ${_sf} — 앱이 스스로 서명을 복구하지 못합니다.")
        endif()
    endforeach()

    # exiftool (프로젝트 내 Perl script + libs → Resources/tools/exiftool/)
    set(EXIFTOOL_SRC "${CMAKE_SOURCE_DIR}/resources/tools/exiftool")
    if(EXISTS "${EXIFTOOL_SRC}/exiftool")
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${EXIFTOOL_SRC}"
                "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/tools/exiftool"
            COMMAND chmod +x "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/tools/exiftool/exiftool"
            COMMENT "Bundling exiftool + Perl libs"
        )
    endif()

    # Python tool scripts
    set(TOOLS_DIR "${CMAKE_SOURCE_DIR}/resources/tools")
    add_custom_command(TARGET Hanishiki POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/tools"
        COMMENT "Creating tools directory"
    )
    # ★ 새 도구 스크립트를 만들면 반드시 여기에도 넣어야 한다. 안 넣으면 개발 트리에서만
    #   돌고 배포본에는 없어서, '내 기계에선 되는데' 가 된다.
    foreach(TOOL_SCRIPT twitter_tid.py twitter_api.py twitter_daemon.py bluesky_daemon.py
                        email_watch.py socks_relay.py x_session.py exif_tags.py)
        if(EXISTS "${TOOLS_DIR}/${TOOL_SCRIPT}")
            add_custom_command(TARGET Hanishiki POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${TOOLS_DIR}/${TOOL_SCRIPT}" "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/tools/${TOOL_SCRIPT}"
                COMMENT "Bundling ${TOOL_SCRIPT}"
            )
        endif()
    endforeach()

    # SingleFile Chrome 확장 — 진짜 페이지 캡쳐 시 인라인 저장에 사용
    if(EXISTS "${TOOLS_DIR}/singlefile_extension")
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${TOOLS_DIR}/singlefile_extension"
                "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/tools/singlefile_extension"
            COMMENT "Bundling SingleFile extension"
        )
    endif()

    # Bundle Python venv
    add_custom_command(TARGET Hanishiki POST_BUILD
        COMMAND bash "${CMAKE_SOURCE_DIR}/scripts/bundle_python.sh" "$<TARGET_BUNDLE_DIR:Hanishiki>"
        COMMENT "Bundling Python venv (twikit, httpx, atproto)"
    )


    # Fix QtWebEngineProcess
    add_custom_command(TARGET Hanishiki POST_BUILD
        COMMAND bash "${CMAKE_SOURCE_DIR}/fix_webengine_helper.sh" "$<TARGET_BUNDLE_DIR:Hanishiki>"
        COMMENT "Fixing QtWebEngineProcess library paths"
    )

    # Icon — APP_ICON_PATH can be overridden via -DAPP_ICON_PATH="..."
    if(NOT APP_ICON_PATH)
        # APP_ICON 과 같은 파일을 가리킨다 — 둘이 어긋나면 아이콘이 조용히 안 바뀐다
        # (Info.plist 는 새 이름을 가리키는데 번들에는 옛 파일만 들어가는 식).
        set(APP_ICON_PATH "${CMAKE_SOURCE_DIR}/resources/${APP_ICON}")
    endif()
    if(NOT EXISTS "${APP_ICON_PATH}")
        message(WARNING "아이콘 파일이 없습니다: ${APP_ICON_PATH} — 기본 아이콘으로 나갑니다")
    endif()
    if(EXISTS "${APP_ICON_PATH}")
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${APP_ICON_PATH}" "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/${APP_ICON}"
            COMMENT "Bundling app icon: ${APP_ICON}"
        )
    endif()

    # Bundle fonts
    add_custom_command(TARGET Hanishiki POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/fonts"
    )
    file(GLOB FONT_FILES "${CMAKE_SOURCE_DIR}/resources/fonts/*.ttf")
    foreach(FONT ${FONT_FILES})
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FONT}"
                "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/fonts/"
            COMMENT "Bundling font: ${FONT}"
        )
    endforeach()

    # ★ Companion PyInstaller apps (anipo, AINU)는 미사용 + codesign sealed resource missing 원인 →
    #   bundle 463MB 차지 + macOS launch SIGKILL 일으킴. 빌드에서 제외.
    #   필요해지면 별도 외부 폴더에서 실행하면 됨.

    # miyo_cli

    # ★ requirements.txt 번들 — Python 업그레이드 시 이 파일을 읽어 패키지를 다시 깐다.
    #   없으면 코드 안 최소 목록으로 폴백하지만 버전 고정이 사라지므로 반드시 넣는다.
    add_custom_command(TARGET Hanishiki POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CMAKE_SOURCE_DIR}/../../requirements.txt"
            "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/requirements.txt"
        COMMENT "Bundling requirements.txt"
    )

    # ★ 보관함 질의응답 스크립트 — 색인/질문/디스코드 부품.
    #   저장소에만 있으면 배포본 사용자는 쓸 수 없다.
    add_custom_command(TARGET Hanishiki POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
            "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/tools/archive"
        COMMENT "Bundling archive scripts"
    )
    foreach(_af archive_index.py archive_ask.py archive_bot.py archive_cog.py README_archive.md)
        if(EXISTS "${CMAKE_SOURCE_DIR}/../../scripts/${_af}")
            add_custom_command(TARGET Hanishiki POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${CMAKE_SOURCE_DIR}/../../scripts/${_af}"
                    "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Resources/tools/archive/${_af}"
            )
        endif()
    endforeach()

    # ★ Qt 프레임워크 번들링 — 반드시 codesign 앞.
    #   이게 없으면 실행 파일이 /opt/homebrew/opt/qt*/lib/... 를 직접 참조해서,
    #   홈브루 Qt 가 없는 맥에서는 앱이 아예 켜지지 않는다.
    #   (v3.9.3 까지 배포한 DMG 가 실제로 그 상태였다 — 이 맥에서만 돌아갔다.)
    #   macdeployqt 가 프레임워크를 Contents/Frameworks 로 복사하고 install name 을
    #   @rpath 로 바꿔 준다. 서명은 우리 스크립트가 뒤에서 다시 한다(-no-strip 로
    #   서명 훼손 방지, -always-overwrite 로 옛 사본이 남지 않게).
    find_program(MACDEPLOYQT_BIN macdeployqt
        HINTS "${Qt6_DIR}/../../../bin" "/opt/homebrew/bin" "/usr/local/bin")
    if(MACDEPLOYQT_BIN)
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND "${MACDEPLOYQT_BIN}" "$<TARGET_BUNDLE_DIR:Hanishiki>"
                    -always-overwrite -no-strip
            COMMENT "Bundling Qt frameworks (macdeployqt)"
        )
    else()
        message(WARNING "macdeployqt 를 찾지 못했습니다 — 이 빌드는 시스템 Qt 에 의존합니다(다른 맥에서 실행 불가).")
    endif()

    # ★ macdeployqt 가 못 고친 외부 경로 참조를 @rpath 로 마무리.
    #   이 번들엔 python_env 안에 파이썬 확장(.so)이 수천 개라 macdeployqt 가
    #   그걸 훑다 rpath 해석 오류를 내고 일부 참조를 끝까지 못 바꾼다.
    #   그러면 프레임워크는 번들에 있는데 서로를 /opt/homebrew 로 참조해서
    #   Homebrew 없는 맥에서 앱이 안 켜진다. 반드시 codesign 앞에서 실행한다
    #   (install_name_tool 이 서명을 깨뜨리므로).
    add_custom_command(TARGET Hanishiki POST_BUILD
        COMMAND bash "${CMAKE_SOURCE_DIR}/fix_bundle_rpaths.sh" "$<TARGET_BUNDLE_DIR:Hanishiki>"
        COMMENT "Rewriting external paths to @rpath (self-contained check)"
    )

    # ★ Codesign 직전: fix_webengine_helper.sh가 만든 잘못된 심볼릭 링크 정리
    #   "Frameworks/Frameworks -> ../../../../../Frameworks" 같은 dangling symlink가
    #   codesign sealed resource verify를 깨뜨려 macOS가 SIGKILL로 앱 죽임.
    add_custom_command(TARGET Hanishiki POST_BUILD
        COMMAND find "$<TARGET_BUNDLE_DIR:Hanishiki>/Contents/Frameworks" -maxdepth 1 -type l -delete 2>/dev/null || true
        COMMENT "Cleaning broken Frameworks symlinks before codesign"
    )

    # Codesign
    add_custom_command(TARGET Hanishiki POST_BUILD
        COMMAND bash "${CMAKE_SOURCE_DIR}/codesign_app.sh" "$<TARGET_BUNDLE_DIR:Hanishiki>"
        COMMENT "Codesigning entire app bundle"
    )

    # ★ 자원 동기화 — 위 자원 복사 POST_BUILD 들도 '타겟이 다시 링크될 때만' 돈다.
    #   그래서 C++ 을 안 건드리고 자원만 바꾸면(도구 교체·HTML 수정·스크립트 수정)
    #   빌드가 조용히 옛것을 그대로 둔다. 실제로 ffprobe 를 arm64 로 갈아 끼우고
    #   빌드했는데 번들엔 옛 x86_64 가 남았다("Bundling" 줄이 한 줄도 안 찍혔다).
    #   항상 도는 이 타겟이 소스와 번들을 다시 맞춘다. build.sh 도 같은 스크립트를
    #   부르므로, cmake 를 직접 부르는 경로(IDE·CI)에서도 똑같이 보호된다.
    #   반드시 codesign_verify 앞에서 — 자원이 바뀌면 서명이 어긋나기 때문이다.
    add_custom_target(sync_resources ALL
        COMMAND bash "${CMAKE_SOURCE_DIR}/sync_resources.sh" "$<TARGET_BUNDLE_DIR:Hanishiki>"
        COMMENT "Syncing bundled resources from source (always runs)"
        VERBATIM
    )
    add_dependencies(sync_resources Hanishiki)

    # ★ 서명 정합성 보증 — 위 POST_BUILD 는 '타겟이 다시 링크될 때만' 돈다.
    #   Info.plist 는 cmake 재구성만으로도 새로 생성되므로(예: VERSION 만 변경),
    #   그때는 링크가 없어 서명 단계가 건너뛰어지고 서명이 plist 와 어긋난 채 남는다.
    #   → macOS 가 앱 실행을 거부한다("invalid Info.plist" / Launch failed).
    #   항상 도는 이 타겟이 확인하고, 어긋났을 때만 다시 서명한다.
    add_custom_target(codesign_verify ALL
        COMMAND bash "${CMAKE_SOURCE_DIR}/verify_or_resign.sh" "$<TARGET_BUNDLE_DIR:Hanishiki>"
        COMMENT "Verifying code signature (re-signs only when stale)"
        VERBATIM
    )
    #   자원 동기화가 먼저 끝나야 한다(자원이 바뀐 뒤에 서명 정합성을 봐야 의미가 있다).
    add_dependencies(codesign_verify Hanishiki sync_resources)
endif()

# ═══════════════════════════════════════════
# Windows-specific post-build steps
# ═══════════════════════════════════════════

if(WIN32)
    # ★ APP_NAME 미설정 시 Chernobyl 기본값 (이전 "ABIWA" 하드코딩 → -DAPP_NAME 무시됐음)
    if(NOT APP_NAME)
        set(APP_NAME "Predormition")
    endif()
    set_target_properties(Hanishiki PROPERTIES
        OUTPUT_NAME "${APP_NAME}"
        WIN32_EXECUTABLE TRUE
    )

    set(WIN_OUTPUT_DIR "$<TARGET_FILE_DIR:Hanishiki>")

    # Python tool scripts → <exe_dir>/tools/
    set(TOOLS_DIR "${CMAKE_SOURCE_DIR}/resources/tools")
    add_custom_command(TARGET Hanishiki POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WIN_OUTPUT_DIR}/tools"
        COMMENT "Creating tools directory"
    )
    # ★ 새 도구 스크립트를 만들면 반드시 여기에도 넣어야 한다. 안 넣으면 개발 트리에서만
    #   돌고 배포본에는 없어서, '내 기계에선 되는데' 가 된다.
    foreach(TOOL_SCRIPT twitter_tid.py twitter_api.py twitter_daemon.py bluesky_daemon.py
                        email_watch.py socks_relay.py x_session.py exif_tags.py)
        if(EXISTS "${TOOLS_DIR}/${TOOL_SCRIPT}")
            add_custom_command(TARGET Hanishiki POST_BUILD
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
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${YTDLP_WIN}" "${WIN_OUTPUT_DIR}/yt-dlp.exe"
            COMMENT "Bundling yt-dlp.exe"
        )
    endif()
    if(EXISTS "${FFMPEG_WIN}")
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FFMPEG_WIN}" "${WIN_OUTPUT_DIR}/ffmpeg.exe"
            COMMENT "Bundling ffmpeg.exe"
        )
    endif()
    if(EXISTS "${FFPROBE_WIN}")
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FFPROBE_WIN}" "${WIN_OUTPUT_DIR}/ffprobe.exe"
            COMMENT "Bundling ffprobe.exe"
        )
    endif()

    # Fonts → <exe_dir>/fonts/
    add_custom_command(TARGET Hanishiki POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WIN_OUTPUT_DIR}/fonts"
    )
    file(GLOB FONT_FILES "${CMAKE_SOURCE_DIR}/resources/fonts/*.ttf")
    foreach(FONT ${FONT_FILES})
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FONT}" "${WIN_OUTPUT_DIR}/fonts/"
            COMMENT "Bundling font: ${FONT}"
        )
    endforeach()

    # Chernobyl.png
    set(CHERNOBYL_PATH "${CMAKE_SOURCE_DIR}/../Chernobyl.png")
    if(EXISTS "${CHERNOBYL_PATH}")
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CHERNOBYL_PATH}" "${WIN_OUTPUT_DIR}/Chernobyl.png"
            COMMENT "Bundling Chernobyl.png"
        )
    endif()

    # exiftool.exe → <exe_dir>/
    set(EXIFTOOL_WIN "${CMAKE_SOURCE_DIR}/resources/tools/exiftool.exe")
    if(EXISTS "${EXIFTOOL_WIN}")
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${EXIFTOOL_WIN}" "${WIN_OUTPUT_DIR}/exiftool.exe"
            COMMENT "Bundling exiftool.exe"
        )
    endif()

    # miyo_cli → <exe_dir>/miyo_cli/
    set(MIYO_CLI_WIN "${CMAKE_SOURCE_DIR}/../miyo_cli")
    if(EXISTS "${MIYO_CLI_WIN}")
        add_custom_command(TARGET Hanishiki POST_BUILD
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
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND cmd /c call "${BUNDLE_PY_WIN}" "${WIN_OUTPUT_DIR}"
            COMMENT "Bundling Python for Windows"
        )
    endif()

    # Windows RC file for icon
    #   ★ 아이콘이 없으면 .rc 를 컴파일하지 않는다 — abiwa.rc 는 abiwa.ico 를 참조하는데
    #     이 트리엔 .ico 가 없어서, 그대로 넘기면 RC2135 로 빌드가 죽는다.
    set(RC_FILE "${CMAKE_SOURCE_DIR}/resources/abiwa.rc")
    if(EXISTS "${RC_FILE}" AND EXISTS "${CMAKE_SOURCE_DIR}/resources/abiwa.ico")
        target_sources(Hanishiki PRIVATE "${RC_FILE}")
    endif()

    # windeployqt: deploy all Qt DLLs and plugins
    find_program(WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(WINDEPLOYQT)
        add_custom_command(TARGET Hanishiki POST_BUILD
            COMMAND "${WINDEPLOYQT}" --no-translations "$<TARGET_FILE:Hanishiki>"
            COMMENT "Running windeployqt (bundling Qt DLLs)"
        )
    endif()
endif()
