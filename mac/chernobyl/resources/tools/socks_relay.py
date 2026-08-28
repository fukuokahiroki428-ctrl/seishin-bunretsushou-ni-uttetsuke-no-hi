#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""로컬 SOCKS5 중계기 — 인증이 붙은 상위 프록시를 '인증 없는 로컬 주소' 로 바꿔 준다.

★ 왜 이게 필요한가.
  이 앱은 요청을 다섯 갈래로 내보낸다(Qt·파이썬 데몬·yt-dlp·rclone·번들 크로미움).
  프록시를 쓰기로 했으면 다섯이 '전부' 같은 IP 로 나가야 한다. 하나라도 빠지면
  같은 세션이 두 IP 에서 온 것처럼 보여, 프록시를 안 쓰느니만 못하다.

  그런데 크로미움은 SOCKS5 의 '사용자/비밀번호 인증' 을 아예 지원하지 않는다
  (--proxy-server 플래그에 자격증명을 넣을 방법이 없다). NordVPN 을 비롯한
  상용 SOCKS5 는 인증이 필수다. 그래서 크로미움만 늘 진짜 IP 로 샜다.

  → 앱이 127.0.0.1 에 인증 없는 SOCKS5 를 하나 열고, 그것이 상위로는 인증해서
    연결한다. 크로미움에는 로컬 주소만 알려 주면 된다.
    덤으로 자격증명이 크로미움 명령줄에 실리지 않는다.

★ 안전.
  · 127.0.0.1 에만 바인딩한다. 다른 기기에서 이 중계기를 쓸 수 없다.
  · 자격증명은 argv 로 받지 않는다(ps 로 남이 읽는다). 첫 줄을 stdin 으로 받는다.
  · CONNECT 만 중계한다(BIND·UDP 는 거절). 열어 둘 필요가 없다.

사용:  socks_relay.py --stdin-args
       stdin 첫 줄: {"listen_port":1080,"host":"...","port":1080,"user":"...","pass":"..."}
       준비되면 stdout 에 {"ready":true,"port":<실제 포트>} 한 줄.
