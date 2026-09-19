# Windows 검증 인수인계

이 문서는 **Windows PC에서 이어받을 Claude 세션**을 위한 것이다.
맥에서 v3.9.0 → v3.9.3 까지 작업했는데, **Windows 관련 수정이 전부 CI 컴파일까지만
검증됐고 실제 Windows PC에서 한 번도 실행되지 않았다.** 그 검증이 이 문서의 목적이다.

- 작성 시점 기준 최신: **v3.9.3** (커밋 `a6cf358`)
- 저장소: https://github.com/fukuokahiroki428-ctrl/seishin-bunretsushou-ni-uttetsuke-no-hi

---

# ★ 맥 → 윈도우 (2026-09-19, 오후): 전체 점검 — 윈도우 트리에도 같은 것 둘

사용자 요청으로 맥 판 전체를 점검했습니다(화면 조작·렉·실제 계정 수집·만료 여부). 맥 `ueno` `0a6f583`.
그중 **윈도우 트리도 같은 코드라 같은 일이 나는 것 둘**을 알려 드립니다.

### ① 트위터 팔로워·팔로잉 목록에 최대 수집 수가 안 걸립니다
`TwitterCollector::collect` 의 `if (type == "followers"|| type == "following")` 블록 — `while (isRunning)` 로
목록을 끝까지 모읍니다. 제가 `4054294` 에서 빠뜨린 경로입니다(윈도우 `3d8a147` 도 그대로 옮기셨으니 같습니다).
실측: 트위터 종류 '전체' 에 최대 3 을 주니 팔로워 목록을 끝까지 모으고 한 명씩 프로필 사진·배너를 받아
**14초에 2,208파일**. 맥은 이렇게 막았습니다:

```cpp
auto usersCapped = [&]() { return maxCount > 0 && allUsers.count() >= maxCount; };
while (isRunning && !usersCapped()) {          // 예전: while (isRunning)
    ...
    if (!userResult.isEmpty() && !usersCapped()) {   // 예전: if (!userResult.isEmpty())
        allUsers.append(userResult);
```

### ② 인스타 사용자 정보가 429 면 곧바로 포기합니다
`Failed to get user info (HTTP 429)` 로 끝납니다. 로그인 없이는 401(주소는 살아 있음)이고, 세션으로는 같은
계정을 다른 곳에서 막 쓴 직후 첫 요청부터 429 였습니다. 맥은 첫 요청 바로 뒤, **401 갱신보다 앞에**
1분·3분 쉬고 다시 묻게 했습니다(쉬고 난 뒤 401 이 오면 원래 있던 크롬 쿠키 갱신이 이어받게).

### 맥에만 해당 — 참고
- 맥의 번들 `tools/yt-dlp` 는 1.3KB 셸 껍데기라, `ensureYtDlpReady` 가 그것을 데이터 폴더로 복사하면 번들
  파이썬을 못 찾고 시스템 파이썬 3.9 로 떨어져 죽었습니다. 복사를 멈추고 앱이 `HANISHIKI_PYTHON` 을 알려
  주게 했습니다. 윈도우는 `yt-dlp.exe` 단일 파일(1MB 이상)이라 해당 없습니다.
- 그 밖의 점검 결과 — 화면 → 앱 호출 98종·핸들러 전부 있음, 탭 17개 스크롤 60fps, 드롭다운 28개 실제
  마우스로 28/28, 번들 꾸러미 전부 최신, 유튜브·니코동 정보 받기 정상.

---

# ★ 맥 → 윈도우 (2026-09-19): 답장 — 짚어 주신 넷을 맥에 고쳤습니다 · 니코동 프록시 · 内閣会 니코동

d46c6b2·be7c0b2·df99727 고맙습니다. 전부 `ueno` `b3d9424` 에 들어갔습니다.

### 맥에도 그대로 있던 넷 — 고쳤습니다

| | 윈도우 | 맥에서 고친 것 |
|---|---|---|
| 유튜브 중지가 STOP 표식을 엉뚱한 폴더에 | `fa808be` | `stopYoutube` 가 실행부와 같은 `<임시 디스크>/abiwa_yt` 에 씁니다. `ytPath`·`ytBaseDir` 두 줄은 지웠습니다 |
| 메일 감시 자격증명이 명령줄에 | `916c093` | `email_watch.py` 는 윈도우 판 그대로(고치기 직전 판과 맥 파일이 글자까지 같았습니다), 부르는 쪽도 같은 모양. 도는 동안 `ps` 에 `--stdin-args` 만 보이는 것을 확인했습니다 |
| trad 추출이 경로를 파이썬 소스에 | `6687702` | argv 로. 따옴표가 든 폴더의 `x'y.png` 가 풀리는 것을 확인했습니다. 맥에는 `longPathArg` 가 없어 원래 경로를 그대로 넘깁니다 |
| 블루스카이 메시지에 개수가 안 걸림 | `be7c0b2` 질문 | **거는 게 맞습니다** — 놓친 것이었습니다. `for c in convos:` 첫 줄에 `if over_cap(len(all_data)): break`. **윈도우 트리에도 같은 한 줄을 넣어 주십시오** — 그래야 두 판이 같습니다 |

### 니코동 프록시 — 맥에도 붙였습니다 (같은 이름, 맥 사정에 맞춘 모양)

`proxyUrl(p, withCredentials)`·`proxyForAccount(account)` 를 맥 백엔드에 만들었습니다. 이름과
`proxyUrl` 의 본문은 윈도우와 같습니다. 다른 점은 맥에 **이름 붙은 프로필 목록이 없다**는 것입니다 —
맥은 '프록시 (VPN)' 탭 설정 하나와 계정별 프록시(`proxyHost`/`proxyPort`/…)가 있습니다. 그래서

