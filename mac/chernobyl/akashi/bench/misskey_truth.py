#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""미스키 정답 — 공개 API 로 한 계정의 '공개로 보이는' 노트와 첨부 목록을 받아 둔다. 크롤러가 받은 것과 견줄 기준.

    python3 akashi/bench/misskey_truth.py tips                 # misskey.io 의 @tips
    python3 akashi/bench/misskey_truth.py notify --host misskey.io

결과: $TMPDIR/akashi-bench/truth_misskey_<host>_<이름>.json (AKASHI_BENCH_DIR 로 바꾼다)

★ 왜 공개 API 인가 — 크롤러가 화면으로 본 것이 '전부' 인지 알려면 전부가 무엇인지 따로 알아야 한다. 미스키는 로그인 없이
  users/show · users/notes(untilId 로 거슬러 올라감)를 준다. 표시되는 노트 수(notesCount)는 지운 것 · 비공개까지 세므로
  기준은 API 가 실제로 돌려주는 노트다. 파이썬 기본 이름표(User-Agent)는 막혀(403) 이름표를 붙인다.
"""
from __future__ import annotations

import argparse
import json
import os
import sys
import tempfile
import time
import urllib.request
from pathlib import Path

UA = 'Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) HanishikiBench/1.0'


def bench_dir() -> Path:
    d = Path(os.environ.get('AKASHI_BENCH_DIR') or (Path(tempfile.gettempdir()) / 'akashi-bench'))
    d.mkdir(parents=True, exist_ok=True)
    return d


def api(host: str, ep: str, body: dict):
    req = urllib.request.Request('https://%s/api/%s' % (host, ep), data=json.dumps(body).encode(),
                                 headers={'Content-Type': 'application/json', 'User-Agent': UA})
    with urllib.request.urlopen(req, timeout=30) as r:
        return json.loads(r.read())


def main() -> int:
    ap = argparse.ArgumentParser(description='미스키 정답 — 공개 API 로 노트 · 첨부 목록')
    ap.add_argument('username')
    ap.add_argument('--host', default='misskey.io')
    a = ap.parse_args()
    u = api(a.host, 'users/show', {'username': a.username})
    notes, until = [], None
    while True:
        body = {'userId': u['id'], 'limit': 100, 'withReplies': True, 'withRenotes': True}
        if until:
            body['untilId'] = until
        page = api(a.host, 'users/notes', body)
        if not page:
            break
        notes += page
        until = page[-1]['id']
        time.sleep(1.2)
        if len(page) < 100:
            break
    out = bench_dir() / ('truth_misskey_%s_%s.json' % (a.host, a.username))
    out.write_text(json.dumps({
        'site': 'misskey', 'host': a.host,
        'user': {'id': u['id'], 'username': a.username, 'notesCount': u.get('notesCount')},
        'notes': [{'id': n['id'], 'createdAt': n['createdAt'], 'text': (n.get('text') or '')[:200],
                   'renoteId': n.get('renoteId'), 'replyId': n.get('replyId'),
                   'files': [{'id': f['id'], 'type': f.get('type'), 'url': f.get('url'),
                              'thumbnailUrl': f.get('thumbnailUrl'), 'size': f.get('size')} for f in (n.get('files') or [])]}
                  for n in notes]}, ensure_ascii=False), encoding='utf-8')
    files = [f for n in notes for f in (n.get('files') or [])]
    print('@%s@%s · 표시 %s · 공개로 받은 노트 %d · 첨부 %d개(%.1f MB) · 가장 옛 %s → %s' % (
        a.username, a.host, u.get('notesCount'), len(notes), len(files), sum((f.get('size') or 0) for f in files) / 1e6,
        notes[-1]['createdAt'][:10] if notes else '-', out.name))
    return 0


if __name__ == '__main__':
    sys.exit(main())
