#!/usr/bin/env python3
"""Bluesky collection daemon for ハンイシキ - based on AINU (ainu_smart.py).

Usage: python3 bluesky_daemon.py '{"handle":"...","password":"..."}'
Then send JSON commands on stdin, receive JSON responses on stdout.

Commands:
  {"action":"posts","target":"handle","save_path":"/path","download_media":true,"exif":true}
  {"action":"likes","target":"handle","save_path":"/path","download_media":true}
  {"action":"media","target":"handle","save_path":"/path"}
  {"action":"followers","target":"handle","save_path":"/path"}
  {"action":"following","target":"handle","save_path":"/path"}
  {"action":"blocks","save_path":"/path"}
  {"action":"mutes","save_path":"/path"}
  {"action":"lists","target":"handle","save_path":"/path"}
  {"action":"notifications","save_path":"/path"}
  {"action":"messages","save_path":"/path"}
  {"action":"search","query":"keyword","save_path":"/path","download_media":true}
  {"action":"profile","target":"handle","save_path":"/path"}
  {"action":"ping"}
  {"action":"quit"}
"""

import os, sys, json, time, asyncio, re, subprocess, shutil
from datetime import datetime, timezone, timedelta
from pathlib import Path

def log(msg):
    print(json.dumps({"log": str(msg)}), flush=True)

# -- 응답 모양이 바뀌어도 버티는 읽기 --------------------------------------
#   atproto SDK 도 lexicon 도 이름을 바꾼다. 이름 하나만 믿고 getattr 하면
#   바뀐 날 빈 목록을 읽고 조용히 0개로 끝난다 -- 팬박스가 그렇게 여덟 달을
#   비어 있었다(2026-09-24). 이름을 여러 개 대고, 못 찾으면 소리를 낸다.
def pick(obj, names, default=None):
    """여러 이름 중 먼저 '있고 비어 있지 않은' 것을 준다. dict 도 객체도 받는다."""
    for n in names:
        v = obj.get(n) if isinstance(obj, dict) else getattr(obj, n, None)
        if v is not None and v != [] and v != "":
            return v
    return default

_shape_warned = set()
def pick_list(obj, names, what):
    """목록을 꺼낸다. 아는 이름이 하나도 없으면 한 번만 경고한다(매 쪽마다 떠들지 않게)."""
    v = pick(obj, names)
    if isinstance(v, list):
        return v
    if v is None and what not in _shape_warned:
        _shape_warned.add(what)
        have = list(obj.keys())[:14] if isinstance(obj, dict) else [k for k in dir(obj) if not k.startswith('_')][:14]
        log("[모양바뀜] " + what + ": 아는 이름(" + "/".join(names) + ")이 없습니다. 받은 칸: " + str(have))
    return v if isinstance(v, list) else []


def progress(count, media_count=0, status="수집 중...", fresh=None, newest=None):
    p = {"count": count, "media": media_count, "status": status}
    # ★ 이번 판의 '새 글' 수와 가장 새 제 글 시각도 함께 싣는다 — 중지 · 데몬이 죽어 마지막 결과 줄이
    #   오지 않아도 앱이 그때까지 센 값을 대상 목록에 남길 수 있게(sendCommand 가 마지막 값을 쥐고 있다).
    if fresh is not None:
        p["fresh"] = fresh
    if newest:
        p["newest"] = newest
    print(json.dumps({"progress": p}), flush=True)


# -- 다 받은 곳에서 일찍 멈추기 (사용자 2026-10-24 — 대상이 수백 개일 때) --------------------
#   gallery-dl 의 abort:N 과 같다. 다시 받을 때 '이미 받은 글' 이 N개 이어 나오면 그 대상(그 종류)은
#   거기서 멈춘다 — 매 판 몇 년 치 피드를 끝까지 훑지 않게. N = 0 이면 예전처럼 끝까지 간다.
#   '이미 받은' 은 이번 판을 시작하기 '전' 의 엑셀에 있던 글만이다. 중간 저장(500·1000개마다, 요금 제한 때)이
#   같은 파일에 이번 판 글을 써 넣으므로, 파일을 그때그때 읽으면 이번 판에 처음 받은 글까지 '이미 받은' 이
#   되어 겹치는 쪽에서 너무 일찍 멈춘다. 그래서 시작할 때 한 번 찍어 둔다.
def snapshot_known(filename):
    """save_xlsx 가 쓴 엑셀의 uri 열을 읽는다 → (set((type, uri)), set(uri), type 열이 있나).
    save_xlsx 가 합칠 때 쓰는 열쇠와 같다 — 포스트는 (type, uri)(제 글을 제가 리포스트하면 같은 uri 로
    Post·Repost 두 줄), 좋아요·답글·댓글은 uri. 파일이 없거나 못 읽으면 빈 집합 — 모두 새 글로 보고 끝까지 간다."""
    keys, uris, has_type = set(), set(), False
    if not filename or not os.path.exists(filename):
        return keys, uris, has_type
    try:
        import openpyxl
        wb = openpyxl.load_workbook(filename, read_only=True)
        try:
            rows = wb.active.iter_rows(values_only=True)
            head = [str(h) if h is not None else '' for h in (next(rows, None) or ())]
            if 'uri' not in head:
                return keys, uris, has_type
            ui = head.index('uri')
            ti = head.index('type') if 'type' in head else -1
            has_type = ti >= 0
            for r in rows:
                if not r or ui >= len(r) or r[ui] is None:
                    continue
                u = str(r[ui])
                if not u:
                    continue
                uris.add(u)
                if has_type:
                    t = r[ti] if ti < len(r) else None
                    keys.add(('' if t is None else str(t), u))
        finally:
            wb.close()
    except Exception as e:
        log(f"이미 받은 글 목록(엑셀)을 읽지 못해 이번 판은 끝까지 봅니다: {e}")
        return set(), set(), False
    return keys, uris, has_type


WALK_MARK_FORMAT = 1   # 수집기를 고쳐 옛 글에서 놓친 것을 다시 받아야 하면 올린다 — 모든 목록이 한 번 끝까지 본다
_walk_full_at = {}     # 이번 명령이 읽은 표시의 '끝까지 본 때' — 일찍 멈춘 판이 다시 적을 때 물려준다


def _walk_period_days(path):
    """끝까지 다시 볼 때까지의 날 수 — 28~35일, 대상 · 목록마다 달리(같은 날 처음 받은 것들이 한날에 몰리지 않게)."""
    h = 2166136261
    for ch in path.encode("utf-8"):
        h ^= ch
        h = (h * 16777619) & 0xFFFFFFFF
    return 28 + h % 8


def _take_walk_file(path):
    try:
        with open(path, encoding="utf-8") as f:
            o = json.load(f)
    except Exception:
        o = {}
    try:
        os.remove(path)
    except OSError:
        pass
    ok = False
    try:
        full = datetime.fromisoformat(str(o.get("full", "")).replace("Z", "+00:00"))
        if full.tzinfo is None:
            full = full.replace(tzinfo=timezone.utc)
        now = datetime.now(timezone.utc)
        ok = (int(o.get("v", 0)) == WALK_MARK_FORMAT and full <= now + timedelta(hours=1)
              and (now - full).days < _walk_period_days(path))
    except Exception:
        ok = False
    if ok:
        _walk_full_at[path] = o["full"]
    else:
        _walk_full_at.pop(path, None)
    return ok


def take_walk_marks(d, prefix):
    """지난번에 그 목록을 끊김 없이 다 봤나(<d>/.hanishiki_walk_<kind>.json) — 같은 엑셀을 쓰는 범위(미디어 켬/끔 ·
    답글 · 리포스트)의 표시를 모두 읽고 지운다(믿을 만한 kind 들을 돌려준다). 없으면(첫 판 · 중지 · 최대 수 · 요금 제한 ·
    API 오류로 끊긴 판 뒤) 이번 판은 일찍 멈추지 않고 끝까지 보아 사이를 메운다 — 끊긴 판이 새 글의 위쪽만 받아 두면,
    다음 판이 그 위에서 멈춰 아래쪽 새 글을 영영 놓쳤다(검토).
    한 범위의 걸음이 그 엑셀에 줄을 넣으면 다른 범위의 표시는 그 줄을 보지 않은 채 남아, 다음 판을 그 범위가
    본 적 없는 줄에서 일찍 멈추게 했다(검토 — 미디어 끔 판 뒤의 미디어 켬 판이 미디어를 영영 안 받음).
    ★ 표시가 있어도 그 목록을 마지막으로 끝까지 본 지 4~5주가 지났거나 형식 번호가 다르면 없는 것으로 본다 — 어떤 까닭으로든
    사이가 생겨도 한 달 안에 한 번은 끝까지 보아 메운다(앱의 Common::takeCleanWalkMark 와 같다)."""
    out = set()
    head = f".hanishiki_walk_{prefix}"
    for k in [k for k in _walk_full_at if os.path.dirname(k) == d and os.path.basename(k).startswith(head)]:
        _walk_full_at.pop(k, None)
    try:
        names = os.listdir(d)
    except OSError:
        return out
    for n in names:
        if n.startswith(head) and n.endswith(".json"):
            if _take_walk_file(os.path.join(d, n)):
                out.add(n[len(".hanishiki_walk_"):-len(".json")])
    return out


def put_walk_mark(d, kind):
    path = os.path.join(d, f".hanishiki_walk_{kind}.json")
    now = datetime.now(timezone.utc).isoformat()
    full = _walk_full_at.pop(path, None) or now   # 믿을 만한 표시로 시작한 판은 앞 표시의 때를, 끝까지 본 판은 지금을
    tmp = path + ".part"
    try:
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"at": now, "full": full, "v": WALK_MARK_FORMAT}, f)
        os.replace(tmp, path)
    except OSError:
        pass


def take_walk_failures(d, kind):
    """지난 판에 미디어를 받지 못한 글(<d>/.hanishiki_walkfail_<kind>.json) — 읽고 지운다."""
    path = os.path.join(d, f".hanishiki_walkfail_{kind}.json")
    try:
        with open(path, encoding="utf-8") as f:
            ids = {str(x) for x in (json.load(f).get("ids") or []) if x}
    except Exception:
        ids = set()
    try:
        os.remove(path)
    except OSError:
        pass
    return ids


def put_walk_failures(d, kind, ids):
    path = os.path.join(d, f".hanishiki_walkfail_{kind}.json")
    if not ids:
        try:
            os.remove(path)
        except OSError:
            pass
        return
    try:
        with open(path, "w", encoding="utf-8") as f:
            json.dump({"at": datetime.now(timezone.utc).isoformat(), "ids": sorted(ids)}, f)
    except OSError:
        pass


def iso_to_ms(s):
    """'2024-05-01T12:34:56.789Z' → 1970 부터의 밀리초(UTC). 못 읽으면 0."""
    if not s:
        return 0
    try:
        dt = datetime.fromisoformat(str(s).replace('Z', '+00:00'))
        if dt.tzinfo is None:
            dt = dt.replace(tzinfo=timezone.utc)   # 시간대 없는 값은 UTC 로 본다(createdAt 은 UTC 가 규격)
        return int(dt.timestamp() * 1000)
    except Exception:
        return 0


