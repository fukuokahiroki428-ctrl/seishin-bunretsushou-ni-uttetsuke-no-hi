#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""배치 점검 — 창 폭을 몇 가지로 바꿔 가며 기능 화면 열넷과 설정 화면이 제대로 놓이는지 격리 사본에서 잰다.

    python3 akashi/iso.py start                                   # 먼저 격리 사본을 띄운다
    python3 akashi/check_layout.py                                # 폭 1180 1000 800 600 420 × 기능 화면 + 설정
    python3 akashi/check_layout.py --widths 1440 900 --tabs twitter,settings
    python3 akashi/check_layout.py --seed-targets 17              # 긴 대상 목록(sample_user_01…)에서 한 줄 높이
    python3 akashi/check_layout.py --shots "$TMPDIR/akashi-layout" # 폭·화면마다 그림 한 장
    python3 akashi/check_layout.py --json                         # 기계용 결과 한 덩어리
    python3 akashi/iso.py stop

폭(CSS px — 앱 배율을 거친 '화면 안쪽' 폭)마다, 화면마다 보는 것:
  모드       기능 화면의 data-fxmode(넓음·보통·좁음)가 실제 내용 폭과 맞나(≥860 넓음 · ≥560 보통)
  열         내용 판의 data-cols 와 실제로 그려진 격자 열 수가 같나 (설정은 격자 열 수를 알려 줌)
  시작 단추  단추 줄의 첫 .btn-primary(넓음은 실행 기둥, 보통·좁음은 위 띠)가 맨 위 첫 화면 안에
             통째로 있고, 가운데·양 끝 세 점 모두에서 맨 위 요소가 바로 그 단추인가(가려지지 않았나)
  옆넘침     .scroll-area 의 scrollWidth − clientWidth > 1 — 오른쪽이 잘려 안 보이는 내용이 있다
  삐져나옴   오른쪽 끝이 화면 끝을 넘는데, 안쪽의 overflow:hidden/clip 조상이 잘라 주지 않는 요소
  액자       액자(.fx-frame·.fx-run·.sg)끼리 겹침 · 판 밖으로 나감 · 액자가 안의 내용을 옆으로 자름 ·
             액자 안에 따로 생긴 세로 스크롤(6f0f90b 「액자 안의 스크롤을 없앤다」 되돌이 막기)
  창 전체    문서가 창보다 넓어지지 않나 (폭마다 한 번)
  --seed-targets N 을 주면 twitter 대상 목록을 잠시 N 명(sample_user_01…)으로 바꿔 한 줄 높이를 잰다
  (한 사람 한 줄 ≈33px · 45px 을 넘으면 두 줄로 접힌 것).

종료 코드: 0 모두 통과 · 1 문제 있음(배치 문제·JS 예외·그림 못 찍음·되돌리기 못 함·도중에 앱이 사라짐)
          2 돌릴 수 없음(격리 사본 없음·포트 닫힘·화면 준비 안 됨·잘못된 인자·Ctrl-C)

★ 왜 격리 사본에만 붙나
  탭을 옮기고, 설정 묶음을 「전체」 로 바꾸고(localStorage 에 남는다), 대상 목록을 바꿔 끼운다. 사용자의
  앱에 붙으면 사용자의 화면·저장 값을 흔드는 셈이다. 그래서 check_adapt.py 와 같은 규칙으로, iso.py 의
  기록(state.json)에 있는 PID 가 살아 있고(실행 파일·뜬 시각까지 대조) 그 PID(또는 자식)가 바로 이
  포트를 듣고 있을 때만 붙는다. 창이 여럿이면 본 창(index.html, 'window=' 없는 것)에만 붙는다.

★ 왜 '삐져나옴' 을 조상의 자름까지 따져 가리나
  .scroll-area 는 overflow-x:hidden 이라, 화면 끝을 넘는 것은 모두 거기서 잘려 '안 보이게' 된다. 우리가
  찾는 것이 바로 그 잘림의 범인이므로 스크롤 영역은 자르는 조상으로 치지 않고, 그 안쪽 조상만 본다.
  위 띠의 마지막 줄(.fx-status)처럼 일부러 줄임표로 자르는 칸 안의 글은 그 칸이 잘라 주므로 빼야
  헛경보가 없다. position:absolute 는 담는 블록(offsetParent) 아래의 조상이 자르지 못하므로 그 사이는
  건너뛰고, position:fixed 는 창에 붙은 것이라 아예 뺀다. 여러 겹이 함께 넘치면 맨 바깥 것만 알린다.

★ 왜 액자까지 보나
  액자 벽돌 쌓기(layoutFrames)는 액자 키를 재서 4px 줄을 몇 개 차지할지 JS 로 적는다. 키가 바뀌었는데
  다시 재지 않으면 다음 액자가 위로 올라와 겹친다 — 폭이 바뀔 때 가장 잘 난다. 액자는 overflow:hidden
  이라 안의 칸이 액자보다 넓으면 '조용히' 잘린다. 가장 가까이서 실제로 자른 조상이 액자일 때만 알린다
  (입력 칸·로그 상자·줄임표 칸이 자른 것은 일부러 그렇게 만든 것이다).

★ 왜 좁음에선 [설정] 보기로 재나
  좁음은 한 번에 한 쪽만 보인다. 배치가 까다로운 쪽은 설정(내용 액자)이고, 시작 단추는 두 보기 모두
  위 띠에 있다. 그래서 그 탭이 [실행] 을 보고 있었으면 [설정] 으로 돌려 재고, 끝나면 원래 보기와
  완료 표시(fx-done — [실행] 으로 돌리면 앱이 지운다)를 그대로 되돌린다.

★ 왜 '멎을 때까지' 기다리나
  폭을 바꾸거나 탭을 옮기면 ResizeObserver → requestAnimationFrame → fxAdapt·fitFxBoard·layoutFrames 가
  한두 박자 늦게 돈다. 고정 시간만 쉬면 반쯤 바뀐 배치를 재서 헛경보가 난다. 모드·열·스크롤 크기·액자
  자리들이 세 번 연달아 같을 때 잰다. 끝내 멎지 않으면(살아 있는 갱신일 수도 있어) 참고로만 적는다.

★ 왜 대상을 심는 동안 설정 저장을 미루나
  대상 목록(platformTargets)은 saveConfig 가 부르면 설정 파일에 그대로 적힌다. 점검 도중 무엇이든
  saveConfig 를 부르면 sample_user_… 가 사본의 설정에 남는다. 그래서 심어 둔 동안만 saveConfig 를
  감싸 '미뤄 두고', 원래 목록을 되돌린 뒤 미룬 것이 있으면 한 번 저장한다(그 사이 바뀐 다른 값도
  잃지 않는다). 원래 목록은 화면 안에 그대로 쥐고 있다가 되돌린다 — 파이썬으로 가져오지 않는다
  (--seed-config 사본이면 진짜 대상 이름이다). 도중에 끊겨 남았으면 다음 실행이 먼저 되돌린다.

★ 왜 그림을 찍을 때 입력 칸을 가리나
  --seed-config 사본에는 쿠키·세션·토큰이 평문 입력 칸(twitter-ct0, fanbox-session …)에 들어 있다. 그림도
  기록이다. 찍는 순간에만 모든 글 입력 칸·글 상자를 -webkit-text-security 로 점으로 바꾸고 곧바로
  푼다(칸 크기는 그대로라 배치는 달라지지 않는다). 재는 동안에는 가리지 않는다.
