# -*- coding: utf-8 -*-
"""한이시키 화면(QtWebEngine)을 CDP 로 다루는 얇은 층.

격리 사본(iso.py)이 QTWEBENGINE_REMOTE_DEBUGGING=127.0.0.1:<포트> 로 뜨면, 그 화면에 붙어
JS 를 돌리고 · 창 폭을 흉내 내고 · 그림을 찍는다. 모든 점검 공구(check_*.py)가 이것을 쓴다.

    from lib.cdp import Page
    with Page.attach(9334) as p:
        p.eval("switchTab('twitter')")
        p.size(900, 760)
        p.shot("out.png")

★ 지켜야 할 것
  · 사용자가 켜 둔 앱에는 붙지 않는다. 그 앱은 원격 디버깅을 켜지 않으므로 애초에 포트가 없다.
    포트가 열려 있으면 그것은 격리 사본이다 — 그래도 attach() 는 127.0.0.1 만 본다.
  · eval 결과로 쿠키·토큰을 돌려받지 않는다. 설정 값을 볼 일이 있으면 '길이·있음/없음' 만.

★ 다시 붙기(reconnect=True — 기능 시험이 켠다)
  덮개를 닫은 맥(외부 화면 없음 · 전원 연결)에서는 앱이 수집을 시작할 때마다 caffeinate -u 로 맥을 FullWake 로 깨우고,
  10초 뒤 macOS 가 'Clamshell Sleep' 으로 DarkWake 에 다시 넣는다. 그 순간(0~3초 안) 앱의 CDP 서버가 연결을 닫는 일이
  있었다 — 2026-10-17 05:53 · 05:54 · 06:29 · 06:31, 매번 'Entering DarkWake' 바로 뒤. 앱은 살아 있었다. 화면이 그대로면
  같은 target 에 다시 붙어 이어 가면 되고, 바뀌었으면 아래 PageGone 으로 드러난다. 그래서 끊기면(FIN → WsClosed · 덜 읽힌 요청이 있으면 RST → ConnectionReset)
  같은 target id 에 다시 붙고, 켜 두었던 영역(Runtime · Log …)을 다시 켜고, 화면 JS 에 둔 표식(mark)이 그대로인지 본다.
  · 보내기 전에 이미 와 있는 것을 거둔다(_preflight). 서버가 그 전에 닫았으면 거기서 드러나고, 보내려던 명령은 아직 가지
    않았으니 다시 붙어 보내면 된다 — 시험은 판 사이에 몇 초씩 자므로 끊김은 대개 여기서 드러난다(startCrawl · stopCrawl 도 무사).
  · 보낸 뒤 답을 기다리다 끊기면 그 명령이 화면에서 돌았는지 알 길이 없다. retry=True 로 부른 것(읽기만 하는 것)만 되풀이하고,
    나머지는 Dropped — 화면을 바꾸는 명령을 두 번 돌리면 판정이 엉뚱해진다.
  · 그 화면이 없거나 표식이 사라졌으면(새로 떴음) PageGone — 그건 연결 탓이 아니라 앱 쪽 일이라 숨기지 않는다.
  · 다시 켠 Runtime · Log 는 지난 콘솔 · 로그를 다시 보낸다. collect_errors 가 since_ms(watch 시작) 앞의 것과 겹친 것을 버린다.
  · 되살리는 것은 X.enable 뿐이다. Emulation(size · dark) 같은 연결별 설정은 다시 걸어야 한다 — 기능 시험은 쓰지 않는다.
"""
from __future__ import annotations

import base64
import json
import os
import socket
import time
import urllib.request
from urllib.parse import urlparse

from .ws import WebSocket, WsClosed, WsError


class CdpError(Exception):
    pass


class JsError(CdpError):
    """화면 쪽 JS 가 던진 예외."""


class PageGone(CdpError):
    """다시 붙으려 했더니 그 화면(target)이 없다 · 화면이 새로 떠 JS 상태가 사라졌다 — 연결 탓이 아니라 앱 쪽 일."""