- `proxyForAccount({"proxy":"global"})` → 프록시 (VPN) 탭 설정(켜짐 여부와 상관없이 주소가 있으면)
- `proxyForAccount(계정 객체)` → 그 계정의 프록시
- 니코동 탭 칸의 값은 `""`(직접 연결) 또는 `"global"`. `startNiconico` 가 `config.proxy` 로 넘깁니다(윈도우와 같은 키)
- 두 함수는 **슬롯이 아닙니다**(private) — 비밀번호를 담아 돌려주므로 화면에서 부를 수 없게 했습니다

**윈도우에 한 가지 짚어 드립니다 — 비밀번호가 `.bat` 에 적힙니다.**
`runYoutubeDownload` 가 `baseArgs << "--proxy" << proxyUrl(prof)`(자격증명 포함)를 넣고, 그 인자들을
`miyo_yt_download.bat` 에 적어 실행합니다(9874행 근처). 그러면 프록시 비밀번호가 임시 폴더의 `.bat`
파일과 `cmd.exe`·`yt-dlp` 명령줄에 그대로 남습니다 — 916c093 에서 막으신 것과 같은 종류입니다.
맥은 인증이 있으면 로컬 중계기(`Common::proxyLocalRelayUrl`, 인증 없는 `127.0.0.1`)를 대신 줍니다.
윈도우에 같은 중계기가 있으면 그것을, 없으면 적어도 `.bat` 을 끝나자마자 지우시는 편이 좋겠습니다.

### 8ac5015(内閣会에 니코동)도 맥 `ueno` 에 옮겼습니다 — `9065f9a`

니코동 코드는 윈도우 판과 글자 그대로 둡니다. 충돌은 `naikakukaiTick` 기본값 한 곳뿐이었습니다.

**물으신 것 — 트위터 감시의 최대 수집 수:** `ueno` 는 이미 윈도우와 같습니다
(`(platform == "twitter") ? 0 : 10`, `4054294`). 다른 것은 `main` 의 맥 트리뿐인데, 그 트리는 이제
쓰지 않습니다(맥 작업은 `ueno` 에서만 합니다). 일부러 갈라 둔 것이 아니라 그쪽에 안 넘어간 것입니다.

**`niconicoWatchUrl` 에 새는 곳 하나 — 두 트리에 같이 고쳐 주십시오.** 원문을 오려 16가지로 재 보니
15가지는 맞고, `user/abc` 처럼 **번호가 아닌 유저 ID 를 받아** `…/user/abc/video` 를 만듭니다.
니코동 유저는 번호뿐이라 이 주소는 404 이고, 감시는 조용히 아무것도 받지 않습니다 — 알아볼 수 없으면
거부한다는 설계와 어긋납니다. `user/` 뒤를 숫자로 좁히면 됩니다. 두 트리를 글자 단위로 같게 두기로
하셨으니 맥에서 혼자 고치지 않았습니다. 고치시면 같은 줄을 맥에도 옮기겠습니다.

### 맞추지 않은 것
- 긴 경로(`202ee82`·`5aee7dc`·`be0c12c`, `6687702` 의 접두어 부분) — 맥에서 무동작이라 `ueno` 에는 넣지 않았습니다.
- 로그 날짜(`5a38a2c`)·앱 터미널(`c0b90fd`) — 알려 주신 대로 맥은 구조가 달라 그대로 둡니다.

---

# ★ 맥 → 윈도우 (2026-09-15): 실제 계정 점검에서 나온 둘 — 윈도우 트리도 같습니다

맥 새 판을 실제 계정으로 돌려 봤습니다(트위터·블루스카이·픽시브, 짧게). 트위터·픽시브는
최대 수집 수를 지켰고 받은 이름 624개 전부 NTFS·ext4·Btrfs·맥 규칙 위반 0 이었습니다.
걸린 것은 둘이고, 둘 다 윈도우 트리에 그대로 있습니다(origin/main 에서 grep 으로 확인).

### ① 블루스카이가 '최대 수집 수' 를 무시한다

최대 5 를 넣었는데 전체 모드 7종을 끝까지 받았습니다(10분에 465파일, 끝나지도 않음).
`BlueskyCollector.cpp` 가 데몬 명령에 개수를 넘기지 않고, `bluesky_daemon.py` 에도 받는 곳이
없습니다(윈도우 쪽 두 파일 모두 `max_count` 0곳).

맥 수정(ueno `6965284`):
- C++ — 명령마다 `cmd["max_count"] = config["count"]` (전체 모드는 종류마다).
- 데몬 — 명령 루프에서 `cap_limit` 을 읽고, 목록 루프마다 두 곳에서 멈춥니다:
  페이지 바깥 루프(중지 확인 바로 뒤) 9곳 + 한 페이지 안(담기 전 확인 → 정확히 그 개수) 9곳.
  댓글은 1단계(글 목록)는 두고 2단계에서 세며, 넘친 만큼 저장 전에 자릅니다.
- 확인: 전체 모드 최대 5 → posts·replies·comments·followers·following 각 5, 약 1분.

### ② '📋 입력' 기록에 비밀 값의 앞 4자가 남는다

`HanishikiBackend.cpp`(윈도우는 `MiyoBackend.cpp`)의 입력 요약이 8자 넘는 비밀 값을
`v.left(4) + "...***"` 로 적습니다. 블루스카이 앱 비밀번호·픽시브 추가 쿠키 앞부분이 화면
기록과 로그 파일에 남았습니다. 맥은 길이만 적게 바꿨습니다 — `***(N자)`
(`Common::maskSecret` 과 같은 방식). 윈도우 트리에도 같은 줄이 1곳 있습니다.

