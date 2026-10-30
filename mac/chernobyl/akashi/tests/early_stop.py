#!/usr/bin/env python3
"""일찍 멈춤 · 엑셀 칸 — 트위터 수집기를 가짜 X(fake_x_daemon.py)에 대고 실제로 돌린다(격리 사본 · 네트워크 없음).

  A  첫 판 끝까지(표시에 top) → 새 글 뒤 일찍 멈춤(검색 · 타임라인)
  B  한 계정을 '트윗' · '답글' 두 줄로 — 서로의 표시를 지우지 않고 둘 다 일찍 멈춤
  C  타임라인 대화 묶음 다섯 아래의 새 리트윗을 놓치지 않음
  D  최대 수에서 끊긴 판 뒤에도 표시가 남고 다음 판이 사이를 메움
  L  좋아요: 목록 끝(트윗 없는 쪽)에서 표시 · 다음 판 첫 쪽에서 멈춤 · errors 로 끝나면 표시 없음
  X  엑셀 칸 — 트윗 · 좋아요 · 북마크의 글 주소 · 작성자 · 종류 · 언어 · 미디어, 옛 꼴 엑셀의 빈칸 메우기
  K  캡처를 끈 판이 이미 있는 진짜 캡처를 합성 카드로 덮지 않음 · '안 함' 이면 카드도 만들지 않음 · '미디어만' 이면 글 없는 트윗은 건너뜀
  S  검색 커서가 날 한가운데서 말라도 그 날의 나머지를 놓치지 않음(트윗 · 답글)
  M  일괄 다운로드 창 — 대상 넷을 하나도 건너뛰지 않고 차례로 받음(예전엔 하나 걸러 하나만 받고 '완료' 로 적었다) · 저장 경로가 없으면 멈추지 않고 끝남
가짜 daemon 은 격리 사본의 script_overrides 에만 넣는다(번들 · 진짜 자료 폴더는 건드리지 않는다).
계정은 이 시험이 만든 가짜 값이고 수집 설정으로만 넘긴다."""
import glob, json, os, shutil, subprocess, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import _common as C  # noqa: E402

_APP = C.test_app()
PY = (Path(_APP) if _APP else HERE) / "Contents" / "Resources" / "python_env" / "bin" / "python3"   # 엑셀을 읽는 openpyxl
EPOCH = 1288834974657
HOUR = 3600 * 1000
NOW = int(time.time() * 1000)


def sf(ms, seq=0):
    return str(((ms - EPOCH) << 22) + seq)


class World:
    def __init__(self, home: Path):
        self.home = home
        self.fx = home / "fake_x"
        self.fx.mkdir(parents=True, exist_ok=True)
        self.st = {"user": {"id": "1001", "screen_name": "tgt"}, "own": [], "timeline_extra": [],
                   "likes": [], "page": 20, "end_mode": "shaped", "search_hidden": []}
        self.seq = 0

    def _id(self, ms):
        self.seq += 1
        return sf(ms, self.seq % 4000)

    def add_tweet(self, ms, reply_to=None):
        t = {"id": self._id(ms), "ms": ms, "text": "t%d" % self.seq, "reply_to": reply_to}
        self.st["own"].append(t)
        return t["id"]

    def add_rt(self, ms):
        orig = sf(ms - 10 * 24 * HOUR, 7)
        rid = self._id(ms)
        self.st["timeline_extra"].append({"kind": "rt", "id": rid, "orig_id": orig, "ms": ms})
        return rid

    def add_like(self, ms, front=True):
        t = {"id": self._id(ms), "ms": ms, "author": "someone"}
        if front:
            self.st["likes"].insert(0, t)
        else:
            self.st["likes"].append(t)
        return t["id"]

    def save(self):
        (self.fx / "state.json").write_text(json.dumps(self.st), encoding="utf-8")

    def requests(self):
        p = self.fx / "requests.jsonl"
        return [json.loads(l) for l in p.read_text(encoding="utf-8").splitlines() if l.strip()] if p.exists() else []

    def clear_requests(self):
        (self.fx / "requests.jsonl").write_text("", encoding="utf-8")


