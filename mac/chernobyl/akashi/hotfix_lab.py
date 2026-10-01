#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""고침 꾸러미 실험실 — 좋은 꾸러미·바꿔치기·서명 없음·낯선 열쇠·판 올림을 격리 사본에 하나씩 먹여,
앱이 약속대로 받거나 버리는지 본다.

    python3 akashi/hotfix_lab.py run                        # 여덟 가지를 차례로 (사본이 없으면 띄우고, 끝나면 끈다)
    python3 akashi/hotfix_lab.py run --only good,unsigned -v
    python3 akashi/hotfix_lab.py build                      # 시험 꾸러미만 만든다(임시 폴더 · 앱 없이 셈한 예상까지)
    python3 akashi/hotfix_lab.py serve good                 # 한 가지를 http://127.0.0.1:8765/hotfix/ 로 (앞에서 · Ctrl-C)
    python3 akashi/hotfix_lab.py verify <꾸러미 폴더>        # 앱이 이 꾸러미를 받을지 앱 없이 대 본다(올리기 전 확인)
    python3 akashi/hotfix_lab.py selftest                   # 앱도 진짜 열쇠도 없이 실험실 자체를 점검
    python3 akashi/hotfix_lab.py clean                      # build 로 남긴 실험 폴더 치우기

시나리오 — 돌리는 차례가 곧 판 번호 차례다(판 = 사본이 지금 쓰는 판 + 차례 번호):
  1 good             좋은 꾸러미(도우미 하나)                → 받음
  2 tamper-manifest  서명한 뒤 manifest 를 고침              → 서명 틀림 · 다른 파일은 받지도 않음 · 앞 판 그대로
  3 tamper-tool      서명한 뒤 도우미를 고침                 → 그 판을 통째로 버림 · 앞 판·도우미 그대로
  4 unsigned         manifest.sig 없음                       → 서명 없음(HTTP 404) · 앞 판 그대로
  5 forged           낯선 열쇠로 서명                        → 서명 틀림 · 앞 판 그대로
  6 minapp           이 앱보다 새 판이 있어야 쓰는 꾸러미      → 받지 않음 · 앞 판 그대로
  7 badname          앱에 없는 이름의 도우미를 섞어 서명      → 그 도우미만 버림(내려받지도 않음) · 나머지는 받음
  8 bumped           판을 올린 좋은 꾸러미                    → 받음 · 도우미도 새것
  run 은 끝에 사본을 한 번 다시 켜서, 앱 기록의 「[고침 꾸러미] vN 을 씀」 줄로 판이 남았는지, 그리고 꺼진 사이
  디스크에서 몰래 고친 도우미를 켤 때 버리는지 본다(이 공구가 띄운 사본일 때만 — 남의 사본은 다시 켜지 않는다).

종료 코드: 0 모두 뜻대로 · 1 어긋난 것 있음(앱이 약속과 다르게 함·꾸러미가 뜻대로 안 만들어짐·되돌리기 못 함)
          2 돌릴 수 없음(앱·포트·도구 없음 · 잘못된 인자 · 열쇠가 없는데 받는 쪽을 골랐음 · Ctrl-C)

★ 왜 진짜 서명 열쇠를 쓰고, 어떻게 안 보나
  앱은 컴파일해 둔 공개 열쇠 하나만 믿는다(src/core/HotfixSig.cpp) — 시험용 열쇠로 바꿔 끼울 길은 일부러 없다.
  그래서 '받는 쪽' 시험은 진짜 열쇠로 서명해야 한다. 서명은 tools/hotfix_publish.py 에 맡긴다: 자식 프로세스에서
  그 파일을 그대로 불러 원본 폴더(SRC)만 시험용으로 바꿔 주고 --out 으로 쓰게 한다. 모양 검사·개인정보 훑기·
  manifest 만들기·서명까지 진짜 올리기와 같은 길을 지난다. 이 공구는 열쇠 파일을 열지 않는다 — 있는지만 본다.
  열쇠가 없으면 진짜 열쇠가 필요 없는 거절 쪽(unsigned·forged)만 돌리고 그렇다고 말한다.
  unsigned·forged 는 그때그때 만들어 곧 지우는 '버릴 열쇠' 로 만든다 — 진짜 열쇠로 서명한 것을 하나라도 덜 만든다.

★ 왜 시험 꾸러미를 남기지 않나
  진짜 열쇠로 서명한 꾸러미는 새어 나가면 '진짜' 다. 누가 hotfix-mac 에 그대로 올리면 앱들은 받는다. 내용은 저장소
  원본에 주석 한 줄이라 해는 작지만, 판 번호가 높으면 그 아래 판의 진짜 고침을 막는다. 그래서 임시 폴더(0700)
  에만 만들고, run·serve 는 끝나면 지우고, 저장소 안에는 만들지 않는다(--out 이 저장소를 가리키면 멈춘다).
  안내 글에 「배포용 아님」 을 적어, 혹시 어디서 받더라도 화면에 그렇게 뜨게 한다. build 로 남긴 것은 clean 으로 지운다.

★ 왜 판 번호를 사본에 맞춰 매기나
  앱은 지금 쓰는 판보다 높은 판만 본다 — 낮으면 도우미를 대 보지도 않고 "최신입니다" 로 끝난다. 거절 시험도
  '받을 만한 판' 이어야 정말 그 까닭으로 버리는지 알 수 있다. 그래서 사본이 쓰는 판을 화면에서 읽고, 그 위로
  시나리오 차례대로 매긴다. 시나리오마다 앱의 규칙(checkHotfix)을 파이썬으로 옮긴 predict() 로 '지금 이 판에서
  앱이 할 일' 을 다시 셈해, 뜻한 결과와 다르면(앞 단계 탓) 그 시나리오는 대 볼 수 없다고 적는다.
  사본의 판은 올라가기만 한다 — 앱이 낮은 판을 받지 않으니 되돌릴 길이 없고, 그것이 맞는 동작이다(격리 사본 안의 일).

★ 왜 badname 은 '받음' 이 맞나
  앱의 약속이 둘로 나뉜다. 앱에 없는 이름의 도우미는 그 하나만 건너뛴다 — 이름만 보고 내려받지도 않는다. 서명이
  맞는 꾸러미의 나머지까지 버릴 까닭은 없다. 반면 sha256 이 어긋난 도우미는 누가 중간에 바꿨다는 뜻이라 판 전체를
  버린다(tamper-tool). 그래서 badname 은 "판은 받고, 그 이름은 디스크·상태·요청 어디에도 없음" 을 대 본다.
  hotfix_publish.py 는 그런 이름을 아예 싣지 않으므로, 싣고 난 뒤 manifest 에 넣고 같은 열쇠로 다시 서명한다.

★ 왜 자동 확인을 끄고 하나
  앱은 켠 지 20초 뒤와 12시간마다 스스로 확인한다. 그것이 우리 확인 사이에 끼면 '어느 확인이 받았는지' 가 흐려진다.
  시작할 때 자동을 끄고(setHotfixAuto) 끝나면 원래 값으로 돌린다. 서버도 사본을 준비한 뒤에야 꾸러미를 내놓는다
  (그 전에는 모든 요청에 404 — 끼어든 확인은 "올라와 있지 않습니다" 로 끝나 아무것도 바꾸지 않는다).

★ 왜 서버를 이 프로세스 안에 두나
  시나리오를 바꿀 때 서버를 껐다 켜지 않고 내놓는 폴더만 바꾼다. 앱이 무엇을 받아 갔는지(경로·응답 번호)를 그대로
  알 수 있어서 "서명이 틀리면 다른 파일은 받지도 않는다" 같은 약속까지 대 본다. 127.0.0.1 에만 열고, /hotfix/ 밑의
  파일만, 목록 없이, 캐시 없이(If-Modified-Since 무시 · no-store) 내놓는다. 포트가 차 있으면 멈춘다 — 남의
  프로세스를 끄지 않는다.

★ 왜 앱이 한 일을 세 군데서 보나
  화면(onHotfixStatus 로 오는 상태 — 갈고리를 걸어 '몇 번째 알림' 인지 센다), 설정 로그 줄, 그리고 사본 자료 폴더의
  hotfix/state.json·tools/ (읽기만). 화면만 보면 앱이 저장을 빠뜨려도 모르고, 디스크만 보면 화면에 거짓이 떠도 모른다.
  시간 대신 알림 횟수로 기다리므로, 같은 거절이 두 번 연달아 와도 헷갈리지 않는다.

★ 무엇을 되돌리나
  자동 확인 켬/끔, 화면에 건 갈고리, 로그 줄에 단 표식, 받은 파이썬 도우미(앱에 든 것으로 — resetHotfixTools).
  우리가 띄운 사본은 끈다(--keep-app 이면 둔다). 시험 꾸러미는 지운다(--keep-files 면 둔다). 판 번호만은 못 되돌린다.
  사용자의 앱·자료 폴더에는 붙지도 읽지도 않는다 — 격리 사본의 기록(iso.py state.json)에 있는 PID 가 그 포트를 듣고
  있을 때만 붙고, 자료는 그 사본의 홈 안에서만 본다.
