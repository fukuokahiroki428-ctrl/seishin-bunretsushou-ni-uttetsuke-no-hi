#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""올리기 전 개인 정보 검사 — 공개 저장소에 나가기 전에, 나갈 파일을 훑는다.

    python3 akashi/privacy_scan.py                 # 아직 안 올린 것(origin 과 다른 파일 + 커밋 안 한 것)
    python3 akashi/privacy_scan.py akashi docs/x.md # 이 파일·폴더
    python3 akashi/privacy_scan.py --allow akashi/privacy_allow.txt

★ 왜
  이 저장소는 공개다. 올라간 것은 누구나 읽고, 지워도 기록에 남는다. 한 번 새면 되돌릴 수 없으니
  올리기 '전' 에 본다. 고침 꾸러미 발행기(tools/hotfix_publish.py)가 꾸러미에 하던 검사를 가져와
  (import 해서 같은 규칙을 쓴다) 모든 파일로 넓혔다.

★ 사람 이름을 이 파일에 박지 않는다
  '누구의 이름을 찾을지' 를 글자로 적으면 이 공구 자체가 그 이름을 공개한다. 실행할 때마다 이 맥에서
  뽑는다 — 사용자 이름 · git 이름·메일 앞머리(hotfix_publish.local_identities) · 컴퓨터 이름 ·
  붙어 있는 외장 디스크 이름.

★ 찾은 것은 가려서 보인다
  앞 세 글자와 길이만. 검사 결과를 그대로 기록에 붙여도 비밀이 새지 않게.

