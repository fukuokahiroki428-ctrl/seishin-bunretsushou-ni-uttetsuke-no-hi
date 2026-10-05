#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""'옛 트위터로 보기' 시험 — 격리 사본 + 가짜 보관 폴더(지어낸 두 계정).

    python3 akashi/tests/viewer.py         # 혼자(약 1분)

살피는 것:
  1 트위터 탭에 단추가 보인다
  2 누르면 번들 파이썬이 twitter_viewer.py 를 돌려 계정 목록 · 계정별 화면 둘 · 계정 자료(accounts.js)를 만든다
  3 화면 로그에 완료 줄, 앱 기록에 '열 곳' 줄(사용자 브라우저는 열지 않는다)
  4 다시 누르면 바뀐 계정만 다시 만든다(그대로면 data.js 를 다시 쓰지 않는다)
  5 번들 안에 .pyc 를 남기지 않는다 — 남기면 봉인(서명)이 깨진다
  6 화면 JS 오류 없음

★ 왜 HANISHIKI_NO_OPEN_URL 인가
  단추는 다 만든 뒤 기본 브라우저로 목록을 연다. 시험이 사용자의 브라우저에 창을 띄우면 안 되므로, 이 환경 변수를
  준 사본은 여는 대신 '[TWITTER-VIEWER] 열 곳:' 을 기록에 남긴다(4.0.0 r37 부터).
"""
from __future__ import annotations

import json
import shutil
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _common as C  # noqa: E402
import fixtures  # noqa: E402
from lib import paths  # noqa: E402


def main() -> int:
    check = C.Checks()
    app = C.test_app()
    py = paths.bundled_python(app) if app else None
    if not py:
        print('번들 파이썬이 없습니다 — 빌드부터')
        return 2
    arch = fixtures.build_fakearch(C.work_dir('fakearch'), py)
    try:
        if not C.iso_start('--env', 'HANISHIKI_NO_OPEN_URL=1'):
            return 2
        base = C.ISO / 'tmp' / 'arch'
        shutil.rmtree(base, ignore_errors=True)
        shutil.copytree(arch, base / 'twitter')
        with C.page() as p:
            time.sleep(1.5)
            p.eval("switchTab('twitter')")
            time.sleep(0.6)
            btn = p.eval("(function(){var b=document.getElementById('twitter-viewer-btn');return !!b && b.offsetParent!==null})()")
            check("'옛 트위터로 보기' 단추가 보인다", btn is True, btn)
            p.eval("document.getElementById('twitter-path').value=%s; document.getElementById('twitter-viewer-btn').click()"
                   % json.dumps(str(base)))
            idx = base / 'twitter' / 'index.html'
            t0 = time.time()
            while time.time() - t0 < 60 and not idx.exists():
                time.sleep(0.5)
            check('계정 목록 화면이 생겼다', idx.exists(), '%.1fs' % (time.time() - t0))
            time.sleep(1.5)
            views = sorted(x.parent.parent.name for x in (base / 'twitter').glob('*/view/index.html'))
            check('계정별 화면 둘', len(views) == 2, views)
            accf = base / 'twitter' / 'accounts.js'
            acc = accf.read_text(encoding='utf-8') if accf.exists() else ''
            check('계정 목록 자료에 두 계정', acc.count('"handle"') == 2)
            logtxt = p.eval("(document.getElementById('twitter-log')||document.body).innerText.slice(-2000)") or ''
            check('화면 로그에 완료 줄', '받아 둔 계정 2개' in logtxt, logtxt[-160:].replace('\n', ' / '))
            applog = (C.ISO / 'app.log').read_text(encoding='utf-8', errors='replace')
            check('사용자 브라우저 대신 기록에 열 곳', '[TWITTER-VIEWER]' in applog)
            # 두 번째 누름 — 바뀐 것이 없으면 다시 만들지 않는다(meta.json 의 sig)
            data = base / 'twitter' / 'akashi_demo' / 'view' / 'data.js'
            m1 = data.stat().st_mtime
            p.eval("document.getElementById('twitter-viewer-btn').click()")
            time.sleep(6)
            check('두 번째에는 바뀐 계정만 다시 만든다', data.stat().st_mtime == m1)
            pyc = list((C.ISO / 'app').rglob('twitter_viewer*.pyc'))
            check('번들 안에 .pyc 를 남기지 않는다(봉인 유지)', not pyc, [x.name for x in pyc[:2]])
            errs = C.page_errors(p)
            check('화면 JS 오류 없음', not errs, errs[:2])
    except Exception as e:  # noqa: BLE001
        check('시험 진행 중 오류 없음', False, '%s: %s' % (type(e).__name__, e))
    finally:
        C.keep_log('viewer_app.log')
        C.iso_stop()
    return check.summary()


if __name__ == '__main__':
    sys.exit(main())
