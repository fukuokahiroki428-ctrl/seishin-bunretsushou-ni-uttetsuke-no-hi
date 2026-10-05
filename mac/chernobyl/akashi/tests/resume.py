#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""이어받기 시험 — 격리 사본 + 이 맥 안의 시험 서버(인터넷으로 안 나감).

    python3 akashi/tests/resume.py         # 혼자(약 1분)

살피는 것:
  A 유튜브 장부 — 오디오로 받은 영상을 나중에 동영상으로도 받는다(.yt_archive_audio.txt 가 따로 생긴다)
  B 받는 도중 앱이 죽어도(SIGKILL) 진짜 이름으로 잘린 파일이 남지 않는다 — 받는 동안은 숨은 조각 파일(.part)

★ 왜 서버가 머리말엔 3MB 라 하고 0.5MB 만 주나
  잘린 파일 문제는 '받는 도중에 죽었을 때' 만 드러난다. 그래서 반쯤 보낸 뒤 1분 멈추는 그림을 주고, 조각 파일이
  생긴 것을 본 순간 사본의 PID 를 SIGKILL 한다(iso.py 기록의 PID — 이름으로 찾지 않는다).
★ 왜 시험 영상을 번들 ffmpeg 로 만드나
  인터넷의 영상을 쓰면 시험이 바깥에 기대게 된다. 2초짜리 시험 화면 · 소리를 그 자리에서 만들어 이 맥 안에서 준다.
"""
from __future__ import annotations

import http.server
import json
import os
import signal
import subprocess
import sys
import threading
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _common as C  # noqa: E402
import fixtures  # noqa: E402
from lib.cdp import Page  # noqa: E402

BIG = 3_000_000
DONE = ('완료', 'Done', '중단됨', '오류', 'Error', 'Ready', '대기')


def make_handler(www: Path):
    class H(http.server.SimpleHTTPRequestHandler):
        def __init__(self, *a, **k):
            super().__init__(*a, directory=str(www), **k)

        def do_GET(self):
            if self.path.startswith('/big.jpg'):
                self.send_response(200)
                self.send_header('Content-Type', 'image/jpeg')
                self.send_header('Content-Length', str(BIG))
                self.end_headers()
                try:
                    self.wfile.write(b'\xff\xd8' + b'\0' * 499_998)
                    self.wfile.flush()
                    time.sleep(60)
                except OSError:
                    pass
                return
            return super().do_GET()

        def log_message(self, *a):
            pass
    return H


def main() -> int:
    check = C.Checks()
    app = C.test_app()
    ffmpeg = (app / 'Contents' / 'MacOS' / 'ffmpeg') if app else None
    if not ffmpeg or not ffmpeg.exists():
        print('번들 ffmpeg 가 없습니다 — 빌드부터')
        return 2
    www = C.work_dir('www_resume')
    clip = www / 'clip.mp4'
    if not clip.exists():
        subprocess.run([str(ffmpeg), '-loglevel', 'error', '-f', 'lavfi', '-i', 'testsrc=duration=2:size=160x120:rate=10',
                        '-f', 'lavfi', '-i', 'sine=duration=2', '-c:v', 'libx264', '-pix_fmt', 'yuv420p', '-c:a', 'aac',
                        '-shortest', str(clip)], check=True)
    (www / 'p.html').write_text('<!doctype html><meta charset="utf-8"><title>큰 그림</title><p>위</p>'
                                '<div style="height:30000px"></div><img loading="lazy" src="/big.jpg" width="10">', encoding='utf-8')
    hp = C.free_port()
    srv = fixtures.QuietServer(('127.0.0.1', hp), make_handler(www))
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    try:
        if not C.iso_start():
            return 2
        yt = C.ISO / 'tmp' / 'yt'
        out = C.ISO / 'tmp' / 'crawl-out'
        with Page.attach(C.PORT) as p:
            time.sleep(1.5)
            # ── A) 유튜브 장부 ──
            base = dict(url='http://127.0.0.1:%d/clip.mp4' % hp, path=str(yt), quality='best', subs=False, thumb=False,
                        metadata=False, playlist=False, sponsor=False, comments=False, realCapture=False, ytPace='fast')
            for typ in ('audio', 'video'):
                p.eval("switchTab('youtube'); backend.startYoutube(%s)" % json.dumps(json.dumps(dict(base, type=typ))))
                time.sleep(3)
                st, dt = C.wait_status(p, 'youtube', DONE, 120, step=1)
                print('   유튜브 %s →' % typ, st.get('status'), ('%.0fs' % dt) if dt else '시간 초과')
                time.sleep(2)
            aud = yt / 'youtube' / '.yt_archive_audio.txt'
            vid = yt / 'youtube' / '.yt_archive.txt'
            check('오디오 장부가 따로 생겼다', aud.exists() and aud.read_text().strip() != '',
                  aud.read_text().strip() if aud.exists() else '없음')
            check('동영상 장부에도 적혔다(오디오로 받은 뒤에도 동영상을 받음)', vid.exists() and vid.read_text().strip() != '',
                  vid.read_text().strip() if vid.exists() else '없음')
            media = sorted(x.suffix for x in yt.rglob('*') if x.suffix.lower() in ('.mp3', '.m4a', '.opus', '.mp4', '.webm', '.mkv'))
            check('오디오 파일과 동영상 파일이 둘 다 있다',
                  any(s in ('.mp3', '.m4a', '.opus') for s in media) and any(s in ('.mp4', '.webm', '.mkv') for s in media), media)

            # ── B) 받는 도중 앱이 죽기 ──
            p.eval("""(function(){var g=function(i){return document.getElementById(i)}; switchTab('crawl');
              g('crawl-url').value=%s; g('crawl-depth').value=0; g('crawl-maxpages').value=1; g('crawl-delay').value='0.2';
              g('crawl-path').value=%s; g('crawl-same-domain').checked=true;
              ['crawl-deep-scroll','crawl-security','crawl-exif','crawl-wait-login'].forEach(function(i){var e=g(i); if(e) e.checked=false});
              var m=g('crawl-method'); if(m) m.value=''; startCrawl();})()"""
                   % (json.dumps('http://127.0.0.1:%d/p.html' % hp), json.dumps(str(out))))
        # 큰 그림 받기가 시작될 때까지(조각 파일이 생길 때까지) 기다렸다가 죽인다
        part = []
        for _ in range(60):
            part = [x for x in out.rglob('*') if x.name.endswith('.part')]
            if part:
                break
            time.sleep(0.5)
        check('받는 동안은 조각 파일(.part)에 쓴다', bool(part), [x.name[:40] for x in part])
        pid = C.iso_state().get('pid')
        if pid:
            os.kill(int(pid), signal.SIGKILL)
            time.sleep(2)
        finals = [x for x in out.rglob('*') if x.is_file() and not x.name.endswith('.part') and x.name.startswith('big')]
        check('죽은 뒤 진짜 이름(big…)으로 잘린 파일이 없다', not finals, [(x.name, x.stat().st_size) for x in finals])
    except Exception as e:  # noqa: BLE001
        check('시험 진행 중 오류 없음', False, '%s: %s' % (type(e).__name__, e))
    finally:
        C.keep_log('resume_app.log')
        C.iso_stop()
        srv.shutdown()
    return check.summary()


if __name__ == '__main__':
    sys.exit(main())
