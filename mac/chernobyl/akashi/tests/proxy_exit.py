#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""프록시 출구 — 트위터가 계정의 프록시로만 나가는지. 인터넷으로 안 나간다.

    python3 akashi/tests/proxy_exit.py     # 혼자(약 2분)

A 데몬만(앱 없이) — 시험할 앱 번들의 진짜 twitter_daemon.py 를 그 번들 파이썬으로 띄운다(가짜 토큰).
  이 프로세스의 바깥 연결은 소켓 단에서 막고(127.0.0.1 만 허락), 이 맥 안의 가짜 HTTP 프록시가 받은 것을 본다.
  1 init 에 proxy 를 주면 X 로 가는 요청이 그 프록시로 간다(CONNECT x.com:443) · 막힌 바깥 연결 없음
  2 (대조) proxy 를 안 주면 x.com 으로 바로 가려다 막힌다 — 이 시험이 우회를 잡아낼 수 있다는 증거
B 앱(격리 사본 + 가짜 X) — 계정에 프록시 프로필을 붙여 수집하면 앱이 데몬 init 에 그 주소를 싣는다.
  앱의 '프록시 미리 확인'(curl → https://x.com/)은 가짜 프록시가 이 맥 안의 TLS 서버로 이어 주고,
  격리 홈의 .curlrc(--insecure)로 그 자체 서명 인증서를 받아들인다 — 격리 사본에만 있는 설정이다.
  3 계정에 프로필 → 미리 확인이 그 프록시로 갔고, 데몬 init 의 proxy 가 그 주소
  4 프로필 없음 → init 에 proxy 가 없다(직접 연결 — 예전과 같다)
  5 (같은 판 입구) 저장 경로가 비면 시작하기 전에 멈추고 알린다 — 예전 맥은 끝까지 돌고 파일이 하나도 안 남았다(윈도우와 맞춤)
  6 계정이 돌았다가(429) 'all' 의 다음 갈래에서 0번으로 돌아와도 — 데몬을 띄울 때마다 그 계정의 출구로 나간다(A 의 쿠키가 B 의 IP 로 나가지 않는다)
    돌고 난 뒤의 미디어도 그 계정의 출구로(앱의 HTTP 객체는 만들 때의 출구에 묶인다 — 한 세션이 두 IP 가 되지 않게)

★ 왜 — 데몬의 세션 계층(x_session.Client)은 전송기를 직접 넣어(AsyncClient(transport=…)) httpx 가 환경 변수의
  프록시를 보지 않는다. 예전 맥은 데몬에 환경 변수로만 프록시를 주어, 계정에 프록시를 걸어도 트위터 API 요청은
  이 컴퓨터의 회선으로 나갔다(윈도우는 init 의 proxy 로 생성자에 직접 준다 — 이제 맥도 같다).
계정 값은 이 시험이 만든 가짜이고 격리 사본 · 데몬 init 으로만 넘긴다.
"""
from __future__ import annotations

import json
import os
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import _common as C  # noqa: E402
from early_stop import World, NOW, HOUR  # noqa: E402

TOKENS = {"auth_token": "akashi-test-token", "ct0": "akashi-test-ct0"}

# 데몬을 감싸 띄우는 것 — 바깥 연결과 바깥 이름 풀기를 막고 기록한다(127.0.0.1 · ::1 · localhost 만 허락).
WRAP = r'''
import json, os, runpy, socket, sys
TOOLS, LOG = sys.argv[1], sys.argv[2]
OK = ("127.0.0.1", "::1", "localhost")
def note(x):
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(json.dumps(x) + "\n")
def host(a):
    return a[0] if isinstance(a, tuple) else str(a)
_c, _cx, _g = socket.socket.connect, socket.socket.connect_ex, socket.getaddrinfo
def connect(self, a):
    if self.family in (socket.AF_INET, socket.AF_INET6) and host(a) not in OK:
        note({"blocked": host(a)})
        raise ConnectionRefusedError("akashi: 바깥 연결 막음")
    return _c(self, a)
def connect_ex(self, a):
    if self.family in (socket.AF_INET, socket.AF_INET6) and host(a) not in OK:
        note({"blocked": host(a)})
        return 61
    return _cx(self, a)
def getaddrinfo(h, *a, **k):
    if h is not None and (h.decode() if isinstance(h, bytes) else h) not in OK:
        note({"blocked_dns": h if isinstance(h, str) else h.decode()})
        raise socket.gaierror(8, "akashi: 바깥 이름 풀기 막음")
    return _g(h, *a, **k)
socket.socket.connect, socket.socket.connect_ex, socket.getaddrinfo = connect, connect_ex, getaddrinfo
sys.path.insert(0, TOOLS)
d = os.path.join(TOOLS, "twitter_daemon.py")
sys.argv = [d, "--stdin-args"]
runpy.run_path(d, run_name="__main__")
'''


class FakeProxy:
    """HTTP 프록시 흉내 — 연결마다 첫 줄을 적는다. tunnel 이 있으면 CONNECT 를 그리로 잇고, 없으면 502."""

    def __init__(self, tunnel=None):
        self.lines: list[str] = []
        self.tunnel = tunnel
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self._one, args=(c,), daemon=True).start()

    @staticmethod
    def _pipe(a, b):
        try:
            while True:
                d = a.recv(65536)
                if not d:
                    break
                b.sendall(d)
        except OSError:
            pass
        finally:
            for s in (a, b):
                try:
                    s.shutdown(socket.SHUT_RDWR)
                except OSError:
                    pass

    def _one(self, c):
        try:
            c.settimeout(15)
            buf = b""
            while b"\r\n\r\n" not in buf:
                d = c.recv(4096)
                if not d:
                    return
                buf += d
            first = buf.split(b"\r\n", 1)[0].decode("latin-1")
            self.lines.append(first)
            if self.tunnel and first.startswith("CONNECT "):
                u = socket.create_connection(self.tunnel, timeout=15)
                c.sendall(b"HTTP/1.1 200 Connection established\r\n\r\n")
                c.settimeout(None)
                u.settimeout(None)
                threading.Thread(target=self._pipe, args=(u, c), daemon=True).start()
                self._pipe(c, u)
                return
            c.sendall(b"HTTP/1.1 502 Bad Gateway\r\nContent-Length: 0\r\nConnection: close\r\n\r\n")
        except OSError:
            pass
        finally:
            try:
                c.close()
            except OSError:
                pass

    def close(self):
        self.sock.close()


class TlsOk:
    """이 맥 안의 HTTPS 서버 — 무엇을 물어도 200 'ok'(자체 서명 인증서)."""

    def __init__(self, work: Path):
        key, crt = work / "key.pem", work / "cert.pem"
        if not crt.exists():
            subprocess.run(["/usr/bin/openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "2",
                            "-keyout", str(key), "-out", str(crt), "-subj", "/CN=x.com"],
                           check=True, capture_output=True)
        self.ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.ctx.load_cert_chain(str(crt), str(key))
        self.sock = socket.socket()
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind(("127.0.0.1", 0))
        self.sock.listen(16)
        self.port = self.sock.getsockname()[1]
        threading.Thread(target=self._loop, daemon=True).start()

    def _loop(self):
        while True:
            try:
                c, _ = self.sock.accept()
            except OSError:
                return
            threading.Thread(target=self._one, args=(c,), daemon=True).start()

    def _one(self, c):
        try:
            c.settimeout(15)
            s = self.ctx.wrap_socket(c, server_side=True)
            buf = b""
            while b"\r\n\r\n" not in buf:
                d = s.recv(4096)
                if not d:
                    break
                buf += d
            s.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok")
            s.close()
        except (OSError, ssl.SSLError):
            try:
                c.close()
            except OSError:
                pass

    def close(self):
        self.sock.close()


def run_daemon(app: Path, work: Path, proxy: str | None, wait: float = 40.0):
    """진짜 데몬을 막힌 소켓 안에서 띄우고, 프록시 쪽 첫 줄이나 막힌 바깥 연결이 보일 때까지 기다린다."""
    py = app / "Contents" / "Resources" / "python_env" / "bin" / "python3"
    tools = app / "Contents" / "Resources" / "tools"
    wrap = work / "wrap_daemon.py"
    wrap.write_text(WRAP, encoding="utf-8")
    log = work / ("blocked_%s.jsonl" % ("proxy" if proxy else "direct"))
    log.write_text("", encoding="utf-8")
    fp = FakeProxy()
    init = dict(TOKENS)
    if proxy:
        init["proxy"] = proxy.replace("PORT", str(fp.port))
    env = {k: v for k, v in os.environ.items() if "proxy" not in k.lower()}
    env.update({"HOME": str(work), "PYTHONNOUSERSITE": "1"})
    # -B — 번들 파이썬이 번들 안에 .pyc 를 쓰면 서명 봉인이 깨진다(doctor 가 잡았다)
    p = subprocess.Popen([str(py), "-I", "-B", str(wrap), str(tools), str(log)], stdin=subprocess.PIPE,
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, cwd=str(work))
    p.stdin.write((json.dumps(init) + "\n").encode())
    p.stdin.flush()
    t0 = time.time()
    blocked = []
    while time.time() - t0 < wait:
        blocked = [json.loads(l) for l in log.read_text(encoding="utf-8").splitlines() if l.strip()]
        if fp.lines or blocked or p.poll() is not None:
            time.sleep(1.5)   # 뒤따르는 것까지
            blocked = [json.loads(l) for l in log.read_text(encoding="utf-8").splitlines() if l.strip()]
            break
        time.sleep(0.3)
    try:
        p.stdin.write(b'{"action": "quit"}\n')
        p.stdin.flush()
    except OSError:
        pass
    try:
        out, _ = p.communicate(timeout=8)
    except subprocess.TimeoutExpired:
        p.kill()
        out, _ = p.communicate()
    fp.close()
    # 데몬은 한 줄에 JSON 하나를 ensure_ascii 로 찍는다 — 한글을 읽을 수 있게 풀어 둔다
    text = []
    for line in (out or b"").decode("utf-8", "replace").splitlines():
        try:
            text.append(json.dumps(json.loads(line), ensure_ascii=False))
        except ValueError:
            text.append(line)
    return fp.lines, blocked, "\n".join(text)


def main() -> int:
    check = C.Checks()
    app = C.test_app()
    if not app or not (app / "Contents" / "Resources" / "python_env" / "bin" / "python3").exists():
        print("번들 파이썬이 없습니다 — 빌드부터")
        return 2
    work = C.work_dir("proxy_exit")

    # ── A. 데몬만 ──
    lines, blocked, out = run_daemon(app, work, "http://127.0.0.1:PORT")
    check("A1 init 의 proxy 로 X 에 간다(가짜 프록시가 CONNECT x.com:443 을 받음)",
          any(l.startswith("CONNECT x.com:443") for l in lines), lines[:3] or out[-300:])
    check("A1 막힌 바깥 연결 없음(프록시를 비켜 간 요청이 없다)", not blocked, blocked[:3])
    check("A1 데몬 기록에 '프록시 사용' (비밀번호는 가려 적는다)", "프록시 사용: http://127.0.0.1:" in out, out[-200:])
    lines, blocked, out = run_daemon(app, work, None)
    check("A2 (대조) proxy 없으면 x.com 으로 바로 가려다 막힌다 — 시험이 우회를 잡아낸다",
          any((b.get("blocked_dns") or b.get("blocked") or "").endswith("x.com") for b in blocked) and not lines,
          (blocked[:3], lines[:2]))

    # ── B. 앱 ──
    tls = TlsOk(work)
    fp = FakeProxy(tunnel=("127.0.0.1", tls.port))
    fq = FakeProxy(tunnel=("127.0.0.1", tls.port))   # 둘째 계정의 출구
    try:
        if not C.iso_start():
            return 2
        home = C.ISO / "home"
        data = home / "Library" / "Application Support" / "Hanishiki"
        ov = data / "script_overrides"
        ov.mkdir(parents=True, exist_ok=True)
        shutil.copy(HERE / "fake_x_daemon.py", ov / "twitter_daemon.py")
        (home / ".curlrc").write_text("--insecure\n", encoding="utf-8")   # 격리 홈에만 — 미리 확인이 이 맥 안의 TLS 서버를 받아들이게
        save = C.ISO / "tmp" / "proxy-e2e"
        shutil.rmtree(save, ignore_errors=True)
        save.mkdir(parents=True)
        w = World(home)
        for i in range(6):
            w.add_tweet(NOW - 3 * 24 * HOUR + i * HOUR)
        w.save()
        initlog = home / "fake_x" / "init.jsonl"

        def run(account, path: str | None = None, kind: str = "tweets", media: bool = False) -> tuple:
            if initlog.exists():
                initlog.unlink()
            cfg = {"platform": "twitter", "target": "tgt", "type": kind, "path": str(save) if path is None else path,
                   "accounts": account if isinstance(account, list) else [account], "realCapture": False, "downloadMedia": media, "excel": False,
                   "mode": "all", "resumeMode": "both", "saveProgress": False, "count": 0}
            DONE = ("완료", "오류", "중지됨", "Error", "Done", "Stopped")
            with C.page() as p:
                p.eval("(typeof platformStats!=='undefined') && (platformStats.twitter = {status: 'akashi-wait'}) && true")
                p.eval("backend.startCollection(%s)" % json.dumps(json.dumps(cfg)))
                t0 = time.time()
                while time.time() - t0 < 60 and C.platform_stats(p, "twitter").get("status") == "akashi-wait":
                    time.sleep(0.5)
                if path == "":
                    # 시작 전에 멈추는 판 — 상태는 '대기' 로 돌아가므로 끝 상태를 기다리지 않는다
                    time.sleep(3)
                    st = C.platform_stats(p, "twitter")
                else:
                    st, _ = C.wait_status(p, "twitter", DONE, 300, step=1.0)
                t1 = time.time()
                while time.time() - t1 < 60 and not p.eval(
                        "(function(){var b=document.getElementById('twitter-stop-btn');return !b||b.disabled;})()", retry=True):
                    time.sleep(1)
                logtxt = p.eval("(document.getElementById('twitter-log')||{}).innerText||''", retry=True) or ""
            time.sleep(2)
            inits = [json.loads(l) for l in initlog.read_text(encoding="utf-8").splitlines() if l.strip()] \
                if initlog.exists() else []
            return st, inits, logtxt

        with C.page() as p:
            time.sleep(1.5)
            p.eval("backend.setProxyProfiles(%s)" % json.dumps(json.dumps(
                [{"provider": "custom", "name": "akashi-p", "type": "http", "host": "127.0.0.1",
                  "port": fp.port, "user": "", "pass": ""},
                 {"provider": "custom", "name": "akashi-q", "type": "http", "host": "127.0.0.1",
                  "port": fq.port, "user": "", "pass": ""}])))
            time.sleep(1)
        before = len(fp.lines)
        st, inits, logtxt = run(dict(TOKENS, proxy="akashi-p"))
        want = "http://127.0.0.1:%d" % fp.port
        check("B3 판이 끝났다", (st or {}).get("status") in ("완료", "Done"), (st, logtxt[-300:]))
        check("B3 앱의 미리 확인이 그 프록시로 갔다", any(l.startswith("CONNECT ") for l in fp.lines[before:]), fp.lines[before:][:3])
        check("B3 데몬 init 의 proxy = 계정 프로필의 주소", bool(inits) and all(i.get("proxy") == want for i in inits),
              (want, inits))
        check("B3 기록에 '🔒 이 계정은 127.0.0.1:%d 로 나갑니다'" % fp.port, ("127.0.0.1:%d 로 나갑니다" % fp.port) in logtxt,
              logtxt[-300:])
        st, inits, logtxt = run(dict(TOKENS))
        check("B4 프로필 없음 — 판이 끝났다", (st or {}).get("status") in ("완료", "Done"), (st, logtxt[-300:]))
        check("B4 프로필 없음 → init 에 proxy 가 없다(직접 연결 — 예전과 같다)",
              bool(inits) and all(i.get("proxy") is None and "proxy" not in i.get("keys", []) for i in inits), inits)
        st, inits, logtxt = run(dict(TOKENS), path="")
        check("B5 저장 경로가 비면 시작 전에 멈추고 알린다(데몬도 띄우지 않는다)",
              "저장 경로가 비어 있습니다" in logtxt and not inits, (st, inits, logtxt[-200:]))
        while not any(int(t["id"][-3:]) % 4 == 0 for t in w.st["own"]):   # 미디어 있는 글이 하나는 있게(가짜 X 는 ID 끝자리로 정한다)
            w.add_tweet(NOW - 2 * 24 * HOUR)
        w.st["rate_limit_once"] = "search"
        w.save()
        accs = [{"name": "A", "auth_token": "akashi-test-token-a", "ct0": "akashi-test-ct0", "proxy": "akashi-p"},
                {"name": "B", "auth_token": "akashi-test-token-b", "ct0": "akashi-test-ct0", "proxy": "akashi-q"}]
        before_q = len(fq.lines)
        # 새 폴더 — 앞 판들이 받아 둔 글은 미디어를 받으러 가지 않는다
        st, inits, logtxt = run(accs, path=str(C.ISO / "tmp" / "proxy-e2e-b6"), kind="all", media=True)
        wantp = {"a": "http://127.0.0.1:%d" % fp.port, "b": "http://127.0.0.1:%d" % fq.port}
        order = [i.get("acct") for i in inits]
        check("B6 판이 끝났다", (st or {}).get("status") in ("완료", "Done"), (st, logtxt[-300:]))
        check("B6 계정이 돌았고(B) 0번(A)으로 돌아왔다", "b" in order and "a" in order[order.index("b"):], order)
        check("B6 데몬을 띄울 때마다 그 계정의 출구", bool(inits) and all(i.get("proxy") == wantp.get(i.get("acct")) for i in inits), inits)
        check("B6 돌고 난 뒤의 미디어는 둘째 계정의 출구로 나갔다", any(l.startswith("CONNECT ") for l in fq.lines[before_q:]), fq.lines[before_q:][:3])
    except Exception as e:  # noqa: BLE001
        check("시험 진행 중 오류 없음", False, "%s: %s" % (type(e).__name__, e))
    finally:
        C.keep_log("proxy_exit_app.log")
        C.iso_stop()
        fp.close()
        fq.close()
        tls.close()
    return check.summary()


if __name__ == "__main__":
    sys.exit(main())
