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

# ★ 데이터 폴더는 앱이 환경변수로 알려 준다 (Common.cpp bundledProcessEnv).
#   예전에는 맥 경로 ~/Library/Application Support/カメラ 가 그대로 박혀 있어서,
#   윈도우에서 expanduser("~") 가 C:\Users\<사용자> 로 풀리는 바람에
#   C:\Users\<사용자>\Library\Application Support\カメラ\ 라는 가짜 맥 트리를
#   사용자 홈에 만들었다. 이름도 옛 코드네임이었다.
#   환경변수가 없을 때만 판별해서 각 운영체제의 제자리를 쓴다.
def _default_data_dir():
    if sys.platform == "win32":
        base = os.environ.get("APPDATA") or os.path.expanduser("~")
        return os.path.join(base, "Predormition")
    if sys.platform == "darwin":
        return os.path.join(os.path.expanduser("~"), "Library", "Application Support", "Predormition")
    base = os.environ.get("XDG_DATA_HOME") or os.path.join(os.path.expanduser("~"), ".local", "share")
    return os.path.join(base, "Predormition")


_data_dir = (os.environ.get("PREDORMITION_DATA_DIR")
             or os.environ.get("HANISHIKI_DATA_DIR")
             or _default_data_dir())
_hash_cache_path = os.path.join(_data_dir, "graphql_hashes.json")
try:
    os.makedirs(_data_dir, exist_ok=True)
except Exception:
    pass

# 옛 자리에 쌓여 있던 캐시를 한 번만 이어받는다 (새 자리가 비어 있을 때만).
try:
    if not os.path.exists(_hash_cache_path):
        for _legacy in (
            os.path.join(os.path.expanduser("~"), "Library", "Application Support",
                         "カメラ", "graphql_hashes.json"),
            os.path.join(os.path.expanduser("~"), "Library", "Application Support",
                         "Miyo", "Predormition", "graphql_hashes.json"),
        ):
            if os.path.exists(_legacy):
                import shutil as _sh
                _sh.copy2(_legacy, _hash_cache_path)
                break
except Exception:
    pass


def _load_cached_hashes():
    """Load previously cached hashes from disk."""
    try:
        if os.path.exists(_hash_cache_path):
            with open(_hash_cache_path, encoding='utf-8') as f:
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
        with open(_hash_cache_path, "w", encoding="utf-8") as f:
            json.dump(data, f)
    except Exception:
        pass


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

def extract_chrome_twitter_cookies():
    """Extract auth_token and ct0 from Chrome's cookie database.
    Works on macOS and Windows. Returns (auth_token, ct0) or (None, None).
    """
    auth_token = None
    ct0 = None

    try:
        if platform.system() == "Darwin":
            # macOS: Chrome cookies are encrypted with Keychain key
            import sqlite3, tempfile, shutil

            # Get Chrome encryption key from Keychain
            raw_key = subprocess.check_output([
                'security', 'find-generic-password', '-w',
                '-s', 'Chrome Safe Storage', '-a', 'Chrome'
            ], stderr=subprocess.DEVNULL).strip()

            # Derive AES key using PBKDF2
            import hashlib
            dk = hashlib.pbkdf2_hmac('sha1', raw_key, b'saltysalt', 1003, dklen=16)

            # Copy cookie DB (Chrome locks it)
            cookie_paths = [
                os.path.expanduser("~/Library/Application Support/Google/Chrome/Default/Cookies"),
                os.path.expanduser("~/Library/Application Support/Google/Chrome/Profile 1/Cookies"),
            ]
            db_path = None
            for p in cookie_paths:
                if os.path.exists(p):
                    db_path = p
                    break
            if not db_path:
                return None, None

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
                    # v10 prefix = Chrome encryption
                    if enc_val[:3] == b'v10':
                        from ctypes import cdll, c_buffer, c_int, byref
                        # Use CommonCrypto for AES-CBC decryption
                        lib = cdll.LoadLibrary('/usr/lib/libcommonCrypto.dylib')
                        iv = b' ' * 16
                        encrypted = enc_val[3:]
                        out_buf = c_buffer(len(encrypted) + 16)
                        out_len = c_int(0)
                        # kCCDecrypt=1, kCCAlgorithmAES128=0, kCCOptionPKCS7Padding=1
                        lib.CCCrypt(1, 0, 1, dk, len(dk), iv,
                                    encrypted, len(encrypted),
                                    out_buf, len(out_buf), byref(out_len))
                        decrypted = out_buf.raw[:out_len.value].decode('utf-8', errors='ignore')
                        if name == 'auth_token':
                            auth_token = decrypted
                        elif name == 'ct0':
                            ct0 = decrypted
                except Exception:
                    continue

            conn.close()
            os.unlink(tmp)

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
            # 크롬의 Local State 는 UTF-8 JSON 이다. 시스템 ANSI 로 읽으면 깨진다.
            with open(local_state_path, encoding='utf-8') as f:
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

    return auth_token, ct0


