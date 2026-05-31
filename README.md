# 精神分裂症を患うにはうってつけの日

> **Chernobyl** (Windows · macOS) + **팬을 잘 쓰고 싶다 / Pen** (macOS)
> 소셜 미디어 다운로더 · 사이트 통째 크롤러 통합 프로젝트

---

## 📦 구성 (Overview)

이 저장소는 세 개의 앱을 하나로 묶은 모노레포입니다.

| 앱 | 플랫폼 | 설명 |
|----|--------|------|
| **Chernobyl** | Windows | Twitter/Bluesky/Instagram/Pixiv/Fanbox/Tumblr/Discord/YouTube 등 다운로더 |
| **Chernobyl** | macOS | 위와 동일 (원본). rclone 기반 NAS 백업 + CDP 캡쳐 포함 |
| **팬을 잘 쓰고 싶다 (Pen)** | macOS | 인터랙티브 사이트 통째 미러 — 소스/자산 분리 보존 (wget -mkp 현대식) |

> **다운로드(설치파일/DMG)는 [Releases](../../releases) 에 있습니다.** 소스 코드는 이 저장소에 있습니다.

---

## ⬇️ 다운로드 (Releases)

[**최신 릴리스로 이동 →**](../../releases/latest)

| 파일 | 플랫폼 | 용량 | 설치 |
|------|--------|------|------|
| `Chernobyl_Setup.exe` | Windows 10/11 (x64) | ~261 MB | 더블클릭 → 설치 (관리자 권한 불필요, %LOCALAPPDATA% 에 설치) |
| `Chernobyl.dmg` | macOS (Apple Silicon) | ~721 MB | 마운트 → Applications 로 드래그 |
| `팬을 잘 쓰고 싶다.dmg` | macOS (Apple Silicon) | ~652 MB | 마운트 → Applications 로 드래그 |

> ⚠️ **서명 안내**: 두 macOS DMG 와 Windows 설치파일은 정식 배포 서명(Apple Developer ID / Microsoft 인증서)이 아닙니다.
> - **macOS**: 첫 실행 시 우클릭 → "열기", 또는 `시스템 설정 → 개인정보 보호 및 보안 → 확인 없이 열기`
> - **Windows**: SmartScreen 경고 → "추가 정보" → "실행"
>
> macOS DMG 는 Qt 프레임워크가 .app 안에 **완전히 번들**되어 있어 Homebrew Qt 없이도 실행됩니다.

---

## 🗂 디렉토리 구조

```
.
├── mac/
│   ├── chernobyl/      # macOS Chernobyl 소스 (Qt6 C++)
│   └── pen/            # macOS 팬을 잘 쓰고 싶다 소스 (Qt6 C++)
├── windows/            # Windows Chernobyl 소스 (Qt6 C++) + GitHub Actions 빌드
└── README.md
```

> 대용량 외부 도구(yt-dlp, ffmpeg, rclone, exiftool, Chromium)와 빌드 산출물은 저장소에서 제외되어 있습니다.
> 빌드 시 자동 다운로드되거나(Windows), 별도 번들 스크립트로 채워집니다(macOS).

---

## 🔧 빌드 방법

### Windows (GitHub Actions — 권장, 클라우드 자동 빌드)
`windows/` 의 코드를 push 하면 `.github/workflows/build.yml` 이 GitHub 서버(windows-2022)에서 자동 빌드합니다.
- Qt 6.7.3 (`win64_msvc2019_64`) + WebEngine 자동 설치
- yt-dlp / ffmpeg / rclone 자동 다운로드
- `windeployqt` 로 Qt DLL 번들 + `Inno Setup` 으로 단일 `Chernobyl_Setup.exe` 생성
- 결과물은 Actions 탭의 **Artifacts** 또는 Releases 에서 다운로드

로컬 빌드 시: `windows/scripts/download_windows_tools.sh` 로 도구 받은 뒤 CMake + MSVC.

