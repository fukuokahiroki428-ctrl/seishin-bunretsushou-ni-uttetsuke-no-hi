#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""폼 저장 점검 — 창마다 바뀐 칸만 보내고, 저장소는 합치고, 모든 창에 알리는지(3512b0a)를 격리 사본에서 대 본다.

    python3 akashi/iso.py start                        # 먼저 격리 사본을 띄운다
    python3 akashi/iso.py start --seed-config          #  (자격 칸 길이까지 대 보려면 — 값은 찍지 않는다)
    python3 akashi/check_forms.py                      # 아홉 가지
    python3 akashi/check_forms.py -v                   # 단계마다 보낸 칸·알림 칸·자격 칸 길이까지
    python3 akashi/check_forms.py --field crawl-delay --field2 crawl-depth
    python3 akashi/check_forms.py --json               # 기계용 결과 한 덩어리
    python3 akashi/iso.py stop

살피는 것 — 3512b0a 「팬박스 계정이 저장되지 않던 것」 이 약속한 움직임:
  1 바뀐 칸만 보낸다        칸 하나를 고치면 backend.saveFormData 에 그 칸 하나만 실린다
  2 저장소와 알림           저장소(설정 파일)에 들어가고, 그 칸의 알림(applyFormPatch)이 창으로 돌아온다
  3 저장소는 합친다         둘째 칸을 저장해도 첫째 칸과 나머지 칸(자격 칸 포함)이 그대로
  4 가만있던 창은 조용       포커스를 잃어도(blur) 바뀐 칸이 없으면 아무것도 보내지 않는다
  5 옛 값을 든 창           새 값을 못 본 창(옛 값·빈 자격 칸)이 포커스를 잃어도 저장소를 덮지 않는다 — 원래 버그
  6 다른 창의 알림          알림은 그 칸만 바꾸고(나머지 칸·자격 칸 길이 그대로) 되받아 보내지 않는다.
                            깨진 알림·화면에 없는 칸의 알림도 견딘다
  7 치는 중인 칸            포커스가 있는 칸은 알림이 덮지 않고, 이어 친 값이 저장된다
  8 복원 전 가드            복원을 못 받은 창(8초 폴백)은 사람이 손댄 칸만 보낸다
  9 진짜 두 창              기능 창이 열려 있으면 — 거기서 저장한 칸이 저장소·본 창에 오고, 본 창은 덮지 않는다
  끝나면 시험 칸을 원래 값으로 저장하고, 가로챈 함수·기준값·탭·보기·포커스·스크롤을 되돌린 뒤 확인한다.

종료 코드: 0 모두 통과(9번은 기능 창이 없으면 생략해도 통과)
          1 문제 있음(실패·건너뜀·JS 예외·되돌리기 못 함·도중에 앱이 사라짐)
          2 돌릴 수 없음(격리 사본 없음·포트 닫힘·화면 없음·옛 빌드·시험 칸을 쓸 수 없음·잘못된 인자·Ctrl-C)

★ 왜 격리 사본에만 붙나
  이 공구는 폼을 실제로 저장시킨다 — 앱은 그것을 자료 폴더의 설정 파일에 쓴다. 사용자의 앱에 붙으면
  사용자의 설정을 고치는 셈이다. 그래서 check_adapt.py 와 같은 문지기(guard)를 지난다: iso.py 의 기록
  (state.json)에 있는 PID 가 살아 있고(실행 파일·뜬 시각까지 대조) 그 PID(또는 자식)가 이 포트를 듣고
  있을 때만 붙는다. 저장소를 읽는 자리도 그 기록의 격리 홈에서 셈하고, 그것이 진짜 홈이나 진짜 자료
  폴더를 가리키면 멈춘다. 창이 여럿이면 본 창(index.html, 'window=' 없는 것)이 시험대이고, 기능 창
  ('window=<탭>')은 9번에만 쓴다.

★ 왜 saveFormData 를 가로채나 — 그리고 값은 왜 화면 밖으로 가져오지 않나
  '바뀐 칸만 보낸다' 는 C++ 에 닿기 전에 무엇이 실렸는지를 봐야 알 수 있다. 그래서 backend.saveFormData
  와 applyFormPatch 를 '적고 그대로 넘기는' 함수로 잠시 바꾼다(앱의 움직임은 그대로다). 적은 묶음은
  화면 안(window.__akashiForm)에만 두고, 파이썬으로는 칸 이름·개수·'시험 값과 같은가' 참/거짓만
  가져온다. 자격 칸(이름에 session·cookie·token·auth·pass·key·ct0 … 가 든 칸, 또는 비밀번호 칸)은
  길이만 가져온다. 저장소(설정 파일)를 견줄 때는 칸마다 길이와 SHA-256 을 메모리에서만 견주고,
  지문도 찍지 않는다 — 짧은 값은 지문으로 거꾸로 찾을 수 있다. 화면에 찍는 값은 이 공구가 만든 시험
  값과, 숫자뿐인 원래 값(딜레이 같은 것)뿐이다.

★ 왜 두 번째 창을 '흉내' 내나
  기능 창은 앱 메뉴(네이티브 메뉴)에서만 열린다. 이 맥에선 손쉬운 사용 권한이 없어 메뉴를 누를 수
  없다. 그래서 2~8번은 C++ 이 모든 창에 부르는 바로 그 함수(applyFormPatch)를 같은 모양의 글로 직접
  불러 '다른 창이 저장했다' 를 흉내 내고, '새 값을 못 본 창' 은 이 창의 화면 값과 기준값(_formBase)을
  옛 값으로 되돌려 흉내 낸다(원래 버그 그대로의 모양이다). 사람이 기능 창을 하나 열어 두었으면
  9번에서 진짜 두 창으로 한 번 더 한다. 기능 창을 열고 닫는 일은 하지 않는다.

★ 왜 시험 칸이 딜레이 숫자 칸인가
  기본은 fanbox-delay(원래 버그가 난 팬박스 탭)와 pixiv-delay. 수집을 돌리지 않으면 아무 데도 쓰이지
  않는 숫자이고, 값은 그 칸의 min·max·step 안에서 고른다. 자격 칸·체크 칸·고르기 칸, 복원 때 앱이 따로
  다루는 칸(*-real-capture·twitter-progress)은 시험 칸이 될 수 없다 — 자격 칸의 값은 한 글자도 바꾸지
  않는다(5번의 '옛 값' 흉내는 화면 안에서만 비우고, 저장되지 않았음을 확인한 뒤 곧바로 되돌린다).

★ 왜 시작하기 전에 한 번 저장하나
  앱은 복원(restoreFormData)한 뒤에 스스로 바꾸는 칸이 있다(이어서 수집 twitter-progress 를 끈다 —
  기준값을 잡은 뒤라 '바뀐 칸' 이 된다). 그대로 두면 4번의 '가만있으면 조용' 이 그 칸 때문에 깨진다.
  앱이 다음 포커스 잃음에 어차피 할 저장을 먼저 한 번 해 두고, 그 뒤의 저장소를 기준으로 삼는다.
  무엇을 저장했는지는 칸 이름만 알린다.

★ 되돌리기와 그 한계
  시험 칸은 원래 화면 값으로 다시 저장한다(기준값을 지워 반드시 보낸다). 저장소에는 '칸을 지우는' 길이
  없다(saveFormData 는 합치기만 한다). 그래서 원래 저장소에 없던 시험 칸은 원래 화면 값으로 남는다 —
  앱이 그 칸을 처음 저장할 때 쓸 값과 같다. 이것은 되돌리기 실패가 아니라 알림으로 적는다.
  도중에 끊기면(Ctrl-C 가 아닌 강제 종료 등) 가로챈 함수와 적어 둔 원래 값이 화면에 남는다. 다음 실행이
  그것을 알아보고 먼저 되돌린다(시험 칸은 원래 값으로 다시 저장).
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import sys
import time
from pathlib import Path
from urllib.parse import urlparse

sys.path.insert(0, str(Path(__file__).resolve().parent))
# 격리 사본인지 가리는 문지기·비밀 가리기·판정 모으기는 자동 배치 점검과 같은 규칙을 쓴다
from check_adapt import guard, scrub, verdict  # noqa: E402
from lib import cdp as cdplib  # noqa: E402
from lib import paths  # noqa: E402
from lib.cdp import CdpError, JsError, Page  # noqa: E402
from lib.ws import WsError  # noqa: E402

DEFAULT_FIELD = "fanbox-delay"
DEFAULT_FIELD2 = "pixiv-delay"
# 자격 칸 — 이름에 이것이 들면 값을 찍지도, 시험 칸으로 쓰지도 않는다(화면 JS 도 같은 글을 받아 쓴다)
SECRET_PAT = "session|cookie|token|auth|pass|key|ct0|secret|sid|csrf|dtsg"
SECRET_RE = re.compile(SECRET_PAT, re.I)
FIELD_RE = re.compile(r"[a-z0-9][a-z0-9-]{1,48}")
NUMERIC_RE = re.compile(r"-?\d{1,6}(\.\d{1,6})?")
GHOST = "akashi-no-such-field"      # 6번 — 화면에 없는 칸의 알림
QUIET = 0.4                          # 저장이 온 뒤 '덤으로 더 보내는지' 지켜볼 초(앱 디바운스 100ms + 채널)
READY_WAIT = 30.0
LOOPBACK = ("127.0.0.1", "localhost", "::1")   # 화면 주소가 이 기계일 때만 붙는다(check_adapt 와 같은 규칙)

