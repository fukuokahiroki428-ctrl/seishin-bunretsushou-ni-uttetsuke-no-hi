#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""대상 목록 시험 — 격리 사본에서 대상이 수백 개일 때 쓰는 도구(검색 · 보기 · 한꺼번에 · 글자로 편집 · 팔로잉에서 가져오기)를 본다.

    python3 akashi/tests/targets.py        # 혼자(약 1분)
    python3 akashi/tests/run_all.py        # 다른 기능 시험과 함께

사용자(2026-10-15): "계정 다운받는데 3~4개면은 괜찮은데 대용량이라 정말 불편" — 다른 앱들(gallery-dl 의 목록 파일 · Hitomi 의
거르기 · JDownloader 의 보기 · tmd 의 팔로잉)처럼 바꿨다.

살피는 것:
  1 대상 500개 — 도구 줄 · 그리는 시간 · 상태 칸(마지막 받은 결과)
  2 검색으로 좁혀 보이는 것만 끄기 · 묶음 · 보기(꺼진 것 · 묶음 · 오류) · 지우기
  3 같은 아이디(대소문자만 다름)를 또 넣지 않는다
  4 글자로 편집 — 내보낸 글(묶음 · 꺼짐 · 별명), 고친 글 적용(주소 → 아이디 · 겹침 하나만 · 메모 줄 빼기),
    아이디가 같은 대상의 옵션 · 계정 지정은 남음
  5 팔로잉에서 가져오기 — 받은 목록(가짜 응답)에서 이미 있는 것 빼고 골라 넣기(별명 = 표시 이름 · 묶음)
  6 화면 JS 오류 없음
