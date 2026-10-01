#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""빌드를 설치본에 올리기 — build/ 의 앱을 사용자가 쓰는 자리의 앱 위에 덮는다. 묻고 한다.

    python3 akashi/install.py --dry-run     # 무엇을 할지만
    python3 akashi/install.py               # 한 단계씩 묻고
    python3 akashi/install.py --to /경로/앱.app --open

★ 순서와 까닭
  1. 빌드본 서명 확인(codesign --verify --deep --strict) — 깨진 번들을 올리면 macOS 가 소리 없이 죽인다.
     빌드가 아직 도는 중(build/.build.lock)이면 멈춘다 — 반쯤 된 번들을 올리게 된다.
  2. 설치본이 켜져 있으면 '닫아도 됩니까?' 묻고, osascript 로 곱게 닫는다(경로로 지목 — 이름이 아니다).
     켠 채로 덮으면 실행 중인 파일이 바뀌어 SIGBUS(KERN_MEMORY_ERROR)로 죽는다 — 충돌 보고서
     (crash_report.py)에 실제로 세 번 남아 있다. 30초 안에 안 닫히면 멈추고 알린다. 죽이지 않는다.
  3. rsync -a --delete 로 덮는다. 대상이 .app 이고 우리 번들(번들 식별자)일 때만 — 엉뚱한 폴더를
     '--delete' 로 비우는 일을 막는다.
  4. 설치본 서명 확인, 원하면 연다.
"""
from __future__ import annotations

import argparse
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lib import paths, proc  # noqa: E402


def ask(q: str, yes: bool) -> bool:
    if yes:
        print(q + " → 예(--yes)")
        return True
    try:
        return input(q + " (y/N) ").strip().lower() in ("y", "yes", "ㅇ", "예", "네")
    except EOFError:
        return False


def codesign_ok(app: Path) -> tuple:
    r = subprocess.run(["codesign", "--verify", "--deep", "--strict", str(app)], capture_output=True, text=True)
    return r.returncode == 0, (r.stderr or r.stdout).strip().splitlines()[-1:] if r.returncode else []


def running_pids(app: Path) -> list:
    return proc.pids_for_exe(paths.exe_of(app))


def quit_gracefully(app: Path, wait: float = 30.0) -> bool:
    # 경로로 지목한다 — 같은 번들 식별자를 가진 빌드본·격리 사본이 떠 있어도 이 설치본만 닫힌다.
    script = 'tell application (POSIX file "%s" as text) to quit' % str(app).replace('"', '\\"')
    subprocess.run(["osascript", "-e", script], capture_output=True, text=True, timeout=40)
    end = time.time() + wait
    while time.time() < end:
        if not running_pids(app):
            return True
        time.sleep(0.5)
    return False


def main() -> int:
    ap = argparse.ArgumentParser(description="빌드를 설치본에 올리기 — 단계마다 묻는다")
    ap.add_argument("--to", help="설치본 .app (기본: 찾은 설치본이 하나일 때 그것)")
    ap.add_argument("--build", help="올릴 빌드 .app (기본: mac/chernobyl/build 의 앱)")
    ap.add_argument("--dry-run", action="store_true", help="무엇을 할지만 보인다")
    ap.add_argument("--yes", action="store_true", help="묻지 않고 한다(켜 둔 앱 닫기 포함 — 조심)")
    ap.add_argument("--open", action="store_true", help="다 되면 연다")
    a = ap.parse_args()

    src = Path(a.build).resolve() if a.build else paths.find_build_app()
    if not src or not paths.is_our_app(src):
        print("올릴 빌드가 없습니다 — mac/chernobyl 에서 ./build.sh 부터")
        return 2
    if (paths.CHERNOBYL / "build" / ".build.lock").exists():
        print("빌드가 아직 도는 중입니다(build/.build.lock) — 끝난 뒤에 다시")
        return 2
    if a.to:
        dst = Path(a.to).resolve()
    else:
        found = paths.find_installed_apps()
        # 예비본(예: 이름_0907.app)을 곁에 두는 일이 많다 — 여럿이면 빌드본과 이름이 같은 것 하나를 고른다
        live = paths.running_apps()
        same = [f for f in found if f.name == src.name]
        if len(found) > 1 and len(live) == 1:
            # 켜져 있는 것이 사용자가 쓰는 설치본이다 — 이름이 같은 예비본보다 먼저
            print("설치본이 %d개 — 지금 켜져 있는 %s 을(를) 고릅니다(다른 것은 --to)" % (len(found), paths.short(live[0])))
            found = live
        elif len(found) > 1 and len(same) == 1:
            print("설치본이 %d개 — 빌드본과 이름이 같은 %s 을(를) 고릅니다(다른 것은 --to)" % (len(found), same[0].name))
            found = same
        if len(found) != 1:
            print("설치본을 %s — --to 로 정해 주십시오%s" % ("찾지 못했습니다" if not found else "여럿 찾았습니다",
                  "".join("\n  " + paths.short(f) for f in found)))
            return 2
        dst = found[0].resolve()
    if dst.suffix != ".app" or not paths.is_our_app(dst):
        print("대상이 한이시키 번들이 아닙니다 — 덮지 않습니다: " + paths.short(dst))
        return 2
    if dst == src.resolve():
        print("빌드본과 설치본이 같은 자리입니다")
        return 2

    ok, why = codesign_ok(src)
    pids = running_pids(dst)
    print("빌드본  %s  %s  서명 %s" % (paths.short(src), paths.version_of(src), "맞음" if ok else "틀림 " + " ".join(why)))
    print("설치본  %s  %s  %s" % (paths.short(dst), paths.version_of(dst), "켜져 있음(PID %s)" % pids if pids else "꺼져 있음"))
    if not ok:
        print("빌드본 서명이 틀려 올리지 않습니다 — ./build.sh 를 다시(또는 verify_or_resign.sh)")
        return 1
    plan = (["켜 둔 설치본을 묻고 곱게 닫기(osascript)"] if pids else []) + \
           ["rsync -a --delete 빌드본/ 설치본/", "설치본 서명 확인"] + (["열기"] if a.open else [])
    print("할 일: " + " → ".join(plan))
    if a.dry_run:
        print("--dry-run — 아무것도 하지 않았습니다")
        return 0

    if pids:
        if not ask("켜 둔 한이시키(설치본)를 닫아도 됩니까? 하던 수집이 있으면 멈춥니다", a.yes):
            print("닫지 않았습니다 — 설치를 멈춥니다")
            return 1
        if not quit_gracefully(dst):
            print("30초 안에 닫히지 않았습니다 — 강제로 끄지 않습니다. 앱에서 직접 닫은 뒤 다시")
            return 1
        print("닫았습니다")
    elif not ask("설치본을 빌드본으로 덮을까요?", a.yes):
        return 1

    r = subprocess.run(["rsync", "-a", "--delete", str(src) + "/", str(dst) + "/"], capture_output=True, text=True)
    if r.returncode != 0:
        print("rsync 실패(%d): %s" % (r.returncode, (r.stderr or "").strip()[-300:]))
        return 1
    ok, why = codesign_ok(dst)
    print("덮음 — 설치본 %s · 서명 %s" % (paths.version_of(dst), "맞음" if ok else "틀림 " + " ".join(why)))
    if not ok:
        return 1
    if a.open:
        subprocess.run(["open", str(dst)])
        print("열었습니다")
    print("통과 — 설치 끝")
    return 0


if __name__ == "__main__":
    sys.exit(main())