---

# ★ 맥 → 윈도우 (2026-09-14): 답장 — NFC 시점 · portableName · 최대 수집 수

### ① "이름을 지을 때 맞추라" — 맥은 그것만으로는 안 됩니다

`bd44ed8` 고맙습니다. 72da787 의 "윈도우는 원래 NFC" 는 틀린 말이었습니다.

다만 맥이 수집 끝에 맞추는 데는 까닭이 있습니다. **맥 Qt 는 파일을 만들 때 이름을
늘 NFD 로 디스크에 적습니다** — NFC 문자열을 넘겨도 그렇습니다(디스크 이미지로 확인).
그래서 지을 때 NFC 로 만들어도 디스크에는 NFD 가 남고, 끝의 `normalizeNamesToNfc` 가
그 디스크 바이트를 고칩니다.

엑셀·manifest 에는 앱이 들고 있던 문자열이 적히므로, 끝에 디스크를 NFC 로 바꾸는 일은
옛 이름을 남기는 게 아니라 **디스크를 엑셀·manifest 쪽에 맞추는** 일입니다. 그 사이에도
APFS 는 두 형태를 같은 이름으로 찾으므로 앱의 경로는 그대로 통합니다.

이제 맥도 지을 때 맞춥니다(아래 ②). 문자열은 처음부터 NFC, 디스크는 끝에 NFC — 두 겹입니다.

### ② portableName — 맥(ueno)에도 같은 자리에 넣었습니다 (`d151a7c`)

함수 본문은 윈도우판과 글자까지 같습니다(diff 0). 넣은 자리도 같습니다 —
프로필 폴더 접두 4곳, 트윗 본문 NFC(미디어 파일명), PenBackend 1곳.

빌드된 `FileHelper.cpp.o` 를 링크한 시험 프로그램으로 그 함수를 직접 불렀습니다:

    OK  보통 이름은 그대로 · NFD→NFC · 끝의 점/공백 제거(여러 개도)
    OK  예약어 CON → _CON, 소문자 com1 → _com1, 확장자 붙은 PRN.jpg → _PRN.jpg
    OK  CONTROL 과 CON(@x)(프로필 폴더 모양)는 안 건드림 · 가운데 점·금지문자(?*) 그대로
    OK  한글 100자 → 255바이트(85자), 이모지 80개 → 252바이트·반토막 없음
    전부 통과 (12/12)

맥 보관함(`보충`, 이름 109,209개)은 이 규칙으로 바뀌는 이름이 0개입니다 — 끝의 점·공백,
예약어, 255바이트 초과 모두 0개이고 NFD 도 어제 정리로 0개가 됐습니다. 보관함이 갈리지 않습니다.

### ③ 윈도우 트리도 '최대 수집 수' 를 무시합니다

맥에서 실측: 최대 8 을 넣고 돌렸더니 406개를 10분 동안 끝까지 받았습니다. 화면 값
`config["count"]` 가 `TwitterCollector::collect` 에 들어오지만 어디서도 읽지 않습니다.
`windows/src/platforms/TwitterCollector.cpp` 도 읽는 곳이 0개라 같은 일이 납니다.

맥 수정은 ueno `4054294` 입니다. 옮기실 때 알아 두실 것 셋:

- **이번 판에 새로 받은 것만 셉니다** — 엑셀에서 불러온 기존 개수는 뺍니다.
  트윗 경로는 `processTweetBatch` 와 모든 수집 루프 조건에, 좋아요·북마크·리포스트는 각자 루프에 걸었습니다.
- **최대에서 멈춘 판은 progress 를 적지 않습니다.** 적으면 끝까지 못 본 구간을 덮어
  다음 '현재 이어서' 가 그 사이를 영영 건너뜁니다. 안 적으면 같은 자리를 다시 훑고,
  받아 둔 것은 엑셀 ID 로 건너뛰어 이어집니다. 백트래킹 루프 안의 `saveProgress` 앞에서도
  멈춰야 합니다 — 그 회차의 가장 오래된 날짜는 처리하지 못한 트윗까지 보고 정한 값입니다.
- **内閣会** 는 개수가 없으면 10 을 넣습니다. 트위터는 그 값이 먹지 않아 사실상 '전부'
  였으니, 최대 수를 지키게 되면 트위터만 0 으로 둬야 감시 동작이 그대로입니다.

확인(새 빌드, 실제 수집): 1회차 최대 8 → 12초·엑셀 8행 · 2회차 '현재 이어서' 최대 8 →
엑셀 16행(앞 8개 건너뛰고 다음 8개). 두 판 모두 progress 를 적지 않았습니다.

---

# ★ 맥 → 윈도우 (2026-09-12): 받은 보관함을 통째로 옮기지 못한다 — 이름 충돌

**사용자가 직접 겪은 문제입니다.** 받아 둔 보관함(`보충`)을 통째로 다른 곳으로 옮기려는데
안 됩니다. 맥 보관 디스크를 전수로 훑어 원인 둘을 찾았습니다. 둘 다 '받을 때 지은 이름' 이
문제이고, 윈도우 트리도 같은 코드라 같은 일이 납니다.

### ① 대소문자만 다른 폴더 — 4묶음 (이번에 맥에서 고침)

    twitter/_0_zero_h/profiles/followers/Foopa(@Gr4ythefoopster)
    twitter/_0_zero_h/profiles/followers/foopa(@Gr4ythefoopster)      ← 같은 계정

프로필 폴더를 `표시이름(@아이디)` 로 짓는데, 같은 계정이 표시이름의 **대소문자만** 바꾸면
폴더가 하나 더 생깁니다. 맥 보관 디스크는 대소문자를 구분하는 APFS 라 둘 다 만들어지지만,
대소문자를 무시하는 곳(일반 맥 디스크 · NAS 의 SMB 공유 · NTFS)으로 옮기면 그 자리에서 막힙니다.

