#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""크롤러(経済産業省 탭) 시험 — 격리 사본 + 이 맥 안의 시험 사이트(인터넷으로 안 나감).

    python3 akashi/tests/crawl.py          # 혼자(약 2분)
    python3 akashi/tests/run_all.py        # 다른 기능 시험과 함께

살피는 것:
  1 사이드바에 항목이 보이고 탭이 열린다
  2 끝까지 돈 판 — 상태 Done, 페이지·자원 수가 0 으로 지워지지 않음, 오프라인 사본(index.html 과 쪽들)이 생김
  3 중간에 멈춘 판 — 크롤러 자신의 끝 줄이 40초 안에 오고, 상태 '중단됨', 늦게 온 끝 신호가 그것을 덮지 않음
  4 그림을 받는 도중 멈추기 세 번 — 제때 끝나고 앱이 살아 있다
  5 화면 JS 오류 없음, 사용자 앱의 Chrome(9223)은 그대로

★ 왜 '받는 도중 멈추기' 를 세 번이나 하나
  그림 받기는 HttpClient 안의 중첩 이벤트 루프에서 돈다. 예전엔 stop() 이 아무 때나 끝 처리를 예약해서,
  그 루프 안에서 크롤러가 지워지는 use-after-free 로 앱이 죽었다(a8eb4ce 에서 '로그인 대기일 때만' 으로 고침).
  늦게 주는 그림(/slow/ — 2초)을 받는 중에 기다리는 시간을 바꿔 가며 멈춘다.
★ 왜 '중단됨' 표시가 아니라 크롤러의 끝 줄을 기다리나
  중지 단추는 누르자마자 화면에 '중단됨' 을 쓴다. 크롤러가 정말 끝났는지는 로그의 '크롤링 완료/중단됨' 줄로만 안다.