모든 자료는 지어낸 것이다. 네트워크에 묻지 않는다(팔로잉 응답은 화면에 바로 넣는다).
"""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _common as C  # noqa: E402
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from lib import paths  # noqa: E402


def cfg_targets(plat: str) -> list:
    cfg = json.loads((paths.data_dir(C.ISO / 'home') / paths.CONFIG_NAME).read_text(encoding='utf-8'))
    return (cfg.get('platformTargets') or {}).get(plat) or []


def rows(p, plat='twitter') -> list:
    dk = 'bsky' if plat == 'bluesky' else plat
    return p.eval("Array.from(document.querySelectorAll('#%s-target-list .batch-target-item')).map(e => +e.dataset.tidx)" % dk) or []


def main() -> int:
    check = C.Checks()
    try:
        if not C.iso_start():
            return 2
        with C.page() as p:
            time.sleep(1.5)
            p.eval("switchTab('twitter'); window.confirm = function(){ return true }; window.prompt = function(){ return window._akAnswer }")
            time.sleep(0.5)
            # 1) 대상 500개 — 하나는 옵션 · 계정 지정, 하나는 오류 난 기록
            took = p.eval("""(function(){
                const a = [];
                for (let i = 0; i < 500; i++) a.push({ target: 'user' + String(i).padStart(3, '0'), nickname: '별명 ' + i, enabled: true, accountIdx: 'all', options: {} });
                a[0].options = { count: 7 }; a[0].accountIdx = '0';
                a[5].last = { at: Date.now() - 3600e3, status: 'error', posts: 0, media: 0 };
                a[6].last = { at: Date.now() - 60e3, status: 'done', posts: 3, media: 9 };
                platformTargets.twitter = a;
                const t0 = performance.now(); renderPlatformTargetList('twitter'); const ms = performance.now() - t0;
                saveConfig(); return ms; })()""")
            n = len(rows(p))
            cnt = p.eval("document.querySelector('#twitter-target-list .tl-count').textContent")
            check('대상 500개 — 다 그리고 도구 줄의 수', n == 500 and cnt == '전체 500개', '%d줄 · %s' % (n, cnt))
            check('500개를 그리는 시간이 짧다(1.5초 안)', took is not None and took < 1500, '%.0fms' % (took or -1))
            st = p.eval("[5,6].map(i => (document.querySelector('#twitter-target-list .batch-target-item[data-tidx=\"' + i + '\"] .tl-status')||{}).textContent||'')")
            check('상태 칸 — 오류 표시 · 받은 미디어 수', st and st[0].startswith('✕') and st[1].endswith('9'), st)

            # 2) 검색 'user01' → user010~019 (10개) → 끄기 · 묶음
            p.eval("{ const q = document.querySelector('#twitter-target-list .tl-q'); q.value = 'user01'; q.dispatchEvent(new Event('input')) }")
            time.sleep(0.5)
            vis = rows(p)
            check('검색으로 좁힘 — 보이는 10개', vis == list(range(10, 20)), vis)
            p.eval("_tlBulk('twitter', 'off'); window._akAnswer = '묶음A'; _tlBulk('twitter', 'group')")
            time.sleep(0.4)
            saved = cfg_targets('twitter')
            off = [e['target'] for e in saved if e.get('enabled') is False]
            grp = [e['target'] for e in saved if e.get('group') == '묶음A']
            want = ['user%03d' % i for i in range(10, 20)]
            check('보이는 것만 끄기 · 묶음 — 설정 파일에도', off == want and grp == want and len(saved) == 500, (off[:3], len(grp), len(saved)))
            p.eval("{ const q = document.querySelector('#twitter-target-list .tl-q'); q.value = ''; q.dispatchEvent(new Event('input')) }")
            time.sleep(0.4)
            p.eval("_tlSetFilter('twitter', 'off')")
            a = rows(p)
            p.eval("_tlSetFilter('twitter', 'group:묶음A')")
            b = rows(p)
            p.eval("_tlSetFilter('twitter', 'error')")
            c = rows(p)
            check("보기 — '꺼진 것' · '묶음: 묶음A' · '오류 난 것'", a == list(range(10, 20)) and b == a and c == [5], (a, b, c))
            # 보기를 그린 뒤 오류가 하나 더 났다(다중 수집 중) — 한꺼번에는 그려 둔 것(보이는 것)에만
            p.eval("_tlSetFilter('twitter', 'error'); _tlRecordResult('twitter', platformTargets.twitter[7], 'error', 0, 0); _tlBulk('twitter', 'off')")
            offs = [e['target'] for e in p.eval("platformTargets.twitter") if e.get('enabled') is False and e['target'] in ('user005', 'user007')]
            check('한꺼번에는 그려 둔 줄에만(그린 뒤 오류 난 user007 은 그대로)', offs == ['user005'], offs)
            p.eval("platformTargets.twitter[5].enabled = true; _tlSetFilter('twitter', 'group:묶음A')")
            p.eval("togglePlatformTargetEnabled('twitter', 10, true, null)")
            hdr = p.eval("document.querySelector('#twitter-target-list .bti-head span').textContent")
            check('좁힌 보기에서 한 줄을 켜면 머리줄은 보이는 것 기준', hdr == '보이는 것  (켜짐 1 / 10 · 전체 500)', hdr)
            p.eval("_tlBulk('twitter', 'del'); _tlSetFilter('twitter', 'all')")
            time.sleep(0.3)
            check('보이는 것만 지우기', len(cfg_targets('twitter')) == 490 and len(rows(p)) == 490, len(cfg_targets('twitter')))

            # 2b) 로그에 실행될 수 있는 글(남의 표시 이름 · 아이디)이 섞여도 글자 그대로
            p.eval("platformTargets.twitter[0].nickname = ''; updatePlatformTargetNickname('twitter', 0, '<img src=x onerror=\"window._xss=1\">');"
                   "appendLog('<b>굵게</b> 그대로', 'info', 'twitter')")
            time.sleep(0.4)
            xs = p.eval("[window._xss === undefined, !!document.querySelector('#twitter-log img'), !!document.querySelector('#twitter-log b')]")
            check('로그 — 실행될 수 있는 것은 글자로(img onerror 실행 안 됨) · <b> 꾸밈은 그대로', xs == [True, False, True], xs)
            p.eval("platformTargets.twitter[0].nickname = '별명 0'; saveConfig()")

            # 3) 같은 아이디는 또 넣지 않는다
            p.eval("document.getElementById('twitter-target').value = 'USER000'; addPlatformTarget('twitter')")
            time.sleep(0.3)
            check('대소문자만 다른 같은 아이디는 또 넣지 않는다', len(p.eval("platformTargets.twitter") or []) == 490)

            # 4) 글자로 편집
            p.eval("platformTargets.twitter = platformTargets.twitter.slice(0, 3); platformTargets.twitter[2].enabled = false;"
                   "platformTargets.twitter[1].group = '묶음B'; renderPlatformTargetList('twitter'); openTargetTextEditor('twitter')")
            time.sleep(0.4)
            txt = p.eval("document.querySelector('#tl-text-modal .mw-text').value") or ''
            check('내보낸 글 — 별명 · 꺼짐(//) · 묶음(===)', 'user000  # 별명 0' in txt and '// user002  # 별명 2' in txt
                  and '=== 묶음B\nuser001  # 별명 1' in txt, txt[-120:])
            new_text = '\n'.join([
                '# 메모 줄은 빼기',
                'user000  # 별명 0',
                'https://x.com/NewOne?s=20  # 새 사람',
                '// off_one  # 꺼짐',
                'USER000',
                '',
                '=== 그룹C',
                'user001  # 새 별명',
                'g1',
            ])
            p.eval("{ const ta = document.querySelector('#tl-text-modal .mw-text'); ta.value = %s; ta.dispatchEvent(new Event('input')) }" % json.dumps(new_text))
            time.sleep(0.4)
            note = p.eval("document.querySelector('#tl-text-modal .mw-note').textContent")
            check('편집 중 수 세기 — 대상 5 · 겹침 1', note == '대상 5개 · 겹침 1', note)
            p.eval("document.querySelector('#tl-text-modal .mw-apply').click()")
            time.sleep(0.4)
            got = [(e['target'], e.get('nickname', ''), e.get('enabled'), e.get('group', '')) for e in cfg_targets('twitter')]
            want = [('user000', '별명 0', True, ''), ('NewOne', '새 사람', True, ''), ('off_one', '꺼짐', False, ''),
                    ('user001', '새 별명', True, '그룹C'), ('g1', '', True, '그룹C')]
            check('적용 — 주소 → 아이디 · 겹침 하나만 · 꺼짐 · 묶음 · 지운 대상(user002) 빠짐', got == want, got)
            keep = (cfg_targets('twitter') or [{}])[0]
            check('아이디가 같은 대상의 옵션 · 계정 지정은 남는다', keep.get('options') == {'count': 7} and keep.get('accountIdx') == '0', keep)
            closed = p.eval("document.getElementById('tl-text-modal').classList.contains('hidden')")
            check('적용하면 창이 닫힌다', closed is True, closed)

            # 4b) 일부러 둘 둔 대상(같은 계정 · 다른 옵션)은 글로 오가도 둘 다 남는다 · 주소로 넣어 둔 대상은 설정을 지킨다
            p.eval("platformTargets.twitter = [{target:'dup', accountIdx:'1', enabled:true, options:{type:'likes'}},"
                   " {target:'Dup', accountIdx:'all', enabled:true, options:{type:'bookmarks'}}]; renderPlatformTargetList('twitter'); openTargetTextEditor('twitter')")
            time.sleep(0.3)
            p.eval("{ const ta = document.querySelector('#tl-text-modal .mw-text'); ta.value += '\\nnew1\\n'; ta.dispatchEvent(new Event('input'));"
                   " document.querySelector('#tl-text-modal .mw-apply').click() }")
            time.sleep(0.4)
            d = [(e['target'], e.get('accountIdx'), (e.get('options') or {}).get('type')) for e in cfg_targets('twitter')]
            check('같은 대상 둘(옵션이 다름)은 둘 다 그대로', d == [('dup', '1', 'likes'), ('Dup', 'all', 'bookmarks'), ('new1', 'all', None)], d)
            p.eval("switchTab('bluesky'); platformTargets.bluesky = [{target:'https://bsky.app/profile/bar.bsky.social', accountIdx:'2', enabled:true, options:{count:5}},"
                   " {target:'@alice.bsky.social', accountIdx:'all', enabled:true, options:{}}]; renderPlatformTargetList('bluesky'); openTargetTextEditor('bluesky')")
            time.sleep(0.3)
            p.eval("{ const ta = document.querySelector('#tl-text-modal .mw-text'); ta.value += '\\nzed.bsky.social\\n'; ta.dispatchEvent(new Event('input'));"
                   " document.querySelector('#tl-text-modal .mw-apply').click() }")
            time.sleep(0.4)
            b = [(e['target'], e.get('accountIdx'), e.get('options')) for e in cfg_targets('bluesky')]
            check('주소로 넣어 둔 대상 — 아이디로 바뀌어도 계정 지정 · 옵션은 그대로', b[0] == ('bar.bsky.social', '2', {'count': 5}) and len(b) == 3, b)
            p.eval("document.getElementById('bsky-target').value = 'https://bsky.app/profile/new.bsky.social alice.bsky.social'; addPlatformTarget('bluesky')")
            time.sleep(0.3)
            b2 = [e['target'] for e in p.eval("platformTargets.bluesky")]
            check("추가 칸 — 블루스카이 주소는 핸들로('profile' 아님) · '@' 붙은 같은 대상은 또 넣지 않음", b2[-1] == 'new.bsky.social' and len(b2) == 4, b2)
            p.eval("switchTab('twitter')")

            # 5) 팔로잉에서 가져오기 — 받은 응답은 지어 넣는다(네트워크 없음)
            check('앱이 팔로잉 불러오기를 안다(backend.listFollowing · followListStop)',
                  p.eval("typeof backend.listFollowing === 'function' && typeof backend.followListStop === 'function'") is True)
            p.eval("switchTab('bluesky'); platformTargets.bluesky = [{target:'exists.bsky.social', enabled:true, accountIdx:'all', options:{}},"
                   " {target:'@atname.bsky.social', enabled:true, accountIdx:'all', options:{}}];"
                   "renderPlatformTargetList('bluesky'); openFollowImport('bluesky')")
            time.sleep(0.4)
            # 다른 플랫폼(트위터)의 늦은 결과는 넣지 않는다 · '이미 돌고 있음' 답에는 그만을 누를 수 있게
            p.eval("onFollowList(%s)" % json.dumps(json.dumps({'platform': 'twitter', 'of': 'tw', 'ok': True, 'users': [{'handle': 'twonly', 'name': 'T'}]})))
            other = p.eval("[document.querySelectorAll('#tl-follow-modal .fi-item').length, document.querySelector('#tl-follow-modal .fi-state').textContent]")
            check('다른 플랫폼의 결과는 이 창에 넣지 않는다', other and other[0] == 0 and '넣지 않았습니다' in other[1], other)
            p.eval("onFollowList(%s)" % json.dumps(json.dumps({'platform': 'bluesky', 'ok': False, 'busy': True, 'error': '이미 가져오는 중입니다'})))
            stop_ok = p.eval("!document.querySelector('#tl-follow-modal .fi-stop').disabled")
            check("'이미 돌고 있음' 답 — 그만을 누를 수 있다", stop_ok is True, stop_ok)
            p.eval("onFollowList(%s)" % json.dumps(json.dumps({'platform': 'bluesky', 'of': 'someone.bsky.social', 'ok': True, 'users': [
                {'handle': 'a.bsky.social', 'name': '에이\n둘째 줄'}, {'handle': 'b.bsky.social', 'name': ''}, {'handle': 'Exists.bsky.social', 'name': '있음'},
                {'handle': 'atname.bsky.social', 'name': '골뱅이'}]})))
            time.sleep(0.3)
            items = p.eval("Array.from(document.querySelectorAll('#tl-follow-modal .fi-item')).map(e => [e.querySelector('.fi-h').textContent, e.classList.contains('fi-have')])")
            check("받은 목록 — 이미 있는 것('@' 붙여 넣어 둔 것도)은 고를 수 없게", items == [['@a.bsky.social', False], ['@b.bsky.social', False], ['@Exists.bsky.social', True], ['@atname.bsky.social', True]], items)
            p.eval("{ const m = document.getElementById('tl-follow-modal'); m.querySelector('.fi-all').click(); m.querySelector('.fi-group').value = '팔로잉';"
                   "m.querySelector('.fi-add').click() }")
            time.sleep(0.4)
            bs = [(e['target'], e.get('nickname', ''), e.get('group', '')) for e in cfg_targets('bluesky')]
            check('고른 것 넣기 — 별명 = 표시 이름(줄바꿈은 빈칸) · 묶음', bs == [('exists.bsky.social', '', ''), ('@atname.bsky.social', '', ''),
                  ('a.bsky.social', '에이 둘째 줄', '팔로잉'), ('b.bsky.social', '', '팔로잉')], bs)
            check("넣은 대상이 정리하기의 계정 표에도", p.eval("_orgAccounts().filter(a => a.plat === 'bluesky').length") == 4)

            errs = C.page_errors(p)
            check('화면 JS 오류 없음', not errs, errs[:2])
    except Exception as e:  # noqa: BLE001 — 시험 도중의 어떤 실패도 판정 한 줄로
        check('시험 진행 중 오류 없음', False, '%s: %s · 앱 살아 있음=%s' % (type(e).__name__, e, C.iso_alive()))
    finally:
        C.keep_log('targets_app.log')
        C.iso_stop()
    return check.summary()


if __name__ == '__main__':
    sys.exit(main())