# ── Main daemon ───────────────────────────────────────────────────────────

async def main():
    init_args = json.loads(sys.argv[1])
    auth_token = init_args["auth_token"]
    ct0 = init_args["ct0"]

    # ── 어느 세션 계층을 쓸지 ────────────────────────────────────────────
    #
    # twikit 을 먼저 쓴다. 오래 써서 검증된 쪽이고, 지금 이 기계에서 실제로
    # 수집이 되고 있는 쪽이기 때문이다.
    #
    # x_session(tools/x_session.py)은 우리가 직접 만든 얇은 계층이고, 여기서는
    # '물러설 자리' 로 둔다. 다만 두 자리에서 실제로 넘어간다:
    #   1) twikit 이 아예 없을 때
    #   2) twikit 으로 X-Client-Transaction-Id 서명을 못 만들 때
    #      (twikit 자기 초기화는 지금 X 에서 실패한다 — "Couldn't get KEY_BYTE
    #       indices". 데몬이 손으로 우회해 왔지만 그 우회도 언젠가 안 맞게 된다.)
    #      전에는 그때 무작위 서명으로 넘어갔다. 그건 되는 척하는 상태다 —
    #      X 가 막으면 그제서야 알게 된다. 되는 것이 있으면 그것을 쓰는 게 낫다.
    #   ※ 손으로 넘기고 싶을 때: 환경변수 PREDORMITION_X_SESSION=1
    #     (twikit 이 멀쩡해 보이는데 수집만 이상할 때 갈아 끼워 보는 용도.
    #      이 스위치가 있어야 '물러설 자리' 가 실제로 도는지 확인할 수 있다 —
    #      한 번도 안 도는 대비책은 필요한 날 같이 고장 나 있다.)
    USING_SHIM = False
    _forced = os.environ.get("PREDORMITION_X_SESSION", "").strip() not in ("", "0")
    try:
        if _forced:
            raise ImportError("PREDORMITION_X_SESSION 으로 강제 전환")
        from twikit import Client
        from twikit.client.gql import Endpoint, FEATURES, USER_FEATURES, flatten_params
        print(json.dumps({"info": "세션 계층: twikit"}), flush=True)
    except ImportError as _te:
        try:
            from x_session import Client, Endpoint, FEATURES, USER_FEATURES, flatten_params
            USING_SHIM = True
            # 강제 전환과 '진짜로 twikit 이 없음' 을 구분해서 적는다.
            #   섞어 적으면 로그를 읽는 사람이 없는 고장을 쫓게 된다.
            print(json.dumps({"info": "세션 계층: x_session (환경변수로 강제 전환)"
                              if _forced else f"세션 계층: x_session (twikit 이 없음: {_te})"}),
                  flush=True)
        except Exception as _se:
            print(json.dumps({"error": f"twikit 도 x_session 도 못 불러왔습니다: {_te} / {_se}"}),
                  flush=True)
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

    # ★ x_session 은 쿠키를 도메인까지 지정해 넣으므로 중복이 안 생긴다 —
    #   이 패치는 twikit 을 쓸 때만 필요하다. 필요 없을 때 남의 라이브러리를
    #   건드려 두면 나중에 원인 찾기만 어려워진다.
    if not USING_SHIM:
        httpx.Cookies.get = _safe_cookies_get
        print(json.dumps({"info": "httpx.Cookies.get() patched: duplicate cookies tolerated"}), flush=True)

    # ── In-process source-level fix for twikit bug: 'code' KeyError ──
    # twikit/client/client.py line ~158 accesses response_data['errors'][0]['code']
    # unconditionally. Twitter sometimes returns errors without 'code' (only 'message'
    # or 'extensions.code'). We patch the bundled source file ONCE if needed.
    try:
        if USING_SHIM:
            raise RuntimeError("x_session 을 쓰므로 twikit 소스 패치는 건너뜁니다")
        import twikit.client.client as _twc
        import os as _os
        _twc_path = _twc.__file__
        with open(_twc_path, 'r', encoding='utf-8') as _f:
            _src = _f.read()
        _bad = "error_code = response_data['errors'][0]['code']"
        _good = ("error_code = response_data['errors'][0].get('code') "
                 "or (response_data['errors'][0].get('extensions') or {}).get('code') or 0")
        if _bad in _src and _good not in _src:
            _src = _src.replace(_bad, _good)
            try:
                with open(_twc_path, 'w', encoding='utf-8') as _f:
                    _f.write(_src)
                # Invalidate bytecode cache so the fix loads next import
                _cache_dir = _os.path.join(_os.path.dirname(_twc_path), '__pycache__')
                if _os.path.isdir(_cache_dir):
                    for _fn in _os.listdir(_cache_dir):
                        if _fn.startswith('client.') and _fn.endswith('.pyc'):
                            try: _os.remove(_os.path.join(_cache_dir, _fn))
                            except Exception: pass
                print(json.dumps({"info": "twikit client.py patched: 'code' KeyError fixed (reload required)"}), flush=True)
                # Reload module in current process
                import importlib
                importlib.reload(_twc)
            except PermissionError:
                print(json.dumps({"info": "twikit client.py patch skipped (read-only)"}), flush=True)
        else:
            print(json.dumps({"info": "twikit client.py already patched or pattern not found"}), flush=True)
    except Exception as _pe:
        # 껍데기를 쓰면 이 패치는 애초에 필요 없다 — '실패' 로 찍지 않는다.
        if not USING_SHIM:
            print(json.dumps({"info": f"twikit source patch failed: {_pe}"}), flush=True)

    # ★ UA 는 앱이 정한 것을 그대로 쓴다. twikit 기본값은 macOS 사파리였다 —
    #   윈도우 크롬 쿠키를 쓰면서 맥 사파리라고 말하는 셈이라 그 자체가 신호였다.
    _ua = init_args.get("user_agent") or None
    if USING_SHIM:
        client = Client(language="ja", user_agent=_ua,
                        proxy=(init_args.get("proxy") or None))
    else:
        client = Client(language="ja")
        if _ua:
            try: client._user_agent = _ua
            except Exception: pass
    client.set_cookies({
        "auth_token": auth_token,
        "ct0": ct0,
    })
    # ★ httpx timeout 길게 — Twitter API 가 가끔 응답 늦음 (Cloudflare / rate limit etc)
    #   default 5초 → 30초. ReadTimeout 자주 발생 방지.
    try:
        import httpx as _httpx
        new_timeout = _httpx.Timeout(30.0, connect=15.0, read=30.0, write=15.0, pool=10.0)
        client.http.timeout = new_timeout
        # ★ 프록시(VPN) — 계정마다 다른 출구로 나가게 한다.
        #   시스템 VPN 은 기계 전체를 한 경로로 보내므로 계정별 분리가 안 된다.
        #   앱이 계정에 붙은 프로필을 URL 한 줄로 만들어 넘겨준다.
        #   transport 를 통째로 바꾸므로 retries 설정도 여기서 같이 준다.
        _proxy = init_args.get("proxy") or ""
        if _proxy:
            if not USING_SHIM:
                client.http._transport = _httpx.HTTPTransport(retries=3, proxy=_proxy)
            # 비밀번호는 찍지 않는다 — 로그가 그대로 남는다.
            _safe = _proxy
            if "@" in _safe:
                _safe = _safe.split("://")[0] + "://***@" + _safe.rsplit("@", 1)[1]
            print(json.dumps({"info": f"프록시 사용: {_safe}"}), flush=True)
        else:
            if not USING_SHIM:
                client.http._transport = _httpx.HTTPTransport(retries=3)
        print(json.dumps({"info": "httpx timeout=30s + retries=3 적용"}), flush=True)
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
    }

    # ★ x_session 은 초기화를 스스로 한다 — 이관 페이지 처리, ondemand.s 청크 찾기,
    #   KEY_BYTE 인덱스 추출까지 유지되는 라이브러리가 맡는다. 아래 손으로 하던
    #   정규식 뭉치는 twikit 을 쓸 때만 돈다.
    if USING_SHIM:
        try:
            tid_ok = await client.client_transaction.init(client.http, ct_headers)
        except Exception as _e:
            tid_ok = False
        print(json.dumps({"info": f"TID(x_session): {client.client_transaction.reason}"}),
              flush=True)

    # Method 1: Direct manual TID init with auth cookies (most reliable)
    try:
        if USING_SHIM:
            raise RuntimeError("x_session 이 이미 처리했습니다")
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
        # x_session 을 쓰면 이 경로는 '실패' 가 아니라 '안 한 것' 이다. 실패로 찍으면
        # 로그를 읽는 사람이 없는 고장을 쫓게 된다.
        if not USING_SHIM:
            print(json.dumps({"info": f"TID manual init failed: {e1}"}), flush=True)

        # Method 2: Try twikit's built-in init
        try:
            if USING_SHIM:
                raise RuntimeError("x_session 이 이미 처리했습니다")
            await client.client_transaction.init(client.http, ct_headers)
            deduplicate_cookies(client)
            tid_ok = True
            print(json.dumps({"info": "TID twikit init OK"}), flush=True)
        except Exception as e2:
            if not USING_SHIM:
                print(json.dumps({"info": f"TID twikit init also failed: {e2}"}), flush=True)

    # If all TID methods failed, use random transaction IDs
    # ★ twikit 으로 서명을 못 만들었으면 여기서 x_session 으로 갈아탄다.
    #   무작위 서명으로 계속 가는 것보다 낫다 — 그건 되는 척하다가 X 가 막을 때
    #   비로소 드러나는 상태다. x_session 은 유지되는 라이브러리로 서명하므로
    #   적어도 '왜 안 되는지' 가 분명해진다.
    if not tid_ok and not USING_SHIM:
        try:
            import x_session as _xs
            print(json.dumps({"info": "twikit 으로 서명을 못 만들어 x_session 으로 갈아탑니다"}),
                  flush=True)
            Endpoint = _xs.Endpoint
            FEATURES = _xs.FEATURES
            USER_FEATURES = _xs.USER_FEATURES
            flatten_params = _xs.flatten_params
            client = _xs.Client(language="ja",
                                user_agent=(init_args.get("user_agent") or None),
                                proxy=(init_args.get("proxy") or None))
            client.set_cookies({"auth_token": auth_token, "ct0": ct0})
            USING_SHIM = True
            # 갈아탄 뒤에도 앱이 긁어 둔 최신 해시를 그대로 얹는다
            _c = _load_cached_hashes()
            if _c:
                apply_hashes_to_endpoint(Endpoint, _c)
            tid_ok = await client.client_transaction.init(client.http, ct_headers)
            print(json.dumps({"info": f"TID(x_session): {client.client_transaction.reason}"}),
                  flush=True)
        except Exception as _xe:
            print(json.dumps({"info": f"x_session 으로도 못 갈아탔습니다: {_xe}"}), flush=True)

    if not tid_ok:
        # ★ 여기서 'import os' 를 하면 안 된다. 함수 안 어디서든 import 하면 파이썬이
        #   그 이름을 함수 전체의 지역 변수로 잡아, 이 줄보다 위에서 os 를 쓰는 곳이
        #   UnboundLocalError 로 죽는다. os 는 이미 파일 맨 위에 있다.
        import base64
        def _fake_transaction_id(*args, **kwargs):
            return base64.b64encode(os.urandom(72)).decode('ascii')[:96]
        client.client_transaction.generate_transaction_id = _fake_transaction_id
        print(json.dumps({"info": "TID bypassed: using random transaction IDs"}), flush=True)

    # Re-apply cached hashes AFTER TID init (in case twikit reset them)
    cached2 = _load_cached_hashes()
    if cached2:
        apply_hashes_to_endpoint(Endpoint, cached2)

    # Signal ready with debug info
    print(json.dumps({"status": "ready", "tid": tid_ok, "_st_url": Endpoint.SEARCH_TIMELINE[-40:]}), flush=True)

    # Track 404 for auto-repair
    repair_count = 0          # How many repairs attempted
    last_repair_time = 0      # Timestamp of last repair
    consecutive_404 = 0       # Track consecutive 404s

    # Process commands from stdin
    #   ★ Windows 는 기본 이벤트 루프가 Proactor 인데, 거기서는 connect_read_pipe 로
    #     stdin 을 읽을 수 없다. 붙이면 _ProactorReadPipeTransport._loop_reading 에서
    #     예외가 나고 이후 명령을 하나도 받지 못한다. 앱 쪽에서는 ready 까지는 받아
    #     놓고(이 루프 진입 전에 출력되므로) 30초를 기다리다
    #     "Daemon: incomplete response (0 bytes, 30000ms)" 로 포기한다 — 실측.
    #     그래서 윈도우에서는 blocking readline 을 스레드에서 돌려 같은 모양으로 쓴다.
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
        try:
            line = await _read_line()
            if not line:
                break  # EOF
            line = line.decode('utf-8').strip()
            if not line:
                continue

            args = json.loads(line)
            action = args.get("action", "")

            if action == "quit":
                print(json.dumps({"status": "bye"}), flush=True)
                break

            if action == "ping":
                print(json.dumps({"status": "pong"}), flush=True)
                continue

            if action == "refresh_cookies":
                # Auto-refresh: extract fresh cookies from Chrome
                print(json.dumps({"info": "Attempting to refresh cookies from Chrome..."}), flush=True)
                new_auth, new_ct0 = extract_chrome_twitter_cookies()
                if new_auth and new_ct0:
                    auth_token = new_auth
                    ct0 = new_ct0
                    client.set_cookies({"auth_token": auth_token, "ct0": ct0})
                    deduplicate_cookies(client)
                    print(json.dumps({"status": "ok", "info": "Cookies refreshed from Chrome",
                                      "auth_token": auth_token[:10] + "...", "ct0": ct0[:10] + "..."}), flush=True)
                else:
                    print(json.dumps({"status": "error", "error": "Failed to extract cookies from Chrome"}), flush=True)
                continue

            if action == "update_endpoints":
                # Manual trigger to refresh hashes (called by C++ on 404)
                print(json.dumps({"info": "Manual hash refresh requested..."}), flush=True)
                updated, err = await auto_repair_hashes(
                    client, Endpoint, auth_token=auth_token, ct0=ct0
                )
                if err:
                    print(json.dumps({"status": "error", "error": err}), flush=True)
                else:
                    # Re-init TID after hash update
                    try:
                        ct_headers = {
                            'Accept-Language': 'en-US,en;q=0.9',
                            'Cache-Control': 'no-cache',
                            'Referer': 'https://x.com',
                            'User-Agent': client._user_agent,
                        }
                        await client.client_transaction.init(client.http, ct_headers)
                        deduplicate_cookies(client)
                    except Exception:
                        pass
                    repair_count = 0
                    consecutive_404 = 0
                    last_repair_time = time.time()
                    print(json.dumps({"status": "ok", "updated": updated or []}), flush=True)
                continue

            deduplicate_cookies(client)  # Clean before each API call
            result = await handle_command(client, args, Endpoint, FEATURES, USER_FEATURES, flatten_params)

            # Auto-repair: if 404, try fetching new hashes and retry
            # Allow repair if: never repaired, or >60 seconds since last repair
            if result.get("status") == 404:
                consecutive_404 += 1
                can_repair = (
                    repair_count < 5 and  # Max 5 auto-repairs per session
                    (time.time() - last_repair_time) > 60  # At least 60s between repairs
                )

                if can_repair:
                    repair_count += 1
                    last_repair_time = time.time()
                    print(json.dumps({"info": f"HTTP 404 detected (attempt {repair_count}/5), auto-repairing GraphQL hashes..."}), flush=True)

                    # Clear cached hashes first to force fresh fetch
                    try:
                        if os.path.exists(_hash_cache_path):
                            os.remove(_hash_cache_path)
                    except Exception:
                        pass

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
                            }
                            await client.client_transaction.init(client.http, ct_headers)
                            deduplicate_cookies(client)
                        except Exception:
                            pass
                        # Retry the command
                        result = await handle_command(client, args, Endpoint, FEATURES, USER_FEATURES, flatten_params)
                        if result.get("status") == 200:
                            consecutive_404 = 0  # Success! Reset counter
            elif result.get("status") in (401, 403):
                # Auth error — try auto-refreshing cookies from Chrome
                print(json.dumps({"info": f"HTTP {result['status']} auth error, attempting cookie refresh from Chrome..."}), flush=True)
                new_auth, new_ct0 = extract_chrome_twitter_cookies()
                if new_auth and new_ct0 and (new_auth != auth_token or new_ct0 != ct0):
                    auth_token = new_auth
                    ct0 = new_ct0
                    client.set_cookies({"auth_token": auth_token, "ct0": ct0})
                    deduplicate_cookies(client)
                    print(json.dumps({"info": f"Cookies refreshed! Retrying... (token={auth_token[:10]}...)"}), flush=True)
                    # Retry the command with fresh cookies
                    result = await handle_command(client, args, Endpoint, FEATURES, USER_FEATURES, flatten_params)
                else:
                    print(json.dumps({"info": "Cookie refresh failed or cookies unchanged"}), flush=True)
                consecutive_404 = 0
            else:
                consecutive_404 = 0  # Non-404 response resets counter

            print(json.dumps(result), flush=True)

        except json.JSONDecodeError as e:
            print(json.dumps({"error": f"Invalid JSON: {str(e)}"}), flush=True)
        except Exception as e:
            print(json.dumps({"error": f"Command error: {str(e)}"}), flush=True)


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


