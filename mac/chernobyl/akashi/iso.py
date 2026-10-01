#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""격리 사본 — 사용자의 앱·자료를 건드리지 않고 한이시키를 하나 더 띄운다.

    python3 akashi/iso.py start                 # build/ 의 앱을 빈 자료로
    python3 akashi/iso.py start --seed-config   # 진짜 설정(계정 포함)을 복사해서 — 끌 때 지운다
    python3 akashi/iso.py status
    python3 akashi/iso.py log -n 80 --grep 창 끌기
    python3 akashi/iso.py stop

★ 왜 이렇게 띄우나
  · 사용자가 앱을 켜 둔 채 같은 자료 폴더를 쓰는 두 번째 앱을 띄우면 설정·색인을 서로 덮어쓴다.
    그래서 HOME·CFFIXED_USER_HOME·TMPDIR 을 통째로 따로 준다(자료 폴더가 따로 생긴다).
  · QTWEBENGINE_REMOTE_DEBUGGING 으로 화면에 CDP 문을 연다 — 127.0.0.1 에만. 점검 공구는 이 문으로 붙는다.
  · 끌 때는 우리가 띄운 PID 하나만, 실행 파일 경로와 뜬 시각까지 대조해서 끈다. 이름으로 찾지 않는다.
  · 한 번에 하나를 권한다. 화면 엔진 하나가 수백 MB 를 먹는다(포트를 달리하면 여럿도 된다).
  · --seed-config 로 복사한 설정에는 계정 토큰이 들어 있다. 값은 찍지 않고, 끌 때 사본 폴더째 지운다.
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lib import paths, proc  # noqa: E402
from lib.cdp import CdpError, wait_for_page  # noqa: E402

MARK = ".akashi-iso"          # 이 표식이 있는 폴더만 지운다 — 엉뚱한 폴더를 지울 길을 막는다


def base_dir(port: int) -> Path:
    root = Path(os.environ.get("AKASHI_ISO_ROOT") or (Path(tempfile.gettempdir()) / "akashi-iso"))
    return root / str(port)


def state_path(port: int) -> Path:
    return base_dir(port) / "state.json"


def load_state(port: int) -> dict:
    try:
        return json.loads(state_path(port).read_text("utf-8"))
    except (OSError, ValueError):
        return {}


def running(st: dict) -> bool:
    pid = st.get("pid")
    if not pid or not proc.alive(pid):
        return False
    # PID 가 다른 프로세스에 다시 쓰였으면 우리 것이 아니다
    return proc.pid_path(pid) == st.get("exe") and proc.started_at(pid) == st.get("started_at")


