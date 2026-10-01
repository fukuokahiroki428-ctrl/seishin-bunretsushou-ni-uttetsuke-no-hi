#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""화면 오류 점검 — 격리 사본의 화면을 새로 고치고 모든 탭과 설정 묶음을 차례로 열면서, 그동안 난
JS 예외·console.error·기록(Log) 오류를 곳마다 모아 보인다.

    python3 akashi/iso.py start                               # 먼저 격리 사본을 띄운다
    python3 akashi/check_console.py                           # 새로 고침 → 탭 17개 → 설정 묶음 7개
    python3 akashi/check_console.py --no-reload               # 지금 화면 그대로 탭만 돈다
    python3 akashi/check_console.py --only settings,fanbox --seconds 1.5
    python3 akashi/check_console.py --widths all              # 좁음·보통·넓음(420·900·1700)에서 한 바퀴씩
    python3 akashi/check_console.py --ignore "ResizeObserver loop" --json
    python3 akashi/iso.py stop

하는 일
  1 붙기 전    Runtime·Log 를 켜면 화면이 뜬 뒤 쌓인 오류가 한꺼번에 다시 온다 — 따로 보인다(판정 밖)
  2 새로 고침  화면을 새로 띄우고, 새 문서가 준비된 뒤 --load-wait 초 더 듣는다(시작 타이머가 6초까지 돈다)
  3 탭         .tab-content 를 화면에서 찾아 switchTab 으로 하나씩 열고 --seconds 초 듣는다. 켜졌는지·보이는지도
               본다. 설정 탭에서는 묶음 단추(setSettingsGroup)도 하나씩. --widths 를 주면 폭마다 한 바퀴씩.
  4 되돌리기   창 크기 흉내·탭·설정 묶음(화면과 저장 값)·스크롤·새로 고침 표식을 처음대로
  오류마다 번호(#1 …)를 붙여 어느 곳에서 몇 번 났는지 보이고, 글과 자리(index.html:줄:칸 함수)는 끝에 한 번만.

종료 코드: 0 오류 없음 · 1 문제 있음(JS 예외·console.error·기록 오류·탭이 안 켜짐/안 보임·대화 상자·
          되돌리기 못 함·도중에 앱이 사라지거나 멈춤) · 2 돌릴 수 없음(격리 사본 없음·포트 닫힘·화면 없음·
          수집 중인데 새로 고침·잘못된 인자·Ctrl-C)

★ 왜 새로 고치나
  화면이 처음 뜰 때(QWebChannel 이 붙고 C++ 가 설정·폼을 보내오는 사이) 나는 오류가 가장 흔한데, 우리가 붙기
  전에 난 것은 어느 단계에서 났는지 알 길이 없다. 새로 고치면 처음부터 우리가 듣는 가운데 다시 뜬다.
  새로 고침이 정말 새 문서를 띄웠는지는 옛 문서에 달아 둔 표식이 사라졌는지로 가린다 — 명령이 먹지 않았는데
  옛 화면을 새 화면으로 착각하지 않도록. 화면의 로그 칸은 비워진다(격리 사본이라 괜찮다). 싫으면 --no-reload.
  수집 중인 탭이 있으면 새로 고치지 않고 2 로 끝난다 — 새로 고친 화면은 수집 중이라는 것을 잃는다.

★ 왜 격리 사본에만 붙나
  이 공구는 화면을 새로 고치고, 설정 묶음을 바꾸면 앱이 localStorage(hanishiki.settingsGroup)에 적는다.
  localStorage 는 앱의 자료 폴더(프로필) 안에 산다 — 사용자의 앱에 붙으면 사용자의 자료를 바꾸는 셈이다.
  그래서 check_adapt 와 같은 규칙으로, iso.py 기록(state.json)의 PID 가 살아 있고(실행 파일·뜬 시각까지 대조)
  그 PID(또는 그 자식)가 바로 이 포트를 듣고 있을 때만 붙는다. 창이 여럿이면 본 창('window=' 없는 것)에만.

★ 왜 사이드바를 누르지 않고 switchTab 을 부르나
  사이드바의 몇 항목(data-window-only)은 누르면 별도 창을 연다 — 점검하다 창이 늘어난다. 앱이 누를 때 부르는
  switchTab·setSettingsGroup 을 그대로 부른다. 그 함수가 바로 던진 예외는 사람이 눌렀다면 콘솔에 떴을 것이므로
  '예외(부를 때)' 로 그 탭에 센다. 부른 뒤에는 그 탭만 켜졌는지, 크기가 있어 보이는지도 본다.

★ 왜 붙기 전 기록·바깥 자원 오류는 판정에 넣지 않나
  붙기 전 기록에는 앞서 돌린 다른 공구가 일부러 만든 상황에서 난 것도 섞여 있다 — 이번 점검의 결과라고 할 수
  없다. 목록에는 보이고, --count-earlier 로 판정에 넣는다(--no-reload 로 앱이 뜰 때의 오류를 따질 때).
  'Failed to load resource' 가 바깥 주소(프로필 그림 등)면 인터넷·계정 사정이지 화면 코드 탓이 아니다. 앱 안
  (file·qrc)이나 127.0.0.1 의 것은 센다. 늘 나는 것을 알고 넘기려면 --ignore — 넘긴 것도 목록에는 남는다.

★ 오류가 어느 탭에 붙나
  탭을 연 뒤 --seconds 동안 온 것은 그 탭의 것으로 친다. 백엔드의 답을 기다렸다 그리는 것처럼 더 늦게 나는
  오류는 다음 곳에 붙을 수 있다 — 의심스러우면 --only 로 그 탭만, --seconds 를 늘려 다시. 설정 탭은 백엔드에
  여러 가지를 묻고 답을 그리므로 적어도 1초 듣는다.

★ 비밀 — 왜 오류 글을 끝에 한꺼번에 찍나
  오류 글에 토큰·쿠키가 섞여 나올 수 있다(요청 주소·응답 조각). 찍기 전에 두 겹으로 가린다.
  ① 화면에게 "이 글에 비밀 칸(쿠키·세션·토큰·비밀번호 칸, accounts·_formBase 의 값)의 12자 이상 조각이 들어
     있나" 를 물어 참/거짓만 받는다 — 값은 이쪽으로 오지 않는다. 들어 있으면 글 전체를 가린다.
  ② 주소는 파일 이름(바깥 주소는 호스트와 끝 이름)만 남기고 물음표 뒤를 버린다. 이름=값 꼴의 비밀, 긴 영숫자,
     12자리 넘는 숫자는 길이만 남긴다. 진짜 홈 경로는 ~ 로.
  ①은 모든 오류가 모인 뒤 한 번에 묻는다. 그래서 탭을 도는 동안에는 번호만 찍고, 글은 대조가 끝난 뒤 목록으로
  보인다. 화면이 도중에 사라져 대조를 못 하면 ②만으로 가리고 그렇다고 적는다. --json 도 같은 글만 낸다.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
import unicodedata
from pathlib import Path
from urllib.parse import unquote, urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))
import iso  # noqa: E402  격리 사본 기록(state.json)을 iso.py 와 같은 규칙으로 읽는다
from lib import cdp as cdplib  # noqa: E402
from lib import paths, proc  # noqa: E402
from lib.cdp import CdpError, JsError, Page  # noqa: E402
from lib.ws import WsError  # noqa: E402

TOOL = "check_console"
DEFAULT_PORT = 9334

# ── 앱과 맞춘 이름(index.html) — 바뀌면 여기도 ───────────────────────────────
K_GROUP = "hanishiki.settingsGroup"   # setSettingsGroup 이 적는 저장 값 — 끝나면 원래 글자대로(없었으면 지움)
MARK = "__akashiConsoleMark"          # 새로 고침이 새 문서를 띄웠는지 가리는 표식(옛 문서에만 있다)