async def handle_command(client, args, Endpoint, FEATURES, USER_FEATURES, flatten_params):
    action = args["action"]

    try:
        if action == "user_by_screen_name":
            variables = {
                "screen_name": args["screen_name"],
                "withSafetyModeUserFields": False,
            }
            params = flatten_params({
                'variables': variables,
                'features': USER_FEATURES,
                'fieldToggles': {"withAuxiliaryUserLabels": False},
            })
            data, response = await client.request(
                'GET', Endpoint.USER_BY_SCREEN_NAME,
                params=params, headers=client._base_headers,
                raise_exception=False
            )
            # Normalize response: inject name/profile_image into legacy for backward compat
            if response.status_code == 200 and isinstance(data, dict):
                try:
                    u = data.get("data", {}).get("user", {}).get("result", {})
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
            return {"status": response.status_code, "body": data if response.status_code == 200 else response.text}

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

            params = flatten_params({
                'variables': variables,
                'features': FEATURES,
            })
            data, response = await client.request(
                'GET', Endpoint.USER_TWEETS,
                params=params, headers=client._base_headers,
                raise_exception=False
            )
            return {"status": response.status_code, "body": data if response.status_code == 200 else response.text}

        elif action == "search":
            variables = {
                "rawQuery": args["query"],
                "count": args.get("count", 40),
                "querySource": "typed_query",
                "product": "Latest",
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]

            params = flatten_params({
                'variables': variables,
                'features': FEATURES,
            })
            data, response = await client.request(
                'GET', Endpoint.SEARCH_TIMELINE,
                params=params, headers=client._base_headers,
                raise_exception=False
            )

            # Fallback: if GraphQL SearchTimeline returns 404 (account restricted),
            # try v1.1 search API which is less likely to be blocked
            if response.status_code == 404:
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

            return {"status": response.status_code, "body": data if response.status_code == 200 else response.text}

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

            params = flatten_params({
                'variables': variables,
                'features': FEATURES,
            })
            data, response = await client.request(
                'GET', Endpoint.USER_LIKES,
                params=params, headers=client._base_headers,
                raise_exception=False
            )
            return {"status": response.status_code, "body": data if response.status_code == 200 else response.text}

        elif action == "bookmarks":
            variables = {
                "count": args.get("count", 40),
                "includePromotedContent": True,
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]

            bk_features = dict(FEATURES)
            bk_features["graphql_timeline_v2_bookmark_timeline"] = True

            params = flatten_params({
                'variables': variables,
                'features': bk_features,
            })
            data, response = await client.request(
                'GET', Endpoint.BOOKMARKS,
                params=params, headers=client._base_headers,
                raise_exception=False
            )
            return {"status": response.status_code, "body": data if response.status_code == 200 else response.text}

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

            params = flatten_params({
                'variables': variables,
                'features': FEATURES,
                'fieldToggles': {"withArticleRichContentState": True, "withArticlePlainText": False, "withGrokAnalyze": False, "withDisallowedReplyControls": False},
            })
            data, response = await client.request(
                'GET', Endpoint.TWEET_DETAIL,
                params=params, headers=client._base_headers,
                raise_exception=False
            )
            return {"status": response.status_code, "body": data if response.status_code == 200 else response.text}

        elif action in ("followers", "following"):
            variables = {
                "userId": args["user_id"],
                "count": args.get("count", 40),
                "includePromotedContent": False,
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]

            endpoint = Endpoint.FOLLOWERS if action == "followers" else Endpoint.FOLLOWING
            params = flatten_params({
                'variables': variables,
                'features': FEATURES,
            })
            data, response = await client.request(
                'GET', endpoint,
                params=params, headers=client._base_headers,
                raise_exception=False
            )

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
                    else:
                        endpoint = Endpoint.FOLLOWERS if action == "followers" else Endpoint.FOLLOWING
                        print(json.dumps({"info": f"hash 갱신됨: {updated} → retry"}), flush=True)
                        data, response = await client.request(
                            'GET', endpoint,
                            params=params, headers=client._base_headers,
                            raise_exception=False
                        )
                except Exception as e:
                    print(json.dumps({"info": f"{action} hash repair 예외: {e}"}), flush=True)

            return {"status": response.status_code, "body": data if response.status_code == 200 else response.text}

        elif action == "highlights":
            variables = {
                "userId": args["user_id"],
                "count": args.get("count", 40),
                "includePromotedContent": False,
                "withVoice": True,
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]
            params = flatten_params({
                'variables': variables,
                'features': FEATURES,
            })
            data, response = await client.request(
                'GET', Endpoint.USER_HIGHLIGHTS_TWEETS,
                params=params, headers=client._base_headers,
                raise_exception=False
            )
            return {"status": response.status_code, "body": data if response.status_code == 200 else response.text}

        elif action in ("favoriters", "retweeters"):
            variables = {
                "tweetId": args["tweet_id"],
                "count": args.get("count", 40),
                "includePromotedContent": True,
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]
            endpoint = Endpoint.FAVORITERS if action == "favoriters" else Endpoint.RETWEETERS
            params = flatten_params({
                'variables': variables,
                'features': FEATURES,
            })
            data, response = await client.request(
                'GET', endpoint,
                params=params, headers=client._base_headers,
                raise_exception=False
            )
            return {"status": response.status_code, "body": data if response.status_code == 200 else response.text}

        elif action == "list_tweets":
            variables = {
                "listId": args["list_id"],
                "count": args.get("count", 40),
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]
            params = flatten_params({
                'variables': variables,
                'features': FEATURES,
            })
            data, response = await client.request(
                'GET', Endpoint.LIST_LATEST_TWEETS_TIMELINE,
                params=params, headers=client._base_headers,
                raise_exception=False
            )
            return {"status": response.status_code, "body": data if response.status_code == 200 else response.text}

        elif action == "community_tweets":
            variables = {
                "communityId": args["community_id"],
                "count": args.get("count", 40),
                "withCommunity": True,
                "rankingMode": args.get("ranking", "Relevance"),  # or "Recency"
            }
            if args.get("cursor"):
                variables["cursor"] = args["cursor"]
            params = flatten_params({
                'variables': variables,
                'features': FEATURES,
            })
            data, response = await client.request(
                'GET', Endpoint.COMMUNITY_TWEETS_TIMELINE,
                params=params, headers=client._base_headers,
                raise_exception=False
            )
            return {"status": response.status_code, "body": data if response.status_code == 200 else response.text}

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
    #     TID 폴백으로 넘어간다 — 즉 윈도우에서 twikit 데몬이 한 번도 뜬 적이 없다.
    #     (실측: Windows 11 / Python 3.14.3 / twikit 2.3.3 정상 설치 상태에서도 동일)
    if hasattr(signal, "SIGPIPE"):
        signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    asyncio.run(main())
