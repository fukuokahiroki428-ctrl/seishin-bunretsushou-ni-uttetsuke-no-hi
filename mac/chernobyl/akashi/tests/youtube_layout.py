#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""유튜브 · 니코동 '저장 방식' 시험 — 채널별 폴더 / 폴더 없이 바로. 격리 사본 + 이 맥 안의 시험 영상(인터넷으로 안 나감).

    python3 akashi/tests/youtube_layout.py     # 혼자(약 1분)

한 저장 폴더에 영상 둘을 — 첫째는 '채널별 폴더', 둘째는 '폴더 없이 바로' 로(화면에서 고르고 '다운로드' 를 눌러,
startYoutube 가 saveLayout 을 싣는지까지). 판마다 앱의 진짜 끝 신호(onCollectionEnded)를 기다린다.
  1 '저장 방식' 칸이 있고 기본은 '채널별 폴더'
  2 채널별 폴더 — 첫째 영상이 <유형>/<채널>/ 아래(예전과 같음)
  3 폴더 없이 — 둘째 영상이 <유형>/ 바로 아래에 '<날짜>_<채널>_<제목> [<ID>]' 이름으로, info.json 이 곁에
  4 _complete 사본은 파일마다 제 모양으로 — 첫째는 _complete/<채널>/, 둘째는 _complete/ 바로 아래, **모두 두 개뿐**
    (저장 방식을 바꾼 판이 지난 영상을 한 벌 더 복사하지 않는다)
  5 엑셀이 만들어지고, 화면 JS 오류 없음
  6 니코동 탭에도 같은 칸 — '폴더 없이 바로' 로 받으면 <유형>/ 바로 아래 · _complete 도 바로 아래

★ 왜 이름에 채널과 ID 를 남기나 — 폴더 없이 두면 다른 채널의 같은 날 · 같은 제목 영상이 같은 이름이 된다. 그러면 yt-dlp 는
  둘째를 '이미 받음' 으로 건너뛰고 장부에까지 적어 다시는 받지 않는다(2026-10-06 실측: 두 영상 → 파일 하나 · 장부 두 줄).
  NAS(255바이트)를 위해 자른 두 제목이 같아져도 ID 가 갈라 준다.
★ 왜 'Done' 이 아니라 끝 신호를 기다리나 — 앱은 'Done' 을 먼저 띄우고 그 뒤에 EXIF · Finder 설명 · _complete 사본 · 엑셀을
  쓴다. 'Done' 만 보고 다음 판을 시작하면 앞 판의 끝 처리가 뒤 판을 '중지됨' 으로 끊을 수 있다(리뷰에서 잡힘).
