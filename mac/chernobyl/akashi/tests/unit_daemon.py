#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""데몬 단위 시험 — resources/tools/twitter_daemon.py 를 가짜 세션(네트워크 없음)에 물려 돌린다(앱 · 네트워크 없음, 1초).

    python3 -B akashi/tests/unit_daemon.py

  M  GraphQL 보내는 방법 — SearchTimeline 은 처음부터 POST(몸 = variables · features · queryId), 나머지는 GET.
     빈 본문 404 면 다른 방법으로 한 번 더 · 200 이면 기억(graphql_methods.json) · 다음엔 기억한 방법부터 · 둘 다 404 면 404 그대로
  R  TimelineReplaceEntry 의 cursor-bottom 을 TimelineAddEntries 로 옮긴다(AddEntries 가 없으면 만든다 · 나머지는 그대로)
  S  정지된 계정의 검색 문(DenySuspendedUsers) → _cause account_suspended · _gate / account_status 는 상태 칸만(이름 · 메일 없음)
  L  x-rate-limit-* 머리 → _rl
  H  update_endpoints — 12시간 안의 캐시면 긁지 않음(답 = status ok · cached True) · force 면 긁음 · 오래됐거나 없으면 긁음
  T  트윗 상세의 fieldToggles 에 withArticlePlainText = True · 검색 variables 에 웹 앱 값
