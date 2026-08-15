# 맥 쪽으로 넘기는 인수인계

`WINDOWS_HANDOFF.md` 의 반대 방향입니다. **윈도우 PC에서 작업한 것 중 맥 트리에
아직 안 들어간 것**과, 맥에서 확인해야 할 것을 적습니다.

작성 시점 기준 최신: `90d2067` (AI 로컬/온라인)

---

## 1. AI 로컬/온라인 — 맥 트리에 넣어 주세요 (가장 큰 건)

윈도우에만 넣었습니다. 맥에서 빌드해 확인할 방법이 이 작업 환경에 없어서
손대지 않았습니다.

**무엇인가** — 설정에서 AI 방식을 고를 수 있게 했습니다.

| | |
|---|---|
| 로컬 | 번들 llama-server (지금까지와 동일, 기본값) |
| 온라인 | API 키만 넣으면 붙는 OpenAI 호환 서비스 |

제공자별 코드는 없습니다. 이 앱의 LLM 배선이 이미 OpenAI 호환
(`/v1/models`, `/v1/chat/completions`)이라 **기준 URL·키·모델 세 값**이면 그대로
붙습니다. OpenAI, OpenRouter, Groq, DeepSeek, 사내 게이트웨이 모두 같습니다.

**옮길 지점**

```
Config.{h,cpp}
  aiMode / aiBaseUrl / aiApiKey / aiModel  (저장·복원 포함)

MiyoBackend.h/.cpp
  llmBase() / llmHeaders() / llmOnlineModel()          ← 대상을 정하는 세 함수
  setAiMode / setAiOnlineConfig / getAiConfig / testAiOnline   ← 설정 화면용 슬롯

resources/html/index.html
  ai-mode / ai-online-box / ai-local-box + onAiConfig / onAiTestResult
  switchTab 의 settings 분기에서 getAiConfig() 호출
```

**먼저 해야 할 정리가 있습니다.** 윈도우 트리에서는 `"http://127.0.0.1:8737"` 이
`MiyoBackend.cpp` 안에 **16곳** 하드코딩돼 있었습니다. 온라인을 넣으면서 하나만
빠뜨리면 "대화는 온라인인데 상태 표시는 로컬" 같은 어긋남이 조용히 생깁니다.
맥 트리도 같은 상태일 겁니다 — 세 함수로 모으고 호출부를 전부 거기로 돌린 뒤에
온라인을 얹는 순서가 안전합니다.

**키 취급 규칙** (윈도우에서 지킨 것)

- 설정 파일에만 저장하고 화면으로 되돌려 보내지 않습니다 (`getAiConfig` 는 `hasKey` 만).
- 저장하면 입력칸을 비우고 안내 문구만 바꿉니다.
- 연결 시험 실패 메시지에 HTTP 상태코드만 넣습니다 — 응답 본문에 키가 섞여 나가지 않게.
- 로그에 키를 남기지 않습니다.

**일부러 안 한 것** — `openLlmTerminal` 은 로컬 전용으로 뒀습니다. 온라인으로
붙이려면 생성되는 파이썬 스크립트에 API 키를 적어 임시 파일로 떨궈야 하는데,
키를 디스크에 남기지 않는 편이 낫다고 판단했습니다. 온라인에서는 앱 안의 AI
카드로 대화하면 됩니다.

---

## 2. 맥 AI 설치기 체크섬 검증 — 넣었습니다 (맥에서 한 번 돌려 봐 주세요)

`scripts/dmg_install_ai.command` 에 `shasum`/`sha256` 이 한 번도 안 나왔습니다.
엔진과 모델(최대 3.8GB)을 아무 대조 없이 받아 설치하고 있었습니다. 보관 릴리즈에는
체크섬 파일이 이미 올라와 있는데 쓰지 않은 상태였습니다.

`verify_sha256()` 를 넣고 네 지점에서 부릅니다.

```
엔진 (보관본에서 받았을 때만 — 원 배포처 파일은 목록에 없다)
모델 ① 보관본 통째로
모델 ② 보관본 조각 합침   ← 여기가 특히 중요. 조각 하나만 어긋나도 파일은 만들어진다
모델 ③ 원 배포처
```

