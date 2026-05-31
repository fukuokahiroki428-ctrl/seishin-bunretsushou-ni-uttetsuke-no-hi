═══════════════════════════════════════════════════════════════
  Chernobyl — Windows 빌드 안내 (소스 ZIP)
═══════════════════════════════════════════════════════════════

이 ZIP은 Windows에서 직접 빌드하는 소스 패키지입니다.
Mac에서 cross-compile 안 됩니다 — Windows 머신에서 진행하세요.


┌─────────────────────────────────────────────────────────────┐
│ 1. 사전 설치 (Windows)                                       │
└─────────────────────────────────────────────────────────────┘

• Qt 6.5 이상 (권장 6.7.0)
  → https://www.qt.io/download-open-source
  → Online Installer 실행 → 6.7.0 선택
  → 컴포넌트:
       ✓ MinGW 11.2.0 64-bit  (또는 MSVC 2019/2022 64-bit)
       ✓ Qt WebEngine
       ✓ Qt WebChannel
       ✓ Qt WebSockets
       ✓ Qt Positioning

• CMake 3.20+
  → https://cmake.org/download/  → Windows x64 Installer
  → 설치 시 "Add to PATH" 체크

• Git (선택 — QXlsx 자동 fetch 시 필요)
  → https://git-scm.com/download/win


┌─────────────────────────────────────────────────────────────┐
│ 2. Windows 전용 도구 (선택, 다운로드 후 resources/tools/)    │
└─────────────────────────────────────────────────────────────┘

ZIP에는 Mac용 바이너리가 빠져있습니다.
영상/메타데이터 기능을 쓰려면 Windows .exe 받아서 배치하세요.

• yt-dlp.exe   → https://github.com/yt-dlp/yt-dlp/releases
• ffmpeg.exe   → https://www.gyan.dev/ffmpeg/builds/  (release-essentials)
• ffprobe.exe  → ffmpeg와 같은 패키지에 포함
• exiftool.exe → https://exiftool.org/

배치 위치: resources\tools\
   yt-dlp.exe
   ffmpeg.exe
   ffprobe.exe
   exiftool.exe   (또는 폴더 통째)


┌─────────────────────────────────────────────────────────────┐
│ 3. 빌드                                                      │
└─────────────────────────────────────────────────────────────┘

  cd miyo_cpp
  build_windows.bat

자동 감지 안 될 때:
  set QT_DIR=C:\Qt\6.7.0\mingw_64
  build_windows.bat


┌─────────────────────────────────────────────────────────────┐
│ 4. 결과물                                                    │
└─────────────────────────────────────────────────────────────┘

  dist\Chernobyl_win\Chernobyl.exe   ← 더블클릭 실행
  dist\Chernobyl_win.zip              ← 배포용 ZIP

Qt DLL, 폰트, tools, SingleFile 확장 등 자체 완결 패키지.


┌─────────────────────────────────────────────────────────────┐
│ 5. 트러블슈팅                                                │
└─────────────────────────────────────────────────────────────┘

Q. CMake configure 실패 — Qt6 못 찾음
A. set QT_DIR=C:\Qt\6.7.0\mingw_64

Q. windeployqt.exe 못 찾음 (DLL 누락)
A. PATH에 %QT_DIR%\bin 추가

Q. mingw32-make 없음
A. Qt Maintenance Tool → MinGW 11.2.0 64-bit 추가 설치


═══════════════════════════════════════════════════════════════
포함된 기능: Twitter / Bluesky / Instagram / Pixiv / Tumblr /
            SpinSpin / Asked / Trad / 내각회 워처 / WebDAV 업로드
═══════════════════════════════════════════════════════════════
