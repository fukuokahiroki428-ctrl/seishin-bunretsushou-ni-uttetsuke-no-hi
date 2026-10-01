# -*- coding: utf-8 -*-
"""표준 라이브러리만으로 된 작은 웹소켓 클라이언트 — CDP(127.0.0.1) 전용.

★ 왜 직접 짰나:
  공구함은 '어느 맥에서든 받자마자 돈다' 가 첫째다. websockets 패키지는 앱에 든 파이썬에는
  있지만 맥 기본 파이썬(/usr/bin/python3 3.9)에는 없다. 공구 하나 쓰려고 pip 부터 하게 하면
  급할 때 못 쓴다. CDP 는 로컬 평문(ws://) · 텍스트 프레임뿐이라 RFC 6455 의 작은 부분만 있으면 된다.

지원: ws:// 만, 텍스트·이어짐·ping/pong·close 프레임, 64비트 길이(스크린숏은 수 MB 다).
하지 않는 것: wss://, 확장(permessage-deflate), 바이너리 보내기.
"""
from __future__ import annotations

import base64
import hashlib
import os
import socket
import struct
from urllib.parse import urlparse

_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WsError(Exception):
    pass


class WsClosed(WsError):
    pass


class WebSocket:
    def __init__(self, url: str, timeout: float = 10.0):
        u = urlparse(url)
        if u.scheme != "ws":
            raise WsError("ws:// 만 됩니다: " + url)
        host, port = u.hostname or "127.0.0.1", u.port or 80
        path = u.path or "/"
        if u.query:
            path += "?" + u.query
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)
        self.send_timeout = timeout
        self._buf = b""
        self._parts = []   # 이어지는 조각(fin 이 아닌 프레임)은 제한 시간을 넘겨도 여기 남는다
        self.closed = False
        # ★ 핸드셰이크가 실패하면 소켓을 닫고 던진다. 안 닫으면 붙기를 되풀이하는 공구(사본을 다시 띄우며
        #   기다리는 hotfix_lab 등)가 실패할 때마다 파일 기술자를 하나씩 흘린다.
        try:
            self._handshake(host, port, path)
        except BaseException:
            self.closed = True
            try:
                self.sock.close()
            except OSError:
                pass
            raise

    def _handshake(self, host, port, path) -> None:
        key = base64.b64encode(os.urandom(16)).decode()
        req = (
            "GET {p} HTTP/1.1\r\nHost: {h}:{o}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
            "Sec-WebSocket-Key: {k}\r\nSec-WebSocket-Version: 13\r\n\r\n"
        ).format(p=path, h=host, o=port, k=key)
        self.sock.sendall(req.encode())
        head = self._read_until(b"\r\n\r\n")
        status = head.split(b"\r\n", 1)[0]
        if b" 101 " not in status + b" ":
            raise WsError("핸드셰이크 거절: " + status.decode("latin-1"))
        want = base64.b64encode(hashlib.sha1((key + _GUID).encode()).digest())
        ok = any(
            line.lower().startswith(b"sec-websocket-accept:") and line.split(b":", 1)[1].strip() == want
            for line in head.split(b"\r\n")
        )
        if not ok:
            raise WsError("Sec-WebSocket-Accept 가 맞지 않습니다")

    # ── 내부 ────────────────────────────────────────────────────────────
    def _read_until(self, mark: bytes) -> bytes:
        while mark not in self._buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise WsClosed("연결이 끊겼습니다(핸드셰이크 중)")
            self._buf += chunk
        head, self._buf = self._buf.split(mark, 1)
        return head

    def _send_frame(self, opcode: int, payload: bytes) -> None:
        # 클라이언트 → 서버 프레임은 반드시 가린다(mask). 안 가리면 서버가 끊는다.
        head = bytes([0x80 | opcode])
        n = len(payload)
        if n < 126:
            head += bytes([0x80 | n])
        elif n < 65536:
            head += bytes([0x80 | 126]) + struct.pack("!H", n)
        else:
            head += bytes([0x80 | 127]) + struct.pack("!Q", n)
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload)) if n < 4096 else _mask_fast(payload, mask)
        # ★ 받을 때 건 짧은 제한 시간(pump 의 '남은 0.001초' 같은)이 소켓에 남아 있으면 보내기에도 그대로 걸린다.
        #   수십 KB 짜리 JS 를 보내다 중간에 시간이 다 되면 반쪽 프레임이 나가고, 서버는 연결을 끊는다
        #   (공구들이 폭을 바꾸자마자 'Connection reset by peer' 로 멈추던 까닭). 보낼 땐 늘 넉넉히 기다린다.
        # ★ 그리고 보낸 뒤엔 받을 때의 제한 시간으로 되돌린다. recv 가 ping 에 pong 으로 답한 뒤 같은 recv 안에서
        #   다시 읽을 때, 넉넉한 보내기 시간(30초)이 남아 있으면 pump(0.3초)가 그만큼 멈춰 선다.
        prev = self.sock.gettimeout()
        self.sock.settimeout(self.send_timeout)
        try:
            self.sock.sendall(head + mask + masked)
        except OSError:
            # ★ 보내다 끊기면 반쪽 프레임이 나갔을 수 있다 — 이 연결로 더 보내면 서버가 틀을 잃는다. 닫힌 것으로 둔다.
            self.closed = True
            try:
                self.sock.close()
            except OSError:
                pass
            raise
        finally:
            if not self.closed:
                self.sock.settimeout(prev)

    # ── 바깥 ────────────────────────────────────────────────────────────
    def send(self, text: str) -> None:
        if self.closed:
            raise WsClosed("닫힌 연결")
        self._send_frame(0x1, text.encode("utf-8"))

    def _take_frame(self):
        """버퍼에 '통째로' 들어와 있는 프레임 하나를 꺼낸다. 아직 덜 왔으면 None — 아무것도 꺼내지 않는다.

        ★ 예전엔 머리 2바이트를 먼저 꺼내고 나머지를 읽다가 제한 시간(pump 의 짧은 기다림)에 걸리면
          머리를 잃은 채 예외가 났다. 다음 읽기부터 틀이 어긋나 엉뚱한 길이·opcode 를 읽었고,
          수집 공구가 새로 고친 뒤 몇 초 만에 '연결이 끊겼습니다' 로 멈췄다. 이제 다 온 프레임만 꺼낸다."""
        b = self._buf
        if len(b) < 2:
            return None
        b1, b2 = b[0], b[1]
        n, pos = b2 & 0x7F, 2
        if n == 126:
            if len(b) < 4:
                return None
            n, pos = struct.unpack("!H", b[2:4])[0], 4
        elif n == 127:
            if len(b) < 10:
                return None
            n, pos = struct.unpack("!Q", b[2:10])[0], 10
        mask = None
        if b2 & 0x80:  # 서버가 가렸다(규약 위반이지만 풀어 준다)
            if len(b) < pos + 4:
                return None
            mask, pos = b[pos:pos + 4], pos + 4
        if len(b) < pos + n:
            return None
        data = b[pos:pos + n]
        self._buf = b[pos + n:]
        if mask:
            data = _mask_fast(data, mask)
        return b1 & 0x80, b1 & 0x0F, data

    def recv(self, timeout: float | None = None) -> str:
        """텍스트 메시지 하나를 돌려준다. 제한 시간이 지나면 socket.timeout — 받던 조각은 버퍼에 남는다."""
        if self.closed:
            raise WsClosed("닫힌 연결")
        if timeout is not None:
            self.sock.settimeout(timeout)
        while True:
            fr = self._take_frame()
            if fr is None:
                chunk = self.sock.recv(262144)   # 여기서 시간이 다 되면 socket.timeout — 잃는 것이 없다
                if not chunk:
                    self.closed = True
                    raise WsClosed("연결이 끊겼습니다")
                self._buf += chunk
                continue
            fin, opcode, data = fr
            if opcode == 0x9:        # ping → pong
                self._send_frame(0xA, data)
                continue
            if opcode == 0xA:        # pong
                continue
            if opcode == 0x8:        # close
                self.closed = True
                raise WsClosed("서버가 닫았습니다")
            if opcode in (0x1, 0x2, 0x0):
                self._parts.append(data)
                if fin:
                    msg, self._parts = b"".join(self._parts), []
                    return msg.decode("utf-8", "replace")

    def close(self) -> None:
        # ★ 서버가 먼저 닫았어도(closed=True) 소켓은 여기서 닫는다 — 예전엔 곧장 돌아가 기술자가 남았다.
        if not self.closed:
            try:
                self._send_frame(0x8, struct.pack("!H", 1000))
            except OSError:
                pass
            self.closed = True
        try:
            self.sock.close()
        except OSError:
            pass


def _mask_fast(data: bytes, mask: bytes) -> bytes:
    # 큰 덩어리(스크린숏 요청 등)는 정수 XOR 로 한 번에 — 바이트마다 돌면 수 MB 에 수 초가 걸린다.
    n = len(data)
    if not n:
        return b""
    rep = (mask * (n // 4 + 1))[:n]
    return (int.from_bytes(data, "big") ^ int.from_bytes(rep, "big")).to_bytes(n, "big")
