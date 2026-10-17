#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""정리하기 시험 — 격리 사본의 '정리하기' 탭을 실제로 눌러, 가짜로 받은 파일을 정리 기준대로 모으는지 본다.

    python3 akashi/tests/organize.py       # 혼자(약 2분)
    python3 akashi/tests/run_all.py        # 다른 기능 시험과 함께

사용자 기준(2026-10-09): 정리할 곳/gaekkin/<이름>・@<핸들>-(X ／ twitter.com)/令和7年11月/ 에 중복 없이, 날짜는 올린 날짜,
빗금은 전각 ／, 원본은 남긴다.

사용자(2026-10-15): 대상이 많으면 규칙을 하나씩 만드는 것이 불편 — 정리할 곳 하나 + 틀 하나로, 대상 목록의 계정 전부를.

살피는 것:
  1 r42 규칙을 틀로 옮김 — 가장 많이 쓴 정리할 곳이 공통, 별명이 든 표시 이름은 '자동', 대상 목록에 없는 규칙은 '따로 추가한 규칙'
  2 '모두 정리' — 계정 폴더 이름(프로필 폴더의 표시 이름 · 전각 ／), 연호 경계(平成31年4月 · 令和元年5月), 날짜 없는 파일은
    파일 시각의 달, 같은 내용(media/ 와 _complete/)은 한 번, 이름만 같은 다른 내용은 '(2)', 정리 대상 아닌 것(.txt · .part)은 빼기,
    아직 받지 않은 대상은 문제가 아니라 '받은 것 없음'
  3 원본이 바이트 · 시각까지 그대로 · 다시 정리하면 새로 두는 것 0
  4 수집이 끝나면 방금 받은 계정만 자동 정리
  5 계정 이름이 바뀌면 폴더 이름만 바뀐다
  6 틀을 바꾸면 새 모양으로 · 맞지 않는 틀은 돌지 않는다
  7 계정 표 — 검색으로 좁혀 한꺼번에 빼기 · 한 계정만 따로 이름
  8 화면 JS 오류 없음
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
    other = root / 'elsewhere' / 'akashi_m'          # 다른 앱으로 받은 폴더 — 따로 추가한 규칙
    other.mkdir(parents=True)
    (other / '20251105_000000_m.jpg').write_bytes(b'M' * 12)
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


def cfg_now() -> dict:
    return json.loads((paths.data_dir(C.ISO / 'home') / paths.CONFIG_NAME).read_text(encoding='utf-8'))