"""
from __future__ import annotations

import glob
import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _common as C  # noqa: E402
import fixtures  # noqa: E402
from lib.cdp import Page  # noqa: E402


def wait_finished(p, secs):
    t0 = time.time()
    while time.time() - t0 < secs:
        txt = p.eval("(document.getElementById('crawl-log')||{}).innerText||''") or ''
        if '크롤링 완료' in txt or '크롤링 중단됨' in txt:
            return time.time() - t0
        time.sleep(0.5)
    return None


def main() -> int:
    check = C.Checks()
    user_chrome = C.listen_pids(C.USER_CAPTURE_PORT)
    print('[사용자 앱 Chrome %d] %s' % (C.USER_CAPTURE_PORT, user_chrome or '없음'))
    site = fixtures.build_site(C.work_dir('testsite'))
    srv, hp = fixtures.serve(site)
    snap = C.work_dir('snaps_crawl')
    for f in snap.glob('*.png'):
        f.unlink()
    try:
        if not C.iso_start('--env', 'HANISHIKI_SNAPSHOT_DIR=%s' % snap):
            return 2
        out = C.ISO / 'tmp' / 'crawl-out'
        out.mkdir(parents=True, exist_ok=True)
        with Page.attach(C.PORT) as p:
            C.watch_errors(p)
            time.sleep(1.5)
            vis = p.eval("(function(){var n=[...document.querySelectorAll('.nav-item')].find(e=>(e.getAttribute('onclick')||'').indexOf(\"'crawl'\")>=0);"
                         "return n? (n.offsetParent!==null) : null})()")
            check('사이드바에 経済産業省 항목이 보인다', vis is True, vis)
            p.eval("switchTab('crawl')")
            time.sleep(0.8)
            shown = p.eval("(function(){var t=document.getElementById('tab-crawl');return !!t && t.offsetParent!==null})()")
            check('크롤 탭이 열린다', shown is True, shown)

            def run(url, depth, maxp, delay):
                p.eval("""(function(){var g=function(i){return document.getElementById(i)};
                  g('crawl-url').value=%s; g('crawl-depth').value=%d; g('crawl-maxpages').value=%d; g('crawl-delay').value=%s;
                  g('crawl-path').value=%s; g('crawl-same-domain').checked=true;
                  ['crawl-deep-scroll','crawl-security','crawl-exif','crawl-wait-login'].forEach(function(i){var e=g(i); if(e) e.checked=false});
                  var m=g('crawl-method'); if(m) m.value=''; var rc=g('crawl-real-chrome'); if(rc) rc.checked=false;
                  startCrawl();})()""" % (json.dumps(url), depth, maxp, json.dumps(str(delay)), json.dumps(str(out))))

            stopped = ('중단됨', 'Done', 'Stopped', '중지됨')
            # 2) 끝까지 도는 판
            run('http://127.0.0.1:%d/index.html' % hp, 2, 10, 0.2)
            st, dt = C.wait_status(p, 'crawl', ('Done', '완료', '중단됨', 'Error', '오류'), 180)
            check('끝까지 돈 판이 끝났다(finished 수신)', dt is not None, '%.1fs' % dt if dt else st)
            check('상태가 Done', str(st.get('status')) in ('Done', '완료'), st.get('status'))
            check('페이지 수가 0으로 지워지지 않음', int(st.get('posts') or 0) > 0, st)
            idx = glob.glob(str(out) + '/**/index.html', recursive=True)
            htmls = glob.glob(str(out) + '/**/*.html', recursive=True)
            check('오프라인 사본이 생겼다', bool(idx) and len(htmls) >= 3, '%d html' % len(htmls))

            # 3) 중간에 멈추는 판 — 느린 간격으로 시작하고 곧 멈춘다
            time.sleep(1)
            run('http://127.0.0.1:%d/index.html' % hp, 3, 50, 3.0)
            time.sleep(4)
            before = C.platform_stats(p, 'crawl')
            p.eval("stopCrawl()")
            st2, _ = C.wait_status(p, 'crawl', stopped, 30)
            dt2 = wait_finished(p, 40)
            check('멈춘 판이 40초 안에 끝났다(크롤러의 끝 줄)', dt2 is not None, ('%.1fs' % dt2) if dt2 else st2)
            st2 = C.platform_stats(p, 'crawl')
            check("상태가 '중단됨'", str(st2.get('status')) in ('중단됨', 'Stopped', '중지됨'), st2.get('status'))
            print('   멈추기 직전', before, '→ 뒤', st2)
            time.sleep(2)
            st3 = C.platform_stats(p, 'crawl')
            check("늦게 온 끝 신호가 '중단됨' 을 덮지 않음", str(st3.get('status')) == str(st2.get('status')), st3.get('status'))

            # 4) 그림을 받는 도중에 멈추기 — 세 번, 기다리는 시간을 바꿔 가며
            for k in range(3):
                time.sleep(1)
                run('http://127.0.0.1:%d/slow.html' % hp, 1, 5, 0.2)
                time.sleep(5 + k)
                p.eval("stopCrawl()")
                st4, _ = C.wait_status(p, 'crawl', stopped, 40)
                dt4 = wait_finished(p, 40)
                time.sleep(3)
                alive = C.iso_alive()
                check('받는 도중 멈춤 %d — 제때 끝나고 앱이 살아 있다' % (k + 1), dt4 is not None and alive,
                      '%s · 살아 있음=%s · %s' % (('%.1fs' % dt4) if dt4 else '시간 초과', alive, st4))
            errs = C.page_errors(p)
            check('화면 JS 오류 없음', not errs, errs[:2])
        snaps = sorted(x.name for x in snap.glob('*.png'))
        print('   터미널 창 그림:', snaps[:6])
    except Exception as e:  # noqa: BLE001 — 시험 도중의 어떤 실패도 판정 한 줄로
        check('시험 진행 중 오류 없음', False, '%s: %s · 앱 살아 있음=%s' % (type(e).__name__, e, C.iso_alive()))
    finally:
        C.keep_log('crawl_app.log')
        C.iso_stop()
        srv.shutdown()
    check('사용자 앱의 Chrome(%d)은 그대로' % C.USER_CAPTURE_PORT, C.listen_pids(C.USER_CAPTURE_PORT) == user_chrome,
          C.listen_pids(C.USER_CAPTURE_PORT))
    return check.summary()


if __name__ == '__main__':
    sys.exit(main())
