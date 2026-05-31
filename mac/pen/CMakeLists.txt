cmake_minimum_required(VERSION 3.20)
project(Pen VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTORCC ON)

# Qt6
find_package(Qt6 REQUIRED COMPONENTS
    Core Widgets WebEngineWidgets WebChannel Network WebSockets)

# 펜은 Excel 기능 없음 — QXlsx 의존성 제거됨

# Sources
set(CORE_SOURCES
    src/core/MainWindow.cpp
    src/core/PenBackend.cpp
    src/core/Common.cpp
    src/core/Config.cpp
)

set(CORE_HEADERS
    src/core/MainWindow.h
    src/core/PenBackend.h
    src/core/WorkerSignals.h
    src/core/Common.h
    src/core/Config.h
)

# 펜은 크롤 전용 — Excel/HTTP/DiskJson/SecurityScanner 안 씀.
# FileHelper만 RealChromeCrawler가 사용 가능성 있어 유지.
set(UTILS_SOURCES
    src/utils/FileHelper.cpp
    src/utils/WebDavUploader.cpp
)

set(UTILS_HEADERS
    src/utils/FileHelper.h
    src/utils/WebDavUploader.h
)

# Platform classes (CDP 크롤러 등)
set(PLATFORM_SOURCES
    src/platforms/RealChromeCrawler.cpp
)
set(PLATFORM_HEADERS
    src/platforms/RealChromeCrawler.h
)

# Resources
set(RESOURCES resources/pen.qrc)

# ═══════════════════════════════════════════
# Platform-specific executable setup
# ═══════════════════════════════════════════

if(APPLE)
    set(MACOSX_BUNDLE_BUNDLE_VERSION ${PROJECT_VERSION})
    set(MACOSX_BUNDLE_SHORT_VERSION_STRING ${PROJECT_VERSION})

    add_executable(Pen MACOSX_BUNDLE
        src/main.cpp
        ${CORE_SOURCES} ${CORE_HEADERS}
        ${UTILS_SOURCES} ${UTILS_HEADERS}
        ${PLATFORM_SOURCES} ${PLATFORM_HEADERS}
        ${RESOURCES}
    )
elseif(WIN32)
    add_executable(Pen WIN32
        src/main.cpp
        ${CORE_SOURCES} ${CORE_HEADERS}
        ${UTILS_SOURCES} ${UTILS_HEADERS}
        ${PLATFORM_SOURCES} ${PLATFORM_HEADERS}
        ${RESOURCES}
    )
else()
    add_executable(Pen
        src/main.cpp
        ${CORE_SOURCES} ${CORE_HEADERS}
        ${UTILS_SOURCES} ${UTILS_HEADERS}
        ${PLATFORM_SOURCES} ${PLATFORM_HEADERS}
        ${RESOURCES}
    )
endif()

target_include_directories(Pen PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/src/core
    ${CMAKE_SOURCE_DIR}/src/utils
    ${CMAKE_SOURCE_DIR}/src/platforms
)

# ═══════════════════════════════════════════
# Linking
# ═══════════════════════════════════════════

target_link_libraries(Pen PRIVATE
    Qt6::Core
    Qt6::Widgets
    Qt6::WebEngineWidgets
    Qt6::WebChannel
    Qt6::Network
    Qt6::WebSockets
)

if(APPLE)
    target_link_libraries(Pen PRIVATE "-framework IOKit")
elseif(WIN32)
    target_link_libraries(Pen PRIVATE dwmapi)
endif()

# ═══════════════════════════════════════════
# macOS-specific post-build steps
# ═══════════════════════════════════════════

