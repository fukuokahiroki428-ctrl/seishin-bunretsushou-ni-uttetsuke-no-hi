#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""끝까지 기록(経済産業省 탭 · '끝까지 기록') 시험 — 격리 사본 + 이 맥 안의 가짜 앱형 사이트(인터넷으로 안 나감).

    python3 akashi/tests/crawl_record.py      # 혼자(약 3분)
    python3 akashi/tests/run_all.py           # 다른 기능 시험과 함께

가짜 사이트는 미스키 · 트위터에서 실측한 까다로운 점을 모두 갖는다.
  · 문서가 아니라 안쪽 상자(#main)가 스크롤된다 — window 를 내리면 아무 일도 없다(미스키)
  · 처음 두 쪽은 스크롤로 이어 붙고, 그다음은 '더 보기' 단추를 눌러야 한다(로그인 안 한 미스키)
  · 지나간 글은 화면에서 지운다 — 마지막 화면엔 15개만 남는다(트위터의 가상 목록)
  · 옆 칸(#side)도 따로 스크롤되고 그 안에 미끼 '더 보기' 가 있다 — 누르면 서버가 센다(미스키 로컬 타임라인 위젯)
  · 글 자료는 POST JSON(untilId) 으로만 온다, 그림은 썸네일만 화면에 걸린다(원본 주소는 JSON 에만)
  · 글 하나는 다른 사람의 글을 품은 리노트다

살피는 것:
  1 끝까지 — 주인 글 45/45 · 리노트 속 글 · 첨부 원본 15/15(썸네일이 아니라 원본, 크기까지) · index.html · 미끼 단추 0번
  2 중간에 멈추기 — 상태 '중단됨', 받은 것까지 index.html · items.jsonl 로 남고, 기록용 Chrome 이 남지 않는다
  3 화면 JS 오류 없음, 사용자 앱의 Chrome(9223)은 그대로
모든 자료는 지어낸 것이다(쓴이 akashi_demo · yubari_demo, 그림은 이 자리에서 만든 PNG).
"""
from __future__ import annotations

import glob
import http.server
import json
import os
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _common as C  # noqa: E402
import fixtures  # noqa: E402

N_NOTES = 45
PAGE = 10

SPA = r"""<!doctype html><html lang="ko"><head><meta charset="utf-8"><title>가짜 앱형 사이트</title>
<style>html,body{margin:0;height:100%;overflow:hidden;font:14px sans-serif}
#main{position:absolute;left:0;top:0;bottom:0;right:320px;overflow-y:auto}
#side{position:absolute;right:0;top:0;bottom:0;width:320px;overflow-y:auto;border-left:1px solid #ccc}
.note{height:180px;border-bottom:1px solid #ddd;padding:8px;box-sizing:border-box}.w{height:140px;border-bottom:1px solid #eee}
</style></head><body>
<div id="main"><div id="spacer"></div><div id="list"></div><div id="sentinel" style="height:10px"></div><div id="morebox" style="text-align:center;padding:20px"></div></div>
<div id="side"><div id="wlist"></div><div style="padding:10px"><button id="wmore">더 보기</button></div></div>
<script>
const slow = location.search.indexOf('slow') >= 0;
let until = null, page = 0, done = false, loading = false;
const list = document.getElementById('list'), spacer = document.getElementById('spacer'), mb = document.getElementById('morebox');
async function load() {
  if (loading || done) return; loading = true;
  const r = await fetch('/api/notes' + (slow ? '?slow=1' : ''), {method: 'POST', headers: {'Content-Type': 'application/json'},
                        body: JSON.stringify({untilId: until, limit: 10})});
  const notes = await r.json(); page++;
  for (const n of notes) {
    const d = document.createElement('div'); d.className = 'note'; d.textContent = n.text || '(리노트) ' + (n.renote ? n.renote.text : '');
    for (const f of n.files) { const im = new Image(); im.src = f.thumbnailUrl; im.width = 80; d.appendChild(im); }
    list.appendChild(d);
  }
  while (list.children.length > 15) { spacer.style.height = (spacer.offsetHeight + list.firstChild.offsetHeight) + 'px'; list.removeChild(list.firstChild); }
  if (notes.length < 10) done = true; else until = notes[notes.length - 1].id;
  loading = false;
  if (done) mb.innerHTML = '<p>끝</p>';
  else if (page >= 2 && !mb.querySelector('button')) { const b = document.createElement('button'); b.textContent = '더 보기'; b.onclick = load; mb.appendChild(b); }
}
new IntersectionObserver(es => { if (es[0].isIntersecting && page < 2) load(); }, {root: document.getElementById('main')})
  .observe(document.getElementById('sentinel'));
async function widget(first) {
  const r = await fetch('/api/decoy', {method: 'POST', body: JSON.stringify({first: first})});
  for (const n of await r.json()) { const d = document.createElement('div'); d.className = 'w'; d.textContent = n.text; document.getElementById('wlist').appendChild(d); }
}
document.getElementById('wmore').onclick = () => widget(false);
widget(true);
load();
</script></body></html>"""


def notes_db():
    out = []
    for i in range(N_NOTES, 0, -1):
        n = {'id': 'n%03d' % i, 'createdAt': '2026-09-%02dT%02d:00:00.000Z' % (1 + i // 2, i % 24),
             'text': '가짜 글 %d번 — 끝까지 기록 시험' % i, 'user': {'username': 'akashi_demo', 'name': '공작함 아카시'}, 'files': []}
        if i % 3 == 0:
            n['files'] = [{'id': 'f%03d' % i, 'type': 'image/png', 'url': '/media/full_%03d.png' % i,
                           'thumbnailUrl': '/media/thumb_%03d.png' % i, 'size': 0}]
        if i == 10:
            n['text'] = None
            n['renote'] = {'id': 'r010', 'createdAt': '2026-08-30T09:00:00.000Z', 'text': '리노트된 남의 글',
                           'user': {'username': 'yubari_demo'}, 'files': []}
        out.append(n)
    return out


class SpaHandler(http.server.BaseHTTPRequestHandler):
    notes = []
    images = {}
    decoy_calls = [0]
    note_calls = [0]

    def log_message(self, *a):
        pass

    def _send(self, code, body: bytes, ctype: str):
        self.send_response(code)
        self.send_header('Content-Type', ctype)
        self.send_header('Content-Length', str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self):
        path = self.path.split('?')[0]
        if path in ('/', '/spa.html'):
            return self._send(200, SPA.encode(), 'text/html; charset=utf-8')
        if path.startswith('/media/') and path[7:] in self.images:
            return self._send(200, self.images[path[7:]], 'image/png')
        return self._send(404, b'not found', 'text/plain')

    def do_POST(self):
        n = int(self.headers.get('Content-Length') or 0)
        body = json.loads(self.rfile.read(n) or b'{}') if n else {}
        if self.path.startswith('/api/decoy'):
            self.decoy_calls[0] += 1
            k = self.decoy_calls[0]
            rows = [{'id': 'w%02d_%d' % (k, j), 'createdAt': '2026-10-01T00:00:00Z', 'text': '위젯 글 %d-%d' % (k, j),
                     'user': {'username': 'decoy_widget'}, 'files': []} for j in range(5)]
            return self._send(200, json.dumps(rows).encode(), 'application/json')
        if self.path.startswith('/api/notes'):
            self.note_calls[0] += 1
            if 'slow' in self.path:
                time.sleep(2.5)
            ids = [x['id'] for x in self.notes]
            start = ids.index(body['untilId']) + 1 if body.get('untilId') in ids else 0
            return self._send(200, json.dumps(self.notes[start:start + PAGE], ensure_ascii=False).encode(), 'application/json')
        return self._send(404, b'{}', 'application/json')


def serve():
    SpaHandler.notes = notes_db()
    imgs = {}
    for i in range(3, N_NOTES + 1, 3):
        imgs['full_%03d.png' % i] = fixtures.png(640, 480, (i * 5 % 256, 90, 200 - i))
        imgs['thumb_%03d.png' % i] = fixtures.png(80, 60, (i * 5 % 256, 90, 200 - i))
    SpaHandler.images = imgs
    srv = fixtures.QuietServer(('127.0.0.1', 0), SpaHandler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


def record_chromes() -> list:
    out = subprocess.run(['/bin/ps', '-axww', '-o', 'pid=,command='], capture_output=True, text=True).stdout
    return [int(l.split()[0]) for l in out.splitlines() if 'chrome_capture_profile_record' in l and str(C.ISO) in l]


def main() -> int:
    check = C.Checks()
    user_chrome = C.listen_pids(C.USER_CAPTURE_PORT)
    print('[사용자 앱 Chrome %d] %s' % (C.USER_CAPTURE_PORT, user_chrome or '없음'))
    srv, hp = serve()
    try:
        if not C.iso_start():
            return 2
        out = C.ISO / 'tmp' / 'record-out'
        out.mkdir(parents=True, exist_ok=True)
        with C.page() as p:
            time.sleep(1.5)
            p.eval("switchTab('crawl')")
            time.sleep(0.8)
            has = p.eval("!!document.getElementById('crawl-record')")
            check("크롤 탭에 '끝까지 기록' 칸이 있다", has is True, has)

            def run(url):
                p.eval("""(function(){var g=function(i){return document.getElementById(i)};
                  g('crawl-url').value=%s; g('crawl-path').value=%s; g('crawl-login-cookie').value=''; g('crawl-login-check').value='';
                  ['crawl-wait-login','crawl-real-chrome'].forEach(function(i){var e=g(i); if(e) e.checked=false});
                  var m=g('crawl-method'); if(m) m.value=''; g('crawl-record').checked=true; startCrawl();})()"""
                       % (json.dumps(url), json.dumps(str(out))))

            # 1) 끝까지
            SpaHandler.decoy_calls[0] = 0
            run('http://127.0.0.1:%d/spa.html' % hp)
            st, took = C.wait_status(p, 'crawl', ('Done', '완료', '중단됨', 'Error', '오류'), 240)
            check('끝까지 기록한 판이 끝났다', took is not None and str(st.get('status')) in ('Done', '완료'),
                  ('%.0fs · %s' % (took, st.get('status'))) if took else st)
            recs = sorted(glob.glob(str(out) + '/crawl_*/record_001_*'))
            rec = Path(recs[-1]) if recs else None
            items = []
            if rec and (rec / 'items.jsonl').exists():
                items = [json.loads(x) for x in (rec / 'items.jsonl').read_text(encoding='utf-8').splitlines() if x.strip()]
            own = [it for it in items if it['author'] == '@akashi_demo']
            check('주인 글 %d/%d' % (len(own), N_NOTES), len(own) == N_NOTES, '전체 %d · 기록 폴더 %s' % (len(items), rec.name if rec else '없음'))
            rn = next((it for it in items if it['id'] == 'n010'), None)
            check('리노트가 품은 남의 글도 글로 남고 인용으로 이어진다', bool(rn) and 'r010' in rn.get('refs', [])
                  and any(it['id'] == 'r010' and it['author'] == '@yubari_demo' for it in items), rn and rn.get('refs'))
            want = {'full_%03d.png' % i: SpaHandler.images['full_%03d.png' % i] for i in range(3, N_NOTES + 1, 3)}
            got = {f: (rec / 'media' / f).read_bytes() for f in want if rec and (rec / 'media' / f).exists()}
            check('첨부 원본 %d/%d — 바이트까지 같다' % (len(got), len(want)), got == want, sorted(set(want) - set(got))[:3])
            thumbs = glob.glob(str(rec / 'media' / 'thumb_*')) if rec else []
            check('썸네일은 첨부로 받지 않는다', not thumbs, thumbs[:2])
            idx = (rec / 'index.html').read_text(encoding='utf-8') if rec and (rec / 'index.html').exists() else ''
            check('index.html — 주인이 먼저 골라져 있고 원본 그림을 가리킨다', 'value="@akashi_demo" selected' in idx
                  and 'media/full_045.png' in idx, len(idx))
            check("옆 칸의 미끼 '더 보기' 는 누르지 않았다", SpaHandler.decoy_calls[0] == 1, '위젯 요청 %d번' % SpaHandler.decoy_calls[0])
            # 로그는 묶어서 화면으로 가 상태 'Done' 보다 조금 늦게 올 수 있다 — 몇 초 기다린다
            t_log = time.time()
            log_txt = ''
            while time.time() - t_log < 8:
                log_txt = p.eval("(document.getElementById('crawl-log')||{}).innerText||''") or ''
                if '✅ 기록' in log_txt:
                    break
                time.sleep(0.4)
            check("로그에 '✅ 기록' 줄", '✅ 기록' in log_txt, log_txt[-200:].replace('\n', ' / '))
            check('기록용 Chrome 이 남지 않았다', not record_chromes(), record_chromes())

            # 2) 중간에 멈추기 — 느린 판(한 쪽에 2.5초)을 시작하고 곧 멈춘다
            time.sleep(1)
            run('http://127.0.0.1:%d/spa.html?slow=1' % hp)
            t0 = time.time()
            while time.time() - t0 < 60 and SpaHandler.note_calls[0] < 1:
                time.sleep(0.5)
            time.sleep(9)
            p.eval("stopCrawl()")
            st2, _ = C.wait_status(p, 'crawl', ('중단됨', 'Stopped', '중지됨', 'Done'), 40)
            t1 = time.time()
            while time.time() - t1 < 40 and record_chromes():
                time.sleep(0.5)
            check("멈춘 판 — 상태 '중단됨'", str(st2.get('status')) in ('중단됨', 'Stopped', '중지됨'), st2.get('status'))
            check('멈춘 판 — 기록용 Chrome 이 40초 안에 내려갔다', not record_chromes(), record_chromes())
            recs2 = sorted(glob.glob(str(out) + '/crawl_*/record_001_*'), key=os.path.getmtime)
            rec2 = Path(recs2[-1]) if recs2 else None
            t2 = time.time()
            while time.time() - t2 < 20 and rec2 and not (rec2 / 'index.html').exists():
                time.sleep(0.5)
            part = [x for x in ((rec2 / 'items.jsonl').read_text(encoding='utf-8').splitlines() if rec2 and (rec2 / 'items.jsonl').exists() else []) if x.strip()]
            check('멈춘 판 — 받은 것까지 index.html · items.jsonl 로 남았다', rec2 is not None and rec2 != rec
                  and (rec2 / 'index.html').exists() and 0 < len(part) < N_NOTES + 10, '%s · 글 %d' % (rec2.name if rec2 else '없음', len(part)))
            errs = C.page_errors(p)
            check('화면 JS 오류 없음', not errs, errs[:2])
    except Exception as e:  # noqa: BLE001 — 시험 도중의 어떤 실패도 판정 한 줄로
        check('시험 진행 중 오류 없음', False, '%s: %s · 앱 살아 있음=%s' % (type(e).__name__, e, C.iso_alive()))
    finally:
        C.keep_log('crawl_record_app.log')
        C.iso_stop()
        srv.shutdown()
    check('사용자 앱의 Chrome(%d)은 그대로' % C.USER_CAPTURE_PORT, C.listen_pids(C.USER_CAPTURE_PORT) == user_chrome,
          C.listen_pids(C.USER_CAPTURE_PORT))
    return check.summary()


if __name__ == '__main__':
    sys.exit(main())