"""
from __future__ import annotations

import json
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _common as C  # noqa: E402
import fixtures  # noqa: E402

MEDIA = ('.mp4', '.mkv', '.webm')
DONE = ('완료', 'Done', '중단됨', '중지됨', '오류', 'Error', 'Ready', '대기')


def main() -> int:
    check = C.Checks()
    app = C.test_app()
    ffmpeg = (app / 'Contents' / 'MacOS' / 'ffmpeg') if app else None
    if not ffmpeg or not ffmpeg.exists():
        print('번들 ffmpeg 가 없습니다 — 빌드부터')
        return 2
    import subprocess
    www = C.work_dir('www_youtube_layout')
    for name, src in (('clip1.mp4', 'testsrc'), ('clip2.mp4', 'testsrc2')):
        if not (www / name).exists():
            subprocess.run([str(ffmpeg), '-loglevel', 'error', '-f', 'lavfi', '-i', src + '=duration=1:size=160x120:rate=10',
                            '-c:v', 'libx264', '-pix_fmt', 'yuv420p', str(www / name)], check=True)
    srv, hp = fixtures.serve(www)
    try:
        if not C.iso_start():
            return 2
        with C.page() as p:
            time.sleep(1.5)
            p.eval("switchTab('youtube')")
            time.sleep(0.6)
            opts = p.eval("(function(){var s=document.getElementById('youtube-save-layout');"
                          "return s ? {value:s.value, options:[...s.options].map(o=>o.value)} : null})()")
            check("'저장 방식' 칸 — 기본은 채널별 폴더", bool(opts) and opts.get('value') == 'channel'
                  and opts.get('options') == ['channel', 'flat'], opts)
            p.eval("(function(){if(window.__akashiYt)return;window.__akashiYt=1;var o=window.onCollectionEnded;"
                   "window.__akashiYtOrig=o;window.__akashiEnded={};window.onCollectionEnded=function(x){window.__akashiEnded[x]=true;return o&&o.apply(this,arguments)};})()")
            base = C.ISO / 'tmp' / 'yt_switch'
            for layout, clip in (('channel', 'clip1.mp4'), ('flat', 'clip2.mp4')):
                p.eval("""(function(){var g=function(i){return document.getElementById(i)};
                  g('youtube-url').value=%s; g('youtube-path').value=%s; g('youtube-type').value='video';
                  g('youtube-pace').value='fast'; g('youtube-save-layout').value=%s;
                  ['youtube-subs','youtube-thumb','youtube-playlist','youtube-sponsor','youtube-comments','youtube-real-capture']
                    .forEach(function(i){var e=g(i); if(e) e.checked=false});
                  window.__akashiEnded.youtube=false; startYoutube();})()""" % (json.dumps('http://127.0.0.1:%d/%s' % (hp, clip)),
                                                                         json.dumps(str(base)), json.dumps(layout)))
                t0 = time.time()
                while not p.eval("!!window.__akashiEnded.youtube") and time.time() - t0 < 180:
                    time.sleep(0.5)
                st = C.platform_stats(p, 'youtube')
                ended = bool(p.eval("!!window.__akashiEnded.youtube"))
                check('%s 판이 끝났다(끝 신호)' % layout, ended and str(st.get('status')) in ('Done', '완료'),
                      '%s · %.0fs' % (st.get('status'), time.time() - t0))
            # ── 니코동 탭 — 같은 '저장 방식' 칸, 폴더 없이 바로
            p.eval("switchTab('niconico')")
            time.sleep(0.6)
            nopts = p.eval("(function(){var s=document.getElementById('niconico-save-layout');"
                           "return s ? {value:s.value, options:[...s.options].map(o=>o.value)} : null})()")
            check("니코동에도 '저장 방식' 칸 — 기본은 채널별 폴더", bool(nopts) and nopts.get('value') == 'channel'
                  and nopts.get('options') == ['channel', 'flat'], nopts)
            nbase = C.ISO / 'tmp' / 'nico_flat'
            p.eval("""(function(){var g=function(i){return document.getElementById(i)};
              g('niconico-url').value=%s; g('niconico-path').value=%s; g('niconico-type').value='video';
              g('niconico-save-layout').value='flat';
              ['niconico-thumb','niconico-comments','niconico-cookies','niconico-real-capture']
                .forEach(function(i){var e=g(i); if(e) e.checked=false});
              window.__akashiEnded.niconico=false; startNiconico();})()""" % (json.dumps('http://127.0.0.1:%d/clip1.mp4' % hp),
                                                                             json.dumps(str(nbase))))
            t0 = time.time()
            while not p.eval("!!window.__akashiEnded.niconico") and time.time() - t0 < 180:
                time.sleep(0.5)
            nst = C.platform_stats(p, 'niconico')
            check('니코동 폴더 없이 판이 끝났다(끝 신호)', bool(p.eval("!!window.__akashiEnded.niconico"))
                  and str(nst.get('status')) in ('Done', '완료'), '%s · %.0fs' % (nst.get('status'), time.time() - t0))
            p.eval("document.getElementById('niconico-save-layout').value='channel'")
            # 되돌리기 — 가로챈 함수와 칸 값
            p.eval("document.getElementById('youtube-save-layout').value='channel';"
                   "if(window.__akashiYt){window.onCollectionEnded=window.__akashiYtOrig;"
                   "delete window.__akashiYt;delete window.__akashiYtOrig;delete window.__akashiEnded;}")
            errs = C.page_errors(p)
            check('화면 JS 오류 없음', not errs, errs[:2])

        def media_in(d: Path):
            return sorted(x for x in d.rglob('*') if x.is_file() and x.suffix.lower() in MEDIA) if d.exists() else []

        yt = base / 'youtube'
        vids = media_in(yt / 'video')
        rel = [str(v.relative_to(yt)) for v in vids]
        sub = [v for v in vids if v.parent.parent == yt / 'video']
        flat = [v for v in vids if v.parent == yt / 'video']
        check('채널별 폴더 — 첫째 영상이 <유형>/<채널>/ 아래', len(sub) == 1 and 'clip1' in sub[0].name, rel)
        check('폴더 없이 — 둘째 영상이 <유형>/ 바로 아래', len(flat) == 1 and 'clip2' in flat[0].name, rel)
        if flat:
            import re
            check("폴더 없이 — 이름이 '<날짜>_<채널>_<제목> [<ID>]'", bool(re.match(r'^[^_]+_[^_]+_.+ \[[^\]]+\]$', flat[0].stem)), flat[0].name)
            check('폴더 없이 — info.json 이 영상 곁에', (yt / 'video' / (flat[0].stem + '.info.json')).exists())
        comp = media_in(yt / '_complete')
        crel = [str(v.relative_to(yt)) for v in comp]
        check('_complete — 첫째는 <채널>/ 아래, 둘째는 바로 아래, 모두 두 개뿐(지난 영상을 한 벌 더 복사하지 않음)',
              len(comp) == 2 and sum(1 for v in comp if v.parent == yt / '_complete') == 1
              and sum(1 for v in comp if v.parent.parent == yt / '_complete') == 1, crel)
        check('엑셀이 만들어졌다(후처리가 두 모양 모두에서 영상을 찾음)', any((yt / 'excel').glob('*.xlsx')),
              sorted(x.name for x in (yt / 'excel').glob('*')))
        nico = nbase / 'niconico'
        nv = media_in(nico / 'video')
        check("니코동 폴더 없이 — 영상이 <유형>/ 바로 아래, '<날짜>_<채널>_<제목> [<ID>]'",
              len(nv) == 1 and nv[0].parent == nico / 'video' and nv[0].stem.endswith(']'), [str(v.relative_to(nico)) for v in nv])
        nc = media_in(nico / '_complete')
        check('니코동 폴더 없이 — _complete 사본도 바로 아래', len(nc) == 1 and nc[0].parent == nico / '_complete',
              [str(v.relative_to(nico)) for v in nc])
    except Exception as e:  # noqa: BLE001
        check('시험 진행 중 오류 없음', False, '%s: %s' % (type(e).__name__, e))
    finally:
        C.keep_log('youtube_layout_app.log')
        C.iso_stop()
        srv.shutdown()
    return check.summary()


if __name__ == '__main__':
    sys.exit(main())
