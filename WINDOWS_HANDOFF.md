# Windows 검증 인수인계

이 문서는 **Windows PC에서 이어받을 Claude 세션**을 위한 것이다.
맥에서 v3.9.0 → v3.9.3 까지 작업했는데, **Windows 관련 수정이 전부 CI 컴파일까지만
검증됐고 실제 Windows PC에서 한 번도 실행되지 않았다.** 그 검증이 이 문서의 목적이다.

- 작성 시점 기준 최신: **v3.9.3** (커밋 `a6cf358`)
- 저장소: https://github.com/fukuokahiroki428-ctrl/seishin-bunretsushou-ni-uttetsuke-no-hi

---

## 0. 먼저 알아야 할 것

**앱 개요** — Predormition (구 Chernobyl / カメラ). Qt6 C++ 데스크톱 앱.
13개 플랫폼(Twitter/X, Bluesky, Instagram, Pixiv, Fanbox, Tumblr, Discord,
YouTube, 니코니코동화, Asked, SpinSpin, 사이트 크롤러 등)에서 자료를 모아 보관한다.
UI 는 단일 HTML/JS 를 QWebChannel(`backend.*` 슬롯)로 C++ 과 연결한다.

**저장소 구조**
```
VERSION                     ← 버전 단일 출처. 여기만 고치면 전부 따라간다
mac/chernobyl/              ← macOS 트리
windows/                    ← Windows 트리  ★ 이번 검증 대상
  build_windows.bat         ← 로컬 빌드 스크립트
  CMakeLists.txt
  predormition.iss          ← Inno Setup 설치본 스크립트
  src/core/MiyoBackend.cpp  ← 대부분의 로직 (16000줄 이상)
.github/workflows/build.yml ← Windows CI (태그 v* 푸시 시 릴리즈 발행)
```

> **주의: mac 트리와 windows 트리의 `MiyoBackend.cpp` 를 통째로 복사하지 말 것.**
> 과거에 그렇게 했다가 mac 전용 코드(`MainWindow::setChromeTheme` 등)가 들어가
> Windows 빌드가 깨졌다. 수정은 항상 **해당 부분만 선택적으로** 옮긴다.

**보안 제약 (반드시 지킬 것)**
- 비밀번호·API 키·토큰·계정 정보를 Claude 가 입력하지 않는다. 사용자가 직접 넣는다.
  (GitHub PAT, Claude API 키, Brave Search API 키, NAS 비밀번호 전부 해당)
- 릴리즈 발행·푸시 등 외부에 나가는 작업은 사용자 승인을 받는다.

---

## 1. 가장 먼저 확인할 것 — 병렬 수집 크래시

**이게 최우선이다.** v3.9.2 에서 실제로 앱이 꺼졌고, v3.9.3 의 수정이 맞는지
아직 아무도 확인하지 않았다.

### 배경

`getChromePtr()` 이 `&m_captureChromesPerThread[trackKey]` — 즉 **QMap 노드의 주소**를
`RealChromeCrawler**` 로 반환한다. 이 포인터를 진행 중인 CDP 콜백 10곳이 붙들고 있다.
그런데 수집 종료 시 정리 코드가 그 노드를 `erase()` 로 없애서, 한 트랙이 끝나는 순간
다른 콜백이 **해제된 메모리를 역참조**해 앱이 죽었다 (use-after-free).

| 버전 | 상태 |
|---|---|
| v3.9.1 | `erase` 코드는 있었지만 찾는 키가 `platform`("twitter") 이라 병렬 키("twitter#0")와 안 맞아 **실행되지 않음** → 안 터짐 |
| v3.9.2 | 키를 `trackKey` 로 고치자 `erase` 가 비로소 실행되며 **크래시 발생** |
| v3.9.3 | 노드를 지우지 않고 값만 `nullptr` 로 비우도록 수정 → **미검증** |

수정 위치: `windows/src/core/MiyoBackend.cpp` 의 `notifyCollectionEnded()`

### 검증 방법

1. 수집을 **두 개 이상 동시에** 시작한다 (예: Twitter 계정 2개, 또는 Twitter + Pixiv).
2. **하나가 먼저 끝나야 한다** — 크래시 경로가 "한 트랙 종료 시 다른 트랙 콜백이 도는 것"
   이라 동시에 끝나면 지나가지 않는다. 한쪽을 적은 분량으로 잡으면 좋다.
3. 먼저 끝난 뒤에도 나머지가 정상 진행되는지, 앱이 살아있는지 본다.

**결과 판정**
- 앱이 죽지 않고 나머지 수집이 완주 → 수정 성공
- 여전히 죽음 → 다른 원인. 아래 "크래시 정보 수집" 참고

### 함께 확인할 것 — 캡쳐 Chrome 포트

같은 영역에서 별개 버그가 하나 더 있었다. '죽은 Chrome 재생성' 가드가 트랙 예약 포트가
아니라 기본값 9223 으로 재생성해서, `RealChromeCrawler::start()` 가 그 포트를 쓰는
**다른 트랙의 멀쩡한 Chrome 을 죽였다.** `capturePortFor(trackKey)` 로 트랙별 포트를
고정해 고쳤으나 이것도 미검증이다.

