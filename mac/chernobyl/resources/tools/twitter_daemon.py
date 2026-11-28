#!/usr/bin/env python3
"""Persistent Twitter API daemon - communicates via stdin/stdout JSON lines.

Usage: python3 twitter_daemon.py '{"auth_token":"...","ct0":"..."}'
Then send JSON commands on stdin (one per line), receive JSON responses on stdout.
Send {"action":"quit"} to exit.

Auto-repairs GraphQL endpoint hashes when X.com rotates them (HTTP 404).
Auto-refreshes auth cookies from Chrome when tokens expire (HTTP 401/403).
Designed for 10+ year stability with multiple fallback strategies.
"""
import sys
import json
import asyncio
import signal
import re
import os
import time
import subprocess
import platform


# ── GraphQL hash auto-repair ──────────────────────────────────────────────
# X.com rotates GraphQL queryIds every few weeks. When we get 404,
# fetch the current hashes from X.com's JS bundle and patch twikit's Endpoint class.

ENDPOINT_NAMES = [
    "SearchTimeline", "UserByScreenName", "UserTweets", "UserTweetsAndReplies",
    "UserMedia", "Likes", "Bookmarks", "Followers", "Following",
    "TweetDetail", "HomeTimeline", "HomeLatestTimeline",
    "UserHighlightsTweets", "Favoriters", "Retweeters",
    "ListLatestTweetsTimeline", "CommunityTweetsTimeline",
]

# ★ 캐시가 어디에 쌓이나 — 경로를 스스로 짓지 않는다.
#   이 자리는 두 번 틀렸다.
#     1) "~/Library/Application Support/カメラ" 에 멈춰 있었다(최초 이름).
#     2) 고친 뒤에도 ".../Miyo/<앱>" 으로 조립했는데, 조직 폴더를 없앤 뒤로
#        앱은 ".../<앱>" 을 쓴다. 다시 앱 밖에 캐시가 쌓였다.
#   이름을 받아 경로를 다시 조립하는 한 이 고장은 계속 돌아온다.
#   → 앱이 완성된 경로를 HANISHIKI_DATA_DIR 로 넘겨 준다. 그것만 쓴다.
_app_name = os.environ.get("HANISHIKI_APP_NAME") or os.environ.get("MIYO_APP_NAME") or "Hanishiki"
_data_dir = os.environ.get("HANISHIKI_DATA_DIR") or os.path.join(
    os.path.expanduser("~"), "Library", "Application Support", _app_name)
_hash_cache_path = os.path.join(_data_dir, "graphql_hashes.json")
os.makedirs(_data_dir, exist_ok=True)

# 옛 자리에 쌓여 있던 캐시를 한 번만 이어받는다(새 자리가 비어 있을 때만).
try:
    if not os.path.exists(_hash_cache_path):
        _legacy = os.path.join(os.path.expanduser("~"), "Library", "Application Support",
                               "Miyo", _app_name, "graphql_hashes.json")
        if os.path.exists(_legacy):
            import shutil as _sh
            _sh.copy2(_legacy, _hash_cache_path)
except Exception:
    pass


def _load_cached_hashes():
    """Load previously cached hashes from disk."""
    try:
        if os.path.exists(_hash_cache_path):
            with open(_hash_cache_path) as f:
                data = json.load(f)
                # Check if cache is less than 7 days old
                cached_time = data.get("_timestamp", 0)
                if time.time() - cached_time < 7 * 86400:
                    hashes = {k: v for k, v in data.items() if not k.startswith("_")}
                    return hashes
                # Stale cache — still return it but it may need refresh
                return {k: v for k, v in data.items() if not k.startswith("_")}
    except Exception:
        pass
    return {}


def _save_cached_hashes(hashes):
    """Save hashes to disk cache with timestamp."""
    try:
        os.makedirs(os.path.dirname(_hash_cache_path), exist_ok=True)
        data = dict(hashes)
        data["_timestamp"] = time.time()
        # 갈아 끼우기 — 이제 캐시의 나이가 긁을지 말지를 정한다(12시간). 반쯤 쓴 캐시는 해시를 잃게 한다
        _atomic_json_write(_hash_cache_path, data)
    except Exception:
        pass


# ★ 해시 캐시가 12시간보다 젊으면 기동 때의 긁기(x.com 첫 화면 + JS 번들 최대 20개)를 건너뛴다(r56).
#   예전엔 데몬을 띄울 때마다 긁었다 — '전체' 한 대상에 데몬이 열네 번쯤 뜨니 그때마다 몇 초 · 요청 스무 개 남짓이 X 로 나갔다.
#   해시가 정말 바뀌면 404 가 나고, 그때는 앱이 force 로 다시 부르고(404 수리 길) 데몬도 스스로 고친다(아래 메인 루프).
_HASH_FRESH_SECS = 12 * 3600


def _hash_cache_age():
    """해시 캐시를 적은 지 몇 초 됐나(파일 안의 _timestamp). 없거나 못 읽으면 None"""
    try:
        with open(_hash_cache_path) as f:
            ts = float((json.load(f) or {}).get("_timestamp", 0) or 0)
    except Exception:
        return None
    return (time.time() - ts) if ts > 0 else None


def _skip_hash_scrape(args):
    """update_endpoints 를 긁지 않고 캐시로 답해도 되나 — force 면 늘 긁는다"""
    if (args or {}).get("force"):
        return False
    age = _hash_cache_age()
    return age is not None and 0 <= age < _HASH_FRESH_SECS


def _cached_hash_reply(args, recent_404=0):
    """update_endpoints 를 긁지 않고 캐시로 답할 때의 답 — 긁어야 하면 None.
    ★ 답의 모양(status ok · cached True)을 시험(unit_daemon H5)이 볼 수 있게 본 루프에서 떼어 냈다(검토)
    ★ recent_404 = 바로 앞까지 이어진 404 수 — 404 뒤의 부름은 force 가 없어도 긁는다(옛 앱의 수리 길, r56 검토 DPC-1)"""
    if recent_404 or not _skip_hash_scrape(args):
        return None
    return {"status": "ok", "cached": True, "updated": []}


# ── GraphQL 을 GET 으로 보낼지 POST 로 보낼지 — 작업(operation)마다 기억한다(r56) ──
# ★ X 는 2026-11-21 부터 SearchTimeline 을 GET 으로 물으면 빈 본문 404 만 준다(어느 계정이든 · X 의 웹 앱 자신도 POST 로 보낸다 —
#   RSSHub #23359, twikit fork PR #6, gallery-dl #9275). 같은 404 가 예전엔 '해시가 바뀌었다' 의 뜻이라, 앱은 해시를 다시 긁고 데몬을
#   세 번 다시 띄운 뒤 검색을 포기했다. Followers 도 곧 POST 만 받는다는 보고가 있다. 그래서 작업마다:
#     · 처음엔 기본(SearchTimeline 은 POST, 나머지는 GET) 또는 기억해 둔 방법으로 보내고,
#     · 빈 본문 404 면 다른 방법으로 한 번 더 — 그게 200 이면 그 방법을 기억한다(<자료 폴더>/graphql_methods.json).
#     · 두 방법 다 404 면 예전처럼 해시 수리로 넘어간다.
#   7일 넘은 기억은 '다시 확인' 한다 — 기억한 방법으로 먼저 보내되(대개 맞다) 빈 404 면 다른 방법으로 물러서고, 맞으면 날짜를 새로 적는다.
_methods_path = os.path.join(_data_dir, "graphql_methods.json")
_METHOD_TTL = 7 * 86400
_POST_DEFAULT_OPS = {"SearchTimeline"}
_gql_methods = None


def _load_gql_methods():
    global _gql_methods
    if _gql_methods is None:
        try:
            with open(_methods_path) as f:
                d = json.load(f)
            _gql_methods = d if isinstance(d, dict) else {}
        except Exception:
            _gql_methods = {}
    return _gql_methods


def _atomic_json_write(path, obj):
    """반쯤 쓴 파일이 남지 않게 — 쓰는 데몬마다 제 임시 파일에 쓰고 갈아 끼운다(r56 검토). 앱은 데몬을 둘 이상 함께 띄운다 —
    예전처럼 모두가 같은 '.tmp' 이름을 쓰면 동시에 쓸 때 한 파일에 겹쳐 써 깨진 JSON 이 들어갈 수 있었다."""
    import tempfile
    d = os.path.dirname(path) or "."
    fd, tmp = tempfile.mkstemp(dir=d, prefix=os.path.basename(path) + ".", suffix=".tmp")
    try:
        with os.fdopen(fd, "w") as f:
            json.dump(obj, f)
        os.replace(tmp, path)
    except Exception:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def _save_gql_methods():
    """기억을 파일로"""
    try:
        _atomic_json_write(_methods_path, _load_gql_methods())
    except Exception:
        pass


def _gql_op(url):
    """엔드포인트 주소의 마지막 마디 = 작업 이름(SearchTimeline 등)"""
    from urllib.parse import urlparse
    return urlparse(url).path.rstrip("/").rsplit("/", 1)[-1]


def _gql_query_id(url):
    """작업 이름 바로 앞 마디 = queryId(update_endpoints 가 고쳐 넣은 값 그대로)"""
    from urllib.parse import urlparse
    parts = urlparse(url).path.rstrip("/").split("/")
    return parts[-2] if len(parts) >= 2 else ""


def _gql_first_method(op):
    e = _load_gql_methods().get(op)
    if isinstance(e, dict) and e.get("method") in ("GET", "POST"):
        return e["method"]
    return "POST" if op in _POST_DEFAULT_OPS else "GET"


def _gql_remember(op, method):
    """이 방법이 200 을 받았다 — 기억이 없고 기본과 다르거나, 다른 방법이 적혀 있거나, 7일이 지났으면 적는다"""
    m = _load_gql_methods()
    e = m.get(op) if isinstance(m.get(op), dict) else None
    default = "POST" if op in _POST_DEFAULT_OPS else "GET"
    now = time.time()
    if e is None and method == default:
        return   # 기본이 맞는다 — 적을 것이 없다(파일을 쓸데없이 쓰지 않는다)
    if e is not None and e.get("method") == method and now - float(e.get("at", 0) or 0) < _METHOD_TTL:
        return
    m[op] = {"method": method, "at": now}
    _save_gql_methods()


def _is_empty_404(response):
    try:
        return response.status_code == 404 and not (response.text or "").strip()
    except Exception:
        return False


async def _gql_send(client, method, url, variables, features, flatten_params, field_toggles=None):
    """한 방법으로 한 번. POST 몸은 X 웹 앱과 같은 {variables, features, queryId[, fieldToggles]}.
    ★ x-client-transaction-id 는 세션 계층(x_session · twikit)이 이 method 와 경로로 만든다 — POST 서명을 GET 으로 만들면 X 가 거절한다."""
    headers = dict(client._base_headers)
    if method == "POST":
        body = {"variables": variables, "features": features, "queryId": _gql_query_id(url)}
        if field_toggles is not None:
            body["fieldToggles"] = field_toggles
        headers["content-type"] = "application/json"
        return await client.request("POST", url, json=body, headers=headers, raise_exception=False)
    p = {"variables": variables, "features": features}
    if field_toggles:   # 빈 것은 GET 쿼리에 싣지 않는다(예전 GET 과 같은 모양)
        p["fieldToggles"] = field_toggles
    return await client.request("GET", url, params=flatten_params(p), headers=headers, raise_exception=False)


