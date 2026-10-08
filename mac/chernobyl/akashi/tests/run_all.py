#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""기능 시험 — 격리 사본으로 기능을 실제로 돌려 보는 시험들을 차례로 돌리고 표로 알린다(약 8분).

    python3 akashi/tests/run_all.py                         # 일곱 가지 모두
    python3 akashi/tests/run_all.py --only crawl,viewer     # 고른 것만
    python3 akashi/tests/run_all.py --app <다른 판.app>     # build/ 대신 그 앱으로(옛 판과 견줄 때)
    python3 akashi/tests/run_all.py -v                      # 시험마다 전체 출력

시험(차례대로):
  capture_cleanup  캡처 Chrome 정리가 이 앱 데이터 폴더의 것만 끄는가(앱 시작 · 좀비 정리 단추 · 진단 수)
  quit_during_hotfix  고침 꾸러미 확인이 도는 사이 꺼도 깨끗이 끝나는가(정상 종료 줄 · 충돌 보고서 없음 · TLS 미리 준비)
  viewer           '옛 트위터로 보기' — 계정 목록 · 계정별 화면 · 다시 누르면 바뀐 것만 · 번들에 .pyc 없음
  resume           이어받기 — 유튜브 오디오/동영상 장부 · 받는 도중 죽어도 잘린 파일 없음
  youtube_layout   유튜브 · 니코동 '저장 방식' — 채널별 폴더 / 폴더 없이 바로(파일 이름 · _complete · 엑셀)
  crawl            크롤러 — 끝까지 · 중간에 멈춤 · 그림 받는 도중 멈춤 세 번
  singlefile       SingleFile 캡처 — 오류 화면 저장 안 함 · 지연 그림 · 다른 출처 · CSP

종료 코드: 0 모두 통과 · 1 실패한 시험이 있음 · 2 돌릴 수 없음(빌드 없음 · 사본이 안 뜸 · 옛 판인데 사용자 앱이 수집 중)

★ inspect_all.py 와 무엇이 다른가
  inspect_all 은 화면을 대 보는 정기 점검(약 3분)이고, 이쪽은 기능을 실제로 돌린다 — 시험 사이트를 크롤하고, 실제 Chrome 으로
  캡처하고, 받는 도중 앱을 죽인다. 시험마다 사본을 새로 띄우고(--wipe) 끝나면 지운다. 그래서 이미 떠 있는 사본(같은 포트)은
  꺼진다 — 다른 일에 쓰던 사본이 있으면 끝낸 뒤에 돌리거나 AKASHI_PORT 로 포트를 달리한다.
★ 재료는 어디에
  시험 사이트 · 가짜 보관 폴더 · 실패했을 때 볼 사본 기록(*_app.log)은 $TMPDIR/akashi-tests(AKASHI_TEST_DIR)에 매번 새로 만든다.
  저장소에는 시험 코드만 둔다.
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
import _common as C  # noqa: E402
from lib import paths  # noqa: E402

TESTS = ["capture_cleanup", "quit_during_hotfix", "viewer", "resume", "youtube_layout", "crawl", "crawl_record", "singlefile"]
LABEL = {"capture_cleanup": "캡처 정리", "quit_during_hotfix": "끄기", "viewer": "옛 트위터", "resume": "이어받기", "youtube_layout": "저장 방식", "crawl": "크롤러", "crawl_record": "끝까지 기록", "singlefile": "SingleFile"}


