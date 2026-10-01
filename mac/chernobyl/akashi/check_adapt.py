#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""자동 배치 점검 — 기능 화면이 폭·높이에 맞춰 스스로 바뀌는 움직임(fxAdapt)을 격리 사본에서 하나씩 대 본다.

    python3 akashi/iso.py start                      # 먼저 격리 사본을 띄운다
    python3 akashi/check_adapt.py                    # 트위터 탭으로 아홉 가지
    python3 akashi/check_adapt.py --tab fanbox -v    # 다른 탭 · 단계마다 잰 값까지
    python3 akashi/check_adapt.py --json             # 기계용 결과 한 덩어리
    python3 akashi/iso.py stop

살피는 것 — 20dd34e 「기능 화면 자동 배치」 가 약속한 움직임:
  1 좁음 420px   수집을 시작하면 실행 화면으로 넘어가고, [실행] 위에 진행 점이 켜진다
  2 좁음         [설정] 을 눌러 돌아가도 진행 점은 그대로
  3 좁음         설정을 보는 중에 끝나면 초록 점(fx-done) · 위 띠에 마지막 줄
  4 좁음         [실행] 을 누르면 초록 점이 꺼진다(본 것으로 친다)
  5 보통 900px   「로그」 이름표를 누르면 아래 패널이 접히고, 수집을 시작하면 다시 펴진다
  6 보통         손잡이를 100px 위로 끌면 패널이 100px 커지고 비율(hanishiki.fxDockRatio)이 저장된다
  7 보통         창 높이를 760→560→900 으로 바꿔도 패널은 같은 비율(±3px)
  8 넓음 1700px  내용 3열 · 단추 줄은 오른쪽 실행 기둥 안
  9 모든 단계    「시작」 단추가 맨 위에서도, 맨 아래까지 스크롤해서도 보이고 가려지지 않는다
  끝나면 수집 표시·저장 값(fxDockRatio·fxDockMin)·패널 높이·보기·시험 로그 줄·탭·스크롤·창 크기·
  잠시 숨긴 안내창을 되돌리고, 되돌린 것을 다시 읽어 확인한다.

종료 코드: 0 모두 통과 · 1 문제 있음(실패·건너뜀·JS 예외·되돌리기 못 함·도중에 앱이 사라지거나 멎음)
          2 돌릴 수 없음(격리 사본 없음·포트 닫힘·화면 없음·대화 상자·그 탭이 수집 중·잘못된 인자·Ctrl-C)

★ 왜 격리 사본에만 붙나
  이 공구는 화면의 localStorage 를 바꾼다(손잡이를 끌고 접으면 앱이 저장한다). localStorage 는 앱의
  자료 폴더(프로필) 안에 산다. 사용자의 앱에 붙으면 사용자의 자료를 바꾸는 셈이다. 그래서 포트가 열려
  있는 것만 믿지 않고, iso.py 의 기록(state.json)에 있는 PID 가 살아 있고(실행 파일·뜬 시각까지 대조)
  그 PID(또는 그 자식)가 바로 이 포트를 듣고 있다고 lsof 로 확인될 때만 붙는다. 누가 듣는지 모르면
  붙지 않는다. 창이 여럿이면 본 창(index.html, 'window=' 없는 것)에만 붙는다 — 기능 창은 배치가 다르다.

★ 왜 JS 의 .click() 이 아니라 CDP 마우스로 누르나
  el.click() 은 무엇에 가려져 있어도 눌린다. 사람은 가려진 단추를 누를 수 없다. 그래서 누르기 전에
  그 자리의 맨 위 요소(elementFromPoint)가 바로 그 단추인지 보고, 화면 안쪽 마우스 입력
  (Input.dispatchMouseEvent)으로 누른다. 손잡이도 같은 길로 끈다(pointerdown·setPointerCapture 가 실제로
  돈다). OS 마우스를 흉내 내는 것은 아니다 — 이 맥에선 손쉬운 사용 권한이 없어 그건 안 된다.

★ 왜 누른 자리를 pointerdown 으로 다시 보나
  CDP 마우스 좌표와 화면의 CSS 좌표는 배율(zoomForWidth)이 끼면 어긋날 수 있다. 어긋나면 엉뚱한 곳이
  눌리고, 점검은 '앱이 틀렸다' 고 잘못 말한다. 그래서 누르는 동안만 문서에 pointerdown/pointerup 귀를
  달아, 누름이 겨눈 요소에 닿았는지와 끈 거리가 화면에서도 그만큼인지 재고 곧바로 뗀다. 어긋나면
  '앱 탓' 이 아니라 '좌표가 어긋남' 이라고 말한다.
  빈 바탕을 누르고 3px 넘게 끌면 앱은 창 옮기기(backend.winStartMove)를 부른다. 시험이 창을 움직여선
  안 되므로, 맨 위 요소가 손잡이·단추일 때만 누른다(손잡이는 끌기 자리가 아니다).