def cmd_start(a) -> int:
    port = a.port
    st = load_state(port)
    if running(st):
        print("이미 떠 있습니다 — 포트 %d, PID %d. 끄려면: iso.py stop --port %d" % (port, st["pid"], port))
        return 0
    if proc.port_open(port):
        print("포트 %d 를 다른 것이 쓰고 있습니다(PID %s). --port 로 다른 번호를 주십시오." % (port, proc.listening_pids(port)))
        return 2

    src_app = Path(a.app).resolve() if a.app else paths.find_build_app()
    if not src_app or not paths.is_our_app(src_app):
        print("띄울 앱이 없습니다. mac/chernobyl 에서 ./build.sh 로 빌드하거나 --app 으로 번들을 주십시오.")
        return 2

    bd = base_dir(port)
    home = Path(a.home).resolve() if a.home else bd / "home"
    real = paths.real_home()
    real_data = paths.data_dir()
    if home == real or real in home.parents and "akashi-iso" not in str(home):
        print("격리 홈이 진짜 홈을 가리킵니다 — 멈춥니다: " + paths.short(home))
        return 2
    # ★ 진짜 자료 폴더 안쪽을 홈으로 주면 사본이 그 안에 자료를 쓴다('akashi-iso' 가 들어간 이름이어도).
    if home == real_data or real_data in home.parents:
        print("격리 홈이 진짜 자료 폴더 안입니다 — 멈춥니다: " + paths.short(home))
        return 2
    # ★ 복사한 설정(계정 토큰)은 사본 폴더째 지워서 치운다. 따로 준 홈은 사본 폴더 밖이라 stop 이 지우지 않아
    #   토큰 사본이 그 자리에 남는다 — 그래서 둘을 함께 쓰지 못하게 한다.
    if a.seed_config and a.home and bd not in home.parents:
        print("--seed-config 는 기본 격리 홈에서만 됩니다(따로 준 --home 에 토큰 사본이 남습니다)")
        return 2
    # ★ --env 로 격리 변수를 덮으면 사본이 진짜 홈·자료 폴더를 쓴다. 위의 홈 검사를 돌아가는 길을 막는다.
    bad_env = [kv.partition("=")[0] for kv in (a.env or [])
               if kv.partition("=")[0] in ("HOME", "CFFIXED_USER_HOME", "TMPDIR", "QTWEBENGINE_REMOTE_DEBUGGING")]
    if bad_env:
        print("--env 로 격리 변수(%s)는 바꿀 수 없습니다 — 홈은 --home, 포트는 --port 로" % ", ".join(bad_env))
        return 2
    # ★ 표식은 '우리가 만든 폴더' 라는 증거다. 이미 있던 남의 폴더(AKASHI_ISO_ROOT 를 잘못 주어 포트 번호와
    #   이름이 겹친 폴더 등)에 표식을 새로 써 버리면, stop --wipe 가 그 폴더를 통째로 지울 수 있게 된다.
    if bd.is_symlink() or (bd.exists() and not (bd / MARK).is_file() and (not bd.is_dir() or any(bd.iterdir()))):
        print("사본 폴더 자리에 우리 것이 아닌 폴더가 있습니다 — 건드리지 않습니다: " + paths.short(bd))
        return 2
    bd.mkdir(parents=True, exist_ok=True)
    (bd / MARK).write_text("akashi iso — 이 폴더는 iso.py stop 이 지워도 되는 시험용 사본입니다\n", "utf-8")
    os.chmod(bd, 0o700)
    tmp = bd / "tmp"
    for d in (home, tmp):
        d.mkdir(parents=True, exist_ok=True)

    # ★ 번들도 떼어 낸다 — 빌드본을 APFS 복제(cp -c)로 떠서 그 사본을 띄운다.
    #   앱은 켤 때 스스로 번들을 고친다: 파이썬 자가 점검이 __pycache__ 를 써 봉인을 깨고, 깨진 봉인을
    #   보면 1분쯤 걸려 다시 서명한다. 그런데 격리 사본은 홈이 달라 로그인 키체인이 안 보여 개발자 인증서
    #   대신 ad-hoc 으로 서명했다 — 빌드본이 ad-hoc 이 되어, 그대로 DMG 를 만들면 macOS 가 '다른 앱' 으로
    #   보고 권한을 초기화한다(2026-09-26 실제로 겪음). 복제본을 띄우면 무슨 일을 해도 빌드본은 그대로다.
    #   같은 APFS 볼륨이면 복제는 순간이고 자리도 거의 안 먹는다. 안 되면(다른 볼륨) 그 자리에서 띄우고 알린다.
    app = src_app
    if not a.in_place:
        dst = bd / "app" / src_app.name
        shutil.rmtree(bd / "app", ignore_errors=True)
        dst.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run(["cp", "-cR", str(src_app), str(dst)], capture_output=True, text=True)
        if r.returncode == 0 and paths.is_our_app(dst):
            app = dst
        else:
            shutil.rmtree(bd / "app", ignore_errors=True)
            # ★ 예전엔 여기서 조용히 빌드본 그 자리로 물러섰다. 그러면 앱이 켤 때 빌드본을 ad-hoc 으로 다시 서명해
            #   위에 적은 사고(권한 초기화)가 그대로 난다. 빌드본을 건드려도 되는지는 사람이 --in-place 로 정한다.
            print("빌드본을 복제하지 못했습니다(%s) — 띄우지 않습니다. 빌드본 그 자리에서 띄우려면 --in-place"
                  % ((r.stderr or "").strip().splitlines() or ["?"])[-1][:80])
            return 2
    exe = paths.exe_of(app)

    # ★ 앞서 --seed-config 로 띄웠던 홈을 다시 쓰면, 이번에 안 주었어도 그 홈에는 토큰 사본이 그대로 있다.
    #   표시를 이어받지 않으면 '빈 자료' 인 줄 알고 진짜 계정으로 돌고, stop 도 사본을 지우지 않았다.
    seeded = bool(st.get("seeded")) and Path(st.get("home") or "") == home \
        and (paths.data_dir(home) / paths.CONFIG_NAME).exists()
    if seeded and not a.seed_config:
        print("주의: 이 격리 홈에는 앞서 복사한 설정(계정 포함)이 남아 있습니다 — 그대로 쓰고, 끌 때 지웁니다")
    if a.seed_config:
        src = paths.data_dir() / paths.CONFIG_NAME
        dst = paths.data_dir(home) / paths.CONFIG_NAME
        if not src.exists():
            print("복사할 설정이 없습니다: " + paths.short(src))
            return 2
        dst.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(src, dst)
        os.chmod(dst, 0o600)
        seeded = True
        print("설정 복사함 — %d 바이트 (값은 찍지 않습니다 · 끌 때 사본째 지웁니다)" % dst.stat().st_size)
    elif not a.first_run:
        # ★ 빈 자료로 뜨면 임시 디스크가 정해지지 않아 첫 실행 안내창(#disk-modal)이 화면을 통째로 덮는다.
        #   그러면 점검 공구가 누르는 것마다 안내창에 막히고, 배치 점검은 시작 단추를 모두 '가려짐' 으로 적었다.
        #   실제 사용자는 이미 디스크를 골라 둔 상태이므로, 사본의 임시 폴더를 tempDir 로 넣어 그 상태로 띄운다.
        #   설정은 칸마다 따로 읽으므로(Config::load 의 contains) 이 한 칸이면 된다. 첫 실행 화면을 볼 땐 --first-run.
        cfg = paths.data_dir(home) / paths.CONFIG_NAME
        if not cfg.exists():
            work = tmp / "work"
            work.mkdir(parents=True, exist_ok=True)
            cfg.parent.mkdir(parents=True, exist_ok=True)
            cfg.write_text(json.dumps({"tempDir": str(work)}, ensure_ascii=False), "utf-8")

    env = dict(os.environ)
    env.update({
        "HOME": str(home), "CFFIXED_USER_HOME": str(home), "TMPDIR": str(tmp) + "/",
        "QTWEBENGINE_REMOTE_DEBUGGING": "127.0.0.1:%d" % port,
    })
    if a.hotfix_base:
        env["HANISHIKI_HOTFIX_BASE"] = a.hotfix_base
    for kv in a.env or []:
        k, _, v = kv.partition("=")
        env[k] = v
    log = bd / "app.log"
    with open(log, "ab") as lf:
        lf.write(("\n═══ akashi iso start %s ═══\n" % time.strftime("%Y-%m-%d %H:%M:%S")).encode())
        p = subprocess.Popen([str(exe)], env=env, stdout=lf, stderr=subprocess.STDOUT,
                             stdin=subprocess.DEVNULL, start_new_session=True)
    time.sleep(0.5)
    st = {"pid": p.pid, "port": port, "app": str(app), "source": str(src_app), "exe": str(exe), "home": str(home),
          "log": str(log), "seeded": seeded, "started_at": proc.started_at(p.pid),
          "version": paths.version_of(app), "hotfix_base": a.hotfix_base or ""}
    state_path(port).write_text(json.dumps(st, ensure_ascii=False, indent=1), "utf-8")
    print("띄움 — PID %d · 포트 %d · %s%s · %s" % (p.pid, port, paths.short(src_app),
          " (복제본)" if app != src_app else "", st["version"]))
    try:
        wait_for_page(port, timeout=a.wait)
    except CdpError as e:
        print("화면이 준비되지 않았습니다: %s\n기록: %s" % (e, paths.short(log)))
        return 1
    # ★ 앱의 자가 진단(SelfRepair)이 끝날 때까지 기다린다. 봉인을 다시 서명하는 동안(1분쯤) 화면이 굳어,
    #   그 사이에 점검을 시작하면 '화면이 답하지 않는다' 로 멈췄다. 진단 보고는 끝에 한꺼번에 찍힌다.
    if a.selfrepair_wait > 0:
        end = time.time() + a.selfrepair_wait
        said = False
        while time.time() < end:
            txt = log.read_text("utf-8", "replace")
            if "SelfRepair 자가진단" in txt.split("akashi iso start")[-1]:
                break
            if not said and "[SEAL]" in txt.split("akashi iso start")[-1]:
                print("앱이 스스로 서명을 고치는 중입니다(1분쯤) — 끝나기를 기다립니다")
                said = True
            time.sleep(1)
        else:
            print("주의: 자가 진단 보고가 %d초 안에 나오지 않았습니다 — 점검이 굳은 화면을 만날 수 있습니다" % a.selfrepair_wait)
    if a.settle:
        time.sleep(a.settle)
    print("준비됨 — 점검 공구를 돌리십시오 (예: python3 akashi/check_console.py)")
    return 0


