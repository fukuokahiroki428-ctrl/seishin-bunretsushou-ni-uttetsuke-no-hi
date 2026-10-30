#!/usr/bin/env python3
"""内閣会 '받을 때 옵션' — 격리 사본 + 가짜 X(fake_x_daemon.py)로 화면에서 고른 옵션대로 받는지. 네트워크 · 진짜 계정 없음.
시험용 계정 값은 여기서 만든 가짜이고 격리 사본의 설정에만 들어간다."""
import json, os, shutil, sys, time
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import early_stop as T  # noqa: E402  (World · xlsx_ids · sf · NOW · HOUR · C)

C = T.C


def main() -> int:
    check = C.Checks()
    if not C.iso_start():
        return 2
    home = C.ISO / "home"
    data = home / "Library" / "Application Support" / "Hanishiki"
    ov = data / "script_overrides"
    ov.mkdir(parents=True, exist_ok=True)
    shutil.copy(HERE / "fake_x_daemon.py", ov / "twitter_daemon.py")
    save = C.ISO / "tmp" / "nk-e2e"
    shutil.rmtree(save, ignore_errors=True)
    save.mkdir(parents=True)
    udir = save / "twitter" / "tgt"
    w = T.World(home)
    for i in range(20):
        w.add_tweet(T.NOW - 10 * 24 * T.HOUR + i * T.HOUR)
    for k in range(15):
        w.add_like(T.NOW - 20 * 24 * T.HOUR + k * T.HOUR, front=True)
    w.save()

    def wait_poll(p, secs=600):
        """内閣会 한 판이 끝날 때까지 — 로그에 '새 글 없음' 또는 '다운 완료' 가 새로 생기면"""
        t0 = time.time()
        while time.time() - t0 < secs:
            txt = p.eval("(document.getElementById('naikakukai-log')||{}).innerText||''", retry=True) or ""
            if txt.count("새 글 없음") + txt.count("다운 완료") > wait_poll.seen:
                wait_poll.seen = txt.count("새 글 없음") + txt.count("다운 완료")
                time.sleep(4)
                return txt
            time.sleep(2)
        return None
    wait_poll.seen = 0

    with C.page() as p:
        time.sleep(1.5)
        p.eval("""(function(){ accounts.twitter = [{name:'akashi', auth_token:'akashi-test-token', ct0:'akashi-test-ct0'}];
                  saveConfig(); switchTab('naikakukai');
                  document.getElementById('naikakukai-path').value = %s;
                  document.getElementById('naikakukai-platform').value = 'twitter';
                  document.getElementById('naikakukai-target').value = 'tgt';
                  addNaikakukaiWatch(); return true; })()""" % json.dumps(str(save)))
        time.sleep(1)
        info = p.eval("""(function(){ var b=document.getElementById('nk-opt-box'); if(!b) return null;
                  var t=b.querySelector('select.nk-ctl[data-k=type]');
                  return {rows: b.querySelectorAll('.nk-row').length, typeOpts: t ? Array.from(t.options).map(o=>o.value) : [],
                          tabOpts: Array.from(document.getElementById('twitter-type').options).map(o=>o.value)}; })()""")
        check("내각회 탭에 '받을 때 옵션' 이 있다", bool(info) and info["rows"] >= 8, info and info["rows"])
        check("유형 선택지가 트위터 탭과 같다(지금처럼 + 탭의 것)", bool(info) and info["typeOpts"][1:] == info["tabOpts"] and "likes" in info["tabOpts"],
              info and (info["typeOpts"][:4], len(info["tabOpts"])))

        def set_ctl(root_sel, k, v):
            return p.eval("""(function(){ var el=document.querySelector(%s+' .nk-ctl[data-k=%s]'); if(!el) return false;
                      el.value=%s; el.dispatchEvent(new Event('change',{bubbles:true})); return true; })()"""
                          % (json.dumps(root_sel), k, json.dumps(v)))
        ok = all([set_ctl('#nk-opt-box', 'type', 'likes'), set_ctl('#nk-opt-box', 'excel', '1'),
                  set_ctl('#nk-opt-box', 'downloadMedia', '0'), set_ctl('#nk-opt-box', 'realCaptureMode', 'off')])
        opts = p.eval("JSON.stringify(naikakukaiOptions)")
        check("화면에서 고른 것이 내각회 옵션이 된다", ok and json.loads(opts).get("twitter") ==
              {"type": "likes", "excel": True, "downloadMedia": False, "realCaptureMode": "off"}, opts)

        w.clear_requests()
        p.eval("startNaikakukai()")
        txt = wait_poll(p)
        reqs = w.requests()
        check("1판: 내각회가 플랫폼 옵션대로 좋아요를 받았다", txt is not None and any(r["action"] == "likes" for r in reqs)
              and not any(r["action"] == "search" for r in reqs), [r["action"] for r in reqs][:6])
        check("1판: 옵션 줄이 기록에 있다", txt is not None and "type=likes" in txt and "excel=켬" in txt,
              (txt or "")[-300:])
        lx = udir / "tgt_likes.xlsx"
        check("1판: Excel 저장 켬 → 좋아요 엑셀 15줄", len(T.xlsx_ids(lx)) == 15, len(T.xlsx_ids(lx)))

        # 대상만 따로 — 유형 = 트윗(대상의 것이 앞선다)
        p.eval("openNaikakukaiWatchOptions(0)")
        time.sleep(0.8)
        ok2 = set_ctl('#nk-watch-modal', 'type', 'tweets')
        wo = p.eval("JSON.stringify(naikakukaiWatches[0].options||{})")
        check("감시 대상 옵션 창에서 유형을 따로 정했다", ok2 and json.loads(wo) == {"type": "tweets"}, wo)
        p.eval("document.getElementById('nk-watch-modal').classList.add('hidden'); stopNaikakukai()")
        time.sleep(3)
        w.clear_requests()
        p.eval("startNaikakukai()")
        txt = wait_poll(p)
        reqs = w.requests()
        check("2판: 대상의 옵션(트윗)이 내각회 옵션(좋아요)보다 앞선다", txt is not None and any(r["action"] in ("search", "user_tweets") for r in reqs)
              and not any(r["action"] == "likes" for r in reqs), [r["action"] for r in reqs][:6])
        check("2판: 그 밖의 내각회 옵션(Excel 켬)은 그대로 물려받는다", len(T.xlsx_ids(udir / "tgt_complete.xlsx")) >= 20,
              len(T.xlsx_ids(udir / "tgt_complete.xlsx")))
        p.eval("stopNaikakukai()")
        time.sleep(3)

    cfgp = data / "hanishiki_config.json"
    cfg = json.loads(cfgp.read_text(encoding="utf-8")) if cfgp.exists() else {}
    check("설정 파일에 내각회 옵션이 남았다", (cfg.get("naikakukaiOptions") or {}).get("twitter", {}).get("type") == "likes",
          cfg.get("naikakukaiOptions"))
    ws = cfg.get("naikakukaiWatches") or []
    check("설정 파일에 대상의 옵션이 남았다", bool(ws) and (ws[0].get("options") or {}).get("type") == "tweets", ws[:1])

    C.keep_log("naikakukai_app.log")
    C.iso_stop()
    return check.summary()


if __name__ == "__main__":
    sys.exit(main())
