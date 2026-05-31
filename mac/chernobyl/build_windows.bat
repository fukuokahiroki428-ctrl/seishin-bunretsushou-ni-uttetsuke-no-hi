@echo off
REM ═════════════════════════════════════════════════════════════════
REM Chernobyl Windows 빌드 스크립트
REM
REM 사전 조건:
REM   1) Qt6 (6.5+) 설치됨 — qt-online-installer.exe 로 MinGW 또는 MSVC 컴포넌트 선택
REM   2) CMake 3.20+ 설치됨 (PATH 등록)
REM   3) MinGW (Qt 동봉) 또는 Visual Studio 2019+ (MSVC) 설치됨
REM
REM 사용법:
REM   1) Qt6 설치 경로를 QT_DIR 환경변수에 설정 (예: C:\Qt\6.7.0\mingw_64)
REM      또는 아래 SET QT_DIR=... 줄 직접 수정
REM   2) build_windows.bat 더블클릭
REM   3) 결과: dist\Chernobyl_win\Chernobyl.exe + 모든 DLL
REM ═════════════════════════════════════════════════════════════════

setlocal enabledelayedexpansion
cd /d "%~dp0"

REM ─── Qt 경로 자동 탐지 ──────────────────────────────────────────
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
    echo [ERROR] Qt6 설치를 찾을 수 없습니다. QT_DIR 환경변수를 설정하세요.
    echo         예: set QT_DIR=C:\Qt\6.7.0\mingw_64
    exit /b 1
)
:qt_found
echo [INFO] Qt: %QT_DIR%

REM ─── 빌드 디렉토리 ───────────────────────────────────────────────
if not exist build_win mkdir build_win
cd build_win

REM ─── Configure (MinGW 우선, 실패 시 Ninja/MSVC) ────────────────
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
    -DAPP_NAME=Chernobyl ^
    -DAPP_ID=com.chernobyl.app ^
    -DAPP_ICON=chernobyl.icns ^
    ..
if %errorlevel% neq 0 (
    echo [ERROR] CMake configure 실패
    cd ..
    exit /b 1
)

REM ─── Build ──────────────────────────────────────────────────────
%MAKE_CMD% -j8
if %errorlevel% neq 0 (
    echo [ERROR] 빌드 실패
    cd ..
    exit /b 1
)

cd ..

REM ─── 배포 디렉토리 ──────────────────────────────────────────────
set "DIST_DIR=dist\Chernobyl_win"
if exist "%DIST_DIR%" rmdir /S /Q "%DIST_DIR%"
mkdir "%DIST_DIR%"

REM exe 복사
copy /Y "build_win\Chernobyl.exe" "%DIST_DIR%\" >nul 2>&1
if not exist "%DIST_DIR%\Chernobyl.exe" copy /Y "build_win\Miyo.exe" "%DIST_DIR%\Chernobyl.exe" >nul 2>&1

REM windeployqt — Qt DLL 자동 번들
"%QT_DIR%\bin\windeployqt.exe" --release --no-translations --no-system-d3d-compiler ^
    --no-opengl-sw "%DIST_DIR%\Chernobyl.exe"

REM 추가 리소스
if exist "resources\fonts"          xcopy /E /I /Y "resources\fonts"          "%DIST_DIR%\fonts\"          >nul
if exist "resources\html"           xcopy /E /I /Y "resources\html"           "%DIST_DIR%\html\"           >nul
if exist "resources\chernobyl.icns" copy /Y         "resources\chernobyl.icns" "%DIST_DIR%\"                >nul

REM 도구 번들 (Windows 바이너리만 있는 경우 — yt-dlp.exe, ffmpeg.exe, ffprobe.exe)
if exist "resources\tools" (
    mkdir "%DIST_DIR%\tools" 2>nul
    REM Python 스크립트들
    for %%F in (resources\tools\*.py) do copy /Y "%%F" "%DIST_DIR%\tools\" >nul
    REM Windows 바이너리만 복사
    if exist "resources\tools\yt-dlp.exe"  copy /Y "resources\tools\yt-dlp.exe"  "%DIST_DIR%\tools\" >nul
    if exist "resources\tools\ffmpeg.exe"  copy /Y "resources\tools\ffmpeg.exe"  "%DIST_DIR%\tools\" >nul
    if exist "resources\tools\ffprobe.exe" copy /Y "resources\tools\ffprobe.exe" "%DIST_DIR%\tools\" >nul
    REM exiftool (Perl 스크립트라 Windows에서도 동작 — perl.exe 필요)
    if exist "resources\tools\exiftool" xcopy /E /I /Y "resources\tools\exiftool" "%DIST_DIR%\tools\exiftool\" >nul
    REM SingleFile 확장
    if exist "resources\tools\singlefile_extension" xcopy /E /I /Y "resources\tools\singlefile_extension" "%DIST_DIR%\tools\singlefile_extension\" >nul
)

REM Python 환경 번들 (Windows-x86_64) — scripts/bundle_python_win.bat이 처리
if exist "scripts\bundle_python_win.bat" (
    echo [INFO] Python 환경 번들...
    call "scripts\bundle_python_win.bat" "%DIST_DIR%"
)

REM ─── ZIP 압축 ──────────────────────────────────────────────────
if exist "%DIST_DIR%.zip" del "%DIST_DIR%.zip"
powershell -Command "Compress-Archive -Path '%DIST_DIR%\*' -DestinationPath '%DIST_DIR%.zip' -Force"

echo.
echo ─────────────────────────────────────────────────────────
echo ✅ 빌드 완료!
echo    실행파일: %DIST_DIR%\Chernobyl.exe
echo    압축본:   %DIST_DIR%.zip
echo ─────────────────────────────────────────────────────────
echo.
echo 누락된 Windows 바이너리 (필요 시 수동 추가):
if not exist "%DIST_DIR%\tools\yt-dlp.exe"  echo    - tools\yt-dlp.exe  (https://github.com/yt-dlp/yt-dlp/releases)
if not exist "%DIST_DIR%\tools\ffmpeg.exe"  echo    - tools\ffmpeg.exe  (https://www.gyan.dev/ffmpeg/builds/)
if not exist "%DIST_DIR%\tools\ffprobe.exe" echo    - tools\ffprobe.exe (위와 동일 패키지)
echo.

endlocal