def cmd_stop(a) -> int:
    port = a.port
    st = load_state(port)
    bd = base_dir(port)
    if not st:
        print("포트 %d 로 띄운 기록이 없습니다." % port)
    elif running(st):
        kids = proc.children(st["pid"])
        how = proc.terminate(st["pid"])
        for k in kids:   # 앱이 띄운 도우미(caffeinate 등) — 앱의 자식인 것만
            if proc.alive(k):
                proc.terminate(k, grace=2)
        print("껐습니다 — PID %d (%s)" % (st["pid"], how))
    else:
        print("이미 꺼져 있습니다.")
    for _ in range(40):
        if not proc.port_open(port):
            break
        time.sleep(0.25)
    if proc.port_open(port):
        print("주의: 포트 %d 가 아직 열려 있습니다(PID %s). 우리 것이 아니면 그대로 둡니다." % (port, proc.listening_pids(port)))
    wipe = a.wipe or (st.get("seeded") and not a.keep)
    if wipe and (bd / MARK).exists():
        shutil.rmtree(bd, ignore_errors=True)
        print("사본 폴더를 지웠습니다" + (" (복사한 설정 포함)" if st.get("seeded") else ""))
    return 0


def cmd_status(a) -> int:
    st = load_state(a.port)
    if not st:
        print("포트 %d — 기록 없음" % a.port)
        return 1
    on = running(st)
    print("포트 %d — %s" % (a.port, "떠 있음" if on else "꺼짐"))
    for k in ("pid", "version", "app", "home", "log", "seeded", "hotfix_base"):
        v = st.get(k)
        if k in ("app", "home", "log"):
            v = paths.short(v)
        print("  %-11s %s" % (k, v))
    return 0 if on else 1


