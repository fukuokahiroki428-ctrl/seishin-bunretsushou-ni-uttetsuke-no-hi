#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""창립일까지 — '전체(2006년부터)' 의 검색 걸음이 긴 공백을 건너 계정을 만든 날까지 내려가는지(앱 격리 사본 + 가짜 X, 약 5분).

    python3 akashi/tests/deep_walk.py

가짜 X 는 진짜 X 처럼 until: 검색이 20일보다 긴 공백을 한 번에 건너지 못하고(search_gap_days) 한 검색의 커서가 3개에서 마른다. 타임라인(UserTweets)은 꺼서
검색 걸음만 본다. 글 묶음: 2일 전 · 115일 전 · 230일 전 · 345일 전(사이 공백 113~115일), 계정을 만든 날 380일 전.
  R1 첫 판 — 공백 셋을 건너 네 묶음을 다 받고 '계정을 만든 날까지 스캔 완료' · 바닥 표시(가장 오래된 날)를 남긴다
     (r54 까지는 빈 결과 10번 = 70일에서 멈춰 첫 공백 아래를 하나도 받지 못했다)
  R2 다시 끝까지(일찍 멈춤 끔) — 받아 둔 옛 글로 공백을 건너뛰고, 표시 아래는 다시 밟지 않는다: until 검색이 R1 의 1/3 도 안 된다
  R3 표시를 남긴 걸음 뒤에 올라온 글(표시의 맨 위 날보다 새 구간 · 공백 너머)도 받는다 — 거기선 건너뛰지 않고 7일씩 걷는다(검토)
  R4 표시의 가장 오래된 날 아래에 받아 둔 글이 있어도(지워졌거나 검색에 안 보이는 글) 그 아래로 내려가지 않는다(검토)
