# -*- coding: utf-8 -*-
"""시험 재료 — 이 맥 안에서만 열리는 시험 사이트와, 트위터 보관 폴더를 흉내 낸 가짜 계정 둘.

★ 왜 매번 새로 만드나
  재료를 저장소에 두면 그림 파일이 공개 저장소에 올라가고, 임시 폴더에 두면 세션 사이에 비워진다
  (2026-10-05 에 실제로 통째로 사라졌다). 몇 초면 만드는 것이라 시험마다 새로 만든다.
★ 왜 진짜 자료를 쓰지 않나
  사용자의 보관 폴더는 시험에 쓰지 않는다(외장 디스크 · 클라우드 폴더 포함). 계정 이름 · 글 · 그림은
  모두 지어낸 것이고 바깥 주소는 example.invalid 뿐이다.
"""
from __future__ import annotations

import functools
import http.server
import struct
import subprocess
import threading
import time
import zlib
from pathlib import Path


def png(w: int, h: int, rgb) -> bytes:
    raw = b"".join(b"\x00" + bytes(rgb) * w for _ in range(h))

    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


COLORS = {"red": (200, 40, 40), "green": (40, 160, 60), "blue": (40, 80, 200),
          "lazy1": (200, 160, 40), "lazy2": (120, 40, 160), "bg": (230, 230, 240)}
NAV = ('<p class="nav"><a href="/index.html">처음</a> · <a href="/gallery.html">그림</a> · <a href="/long.html">긴 글</a>'
       ' · <a href="/sub/deep.html">깊은 곳</a> · <a href="/csp.html">CSP</a></p>')
STRICT_CSP = "default-src 'self'; script-src 'none'; style-src 'self'; img-src 'self'"


def _page(title: str, body: str) -> str:
    return ('<!doctype html><html lang="ko"><head><meta charset="utf-8"><title>%s</title>'
            '<link rel="stylesheet" href="/css/site.css"></head><body>%s<h1>%s</h1>%s</body></html>' % (title, NAV, title, body))


def build_site(root: Path) -> Path:
    """시험 사이트 — 처음 · 그림(지연 그림 셋) · 긴 글 · 깊은 곳 둘 · CSP 쪽 · 느린 그림 쪽."""
    s = Path(root)
    for d in ("img", "css", "sub"):
        (s / d).mkdir(parents=True, exist_ok=True)
    for name, col in COLORS.items():
        (s / "img" / (name + ".png")).write_bytes(png(120, 80, col))
    (s / "css" / "site.css").write_text(
        "body{font-family:Georgia,serif;margin:2em;background:url(../img/bg.png)} .box{border:2px solid #333;padding:1em} h1{color:#246}\n",
        encoding="utf-8")
    w = lambda rel, html: (s / rel).write_text(html, encoding="utf-8")  # noqa: E731
    w("index.html", _page("아카시 시험 사이트", '<div class="box"><img src="/img/red.png" alt="빨강"><p>이 맥 안에서만 열리는 시험 사이트입니다.</p></div>'))
    lazy = "".join('<div style="height:900px"></div><img loading="lazy" src="/img/lazy%d.png" alt="lazy%d">' % (i, i) for i in (1, 2))
    lazy += ('<div style="height:600px"></div><img class="js-lazy" data-src="/img/green.png" alt="js-lazy">'
             '<script>window.addEventListener("scroll",function(){document.querySelectorAll("img.js-lazy").forEach(function(i)'
             '{if(i.getBoundingClientRect().top<innerHeight)i.src=i.dataset.src;});});</script>')
    w("gallery.html", _page("그림 모음", '<img src="/img/blue.png" alt="파랑">' + lazy))
    w("long.html", _page("긴 글", "".join("<p>문단 %d — 한글과 日本語 와 English 가 섞인 긴 글입니다.</p>" % i for i in range(1, 3001))))
    w("sub/deep.html", _page("깊은 곳", '<p><a href="/sub/deeper.html">더 깊이</a></p><img src="../img/green.png">'))
    w("sub/deeper.html", _page("더 깊은 곳", '<p>여기까지.</p><p><a href="https://example.invalid/out">바깥 링크(따라가면 안 됨)</a></p>'))
    w("csp.html", _page("CSP 페이지", '<img src="/img/red.png"><p>엄격한 CSP 머리줄을 받는 페이지입니다.</p>'))
    slow = "".join('<img src="/slow/%s.png?n=%d" width=80>' % (("red", "blue", "green")[i % 3], i) for i in range(1, 9))
    w("slow.html", '<!doctype html><meta charset="utf-8"><title>느린 그림</title><h1>느린 그림 쪽</h1>%s'
                   '<p><a href="/index.html">처음</a></p>' % slow)
    return s


