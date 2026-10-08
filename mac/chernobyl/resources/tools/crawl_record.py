#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""끝까지 기록 — 앱형 사이트(트위터 · 미스키 …)를 끝까지 내리며, 페이지가 받은 글 자료(JSON)와 첨부 원본을 남긴다.

앱(経済産業省 크롤 탭 · 실제 Chrome · '끝까지 기록')이 부른다:
    python3 crawl_record.py --stdin-args        # 첫 줄 = 설정 JSON. 로그인 쿠키가 들어 있어 명령줄(ps)로 넘기지 않는다
손으로(시험 · 벤치):
    python3 crawl_record.py --url https://misskey.io/@tips --out /tmp/rec --chrome "<Chrome for Testing 경로>"

설정 JSON 칸: url · out_dir · chrome · profile_dir(없으면 임시) · cookies([CDP 쿠키]) · headful · wait_login ·
             login_check_js · max_seconds(1800) · max_clicks(3000) · download(true)
stdout 은 한 줄에 JSON 하나: {"ev": "log" | "progress" | "login_needed" | "done", ...}.
'login_needed' 를 낸 뒤에는 stdin 한 줄(continue)을 기다린다 — 사용자가 뜬 Chrome 창에서 로그인하고 앱의 확인을 누른다.
앱은 stdin 을 끝까지 열어 둔다. 닫히면(앱이 죽었거나 윈도우에서 중지) SIGTERM 과 같이 멈추고 Chrome 을 내린다.

남기는 것(out_dir):
  index.html  — 받은 글을 날짜순으로 보는 쪽(인터넷 없이 열린다)
  items.jsonl — 글 하나에 한 줄: id · 날짜 · 쓴이 · 본문 · 첨부(파일 이름)
  media/      — 첨부 원본
  api/        — 페이지가 받은 JSON 응답 그대로(다시 풀어 볼 때)
  report.json — 숫자 · 멈춘 까닭

★ 사이트별 코드가 없다. 규칙은 넷이다.
  1 본문 스크롤 상자 — 보이는 넓이가 가장 큰 스크롤 상자를 한 화면씩 내린다. 미스키는 문서가 아니라 안쪽 상자가
    스크롤되고(window.scrollBy 가 아무 일도 안 한다), 옆 칸(로컬 타임라인 위젯)도 따로 스크롤된다.
  2 '더 보기' 류 단추 — 본문 상자에 바로 딸리고, 고정(fixed · sticky) 칸 · aside · nav · 페이지 머리/꼬리 밖이며,
    아래쪽 40% 에 있는 것만 누른다. 같은 자리의 단추는 두 번까지만 — 트위터 옆 칸의 '더 보기' 를 아무 효과 없이
    103번 누르며 10분 상한까지 돈 적이 있다(2026-10-08 실측). 로그인하지 않은 미스키는 스크롤로는 다음 글을 부르지
    않고 이 단추를 눌러야 한다.
  3 글 — id · 날짜 · 본문 칸을 함께 가진 JSON 객체. 화면이 지나간 글을 지우는(가상 목록) 트위터도 응답에는 다 있다.
  4 첨부 — type · mimeType · content_type 이 image/video/photo/gif/audio 인 객체 안의 미디어 주소(변형이 여럿이면
    bitrate 가 가장 높은 것), 또는 fullsize · original 같은 이름의 칸. JSON 을 통째로 정규식으로 긁으면 이모지 ·
    아바타 · 배너가 수천 개 섞인다(미스키 api/emojis 하나에 수천 개).
  예외 하나 — ORIGINAL_RULES: 주소만으로는 원본이 아닌 곳. pbs.twimg.com 은 그냥 받으면 1200px 이다(원본 2018px).
★ 헤드리스가 기본이다 — 화면이 잠들면 macOS 가 창 그리기를 멈추고, 최소화된 창은 '보이면 불러오기' 가 돌지 않는다.
  헤드리스는 화면과 무관하게 그린다. 로그인해야 할 때만 창을 띄운다(wait_login).
★ 실측(2026-10-08): 미스키 @tips 47/47 · @notify 408/408(첨부 92 중 살아 있는 91 전부 — 하나는 2019년 서버가 없어졌다),
  X @neuralink 게시물 탭 끝까지(본인 글 168 · 첨부 94 전부). 예전 크롤러(보이지 않는 QWebEngine · 스크롤 5번)는 @tips
  47 중 15 에서, 실제 Chrome 모드(window 스크롤)도 첫 화면에서 멈췄다.