### macOS (로컬 빌드)
요구사항: **Qt 6.7+ (Homebrew: `brew install qt`)**, CMake 3.20+, Xcode CLT

```bash
# Chernobyl
cd mac/chernobyl
cmake -B build -DAPP_NAME=Chernobyl -DAPP_ID=com.chernobyl.app
cmake --build build -j

# 팬을 잘 쓰고 싶다 (Pen)
cd mac/pen
cmake -B build
cmake --build build -j
```

#### 배포용 DMG 만들기 (Qt 프레임워크 번들 — 깨끗한 맥에서도 실행)
개발 빌드는 시스템 Homebrew Qt 에 의존합니다. 배포본은 `macdeployqt` 로 Qt 를 .app 안에 넣어야 합니다.

```bash
cd mac/pen
scripts/make_dist.sh /tmp/Pen_dist.app   # Qt 번들 + 경로 치환 + inside-out 코드사인
```
이 스크립트가 하는 일:
1. `macdeployqt` 로 Qt 프레임워크 54개 번들
2. 남은 Homebrew 절대경로를 `@rpath` / `@executable_path` 로 전부 치환
3. WebEngine 헬퍼(QtWebEngineProcess) 경로 수정
4. inside-out 코드사인 + `--deep --strict` 검증

> **중요(macOS 플러그인 충돌)**: `main.cpp` 는 `_NSGetExecutablePath` 로 번들 PlugIns 를 우선 사용합니다.
> 이 처리가 없으면 번들 Qt 와 Homebrew Qt 가 동시에 로드되어("two sets of Qt") SIGABRT 로 죽습니다.

---

## 🧩 기술 스택

- **Qt 6** (Widgets · WebEngine · WebChannel · Network · WebSockets)
- UI: `QWebEngineView` 안의 단일 HTML/JS + C++↔JS 브리지(`QWebChannel`)
- 캡쳐: **Chrome DevTools Protocol (CDP)** — 번들 Chromium 또는 사용자 Chrome
- Twitter/Bluesky: 번들 Python (twikit / atproto)
- NAS 백업/업로드: **rclone** (WebDAV/SFTP/S3, MIT)
- 사이트 미러: SingleFile inline / 분리 파일(wget -mkp 방식)

---

## 📋 주요 기능

### Chernobyl
- 플랫폼별 다운로더: Twitter/X, Bluesky, Instagram, Pixiv, Fanbox, Tumblr, Discord, YouTube, 그 외
- 체크박스 선택 항목 전체 로그 기록 + 무결성 매니페스트 생성
- NAS 백업(rclone 멀티스레드) + 진행률 터미널
- 캡쳐 시 계정 쿠키 자동 주입(로그인 상태 캡쳐)
- 설정 내보내기/불러오기

### 팬을 잘 쓰고 싶다 (Pen)
- 인터랙티브 CDP 크롤러 — 사용자가 직접 로그인/캡챠 풀면서 캡쳐
- **사이트 통째 미러**: 깊이/페이지 수 지정, 같은 도메인 제한
  - *SingleFile inline*: 한 페이지 = 한 HTML(자산 base64 내장)
  - *분리 파일*: 원본 디렉토리 구조 그대로 + CSS/JS/이미지/폰트 별도 보존
- Twitter/X 프로필 통째 미러(자동 스크롤 + 트윗 링크 로컬 재작성)
- WebDAV NAS 자동 업로드

---

## ⚖️ 라이선스 / 고지

- 본 소스는 개인 사용 목적입니다. 번들 외부 도구는 각자의 라이선스를 따릅니다
  (yt-dlp: Unlicense, ffmpeg: LGPL/GPL, rclone: MIT, exiftool: Perl/Artistic, Chromium: BSD).
- 다운로드 대상 콘텐츠의 저작권 및 각 플랫폼 약관 준수는 사용자 책임입니다.
