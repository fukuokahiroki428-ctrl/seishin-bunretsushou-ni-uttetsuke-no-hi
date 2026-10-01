#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""그림 찍기 — 고른 탭 × 폭 × 테마를 격리 사본에서 한 장씩 찍고, 한눈에 훑는 목록(contact.html)을 만든다.

    python3 akashi/iso.py start                                  # 먼저 격리 사본을 띄운다
    python3 akashi/shots.py                                      # 기능 탭 전부 + 설정 × 420·900·1180·1700 × 라이트·다크
    python3 akashi/shots.py --tabs twitter,settings --widths 900,1440x900 --themes dark
    python3 akashi/shots.py --tabs settings --groups each        # 설정은 '전체' 와 묶음 하나하나
    python3 akashi/shots.py --run-view --pages 3 --out /tmp/보기  # 좁음의 실행 화면도 · 스크롤해 아래쪽까지
    python3 akashi/shots.py --list                               # 찍을 수 있는 탭·설정 묶음만 보고 끝
    python3 akashi/iso.py stop

만드는 것 (--out, 기본은 지금 폴더의 akashi-shots-<시각>/):
  <탭>[-<묶음>][-run]_<폭>x<높이>_<테마>[_p<쪽>].png   그림 한 장씩
  contact.html   모든 그림을 탭별 격자로 — 테마·폭·탭·주의만 거르기, 누르면 크게(←→ 로 넘김, Esc 닫기)
  shots.json     잰 값(모드·열 수·옆 넘침·가린 칸 수·덮개·JS 예외)과 판정 — 기계용
  .gitignore(*)  이 폴더가 git 에 올라가지 않게 · .akashi-shots  이 공구가 만든 폴더라는 표식

찍으면서 재어 두는 것 (그림 밑에 적힌다):
  자동 배치 모드(좁음/보통/넓음)와 열 수 · 옆으로 넘친 px · 본 화면을 덮은 창(모달) · 그동안 난 JS 예외
  · 가린 비밀 칸 수 · 창 흉내가 먹었는지(화면 안쪽 폭·그림 화소 크기)
끝나면 창 크기 흉내·색 흉내·data-theme·ntTheme·설정 묶음(과 그 저장 값)·기능 탭의 보기(설정/실행)·
완료 점·탭·스크롤·가림 표식·찍기용 스타일을 모두 처음대로 되돌리고, 되돌린 것을 다시 읽어 확인한다.

종료 코드: 0 모두 찍고 되돌림(JS 예외 없음)
          1 문제 있음(못 찍은 그림·창 흉내가 안 먹음·JS 예외·되돌리기 못 함·도중에 앱이 사라짐
            · --strict 면 옆 넘침·덮개도)
          2 돌릴 수 없음(격리 사본 없음·포트 닫힘·화면 없음·잘못된 인자·쓸 수 없는 폴더·Ctrl-C)

★ 왜 격리 사본에만 붙나
  찍는 동안 화면의 localStorage(설정 묶음 hanishiki.settingsGroup, 테마 ntTheme)가 바뀔 수 있고, 그것은
  앱의 자료 폴더(프로필) 안에 산다. 사용자의 앱은 원격 디버깅 문을 열지 않으니 애초에 붙을 수도 없지만,
  포트가 열려 있다는 것만 믿지 않는다 — iso.py 의 기록(state.json)의 PID 가 살아 있고(실행 파일·뜬 시각
  대조) 그 PID(또는 자식)가 이 포트를 듣고 있을 때만 붙는다. 창이 여럿이면 본 창('window=' 없는 것)에만.

★ 왜 창을 움직이지 않고 폭을 '흉내' 내나
  앱은 창 폭을 배율로 나눠 보여 준다(MainWindow::zoomForWidth = 창폭/1180 을 0.80~1.60 로 묶음). 기본
  '둘 다' 모드에선 944~1888pt 창이 모두 화면 안쪽 1180px 근처로 보인다. 창 크기로는 원하는 폭을 고를 수
  없어서 CDP(Emulation.setDeviceMetricsOverride)로 화면 안쪽 폭(CSS px)을 바로 준다. --widths 의 숫자는
  그래서 '창 폭' 이 아니라 '앱이 보는 폭' 이다. 기본 네 폭은 좁음(420) · 보통(900) · 넓음 2열(1180,
  평소 창) · 넓음 3열(1700) 을 한 번씩 밟는다. 흉내가 먹었는지는 innerWidth 와 그림 화소로 다시 잰다.

★ 왜 테마를 두 겹으로 바꾸나
  CSS 는 <html data-theme> 만 보고, prefers-color-scheme 은 '자동' 을 고른 사람에게만 뜻이 있다(앱의
  matchMedia 가 바뀌면 applyTheme('auto') 로 data-theme 를 다시 쓴다). 그래서 색 흉내(p.dark)로 '자동'
  쪽이 우리와 같은 답을 내게 하고, data-theme 를 직접 써서 '라이트/다크' 로 고정한 사람도 바뀌게 한다.
  applyTheme() 는 부르지 않는다 — 그것은 ntTheme 을 저장하고 네이티브 제목 띠 색까지 바꾼다. 흉내 때문에
  앱이 스스로 그것을 불렀다면(자동인 사람) 끝낼 때 ntTheme 원래 글자·_chromeDark 까지 맞춰 둔다.

★ 왜 비밀 칸을 가리나
  --seed-config 로 띄운 사본은 진짜 계정 설정을 들고 있고, 트위터 auth_token·ct0, 팬박스 FANBOXSESSID 같은
  쿠키가 평범한 글 칸(type=text)에 그대로 보인다. 그림에 한 번 박히면 지울 길이 없다. 그래서 매 장 찍기
  직전에, 이름·id·placeholder 가 쿠키·세션·토큰·비밀번호·키를 가리키는 칸과 type=password 칸 가운데 값이
  있는 것을 빗금으로 덮는다(글자색 투명). 로그 줄에 'token=…' 꼴이 보이면 그 줄도 덮는다. 값은 화면 안에서
  '비었나' 만 보고, 이 공구로 돌려받지 않는다 — 돌려받는 것은 가린 칸의 '수' 뿐이다.

★ 왜 출력 폴더에 .gitignore(*) 를 넣고, 남의 폴더에는 쓰지 않나
  기본 출력 자리는 지금 폴더라 저장소 안(mac/chernobyl)이 되기 쉽다. 그림에는 계정 이름이 보일 수 있으니
  실수로 커밋되지 않게 폴더 안에 '*' 한 줄짜리 .gitignore 를 둔다(자기 자신까지 무시돼 git 에는 아예 안
  보인다). 그리고 비어 있지 않은 폴더는, 이 공구의 표식(.akashi-shots)이 없으면 쓰지 않는다 — 같은 이름의
  남의 파일을 덮어쓸 수 있어서다. 지우는 일은 하지 않는다(찍다 만 .part 조각만 치운다).

★ 왜 전환 효과를 끄고 '가라앉기' 를 기다리나
  테마를 바꾸면 배경색이 0.2초 동안 번져 가고, 크기를 바꾸면 ResizeObserver → requestAnimationFrame →
  fxAdapt → layoutFrames(벽돌 쌓기) 가 몇 틀에 걸쳐 돈다. 설정 탭은 열 때 백엔드에 물어 칸을 채운다.
  그래서 찍는 동안만 transition 을 끄고(깜빡이는 글 커서도 숨김), 탭·높이·배치 값이 두 번 연달아 같을
  때까지 기다린 뒤 찍는다. 끝내 멎지 않으면 찍기는 하되 '배치가 멎지 않음' 으로 적는다.

★ 왜 덮은 창(모달)은 기본으로 숨기지 않나
  첫 실행 안내·디스크 고르기 같은 창이 떠 있으면 그림이 다 가려진다. 그렇다고 '확인' 을 누르면 사본의
  설정이 바뀌고, 말없이 숨기면 사람이 실제로 보는 화면이 아니게 된다. 그래서 기본은 그대로 찍고 무엇이
  덮었는지 적는다. --hide-overlays 를 주면 찍는 동안만 display:none 으로 숨기고 끝날 때 되돌린다.
