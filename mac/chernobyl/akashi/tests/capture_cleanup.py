#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""캡처 Chrome 정리 시험 — '이 앱 데이터 폴더의 것' 만 끄는가. 격리 사본 + 가짜 프로세스(진짜 Chrome 은 띄우지 않는다).

    python3 akashi/tests/capture_cleanup.py                 # 혼자(약 1분)
    python3 akashi/tests/run_all.py --app <옛 판.app> --only capture_cleanup   # 옛 판이 실패하는지(되돌림 확인)

가짜 = `python3 -c sleep` 에 `--user-data-dir=…` 인자만 붙인 것. ps 에는 캡처 Chrome 처럼 보인다.
  ① 앱 시작 때 정리   ② 설정 → 유지보수 → 디버그의 '좀비 Chrome 정리'(killZombieChromes)   ③ 진단의 캡처 Chrome 수
  남아야 하는 것: 다른 홈(다른 사본)의 캡처 · 트랙 프로필, 이름만 비슷한 …chrome_capture_profileX,
                  다른 홈의 사라진 폴더, 같은 홈의 살아 있는 다른 폴더
  꺼져야 하는 것: 이 사본의 chrome_capture_profile · _pen · _<트랙>, 같은 홈의 사라진 옛 폴더(앱 이름이 바뀐 뒤의 고아) 둘

★ 왜 이 시험이 있나
  r37 까지 앱은 켜질 때 `pkill -f chrome_capture_profile`(이름만)로 정리해서, 시험용 사본이 켜지는 순간 사용자 앱이
  수집 중이던 Chrome 까지 껐다. 0aa09aa 부터 --user-data-dir 전체 경로로만 고른다(Common::killCaptureChromes ·
  killOrphanedCaptureChromes). r37 빌드로 돌리면 '남아야 하는 것' 다섯이 모두 꺼져 실패한다 — 그것이 이 시험의 눈금이다.
★ 왜 사본을 두 번 띄우나
  사본의 데이터 폴더 경로는 띄워 봐야 안다(격리 홈 · 임시 폴더 경로). 한 번 띄워 경로를 읽고 홈은 둔 채 끈 뒤,
  그 경로를 쓰는 가짜들을 세워 놓고 다시 띄운다 — 두 번째 시작의 정리가 시험 대상이다.
