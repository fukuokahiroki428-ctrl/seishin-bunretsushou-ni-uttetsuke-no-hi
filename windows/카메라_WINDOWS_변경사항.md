# カメラ (Chernobyl) — Windows 포트 변경사항

Qt6 / QWebEngine 기반 미디어 수집 앱의 Windows 포트. 자매 앱 **PEN(팬을 잘 쓰고 싶다)** 의
인터랙티브 CDP 미러 엔진을 한 앱으로 통합했다.

---

## 1. 앱 리브랜딩 · 네이티브 UI
- 앱 표시명을 **カメラ** 로 변경 (아이콘 교체, `abiwa.rc` 에 VERSIONINFO 추가 → 탐색기/작업표시줄 표기).
- **Windows 11 둥근 모서리** — HTML/Qt 마스크가 아니라 Win32 DWM(`DWMWA_WINDOW_CORNER_PREFERENCE`)로 네이티브 처리.
- 프레임리스 타이틀바 + 더블클릭 최대화(`WM_GETMINMAXINFO` 로 작업표시줄 존중), 드래그 이동/리사이즈.
- HTML 특유의 느낌 제거 — `user-select:none`, 클릭 포커스링 제거 등 앱-네이티브 CSS.
- 상단 바: 사이드바 토글 + 설정 바로가기 버튼.

## 2. PEN 통합 — 사이트 미러 (Capture 탭)
PEN 백엔드를 **합성(composition)** 으로 통합: `PenBackend` + `PenChromeCrawler` 를 빌드에 추가하고
QWebChannel 의 2번째 객체(`penBackend`)로 등록. 기존 `MiyoBackend` 와 공존.

- **방식 단일화** — 기존 5모드(딥/트위터/자동/인터랙티브/사이트) → **단일 통합 미러** 하나로 통일,
  각 모드의 장점만 흡수:
  - BFS 다중 페이지 순회 + 링크 재작성 → 오프라인 클릭 네비게이션 `mirror/index.html`
  - SingleFile 브라우저 인라인(이미지·CSS 통째 보존, 봇 차단 우회)
  - `fetchViaChrome` — 페이지 컨텍스트 `fetch`(쿠키/세션 포함)로 이미지·비디오 다운로드 (curl 차단 우회)
  - **매 페이지 Cloudflare 처리** — JS 챌린지 자동 통과 대기, 진짜 캡챠만 멈춰서 사용자가 직접 풂

## 3. 웹사이트 엔진 분석 미러 (GNUBoard 등)
`github.com/gnuboard/gnuboard5` 실제 소스를 분석해 추측이 아닌 **실제 구조** 기반으로 구현:

- **엔진 자동 감지** — GNUBoard / WordPress / XpressEngine·Rhymix / Drupal / Joomla / Shopify / MediaWiki.
  GNUBoard 는 generator 메타를 안 쓰므로, 모든 페이지 `<head>` 에 박히는 JS 전역
  `var g5_url` · `g5_bbs_url` · `g5_bo_table` 와 `/js/wrest.js` 지문으로 감지.
  → demo.sir.kr/gnuboard5 실측에서 `GNUBoard` 정확 감지 확인.
- **쿼리스트링 파일명 분리** — 게시판은 `board.php?bo_table=X&wr_id=N` 처럼 쿼리로 글이 갈린다.
  기존엔 경로만 써서 모든 글이 `board.php` 한 파일로 충돌 → 미러가 비었다.
  쿼리를 안전 파일명으로 직렬화(`board.php__bo_table-X__wr_id-N.html`), 길면 MD5 축약, 충돌 시 `-2` 분리.
- **`&amp;` 링크 재작성** — DOM 의 `a.href` 는 디코드된 `&` 인데 저장 HTML 속성은 `&amp;` → 양쪽 폼 모두 치환.
- **액션 URL 스킵** — 실제 `bbs/` 스크립트 전수 기준(login/register/write/delete/good/scrap/memo/poll/qa·ajax.*)을
  `/login.php` 식 정확 매칭으로 제외(게시판 이름 오탐 방지) + 검색/정렬 파라미터(`sfl/stx/sst/sod/sop/spt/sca`) 차단으로 크롤 폭주 방지.
- **오프라인 인덱스** — 엔진 배지 + **게시판(bo_table)별 그룹** + **실시간 검색 필터(JS)**.

## 4. 보조 프로세스 백그라운드화
카메라가 띄우는 보조 앱을 작업관리자에서 **카메라 하위로 중첩 + 콘솔창 제거**:
- `MiyoBackend::launchChildConsole` 의 `CREATE_NEW_CONSOLE`(수집기마다 보이는 콘솔창) 제거 → 인앱 로그로 대체.
- PEN/Discord 의 `cmd /c start` detached 콘솔(최상위로 뜸) 제거.
- Qt6 QProcess 는 기본 `CREATE_NO_WINDOW` → 파이썬 데몬·yt-dlp·Chrome 은 콘솔 없이 카메라 자식으로 중첩.

## 5. 캡쳐 Chrome 분리 (겹침/꺼짐 수정)
캡쳐용 Chrome 은 전용 프로필(`chrome_capture_profile`)이라 개인 Chrome 과는 분리돼 있으나,
같은 프로필의 **잔존 Chrome 이 살아있으면 새 인스턴스가 핸드오프되며 즉시 종료**됐다.
→ 새 캡쳐 Chrome 기동 직전, 포트 리스닝 여부와 무관하게 **같은 캡쳐 프로필을 쓰는 chrome.exe 를 모두 정리**
(커맨드라인 매칭, 개인 Chrome 은 프로필 경로가 달라 안 건드림) → 항상 깨끗한 독립 인스턴스 보장.

## 6. YouTube
- **댓글 수집** — `--write-comments` + `comment_sort=top;max_comments=300` (토글), info.json → `<base>.comments.txt`.
- **설명 임베드** — `--embed-metadata` 항상.
- **URL 리졸버** — URL / 스킴리스 / `@handle` / `UC...` / bare ID 모두 허용 (`@95EC54B24A70` 404 버그 수정).

---

## 빌드 (Windows, 로컬)
- Qt `D:\Qt\6.7.3\msvc2019_64`, MSVC VS2022 BuildTools, cmake+ninja.
- `_build\build_local.bat` (vcvarsall x64 → cmake configure → ninja). `/utf-8` 필수(한/일 문자열), rc 는 `#pragma code_page(65001)`.
- 산출물 `build_win\Chernobyl.exe`.

## 주요 파일
| 파일 | 역할 |
|---|---|
| `src/core/PenBackend.{cpp,h}` | 통합 미러 엔진(BFS·SingleFile·엔진감지·링크재작성·인덱스·fetchViaChrome·CF) |
| `src/platforms/PenChromeCrawler.{cpp,h}` | CDP Chrome 런처/제어(프로필 분리·확장 로드) |
| `src/core/MiyoBackend.cpp` | 본체 수집기(YouTube 등) + 콘솔 백그라운드화 |
| `resources/html/index.html` | 전체 UI (Capture 탭 · 엔진 배지 · 설정) |