STATUS_KO = {"pass": "통과", "fail": "실패", "skip": "건너뜀", "na": "생략"}

_MISSING = object()   # 저장소에 그 칸이 없음
_NOFILE = object()    # 저장소 파일을 못 읽음


# ── 화면 쪽 도우미 — 매번 함수 안에 넣어 돌린다(시험 상태는 window.__akashiForm 하나에만) ──────
JS_HEAD = r"""
var F = window.__akashiForm || null;
var IDS = (typeof _formFieldIds !== 'undefined' && _formFieldIds) ? _formFieldIds : [];
var SECRET = new RegExp(A.secretPat, 'i');
var OWN = Object.prototype.hasOwnProperty;
function el(id) { return document.getElementById(id); }
function rd(e) { return e.type === 'checkbox' ? e.checked : e.value; }
function put(e, v) { if (e.type === 'checkbox') e.checked = !!v; else e.value = (v == null ? '' : v); }
function secret(id) { var e = el(id); return SECRET.test(id) || !!(e && e.type === 'password'); }
function len(v) { return v == null ? 0 : (typeof v === 'boolean' ? (v ? 1 : 0) : String(v).length); }
function need() { if (!F) throw new Error('시험 준비(window.__akashiForm)가 없습니다 — 화면이 새로 고쳐졌을 수 있습니다'); }
// 자격 칸은 길이만
function secretLens() {
  var o = {};
  IDS.forEach(function (id) { var e = el(id); if (e && secret(id)) o[id] = len(rd(e)); });
  return o;
}
// 적은 묶음을 파이썬에 줄 모양으로 — 칸 이름·개수·'이 칸이 이 값인가' 만(값은 화면 밖으로 내지 않는다)
function view(list, n, id, v) {
  return list.slice(n).map(function (c) {
    return { keys: c.keys.slice(0, 40), n: c.keys.length, bad: !!c.bad,
             hit: (id && c.data && OWN.call(c.data, id)) ? c.data[id] === v : null };
  });
}
function blurWin() { window.dispatchEvent(new Event('blur')); }
function scroller() { return document.querySelector('.scroll-area'); }
function tabNow() { return (typeof currentTab !== 'undefined' && currentTab) ? String(currentTab) : ''; }
// 옛 값 흉내(5번)가 남긴 것을 되돌린다 — 되돌리기·흔적 치우기가 같이 쓴다
function unsnap() {
  var n = 0, bad = [], snap = (F && F.snap) || {};
  Object.keys(snap).forEach(function (id) {
    var e = el(id), s = snap[id];
    if (!e) return;
    put(e, s.v);
    if (s.hasB) _formBase[id] = s.b; else delete _formBase[id];
    if (rd(e) !== s.v) bad.push(id);
    n++;
  });
  if (F) delete F.snap;
  return { n: n, bad: bad };
}
// 7번에서 바꾼 보기(좁은 창의 설정/실행)와 탭을 되돌린다
function viewTabBack() {
  var out = { tab: true, view: true };
  if (F.fxTab) {
    var t = document.getElementById('tab-' + F.fxTab);
    if (t && typeof fxSetView === 'function') {
      fxSetView(t, F.fxView || 'set');
      t.classList.toggle('fx-done', !!F.fxDone);
      out.view = (t.dataset.fxview || 'set') === (F.fxView || 'set');
    }
  }
  if (F.tab && tabNow() !== F.tab && typeof switchTab === 'function' && document.getElementById('tab-' + F.tab)) switchTab(F.tab);
  out.tab = !F.tab || tabNow() === F.tab;
  var S = scroller();
  if (S) S.scrollTop = F.scroll || 0;
  return out;
}
"""

JS_READY = """
return {
  page: typeof switchTab === 'function' && document.readyState === 'complete',
  fn: typeof applyFormPatch === 'function' && typeof _doSaveForm === 'function'
      && typeof restoreFormData === 'function' && typeof saveFormDataToBackend === 'function'
      && typeof _formBase === 'object' && IDS.length > 0,
  be: !!(window.backend && typeof backend.saveFormData === 'function'),
  restored: typeof _formRestored !== 'undefined' && _formRestored === true,
  loaded: typeof _formLoaded !== 'undefined' && _formLoaded === true,
  stale: !!F
};
"""

# 지난 실행이 도중에 끊겨 남긴 것 치우기 — 가로챈 함수 · 옛 값 흉내 · 시험 칸(원래 값으로 다시 저장) · 보기·탭
JS_RECOVER = """
if (!F) return null;
var fields = (F.fields || []).slice();
var u = unsnap();
if (F.orig) backend.saveFormData = F.orig;
if (F.patchOrig) window.applyFormPatch = F.patchOrig;
if (typeof F.restored === 'boolean') _formRestored = F.restored;
delete _formBase[A.ghost];
_formDirty = {};
var sent = 0, save = F.save !== false;
fields.forEach(function (id) {
  var e = el(id), s = F.stash && F.stash[id];
  if (!e || !s) return;
  put(e, s.v);
  // 본 창은 원래 값으로 다시 저장(기준값을 지워 반드시 보낸다) · 기능 창은 화면만(저장은 본 창이 한다)
  if (save) { delete _formBase[id]; _formDirty[id] = true; sent++; } else { _formBase[id] = s.v; }
});
if (sent) _doSaveForm();
try { viewTabBack(); } catch (e) {}
delete window.__akashiForm;
return { fields: fields, snap: u.n, saved: sent > 0 };
"""

JS_INSTALL = """
if (F) throw new Error('앞선 시험 흔적(window.__akashiForm)이 남아 있습니다');
var bad = [];
A.fields.forEach(function (id) {
  var e = el(id);
  if (IDS.indexOf(id) < 0) bad.push(id + ' — 저장하는 칸 목록(_formFieldIds)에 없음');
  else if (!e) bad.push(id + ' — 화면에 없음');
  else if (secret(id)) bad.push(id + ' — 자격 칸은 시험 칸으로 쓰지 않음');
  else if (/-real-capture$/.test(id) || id === 'twitter-progress') bad.push(id + ' — 복원 때 앱이 따로 다루는 칸');
  else if (!(e.tagName === 'TEXTAREA' || (e.tagName === 'INPUT' && /^(number|text|search|url)$/.test(e.type))))
    bad.push(id + ' — 숫자·글 칸만 씀(' + e.tagName.toLowerCase() + (e.type ? '/' + e.type : '') + ')');
  else if (e.disabled || e.readOnly) bad.push(id + ' — 잠긴 칸');
  else if (document.activeElement === e) bad.push(id + ' — 지금 누가 치는 중');
});
if (bad.length) return { bad: bad };
// 시험 값 — 숫자 칸은 min·max·step 안에서, 글 칸은 누가 봐도 시험인 글. 원래 값과 겹치지 않게
function vals(e, n) {
  var out = [], cur = String(rd(e));
  if (e.type === 'number') {
    var lo = parseFloat(e.min), hi = parseFloat(e.max), st = parseFloat(e.step);
    if (!(st > 0)) st = 1;
    if (!isFinite(lo)) lo = 0;
    if (!isFinite(hi)) hi = lo + st * 60;
    for (var k = 1; out.length < n && lo + k * st <= hi + 1e-9; k++) {
      var v = String(Number((lo + k * st).toFixed(6)));
      if (Number(v) !== Number(cur)) out.push(v);
    }
  } else {
    for (var j = 1; out.length < n && j < 60; j++) { var w = 'akashi-test-' + j; if (w !== cur) out.push(w); }
  }
  return out.length === n ? out : null;
}
var testVals = {};
for (var i = 0; i < A.fields.length; i++) {
  var fid = A.fields[i], tv = vals(el(fid), A.nvals[i]);
  if (!tv) return { bad: [fid + ' — 시험 값을 ' + A.nvals[i] + '개 고를 수 없음(min·max·step 이 좁음)'] };
  testVals[fid] = tv;
}
// 앱이 다음 포커스 잃음에 어차피 할 저장을 먼저 — 이름만 돌려준다
var pre = [];
IDS.forEach(function (id) {
  var e = el(id);
  if (e && (!OWN.call(_formBase, id) || _formBase[id] !== rd(e))) pre.push(id);
});
if (pre.length) _doSaveForm();
var stash = {};
IDS.forEach(function (id) {
  var e = el(id);
  if (e) stash[id] = { v: rd(e), b: _formBase[id], hasB: OWN.call(_formBase, id) };
});
var S = scroller();
F = { v: 1, fields: A.fields.slice(), save: A.save !== false, stash: stash, calls: [], patches: [],
      orig: backend.saveFormData, patchOrig: window.applyFormPatch, restored: _formRestored,
      tab: tabNow(), scroll: S ? S.scrollTop : 0, fxTab: '', fxView: '', fxDone: false };
var G = F;
function rec(list, s) {
  var c = { t: Date.now(), keys: [], data: null, bad: false };
  try { c.data = JSON.parse(s); c.keys = Object.keys(c.data || {}); } catch (e) { c.bad = true; }
  list.push(c);
}
// 적고 그대로 넘긴다 — 앱의 움직임은 바뀌지 않는다
backend.saveFormData = function (s) { rec(G.calls, s); return G.orig.apply(backend, arguments); };
window.applyFormPatch = function (s) { rec(G.patches, s); return G.patchOrig.apply(window, arguments); };
window.__akashiForm = F;
var orig = {};
A.fields.forEach(function (id) { orig[id] = stash[id].v; });
return { bad: [], vals: testVals, orig: orig, pre: pre, secrets: secretLens(), tab: F.tab, ids: IDS.length };
"""