표준 라이브러리만 쓴다 — 앱 안의 '모듈 업데이트' 가 패키지를 바꿔도, 맥 기본 파이썬으로도 돈다.
"""
from __future__ import annotations

import argparse
import base64
import concurrent.futures
import datetime as dt
import email.utils
import hashlib
import html
import json
import os
import re
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import urllib.parse
import urllib.request

STOP = threading.Event()
LOGIN_GO = threading.Event()   # 앱이 stdin 에 'continue' 를 보냈다(로그인 확인)
STDIN_WATCHED = threading.Event()


def emit(ev: str, **kw) -> None:
    kw['ev'] = ev
    try:
        sys.stdout.write(json.dumps(kw, ensure_ascii=False) + '\n')
        sys.stdout.flush()
    except (BrokenPipeError, ValueError):   # 앱이 먼저 닫혔다 — 기록은 계속 디스크에 남긴다
        pass


def log(msg: str, level: str = 'info') -> None:
    emit('log', level=level, msg=msg)


# ── 작은 웹소켓(CDP 는 127.0.0.1 평문 · 텍스트 프레임뿐) — akashi/lib/ws.py 와 같은 짜임 ─────────────
class WsClosed(Exception):
    pass


class WebSocket:
    def __init__(self, url: str, timeout: float = 15.0):
        u = urllib.parse.urlparse(url)
        host, port = u.hostname or '127.0.0.1', u.port or 80
        path = (u.path or '/') + ('?' + u.query if u.query else '')
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.send_timeout = timeout
        self.buf = b''
        self.parts = []
        self.closed = False
        key = base64.b64encode(os.urandom(16)).decode()
        self.sock.sendall(('GET %s HTTP/1.1\r\nHost: %s:%d\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                           'Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\n\r\n' % (path, host, port, key)).encode())
        while b'\r\n\r\n' not in self.buf:
            c = self.sock.recv(65536)
            if not c:
                raise WsClosed('핸드셰이크 중 끊김')
            self.buf += c
        head, self.buf = self.buf.split(b'\r\n\r\n', 1)
        if b' 101 ' not in head.split(b'\r\n', 1)[0] + b' ':
            raise WsClosed('핸드셰이크 거절')

    def send(self, text: str) -> None:
        if self.closed:
            raise WsClosed('닫힌 연결')
        data = text.encode('utf-8')
        n = len(data)
        head = bytes([0x81]) + (bytes([0x80 | n]) if n < 126 else
                                bytes([0x80 | 126]) + struct.pack('!H', n) if n < 65536 else
                                bytes([0x80 | 127]) + struct.pack('!Q', n))
        mask = os.urandom(4)
        rep = (mask * (n // 4 + 1))[:n]
        masked = (int.from_bytes(data, 'big') ^ int.from_bytes(rep, 'big')).to_bytes(n, 'big') if n else b''
        prev = self.sock.gettimeout()
        self.sock.settimeout(self.send_timeout)   # 받을 때의 짧은 제한 시간으로 보내면 반쪽 프레임이 나간다
        try:
            self.sock.sendall(head + mask + masked)
        except OSError:
            self.closed = True
            raise
        finally:
            if not self.closed:
                self.sock.settimeout(prev)

    def _take(self):
        b = self.buf
        if len(b) < 2:
            return None
        n, pos = b[1] & 0x7F, 2
        if n == 126:
            if len(b) < 4:
                return None
            n, pos = struct.unpack('!H', b[2:4])[0], 4
        elif n == 127:
            if len(b) < 10:
                return None
            n, pos = struct.unpack('!Q', b[2:10])[0], 10
        if len(b) < pos + n:
            return None
        self.buf = b[pos + n:]
        return b[0] & 0x80, b[0] & 0x0F, b[pos:pos + n]

    def recv(self, timeout: float) -> str:
        """텍스트 메시지 하나. 시간이 다 되면 socket.timeout — 받던 조각은 버퍼에 남는다(다 온 프레임만 꺼낸다)."""
        if self.closed:
            raise WsClosed('닫힌 연결')
        self.sock.settimeout(max(0.001, timeout))
        while True:
            fr = self._take()
            if fr is None:
                c = self.sock.recv(1 << 20)
                if not c:
                    self.closed = True
                    raise WsClosed('연결이 끊겼습니다')
                self.buf += c
                continue
            fin, op, data = fr
            if op == 0x8:
                self.closed = True
                raise WsClosed('Chrome 이 닫았습니다')
            if op in (0x0, 0x1, 0x2):
                self.parts.append(data)
                if fin:
                    msg, self.parts = b''.join(self.parts), []
                    return msg.decode('utf-8', 'replace')

    def close(self) -> None:
        self.closed = True
        try:
            self.sock.close()
        except OSError:
            pass


class Cdp:
    def __init__(self, ws_url: str):
        self.ws = WebSocket(ws_url)
        self.mid = 0
        self.events: list = []

    def cmd(self, method: str, params: dict | None = None, timeout: float = 30.0) -> dict:
        self.mid += 1
        mid = self.mid
        self.ws.send(json.dumps({'id': mid, 'method': method, 'params': params or {}}))
        end = time.time() + timeout
        while True:
            left = end - time.time()
            if left <= 0:
                raise TimeoutError(method)
            try:
                msg = json.loads(self.ws.recv(left))
            except socket.timeout:
                continue
            if msg.get('id') == mid:
                if 'error' in msg:
                    raise RuntimeError('%s: %s' % (method, msg['error'].get('message')))
                return msg.get('result', {})
            if 'method' in msg:
                self.events.append(msg)

    def eval(self, expr: str, timeout: float = 30.0):
        r = self.cmd('Runtime.evaluate', {'expression': expr, 'returnByValue': True, 'awaitPromise': True}, timeout)
        if 'exceptionDetails' in r:
            d = r['exceptionDetails']
            raise RuntimeError(((d.get('exception') or {}).get('description') or d.get('text') or '?').splitlines()[0][:200])
        return (r.get('result') or {}).get('value')

    def pump(self, seconds: float) -> None:
        end = time.time() + seconds
        while not STOP.is_set():
            left = end - time.time()
            if left <= 0:
                return
            try:
                msg = json.loads(self.ws.recv(min(left, 0.5)))
            except socket.timeout:
                continue
            if 'method' in msg:
                self.events.append(msg)


# ── Chrome ────────────────────────────────────────────────────────────────────────────────────
def launch_chrome(chrome: str, profile: str, headful: bool):
    os.makedirs(profile, exist_ok=True)
    port_file = os.path.join(profile, 'DevToolsActivePort')
    try:
        os.remove(port_file)   # 지난번 것이 남아 있으면 엉뚱한 포트에 붙는다
    except OSError:
        pass
    args = [chrome, '--remote-debugging-port=0', '--user-data-dir=' + profile, '--no-first-run',
            '--no-default-browser-check', '--window-size=1280,1000', '--mute-audio',
            '--disable-background-timer-throttling', '--disable-renderer-backgrounding',
            '--disable-backgrounding-occluded-windows',
            # ★ 시크릿 창 — 쿠키를 디스크에 남기지 않는다(앱의 캡처 Chrome 과 같다). 남기려면 macOS 키체인의 암호 열쇠가
            #   필요한데, 홈이 다른 격리 사본에서는 그 열쇠를 찾다 첫 쪽이 끝내 열리지 않았다(실측 — 120초 넘게 멈춤).
            #   넣는 로그인(저장된 계정 · 쿠키 칸)은 판마다 다시 넣고, 창에서 직접 한 로그인은 그 판 동안만 간다.
            '--incognito']
    if not headful:
        args.append('--headless=new')
    proc = subprocess.Popen(args + ['about:blank'], stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    port = 0
    for _ in range(160):
        if proc.poll() is not None:
            raise RuntimeError('Chrome 이 바로 꺼졌습니다(같은 프로필을 쓰는 Chrome 이 이미 떠 있을 수 있습니다)')
        try:
            with open(port_file, encoding='utf-8') as f:
                first = f.read().split()
            if first:
                port = int(first[0])
                break
        except (OSError, ValueError):
            pass
        time.sleep(0.25)
    if not port:
        proc.kill()
        raise RuntimeError('Chrome 디버그 포트를 40초 안에 얻지 못했습니다')
    ws_url = ''
    for _ in range(40):
        try:
            with urllib.request.urlopen('http://127.0.0.1:%d/json/list' % port, timeout=3) as r:
                pages = [t for t in json.loads(r.read()) if t.get('type') == 'page']
            if pages:
                ws_url = pages[0]['webSocketDebuggerUrl']
                break
        except (OSError, ValueError):
            pass
        time.sleep(0.25)
    if not ws_url:
        proc.kill()
        raise RuntimeError('Chrome 탭을 찾지 못했습니다')
    return proc, ws_url


def stop_chrome(proc) -> None:
    if proc is None or proc.poll() is not None:
        return
    proc.terminate()
    try:
        proc.wait(8)
    except subprocess.TimeoutExpired:
        proc.kill()


# ── 페이지 안에서 도는 것 ─────────────────────────────────────────────────────────────────────
MAIN_JS = r"""window.__akMain = window.__akMain || (() => {
  const se = document.scrollingElement || document.documentElement;
  const scrolls = e => /(auto|scroll|overlay)/.test(getComputedStyle(e).overflowY) && e.scrollHeight > e.clientHeight + 50;
  const area = e => { const r = e.getBoundingClientRect();
    return Math.max(0, Math.min(r.right, innerWidth) - Math.max(r.left, 0)) * Math.max(0, Math.min(r.bottom, innerHeight) - Math.max(r.top, 0)); };
  let best = se.scrollHeight > se.clientHeight + 50 ? se : null, bestA = best ? innerWidth * innerHeight * 0.6 : 0;
  for (const e of document.querySelectorAll('body *')) {
    if (e.clientHeight < 200 || !scrolls(e)) continue;
    const a = area(e); if (a > bestA) { best = e; bestA = a; }
  }
  return best || se;
})();
window.__akScrollParent = window.__akScrollParent || (e => {
  for (let x = e.parentElement; x && x !== document.body && x !== document.documentElement; x = x.parentElement)
    if (/(auto|scroll|overlay)/.test(getComputedStyle(x).overflowY) && x.scrollHeight > x.clientHeight + 50) return x;
  return document.scrollingElement || document.documentElement;
});"""
RESET_JS = "if (!window.__akMain || !document.contains(window.__akMain)) window.__akMain = null;"
STEP_JS = """(async () => {
  %s
  %s
  const se = document.scrollingElement || document.documentElement, m = window.__akMain;
  m.scrollBy(0, Math.floor(m.clientHeight * 0.9));
  await Promise.race([new Promise(r => requestAnimationFrame(() => requestAnimationFrame(r))), new Promise(r => setTimeout(r, 1200))]);
  return [m.scrollHeight, Math.round(m.scrollTop + m.clientHeight), m === se ? 'document' : (m.className || m.tagName).toString().slice(0, 40)];
})()""" % (RESET_JS, MAIN_JS)
HEIGHT_JS = "(window.__akMain && document.contains(window.__akMain) ? window.__akMain : (document.scrollingElement || document.documentElement)).scrollHeight"
MORE_JS = r"""(() => {
  %s
  %s
  const re = /^(もっと見る|もっと読む|さらに(読み込む|表示)?|続きを(読む|見る)|再試行|load more|show more|see more|view more|more|retry|try again|더 ?보기|더 불러오기|다시 시도|이전 글|older( posts)?|next|次へ)$/i;
  const se = document.scrollingElement || document.documentElement, m = window.__akMain;
  const top = m === se ? 0 : m.getBoundingClientRect().top;
  const tried = window.__akTried || (window.__akTried = new Map());
  const pinned = e => { for (let x = e; x && x !== m && x !== document.body; x = x.parentElement) {
    const p = getComputedStyle(x).position; if (p === 'fixed' || p === 'sticky') return true; } return false; };
  const chrome = e => !!e.closest('aside, nav, [role="complementary"], [role="navigation"], [role="banner"], [role="contentinfo"]')
    || (() => { const h = e.closest('header, footer'); return !!h && !h.closest('article, section, main, [role="main"], [role="feed"]'); })();
  let best = null, bestY = -1, bestT = '';
  for (const b of document.querySelectorAll('button, a, [role="button"]')) {
    if (!b.offsetParent || b.disabled || b.getAttribute('aria-disabled') === 'true') continue;
    const t = (b.innerText || b.getAttribute('aria-label') || '').replace(/\s+/g, ' ').trim();
    if (!t || t.length > 24 || !re.test(t)) continue;
    if (chrome(b) || window.__akScrollParent(b) !== m || pinned(b)) continue;
    const y = b.getBoundingClientRect().bottom - top + m.scrollTop;
    if (y <= m.scrollHeight * 0.6) continue;
    if ((tried.get(t + '|' + Math.round(y / 40)) || 0) >= 2) continue;
    if (y > bestY) { best = b; bestY = y; bestT = t; }
  }
  if (!best) return '';
  const key = bestT + '|' + Math.round(bestY / 40);
  tried.set(key, (tried.get(key) || 0) + 1);
  best.scrollIntoView({block: 'center'}); best.click();
  return bestT;
})()""" % (RESET_JS, MAIN_JS)


# ── 글 · 첨부 알아보기(사이트별 해석 없음) ─────────────────────────────────────────────────────
ID_KEYS = ('id_str', 'rest_id', 'id', 'uri')
DATE_KEYS = ('createdAt', 'created_at', 'publishedAt', 'published_at', 'published', 'postedAt', 'posted_at',
             'indexedAt', 'date', 'timestamp')
TEXT_KEYS = ('full_text', 'text', 'content', 'body', 'caption', 'message')
NEST_KEYS = ('record', 'legacy', 'note', 'post', 'object', 'value')
AUTHOR_KEYS = ('screen_name', 'username', 'handle', 'acct')
# 쓴이는 이 이름의 칸만 따라가 찾는다 — 본문 속 '언급된 사람'(entities.user_mentions)을 쓴이로 집은 적이 있다
AUTHOR_HOPS = re.compile(r'^(user|author|account|owner|creator|core|user_results|result|by|profile|actor|poster|uploader)$')
MEDIA_EXT = r'(?:jpe?g|png|gif|webp|avif|heic|mp4|webm|mov|m4v|m4a|mp3|ogg|opus|wav)'
MEDIA_URL = re.compile(r'^https?://[^\s"<>]+?(?:\.' + MEDIA_EXT + r'|@(?:jpeg|png|webp))(?:[?#][^\s"<>]*)?$', re.I)
TYPE_KEYS = ('type', 'mimeType', 'mime_type', 'content_type', 'contentType', 'media_type', 'mediaType', '$type')
TYPE_VAL = re.compile(r'image|video|photo|animated_gif|gif|audio', re.I)
SKIP_KEY = re.compile(r'thumb|avatar|icon|banner|preview|blurhash|small|emoji|poster', re.I)
FULL_KEY = re.compile(r'^(fullsize|full|original|orig|large|download_url|video_url|media_url_https?)$', re.I)
ORIGINAL_RULES = [
    # (호스트, 경로 시작, 고치기) — 주소만으로 원본을 알 수 없는 곳만. 늘릴수록 '사이트별' 이 되니 아껴 쓴다.
    ('pbs.twimg.com', '/media/', lambda u: u.split('?')[0] + '?name=orig'),
    ('cdn.bsky.app', '/img/feed_thumbnail/', lambda u: u.replace('/img/feed_thumbnail/', '/img/feed_fullsize/')),
]


def _id(d: dict):
    for k in ID_KEYS:
        v = d.get(k)
        if isinstance(v, (str, int)) and not isinstance(v, bool) and str(v).strip():
            return str(v)
    return None


def _date_raw(d: dict):
    for k in DATE_KEYS:
        v = d.get(k)
        if isinstance(v, (str, int, float)) and not isinstance(v, bool) and str(v).strip():
            return v
    return None


def _has_text(d: dict) -> bool:
    return any(k in d and (d[k] is None or isinstance(d[k], str)) for k in TEXT_KEYS)


def is_item(d) -> bool:
    if not isinstance(d, dict) or _id(d) is None or _date_raw(d) is None:
        return False
    return _has_text(d) or any(isinstance(d.get(k), dict) and _has_text(d[k]) for k in NEST_KEYS)


def norm_date(v) -> str:
    try:
        if isinstance(v, (int, float)) or (isinstance(v, str) and v.isdigit()):
            x = float(v)
            return dt.datetime.fromtimestamp(x / 1000 if x > 1e12 else x, dt.timezone.utc).isoformat()
        s = str(v).strip()
        try:
            t = dt.datetime.fromisoformat(s.replace('Z', '+00:00'))
        except ValueError:
            t = email.utils.parsedate_to_datetime(s)   # 트위터: 'Wed Oct 10 20:19:24 +0000 2018'
        if t.tzinfo is None:
            t = t.replace(tzinfo=dt.timezone.utc)
        return t.astimezone(dt.timezone.utc).isoformat()
    except (TypeError, ValueError, OverflowError, IndexError):
        return str(v)


def item_text(d: dict) -> str:
    for src in [d] + [d[k] for k in NEST_KEYS if isinstance(d.get(k), dict)]:
        for k in TEXT_KEYS:
            if isinstance(src.get(k), str):
                return src[k]
    return ''


def _bfs(root, want, depth: int, skip, hops=None):
    """root 아래를 얕은 것부터 본다(인용 · 리노트 속 다른 글은 skip 이 막는다). hops 가 있으면 그 이름의 칸만 따라간다."""
    q = [(root, 0)]
    while q:
        node, dpt = q.pop(0)
        if isinstance(node, dict):
            r = want(node)
            if r:
                return r
            if dpt < depth:
                q += [(v, dpt + 1) for k, v in node.items() if isinstance(v, (dict, list)) and not skip(v)
                      and (hops is None or hops.match(k))]
        elif isinstance(node, list) and dpt < depth:
            q += [(v, dpt + 1) for v in node if isinstance(v, (dict, list)) and not skip(v)]
    return None


def item_author(d: dict, parent) -> str:
    def want(n):
        for k in AUTHOR_KEYS:
            v = n.get(k)
            if isinstance(v, str) and v and len(v) < 80:
                host = n.get('host')
                return '@' + v.lstrip('@') + ('@' + host if isinstance(host, str) and host else '')
        return None
    for root in (d, parent):
        if isinstance(root, dict):
            a = _bfs(root, lambda n: None if n is root else want(n), 6, lambda v: v is not d and is_item(v), AUTHOR_HOPS)
            if a:
                return a
    return ''


def longer_text(d: dict, parent, text: str) -> str:
    """잘린 본문(트위터 긴 글은 full_text 가 잘리고 옆 칸에 전문이 있다)을 같은 머리로 시작하는 더 긴 글로 바꾼다."""
    if not isinstance(parent, dict) or len(text) < 20:
        return text
    head = re.sub(r'\s+', ' ', text)[:30]

    def want(n):
        for k in TEXT_KEYS:
            v = n.get(k)
            if isinstance(v, str) and len(v) > len(text) + 5 and re.sub(r'\s+', ' ', v).startswith(head):
                return v
        return None
    return _bfs(parent, want, 6, lambda v: v is not d and is_item(v)) or text


def nested_items(d: dict, parent=None) -> list:
    """d 안에 바로 든 다른 글의 id(그 글 속으로는 더 들어가지 않는다). 감싼 객체(parent)의 낱칸도 본다 —
    트위터는 인용한 글을 본문(legacy) 밖 quoted_status_result 에 둔다. 목록 칸은 보지 않는다(미스키 사용자 객체의
    고정 노트 목록이 서로를 인용으로 잡는다)."""
    roots = [v for v in d.values() if isinstance(v, (dict, list))]
    if isinstance(parent, dict) and not is_item(parent):
        roots += [v for v in parent.values() if isinstance(v, dict) and v is not d]
    found, q = [], [(v, 1) for v in roots]
    while q:
        node, dpt = q.pop(0)
        if node is d:
            continue
        if isinstance(node, dict) and is_item(node):
            i = _id(node)
            if i not in found:
                found.append(i)
            continue
        if dpt < 6:
            vals = node.values() if isinstance(node, dict) else node
            q += [(v, dpt + 1) for v in vals if isinstance(v, (dict, list))]
    return found


def absolute(v: str, base: str) -> str:
    """JSON 속 주소가 '/media/a.png' · '//cdn/a.png' 처럼 반쪽이면 그 응답의 주소로 채운다."""
    if v.startswith('//'):
        return 'https:' + v
    if v.startswith('/') and base:
        return urllib.parse.urljoin(base, v)
    return v


def harvest_media(node, out: dict, stop=None, base: str = '') -> None:
    """out: 주소 → 종류. stop(객체) 가 참이면 그 아래는 다른 글의 것이라 들어가지 않는다. base: 그 응답의 주소."""
    if isinstance(node, dict):
        if stop is not None and stop(node):
            return
        tv = next((node[k] for k in TYPE_KEYS if isinstance(node.get(k), str) and TYPE_VAL.search(node[k])), None)
        moving = bool(tv) and re.search(r'video|animated_gif', tv, re.I) is not None   # 영상의 미리보기 그림은 첨부가 아니다
        for k, v in node.items():
            if not isinstance(v, str) or SKIP_KEY.search(k) or not (tv or FULL_KEY.match(k)):
                continue
            v = absolute(v, base)
            if MEDIA_URL.match(v):
                if moving and kind_of(v) == 'image':
                    continue
                out.setdefault(v, kind_of(v))
        for v in node.values():
            if isinstance(v, (dict, list)):
                harvest_media(v, out, stop, base)
    elif isinstance(node, list):
        rated = [x for x in node if isinstance(x, dict) and isinstance(x.get('bitrate'), (int, float))
                 and any(isinstance(x.get(k), str) and TYPE_VAL.search(x[k]) for k in TYPE_KEYS)]
        best = max(rated, key=lambda x: x['bitrate']) if len(rated) > 1 else None
        for x in node:
            if best is None or x is best or not any(x is r for r in rated):
                harvest_media(x, out, stop, base)


def kind_of(url: str) -> str:
    p = url.split('?')[0].lower()
    if re.search(r'\.(mp4|webm|mov|m4v)$', p):
        return 'video'
    if re.search(r'\.(m4a|mp3|ogg|opus|wav)$', p):
        return 'audio'
    return 'image'


def original_url(u: str) -> str:
    pu = urllib.parse.urlparse(u)
    for host, prefix, fix in ORIGINAL_RULES:
        if pu.hostname == host and pu.path.startswith(prefix):
            return fix(u)
    return u


# ── 기록기 ──────────────────────────────────────────────────────────────────────────────────
class Recorder:
    def __init__(self, cfg: dict):
        self.cfg = cfg
        self.url = cfg['url']
        self.out = cfg['out_dir']
        self.api_dir = os.path.join(self.out, 'api')
        self.media_dir = os.path.join(self.out, 'media')
        os.makedirs(self.api_dir, exist_ok=True)
        os.makedirs(self.media_dir, exist_ok=True)
        self.items: dict = {}        # id → 글
        self.media: dict = {}        # 주소 → 종류(글에 딸린 것 전부)
        self.page_media: dict = {}   # JSON 이 없는 사이트를 위한 물러설 자리 — 페이지가 받은 큰 그림 · 영상
        self.resp: dict = {}
        self.done_ids: set = set()
        self.n_json = 0
        self.clicks = 0
        self.scroller = ''
        self.stop_reason = ''
        self.ua = ''
        self.t0 = time.time()
        self.last_progress = 0.0
        self.cdp = None
        self.owner = ''

    # 응답 하나(JSON) — 저장하고 글 · 첨부를 꺼낸다
    def take_json(self, url: str, data: bytes) -> None:
        self.n_json += 1
        ep = re.sub(r'[^A-Za-z0-9_-]', '_', urllib.parse.urlparse(url).path.rstrip('/').rsplit('/', 1)[-1])[:40] or 'root'
        name = '%05d_%s.json' % (self.n_json, ep)
        with open(os.path.join(self.api_dir, name), 'wb') as f:
            f.write(data)
        with open(os.path.join(self.api_dir, '_index.tsv'), 'a', encoding='utf-8') as f:
            # 쿼리는 적지 않는다 — 어떤 API 는 열쇠 · 토큰을 주소에 싣는다
            f.write('%s\t%s\t%s\n' % (name, dt.datetime.now().isoformat(timespec='seconds'), url.split('?')[0]))
        try:
            doc = json.loads(data)
        except ValueError:
            return
        self._walk(doc, None, name, 0, url)

    def _walk(self, node, parent, src: str, depth: int, base: str = '') -> None:
        if depth > 60:
            return
        if isinstance(node, dict):
            if is_item(node):
                self._add_item(node, parent, src, base)
            for v in node.values():
                if isinstance(v, (dict, list)):
                    self._walk(v, node, src, depth + 1, base)
        elif isinstance(node, list):
            for v in node:
                if isinstance(v, (dict, list)):
                    self._walk(v, parent, src, depth + 1, base)

    def _add_item(self, d: dict, parent, src: str, base: str = '') -> None:
        iid = _id(d)
        got: dict = {}
        harvest_media(d, got, stop=lambda n: n is not d and is_item(n), base=base)
        refs = nested_items(d, parent)   # 리노트 · 리트윗 · 인용 — 안에 든 다른 글(그것도 따로 글로 남는다)
        for u, k in got.items():
            self.media.setdefault(u, k)
        text = longer_text(d, parent, item_text(d) or '')
        old = self.items.get(iid)
        if old:   # 같은 글이 여러 응답에 온다 — 더 많이 아는 쪽을 남긴다
            old['media'] = list(dict.fromkeys(old['media'] + list(got)))
            old['refs'] = list(dict.fromkeys(old['refs'] + refs))
            if len(text) > len(old['text']):
                old['text'] = text
            if not old['author']:
                old['author'] = item_author(d, parent)
            return
        self.items[iid] = {'id': iid, 'date': norm_date(_date_raw(d)), 'author': item_author(d, parent),
                           'text': text, 'media': list(got), 'refs': refs, 'src': src}

    def drain(self) -> None:
        evs, self.cdp.events = self.cdp.events, []
        for e in evs:
            m, p = e.get('method'), e.get('params', {})
            if m == 'Network.responseReceived':
                r = p.get('response', {})
                self.resp[p['requestId']] = (r.get('url', ''), (r.get('mimeType') or '').lower(), r.get('status', 0))
            elif m == 'Network.loadingFinished':
                rid = p['requestId']
                if rid in self.done_ids or rid not in self.resp:
                    continue
                self.done_ids.add(rid)
                u, mime, st = self.resp.pop(rid)
                if st != 200:
                    continue
                if 'json' in mime:
                    try:
                        b = self.cdp.cmd('Network.getResponseBody', {'requestId': rid}, 30)
                    except (RuntimeError, TimeoutError):   # 버퍼에서 밀려났다
                        continue
                    body = base64.b64decode(b['body']) if b.get('base64Encoded') else (b.get('body') or '').encode('utf-8')
                    self.take_json(u, body)
                elif (mime.startswith('image/') or mime.startswith('video/')) and p.get('encodedDataLength', 0) >= 50000:
                    self.page_media.setdefault(u, 'video' if mime.startswith('video/') else 'image')
            elif m == 'Network.loadingFailed':
                self.resp.pop(p.get('requestId'), None)
        self.progress()

    def progress(self, force: bool = False) -> None:
        now = time.time()
        if force or now - self.last_progress >= 5:
            self.last_progress = now
            emit('progress', sec=int(now - self.t0), items=len(self.items), media=len(self.media),
                 json=self.n_json, clicks=self.clicks)

    def open_page(self) -> None:
        c = self.cdp
        c.cmd('Network.enable', {'maxResourceBufferSize': 50_000_000, 'maxTotalBufferSize': 400_000_000})
        c.cmd('Page.enable')
        ua = (c.cmd('Browser.getVersion').get('userAgent') or '').replace('HeadlessChrome', 'Chrome')
        if ua:   # 헤드리스 표시를 달고 가면 막는 사이트가 있다
            c.cmd('Network.setUserAgentOverride', {'userAgent': ua})
            self.ua = ua
        cookies = self.cfg.get('cookies') or []
        if cookies:
            try:
                c.cmd('Network.setCookies', {'cookies': cookies})
                n_ok = len(cookies)
            except RuntimeError:   # 하나가 틀리면 묶음 전체가 거절된다 — 하나씩 넣고 틀린 것만 뺀다
                n_ok = 0
                for ck in cookies:
                    try:
                        c.cmd('Network.setCookie', ck)
                        n_ok += 1
                    except RuntimeError:
                        pass
            log('쿠키 %d/%d개 넣음(값은 적지 않음)' % (n_ok, len(cookies)), 'info' if n_ok == len(cookies) else 'warning')
        c.events.clear()
        self.navigate()

    def navigate(self) -> None:
        """주소를 연다. 새로 설치한 앱의 Chrome 은 처음 뜰 때 macOS 가 도우미 앱들을 살피느라 첫 쪽이 30초 넘게 걸린 적이 있다
        (격리 사본 실측) — 넉넉히 기다리고, 그래도 늦으면 멈추지 않고 이어서 지켜본다."""
        c = self.cdp
        try:
            c.cmd('Page.navigate', {'url': self.url}, 120)
        except TimeoutError:
            log('페이지가 120초 안에 열리지 않았습니다 — 계속 지켜봅니다', 'warning')
        self.t0 = time.time()
        for _ in range(16):
            if STOP.is_set():
                return
            c.pump(0.5)
            self.drain()

    def wait_login_if_needed(self) -> None:
        js = (self.cfg.get('login_check_js') or '').strip()
        if not js:
            return
        try:
            need = bool(self.cdp.eval(js, 15))
        except (RuntimeError, TimeoutError):
            need = False
        if not need:
            return
        if not self.cfg.get('headful'):
            log("로그인 화면입니다 — 로그인 쿠키를 넣거나 '로그인 후 확인' 을 켜고 다시 하십시오. 보이는 만큼만 기록합니다", 'warning')
            return
        emit('login_needed')
        log('Chrome 창에서 로그인한 뒤 앱의 로그인 확인을 누르십시오', 'warning')
        LOGIN_GO.clear()
        if not STDIN_WATCHED.is_set():   # 손으로 돌릴 때 — 엔터 한 번
            threading.Thread(target=lambda: (sys.stdin.readline(), LOGIN_GO.set()), daemon=True).start()
        while not LOGIN_GO.is_set() and not STOP.is_set():
            self.cdp.pump(0.5)
            self.cdp.events.clear()   # 로그인하는 동안의 응답은 기록하지 않는다
        if STOP.is_set():
            return
        log('로그인 확인 — 처음부터 다시 엽니다', 'success')
        self.items.clear()
        self.media.clear()
        self.page_media.clear()
        self.navigate()

    def ev(self, js: str, default=None):
        """페이지가 바뀌는 순간(다음 쪽 링크)에는 평가가 실패한다 — 잠깐 기다렸다 다시. 다섯 번 연달아면 포기."""
        for _ in range(5):
            try:
                return self.cdp.eval(js, 30)
            except (RuntimeError, TimeoutError):
                self.cdp.pump(2.0)
                self.drain()
        raise RuntimeError('페이지가 응답하지 않습니다')

    def scroll_loop(self) -> None:
        c = self.cdp
        max_s = float(self.cfg.get('max_seconds') or 1800)
        max_clicks = int(self.cfg.get('max_clicks') or 3000)
        same, prev = 0, None
        while True:
            if STOP.is_set():
                self.stop_reason = '중지'
                return
            if time.time() - self.t0 >= max_s:
                self.stop_reason = '시간 상한 %d초' % max_s
                return
            r = self.ev(STEP_JS)
            if not isinstance(r, list) or len(r) != 3:
                c.pump(1.0)
                self.drain()
                continue
            h, bottom, self.scroller = r
            c.pump(0.6)
            self.drain()
            if bottom < h - 4:
                continue
            c.pump(2.0)
            self.drain()
            h2 = self.ev(HEIGHT_JS)
            if h2 == h and self.clicks < max_clicks:
                n0 = len(self.items)
                label = self.ev(MORE_JS)
                if label:
                    self.clicks += 1
                    c.pump(2.5)
                    self.drain()
                    if self.ev(HEIGHT_JS) != h2 or len(self.items) != n0:
                        same, prev = 0, None
                        continue
            # 끝 판정은 본문 높이만 본다 — 옆 위젯 · 링크 미리보기 · 실시간 갱신은 JSON 을 계속 받는다
            same = same + 1 if h2 == prev else 0
            prev = h2
            if same >= 3:
                self.stop_reason = '끝'
                return

    # ── 받기 ──
    def download_all(self) -> dict:
        want = dict(self.media)
        fallback = False
        if not want and self.page_media:   # 글 자료(JSON)가 없는 사이트 — 페이지가 받은 큰 그림 · 영상이라도
            want = dict(self.page_media)
            fallback = True
        names: dict = {}
        used: dict = {}
        for u in want:
            ou = original_url(u)
            base = urllib.parse.unquote(urllib.parse.urlparse(u).path.rsplit('/', 1)[-1])
            base = re.sub(r'@(jpeg|png|webp)$', lambda m: '.' + ('jpg' if m.group(1) == 'jpeg' else m.group(1)), base)
            base = re.sub(r'[^\w.\-]', '_', base)[-120:] or 'media'
            if '.' not in base:
                base += '.jpg' if want[u] == 'image' else '.mp4'
            if used.get(base, ou) != ou:
                base = hashlib.md5(ou.encode()).hexdigest()[:8] + '_' + base
            used[base] = ou
            names[u] = (ou, base)
        saved, failed = {}, []

        def one(u):
            if STOP.is_set():
                return u, None
            ou, base = names[u]
            dst = os.path.join(self.media_dir, base)
            if os.path.exists(dst) and os.path.getsize(dst) > 0:
                return u, base
            for tryu in ([ou, u] if ou != u else [u]):
                tmp = dst + '.part'
                try:
                    req = urllib.request.Request(tryu, headers={'User-Agent': self.ua or 'Mozilla/5.0', 'Referer': self.url})
                    with urllib.request.urlopen(req, timeout=120) as r, open(tmp, 'wb') as f:
                        while True:
                            if STOP.is_set():
                                raise InterruptedError
                            chunk = r.read(1 << 20)
                            if not chunk:
                                break
                            f.write(chunk)
                    os.replace(tmp, dst)
                    return u, base
                except (OSError, ValueError, InterruptedError):
                    try:
                        os.remove(tmp)
                    except OSError:
                        pass
            return u, None
        total = len(names)
        done = 0
        with concurrent.futures.ThreadPoolExecutor(4) as ex:
            for u, base in ex.map(one, list(names)):
                done += 1
                if base:
                    saved[u] = base
                else:
                    failed.append(u)
                if done % 20 == 0 or done == total:
                    emit('progress', sec=int(time.time() - self.t0), items=len(self.items), media=total,
                         saved=len(saved), json=self.n_json, clicks=self.clicks, phase='download')
        if fallback:
            log('글 자료(JSON)가 없어 페이지가 받은 큰 그림 · 영상 %d개를 받았습니다' % len(saved))
        return {'saved': saved, 'failed': failed, 'fallback': fallback}

    # ── 쓰기 ──
    def write(self, dl: dict) -> dict:
        saved = dl.get('saved', {})
        items = sorted(self.items.values(), key=lambda x: x['date'], reverse=True)
        with open(os.path.join(self.out, 'items.jsonl'), 'w', encoding='utf-8') as f:
            for it in items:
                row = dict(it)
                row['media'] = [{'url': u, 'kind': self.media.get(u, 'image'),
                                 'file': ('media/' + saved[u]) if u in saved else None} for u in it['media']]
                f.write(json.dumps(row, ensure_ascii=False) + '\n')
        authors: dict = {}
        for it in items:
            authors[it['author']] = authors.get(it['author'], 0) + 1
        # 이 페이지의 주인 — 글이 가장 많은 쓴이가 30% 를 넘으면 그 사람으로 본다(미스키 옆 위젯 · X 추천의 남의 글을 가린다)
        top = max(authors.items(), key=lambda x: x[1]) if authors else ('', 0)
        self.owner = top[0] if top[0] and top[1] >= 0.3 * len(items) else ''
        report = {'url': self.url, 'recorded_at': dt.datetime.now().isoformat(timespec='seconds'),
                  'seconds': int(time.time() - self.t0), 'stop': self.stop_reason, 'scroller': self.scroller,
                  'items': len(items), 'json_responses': self.n_json, 'more_clicks': self.clicks,
                  'media_found': len(self.media) if not dl.get('fallback') else len(self.page_media),
                  'media_saved': len(saved), 'media_failed': len(dl.get('failed', [])),
                  'media_fallback': bool(dl.get('fallback')),
                  'owner': self.owner, 'owner_items': authors.get(self.owner, 0) if self.owner else 0,
                  'top_authors': sorted(authors.items(), key=lambda x: -x[1])[:10]}
        with open(os.path.join(self.out, 'report.json'), 'w', encoding='utf-8') as f:
            json.dump(report, f, ensure_ascii=False, indent=1)
        self.write_index(items, saved, authors, report, dl)
        return report

    def write_index(self, items, saved, authors, report, dl) -> None:
        e = html.escape
        pu = urllib.parse.urlparse(self.url)
        title = (pu.hostname or '') + (pu.path if pu.path != '/' else '')
        opts = ''.join('<option value="%s"%s>%s (%d)</option>' % (e(a), ' selected' if a == self.owner else '', e(a or '(쓴이 모름)'), n)
                       for a, n in sorted(authors.items(), key=lambda x: -x[1]))
        byid = {it['id']: it for it in items}

        def when(iso: str) -> str:
            try:   # 이 맥의 시간대로 보여 준다(items.jsonl 은 UTC 그대로)
                return dt.datetime.fromisoformat(iso).astimezone().strftime('%Y-%m-%d %H:%M')
            except ValueError:
                return iso[:16]

        def media_html(it) -> str:
            md = []
            for u in it['media']:
                f = saved.get(u)
                src = e('media/' + f) if f else e(u)
                k = self.media.get(u, 'image')
                if k == 'video':
                    md.append('<video controls preload="none" src="%s"></video>' % src)
                elif k == 'audio':
                    md.append('<audio controls preload="none" src="%s"></audio>' % src)
                else:
                    md.append('<a href="%s"><img loading="lazy" src="%s" alt=""></a>' % (src, src))
            return '<div class="md">%s</div>' % ''.join(md) if md else ''

        rows = []
        for it in items:
            quotes = ''.join('<blockquote><div class="meta">%s · %s</div>%s%s</blockquote>' % (
                e(when(r['date'])), e(r['author']), '<div class="tx">%s</div>' % e(r['text']) if r['text'] else '', media_html(r))
                for r in (byid.get(x) for x in it.get('refs', [])) if r)
            rows.append('<article class="it" data-a="%s"><div class="meta">%s · %s</div>%s%s%s</article>' % (
                e(it['author']), e(when(it['date'])), e(it['author'] or ''),
                '<div class="tx">%s</div>' % e(it['text']) if it['text'] else '', media_html(it), quotes))
        extra = ''
        if dl.get('fallback') and saved:
            parts = []
            for u, f in saved.items():
                if self.page_media.get(u) == 'video':
                    parts.append('<video controls preload="none" src="media/%s"></video>' % e(f))
                else:
                    parts.append('<a href="media/%s"><img loading="lazy" src="media/%s" alt=""></a>' % (e(f), e(f)))
            extra = '<section><h2>페이지가 받은 그림 · 영상</h2><div class="md">%s</div></section>' % ''.join(parts)
        doc = """<!doctype html><html lang="ko"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>기록 · %(title)s</title><style>