async def _gql_request(client, url, variables, features, flatten_params, field_toggles=None):
    """모든 GraphQL 작업이 이것으로 보낸다 — (data, response) 를 돌려준다(client.request 와 같은 꼴)"""
    op = _gql_op(url)
    first = _gql_first_method(op)
    data, response = await _gql_send(client, first, url, variables, features, flatten_params, field_toggles)
    if _is_empty_404(response):
        other = "POST" if first == "GET" else "GET"
        print(json.dumps({"info": "%s: %s 가 빈 404 — %s 로 한 번 더 묻습니다" % (op, first, other)}), flush=True)
        data2, response2 = await _gql_send(client, other, url, variables, features, flatten_params, field_toggles)
        if response2.status_code == 200:
            _gql_remember(op, other)
            print(json.dumps({"info": "%s: 이제 %s 로 보냅니다(기억함)" % (op, other)}), flush=True)
        return data2, response2
    if response.status_code == 200:
        _gql_remember(op, first)
    return data, response


def _rate_limit_of(response):
    """x-rate-limit-limit / remaining / reset 머리 → {"limit","remaining","reset"}(있는 것만). 없으면 None"""
    h = getattr(response, "headers", None) or {}
    out = {}
    for k in ("limit", "remaining", "reset"):
        v = None
        try:
            v = h.get("x-rate-limit-" + k)
            if v is None:
                v = h.get("X-Rate-Limit-" + k.capitalize())
        except Exception:
            v = None
        if v is None:
            continue
        try:
            out[k] = int(str(v).strip())
        except ValueError:
            pass
    return out or None


def _suspended_gate(body):
    """200 본문의 errors 가 '정지된 계정은 막는다' 문(gate)이면 그 문 이름, 아니면 None.
    ★ 정지된 계정으로 검색하면 X 는 200 과 함께 빈 search_timeline + errors[{"message": "...ProductDisabled: Closed gate stopped
      execution of the pipeline (LatestTabSrpDenySuspendedUsers)"}] 를 준다(2026-11-27 실측). 앱은 이것을 '모양 없는 빈 쪽'
      으로 보고 같은 질의를 되풀이했다. 이것은 해시 · 서명 문제가 아니라 계정 상태다 — 원인을 붙여 돌려준다."""
    errs = body.get("errors") if isinstance(body, dict) else None
    for e in errs or []:
        msg = str(e.get("message") or "") if isinstance(e, dict) else str(e)
        if "DenySuspendedUsers" in msg or ("ProductDisabled" in msg and "Suspended" in msg):
            m = (re.search(r"([A-Za-z0-9_]*DenySuspendedUsers[A-Za-z0-9_]*)", msg)
                 or re.search(r"\(([A-Za-z0-9_]+)\)\s*$", msg))
            return m.group(1) if m else "ProductDisabled"
    return None


def _lift_replace_cursor(ins):
    """instructions 하나 — TimelineReplaceEntry 에 든 cursor-bottom 을 TimelineAddEntries 의 entries 뒤에 붙인다(제자리 수정)"""
    lifted = []
    for inst in ins:
        if not isinstance(inst, dict) or inst.get("type") != "TimelineReplaceEntry":
            continue
        e = inst.get("entry")
        if not isinstance(e, dict):
            continue
        eid = str(e.get("entryId") or "")
        rep = str(inst.get("entry_id_to_replace") or "")
        if eid.startswith("cursor-bottom"):
            lifted.append(e)
        elif rep.startswith("cursor-bottom"):
            lifted.append(dict(e, entryId=rep))   # 앱은 entryId 로 커서를 찾는다
    if not lifted:
        return
    add = next((i for i in ins if isinstance(i, dict) and i.get("type") == "TimelineAddEntries"), None)
    if add is None:
        add = {"type": "TimelineAddEntries", "entries": []}
        ins.append(add)
    if not isinstance(add.get("entries"), list):
        add["entries"] = []
    for e in lifted:
        if e not in add["entries"]:
            add["entries"].append(e)


def _normalize_timeline(obj, depth=0):
    """타임라인 모양 본문(검색 · UserTweets · 좋아요 · 북마크 · 하이라이트 · 리스트 · 커뮤니티 · 트윗 상세 …)을 고르게 한다(r56).
    ★ X 는 검색의 2쪽부터 다음 쪽 커서(cursor-bottom)를 TimelineAddEntries 가 아니라 TimelineReplaceEntry(instruction.entry)에
      싣는다(twikit 2.3.3 client.py:771-778 이 그렇게 읽는다). 앱은 AddEntries 만 읽어 모든 검색이 두 쪽(약 40개)에서 끝났다 —
      '기간 지정' 이 그 달의 마지막 며칠만 받던 까닭이다. 여기서 그 커서를 AddEntries 로 옮겨 앱의 어느 읽는 곳이든 보게 한다.
      나머지는 건드리지 않는다. 트윗 묶음(entries · items) 안으로는 내려가지 않는다(커서는 instructions 바로 아래에만 있다)."""
    if depth > 12:
        return
    if isinstance(obj, dict):
        ins = obj.get("instructions")
        if isinstance(ins, list):
            _lift_replace_cursor(ins)
        for k, v in obj.items():
            if k in ("instructions", "entries", "items", "legacy") or not isinstance(v, (dict, list)):
                continue
            _normalize_timeline(v, depth + 1)
    elif isinstance(obj, list):
        for v in obj:
            if isinstance(v, (dict, list)):
                _normalize_timeline(v, depth + 1)


def _api_reply(response, data):
    """API 답 한 줄 — 200 이면 본문을 고르게 하고(_normalize_timeline) 정지 계정 문을 알리고, 레이트 리밋 머리를 붙인다(_rl)"""
    st = response.status_code
    out = {"status": st, "body": data if st == 200 else response.text}
    if st == 200 and isinstance(data, dict):
        _normalize_timeline(data)
        gate = _suspended_gate(data)
        if gate is not None:
            out["_cause"] = "account_suspended"
            out["_gate"] = gate
    rl = _rate_limit_of(response)
    if rl:
        out["_rl"] = rl
    return out


def _account_status_reply(response, data):
    """verify_credentials 의 답에서 상태만 — screen_name · 메일 · 토큰은 절대 내보내지 않는다(로그에 그대로 남는다)"""
    st = getattr(response, "status_code", 0)
    d = data if isinstance(data, dict) else {}
    codes = sorted(_x_error_codes(d))
    out = {"status": st,
           "suspended": bool(d.get("suspended")) or 64 in codes,
           "protected": bool(d.get("protected")),
           "needs_phone_verification": bool(d.get("needs_phone_verification")),
           "locked": 326 in codes,
           "error_code": codes[0] if codes else 0,
           "user_id": str(d.get("id_str") or d.get("rest_id") or "")}
    if st != 200 and codes:
        # 메인 루프의 401/403 처리(잠김 · 정지면 쿠키를 갈지 않는다)가 오류 번호를 읽는 자리 — 번호만 싣는다
        out["body"] = {"errors": [{"code": c} for c in codes]}
    rl = _rate_limit_of(response)
    if rl:
        out["_rl"] = rl
    return out


async def fetch_current_hashes(http_client, user_agent, auth_token=None, ct0=None):
    """Fetch current GraphQL hashes from X.com's JS bundles.
    Uses auth cookies for reliable access (avoids login redirects).
    Multiple fallback URL patterns for long-term stability.
    """
    import httpx

    headers = {
        "User-Agent": user_agent,
        "Accept": "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8",
        "Accept-Language": "en-US,en;q=0.9",
    }
    # Use auth cookies to avoid login redirect
    if auth_token and ct0:
        headers["Cookie"] = f"auth_token={auth_token}; ct0={ct0}"

    # Step 1: Fetch X.com homepage to find JS bundle URLs
    html = ""
    for url in ["https://x.com/home", "https://x.com", "https://twitter.com"]:
        try:
            resp = await http_client.get(url, headers=headers, follow_redirects=True)
            if resp.status_code == 200 and len(resp.text) > 1000:
                html = resp.text
                break
        except Exception:
            continue

    if not html:
        return None, "Failed to fetch x.com (all URLs failed)"

    # Find JS bundle URLs — multiple patterns for long-term stability
    js_urls = []

    # Pattern 1: abs.twimg.com/responsive-web/client-web (classic)
    js_urls += re.findall(r'https://abs\.twimg\.com/responsive-web/client-web[^"\'>\s]+\.js', html)

    # Pattern 2: href/src attributes containing client-web JS
    js_urls += re.findall(r'(?:href|src)="([^"]*client-web[^"]*\.js)"', html)

    # Pattern 3: Any twimg.com JS bundle
    js_urls += re.findall(r'https://[a-z]+\.twimg\.com/[^"\'>\s]+\.js', html)

    # Pattern 4: Relative paths
    rel_paths = re.findall(r'(?:href|src)="(/[^"]*\.js)"', html)
    for rp in rel_paths:
        if 'client' in rp or 'main' in rp or 'bundle' in rp:
            js_urls.append(f"https://x.com{rp}")

    # Deduplicate while preserving order
    seen = set()
    unique_urls = []
    for u in js_urls:
        if u not in seen:
            seen.add(u)
            unique_urls.append(u)
    js_urls = unique_urls

    if not js_urls:
        return None, f"No JS bundle URLs found (HTML length: {len(html)})"

    # Step 2: Search JS bundles for GraphQL queryIds
    hashes = {}
    for js_url in js_urls[:20]:  # Check first 20 bundles
        if len(hashes) >= len(ENDPOINT_NAMES):
            break
        try:
            js_resp = await http_client.get(js_url, headers={"User-Agent": user_agent})
            js_text = js_resp.text

            for name in ENDPOINT_NAMES:
                if name in hashes:
                    continue
                # Multiple patterns — X.com changes these periodically
                patterns = [
                    # Standard minified
                    rf'queryId:"([A-Za-z0-9_-]+)",operationName:"{name}"',
                    # JSON-style
                    rf'"queryId":"([A-Za-z0-9_-]+)","operationName":"{name}"',
                    # Reversed order
                    rf'operationName:"{name}"[^}}]{{0,50}}queryId:"([A-Za-z0-9_-]+)"',
                    # With braces
                    rf'\{{queryId:"([A-Za-z0-9_-]+)",operationName:"{name}"',
                    # Single quotes (unlikely but future-proof)
                    rf"queryId:'([A-Za-z0-9_-]+)',operationName:'{name}'",
                    # Spaced out
                    rf'queryId\s*:\s*"([A-Za-z0-9_-]+)"\s*,\s*operationName\s*:\s*"{name}"',
                    # e.exports pattern (webpack)
                    rf'exports\s*=\s*\{{\s*queryId\s*:\s*"([A-Za-z0-9_-]+)"[^}}]*operationName\s*:\s*"{name}"',
                ]
                for pat in patterns:
                    m = re.search(pat, js_text)
                    if m:
                        hashes[name] = m.group(1)
                        break
        except Exception:
            continue

    if not hashes:
        return None, f"No GraphQL hashes found in {len(js_urls)} JS bundles"

    return hashes, None