자료 폴더는 시험이 만든 임시 폴더(HANISHIKI_DATA_DIR)다 — 사용자의 Application Support 는 건드리지 않는다.
"""
from __future__ import annotations

import asyncio
import importlib.util
import json
import os
import shutil
import sys
import tempfile
import time
from pathlib import Path

sys.dont_write_bytecode = True   # resources/tools 에 __pycache__ 를 남기지 않는다(번들 서명 · 커밋 대상 밖)
HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import _common as C  # noqa: E402

TOOLS = HERE.parent.parent / "resources" / "tools"


def load_daemon(data_dir: Path):
    os.environ["HANISHIKI_DATA_DIR"] = str(data_dir)   # 모듈이 불러올 때 이 폴더를 만든다 — 임시 폴더로
    spec = importlib.util.spec_from_file_location("twitter_daemon_ut", TOOLS / "twitter_daemon.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


class Resp:
    def __init__(self, status, body=None, headers=None):
        self.status_code = status
        self._body = body
        self.text = "" if body is None else (body if isinstance(body, str) else json.dumps(body))
        self.headers = headers or {}


class Client:
    """twikit · x_session 이 데몬에 주는 것 가운데 쓰는 것만 — request() 는 (data, response)"""

    def __init__(self, handler):
        self.handler = handler
        self.calls = []

    @property
    def _base_headers(self):
        return {"authorization": "Bearer x", "content-type": "application/json", "x-csrf-token": "akashi-ct0"}

    async def request(self, method, url, params=None, json=None, headers=None, raise_exception=True, **kw):
        self.calls.append({"method": method, "url": url, "params": params, "json": json, "headers": dict(headers or {})})
        r = self.handler(method, url, params, json)
        data = r._body if not isinstance(r._body, str) and r._body is not None else r.text
        return data, r


class Endpoint:
    BASE = "https://x.com/i/api/graphql"
    USER_BY_SCREEN_NAME = BASE + "/QIDuser/UserByScreenName"
    USER_TWEETS = BASE + "/QIDut/UserTweets"
    SEARCH_TIMELINE = BASE + "/QIDsearch/SearchTimeline"
    USER_LIKES = BASE + "/QIDlikes/Likes"
    BOOKMARKS = BASE + "/QIDbk/Bookmarks"
    FOLLOWERS = BASE + "/QIDfol/Followers"
    FOLLOWING = BASE + "/QIDfing/Following"
    TWEET_DETAIL = BASE + "/QIDtd/TweetDetail"
    RETWEETERS = BASE + "/QIDrt/Retweeters"
    FAVORITERS = BASE + "/QIDfav/Favoriters"
    USER_HIGHLIGHTS_TWEETS = BASE + "/QIDhl/UserHighlightsTweets"
    LIST_LATEST_TWEETS_TIMELINE = BASE + "/QIDlist/ListLatestTweetsTimeline"
    COMMUNITY_TWEETS_TIMELINE = BASE + "/QIDcom/CommunityTweetsTimeline"


FEATURES = {"f1": True}
USER_FEATURES = {"u1": True}


def flatten(params):
    return {k: (json.dumps(v, separators=(",", ":")) if isinstance(v, (dict, list)) else v) for k, v in params.items()}


def search_body(instructions, errors=None):
    b = {"data": {"search_by_raw_query": {"search_timeline": {"timeline": {"instructions": instructions}}}}}
    if errors:
        b["errors"] = errors
    return b


def tweet_entry(i):
    return {"entryId": "tweet-%d" % i, "content": {"entryType": "TimelineTimelineItem",
                                                  "itemContent": {"tweet_results": {"result": {"rest_id": str(i)}}}}}


def cursor(kind, v):
    return {"entryId": "cursor-%s-%s" % (kind, v),
            "content": {"entryType": "TimelineTimelineCursor", "value": v, "cursorType": kind.capitalize()}}


def main() -> int:
    check = C.Checks()
    data_dir = Path(tempfile.mkdtemp(prefix="unit_daemon_"))
    d = load_daemon(data_dir)
    run = asyncio.run

    def cmd(client, args):
        return run(d.handle_command(client, args, Endpoint, FEATURES, USER_FEATURES, flatten))

    methods_file = data_dir / "graphql_methods.json"

    # ── M: 보내는 방법 ──
    seen = []

    def h_search_post_ok(method, url, params, body):
        seen.append(method)
        return Resp(200, search_body([{"type": "TimelineAddEntries", "entries": [tweet_entry(1)]}]))

    c = Client(h_search_post_ok)
    r = cmd(c, {"action": "search", "query": "from:tgt", "count": 20})
    call = c.calls[0]
    check("M1 SearchTimeline 은 처음부터 POST", call["method"] == "POST" and r.get("status") == 200, call["method"])
    j = call["json"] or {}
    check("M2 POST 몸 = variables · features · queryId(주소의 앞 마디) · 쿼리스트링 없음",
          set(j) == {"variables", "features", "queryId"} and j["queryId"] == "QIDsearch" and j["features"] == FEATURES
          and call["params"] is None, j)
    check("M3 POST 머리 content-type: application/json",
          call["headers"].get("content-type") == "application/json", call["headers"])
    v = j.get("variables") or {}
    check("T1 검색 variables 에 웹 앱 값(withGrokTranslatedBio · withQuickPromoteEligibilityTweetFields = False)",
          v.get("withGrokTranslatedBio") is False and v.get("withQuickPromoteEligibilityTweetFields") is False
          and v.get("rawQuery") == "from:tgt" and v.get("product") == "Latest", v)
    check("M4 기본이 맞으면 기억 파일을 쓰지 않는다", not methods_file.exists())

    # UserTweets: GET 이 빈 404 → POST 200 → 기억 → 다음엔 POST 부터
    def h_ut(method, url, params, body):
        if method == "GET":
            return Resp(404, "")
        return Resp(200, {"data": {"user": {"result": {"timeline": {"timeline": {"instructions": []}}}}}})

    c = Client(h_ut)
    r = cmd(c, {"action": "user_tweets", "user_id": "1"})
    check("M5 GET 빈 404 → POST 로 한 번 더 → 200", [x["method"] for x in c.calls] == ["GET", "POST"] and r.get("status") == 200,
          [x["method"] for x in c.calls])
    mem = json.loads(methods_file.read_text()) if methods_file.exists() else {}
    check("M6 기억 파일 graphql_methods.json 에 UserTweets = POST(시각 포함)",
          mem.get("UserTweets", {}).get("method") == "POST" and mem["UserTweets"].get("at", 0) > time.time() - 60, mem)
    c = Client(h_ut)
    cmd(c, {"action": "user_tweets", "user_id": "1", "cursor": "abc"})
    check("M7 다음엔 기억한 POST 부터(한 번만 묻는다)", [x["method"] for x in c.calls] == ["POST"], [x["method"] for x in c.calls])
    check("M8 POST 몸의 variables 에 커서", (c.calls[0]["json"] or {}).get("variables", {}).get("cursor") == "abc")

    # 기억이 새 모듈(데몬 재기동)에도 남는다
    d2 = load_daemon(data_dir)
    check("M9 데몬을 다시 띄워도 기억이 남는다", d2._gql_first_method("UserTweets") == "POST")

    # 7일 넘은 기억 — 기억한 방법으로 먼저 보내고, 맞으면 날짜를 새로 적는다
    mem["UserTweets"]["at"] = time.time() - 8 * 86400
    methods_file.write_text(json.dumps(mem))
    d._gql_methods = None
    c = Client(h_ut)
    cmd(c, {"action": "user_tweets", "user_id": "1"})
    mem2 = json.loads(methods_file.read_text())
    check("M10 7일 넘은 기억 — 기억한 방법부터 · 맞으면 날짜를 새로",
          [x["method"] for x in c.calls] == ["POST"] and mem2["UserTweets"]["at"] > time.time() - 60, mem2)

    # SearchTimeline: POST 빈 404 → GET 200 → GET 기억
    def h_search_get(method, url, params, body):
        if method == "POST":
            return Resp(404, "")
        return Resp(200, search_body([]))

    c = Client(h_search_get)
    r = cmd(c, {"action": "search", "query": "from:tgt"})
    mem3 = json.loads(methods_file.read_text())
    check("M11 SearchTimeline POST 빈 404 → GET 200 → GET 기억",
          [x["method"] for x in c.calls] == ["POST", "GET"] and mem3.get("SearchTimeline", {}).get("method") == "GET", mem3)
    gp = c.calls[1]["params"] or {}
    check("M12 GET 은 variables · features 를 쿼리스트링으로(json 몸 없음)",
          "variables" in gp and "features" in gp and c.calls[1]["json"] is None, gp)

    # 둘 다 404 — 404 그대로(해시 수리 길), 기억은 그대로
    def h_404(method, url, params, body):
        return Resp(404, "")

    c = Client(h_404)
    r = cmd(c, {"action": "likes", "user_id": "1"})
    mem4 = json.loads(methods_file.read_text())
    check("M13 두 방법 다 빈 404 → 404 를 돌려준다 · Likes 는 기억하지 않는다",
          r.get("status") == 404 and len(c.calls) == 2 and "Likes" not in mem4, (r, mem4))

    # 본문이 있는 404 는 '방법' 문제가 아니다 — 다시 묻지 않는다
    def h_404_body(method, url, params, body):
        return Resp(404, {"errors": [{"message": "Not found"}]})

    c = Client(h_404_body)
    r = cmd(c, {"action": "highlights", "user_id": "1"})
    check("M14 본문 있는 404 는 한 번만 묻는다", len(c.calls) == 1 and r.get("status") == 404, len(c.calls))

    # ── R: ReplaceEntry 커서 ──
    def h_replace(method, url, params, body):
        return Resp(200, search_body([
            {"type": "TimelineAddEntries", "entries": [tweet_entry(5), tweet_entry(4)]},
            {"type": "TimelineReplaceEntry", "entry_id_to_replace": "cursor-top-1", "entry": cursor("top", "T2")},
            {"type": "TimelineReplaceEntry", "entry_id_to_replace": "cursor-bottom-1", "entry": cursor("bottom", "B2")},
        ]))

    c = Client(h_replace)
    r = cmd(c, {"action": "search", "query": "from:tgt", "cursor": "B1"})
    ins = r["body"]["data"]["search_by_raw_query"]["search_timeline"]["timeline"]["instructions"]
    add = [i for i in ins if i["type"] == "TimelineAddEntries"][0]["entries"]
    check("R1 ReplaceEntry 의 cursor-bottom 이 AddEntries 끝에 붙었다",
          add[-1]["entryId"].startswith("cursor-bottom") and add[-1]["content"]["value"] == "B2", [e["entryId"] for e in add])
    check("R2 cursor-top 은 옮기지 않고 · 원래 지시들은 그대로", not any(e["entryId"].startswith("cursor-top") for e in add)
          and len(ins) == 3 and len(add) == 3, [i["type"] for i in ins])

    def h_replace_only(method, url, params, body):
        return Resp(200, {"data": {"user": {"result": {"timeline": {"timeline": {"instructions": [
            {"type": "TimelineReplaceEntry", "entry_id_to_replace": "cursor-bottom-9",
             "entry": {"entryId": "sq-cursor", "content": {"value": "B9", "cursorType": "Bottom"}}}]}}}}}})

    c = Client(h_replace_only)
    r = cmd(c, {"action": "likes", "user_id": "1"})
    ins = r["body"]["data"]["user"]["result"]["timeline"]["timeline"]["instructions"]
    adds = [i for i in ins if i["type"] == "TimelineAddEntries"]
    check("R3 AddEntries 가 없으면 만든다 · entryId 가 cursor-bottom 이 아니면 바꿀 자리의 이름을 단다",
          len(adds) == 1 and adds[0]["entries"][0]["entryId"] == "cursor-bottom-9"
          and adds[0]["entries"][0]["content"]["value"] == "B9", ins)

    # ── S: 정지된 계정 ──
    gate_err = [{"message": "InvalidArgument: ProductDisabled: Closed gate stopped execution of the pipeline "
                            "(LatestTabSrpDenySuspendedUsers)", "name": "InvalidArgument",
                 "path": ["search_by_raw_query", "search_timeline", "timeline"]}]

    def h_gate(method, url, params, body):
        return Resp(200, {"data": {"search_by_raw_query": {"search_timeline": {}}}, "errors": gate_err})

    c = Client(h_gate)
    r = cmd(c, {"action": "search", "query": "from:tgt"})
    check("S1 검색 문(DenySuspendedUsers) → status 200 · _cause account_suspended · _gate",
          r.get("status") == 200 and r.get("_cause") == "account_suspended" and r.get("_gate") == "LatestTabSrpDenySuspendedUsers"
          and isinstance(r.get("body"), dict), {k: r.get(k) for k in ("status", "_cause", "_gate")})
    check("S2 다른 errors 는 정지로 보지 않는다", d._suspended_gate({"errors": [{"message": "Timeout: Unspecified"}]}) is None
          and d._suspended_gate({"errors": [{"message": "ProductDisabled: x (TopTabSuspendedGate)"}]}) == "TopTabSuspendedGate")

    def h_verify(method, url, params, body):
        assert "verify_credentials" in url and method == "GET"
        return Resp(200, {"id_str": "777", "screen_name": "secret_name", "email": "a@b", "suspended": True,
                          "protected": False, "needs_phone_verification": True},
                    {"x-rate-limit-limit": "75", "x-rate-limit-remaining": "74", "x-rate-limit-reset": "1800000000"})

    c = Client(h_verify)
    r = cmd(c, {"action": "account_status"})
    check("S3 account_status — suspended · needs_phone_verification · user_id",
          r.get("status") == 200 and r.get("suspended") is True and r.get("needs_phone_verification") is True
          and r.get("user_id") == "777" and r.get("locked") is False, r)
    check("S4 account_status 는 이름 · 메일을 싣지 않는다", "secret_name" not in json.dumps(r) and "a@b" not in json.dumps(r)
          and (c.calls[0]["params"] or {}).get("skip_status") == "true"
          and (c.calls[0]["params"] or {}).get("include_email") == "false", r)

    def h_locked(method, url, params, body):
        return Resp(403, {"errors": [{"code": 326, "message": "locked"}]})

    r = cmd(Client(h_locked), {"action": "account_status"})
    check("S5 잠긴 계정(326) → locked · error_code · 번호만 든 body", r.get("locked") is True and r.get("error_code") == 326
          and r.get("body") == {"errors": [{"code": 326}]}, r)

    # ── L: 레이트 리밋 머리 ──
    def h_rl(method, url, params, body):
        return Resp(200, search_body([]), {"x-rate-limit-limit": "50", "x-rate-limit-remaining": "0",
                                           "x-rate-limit-reset": "1800000123"})

    r = cmd(Client(h_rl), {"action": "search", "query": "q"})
    check("L1 x-rate-limit-* → _rl", r.get("_rl") == {"limit": 50, "remaining": 0, "reset": 1800000123}, r.get("_rl"))
    r = cmd(Client(h_search_post_ok), {"action": "search", "query": "q"})
    check("L2 머리가 없으면 _rl 도 없다", "_rl" not in r)

    def h_429(method, url, params, body):
        return Resp(429, "", {"x-rate-limit-remaining": "0", "x-rate-limit-reset": "1800000999"})

    r = cmd(Client(h_429), {"action": "user_tweets", "user_id": "1"})
    check("L3 429 답에도 _rl(풀리는 시각)", r.get("status") == 429 and r.get("_rl", {}).get("reset") == 1800000999, r)

    # ── H: 해시 캐시 ──
    hp = data_dir / "graphql_hashes.json"
    if hp.exists():
        hp.unlink()
    check("H1 캐시가 없으면 긁는다", d._skip_hash_scrape({}) is False)
    hp.write_text(json.dumps({"SearchTimeline": "abc", "_timestamp": time.time() - 3600}))
    check("H2 1시간 된 캐시 — 긁지 않는다", d._skip_hash_scrape({}) is True)
    check("H3 force 면 긁는다", d._skip_hash_scrape({"force": True}) is False)
    check("H5 캐시로 답할 때의 답 — status ok · cached True · updated []",
          d._cached_hash_reply({}) == {"status": "ok", "cached": True, "updated": []}, d._cached_hash_reply({}))
    check("H6 force 면 캐시로 답하지 않는다(긁는다)", d._cached_hash_reply({"force": True}) is None)
    check("H7 바로 앞이 404 면 force 가 없어도 긁는다(고침 꾸러미로 이 데몬을 받은 옛 앱의 수리 길)",
          d._cached_hash_reply({}, recent_404=1) is None and d._cached_hash_reply({}, recent_404=0) is not None)
    hp.write_text(json.dumps({"SearchTimeline": "abc", "_timestamp": time.time() - 13 * 3600}))
    check("H4 13시간 된 캐시 — 긁는다", d._skip_hash_scrape({}) is False)

    # ── T: 트윗 상세 fieldToggles ──
    def h_td(method, url, params, body):
        return Resp(200, {"data": {}})

    c = Client(h_td)
    cmd(c, {"action": "tweet_detail", "tweet_id": "1"})
    ft = json.loads((c.calls[0]["params"] or {}).get("fieldToggles", "{}"))
    check("T2 트윗 상세 — GET · fieldToggles.withArticlePlainText = True",
          c.calls[0]["method"] == "GET" and ft.get("withArticlePlainText") is True, ft)
    c = Client(h_td)
    cmd(c, {"action": "user_by_screen_name", "screen_name": "tgt"})
    up = c.calls[0]["params"] or {}
    check("T3 사용자 조회 — USER_FEATURES · fieldToggles 그대로", json.loads(up.get("features", "{}")) == USER_FEATURES
          and json.loads(up.get("fieldToggles", "{}")) == {"withAuxiliaryUserLabels": False}, up)

    # ── 기능 플래그(웹 앱 값) — x_session 을 불러올 수 있을 때만(httpx 가 있어야 한다) ──
    try:
        sys.path.insert(0, str(TOOLS))
        import x_session  # noqa: E402
        f = x_session.FEATURES
        check("F1 x_session.FEATURES 에 웹 앱의 키(rweb_cashtags_enabled · articles_preview_enabled …) · 옛 키도 남음",
              f.get("rweb_cashtags_enabled") is True and f.get("articles_preview_enabled") is True
              and "tweetypie_unmention_optimization_enabled" in f and f.get("longform_notetweets_inline_media_enabled") is False,
              len(f))
    except ImportError as e:
        print("  - x_session 을 불러오지 못해 기능 플래그 확인은 건너뜀:", e)

    shutil.rmtree(data_dir, ignore_errors=True)
    return check.summary()


if __name__ == "__main__":
    sys.exit(main())
