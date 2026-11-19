#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""정비 점검 — 앱을 띄우지 않고 빌드본·설치본·자료 폴더가 멀쩡한지 본다. 읽기만 한다.

    python3 akashi/doctor.py              # 전부
    python3 akashi/doctor.py --quick      # 서명·도구 실행은 건너뛰고 빨리
    python3 akashi/doctor.py --show-paths # 자료 경로도 보인다(기본은 '있음 · 여유' 만)

★ 무엇을 보나
  · 서명(codesign --verify --deep --strict) — 깨진 번들은 macOS 가 소리 없이 죽인다.
  · 번들 도구(ffmpeg · ffprobe · yt-dlp · rclone · deno · exiftool)가 실제로 도는지(--version).
  · 번들 파이썬이 requirements.txt 의 모듈을 모두 import 하는지.
  · 아키텍처 — 호스트가 arm64 인데 x86_64 만 든 실행 파일(로제타가 있어야만 돈다). build.sh 가 경고하는 것과 같다.
    ffprobe 하나만 x86_64 로 들어가 있던 적이 있다. 로제타가 없는 맥에서는 "Bad CPU type" 으로 안 돈다.
  · 빌드본만: 화면·파이썬 도구가 소스와 같은지 — 자원만 바꾼 빌드가 옛것을 남긴 적이 있다(sync_resources.sh).
  · 자료 폴더: 설정 파일이 있는지(크기만) · 고침 꾸러미 판 · 임시·저장 폴더가 있는지와 여유 공간.

★ 찍지 않는 것
  설정 파일의 값(계정·토큰·비밀번호·API 키). 경로도 기본은 가린다 — 디스크 배치가 드러나기 때문이다.