def apply_hashes_to_endpoint(Endpoint, hashes):
    """Patch twikit's Endpoint class with new hashes."""
    mapping = {
        "SearchTimeline": "SEARCH_TIMELINE",
        "UserByScreenName": "USER_BY_SCREEN_NAME",
        "UserTweets": "USER_TWEETS",
        "UserTweetsAndReplies": "USER_TWEETS_AND_REPLIES",
        "UserMedia": "USER_MEDIA",
        "Likes": "USER_LIKES",
        "Bookmarks": "BOOKMARKS",
        "Followers": "FOLLOWERS",
        "Following": "FOLLOWING",
        "TweetDetail": "TWEET_DETAIL",
        "HomeTimeline": "HOME_TIMELINE",
        "HomeLatestTimeline": "HOME_LATEST_TIMELINE",
        "UserHighlightsTweets": "USER_HIGHLIGHTS_TWEETS",
        "Favoriters": "FAVORITERS",
        "Retweeters": "RETWEETERS",
        "ListLatestTweetsTimeline": "LIST_LATEST_TWEETS_TIMELINE",
        "CommunityTweetsTimeline": "COMMUNITY_TWEETS_TIMELINE",
    }
    updated = []
    for name, query_id in hashes.items():
        attr = mapping.get(name)
        if attr and hasattr(Endpoint, attr):
            new_url = f"https://x.com/i/api/graphql/{query_id}/{name}"
            old_url = getattr(Endpoint, attr)
            if old_url != new_url:
                setattr(Endpoint, attr, new_url)
                updated.append(name)
    return updated


async def auto_repair_hashes(client, Endpoint, auth_token=None, ct0=None):
    """Fetch latest hashes from X.com and patch Endpoint class. Returns list of updated names."""
    hashes, err = await fetch_current_hashes(
        client.http, client._user_agent, auth_token=auth_token, ct0=ct0
    )
    if err:
        return None, err

    updated = apply_hashes_to_endpoint(Endpoint, hashes)
    if hashes:
        _save_cached_hashes(hashes)
    return updated, None


# ── Auto cookie refresh from Chrome ───────────────────────────────────────

LAST_COOKIE_ERROR = ''
# ★ 키체인에서 한 번 읽은 Chrome 쿠키 키(PBKDF2 를 거친 것) — 데몬이 사는 동안 다시 묻지 않는다(401 마다 키체인 창이 뜨지 않게).
#   풀리는 값이 하나도 없으면(키가 바뀜) 버리고 다음에 다시 읽는다.
_CHROME_KEY = None
# 키체인 창을 기다리는 시간 — 앱이 답을 기다리는 시간(보통 60초, 트윗 상세는 30초)보다 짧게. 사람이 맥 앞에 있으면
#   「허용」을 누를 시간은 된다(「항상 허용」이면 다음부터 묻지 않는다). r53 의 60초는 앱의 기다림과 같아 답이 늘 늦었다.
_KEYCHAIN_TIMEOUT = 25


def _x_uid_from_twid(v):
    """twid 쿠키 값('u%3D123' · '"u=123"')에서 X 사용자 번호만. 모르면 ''"""
    try:
        from urllib.parse import unquote
        s = unquote(v or '').strip().strip('"')
    except Exception:
        return ''
    if s.startswith('u='):
        s = s[2:]
    return s if s.isdigit() else ''


def _x_error_codes(body):
    """X 오류 응답 본문의 errors[].code 들(정수 집합). 못 읽으면 빈 집합"""
    try:
        data = json.loads(body) if isinstance(body, (str, bytes)) else body
        out = set()
        for e in (data or {}).get('errors') or []:
            if isinstance(e, dict):
                try:
                    out.add(int(e.get('code')))
                except (TypeError, ValueError):
                    pass
        return out
    except Exception:
        return set()


def _cookie_error(msg):
    """쿠키를 못 읽은 까닭 — 부르는 쪽이 오류 줄에 덧붙인다(예전엔 늘 'Failed to extract cookies' 뿐이라 까닭을 몰랐다)"""
    global LAST_COOKIE_ERROR
    LAST_COOKIE_ERROR = msg


def _aes_cbc_decrypt(key, data):
    """Chrome 쿠키 값(AES-128-CBC, IV=공백 16개, PKCS7) — 번들 파이썬의 cryptography(없으면 pycryptodomex)로. 실패하면 None"""
    iv = b' ' * 16
    try:
        try:
            from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
            d = Cipher(algorithms.AES(key), modes.CBC(iv)).decryptor()
            out = d.update(data) + d.finalize()
        except ImportError:
            from Cryptodome.Cipher import AES
            out = AES.new(key, AES.MODE_CBC, iv).decrypt(data)
    except Exception:
        return None
    if not out:
        return None
    pad = out[-1]
    if 1 <= pad <= 16 and out.endswith(bytes([pad]) * pad):
        return out[:-pad]
    return None


def extract_chrome_twitter_cookies(current_auth=None, expected_uid=None):
    """Chrome 의 쿠키 DB 에서 auth_token · ct0 를 꺼낸다. (auth_token, ct0) 또는 (None, None).

    ★ 이 계정과 '같은 X 계정' 임이 확인된 로그인만 돌려준다(검토, r54):
      (1) auth_token 이 지금 것(current_auth)과 같다 — 같은 로그인이고 ct0 만 바뀐 경우, 또는
      (2) 그 로그인의 twid(사용자 번호)가 expected_uid 와 같다 — 앱이 Chrome 에서 이 계정의 토큰을 가져올 때 적어 둔 번호.
      r53 은 Chrome 이 가장 최근에 쓴 프로필의 로그인을 아무것이나 받았다. 다른 계정이면 이 계정의 프록시로 남의 세션이
      나가고(계정마다 출구를 가르는 앱의 규칙을 깬다) 수집물이 다른 계정의 것이 됐다(북마크 등). r52 까지는 맥에서 쿠키를
      풀지 못해(/usr/lib/libcommonCrypto.dylib 가 디스크에 없다) 이 길이 한 번도 돌지 않았다.
    """
    global _CHROME_KEY
    auth_token = None
    ct0 = None
    _cookie_error('')
    exp_uid = str(expected_uid or '').strip()

    def _same_account(a, uid):
        return bool((current_auth and a == current_auth) or (exp_uid and uid == exp_uid))

    try:
        if platform.system() == "Darwin":
            # macOS: 쿠키 값은 키체인의 'Chrome Safe Storage' 로 만든 키(PBKDF2)로 AES-CBC 암호화('v10').
            #   ★ 앞으로의 Chrome · macOS 에 대비한다 —
            #     (1) 쿠키 파일이 <프로필>/Network/Cookies 로 옮겨 갈 수 있다(윈도우 · 리눅스는 이미 그렇다) — 둘 다 · 모든 프로필을 본다
            #     (2) 'v10' 이 아닌 방식(윈도우의 앱 묶음 암호화 'v20' 같은 것)이면 말없이 건너뛰지 않고 그렇다고 알린다
            #     (3) 쿠키 DB 메타 판 24 부터 값 앞에 도메인의 SHA-256(32바이트)이 붙는다 — 맞으면 떼어 낸다(안 떼면 토큰이 깨진다)
            #     (4) /usr/lib 의 dylib 경로를 직접 열지 않는다(그 파일은 이미 디스크에 없고 dyld 공유 캐시로만 풀린다) — 번들의 cryptography 로
            #     (5) 같은 X 계정의 로그인만 받는다(위 설명) — auth_token · ct0 · twid 는 같은 사이트의 것끼리 묶는다
            import sqlite3, tempfile, shutil, glob, hashlib
            # AES 라이브러리부터 — 없으면 'Chrome 에 로그인하라' 대신 그렇다고 알린다
            try:
                import cryptography.hazmat.primitives.ciphers  # noqa: F401
            except ImportError:
                try:
                    import Cryptodome.Cipher.AES  # noqa: F401
                except ImportError:
                    _cookie_error('이 파이썬(%s)에는 Chrome 쿠키를 풀 AES 라이브러리(cryptography · pycryptodomex)가 없습니다'
                                  % sys.executable)
                    return None, None
            base = os.path.expanduser("~/Library/Application Support/Google/Chrome")
            dbs = []
            for pdir in [os.path.join(base, 'Default')] + sorted(glob.glob(os.path.join(base, 'Profile *'))):
                for rel in ('Network/Cookies', 'Cookies'):
                    cand = os.path.join(pdir, rel)
                    if os.path.isfile(cand):
                        dbs.append(cand)

            def _mtime(path):
                try:
                    return os.path.getmtime(path)
                except OSError:
                    return 0
            dbs.sort(key=_mtime, reverse=True)   # 가장 최근에 쓴 프로필부터
            if not dbs:
                _cookie_error('Chrome 쿠키 파일을 찾지 못했습니다(~/Library/Application Support/Google/Chrome/<프로필>/(Network/)Cookies)')
                return None, None
            # 키체인은 쿠키 파일이 있을 때만 · 데몬이 사는 동안 한 번만 연다
            if _CHROME_KEY is None:
                try:
                    raw_key = subprocess.check_output(['security', 'find-generic-password', '-w',
                                                       '-s', 'Chrome Safe Storage', '-a', 'Chrome'],
                                                      stderr=subprocess.DEVNULL, timeout=_KEYCHAIN_TIMEOUT).strip()
                except subprocess.TimeoutExpired:
                    _cookie_error('키체인 창에 %d초 안에 답이 없어 Chrome 쿠키 키를 읽지 못했습니다 — 키체인 창에서 「항상 허용」을 누르면 '
                                  '다음부터는 묻지 않습니다' % _KEYCHAIN_TIMEOUT)
                    return None, None
                except subprocess.CalledProcessError:
                    _cookie_error('키체인에서 Chrome 쿠키 키를 읽지 못했습니다(Chrome 이 없거나 키체인 접근을 거부함)')
                    return None, None
                _CHROME_KEY = hashlib.pbkdf2_hmac('sha1', raw_key, b'saltysalt', 1003, dklen=16)
            dk = _CHROME_KEY
            now_us = int((time.time() + 11644473600) * 1000000)   # Chrome 의 시각: 1601년부터의 마이크로초
            unknown = set()
            read_errs, read_ok, dec_ok, dec_fail, others = [], 0, 0, 0, 0
            for db_path in dbs:
                rows, meta_ver = [], 0
                fd, tmp = tempfile.mkstemp(suffix=".db")
                os.close(fd)
                try:
                    shutil.copy2(db_path, tmp)   # Chrome 이 잠가 두므로 사본으로 읽는다
                    conn = sqlite3.connect(tmp)
                    try:
                        try:
                            row = conn.execute("SELECT value FROM meta WHERE key='version'").fetchone()
                            meta_ver = int(row[0]) if row else 0
                        except Exception:
                            meta_ver = 0
                        cols = {c[1] for c in conn.execute("PRAGMA table_info(cookies)")}
                        # ★ 도메인은 정확히 — LIKE '%x.com' 은 netflix.com · dropbox.com 의 auth_token 까지 잡았다(검토).
                        #   지난 쿠키 · 다른 사이트에 묶인(분할, CHIPS) 쿠키는 빼고, 같은 이름이면 나중에 만든 것이 이긴다.
                        where = ("host_key IN ('.x.com', 'x.com', '.twitter.com', 'twitter.com') "
                                 "AND name IN ('auth_token', 'ct0', 'twid')")
                        params = []
                        if 'expires_utc' in cols and 'has_expires' in cols:
                            where += " AND (has_expires = 0 OR expires_utc = 0 OR expires_utc > ?)"
                            params.append(now_us)
                        if 'top_frame_site_key' in cols:
                            where += " AND top_frame_site_key = ''"
                        order = " ORDER BY creation_utc" if 'creation_utc' in cols else ""
                        rows = conn.execute("SELECT name, encrypted_value, host_key FROM cookies WHERE " + where + order,
                                            params).fetchall()
                        read_ok += 1
                    finally:
                        conn.close()
                except Exception as e:
                    rows = []
                    # 말없이 넘기지 않는다 — 다 실패하면 'Chrome 에 로그인하라' 대신 이 까닭을 알린다(검토)
                    read_errs.append('%s: %s' % (os.path.relpath(db_path, base), e))
                finally:
                    try:
                        os.unlink(tmp)
                    except OSError:
                        pass
                by_site = {}
                for name, enc_val, host in rows:
                    if not enc_val or len(enc_val) < 4:
                        continue
                    if enc_val[:3] != b'v10':
                        unknown.add(enc_val[:3].decode('latin-1', 'replace'))
                        continue
                    plain = _aes_cbc_decrypt(dk, enc_val[3:])
                    if plain is None:
                        dec_fail += 1
                        continue
                    dec_ok += 1
                    if meta_ver >= 24 and len(plain) >= 32 and plain[:32] == hashlib.sha256((host or '').encode()).digest():
                        plain = plain[32:]
                    try:
                        by_site.setdefault((host or '').lstrip('.'), {})[name] = plain.decode('utf-8')
                    except UnicodeDecodeError:
                        continue
                for site in ('x.com', 'twitter.com'):
                    s = by_site.get(site) or {}
                    a, c = s.get('auth_token'), s.get('ct0')
                    if not (a and c):
                        continue
                    if _same_account(a, _x_uid_from_twid(s.get('twid', ''))):
                        return a, c
                    others += 1
            if dec_fail and not dec_ok:
                _CHROME_KEY = None     # 키가 바뀌었을 수 있다 — 다음에는 키체인에서 다시 읽는다
            detail = (' · 읽지 못한 쿠키 DB: ' + '; '.join(read_errs[:3])) if read_errs else ''
            if others:
                _cookie_error(('Chrome 의 X 로그인 %d개가 이 계정과 같은 계정인지 확인되지 않아 쓰지 않았습니다 — 설정 → 계정 토큰 자동 '
                               '추출 → Twitter 로 이 계정의 토큰을 다시 넣어 주십시오') % others + detail)
            elif unknown:
                _cookie_error('Chrome 의 쿠키 암호 방식이 바뀌어(%s) 읽을 수 없습니다 — 트위터 계정의 auth_token · ct0 를 설정에 직접 넣어 주십시오'
                              % ', '.join(sorted(unknown)) + detail)
            elif read_errs and not read_ok:
                _cookie_error('Chrome 쿠키 DB 를 읽지 못했습니다 — ' + '; '.join(read_errs[:3]))
            elif dec_fail and not dec_ok:
                _cookie_error('Chrome 쿠키를 풀지 못했습니다(키체인의 키가 바뀌었을 수 있습니다 — 다음에 다시 읽습니다)' + detail)
            else:
                _cookie_error('Chrome 에 X(트위터) 로그인 쿠키가 없습니다 — Chrome 에서 x.com 에 로그인한 뒤 다시 해 보십시오' + detail)
            return None, None
        elif platform.system() == "Windows":
            # Windows: Chrome cookies encrypted with DPAPI
            import sqlite3, tempfile, shutil

            local_app = os.environ.get("LOCALAPPDATA", "")
            cookie_paths = [
                os.path.join(local_app, "Google", "Chrome", "User Data", "Default", "Network", "Cookies"),
                os.path.join(local_app, "Google", "Chrome", "User Data", "Default", "Cookies"),
            ]
            db_path = None
            for p in cookie_paths:
                if os.path.exists(p):
                    db_path = p
                    break
            if not db_path:
                return None, None

            # Get encryption key from Local State
            local_state_path = os.path.join(local_app, "Google", "Chrome", "User Data", "Local State")
            with open(local_state_path) as f:
                local_state = json.load(f)
            import base64
            encrypted_key = base64.b64decode(local_state["os_crypt"]["encrypted_key"])
            encrypted_key = encrypted_key[5:]  # Remove DPAPI prefix

            import ctypes, ctypes.wintypes
            class DATA_BLOB(ctypes.Structure):
                _fields_ = [("cbData", ctypes.wintypes.DWORD), ("pbData", ctypes.POINTER(ctypes.c_char))]

            blob_in = DATA_BLOB(len(encrypted_key), ctypes.create_string_buffer(encrypted_key, len(encrypted_key)))
            blob_out = DATA_BLOB()
            ctypes.windll.crypt32.CryptUnprotectData(ctypes.byref(blob_in), None, None, None, None, 0, ctypes.byref(blob_out))
            aes_key = bytes(blob_out.pbData[:blob_out.cbData])

            tmp = tempfile.mktemp(suffix=".db")
            shutil.copy2(db_path, tmp)
            conn = sqlite3.connect(tmp)
            cursor = conn.cursor()
            cursor.execute(
                "SELECT name, encrypted_value FROM cookies "
                "WHERE (host_key LIKE '%twitter.com' OR host_key LIKE '%x.com') "
                "AND name IN ('auth_token', 'ct0')"
            )

            for name, enc_val in cursor.fetchall():
                if not enc_val or len(enc_val) < 4:
                    continue
                try:
                    if enc_val[:3] == b'v10':
                        nonce = enc_val[3:15]
                        ciphertext = enc_val[15:-16]
                        tag = enc_val[-16:]
                        # AES-GCM decryption
                        from cryptography.hazmat.primitives.ciphers.aead import AESGCM
                        aesgcm = AESGCM(aes_key)
                        decrypted = aesgcm.decrypt(nonce, ciphertext + tag, None).decode('utf-8')
                        if name == 'auth_token':
                            auth_token = decrypted
                        elif name == 'ct0':
                            ct0 = decrypted
                except Exception:
                    continue

            conn.close()
            os.unlink(tmp)

    except Exception as e:
        print(json.dumps({"info": f"Chrome cookie extraction failed: {e}"}), flush=True)

    # 윈도우 가지(이 파일의 것은 맥 앱에서 돌지 않는다 — 윈도우 앱은 windows/ 의 데몬을 쓴다)도 같은 로그인만 받는다
    if auth_token and ct0 and not _same_account(auth_token, ''):
        _cookie_error('Chrome 의 X 로그인이 이 계정과 같은 계정인지 확인되지 않아 쓰지 않았습니다')
        return None, None
    return auth_token, ct0


