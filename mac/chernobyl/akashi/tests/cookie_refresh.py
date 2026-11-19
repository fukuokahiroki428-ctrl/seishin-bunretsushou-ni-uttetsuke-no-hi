#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""트위터 쿠키 자동 갱신 시험 — twitter_daemon.py 의 Chrome 쿠키 읽기를 가짜 Chrome 프로필로 돌린다(앱 · 네트워크 · 키체인 없음, 몇 초).

    python3 akashi/tests/cookie_refresh.py

시험하는 앱(build/ 또는 --app)의 번들 파이썬과 번들 데몬으로 돈다 — 실제로 실려 나가는 cryptography 로 푼다.
진짜 Chrome 프로필 · 쿠키 DB · 키체인은 열지 않는다: HOME 을 가짜 집으로 돌리고 `security` 호출은 바꿔치기한다.

보는 것(r54):
  같은 X 계정만 받는다 — 사용자 번호(twid)가 맞는 프로필 · 같은 auth_token(ct0 만 바뀜). 다른 계정 · 확인할 수 없는 계정은 받지 않는다.
  도메인은 정확히(netflix.com 의 auth_token 무시) · 지난 쿠키 · 분할(CHIPS) 쿠키 무시 · 같은 사이트의 것끼리 묶기 · 메타 판 24 의 앞 32바이트.
  까닭 알리기 — 암호 방식이 바뀜(v20) · DB 를 못 읽음 · 키체인 시간 초과(25초). 키는 한 번만 묻는다.
  명령 번호(_rid)를 답에 돌려주기 · X 오류 번호 읽기.
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import textwrap
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import _common as C  # noqa: E402

CHERNOBYL = HERE.parent.parent