"""
from __future__ import annotations

import argparse
import base64
import hashlib
import http.server
import importlib.util
import json
import mmap
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

# hotfix_publish.py 를 불러오므로 저장소 tools/ 에 __pycache__ 가 생기지 않게 한다(커밋될 찌꺼기)
sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import iso  # noqa: E402  격리 사본 기록(state.json)을 iso.py 와 같은 규칙으로 읽는다
from lib import cdp as cdplib  # noqa: E402
from lib import paths, proc  # noqa: E402
from lib.cdp import CdpError, Page  # noqa: E402
from lib.ws import WsError  # noqa: E402

HERE = Path(__file__).resolve().parent
ISO_PY = HERE / "iso.py"
PUBLISHER = paths.CHERNOBYL / "tools" / "hotfix_publish.py"
SIG_SRC = paths.CHERNOBYL / "src" / "core" / "HotfixSig.cpp"
BACKEND_SRC = paths.CHERNOBYL / "src" / "core" / "HanishikiBackend.cpp"
OPENSSL = "/usr/bin/openssl" if os.path.exists("/usr/bin/openssl") else (shutil.which("openssl") or "openssl")

MARK = ".akashi-hotfix-lab"        # 이 표식이 있는 폴더만 지운다 — 엉뚱한 폴더를 지울 길을 막는다
PREFIX = "akashi-hotfix-lab-"
DEFAULT_HTTP = 8765
PREFER_TOOL = "ig_docids.py"       # 이 세션에서 실제로 갈아 끼워 본 도우미 — 없으면 가장 작은 것
BAD_STEM = "akashi_lab_probe"      # 앱에 없는 이름(이름 규칙은 맞게 — '모양' 이 아니라 '없음' 으로 버려져야 한다)

# ── 앱(HanishikiBackend::checkHotfix · HotfixSig::verify)과 같은 수 — 바뀌면 여기도 ─────────
MANIFEST_MAX = 64 * 1024
FILE_MAX = 256 * 1024
TOOL_MAX = 512 * 1024
SIG_MAX = 256
TOOL_NAME = re.compile(r"^[a-z0-9_]{2,40}\.py$")
FILES = ("api_overrides.json", "shape_aliases.json", "repair_rules.json")
# P-256 공개 열쇠의 SubjectPublicKeyInfo 머리 — 뒤에 04||X||Y 65바이트를 붙이면 openssl 이 읽는 DER 이 된다
SPKI_P256 = bytes.fromhex("3059301306072a8648ce3d020106082a8648ce3d030107034200")

# 앱이 쓰는 글(HanishikiBackend.cpp). selftest 가 소스에 그대로 있는지 대 본다 — 글이 바뀌면 판정이 어긋나므로.
APP_PHRASES = (
    "고침 꾸러미의 서명이 맞지 않아 쓰지 않습니다 (서명 %1)",
    'QStringLiteral("틀림")',
    "없음 · HTTP %1",
    "고침 꾸러미 v%1 은 새 판(%2 이상)이 있어야 씁니다",
    "고침 꾸러미가 최신입니다 (v%1)",
    "고침 꾸러미 v%1 — %2 을(를) 받지 못했거나 내용이 맞지 않습니다",
    "고침 꾸러미 v%1 — 도우미 %2 의 내용이 맞지 않습니다",
    "고침 꾸러미 v%1 받음(서명 맞음)",
    "앱에 없는 도우미라 받지 않은 것: %1",
    "v%1 을 쓰는 중",
    "[고침 꾸러미] v%1 을 씀 — 값 %2 · 별명 %3 · 수리 규칙 %4 · 파이썬 도우미 %5",
)


class Fatal(Exception):
    """돌릴 수 없음(종료 코드 2)."""


class Scn:
    def __init__(self, sid: str, title: str, key: str, expect: str, promise: str):
        self.sid, self.title, self.key, self.expect, self.promise = sid, title, key, expect, promise


# key: real = 진짜 열쇠로 서명(열쇠가 있어야 함) · throwaway = 버릴 열쇠로(열쇠 없어도 됨)
SCENARIOS = [
    Scn("good", "좋은 꾸러미", "real", "applied", "받음"),
    Scn("tamper-manifest", "서명한 뒤 manifest 를 고침", "real", "sig-bad",
        "서명 틀림 · 다른 파일은 받지도 않음 · 앞 판 그대로"),
    Scn("tamper-tool", "서명한 뒤 도우미를 고침", "real", "tool-bad", "그 판을 통째로 버림 · 앞 판·도우미 그대로"),
    Scn("unsigned", "manifest.sig 없음", "throwaway", "sig-missing", "서명 없음(HTTP 404) · 앞 판 그대로"),
    Scn("forged", "낯선 열쇠로 서명", "throwaway", "sig-bad", "서명 틀림 · 앞 판 그대로"),
    Scn("minapp", "새 판이 있어야 쓰는 꾸러미", "real", "minapp", "받지 않음 · 앞 판 그대로"),
    Scn("badname", "앱에 없는 이름의 도우미를 섞음", "real", "applied",
        "그 도우미만 버림(내려받지도 않음) · 나머지는 받음"),
    Scn("bumped", "판을 올린 좋은 꾸러미", "real", "applied", "받음 · 도우미도 새것"),
]
BY_ID = {s.sid: s for s in SCENARIOS}
CODE_KO = {
    "applied": "받음", "sig-bad": "서명 틀림", "sig-missing": "서명 없음", "minapp": "새 판이 있어야 씀",
    "latest": "최신이라 건너뜀", "file-bad": "파일이 어긋나 판을 버림", "tool-bad": "도우미가 어긋나 판을 버림",
    "no-manifest": "manifest 를 못 받음",
}


# ── 작은 도구 ────────────────────────────────────────────────────────────────
def sha256_bytes(b: bytes) -> str:
    return hashlib.sha256(b).hexdigest()


def sha256_file(p: Path) -> str:
    h = hashlib.sha256()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def read_json(p: Path) -> dict:
    try:
        v = json.loads(Path(p).read_text("utf-8"))
        return v if isinstance(v, dict) else {}
    except (OSError, ValueError):
        return {}


def dump_manifest(p: Path, man: dict) -> None:
    # hotfix_publish.py 와 같은 모양으로 쓴다(들여쓰기 2 · 한글 그대로 · 끝 줄바꿈)
    p.write_text(json.dumps(man, ensure_ascii=False, indent=2) + "\n", "utf-8")


def scrub(text: str) -> str:
    """자식 프로세스의 글을 사람에게 보일 때 — 진짜 홈은 ~ 로, 열쇠 머리가 보이는 줄은 통째로 뺀다."""
    home = str(paths.real_home())
    out = []
    for line in str(text).replace(home, "~").splitlines():
        if "PRIVATE KEY" in line:
            continue
        out.append(line.rstrip())
    return "\n".join(out)


def tail(text: str, n: int = 4) -> str:
    ls = [l for l in scrub(text).splitlines() if l.strip()]
    return " / ".join(ls[-n:]) if ls else "(글 없음)"


def qt_int(v) -> int:
    """QJsonValue::toInt(0) 흉내 — 정수로 떨어지는 수만 받고 나머지(글·참거짓·없음)는 0."""
    if isinstance(v, bool) or v is None:
        return 0
    if isinstance(v, int):
        return v
    if isinstance(v, float) and v.is_integer():
        return int(v)
    return 0


def qver(s) -> tuple:
    """QVersionNumber::fromString — 앞쪽의 점으로 이은 숫자만."""
    m = re.match(r"(\d+(?:\.\d+)*)", str(s or ""))
    return tuple(int(x) for x in m.group(1).split(".")) if m else ()


def qver_cmp(a: tuple, b: tuple) -> int:
    """QVersionNumber::compare — 겹치는 자리가 같으면 자리가 많은 쪽이 크다(4.0 < 4.0.0)."""
    for x, y in zip(a, b):
        if x != y:
            return -1 if x < y else 1
    return (len(a) > len(b)) - (len(a) < len(b))


def names(d: dict) -> str:
    return ", ".join("%s(%s)" % (k, str(v)[:8]) for k, v in sorted(d.items())) or "없음"


# ── hotfix_publish.py 잇기 ──────────────────────────────────────────────────────
_HP = None


def publisher():
    """상수(KEY 경로·FILES·BUNDLED_TOOLS)와 검사 함수만 쓰려고 이 프로세스에 불러온다. 서명은 여기서 하지 않는다."""
    global _HP
    if _HP is None:
        if not PUBLISHER.is_file():
            raise Fatal("tools/hotfix_publish.py 가 없습니다: " + paths.short(PUBLISHER))
        try:
            spec = importlib.util.spec_from_file_location("akashi_hotfix_publish", str(PUBLISHER))
            m = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(m)
        except Exception as e:  # noqa: BLE001 — 무엇이 깨졌든 '돌릴 수 없음' 이다
            raise Fatal("hotfix_publish.py 를 불러오지 못했습니다 (%s)" % type(e).__name__)
        _HP = m
    return _HP


# 자식 프로세스에서 hotfix_publish.py 를 불러, 원본 폴더(SRC)와 — selftest 에서만 — 열쇠 경로(KEY)를 바꿔 준다.
#   ★ 왜 자식에서: 그 공구는 실패하면 sys.exit 하고 print 한다. 이 실험실의 서버 스레드·되돌리기 순서를 흔들지
#     않게 떼어 둔다. 열쇠 파일은 그 안에서 openssl 이 열 뿐, 이 프로세스로는 서명 바이트만 돌아온다.
_BOOT = r"""
import importlib.util, sys
sys.dont_write_bytecode = True
mode, pub, a1, a2, key = sys.argv[1:6]
spec = importlib.util.spec_from_file_location("hotfix_publish", pub)
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
if key:
    m.KEY = key
if mode == "publish":
    m.SRC = a1
    sys.argv = ["hotfix_publish.py", "--out", a2]
    m.main()
elif mode == "sign":
    sig = m.sign(a1)
    with open(a2, "wb") as f:
        f.write(sig)
else:
    sys.exit(3)