# ── 기다림 ──────────────────────────────────────────────────────────────
EARLY_WAIT = 0.5                      # Runtime·Log 를 켠 뒤 지난 글이 다 올 때까지
SETTINGS_MIN_WAIT = 1.0               # 설정 탭은 백엔드 답을 그린다 — 적어도 이만큼
RESIZE_SETTLE = 0.8                   # 창 크기를 바꾼 뒤 배치(fxAdapt)가 가라앉을 때까지
EVAL_TIMEOUT = 10.0                   # 탭 하나 여는 데 이보다 오래면 멈춘 것으로 의심한다
SLOW_MS = 1000                        # 탭 열기가 이보다 오래면 '느림' 으로 적는다(판정 밖)
PRESET_WIDTHS = (420, 900, 1700)      # check_adapt 와 같은 좁음·보통·넓음 (CSS px)

# ── 글 ──────────────────────────────────────────────────────────────────
LEAK_WINDOW = 12                      # 비밀 값의 이만한 조각이 글에 있으면 통째로 가린다
MAX_LIST = 30                         # 오류 목록은 이만큼까지 글로(나머지는 --json)
NAME_W = 18                           # 곳 이름 칸 너비

KIND_KO = {"exc": "예외", "call": "예외(부를 때)", "console": "console.error", "log": "기록 오류", "net": "바깥 자원"}
MODE_KO = {"wide": "넓음", "mid": "보통", "narrow": "좁음", "": "배치 없음"}
NAME_RX = re.compile(r"^[a-z][a-z0-9_-]{0,30}$")


# ── 화면 쪽 도우미 — 매번 함수 안에 넣어 돌린다(전역에 아무것도 남기지 않는다) ─────
JS_HEAD = r"""
var KG = 'hanishiki.settingsGroup', MK = '__akashiConsoleMark';
function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
function errInfo(e) {
  if (e === null || typeof e !== 'object') return { name: 'throw', message: String(e).split('\n')[0].slice(0, 300), stack: '' };
  var m = (e.message !== undefined) ? e.message : e;
  return { name: String(e.name || 'Error'), message: String(m).split('\n')[0].slice(0, 300),
           stack: String(e.stack || '').split('\n').slice(1, 7).join('\n') };
}
function curTab() {
  try { if (typeof currentTab !== 'undefined' && currentTab) return String(currentTab); } catch (e) {}
  var a = document.querySelector('.tab-content.active');
  return a ? a.id.replace(/^tab-/, '') : '';
}
function curGroup() { var c = document.querySelector('#tab-settings .sf-chip.active'); return c ? (c.getAttribute('data-group') || '') : ''; }
function scroller() { return document.querySelector('.scroll-area'); }
function fxMode() { return document.body ? (document.body.getAttribute('data-fxmode') || '') : ''; }
"""

# 탭·묶음 목록과 되돌릴 처음 상태
JS_STATE = r"""
// @state
var tabs = [], seen = {};
[].slice.call(document.querySelectorAll('.tab-content[id^="tab-"]')).forEach(function (t) {
  var n = t.id.slice(4);
  if (!n || seen[n]) return;
  seen[n] = 1;
  var stop = document.getElementById(n + '-stop-btn');
  var busy = t.classList.contains('fx-running')
          || !!(stop && !stop.disabled && getComputedStyle(stop).display !== 'none');
  tabs.push({ name: n, fx: t.classList.contains('fx'), running: busy });
});
var groups = [].slice.call(document.querySelectorAll('#tab-settings .sf-chip[data-group]')).map(function (c) {
  var f = c.firstChild;
  var label = (f && f.nodeType === 3) ? f.textContent : c.textContent;   // 뒤의 숫자 딱지(.sf-n)는 뺀다
  return { g: c.getAttribute('data-group') || '', label: String(label || '').replace(/\s+/g, ' ').trim().slice(0, 30) };
});
var s = scroller();
return { tabs: tabs, groups: groups, groupFn: typeof setSettingsGroup === 'function', switchFn: typeof switchTab === 'function',
         tab: curTab(), group: curGroup(), lsGroup: lsGet(KG), scroll: s ? Math.round(s.scrollTop) : 0,
         vw: innerWidth, vh: innerHeight, mode: fxMode() };
"""

JS_READY = r"""
// @ready
var r = { fresh: typeof window[MK] === 'undefined', rs: document.readyState, sw: typeof switchTab === 'function', be: false };
try { r.be = typeof backend !== 'undefined' && !!backend; } catch (e) {}
r.ready = r.sw && r.be && r.rs === 'complete';
return r;
"""

JS_MARK = r"""
// @mark
window[MK] = Date.now();
return true;
"""

JS_SWITCH = r"""
// @switch
var t0 = performance.now(), err = null;
try { switchTab(A.name); } catch (e) { err = errInfo(e); }
var ms = Math.round(performance.now() - t0);
var el = document.getElementById('tab-' + A.name);
var others = [].slice.call(document.querySelectorAll('.tab-content.active'))
  .map(function (x) { return x.id; }).filter(function (id) { return id !== 'tab-' + A.name; });
var shown = false, why = '';
if (el) {
  var cs = getComputedStyle(el), r = el.getBoundingClientRect();
  shown = cs.display !== 'none' && cs.visibility !== 'hidden' && r.width > 0 && r.height > 0;
  if (!shown) why = cs.display === 'none' ? 'display:none' : cs.visibility === 'hidden' ? 'visibility:hidden'
                  : ('크기 ' + Math.round(r.width) + 'x' + Math.round(r.height));
}
var tt = document.getElementById('toolbar-title');
return { ms: ms, err: err, exists: !!el, active: !!el && el.classList.contains('active'), others: others,
         shown: shown, why: why, cur: curTab(), title: tt ? String(tt.textContent || '').trim().slice(0, 40) : '' };
"""

JS_GROUP = r"""
// @group
var t0 = performance.now(), err = null;
try { setSettingsGroup(A.g); } catch (e) { err = errInfo(e); }
var ms = Math.round(performance.now() - t0);
var b = document.getElementById('settings-board');
var secs = b ? [].slice.call(b.querySelectorAll('.sg')) : [];
return { ms: ms, err: err, active: curGroup(),
         shown: secs.filter(function (x) { return !x.hidden; }).length, total: secs.length };
"""

JS_MODE = r"""
// @mode
return { mode: fxMode(), vw: innerWidth, vh: innerHeight };
"""

JS_RESTORE = r"""
// @restore
var errs = [];
if (A.group && typeof setSettingsGroup === 'function') {
  try { setSettingsGroup(A.group); } catch (e) { errs.push({ what: "setSettingsGroup('" + A.group + "')", err: errInfo(e) }); }
}
var lsOk = true;   // setSettingsGroup 이 적은 것을 처음 글자로 — 없었으면 지운다
try { if (A.lsGroup === null) localStorage.removeItem(KG); else localStorage.setItem(KG, A.lsGroup); } catch (e) { lsOk = false; }
if (A.tab && typeof switchTab === 'function') {
  try { switchTab(A.tab); } catch (e) { errs.push({ what: "switchTab('" + A.tab + "')", err: errInfo(e) }); }
}
try { delete window[MK]; } catch (e) {}
return { errs: errs, lsOk: lsOk };
"""

JS_SCROLL = r"""
// @scroll
var s = scroller();
if (s) s.scrollTop = A.scroll;
return s ? Math.round(s.scrollTop) : 0;
"""

JS_VERIFY = r"""
// @verify
var s = scroller();
return { tab: curTab(), group: curGroup(), lsGroup: lsGet(KG), scroll: s ? Math.round(s.scrollTop) : 0,
         mark: typeof window[MK] !== 'undefined', vw: innerWidth, vh: innerHeight };
"""