JS_COUNTS = """
need();
return { c: F.calls.length, p: F.patches.length };
"""

# 사람이 친 것처럼 — 값을 넣고 input 을 쏜다(fire 가 거짓이면 값만)
JS_SET = """
need();
var e = el(A.id), r = { c: F.calls.length, p: F.patches.length };
put(e, A.v);
if (A.fire) e.dispatchEvent(new Event('input', { bubbles: true }));
return r;
"""

JS_SINCE = """
need();
var e = A.id ? el(A.id) : null;
return { calls: view(F.calls, A.c, A.id, A.v), patches: view(F.patches, A.p, A.id, A.v),
         dom: e ? rd(e) === A.v : null, base: A.id ? _formBase[A.id] === A.v : null };
"""

# 창이 포커스를 잃은 것처럼(앱의 blur 듣개가 _doSaveForm 을 부른다) — poke 면 디바운스 저장도 한 번
JS_BLUR = """
need();
var r = { c: F.calls.length, p: F.patches.length };
blurWin();
if (A.poke) saveFormDataToBackend();
return r;
"""

# 5번 — 새 값을 못 본 창: 시험 칸은 원래 값, 값 있는 자격 칸은 빈 값. 화면 값과 기준값을 같이 되돌린다
JS_STALE = """
need();
var r = { c: F.calls.length, touched: 0, secrets: 0 };
F.snap = {};
function stale(id, v) {
  var e = el(id);
  if (!e) return false;
  F.snap[id] = { v: rd(e), b: _formBase[id], hasB: OWN.call(_formBase, id) };
  put(e, v); _formBase[id] = v; r.touched++;
  return true;
}
stale(A.id, A.old);
IDS.forEach(function (id) {
  var e = el(id);
  if (e && id !== A.id && secret(id) && e.type !== 'checkbox' && len(rd(e)) > 0 && stale(id, '')) r.secrets++;
});
_formDirty = {};
blurWin();
return r;
"""

JS_UNSTALE = """
need();
return unsnap();
"""

# 6번 — 다른 창이 저장했다는 알림(C++ 이 부르는 그 함수를 원래 것으로 직접). 깨진 알림·없는 칸도
JS_PATCH_SIM = """
need();
var before = {}, bb = {};
IDS.forEach(function (id) { var e = el(id); if (e) before[id] = rd(e); bb[id] = _formBase[id]; });
var o = {}; o[A.id] = A.v;
var threw = [];
try { F.patchOrig.call(window, JSON.stringify(o)); } catch (e) { threw.push('정상 알림: ' + e.message); }
try { F.patchOrig.call(window, '{깨진 알림'); } catch (e) { threw.push('깨진 알림: ' + e.message); }
var g = {}; g[A.ghost] = '1';
try { F.patchOrig.call(window, JSON.stringify(g)); } catch (e) { threw.push('없는 칸: ' + e.message); }
delete _formBase[A.ghost];     // 앱은 없는 칸도 기준값에 적는다 — 저장 목록 밖이라 해는 없지만 치운다
var movedDom = [], movedBase = [];
IDS.forEach(function (id) {
  if (id === A.id) return;
  var e = el(id);
  if (e && rd(e) !== before[id]) movedDom.push(id);
  if (_formBase[id] !== bb[id]) movedBase.push(id);
});
var t = el(A.id);
return { dom: rd(t) === A.v, base: _formBase[A.id] === A.v, movedDom: movedDom, movedBase: movedBase,
         threw: threw, secrets: secretLens() };
"""

# 7번 — 칸에 포커스를 주고(그 칸의 탭·설정 보기로), 치는 중에 다른 창의 알림이 온다
JS_FOCUS = """
need();
var e = el(A.id);
var T = e.closest('.tab-content');
var tab = T ? T.id.replace(/^tab-/, '') : '';
if (tab && typeof switchTab === 'function' && tabNow() !== tab) switchTab(tab);
if (T && T.dataset.fxview === 'run' && typeof fxSetView === 'function' && !F.fxTab) {
  F.fxTab = tab; F.fxView = 'run'; F.fxDone = T.classList.contains('fx-done');
  fxSetView(T, 'set');
}
e.focus({ preventScroll: true });
if (document.activeElement !== e) {
  var cs = getComputedStyle(e), b = e.getBoundingClientRect();
  return { focused: false, why: '탭 ' + (tab || '?') + ' · display=' + cs.display + ' · visibility=' + cs.visibility
           + ' · 크기 ' + Math.round(b.width) + 'x' + Math.round(b.height) };
}
var r = { focused: true, tab: tab };
put(e, A.typing);                           // 치는 중 — input 이 나가기 전
var o = {}; o[A.id] = A.remote;
F.patchOrig.call(window, JSON.stringify(o)); // 그 사이 다른 창이 저장했다
r.kept = rd(e) === A.typing;
r.base = _formBase[A.id] === A.remote;
return r;
"""

JS_TYPE_ON = """
need();
var e = el(A.id), r = { c: F.calls.length, p: F.patches.length };
e.dispatchEvent(new Event('input', { bubbles: true }));
return r;
"""

JS_UNFOCUS = """
var e = el(A.id);
if (e && document.activeElement === e) e.blur();
return !e || document.activeElement !== e;
"""

# 8번 — 복원을 못 받은 창(8초 폴백): 손대지 않은 칸은 보내지 않고, 손댄 칸만
JS_GUARD = """
need();
var e = el(A.id), c0 = F.calls.length, r0 = _formRestored;
var out = { c: c0 };
try {
  _formRestored = false; _formDirty = {};
  put(e, A.v);                 // 값은 바뀌었지만 사람이 손댄 표시는 없다(= 기본값이 들어간 칸)
  _doSaveForm();
  out.quiet = F.calls.length - c0;
  var c1 = F.calls.length;
  _formDirty[A.id] = true;     // 이제 사람이 손댔다
  _doSaveForm();
  out.after = view(F.calls, c1, A.id, A.v);
} finally {
  _formRestored = r0;
  _formDirty = {};
}
out.back = _formRestored === r0;
return out;
"""

# 되돌리기 1 — 옛 값 흉내를 치우고, 시험 칸을 원래 값으로(save 면 기준값을 지워 반드시 저장)
JS_RESTORE = """
need();
var r = { c: F.calls.length };
var ae = document.activeElement;
if (ae && ae.id && F.stash[ae.id]) ae.blur();
r.snap = unsnap();
_formRestored = F.restored;
delete _formBase[A.ghost];
_formDirty = {};
F.fields.forEach(function (id) {
  var e = el(id), s = F.stash[id];
  if (!e || !s) return;
  put(e, s.v);
  if (A.save) { delete _formBase[id]; _formDirty[id] = true; } else { _formBase[id] = s.v; }
});
if (A.save && F.fields.length) _doSaveForm();
r.sent = view(F.calls, r.c, null, null);
return r;
"""

# 되돌리기 2 — 보기·탭·스크롤, 가로챈 함수를 풀고, 적어 둔 원래 상태와 견준 뒤 흔적을 지운다
JS_FINISH = """
need();
var out = {};
var vt = viewTabBack();
out.tabBack = vt.tab; out.viewBack = vt.view;
backend.saveFormData = F.orig;
window.applyFormPatch = F.patchOrig;
out.unwrapped = backend.saveFormData === F.orig && window.applyFormPatch === F.patchOrig;
var dom = [], base = [];
IDS.forEach(function (id) {
  var e = el(id), s = F.stash[id];
  if (!e || !s) return;
  if (rd(e) !== s.v) dom.push(id);
  var mine = F.fields.indexOf(id) >= 0;
  var wantHas = mine ? true : s.hasB, wantB = mine ? s.v : s.b, has = OWN.call(_formBase, id);
  if (has !== wantHas || (has && _formBase[id] !== wantB)) base.push(id);
});
out.dom = dom; out.base = base;
out.restored = _formRestored === F.restored;
out.dirty = Object.keys(_formDirty || {}).length;
out.ghost = OWN.call(_formBase, A.ghost);
out.calls = F.calls.length; out.patches = F.patches.length;
delete window.__akashiForm;
out.gone = !window.__akashiForm;
return out;
"""


# ── 작은 도우미 ─────────────────────────────────────────────────────────────
def digest(v) -> tuple:
    """저장소 칸 하나의 (길이, SHA-256). 메모리에서 견주기만 한다 — 찍지 않는다."""
    s = v if isinstance(v, str) else json.dumps(v, ensure_ascii=False, sort_keys=True)
    return len(s), hashlib.sha256(s.encode("utf-8")).hexdigest()


def keys_desc(c: dict) -> str:
    if c.get("bad"):
        return "(JSON 이 아닌 묶음)"
    ks = c.get("keys") or []
    s = ", ".join(ks[:8])
    n = c.get("n", len(ks))
    if n > 8:
        s += " 외 %d개" % (n - 8)
    return s or "(빈 묶음)"