def cmd_log(a) -> int:
    st = load_state(a.port)
    lp = Path(st.get("log") or base_dir(a.port) / "app.log")
    if not lp.exists():
        print("기록이 없습니다.")
        return 1
    lines = lp.read_text("utf-8", "replace").splitlines()
    if a.grep:
        lines = [l for l in lines if a.grep in l]
    for l in lines[-a.n:]:
        print(l)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description="한이시키 격리 사본 띄우기·끄기")
    ap.add_argument("--port", type=int, default=int(os.environ.get("AKASHI_PORT", 9334)))
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("start", help="띄운다")
    s.add_argument("--app", help="띄울 .app (기본: mac/chernobyl/build 의 앱)")
    s.add_argument("--home", help="격리 홈 (기본: $TMPDIR/akashi-iso/<포트>/home)")
    s.add_argument("--seed-config", action="store_true", help="진짜 설정을 복사(계정 포함 · 끌 때 지움)")
    s.add_argument("--first-run", action="store_true", help="빈 설정 그대로(첫 실행 안내창이 뜬다) — 기본은 임시 디스크만 정해 둔다")
    s.add_argument("--hotfix-base", help="고침 꾸러미를 받을 주소(HANISHIKI_HOTFIX_BASE)")
    s.add_argument("--env", action="append", metavar="K=V", help="더 줄 환경 변수")
    s.add_argument("--wait", type=float, default=45, help="화면을 기다릴 초")
    s.add_argument("--settle", type=float, default=3, help="준비된 뒤 더 기다릴 초(글꼴·첫 배치)")
    s.add_argument("--selfrepair-wait", type=float, default=150, help="앱의 자가 진단(봉인 복구 포함)을 기다릴 초 — 0 이면 안 기다림")
    s.add_argument("--in-place", action="store_true", help="복제하지 않고 빌드본 그 자리에서 띄운다(앱이 빌드본을 고칠 수 있음)")
    t = sub.add_parser("stop", help="끈다")
    t.add_argument("--wipe", action="store_true", help="사본 폴더를 지운다")
    t.add_argument("--keep", action="store_true", help="설정을 복사했어도 지우지 않는다")
    sub.add_parser("status", help="상태")
    g = sub.add_parser("log", help="앱 기록")
    g.add_argument("-n", type=int, default=60)
    g.add_argument("--grep")
    for p in (s, t, g):
        p.add_argument("--port", type=int, default=argparse.SUPPRESS)
    a = ap.parse_args()
    return {"start": cmd_start, "stop": cmd_stop, "status": cmd_status, "log": cmd_log}[a.cmd](a)


if __name__ == "__main__":
    sys.exit(main())