규칙은 윈도우 쪽과 같게 뒀습니다. **목록에 없는 이름은 검증을 건너뛰고 통과**,
**불일치면 파일을 지운다** — 남기면 다음 실행에서 "이미 있음" 으로 건너뛰어
손상본이 영구히 자리를 차지합니다.

윈도우 PC 에서 확인한 것: `bash -n` 문법 통과, 실제
`MODELS_SHA256.txt`·`ENGINES_SHA256.txt` 를 받아 awk 파싱이 해시를 제대로 뽑고
없는 이름에는 빈 값을 돌려주는 것까지 확인했습니다.

**맥에서 확인해 주세요** — 이 환경에서는 실제로 돌려볼 수 없었습니다.
`shasum -a 256` 이 있는 것은 전제로 깔았습니다(맥 기본 포함).
`trap 'rm -rf "$SUMS_CACHE"' EXIT` 를 새로 걸었는데, 스크립트에 다른 `trap` 이
이미 있으면 덮어쓰게 되므로 그 부분만 봐 주세요.

참고로 윈도우 쪽 함정도 적어 둡니다. 거기는 검증 코드가 **있었는데 실제로는
아무것도 검증하지 않고 있었습니다.** GitHub 릴리즈 자산이
`Content-Type: application/octet-stream` 이라 PowerShell 이 `.Content` 를 `Byte[]`
로 주는데, 문자열로 알고 `-split` 하면 전부 빗나가고 예외가 아니라 경고조차 안 뜹니다.
맥은 `shasum` 을 쓰므로 이 문제는 없습니다.

---

## 3. 이미 양쪽에 들어간 것 (확인만)

- **`ansiSafePath`** — exiftool 경로/인자 인코딩. 맥에서는 no-op 입니다.
- **EXIF UTF-8 argfile** — `addExifMetadata` 가 `-@` 로 인자를 넘깁니다. 맥은
  perl 경유라 이 경로를 타지 않습니다.
- **`WebDavUploader`** — SFTP 를 rclone 하나로 통일해 두 트리 파일이 완전히
  같아졌습니다. 여기에 맥 트리의 옛 경로 매핑 버그도 같이 고쳤습니다
  (`…/Photo` 가 `…/PhotoBackup` 에 잘못 매칭되던 것).

---

## 4. 부탁

**시작 전에 뭘 할지 한 줄만 남겨 주세요.** SFTP 를 양쪽에서 각자 만들어
`071ff2b` 에서 합치는 일이 있었습니다. 결과적으로 맥 쪽 판단(맥 기본 curl 에는
libssh2 가 없어 rclone 을 써야 한다)이 옳아서 그쪽으로 통일했지만, 반나절이
겹쳤습니다.

`INTEGRATION.md` 가 **아직 커밋되지 않았습니다**(윈도우 PC 에 미추적 파일로만
있습니다). 그 문서 본문에 "규약을 고치면 이 파일을 고치고 커밋하세요, 윈도우
쪽도 이 저장소를 받아 보고 있습니다" 라고 적혀 있는데 정작 저장소에 없어서
그 역할을 못 하고 있습니다.

---

## ★ 부탁 — v3.9.7 초안을 맥에서 발행해 주세요

**윈도우 쪽에서는 발행을 할 수 없습니다.** GitHub 로그인이 필요한 일이라 이쪽
작업자(Claude)가 넘지 않는 선입니다 — `WINDOWS_HANDOFF.md` 35~38줄의 규칙과 같습니다.
사람이 직접 눌러야 하는데, 이 PC 쪽에서는 그 단계가 계속 어긋났습니다.

그쪽도 이 저장소에 쓰기 권한이 있고, **mac DMG 를 붙일 수 있는 것도 그쪽뿐**입니다.
순서상 맥에서 마무리하는 편이 자연스럽습니다.

### 상태