INNER = r'''
import hashlib, importlib.util, io, json, os, sqlite3, subprocess, sys, contextlib

WORK = sys.argv[1]
DAEMON = sys.argv[2]
os.environ["HANISHIKI_DATA_DIR"] = os.path.join(WORK, "data")
os.environ["TMPDIR"] = os.path.join(WORK, "tmp")
os.makedirs(os.environ["TMPDIR"], exist_ok=True)
import tempfile
tempfile.tempdir = None

spec = importlib.util.spec_from_file_location("td", DAEMON)
td = importlib.util.module_from_spec(spec)
spec.loader.exec_module(td)
from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes

PW = b"FAKE-chrome-safe-storage-pw=="
CALLS = []
MODE = {"m": "ok"}


def fake_check_output(args, **kw):
    CALLS.append(kw.get("timeout"))
    assert list(args[:2]) == ["security", "find-generic-password"], args
    if MODE["m"] == "timeout":
        raise subprocess.TimeoutExpired(args, kw.get("timeout"))
    if MODE["m"] == "denied":
        raise subprocess.CalledProcessError(128, args)
    return PW + b"\n"


td.subprocess.check_output = fake_check_output
KEY = hashlib.pbkdf2_hmac("sha1", PW, b"saltysalt", 1003, dklen=16)


def enc(value, host, v24=False, prefix=b"v10"):
    plain = value.encode()
    if v24:
        plain = hashlib.sha256(host.encode()).digest() + plain
    pad = 16 - len(plain) % 16
    plain += bytes([pad]) * pad
    e = Cipher(algorithms.AES(KEY), modes.CBC(b" " * 16)).encryptor()
    return prefix + e.update(plain) + e.finalize()


SCHEMA = ("CREATE TABLE cookies(creation_utc INTEGER NOT NULL, host_key TEXT NOT NULL, top_frame_site_key TEXT NOT NULL DEFAULT '', "
          "name TEXT NOT NULL, value TEXT NOT NULL DEFAULT '', encrypted_value BLOB NOT NULL DEFAULT '', path TEXT NOT NULL DEFAULT '/', "
          "expires_utc INTEGER NOT NULL DEFAULT 0, has_expires INTEGER NOT NULL DEFAULT 1)")
FUTURE = int((2100 - 1601) * 365.25 * 86400 * 1e6)
PAST = int((2001 - 1601) * 365.25 * 86400 * 1e6)


def db(home, profile, rows, meta=None, rel="Network/Cookies", mtime=None):
    """rows: (host, name, value | bytes, {expires, partition, v24, prefix})"""
    path = os.path.join(home, "Library", "Application Support", "Google", "Chrome", profile, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    c = sqlite3.connect(path)
    if meta is not None:
        c.execute("CREATE TABLE meta(key LONGVARCHAR NOT NULL UNIQUE PRIMARY KEY, value LONGVARCHAR)")
        c.execute("INSERT INTO meta VALUES('version', ?)", (str(meta),))
    c.execute(SCHEMA)
    for i, row in enumerate(rows):
        host, name, val = row[:3]
        o = row[3] if len(row) > 3 else {}
        ev = val if isinstance(val, bytes) else enc(val, host, o.get("v24", False), o.get("prefix", b"v10"))
        c.execute("INSERT INTO cookies(creation_utc, host_key, top_frame_site_key, name, encrypted_value, expires_utc, has_expires) "
                  "VALUES(?,?,?,?,?,?,?)", (i + 1, host, o.get("partition", ""), name, ev, o.get("expires", FUTURE), 1))
    c.commit()
    c.close()
    if mtime is not None:
        os.utime(path, (mtime, mtime))
    return path


def x_login(auth, ct0, uid, host=".x.com", **o):
    rows = [(host, "auth_token", auth, o), (host, "ct0", ct0, o)]
    if uid:
        rows.append((host, "twid", "u%3D" + uid, o))
    return rows


OUT = {}
N = [0]


def case(name, setup, current_auth=None, expected_uid=None, mode="ok", reset_key=True):
    N[0] += 1
    home = os.path.join(WORK, "home_%02d_%s" % (N[0], name))
    os.makedirs(home)
    setup(home)
    os.environ["HOME"] = home
    MODE["m"] = mode
    if reset_key:
        td._CHROME_KEY = None
    CALLS.clear()
    r = td.extract_chrome_twitter_cookies(current_auth, expected_uid)
    OUT[name] = {"out": list(r), "err": td.LAST_COOKIE_ERROR, "calls": list(CALLS)}


A_OLD, A_NEW, CT_A, CT_A2 = "a0" * 20, "a1" * 20, "ca" * 40, "cb" * 40
B_AUTH, CT_B = "b2" * 20, "cd" * 40

# 1) 가장 최근 프로필은 다른 계정(B) — 번호가 맞는 A 의 새 로그인을 고른다
case("uid_match", lambda h: (db(h, "Default", x_login(B_AUTH, CT_B, "222"), mtime=2000000000),
                             db(h, "Profile 1", x_login(A_NEW, CT_A2, "111"), mtime=1900000000)),
     current_auth=A_OLD, expected_uid="111")
# 2) 번호를 모르면 다른 로그인은 받지 않는다
case("no_uid_other", lambda h: db(h, "Default", x_login(B_AUTH, CT_B, "222")), current_auth=A_OLD)
# 3) 번호를 몰라도 같은 auth_token(ct0 만 바뀜)은 받는다
case("same_session", lambda h: db(h, "Default", x_login(A_OLD, CT_A2, "")), current_auth=A_OLD)
# 4) 번호가 다른 로그인만 있으면 받지 않는다
case("uid_mismatch", lambda h: db(h, "Default", x_login(B_AUTH, CT_B, "222")), current_auth=A_OLD, expected_uid="111")
# 5) netflix.com 의 auth_token · ct0 는 X 것이 아니다
case("foreign_host", lambda h: db(h, "Default", [("netflix.com", "auth_token", A_NEW), (".netflix.com", "ct0", CT_A2)]),
     current_auth=A_OLD, expected_uid="111")
# 6) 지난 쿠키는 뺀다
case("expired", lambda h: db(h, "Default", x_login(A_NEW, CT_A2, "111", expires=PAST)), current_auth=A_OLD, expected_uid="111")
# 7) 분할(CHIPS) 쿠키는 뺀다
case("partitioned", lambda h: db(h, "Default", x_login(A_NEW, CT_A2, "111", partition="https://example.com")),
     current_auth=A_OLD, expected_uid="111")
# 8) 메타 판 24 — 값 앞의 SHA-256(도메인)을 뗀다
case("v24", lambda h: db(h, "Default", x_login(A_NEW, CT_A2, "111", v24=True), meta=24), current_auth=A_OLD, expected_uid="111")
# 9) 같은 사이트의 것끼리 — x.com 의 auth 와 twitter.com 의 ct0 · twid 를 섞지 않는다
case("site_pairing", lambda h: db(h, "Default", [(".x.com", "auth_token", B_AUTH), (".twitter.com", "ct0", CT_A2),
                                               (".twitter.com", "twid", "u%3D111")]),
     current_auth=A_OLD, expected_uid="111")
# 10) 'v10' 이 아닌 암호 방식은 그렇다고 알린다
case("v20", lambda h: db(h, "Default", x_login(A_NEW, CT_A2, "111", prefix=b"v20")), current_auth=A_OLD, expected_uid="111")
# 11) 읽지 못한 DB 는 까닭을 알린다
def _corrupt(h):
    p = os.path.join(h, "Library", "Application Support", "Google", "Chrome", "Default", "Network", "Cookies")
    os.makedirs(os.path.dirname(p), exist_ok=True)
    open(p, "wb").write(b"this is not sqlite" * 64)
case("db_error", _corrupt, current_auth=A_OLD, expected_uid="111")
# 12) 키체인 창에 답이 없으면 25초에서 그만두고 그렇다고 알린다
case("kc_timeout", lambda h: db(h, "Default", x_login(A_NEW, CT_A2, "111")), current_auth=A_OLD, expected_uid="111", mode="timeout")
# 13) 키는 한 번만 묻는다(앞 시험이 받은 키를 그대로)
td._CHROME_KEY = None
case("kc_first", lambda h: db(h, "Default", x_login(A_NEW, CT_A2, "111")), current_auth=A_OLD, expected_uid="111")
case("kc_cached", lambda h: db(h, "Default", x_login(A_NEW, CT_A2, "111")), current_auth=A_OLD, expected_uid="111", reset_key=False)

# 명령 번호 · 오류 번호
buf = io.StringIO()
with contextlib.redirect_stdout(buf):
    td._reply({"status": 200}, "7")
    td._reply({"status": 200})
lines = [json.loads(x) for x in buf.getvalue().splitlines()]
OUT["_reply"] = lines
OUT["codes"] = {
    "locked": sorted(td._x_error_codes('{"errors":[{"code":326,"message":"locked"}]}')),
    "csrf": sorted(td._x_error_codes('{"errors":[{"code":353}]}')),
    "junk": sorted(td._x_error_codes("<html>rate</html>")),
}
OUT["twid"] = [td._x_uid_from_twid("u%3D123"), td._x_uid_from_twid('"u=45"'), td._x_uid_from_twid("x%3Dabc")]
OUT["_tokens"] = {"A_NEW": A_NEW, "CT_A2": CT_A2, "A_OLD": A_OLD, "B_AUTH": B_AUTH}
print("@@JSON@@" + json.dumps(OUT))
'''