# 오류 글에 비밀 칸의 값(조각)이 들어 있나 — 참/거짓만 돌려준다. 값은 화면 밖으로 나오지 않는다.
JS_LEAK = r"""
// @leak
var W = A.w, vals = [];
var RX = /token|ct0|pass|cookie|sess|secret|akey|api-?key|-key$|auth|cred|dtsg|lsd|^ap-w-/i;
function add(v, min) { v = (v === null || v === undefined) ? '' : String(v); if (v.length >= min) vals.push(v); }
function walk(v, d) {
  if (v === null || v === undefined || d > 5) return;
  if (typeof v === 'string') { add(v, 8); return; }
  if (typeof v !== 'object') return;
  for (var k in v) {
    if (!Object.prototype.hasOwnProperty.call(v, k) || k === 'name' || k === 'handle') continue;
    walk(v[k], d + 1);
  }
}
try {
  [].slice.call(document.querySelectorAll('input, textarea')).forEach(function (el) {
    if (el.type === 'password') add(el.value, 4);
    else if (RX.test(el.id || '') || RX.test(el.name || '')) add(el.value, 8);
  });
} catch (e) {}
try { if (typeof accounts !== 'undefined') walk(accounts, 0); } catch (e) {}
try { if (typeof _formBase !== 'undefined' && _formBase) { for (var k in _formBase) if (RX.test(k)) walk(_formBase[k], 0); } } catch (e) {}
function leaks(t) {
  t = String(t || '');
  for (var j = 0; j < vals.length; j++) if (vals[j].length < W && t.indexOf(vals[j]) >= 0) return true;
  for (var i = 0; i + W <= t.length; i++) {
    var w = t.substr(i, W);
    if (/\s/.test(w)) continue;
    for (var j2 = 0; j2 < vals.length; j2++) if (vals[j2].length >= W && vals[j2].indexOf(w) >= 0) return true;
  }
  return false;
}
return A.texts.map(leaks);
"""


# ── 글 다듬기: 주소 줄이기 · 비밀 가리기 ─────────────────────────────────────
_URL = re.compile(r"(?i)\b(?:https?|wss?|file|qrc|blob|data|chrome-extension|chrome|devtools):[^\s'\"()<>\[\]{}]+")
_BEARER = re.compile(r"(?i)\b(bearer|basic)(\s+)([A-Za-z0-9._~+/=\-]{8,})")
_SECRET_KV = re.compile(
    r"(?i)\b(auth_token|access_token|refresh_token|id_token|x-csrf-token|csrftoken|csrf_token|xsrf-token|ct0|"
    r"fanboxsessid|phpsessid|sessionid|session_id|sessid|session|fb_dtsg|lsd|set-cookie|cookie|"
    r"password|passwd|pwd|pass|client_secret|secret|x-api-key|api[_-]?key|private[_-]?key|authorization|auth|token|sid)"
    r"([\"']?\s*[=:]\s*[\"']?)([^\s;&,'\"]+)")
_KV_LONG = re.compile(r"\b([A-Za-z_][\w.\-]{0,40})(=)([^\s;&,'\"]{12,})")
_LONG = re.compile(r"[A-Za-z0-9%_\-+/=.]{32,}")
_MIXED = re.compile(r"[A-Za-z0-9%_\-+/=]{20,}")
_DIGITS = re.compile(r"\d{12,}")
_TAIL = re.compile(r"[A-Za-z0-9%_\-+/=.:]+$")
_HARMLESS = {"null", "undefined", "true", "false", "none", "nan", "[object", "object"}


def _mask_kv(m) -> str:
    v = m.group(3)
    if len(v) < 4 or v.lower().strip("\"'") in _HARMLESS:
        return m.group(0)
    return "%s%s<가림 %d자>" % (m.group(1), m.group(2), len(v))


def _mask_all(m) -> str:
    return "<가림 %d자>" % len(m.group(0))


def _mask_mixed(m) -> str:
    s = m.group(0)
    # 토큰은 글자와 숫자가 섞여 있다. getBoundingClientRect 같은 긴 이름은 숫자가 없어 그대로 둔다.
    if re.search(r"\d", s) and re.search(r"[A-Za-z]", s):
        return _mask_all(m)
    return s


def short_url(u) -> str:
    """주소는 파일 이름만(앱 안), 바깥은 호스트와 끝 이름만. 물음표·# 뒤와 사용자 경로를 버린다."""
    u = str(u or "").strip()
    if not u:
        return ""
    low = u.lower()
    if low.startswith("data:"):
        return "data:" + u[5:].split(",", 1)[0].split(";", 1)[0][:40] + ",…"
    if low.startswith("blob:"):
        return "blob:…"
    try:
        pu = urlparse(u)
        port = pu.port
    except ValueError:
        return "(주소)"
    path = unquote(pu.path or "")
    if pu.scheme in ("http", "https", "ws", "wss"):
        host = (pu.hostname or "") + (":%d" % port if port else "")
        segs = [x for x in path.split("/") if x]
        if not segs:
            return "%s://%s/" % (pu.scheme, host)
        return "%s://%s/%s%s" % (pu.scheme, host, "…/" if len(segs) > 1 else "", segs[-1])
    if pu.scheme in ("file", "qrc", "chrome", "devtools", "chrome-extension", ""):
        base = (path or u).rstrip("/").rsplit("/", 1)[-1]
        return base or (pu.scheme + ":")
    return pu.scheme + ":…"


def scrub(text, truncated: bool = False) -> str:
    """찍어도 되는 글로. 비밀 모양은 길이만 남긴다(값·조각을 남기지 않는다)."""
    s = str(text)
    if truncated:   # 240자에서 잘린 글 — 끝에 걸친 토큰 조각은 떼어 낸다
        s = _TAIL.sub("", s).rstrip() + " …"
    s = _URL.sub(lambda m: short_url(m.group(0)), s)
    s = s.replace(str(paths.real_home()), "~")
    s = _BEARER.sub(_mask_kv, s)
    s = _SECRET_KV.sub(_mask_kv, s)
    s = _KV_LONG.sub(_mask_kv, s)
    s = _LONG.sub(_mask_all, s)
    s = _MIXED.sub(_mask_mixed, s)
    s = _DIGITS.sub(_mask_all, s)
    s = s.encode("utf-8", "replace").decode("utf-8")      # 깨진 UTF-16(외톨이 대리 문자)이 print 를 넘어뜨리지 않게
    return s.replace("\r", " ").replace("\n", " ⏎ ")


def is_external(url) -> bool:
    try:
        pu = urlparse(str(url or ""))
    except ValueError:
        return False
    return pu.scheme in ("http", "https", "ws", "wss") and (pu.hostname or "") not in ("127.0.0.1", "localhost", "::1")


def host_of(url) -> str:
    try:
        return urlparse(str(url or "")).hostname or ""
    except ValueError:
        return ""


def dwidth(s: str) -> int:
    return sum(2 if unicodedata.east_asian_width(c) in ("W", "F") else 1 for c in s)


def wpad(s: str, n: int) -> str:
    """한글은 두 칸을 먹는다 — 칸을 맞춘다."""
    return s + " " * max(0, n - dwidth(s))


def show_ls(v) -> str:
    return "없음" if v is None else "'%s'" % v


# ── 오류 이벤트 → 기록 ──────────────────────────────────────────────────────
_STACK = re.compile(r"^\s*at\s+(?:(.*?)\s+\()?(.+?):(\d+):(\d+)\)?\s*$")


def mk_frame(fn, url, line, col, zero: bool) -> dict:
    """CDP 는 줄·칸을 0 부터 센다(zero=True). 사람이 읽는 줄 번호로 맞춘다."""
    def num(v):
        try:
            return int(v) + (1 if zero else 0)
        except (TypeError, ValueError):
            return 0
    return {"fn": str(fn or ""), "url": str(url or ""), "line": num(line), "col": num(col)}


def cdp_frames(cf, limit: int = 5) -> list:
    return [mk_frame(f.get("functionName"), f.get("url"), f.get("lineNumber"), f.get("columnNumber"), True)
            for f in (cf or [])[:limit] if isinstance(f, dict)]


def frames_from_stack(text, limit: int = 5) -> list:
    """e.stack 글('    at fn (url:줄:칸)')에서 자리를 뽑는다 — 여기 줄 번호는 이미 1 부터다."""
    out = []
    for line in str(text or "").splitlines():
        m = _STACK.match(line)
        if m:
            out.append(mk_frame(m.group(1), m.group(2), m.group(3), m.group(4), False))
            if len(out) >= limit:
                break
    return out