"""


def run_publisher(mode: str, a1, a2, key=None, cwd=None):
    env = dict(os.environ, PYTHONDONTWRITEBYTECODE="1")
    try:
        r = subprocess.run([sys.executable, "-c", _BOOT, mode, str(PUBLISHER), str(a1), str(a2), str(key or "")],
                           capture_output=True, text=True, timeout=180, env=env, cwd=str(cwd) if cwd else None)
    except (OSError, subprocess.SubprocessError) as e:
        return False, "hotfix_publish.py 를 돌리지 못했습니다 (%s)" % type(e).__name__
    return r.returncode == 0, scrub((r.stdout or "") + "\n" + (r.stderr or ""))


# ── 서명 확인(앱 없이) ───────────────────────────────────────────────────────────
def app_pub_hex():
    """앱에 컴파일되는 공개 열쇠(04||X||Y). 공개 값이다."""
    try:
        src = SIG_SRC.read_text("utf-8")
    except OSError:
        return None
    m = re.search(r'kPublicKeyHex\[\]\s*=\s*"(04[0-9a-fA-F]{128})"', src)
    return m.group(1) if m else None


def write_pub_pem(hexpt: str, path: Path) -> Path:
    pt = bytes.fromhex(hexpt)
    if len(pt) != 65 or pt[0] != 4:
        raise Fatal("공개 열쇠 모양이 아닙니다(65바이트 04||X||Y 이어야)")
    b64 = base64.b64encode(SPKI_P256 + pt).decode("ascii")
    body = "\n".join(b64[i:i + 64] for i in range(0, len(b64), 64))
    path.write_text("-----BEGIN PUBLIC KEY-----\n" + body + "\n-----END PUBLIC KEY-----\n", "ascii")
    return path


def sig_ok(pem: Path, manifest: Path, sig: Path) -> bool:
    try:
        r = subprocess.run([OPENSSL, "dgst", "-sha256", "-verify", str(pem), "-signature", str(sig), str(manifest)],
                           capture_output=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return False
    return r.returncode == 0


def throwaway_key(d: Path):
    """버릴 열쇠 — 낯선 서명(forged)·서명 없음(unsigned)·selftest 용. (열쇠 경로, 공개 hex)."""
    k = d / ("throwaway_%s.pem" % os.urandom(4).hex())
    old = os.umask(0o077)
    try:
        subprocess.run([OPENSSL, "ecparam", "-genkey", "-name", "prime256v1", "-noout", "-out", str(k)],
                       check=True, capture_output=True, timeout=30)
        der = subprocess.run([OPENSSL, "ec", "-in", str(k), "-pubout", "-outform", "DER"],
                             check=True, capture_output=True, timeout=30).stdout
    except (OSError, subprocess.SubprocessError):
        raise Fatal("openssl 로 버릴 열쇠를 만들지 못했습니다 (%s)" % OPENSSL)
    finally:
        os.umask(old)
    return k, der[-65:].hex()


def app_has_key(app, hexpt: str):
    """구운 앱 안에 소스의 공개 열쇠 글자가 그대로 있는가. 모르면 None."""
    if not app or not hexpt:
        return None
    try:
        with open(paths.exe_of(app), "rb") as f, mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ) as mm:
            return mm.find(hexpt.encode("ascii")) >= 0 or mm.find(hexpt.lower().encode("ascii")) >= 0
    except (OSError, ValueError):
        return None


def app_version(app) -> str:
    """앱이 minApp 과 견주는 판(PREDORMITION_VERSION_STR = CFBundleVersion — 짧은 판 글은 이름(上野)이다)."""
    v = str(paths.info_plist(app).get("CFBundleVersion", "")) if app else ""
    if not re.match(r"^\d+(\.\d+)*$", v):
        try:
            v = (paths.REPO / "VERSION").read_text("utf-8").strip()
        except OSError:
            v = "0"
    return v


def app_tools_dir(app):
    return Path(app) / "Contents" / "Resources" / "tools" if app else None


def tool_checker(app):
    """앱의 bundledToolExists — 이름 규칙 + 앱에 든 tools/ 나 tools/archive/ 에 있음. 앱을 모르면 저장소 원본으로."""
    tdir = app_tools_dir(app)
    hp = publisher()

    def ok(name: str) -> bool:
        if not TOOL_NAME.match(name):
            return False
        if tdir is not None and tdir.is_dir():
            return (tdir / name).is_file() or (tdir / "archive" / name).is_file()
        return bool(hp.allowed_tool(name))
    return ok


def pick_tool(tool_ok):
    hp = publisher()
    src = Path(hp.BUNDLED_TOOLS)
    try:
        cands = sorted((p for p in src.iterdir() if p.suffix == ".py" and p.is_file()), key=lambda p: p.stat().st_size)
    except OSError:
        return None
    order = [src / PREFER_TOOL] + [p for p in cands if p.name != PREFER_TOOL]
    for p in order:
        if p.is_file() and tool_ok(p.name) and hp.allowed_tool(p.name) and p.stat().st_size < TOOL_MAX - 4096:
            return p.name
    return None


def pick_bad(tool_ok):
    hp = publisher()
    for i in range(100):
        n = "%s%s.py" % (BAD_STEM, "" if i == 0 else "_%d" % i)
        if TOOL_NAME.match(n) and not tool_ok(n) and not hp.allowed_tool(n):
            return n
    raise Fatal("앱에 없는 도우미 이름을 고르지 못했습니다")


def phrase_drift() -> list:
    """앱 소스에서 사라진 글 — 있으면 판정 글을 고쳐야 한다."""
    try:
        src = BACKEND_SRC.read_text("utf-8")
    except OSError:
        return ["(HanishikiBackend.cpp 를 읽지 못함)"]
    return [p for p in APP_PHRASES if p not in src]


# ── 앱이 할 일 셈하기(checkHotfix 를 파이썬으로) ───────────────────────────────────
def predict(b: Path, pem: Path, have: int, app_ver: str, tool_ok) -> dict:
    """꾸러미 폴더 b 를 앱이 받으면 무엇을 하나. code 는 CODE_KO 의 열쇠 중 하나."""
    r = {"code": "", "version": 0, "need": "", "name": "", "accepted": {}, "rejected": [], "have": have}
    mp, sp = b / "manifest.json", b / "manifest.sig"
    try:
        raw = mp.read_bytes()
    except OSError:
        r["code"] = "no-manifest"
        return r
    if len(raw) > MANIFEST_MAX:
        r["code"] = "no-manifest"
        return r
    if not sp.is_file():
        r["code"] = "sig-missing"
        return r
    sig = sp.read_bytes()
    if not raw or not sig or len(sig) > SIG_MAX or not sig_ok(pem, mp, sp):
        r["code"] = "sig-bad"
        return r
    try:
        man = json.loads(raw.decode("utf-8"))
    except (ValueError, UnicodeDecodeError):
        man = {}
    man = man if isinstance(man, dict) else {}
    ver = qt_int(man.get("version"))
    need = man.get("minApp") if isinstance(man.get("minApp"), str) else "0"
    r["version"], r["need"] = ver, need
    if qver_cmp(qver(need), qver(app_ver)) > 0:
        r["code"] = "minapp"
        return r
    if ver <= have:
        r["code"] = "latest"
        return r
    files = man.get("files") if isinstance(man.get("files"), dict) else {}
    for name in FILES:
        meta = files.get(name)
        if not isinstance(meta, dict) or not meta:
            continue                                   # 이 판엔 없는 파일
        try:
            data = (b / name).read_bytes()
        except OSError:
            data = None
        if data is None or len(data) > FILE_MAX or sha256_bytes(data) != str(meta.get("sha256", "")).lower():
            r["code"], r["name"] = "file-bad", name
            return r
    tools = man.get("tools") if isinstance(man.get("tools"), dict) else {}
    for name in sorted(tools):                         # QJsonObject 는 열쇠 차례로 돈다
        if not tool_ok(name):
            r["rejected"].append(name)
            continue
        meta = tools[name] if isinstance(tools[name], dict) else {}
        try:
            data = (b / "tools" / name).read_bytes()
        except OSError:
            data = None
        if data is None or len(data) > TOOL_MAX or sha256_bytes(data) != str(meta.get("sha256", "")).lower():
            r["code"], r["name"], r["accepted"] = "tool-bad", name, {}
            return r
        r["accepted"][name] = sha256_bytes(data)
    r["code"] = "applied"
    return r


def result_fragments(pred: dict) -> list:
    """앱 상태칸(lastResult)에 모두 들어 있어야 할 조각."""
    c, v = pred["code"], pred["version"]
    return {
        "applied": ["v%d 을 쓰는 중" % v],
        "sig-bad": ["서명이 맞지 않아", "서명 틀림"],
        "sig-missing": ["서명이 맞지 않아", "서명 없음", "HTTP 404"],
        "minapp": ["v%d 은 새 판(" % v, "이상)이 있어야"],
        "latest": ["최신입니다 (v%d)" % pred["have"]],
        "file-bad": ["v%d — %s 을(를) 받지 못했거나" % (v, pred["name"])],
        "tool-bad": ["v%d — 도우미 %s 의 내용이 맞지 않습니다" % (v, pred["name"])],
        "no-manifest": ["고침 꾸러미"],
    }.get(c, ["?"])


def log_fragments(pred: dict) -> list:
    """설정 로그에 있어야 할 줄들(줄마다 조각 목록)."""
    if pred["code"] == "applied":
        out = [["고침 꾸러미 v%d 받음(서명 맞음)" % pred["version"]]]
        for b in pred["rejected"]:
            out.append(["앱에 없는 도우미라 받지 않은 것", b])
        return out
    if pred["code"] == "no-manifest":
        return []
    return [result_fragments(pred)]


# ── 실험 폴더 ─────────────────────────────────────────────────────────────────
def new_lab(out=None) -> Path:
    if out:
        p = Path(out).expanduser().resolve()
        repo = paths.REPO.resolve()
        if p == repo or repo in p.parents:
            raise Fatal("저장소 안에는 만들지 않습니다 — 진짜 열쇠로 서명한 시험 꾸러미가 커밋될 수 있습니다: " + paths.short(p))
        real = paths.data_dir().resolve()
        if p == real or real in p.parents:
            raise Fatal("사용자의 자료 폴더 안에는 만들지 않습니다: " + paths.short(p))
        if p.exists() and not p.is_dir():      # ★ 파일을 주면 iterdir 가 NotADirectoryError 로 넘어져 종료 1(문제)로 보였다
            raise Fatal("폴더가 아니라 파일입니다: " + paths.short(p))
        if p.exists() and not (p / MARK).is_file() and any(p.iterdir()):
            raise Fatal("비어 있지 않고 실험실 표식도 없는 폴더라 쓰지 않습니다: " + paths.short(p))
        p.mkdir(parents=True, exist_ok=True)
        os.chmod(p, 0o700)
    else:
        p = Path(tempfile.mkdtemp(prefix=PREFIX)).resolve()     # 0700
    (p / MARK).write_text("akashi hotfix_lab — 시험 꾸러미(진짜 열쇠로 서명됐을 수 있음 · 배포 금지). "
                          "hotfix_lab.py clean 으로 지운다\n", "utf-8")
    return p


def remove_lab(p) -> bool:
    if p and Path(p, MARK).is_file():
        shutil.rmtree(p, ignore_errors=True)
        return not Path(p).exists()
    return False


def key_material_in(p: Path, remove: bool = False) -> list:
    """폴더 안에 비밀 열쇠 글이 섞여 들었는가(있으면 안 된다). 파일 이름만 돌려준다.
    remove=True 면 걸린 파일을 그 자리에서 지운다(실험 폴더 안의 것만 — 부르는 쪽이 표식 폴더를 준다)."""
    hits = []
    for f in list(Path(p).rglob("*")):
        try:
            if f.is_file() and f.stat().st_size < 4 * 1024 * 1024 and b"PRIVATE KEY" in f.read_bytes():
                hits.append(f.name)
                if remove:
                    f.unlink()
        except OSError:
            pass
    return hits


class Ctx:
    """꾸러미를 만들고 셈하는 데 드는 것 — 앱 판·도우미 이름·공개 열쇠."""

    def __init__(self, app):
        if not (os.path.exists(OPENSSL) or shutil.which(OPENSSL)):
            raise Fatal("openssl 이 없습니다 — 서명을 만들고 대 볼 수 없습니다")
        self.hp = publisher()
        self.key_ok = Path(self.hp.KEY).is_file()        # 있는지만 본다 — 열지 않는다
        self.pub_hex = app_pub_hex()
        if not self.pub_hex:
            raise Fatal("앱의 공개 열쇠를 찾지 못했습니다: " + paths.short(SIG_SRC))
        self.app = app
        self.app_ver = app_version(app)
        self.tool_ok = tool_checker(app)
        self.tool = pick_tool(self.tool_ok)
        if not self.tool:
            raise Fatal("갈아 끼워 볼 파이썬 도우미를 찾지 못했습니다(앱과 저장소 resources/tools 에 함께 있는 것)")
        self.bad = pick_bad(self.tool_ok)
        self.app_key = app_has_key(app, self.pub_hex)


def app_for(st: dict):
    if st and iso.running(st) and st.get("app") and paths.is_our_app(st["app"]):
        return Path(st["app"])
    return paths.find_build_app()


def sandbox_hotfix_dir(st: dict):
    """격리 사본의 고침 꾸러미 폴더. 진짜 자료 폴더를 가리키면 None(보지 않는다)."""
    home = (st or {}).get("home")
    if not home:
        return None
    d = paths.data_dir(Path(home)) / "hotfix"
    if d.resolve() == (paths.data_dir() / "hotfix").resolve() or Path(home).resolve() == paths.real_home():
        return None
    return d


def parse_only(s) -> list:
    if not s:
        return [x.sid for x in SCENARIOS]
    want = [w.strip() for w in s.split(",") if w.strip()]
    unknown = [w for w in want if w not in BY_ID]
    if unknown:
        raise Fatal("모르는 시나리오: %s (있는 것: %s)" % (", ".join(unknown), ", ".join(BY_ID)))
    return [x.sid for x in SCENARIOS if x.sid in want]


def version_of(sid: str, base: int) -> int:
    # 차례 번호로 판을 매긴다 — 일부만 골라도 같은 시나리오는 같은 판(기준 위), 돌리는 차례대로 커진다
    return base + [s.sid for s in SCENARIOS].index(sid) + 1


# ── 꾸러미 만들기 ─────────────────────────────────────────────────────────────
def make_src(src: Path, s: Scn, v: int, ctx: Ctx, tool_bytes: bytes) -> None:
    """hotfix_publish.py 가 읽을 원본 폴더 — 저장소 hotfix/ 를 베끼고 판·안내·도우미만 바꾼다."""
    hp = ctx.hp
    root = Path(hp.SRC)
    src.mkdir(parents=True)
    for n in list(hp.FILES) + ["README.md"]:
        shutil.copyfile(root / n, src / n)
    # minApp 은 앱 판과 '같게' — 같으면 받는다는 경계까지 함께 대 본다. minapp 시나리오만 먼 미래 판.
    meta = {"version": v, "minApp": "999.0" if s.sid == "minapp" else ctx.app_ver,
            "notes": "아카시 실험실 시험 꾸러미(%s) — 배포용 아님" % s.sid}
    (src / "hotfix.json").write_text(json.dumps(meta, ensure_ascii=False, indent=2) + "\n", "utf-8")
    (src / "tools").mkdir()
    # 주석 한 줄만 덧붙인다 — 사본이 이 도우미를 돌려도 원본과 똑같이 움직인다. 시나리오마다 sha 가 달라진다.
    mark = "\n# 아카시 실험실 시험 표시 — %s v%d · 이 줄 말고는 앱에 든 원본과 같다\n" % (s.sid, v)
    (src / "tools" / ctx.tool).write_bytes(tool_bytes + mark.encode("utf-8"))


def post_edit(s: Scn, b: Path, ctx: Ctx, trusted_key, cwd):
    """서명한 뒤에 손대기. 실패하면 까닭 글."""
    mp = b / "manifest.json"
    if s.sid == "tamper-manifest":
        man = read_json(mp)
        man["notes"] = str(man.get("notes", "")) + " · 바꿔치기"     # 한 글자만 달라도 서명은 틀려야 한다
        dump_manifest(mp, man)
    elif s.sid == "tamper-tool":
        with open(b / "tools" / ctx.tool, "ab") as f:
            f.write("import os  # 아카시 실험실 — 서명한 뒤에 몰래 끼운 줄\n".encode("utf-8"))
    elif s.sid == "unsigned":
        (b / "manifest.sig").unlink()
    elif s.sid == "badname":
        data = "# 아카시 실험실 — 앱에 없는 이름의 도우미. 앱은 이것을 내려받지도 말아야 한다.\n".encode("utf-8")
        (b / "tools" / ctx.bad).write_bytes(data)
        man = read_json(mp)
        tools = man.get("tools") if isinstance(man.get("tools"), dict) else {}
        tools[ctx.bad] = {"sha256": sha256_bytes(data), "size": len(data)}
        man["tools"] = tools
        dump_manifest(mp, man)
        # 같은 열쇠로 다시 서명 — hotfix_publish.py 의 sign() 으로(열쇠가 저장소 안이면 그쪽이 멈춘다)
        ok, msg = run_publisher("sign", mp, b / "manifest.sig", trusted_key, cwd=cwd)
        if not ok:
            return "다시 서명하지 못함 — " + tail(msg)
    return None


def build_lab(lab: Path, ids: list, base: int, ctx: Ctx, pem: Path, trusted_key=None, say=None):
    """lab/<id>/hotfix 에 시나리오 꾸러미를 만든다. (기록 목록, 문제 목록).
    trusted_key 가 None 이면 진짜 열쇠(hotfix_publish 의 KEY) — selftest 만 버릴 열쇠를 준다."""
    say = say or (lambda line: None)
    work = lab / "_work"
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(mode=0o700)
    recs, problems = [], []
    try:
        foreign, _ = throwaway_key(work)
        tool_bytes = (Path(ctx.hp.BUNDLED_TOOLS) / ctx.tool).read_bytes()
        if not tool_bytes.endswith(b"\n"):
            tool_bytes += b"\n"
        have = base
        for sid in ids:
            s = BY_ID[sid]
            v = version_of(sid, base)
            rec = {"id": sid, "title": s.title, "version": v, "expect": s.expect, "promise": s.promise,
                   "key": s.key, "tool": ctx.tool, "bad": ctx.bad if sid == "badname" else "", "dir": "", "why": []}
            recs.append(rec)
            src = work / "src" / sid
            make_src(src, s, v, ctx, tool_bytes)
            out = lab / sid
            shutil.rmtree(out, ignore_errors=True)
            key = str(foreign) if s.key == "throwaway" else trusted_key
            ok, msg = run_publisher("publish", src, out, key, cwd=work)
            b = out / "hotfix"
            if not ok or not (b / "manifest.json").is_file():
                rec["why"].append("hotfix_publish.py 가 멈춤 — " + tail(msg))
                problems.append("%s: hotfix_publish.py 가 멈춤 — %s" % (sid, tail(msg)))
                continue
            err = post_edit(s, b, ctx, trusted_key, work)
            if err:
                rec["why"].append(err)
                problems.append("%s: %s" % (sid, err))
                continue
            rec["dir"] = str(out)
            pred = predict(b, pem, have, ctx.app_ver, ctx.tool_ok)
            rec["pred"] = pred
            # 뜻대로 만들어졌나 — 뜻한 까닭으로 버려지는 꾸러미여야 시험이 된다
            if pred["code"] != s.expect:
                if s.key == "real" and pred["code"] == "sig-bad" and s.expect != "sig-bad":
                    rec["why"].append("진짜 열쇠의 서명이 앱의 공개 열쇠(HotfixSig.cpp)와 맞지 않음 — 열쇠를 바꿨다면 "
                                      "공개 열쇠도 바꿔 새 판을 구워야 합니다")
                else:
                    rec["why"].append("앱이 할 일이 '%s' 로 셈됨(뜻: %s)" % (CODE_KO.get(pred["code"], pred["code"]),
                                                                  CODE_KO[s.expect]))
            if s.expect == "applied" and ctx.tool not in pred["accepted"]:
                rec["why"].append("도우미 %s 가 받을 것에 없음" % ctx.tool)
            if sid == "badname" and pred["rejected"] != [ctx.bad]:
                rec["why"].append("버려질 이름이 %s (뜻: %s)" % (pred["rejected"] or "없음", ctx.bad))
            if sid == "tamper-tool" and pred["name"] != ctx.tool:
                rec["why"].append("어긋난 도우미가 %s (뜻: %s)" % (pred["name"] or "없음", ctx.tool))
            for w in rec["why"]:
                problems.append("%s: %s" % (sid, w))
            if pred["code"] == "applied":
                have = pred["version"]
            say(rec)
    finally:
        shutil.rmtree(work, ignore_errors=True)       # 버릴 열쇠·원본 사본 — 남기지 않는다
    # ★ '지웁니다' 라고 적고 남겨 두면 build(--out)·--keep-files 는 실험 폴더를 지우지 않아 열쇠 글이 디스크에 남는다
    leaked = key_material_in(lab, remove=True)
    if leaked:
        problems.append("시험 폴더에 비밀 열쇠 글이 섞여 들었습니다(파일: %s) — 지웁니다" % ", ".join(leaked))
    info = {"tool": "hotfix_lab", "base": base, "app_version": ctx.app_ver, "helper": ctx.tool, "bad": ctx.bad,
            "scenarios": [{k: r[k] for k in ("id", "version", "expect", "key")} for r in recs]}
    (lab / "lab.json").write_text(json.dumps(info, ensure_ascii=False, indent=1), "utf-8")
    return recs, problems


# ── 서버 ─────────────────────────────────────────────────────────────────────
class LabHandler(http.server.SimpleHTTPRequestHandler):
    server_version = "akashi-hotfix-lab"
    sys_version = ""

    def __init__(self, request, client_address, server):
        super().__init__(request, client_address, server, directory=server.root_str())

    def _path(self) -> str:
        return urllib.parse.unquote(urllib.parse.urlsplit(self.path).path)

    def _gate(self) -> bool:
        p = self._path()
        # /hotfix/ 밑의 파일만. 점으로 시작하는 조각(.., 표식 파일)·역슬래시·NUL 은 모두 404.
        if (self.server.root is None or not p.startswith("/hotfix/") or p.endswith("/")
                or "/." in p or "\\" in p or "\x00" in p):
            self.send_error(404)
            return False
        # 304 로 답하면 앱은 '못 받음' 으로 읽는다 — 시나리오마다 새 내용이니 늘 통째로 준다
        for h in ("If-Modified-Since", "If-None-Match"):
            if h in self.headers:
                del self.headers[h]
        return True

    def do_GET(self):
        if self._gate():
            super().do_GET()

    def do_HEAD(self):
        if self._gate():
            super().do_HEAD()

    def list_directory(self, path):
        self.send_error(404)
        return None

    def end_headers(self):
        self.send_header("Cache-Control", "no-store")
        super().end_headers()

    def log_request(self, code="-", size="-"):
        try:
            c = int(code)
        except (TypeError, ValueError):
            c = 0
        self.server.record(self.command, self._path(), c)

    def log_message(self, fmt, *args):
        pass


class LabServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, port: int):
        self.root = None
        self.echo = None
        self._hits = []
        self._lock = threading.Lock()
        super().__init__(("127.0.0.1", port), LabHandler)

    def root_str(self) -> str:
        r = self.root
        return str(r) if r is not None else str(Path(tempfile.gettempdir()) / "akashi-hotfix-lab-nothing")

    def set_root(self, p) -> None:
        with self._lock:
            self.root = Path(p) if p else None
            self._hits = []

    def record(self, method: str, path: str, code: int) -> None:
        with self._lock:
            self._hits.append((method, path, code))
        if self.echo:
            self.echo(method, path, code)

    def hits(self) -> list:
        with self._lock:
            return list(self._hits)

    def start(self) -> None:
        threading.Thread(target=self.serve_forever, kwargs={"poll_interval": 0.2}, daemon=True).start()

    def stop(self) -> None:
        try:
            self.shutdown()
        finally:
            self.server_close()


def req_names(hits: list) -> list:
    """앱이 받아 간 것 — /hotfix/ 를 뗀 이름."""
    return [p[len("/hotfix/"):] for (_m, p, _c) in hits if p.startswith("/hotfix/")]


def show_hits(hits: list) -> str:
    return " · ".join("%s %d" % (p[len("/hotfix/"):] if p.startswith("/hotfix/") else p, c) for (_m, p, c) in hits) or "없음"


# ── 화면 쪽 ──────────────────────────────────────────────────────────────────
# onHotfixStatus 에 갈고리 — 몇 번째 알림인지 세고 마지막 상태를 둔다. 원래 함수는 그대로 불린다.
JS_HOOK = r"""(function(){
  var H = window.__akashiHf;
  if (!H) {
    if (typeof window.onHotfixStatus !== 'function') return -1;
    H = window.__akashiHf = {n: 0, last: null, orig: window.onHotfixStatus};
    window.onHotfixStatus = function (a) {
      try {
        var st = Array.isArray(a) ? a[0] : a;
        if (typeof st === 'string') st = JSON.parse(st);
        H.last = st || {};
      } catch (e) { H.last = {}; }
      H.n++;
      return H.orig.apply(this, arguments);
    };
  }
  return H.n;
})()"""
JS_UNHOOK = r"""(function(){
  var H = window.__akashiHf;
  if (H) { window.onHotfixStatus = H.orig; delete window.__akashiHf; }
  var old = document.querySelectorAll('#settings-log [data-akashi-hf]');
  for (var i = 0; i < old.length; i++) old[i].removeAttribute('data-akashi-hf');
  return !!H;
})()"""
# 설정 로그의 지금 마지막 줄에 표식 — 그 뒤에 붙는 줄이 이번 확인의 줄이다
JS_LOG_MARK = r"""(function(){
  var b = document.getElementById('settings-log'); if (!b) return false;
  var old = b.querySelectorAll('[data-akashi-hf]');
  for (var i = 0; i < old.length; i++) old[i].removeAttribute('data-akashi-hf');
  if (b.lastElementChild) b.lastElementChild.setAttribute('data-akashi-hf', '1');
  return true;
})()"""
# 표식이 밀려 사라졌거나 처음부터 비었으면 처음부터 — 고침 꾸러미 줄만, 마지막 12줄
JS_LOG_SINCE = r"""(function(){
  var b = document.getElementById('settings-log'); if (!b) return [];
  var m = b.querySelector('[data-akashi-hf]');
  var e = m ? m.nextElementSibling : b.firstElementChild, out = [];
  for (; e; e = e.nextElementSibling) {
    var t = (e.textContent || '').replace(/\s+/g, ' ').trim();
    if (/고침 꾸러미|도우미|서명/.test(t)) out.push(t.slice(0, 300));
  }
  return out.slice(-12);
})()"""
JS_READY = ("typeof backend !== 'undefined' && !!backend.checkHotfixNow && !!backend.getHotfixStatus"
            " && !!backend.setHotfixAuto && !!backend.resetHotfixTools && typeof checkHotfixNow === 'function'"
            " && typeof onHotfixStatus === 'function' && !!document.getElementById('hotfix-status')")


def guard(port: int):
    """(오류 글, 기록). 붙어도 되는 격리 사본이면 오류 글이 None — check_adapt.py 와 같은 규칙."""
    st = iso.load_state(port)
    if not st:
        return "포트 %d 로 띄운 격리 사본 기록이 없습니다" % port, None
    if not iso.running(st):
        return "기록된 격리 사본(PID %s)이 떠 있지 않습니다" % st.get("pid"), None
    if not proc.port_open(port):
        return "격리 사본은 떠 있지만 포트 %d 가 닫혀 있습니다" % port, None
    lp = proc.listening_pids(port)
    pid = int(st["pid"])
    # ★ 듣는 PID 를 못 보면(lsof 실패·다른 사용자 프로세스) 격리 사본인지 모른다 — check_adapt 처럼 닫힌 쪽으로
    if not lp:
        return "포트 %d 를 누가 듣는지 확인하지 못했습니다(lsof 가 답하지 않음) — 붙지 않습니다" % port, None
    if pid not in lp and not (set(lp) & set(proc.children(pid))):
        return ("포트 %d 를 격리 사본(PID %d)이 아닌 프로세스(PID %s)가 듣고 있습니다 — 붙지 않습니다"
                % (port, pid, ", ".join(str(x) for x in lp))), None
    return None, st


def iso_call(args: list, timeout: float):
    try:
        r = subprocess.run([sys.executable, str(ISO_PY)] + args, capture_output=True, text=True, timeout=timeout)
        return r.returncode, scrub((r.stdout or "") + (r.stderr or "")).splitlines()
    except subprocess.TimeoutExpired:
        return 124, ["iso.py 가 %d초 안에 끝나지 않았습니다" % timeout]
    except OSError as e:
        return 2, ["iso.py 를 돌리지 못했습니다 (%s)" % e]


def applog_size(st: dict) -> int:
    try:
        return os.path.getsize(st.get("log") or "")
    except OSError:
        return 0


def applog_since(st: dict, off: int) -> list:
    try:
        with open(st.get("log") or "", "rb") as f:
            f.seek(off)
            data = f.read(4 * 1024 * 1024)
    except OSError:
        return []
    return data.decode("utf-8", "replace").splitlines()


# ── run ──────────────────────────────────────────────────────────────────────
class LabRun:
    def __init__(self, a):
        self.a = a
        self.url = "http://127.0.0.1:%d/hotfix/" % a.http_port
        self.problems = []          # 시나리오 밖의 문제
        self.notes = []             # 알릴 것(문제 아님)
        self.restored = []
        self.results = []
        self.restart = None
        self.page = None
        self.st = {}
        self.hdir = None
        self.srv = None
        self.lab = None
        self.ctx = None
        self.started = False
        self.initial = {}
        self.prev_auto = True
        self.auto_changed = False
        self.aborted = ""
        self.fatal_msg = ""
        self.base = 0
        self.ids = []

    # ── 말하기 ──
    def say(self, line: str = "") -> None:
        if not self.a.json:
            print(line, flush=True)

    def detail(self, line: str) -> None:
        if self.a.verbose and not self.a.json:
            print(line, flush=True)

    # ── 화면 ──
    def js(self, expr: str, timeout=None):
        return self.page.eval(expr, timeout)

    def hook(self) -> int:
        n = self.js(JS_HOOK)
        if n is None or n < 0:
            raise Fatal("화면에 onHotfixStatus 가 없습니다 — 이 앱 판엔 고침 꾸러미 화면이 없습니다")
        return n

    def status_after(self, trigger: str, timeout: float) -> dict:
        """trigger 를 돌리고, 앱이 onHotfixStatus 로 새 상태를 한 번 더 알릴 때까지 기다려 그 상태를 돌려준다."""
        n0 = self.js("window.__akashiHf ? window.__akashiHf.n : -1")
        if n0 is None or n0 < 0:
            n0 = self.hook()
        self.js(trigger)
        self.page.wait_for("!!(window.__akashiHf && window.__akashiHf.n > %d)" % n0, timeout=timeout, every=0.2)
        return self.js("window.__akashiHf.last") or {}

    def last_status(self) -> dict:
        return self.js("window.__akashiHf && window.__akashiHf.last") or {}

    def attach(self) -> None:
        err, st = guard(self.a.port)
        if err:
            raise Fatal(err)
        self.st = st
        self.hdir = sandbox_hotfix_dir(st)
        if self.hdir is None:
            raise Fatal("격리 사본의 홈이 진짜 홈을 가리킵니다 — 붙지 않습니다")
        try:
            ts = [t for t in cdplib.page_targets(self.a.port) if "window=" not in t.get("url", "")]
        except (OSError, ValueError) as e:
            raise Fatal("포트 %d 에서 화면 목록을 읽지 못했습니다 (%s)" % (self.a.port, e))
        if not ts:
            raise Fatal("포트 %d 에 본 창(index.html) 화면이 없습니다" % self.a.port)
        ws_url = ts[0].get("webSocketDebuggerUrl") or ""
        # ★ 주소는 앱이 알려 준 것 — check_adapt 처럼 이 기계(127.0.0.1) 밖으로는 나가지 않는다.
        #   다른 디버거가 먼저 붙어 있으면 주소가 빠져 온다 — ["…"] 로 읽으면 KeyError 로 넘어져 종료 1(문제)로 보였다
        if urllib.parse.urlparse(ws_url).hostname not in ("127.0.0.1", "localhost", "::1"):
            raise Fatal("화면 주소가 비었거나 이 기계(127.0.0.1)가 아닙니다 — 붙지 않습니다(다른 디버거가 먼저 붙어 있으면 비어 옵니다)")
        try:
            self.page = Page(ws_url, timeout=15)
        except (WsError, OSError) as e:
            raise Fatal("화면에 붙지 못했습니다 (%s)" % e)
        try:
            self.page.wait_for(JS_READY, timeout=30, every=0.3)
        except CdpError:
            raise Fatal("화면에 고침 꾸러미 기능(backend.checkHotfixNow 등)이 없습니다 — 앱이 옛 판이거나 준비 중")
        # 이 뒤에 생긴 JS 예외만 센다(붙기 전 것은 우리 탓이 아니다)
        self.page.cmd("Runtime.enable")
        self.page.pump(0.3)
        self.page.events.clear()
        self.hook()

    def disk(self) -> dict:
        """사본 자료 폴더의 hotfix/ — 읽기만."""
        d = self.hdir
        st = read_json(d / "state.json")
        tools = {}
        td = d / "tools"
        if td.is_dir():
            for f in sorted(td.iterdir()):
                if f.is_file():
                    tools[f.name] = sha256_file(f)
        return {"version": qt_int(st.get("version")), "tools": tools,
                "state_tools": st.get("tools") if isinstance(st.get("tools"), dict) else {},
                "staging": (d / "tools.staging").exists()}

    # ── 사본 ──
    def start_iso(self) -> None:
        self.say("  격리 사본을 띄웁니다 — 고침 주소 %s" % self.url)
        rc, out = iso_call(["start", "--port", str(self.a.port), "--hotfix-base", self.url], timeout=180)
        self.started = True          # 반쯤 떴어도 우리가 끈다
        for l in out:
            if rc != 0 or self.a.verbose or l.startswith("띄움"):
                self.say("    │ " + l)
        if rc != 0:
            raise Fatal("격리 사본을 띄우지 못했습니다 (iso.py 종료 %d)" % rc)

    def stop_iso(self) -> bool:
        rc, out = iso_call(["stop", "--port", str(self.a.port)], timeout=60)
        for l in out:
            self.detail("    │ " + l)
        return rc == 0 and not iso.running(iso.load_state(self.a.port))

    def ensure_iso(self) -> None:
        st = iso.load_state(self.a.port)
        if iso.running(st):
            base = (st.get("hotfix_base") or "").rstrip("/")
            if base == self.url.rstrip("/"):
                self.say("  격리 사본 그대로 씀 — PID %s · 포트 %d (이 실험실 주소로 떠 있음)" % (st.get("pid"), self.a.port))
                return
            if not self.a.replace:
                raise Fatal("격리 사본(PID %s)이 다른 고침 주소(%s)로 떠 있습니다 — 주소는 켤 때만 정해집니다.\n"
                            "  끄고 다시: python3 akashi/iso.py stop --port %d  (또는 이 공구에 --replace)"
                            % (st.get("pid"), base or "기본 · 공개 저장소", self.a.port))
            self.say("  다른 주소로 떠 있던 격리 사본을 끕니다(--replace)")
            if not self.stop_iso():
                raise Fatal("격리 사본을 끄지 못했습니다")
            self.notes.append("원래 떠 있던 격리 사본(다른 고침 주소)은 껐습니다 — 필요하면 iso.py start 로 다시")
        elif proc.port_open(self.a.port):
            raise Fatal("포트 %d 를 격리 사본이 아닌 것(PID %s)이 쓰고 있습니다 — --port 로 다른 번호를"
                        % (self.a.port, proc.listening_pids(self.a.port)))
        self.start_iso()

    # ── 한 가지 ──
    def one(self, idx: int, total: int, rec: dict) -> None:
        s = BY_ID[rec["id"]]
        b = Path(rec["dir"]) / "hotfix"
        before = self.last_status()
        disk_b = self.disk()
        have = qt_int(before.get("version"))
        pem = self.lab / "_app_pub.pem"
        # 앱이 '지금' 할 일을 다시 셈한다 — 앞 단계가 뜻과 다르게 끝났으면 이 시험은 할 수 없다
        pred = predict(b, pem, have, self.ctx.app_ver, self.ctx.tool_ok)
        res = {"id": s.sid, "title": s.title, "version": rec["version"], "expect": s.expect,
               "promise": s.promise, "ok": False, "problems": [], "result": "", "requests": [], "lines": [],
               "seconds": 0.0}
        self.results.append(res)
        head = "  %s %d %-16s v%-3d" % ("%s", idx, s.sid, rec["version"])
        if pred["code"] != s.expect:
            res["problems"].append("대 볼 수 없음 — 사본이 v%d 을 쓰는 중이라 앱이 할 일이 '%s' (앞 단계가 뜻과 달랐음)"
                                   % (have, CODE_KO.get(pred["code"], pred["code"])))
            self.say(head % "✗" + " 건너뜀")
            for p in res["problems"]:
                self.say("      · " + p)
            return
        self.srv.set_root(Path(rec["dir"]))
        self.js(JS_LOG_MARK)
        t0 = time.time()
        timed_out = False
        try:
            after = self.status_after("(checkHotfixNow(), 1)", self.a.timeout)
        except CdpError as e:
            if not iso.running(iso.load_state(self.a.port)):
                raise
            timed_out = True
            after = self.last_status()
            res["problems"].append("앱이 %d초 안에 답하지 않았습니다 (%s)" % (self.a.timeout, str(e)[:80]))
        res["seconds"] = round(time.time() - t0, 2)
        # 로그 줄은 모아서 보낸다(flushLogs) — 상태 알림보다 조금 늦을 수 있다
        want = log_fragments(pred)
        end = time.time() + 3.0
        lines = []
        while True:
            lines = self.js(JS_LOG_SINCE) or []
            if all(any(all(f in l for f in fr) for l in lines) for fr in want) or time.time() > end:
                break
            time.sleep(0.2)
        hits = self.srv.hits()
        reqs = req_names(hits)
        disk_a = self.disk()
        res["result"] = str(after.get("lastResult") or "")
        res["requests"] = ["%s %d" % (p, c) for (_m, p, c) in hits]
        res["lines"] = lines
        if not timed_out:
            probs, summary = judge(pred, before, after, disk_b, disk_a, reqs, lines)
            res["problems"] += probs
        else:
            summary = ""
        for m in self.page.events:
            if m.get("method") == "Runtime.exceptionThrown":
                d = (m.get("params") or {}).get("exceptionDetails", {})
                txt = ((d.get("exception") or {}).get("description") or d.get("text") or "?").splitlines()[0][:160]
                res["problems"].append("화면 JS 예외: " + txt)
        self.page.events.clear()
        res["ok"] = not res["problems"]
        res["summary"] = summary
        if res["ok"]:
            self.say(head % "✓" + " " + summary + " (%.1f초)" % res["seconds"])
        else:
            self.say(head % "✗" + " 뜻: " + s.promise)
            self.say("      앱: " + (res["result"] or "(상태 줄 없음)"))
            for p in res["problems"]:
                self.say("      · " + p)
        self.detail("      받아 간 것: " + show_hits(hits))
        for l in lines:
            self.detail("      로그: " + l)
        try:
            self.detail("      상태칸: " + str(self.js("(document.getElementById('hotfix-status')||{}).textContent") or ""))
        except CdpError:
            pass

    # ── 다시 켜 보기 ──
    def restart_check(self) -> None:
        cur = self.last_status()
        ver = qt_int(cur.get("version"))
        r = {"ok": False, "problems": [], "line": "", "tampered": ""}
        self.restart = r
        if ver == 0:
            r["ok"] = True
            r["skipped"] = "받은 판이 없어 다시 켜 볼 것이 없음"
            self.say("  – 다시 켜 보기: 건너뜀(받은 판이 없음)")
            return
        disk0 = self.disk()
        valid = {n: h for n, h in disk0["tools"].items() if disk0["state_tools"].get(n) == h and self.ctx.tool_ok(n)}
        self.say("  다시 켜 보기 — 끄고, 꺼진 사이 도우미 하나를 몰래 고치고, 다시 켭니다")
        off = applog_size(self.st)
        try:
            self.page.close()
        except OSError:
            pass
        self.page = None
        if not self.stop_iso():
            r["problems"].append("격리 사본을 끄지 못했습니다")
            return
        tampered = ""
        if valid:
            name = sorted(valid)[0]
            bd = iso.base_dir(self.a.port).resolve()
            tp = (self.hdir / "tools" / name).resolve()
            real = paths.data_dir().resolve()
            # 격리 사본 폴더(iso 표식 있음) 안의 파일일 때만 — 진짜 자료 폴더는 절대 아니다
            if (bd / iso.MARK).is_file() and bd in tp.parents and real not in tp.parents and tp.is_file():
                with open(tp, "ab") as f:
                    f.write("\n# 아카시 실험실 — 앱이 꺼진 사이 디스크에서 몰래 고친 줄\n".encode("utf-8"))
                tampered = name
            else:
                self.notes.append("도우미를 몰래 고치는 시험은 건너뜀(격리 사본 폴더 밖이라 손대지 않음)")
        r["tampered"] = tampered
        self.start_iso()
        self.attach()
        want_tools = len(valid) - (1 if tampered else 0)
        line = ""
        end = time.time() + 20
        while time.time() < end:
            hits = [l for l in applog_since(self.st, off) if "[고침 꾸러미]" in l]
            if hits:
                line = hits[-1].strip()
                break
            time.sleep(0.3)
        r["line"] = line
        if not line:
            r["problems"].append("앱 기록에 「[고침 꾸러미] …」 줄이 없습니다(켤 때 받아 둔 꾸러미를 읽지 않음)")
        else:
            if ("[고침 꾸러미] v%d 을 씀" % ver) not in line:
                r["problems"].append("다시 켠 뒤 쓰는 판이 다름 — 「%s」 (기대 v%d)" % (line, ver))
            if ("파이썬 도우미 %d" % want_tools) not in line:
                r["problems"].append("다시 켠 뒤 쓰는 도우미 수가 다름 — 「%s」 (기대 %d%s)"
                                     % (line, want_tools, " · 몰래 고친 %s 는 버려야" % tampered if tampered else ""))
        if tampered and (self.hdir / "tools" / tampered).exists():
            r["problems"].append("몰래 고친 %s 가 디스크에 그대로 남음(켤 때 지워야)" % tampered)
        st2 = self.status_after("(backend.getHotfixStatus(), 1)", 10)
        if qt_int(st2.get("version")) != ver:
            r["problems"].append("다시 켠 뒤 화면의 판이 v%d (기대 v%d)" % (qt_int(st2.get("version")), ver))
        r["ok"] = not r["problems"]
        if r["ok"]:
            self.say("  ✓ 다시 켜도 v%d%s" % (ver, " · 꺼진 사이 몰래 고친 %s 는 켤 때 버림" % tampered if tampered else ""))
        else:
            self.say("  ✗ 다시 켜 보기")
            for p in r["problems"]:
                self.say("      · " + p)
        self.detail("      앱 기록: " + (line or "(없음)"))

    # ── 되돌리기 ──
    def cleanup(self) -> None:
        if self.page is not None:
            try:
                cur = self.last_status()
                tools_now = cur.get("tools") or {}
                if tools_now and tools_now != (self.initial.get("tools") or {}):
                    after = self.status_after("(backend.resetHotfixTools(), 1)", 10)
                    if after.get("tools"):
                        self.problems.append("받은 파이썬 도우미를 되돌리지 못했습니다")
                    else:
                        self.restored.append("받은 파이썬 도우미를 앱에 든 것으로")
                        if self.initial.get("tools"):
                            self.notes.append("처음에 있던 받은 도우미(%s)는 되살릴 수 없습니다 — 격리 사본 안의 일"
                                              % ", ".join(sorted(self.initial["tools"])))
                if self.auto_changed:
                    after = self.status_after("(backend.setHotfixAuto(%s), 1)" % ("true" if self.prev_auto else "false"), 10)
                    if (after.get("auto") is not False) != self.prev_auto:
                        self.problems.append("자동 확인을 원래대로 돌리지 못했습니다")
                    else:
                        self.restored.append("자동 확인 %s" % ("켬" if self.prev_auto else "끔"))
                self.js(JS_UNHOOK)
                self.js("(backend.getHotfixStatus(), 1)")
                self.restored.append("화면 갈고리·로그 표식 뗌")
            except (CdpError, WsError, OSError) as e:
                self.problems.append("화면을 되돌리지 못했습니다 (%s)" % str(e)[:120])
            try:
                self.page.close()
            except OSError:
                pass
            self.page = None
        elif self.auto_changed:
            self.problems.append("앱과 연결이 끊겨 자동 확인을 원래대로 돌리지 못했습니다")
        if self.srv is not None:
            self.srv.stop()
            self.srv = None
        if self.started:
            if self.a.keep_app:
                self.notes.append("격리 사본을 켜 둡니다 — 끄기: python3 akashi/iso.py stop --port %d" % self.a.port)
            elif self.stop_iso():
                self.restored.append("띄운 격리 사본을 끔")
            else:
                self.problems.append("띄운 격리 사본을 끄지 못했습니다 — python3 akashi/iso.py stop --port %d" % self.a.port)
        if self.lab is not None:
            if self.a.keep_files:
                self.notes.append("시험 꾸러미를 남김: %s — 진짜 열쇠로 서명됨 · 다 쓰면 hotfix_lab.py clean"
                                  % paths.short(self.lab))
            elif remove_lab(self.lab):
                self.restored.append("시험 꾸러미 지움")
            else:
                self.problems.append("시험 꾸러미를 지우지 못했습니다: " + paths.short(self.lab))

    # ── 본 흐름 ──
    def main(self) -> int:
        a = self.a
        try:
            try:
                self.prepare()
                if proc.port_open(a.http_port):
                    raise Fatal("실험실 서버 포트 %d 를 다른 것(PID %s)이 쓰고 있습니다 — --http-port 로 다른 번호를"
                                % (a.http_port, proc.listening_pids(a.http_port)))
                try:
                    self.srv = LabServer(a.http_port)
                except OSError as e:
                    raise Fatal("실험실 서버를 열지 못했습니다 — 127.0.0.1:%d (%s)" % (a.http_port, e))
                self.srv.start()           # 폴더를 정하기 전에는 모든 요청에 404
                self.ensure_iso()
                self.attach()
                self.initial = self.status_after("(backend.getHotfixStatus(), 1)", 15)
                self.base = qt_int(self.initial.get("version"))
                self.prev_auto = self.initial.get("auto") is not False
                if self.prev_auto:
                    st = self.status_after("(backend.setHotfixAuto(false), 1)", 10)
                    self.auto_changed = True
                    if st.get("auto") is not False:
                        raise Fatal("자동 확인을 끄지 못했습니다 — 우리 확인과 섞일 수 있어 멈춥니다")
                vs = [version_of(i, self.base) for i in self.ids]
                self.say("  사본이 쓰는 판 v%d → 시험 판 v%d~v%d · 자동 확인 %s"
                         % (self.base, min(vs), max(vs), "잠시 끔" if self.prev_auto else "원래 꺼져 있음"))
                self.lab = new_lab()
                pem = write_pub_pem(self.ctx.pub_hex, self.lab / "_app_pub.pem")
                recs, probs = build_lab(self.lab, self.ids, self.base, self.ctx, pem)
                if probs:
                    self.problems += probs
                    self.aborted = "시험 꾸러미가 뜻대로 만들어지지 않아 앱에 먹이지 않았습니다"
                else:
                    self.say("  시험 꾸러미 %d가지 만듦(앱 없이 셈한 예상이 모두 뜻대로)" % len(recs))
                    self.say("")
                    for i, rec in enumerate(recs, 1):
                        if not iso.running(iso.load_state(a.port)):
                            self.aborted = "도중에 격리 사본이 사라졌습니다"
                            break
                        self.one(i, len(recs), rec)
                    if not self.aborted:
                        if self.started and not a.no_restart_check:
                            self.say("")
                            self.restart_check()
                        elif not a.no_restart_check:
                            self.notes.append("다시 켜 보기는 건너뜀 — 이 공구가 띄운 사본이 아니면 다시 켜지 않습니다")
            except Fatal as e:
                self.fatal_msg = str(e)
            except KeyboardInterrupt:
                self.aborted = "Ctrl-C 로 멈춤"
                self.fatal_msg = self.fatal_msg or "Ctrl-C 로 멈춤"
            except (CdpError, WsError, OSError) as e:
                self.aborted = "앱과 연결이 끊김 — %s" % str(e)[:160]
        finally:
            try:
                self.cleanup()
            except KeyboardInterrupt:
                self.problems.append("되돌리는 도중 Ctrl-C — 남은 것을 확인하십시오(iso.py status · hotfix_lab.py clean)")
        return self.report()

    def prepare(self) -> None:
        a = self.a
        st = iso.load_state(a.port)
        app = app_for(st)
        if not app or not paths.is_our_app(app):
            raise Fatal("격리 사본으로 띄울 앱이 없습니다 — mac/chernobyl 에서 ./build.sh")
        self.ctx = ctx = Ctx(app)
        ids = parse_only(a.only)
        if not ctx.key_ok:
            need = [i for i in ids if BY_ID[i].key == "real"]
            if a.only and need:
                raise Fatal("서명 열쇠가 없어 돌릴 수 없는 시나리오: %s (열쇠 없이 되는 것: unsigned, forged)"
                            % ", ".join(need))
            ids = [i for i in ids if BY_ID[i].key != "real"]
            self.notes.append("서명 열쇠가 없어 거절 쪽 %d가지(%s)만 돌렸습니다 — 받는 쪽은 열쇠가 있는 맥에서"
                              % (len(ids), ", ".join(ids)))
        self.ids = ids
        drift = phrase_drift()
        if drift:
            self.notes.append("앱 소스에서 판정에 쓰는 글이 바뀌었습니다(%d개) — selftest 로 확인" % len(drift))
        self.say("고침 꾸러미 실험실 — 격리 사본 포트 %d · 앱 %s · 서버 %s" % (a.port, ctx.app_ver, self.url))
        self.say("  서명 열쇠 %s · 앱 속 공개 열쇠 %s · 도우미 %s · 앱에 없는 이름 %s"
                 % ("있음(열지 않음)" if ctx.key_ok else "없음",
                    {True: "소스와 같음", False: "소스와 다름!", None: "확인 못 함"}[ctx.app_key], ctx.tool, ctx.bad))
        if ctx.app_key is False:
            self.problems.append("구운 앱에 소스(HotfixSig.cpp)의 공개 열쇠가 없습니다 — ./build.sh 로 다시 구우십시오")

    def report(self) -> int:
        bad = [r for r in self.results if not r["ok"]]
        restart_bad = bool(self.restart and not self.restart["ok"])
        n = len(bad) + len(self.problems) + (1 if restart_bad else 0) + (1 if self.aborted and not self.fatal_msg else 0)
        if self.fatal_msg:
            code = 2
            line = "돌릴 수 없음 — " + self.fatal_msg
        elif n:
            code = 1
            what = ["%s(%s)" % (r["id"], r["problems"][0][:60] if r["problems"] else "?") for r in bad]
            if restart_bad:
                what.append("다시 켜 보기")
            what += [p[:80] for p in self.problems]
            if self.aborted:
                what.append(self.aborted)
            line = "문제 %d건 — %s" % (n, " · ".join(what[:4]) + (" …" if len(what) > 4 else ""))
        else:
            code = 0
            partial = not self.ctx.key_ok if self.ctx else False
            line = "통과 — %s %d가지 모두 뜻대로%s%s" % (
                "거절 쪽" if partial else "시나리오", len(self.results),
                " · 다시 켜도 그대로" if self.restart and self.restart.get("line") else "",
                " (서명 열쇠가 없어 받는 쪽은 돌리지 않음)" if partial else "")
        if self.a.json:
            print(json.dumps({
                "tool": "hotfix_lab", "cmd": "run", "port": self.a.port, "server": self.url,
                "key": bool(self.ctx and self.ctx.key_ok), "app_version": self.ctx.app_ver if self.ctx else "",
                "helper": self.ctx.tool if self.ctx else "", "base": self.base, "scenarios": self.results,
                "restart": self.restart, "problems": self.problems, "notes": self.notes, "restored": self.restored,
                "aborted": self.aborted, "verdict": line, "exit": code,
            }, ensure_ascii=False, indent=1))
            return code
        if self.results or self.problems or self.notes or self.restored:
            self.say("")
        for p in self.problems:
            self.say("  ✗ " + p)
        if self.aborted and not self.fatal_msg:
            self.say("  ✗ " + self.aborted)
        for nte in self.notes:
            self.say("  · " + nte)
        if self.restored:
            self.say("  되돌림: " + " · ".join(self.restored))
        self.say(line)
        return code


def judge(pred: dict, before: dict, after: dict, disk_b: dict, disk_a: dict, reqs: list, lines: list):
    """앱이 한 일과 약속을 맞대 본다. (문제 목록, 한 줄 요약)."""
    probs = []
    if "manifest.json" not in reqs:
        probs.append("앱이 이 실험실 서버에 오지 않았습니다(요청 없음) — 고침 주소·프록시를 확인")
    res = str(after.get("lastResult") or "")
    frags = result_fragments(pred)
    if not all(f in res for f in frags):
        probs.append("상태 줄이 다름 — 기대 「%s」" % " … ".join(frags))
    v_b, v_a = qt_int(before.get("version")), qt_int(after.get("version"))
    t_b = before.get("tools") if isinstance(before.get("tools"), dict) else {}
    t_a = after.get("tools") if isinstance(after.get("tools"), dict) else {}
    code = pred["code"]
    if code == "applied":
        v = pred["version"]
        if v_a != v:
            probs.append("화면의 판이 v%d (기대 v%d)" % (v_a, v))
        if disk_a["version"] != v:
            probs.append("저장된 판이 v%d (기대 v%d)" % (disk_a["version"], v))
        if t_a != pred["accepted"]:
            probs.append("상태의 도우미가 %s (기대 %s)" % (names(t_a), names(pred["accepted"])))
        if disk_a["tools"] != pred["accepted"]:
            probs.append("디스크의 도우미가 %s (기대 %s)" % (names(disk_a["tools"]), names(pred["accepted"])))
        for n in pred["accepted"]:
            if ("tools/" + n) not in reqs:
                probs.append("도우미 %s 를 받아 가지 않음" % n)
        for bn in pred["rejected"]:
            if bn in disk_a["tools"] or bn in t_a:
                probs.append("앱에 없는 이름 %s 이(가) 들어옴" % bn)
            if ("tools/" + bn) in reqs:
                probs.append("앱에 없는 이름 %s 을(를) 내려받음(이름만 보고 건너뛰어야)" % bn)
        if qt_int(after.get("rejected")) < len(pred["rejected"]):
            probs.append("버린 수가 %d (기대 %d 이상)" % (qt_int(after.get("rejected")), len(pred["rejected"])))
        tl = sorted(pred["accepted"])
        summary = "받음 — v%d → v%d · 도우미 %s%s" % (
            v_b, v, ", ".join(tl) or "없음",
            " · 버린 이름 %s(내려받지 않음)" % ", ".join(pred["rejected"]) if pred["rejected"] else "")
    else:
        if v_a != v_b:
            probs.append("판이 바뀜 v%d → v%d (앞 판을 지켜야)" % (v_b, v_a))
        if disk_a["version"] != disk_b["version"]:
            probs.append("저장된 판이 바뀜 v%d → v%d" % (disk_b["version"], disk_a["version"]))
        if t_a != t_b:
            probs.append("상태의 도우미가 바뀜 %s → %s" % (names(t_b), names(t_a)))
        if disk_a["tools"] != disk_b["tools"]:
            probs.append("디스크의 도우미가 바뀜 %s → %s" % (names(disk_b["tools"]), names(disk_a["tools"])))
        extra = []
        if code in ("sig-bad", "sig-missing", "minapp"):
            extra = sorted(set(r for r in reqs if r not in ("manifest.json", "manifest.sig")))
            if extra:
                probs.append("서명·판을 보기 전에 받아 감: %s" % ", ".join(extra))
        summary = "버림 — %s · v%d 그대로%s" % (
            CODE_KO.get(code, code), v_b,
            " · 다른 파일은 받지도 않음" if code in ("sig-bad", "sig-missing", "minapp") and not extra else "")
    if disk_a["staging"]:
        probs.append("tools.staging 폴더가 남음")
    for fr in log_fragments(pred):
        if not any(all(f in l for f in fr) for l in lines):
            probs.append("설정 로그에 「%s」 줄이 없음" % " … ".join(fr))
    return probs, summary


def cmd_run(a) -> int:
    for p, what in ((a.port, "--port"), (a.http_port, "--http-port")):
        if not (1024 <= p < 65536):
            print("돌릴 수 없음 — %s 가 올바르지 않습니다: %d" % (what, p))
            return 2
    if a.port == a.http_port:
        print("돌릴 수 없음 — --port 와 --http-port 가 같습니다")
        return 2
    if not (3 <= a.timeout <= 300):
        print("돌릴 수 없음 — --timeout 은 3~300 초")
        return 2
    return LabRun(a).main()


# ── build ────────────────────────────────────────────────────────────────────
def cmd_build(a) -> int:
    out = [] if a.json else None

    def fatal(msg: str) -> int:
        if a.json:
            print(json.dumps({"tool": "hotfix_lab", "cmd": "build", "error": msg, "exit": 2}, ensure_ascii=False))
        else:
            print("돌릴 수 없음 — " + msg)
        return 2

    try:
        st = iso.load_state(a.port)
        ctx = Ctx(app_for(st))
        ids = parse_only(a.only)
        note = ""
        if not ctx.key_ok:
            need = [i for i in ids if BY_ID[i].key == "real"]
            if a.only and need:
                return fatal("서명 열쇠가 없어 만들 수 없는 시나리오: %s" % ", ".join(need))
            ids = [i for i in ids if BY_ID[i].key != "real"]
            note = "서명 열쇠가 없어 거절 쪽(%s)만 만들었습니다" % ", ".join(ids)
        if a.base_version is not None:
            base, why = a.base_version, "--base-version"
        else:
            hd = sandbox_hotfix_dir(st)
            base = qt_int(read_json(hd / "state.json").get("version")) if hd else 0
            why = "격리 사본(포트 %d)이 쓰는 판" % a.port if hd else "격리 사본 기록 없음"
        if base < 0 or base > 10 ** 6:
            return fatal("--base-version 이 올바르지 않습니다")
        lab = new_lab(a.out)
    except Fatal as e:
        return fatal(str(e))

    if not a.json:
        print("시험 꾸러미 만들기 — 판 기준 v%d (%s) · 앱 판 %s · 도우미 %s" % (base, why, ctx.app_ver, ctx.tool))

    def show(rec):
        if a.json:
            return
        pred = rec.get("pred") or {}
        print("  %s %-16s v%-3d %-26s → %s" % ("✓" if not rec["why"] else "✗", rec["id"], rec["version"], rec["title"],
                                              CODE_KO.get(pred.get("code", ""), "?")))
        for w in rec["why"]:
            print("      · " + w)

    try:
        pem = write_pub_pem(ctx.pub_hex, lab / "_app_pub.pem")
        recs, probs = build_lab(lab, ids, base, ctx, pem, say=show)
    except Fatal as e:
        remove_lab(lab) if not a.out else None
        return fatal(str(e))
    except KeyboardInterrupt:
        # ★ '만든 곳' 은 끝에야 알린다 — 도중에 멈추면 진짜 열쇠로 서명한 반쪽 꾸러미가 알리지도 않은 임시 폴더에 남았다
        if not a.out:
            remove_lab(lab)
        return fatal("Ctrl-C 로 멈춤" + (" — 반쯤 만든 폴더: %s (hotfix_lab.py clean)" % paths.short(lab) if a.out else ""))
    for r in recs:                      # 앞에서 멈춘 것(show 를 못 거친 것)
        if r["why"] and "pred" not in r and not a.json:
            print("  ✗ %-16s v%-3d %s" % (r["id"], r["version"], r["title"]))
            for w in r["why"]:
                print("      · " + w)
    if ctx.app_key is False:
        probs.append("구운 앱에 소스(HotfixSig.cpp)의 공개 열쇠가 없습니다 — ./build.sh 로 다시 구우십시오")
    code = 1 if probs else 0
    line = ("문제 %d건 — %s" % (len(probs), " · ".join(p[:80] for p in probs[:3]))) if probs else \
        "통과 — %d가지를 만들었고 앱 없이 셈한 예상이 모두 뜻대로" % len(recs)
    if a.json:
        print(json.dumps({"tool": "hotfix_lab", "cmd": "build", "dir": str(lab), "base": base,
                          "app_version": ctx.app_ver, "helper": ctx.tool, "bad": ctx.bad, "key": ctx.key_ok,
                          "scenarios": [{k: v for k, v in r.items() if k != "dir"} for r in recs],
                          "problems": probs, "note": note, "verdict": line, "exit": code},
                         ensure_ascii=False, indent=1))
        return code
    if note:
        print("  · " + note)
    print("  만든 곳: %s" % paths.short(lab))
    print("  ! 진짜 열쇠로 서명한 시험 꾸러미입니다 — 저장소·공유 폴더에 옮기지 말고, 다 쓰면: "
          "python3 akashi/hotfix_lab.py clean")
    print("  내보내기: python3 akashi/hotfix_lab.py serve good --dir %s" % paths.short(lab))
    print(line)
    return code


# ── serve ────────────────────────────────────────────────────────────────────
def cmd_serve(a) -> int:
    sid = a.scenario
    if not (1024 <= a.http_port < 65536):
        print("돌릴 수 없음 — --http-port 가 올바르지 않습니다")
        return 2
    own = None
    try:
        if a.dir:
            lab = Path(a.dir).expanduser().resolve()
            if not (lab / MARK).is_file():
                raise Fatal("실험실 폴더가 아닙니다(표식 없음): " + paths.short(lab))
            if not (lab / sid / "hotfix" / "manifest.json").is_file():
                raise Fatal("그 폴더에 %s 꾸러미가 없습니다 — build --only %s 로 먼저" % (sid, sid))
            ver = next((s.get("version") for s in read_json(lab / "lab.json").get("scenarios", []) if s.get("id") == sid), "?")
        else:
            st = iso.load_state(a.port)
            ctx = Ctx(app_for(st))
            if BY_ID[sid].key == "real" and not ctx.key_ok:
                raise Fatal("서명 열쇠가 없어 %s 는 만들 수 없습니다(열쇠 없이 되는 것: unsigned, forged)" % sid)
            if a.base_version is not None:
                base = a.base_version
            else:
                hd = sandbox_hotfix_dir(st)
                base = qt_int(read_json(hd / "state.json").get("version")) if hd else 0
            own = lab = new_lab()
            pem = write_pub_pem(ctx.pub_hex, lab / "_app_pub.pem")
            recs, probs = build_lab(lab, [sid], base, ctx, pem)
            if probs:
                for p in probs:
                    print("  ✗ " + p)
                remove_lab(own)
                print("문제 %d건 — 꾸러미를 뜻대로 만들지 못했습니다" % len(probs))
                return 1
            ver = recs[0]["version"]
        if proc.port_open(a.http_port):
            raise Fatal("포트 %d 를 다른 것(PID %s)이 쓰고 있습니다 — --http-port 로 다른 번호를"
                        % (a.http_port, proc.listening_pids(a.http_port)))
        srv = LabServer(a.http_port)
    except Fatal as e:
        remove_lab(own)
        print("돌릴 수 없음 — " + str(e))
        return 2
    except OSError as e:
        remove_lab(own)
        print("돌릴 수 없음 — 서버를 열지 못했습니다 (%s)" % e)
        return 2
    except KeyboardInterrupt:
        # ★ 꾸러미를 만드는 동안(서명에 몇 초) Ctrl-C 면 진짜 열쇠로 서명한 꾸러미가 임시 폴더에 그대로 남았다.
        #   serve 는 '멈출 때 지운다' 고 약속한다 — 여기서도 지운다
        remove_lab(own)
        print("\n돌릴 수 없음 — Ctrl-C")
        return 2
    url = "http://127.0.0.1:%d/hotfix/" % a.http_port
    srv.set_root(lab / sid)
    srv.echo = lambda m, p, c: print("  %s %s %s %d" % (time.strftime("%H:%M:%S"), m, p, c), flush=True)
    print("내보내는 중 — %s (%s · v%s · %s) · 멈추려면 Ctrl-C" % (url, sid, ver, BY_ID[sid].title))
    print("  격리 사본을 이 주소로: python3 akashi/iso.py start --hotfix-base %s" % url)
    print("  사본에서: 설정 → 유지보수 → 고침 꾸러미 → 지금 확인  (앱은 지금 판보다 높은 판만 봅니다)")
    try:
        srv.serve_forever(poll_interval=0.3)
    except KeyboardInterrupt:
        pass
    finally:
        srv.server_close()
        gone = remove_lab(own) if own else False
    n = len(srv.hits())
    print("멈춤 — 받아 간 요청 %d건%s" % (n, " · 시험 꾸러미 지움" if gone else ""))
    return 0


# ── verify ───────────────────────────────────────────────────────────────────
def cmd_verify(a) -> int:
    d = Path(a.dir).expanduser().resolve()
    b = d if (d / "manifest.json").is_file() else d / "hotfix"
    if not (b / "manifest.json").is_file():
        print("돌릴 수 없음 — manifest.json 이 없습니다: " + paths.short(d))
        return 2
    try:
        app = Path(a.app).resolve() if a.app else paths.find_build_app()
        if a.app and not paths.is_our_app(app):
            raise Fatal("우리 앱 번들이 아닙니다: " + paths.short(app))
        ctx = Ctx(app)
    except Fatal as e:
        print("돌릴 수 없음 — " + str(e))
        return 2
    tmp = Path(tempfile.mkdtemp(prefix=PREFIX + "verify-"))
    try:
        pem = write_pub_pem(ctx.pub_hex, tmp / "app_pub.pem")
        pred = predict(b, pem, a.have, ctx.app_ver, ctx.tool_ok)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    man = read_json(b / "manifest.json")
    rows = []
    sig = "없음" if pred["code"] == "sig-missing" else ("틀림" if pred["code"] == "sig-bad" else
                                                        ("?" if pred["code"] == "no-manifest" else "맞음"))
    rows.append(("서명", "%s (앱의 공개 열쇠로)" % sig))
    files = man.get("files") if isinstance(man.get("files"), dict) else {}
    for n in FILES:
        meta = files.get(n)
        if not isinstance(meta, dict) or not meta:
            rows.append((n, "싣지 않음"))
            continue
        try:
            data = (b / n).read_bytes()
            ok = sha256_bytes(data) == str(meta.get("sha256", "")).lower() and len(data) <= FILE_MAX
            rows.append((n, "sha256 맞음 · %d바이트" % len(data) if ok else "sha256 어긋남 또는 너무 큼"))
        except OSError:
            rows.append((n, "파일 없음"))
    tools = man.get("tools") if isinstance(man.get("tools"), dict) else {}
    for n in sorted(tools):
        if not ctx.tool_ok(n):
            rows.append(("tools/" + n, "앱에 없는 이름 — 앱이 건너뜀"))
            continue
        # ★ 올리기 전 꾸러미는 모양이 틀릴 수 있다 — 항목이 사전이 아니면(글 등) .get 에서 넘어져 종료 1(문제)로 보였다.
        #   predict() 처럼 사전이 아니면 빈 것으로 본다(sha256 어긋남)
        meta = tools[n] if isinstance(tools[n], dict) else {}
        try:
            data = (b / "tools" / n).read_bytes()
            ok = sha256_bytes(data) == str(meta.get("sha256", "")).lower() and len(data) <= TOOL_MAX
            rows.append(("tools/" + n, "sha256 맞음 · %d바이트" % len(data) if ok else "sha256 어긋남 — 판 전체를 버림"))
        except OSError:
            rows.append(("tools/" + n, "파일 없음 — 판 전체를 버림"))
    # 모양 — 앱은 틀린 항목만 버리고 나머지는 쓴다(올리는 공구는 아예 멈춘다)
    data = {}
    for n in FILES:
        try:
            data[n] = json.loads((b / n).read_text("utf-8")) if (b / n).is_file() else ({} if n != "repair_rules.json" else [])
        except (OSError, ValueError):
            data[n] = {} if n != "repair_rules.json" else []
    try:
        shape = ctx.hp.validate(data)
    except Exception:  # noqa: BLE001 — 모양이 아주 틀리면 검사 자체가 넘어진다
        shape = ["모양 검사가 넘어짐(파일 모양이 크게 다름)"]
    would = pred["code"] == "applied"
    if a.json:
        print(json.dumps({"tool": "hotfix_lab", "cmd": "verify", "dir": paths.short(b), "have": a.have,
                          "app_version": ctx.app_ver, "manifest": {k: man.get(k) for k in ("version", "minApp", "notes")},
                          "predict": pred, "rows": rows, "shape": shape, "app_key_in_binary": ctx.app_key,
                          "exit": 0 if would else 1}, ensure_ascii=False, indent=1))
        return 0 if would else 1
    print("꾸러미 %s — v%s · minApp %s · 앱 판 %s · 지금 쓰는 판(가정) v%d"
          % (paths.short(b), man.get("version"), man.get("minApp"), ctx.app_ver, a.have))
    if man.get("notes"):
        print("  안내: " + str(man.get("notes"))[:200])
    for k, v in rows:
        print("  %-24s %s" % (k, v))
    for s in shape:
        print("  모양: " + s + "  (앱은 이 항목만 버림)")
    if ctx.app_key is False:
        print("  ! 구운 앱에 소스의 공개 열쇠가 없습니다 — 앱은 소스와 다른 열쇠를 믿습니다")
    if would:
        acc = sorted(pred["accepted"])
        print("통과 — 앱은 이 꾸러미를 받습니다(v%d · 도우미 %d%s%s)" % (
            pred["version"], len(acc), " — " + ", ".join(acc) if acc else "",
            " · 건너뛸 이름 " + ", ".join(pred["rejected"]) if pred["rejected"] else ""))
        return 0
    print("문제 1건 — 앱은 받지 않습니다: %s%s" % (CODE_KO.get(pred["code"], pred["code"]),
                                              " (%s)" % pred["name"] if pred["name"] else ""))
    return 1


# ── selftest ─────────────────────────────────────────────────────────────────
def cmd_selftest(a) -> int:
    checks = []

    def check(ok: bool, what: str) -> None:
        checks.append((bool(ok), what))
        print("  %s %s" % ("✓" if ok else "✗", what), flush=True)

    print("실험실 자체 점검 — 앱도 진짜 서명 열쇠도 쓰지 않습니다(버릴 열쇠로)")
    lab = None
    srv = None
    work = None
    try:
        # 1) 순수 셈
        check(qver_cmp(qver("4.0.0"), qver("4.0.0")) == 0 and qver_cmp(qver("999.0"), qver("4.0.0")) > 0
              and qver_cmp(qver("0"), qver("4.0.0")) < 0 and qver_cmp(qver("4.0"), qver("4.0.0")) < 0
              and qver_cmp(qver(""), qver("1")) < 0, "판 견주기가 QVersionNumber 와 같음(4.0 < 4.0.0 포함)")
        check(qt_int(3) == 3 and qt_int(3.0) == 3 and qt_int("3") == 0 and qt_int(True) == 0 and qt_int(None) == 0,
              "판 번호 읽기가 QJsonValue::toInt 와 같음(글·참거짓은 0)")
        drift = phrase_drift()
        check(not drift, "판정에 쓰는 앱 글이 소스에 그대로 있음" + ("" if not drift else " — 사라진 것: " + " / ".join(drift)))
        check(app_pub_hex() is not None, "앱의 공개 열쇠를 HotfixSig.cpp 에서 읽음")

        # 2) 꾸러미 만들기 — 버릴 열쇠를 '진짜 열쇠' 자리에
        app = paths.find_build_app()
        ctx = Ctx(app)
        lab = new_lab()
        # ★ 대신 쓸 '믿는 열쇠'(버릴 열쇠)는 실험 폴더 '밖' 에 둔다. 안에 두면 build_lab 의 누출 검사
        #   (실험 폴더에 비밀 열쇠 글이 없어야 한다)가 이 열쇠를 잡아 자체 시험이 늘 한 건 실패했다.
        work = Path(tempfile.mkdtemp(prefix="akashi-selftest-"))
        os.chmod(work, 0o700)
        tkey, thex = throwaway_key(work)
        pem = write_pub_pem(thex, work / "trusted_pub.pem")
        base = 5
        recs, probs = build_lab(lab, [s.sid for s in SCENARIOS], base, ctx, pem, trusted_key=str(tkey))
        check(not probs, "여덟 가지를 hotfix_publish.py 로 만들고 서명함" + ("" if not probs else " — " + " / ".join(probs[:3])))
        for r in recs:
            pred = r.get("pred") or {}
            check(pred.get("code") == r["expect"] and not r["why"],
                  "%-16s v%d → %s" % (r["id"], r["version"], CODE_KO.get(pred.get("code", ""), "만들지 못함")))
        check(not (lab / "_work").exists(), "버릴 열쇠·원본 사본을 남기지 않음(_work 없음)")
        check(not key_material_in(lab / "good") and not key_material_in(lab / "forged"),
              "꾸러미 안에 비밀 열쇠 글이 없음")
        by = {r["id"]: r for r in recs}
        if "good" in by and by["good"].get("pred"):
            g = lab / "good" / "hotfix"
            v = by["good"]["version"]
            check(predict(g, pem, v, ctx.app_ver, ctx.tool_ok)["code"] == "latest", "같은 판을 다시 주면 '최신' 으로 셈함")
            check(predict(g, pem, 0, "3.9", ctx.tool_ok)["code"] == ("minapp" if qver_cmp(qver(ctx.app_ver), qver("3.9")) > 0 else "applied"),
                  "minApp(앱 판과 같게 둔 것)이 옛 앱에선 '새 판 필요' 로 셈함")
            real = app_pub_hex()
            if real:
                rp = write_pub_pem(real, work / "real_pub.pem")
                check(predict(g, rp, 0, ctx.app_ver, ctx.tool_ok)["code"] == "sig-bad",
                      "버릴 열쇠의 서명은 앱의 진짜 공개 열쇠로는 '서명 틀림'")
        if "forged" in by and by["forged"].get("pred"):
            check(not sig_ok(pem, lab / "forged" / "hotfix" / "manifest.json", lab / "forged" / "hotfix" / "manifest.sig"),
                  "낯선 열쇠(forged)의 서명은 믿는 열쇠로 풀리지 않음")
        if "unsigned" in by:
            check(not (lab / "unsigned" / "hotfix" / "manifest.sig").exists(), "unsigned 에 manifest.sig 가 없음")

        # 3) 서버
        srv = LabServer(0)
        port = srv.server_address[1]
        srv.start()
        opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))

        def get(path, headers=None):
            req = urllib.request.Request("http://127.0.0.1:%d%s" % (port, path), headers=headers or {})
            try:
                with opener.open(req, timeout=5) as r:
                    return r.status, r.read(), dict(r.headers)
            except urllib.error.HTTPError as e:
                return e.code, b"", dict(e.headers or {})
            except OSError:
                return 0, b"", {}

        check(get("/hotfix/manifest.json")[0] == 404, "폴더를 정하기 전에는 모든 요청에 404")
        if "good" in by and by["good"].get("dir"):
            srv.set_root(lab / "good")
            c, body, h = get("/hotfix/manifest.json")
            check(c == 200 and body == (lab / "good" / "hotfix" / "manifest.json").read_bytes(),
                  "manifest.json 을 바이트 그대로 줌")
            check("no-store" in (h.get("Cache-Control") or ""), "캐시하지 말라고 알림(Cache-Control: no-store)")
            c2 = get("/hotfix/manifest.json", {"If-Modified-Since": "Fri, 01 Jan 2100 00:00:00 GMT"})[0]
            check(c2 == 200, "If-Modified-Since 를 무시하고 늘 통째로 줌(304 없음)")
            check(get("/hotfix/tools/%s" % ctx.tool)[0] == 200, "도우미 파일을 줌")
            bad_paths = ["/hotfix/", "/hotfix/tools/", "/hotfix/tools", "/hotfix/../lab.json", "/hotfix/%2e%2e/lab.json",
                         "/hotfix/.." + "/" + MARK, "/lab.json", "/" + MARK, "/"]
            check(all(get(p)[0] == 404 for p in bad_paths), "목록·폴더 밖·숨은 파일은 모두 404")
            names_seen = req_names(srv.hits())
            check("manifest.json" in names_seen, "받아 간 것을 적어 둠")
        if "unsigned" in by and by["unsigned"].get("dir"):
            srv.set_root(lab / "unsigned")
            check(get("/hotfix/manifest.sig")[0] == 404 and get("/hotfix/manifest.json")[0] == 200,
                  "폴더를 바꾸면 바로 그 꾸러미를 줌(unsigned 의 서명은 404)")
        srv.stop()
        srv = None
        check(not proc.port_open(port), "서버를 닫으면 포트도 닫힘")
    except Fatal as e:
        print("돌릴 수 없음 — " + str(e))
        if srv is not None:
            srv.stop()
        remove_lab(lab)
        return 2
    finally:
        if srv is not None:
            srv.stop()
        if work is not None:
            shutil.rmtree(work, ignore_errors=True)   # 버릴 열쇠 — 남기지 않는다
    gone = remove_lab(lab)
    check(gone, "실험 폴더를 지움")
    bad = [w for ok, w in checks if not ok]
    if bad:
        print("문제 %d건 — %s" % (len(bad), " · ".join(w.strip()[:60] for w in bad[:3])))
        return 1
    print("통과 — %d가지 모두 맞음(앱을 띄우지 않고 · 진짜 열쇠를 쓰지 않고)" % len(checks))
    return 0


# ── clean ────────────────────────────────────────────────────────────────────
def cmd_clean(a) -> int:
    if a.dirs:
        targets = [Path(d).expanduser().resolve() for d in a.dirs]
    else:
        root = Path(tempfile.gettempdir())
        targets = sorted(p for p in root.glob(PREFIX + "*") if p.is_dir())
    removed, skipped = 0, []
    for p in targets:
        if not (p / MARK).is_file():
            skipped.append(p)
            continue
        if remove_lab(p):
            removed += 1
            print("  지움 " + paths.short(p))
        else:
            skipped.append(p)
    for p in skipped:
        print("  남김 %s (실험실 표식 없음 또는 지우지 못함)" % paths.short(p))
    if a.dirs and skipped:
        print("문제 %d건 — 지우지 않은 폴더가 있습니다" % len(skipped))
        return 1
    print("통과 — 실험 폴더 %d개를 지웠습니다" % removed if removed else "통과 — 지울 실험 폴더가 없습니다")
    return 0


# ── 입구 ─────────────────────────────────────────────────────────────────────
def main() -> int:
    # ★ 빈 AKASHI_PORT 에 int("") 로 --help 까지 넘어졌다 — 글로 넘기면 argparse 가 type=int 로 바꾸고 틀리면 종료 2
    port_default = os.environ.get("AKASHI_PORT") or 9334
    ap = argparse.ArgumentParser(
        description="고침 꾸러미 실험실 — 서명·바꿔치기·판 번호를 격리 사본에 먹여 앱이 약속대로 받거나 버리는지 본다.",
        epilog="사용자의 앱에는 붙지 않습니다 — 격리 사본(iso.py)에만.\n"
               "종료 코드: 0 모두 뜻대로 · 1 어긋난 것 있음 · 2 돌릴 수 없음",
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", type=int, default=port_default,
                    help="격리 사본의 CDP 포트 (기본 9334 · 환경 변수 AKASHI_PORT)")
    sub = ap.add_subparsers(dest="cmd", required=True, metavar="{run,build,serve,verify,selftest,clean}")

    r = sub.add_parser("run", help="시나리오를 격리 사본에 차례로 먹여 본다(사본이 없으면 띄우고 끝나면 끈다)",
                       description="시나리오마다 꾸러미를 내놓고 앱에 '지금 확인' 을 누르게 한 뒤, 상태 줄·설정 로그·"
                                   "사본 자료 폴더·받아 간 요청으로 약속대로 했는지 본다.")
    r.add_argument("--only", metavar="이름,…", help="이 시나리오만 (%s)" % ", ".join(BY_ID))
    r.add_argument("--http-port", type=int, default=DEFAULT_HTTP, help="실험실 서버 포트 (기본 %d · 127.0.0.1 만)" % DEFAULT_HTTP)
    r.add_argument("--timeout", type=float, default=30, help="시나리오마다 앱의 답을 기다릴 초 (기본 30)")
    r.add_argument("--replace", action="store_true", help="격리 사본이 다른 고침 주소로 떠 있으면 끄고 새로 띄운다")
    r.add_argument("--keep-app", action="store_true", help="이 공구가 띄운 격리 사본을 끝나도 켜 둔다")
    r.add_argument("--keep-files", action="store_true", help="시험 꾸러미를 지우지 않는다(진짜 열쇠로 서명됨 — 조심)")
    r.add_argument("--no-restart-check", action="store_true", help="끝에 사본을 다시 켜 보는 것을 건너뛴다")
    r.add_argument("-v", "--verbose", action="store_true", help="받아 간 요청·로그 줄·상태칸까지 찍는다")
    r.add_argument("--json", action="store_true", help="사람용 글 대신 JSON 한 덩어리로")

    b = sub.add_parser("build", help="시험 꾸러미만 만든다(앱 없이 셈한 예상까지)")
    b.add_argument("--out", help="만들 폴더 (기본: 임시 폴더 · 저장소 안은 안 됨)")
    b.add_argument("--base-version", type=int, help="판 기준 (기본: 격리 사본이 쓰는 판 · 없으면 0)")
    b.add_argument("--only", metavar="이름,…", help="이 시나리오만")
    b.add_argument("--json", action="store_true", help="JSON 으로")

    s = sub.add_parser("serve", help="한 시나리오를 127.0.0.1:<포트>/hotfix/ 로 내놓는다(앞에서 · Ctrl-C 로 멈춤)")
    s.add_argument("scenario", choices=list(BY_ID), help="내놓을 시나리오")
    s.add_argument("--dir", help="build 로 만든 폴더 (없으면 이 자리에서 만들고 멈출 때 지운다)")
    s.add_argument("--http-port", type=int, default=DEFAULT_HTTP, help="서버 포트 (기본 %d)" % DEFAULT_HTTP)
    s.add_argument("--base-version", type=int, help="판 기준 (--dir 이 없을 때 · 기본: 격리 사본이 쓰는 판)")

    v = sub.add_parser("verify", help="꾸러미 폴더를 앱이 받을지 앱 없이 대 본다")
    v.add_argument("dir", help="꾸러미 폴더(manifest.json 이 있는 곳 또는 그 위)")
    v.add_argument("--have", type=int, default=0, help="앱이 지금 쓰는 판이라고 칠 번호 (기본 0)")
    v.add_argument("--app", help="견줄 앱 번들 (기본: mac/chernobyl/build 의 앱)")
    v.add_argument("--json", action="store_true", help="JSON 으로")

    sub.add_parser("selftest", help="앱도 진짜 열쇠도 없이 실험실 자체를 점검")

    c = sub.add_parser("clean", help="실험 폴더(표식 있는 것만)를 지운다")
    c.add_argument("dirs", nargs="*", help="지울 폴더 (없으면 임시 폴더의 %s* 전부)" % PREFIX)

    for p in (r, b, s):
        p.add_argument("--port", type=int, default=argparse.SUPPRESS, help="격리 사본의 CDP 포트")
    a = ap.parse_args()
    return {"run": cmd_run, "build": cmd_build, "serve": cmd_serve, "verify": cmd_verify,
            "selftest": cmd_selftest, "clean": cmd_clean}[a.cmd](a)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        print("\n돌릴 수 없음 — Ctrl-C")
        sys.exit(2)
