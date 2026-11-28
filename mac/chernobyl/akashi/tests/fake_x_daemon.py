#!/usr/bin/env python3
"""가짜 X — 시험용 twitter_daemon.py(네트워크 없음). 격리 사본의 script_overrides 에만 넣는다(번들 · 진짜 자료 폴더는 건드리지 않는다).

Fake twitter_daemon.py for offline end-to-end tests of TwitterCollector (no network).

Speaks the app's daemon protocol: first stdin line = init args (only its keys, proxy and the fake token's last letter are logged to
~/fake_x/init.jsonl), then one JSON command per line,
one JSON reply per line ({"status": 200, "body": <X GraphQL-shaped JSON>}).
State comes from ~/fake_x/state.json (re-read on every command, so a test can change it between runs);
every request is appended to ~/fake_x/requests.jsonl.

state = {
  "user": {"id": "1001", "screen_name": "tgt"},
  "own":  [{"id": "...", "ms": 1700000000000, "text": "...", "reply_to": "someone"|null}],   # search corpus (tweets + replies)
  "timeline_extra": [{"kind": "rt", "id": "...", "orig_id": "...", "ms": ...},
                     {"kind": "module", "ids": ["rootId", "replyId"]}],                     # merged into UserTweets by position
  "search_hidden": ["id", ...],      # own tweets search never returns
  "likes": [{"id": "...", "ms": ..., "author": "x"}],   # newest-liked first
  "page": 20,
  "end_mode": "shaped" | "errors",    # what UserTweets / likes return past the end
  "search_depth": 25,                 # a search query's cursor dries up after this many results (optional)
  "timeline_off": true,               # UserTweets returns nothing (optional)
  "rate_limit_once": "search",        # the next request of this action answers 429, once (optional)
  "search_gap_days": 20,              # like the real X, an until: search cannot bridge a gap longer than this (optional)
  "user": {..., "created_ms": ms}     # when the account was made (optional; null = unknown)
  --- r56 (X API layer) ---
  "replace_cursor": true,             # default true: like the real X, pages after the first carry cursor-bottom in a TimelineReplaceEntry
  "search_gap_since": true,           # also apply search_gap_days to queries with since: (default off — old tests)
  "suspended_tokens": ["tok", ...],   # accounts (auth_token) X treats as suspended: search is gated, account_status says suspended
  "rl": {"limit": 50, "remaining": 49, "reset": 1800000000}   # sent back as "_rl" on every API reply (optional)
}
search operators: from: (own corpus), until: / since: (UTC dates), since_id:, max_id:, until_time: / since_time: (unix seconds),
filter:replies, filter:nativeretweets / include:nativeretweets (native RTs = timeline_extra kind "rt"; plain from: never returns them,
with filter:nativeretweets only RTs come back).
r56 media fixtures (optional, all keys may be absent — then nothing changes):
  own[i] may carry  "photos": [{"key": "k1", "alt": "..."}], "video": {"key": "v1", "hls_only": true},
                    "card": {"title": "...", "url": "https://...", "img": "c1"}, "article": {"title", "text", "cover"},
                    "quote": {"id", "ms", "text", "author", "photos": [...]}, "edit_ids": [...], "note": {"text", "urls": [...]}
  timeline_extra rt may carry "orig_fx": {...the same keys...} for the retweeted original
  "media_base": "http://127.0.0.1:PORT"    # media URLs point at the test's fake media server (serve_media below)
  "followers" / "following": [{"id": "...", "sn": "..."}], "followers_fail_from_page": 3   # HTTP 500 from that page on
  "conversations": {"<tweet id>": [own-like items with "author"]}                         # TweetDetail replies (Phase 4)
"""
import json, os, re, sys
from datetime import datetime, timezone

HOME = os.path.expanduser("~/fake_x")
TOKEN = ""   # 이 데몬이 받은 계정(가짜 auth_token) — 정지 계정 흉내(suspended_tokens)에 쓴다. 값은 어디에도 적지 않는다
STATE = os.path.join(HOME, "state.json")
REQLOG = os.path.join(HOME, "requests.jsonl")


def state():
    with open(STATE, encoding="utf-8") as f:
        return json.load(f)