"""
from __future__ import annotations

import argparse
import base64
import json
import os
import re
import sys
import time
import unicodedata
from pathlib import Path
from urllib.parse import urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))
import iso  # noqa: E402  격리 사본 기록(state.json)을 iso.py 와 같은 규칙으로 읽는다
from lib import cdp as cdplib  # noqa: E402
from lib import paths, proc  # noqa: E402
from lib.cdp import CdpError, JsError, Page  # noqa: E402
from lib.ws import WsError  # noqa: E402

# ── 앱과 같은 수(index.html 의 fxAdapt) — 바뀌면 여기도 ─────────────────────────
WIDE_MIN = 860                # W >= 860 넓음
NARROW_MIN = 560              # 560 <= W < 860 보통, 그 아래 좁음

DEFAULT_WIDTHS = (1180, 1000, 800, 600, 420)
DEFAULT_HEIGHT = 760
TOL = 1                       # px — 소수점 배치(배율)의 반올림 흔들림
ROW_EXPECT = 33               # 대상 한 줄(.batch-target-item) — @container 규칙으로 한 줄에 든 높이
ROW_MAX = 45                  # 이보다 크면 두 줄로 접힌 것
SEED_PLAT = "twitter"         # 긴 목록을 심는 판(대상 목록이 있고 스키마가 가장 넓다)
K_GROUP = "hanishiki.settingsGroup"
SETTINGS_GROUP = "all"        # 설정은 묶음 여섯을 다 펼쳐 잰다(열 수가 의미 있는 것은 이때뿐)
MASK_ID = "akashi-layout-mask"
MASK_CSS = ("input:not([type=checkbox]):not([type=radio]):not([type=range]):not([type=color])"
            ":not([type=button]):not([type=submit]):not([type=reset]):not([type=file]):not([type=image]),"
            "textarea,[contenteditable]:not([contenteditable=false])"
            "{-webkit-text-security:disc !important}")

MODE_KO = {"wide": "넓음", "mid": "보통", "narrow": "좁음", "": "(없음)"}
WHERE_KO = {"bar": "위띠", "run": "기둥", "board": "판"}


# ── 화면 쪽 도우미 — 매번 함수 안에 넣어 돌린다(전역에 아무것도 남기지 않는다) ─────
JS_HEAD = r"""
var S = document.querySelector('.scroll-area');
if (!S) throw new Error('스크롤 영역(.scroll-area)이 없습니다');
function lsGet(k) { try { return localStorage.getItem(k); } catch (e) { return null; } }
function lsPut(k, v) { try { if (v === null || v === undefined) localStorage.removeItem(k); else localStorage.setItem(k, v); } catch (e) {} }
// 요소 이름 — 태그·id·클래스 둘까지. 글 내용은 절대 담지 않는다(대상 이름·로그가 섞이지 않게)
function name(e) {
  if (!e || !e.tagName) return String(e);
  var s = e.tagName.toLowerCase();
  if (e.id) s += '#' + e.id;
  else {
    var c = (typeof e.className === 'string') ? e.className.trim() : '';
    if (c) s += '.' + c.split(/\s+/).slice(0, 2).join('.');
    var g = e.getAttribute && e.getAttribute('data-group');     // 설정 액자(.sg)는 묶음 이름으로 가린다
    if (g) s += '[' + g + ']';
  }
  return s.replace(/[^\w#.\-\[\]]/g, '').slice(0, 48);
}
function shown(e) {
  if (!e) return false;
  var cs = getComputedStyle(e);
  if (cs.display === 'none' || cs.visibility === 'hidden') return false;
  var r = e.getBoundingClientRect();
  return r.width >= 1 && r.height >= 1;
}
// 실제로 그려진 격자 열 수 — 계산된 값은 'Npx Npx …' 로 풀려 나온다
function tracks(b) {
  return String(getComputedStyle(b).gridTemplateColumns || '').split(/\s+/)
    .filter(function (t) { return /px$/.test(t); }).length;
}
function curTab() {
  if (typeof currentTab !== 'undefined' && currentTab) return String(currentTab);
  var a = document.querySelector('.tab-content.active');
  return a ? a.id.replace(/^tab-/, '') : '';
}
"""

JS_READY = """
return typeof switchTab === 'function' && typeof window.fxAdapt === 'function'
    && typeof window.fxSetView === 'function' && typeof window.layoutFrames === 'function'
    && !!document.querySelector('.tab-content.fx');
"""

# 처음 상태 — 끝나면 이대로 되돌린다. 값(글)은 담지 않는다: 이름·수·참거짓뿐
JS_ORIG = """
var fx = [].map.call(document.querySelectorAll('.tab-content.fx'), function (t) {
  return { tab: t.id.replace(/^tab-/, ''), view: t.dataset.fxview || '', done: t.classList.contains('fx-done') };
});
var tabs = [].map.call(document.querySelectorAll('.scroll-area > .tab-content[id^="tab-"]'), function (t) {
  return t.id.replace(/^tab-/, '');
});
var chip = document.querySelector('#tab-settings .sf-chip.active');
var app = document.querySelector('.app');
var hasT = typeof platformTargets !== 'undefined' && platformTargets && Array.isArray(platformTargets[A.plat]);
return {
  tab: curTab(), scroll: S.scrollTop, fx: fx, tabs: tabs, settings: !!document.getElementById('tab-settings'),
  group: chip ? chip.getAttribute('data-group') : null, lsGroup: lsGet(A.kGroup),
  navOpen: !!(app && app.classList.contains('nav-open-narrow')),
  stale: !!window.__akashiLayout, staleMask: !!document.getElementById(A.maskId),
  targets: hasT ? platformTargets[A.plat].length : -1,
  canSeed: hasT && typeof renderPlatformTargetList === 'function',
  canGroup: typeof setSettingsGroup === 'function'
};
"""

# 배치가 멎었는지 — 이 값이 세 번 연달아 같으면 잰다
JS_SIG = """
var T = document.getElementById('tab-' + (A.tab || curTab())) || document.querySelector('.tab-content.active');
var b = T && (T.querySelector(':scope > .fx-board') || T.querySelector('.settings-board'));
var fr = b ? [].map.call(b.children, function (f) {
  var r = f.getBoundingClientRect();
  return [Math.round(r.left), Math.round(r.top), Math.round(r.width), Math.round(r.height), f.style.gridRowEnd || ''];
}) : [];
return JSON.stringify([innerWidth, innerHeight, S.clientWidth, S.scrollWidth, S.scrollHeight,
  T ? (T.dataset.fxmode || '') : '', b ? (b.dataset.cols || '') : '', T ? T.classList.contains('active') : false,
  T ? (T.dataset.fxview || '') : '', fr]);
"""

# 그 탭으로 — 설정이면 묶음 「전체」, 좁음이면 [설정] 보기, 스크롤은 맨 위(첫 화면)
JS_PREP = """
if (curTab() !== A.tab) switchTab(A.tab);
var T = document.getElementById('tab-' + A.tab);
if (!T) throw new Error('탭이 없습니다: tab-' + A.tab);
var ch = { group: false, view: false };
if (A.tab === 'settings' && A.group && typeof setSettingsGroup === 'function') {
  var act = document.querySelector('#tab-settings .sf-chip.active');
  if (!act || act.getAttribute('data-group') !== A.group) { setSettingsGroup(A.group); ch.group = true; }
}
if (T.classList.contains('fx') && T.dataset.fxmode === 'narrow' && T.dataset.fxview && T.dataset.fxview !== 'set'
    && typeof window.fxSetView === 'function') { fxSetView(T, 'set'); ch.view = true; }
S.scrollTop = 0;
return { active: T.classList.contains('active'), changed: ch };
"""

JS_MEASURE = r"""
var T = document.getElementById('tab-' + A.tab);
if (!T) throw new Error('탭이 없습니다: tab-' + A.tab);
var TOL = A.tol;
S.scrollTop = 0;                                   // '첫 화면' 은 맨 위에서
var scs = getComputedStyle(S), sr = S.getBoundingClientRect();
var padL = parseFloat(scs.paddingLeft) || 0, padR = parseFloat(scs.paddingRight) || 0;
// 화면 끝 = 창 끝과 스크롤 영역이 자르는 자리(스크롤 막대 안쪽) 중 가까운 것
var limit = Math.min(innerWidth, sr.left + S.clientLeft + S.clientWidth);
var de = document.documentElement;
var fx = T.classList.contains('fx');
var out = {
  active: T.classList.contains('active'), fx: fx, vw: innerWidth, vh: innerHeight,
  W: Math.round((sr.width - padL - padR) * 10) / 10, limit: Math.round(limit),
  saOver: Math.round(S.scrollWidth - S.clientWidth), docOver: Math.round(de.scrollWidth - de.clientWidth),
  mode: fx ? (T.dataset.fxmode || '') : '', view: fx ? (T.dataset.fxview || '') : '',
  cols: '', grid: null, used: null, start: null, frames: 0,
  offN: 0, offenders: [], overlaps: [], outside: [], cut: [], inner: [], rows: null
};

// ── 액자 ──
var board = fx ? T.querySelector(':scope > .fx-board') : T.querySelector('.settings-board');
var frames = [];
if (board && shown(board)) {
  out.grid = tracks(board);
  frames = [].filter.call(board.children, function (f) {
    return (f.classList.contains('fx-frame') || f.classList.contains('fx-run') || f.classList.contains('sg'))
        && !f.hidden && shown(f);
  });
}
out.cols = fx ? (board ? (board.dataset.cols || '') : '') : (out.grid === null ? '' : String(out.grid));
if (!fx && board) {
  var xs = {};
  frames.forEach(function (f) { xs[Math.round(f.getBoundingClientRect().left)] = 1; });
  out.used = Object.keys(xs).length;
}
out.frames = frames.length;
function fname(f) {
  var i = frames.indexOf(f);
  var kind = f.classList.contains('sg') ? 'sg' : f.classList.contains('fx-run') ? 'fx-run' : 'fx-frame';
  var g = f.getAttribute('data-group');
  var t = f.querySelector(':scope > .fx-title, :scope > .sg-title');
  var label = t ? t.textContent.replace(/\s+/g, ' ').trim().slice(0, 16) : '';   // 앱이 박아 둔 액자 제목
  return kind + (g ? '[' + g + ']' : i >= 0 ? '#' + (i + 1) : '') + (label ? ' 「' + label + '」' : '');
}
var rects = frames.map(function (f) { return f.getBoundingClientRect(); });
var br = board ? board.getBoundingClientRect() : null;
for (var i = 0; i < frames.length; i++) {
  var a = rects[i];
  if (br && (a.right > br.right + TOL || a.left < br.left - TOL))
    out.outside.push(fname(frames[i]) + ' ' + Math.round(a.left) + '~' + Math.round(a.right) + 'px (판 '
                     + Math.round(br.left) + '~' + Math.round(br.right) + 'px)');
  for (var j = i + 1; j < frames.length; j++) {
    var b = rects[j];
    var ow = Math.min(a.right, b.right) - Math.max(a.left, b.left), oh = Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top);
    if (ow > TOL && oh > TOL)
      out.overlaps.push(fname(frames[i]) + ' ↔ ' + fname(frames[j]) + ' ' + Math.round(ow) + '×' + Math.round(oh) + 'px');
  }
  var bodies = frames[i].querySelectorAll(':scope > .fx-body, :scope > .sg-body');
  for (var q = 0; q < bodies.length; q++) {
    var bd = bodies[q];
    if (getComputedStyle(bd).overflowY !== 'visible' && bd.scrollHeight > bd.clientHeight + 2)
      out.inner.push(fname(frames[i]) + ' ' + (bd.scrollHeight - bd.clientHeight) + 'px');
  }
}

// ── 자르는 조상 ── (★ 왜 '삐져나옴' 을 … 참고)
var memo = new Map();
function info(e) {
  var v = memo.get(e);
  if (v) return v;
  var cs = getComputedStyle(e);
  var clip = cs.overflowX !== 'visible' || /\b(paint|strict|content)\b/.test(cs.contain || '');
  var edge = Infinity;
  if (clip) { var r = e.getBoundingClientRect(); edge = r.left + e.clientLeft + e.clientWidth; }   // 안쪽(padding) 끝
  v = { pos: cs.position, vis: cs.visibility, clip: clip, edge: edge };
  memo.set(e, v);
  return v;
}
// e 를 자를 수 있는 조상들, 가까운 것부터 [조상, 자르는 x]. 창에 붙은 것이면 null.
function clippers(e) {
  var me = info(e);
  if (me.pos === 'fixed') return null;
  var list = [], esc = me.pos === 'absolute' ? e.offsetParent : null;
  for (var p = e.parentElement; p && p !== S; p = p.parentElement) {
    var pi = info(p);
    if (pi.pos === 'fixed') return null;
    if (esc && p === esc) esc = null;              // 담는 블록에 닿음 — 여기부터는 자른다
    if (!esc && pi.clip) list.push([p, pi.edge]);
    if (!esc && pi.pos === 'absolute') esc = p.offsetParent;
  }
  return list;
}
var run = fx ? T.querySelector(':scope > .fx-run') : null;
var cutters = new Set(frames);
if (run && shown(run)) cutters.add(run);
var offs = [], offSet = new Set(), cuts = [], cutSet = new Set();
var els = T.getElementsByTagName('*');
for (var k = 0; k < els.length; k++) {
  var e = els[k];
  var r = e.getBoundingClientRect();
  if (r.width < 0.5 || r.height < 0.5) continue;
  var fr = e.closest('.fx-frame, .fx-run, .sg');
  var fe = (fr && fr !== e && cutters.has(fr)) ? info(fr).edge : Infinity;
  if (r.right <= limit + TOL && r.right <= fe + TOL) continue;       // 액자 안·화면 안 — 볼 것 없음
  if (info(e).vis === 'hidden') continue;
  var cl = clippers(e);
  if (!cl) continue;
  var cutter = null, eff = r.right;
  for (var c = 0; c < cl.length; c++) {
    if (!cutter && cl[c][1] < r.right - TOL) cutter = cl[c][0];     // 실제로 자른 가장 가까운 조상
    if (cl[c][1] < eff) eff = cl[c][1];
  }
  if (cutter && cutters.has(cutter)) { cuts.push([e, cutter, r.right - info(cutter).edge]); cutSet.add(e); }
  if (eff > limit + TOL) { offs.push([e, eff, r.width]); offSet.add(e); }
}
// 여러 겹이 함께 넘치면 맨 바깥 것만
function under(e, set, stop) {
  for (var p = e.parentElement; p && p !== stop && p !== S; p = p.parentElement) if (set.has(p)) return true;
  return false;
}
var topOffs = offs.filter(function (o) { return !under(o[0], offSet, null); });
out.offN = topOffs.length;
out.offenders = topOffs.slice(0, 8).map(function (o) { return { el: name(o[0]), right: Math.round(o[1]), w: Math.round(o[2]) }; });
var byCutter = new Map();
cuts.forEach(function (c) {
  if (under(c[0], cutSet, c[1])) return;
  var g = byCutter.get(c[1]) || { frame: fname(c[1]), n: 0, px: 0, els: [] };
  g.n++; g.px = Math.max(g.px, Math.round(c[2]));
  if (g.els.length < 3) g.els.push(name(c[0]));
  byCutter.set(c[1], g);
});
byCutter.forEach(function (g) { out.cut.push(g); });

// ── 시작 단추 ── 단추 줄은 넓음이면 실행 기둥, 보통·좁음이면 위 띠에 있다
function startInfo(btn) {
  if (!btn) return { why: '없음' };
  var nm = name(btn), where = btn.closest('.fx-bar') ? 'bar' : btn.closest('.fx-run') ? 'run' : 'board';
  var cs = getComputedStyle(btn);
  if (cs.display === 'none' || cs.visibility === 'hidden') return { why: '숨음', el: nm, where: where };
  var r = btn.getBoundingClientRect();
  if (r.width < 1 || r.height < 1) return { why: '크기 0', el: nm, where: where };
  var top = Math.max(0, sr.top), bot = Math.min(innerHeight, sr.bottom), left = Math.max(0, sr.left);
  var rect = [r.left, r.top, r.right, r.bottom].map(Math.round);
  if (r.top < top - 0.5 || r.bottom > bot + 0.5 || r.left < left - 0.5 || r.right > limit + 0.5)
    return { why: '첫 화면 밖', el: nm, where: where, rect: rect, box: [left, top, limit, bot].map(Math.round) };
  var y = r.top + r.height / 2, inset = Math.min(4, r.width / 3);
  var pts = [r.left + r.width / 2, r.left + inset, r.right - inset];
  // ★ 꺼진 단추(.btn:disabled)는 pointer-events:none 이라 elementFromPoint 가 단추를 지나쳐 부모(단추 줄)를
  //   돌려준다. 유튜브 「다운로드」 는 분석 전엔 늘 꺼져 있어 '가려짐' 으로 잘못 적혔다. 재는 동안만 받게 한다.
  var pe = btn.style.pointerEvents, lift = cs.pointerEvents === 'none';
  if (lift) btn.style.pointerEvents = 'auto';
  try {
    for (var n = 0; n < pts.length; n++) {
      var h = document.elementFromPoint(pts[n], y);
      if (!h) return { why: '그 자리에 아무것도 없음', el: nm, where: where, rect: rect };
      if (h !== btn && !btn.contains(h)) return { why: '가려짐', by: name(h), el: nm, where: where, rect: rect };
    }
  } finally {
    if (lift) btn.style.pointerEvents = pe;
  }
  return { why: 'ok', el: nm, where: where, rect: rect, off: btn.disabled || undefined };
}
if (fx) {
  var btn = T.querySelector(':scope > .fx-run > .btn-row .btn-primary')
         || T.querySelector(':scope > .fx-bar .fx-bar-btns > .btn-row .btn-primary');
  if (!btn) { var sb = document.getElementById(A.tab + '-start-btn'); if (sb && T.contains(sb)) btn = sb; }
  if (!btn) btn = T.querySelector('.btn-row .btn-primary');
  out.start = startInfo(btn);
}

// ── 대상 목록 한 줄 높이 (심었을 때만) ──
if (A.rows) {
  var items = [].filter.call(T.querySelectorAll('.batch-target-item'), shown);
  var hs = items.map(function (x) { return x.getBoundingClientRect().height; }).sort(function (x, y) { return x - y; });
  var lst = items.length ? items[0].parentElement : null;
  out.rows = { n: items.length, min: hs.length ? Math.round(hs[0]) : 0,
               med: hs.length ? Math.round(hs[Math.floor(hs.length / 2)]) : 0,
               max: hs.length ? Math.round(hs[hs.length - 1]) : 0,
               listW: lst ? Math.round(lst.getBoundingClientRect().width) : 0 };
}
return out;
"""

# 긴 대상 목록 심기 — 원래 배열은 화면 안에 쥐고 있는다(★ 왜 대상을 심는 동안 … 참고)
JS_SEED = """
if (typeof platformTargets === 'undefined' || typeof renderPlatformTargetList !== 'function')
  throw new Error('대상 목록(platformTargets·renderPlatformTargetList)이 없습니다');
if (window.__akashiLayout) throw new Error('이미 심어 둔 대상 목록이 있습니다');
var P = A.plat, seeded = [];
for (var i = 1; i <= A.n; i++) {
  var num = String(i);
  while (num.length < A.digits) num = '0' + num;
  seeded.push({ target: 'sample_user_' + num, accountIdx: 'all', enabled: true,
                nickname: i % 3 ? '' : '별명 ' + num, options: {} });
}
var k = { plat: P, had: Object.prototype.hasOwnProperty.call(platformTargets, P), orig: platformTargets[P],
          seeded: seeded, save: window.saveConfig, deferred: 0 };
window.__akashiLayout = k;
if (typeof k.save === 'function') {
  window.saveConfig = function () {
    var s = window.__akashiLayout;
    if (s && platformTargets[s.plat] === s.seeded) { s.deferred++; return; }   // 심은 목록은 저장하지 않는다
    return (s ? s.save : k.save).apply(this, arguments);
  };
}
platformTargets[P] = seeded;
renderPlatformTargetList(P);
return { n: seeded.length, orig: k.orig ? k.orig.length : 0 };
"""

JS_UNSEED = """
var k = window.__akashiLayout;
if (!k) return { none: true };
if (typeof k.save === 'function' && window.saveConfig !== k.save) window.saveConfig = k.save;
var mine = typeof platformTargets !== 'undefined' && platformTargets[k.plat] === k.seeded;
if (mine) { if (k.had) platformTargets[k.plat] = k.orig; else delete platformTargets[k.plat]; }
delete window.__akashiLayout;
if (typeof renderPlatformTargetList === 'function') renderPlatformTargetList(k.plat);
if (k.deferred && typeof saveConfig === 'function') { try { saveConfig(); } catch (e) {} }
return { none: false, mine: mine, deferred: k.deferred,
         n: (typeof platformTargets !== 'undefined' && platformTargets[k.plat]) ? platformTargets[k.plat].length : -1 };
"""

JS_MASK = """
var st = document.getElementById(A.id);
if (A.on && !st) { st = document.createElement('style'); st.id = A.id; st.textContent = A.css; document.head.appendChild(st); }
if (!A.on && st) st.remove();
return !!document.getElementById(A.id);
"""

JS_RESTORE_VIEWS = """
A.fx.forEach(function (o) {
  var t = document.getElementById('tab-' + o.tab);
  if (!t) return;
  if (o.view && t.dataset.fxview !== o.view && typeof window.fxSetView === 'function') fxSetView(t, o.view);
  t.classList.toggle('fx-done', !!o.done);          // [실행] 으로 돌리면 앱이 지우므로 기록대로 다시
});
return 1;
"""

JS_RESTORE_GROUP = """
if (typeof setSettingsGroup === 'function') setSettingsGroup(A.group || 'all');
lsPut(A.k, A.ls);                                    // 원래 글자 그대로(없었으면 지움)
return 1;
"""

JS_RESTORE_TAB = """
if (A.tab && document.getElementById('tab-' + A.tab) && curTab() !== A.tab) switchTab(A.tab);
var app = document.querySelector('.app');
if (app) app.classList.toggle('nav-open-narrow', !!A.navOpen);
return 1;
"""

# 스크롤은 탭을 되돌리고 액자가 제 폭으로 다시 쌓인 뒤에 — 먼저 두면 짧은 판에 걸려 잘린다
JS_RESTORE_SCROLL = """
S.scrollTop = A.scroll;
return Math.round(S.scrollTop);
"""

JS_VERIFY = """
var bad = [];
if (A.tab && curTab() !== A.tab) bad.push('탭 ' + curTab() + '(원래 ' + A.tab + ')');
A.fx.forEach(function (o) {
  var t = document.getElementById('tab-' + o.tab);
  if (!t) return;
  if (o.view && t.dataset.fxview !== o.view) bad.push(o.tab + ' 보기 ' + (t.dataset.fxview || '없음') + '(원래 ' + o.view + ')');
  if (t.classList.contains('fx-done') !== !!o.done) bad.push(o.tab + ' 완료 표시가 원래와 다름');
});
if (A.group !== undefined) {
  var chip = document.querySelector('#tab-settings .sf-chip.active');
  var g = chip ? chip.getAttribute('data-group') : null;
  if (A.group && g !== A.group) bad.push('설정 묶음 ' + g + '(원래 ' + A.group + ')');
  if (lsGet(A.k) !== A.ls) bad.push(A.k + ' 저장 값이 원래와 다름');
}
if (window.__akashiLayout) bad.push('심은 대상 목록이 남음');
if (String(window.saveConfig).indexOf('__akashiLayout') >= 0) bad.push('설정 저장 가로채기가 남음');
if (A.targets >= 0 && typeof platformTargets !== 'undefined') {
  var n = (platformTargets[A.plat] || []).length;
  if (n !== A.targets) bad.push(A.plat + ' 대상 ' + n + '개(원래 ' + A.targets + '개)');
}
if (document.getElementById(A.maskId)) bad.push('입력 칸 가림이 남음');
var want = Math.min(A.scroll, Math.max(0, S.scrollHeight - S.clientHeight));
if (Math.abs(S.scrollTop - want) > 2) bad.push('스크롤 ' + Math.round(S.scrollTop) + '(원래 ' + Math.round(want) + ')');
return bad;
"""


# ── 비밀 가리기 — 화면 오류 글에 토큰이 섞여 나와도 찍지 않는다(check_adapt.py 와 같은 규칙) ──
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


# ── 표 — 한글은 칸 두 개를 먹는다 ──────────────────────────────────────────────
def dwidth(s: str) -> int:
    n = 0
    for ch in s:
        if unicodedata.combining(ch):
            continue
        n += 2 if unicodedata.east_asian_width(ch) in ("W", "F") else 1
    return n


def pad(s: str, n: int) -> str:
    return s + " " * max(0, n - dwidth(s))


COLS = (("탭", 11), ("모드", 5), ("열", 6), ("시작 단추", 11), ("옆넘침", 8), ("삐져나옴", 9), ("액자", 9), ("판정", 0))


def table_line(cells) -> str:
    return "   " + "".join(pad(c, w) if w else c for c, (_, w) in zip(cells, COLS)).rstrip()


def expect_mode(w: float) -> str:
    return "wide" if w >= WIDE_MIN else "mid" if w >= NARROW_MIN else "narrow"


# ── 판정 — 잰 값 하나를 (갈래, 글) 문제 목록으로 ─────────────────────────────────
def judge(m: dict, seed_n: int = 0) -> list:
    probs = []
    if m.get("fx"):
        mode, W = m.get("mode") or "", float(m.get("W") or 0)
        exp = expect_mode(W)
        if not mode:
            probs.append(("모드", "모드 없음 — fxAdapt 가 이 탭에 data-fxmode 를 붙이지 않음"))
        elif mode != exp:
            probs.append(("모드", "모드가 %s 인데 내용 폭 %.1fpx 은 %s(≥%d 넓음 · ≥%d 보통)"
                          % (MODE_KO.get(mode, mode), W, MODE_KO[exp], WIDE_MIN, NARROW_MIN)))
        g, c = m.get("grid"), m.get("cols") or ""
        if g is not None and c and str(g) != c:
            probs.append(("열", "열: data-cols=%s 인데 실제로 그려진 격자는 %d열" % (c, g)))
        s = m.get("start") or {}
        why = s.get("why")
        if why != "ok":
            el = s.get("el") or "단추 줄의 .btn-primary"
            if why == "가려짐":
                d = "가려짐 — 그 자리 맨 위는 %s" % s.get("by")
            elif why == "첫 화면 밖":
                d = "첫 화면 밖 — 단추 %s / 보이는 곳 %s" % (s.get("rect"), s.get("box"))
            elif why == "없음":
                d = "없음 — 탭 안에 .btn-row .btn-primary 가 없음"
            else:
                d = str(why)
            probs.append(("시작 단추", "시작 단추 %s: %s" % (el, d)))
    if (m.get("saOver") or 0) > TOL:
        probs.append(("옆넘침", "옆넘침: 스크롤 영역 안 내용이 %dpx 더 넓음 — 오른쪽이 잘려 안 보임" % m["saOver"]))
    if m.get("offN"):
        shown = ", ".join("%s(끝 %dpx)" % (o["el"], o["right"]) for o in m.get("offenders", [])[:4])
        more = " 외 %d곳" % (m["offN"] - 4) if m["offN"] > 4 else ""
        probs.append(("삐져나옴", "삐져나옴 %d곳: %s%s — 화면 끝 %dpx" % (m["offN"], shown, more, m.get("limit", 0))))
    for x in m.get("overlaps", []):
        probs.append(("겹침", "액자 겹침: " + x))
    for x in m.get("outside", []):
        probs.append(("판 밖", "판 밖으로 나간 액자: " + x))
    for c in m.get("cut", []):
        probs.append(("잘림", "액자가 내용을 자름: %s 안 %d곳, 최대 %dpx (%s)" % (c["frame"], c["n"], c["px"], ", ".join(c["els"]))))
    for x in m.get("inner", []):
        probs.append(("안 스크롤", "액자 안 스크롤: %s 넘침 — 액자는 제 키대로 늘어나야 함" % x))
    rows = m.get("rows")
    if rows is not None and seed_n:
        if rows["n"] != seed_n:
            probs.append(("대상 줄", "대상 줄이 %d개 보임 — 심은 %d개와 다름" % (rows["n"], seed_n)))
        elif rows["max"] > ROW_MAX:
            probs.append(("대상 줄", "대상 줄이 두 줄로 접힘 — 최대 %dpx (보통 %dpx · 기준 ≤%dpx · 목록 폭 %dpx)"
                          % (rows["max"], rows["med"], ROW_MAX, rows["listW"])))
    return [(k, scrub(t)) for k, t in probs]


def cells_for(tab: str, m: dict, probs: list) -> list:
    if m is None:
        return [tab, "?", "?", "?", "?", "?", "?", "문제 %d" % max(1, len(probs))]
    if m.get("fx"):
        mode = MODE_KO.get(m.get("mode") or "", m.get("mode") or "?")
        g, c = m.get("grid"), m.get("cols") or "?"
        cols = "%s≠%d" % (c, g) if (g is not None and c != "?" and str(g) != c) else c
        s = m.get("start") or {}
        if s.get("why") == "ok":
            start = "보임·" + WHERE_KO.get(s.get("where"), "?")
        else:
            start = {"첫 화면 밖": "화면 밖", "그 자리에 아무것도 없음": "빈 자리"}.get(s.get("why"), s.get("why") or "?")
    else:
        mode, start = "—", "—"
        cols = str(m.get("grid")) if m.get("grid") is not None else "—"
    over = str(m.get("saOver")) if (m.get("saOver") or 0) > TOL else "0"
    nfp = sum(1 for k, _ in probs if k in ("겹침", "판 밖", "잘림", "안 스크롤"))
    frames = "%d" % m.get("frames", 0) + ("·문제%d" % nfp if nfp else "")
    verdict = "통과" if not probs else "문제 %d" % len(probs)
    return [tab, mode, cols, start, over, str(m.get("offN", 0)), frames, verdict]


class Abort(Exception):
    """앱과 연결이 끊겼다 — 더 할 수 없다."""


# ── 격리 사본인지(check_adapt.py 와 같은 규칙) ─────────────────────────────────
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
class LayoutCheck:
    def __init__(self, p: Page, a, st: dict):
        self.p, self.a, self.st = p, a, st
        self.orig = None
        self.tabs: list = []
        self.results: list = []          # 폭·화면마다 {width, tab, m, problems, …}
        self.width_notes: list = []      # 폭마다 한 번 보는 것(창 전체 넘침)
        self.notes: list = []            # 시작 전 정리·알림
        self.restore_notes: list = []
        self.seed = None                 # 심은 결과
        self.seed_tried = False
        self.unseed = None               # 되돌린 결과
        self.group_touched = False

    # ── 말하기 ───────────────────────────────────────────────────────────
    def say(self, line: str = "") -> None:
        if not self.a.json:
            print(line, flush=True)

    # ── 화면 다루기 ──────────────────────────────────────────────────────
    def js(self, body: str, **kw):
        expr = "(function(A){%s\n%s\n})(%s)" % (JS_HEAD, body, json.dumps(kw, ensure_ascii=False))
        try:
            return self.p.eval(expr)
        except (WsError, OSError) as e:
            raise Abort("앱과 연결이 끊겼습니다 (%s)" % e)

    def cdp(self, fn, *args):
        try:
            return fn(*args)
        except (WsError, OSError) as e:
            raise Abort("앱과 연결이 끊겼습니다 (%s)" % e)

    def settle(self, tab: str) -> bool:
        """배치가 세 번 연달아 같을 때까지. 끝내 멎지 않으면 False."""
        last, same = None, 0
        end = time.time() + self.a.settle_timeout
        while time.time() < end:
            v = self.js(JS_SIG, tab=tab)
            if v == last:
                same += 1
                if same >= 2:
                    return True
            else:
                last, same = v, 0
            time.sleep(0.1)
        return False

    def mask(self, on: bool) -> None:
        self.js(JS_MASK, on=on, id=MASK_ID, css=MASK_CSS)

    # ── 준비 ─────────────────────────────────────────────────────────────
    def prepare(self):
        """붙은 뒤 첫 확인. 돌릴 수 없으면 까닭 글, 되면 None."""
        end = time.time() + 20
        ready = False
        while time.time() < end:
            if self.js(JS_READY):
                ready = True
                break
            time.sleep(0.4)
        if not ready:
            return "화면에 자동 배치(fxAdapt·fxSetView·layoutFrames)가 없습니다 — 20dd34e 이전 빌드이거나 화면이 아직 준비 중입니다."
        o = self.js(JS_ORIG, plat=SEED_PLAT, kGroup=K_GROUP, maskId=MASK_ID) or {}
        if not o.get("fx"):
            return "기능 화면(.tab-content.fx)이 하나도 없습니다."
        if o.get("staleMask"):
            self.mask(False)
            self.notes.append("지난 점검이 남긴 입력 칸 가림을 먼저 걷었습니다")
        if o.get("stale"):
            r = self.js(JS_UNSEED) or {}
            self.notes.append("지난 점검이 남긴 대상 목록을 먼저 되돌렸습니다" +
                              (" (미뤄 둔 저장 %d번을 지금 한 번에)" % r.get("deferred") if r.get("deferred") else ""))
            o = self.js(JS_ORIG, plat=SEED_PLAT, kGroup=K_GROUP, maskId=MASK_ID) or {}

        fx_names = [x["tab"] for x in o.get("fx", [])]
        default = fx_names + (["settings"] if o.get("settings") else [])
        if self.a.tabs:
            have = set(o.get("tabs") or []) | set(default)
            miss = [t for t in self.a.tabs if t not in have]
            if miss:
                return "그런 탭이 없습니다: %s (있는 것: %s)" % (", ".join(miss), " ".join(default))
            tabs = list(self.a.tabs)
        else:
            tabs = default
        if self.a.seed_targets:
            if not o.get("canSeed"):
                return "대상 목록(platformTargets.%s·renderPlatformTargetList)이 없어 --seed-targets 를 할 수 없습니다." % SEED_PLAT
            if SEED_PLAT not in tabs:
                tabs.append(SEED_PLAT)
                self.notes.append("--seed-targets 라 %s 화면도 잽니다" % SEED_PLAT)
        if "settings" in tabs and not o.get("canGroup"):
            self.notes.append("설정 묶음 단추(setSettingsGroup)가 없어 지금 묶음 그대로 잽니다")
        self.tabs = tabs
        self.orig = o
        return None

    # ── 폭 하나 · 화면 하나 ──────────────────────────────────────────────
    def measure(self, width: int, tab: str) -> dict:
        res = {"width": width, "height": self.a.height, "tab": tab, "m": None, "problems": [],
               "unsettled": False, "shot": None}
        try:
            r = self.js(JS_PREP, tab=tab, group=SETTINGS_GROUP if self.orig.get("canGroup") else "") or {}
            if (r.get("changed") or {}).get("group"):
                self.group_touched = True
            if not r.get("active"):
                res["problems"].append(("측정", "탭을 열지 못함 — switchTab('%s') 뒤에도 보이지 않음" % tab))
                return res
            res["unsettled"] = not self.settle(tab)
            m = self.js(JS_MEASURE, tab=tab, tol=TOL, rows=bool(self.a.seed_targets and tab == SEED_PLAT and self.seed))
            if not isinstance(m, dict):
                res["problems"].append(("측정", "측정 값이 비어 있음 — 화면이 답을 돌려주지 않음"))
                return res
            res["m"] = m
            res["problems"] = judge(m, self.seed["n"] if (self.seed and tab == SEED_PLAT) else 0)
        except JsError as e:
            res["problems"].append(("측정", "측정 못 함 — 화면 JS 오류: " + scrub(e)))
        except CdpError as e:
            res["problems"].append(("측정", "측정 못 함 — " + scrub(e)))
        if self.a.shots:
            path = Path(self.a.shots) / ("%04d_%s.png" % (width, tab))
            try:
                self.mask(True)
                try:
                    r = self.cdp(self.p.cmd, "Page.captureScreenshot", {"format": "png"}, 30)
                finally:
                    self.mask(False)
                # 파일 쓰기는 CDP 밖에서 — 디스크 오류를 '연결 끊김' 으로 잘못 알리지 않게
                with open(path, "wb") as f:
                    f.write(base64.b64decode(r["data"]))
                res["shot"] = str(path)
            except (JsError, CdpError) as e:
                res["problems"].append(("그림", "그림 못 찍음 — " + scrub(e)))
            except (OSError, KeyError, ValueError) as e:
                res["problems"].append(("그림", "그림을 쓰지 못함 — %s (%s)" % (paths.short(path), scrub(e))))
        return res

    def show(self, res: dict) -> None:
        m, probs = res["m"], res["problems"]
        self.say(table_line(cells_for(res["tab"], m, probs)))
        lines = []
        if m and self.a.verbose:
            extra = []
            if m.get("fx"):
                extra.append("보기 %s" % (m.get("view") or "없음"))
            else:
                extra.append("격자 %s열 · 쓰는 열 %s" % (m.get("grid"), m.get("used")))
            s = m.get("start") or {}
            if s.get("rect"):
                extra.append("시작 단추 %s %s" % (s.get("el"), s.get("rect")))
            lines.append("내용 폭 %.1fpx · 화면 끝 %dpx · 액자 %d개 · %s" % (
                float(m.get("W") or 0), m.get("limit", 0), m.get("frames", 0), " · ".join(extra)))
        for _, t in probs:
            lines.append(t)
        if m and m.get("rows") is not None and not any(k == "대상 줄" for k, _ in probs):
            r = m["rows"]
            lines.append("대상 줄 %d개: 보통 %dpx · 가장 낮은 %dpx · 가장 높은 %dpx (목록 폭 %dpx · 기대 ≈%dpx · 기준 ≤%dpx)"
                         % (r["n"], r["med"], r["min"], r["max"], r["listW"], ROW_EXPECT, ROW_MAX))
        if res["unsettled"]:
            lines.append("참고: 배치가 %.0f초 안에 멎지 않았습니다(살아 있는 갱신 때문일 수 있음 — 잰 값은 마지막 것)"
                         % self.a.settle_timeout)
        if res["shot"] and self.a.verbose:
            lines.append("그림 " + paths.short(res["shot"]))
        for ln in lines:
            self.say("      ↳ " + ln)

    def do_width(self, width: int) -> None:
        self.say("")
        self.say("── %d × %dpx %s" % (width, self.a.height, "─" * 40))
        self.say(table_line([c for c, _ in COLS]))
        self.cdp(self.p.size, width, self.a.height, self.a.settle)
        self.settle("")                     # 지금 탭에서 fxAdapt 가 새 폭을 먹을 때까지
        doc = []
        for tab in self.tabs:
            res = self.measure(width, tab)
            self.results.append(res)
            self.show(res)
            m = res["m"] or {}
            if (m.get("docOver") or 0) > TOL:
                doc.append((tab, m["docOver"]))
        if doc:
            px = max(d for _, d in doc)
            t = scrub("창 전체가 옆으로 %dpx 넘침 — 스크롤 영역 밖(위 막대·옆 막대 등)이 창보다 넓음 (탭 %s)"
                      % (px, " ".join(x for x, _ in doc[:6]) + (" 외" if len(doc) > 6 else "")))
            self.width_notes.append({"width": width, "docOver": px, "tabs": [x for x, _ in doc], "problem": t})
            self.say("   창 전체  문제 1")
            self.say("      ↳ " + t)

    # ── 되돌리기 ─────────────────────────────────────────────────────────
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

        def unseed():
            r = self.js(JS_UNSEED) or {}
            self.unseed = r
            if r.get("none"):
                self.restore_notes.append("되돌릴 대상 목록이 없었습니다(도중에 화면이 새로 읽혔을 수 있음)")
            elif not r.get("mine"):
                self.restore_notes.append("앱이 도중에 대상 목록을 새로 읽어 그 목록을 그대로 둠")
            if r.get("deferred"):
                self.restore_notes.append("심어 둔 동안 미룬 설정 저장 %d번 → 원래 목록으로 한 번 저장" % r["deferred"])

        if self.seed_tried:
            step("대상 목록", unseed)
        step("입력 칸 가림", lambda: self.mask(False))
        step("보기·완료 표시", lambda: self.js(JS_RESTORE_VIEWS, fx=o.get("fx") or []))
        if self.group_touched:
            step("설정 묶음", lambda: self.js(JS_RESTORE_GROUP, group=o.get("group"), ls=o.get("lsGroup"), k=K_GROUP))
        step("창 크기", lambda: self.cdp(self.p.clear_size))
        time.sleep(max(0.4, self.a.settle))

        def tab_and_scroll():
            tab = o.get("tab") or ""
            self.settle(tab)                 # 진짜 창 폭으로 fxAdapt 가 다시 돈 뒤
            self.js(JS_RESTORE_TAB, tab=tab, navOpen=bool(o.get("navOpen")))
            self.settle(tab)                 # 그 탭의 액자가 제 폭으로 다시 쌓인 뒤
            self.js(JS_RESTORE_SCROLL, scroll=o.get("scroll") or 0)

        step("탭·스크롤", tab_and_scroll)
        try:
            kw = {"tab": o.get("tab") or "", "fx": o.get("fx") or [], "scroll": o.get("scroll") or 0,
                  "plat": SEED_PLAT, "targets": o.get("targets", -1) if self.seed_tried else -1, "maskId": MASK_ID}
            if self.group_touched:
                kw.update({"group": o.get("group"), "ls": o.get("lsGroup"), "k": K_GROUP})
            miss = self.js(JS_VERIFY, **kw) or []
            if miss:
                ok = False
                self.restore_notes.append("확인: " + " · ".join(scrub(x) for x in miss))
        except (JsError, CdpError) as e:
            ok = False
            self.restore_notes.append("되돌린 뒤 확인 못 함 — " + scrub(e)[:200])
        return ok

    # ── 본 흐름 ──────────────────────────────────────────────────────────
    def main(self) -> int:
        try:
            why = self.prepare()
        except Abort as e:
            return self.fatal(str(e))
        except CdpError as e:
            return self.fatal("화면을 읽지 못했습니다 — " + scrub(e))
        if why:
            return self.fatal(why)

        # 이 뒤로 난 화면 JS 예외만 센다 — enable 하면 지난 콘솔 글이 한꺼번에 다시 오므로 비운다
        try:
            self.cdp(self.p.cmd, "Runtime.enable")
            self.cdp(self.p.cmd, "Log.enable")
            self.p.pump(0.4)
        except Abort as e:
            return self.fatal(str(e))
        except CdpError as e:
            return self.fatal("화면 기록을 켜지 못했습니다 — " + scrub(e))
        self.p.events.clear()

        self.say("배치 점검 — 포트 %d · 앱 %s (격리 사본 PID %s)" % (self.a.port, self.st.get("version") or "?", self.st.get("pid")))
        self.say("   폭 %s × 높이 %d · 화면 %d개 (%s)" % (" ".join(str(w) for w in self.a.widths), self.a.height,
                                                    len(self.tabs), " ".join(self.tabs)))
        if self.a.shots:
            self.say("   그림 → %s (찍는 동안만 입력 칸 값을 점으로 가림)" % paths.short(self.a.shots))
            if self.st.get("seeded"):
                self.say("   주의: 설정을 복사한 사본(--seed-config)입니다 — 그림에 계정 이름 등이 보일 수 있습니다")
        for n in self.notes:
            self.say("   " + n)

        aborted, lost = "", False
        try:
            if self.a.seed_targets:
                self.seed_tried = True
                n = self.a.seed_targets
                digits = max(2, len(str(n)))
                self.seed = self.js(JS_SEED, plat=SEED_PLAT, n=n, digits=digits) or {"n": 0, "orig": 0}
                self.say("   %s 대상 목록을 잠시 %d명(sample_user_%s~%s)으로 바꿈 — 원래 %d명은 끝나면 되돌림"
                         % (SEED_PLAT, self.seed.get("n", 0), "1".zfill(digits), str(n).zfill(digits), self.seed.get("orig", 0)))
            for w in self.a.widths:
                self.do_width(w)
        except Abort as e:
            aborted, lost = str(e), True
        except KeyboardInterrupt:
            aborted = "사용자가 멈춤(Ctrl-C)"
        except (JsError, CdpError) as e:
            aborted = "화면을 다루지 못했습니다 — " + scrub(e)

        restored = False
        if not lost:
            try:
                restored = self.restore()
            except Abort as e:
                aborted, lost = aborted or str(e), True
            except KeyboardInterrupt:
                self.restore_notes.append("되돌리는 중에 멈춤(Ctrl-C)")

        exc, other = [], []
        if not lost:
            try:
                self.p.pump(0.3)
            except (WsError, OSError):
                pass
            for e in self.p.collect_errors():
                (exc if e.startswith("예외") else other).append(scrub(e))
        return self.report(aborted, lost, restored, exc, other)

    def fatal(self, msg: str) -> int:
        if self.a.json:
            print(json.dumps({"tool": "check_layout", "port": self.a.port, "error": msg, "exit": 2},
                             ensure_ascii=False, indent=1))
        else:
            print("돌릴 수 없음 — " + msg)
        return 2

    def report(self, aborted: str, lost: bool, restored: bool, exc: list, other: list) -> int:
        items = []                       # 판정에 쓰는 짧은 이름표
        for r in self.results:
            for k, _ in r["problems"]:
                items.append("%dpx %s %s" % (r["width"], r["tab"], k))
        for wn in self.width_notes:
            items.append("%dpx 창 전체 넘침" % wn["width"])
        problems = len(items)
        self.say("")
        if not lost:
            self.say("   화면 JS 예외 %d건%s" % (len(exc), "" if not other else " (참고: console.error·기록 오류 %d건)" % len(other)))
            for e in exc[:5]:
                self.say("       " + e[:240])
            if self.a.verbose:
                for e in other[:5]:
                    self.say("       참고 " + e[:240])
            if self.orig is not None:
                self.say("   정리 — %s" % ("탭·스크롤·보기·완료 표시·설정 묶음·창 크기%s 되돌림"
                                        % ("·대상 목록" if self.seed_tried else "") if restored else "다 되돌리지 못함"))
                for n in self.restore_notes:
                    self.say("       " + n)
        if aborted:
            self.say("   도중에 멈춤 — " + aborted)

        if exc:
            problems += 1
            items.append("JS 예외 %d건" % len(exc))
        if not lost and self.orig is not None and not restored:
            problems += 1
            items.append("되돌리기 못 함")
        if aborted:
            problems += 1
            items.append("끝까지 못 돎")
        want = len(self.a.widths) * len(self.tabs)
        done = len(self.results)
        if problems == 0 and done == want:
            line = "통과 — 폭 %d가지 × 화면 %d개(%d곳) 배치 문제 없음 · 원래대로 되돌림" % (len(self.a.widths), len(self.tabs), done)
        else:
            problems = max(problems, 1)
            shown = " · ".join(items[:6]) + (" 외 %d건" % (len(items) - 6) if len(items) > 6 else "")
            line = "문제 %d건 — %s" % (problems, shown or "결과가 모자람(%d/%d곳)" % (done, want))
        if aborted and not lost and aborted.startswith("사용자가 멈춤"):
            code = 2                     # Ctrl-C — 판정이 아니다
        else:
            code = 0 if (problems == 0 and done == want) else 1

        if self.a.json:
            print(json.dumps({
                "tool": "check_layout", "port": self.a.port, "app": self.st.get("version"),
                "widths": self.a.widths, "height": self.a.height, "tabs": self.tabs,
                "results": [{"width": r["width"], "height": r["height"], "tab": r["tab"],
                             "ok": not r["problems"], "problems": [t for _, t in r["problems"]],
                             "kinds": [k for k, _ in r["problems"]], "unsettled": r["unsettled"],
                             "shot": paths.short(r["shot"]) if r["shot"] else None, "measure": r["m"]}
                            for r in self.results],
                "window": self.width_notes,
                "seed": None if not self.seed_tried else {"platform": SEED_PLAT, "n": (self.seed or {}).get("n", 0),
                                                          "original": (self.seed or {}).get("orig", 0),
                                                          "unseed": self.unseed},
                "notes": self.notes, "js_exceptions": exc, "console_errors": len(other),
                "restored": restored, "restore_notes": self.restore_notes, "aborted": aborted,
                "problems": problems, "verdict": line, "exit": code,
            }, ensure_ascii=False, indent=1))
        else:
            self.say(line)
        return code


# ── 인자 ────────────────────────────────────────────────────────────────────
def parse_tabs(values) -> list:
    out = []
    for v in values or []:
        for t in re.split(r"[,\s]+", v.strip()):
            if t and t not in out:
                out.append(t)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(
        description="배치 점검 — 창 폭을 바꿔 가며 기능 화면 열넷과 설정 화면의 모드·열·시작 단추·옆넘침·"
                    "삐져나온 요소·액자 겹침/잘림을 격리 사본에서 잰다.",
        epilog="먼저 python3 akashi/iso.py start 로 격리 사본을 띄우십시오. 사용자의 앱에는 붙지 않습니다.\n"
               "종료 코드: 0 모두 통과 · 1 문제 있음 · 2 돌릴 수 없음",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=os.environ.get("AKASHI_PORT") or 9334,   # ★ 글이면 argparse 가 int 로 — 빈 값·숫자 아님도 넘어지지 않고 종료 2
                    help="격리 사본의 CDP 포트 (기본 9334 · 환경 변수 AKASHI_PORT)")
    ap.add_argument("--widths", type=int, nargs="+", default=list(DEFAULT_WIDTHS), metavar="PX",
                    help="잴 창 폭들, CSS px (기본 %s — 앱 배율을 거친 '화면 안쪽' 폭)" % " ".join(str(w) for w in DEFAULT_WIDTHS))
    ap.add_argument("--height", type=int, default=DEFAULT_HEIGHT, metavar="PX",
                    help="창 높이 = '첫 화면' 의 높이 (기본 %d)" % DEFAULT_HEIGHT)
    ap.add_argument("--tabs", nargs="+", metavar="TAB",
                    help="잴 화면만 (쉼표·빈칸으로 여럿, 예: twitter,pixiv settings · 기본: 기능 화면 전부 + settings)")
    ap.add_argument("--seed-targets", type=int, default=0, metavar="N",
                    help="twitter 대상 목록을 잠시 N명(sample_user_01…)으로 바꿔 한 줄 높이를 잰다 (끝나면 원래 목록으로)")
    ap.add_argument("--shots", metavar="DIR",
                    help="폭·화면마다 그림 한 장(DIR/0420_twitter.png …) — 찍는 동안만 입력 칸 값을 가린다")
    ap.add_argument("--settle", type=float, default=0.4, metavar="SEC",
                    help="창 크기를 바꾼 뒤 먼저 쉴 초 (기본 0.4 · 그 뒤엔 배치가 멎을 때까지 스스로 기다림)")
    ap.add_argument("--settle-timeout", type=float, default=4.0, metavar="SEC",
                    help="배치가 멎기를 기다리는 가장 긴 초 (기본 4)")
    ap.add_argument("-v", "--verbose", action="store_true", help="통과한 줄도 잰 값(내용 폭·화면 끝·단추 자리·그림)을 찍는다")
    ap.add_argument("--json", action="store_true", help="사람용 글 대신 JSON 한 덩어리로")
    a = ap.parse_args()

    def fatal(msg: str) -> int:
        if a.json:
            print(json.dumps({"tool": "check_layout", "port": a.port, "error": msg, "exit": 2}, ensure_ascii=False, indent=1))
        else:
            print("돌릴 수 없음 — " + msg)
        return 2

    if not (0 < a.port < 65536):
        return fatal("--port 가 올바르지 않습니다: %d" % a.port)
    widths = []
    for w in a.widths:
        if not (240 <= w <= 3840):
            return fatal("--widths 는 240~3840 사이여야 합니다: %d" % w)
        if w not in widths:
            widths.append(w)
    a.widths = widths
    if not (300 <= a.height <= 2400):
        return fatal("--height 는 300~2400 사이여야 합니다: %d" % a.height)
    if not (0 <= a.seed_targets <= 500):
        return fatal("--seed-targets 는 0~500 이어야 합니다: %d" % a.seed_targets)
    if not (0 <= a.settle <= 10):
        return fatal("--settle 은 0~10 초여야 합니다.")
    if not (0.5 <= a.settle_timeout <= 30):
        return fatal("--settle-timeout 은 0.5~30 초여야 합니다.")
    a.tabs = parse_tabs(a.tabs)
    for t in a.tabs:
        if not re.fullmatch(r"[a-z]{2,20}", t):
            return fatal("--tabs 는 영어 소문자 탭 이름이어야 합니다(예: twitter,settings): %s" % t)
    if a.shots:
        d = Path(os.path.expanduser(a.shots)).resolve()
        real_data = paths.data_dir().resolve()
        if d == real_data or real_data in d.parents:
            return fatal("--shots 가 사용자의 자료 폴더 안을 가리킵니다 — 다른 곳을 주십시오: " + paths.short(d))
        if any(part.endswith(".app") for part in d.parts):
            return fatal("--shots 가 앱 번들 안을 가리킵니다 — 다른 곳을 주십시오: " + paths.short(d))
        if d.exists() and not d.is_dir():
            return fatal("--shots 가 폴더가 아닙니다: " + paths.short(d))
        # ★ 그림에 계정 이름이 보일 수 있다(--seed-config 사본). 공개 저장소 안의 기존 폴더에 쓰면 커밋될 수 있어
        #   shots.py 처럼 .gitignore(*) 가 있는 폴더만 받는다 — 새로 만드는 폴더에는 아래에서 넣어 준다
        repo = paths.REPO.resolve()
        if d.exists() and (d == repo or repo in d.parents) and not (d / ".gitignore").is_file():
            return fatal("--shots 가 저장소 안의 기존 폴더입니다 — 그림이 커밋될 수 있어 쓰지 않습니다. "
                         "새 폴더 이름이나 $TMPDIR 밑을 주십시오: " + paths.short(d))
        a.shots_new = not d.exists()
        a.shots = str(d)

    err, st = guard(a.port)
    if err:
        return fatal(err)
    if a.shots:                          # 돌릴 수 있을 때만 폴더를 만든다
        try:
            Path(a.shots).mkdir(parents=True, exist_ok=True)
            gi = Path(a.shots) / ".gitignore"
            if a.shots_new and not gi.exists():     # 우리가 만든 폴더만 — 남의 폴더의 git 규칙은 건드리지 않는다
                gi.write_text("# akashi check_layout — 그림에 계정 이름이 보일 수 있어 git 에 올리지 않는다(이 파일까지)\n*\n",
                              "utf-8")
        except OSError as e:
            return fatal("--shots 폴더를 만들지 못했습니다: %s (%s)" % (paths.short(a.shots), e.strerror or e))
    try:
        ts = [t for t in cdplib.page_targets(a.port) if "window=" not in t.get("url", "")]
    except (OSError, ValueError) as e:
        return fatal("포트 %d 에서 화면 목록을 읽지 못했습니다 (%s)" % (a.port, e))
    if not ts:
        return fatal("포트 %d 에 본 창(index.html) 화면이 없습니다 — 화면이 준비될 때까지 기다린 뒤 다시." % a.port)
    ws_url = ts[0].get("webSocketDebuggerUrl") or ""
    # ★ 주소는 앱이 알려 준 것 — check_adapt 처럼 이 기계(127.0.0.1) 밖으로는 나가지 않는다.
    #   다른 디버거가 먼저 붙어 있으면 주소가 빠져 온다 — ["…"] 로 읽으면 KeyError 로 넘어져 종료 1(문제)로 보였다
    if urlparse(ws_url).hostname not in ("127.0.0.1", "localhost", "::1"):
        return fatal("화면 주소가 비었거나 이 기계(127.0.0.1)가 아닙니다 — 붙지 않습니다(다른 디버거가 먼저 붙어 있으면 비어 옵니다).")
    try:
        p = Page(ws_url, timeout=20)
    except (WsError, OSError) as e:
        return fatal("화면에 붙지 못했습니다 (%s)" % e)
    with p:
        try:
            return LayoutCheck(p, a, st).main()
        except KeyboardInterrupt:        # 준비하는 동안(아직 아무것도 바꾸기 전)에 멈춘 경우
            return fatal("사용자가 멈춤(Ctrl-C)")


if __name__ == "__main__":
    sys.exit(main())