"""
from __future__ import annotations

import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _common as C  # noqa: E402
from lib.cdp import Page  # noqa: E402


def main() -> int:
    check = C.Checks()
    procs = []

    def fake(profile: Path):
        p = subprocess.Popen(['/usr/bin/python3', '-c', 'import time; time.sleep(900)', '--user-data-dir=' + str(profile)],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        procs.append(p)
        return p

    def alive(p):
        return p.poll() is None

    user_chrome = C.listen_pids(C.USER_CAPTURE_PORT)
    print('[사용자 앱 Chrome %d] %s' % (C.USER_CAPTURE_PORT, user_chrome or '없음'))
    other = C.work_dir('other_copy_home') / 'Library' / 'Application Support' / 'Hanishiki'
    try:
        if not C.iso_start():
            return 2
        own = Path(C.iso_state()['home']) / 'Library' / 'Application Support' / 'Hanishiki'
        print('   사본 데이터 폴더 끝:', '/'.join(own.parts[-4:]))
        check('사본 데이터 폴더가 생겼다', own.is_dir())
        C.iso_stop(wipe=False)                                    # 홈은 두고 끈다
        time.sleep(1)

        live_other = own.parent / 'LiveOther'
        live_other.mkdir(parents=True, exist_ok=True)
        check('고아 시험 전제: 옛 폴더들은 없다', not (own.parent / 'Predormition').exists() and not (own.parent / 'Miyo').exists())
        keep = {'다른 사본 캡처': fake(other / 'chrome_capture_profile'),
                '다른 사본 트랙': fake(other / 'chrome_capture_profile_Zm9v'),
                '이름만 비슷한 것': fake(own / 'chrome_capture_profileX'),
                '다른 홈의 사라진 폴더': fake(other.parent / 'GoneApp' / 'chrome_capture_profile'),
                '같은 홈의 살아 있는 다른 폴더': fake(live_other / 'chrome_capture_profile')}
        kill = {'이 사본 캡처': fake(own / 'chrome_capture_profile'),
                '이 사본 PEN': fake(own / 'chrome_capture_profile_pen'),
                '이 사본 트랙': fake(own / 'chrome_capture_profile_dHdpdHRlciMw'),
                '사라진 옛 폴더의 고아(트랙)': fake(own.parent / 'Predormition' / 'chrome_capture_profile_Zm9v'),
                '사라진 옛 폴더의 고아(Miyo/…)': fake(own.parent / 'Miyo' / 'Chernobyl' / 'chrome_capture_profile')}
        time.sleep(0.5)
        check('가짜 프로세스 %d개가 떠 있다' % len(procs), all(alive(p) for p in procs))

        # 두 번째 시작이 시험 대상이다 — 못 떠도 '못 돎' 이 아니라 실패로 적고, 가짜들의 판정(Popen 만 본다)은 한다
        started = check('두 번째 시작 — 사본이 떴다(시작 때 정리가 앱을 죽이거나 굳히지 않음)', C.iso_start(wipe_first=False))
        time.sleep(1)
        print('── ① 앱 시작 때 정리')
        for k, p in keep.items():
            check('남음: ' + k, alive(p))
        for k, p in kill.items():
            check('꺼짐: ' + k, not alive(p))
        if not started:
            raise RuntimeError('사본이 뜨지 않아 ②·③ 을 건너뜀')

        print('── ② 좀비 Chrome 정리 단추 · ③ 진단 수')
        kill2 = {'이 사본 캡처(2)': fake(own / 'chrome_capture_profile'),
                 '이 사본 PEN(2)': fake(own / 'chrome_capture_profile_pen')}
        time.sleep(0.5)
        with Page.attach(C.PORT) as pg:
            C.watch_errors(pg)
            time.sleep(1)
            pg.eval("window.__akashiDiag=''; window.__akashiOnDiag=window.onDiagInfo;"
                    "window.onDiagInfo=function(t){window.__akashiDiag=String(t)}; backend.getDiagnosticInfo()")
            t0 = time.time()
            diag = ''
            while time.time() - t0 < 15 and not diag:
                diag = pg.eval("window.__akashiDiag") or ''
                time.sleep(0.5)
            pg.eval("window.onDiagInfo=window.__akashiOnDiag; delete window.__akashiOnDiag; delete window.__akashiDiag")
            line = next((ln for ln in diag.splitlines() if '캡쳐 Chrome' in ln), '')
            check('진단 수 = 이 사본 것 2개(다른 사본 · 비슷한 이름 · 살아 있는 다른 폴더 빼고)', line.endswith(': 2개'), line)
            if not C.scoped_cleanup(C.test_app()) and (C.listen_pids(C.USER_CAPTURE_PORT) or C.foreign_capture_chromes()):
                # 옛 판의 단추는 이름으로 끈다 — 그 사이 사용자 앱이 수집을 시작했으면 누르지 않는다
                check('② 좀비 Chrome 정리 단추 — 옛 판인데 사용자 캡처 Chrome 이 떠 있어 누르지 않음', False)
            else:
                pg.eval("backend.killZombieChromes()")
                time.sleep(2.5)
            errs = C.page_errors(pg)
            check('화면 JS 오류 없음', not errs, errs[:2])
        for k, p in kill2.items():
            check('꺼짐: ' + k, not alive(p))
        for k, p in keep.items():
            check('여전히 남음: ' + k, alive(p))
        check('앱(사본)은 살아 있다', C.iso_alive())
    except Exception as e:  # noqa: BLE001
        check('시험 진행 중 오류 없음', False, '%s: %s' % (type(e).__name__, e))
    finally:
        for p in procs:                                           # 우리가 띄운 가짜만, Popen 으로(이름으로 찾지 않는다)
            if alive(p):
                p.kill()
        C.keep_log('capture_cleanup_app.log')
        C.iso_stop()
        shutil.rmtree(C.work_dir('other_copy_home'), ignore_errors=True)
    check('사용자 앱의 Chrome(%d)은 그대로' % C.USER_CAPTURE_PORT, C.listen_pids(C.USER_CAPTURE_PORT) == user_chrome,
          C.listen_pids(C.USER_CAPTURE_PORT))
    return check.summary()


if __name__ == '__main__':
    sys.exit(main())
