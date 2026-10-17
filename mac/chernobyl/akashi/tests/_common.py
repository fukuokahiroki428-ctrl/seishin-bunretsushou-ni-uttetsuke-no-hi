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
★ 왜 덮개를 보나 — 시험 도중 CDP 가 끊기던 것(2026-10-17)
  덮개를 닫은 맥(전원 연결 · 외부 화면 없음)은 앱이 수집을 시작할 때마다(holdAwake 의 caffeinate -u) FullWake 로 깨었다가
  10초 뒤 'Clamshell Sleep' 으로 DarkWake 에 든다. 그때 0~3초 안에 앱의 CDP 서버가 우리 연결을 닫았다(앱은 살아 있음 —
  crawl 의 받는 도중 멈춤 판, youtube_layout 의 니코동 판에서 ConnectionReset · WsClosed. r42 도 똑같았다).
  그래서 page() 는 끊기면 같은 화면에 다시 붙는 Page(reconnect=True)를 넘기고, 기다리며 읽는 식은 retry=True 로 부른다
  (lib/cdp.py 의 ★ 다시 붙기). 시작할 때 덮개가 닫혀 있거나 DarkWake 면 env_warning() 이 한 줄로 알린다 — 화면이 그려지지
  않는 동안의 판정(지연 그림 등)은 믿기 어렵다. 다시 붙은 일은 끝에 '⚠ CDP 연결이 n번 끊겨…' 로 남긴다(run_all 이 모은다).
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
from lib.cdp import DROPPED, CdpError, Dropped, Page, PageGone  # noqa: E402

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


def power_state() -> dict:
    """덮개가 닫혔나 · 맥이 화면을 그릴 수 있나(FullWake) — ioreg IOPMrootDomain 을 읽기만. 모르면 None."""
    out = subprocess.run(["/usr/sbin/ioreg", "-r", "-n", "IOPMrootDomain", "-d", "1"], capture_output=True, text=True).stdout
    lid = caps = None
    for line in out.splitlines():
        key, _, val = line.strip().partition(" = ")
        if key == '"AppleClamshellState"':
            lid = val.strip() == "Yes"
        elif key == '"System Capabilities"':
            try:
                caps = int(val.strip())
            except ValueError:
                pass
    # System Capabilities: 1 CPU · 2 화면(그래픽) · 4 소리 · 8 네트워크 — FullWake 는 15, DarkWake 는 그래픽이 빠진 9
    ps = subprocess.run(["/usr/bin/pmset", "-g", "ps"], capture_output=True, text=True).stdout
    battery = True if "'Battery Power'" in ps else (False if "'AC Power'" in ps else None)
    return {"lid_closed": lid, "dark_wake": None if caps is None else not (caps & 2), "caps": caps, "battery": battery}