def logreq(o):
    with open(REQLOG, "a", encoding="utf-8") as f:
        f.write(json.dumps(o, ensure_ascii=False) + "\n")


def xdate(ms):
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%a %b %d %H:%M:%S +0000 %Y")


def user_obj(st, sn=None):
    u = st["user"]
    sn = sn or u["screen_name"]
    return {"rest_id": u["id"] if sn == u["screen_name"] else "9" + str(abs(hash(sn)) % 10**8),
            "legacy": {"screen_name": sn, "name": sn, "followers_count": 1, "friends_count": 1,
                       "statuses_count": len(st["own"]), "description": "",
                       **({} if created_ms(st) is None else {"created_at": xdate(created_ms(st))})}}


def created_ms(st):
    """계정을 만든 때 — user.created_ms(None 이면 모름). 없으면 가장 오래된 제 글 이틀 전(글이 없으면 2020-09-13).
    ★ 앱은 '전체' 걸음을 계정을 만든 날까지 내려간다(r55). 예전처럼 2020 으로 두면 최근 글만 둔 시험들이 몇 년치 빈 주를 걸었다."""
    u = st["user"]
    if "created_ms" in u:
        return u["created_ms"]
    own = [t["ms"] for t in st.get("own", [])]
    return (min(own) - 2 * 24 * 3600 * 1000) if own else 1600000000000


def tweet_obj(st, t, sn=None):
    n = int(t["id"][-3:]) if t["id"][-3:].isdigit() else 0
    leg = {"id_str": t["id"], "full_text": t.get("text") or ("tweet " + t["id"]), "created_at": xdate(t["ms"]),
           "favorite_count": 1, "retweet_count": 2, "reply_count": 0, "quote_count": 0, "lang": "ja",
           "possibly_sensitive": False, "conversation_id_str": t["id"], "source": '<a href="x">Twitter Web App</a>',
           "in_reply_to_screen_name": t.get("reply_to") or "",
           "entities": {"hashtags": [{"text": "tag%d" % (n % 3)}] if n % 3 == 0 else [], "urls": []}}
    if n % 4 == 0:   # 미디어 있는 글(받지는 않는다 — 시험은 미디어 받기 끔. 받는 시험은 media_base 의 가짜 서버로)
        leg["extended_entities"] = {"media": [{"type": "photo", "media_url_https": "%s/media/fake%s.jpg" % (media_base(st), t["id"])}]}
    if t.get("reply_to"):
        leg["in_reply_to_status_id_str"] = t.get("reply_to_id", "1")
    return decorate(st, t, {"rest_id": t["id"], "core": {"user_results": {"result": user_obj(st, sn)}}, "legacy": leg})


def rt_obj(st, rt, own_by_id):
    orig = own_by_id.get(rt["orig_id"]) or dict({"id": rt["orig_id"], "ms": rt["ms"] - 3600000, "text": "orig " + rt["orig_id"]},
                                                **rt.get("orig_fx", {}))
    o = tweet_obj(st, {"id": rt["id"], "ms": rt["ms"], "text": "RT @other: " + orig.get("text", "")})
    o["legacy"]["retweeted_status_result"] = {"result": tweet_obj(st, orig, "other")}
    return o


def entry(o):
    return {"entryId": "tweet-" + o["rest_id"],
            "content": {"entryType": "TimelineTimelineItem",
                        "itemContent": {"tweet_results": {"result": o}}}}


def module_entry(objs):
    return {"entryId": "profile-conversation-" + objs[-1]["rest_id"],
            "content": {"entryType": "TimelineTimelineModule",
                        "items": [{"item": {"itemContent": {"tweet_results": {"result": o}}}} for o in objs]}}


def cursor_entry(v):
    return {"entryId": "cursor-bottom-" + v, "content": {"value": v}}


def page_of(items, cursor, size):
    off = int(cursor) if cursor and cursor.isdigit() else 0
    return items[off:off + size], off + size, off


def timeline_body(entries, root):
    ins = [{"type": "TimelineAddEntries", "entries": entries}]
    if root == "user":
        return {"data": {"user": {"result": {"timeline_v2": {"timeline": {"instructions": ins}}}}}}
    return {"data": {"search_by_raw_query": {"search_timeline": {"timeline": {"instructions": ins}}}}}