def xlsx_ids(path: Path):
    if not path.exists():
        return set()
    code = ("import openpyxl,sys,json;wb=openpyxl.load_workbook(sys.argv[1],read_only=True);ws=wb.worksheets[0];"
            "print(json.dumps([str(r[0]) for r in ws.iter_rows(min_row=2,max_col=1,values_only=True) if r and r[0] is not None]))")
    out = subprocess.run([str(PY), "-I", "-B", "-c", code, str(path)], capture_output=True, text=True)
    try:
        return set(json.loads(out.stdout or "[]"))
    except ValueError:
        print("   xlsx read failed:", out.stderr[-300:])
        return set()


def xlsx_rows(path: Path):
    """id → {열 이름: 값} (첫 줄이 머리)"""
    if not path.exists():
        return {}
    code = ("import openpyxl,sys,json;wb=openpyxl.load_workbook(sys.argv[1],read_only=True);ws=wb.worksheets[0];"
            "rows=list(ws.iter_rows(values_only=True));h=[str(x) for x in rows[0]];"
            "print(json.dumps({str(r[0]):{h[i]:('' if r[i] is None else str(r[i])) for i in range(min(len(h),len(r)))} for r in rows[1:] if r and r[0] is not None}))")
    out = subprocess.run([str(PY), "-I", "-B", "-c", code, str(path)], capture_output=True, text=True)
    try:
        return json.loads(out.stdout or "{}")
    except ValueError:
        print("   xlsx read failed:", out.stderr[-300:])
        return {}


def blank_cols(path: Path, cols):
    """옛 꼴 흉내 — 그 열들을 비운다(엑셀을 직접 고쳐 쓴다)"""
    code = ("import openpyxl,sys,json;wb=openpyxl.load_workbook(sys.argv[1]);ws=wb.worksheets[0];h=[c.value for c in ws[1]];"
            "cols=json.loads(sys.argv[2]);idx=[h.index(c)+1 for c in cols if c in h]\n"
            "for r in range(2, ws.max_row+1):\n for i in idx: ws.cell(row=r,column=i).value=None\n"
            "wb.save(sys.argv[1])")
    subprocess.run([str(PY), "-I", "-B", "-c", code, str(path), json.dumps(cols)], capture_output=True, text=True)


def bad_rows(rows, cols, only=None):
    """그 열이 빈 줄 id"""
    return sorted(i for i, r in rows.items() if (only is None or i in only) and any(not r.get(c, "") for c in cols))


def marks(user_dir: Path):
    out = {}
    for p in glob.glob(str(user_dir / ".hanishiki_walk_*.json")):
        try:
            out[os.path.basename(p)[len(".hanishiki_walk_"):-5]] = json.loads(Path(p).read_text())
        except Exception:
            out[os.path.basename(p)] = {}
    return out


