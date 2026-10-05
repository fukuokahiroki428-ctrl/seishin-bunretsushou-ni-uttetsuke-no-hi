#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SingleFile 캡처 시험 — 격리 사본의 수집용 실제 Chrome + 이 맥 안의 시험 서버 둘(인터넷으로 안 나감).

    python3 akashi/tests/singlefile.py     # 혼자(약 2분)

살피는 것(쪽 다섯 + 열 수 없는 주소 하나를 실제 Chrome 크롤로):
  1 열 수 없는 주소를 뺀 다섯 쪽만 '저장' 으로 센다 — 크롬 오류 화면을 캡처 완료로 저장하지 않는다
  2 저장된 파일마다 SingleFile 결과이고, 그림이 data: 로 들어가 바깥 주소가 남지 않았다
  3 스크롤해야 뜨는 그림(IntersectionObserver) · 다른 출처 그림(CORS 머리 없음) · 엄격한 CSP 쪽도 그림이 비지 않는다
  4 화면 JS 오류 없음, 사용자 앱의 Chrome(9223)은 그대로

★ 왜 서버가 둘인가
  다른 출처 그림은 출처(포트)가 달라야 생긴다. 둘째 서버는 그림만 주고 CORS 머리를 붙이지 않는다 — 앱이 캡처 동안
  응답에 ACAO 를 붙여 주는지(그림 · 글꼴 · CSS · 미디어만)를 본다. 그 쪽(xorigin.html)은 connect-src 'self' 의 CSP 도 받는다.
★ 왜 그림 여섯 장을 3000px 씩 띄우나
  최소화된 창은 그리는 간격이 길다. 0.3초씩만 머물면 여섯 장 중 둘셋만 들어갔다 — 한 화면씩 내리며 그리기를
  기다리는지(requestAnimationFrame 두 번)를 이 쪽이 가린다.