def do_search(st, args):
    q = args.get("query", "")
    page = st.get("page", 20)
    hidden = set(st.get("search_hidden", []))
    items = [t for t in st["own"] if t["id"] not in hidden]
    items = search_corpus_rts(st, q, items)   # r56: 리트윗은 filter:nativeretweets 일 때만(그때는 리트윗만)
    if "filter:replies" in q:
        items = [t for t in items if t.get("reply_to")]
    m = re.search(r"until:(\d{4}-\d{2}-\d{2})", q)
    if m:
        lim = datetime.strptime(m.group(1), "%Y-%m-%d").replace(tzinfo=timezone.utc).timestamp() * 1000
        items = [t for t in items if t["ms"] < lim]
    m = re.search(r"since:(\d{4}-\d{2}-\d{2})", q)
    if m:
        lim = datetime.strptime(m.group(1), "%Y-%m-%d").replace(tzinfo=timezone.utc).timestamp() * 1000
        items = [t for t in items if t["ms"] >= lim]
    m = re.search(r"since_id:(\d+)", q)
    if m:
        items = [t for t in items if int(t["id"]) > int(m.group(1))]
    items = search_id_time_ops(q, items)      # r56: max_id: · until_time: · since_time:
    items.sort(key=lambda t: int(t["id"]), reverse=True)
    # 진짜 X 처럼 until: 검색은 긴 공백을 한 번에 건너지 못한다 — until 에서(또는 앞 글에서) 다음 글까지가 search_gap_days 보다
    #   멀면 거기서 끊긴다(바로 아래 글이 멀면 빈 결과). 첫 글만 보면 날짜 경계에 걸친 묶음이 공백 너머의 글까지 한 쪽에 실었다.
    gm = re.search(r"until:(\d{4}-\d{2}-\d{2})", q)
    if st.get("search_gap_days") and gm and ("since:" not in q or st.get("search_gap_since")):
        prev = datetime.strptime(gm.group(1), "%Y-%m-%d").replace(tzinfo=timezone.utc).timestamp() * 1000
        gap = st["search_gap_days"] * 24 * 3600 * 1000
        kept = []
        for t in items:
            if prev - t["ms"] > gap:
                break
            kept.append(t)
            prev = t["ms"]
        items = kept
    if st.get("search_depth"):   # 진짜 X 처럼 한 질의의 커서가 이 깊이에서 마른다 — 날 한가운데서 끊긴다
        items = items[:st["search_depth"]]
    chunk, nxt, off = page_of(items, args.get("cursor", ""), page)
    entries = [search_item_entry(st, t) for t in chunk]
    if nxt < len(items):
        entries.append(cursor_entry(str(nxt)))
    logreq({"action": "search", "query": q, "off": off, "n": len(chunk), "acct": TOKEN[-1:]})
    return {"status": 200, "body": search_replace_cursor(st, args, timeline_body(entries, "search"))}


# ── r56: 검색 쪽 흉내(리트윗 · ID/시각 연산자 · ReplaceEntry 커서 · 정지 계정) ──
def search_corpus_rts(st, q, items):
    """진짜 X 처럼 from: 검색은 리트윗을 돌려주지 않는다. filter:nativeretweets(또는 include:nativeretweets)면 리트윗도 싣고,
    filter:nativeretweets 면 리트윗만. 리트윗 = timeline_extra 의 kind 'rt'."""
    if "filter:nativeretweets" not in q and "include:nativeretweets" not in q:
        return items
    rts = [dict(x, _rt=True) for x in st.get("timeline_extra", []) if x.get("kind") == "rt"]
    return rts if "filter:nativeretweets" in q else items + rts


def search_id_time_ops(q, items):
    m = re.search(r"max_id:(\d+)", q)
    if m:
        items = [t for t in items if int(t["id"]) <= int(m.group(1))]
    m = re.search(r"until_time:(\d+)", q)
    if m:
        items = [t for t in items if t["ms"] < int(m.group(1)) * 1000]
    m = re.search(r"since_time:(\d+)", q)
    if m:
        items = [t for t in items if t["ms"] >= int(m.group(1)) * 1000]
    return items


