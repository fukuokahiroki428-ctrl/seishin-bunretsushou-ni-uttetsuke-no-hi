#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""SingleFile 캡처 시험 — 격리 사본의 수집용 실제 Chrome + 이 맥 안의 시험 서버 둘(인터넷으로 안 나감).

    python3 akashi/tests/singlefile.py     # 혼자(약 5분)

살피는 것(쪽 다섯 + 열 수 없는 주소 하나를 실제 Chrome 크롤로):
  1 열 수 없는 주소를 뺀 다섯 쪽만 '저장' 으로 센다 — 크롬 오류 화면을 캡처 완료로 저장하지 않는다
  2 저장된 파일마다 SingleFile 결과이고, 그림이 data: 로 들어가 바깥 주소가 남지 않았다
  3 스크롤해야 뜨는 그림(IntersectionObserver) · 다른 출처 그림(CORS 머리 없음) · 엄격한 CSP 쪽도 그림이 비지 않는다
  4 화면 JS 오류 없음, 사용자 앱의 Chrome(9223)은 그대로
  5 (r56) 끝없이 줄이 붙는 쪽(endless.html)도 카드가 아닌 캡처로 저장되고, 멈춘 쪽은 '바닥 확인' 에 15초를 쓰지 않는다
  6 (r56) 저장 폴더로 못 옮긴 캡처 — HANISHIKI_CAP_STAGE_ALL=1(어느 곳이든 임시 디스크를 거침)로 다시 띄우고 첫 쪽 동안
    캡처 폴더를 읽기 전용(555)으로: 그 쪽은 '✅ 캡쳐 완료' 가 없고 임시 디스크에 파일 + 옆 기록(.dest.json)이 남는다 →
    755 로 되돌리고 사본을 다시 켜면 같은 바이트로 제 자리에 들어가고 임시 디스크는 빈다

★ 왜 서버가 둘인가
  다른 출처 그림은 출처(포트)가 달라야 생긴다. 둘째 서버는 그림만 주고 CORS 머리를 붙이지 않는다 — 앱이 캡처 동안
  응답에 ACAO 를 붙여 주는지(그림 · 글꼴 · CSS · 미디어만)를 본다. 그 쪽(xorigin.html)은 connect-src 'self' 의 CSP 도 받는다.
★ 왜 그림 여섯 장을 3000px 씩 띄우나
  최소화된 창은 그리는 간격이 길다. 0.3초씩만 머물면 여섯 장 중 둘셋만 들어갔다 — 한 화면씩 내리며 그리기를
  기다리는지(requestAnimationFrame 두 번)를 이 쪽이 가린다.