class Dropped(CdpError):
    """연결이 끊겨 같은 화면에 다시 붙었지만, 끊긴 명령은 되풀이하지 않았다(화면을 바꾸는 명령일 수 있어서)."""


# 연결이 끊긴 것 — 앱이 닫았다(FIN → WsClosed) · 닫을 때 우리 요청이 덜 읽혀 있었다(RST → ConnectionReset) · 보내다 끊김.
#   socket.timeout 은 들지 않는다 — 그건 '답이 늦다' 이지 '끊겼다' 가 아니다.
DROPPED = (ConnectionResetError, ConnectionAbortedError, BrokenPipeError, WsClosed)


# ★ 프록시를 거치지 않는다. urlopen 은 http_proxy·시스템 프록시를 따르는데, no_proxy 가 없으면 127.0.0.1 도
#   프록시로 보낸다(파이썬 3.9 에서 확인) — 로컬 CDP 목록 요청이 바깥 프록시로 나가고, 붙기도 실패한다.
#   hotfix_lab 의 로컬 요청과 같은 방법이다.
_LOCAL = urllib.request.build_opener(urllib.request.ProxyHandler({}))


def targets(port: int, timeout: float = 3.0) -> list:
    with _LOCAL.open("http://127.0.0.1:%d/json" % port, timeout=timeout) as r:
        return json.load(r)


def page_targets(port: int, match: str = "index.html") -> list:
    return [t for t in targets(port) if t.get("type") == "page" and match in t.get("url", "")]


def wait_for_page(port: int, match: str = "index.html", timeout: float = 45.0) -> dict:
    """앱이 뜨고 화면이 준비될 때까지 기다린다."""
    end = time.time() + timeout
    last = None
    while time.time() < end:
        try:
            ts = page_targets(port, match)
            if ts:
                return ts[0]
        except (OSError, ValueError) as e:  # 아직 포트가 안 열림 · 뜨는 중이라 목록이 반쪽(JSON 아님)
            last = e
        time.sleep(0.5)
    raise CdpError("%d초 안에 화면(%s)이 뜨지 않았습니다 (%s)" % (timeout, match, last))


