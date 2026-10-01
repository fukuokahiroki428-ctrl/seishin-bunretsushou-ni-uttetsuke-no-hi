#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""충돌 보고서 읽기 — macOS 가 남긴 한이시키 충돌(.ips)을 사람이 읽을 만하게. 읽기만 한다.

    python3 akashi/crash_report.py              # 최근 충돌 목록
    python3 akashi/crash_report.py --latest     # 가장 최근 것의 멈춘 스레드 자세히
    python3 akashi/crash_report.py --show 2     # 목록 2번
    python3 akashi/crash_report.py --file X.ips # 받은 보고서 파일(사용자가 보내 준 것 등)

★ 왜 따로 만들었나
  .ips 는 첫 줄이 머리(JSON), 나머지가 몸(JSON)인 두 덩어리 파일이라 그대로 열면 수천 줄이다. 볼 것은
  몇 개뿐이다 — 무슨 예외인지, 어느 스레드가 멈췄는지, 그 스레드의 위쪽 프레임 중 '우리 코드' 가 어디인지.
  2026-09 팬박스 충돌(runFanboxCollection SIGSEGV)도 그 몇 줄로 원인(서로 다른 임시 객체의 begin/end)을 찾았다.

★ 찍지 않는 것
  crashReporterKey · sleepWakeUUID · bootSessionUUID · userID · incident id 같은 기기·사람을 가리키는 값.
  경로는 진짜 홈을 ~ 로 줄인다. 그래서 출력을 그대로 기록이나 인계서에 붙여도 된다.

★ 흔한 모양과 뜻(짐작 줄)
  SIGSEGV/SIGBUS + 우리 코드 프레임 → 우리 버그일 가능성이 크다(널·해제된 메모리·엇갈린 반복자).
  SIGBUS + KERN_MEMORY_ERROR · 또는 termination 이 CODESIGNING → 앱 파일이 실행 중에 바뀌었다
  (켜 둔 채 설치 · 번들 재서명). install.py 가 켜 둔 앱을 먼저 닫게 하는 까닭이다.
  SIGABRT → qFatal·assert·std::terminate. 위 프레임에 abort 를 부른 쪽이 보인다.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lib import paths  # noqa: E402

NAMES = {"Hanishiki", "Miyo", "Predormition"}          # 지금 이름과 옛 이름
HIDE = {"crashReporterKey", "sleepWakeUUID", "bootSessionUUID", "userID", "incident", "incident_id",
        "deviceIdentifierForVendor", "logWritingSignature"}


def report_dirs() -> list:
    base = paths.real_home() / "Library" / "Logs" / "DiagnosticReports"
    return [base, base / "Retired", Path("/Library/Logs/DiagnosticReports")]


def our_names() -> set:
    names = set(NAMES)
    for app in [paths.find_build_app()] + paths.find_installed_apps():
        if app:
            names.add(paths.exe_of(app).name)
    return names


def read_ips(path: Path):
    """(머리, 몸). 옛 .crash 글 형식이면 None."""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        first = f.readline()
        rest = f.read()
    head = json.loads(first)
    body = json.loads(rest) if rest.strip().startswith("{") else {}
    return head, body


def find_reports(names: set) -> list:
    out = []
    for d in report_dirs():
        for p in glob.glob(str(d / "*.ips")):
            base = os.path.basename(p)
            if not any(base.startswith(n + "-") or base.startswith(n + "_") for n in names):
                continue
            out.append(Path(p))
    return sorted(set(out), key=lambda p: p.stat().st_mtime, reverse=True)


def sh(s) -> str:
    return paths.short(s) if s else ""


def frame_text(fr: dict, images: list, ours: set) -> tuple:
    img = images[fr["imageIndex"]] if 0 <= fr.get("imageIndex", -1) < len(images) else {}
    name = img.get("name") or Path(img.get("path") or "?").name
    if "symbol" in fr:
        where = "%s + %s" % (fr["symbol"], fr.get("symbolLocation", 0))
    else:
        where = "0x%x" % fr.get("imageOffset", 0)
    mine = name in ours
    return ("★ " if mine else "  ") + "%s!%s" % (name, where), mine