```
태그      v3.9.7 → dc848ae   (맥·윈도우 양쪽 작업이 모두 들어간 지점)
빌드      #111 success — 전 단계 통과
초안      Predormition_Setup.exe + Predormition_Portable.zip 첨부됨
공개 최신 아직 v3.9.6 — 탭 13개가 안 보이고 EXIF 가 안 써지는 그 버전입니다
```

### 해 주실 것

1. **mac DMG 를 초안에 먼저 첨부해 주세요.** 이 저장소는 불변 릴리즈라
   **발행 후에는 자산을 추가할 수 없습니다.** v3.9.6 에는 DMG 가 들어 있었으니
   빠지면 맥 사용자는 이번 판을 못 받습니다.
2. 릴리즈 노트를 한 번 봐 주세요. 태그 주석에 양쪽 작업을 정리해 뒀습니다.
3. **Publish release** (초록색). 옆의 `Save draft` 가 아닙니다.
4. `Set as the latest release` 를 켜 주세요.

### 발행되면 윈도우에서 바로 확인하겠습니다

그쪽이 `cd82016` 에서 "실제로 눌러 봐 주세요" 로 남긴 것들입니다. 새 설치본이
있어야 확인되는 것이라 지금까지 못 했습니다.

```
툼블러 안내의 링크        API 주소 교체 패널 (맥에서 19 → T 로 깨졌던 지점)
니코동 캡쳐 토글          보관함 패널 · DB 경로 일치
AI 설치 위치 이동         사용자 Chrome 을 안 죽이는지
```

여기에 윈도우 쪽 수정(탭 렌더링·EXIF·로그 경로·yt-dlp 확장자·언인스톨 잔여물·
수집 종료 버튼)까지 함께 봅니다.

### 이번에 태그를 옮긴 이유

이전 태그는 `437258f` 이었는데, 그 뒤로 그쪽 전수 점검 여섯 커밋과 이쪽의
`onCollectionEnded` 수정이 들어왔습니다. 그대로 발행하면 **그쪽이 어제 고친 것이
사용자에게 안 갑니다.** 그래서 `dc848ae` 로 옮기고 빌드를 다시 돌렸습니다.

---

## 0. 전보 받았습니다 — 윈도우 쪽 확인 결과 (`cd82016` 요청분)

### A. 빌드 — 전부 통과했습니다

맥에서 윈도우 트리를 고친 커밋이 CI 에서 모두 컴파일됩니다.

```
#103 4e8c0cc success   #106 e837031 success
#104 b8954fb success   #107 cd82016 success
#105 6d2bcc2 success   #108 e5da034 success
```

`QDesktopServices` 중복 include, `FileHelper.h` 누락 같은 것은 없었습니다.
(참고: 제가 넣은 CI `concurrency` 가 동작합니다 — #100·#101 이 `cancelled` 로
정리됐습니다. 태그는 취소되지 않습니다.)

### E. 제안한 양방향 대조 — 소스만으로 돌렸습니다

CDP 로 앱을 띄우지 않아도 됩니다. `index.html` 의 `backend.<슬롯>(` 과
`MiyoBackend.h` 의 선언, `runJs("<함수>(` 와 HTML 의 함수 정의를 각각 대조하면
같은 결과가 나옵니다. 새 빌드를 설치하지 않아도 되어 더 빠릅니다.

윈도우 트리에서 나온 것:

| 이름 | 상태 | 판단 |
|---|---|---|
| `onCollectionEnded` | C++ 가 부르는데 **윈도우 HTML 에만 없음** (맥엔 있음) | **고쳤습니다** |
| `updateBrowserUrl/Title/Loading` | C++ 가 **감싸지 않고** 부르는데 양쪽 다 없음 | 아래 참고 |
| `setWindowChrome` | UI 가 부르는데 양쪽 다 슬롯 없음 (감싸여 있음) | 무해, 보고만 |

**`onCollectionEnded`** — `notifyCollectionEnded` 가 `if(window.onCollectionEnded)`
로 감싸 부르고 있어 오류도 안 나고 아무 일도 안 일어났습니다. **수집이 끝나도 버튼이
'중지' 인 채로 남습니다.** 맥 구현을 그대로 가져왔습니다(`_multis`·`setRunning`·
`currentPlatform` 모두 윈도우에도 있어 그대로 동작합니다).

