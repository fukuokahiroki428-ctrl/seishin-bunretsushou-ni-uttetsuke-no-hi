#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""판 내기(맥) — DMG 를 만들고, 태그를 달고, GitHub 에 판을 올린다. 바깥으로 나가는 단계마다 묻는다.

    python3 akashi/release.py --notes notes.md --dry-run   # 계획만
    python3 akashi/release.py --notes notes.md             # 한 단계씩 묻고
    python3 akashi/release.py --notes notes.md --skip-dmg  # 이미 만든 build/*.dmg 로

★ 규칙(지금까지의 판과 같게)
  태그는 mac-<판>-r<번호> — 번호는 그 판의 가장 큰 r 번호 + 1 을 스스로 셈한다. 'v' 로 시작하는 태그는
  만들지 않는다(윈도우 판의 태그와 섞인다). 판 이름은 '<앱 이름> <판> (r<번호>)'. --latest 로 올린다.
★ 왜 이렇게 묻나
  푸시·태그·판은 공개다. 한 번 나가면 받아 간 사람이 있을 수 있다. 사용자의 허락 없이 나가지 않도록
  단계마다 묻고, --dry-run 으로 먼저 보게 한다. 알림 글(--notes)이 없으면 하지 않는다.
★ 확인하는 것
  mac/chernobyl 에 커밋 안 한 변경이 없는지 · 가지가 올라가 있는지(아니면 올릴지 묻는다) ·
  빌드본 서명 · 빌드본 화면(html)이 소스와 같은지(자원만 바꾼 빌드가 옛것을 남긴 일이 있었다).