"""
import asyncio
import json
import os
import socket
import struct
import sys

ATYP_IPV4, ATYP_HOST, ATYP_IPV6 = 1, 3, 4


def _read_init():
    """자격증명을 stdin 첫 줄로 받는다 — 명령줄은 같은 기계의 아무 프로세스나 읽는다."""
    if len(sys.argv) >= 2 and sys.argv[1] == "--stdin-args":
        buf = b""
        while True:
            ch = os.read(0, 1)
            if not ch or ch == b"\n":
                break
            buf += ch
        return json.loads(buf.decode("utf-8", "replace"))
    if len(sys.argv) >= 2:
        return json.loads(sys.argv[1])
    print(json.dumps({"error": "init args required (--stdin-args)"}), flush=True)
    sys.exit(1)


async def _recv_exact(reader, n):
    return await reader.readexactly(n)


async def _client_handshake(reader, writer):
    """클라이언트(크로미움 등)와 인증 없는 SOCKS5 로 악수하고, 목적지를 돌려준다."""
    ver, nmethods = struct.unpack("!BB", await _recv_exact(reader, 2))
    if ver != 5:
        raise ValueError("SOCKS5 아님")
    await _recv_exact(reader, nmethods)          # 제시한 방식 목록은 무시
    writer.write(b"\x05\x00")                     # 인증 없음
    await writer.drain()

    ver, cmd, _rsv, atyp = struct.unpack("!BBBB", await _recv_exact(reader, 4))
    if ver != 5:
        raise ValueError("SOCKS5 아님")
    if cmd != 1:                                  # CONNECT 만
        writer.write(b"\x05\x07\x00\x01" + b"\x00" * 6)
        await writer.drain()
        raise ValueError("CONNECT 만 중계한다")

    if atyp == ATYP_IPV4:
        host = socket.inet_ntoa(await _recv_exact(reader, 4))
    elif atyp == ATYP_HOST:
        ln = (await _recv_exact(reader, 1))[0]
        host = (await _recv_exact(reader, ln)).decode("utf-8", "replace")
    elif atyp == ATYP_IPV6:
        host = socket.inet_ntop(socket.AF_INET6, await _recv_exact(reader, 16))
    else:
        raise ValueError("모르는 주소 종류")
    port = struct.unpack("!H", await _recv_exact(reader, 2))[0]
    return atyp, host, port


async def _upstream_connect(cfg, atyp, host, port):
    """상위 SOCKS5 에 인증해서 붙고, 같은 목적지로 CONNECT 한다."""
    ureader, uwriter = await asyncio.open_connection(cfg["host"], int(cfg["port"]))

    user = (cfg.get("user") or "").encode("utf-8")
    pw = (cfg.get("pass") or "").encode("utf-8")
    if user:
        uwriter.write(b"\x05\x01\x02")            # 사용자/비밀번호
        await uwriter.drain()
        ver, method = struct.unpack("!BB", await _recv_exact(ureader, 2))
        if method != 2:
            raise ValueError("상위 프록시가 사용자/비밀번호 인증을 받지 않는다")
        uwriter.write(b"\x01" + bytes([len(user)]) + user + bytes([len(pw)]) + pw)
        await uwriter.drain()
        _v, status = struct.unpack("!BB", await _recv_exact(ureader, 2))
        if status != 0:
            raise ValueError("상위 프록시 인증 실패 — 자격증명을 확인하십시오")
    else:
        uwriter.write(b"\x05\x01\x00")
        await uwriter.drain()
        ver, method = struct.unpack("!BB", await _recv_exact(ureader, 2))
        if method != 0:
            raise ValueError("상위 프록시가 인증을 요구한다")

    if atyp == ATYP_IPV4:
        addr = b"\x01" + socket.inet_aton(host)
    elif atyp == ATYP_IPV6:
        addr = b"\x04" + socket.inet_pton(socket.AF_INET6, host)
    else:
        hb = host.encode("utf-8")
        addr = b"\x03" + bytes([len(hb)]) + hb
    uwriter.write(b"\x05\x01\x00" + addr + struct.pack("!H", port))
    await uwriter.drain()

    ver, rep, _rsv, ratyp = struct.unpack("!BBBB", await _recv_exact(ureader, 4))
    if ratyp == ATYP_IPV4:
        await _recv_exact(ureader, 4)
    elif ratyp == ATYP_HOST:
        ln = (await _recv_exact(ureader, 1))[0]
        await _recv_exact(ureader, ln)
    elif ratyp == ATYP_IPV6:
        await _recv_exact(ureader, 16)
    await _recv_exact(ureader, 2)
    if rep != 0:
        raise ValueError("상위 프록시가 연결을 거절했다(코드 %d)" % rep)
    return ureader, uwriter


async def _pipe(src, dst):
    try:
        while True:
            data = await src.read(65536)
            if not data:
                break
            dst.write(data)
            await dst.drain()
    except Exception:
        pass
    finally:
        try:
            dst.close()
        except Exception:
            pass


async def _handle(cfg, reader, writer):
    uwriter = None
    try:
        atyp, host, port = await _client_handshake(reader, writer)
        ureader, uwriter = await _upstream_connect(cfg, atyp, host, port)
        writer.write(b"\x05\x00\x00\x01" + b"\x00" * 6)   # 성공
        await writer.drain()
        await asyncio.gather(_pipe(reader, uwriter), _pipe(ureader, writer))
    except Exception:
        # 실패는 조용히 끊는다 — 여기서 자세히 적으면 자격증명이 로그에 샐 수 있다.
        try:
            writer.write(b"\x05\x01\x00\x01" + b"\x00" * 6)
            await writer.drain()
        except Exception:
            pass
    finally:
        for w in (writer, uwriter):
            try:
                if w:
                    w.close()
            except Exception:
                pass


async def main():
    cfg = _read_init()
    want = int(cfg.get("listen_port") or 0)      # 0 이면 빈 포트를 OS 가 고른다
    server = await asyncio.start_server(
        lambda r, w: _handle(cfg, r, w), "127.0.0.1", want)
    actual = server.sockets[0].getsockname()[1]
    print(json.dumps({"ready": True, "port": actual}), flush=True)
    async with server:
        await server.serve_forever()


if __name__ == "__main__":
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
    except Exception as e:
        print(json.dumps({"error": str(e)[:200]}), flush=True)
        sys.exit(1)
