#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
port_parity.py — 맥판과 윈도우판이 갈라진 곳을 찾아 준다.

왜 있는가
---------
이 앱은 맥에서 만들어 윈도우로 옮겼다. 두 판은 src/ 대부분을 같은 파일로 쓴다.
그런데 한쪽에서 고친 것이 다른 쪽에 안 넘어가도 아무 일도 일어나지 않는다 —
빌드는 되고, 테스트도 통과하고, 그냥 한쪽만 조용히 낡는다.

실제로 그랬다. SelfRepair.h 는 파일 머리에 "mac/ 과 동일" 이라고 적혀 있는데
실제로는 1240줄이 갈라져 있었다. 윈도우 쪽에는 도구 자동 갱신·실기능 확인이
들어갔고, 맥 쪽에는 서명 자동복구가 들어갔다. 서로 모르는 채로.
그 주석은 그냥 틀린 말이 되어 있었다 — 사람이 읽고 믿는 종류의 틀린 말이다.

이 스크립트는 고쳐 주지 않는다. '어디가 갈라졌는지' 만 매번 눈에 띄게 말한다.
합칠지 말지는 사람이 정한다 — 두 판이 일부러 다른 곳도 있기 때문이다.

쓰는 법
-------
    python scripts/port_parity.py              # 사람이 읽는 표
    python scripts/port_parity.py --github     # CI 요약(단계 요약)에 붙일 마크다운

