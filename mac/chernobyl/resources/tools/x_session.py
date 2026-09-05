#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
x_session.py — twikit 없이 X(트위터)에 붙는 얇은 세션 계층.

왜 만들었나
-----------
수집은 twikit 에 얹혀 있었다. 그런데 twikit 은 최신이 2.3.3 이고 2025-02 배포다.
1년 넘게 새 판이 없다. X 는 계속 바뀌는데 따라갈 사람이 없다는 뜻이다.
업데이트로 해결되는 문제가 아니라서, 대신할 것을 두는 수밖에 없다.

그런데 실제로 세어 보니 twitter_daemon.py 가 twikit 에서 쓰는 것은 일곱 개뿐이었다:
  Client / set_cookies / http / _base_headers / _user_agent / request /
  client_transaction (그중에서도 메서드 세 개)
쿼리 해시는 이미 앱이 X 의 JS 번들에서 직접 긁어 오고 있었고, TID 초기화도
데몬이 손으로 다 하고 있었다. twikit 은 사실상 httpx 껍데기 + 서명기였다.
게다가 데몬은 twikit 의 소스 파일을 실행 중에 고쳐 쓰고 있었다(‘code’ KeyError).
그 정도로 붙들고 있을 이유가 없다.

무엇으로 대신하나
-----------------
· HTTP·쿠키·헤더 — httpx 로 직접 한다(이미 의존성에 있다). 여기 있는 게 그것이다.
· X-Client-Transaction-Id 서명 — xclienttransaction(x_client_transaction).
  2026-06 배포로 살아 있고, 필요한 세 메서드를 그대로 갖고 있다.
  게다가 ondemand.s 청크 찾기까지 해 줘서 데몬이 손으로 하던 정규식이 필요 없다.
· 엔드포인트·기능 플래그 — 아래 상수. 이건 X 가 요구하는 값의 목록일 뿐이고,
  쿼리 해시는 어차피 앱이 런타임에 덮어쓴다.

정직한 한계
-----------
· 기능 플래그(FEATURES)는 X 가 가끔 늘린다. 빠지면 X 가 어떤 플래그가 없는지
  오류 메시지로 알려 주므로, 그때 여기에 한 줄 추가하면 된다.
· 이 파일은 twikit 을 '대신할 수 있게' 두는 것이지, twikit 을 지우는 것이 아니다.
  데몬은 이것을 먼저 쓰고, 안 되면 twikit 으로 물러선다.