def search_item_entry(st, t):
    if t.get("_rt"):
        own_by_id = {o["id"]: o for o in st["own"]}
        return entry(rt_obj(st, t, own_by_id))
    return entry(tweet_obj(st, t))


def search_replace_cursor(st, args, body):
    """진짜 X 는 검색의 2쪽부터 cursor-bottom 을 TimelineAddEntries 가 아니라 TimelineReplaceEntry 에 싣는다(twikit client.py 의 폴백).
    replace_cursor=false 면 예전처럼 AddEntries 에."""
    if not args.get("cursor") or not st.get("replace_cursor", True):
        return body
    ins = body["data"]["search_by_raw_query"]["search_timeline"]["timeline"]["instructions"]
    add = ins[0]["entries"]
    cur = [e for e in add if e["entryId"].startswith("cursor-bottom")]
    for e in cur:
        add.remove(e)
        ins.append({"type": "TimelineReplaceEntry", "entry_id_to_replace": "cursor-bottom-0",
                    "entry": {"entryId": e["entryId"], "content": {"entryType": "TimelineTimelineCursor",
                                                                   "value": e["content"]["value"], "cursorType": "Bottom"}}})
    return body


def suspended(st):
    return bool(TOKEN) and TOKEN in st.get("suspended_tokens", [])


def suspended_search_reply(st, args):
    gate = "LatestTabSrpDenySuspendedUsers"
    logreq({"action": "search", "query": args.get("query", ""), "suspended": True, "acct": TOKEN[-1:]})
    return {"status": 200, "_cause": "account_suspended", "_gate": gate,
            "body": {"data": {"search_by_raw_query": {"search_timeline": {}}},
                     "errors": [{"message": "InvalidArgument: ProductDisabled: Closed gate stopped execution of the pipeline (%s)" % gate,
                                 "name": "InvalidArgument", "path": ["search_by_raw_query", "search_timeline", "timeline"]}]}}


def account_status_reply(st):
    logreq({"action": "account_status", "acct": TOKEN[-1:]})
    return {"status": 200, "suspended": suspended(st), "protected": False, "needs_phone_verification": False,
            "locked": False, "error_code": 0, "user_id": "77" + (TOKEN[-1:] if TOKEN[-1:].isdigit() else "0")}


def end_page(st, cursor):
    if st.get("end_mode") == "errors":
        return {"status": 200, "body": {"errors": [{"message": "Timeout: Unspecified"}]}}
    return {"status": 200, "body": timeline_body([cursor_entry(cursor or "end")], "user")}


def do_user_tweets(st, args):
    own_by_id = {t["id"]: t for t in st["own"]}
    in_module = set()
    rows = []   # (sort id, entry)
    for x in st.get("timeline_extra", []):
        if x["kind"] == "module":
            objs = [tweet_obj(st, own_by_id[i]) for i in x["ids"] if i in own_by_id]
            in_module.update(x["ids"])
            rows.append((max(int(i) for i in x["ids"]), module_entry(objs)))
        elif x["kind"] == "rt":
            rows.append((int(x["id"]), entry(rt_obj(st, x, own_by_id))))
    for t in st["own"]:
        if t.get("reply_to") or t["id"] in in_module:
            continue   # UserTweets 는 답글을 싣지 않는다(대화 묶음 안의 것은 묶음으로)
        rows.append((int(t["id"]), entry(tweet_obj(st, t))))
    rows.sort(key=lambda r: r[0], reverse=True)
    if st.get("timeline_off"):   # 타임라인이 메워 주지 못하는 옛 구간 — 검색의 구멍을 드러낸다
        rows = []
    page = st.get("page", 20)
    chunk, nxt, off = page_of(rows, args.get("cursor", ""), page)
    logreq({"action": "user_tweets", "off": off, "n": len(chunk)})
    if not chunk:
        return end_page(st, args.get("cursor", ""))
    return {"status": 200, "body": timeline_body([r[1] for r in chunk] + [cursor_entry(str(nxt))], "user")}


def do_likes(st, args):
    page = st.get("page", 20)
    items = st.get("likes", [])
    chunk, nxt, off = page_of(items, args.get("cursor", ""), page)
    logreq({"action": "likes", "off": off, "n": len(chunk)})
    if not chunk:
        return end_page(st, args.get("cursor", ""))
    entries = [entry(tweet_obj(st, t, t.get("author", "other"))) for t in chunk] + [cursor_entry(str(nxt))]
    return {"status": 200, "body": timeline_body(entries, "user")}


