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

## 2. 맥 AI 설치기에 체크섬 검증이 없습니다

`scripts/dmg_install_ai.command` 에 `shasum`/`sha256` 이 한 번도 안 나옵니다.
엔진과 모델(최대 3.8GB)을 아무 대조 없이 받아 설치합니다.

보관 릴리즈에는 체크섬 파일이 **이미 있습니다.**

```
ai-engines-v1/ENGINES_SHA256.txt
ai-assets-v1/MODELS_SHA256.txt
```

실제로 받아서 대조해 봤고 공개된 해시가 실제 파일과 일치합니다. 쓰기만 하면 됩니다.

윈도우 쪽에서 겪은 함정도 적어 둡니다. 거기서는 검증 코드가 있었는데 **실제로는
아무것도 검증하지 않고 있었습니다.**

```powershell
$sums = (Invoke-WebRequest ".../ENGINES_SHA256.txt").Content   # Byte[] 로 온다
$line = ($sums -split "`n" | Where-Object { $_ -match $name })  # 항상 null
```

GitHub 릴리즈 자산은 `Content-Type: application/octet-stream` 이라 PowerShell 이
`.Content` 를 `Byte[]` 로 줍니다. 문자열로 알고 쓰면 전부 빗나가고, 예외가 아니라
`catch` 의 경고조차 안 뜹니다. 맥은 `shasum -c` 를 쓰면 이런 문제가 없습니다.
다만 **불일치일 때 받은 파일을 지우는 것**은 잊지 마세요 — 남기면 다음 실행에서
"이미 있음" 으로 건너뛰어 손상본이 영구히 자리를 차지합니다.

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
  다만 `concurrency:` 설정이 없어 **중복 실행이 쌓인다**(같은 커밋 `1ac46e1` 을
  #88·#89 가 각각 10분씩 빌드한 것을 봤다). 망가지진 않고 시간만 버린다.
  `main` 은 `cancel-in-progress: true`, 태그는 취소하면 안 되므로 제외하는 형태를
  권한다 — 릴리즈 빌드가 중간에 취소되면 초안이 안 만들어진다.
- **`windows/build_windows.bat`**: 잠금이 없다. 맥의 `build/.build.lock` 에 해당하는
  것이 없으므로 두 번 겹쳐 돌리면 같은 문제가 난다. 아직 안 고쳤다.

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