def power_note(st: dict | None = None) -> str:
    st = st or power_state()
    bits = []
    if st.get("lid_closed"):
        bits.append("덮개 닫힘")
    if st.get("dark_wake"):
        bits.append("DarkWake(화면 꺼짐)")
    if st.get("lid_closed") and st.get("battery"):
        bits.append("배터리")
    idle = user_idle_seconds()
    if idle > 300:
        bits.append("사람 입력 없이 %d분" % (idle // 60))
    return " · ".join(bits)


def env_warning() -> str:
    """시험 결과를 흐릴 수 있는 맥의 상태를 한 줄로(없으면 빈 글)."""
    st = power_state()
    if st.get("lid_closed") and st.get("battery"):
        # 배터리면 앱의 PreventSystemSleep · caffeinate -s 가 덮개 닫힘을 막지 못해 맥이 정말 잠든다(2026-10-17 13:49~16:05 실측 —
        # 'Entering Sleep state due to Clamshell Sleep … Using Batt', 이따금 몇 초씩만 깸). 시험은 멈췄다 이어지고, 벽시계로 잰
        # 기다림(유튜브 끝 신호 180초 등)이 잠든 시간까지 세어 실패한다 — CDP 와는 무관하다.
        return ("⚠ 환경: %s — 배터리로 덮개를 닫으면 맥이 정말 잠듭니다. 시험이 멈췄다 이어지고 시간 판정이 잠든 시간까지 세어 "
                "실패합니다 — 전원을 꽂거나 덮개를 열고 다시 돌리십시오" % power_note(st))
    if st.get("lid_closed") or st.get("dark_wake"):
        return ("⚠ 환경: %s — 앱이 수집을 시작할 때마다 맥이 FullWake 로 깨었다가 10초 뒤 DarkWake 로 들고, 그때 CDP 연결이 "
                "끊길 수 있습니다(다시 붙어 이어 갑니다). 화면이 그려지지 않으니 그림 · 지연 로딩 판정은 덮개를 열고 다시 보십시오"
                % power_note(st))
    return ""


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
    page.since_ms = time.time() * 1000      # 다시 붙어 다시 켤 때 재생되는 이 앞의 것도 버리게(lib/cdp.py)
    page.cmd("Runtime.enable")
    page.cmd("Log.enable")
    page.pump(0.4)
    page.events.clear()


@contextlib.contextmanager
def page(retries: int = 3):
    """사본의 본 화면에 붙어 오류 받기를 켜고(watch_errors) 한 번 대 본 뒤 넘긴다. 끊기면 잠깐 뒤 다시 붙는다.

    ★ 왜 다시 붙나 — 사본이 막 뜬 뒤(자가진단 · 꾸러미 맞추기 직후) 붙는 순간 CDP 가 끊기거나(Connection reset)
      시간 초과로 끝나는 일이 가끔 있다. 시험은 아직 아무것도 하지 않은 때라 다시 붙어도 판정이 바뀌지 않는다.
    ★ 넘긴 뒤에 끊기면 — 넘기는 Page 는 reconnect=True 다. 같은 화면(target · 표식)에 다시 붙고, retry=True 로 부른
      읽기 식만 되풀이한다(덮개 닫힌 맥의 DarkWake — 맨 위 ★ 왜 덮개를 보나). 화면을 바꾸는 식이 끊기면 Dropped 로 멈춘다."""
    last = None
    for i in range(retries):
        pg = None
        try:
            pg = Page.attach(PORT, reconnect=True)
            watch_errors(pg)
            pg.eval("document.readyState")
            pg.mark()                                 # 다시 붙은 뒤 '같은 화면인가' 를 볼 표식
            pg.on_drop = _note_drop
            break
        except (OSError, CdpError) + DROPPED as e:    # socket.timeout · ConnectionReset 은 OSError, 닫힘은 WsClosed
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
    except (CdpError,) + DROPPED as e:                     # 시험을 멈춘 CDP 일 — 까닭이 될 만한 맥의 상태를 곁들인다
        note = power_note()
        if note or isinstance(e, DROPPED + (Dropped, PageGone)):
            print("   (CDP: %s: %s%s)" % (type(e).__name__, str(e)[:160], (" — 지금 " + note) if note else ""), flush=True)
        raise
    finally:
        if pg.drops:
            print("  ⚠ CDP 연결이 %d번 끊겨 같은 화면에 다시 붙음 — %s" % (len(pg.drops), "; ".join(
                "%s %s(%.1fs%s)" % (time.strftime("%H:%M:%S", time.localtime(d["at"])), d["why"][:48], d["took"],
                                     (" · " + d["power"]) if d.get("power") else "") for d in pg.drops[:4])), flush=True)
        try:
            pg.close()
        except OSError:
            pass


def _note_drop(rec: dict) -> None:
    rec["power"] = power_note()
    print("   (CDP 연결이 끊겨 다시 붙음 %s — %s%s)" % (time.strftime("%H:%M:%S", time.localtime(rec["at"])), rec["why"][:80],
                                             (" · " + rec["power"]) if rec["power"] else ""), flush=True)


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
    warn = env_warning()
    if warn:
        print(warn, flush=True)
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
    raw = page.eval("JSON.stringify((typeof platformStats!=='undefined' && platformStats[%s]) || {})" % json.dumps(plat),
                    retry=True)
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