"""
from __future__ import annotations

import argparse
import base64
import html
import json
import os
import re
import struct
import subprocess
import sys
import time
from pathlib import Path
from urllib.parse import quote, urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))
import iso  # noqa: E402  격리 사본 기록(state.json)을 iso.py 와 같은 규칙으로 읽는다
from lib import cdp as cdplib  # noqa: E402
from lib import paths, proc  # noqa: E402
from lib.cdp import CdpError, JsError, Page  # noqa: E402
from lib.ws import WsError  # noqa: E402

TOOL = "shots"
MARK = ".akashi-shots"                 # 이 공구가 만든 폴더라는 표식 — 있어야 다시 쓴다
STYLE_ID = "akashi-shots-style"        # 찍는 동안만 넣는 <style>
K_THEME = "ntTheme"                    # index.html 의 테마 저장 키
K_GROUP = "hanishiki.settingsGroup"    # setSettingsGroup() 이 쓰는 키

DEFAULT_TABS = "fx,settings"
DEFAULT_WIDTHS = "420,900,1180,1700"
DEFAULT_HEIGHT = 860
MIN_W, MAX_W = 320, 3840
MIN_H, MAX_H = 320, 2400
MAX_PAGES = 10
OVERLAP = 48                           # 쪽을 넘길 때 겹쳐 보여 줄 px — 어디서 이어지는지 보이게
SETTLE_MAX = 4.0                       # 배치가 멎기를 기다리는 최대 초

THEME_ALIASES = {"light": "light", "라이트": "light", "밝음": "light", "밝게": "light",
                 "dark": "dark", "다크": "dark", "어두움": "dark", "어둡게": "dark"}
THEME_KO = {"light": "라이트", "dark": "다크"}
MODE_KO = {"wide": "넓음", "mid": "보통", "narrow": "좁음"}
VIEW_KO = {"set": "설정 보기", "run": "실행 화면"}

_NAME_RE = re.compile(r"[a-z][a-z0-9_-]{0,30}")

# 찍는 동안만 쓰는 스타일. 끝나면 요소째 지운다.
SHOT_CSS = """
/* akashi shots — 찍는 동안만 있다. 끝나면 지운다. */
*, *::before, *::after { transition: none !important; caret-color: transparent !important; }
[data-akashi-mask] {
  color: transparent !important; -webkit-text-fill-color: transparent !important;
  text-shadow: none !important; -webkit-text-security: disc !important;
  background-image: repeating-linear-gradient(135deg, rgba(127,127,127,.34) 0 5px, transparent 5px 10px) !important;
}
[data-akashi-mask] * { color: transparent !important; -webkit-text-fill-color: transparent !important; }
[data-akashi-mask]::placeholder { color: transparent !important; -webkit-text-fill-color: transparent !important; }
[data-akashi-mask]::selection { background: transparent !important; }
[data-akashi-hide] { display: none !important; }
"""


# ── 화면 쪽 도우미 — 매번 함수 안에 넣어 돌린다(전역에 아무것도 남기지 않는다) ─────────
JS_HEAD = r"""
var S = document.querySelector('.scroll-area');
var T = A.tab ? document.getElementById('tab-' + A.tab) : null;
function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
function lsPut(k, v) { try { if (v === null || v === undefined) localStorage.removeItem(k); else localStorage.setItem(k, v); } catch (e) {} }
function name(e) {
  if (!e || !e.tagName) return String(e);
  var s = e.tagName.toLowerCase();
  if (e.id) return s + '#' + e.id;
  var c = (typeof e.className === 'string') ? e.className.trim() : '';
  return c ? s + '.' + c.split(/\s+/).slice(0, 2).join('.') : s;
}
function ownText(e) {
  return [].filter.call(e.childNodes, function (n) { return n.nodeType === 3; })
           .map(function (n) { return n.textContent; }).join('').replace(/\s+/g, ' ').trim();
}
"""

JS_READY = """
return typeof switchTab === 'function' && !!S && !!document.querySelector('.tab-content[id^="tab-"]');
"""

# 처음 상태 — 되돌릴 것 전부와, 찍을 수 있는 탭·묶음 목록
JS_ORIG = r"""
var app = document.querySelector('.app'), act = document.querySelector('.tab-content.active');
var tabs = [].map.call(document.querySelectorAll('.tab-content[id^="tab-"]'), function (e) {
  return { name: e.id.slice(4), fx: e.classList.contains('fx') };
});
var groups = [], glabels = {};
[].forEach.call(document.querySelectorAll('#tab-settings .sf-chip[data-group]'), function (c) {
  var g = c.getAttribute('data-group'); groups.push(g); glabels[g] = ownText(c).slice(0, 40);
});
var chip = document.querySelector('#tab-settings .sf-chip.active');
var fxv = {}, done = [], running = [];
[].forEach.call(document.querySelectorAll('.tab-content.fx'), function (t) {
  var k = t.id.slice(4);
  fxv[k] = t.dataset.fxview || '';
  if (t.classList.contains('fx-done')) done.push(k);
  if (t.classList.contains('fx-running')) running.push(k);
});
return {
  adapt: typeof window.fxAdapt === 'function', canView: typeof window.fxSetView === 'function',
  canGroup: typeof setSettingsGroup === 'function',
  tabs: tabs, groups: groups, glabels: glabels,
  tab: (typeof currentTab !== 'undefined' && currentTab) ? String(currentTab) : (act ? act.id.slice(4) : ''),
  theme: document.documentElement.getAttribute('data-theme'),
  chromeDark: (typeof window._chromeDark === 'boolean') ? window._chromeDark : null,
  lsTheme: lsGet(A.kt), lsGroup: lsGet(A.kg),
  group: chip ? chip.getAttribute('data-group') : '',
  scroll: S ? S.scrollTop : 0,
  navOpen: !!(app && app.classList.contains('nav-open-narrow')),
  fxview: fxv, done: done, running: running,
  leftovers: !!document.getElementById(A.id) || !!document.querySelector('[data-akashi-mask], [data-akashi-hide]')
};
"""

JS_STYLE_ON = """
var st = document.getElementById(A.id);
if (!st) { st = document.createElement('style'); st.id = A.id; (document.head || document.documentElement).appendChild(st); }
st.textContent = A.css;
return 1;
"""

# 찍기용 스타일·가림·숨김 표식을 모두 걷는다(시작할 때 지난 찌꺼기에도, 끝날 때에도)
JS_CLEAN = """
var st = document.getElementById(A.id); if (st) st.remove();
var n = 0;
[].forEach.call(document.querySelectorAll('[data-akashi-mask]'), function (e) { e.removeAttribute('data-akashi-mask'); n++; });
[].forEach.call(document.querySelectorAll('[data-akashi-hide]'), function (e) { e.removeAttribute('data-akashi-hide'); n++; });
return n;
"""

JS_THEME = """
document.documentElement.setAttribute('data-theme', A.theme);
return document.documentElement.getAttribute('data-theme');
"""

# 자동 배치 모드는 모든 기능 탭에 같이 붙는다(fxAdapt) — 아무 기능 탭에서나 읽는다
JS_MODE = """
var f = document.querySelector('.tab-content.fx');
return f ? (f.dataset.fxmode || '') : '';
"""

JS_GO = """
if (!T) throw new Error('탭이 없습니다: tab-' + A.tab);
if (typeof currentTab === 'undefined' || currentTab !== A.tab || !T.classList.contains('active')) switchTab(A.tab);
if (A.group && typeof setSettingsGroup === 'function') {
  var c = document.querySelector('#tab-settings .sf-chip.active');
  if (!c || c.getAttribute('data-group') !== A.group) setSettingsGroup(A.group);
}
if (A.view && T.classList.contains('fx') && typeof fxSetView === 'function' && T.dataset.fxview !== A.view) fxSetView(T, A.view);
if (S) S.scrollTop = A.top || 0;
return T.classList.contains('active');
"""

JS_SCROLL = """
if (S) S.scrollTop = A.top;
return S ? Math.round(S.scrollTop) : 0;
"""

# 한 틀(또는 250ms) 뒤의 배치 지문 — 두 번 연달아 같으면 가라앉은 것
JS_SIG = """
var b = T && (T.querySelector(':scope > .fx-board') || T.querySelector('.settings-board'));
return new Promise(function (res) {
  var sent = false;
  function go() {
    if (sent) return; sent = true;
    res([T ? T.className : '', T ? (T.dataset.fxmode || '') : '', T ? (T.dataset.fxview || '') : '',
         S ? S.scrollHeight : 0, S ? S.scrollWidth : 0, S ? Math.round(S.scrollTop) : 0,
         b ? Math.round(b.getBoundingClientRect().height) : 0,
         document.fonts ? document.fonts.status : 'loaded', innerWidth, innerHeight]);
  }
  requestAnimationFrame(function () { requestAnimationFrame(go); });
  setTimeout(go, 250);   // 창이 가려져 틀이 멈춰 있어도 기다림이 끝나게
});
"""

# 찍기 직전: 비밀 칸·로그 줄 가리기 · 덮은 창 찾기(숨기기) · 테마 다시 확인
JS_PREP = r"""
var RX = /cookie|sess|token|ct0|auth|pass|secret|api.?key|akey|dtsg|\blsd\b|bearer|credential|private/i;
var KV = /(ct0|auth_token|sessionid|sessid|fanboxsessid|phpsessid|fb_dtsg|\blsd\b|csrftoken|token|cookie|password|passwd|api[_-]?key|secret|authorization)\s*[=:]\s*[^\s;&,'"<>]{8,}|\bbearer\s+\S{8,}/i;
var SKIP = { checkbox: 1, radio: 1, button: 1, submit: 1, reset: 1, range: 1, color: 1, file: 1, hidden: 1, image: 1 };
var fields = 0, lines = 0;
[].forEach.call(document.querySelectorAll('input, textarea'), function (e) {
  var t = (e.getAttribute('type') || '').toLowerCase();
  if (SKIP[t]) return;
  var hint = [e.id, e.getAttribute('name'), e.getAttribute('placeholder'),
              e.getAttribute('autocomplete'), e.getAttribute('aria-label')].join(' ');
  // 값은 '비었나' 만 본다 — 돌려주는 것은 수뿐
  var hide = (t === 'password' || RX.test(hint)) && e.value !== '';
  if (hide) { if (!e.hasAttribute('data-akashi-mask')) e.setAttribute('data-akashi-mask', ''); fields++; }
  else if (e.hasAttribute('data-akashi-mask')) e.removeAttribute('data-akashi-mask');
});
// 로그 줄·위 띠의 마지막 줄 — 'token=…' 꼴이 보이면 그 줄째 덮는다(글이 바뀌어 안 맞게 되면 걷는다)
[].forEach.call(document.querySelectorAll('.log-box > *, .fx-status, .sl-last'), function (e) {
  if (KV.test(e.textContent || '')) { if (!e.hasAttribute('data-akashi-mask')) e.setAttribute('data-akashi-mask', ''); lines++; }
  else if (e.hasAttribute('data-akashi-mask')) e.removeAttribute('data-akashi-mask');
});
var app = document.querySelector('.app');
function scan() {
  var pts = [[0.5, 0.5], [0.3, 0.35], [0.7, 0.65], [0.5, 0.22]], out = [];
  pts.forEach(function (p) {
    var h = document.elementFromPoint(innerWidth * p[0], innerHeight * p[1]);
    if (!h || !app || app.contains(h) || h === document.body || h === document.documentElement) return;
    var top = h;
    while (top.parentElement && top.parentElement !== document.body) top = top.parentElement;
    if (top.parentElement === document.body && out.indexOf(top) < 0) out.push(top);
  });
  return out;
}
var cur = scan(), tries = 0;
while (A.hide && cur.length && tries < 4) {   // 덮개 밑에 또 덮개가 있을 수 있다
  cur.forEach(function (e) { e.setAttribute('data-akashi-hide', ''); });
  cur = scan(); tries++;
}
// 앞 그림에서 숨긴 것도 그대로 숨어 있다 — 매 장 '지금 숨어 있는 것' 을 적는다
var hidden = [].map.call(document.querySelectorAll('[data-akashi-hide]'), name);
var de = document.documentElement, fixed = false;
if (A.theme && de.getAttribute('data-theme') !== A.theme) { de.setAttribute('data-theme', A.theme); fixed = true; }
return { fields: fields, lines: lines, overlays: cur.map(name), hidden: hidden, themeFixed: fixed };
"""

JS_MEASURE = r"""
var de = document.documentElement;
var board = T && T.querySelector(':scope > .fx-board');
var sb = T && T.querySelector('.settings-board');
var chip = document.querySelector('#tab-settings .sf-chip.active');
var tt = document.getElementById('toolbar-title');
var cols = '';
if (board) cols = board.dataset.cols || '';
else if (sb) {
  var xs = {};
  [].forEach.call(sb.querySelectorAll(':scope > .sg'), function (g) {
    if (!g.hidden && g.offsetParent) xs[Math.round(g.getBoundingClientRect().left)] = 1;
  });
  cols = String(Object.keys(xs).length);
}
return {
  active: !!(T && T.classList.contains('active')), fx: !!(T && T.classList.contains('fx')),
  label: tt ? tt.textContent.replace(/\s+/g, ' ').trim().slice(0, 60) : '',
  mode: (T && T.dataset.fxmode) || '', view: (T && T.dataset.fxview) || '', cols: cols,
  group: chip ? chip.getAttribute('data-group') : '',
  theme: de.getAttribute('data-theme') || '',
  overX: S ? Math.max(0, S.scrollWidth - S.clientWidth) : 0,
  docOverX: Math.max(0, de.scrollWidth - de.clientWidth),
  top: S ? Math.round(S.scrollTop) : 0, clientH: S ? S.clientHeight : 0, scrollH: S ? S.scrollHeight : 0,
  vw: innerWidth, vh: innerHeight
};
"""

JS_RESTORE_VIEWS = """
var out = [];
[].forEach.call(document.querySelectorAll('.tab-content.fx'), function (t) {
  var k = t.id.slice(4), v = A.fxview[k];
  if (v && t.dataset.fxview !== v && typeof fxSetView === 'function') { fxSetView(t, v); out.push(k); }
  // 실행 화면을 보면 앱이 완료 점(fx-done)을 '본 것' 으로 끈다 — 원래 켜져 있었으면 다시 켠다
  if (A.done.indexOf(k) >= 0 && !t.classList.contains('fx-done')) t.classList.add('fx-done');
});
return out;
"""

JS_RESTORE_GROUP = """
if (A.group && typeof setSettingsGroup === 'function') {
  var c = document.querySelector('#tab-settings .sf-chip.active');
  if (!c || c.getAttribute('data-group') !== A.group) setSettingsGroup(A.group);
}
lsPut(A.kg, A.ls);
return lsGet(A.kg);
"""

JS_RESTORE_THEME = """
var de = document.documentElement;
if (A.theme === null) de.removeAttribute('data-theme');
else if (de.getAttribute('data-theme') !== A.theme) de.setAttribute('data-theme', A.theme);
var chrome = false;
if (A.chrome !== null && window._chromeDark !== A.chrome) {
  window._chromeDark = A.chrome;
  try { if (window.backend && backend.setWindowChrome) { backend.setWindowChrome(A.chrome); chrome = true; } } catch (e) {}
}
lsPut(A.kt, A.ls);
return { theme: de.getAttribute('data-theme'), chrome: chrome };
"""

JS_RESTORE_TAB = """
if (A.orig && typeof switchTab === 'function'
    && (typeof currentTab === 'undefined' || currentTab !== A.orig)) switchTab(A.orig);
var app = document.querySelector('.app');
if (app && A.nav && !app.classList.contains('nav-open-narrow')) app.classList.add('nav-open-narrow');
if (S) S.scrollTop = A.scroll || 0;
return 1;
"""

JS_VERIFY = """
var de = document.documentElement, chip = document.querySelector('#tab-settings .sf-chip.active');
var fxv = {};
[].forEach.call(document.querySelectorAll('.tab-content.fx'), function (t) { fxv[t.id.slice(4)] = t.dataset.fxview || ''; });
return {
  tab: (typeof currentTab !== 'undefined' && currentTab) ? String(currentTab) : '',
  theme: de.getAttribute('data-theme'), lsTheme: lsGet(A.kt), lsGroup: lsGet(A.kg),
  group: chip ? chip.getAttribute('data-group') : '',
  chrome: (typeof window._chromeDark === 'boolean') ? window._chromeDark : null,
  style: !!document.getElementById(A.id),
  marks: document.querySelectorAll('[data-akashi-mask], [data-akashi-hide]').length,
  fxview: fxv
};
"""


# ── 비밀 가리기 — 화면 오류 글에 토큰이 섞여 나와도 찍지 않는다(check_adapt 와 같은 규칙) ──
_SECRET_KV = re.compile(
    r"(?i)\b(ct0|auth_token|sessionid|fanboxsessid|phpsessid|fb_dtsg|lsd|csrftoken|x-csrf-token|token|cookie|"
    r"password|passwd|api[_-]?key|secret|authorization)(\s*[=:]\s*)([^\s;&,'\"]+)")
_BEARER = re.compile(r"(?i)\b(bearer)(\s+)(\S+)")
_LONG = re.compile(r"[A-Za-z0-9%_\-+/=.]{32,}")


def scrub(text) -> str:
    s = str(text)
    s = s.replace(str(paths.real_home()), "~")
    s = _SECRET_KV.sub(lambda m: "%s%s<가림 %d자>" % (m.group(1), m.group(2), len(m.group(3))), s)
    s = _BEARER.sub(lambda m: "%s%s<가림 %d자>" % (m.group(1), m.group(2), len(m.group(3))), s)
    return _LONG.sub(lambda m: "<가림 %d자>" % len(m.group(0)), s)


# ── 인자 읽기 ─────────────────────────────────────────────────────────────────
def parse_sizes(spec: str, height: int) -> list:
    """'420,900x760,1700' → [(420, height), (900, 760), (1700, height)] — 순서 그대로, 겹침은 하나로."""
    out = []
    for item in (spec or "").split(","):
        s = item.strip().lower().replace("×", "x").replace("*", "x")
        if not s:
            continue
        m = re.fullmatch(r"(\d{3,4})(?:x(\d{3,4}))?", s)
        if not m:
            raise ValueError("크기 '%s' 를 읽지 못했습니다 — 900 이나 900x760 처럼 주십시오" % item.strip())
        w, h = int(m.group(1)), int(m.group(2) or height)
        if not (MIN_W <= w <= MAX_W):
            raise ValueError("폭 %d 은(는) %d~%d 사이여야 합니다" % (w, MIN_W, MAX_W))
        if not (MIN_H <= h <= MAX_H):
            raise ValueError("높이 %d 은(는) %d~%d 사이여야 합니다" % (h, MIN_H, MAX_H))
        if (w, h) not in out:
            out.append((w, h))
    if not out:
        raise ValueError("크기가 하나도 없습니다")
    return out


def parse_themes(spec: str) -> list:
    out = []
    for item in (spec or "").split(","):
        k = item.strip().lower()
        if not k:
            continue
        t = THEME_ALIASES.get(k)
        if not t:
            raise ValueError("테마 '%s' 는 모릅니다 — light, dark (또는 라이트, 다크)" % item.strip())
        if t not in out:
            out.append(t)
    if not out:
        raise ValueError("테마가 하나도 없습니다")
    return out


def split_names(spec: str, what: str) -> list:
    """쉼표 목록을 소문자 이름으로 — 모양만 본다(있는지는 화면에 붙은 뒤에)."""
    out = []
    for item in (spec or "").split(","):
        k = item.strip().lower()
        if not k:
            continue
        if not _NAME_RE.fullmatch(k):
            raise ValueError("%s '%s' 는 영어 소문자 이름이어야 합니다(예: twitter)" % (what, item.strip()))
        if k not in out:
            out.append(k)
    if not out:
        raise ValueError("%s 이(가) 하나도 없습니다" % what)
    return out


def resolve_tabs(names: list, avail: list) -> list:
    """fx = 기능 탭 전부, all = 모든 탭. 없는 이름은 ValueError(있는 탭 목록과 함께)."""
    have = [t["name"] for t in avail if _NAME_RE.fullmatch(t.get("name") or "")]
    fx = [t["name"] for t in avail if t.get("fx") and t["name"] in have]
    out, unknown = [], []
    for k in names:
        if k == "fx":
            add = fx
        elif k == "all":
            add = have
        elif k in have:
            add = [k]
        else:
            unknown.append(k)
            continue
        out.extend(x for x in add if x not in out)
    if unknown:
        raise ValueError("화면에 없는 탭: %s\n  있는 탭 — 기능: %s\n            그 밖: %s"
                         % (", ".join(unknown), " ".join(fx) or "(없음)",
                            " ".join(x for x in have if x not in fx) or "(없음)"))
    return out


def resolve_groups(names: list, avail: list) -> list:
    """each = 전체와 묶음 하나하나. 묶음 단추가 없는 옛 빌드면 [None](있는 그대로 한 장)."""
    avail = [g for g in avail if _NAME_RE.fullmatch(g or "")]
    if not avail:
        if names == ["all"]:
            return [None]
        raise ValueError("이 빌드의 설정 탭에는 묶음 단추(.sf-chip)가 없습니다 — --groups all 만 됩니다")
    out, unknown = [], []
    for k in names:
        if k == "each":
            add = (["all"] if "all" in avail else []) + [g for g in avail if g != "all"]
        elif k in avail:
            add = [k]
        else:
            unknown.append(k)
            continue
        out.extend(x for x in add if x not in out)
    if unknown:
        raise ValueError("설정에 없는 묶음: %s — 있는 묶음: %s" % (", ".join(unknown), " ".join(avail)))
    return out


def image_size(data: bytes):
    """PNG·JPEG 머리에서 (가로, 세로) 화소. 모르면 None."""
    if data[:8] == b"\x89PNG\r\n\x1a\n" and data[12:16] == b"IHDR":
        return struct.unpack(">II", data[16:24])
    if data[:2] == b"\xff\xd8":
        i = 2
        while i + 9 < len(data):
            if data[i] != 0xFF:
                return None
            mk = data[i + 1]
            if mk == 0xFF:               # 채움 바이트
                i += 1
                continue
            if mk in (0xD8, 0x01) or 0xD0 <= mk <= 0xD7:
                i += 2
                continue
            seg = struct.unpack(">H", data[i + 2:i + 4])[0]
            if 0xC0 <= mk <= 0xCF and mk not in (0xC4, 0xC8, 0xCC):
                h, w = struct.unpack(">HH", data[i + 5:i + 9])
                return (w, h)
            i += 2 + seg
    return None


# ── 출력 폴더 ────────────────────────────────────────────────────────────────
def check_out(spec):
    """(폴더, 오류 글). 아직 만들지는 않는다 — 앱에 붙어 찍을 것이 정해진 뒤에 만든다."""
    if spec:
        out = Path(spec).expanduser()
        out = out if out.is_absolute() else Path.cwd() / out
    else:
        out = Path.cwd() / ("akashi-shots-" + time.strftime("%Y%m%d-%H%M%S"))
    out = Path(os.path.abspath(str(out)))
    real = Path(os.path.realpath(str(out)))      # 없는 경로도 있는 앞부분까지는 링크를 따라간다
    dd = paths.data_dir().resolve()
    if real == dd or dd in real.parents:
        return None, "사용자 자료 폴더 안에는 쓰지 않습니다: " + paths.short(out)
    if any(part.endswith(".app") for part in real.parts):
        return None, "앱 번들(.app) 안에는 쓰지 않습니다: " + paths.short(out)
    if out.exists():
        if not out.is_dir():
            return None, "폴더가 아니라 파일입니다: " + paths.short(out)
        try:
            busy = any(True for _ in out.iterdir())
        except OSError as e:
            return None, "폴더를 읽지 못했습니다 (%s): %s" % (e.strerror or e, paths.short(out))
        if busy and not (out / MARK).exists():
            return None, ("비어 있지 않은 폴더입니다 — 같은 이름의 파일을 덮어쓸 수 있어 쓰지 않습니다: %s\n"
                          "  새 폴더 이름을 주거나 --out 을 빼십시오(시각이 붙은 새 폴더를 만듭니다)." % paths.short(out))
    return out, None


def make_out(out: Path) -> None:
    out.mkdir(parents=True, exist_ok=True)
    (out / MARK).write_text("akashi shots — shots.py 가 만든 그림 폴더입니다. 다시 찍으면 같은 이름을 덮어씁니다.\n", "utf-8")
    gi = out / ".gitignore"
    if not gi.exists():
        gi.write_text("# akashi shots — 그림에 계정 이름이 보일 수 있어 git 에 올리지 않는다(이 파일까지)\n*\n", "utf-8")


# ── 격리 사본인지 (check_adapt.py 와 같은 규칙) ─────────────────────────────────
def guard(port: int):
    """(오류 글, 기록). 붙어도 되는 격리 사본이면 오류 글이 None."""
    st = iso.load_state(port)
    if not st:
        return ("포트 %d 로 띄운 격리 사본 기록이 없습니다. 먼저: python3 akashi/iso.py start --port %d" % (port, port)), None
    if not iso.running(st):
        return ("기록된 격리 사본(PID %s)이 떠 있지 않습니다. 먼저: python3 akashi/iso.py start --port %d"
                % (st.get("pid"), port)), None
    if not proc.port_open(port):
        return ("격리 사본은 떠 있지만 포트 %d 가 닫혀 있습니다(화면이 아직 준비 중이거나 원격 디버깅이 꺼짐)." % port), None
    lp = proc.listening_pids(port)
    pid = int(st["pid"])
    # ★ 듣는 PID 를 못 보면(lsof 실패·다른 사용자 프로세스) 격리 사본인지 모른다 — check_adapt 처럼 닫힌 쪽으로
    if not lp:
        return ("포트 %d 를 누가 듣는지 확인하지 못했습니다(lsof 가 답하지 않음) — 격리 사본인지 모르니 붙지 않습니다."
                % port), None
    if pid not in lp and not (set(lp) & set(proc.children(pid))):
        return ("포트 %d 를 격리 사본(PID %d)이 아닌 프로세스(PID %s)가 듣고 있습니다 — 붙지 않습니다."
                % (port, pid, ", ".join(str(x) for x in lp))), None
    return None, st


class Abort(Exception):
    """앱과 연결이 끊겼다 — 더 할 수 없다."""


# ── 찍기 ─────────────────────────────────────────────────────────────────────
class Shooter:
    def __init__(self, p: Page, a, st: dict, orig: dict):
        self.p, self.a, self.st, self.orig = p, a, st, orig
        self.fx = {t["name"] for t in orig.get("tabs") or [] if t.get("fx")}
        self.glabels = orig.get("glabels") or {}
        self.tabs: list = []
        self.groups: list = [None]
        self.sizes: list = []
        self.themes: list = []
        self.out: Path | None = None
        self.shots: list = []
        self.exc: list = []
        self.console = 0
        self.restore_notes: list = []
        self.notes: list = []
        self.touched = set()           # 무엇을 건드렸나 — 건드린 것만 되돌린다
        self.t0 = time.time()

    # ── 말하기 ───────────────────────────────────────────────────────────
    def say(self, line: str = "") -> None:
        if not self.a.json:
            print(line, flush=True)

    # ── 화면 다루기 ──────────────────────────────────────────────────────
    def js(self, body: str, **kw):
        kw.setdefault("tab", "")
        expr = "(function(A){%s\n%s\n})(%s)" % (JS_HEAD, body, json.dumps(kw, ensure_ascii=False))
        try:
            return self.p.eval(expr)
        except (WsError, OSError) as e:
            raise Abort("앱과 연결이 끊겼습니다 (%s)" % e)

    def cmd(self, method: str, params: dict | None = None, timeout: float | None = None):
        try:
            return self.p.cmd(method, params, timeout)
        except (WsError, OSError) as e:
            raise Abort("앱과 연결이 끊겼습니다 (%s)" % e)

    def drain_errors(self):
        """지난번 이후 모인 JS 예외·console.error — 모은 이벤트는 비운다(오래 돌아도 쌓이지 않게)."""
        exc, other = [], []
        for e in self.p.collect_errors():
            (exc if e.startswith("예외") else other).append(scrub(e))
        del self.p.events[:]
        self.exc.extend(exc)
        self.console += len(other)
        return exc, other

    def settle(self, tab: str) -> bool:
        """배치 지문이 두 번 연달아 같을 때까지. 끝내 흔들리면 False(찍기는 한다)."""
        last, same = None, 0
        end = time.time() + SETTLE_MAX
        while time.time() < end:
            v = self.js(JS_SIG, tab=tab)
            if v == last:
                same += 1
                if same >= 2:
                    return True
            else:
                last, same = v, 0
            time.sleep(0.06)
        return False

    def set_size(self, w: int, h: int) -> None:
        self.touched.add("size")
        self.cmd("Emulation.setDeviceMetricsOverride",
                 {"width": int(w), "height": int(h), "deviceScaleFactor": self.a.dpr, "mobile": False})
        time.sleep(self.a.settle)

    def set_theme(self, theme: str) -> None:
        self.touched.add("theme")
        try:
            self.p.dark(theme == "dark")
        except (WsError, OSError) as e:
            raise Abort("앱과 연결이 끊겼습니다 (%s)" % e)
        time.sleep(0.15)      # '자동' 인 사람: 앱의 matchMedia 가 먼저 한 번 돌게
        got = self.js(JS_THEME, theme=theme)
        if got != theme:
            self.notes.append("data-theme 를 %s 로 바꾸지 못했습니다(%s)" % (theme, got))
        time.sleep(self.a.settle)

    def capture(self) -> bytes:
        params = {"format": self.a.format}
        if self.a.format == "jpeg":
            params["quality"] = self.a.quality
        r = self.cmd("Page.captureScreenshot", params, timeout=45)
        data = base64.b64decode(r.get("data") or "")
        if not data:
            raise CdpError("그림이 비어 왔습니다")
        return data

    # ── 한 장 ────────────────────────────────────────────────────────────
    def file_name(self, tab: str, group, view, page: int, w: int, h: int, theme: str) -> str:
        stem = tab
        if group and group != "all":
            stem += "-" + group
        if view == "run":
            stem += "-run"
        ext = "jpg" if self.a.format == "jpeg" else "png"
        return "%s_%dx%d_%s%s.%s" % (stem, w, h, theme, ("_p%d" % page) if page > 1 else "", ext)

    def shoot(self, tab: str, group, view, page: int, w: int, h: int, theme: str) -> dict:
        fname = self.file_name(tab, group, view, page, w, h, theme)
        rec = {"file": fname, "tab": tab, "label": tab, "group": group or "", "view": view or "", "page": page,
               "width": w, "height": h, "theme": theme, "ok": False, "why": "",
               "warn": [], "bad": [], "notes": [], "masked": 0, "maskedLines": 0}
        try:
            if page == 1:
                active = self.js(JS_GO, tab=tab, group=group or "", view=view or "", top=0)
                if group:
                    self.touched.add("group")
                if view:
                    self.touched.add("views")
                if not active:
                    raise CdpError("switchTab('%s') 뒤에도 탭이 열리지 않았습니다" % tab)
            stable = self.settle(tab)
            prep = self.js(JS_PREP, tab=tab, theme=theme, hide=bool(self.a.hide_overlays)) or {}
            if prep.get("hidden"):
                self.touched.add("hide")
            if prep.get("fields") or prep.get("lines"):
                self.touched.add("mask")
            self.js(JS_SIG, tab=tab)               # 가림·숨김이 그려질 한 틀
            m = self.js(JS_MEASURE, tab=tab) or {}
            data = self.capture()
            path = self.out / fname
            part = path.with_name(path.name + ".part")
            with open(str(part), "wb") as f:
                f.write(data)
            os.replace(str(part), str(path))
            px = image_size(data)
            want = (int(round(w * self.a.dpr)), int(round(h * self.a.dpr)))
            rec.update({
                "ok": True, "label": m.get("label") or tab, "mode": m.get("mode") or "", "fxview": m.get("view") or "",
                "cols": m.get("cols") or "", "themeSeen": m.get("theme") or "", "overX": int(m.get("overX") or 0),
                "docOverX": int(m.get("docOverX") or 0), "top": int(m.get("top") or 0),
                "clientH": int(m.get("clientH") or 0), "scrollH": int(m.get("scrollH") or 0),
                "vw": m.get("vw"), "vh": m.get("vh"), "px": list(px) if px else None, "bytes": len(data),
                "masked": int(prep.get("fields") or 0), "maskedLines": int(prep.get("lines") or 0),
                "overlays": prep.get("overlays") or [], "hidden": prep.get("hidden") or [], "stable": stable,
            })
            rec["atBottom"] = rec["top"] + rec["clientH"] >= rec["scrollH"] - 2
            if not m.get("active"):
                rec["bad"].append("탭 %s 이(가) 열려 있지 않음" % tab)
            if m.get("vw") != w or m.get("vh") != h:
                rec["bad"].append("창 흉내가 먹지 않음 — 화면 안쪽 %sx%s (기대 %dx%d)" % (m.get("vw"), m.get("vh"), w, h))
            elif px and tuple(px) != want:
                rec["warn"].append("그림 %dx%d 화소 (기대 %dx%d)" % (px[0], px[1], want[0], want[1]))
            if m.get("theme") != theme:
                rec["warn"].append("테마가 %s 로 남음" % (m.get("theme") or "없음"))
            if rec["overX"] > 1:
                rec["warn"].append("옆 넘침 %dpx" % rec["overX"])
            elif rec["docOverX"] > 1:
                rec["warn"].append("문서 옆 넘침 %dpx" % rec["docOverX"])
            if rec["overlays"]:
                rec["warn"].append("덮개: %s" % ", ".join(rec["overlays"][:3]))
            if not stable:
                rec["warn"].append("배치가 멎지 않음")
            if rec["hidden"]:
                rec["notes"].append("덮개 숨김: %s" % ", ".join(rec["hidden"][:3]))
            if prep.get("themeFixed"):
                rec["notes"].append("찍기 직전 data-theme 를 다시 맞춤")
        except JsError as e:
            rec["why"] = "화면 JS 오류 — " + scrub(e)
        except CdpError as e:
            rec["why"] = scrub(e)
        except OSError as e:     # 그림 파일을 쓰지 못함(디스크 가득 등) — 연결 문제가 아니다
            rec["why"] = "그림을 저장하지 못했습니다 — %s" % (e.strerror or e)
        exc, other = self.drain_errors()
        rec["exceptions"] = exc
        rec["consoleErrors"] = len(other)
        self.shots.append(rec)
        if self.a.verbose:
            self.say("    %s" % self.line(rec))
        return rec

    def line(self, r: dict) -> str:
        if not r["ok"]:
            return "%-34s 못 찍음 — %s" % (r["file"], r["why"])
        bits = []
        if r.get("mode"):
            bits.append("%s %s열" % (MODE_KO.get(r["mode"], r["mode"]), r.get("cols") or "?"))
        elif r.get("cols"):
            bits.append("%s열" % r["cols"])
        if r.get("masked") or r.get("maskedLines"):
            bits.append("가림 %d" % (r["masked"] + r["maskedLines"]))
        bits += r["bad"] + r["warn"]
        if r.get("exceptions"):
            bits.append("JS 예외 %d" % len(r["exceptions"]))
        return "%-34s %s" % (r["file"], " · ".join(bits) or "-")

    def shoot_pages(self, tab: str, group, view, w: int, h: int, theme: str) -> list:
        recs, step = [], 0
        for k in range(1, self.a.pages + 1):
            if k > 1:
                last = recs[-1]
                if not last["ok"] or last.get("atBottom"):
                    break
                try:
                    got = self.js(JS_SCROLL, top=(k - 1) * step)
                except CdpError as e:     # 스크롤이 안 되면 그 탭의 아래쪽만 못 찍는다 — 다음 탭은 간다
                    last["notes"].append("아래쪽으로 넘기지 못함 — " + scrub(e)[:160])
                    break
                if (got or 0) <= last.get("top", 0) + 1:
                    break
            r = self.shoot(tab, group, view, k, w, h, theme)
            recs.append(r)
            if k == 1:
                step = max(120, (r.get("clientH") or h) - OVERLAP)
        return recs

    # ── 되돌리기 ─────────────────────────────────────────────────────────
    def restore(self) -> bool:
        o = self.orig
        ok = True

        def step(label: str, fn) -> None:
            nonlocal ok
            try:
                fn()
            except Abort:
                raise
            except Exception as e:  # 하나가 안 돼도 나머지는 되돌린다
                ok = False
                self.restore_notes.append("%s 못 함 — %s" % (label, scrub(e)[:200]))

        step("가림·숨김 표식과 찍기용 스타일 걷기", lambda: self.js(JS_CLEAN, id=STYLE_ID))
        if "views" in self.touched:
            step("기능 탭의 보기(설정/실행)·완료 점", lambda: self.js(
                JS_RESTORE_VIEWS, fxview=o.get("fxview") or {}, done=o.get("done") or []))
        if "group" in self.touched:
            step("설정 묶음", lambda: self.js(JS_RESTORE_GROUP, group=o.get("group") or "",
                                             ls=o.get("lsGroup"), kg=K_GROUP))
        if "theme" in self.touched:
            step("색 흉내", lambda: self.cmd("Emulation.setEmulatedMedia", {"media": "", "features": []}))
        if "size" in self.touched:
            step("창 크기 흉내", lambda: self.cmd("Emulation.clearDeviceMetricsOverride"))
        time.sleep(max(0.5, self.a.settle))     # '자동' 인 사람의 matchMedia·크기 되돌림 배치가 먼저 돌게
        if "theme" in self.touched:
            step("테마(data-theme·ntTheme·제목 띠)", lambda: self.js(
                JS_RESTORE_THEME, theme=o.get("theme"), chrome=o.get("chromeDark"), ls=o.get("lsTheme"), kt=K_THEME))
        step("탭·사이드바·스크롤", lambda: self.js(
            JS_RESTORE_TAB, orig=o.get("tab") or "", nav=bool(o.get("navOpen")), scroll=o.get("scroll") or 0))
        try:
            v = self.js(JS_VERIFY, id=STYLE_ID, kt=K_THEME, kg=K_GROUP) or {}
            miss = []
            if v.get("style") or v.get("marks"):
                miss.append("찍기용 스타일·표식이 남음(%d)" % (v.get("marks") or 0))
            if v.get("theme") != o.get("theme"):
                miss.append("data-theme %s(원래 %s)" % (v.get("theme"), o.get("theme")))
            if v.get("lsTheme") != o.get("lsTheme"):
                miss.append("%s 저장 값이 원래와 다름" % K_THEME)
            if v.get("lsGroup") != o.get("lsGroup"):
                miss.append("%s 저장 값이 원래와 다름" % K_GROUP)
            if o.get("group") and v.get("group") != o.get("group"):
                miss.append("설정 묶음 %s(원래 %s)" % (v.get("group"), o.get("group")))
            if o.get("chromeDark") is not None and v.get("chrome") != o.get("chromeDark"):
                miss.append("제목 띠 색(_chromeDark)이 원래와 다름")
            if o.get("tab") and v.get("tab") != o.get("tab"):
                miss.append("탭 %s(원래 %s)" % (v.get("tab"), o.get("tab")))
            views = [k for k, x in (o.get("fxview") or {}).items() if x and (v.get("fxview") or {}).get(k) != x]
            if views:
                miss.append("보기가 원래와 다른 탭: %s" % ", ".join(views))
            if miss:
                ok = False
                self.restore_notes.append("확인: " + " · ".join(miss))
        except (JsError, CdpError) as e:
            ok = False
            self.restore_notes.append("되돌린 뒤 확인 못 함 — " + scrub(e)[:200])
        return ok

    # ── 본 흐름 ──────────────────────────────────────────────────────────
    def run(self) -> dict:
        aborted, lost, interrupted = "", False, False
        try:
            if self.orig.get("leftovers"):
                n = self.js(JS_CLEAN, id=STYLE_ID)
                self.say("  지난번에 끊긴 찍기의 찌꺼기(스타일·표식 %s개)를 먼저 걷었습니다" % n)
            self.js(JS_STYLE_ON, id=STYLE_ID, css=SHOT_CSS)
            self.touched.add("style")
            for theme in self.themes:
                for (w, h) in self.sizes:
                    self.set_size(w, h)
                    if (w, h) == self.sizes[0]:
                        self.set_theme(theme)   # 크기를 먼저 — 테마 전환 뒤 배치가 한 번만 돌게
                    mode = self.js(JS_MODE) or ""
                    batch = []
                    for tab in self.tabs:
                        groups = self.groups if tab == "settings" else [None]
                        views = [None]
                        if tab in self.fx and mode == "narrow" and self.orig.get("canView"):
                            views = ["set", "run"] if self.a.run_view else ["set"]
                        for g in groups:
                            for v in views:
                                batch += self.shoot_pages(tab, g, v, w, h, theme)
                    self.say_batch(theme, w, h, mode, batch)
        except Abort as e:
            aborted, lost = str(e), True
        except KeyboardInterrupt:
            aborted, interrupted = "사용자가 멈춤(Ctrl-C)", True
        except CdpError as e:     # 크기·색 흉내 같은 판 전체 명령이 거절됨 — 더 찍어도 같은 그림이다
            aborted = "화면 명령이 거절됨 — " + scrub(e)[:200]

        restored = False
        if not lost:
            try:
                restored = self.restore()
            except Abort as e:
                aborted, lost = aborted or str(e), True
            except KeyboardInterrupt:
                interrupted = True
                aborted = aborted or "사용자가 멈춤(Ctrl-C)"
                self.restore_notes.append("되돌리는 중에 멈춤(Ctrl-C)")
        if not lost:
            try:
                self.p.pump(0.3)
                self.drain_errors()
            except (WsError, OSError):
                pass
        return {"aborted": aborted, "lost": lost, "interrupted": interrupted, "restored": restored}

    def say_batch(self, theme: str, w: int, h: int, mode: str, batch: list) -> None:
        good = [r for r in batch if r["ok"]]
        warn = sum(1 for r in good if r["warn"])
        bad = sum(1 for r in batch if not r["ok"] or r["bad"])
        exc = sum(len(r.get("exceptions") or []) for r in batch)
        tail = []
        if warn:
            tail.append("주의 %d" % warn)
        if bad:
            tail.append("문제 %d" % bad)
        if exc:
            tail.append("JS 예외 %d" % exc)
        self.say("  %s · %d×%d%s — %d장%s" % (
            THEME_KO[theme], w, h, (" (%s)" % MODE_KO.get(mode, mode)) if mode else "",
            len(good), (" · " + " · ".join(tail)) if tail else ""))


# ── contact.html ─────────────────────────────────────────────────────────────
CONTACT_CSS = """
:root { color-scheme: light dark; --bg:#f3f3f1; --card:#ffffff; --ink:#1c1e22; --sub:#626a74; --line:#d8dbdf;
  --warn:#a3261c; --warn-bg:#fcebe8; --note:#6b5a12; --accent:#2c56c9; --accent-ink:#ffffff; --thumb:300px; }
@media (prefers-color-scheme: dark) { :root { --bg:#141619; --card:#1d2024; --ink:#e6e8eb; --sub:#9aa3ad; --line:#30353b;
  --warn:#ff8f84; --warn-bg:#3a1e1b; --note:#e2c86a; --accent:#8eaeff; --accent-ink:#0d1220; } }
* { box-sizing: border-box; }
body { margin:0; background:var(--bg); color:var(--ink);
  font:14px/1.5 -apple-system, BlinkMacSystemFont, "Apple SD Gothic Neo", "Noto Sans KR", "Segoe UI", sans-serif; }
header { position:sticky; top:0; z-index:5; background:var(--bg); border-bottom:1px solid var(--line); padding:12px 16px 10px; }
h1 { font-size:17px; margin:0 0 2px; }
.meta { color:var(--sub); font-size:12.5px; }
.verdict { margin:6px 0 0; font-weight:600; }
.verdict.bad { color:var(--warn); }
.bar { display:flex; flex-wrap:wrap; gap:6px 16px; margin-top:8px; align-items:center; }
.grp { display:flex; flex-wrap:wrap; gap:4px; align-items:center; }
.grp > b { font-size:12px; color:var(--sub); font-weight:600; margin-right:2px; }
button.f { border:1px solid var(--line); background:var(--card); color:var(--ink); border-radius:999px;
  padding:2px 10px; font:inherit; font-size:12px; cursor:pointer; }
button.f[aria-pressed="true"] { background:var(--accent); border-color:var(--accent); color:var(--accent-ink); }
label.sz { font-size:12px; color:var(--sub); display:flex; gap:6px; align-items:center; }
.count { font-size:12px; color:var(--sub); }
main { padding:8px 16px 48px; }
.problems { background:var(--warn-bg); border:1px solid var(--warn); border-radius:8px; padding:8px 12px; margin:12px 0; }
.problems li { margin:2px 0; }
section { margin:18px 0 28px; }
section[hidden] { display:none; }
h2 { font-size:15px; margin:0 0 8px; }
h2 small { color:var(--sub); font-weight:400; margin-left:6px; }
.grid { display:grid; grid-template-columns:repeat(auto-fill, minmax(min(100%, var(--thumb)), 1fr)); gap:12px; align-items:start; }
figure { margin:0; background:var(--card); border:1px solid var(--line); border-radius:8px; overflow:hidden; }
figure[hidden] { display:none; }
figure.warn { border-color:var(--warn); }
figure a { display:block; line-height:0; }
figure img { display:block; width:100%; height:auto; border-bottom:1px solid var(--line); }
figcaption { padding:6px 9px 8px; font-size:12px; line-height:1.45; }
figcaption .k { font-weight:600; }
figcaption .s { color:var(--sub); }
figcaption .w { color:var(--warn); }
figcaption .n { color:var(--note); }
.fail { padding:28px 10px; text-align:center; color:var(--warn); background:var(--warn-bg); font-size:12.5px; }
footer { color:var(--sub); font-size:12px; padding:0 16px 32px; }
#lb { position:fixed; inset:0; z-index:50; background:rgba(8,9,11,.93); display:flex; flex-direction:column;
  align-items:center; justify-content:center; padding:12px; }
#lb[hidden] { display:none; }
#lb img { max-width:100%; max-height:calc(100vh - 80px); object-fit:contain; }
#lb .cap { color:#e8e8e8; font-size:13px; margin-top:8px; text-align:center; }
#lb .cap a { color:#9fbaff; }
#lb button { position:absolute; background:rgba(255,255,255,.12); color:#fff; border:0; border-radius:8px;
  font-size:22px; width:44px; height:44px; cursor:pointer; }
#lb .x { top:12px; right:12px; }
#lb .prev { left:12px; top:50%; }
#lb .next { right:12px; top:50%; }
"""

CONTACT_JS = r"""
(function () {
  var figs = [].slice.call(document.querySelectorAll('figure[data-tab]'));
  var on = { theme: {}, size: {}, tab: {} }, warnOnly = false;
  var count = document.getElementById('count');
  function empty(o) { for (var k in o) if (o[k]) return false; return true; }
  function apply() {
    var n = 0;
    figs.forEach(function (f) {
      var ok = (empty(on.theme) || on.theme[f.dataset.theme]) && (empty(on.size) || on.size[f.dataset.size])
            && (empty(on.tab) || on.tab[f.dataset.tab]) && (!warnOnly || f.classList.contains('warn'));
      f.hidden = !ok; if (ok) n++;
    });
    [].forEach.call(document.querySelectorAll('section[data-tab]'), function (s) {
      s.hidden = !s.querySelector('figure:not([hidden])');
    });
    if (count) count.textContent = n + ' / ' + figs.length + '장';
  }
  [].forEach.call(document.querySelectorAll('button.f'), function (b) {
    b.addEventListener('click', function () {
      var k = b.getAttribute('data-k'), v = b.getAttribute('data-v');
      if (k === 'warn') warnOnly = !warnOnly; else on[k][v] = !on[k][v];
      b.setAttribute('aria-pressed', String(k === 'warn' ? warnOnly : !!on[k][v]));
      apply();
    });
  });
  var sz = document.getElementById('thumb');
  if (sz) sz.addEventListener('input', function () { document.documentElement.style.setProperty('--thumb', sz.value + 'px'); });
  var lb = document.getElementById('lb'), img = lb.querySelector('img'), cap = lb.querySelector('.cap'), cur = -1;
  function shown() { return figs.filter(function (f) { return !f.hidden && f.querySelector('a[href]'); }); }
  function open(f) {
    var list = shown(); cur = list.indexOf(f); if (cur < 0) return;
    var href = f.querySelector('a[href]').getAttribute('href');
    img.src = href; cap.textContent = f.getAttribute('data-title') + '  ';
    var a = document.createElement('a'); a.href = href; a.target = '_blank'; a.textContent = '원본';
    cap.appendChild(a); lb.hidden = false;
  }
  function step(d) { var list = shown(); if (!list.length) return; cur = (cur + d + list.length) % list.length; open(list[cur]); }
  figs.forEach(function (f) {
    var a = f.querySelector('a[href]'); if (!a) return;
    a.addEventListener('click', function (e) {
      if (e.metaKey || e.ctrlKey || e.shiftKey || e.altKey) return;
      e.preventDefault(); open(f);
    });
  });
  lb.addEventListener('click', function (e) { if (e.target === lb || e.target.classList.contains('x')) lb.hidden = true; });
  lb.querySelector('.prev').addEventListener('click', function (e) { e.stopPropagation(); step(-1); });
  lb.querySelector('.next').addEventListener('click', function (e) { e.stopPropagation(); step(1); });
  document.addEventListener('keydown', function (e) {
    if (lb.hidden) return;
    if (e.key === 'Escape') lb.hidden = true;
    else if (e.key === 'ArrowRight') step(1);
    else if (e.key === 'ArrowLeft') step(-1);
  });
  apply();
})();
"""


def esc(s) -> str:
    return html.escape(str(s), quote=True)


def write_contact(out: Path, meta: dict, shots: list, order: dict) -> Path:
    """사람이 한눈에 훑는 격자. 그림은 상대 경로로만 건다 — 폴더째 옮겨도 열린다. JS 없이도 다 보인다."""
    tab_i = {t: i for i, t in enumerate(order["tabs"])}
    grp_i = {g: i for i, g in enumerate(order["groups"])}
    th_i = {t: i for i, t in enumerate(order["themes"])}
    sz_i = {s: i for i, s in enumerate(order["sizes"])}

    def key(r):
        return (tab_i.get(r["tab"], 99), grp_i.get(r["group"] or None, 99), 0 if r["view"] != "run" else 1,
                th_i.get(r["theme"], 9), sz_i.get((r["width"], r["height"]), 99), r["page"])

    sections: list = []
    for r in sorted(shots, key=key):
        sk = (r["tab"], r["group"])
        if not sections or sections[-1][0] != sk:
            sections.append((sk, []))
        sections[-1][1].append(r)

    h = ['<!doctype html>\n<html lang="ko">\n<head>\n<meta charset="utf-8">\n'
         '<meta name="viewport" content="width=device-width, initial-scale=1">\n'
         '<title>그림 찍기 — %s</title>\n<style>%s</style>\n</head>\n<body>\n' % (esc(meta["when"]), CONTACT_CSS)]
    h.append('<header>\n<h1>그림 찍기 — 한이시키 %s</h1>\n' % esc(meta.get("version") or "?"))
    h.append('<div class="meta">%s · 격리 사본 포트 %s · 그림 %d장 · %d초 · 화소 배율 %g · %s</div>\n' % (
        esc(meta["when"]), esc(meta["port"]), meta["taken"], int(meta["elapsed"]), float(meta["dpr"]), esc(meta["format"])))
    h.append('<div class="verdict%s">%s</div>\n' % ("" if meta["exit"] == 0 else " bad", esc(meta["verdict"])))
    h.append('<div class="bar">\n')
    chips = [("theme", "테마", [(t, THEME_KO[t]) for t in order["themes"]]),
             ("size", "폭", [("%dx%d" % s, "%d×%d" % s) for s in order["sizes"]]),
             ("tab", "탭", [(t, t) for t in order["tabs"]])]
    for k, title, items in chips:
        if len(items) < 2:
            continue
        h.append('<div class="grp"><b>%s</b>' % esc(title))
        for v, lab in items:
            h.append('<button type="button" class="f" data-k="%s" data-v="%s" aria-pressed="false">%s</button>'
                     % (esc(k), esc(v), esc(lab)))
        h.append('</div>\n')
    h.append('<div class="grp"><button type="button" class="f" data-k="warn" data-v="1" aria-pressed="false">'
             '주의·문제만</button></div>\n')
    h.append('<label class="sz">보기 크기 <input type="range" id="thumb" min="160" max="900" step="20" value="300"></label>\n')
    h.append('<span class="count" id="count">%d장</span>\n</div>\n</header>\n<main>\n' % len(shots))

    probs = meta.get("problem_lines") or []
    if probs:
        h.append('<ul class="problems">%s</ul>\n' % "".join("<li>%s</li>" % esc(x) for x in probs))

    for (tab, group), recs in sections:
        first = next((r for r in recs if r["ok"]), recs[0])
        title = first.get("label") or tab
        if group:
            title += " — " + (order["glabels"].get(group) or group)
        h.append('<section data-tab="%s">\n<h2>%s<small>%s%s · %d장</small></h2>\n<div class="grid">\n' % (
            esc(tab), esc(title), esc(tab), esc((" · " + group) if group else ""), len(recs)))
        for r in recs:
            size = "%dx%d" % (r["width"], r["height"])
            head = "%d×%d · %s" % (r["width"], r["height"], THEME_KO.get(r["theme"], r["theme"]))
            if r["view"] == "run":
                head += " · " + VIEW_KO["run"]
            if r["page"] > 1:
                head += " · %d쪽" % r["page"]
            flagged = (not r["ok"]) or r["warn"] or r["bad"] or r.get("exceptions")
            dtitle = "%s · %s" % (title, head)
            h.append('<figure data-tab="%s" data-theme="%s" data-size="%s" data-title="%s"%s>' % (
                esc(tab), esc(r["theme"]), esc(size), esc(dtitle), ' class="warn"' if flagged else ""))
            if r["ok"]:
                px = r.get("px") or [r["width"], r["height"]]
                h.append('<a href="%s"><img src="%s" alt="%s" loading="lazy" width="%d" height="%d"></a>' % (
                    esc(quote(r["file"])), esc(quote(r["file"])), esc(dtitle), px[0], px[1]))
            else:
                h.append('<div class="fail">못 찍음 — %s</div>' % esc(r["why"]))
            h.append('<figcaption><div class="k">%s</div>' % esc(head))
            sub = []
            if r.get("mode"):
                sub.append("%s · %s열" % (MODE_KO.get(r["mode"], r["mode"]), r.get("cols") or "?"))
            elif r.get("cols"):
                sub.append("%s열" % r["cols"])
            if r.get("masked") or r.get("maskedLines"):
                sub.append("비밀 가림 %d" % (r["masked"] + r["maskedLines"]))
            if r["ok"]:
                sub.append(r["file"])
            if sub:
                h.append('<div class="s">%s</div>' % " · ".join(esc(x) for x in sub))
            for x in r["bad"] + r["warn"]:
                h.append('<div class="w">%s</div>' % esc(x))
            for x in (r.get("exceptions") or [])[:3]:
                h.append('<div class="w">%s</div>' % esc(x[:200]))
            for x in r.get("notes") or []:
                h.append('<div class="n">%s</div>' % esc(x))
            h.append('</figcaption></figure>\n')
        h.append('</div>\n</section>\n')

    h.append('</main>\n<footer>비밀 칸(쿠키·세션·토큰·비밀번호·키)과 그런 값이 보이는 로그 줄은 찍기 직전에 빗금으로 가렸습니다 — '
             '값은 읽지도 적지도 않았습니다. 이 폴더에는 .gitignore(*) 가 있어 git 에 올라가지 않습니다. '
             '폭은 앱이 보는 화면 안쪽 폭(CSS px)입니다.</footer>\n')
    h.append('<div id="lb" hidden><button type="button" class="x" aria-label="닫기">×</button>'
             '<button type="button" class="prev" aria-label="앞 그림">‹</button><img alt="">'
             '<div class="cap"></div><button type="button" class="next" aria-label="다음 그림">›</button></div>\n')
    h.append('<script>%s</script>\n</body>\n</html>\n' % CONTACT_JS)
    path = out / "contact.html"
    part = out / "contact.html.part"
    part.write_text("".join(h), "utf-8")
    os.replace(str(part), str(path))
    return path


# ── 판정 ─────────────────────────────────────────────────────────────────────
def judge(sh: Shooter, res: dict, strict: bool) -> dict:
    shots = sh.shots
    failed = [r for r in shots if not r["ok"]]
    bad = [r for r in shots if r["ok"] and r["bad"]]
    over = [r for r in shots if r["ok"] and any(w.startswith(("옆 넘침", "문서 옆 넘침")) for w in r["warn"])]
    cover = [r for r in shots if r["ok"] and r.get("overlays")]
    shaky = [r for r in shots if r["ok"] and not r.get("stable", True)]
    other = [r for r in shots if r["ok"] and any(w.startswith(("그림 ", "테마가")) for w in r["warn"])]
    masked = max([r.get("masked", 0) + r.get("maskedLines", 0) for r in shots] or [0])
    parts, warns = [], []
    if failed:
        parts.append("못 찍음 %d장" % len(failed))
    if bad:
        parts.append("창 흉내·탭이 어긋남 %d장" % len(bad))
    if sh.exc:
        parts.append("JS 예외 %d건" % len(sh.exc))
    if not res["lost"] and not res["restored"]:
        parts.append("되돌리기 못 함")
    if res["aborted"]:
        parts.append("끝까지 못 돎 — " + res["aborted"])
    for lst, what in ((over, "옆 넘침"), (cover, "덮개"), (shaky, "배치가 멎지 않음"), (other, "크기·테마 어긋남")):
        if lst:
            (parts if strict and what in ("옆 넘침", "덮개") else warns).append("%s %d장" % (what, len(lst)))
    problems = (len(failed) + len(bad) + (1 if sh.exc else 0) + (0 if (res["lost"] or res["restored"]) else 1)
                + (1 if res["aborted"] else 0) + ((len(over) + len(cover)) if strict else 0))
    taken = len(shots) - len(failed)
    if res.get("interrupted"):
        code = 2                 # Ctrl-C — 판정이 아니다
    else:
        code = 0 if problems == 0 and taken > 0 else 1
    if code == 0:
        line = "통과 — 그림 %d장을 찍고 원래대로 되돌렸습니다%s" % (
            taken, "" if not warns else " (주의: %s)" % " · ".join(warns))
    else:
        line = "문제 %d건 — %s" % (max(problems, 1), " · ".join(parts) or "찍은 그림이 없음")
    return {"failed": len(failed), "bad": len(bad), "overflow": len(over), "overlay": len(cover),
            "unstable": len(shaky), "maskedMax": masked, "warns": warns, "parts": parts,
            "problems": problems, "taken": taken, "verdict": line, "exit": code}


def fatal(a, msg: str) -> int:
    if a.json:
        print(json.dumps({"tool": TOOL, "port": a.port, "error": msg, "exit": 2}, ensure_ascii=False, indent=1))
    else:
        print("돌릴 수 없음 — " + msg)
    return 2


def main() -> int:
    ap = argparse.ArgumentParser(
        description="그림 찍기 — 고른 탭 × 폭 × 테마를 격리 사본에서 찍고, 한눈에 훑는 contact.html 을 만든다.",
        epilog="먼저 python3 akashi/iso.py start 로 격리 사본을 띄우십시오. 사용자의 앱에는 붙지 않습니다.\n"
               "폭은 창 폭이 아니라 앱이 보는 화면 안쪽 폭(CSS px)입니다. 자동 배치는 내용 폭(≈ 폭 − 사이드바 148 − 여백 48)으로\n"
               "나뉩니다 — 좁음 <560 · 보통 560~859 · 넓음 860~. 그래서 420 좁음 · 900 보통 · 1180 넓음 2열 · 1700 넓음 3열.\n"
               "종료 코드: 0 모두 찍고 되돌림 · 1 문제 있음 · 2 돌릴 수 없음",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=os.environ.get("AKASHI_PORT") or 9334,   # ★ 글이면 argparse 가 int 로 — 빈 값·숫자 아님도 넘어지지 않고 종료 2
                    help="격리 사본의 CDP 포트 (기본 9334 · 환경 변수 AKASHI_PORT)")
    ap.add_argument("--tabs", default=DEFAULT_TABS,
                    help="찍을 탭, 쉼표로 (기본 %s). fx = 기능 탭 전부, all = 모든 탭(대시보드·프록시 포함), "
                         "그 밖엔 이름: twitter,fanbox,settings …  --list 로 목록을 봅니다" % DEFAULT_TABS)
    ap.add_argument("--widths", default=DEFAULT_WIDTHS,
                    help="화면 안쪽 폭(CSS px), 쉼표로. 높이까지 주려면 900x760 (기본 %s = 좁음·보통·넓음 2열·넓음 3열)"
                         % DEFAULT_WIDTHS)
    ap.add_argument("--height", type=int, default=DEFAULT_HEIGHT,
                    help="폭만 준 항목의 높이 (기본 %d)" % DEFAULT_HEIGHT)
    ap.add_argument("--themes", default="light,dark", help="light·dark 중에서, 쉼표로 (기본 둘 다 · 라이트/다크 도 됨)")
    ap.add_argument("--groups", default="all",
                    help="설정 탭을 찍을 묶음, 쉼표로 (기본 all · each = 전체와 묶음 하나하나 · 예: all,storage,ai)")
    ap.add_argument("--run-view", action="store_true",
                    help="좁음에서 기능 탭의 실행 화면(로그)도 따로 찍는다 (기본은 설정 보기만)")
    ap.add_argument("--pages", type=int, default=1,
                    help="스크롤해 아래쪽까지 몇 쪽을 찍을지 (기본 1 = 맨 위만 · 최대 %d · 끝에 닿으면 멈춤)" % MAX_PAGES)
    ap.add_argument("--dpr", type=float, default=1.0,
                    help="화소 배율 (기본 1 · 2 면 레티나처럼 선명하지만 파일이 네 배 · 최대 3)")
    ap.add_argument("--format", choices=("png", "jpeg"), default="png", help="그림 형식 (기본 png)")
    ap.add_argument("--quality", type=int, default=85, help="jpeg 품질 1~100 (기본 85)")
    ap.add_argument("--hide-overlays", action="store_true",
                    help="본 화면을 덮은 창(모달)을 찍는 동안만 숨긴다 — 기본은 그대로 찍고 '덮개' 로 적는다")
    ap.add_argument("--out", help="그림 폴더 (기본: 지금 폴더의 akashi-shots-<시각>) · 비어 있지 않은 남의 폴더에는 쓰지 않는다")
    ap.add_argument("--settle", type=float, default=0.4,
                    help="크기·테마를 바꾼 뒤 먼저 기다릴 초 (기본 0.4 · 그 뒤엔 배치가 멎을 때까지 스스로 기다림)")
    ap.add_argument("--max-shots", type=int, default=400,
                    help="이보다 많이 찍게 되면 시작하지 않는다 (기본 400 · 쪽·실행 화면까지 넉넉히 센 수)")
    ap.add_argument("--strict", action="store_true", help="옆 넘침·덮개도 문제로 센다 (종료 코드 1)")
    ap.add_argument("--open", action="store_true", help="끝나면 contact.html 을 기본 브라우저로 연다")
    ap.add_argument("--list", action="store_true", help="찍을 수 있는 탭·설정 묶음과 지금 상태만 보여 주고 끝낸다")
    ap.add_argument("-v", "--verbose", action="store_true", help="그림마다 한 줄씩 (모드·열 수·주의)")
    ap.add_argument("--json", action="store_true", help="사람용 글 대신 JSON 한 덩어리로 (shots.json 과 같은 내용)")
    a = ap.parse_args()

    # ── 붙기 전에 볼 수 있는 것은 먼저 본다 ─────────────────────────────────
    if not (0 < a.port < 65536):
        return fatal(a, "--port 가 올바르지 않습니다: %d" % a.port)
    try:
        sizes = parse_sizes(a.widths, a.height)
        themes = parse_themes(a.themes)
        tab_names = split_names(a.tabs, "탭")
        group_names = split_names(a.groups, "설정 묶음")
    except ValueError as e:
        return fatal(a, str(e))
    if not (1 <= a.pages <= MAX_PAGES):
        return fatal(a, "--pages 는 1~%d 이어야 합니다." % MAX_PAGES)
    if not (1.0 <= a.dpr <= 3.0):
        return fatal(a, "--dpr 은 1~3 이어야 합니다.")
    if not (1 <= a.quality <= 100):
        return fatal(a, "--quality 는 1~100 이어야 합니다.")
    if not (0 <= a.settle <= 10):
        return fatal(a, "--settle 은 0~10 초여야 합니다.")
    if a.max_shots < 1:
        return fatal(a, "--max-shots 는 1 이상이어야 합니다.")
    out = None
    if not a.list:
        out, err = check_out(a.out)
        if err:
            return fatal(a, err)

    # ── 격리 사본에 붙기 ─────────────────────────────────────────────────────
    err, st = guard(a.port)
    if err:
        return fatal(a, err)
    try:
        ts = [t for t in cdplib.page_targets(a.port) if "window=" not in t.get("url", "")]
    except (OSError, ValueError) as e:
        return fatal(a, "포트 %d 에서 화면 목록을 읽지 못했습니다 (%s)" % (a.port, e))
    if not ts:
        return fatal(a, "포트 %d 에 본 창(index.html) 화면이 없습니다 — 화면이 준비될 때까지 기다린 뒤 다시." % a.port)
    ws_url = ts[0].get("webSocketDebuggerUrl") or ""
    # ★ 주소는 앱이 알려 준 것 — check_adapt 처럼 이 기계(127.0.0.1) 밖으로는 나가지 않는다.
    #   다른 디버거가 먼저 붙어 있으면 주소가 빠져 온다 — ["…"] 로 읽으면 KeyError 로 넘어져 종료 1(문제)로 보였다
    if urlparse(ws_url).hostname not in ("127.0.0.1", "localhost", "::1"):
        return fatal(a, "화면 주소가 비었거나 이 기계(127.0.0.1)가 아닙니다 — 붙지 않습니다(다른 디버거가 먼저 붙어 있으면 비어 옵니다).")
    try:
        p = Page(ws_url, timeout=20)
    except (WsError, OSError) as e:
        return fatal(a, "화면에 붙지 못했습니다 (%s)" % e)

    with p:
        sh = Shooter(p, a, st, {})
        try:
            ready = False
            end = time.time() + 20
            while time.time() < end:
                if sh.js(JS_READY):
                    ready = True
                    break
                time.sleep(0.4)
            if not ready:
                return fatal(a, "화면이 아직 준비되지 않았습니다(switchTab·.scroll-area 없음) — 잠시 뒤 다시.")
            orig = sh.js(JS_ORIG, id=STYLE_ID, kt=K_THEME, kg=K_GROUP) or {}
        except Abort as e:
            return fatal(a, str(e))
        except CdpError as e:
            return fatal(a, "화면을 읽지 못했습니다 — " + scrub(e))
        sh.orig = orig
        sh.fx = {t["name"] for t in orig.get("tabs") or [] if t.get("fx")}
        sh.glabels = orig.get("glabels") or {}

        avail = orig.get("tabs") or []
        if a.list:
            fx = [t["name"] for t in avail if t.get("fx")]
            rest = [t["name"] for t in avail if not t.get("fx")]
            if a.json:
                print(json.dumps({
                    "tool": TOOL, "port": a.port, "version": st.get("version") or "", "fx_tabs": fx, "other_tabs": rest,
                    "groups": orig.get("groups") or [], "group_labels": sh.glabels, "tab": orig.get("tab") or "",
                    "theme": orig.get("theme"), "ntTheme": orig.get("lsTheme"), "group": orig.get("group") or "",
                    "adapt": bool(orig.get("adapt")), "running": orig.get("running") or [], "exit": 0,
                }, ensure_ascii=False, indent=1))
                return 0
            print("찍을 수 있는 탭 — 기능 %d: %s" % (len(fx), " ".join(fx) or "(없음)"))
            print("                  그 밖 %d: %s" % (len(rest), " ".join(rest) or "(없음)"))
            gs = orig.get("groups") or []
            print("설정 묶음 — %s" % (" ".join("%s(%s)" % (g, sh.glabels.get(g) or "") for g in gs) or "(묶음 단추 없음)"))
            print("지금 — 탭 %s · data-theme %s · ntTheme %s · 설정 묶음 %s · 자동 배치 %s" % (
                orig.get("tab") or "?", orig.get("theme") or "(없음)",
                "(없음=자동)" if orig.get("lsTheme") is None else orig.get("lsTheme"),
                orig.get("group") or "?", "있음" if orig.get("adapt") else "없음(20dd34e 이전 빌드)"))
            if orig.get("running"):
                print("수집 중인 탭 — %s" % " ".join(orig["running"]))
            return 0

        try:
            sh.tabs = resolve_tabs(tab_names, avail)
            if "settings" in sh.tabs and orig.get("canGroup"):
                sh.groups = resolve_groups(group_names, orig.get("groups") or [])
            elif group_names != ["all"]:
                return fatal(a, "--groups 는 settings 탭을 찍을 때만 뜻이 있습니다(이 빌드엔 묶음 단추가 없거나 탭 목록에 settings 가 없음).")
        except ValueError as e:
            return fatal(a, str(e))
        if not sh.tabs:
            return fatal(a, "찍을 탭이 없습니다.")
        sh.sizes, sh.themes = sizes, themes

        per_size = sum(len(sh.groups) if t == "settings" else 1 for t in sh.tabs)
        base = per_size * len(sizes) * len(themes)
        fx_n = sum(1 for t in sh.tabs if t in sh.fx)
        worst = (base + (fx_n * len(sizes) * len(themes) if a.run_view else 0)) * a.pages
        if worst > a.max_shots:
            return fatal(a, "최대 %d장까지 찍게 됩니다(탭 %d × 크기 %d × 테마 %d%s%s) — --max-shots 한도 %d.\n"
                            "  --tabs·--widths·--themes 를 줄이거나 --max-shots 를 올리십시오."
                         % (worst, len(sh.tabs), len(sizes), len(themes), " + 실행 화면" if a.run_view else "",
                            " × %d쪽" % a.pages if a.pages > 1 else "", a.max_shots))
        try:
            make_out(out)
        except OSError as e:
            return fatal(a, "그림 폴더를 만들지 못했습니다 (%s): %s" % (e.strerror or e, paths.short(out)))
        sh.out = out

        # 이 뒤로 난 화면 JS 예외만 센다 — enable 하면 지난 콘솔 글이 한꺼번에 다시 오므로 비운다
        try:
            sh.cmd("Runtime.enable")
            sh.cmd("Log.enable")
            p.pump(0.4)
        except Abort as e:
            return fatal(a, str(e))
        del p.events[:]

        sh.say("그림 찍기 — 포트 %d · 앱 %s (격리 사본 PID %s)" % (a.port, st.get("version") or "?", st.get("pid")))
        sh.say("  탭 %d × 크기 %d × 테마 %d = %d장%s → %s" % (
            per_size, len(sizes), len(themes), base,
            (" (+ 좁음 실행 화면·아래쪽 쪽은 찍으며 더함)" if (a.run_view or a.pages > 1) else ""), paths.short(out)))
        if st.get("seeded"):
            sh.say("  주의 — 진짜 설정을 복사한 사본입니다. 비밀 칸은 가리지만 계정 이름은 그림에 보일 수 있으니 나눠 줄 때 살피십시오.")
        if orig.get("running"):
            sh.say("  참고 — 수집 중인 탭: %s (그대로 찍습니다)" % " ".join(orig["running"]))
        if not orig.get("adapt"):
            sh.say("  참고 — 자동 배치(fxAdapt)가 없는 빌드입니다. 모드·열 수는 비어 있습니다.")

        res = sh.run()

        # 쓰다 만 조각(.part)은 이 공구가 이 폴더(표식 있음)에 만든 것뿐이다 — 그것만 치운다
        for f in out.glob("*.part"):
            try:
                f.unlink()
            except OSError:
                pass
        for n in sh.notes:
            sh.say("  참고 — " + n)
        j = judge(sh, res, a.strict)
        if not res["lost"]:
            sh.say("  정리 — %s" % ("크기·색 흉내·테마·설정 묶음·보기·탭·스크롤·가림 표식을 되돌림" if res["restored"]
                                  else "다 되돌리지 못함"))
            for n in sh.restore_notes:
                sh.say("    " + n)
        if sh.exc:
            sh.say("  화면 JS 예외 %d건" % len(sh.exc))
            for e in sh.exc[:5]:
                sh.say("    " + e[:240])
        if sh.console and a.verbose:
            sh.say("  참고 — console.error·기록 오류 %d건" % sh.console)
        if j["maskedMax"]:
            sh.say("  가림 — 비밀 칸·줄을 한 장에 최대 %d곳 빗금으로 덮었습니다(값은 읽지 않음)" % j["maskedMax"])

        problem_lines = list(j["parts"]) + ["되돌리기: " + n for n in sh.restore_notes]
        meta = {
            "tool": TOOL, "when": time.strftime("%Y-%m-%d %H:%M:%S"), "port": a.port,
            "version": st.get("version") or "", "app": paths.short(st.get("app") or ""),
            "out": paths.short(out), "elapsed": round(time.time() - sh.t0, 1), "dpr": a.dpr, "format": a.format,
            "tabs": sh.tabs, "groups": [g for g in sh.groups if g], "sizes": ["%dx%d" % s for s in sizes],
            "themes": themes, "runView": a.run_view, "pages": a.pages, "hideOverlays": a.hide_overlays,
            "strict": a.strict, "taken": j["taken"], "failed": j["failed"], "overflow": j["overflow"],
            "overlay": j["overlay"], "unstable": j["unstable"], "maskedMax": j["maskedMax"],
            "js_exceptions": sh.exc, "console_errors": sh.console,
            "restored": res["restored"], "restore_notes": sh.restore_notes, "aborted": res["aborted"],
            "problems": j["problems"], "verdict": j["verdict"], "exit": j["exit"],
            "problem_lines": problem_lines, "shots": sh.shots,
        }
        contact = None
        try:
            contact = write_contact(out, meta, sh.shots, {
                "tabs": sh.tabs, "groups": sh.groups, "themes": themes, "sizes": sizes, "glabels": sh.glabels})
            part = out / "shots.json.part"
            part.write_text(json.dumps(meta, ensure_ascii=False, indent=1), "utf-8")
            os.replace(str(part), str(out / "shots.json"))
        except OSError as e:
            sh.say("  contact.html·shots.json 을 쓰지 못했습니다 — %s" % (e.strerror or e))
            if meta["exit"] == 0:
                meta["exit"] = 1
                meta["verdict"] = "문제 1건 — 목록 파일을 쓰지 못함"
        meta["contact"] = paths.short(contact) if contact else ""

        if a.json:
            print(json.dumps(meta, ensure_ascii=False, indent=1))
        else:
            if contact:
                print("  보기 — open %s" % paths.short(contact))
            print(meta["verdict"])
        if a.open and contact and sys.platform == "darwin":
            try:
                subprocess.run(["open", str(contact)], timeout=10, check=False)
            except (OSError, subprocess.SubprocessError):
                pass
        return meta["exit"]


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:     # 찍기 전·목록을 쓴 뒤의 Ctrl-C — 찍는 도중이면 run() 이 받아 되돌린다
        print("\n멈춤 — Ctrl-C")
        sys.exit(2)