def names(ids, most: int = 6) -> str:
    ids = list(ids)
    s = ", ".join(ids[:most])
    return s + (" 외 %d개" % (len(ids) - most) if len(ids) > most else "")


def sent_only(calls: list, key: str):
    """보낸 것이 '그 칸 하나, 그 값' 한 번이면 None, 아니면 까닭."""
    if not calls:
        return "아무것도 보내지 않음"
    if len(calls) > 1:
        return "%d번 보냄 — %s" % (len(calls), " / ".join("[%s]" % keys_desc(c) for c in calls[:3]))
    c = calls[0]
    if c.get("keys") != [key]:
        return "보낸 칸이 [%s] %d개 — 기대: %s 하나" % (keys_desc(c), c.get("n", 0), key)
    if c.get("hit") is not True:
        return "보낸 값이 넣은 값과 다름"
    return None


class Skip(Exception):
    """앞 단계 때문에(또는 화면 사정으로) 이 시험을 할 수 없다."""


class NotApplicable(Exception):
    """이 환경에선 할 일이 없다(기능 창이 없음 등) — 문제로 세지 않는다."""


class Abort(Exception):
    """앱과 연결이 끊겼다 — 더 할 수 없다."""


# ── 점검 ────────────────────────────────────────────────────────────────────
class FormCheck:
    SCENARIOS = (
        (1, "바뀐 칸만 보낸다", "s1"),
        (2, "저장소에 들어가고, 알림이 창으로 돌아온다", "s2"),
        (3, "저장소는 합친다 — 둘째 칸이 첫째 칸·나머지 칸을 지우지 않는다", "s3"),
        (4, "가만있던 창은 포커스를 잃어도 보내지 않는다", "s4"),
        (5, "옛 값을 든 창이 포커스를 잃어도 새 값을 덮지 않는다", "s5"),
        (6, "다른 창의 알림은 그 칸만 바꾸고 되받아 보내지 않는다", "s6"),
        (7, "치는 중인 칸은 알림이 덮지 않는다", "s7"),
        (8, "복원을 못 받은 창은 손댄 칸만 보낸다", "s8"),
        (9, "진짜 두 창 — 기능 창에서 저장한 칸", "s9"),
    )

    def __init__(self, a, main: Page, side, side_name: str, n_sides: int, side_err: str, st: dict, cfg: Path):
        self.a = a
        self.A = main
        self.B = side
        self.b_name = side_name
        self.n_sides = n_sides
        self.b_err = side_err
        self.st = st
        self.cfg = cfg
        self.P, self.S = a.field, a.field2
        self.results: list = []
        self.restore_notes: list = []
        self.info_notes: list = []
        self.installed = False
        self.b_installed = False
        self.focus_emu = False
        self.orig: dict = {}
        self.vp: list = []            # 첫째 칸 시험 값 4개: 1번·6번(알림)·7번(친 값)·7번(알림)
        self.vs: list = []            # 둘째 칸 시험 값 3개: 3번·8번·9번
        self.known: set = set()       # 화면에 찍어도 되는 값(이 공구가 만든 것 · 숫자뿐인 원래 값)
        self.secret_lens: dict = {}
        self.snap0: dict = {}
        self.snap0_file = False
        self.pre: list = []
        self.recovered: list = []
        self.store_last_p = None      # 저장소에 들어 있어야 할 첫째 칸 값(단계마다 바뀐다)
        self.r1 = None                # 1번을 시작할 때의 저장·알림 수 — 2번이 그 뒤의 알림을 본다

    # ── 말하기 ───────────────────────────────────────────────────────────
    def say(self, line: str = "") -> None:
        if not self.a.json:
            print(line, flush=True)

    def detail(self, line: str) -> None:
        if self.a.verbose:
            self.say("       · " + line)

    def record(self, no: int, title: str, status: str, why: str) -> None:
        self.results.append({"no": no, "title": title, "status": status, "why": why})
        self.say("%2d %-3s %s" % (no, STATUS_KO[status], title))
        self.say("       %s" % why)

    def show(self, v) -> str:
        """저장소·화면 값을 사람에게 — 이 공구가 아는 값만 글자로, 나머지는 길이만."""
        if v is _NOFILE:
            return "저장소 파일 없음"
        if v is _MISSING:
            return "칸 없음"
        if isinstance(v, bool):
            return "참" if v else "거짓"
        if isinstance(v, str) and v in self.known:
            return "'%s'" % v
        return "%d자 값" % len(v if isinstance(v, str) else json.dumps(v, ensure_ascii=False))

    # ── 화면 다루기 ──────────────────────────────────────────────────────
    def js(self, body: str, page=None, **kw):
        kw["secretPat"] = SECRET_PAT
        kw.setdefault("ghost", GHOST)
        expr = "(function(A){%s\n%s\n})(%s)" % (JS_HEAD, body, json.dumps(kw, ensure_ascii=False))
        try:
            return (page or self.A).eval(expr)
        except (WsError, OSError) as e:
            raise Abort("앱과 연결이 끊겼습니다 (%s)" % e)

    def cmd(self, page, method: str, params: dict | None = None):
        try:
            return page.cmd(method, params)
        except (WsError, OSError) as e:
            raise Abort("앱과 연결이 끊겼습니다 (%s)" % e)

    def wait_js(self, page, body: str, cond, timeout: float | None = None, every: float = 0.1, **kw):
        end = time.time() + (timeout if timeout is not None else self.a.wait)
        r = self.js(body, page, **kw) or {}
        while not cond(r) and time.time() < end:
            time.sleep(every)
            r = self.js(body, page, **kw) or {}
        return r

    def saved(self, page, r0: dict, key: str, v: str) -> dict:
        """저장(saveFormData)이 올 때까지 기다리고, 온 뒤에도 잠깐 더 — 덤으로 더 보내는지까지 센다."""
        kw = {"c": r0["c"], "p": r0["p"], "id": key, "v": v}
        self.wait_js(page, JS_SINCE, lambda r: len(r.get("calls") or []) >= 1, **kw)
        time.sleep(QUIET)
        r = self.js(JS_SINCE, page, **kw) or {}
        for c in r.get("calls") or []:
            self.detail("보낸 칸: [%s]" % keys_desc(c))
        return r

    def quiet_since(self, page, r0: dict) -> list:
        time.sleep(QUIET + 0.2)
        r = self.js(JS_SINCE, page, c=r0["c"], p=r0["p"], id=None, v=None) or {}
        return r.get("calls") or []

    # ── 저장소(격리 사본의 설정 파일) ─────────────────────────────────────
    def form(self):
        """formData 사전. 파일이 없거나 못 읽으면 None. 원자적 교체 중이면 잠깐 뒤 다시."""
        for _ in range(6):
            try:
                data = json.loads(self.cfg.read_text("utf-8"))
                fd = data.get("formData") if isinstance(data, dict) else None
                return fd if isinstance(fd, dict) else {}
            except FileNotFoundError:
                return None
            except (OSError, ValueError):
                time.sleep(0.1)
        return None

    def store_get(self, fd, key):
        if fd is None:
            return _NOFILE
        return fd.get(key, _MISSING)

    def wait_store(self, key: str, want: str, timeout: float | None = None):
        end = time.time() + (timeout if timeout is not None else self.a.wait)
        while True:
            got = self.store_get(self.form(), key)
            if got == want:
                return True, got
            if time.time() >= end:
                return False, got
            time.sleep(0.1)

    def snapshot(self, fd) -> dict:
        return {k: digest(v) for k, v in (fd or {}).items()}

    def rest(self, fd, exclude) -> tuple:
        """처음 저장소(snap0)와 견준 '나머지 칸' — (사라진 칸, 바뀐 칸) 이름."""
        now = self.snapshot(fd)
        missing = [k for k in self.snap0 if k not in exclude and k not in now]
        changed = [k for k in self.snap0 if k not in exclude and k in now and now[k] != self.snap0[k]]
        return missing, changed

    def rest_counts(self, exclude) -> tuple:
        keys = [k for k in self.snap0 if k not in exclude]
        sec = [k for k in keys if SECRET_RE.search(k)]
        filled = [k for k in sec if self.snap0[k][0] > 0]
        return len(keys), len(sec), len(filled)

    def rest_problems(self, fd, exclude) -> list:
        if fd is None:
            return [(False, "저장소 파일을 읽지 못함 — " + paths.short(self.cfg))]
        missing, changed = self.rest(fd, exclude)
        sm = [k for k in missing if SECRET_RE.search(k)]
        sc = [k for k in changed if SECRET_RE.search(k)]
        return [
            (not missing, "저장소에서 칸 %d개가 사라짐 — %s%s" % (
                len(missing), names(missing), " (자격 칸 %d개 포함)" % len(sm) if sm else "")),
            (not changed, "저장소에서 칸 %d개 값이 바뀜 — %s%s" % (
                len(changed), names(changed), " (자격 칸 %d개 포함)" % len(sc) if sc else "")),
        ]

    # ── 1~9 ──────────────────────────────────────────────────────────────
    def s1(self):
        P, T1 = self.P, self.vp[0]
        r0 = self.js(JS_SET, id=P, v=T1, fire=True)
        self.r1 = r0
        r = self.saved(self.A, r0, P, T1)
        bad = sent_only(r.get("calls") or [], P)
        if bad == "아무것도 보내지 않음":
            bad = "칸을 고쳐도 saveFormData 가 불리지 않음(input → 100ms 뒤 저장이 돌지 않음)"
        return verdict([
            (bad is None, bad or ""),
            (r.get("base") is True, "기준값(_formBase)이 고친 값으로 바뀌지 않음 — 다음에 또 보낸다"),
        ], "%s 에 '%s' → 보낸 칸: %s 하나 · 값 맞음 · 기준값 갱신" % (P, T1, P))

    def s2(self):
        P, T1 = self.P, self.vp[0]
        if not self.r1:
            raise Skip("1번에서 칸을 고치지 못해 건너뜀")
        ok_store, got = self.wait_store(P, T1)
        if ok_store:
            self.store_last_p = T1

        def came(r):
            return any(x.get("keys") == [P] and x.get("hit") for x in r.get("patches") or [])
        r = self.wait_js(self.A, JS_SINCE, came, c=self.r1["c"], p=self.r1["p"], id=P, v=T1)
        pts = r.get("patches") or []
        for x in pts:
            self.detail("받은 알림: [%s]" % keys_desc(x))
        good = [x for x in pts if x.get("keys") == [P] and x.get("hit")]
        other = [x for x in pts if x.get("keys") != [P]]
        return verdict([
            (ok_store, "저장소(설정 파일)의 %s 가 %s — 기대 '%s'" % (P, self.show(got), T1)),
            (bool(good), "저장한 칸의 알림(applyFormPatch)이 이 창으로 돌아오지 않음(알림 %d번)" % len(pts)),
            (not other, "알림에 다른 칸이 섞임 — %s" % " / ".join("[%s]" % keys_desc(x) for x in other[:3])),
            (r.get("dom") is True, "알림을 받은 뒤 화면 값이 '%s' 가 아님" % T1),
        ], "저장소 %s='%s' · 그 칸의 알림 %d번 돌아옴(다른 칸 없음)" % (P, T1, len(good)))

    def s3(self):
        P, S, T2 = self.P, self.S, self.vs[0]
        r0 = self.js(JS_SET, id=S, v=T2, fire=True)
        r = self.saved(self.A, r0, S, T2)
        bad = sent_only(r.get("calls") or [], S)
        ok_store, got = self.wait_store(S, T2)
        fd = self.form()
        gp = self.store_get(fd, P)
        want_p = self.store_last_p or self.vp[0]
        n, ns, nf = self.rest_counts({P, S})
        checks = [
            (bad is None, bad or ""),
            (ok_store, "저장소의 %s 가 %s — 기대 '%s'" % (S, self.show(got), T2)),
            (gp == want_p, "둘째 칸을 저장하자 첫째 칸 %s 가 %s 로 — 저장소가 합치지 않고 통째로 바꿈" % (P, self.show(gp))),
        ] + self.rest_problems(fd, {P, S})
        if n:
            tail = "다른 칸 %d개(자격 칸 %d개, 값 있는 것 %d개) 길이·내용 그대로" % (n, ns, nf)
        else:
            tail = "다른 칸은 저장소에 없음 — 두 시험 칸끼리만 봄(--seed-config 로 띄우면 자격 칸까지)"
        return verdict(checks, "보낸 칸: %s 하나 · 저장소에서 %s 그대로 · %s" % (S, P, tail))

    def s4(self):
        r0 = self.js(JS_BLUR, poke=True)
        calls = self.quiet_since(self.A, r0)
        return verdict([
            (not calls, "바뀐 칸이 없는데 %d번 보냄 — %s" % (len(calls), " / ".join("[%s]" % keys_desc(c) for c in calls[:3]))),
        ], "포커스를 잃고(blur) 저장을 불러도 보낸 것 0건")

    def s5(self):
        P = self.P
        want_p = self.store_last_p or self.vp[0]
        old = self.orig.get(P, "")
        r0, calls, fd, u = {}, [], None, {"bad": []}
        try:
            r0 = self.js(JS_STALE, id=P, old=old)
            calls = self.quiet_since(self.A, {"c": r0["c"], "p": 0})
            fd = self.form()
        finally:
            try:
                u = self.js(JS_UNSTALE) or {"bad": []}
            except (JsError, CdpError) as e:
                u = {"bad": ["(되돌리기 오류: %s)" % scrub(e)[:120]]}
        gp = self.store_get(fd, P)
        ns = int(r0.get("secrets") or 0)
        checks = [
            (not calls, "옛 값을 든 창이 포커스를 잃자 %d번 보냄 — %s ← 원래 버그(가만있던 칸의 옛 값으로 남의 새 값을 덮음)"
             % (len(calls), " / ".join("[%s]" % keys_desc(c) for c in calls[:3]))),
            (gp == want_p, "저장소의 %s 가 %s 로 덮임 — 기대 '%s' 그대로" % (P, self.show(gp), want_p)),
        ] + self.rest_problems(fd, {P, self.S}) + [
            (not u.get("bad"), "옛 값 흉내를 되돌리지 못한 칸 — %s" % names(u.get("bad") or [])),
        ]
        what = ("시험 칸 + 값 있는 자격 칸 %d개를 비운 창" % ns) if ns else \
               "시험 칸이 옛 값인 창(값 있는 자격 칸은 없음 — --seed-config 면 그것도 비워 봄)"
        return verdict(checks, "%s이 포커스를 잃어도 보낸 것 0건 · 저장소 그대로 · 화면 되돌림" % what)

    def s6(self):
        P, T3 = self.P, self.vp[1]
        r = self.js(JS_PATCH_SIM, id=P, v=T3) or {}
        r0 = self.js(JS_BLUR, poke=True)
        calls = self.quiet_since(self.A, r0)
        now = r.get("secrets") or {}
        moved = [k for k in self.secret_lens if now.get(k) != self.secret_lens[k]]
        self.detail("알림 뒤 자격 칸 길이: %s" % (", ".join("%s %d자" % (k, now.get(k, 0)) for k in sorted(now)) or "없음"))
        n_sec = len(self.secret_lens)
        return verdict([
            (r.get("dom") is True, "알림을 받았는데 %s 화면 값이 '%s' 가 아님" % (P, T3)),
            (r.get("base") is True, "알림을 받았는데 기준값이 바뀌지 않음 — 다음 포커스 잃음에 옛 값을 되받아 보냄"),
            (not r.get("movedDom"), "알림은 한 칸인데 다른 칸 %d개 화면 값이 바뀜 — %s"
             % (len(r.get("movedDom") or []), names(r.get("movedDom") or []))),
            (not r.get("movedBase"), "다른 칸 %d개 기준값이 바뀜 — %s"
             % (len(r.get("movedBase") or []), names(r.get("movedBase") or []))),
            (not moved, "자격 칸 길이가 바뀜 — %s" % ", ".join(
                "%s %d→%s자" % (k, self.secret_lens[k], now.get(k, "?")) for k in moved[:6])),
            (not r.get("threw"), "알림이 예외를 던짐 — %s" % scrub(" · ".join(r.get("threw") or []))[:200]),
            (not calls, "알림을 받은 뒤 포커스를 잃자 되받아 보냄 %d번 — %s"
             % (len(calls), " / ".join("[%s]" % keys_desc(c) for c in calls[:3]))),
        ], "그 칸만 '%s' 로 · 다른 칸·자격 칸 %d개 길이 그대로 · 깨진 알림·없는 칸 견딤 · 되받아 보내지 않음"
            % (T3, n_sec))

    def _focus(self, P, T4, T5):
        r = self.js(JS_FOCUS, id=P, typing=T4, remote=T5) or {}
        if r.get("focused") or self.focus_emu:
            return r
        # 앱 창이 앞에 없으면 포커스가 안 들 수 있다 — 화면이 '앞에 있다' 고 여기게 하고 한 번 더
        try:
            self.cmd(self.A, "Emulation.setFocusEmulationEnabled", {"enabled": True})
            self.focus_emu = True
        except CdpError:
            return r
        return self.js(JS_FOCUS, id=P, typing=T4, remote=T5) or {}

    def s7(self):
        P, T4, T5 = self.P, self.vp[2], self.vp[3]
        r = self._focus(P, T4, T5)
        if not r.get("focused"):
            raise Skip("칸에 포커스를 줄 수 없어 건너뜀 — " + scrub(r.get("why") or "?"))
        s, ok_store, got = {}, False, None
        try:
            if r.get("kept"):
                r1 = self.js(JS_TYPE_ON, id=P)
                s = self.saved(self.A, r1, P, T4)
                ok_store, got = self.wait_store(P, T4)
                if ok_store:
                    self.store_last_p = T4
        finally:
            self.js(JS_UNFOCUS, id=P)
        bad = sent_only(s.get("calls") or [], P) if r.get("kept") else None
        return verdict([
            (r.get("kept") is True, "치는 중인 칸을 다른 창의 알림('%s')이 덮음" % T5),
            (r.get("base") is True, "알림 값이 기준값에 들지 않음"),
            (bad is None, "이어 친 값: %s" % (bad or "")),
            (not r.get("kept") or ok_store, "저장소의 %s 가 %s — 기대 '%s'(이어 친 값)" % (P, self.show(got), T4)),
        ], "포커스 있는 칸(탭 %s)은 알림 '%s' 이 덮지 않음 · 이어 친 '%s' 가 저장됨" % (r.get("tab") or "?", T5, T4))

    def s8(self):
        S, T6 = self.S, self.vs[1]
        r = self.js(JS_GUARD, id=S, v=T6) or {}
        after = r.get("after") or []
        bad = sent_only(after, S)
        ok_store, got = self.wait_store(S, T6)
        return verdict([
            (r.get("quiet") == 0, "복원을 못 받은 창이 손대지 않은 칸을 %s번 보냄 — 기본값이 저장된 값을 덮을 수 있음" % r.get("quiet")),
            (bad is None, "손댄 칸만 보내야 하는데 %s" % (bad or "")),
            (r.get("back") is True, "_formRestored 를 원래대로 되돌리지 못함"),
            (ok_store, "저장소의 %s 가 %s — 기대 '%s'" % (S, self.show(got), T6)),
        ], "복원 전(_formRestored=false)엔 손대지 않은 칸 0건 · 손댄 %s 하나만 보냄 · 저장소 '%s'" % (S, T6))

    def s9(self):
        if self.a.no_window:
            raise NotApplicable("--no-window 로 생략")
        if self.n_sides == 0:
            raise NotApplicable("기능 창이 열려 있지 않아 생략 — 앱 메뉴에서 기능 창을 하나 열어 두면 진짜 두 창으로도 대 봅니다")
        if self.b_err:
            return False, self.b_err
        P, S = self.P, self.S
        cur_p = self.store_last_p or self.vp[0]
        T7 = self.vs[2]
        # 본 창에서 저장한 첫째 칸이 기능 창에도 와 있나
        b1 = self.wait_js(self.B, JS_SINCE, lambda r: r.get("dom") is True, c=0, p=0, id=P, v=cur_p)
        a0 = self.js(JS_COUNTS)
        rb0 = self.js(JS_SET, self.B, id=S, v=T7, fire=True)
        rb = self.saved(self.B, rb0, S, T7)
        bad = sent_only(rb.get("calls") or [], S)
        ok_store, got = self.wait_store(S, T7)
        ra = self.wait_js(self.A, JS_SINCE, lambda r: r.get("dom") is True and r.get("base") is True,
                          c=a0["c"], p=a0["p"], id=S, v=T7)
        # 두 창 다 포커스를 잃어 본다 — 누구도 되받아 보내거나 덮지 않아야 한다
        xa0 = self.js(JS_BLUR, poke=True)
        xb0 = self.js(JS_BLUR, self.B, poke=True)
        xa = self.quiet_since(self.A, xa0)
        xb = self.quiet_since(self.B, xb0)
        a_all = (self.js(JS_SINCE, c=a0["c"], p=a0["p"], id=None, v=None) or {}).get("calls") or []
        gs = self.store_get(self.form(), S)
        return verdict([
            (b1.get("dom") is True, "본 창에서 저장한 %s('%s')가 기능 창에 오지 않음" % (P, cur_p)),
            (bad is None, "기능 창이 보낸 것: %s" % (bad or "")),
            (ok_store, "기능 창에서 저장한 %s 가 저장소에 %s — 기대 '%s'" % (S, self.show(got), T7)),
            (ra.get("dom") is True and ra.get("base") is True,
             "기능 창에서 저장한 %s 가 본 창에 오지 않음(화면 %s · 기준값 %s · 받은 알림 %d번)"
             % (S, "맞음" if ra.get("dom") else "다름", "맞음" if ra.get("base") else "다름", len(ra.get("patches") or []))),
            (not a_all and not xa, "본 창이 %d번 보냄 — %s" % (
                len(a_all), " / ".join("[%s]" % keys_desc(c) for c in (a_all or xa)[:3]))),
            (not xb, "기능 창이 포커스를 잃자 다시 보냄 %d번" % len(xb)),
            (gs == T7, "두 창이 포커스를 잃은 뒤 저장소의 %s 가 %s — 기대 '%s' ← 원래 버그" % (S, self.show(gs), T7)),
        ], "기능 창(%s)에서 저장한 %s 가 저장소·본 창에 옴 · 본 창의 저장도 기능 창에 와 있음 · 두 창 다 덮지 않음"
            % (self.b_name or "?", S))

    def run_one(self, no: int, title: str, fn) -> None:
        try:
            ok, why = fn()
            status = "pass" if ok else "fail"
        except Abort:
            raise
        except Skip as e:
            status, why = "skip", str(e)
        except NotApplicable as e:
            status, why = "na", str(e)
        except JsError as e:
            status, why = "fail", "화면 JS 오류 — " + scrub(e)
        except CdpError as e:
            status, why = "fail", scrub(e)
        except Exception as e:  # 공구 쪽 잘못이라도 되돌리기까지는 가야 한다(화면에 가로챈 함수가 남지 않게)
            status, why = "fail", "점검 공구 오류 — %s: %s" % (type(e).__name__, scrub(e)[:200])
        self.record(no, title, status, why)

    # ── 준비 ────────────────────────────────────────────────────────────
    def wait_ready(self, page, timeout: float) -> dict:
        end = time.time() + timeout
        seen_page = None
        r = {}
        while time.time() < end:
            r = self.js(JS_READY, page) or {}
            if r.get("fn") and r.get("be") and r.get("restored") and r.get("loaded"):
                return r
            # 화면은 다 떴는데 함수가 없으면 옛 빌드 — 30초를 다 기다리지 않는다
            if r.get("page") and not r.get("fn"):
                seen_page = seen_page or time.time()
                if time.time() - seen_page > 3:
                    return r
            time.sleep(0.4)
        return r

    def recover(self, page, where: str) -> None:
        rec = self.js(JS_RECOVER, page) or {}
        f = rec.get("fields") or []
        how = "원래 값으로 다시 저장" if rec.get("saved") else "원래 값으로 되돌림"
        self.recovered.append("%s — 가로챈 함수를 풀고%s%s" % (
            where, (" · 시험 칸 %s 을 %s" % (names(f), how)) if f else "",
            (" · 옛 값 흉내 %d칸 되돌림" % rec["snap"]) if rec.get("snap") else ""))

    def prepare(self):
        """붙은 뒤 첫 확인과 설치. 돌릴 수 없으면 까닭 글, 되면 None."""
        r = self.wait_ready(self.A, READY_WAIT)
        if not r.get("fn"):
            return "화면에 폼 저장 함수(applyFormPatch·_doSaveForm·_formBase)가 없습니다 — 3512b0a 이전 빌드이거나 화면이 아직 준비 중입니다."
        if not r.get("be"):
            return "백엔드(backend.saveFormData)가 연결되지 않았습니다 — 화면이 아직 준비 중이면 잠시 뒤 다시."
        if not (r.get("restored") and r.get("loaded")):
            return ("앱이 %d초 안에 폼 복원(restoreFormData)을 보내지 않았습니다 — 이 상태의 창은 '손댄 칸만' 보내므로 "
                    "점검할 수 없습니다(복원이 안 오는 것 자체가 이상입니다 — iso.py log 를 보십시오)." % READY_WAIT)
        if r.get("stale"):
            self.recover(self.A, "본 창")

        if self.B is not None and not self.b_err:
            rb = self.wait_ready(self.B, 10.0)
            if not (rb.get("fn") and rb.get("be") and rb.get("restored") and rb.get("loaded")):
                self.b_err = "기능 창(%s)이 준비되지 않았거나 옛 코드입니다(저장 함수 %s · 복원 %s)" % (
                    self.b_name or "?", "있음" if rb.get("fn") else "없음", "받음" if rb.get("restored") else "못 받음")
            elif rb.get("stale"):
                self.recover(self.B, "기능 창")

        inst = self.js(JS_INSTALL, fields=[self.P, self.S], nvals=[4, 3], save=True) or {}
        if inst.get("bad"):
            return "시험 칸을 쓸 수 없습니다 — " + " · ".join(inst["bad"]) + " (--field/--field2 로 다른 숫자 칸을)"
        self.installed = True
        self.vp = inst["vals"][self.P]
        self.vs = inst["vals"][self.S]
        self.orig = inst.get("orig") or {}
        self.secret_lens = inst.get("secrets") or {}
        self.pre = inst.get("pre") or []
        self.known = set(self.vp) | set(self.vs)
        for v in self.orig.values():
            if isinstance(v, str) and NUMERIC_RE.fullmatch(v):
                self.known.add(v)

        if self.B is not None and not self.b_err:
            ib = self.js(JS_INSTALL, self.B, fields=[self.S], nvals=[0], save=False) or {}
            if ib.get("bad"):
                self.b_err = "기능 창에서 시험 칸을 쓸 수 없음 — " + " · ".join(ib["bad"])
            else:
                self.b_installed = True
                self.pre += ["(기능 창) " + k for k in (ib.get("pre") or [])]
                if (ib.get("orig") or {}).get(self.S) != self.orig.get(self.S):
                    self.info_notes.append("시작할 때 기능 창의 %s 가 본 창과 달랐음(옛 창?)" % self.S)

        # 먼저 한 저장·흔적 치우기가 저장소에 닿고 알림이 돌 때까지 — 그 뒤를 기준으로 삼는다
        time.sleep(0.8 if (self.pre or self.recovered) else 0.2)
        fd = self.form()
        self.snap0_file = fd is not None
        self.snap0 = self.snapshot(fd)

        for pg in [self.A] + ([self.B] if self.b_installed else []):
            self.cmd(pg, "Runtime.enable")
            self.cmd(pg, "Log.enable")
            try:
                pg.pump(0.3)
            except (WsError, OSError):
                pass
            pg.events.clear()
        return None

    # ── 되돌리기 ─────────────────────────────────────────────────────────
    def restore(self) -> bool:
        ok = True

        def step(label: str, fn):
            nonlocal ok
            try:
                return fn()
            except Abort:
                raise
            except Exception as e:  # 하나가 안 돼도 나머지는 되돌린다
                ok = False
                self.restore_notes.append("%s 못 함 — %s" % (label, scrub(e)[:200]))
                return None

        P, S = self.P, self.S
        oP, oS = self.orig.get(P), self.orig.get(S)
        r = step("시험 칸 원래 값으로 저장", lambda: self.js(JS_RESTORE, save=True))
        if r:
            sent = r.get("sent") or []
            extra = sorted({k for c in sent for k in c.get("keys") or []} - {P, S})
            if extra:
                ok = False
                self.restore_notes.append("되돌리며 시험 칸 말고도 보냄 — %s" % names(extra))
            if (r.get("snap") or {}).get("bad"):
                ok = False
                self.restore_notes.append("옛 값 흉내를 되돌리지 못한 칸 — %s" % names(r["snap"]["bad"]))
        for key, want in ((P, oP), (S, oS)):
            good, got = self.wait_store(key, want)
            if not good:
                ok = False
                self.restore_notes.append("저장소의 %s 가 %s — 원래 값 %s 로 돌아오지 않음" % (key, self.show(got), self.show(want)))

        if self.b_installed:
            def b_back():
                w1 = self.wait_js(self.B, JS_SINCE, lambda x: x.get("dom") is True, c=0, p=0, id=P, v=oP)
                w2 = self.wait_js(self.B, JS_SINCE, lambda x: x.get("dom") is True, c=0, p=0, id=S, v=oS)
                if not (w1.get("dom") and w2.get("dom")):
                    self.restore_notes.append("기능 창에 원래 값의 알림이 오지 않아 적어 둔 값으로 되돌림")
                self.js(JS_RESTORE, self.B, save=False)
                return self.js(JS_FINISH, self.B)
            fb = step("기능 창 되돌리기", b_back)
            if fb is not None:
                ok = self._judge_finish(fb, "기능 창") and ok

        fa = step("본 창 되돌리기(함수·보기·탭·스크롤)", lambda: self.js(JS_FINISH))
        if fa is not None:
            ok = self._judge_finish(fa, "본 창") and ok
        if self.focus_emu:
            step("포커스 흉내 끄기", lambda: self.cmd(self.A, "Emulation.setFocusEmulationEnabled", {"enabled": False}))

        # 저장소 — 시험 칸 말고는 처음과 같고, 시험 칸은 원래 값
        fd = self.form()
        if fd is None:
            ok = False
            self.restore_notes.append("되돌린 뒤 저장소를 읽지 못함 — " + paths.short(self.cfg))
        else:
            missing, changed = self.rest(fd, {P, S})
            if missing or changed:
                ok = False
                self.restore_notes.append("저장소 확인: 사라진 칸 %d개 · 바뀐 칸 %d개 — %s" % (
                    len(missing), len(changed), names(missing + changed)))
            new = [k for k in (P, S) if k not in self.snap0 and k in fd]
            if new:
                self.info_notes.append("원래 저장소에 없던 %s 은 원래 화면 값으로 남음(저장소엔 칸을 지우는 길이 없음)" % names(new))
        return ok

    def _judge_finish(self, f: dict, where: str) -> bool:
        miss = []
        if not f.get("unwrapped"):
            miss.append("가로챈 함수가 그대로")
        if f.get("dom"):
            miss.append("화면 값이 원래와 다른 칸 %s" % names(f["dom"]))
        if f.get("base"):
            miss.append("기준값이 원래와 다른 칸 %s" % names(f["base"]))
        if not f.get("restored"):
            miss.append("_formRestored 가 원래와 다름")
        if f.get("dirty"):
            miss.append("손댄 표시(_formDirty) %d칸이 남음" % f["dirty"])
        if f.get("ghost"):
            miss.append("없는 칸의 기준값이 남음")
        if not f.get("tabBack"):
            miss.append("탭이 원래대로 돌아오지 않음")
        if not f.get("viewBack"):
            miss.append("설정/실행 보기가 원래대로 돌아오지 않음")
        if not f.get("gone"):
            miss.append("시험 흔적(window.__akashiForm)이 남음")
        if miss:
            self.restore_notes.append("%s 확인: %s" % (where, " · ".join(miss)))
            return False
        return True

    # ── 본 흐름 ──────────────────────────────────────────────────────────
    def main(self) -> int:
        try:
            why = self.prepare()
        except Abort as e:
            return self.fatal(str(e))
        except (JsError, CdpError) as e:
            why = "화면을 읽지 못했습니다 — " + scrub(e)
        except KeyboardInterrupt:
            why = "사용자가 멈춤(Ctrl-C)"
        except Exception as e:  # 설치 뒤의 공구 쪽 잘못 — 되돌리고 나간다
            why = "점검 공구 오류 — %s: %s" % (type(e).__name__, scrub(e)[:200])
        if why:
            if self.installed:          # 설치 뒤에 멈췄으면 치우고 나간다
                try:
                    self.restore()
                except (Abort, KeyboardInterrupt):
                    pass
            return self.fatal(why)

        self.say("폼 저장 점검 — 포트 %d · 앱 %s (격리 사본 PID %s)" % (self.a.port, self.st.get("version") or "?", self.st.get("pid")))
        n, ns, nf = self.rest_counts(set())
        side = ("기능 창 %s%s" % (self.b_name or "?", " 외 %d개(첫째만 씀)" % (self.n_sides - 1) if self.n_sides > 1 else "")
                if self.n_sides and not self.a.no_window else "기능 창 없음")
        self.say("  시험 칸 %s(원래 %s) · %s(원래 %s) · 화면 자격 칸 %d개(값 있는 것 %d개) · %s" % (
            self.P, self.show(self.orig.get(self.P, "")), self.S, self.show(self.orig.get(self.S, "")),
            len(self.secret_lens), sum(1 for v in self.secret_lens.values() if v > 0), side))
        self.say("  저장소 %s — %s" % (
            paths.short(self.cfg),
            ("칸 %d개(자격 칸 %d개, 값 있는 것 %d개)%s" % (n, ns, nf, " · 설정 복사본" if self.st.get("seeded") else ""))
            if self.snap0_file else "아직 파일 없음(첫 저장 때 생김)"))
        for line in self.recovered:
            self.say("  지난 점검이 남긴 흔적을 먼저 치움: " + line)
        if self.pre:
            self.say("  시작 전 저장 안 된 칸 %d개를 먼저 저장함(앱이 다음 포커스 잃음에 할 일): %s" % (len(self.pre), names(self.pre)))
        if self.a.verbose and self.secret_lens:
            self.say("  자격 칸 길이: " + ", ".join("%s %d자" % (k, v) for k, v in sorted(self.secret_lens.items())))
        if self.a.verbose:
            self.say("  시험 값: %s %s · %s %s" % (self.P, "/".join(self.vp), self.S, "/".join(self.vs)))
        self.say("")

        aborted, lost = "", False
        try:
            for no, title, fn in self.SCENARIOS:
                self.run_one(no, title, getattr(self, fn))
        except Abort as e:
            aborted, lost = str(e), True
        except KeyboardInterrupt:
            aborted = "사용자가 멈춤(Ctrl-C)"

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
            for pg in [self.A] + ([self.B] if self.b_installed else []):
                try:
                    pg.pump(0.3)
                except (WsError, OSError):
                    pass
                for e in pg.collect_errors():
                    (exc if e.startswith("예외") else other).append(scrub(e))
        return self.report(aborted, lost, restored, exc, other)

    def fatal(self, msg: str) -> int:
        if self.a.json:
            print(json.dumps({"tool": "check_forms", "port": self.a.port, "error": msg,
                              "restore_notes": self.restore_notes, "exit": 2}, ensure_ascii=False, indent=1))
        else:
            for n in self.restore_notes:
                print("   " + n)
            print("돌릴 수 없음 — " + msg)
        return 2

    def report(self, aborted: str, lost: bool, restored: bool, exc: list, other: list) -> int:
        fails = [r["no"] for r in self.results if r["status"] == "fail"]
        skips = [r["no"] for r in self.results if r["status"] == "skip"]
        nas = [r for r in self.results if r["status"] == "na"]
        self.say("")
        if not lost:
            self.say("   화면 JS 예외 %d건%s" % (len(exc), "" if not other else " (참고: console.error·기록 오류 %d건)" % len(other)))
            for e in exc[:5]:
                self.say("       " + e[:240])
            if self.a.verbose:
                for e in other[:5]:
                    self.say("       참고 " + e[:240])
            self.say("   정리 — %s" % ("시험 칸을 원래 값으로 저장 · 가로챈 함수·기준값·탭·보기·포커스·스크롤 되돌림"
                                    if restored else "다 되돌리지 못함"))
            for n in self.restore_notes:
                self.say("       " + n)
            for n in self.info_notes:
                self.say("       알림: " + n)
        if aborted:
            self.say("   도중에 멈춤 — " + aborted)

        parts = []
        if fails:
            parts.append("실패 %s번" % "·".join(str(n) for n in fails))
        if skips:
            parts.append("건너뜀 %s번" % "·".join(str(n) for n in skips))
        if exc:
            parts.append("JS 예외 %d건" % len(exc))
        if not lost and not restored:
            parts.append("되돌리기 못 함")
        if aborted:
            parts.append("끝까지 못 돎")
        problems = len(fails) + len(skips) + (1 if exc else 0) + (0 if (lost or restored) else 1) + (1 if aborted else 0)
        total = len(self.SCENARIOS)
        done = len(self.results) == total
        if problems == 0 and done:
            ran = total - len(nas)
            line = "통과 — 폼 저장 %d가지가 모두 맞고%s, 화면 JS 예외 없이 원래대로 되돌렸습니다" % (
                ran, "(9번 진짜 두 창은 생략 — 기능 창 없음)" if nas else "")
        else:
            problems = max(problems, 1)
            line = "문제 %d건 — %s" % (problems, " · ".join(parts) or "결과가 모자람")
        if aborted and not lost:
            code = 2            # Ctrl-C — 판정이 아니다
        else:
            code = 0 if problems == 0 and done else 1

        if self.a.json:
            print(json.dumps({
                "tool": "check_forms", "port": self.a.port,
                "fields": {"primary": self.P, "secondary": self.S},
                "test_values": {self.P: self.vp, self.S: self.vs},
                "secret_lengths": self.secret_lens,          # 길이만 — 값·지문은 없다
                "store": {"file": paths.short(self.cfg), "keys_at_start": len(self.snap0)},
                "feature_window": self.b_name if self.b_installed else "",
                "preflushed": self.pre, "recovered": self.recovered,
                "results": self.results, "js_exceptions": exc, "console_errors": len(other),
                "restored": restored, "restore_notes": self.restore_notes, "notes": self.info_notes,
                "aborted": aborted, "problems": problems, "verdict": line, "exit": code,
            }, ensure_ascii=False, indent=1))
        else:
            self.say(line)
        return code


