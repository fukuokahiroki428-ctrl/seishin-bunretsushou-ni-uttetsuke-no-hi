#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""글 갈래 — 올라온 것을 빠짐없이 받는가(앱 격리 사본 + 가짜 X + 가짜 미디어 서버 + 가짜 yt-dlp, 네트워크 없음, 약 9분).

    python3 akashi/tests/media_coverage.py

  G1 '트윗' 첫 판 — 새 엑셀 칸(대체 글 · 카드 · X 글 · 고친 이력 · 인용한 글의 본문과 작성자)이 차고, 옛 30칸 뒤에 붙었다.
     카드 그림 · X 글 표지 · 인용한 글의 그림 · 리트윗한 인용 글 속 그림(r55 까지 놓쳤다)을 받았다. 영상 칸은 mp4 주소.
     긴 글(note_tweet)의 280자 뒤 링크도 urls 칸에. HLS(m3u8)만 있는 영상은 실패로 알리고 yt-dlp 로 받아 보았다.
     한 번 실패하는 그림(가짜 서버가 처음 한 번 503)은 받지 못한 미디어로 남았다(.hanishiki_media_retry_*.json)
  G2 둘째 판 — 걸음이 그 글에 다시 닿지 않아도 TweetDetail 로 다시 받아 그 그림 · HLS 영상이 생기고, 다시 받을 목록이 비었다
  G3 '답글' — 이번 판에 받은 답글의 대화 미디어(Phase 4)를 받았다(r55 까지는 저장이 비운 버퍼를 읽어 0개였다)
  G4 팔로워 — 3쪽째부터 오류가 이어져도 기존 엑셀의 사람을 지우지 않고 합친다('일부만 받았습니다')
  G5 남의 좋아요 — 로그인한 계정의 번호를 알면 묻지 않고 까닭을 알린다 · 모르면 빈 쪽 다섯 번 뒤에 같은 까닭을 알린다
  G6 스레드 — 첫 글에 달린 남의 답글은 담지 않는다(검토 B-1: 첫 쪽에서는 작성자를 몰라 남의 답글과 그 그림까지 담았다)
