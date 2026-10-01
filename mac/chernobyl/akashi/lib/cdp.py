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
"""
from __future__ import annotations

import base64
import json
import socket
import time
import urllib.request

from .ws import WebSocket, WsClosed


class CdpError(Exception):
    pass


class JsError(CdpError):
    """화면 쪽 JS 가 던진 예외."""


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
    def __init__(self, ws_url: str, timeout: float = 30.0):
        self.ws = WebSocket(ws_url, timeout=timeout)
        self.timeout = timeout
        self._id = 0
        self.events: list = []          # 기다리는 동안 받은 이벤트(콘솔·예외 등)

    # ── 붙기 ────────────────────────────────────────────────────────────
    @classmethod
    def attach(cls, port: int = 9334, match: str = "index.html", timeout: float = 30.0, index: int = 0) -> "Page":
        ts = page_targets(port, match)
        if not ts:
            raise CdpError("포트 %d 에 '%s' 화면이 없습니다. iso.py start 로 격리 사본을 먼저 띄우십시오." % (port, match))
        if index >= len(ts):
            raise CdpError("화면이 %d 개뿐입니다 (index=%d)" % (len(ts), index))
        return cls(ts[index]["webSocketDebuggerUrl"], timeout=timeout)

    def __enter__(self):
        return self

    def __exit__(self, *a):
        self.close()

    def close(self) -> None:
        self.ws.close()

    # ── 명령 ────────────────────────────────────────────────────────────
    def cmd(self, method: str, params: dict | None = None, timeout: float | None = None) -> dict:
        self._id += 1
        mid = self._id
        self.ws.send(json.dumps({"id": mid, "method": method, "params": params or {}}))
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

    def eval(self, expr: str, timeout: float | None = None):
        """JS 식을 돌려 값(JSON 으로 바꿀 수 있는 것)을 돌려준다. 예외는 JsError."""
        r = self.cmd("Runtime.evaluate",
                     {"expression": expr, "returnByValue": True, "awaitPromise": True}, timeout)
        if "exceptionDetails" in r:
            d = r["exceptionDetails"]
            desc = (d.get("exception") or {}).get("description") or d.get("text") or "?"
            raise JsError(desc.splitlines()[0][:300])
        return (r.get("result") or {}).get("value")

    def wait_for(self, expr: str, timeout: float = 10.0, every: float = 0.2):
        """식이 참이 될 때까지 기다린다. 끝내 거짓이면 CdpError."""
        end = time.time() + timeout
        while time.time() < end:
            v = self.eval(expr)
            if v:
                return v
            time.sleep(every)
        raise CdpError("기다렸지만 참이 되지 않았습니다: " + expr[:120])

    def pump(self, seconds: float) -> None:
        """그동안 오는 이벤트를 모은다(명령 없이 기다리기)."""
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
                return
            if "method" in msg:
                self.events.append(msg)

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
        for m in self.events:
            meth, pr = m.get("method"), m.get("params", {})
            if meth == "Runtime.exceptionThrown":
                d = pr.get("exceptionDetails", {})
                out.append("예외: " + ((d.get("exception") or {}).get("description") or d.get("text") or "?").splitlines()[0][:240])
            elif meth == "Runtime.consoleAPICalled" and pr.get("type") == "error":
                args = pr.get("args") or [{}]
                out.append("console.error: " + str(args[0].get("value", args[0].get("description", "")))[:240])
            elif meth == "Log.entryAdded" and (pr.get("entry") or {}).get("level") == "error":
                out.append("log: " + (pr["entry"].get("text") or "")[:240])
        return out
