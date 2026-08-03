# 精神分裂症を患うにはうってつけの日

> **Predormition** (Windows · macOS)
> 소셜 미디어 다운로더 · 사이트 통째 크롤러 통합 앱
> (구 별도 앱 "팬을 잘 쓰고 싶다 / Pen"은 Predormition 에 **통합**되었습니다 — PenBackend)

---

## 📦 구성 (Overview)

이 저장소는 Predormition(구 Chernobyl · カメラ) 앱의 Windows / macOS 포트를 담은 모노레포입니다.

| 앱 | 플랫폼 | 설명 |
|----|--------|------|
| **Predormition** | Windows | Twitter/Bluesky/Instagram/Pixiv/Fanbox/Tumblr/Discord/YouTube 등 다운로더 |
| **Predormition** | macOS | 위와 동일 (원본). rclone 기반 NAS 백업 + CDP 캡쳐 포함 |

> 구 **팬을 잘 쓰고 싶다 (Pen)** 독립 앱은 폐기되고 기능(사이트 통째 미러)이
> Predormition 내부(`PenBackend`)로 통합되었습니다.

> **다운로드(설치파일/DMG)는 [Releases](../../releases) 에 있습니다.** 소스 코드는 이 저장소에 있습니다.

---

## ⬇️ 다운로드 (Releases)

[**최신 릴리스로 이동 →**](../../releases/latest)

| 파일 | 플랫폼 | 용량 | 설치 |
|------|--------|------|------|
| `Predormition_Setup.exe` | Windows 10/11 (x64) | ~261 MB | 더블클릭 → 설치 (관리자 권한 불필요, %LOCALAPPDATA% 에 설치) |
| `Predormition.dmg` | macOS (Apple Silicon) | ~721 MB | 마운트 → Applications 로 드래그 |

> 📌 구 릴리스의 `Pen.dmg` 는 폐기된 독립 앱입니다 — 해당 기능은 이제 Chernobyl 안에 있습니다.

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
│   └── chernobyl/      # macOS Chernobyl 소스 (Qt6 C++)
├── windows/            # Windows Chernobyl 소스 (Qt6 C++) + GitHub Actions 빌드
├── design-system/      # Darkroom v2 디자인 시스템 카드 (claude.ai/design 동기화)
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
- `windeployqt` 로 Qt DLL 번들 + `Inno Setup` 으로 단일 `Predormition_Setup.exe` 생성
- 결과물은 Actions 탭의 **Artifacts** 또는 Releases 에서 다운로드

로컬 빌드 시: `windows/scripts/download_windows_tools.sh` 로 도구 받은 뒤 CMake + MSVC.

### macOS (로컬 빌드)
요구사항: **Qt 6.7+ (Homebrew: `brew install qt`)**, CMake 3.20+, Xcode CLT

```bash
cd mac/chernobyl
cmake -B build -DAPP_NAME=Predormition -DAPP_ID=com.predormition.app
cmake --build build -j
#  ★ 앱 이름을 바꾸면 scripts/make_dmg.sh 의 기본 경로(build/Predormition.app)와 어긋나 DMG 생성이 실패한다.

```

#### 배포용 DMG 만들기 (Qt 프레임워크 번들 — 깨끗한 맥에서도 실행)
개발 빌드는 시스템 Homebrew Qt 에 의존합니다. 배포본은 `macdeployqt` 로 Qt 를 .app 안에 넣어야 합니다.

```bash
cd mac/chernobyl
./build.sh   # macdeployqt 로 Qt 번들 + fix_webengine_helper.sh + 코드사인 (packaging/ 참고)
```
배포 파이프라인이 하는 일:
1. `macdeployqt` 로 Qt 프레임워크 번들
2. WebEngine 헬퍼(QtWebEngineProcess) 경로 수정 (`fix_webengine_helper.sh` — 백지창 방지)
3. 코드사인 (`codesign_app.sh` / `packaging/` 의 개발자 인증 스크립트)

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

### 사이트 통째 미러 (구 Pen — Chernobyl 에 통합)
- 인터랙티브 CDP 크롤러 — 사용자가 직접 로그인/캡챠 풀면서 캡쳐
- **사이트 통째 미러**: 깊이/페이지 수 지정, 같은 도메인 제한
  - *SingleFile inline*: 한 페이지 = 한 HTML(자산 base64 내장)
  - *분리 파일*: 원본 디렉토리 구조 그대로 + CSS/JS/이미지/폰트 별도 보존
- Twitter/X 프로필 통째 미러(자동 스크롤 + 트윗 링크 로컬 재작성)
- WebDAV NAS 자동 업로드

