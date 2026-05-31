@echo off
REM Bundle standalone Python 3.14 + packages for Windows
REM Usage: bundle_python_win.bat <OUTPUT_DIR>
REM OUTPUT_DIR = directory containing ABIWA.exe

setlocal enabledelayedexpansion

set "OUTPUT_DIR=%~1"
if "%OUTPUT_DIR%"=="" (
    echo Usage: %~nx0 ^<path\to\output\dir^>
    exit /b 1
)

set "PYTHON_DIR=%OUTPUT_DIR%\python_env"
set "MARKER=%PYTHON_DIR%\.bundled_ok"

REM Standalone Python tarball
set "SCRIPT_DIR=%~dp0"
set "TARBALL_DIR=%SCRIPT_DIR%python_standalone"
set "ZIPFILE=%TARBALL_DIR%\cpython-3.14-x86_64-pc-windows-msvc.zip"
set "DOWNLOAD_URL=https://github.com/astral-sh/python-build-standalone/releases/download/20260320/cpython-3.14.3+20260320-x86_64-pc-windows-msvc-install_only.tar.gz"

REM Check if already bundled
if exist "%MARKER%" (
    "%PYTHON_DIR%\python.exe" -c "import twikit; import httpx; import atproto; import browser_cookie3" 2>nul
    if !errorlevel!==0 (
        echo [bundle_python_win] Already bundled and OK, skipping
        exit /b 0
    )
    echo [bundle_python_win] Packages missing, rebuilding...
    rmdir /s /q "%PYTHON_DIR%"
)

REM Download if not cached
if not exist "%TARBALL_DIR%" mkdir "%TARBALL_DIR%"

REM Use PowerShell to download the standalone Python
set "TARBALL=%TARBALL_DIR%\cpython-3.14-windows.tar.gz"
if not exist "%TARBALL%" (
    echo [bundle_python_win] Downloading standalone Python 3.14...
    powershell -Command "Invoke-WebRequest -Uri '%DOWNLOAD_URL%' -OutFile '%TARBALL%'"
    if !errorlevel! neq 0 (
        echo [bundle_python_win] WARN: Download failed -- skipping Python bundle ^(best-effort^). App re-bundles on first run.
        exit /b 0
    )
)

REM Extract (tar is available on Windows 10+)
echo [bundle_python_win] Extracting standalone Python 3.14...
if not exist "%PYTHON_DIR%" mkdir "%PYTHON_DIR%"
tar xzf "%TARBALL%" -C "%PYTHON_DIR%" --strip-components=1

REM Verify
if not exist "%PYTHON_DIR%\python.exe" (
    echo [bundle_python_win] WARN: python.exe not found after extraction -- skipping ^(best-effort^).
    dir "%PYTHON_DIR%" 2>nul
    exit /b 0
)

echo [bundle_python_win] Python:
"%PYTHON_DIR%\python.exe" --version

REM Install packages
echo [bundle_python_win] Installing twikit, httpx, atproto, openpyxl, Pillow, piexif, bs4, websockets, lxml, m3u8, cryptography, browser_cookie3...
"%PYTHON_DIR%\python.exe" -m pip install --quiet --no-cache-dir twikit httpx atproto openpyxl Pillow piexif beautifulsoup4 websockets lxml m3u8 cryptography browser_cookie3 2>&1

REM Verify packages (browser_cookie3 포함 — Instagram/Pixiv/Tumblr Chrome 쿠키 추출에 필수)
echo [bundle_python_win] Verifying...
"%PYTHON_DIR%\python.exe" -c "import twikit, httpx, atproto, openpyxl, PIL, piexif, bs4, browser_cookie3; print('All packages verified!')"
if !errorlevel! neq 0 (
    echo [bundle_python_win] WARN: package verification failed -- skipping marker ^(best-effort^). App re-bundles on first run.
    exit /b 0
)

REM Cleanup to reduce size
echo [bundle_python_win] Cleaning up...
REM Remove test directories
for /d /r "%PYTHON_DIR%" %%d in (tests test) do (
    if exist "%%d" rmdir /s /q "%%d" 2>nul
)
REM Remove unnecessary modules
if exist "%PYTHON_DIR%\Lib\test" rmdir /s /q "%PYTHON_DIR%\Lib\test"
if exist "%PYTHON_DIR%\Lib\idlelib" rmdir /s /q "%PYTHON_DIR%\Lib\idlelib"
if exist "%PYTHON_DIR%\Lib\tkinter" rmdir /s /q "%PYTHON_DIR%\Lib\tkinter"
if exist "%PYTHON_DIR%\Lib\turtledemo" rmdir /s /q "%PYTHON_DIR%\Lib\turtledemo"
REM ensurepip 유지 — pip 복구에 필요
if exist "%PYTHON_DIR%\Lib\pydoc_data" rmdir /s /q "%PYTHON_DIR%\Lib\pydoc_data"
if exist "%PYTHON_DIR%\Lib\lib2to3" rmdir /s /q "%PYTHON_DIR%\Lib\lib2to3"
if exist "%PYTHON_DIR%\include" rmdir /s /q "%PYTHON_DIR%\include"
REM pip 유지 — 앱 내 모듈 업데이트에 필요
REM setuptools만 삭제
for /d %%d in ("%PYTHON_DIR%\Lib\site-packages\setuptools*") do rmdir /s /q "%%d" 2>nul

REM Write marker
echo standalone python 3.14 bundled %date% %time% > "%MARKER%"

echo [bundle_python_win] Done! Standalone Python 3.14 bundled.
endlocal
exit /b 0
