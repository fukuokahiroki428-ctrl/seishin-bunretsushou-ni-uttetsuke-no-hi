# 인스타그램 — 2026-09-24 실측 처방

## 무엇이 죽었나
| 주소 | 결과 |
|---|---|
| `GET /api/v1/users/web_profile_info/?username=` | **429 + 21KB HTML**("페이지를 찾을 수 없습니다"). 브라우저로 불러도 같다 — 끝점 자체가 닫혔다. |
| `GET /api/v1/feed/user/<id>/?count=12` | **302** (죽음) |
| `POST /api/v1/clips/user/` | **302** (죽음) |

## 무엇이 살아 있나
| 주소 | 결과 |
|---|---|
| `GET /api/v1/feed/timeline/` | 200 (세션 확인용으로 쓸 만하다) |
| `GET /api/v1/highlights/<id>/highlights_tray/` | 200 |
| `GET /api/v1/feed/reels_media/?reel_ids=<id>` | 200 |
| `POST /graphql/query` (아래 질의들) | 200 |

**브라우저가 아니어도 된다.** httpx(=우리 HttpClient 와 같은 처지)로 전부 200 을 받았다.
TLS 지문 문제가 아니라 '끝점이 닫힌' 문제였다.

## 부르는 법
1. `GET https://www.instagram.com/<username>/` (쿠키 붙여서) 한 번.
   - `"LSD",[],{"token":"…"}` → **lsd**
   - `"DTSGInitialData",[],{"token":"…"}` → **fb_dtsg**
2. `POST https://www.instagram.com/graphql/query`
   - 헤더: `X-IG-App-ID: 936619743392459`, `X-ASBD-ID: 129477`, `X-CSRFToken: <csrftoken 쿠키>`,
     `X-FB-LSD: <lsd>`, `X-FB-Friendly-Name: <질의 이름>`,
     `Content-Type: application/x-www-form-urlencoded`, `Origin`/`Referer`, sec-ch-ua 계열
   - 본문(폼): `av=<ds_user_id>`, `__d=www`, `__user=0`, `__a=1`, `__req=a`, `dpr=2`,
     `lsd=<lsd>`, `__comet_req=7`, `fb_dtsg=<dtsg>`, `fb_api_caller_class=RelayModern`,
     `fb_api_req_friendly_name=<이름>`, `server_timestamps=true`, `doc_id=<번호>`,
     `variables=<JSON>`

## 질의 셋 (2026-09-24 실측)
### 게시물 — `PolarisProfilePostsQuery` · doc_id `28379418928391013`
```json
{"data":{"count":12,"include_reel_media_seen_timestamp":true,"include_relationship_info":true,
         "latest_besties_reel_media":true,"latest_reel_media":true},
 "username":"<이름>","first":12,"before":null,"last":null,"after":<커서 또는 null>,
 "__relay_internal__pv__PolarisMultiCaptionCarouselEnabledrelayprovider":false,
 "__relay_internal__pv__PolarisShortDramaEnabledrelayprovider":false,
 "__relay_internal__pv__PolarisReelsRecoDebugOverlayEnabledrelayprovider":false}
```
- 응답: `data.xdt_api__v1__feed__user_timeline_graphql_connection.edges[].node`
  — **node 는 옛 REST item 과 같은 모양**(code·taken_at·image_versions2·carousel_media·user.id).
- 이어받기: `page_info.end_cursor` 를 **variables 최상위 `after`** 에 넣는다.
  `data` 안에 넣으면 같은 쪽이 다시 온다(실측: 2쪽이 1쪽과 12개 전부 겹침 → 최상위로 옮기니 0개).

### 프로필 정보 — `PolarisProfilePageContentQuery`
`{"enable_integrity_filters":true,"id":"<사용자번호>", + 제공자 깃발 5개}` → `data.user`

### 릴스 — `PolarisProfileReelsTabContentQuery` · doc_id `29628758406714645`
```json
{"data":{"include_feed_video":true,"page_size":12,"target_user_id":"<번호>"},
 "user_id":"<번호>","__relay_internal__pv__PolarisShortDramaEnabledrelayprovider":false}
```

## 주의
- `__relay_internal__pv__…` 깃발은 **질의마다 이름이 다르다.** 틀리면 200 에 `execution error` 만 온다.
- `doc_id` 와 깃발 이름은 인스타가 바꾼다. 틀어지면 위 방법대로 브라우저에서 다시 뜨면 된다
  (앱 번들 Chromium 을 `--remote-debugging-port` 로 띄우고 CDP `Network.requestWillBeSent` 의 postData 를 본다).
- 사용자 번호는 게시물 질의 응답의 `node.user.id` 에서 얻을 수 있다 — web_profile_info 가 없어도 된다.