def fmt_frame(f) -> str:
    if not f:
        return ""
    s = short_url(f.get("url")) or "(주소 없음)"
    if f.get("line", 0) > 0:
        s += ":%d" % f["line"]
        if f.get("col", 0) > 0:
            s += ":%d" % f["col"]
    fn = (f.get("fn") or "").strip()
    return scrub((s + " " + fn).strip())


def error_events(batch) -> list:
    """p.collect_errors() 가 고르는 것과 같은 이벤트들, 같은 차례로 — 글에 자리를 붙이려고."""
    out = []
    for m in batch:
        meth, pr = m.get("method"), m.get("params") or {}
        if meth == "Runtime.exceptionThrown":
            out.append(m)
        elif meth == "Runtime.consoleAPICalled" and pr.get("type") == "error":
            out.append(m)
        elif meth == "Log.entryAdded" and (pr.get("entry") or {}).get("level") == "error":
            out.append(m)
    return out


def parse_record(text, ev) -> dict:
    """collect_errors 의 글 한 줄(+ 그 이벤트) → 종류·글·자리. 글은 아직 가리지 않은 날것 — 찍지 않는다."""
    kind, msg = "log", str(text)
    for pre, k in (("예외: ", "exc"), ("console.error: ", "console"), ("log: ", "log")):
        if msg.startswith(pre):
            kind, msg = k, msg[len(pre):]
            break
    first = msg.splitlines()[0] if msg else ""
    rec = {"kind": kind, "raw": first, "truncated": len(first) >= 240, "frames": [], "site": None, "host": ""}
    if not ev:
        return rec
    meth, pr = ev.get("method"), ev.get("params") or {}
    if meth == "Runtime.exceptionThrown":
        d = pr.get("exceptionDetails") or {}
        fr = cdp_frames((d.get("stackTrace") or {}).get("callFrames"))
        if not fr and (d.get("url") or d.get("lineNumber") is not None):
            fr = [mk_frame("", d.get("url"), d.get("lineNumber"), d.get("columnNumber"), True)]
        if not fr:
            fr = frames_from_stack((d.get("exception") or {}).get("description"))
        rec["frames"] = fr
    elif meth == "Runtime.consoleAPICalled":
        a0 = (pr.get("args") or [{}])[0] or {}
        site = cdp_frames((pr.get("stackTrace") or {}).get("callFrames"))
        # console.error(e) 면 e 가 생긴 자리가 더 쓸모 있다. 부른 자리는 따로 둔다.
        origin = frames_from_stack(a0.get("description")) if a0.get("subtype") == "error" else []
        rec["frames"] = origin or site
        if origin and site:
            rec["site"] = site[0]
    elif meth == "Log.entryAdded":
        e = pr.get("entry") or {}
        fr = cdp_frames((e.get("stackTrace") or {}).get("callFrames"))
        if not fr and e.get("url"):
            fr = [mk_frame("", e.get("url"), e.get("lineNumber"), None, True)]
        rec["frames"] = fr
        if e.get("source") == "network" and is_external(e.get("url")):
            rec["kind"] = "net"
            rec["host"] = host_of(e.get("url"))
    return rec


def call_record(what: str, err) -> dict:
    """우리가 부른 switchTab·setSettingsGroup 이 바로 던진 예외."""
    err = err if isinstance(err, dict) else {}
    raw = "%s — %s: %s" % (what, err.get("name") or "Error", err.get("message") or "")
    return {"kind": "call", "raw": raw[:300], "truncated": False,
            "frames": frames_from_stack(err.get("stack")), "site": None, "host": ""}


class Lost(Exception):
    """앱(화면)과 연결이 끊겼다 — 더 할 수 없다."""


class Hung(Exception):
    """화면이 답하지 않는다(멈춤·대화 상자) — 더 할 수 없다."""


# ── 격리 사본인지 (check_adapt.guard 와 같은 규칙) ───────────────────────────
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