찾는 것: 이 맥의 이름들 · 개인 경로(/Users/이름 · C:\\Users) · 메일 · 공인 IP · 쿠키·토큰 '이름=값' ·
비밀 열쇠(PEM) · 비밀처럼 보이는 긴 문자열. 받아들인 것은 --allow 파일에 '경로:분류' 로 적는다.
"""
from __future__ import annotations

import argparse
import importlib.util
import math
import os
import re
import socket
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from lib import paths  # noqa: E402

SKIP_DIRS = {".git", "build", "__pycache__", "node_modules", "third_party", "python_env", "deno", "exiftool",
             "singlefile_extension", "perl"}
TEXT_EXT = {".py", ".md", ".txt", ".json", ".js", ".html", ".css", ".cpp", ".h", ".mm", ".sh", ".command",
            ".bat", ".iss", ".cmake", ".yml", ".yaml", ".plist", ".in", ".qrc", ".rc", ".cfg", ".ini", ".toml", ""}
SECRET_NAMES = r"auth_token|ct0|sessionid|FANBOXSESSID|PHPSESSID|csrftoken|ds_user_id|fb_dtsg|lsd|twid|kdt|sessionToken"
RX_ASSIGN = re.compile(r"(?<![A-Za-z0-9_])(%s)\s*[=:]\s*[\"']?([A-Za-z0-9%%_\-.:/+]{16,})" % SECRET_NAMES, re.I)
RX_BEARER = re.compile(r"Bearer\s+([A-Za-z0-9%_\-=]{30,})")
RX_PEM = re.compile(r"-----BEGIN (?:EC |RSA |OPENSSH |ENCRYPTED |DSA )?PRIVATE KEY-----")
RX_HOME = re.compile(r"/Users/(?!Shared\b)[A-Za-z0-9._-]+|/home/[A-Za-z0-9._-]+|[Cc]:\\\\?Users\\\\?[A-Za-z0-9._-]+")
RX_MAIL = re.compile(r"[A-Za-z0-9._%+-]+@(?!example\.|users\.noreply\.github\.com)[A-Za-z0-9.-]+\.[A-Za-z]{2,}")
RX_IP = re.compile(r"(?<![\d.])(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?![\d.])")
RX_LONG = re.compile(r"[A-Za-z0-9+/_\-=%]{40,}")
RX_EXPR = re.compile(r"^[A-Za-z_$][\w$]*\.[A-Za-z_$]")                         # 코드의 a.b 식
RX_PLACEHOLDER_MAIL = re.compile(r"^(you|your|user|name|me|someone|example|test|foo)@", re.I)   # 자리 표시 글


def load_publisher():
    p = paths.CHERNOBYL / "tools" / "hotfix_publish.py"
    spec = importlib.util.spec_from_file_location("hotfix_publish", str(p))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def identities() -> set:
    ids = set()
    try:
        ids |= set(load_publisher().local_identities())
    except Exception:   # 발행기를 못 읽어도 검사는 한다
        pass
    ids.add(paths.real_home().name)
    host = socket.gethostname().split(".")[0]
    if host and host.lower() not in ("localhost",):
        ids.add(host)
    try:
        for v in os.listdir("/Volumes"):
            if v not in ("Macintosh HD", "Macintosh HD - Data", "Recovery", "Preboot", "VM", "Update") and not v.startswith("com.apple"):
                ids.add(v)
    except OSError:
        pass
    return {i for i in ids if len(i) >= 3 and i.lower() not in ("root", "user", "admin", "mac", "macbook")}


def mask(s: str) -> str:
    return "%s…(%d자)" % (s[:3], len(s))


def entropy(s: str) -> float:
    n = len(s)
    return -sum(c / n * math.log2(c / n) for c in (s.count(ch) for ch in set(s)))


def looks_secret(s: str) -> bool:
    if s.startswith(("http", "__relay_internal__", "data:", "sha256", "/")) or "/" in s[1:] and s.count("/") > 2:
        return False
    if re.fullmatch(r"[0-9a-f]{64}", s) or re.fullmatch(r"[0-9a-f]{40}", s):    # sha256·git 해시
        return False
    if re.fullmatch(r"[A-Za-z_\-]+", s) or re.fullmatch(r"[=\-_]+", s):          # 낱말·줄긋기
        return False
    classes = sum(bool(re.search(p, s)) for p in (r"[a-z]", r"[A-Z]", r"\d"))
    return classes >= 3 and entropy(s) > 4.2


def changed_files() -> list:
    def git(*args):
        r = subprocess.run(["git", "-C", str(paths.REPO)] + list(args), capture_output=True, text=True)
        return [l for l in r.stdout.splitlines() if l.strip()] if r.returncode == 0 else []
    br = (git("rev-parse", "--abbrev-ref", "HEAD") or ["HEAD"])[0]
    files = set(git("diff", "--name-only", "origin/%s...HEAD" % br)) | set(git("diff", "--name-only")) \
        | set(git("diff", "--name-only", "--cached")) | set(git("ls-files", "--others", "--exclude-standard", "--", "mac"))
    return [paths.REPO / f for f in sorted(files) if (paths.REPO / f).is_file()]


def expand(targets) -> list:
    out = []
    for t in targets:
        p = Path(t).resolve()
        if p.is_file():
            out.append(p)
        elif p.is_dir():
            for root, dirs, files in os.walk(p):
                dirs[:] = [d for d in dirs if d not in SKIP_DIRS and not d.startswith(".")]
                out += [Path(root) / f for f in files]
    return out


def rel(p: Path) -> str:
    try:
        return str(p.resolve().relative_to(paths.REPO))
    except ValueError:
        return paths.short(p)


def scan_file(p: Path, me_rx):
    if p.suffix.lower() not in TEXT_EXT or p.stat().st_size > 3_000_000:
        return []
    try:
        text = p.read_text("utf-8")
    except (UnicodeDecodeError, OSError):
        return []
    hits, r = [], rel(p)
    if me_rx and me_rx.search(r):
        hits.append((r, 0, "이 맥의 이름(파일 경로)", mask(me_rx.search(r).group(0))))
    for n, line in enumerate(text.splitlines(), 1):
        if me_rx:
            for m in me_rx.finditer(line):
                hits.append((r, n, "이 맥의 이름", mask(m.group(0))))
        for m in RX_HOME.finditer(line):
            hits.append((r, n, "개인 경로", mask(m.group(0))))
        for m in RX_MAIL.finditer(line):
            if "noreply" not in m.group(0) and not RX_PLACEHOLDER_MAIL.match(m.group(0)):
                hits.append((r, n, "메일", mask(m.group(0))))
        for m in RX_IP.finditer(line):
            o = [int(x) for x in m.groups()]
            if max(o) > 255 or o[0] in (0, 10, 127, 255) or o[:2] == [192, 168] or (o[0] == 172 and 16 <= o[1] <= 31) or o[0] == 169:
                continue
            if re.search(r"(version|ver|v)\s*[:=]?\s*$", line[:m.start()], re.I):
                continue
            hits.append((r, n, "공인 IP", mask(m.group(0))))
        for m in RX_ASSIGN.finditer(line):
            if RX_EXPR.match(m.group(2)):      # document.getElementById… · accounts.x — 코드 식이지 값이 아니다
                continue
            hits.append((r, n, "쿠키·토큰 값", "%s=%s" % (m.group(1), mask(m.group(2)))))
        for m in RX_BEARER.finditer(line):
            hits.append((r, n, "Bearer 값", mask(m.group(1))))
        if RX_PEM.search(line):
            hits.append((r, n, "비밀 열쇠(PEM)", "-----BEGIN … PRIVATE KEY-----"))
        for m in RX_LONG.finditer(line):
            s = m.group(0)
            if looks_secret(s):
                hits.append((r, n, "비밀 같은 긴 글", mask(s)))
    return hits


def load_allow(f) -> set:
    out = set()
    if f and Path(f).is_file():
        for line in Path(f).read_text("utf-8").splitlines():
            line = line.split("#", 1)[0].strip()
            if ":" in line:
                a, b = line.rsplit(":", 1)
                out.add((a.strip(), b.strip()))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description="올리기 전 개인 정보 검사 — 찾은 것은 가려서 보인다")
    ap.add_argument("targets", nargs="*", help="파일·폴더 (없으면 아직 안 올린 파일)")
    ap.add_argument("--allow", default=str(paths.AKASHI / "privacy_allow.txt"),
                    help="받아들인 것 목록 — 한 줄에 '저장소 기준 경로:분류' (기본 akashi/privacy_allow.txt)")
    ap.add_argument("--json", action="store_true")
    a = ap.parse_args()
    files = expand(a.targets) if a.targets else changed_files()
    if not files:
        print("훑을 파일이 없습니다 — 올릴 것이 없거나 경로가 틀렸습니다")
        return 0
    ids = identities()
    me_rx = re.compile(r"(?<![A-Za-z0-9])(%s)(?![A-Za-z0-9])" % "|".join(re.escape(i) for i in sorted(ids, key=len, reverse=True)), re.I) if ids else None
    allow = load_allow(a.allow)
    hits = []
    for f in files:
        hits += [h for h in scan_file(f, me_rx) if (h[0], h[2]) not in allow]
    if a.json:
        import json
        print(json.dumps([{"file": h[0], "line": h[1], "kind": h[2], "masked": h[3]} for h in hits], ensure_ascii=False, indent=1))
    else:
        for f, n, kind, m in hits:
            print("%s:%d  %s  %s" % (f, n, kind, m))
        print(("문제 %d건 — 파일 %d개 중 · 고치거나, 괜찮은 것이면 %s 에 '경로:분류' 로" % (len(hits), len(files), paths.short(a.allow)))
              if hits else "통과 — 파일 %d개에서 개인 정보·비밀을 찾지 못했습니다(이 맥의 이름 %d개 대조)" % (len(files), len(ids)))
    return 1 if hits else 0


if __name__ == "__main__":
    sys.exit(main())