★ 왜 디스크 안내창을 잠시 숨기나
  빈 자료로 뜬 격리 사본은 임시 디스크 설정이 없어 처음에 「임시 저장 디스크 선택」 안내창(#disk-modal)이
  뜬다. 화면 전체를 덮어 사람도 아무것도 못 누른다. 그 창의 「확인」 과 같은 일(hidden 붙이기)만 하고,
  끝나면 다시 띄운다. 다른 대화 상자(이어받기 방향·캡처 설정·일괄 추가 등)는 무엇을 고를지 대신 정하지
  않는다 — 이름만 알리고 2 로 끝낸다.

★ 왜 수집 단추는 누르지 않나
  「수집 시작」 을 누르면 진짜 계정으로 바깥에 나간다. 자동 배치가 보는 것은 setRunning(판, 참/거짓) 뿐이라
  그것만 부른다 — 단추의 켜짐/꺼짐과 fxRunState 만 바뀌고, 수집은 일어나지 않는다. 이미 그 탭이 수집
  중이면(중지 단추가 켜져 있으면) 손대지 않고 2 로 끝난다.

★ 왜 되돌릴 때 손잡이를 한 번 더 끄나
  패널 비율(dockRatio)과 접힘(dockMin)은 화면 JS 의 닫힌 변수에 산다 — 밖에서 바꿀 길이 없다. 저장 값만
  되돌리면 화면은 새로 고칠 때까지 우리가 끈 높이로 남는다. 새로 고침은 탭·로그·백엔드 연결을 다 흔드니
  쓰지 않고, 사람이 하듯 손잡이를 원래 높이로 되끌고 이름표로 접힘을 맞춘 뒤, 저장 값은 원래 글자
  그대로(없었으면 지움) 써 넣는다. 화면 속 비율은 1px 안쪽까지만 맞출 수 있다(높이가 정수 px 라서).
  다음 새로 고침부터는 저장 값을 읽으므로 완전히 같다.

★ 왜 ±3px 이고, 기대값은 어떻게 셈하나
  앱은 비율을 소수 셋째 자리로 저장하고(toFixed(3)) 높이를 정수로 반올림한다. 높이 900 이면 저장 반올림만
  으로 0.4px, 반올림이 겹치면 1px 가 어긋날 수 있다. 기대값은 앱의 setDock 과 똑같이 센다:
  Math.round(max(140, min(화면높이×0.75, 비율×화면높이))) — 화면높이는 스크롤 영역에서 위아래 여백을 뺀 것.

★ 로그에 남기는 줄
  appendLog 로 넣는 두 줄은 누가 봐도 시험 줄인 글이고, data-akashi 표식을 달아 끝날 때 그 줄만 지운다.
  사용자·계정 이름은 넣지 않는다. 로그 글은 결과로 돌려받지도 않는다(들어 있는지 참/거짓만).
"""
from __future__ import annotations

import argparse
import json
import math
import os
import re
import sys
import time
from pathlib import Path
from urllib.parse import urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))
import iso  # noqa: E402  격리 사본 기록(state.json)을 iso.py 와 같은 규칙으로 읽는다
from lib import cdp as cdplib  # noqa: E402
from lib import paths, proc  # noqa: E402
from lib.cdp import CdpError, JsError, Page  # noqa: E402
from lib.ws import WsError  # noqa: E402

# ── 앱과 같은 수(index.html 의 fxAdapt) — 바뀌면 여기도 ─────────────────────────
K_RATIO = "hanishiki.fxDockRatio"
K_MIN = "hanishiki.fxDockMin"
DEFAULT_RATIO = 0.38          # 저장이 없거나 0·글자일 때 (parseFloat(v) || 0.38)
DOCK_MIN_PX = 140             # setDock 의 아래 한도
DOCK_MAX_FRAC = 0.75          # setDock 의 위 한도(화면 높이의 3/4)
AREA_MIN = 200                # areaH = max(200, H)

# ── 점검에 쓰는 크기 (CSS px — 앱 배율을 거친 '화면 안쪽' 크기) ─────────────────
NARROW = (420, 760)
MID = (900, 760)
WIDE = (1700, 900)
HEIGHTS = (760, 560, 900)
DRAG_UP = 100
TOL = 3
COLLAPSED_MAX = 110           # 접힌 패널은 손잡이+이름표뿐 — 실제 45~70px

MARK_RUN = "아카시 점검 — 시험 줄(수집 아님)"
MARK_DONE = "아카시 점검 — 완료 흉내(수집 아님)"

# 숨겨도 되는 안내창 — 「확인」 이 하는 일(hidden 붙이기)과 되돌리기(떼기)가 뚜렷한 것만
DISMISSABLE = {"#disk-modal": "임시 저장 디스크 선택 안내창"}

MODE_KO = {"wide": "넓음", "mid": "보통", "narrow": "좁음", "": "(없음)"}
STATUS_KO = {"pass": "통과", "fail": "실패", "skip": "건너뜀"}
VIS_KEYS = ("좁음·설정", "좁음·실행", "좁음·설정(실행 중)", "보통", "보통·접힘", "넓음")


# ── 화면 쪽 도우미 — 매번 함수 안에 넣어 돌린다(전역에 아무것도 남기지 않는다) ─────
JS_HEAD = r"""
var T = document.getElementById('tab-' + A.tab);
var S = document.querySelector('.scroll-area');
if (!T) throw new Error('탭이 없습니다: tab-' + A.tab);
if (!S) throw new Error('스크롤 영역(.scroll-area)이 없습니다');
var K1 = 'hanishiki.fxDockRatio', K2 = 'hanishiki.fxDockMin';
function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
function name(e) {
  if (!e || !e.tagName) return String(e);
  var s = e.tagName.toLowerCase();
  if (e.id) return s + '#' + e.id;
  var c = (typeof e.className === 'string') ? e.className.trim() : '';
  return c ? s + '.' + c.split(/\s+/).slice(0, 2).join('.') : s;
}
// 'ok' 또는 왜 사람이 못 보고 못 누르는지
function hit(e, fy) {
  if (!e) return '요소 없음';
  var cs = getComputedStyle(e);
  if (cs.display === 'none' || cs.visibility === 'hidden') return '숨음(' + cs.display + '/' + cs.visibility + ')';
  var b = e.getBoundingClientRect();
  if (b.width < 1 || b.height < 1) return '크기 0';
  if (b.top < -0.5 || b.left < -0.5 || b.bottom > innerHeight + 0.5 || b.right > innerWidth + 0.5)
    return '화면 밖(' + [b.left, b.top, b.right, b.bottom].map(Math.round).join(',') + ' / 화면 ' + innerWidth + 'x' + innerHeight + ')';
  // ★ 꺼진 단추(.btn:disabled)는 pointer-events:none — 수집 중의 「시작」 이 그렇다. 그대로 재면
  //   elementFromPoint 가 단추를 지나쳐 부모(단추 줄)를 돌려줘 '가려짐' 으로 잘못 적는다. 재는 동안만 받게 한다.
  var pe = e.style.pointerEvents, lift = cs.pointerEvents === 'none';
  if (lift) e.style.pointerEvents = 'auto';
  var h = document.elementFromPoint(b.left + b.width / 2, b.top + b.height * (fy == null ? 0.5 : fy));
  if (lift) e.style.pointerEvents = pe;
  if (!h) return '그 자리에 아무것도 없음';
  if (h !== e && !e.contains(h)) return '가려짐(' + name(h) + ')';
  return 'ok';
}
// 앱의 adapt() 와 같은 셈 — 스크롤 영역에서 여백을 뺀 폭·높이
function area() {
  var cs = getComputedStyle(S);
  return { H: S.clientHeight - (parseFloat(cs.paddingTop) || 0) - (parseFloat(cs.paddingBottom) || 0),
           W: S.getBoundingClientRect().width - (parseFloat(cs.paddingLeft) || 0) - (parseFloat(cs.paddingRight) || 0) };
}
// 색 변수의 실제 값 — 점 색을 '강조색/초록' 으로 가리는 데 쓴다(같은 층에서 재야 테마와 상관없다)
function probe(v) {
  var s = document.createElement('span');
  s.style.cssText = 'position:absolute;left:0;top:0;width:1px;height:1px;pointer-events:none;background:var(' + v + ')';
  T.appendChild(s); var c = getComputedStyle(s).backgroundColor; s.remove(); return c;
}
"""

JS_READY = """
return typeof window.fxAdapt === 'function' && typeof window.fxSetView === 'function'
    && typeof window.fxRunState === 'function' && typeof setRunning === 'function'
    && typeof appendLog === 'function' && typeof switchTab === 'function';
"""

JS_ORIG = """
var start = document.getElementById(A.tab + '-start-btn'), stop = document.getElementById(A.tab + '-stop-btn');
var act = document.querySelector('.tab-content.active');
var l = document.getElementById(A.tab + '-log');
return {
  fx: T.classList.contains('fx'), start: !!start, log: !!l,
  bar: !!T.querySelector(':scope > .fx-bar .fx-seg'), grip: !!T.querySelector(':scope > .fx-run > .fx-grip'),
  label: !!T.querySelector(':scope > .fx-run > .fx-log > .section-label'),
  running: T.classList.contains('fx-running') || !!(stop && !stop.disabled),
  leftover: l ? l.querySelectorAll(':scope > [data-akashi]').length : 0,
  done: T.classList.contains('fx-done'), dockMin: T.classList.contains('fx-dock-min'), view: T.dataset.fxview || '',
  lsRatio: lsGet(K1), lsMin: lsGet(K2), scroll: S.scrollTop,
  tabNow: (typeof currentTab !== 'undefined' && currentTab) ? String(currentTab) : (act ? act.id.replace(/^tab-/, '') : '')
};
"""

# 화면을 덮는 대화 상자 — 이름(#id·클래스)만 돌려받는다. 안의 글은 읽지 않는다
JS_DIALOGS = """
var out = [];
[].slice.call(document.querySelectorAll('.modal-overlay, .hx-modal, .batch-overlay, #resumeModeModal, #loginPauseFloat'))
  .forEach(function (e) {
    var cs = getComputedStyle(e);
    if (cs.display === 'none' || cs.visibility === 'hidden') return;
    var b = e.getBoundingClientRect();
    if (b.width < 2 || b.height < 2) return;
    out.push(e.id ? '#' + e.id : name(e));
  });
return out;
"""

JS_DIALOG_HIDE = """
var d = document.querySelector(A.sel);
if (!d || d.classList.contains('hidden')) return false;
d.classList.add('hidden'); return true;
"""

JS_DIALOG_SHOW = """
var d = document.querySelector(A.sel);
if (!d) return false;
d.classList.remove('hidden'); return !d.classList.contains('hidden');
"""

JS_STATE = """
var run = T.querySelector(':scope > .fx-run'), bar = T.querySelector(':scope > .fx-bar');
var bd = T.querySelector(':scope > .fx-bar .fx-seg .fx-badge');
var board = T.querySelector(':scope > .fx-board');
var btn = document.getElementById(A.tab + '-start-btn');
var row = btn && btn.closest('.btn-row');
var where = !row ? '없음'
          : (run && row.parentElement === run) ? 'run'
          : (bar && bar.contains(row) && row.parentElement.classList.contains('fx-bar-btns')) ? 'bar'
          : name(row.parentElement);
var st = bar && bar.querySelector('.fx-status');
var lb = run && run.querySelector('.log-box');
var a = area(), bcs = bd && getComputedStyle(bd);
return {
  mode: T.dataset.fxmode || '', view: T.dataset.fxview || '', active: T.classList.contains('active'),
  running: T.classList.contains('fx-running'), done: T.classList.contains('fx-done'), dockMin: T.classList.contains('fx-dock-min'),
  badge: bd ? { display: bcs.display, anim: bcs.animationName, bg: bcs.backgroundColor, hit: hit(bd) } : null,
  accent: probe('--accent'), success: probe('--success'),
  runH: run ? Math.round(run.getBoundingClientRect().height) : -1,
  runPos: run ? getComputedStyle(run).position : '',
  dockVar: T.style.getPropertyValue('--fx-dock-h').trim(),
  logBox: lb ? getComputedStyle(lb).display : '',
  barDisplay: bar ? getComputedStyle(bar).display : '',
  statusMark: !!(st && A.mark && st.textContent.indexOf(A.mark) >= 0),
  statusShown: !!(st && getComputedStyle(st).display !== 'none'),
  areaH: a.H, W: Math.round(a.W), cols: board ? (board.dataset.cols || '') : '',
  where: where, lsRatio: lsGet(K1), lsMin: lsGet(K2), vw: innerWidth, vh: innerHeight
};
"""

# 레이아웃이 가라앉았는지 — 모드·패널 높이·높이 변수·보기가 두 번 연달아 같으면
JS_SETTLE = """
var r = T.querySelector(':scope > .fx-run');
return [T.dataset.fxmode || '', r ? Math.round(r.getBoundingClientRect().height) : -1,
        T.style.getPropertyValue('--fx-dock-h'), S.clientHeight, T.dataset.fxview || '', T.className];
"""

JS_MODE = "return { m: T.dataset.fxmode || '', W: Math.round(area().W), vw: innerWidth, vh: innerHeight };"

JS_AIM = """
var e = document.querySelector(A.sel);
var h = hit(e, A.fy);
if (h !== 'ok') return { why: h };
var b = e.getBoundingClientRect();
return { why: 'ok', x: b.left + b.width / 2, y: b.top + b.height * A.fy };
"""

# 누르는 동안만 다는 귀 — 누름이 어디에 닿았고 화면에서 얼마나 움직였는지. 함수는 그 요소에 잠시 붙였다 뗀다
JS_ARM = """
var e = document.querySelector(A.sel);
if (!e) return false;
var rec = { hit: '', y0: null, y1: null };
function dn(ev) { if (rec.hit) return; rec.hit = (ev.target === e || e.contains(ev.target)) ? 'ok' : name(ev.target); rec.y0 = ev.clientY; }
function up(ev) { rec.y1 = ev.clientY; }
document.addEventListener('pointerdown', dn, true);
document.addEventListener('pointerup', up, true);
e.__akashiEar = { rec: rec, dn: dn, up: up };
return true;
"""

JS_DISARM = """
var e = document.querySelector(A.sel);
var k = e && e.__akashiEar;
if (!k) return null;
document.removeEventListener('pointerdown', k.dn, true);
document.removeEventListener('pointerup', k.up, true);
delete e.__akashiEar;
return k.rec;
"""

# 「시작」 단추가 맨 위·맨 아래 스크롤에서 보이나 — 스크롤은 제자리로
JS_START_VIS = """
var btn = document.getElementById(A.tab + '-start-btn');
var s0 = S.scrollTop, out = {};
S.scrollTop = 0; out.top = hit(btn);
S.scrollTop = S.scrollHeight; out.bottom = hit(btn);
S.scrollTop = s0;
return out;
"""

JS_LOG = """
appendLog(A.msg, A.kind, A.tab);
var l = document.getElementById(A.tab + '-log');
if (l && l.lastElementChild) l.lastElementChild.setAttribute('data-akashi', '1');
return 1;
"""

JS_STOP = "if (T.classList.contains('fx-running')) setRunning(A.tab, false); return 1;"
JS_START = "if (!T.classList.contains('fx-running')) setRunning(A.tab, true); return 1;"
JS_FRESH = ("if (T.classList.contains('fx-running')) setRunning(A.tab, false);"
            "fxSetView(T, 'set'); T.classList.remove('fx-done'); return 1;")

JS_PUT_LS = """
function put(k, v) { try { if (v === null || v === undefined) localStorage.removeItem(k); else localStorage.setItem(k, v); } catch (e) {} }
put(K1, A.r); put(K2, A.m);
return [lsGet(K1), lsGet(K2)];
"""

JS_CLEAN = """
var l = document.getElementById(A.tab + '-log');
if (l) [].slice.call(l.querySelectorAll(':scope > [data-akashi]')).forEach(function (e) { e.remove(); });
fxSetView(T, A.view || 'set');
T.classList.toggle('fx-done', !!A.done);
return 1;
"""

JS_VERIFY = """
var l = document.getElementById(A.tab + '-log');
var d = A.dlg ? document.querySelector(A.dlg) : null;
return { r: lsGet(K1), m: lsGet(K2), running: T.classList.contains('fx-running'),
         mine: l ? l.querySelectorAll(':scope > [data-akashi]').length : 0, view: T.dataset.fxview || '',
         done: T.classList.contains('fx-done'), dockMin: T.classList.contains('fx-dock-min'),
         dlgShown: d ? !d.classList.contains('hidden') : null,
         tab: (typeof currentTab !== 'undefined') ? String(currentTab) : '' };
"""


# ── 비밀 가리기 — 화면 오류 글에 토큰이 섞여 나와도 찍지 않는다 ──────────────────
_SECRET_KV = re.compile(
    r"(?i)\b(ct0|auth_token|sessionid|fanboxsessid|phpsessid|fb_dtsg|lsd|csrftoken|x-csrf-token|token|cookie|"
    r"password|passwd|api[_-]?key|secret|authorization)(\s*[=:]\s*)([^\s;&,'\"]+)")
_BEARER = re.compile(r"(?i)\b(bearer)(\s+)(\S+)")
_LONG = re.compile(r"[A-Za-z0-9%_\-+/=.]{32,}")
# 긴 덩어리 중 글자와 점뿐인 것(Emulation.setDeviceMetricsOverride 같은 이름)은 토큰일 수 없어 남긴다
_IDENT = re.compile(r"[A-Za-z]+(?:\.[A-Za-z]+)+")


def _mask_long(m) -> str:
    t = m.group(0)
    return t if _IDENT.fullmatch(t) else "<가림 %d자>" % len(t)


def scrub(text) -> str:
    """이름 붙은 비밀(ct0=…·token:…·Bearer …)과 32자 넘는 토큰 모양 덩어리를 길이만 남기고 가린다."""
    s = str(text)
    s = s.replace(str(paths.real_home()), "~")
    s = _SECRET_KV.sub(lambda m: "%s%s<가림 %d자>" % (m.group(1), m.group(2), len(m.group(3))), s)
    s = _BEARER.sub(lambda m: "%s%s<가림 %d자>" % (m.group(1), m.group(2), len(m.group(3))), s)
    return _LONG.sub(_mask_long, s)


def js_round(x: float) -> int:
    """JS Math.round — 파이썬 round() 는 .5 를 짝수로 보내 1px 어긋난다."""
    return int(math.floor(x + 0.5))


def js_parse_float(v):
    """JS parseFloat 처럼 앞쪽 숫자만 읽는다. 못 읽으면 None."""
    if v is None:
        return None
    m = re.match(r"\s*([+-]?(?:\d+\.?\d*|\.\d+)(?:[eE][+-]?\d+)?)", str(v))
    return float(m.group(1)) if m else None


def expected_dock(ratio: float, area_h: float) -> int:
    """앱의 setDock(dockRatio * areaH) 과 같은 셈."""
    return js_round(max(DOCK_MIN_PX, min(area_h * DOCK_MAX_FRAC, ratio * area_h)))


def show_ls(v) -> str:
    return "없음" if v is None else "'%s'" % v


def verdict(checks, good: str):
    bad = [msg for ok, msg in checks if not ok]
    return (True, good) if not bad else (False, " · ".join(bad))


def num(v, default: int = -1) -> int:
    """화면이 null·글자를 돌려줘도 셈이 터지지 않게."""
    try:
        return int(round(float(v)))
    except (TypeError, ValueError):
        return default


def fatal(a, msg: str) -> int:
    if a.json:
        print(json.dumps({"tool": "check_adapt", "port": a.port, "tab": a.tab, "error": msg, "exit": 2},
                         ensure_ascii=False, indent=1))
    else:
        print("돌릴 수 없음 — " + msg)
    return 2


class Skip(Exception):
    """앞 단계 때문에 이 시험을 할 수 없다."""


class Abort(Exception):
    """앱과 연결이 끊겼거나 앱이 답하지 않는다 — 더 할 수 없다."""


# ── 격리 사본인지 ────────────────────────────────────────────────────────────
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
    pid = int(st["pid"])
    lp = proc.listening_pids(port)
    if not lp:
        return ("포트 %d 를 누가 듣는지 확인하지 못했습니다(lsof 가 답하지 않음) — 격리 사본인지 모르니 붙지 않습니다."
                % port), None
    if pid not in lp and not (set(lp) & set(proc.children(pid))):
        return ("포트 %d 를 격리 사본(PID %d)이 아닌 프로세스(PID %s)가 듣고 있습니다 — 붙지 않습니다."
                % (port, pid, ", ".join(str(x) for x in lp))), None
    return None, st


# ── 점검 ────────────────────────────────────────────────────────────────────
class AdaptCheck:
    def __init__(self, p: Page, tab: str, a):
        self.p, self.tab, self.a = p, tab, a
        self.results: list = []
        self.vis: dict = {}
        self.orig = None
        self.narrow_ok = self.mid_ok = False
        self.saved_ratio = None
        self.pending_dialogs: list = []   # 떠 있던, 숨겨도 되는 안내창(prepare 가 찾는다)
        self.hidden_dialog = ""           # 실제로 잠시 숨긴 안내창의 선택자(없으면 빈 글)
        self.restore_notes: list = []

    # ── 말하기 ───────────────────────────────────────────────────────────
    def say(self, line: str = "") -> None:
        if not self.a.json:
            print(line, flush=True)

    def record(self, no: int, title: str, status: str, why: str) -> None:
        self.results.append({"no": no, "title": title, "status": status, "why": why})
        self.say("%2d %s  %s" % (no, STATUS_KO[status], title))
        self.say("       %s" % why)

    # ── 화면 다루기 ──────────────────────────────────────────────────────
    @staticmethod
    def _call(fn, *args, **kw):
        """화면에 묻는 모든 길이 여기를 지난다 — 끊김·멎음은 Abort 로 바꾼다.
        cdp.Page.cmd 는 답이 없으면 'N초 안에 답이 없습니다' 로 던진다. 멎은 앱에 계속 물으면 부를
        때마다 제한 시간만큼 기다리므로, 한 번 멎으면 그만둔다. 화면 JS 오류(JsError)는 그대로 올린다."""
        try:
            return fn(*args, **kw)
        except (WsError, OSError) as e:
            raise Abort("앱과 연결이 끊겼습니다 (%s)" % scrub(e)[:160])
        except CdpError as e:
            if not isinstance(e, JsError) and "답이 없습니다" in str(e):
                raise Abort("앱이 답하지 않습니다 (%s)" % scrub(e)[:160])
            raise

    def js(self, body: str, **kw):
        kw.setdefault("tab", self.tab)
        kw.setdefault("mark", MARK_DONE)
        expr = "(function(A){%s\n%s\n})(%s)" % (JS_HEAD, body, json.dumps(kw, ensure_ascii=False))
        return self._call(self.p.eval, expr)

    def cmd(self, method: str, params: dict | None = None):
        return self._call(self.p.cmd, method, params)

    def state(self) -> dict:
        s = self.js(JS_STATE) or {}
        if self.a.verbose:
            keep = ("mode", "view", "running", "done", "dockMin", "runH", "dockVar", "areaH", "W", "cols",
                    "where", "lsRatio", "lsMin", "vw", "vh")
            self.say("       · " + " ".join("%s=%s" % (k, s.get(k)) for k in keep))
        return s

    def _poll(self, body: str, timeout: float, every: float = 0.1, **kw) -> bool:
        end = time.time() + timeout
        while True:
            if self.js(body, **kw):
                return True
            if time.time() >= end:
                return False
            time.sleep(every)

    def settle(self, timeout: float = 2.5) -> None:
        """ResizeObserver → requestAnimationFrame → adapt() 가 끝날 때까지(값이 두 번 연달아 같을 때까지)."""
        last, same = None, 0
        end = time.time() + timeout
        while time.time() < end:
            v = self.js(JS_SETTLE)
            if v == last:
                same += 1
                if same >= 2:
                    return
            else:
                last, same = v, 0
            time.sleep(0.1)

    def goto(self, size, mode: str):
        """창 크기를 흉내 내고 그 모드가 되기를 기다린다. 안 되면 까닭 글, 되면 None."""
        w, h = size
        self._call(self.p.size, w, h, settle=self.a.settle)
        if not self._poll("return T.dataset.fxmode === A.m;", timeout=5.0, m=mode):
            s = self.js(JS_MODE) or {}
            return "%dx%d 에서 %s 이 아니라 %s (내용 폭 %spx · 화면 %sx%s)" % (
                w, h, MODE_KO[mode], MODE_KO.get(s.get("m"), s.get("m")), s.get("W"), s.get("vw"), s.get("vh"))
        self.settle()
        return None

    def _wait_view(self, v: str, timeout: float = 2.0) -> bool:
        return self._poll("return T.dataset.fxview === A.v;", timeout=timeout, v=v)

    def aim(self, sel: str, fy: float = 0.5):
        r = self.js(JS_AIM, sel=sel, fy=fy) or {}
        if r.get("why") != "ok":
            return None, "누를 수 없음 — %s" % (r.get("why") or "?")
        return (r["x"], r["y"]), None

    def _press(self, sel: str, fy: float, act, dy: float = 0.0):
        """겨눈 요소가 맨 위일 때만 누르고, 누름이 거기 닿았는지(끌면 거리까지) 다시 본다.
        (★ 왜 누른 자리를 pointerdown 으로 다시 보나 참고) 잘못이면 까닭 글, 되면 None."""
        xy, err = self.aim(sel, fy)
        if err:
            return err
        if not self.js(JS_ARM, sel=sel):
            return "누를 수 없음 — 요소 없음"
        try:
            self._call(act, xy[0], xy[1])
        finally:
            try:
                rec = self.js(JS_DISARM, sel=sel)
            except (Abort, CdpError):
                rec = None
        time.sleep(0.15)
        self.settle()
        if not rec:
            return "누름 기록을 읽지 못함(요소가 바뀜?)"
        if not rec.get("hit"):
            return "누름이 화면에 닿지 않음(pointerdown 없음)"
        if rec.get("hit") != "ok":
            return "누름이 %s 에 닿음 — CDP 좌표와 화면 좌표가 어긋남(배율?)" % rec.get("hit")
        if dy:
            y0, y1 = rec.get("y0"), rec.get("y1")
            if y0 is None or y1 is None:
                return "끌기가 끝나지 않음(pointerup 없음)"
            if abs((y1 - y0) - dy) > 1:
                return "%+dpx 끌었는데 화면에는 %+.0fpx 로 닿음 — CDP 좌표와 화면 좌표가 어긋남(배율?)" % (dy, y1 - y0)
        return None

    def click(self, sel: str, fy: float = 0.5):
        """사람처럼 누른다 — 가려져 있으면 누르지 않고 까닭을 돌려준다."""
        def act(x, y):
            self.cmd("Input.dispatchMouseEvent", {"type": "mouseMoved", "x": x, "y": y, "button": "none", "buttons": 0})
            self.cmd("Input.dispatchMouseEvent", {"type": "mousePressed", "x": x, "y": y, "button": "left",
                                                  "buttons": 1, "clickCount": 1})
            self.cmd("Input.dispatchMouseEvent", {"type": "mouseReleased", "x": x, "y": y, "button": "left",
                                                  "buttons": 0, "clickCount": 1})
        return self._press(sel, fy, act)

    def drag(self, sel: str, dy: float, fy: float = 0.3):
        """누르고 dy 만큼 세로로 끈다(위는 음수). 손잡이는 12px 이고 아래 6px 이 로그 칸과 겹쳐
        위쪽 30% 자리를 잡는다."""
        dy = int(round(dy))
        return self._press(sel, fy, lambda x, y: self.p.mouse_drag(x, y, 0, dy, steps=8), dy=dy)

    def log_line(self, msg: str, kind: str) -> None:
        self.js(JS_LOG, msg=msg, kind=kind)

    def check_start(self, key: str) -> None:
        self.vis[key] = self.js(JS_START_VIS) or {}

    def sel(self, rest: str) -> str:
        return "#tab-%s > %s" % (self.tab, rest)

    def need(self, ok: bool, what: str) -> None:
        if not ok:
            raise Skip("%s 이 되지 않아 건너뜀" % what)

    @property
    def label_sel(self) -> str:
        return self.sel(".fx-run > .fx-log > .section-label")

    @property
    def grip_sel(self) -> str:
        return self.sel(".fx-run > .fx-grip")

    # ── 1~4 좁음 ──────────────────────────────────────────────────────────
    def s1(self):
        # 깨끗한 출발 — 멈춤 · 설정 보기 · 표시 없음
        self.js(JS_FRESH)
        err = self.goto(NARROW, "narrow")
        if err:
            return False, err
        self.narrow_ok = True
        self.js(JS_FRESH)
        self.settle()
        self.check_start("좁음·설정")
        self.log_line(MARK_RUN, "info")
        self.js("setRunning(A.tab, true); return 1;")
        self._wait_view("run")
        self.settle()
        s = self.state()
        self.check_start("좁음·실행")
        b = s.get("badge") or {}
        return verdict([
            (s.get("view") == "run", "보기가 실행으로 넘어가지 않음(view=%s)" % (s.get("view") or "없음")),
            (s.get("running"), "fx-running 이 붙지 않음"),
            (b.get("display") not in (None, "none") and b.get("hit") == "ok",
             "[실행] 위 진행 점이 안 보임(display=%s, %s)" % (b.get("display"), b.get("hit"))),
            (b.get("bg") == s.get("accent"), "진행 점 색이 강조색이 아님(%s, 기대 %s)" % (b.get("bg"), s.get("accent"))),
            (b.get("anim") not in (None, "", "none"), "진행 점이 깜빡이지 않음(animation=%s)" % b.get("anim")),
        ], "view=run · fx-running · [실행] 위 진행 점(강조색·깜빡임) 보임")

    def s2(self):
        self.need(self.narrow_ok, "좁음(420px)")
        # 1번 결과와 상관없이 같은 출발점: 실행 중 · 실행 보기
        self.js(JS_START)
        self.js("fxSetView(T, 'run'); return 1;")
        self.settle()
        err = self.click(self.sel('.fx-bar .fx-seg [data-v="set"]'))
        if err:
            return False, "[설정] " + err
        s = self.state()
        self.check_start("좁음·설정(실행 중)")
        b = s.get("badge") or {}
        return verdict([
            (s.get("view") == "set", "[설정] 을 눌러도 보기가 그대로(view=%s)" % (s.get("view") or "없음")),
            (s.get("running"), "실행 중 표시(fx-running)가 사라짐"),
            (b.get("display") not in (None, "none") and b.get("hit") == "ok",
             "진행 점이 꺼짐(display=%s, %s)" % (b.get("display"), b.get("hit"))),
            (b.get("bg") == s.get("accent"), "진행 점 색이 바뀜(%s, 기대 %s)" % (b.get("bg"), s.get("accent"))),
        ], "view=set · 실행 중 표시 그대로 · [실행] 위 진행 점 켜짐")

    def s3(self):
        self.need(self.narrow_ok, "좁음(420px)")
        self.js(JS_START)
        self.js("fxSetView(T, 'set'); return 1;")
        self.log_line(MARK_DONE, "success")
        self.js("setRunning(A.tab, false); return 1;")
        self.settle()
        s = self.state()
        b = s.get("badge") or {}
        return verdict([
            (s.get("done"), "설정을 보는 중에 끝났는데 fx-done 이 붙지 않음"),
            (not s.get("running"), "끝났는데 fx-running 이 남음"),
            (b.get("display") not in (None, "none") and b.get("hit") == "ok",
             "완료 점이 안 보임(display=%s, %s)" % (b.get("display"), b.get("hit"))),
            (b.get("bg") == s.get("success"), "점 색이 초록(--success)이 아님(%s, 기대 %s)" % (b.get("bg"), s.get("success"))),
            (b.get("anim") in ("none", ""), "완료 점이 깜빡임(animation=%s) — 멈춰 있어야 함" % b.get("anim")),
            (s.get("statusMark") and s.get("statusShown"), "위 띠에 마지막 줄(완료)이 안 보임"),
        ], "fx-done · [실행] 위 초록 점(멈춤) · 위 띠에 마지막 줄")

    def s4(self):
        self.need(self.narrow_ok, "좁음(420px)")
        self.js(JS_STOP)
        self.js("fxSetView(T, 'set'); return 1;")
        note = ""
        if not self.js("return T.classList.contains('fx-done');"):
            # 3번이 실패해도 '누르면 지워지는가' 는 따로 볼 수 있다
            self.js("T.classList.add('fx-done'); return 1;")
            note = " (3번에서 표시가 안 붙어 손으로 붙이고 시험)"
        err = self.click(self.sel('.fx-bar .fx-seg [data-v="run"]'))
        if err:
            return False, "[실행] " + err
        s = self.state()
        b = s.get("badge") or {}
        return verdict([
            (s.get("view") == "run", "[실행] 을 눌러도 보기가 그대로(view=%s)" % (s.get("view") or "없음")),
            (not s.get("done"), "보았는데 fx-done 이 남음"),
            (b.get("display") == "none", "점이 꺼지지 않음(display=%s)" % b.get("display")),
        ], "view=run · fx-done 지워짐 · 점 꺼짐" + note)

    # ── 5~7 보통 ──────────────────────────────────────────────────────────
    def _open_dock(self, s: dict):
        """접혀 있으면 이름표로 편다. (상태, 까닭 글 또는 None)"""
        if not s.get("dockMin"):
            return s, None
        err = self.click(self.label_sel)
        if err:
            return s, "접힌 패널을 펴려 했으나 「로그」 이름표 " + err
        s = self.state()
        if s.get("dockMin"):
            return s, "접힌 패널이 「로그」 이름표를 눌러도 펴지지 않음"
        return s, None

    def s5(self):
        self.js(JS_FRESH)
        err = self.goto(MID, "mid")
        if err:
            return False, err
        self.mid_ok = True
        s0 = self.state()
        s, err = self._open_dock(s0)
        if err:
            return False, "처음에 " + err
        pre = " (처음엔 접혀 있어 먼저 폄)" if s0.get("dockMin") else ""
        self.check_start("보통")
        h_open = num(s.get("runH"))
        err = self.click(self.label_sel)
        if err:
            return False, "「로그」 이름표 " + err
        c = self.state()
        self.check_start("보통·접힘")
        h_min = num(c.get("runH"))
        ok1, why1 = verdict([
            (c.get("dockMin"), "눌러도 fx-dock-min 이 붙지 않음"),
            (0 < h_min <= COLLAPSED_MAX and h_min < h_open * 0.6,
             "접어도 높이 %dpx (펼침 %dpx · 접힘은 %dpx 이하여야 함)" % (h_min, h_open, COLLAPSED_MAX)),
            (c.get("logBox") == "none", "접었는데 로그 상자가 보임(display=%s)" % c.get("logBox")),
            (c.get("lsMin") == "1", "접힘이 저장되지 않음(%s=%s)" % (K_MIN, show_ls(c.get("lsMin")))),
        ], "")
        if not ok1:
            return False, "접기: " + why1
        self.js("setRunning(A.tab, true); return 1;")
        self.settle()
        o = self.state()
        h_back = num(o.get("runH"))
        ok2, why2 = verdict([
            (not o.get("dockMin"), "수집을 시작해도 접힌 채"),
            (abs(h_back - h_open) <= TOL, "다시 편 높이 %dpx ≠ 처음 %dpx" % (h_back, h_open)),
            (o.get("logBox") not in ("none", ""), "로그 상자가 안 보임(display=%s)" % o.get("logBox")),
            (o.get("lsMin") == "0", "펼침이 저장되지 않음(%s=%s)" % (K_MIN, show_ls(o.get("lsMin")))),
        ], "")
        if not ok2:
            return False, "시작 뒤 펴기: " + why2
        return True, "「로그」 이름표 → 접힘 %d→%dpx · 수집을 시작하면 다시 %dpx%s" % (h_open, h_min, h_back, pre)

    def s6(self):
        self.need(self.mid_ok, "보통(900px)")
        self.js(JS_STOP)              # 5번이 켜 둔 수집 표시 — 이 시험은 멈춘 상태에서
        s, err = self._open_dock(self.state())
        if err:
            return False, err
        area_h = max(AREA_MIN, float(s.get("areaH") or 0))
        hi = area_h * DOCK_MAX_FRAC
        h0 = num(s.get("runH"))
        note = ""
        if h0 + DRAG_UP > hi - 4:
            # 위 한도에 걸려 100px 를 다 못 큰다 — 먼저 화면의 35% 로 낮춘다
            target = js_round(area_h * 0.35)
            if target < DOCK_MIN_PX or target + DRAG_UP > hi - 4:
                return False, "화면이 낮아 %dpx 끌 자리가 없음(화면 높이 %.0fpx · 한도 %.0fpx)" % (DRAG_UP, area_h, hi)
            err = self.drag(self.grip_sel, h0 - target)
            if err:
                return False, "손잡이(낮추기) " + err
            s = self.state()
            if abs(num(s.get("runH")) - target) > TOL:
                return False, "손잡이를 %dpx 로 낮추려 끌었지만 %dpx" % (target, num(s.get("runH")))
            h0 = num(s.get("runH"))
            note = " (위 한도에 걸려 먼저 %dpx 로 낮춤)" % h0
        err = self.drag(self.grip_sel, -DRAG_UP)
        if err:
            return False, "손잡이 " + err
        e = self.state()
        h1 = num(e.get("runH"))
        r = js_parse_float(e.get("lsRatio"))
        self.saved_ratio = r
        ok, why = verdict([
            (h1 > h0, "끌어도 커지지 않음(%d→%dpx)" % (h0, h1)),
            (abs(h1 - (h0 + DRAG_UP)) <= TOL, "%dpx 커져야 하는데 %+dpx" % (DRAG_UP, h1 - h0)),
            (r is not None, "비율이 저장되지 않음(%s=%s)" % (K_RATIO, show_ls(e.get("lsRatio")))),
            (r is None or abs(r * area_h - h1) <= TOL,
             "저장 비율 %s × 화면 높이 %.0f = %.0fpx 가 실제 %dpx 와 다름" % (e.get("lsRatio"), area_h, (r or 0) * area_h, h1)),
        ], "")
        if not ok:
            return False, why
        return True, "손잡이 %dpx 위로 → %d→%dpx · 저장 비율 %s (= %d / %.0f)%s" % (
            DRAG_UP, h0, h1, e.get("lsRatio"), h1, area_h, note)

    def s7(self):
        self.need(self.mid_ok, "보통(900px)")
        r = self.saved_ratio
        if r is None:
            r = js_parse_float(self.js("return lsGet(K1);"))
        if r is None:
            raise Skip("저장된 비율(%s)이 없어 건너뜀 — 6번 참고" % K_RATIO)
        good, bad = [], []
        for h in HEIGHTS:
            err = self.goto((MID[0], h), "mid")
            if err:
                bad.append("높이 %d: %s" % (h, err))
                continue
            s = self.state()
            area_h = max(AREA_MIN, float(s.get("areaH") or 0))
            exp = expected_dock(r, area_h)
            got = num(s.get("runH"))
            lim = "" if DOCK_MIN_PX < r * area_h < area_h * DOCK_MAX_FRAC else " · 한도에 걸림"
            if s.get("dockMin"):
                bad.append("높이 %d: 패널이 접혀 있음" % h)
            elif abs(got - exp) > TOL:
                bad.append("높이 %d: %dpx (기대 %dpx = %.3f × %.0f%s)" % (h, got, exp, r, area_h, lim))
            else:
                good.append("%d→%dpx%s" % (h, got, lim))
        if bad:
            return False, " · ".join(bad)
        return True, "창 높이 %s (비율 %.3f · ±%dpx)" % (" · ".join(good), r, TOL)

    # ── 8 넓음 ────────────────────────────────────────────────────────────
    def s8(self):
        self.js(JS_STOP)
        err = self.goto(WIDE, "wide")
        if err:
            return False, err
        s = self.state()
        self.check_start("넓음")
        where = {"run": "실행 기둥", "bar": "위 띠"}.get(s.get("where"), s.get("where"))
        return verdict([
            (s.get("cols") == "3", "내용 %s열 — 3열이어야 함(내용 폭 %spx)" % (s.get("cols") or "?", s.get("W"))),
            (s.get("where") == "run", "단추 줄이 실행 기둥 밖(%s)" % where),
            (s.get("barDisplay") == "none", "넓음인데 위 띠가 보임(display=%s)" % s.get("barDisplay")),
            (s.get("runPos") == "sticky", "실행 기둥이 따라오지 않음(position=%s)" % s.get("runPos")),
        ], "내용 3열 · 단추 줄은 실행 기둥 안 · 기둥은 붙박이(sticky) · 내용 폭 %spx" % s.get("W"))

    # ── 9 시작 단추 ──────────────────────────────────────────────────────
    def s9(self):
        # 앞 시험이 도중에 멈춰 못 잰 자리는 '안 보임' 이 아니다 — 잰 곳에서 안 보이면 실패, 못 잰 곳만 있으면 건너뜀
        if not self.vis:
            raise Skip("어느 단계에도 이르지 못해 잴 수 없었음")
        bad, unmeasured = [], []
        for k in VIS_KEYS:
            v = self.vis.get(k)
            if v is None:
                unmeasured.append(k)
                continue
            for pos, ko in (("top", "맨 위"), ("bottom", "맨 아래")):
                if v.get(pos) != "ok":
                    bad.append("%s·%s: %s" % (k, ko, v.get(pos) or "?"))
        tail = (" (못 잰 곳: %s)" % " · ".join(unmeasured)) if unmeasured else ""
        if bad:
            return False, " · ".join(bad) + tail
        if unmeasured:
            raise Skip("잰 %d곳은 모두 보이지만 %s 은 앞 시험이 멈춰 재지 못함"
                       % (len(VIS_KEYS) - len(unmeasured), " · ".join(unmeasured)))
        return True, "%s — 맨 위·맨 아래 스크롤 모두 보이고 가려지지 않음" % " · ".join(VIS_KEYS)

    SCENARIOS = (
        (1, "좁음 420px — 시작하면 실행 화면으로", "s1"),
        (2, "좁음 — [설정] 을 눌러도 진행 점은 그대로", "s2"),
        (3, "좁음 — 설정을 보는 중에 끝나면 초록 점", "s3"),
        (4, "좁음 — [실행] 을 누르면 초록 점이 꺼짐", "s4"),
        (5, "보통 900px — 「로그」 이름표로 접기, 시작하면 다시 펴짐", "s5"),
        (6, "보통 — 손잡이를 100px 위로 끌면 커지고 비율을 저장", "s6"),
        (7, "보통 — 창 높이 760→560→900 에도 같은 비율", "s7"),
        (8, "넓음 1700px — 3열, 단추는 실행 기둥 안", "s8"),
        (9, "모든 단계 — 「시작」 단추가 보임", "s9"),
    )

    def run_one(self, no: int, title: str, fn) -> None:
        try:
            ok, why = fn()
            status = "pass" if ok else "fail"
        except Skip as e:
            status, why = "skip", str(e)
        except JsError as e:
            status, why = "fail", "화면 JS 오류 — " + scrub(e)
        except CdpError as e:
            status, why = "fail", scrub(e)
        except (Abort, KeyboardInterrupt):
            raise
        except Exception as e:  # 공구 쪽 잘못이어도 되돌리기까지는 가야 한다
            status, why = "fail", "점검 공구 안의 오류 — %s: %s" % (type(e).__name__, scrub(e)[:200])
        self.record(no, title, status, why)

    # ── 되돌리기 ─────────────────────────────────────────────────────────
    def _restore_dock(self) -> None:
        """화면 속 비율·접힘을 사람이 하듯 손잡이·이름표로 되돌린다(★ 왜 되돌릴 때 … 참고)."""
        o = self.orig
        r0 = js_parse_float(o.get("lsRatio")) or DEFAULT_RATIO
        err = self.goto(MID, "mid")
        if err:
            raise CdpError("보통 폭이 되지 않음 — " + err)
        s = self.state()
        if s.get("dockMin"):
            err = self.click(self.label_sel)
            if err:
                raise CdpError("「로그」 이름표 " + err)
            s = self.state()
        area_h = max(AREA_MIN, float(s.get("areaH") or 0))
        target = expected_dock(r0, area_h)
        if abs(num(s.get("runH")) - target) > 1:
            err = self.drag(self.grip_sel, num(s.get("runH")) - target)
            if err:
                raise CdpError("손잡이 " + err)
            s = self.state()
            if abs(num(s.get("runH")) - target) > 1:
                self.restore_notes.append("패널 높이 %dpx(원래 %dpx) — 저장 값은 원래대로라 다음 새로 고침에 맞춰짐"
                                          % (num(s.get("runH")), target))
        if bool(o.get("dockMin")) != bool(s.get("dockMin")):
            err = self.click(self.label_sel)
            if err:
                raise CdpError("접힘을 되돌리려 했으나 「로그」 이름표 " + err)

    def restore(self) -> bool:
        o = self.orig
        if o is None:
            return True
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

        step("수집 표시 끄기", lambda: self.js(JS_STOP))
        step("패널 높이·접힘", self._restore_dock)
        step("저장 값", lambda: self.js(JS_PUT_LS, r=o.get("lsRatio"), m=o.get("lsMin")))
        step("시험 줄·표시·보기", lambda: self.js(JS_CLEAN, view=o.get("view") or "set", done=bool(o.get("done"))))
        step("창 크기", lambda: self.cmd("Emulation.clearDeviceMetricsOverride"))
        time.sleep(max(0.4, self.a.settle))
        step("탭·스크롤", lambda: self.js(
            "if (A.orig && A.orig !== A.tab && typeof switchTab === 'function') switchTab(A.orig);"
            "S.scrollTop = A.st; return 1;", orig=o.get("tabNow") or "", st=o.get("scroll") or 0))
        if self.hidden_dialog:
            step("안내창 다시 띄우기", lambda: self.js(JS_DIALOG_SHOW, sel=self.hidden_dialog))
        try:
            v = self.js(JS_VERIFY, dlg=self.hidden_dialog) or {}
            miss = []
            if v.get("r") != o.get("lsRatio"):
                miss.append("%s=%s(원래 %s)" % (K_RATIO, show_ls(v.get("r")), show_ls(o.get("lsRatio"))))
            if v.get("m") != o.get("lsMin"):
                miss.append("%s=%s(원래 %s)" % (K_MIN, show_ls(v.get("m")), show_ls(o.get("lsMin"))))
            if v.get("running"):
                miss.append("수집 표시가 켜진 채")
            if v.get("mine"):
                miss.append("시험 로그 줄 %d개가 남음" % v.get("mine"))
            if (o.get("view") or "set") != v.get("view"):
                miss.append("보기 %s(원래 %s)" % (v.get("view"), o.get("view") or "set"))
            if bool(v.get("done")) != bool(o.get("done")):
                miss.append("완료 표시(fx-done)가 원래와 다름")
            if bool(v.get("dockMin")) != bool(o.get("dockMin")):
                miss.append("패널 접힘이 원래와 다름")
            if o.get("tabNow") and v.get("tab") and v.get("tab") != o.get("tabNow"):
                miss.append("탭 %s(원래 %s)" % (v.get("tab"), o.get("tabNow")))
            if self.hidden_dialog and v.get("dlgShown") is False:
                miss.append("숨긴 안내창(%s)이 다시 뜨지 않음" % self.hidden_dialog)
            if miss:
                ok = False
                self.restore_notes.append("확인: " + " · ".join(miss))
        except (JsError, CdpError) as e:
            ok = False
            self.restore_notes.append("되돌린 뒤 확인 못 함 — " + scrub(e)[:200])
        return ok

    # ── 본 흐름 ──────────────────────────────────────────────────────────
    def prepare(self):
        """붙은 뒤 첫 확인. 돌릴 수 없으면 까닭 글, 되면 None. 여기서는 아무것도 바꾸지 않는다."""
        if not self._poll(JS_READY, timeout=20, every=0.4):
            return "화면에 자동 배치(fxAdapt·fxSetView·fxRunState)가 없습니다 — 20dd34e 이전 빌드이거나 화면이 아직 준비 중입니다."
        o = self.js(JS_ORIG) or {}
        if not o.get("fx"):
            return "tab-%s 는 기능 화면(.tab-content.fx)이 아닙니다." % self.tab
        parts = (("start", "시작 단추"), ("log", "로그 상자"), ("bar", "위 띠(.fx-seg)"),
                 ("grip", "손잡이(.fx-grip)"), ("label", "「로그」 이름표"))
        miss = [ko for k, ko in parts if not o.get(k)]
        if miss:
            return "tab-%s 에 %s 이(가) 없습니다 — 이 탭으로는 점검할 수 없습니다." % (self.tab, ", ".join(miss))
        if o.get("running"):
            hint = ""
            if o.get("leftover"):
                hint = (" 로그에 지난 점검의 시험 줄 %d개가 남아 있어, 지난 점검이 도중에 끊긴 흔적일 수 있습니다 —"
                        " 격리 사본을 다시 띄우십시오(iso.py stop → iso.py start)." % o.get("leftover"))
            return "%s 탭이 수집 중입니다(중지 단추가 켜져 있음) — 끝난 뒤 다시 돌리십시오.%s" % (self.tab, hint)
        dialogs = self.js(JS_DIALOGS) or []
        others = [d for d in dialogs if d not in DISMISSABLE]
        if others:
            return ("화면에 대화 상자가 떠 있습니다(%s) — 무엇을 고를지 대신 정하지 않습니다. 닫은 뒤 다시 돌리십시오."
                    % ", ".join(others))
        self.orig = o
        self.pending_dialogs = [d for d in dialogs if d in DISMISSABLE]
        return None

    def hide_dialogs(self) -> None:
        for d in self.pending_dialogs:
            if self.js(JS_DIALOG_HIDE, sel=d):
                self.hidden_dialog = d
                self.say("  %s(%s)이 떠 있어 잠시 숨김 — 끝나면 다시 띄웁니다" % (DISMISSABLE[d], d))

    def main(self, st: dict) -> int:
        try:
            why = self.prepare()
        except Abort as e:
            return fatal(self.a, str(e))
        except CdpError as e:
            return fatal(self.a, "화면을 읽지 못했습니다 — " + scrub(e))
        if why:
            return fatal(self.a, why)

        # 이 뒤로 난 화면 JS 예외만 센다 — enable 하면 지난 콘솔 글이 한꺼번에 다시 오므로 비운다
        try:
            self.cmd("Runtime.enable")
            self.cmd("Log.enable")
            self.p.pump(0.4)
        except (Abort, CdpError) as e:
            return fatal(self.a, "화면 기록을 켜지 못했습니다 — " + scrub(e))
        self.p.events.clear()

        self.say("자동 배치 점검 — 포트 %d · 탭 %s · 앱 %s (격리 사본 PID %s)"
                 % (self.a.port, self.tab, st.get("version") or "?", st.get("pid")))
        if self.a.verbose:
            self.say("  처음 상태: 보기=%s 접힘=%s %s=%s %s=%s 탭=%s" % (
                self.orig.get("view") or "set", self.orig.get("dockMin"), K_RATIO, show_ls(self.orig.get("lsRatio")),
                K_MIN, show_ls(self.orig.get("lsMin")), self.orig.get("tabNow") or "?"))

        aborted, lost, interrupted, unstarted = "", False, False, False
        try:
            self.hide_dialogs()
            self.say("")
            self.js("switchTab(A.tab); return 1;")
            for no, title, fn in self.SCENARIOS:
                self.run_one(no, title, getattr(self, fn))
        except Abort as e:
            aborted, lost = str(e), True
        except KeyboardInterrupt:
            aborted, interrupted = "사용자가 멈춤(Ctrl-C)", True
        except CdpError as e:        # 탭을 열거나 안내창을 숨기다 난 화면 오류 — 되돌리기는 한다
            aborted, unstarted = "점검을 시작하지 못함 — " + scrub(e), True

        restored = False
        if not lost:
            try:
                restored = self.restore()
            except Abort as e:
                aborted, lost = aborted or str(e), True
            except KeyboardInterrupt:
                interrupted = True
                aborted = aborted or "되돌리는 중에 사용자가 멈춤(Ctrl-C)"
                self.restore_notes.append("되돌리는 중에 멈춤(Ctrl-C)")

        exc, other = [], []
        if not lost:
            try:
                self.p.pump(0.3)
            except (WsError, OSError):
                pass
            for e in self.p.collect_errors():
                (exc if e.startswith("예외") else other).append(scrub(e))

        return self.report(aborted, lost, interrupted or unstarted, restored, exc, other)

    def report(self, aborted: str, lost: bool, no_verdict: bool, restored: bool, exc: list, other: list) -> int:
        """no_verdict = Ctrl-C 또는 시작도 못 함 — 판정이 아니라 '돌릴 수 없음'(2)."""
        fails = [r["no"] for r in self.results if r["status"] == "fail"]
        skips = [r["no"] for r in self.results if r["status"] == "skip"]
        self.say("")
        if not lost:
            self.say("   화면 JS 예외 %d건%s" % (len(exc), "" if not other else " (참고: console.error·기록 오류 %d건)" % len(other)))
            for e in exc[:5]:
                self.say("       " + e[:240])
            if self.a.verbose:
                for e in other[:5]:
                    self.say("       참고 " + e[:240])
            if self.orig is not None:
                what = "수집 표시·저장 값·패널·보기·시험 줄·탭·스크롤·창 크기" + ("·안내창" if self.hidden_dialog else "")
                self.say("   정리 — %s" % ((what + " 되돌림") if restored else "다 되돌리지 못함"))
                for n in self.restore_notes:
                    self.say("       " + n)
        else:
            self.say("   정리 못 함 — 앱과 끊겼거나 앱이 멎어 되돌리지 못했습니다. 격리 사본을 다시 띄우십시오(iso.py stop → start).")
        if aborted:
            self.say("   도중에 멈춤 — " + aborted)

        total = len(self.SCENARIOS)
        parts = []
        if fails:
            parts.append("실패 %s번" % "·".join(str(n) for n in fails))
        if skips:
            parts.append("건너뜀 %s번" % "·".join(str(n) for n in skips))
        if exc:
            parts.append("JS 예외 %d건" % len(exc))
        not_restored = self.orig is not None and not lost and not restored
        if not_restored:
            parts.append("되돌리기 못 함")
        if aborted:
            parts.append("끝까지 못 돎(%d/%d)" % (len(self.results), total))
        problems = len(fails) + len(skips) + (1 if exc else 0) + (1 if not_restored else 0) + (1 if aborted else 0)
        if problems == 0 and len(self.results) == total:
            line = "통과 — 자동 배치 %d가지가 모두 맞고, 화면 JS 예외 없이 원래대로 되돌렸습니다" % total
            code = 0
        elif no_verdict and not lost:
            # Ctrl-C·시작 못 함은 판정이 아니다(돌리다 만 것) — 앱이 사라지거나 멎은 것은 문제로 친다(아래)
            rest = [x for x in parts if not x.startswith("끝까지")]
            line = "돌릴 수 없음 — %s · %d/%d 까지 돎%s" % (
                aborted or "도중에 멈춤", len(self.results), total, (" · " + " · ".join(rest)) if rest else "")
            code = 2
        else:
            problems = max(problems, 1)
            line = "문제 %d건 — %s" % (problems, " · ".join(parts) or "결과가 모자람")
            code = 1

        if self.a.json:
            print(json.dumps({
                "tool": "check_adapt", "port": self.a.port, "tab": self.tab,
                "results": self.results, "start_button": self.vis,
                "js_exceptions": exc, "console_errors": len(other),
                "dialog_hidden": self.hidden_dialog, "restored": restored, "restore_notes": self.restore_notes,
                "aborted": aborted, "problems": problems if code else 0, "verdict": line, "exit": code,
            }, ensure_ascii=False, indent=1))
        else:
            self.say(line)
        return code


def main() -> int:
    ap = argparse.ArgumentParser(
        description="자동 배치 점검 — 기능 화면의 좁음/보통/넓음 움직임(fxAdapt)을 격리 사본에서 아홉 가지로 대 본다.",
        epilog="먼저 python3 akashi/iso.py start 로 격리 사본을 띄우십시오. 사용자의 앱에는 붙지 않습니다.\n"
               "끝나면 바꾼 것(수집 표시·패널 비율/접힘 저장 값·보기·시험 로그 줄·탭·창 크기)을 되돌립니다.\n"
               "종료 코드: 0 모두 통과 · 1 문제 있음 · 2 돌릴 수 없음",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=os.environ.get("AKASHI_PORT") or 9334,   # ★ 글이면 argparse 가 int 로 — 빈 값·숫자 아님도 넘어지지 않고 종료 2
                    help="격리 사본의 CDP 포트 (기본 9334 · 환경 변수 AKASHI_PORT)")
    ap.add_argument("--tab", default="twitter",
                    help="시험할 기능 탭 이름 (기본 twitter · 시작 단추와 로그가 있는 탭: bluesky, fanbox, pixiv …)")
    ap.add_argument("--settle", type=float, default=0.6,
                    help="창 크기를 바꾼 뒤 먼저 기다릴 초 (기본 0.6 · 그 뒤엔 값이 멎을 때까지 스스로 기다림)")
    ap.add_argument("-v", "--verbose", action="store_true", help="단계마다 잰 값(모드·보기·높이·열 수·저장 값)을 찍는다")
    ap.add_argument("--json", action="store_true", help="사람용 글 대신 JSON 한 덩어리로")
    a = ap.parse_args()

    if not re.fullmatch(r"[a-z]{2,20}", a.tab or ""):
        return fatal(a, "--tab 은 영어 소문자 탭 이름이어야 합니다(예: twitter).")
    if not (0 < a.port < 65536):
        return fatal(a, "--port 가 올바르지 않습니다: %d" % a.port)
    if a.settle < 0 or a.settle > 10:
        return fatal(a, "--settle 은 0~10 초여야 합니다.")

    try:
        err, st = guard(a.port)
        if err:
            return fatal(a, err)
        try:
            ts = [t for t in cdplib.page_targets(a.port) if "window=" not in t.get("url", "")]
        except (OSError, ValueError) as e:
            return fatal(a, "포트 %d 에서 화면 목록을 읽지 못했습니다 (%s)" % (a.port, scrub(e)))
        if not ts:
            return fatal(a, "포트 %d 에 본 창(index.html) 화면이 없습니다 — 화면이 준비될 때까지 기다린 뒤 다시." % a.port)
        ws_url = ts[0].get("webSocketDebuggerUrl") or ""
        # 주소는 앱이 알려 준 것 — 그래도 이 기계(127.0.0.1) 밖으로는 나가지 않는다
        if urlparse(ws_url).hostname not in ("127.0.0.1", "localhost", "::1"):
            return fatal(a, "화면 주소가 이 기계(127.0.0.1)가 아닙니다 — 붙지 않습니다.")
        try:
            p = Page(ws_url, timeout=15)
        except (WsError, OSError) as e:
            return fatal(a, "화면에 붙지 못했습니다 (%s)" % scrub(e))
        with p:
            return AdaptCheck(p, a.tab, a).main(st)
    except KeyboardInterrupt:          # 붙기 전·확인 중에 멈춤 — 아직 아무것도 바꾸지 않았다
        return fatal(a, "사용자가 멈춤(Ctrl-C)")


if __name__ == "__main__":
    sys.exit(main())
