#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""끝까지 기록 벤치 — 앱의 도우미(resources/tools/crawl_record.py)를 번들 Chrome for Testing 으로 돌리고, 정답과 견준다.

    python3 akashi/bench/misskey_truth.py tips                     # 정답 먼저(미스키 공개 API)
    python3 akashi/bench/spa_record.py https://misskey.io/@tips --truth truth_misskey_misskey.io_tips.json
    python3 akashi/bench/spa_record.py https://x.com/<계정> --cookies <CDP 쿠키 JSON 파일>   # 값은 찍지 않는다

결과: $TMPDIR/akashi-bench/rec_<호스트>_<경로>/ (index.html · items.jsonl · media/ · api/ · report.json)
      받은 것은 시험이 끝나면 지운다 — 남의 계정 자료다.

★ 도우미를 그대로 부른다 — 벤치와 앱이 다른 코드를 돌리면 벤치 숫자가 앱의 숫자가 아니다.
★ 실측(2026-10-08, 시제품 단계):
    미스키 @tips    47/47 노트 · 첨부 84/84 (64초)
    미스키 @notify  408/408 노트 · 첨부 91/92 (176초 — 빠진 하나는 2019년 저장 서버가 없어진 것)
    X @neuralink    게시물 탭 끝까지: 본인 글 168(가장 옛 2019-07-11) · 첨부 94/94
  예전: 크롤러(보이지 않는 QWebEngine · 스크롤 5번) @tips 15/47, 실제 Chrome 모드(window 스크롤)는 첫 화면만.
"""
from __future__ import annotations

import argparse
import glob
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from urllib.parse import urlparse

HERE = Path(__file__).resolve().parent
CHERNOBYL = HERE.parent.parent
sys.path.insert(0, str(HERE.parent))
from lib import paths  # noqa: E402


def bench_dir() -> Path:
    d = Path(os.environ.get('AKASHI_BENCH_DIR') or (Path(tempfile.gettempdir()) / 'akashi-bench'))
    d.mkdir(parents=True, exist_ok=True)
    return d


def chrome_path() -> str:
    app = paths.find_build_app()
    hits = sorted(glob.glob(str(app / 'Contents/Resources/chromium*/Chromium.app/Contents/MacOS/Google Chrome for Testing'))) if app else []
    if not hits:
        raise SystemExit('번들 Chrome for Testing 을 찾지 못했습니다 — 빌드부터')
    return hits[0]


def main() -> int:
    ap = argparse.ArgumentParser(description='끝까지 기록 벤치')
    ap.add_argument('url')
    ap.add_argument('--truth', help='정답 JSON(bench 폴더 안 이름 또는 경로)')
    ap.add_argument('--cookies', help='CDP 쿠키 JSON 파일')
    ap.add_argument('--max-seconds', type=float, default=1800)
    ap.add_argument('--headful', action='store_true')
    ap.add_argument('--no-download', action='store_true')
    a = ap.parse_args()

    pu = urlparse(a.url)
    out = bench_dir() / ('rec_%s_%s' % (pu.hostname, re.sub(r'[^A-Za-z0-9_-]', '_', pu.path.strip('/'))[:60] or 'root'))
    shutil.rmtree(out, ignore_errors=True)
    cmd = [sys.executable, str(CHERNOBYL / 'resources/tools/crawl_record.py'), '--url', a.url, '--out', str(out),
           '--chrome', chrome_path(), '--max-seconds', str(a.max_seconds)]
    if a.cookies:
        cmd += ['--cookies-file', a.cookies]
    if a.headful:
        cmd.append('--headful')
    if a.no_download:
        cmd.append('--no-download')
    report = {}
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True) as p:
        for line in p.stdout:
            try:
                ev = json.loads(line)
            except ValueError:
                print('  ?', line.rstrip())
                continue
            if ev['ev'] == 'log':
                print('  [%s] %s' % (ev.get('level'), ev.get('msg')))
            elif ev['ev'] == 'progress':
                print('  %4d초 · 글 %d · 첨부 %d%s · JSON %d · 단추 %d' % (
                    ev['sec'], ev['items'], ev['media'], (' (받음 %d)' % ev['saved']) if 'saved' in ev else '',
                    ev['json'], ev['clicks']), flush=True)
            elif ev['ev'] == 'done':
                report = ev
    print('== %s · %s초 · 멈춤: %s · 본문 상자 %s' % (a.url, report.get('seconds'), report.get('stop'), report.get('scroller')))
    print('   글 %s · JSON %s · 더 보기 %s번 · 첨부 %s 중 %s 받음(실패 %s)%s' % (
        report.get('items'), report.get('json_responses'), report.get('more_clicks'), report.get('media_found'),
        report.get('media_saved'), report.get('media_failed'), ' · 물러선 자리(페이지 그림)' if report.get('media_fallback') else ''))
    print('   쓴이 상위:', report.get('top_authors', [])[:4])
    if a.truth:
        tp = Path(a.truth) if Path(a.truth).exists() else bench_dir() / a.truth
        truth = json.loads(tp.read_text(encoding='utf-8'))
        got = {}
        with open(out / 'items.jsonl', encoding='utf-8') as f:
            for line in f:
                it = json.loads(line)
                got[it['id']] = it
        names = ' '.join(os.listdir(out / 'media'))
        ids = [n['id'] for n in truth['notes']]
        hit = [i for i in ids if i in got]
        keys = [re.sub(r'[^\w.\-]', '_', (f.get('url') or '').rsplit('/', 1)[-1].split('?')[0]).rsplit('.', 1)[0]
                for n in truth['notes'] for f in n['files']]
        fhit = [k for k in keys if k and k in names]
        print('   정답 견주기: 노트 %d 중 %d (%.0f%%) · 첨부 원본 %d 중 %d (%.0f%%)' % (
            len(ids), len(hit), 100 * len(hit) / max(1, len(ids)), len(keys), len(fhit), 100 * len(fhit) / max(1, len(keys))))
        miss = [n for n in truth['notes'] if n['id'] not in got]
        if miss:
            print('   빠진 노트 예:', [(n['createdAt'][:10], (n['text'] or '')[:24].replace('\n', ' ')) for n in miss[:3]])
    return 0


if __name__ == '__main__':
    sys.exit(main())