가짜 daemon · 가짜 yt-dlp 는 격리 사본의 데이터 폴더(script_overrides · tools)에만 넣는다. 계정은 이 시험이 만든 가짜 값이다.
"""
import glob, hashlib, json, os, shutil, stat, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import _common as C  # noqa: E402
from early_stop import World, NOW, HOUR, sf, xlsx_rows  # noqa: E402
import fake_x_daemon as FX  # noqa: E402  (serve_media 만 쓴다 — 이 프로세스에서는 ~/fake_x 를 건드리지 않는다)

DAY = 24 * HOUR
TOKEN = "akashi-test-token"
OLD30 = ["id", "tweet_url", "text", "language", "type", "author_name", "author_username", "bookmark_count", "favorite_count",
         "retweet_count", "retweeted", "reply_count", "quote_count", "view_count", "created_at", "retweet_time", "source",
         "hashtags", "urls", "media_type", "media_urls", "conversation_id", "in_reply_to", "is_quote", "quoted_tweet_url",
         "possibly_sensitive", "user_followers", "user_following", "user_verified", "user_profile_image"]
NEW8 = ["alt_text", "card_title", "card_url", "article_title", "article_text", "edit_ids", "quoted_text", "quoted_author"]

# 가짜 yt-dlp — 앱은 데이터 폴더 tools/yt-dlp 가 1MB 를 넘고 실행할 수 있으면 그것을 먼저 쓴다(Common::ytDlpExecutable).
#   -o 의 %(ext)s 를 mp4 로 바꾼 곳에 작은 mp4 를 쓴다. FAIL 표시 파일이 있으면 그것을 지우고 한 번 실패한다.
FAKE_YTDLP = r'''#!/bin/sh
LOG="__LOG__"; FAIL="__FAIL__"
out=""; url=""
while [ $# -gt 0 ]; do
  case "$1" in
    -o) out="$2"; shift 2;;
    --ffmpeg-location|--remux-video) shift 2;;
    -*) shift;;
    *) url="$1"; shift;;
  esac
done
if [ -f "$FAIL" ]; then rm -f "$FAIL"; printf '{"url": "%s", "failed": true}\n' "$url" >> "$LOG"; echo "ERROR: fake fail"; exit 1; fi
printf '{"url": "%s"}\n' "$url" >> "$LOG"
f=$(printf '%s' "$out" | sed 's/%(ext)s/mp4/')
printf '\000\000\000\030ftypmp42fake-hls' > "$f"
echo "[download] 100%"
exit 0
'''


def media_files(udir: Path, key: str):
    return [p for p in glob.glob(str(udir / "media" / "**" / ("*(%s).*" % key)), recursive=True) if "/_complete/" not in p]


def main() -> int:
    check = C.Checks()
    if not C.test_app():
        print("시험할 앱이 없습니다 — 빌드부터")
        return 2
    srv = None
    try:
        if not C.iso_start():
            return 2
        home = C.ISO / "home"
        data = home / "Library" / "Application Support" / "Hanishiki"
        ov = data / "script_overrides"
        ov.mkdir(parents=True, exist_ok=True)
        shutil.copy(HERE / "fake_x_daemon.py", ov / "twitter_daemon.py")
        w = World(home)
        medialog = w.fx / "media.jsonl"
        ytlog = w.fx / "ytdlp.jsonl"
        ytfail = w.fx / "ytdlp_fail_once"
        for p in (medialog, ytlog):
            if p.exists():
                p.unlink()
        ytfail.write_text("1", encoding="utf-8")
        tools = data / "tools"
        tools.mkdir(parents=True, exist_ok=True)
        yt = tools / "yt-dlp"
        body = FAKE_YTDLP.replace("__LOG__", str(ytlog)).replace("__FAIL__", str(ytfail))
        yt.write_text(body + ("#" + "x" * 99 + "\n") * 10500, encoding="utf-8")   # 1MB 를 넘겨야 앱이 쓴다
        yt.chmod(yt.stat().st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)
        srv, port = FX.serve_media(fail_once=("/media/flaky1",), log_path=str(medialog))

        save = C.ISO / "tmp" / "tw-media"
        shutil.rmtree(save, ignore_errors=True)
        save.mkdir(parents=True)
        udir = save / "twitter" / "tgt"
        w.st["media_base"] = "http://127.0.0.1:%d" % port

        def own(ms, seq, **kw):
            t = dict({"id": sf(ms, seq), "ms": ms, "text": "글 %d" % seq, "reply_to": None}, **kw)
            w.st["own"].append(t)
            return t["id"]

        long_text = "긴 글 " + "가나다라마바사 " * 50 + "끝"
        t_alt = own(NOW - 9 * DAY, 101, photos=[{"key": "alt1", "alt": "고양이가 창가에 앉아 있다"}, {"key": "alt2"}])
        t_card = own(NOW - 8 * DAY, 102, card={"title": "카드 제목", "url": "https://example.invalid/page", "img": "card1"})
        t_art = own(NOW - 7 * DAY, 103, article={"title": "글 제목", "text": "글 본문 — 280자를 넘어도 잘리지 않는다", "cover": "cover1"})
        t_hls = own(NOW - 6 * DAY, 105, video={"key": "hls1", "hls_only": True})
        t_flaky = own(NOW - 5 * DAY, 106, photos=[{"key": "flaky1"}])
        q_id = sf(NOW - 30 * DAY, 107)
        t_quote = own(NOW - 4 * DAY, 109, text="인용합니다", edit_ids=[sf(NOW - 4 * DAY - 60000, 108), sf(NOW - 4 * DAY, 109)],
                      quote={"id": q_id, "ms": NOW - 30 * DAY, "text": "인용된 글의 본문", "author": "quoted_guy", "photos": [{"key": "quoted1"}]},
                      note={"text": long_text, "urls": ["https://example.invalid/after-280"]})
        t_vid = own(NOW - 3 * DAY, 110, video={"key": "mp4v1"})
        rt_id = sf(NOW - 2 * DAY, 111)
        rq_id = sf(NOW - 40 * DAY, 113)
        w.st["timeline_extra"].append({"kind": "rt", "id": rt_id, "orig_id": sf(NOW - 20 * DAY, 112), "ms": NOW - 2 * DAY,
                                       "orig_fx": {"text": "인용 글(리트윗됨)",
                                                   "quote": {"id": rq_id, "ms": NOW - 40 * DAY, "text": "깊은 인용", "author": "deep",
                                                             "photos": [{"key": "rtquoted1"}]}}})
        w.save()

        def run(kind, label, accounts=None, **extra):
            w.clear_requests()
            cfg = {"platform": "twitter", "target": "tgt", "type": kind, "path": str(save),
                   "accounts": accounts or [{"auth_token": TOKEN, "ct0": "akashi-test-ct0"}],
                   "stopAfterKnown": 0, "realCapture": False, "realCaptureMode": "off", "downloadMedia": True, "exif": False,
                   "excel": True, "mode": "all", "resumeMode": "both", "saveProgress": False, "count": 0}
            cfg.update(extra)
            DONE = ("완료", "오류", "중지됨", "Error", "Done", "Stopped")
            with C.page() as p:
                def idle():
                    return p.eval("(function(){var b=document.getElementById('twitter-stop-btn');return !b||b.disabled;})()", retry=True) is True
                t0 = time.time()
                while not idle() and time.time() - t0 < 300:
                    time.sleep(1)
                p.eval("(typeof platformStats!=='undefined') && (platformStats.twitter = {status: 'akashi-wait'}) && true")
                p.eval("(function(){var e=document.getElementById('twitter-log'); if(e) e.innerHTML=''; return true;})()")
                p.eval("backend.startCollection(%s)" % json.dumps(json.dumps(cfg)))
                t0 = time.time()
                while time.time() - t0 < 60 and C.platform_stats(p, "twitter").get("status") == "akashi-wait":
                    time.sleep(0.5)
                st, took = C.wait_status(p, "twitter", DONE, 900, step=1.0)
                t1 = time.time()
                while not idle() and time.time() - t1 < 300:
                    time.sleep(1)
                time.sleep(4)
                logtxt = p.eval("(document.getElementById('twitter-log')||{}).innerText||''", retry=True) or ""
            reqs = w.requests()
            print("   [%s] %s %s · %.0fs · 요청 %d" % (label, kind, (st or {}).get("status"), took or -1, len(reqs)), flush=True)
            return st, reqs, logtxt

        def ytcalls():
            return [json.loads(l) for l in ytlog.read_text(encoding="utf-8").splitlines() if l.strip()] if ytlog.exists() else []

        retry_file = udir / ".hanishiki_media_retry_tgt_complete.json"
        xl = udir / "tgt_complete.xlsx"

        # ── G1 첫 판 ──
        st, reqs, logtxt = run("tweets", "G1")
        check("G1 판이 끝났다", (st or {}).get("status") in ("완료", "Done"), (st, logtxt[-300:]))
        rows = xlsx_rows(xl)
        head = list(next(iter(rows.values())).keys()) if rows else []
        check("G1 옛 30칸은 그대로 · 새 칸 여덟은 그 뒤에", head[:30] == OLD30 and head[30:38] == NEW8, head[28:])
        r = rows.get(t_alt, {})
        check("G1 대체 글(alt_text)", "고양이가 창가에 앉아 있다" in r.get("alt_text", ""), r.get("alt_text"))
        r = rows.get(t_card, {})
        check("G1 카드 제목 · 주소", r.get("card_title") == "카드 제목" and r.get("card_url") == "https://example.invalid/page",
              (r.get("card_title"), r.get("card_url")))
        r = rows.get(t_art, {})
        check("G1 X 글 제목 · 본문", r.get("article_title") == "글 제목" and "잘리지 않는다" in r.get("article_text", ""),
              (r.get("article_title"), r.get("article_text")))
        r = rows.get(t_quote, {})
        check("G1 인용한 글의 본문 · 작성자 · 고친 이력", r.get("quoted_text") == "인용된 글의 본문" and r.get("quoted_author") == "quoted_guy"
              and t_quote in r.get("edit_ids", ""), (r.get("quoted_text"), r.get("quoted_author"), r.get("edit_ids")))
        check("G1 긴 글 — 280자 뒤까지 본문 · 그 뒤의 링크도 urls 칸에", len(r.get("text", "")) > 280
              and "https://example.invalid/after-280" in r.get("urls", ""), (len(r.get("text", "")), r.get("urls")))
        r = rows.get(t_vid, {})
        check("G1 영상의 media_urls 는 받은 mp4 주소(미리보기 그림이 아니다)", "1280x720/mp4v1.mp4" in r.get("media_urls", ""), r.get("media_urls"))
        r = rows.get(rt_id, {})
        check("G1 리트윗한 인용 글 — 원글이 인용한 글의 본문 · 작성자", r.get("quoted_author") == "deep" and r.get("quoted_text") == "깊은 인용",
              (r.get("quoted_author"), r.get("quoted_text")))
        for key, what in (("alt1", "그림"), ("card1", "카드 그림"), ("cover1", "X 글 표지"), ("quoted1", "인용한 글의 그림"),
                          ("rtquoted1", "리트윗한 인용 글 속 그림"), ("mp4v1", "mp4 영상")):
            check("G1 %s를 받았다(%s)" % (what, key), bool(media_files(udir, key)), key)
        check("G1 HLS 만 있는 영상을 알렸다", "HLS(m3u8) 만 있습니다" in logtxt, logtxt[-300:])
        calls = ytcalls()
        check("G1 그 영상은 yt-dlp 로 글 주소에서 받아 보았다(첫 번은 일부러 실패)", any(t_hls in c.get("url", "") and c.get("failed") for c in calls), calls)
        check("G1 한 번 실패한 그림 · 받지 못한 HLS 영상은 아직 없다", not media_files(udir, "flaky1") and not media_files(udir, "hls1"),
              (media_files(udir, "flaky1"), media_files(udir, "hls1")))
        retry = json.loads(retry_file.read_text(encoding="utf-8")) if retry_file.exists() else {}
        check("G1 받지 못한 두 글을 다시 받을 목록에 적었다(줄은 엑셀에 있다)", {t_flaky, t_hls} <= set(retry) and {t_flaky, t_hls} <= set(rows),
              (sorted(retry), t_flaky in rows, t_hls in rows))

        # ── G2 둘째 판 — 다시 받기 ──
        st, reqs, logtxt = run("tweets", "G2")
        details = [x.get("tweet_id") for x in reqs if x["action"] == "tweet_detail"]
        check("G2 걸음 전에 그 두 글만 TweetDetail 로 다시 물었다", {t_flaky, t_hls} <= set(details), details)
        check("G2 한 번 실패한 그림을 받았다", bool(media_files(udir, "flaky1")), media_files(udir, "flaky1"))
        check("G2 HLS 영상을 yt-dlp 로 받아 그 이름(미디어 키)으로 두었다", any(p.endswith(".mp4") for p in media_files(udir, "hls1")),
              media_files(udir, "hls1"))
        check("G2 다시 받을 목록이 비었다(파일도 지웠다)", not retry_file.exists(),
              retry_file.read_text(encoding="utf-8") if retry_file.exists() else "")
        check("G2 '다시 받기: 2개 받음'", "다시 받기: 2개 받음" in logtxt, logtxt[-400:])

        # ── G3 답글 — Phase 4 대화 미디어 ──
        rep = own(NOW - DAY, 120, reply_to="friend", reply_to_id=sf(NOW - DAY - HOUR, 119))
        w.st["conversations"] = {rep: [{"id": sf(NOW - DAY + HOUR, 121), "ms": NOW - DAY + HOUR, "text": "대화 속 그림",
                                        "author": "friend", "photos": [{"key": "conv1"}]}]}
        w.save()
        st, reqs, logtxt = run("replies", "G3")
        details = [x.get("tweet_id") for x in reqs if x["action"] == "tweet_detail"]
        check("G3 이번 판에 받은 답글의 대화를 물었다", rep in details, details)
        check("G3 대화 속 그림을 media/conversations 에 받았다", any("/conversations/" in p for p in media_files(udir, "conv1")),
              media_files(udir, "conv1"))

        # ── G4 팔로워 — 끊긴 판은 합친다 ──
        w.st["followers"] = [{"id": str(500000 + k), "sn": "fan%d" % k} for k in range(50)]
        w.save()
        st, reqs, logtxt = run("followers", "G4a")
        fx = udir / "tgt_followers.xlsx"
        check("G4 첫 판 — 50명", len(xlsx_rows(fx)) == 50, len(xlsx_rows(fx)))
        w.st["followers"] = [{"id": str(600000 + k), "sn": "newfan%d" % k} for k in range(5)] + w.st["followers"]
        w.st["followers_fail_from_page"] = 3
        w.save()
        st, reqs, logtxt = run("followers", "G4b")
        frows = xlsx_rows(fx)
        fails = [x for x in reqs if x["action"] == "followers" and x.get("failed")]
        check("G4 3쪽째 오류를 다섯 번 다시 물었다", len(fails) == 6, len(fails))
        check("G4 기존 50명은 그대로 · 새 5명이 더해졌다(55명)", len(frows) == 55 and all(str(500000 + k) in frows for k in range(50)),
              len(frows))
        check("G4 '일부만 받았습니다' 를 알렸다", "일부만 받았습니다" in logtxt, logtxt[-300:])
        w.st.pop("followers_fail_from_page", None)
        w.save()

        # ── G5 남의 좋아요 ──
        tok = hashlib.sha256(TOKEN.encode()).hexdigest()[:16]
        st, reqs, logtxt = run("likes", "G5a", accounts=[{"auth_token": TOKEN, "ct0": "akashi-test-ct0", "uid": "555", "uid_tok": tok}])
        check("G5 로그인한 계정이 아니면 묻지 않고 까닭을 알린다", "좋아요를 본인 것만" in logtxt and not [x for x in reqs if x["action"] == "likes"],
              (logtxt[-300:], len([x for x in reqs if x["action"] == "likes"])))
        st, reqs, logtxt = run("likes", "G5b")
        check("G5 번호를 모르면 빈 쪽 다섯 번 뒤에 같은 까닭을 알린다", "좋아요를 본인 것만" in logtxt, logtxt[-300:])

        # ── G6 스레드 — 첫 글의 작성자만 ──
        root = own(NOW - 10 * DAY, 130, text="스레드 첫 글")
        self_rep = sf(NOW - 10 * DAY + HOUR, 131)
        other_rep = sf(NOW - 10 * DAY + 2 * HOUR, 132)
        w.st.setdefault("conversations", {})[root] = [
            {"id": self_rep, "ms": NOW - 10 * DAY + HOUR, "text": "스레드 둘째 글", "author": "tgt", "reply_to": "tgt",
             "reply_to_id": root, "photos": [{"key": "thr1"}]},
            {"id": other_rep, "ms": NOW - 10 * DAY + 2 * HOUR, "text": "남의 답글", "author": "friend", "reply_to": "tgt",
             "reply_to_id": root, "photos": [{"key": "stranger1"}]}]
        w.save()
        st, reqs, logtxt = run("thread", "G6", target="https://x.com/tgt/status/%s" % root)
        trows = xlsx_rows(udir / ("tgt_thread_%s.xlsx" % root))
        check("G6 첫 글 · 이어 쓴 글을 담았다", root in trows and self_rep in trows, sorted(trows))
        check("G6 남의 답글은 엑셀에 없다", other_rep not in trows, sorted(trows))
        check("G6 이어 쓴 글의 그림은 받고 남의 답글 그림은 받지 않았다",
              bool(media_files(udir, "thr1")) and not media_files(udir, "stranger1"),
              (media_files(udir, "thr1"), media_files(udir, "stranger1")))
    except Exception as e:  # noqa: BLE001
        check("시험 진행 중 오류 없음", False, "%s: %s" % (type(e).__name__, e))
    finally:
        C.keep_log("media_coverage_app.log")
        C.iso_stop()
        if srv:
            srv.shutdown()
    return check.summary()


if __name__ == "__main__":
    sys.exit(main())
