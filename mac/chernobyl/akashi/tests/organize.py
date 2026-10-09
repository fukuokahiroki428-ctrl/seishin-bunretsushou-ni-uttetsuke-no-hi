#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""정리하기 시험 — 격리 사본의 '정리하기' 탭을 실제로 눌러, 가짜로 받은 파일을 정리 기준대로 모으는지 본다.

    python3 akashi/tests/organize.py       # 혼자(약 2분)
    python3 akashi/tests/run_all.py        # 다른 기능 시험과 함께

사용자 기준(2026-10-09): 정리할 곳/gaekkin/<이름>・@<핸들>-(X ／ twitter.com)/令和7年11月/ 에 중복 없이, 날짜는 올린 날짜,
빗금은 전각 ／, 원본은 남긴다.

살피는 것:
  1 탭이 열리고 규칙이 설정 파일에 남는다(organizeRules)
  2 '모두 정리' — 계정 폴더 이름(프로필 폴더의 표시 이름 · 전각 ／), 연호 경계(平成31年4月 · 令和元年5月), 날짜 없는 파일은
    파일 시각의 달, 같은 내용(media/ 와 _complete/)은 한 번, 이름만 같은 다른 내용은 '(2)', 정리 대상 아닌 것(.txt · .part)은 빼기
  3 원본이 바이트 · 시각까지 그대로
  4 다시 정리하면 새로 두는 것 0
  5 수집이 끝났다는 신호(onCollectionEnded)에 '받은 뒤 자동 정리' 가 새 파일만 더한다
  6 화면 JS 오류 없음