종료 코드는 항상 0 이다. 이건 막는 문이 아니라 알려 주는 표지판이다.
(빌드를 세우면 사람들은 곧 이 검사를 꺼 버린다. 그러면 없느니만 못하다.)
"""

import argparse
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WIN  = os.path.join(ROOT, "windows", "src")
MAC  = os.path.join(ROOT, "mac", "chernobyl", "src")

# 이름이 바뀐 짝 — 앱 이름을 네댓 번 바꾸면서 클래스 이름도 같이 바뀌었다.
#   ABIWA → Miyo → Chernobyl → Predormition.
#   파일 이름이 다르다고 '한쪽에만 있는 파일' 로 세면 매번 거짓 경보가 난다.
ALIASES = {
    "core/MiyoBackend.cpp":       "core/HanishikiBackend.cpp",
    "core/MiyoBackend.h":         "core/HanishikiBackend.h",
}

# 일부러 한쪽에만 있는 것들 — 여기 적어 두면 조용히 넘어간다.
#   ★ 적을 때는 왜 그런지도 같이 적는다. 이유 없는 예외는 다음 사람이 못 지운다.
EXPECTED_ONLY = {
    "windows": {
        "core/TerminalWindow.cpp":       "윈도우엔 붙일 터미널이 없어서 앱 안에 로그 창을 따로 만들었다",
        "core/TerminalWindow.h":         "위와 같음",
        "platforms/DiscordCollector.cpp": "맥판은 아직 디스코드를 안 옮겼다",
        "platforms/DiscordCollector.h":   "위와 같음",
        "platforms/InstagramCollector.cpp": "맥판은 아직 인스타그램을 안 옮겼다",
        "platforms/InstagramCollector.h":   "위와 같음",
        "platforms/YouTubeDownloader.cpp": "맥판은 유튜브를 백엔드 안에서 처리한다",
        "platforms/YouTubeDownloader.h":   "위와 같음",
    },
    "mac": {
        "platforms/DiscordError.h": "맥판 전용 디스코드 오류 타입",
    },
}

# 함수 머리 — 완벽한 C++ 파서가 아니다. 이름만 뽑으면 충분하다.
#
# ★ 들여쓰기를 어떻게 다룰지가 이 스크립트의 전부다. 두 번 틀렸다.
#     1) 줄 맨 앞만 봤다 → 헤더 안의 들여쓴 멤버 선언을 통째로 놓쳤고,
#        양쪽에 다 있는 setRunFlag·startDaemon 을 "한쪽에만 있다" 고 알렸다.
#     2) 들여쓰기를 전부 허용했다 → 이번엔 지역 변수(QFileInfo fi(path);)까지
#        함수로 세서 MiyoBackend.cpp 하나에 56개가 쏟아졌다. 신호가 잡음에 묻혔다.
#   답은 파일 종류로 나누는 것이다:
#     .cpp — 정의는 반드시 1열에서 시작한다. 지역 변수는 반드시 들여써 있다.
#     .h   — 멤버 선언은 들여써 있다. 지역 변수는 (거의) 없다.
#   그래서 .cpp 는 1열만, .h 는 들여쓰기를 허용해서 본다.
_BODY = (r"(?:[A-Za-z_][\w:<>,\s\*&]*?\s+)?"     # 반환형 (생성자는 없음)
         r"([A-Za-z_]\w*)\s*\(")                 # 이름
FUNC_CPP = re.compile(r"^(?:inline\s+|static\s+|virtual\s+|explicit\s+)*" + _BODY, re.M)
FUNC_HDR = re.compile(r"^[ \t]*(?:inline\s+|static\s+|virtual\s+|explicit\s+)*" + _BODY, re.M)

# 흔한 오검출 — 제어문·매크로는 함수가 아니다
NOT_FUNCS = {"if", "for", "while", "switch", "catch", "return", "sizeof",
             "throw", "else", "do", "case", "defined", "Q_UNUSED", "emit",
             "Q_OBJECT", "Q_PROPERTY", "signals", "public", "private", "protected"}


def funcs_of(path):
    try:
        with open(path, encoding="utf-8") as f:
            src = f.read()
    except OSError:
        return set()
    # 주석과 문자열은 걷어낸다 — 주석 속 예시가 함수로 잡히면 잡음만 는다
    src = re.sub(r"/\*.*?\*/", "", src, flags=re.S)
    src = re.sub(r"//[^\n]*", "", src)
    src = re.sub(r'"(?:[^"\\\n]|\\.)*"', '""', src)
    rx = FUNC_HDR if path.endswith(".h") else FUNC_CPP
    return {m.group(1) for m in rx.finditer(src)} - NOT_FUNCS


def rel_files(base):
    out = set()
    for dirpath, _dirs, files in os.walk(base):
        for fn in files:
            if fn.endswith((".cpp", ".h")):
                out.add(os.path.relpath(os.path.join(dirpath, fn), base).replace("\\", "/"))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--github", action="store_true", help="마크다운으로 출력 (CI 요약용)")
    args = ap.parse_args()

    if not os.path.isdir(WIN) or not os.path.isdir(MAC):
        print("두 판의 src/ 를 모두 찾지 못했습니다 — 검사를 건너뜁니다.")
        return 0

    win, mac = rel_files(WIN), rel_files(MAC)

    # 별칭을 맥 쪽 이름으로 맞춰 놓고 비교한다
    win_norm = {ALIASES.get(f, f) for f in win}

    only_win = sorted(f for f in win if ALIASES.get(f, f) not in mac)
    only_mac = sorted(f for f in mac if f not in win_norm)

    unexpected_win = [f for f in only_win if f not in EXPECTED_ONLY["windows"]]
    unexpected_mac = [f for f in only_mac if f not in EXPECTED_ONLY["mac"]]

    # 양쪽에 다 있는 파일의 함수 차이
    drift = []
    for f in sorted(win):
        peer = ALIASES.get(f, f)
        if peer not in mac:
            continue
        wf = funcs_of(os.path.join(WIN, f))
        mf = funcs_of(os.path.join(MAC, peer))
        w_only, m_only = sorted(wf - mf), sorted(mf - wf)
        if w_only or m_only:
            drift.append((f, peer, w_only, m_only))

    md = args.github
    B = (lambda t: "**%s**" % t) if md else (lambda t: t)
    out = []
    add = out.append

    add(("## " if md else "═══ ") + "맥·윈도우 판 갈라짐 검사" + ("" if md else " ═══"))
    add("")
    if not drift and not unexpected_win and not unexpected_mac:
        add("갈라진 곳 없음. 두 판의 공통 파일이 같은 함수를 갖고 있습니다.")
        print("\n".join(out))
        return 0

    if unexpected_win or unexpected_mac:
        add(B("한쪽에만 있는 파일 (예상 목록에 없음)"))
        for f in unexpected_win:
            add("  · 윈도우에만: %s" % f)
        for f in unexpected_mac:
            add("  · 맥에만: %s" % f)
        add("")

    if drift:
        add(B("공통 파일인데 함수가 다른 곳"))
        add("")
        for f, peer, w_only, m_only in drift:
            name = f if f == peer else "%s ↔ %s" % (f, peer)
            add(("### " if md else "  ") + name)
            if w_only:
                add("    윈도우에만 있는 함수 (%d): %s" % (len(w_only), ", ".join(w_only[:12])
                    + (" …" if len(w_only) > 12 else "")))
            if m_only:
                add("    맥에만 있는 함수 (%d): %s" % (len(m_only), ", ".join(m_only[:12])
                    + (" …" if len(m_only) > 12 else "")))
            add("")

    add("한쪽에만 있는 것이 전부 잘못은 아닙니다 — 일부러 다른 곳도 있습니다.")
    add("다만 '고쳤는데 저쪽에 안 넘어간 것' 이 여기 섞여 있으면 그건 조용히 낡습니다.")
    print("\n".join(out))
    return 0


if __name__ == "__main__":
    sys.exit(main())
