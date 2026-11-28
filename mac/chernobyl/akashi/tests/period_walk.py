#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""기간 수집 — '기간 지정' · 하루에 글이 많은 날 · 새 글만 · 정지 계정(r56, 앱 격리 사본 + 가짜 X, 약 10분).

    python3 akashi/tests/period_walk.py

가짜 X 는 진짜 X 처럼 한 검색의 커서가 25개에서 마르고(search_depth), until: 검색이 20일보다 긴 공백을 한 번에 건너지 못하며
(search_gap_days — since: 가 든 질의도: search_gap_since), 2쪽부터 커서를 TimelineReplaceEntry 에 싣는다(replace_cursor 기본).
글 묶음(2026-01 이 중심): 2025-11-16~20 에 30개 → 40일 넘는 공백 → 2026-01-02~30 에 60개 + 2026-01-15(UTC) 하루에 60개 = 제 글 150,
1월의 리트윗 6(from: 검색에는 안 나온다 — filter:nativeretweets 로만).
  P1 기간 판(2025-11-15 ~ 2026-01-31, 트윗) — 제 글 150 과 리트윗 6 을 다 받는다(예전엔 질의 하나 · 커서 두 쪽 → 마지막 며칠 · 리트윗 0)
  P2 '전체(2006년부터)' 판 — 150 을 다 받는다(01-15 하루 60개를 max_id 로 잇는다 — 예전엔 25개에서 그 날의 나머지를 건너뛰었다) · 바닥 표시
  P3 그 뒤 새 글 100(2026-02, 하루 40개 포함)을 올리고 '새 글만' — 100 을 다 받고 가장 새 쪽 표시가 그 위로 오른다
  P4 하나뿐인 계정이 정지(검색 문이 닫힘) — 한국어 안내 · 바닥 표시 없음 · 타임라인 단계는 돈다(검색 요청은 보내지 않는다)
  P6 하나뿐인 계정이 정지 + 기간 판(2026-01) — 기간 타임라인이 그 기간의 제 글을 타임라인으로 받는다 · 기간 밖(11월)은 넣지 않는다(검토 XAPI-1)
  P5 계정 둘(첫째가 정지) — 둘째 계정으로 바꿔 검색을 끝까지 하고 150 을 다 받는다