# ── 점검 ────────────────────────────────────────────────────────────────────
class ConsoleCheck:
    def __init__(self, p, a, st: dict):
        self.p, self.a, self.st = p, a, st
        self.steps: list = []
        self.entries: dict = {}           # 열쇠 → 오류 하나(같은 것은 한 번만)
        self.order: list = []             # 처음 본 차례
        self.orig = None
        self.restored = None              # None = 되돌리기까지 못 감
        self.restore_notes: list = []
        self.aborted = ""
        self.lost = False                 # 화면과 끊김/멈춤 — 되돌리기·대조를 못 한다
        self.user_stop = False
        self.crashed = False
        self.sized = False
        self.leak_checked = False
        self.scope = {"tabs": 0, "groups": 0, "widths": 0, "reload": False}
        self.ignore = [x.lower() for x in (a.ignore or []) if x.strip()]

    # ── 말하기 ───────────────────────────────────────────────────────────
    def say(self, line: str = "") -> None:
        if not self.a.json:
            print(line, flush=True)

    def fatal(self, msg: str) -> int:
        return fatal(self.a, msg)

    # ── 화면 다루기 ──────────────────────────────────────────────────────
    def js(self, body: str, timeout: float = EVAL_TIMEOUT, **kw):
        # ensure_ascii — 오류 글에 깨진 대리 문자가 있어도 \\uXXXX 로 넘어가 웹소켓(UTF-8)이 넘어지지 않는다
        expr = "(function(A){%s\n%s\n})(%s)" % (JS_HEAD, body, json.dumps(kw, ensure_ascii=True))
        t0 = time.time()
        try:
            return self.p.eval(expr, timeout=timeout)
        except JsError:
            raise
        except CdpError:
            if time.time() - t0 < timeout - 0.2:
                raise                   # 곧바로 온 거절(맥락 없음 등) — 부른 쪽이 적는다
            self._close_dialog()
            if not self._alive():
                raise Hung("화면이 %d초 넘게 답하지 않습니다 — 멈췄거나 대화 상자가 떠 있습니다" % int(timeout))
            raise CdpError("%d초 넘게 답이 없다가 풀렸습니다" % int(timeout))
        except (WsError, OSError) as e:
            raise Lost("앱과 연결이 끊겼습니다 (%s)" % e)

    def cmd(self, method: str, params: dict | None = None, timeout: float | None = None):
        try:
            return self.p.cmd(method, params, timeout)
        except (WsError, OSError) as e:
            raise Lost("앱과 연결이 끊겼습니다 (%s)" % e)

    def pump(self, seconds: float) -> None:
        try:
            self.p.pump(seconds)
        except (WsError, OSError) as e:
            raise Lost("앱과 연결이 끊겼습니다 (%s)" % e)

    def _alive(self) -> bool:
        try:
            return self.p.eval("1", timeout=5) == 1
        except CdpError:
            return False
        except (WsError, OSError) as e:
            raise Lost("앱과 연결이 끊겼습니다 (%s)" % e)

    def _close_dialog(self) -> None:
        # alert·confirm 이 떠 있으면 화면이 멎는다. [취소] 쪽으로 닫는다(아무것도 하지 않는 쪽).
        try:
            self.p.cmd("Page.handleJavaScriptDialog", {"accept": False}, 5)
        except (CdpError, WsError, OSError):
            pass

    def wait_ready(self, timeout: float) -> dict:
        end = time.time() + timeout
        last: dict = {}
        while True:
            try:
                last = self.js(JS_READY, timeout=5) or {}
            except (JsError, CdpError, Hung):
                last = {}
            if last.get("ready") or time.time() >= end:
                return last
            time.sleep(0.25)

    # ── 곳(단계) ─────────────────────────────────────────────────────────
    def begin(self, kind: str, name: str, label: str = "", width=None, counted: bool = True) -> dict:
        key = name if width is None or kind in ("resize",) else "%s@%d" % (name, width)
        s = {"kind": kind, "name": name, "key": key, "label": label, "width": width, "ms": None,
             "ids": {}, "problems": [], "notes": [], "counted": counted, "status": ""}
        self.steps.append(s)
        return s

    def harvest(self, s: dict) -> None:
        """지금까지 받은 이벤트를 이 곳의 것으로 거둔다(공통 층 collect_errors 의 글 + 이벤트의 자리)."""
        texts = self.p.collect_errors()
        batch = self.p.events
        self.p.events = []
        for m in batch:
            meth = m.get("method")
            if meth == "Page.javascriptDialogOpening":
                typ = str((m.get("params") or {}).get("type") or "?")[:20]
                s["problems"].append("대화 상자(%s)가 떴음 — [취소]로 닫음" % typ)
                self._close_dialog()
            elif meth == "Inspector.targetCrashed":
                s["problems"].append("화면 프로세스가 죽었음(targetCrashed)")
                self.crashed = True
        evs = error_events(batch)
        if len(evs) != len(texts):      # 공통 층의 고르는 법이 바뀌었다 — 자리 없이 글만
            evs = [None] * len(texts)
        for t, ev in zip(texts, evs):
            self.add(s, parse_record(t, ev))

    def add(self, s: dict, rec: dict) -> None:
        f0 = rec["frames"][0] if rec["frames"] else {}
        if rec["kind"] == "net":        # 바깥 그림 수십 장이 따로 줄을 차지하지 않게 — 호스트로 묶는다
            k = (rec["kind"], rec["raw"], rec["host"])
        else:
            k = (rec["kind"], rec["raw"], f0.get("url", ""), f0.get("line", 0), f0.get("col", 0), f0.get("fn", ""))
        e = self.entries.get(k)
        if e is None:
            low = rec["raw"].lower()
            rx = scrub(rec["raw"], rec["truncated"]).lower()     # 사람이 본 글(가린 뒤)로도 --ignore 가 맞게
            e = dict(rec)
            e.update({"id": len(self.order) + 1, "total": 0, "steps": {},
                      "ignored": any(x in low or x in rx for x in self.ignore)})
            self.entries[k] = e
            self.order.append(e)
        e["total"] += 1
        e["steps"][s["key"]] = e["steps"].get(s["key"], 0) + 1
        s["ids"][e["id"]] = s["ids"].get(e["id"], 0) + 1

    def counts(self, e: dict, s: dict) -> bool:
        """이 곳에서 난 이 오류를 판정에 넣나."""
        return s["counted"] and not e["ignored"] and e["kind"] != "net"

    def status(self, s: dict) -> str:
        if s["problems"]:
            return "문제"
        byid = {e["id"]: e for e in self.order}
        if any(self.counts(byid[i], s) for i in s["ids"]):
            return "오류"
        return "참고" if s["ids"] else "통과"

    def show(self, s: dict) -> None:
        s["status"] = self.status(s)
        byid = {e["id"]: e for e in self.order}
        refs = []
        for i in sorted(s["ids"]):
            n = s["ids"][i]
            r = "#%d" % i + (" ×%d" % n if n > 1 else "")
            e = byid[i]
            if not self.counts(e, s):
                r += "(%s)" % why_not(e, s)
            refs.append(r)
        tail = []
        if refs:
            tail.append(" ".join(refs))
        tail += s["problems"] + s["notes"]
        if self.a.verbose and s.get("ms") is not None and s["kind"] in ("tab", "group"):
            tail.append("%dms" % s["ms"])
        body = scrub(s["label"]) if s["label"] else ""
        if tail:
            body = (body + " — " if body else "— ") + " · ".join(tail)
        self.say(("  %s  %s  %s" % (s["status"], wpad(s["key"], NAME_W), body)).rstrip())

    def finish(self, s: dict) -> None:
        self.harvest(s)
        self.show(s)
        if self.crashed:
            raise Lost("화면 프로세스가 죽었습니다")

    def step_error(self, s: dict, e: Exception) -> None:
        """우리 쪽 점검 코드·명령이 돌지 못함 — 그 곳의 문제로 적는다."""
        if isinstance(e, JsError):
            s["problems"].append("점검 코드가 돌지 못함 — " + scrub(e))
        else:
            s["problems"].append("화면 명령 실패 — " + scrub(e))

    # ── 1 붙기 전 기록 ───────────────────────────────────────────────────
    def earlier(self) -> None:
        try:
            self.cmd("Inspector.enable")          # 화면 프로세스가 죽으면 알려 준다(없는 판도 있다)
        except CdpError:
            pass
        for m in ("Page.enable", "Runtime.enable", "Log.enable"):
            self.cmd(m)
        self.pump(EARLY_WAIT)
        s = self.begin("earlier", "붙기 전", counted=self.a.count_earlier)
        self.harvest(s)
        s["label"] = "쌓여 있던 오류 %s" % ("%d가지" % len(s["ids"]) if s["ids"] else "없음")
        self.show(s)

    # ── 2 새로 고침 ──────────────────────────────────────────────────────
    def reload(self) -> bool:
        s = self.begin("reload", "새로 고침")
        self.scope["reload"] = True
        marked = False
        try:
            marked = bool(self.js(JS_MARK))
        except (JsError, CdpError) as e:
            self.step_error(s, e)
        if not marked:      # 표식이 없으면 '새 문서' 를 가릴 수 없다 — 준비만 본다
            s["notes"].append("표식을 달지 못해 새 문서인지는 확인 못 함")
        t0 = time.time()
        try:
            self.p.reload(settle=0.0)             # 명령만 — 준비는 아래에서 표식까지 보고 따진다
        except (WsError, OSError) as e:
            raise Lost("앱과 연결이 끊겼습니다 (%s)" % e)
        except CdpError as e:
            s["problems"].append("새로 고침 명령이 듣지 않음 — " + scrub(e))
            self.finish(s)
            return False
        last: dict = {}
        end = t0 + self.a.load_timeout
        while time.time() < end:
            try:
                last = self.p.eval("(function(A){%s\n%s\n})({})" % (JS_HEAD, JS_READY), timeout=5) or {}
            except (WsError, OSError) as e:
                raise Lost("앱과 연결이 끊겼습니다 (%s)" % e)
            except CdpError:
                last = {}                         # 문서가 바뀌는 사이 — 잠깐 맥락이 없다
            if (last.get("fresh") or not marked) and last.get("ready"):
                break
            time.sleep(0.25)
        ready_s = time.time() - t0
        s["ms"] = int(ready_s * 1000)
        if marked and last.get("fresh") is False:
            s["problems"].append("새로 고침이 일어나지 않음 — %d초 뒤에도 옛 문서(표식이 그대로)" % int(self.a.load_timeout))
            self.finish(s)
            return True                           # 옛 화면이라도 탭은 돌 수 있다
        if not last.get("ready"):
            miss = [n for k, n in (("sw", "switchTab"), ("be", "backend")) if not last.get(k)]
            if last.get("rs") and last.get("rs") != "complete":
                miss.append("문서 %s" % last.get("rs"))
            s["problems"].append("새 화면이 %d초 안에 준비되지 않음 (%s)" % (int(self.a.load_timeout), ", ".join(miss) or "답 없음"))
            self.finish(s)
            return False
        self.pump(self.a.load_wait)
        s["label"] = "준비 %.1f초 · %.1f초 더 들음" % (ready_s, self.a.load_wait)
        self.finish(s)
        return True

    # ── 3 탭·묶음 ────────────────────────────────────────────────────────
    def visit_tab(self, t: dict, width) -> None:
        name = t["name"]
        s = self.begin("tab", name, width=width)
        wait = max(self.a.seconds, SETTINGS_MIN_WAIT) if name == "settings" else self.a.seconds
        r = None
        try:
            r = self.js(JS_SWITCH, name=name) or {}
        except (JsError, CdpError) as e:
            self.step_error(s, e)
        if r is not None:
            s["label"] = r.get("title") or ""
            s["ms"] = r.get("ms")
            if r.get("err"):
                self.add(s, call_record("switchTab('%s')" % name, r["err"]))
            if not r.get("exists"):
                s["problems"].append("탭 요소(tab-%s)가 없음" % name)
            elif not r.get("active"):
                s["problems"].append("켜지지 않음 (지금 탭: %s)" % scrub(r.get("cur") or "?"))
            elif not r.get("shown"):
                s["problems"].append("켜졌지만 보이지 않음 (%s)" % scrub(r.get("why") or "?"))
            if r.get("others"):
                s["problems"].append("다른 탭도 함께 켜져 있음: " + scrub(", ".join(str(x) for x in r["others"][:5])))
            if isinstance(s["ms"], int) and s["ms"] > SLOW_MS:
                s["notes"].append("느림 %.1f초" % (s["ms"] / 1000.0))
        self.pump(wait)
        self.finish(s)

    def visit_group(self, g: dict, width) -> None:
        s = self.begin("group", "settings·" + g["g"], label=g.get("label") or "", width=width)
        r = None
        try:
            r = self.js(JS_GROUP, g=g["g"]) or {}
        except (JsError, CdpError) as e:
            self.step_error(s, e)
        if r is not None:
            s["ms"] = r.get("ms")
            if r.get("err"):
                self.add(s, call_record("setSettingsGroup('%s')" % g["g"], r["err"]))
            if r.get("active") != g["g"]:
                s["problems"].append("묶음 단추가 바뀌지 않음 (켜진 것: %s)" % scrub(r.get("active") or "없음"))
            elif g["g"] != "all" and not r.get("shown"):
                s["problems"].append("그 묶음에 보이는 칸이 없음 (전체 %s칸)" % r.get("total"))
            if isinstance(s["ms"], int) and s["ms"] > SLOW_MS:
                s["notes"].append("느림 %.1f초" % (s["ms"] / 1000.0))
        self.pump(self.a.seconds)
        self.finish(s)

    def resize(self, w: int, h: int) -> None:
        s = self.begin("resize", "폭 %dpx" % w, width=w)
        self.sized = True
        try:
            self.p.size(w, h, settle=RESIZE_SETTLE)
        except (WsError, OSError) as e:
            raise Lost("앱과 연결이 끊겼습니다 (%s)" % e)
        except CdpError as e:
            s["problems"].append("창 크기 흉내가 듣지 않음 — " + scrub(e))
        self.pump(self.a.seconds)
        try:
            r = self.js(JS_MODE) or {}
            s["label"] = "%s · 화면 안쪽 %sx%s" % (MODE_KO.get(r.get("mode") or "", r.get("mode")), r.get("vw"), r.get("vh"))
        except (JsError, CdpError) as e:
            self.step_error(s, e)
        self.finish(s)

    def discover(self):
        o = self.js(JS_STATE) or {}
        tabs = [t for t in (o.get("tabs") or []) if isinstance(t, dict) and NAME_RX.match(str(t.get("name") or ""))]
        groups = [g for g in (o.get("groups") or []) if isinstance(g, dict) and NAME_RX.match(str(g.get("g") or ""))]
        return o, tabs, groups

    def visit_all(self) -> None:
        o, tabs, groups = self.discover()
        if not o.get("switchFn"):       # 준비를 확인한 뒤라 거의 없는 일 — 끊긴 것은 아니니 되돌리기는 한다
            self.aborted = "화면에 switchTab 이 없어 탭을 돌지 못함"
            return
        if self.a.only:
            tabs = [t for t in tabs if t["name"] in self.a.only]
            gone = [n for n in self.a.only if n not in [t["name"] for t in tabs]]
            if gone:
                self.say("  주의: 새로 고친 화면에 없는 탭 — %s (건너뜀)" % ", ".join(gone))
        do_groups = bool(groups) and bool(o.get("groupFn")) and any(t["name"] == "settings" for t in tabs)
        if any(t["name"] == "settings" for t in tabs) and not do_groups:
            self.say("  참고: 설정 묶음 단추(setSettingsGroup)가 없어 묶음은 건너뜁니다")
        widths = self.a.widths or [None]
        self.scope.update({"tabs": len(tabs), "groups": len(groups) if do_groups else 0,
                           "widths": len(self.a.widths or [])})
        per = "한 곳에 %.1f초" % self.a.seconds
        if any(t["name"] == "settings" for t in tabs) and self.a.seconds < SETTINGS_MIN_WAIT:
            per += "(설정 탭은 %.1f초)" % SETTINGS_MIN_WAIT
        self.say("  탭 %d개%s%s — %s" % (
            len(tabs), " · 설정 묶음 %d개" % len(groups) if do_groups else "",
            " · 폭 %s" % "·".join(str(w) for w in self.a.widths) if self.a.widths else "", per))
        h = max(500, int(self.orig.get("vh") or 800))
        for w in widths:
            if w is not None:
                self.resize(w, h)
            for t in tabs:
                self.visit_tab(t, w)
                if t["name"] == "settings" and do_groups:
                    for g in groups:
                        self.visit_group(g, w)

    # ── 4 되돌리기 ───────────────────────────────────────────────────────
    def restore(self) -> None:
        o = self.orig
        s = self.begin("restore", "되돌리기")
        fails, done = [], []
        if self.sized:
            try:
                self.p.clear_size()
                done.append("창 크기")
            except (WsError, OSError) as e:
                raise Lost("앱과 연결이 끊겼습니다 (%s)" % e)
            except CdpError as e:
                fails.append("창 크기 흉내를 걷지 못함 — " + scrub(e))
            time.sleep(RESIZE_SETTLE)
        r = None
        try:
            r = self.js(JS_RESTORE, tab=o.get("tab") or "", group=o.get("group") or "", lsGroup=o.get("lsGroup")) or {}
        except (JsError, CdpError) as e:
            fails.append("되돌리는 코드가 돌지 못함 — " + scrub(e))
        if r is not None:
            for x in r.get("errs") or []:
                self.add(s, call_record(str(x.get("what") or "?"), x.get("err")))
            if not r.get("lsOk", True):
                fails.append("저장 값 %s 을 쓰지 못함" % K_GROUP)
        time.sleep(0.3)                           # 탭을 바꾼 뒤 벽돌 쌓기(layoutFrames)가 끝나야 스크롤이 맞는다
        try:
            self.js(JS_SCROLL, scroll=int(o.get("scroll") or 0))
        except (JsError, CdpError):
            pass
        self.pump(max(0.3, self.a.seconds))
        v = None
        try:
            v = self.js(JS_VERIFY) or {}
        except (JsError, CdpError) as e:
            fails.append("되돌린 것을 확인하지 못함 — " + scrub(e))
        if v is not None:
            if o.get("tab"):
                if v.get("tab") == o["tab"]:
                    done.append("탭 %s" % o["tab"])
                else:
                    fails.append("탭이 %s 가 아니라 %s" % (o["tab"], scrub(v.get("tab") or "?")))
            if o.get("group"):
                if v.get("group") == o["group"]:
                    done.append("설정 묶음 %s" % o["group"])
                else:
                    fails.append("설정 묶음이 %s 가 아니라 %s" % (o["group"], scrub(v.get("group") or "?")))
            if v.get("lsGroup") == o.get("lsGroup"):
                done.append("저장 값")
            else:
                fails.append("%s 이 %s 가 아니라 %s" % (K_GROUP, show_ls(o.get("lsGroup")), show_ls(v.get("lsGroup"))))
            if v.get("mark"):
                fails.append("새로 고침 표식(%s)이 남음" % MARK)
            if abs(int(v.get("scroll") or 0) - int(o.get("scroll") or 0)) <= 2:
                done.append("스크롤")
            else:   # 새로 고친 뒤엔 내용 높이가 달라 다 못 맞출 수 있다 — 판정 밖
                self.restore_notes.append("스크롤 %s → %s(내용 높이가 달라 다 못 맞춤)" % (o.get("scroll"), v.get("scroll")))
            if self.sized and (abs(int(v.get("vw") or 0) - int(o.get("vw") or 0)) > 1
                               or abs(int(v.get("vh") or 0) - int(o.get("vh") or 0)) > 1):
                self.restore_notes.append("화면 안쪽 %sx%s → %sx%s(처음에 다른 흉내가 걸려 있었을 수 있음)"
                                          % (o.get("vw"), o.get("vh"), v.get("vw"), v.get("vh")))
        self.restored = not fails
        s["label"] = " · ".join(done) + (" 되돌림" if done else "")
        s["problems"] += fails
        s["notes"] += self.restore_notes          # 판정 밖 참고 — 줄에는 보이고 문제로 세지 않는다
        self.finish(s)

    # ── 끝: 비밀 대조 · 글 다듬기 ────────────────────────────────────────
    def finalize(self) -> None:
        hits = None
        if self.order and not self.lost:
            try:
                hits = self.js(JS_LEAK, timeout=30, w=LEAK_WINDOW, texts=[e["raw"] for e in self.order])
            except (JsError, CdpError, Lost, Hung):
                hits = None
            except KeyboardInterrupt:           # 대조를 못 했어도 글은 ②로 가려서 낸다
                hits, self.user_stop = None, True
                self.aborted = self.aborted or "사용자가 멈춤(Ctrl-C)"
        self.leak_checked = (not self.order) or (isinstance(hits, list) and len(hits) == len(self.order))
        for i, e in enumerate(self.order):
            hit = bool(hits[i]) if isinstance(hits, list) and self.leak_checked else False
            if hit:
                e["text"] = "<비밀 칸의 값이 섞여 있어 통째로 가림 · %d자>" % len(e["raw"])
            else:
                e["text"] = scrub(e["raw"], e["truncated"])
            e["where"] = fmt_frame(e["frames"][0]) if e["frames"] else ""
            e["frames_s"] = [fmt_frame(f) for f in e["frames"]]
            e["site_s"] = fmt_frame(e["site"]) if e.get("site") else ""
            if e["kind"] == "net" and not e["where"]:
                e["where"] = scrub(e.get("host") or "")
            e.pop("raw", None)                    # 날것은 여기서 버린다 — 뒤로는 찍을 수 있는 글만 남는다

    # ── 보고 ─────────────────────────────────────────────────────────────
    def counted_entries(self) -> list:
        bykey = {s["key"]: s for s in self.steps}
        out = []
        for e in self.order:
            n = sum(c for k, c in e["steps"].items() if k in bykey and self.counts(e, bykey[k]))
            if n:
                out.append((e, n))
        return out

    def print_catalog(self) -> None:
        if not self.order:
            return
        self.say("")
        self.say("  오류 목록 — 같은 것은 한 번만 (번호는 위 줄의 #)")
        bykey = {s["key"]: s for s in self.steps}
        pad = " " * 22
        for e in self.order[:MAX_LIST]:
            self.say("   #%-3d %s %s" % (e["id"], wpad(KIND_KO.get(e["kind"], e["kind"]), 14), e["text"]))
            if e["where"]:
                self.say(pad + "자리 " + e["where"])
            if self.a.verbose:
                for f in e["frames_s"][1:]:
                    self.say(pad + "     " + f)
                if e["site_s"]:
                    self.say(pad + "부른 자리 " + e["site_s"])
            where = " · ".join(("%s ×%d" % (k, n)) if n > 1 else k for k, n in e["steps"].items())
            outs = sorted({why_not(e, bykey[k]) for k in e["steps"] if k in bykey and not self.counts(e, bykey[k])})
            self.say(pad + "나온 곳 " + where + (" [판정 밖: %s]" % ", ".join(outs) if outs else ""))
        if len(self.order) > MAX_LIST:
            self.say("   … 그 밖에 %d가지 — --json 으로 모두 봅니다" % (len(self.order) - MAX_LIST))
        if not self.leak_checked:
            self.say("  (화면과 비밀 칸을 대조하지 못해 글자 모양으로만 가렸습니다)")

    def report(self) -> int:
        counted = self.counted_entries()
        occ = sum(n for _, n in counted)
        prob_steps = [s for s in self.steps if s["problems"]]
        nprob = sum(len(s["problems"]) for s in prob_steps)      # 되돌리기에서 못 한 것도 하나씩
        # Ctrl-C 로 되돌리기가 끊겨 적힌 문제가 없을 때만 따로 하나 — 두 번 세지 않는다
        restore_bad = self.restored is False and not any(s["kind"] == "restore" for s in prob_steps)
        problems = len(counted) + nprob + (1 if restore_bad else 0) + (1 if self.lost else 0)

        self.print_catalog()
        if self.aborted:
            self.say("")
            self.say("  도중에 멈춤 — " + self.aborted)

        sc = self.scope
        what = []
        if sc["reload"]:
            what.append("새로 고침")
        what.append("탭 %d개" % sc["tabs"])
        if sc["groups"]:
            what.append("설정 묶음 %d개" % sc["groups"])
        if sc["widths"]:
            what.append("폭 %d가지" % sc["widths"])
        counted_ids = {e["id"] for e, _ in counted}
        outside = [e for e in self.order if e["id"] not in counted_ids]
        if problems == 0 and not self.aborted:
            line = "통과 — %s에서 JS 예외·console.error·기록 오류 없음" % " · ".join(what)
            if outside:
                line += " (판정 밖 %d가지는 목록 참고)" % len(outside)
        else:
            parts = []
            if counted:
                bykey = {s["key"]: s for s in self.steps}
                places = []
                for e, _ in counted:
                    for k in e["steps"]:
                        if k in bykey and self.counts(e, bykey[k]) and k not in places:
                            places.append(k)
                parts.append("JS 오류 %d가지(모두 %d번 · %s%s)" % (
                    len(counted), occ, ", ".join(places[:6]), " 외 %d곳" % (len(places) - 6) if len(places) > 6 else ""))
            if prob_steps:
                keys = [s["key"] for s in prob_steps]
                parts.append("곳 문제 %d건(%s%s)" % (nprob, ", ".join(keys[:6]), " 외" if len(keys) > 6 else ""))
            if restore_bad:
                parts.append("되돌리기 못 함")
            if self.aborted:
                parts.append("끝까지 못 돎")
            line = "문제 %d건 — %s" % (max(problems, 1), " · ".join(parts) or "결과가 모자람")
        if self.user_stop:
            code = 2            # Ctrl-C — 판정이 아니다
        else:
            code = 0 if problems == 0 and not self.aborted else 1

        if self.a.json:
            bykey = {s["key"]: s for s in self.steps}
            print(json.dumps({
                "tool": TOOL, "port": self.a.port, "app_version": self.st.get("version"), "pid": self.st.get("pid"),
                "reload": not self.a.no_reload, "seconds": self.a.seconds, "load_wait": self.a.load_wait,
                "widths": self.a.widths or [], "only": self.a.only or [], "ignore": self.a.ignore or [],
                "count_earlier": self.a.count_earlier,
                "steps": [{"kind": s["kind"], "name": s["name"], "key": s["key"], "width": s["width"],
                           "label": scrub(s["label"]) if s["label"] else "", "status": s["status"], "ms": s["ms"],
                           "errors": {str(i): n for i, n in sorted(s["ids"].items())},
                           "problems": s["problems"], "notes": s["notes"]} for s in self.steps],
                "errors": [{"id": e["id"], "kind": e["kind"], "kind_ko": KIND_KO.get(e["kind"], e["kind"]),
                            "text": e["text"], "where": e["where"], "frames": e["frames_s"], "console_site": e["site_s"],
                            "total": e["total"], "steps": e["steps"],
                            "counted": any(self.counts(e, bykey[k]) for k in e["steps"] if k in bykey),
                            "outside": sorted({why_not(e, bykey[k]) for k in e["steps"]
                                               if k in bykey and not self.counts(e, bykey[k])})}
                           for e in self.order],
                "leak_checked": self.leak_checked, "restored": self.restored, "restore_notes": self.restore_notes,
                "aborted": self.aborted, "problems": problems, "verdict": line, "exit": code,
            }, ensure_ascii=False, indent=1))
        else:
            self.say(line)
        return code

    # ── 차례 ─────────────────────────────────────────────────────────────
    def main(self) -> int:
        try:
            ready = self.wait_ready(20)
            if not ready.get("ready"):
                return self.fatal("화면이 아직 준비되지 않았습니다(switchTab·backend 없음) — 조금 뒤 다시 돌리십시오.")
            o = self.js(JS_STATE) or {}
        except (Lost, Hung) as e:
            return self.fatal(str(e))
        except CdpError as e:
            return self.fatal("화면 상태를 읽지 못했습니다 — " + scrub(e))
        self.orig = o
        names = [t.get("name") for t in (o.get("tabs") or []) if isinstance(t, dict)]
        if self.a.only:
            unknown = [n for n in self.a.only if n not in names]
            if unknown:
                return self.fatal("--only 에 없는 탭: %s (있는 탭: %s)" % (", ".join(unknown), ", ".join(names)))
        busy = [t.get("name") for t in (o.get("tabs") or []) if isinstance(t, dict) and t.get("running")]
        if busy and not self.a.no_reload:
            return self.fatal("수집 중인 탭이 있습니다(%s) — 새로 고치면 화면이 수집 상태를 잃습니다. "
                              "끝난 뒤 다시 돌리거나 --no-reload 로 돌리십시오." % ", ".join(busy))

        self.say("화면 오류 점검 — 포트 %d · 앱 %s · 격리 사본 PID %s"
                 % (self.a.port, self.st.get("version") or "?", self.st.get("pid")))
        if self.a.verbose:
            self.say("  처음 상태: 탭 %s · 설정 묶음 %s · %s=%s · 스크롤 %s · 화면 안쪽 %sx%s (%s)" % (
                o.get("tab") or "?", o.get("group") or "?", K_GROUP, show_ls(o.get("lsGroup")), o.get("scroll"),
                o.get("vw"), o.get("vh"), MODE_KO.get(o.get("mode") or "", o.get("mode"))))
        self.say("")

        try:
            self.earlier()
            ok = True
            if not self.a.no_reload:
                ok = self.reload()
            if ok:
                self.visit_all()
            else:
                self.aborted = "새로 고친 화면이 준비되지 않아 탭을 돌지 못함"
        except (Lost, Hung) as e:
            self.aborted, self.lost = str(e), True
        except KeyboardInterrupt:
            self.aborted, self.user_stop = "사용자가 멈춤(Ctrl-C)", True

        if not self.lost:
            try:
                self.restore()
            except (Lost, Hung) as e:
                self.aborted, self.lost = self.aborted or str(e), True
            except KeyboardInterrupt:
                self.user_stop = True
                self.restored = False
                self.restore_notes.append("되돌리는 중에 멈춤(Ctrl-C)")
                self.aborted = self.aborted or "사용자가 멈춤(Ctrl-C)"
        self.finalize()
        return self.report()


