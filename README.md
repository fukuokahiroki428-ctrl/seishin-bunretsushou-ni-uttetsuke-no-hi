# 精神分裂症を患うにはうってつけの日

> **ハンイシキ** (macOS · 판 이름 上野) · **Predormition** (Windows)
> 소셜 미디어 다운로더 · 사이트 통째 크롤러 통합 앱
> (구 별도 앱 "팬을 잘 쓰고 싶다 / Pen"은 Predormition 에 **통합**되었습니다 — PenBackend)

---

## 📦 구성 (Overview)

이 저장소는 Predormition(구 Chernobyl · カメラ) 앱의 Windows / macOS 포트를 담은 모노레포입니다.

| 앱 | 플랫폼 | 설명 |
|----|--------|------|
| **Predormition** | Windows | Twitter/Bluesky/Instagram/Pixiv/Fanbox/Tumblr/Discord/YouTube 등 다운로더 |
| **ハンイシキ** | macOS | 독자 노선. 13개 플랫폼 + rclone NAS 백업 + CDP 캡쳐 + 로컬 AI |

> 구 **팬을 잘 쓰고 싶다 (Pen)** 독립 앱은 폐기되고 기능(사이트 통째 미러)이
> Predormition 내부(`PenBackend`)로 통합되었습니다.

> **다운로드(설치파일/DMG)는 [Releases](../../releases) 에 있습니다.** 소스 코드는 이 저장소에 있습니다.

---

## ⬇️ 다운로드 (Releases)

[**최신 릴리스로 이동 →**](../../releases/latest)

| 파일 | 플랫폼 | 용량 | 설치 |
|------|--------|------|------|
| `Predormition_Setup.exe` | Windows 10/11 (x64) | ~261 MB | 더블클릭 → 설치 (관리자 권한 불필요, %LOCALAPPDATA% 에 설치) |
| `Hanishiki-<버전>.dmg` | macOS 26.0+ (Apple Silicon) | ~700 MB | 마운트 → Applications 로 드래그 → `첫실행_준비.command` |

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
│   └── chernobyl/      # macOS ハンイシキ 소스 (Qt6 C++) — 폴더 이름은 옛 이름 그대로
├── windows/            # Windows Predormition 소스 (Qt6 C++) + GitHub Actions 빌드
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

### macOS (ハンイシキ)

아래 「🍎 ハンイシキ (macOS)」 절의 **빌드** 를 보십시오.
옛 명령(`-DAPP_NAME=Predormition`, `scripts/make_dmg.sh`)은 더 이상 맞지 않습니다.

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

---

# 🍎 ハンイシキ (macOS) — 판 이름 **上野**

> macOS 라인은 Windows 와 갈라진 **독자 노선**입니다. 이름이 여러 번 바뀌었습니다:
> **カメラ → Chernobyl → Predormition → ハンイシキ**.
> 아래 설명은 전부 지금(4.0.0 / 上野) 기준이며, 실제로 확인한 것만 적었습니다.

## 이름이 세 가지인 이유

| 쓰이는 곳 | 값 | 바꾸면 생기는 일 |
|---|---|---|
| 화면 표시 (Finder·Dock·메뉴·창 제목) | `ハンイシキ` | — (보이는 것뿐) |
| 실행 파일 (`CFBundleExecutable`) | `Hanishiki` | `kill_app.sh` 가 프로세스를 못 찾는다 |
| **사용자 데이터 폴더** | `~/Library/Application Support/Miyo/Hanishiki` | **설정·수집물이 갈라진다. 실제로 잃은 적이 있다** |

`.app` 폴더 이름은 `build.sh` 가 codesign 직전에 표시 이름(`ハンイシキ.app`)으로 바꿉니다.
Finder 는 `CFBundleDisplayName` 이 아니라 **파일명**을 보여주기 때문입니다
(메뉴 막대·Dock 만 표시 이름을 씁니다 — `LSDisplayName`).

## 설치

[Releases](../../releases/latest) 에서 `Hanishiki-<버전>.dmg` 를 받으십시오.

1. DMG 를 열고 **ハンイシキ** 를 `Applications` 로 끌어다 놓습니다.
2. 같이 든 `첫실행_준비.command` 를 두 번 누릅니다.
   - 그 파일도 처음엔 막힙니다 → **오른쪽 클릭 → 열기**.
   - 이 앱 하나의 격리 표시만 뗍니다. **시스템 Gatekeeper 는 건드리지 않습니다**
     (`spctl --master-disable` 같은 건 맥 전체를 무방비로 만들기 때문에 쓰지 않습니다).
   - 쓰기 싫으시면 앱을 오른쪽 클릭 → 열기 로 한 번 실행해도 같습니다.