class KnownStreak:
    """한 종류(포스트 · 답글 · 좋아요) 한 판의 '이미 받은 글' 연속 수를 센다.
    - 이미 받은 글 → 연속 +1 (받던 방식은 그대로 — 엑셀에 다시 담고 미디어도 예전처럼 확인한다)
    - 정말 새 글  → 연속 0, 새 글 +1
    - 이번 판에 이미 본 글(쪽이 겹쳐 두 번 온 것) · 거른 글(리포스트 빼기 · 답글 아닌 글)은 세지 않는다(중립)"""

    def __init__(self, filename, stop_after=0):
        self.keys, self.uris, self.has_type = snapshot_known(filename)
        try:
            self.stop_after = max(0, int(stop_after or 0))
        except (TypeError, ValueError):
            self.stop_after = 0
        self.streak = 0
        self.new = 0
        self.stopped = 0      # 멈췄으면 그때의 연속 수
        self.newest = 0       # 이번 판에 본 그 계정의 '제 글' 가운데 가장 새 것(ms, UTC)
        self._seen = set()

    def is_known(self, ptype, uri):
        if self.has_type:
            return (ptype or '', uri) in self.keys
        return uri in self.uris

    def see(self, ptype, uri):
        """받은 글 하나를 센다. True 면 여기서 멈춘다."""
        if not uri:
            return False
        k = (ptype or '', uri)
        if k in self._seen:
            return False
        self._seen.add(k)
        if self.is_known(ptype, uri):
            self.streak += 1
            if self.stop_after > 0 and self.streak >= self.stop_after:
                self.stopped = self.streak
                return True
        else:
            self.streak = 0
            self.new += 1
        return False

    def own(self, post, target_did=''):
        """그 계정의 제 글이면 작성 시각(record.createdAt)을 '가장 새 글' 후보로 둔다.
        리포스트는 부르지 않는다 — 남의 옛 글이라 그 계정이 얼마나 자주 쓰는지와 상관이 없다."""
        author = getattr(post, 'author', None)
        if target_did and author is not None and getattr(author, 'did', '') not in ('', target_did):
            return
        record = getattr(post, 'record', None)
        ms = iso_to_ms(getattr(record, 'created_at', '') if record else '')
        # 앞날(시계가 틀린 글 · 손으로 지어낸 createdAt)은 버린다 — 하나라도 가장 큰 값이 되면 앱이 통째로 버린다
        if ms > self.newest and ms <= (time.time() + 86400) * 1000:
            self.newest = ms

    def fields(self):
        """결과 줄에 붙일 칸 — new_count(이번 판 새 글) · stopped_known(멈춘 연속 수, 0 = 안 멈춤) · newest(ms)."""
        d = {"new_count": self.new, "stopped_known": self.stopped}
        if self.newest:
            d["newest"] = self.newest
        return d


