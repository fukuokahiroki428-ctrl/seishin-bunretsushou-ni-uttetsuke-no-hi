#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""파이썬 환경 바꾸기 — 실패해도 옛 환경이 남는가(격리 사본 · 인터넷으로 안 나감, 약 5분).

    python3 akashi/tests/python_swap.py

앱의 'Python 업그레이드' 를 시험 꾸러미(HANISHIKI_TEST_PY_TARBALL)로 돌린다. 꾸러미의 파이썬은 가짜 셸 스크립트다.
  1 깨진 꾸러미 → 압축 해제 실패 · 옛 환경 그대로 · 곁에 푼 것(.new)이 남지 않음
  2 돌지 않는 파이썬 → 교체하지 않음 · 옛 환경 그대로
  3 모듈을 하나도 못 깖(네트워크 끊김 흉내) → 옛 환경으로 되돌림
  3b 몇 개만 깔고 끊김 → 그래도 옛 환경으로 되돌림(모듈이 빠진 파이썬으로는 그 수집이 멈춘다)
  3c 다 깔았는데 수집에 쓰는 환경으로 불러지지 않음 → 옛 환경으로 되돌림
  4 정상 → 새 환경으로 바뀌고 .old · .new 가 남지 않음 · pip 는 pypkgs 덧씌움 없이 돈다
  4b 옛 파이썬이 있지만 돌지 않을 때 모듈을 못 깖 → 되돌리지 않고 새 환경을 둔다(돌지 않는 것으로 되돌리면 파이썬을 잃는다)
  5 bin/python3 이 없을 때 '환경 복구' → 앱이 스스로 받아 깐다(윈도우와 같다)
  6 python_env 폴더가 통째로 없을 때 '환경 복구' → 그래도 받아 깐다(없는 폴더를 '쓸 수 없음' 으로 잘못 보지 않는다)
  7 교체 한가운데서 앱이 꺼진 자리(.old + 덜 깔린 환경 + .new) → 다음 실행에 옛 환경이 제자리로
  8 지우다 끊긴 .old(파이썬 없음) → 다음 실행에 치운다 · 지금 환경은 그대로
★ 왜 — 예전 맥은 압축을 풀기 전에 python_env 를 지워, 실패하면 파이썬이 통째로 사라져 모든 수집이 멈췄다.
"""
from __future__ import annotations

import io
import json
import shutil
import sys
import tarfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import _common as C  # noqa: E402


def make_tar(path: Path, script: str) -> None:
    """python/bin/python3 하나를 담은 꾸러미(앱은 맨 위 폴더를 벗겨 푼다)."""
    data = script.encode()
    with tarfile.open(path, "w:gz") as t:
        for d in ("python", "python/bin", "python/lib"):
            ti = tarfile.TarInfo(d)
            ti.type = tarfile.DIRTYPE
            ti.mode = 0o755
            t.addfile(ti)
        ti = tarfile.TarInfo("python/bin/python3")
        ti.size = len(data)
        ti.mode = 0o755
        t.addfile(ti, io.BytesIO(data))


GOOD = """#!/bin/sh
case "$*" in
  *--version*) echo "Python 3.99.1" ;;
  *) ;;
esac
exit 0
"""
PIP_FAILS = """#!/bin/sh
case "$*" in
  *--version*) echo "Python 3.99.2" ;;
  *"pip install"*) echo "no network" >&2; exit 1 ;;
  *) ;;
esac
exit 0
"""
PIP_FAILS_LATER = """#!/bin/sh
case "$*" in
  *--version*) echo "Python 3.99.3" ;;
  *"pip install"*) n=$(cat "%(count)s" 2>/dev/null || echo 0); n=$((n+1)); echo "$n" > "%(count)s"
                   [ "$n" -lt 3 ] || { echo "no network" >&2; exit 1; } ;;
  *) ;;
esac
exit 0
"""
IMPORT_FAILS = """#!/bin/sh
case "$*" in
  *--version*) echo "Python 3.99.4" ;;
  *"import lxml"*) exit 1 ;;
  *) ;;