def do_bookmarks(st, args):
    page = st.get("page", 20)
    items = st.get("bookmarks", [])
    chunk, nxt, off = page_of(items, args.get("cursor", ""), page)
    logreq({"action": "bookmarks", "off": off, "n": len(chunk)})
    ins = [{"type": "TimelineAddEntries", "entries": [entry(tweet_obj(st, t, t.get("author", "other"))) for t in chunk]
            + [cursor_entry(str(nxt) if chunk else (args.get("cursor") or "end"))]}]
    return {"status": 200, "body": {"data": {"bookmark_timeline_v2": {"timeline": {"instructions": ins}}}}}


# ── r56 미디어 · 글 갈래 흉내(대체 글 · 카드 · X 글 · HLS 만 있는 영상 · 리트윗한 인용 글 · 고친 이력 · 긴 글) ──
def media_base(st):
    return st.get("media_base") or "https://pbs.twimg.com"


def decorate(st, t, o):
    """state 의 글 항목에 붙은 r56 갈래를 X 모양으로 덧붙인다(없으면 o 그대로)."""
    leg = o["legacy"]
    base = media_base(st)
    media = []
    for p in t.get("photos", []):
        m = {"type": "photo", "media_url_https": "%s/media/%s.jpg" % (base, p["key"])}
        if p.get("alt"):
            m["ext_alt_text"] = p["alt"]
        media.append(m)
    v = t.get("video")
    if v:
        k = v["key"]
        variants = [{"content_type": "application/x-mpegURL", "url": "%s/vid/%s/pl/%s.m3u8" % (base, k, k)}]
        if not v.get("hls_only"):
            variants += [{"content_type": "video/mp4", "bitrate": 256000, "url": "%s/vid/%s/320x180/%s.mp4" % (base, k, k)},
                         {"content_type": "video/mp4", "bitrate": 2176000, "url": "%s/vid/%s/1280x720/%s.mp4" % (base, k, k)}]
        media.append({"type": "video", "media_url_https": "%s/media/%s_thumb.jpg" % (base, k), "video_info": {"variants": variants}})
    if media:
        leg["extended_entities"] = {"media": media}
        leg["entities"]["media"] = media
    c = t.get("card")
    if c:
        bv = [{"key": "title", "value": {"type": "STRING", "string_value": c.get("title", "")}},
              {"key": "card_url", "value": {"type": "STRING", "string_value": c.get("url", "")}}]
        if c.get("img"):
            bv.append({"key": "thumbnail_image_original",
                       "value": {"type": "IMAGE", "image_value": {"url": "%s/card_img/1/%s?format=jpg&name=orig" % (base, c["img"])}}})
        o["card"] = {"rest_id": "card://" + t["id"], "legacy": {"name": "summary_large_image", "url": c.get("url", ""),
                                                               "binding_values": bv}}
    a = t.get("article")
    if a:
        r = {"rest_id": "a" + t["id"], "title": a.get("title", ""), "preview_text": (a.get("text") or "")[:50],
             "plain_text": a.get("text", "")}
        if a.get("cover"):
            r["cover_media"] = {"media_info": {"original_img_url": "%s/media/%s.jpg" % (base, a["cover"])}}
        o["article"] = {"article_results": {"result": r}}
    q = t.get("quote")
    if q:
        o["quoted_status_result"] = {"result": tweet_obj(st, q, q.get("author", "other"))}
        leg["is_quote_status"] = True
    if t.get("edit_ids"):
        o["edit_control"] = {"edit_tweet_ids": list(t["edit_ids"])}
    n = t.get("note")
    if n:
        o["note_tweet"] = {"note_tweet_results": {"result": {"text": n.get("text", ""), "entity_set": {
            "urls": [{"url": "https://t.co/n%d" % i, "expanded_url": u} for i, u in enumerate(n.get("urls", []))],
            "hashtags": [{"text": h} for h in n.get("hashtags", [])]}}}}
    return o


