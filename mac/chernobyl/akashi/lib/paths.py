# -*- coding: utf-8 -*-
"""앱·저장소·자료 폴더 찾기.

★ 경로를 박지 않는다.
  설치본이 어느 디스크에 있는지, 사용자 이름이 무엇인지는 기계마다 다르다. 공개 저장소에
  올라가는 공구라 특정 사람의 경로가 들어가서도 안 된다. 번들 식별자(com.hanishiki.*)로 찾는다.
"""
from __future__ import annotations

import glob
import os
import plistlib
import pwd
import sys
from pathlib import Path

LIB = Path(__file__).resolve().parent
AKASHI = LIB.parent
CHERNOBYL = AKASHI.parent            # mac/chernobyl
REPO = CHERNOBYL.parent.parent       # 저장소 뿌리

# 옛 이름(miyo·predormition)으로 설치된 번들도 알아본다 — kill_app.sh 와 같은 목록.
BUNDLE_ID_PREFIXES = ("com.hanishiki.", "com.miyo.", "com.predormition.")
DATA_DIR_NAME = "Hanishiki"          # ~/Library/Application Support/<이름>
CONFIG_NAME = "hanishiki_config.json"


def info_plist(app) -> dict:
    try:
        with open(Path(app) / "Contents" / "Info.plist", "rb") as f:
            return plistlib.load(f)
    except (OSError, plistlib.InvalidFileException, ValueError):
        return {}


def bundle_id(app) -> str:
    return str(info_plist(app).get("CFBundleIdentifier", ""))


def is_our_app(app) -> bool:
    return Path(app).is_dir() and bundle_id(app).startswith(BUNDLE_ID_PREFIXES)


def exe_of(app) -> Path:
    name = info_plist(app).get("CFBundleExecutable") or Path(app).stem
    return (Path(app) / "Contents" / "MacOS" / name).resolve()


def version_of(app) -> str:
    p = info_plist(app)
    return "%s (%s)" % (p.get("CFBundleShortVersionString", "?"), p.get("CFBundleVersion", "?"))


def find_build_app():
    for c in sorted(glob.glob(str(CHERNOBYL / "build" / "*.app"))):
        if is_our_app(c):
            return Path(c)
    return None


def running_apps() -> list:
    """지금 켜져 있는 우리 앱 번들(빌드본·격리 사본의 복제본은 뺀다).

    ★ 설치 자리는 사람마다, 때마다 다르다(외장 디스크 → 문서 폴더로 옮긴 적이 있다). 정해 둔 자리만
      뒤지면 '쓰지 않는 설치본' 을 고쳐 놓고 끝날 수 있다. 켜져 있는 것이 곧 쓰는 것이다."""
    from . import proc    # proc 은 paths 를 부르지 않는다 — 고리 없음
    build = find_build_app()
    out = []
    for name in ("Hanishiki", "Miyo", "Predormition"):
        for pid in proc.pids_named(name):
            exe = proc.pid_path(pid)
            app = next((q for q in Path(exe or "/").parents if q.suffix == ".app"), None)
            if not app or not is_our_app(app) or "akashi-iso" in str(app):
                continue
            if build is not None and app.resolve() == build.resolve():
                continue
            if app not in out:
                out.append(app)
    return out


def find_installed_apps() -> list:
    """켜져 있는 것 먼저, 그다음 /Applications · ~/Applications · 외장 디스크 맨 위(/Volumes/*/) 의 설치본."""
    home = real_home()
    pats = ["/Applications/*.app", str(home / "Applications" / "*.app"), "/Volumes/*/*.app"]
    env = os.environ.get("HANISHIKI_APP")
    out = [Path(env)] if env and is_our_app(env) else []
    build = find_build_app()
    for p in running_apps():
        if p not in out:
            out.append(p)
    for pat in pats:
        for c in sorted(glob.glob(pat)):
            p = Path(c)
            if is_our_app(p) and p not in out and (build is None or p.resolve() != build.resolve()):
                out.append(p)
    return out


def real_home() -> Path:
    """HOME 을 바꿔 띄운 격리 사본 안에서도 '진짜' 홈. 격리 경로가 진짜 홈을 가리키는지 막는 데 쓴다."""
    return Path(pwd.getpwuid(os.getuid()).pw_dir).resolve()


def data_dir(home=None) -> Path:
    home = Path(home) if home else real_home()
    if sys.platform == "darwin":
        return home / "Library" / "Application Support" / DATA_DIR_NAME
    return home / ".local" / "share" / DATA_DIR_NAME


def bundled_python(app):
    p = Path(app) / "Contents" / "Resources" / "python_env" / "bin" / "python3"
    return p if p.exists() else None


def short(p) -> str:
    """사람에게 보일 때 진짜 홈은 ~ 로 줄인다(기록·화면에 사용자 이름을 남기지 않는다)."""
    s = str(p)
    h = str(real_home())
    # ★ 경계까지 본다 — 홈 폴더 이름이 ab 일 때, 옆의 abc 폴더 안 경로를 '~c/…' 로 잘못 줄이지 않게.
    return "~" + s[len(h):] if s == h or s.startswith(h + os.sep) else s