**요구 환경**: macOS **26.0 이상**, 애플 실리콘(arm64).
26.0 이라는 값은 우리가 고른 게 아니라 Homebrew 로 받은 Qt·ICU 등 dylib 63개가 그 macOS 에서
만들어졌기 때문입니다(Qt 자체는 14.0 을 지원). `build.sh` 가 번들 안 Mach-O 의 `minos` 최대값을
빌드마다 계산해 `LSMinimumSystemVersion` 에 적습니다 — 손으로 적어 두면 조용히 어긋나고,
그러면 낮은 macOS 사용자는 **설치는 되는데 실행만 안 되는** 상태가 됩니다.

**공증(notarize)은 되어 있지 않습니다.** 서명은 Apple Development 인증서입니다.

## 빌드

```bash
brew install qt cmake
cd mac/chernobyl
cmake -B build
./build.sh          # macdeployqt → 경로 수정 → 자원 동기화 → 검사 → 이름 변경 → 서명
./make_dmg.sh       # 서명 검증 → DMG → 안의 앱 재검증
```

`build.sh` 는 단순한 래퍼가 아닙니다. 다음을 순서대로 합니다(각 단계는 실제 사고를 겪고 생겼습니다):

| 단계 | 왜 |
|---|---|
| 빌드 잠금 | 빌드를 겹쳐 돌리면 서로의 실행 파일을 지운다. '성공' 인데 앱이 안 뜬다 |
| 끊어진 심볼릭 링크 제거 | 하나만 있어도 `codesign --verify` 가 앱 전체를 가리키며 실패한다 |
| **자원 동기화** | CMake 의 자원 복사는 `POST_BUILD` 라 **타겟이 다시 링크될 때만** 돈다. C++ 을 안 건드리고 자원만 바꾸면 옛것이 그대로 남는다 |
| 도구 아키텍처 확인 | `ffprobe` 만 x86_64 인 채 배포된 적이 있다. 로제타가 있으면 그냥 돌아서 아무도 몰랐다 |
| 최소 macOS 실측 | 위 참고 |
| 번들 이름 변경 | Finder 표시용 |
| inside-out 서명 | `--deep` 서명은 쓰지 않는다(애플도 배포용이 아니라고 한다) |

### 번들 Chromium 넣기 (캡쳐 기능에 필요)

캡쳐는 CDP 로 Chromium 을 몰아 씁니다. 번들에 없으면 **사용자 시스템 Chrome** 으로 떨어지고,
크롬이 없는 맥에서는 캡쳐가 통째로 안 됩니다. CMake 가 없으면 경고합니다.

```bash
# Google 공식 Chrome for Testing (버전은 아래 JSON 에서 확인)
# https://googlechromelabs.github.io/chrome-for-testing/last-known-good-versions-with-downloads.json
curl -fL -o /tmp/cft.zip \
  "https://storage.googleapis.com/chrome-for-testing-public/152.0.7977.54/mac-arm64/chrome-mac-arm64.zip"
unzip -q /tmp/cft.zip -d /tmp/cft
mkdir -p mac/chernobyl/resources/chromium
cp -Rp "/tmp/cft/chrome-mac-arm64/Google Chrome for Testing.app" \
       mac/chernobyl/resources/chromium/Chromium.app
```

`.app` 폴더 이름은 반드시 `Chromium.app`, 그 안의 실행 파일은 `Google Chrome for Testing` 이어야
합니다(`RealChromeCrawler::findChromeExecutable()` 이 그렇게 찾습니다). git 에는 넣지 않습니다(`.gitignore`).

## 앱 내부 한 갈래 원칙

파이썬·도구·스크립트는 **전부 앱 번들 안**에서 돕니다. 예전엔 '번들이 쓰기가능하면 번들,
아니면 외부 복사본' 두 갈래였는데, 층마다 가정이 달라 **Python 업그레이드가 늘 중단되는**
고장이 있었습니다(한쪽은 "번들 우선", 다른 쪽은 "번들이면 중단"이었습니다).

번들에 쓰면 codesign 봉인이 깨지므로 **자동으로 다시 서명**합니다:

- 설치·업그레이드 직후 → `Common::resealAppBundle()`
- 기동할 때마다 → 봉인을 확인하고 깨져 있으면 스스로 복구 (`[SEAL]` 로그)

재서명은 번들에 동봉한 `codesign_app.sh`(빌드가 쓰는 그 스크립트)가 합니다.
인증서가 있으면 그것으로, 없으면 ad-hoc 으로 — **양쪽 다 실제로 검증까지 통과하는 것을 확인**했습니다.

