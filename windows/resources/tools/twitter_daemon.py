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

_hash_cache_path = os.path.join(
    os.path.expanduser("~"), "Library", "Application Support", "ABIWA", "graphql_hashes.json"
)


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
        with open(_hash_cache_path, "w") as f:
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

    return auth_token, ct0


# ── Main daemon ───────────────────────────────────────────────────────────

async def main():
    init_args = json.loads(sys.argv[1])
    auth_token = init_args["auth_token"]
    ct0 = init_args["ct0"]

    try:
        from twikit import Client
        from twikit.client.gql import Endpoint, FEATURES, USER_FEATURES, flatten_params
    except ImportError:
        print(json.dumps({"error": "twikit not installed"}), flush=True)
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

    # ── In-process source-level fix for twikit bug: 'code' KeyError ──
    # twikit/client/client.py line ~158 accesses response_data['errors'][0]['code']
    # unconditionally. Twitter sometimes returns errors without 'code' (only 'message'
    # or 'extensions.code'). We patch the bundled source file ONCE if needed.
    try:
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
        print(json.dumps({"info": f"twikit source patch failed: {_pe}"}), flush=True)

    client = Client(language="ja")
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
        # 재시도 transport — connection error 시 자동 3회 retry
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

    # Method 1: Direct manual TID init with auth cookies (most reliable)
    try:
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
            await client.client_transaction.init(client.http, ct_headers)
            deduplicate_cookies(client)
            tid_ok = True
            print(json.dumps({"info": "TID twikit init OK"}), flush=True)
        except Exception as e2:
            print(json.dumps({"info": f"TID twikit init also failed: {e2}"}), flush=True)

    # If all TID methods failed, use random transaction IDs
    if not tid_ok:
        import base64, os
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
    loop = asyncio.get_event_loop()
    reader = asyncio.StreamReader()
    protocol = asyncio.StreamReaderProtocol(reader)
    await loop.connect_read_pipe(lambda: protocol, sys.stdin)

    while True:
        try:
            line = await reader.readline()
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
    signal.signal(signal.SIGPIPE, signal.SIG_DFL)
    asyncio.run(main())
