#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""정기 점검 — 한 줄로 격리 사본을 띄워 화면 점검을 차례로 돌리고, 끄고, 표로 알린다.

    python3 akashi/inspect_all.py                 # 화면 오류 · 배치 · 자동 배치 · 폼 저장
    python3 akashi/inspect_all.py --hotfix        # + 고침 꾸러미 실험실(진짜 서명 열쇠로 서명 · 앱이 받는지)
    python3 akashi/inspect_all.py --doctor        # + 정비 점검(서명·도구·모듈)
    python3 akashi/inspect_all.py -v              # 공구마다 전체 출력

★ 언제 돌리나
  빌드를 새로 구운 뒤, 설치(install.py)·판 내기(release.py) 전에. 사람이 눌러 보기 전에 기계가 먼저
  한 바퀴 돈다. 화면 JS 한 줄이 틀려도 탭 하나가 통째로 죽는 앱이라, 전 탭을 도는 check_console 이 가장 값지다.

★ 순서와 까닭
  1. 격리 사본이 없으면 띄운다(iso.py start — 사용자의 앱·자료와 따로). 이미 떠 있으면 그것을 쓰고 끄지 않는다.
  2. check_console → check_layout(대상 17명 심어서) → check_adapt → check_forms. 각 공구는 바꾼 것을 되돌린다.
  3. --hotfix 는 마지막에 — 실험실이 고침 주소를 바꿔 사본을 다시 띄우기 때문이다.
  4. 여기서 띄운 사본만 끈다. 한 번에 사본 하나 — 화면 엔진 하나가 수백 MB 를 먹는다.
"""
from __future__ import annotations

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
from lib import proc  # noqa: E402

VERDICT = ("통과", "문제", "돌릴 수 없음", "실패", "도중에 멈춤")


def tool(name: str, args, port: int, verbose: bool, timeout: int = 900):
    cmd = [sys.executable, str(HERE / name)] + list(args)
    env = dict(os.environ, AKASHI_PORT=str(port))
    t0 = time.time()
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout, env=env)
        out, code = (r.stdout or "") + (r.stderr or ""), r.returncode
    except subprocess.TimeoutExpired:
        out, code = "시간 초과(%ds)" % timeout, 1
    lines = [l for l in out.splitlines() if l.strip()]
    last = next((l.strip() for l in reversed(lines) if l.strip().startswith(VERDICT)), lines[-1].strip() if lines else "")
    if verbose:
        print("\n".join("    │ " + l for l in lines))
    return code, last, time.time() - t0


def main() -> int:
    ap = argparse.ArgumentParser(description="정기 점검 — 격리 사본으로 화면 점검을 한 번에")
    ap.add_argument("--port", type=int, default=int(os.environ.get("AKASHI_PORT", 9334)))
    ap.add_argument("--hotfix", action="store_true", help="고침 꾸러미 실험실도(진짜 서명 열쇠 · 사본을 다시 띄움)")
    ap.add_argument("--doctor", action="store_true", help="정비 점검(doctor.py)도")
    ap.add_argument("--keep", action="store_true", help="여기서 띄운 사본을 끝나도 켜 둔다")
    ap.add_argument("-v", "--verbose", action="store_true", help="공구마다 전체 출력")
    a = ap.parse_args()

    started = False
    if not proc.port_open(a.port):
        print("격리 사본을 띄웁니다(포트 %d)…" % a.port, flush=True)
        r = subprocess.run([sys.executable, str(HERE / "iso.py"), "--port", str(a.port), "start"], capture_output=True, text=True)
        if r.returncode != 0:
            print((r.stdout or r.stderr).strip())
            print("돌릴 수 없음 — 격리 사본이 뜨지 않았습니다")
            return 2
        started = True
    else:
        print("떠 있는 격리 사본(포트 %d)을 씁니다 — 끄지 않습니다" % a.port)

    plan = [("화면 오류", "check_console.py", []),
            ("배치", "check_layout.py", ["--seed-targets", "17"]),
            ("자동 배치", "check_adapt.py", []),
            ("폼 저장", "check_forms.py", [])]
    if a.doctor:
        plan.append(("정비 점검", "doctor.py", ["--no-installed"]))
    if a.hotfix:
        plan.append(("고침 꾸러미", "hotfix_lab.py", ["run", "--replace"] + (["--keep-app"] if (a.keep or not started) else [])))
    rows = []
    try:
        for label, name, args in plan:
            print("… %s (%s)" % (label, name), flush=True)
            code, last, secs = tool(name, args, a.port, a.verbose)
            rows.append((label, code, last, secs))
    except KeyboardInterrupt:
        rows.append(("(멈춤)", 2, "사용자가 멈춤(Ctrl-C)", 0))
    finally:
        if started and not a.keep and proc.port_open(a.port):
            subprocess.run([sys.executable, str(HERE / "iso.py"), "--port", str(a.port), "stop"], capture_output=True, text=True)
            print("여기서 띄운 격리 사본을 껐습니다")

    print("\n  %-10s %-6s %6s  %s" % ("점검", "결과", "초", "마지막 줄"))
    for label, code, last, secs in rows:
        mark = {0: "통과", 1: "문제", 2: "못 돎"}.get(code, "문제(%d)" % code)
        print("  %-10s %-6s %6.0f  %s" % (label, mark, secs, last[:110]))
    bad = [r for r in rows if r[1] != 0]
    print(("\n통과 — 점검 %d가지 모두" % len(rows)) if not bad else
          "\n문제 %d건 — %s (자세히: -v 또는 그 공구를 따로)" % (len(bad), ", ".join(r[0] for r in bad)))
    return 0 if not bad else (2 if all(r[1] == 2 for r in bad) else 1)


if __name__ == "__main__":
    sys.exit(main())