# ── Main daemon ───────────────────────────────────────────────────────────

def _reply(obj, rid=None):
    """명령의 마지막 답 한 줄. 앱이 붙인 명령 번호(_rid)를 그대로 돌려준다 — 앱은 번호가 다른 답(앞 명령의 늦은 답)을 버린다.
    ★ 키체인 창 · 해시 수리처럼 앱의 기다림(60초 · 트윗 상세 30초)보다 오래 걸린 명령의 답이 다음 명령의 답으로 읽혔다(검토, r54).
      번호 없이 온 명령(옛 앱)에는 예전처럼 번호 없이 답한다."""
    if rid is not None and isinstance(obj, dict):
        obj = dict(obj)
        obj["_rid"] = rid
    print(json.dumps(obj), flush=True)


def _read_init_line():
    """첫 줄만 원시 fd 에서 읽는다.
    ★ sys.stdin.readline() 을 쓰면 안 된다. 파이썬 버퍼가 줄 너머까지 미리 읽어
      버려서, 뒤이은 명령 루프(asyncio 가 fd 0 을 직접 붙인다)가 첫 명령을 잃는다."""
    buf = b""
    while True:
        ch = os.read(0, 1)
        if not ch or ch == b"\n":
            break
        buf += ch
    return buf.decode("utf-8", "replace")


def _load_init_args(usage):
    """자격증명을 받는다.
    ★ 예전에는 JSON 을 argv 로 받았다. macOS 에서 명령줄은 ps 로 같은 기계의 아무
      프로세스나 읽을 수 있어서, auth_token·ct0 세션 자격증명이 그대로 노출됐다.
      이제 stdin 첫 줄로 받는다. 뒤이은 명령 통로는 그대로다."""
    if len(sys.argv) >= 2 and sys.argv[1] == "--stdin-args":
        line = _read_init_line()
        if not line.strip():
            print(json.dumps({"error": "init args not received on stdin"}), flush=True)
            sys.exit(1)
        return json.loads(line)
    if len(sys.argv) >= 2:
        return json.loads(sys.argv[1])
    print(json.dumps({"error": usage}), flush=True)
    sys.exit(1)


