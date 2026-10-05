# -*- coding: utf-8 -*-
"""기능 시험이 함께 쓰는 것 — 판정 줄 · 격리 사본 띄우기/끄기 · 작업 폴더 · 사용자 Chrome 지키기.

★ 왜 작업 폴더가 저장소 밖(임시 폴더)인가
  시험 사이트 · 가짜 보관 폴더 · 캡처 결과는 매번 새로 만든다. 저장소에 두면 커밋에 섞이고, 그림 파일을
  공개 저장소에 올리게 된다. 기본은 $TMPDIR/akashi-tests, AKASHI_TEST_DIR 로 바꾼다.
★ 왜 자가진단을 기다리나
  사본이 켜진 뒤 앱의 자가진단(SelfRepair — 네트워크 확인 · 파이썬 꾸러미 맞추기)이 도는 동안 화면이 굳는다.
  디스크가 거의 찼을 때 3분 가까이 걸린 적이 있다(2026-10-05). 그 사이에 붙으면 CDP 가 시간 초과로 끝난다.
  그래서 기록(app.log)에 'SelfRepair' 줄이 나올 때까지 기다린 뒤 붙는다.
★ 왜 사용자 Chrome(9223)을 앞뒤로 재나
  사본이 켜질 때 하는 캡처 Chrome 정리가 사용자 앱의 Chrome 을 끄던 때가 있었다(0aa09aa 에서 고침).
  시험마다 끝에 '사용자 앱의 Chrome 은 그대로' 를 판정한다. 시험할 앱이 그 고침 전의 판(실행 파일에
  chrome_capture_profile_pen 이 없음 — --app 의 옛 판이든 묵은 build/ 든)인데 사용자 앱의 캡처 Chrome
  (9223 · 트랙 · 출구 · PEN 어느 것이든)이 떠 있으면 아예 시작하지 않는다 — 그 판의 사본은 켜지는 순간 이름으로 끈다.
★ 왜 화면 오류를 따로 켜나
  CDP 는 Runtime · Log 를 켜야 예외 · console.error 를 보내 준다. 켜지 않으면 '화면 JS 오류 없음' 은 늘 통과한다
  (옮기기 전의 시험들이 그랬다). watch_errors() 로 켜고, 그 전의 것은 비운다.
"""
from __future__ import annotations

import contextlib
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TESTS = Path(__file__).resolve().parent
AKASHI = TESTS.parent
sys.path.insert(0, str(AKASHI))
import iso  # noqa: E402
from lib import paths  # noqa: E402
from lib.cdp import CdpError, Page  # noqa: E402

PORT = int(os.environ.get("AKASHI_PORT") or 9334)
USER_CAPTURE_PORT = 9223            # 사용자 앱의 캡처 Chrome 기본 포트(Common::capturePortBase)
ISO = iso.base_dir(PORT)


class Checks:
    """판정 줄을 찍고 모은다. 끝에 summary() 가 '통과'/'실패' 한 줄과 종료 코드를 낸다."""

    def __init__(self):
        self.ok = True
        self.n = 0

    def __call__(self, name, cond, detail=""):
        self.ok &= bool(cond)
        self.n += 1
        print(("  ✔ " if cond else "  ✘ ") + name + (("  — " + str(detail)) if detail else ""), flush=True)
        return bool(cond)

    def summary(self) -> int:
        print("통과" if self.ok else "실패", flush=True)
        return 0 if self.ok else 1


def work_dir(sub: str = "") -> Path:
    root = Path(os.environ.get("AKASHI_TEST_DIR") or (Path(tempfile.gettempdir()) / "akashi-tests"))
    d = root / sub if sub else root
    d.mkdir(parents=True, exist_ok=True)
    return d


def test_app():
    """시험할 앱 — AKASHI_TEST_APP(run_all.py --app) 이 있으면 그것, 없으면 build/ 의 앱."""
    env = os.environ.get("AKASHI_TEST_APP")
    if env:
        return Path(env) if paths.is_our_app(env) else None
    return paths.find_build_app()


def user_idle_seconds() -> int:
    """사람이 키보드 · 마우스를 마지막으로 쓴 뒤 지난 초(ioreg HIDIdleTime — 읽기만). 모르면 0."""
    out = subprocess.run(["/usr/sbin/ioreg", "-c", "IOHIDSystem"], capture_output=True, text=True).stdout
    for line in out.splitlines():
        if "HIDIdleTime" in line:
            try:
                return int(line.split("=")[-1].strip()) // 1_000_000_000
            except ValueError:
                return 0
    return 0


def free_port() -> int:
    s = socket.socket()
    s.bind(("127.0.0.1", 0))
    p = s.getsockname()[1]
    s.close()
    return p


def listen_pids(port: int) -> list:
    out = subprocess.run(["lsof", "-nP", "-ti", "TCP:%d" % port, "-sTCP:LISTEN"], capture_output=True, text=True).stdout
    return sorted(int(x) for x in out.split())


def scoped_cleanup(app) -> bool:
    """캡처 Chrome 정리를 '이 앱 데이터 폴더의 것' 으로 좁힌 판(0aa09aa~)인가 — 그 커밋에서 생긴 PEN 프로필 이름이 실행 파일에 있다."""
    try:
        return b"chrome_capture_profile_pen" in paths.exe_of(app).read_bytes()
    except OSError:
        return False


