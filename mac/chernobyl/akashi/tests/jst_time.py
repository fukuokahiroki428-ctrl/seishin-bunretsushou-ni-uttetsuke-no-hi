#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""일본 시간 기준 — 맥의 시간대를 뉴욕으로 둔 격리 사본에서, 저장되는 날짜가 일본 시간인지 본다(인터넷으로 안 나감, 약 2분).

    python3 akashi/tests/jst_time.py

글 시각은 모두 2026-01-02 08:30 JST (= 2026-01-01 23:30 UTC = 2026-01-01 18:30 뉴욕) — 세 시간대의 날짜가 갈리는 시각이다.
  트위터(가짜 X): 엑셀의 글 시각 · 캡처 파일 이름 · 기간 거르기(일본 시간의 하루) — 트윗 · 답글 · 좋아요
  유튜브(이 맥 안의 시험 페이지): 파일 이름의 날짜 · 엑셀의 upload_date · 파일의 수정 · 만든 날짜(그 순간 그대로)
    · 예전 판이 맞춰 둔 파일 시각(UTC 자정)은 다시 돌아도 그대로(시각이 바뀌면 백업이 영상을 모두 다시 올린다)
    · 예전 이름(UTC 날짜)으로 받아 둔 영상이 장부에 없어도 또 받지 않는다 · 엑셀은 한 줄
    · 썸네일만 받기(yt-dlp 가 장부에 적지 않는다) — 예전 이름의 썸네일이 있으면 또 받지 않는다 · 동영상 장부는 그대로
    · 그냥 다시 돌면 뒤처리를 마친 영상은 다시 쓰지 않는다(판마다 모든 영상의 EXIF 를 다시 쓰던 것)