:root{color-scheme:light dark;--bg:#fbfbfd;--fg:#1d1d1f;--mut:#6e6e73;--line:#e3e3e8;--card:#f0f0f3}
@media (prefers-color-scheme:dark){:root{--bg:#121214;--fg:#ececf0;--mut:#9a9aa2;--line:#2a2a2e;--card:#1c1c20}}
body{margin:0;background:var(--bg);color:var(--fg);font:15px/1.6 -apple-system,"Hiragino Sans","Apple SD Gothic Neo",system-ui,sans-serif}
main{max-width:760px;margin:0 auto;padding:20px 16px 60px}h1{font-size:20px;margin:0 0 4px;word-break:break-all}
.head{border-bottom:1px solid var(--line);padding-bottom:12px}.meta{color:var(--mut);font-size:12.5px}
select{font:inherit;font-size:12.5px;margin-left:6px;max-width:60vw}.it{border-bottom:1px solid var(--line);padding:14px 0}
.tx{white-space:pre-wrap;word-break:break-word;margin:4px 0 6px}.md{display:grid;grid-template-columns:repeat(auto-fill,minmax(150px,1fr));gap:6px}
.md img,.md video{width:100%%;max-height:340px;object-fit:cover;border-radius:8px;background:var(--card);display:block}.md audio{width:100%%}
h2{font-size:15px;margin:24px 0 8px}a{color:inherit}
blockquote{margin:8px 0 0;padding:8px 12px;border:1px solid var(--line);border-radius:10px}blockquote .md{grid-template-columns:repeat(auto-fill,minmax(120px,1fr))}
</style></head><body><main><div class="head"><h1>%(title)s</h1>
<div class="meta">글 %(n)d · 첨부 %(m)d · %(when)s 기록 · <a href="%(url)s">원래 주소</a> · 쓴이<select id="au"><option value="">전체</option>%(opts)s</select></div></div>
%(rows)s%(extra)s</main><script>
(function(){var s=document.getElementById('au');function f(){var v=s.value;document.querySelectorAll('.it').forEach(function(a){a.hidden=!!v&&a.getAttribute('data-a')!==v;});}s.onchange=f;f();})();
</script></body></html>""" % {'title': e(title), 'n': len(items), 'm': len(saved), 'when': e(report['recorded_at'].replace('T', ' ')),
                            'url': e(self.url), 'opts': opts, 'rows': '\n'.join(rows), 'extra': extra}
        with open(os.path.join(self.out, 'index.html'), 'w', encoding='utf-8') as f:
            f.write(doc)


def run(cfg: dict) -> int:
    rec = Recorder(cfg)
    profile = cfg.get('profile_dir') or ''
    tmp_profile = not profile
    if tmp_profile:
        profile = tempfile.mkdtemp(prefix='crawl-record-')
    proc = None
    dl: dict = {}
    try:
        proc, ws_url = launch_chrome(cfg['chrome'], profile, bool(cfg.get('headful')))
        rec.cdp = Cdp(ws_url)
        log('기록 시작: %s (%s)' % (rec.url, '창' if cfg.get('headful') else '헤드리스'))
        rec.open_page()
        if not STOP.is_set():
            rec.wait_login_if_needed()
        if not STOP.is_set():
            rec.scroll_loop()
        else:
            rec.stop_reason = '중지'
        rec.drain()
    except Exception as ex:  # noqa: BLE001 — 무엇이 깨져도 받은 것까지는 남긴다
        rec.stop_reason = rec.stop_reason or ('중단: %s' % ex)
        log('기록 중단 — %s. 받은 것까지 남깁니다' % ex, 'warning')
    finally:
        if rec.cdp:
            rec.cdp.ws.close()
        stop_chrome(proc)
        if tmp_profile:
            shutil.rmtree(profile, ignore_errors=True)
    if cfg.get('download', True) and not STOP.is_set():
        log('첨부 원본 받기: %d개' % (len(rec.media) or len(rec.page_media)))
        dl = rec.download_all()
    report = rec.write(dl)
    emit('done', **report, out_dir=rec.out)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description='끝까지 기록 — 앱형 사이트의 글 자료와 첨부 원본')
    ap.add_argument('--stdin-args', action='store_true', help='첫 줄 = 설정 JSON(앱이 쓴다)')
    ap.add_argument('--url')
    ap.add_argument('--out')
    ap.add_argument('--chrome')
    ap.add_argument('--profile')
    ap.add_argument('--cookies-file', help='CDP 쿠키 JSON 파일 — 값은 찍지 않는다')
    ap.add_argument('--headful', action='store_true')
    ap.add_argument('--max-seconds', type=float, default=1800)
    ap.add_argument('--no-download', action='store_true')
    a = ap.parse_args()
    if a.stdin_args:
        cfg = json.loads(sys.stdin.readline() or '{}')

        def watch_stdin():
            # 앱은 stdin 을 끝까지 열어 둔다 — 닫히면(앱이 죽었거나 윈도우에서 중지) 멈추고 Chrome 을 내린다
            for line in sys.stdin:
                if line.strip() == 'continue':
                    LOGIN_GO.set()
            STOP.set()
        STDIN_WATCHED.set()
        threading.Thread(target=watch_stdin, daemon=True).start()
    else:
        if not (a.url and a.out and a.chrome):
            ap.error('--url · --out · --chrome 가 있어야 합니다(앱은 --stdin-args)')
        cfg = {'url': a.url, 'out_dir': a.out, 'chrome': a.chrome, 'profile_dir': a.profile or '',
               'headful': a.headful, 'max_seconds': a.max_seconds, 'download': not a.no_download}
        if a.cookies_file:
            with open(a.cookies_file, encoding='utf-8') as f:
                cfg['cookies'] = json.load(f)
    for k in ('url', 'out_dir', 'chrome'):
        if not cfg.get(k):
            emit('done', error='설정에 %s 가 없습니다' % k)
            return 2

    def on_term(*_):
        STOP.set()
    signal.signal(signal.SIGTERM, on_term)
    signal.signal(signal.SIGINT, on_term)
    return run(cfg)


if __name__ == '__main__':
    sys.exit(main())
