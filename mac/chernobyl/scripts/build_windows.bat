@echo off
REM ═══════════════════════════════════════════
REM ABIWA Windows Build Script
REM ═══════════════════════════════════════════
REM
REM 사전 준비:
REM 1. Qt6 설치: https://www.qt.io/download-qt-installer
REM    - Qt 6.7+ 선택, "Qt WebEngine" 컴포넌트 체크
REM    - 설치 후 환경변수: CMAKE_PREFIX_PATH=C:\Qt\6.7.3\msvc2022_64
REM
REM 2. Visual Studio 2022 (Build Tools만 설치 가능)
REM    - "C++를 사용한 데스크톱 개발" 워크로드 선택
REM    - https://visualstudio.microsoft.com/downloads/
REM
REM 3. CMake 설치 (Visual Studio에 포함됨, 또는 별도 설치)
REM    - https://cmake.org/download/
REM
REM 4. Git 설치 (QXlsx 빌드에 필요)
REM    - https://git-scm.com/download/win
REM
REM 5. (선택) yt-dlp.exe, ffmpeg.exe를 resources/tools/에 배치
REM    - yt-dlp: https://github.com/yt-dlp/yt-dlp/releases
REM    - ffmpeg: https://www.gyan.dev/ffmpeg/builds/
REM
REM 사용법:
REM   1. "x64 Native Tools Command Prompt for VS 2022" 열기
REM   2. cd <miyo_cpp 폴더>
REM   3. scripts\build_windows.bat
REM ═══════════════════════════════════════════

setlocal enabledelayedexpansion

echo =========================================
echo   ABIWA Windows Build
echo =========================================

REM Check Qt
if "%CMAKE_PREFIX_PATH%"=="" (
    REM Try common Qt paths
    for %%Q in (
        "C:\Qt\6.8.0\msvc2022_64"
        "C:\Qt\6.7.3\msvc2022_64"
        "C:\Qt\6.7.2\msvc2022_64"
        "C:\Qt\6.7.0\msvc2022_64"
        "C:\Qt\6.6.0\msvc2022_64"
    ) do (
        if exist %%Q (
            set "CMAKE_PREFIX_PATH=%%~Q"
            echo [build] Found Qt at: !CMAKE_PREFIX_PATH!
            goto :qt_found
        )
    )
    echo [build] ERROR: Qt6 not found!
    echo [build] Set CMAKE_PREFIX_PATH=C:\Qt\6.x.x\msvc2022_64
    exit /b 1
)
:qt_found

REM Check CMake
where cmake >nul 2>&1
if !errorlevel! neq 0 (
    echo [build] ERROR: CMake not found! Install Visual Studio with C++ or CMake separately.
    exit /b 1
)

REM Check compiler
where cl >nul 2>&1
if !errorlevel! neq 0 (
    echo [build] WARNING: MSVC compiler not found!
    echo [build] Run this script from "x64 Native Tools Command Prompt for VS 2022"
    exit /b 1
)

set "SRC_DIR=%~dp0.."
set "BUILD_DIR=%SRC_DIR%\build_win"
set "INSTALL_DIR=%SRC_DIR%\dist\ABIWA"

echo [build] Source: %SRC_DIR%
echo [build] Build:  %BUILD_DIR%
echo [build] Output: %INSTALL_DIR%

REM Detect generator (Ninja preferred, VS fallback)
set "CMAKE_GEN=Ninja"
set "CMAKE_EXTRA="
where ninja >nul 2>&1
if !errorlevel! neq 0 (
    echo [build] Ninja not found, using Visual Studio generator
    set "CMAKE_GEN=Visual Studio 17 2022"
    set "CMAKE_EXTRA=-A x64"
) else (
    echo [build] Using Ninja generator
)

REM Configure
echo.
echo [build] Configuring with CMake...
cmake -S "%SRC_DIR%" -B "%BUILD_DIR%" ^
    -G "%CMAKE_GEN%" %CMAKE_EXTRA% ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="%CMAKE_PREFIX_PATH%" ^
    -DCMAKE_INSTALL_PREFIX="%INSTALL_DIR%"

if !errorlevel! neq 0 (
    echo [build] CMake configuration failed!
    exit /b 1
)

REM Build
echo.
echo [build] Building...
cmake --build "%BUILD_DIR%" --config Release --parallel

if !errorlevel! neq 0 (
    echo [build] Build failed!
    exit /b 1
)

echo.
echo =========================================
echo   Build successful!
echo   Output: %BUILD_DIR%
echo =========================================
echo.
echo Next steps:
echo   1. windeployqt should have run automatically
echo   2. Run scripts\bundle_python_win.bat "%BUILD_DIR%\Release"
echo      (or the directory containing ABIWA.exe)
echo   3. Test: "%BUILD_DIR%\Release\ABIWA.exe"
echo.

endlocal
exit /b 0
