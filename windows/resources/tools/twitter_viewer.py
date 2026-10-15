#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""옛 트위터 보기 — 받아 둔 트위터 계정을 '예전 트위터 사이트' 모양으로 다시 연다.

    python3 twitter_viewer.py <트위터 보관 폴더>/twitter/<계정>      # 그 계정의 view/ 를 만든다
    python3 twitter_viewer.py <트위터 보관 폴더>/twitter --all       # 모든 계정 + 계정 목록(index.html)

★ 왜
  · 사용자: "트위터 서버를 옮겨서 여기 설치한다는 생각으로 — 계정 프로필에 들어가서 순서대로, 시간까지
    전부 예전 트위터 사이트 형식으로 볼 수 있게". 엑셀과 파일 더미로는 '그 계정을 보는' 느낌이 안 난다.
  · 받은 자료(엑셀 · media · captures · profiles)만 읽는다. 인터넷에 나가지 않고, 원본 파일은 건드리지 않는다.
    만드는 것은 계정 폴더 안의 view/ (그리고 --all 이면 보관 폴더의 index.html) 뿐이다.
  · 화면은 이 앱이 새로 짠 것이다. 참고한 브라우저 확장(Old Twitter Layout)은 사용 허가가
    'All Rights Reserved' 라 코드·글꼴·그림을 옮기지 않았다. 트위터 로고도 쓰지 않는다.
  · 파일로 바로 열린다(file://). 자료는 data.js(<script>)로 싣는다 — file:// 에서는 fetch 가 막힌다.
"""
from __future__ import annotations

import datetime as _dt
import glob
import html
import json
import os
import re
import sys
import urllib.parse

try:
    import openpyxl
except Exception:  # 번들 파이썬에는 있다
    openpyxl = None

TOOL_VERSION = 6          # 화면 틀을 바꾸면 올린다 — 그러면 모든 계정을 다시 만든다
ID_RE = re.compile(r"(\d{8,20})")
# 미디어 파일 이름(TwitterCollector composeName):
#   {날짜_시각_}{본문}-{트윗번호}[-{순번}]({미디어키}).확장자   — 본문이 있을 때
#   {날짜_시각_}{트윗번호}-{순번}({미디어키}).확장자            — 본문이 없을 때(앞이 '_' 다)
MEDIA_ID_RE = re.compile(r"(?:^|[-_])(\d{10,20})(?:-\d+)?(?:\([^)]*\))?\.[A-Za-z0-9]{2,5}$")
CAPTURE_ID_RE = re.compile(r"_(\d{8,20})\.html?$")
MEDIA_EXT = {".jpg", ".jpeg", ".png", ".gif", ".webp", ".mp4", ".mov", ".m4v", ".webm"}
VIDEO_EXT = {".mp4", ".mov", ".m4v", ".webm"}


# ── 읽기 ────────────────────────────────────────────────────────────────────
def read_xlsx(path: str) -> list[dict]:
    if not openpyxl or not os.path.isfile(path):
        return []
    try:
        wb = openpyxl.load_workbook(path, read_only=True, data_only=True)
    except Exception:
        return []
    out = []
    try:
        ws = wb.active
        rows = ws.iter_rows(values_only=True)
        hdr = [str(h or "").strip() for h in next(rows, [])]
        for r in rows:
            if not r or all(v in (None, "") for v in r):
                continue
            out.append({h: ("" if v is None else v) for h, v in zip(hdr, r)})
    finally:
        wb.close()
    return out


def pick(d: dict, *names, default=""):
    low = {k.lower(): v for k, v in d.items()}
    for n in names:
        v = low.get(n.lower())
        if v not in (None, ""):
            return v
    return default


def to_int(v) -> int:
    try:
        return int(float(str(v).replace(",", "").strip()))
    except Exception:
        return 0


def snowflake_time(tid: str) -> _dt.datetime | None:
    """시각 칸이 비었을 때 — 트윗 번호(snowflake)에 만든 때가 들어 있다(2010-11 이후 번호).
    엑셀과 같게 일본 시각(UTC+9)의 맨 시각으로 돌려준다."""
    try:
        n = int(tid)
    except (TypeError, ValueError):
        return None
    if n < 100_000_000_000:   # 옛 일련번호(2010-11 전 — 약 300억까지 갔다) — 시각이 안 들어 있다. 눈송이 번호는 첫날에도 이보다 훨씬 크다
        return None
    ms = (n >> 22) + 1288834974657
    return _dt.datetime.fromtimestamp(ms / 1000, _dt.timezone(_dt.timedelta(hours=9))).replace(tzinfo=None)


def parse_time(s) -> _dt.datetime | None:
    """엑셀의 시각. 앱은 'YYYY/MM/DD HH:MM'(일본 시각)로 적는다. 다른 꼴도 받는다."""
    if isinstance(s, _dt.datetime):
        return s
    s = str(s or "").strip()
    if not s:
        return None
    for fmt in ("%Y/%m/%d %H:%M:%S", "%Y/%m/%d %H:%M", "%Y-%m-%d %H:%M:%S", "%Y-%m-%dT%H:%M:%S",
                "%Y-%m-%d %H:%M", "%a %b %d %H:%M:%S +0000 %Y"):
        try:
            return _dt.datetime.strptime(s if "%a" in fmt else s[:19], fmt)
        except Exception:
            pass
    m = re.match(r"(\d{4})年(\d{1,2})月(\d{1,2})日\D*?(\d{1,2}):(\d{2})(?::(\d{2}))?", s)
    if m:
        y, mo, d, h, mi, se = m.groups()
        return _dt.datetime(int(y), int(mo), int(d), int(h), int(mi), int(se or 0))
    return None


URL_USER_RE = re.compile(r"https?://(?:x|twitter)\.com/([A-Za-z0-9_]{1,15})/status/")


def user_of(r: dict) -> str:
    """작성자 아이디. 엑셀의 author_username 이 숫자(사용자 번호)로 들어 있는 판이 있어 주소에서 꺼낸다."""
    u = str(pick(r, "author_username")).strip().lstrip("@")
    if u and not u.isdigit():
        return u
    m = URL_USER_RE.search(str(pick(r, "tweet_url")))
    return m.group(1) if m else u


def rel_url(path: str, base_dir: str) -> str:
    """view/ 에서 본 상대 주소 — 파일 이름의 # ? % 공백·한글을 모두 퍼센트로 감싼다."""
    rel = os.path.relpath(path, base_dir).replace(os.sep, "/")
    return urllib.parse.quote(rel, safe="/")


def newest(paths: list[str]) -> str:
    paths = [p for p in paths if os.path.isfile(p)]
    return max(paths, key=lambda p: (os.path.basename(p), os.path.getmtime(p))) if paths else ""


OUT_ROOT = ""   # --out 이 있으면 view 를 그 아래 <계정>/view 에 만든다(원본 폴더에는 아무것도 안 쓴다)


def view_dir(ud: str) -> str:
    return os.path.join(OUT_ROOT, os.path.basename(os.path.normpath(ud)), "view") if OUT_ROOT else os.path.join(ud, "view")


def scan_account(ud: str) -> dict:
    handle = os.path.basename(os.path.normpath(ud))
    view = view_dir(ud)

    # 글 — _complete 가 본편이다. 리포스트·답글 엑셀이 따로 있으면 더한다(같은 id 는 한 번만).
    tweets: dict[str, dict] = {}
    files = sorted(glob.glob(os.path.join(ud, "*_complete.xlsx"))) + \
            sorted(f for f in glob.glob(os.path.join(ud, "*.xlsx"))
                   if re.search(r"_(reposts|replies|media|narrow|highlights)\.xlsx$", f))
    for f in files:
        for r in read_xlsx(f):
            tid = str(pick(r, "id")).strip()
            if not tid or not tid.isdigit() or tid in tweets:
                continue
            ttype = str(pick(r, "type") or "Tweet").strip()
            when = parse_time(pick(r, "created_at")) or snowflake_time(tid)
            rt_when = parse_time(pick(r, "retweet_time"))
            tweets[tid] = {
                "id": tid,
                "type": ttype,
                "pinned": ttype.startswith("\U0001F4CC") or "pin" in ttype.lower(),
                "text": str(pick(r, "text")),
                "name": str(pick(r, "author_name")),
                "user": user_of(r),
                "url": str(pick(r, "tweet_url")),
                "t": (rt_when or when).strftime("%Y-%m-%dT%H:%M:%S") if (rt_when or when) else "",
                "ot": when.strftime("%Y-%m-%dT%H:%M:%S") if when else "",
                "reply": to_int(pick(r, "reply_count")),
                "rt": to_int(pick(r, "retweet_count")),
                "like": to_int(pick(r, "favorite_count")),
                "quote": to_int(pick(r, "quote_count")),
                "view": to_int(pick(r, "view_count")),
                "replyTo": str(pick(r, "in_reply_to")),
                "quoted": str(pick(r, "quoted_tweet_url")),
                "sensitive": str(pick(r, "possibly_sensitive")).lower() in ("true", "1", "yes"),
                "mediaTypes": [m.strip() for m in str(pick(r, "media_type")).split(",") if m.strip()],
                "media": [],
                "cap": "",
            }

    # 미디어 — 파일 이름 끝의 '-{트윗번호}' 로 잇는다(TwitterCollector 의 파일명 규칙).
    mcount = 0
    for path in glob.glob(os.path.join(ud, "media", "**", "*"), recursive=True):
        ext = os.path.splitext(path)[1].lower()
        if ext not in MEDIA_EXT or not os.path.isfile(path):
            continue
        m = MEDIA_ID_RE.search(os.path.basename(path))
        if not m or m.group(1) not in tweets:
            continue
        t = tweets[m.group(1)]
        src = rel_url(path, view)
        if any(x["src"] == src for x in t["media"]):
            continue
        t["media"].append({"src": src, "video": ext in VIDEO_EXT})
        mcount += 1
    for t in tweets.values():
        t["media"].sort(key=lambda x: x["src"])
        if t["mediaTypes"] and "animated_gif" in t["mediaTypes"]:
            for x in t["media"]:
                if x["video"]:
                    x["gif"] = True

    # 캡처(SingleFile) — '날짜_시각_{트윗번호}.html'
    ccount = 0
    for path in glob.glob(os.path.join(ud, "captures", "**", "*.htm*"), recursive=True):
        m = CAPTURE_ID_RE.search(os.path.basename(path))
        if m and m.group(1) in tweets and not tweets[m.group(1)]["cap"]:
            tweets[m.group(1)]["cap"] = rel_url(path, view)
            ccount += 1

    # 프로필 — 앱의 엑셀은 두 꼴이다.
    #   ① profiles/target/…/{대상}_profile.xlsx — 'Field | Value' 두 칸(대상 한 사람)
    #   ② profiles/{날짜}/…_profile.xlsx — 'screen_name, name, description…' 줄마다 한 사람(글에 나온 사람들)
    #   ①을 먼저 쓰고, 빈 칸은 ②에서 대상 본인의 줄로 채운다. 둘 다 최신 파일이 이긴다.
    pr: dict = {}
    # ①(Field|Value) 이름을 ②(줄마다 한 사람) 이름으로 맞춰 같은 칸에 넣는다 — 그래야 ①이 먼저 차지한 칸을 ②가 못 덮는다
    #   (pick 은 키를 소문자로 접으며 나중 키가 이기고, 아래 pick 들은 ② 이름을 먼저 찾는다).
    _alias = {"handle": "screen_name", "bio": "description", "followers": "followers_count",
              "following": "friends_count", "tweets": "statuses_count", "likes": "favourites_count",
              "created": "created_at", "created at": "created_at"}
    def _fill(src: dict) -> None:
        for k, v in src.items():
            k = str(k or "").strip().lower()
            k = _alias.get(k, k)
            if k and v not in (None, "") and str(v).strip() not in ("", "0") and not pr.get(k):
                pr[k] = v
    prof_files = sorted(glob.glob(os.path.join(ud, "profiles", "**", "*profile*.xlsx"), recursive=True),
                        key=lambda f: os.path.getmtime(f), reverse=True)
    for f in prof_files:
        rows = read_xlsx(f)
        if rows and {k.lower() for k in rows[0]} >= {"field", "value"}:
            _fill({str(pick(r, "Field")).strip(): pick(r, "Value") for r in rows})
    for f in prof_files:
        for r in read_xlsx(f):
            if str(pick(r, "screen_name")).strip().lstrip("@").lower() == handle.lower():
                _fill(r)
    target_dirs = glob.glob(os.path.join(ud, "profiles", "target", "*"))
    avatar = newest(sum([glob.glob(os.path.join(d, "profile_*.jp*")) + glob.glob(os.path.join(d, "profile_*.png"))
                         for d in target_dirs], []))
    banner = newest(sum([glob.glob(os.path.join(d, "banner_*.jp*")) + glob.glob(os.path.join(d, "banner_*.png"))
                         for d in target_dirs], []))
    name_from_dir = ""
    if target_dirs:
        m = re.match(r"(.*)\(@[^)]*\)$", os.path.basename(sorted(target_dirs)[-1]))
        name_from_dir = m.group(1) if m else ""
    created = parse_time(pick(pr, "created_at", "Created At", "Created"))
    profile = {
        "handle": str(pick(pr, "screen_name", "Handle") or handle).lstrip("@"),
        "name": str(pick(pr, "name", "Name") or name_from_dir or handle),
        "bio": str(pick(pr, "description", "Bio")),
        "location": str(pick(pr, "location", "Location")),
        "url": str(pick(pr, "url", "URL")),
        "joined": created.strftime("%Y-%m") if created else "",
        "joinedRaw": "" if created else str(pick(pr, "created_at", "Created At", "Created")),
        "tweets": to_int(pick(pr, "statuses_count", "Tweets")),
        "following": to_int(pick(pr, "friends_count", "Following")),
        "followers": to_int(pick(pr, "followers_count", "Followers")),
        "likes": to_int(pick(pr, "Likes", "favourites_count")),
        "verified": str(pick(pr, "verified", "Verified")).lower() in ("true", "1", "yes"),
        "avatar": rel_url(avatar, view) if avatar else "",
        "banner": rel_url(banner, view) if banner else "",
    }

    # 팔로워·팔로잉 — 받아 둔 목록과 그 사람들의 프로필 사진(있으면).
    def people(kind: str) -> list[dict]:
        f = newest(glob.glob(os.path.join(ud, "*_%s.xlsx" % kind)))
        if not f:
            return []
        pics = {}
        for d in glob.glob(os.path.join(ud, "profiles", kind, "*")):
            m = re.search(r"\(@([^)]+)\)$", os.path.basename(d))
            img = newest(glob.glob(os.path.join(d, "profile_*")))
            if m and img:
                pics[m.group(1).lower()] = rel_url(img, view)
        out = []
        for r in read_xlsx(f):
            h = str(pick(r, "Handle", "screen_name")).lstrip("@")
            if not h:
                continue
            out.append({"handle": h, "name": str(pick(r, "Name", "name")), "bio": str(pick(r, "Bio", "description")),
                        "followers": to_int(pick(r, "Followers", "followers_count")),
                        "pic": pics.get(h.lower(), "")})
        return out

    tl = sorted(tweets.values(), key=lambda t: (t["pinned"], t["t"]), reverse=True)
    return {
        "profile": profile,
        "tweets": tl,
        "followers": people("followers"),
        "following": people("following"),
        "stats": {"tweets": len(tl), "media": mcount, "captures": ccount,
                  "first": min((t["t"] for t in tl if t["t"]), default=""),
                  "last": max((t["t"] for t in tl if t["t"]), default=""),
                  "built": _dt.datetime.now().strftime("%Y-%m-%dT%H:%M:%S")},
    }


# ── 쓰기 ────────────────────────────────────────────────────────────────────
def source_sig(ud: str) -> list:
    """다시 만들어야 하는지 가르는 지문 — 엑셀들의 크기·수정 시각과 media·captures 아래 폴더들의 수정 시각.
    ★ 수집이 끝날 때마다 모든 계정을 다시 읽으면, 보관 폴더가 클라우드(구글 드라이브 등)일 때 계정마다
      파일을 받아 오느라 한참 걸린다(실측: 계정당 약 1분). 바뀐 계정만 다시 만든다."""
    sig = [TOOL_VERSION]
    for f in sorted(glob.glob(os.path.join(ud, "*.xlsx")) + glob.glob(os.path.join(ud, "profiles", "**", "*.xlsx"), recursive=True)):
        try:
            st = os.stat(f)
            sig.append([os.path.relpath(f, ud), st.st_size, int(st.st_mtime)])
        except OSError:
            pass
    for sub in ("media", "captures"):
        for d in [os.path.join(ud, sub)] + glob.glob(os.path.join(ud, sub, "*")):
            if os.path.isdir(d):
                sig.append([os.path.relpath(d, ud), int(os.stat(d).st_mtime)])
    return sig


def write_account(ud: str, accounts: list[str], force: bool = False) -> dict:
    view = view_dir(ud)
    sig = source_sig(ud)
    mp = os.path.join(view, "meta.json")
    if not force and os.path.isfile(mp) and os.path.isfile(os.path.join(view, "index.html")):
        try:
            old = json.load(open(mp, encoding="utf-8"))
            if old.get("sig") == sig:
                return old            # 바뀐 것이 없다 — 그대로 둔다
        except Exception:
            pass
    data = scan_account(ud)
    os.makedirs(view, exist_ok=True)
    data["siblings"] = [a for a in accounts if a != os.path.basename(os.path.normpath(ud))]
    js = "window.ARCHIVE=" + json.dumps(data, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/") + ";"
    with open(os.path.join(view, "data.js"), "w", encoding="utf-8") as f:
        f.write(js)
    with open(os.path.join(view, "index.html"), "w", encoding="utf-8") as f:
        f.write(PROFILE_HTML)
    meta = {"handle": data["profile"]["handle"], "name": data["profile"]["name"], "bio": data["profile"]["bio"],
            "avatar": data["profile"]["avatar"], "banner": data["profile"]["banner"],
            "stats": data["stats"], "dir": os.path.basename(os.path.normpath(ud)), "sig": sig}
    with open(os.path.join(view, "meta.json"), "w", encoding="utf-8") as f:
        json.dump(meta, f, ensure_ascii=False)
    return meta


def write_index(root: str, metas: list[dict]) -> None:
    root = OUT_ROOT or root
    metas = [dict(m) for m in metas]
    for m in metas:   # 계정 목록은 보관 폴더에 있다 — 그림 주소를 그 자리 기준으로 고친다
        m.pop("sig", None)
        for k in ("avatar", "banner"):
            if m.get(k):
                m[k] = urllib.parse.quote(m["dir"], safe="") + "/view/" + m[k]
    js = "window.ACCOUNTS=" + json.dumps(metas, ensure_ascii=False, separators=(",", ":")).replace("</", "<\\/") + ";"
    with open(os.path.join(root, "accounts.js"), "w", encoding="utf-8") as f:
        f.write(js)
    with open(os.path.join(root, "index.html"), "w", encoding="utf-8") as f:
        f.write(INDEX_HTML)


def is_account_dir(d: str) -> bool:
    return os.path.isdir(d) and not os.path.basename(d).startswith(".") and (
        glob.glob(os.path.join(d, "*_complete.xlsx")) or os.path.isdir(os.path.join(d, "media")))


def main(argv: list[str]) -> int:
    if not argv:
        print(__doc__)
        return 2
    if openpyxl is None:   # 엑셀을 못 읽으면 빈 화면으로 멀쩡한 화면을 덮어쓰게 된다 — 아예 멈춘다
        print(json.dumps({"ok": False, "error": "openpyxl 이 없어 엑셀을 읽을 수 없습니다"}, ensure_ascii=False))
        return 1
    global OUT_ROOT
    if "--out" in argv:
        i = argv.index("--out")
        OUT_ROOT = os.path.abspath(os.path.expanduser(argv[i + 1]))
        os.makedirs(OUT_ROOT, exist_ok=True)
        argv = argv[:i] + argv[i + 2:]
    target = os.path.abspath(os.path.expanduser(argv[0]))
    if "--all" in argv:
        root = target
        accts = sorted(d for d in os.listdir(root) if is_account_dir(os.path.join(root, d)))
        metas = []
        for a in accts:
            try:
                metas.append(write_account(os.path.join(root, a), accts, force="--force" in argv))
            except Exception as e:
                print("건너뜀 %s: %s" % (a, e), file=sys.stderr)
        write_index(root, metas)
        print(json.dumps({"ok": True, "accounts": len(metas), "index": os.path.join(OUT_ROOT or root, "index.html")}, ensure_ascii=False))
        return 0
    if not is_account_dir(target):
        print(json.dumps({"ok": False, "error": "계정 폴더가 아닙니다"}, ensure_ascii=False))
        return 1
    root = os.path.dirname(target)
    accts = sorted(d for d in os.listdir(root) if is_account_dir(os.path.join(root, d)))
    meta = write_account(target, accts)
    # 계정 목록도 고친다 — 이미 만든 다른 계정의 meta.json 을 모아서
    metas = []
    for a in accts:
        mp = os.path.join(view_dir(os.path.join(root, a)), "meta.json")
        if os.path.isfile(mp):
            try:
                metas.append(json.load(open(mp, encoding="utf-8")))
            except Exception:
                pass
    write_index(root, metas)
    print(json.dumps({"ok": True, "tweets": meta["stats"]["tweets"], "media": meta["stats"]["media"],
                      "view": os.path.join(view_dir(target), "index.html")}, ensure_ascii=False))
    return 0


# ── 화면 ────────────────────────────────────────────────────────────────────
#   예전 트위터(2016~2019 웹)의 짜임 — 위 띠, 배너, 숫자 줄(프로필 탭), 왼쪽 프로필, 가운데 타임라인,
#   오른쪽 상자. 색은 그 시절 웹의 밝은 회청색 바탕과 파랑 강조, '야간 모드'(어두운 남색)를 둔다.
COMMON_CSS = r"""
:root{--bg:#E6ECF0;--card:#FFFFFF;--line:#E6ECF0;--ink:#14171A;--ink2:#657786;--blue:#1DA1F2;--blue-d:#1A91DA;
 --blue-soft:#E8F5FD;--like:#E0245E;--rt:#17BF63;--shadow:0 1px 3px rgba(0,0,0,.08);color-scheme:light}
:root[data-mode=night]{--bg:#10171E;--card:#15202B;--line:#38444D;--ink:#FFFFFF;--ink2:#8899A6;--blue:#1DA1F2;
 --blue-d:#4AB3F4;--blue-soft:#1C2938;--shadow:none;color-scheme:dark}
*{box-sizing:border-box}
html,body{margin:0;background:var(--bg);color:var(--ink);
 font:14px/1.38 "Helvetica Neue",Helvetica,Arial,"Apple SD Gothic Neo","Hiragino Sans",sans-serif}
a{color:var(--blue);text-decoration:none} a:hover{text-decoration:underline}
.topbar{position:sticky;top:0;z-index:20;height:46px;background:var(--card);border-bottom:1px solid var(--line);box-shadow:var(--shadow)}
.topbar .in{max-width:1190px;margin:0 auto;height:100%;display:flex;align-items:center;gap:18px;padding:0 14px}
.topbar .home{font-weight:700;color:var(--ink2);display:flex;align-items:center;gap:6px}
.topbar .home:hover{color:var(--blue);text-decoration:none}
.topbar .grow{flex:1}
.topbar input{width:220px;max-width:40vw;height:32px;border-radius:16px;border:1px solid var(--line);background:var(--bg);
 color:var(--ink);padding:0 14px;font:inherit;font-size:13px}
.topbar input:focus{outline:none;border-color:var(--blue);background:var(--card)}
.modebtn{border:1px solid var(--blue);color:var(--blue);background:transparent;border-radius:16px;height:30px;padding:0 12px;
 font:600 12px inherit;cursor:pointer}
.modebtn:hover{background:var(--blue-soft)}
"""

PROFILE_HTML = r"""<!doctype html>
<html lang="ko"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>보관된 트위터</title>
<style>""" + COMMON_CSS + r"""
.banner{height:280px;background:var(--blue) center/cover no-repeat}
.pnav{background:var(--card);border-bottom:1px solid var(--line);box-shadow:var(--shadow);position:sticky;top:46px;z-index:15}
.pnav .in{max-width:1190px;margin:0 auto;display:flex;padding-left:310px;min-height:60px}
.pnav .st{padding:10px 18px 8px;border-bottom:4px solid transparent;cursor:pointer;color:var(--ink2);text-align:left;background:none;border-top:0;border-left:0;border-right:0;font:inherit}
.pnav .st b{display:block;font-size:18px;color:var(--ink2);font-weight:700;line-height:1.2}
.pnav .st span{font-size:12px;font-weight:700;letter-spacing:.02em}
.pnav .st:hover b,.pnav .st.on b{color:var(--blue)} .pnav .st.on{border-bottom-color:var(--blue)}
.wrap{max-width:1190px;margin:0 auto;padding:10px 14px 60px;display:grid;grid-template-columns:290px minmax(0,600px) 260px;gap:10px;align-items:start}
.side .avatar{width:210px;height:210px;border-radius:50%;border:5px solid var(--card);background:var(--card) center/cover;margin:-170px 0 10px 0;box-shadow:var(--shadow);position:relative;z-index:16}
.side h1{font-size:22px;margin:0;line-height:1.2;word-break:break-word}
.side .h{color:var(--ink2);font-size:14px;margin-bottom:10px}
.side .bio{white-space:pre-wrap;word-break:break-word;margin:0 0 10px}
.side .meta{color:var(--ink2);font-size:14px;display:grid;gap:4px}
.side .arch{margin-top:14px;font-size:12px;color:var(--ink2);border-top:1px solid var(--line);padding-top:10px}
.card{background:var(--card);border:1px solid var(--line);border-radius:5px;box-shadow:var(--shadow)}
.tabs{display:flex;border-bottom:1px solid var(--line);padding:0 12px}
.tabs button{background:none;border:0;padding:12px 10px 10px;font:700 15px inherit;color:var(--blue);cursor:pointer;border-bottom:2px solid transparent}
.tabs button.on{color:var(--ink);border-bottom-color:var(--ink)}
.tabs button:hover{text-decoration:underline}
.filter{padding:8px 16px;color:var(--ink2);font-size:13px;border-bottom:1px solid var(--line);display:none;justify-content:space-between}
.filter.on{display:flex}
.tw{display:grid;grid-template-columns:48px minmax(0,1fr);gap:10px;padding:10px 16px;border-bottom:1px solid var(--line)}
.tw:hover{background:var(--blue-soft)}
.tw .ctx{grid-column:2;font-size:12px;color:var(--ink2);margin-bottom:-6px}
.tw .av{width:48px;height:48px;border-radius:50%;background:var(--line) center/cover}
.tw .hd{display:flex;gap:4px;align-items:baseline;flex-wrap:wrap;min-width:0}
.tw .hd .n{font-weight:700;color:var(--ink)} .tw .hd .u,.tw .hd .t{color:var(--ink2)}
.tw .hd .t:hover{color:var(--blue)}
.tw .tx{white-space:pre-wrap;word-break:break-word;font-size:14px;margin:2px 0 6px}
.tw .rep{color:var(--ink2);font-size:13px}
/* 미디어 — 한 장은 원래 비율(높이 상한), 여럿은 높이를 맞춘 격자(그 시절 웹과 같은 짜임) */
.mg{border-radius:12px;overflow:hidden;margin:8px 0 4px;border:1px solid var(--line)}
.mg.n1 img,.mg.n1 video{display:block;width:100%;height:auto;max-height:506px;object-fit:cover;background:#000;cursor:zoom-in}
.mg.n2,.mg.n3,.mg.n4{display:grid;gap:2px;height:286px}
.mg.n2{grid-template-columns:1fr 1fr} .mg.n3{grid-template-columns:2fr 1fr;grid-template-rows:1fr 1fr}
.mg.n3 > :first-child{grid-row:1/3} .mg.n4{grid-template-columns:1fr 1fr;grid-template-rows:1fr 1fr}
.mg.n2 > *,.mg.n3 > *,.mg.n4 > *{width:100%;height:100%;min-height:0;object-fit:cover;display:block;background:#000;cursor:zoom-in}
.mg video{object-fit:contain;cursor:default}
.mx{display:block;margin:8px 0 4px;padding:14px;border:1px dashed var(--line);border-radius:12px;color:var(--ink2);font-size:13px;text-align:center}
.mg .sens{filter:blur(18px)} .mg .sens:hover{filter:none}
.qt{border:1px solid var(--line);border-radius:12px;padding:8px 12px;margin:6px 0;font-size:13px;color:var(--ink2);display:block}
.acts{display:flex;gap:46px;color:var(--ink2);font-size:12px;margin-top:4px}
.acts span{display:inline-flex;align-items:center;gap:6px} .acts svg{width:16px;height:16px;fill:currentColor}
.acts .lk b{color:var(--like)} .acts .rp b{color:var(--rt)} .acts b{font-weight:400}
.more{padding:16px;text-align:center;color:var(--ink2)}
.empty{padding:40px 16px;text-align:center;color:var(--ink2)}
.ppl{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:10px;padding:12px}
.pc{border:1px solid var(--line);border-radius:5px;overflow:hidden;background:var(--card)}
.pc .pb{height:60px;background:var(--blue-soft)} .pc .pa{width:64px;height:64px;border-radius:50%;border:3px solid var(--card);margin:-34px 0 0 10px;background:var(--line) center/cover}
.pc .pi{padding:4px 10px 12px} .pc .pi b{display:block;word-break:break-word} .pc .pi small{color:var(--ink2)} .pc .pi p{margin:6px 0 0;font-size:12px;color:var(--ink2);word-break:break-word}
.box{padding:12px 15px}
.box h3{font-size:18px;margin:0 0 8px;font-weight:700}
.box .yr{display:flex;justify-content:space-between;padding:6px 0;border-top:1px solid var(--line);cursor:pointer;color:var(--ink)}
.box .yr:hover{color:var(--blue)} .box .yr.on{color:var(--blue);font-weight:700}
.box small{color:var(--ink2)}
.lb{position:fixed;inset:0;background:rgba(0,0,0,.88);display:none;align-items:center;justify-content:center;z-index:50;cursor:zoom-out}
.lb.on{display:flex} .lb img{max-width:94vw;max-height:94vh}
@media(max-width:1000px){.wrap{grid-template-columns:minmax(0,1fr)}.pnav .in{padding-left:12px;overflow-x:auto}.side .avatar{width:120px;height:120px;margin-top:-80px}.right{display:none}.banner{height:180px}}
</style></head>
<body>
<div class="topbar"><div class="in">
  <a class="home" href="../../index.html" title="받아 둔 계정 목록">&#8962; 보관된 계정</a>
  <span class="grow"></span>
  <input id="q" type="search" placeholder="이 계정에서 찾기" aria-label="이 계정에서 찾기">
  <button class="modebtn" id="mode" type="button">야간 모드</button>
</div></div>
<div class="banner" id="banner"></div>
<div class="pnav"><div class="in" id="pnav"></div></div>
<div class="wrap">
  <aside class="side" id="side"></aside>
  <main class="card" id="main">
    <div class="tabs" id="tabs"></div>
    <div class="filter" id="filter"><span id="ftext"></span><a href="#" id="fclear">모두 보기</a></div>
    <div id="list"></div>
    <div class="more" id="more"></div>
  </main>
  <aside class="right" id="right"></aside>
</div>
<div class="lb" id="lb"><img id="lbimg" alt=""></div>
<script src="data.js"></script>
<script>
(function(){
var A = window.ARCHIVE || {profile:{},tweets:[],followers:[],following:[],stats:{}};
var P = A.profile, T = A.tweets;
var esc = function(s){ return String(s==null?'':s).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];}); };
var num = function(n){ n=+n||0; if(n>=10000) return (n/10000).toFixed(n>=100000?0:1).replace(/\.0$/,'')+'만'; return n.toLocaleString('ko-KR'); };
var D = function(s){ if(!s) return null; var m=s.match(/(\d+)-(\d+)-(\d+)T(\d+):(\d+)/); return m?{y:+m[1],mo:+m[2],d:+m[3],h:+m[4],mi:+m[5]}:null; };
var when = function(s){ var d=D(s); if(!d) return ''; var ap=d.h<12?'오전':'오후', h=d.h%12||12;
  return d.y+'년 '+d.mo+'월 '+d.d+'일 '+ap+' '+h+':'+(d.mi<10?'0':'')+d.mi; };
var sibs = {}; (A.siblings||[]).forEach(function(s){ sibs[s.toLowerCase()]=s; });
var userLink = function(h){ var k=String(h).toLowerCase(); return sibs[k] ? '../../'+encodeURIComponent(sibs[k])+'/view/index.html' : 'https://x.com/'+encodeURIComponent(h); };
function linkify(t){
  var s = esc(t);
  s = s.replace(/(https?:\/\/[^\s<]+)/g,'<a href="$1" target="_blank" rel="noopener">$1</a>');
  s = s.replace(/(^|[^\w&\/])@(\w{1,15})/g,function(m,p,h){ return p+'<a href="'+userLink(h)+'">@'+h+'</a>'; });
  s = s.replace(/(^|[^\w&])#([\wÀ-￿]+)/g,function(m,p,h){ return p+'<a href="#" data-tag="'+h+'">#'+h+'</a>'; });
  return s;
}
// 배너·옆 프로필
document.title = (P.name||P.handle) + ' (@' + P.handle + ') · 보관된 트위터';
if (P.banner) document.getElementById('banner').style.backgroundImage = 'url("'+P.banner+'")';
document.getElementById('side').innerHTML =
  '<div class="avatar" style="'+(P.avatar?'background-image:url(&quot;'+P.avatar+'&quot;)':'')+'"></div>'
 +'<h1>'+esc(P.name)+(P.verified?' <span title="인증된 계정" style="color:var(--blue)">&#10004;</span>':'')+'</h1>'
 +'<div class="h">@'+esc(P.handle)+'</div>'
 +(P.bio?'<p class="bio">'+linkify(P.bio)+'</p>':'')
 +'<div class="meta">'
 +(P.location?'<div>&#9906; '+esc(P.location)+'</div>':'')
 +(P.url?'<div>&#128279; <a href="'+esc(P.url)+'" target="_blank" rel="noopener">'+esc(P.url.replace(/^https?:\/\//,''))+'</a></div>':'')
 +(P.joined?'<div>&#128197; '+P.joined.replace(/^(\d+)-(\d+)$/,'$1년 $2월')+' 가입</div>':(P.joinedRaw?'<div>&#128197; '+esc(P.joinedRaw)+' 가입</div>':''))
 +'</div>'
 +'<div class="arch">보관한 글 '+num(A.stats.tweets)+' · 미디어 '+num(A.stats.media)+' · 페이지 캡처 '+num(A.stats.captures)
 +'<br>'+(A.stats.first?when(A.stats.first).replace(/ 오[전후].*/,''):'')+' ~ '+(A.stats.last?when(A.stats.last).replace(/ 오[전후].*/,''):'')
 +'<br>만든 때 '+when(A.stats.built)+'</div>';
// 숫자 줄 — 그 시절 프로필 탭
var stats = [['tweets','트윗',P.tweets||A.stats.tweets],['following','팔로잉',P.following],['followers','팔로워',P.followers],['likes','마음에 들어요',P.likes]];
document.getElementById('pnav').innerHTML = stats.map(function(s){ return '<button class="st" type="button" data-k="'+s[0]+'"><span>'+s[1]+'</span><b>'+num(s[2])+'</b></button>'; }).join('');
// 타임라인 탭
var tabs = [['tweets','트윗'],['replies','트윗 및 답글'],['media','미디어']];
if (T.some(function(t){ return /^\W*Retweet/.test(t.type); })) tabs.push(['reposts','리트윗']);
if ((A.following||[]).length) tabs.push(['following','팔로잉']);
if ((A.followers||[]).length) tabs.push(['followers','팔로워']);
var st = { tab:'tweets', year:'', q:'', tag:'', shown:0 };
try { var m0 = localStorage.getItem('archMode'); if (m0==='night') document.documentElement.setAttribute('data-mode','night'); } catch(e){}
function paintMode(){ document.getElementById('mode').textContent = document.documentElement.getAttribute('data-mode')==='night' ? '주간 모드' : '야간 모드'; }
paintMode();
document.getElementById('mode').onclick = function(){ var n = document.documentElement.getAttribute('data-mode')==='night';
  if (n) document.documentElement.removeAttribute('data-mode'); else document.documentElement.setAttribute('data-mode','night');
  try{ localStorage.setItem('archMode', n?'day':'night'); }catch(e){} paintMode(); };
function renderTabs(){
  document.getElementById('tabs').innerHTML = tabs.map(function(t){ return '<button type="button" data-t="'+t[0]+'" class="'+(st.tab===t[0]?'on':'')+'">'+t[1]+'</button>'; }).join('');
  document.querySelectorAll('#pnav .st').forEach(function(b){ b.classList.toggle('on', b.dataset.k===st.tab || (b.dataset.k==='tweets' && /tweets|replies|media|reposts/.test(st.tab))); });
}
function rows(){
  var q = st.q.toLowerCase();
  return T.filter(function(t){
    var rt = /^\W*Retweet/.test(t.type), rep = /Reply/.test(t.type) || !!t.replyTo;
    if (st.tab==='tweets' && (rep && !rt)) return false;
    if (st.tab==='media' && !t.media.length) return false;
    if (st.tab==='reposts' && !rt) return false;
    if (st.year && t.t.slice(0,4)!==st.year) return false;
    if (st.tag && t.text.indexOf('#'+st.tag)<0) return false;
    if (q && (t.text+' '+t.name+' '+t.user).toLowerCase().indexOf(q)<0) return false;
    return true;
  });
}
var ICON = {
  reply:'<svg viewBox="0 0 24 24"><path d="M12 3C6.5 3 2 6.6 2 11c0 2.4 1.3 4.6 3.4 6.1L4.6 21l4.5-2.4c.9.2 1.9.4 2.9.4 5.5 0 10-3.6 10-8s-4.5-8-10-8zm0 14c-.9 0-1.8-.1-2.6-.4l-.4-.1-2 1.1.4-1.8-.5-.4C5.1 14.3 4 12.7 4 11c0-3.3 3.6-6 8-6s8 2.7 8 6-3.6 6-8 6z"/></svg>',
  rt:'<svg viewBox="0 0 24 24"><path d="M7 7h9v3l4-4-4-4v3H5v6h2V7zm10 10H8v-3l-4 4 4 4v-3h11v-6h-2v4z"/></svg>',
  like:'<svg viewBox="0 0 24 24"><path d="M12 21s-7.5-4.6-9.6-9C.9 8.6 3 5 6.5 5c2 0 3.5 1.1 4.3 2.6h2.4C14 6.1 15.5 5 17.5 5 21 5 23.1 8.6 21.6 12c-2.1 4.4-9.6 9-9.6 9zm-5.5-14C4.4 7 3.2 9.2 4.2 11.3 5.7 14.5 10.6 18 12 18.9c1.4-.9 6.3-4.4 7.8-7.6 1-2.1-.2-4.3-2.3-4.3-1.6 0-2.7 1.1-3.2 2.4h-4.6C9.2 8.1 8.1 7 6.5 7z"/></svg>',
  view:'<svg viewBox="0 0 24 24"><path d="M4 20h3V10H4v10zm6.5 0h3V4h-3v16zM17 20h3v-7h-3v7z"/></svg>'
};
function card(t){
  var rt = /^\W*Retweet/.test(t.type);
  var av = rt ? '' : P.avatar;
  var media = '';
  if (t.media.length){
    var n = Math.min(t.media.length,4);
    media = '<div class="mg n'+n+'">'+t.media.slice(0,4).map(function(m){
      if (m.video) return m.gif ? '<video src="'+m.src+'" autoplay loop muted playsinline></video>' : '<video src="'+m.src+'" controls preload="metadata"></video>';
      return '<img loading="lazy" src="'+m.src+'" alt="" class="'+(t.sensitive?'sens':'')+'" data-full="'+m.src+'">';
    }).join('')+'</div>';
  } else if ((t.mediaTypes||[]).length){
    // 표에는 미디어가 있다고 적혔는데 파일은 받아 두지 않았다(대개 리트윗) — 자리만 보이고 원본으로 잇는다
    // media_type 은 종류를 중복 없이 모은 칸이라(TwitterCollector composeName 쪽) 개수를 알 수 없다 — 종류만 적는다
    var ph = t.mediaTypes.indexOf('photo')>=0, vd = t.mediaTypes.some(function(x){return x!=='photo';});
    var what = [ph?'사진':'', vd?'동영상':''].filter(Boolean).join(' · ');
    media = '<a class="mx" href="'+(t.cap||esc(t.url))+'"'+(t.cap?'':' target="_blank" rel="noopener"')+'>'+what+' — 받아 두지 않음</a>';
  }
  var time = '<a class="t" title="'+esc(when(t.ot||t.t))+'" href="'+(t.cap||esc(t.url))+'"'+(t.cap?'':' target="_blank" rel="noopener"')+'>'+esc(when(t.ot||t.t))+'</a>';
  return '<article class="tw">'
    +(t.pinned?'<div class="ctx">&#128204; 고정된 트윗</div>':'')
    +(rt?'<div class="ctx">&#8634; '+esc(P.name)+' 님이 리트윗함 · '+esc(when(t.t))+'</div>':'')
    +'<div class="av" style="'+(av?'background-image:url(&quot;'+av+'&quot;)':'')+'"></div>'
    +'<div><div class="hd"><span class="n">'+esc(t.name||P.name)+'</span><span class="u">@'+esc(rt?t.user:(P.handle))+'</span><span class="u">·</span>'+time+'</div>'
    +(t.replyTo?'<div class="rep">@'+esc(t.replyTo)+' 님에게 보내는 답글</div>':'')
    +'<div class="tx">'+linkify(t.text)+'</div>'+media
    +(t.quoted?'<a class="qt" href="'+esc(t.quoted)+'" target="_blank" rel="noopener">인용한 트윗 — '+esc(t.quoted.replace(/^https?:\/\//,''))+'</a>':'')
    +'<div class="acts"><span>'+ICON.reply+'<b>'+(t.reply?num(t.reply):'')+'</b></span><span class="rp">'+ICON.rt+'<b>'+(t.rt?num(t.rt):'')+'</b></span>'
    +'<span class="lk">'+ICON.like+'<b>'+(t.like?num(t.like):'')+'</b></span>'+(t.view?'<span>'+ICON.view+'<b>'+num(t.view)+'</b></span>':'')
    +(t.cap?'<span><a href="'+t.cap+'" title="받아 둔 원본 페이지(SingleFile)">원본 페이지</a></span>':'')+'</div>'
    +'</div></article>';
}
function people(list){
  if (!list.length) return '<div class="empty">받아 둔 목록이 없습니다.</div>';
  return '<div class="ppl">'+list.map(function(p){
    return '<a class="pc" href="'+userLink(p.handle)+'"><div class="pb"></div><div class="pa" style="'+(p.pic?'background-image:url(&quot;'+p.pic+'&quot;)':'')+'"></div>'
      +'<div class="pi"><b>'+esc(p.name||p.handle)+'</b><small>@'+esc(p.handle)+'</small>'+(p.bio?'<p>'+esc(p.bio).slice(0,140)+'</p>':'')+'</div></a>';
  }).join('')+'</div>';
}
var cur = [], STEP = 40;
function render(reset){
  var L = document.getElementById('list'), M = document.getElementById('more');
  if (st.tab==='following' || st.tab==='followers'){ L.innerHTML = people(A[st.tab]||[]); M.textContent=''; return; }
  if (reset){ cur = rows(); st.shown = 0; L.innerHTML = ''; }
  if (!cur.length){ L.innerHTML = '<div class="empty">보여 줄 트윗이 없습니다.</div>'; M.textContent=''; return; }
  var next = cur.slice(st.shown, st.shown+STEP);
  L.insertAdjacentHTML('beforeend', next.map(card).join(''));
  st.shown += next.length;
  M.textContent = st.shown < cur.length ? (cur.length - st.shown).toLocaleString('ko-KR')+'개 더 — 내리면 이어서 보입니다' : '여기까지 '+cur.length.toLocaleString('ko-KR')+'개';
  var f = document.getElementById('filter'), parts = [];
  if (st.year) parts.push(st.year+'년'); if (st.tag) parts.push('#'+st.tag); if (st.q) parts.push('"'+st.q+'"');
  f.classList.toggle('on', parts.length>0); document.getElementById('ftext').textContent = parts.join(' · ')+' — '+cur.length.toLocaleString('ko-KR')+'개';
}
// 오른쪽 — 연도별
(function(){
  var by = {}; T.forEach(function(t){ var y=t.t.slice(0,4); if(y) by[y]=(by[y]||0)+1; });
  var ys = Object.keys(by).sort().reverse();
  document.getElementById('right').innerHTML = '<div class="card box"><h3>연도별 보기</h3>'+ys.map(function(y){ return '<div class="yr" data-y="'+y+'"><span>'+y+'년</span><small>'+by[y].toLocaleString('ko-KR')+'</small></div>'; }).join('')+'</div>';
})();
document.addEventListener('click', function(e){
  var b = e.target.closest('[data-t]'); if (b){ st.tab=b.dataset.t; renderTabs(); render(true); return; }
  var k = e.target.closest('#pnav .st'); if (k){ var map={tweets:'tweets',following:'following',followers:'followers',likes:'tweets'}; st.tab=map[k.dataset.k]||'tweets'; if(!tabs.some(function(t){return t[0]===st.tab;})) st.tab='tweets'; renderTabs(); render(true); return; }
  var y = e.target.closest('.yr'); if (y){ st.year = st.year===y.dataset.y ? '' : y.dataset.y; document.querySelectorAll('.yr').forEach(function(x){ x.classList.toggle('on', x.dataset.y===st.year); }); render(true); return; }
  var tg = e.target.closest('[data-tag]'); if (tg){ e.preventDefault(); st.tag=tg.dataset.tag; render(true); window.scrollTo(0,0); return; }
  if (e.target.id==='fclear'){ e.preventDefault(); st.year='';st.tag='';st.q=''; document.getElementById('q').value=''; document.querySelectorAll('.yr').forEach(function(x){x.classList.remove('on');}); render(true); return; }
  var im = e.target.closest('img[data-full]'); if (im){ document.getElementById('lbimg').src = im.dataset.full; document.getElementById('lb').classList.add('on'); return; }
  if (e.target.closest('#lb')) document.getElementById('lb').classList.remove('on');
});
var qt; document.getElementById('q').addEventListener('input', function(){ clearTimeout(qt); var v=this.value; qt=setTimeout(function(){ st.q=v.trim(); render(true); }, 200); });
document.addEventListener('keydown', function(e){ if (e.key==='Escape') document.getElementById('lb').classList.remove('on'); });
new IntersectionObserver(function(es){ if (es[0].isIntersecting && st.shown < cur.length && !/follow/.test(st.tab)) render(false); }, {rootMargin:'600px'}).observe(document.getElementById('more'));
renderTabs(); render(true);
})();
</script>
</body></html>
"""

INDEX_HTML = r"""<!doctype html>
<html lang="ko"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>보관된 트위터 — 계정 목록</title>
<style>""" + COMMON_CSS + r"""
.wrap{max-width:1190px;margin:0 auto;padding:16px 14px 60px}
h1{font-size:22px;margin:6px 0 14px}
.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(270px,1fr));gap:12px}
.ac{background:var(--card);border:1px solid var(--line);border-radius:5px;overflow:hidden;color:var(--ink);box-shadow:var(--shadow);display:block}
.ac:hover{text-decoration:none;border-color:var(--blue)}
.ac .b{height:90px;background:var(--blue) center/cover}
.ac .a{width:72px;height:72px;border-radius:50%;border:4px solid var(--card);background:var(--line) center/cover;margin:-40px 0 0 12px}
.ac .i{padding:2px 14px 14px} .ac .i b{font-size:16px;display:block;word-break:break-word} .ac .i small{color:var(--ink2)}
.ac .i p{margin:6px 0 8px;font-size:13px;color:var(--ink2);max-height:3.9em;overflow:hidden;word-break:break-word}
.ac .s{font-size:12px;color:var(--ink2)}
.empty{padding:40px;text-align:center;color:var(--ink2)}
</style></head>
<body>
<div class="topbar"><div class="in">
  <span class="home">&#8962; 보관된 계정</span><span class="grow"></span>
  <input id="q" type="search" placeholder="계정 찾기" aria-label="계정 찾기">
  <button class="modebtn" id="mode" type="button">야간 모드</button>
</div></div>
<div class="wrap"><h1 id="h"></h1><div class="grid" id="g"></div></div>
<script src="accounts.js"></script>
<script>
(function(){
var L = (window.ACCOUNTS||[]).slice().sort(function(a,b){ return (b.stats.last||'').localeCompare(a.stats.last||''); });
var esc = function(s){ return String(s==null?'':s).replace(/[&<>"']/g,function(c){return {'&':'&amp;','<':'&lt;','>':'&gt;','"':'&quot;',"'":'&#39;'}[c];}); };
try { if (localStorage.getItem('archMode')==='night') document.documentElement.setAttribute('data-mode','night'); } catch(e){}
function paintMode(){ document.getElementById('mode').textContent = document.documentElement.getAttribute('data-mode')==='night' ? '주간 모드' : '야간 모드'; }
paintMode();
document.getElementById('mode').onclick = function(){ var n = document.documentElement.getAttribute('data-mode')==='night';
  if (n) document.documentElement.removeAttribute('data-mode'); else document.documentElement.setAttribute('data-mode','night');
  try{ localStorage.setItem('archMode', n?'day':'night'); }catch(e){} paintMode(); };
function draw(q){
  q = (q||'').toLowerCase();
  var rows = L.filter(function(a){ return !q || (a.handle+' '+a.name).toLowerCase().indexOf(q)>=0; });
  document.getElementById('h').textContent = '받아 둔 계정 ' + rows.length + '개';
  document.getElementById('g').innerHTML = rows.length ? rows.map(function(a){
    return '<a class="ac" href="'+encodeURIComponent(a.dir)+'/view/index.html">'
      +'<div class="b" style="'+(a.banner?'background-image:url(&quot;'+a.banner+'&quot;)':'')+'"></div>'
      +'<div class="a" style="'+(a.avatar?'background-image:url(&quot;'+a.avatar+'&quot;)':'')+'"></div>'
      +'<div class="i"><b>'+esc(a.name)+'</b><small>@'+esc(a.handle)+'</small>'+(a.bio?'<p>'+esc(a.bio)+'</p>':'')
      +'<div class="s">글 '+(a.stats.tweets||0).toLocaleString('ko-KR')+' · 미디어 '+(a.stats.media||0).toLocaleString('ko-KR')
      +(a.stats.last?' · 마지막 글 '+a.stats.last.slice(0,10):'')+'</div></div></a>';
  }).join('') : '<div class="empty">아직 받아 둔 계정이 없습니다.</div>';
}
document.getElementById('q').addEventListener('input', function(){ draw(this.value); });
draw('');
})();
</script>
</body></html>
"""

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