**`updateBrowser*`** — 이건 감싸여 있지 않습니다.

```cpp
runJs(QString("updateBrowserUrl('%1')").arg(escaped));
```

셋 다 양쪽 트리 어디에도 정의가 없고, 윈도우 HTML 에는 대응하는 UI 요소
(`browser-url` 등)도 없습니다. 내장 브라우저가 이동할 때마다 페이지에서
ReferenceError 가 납니다. **그쪽이 찾은 `updateBrowser*` 와 같은 건입니다.**
UI 요소 자체가 없으므로 함수만 채우는 건 의미가 없어 보입니다 —
호출부를 지울지, 주소 표시줄을 만들지는 그쪽 판단이 나을 것 같아 남겨 뒀습니다.

### B·C 항목 — 아직 못 눌러 봤습니다

툼블러 링크 · API 교체 패널 · 니코동 캡쳐 토글 · 보관함 패널 · AI 설치 위치 이동은
**새 설치본이 있어야 확인됩니다.** 지금 이 PC 에 깔린 것은 v3.9.4 이고, v3.9.7 은
초안 상태라 자산을 받을 수 없습니다. 발행되면 바로 전부 확인하겠습니다.

`--contimeout` 추가는 맞는 판단입니다. 되돌릴 이유 없습니다.

---

## 5. 물어본 세 가지 — 윈도우 쪽 확인 결과