class SiteHandler(http.server.SimpleHTTPRequestHandler):
    """정적 파일 + 두 가지 장치.
    /slow/<그림> — 2초 늦게 준다(크롤러가 그림을 받는 도중에 멈추는 시험용)
    csp — {경로 앞부분: CSP 머리줄}. 기본은 /csp.html 에 엄격한 CSP."""
    csp = {"/csp.html": STRICT_CSP}

    def do_GET(self):
        if self.path.startswith("/slow/"):
            time.sleep(2)
            self.path = "/img/" + self.path[len("/slow/"):].split("?")[0]
        return super().do_GET()

    def end_headers(self):
        for prefix, value in self.csp.items():
            if self.path.startswith(prefix):
                self.send_header("Content-Security-Policy", value)
        super().end_headers()

    def log_message(self, *a):
        pass


class QuietServer(http.server.ThreadingHTTPServer):
    """크롤러 · Chrome 이 받다 만 연결(BrokenPipe · ConnectionReset)의 traceback 을 찍지 않는다 — 시험 기록이 그것으로 덮인다."""
    daemon_threads = True

    def handle_error(self, request, client_address):
        import sys
        if not isinstance(sys.exc_info()[1], ConnectionError):
            super().handle_error(request, client_address)


def serve(directory: Path, port: int = 0, csp: dict | None = None):
    """127.0.0.1 에만 여는 시험 서버(스레드). (서버, 포트)를 돌려준다 — 끝나면 server.shutdown()."""
    attrs = {"csp": dict(SiteHandler.csp, **(csp or {}))}
    handler = functools.partial(type("Handler", (SiteHandler,), attrs), directory=str(directory))
    srv = QuietServer(("127.0.0.1", port), handler)
    threading.Thread(target=srv.serve_forever, daemon=True).start()
    return srv, srv.server_address[1]