가짜 daemon 은 격리 사본의 script_overrides 에만 넣는다. 계정은 이 시험이 만든 가짜 값이다.
"""
import json, os, shutil, sys, time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import _common as C  # noqa: E402
from early_stop import World, HOUR, xlsx_ids  # noqa: E402

DAY = 24 * HOUR
MIN = 60 * 1000
TOK1, TOK2 = "akashi-test-token1", "akashi-test-token2"


def utc_ms(y, m, d, hh=0, mm=0):
    return int(datetime(y, m, d, hh, mm, tzinfo=timezone.utc).timestamp() * 1000)


def main() -> int:
    check = C.Checks()
    if not C.test_app():
        print("시험할 앱이 없습니다 — 빌드부터")
        return 2
    try:
        if not C.iso_start():
            return 2
        home = C.ISO / "home"
        ov = home / "Library" / "Application Support" / "Hanishiki" / "script_overrides"
        ov.mkdir(parents=True, exist_ok=True)
        shutil.copy(HERE / "fake_x_daemon.py", ov / "twitter_daemon.py")
        root = C.ISO / "tmp" / "period-walk"
        shutil.rmtree(root, ignore_errors=True)
        root.mkdir(parents=True)

        w = World(home)
        w.st["user"]["created_ms"] = utc_ms(2025, 11, 1)
        w.st["page"] = 10
        w.st["search_depth"] = 25
        w.st["search_gap_days"] = 20
        w.st["search_gap_since"] = True
        w.st["timeline_off"] = True
        nov = [w.add_tweet(utc_ms(2025, 11, 16 + i // 6, 3 + (i % 6) * 2)) for i in range(30)]
        jan = [w.add_tweet(utc_ms(2026, 1, 2 + i // 2, 4 + (i % 2) * 8)) for i in range(56)]
        jan += [w.add_tweet(utc_ms(2026, 1, 30, 1 + i)) for i in range(4)]
        busy = [w.add_tweet(utc_ms(2026, 1, 15, 2) + i * 10 * MIN) for i in range(60)]   # 2026-01-15 02:00~11:50 UTC
        rts = [w.add_rt(utc_ms(2026, 1, 10 + 2 * i, 13)) for i in range(6)]
        w.save()
        own = set(nov) | set(jan) | set(busy)
        check("자료 — 제 글 150 · 리트윗 6", len(own) == 150 and len(rts) == 6, (len(own), len(rts)))

        def run(label, save, accounts=(TOK1,), **extra):
            w.clear_requests()
            cfg = {"platform": "twitter", "target": "tgt", "type": "tweets", "path": str(save),
                   "accounts": [{"name": "akashi%d" % (i + 1), "auth_token": t, "ct0": "akashi-test-ct0"} for i, t in enumerate(accounts)],
                   "stopAfterKnown": 0, "realCapture": False, "downloadMedia": False, "excel": True,
                   "mode": "all", "resumeMode": "both", "saveProgress": False, "count": 0}
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
                st, took = C.wait_status(p, "twitter", DONE, 1500, step=1.0)
                t1 = time.time()
                while not idle() and time.time() - t1 < 180:
                    time.sleep(1)
                time.sleep(3)
                log = p.eval("(document.getElementById('twitter-log')||{}).innerText||''", retry=True) or ""
            reqs = w.requests()
            print("   [%s] %s · %.0fs · 검색 %d" % (label, (st or {}).get("status"), took or -1,
                                                 len([r for r in reqs if r["action"] == "search"])), flush=True)
            if os.environ.get("PERIOD_WALK_DEBUG"):
                print("     질의:", [(r.get("query", "").replace("from:tgt ", ""), r.get("n")) for r in reqs if r["action"] == "search"])
                print("     기록:\n" + "\n".join("       " + ln for ln in log.splitlines()[-80:]))
            return st, log, reqs

        def done(st):
            return (st or {}).get("status") in ("완료", "Done")

        # ── P1 기간 판 ──
        s1 = root / "p1"
        st, log, reqs = run("P1", s1, mode="period", since="2025-11-15", until="2026-01-31")
        got = xlsx_ids(s1 / "twitter" / "tgt" / "tgt_complete.xlsx")
        check("P1 판이 끝났다", done(st), (st, log[-300:]))
        check("P1 기간의 제 글 150 을 다 받았다 %d/150" % len(got & own), own <= got,
              {"11월": len(set(nov) & got), "1월": len(set(jan) & got), "01-15": len(set(busy) & got)})
        check("P1 1월의 리트윗 6 을 받았다 %d/6" % len(got & set(rts)), set(rts) <= got, log[-300:])
        check("P1 리트윗은 filter:nativeretweets 로 물었다",
              any("filter:nativeretweets" in r.get("query", "") for r in reqs if r["action"] == "search"), "")
        check("P1 since: 를 단 채 until: 을 물려 걸었다(질의 하나가 아니다)",
              len([r for r in reqs if r["action"] == "search" and "since:" in r.get("query", "") and "until:" in r.get("query", "")
                   and r.get("off") == 0]) >= 4, "")

        # ── P2 '전체' 판(하루에 60개) ──
        s2 = root / "p2"
        st, log, reqs = run("P2", s2)
        udir2 = s2 / "twitter" / "tgt"
        got = xlsx_ids(udir2 / "tgt_complete.xlsx")
        check("P2 판이 끝났다", done(st), (st, log[-300:]))
        check("P2 150 을 다 받았다(01-15 하루 60개 포함) %d/150" % len(got & own), own <= got, {"01-15": len(set(busy) & got)})
        check("P2 그 날을 max_id 로 이었다", "max_id 로 이어 받습니다" in log
              and any("max_id:" in r.get("query", "") for r in reqs if r["action"] == "search"), log[-400:])
        check("P2 끊김 없이 걸어 바닥 표시를 남겼다", (udir2 / ".hanishiki_floor_tw_tweets_search.json").exists(), log[-300:])

        # ── P3 새 글만 ──
        new = [w.add_tweet(utc_ms(2026, 2, 10, 1) + i * 15 * MIN) for i in range(40)]
        new += [w.add_tweet(utc_ms(2026, 2, 2 + i // 3, 2 + (i % 3) * 6)) for i in range(60)]
        w.save()
        st, log, reqs = run("P3", s2, mode="incremental", saveProgress=True, resumeMode="future")
        got = xlsx_ids(udir2 / "tgt_complete.xlsx")
        check("P3 판이 끝났다", done(st), (st, log[-300:]))
        check("P3 새 글 100 을 다 받았다 %d/100" % len(got & set(new)), set(new) <= got, log[-400:])
        prog = udir2 / ".progress_tweets.json"
        po = json.loads(prog.read_text(encoding="utf-8")) if prog.exists() else {}
        check("P3 가장 새 쪽 표시가 새 글의 맨 위로 올랐다", po.get("newestId") == max(new, key=int), po)
        check("P3 과거로는 되짚지 않았다(since: 없는 until: 질의 없음)",
              not [r for r in reqs if r["action"] == "search" and "until:" in r.get("query", "") and "since:" not in r.get("query", "")],
              "")

        # ── P4 하나뿐인 계정이 정지 ──
        w.st["suspended_tokens"] = [TOK1]
        w.st["timeline_off"] = False
        w.save()
        s4 = root / "p4"
        st, log, reqs = run("P4", s4)
        udir4 = s4 / "twitter" / "tgt"
        check("P4 판이 끝났다", done(st), (st, log[-300:]))
        check("P4 한국어 안내 — 정지 상태로 보아 검색을 막았다 · 타임라인으로 받는다",
              "X 가 계정을 정지 상태로 보아 검색을 막았습니다" in log and "X 앱에서 계정 상태를 확인하십시오" in log, log[-500:])
        check("P4 바닥 표시 · 검색 걸음 표시 없음", not (udir4 / ".hanishiki_floor_tw_tweets_search.json").exists()
              and not list(udir4.glob(".hanishiki_walk_tw_tweets_search*")), sorted(x.name for x in udir4.glob(".hanishiki_*")))
        check("P4 타임라인 단계가 돌았다", any(r["action"] == "user_tweets" for r in reqs), [r["action"] for r in reqs][:10])
        check("P4 정지 계정으로 검색을 되풀이하지 않았다(해시 새로 받기 · 데몬 재시작 없음)",
              len([r for r in reqs if r["action"] == "search"]) <= 1 and "해시를 새로 받아" not in log, log[-300:])
        got = xlsx_ids(udir4 / "tgt_complete.xlsx")
        check("P4 타임라인의 제 글을 받았다", own <= got, len(own & got))

        # ── P6 하나뿐인 계정이 정지 + 기간 판 — 예전엔 기간 판이 타임라인을 돌지 않아 0개였다(검토 XAPI-1) ──
        s6 = root / "p6"
        st, log, reqs = run("P6", s6, mode="period", since="2026-01-01", until="2026-01-31")
        got = xlsx_ids(s6 / "twitter" / "tgt" / "tgt_complete.xlsx")
        janall = set(jan) | set(busy)
        check("P6 판이 끝났다", done(st), (st, log[-300:]))
        check("P6 기간 타임라인이 돌았다", "기간 타임라인" in log and any(r["action"] == "user_tweets" for r in reqs), log[-500:])
        check("P6 1월의 제 글 120 을 타임라인으로 받았다 %d/120" % len(got & janall), janall <= got, log[-400:])
        check("P6 기간 밖(11월)은 넣지 않았다", not (got & set(nov)), len(got & set(nov)))
        check("P6 그 기간의 시작까지 닿았다고 알렸다", "그 기간의 시작까지 닿았습니다" in log, log[-400:])

        # ── P5 계정 둘, 첫째가 정지 ──
        w.st["timeline_off"] = True
        w.save()
        s5 = root / "p5"
        st, log, reqs = run("P5", s5, accounts=(TOK1, TOK2))
        got = xlsx_ids(s5 / "twitter" / "tgt" / "tgt_complete.xlsx")
        srch = [r for r in reqs if r["action"] == "search" and not r.get("suspended")]
        check("P5 판이 끝났다", done(st), (st, log[-300:]))
        check("P5 둘째 계정으로 바꿔 검색했다", "검색이 막히지 않은 계정 2" in log or "계정 전환" in log, log[-400:])
        check("P5 검색은 둘째 계정으로만 나갔다", srch and all(r.get("acct") == TOK2[-1] for r in srch), [r.get("acct") for r in srch][:5])
        check("P5 150 을 다 받았다 %d/150" % len(got & own), own <= got, len(own & got))
        check("P5 '모든 계정이 막혔다' 고 하지 않았다", "X 가 계정을 정지 상태로 보아 검색을 막았습니다" not in log, "")
    except Exception as e:  # noqa: BLE001
        check("시험 진행 중 오류 없음", False, "%s: %s" % (type(e).__name__, e))
    finally:
        C.keep_log("period_walk_app.log")
        C.iso_stop()
    return check.summary()


if __name__ == "__main__":
    sys.exit(main())