def summarize(path: Path, ours: set) -> dict:
    head, body = read_ips(path)
    exc = body.get("exception") or {}
    term = body.get("termination") or {}
    ti = body.get("faultingThread", 0)
    threads = body.get("threads") or []
    th = threads[ti] if 0 <= ti < len(threads) else {}
    images = body.get("usedImages") or []
    frames = [frame_text(fr, images, ours) for fr in (th.get("frames") or [])]
    first_ours = next((t for t, mine in frames if mine), "")
    top = frames[0][0].strip() if frames else ""
    hint = []
    sig, typ, sub = exc.get("signal", ""), exc.get("type", ""), exc.get("subtype", "")
    if term.get("namespace") == "CODESIGNING" or "KERN_MEMORY_ERROR" in sub:
        hint.append("앱 파일이 실행 중에 바뀐 것으로 보입니다(켜 둔 채 설치·재서명) — 코드 버그가 아닐 수 있습니다")
    elif sig in ("SIGSEGV", "SIGBUS") and first_ours:
        hint.append("우리 코드에서 잘못된 메모리를 만졌습니다 — ★ 줄부터 보십시오")
    elif sig == "SIGABRT":
        hint.append("스스로 멈췄습니다(qFatal·assert·terminate) — abort 를 부른 위쪽 프레임을 보십시오")
    return {
        "file": sh(path), "time": head.get("timestamp", ""), "app": head.get("app_name") or head.get("name"),
        "version": head.get("app_version") or head.get("build_version") or "", "os": head.get("os_version", ""),
        "exception": " · ".join(x for x in (typ, sig, sub) if x),
        "termination": " ".join(str(term.get(k, "")) for k in ("namespace", "indicator") if term.get(k)),
        "thread": "#%d %s%s" % (ti, th.get("name", ""), (" [" + th["queue"] + "]") if th.get("queue") else ""),
        "top": top, "first_ours": first_ours.strip(), "frames": [t for t, _ in frames],
        "proc": sh(body.get("procPath", "")), "parent": body.get("parentProc", ""),
        "region": sh(str(body.get("vmRegionInfo", "")).strip())[:400], "hint": hint,
    }


def show(s: dict, n: int) -> None:
    print("%s  %s %s" % (s["time"], s["app"], s["version"]))
    print("  파일      %s" % s["file"])
    print("  실행 파일 %s  (부모 %s)" % (s["proc"], s["parent"]))
    print("  예외      %s" % s["exception"])
    if s["termination"]:
        print("  끝남      %s" % s["termination"])
    print("  멈춘 곳   %s" % s["thread"])
    for h in s["hint"]:
        print("  짐작      " + h)
    if s["region"]:
        print("  메모리    " + s["region"].splitlines()[0][:160])
    print("  ── 위에서 %d프레임 (★ = 우리 실행 파일)" % min(n, len(s["frames"])))
    for i, t in enumerate(s["frames"][:n]):
        print("  %2d %s" % (i, t))


def main() -> int:
    ap = argparse.ArgumentParser(description="충돌 보고서 읽기 — 한이시키 .ips 를 요약한다(읽기만)")
    ap.add_argument("--latest", action="store_true", help="가장 최근 충돌을 자세히")
    ap.add_argument("--show", type=int, metavar="N", help="목록의 N번을 자세히")
    ap.add_argument("--file", help="이 .ips 파일을 읽는다")
    ap.add_argument("--frames", type=int, default=20, help="보일 프레임 수 (기본 20)")
    ap.add_argument("-n", type=int, default=10, help="목록에 보일 개수 (기본 10)")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    ours = our_names()

    if a.file:
        files = [Path(a.file)]
    else:
        files = find_reports(ours)
        if not files:
            print("한이시키 충돌 보고서가 없습니다 (%s)" % ", ".join(sh(d) for d in report_dirs()[:2]))
            return 0
    try:
        if a.file or a.latest or a.show:
            idx = 0 if (a.file or a.latest) else a.show - 1
            if not 0 <= idx < len(files):
                print("그런 번호가 없습니다 — 1~%d" % len(files))
                return 2
            s = summarize(files[idx], ours)
            if a.json:
                print(json.dumps(s, ensure_ascii=False, indent=1))
            else:
                show(s, a.frames)
            return 0
        rows = []
        for i, f in enumerate(files[:a.n], 1):
            try:
                rows.append(summarize(f, ours))
            except (OSError, ValueError, KeyError) as e:
                rows.append({"file": sh(f), "time": "", "exception": "읽지 못함 (%s)" % type(e).__name__,
                             "first_ours": "", "top": "", "hint": []})
        if a.json:
            print(json.dumps(rows, ensure_ascii=False, indent=1))
            return 0
        for i, s in enumerate(rows, 1):
            print("%2d  %s  %s" % (i, s["time"][:19], s["exception"]))
            print("      %s" % (s["first_ours"] or s["top"] or "-"))
            for h in s["hint"]:
                print("      → " + h)
        print("충돌 %d건 (보인 것 %d) — 자세히: --show N 또는 --latest" % (len(files), len(rows)))
    except (OSError, ValueError) as e:
        print("읽지 못함 — %s" % e)
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