if(APPLE)
    if(NOT APP_NAME)
        # ★ ASCII 이름 — macOS codesign NFC/NFD 충돌 회피 + 스페이스 없는 깨끗한 번들 경로.
        #   Display 이름(Dock/Finder 표시)은 Info.plist의 CFBundleDisplayName 로 한국어 유지 가능.
        set(APP_NAME "Pen")
    endif()
    if(NOT APP_ICON)
        set(APP_ICON "beta.icns")
    endif()
    if(NOT APP_ID)
        set(APP_ID "com.pen.app")
    endif()

    set_target_properties(Pen PROPERTIES
        MACOSX_BUNDLE TRUE
        MACOSX_BUNDLE_INFO_PLIST ${CMAKE_SOURCE_DIR}/Info.plist.in
        MACOSX_BUNDLE_ICON_FILE "${APP_ICON}"
        MACOSX_BUNDLE_GUI_IDENTIFIER "${APP_ID}"
        MACOSX_BUNDLE_BUNDLE_NAME "${APP_NAME}"
        OUTPUT_NAME "${APP_NAME}"
    )

    # ★ HTML 외부화 — AUTORCC 의존성 캐시 우회
    add_custom_command(TARGET Pen POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CMAKE_SOURCE_DIR}/resources/html"
            "$<TARGET_BUNDLE_DIR:Pen>/Contents/Resources/html"
        COMMENT "Bundling HTML (external)"
    )

    # ★ Chromium 번들 — Chernobyl 의 resources/chromium/ 공유 사용
    set(SHARED_CHROMIUM "${CMAKE_SOURCE_DIR}/../mac/miyo_cpp/resources/chromium/Chromium.app")
    if(EXISTS "${SHARED_CHROMIUM}")
        add_custom_command(TARGET Pen POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${SHARED_CHROMIUM}"
                "$<TARGET_BUNDLE_DIR:Pen>/Contents/Resources/chromium/Chromium.app"
            COMMENT "Bundling Chromium"
        )
    endif()

    # exiftool
    set(EXIFTOOL_SRC "${CMAKE_SOURCE_DIR}/resources/tools/exiftool")
    if(EXISTS "${EXIFTOOL_SRC}/exiftool")
        add_custom_command(TARGET Pen POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${EXIFTOOL_SRC}"
                "$<TARGET_BUNDLE_DIR:Pen>/Contents/Resources/tools/exiftool"
            COMMAND chmod +x "$<TARGET_BUNDLE_DIR:Pen>/Contents/Resources/tools/exiftool/exiftool"
            COMMENT "Bundling exiftool + Perl libs"
        )
    endif()

    # SingleFile Chrome 확장 (CDP 크롤 시 페이지 인라인 캡쳐용)
    set(SF_EXT_SRC "${CMAKE_SOURCE_DIR}/resources/tools/singlefile_extension")
    if(EXISTS "${SF_EXT_SRC}/manifest.json")
        add_custom_command(TARGET Pen POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_directory
                "${SF_EXT_SRC}"
                "$<TARGET_BUNDLE_DIR:Pen>/Contents/Resources/tools/singlefile_extension"
            COMMENT "Bundling SingleFile extension"
        )
    endif()

    # Icon
    if(NOT APP_ICON_PATH)
        set(APP_ICON_PATH "${CMAKE_SOURCE_DIR}/beta.icns")
    endif()
    if(EXISTS "${APP_ICON_PATH}")
        add_custom_command(TARGET Pen POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${APP_ICON_PATH}" "$<TARGET_BUNDLE_DIR:Pen>/Contents/Resources/${APP_ICON}"
            COMMENT "Bundling app icon: ${APP_ICON}"
        )
    endif()

    # Bundle fonts
    add_custom_command(TARGET Pen POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_BUNDLE_DIR:Pen>/Contents/Resources/fonts"
    )
    file(GLOB FONT_FILES "${CMAKE_SOURCE_DIR}/resources/fonts/*.ttf")
    foreach(FONT ${FONT_FILES})
        add_custom_command(TARGET Pen POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FONT}"
                "$<TARGET_BUNDLE_DIR:Pen>/Contents/Resources/fonts/"
            COMMENT "Bundling font: ${FONT}"
        )
    endforeach()

    # Fix QtWebEngineProcess
    set(FIX_WEBENGINE "${CMAKE_SOURCE_DIR}/scripts/fix_webengine_helper.sh")
    if(EXISTS "${FIX_WEBENGINE}")
        add_custom_command(TARGET Pen POST_BUILD
            COMMAND bash "${FIX_WEBENGINE}" "$<TARGET_BUNDLE_DIR:Pen>"
            COMMENT "Fixing QtWebEngineProcess library paths"
        )
    endif()

    # ★ codesign 전 dangling Frameworks 심볼릭 링크 정리 (sealed resource fix)
    add_custom_command(TARGET Pen POST_BUILD
        COMMAND find "$<TARGET_BUNDLE_DIR:Pen>/Contents/Frameworks" -maxdepth 1 -type l -delete 2>/dev/null || true
        COMMENT "Cleaning broken Frameworks symlinks before codesign"
    )

    # Codesign
    set(CODESIGN_SCRIPT "${CMAKE_SOURCE_DIR}/scripts/codesign_app.sh")
    if(EXISTS "${CODESIGN_SCRIPT}")
        add_custom_command(TARGET Pen POST_BUILD
            COMMAND bash "${CODESIGN_SCRIPT}" "$<TARGET_BUNDLE_DIR:Pen>"
            COMMENT "Codesigning entire app bundle"
        )
    endif()
endif()

# ═══════════════════════════════════════════
# Windows-specific post-build steps
# ═══════════════════════════════════════════

if(WIN32)
    # ★ APP_NAME 미설정 시 Pen 기본값 (한국어 + 공백 이름은 빌드 도구가 깨짐 — ASCII 사용)
    if(NOT APP_NAME)
        set(APP_NAME "Pen")
    endif()
    set_target_properties(Pen PROPERTIES
        OUTPUT_NAME "${APP_NAME}"
        WIN32_EXECUTABLE TRUE
    )

    set(WIN_OUTPUT_DIR "$<TARGET_FILE_DIR:Pen>")

    # Fonts
    add_custom_command(TARGET Pen POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${WIN_OUTPUT_DIR}/fonts"
    )
    file(GLOB FONT_FILES "${CMAKE_SOURCE_DIR}/resources/fonts/*.ttf")
    foreach(FONT ${FONT_FILES})
        add_custom_command(TARGET Pen POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different "${FONT}" "${WIN_OUTPUT_DIR}/fonts/"
            COMMENT "Bundling font: ${FONT}"
        )
    endforeach()

    # windeployqt
    find_program(WINDEPLOYQT windeployqt HINTS "${Qt6_DIR}/../../../bin")
    if(WINDEPLOYQT)
        add_custom_command(TARGET Pen POST_BUILD
            COMMAND "${WINDEPLOYQT}" --no-translations "$<TARGET_FILE:Pen>"
            COMMENT "Running windeployqt (bundling Qt DLLs)"
        )
    endif()
endif()