def _read_init_line():
    """첫 줄만 원시 fd 에서 읽는다.
    ★ sys.stdin.readline() 을 쓰면 안 된다. 파이썬의 버퍼가 줄 너머까지 미리 읽어
      버려서, 그 뒤 명령 루프(asyncio 는 fd 0 을 직접 붙인다)가 첫 명령을 잃는다.
      한 줄은 수백 바이트뿐이라 한 글자씩 읽어도 비용이 없다."""
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
      프로세스나 읽을 수 있어서, 앱 비밀번호와 세션 토큰이 그대로 노출됐다
      (실제로 ps 출력에 찍히는 것을 확인했다).
      이제 stdin 첫 줄로 받는다. 뒤이은 명령 통로는 그대로다.
      argv 경로는 손으로 시험할 때를 위해 남겨 두되, 그 쓰임은 노출된다."""
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


def main():
    init_args = _load_init_args("Usage: bluesky_daemon.py --stdin-args  (init JSON on first stdin line)")

    # Support multiple accounts: {"accounts":[{"handle":"...","password":"..."},...]}
    # or single account: {"handle":"...","password":"..."}
    accounts = init_args.get("accounts", [])
    if not accounts:
        accounts = [{"handle": init_args["handle"], "password": init_args["password"]}]

    # Import dependencies
    try:
        from atproto import Client as ATProtoClient
    except ImportError:
        print(json.dumps({"error": "atproto not installed. pip install atproto"}), flush=True)
        sys.exit(1)

    try:
        import openpyxl
        from openpyxl.styles import Font, PatternFill
    except ImportError:
        print(json.dumps({"error": "openpyxl not installed"}), flush=True)
        sys.exit(1)

    # Login with first account
    current_account_idx = 0
    clients = []
    for i, acct in enumerate(accounts):
        try:
            c = ATProtoClient()
            c.login(acct["handle"], acct["password"])
            clients.append(c)
            log(f"Account {i+1} logged in: {acct['handle']}")
        except Exception as e:
            log(f"Account {i+1} login failed: {acct['handle']} - {e}")

    if not clients:
        print(json.dumps({"error": f"Login failed: all accounts failed"}), flush=True)
        sys.exit(1)

    client = clients[0]

    def rotate_account():
        """Rate limit 시 다음 계정으로 순환"""
        nonlocal client, current_account_idx
        if len(clients) <= 1:
            return False
        current_account_idx = (current_account_idx + 1) % len(clients)
        client = clients[current_account_idx]
        log(f"🔄 계정 전환: {accounts[current_account_idx]['handle']} ({current_account_idx+1}/{len(clients)})")
        return True

    rate_limit_count = 0
    total_rate_limit_waits = 0
    MAX_TOTAL_WAITS = 20  # 대기를 최대 20회까지 허용
    adaptive_delay = 1.0  # 아이누 방식: 기본 1초 고정

    def get_delay():
        """아이누 방식: 고정 딜레이 (기본 1초, RL 후 일시 증가)"""
        return adaptive_delay

    def decay_delay():
        """아이누 방식: RL 후 증가한 딜레이를 원래대로"""
        nonlocal adaptive_delay
        adaptive_delay = 1.0  # 아이누: 항상 1초로 리셋

    # Flag for graceful shutdown during rate limit waits
    stop_requested = False

    def check_stdin_for_quit():
        """Non-blocking stdin check for quit command during rate limit waits.
        POSIX uses select(); Windows can't select() on a file object (raises
        OSError WinError 10093) → short-circuit so the collection loop never crashes.
        On Windows the Stop button still works via QProcess kill from C++."""
        nonlocal stop_requested
        if sys.platform == 'win32':
            return False
        try:
            import select
            while select.select([sys.stdin], [], [], 0)[0]:
                line = sys.stdin.readline().strip()
                if not line:
                    continue
                cmd = json.loads(line)
                if cmd.get("action") == "quit":
                    stop_requested = True
                    return True
        except Exception:
            return False
        return False

    # Rate limit mode: "wait" = 대기 후 재시도, "stop" = 즉시 중지
    rl_mode = "wait"  # default
    rl_wait_secs = 900  # default 15분

    def set_rate_limit_mode(mode, wait_mins=15):
        nonlocal rl_mode, rl_wait_secs
        rl_mode = mode  # "wait" or "stop"
        rl_wait_secs = max(60, min(3600, wait_mins * 60))

    def handle_rate_limit():
        """AINU 방식 Rate limit: 대기 → 중간 저장 → 다음 계정 전환"""
        nonlocal rate_limit_count, total_rate_limit_waits, client, adaptive_delay, stop_requested
        rate_limit_count += 1

        # "stop" 모드면 즉시 중지
        if rl_mode == "stop":
            log("⚠️ Rate Limit 발생 → 수집 중지 (설정: 중지 모드)")
            return False

        wait_secs = rl_wait_secs
        wait_mins = wait_secs // 60
        log(f"⚠️ Rate Limit 발생")
        log(f"⏰ {wait_mins}분 대기... (중지하려면 앱에서 중지 버튼을 누르세요)")

        # 대기 — 1초마다 stdin 체크해서 중지 가능하게
        for i in range(wait_secs):
            if stop_requested or check_stdin_for_quit():
                log("⏹️ 사용자에 의해 중지됨")
                return False
            remaining = wait_secs - i
            if remaining % 10 == 0:
                mins = remaining // 60
                secs = remaining % 60
                progress(0, 0, f"Rate Limit 대기... {mins}분 {secs}초")
            time.sleep(1)

        adaptive_delay = 1.0  # 대기 완료 → 딜레이 리셋
        log("✅ 대기 완료! 다음 계정으로 전환...")

        # 다음 계정으로 전환 (AINU 방식)
        if rotate_account():
            # 재로그인
            try:
                acct = accounts[current_account_idx]
                clients[current_account_idx] = ATProtoClient()
                clients[current_account_idx].login(acct["handle"], acct["password"])
                client = clients[current_account_idx]
                log(f"🔄 계정 전환: {acct['handle']}")
            except Exception as e:
                log(f"⚠️ 계정 전환 실패: {e}")
            return True
        else:
            # 계정 1개: 15분 대기 후 재로그인 시도
            total_rate_limit_waits += 1
            if total_rate_limit_waits > MAX_TOTAL_WAITS:
                log("❌ Rate Limit 소진. 나중에 다시 시도해주세요.")
                return False
            try:
                acct = accounts[current_account_idx]
                clients[current_account_idx] = ATProtoClient()
                clients[current_account_idx].login(acct["handle"], acct["password"])
                client = clients[current_account_idx]
                log("✅ 재로그인 성공! 수집 재개...")
            except Exception as e:
                log(f"⚠️ 재로그인 실패: {e}, 그래도 계속 시도...")
            return True

    print(json.dumps({"status": "ready"}), flush=True)

    # Helper: normalize handle
    def norm_handle(h):
        h = h.strip().lstrip('@')
        short = h.replace('.bsky.social', '')
        api = h if '.' in h else h + '.bsky.social'
        return short, api

    # Helper: process a post into a data row
    def process_post(post):
        """Extract data from a Bluesky post. Returns list of 11 fields."""
        try:
            uri = getattr(post, 'uri', '') or ''
            cid = getattr(post, 'cid', '') or ''
            author = getattr(post, 'author', None)
            record = getattr(post, 'record', None)

            author_handle = getattr(author, 'handle', '') if author else ''
            author_name = getattr(author, 'display_name', '') if author else ''

            text = getattr(record, 'text', '') if record else ''
            text = text.replace('\n', ' ').replace('\r', '')
            lang_list = getattr(record, 'langs', []) if record else []
            language = lang_list[0] if lang_list else ''

            created_str = getattr(record, 'created_at', '') if record else ''
            # Convert to JST
            created_at = ''
            if created_str:
                try:
                    dt = datetime.fromisoformat(created_str.replace('Z', '+00:00'))
                    from datetime import timedelta
                    jst = dt.astimezone(timezone(timedelta(hours=9)))
                    created_at = jst.strftime('%Y/%m/%d %H:%M:%S')
                except:
                    created_at = str(created_str)

            like_count = getattr(post, 'like_count', 0) or 0
            repost_count = getattr(post, 'repost_count', 0) or 0
            reply_count = (pick(post, ['reply_count', 'replyCount', 'replies_count'], 0) or 0)

            # Media
            embed = getattr(post, 'embed', None)
            has_media = False
            media_count = 0
            media_urls = []

            if embed:
                images = getattr(embed, 'images', None)
                if images:
                    has_media = True
                    for img in images:
                        url = pick(img, ['fullsize', 'thumb', 'url', 'image'], '')
                        if url:
                            media_urls.append(url)
                            media_count += 1

                video = getattr(embed, 'video', None)
                if video:
                    has_media = True
                    playlist = pick(video, ['playlist', 'url', 'cid'], '')
                    if playlist:
                        media_urls.append(playlist)
                        media_count += 1

                # Record with media (quote + images)
                rmedia = getattr(embed, 'media', None)
                if rmedia:
                    rimages = getattr(rmedia, 'images', None)
                    if rimages:
                        has_media = True
                        for img in rimages:
                            url = pick(img, ['fullsize', 'thumb', 'url', 'image'], '')
                            if url:
                                media_urls.append(url)
                                media_count += 1

                ext = getattr(embed, 'external', None)
                if ext:
                    thumb = getattr(ext, 'thumb', '')
                    if thumb:
                        has_media = True
                        media_urls.append(thumb)
                        media_count += 1

            # Post URL
            post_id = uri.split('/')[-1] if uri else ''
            post_url = f"https://bsky.app/profile/{author_handle}/post/{post_id}" if author_handle and post_id else ''

            return [uri, post_url, text, language, created_at,
                    author_handle, author_name,
                    like_count, repost_count, reply_count,
                    'Yes' if has_media else 'No', media_count,
                    '\n'.join(media_urls)]
        except Exception as e:
            return None

    # Helper: download media files
    # 이번 명령에서 미디어를 받지 못한 글 — 처음 실패한 글이 있으면 그 목록을 '끊김 없이 다 봄' 으로 적지 않는다(다음 판이 끝까지 보며 다시 받는다)
    dl_failed = set()   # 그 글의 uri — 두 판 잇따라 같은 글이 실패하면(지워진 미디어) 영영 안 되는 것으로 보아 표시를 더는 막지 않는다

    def finish_failures(excel_dir, walk_kind, prev_failed, ks, sak):
        """처음 실패한 글이 있으면 True(표시를 막는다 — 다음 판이 끝까지 보며 다시 받는다). 이번 판의 실패를 적어 둔다
        (이번 판에 보지 못한 글의 지난 기록은 그대로 둔다). uri 를 모르는 실패는 가릴 수 없어 늘 막는다."""
        named = {u for u in dl_failed if u}
        new = named - prev_failed
        gave = named & prev_failed
        if sak > 0 and new:   # 일찍 멈춤을 안 쓰면 판마다 끝까지 보며 다시 받는다 — 안내할 것이 없다
            log(f"미디어를 받지 못한 글 {len(new)}개 — 다음 판은 끝까지 보며 다시 받습니다")
        if sak > 0 and gave:
            log(f"두 판 잇따라 미디어를 받지 못한 글 {len(gave)}개는 일찍 멈춤을 더는 막지 않습니다(지워졌거나 막힌 듯 — 4~5주마다 끝까지 볼 때 다시 받아 봅니다)")
        seen = {u for _, u in ks._seen}
        put_walk_failures(excel_dir, walk_kind, named | (prev_failed - seen))
        return bool(new) or ('' in dl_failed)

    def download_media_files(post, media_dir, post_data, action_type="posts"):
        """Download media from a post. Returns count of new files.
        action_type: posts / likes / search — per-type subfolder + _complete mirror."""
        import urllib.request, shutil
        embed = getattr(post, 'embed', None)
        if not embed:
            return 0

        uri = getattr(post, 'uri', '') or ''
        post_id = uri.split('/')[-1] if uri else 'unknown'
        count = 0

        # Author username subfolder
        author = getattr(post, 'author', None)
        author_handle = getattr(author, 'handle', '') if author else ''
        if author_handle:
            short_author = author_handle.replace('.bsky.social', '')
            media_dir = os.path.join(media_dir, short_author)

        # Per-type subfolder (posts/, likes/, search/)
        type_dir = os.path.join(media_dir, action_type)
        os.makedirs(type_dir, exist_ok=True)
        # _complete mirror folder
        complete_dir = os.path.join(media_dir, "_complete")
        os.makedirs(complete_dir, exist_ok=True)
        # Override media_dir to type subfolder
        media_dir = type_dir

        # 파일명 생성: 게시물텍스트 (post_id_날짜).확장자
        def make_filename(suffix, ext):
            import re
            text = ''
            if post_data and len(post_data) > 1:
                text = str(post_data[1])[:40].strip().replace('\n', ' ')
            text = re.sub(r'[/\\:*?"<>|\x00-\x1f]', '_', text)
            if not text:
                text = post_id
            date_str = ''
            if post_data and len(post_data) > 4 and post_data[4]:
                try:
                    dt = datetime.strptime(post_data[4], '%Y/%m/%d %H:%M:%S')
                    date_str = dt.strftime('%Y%m%d_%H%M')
                except:
                    pass
            # 업로드 시각 prefix → OS 기본 정렬로 업로드 순 배치
            order_prefix = f"{date_str}_" if date_str else ""
            name = f"{order_prefix}{text} ({post_id}"
            if suffix:
                name += f"_{suffix}"
            name += f"){ext}"
            return name[:200]

        def dl(url, filename):
            nonlocal count
            filepath = os.path.join(media_dir, filename)
            if os.path.exists(filepath):
                # ★ 예전 판은 영상의 HLS 재생목록(m3u8 텍스트, 수백 바이트)을 .mp4 이름으로 저장했다.
                #   그것은 '받아 둔 영상' 이 아니다 — 지우고 다시 받는다. 진짜 파일은 그대로 건너뛴다.
                #   (0 바이트와 작은 .mp4 만 연다 — 매 판 수천 장의 사진을 NAS 에서 하나씩 열지 않게)
                try:
                    _sz = os.path.getsize(filepath)
                    _bad = _sz == 0
                    if not _bad and _sz < 65536 and filepath.lower().endswith('.mp4'):
                        with open(filepath, 'rb') as _f:
                            _bad = _f.read(7) == b'#EXTM3U'
                except OSError:
                    _bad = False
                if not _bad:
                    return filepath
                try:
                    os.remove(filepath)
                except OSError:
                    return filepath
            # ★ 받는 동안은 .part 에 쓰고 다 받은 뒤에만 진짜 이름으로 바꾼다 — 중간에 죽어도 잘린
            #   파일이 진짜 이름으로 남아 위 exists() 검사에 영원히 걸리는 일이 없다.
            part = filepath + '.part'
            try:
                try:
                    # 앞 판(중지 = 데몬 SIGKILL)이 남긴 조각. 그 판의 ffmpeg 는 고아로 아직 쓰고 있을 수 있다 —
                    #   먼저 지워 두면 그 쓰기는 이름 없는 옛 파일로 가고 새 파일과 섞이지 않는다.
                    os.remove(part)
                except OSError:
                    pass
                try:
                    if '.m3u8' in url:
                        # 블루스카이 영상은 HLS 다. 재생목록을 저장하지 않고 ffmpeg 로 이어 붙여 mp4 로 만든다.
                        # ★ ffmpeg 는 http:// 프록시(http_proxy)만 쓰고 SOCKS 는 모른다. 앱이 socks5:// 를 걸어 둔 채로
                        #   부르면 프록시를 건너뛰고 이 컴퓨터 회선으로 직접 나간다(앱의 '닫힌 길' socks5://127.0.0.1:9 도
                        #   무시된다). 그때는 받지 않는다 — 실패로 남겨 다음 판(프록시를 바꾼 뒤)에 다시 본다.
                        _pk = ('http_proxy', 'HTTP_PROXY', 'https_proxy', 'HTTPS_PROXY', 'all_proxy', 'ALL_PROXY')
                        _anyp = any(os.environ.get(k) for k in _pk)
                        if _anyp and not os.environ.get('http_proxy', '').lower().startswith('http://'):
                            raise RuntimeError('프록시가 HTTP 가 아니라 HLS 영상은 받지 않습니다'
                                               '(ffmpeg 는 SOCKS 를 몰라 직접 연결로 샙니다)')
                        ff = shutil.which('ffmpeg')
                        if not ff:
                            raise RuntimeError('ffmpeg 를 찾지 못해 HLS 영상을 받을 수 없습니다')
                        r = subprocess.run([ff, '-nostdin', '-hide_banner', '-loglevel', 'error', '-y',
                                            '-i', url, '-c', 'copy', '-f', 'mp4', part],
                                           stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, timeout=1800)
                        if r.returncode != 0 or not os.path.exists(part) or os.path.getsize(part) == 0:
                            raise RuntimeError('ffmpeg 실패: ' + (r.stderr or b'').decode('utf-8', 'replace')[-300:])
                    else:
                        urllib.request.urlretrieve(url, part)
                    os.replace(part, filepath)
                except BaseException:
                    try:
                        os.remove(part)
                    except OSError:
                        pass
                    raise
                count += 1
                # Set file date
                if post_data and len(post_data) > 4 and post_data[4]:
                    try:
                        from datetime import timedelta
                        dt = datetime.strptime(post_data[4], '%Y/%m/%d %H:%M:%S')
                        dt = dt.replace(tzinfo=timezone(timedelta(hours=9)))
                        ts = dt.timestamp()
                        os.utime(filepath, (ts, ts))
                        # macOS creation date
                        mac_date = dt.strftime('%m/%d/%Y %H:%M:%S')
                        # ★ os.system + f-string 금지 — filepath 에 " $ ` \ 가 있으면 셸 주입.
                        #   argv 로 직접 실행(셸 미경유). SetFile 은 legacy 라 없으면 조용히 스킵.
                        if shutil.which('SetFile'):
                            subprocess.run(['SetFile', '-d', mac_date, filepath],
                                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
                    except Exception:
                        pass
                # macOS xattr: kMDItemWhereFroms (소스 URL 메타데이터)
                post_url_for_xattr = ''
                if post_data and len(post_data) > 1:
                    post_url_for_xattr = str(post_data[1]) if post_data[1] else ''
                if not post_url_for_xattr and uri:
                    # fallback: construct from URI
                    post_url_for_xattr = f"https://bsky.app/profile/{author_handle}/post/{post_id}" if author_handle else ''
                if post_url_for_xattr and sys.platform == 'darwin':
                    try:
                        import plistlib
                        plist_data = plistlib.dumps([post_url_for_xattr, url], fmt=plistlib.FMT_XML)
                        import ctypes, ctypes.util
                        libc = ctypes.CDLL(ctypes.util.find_library('c'))
                        libc.setxattr(
                            filepath.encode('utf-8'),
                            b'com.apple.metadata:kMDItemWhereFroms',
                            plist_data, len(plist_data), 0, 0
                        )
                    except:
                        pass
                # EXIF metadata (for JPG)
                if filepath.lower().endswith(('.jpg', '.jpeg')):
                    try:
                        add_exif(filepath, post_data)
                    except:
                        pass
                # Finder comment (all files including JPG)
                try:
                    add_finder_comment(filepath, post_data)
                except:
                    pass
                # Mirror to _complete folder
                try:
                    complete_path = os.path.join(complete_dir, filename)
                    # 크기가 다르면(예전의 m3u8 텍스트 .mp4 같은 망가진 사본) 새로 받은 것으로 바꾼다
                    if not os.path.exists(complete_path) or os.path.getsize(complete_path) != os.path.getsize(filepath):
                        shutil.copy2(filepath, complete_path)
                        # Apply Finder comment + xattr to _complete copy
                        try:
                            add_finder_comment(complete_path, post_data)
                        except:
                            pass
                        if post_url_for_xattr and sys.platform == 'darwin':
                            try:
                                import plistlib
                                plist_data = plistlib.dumps([post_url_for_xattr, url], fmt=plistlib.FMT_XML)
                                import ctypes, ctypes.util
                                libc = ctypes.CDLL(ctypes.util.find_library('c'))
                                libc.setxattr(
                                    complete_path.encode('utf-8'),
                                    b'com.apple.metadata:kMDItemWhereFroms',
                                    plist_data, len(plist_data), 0, 0
                                )
                            except:
                                pass
                except:
                    pass
                return filepath
            except Exception as e:
                log(f"Download failed: {e}")
                dl_failed.add(str(getattr(post, 'uri', '') or ''))
                return None

        # Images
        images = getattr(embed, 'images', None)
        if images:
            for i, img in enumerate(images):
                url = pick(img, ['fullsize', 'thumb', 'url', 'image'], '')
                if url:
                    ext = '.jpg'
                    if '.png' in url: ext = '.png'
                    elif '.gif' in url: ext = '.gif'
                    elif '.webp' in url: ext = '.webp'
                    dl(url, make_filename(str(i) if len(images) > 1 else '', ext))

        # Video
        video = getattr(embed, 'video', None)
        if video:
            playlist = pick(video, ['playlist', 'url', 'cid'], '')
            if playlist:
                dl(playlist, make_filename('video', '.mp4'))

        # Record with media
        rmedia = getattr(embed, 'media', None)
        if rmedia:
            rimages = getattr(rmedia, 'images', None)
            if rimages:
                for i, img in enumerate(rimages):
                    url = pick(img, ['fullsize', 'thumb', 'url', 'image'], '')
                    if url:
                        ext = '.jpg'
                        if '.png' in url: ext = '.png'
                        dl(url, make_filename(f'qm{i}', ext))

        # External thumb
        ext_embed = getattr(embed, 'external', None)
        if ext_embed:
            thumb = getattr(ext_embed, 'thumb', '')
            if thumb:
                dl(thumb, make_filename('thumb', '.jpg'))

        return count

    # EXIF metadata
    def add_exif(filepath, post_data):
        # ★ 예전엔 piexif 를 썼다. 최신 배포가 2019-07 로 7년째 멈춰 있는데,
        #   우리가 쓰는 것은 태그 여섯 개뿐이었다. EXIF 는 바깥 서비스가 아니라
        #   1998년에 굳은 파일 규격이라 따라다닐 일이 없다 — 직접 쓴다.
        #   (tools/exif_tags.py · exiftool 로 되읽어 검증했다)
        try:
            import exif_tags

            author = post_data[5] if len(post_data) > 5 else ''
            text = (post_data[2] if len(post_data) > 2 else '')[:200]
            url = post_data[1] if len(post_data) > 1 else ''
            date_str = post_data[4] if len(post_data) > 4 else ''

            kw = {
                "artist": f"@{author}",
                "description": text,
                "copyright": f"Bluesky @{author}",
            }
            if date_str:
                exif_date = date_str.replace('/', ':')[:19]
                kw["datetime"] = exif_date
                kw["datetime_original"] = exif_date
            if url:
                kw["user_comment"] = url

            with open(filepath, 'rb') as f:
                raw = f.read()
            out = exif_tags.set_tags(raw, **kw)
            # ★ 원본을 곧바로 덮지 않는다. 쓰다 실패하면 사진이 깨진 채로 남는다.
            tmp = filepath + '.exif_tmp'
            with open(tmp, 'wb') as f:
                f.write(out)
            os.replace(tmp, filepath)
        except ImportError:
            # Fallback: Finder comment
            add_finder_comment(filepath, post_data)
        except Exception:
            try:
                if os.path.exists(filepath + '.exif_tmp'):
                    os.remove(filepath + '.exif_tmp')
            except Exception:
                pass

    # Finder comment (macOS)
    def add_finder_comment(filepath, post_data):
        try:
            import subprocess
            author = post_data[5] if len(post_data) > 5 else ''
            text = (post_data[2] if len(post_data) > 2 else '')[:100]
            url = post_data[1] if len(post_data) > 1 else ''
            comment = f"@{author}: {text}\\n{url}"
            subprocess.run(['osascript', '-e',
                f'tell application "Finder" to set comment of (POSIX file "{filepath}" as alias) to "{comment}"'],
                capture_output=True, timeout=5)
        except:
            pass

    # Save Excel
    def save_xlsx(filename, data, headers):
        # ★ 기존 파일과 합친다(uri 열 기준, type 열이 있으면 (type, uri) — 제 글을 제가 리포스트하면 같은 uri 로
        #   Post·Repost 두 줄이 된다). 예전엔 이번 판 행만으로 새로 써서, 중지·'최대 수집 수'·内閣会 폴링(count=10)
        #   판이 몇 천 행짜리 엑셀을 몇 행으로 덮어썼다. 옛 행은 '열 이름' 으로 맞춰 옮긴다(중간 저장본은 14열,
        #   최종본은 15열 — 옛 판 파일의 열 순서가 달라도 섞이지 않게). 부르는 쪽 목록·헤더는 건드리지 않는다.
        #   팔로워·팔로잉·차단처럼 uri 열이 없는 '그때의 명단' 은 예전처럼 새로 쓴다.
        data = list(data)
        if 'uri' in headers and os.path.exists(filename):
            try:
                _owb = openpyxl.load_workbook(filename, read_only=True)
                try:
                    _old = list(_owb.active.iter_rows(values_only=True))
                finally:
                    _owb.close()
                _oh = [str(h) if h is not None else '' for h in (_old[0] if _old else ())]
                if 'uri' in _oh:
                    # 옛 파일에만 있는 열(예: 최종본의 repost_time — 중간 저장 헤더에는 빠져 있다)은 뒤에 붙여 지킨다
                    headers = list(headers) + [h for h in _oh if h and h not in headers]
                    _pos = [(_oh.index(h) if h in _oh else -1) for h in headers]
                    _ti = headers.index('type') if 'type' in headers else -1
                    _ui = headers.index('uri')

                    def _key(r):
                        t = r[_ti] if 0 <= _ti < len(r) else None
                        u = r[_ui] if _ui < len(r) else None
                        return ('' if t is None else str(t), '' if u is None else str(u))

                    seen = {_key(r) for r in data}
                    for orow in _old[1:]:
                        if not orow:
                            continue
                        r = [(orow[p] if 0 <= p < len(orow) else None) for p in _pos]
                        k = _key(r)
                        if not k[1] or k in seen:
                            continue
                        seen.add(k)
                        data.append(r)
            except Exception as e:
                # 못 읽는 옛 파일은 지우지 않고 옆에 남긴다
                try:
                    shutil.copy2(filename, filename + f'.unreadable_{int(time.time())}')
                except Exception:
                    pass
                log(f"기존 Excel 을 읽지 못해 합치지 못했습니다(원본은 .unreadable_* 로 남김): {e}")
        try:
            wb = openpyxl.Workbook()
            ws = wb.active
            ws.title = "Posts"

            header_font = Font(bold=True, color="FFFFFF")
            header_fill = PatternFill(start_color="0085FF", end_color="0085FF", fill_type="solid")
            for col, h in enumerate(headers, 1):
                cell = ws.cell(row=1, column=col, value=h)
                cell.font = header_font
                cell.fill = header_fill

            # Sort by created_at desc
            try:
                data.sort(key=lambda x: x[4] if len(x) > 4 else '', reverse=True)
            except:
                pass

            for row_idx, row_data in enumerate(data, 2):
                for col_idx, val in enumerate(row_data, 1):
                    ws.cell(row=row_idx, column=col_idx, value=val)

            # 옆 이름에 다 쓴 뒤 한 번에 바꾼다 — 쓰는 도중 죽어도 기존 파일은 멀쩡하다.
            _tmp = filename + '.part'
            wb.save(_tmp)
            os.replace(_tmp, filename)
            return True
        except Exception as e:
            log(f"Excel save error: {e}")
            # Fallback: TSV
            try:
                with open(filename.replace('.xlsx', '.txt'), 'w') as f:
                    f.write('\t'.join(headers) + '\n')
                    for row in data:
                        f.write('\t'.join(str(v) for v in row) + '\n')
                return True
            except:
                return False

    # ═══ Collection handlers ═══

    # ★ 최대 수집 수(화면의 '최대 수집 수', 0 = 전체). 명령마다 max_count 로 온다.
    #   예전엔 C++ 이 이 값을 넘기지 않아 5 를 넣어도 전체 모드 7종을 끝까지 받았다
    #   (실측 2026-09-15: 10분에 465파일). 목록마다 '이번에 담은 개수' 로 센다.
    cap_limit = 0
    def over_cap(n):
        return cap_limit > 0 and n >= cap_limit

    def apply_rl_settings(args):
        """커맨드에서 rate limit 설정 적용"""
        mode = args.get("rl_mode", "wait")
        mins = args.get("rl_wait_mins", 15)
        set_rate_limit_mode(mode, mins)

    def collect_posts(args):
        apply_rl_settings(args)
        target = args.get("target", "")
        save_path = args.get("save_path", ".")
        do_media = args.get("download_media", True)
        do_exif = args.get("exif", True)
        # ★ 화면의 '답글 포함'·'리포스트 포함'. 없으면 예전처럼 다 받는다.
        inc_replies = args.get("include_replies", True)
        inc_reposts = args.get("include_reposts", True)

        short_handle, api_handle = norm_handle(target)
        user_dir = os.path.join(save_path, "bluesky", short_handle)
        media_dir = os.path.join(user_dir, "media")
        excel_dir = os.path.join(user_dir, "excel")
        os.makedirs(media_dir, exist_ok=True)
        os.makedirs(excel_dir, exist_ok=True)

        log(f"Target: @{short_handle}")
        log(f"Save: {user_dir}")
        # 이미 받은 글은 이번 판을 시작하기 '전' 의 엑셀로 정한다(중간 저장이 같은 파일에 쓰므로 지금 찍어 둔다)
        # 표시 이름에 범위를 넣는다 — 미디어를 끄고 다 본 것이 미디어를 켠 판을 '다 봤다' 로 만들지 않게(검토)
        walk_kind = f"bs_posts_m{int(bool(do_media))}_p{int(bool(inc_replies))}_r{int(bool(inc_reposts))}"
        ks = None
        clean = False   # 끊김 없이 끝났나(피드 끝 · 일찍 멈춤)
        capped = False
        target_did = ''
        api_error = ''

        # Get profile info
        try:
            profile = client.get_profile(actor=api_handle)
            target_did = getattr(profile, 'did', '') or ''
            posts_count = getattr(profile, 'posts_count', 0) or 0
            followers_count = getattr(profile, 'followers_count', 0) or 0
            follows_count = getattr(profile, 'follows_count', 0) or 0
            log(f"Posts: {posts_count} | Followers: {followers_count} | Following: {follows_count}")

            # Save profile info
            save_profile(user_dir, profile, short_handle)

            # Download avatar & banner (with archive on change)
            avatar_url = getattr(profile, 'avatar', '')
            banner_url = getattr(profile, 'banner', '')
            import urllib.request, hashlib
            def archive_download(url, path):
                """Download image, archive old version if changed."""
                try:
                    tmp = path + ".tmp"
                    urllib.request.urlretrieve(url, tmp)
                    if os.path.exists(path):
                        # Compare MD5
                        with open(path, 'rb') as f: old_md5 = hashlib.md5(f.read()).hexdigest()
                        with open(tmp, 'rb') as f: new_md5 = hashlib.md5(f.read()).hexdigest()
                        if old_md5 == new_md5:
                            os.remove(tmp)
                            return  # Same file
                        # Archive old version
                        from datetime import datetime
                        date_str = datetime.now().strftime("%Y-%m-%d")
                        archive_dir = os.path.join(os.path.dirname(path), "archive", date_str)
                        os.makedirs(archive_dir, exist_ok=True)
                        os.rename(path, os.path.join(archive_dir, os.path.basename(path)))
                        os.rename(tmp, path)
                        log(f"Profile changed → archived to {date_str}/")
                    else:
                        os.rename(tmp, path)
                except Exception as e:
                    if os.path.exists(tmp): os.remove(tmp)
            # Date-based profile directory
            from datetime import datetime as _dt_main
            _main_pdir = os.path.join(user_dir, "profiles", _dt_main.now().strftime("%Y-%m-%d"))
            os.makedirs(_main_pdir, exist_ok=True)
            if avatar_url:
                _ap = os.path.join(_main_pdir, f"{short_handle}_avatar.jpg")
                if not os.path.exists(_ap):
                    try: urllib.request.urlretrieve(avatar_url, _ap)
                    except: pass
            if banner_url:
                _bp = os.path.join(_main_pdir, f"{short_handle}_banner.jpg")
                if not os.path.exists(_bp):
                    try: urllib.request.urlretrieve(banner_url, _bp)
                    except: pass
        except Exception as e:
            log(f"Profile error: {e}")
            return {"error": str(e)}

        # 프로필을 받은 뒤에 — 받지 못하고 돌아가면 표시만 지워 다음 판이 괜히 끝까지 보았다
        armed = walk_kind in take_walk_marks(excel_dir, "bs_posts_")   # 다른 범위의 표시도 함께 지운다
        ks = KnownStreak(os.path.join(excel_dir, f"{short_handle}_posts.xlsx"), args.get("stop_after_known", 0) if armed else 0)
        dl_failed.clear()
        prev_failed = take_walk_failures(excel_dir, walk_kind)
        sak = max(0, int(args.get("stop_after_known", 0) or 0))
        def hit():   # 최대 수 — 일찍 멈춤을 쓰면 새로 받은 것만 센다(받아 둔 것까지 세면 늘 최대 수에서 끊겨 표시가 영영 안 적혔다 — 검토)
            return cap_limit > 0 and ((ks.new if sak > 0 else len(all_data)) >= cap_limit)

        # Collect all posts
        all_data = []
        cursor = None
        total_media = 0
        page = 0

        while True:
            if stop_requested or check_stdin_for_quit():
                log("⛔ 중지 요청 감지 — 포스트 수집 중단")
                break
            if hit():
                log(f"최대 수집 수 {cap_limit}개에 닿아 멈춤")
                capped = True
                break
            try:
                resp = client.get_author_feed(
                    actor=api_handle, limit=100, cursor=cursor,
                    # 답글을 빼면 서버에서 걸러 온다 — 받아서 버리는 것보다 빠르고
                    # 최대 수집 수도 '실제로 받은 것' 기준으로 맞는다.
                    filter=('posts_and_author_threads' if inc_replies else 'posts_no_replies')
                )
            except Exception as e:
                err = str(e).lower()
                if 'rate' in err or '429' in err:
                    # 중간 저장 (AINU 방식)
                    if all_data:
                        _h = ["type", "uri", "post_url", "text", "language", "created_at",
                              "author_handle", "author_name", "likes", "reposts", "replies",
                              "has_media", "media_count", "media_urls"]
                        save_xlsx(os.path.join(excel_dir, f"{short_handle}_posts.xlsx"), all_data, _h)
                        log(f"💾 중간 저장: {len(all_data)}건")
                    if not handle_rate_limit():
                        break
                    continue
                log(f"API error: {e}")
                api_error = str(e)
                break

            feed = pick_list(resp, ['feed', 'posts', 'items'], 'feed')
            if not feed:
                clean = True
                break

            for item in feed:
                if hit(): break
                post = pick(item, ['post', 'value', 'record'])
                if not post:
                    continue

                # Check if repost
                is_repost = hasattr(item, 'reason') and item.reason is not None
                # 리포스트를 빼기로 했으면 여기서 건너뛴다(서버 필터로는 못 뺀다).
                if is_repost and not inc_reposts:
                    continue

                data = process_post(post)
                if not data:
                    continue

                if is_repost:
                    data.insert(0, 'Repost')
                    # repost_time: when the repost happened (from reason.indexed_at)
                    reason = getattr(item, 'reason', None)
                    repost_time = ''
                    if reason:
                        indexed = getattr(reason, 'indexed_at', '') or getattr(reason, 'created_at', '')
                        if indexed:
                            try:
                                from datetime import timedelta
                                rdt = datetime.fromisoformat(str(indexed).replace('Z', '+00:00'))
                                jst = rdt.astimezone(timezone(timedelta(hours=9)))
                                repost_time = jst.strftime('%Y/%m/%d %H:%M:%S')
                            except:
                                repost_time = str(indexed)
                    data.append(repost_time)
                else:
                    data.insert(0, 'Post')
                    data.append('')  # no repost_time for original posts

                all_data.append(data)

                # Log each post like Twitter does
                ptype = data[0]  # Post or Repost
                pdate = data[5] if len(data) > 5 else ''  # created_at
                ptext = (data[3] if len(data) > 3 else '')[:80]  # text preview
                pauthor = data[6] if len(data) > 6 else ''  # author_handle
                pmedia = data[12] if len(data) > 12 else 0  # media_count
                plikes = data[8] if len(data) > 8 else 0  # likes
                media_tag = f" 📎{pmedia}" if pmedia else ""
                rt_tag = f" ♻️@{pauthor}" if ptype == 'Repost' else ""
                log(f"[{len(all_data)}] {pdate} ❤{plikes}{media_tag}{rt_tag} {ptext}")

                if do_media:
                    new_media = download_media_files(post, media_dir, data[1:], action_type="posts")  # skip type column
                    total_media += new_media

                if not is_repost:
                    ks.own(post, target_did)   # 그 계정이 마지막으로 쓴 때 — 뜸한 계정 판정
                if ks.see(data[0], data[1]):
                    break

            page += 1
            progress(len(all_data), total_media, fresh=ks.new, newest=ks.newest)
            if ks.stopped:
                log(f"이미 받은 글이 {ks.stopped}개 이어져 여기서 멈춥니다 — 새 글 {ks.new}개")
                clean = True
                break

            cursor = pick(resp, ['cursor', 'next_cursor', 'nextCursor'])
            if not cursor:
                clean = True   # 피드 끝
                break

            # 아이누 방식 진행 로그 + 중간저장
            n = len(all_data)
            if n > 0 and n % 1000 == 0:
                headers = ["type", "uri", "post_url", "text", "language", "created_at",
                          "author_handle", "author_name", "likes", "reposts", "replies",
                          "has_media", "media_count", "media_urls"]
                save_xlsx(os.path.join(excel_dir, f"{short_handle}_posts.xlsx"), all_data, headers)
                log(f"💾 중간 저장: {n}개")
                time.sleep(5)
            elif n > 0 and n % 500 == 0:
                log(f"📊 진행: {n}개, 미디어 {total_media}개")
                time.sleep(2)
            else:
                time.sleep(1)  # 아이누: 기본 1초

        # Final save
        if all_data:
            headers = ["type", "uri", "post_url", "text", "language", "created_at",
                      "author_handle", "author_name", "likes", "reposts", "replies",
                      "has_media", "media_count", "media_urls", "repost_time"]
            save_xlsx(os.path.join(excel_dir, f"{short_handle}_posts.xlsx"), all_data, headers)
            log(f"Excel saved: {len(all_data)} posts")

        log(f"Complete! Posts: {len(all_data)}, Media: {total_media}")
        progress(len(all_data), total_media, "완료", fresh=ks.new, newest=ks.newest)
        dl_blocked = finish_failures(excel_dir, walk_kind, prev_failed, ks, sak)
        if clean and not api_error and not capped and not hit() and not dl_blocked:
            put_walk_mark(excel_dir, walk_kind)
        res = {"status": "ok", "posts": len(all_data), "media": total_media, **ks.fields()}
        if api_error:
            res["api_error"] = api_error   # 앱이 오류로 적는다(오류 난 것만 다시) — 받은 데까지는 그대로 낸다
        return res

    def collect_likes(args):
        apply_rl_settings(args)
        target = args.get("target", "")
        save_path = args.get("save_path", ".")
        do_media = args.get("download_media", True)

        short_handle, api_handle = norm_handle(target)
        user_dir = os.path.join(save_path, "bluesky", short_handle)
        media_dir = os.path.join(user_dir, "media")
        excel_dir = os.path.join(user_dir, "excel")
        os.makedirs(media_dir, exist_ok=True)
        os.makedirs(excel_dir, exist_ok=True)

        log(f"Collecting likes for @{short_handle}")
        walk_kind = f"bs_likes_m{int(bool(do_media))}"
        clean = False
        api_error = ''

        try:
            profile = client.get_profile(actor=api_handle)
            user_did = profile.did
        except Exception as e:
            return {"error": str(e)}
        # ★ 좋아요는 그 계정 자신만 볼 수 있다(getActorLikes 는 actor 가 묻는 계정이어야 한다). 남의 계정이면 묻지 않고 넘어간다 —
        #   묻던 때는 늘 거절당해, '전체' 로 받는 대상 목록이 블루스카이 대상을 모두 ✕ 로 적었다(검토). 로그인한 계정이 여럿이면
        #   그 대상 자신으로 로그인한 계정으로 묻는다. 계정 정보(me)를 모르는 옛 꾸러미면 예전처럼 지금 계정으로 묻는다.
        my_dids = [getattr(getattr(c, 'me', None), 'did', '') or '' for c in clients]
        likes_client = client
        if any(my_dids):
            if user_did not in my_dids:
                log("좋아요는 로그인한 그 계정 것만 볼 수 있어 건너뜁니다")
                return {"status": "ok", "count": 0, "media": 0}
            likes_client = clients[my_dids.index(user_did)]
        armed = walk_kind in take_walk_marks(excel_dir, "bs_likes_")
        ks = KnownStreak(os.path.join(excel_dir, f"{short_handle}_likes.xlsx"), args.get("stop_after_known", 0) if armed else 0)
        dl_failed.clear()
        prev_failed = take_walk_failures(excel_dir, walk_kind)
        sak = max(0, int(args.get("stop_after_known", 0) or 0))
        def hit():   # 최대 수 — 일찍 멈춤을 쓰면 새로 받은 것만 센다(받아 둔 것까지 세면 늘 최대 수에서 끊겨 표시가 영영 안 적혔다 — 검토)
            return cap_limit > 0 and ((ks.new if sak > 0 else len(all_data)) >= cap_limit)

        all_data = []
        cursor = None
        total_media = 0

        while True:
            if stop_requested or check_stdin_for_quit():
                log("⛔ 중지 요청 감지 — 좋아요 수집 중단")
                break
            if hit():
                log(f"최대 수집 수 {cap_limit}개에 닿아 멈춤")
                break
            try:
                params = {'actor': user_did, 'limit': 100}
                if cursor:
                    params['cursor'] = cursor
                resp = likes_client.app.bsky.feed.get_actor_likes(params)
            except Exception as e:
                err = str(e).lower()
                if 'rate' in err or '429' in err:
                    if not handle_rate_limit():
                        break
                    continue
                log(f"API error: {e}")
                api_error = str(e)
                break

            feed = pick_list(resp, ['feed', 'posts', 'items'], 'feed')
            if not feed:
                clean = True   # 피드 끝
                break

            for item in feed:
                if hit(): break
                post = pick(item, ['post', 'value', 'record'])
                if not post:
                    continue
                data = process_post(post)
                if data:
                    all_data.append(data)

                    # Log each liked post
                    pdate = data[4] if len(data) > 4 else ''
                    ptext = (data[2] if len(data) > 2 else '')[:80]
                    pauthor = data[5] if len(data) > 5 else ''
                    pmedia = data[11] if len(data) > 11 else 0
                    plikes = data[7] if len(data) > 7 else 0
                    media_tag = f" 📎{pmedia}" if pmedia else ""
                    log(f"[{len(all_data)}] {pdate} @{pauthor} ❤{plikes}{media_tag} {ptext}")

                    if do_media:
                        new_media = download_media_files(post, media_dir, data, action_type="likes")
                        total_media += new_media
                    if ks.see(None, data[0]):   # 받을 것은 받은 뒤에 센다(멈추는 좋아요의 미디어도 확인)
                        break

            progress(len(all_data), total_media, fresh=ks.new)
            if ks.stopped:
                log(f"이미 받은 좋아요가 {ks.stopped}개 이어져 여기서 멈춥니다 — 새로 {ks.new}개")
                clean = True
                break

            cursor = pick(resp, ['cursor', 'next_cursor', 'nextCursor'])
            if not cursor:
                clean = True
                break
            time.sleep(2)  # 아이누: 2초 고정

        if all_data:
            headers = ["uri", "post_url", "text", "language", "created_at",
                      "author_handle", "author_name", "likes", "reposts", "replies",
                      "has_media", "media_count", "media_urls", "repost_time"]
            save_xlsx(os.path.join(excel_dir, f"{short_handle}_likes.xlsx"), all_data, headers)
            log(f"Excel saved: {len(all_data)} likes")

        dl_blocked = finish_failures(excel_dir, walk_kind, prev_failed, ks, sak)
        if clean and not api_error and not hit() and not dl_blocked:
            put_walk_mark(excel_dir, walk_kind)
        res = {"status": "ok", "count": len(all_data), "media": total_media, **ks.fields()}
        res.pop("newest", None)   # 남의 글 — 그 계정이 얼마나 자주 쓰는지와 상관없다
        if api_error:
            res["api_error"] = api_error
        return res

    def collect_followers(args):
        apply_rl_settings(args)
        target = args.get("target", "")
        save_path = args.get("save_path", ".")

        short_handle, api_handle = norm_handle(target)
        user_dir = os.path.join(save_path, "bluesky", short_handle)
        profile_dir = os.path.join(user_dir, "profiles")
        excel_dir = os.path.join(user_dir, "excel")
        from datetime import datetime as _dt
        _date_dir = os.path.join(profile_dir, _dt.now().strftime("%Y-%m-%d"))
        os.makedirs(_date_dir, exist_ok=True)
        os.makedirs(excel_dir, exist_ok=True)

        log(f"Collecting followers for @{short_handle}")

        all_data = []
        cursor = None
        import urllib.request

        while True:
            if stop_requested or check_stdin_for_quit():
                log("⛔ 중지 요청 감지 — 팔로워 수집 중단")
                break
            if over_cap(len(all_data)):
                log(f"최대 수집 수 {cap_limit}개에 닿아 멈춤")
                break
            try:
                resp = client.get_followers(actor=api_handle, limit=100, cursor=cursor)
            except Exception as e:
                err = str(e).lower()
                if 'rate' in err or '429' in err:
                    if not handle_rate_limit():
                        break
                    continue
                log(f"API error: {e}")
                break

            followers = pick_list(resp, ['followers', 'subjects', 'items'], 'followers')
            if not followers:
                break

            for f in followers:
                if over_cap(len(all_data)): break
                did = getattr(f, 'did', '')
                fhandle = getattr(f, 'handle', '')
                name = getattr(f, 'display_name', '')
                desc = getattr(f, 'description', '') or ''
                fc = getattr(f, 'followers_count', 0) or 0
                fwc = getattr(f, 'follows_count', 0) or 0
                pc = getattr(f, 'posts_count', 0) or 0
                avatar = getattr(f, 'avatar', '')
                banner = getattr(f, 'banner', '')

                all_data.append([did, fhandle, name, desc[:300], fc, fwc, pc, avatar or '', banner or ''])
                log(f"[{len(all_data)}] @{fhandle} ({name}) 팔로워:{fc} 팔로잉:{fwc} 포스트:{pc}")

                # Download profile pic & banner (date-based folder)
                if avatar:
                    p = os.path.join(_date_dir, f"{fhandle}_avatar.jpg")
                    if not os.path.exists(p):
                        try: urllib.request.urlretrieve(avatar, p)
                        except: pass
                if banner:
                    p = os.path.join(_date_dir, f"{fhandle}_banner.jpg")
                    if not os.path.exists(p):
                        try: urllib.request.urlretrieve(banner, p)
                        except: pass

            progress(len(all_data), 0)

            cursor = pick(resp, ['cursor', 'next_cursor', 'nextCursor'])
            if not cursor:
                break
            time.sleep(2)  # 아이누: 2초 고정

        if all_data:
            headers = ["DID", "Handle", "Name", "Description", "Followers", "Following", "Posts", "Avatar URL", "Banner URL"]
            save_xlsx(os.path.join(excel_dir, f"{short_handle}_followers.xlsx"), all_data, headers)
            log(f"Excel saved: {len(all_data)} followers")

        return {"status": "ok", "count": len(all_data)}

    def collect_following(args):
        apply_rl_settings(args)
        target = args.get("target", "")
        save_path = args.get("save_path", ".")

        short_handle, api_handle = norm_handle(target)
        user_dir = os.path.join(save_path, "bluesky", short_handle)
        profile_dir = os.path.join(user_dir, "profiles")
        excel_dir = os.path.join(user_dir, "excel")
        from datetime import datetime as _dt
        _date_dir = os.path.join(profile_dir, _dt.now().strftime("%Y-%m-%d"))
        os.makedirs(_date_dir, exist_ok=True)
        os.makedirs(excel_dir, exist_ok=True)

        log(f"Collecting following for @{short_handle}")

        all_data = []
        cursor = None
        import urllib.request

        while True:
            if stop_requested or check_stdin_for_quit():
                log("⛔ 중지 요청 감지 — 팔로잉 수집 중단")
                break
            if over_cap(len(all_data)):
                log(f"최대 수집 수 {cap_limit}개에 닿아 멈춤")
                break
            try:
                resp = client.get_follows(actor=api_handle, limit=100, cursor=cursor)
            except Exception as e:
                err = str(e).lower()
                if 'rate' in err or '429' in err:
                    if not handle_rate_limit():
                        break
                    continue
                log(f"API error: {e}")
                break

            follows = pick_list(resp, ['follows', 'subjects', 'items'], 'follows')
            if not follows:
                break

            for f in follows:
                if over_cap(len(all_data)): break
                did = getattr(f, 'did', '')
                fhandle = getattr(f, 'handle', '')
                name = getattr(f, 'display_name', '')
                desc = getattr(f, 'description', '') or ''
                fc = getattr(f, 'followers_count', 0) or 0
                fwc = getattr(f, 'follows_count', 0) or 0
                pc = getattr(f, 'posts_count', 0) or 0
                avatar = getattr(f, 'avatar', '')
                banner = getattr(f, 'banner', '')

                all_data.append([did, fhandle, name, desc[:300], fc, fwc, pc, avatar or '', banner or ''])
                log(f"[{len(all_data)}] @{fhandle} ({name}) 팔로워:{fc} 팔로잉:{fwc} 포스트:{pc}")

                if avatar:
                    p = os.path.join(_date_dir, f"{fhandle}_avatar.jpg")
                    if not os.path.exists(p):
                        try: urllib.request.urlretrieve(avatar, p)
                        except: pass
                if banner:
                    p = os.path.join(_date_dir, f"{fhandle}_banner.jpg")
                    if not os.path.exists(p):
                        try: urllib.request.urlretrieve(banner, p)
                        except: pass

            progress(len(all_data), 0)

            cursor = pick(resp, ['cursor', 'next_cursor', 'nextCursor'])
            if not cursor:
                break
            time.sleep(2)  # 아이누: 2초 고정

        if all_data:
            headers = ["DID", "Handle", "Name", "Description", "Followers", "Following", "Posts", "Avatar URL", "Banner URL"]
            save_xlsx(os.path.join(excel_dir, f"{short_handle}_following.xlsx"), all_data, headers)
            log(f"Excel saved: {len(all_data)} following")

        return {"status": "ok", "count": len(all_data)}

    def collect_blocks(args):
        save_path = args.get("save_path", ".")
        user_dir = os.path.join(save_path, "bluesky", "_blocks")
        excel_dir = os.path.join(user_dir, "excel")
        os.makedirs(user_dir, exist_ok=True)
        os.makedirs(excel_dir, exist_ok=True)

        all_data = []
        cursor = None

        while True:
            if stop_requested or check_stdin_for_quit():
                log("⛔ 중지 요청 감지 — 차단 목록 수집 중단")
                break
            if over_cap(len(all_data)):
                log(f"최대 수집 수 {cap_limit}개에 닿아 멈춤")
                break
            try:
                params = {'limit': 100}
                if cursor: params['cursor'] = cursor
                resp = client.app.bsky.graph.get_blocks(params)
            except Exception as e:
                log(f"API error: {e}")
                break

            blocks = getattr(resp, 'blocks', [])
            if not blocks: break

            for b in blocks:
                if over_cap(len(all_data)): break
                all_data.append([
                    getattr(b, 'did', ''),
                    getattr(b, 'handle', ''),
                    getattr(b, 'display_name', ''),
                    (getattr(b, 'description', '') or '')[:300],
                    getattr(b, 'avatar', '') or '',
                ])

            progress(len(all_data), 0)
            cursor = pick(resp, ['cursor', 'next_cursor', 'nextCursor'])
            if not cursor: break
            time.sleep(2)  # 아이누: 2초 고정

        if all_data:
            save_xlsx(os.path.join(excel_dir, "blocks.xlsx"), all_data,
                     ["DID", "Handle", "Name", "Description", "Avatar"])

        return {"status": "ok", "count": len(all_data)}

    def collect_mutes(args):
        save_path = args.get("save_path", ".")
        user_dir = os.path.join(save_path, "bluesky", "_mutes")
        excel_dir = os.path.join(user_dir, "excel")
        os.makedirs(user_dir, exist_ok=True)
        os.makedirs(excel_dir, exist_ok=True)

        all_data = []
        cursor = None

        while True:
            if stop_requested or check_stdin_for_quit():
                log("⛔ 중지 요청 감지 — 뮤트 목록 수집 중단")
                break
            if over_cap(len(all_data)):
                log(f"최대 수집 수 {cap_limit}개에 닿아 멈춤")
                break
            try:
                params = {'limit': 100}
                if cursor: params['cursor'] = cursor
                resp = client.app.bsky.graph.get_mutes(params)
            except Exception as e:
                log(f"API error: {e}")
                break

            mutes = getattr(resp, 'mutes', [])
            if not mutes: break

            for m in mutes:
                if over_cap(len(all_data)): break
                all_data.append([
                    getattr(m, 'did', ''),
                    getattr(m, 'handle', ''),
                    getattr(m, 'display_name', ''),
                    (getattr(m, 'description', '') or '')[:300],
                    getattr(m, 'avatar', '') or '',
                ])

            progress(len(all_data), 0)
            cursor = pick(resp, ['cursor', 'next_cursor', 'nextCursor'])
            if not cursor: break
            time.sleep(2)  # 아이누: 2초 고정

        if all_data:
            save_xlsx(os.path.join(excel_dir, "mutes.xlsx"), all_data,
                     ["DID", "Handle", "Name", "Description", "Avatar"])

        return {"status": "ok", "count": len(all_data)}

    def collect_search(args):
        apply_rl_settings(args)
        query = args.get("query", "")
        save_path = args.get("save_path", ".")
        do_media = args.get("download_media", True)

        safe_query = re.sub(r'[^\w\s-]', '', query)[:30]
        user_dir = os.path.join(save_path, "bluesky", f"_search_{safe_query}")
        media_dir = os.path.join(user_dir, "media")
        excel_dir = os.path.join(user_dir, "excel")
        os.makedirs(media_dir, exist_ok=True)
        os.makedirs(excel_dir, exist_ok=True)

        log(f"Searching: {query}")
        all_data = []
        cursor = None
        total_media = 0
        max_pages = 100

        for page in range(max_pages):
            if stop_requested or check_stdin_for_quit():
                log("⛔ 중지 요청 감지 — 검색 수집 중단")
                break
            if over_cap(len(all_data)):
                log(f"최대 수집 수 {cap_limit}개에 닿아 멈춤")
                break
            try:
                params = {'q': query, 'limit': 100}
                if cursor: params['cursor'] = cursor
                resp = client.app.bsky.feed.search_posts(params)
            except Exception as e:
                log(f"Search error: {e}")
                break

            posts = pick_list(resp, ['posts', 'feed', 'items'], 'searchPosts')
            if not posts: break

            for post in posts:
                if over_cap(len(all_data)): break
                data = process_post(post)
                if data:
                    all_data.append(data)
                    if do_media:
                        total_media += download_media_files(post, media_dir, data, action_type="search")

            progress(len(all_data), total_media)
            cursor = pick(resp, ['cursor', 'next_cursor', 'nextCursor'])
            if not cursor: break
            time.sleep(2)  # 아이누: 2초 고정

        if all_data:
            headers = ["uri", "post_url", "text", "language", "created_at",
                      "author_handle", "author_name", "likes", "reposts", "replies",
                      "has_media", "media_count", "media_urls", "repost_time"]
            save_xlsx(os.path.join(excel_dir, f"search_{safe_query}.xlsx"), all_data, headers)

        return {"status": "ok", "count": len(all_data), "media": total_media}

    def collect_notifications(args):
        save_path = args.get("save_path", ".")
        user_dir = os.path.join(save_path, "bluesky", "_notifications")
        excel_dir = os.path.join(user_dir, "excel")
        os.makedirs(user_dir, exist_ok=True)
        os.makedirs(excel_dir, exist_ok=True)

        all_data = []
        cursor = None

        for _ in range(50):
            if stop_requested or check_stdin_for_quit():
                log("⛔ 중지 요청 감지 — 알림 수집 중단")
                break
            if over_cap(len(all_data)):
                log(f"최대 수집 수 {cap_limit}개에 닿아 멈춤")
                break
            try:
                params = {'limit': 100}
                if cursor: params['cursor'] = cursor
                resp = client.app.bsky.notification.list_notifications(params)
            except Exception as e:
                log(f"API error: {e}")
                break

            notifs = getattr(resp, 'notifications', [])
            if not notifs: break

            for n in notifs:
                if over_cap(len(all_data)): break
                author = getattr(n, 'author', None)
                record = getattr(n, 'record', None)
                all_data.append([
                    getattr(n, 'uri', ''),
                    getattr(n, 'reason', ''),
                    str(getattr(n, 'is_read', '')),
                    getattr(author, 'handle', '') if author else '',
                    getattr(author, 'display_name', '') if author else '',
                    (getattr(record, 'text', '') if record else '')[:100],
                    getattr(n, 'indexed_at', ''),
                ])

            progress(len(all_data), 0)
            cursor = pick(resp, ['cursor', 'next_cursor', 'nextCursor'])
            if not cursor: break
            time.sleep(2)  # 아이누: 2초 고정

        if all_data:
            save_xlsx(os.path.join(excel_dir, "notifications.xlsx"), all_data,
                     ["URI", "Reason", "Read", "Author Handle", "Author Name", "Text", "Date"])

        return {"status": "ok", "count": len(all_data)}

    def collect_messages(args):
        save_path = args.get("save_path", ".")
        user_dir = os.path.join(save_path, "bluesky", "_messages")
        excel_dir = os.path.join(user_dir, "excel")
        os.makedirs(user_dir, exist_ok=True)
        os.makedirs(excel_dir, exist_ok=True)

        all_data = []
        try:
            resp = client.chat.bsky.convo.list_convos({'limit': 100})
            convos = getattr(resp, 'convos', [])
            for c in convos:
                if over_cap(len(all_data)): break   # 최대 수집 수 — 다른 종류와 같게(윈도우 be7c0b2 가 짚었다)
                members = getattr(c, 'members', [])
                handles = ', '.join(getattr(m, 'handle', '') for m in members)
                names = ', '.join(getattr(m, 'display_name', '') or '' for m in members)
                last_msg = getattr(c, 'last_message', None)
                text = ''
                if last_msg:
                    text = (getattr(last_msg, 'text', '') or '')[:100]
                all_data.append([
                    getattr(c, 'id', ''),
                    handles, names, text,
                    getattr(c, 'updated_at', '') or ''
                ])
        except Exception as e:
            log(f"Messages error: {e}")

        if all_data:
            save_xlsx(os.path.join(excel_dir, "messages.xlsx"), all_data,
                     ["Convo ID", "Members", "Names", "Last Message", "Updated At"])

        return {"status": "ok", "count": len(all_data)}

    def collect_profile(args):
        target = args.get("target", "")
        save_path = args.get("save_path", ".")

        short_handle, api_handle = norm_handle(target)
        user_dir = os.path.join(save_path, "bluesky", short_handle)
        excel_dir = os.path.join(user_dir, "excel")
        os.makedirs(user_dir, exist_ok=True)
        os.makedirs(excel_dir, exist_ok=True)

        try:
            profile = client.get_profile(actor=api_handle)
            save_profile(user_dir, profile, short_handle)

            # Download avatar & banner (with archive on change)
            import urllib.request  # ★ 함수-로컬 import 누락 → NameError 로 아바타/배너 저장이 조용히 안 되던 버그
            avatar_url = getattr(profile, 'avatar', '')
            banner_url = getattr(profile, 'banner', '')
            from datetime import datetime as _dt_p
            _pdir = os.path.join(user_dir, "profiles", _dt_p.now().strftime("%Y-%m-%d"))
            os.makedirs(_pdir, exist_ok=True)
            if avatar_url:
                _ap = os.path.join(_pdir, f"{short_handle}_avatar.jpg")
                if not os.path.exists(_ap):
                    try: urllib.request.urlretrieve(avatar_url, _ap)
                    except: pass
            if banner_url:
                _bp = os.path.join(_pdir, f"{short_handle}_banner.jpg")
                if not os.path.exists(_bp):
                    try: urllib.request.urlretrieve(banner_url, _bp)
                    except: pass

            log(f"Profile saved for @{short_handle}")
            return {"status": "ok"}
        except Exception as e:
            return {"error": str(e)}

    def save_profile(user_dir, profile, short_handle):
        try:
            # ★ excel_dir을 user_dir에서 파생 — 호출 컨텍스트의 클로저에 의존하지 않음
            #   (이전: collect_profile/collect_posts 등에 정의된 excel_dir을 참조하려 했는데
            #    save_profile은 동일 스코프 함수라 그 변수가 안 보여서 NameError 발생)
            excel_dir = os.path.join(user_dir, "excel")
            os.makedirs(excel_dir, exist_ok=True)

            wb = openpyxl.Workbook()
            ws = wb.active
            ws.title = "Profile"
            ws.column_dimensions['A'].width = 20
            ws.column_dimensions['B'].width = 60

            header_font = Font(bold=True, color="FFFFFF")
            header_fill = PatternFill(start_color="0085FF", end_color="0085FF", fill_type="solid")
            for col, h in enumerate(["Item", "Value"], 1):
                cell = ws.cell(row=1, column=col, value=h)
                cell.font = header_font
                cell.fill = header_fill

            items = [
                ("Handle", getattr(profile, 'handle', '')),
                ("Display Name", getattr(profile, 'display_name', '')),
                ("Description", getattr(profile, 'description', '')),
                ("DID", getattr(profile, 'did', '')),
                ("Followers", getattr(profile, 'followers_count', 0)),
                ("Following", getattr(profile, 'follows_count', 0)),
                ("Posts", getattr(profile, 'posts_count', 0)),
                ("Avatar URL", getattr(profile, 'avatar', '')),
                ("Banner URL", getattr(profile, 'banner', '')),
                ("Created At", getattr(profile, 'created_at', '')),
                ("Indexed At", getattr(profile, 'indexed_at', '')),
            ]
            for i, (key, val) in enumerate(items, 2):
                ws.cell(row=i, column=1, value=key)
                ws.cell(row=i, column=2, value=str(val) if val else '')

            wb.save(os.path.join(excel_dir, f"{short_handle}_profile.xlsx"))
        except Exception as e:
            log(f"Profile save error: {e}")

    def collect_replies(args):
        """Collect all replies the user posted to other people's posts."""
        apply_rl_settings(args)
        target = args.get("target", "")
        save_path = args.get("save_path", ".")
        do_media = args.get("download_media", True)

        short_handle, api_handle = norm_handle(target)
        user_dir = os.path.join(save_path, "bluesky", short_handle)
        media_dir = os.path.join(user_dir, "media")
        excel_dir = os.path.join(user_dir, "excel")
        os.makedirs(media_dir, exist_ok=True)
        os.makedirs(excel_dir, exist_ok=True)

        log(f"Collecting replies by @{short_handle}")
        walk_kind = f"bs_replies_m{int(bool(do_media))}"
        armed = walk_kind in take_walk_marks(excel_dir, "bs_replies_")
        ks = KnownStreak(os.path.join(excel_dir, f"{short_handle}_replies.xlsx"), args.get("stop_after_known", 0) if armed else 0)
        dl_failed.clear()
        prev_failed = take_walk_failures(excel_dir, walk_kind)
        sak = max(0, int(args.get("stop_after_known", 0) or 0))
        def hit():   # 최대 수 — 일찍 멈춤을 쓰면 새로 받은 것만 센다(받아 둔 것까지 세면 늘 최대 수에서 끊겨 표시가 영영 안 적혔다 — 검토)
            return cap_limit > 0 and ((ks.new if sak > 0 else len(all_data)) >= cap_limit)
        clean = False
        api_error = ''

        all_data = []
        cursor = None
        total_media = 0

        while True:
            if stop_requested or check_stdin_for_quit():
                log("⛔ 중지 요청 감지 — 답글 수집 중단")
                break
            if hit():
                log(f"최대 수집 수 {cap_limit}개에 닿아 멈춤")
                break
            try:
                # posts_with_replies: 포스트 + 답글 포함
                resp = client.get_author_feed(
                    actor=api_handle, limit=100, cursor=cursor,
                    filter='posts_with_replies'
                )
            except Exception as e:
                err = str(e).lower()
                if 'rate' in err or '429' in err:
                    if all_data:
                        _h = ["reply_to_url", "uri", "post_url", "text", "language", "created_at",
                              "author_handle", "author_name", "likes", "reposts", "replies",
                              "has_media", "media_count", "media_urls"]
                        save_xlsx(os.path.join(excel_dir, f"{short_handle}_replies.xlsx"), all_data, _h)
                        log(f"💾 중간 저장: {len(all_data)}건")
                    if not handle_rate_limit():
                        break
                    continue
                log(f"API error: {e}")
                api_error = str(e)
                break

            feed = pick_list(resp, ['feed', 'posts', 'items'], 'feed')
            if not feed:
                clean = True   # 피드 끝
                break

            for item in feed:
                if hit(): break
                post = pick(item, ['post', 'value', 'record'])
                if not post:
                    continue

                # 답글만 필터링 (reply가 있는 것)
                reply_info = getattr(item, 'reply', None)
                record = getattr(post, 'record', None)
                reply_ref = getattr(record, 'reply', None) if record else None

                if not reply_info and not reply_ref:
                    continue  # 원본 포스트는 스킵, 답글만

                data = process_post(post)
                if not data:
                    continue

                # 답글 대상 포스트 URL 추출
                reply_to_url = ''
                if reply_info:
                    parent = getattr(reply_info, 'parent', None)
                    if parent:
                        p_author = getattr(parent, 'author', None)
                        p_handle = getattr(p_author, 'handle', '') if p_author else ''
                        p_uri = getattr(parent, 'uri', '') or ''
                        p_id = p_uri.split('/')[-1] if p_uri else ''
                        if p_handle and p_id:
                            reply_to_url = f"https://bsky.app/profile/{p_handle}/post/{p_id}"

                row = [reply_to_url] + data
                all_data.append(row)

                # Log
                pdate = data[4] if len(data) > 4 else ''
                ptext = (data[2] if len(data) > 2 else '')[:80]
                plikes = data[7] if len(data) > 7 else 0
                pmedia = data[11] if len(data) > 11 else 0
                media_tag = f" 📎{pmedia}" if pmedia else ""
                log(f"[{len(all_data)}] {pdate} ❤{plikes}{media_tag} ↩️ {ptext}")

                if do_media:
                    new_media = download_media_files(post, media_dir, data, action_type="replies")
                    total_media += new_media

                if getattr(item, 'reason', None) is None:   # 리포스트(남의 답글)는 그 계정의 활동 시각이 아니다
                    ks.own(post)
                if ks.see(None, data[0]):
                    break

            progress(len(all_data), total_media, fresh=ks.new, newest=ks.newest)
            if ks.stopped:
                log(f"이미 받은 답글이 {ks.stopped}개 이어져 여기서 멈춥니다 — 새로 {ks.new}개")
                clean = True
                break

            cursor = pick(resp, ['cursor', 'next_cursor', 'nextCursor'])
            if not cursor:
                clean = True
                break

            n = len(all_data)
            if n > 0 and n % 1000 == 0:
                headers = ["reply_to_url", "uri", "post_url", "text", "language", "created_at",
                          "author_handle", "author_name", "likes", "reposts", "replies",
                          "has_media", "media_count", "media_urls"]
                save_xlsx(os.path.join(excel_dir, f"{short_handle}_replies.xlsx"), all_data, headers)
                log(f"💾 중간 저장: {n}개")
                time.sleep(5)
            elif n > 0 and n % 500 == 0:
                log(f"📊 진행: {n}개, 미디어 {total_media}개")
                time.sleep(2)
            else:
                time.sleep(1)

        if all_data:
            headers = ["reply_to_url", "uri", "post_url", "text", "language", "created_at",
                      "author_handle", "author_name", "likes", "reposts", "replies",
                      "has_media", "media_count", "media_urls"]
            save_xlsx(os.path.join(excel_dir, f"{short_handle}_replies.xlsx"), all_data, headers)
            log(f"Excel saved: {len(all_data)} replies")

        log(f"Complete! Replies: {len(all_data)}, Media: {total_media}")
        progress(len(all_data), total_media, "완료", fresh=ks.new, newest=ks.newest)
        dl_blocked = finish_failures(excel_dir, walk_kind, prev_failed, ks, sak)
        if clean and not api_error and not hit() and not dl_blocked:
            put_walk_mark(excel_dir, walk_kind)
        res = {"status": "ok", "count": len(all_data), "media": total_media, **ks.fields()}
        if api_error:
            res["api_error"] = api_error
        return res

    def collect_comments(args):
        """Collect all replies/comments on a user's posts using getPostThread recursion."""
        apply_rl_settings(args)
        target = args.get("target", "")
        save_path = args.get("save_path", ".")
        do_media = args.get("download_media", True)

        short_handle, api_handle = norm_handle(target)
        user_dir = os.path.join(save_path, "bluesky", short_handle)
        media_dir = os.path.join(user_dir, "media")
        excel_dir = os.path.join(user_dir, "excel")
        os.makedirs(media_dir, exist_ok=True)
        os.makedirs(excel_dir, exist_ok=True)

        log(f"Collecting comments on @{short_handle}'s posts")

        # Helper: recursively extract all replies from a thread
        def extract_replies(thread_node, depth=0):
            replies_data = []
            replies = getattr(thread_node, 'replies', None)
            if not replies:
                return replies_data
            for reply_node in replies:
                post = getattr(reply_node, 'post', None)
                if not post:
                    continue
                data = process_post(post)
                if data:
                    # Add depth info and parent context
                    data.append(depth)
                    replies_data.append((data, post))
                # Recurse into nested replies (unlimited depth)
                replies_data.extend(extract_replies(reply_node, depth + 1))
            return replies_data

        # Step 1: Get all posts from the user
        log("Phase 1: Fetching user's posts...")
        post_uris = []
        cursor = None
        while True:
            if stop_requested or check_stdin_for_quit():
                log("⛔ 중지 요청 감지 — 댓글 수집 중단")
                break
            try:
                resp = client.get_author_feed(
                    actor=api_handle, limit=100, cursor=cursor,
                    filter='posts_no_replies'
                )
            except Exception as e:
                err = str(e).lower()
                if 'rate' in err or '429' in err:
                    if not handle_rate_limit():
                        break
                    continue
                log(f"API error: {e}")
                break

            feed = pick_list(resp, ['feed', 'posts', 'items'], 'feed')
            if not feed:
                break

            for item in feed:
                post = pick(item, ['post', 'value', 'record'])
                if not post:
                    continue
                uri = getattr(post, 'uri', '')
                reply_count = (pick(post, ['reply_count', 'replyCount', 'replies_count'], 0) or 0)
                if reply_count > 0:
                    post_uris.append((uri, reply_count))

            cursor = pick(resp, ['cursor', 'next_cursor', 'nextCursor'])
            if not cursor:
                break
            time.sleep(2)  # 아이누: 2초 고정

        log(f"Found {len(post_uris)} posts with replies (total reply indicators: {sum(r for _, r in post_uris)})")

        # Step 2: For each post with replies, fetch the full thread
        log("Phase 2: Fetching reply threads...")
        all_comments = []
        total_media = 0
        import urllib.request

        for idx, (uri, expected_replies) in enumerate(post_uris):
            if over_cap(len(all_comments)):
                log(f"최대 수집 수 {cap_limit}개에 닿아 멈춤")
                break
            try:
                # depth=1000 to get as many nested replies as possible
                # The API typically returns up to ~1000 replies per thread
                params = {'uri': uri, 'depth': 1000, 'parentHeight': 0}
                resp = client.app.bsky.feed.get_post_thread(params)
            except Exception as e:
                err = str(e).lower()
                if 'rate' in err or '429' in err:
                    log("Rate limited, waiting 60s...")
                    time.sleep(60)
                    # Retry
                    try:
                        params = {'uri': uri, 'depth': 1000, 'parentHeight': 0}
                        resp = client.app.bsky.feed.get_post_thread(params)
                    except:
                        continue
                else:
                    continue

            thread = pick(resp, ['thread', 'post', 'value'])
            if not thread:
                continue

            # Get parent post info for context
            parent_post = getattr(thread, 'post', None)
            parent_url = ''
            if parent_post:
                p_uri = getattr(parent_post, 'uri', '')
                p_author = getattr(parent_post, 'author', None)
                p_handle = getattr(p_author, 'handle', '') if p_author else ''
                p_id = p_uri.split('/')[-1] if p_uri else ''
                parent_url = f"https://bsky.app/profile/{p_handle}/post/{p_id}"

            # Extract all replies recursively
            replies_with_posts = extract_replies(thread)
            for data, post_obj in replies_with_posts:
                depth = data.pop()  # Remove depth from data, add separately
                row = [parent_url, depth] + data
                all_comments.append(row)

                if do_media:
                    new_media = download_media_files(post_obj, media_dir, data, action_type="comments")
                    total_media += new_media

            if (idx + 1) % 10 == 0 or idx == len(post_uris) - 1:
                progress(len(all_comments), total_media, f"댓글 수집 중... ({idx+1}/{len(post_uris)} posts)")
                log(f"Progress: {idx+1}/{len(post_uris)} posts → {len(all_comments)} comments")

            # Rate limit: be gentle
            time.sleep(0.5)

            # Intermediate save every 1000 comments
            if len(all_comments) >= 1000 and len(all_comments) % 1000 < 50:
                headers = ["parent_post_url", "reply_depth",
                          "uri", "post_url", "text", "language", "created_at",
                          "author_handle", "author_name", "likes", "reposts", "replies",
                          "has_media", "media_count", "media_urls"]
                save_xlsx(os.path.join(excel_dir, f"{short_handle}_comments.xlsx"), all_comments, headers)

        if cap_limit > 0:
            all_comments = all_comments[:cap_limit]   # 스레드 하나에서 넘친 만큼
        # Final save
        if all_comments:
            headers = ["parent_post_url", "reply_depth",
                      "uri", "post_url", "text", "language", "created_at",
                      "author_handle", "author_name", "likes", "reposts", "replies",
                      "has_media", "media_count", "media_urls"]
            save_xlsx(os.path.join(excel_dir, f"{short_handle}_comments.xlsx"), all_comments, headers)
            log(f"Excel saved: {len(all_comments)} comments")

        log(f"Complete! Comments: {len(all_comments)}, Media: {total_media}")
        progress(len(all_comments), total_media, "완료")
        return {"status": "ok", "count": len(all_comments), "media": total_media}

    # ═══ Command loop ═══
    handlers = {
        "posts": collect_posts,
        "likes": collect_likes,
        "replies": collect_replies,
        "media": lambda a: collect_posts({**a, "download_media": True}),
        "followers": collect_followers,
        "following": collect_following,
        "blocks": collect_blocks,
        "mutes": collect_mutes,
        "search": collect_search,
        "notifications": collect_notifications,
        "messages": collect_messages,
        "profile": collect_profile,
        "comments": collect_comments,
    }

    for line in sys.stdin:
        line = line.strip()
        if not line:
            continue

        try:
            args = json.loads(line)
        except json.JSONDecodeError as e:
            print(json.dumps({"error": f"Invalid JSON: {e}"}), flush=True)
            continue

        action = args.get("action", "")

        if action == "quit":
            print(json.dumps({"status": "bye"}), flush=True)
            break

        if action == "ping":
            print(json.dumps({"status": "pong"}), flush=True)
            continue

        handler = handlers.get(action)
        if handler:
            try:
                rate_limit_count = 0  # reset per action
                adaptive_delay = 1.0  # reset adaptive delay per action
                stop_requested = False  # reset stop flag per action
                try:
                    cap_limit = max(0, int(args.get("max_count", 0) or 0))
                except (TypeError, ValueError):
                    cap_limit = 0
                result = handler(args)
                print(json.dumps(result), flush=True)
            except Exception as e:
                print(json.dumps({"error": str(e)}), flush=True)
        else:
            print(json.dumps({"error": f"Unknown action: {action}"}), flush=True)

if __name__ == "__main__":
    main()