esac
exit 0
"""
GOOD_LOGGING = """#!/bin/sh
case "$*" in
  *--version*) echo "Python 3.99.1" ;;
  *"pip install"*) echo "PYTHONPATH=$PYTHONPATH" >> "%(log)s" ;;
  *) ;;
esac
exit 0
"""
NOT_RUNNING = "#!/bin/sh\nexit 3\n"


def main() -> int:
    check = C.Checks()
    work = C.work_dir("python_swap")
    tarball = work / "cpython-test.tar.gz"
    tarball.write_bytes(b"this is not a tar file")
    try:
        env_arg = "HANISHIKI_TEST_PY_TARBALL=%s" % tarball
        if not C.iso_start("--env", env_arg):
            return 2
        found = sorted(C.ISO.glob("**/*.app/Contents/Resources/python_env"))
        if not found:
            check("격리 사본의 python_env 를 찾았다", False)
            return check.summary()
        res = found[0].parent
        env = res / "python_env"
        new, old, dead = res / "python_env.new", res / "python_env.old", res / "python_env.dead"
        real_py = env / "bin" / "python3"
        check("시험 전 — 옛 환경에 진짜 파이썬이 있다", real_py.exists())
        mark = env / ".akashi_old_env"
        mark.write_text("옛 환경", encoding="utf-8")

        def upgrade(fn: str = "upgradePython", secs: float = 420.0) -> str:
            """앱의 upgradePython 을 부르고 끝날 때의 상태 글을 돌려준다."""
            with C.page() as p:
                p.eval("""(function(){ window.__akashiPy = []; if (!window.__akashiPyHook) { window.__akashiPyHook = 1;
                          var o = window.setPythonEnvBusy; window.setPythonEnvBusy = function(b, s){ window.__akashiPy.push([b, s]); return o && o.apply(this, arguments); }; }
                          return true; })()""")
                p.eval("backend.%s()" % fn)
                t0 = time.time()
                while time.time() - t0 < secs:
                    seen = p.eval("JSON.stringify(window.__akashiPy)", retry=True)
                    arr = json.loads(seen or "[]")
                    ends = [x for x in arr if x and x[0] is False]
                    if ends:
                        time.sleep(1)
                        return str(ends[-1][1])
                    time.sleep(1)
            return "(시간 초과)"

        st = upgrade()
        check("1 깨진 꾸러미 — 압축 해제 실패로 멈춘다", "압축 해제 실패" in st, st)
        check("1 옛 환경 그대로 · .new 가 남지 않음", mark.exists() and real_py.exists() and not new.exists() and not old.exists())

        make_tar(tarball, NOT_RUNNING)
        st = upgrade()
        check("2 돌지 않는 파이썬 — 교체하지 않는다", "기존 유지" in st, st)
        check("2 옛 환경 그대로 · .new 가 남지 않음", mark.exists() and real_py.exists() and not new.exists() and not old.exists())

        make_tar(tarball, PIP_FAILS)
        st = upgrade()
        check("3 모듈을 하나도 못 깔면 — 옛 환경으로 되돌린다", "기존 복원" in st, st)
        check("3 옛 환경이 제자리에 · .new · .old 가 남지 않음", mark.exists() and real_py.exists() and not new.exists() and not old.exists())

        count = work / "pip_count"
        count.unlink(missing_ok=True)
        make_tar(tarball, PIP_FAILS_LATER % {"count": count})
        st = upgrade()
        n = int(count.read_text().strip() or 0) if count.exists() else 0
        check("3b 몇 개만 깔고 끊겨도 — 옛 환경으로 되돌린다", "기존 복원" in st and n >= 3, (st, n))
        check("3b 옛 환경이 제자리에 · .new · .old 가 남지 않음", mark.exists() and real_py.exists() and not new.exists() and not old.exists())

        make_tar(tarball, IMPORT_FAILS)
        st = upgrade()
        check("3c 불러지지 않으면 — 옛 환경으로 되돌린다", "기존 복원" in st, st)
        check("3c 옛 환경이 제자리에 · .new · .old 가 남지 않음", mark.exists() and real_py.exists() and not new.exists() and not old.exists())

        overlay = C.ISO / "home" / "Library" / "Application Support" / "Hanishiki" / "pypkgs"
        overlay.mkdir(parents=True, exist_ok=True)
        piplog = work / "pip_env.log"
        piplog.unlink(missing_ok=True)
        make_tar(tarball, GOOD_LOGGING % {"log": piplog})
        st = upgrade()
        check("4 정상 — 끝났다", st == "완료", st)
        lines = piplog.read_text(encoding="utf-8").splitlines() if piplog.exists() else []
        check("4 pip 는 pypkgs 덧씌움 없이 돈다(있으면 'already satisfied' 로 새 환경에 깔지 않는다)",
              bool(lines) and not any("pypkgs" in x for x in lines), lines[:2])
        check("4 새 환경으로 바뀌었다(옛 표시가 없다) · .new · .old 가 남지 않음", not mark.exists() and (env / "bin" / "python3").exists()
              and (env / ".bundled_ok").exists() and not new.exists() and not old.exists())

        (env / "bin" / "python3").write_text(NOT_RUNNING, encoding="utf-8")   # 파일은 있지만 돌지 않는 옛 파이썬
        make_tar(tarball, PIP_FAILS)
        st = upgrade()
        check("4b 돌지 않는 옛 파이썬 — 되돌리지 않고 새 환경을 둔다", st == "완료" and "3.99.2" in (env / "bin" / "python3").read_text(encoding="utf-8")
              and not old.exists() and not new.exists(), st)

        (env / "bin" / "python3").unlink()
        make_tar(tarball, GOOD)
        st = upgrade("repairPython")
        check("5 파이썬이 없을 때 복구 — 앱이 스스로 받아 깐다", st == "완료" and (env / "bin" / "python3").exists(), st)

        shutil.rmtree(env)
        st = upgrade("repairPython")
        check("6 폴더가 통째로 없어도 복구 — 앱이 스스로 받아 깐다", st == "완료" and (env / "bin" / "python3").exists() and (env / ".bundled_ok").exists(), st)
        check("6 .new · .old · .dead 가 남지 않음", not new.exists() and not old.exists() and not dead.exists())

        # 교체 한가운데서 꺼진 자리를 만들고 다시 켠다
        C.iso_stop(wipe=False)
        env.rename(old)
        mark.parent.mkdir(parents=True)                 # 지금 python_env = 덜 깔린 새 환경
        (env / "half_installed").write_text("x", encoding="utf-8")
        (old / ".akashi_old_env").write_text("옛 환경", encoding="utf-8")
        new.mkdir()
        (new / "junk").write_text("x", encoding="utf-8")
        check("7 앱을 다시 켰다", C.iso_start("--env", env_arg, "--keep-app", wipe_first=False))
        check("7 옛 환경이 제자리로 · 덜 깔린 것은 지움", mark.exists() and (env / "bin" / "python3").exists() and not (env / "half_installed").exists(),
              sorted(x.name for x in env.iterdir()) if env.exists() else "python_env 없음")
        check("7 .old · .new · .dead 가 남지 않음", not old.exists() and not new.exists() and not dead.exists())

        C.iso_stop(wipe=False)
        (old / "lib").mkdir(parents=True)                # 파이썬은 이미 지워지고 나머지가 남은 .old
        (old / "lib" / "leftover").write_text("x", encoding="utf-8")
        check("8 앱을 다시 켰다", C.iso_start("--env", env_arg, "--keep-app", wipe_first=False))
        check("8 파이썬 없는 .old 는 치우고 지금 환경은 그대로", not old.exists() and mark.exists() and (env / "bin" / "python3").exists())
    except Exception as e:  # noqa: BLE001
        check("시험 진행 중 오류 없음", False, "%s: %s" % (type(e).__name__, e))
    finally:
        C.keep_log("python_swap_app.log")
        C.iso_stop()
    return check.summary()


if __name__ == "__main__":
    sys.exit(main())