def find_tweet(st, tid):
    """TweetDetail 의 그 글 — 제 글 · 리트윗된 원글 · 인용된 글 · 대화 답글에서 찾는다."""
    own_by_id = {t["id"]: t for t in st["own"]}
    if tid in own_by_id:
        return tweet_obj(st, own_by_id[tid])
    for x in st.get("timeline_extra", []):
        if x.get("kind") != "rt":
            continue
        if x["id"] == tid:
            return rt_obj(st, x, own_by_id)
        if x["orig_id"] == tid:
            return rt_obj(st, x, own_by_id)["legacy"]["retweeted_status_result"]["result"]
    for t in st["own"]:
        if t.get("quote", {}).get("id") == tid:
            return tweet_obj(st, t["quote"], t["quote"].get("author", "other"))
    for items in st.get("conversations", {}).values():
        for t in items:
            if t["id"] == tid:
                return tweet_obj(st, t, t.get("author", "friend"))
    return None


def do_tweet_detail(st, args):
    tid = str(args.get("tweet_id", ""))
    focal = find_tweet(st, tid)
    entries = [entry(focal)] if focal else []
    for t in st.get("conversations", {}).get(tid, []):
        o = tweet_obj(st, t, t.get("author", "friend"))
        entries.append({"entryId": "conversationthread-" + t["id"],
                        "content": {"entryType": "TimelineTimelineModule",
                                    "items": [{"entryId": "conversationthread-%s-tweet-%s" % (t["id"], t["id"]),
                                               "item": {"itemContent": {"tweet_results": {"result": o}}}}]}})
    logreq({"action": "tweet_detail", "tweet_id": tid, "found": bool(focal)})
    return {"status": 200, "body": {"data": {"threaded_conversation_with_injections_v2": {
        "instructions": [{"type": "TimelineAddEntries", "entries": entries}]}}}}


def do_follow_list(st, args, a):
    page = st.get("page", 20)
    users = st.get(a, [])
    off = int(args["cursor"]) if str(args.get("cursor", "")).isdigit() else 0
    pno = off // page + 1
    fail = st.get(a + "_fail_from_page")
    if fail and pno >= fail:
        logreq({"action": a, "off": off, "page": pno, "failed": True})
        return {"status": 500, "body": {}}
    chunk = users[off:off + page]
    entries = [{"entryId": "user-" + u["id"], "content": {"itemContent": {"user_results": {"result": {
        "rest_id": u["id"], "core": {"screen_name": u["sn"], "name": u.get("name", u["sn"])},
        "legacy": {"screen_name": u["sn"], "name": u.get("name", u["sn"]), "followers_count": 1, "friends_count": 1}}}}}}
               for u in chunk]
    if chunk and off + page < len(users):
        entries.append(cursor_entry(str(off + page)))
    logreq({"action": a, "off": off, "page": pno, "n": len(chunk)})
    return {"status": 200, "body": {"data": {"user": {"result": {"timeline": {"timeline": {
        "instructions": [{"type": "TimelineAddEntries", "entries": entries}]}}}}}}}


