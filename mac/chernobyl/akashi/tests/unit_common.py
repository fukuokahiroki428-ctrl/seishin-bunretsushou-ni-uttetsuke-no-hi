#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""공통 코드 단위 시험 — 빌드의 오브젝트(Common · FileHelper · HttpClient)에 unit/ut_common.cpp 를 묶어 돌린다(앱 · 네트워크 없음, 약 30초).

    python3 akashi/tests/unit_common.py

표시 기록부(걸음마다 따로 · 막힌 걸음 · 되돌리기 · 옛 기록 · 믿지 않는 표시), 이름의 대소문자, 무결성 검사(디코더 없는 형식),
만든 날짜(시간대와 무관 — 일부러 TZ 를 뉴욕으로), UA(여러 스레드 · 기본값 · 힌트).
"""
from __future__ import annotations

import os
import shlex
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import _common as C  # noqa: E402

CHERNOBYL = HERE.parent.parent
MOC = "/opt/homebrew/share/qt/libexec/moc"
OBJS = ("src/core/Common.cpp.o", "src/utils/FileHelper.cpp.o", "src/utils/HttpClient.cpp.o")


def main() -> int:
    b = CHERNOBYL / "build" / "CMakeFiles" / "Hanishiki.dir"
    flags = b / "flags.make"
    if not flags.exists() or not all((b / o).exists() for o in OBJS):
        print("빌드 오브젝트가 없습니다 — mac/chernobyl 에서 ./build.sh 부터")
        return 2
    vals = {}
    for line in flags.read_text(encoding="utf-8").splitlines():
        if " = " in line and not line.startswith("#"):
            k, _, v = line.partition(" = ")
            vals[k] = v
    defs = shlex.split(vals.get("CXX_DEFINES", ""))
    incs = shlex.split(vals.get("CXX_INCLUDES", ""))
    work = C.work_dir("unit_common")
    moc_cpp = work / "moc_HttpClient.cpp"
    subprocess.run([MOC] + defs + [i for i in incs if i.startswith("-I")] + [str(CHERNOBYL / "src/utils/HttpClient.h"), "-o", str(moc_cpp)], check=True, cwd=str(CHERNOBYL))
    exe = work / "ut_common"
    cmd = ["clang++", "-std=gnu++17", "-O1", "-arch", "arm64"] + defs + incs + ["-F/opt/homebrew/lib",
           str(HERE / "unit" / "ut_common.cpp"), str(moc_cpp)] + [str(b / o) for o in OBJS] + [
           "-F/opt/homebrew/lib", "-framework", "QtCore", "-framework", "QtNetwork", "-framework", "QtGui",
           "-framework", "CoreFoundation", "-framework", "CoreServices", "-framework", "Foundation", "-framework", "AppKit", "-o", str(exe)]
    r = subprocess.run(cmd, cwd=str(CHERNOBYL), capture_output=True, text=True)
    if r.returncode != 0:
        print("빌드 실패:\n" + (r.stderr or r.stdout)[-1500:])
        print("실패")
        return 1
    env = dict(os.environ, TZ="America/New_York", HOME=str(work), QT_LOGGING_RULES="*.info=false")
    r = subprocess.run([str(exe)], cwd=str(work), env=env, capture_output=True, text=True, timeout=120)
    print(r.stdout, end="")
    if r.returncode not in (0, 1):
        print((r.stderr or "")[-800:])
        print("실패")
        return 1
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