async def main():
    init_args = _load_init_args("Usage: twitter_daemon.py --stdin-args  (init JSON on first stdin line)")
    auth_token = init_args["auth_token"]
    ct0 = init_args["ct0"]
    # ★ 이 계정의 X 사용자 번호 — 앱이 Chrome 에서 이 토큰을 가져올 때 적어 둔 것(토큰과 짝이 맞을 때만 온다).
    #   쿠키 자동 갱신은 지금 로그인 그대로이거나 이 번호와 같은 로그인만 받는다(extract_chrome_twitter_cookies).
    expected_uid = str(init_args.get("expected_uid") or "").strip()
    # 자격증명이 실제로 도착했는지 길이로만 알린다 — 값은 절대 찍지 않는다.
    #   전달 방식이 깨졌을 때 "X 가 막았다" 와 구별하려면 이 한 줄이 필요하다.
    print(json.dumps({"info": "자격증명 도착: auth_token %d자, ct0 %d자"
                              % (len(auth_token or ""), len(ct0 or ""))}), flush=True)

    # ── 어느 세션 계층을 쓸지 ──────────────────────────────────────────────
    # x_session 은 이 저장소가 직접 들고 있는 얇은 계층이다(tools/x_session.py).
    # twikit 은 최신이 2025-02 배포로 1년 넘게 멈춰 있고, 실제로 자기 TID 초기화가
    # 지금 X 에서 실패한다("Couldn't get KEY_BYTE indices"). 데몬이 그것을 손으로
    # 우회해 왔을 뿐이다. 그래서 우리 것을 먼저 쓰고, 안 되면 물러선다.
    #   ★ 물러설 자리를 남겨 두는 이유: 우리 것에 아직 못 본 구멍이 있을 수 있다.
    #     사용자의 주 수집기가 내 확신 때문에 멈추면 안 된다.
    USING_SHIM = False
    try:
        from x_session import Client, Endpoint, FEATURES, USER_FEATURES, flatten_params
        USING_SHIM = True
        print(json.dumps({"info": "세션 계층: x_session (앱 내장)"}), flush=True)
    except Exception as _se:
        try:
            from twikit import Client
            from twikit.client.gql import Endpoint, FEATURES, USER_FEATURES, flatten_params
            print(json.dumps({"info": f"세션 계층: twikit 으로 물러섬 ({_se})"}), flush=True)
        except ImportError:
            print(json.dumps({"error": "x_session 도 twikit 도 없습니다"}), flush=True)
            sys.exit(1)

    # ── Monkey-patch httpx.Cookies.get() BEFORE creating Client ──
    # Root cause: x.com sets duplicate cookies (guest_id_ads, etc.) via Set-Cookie headers.
    # httpx's Cookies.get() raises CookieConflict when it finds duplicates.
    # This happens deep inside httpx when building request headers.
    # Fix: Patch get() to return first value instead of raising error.
    import httpx
    _original_cookies_get = httpx.Cookies.get

    def _safe_cookies_get(self, name, default=None, domain=None, path=None):
        """Return first matching cookie instead of raising CookieConflict on duplicates."""
        value = None
        for cookie in self.jar:
            if cookie.name == name:
                if domain is None or cookie.domain == domain:
                    if path is None or cookie.path == path:
                        if value is not None:
                            # Duplicate found — just return first one (don't raise!)
                            continue
                        value = cookie.value
        return value if value is not None else default

    httpx.Cookies.get = _safe_cookies_get
    print(json.dumps({"info": "httpx.Cookies.get() patched: duplicate cookies tolerated"}), flush=True)

    # ── twikit 상류 버그 우회: 'code' KeyError ──
    # twikit/client/client.py 는 response_data['errors'][0]['code'] 를 무조건 읽는다.
    # 트위터는 'code' 없이 'message' 만, 또는 extensions.code 로만 주는 경우가 있다.
    #
    # ★ 예전엔 이 파일을 '디스크에 고쳐 썼다'. 그런데 그 파일은 서명된 앱 번들 안에 있다.
    #   한 글자만 바꿔도 codesign 봉인이 깨지고, 다음 기동 때 앱이 스스로 1분 넘게
    #   재서명을 돌린다. 그 사이에 앱을 끄면 번들이 무효인 채로 남는다.
    #   빌드할 때마다 새 twikit 이 들어오므로 '첫 트위터 수집' 마다 이 일이 반복됐다.
    #   봉인이 깨지는 진짜 원인이 여기였다.
    #
    #   → 디스크를 건드리지 않는다. 고친 소스를 메모리에서 컴파일해 모듈에 덮는다.
    #     아래 Client() 를 만들기 전이라 이미 만들어진 객체가 없어 안전하다.
    #     (바로 위 httpx.Cookies.get 과 같은 방식 — 실행 중에만 고친다)
    try:
        if USING_SHIM:
            raise RuntimeError("x_session 을 쓰므로 twikit 소스 패치는 건너뜁니다")
        import twikit.client.client as _twc
        _twc_path = _twc.__file__
        with open(_twc_path, 'r', encoding='utf-8') as _f:
            _src = _f.read()
        _bad = "error_code = response_data['errors'][0]['code']"
        _good = ("error_code = response_data['errors'][0].get('code') "
                 "or (response_data['errors'][0].get('extensions') or {}).get('code') or 0")
        if _bad in _src:
            exec(compile(_src.replace(_bad, _good), _twc_path, 'exec'), _twc.__dict__)
            print(json.dumps({"info": "twikit 'code' KeyError 우회 적용 (메모리에서만 — 번들 서명 유지)"}), flush=True)
        else:
            print(json.dumps({"info": "twikit 'code' 패턴 없음 — 상류가 이미 고쳤거나 구조가 바뀜"}), flush=True)
    except Exception as _pe:
        print(json.dumps({"info": f"twikit 우회 실패(수집은 계속 시도): {_pe}"}), flush=True)

    # ★ 프록시(VPN) — 계정마다 다른 출구로 나가게 한다. 앱이 계정에 붙은 프로필을
    #   URL 한 줄(init_args['proxy'])로 만들어 넘겨준다(윈도우와 같다).
    #   x_session.Client 는 AsyncClient(transport=...) 로 전송기를 직접 넣으므로 httpx 가
    #   ALL_PROXY 같은 환경 변수를 보지 않는다(_mounts 가 빈다). 예전엔 환경만 믿어서,
    #   계정에 프록시를 걸어도 트위터 API 요청은 이 컴퓨터의 회선으로 바로 나갔다
    #   (닫힌 길 127.0.0.1:9 도 먹지 않았다). 그래서 출구를 생성자에 직접 준다.
    _proxy = init_args.get("proxy") or ""
    # ★ 브라우저 흉내 — 앱이 설치된 Chrome 의 판으로 만든 UA 와 클라이언트 힌트를 넘겨 준다(Common::browserUserAgent).
    #   예전엔 늘 '윈도우 Chrome 153' 고정값으로 나갔다. 맥 Chrome 156 의 쿠키로 윈도우 153 이라 말하는 셈이라
    #   그 자체가 자동화 신호였고, 판이 갈수록 벌어졌다. 같은 계정의 캡처 Chrome 은 이미 맥 Chrome 으로 나간다.
    _ua = init_args.get("user_agent") or None
    _hints = {k: v for k, v in (init_args.get("client_hints") or {}).items()
              if k in ("sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform")}
    if USING_SHIM:
        client = Client(language="ja", proxy=(_proxy or None), user_agent=_ua, client_hints=_hints)
    else:
        try:
            client = Client(language="ja", user_agent=_ua) if _ua else Client(language="ja")
        except TypeError:
            client = Client(language="ja")
    if not _hints and USING_SHIM:
        _hints = dict(getattr(client, "_client_hints", {}) or {})
    _ua_major = re.search(r"Chrome/(\d+)", client._user_agent or "")
    print(json.dumps({"info": "브라우저 흉내: %s Chrome %s" % (
        "맥" if "Macintosh" in (client._user_agent or "") else "윈도우" if "Windows" in (client._user_agent or "") else "?",
        _ua_major.group(1) if _ua_major else "?")}), flush=True)
    client.set_cookies({
        "auth_token": auth_token,
        "ct0": ct0,
    })
    if _proxy:
        # 비밀번호는 찍지 않는다 — 로그가 그대로 남는다.
        _safe = _proxy
        if "@" in _safe:
            _safe = _safe.split("://")[0] + "://***@" + _safe.rsplit("@", 1)[1]
        print(json.dumps({"info": f"프록시 사용: {_safe}"}), flush=True)
    # ★ httpx timeout 길게 — Twitter API 가 가끔 응답 늦음 (Cloudflare / rate limit etc)
    #   default 5초 → 30초. ReadTimeout 자주 발생 방지.
    try:
        import httpx as _httpx
        new_timeout = _httpx.Timeout(30.0, connect=15.0, read=30.0, write=15.0, pool=10.0)
        client.http.timeout = new_timeout
        # 재시도 transport — connection error 시 자동 3회 retry
        #   ★ x_session 을 쓸 때는 건드리지 않는다. 그쪽 생성자가 이미
        #     AsyncHTTPTransport(retries=3) 과 30초 시간제한을 넣어 둔다.
        #   ★ 그리고 여기서 쓰던 것은 '동기' HTTPTransport 였다. 비동기 클라이언트에
        #     동기 전송기를 꽂으면 첫 요청에서 바로 깨진다 —
        #       'HTTPTransport' object has no attribute 'handle_async_request'
        #     twikit 시절에는 이 대입이 그쪽 구조 때문에 효력이 없어 드러나지 않았을
        #     뿐이고, 애초에 틀린 줄이었다. 비동기 것으로 바로잡는다.
        #   transport 를 통째로 바꾸므로 프록시 출구도 여기서 같이 준다.
        if not USING_SHIM:
            client.http._transport = (_httpx.AsyncHTTPTransport(retries=3, proxy=_proxy) if _proxy
                                      else _httpx.AsyncHTTPTransport(retries=3))
            print(json.dumps({"info": "httpx timeout=30s + retries=3 적용"}), flush=True)
        else:
            print(json.dumps({"info": "httpx 설정은 x_session 이 이미 함 — 건너뜀"}), flush=True)
    except Exception as _te:
        print(json.dumps({"info": f"timeout 설정 실패 (무시): {_te}"}), flush=True)

    def deduplicate_cookies(c):
        """Clean duplicate cookies from jar, keeping only first of each name."""
        try:
            seen = set()
            to_remove = []
            for cookie in list(c.http.cookies.jar):
                key = (cookie.name, cookie.domain, cookie.path)
                if key in seen:
                    to_remove.append(cookie)
                else:
                    seen.add(key)
            for cookie in to_remove:
                c.http.cookies.jar.clear(cookie.domain, cookie.path, cookie.name)
        except Exception:
            pass

    # Apply cached hashes first (instant, no network)
    cached = _load_cached_hashes()
    if cached:
        patched = apply_hashes_to_endpoint(Endpoint, cached)
        if patched:
            print(json.dumps({"info": f"Loaded cached hashes: {','.join(patched)}"}), flush=True)

    # Initialize client transaction — with auth cookies for reliable homepage fetch
    tid_ok = False
    ct_headers = {
        'Accept-Language': 'en-US,en;q=0.9',
        'Cache-Control': 'no-cache',
        'Referer': 'https://x.com',
        'User-Agent': client._user_agent,
        **_hints,
    }

    # Method 1: Direct manual TID init with auth cookies (most reliable)
    try:
        if USING_SHIM:
            raise RuntimeError("x_session 사용 중 — 수동 초기화 건너뜀")
        import bs4, hashlib, math, random as rand_mod, base64 as b64_mod
        from functools import reduce

        # Fetch x.com with auth cookies (avoids login redirect)
        home_headers = {**ct_headers, 'Cookie': f'auth_token={auth_token}; ct0={ct0}'}
        home_resp = await client.http.request('GET', 'https://x.com', headers=home_headers, follow_redirects=True)
        home_html = home_resp.text
        home_soup = bs4.BeautifulSoup(home_html, 'html.parser')

        # Get verification key
        meta = home_soup.select_one("[name='twitter-site-verification']")
        if not meta:
            raise Exception("No twitter-site-verification meta tag")
        key = meta.get("content")

        # Find ondemand.s chunk: pattern is chunkId:"ondemand.s"
        chunk_match = re.search(r'(\d+)\s*:\s*["\']ondemand\.s["\']', home_html)
        if not chunk_match:
            raise Exception("No ondemand.s chunk ID found")
        chunk_id = chunk_match.group(1)

        # Find hash for this chunk ID
        hash_matches = re.findall(rf'{chunk_id}\s*:\s*["\']([\w]+)["\']', home_html)
        od_hash = None
        for h in hash_matches:
            if h != 'ondemand.s' and len(h) >= 5:
                od_hash = h
                break
        if not od_hash:
            raise Exception(f"No hash found for chunk {chunk_id}")

        # Fetch ondemand.s JS file
        od_url = f"https://abs.twimg.com/responsive-web/client-web/ondemand.s.{od_hash}a.js"
        od_resp = await client.http.request('GET', od_url, headers=ct_headers)
        if od_resp.status_code != 200:
            raise Exception(f"ondemand.s fetch failed: {od_resp.status_code}")

        # Extract KEY_BYTE indices
        indices_regex = re.compile(r'(\(\w{1}\[(\d{1,2})\],\s*16\))+', re.VERBOSE | re.MULTILINE)
        idx_matches = list(indices_regex.finditer(od_resp.text))
        if not idx_matches:
            raise Exception("No KEY_BYTE indices in ondemand.s")
        key_byte_indices = [int(m.group(2)) for m in idx_matches]

        # Set up ClientTransaction manually
        ct = client.client_transaction
        ct.home_page_response = home_soup
        ct.DEFAULT_ROW_INDEX = key_byte_indices[0]
        ct.DEFAULT_KEY_BYTES_INDICES = key_byte_indices[1:]
        ct.key = key
        ct.key_bytes = ct.get_key_bytes(key=key)
        ct.animation_key = ct.get_animation_key(key_bytes=ct.key_bytes, response=home_soup)

        deduplicate_cookies(client)
        tid_ok = True
        print(json.dumps({"info": f"TID manual init OK (indices={key_byte_indices}, hash={od_hash})"}), flush=True)

    except Exception as e1:
        print(json.dumps({"info": f"TID manual init failed: {e1}"}), flush=True)

        # Method 2: Try twikit's built-in init
        try:
            _r = await client.client_transaction.init(client.http, ct_headers)
            if _r is False:
                raise RuntimeError(getattr(client.client_transaction, "reason", "init 실패"))
            deduplicate_cookies(client)
            tid_ok = True
            print(json.dumps({"info": "TID init OK: " + str(getattr(client.client_transaction, "reason", "twikit"))}), flush=True)
        except Exception as e2:
            print(json.dumps({"info": f"TID twikit init also failed: {e2}"}), flush=True)

    # 무효 TID 는 SearchTimeline 에서 빈 본문 404 로만 돌아와 해시 만료와 구별되지 않는다(RSSHub #23408)
    if not tid_ok:
        if not USING_SHIM:
            client.client_transaction.generate_transaction_id = lambda *a, **k: ""
        print(json.dumps({"info": "TID 초기화 실패 — 서명 없이 보냅니다. SearchTimeline 404 는 해시 문제가 아닙니다"}), flush=True)

    # Re-apply cached hashes AFTER TID init (in case twikit reset them)
    cached2 = _load_cached_hashes()
    if cached2:
        apply_hashes_to_endpoint(Endpoint, cached2)

    # Signal ready with debug info
    print(json.dumps({"status": "ready", "tid": tid_ok,
                      "tid_reason": str(getattr(client.client_transaction, "reason", "")),
                      "_st_url": Endpoint.SEARCH_TIMELINE[-40:]}), flush=True)

    # 재초기화가 성공하면 위에서 건 '서명 없이 보내기' 를 걷는다(twikit 으로 물러섰을 때만 — 윈도우와 같다).
    #   안 그러면 '성공' 이라 찍고도 그 뒤 요청이 계속 서명 없이 나가 SearchTimeline 404 가 이어졌다.
    def _tid_reinit_ok():
        if USING_SHIM:
            return
        _c = client.client_transaction
        _c.__dict__.pop("generate_transaction_id", None)   # 클래스의 진짜 서명기로 돌아간다
        _c.ready = True
        _c.reason = "twikit 재초기화 성공"

    # Track 404 for auto-repair
    repair_count = 0          # How many repairs attempted
    last_repair_time = 0      # Timestamp of last repair
    consecutive_404 = 0       # Track consecutive 404s

    # Process commands from stdin
    #   ★ Windows 는 기본 이벤트 루프가 Proactor 인데, 거기서는 connect_read_pipe 로
    #     stdin 을 읽을 수 없다. 붙이면 _ProactorReadPipeTransport._loop_reading 에서
    #     예외가 나고 이후 명령을 하나도 받지 못한다. 앱은 ready 만 받아 놓고 30초 뒤
    #     "Daemon: incomplete response (0 bytes, 30000ms)" 로 포기한다 — 윈도우에서 실측.
    #     맥은 기존 경로를 그대로 탄다. 두 트리를 같은 모양으로 둔다.
    loop = asyncio.get_event_loop()
    if sys.platform == "win32":
        async def _read_line():
            return await loop.run_in_executor(None, sys.stdin.buffer.readline)
    else:
        reader = asyncio.StreamReader()
        protocol = asyncio.StreamReaderProtocol(reader)
        await loop.connect_read_pipe(lambda: protocol, sys.stdin)

        async def _read_line():
            return await reader.readline()

    while True:
        _rid = None          # 이 명령의 번호(앱이 붙였으면) — 앞 명령의 번호가 남지 않게 매번 비운다
        try:
            line = await _read_line()
            if not line:
                break  # EOF
            line = line.decode('utf-8').strip()
            if not line:
                continue

            args = json.loads(line)
            _rid = args.pop("_rid", None) if isinstance(args, dict) else None
            action = args.get("action", "")

            if action == "quit":
                _reply({"status": "bye"}, _rid)
                break

            if action == "ping":
                _reply({"status": "pong"}, _rid)
                continue

            if action == "refresh_cookies":
                # Auto-refresh: extract fresh cookies from Chrome
                print(json.dumps({"info": "Attempting to refresh cookies from Chrome..."}), flush=True)
                new_auth, new_ct0 = extract_chrome_twitter_cookies(auth_token, expected_uid)
                if new_auth and new_ct0:
                    auth_token = new_auth
                    ct0 = new_ct0
                    client.set_cookies({"auth_token": auth_token, "ct0": ct0})
                    deduplicate_cookies(client)
                    # 토큰은 앞자리도 내보내지 않는다(로그에 남는다) — 길이만
                    _reply({"status": "ok", "info": "Cookies refreshed from Chrome",
                            "auth_token_len": len(auth_token), "ct0_len": len(ct0)}, _rid)
                else:
                    _reply({"status": "error", "error": "Failed to extract cookies from Chrome"
                            + (" — " + LAST_COOKIE_ERROR if LAST_COOKIE_ERROR else "")}, _rid)
                continue

            if action == "update_endpoints":
                # Manual trigger to refresh hashes (called by C++ on 404)
                # ★ 12시간 안에 긁은 캐시가 있으면 긁지 않는다 — 캐시는 기동 때 이미 Endpoint 에 얹혔다. 앱의 404 수리 길은 force 를 싣는다(r56)
                # ★ 바로 앞이 404 였으면 캐시로 답하지 않고 긁는다(r56 검토 DPC-1) — 고침 꾸러미로 이 데몬을 받은 옛 앱(r51~r55)은
                #   force 를 싣지 않는다. 옛 앱의 사용자 조회 · 좋아요 · 북마크는 404 면 update_endpoints 한 번 + 다시 묻기 한 번이 전부라,
                #   캐시로 답하면 해시가 정말 바뀐 뒤 12시간까지 그 셋이 막혔다. 기동 때의 부름(404 없음)만 캐시를 쓴다.
                _cached_reply = _cached_hash_reply(args, consecutive_404)
                if _cached_reply is not None:
                    _age = _hash_cache_age() or 0
                    print(json.dumps({"info": "GraphQL 해시: %d시간 전에 받은 캐시를 씁니다(12시간마다 · 404 면 다시 받습니다)"
                                              % int(_age // 3600)}), flush=True)
                    _reply(_cached_reply, _rid)
                    continue
                print(json.dumps({"info": "Manual hash refresh requested..."}), flush=True)
                updated, err = await auto_repair_hashes(
                    client, Endpoint, auth_token=auth_token, ct0=ct0
                )
                if err:
                    _reply({"status": "error", "error": err}, _rid)
                else:
                    # Re-init TID after hash update
                    try:
                        ct_headers = {
                            'Accept-Language': 'en-US,en;q=0.9',
                            'Cache-Control': 'no-cache',
                            'Referer': 'https://x.com',
                            'User-Agent': client._user_agent,
                            **_hints,
                        }
                        if await client.client_transaction.init(client.http, ct_headers) is not False:
                            _tid_reinit_ok()
                        deduplicate_cookies(client)
                    except Exception:
                        pass
                    repair_count = 0
                    consecutive_404 = 0
                    last_repair_time = time.time()
                    _reply({"status": "ok", "updated": updated or []}, _rid)
                continue

            deduplicate_cookies(client)  # Clean before each API call
            result = await handle_command(client, args, Endpoint, FEATURES, USER_FEATURES, flatten_params)

            # Auto-repair: if 404, try fetching new hashes and retry
            # Allow repair if: never repaired, or >60 seconds since last repair
            if result.get("status") == 404 and result.get("_cause") == "no_tid":
                # 서명기(x-client-transaction-id)가 준비되지 않아 난 404 — 해시 수리·캐시 삭제를 하지 않는다.
                #   앱의 검색 루프는 ERROR 뒤 5초마다 같은 검색을 끝없이 다시 보낸다. 그래서 재초기화
                #   (x.com/home + ondemand.s)는 60초에 한 번만 한다.
                print(json.dumps({"info": "404 (TID 없음: %s) — 해시 수리 건너뜀" % result.get("_tid_reason", "")}), flush=True)
                _ct = client.client_transaction
                if time.time() - getattr(_ct, "_hn_reinit_at", 0) > 60:
                    try:
                        _ct._hn_reinit_at = time.time()
                        if await _ct.init(client.http, ct_headers) is not False:
                            _tid_reinit_ok()
                            deduplicate_cookies(client)
                            print(json.dumps({"info": "TID 재초기화 성공 — 다시 보냅니다"}), flush=True)
                            result = await handle_command(client, args, Endpoint, FEATURES, USER_FEATURES, flatten_params)
                    except Exception as _re:
                        print(json.dumps({"info": f"TID 재초기화 실패: {_re}"}), flush=True)
            elif result.get("status") == 404:
                consecutive_404 += 1
                can_repair = (
                    repair_count < 5 and  # Max 5 auto-repairs per session
                    (time.time() - last_repair_time) > 60  # At least 60s between repairs
                )

                if can_repair:
                    repair_count += 1
                    last_repair_time = time.time()
                    print(json.dumps({"info": f"HTTP 404 detected (attempt {repair_count}/5), auto-repairing GraphQL hashes..."}), flush=True)

                    # 캐시는 지우지 않는다. auto_repair_hashes 는 캐시를 읽지 않고, 성공하면 덮어쓴다.
                    #   지우면 갱신이 실패했을 때 마지막으로 맞던 해시만 잃는다.
                    #   게다가 파일이 없으면 다음 기동 때 옛 Miyo 자리의 캐시를 끌어온다(파일 머리 50-55행).
                    #   (tid_ok 가 참이면 main() 안의 'import base64, os' 때문에 이 삭제가 UnboundLocalError 로
                    #    조용히 죽어 있었다. 그 import 를 없애면 살아나므로 같이 뺀다)

                    updated, err = await auto_repair_hashes(
                        client, Endpoint, auth_token=auth_token, ct0=ct0
                    )
                    if err:
                        print(json.dumps({"info": f"Auto-repair failed: {err}"}), flush=True)
                    else:
                        names = updated or []
                        print(json.dumps({"info": f"Hashes updated: {','.join(names) if names else 'none changed'}. Retrying..."}), flush=True)
                        # Re-init TID with new endpoints
                        try:
                            ct_headers = {
                                'Accept-Language': 'en-US,en;q=0.9',
                                'Cache-Control': 'no-cache',
                                'Referer': 'https://x.com',
                                'User-Agent': client._user_agent,
                                **_hints,
                            }
                            if await client.client_transaction.init(client.http, ct_headers) is not False:
                                _tid_reinit_ok()
                            deduplicate_cookies(client)
                        except Exception:
                            pass
                        # Retry the command
                        result = await handle_command(client, args, Endpoint, FEATURES, USER_FEATURES, flatten_params)
                        if result.get("status") == 200:
                            consecutive_404 = 0  # Success! Reset counter
            elif result.get("status") in (401, 403):
                # Auth error — Chrome 의 같은 계정 로그인으로 쿠키를 갈아 본다(같은 계정임이 확인될 때만, r54)
                _st = result.get("status")
                _codes = _x_error_codes(result.get("body"))
                if _codes & {64, 326}:
                    # 잠김(326) · 정지(64) — 쿠키를 바꿔도 소용없고, 다른 계정의 로그인으로 바꾸면 안 된다
                    print(json.dumps({"info": "HTTP %d — 계정이 잠겼거나 정지됐습니다(오류 %s) — 쿠키 자동 갱신은 하지 않습니다"
                                      % (_st, ', '.join(str(c) for c in sorted(_codes)))}), flush=True)
                elif _st == 401 and not expected_uid:
                    # 지금 로그인(같은 auth_token)은 방금 거절됐다 — 사용자 번호를 모르면 다른 로그인은 받을 수 없으니 키체인을 열 까닭이 없다
                    print(json.dumps({"info": "HTTP 401 — 이 계정의 X 사용자 번호를 몰라 Chrome 쿠키 자동 갱신은 하지 않습니다"
                                      "(설정 → 계정 토큰 자동 추출 → Twitter 로 넣은 계정만 됩니다)"}), flush=True)
                else:
                    print(json.dumps({"info": f"HTTP {_st} auth error, attempting cookie refresh from Chrome (same X account only)..."}), flush=True)
                    new_auth, new_ct0 = extract_chrome_twitter_cookies(auth_token, expected_uid)
                    if new_auth and new_ct0 and (new_auth != auth_token or new_ct0 != ct0):
                        auth_token = new_auth
                        ct0 = new_ct0
                        client.set_cookies({"auth_token": auth_token, "ct0": ct0})
                        deduplicate_cookies(client)
                        # 토큰 앞자리도 찍지 않는다 — 이 줄은 앱 로그로 그대로 흘러간다.
                        print(json.dumps({"info": "Cookies refreshed (same X account)! Retrying..."}), flush=True)
                        # Retry the command with fresh cookies
                        result = await handle_command(client, args, Endpoint, FEATURES, USER_FEATURES, flatten_params)
                    else:
                        print(json.dumps({"info": "Cookie refresh failed or cookies unchanged"
                                          + (" — " + LAST_COOKIE_ERROR if LAST_COOKIE_ERROR else "")}), flush=True)
                consecutive_404 = 0
            else:
                consecutive_404 = 0  # Non-404 response resets counter

            _reply(result, _rid)

        except json.JSONDecodeError as e:
            print(json.dumps({"error": f"Invalid JSON: {str(e)}"}), flush=True)
        except Exception as e:
            _reply({"error": f"Command error: {str(e)}"}, _rid)


def _v1_to_graphql_search(v1_data):
    """Convert v1.1 search/tweets.json response to GraphQL SearchTimeline format.
    This allows the C++ parser to handle v1.1 fallback data seamlessly.
    """
    entries = []
    statuses = v1_data.get("statuses", [])
    for tweet in statuses:
        tid = tweet.get("id_str", "")
        user = tweet.get("user", {})
        # Build GraphQL-compatible tweetResult
        legacy = dict(tweet)
        legacy.pop("user", None)  # user goes in core, not legacy
        # v1.1 uses full_text, GraphQL also uses full_text
        tweet_result = {
            "__typename": "Tweet",
            "rest_id": tid,
            "legacy": legacy,
            "core": {
                "user_results": {
                    "result": {
                        "__typename": "User",
                        "rest_id": user.get("id_str", ""),
                        "legacy": user,
                    }
                }
            },
        }
        entry = {
            "entryId": f"tweet-{tid}",
            "content": {
                "entryType": "TimelineTimelineItem",
                "itemContent": {
                    "tweet_results": {"result": tweet_result},
                    "tweetDisplayType": "Tweet",
                },
            },
        }
        entries.append(entry)

    # Cursor from search_metadata
    meta = v1_data.get("search_metadata", {})
    next_results = meta.get("next_results", "")
    if next_results:
        import urllib.parse
        qs = urllib.parse.parse_qs(next_results.lstrip("?"))
        max_id = qs.get("max_id", [""])[0]
        if max_id:
            entries.append({
                "entryId": "cursor-bottom-0",
                "content": {"entryType": "TimelineTimelineCursor", "value": max_id, "cursorType": "Bottom"},
            })

    return {
        "data": {
            "search_by_raw_query": {
                "search_timeline": {
                    "timeline": {
                        "instructions": [
                            {"type": "TimelineAddEntries", "entries": entries}
                        ]
                    }
                }
            }
        }
    }


def _adaptive_to_graphql_search(adaptive_data):
    """Convert adaptive search response to GraphQL SearchTimeline format."""
    entries = []
    tweets_dict = adaptive_data.get("globalObjects", {}).get("tweets", {})
    users_dict = adaptive_data.get("globalObjects", {}).get("users", {})

    # Get timeline order from instructions
    timeline = adaptive_data.get("timeline", {})
    instructions = timeline.get("instructions", [])
    cursor_bottom = ""

    for inst in instructions:
        if "addEntries" in inst:
            for entry in inst["addEntries"].get("entries", []):
                eid = entry.get("entryId", "")
                if eid.startswith("tweet-") or eid.startswith("sq-I-t-"):
                    # Extract tweet ID
                    content = entry.get("content", {})
                    item = content.get("item", content.get("content", {}))
                    tweet_info = item.get("content", item).get("tweet", {})
                    tid = tweet_info.get("id", eid.split("-")[-1])

                    tweet = tweets_dict.get(tid, {})
                    if not tweet:
                        continue
                    uid = tweet.get("user_id_str", "")
                    user = users_dict.get(uid, {})

                    tweet_result = {
                        "__typename": "Tweet",
                        "rest_id": tid,
                        "legacy": tweet,
                        "core": {
                            "user_results": {
                                "result": {
                                    "__typename": "User",
                                    "rest_id": uid,
                                    "legacy": user,
                                }
                            }
                        },
                    }
                    entries.append({
                        "entryId": f"tweet-{tid}",
                        "content": {
                            "entryType": "TimelineTimelineItem",
                            "itemContent": {
                                "tweet_results": {"result": tweet_result},
                                "tweetDisplayType": "Tweet",
                            },
                        },
                    })
                elif "cursor-bottom" in eid:
                    cursor_val = entry.get("content", {}).get("operation", {}).get("cursor", {}).get("value", "")
                    if not cursor_val:
                        cursor_val = entry.get("content", {}).get("value", "")
                    if cursor_val:
                        cursor_bottom = cursor_val

    if cursor_bottom:
        entries.append({
            "entryId": "cursor-bottom-0",
            "content": {"entryType": "TimelineTimelineCursor", "value": cursor_bottom, "cursorType": "Bottom"},
        })

    return {
        "data": {
            "search_by_raw_query": {
                "search_timeline": {
                    "timeline": {
                        "instructions": [
                            {"type": "TimelineAddEntries", "entries": entries}
                        ]
                    }
                }
            }
        }
    }


def _dig(obj, *keys):
    """중첩 dict 를 안전하게 따라간다 — 중간이 dict 가 아니면 None."""
    for k in keys:
        if not isinstance(obj, dict):
            return None
        obj = obj.get(k)
    return obj


def _normalize_user_result(u):
    """UserByScreenName 의 data.user.result 를 옛 legacy 모양으로 채운다(제자리 수정).

    X 는 2026-08 초부터 user 객체에서 legacy 를 통째로 뺐다(키 자체가 없다).
    필드는 core / profile_bio / relationship_counts / tweet_counts / action_counts /
    banner / avatar / location / privacy / verification / website / pinned_items 로 흩어졌다.
    C++(TwitterCollector)은 legacy 를 읽으므로 여기서 되돌려 채운다.

    ★ legacy 에 이미 값이 있으면 건드리지 않는다 — 옛 모양 응답은 그대로 지나간다.
      None·빈 문자열만 '없음' 으로 본다(0 과 False 는 값이다).
    """
    if not isinstance(u, dict):
        return u
    leg = u.get("legacy")
    if not isinstance(leg, dict):
        leg = {}  # 없음(2026-08~) 또는 null

    def fill(key, value):
        if value is None or value == "":
            return
        # 숫자가 문자열로 오는 변종 대비 — C++ toInt() 는 문자열이면 0 이 된다
        if key.endswith("_count") and isinstance(value, str) and value.isdigit():
            value = int(value)
        cur = leg.get(key)
        if cur is None or cur == "":
            leg[key] = value

    # 이름·아이디·가입일 — 2025 부터 core. 더 옛 변종 core.user_results.result.legacy 도 본다.
    old_core_leg = _dig(u, "core", "user_results", "result", "legacy")
    for k in ("name", "screen_name", "created_at"):
        fill(k, _dig(u, "core", k))
        fill(k, _dig(old_core_leg, k))
    # 소개글 + 링크 엔티티(description.urls / url.urls)
    fill("description", _dig(u, "profile_bio", "description"))
    ents = _dig(u, "profile_bio", "entities")
    if isinstance(ents, dict) and not leg.get("entities"):
        leg["entities"] = ents
    # 위치 — {"location": "..."} 또는 문자열
    loc = u.get("location")
    fill("location", loc.get("location") if isinstance(loc, dict) else loc)
    # 웹사이트 — 옛 legacy.url 과 같은 t.co 주소(펼친 주소는 entities.url.urls[0].expanded_url)
    fill("url", _dig(u, "website", "url"))
    urls = _dig(leg, "entities", "url", "urls")
    if isinstance(urls, list) and urls and isinstance(urls[0], dict):
        fill("url", urls[0].get("url"))
    # 비공개·인증
    fill("protected", _dig(u, "privacy", "protected"))
    fill("verified", _dig(u, "verification", "verified"))
    fill("verified_type", _dig(u, "verification", "verified_type"))
    # 숫자
    fill("followers_count", _dig(u, "relationship_counts", "followers"))
    fill("friends_count", _dig(u, "relationship_counts", "following"))
    fill("statuses_count", _dig(u, "tweet_counts", "tweets"))
    fill("media_count", _dig(u, "tweet_counts", "media_tweets"))
    fill("favourites_count", _dig(u, "action_counts", "favorites_count"))
    # 이미지
    fill("profile_banner_url", _dig(u, "banner", "image_url"))
    av = u.get("avatar")
    if isinstance(av, dict):
        fill("profile_image_url_https", av.get("image_url") or av.get("url"))
    elif isinstance(av, str):
        fill("profile_image_url_https", av)
    # 기타 — 옛 legacy 에 있던 것
    fill("pinned_tweet_ids_str", _dig(u, "pinned_items", "tweet_ids_str"))
    fill("possibly_sensitive", u.get("possibly_sensitive"))

    if leg:
        u["legacy"] = leg
    return u


async def handle_command(client, args, Endpoint, FEATURES, USER_FEATURES, flatten_params):
    action = args["action"]

    try:
        if action == "user_by_screen_name":
            variables = {
                "screen_name": args["screen_name"],
                "withSafetyModeUserFields": False,
            }
            data, response = await _gql_request(client, Endpoint.USER_BY_SCREEN_NAME, variables, USER_FEATURES,
                                                flatten_params, field_toggles={"withAuxiliaryUserLabels": False})
            # Normalize response: inject name/profile_image into legacy for backward compat
            if response.status_code == 200 and isinstance(data, dict):
                try:
                    u = data.get("data", {}).get("user", {}).get("result", {})
                    # X 는 2026-08 초부터 user 에서 legacy 를 통째로 뺐다 — 새 자리에서
                    #   legacy 의 '없는 키만' 채운다(옛 모양은 그대로). 아래 옛 보정은 그 뒤 no-op.
                    _normalize_user_result(u)
                    leg = u.get("legacy", {})
                    # If name is missing from legacy, try to get from core or top-level
                    if not leg.get("name"):
                        core = u.get("core", {})
                        if isinstance(core, dict):
                            # core might have user_results.result.legacy.name
                            core_name = core.get("user_results", {}).get("result", {}).get("legacy", {}).get("name", "")
                            if not core_name:
                                core_name = core.get("name", "")
                            if core_name:
                                leg["name"] = core_name
                                data["data"]["user"]["result"]["legacy"] = leg
                    # If screen_name is missing from legacy
                    if not leg.get("screen_name"):
                        core = u.get("core", {})
                        if isinstance(core, dict):
                            core_sn = core.get("user_results", {}).get("result", {}).get("legacy", {}).get("screen_name", "")
                            if core_sn:
                                leg["screen_name"] = core_sn
                                data["data"]["user"]["result"]["legacy"] = leg
                    # If profile_image_url_https is missing, try avatar field
                    if not leg.get("profile_image_url_https"):
                        avatar = u.get("avatar", {})
                        if isinstance(avatar, dict):
                            img_url = avatar.get("image_url", "") or avatar.get("url", "")
                            if not img_url and isinstance(avatar, str):
                                img_url = avatar
                        elif isinstance(avatar, str):
                            img_url = avatar
                        else:
                            img_url = ""
                        if img_url:
                            leg["profile_image_url_https"] = img_url
                            data["data"]["user"]["result"]["legacy"] = leg
                except Exception:
                    pass
            return _api_reply(response, data)

        elif action == "user_tweets":
            variables = {
                "userId": args["user_id"],
                "count": args.get("count", 40),
                "includePromotedContent": True,
                "withQuickPromoteEligibilityTweetFields": True,
                "withVoice": True,
                "withV2Timeline": True,
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]

            data, response = await _gql_request(client, Endpoint.USER_TWEETS, variables, FEATURES, flatten_params)
            return _api_reply(response, data)

        elif action == "search":
            variables = {
                "rawQuery": args["query"],
                "count": args.get("count", 40),
                "querySource": "typed_query",
                "product": "Latest",
                # X 웹 앱이 2026-11 에 싣는 값 그대로(실측) — 빠지면 언젠가 400 이 된다
                "withGrokTranslatedBio": False,
                "withQuickPromoteEligibilityTweetFields": False,
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]

            data, response = await _gql_request(client, Endpoint.SEARCH_TIMELINE, variables, FEATURES, flatten_params)

            # 404 의 실제 원인은 대개 x-client-transaction-id 없음/무효(RSSHub #23408, 2026-09-27)
            if response.status_code == 404 and not getattr(client.client_transaction, "ready", True):
                return dict(_api_reply(response, data), _cause="no_tid",
                            _tid_reason=str(getattr(client.client_transaction, "reason", "")))
            # 1.1/search/tweets.json · 2/search/adaptive.json 은 웹이 쓰지 않는 끝점 — 기본 꺼 둠
            if response.status_code == 404 and os.environ.get("HANISHIKI_LEGACY_SEARCH") == "1":
                try:
                    v1_params = {
                        "q": args["query"],
                        "count": str(args.get("count", 40)),
                        "result_type": "recent",
                        "tweet_mode": "extended",
                    }
                    if args.get("cursor"):
                        v1_params["max_id"] = args["cursor"]
                    v1_url = "https://x.com/i/api/1.1/search/tweets.json"
                    v1_data, v1_resp = await client.request(
                        'GET', v1_url,
                        params=v1_params, headers=client._base_headers,
                        raise_exception=False
                    )
                    if v1_resp.status_code == 200 and isinstance(v1_data, dict):
                        converted = _v1_to_graphql_search(v1_data)
                        return {"status": 200, "body": converted, "_source": "v1.1"}
                except Exception as e:
                    print(json.dumps({"info": f"v1.1 search fallback failed: {e}"}), flush=True)

                # Fallback 2: try adaptive search (older endpoint)
                try:
                    adaptive_params = {
                        "q": args["query"],
                        "count": str(args.get("count", 40)),
                        "query_source": "typed_query",
                        "pc": "1",
                        "spelling_corrections": "1",
                    }
                    if args.get("cursor"):
                        adaptive_params["cursor"] = args["cursor"]
                    adaptive_url = "https://x.com/i/api/2/search/adaptive.json"
                    ad_data, ad_resp = await client.request(
                        'GET', adaptive_url,
                        params=adaptive_params, headers=client._base_headers,
                        raise_exception=False
                    )
                    if ad_resp.status_code == 200 and isinstance(ad_data, dict):
                        converted = _adaptive_to_graphql_search(ad_data)
                        return {"status": 200, "body": converted, "_source": "adaptive"}
                except Exception as e:
                    print(json.dumps({"info": f"adaptive search fallback failed: {e}"}), flush=True)

            return _api_reply(response, data)

        elif action == "likes":
            variables = {
                "userId": args["user_id"],
                "count": args.get("count", 40),
                "includePromotedContent": True,
                "withQuickPromoteEligibilityTweetFields": True,
                "withVoice": True,
                "withV2Timeline": True,
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]

            data, response = await _gql_request(client, Endpoint.USER_LIKES, variables, FEATURES, flatten_params)
            return _api_reply(response, data)

        elif action == "bookmarks":
            variables = {
                "count": args.get("count", 40),
                "includePromotedContent": True,
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]

            bk_features = dict(FEATURES)
            bk_features["graphql_timeline_v2_bookmark_timeline"] = True

            data, response = await _gql_request(client, Endpoint.BOOKMARKS, variables, bk_features, flatten_params)
            return _api_reply(response, data)

        elif action == "tweet_detail":
            variables = {
                "focalTweetId": args["tweet_id"],
                "with_rux_injections": False,
                "rankingMode": "Relevance",
                "includePromotedContent": True,
                "withCommunity": True,
                "withQuickPromoteEligibilityTweetFields": True,
                "withBirdwatchNotes": True,
                "withVoice": True,
                "withV2Timeline": True,
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]
                variables["referrer"] = "tweet"

            # ★ withArticlePlainText 를 켠다(r56) — X 의 글(Article) 본문을 평문으로도 받아 앱이 엑셀에 담는다(본문 읽기는 C++ 쪽)
            data, response = await _gql_request(
                client, Endpoint.TWEET_DETAIL, variables, FEATURES, flatten_params,
                field_toggles={"withArticleRichContentState": True, "withArticlePlainText": True,
                               "withGrokAnalyze": False, "withDisallowedReplyControls": False})
            return _api_reply(response, data)

        elif action in ("followers", "following"):
            variables = {
                "userId": args["user_id"],
                "count": args.get("count", 40),
                "includePromotedContent": False,
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]

            endpoint = Endpoint.FOLLOWERS if action == "followers" else Endpoint.FOLLOWING
            data, response = await _gql_request(client, endpoint, variables, FEATURES, flatten_params)

            # 서명기가 준비되지 않은 404 는 해시 문제가 아니다 — 해시를 다시 받지 않고 원인을 붙여 돌려준다
            #   (메인 루프가 _cause == "no_tid" 를 보고 TID 만 다시 세운다)
            if response.status_code == 404 and not getattr(client.client_transaction, "ready", True):
                return dict(_api_reply(response, data), _cause="no_tid",
                            _tid_reason=str(getattr(client.client_transaction, "reason", "")))
            # 404 = endpoint hash 만료 → 즉시 fresh hash fetch + retry (메인 루프 throttle 우회)
            if response.status_code == 404:
                try:
                    # client cookies에서 auth 토큰 추출
                    cks = {}
                    try:
                        for c in client.http.cookies.jar:
                            cks[c.name] = c.value
                    except Exception:
                        try:
                            cks = dict(client.http.cookies)
                        except Exception:
                            pass
                    auth_tk = args.get("auth_token") or cks.get("auth_token")
                    ct0_tk = args.get("ct0") or cks.get("ct0")
                    print(json.dumps({"info": f"{action} 404 → fresh hash fetch (auth={'있음' if auth_tk else '없음'})"}), flush=True)
                    updated, err = await auto_repair_hashes(client, Endpoint, auth_token=auth_tk, ct0=ct0_tk)
                    if err:
                        print(json.dumps({"info": f"{action} hash fetch 실패: {err}"}), flush=True)
                    elif not updated:
                        # ★ 빈 배열이면 아무것도 안 바뀐 것이다. 그대로 다시 물으면 같은 404 가
                        #   온다 — '갱신됨' 이라고 적으면 안 된다. 예전에는 그렇게 적고 한 번 더
                        #   똑같이 물어본 뒤 "API error: 404" 로 끝났다(윈도우 실측: _0_zero_h).
                        print(json.dumps({"info": f"{action} hash 갱신 결과가 비어 있음 — 새 해시를 못 받았습니다. "
                                                  f"이 계정이 그 목록을 못 보는 상태이거나(비공개·차단·미로그인), "
                                                  f"X 가 끝점을 바꿨을 수 있습니다. 다시 묻지 않고 끝냅니다."}), flush=True)
                    else:
                        endpoint = Endpoint.FOLLOWERS if action == "followers" else Endpoint.FOLLOWING
                        print(json.dumps({"info": f"hash 갱신됨: {updated} → retry"}), flush=True)
                        data, response = await _gql_request(client, endpoint, variables, FEATURES, flatten_params)
                except Exception as e:
                    print(json.dumps({"info": f"{action} hash repair 예외: {e}"}), flush=True)

            return _api_reply(response, data)

        elif action == "highlights":
            variables = {
                "userId": args["user_id"],
                "count": args.get("count", 40),
                "includePromotedContent": False,
                "withVoice": True,
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]
            data, response = await _gql_request(client, Endpoint.USER_HIGHLIGHTS_TWEETS, variables, FEATURES, flatten_params)
            return _api_reply(response, data)

        elif action in ("favoriters", "retweeters"):
            variables = {
                "tweetId": args["tweet_id"],
                "count": args.get("count", 40),
                "includePromotedContent": True,
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]
            endpoint = Endpoint.FAVORITERS if action == "favoriters" else Endpoint.RETWEETERS
            data, response = await _gql_request(client, endpoint, variables, FEATURES, flatten_params)
            return _api_reply(response, data)

        elif action == "list_tweets":
            variables = {
                "listId": args["list_id"],
                "count": args.get("count", 40),
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]
            data, response = await _gql_request(client, Endpoint.LIST_LATEST_TWEETS_TIMELINE, variables, FEATURES, flatten_params)
            return _api_reply(response, data)

        elif action == "community_tweets":
            variables = {
                "communityId": args["community_id"],
                "count": args.get("count", 40),
                "withCommunity": True,
                "rankingMode": args.get("ranking", "Relevance"),  # or "Recency"
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]
            data, response = await _gql_request(client, Endpoint.COMMUNITY_TWEETS_TIMELINE, variables, FEATURES, flatten_params)
            return _api_reply(response, data)

        elif action == "account_status":
            # ★ 이 계정이 X 에서 어떤 상태인가(r56) — 정지된 계정은 검색만 막히고(200 + 'DenySuspendedUsers') 타임라인 · 사용자 조회는
            #   그대로 된다(2026-11-27 실측). 앱이 판마다 계정당 한 번 묻고, 정지된 계정으로는 검색하지 않는다.
            #   돌려주는 것은 상태 칸뿐이다 — screen_name · 메일 · 토큰은 싣지 않는다.
            try:
                data, response = await client.request(
                    'GET', "https://x.com/i/api/1.1/account/verify_credentials.json",
                    params={"skip_status": "true", "include_email": "false"}, headers=client._base_headers,
                    raise_exception=False
                )
            except Exception as e:
                if type(e).__name__ == "AccountSuspended":   # twikit 으로 물러섰을 때 — 오류 64/37 을 예외로 던진다
                    return {"status": 200, "suspended": True, "protected": False, "needs_phone_verification": False,
                            "locked": False, "error_code": 64, "user_id": ""}
                if type(e).__name__ == "AccountLocked":
                    return {"status": 200, "suspended": False, "protected": False, "needs_phone_verification": False,
                            "locked": True, "error_code": 326, "user_id": ""}
                raise
            return _account_status_reply(response, data)

        else:
            return {"error": f"Unknown action: {action}"}

    except Exception as e:
        import traceback
        err_msg = str(e) or f"{type(e).__name__}: {traceback.format_exc().splitlines()[-1]}"
        return {"error": err_msg, "traceback": traceback.format_exc()[-500:]}


if __name__ == "__main__":
    # Ignore SIGPIPE
    #   ★ SIGPIPE 는 POSIX 신호라 Windows 에는 없다. 그대로 부르면 시작하자마자
    #     AttributeError 로 죽고, 앱은 "Daemon: ready signal not received" 뒤
    #     TID 폴백으로 넘어간다 — 즉 윈도우에서 twikit 데몬이 한 번도 뜬 적이 없었다.
    #     맥에서는 hasattr 이 참이라 동작이 그대로다. 두 트리를 같은 모양으로 둔다.
    if hasattr(signal, "SIGPIPE"):
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    asyncio.run(main())
