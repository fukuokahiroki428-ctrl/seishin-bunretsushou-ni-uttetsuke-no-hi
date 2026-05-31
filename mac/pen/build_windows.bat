@echo off
REM ═════════════════════════════════════════════════════════════════
REM Pen (팬을 잘 쓰고 싶다) Windows 빌드 스크립트
REM
REM 사전 조건:
REM   1) Qt6 (6.5+) — qt-online-installer.exe로 MinGW 또는 MSVC 설치
REM   2) CMake 3.20+
REM   3) MinGW 또는 Visual Studio 2019+
REM
REM 사용법:
REM   set QT_DIR=C:\Qt\6.7.0\mingw_64  (또는 환경변수에 미리 등록)
REM   build_windows.bat
REM ═════════════════════════════════════════════════════════════════

setlocal enabledelayedexpansion
cd /d "%~dp0"

if "%QT_DIR%"=="" (
    for %%V in (6.8.0 6.7.3 6.7.2 6.7.1 6.7.0 6.6.0 6.5.3 6.5.2 6.5.1 6.5.0) do (
        if exist "C:\Qt\%%V\mingw_64\bin\qmake.exe" (
            set "QT_DIR=C:\Qt\%%V\mingw_64"
            goto :qt_found
        )
        if exist "C:\Qt\%%V\msvc2019_64\bin\qmake.exe" (
            set "QT_DIR=C:\Qt\%%V\msvc2019_64"
            goto :qt_found
        )
        if exist "C:\Qt\%%V\msvc2022_64\bin\qmake.exe" (
            set "QT_DIR=C:\Qt\%%V\msvc2022_64"
            goto :qt_found
        )
    )
    echo [ERROR] Qt6 설치를 찾을 수 없습니다. QT_DIR 환경변수 설정 필요.
    exit /b 1
)
:qt_found
echo [INFO] Qt: %QT_DIR%

if not exist build_win mkdir build_win
cd build_win

where mingw32-make >nul 2>&1
if %errorlevel%==0 (
    set "GENERATOR=MinGW Makefiles"
    set "MAKE_CMD=mingw32-make"
) else (
    set "GENERATOR=Ninja"
    set "MAKE_CMD=ninja"
)

cmake -G "%GENERATOR%" ^
    -DCMAKE_BUILD_TYPE=Release ^
    -DCMAKE_PREFIX_PATH="%QT_DIR%" ^
    -DAPP_NAME=Pen ^
    -DAPP_ID=com.pen.app ^
    ..
if %errorlevel% neq 0 (
    echo [ERROR] CMake configure 실패
    cd ..
    exit /b 1
)

%MAKE_CMD% -j8
if %errorlevel% neq 0 (
    echo [ERROR] 빌드 실패
    cd ..
    exit /b 1
)

cd ..

REM ─── 배포 디렉토리 — ASCII 이름 (NFC/NFD 충돌 회피) ────────────
set "DIST_DIR=dist\Pen_win"
if exist "%DIST_DIR%" rmdir /S /Q "%DIST_DIR%"
mkdir "%DIST_DIR%"

copy /Y "build_win\Pen.exe" "%DIST_DIR%\" >nul 2>&1

REM windeployqt — Qt DLL 자동 번들
"%QT_DIR%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler ^
    --no-opengl-sw "%DIST_DIR%\Pen.exe"

REM 리소스
if exist "resources\fonts" xcopy /E /I /Y "resources\fonts" "%DIST_DIR%\fonts\" >nul
if exist "resources\html"  xcopy /E /I /Y "resources\html"  "%DIST_DIR%\html\"  >nul

REM 도구 번들 (Windows 바이너리만)
if exist "resources\tools" (
    mkdir "%DIST_DIR%\tools" 2>nul
    for %%F in (resources\tools\*.py) do copy /Y "%%F" "%DIST_DIR%\tools\" >nul
    if exist "resources\tools\singlefile_extension" xcopy /E /I /Y "resources\tools\singlefile_extension" "%DIST_DIR%\tools\singlefile_extension\" >nul
    if exist "resources\tools\exiftool" xcopy /E /I /Y "resources\tools\exiftool" "%DIST_DIR%\tools\exiftool\" >nul
)

REM ─── ZIP 압축 ──────────────────────────────────────────────────
if exist "%DIST_DIR%.zip" del "%DIST_DIR%.zip"
powershell -Command "Compress-Archive -Path '%DIST_DIR%\*' -DestinationPath '%DIST_DIR%.zip' -Force"

echo.
echo ─────────────────────────────────────────────────────────
echo ✅ 빌드 완료!
echo    실행파일: %DIST_DIR%\Pen.exe (Display: 팬을 잘 쓰고 싶다)
echo    압축본:   %DIST_DIR%.zip
echo ─────────────────────────────────────────────────────────
echo.

endlocal