모든 자료는 지어낸 것이다.
"""
from __future__ import annotations

import hashlib
import json
import os
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _common as C  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from lib import paths  # noqa: E402

ACC = '공작함 아카시・@akashi_demo-(X ／ twitter.com)'


def make_downloads(root: Path) -> Path:
    """앱의 트위터 저장 모양을 흉내 낸 가짜 받은 폴더."""
    u = root / 'twitter' / 'akashi_demo'
    for d in ('media/_complete', 'media/sub', 'profiles/target/공작함 아카시(@akashi_demo)', 'profiles/followers/Stranger(@someone)', 'captures'):
        (u / d).mkdir(parents=True, exist_ok=True)
    files = {
        'media/20251103_142000_111.jpg': b'A' * 100,
        'media/_complete/20251103_142000_111.jpg': b'A' * 100,          # 같은 내용 — 한 번만
        'media/20190430_235900_222.png': b'B' * 50,                     # 平成31年4月
        'media/20190501_000100_333.mp4': b'C' * 70,                     # 令和元年5月
        'media/nodate.jpg': b'D' * 30,                                  # 파일 시각 → 令和6年7月
        'media/sub/20251110_000000_dup.jpg': b'E' * 10,
        'media/_complete/20251110_000000_dup.jpg': b'F' * 10,           # 이름만 같다 — (2)
        'media/note.txt': b'not media',
        'media/20251101_000000_x.mp4.part': b'partial',
        'captures/20251103_142000_111.html': b'<html></html>',           # 캡처는 기본으로 빼기
        'profiles/followers/Stranger(@someone)/profile_20261003.jpg': b'Q' * 9,   # 남의 프로필 — 빼기
    }
    for rel, data in files.items():
        (u / rel).write_bytes(data)
    os.utime(u / 'media/nodate.jpg', (time.mktime((2024, 7, 15, 1, 1, 0, 0, 0, -1)),) * 2)
    return u


def snapshot(root: Path) -> dict:
    out = {}
    for p in sorted(root.rglob('*')):
        if p.is_file():
            st = p.stat()
            out[str(p.relative_to(root))] = (hashlib.sha256(p.read_bytes()).hexdigest(), st.st_mtime_ns)
    return out


def tree(dest: Path) -> list:
    return sorted(str(p.relative_to(dest)) for p in dest.rglob('*') if p.is_file() and not p.name.startswith('.'))


def wait_done(p, secs=60):
    t0 = time.time()
    while time.time() - t0 < secs:
        meta = p.eval("(document.getElementById('organize-meta')||{}).textContent||''") or ''
        busy = p.eval("!!document.getElementById('organize-start-btn').disabled")
        if not busy and meta in ('끝', '멈춤', '실패'):
            return meta, time.time() - t0
        time.sleep(0.4)
    return None, None


def main() -> int:
    check = C.Checks()
    try:
        if not C.iso_start():
            return 2
        tmp = C.ISO / 'tmp'
        dl = tmp / 'organize-dl'
        dest = tmp / 'organize-out' / 'gaekkin'
        dest.parent.mkdir(parents=True, exist_ok=True)   # 최상위 폴더(gaekkin)는 정리가 만든다 — 바로 위가 있으니
        src = make_downloads(dl)
        before = snapshot(dl)
        with C.page() as p:
            time.sleep(1.5)
            p.eval("switchTab('organize')")
            time.sleep(0.6)
            shown = p.eval("(function(){var t=document.getElementById('tab-organize');return !!t && t.offsetParent!==null})()")
            check('정리하기 탭이 열린다', shown is True, shown)
            # 트위터 탭의 저장 경로 = 가짜 받은 곳의 뿌리, 규칙 하나(받은 곳은 비움 → 자동으로 찾기)
            p.eval("document.getElementById('twitter-path').value = %s" % json.dumps(str(dl)))
            p.eval("organizeRestore([{platform:'twitter', target:'@akashi_demo', name:'', source:'', dest:%s}]); organizeSave();"
                   % json.dumps(str(dest)))
            time.sleep(1.0)
            cfg = json.loads((paths.data_dir(C.ISO / 'home') / paths.CONFIG_NAME).read_text(encoding='utf-8'))
            rules = cfg.get('organizeRules') or []
            check('규칙이 설정 파일에 남는다', len(rules) == 1 and rules[0].get('dest') == str(dest), rules)
            prev = p.eval("(document.querySelector('.org-preview')||{}).textContent||''") or ''
            check('미리보기 — 이름 칸이 비면 자리만(‹자동 이름›), 나머지는 실제 모양', '‹자동 이름›・@akashi_demo-(X ／ twitter.com)/' in prev and '年' in prev, prev)

            # 1) 모두 정리
            p.eval('organizeRunAll()')
            meta, took = wait_done(p)
            check('정리가 끝났다', meta == '끝', '%s %.1fs' % (meta, took or -1))
            got = tree(dest)
            want = sorted([
                ACC + '/令和7年11月/20251103_142000_111.jpg',
                ACC + '/平成31年4月/20190430_235900_222.png',
                ACC + '/令和元年5月/20190501_000100_333.mp4',
                ACC + '/令和6年7月/nodate.jpg',
                ACC + '/令和7年11月/20251110_000000_dup.jpg',
                ACC + '/令和7年11月/20251110_000000_dup (2).jpg',
            ])
            check('정리 결과가 기준대로(이름 · 연호 · 중복 · 같은 이름 다른 내용 · 빼기)', got == want,
                  '\n      더 있음: %s\n      없음: %s' % (sorted(set(got) - set(want)), sorted(set(want) - set(got))))
            media = {h for rel, (h, _) in before.items() if '/media/' in '/' + rel and rel.split('.')[-1] in ('jpg', 'png', 'mp4')}
            organized = {hashlib.sha256((dest / g).read_bytes()).hexdigest() for g in got}
            check('정리된 파일 내용 = 원본 미디어(서로 다른 내용마다 하나씩)', organized == media,
                  '%d vs %d' % (len(organized), len(media)))
            check('원본이 바이트 · 시각까지 그대로', snapshot(dl) == before)
            kv = p.eval("['copied','dup','errors'].map(function(k){return document.getElementById('organize-kv-'+k).textContent})")
            prev2 = p.eval("(document.querySelector('.org-preview')||{}).textContent||''") or ''
            check('정리한 뒤 미리보기는 찾은 이름으로', ACC in prev2, prev2)
            check('요약 — 새로 6 · 중복 1 · 문제 0', kv == ['6', '1', '0'], kv)

            # 2) 다시 정리 — 새로 0
            time.sleep(0.5)
            p.eval('organizeRunAll()')
            meta, _ = wait_done(p)
            kv2 = p.eval("['copied','dup'].map(function(k){return document.getElementById('organize-kv-'+k).textContent})")
            check('다시 정리하면 새로 두는 것 0', meta == '끝' and kv2 and kv2[0] == '0', kv2)

            # 3) 수집이 끝났다는 신호 → 받은 뒤 자동 정리(새 파일만)
            (src / 'media' / '20251225_090000_444.jpg').write_bytes(b'G' * 20)
            p.eval("document.getElementById('organize-auto').checked = true; window.onCollectionEnded('twitter')")
            time.sleep(0.8)
            meta, _ = wait_done(p)
            check("수집이 끝나면 자동 정리 — 새 파일만", (dest / ACC / '令和7年12月' / '20251225_090000_444.jpg').exists()
                  and len(tree(dest)) == 7, tree(dest)[-2:])

            # 4) 앱이 받은 파일의 EXIF 를 다시 쓴 흉내(같은 크기 · 시각 되돌림) — 같은 사진을 '(2)' 로 또 두지 않는다
            f = src / 'media' / '20251103_142000_111.jpg'
            st = f.stat()
            f.write_bytes(b'Z' * 100)
            os.utime(f, ns=(st.st_atime_ns, st.st_mtime_ns))
            time.sleep(0.3)
            p.eval('organizeRunAll()')
            meta, _ = wait_done(p)
            check('원본을 다시 써도 같은 사진을 또 두지 않는다', meta == '끝' and len(tree(dest)) == 7, tree(dest))
            errs = C.page_errors(p)
            check('화면 JS 오류 없음', not errs, errs[:2])
    except Exception as e:  # noqa: BLE001 — 시험 도중의 어떤 실패도 판정 한 줄로
        check('시험 진행 중 오류 없음', False, '%s: %s · 앱 살아 있음=%s' % (type(e).__name__, e, C.iso_alive()))
    finally:
        C.keep_log('organize_app.log')
        C.iso_stop()
    return check.summary()


if __name__ == '__main__':
    sys.exit(main())
