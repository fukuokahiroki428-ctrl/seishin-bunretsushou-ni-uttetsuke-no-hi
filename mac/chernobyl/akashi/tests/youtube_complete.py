#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""유튜브 · 니코동 '끝나면 _complete 만 남기기' 시험 — 격리 사본 + 이 맥 안의 시험 영상(인터넷으로 안 나감).

    python3 akashi/tests/youtube_complete.py     # 혼자(약 3분)

판마다 화면에서 고르고 '다운로드' 를 눌러(startYoutube · startNiconico 가 completeOnly 를 싣는지까지) 앱의 진짜 끝 신호를 기다린다.
  화면  두 탭의 칸 · 기본은 꺼짐 · 저장되는 칸 · 内閣会 니코동 옵션 · 칸을 눌러 켤 때 묻고 '아니오' 면 꺼진 채(두 탭 모두)
  A  옵션 없이 여섯 개(clip1 · clip3 · clip4 · clip5 · clip6 · 3MB 넘는 big) — 예전처럼 원본 + _complete 사본
  B  옵션을 켜고 다른 영상 하나 — 예전 것까지 받은 폴더를 비운다
     · 원본 · info.json · 받다 남은 조각(.part · .ytdl · .f137.mp4 · .temp.mp4 · -Frag)은 지워지고
     · 자막 · 댓글 · 설명 · 썸네일 그림은 _complete 로 옮겨지고(clip6 — 사본이 원본과 같다)
     · 크기는 같고 앞(clip1) · 뒤 1MB 안(big)이 다른 _complete 사본은 덮지 않고 그대로 — 원본은 '<이름> [<번호>]' 로 따로 놓이고
     · 같은 이름으로 시작하는 '다른 영상' 의 파일(info.json 이 있는 'NAME.f2' 의 받던 조각 · info.json 없는 'NAME.final.mp4')은 남고
     · 정리 장부에 있는데 원본만 없어진 것(지난 정리가 끊김, clip3)은 마저 치우고, 장부에 없는 것(사용자가 원본만 지움, clip5)은 둔다
     · 비게 된 채널 폴더는 걷히고 · 장부(yt-dlp · 정리)에 영상들이 있다
     · 형태별 엑셀은 사용자가 손으로 지운 영상(clip4)의 줄을 예전처럼 빼고, complete 엑셀은 남긴다
  C  같은 주소를 다시 — 다시 받지 않는다(정리 줄 없음) · 정리한 영상의 엑셀 줄은 남는다 · 그 사이 다시 누른 시작은 막힌다(한 판만)
  G  읽을 수 없는 엑셀 — 덮지 않고, 그 판은 받은 폴더를 비우지 않는다. 엑셀을 되돌린 다음 판에 정리된다
  X·Y  같은 채널 · 같은 날 · 같은 제목의 다른 영상 둘을 차례로 — 앞 영상의 하나뿐인 사본을 덮지 않고 뒤 것은 '[번호]' 로
  D  썸네일만 받기 + 옵션 — 그림이 _complete 로 가고 받은 폴더는 빈다 · 정리 장부에 남는다 · complete 엑셀에 모든 형태
  E  같은 주소를 썸네일만으로 다시 — 다시 받지 않는다(이번 판 장부에 정리 장부가 더해진다 · 정리 줄도 엑셀 줄도 없음)
  H  동영상 + '썸네일' 로 같은 채널 · 날 · 제목 — 그 썸네일(곁 파일)이 썸네일만 받은 판의 하나뿐인 그림을 덮지 않고 '[번호]' 로
  S  시작하자마자 중지 — 띄우기 전에 누른 중지로 스크립트를 띄우지 않고, 곧바로 다시 누른 시작은 막히지 않는다
  F  니코동 탭 — 폴더 없이 바로 + 옵션이면 niconico/video 가 비고 _complete 바로 아래에 영상