class Page:
    def __init__(self, ws_url: str, timeout: float = 30.0, reconnect: bool = False):
        self.ws_url = ws_url
        u = urlparse(ws_url)
        self.port = u.port
        self.target_id = u.path.rsplit("/", 1)[-1]
        self.ws = WebSocket(ws_url, timeout=timeout)
        self.timeout = timeout
        self._id = 0
        self.events: list = []          # 기다리는 동안 받은 이벤트(콘솔·예외 등)
        self.auto_reconnect = reconnect  # 끊기면 같은 화면에 다시 붙는다(위 ★ 다시 붙기)
        self.enabled: list = []         # 켠 영역(Runtime.enable · Log.enable …) — 다시 붙으면 다시 켠다
        self.since_ms = 0.0             # 이 시각(1970 기준 밀리초) 앞의 화면 오류는 collect_errors 가 버린다
        self.marker = None              # 화면 JS 에 둔 표식(mark) — 다시 붙은 뒤에도 같으면 같은 화면이다
        self.drops: list = []           # 끊겼다 다시 붙은 기록 {'at': 시각, 'why': 까닭, 'took': 초}
        self.on_drop = None             # 다시 붙은 뒤 부를 함수(기록 dict 를 받아 덧붙일 수 있다 — 시험이 전원 상태를 적는다)

    # ── 붙기 ────────────────────────────────────────────────────────────
    @classmethod
    def attach(cls, port: int = 9334, match: str = "index.html", timeout: float = 30.0, index: int = 0,
               reconnect: bool = False) -> "Page":
        ts = page_targets(port, match)
        if not ts:
            raise CdpError("포트 %d 에 '%s' 화면이 없습니다. iso.py start 로 격리 사본을 먼저 띄우십시오." % (port, match))
        if index >= len(ts):
            raise CdpError("화면이 %d 개뿐입니다 (index=%d)" % (len(ts), index))
        return cls(ts[index]["webSocketDebuggerUrl"], timeout=timeout, reconnect=reconnect)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def close(self) -> None:
        self.ws.close()

    # ── 명령 ────────────────────────────────────────────────────────────
    def cmd(self, method: str, params: dict | None = None, timeout: float | None = None, retry: bool = False) -> dict:
        """명령 하나를 보내고 답을 기다린다.

        다시 붙기를 켠 Page(reconnect=True)는 — 보내기 전에 이미 끊겨 있었으면(서버가 닫음) 다시 붙어 보낸다. 그 명령은 아직
        가지 않았으니 무엇이든 괜찮다(시험은 판 사이에 몇 초씩 자므로 끊김은 대개 여기서 드러난다). 보낸 뒤 답을 기다리다
        끊기면 화면에서 돌았는지 알 수 없다 — retry=True(읽기만 하는 명령)일 때만 한 번 더 보내고, 아니면 다시 붙어 둔 채 Dropped."""
        if not self.auto_reconnect:
            r = self._wait(self._send(method, params), method, timeout)
        else:
            r = self._cmd_reconnecting(method, params, timeout, retry)
        if method.endswith(".enable") and method not in self.enabled:
            self.enabled.append(method)
        return r

    def _cmd_reconnecting(self, method: str, params: dict | None, timeout: float | None, retry: bool) -> dict:
        for _ in range(3):
            try:
                self._preflight()
                mid = self._send(method, params)
            except DROPPED as e:                 # 아직 가지 않았다 — 다시 붙어 보내면 된다
                self.reconnect(e)
                continue
            try:
                return self._wait(mid, method, timeout)
            except DROPPED as e:                 # 보낸 뒤에 끊겼다 — 화면에서 돌았는지 모른다
                self.reconnect(e)
                if not retry:
                    raise Dropped("%s — 답을 기다리다 연결이 끊겨(%s) 같은 화면에 다시 붙었지만, 이 명령은 되풀이하지 않습니다"
                                  "(화면을 바꾸는 명령일 수 있어 — 끊긴 순간 돌았는지 알 수 없음)" % (method, type(e).__name__))
        raise CdpError("%s — 다시 붙어도 연결이 거듭 끊깁니다" % method)

    def _send(self, method: str, params: dict | None) -> int:
        self._id += 1
        self.ws.send(json.dumps({"id": self._id, "method": method, "params": params or {}}))
        return self._id

    def _wait(self, mid: int, method: str, timeout: float | None) -> dict:
        end = time.time() + (timeout or self.timeout)
        while True:
            left = end - time.time()
            if left <= 0:
                raise CdpError("%s — %.0f초 안에 답이 없습니다" % (method, timeout or self.timeout))
            try:
                msg = json.loads(self.ws.recv(timeout=left))
            except socket.timeout:
                continue
            if msg.get("id") == mid:
                if "error" in msg:
                    raise CdpError("%s: %s" % (method, msg["error"].get("message")))
                return msg.get("result", {})
            if "method" in msg:
                self.events.append(msg)

    def _preflight(self) -> None:
        """보내기 전에 이미 와 있는 것을 거둔다. 그 사이 서버가 연결을 닫았으면(FIN · RST) 여기서 WsClosed · ConnectionReset 으로
        드러난다 — 그러면 보내려던 명령은 아직 가지 않은 것이다."""
        while self.ws.readable():
            try:
                msg = json.loads(self.ws.recv(timeout=1.0))
            except socket.timeout:               # 프레임이 반만 와 있었다 — 나머지는 다음 recv 가 잇는다(ws.py)
                return
            if "method" in msg:
                self.events.append(msg)

    def eval(self, expr: str, timeout: float | None = None, retry: bool = False):
        """JS 식을 돌려 값(JSON 으로 바꿀 수 있는 것)을 돌려준다. 예외는 JsError.
        retry=True 는 읽기만 하는 식에만 — 연결이 끊겨 다시 붙으면 한 번 더 돌린다(cmd 참고)."""
        r = self.cmd("Runtime.evaluate",
                     {"expression": expr, "returnByValue": True, "awaitPromise": True}, timeout, retry=retry)
        if "exceptionDetails" in r:
            d = r["exceptionDetails"]
            desc = (d.get("exception") or {}).get("description") or d.get("text") or "?"
            raise JsError(desc.splitlines()[0][:300])
        return (r.get("result") or {}).get("value")

    def wait_for(self, expr: str, timeout: float = 10.0, every: float = 0.2):
        """식이 참이 될 때까지 기다린다. 끝내 거짓이면 CdpError."""
        end = time.time() + timeout
        while time.time() < end:
            v = self.eval(expr, retry=True)      # 기다리는 식은 읽기만 한다 — 끊겨도 다시 대 보면 그만
            if v:
                return v
            time.sleep(every)
        raise CdpError("기다렸지만 참이 되지 않았습니다: " + expr[:120])

    def pump(self, seconds: float) -> None:
        """그동안 오는 이벤트를 모은다(명령 없이 기다리기). 다시 붙기를 켰으면 끊겨도 다시 붙는다(되풀이할 명령이 없다)."""
        if not self.auto_reconnect:
            return self._drain(seconds)
        try:
            self._drain(seconds, raise_drop=True)
        except DROPPED as e:
            self.reconnect(e)

    def _drain(self, seconds: float, raise_drop: bool = False) -> None:
        end = time.time() + seconds
        while True:
            left = end - time.time()
            if left <= 0:
                return
            try:
                msg = json.loads(self.ws.recv(timeout=left))
            except socket.timeout:
                return
            except WsClosed:
                if raise_drop:
                    raise
                return
            if "method" in msg:
                self.events.append(msg)

    # ── 다시 붙기 ────────────────────────────────────────────────────────
    def mark(self) -> str:
        """화면 JS 에 표식을 둔다. 다시 붙은 뒤에도 표식이 그대로면 같은 화면(새로 고쳐지지 않음)이다."""
        self.marker = "akashi-%d-%d" % (os.getpid(), int(time.time() * 1000))
        self.eval("window.__akashiMark=%s" % json.dumps(self.marker))
        return self.marker

    def reconnect(self, why: BaseException, wait: float = 20.0) -> None:
        """끊긴 연결을 같은 화면(target id)에 다시 잇는다. 그 화면이 없거나 새로 떴으면 PageGone, 끝내 못 붙으면 CdpError."""
        t0 = time.time()
        last = None
        while True:
            try:
                self.ws.close()
            except OSError:
                pass
            try:
                ids = [t.get("id") for t in targets(self.port)]
                if self.target_id not in ids:
                    raise PageGone("다시 붙으려 했더니 화면(target %s…)이 없습니다 — 닫혔거나 새로 떴습니다 (끊긴 까닭: %s: %s)"
                                   % (self.target_id[:8], type(why).__name__, why))
                self.ws = WebSocket(self.ws_url, timeout=self.timeout)
                for m in list(self.enabled):      # 켜 두었던 영역을 다시 — 안 켜면 '화면 JS 오류 없음' 이 늘 통과한다
                    self._wait(self._send(m, None), m, None)
                if self.marker is not None:
                    r = self._wait(self._send("Runtime.evaluate", {"expression": "window.__akashiMark", "returnByValue": True}),
                                   "Runtime.evaluate", None)
                    got = (r.get("result") or {}).get("value")
                    if got != self.marker:
                        raise PageGone("같은 화면에 다시 붙었지만 JS 상태가 사라졌습니다(새로 고쳐짐 — 표식 %r → %r) (끊긴 까닭: %s: %s)"
                                       % (self.marker, got, type(why).__name__, why))
                self._drain(0.4, raise_drop=True)  # 다시 켠 영역이 보내는 지난 콘솔 · 로그 — collect_errors 가 겹친 것을 거른다
                break
            except (OSError, ValueError, WsError) as e:   # 앱이 잠깐 답하지 않거나 붙자마자 또 끊김(DarkWake 로 넘어가는 중 등)
                last = e
                if time.time() - t0 > wait:
                    raise CdpError("연결이 끊긴 뒤 %.0f초 동안 다시 붙지 못했습니다 (끊긴 까닭: %s: %s · 마지막: %s: %s)"
                                   % (wait, type(why).__name__, why, type(last).__name__, last))
                time.sleep(1.0)
        rec = {"at": t0, "why": "%s: %s" % (type(why).__name__, why), "took": time.time() - t0}
        self.drops.append(rec)
        if self.on_drop:
            self.on_drop(rec)

    # ── 화면 ────────────────────────────────────────────────────────────
    def size(self, width: int, height: int = 800, settle: float = 0.8) -> None:
        """창 폭을 흉내 낸다(CSS px). 앱의 배율(zoomForWidth)을 거친 '화면 안쪽 폭' 이다."""
        self.cmd("Emulation.setDeviceMetricsOverride",
                 {"width": int(width), "height": int(height), "deviceScaleFactor": 1, "mobile": False})
        time.sleep(settle)

    def clear_size(self) -> None:
        self.cmd("Emulation.clearDeviceMetricsOverride")

    def dark(self, on: bool = True) -> None:
        self.cmd("Emulation.setEmulatedMedia",
                 {"features": [{"name": "prefers-color-scheme", "value": "dark" if on else "light"}]})

    def shot(self, path: str) -> str:
        r = self.cmd("Page.captureScreenshot", {"format": "png"}, timeout=30)
        with open(path, "wb") as f:
            f.write(base64.b64decode(r["data"]))
        return path

    def reload(self, settle: float = 5.0, wait_expr: str = "typeof backend !== 'undefined' && !!window.switchTab") -> None:
        self.cmd("Page.enable")
        self.cmd("Page.reload", {"ignoreCache": True})
        time.sleep(1.0)
        try:
            self.wait_for(wait_expr, timeout=settle + 20)
        except CdpError:
            pass
        time.sleep(max(0.0, settle - 1.0))
        if self.marker is not None:           # 새로 고친 화면엔 표식이 없다 — 다시 둔다(안 그러면 다시 붙을 때 PageGone)
            self.mark()

    def mouse_drag(self, x: float, y: float, dx: float, dy: float, steps: int = 6) -> None:
        """화면 '안' 의 누르고-끌기(격리 사본 시험용). OS 마우스를 흉내 내는 것이 아니다."""
        self.cmd("Input.dispatchMouseEvent", {"type": "mousePressed", "x": x, "y": y, "button": "left", "buttons": 1, "clickCount": 1})
        for k in range(1, steps + 1):
            self.cmd("Input.dispatchMouseEvent", {"type": "mouseMoved", "x": x + dx * k / steps, "y": y + dy * k / steps, "button": "left", "buttons": 1})
            time.sleep(0.03)
        self.cmd("Input.dispatchMouseEvent", {"type": "mouseReleased", "x": x + dx, "y": y + dy, "button": "left", "buttons": 0, "clickCount": 1})

    # ── 오류 모으기 ──────────────────────────────────────────────────────
    def collect_errors(self) -> list:
        """지금까지 모은 이벤트 중 JS 예외·console.error 를 글로 돌려준다."""
        out = []
        seen = set()
        for m in self.events:
            meth, pr = m.get("method"), m.get("params", {})
            if meth == "Runtime.exceptionThrown":
                d = pr.get("exceptionDetails", {})
                line = "예외: " + ((d.get("exception") or {}).get("description") or d.get("text") or "?").splitlines()[0][:240]
            elif meth == "Runtime.consoleAPICalled" and pr.get("type") == "error":
                args = pr.get("args") or [{}]
                line = "console.error: " + str(args[0].get("value", args[0].get("description", "")))[:240]
            elif meth == "Log.entryAdded" and (pr.get("entry") or {}).get("level") == "error":
                line = "log: " + (pr["entry"].get("text") or "")[:240]
            else:
                continue
            # 다시 붙어 Runtime · Log 를 다시 켜면 지난 것을 다시 보낸다(새 objectId 로) — watch 앞의 것과 이미 받은 것은 건너뛴다
            ts = pr.get("timestamp") or (pr.get("entry") or {}).get("timestamp")
            if ts and self.since_ms and ts < self.since_ms:
                continue
            if ts:
                if (line, ts) in seen:
                    continue
                seen.add((line, ts))
            out.append(line)
        return out