"""

import json
from urllib.parse import urlparse

import httpx

# X 웹 앱이 쓰는 공개 bearer 토큰. 사용자 비밀이 아니라 웹 클라이언트 상수다.
BEARER = ("Bearer AAAAAAAAAAAAAAAAAAAAANRILgAAAAAAnNwIzUejRCOuH5E6I8xnZz4puTs%3D"
          "1Zv7ttfk8LF81IUq16cHjhLTvJu4FA33AGWWjCpTnA")

# 기본 User-Agent — 앱이 실제 브라우저 판을 넘겨 주면 그것을 쓴다.
#   twikit 기본값은 macOS 사파리였다. 윈도우 크롬 쿠키를 쓰면서 맥 사파리라고
#   말하는 셈이라 그 자체가 자동화 신호였다. 기본값부터 윈도우 크롬으로 둔다.
DEFAULT_UA = ("Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
              "(KHTML, like Gecko) Chrome/153.0.0.0 Safari/537.36")


class Endpoint:
    """앱이 쓰는 GraphQL 엔드포인트.

    ★ 이 값들은 손으로 적지 않았다. 지금 돌고 있는 twikit 에서 그대로 읽어 왔다.
      처음엔 기억으로 적었다가 13개 중 6개가 틀렸다 — 읽을 수 있는 값을 추측하면
      안 된다. 그런 종류의 오류는 빌드도 통과하고 수집만 조용히 깨진다.
    ★ 해시는 X 가 배포마다 바꾼다. 앱이 런타임에 JS 번들에서 긁어 덮어쓰므로
      여기 값은 그 갱신이 실패했을 때의 바닥값이다.
    """
    BASE = "https://x.com/i/api/graphql"
    USER_BY_SCREEN_NAME = BASE + "/NimuplG1OB7Fd2btCLdBOw/UserByScreenName"
    USER_TWEETS = BASE + "/QWF3SzpHmykQHsQMixG0cg/UserTweets"
    SEARCH_TIMELINE = BASE + "/flaR-PUMshxFWZWPNpq4zA/SearchTimeline"
    USER_LIKES = BASE + "/IohM3gxQHfvWePH5E3KuNA/Likes"
    BOOKMARKS = BASE + "/qToeLeMs43Q8cr7tRYXmaQ/Bookmarks"
    FOLLOWERS = BASE + "/gC_lyAxZOptAMLCJX5UhWw/Followers"
    FOLLOWING = BASE + "/2vUj-_Ek-UmBVDNtd8OnQA/Following"
    TWEET_DETAIL = BASE + "/U0HTv-bAWTBYylwEMT7x5A/TweetDetail"
    RETWEETERS = BASE + "/X-XEqG5qHQSAwmvy00xfyQ/Retweeters"
    FAVORITERS = BASE + "/LLkw5EcVutJL6y-2gkz22A/Favoriters"
    USER_HIGHLIGHTS_TWEETS = BASE + "/tHFm_XZc_NNi-CfUThwbNw/UserHighlightsTweets"
    LIST_LATEST_TWEETS_TIMELINE = BASE + "/HjsWc-nwwHKYwHenbHm-tw/ListLatestTweetsTimeline"
    COMMUNITY_TWEETS_TIMELINE = BASE + "/mhwSsmub4JZgHcs0dtsjrw/CommunityTweetsTimeline"


# X 가 요구하는 기능 플래그 — 이것도 twikit 에서 그대로 읽어 왔다.
#   빠지면 X 가 400 과 함께 어떤 플래그가 없는지 알려 준다. 그때 한 줄 추가하면 된다.
FEATURES = {
    "c9s_tweet_anatomy_moderator_badge_enabled": True,
    "creator_subscriptions_tweet_preview_api_enabled": True,
    "freedom_of_speech_not_reach_fetch_enabled": True,
    "graphql_is_translatable_rweb_tweet_is_translatable_enabled": True,
    "longform_notetweets_consumption_enabled": True,
    "longform_notetweets_inline_media_enabled": True,
    "longform_notetweets_rich_text_read_enabled": True,
    "responsive_web_edit_tweet_api_enabled": True,
    "responsive_web_enhance_cards_enabled": False,
    "responsive_web_graphql_exclude_directive_enabled": True,
    "responsive_web_graphql_skip_user_profile_image_extensions_enabled": False,
    "responsive_web_graphql_timeline_navigation_enabled": True,
    "responsive_web_media_download_video_enabled": False,
    "responsive_web_twitter_article_tweet_consumption_enabled": True,
    "rweb_video_timestamps_enabled": True,
    "standardized_nudges_misinfo": True,
    "tweet_awards_web_tipping_enabled": False,
    "tweet_with_visibility_results_prefer_gql_limited_actions_policy_enabled": True,
    "tweetypie_unmention_optimization_enabled": True,
    "verified_phone_label_enabled": False,
    "view_counts_everywhere_api_enabled": True,
}

USER_FEATURES = {
    "creator_subscriptions_tweet_preview_api_enabled": True,
    "hidden_profile_likes_enabled": True,
    "hidden_profile_subscriptions_enabled": True,
    "highlights_tweets_tab_ui_enabled": True,
    "responsive_web_graphql_exclude_directive_enabled": True,
    "responsive_web_graphql_skip_user_profile_image_extensions_enabled": False,
    "responsive_web_graphql_timeline_navigation_enabled": True,
    "responsive_web_twitter_article_notes_tab_enabled": False,
    "subscriptions_verification_info_is_identity_verified_enabled": True,
    "subscriptions_verification_info_verified_since_enabled": True,
    "verified_phone_label_enabled": False,
}


def flatten_params(params: dict) -> dict:
    """중첩된 값을 JSON 문자열로 눕힌다 — GraphQL 쿼리스트링이 그 꼴을 요구한다."""
    out = {}
    for key, value in params.items():
        if isinstance(value, (list, dict)):
            value = json.dumps(value, separators=(",", ":"))
        out[key] = value
    return out


class _Transaction:
    """
    X-Client-Transaction-Id 서명기.

    실제 계산은 xclienttransaction 이 한다. 여기서는 '초기화가 실패해도 앱이
    멈추지 않게' 감싸는 일만 한다 — 서명이 없으면 X 가 막을 수는 있어도,
    앱이 예외로 죽어 버리는 것보다는 낫다.
    """

    def __init__(self):
        self._inner = None
        self.ready = False
        self.reason = "아직 초기화하지 않았습니다"

    async def init(self, http: httpx.AsyncClient, headers: dict) -> bool:
        try:
            from x_client_transaction import ClientTransaction
            from x_client_transaction.utils import get_ondemand_file_url
        except ImportError as e:
            self.reason = f"xclienttransaction 이 없습니다 ({e})"
            return False
        try:
            import bs4
            from x_client_transaction.utils import get_migration_url, get_migration_form

            # ★ 그냥 https://x.com 을 받으면 안 된다. 실측: 앱 껍데기가 아니라
            #   이관(migration) 안내 페이지가 와서 ondemand.s 를 못 찾았다
            #   (AttributeError: 'NoneType' object has no attribute 'group').
            #   라이브러리의 동기용 handle_x_migration 과 같은 순서를 비동기로 따라간다:
            #     /home 을 받고 → meta refresh 가 있으면 따라가고 → 이관 폼이 있으면 제출한다.
            resp = await http.get("https://x.com/home", headers=headers, follow_redirects=True)
            soup = bs4.BeautifulSoup(resp.content, "html.parser")

            m = get_migration_url(response=soup)
            if m:
                resp = await http.get(m.group(0), headers=headers, follow_redirects=True)
                soup = bs4.BeautifulSoup(resp.content, "html.parser")

            form = get_migration_form(response=soup)
            if form:
                resp = await http.request(form["method"], form["url"],
                                          data=form["data"], headers=headers,
                                          follow_redirects=True)
                soup = bs4.BeautifulSoup(resp.content, "html.parser")

            od_url = get_ondemand_file_url(response=soup)
            od = await http.get(od_url, headers=headers)
            if od.status_code != 200:
                raise RuntimeError(f"ondemand.s 를 못 받았습니다 (HTTP {od.status_code})")
            self._inner = ClientTransaction(home_page_response=soup,
                                            ondemand_file_response=od.text)
            self.ready = True
            self.reason = f"준비됨 ({od_url.rsplit('/', 1)[-1]})"
            return True
        except Exception as e:
            self.reason = f"초기화 실패: {type(e).__name__}: {e}"
            return False

    def generate_transaction_id(self, method: str = "GET", path: str = "", **kw) -> str:
        if not self._inner:
            # 서명 없이 보낸다. 막히면 막히는 대로 오류가 보인다 — 조용히 죽지는 않는다.
            return ""
        try:
            return self._inner.generate_transaction_id(method=method, path=path)
        except Exception:
            return ""


class Client:
    """
    twitter_daemon.py 가 twikit.Client 에게 기대하는 것만 제공한다.

    일부러 좁게 만들었다. 넓게 만들면 '두 번째 twikit' 이 될 뿐이고,
    여기서 필요한 것은 일곱 개뿐이다.
    """

    def __init__(self, language: str = "en-US", user_agent: str | None = None,
                 proxy: str | None = None, timeout: float = 30.0):
        self._user_agent = user_agent or DEFAULT_UA
        self._language = language
        self._cookies = {}
        self.client_transaction = _Transaction()

        transport = httpx.AsyncHTTPTransport(retries=3, proxy=proxy) if proxy \
            else httpx.AsyncHTTPTransport(retries=3)
        self.http = httpx.AsyncClient(
            transport=transport,
            timeout=httpx.Timeout(timeout, connect=15.0, read=timeout, write=15.0, pool=10.0),
            follow_redirects=True,
        )

    # ── twikit 이 갖고 있던 이름들 ──────────────────────────────────
    @property
    def _base_headers(self) -> dict:
        h = {
            "authorization": BEARER,
            "content-type": "application/json",
            "X-Twitter-Auth-Type": "OAuth2Session",
            "X-Twitter-Active-User": "yes",
            "Referer": "https://x.com/",
            "User-Agent": self._user_agent,
            "Accept-Language": self._language,
            "X-Twitter-Client-Language": self._language,
        }
        if self._cookies.get("ct0"):
            h["x-csrf-token"] = self._cookies["ct0"]
        return h

    def set_cookies(self, cookies: dict, clear_cookies: bool = False):
        if clear_cookies:
            self._cookies = {}
            self.http.cookies.clear()
        self._cookies.update(cookies)
        for k, v in cookies.items():
            # 도메인을 명시한다. 안 하면 httpx 가 중복 쿠키를 만들어 CookieConflict 가 난다
            # — 데몬이 httpx.Cookies.get 을 원숭이 패치하던 바로 그 문제다.
            self.http.cookies.set(k, v, domain=".x.com")

    async def get(self, url: str, **kw):
        return await self.request("GET", url, **kw)

    async def post(self, url: str, **kw):
        return await self.request("POST", url, **kw)

    async def request(self, method: str, url: str, *, params=None, headers=None,
                      json=None, data=None, raise_exception: bool = True, **kw):
        """(파싱된 본문, 응답) 을 돌려준다 — twikit 의 반환 모양 그대로."""
        hdr = dict(headers) if headers else dict(self._base_headers)
        hdr.setdefault("x-csrf-token", self._cookies.get("ct0", ""))
        try:
            tid = self.client_transaction.generate_transaction_id(
                method=method, path=urlparse(url).path)
            if tid:
                hdr["x-client-transaction-id"] = tid
        except Exception:
            pass

        resp = await self.http.request(method, url, params=params, headers=hdr,
                                       json=json, data=data, **kw)
        try:
            body = resp.json()
        except Exception:
            body = resp.text

        if raise_exception and resp.status_code >= 400:
            msg = body
            if isinstance(body, dict) and body.get("errors"):
                first = body["errors"][0]
                msg = first.get("message") or first
            raise RuntimeError(f"X HTTP {resp.status_code}: {str(msg)[:300]}")
        return body, resp

    async def aclose(self):
        try:
            await self.http.aclose()
        except Exception:
            pass