def main() -> int:
    ck = C.Checks()
    app = C.test_app()
    py = daemon = None
    if app:
        cand = Path(app) / "Contents" / "Resources" / "python_env" / "bin" / "python3"
        dm = Path(app) / "Contents" / "Resources" / "tools" / "twitter_daemon.py"
        if cand.exists() and dm.exists():
            py, daemon = cand, dm
    if py is None:
        py, daemon = Path(sys.executable), CHERNOBYL / "resources" / "tools" / "twitter_daemon.py"
        print("  (시험할 앱이 없어 이 파이썬 · 소스의 데몬으로 돕니다: %s)" % py)
    work = C.work_dir("cookie_refresh")
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    inner = work / "inner.py"
    inner.write_text(textwrap.dedent(INNER), encoding="utf-8")
    env = {k: v for k, v in os.environ.items() if not k.startswith("PYTHON")}
    r = subprocess.run([str(py), "-B", "-I", str(inner), str(work), str(daemon)], capture_output=True, text=True, timeout=120, env=env)
    js = next((ln[8:] for ln in (r.stdout or "").splitlines() if ln.startswith("@@JSON@@")), None)
    if not js:
        print(((r.stderr or "") + (r.stdout or ""))[-1500:])
        ck("시험 몸체가 돌았다", False)
        return ck.summary()
    o = json.loads(js)
    T = o["_tokens"]

    def out(name):
        return o[name]["out"], o[name]["err"]

    v, e = out("uid_match")
    ck("번호가 맞는 프로필을 고른다(가장 최근 프로필의 다른 계정이 아니라)", v == [T["A_NEW"], T["CT_A2"]], e)
    v, e = out("no_uid_other")
    ck("번호를 모르면 다른 로그인은 받지 않는다", v == [None, None] and "확인되지 않아" in e, e)
    v, e = out("same_session")
    ck("번호를 몰라도 같은 auth_token(ct0 만 바뀜)은 받는다", v == [T["A_OLD"], T["CT_A2"]], e)
    v, e = out("uid_mismatch")
    ck("번호가 다른 로그인은 받지 않는다", v == [None, None] and "확인되지 않아" in e, e)
    v, e = out("foreign_host")
    ck("netflix.com 의 auth_token 은 X 것으로 보지 않는다", v == [None, None] and "로그인 쿠키가 없습니다" in e, e)
    v, e = out("expired")
    ck("지난 쿠키는 뺀다", v == [None, None], e)
    v, e = out("partitioned")
    ck("분할(CHIPS) 쿠키는 뺀다", v == [None, None], e)
    v, e = out("v24")
    ck("메타 판 24 의 앞 32바이트를 떼어 바른 토큰", v[0] == T["A_NEW"], e)
    v, e = out("site_pairing")
    ck("다른 사이트의 auth · ct0 · twid 를 섞지 않는다", v == [None, None], e)
    v, e = out("v20")
    ck("'v10' 이 아닌 암호 방식(v20)은 그렇다고 알린다", v == [None, None] and "v20" in e, e)
    v, e = out("db_error")
    ck("읽지 못한 쿠키 DB 는 까닭을 알린다(로그인하라고 하지 않는다)", v == [None, None] and "읽지 못했습니다" in e and "Default" in e, e)
    v, e = out("kc_timeout")
    ck("키체인 창은 25초만 기다리고 그렇다고 알린다", v == [None, None] and o["kc_timeout"]["calls"] == [25] and "답이 없어" in e, e)
    ck("키는 처음 한 번만 묻는다", o["kc_first"]["calls"] == [25] and o["kc_cached"]["calls"] == [] and o["kc_cached"]["out"][0] == T["A_NEW"],
       "%s / %s" % (o["kc_first"]["calls"], o["kc_cached"]["calls"]))
    ck("답에 명령 번호를 돌려준다(번호 없이 온 명령엔 번호 없이)", o["_reply"] == [{"status": 200, "_rid": "7"}, {"status": 200}], o["_reply"])
    ck("X 오류 번호를 읽는다(잠김 326 · CSRF 353 · 엉뚱한 본문)",
       o["codes"] == {"locked": [326], "csrf": [353], "junk": []}, o["codes"])
    ck("twid 에서 사용자 번호만", o["twid"] == ["123", "45", ""], o["twid"])
    leftover = list((work / "tmp").glob("*.db")) if (work / "tmp").exists() else []
    ck("쿠키 DB 사본을 남기지 않는다", not leftover, [p.name for p in leftover])
    return ck.summary()


if __name__ == "__main__":
    sys.exit(main())