---

## 🩺 자가수리(SelfRepair) + 로컬 LLM 진단

두 포트(Windows·macOS) 모두 시작 시 백그라운드로 자가진단·자가수리를 수행합니다 (`src/utils/SelfRepair.h`).

1. **자가진단** — 번들 도구(yt-dlp/ffmpeg/python/exiftool/rclone)를 **앱 내부 경로 우선**으로
   해석해 실행 검증. 시스템(homebrew 등) 경로는 최후 폴백.
2. **자동 수리** — 손상된 사용자 복사본 삭제 → 번들본 재복사, 실행권한 복원,
   캡쳐 Chrome stale lock 정리. (다운로더 고장의 최다 원인인 사이트 측 변경은
   설정의 **yt-dlp 자동 업데이트**가 해결 — 켜두기를 권장.)
3. **로컬 LLM 진단** — 복구 실패 항목이 남거나 수집 로그에 오류가 다발하면
   로컬 LLM 에 로그를 보내 원인·조치를 앱 로그에 표시합니다.
   - 자동 탐색: **Ollama**(11434) → **LM Studio**(1234) → **llama.cpp server**(8080)
   - **앱 내부 탑재**: `Resources/llm/` 에 `llama-server` + `*.gguf` 가 있으면 자동 기동(8737).
     패키징 시 `bash scripts/bundle_llm.sh <App.app 또는 배포폴더>` 1회 실행으로 배치
     (기본 모델: Qwen2.5-1.5B-Instruct Q4_K_M ~1GB — git 에는 커밋하지 않음)
   - env `CHERNOBYL_LLM_ENDPOINT` 로 엔드포인트 직접 지정 가능
4. 보고서: `~/Library/Application Support/<앱>/selfrepair/last_report.txt`
   (Windows: `%APPDATA%/<앱>/selfrepair/`)

> **정직한 한계**: LLM 은 *진단·조치 안내*까지만 합니다. 컴파일된 C++ 앱을 런타임에
> 스스로 재작성할 수는 없으며, 실제 "자동 수리"는 1~2의 결정론적 루틴이 수행합니다.

---

## ⚖️ 라이선스 / 고지

- 본 소스는 개인 사용 목적입니다. 번들 외부 도구는 각자의 라이선스를 따릅니다
  (yt-dlp: Unlicense, ffmpeg: LGPL/GPL, rclone: MIT, exiftool: Perl/Artistic, Chromium: BSD).
- 다운로드 대상 콘텐츠의 저작권 및 각 플랫폼 약관 준수는 사용자 책임입니다.

---

## 🚀 릴리즈 배포 절차 (중요)

이 저장소는 **발행된 릴리즈가 immutable** 이라, 발행 후에는 자산을 추가할 수 없다.
mac DMG 는 CI 가 아니라 **로컬에서 빌드·서명**하므로 순서를 반드시 지켜야 한다.

```bash
# 1) mac 앱 빌드 (모델 제외본으로 DMG 를 만든다 — 릴리즈 자산은 파일당 2GB 제한)
cd mac/chernobyl && cmake -B build -DAPP_NAME=Predormition -DAPP_ID=com.predormition.app && cmake --build build -j

# 2) DMG 생성 (서명 자동) → repo/dist/Predormition.dmg
cd ../.. && bash scripts/make_dmg.sh

# 3) 태그 push → CI 가 Windows 빌드 후 '초안(draft)' 릴리즈에 Predormition_Setup.exe 를 올린다
git tag -a v3.7.1 -m "..." && git push origin v3.7.1

# 4) CI 완료를 기다린 뒤 DMG 를 같은 초안에 추가
gh release upload v3.7.1 dist/Predormition.dmg

# 5) 발행 (이 시점 이후로는 자산 추가 불가)
gh release edit v3.7.1 --draft=false --latest --notes "..."
```

**주의**
- 3단계에서 CI 가 만드는 것은 **초안**이다. GitHub UI 에서 먼저 발행해 버리면 mac DMG 를 붙일 수 없다.
- 로컬 AI 모델(gguf, 약 9GB)은 용량 때문에 배포본에 넣지 않는다. DMG 안의
  `AI_설치_더블클릭.command` 가 사용자 PC 에서 내려받아 앱 내부에 설치한다.
- mac 앱 번들 안에 파일을 추가하면 코드서명 봉인이 깨진다 → 반드시 재서명
  (`scripts/bundle_llm.sh` 와 `Common::resealAppBundle()` 이 자동 처리).
