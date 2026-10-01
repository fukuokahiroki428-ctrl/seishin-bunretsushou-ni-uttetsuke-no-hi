# -*- coding: utf-8 -*-
"""프로세스 다루기 — 이름으로 죽이지 않는다.

★ kill_app.sh 머리말과 같은 교훈이다. 명령줄을 훑어 죽이면 앱 경로를 인자로 받은 codesign 까지
  잡혀 서명 도중에 죽고 번들이 망가졌다. 그리고 ps 는 한글 경로를 8진 이스케이프로 찍어 문자열
  비교가 늘 실패했다. 여기서는 커널에 '이 PID 의 실행 파일 경로' 를 직접 묻는다(libproc).
  그리고 격리 사본을 끌 때는 우리가 띄운 PID 하나만, 띄운 시각까지 대조해서 끈다.
"""
from __future__ import annotations

import ctypes
import ctypes.util
import os
import signal
import socket
import subprocess
import sys
import time

_libproc = None
if sys.platform == "darwin":
    try:
        _libproc = ctypes.CDLL(ctypes.util.find_library("proc") or "/usr/lib/libproc.dylib")
    except OSError:
        _libproc = None


def pid_path(pid: int):
    """PID 의 실행 파일 절대 경로. 모르면 None."""
    if _libproc is not None:
        buf = ctypes.create_string_buffer(4096)
        n = _libproc.proc_pidpath(int(pid), buf, ctypes.c_uint32(4096))
        return os.path.realpath(buf.value[:n].decode("utf-8", "replace")) if n > 0 else None
    link = "/proc/%d/exe" % pid
    try:
        return os.path.realpath(os.readlink(link))
    except OSError:
        return None


def alive(pid: int) -> bool:
    try:
        os.kill(int(pid), 0)
        return True
    except ProcessLookupError:
        return False
    except PermissionError:
        return True


def started_at(pid: int) -> str:
    """PID 가 뜬 시각(ps lstart). PID 가 다른 프로세스에 다시 쓰였는지 가리는 데 쓴다."""
    try:
        return subprocess.run(["ps", "-o", "lstart=", "-p", str(int(pid))], capture_output=True,
                              text=True, timeout=5).stdout.strip()
    except (OSError, subprocess.SubprocessError):
        return ""


def children(pid: int) -> list:
    try:
        out = subprocess.run(["pgrep", "-P", str(int(pid))], capture_output=True, text=True, timeout=5).stdout
        return [int(x) for x in out.split()]
    except (OSError, subprocess.SubprocessError, ValueError):
        return []


def pids_for_exe(exe) -> list:
    """실행 파일 경로가 정확히 exe 인 프로세스들(이름으로 1차 추림 → 커널 경로로 확정)."""
    exe = os.path.realpath(str(exe))
    name = os.path.basename(exe)
    # ★ 커널의 프로세스 이름(p_comm)은 16바이트에서 잘린다(MAXCOMLEN). 실행 파일 이름이 그보다 길면
    #   pgrep -x 가 아무것도 못 찾아 '꺼져 있음' 이 되고, install.py 가 켜 둔 앱 위에 덮어 SIGBUS 를 낸다.
    #   그럴 땐 이름으로 추리지 않고 모든 PID 의 실행 파일 경로를 커널에 물어 확정한다.
    cmd = ["pgrep", "-x", name] if len(name.encode("utf-8")) < 16 else ["ps", "-axo", "pid="]
    try:
        out = subprocess.run(cmd, capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return []
    return [int(p) for p in out.split() if p.isdigit() and pid_path(int(p)) == exe]


def pids_named(name: str) -> list:
    """프로세스 이름이 정확히 name 인 PID 들(경로는 묻지 않는다 — 찾은 뒤 pid_path 로 확정할 것)."""
    try:
        out = subprocess.run(["pgrep", "-x", name[:15]], capture_output=True, text=True, timeout=5).stdout
        return [int(x) for x in out.split() if x.isdigit()]
    except (OSError, subprocess.SubprocessError):
        return []


def port_open(port: int, host: str = "127.0.0.1") -> bool:
    s = socket.socket()
    s.settimeout(0.5)
    try:
        s.connect((host, int(port)))
        return True
    except OSError:
        return False
    finally:
        s.close()


def listening_pids(port: int) -> list:
    try:
        out = subprocess.run(["lsof", "-nP", "-iTCP:%d" % int(port), "-sTCP:LISTEN", "-t"],
                             capture_output=True, text=True, timeout=8).stdout
        return sorted({int(x) for x in out.split()})
    except (OSError, subprocess.SubprocessError, ValueError):
        return []


def terminate(pid: int, grace: float = 8.0, verify=None) -> str:
    """SIGTERM → 기다림 → 그래도 있으면 SIGKILL. 무엇을 했는지 돌려준다.

    verify: 인자 없는 함수 — SIGKILL 직전에 '아직 그 프로세스인가(경로·뜬 시각)' 를 다시 묻는다.
    ★ 기다리는 사이 끝난 PID 가 남에게 다시 쓰였으면, 살아 있다는 것만 보고 남의 프로세스를 죽이게 된다."""
    if not alive(pid):
        return "이미 없음"
    try:
        os.kill(pid, signal.SIGTERM)
    except ProcessLookupError:
        return "이미 없음"
    end = time.time() + grace
    while time.time() < end:
        if not alive(pid):
            return "SIGTERM 으로 끝남"
        time.sleep(0.2)
    if verify is not None and not verify():
        return "SIGTERM 으로 끝남(PID 가 다른 프로세스로 바뀌어 더 건드리지 않음)"
    try:
        os.kill(pid, signal.SIGKILL)
    except ProcessLookupError:
        return "SIGTERM 으로 끝남"
    return "SIGKILL"