"""
from __future__ import annotations

import glob
import json
import re
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _common as C  # noqa: E402
import fixtures  # noqa: E402

XORIGIN_CSP = "default-src 'self'; img-src *; connect-src 'self'; script-src 'unsafe-inline'"
PAGES = ['index.html', 'gallery.html', 'io.html', 'xorigin.html', 'csp.html']


def main() -> int:
    check = C.Checks()
    shutil.rmtree(C.work_dir('singlefile_captures'), ignore_errors=True)   # 지난 판의 결과가 남아 '이번 결과' 로 읽히지 않게
    user_chrome = C.listen_pids(C.USER_CAPTURE_PORT)
    print('[사용자 앱 Chrome %d] %s' % (C.USER_CAPTURE_PORT, user_chrome or '없음'))
    idle = C.user_idle_seconds()
    if idle > 300:
        # 화면이 잠들면 macOS 가 그리기를 멈춰, 최소화된 캡처 Chrome 의 IntersectionObserver 가 한 번도 판정하지 않는다
        # (2026-10-06 실측: 옛 판 1.22.98 · 새 판 1.28.1 모두 io.html 0/6, 나머지 쪽은 그대로). 판의 문제가 아니다.
        print('   주의: 사람 입력 없이 %d분 — 화면이 꺼져 있으면 지연 그림 쪽(io.html)이 0/6 로 실패한다(판과 무관)' % (idle // 60))
    site = fixtures.build_site(C.work_dir('testsite_sf'))
    sb, b = fixtures.serve(site)                                   # 다른 출처 — 그림만
    (site / 'io.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>스크롤해야 뜨는 그림</title><h1>IO</h1>' +
        ''.join('<div style="height:3000px"></div><img class="io" data-src="/img/%s.png?io=%d" width="60" height="60" alt="io%d">'
                % (c, i, i) for i, c in enumerate(['red', 'blue', 'green', 'lazy1', 'lazy2', 'bg'])) +
        '<script>var o=new IntersectionObserver(function(es){es.forEach(function(e){if(e.isIntersecting){e.target.src=e.target.dataset.src;o.unobserve(e.target);}})});'
        'document.querySelectorAll("img.io").forEach(function(i){o.observe(i)});</script>', encoding='utf-8')
    (site / 'xorigin.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>다른 출처 그림</title><h1>교차 출처</h1>'
        '<img src="http://127.0.0.1:%d/img/blue.png" width="60"><img src="http://127.0.0.1:%d/img/green.png" width="60">' % (b, b),
        encoding='utf-8')
    sa, a = fixtures.serve(site, csp={'/xorigin.html': XORIGIN_CSP})
    urls = ['http://127.0.0.1:%d/%s' % (a, pg) for pg in PAGES] + ['http://127.0.0.1:%d/nothing.html' % C.free_port()]
    try:
        if not C.iso_start():
            return 2
        out = C.ISO / 'tmp' / 'sf-out'
        shutil.rmtree(out, ignore_errors=True)
        out.mkdir(parents=True, exist_ok=True)
        with C.page() as p:
            time.sleep(1.5)
            p.eval("""(function(){var g=function(i){return document.getElementById(i)}; switchTab('crawl');
              g('crawl-url').value=%s; g('crawl-path').value=%s; g('crawl-real-chrome').checked=true;
              var w=g('crawl-wait-login'); if(w) w.checked=false; var lc=g('crawl-login-check'); if(lc) lc.value='';
              startCrawl();})()""" % (json.dumps(' '.join(urls)), json.dumps(str(out))))
            t0 = time.time()
            txt = ''
            while time.time() - t0 < 600:
                txt = p.eval("(document.getElementById('crawl-log')||{}).innerText||''") or ''
                if '크롤 완료' in txt or '중단' in txt:
                    break
                time.sleep(2)
            print('   걸린 시간 %.0fs' % (time.time() - t0))
            m = re.search(r'크롤 완료: (\d+)/(\d+)', txt)
            check('실제 Chrome 크롤이 끝났다', bool(m), txt[-200:].replace('\n', ' / '))
            if m:
                check('열 수 없는 주소를 뺀 %d개만 저장으로 셈' % len(PAGES), int(m.group(1)) == len(PAGES), '%s/%s' % m.groups())
            errs = C.page_errors(p)
            check('화면 JS 오류 없음', not errs, errs[:2])
        files = sorted(glob.glob(str(out) + '/**/captures/*.html', recursive=True))
        print('   저장된 파일:', [Path(f).name for f in files])
        # 사본은 끝나면 지워지므로 결과를 작업 폴더에 남긴다 — 판을 바꿀 때 앞뒤를 견주거나 실패를 볼 때
        keep = C.work_dir('singlefile_captures')
        shutil.rmtree(keep, ignore_errors=True)
        keep.mkdir(parents=True)
        for f in files:
            shutil.copy(f, keep / Path(f).name)
        check('쪽마다 파일 하나(오류 화면은 저장 안 됨)', len(files) == len(PAGES), len(files))
        for f in files:
            h = Path(f).read_text(encoding='utf-8', errors='replace')
            sf = 'Page saved with SingleFile' in h
            err = 'chrome-error://' in h or 'ERR_CONNECTION_REFUSED' in h
            left = re.findall(r'<img[^>]+src="?https?://127\.0\.0\.1', h)
            imgs = len(re.findall(r'<img', h))
            datas = len(re.findall(r'<img[^>]+src="?data:image/[a-z]+;base64,', h))
            empties = len(re.findall(r'src="?data:,', h))
            if datas < imgs:
                for tag in re.findall(r'<img[^>]*>', h):
                    print('      ' + re.sub(r'(base64,)[A-Za-z0-9+/=]{20,}', r'\1…', tag)[:220])
            check('%s — SingleFile 결과 · 그림 %d개 중 data: %d개 · 빈 그림 %d · 바깥 주소 %d' % (Path(f).name, imgs, datas, empties, len(left)),
                  sf and not err and not left and empties == 0 and datas >= max(1, imgs - 1))
    except Exception as e:  # noqa: BLE001
        check('시험 진행 중 오류 없음', False, '%s: %s' % (type(e).__name__, e))
    finally:
        C.keep_log('singlefile_app.log')
        C.iso_stop()
        sa.shutdown()
        sb.shutdown()
    check('사용자 앱의 Chrome(%d)은 그대로' % C.USER_CAPTURE_PORT, C.listen_pids(C.USER_CAPTURE_PORT) == user_chrome,
          C.listen_pids(C.USER_CAPTURE_PORT))
    return check.summary()


if __name__ == '__main__':
    sys.exit(main())