def run(name: str, verbose: bool, env: dict, timeout: int = 1200):
    t0 = time.time()
    log = C.work_dir() / (name + ".log")
    try:
        r = subprocess.run([sys.executable, str(HERE / (name + ".py"))], capture_output=True, text=True, timeout=timeout, env=env)
        out, code = (r.stdout or "") + (r.stderr or ""), r.returncode
    except subprocess.TimeoutExpired as e:
        out, code = ((e.stdout or b"").decode("utf-8", "replace") if isinstance(e.stdout, bytes) else (e.stdout or "")) \
            + "\n시간 초과(%ds)" % timeout, 1
        subprocess.run([sys.executable, str(C.AKASHI / "iso.py"), "--port", str(C.PORT), "stop", "--wipe"], capture_output=True)
    log.write_text(out, encoding="utf-8")
    lines = [ln for ln in out.splitlines() if ln.strip()]
    passed = sum(1 for ln in lines if ln.lstrip().startswith("✔"))
    failed = [ln.strip() for ln in lines if ln.lstrip().startswith("✘")]
    if verbose:
        print("\n".join("    │ " + ln for ln in lines))
    elif failed or code not in (0, 1):
        print("\n".join("    │ " + ln for ln in (failed or lines[-6:])))
    verdict = next((ln.strip() for ln in reversed(lines) if ln.strip() in ("통과", "실패")), "")
    last = failed[0] if failed else (verdict or (lines[-1].strip() if lines else ""))
    return code, passed, len(failed), last, time.time() - t0


def main() -> int:
    ap = argparse.ArgumentParser(description="기능 시험 — 격리 사본으로 기능을 실제로 돌려 본다")
    ap.add_argument("--only", help="고른 시험만(쉼표로) — " + ",".join(TESTS))
    ap.add_argument("--app", help="build/ 대신 이 앱으로(.app)")
    ap.add_argument("-v", "--verbose", action="store_true", help="시험마다 전체 출력")
    a = ap.parse_args()

    names = TESTS
    if a.only:
        names = [n.strip() for n in a.only.replace(" ", ",").split(",") if n.strip()]
        bad = [n for n in names if n not in TESTS]
        if bad:
            print("모르는 시험: %s (있는 것: %s)" % (", ".join(bad), ", ".join(TESTS)))
            return 2
    env = dict(os.environ, AKASHI_PORT=str(C.PORT))
    if a.app:
        app = Path(a.app).resolve()
        if not paths.is_our_app(app):
            print("한이시키 번들이 아닙니다: " + paths.short(app))
            return 2
        env["AKASHI_TEST_APP"] = str(app)
    app = Path(env["AKASHI_TEST_APP"]) if a.app else paths.find_build_app()
    if app is None:
        print("빌드가 없습니다 — mac/chernobyl 에서 ./build.sh 부터")
        return 2
    print("시험할 앱: %s  %s · 재료: %s" % (paths.short(app), paths.version_of(app), paths.short(C.work_dir())), flush=True)

    rows = []
    try:
        for n in names:
            print("… %s (%s.py)" % (LABEL[n], n), flush=True)
            rows.append((n,) + run(n, a.verbose, env))
    except KeyboardInterrupt:
        subprocess.run([sys.executable, str(C.AKASHI / "iso.py"), "--port", str(C.PORT), "stop", "--wipe"], capture_output=True)
        rows.append(("(멈춤)", 2, 0, 0, "사용자가 멈춤(Ctrl-C)", 0))

    print("\n  %-11s %-6s %5s %5s  %s" % ("시험", "결과", "판정", "초", "마지막 줄 / 첫 실패"))
    for n, code, passed, failed, last, secs in rows:
        mark = {0: "통과", 1: "실패", 2: "못 돎"}.get(code, "실패(%d)" % code)
        print("  %-11s %-6s %5s %5.0f  %s" % (LABEL.get(n, n), mark, "%d/%d" % (passed, passed + failed), secs, last[:100]))
    bad = [r for r in rows if r[1] != 0]
    print(("\n통과 — 기능 시험 %d가지 모두" % len(rows)) if not bad else
          "\n문제 %d건 — %s (기록: %s/<이름>.log)" % (len(bad), ", ".join(LABEL.get(r[0], r[0]) for r in bad), paths.short(C.work_dir())))
    return 0 if not bad else (2 if all(r[1] == 2 for r in bad) else 1)


if __name__ == "__main__":
    sys.exit(main())