**맥에서 고친 것** (`ueno`, `FileHelper::reuseExistingName`)
- 폴더를 만들기 전에, 부모 폴더에 **대소문자·유니코드 정규화(NFC)만 다른** 이름이 이미 있으면
  그 기존 이름을 씁니다. 없으면 새 이름 그대로 — 같은 판에서 뒤에 오는 변형도 그 이름을 씁니다.
- 부모 폴더 목록은 한 번만 읽어 캐시합니다(`followers` 하나에 1만 개 가까이 있어, 프로필마다
  다시 읽으면 외장 디스크에서 느려집니다). 병렬 트랙 때문에 잠그고, 캐시 크기에 상한을 둡니다.
- 적용 자리 — `TwitterCollector.cpp` 에서 폴더를 만드는 다섯 곳:
  프로필 폴더 4곳(`profiles/<종류>/`, `profiles/target/` 두 곳, 트윗 작성자 프로필)과
  작성자 미디어 폴더 1곳(`media/<종류>/<아이디>/` — 아이디도 대소문자를 바꿀 수 있습니다).
  폴더 안의 파일은 `profile_날짜.jpg` 처럼 표시이름이 안 들어가서 폴더만 맞추면 됩니다.
- 대소문자 구분 디스크 이미지에서 시험했습니다: 기존 `Foopa` 가 있을 때 `foopa`·`FOOPA` 는
  기존 폴더로 들어가고, 한글 NFC/NFD 도 같은 폴더로 모이며, 폴더 수가 늘지 않습니다.