def wait_done(p, secs=60):
    t0 = time.time()
    while time.time() - t0 < secs:
        meta = p.eval("(document.getElementById('organize-meta')||{}).textContent||''", retry=True) or ''
        busy = p.eval("!!document.getElementById('organize-start-btn').disabled", retry=True)
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
            # 트위터 탭의 저장 경로 = 가짜 받은 곳의 뿌리. 대상 목록: 받은 계정 하나 + 아직 안 받은 계정 하나
            p.eval("document.getElementById('twitter-path').value = %s" % json.dumps(str(dl)))
            p.eval("platformTargets.twitter = [{target:'akashi_demo', nickname:'별명', enabled:true}, {target:'never_dl', enabled:true}];"
                   "window.confirm = function(){ return true };")
            # r42 의 설정 — 대상마다 규칙(불러오기가 별명을 표시 이름에 넣었다) + 대상 목록에 없는 규칙 하나. 틀로 옮긴다
            p.eval("organizeRestoreAll({organizeRules: [{platform:'twitter', target:'@akashi_demo', name:'별명', source:'', dest:%s},"
                   " {platform:'misskey', target:'akashi_m', name:'', source:%s, dest:%s}]}); organizeSave();"
                   % (json.dumps(str(dest)), json.dumps(str(dl / 'elsewhere' / 'akashi_m')), json.dumps(str(dest))))
            time.sleep(1.0)
            cfg = cfg_now()
            org = cfg.get('organize') or {}
            check('r42 규칙을 틀로 — 공통 정리할 곳 · 옮김 표시 · 옮기기 전 규칙 보관',
                  org.get('root') == str(dest) and org.get('migrated') is True and len(org.get('before') or []) == 2, org)
            check("별명이 든 표시 이름은 '자동'(따로 정함 없음) · 대상 목록에 없는 규칙만 남음",
                  not (org.get('overrides') or {}) and [r.get('target') for r in cfg.get('organizeRules') or []] == ['akashi_m'],
                  (org.get('overrides'), cfg.get('organizeRules')))
            rows = p.eval("Array.from(document.querySelectorAll('#org-accounts .org-acc')).map(e => e.dataset.key)")
            check('계정 표 = 대상 목록 전부', rows == ['twitter:akashi_demo', 'twitter:never_dl'], rows)
            prev = p.eval("document.getElementById('org-tpl-preview').textContent") or ''
            check('미리보기 — 이름 칸이 비면 자리만(‹자동 이름›), 나머지는 실제 모양',
                  '‹자동 이름›・@akashi_demo-(X ／ twitter.com)/' in prev and '年' in prev, prev)
            wrapped = p.eval("(function(){var s=document.querySelector('.org-rule select');return !!(s && s.closest('.select-wrap'))})()")
            check('따로 추가한 규칙의 플랫폼 선택 상자가 눌린다(감싸짐)', wrapped is True, wrapped)

            # 1) 모두 정리
            p.eval('organizeRunAll()')
            meta, took = wait_done(p)
            check('정리가 끝났다', meta == '끝', '%s %.1fs' % (meta, took or -1))
            got = tree(dest)
            MIS = 'akashi_m・@akashi_m-(Misskey ／ misskey.io)'
            want = sorted([
                ACC + '/令和7年11月/20251103_142000_111.jpg',
                ACC + '/平成31年4月/20190430_235900_222.png',
                ACC + '/令和元年5月/20190501_000100_333.mp4',
                ACC + '/令和6年7月/nodate.jpg',
                ACC + '/令和7年11月/20251110_000000_dup.jpg',
                ACC + '/令和7年11月/20251110_000000_dup (2).jpg',
                MIS + '/令和7年11月/20251105_000000_m.jpg',
            ])
            check('정리 결과가 기준대로(이름 · 연호 · 중복 · 같은 이름 다른 내용 · 빼기 · 따로 추가한 규칙)', got == want,
                  '\n      더 있음: %s\n      없음: %s' % (sorted(set(got) - set(want)), sorted(set(want) - set(got))))
            media = {h for rel, (h, _) in before.items() if '/media/' in '/' + rel and rel.split('.')[-1] in ('jpg', 'png', 'mp4')}
            media |= {hashlib.sha256(b'M' * 12).hexdigest()}
            organized = {hashlib.sha256((dest / g).read_bytes()).hexdigest() for g in got}
            check('정리된 파일 내용 = 원본 미디어(서로 다른 내용마다 하나씩)', organized == media,
                  '%d vs %d' % (len(organized), len(media)))
            check('원본이 바이트 · 시각까지 그대로', snapshot(dl) == before)
            kv = p.eval("['copied','dup','missing','errors'].map(function(k){return document.getElementById('organize-kv-'+k).textContent})")
            check('요약 — 새로 7 · 중복 1 · 받은 것 없음 1(아직 안 받은 대상) · 문제 0', kv == ['7', '1', '1', '0'], kv)
            nm = p.eval("(document.querySelector('.org-acc[data-key=\"twitter:akashi_demo\"] .org-acc-name')||{}).textContent||''")
            seen = ((cfg_now().get('organize') or {}).get('seen') or {})
            check('정리한 뒤 표에 찾은 이름 · 설정에 마지막 결과', nm == '공작함 아카시' and (seen.get('twitter:akashi_demo') or {}).get('copied') == 6
                  and (seen.get('twitter:never_dl') or {}).get('missing') == 1, (nm, seen))

            # 2) 다시 정리 — 새로 0
            time.sleep(0.5)
            p.eval('organizeRunAll()')
            meta, _ = wait_done(p)
            kv2 = p.eval("['copied','dup'].map(function(k){return document.getElementById('organize-kv-'+k).textContent})")
            check('다시 정리하면 새로 두는 것 0', meta == '끝' and kv2 and kv2[0] == '0', kv2)

            # 3) akashi_demo 하나를 받았다 — 진짜 수집 시작(backend.startCollection, 계정이 없어 곧 끝남)이 그 계정을 적고,
            #    수집이 끝났다는 신호에 받은 뒤 자동 정리는 그 계정만
            (src / 'media' / '20251225_090000_444.jpg').write_bytes(b'G' * 20)
            check('수집 시작에 고리가 걸렸다(어느 계정을 받는지 적음)', p.eval("backend._orgHooked === true") is True)
            p.eval("document.getElementById('organize-auto').checked = true; document.getElementById('organize-meta').textContent = '기다림';"
                   "backend.startCollection(JSON.stringify({platform:'twitter', target:'akashi_demo', accounts:[]}))")
            time.sleep(1.0)
            meta, _ = wait_done(p, 90)
            ran = p.eval("_orgRunIds.slice()")
            check("수집이 끝나면 방금 받은 계정만 자동 정리 — 새 파일만", ran == ['t:twitter:akashi_demo']
                  and (dest / ACC / '令和7年12月' / '20251225_090000_444.jpg').exists() and len(tree(dest)) == 8, (ran, tree(dest)[-2:]))

            # 4) 앱이 받은 파일의 EXIF 를 다시 쓴 흉내(같은 크기 · 시각 되돌림) — 같은 사진을 '(2)' 로 또 두지 않는다
            f = src / 'media' / '20251103_142000_111.jpg'
            st = f.stat()
            f.write_bytes(b'Z' * 100)
            os.utime(f, ns=(st.st_atime_ns, st.st_mtime_ns))
            time.sleep(0.3)
            p.eval('organizeRunAll()')
            meta, _ = wait_done(p)
            check('원본을 다시 써도 같은 사진을 또 두지 않는다', meta == '끝' and len(tree(dest)) == 8, tree(dest))

            # 5) 계정이 이름을 바꿨다 — 다음 수집이 profiles/target/<새 이름>(@핸들)/ 을 남긴다. 계정 폴더는 이름만 바뀌고 다시 복사하지 않는다
            oldp = src / 'profiles' / 'target' / '공작함 아카시(@akashi_demo)'
            os.utime(oldp, (time.mktime((2026, 10, 3, 9, 0, 0, 0, 0, -1)),) * 2)   # 실제처럼 예전 수집의 시각
            newp = src / 'profiles' / 'target' / '새 이름(@akashi_demo)'
            newp.mkdir(parents=True)
            (newp / 'profile_20261015.jpg').write_bytes(b'N' * 11)
            time.sleep(0.3)
            p.eval('organizeRunAll()')
            meta, _ = wait_done(p)
            NEW = '새 이름・@akashi_demo-(X ／ twitter.com)'
            names = sorted(x.name for x in dest.iterdir() if not x.name.startswith('.'))
            check('이름이 바뀌면 계정 폴더 이름만 바뀐다(다시 복사 없음)', names == sorted([NEW, MIS]) and len(tree(dest)) == 9
                  and (dest / NEW / '令和8年10月' / 'profile_20261015.jpg').exists(), '%s · %d개' % (names, len(tree(dest))))

            # 6) 틀 — 맞지 않는 틀은 미리보기가 알리고 돌지 않는다 · 다른 틀은 그 모양으로
            p.eval("organizeSetTemplate('{year}/{name}')")
            bad = p.eval("document.getElementById('org-tpl-preview').className")
            started = p.eval("organizeRunAll(); _orgRunning")
            check('맞지 않는 틀(계정 폴더 없음)은 미리보기에 ✕ · 정리가 돌지 않는다', 'org-bad' in (bad or '') and started is False, (bad, started))
            p.eval("organizeSetTemplate('{platform}/{handle}/{year}-{mm}'); organizeRunAcc(0)")
            meta, _ = wait_done(p)
            alt = dest / 'twitter' / 'akashi_demo' / '2025-11' / '20251103_142000_111.jpg'
            tpl_saved = (cfg_now().get('organize') or {}).get('template')
            check('틀을 바꾸면 그 모양으로(그 계정만 정리 · 설정에 남음)', meta == '끝' and alt.exists()
                  and tpl_saved == '{platform}/{handle}/{year}-{mm}', (meta, tpl_saved))
            p.eval("organizeSetTemplate('')")
            check("'기본값' 은 틀을 비운다(기본 틀)", (cfg_now().get('organize') or {}).get('template') == '')

            # 7) 계정 표 — 검색으로 좁혀 '빼기', 한 계정만 따로 이름
            p.eval("document.getElementById('org-acc-q').value='never'; organizeAccSearch('never')")
            time.sleep(0.4)
            vis = p.eval("_orgView.map(a => a.key)")
            p.eval("organizeBulk('ex'); organizeAccSearch('')")
            time.sleep(0.4)
            ov = (cfg_now().get('organize') or {}).get('overrides') or {}
            rules_now = p.eval("_orgRulesFor().map(r => r.id)")
            check('검색으로 좁혀 보이는 것만 빼기 — 정리할 목록에서 빠짐', vis == ['twitter:never_dl'] and (ov.get('twitter:never_dl') or {}).get('exclude') is True
                  and 't:twitter:never_dl' not in (rules_now or []), (vis, ov, rules_now))
            p.eval("organizeAccToggle(0); organizeSetOv('name', '손 이름')")
            time.sleep(0.4)
            pv = p.eval("(document.querySelector('.org-acc-more .org-preview')||{}).textContent||''") or ''
            ov = (cfg_now().get('organize') or {}).get('overrides') or {}
            check("한 계정만 따로 이름 — 펼친 칸의 미리보기 · 설정 · '따로' 표시", '손 이름・@akashi_demo-(X ／ twitter.com)/' in pv
                  and (ov.get('twitter:akashi_demo') or {}).get('name') == '손 이름'
                  and p.eval("!!document.querySelector('.org-acc[data-key=\"twitter:akashi_demo\"] .org-badge')"), (pv, ov))
            p.eval("organizeClearOv()")
            time.sleep(0.3)
            check('따로 정한 것 지우기', 'twitter:akashi_demo' not in ((cfg_now().get('organize') or {}).get('overrides') or {}))
            # 8) 옮기기의 모서리 — 한 계정에 규칙 둘(둘째는 따로 남김) · 공통 정리할 곳은 대상 목록의 계정 규칙에서만 셈
            mig = p.eval("""(function(){
                const T = platformTargets.twitter, R = organizeRules, K = organizeConf;
                platformTargets.twitter = [{target:'a'}, {target:'b'}];
                organizeConf = _orgBlank();
                organizeRules = [{id:'1', platform:'twitter', target:'a', dest:'/P'}, {id:'2', platform:'twitter', target:'@A', source:'/gdl/a', dest:'/P'},
                    {id:'3', platform:'twitter', target:'https://x.com/b', dest:'/B'}, {id:'4', platform:'youtube', target:'', name:'ChanA', source:'/yt/a', dest:'/V'},
                    {id:'5', platform:'youtube', target:'', name:'ChanB', source:'/yt/b', dest:'/V'}, {id:'6', platform:'youtube', target:'', name:'ChanC', source:'/yt/c', dest:'/V'}];
                _orgMigrate();
                const out = { root: organizeConf.root, ov: organizeConf.overrides, keep: organizeRules.map(r => r.id) };
                platformTargets.twitter = T; organizeRules = R; organizeConf = K;
                return out; })()""")
            check('옮기기 — 공통은 대상 계정의 규칙에서만(/P) · 다른 곳만 따로(b → /B) · 한 계정의 둘째 규칙과 채널 규칙은 따로 추가한 규칙',
                  mig == {'root': '/P', 'ov': {'twitter:b': {'dest': '/B'}}, 'keep': ['2', '4', '5', '6']}, mig)
            # 9) 틀 칸에 치는 중(초안)에 정리가 돌아도 저장된 틀로
            sent = p.eval("""(function(){
                const o = backend.organizeRun; let t = null;
                backend.organizeRun = function(j){ t = JSON.parse(j).template; backend.organizeRun = o; };
                organizeTplTyping('{name}/{ye'); organizeRunAcc(0); const busy = _orgRunning;
                _orgUi.draft = null; _orgSetRunning(false); return [t, busy]; })()""")
            check('치는 중인 틀(초안)이 아니라 저장된 틀로 돈다', sent and sent[0] == '{name}・@{handle}-({service})/{wareki}{month}月', sent)
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