"""
from __future__ import annotations

import hashlib
import json
import shutil
import subprocess
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import _common as C  # noqa: E402
import fixtures  # noqa: E402

MEDIA = ('.mp4', '.mkv', '.webm')


def sha(p: Path) -> str:
    return hashlib.sha256(p.read_bytes()).hexdigest()


def files_in(d: Path, suffixes=None):
    if not d.exists():
        return []
    return sorted(x for x in d.rglob('*') if x.is_file() and (suffixes is None or x.suffix.lower() in suffixes))


def where_from(p: Path) -> str:
    out = subprocess.run(['/usr/bin/xattr', '-p', 'com.apple.metadata:kMDItemWhereFroms', str(p)], capture_output=True, text=True)
    return out.stdout if out.returncode == 0 else ''


def poke(p: Path, offset: int) -> None:
    """같은 크기로 한 바이트를 바꾼다(offset 이 음수면 끝에서)"""
    with open(p, 'r+b') as f:
        f.seek(offset if offset >= 0 else p.stat().st_size + offset)
        b = f.read(1)
        f.seek(-1, 1)
        f.write(bytes([b[0] ^ 0xFF]))


def main() -> int:
    check = C.Checks()
    app = C.test_app()
    ffmpeg = (app / 'Contents' / 'MacOS' / 'ffmpeg') if app else None
    py = (app / 'Contents' / 'Resources' / 'python_env' / 'bin' / 'python3') if app else None
    if not ffmpeg or not ffmpeg.exists() or not py or not py.exists():
        print('번들 ffmpeg · 파이썬이 없습니다 — 빌드부터')
        return 2

    def xlsx_ids(path: Path):
        if not path.exists():
            return set()
        code = ("import openpyxl,sys,json;wb=openpyxl.load_workbook(sys.argv[1],read_only=True);ws=wb.worksheets[0];"
                "print(json.dumps([str(r[0]) for r in ws.iter_rows(min_row=2,max_col=1,values_only=True) if r and r[0] is not None]))")
        out = subprocess.run([str(py), '-I', '-B', '-c', code, str(path)], capture_output=True, text=True)
        try:
            return set(json.loads(out.stdout or '[]'))
        except ValueError:
            print('   xlsx read failed:', out.stderr[-300:])
            return set()

    www = C.work_dir('www_youtube_complete')
    clips = {'clip1.mp4': ('testsrc', '160x120', 1), 'clip2.mp4': ('testsrc2', '160x120', 1), 'clip3.mp4': ('testsrc', '176x144', 1),
             'clip4.mp4': ('testsrc2', '176x144', 1), 'clip5.mp4': ('testsrc', '192x144', 1), 'clip6.mp4': ('testsrc2', '192x144', 1),
             'clip7.mp4': ('testsrc', '208x160', 1), 'clip8.mp4': ('testsrc2', '208x160', 1)}
    for name, (src, size, dur) in clips.items():
        if not (www / name).exists():
            subprocess.run([str(ffmpeg), '-loglevel', 'error', '-f', 'lavfi', '-i', '%s=duration=%d:size=%s:rate=10' % (src, dur, size),
                            '-c:v', 'libx264', '-pix_fmt', 'yuv420p', str(www / name)], check=True)
    if not (www / 'big.mp4').exists() or (www / 'big.mp4').stat().st_size < 3_000_000:   # 3MB 넘게 — 앞 1MB · 뒤 1MB 비교가 따로 돈다
        subprocess.run([str(ffmpeg), '-y', '-loglevel', 'error', '-f', 'lavfi', '-i', 'testsrc2=duration=6:size=1280x720:rate=10',
                        '-c:v', 'libx264', '-preset', 'ultrafast', '-crf', '0', '-pix_fmt', 'yuv420p', str(www / 'big.mp4')], check=True)
    if not (www / 'thumb.jpg').exists():
        subprocess.run([str(ffmpeg), '-loglevel', 'error', '-f', 'lavfi', '-i', 'color=c=red:s=32x32', '-frames:v', '1',
                        str(www / 'thumb.jpg')], check=True)
    big_size = (www / 'big.mp4').stat().st_size
    srv, hp = fixtures.serve(www)
    url = lambda n: 'http://127.0.0.1:%d/%s' % (hp, n)  # noqa: E731

    def page_with(name, title, author, content, thumb=None):
        ld = {"@context": "https://schema.org", "@type": "VideoObject", "name": title,
              "contentUrl": url(content), "uploadDate": "2026-01-02T00:00:00+00:00",
              "author": {"@type": "Person", "name": author}}
        if thumb:
            ld["thumbnailUrl"] = url(thumb)
        (www / name).write_text('<!doctype html><html><head><title>%s</title><script type="application/ld+json">%s'
                                '</script></head><body>v</body></html>' % (title, json.dumps(ld)), encoding='utf-8')
    page_with('studio.html', 's1', 'Studio Inc.', 'clip2.mp4')
    page_with('thumbpage.html', 't1', 'Thumb Ch', 'clip1.mp4', thumb='thumb.jpg')
    page_with('coll1.html', 'same', 'Coll Ch', 'clip3.mp4')     # 같은 채널 · 같은 날 · 같은 제목의 다른 영상 둘
    page_with('coll2.html', 'same', 'Coll Ch', 'clip4.mp4')
    page_with('thumbpage2.html', 't1', 'Thumb Ch', 'clip5.mp4', thumb='thumb.jpg')   # 썸네일만 받은 t1 과 같은 채널 · 날 · 제목의 동영상

    base = C.ISO / 'tmp' / 'yt_complete'
    yt = base / 'youtube'
    xv_path, xc_path = yt / 'excel' / 'youtube_video.xlsx', yt / 'excel' / 'youtube_complete.xlsx'
    try:
        if not C.iso_start():
            return 2
        with C.page() as p:
            time.sleep(1.5)
            p.eval("switchTab('youtube')")
            time.sleep(0.6)
            # 칸을 실제로 눌러(click → change → onchange) 묻는지 — 두 탭 모두
            ui = p.eval("""(function(){var y=document.getElementById('youtube-complete-only'), n=document.getElementById('niconico-complete-only');
                var r={y:!!y, n:!!n, yOff: !!y && !y.checked, nOff: !!n && !n.checked,
                       saved: _formFieldIds.indexOf('youtube-complete-only')>=0 && _formFieldIds.indexOf('niconico-complete-only')>=0,
                       nk: (NK_OPTION_SCHEMA.niconico||[]).some(function(s){return s.k==='completeOnly'})};
                if (y && n) { var c=window.confirm, asked=0;
                    window.confirm=function(){asked++; return false}; y.click(); n.click(); r.declined=!y.checked && !n.checked && asked===2;
                    window.confirm=function(){asked++; return true}; y.click(); n.click(); r.accepted=y.checked && n.checked && asked===4;
                    window.confirm=c; y.checked=false; n.checked=false; }
                return r})()""")
            check("두 탭에 '끝나면 _complete 만 남기기' 칸 — 기본은 꺼짐", bool(ui) and ui.get('y') and ui.get('n')
                  and ui.get('yOff') and ui.get('nOff'), ui)
            check('그 칸이 저장되는 칸이고 内閣会 니코동 옵션에도 있다', bool(ui) and ui.get('saved') and ui.get('nk'), ui)
            check("칸을 눌러 켤 때 묻는다 — '아니오' 면 꺼진 채, '예' 면 켜진다(두 탭)", bool(ui) and ui.get('declined') and ui.get('accepted'), ui)
            p.eval("(function(){if(window.__akashiYt)return;window.__akashiYt=1;var o=window.onCollectionEnded;"
                   "window.__akashiYtOrig=o;window.__akashiEnded={};window.onCollectionEnded=function(x){window.__akashiEnded[x]=true;return o&&o.apply(this,arguments)};})()")

            # ★ 기록은 QWebChannel 신호(logSignal)로, 끝 신호는 runJavaScript 로 화면에 온다 — 둘의 차례는 정해져 있지 않다.
            #   실측(2026-11-12): 앞 판의 '📊 Excel 저장' 줄이 다음 판을 시작한 뒤에 왔다. 끝 신호만 보고 기록을 읽으면 끝 처리의
            #   줄(🧹 · 엑셀)이 아직 없다 — 판이 끝나면 기록이 조용해질 때까지, 바라는 줄은 올 때까지 기다린다.
            def log_text(tab):
                return p.eval("(document.getElementById('%s-log')||{}).innerText||''" % tab, retry=True) or ''

            def wait_log(tab, needle, secs=15.0):
                t0, txt = time.time(), ''
                while time.time() - t0 < secs:
                    txt = log_text(tab)
                    if needle in txt:
                        break
                    time.sleep(0.3)
                return txt

            def settle_log(tab, quiet=2.0, secs=20.0):
                t0, last, since = time.time(), None, time.time()
                while time.time() - t0 < secs:
                    txt = log_text(tab)
                    if txt != last:
                        last, since = txt, time.time()
                    elif time.time() - since >= quiet:
                        break
                    time.sleep(0.3)
                return last or ''

            def run_yt(label, names, layout, typ, complete_only, double_start=False, thumb=False, pace='fast', stop_now=False):
                p.eval("""(function(){var g=function(i){return document.getElementById(i)};
                  g('youtube-url').value=%s; g('youtube-path').value=%s; g('youtube-type').value=%s;
                  g('youtube-pace').value=%s; g('youtube-save-layout').value=%s;
                  ['youtube-subs','youtube-thumb','youtube-playlist','youtube-sponsor','youtube-comments','youtube-real-capture']
                    .forEach(function(i){var e=g(i); if(e) e.checked=false});
                  g('youtube-thumb').checked=%s;
                  g('youtube-complete-only').checked=%s;
                  window.__akashiEnded.youtube=false; startYoutube();
                  if (%s) backend.startYoutube(JSON.stringify({url:g('youtube-url').value, path:g('youtube-path').value, type:%s,
                                                              ytPace:'fast', saveLayout:%s, completeOnly:%s}));
                  if (%s) stopYoutube();})()""" % (
                    json.dumps('\n'.join(url(n) for n in names)), json.dumps(str(base)), json.dumps(typ),
                    json.dumps(pace), json.dumps(layout), 'true' if thumb else 'false', 'true' if complete_only else 'false',
                    'true' if double_start else 'false',
                    json.dumps(typ), json.dumps(layout), 'true' if complete_only else 'false', 'true' if stop_now else 'false'))
                t0 = time.time()
                while not p.eval("!!window.__akashiEnded.youtube", retry=True) and time.time() - t0 < 240:
                    time.sleep(0.5)
                st = C.platform_stats(p, 'youtube')
                want = ('중지됨', 'Done', '완료') if stop_now else ('Done', '완료')
                check('%s 판이 끝났다(끝 신호)' % label, bool(p.eval("!!window.__akashiEnded.youtube", retry=True))
                      and str(st.get('status')) in want, '%s · %.0fs' % (st.get('status'), time.time() - t0))
                return settle_log('youtube')

            def rel(xs, root=yt):
                return [str(x.relative_to(root)) for x in xs]

            # ── A 옵션 없이 다섯
            run_yt('A(옵션 없음)', ['clip1.mp4', 'clip3.mp4', 'clip4.mp4', 'clip5.mp4', 'clip6.mp4', 'big.mp4'], 'channel', 'video', False)
            orig = {}
            for n in ('clip1', 'clip3', 'clip4', 'clip5', 'clip6', 'big'):
                vs = [v for v in files_in(yt / 'video', MEDIA) if v.stem.endswith('_' + n)]
                ms = [v for v in files_in(yt / '_complete', MEDIA) if v.stem.endswith('_' + n)]
                orig[n] = (vs[0] if len(vs) == 1 else None, ms[0] if len(ms) == 1 else None)
            check('A — 예전처럼 원본과 _complete 사본이 하나씩(여섯 영상)', all(v and m for v, m in orig.values()),
                  {k: (str(v), str(m)) for k, (v, m) in orig.items()})
            if not all(v and m for v, m in orig.values()):
                raise RuntimeError('A 결과가 없어 이어 갈 수 없음')
            v1, m1 = orig['clip1']
            v6, m6 = orig['clip6']
            d1, stem = v6.parent, v6.stem          # 곁 파일은 사본이 원본과 같은 clip6 에 심는다
            check('A — info.json 이 원본 곁에 · 사본에 받은 곳(이 영상의 주소)', (v1.parent / (v1.stem + '.info.json')).exists()
                  and url('clip1.mp4') in where_from(m1), where_from(m1)[:160])
            check('A — big 은 3MB 를 넘는다(앞 1MB · 뒤 1MB 비교가 따로 돈다)', big_size > 3_000_000, big_size)
            sha_orig = {n: sha(v) for n, (v, _) in orig.items()}
            # B 앞에 심는다 — 크기는 같고 바이트가 다른 사본(앞: clip1 · 뒤 1MB 안: big)
            poke(m1, 100)
            poke(orig['big'][1], -1000)
            poked = {'clip1': sha(m1), 'big': sha(orig['big'][1])}
            plant = {
                'move': [stem + '.en.vtt', stem + '.comments.txt', stem + '.jpg', stem + '.description'],
                'gone': [stem + '.f137.mp4.part', stem + '.temp.mp4',
                         stem + '.f251-drc.webm', stem + '.mp4.ytdl', stem + '.mp4.part-Frag3'],
                'keep': [stem + '.final.mp4', stem + '.f2.info.json', stem + '.f2.mp4.part'],
            }
            for n in plant['move'] + plant['gone'] + plant['keep']:
                q = d1 / n
                if q.exists():
                    continue
                if n.endswith('.jpg'):
                    shutil.copy(www / 'thumb.jpg', q)
                elif n.endswith('.f2.info.json'):
                    q.write_text(json.dumps({"id": "fake-f2", "extractor_key": "Generic", "title": "f2",
                                             "webpage_url": url('f2')}), encoding='utf-8')
                elif n.endswith('.vtt'):
                    q.write_text('WEBVTT\n\n00:00.000 --> 00:01.000\nhi\n', encoding='utf-8')
                else:
                    q.write_bytes(b'x' * 64 + n.encode())
            # clip3 — 지난 정리가 끊긴 모양: 정리 장부에 있고 원본만 없다(곁 파일 · info.json 은 남음). yt-dlp 장부에서는 빼 둔다
            v3, m3 = orig['clip3']
            s3 = v3.stem
            (v3.parent / (s3 + '.jpg')).write_bytes((www / 'thumb.jpg').read_bytes())
            (v3.parent / (s3 + '.en.vtt')).write_text('WEBVTT\n\n', encoding='utf-8')
            v3.unlink()
            ledger = yt / '_complete' / '.yt_cleaned_video.txt'
            ledger.write_text('generic clip3\n', encoding='utf-8')
            arch = yt / '.yt_archive.txt'
            arch.write_text(''.join(l + '\n' for l in arch.read_text(encoding='utf-8').splitlines() if l.strip() != 'generic clip3'),
                            encoding='utf-8')
            # clip4 — 사용자가 원본과 info.json 을 손으로 지움(장부에 없음) · clip5 — 원본만 지움(장부에 없음)
            v4, _ = orig['clip4']
            v4.unlink()
            (v4.parent / (v4.stem + '.info.json')).unlink()
            v5, _ = orig['clip5']
            v5.unlink()

            # ── B 옵션을 켜고 다른 영상
            run_yt('B(옵션 켬)', ['studio.html'], 'channel', 'video', True)
            blog = wait_log('youtube', '🧹 _complete 만 남기기')
            check('B — 기록에 정리 줄(clip1 · clip6 · big · studio · 끊긴 clip3 = 5개)', '🧹 _complete 만 남기기: 5개 정리' in blog, blog[-500:])
            check('B — 예전에 받은 원본 · info.json 이 지워졌다', not v1.exists() and not v6.exists()
                  and not (d1 / (stem + '.info.json')).exists() and not orig['big'][0].exists())
            left = [n for n in plant['gone'] if (d1 / n).exists() or (m6.parent / n).exists()]
            check('B — 받다 남은 조각이 지워졌다(_complete 로도 가지 않음)', not left, left)
            kept = [n for n in plant['keep'] if not (d1 / n).exists()]
            check("B — 같은 이름으로 시작하는 다른 영상의 파일은 남았다('.f2' 의 받던 조각 · '.final.mp4')", not kept, kept)
            mb = orig['big'][1]
            alt1 = m1.parent / (m1.stem + ' [clip1]' + m1.suffix)
            altb = mb.parent / (mb.stem + ' [big]' + mb.suffix)
            check('B — 앞 1MB 가 다른 사본(clip1)은 덮지 않고 그대로, 원본은 [clip1] 로 따로', m1.exists() and sha(m1) == poked['clip1']
                  and alt1.exists() and sha(alt1) == sha_orig['clip1'], rel(files_in(m1.parent, MEDIA)))
            check('B — 뒤 1MB 안이 다른 사본(big · 3MB 넘음)도 덮지 않고, 원본은 [big] 로 따로', sha(mb) == poked['big']
                  and altb.exists() and sha(altb) == sha_orig['big'])
            check("B — 기록에 '[번호]' 로 둔 수(2개)", "2개는 이름 끝에 '[번호]' 를 붙여 둠" in blog, blog[-300:])
            moved = [n for n in plant['move'] if not (m6.parent / n).exists() or (d1 / n).exists()]
            check('B — 자막 · 댓글 · 설명 · 썸네일 그림이 _complete 로 옮겨졌다(사본과 같은 이름 줄기)', not moved, moved)
            check('B — 끊긴 정리(clip3, 정리 장부에 있음)를 마저 치웠다: info.json 은 지우고 썸네일 · 자막은 옮김',
                  not (v3.parent / (s3 + '.info.json')).exists() and (m3.parent / (s3 + '.jpg')).exists()
                  and (m3.parent / (s3 + '.en.vtt')).exists() and not (v3.parent / (s3 + '.jpg')).exists())
            check('B — 정리 장부에 없는 것(clip5, 사용자가 원본만 지움)은 손대지 않았다', (v5.parent / (v5.stem + '.info.json')).exists())
            check("B — 비게 된 채널 폴더('Studio Inc#')는 걷혔다", not (yt / 'video' / 'Studio Inc#').exists(),
                  sorted(x.name for x in (yt / 'video').iterdir()) if (yt / 'video').exists() else [])
            s1 = [v for v in files_in(yt / '_complete', MEDIA) if '_s1' in v.name]
            check('B — 새로 받은 영상은 _complete 에만', len(s1) == 1 and not [v for v in files_in(yt / 'video', MEDIA) if '_s1' in v.name],
                  rel(s1))
            a_txt = arch.read_text(encoding='utf-8')
            l_txt = ledger.read_text(encoding='utf-8') if ledger.exists() else ''
            check('B — yt-dlp 장부에 영상들(빼 둔 clip3 은 정리 장부에서 되돌아옴)',
                  all(('generic %s' % n) in a_txt for n in ('clip1', 'clip6', 'studio', 'big', 'clip3')), a_txt[-200:])
            check('B — 정리 장부(_complete 안)에 정리한 영상들 — 남겨 둔 clip5 는 없음',
                  all(('generic %s' % n) in l_txt for n in ('clip1', 'clip6', 'studio', 'big', 'clip3')) and 'generic clip5' not in l_txt,
                  l_txt[-200:])
            xv, xc = xlsx_ids(xv_path), xlsx_ids(xc_path)
            check('B — 형태별 엑셀: 정리한 영상 · 받은 폴더의 것은 있고, 손으로 지운 clip4 는 빠졌다(예전과 같음)',
                  {'clip1', 'clip6', 'studio', 'big', 'clip3', 'clip5', 'fake-f2'} <= xv and 'clip4' not in xv, sorted(xv))
            check('B — complete 엑셀은 쌓는다(clip4 도 남음)', {'clip1', 'clip6', 'studio', 'big', 'clip3', 'clip4'} <= xc, sorted(xc))

            # ── C 같은 주소를 다시(+ 그 사이 시작을 한 번 더 — 한 판만 돌아야)
            clog = run_yt('C(다시)', ['studio.html'], 'channel', 'video', True, double_start=True)
            clog = wait_log('youtube', '정리한 영상') if '정리한 영상' not in clog else clog
            check('C — 다시 받지 않았다(정리 줄 없음 · 받은 폴더에 새 영상 없음)', '🧹' not in clog
                  and not [v for v in files_in(yt / 'video', MEDIA) if '_s1' in v.name], clog[-300:])
            check('C — 받는 중에 다시 누른 시작은 막혔다(한 판만)', '이미 받는 중입니다' in clog, clog[-300:])
            xv = xlsx_ids(xv_path)
            check('C — 정리한 영상들의 엑셀 줄이 남았다(info.json 이 없어도)', {'clip1', 'clip6', 'studio', 'big', 'clip3'} <= xv, sorted(xv))
            check('C — 기록에 그 수(정리한 영상 5개)', '정리한 영상 5개의 줄 그대로' in clog, clog[-300:])

            # ── G 읽을 수 없는 엑셀 — 덮지 않고, 그 판은 비우지 않는다
            good = xv_path.read_bytes()
            xv_path.write_bytes(b'PK\x03\x04 this is not a workbook ' * 20)
            garbage = sha(xv_path)
            run_yt('G(엑셀을 못 읽음)', ['clip2.mp4'], 'channel', 'video', True)
            glog = wait_log('youtube', '이번에는 받은 폴더를 비우지 않습니다')
            v2 = [v for v in files_in(yt / 'video', MEDIA) if v.stem.endswith('_clip2')]
            check('G — 못 읽은 엑셀을 덮지 않았다', sha(xv_path) == garbage)
            check('G — 기록에 까닭과 비우지 않는다는 말', '엑셀을 읽지 못해 덮어쓰지 않습니다' in glog
                  and '이번에는 받은 폴더를 비우지 않습니다' in glog, glog[-400:])
            check('G — 그 판에 받은 원본 · info.json 은 받은 폴더에 남았다', len(v2) == 1
                  and (v2[0].parent / (v2[0].stem + '.info.json')).exists(), rel(v2))
            xv_path.write_bytes(good)
            run_yt('G2(엑셀을 되돌림)', ['clip2.mp4'], 'channel', 'video', True)
            g2log = wait_log('youtube', '🧹 _complete 만 남기기')
            check('G2 — 엑셀을 되돌린 다음 판에 정리됐다', '🧹 _complete 만 남기기: 1개 정리' in g2log
                  and not [v for v in files_in(yt / 'video', MEDIA) if v.stem.endswith('_clip2')], g2log[-300:])
            check('G2 — 엑셀에 예전 줄과 clip2', {'clip1', 'clip6', 'studio', 'big', 'clip3', 'clip2'} <= xlsx_ids(xv_path))

            # ── X · Y 같은 이름의 다른 영상 둘
            run_yt('X(같은 이름 첫째)', ['coll1.html'], 'channel', 'video', True)
            cm = [v for v in files_in(yt / '_complete' / 'Coll Ch', MEDIA)]
            check('X — 첫째가 _complete/Coll Ch/<날짜>_same.mp4 로', len(cm) == 1 and cm[0].name.endswith('_same.mp4'), rel(cm))
            first = cm[0] if cm else None
            first_sha = sha(first) if first else ''
            ylog = run_yt('Y(같은 이름 둘째)', ['coll2.html'], 'channel', 'video', True)
            ylog = wait_log('youtube', '🧹 _complete 만 남기기') if '🧹' not in ylog else ylog
            cm = [v for v in files_in(yt / '_complete' / 'Coll Ch', MEDIA)]
            alt = [v for v in cm if v.name.endswith('_same [coll2].mp4')]
            check('Y — 첫째의 하나뿐인 사본을 덮지 않았다(바이트 · 받은 곳 그대로)', first is not None and first.exists()
                  and sha(first) == first_sha and url('coll1.html') in where_from(first), (rel(cm), where_from(first)[:120] if first else ''))
            check("Y — 둘째는 '[번호]' 를 붙여 따로 두고 받은 폴더는 비웠다", len(alt) == 1 and url('coll2.html') in where_from(alt[0])
                  and not (yt / 'video' / 'Coll Ch').exists() and "'[번호]' 를 붙여 둠" in ylog, (rel(cm), ylog[-300:]))

            # ── D · E 썸네일만
            run_yt('D(썸네일 · 옵션)', ['thumbpage.html'], 'channel', 'thumbnail', True)
            th = [x for x in files_in(yt / 'thumbnail') if not x.name.startswith('.')]
            tc = [v for v in files_in(yt / '_complete', {'.jpg', '.png', '.webp'}) if '_t1' in v.name]
            tled = yt / '_complete' / '.yt_cleaned_thumbnail.txt'
            check('D — 썸네일이 _complete 로 가고 받은 폴더는 비었다', len(tc) == 1 and not th, (rel(th), rel(tc)))
            check('D — 정리 장부(썸네일)에 남았다', tled.exists() and 'generic thumbpage' in tled.read_text(encoding='utf-8'))
            xc, xt = xlsx_ids(xc_path), xlsx_ids(yt / 'excel' / 'youtube_thumbnail.xlsx')
            check('D — complete 엑셀에 모든 형태(동영상 + 썸네일)', {'clip1', 'clip6', 'studio', 'big', 'clip4', 'coll1', 'coll2', 'thumbpage'} <= xc,
                  sorted(xc))
            check('D — 썸네일 엑셀은 썸네일만', xt == {'thumbpage'}, sorted(xt))
            elog = run_yt('E(썸네일 다시)', ['thumbpage.html'], 'channel', 'thumbnail', True)
            th = [x for x in files_in(yt / 'thumbnail') if not x.name.startswith('.')]
            tarch = yt / '.yt_archive_thumbnail.txt'
            check('E — 다시 받지 않았다(정리 줄도 엑셀 줄도 없음 · 받은 폴더 빔)', not th and '🧹' not in elog
                  and '📊 Excel 저장' not in elog, (rel(th), elog[-300:]))
            check('E — 이번 판의 썸네일 장부에 정리 장부가 더해졌다', tarch.exists() and 'generic thumbpage' in tarch.read_text(encoding='utf-8'))

            # ── H 동영상 + 썸네일 — 썸네일만 받은 판의 그림과 같은 자리(같은 채널 · 날 · 제목)
            timg = tc[0] if tc else None
            timg_sha = sha(timg) if timg else ''
            hlog = run_yt('H(동영상 + 썸네일, 같은 이름)', ['thumbpage2.html'], 'channel', 'video', True, thumb=True)
            hlog = wait_log('youtube', '🧹 _complete 만 남기기') if '🧹' not in hlog else hlog
            hd = yt / '_complete' / 'Thumb Ch'
            hjpg = [x for x in files_in(hd, {'.jpg', '.png', '.webp'}) if '[thumbpage2]' in x.name]
            check('H — 썸네일만 받은 판의 하나뿐인 그림을 덮지 않았다', timg is not None and timg.exists() and sha(timg) == timg_sha,
                  rel(files_in(hd)))
            check("H — 동영상의 썸네일(곁 파일)은 '[번호]' 로 따로 두고, 영상 사본 · 받은 폴더 정리도 됐다",
                  len(hjpg) == 1 and any(x.suffix == '.mp4' for x in files_in(hd)) and not (yt / 'video' / 'Thumb Ch').exists()
                  and "곁 파일 1개도 같은 이름의 다른 파일이 있어" in hlog, (rel(files_in(hd)), hlog[-300:]))

            # ── S 시작하자마자 중지 — 띄우기 전의 중지를 놓치지 않는다(놓치면 지켜보는 이 없는 판이 돌며 시작을 막았다)
            run_yt('S(시작하자마자 중지)', ['clip7.mp4', 'clip8.mp4'], 'channel', 'video', True, pace='safe', stop_now=True)
            s7 = [v for v in files_in(yt / 'video', MEDIA) if v.stem.endswith('_clip7')]
            time.sleep(5)
            slog = run_yt('S2(곧바로 다시)', ['clip7.mp4'], 'channel', 'video', False)
            check('S — 중지 뒤 곧바로 다시 누른 시작이 막히지 않고 받았다', '이미 받는 중입니다' not in slog
                  and any(v.stem.endswith('_clip7') for v in files_in(yt / 'video', MEDIA)), (rel(s7), slog[-300:]))

            # ── F 니코동 탭
            nbase = C.ISO / 'tmp' / 'nico_complete'
            p.eval("switchTab('niconico')")
            time.sleep(0.6)
            p.eval("""(function(){var g=function(i){return document.getElementById(i)};
              g('niconico-url').value=%s; g('niconico-path').value=%s; g('niconico-type').value='video';
              g('niconico-save-layout').value='flat';
              ['niconico-thumb','niconico-comments','niconico-cookies','niconico-real-capture']
                .forEach(function(i){var e=g(i); if(e) e.checked=false});
              g('niconico-complete-only').checked=true;
              window.__akashiEnded.niconico=false; startNiconico();})()""" % (json.dumps(url('clip2.mp4')), json.dumps(str(nbase))))
            t0 = time.time()
            while not p.eval("!!window.__akashiEnded.niconico", retry=True) and time.time() - t0 < 180:
                time.sleep(0.5)
            nst = C.platform_stats(p, 'niconico')
            nlog = wait_log('niconico', '🧹 _complete 만 남기기')
            check('F 니코동 판이 끝났다(끝 신호)', bool(p.eval("!!window.__akashiEnded.niconico", retry=True))
                  and str(nst.get('status')) in ('Done', '완료'), '%s · %.0fs' % (nst.get('status'), time.time() - t0))
            nico = nbase / 'niconico'
            nv = [x for x in files_in(nico / 'video') if not x.name.startswith('.')]
            nc = files_in(nico / '_complete', MEDIA)
            check('F — niconico/video 가 비고 _complete 바로 아래에 영상', not nv and len(nc) == 1 and nc[0].parent == nico / '_complete'
                  and '🧹 _complete 만 남기기: 1개 정리' in nlog, (rel(nv, nico), rel(nc, nico), nlog[-200:]))
            # 되돌리기 — 칸 값과 가로챈 함수
            p.eval("['youtube-complete-only','niconico-complete-only'].forEach(function(i){var e=document.getElementById(i); if(e) e.checked=false});"
                   "document.getElementById('niconico-save-layout').value='channel';"
                   "if(window.__akashiYt){window.onCollectionEnded=window.__akashiYtOrig;"
                   "delete window.__akashiYt;delete window.__akashiYtOrig;delete window.__akashiEnded;}")
            errs = C.page_errors(p)
            check('화면 JS 오류 없음', not errs, errs[:2])
    except Exception as e:  # noqa: BLE001
        check('시험 진행 중 오류 없음', False, '%s: %s' % (type(e).__name__, e))
    finally:
        C.keep_log('youtube_complete_app.log')
        C.iso_stop()
        srv.shutdown()
    return check.summary()


if __name__ == '__main__':
    sys.exit(main())