"""
from __future__ import annotations

import glob
import hashlib
import json
import os
import re
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _common as C  # noqa: E402
import fixtures  # noqa: E402

XORIGIN_CSP = "default-src 'self'; img-src *; connect-src 'self'; script-src 'unsafe-inline'"
PAGES = ['index.html', 'gallery.html', 'io.html', 'io2.html', 'lazydiv.html', 'xorigin.html', 'csp.html', 'endless.html']
# '바닥 확인' 단계(r56) — 멈춘 쪽은 높이가 네 번 그대로면 멈춘다(약 2.5초). 예전엔 늘 15초였다
BOTTOM_STATIC_MAX = 6.0


def main() -> int:
    check = C.Checks()
    shutil.rmtree(C.work_dir('singlefile_captures'), ignore_errors=True)   # 지난 판의 결과가 남아 '이번 결과' 로 읽히지 않게
    user_chrome = C.listen_pids(C.USER_CAPTURE_PORT)
    print('[사용자 앱 Chrome %d] %s' % (C.USER_CAPTURE_PORT, user_chrome or '없음'))
    idle = C.user_idle_seconds()
    if idle > 300:
        # 화면이 잠들면 macOS 가 그리기를 멈춰, 최소화된 캡처 Chrome 의 IntersectionObserver 가 한 번도 판정하지 않는다
        # (2026-10-06 실측: 옛 판 1.22.98 · 새 판 1.28.1 모두 io.html 0/6). r50 부터 앱이 캡처 직전에 지연 그림을 직접 불러온다
        # (__hkFireIO · loading=eager · data-src) — 화면이 꺼져 있어도 io.html 이 담겨야 한다. 비면 그 수리가 듣지 않는 것이다.
        print('   알림: 사람 입력 없이 %d분 — 화면이 꺼져 있을 수 있다(r50 부터는 그래도 지연 그림이 담겨야 한다)' % (idle // 60))
    site = fixtures.build_site(C.work_dir('testsite_sf'))
    sb, b = fixtures.serve(site)                                   # 다른 출처 — 그림만
    (site / 'io.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>스크롤해야 뜨는 그림</title><h1>IO</h1>' +
        ''.join('<div style="height:3000px"></div><img class="io" data-src="/img/%s.png?io=%d" width="60" height="60" alt="io%d">'
                % (c, i, i) for i, c in enumerate(['red', 'blue', 'green', 'lazy1', 'lazy2', 'bg'])) +
        '<script>var o=new IntersectionObserver(function(es){es.forEach(function(e){if(e.isIntersecting){e.target.src=e.target.dataset.src;o.unobserve(e.target);}})});'
        'document.querySelectorAll("img.io").forEach(function(i){o.observe(i)});</script>', encoding='utf-8')
    # 그리지 않아도 뜨는 그림 — 관찰자의 root 가 크기 0 이라 스크롤해도 '보인다' 가 오지 않는다(화면이 꺼진 맥과 같은 처지).
    #   그림 주소는 data-real 에 둔다(앱이 옮겨 주는 data-src 가 아니다) — 앱이 캡처 직전에 아직 못 들은 그림 대상에
    #   '보인다' 를 직접 알려야만(__hkFireIO) 담긴다. 옛 판은 이 쪽의 그림이 빈다.
    (site / 'io2.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>그리지 않아도 뜨는 그림</title><h1>IO2</h1>'
        '<div id="r" style="width:0;height:0;overflow:hidden"></div>' +
        ''.join('<img class="io2" data-real="/img/%s.png?io2=%d" width="60" height="60" alt="io2-%d">' % (c, i, i)
                for i, c in enumerate(['red', 'blue', 'green'])) +
        '<script>var o=new IntersectionObserver(function(es){es.forEach(function(e){if(e.isIntersecting){'
        'e.target.src=e.target.getAttribute("data-real");o.unobserve(e.target);}})},{root:document.getElementById("r")});'
        'document.querySelectorAll("img.io2").forEach(function(i){o.observe(i)});</script>', encoding='utf-8')
    # 보일 때 붙이는 그림(X 와 같은 꼴) — 그림 자리가 <img> 가 아닌 빈 <div> 이고, 관찰자가 '보인다' 를 들어야 그 안에 그림을 붙인다:
    #   프로필 칸 하나 + 가로로 넘기는 칸 넷(여러 장 그림). 넘겨야 보이는 칸(3 · 4)은 화면이 켜져 있어도 원래는 오지 않는다 — 앱이
    #   캡처하며 넘김 칸을 '보인다' 로 알려야(__hkTickIO carousel) 다섯 장이 다 담긴다. 그리지 않는 창에서는 스크롤마다 자로 재어 알린다.
    #   (사용자 보고 2026-11-15: X 의 4장 넘김 글 캡처에 그림이 한 장도 없었다)
    (site / 'lazydiv.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>넘김 칸</title><style>.car{display:flex;overflow-x:auto;width:320px}'
        '.slot{flex:0 0 320px;aspect-ratio:0.79/1;background:#eee}.av{width:48px;height:48px;background:#ddd}</style>'
        '<h1>LAZYDIV</h1><div class="av" id="av"></div><div class="car">' +
        ''.join('<div class="slot" data-n="%d"></div>' % n for n in range(1, 5)) + '</div><div style="height:2400px"></div>'
        '<script>var c=["red","blue","green","lazy1","lazy2"];var io=new IntersectionObserver(function(es){es.forEach(function(e){'
        'if(!e.isIntersecting||e.target.firstChild)return;var n=e.target.id==="av"?0:+e.target.dataset.n;var im=document.createElement("img");'
        'im.width=60;im.height=60;im.src="/img/"+c[n]+".png?lazydiv="+n;e.target.appendChild(im);io.unobserve(e.target);});});'
        'io.observe(document.getElementById("av"));document.querySelectorAll(".slot").forEach(function(s){io.observe(s)});</script>',
        encoding='utf-8')
    # 끝없이 줄이 붙는 쪽(답글이 계속 붙는 X 글과 같은 처지) — 높이가 멈추지 않아도 시간 안에 카드가 아닌 캡처로 저장돼야 한다
    (site / 'endless.html').write_text(
        '<!doctype html><meta charset="utf-8"><title>끝없는 목록</title><h1>ENDLESS</h1><img src="/img/red.png" width="60">'
        '<div id="l"></div><script>var n=0;setInterval(function(){if(n>=2000)return;var d=document.createElement("div");'
        'd.style.height="40px";d.textContent="row "+(++n);document.getElementById("l").appendChild(d);},250);</script>',
        encoding='utf-8')
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
                txt = p.eval("(document.getElementById('crawl-log')||{}).innerText||''", retry=True) or ''
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
            # ★ r56 — 단계별 시간: 캡처 로그는 트위터 탭 로그로 간다
            tw = p.eval("(document.getElementById('twitter-log')||{}).innerText||''", retry=True) or ''
            bottoms = [float(x) for x in re.findall(r'바닥 확인 ([0-9.]+)s\(\d+번\)', tw)]
            print('   바닥 확인 단계(초):', bottoms)
            fast = [x for x in bottoms if x < BOTTOM_STATIC_MAX]
            check('멈춘 쪽은 바닥 확인에 %.0f초 넘게 쓰지 않는다(예전 15초)' % BOTTOM_STATIC_MAX, len(fast) >= len(PAGES) - 1,
                  bottoms)
            check('저장마다 ✅ 캡쳐 완료 한 줄(%d)' % len(PAGES), tw.count('✅ 캡쳐 완료') == len(PAGES), tw.count('✅ 캡쳐 완료'))
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
            if 'LAZYDIV' in h:
                check('보일 때 붙이는 그림(X 꼴 — 프로필 칸 + 넘김 칸 넷) — 다섯 장 모두 담겼다', datas >= 5 and imgs == 5,
                      '그림 %d · data: %d' % (imgs, datas))
            if 'ENDLESS' in h:
                check('끝없이 줄이 붙는 쪽 — 카드가 아닌 SingleFile 캡처로 저장', sf and not err)
            if 'IO2' in h:
                check('그리지 않아도 뜨는 그림(io2) — 3장 모두 담겼다(화면이 꺼진 맥의 밤 캡처와 같은 처지)', datas >= 3 and imgs == 3,
                      '그림 %d · data: %d' % (imgs, datas))
        C.keep_log('singlefile_app.log')
        stage_phase(check, a)
    except Exception as e:  # noqa: BLE001
        check('시험 진행 중 오류 없음', False, '%s: %s' % (type(e).__name__, e))
    finally:
        C.keep_log('singlefile_stage_app.log')
        C.iso_stop()
        sa.shutdown()
        sb.shutdown()
    check('사용자 앱의 Chrome(%d)은 그대로' % C.USER_CAPTURE_PORT, C.listen_pids(C.USER_CAPTURE_PORT) == user_chrome,
          C.listen_pids(C.USER_CAPTURE_PORT))
    return check.summary()


def _stage_dirs() -> list:
    """사본의 임시 디스크 안 .abiwa_cap — 설정의 tempDir(iso.py 가 tmp/work 로 심는다)과 앱 자료 폴더의 temp 둘 다 본다."""
    out = [C.ISO / 'tmp' / 'work' / '.abiwa_cap']
    home = C.iso_state().get('home')
    if home:
        out += list(Path(home).glob('Library/Application Support/*/temp/.abiwa_cap'))
    return [d for d in out if d.is_dir()]


def _staged(name: str) -> list:
    return sorted(f for d in _stage_dirs() for f in d.iterdir() if f.name.endswith('_' + name))


def stage_phase(check, a: int) -> None:
    """r56 — 저장 폴더로 못 옮긴 진짜 캡처는 '✅' 를 말하지 않고 임시 디스크에 옆 기록과 남는다 → 다시 켜면 제 자리로."""
    C.iso_stop()
    if not check('사본을 HANISHIKI_CAP_STAGE_ALL=1 로 다시 띄움', C.iso_start('--env', 'HANISHIKI_CAP_STAGE_ALL=1')):
        return
    out = C.ISO / 'tmp' / 'sf-stage'
    shutil.rmtree(out, ignore_errors=True)
    out.mkdir(parents=True, exist_ok=True)
    urls = ['http://127.0.0.1:%d/%s' % (a, pg) for pg in ('index.html', 'gallery.html')]
    names = ['page_%03d_%s.html' % (i + 1, hashlib.md5(u.encode()).hexdigest()[:8]) for i, u in enumerate(urls)]
    capdir = None
    try:
        with C.page() as p:
            time.sleep(1.5)
            p.eval("""(function(){var g=function(i){return document.getElementById(i)}; switchTab('crawl');
              g('crawl-url').value=%s; g('crawl-path').value=%s; g('crawl-real-chrome').checked=true;
              var w=g('crawl-wait-login'); if(w) w.checked=false; var lc=g('crawl-login-check'); if(lc) lc.value='';
              startCrawl();})()""" % (json.dumps(' '.join(urls)), json.dumps(str(out))))
            t0 = time.time()
            txt = tw = ''
            unlocked = False
            while time.time() - t0 < 300:
                if capdir is None:
                    found = glob.glob(str(out) + '/crawl_*/captures')
                    if found:
                        capdir = Path(found[0])
                        os.chmod(capdir, 0o555)   # 첫 쪽이 다 받기 전에 — 옮기기가 실패하게
                tw = p.eval("(document.getElementById('twitter-log')||{}).innerText||''", retry=True) or ''
                if capdir is not None and not unlocked and '임시 디스크에 두었습니다' in tw:
                    os.chmod(capdir, 0o755)       # 둘째 쪽은 바로 들어가게
                    unlocked = True
                txt = p.eval("(document.getElementById('crawl-log')||{}).innerText||''", retry=True) or ''
                if '크롤 완료' in txt or '중단' in txt:
                    break
                time.sleep(0.2)
        check('캡처 폴더를 읽기 전용으로 둠', capdir is not None)
        if capdir is None:
            return
        os.chmod(capdir, 0o755)
        check('옮기지 못한 쪽 — 임시 디스크에 두었다고 알림', unlocked, tw[-300:].replace('\n', ' / '))
        check('옮기지 못한 쪽 — \'✅ 캡쳐 완료\' 라고 말하지 않음', ('✅ 캡쳐 완료: ' + names[0]) not in tw)
        check('옮긴 쪽(임시 디스크를 거쳐) — ✅ 캡쳐 완료 · 제 자리에 있음',
              ('✅ 캡쳐 완료: ' + names[1]) in tw and (capdir / names[1]).is_file())
        m = re.search(r'크롤 완료: (\d+)/(\d+)', txt)
        check('저장으로 센 것은 옮긴 쪽 하나', bool(m) and m.group(1) == '1', m.groups() if m else txt[-200:])
        staged = [f for f in _staged(names[0]) if not f.name.endswith('.dest.json') and not f.name.endswith('.part')]
        check('임시 디스크에 진짜 캡처 하나', len(staged) == 1, [f.name for f in _staged(names[0])])
        if len(staged) != 1:
            return
        body = staged[0].read_bytes()
        side = Path(str(staged[0]) + '.dest.json')
        rec = json.loads(side.read_text('utf-8')) if side.exists() else {}
        check('옆 기록(.dest.json) — 저장할 자리 · 주소 · 바이트', rec.get('dest') == str(capdir / names[0])
              and rec.get('url') == urls[0] and rec.get('bytes') == len(body), rec)
        check('임시 디스크의 것은 SingleFile 결과', b'Page saved with SingleFile' in body[:65536])
        dest = capdir / names[0]
        check('제 자리에 SingleFile 이 아닌 것만(카드 · 없음)', not dest.exists() or b'Page saved with SingleFile' not in dest.read_bytes()[:65536])
        # 다시 켜면(디스크 확인 뒤) 옮긴다
        C.iso_stop(wipe=False)
        if not check('사본 다시 켜기(같은 홈)', C.iso_start('--env', 'HANISHIKI_CAP_STAGE_ALL=1', wipe_first=False)):
            return
        t0 = time.time()
        while time.time() - t0 < 90 and not (dest.is_file() and dest.read_bytes() == body):
            time.sleep(1)
        check('다시 켜면 진짜 캡처가 같은 바이트로 제 자리에', dest.is_file() and dest.read_bytes() == body,
              '%.0fs' % (time.time() - t0))
        left = [f.name for d in _stage_dirs() for f in d.iterdir()]
        check('임시 디스크(.abiwa_cap)가 비었다', not left, left)
    finally:
        if capdir is not None and capdir.exists():
            os.chmod(capdir, 0o755)


if __name__ == '__main__':
    sys.exit(main())
