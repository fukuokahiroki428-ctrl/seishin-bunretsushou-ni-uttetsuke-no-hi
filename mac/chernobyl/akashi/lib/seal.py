# -*- coding: utf-8 -*-
"""앱 서명 봉인 판정 — 앱의 Common::checkAppSeal 과 같은 뜻.

    from lib.seal import seal_state
    state, detail = seal_state(app, strip=False)   # "ok" · "finder-detritus" · "broken"

★ 왜
  · 엄격 검사(codesign --verify --deep --strict)는 앱 안에 'Finder 정보' 확장 속성이 있으면 실패한다
    ("resource fork, Finder information, or similar detritus not allowed"). 서명 자체는 멀쩡해도 그렇다.
  · 다른 곳에서 Finder 로 복사한 앱에는 프레임워크 폴더마다 이 표시가 붙는다 — 지우면 된다(내용은 그대로).
  · iCloud 로 동기화되는 폴더(문서·데스크탑)에 둔 앱은 시스템(파일 공급자)이 앱 폴더 맨 위에 이 표시를
    붙이고, 지워지지 않는다. 다시 서명해도 그대로다. 그러니 '서명은 맞음' 으로 보고 다시 서명하지 않는다.
  · strip=True 는 지울 수 있는 표시를 지운다(확장 속성만 — 파일 내용은 건드리지 않는다).
    점검(doctor)은 사용자 앱을 고치지 않으므로 strip=False 로 부른다.
"""
from __future__ import annotations

import subprocess
from pathlib import Path

DETRITUS = "detritus"


def _verify(app: Path, strict: bool) -> tuple:
    args = ["codesign", "--verify", "--deep"] + (["--strict"] if strict else []) + [str(app)]
    try:
        r = subprocess.run(args, capture_output=True, text=True, timeout=900)
    except subprocess.TimeoutExpired:
        return False, "codesign 시간 초과"
    return r.returncode == 0, (r.stderr or r.stdout or "").strip()


def _last(msg: str) -> str:
    lines = [l for l in msg.splitlines() if l.strip()]
    return lines[-1][:200] if lines else ""


def seal_state(app: Path, strip: bool = False) -> tuple:
    """(state, detail) — state 는 "ok" · "finder-detritus" · "broken"."""
    ok, err = _verify(app, True)
    if ok:
        return "ok", ""
    if DETRITUS in err:
        if strip:
            for attr in ("com.apple.FinderInfo", "com.apple.ResourceFork"):
                subprocess.run(["xattr", "-rd", attr, str(app)],
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            ok, err = _verify(app, True)
            if ok:
                return "ok", ""
        if DETRITUS in err and _verify(app, False)[0]:
            return "finder-detritus", _last(err)
    return "broken", _last(err)


def describe(state: str) -> str:
    return {"ok": "맞음",
            "finder-detritus": "맞음(iCloud 폴더라 앱 폴더에 Finder 표시가 붙어 엄격 검사만 걸림 — 앱은 정상)",
            "broken": "틀림"}.get(state, state)