"""
from __future__ import annotations

import argparse
import glob
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lib import paths  # noqa: E402

CH = paths.CHERNOBYL


def run(cmd, cwd=None, check=False) -> subprocess.CompletedProcess:
    r = subprocess.run(cmd, cwd=str(cwd or paths.REPO), capture_output=True, text=True)
    if check and r.returncode != 0:
        raise SystemExit("실패: %s\n%s" % (" ".join(cmd), (r.stderr or r.stdout).strip()[-600:]))
    return r


def git(*args) -> str:
    return run(["git"] + list(args)).stdout.strip()


def ask(q: str, yes: bool) -> bool:
    if yes:
        print("  " + q + " → 예(--yes)")
        return True
    try:
        return input("  " + q + " (y/N) ").strip().lower() in ("y", "yes", "ㅇ", "예", "네")
    except EOFError:
        return False


def next_tag(version: str) -> str:
    nums = [int(m.group(1)) for t in git("tag", "-l", "mac-%s-r*" % version).split()
            for m in [re.fullmatch(r"mac-%s-r(\d+)" % re.escape(version), t)] if m]
    return "mac-%s-r%d" % (version, (max(nums) if nums else 0) + 1)


def main() -> int:
    ap = argparse.ArgumentParser(description="판 내기(맥) — DMG · 태그 · GitHub 판. 단계마다 묻는다")
    ap.add_argument("--notes", required=True, help="판 알림 글(마크다운 파일)")
    ap.add_argument("--tag", help="태그를 직접 정한다(기본: 다음 r 번호를 셈)")
    ap.add_argument("--skip-dmg", action="store_true", help="make_dmg.sh 를 건너뛰고 build/*.dmg 를 쓴다")
    ap.add_argument("--dry-run", action="store_true", help="계획만 보이고 아무것도 하지 않는다")
    ap.add_argument("--yes", action="store_true", help="묻지 않는다 — 사용자가 이미 허락한 때만")
    a = ap.parse_args()

    notes = Path(a.notes)
    if not notes.is_file() or not notes.read_text("utf-8").strip():
        print("알림 글이 없거나 비었습니다 — 판을 내지 않습니다")
        return 2
    app = paths.find_build_app()
    if not app:
        print("빌드본이 없습니다 — ./build.sh 부터")
        return 2
    info = paths.info_plist(app)
    # ★ 판 번호는 저장소의 VERSION 파일 — make_dmg.sh 와 같다. 앱의 CFBundleShortVersionString 에는
    #   판 번호가 아니라 암호명(上野 등)이 들어 있어, 그것으로 셈하면 'mac-上野-r1' 같은 태그가 나왔다.
    vf = paths.REPO / "VERSION"
    version = vf.read_text("utf-8").strip() if vf.is_file() else str(info.get("CFBundleVersion") or "")
    if not re.fullmatch(r"\d+(\.\d+){1,3}", version):
        print("판 번호를 읽지 못했습니다(VERSION: %r)" % version)
        return 2
    name = str(info.get("CFBundleName") or info.get("CFBundleExecutable") or "Hanishiki")
    tag = a.tag or next_tag(version)
    if tag.startswith("v"):
        print("'v' 로 시작하는 태그는 만들지 않습니다: " + tag)
        return 2
    if git("tag", "-l", tag):
        print("태그가 이미 있습니다: " + tag)
        return 2
    title = "%s %s (%s)" % (name, version, tag.rsplit("-", 1)[-1])
    branch = git("rev-parse", "--abbrev-ref", "HEAD")
    head = git("rev-parse", "--short", "HEAD")

    # ── 확인 ────────────────────────────────────────────────────────────
    problems = []
    dirty = [l for l in git("status", "--porcelain", "--", str(CH)).splitlines()
             if not l.startswith("??") and "/build/" not in l]
    if dirty:
        problems.append("커밋 안 한 변경이 있습니다: " + ", ".join(l[3:] for l in dirty[:5]))
    if (CH / "build" / ".build.lock").exists():
        problems.append("빌드가 아직 도는 중입니다(build/.build.lock)")
    sig = run(["codesign", "--verify", "--deep", "--strict", str(app)])
    if sig.returncode != 0:
        problems.append("빌드본 서명이 틀립니다")
    src_html, app_html = CH / "resources" / "html" / "index.html", app / "Contents" / "Resources" / "html" / "index.html"
    if app_html.exists() and src_html.read_bytes() != app_html.read_bytes():
        problems.append("빌드본 화면이 소스와 다릅니다 — 다시 빌드하십시오")
    run(["git", "fetch", "-q", "origin", branch])
    ahead = git("rev-list", "--count", "origin/%s..HEAD" % branch) or "?"

    print("판 내기 — %s" % title)
    print("  가지 %s @ %s · 올라가지 않은 커밋 %s개" % (branch, head, ahead))
    print("  태그 %s · 알림 글 %s (%d줄)" % (tag, paths.short(notes), len(notes.read_text("utf-8").splitlines())))
    for p in problems:
        print("  ✗ " + p)
    if problems:
        print("멈춤 — 위 문제를 먼저")
        return 1
    steps = []
    if ahead not in ("0", "?"):
        steps.append(("git push origin " + branch, ["git", "push", "origin", branch], True))
    if not a.skip_dmg:
        steps.append(("./make_dmg.sh", ["./make_dmg.sh"], False))
    steps.append(("git tag -a %s -m '%s'" % (tag, title), ["git", "tag", "-a", tag, "-m", title], False))
    steps.append(("git push origin " + tag, ["git", "push", "origin", tag], True))
    steps.append(("gh release create %s <dmg> --title '%s' --notes-file <알림> --latest" % (tag, title), None, True))
    print("  할 일:")
    for s, _, outward in steps:
        print("    %s %s" % ("↗" if outward else "·", s))
    if a.dry_run:
        print("--dry-run — 아무것도 하지 않았습니다 (↗ = 바깥으로 나감)")
        return 0

    for label, cmd, outward in steps:
        if outward and not ask("바깥으로 나갑니다 — %s ?" % label, a.yes):
            print("멈춤 — 여기까지 한 것: 위 단계들")
            return 1
        if cmd is None:   # gh release
            dmgs = sorted(glob.glob(str(CH / "build" / "*.dmg")), key=lambda p: Path(p).stat().st_mtime)
            if not dmgs:
                print("DMG 가 없습니다")
                return 1
            cmd = ["gh", "release", "create", tag, dmgs[-1], "--title", title, "--notes-file", str(notes), "--latest"]
        cwd = CH if cmd[0].startswith("./") else paths.REPO
        print("  … " + label)
        r = run(cmd, cwd=cwd)
        out = (r.stdout or "").strip().splitlines()
        if r.returncode != 0:
            print("  실패(%d): %s" % (r.returncode, (r.stderr or r.stdout).strip()[-400:]))
            return 1
        if out:
            print("    " + out[-1][:200])
    print("통과 — %s 를 냈습니다" % title)
    return 0


if __name__ == "__main__":
    sys.exit(main())
