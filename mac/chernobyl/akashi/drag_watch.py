#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""창 끌기 확인 — 사람이 격리 사본의 창을 끄는 동안 앱이 무엇을 했는지 실시간으로 보여 준다.

    python3 akashi/drag_watch.py              # 기록을 지켜보며 안내 (Ctrl-C 로 끝)
    python3 akashi/drag_watch.py --regions    # 지금 화면이 넘기는 '끌 자리' 만 보이고 끝
    python3 akashi/drag_watch.py --anywhere   # '아무 데나 끌기' 시험 상태로 (끝나면 되돌림)
    python3 akashi/drag_watch.py --seconds 60

★ 왜 사람이 끌어야 하나
  창 끌기는 macOS 창 서버에 '지금 들어온 진짜 마우스 누름' 을 넘겨야 부드럽다(performWindowDragWithEvent:).
  흉내 낸 누름은 창 서버가 조용히 무시한다 — 처음에 그렇게 짜서 두 번을 헛빌드했다. 그리고 이 맥에는
  손쉬운 사용 권한이 없어 OS 마우스를 흉내 낼 수도 없다. 그래서 공구는 '어디를 끌어 보라' 고 안내하고,
  앱 기록의 [창 끌기] 줄로 무슨 길을 탔는지 보여 준다.

★ 기록 줄의 뜻 (src/core/MainWindow.cpp)
  「끌 자리 N곳 · 단추 자리 M곳 받음 (열쇠)」 — 화면(publishDragRegions)이 좌표를 넘겼다. 열쇠는 main 또는 기능 창 이름.
  「창 서버에 맡김」 — 네이티브 길로 끌었다(부드러운 길). 처음 세 번만 적힌다.
  이 줄 없이 창이 움직였다면 8ms 타이머 예비 길로 끈 것이다(굼뜬 길).

★ --anywhere
  화면이 보내는 '마우스가 끌 자리 위에 있다' 신호(backend.setDragHover)를 늘 참으로 바꿔, 좌표·신호 판단과
  네이티브 필터를 떼어 본다. 어디를 잡아도 창이 부드럽게 끌리면 네이티브 쪽은 멀쩡하고 문제는 판단 쪽이다.
  끝날 때(Ctrl-C 포함) 원래 함수로 되돌린다.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lib.cdp import CdpError, JsError, Page  # noqa: E402

# index.html 의 publishDragRegions 와 같은 고르개 — 그쪽이 바뀌면 여기도 (창 끌기 IIFE 안의 ZONE_SEL·HOLE_SEL)
JS_REGIONS = r"""(function(){
  var ZONE='.sidebar, .sidebar-header, .toolbar';
  var HOLE='button, input, select, textarea, a, [onclick], [contenteditable], .nav-item, label';
  function vis(e){var r=e.getBoundingClientRect();return r.width>0&&r.height>0;}
  function rc(e){var r=e.getBoundingClientRect();return [Math.round(r.left),Math.round(r.top),Math.round(r.width),Math.round(r.height)];}
  var zones=[].slice.call(document.querySelectorAll(ZONE)).filter(vis), holes=0;
  var out=zones.map(function(z){var h=[].slice.call(z.querySelectorAll(HOLE)).filter(vis).length; holes+=h;
    return {sel:(z.className||z.tagName).toString().split(' ')[0], rect:rc(z), holes:h};});
  var m=(location.hash.match(/window=([a-z]+)/)||location.search.match(/window=([a-z]+)/)||[])[1]||'main';
  return {key:m, zones:out, holes:holes, fxBar:[].slice.call(document.querySelectorAll('.tab-content.fx.active > .fx-bar')).filter(vis).length};
})()"""

JS_ANY_ON = r"""(function(){
  if (!window.backend || !backend.setDragHover) return 'no-backend';
  if (!window.__akashiHover) window.__akashiHover = backend.setDragHover;
  window.__akashiHoverLog = 0;
  backend.setDragHover = function(v){ window.__akashiHoverLog++; };   // 화면의 판단은 세기만
  window.__akashiHover.call(backend, true);                          // 네이티브 쪽은 늘 '끌 수 있음'
  return 'on';
})()"""
JS_ANY_OFF = r"""(function(){
  if (!window.__akashiHover) return 'none';
  backend.setDragHover = window.__akashiHover; window.__akashiHover.call(backend, false);
  var n = window.__akashiHoverLog || 0; delete window.__akashiHover; delete window.__akashiHoverLog;
  return n;
})()"""


