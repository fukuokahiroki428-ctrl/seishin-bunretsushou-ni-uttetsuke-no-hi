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
  "rate_limit_once": "search"         # the next request of this action answers 429, once (optional)
}
"""
import json, os, re, sys
from datetime import datetime, timezone

HOME = os.path.expanduser("~/fake_x")
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
                       "statuses_count": len(st["own"]), "description": "", "created_at": xdate(1600000000000)}}


def tweet_obj(st, t, sn=None):
    n = int(t["id"][-3:]) if t["id"][-3:].isdigit() else 0
    leg = {"id_str": t["id"], "full_text": t.get("text") or ("tweet " + t["id"]), "created_at": xdate(t["ms"]),
           "favorite_count": 1, "retweet_count": 2, "reply_count": 0, "quote_count": 0, "lang": "ja",
           "possibly_sensitive": False, "conversation_id_str": t["id"], "source": '<a href="x">Twitter Web App</a>',
           "in_reply_to_screen_name": t.get("reply_to") or "",
           "entities": {"hashtags": [{"text": "tag%d" % (n % 3)}] if n % 3 == 0 else [], "urls": []}}
    if n % 4 == 0:   # 미디어 있는 글(받지는 않는다 — 시험은 미디어 받기 끔)
        leg["extended_entities"] = {"media": [{"type": "photo", "media_url_https": "https://pbs.twimg.com/media/fake%s.jpg" % t["id"]}]}
    if t.get("reply_to"):
        leg["in_reply_to_status_id_str"] = t.get("reply_to_id", "1")
    return {"rest_id": t["id"], "core": {"user_results": {"result": user_obj(st, sn)}}, "legacy": leg}


def rt_obj(st, rt, own_by_id):
    orig = own_by_id.get(rt["orig_id"]) or {"id": rt["orig_id"], "ms": rt["ms"] - 3600000, "text": "orig " + rt["orig_id"]}
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
    items.sort(key=lambda t: int(t["id"]), reverse=True)
    if st.get("search_depth"):   # 진짜 X 처럼 한 질의의 커서가 이 깊이에서 마른다 — 날 한가운데서 끊긴다
        items = items[:st["search_depth"]]
    chunk, nxt, off = page_of(items, args.get("cursor", ""), page)
    entries = [entry(tweet_obj(st, t)) for t in chunk]
    if nxt < len(items):
        entries.append(cursor_entry(str(nxt)))
    logreq({"action": "search", "query": q, "off": off, "n": len(chunk)})
    return {"status": 200, "body": timeline_body(entries, "search")}


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
        return do_search(st, args)
    if a == "user_tweets":
        return do_user_tweets(st, args)
    if a == "likes":
        return do_likes(st, args)
    if a == "bookmarks":
        return do_bookmarks(st, args)
    logreq({"action": a, "unknown": True})
    return {"status": 200, "body": {"data": {}}}


def main():
    os.makedirs(HOME, exist_ok=True)
    init_line = sys.stdin.readline()   # init args — 가짜 토큰. 값은 적지 않고 열쇠 이름과 프록시 출구만 남긴다
    try:                               # (proxy_exit.py 가 '앱이 계정의 프록시를 데몬에 실어 보내는지' 를 본다)
        ia = json.loads(init_line or "{}")
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
        a = args.get("action", "")
        if a == "quit":
            print(json.dumps({"status": "bye"}), flush=True)
            break
        if a == "ping":
            print(json.dumps({"status": "pong"}), flush=True)
            continue
        if a == "update_endpoints":
            print(json.dumps({"status": "ok"}), flush=True)
            continue
        try:
            print(json.dumps(handle(args), ensure_ascii=False), flush=True)
        except Exception as e:
            print(json.dumps({"error": "fake: %r" % (e,)}), flush=True)


if __name__ == "__main__":
    main()