# ── 격리 사본의 저장소 자리 ────────────────────────────────────────────────
def config_path(st: dict):
    """(설정 파일 경로, 오류 글). 격리 홈에서 셈하고, 진짜 홈·진짜 자료 폴더를 가리키면 오류."""
    h = st.get("home")
    if not h:
        return None, "격리 사본 기록에 홈 폴더가 없습니다 — iso.py 로 다시 띄우십시오."
    home = Path(h).resolve()
    if home == paths.real_home():
        return None, "격리 홈이 진짜 홈을 가리킵니다 — 멈춥니다: " + paths.short(home)
    cfg = (paths.data_dir(home) / paths.CONFIG_NAME).resolve()
    real_data = paths.data_dir().resolve()
    if cfg == real_data or real_data in cfg.parents:
        return None, "격리 사본의 저장소가 진짜 자료 폴더를 가리킵니다 — 멈춥니다: " + paths.short(cfg)
    return cfg, None


def main() -> int:
    ap = argparse.ArgumentParser(
        description="폼 저장 점검 — 창마다 바뀐 칸만 보내고·저장소는 합치고·모든 창에 알리는지(3512b0a)를 "
                    "격리 사본에서 아홉 가지로 대 본다. 자격 칸의 값은 찍지도 바꾸지도 않는다(길이만).",
        epilog="먼저 python3 akashi/iso.py start 로 격리 사본을 띄우십시오(--seed-config 면 자격 칸 길이까지 대 봄).\n"
               "사용자의 앱에는 붙지 않습니다. 기능 창을 하나 열어 두면 9번(진짜 두 창)도 합니다.\n"
               "종료 코드: 0 모두 통과 · 1 문제 있음 · 2 돌릴 수 없음",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=os.environ.get("AKASHI_PORT") or 9334,   # ★ 글이면 argparse 가 int 로 — 빈 값·숫자 아님도 넘어지지 않고 종료 2
                    help="격리 사본의 CDP 포트 (기본 9334 · 환경 변수 AKASHI_PORT)")
    ap.add_argument("--field", default=DEFAULT_FIELD,
                    help="시험에 쓸 첫째 칸 id (기본 %s · 숫자·글 칸만, 자격 칸은 안 됨)" % DEFAULT_FIELD)
    ap.add_argument("--field2", default=DEFAULT_FIELD2,
                    help="시험에 쓸 둘째 칸 id (기본 %s · 첫째 칸과 달라야 함)" % DEFAULT_FIELD2)
    ap.add_argument("--wait", type=float, default=3.0,
                    help="저장·알림·저장소 쓰기를 기다릴 초 (기본 3)")
    ap.add_argument("--no-window", action="store_true",
                    help="기능 창이 열려 있어도 9번(진짜 두 창)을 하지 않는다 — 기능 창에는 붙지도 않는다")
    ap.add_argument("--need-window", action="store_true",
                    help="기능 창이 열려 있지 않으면 돌리지 않는다(종료 코드 2)")
    ap.add_argument("-v", "--verbose", action="store_true",
                    help="단계마다 보낸 칸·받은 알림·자격 칸 길이·시험 값을 찍는다(값은 시험 값만)")
    ap.add_argument("--json", action="store_true", help="사람용 글 대신 JSON 한 덩어리로")
    a = ap.parse_args()

    def fatal(msg: str) -> int:
        if a.json:
            print(json.dumps({"tool": "check_forms", "port": a.port, "error": msg, "exit": 2},
                             ensure_ascii=False, indent=1))
        else:
            print("돌릴 수 없음 — " + msg)
        return 2

    for opt, v in (("--field", a.field), ("--field2", a.field2)):
        if not FIELD_RE.fullmatch(v or ""):
            return fatal("%s 는 영어 소문자·숫자·하이픈으로 된 칸 id 여야 합니다(예: fanbox-delay)." % opt)
        if SECRET_RE.search(v):
            return fatal("%s %s — 자격 칸(이름에 %s 중 하나)은 시험 칸으로 쓰지 않습니다." % (opt, v, SECRET_PAT.replace("|", "·")))
    if a.field == a.field2:
        return fatal("--field 와 --field2 는 서로 다른 칸이어야 합니다.")
    if not (0 < a.port < 65536):
        return fatal("--port 가 올바르지 않습니다: %d" % a.port)
    if not (0.5 <= a.wait <= 30):
        return fatal("--wait 는 0.5~30 초여야 합니다.")
    if a.no_window and a.need_window:
        return fatal("--no-window 와 --need-window 는 함께 쓸 수 없습니다.")

    err, st = guard(a.port)
    if err:
        return fatal(err)
    cfg, why = config_path(st)
    if why:
        return fatal(why)
    try:
        ts = cdplib.page_targets(a.port)
    except (OSError, ValueError) as e:
        return fatal("포트 %d 에서 화면 목록을 읽지 못했습니다 (%s)" % (a.port, e))
    mains = [t for t in ts if "window=" not in t.get("url", "")]
    sides = [t for t in ts if "window=" in t.get("url", "")]
    if not mains:
        return fatal("포트 %d 에 본 창(index.html) 화면이 없습니다 — 화면이 준비될 때까지 기다린 뒤 다시." % a.port)
    if a.need_window and not sides:
        return fatal("기능 창이 열려 있지 않습니다(--need-window) — 앱 메뉴에서 기능 창을 하나 연 뒤 다시.")
    ws_url = mains[0].get("webSocketDebuggerUrl") or ""
    # ★ 주소는 앱이 알려 준 것 — check_adapt 처럼 이 기계(127.0.0.1) 밖으로는 나가지 않는다.
    #   다른 디버거가 먼저 붙어 있으면 주소가 빠져 온다 — ["…"] 로 읽으면 KeyError 로 넘어져 종료 1(문제)로 보였다
    if urlparse(ws_url).hostname not in LOOPBACK:
        return fatal("화면 주소가 비었거나 이 기계(127.0.0.1)가 아닙니다 — 붙지 않습니다(다른 디버거가 먼저 붙어 있으면 비어 옵니다).")
    try:
        main_page = Page(ws_url, timeout=15)
    except (WsError, OSError) as e:
        return fatal("화면에 붙지 못했습니다 (%s)" % e)

    side, side_name, side_err = None, "", ""
    if sides and not a.no_window:
        m = re.search(r"window=([a-z]+)", sides[0].get("url", ""))
        side_name = m.group(1) if m else "?"
        side_ws = sides[0].get("webSocketDebuggerUrl") or ""
        if urlparse(side_ws).hostname not in LOOPBACK:
            side_err = "기능 창(%s)의 화면 주소가 비었거나 이 기계가 아닙니다 — 붙지 않습니다" % side_name
        else:
            try:
                side = Page(side_ws, timeout=15)
            except (WsError, OSError) as e:
                side_err = "기능 창(%s)에 붙지 못했습니다 (%s)" % (side_name, e)
    try:
        return FormCheck(a, main_page, side, side_name, len(sides), side_err, st, cfg).main()
    finally:
        main_page.close()
        if side is not None:
            side.close()


if __name__ == "__main__":
    sys.exit(main())