def iso_state(port: int) -> dict:
    import tempfile
    root = Path(os.environ.get("AKASHI_ISO_ROOT") or (Path(tempfile.gettempdir()) / "akashi-iso"))
    try:
        return json.loads((root / str(port) / "state.json").read_text("utf-8"))
    except (OSError, ValueError):
        return {}


def main() -> int:
    ap = argparse.ArgumentParser(description="창 끌기 확인 — 사람이 끄는 동안 [창 끌기] 기록을 보여 준다")
    ap.add_argument("--port", type=int, default=int(os.environ.get("AKASHI_PORT", 9334)))
    ap.add_argument("--regions", action="store_true", help="지금 넘기는 끌 자리만 보이고 끝")
    ap.add_argument("--anywhere", action="store_true", help="'아무 데나 끌기' 시험 상태(끝나면 되돌림)")
    ap.add_argument("--seconds", type=float, default=0, help="지켜볼 초 (기본: Ctrl-C 까지)")
    a = ap.parse_args()

    st = iso_state(a.port)
    log = Path(st.get("log", "")) if st.get("log") else None
    try:
        p = Page.attach(a.port)
    except (CdpError, OSError) as e:
        print("격리 사본에 붙지 못함 — %s (iso.py start 부터)" % e)
        return 2
    with p:
        try:
            reg = p.eval(JS_REGIONS)
        except JsError as e:
            print("끌 자리를 읽지 못함 — %s" % e)
            return 1
        print("끌 자리(%s) — 띠 %d곳 · 그 안의 단추 자리 %d곳 · 기능 화면 위 띠 %d" %
              (reg["key"], len(reg["zones"]), reg["holes"], reg["fxBar"]))
        for z in reg["zones"]:
            print("  %-16s %s  단추 %d" % (z["sel"], z["rect"], z["holes"]))
        if a.regions:
            return 0 if reg["zones"] else 1
        if not log or not log.exists():
            print("격리 사본 기록을 찾지 못했습니다 — iso.py 로 띄운 사본에서만 기록을 따라갑니다")
            return 2

        if a.anywhere:
            r = p.eval(JS_ANY_ON)
            print("아무 데나 끌기 — %s" % ("켬" if r == "on" else r))
        print("\n이제 창을 끌어 보십시오: ① 왼쪽 목록(사이드바) 빈 곳 ② 위 제목 띠 ③ 본문의 빈 바탕"
              + (" ④ 어디든(--anywhere)" if a.anywhere else "") + "\n(Ctrl-C 로 끝)\n")
        seen = {"regions": 0, "native": 0}
        pos = log.stat().st_size
        end = time.time() + a.seconds if a.seconds else None
        try:
            while end is None or time.time() < end:
                with open(log, "r", encoding="utf-8", errors="replace") as f:
                    f.seek(pos)
                    chunk = f.read()
                    pos = f.tell()
                for line in chunk.splitlines():
                    if "[창 끌기]" in line:
                        print(time.strftime("%H:%M:%S ") + line.strip())
                        if "창 서버에 맡김" in line:
                            seen["native"] += 1
                        elif "받음" in line:
                            seen["regions"] += 1
                time.sleep(0.3)
        except KeyboardInterrupt:
            pass
        finally:
            if a.anywhere:
                try:
                    n = p.eval(JS_ANY_OFF)
                    print("\n아무 데나 끌기 끔 — 그동안 화면 판단 신호 %s번(넘기지 않고 셈만)" % n)
                except (CdpError, OSError):
                    print("\n되돌리지 못했습니다 — 화면을 새로 고치면 원래대로 돌아옵니다")
        print("\n요약 — 좌표 받음 %d번 · 창 서버에 맡김 %d번%s" % (
            seen["regions"], seen["native"],
            "" if seen["native"] else " (끌었는데 0이면 예비 타이머 길 — docs/창끌기.md)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
