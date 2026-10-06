#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""끄기 시험 — 고침 꾸러미 확인이 도는 사이 앱을 꺼도 깨끗이 끝나는가. 격리 사본 + 이 맥 안의 '늦게 답하는' 서버.

    python3 akashi/tests/quit_during_hotfix.py          # 혼자(약 1분)
    AKASHI_CRASH_WAIT=240 python3 akashi/tests/quit_during_hotfix.py --app <옛 판.app>   # 옛 판이 죽는지(보고서를 오래 기다림)

살피는 것:
  1 앱이 켜진 뒤 고침 꾸러미 확인(manifest.json)이 이 맥 안의 서버로 온다 — 서버는 답하지 않고 붙든다
  2 그 요청이 붙들린 동안 사본을 끈다(iso.py stop — SIGTERM, 앱은 이것을 정상 종료로 바꾼다)
  3 강제로 끄지(SIGKILL) 않고 몇 초 안에 끝났다
  4 기록 끝에 '[exit] 정상 종료' — Qt 를 다 치운 뒤에 찍히는 줄(main.cpp)이 있다
  5 이 PID 의 충돌 보고서(~/Library/Logs/DiagnosticReports/Hanishiki-*.ips)가 생기지 않았다
  6 TLS 를 켤 때 주 스레드에서 시스템 인증서까지 미리 읽었다(기록의 '[TLS] … 인증서 N개' — N > 0)

★ 왜 — 2026-10-06 충돌 보고: 고침 꾸러미 확인은 켠 지 20초 뒤 떨어져 나간 스레드에서 도는데, 앱이 끝날 때 그 스레드를
  기다리지 않았다. 확인이 첫 HTTPS 를 시작하는 순간(TLS 플러그인 올리기) 앱이 끝나던 중이라, 치워지는 Qt 를 건드려
  SIGSEGV 로 죽었다(격리 사본이 30초 만에 꺼질 때 걸림). 고친 것: 끝날 때 '끝나는 중' 을 켜 HttpClient 가 새 요청을
  안 하고 진행 중인 것도 0.1초 안에 멈추게, 그 스레드를 최대 3초 기다리게, TLS 는 켤 때 주 스레드에서 미리 준비하게.
★ 이 시험이 재는 것과 못 재는 것 — 실제 충돌은 '첫 TLS 준비' 와 '끄기' 가 수십 밀리초 안에서 겹쳐야 난다. 그 순간은
  맞추기 어려워, 고치기 전 판으로 이 시험을 돌려도 죽지는 않았다(2026-10-06 — 보고서 240초 지켜봄). 그래서 이 시험은
  고친 길이 실제로 도는지를 잰다: 요청이 붙들린 채 꺼도 Qt 를 다 치우고 끝나는지(4), 인증서를 주 스레드에서 미리
  읽었는지(6) — 충돌이 난 자리(뒷일 스레드가 인증서를 읽으며 TLS 플러그인을 다시 찾기)가 뒷일 스레드에서 길게
  일어나지 않게 하는 쪽. 서버가 http 라 이 시험의 요청 자체는 TLS 를 타지 않는다.
"""
from __future__ import annotations

import http.server
import json
import os
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _common as C  # noqa: E402
import fixtures  # noqa: E402

HITS = []


class Slow(http.server.BaseHTTPRequestHandler):
    """/hotfix/manifest.json 을 받으면 적어 두고 60초 동안 답하지 않는다(끊기면 그만둔다)."""

    def do_GET(self):
        HITS.append((time.time(), self.path))
        if 'manifest' in self.path:
            end = time.time() + 60
            while time.time() < end:
                time.sleep(0.2)
                try:
                    self.wfile.flush()
                except OSError:
                    return
        self.send_response(404)
        self.end_headers()

    def log_message(self, *a):
        pass


def crash_report_for(pid: int, since: float):
    d = Path.home() / 'Library' / 'Logs' / 'DiagnosticReports'
    for f in sorted(d.glob('Hanishiki-*.ips')):
        try:
            if f.stat().st_mtime < since:
                continue
            body = f.read_text(encoding='utf-8', errors='replace')
            if '"pid" : %d,' % pid in body or '"pid":%d,' % pid in body:
                return f.name
        except OSError:
            continue
    return None


def main() -> int:
    check = C.Checks()
    t_start = time.time()
    srv = fixtures.QuietServer(('127.0.0.1', 0), Slow)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    base = 'http://127.0.0.1:%d/hotfix/' % srv.server_address[1]
    pid = None
    try:
        if not C.iso_start('--hotfix-base', base):
            return 2
        pid = int(C.iso_state()['pid'])
        t0 = time.time()
        while time.time() - t0 < 90 and not any('manifest' in p for _, p in HITS):
            time.sleep(0.5)
        hit = [t for t, p in HITS if 'manifest' in p]
        check('고침 꾸러미 확인이 이 맥 안의 서버로 왔다(서버는 답하지 않고 붙듦)', bool(hit),
              '%.0fs 뒤' % (hit[0] - t0) if hit else '90초 안에 안 옴')
        time.sleep(1.0)                                  # 요청이 확실히 붙들린 상태에서
        t_stop = time.time()
        r = C._iso('stop')                               # SIGTERM → 앱은 정상 종료로 바꾼다 · 기록은 남긴다
        took = time.time() - t_stop
        out = (r.stdout or '') + (r.stderr or '')
        check('강제로 끄지 않고 끝났다(SIGKILL 없음)', 'SIGKILL' not in out, out.strip().splitlines()[-1:] if out.strip() else '')
        check('끄는 데 오래 걸리지 않았다(8초 미만)', took < 8, '%.1fs' % took)
        log = (C.ISO / 'app.log').read_text(encoding='utf-8', errors='replace').split('akashi iso start')[-1]
        import re
        m = re.search(r'\[TLS\] 주 스레드에서 미리 준비:[^\n]*인증서 (\d+)개', log)
        check('TLS 를 켤 때 주 스레드에서 시스템 인증서까지 미리 읽었다(기록 [TLS] 줄)', bool(m) and int(m.group(1)) > 0,
              m.group(0)[-40:] if m else '줄 없음')
        check("기록 끝에 '[exit] 정상 종료'(Qt 를 다 치운 뒤의 줄)", '[exit] 정상 종료' in log,
              '' if '[exit] 정상 종료' in log else log.strip().splitlines()[-1:])
        wait = float(os.environ.get('AKASHI_CRASH_WAIT') or 20)
        t1 = time.time()
        rep = None
        while time.time() - t1 < wait and not rep:
            rep = crash_report_for(pid, t_start)
            time.sleep(2)
        check('이 PID 의 충돌 보고서가 없다(%.0f초 지켜봄)' % wait, rep is None, rep or '')
    except Exception as e:  # noqa: BLE001
        check('시험 진행 중 오류 없음', False, '%s: %s' % (type(e).__name__, e))
    finally:
        C.keep_log('quit_during_hotfix_app.log')
        C.iso_stop()
        srv.shutdown()
    return check.summary()


if __name__ == '__main__':
    sys.exit(main())