def serve_media(port=0, fail_once=(), log_path=None):
    """시험의 가짜 미디어 서버(127.0.0.1) — 어떤 경로든 작은 그림 · 영상 바이트를 준다. fail_once 의 글자를 품은 경로는
    처음 한 번만 503. 돌려주는 값: (서버, 포트). 시험이 끝나면 server.shutdown()."""
    import threading
    from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
    pending = set(fail_once)
    lock = threading.Lock()

    class H(BaseHTTPRequestHandler):
        def log_message(self, *a):
            pass

        def do_GET(self):
            path = self.path.split("?")[0]
            with lock:
                hit = next((f for f in pending if f in path), None)
                if hit:
                    pending.discard(hit)
            if log_path:
                with open(log_path, "a", encoding="utf-8") as f:
                    f.write(json.dumps({"path": self.path, "failed": bool(hit)}) + "\n")
            if hit:
                self.send_response(503)
                self.send_header("Content-Length", "0")
                self.end_headers()
                return
            body = (b"\x00\x00\x00\x18ftypmp42" if path.endswith(".mp4") else b"\xff\xd8\xff\xe0") + \
                path.encode() * 8 + b"\xff\xd9"
            self.send_response(200)
            self.send_header("Content-Type", "video/mp4" if path.endswith(".mp4") else "image/jpeg")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)

    srv = ThreadingHTTPServer(("127.0.0.1", port), H)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def handle(args):
    st = state()
    a = args.get("action", "")
    if st.get("rate_limit_once") == a:   # 이 동작의 다음 요청 하나만 429 — 계정이 도는 길을 시험한다
        st.pop("rate_limit_once")
        with open(STATE, "w", encoding="utf-8") as f:
            json.dump(st, f)
        logreq({"action": a, "rate_limited": True})
        return {"status": 429, "body": {}}
    if a == "user_by_screen_name":
        logreq({"action": a, "screen_name": args.get("screen_name")})
        return {"status": 200, "body": {"data": {"user": {"result": user_obj(st, args.get("screen_name"))}}}}
    if a == "search":
        return suspended_search_reply(st, args) if suspended(st) else do_search(st, args)
    if a == "account_status":
        return account_status_reply(st)
    if a == "user_tweets":
        return do_user_tweets(st, args)
    if a == "likes":
        return do_likes(st, args)
    if a == "bookmarks":
        return do_bookmarks(st, args)
    if a == "tweet_detail":
        return do_tweet_detail(st, args)
    if a in ("followers", "following") and a in st:
        return do_follow_list(st, args, a)
    logreq({"action": a, "unknown": True})
    return {"status": 200, "body": {"data": {}}}


def with_rl(o):
    """r56: state 의 rl 을 답의 _rl 로(진짜 데몬이 x-rate-limit-* 머리를 싣는 것처럼)"""
    try:
        rl = state().get("rl")
    except Exception:
        rl = None
    if rl and isinstance(o, dict) and "status" in o:
        o = dict(o, _rl=rl)
    return o


def main():
    os.makedirs(HOME, exist_ok=True)
    init_line = sys.stdin.readline()   # init args — 가짜 토큰. 값은 적지 않고 열쇠 이름과 프록시 출구만 남긴다
    global TOKEN
    try:                               # (proxy_exit.py 가 '앱이 계정의 프록시를 데몬에 실어 보내는지' 를 본다)
        ia = json.loads(init_line or "{}")
        TOKEN = str(ia.get("auth_token") or "")
        with open(os.path.join(HOME, "init.jsonl"), "a", encoding="utf-8") as f:
            # acct = 가짜 토큰의 끝 글자(계정과 출구의 짝을 본다)
            f.write(json.dumps({"keys": sorted(ia), "proxy": ia.get("proxy"), "acct": str(ia.get("auth_token") or "")[-1:]}) + "\n")
    except Exception:
        pass
    print(json.dumps({"status": "ready", "tid": True}), flush=True)
    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue
        try:
            args = json.loads(line)
        except Exception:
            continue
        # 진짜 데몬처럼 앱이 붙인 명령 번호(_rid)를 답에 돌려준다(r54)
        rid = args.pop("_rid", None) if isinstance(args, dict) else None

        def out(o):
            if rid is not None and isinstance(o, dict):
                o = dict(o, _rid=rid)
            print(json.dumps(o, ensure_ascii=False), flush=True)

        a = args.get("action", "")
        if a == "quit":
            out({"status": "bye"})
            break
        if a == "ping":
            out({"status": "pong"})
            continue
        # 늦은 답 흉내(state 의 stale_reply_once) — 앞 명령의 번호를 단 엉뚱한 답을 먼저 한 줄 낸다. 앱은 버려야 한다.
        if rid is not None:
            try:
                st = state()
                if st.pop("stale_reply_once", None):
                    with open(STATE, "w", encoding="utf-8") as f:
                        json.dump(st, f)
                    logreq({"action": a, "stale_reply": True})
                    print(json.dumps({"status": 200, "body": {"akashi_stale_reply": True},
                                      "_rid": "akashi-stale-" + str(rid)}), flush=True)
            except Exception:
                pass
        if a == "update_endpoints":
            out({"status": "ok"})
            continue
        try:
            out(with_rl(handle(args)))
        except Exception as e:
            out({"error": "fake: %r" % (e,)})


if __name__ == "__main__":
    main()