def foreign_capture_chromes() -> list:
    """시험 폴더(사본 · 작업 폴더) 밖의, 명령줄에 chrome_capture_profile 이 든 프로세스 — 읽기만 하고 끄지 않는다."""
    ours = {str(ISO), str(ISO.resolve()), str(work_dir()), str(work_dir().resolve())}
    out = subprocess.run(["/bin/ps", "-axww", "-o", "pid=,command="], capture_output=True, text=True).stdout
    found = []
    for line in out.splitlines():
        pid, _, cmd = line.strip().partition(" ")
        at = cmd.find("--user-data-dir=")            # Chrome 의 프로필 인자에 든 것만(그 글자를 품은 셸 명령 등은 빼고)
        if (pid.isdigit() and int(pid) != os.getpid() and at >= 0 and "chrome_capture_profile" in cmd[at:at + 1024]
                and not any(o in cmd for o in ours)):
            found.append(int(pid))
    return found


def watch_errors(page) -> None:
    """화면의 JS 예외 · console.error 를 받기 시작한다(켜기 전의 것은 버린다)."""
    page.cmd("Runtime.enable")
    page.cmd("Log.enable")
    page.pump(0.4)
    page.events.clear()


@contextlib.contextmanager
def page(retries: int = 3):
    """사본의 본 화면에 붙어 오류 받기를 켜고(watch_errors) 한 번 대 본 뒤 넘긴다. 끊기면 잠깐 뒤 다시 붙는다.

    ★ 왜 다시 붙나 — 사본이 막 뜬 뒤(자가진단 · 꾸러미 맞추기 직후) 붙는 순간 CDP 가 끊기거나(Connection reset)
      시간 초과로 끝나는 일이 가끔 있다. 시험은 아직 아무것도 하지 않은 때라 다시 붙어도 판정이 바뀌지 않는다."""
    last = None
    for i in range(retries):
        pg = None
        try:
            pg = Page.attach(PORT)
            watch_errors(pg)
            pg.eval("document.readyState")
            break
        except (OSError, CdpError) as e:              # socket.timeout · ConnectionReset 은 OSError
            last = e
            print("   (화면에 붙다 끊김 %d/%d — %s: %s · 3초 뒤 다시)" % (i + 1, retries, type(e).__name__, e), flush=True)
            if pg is not None:
                try:
                    pg.close()
                except OSError:
                    pass
            time.sleep(3)
    else:
        raise last
    try:
        yield pg
    finally:
        try:
            pg.close()
        except OSError:
            pass


def page_errors(page) -> list:
    page.pump(0.5)                   # 마지막 명령 뒤에 온 것까지
    return page.collect_errors()


def _iso(*args) -> subprocess.CompletedProcess:
    return subprocess.run([sys.executable, str(AKASHI / "iso.py"), "--port", str(PORT)] + list(args),
                          capture_output=True, text=True)


def iso_state() -> dict:
    return iso.load_state(PORT)


def iso_alive() -> bool:
    st = iso_state()
    try:
        os.kill(int(st["pid"]), 0)
        return True
    except (KeyError, ValueError, TypeError, OSError):
        return False


def wait_selfrepair(limit: float = 300.0) -> float:
    log = ISO / "app.log"
    t0 = time.time()
    while time.time() - t0 < limit:
        # iso.py 는 기록에 이어 쓴다(시작마다 '═══ akashi iso start' 표식). 지난 시작의 SelfRepair 줄을 세지 않게 마지막 표식 뒤만
        if log.exists() and "SelfRepair" in log.read_text(encoding="utf-8", errors="replace").split("akashi iso start")[-1]:
            break
        time.sleep(2)
    return time.time() - t0


def iso_start(*extra, wipe_first: bool = True) -> bool:
    """사본을 띄우고 자가진단까지 기다린다. 실패하면 False(까닭은 찍는다)."""
    app = test_app()
    if app is None:
        print("시험할 앱이 없습니다 — mac/chernobyl 에서 ./build.sh 부터(또는 --app 이 우리 앱이 아님)")
        return False
    if not scoped_cleanup(app) and (listen_pids(USER_CAPTURE_PORT) or foreign_capture_chromes()):
        print("시험할 앱(%s)이 캡처 정리 고침(0aa09aa) 전의 판인데 사용자 앱의 캡처 Chrome 이 떠 있습니다 — "
              "그 판의 사본은 켜지는 순간 이름으로 끌 수 있어 시작하지 않습니다" % paths.short(app))
        return False
    if wipe_first:
        _iso("stop", "--wipe")
    # 새로 구운 번들은 첫 실행이 느리다(복제본마다 macOS 가 새 앱으로 살핀다) — iso.py 의 45초 대신 넉넉히
    args = ["start", "--wait", "150"] + (["--app", str(app)] if os.environ.get("AKASHI_TEST_APP") else []) + list(extra)
    r = _iso(*args)
    last = (r.stdout.strip().splitlines() or ["?"])[-1]
    print("[사본] " + last, flush=True)
    if r.returncode != 0 or not iso_alive():
        print((r.stderr or r.stdout).strip()[-400:])
        return False
    waited = wait_selfrepair()
    print("   자가진단 기다림 %.0fs" % waited, flush=True)
    return True


def iso_stop(wipe: bool = True) -> None:
    _iso(*(["stop", "--wipe"] if wipe else ["stop"]))


def keep_log(name: str) -> None:
    """끄기 전에 사본의 app.log 를 작업 폴더로(시험이 실패했을 때 볼 수 있게)."""
    try:
        (work_dir() / name).write_bytes((ISO / "app.log").read_bytes())
    except OSError:
        pass


def platform_stats(page, plat: str) -> dict:
    raw = page.eval("JSON.stringify((typeof platformStats!=='undefined' && platformStats[%s]) || {})" % json.dumps(plat))
    return json.loads(raw or "{}")


def wait_status(page, plat: str, want, secs: float, step: float = 0.5):
    t0 = time.time()
    st = {}
    while time.time() - t0 < secs:
        st = platform_stats(page, plat)
        if str(st.get("status", "")) in want:
            return st, time.time() - t0
        time.sleep(step)
    return st, None