# 가짜 트위터 보관 폴더 — openpyxl 이 필요해 앱 번들의 파이썬으로 돌린다(표준 라이브러리 약속은 이 공구 쪽 이야기다).
_FAKEARCH = r'''
import sys, os, zlib, struct, openpyxl, datetime
R = sys.argv[1]
def png(w, h, rgb, path):
    raw = b''.join(b'\x00' + bytes(rgb) * w for _ in range(h))
    ck = lambda t, d: struct.pack('>I', len(d)) + t + d + struct.pack('>I', zlib.crc32(t + d) & 0xffffffff)
    open(path, 'wb').write(b'\x89PNG\r\n\x1a\n' + ck(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 2, 0, 0, 0))
                           + ck(b'IDAT', zlib.compress(raw)) + ck(b'IEND', b''))
for h, name, col in (('akashi_demo', '공작함 아카시', (29, 161, 242)), ('yubari_demo', '유바리', (23, 191, 99))):
    ud = os.path.join(R, h); os.makedirs(ud + '/media/_complete', exist_ok=True); os.makedirs(ud + '/captures', exist_ok=True)
    pd = os.path.join(ud, 'profiles', 'target', f'{name}(@{h})'); os.makedirs(pd, exist_ok=True)
    png(400, 400, col, pd + '/profile_20261003.png'); png(1500, 500, tuple(min(255, c + 60) for c in col), pd + '/banner_20261003.png')
    wb = openpyxl.Workbook(); ws = wb.active
    ws.append(["screen_name", "name", "description", "followers_count", "friends_count", "statuses_count", "location", "url",
               "created_at", "verified", "profile_image_url", "profile_banner_url"])
    ws.append([h, name, '예시 계정입니다. #시험 과 @yubari_demo 를 걸어 봅니다.\n둘째 줄.', 12345, 321, 4567, '서울',
               'https://example.invalid', '2012/04/01 09:00', 'false', '', ''])
    os.makedirs(os.path.join(ud, 'profiles', '20261003'), exist_ok=True); wb.save(os.path.join(ud, 'profiles', '20261003', h + '__profile.xlsx'))
    wb = openpyxl.Workbook(); ws = wb.active
    ws.append(["id", "tweet_url", "text", "language", "type", "author_name", "author_username", "bookmark_count", "favorite_count",
               "retweet_count", "retweeted", "reply_count", "quote_count", "view_count", "created_at", "retweet_time", "source",
               "hashtags", "urls", "media_type", "media_urls", "conversation_id", "in_reply_to", "is_quote", "quoted_tweet_url",
               "possibly_sensitive", "user_followers", "user_following", "user_verified", "user_profile_image"])
    base = datetime.datetime(2019, 3, 4, 15, 12)
    for i in range(120):
        tid = str(1100000000000000000 + i * 7919); t = base + datetime.timedelta(days=i * 9, minutes=i * 13)
        typ = 'Tweet'; reply = ''; mt = ''; text = f'{i}번째 글입니다 — 예전 트위터 모양 시험. https://example.invalid/{i} #시험'
        if i % 7 == 3: typ = 'Reply'; reply = 'yubari_demo'
        if i % 11 == 5: typ = 'Retweet'
        if i % 4 == 0:
            mt = 'photo'
            for k in range(1 + (i % 3)):
                png(600, 400, ((i * 40) % 255, (k * 90) % 255, 150),
                    os.path.join(ud, 'media', '_complete', f'{t:%Y%m%d_%H%M}_예시 본문{i}-{tid}-{k}(3_{tid}{k}).png'))
        if i % 9 == 2:
            mt = 'photo'; png(500, 500, (200, 120, (i * 20) % 255), os.path.join(ud, 'media', '_complete', f'{t:%Y%m%d_%H%M}_{tid}-0(3_{tid}).png'))
        ws.append([tid, f'https://x.com/{h}/status/{tid}', text, 'ko', typ, name, h, 0, i * 3, i, 'False', i % 5, 0, i * 100,
                   f'{t:%Y/%m/%d %H:%M}', '', '', '', '', mt, '', '', reply, 'False', '', 'False', 12345, 321, 'False', ''])
        open(os.path.join(ud, 'captures', f'{t:%Y%m%d_%H%M%S}_{tid}.html'), 'w').write('<html><body>캡처 예시 ' + tid + '</body></html>')
    wb.save(os.path.join(ud, h + '_complete.xlsx'))
print('ok')
'''


def build_fakearch(root: Path, python: Path) -> Path:
    """<root>/twitter/{akashi_demo,yubari_demo} — 프로필 엑셀 · 글 120개 엑셀 · 그림 · 캡처. python 은 openpyxl 이 있는 것."""
    import shutil
    tw = Path(root) / "twitter"
    shutil.rmtree(tw, ignore_errors=True)
    tw.mkdir(parents=True)
    r = subprocess.run([str(python), "-B", "-", str(tw)], input=_FAKEARCH, text=True, capture_output=True)
    if r.returncode != 0 or "ok" not in r.stdout:
        raise RuntimeError("가짜 보관 폴더를 만들지 못했습니다: " + (r.stderr or r.stdout).strip()[-300:])
    return tw