병렬 수집 중 작업 관리자에서 **Chrome 프로세스가 트랙 수만큼 유지되는지** 본다.
하나가 사라지고 그 트랙 수집이 실패하기 시작하면 이 문제다.

### 크래시 정보 수집

앱이 죽으면 아래를 모아서 보고한다.

```powershell
# 1) 앱 자체 로그 (v3.9.1 부터 여기로 옮겼다. 예전엔 설치 폴더라 권한 때문에 안 써졌음)
Get-Content "$env:APPDATA\Miyo\Predormition\predormition_log.txt" -Tail 200

# 2) Windows 이벤트 로그의 응용 프로그램 오류
Get-EventLog -LogName Application -EntryType Error -Newest 10 |
  Where-Object { $_.Message -match "Predormition" } | Format-List

# 3) 크래시 덤프 (있다면)
Get-ChildItem "$env:LOCALAPPDATA\CrashDumps" -Filter "Predormition*" | Sort-Object LastWriteTime -Desc
```

---

## 2. 나머지 미검증 항목 (v3.9.1 의 Windows 수정 25건)

전부 CI 컴파일만 통과했다. 아래는 **실제로 눌러봐야 아는 것들**이다.

### 중대

| # | 항목 | 확인 방법 |
|---|---|---|
| 1 | 죽은 캡쳐 Chrome 재사용 가드 | 캡쳐 중 Chrome 을 강제 종료 → 다음 항목부터 자동 재생성되어 계속 수집되는가 (예전엔 이후 전부 실패) |
| 2 | `.bat` 저장 경로 이스케이프 | YouTube 저장 폴더 이름에 `&` `(` `)` `%` 를 넣고 다운로드 → 배치가 안 깨지는가 |

### 중간

| # | 항목 | 확인 방법 |
|---|---|---|
| 3 | 다운로드 무결성 (쓰기 실패·크기 불일치 검출) | 다운로드 중 USB/NAS 를 뽑아본다 → "성공"으로 보고하지 않고 실패 처리되는가 |
| 4 | **Python 업그레이드** | 설정 → Python 업그레이드. **수집을 돌리는 중에도 한 번 눌러본다** → 실패하더라도 기존 python_env 가 살아있어야 한다 (예전엔 통째로 사라질 수 있었다) |
| 5 | Instagram 쿠키 우선순위 + X-CSRFToken | Instagram 수집이 실제로 게시물을 가져오는가 |
| 6 | manifest 범위 축소 | 수집 후 생성되는 manifest 가 저장 루트 전체가 아니라 **이번 대상 폴더** 기준인가 |
| 7 | SelfRepair 락 정리 | Chrome 이 "이미 실행 중"으로 즉시 종료될 때 자동 복구되는가 |
| 8 | WebDAV 경로 매칭 (구분자·대소문자) | NAS 백업 시 파일이 엉뚱한 경로로 안 가는가 |
| 9 | PowerShell tail 경로 이스케이프 | 경로에 작은따옴표(`'`)가 있어도 로그 창이 뜨는가 |

### 경미

| # | 항목 | 확인 방법 |
|---|---|---|
| 10 | **2차 실행 시 기존 창 활성화** | 앱이 켜진 상태에서 바탕화면 아이콘을 다시 누른다 → **창이 앞으로 나와야 한다.** v3.9.2 까지는 `FindWindowW` 가 옛 제목 `カメラ` 를 찾아 항상 실패했다 (v3.9.3 에서 수정) |
| 11 | 저장 경로 선택 시작 위치 | "찾아보기" 시 기존 저장 경로에서 시작하는가 (예전엔 macOS 전용 `/Volumes` 라 항상 홈) |
| 12 | exiftool | EXIF 메타데이터가 실제로 기록되는가 (Windows 는 단독 `exiftool.exe` 사용, perl 불필요) |
| 13 | 진단 로그 위치·회전 | `%APPDATA%\Miyo\Predormition\predormition_log.txt` 에 쌓이는가, 5MB 넘으면 `.1` 로 회전하는가 |
| 14 | 니코동 EXIF 출처 표기 | 니코동 영상 파일의 EXIF 출처가 "YouTube @" 가 아니라 "ニコニコ @" 인가 |
| 15 | 설치본 제거 | 제어판에서 제거 → `python_env`, `tools`, `*.log` 가 남지 않는가 |

---

## 3. 빌드 방법 (Windows 로컬)

CI 를 기다리지 않고 직접 빌드할 때.

```bat
cd windows
build_windows.bat
```

필요 조건: Visual Studio 2022 (MSVC), Qt 6.7.3 msvc2019_64, CMake, Ninja.

> `build_windows.bat` 은 v3.9.2 까지 `-DAPP_NAME=Chernobyl` 로 빌드해서 산출물 이름이
> 설치 스크립트와 어긋났다. 지금은 Predormition 으로 통일돼 있다.