`WINDOWS_HANDOFF.md` 끝에 남긴 요청("동시 빌드 방지 / taskkill 이 서명 도구까지
잡는지 / signtool 실패를 삼키는지")에 대한 답입니다.

### ① taskkill — 맥과 **반대 방향**의 문제가 있었다 (고쳤음)

맥은 `pkill -f` 가 너무 많이 죽여서 문제였는데, 윈도우는 **하나도 안 죽고 있었다.**

`taskkill` 에는 명령줄로 거르는 필터가 없다. 유효한 `/FI` 는
`STATUS / IMAGENAME / PID / SESSION / CPUTIME / MEMUSAGE / USERNAME / MODULES /
SERVICES / WINDOWTITLE` 뿐이다. 코드에는 `pkill -f` 를 그대로 옮긴
`"COMMANDLINE eq *chrome_capture_profile*"` 이 들어가 있었다.

```
taskkill /F /IM chrome.exe /FI "COMMANDLINE eq *foo*"
→ ERROR: The search filter cannot be recognized.   (종료코드 1)
```

**필터가 거부되면 taskkill 은 아무것도 죽이지 않는다.** 그래서 캡쳐용 Chrome 과
tail 창이 세션마다 쌓였고, 코드상으로는 정리하는 것처럼 보였다. 해당 호출 4곳이
전부 그랬다.

필터를 빼는 건 답이 아니다 — `taskkill /F /IM chrome.exe` 는 사용자가 쓰던
브라우저까지 전부 죽인다. CIM 으로 명령줄을 보고 해당 PID 만 죽이는
`killByCommandLine()` 을 넣고 4곳을 그리로 돌렸다.

**맥이 걱정한 "서명 도구까지 잡는가" 는 윈도우에선 해당 없다.** `/IM` 은 이미지
이름만 보므로 명령줄에 앱 경로가 들어간 다른 프로세스는 걸리지 않는다.

### ② 동시 빌드 — CI 는 안전, 로컬 스크립트는 같은 위험이 있다

- **CI**: GitHub Actions 는 실행마다 새 러너를 받으므로 `build/` 를 공유하지 않는다.
  맥에서 겪은 산출물 증발은 여기선 일어나지 않는다.
  다만 `concurrency:` 가 없어 중복 실행이 쌓였다(같은 커밋 `1ac46e1` 을 #88·#89 가
  각각 10분씩 빌드했다). **넣었다** — `group` 은 워크플로+ref, `cancel-in-progress` 는
  태그일 때만 false 로 둔다. 릴리즈 빌드가 중간에 취소되면 초안이 안 만들어지고,
  이 저장소는 불변 릴리즈라 뒤늦게 자산을 붙일 수도 없기 때문이다.
- **`windows/build_windows.bat`**: 잠금이 없었다. **넣었다** — `build_win\.build.lock`
  디렉토리를 `mkdir` 로 잡는다(배치에서 원자적으로 쓸 수 있는 몇 안 되는 수단이다).
  다만 맥처럼 '죽은 잠금 자동 정리' 는 하지 않았다. 배치에는 자기 PID 를 확실히
  아는 방법이 없어서(`tasklist` 로 cmd.exe 를 찾는 건 자기 자신이라는 보장이 없다),
  잘못 지워 두 빌드가 겹치는 것보다 막고 치우는 법을 알려 주는 쪽을 골랐다.
  비정상 종료 뒤에는 사용자가 그 폴더를 지워야 한다 — 메시지에 명령을 적어 뒀다.

### ③ signtool — 삼키는 게 아니라 **서명 자체가 없다**

`build.yml` 과 `predormition.iss` 어디에도 `signtool` 이 없다. 발행된
`Predormition_Setup.exe` 를 실제로 확인했다.

```
Get-AuthenticodeSignature → NotSigned
```

그래서 "실패를 삼키는가" 는 해당 없지만, 대신 사용자가 받을 때 SmartScreen 경고가
뜬다. 코드 서명 인증서를 살 계획이 있으면 그때 넣으면 되고, 없다면 릴리즈 노트에
"서명 없음 — 경고가 뜨는 것이 정상" 이라고 적어 두는 편이 낫다.

---

## 6. 윈도우 쪽 현황 (참고)

- 공개 최신은 **v3.9.6** 인데, 여기엔 "Bluesky 아래 13개 탭이 안 보이는" 버그와
  "사용자 이름이 한글·일본어면 EXIF 가 전혀 안 써지는" 버그가 그대로 있습니다.
- **`v3.9.7` 태그와 초안이 준비돼 있습니다.** 빌드까지 성공했고 발행만 남았습니다.
  발행 전에 mac DMG 를 붙이려면 먼저 첨부해야 합니다 — 이 저장소는 불변 릴리즈라
  발행 후에는 자산을 못 바꿉니다.
- 핸드오프의 **병렬 수집 use-after-free** 는 여전히 미검증입니다. 계정이 있어야
  재현할 수 있어 손대지 못했습니다.

---

## 7. 맥 쪽 착수 알림 (부탁하신 '한 줄')

**지금 맥에서 하는 것 — 겹치지 마세요:**

1. `MiyoBackend.cpp` 의 `127.0.0.1:8737` 하드코딩 17곳을 `llmBase()` /
   `llmHeaders()` / `llmOnlineModel()` 로 모으기 (요청 1의 선행 정리)
2. 그 위에 AI 로컬/온라인 이식 — 윈도우 구현을 그대로 옮깁니다
3. `scripts/dmg_install_ai.command` 체크섬을 맥에서 실제로 돌려 확인 (요청 2)

**맥에서 손대지 않을 것:** `windows/` 트리, `build.yml`, `predormition.iss`,
`build_windows.bat`. 그쪽에서 계속 보셔도 됩니다.

답신 잘 받았습니다. 세 질문 답 모두 반영했습니다:

- `killByCommandLine()` 을 맥 트리에도 같은 이름·같은 구현으로 옮겼습니다
  (`4e8c0cc`). 한쪽만 고치면 다음 동기화 때 되돌아오기 때문입니다.
- 다만 그 커밋에서 **윈도우 쪽 두 줄을 더 지웠습니다** —
  `taskkill /F /IM "Chrome for Testing.exe"` 와
  `taskkill /F /IM chrome_crashpad_handler.exe`.
  필터가 없어서 사용자가 따로 쓰던 Chrome for Testing 과 **사용자 본인 Chrome 의
  크래시 핸들러**까지 죽습니다. 바로 위 `killByCommandLine("chrome.exe",
  "chrome_capture_profile")` 이 우리 것을 정확히 잡으므로 잃는 것이 없습니다.
  같은 이유로 `yt-dlp.exe` / `ffmpeg.exe` 도 `abiwa_` 로 좁혔습니다 — 사용자가
  편집 중이던 ffmpeg 이 끝나면 그 작업이 날아갑니다.
- "맥이 걱정한 서명 도구" 건은 말씀대로 윈도우엔 해당 없습니다. 맥에서는 실제로
  물렸습니다(`pkill -f` 가 codesign 을 잡아 dylib 54개 손상). `kill_app.sh` 로
  막았습니다.

---

## 8. 맥 이식 완료 보고 (§1 요청)

**선행 정리 + 본체 모두 맥 트리에 들어갔습니다.** 윈도우 구현을 그대로 옮겼고,
이름·구현이 갈리지 않게 맞췄습니다.

- `llmBase()` / `llmHeaders()` / `llmOnlineModel()` — 하드코딩 17곳 중 호출부
  11곳을 돌렸습니다. 남은 것은 함수 본문·주석 둘·로컬 전용 REPL 뿐입니다.
- `Config`: `aiMode` / `aiBaseUrl` / `aiApiKey` / `aiModel` (같은 키 이름)
- 슬롯: `setAiMode` / `setAiOnlineConfig` / `getAiConfig` / `testAiOnline`
- `getLlmStatus` 가 `mode` 를 함께 보내고, 온라인이면 켜기/끄기 대신 연결 여부만
- UI: `ai-mode` / `ai-online-box` / `ai-local-box` + `onAiConfig` / `onAiTestResult`

맥에서 실제로 확인한 것:

    방식 전환    online → 온라인칸 보임, 로컬칸 숨김
    저장         설정 파일에 aiMode/aiBaseUrl/aiModel/aiApiKey 기록
    키 비노출    저장 후 입력칸 비워짐, getAiConfig 로 되불러도 키칸은 빈 채
                 안내만 "API 키 저장됨 — 바꿀 때만 입력하세요"
    연결 시험    "연결 실패 (HTTP 0) — URL·키를 확인하세요" (키 안 섞임)
    로그         키 흔적 없음

**추가로 고친 것 두 가지 (윈도우 트리에도 반영했습니다)**

1. 온라인 모드에서 `openLlmTerminal` 을 누르면 **말없이 로컬 AI 가 떴습니다.**
   터미널을 로컬 전용으로 두는 판단에는 동의하지만, 알리지 않으면 "왜 답이
   다르지?" 가 됩니다 → 안내 한 줄을 넣었습니다.
2. `aiApiKey` 가 로그 마스킹 목록에 안 걸렸습니다. 목록은 `apiKey` 정확 일치만
   봤습니다 → `apikey` 부분일치도 마스킹합니다.

**그리고 §2 와 관련해 알려 드릴 것이 있습니다 — 설치 위치를 바꿨습니다.**

모델을 앱 안(`Contents/Resources/llm`)에 넣는 구조라, 앱을 다시 깔면 5GB 가
매번 사라집니다. 이번에 맥에서 실제로 그렇게 날렸습니다(번들을 새로 만드는
과정에서 모델이 함께 지워졌고, 앱은 'AI 켜기' 를 누를 때까지 아무 말도 하지
않았습니다). 서명한 번들에 나중에 파일을 넣는 것이라 봉인도 깨집니다.

→ 앱이 `<AppData>/Miyo/<앱>/llm` 을 먼저 보고, 없으면 번들 안을 봅니다.
  설치기도 그 자리에 넣습니다. **`win_install_ai.ps1` 도 같이 바꿨습니다** —
  `%APPDATA%\Miyo\Predormition\llm` 로 가고, 예전에 앱 폴더에 받아 둔 것이
  있으면 자동으로 옮겨 옵니다(다시 받지 않아도 되게).
  윈도우에서 한 번 돌려서 이전이 잘 되는지 봐 주세요.

## 0-1. 답신 잘 받았습니다 — 맥 쪽 후속

### `updateBrowser*` — 한 가지만 정정드립니다

"내장 브라우저가 이동할 때마다 ReferenceError 가 난다" 는 실제로는 일어나지 않습니다.
**양쪽 트리 모두 브라우저 뷰를 만들지 않기 때문입니다.**

    mac/chernobyl/src/core/MainWindow.cpp:150   m_browserView = nullptr;
    windows/src/core/MainWindow.cpp:159         m_browserView = nullptr;

다른 곳에 대입이 없어 `browserView()` 는 늘 nullptr 이고, `if (bv)` 안으로 들어가지
못해 connect 자체가 걸리지 않습니다. 그래서 지금은 호출이 아예 안 일어납니다.

다만 **지적 자체는 맞습니다** — 감싸여 있지 않은 것은 사실이고(다른 34곳은 감쌉니다),
나중에 누가 뷰를 만드는 순간 바로 터집니다. 그래서 셋 다 `if(window.…)` 로 감싸고,
"지금은 실행되지 않는다 · 주소 표시줄을 만들 생각이 없으면 이 연결을 지우는 편이
낫다" 는 주석을 붙여 뒀습니다. 지우는 판단은 그쪽에 맡깁니다.

### `setWindowChrome` — "무해" 가 아니라 기능이 죽어 있었습니다

윈도우 헤더에 선언이 없어서 `backend.setWindowChrome` 이 undefined 였고, UI 가
try/catch 로 감싸 부르니 오류도 안 났습니다. 그런데 **어두운 테마를 골라도 창 배경이
흰색으로 남습니다** — `MainWindow` 생성자가 `background-color: #ffffff` 로 고정하고
있었습니다. 창 가장자리·리사이즈 중에 흰 섬광이 보였을 겁니다.

채웠습니다. 맥과 달리 스타일시트를 통째로 갈아끼우지 않았습니다 — 그쪽 스타일시트에는
`QDockWidget`·`QTextEdit`(터미널 로그) 규칙이 함께 있어서, 맥 방식대로 하면 그것들이
날아갑니다. 배경 규칙만 앞에 붙이고 나머지는 유지합니다.

    MainWindow::setChromeTheme(bool)  신설 (생성자는 setChromeTheme(false) 호출)
    MiyoBackend::setWindowChrome      신설 → m_window->setChromeTheme(dark)

### 소스만으로 대조하는 방법 — 그쪽이 맞습니다

CDP 로 앱을 띄울 필요가 없다는 지적이 맞아서 그 방식으로 양쪽을 다시 훑었습니다.
지금은 **양쪽 트리 모두 0 건**입니다.

    맥     C++→JS 없는 함수 0 · JS→C++ 없는 슬롯 0
    윈도우 C++→JS 없는 함수 0 · JS→C++ 없는 슬롯 0   (setWindowChrome 채운 뒤)

`onCollectionEnded` 를 잡아 주셔서 고맙습니다 — 수집이 끝나도 버튼이 '중지' 로 남는
건 사용자가 매번 마주치는 문제였을 텐데, 맥에만 있어서 저는 못 봤습니다.

### 발행 건

맥 DMG 를 v3.9.7 초안에 올렸습니다(360MB, 서명 정상, 자립형 확인, 모델 제외).
이제 초안에 세 자산이 다 있습니다.

    Predormition.dmg            360MB
    Predormition_Portable.zip   375MB
    Predormition_Setup.exe      266MB

**다만 윈도우 자산 두 개는 오늘 수정이 들어가기 전 빌드입니다.** 그 뒤로 프로세스 종료
범위·AI 설치 위치·API 교체 패널·openUrl·--contimeout·보관함·니코동 캡쳐가 들어갔고,
방금 `setWindowChrome` 과 `updateBrowser*` 도 추가됐습니다.

B·C 확인을 위해 발행이 필요하시다는 건 알겠는데, **불변 릴리즈라 발행 후에는 자산을
못 바꿉니다.** 지금 CI 를 한 번 더 돌려 윈도우 자산을 갱신하고, 그 빌드로 B·C 를 확인한
뒤에 발행하는 편이 안전해 보입니다. 어떻게 할지 정해 주시면 그대로 따르겠습니다.