"""
from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lib import paths  # noqa: E402
from lib.seal import seal_state, describe  # noqa: E402

# pip 이름 → import 이름 (다른 것만)
MODULE = {"xclienttransaction": "x_client_transaction", "yt-dlp": "yt_dlp", "Pillow": "PIL",
          "beautifulsoup4": "bs4", "discord.py": "discord"}
TOOLS = [("ffmpeg", ["-version"]), ("ffprobe", ["-version"]), ("yt-dlp", ["--version"]),
         ("rclone", ["version"]), ("deno", ["--version"])]


class Report:
    def __init__(self):
        self.rows = []

    def add(self, level: str, what: str, detail: str = "") -> None:
        self.rows.append((level, what, detail))
        mark = {"OK": "  OK  ", "WARN": " WARN ", "FAIL": " FAIL "}[level]
        print("%s %s%s" % (mark, what, (" — " + detail) if detail else ""), flush=True)


def run(cmd, timeout=25):
    try:
        r = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return r.returncode, (r.stdout or r.stderr or "").strip()
    except subprocess.TimeoutExpired:
        return -1, "시간 초과(%ds)" % timeout
    except OSError as e:
        return -2, str(e)


def tool_path(app: Path, name: str):
    for p in (app / "Contents" / "MacOS" / name, app / "Contents" / "Resources" / "tools" / name):
        if p.is_file():
            return p
    return None


def is_macho(p: Path) -> bool:
    try:
        with open(p, "rb") as f:
            return f.read(4) in (b"\xcf\xfa\xed\xfe", b"\xce\xfa\xed\xfe", b"\xca\xfe\xba\xbe", b"\xbe\xba\xfe\xca",
                                 b"\xca\xfe\xba\xbf", b"\xbf\xba\xfe\xca")   # 64비트 FAT 도
    except OSError:
        return False


def disabled_chrome_features() -> list:
    """수집 Chrome 에 넘기는 --disable-features 이름들(RealChromeCrawler.cpp 의 disabledFeatures 초기 목록 — 맥에 쓰는 것)"""
    import re
    src = paths.CHERNOBYL / "src" / "platforms" / "RealChromeCrawler.cpp"
    try:
        t = src.read_text(encoding="utf-8")
    except OSError:
        return []
    m = re.search(r"QStringList disabledFeatures = \{(.*?)\};", t, re.S)
    return re.findall(r'QStringLiteral\("([^"]+)"\)', m.group(1)) if m else []


def chrome_knows(framework: Path, names: list) -> list:
    """번들 Chrome 이 모르는 기능 이름. 이름 문자열('\\0이름\\0')도, 새 BASE_FEATURE 의 'k이름'('\\0k이름\\0')도 없으면 없는 기능이다
    (있다고 꼭 기능은 아니다 — 그래도 이름이 바뀌거나 빠진 것은 잡는다)"""
    import mmap
    try:
        with open(framework, "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as m:
            return [n for n in names if m.find(b"\0" + n.encode() + b"\0") < 0 and m.find(b"\0k" + n.encode() + b"\0") < 0]
    except (OSError, ValueError):
        return names


def check_app(app: Path, rep: Report, quick: bool, is_build: bool) -> None:
    print("\n── %s %s  %s  (%s)" % ("빌드본" if is_build else "설치본", app.name, paths.version_of(app), paths.bundle_id(app)))
    if not quick:
        # 점검은 사용자 앱을 고치지 않는다(strip=False) — iCloud 폴더의 Finder 표시는 알리기만 한다.
        state, why = seal_state(app, strip=False)
        rep.add({"ok": "OK", "finder-detritus": "WARN"}.get(state, "FAIL"), "서명",
                "" if state == "ok" else (describe(state) if state != "broken" else why[:160]))
    # 도구
    for name, args in TOOLS:
        p = tool_path(app, name)
        if not p:
            rep.add("WARN", name, "번들에 없음")
            continue
        if quick:
            rep.add("OK" if os.access(p, os.X_OK) else "FAIL", name, "있음" if os.access(p, os.X_OK) else "실행 권한 없음")
            continue
        code, out = run([str(p)] + args)
        first = out.splitlines()[0][:70] if out else ""
        rep.add("OK" if code == 0 else "FAIL", name, first if code == 0 else "안 돎 (%s)" % first)
    perl = app / "Contents" / "Resources" / "tools" / "perl" / "bin" / "perl"
    ex = app / "Contents" / "Resources" / "tools" / "exiftool" / "exiftool"
    if ex.is_file():
        if quick:
            rep.add("OK", "exiftool", "있음")
        else:
            code, out = run([str(perl) if perl.is_file() else "perl", str(ex), "-ver"])
            rep.add("OK" if code == 0 else "FAIL", "exiftool", ("%s (%s perl)" % (out.strip(), "번들" if perl.is_file() else "시스템")) if code == 0 else out[:80])
    else:
        rep.add("WARN", "exiftool", "번들에 없음")
    # 파이썬
    py = paths.bundled_python(app)
    req = paths.REPO / "requirements.txt"
    if not py:
        rep.add("FAIL", "번들 파이썬", "없음")
    elif req.is_file() and not quick:
        names = [l.split("==")[0].split(">=")[0].strip() for l in req.read_text().splitlines()
                 if l.strip() and not l.startswith("#")]
        mods = [MODULE.get(n, n.replace("-", "_")) for n in names]
        # ★ -B: 바이트코드(__pycache__)를 번들에 쓰지 않는다. 쓰면 번들 봉인이 깨지고, 앱은 다음에 켤 때
        #   스스로 다시 서명한다(설치본을 건드리는 셈이다). 점검은 읽기만 해야 한다.
        code, out = run([str(py), "-B", "-c", "import importlib,sys\nbad=[]\nfor m in sys.argv[1:]:\n  try: importlib.import_module(m)\n  except Exception as e: bad.append(m+':'+type(e).__name__)\nprint(','.join(bad))"] + mods, timeout=60)
        bad = [b for b in out.split(",") if b] if code == 0 else ["실행 실패"]
        rep.add("OK" if not bad else "FAIL", "번들 파이썬 모듈 %d개" % len(mods), "모두 import" if not bad else "안 되는 것: " + ", ".join(bad))
    # 아키텍처 — 번들 안 모든 실행 파일(Mach-O)이 arm64(또는 arm64e)를 품어야 한다. 로제타는 macOS 27 을 끝으로 빠진다(애플,
    #   WWDC 2025) — Intel 전용이 하나라도 있으면 다음 macOS 에서 그 기능이 멈추므로 FAIL. 빌드하는 맥의 uname 과 견주지 않는다.
    #   ★ 실행 권한 · 확장자가 아니라 파일 머리(매직)로 고른다 — 프레임워크 본체(Qt 23개)는 0644 에 확장자도 없어 예전엔 빠졌다(검토 r54).
    cands = []
    for root, _dirs, files in os.walk(app / "Contents"):
        for fn in files:
            p = Path(root) / fn
            if p.is_file() and not p.is_symlink() and is_macho(p):
                cands.append(p)
    lone = []
    for p in cands:
        code, out = run(["lipo", "-archs", str(p)], timeout=10)
        archs = out.split()
        if code == 0 and not ({"arm64", "arm64e"} & set(archs)):
            lone.append("%s(%s)" % (p.relative_to(app), out.strip()))
    rep.add("OK" if not lone else "FAIL", "아키텍처 arm64 — 실행 파일 %d개" % len(cands),
            "모두 arm64" if not lone else "arm64 가 없는 것(로제타가 빠지면 멈춤): " + ", ".join(lone[:6]))
    # 캡처용 Chrome 이 뜨는가(앱의 자가진단과 같은 검사 — 뜨는지만)
    chrome = app / "Contents" / "Resources" / "chromium" / "Chromium.app" / "Contents" / "MacOS" / "Google Chrome for Testing"
    if chrome.is_file():
        if quick:   # --quick 은 도구를 띄우지 않는다(다른 도구와 같게)
            ok = os.access(chrome, os.X_OK)
            rep.add("OK" if ok else "FAIL", "캡처용 Chrome", "있음" if ok else "실행 권한 없음")
        else:
            code, out = run([str(chrome), "--version"], timeout=30)
            rep.add("OK" if code == 0 and "Chrome" in out else "FAIL", "캡처용 Chrome", out.strip()[:80] or "실행되지 않습니다")
            # 수집 Chrome 에 끄라고 넘기는 기능 이름이 이 Chrome 에 실제로 있는가 — 모르는 이름은 Chrome 이 말없이 넘겨 그 장치가 무력해진다
            #   (r53 의 FreezingOnEnergySaver 가 그랬다). Chrome 을 올렸을 때 이름이 바뀌면 여기서 드러난다.
            names = disabled_chrome_features()
            fws = sorted((app / "Contents" / "Resources" / "chromium" / "Chromium.app" / "Contents" / "Frameworks").glob(
                "*Framework.framework/Versions/[0-9]*/*Framework"))
            if names and fws:
                unknown = chrome_knows(fws[-1], names)
                rep.add("OK" if not unknown else "FAIL", "수집 Chrome 의 끄는 기능 %d개" % len(names),
                        "모두 Chrome 에 있음" if not unknown else "Chrome 에 없는 이름(아무 일도 안 함): " + ", ".join(unknown))
    # 빌드본: 자원이 소스와 같은가
    if is_build:
        diff = []
        pairs = [(paths.CHERNOBYL / "resources" / "html" / "index.html", app / "Contents" / "Resources" / "html" / "index.html")]
        for s in sorted((paths.CHERNOBYL / "resources" / "tools").glob("*.py")):
            pairs.append((s, app / "Contents" / "Resources" / "tools" / s.name))
        for s, b in pairs:
            if not b.is_file() or s.read_bytes() != b.read_bytes():
                diff.append(s.name)
        rep.add("OK" if not diff else "FAIL", "빌드본 자원이 소스와 같음(%d개)" % len(pairs),
                "" if not diff else "다른 것: " + ", ".join(diff) + " — 다시 빌드(./build.sh)")


def free_gb(p: Path) -> float:
    return shutil.disk_usage(str(p)).free / 1e9


def check_data(rep: Report, show: bool) -> None:
    d = paths.data_dir()
    print("\n── 자료 폴더 %s" % (paths.short(d) if show else "(~/Library/Application Support/…)"))
    cfg = d / paths.CONFIG_NAME
    if not cfg.is_file():
        rep.add("WARN", "설정 파일", "없음(첫 실행 전)")
        return
    try:
        c = json.loads(cfg.read_text("utf-8"))
        rep.add("OK", "설정 파일", "%d 바이트 · 칸 %d개 · 계정 묶음 %d" % (cfg.stat().st_size, len(c), len(c.get("accounts") or {})))
    except (OSError, ValueError) as e:
        rep.add("FAIL", "설정 파일", "읽지 못함(%s) — 앱이 빈 설정으로 뜰 수 있습니다" % type(e).__name__)
        return
    hs = d / "hotfix" / "state.json"
    if hs.is_file():
        try:
            h = json.loads(hs.read_text("utf-8"))
            rep.add("OK" if h.get("signed", True) else "WARN", "고침 꾸러미",
                    "v%s · 받은 때 %s · 마지막 확인 %s" % (h.get("version", "?"), h.get("appliedAt", "?"), h.get("lastResult") or h.get("lastCheck", "?")))
        except (OSError, ValueError):
            rep.add("WARN", "고침 꾸러미", "상태 파일을 읽지 못함")
    else:
        rep.add("WARN", "고침 꾸러미", "받은 적 없음")
    for key, label in (("tempDir", "임시 폴더"), ("storageRoot", "저장 뿌리"), ("secondaryPath", "보조 저장"), ("backupPath", "백업")):
        v = c.get(key)
        if not v:
            continue
        p = Path(v)
        where = (" " + paths.short(p)) if show else ""
        if p.exists():
            gb = free_gb(p)
            rep.add("OK" if gb >= 5 else "WARN", label + where, "있음 · 여유 %.0f GB%s" % (gb, "" if gb >= 5 else " — 곧 찹니다"))
        else:
            rep.add("WARN", label + where, "지금 안 보임(디스크가 빠졌거나 NAS 가 끊김)")
    gb = free_gb(d)
    rep.add("OK" if gb >= 2 else "WARN", "시동 디스크", "여유 %.0f GB" % gb)


def main() -> int:
    ap = argparse.ArgumentParser(description="정비 점검 — 빌드본·설치본·자료 폴더(읽기만, 앱을 띄우지 않음)")
    ap.add_argument("--quick", action="store_true", help="서명 검사·도구 실행·모듈 import 를 건너뛴다")
    ap.add_argument("--show-paths", action="store_true", help="자료 경로를 보인다(기본은 가림)")
    ap.add_argument("--no-installed", action="store_true", help="설치본은 보지 않는다")
    a = ap.parse_args()
    rep = Report()
    build = paths.find_build_app()
    if build:
        if (paths.CHERNOBYL / "build" / ".build.lock").exists():
            rep.add("WARN", "빌드본", "빌드가 도는 중(build/.build.lock) — 끝난 뒤 다시 보십시오")
        else:
            check_app(build, rep, a.quick, True)
    else:
        rep.add("WARN", "빌드본", "없음")
    if not a.no_installed:
        apps = paths.find_installed_apps()
        if not apps:
            rep.add("WARN", "설치본", "찾지 못함")
        for app in apps:
            check_app(app, rep, a.quick, False)
    check_data(rep, a.show_paths)
    fails = sum(1 for r in rep.rows if r[0] == "FAIL")
    warns = sum(1 for r in rep.rows if r[0] == "WARN")
    print("\n" + ("통과 — 점검 %d가지%s" % (len(rep.rows), (" · 주의 %d" % warns) if warns else "")
                  if not fails else "문제 %d건 — 주의 %d · 점검 %d가지" % (fails, warns, len(rep.rows))))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