가짜 daemon 은 격리 사본의 script_overrides 에만 넣는다. 계정은 이 시험이 만든 가짜 값이다.
"""
import json, os, shutil, sys, time
from datetime import datetime, timezone
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import _common as C  # noqa: E402
from early_stop import World, NOW, HOUR, xlsx_ids  # noqa: E402

DAY = 24 * HOUR


def utc_date(ms):
    return datetime.fromtimestamp(ms / 1000, tz=timezone.utc).strftime("%Y-%m-%d")


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
        save = C.ISO / "tmp" / "deep-walk"
        shutil.rmtree(save, ignore_errors=True)
        save.mkdir(parents=True)
        udir = save / "twitter" / "tgt"
        w = World(home)
        created = NOW - 380 * DAY
        w.st["user"]["created_ms"] = created
        w.st["search_gap_days"] = 20
        w.st["search_depth"] = 3        # 한 검색의 커서가 3개에서 마른다(진짜 X 처럼) — 맨 위 검색이 다 돌려주면 공백을 건널 일이 없다
        w.st["timeline_off"] = True
        groups = {"D": NOW - 2 * DAY, "C": NOW - 115 * DAY, "B": NOW - 230 * DAY, "A": NOW - 345 * DAY}
        ids = {}
        for g, base in groups.items():
            ids[g] = [w.add_tweet(base + i * HOUR) for i in range(3)]
        w.save()
        want = {i for v in ids.values() for i in v}

        def run(label, **extra):
            w.clear_requests()
            cfg = {"platform": "twitter", "target": "tgt", "type": "tweets", "path": str(save),
                   "accounts": [{"auth_token": "akashi-test-token", "ct0": "akashi-test-ct0"}],
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
                st, took = C.wait_status(p, "twitter", DONE, 1200, step=1.0)
                t1 = time.time()
                while not idle() and time.time() - t1 < 180:   # 끝까지(답글 걸음 · 엑셀 · 표시) — 다음 판이 '이미 수집 중' 이 되지 않게
                    time.sleep(1)
                time.sleep(3)
                log = p.eval("(document.getElementById('twitter-log')||{}).innerText||''", retry=True) or ""
            reqs = w.requests()
            untils = [r for r in reqs if r["action"] == "search" and "until:" in r.get("query", "") and "filter:replies" not in r["query"]]
            print("   [%s] %s · %.0fs · until 검색 %d" % (label, (st or {}).get("status"), took or -1, len(untils)), flush=True)
            if os.environ.get("DEEP_WALK_DEBUG"):
                print("     질의:", [(r.get("query", "").replace("from:tgt ", ""), r.get("n")) for r in reqs if r["action"] == "search"])
                print("     기록:\n" + "\n".join("       " + ln for ln in log.splitlines()[-80:]))
            return st, log, untils

        # ── R1 ──
        st, log, u1 = run("R1")
        got = xlsx_ids(udir / "tgt_complete.xlsx")
        check("R1 판이 끝났다", (st or {}).get("status") in ("완료", "Done"), (st, log[-300:]))
        check("R1 공백 셋을 건너 네 묶음을 다 받았다 %d/%d" % (len(got & want), len(want)), want <= got,
              {g: len(set(v) & got) for g, v in ids.items()})
        check("R1 '계정을 만든 날(%s)까지 스캔 완료'" % utc_date(created), ("계정을 만든 날(%s)까지 스캔 완료" % utc_date(created)) in log,
              log[-400:])
        mark = udir / ".hanishiki_floor_tw_tweets_search.json"
        mo = json.loads(mark.read_text(encoding="utf-8")) if mark.exists() else {}
        check("R1 바닥 표시 — 바닥 = 계정을 만든 날 · 가장 오래된 날 = A 묶음의 날 · 맨 위 = 어제(UTC) · 받아 둔 글 12",
              mo.get("floor") == utc_date(created) and mo.get("oldest") == utc_date(groups["A"])
              and mo.get("top") in (utc_date(NOW - DAY), utc_date(NOW)) and mo.get("n") == 12 and mo.get("v") == 2, mo)

        # ── R2 ──
        st, log, u2 = run("R2")
        got = xlsx_ids(udir / "tgt_complete.xlsx")
        check("R2 판이 끝났다 · 다 그대로", (st or {}).get("status") in ("완료", "Done") and want <= got, (st, len(got & want)))
        check("R2 받아 둔 옛 글로 공백을 건너뛰었다", log.count("받아 둔 옛 글(") >= 2, log[-500:])
        check("R2 표시 아래는 다시 밟지 않았다", "다시 보지 않습니다" in log, log[-300:])
        check("R2 until 검색이 R1 의 1/3 보다 적다(%d → %d)" % (len(u1), len(u2)), len(u2) * 3 < len(u1), (len(u1), len(u2)))

        # ── R3: 표시를 남긴 걸음이 60일 전까지만 덮었다고 치고(맨 위 날을 당긴다), 그 뒤 · 공백 너머에 새 글 E(40일 전) ──
        mo3 = dict(json.loads(mark.read_text(encoding="utf-8")))
        mo3["top"] = utc_date(NOW - 60 * DAY)
        mo3["n"] = 9                                   # 60일 전 아래에 받아 둔 글(C · B · A)
        mark.write_text(json.dumps(mo3), encoding="utf-8")
        ids["E"] = [w.add_tweet(NOW - 40 * DAY + i * HOUR) for i in range(3)]
        w.save()
        st, log, u3 = run("R3")
        got = xlsx_ids(udir / "tgt_complete.xlsx")
        check("R3 표시의 맨 위 날보다 새 · 공백 너머의 글 E 를 받았다", set(ids["E"]) <= got, (len(set(ids["E"]) & got), log[-400:]))
        check("R3 그 아래(맨 위 날 아래)에선 다시 건너뛰었다", "받아 둔 옛 글(" in log and "다시 보지 않습니다" in log, log[-400:])

        # ── R4: A 묶음이 검색에서 사라졌고(지움) 표시의 가장 오래된 날이 B 라면 — 받아 둔 A 로 내려가지 않고 B 아래에서 멈춘다 ──
        w.st["search_hidden"] = list(ids["A"])
        w.save()
        mo4 = dict(json.loads(mark.read_text(encoding="utf-8")))
        mo4["oldest"] = utc_date(groups["B"])
        mark.write_text(json.dumps(mo4), encoding="utf-8")
        st, log, u4 = run("R4")
        a_queries = [r for r in u4 if r.get("query", "").split("until:")[-1] < utc_date(groups["B"])]
        check("R4 B 아래로는 묻지 않았다(받아 둔 A 가 있어도)", not a_queries and "다시 보지 않습니다" in log,
              ([r.get("query") for r in a_queries][:3], log[-300:]))
    except Exception as e:  # noqa: BLE001
        check("시험 진행 중 오류 없음", False, "%s: %s" % (type(e).__name__, e))
    finally:
        C.keep_log("deep_walk_app.log")
        C.iso_stop()
    return check.summary()


if __name__ == "__main__":
    sys.exit(main())