def why_not(e: dict, s: dict) -> str:
    if e["ignored"]:
        return "--ignore"
    if e["kind"] == "net":
        return "바깥 자원"
    if not s["counted"]:
        return "붙기 전"
    return ""


def fatal(a, msg: str) -> int:
    if getattr(a, "json", False):
        print(json.dumps({"tool": TOOL, "port": getattr(a, "port", None), "error": msg, "exit": 2},
                         ensure_ascii=False, indent=1))
    else:
        print("돌릴 수 없음 — " + msg)
    return 2


def parse_widths(v):
    """'all' 또는 '420,900' → [420, 900]. 틀리면 ValueError(까닭)."""
    v = (v or "").strip().lower()
    if v in ("all", "모두"):
        return list(PRESET_WIDTHS)
    out = []
    for part in v.split(","):
        part = part.strip()
        if part.endswith("px"):
            part = part[:-2]
        if not part.isdigit():
            raise ValueError("--widths 는 'all' 또는 쉼표로 이은 숫자입니다(예: 420,900,1700): %s" % v)
        w = int(part)
        if not 320 <= w <= 3840:
            raise ValueError("--widths 의 폭은 320~3840 px 이어야 합니다: %d" % w)
        if w not in out:
            out.append(w)
    if not out or len(out) > 6:
        raise ValueError("--widths 는 1~6 가지입니다")
    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description="화면 오류 점검 — 격리 사본의 화면을 새로 고치고 모든 탭·설정 묶음을 열어 JS 예외·console.error·"
                    "기록 오류를 곳마다 모은다.",
        epilog="먼저 python3 akashi/iso.py start 로 격리 사본을 띄우십시오. 사용자의 앱에는 붙지 않습니다.\n"
               "종료 코드: 0 오류 없음 · 1 문제 있음 · 2 돌릴 수 없음(Ctrl-C 포함)",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=None,
                    help="격리 사본의 CDP 포트 (기본 %d · 환경 변수 AKASHI_PORT)" % DEFAULT_PORT)
    ap.add_argument("--no-reload", action="store_true",
                    help="새로 고치지 않고 지금 화면 그대로 탭만 돈다 (화면 로그 칸을 지키고 싶을 때)")
    ap.add_argument("--seconds", type=float, default=0.4,
                    help="탭·묶음 하나를 연 뒤 들을 초 (기본 0.4 · 설정 탭은 적어도 %.1f)" % SETTINGS_MIN_WAIT)
    ap.add_argument("--load-wait", type=float, default=6.5,
                    help="새로 고친 화면이 준비된 뒤 더 들을 초 (기본 6.5 · 시작 타이머가 6초까지 돈다)")
    ap.add_argument("--load-timeout", type=float, default=30.0,
                    help="새로 고친 화면이 준비되기를 기다릴 초 (기본 30)")
    ap.add_argument("--widths", metavar="W[,W…]|all",
                    help="이 폭들(CSS px)에서 한 바퀴씩 더 돈다. all = 420,900,1700 (좁음·보통·넓음). 끝나면 걷는다")
    ap.add_argument("--only", metavar="탭[,탭…]",
                    help="이 탭들만 연다 (예: settings,fanbox). settings 가 있으면 설정 묶음도")
    ap.add_argument("--ignore", action="append", default=[], metavar="글",
                    help="이 글이 든 오류는 판정에서 뺀다(목록에는 남음 · 대소문자 가리지 않음 · 여러 번 줄 수 있음)")
    ap.add_argument("--count-earlier", action="store_true",
                    help="붙기 전에 쌓여 있던 오류도 판정에 넣는다 (--no-reload 로 앱이 뜰 때의 오류를 따질 때)")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="곳마다 걸린 시간 · 오류마다 호출 자리 5단까지 · 정리 참고를 찍는다")
    ap.add_argument("--json", action="store_true", help="사람용 글 대신 JSON 한 덩어리로")
    a = ap.parse_args()

    if a.port is None:
        env = os.environ.get("AKASHI_PORT", "").strip()
        if env and not env.isdigit():
            return fatal(a, "환경 변수 AKASHI_PORT 가 숫자가 아닙니다: %r" % env[:20])
        a.port = int(env) if env else DEFAULT_PORT
    if not (0 < a.port < 65536):
        return fatal(a, "--port 가 올바르지 않습니다: %d" % a.port)
    if not (0.05 <= a.seconds <= 10):
        return fatal(a, "--seconds 는 0.05~10 초여야 합니다.")
    if not (0 <= a.load_wait <= 60):
        return fatal(a, "--load-wait 는 0~60 초여야 합니다.")
    if not (5 <= a.load_timeout <= 120):
        return fatal(a, "--load-timeout 은 5~120 초여야 합니다.")
    if a.widths is not None:
        try:
            a.widths = parse_widths(a.widths)
        except ValueError as e:
            return fatal(a, str(e))
    if a.only is not None:
        a.only = [x.strip() for x in a.only.split(",") if x.strip()]
        bad = [x for x in a.only if not NAME_RX.match(x)]
        if bad or not a.only:
            return fatal(a, "--only 는 영어 소문자 탭 이름을 쉼표로 잇습니다(예: settings,fanbox): %s" % ", ".join(bad))
    a.ignore = [x for x in (a.ignore or []) if x.strip()]
    if any(len(x) > 200 for x in a.ignore):
        return fatal(a, "--ignore 글은 200자까지입니다.")

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
        p = Page(ws_url, timeout=15)
    except (WsError, OSError) as e:
        return fatal(a, "화면에 붙지 못했습니다 (%s)" % e)
    with p:
        try:
            return ConsoleCheck(p, a, st).main()
        except KeyboardInterrupt:       # 준비 단계에서 멈춤 — 아직 아무것도 바꾸지 않았다
            return fatal(a, "사용자가 멈춤(Ctrl-C)")


if __name__ == "__main__":
    sys.exit(main())