def main() -> int:
    check = C.Checks()
    if not C.iso_start():
        return 2
    home = C.ISO / "home"
    data = home / "Library" / "Application Support" / "Hanishiki"
    ov = data / "script_overrides"
    ov.mkdir(parents=True, exist_ok=True)
    shutil.copy(HERE / "fake_x_daemon.py", ov / "twitter_daemon.py")
    save = C.ISO / "tmp" / "tw-e2e"
    shutil.rmtree(save, ignore_errors=True)
    save.mkdir(parents=True)
    udir = save / "twitter" / "tgt"
    w = World(home)

    # 기본 자료 — 20일 전 ~ 8시간 전: 제 글 60 · 답글 30 · 리트윗 10
    base_ids = []
    for i in range(90):
        ms = NOW - 20 * 24 * HOUR + i * int((20 * 24 - 8) * HOUR / 90)
        base_ids.append(w.add_tweet(ms, reply_to=("friend" if i % 3 == 2 else None)))
    for i in range(10):
        w.add_rt(NOW - 19 * 24 * HOUR + i * 40 * HOUR)
    w.save()

    def run(kind, count=0, label="", path=None, **extra):
        w.clear_requests()
        cfg = {"platform": "twitter", "target": "tgt", "type": kind, "path": str(path or save),
               "accounts": [{"auth_token": "akashi-test-token", "ct0": "akashi-test-ct0"}],
               "stopAfterKnown": 5, "realCapture": False, "downloadMedia": False, "excel": True,
               "mode": "all", "resumeMode": "both", "saveProgress": False, "count": count}
        cfg.update(extra)
        DONE = ("완료", "오류", "중지됨", "Error", "Done", "Stopped")
        with C.page() as p:
            def idle():
                return p.eval("(function(){var b=document.getElementById('twitter-stop-btn');return !b||b.disabled;})()", retry=True) is True
            t0 = time.time()
            while not idle() and time.time() - t0 < 300:   # 앞 판이 정말 끝날 때까지
                time.sleep(1)
            p.eval("(typeof platformStats!=='undefined') && (platformStats.twitter = {status: 'akashi-wait'}) && true")
            p.eval("backend.startCollection(%s)" % json.dumps(json.dumps(cfg)))
            t0 = time.time()
            while time.time() - t0 < 60 and C.platform_stats(p, "twitter").get("status") == "akashi-wait":
                time.sleep(0.5)
            st, took = C.wait_status(p, "twitter", DONE, 900, step=1.0)
            t1 = time.time()
            while not idle() and time.time() - t1 < 120:
                time.sleep(1)
        time.sleep(4)   # 끝자락(표시 되돌리기 · 엑셀)
        reqs = w.requests()
        print("   [%s] %s %s · %.0fs · 요청 %d" % (label, kind, (st or {}).get("status"), took or -1, len(reqs)), flush=True)
        return st, reqs

    def until_searches(reqs, replies=None):
        out = [r for r in reqs if r["action"] == "search" and "until:" in r.get("query", "")]
        if replies is not None:
            out = [r for r in out if ("filter:replies" in r["query"]) == replies]
        return out

    xl = udir / "tgt_complete.xlsx"

    # ── A. 첫 판(끝까지) → 새 글 뒤 일찍 멈춤 ──
    st, reqs = run("tweets", label="A1")
    ids = xlsx_ids(xl)
    want = {t["id"] for t in w.st["own"]} | {x["id"] for x in w.st["timeline_extra"]}
    check("A1 첫 판이 끝났다", (st or {}).get("status") in ("완료", "Done"), st)
    check("A1 제 글 · 답글 · 리트윗을 다 받았다 %d/%d" % (len(ids & want), len(want)), want <= ids, sorted(want - ids)[:3])
    m = marks(udir)
    check("A1 검색 · 타임라인 표시에 top", int(m.get("tw_tweets_search_m0", {}).get("top", 0)) > 0
          and int(m.get("tw_tweets_timeline_m0", {}).get("top", 0)) > 0, m)
    check("A1 은 끝까지 되짚었다(until: 검색이 있다)", len(until_searches(reqs)) > 0, len(until_searches(reqs)))
    rows = xlsx_rows(xl)
    check("X1 트윗 엑셀 — 모든 줄에 글 주소 · 작성자 · 종류 · 언어", not bad_rows(rows, ["tweet_url", "author_username", "type", "language"]),
          bad_rows(rows, ["tweet_url", "author_username", "type", "language"])[:3])
    with_media = [t["id"] for t in w.st["own"] if t["id"][-3:].isdigit() and int(t["id"][-3:]) % 4 == 0]
    check("X1 미디어 있는 글에 미디어 종류 · 주소", not bad_rows(rows, ["media_type", "media_urls"], set(with_media)) and with_media,
          bad_rows(rows, ["media_type", "media_urls"], set(with_media))[:3])
    new_a = [w.add_tweet(NOW - 60 * 60 * 1000 + k * 60000) for k in range(3)] + \
            [w.add_tweet(NOW - 50 * 60 * 1000, reply_to="friend"), w.add_rt(NOW - 40 * 60 * 1000)]
    w.save()
    st, reqs = run("tweets", label="A2")
    ids = xlsx_ids(xl)
    check("A2 새 글 다섯(제 글 · 답글 · 리트윗)을 받았다", set(new_a) <= ids, sorted(set(new_a) - ids))
    check("A2 는 일찍 멈췄다(until: 되짚기 없음)", len(until_searches(reqs)) == 0, [r["query"] for r in until_searches(reqs)][:2])
    check("A2 타임라인도 한 쪽에서 멈췄다", len([r for r in reqs if r["action"] == "user_tweets"]) <= 2,
          [r for r in reqs if r["action"] == "user_tweets"])

    # ── B. 한 계정을 '트윗' · '답글' 두 줄로(r45 의 한계) ──
    st, reqs = run("replies", label="B1")
    check("B1 답글 첫 판은 끝까지 되짚었다", len(until_searches(reqs, replies=True)) > 0)
    check("B1 답글 표시가 적혔다", int(marks(udir).get("tw_tweets_replies_m0", {}).get("top", 0)) > 0, marks(udir).keys())
    new_b = [w.add_tweet(NOW - 30 * 60 * 1000, reply_to="friend"), w.add_tweet(NOW - 29 * 60 * 1000, reply_to="friend"),
             w.add_tweet(NOW - 28 * 60 * 1000), w.add_tweet(NOW - 27 * 60 * 1000)]
    w.save()
    st, reqs = run("tweets", label="B2")
    check("B2 트윗 줄이 새 답글 · 제 글을 받았다", set(new_b) <= xlsx_ids(xl))
    check("B2 뒤에도 답글 표시가 남았다(r45 는 지웠다)", "tw_tweets_replies_m0" in marks(udir), marks(udir).keys())
    new_b2 = [w.add_tweet(NOW - 20 * 60 * 1000, reply_to="friend")]
    w.save()
    st, reqs = run("replies", label="B3")
    check("B3 답글 줄이 일찍 멈췄다(되짚기 없음)", len(until_searches(reqs, replies=True)) == 0,
          [r["query"] for r in until_searches(reqs, replies=True)][:2])
    check("B3 새 답글을 받았다", set(new_b2) <= xlsx_ids(xl))

    # ── C. 대화 묶음 다섯이 새 리트윗 위에(검토 r6 의 반례) ──
    roots = [i for i in base_ids if not any(t["id"] == i and t.get("reply_to") for t in w.st["own"])][-5:]
    r_id = w.add_rt(NOW - 15 * 60 * 1000)
    for k, root in enumerate(roots):
        rep = w.add_tweet(NOW - 10 * 60 * 1000 + k * 60000, reply_to="tgt")
        w.st["timeline_extra"].append({"kind": "module", "ids": [root, rep]})
    w.save()
    st, reqs = run("tweets", label="C1")
    check("C1 대화 묶음 다섯 아래의 새 리트윗을 받았다", r_id in xlsx_ids(xl), r_id)

    # ── D. 최대 수에서 끊긴 판 → 표시를 지우지 않고 다음 판이 사이를 메운다 ──
    new_d = [w.add_tweet(NOW - 5 * 60 * 1000 + k * 5000) for k in range(25)]
    w.save()
    st, reqs = run("tweets", count=10, label="D1")
    got = len(set(new_d) & xlsx_ids(xl))
    check("D1 은 최대 수에서 끊겼다(새 글 일부만)", 0 < got < 25, got)
    check("D1 뒤에도 검색 · 타임라인 표시가 남았다", {"tw_tweets_search_m0", "tw_tweets_timeline_m0"} <= set(marks(udir)), marks(udir).keys())
    st, reqs = run("tweets", label="D2")
    check("D2 가 남은 새 글을 다 받았다", set(new_d) <= xlsx_ids(xl), len(set(new_d) - xlsx_ids(xl)))
    check("D2 는 일찍 멈췄다(되짚기 없음)", len(until_searches(reqs)) == 0, len(until_searches(reqs)))

    # ── L. 좋아요: 목록 끝(트윗 없는 쪽) → 표시 · 일찍 멈춤 · errors 로 끝나면 표시 없음 ──
    for k in range(50):
        w.add_like(NOW - 30 * 24 * HOUR + k * HOUR, front=True)
    w.save()
    st, reqs = run("likes", label="L1")
    lx = udir / "tgt_likes.xlsx"
    check("L1 좋아요 50개", len(xlsx_ids(lx)) == 50, len(xlsx_ids(lx)))
    check("L1 목록 끝에서 표시가 적혔다", any(k.startswith("tw_likes") for k in marks(udir)), marks(udir).keys())
    LCOLS = ["tweet_url", "author_username", "author_name", "type", "language", "possibly_sensitive", "conversation_id", "source"]
    lrows = xlsx_rows(lx)
    check("X2 좋아요 엑셀 — 글 주소 · 작성자 · 종류 · 언어 · 민감 · 대화 · 출처가 찼다(예전엔 빈칸)", lrows and not bad_rows(lrows, LCOLS),
          bad_rows(lrows, LCOLS)[:3])
    lmedia = [t["id"] for t in w.st["likes"] if t["id"][-3:].isdigit() and int(t["id"][-3:]) % 4 == 0]
    check("X2 좋아요의 미디어 종류 · 주소", lmedia and not bad_rows(lrows, ["media_type", "media_urls"], set(lmedia)),
          bad_rows(lrows, ["media_type", "media_urls"], set(lmedia))[:3])
    nl = [w.add_like(NOW - 60000 * k) for k in range(3)]
    w.save()
    blank_cols(lx, ["tweet_url", "author_username", "author_name", "language"])   # 옛 판이 쓴 좋아요 엑셀 흉내
    st, reqs = run("likes", label="L2")
    lrows = xlsx_rows(lx)
    seen = {t["id"] for t in w.st["likes"][:7]}   # 새 셋 + 받아 둔 넷(다섯째에서 멈추고 그것은 처리하지 않는다)
    check("X3 다시 본 좋아요 줄은 빈칸이 다 채워졌다", not bad_rows(lrows, ["tweet_url", "author_username", "author_name", "language"], seen),
          bad_rows(lrows, ["tweet_url", "author_username", "author_name", "language"], seen)[:3])
    check("X3 다시 보지 않은 줄도 글 주소는 ID 로 채워졌다", not bad_rows(lrows, ["tweet_url"]), bad_rows(lrows, ["tweet_url"])[:3])
    check("X3 이미 차 있던 칸은 그대로", all(lrows[i].get("source") for i in lrows), "")
    check("L2 새 좋아요 셋을 받았다", set(nl) <= xlsx_ids(lx))
    check("L2 는 첫 쪽에서 멈췄다", len([r for r in reqs if r["action"] == "likes"]) == 1, [r for r in reqs if r["action"] == "likes"])
    for k in list(marks(udir)):
        if k.startswith("tw_likes"):
            os.remove(udir / (".hanishiki_walk_%s.json" % k))
    w.st["end_mode"] = "errors"
    w.save()
    st, reqs = run("likes", label="L3")
    check("L3 errors 로 끝난 목록은 표시를 적지 않는다", not any(k.startswith("tw_likes") for k in marks(udir)), marks(udir).keys())

    w.st["bookmarks"] = [{"id": sf(NOW - 40 * 24 * HOUR + k * HOUR, 3000 + k), "ms": NOW - 40 * 24 * HOUR + k * HOUR, "author": "someone"}
                         for k in range(30)][::-1]
    w.st["end_mode"] = "shaped"
    w.save()
    st, reqs = run("bookmarks", label="BK")
    brows = xlsx_rows(udir / "tgt_bookmarks.xlsx")
    check("X4 북마크 30개 · 칸이 찼다", len(brows) == 30 and not bad_rows(brows, LCOLS), (len(brows), bad_rows(brows, LCOLS)[:3]))

    # ── K. 캡처를 끈 판이 진짜 캡처를 덮지 않는다 ──
    capdir = udir / "captures" / "bookmarks"
    REAL = "<!-- Page saved with SingleFile --><html>진짜 페이지</html>"
    st, reqs = run("bookmarks", label="K1", stopAfterKnown=0)
    cards = sorted(capdir.glob("*.html"))
    check("K1 캡처를 끈 판은 합성 카드를 만든다", len(cards) == 30, len(cards))
    for c in cards:
        c.write_text(REAL, encoding="utf-8")   # 앞서 켜고 받아 둔 진짜 캡처라 치자
    st, reqs = run("bookmarks", label="K2", stopAfterKnown=0)
    kept = sum(1 for c in cards if c.read_text(encoding="utf-8") == REAL)
    check("K2 끈 판이 다시 돌아도 진짜 캡처 30개가 그대로", kept == 30, kept)
    shutil.rmtree(capdir)
    st, reqs = run("bookmarks", label="K3", stopAfterKnown=0, realCaptureMode="off")
    n_off = len(list(capdir.glob("*.html"))) if capdir.exists() else 0
    check("K3 '안 함' 이면 카드도 만들지 않는다(본 걸음과 같다)", n_off == 0, n_off)

    # ── S. 검색 커서가 날 한가운데서 말라도 그 날의 나머지를 놓치지 않는다(타임라인이 메워 주지 못하는 옛 구간) ──
    save_s = C.ISO / "tmp" / "tw-e2e-s"
    shutil.rmtree(save_s, ignore_errors=True)
    save_s.mkdir(parents=True)
    w.st = {"user": {"id": "1001", "screen_name": "tgt"}, "own": [], "timeline_extra": [], "likes": [], "page": 10,
            "end_mode": "shaped", "search_hidden": [], "search_depth": 25, "timeline_off": True}
    t0 = NOW - 40 * 24 * HOUR
    for i in range(96):                     # 3시간마다 12일 — 25개면 사흘 남짓, 끊기는 곳은 늘 날 한가운데다
        w.add_tweet(t0 + i * 3 * HOUR)
    for i in range(48):                     # 답글은 검색으로만 온다
        w.add_tweet(t0 + i * 6 * HOUR + HOUR, reply_to="friend")
    w.save()
    st, reqs = run("tweets", label="S1", path=save_s, stopAfterKnown=0)
    uds = save_s / "twitter" / "tgt"
    ids = xlsx_ids(uds / "tgt_complete.xlsx")
    want = {t["id"] for t in w.st["own"]}
    check("S1 검색이 날 한가운데서 끊겨도 트윗 · 답글을 모두 받았다 %d/%d" % (len(ids & want), len(want)), want <= ids,
          sorted(want - ids)[:3])
    check("S1 그 날을 다시 포함해 이었다(until: 이 여러 번)", len(until_searches(reqs)) >= 3, len(until_searches(reqs)))

    # ── M. 일괄 다운로드 창 — 화면에서 대상을 넣고 시작을 누른 것처럼 ──
    save_m = C.ISO / "tmp" / "tw-e2e-m"
    shutil.rmtree(save_m, ignore_errors=True)
    save_m.mkdir(parents=True)
    w.st = {"user": {"id": "1001", "screen_name": "tgt"}, "own": [], "timeline_extra": [], "likes": [], "page": 20,
            "end_mode": "shaped", "search_hidden": []}
    for i in range(4):
        w.add_tweet(NOW - 5 * 24 * HOUR + i * HOUR)
    w.save()
    w.clear_requests()

    def batch(names, path):
        with C.page() as p:
            p.eval("""(function(){ accounts.twitter = [{name:'akashi', auth_token:'akashi-test-token', ct0:'akashi-test-ct0'}];
                      var g = function(i){return document.getElementById(i)};
                      g('twitter-path').value = %s; g('twitter-type').value = 'tweets';
                      g('twitter-media').checked = false; g('twitter-excel').checked = true;
                      g('twitter-real-capture').checked = false; g('twitter-real-capture-mode').value = 'off';
                      openBatchModal(); clearBatchTargets();
                      g('batch-platform').value = 'twitter'; g('batch-input').value = %s;
                      addBatchTargets(); startBatch(); return true; })()""" % (json.dumps(path), json.dumps("\n".join(names))))
            t0 = time.time()
            txt = ""
            while time.time() - t0 < 300:
                txt = p.eval("(document.getElementById('batch-log')||{}).innerText||''", retry=True) or ""
                if "일괄 다운로드 종료" in txt:
                    break
                time.sleep(1)
            p.eval("closeBatchModal()")
        return txt

    names = ["m1", "m2", "m3", "m4"]
    txt = batch(names, str(save_m))
    looked = [r.get("screen_name") for r in w.requests() if r["action"] == "user_by_screen_name"]
    check("M1 일괄이 끝났다", "일괄 다운로드 종료" in txt, txt[-200:])
    check("M1 대상 넷을 하나도 건너뛰지 않고 받았다", all(n in looked for n in names), looked)
    check("M1 대상마다 한 번씩 '완료' — 받지 않은 대상을 완료로 적지 않는다", all(txt.count(n + " — 완료") == 1 for n in names), txt[-300:])
    check("M1 넷 다 폴더가 생겼다", all((save_m / "twitter" / n).is_dir() for n in names),
          sorted(x.name for x in (save_m / "twitter").iterdir()) if (save_m / "twitter").exists() else [])
    txt = batch(["m5", "m6"], "")
    check("M2 저장 경로가 없어도 일괄이 멈추지 않고 끝난다(대상마다 오류)", "일괄 다운로드 종료" in txt
          and txt.count("— 오류") == 2, txt[-300:])
    # 일괄 도중에 탭의 중지 — 창이 '진행 중' 에 멈추지 않는다
    with C.page() as p:
        p.eval("""(function(){ var g = function(i){return document.getElementById(i)};
                  g('twitter-path').value = %s; openBatchModal(); clearBatchTargets();
                  g('batch-platform').value = 'twitter'; g('batch-input').value = 'n1\\nn2';
                  addBatchTargets(); startBatch(); return true; })()""" % json.dumps(str(save_m)))
        time.sleep(4)
        p.eval("backend.stopCollection('twitter')")
        t0 = time.time()
        txt = ""
        while time.time() - t0 < 60:
            txt = p.eval("(document.getElementById('batch-log')||{}).innerText||''", retry=True) or ""
            if "일괄 다운로드 종료" in txt:
                break
            time.sleep(1)
        p.eval("closeBatchModal()")
        check("M3 일괄 도중 탭에서 중지하면 일괄도 끝난다", "일괄 다운로드 종료" in txt, txt[-200:])
        t1 = time.time()
        while time.time() - t1 < 60 and not p.eval(
                "(function(){var b=document.getElementById('twitter-stop-btn');return !b||b.disabled;})()", retry=True):
            time.sleep(1)
    time.sleep(3)
    # 다른 판이 돌고 있으면 — 그 대상은 받지 못했다고 적고 끝난다(돌던 판의 끝을 그 대상의 끝으로 적지 않는다)
    with C.page() as p:
        cfg = {"platform": "twitter", "target": "tgt", "type": "tweets", "path": str(save_m),
               "accounts": [{"auth_token": "akashi-test-token", "ct0": "akashi-test-ct0"}],
               "realCapture": False, "downloadMedia": False, "excel": True, "mode": "all", "resumeMode": "both", "saveProgress": False, "count": 0}
        p.eval("backend.startCollection(%s)" % json.dumps(json.dumps(cfg)))
        time.sleep(2)
        p.eval("""(function(){ var g = function(i){return document.getElementById(i)};
                  openBatchModal(); clearBatchTargets(); g('batch-platform').value = 'twitter'; g('batch-input').value = 'r1\\nr2';
                  addBatchTargets(); startBatch(); return true; })()""")
        t0 = time.time()
        txt = ""
        while time.time() - t0 < 120:
            txt = p.eval("(document.getElementById('batch-log')||{}).innerText||''", retry=True) or ""
            if "일괄 다운로드 종료" in txt:
                break
            time.sleep(1)
        p.eval("closeBatchModal()")
        check("M4 다른 판이 돌고 있으면 받지 못한 대상은 오류로 적고 끝난다", "일괄 다운로드 종료" in txt and "r1 — 오류" in txt
              and "r1 — 완료" not in txt, txt[-250:])
        t1 = time.time()
        while time.time() - t1 < 120 and not p.eval(
                "(function(){var b=document.getElementById('twitter-stop-btn');return !b||b.disabled;})()", retry=True):
            time.sleep(1)

    C.keep_log("early_stop_app.log")
    C.iso_stop()
    return check.summary()


if __name__ == "__main__":
    sys.exit(main())