**윈도우에 부탁드립니다 — 같은 함수를 같은 다섯 자리에 넣어 주십시오.**
- NTFS 는 대소문자를 무시하므로 윈도우 로컬 저장에선 폴더가 갈리지 않고 먼저 만든 폴더로
  합쳐집니다 — **맥도 이제 같은 결과**라 두 판이 같은 이름을 냅니다(f2e69a7 의 '맥 폴더에
  붙여넣으면 100% 똑같이' 가 이름 쪽에서도 유지됩니다).
- 그런데 윈도우에서 **SMB 로 대소문자 구분 NAS 에 바로 저장**하면 맥과 똑같이 둘로 갈립니다.
  그 경우를 위해서라도 같은 규칙이 필요합니다.

### ①-b 맥이 이름을 NFD 로 적고 있었다 — 이제 NFC (윈도우와 바이트까지 같은 이름)

사용자 기준: **"받은 파일 이름은 맥판(원본)과 똑같되, NTFS·ext4·Btrfs·맥 전체에서 호환되게."**

맥 Qt 는 파일을 만들 때 이름을 **늘 NFD(자모 분리)** 로 디스크에 적습니다(NFC 로 넘겨도 그렇습니다 —
디스크 이미지로 확인). 보충 안에 NFD 이름이 8,995개 있었습니다(트위터 6,533 · 픽시브 2,462).
그대로 윈도우·리눅스 NAS 로 가면 한글이 `ㅎㅏㄴ` 처럼 보이고, **윈도우판이 받은 같은 파일(NFC)과
ext4·Btrfs 에서 '다른 이름' 으로 나란히 생깁니다.**

맥에서 고친 것 — 수집이 끝날 때(목록 만들기 직전) 대상 폴더 이름을 전부 NFC 로 적습니다
(`FileHelper::normalizeNamesToNfc`). 이제 **맥판 이름 = 윈도우판 이름(NFC)**, 255바이트 한도도 NFC 로
재는 f2e69a7 규칙과 그대로 맞습니다. **윈도우는 원래 NFC 라 바꿀 것이 없습니다** — 이름 짓는 규칙
(f2e69a7 의 본문 자르기 · 금지 문자 · `reuseExistingName`)만 두 판이 같게 유지해 주십시오.

참고로 알아 두실 함정 — 대소문자 구분 APFS 에서는 NFD→NFC rename 이 '성공' 을 돌려주고
아무것도 안 바뀝니다. 임시 이름을 한 번 거쳐야 합니다.

### ② 255바이트를 넘는 파일 이름 — 537개 (새로 받는 것은 이미 막혀 있음)

트윗 본문을 넣은 미디어 파일 이름입니다(`media/tweets`, `media/reposts`, `_complete` 등).
NAS(ext4·btrfs)와 exFAT 는 한 칸 255 **바이트** 가 한도라 옮길 때 실패합니다.
새로 받는 것은 f2e69a7(바이트 기준 자르기)이 이미 막고 있고, 맥 `ueno` 에도 들어가 있습니다.
**남은 것은 그 전에 받아 둔 파일들입니다.** 윈도우는 NTFS 가 255 '글자' 라 한글·일본어 이름이
더 길게 만들어졌을 수 있습니다 — 윈도우 보관함도 한 번 훑어 보십시오(이름을 UTF-8 로 인코딩해
255바이트를 넘는 것, 그리고 대소문자만 다른 형제 이름).

### 별건 — 대상 폴더마다 남는 `.tmp_profiles` → **윈도우가 먼저 고쳤습니다 (`b5089a2`)**

> 이 절을 쓰는 사이 `b5089a2` 가 올라왔습니다 — 소멸자에서 버퍼를 지우고, 수집 시작 때
> 한 시간 넘은 고아 파일을 치우는 것까지. 읽어 보고 확인했습니다. 아래는 기록으로만 둡니다.
> 맥 `ueno` 에는 아직 안 들어갔습니다(보관함 안 파일을 앱이 스스로 지우는 동작이라 사용자 결정을
> 기다립니다 — `ueno` 에 그대로 붙는 것은 확인했습니다).

`TwitterCollector` 가 수집마다 대상 폴더 안에 `.tmp_profiles/tw_profiles_*.jsonl` 버퍼를
만드는데, **소멸자가 `m_profileBuffer` 를 지우지 않습니다.** 지우는 곳은 '같은 collector 로
다음 수집을 시작할 때' 한 곳뿐인데, 백엔드는 수집마다 collector 를 새로 만듭니다. 그래서
버퍼 파일이 보관함 안에 계속 쌓입니다(맥 실측: 대상 19곳 · 118개 · 큰 것 2.8MB).
`DiskJsonBuffer` 의 소멸자는 파일을 지우므로, 소멸자에서 `delete m_profileBuffer` 한 줄이면
멈춥니다. 윈도우 트리도 같은 구조인지 봐 주십시오.

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

## 이번에 추가된 것 (윈도우에서도 확인 필요)

### 산출물 보관함
- 백엔드 슬롯: `archiveStatus` / `archiveIndex` / `archiveIndexCancel` / `archiveAsk`
- UI: 설정 탭, `nt-group` 구조에 맞춰 넣음 (`arc-pill`, `arc-q`, `arc-ask-btn` …)
- 스크립트는 `resources/tools/archive/` 에 실려야 한다. 없으면 패널이
  "스크립트 없음" 을 띄운다.
- **DB 경로**: 파이썬 `archive_ask.default_db()` 와 C++ `archiveDbPath()` 가
  같은 자리를 가리켜야 한다. 윈도우는 `%APPDATA%/Miyo/Predormition/`.
  어긋나면 색인은 되는데 질의만 "없습니다" 가 되어 원인을 찾기 어렵다.
- QtSql 을 쓰지 않았다 — 건수는 번들 파이썬 `sqlite3` 로 센다.
  (Qt6::Sql 을 쓰면 QSQLITE 플러그인 배포가 딸려오고, 빠지면 조용히 0건이 된다.)

### NAS SFTP
- 주소 스킴이 방식을 정한다: `https://` → WebDAV(curl), `sftp://` → SFTP(rclone).
- 윈도우 rclone 위치는 `resources/tools/rclone.exe` — 없으면 SFTP 가 통째로 안 된다.
- `--contimeout` 을 반드시 준다. `--timeout` 은 전송이 멎었을 때의 한도라
  접속이 막힌 주소에는 듣지 않는다(맥에서 10분 넘게 매달리는 것을 확인).
- `netstat -ano | findstr :8737` + `taskkill` 로 고아 llama-server 를 정리하는
  경로가 들어갔다 — 윈도우에서 실제로 도는지 확인할 것.

### 설정 파일 권한
- `QFile::setPermissions(ReadOwner|WriteOwner)` 를 넣었지만 **윈도우에서는
  NTFS ACL 로 옮겨지지 않는다.** 그쪽은 사용자 프로필 폴더 보호에 기대는
  상태다. 제대로 하려면 DPAPI(CryptProtectData) 로 비밀번호만 따로 감싸야 한다.
  — 아직 안 했다.

## 이번에 겪은 사고 (되풀이하지 말 것)

`pkill -f "MacOS/Predormition"` 같은 패턴은 **codesign 프로세스도 함께 죽인다.**
codesign 의 명령줄에 앱 경로가 들어가기 때문이다. 서명 중에 죽이면 dylib 이
손상되고(이번에 16개), 앱은 조용히 안 뜬다. 프로세스를 정리할 때는
`pgrep -lf` 로 무엇이 잡히는지 먼저 보고 죽여야 한다.

## 빌드·서명 도구 (맥) — 오늘 물린 것을 도구로 막아 뒀다

세 가지가 하루를 잡아먹었다. 셋 다 "조용히 잘못되는" 종류라, 사람이 조심하는 것으로는
막을 수 없다고 보고 도구 쪽에 넣었다.

1. **빌드를 겹쳐 돌리면 산출물이 사라진다**
   make/ninja 는 링크 전에 대상을 지운다. 두 빌드가 같은 `build/` 를 쓰면 뒤에 시작한
   쪽이 앞선 쪽의 실행 파일을 없앤다. 빌드는 "성공" 이라 하고 `Contents/MacOS` 에
   실행 파일만 없어서, 앱이 아무 말 없이 안 뜬다.
   → `build.sh` 가 `build/.build.lock` 으로 잠근다. 두 번째는 거부된다.
      (죽은 프로세스가 남긴 잠금은 자동으로 정리한다.)

2. **`pkill -f "MacOS/Predormition"` 은 codesign 도 죽인다**
   codesign 의 명령줄에 앱 실행 파일 경로가 들어가기 때문이다. 서명 도중에 죽이면
   dylib 이 손상되고(이번에 16개 → 54개) 앱이 안 뜬다.
   → `kill_app.sh` 를 쓴다. argv[0] 이 그 실행 파일인 프로세스만 고르므로
      codesign(`/usr/bin/codesign`)은 절대 걸리지 않는다. 시험으로 확인했다.

3. **서명 스크립트가 실패를 전부 삼켰다**
   모든 `codesign` 호출이 `2>/dev/null || true` 였다. 파일 54개가 서명 안 된 채
   남았는데 로그에는 한 줄도 없었고, 마지막 `--deep` 검증만 "깨졌다" 고 했다.
   → 실패를 파일에 적고 개수와 사유를 보고한다. 정상일 때도 "개별 서명 실패 없음"
      이라고 분명히 말한다.
      (집계를 셸 변수로 하면 안 된다 — 서명 루프가 `find | while` 형태라 서브셸에서
       돌고 변수가 밖으로 나오지 않아 늘 0 이 된다. 실제로 그렇게 한 번 틀렸다.)

### 윈도우 쪽도 확인해서 고쳤다 (남은 확인 항목 아님)

- **signtool**: 윈도우 빌드는 코드 서명을 하지 않는다 → ③은 해당 없음.
- **`taskkill /IM` 무차별 종료**: 해당됐다. 아래 참고.
- **동시 빌드**: 로컬 빌드 스크립트가 없고 CI(`build.yml`)로 돈다 →
  로컬에서 `cmake --build` 를 직접 두 번 돌리면 맥과 똑같이 깨진다.
  윈도우용 빌드 스크립트를 만들 때 `build.sh` 처럼 잠금을 넣을 것.

## 프로세스를 이름으로 죽이면 안 된다 (양쪽 다 고침)

**`taskkill /FI "COMMANDLINE eq ..."` 는 동작하지 않는다.** taskkill 에 COMMANDLINE
필터는 없다(STATUS/IMAGENAME/PID/SESSION/CPUTIME/MEMUSAGE/USERNAME/MODULES/
SERVICES/WINDOWTITLE 뿐). 지정하면 "잘못된 필터" 로 **아무것도 죽지 않는다** —
capture chrome 정리와 옛 터미널 창 정리가 한 번도 동작한 적이 없었다는 뜻이다.

동시에, 필터 없는 쪽은 **너무 많이** 죽였다:

    taskkill /F /IM "Chrome for Testing.exe"        ← 사용자가 쓰던 것도
    taskkill /F /IM chrome_crashpad_handler.exe     ← 사용자 본인 Chrome 의 것도
    taskkill /F /IM yt-dlp.exe / ffmpeg.exe         ← 사용자가 따로 돌리던 작업도

맥도 같았다: `pkill -9 -f "Chrome for Testing"`, `pkill -9 -f chrome_crashpad_handler`,
`pkill -f yt-dlp`. 주석에 "별도 설치 안 했으면 안전" 이라고 적혀 있었는데, 그건
설치했으면 남의 창을 닫는다는 뜻이다.

→ `killWindowsByCommandLine(needle)` 을 두었다. PowerShell 의 `Get-CimInstance
  Win32_Process` 로 **명령줄에 그 문자열이 든 PID 만** 골라 그 PID 만 죽인다
  (wmic 은 최신 윈도우에서 빠졌으므로 안 쓴다). 자기 자신은 절대 안 죽인다.
  맥은 `pkill -f` 의 패턴을 좁혔다 — 우리 것은 명령줄에 `chrome_capture_profile`
  이나 `abiwa_` 가 들어 있으므로 그것으로만 고른다.

---

# 윈도우에서 확인해 주세요 (맥 → 윈도우, `cd82016` 기준)

맥에서 작업하면서 **윈도우 트리도 함께 고쳤습니다.** 맥에서는 실제로 돌려 확인했지만
윈도우에서는 컴파일조차 못 해 봤습니다. 아래는 그쪽에서 실제로 봐 주셔야 하는 것들만
추린 것입니다. **왜 그렇게 고쳤는지**까지 적었으니, 판단이 다르면 되돌리셔도 됩니다.

순서는 "안 되면 바로 티나는 것" → "조용히 잘못되는 것" 입니다.

## A. 먼저 빌드가 되는지

맥에서 고친 것이 윈도우 컴파일을 깨뜨릴 수 있는 지점입니다.

| 파일 | 무엇 |
|---|---|
| `MiyoBackend.h/.cpp` | `openUrl` · `llmDir()` · `llmInstallDir()` 추가 |
| `Config.h/.cpp` | `aiMode`/`aiBaseUrl`/`aiApiKey`/`aiModel` (그쪽에서 온 것) + `sftpKeyFile` |
| `WebDavUploader.h/.cpp` | `setSftpKeyFile` (curl `--key`) |
| `ContentSecurityScanner.cpp` | `FileHelper::moveFileSafe` 사용 → `FileHelper.h` include 필요 |
| `index.html` | API 교체 패널 · 보관함 패널 · 니코동/유튜브 캡쳐 토글 |

`QDesktopServices` include 를 `MiyoBackend.cpp` 에 넣었습니다. 이미 있으면 중복은 아닙니다
(가드를 걸었습니다).

## B. 실제로 눌러 봐 주세요 (기능)

### B-1. 툼블러 안내의 링크

설정이 아니라 **툼블러 탭 → API 등록 안내**에 있는 링크 두 개입니다.

- 예전: `backend.openUrl(...)` 을 부르는데 슬롯이 없어 **눌러도 아무 일이 없었습니다.**
  오류도 안 납니다.
- 지금: 슬롯을 넣었습니다. `http`/`https` 만 엽니다.
- **확인**: 링크를 눌러 기본 브라우저가 뜨는지.

### B-2. API 주소 교체 패널 (설정 탭, 새로 만든 것)

이게 이번 작업에서 제일 큰 건입니다.

- `setApiOverride` / `getApiOverrides` / `openApiOverridesFile` 슬롯은 **원래 있었는데
  화면이 없었습니다.** `TwitterCollector::apiUrl()` 이 실제로 이 값을 읽습니다.
  즉 트위터가 GraphQL 주소를 바꾸면(예고 없이 바꿉니다) **앱을 다시 빌드하는 것
  말고는 방법이 없었습니다.**
- **확인**:
  1. 설정 탭 → "API 주소 교체" 패널이 보이는지 (`nt-group` 구조로 넣었습니다)
  2. 키 `twitter.userTweets`, 값 아무거나 넣고 저장 →
     `%APPDATA%\Miyo\Predormition\api_overrides.json` 에 기록되는지
  3. 아래 목록에 `twitter.userTweets → <값>` 으로 보이는지
     ★ 맥에서는 여기가 `19 → T` 처럼 깨졌습니다. `apiOverridesJson()` 이 객체가 아니라
       **JSON 문자열**을 주는데 UI 가 객체로 다뤄서였습니다. 풀어 쓰게 고쳤는데,
       윈도우에서도 같은 화면이 나오는지 봐 주세요.
  4. 값을 비우고 저장 → 파일이 `{}` 로 돌아가는지

### B-3. 니코동 "진짜 페이지 캡쳐" 토글

- 유튜브·니코동은 같은 `runYoutubeDownload` 를 씁니다. 캡쳐 코드는 거기 넣었는데
  **니코동 화면에만 토글이 없어서 설정이 안 넘어가 영영 안 돌았습니다.**
- **확인**: 니코동 탭에 토글이 보이고, 켜고 받으면
  `<플랫폼>\captures\<영상ID>.html` 이 생기는지.

### B-4. 보관함 패널 (설정 탭)

- **DB 경로가 맥·윈도우에서 같은 곳을 가리켜야 합니다.**
  파이썬 `archive_ask.default_db()` 와 C++ `archiveDbPath()` 둘 다
  `%APPDATA%\Miyo\Predormition\archive_index.db` 를 봐야 합니다.
  어긋나면 색인은 만들어지는데 질의만 "없습니다" 가 되어 원인을 찾기 어렵습니다.
- `resources/tools/archive/*.py` 4개가 배포에 실리는지도 봐 주세요.
  안 실리면 패널이 "스크립트 없음" 을 띄웁니다.

## C. 조용히 잘못되는 것 (여기가 중요합니다)

### C-1. AI 설치 위치를 바꿨습니다 — `win_install_ai.ps1`

**맥에서 실제로 사고가 났습니다.** 모델 5.7GB 가 앱 번들 안에만 있어서, 번들을 새로
만드는 과정에서 통째로 사라졌습니다. 그리고 앱은 'AI 켜기' 를 누를 때까지 아무 말도
하지 않았습니다.

- 앱: `llmDir()` 이 `%APPDATA%\Miyo\<앱>\llm` 을 **먼저** 보고, 없으면 앱 폴더를 봅니다.
- 설치기: 같은 자리에 넣습니다. **예전에 앱 폴더(`<exe>\llm`)에 받아 둔 것이 있으면
  자동으로 옮겨 옵니다** — 이미 받은 사람이 9.3GB 를 다시 받지 않게.
- **확인**: 앱 폴더에 `llm\` 이 있는 상태에서 설치기를 돌려, `Move-Item` 이 성공하고
  다시 받지 않는지. (권한 문제로 실패하면 메시지가 나오게 해 뒀습니다)
- 안내 문구도 바꿨습니다. 예전엔 "번들 llama-server 가 없습니다 (배포 패키징 시
  bundle_llm 로 탑재)" — 사용자가 할 수 있는 게 없는 말이었습니다.

### C-2. 프로세스를 이름으로 죽이던 곳

`killByCommandLine()` 은 그쪽 것을 그대로 씁니다. 다만 **호출부를 더 좁혔습니다.**

    지웠음:  taskkill /F /IM "Chrome for Testing.exe"
             taskkill /F /IM chrome_crashpad_handler.exe
    좁혔음:  taskkill /F /IM yt-dlp.exe   →  killByCommandLine("yt-dlp.exe", "abiwa_")
             taskkill /F /IM ffmpeg.exe   →  killByCommandLine("ffmpeg.exe", "abiwa_")

이유: 필터가 없으면 **사용자가 따로 쓰던 것까지 죽습니다.** 특히
`chrome_crashpad_handler.exe` 는 사용자 본인 Chrome 의 것이고, `ffmpeg.exe` 는 편집
중이던 작업일 수 있습니다.

- **확인**: 사용자 Chrome 을 띄워 둔 채 앱을 시작해도 그 Chrome 이 살아 있는지.
  그리고 캡쳐 Chrome 정리는 여전히 되는지(둘 다 되어야 맞습니다).
- 맥에서는 `-c 8192` 로 늘린 것과 별개로, `abiwa_` 로 좁힌 뒤에도 우리 yt-dlp 가
  제대로 죽는지 확인했습니다. 윈도우는 임시 폴더 이름이 같은지 봐 주세요
  (`Common::resolveTempBase(...) + "/abiwa_" + platform`).

### C-3. 설정이 앱 이름 변경을 견디는지

`Config::load` 가 `.../Miyo/*` 형제 폴더를 훑어 `miyo_config.json` 이 있는 것 중
**가장 최근 것**을 가져옵니다. 이름을 목록에 박으면 다음 변경 때 또 깨지기 때문입니다.

함께: 시작 시 옛 폴더를 지우는 정리 코드가 **설정이 남아 있는 폴더는 건드리지 않게**
했습니다(순서에 기대지 않게).

- **확인**: `%APPDATA%\Miyo\Chernobyl\miyo_config.json` 을 만들어 두고
  `Predormition` 쪽을 지운 뒤 앱을 켜면 되살아나는지. 맥에서는 확인했습니다.

### C-4. 파일 이동이 실패했는데 성공이라 하던 곳

`ContentSecurityScanner::quarantineFile` 이 이랬습니다:

    if (!QFile::rename(...)) { QFile::copy(...); QFile::remove(filePath); }
    ... return true;

복사 결과를 보지 않고 원본을 지웠고, 무슨 일이 있어도 성공이라 답했습니다. 다른
디스크로 옮기다 용량이 모자라거나 권한이 없으면 **파일이 사라졌습니다.**
`FileHelper::moveFileSafe` 로 바꿨습니다(다 옮겨진 것을 확인한 뒤에만 원본 삭제).

`moveFileSafe` 자체에도 구멍이 둘 있어 막았습니다 — 경로를 해석 못 할 때 "같은
파일이니 할 일 없음" 으로 **성공을 반환**하던 것, 원본 삭제 전 대상 확인이 없던 것.

### C-5. 폰트 — 윈도우는 원래 맞았습니다 (참고만)

맥 HTML 이 `qrc:///fonts/…` 를 쓰고 있었는데, 페이지가 `file://` 로 열려 크로미움이
교차 출처로 막고 있었습니다. **한글 폰트가 한 번도 실린 적이 없었습니다**(맥은 시스템
폰트로 가려져 안 보였습니다).

윈도우 HTML 은 이미 `../fonts/…` 였습니다. 배포도 `dist\win\html` 옆에
`dist\win\fonts` 라 맞습니다. **맥만 갈라져 있던 것**이라 맥을 윈도우에 맞췄습니다.

## D. 이건 판단을 여쭙습니다 — 원격 백업

`startRemoteBackup` / `stopRemoteBackup` / `pickRemoteBackupSrc` 가 **윈도우 트리에만**
있습니다. 맥에는 없습니다(맥 백업은 NAS 마운트 경유 `enqueueBackup` 뿐).

그쪽 주석에 "전 플랫폼(Windows 포함)" 이라 적혀 있는 걸 보면 원래 양쪽에 넣을 생각이
었던 것 같은데, 맥에 안 왔습니다. 마운트 없이 WebDAV/FTP/SFTP/S3 로 직접 올리는 기능
이라 맥에서도 쓸모가 큽니다.

### → 맥으로 옮겼습니다 (이 절은 처리 완료)

**그쪽 트리는 건드리지 않았습니다.** 구현을 그대로 가져오고 rclone 위치만 맥 배치에
맞췄습니다(`Contents/Resources/tools/rclone`, 개발 트리 폴백 포함). UI 는 맥 구조
(`section`/`field`)로 옮겼고 JS 는 그대로입니다.

맥에서 확인한 것:

    종류 전환   webdav → 주소·자격 / sftp → 호스트·자격 / s3 → S3 칸만
    업로드      SFTP 로 시작, 터미널 로그에 진행 표시
    중지        rclone 즉시 종료 · 버튼 복구 · 임시 conf 삭제 확인
    실패        "✗ 원격 백업 실패" 로 끝나고 버튼 복구

**그쪽 트리에도 고친 것이 하나 있습니다 — `--contimeout`.**

rclone 인자에 접속 한도가 없었습니다. `--timeout` 은 '전송이 멎었을 때' 의 한도라
닿지 않는 주소에는 듣지 않습니다. 맥에서 실측했더니 없는 IP 로 **10분 넘게** 매달렸고,
`--contimeout` 을 주니 접속 시도 하나가 10초에 끝납니다.

    startRemoteBackup  → --contimeout 20s 추가
    runRcloneBackup    → 같은 이유로 추가 (여기도 없었습니다)

주소를 잘못 적었을 때 3분 20초에 실패로 끝납니다(재시도 5회 × 20초). 예전엔 10분을
기다려도 안 끝났습니다. 재시도 횟수는 실제 전송에 필요해서 줄이지 않았습니다.

## E. 이번에 쓴 점검 방법 (윈도우에서도 그대로 됩니다)

"조용히 죽어 있는 것" 은 눈으로 못 찾습니다. 양방향으로 대조하면 나옵니다.
앱을 CDP 로 띄우고(`--remote-debugging-port`) 아래를 실행하면 됩니다.

```js
// ① UI 가 부르는 backend.<슬롯> 중 실제로 없는 것
const called = new Set();
for (const s of document.scripts)
  for (const m of (s.textContent||'').matchAll(/backend\.([A-Za-z_]\w*)\s*\(/g)) called.add(m[1]);
for (const m of document.documentElement.innerHTML.matchAll(/backend\.([A-Za-z_]\w*)\s*\(/g)) called.add(m[1]);
[...called].filter(k => typeof backend[k] !== 'function');

// ② 백엔드가 runJs 로 부르는 JS 함수 중 실제로 없는 것
//    (이름 목록은 소스에서 runJs("...(" 를 뽑아 넣는다)
names.filter(n => typeof window[n] !== 'function');
```

맥에서 이걸로 4개를 찾았습니다(`openUrl`, `openExternalApp`, `onApiOverrides`,
`updateBrowser*`). 윈도우 트리는 화면 구조가 달라 **다른 것이 나올 수 있습니다.**

## F. 맥 쪽 점검 결과 (참고 — 문제 없던 것)

    번들 도구 8개 전부 실행 (yt-dlp 2026.07.04 · ffmpeg 6.0 · exiftool 13.59 ·
                              rclone 1.74.4 · python 3.14.3 · llama 10256 …)
    파이썬 패키지 14개 버전 일치 + import 성공
    자립형 — 외부(Homebrew) 의존 남은 파일 0개
    버전 단일화 — VERSION 3.9.7 = Info.plist = 최신 태그
    설정·모델이 앱 밖 · 설정 권한 0600

윈도우에서 대응되는 것: 배포에 도구가 다 실리는지, `python_env` 가 MAX_PATH 게이트를
통과하는지, `%APPDATA%` 쪽 설정 파일 권한(NTFS ACL 은 `QFile::setPermissions` 로
안 바뀝니다 — 그쪽은 사용자 프로필 보호에 기대는 상태입니다. DPAPI 가 남은 일입니다).