**버전을 바꿀 때는 루트 `VERSION` 파일만 고친다.** CMake·Info.plist·exe 버전 리소스·
설치본 버전이 전부 따라간다. CI 는 태그와 VERSION 이 다르면 빌드를 세운다.

설치본만 따로 만들려면 (Inno Setup 6 필요):
```bat
iscc /DMyAppVersion=3.9.3 predormition.iss
```

---

## 4. CI 에 대해

`.github/workflows/build.yml` — `main` 푸시 또는 `v*` 태그 푸시 시 동작.
태그일 때만 GitHub Release 를 **draft** 로 만들고 `Predormition_Setup.exe` 를 올린다.
(맥 DMG 는 맥에서 만들어 수동으로 같은 릴리즈에 올린 뒤 발행한다.)

**산출물 검증 게이트 11종** — 하나라도 없으면 빌드를 세운다. 조용히 빠진 파일 때문에
"켜지긴 하는데 기능이 없는" 설치본이 나가던 문제를 막기 위한 것이다.

```
Predormition.exe, Qt6Core.dll, Qt6WebEngineCore.dll,
QtWebEngineProcess.exe, resources/qtwebengine_resources.pak, resources/icudtl.dat,
html/index.html, yt-dlp.exe, ffmpeg.exe,
tools/twitter_daemon.py, tools/bluesky_daemon.py
```

그 외 **MAX_PATH 게이트**(상대 경로 180자 초과 시 중단), **python_env import 게이트**
(`twikit`/`atproto`/`browser_cookie3` 실제 import), **태그↔VERSION 대조 게이트**가 있다.

---

## 5. 알려진 한계 / 미해결

- **NAS 업로드 보류 중.** 90GiB zip(`神州ノ不滅ヲ信シ.zip`)을 Synology WebDAV 에 올리려다
  512 KiB 이상이 `507 Insufficient Storage` 로 거부됨. 파일 자체는 정상(Zip64 온전,
  파일명 문제없음). 용량 문제인지 요청 크기 제한인지 **아직 안 갈림** — 사용자는 2TB 라고
  했으므로 후자 가능성 있음. 조각 업로드 스크립트는 맥의 임시 폴더에만 있고 저장소에는 없다.
- **NAS keep-alive 워치독** — 30초마다 네트워크 마운트를 건드려 세션이 끊기지 않게 한다.
  맥에서만 구현·확인했다(`mount` 파싱 기반). Windows 는 UNC/드라이브 문자 구조가 달라
  별도 구현이 필요하다. 설정에서 "NAS 자동 재연결"을 켜야 동작한다.
- **Pixiv NAS/외장 저장 재시험 안 됨** — 계정이 없어 런타임 확인 못 함.
- **Windows 용 AI 설치기 — 만들었으나 실행 검증 안 됨.**
  `scripts\AI_설치_더블클릭.bat` (런처) + `scripts\win_install_ai.ps1` (본체).
  맥에서 구문 검사·정적 분석(PSScriptAnalyzer)·URL 접근까지는 확인했지만
  **Windows 에서 실제로 돌려보지 않았다.** 아래를 확인해 달라:
  - 앱 설치 후 `.bat` 더블클릭 → 앱 폴더를 찾는가 (`%LOCALAPPDATA%\Programs\Predormition`)
  - 엔진이 `<앱>\llm\llama-server.exe` 로 들어가는가, 체크섬 검증이 통과하는가
  - 모델 분할본(`.partaa`/`.partab`)이 올바르게 합쳐지는가 (스트림 복사로 구현)
  - 중간에 Ctrl+C 로 끊고 다시 실행 → 처음부터가 아니라 **이어받는가**
  - 설정 → 로컬 AI 에서 "AI 켜기" 가 동작하는가
  - 권한 없는 위치(Program Files)에 설치했을 때 안내가 뜨는가

---

## 6. 이 작업에서 배운 것 (같은 실수 반복 방지)

- **캡쳐 Chrome 관리 코드는 병렬 경로에서 연달아 세 번 문제를 냈다**
  (포트 충돌 → 정리 누락 → use-after-free). 이 부분을 건드릴 때는
  **병렬 수집 실행 검증을 먼저 확보한 뒤** 손댈 것.
- **전역 문자열 치환 금지.** `/Volumes` 를 전역 치환했다가 주석·mac 전용 코드까지 바뀌어
  Windows 빌드가 C2059 로 깨졌다. 항상 해당 지점만 고친다.
- **맥에서 앱이 떠 있는 채로 재빌드하지 말 것.** 재서명이 실행 중인 앱의 서명을 깨서
  macOS 가 죽인다. 크래시 리포트도 안 남아 원인 오판하기 쉽다.
- **"성공했다"는 로그를 믿지 말 것.** DMG 스크립트가 재서명 실패를 경고 한 줄로 삼키고
  "✅ 완료"를 찍고 있었다. Mountain Duck 도 로컬 캐시에 쓴 것을 100% 진행률로 보여준다.
  검증은 **서버 응답이나 실제 산출물**로 한다.