> 참고: 코드 곳곳의 옛 주석은 "봉인이 깨지면 macOS 가 SIGKILL 한다" 고 하지만,
> 지금 서명 구성(Apple Development, 하드닝 런타임 없음)에서는 **깨진 채로도 실행됩니다**.
> 그래도 고쳐야 합니다 — `codesign --verify` 가 계속 실패하고, 공증·배포에서 막히며,
> 하드닝 런타임을 켜는 날 갑자기 안 뜹니다. 급한 불이 아니라 위생 문제입니다.

**앱 밖에 남는 것** (사용자 데이터라 앱을 지워도 남아야 합니다):

```
~/Library/Application Support/Miyo/Hanishiki/
├── miyo_config.json        설정·계정·토큰
├── archive_index.db        산출물 색인 (수백 MB)
├── api_overrides.json      외부 API 상수 덮어쓰기
├── llm/                    AI 엔진·모델 (~9GB — 번들에 넣으면 DMG 가 10GB)
├── script_overrides/       AI 가 고친 스크립트
├── chrome_capture_profile/ 캡쳐용 크롬 프로필
└── tools/                  yt-dlp 자동 갱신본
```

## 배포 절차

**릴리즈가 immutable 입니다** — 발행 뒤에는 자산 교체·추가가 **거부됩니다**
(`HTTP 422: Cannot upload assets to an immutable release`). 잘못 올리면 새 태그로 다시 내야 합니다.

```bash
cd mac/chernobyl && ./build.sh && ./make_dmg.sh
cd ../..
gh release create mac-<버전> "mac/chernobyl/build/Hanishiki-<버전>.dmg" \
   --target ueno --title "ハンイシキ <버전> (上野)" --notes "..."
```

**태그를 `v` 로 시작하지 마십시오.** `.github/workflows/build.yml` 은 `v*` 태그에 반응하는
**Windows 전용** 워크플로입니다. `v4.0.0` 을 밀면 맥과 무관한 빌드가 돕니다.
맥 라인용 CI 는 없습니다 — 로컬 빌드 그대로 냅니다.

## 도구 스크립트

| 파일 | 하는 일 |
|---|---|
| `build.sh` | 위 표의 전 과정 |
| `make_dmg.sh` | 서명 검증 → DMG → 안의 앱 재검증. 파일명은 ASCII, 볼륨명은 카타카나 |
| `sync_resources.sh` | 소스 자원 → 번들. `build.sh` 와 CMake 의 항상 도는 타겟이 **둘 다** 부른다 |
| `codesign_app.sh` | inside-out 서명. 번들에도 동봉되어 런타임 자동 재서명이 쓴다 |
| `kill_app.sh` | 실행 중인 앱만 종료. `pgrep -x` ∩ `pgrep -f` — `ps` 는 한글 경로를 8진 이스케이프로 찍어 비교가 **언제나** 실패했다 |
| `fix_webengine_helper.sh` | QtWebEngineProcess 의 `@executable_path` → `@rpath`. 안 하면 백지 창 |
| `fix_bundle_rpaths.sh` | Homebrew 절대경로 → `@rpath`. 안 하면 남의 맥에서 안 뜬다 |

## 알려진 제약

- **공증 안 됨** — 첫 실행에 오른쪽 클릭 → 열기가 필요합니다
- **macOS 26.0 이상** — Homebrew 의존성이 정합니다. 낮추려면 의존성을 낮은 대상으로 다시 빌드해야 합니다
- **애플 실리콘 전용** — 번들 도구가 arm64 입니다

---

## ⚖️ 라이선스 / 고지

- 본 소스는 개인 사용 목적입니다. 번들 외부 도구는 각자의 라이선스를 따릅니다
  (yt-dlp: Unlicense, ffmpeg: LGPL/GPL, rclone: MIT, exiftool: Perl/Artistic, Chromium: BSD).
- 다운로드 대상 콘텐츠의 저작권 및 각 플랫폼 약관 준수는 사용자 책임입니다.

---

## 🚀 릴리즈 배포 절차

> **macOS(ハンイシキ) 절차는 위 「배포 절차」 를 보십시오.** 아래는 Windows 용입니다.
> 두 라인은 갈라졌습니다 — 맥은 로컬 빌드, 윈도우는 CI 입니다.

### Windows
`v*` 태그를 push 하면 `.github/workflows/build.yml` 이 windows-2022 에서 빌드하고
`Predormition_Setup.exe` 를 릴리즈에 올립니다.

```bash
git tag -a v<버전> -m "..." && git push origin v<버전>
```

**주의**: 이 저장소는 발행된 릴리즈가 **immutable** 이라 발행 뒤에는 자산을 추가·교체할 수
없습니다(`HTTP 422`). 자산을 다 올린 뒤에 발행하십시오.