★ 왜 뉴욕인가 — 이 맥은 KST 라 맥의 현지 시각을 쓰는 코드도 일본 시간과 똑같이 나와 드러나지 않는다. 시간대를 바꿔야 보인다.
가짜 계정 값은 이 시험이 만든 것이고 수집 설정으로만 넘긴다.
"""
from __future__ import annotations

import json
import os
import shutil
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import _common as C  # noqa: E402
import fixtures  # noqa: E402
from early_stop import World, xlsx_rows  # noqa: E402

POST_EPOCH = 1767310200          # 2026-01-02 08:30 JST
POST_MS = POST_EPOCH * 1000
OLD_YT_MIDNIGHT = 1767225600     # 2026-01-01 00:00 UTC — 예전 판이 이 영상에 적던 파일 시각


def main() -> int:
    check = C.Checks()
    app = C.test_app()
    ffmpeg = (app / "Contents" / "MacOS" / "ffmpeg") if app else None
    if not ffmpeg or not ffmpeg.exists():
        print("번들 ffmpeg 가 없습니다 — 빌드부터")
        return 2
    import subprocess
    www = C.work_dir("www_jst")
    clip = www / "clip.mp4"
    if not clip.exists():
        subprocess.run([str(ffmpeg), "-loglevel", "error", "-f", "lavfi", "-i", "testsrc=duration=1:size=160x120:rate=10",
                        "-c:v", "libx264", "-pix_fmt", "yuv420p", str(clip)], check=True)
    srv, hp = fixtures.serve(www)
    thumb = www / "t.jpg"
    if not thumb.exists():
        subprocess.run([str(ffmpeg), "-loglevel", "error", "-f", "lavfi", "-i", "testsrc=size=160x120", "-frames:v", "1", str(thumb)], check=True)
    ld = {"@context": "https://schema.org", "@type": "VideoObject", "name": "s1", "contentUrl": "http://127.0.0.1:%d/clip.mp4" % hp,
          "thumbnailUrl": "http://127.0.0.1:%d/t.jpg" % hp,
          "uploadDate": "2026-01-02T08:30:00+09:00", "author": {"@type": "Person", "name": "ch1"}}
    # 썸네일만 받기용 — 다른 영상(동영상으로 받아 둔 영상은 동영상 장부에 올라 있어 썸네일만 받기도 건너뛴다 · 예전부터)
    ld_t = dict(ld, name="s2")
    (www / "t.html").write_text('<!doctype html><html><head><title>s2</title><script type="application/ld+json">%s</script></head><body>t</body></html>'
                                % json.dumps(ld_t), encoding="utf-8")
    (www / "v.html").write_text('<!doctype html><html><head><title>s1</title><script type="application/ld+json">%s</script></head><body>v</body></html>'
                                % json.dumps(ld), encoding="utf-8")
    try:
        if not C.iso_start("--env", "TZ=America/New_York"):
            return 2
        home = C.ISO / "home"
        ov = home / "Library" / "Application Support" / "Hanishiki" / "script_overrides"
        ov.mkdir(parents=True, exist_ok=True)
        shutil.copy(HERE / "fake_x_daemon.py", ov / "twitter_daemon.py")
        w = World(home)
        tid = w.add_tweet(POST_MS)
        w.add_tweet(POST_MS + 3600 * 1000)
        w.add_tweet(POST_MS - 12 * 3600 * 1000)    # 2026-01-01 20:30 JST — 기간 거르기로 빠져야 한다
        w.save()
        save = C.ISO / "tmp" / "jst-tw"
        shutil.rmtree(save, ignore_errors=True)
        save.mkdir(parents=True)
        udir = save / "twitter" / "tgt"
        DONE = ("완료", "오류", "중지됨", "Error", "Done", "Stopped")

        def run_tw(**extra):
            cfg = {"platform": "twitter", "target": "tgt", "type": "tweets", "path": str(save),
                   "accounts": [{"auth_token": "akashi-test-token", "ct0": "akashi-test-ct0"}],
                   "realCapture": False, "downloadMedia": False, "excel": True, "mode": "all", "resumeMode": "both", "saveProgress": False, "count": 0}
            cfg.update(extra)
            with C.page() as p:
                p.eval("(typeof platformStats!=='undefined') && (platformStats.twitter = {status: 'akashi-wait'}) && true")
                p.eval("backend.startCollection(%s)" % json.dumps(json.dumps(cfg)))
                t0 = time.time()
                while time.time() - t0 < 60 and C.platform_stats(p, "twitter").get("status") == "akashi-wait":
                    time.sleep(0.5)
                C.wait_status(p, "twitter", DONE, 300, step=1.0)
                t1 = time.time()
                while time.time() - t1 < 60 and not p.eval(
                        "(function(){var b=document.getElementById('twitter-stop-btn');return !b||b.disabled;})()", retry=True):
                    time.sleep(1)
            time.sleep(3)

        with C.page() as p:
            time.sleep(1.5)
            offs = p.eval("new Date().getTimezoneOffset()")
            check("맥의 시간대가 정말 뉴욕이다(이 시험의 전제)", offs in (240, 300), offs)
            # 화면의 일본 시간 도우미 — 돌려준 것의 칸(연 · 월 · 일 · 시 · 분)이 일본 시간이다(같은 순간으로 견준다)
            got = p.eval("""(function(){ var n=Date.now(), real=Date.now; Date.now=function(){return n};
                      try { var w=window._jstWallClock(); return [n, w.getFullYear(), w.getMonth()+1, w.getDate(), w.getHours(), w.getMinutes(), w.getTime()]; }
                      finally { Date.now=real; } })()""")
            jt = time.gmtime(got[0] / 1000 + 9 * 3600) if got else None
            check("화면의 일본 시간 도우미 — 칸이 일본 시간 · 그 순간 그대로",
                  bool(got) and got[1:6] == [jt.tm_year, jt.tm_mon, jt.tm_mday, jt.tm_hour, jt.tm_min] and got[6] == got[0], got)
            # 뉴욕의 서머타임이 시작되는 날 없는 시각(2027-03-14 02:30) — 일본 시간이 그 시각인 순간에도 한 시간 밀리지 않아야
            #   (현지 Date 를 일본 시간의 칸으로 만들던 예전 방식은 03:30 을 돌려준다)
            dst = p.eval("""(function(){ var real=Date.now; Date.now=function(){return Date.UTC(2027,2,13,17,30,0)};
                      try { var w=window._jstWallClock(); return [w.getFullYear(), w.getMonth()+1, w.getDate(), w.getHours(), w.getMinutes()]; }
                      finally { Date.now=real; } })()""")
            check("화면의 일본 시간 도우미 — 현지(뉴욕)에 없는 시각에도 칸이 맞다(서머타임 시작 날)", dst == [2027, 3, 14, 2, 30], dst)

        run_tw()
        rows = xlsx_rows(udir / "tgt_complete.xlsx")
        check("트위터 엑셀의 글 시각이 일본 시간", str((rows.get(tid) or {}).get("created_at", "")).startswith("2026/01/02 08:30"), rows.get(tid))
        caps = sorted(x.name for x in (udir / "captures").rglob("*.html")) if (udir / "captures").exists() else []
        check("트위터 캡처 파일 이름이 일본 시간", ("20260102_083000_%s.html" % tid) in caps, caps[:3])

        # 기간은 일본 시간의 하루 — 2026-01-02 하루만
        save2 = C.ISO / "tmp" / "jst-tw2"
        shutil.rmtree(save2, ignore_errors=True)
        save2.mkdir(parents=True)
        w.clear_requests()
        run_tw(path=str(save2), mode="period", since="2026-01-02", until="2026-01-02")
        rows2 = xlsx_rows(save2 / "twitter" / "tgt" / "tgt_complete.xlsx")
        qs = [r.get("query", "") for r in w.requests() if r["action"] == "search"]
        check("기간 거르기 — 일본 시간 2026-01-02 의 글만(같은 날 08:30 · 09:30, 전날 20:30 은 뺌)",
              set(rows2) == {t["id"] for t in w.st["own"][:2]}, sorted(rows2))
        check("기간 검색은 앞뒤로 하루씩 넓혀 묻는다(X 의 날짜는 UTC · until 은 그 날을 뺌)",
              any("since:2026-01-01" in q and "until:2026-01-03" in q for q in qs), qs[:2])

        # 기간 — 답글 · 좋아요도(예전엔 기간 판에서 답글 단계가 통째로 빠져 0개, 좋아요는 기간을 보지 않고 전부)
        rid_in = w.add_tweet(POST_MS + 2 * 3600 * 1000, reply_to="111")       # 2026-01-02 10:30 JST — 기간 안
        rid_out = w.add_tweet(POST_MS - 24 * 3600 * 1000, reply_to="111")     # 2026-01-01 08:30 JST — 기간 밖
        lid_in = w.add_like(POST_MS + 3600 * 1000)                             # 그 글을 쓴 때가 기간 안
        lid_out = w.add_like(POST_MS - 3 * 24 * 3600 * 1000)                   # 기간 밖
        w.save()
        save3 = C.ISO / "tmp" / "jst-tw3"
        shutil.rmtree(save3, ignore_errors=True)
        save3.mkdir(parents=True)
        w.clear_requests()
        run_tw(path=str(save3), type="replies", mode="period", since="2026-01-02", until="2026-01-02")
        qs3 = [r.get("query", "") for r in w.requests() if r["action"] == "search"]
        rows3 = xlsx_rows(save3 / "twitter" / "tgt" / "tgt_complete.xlsx")
        check("기간 — 답글도 기간 검색으로 묻는다(filter:replies · 앞뒤로 하루씩)",
              any("filter:replies" in q and "since:2026-01-01" in q and "until:2026-01-03" in q for q in qs3), qs3[:3])
        check("기간 — 답글은 일본 시간 2026-01-02 의 것만", rid_in in rows3 and rid_out not in rows3, sorted(rows3))
        run_tw(path=str(save3), type="likes", mode="period", since="2026-01-02", until="2026-01-02")
        rows4 = xlsx_rows(save3 / "twitter" / "tgt" / "tgt_likes.xlsx")
        check("기간 — 좋아요는 그 글을 쓴 때로 거른다", lid_in in rows4 and lid_out not in rows4, sorted(rows4))

        # 유튜브
        base = C.ISO / "tmp" / "jst-yt"
        shutil.rmtree(base, ignore_errors=True)

        def run_yt(kind="video", page="v.html"):
            with C.page() as p:
                p.eval("switchTab('youtube')")
                time.sleep(0.6)
                p.eval("(function(){if(window.__akashiYt)return;window.__akashiYt=1;var o=window.onCollectionEnded;"
                       "window.__akashiEnded={};window.onCollectionEnded=function(x){window.__akashiEnded[x]=true;return o&&o.apply(this,arguments)};})()")
                p.eval("""(function(){var g=function(i){return document.getElementById(i)};
                  g('youtube-url').value=%s; g('youtube-path').value=%s; g('youtube-type').value=%s;
                  g('youtube-pace').value='fast'; g('youtube-save-layout').value='channel';
                  ['youtube-subs','youtube-thumb','youtube-playlist','youtube-sponsor','youtube-comments','youtube-real-capture']
                    .forEach(function(i){var e=g(i); if(e) e.checked=false});
                  window.__akashiEnded.youtube=false; startYoutube();})()""" % (json.dumps("http://127.0.0.1:%d/%s" % (hp, page)), json.dumps(str(base)), json.dumps(kind)))
                t0 = time.time()
                while not p.eval("!!window.__akashiEnded.youtube", retry=True) and time.time() - t0 < 180:
                    time.sleep(0.5)

        run_yt()
        vids = sorted((base / "youtube" / "video").rglob("*.mp4")) if (base / "youtube" / "video").exists() else []
        check("유튜브 파일 이름의 날짜가 일본 시간(UTC 날짜 20260101 이 아니다)", len(vids) == 1 and vids[0].name.startswith("20260102_"),
              [v.name for v in vids])
        if vids:
            st = vids[0].stat()
            check("유튜브 파일의 수정 · 만든 날짜가 그 순간 그대로", int(st.st_mtime) == POST_EPOCH and int(st.st_birthtime) == POST_EPOCH,
                  (int(st.st_mtime), int(st.st_birthtime), POST_EPOCH))
            # 그냥 다시 돌면 뒤처리를 마친 영상은 건드리지 않는다(예전엔 판마다 EXIF 를 다시 써 파일을 통째로 다시 썼다)
            before = vids[0].stat()
            time.sleep(1.2)
            run_yt()
            after = vids[0].stat()
            check("유튜브 다시 돌기 — 뒤처리를 마친 영상은 다시 쓰지 않는다", after.st_ctime == before.st_ctime and after.st_mtime == before.st_mtime,
                  (before.st_ctime, after.st_ctime))
            # 파일이 바뀌면(시각이 다르면) 다시 만진다 — 다른 시각은 바로잡고, 예전 판의 UTC 자정은 그대로 둔다
            os.utime(vids[0], (1700000000, 1700000000))
            run_yt()
            check("유튜브 다시 돌기 — 틀린 파일 시각은 바로잡는다", int(vids[0].stat().st_mtime) == POST_EPOCH, int(vids[0].stat().st_mtime))
            os.utime(vids[0], (OLD_YT_MIDNIGHT, OLD_YT_MIDNIGHT))
            run_yt()
            check("유튜브 다시 돌기 — 예전 판의 파일 시각(UTC 자정)은 그대로 둔다", int(vids[0].stat().st_mtime) == OLD_YT_MIDNIGHT, int(vids[0].stat().st_mtime))
            # 예전 판의 이름(yt-dlp 의 UTC 날짜)으로 받아 두었는데 장부에는 없는 영상 — 장부를 적기 전에 끊긴 판
            old_vid = vids[0].with_name(vids[0].name.replace("20260102_", "20260101_", 1))
            info = vids[0].with_name(vids[0].stem + ".info.json")
            vids[0].rename(old_vid)
            if info.exists():
                info.rename(old_vid.with_name(old_vid.stem + ".info.json"))
            archive = base / "youtube" / ".yt_archive.txt"
            archive.unlink(missing_ok=True)
            run_yt()
            vids2 = sorted(x.name for x in (base / "youtube" / "video").rglob("*.mp4"))
            check("유튜브 — 예전 이름으로 받아 둔 영상이 장부에 없어도 또 받지 않는다", vids2 == [old_vid.name], vids2)
            check("유튜브 — 그 영상을 장부에 올렸다", archive.exists() and "v" in archive.read_text(encoding="utf-8").split(), archive.read_text(encoding="utf-8") if archive.exists() else "장부 없음")
        xl = sorted((base / "youtube" / "excel").glob("*.xlsx")) if (base / "youtube" / "excel").exists() else []
        cells = xlsx_rows(xl[0]) if xl else {}
        flat = json.dumps(cells, ensure_ascii=False)
        check("유튜브 엑셀의 날짜가 일본 시간", "20260102" in flat and "20260101" not in flat, flat[:200])

        tdir = base / "youtube" / "thumbnail"
        # 동영상으로 받아 둔 영상(v · s1 — 장부에 올라 있다)은 썸네일만 받기도 건너뛴다(같은 장부의 사본 — 예전과 같다).
        #   그 영상의 쪽(v.html)에 직접 대 본다 — 다른 영상의 쪽으로는 이 약속이 깨져도 드러나지 않는다(검토)
        run_yt("thumbnail", "v.html")
        th0 = sorted(tdir.rglob("*.jpg")) if tdir.exists() else []
        check("썸네일만 — 동영상으로 받아 둔 영상은 건너뛴다(예전과 같다)", th0 == [], [t.name for t in th0])
        # 썸네일만 받기 — 첫 판, 예전 이름으로 바꿔 둔 뒤 다시
        run_yt("thumbnail", "t.html")
        thumbs = sorted(tdir.rglob("*.jpg")) if tdir.exists() else []
        check("썸네일만 — 이름의 날짜가 일본 시간", len(thumbs) == 1 and thumbs[0].name.startswith("20260102_"), [t.name for t in thumbs])
        if thumbs:
            old_t = thumbs[0].with_name(thumbs[0].name.replace("20260102_", "20260101_", 1))
            tinfo = thumbs[0].with_name(thumbs[0].stem + ".info.json")
            thumbs[0].rename(old_t)
            if tinfo.exists():
                tinfo.rename(old_t.with_name(old_t.stem + ".info.json"))
            before = (base / "youtube" / ".yt_archive.txt").read_text(encoding="utf-8")
            run_yt("thumbnail", "t.html")
            thumbs2 = sorted(x.name for x in tdir.rglob("*.jpg"))
            check("썸네일만 — 예전 이름의 썸네일이 있으면 또 받지 않는다", thumbs2 == [old_t.name], thumbs2)
            check("썸네일만 — 동영상 장부는 건드리지 않는다", (base / "youtube" / ".yt_archive.txt").read_text(encoding="utf-8") == before)
    except Exception as e:  # noqa: BLE001
        check("시험 진행 중 오류 없음", False, "%s: %s" % (type(e).__name__, e))
    finally:
        C.keep_log("jst_time_app.log")
        C.iso_stop()
        srv.shutdown()
    return check.summary()


if __name__ == "__main__":
    sys.exit(main())
