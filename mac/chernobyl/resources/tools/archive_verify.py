#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""archive_verify.py — 예전에 남긴 기록과 지금 디스크를 대조한다.

왜 필요한가
-----------
수집할 때마다 폴더마다 __ARCHIVE_MANIFEST__.json 을 남기고 있었다.
파일 수·전체 크기·확장자별·폴더별 통계가 다 들어 있다.
그런데 그것을 '다시 읽어 대조하는' 코드가 어디에도 없었다 — 쓰기만 했다.

그래서 무결성 검사는 내려받는 순간의 파일만 본다. 몇 년 뒤 외장 디스크에서
파일이 조용히 사라지거나 0바이트가 되어도 아무도 모른다. 보관이 목적인 앱에서
이건 조용한 손실이다.

무엇을 하나
-----------
manifest 를 찾아 그때의 기록과 지금을 견준다.
  · 파일 수가 줄었나        → 사라진 것이 있다
  · 전체 크기가 줄었나      → 잘렸거나 비었다
  · 0바이트 파일이 생겼나   → 쓰다 만 것이다
늘어난 것은 문제가 아니다(계속 수집하니까). 줄어든 것만 본다.

정직한 한계
-----------
· 파일 하나하나의 해시는 안 본다. manifest 에 해시가 없기 때문이다.
  같은 크기로 내용만 바뀌는 손상은 못 잡는다. 그런 손상은 드물고,
  잡으려면 수집 때부터 해시를 남겨야 한다(앞으로의 과제).
· 여기서 하는 것은 '사라짐·잘림' 을 잡는 것이다. 실제로 나는 고장은 대부분 그쪽이다.
"""

import json, os, sys


def scan(d):
    """★ 세는 규칙은 manifest 를 만드는 쪽과 '똑같아야' 한다.
    한 글자라도 다르면 매번 거짓 경보가 난다. 실제로 겪었다 —
    처음엔 숨김 '폴더' 를 통째로 건너뛰었는데, 팔로워 폴더 중에
    '.・🫀︴hibiki ꨄ︎…' 처럼 점으로 시작하는 진짜 데이터가 있었다.
    그 바람에 멀쩡한 보관함을 '178개 유실' 로 잘못 읽었다.

    생성 쪽 규칙(HanishikiBackend.cpp)은 이렇다:
      · 파일 '이름' 이 . 로 시작하면 제외 — 폴더는 건너뛰지 않는다
      · __ARCHIVE_MANIFEST / __CHERNOBYL_MANIFEST 제외
      · Thumbs.db · desktop.ini 제외
      · 경로에 /.abiwa_ 나 /.rsync- 가 들어가면 제외
      · 심볼릭 링크는 따라가지 않는다
    """
    n = 0
    total = 0
    empty = []
    for root, dirs, files in os.walk(d, followlinks=False):
        for f in files:
            if f.startswith('.') or f.startswith('__ARCHIVE_MANIFEST') \
               or f.startswith('__CHERNOBYL_MANIFEST') \
               or f in ('Thumbs.db', 'desktop.ini'):
                continue
            if '/.abiwa_' in root or '/.rsync-' in root:
                continue

            if os.path.islink(os.path.join(root, f)):
                continue
            p = os.path.join(root, f)
            try:
                sz = os.path.getsize(p)
            except OSError:
                continue
            n += 1
            total += sz
            # ★ 세는 것은 생성 쪽과 똑같이 한다(임시 폴더도 포함). 규칙이 한 글자라도
            #   다르면 매번 거짓 경보가 난다 — 실제로 두 번 겪었다.
            #   다만 '0바이트 경고' 만은 임시 작업 폴더를 뺀다. 거기 파일은
            #   수집 중에 생겼다 지워지는 것이라 0바이트여도 손상이 아니다.
            if sz == 0 and '/.tmp' not in root:
                empty.append(os.path.relpath(p, d))
    return n, total, empty


def human(b):
    for u in ("B", "KB", "MB", "GB", "TB"):
        if b < 1024 or u == "TB":
            return "%.1f %s" % (b, u)
        b /= 1024.0


def verify(root):
    out = []
    checked = 0
    for cur, dirs, files in os.walk(root):
        dirs[:] = [x for x in dirs if not x.startswith('.')]
        if "__ARCHIVE_MANIFEST__.json" not in files:
            continue
        checked += 1
        mpath = os.path.join(cur, "__ARCHIVE_MANIFEST__.json")
        try:
            with open(mpath, encoding="utf-8") as f:
                m = json.load(f)
            s = m.get("summary", {})
            was_n = int(s.get("total_files", 0))
            was_b = int(s.get("total_size_bytes", 0))
            when = m.get("created_at", "?")
        except Exception as e:
            out.append({"dir": cur, "level": "warn",
                        "msg": "기록을 읽지 못했습니다 (%s)" % type(e).__name__})
            continue

        now_n, now_b, empty = scan(cur)
        rel = os.path.relpath(cur, root)
        if now_n < was_n:
            out.append({"dir": rel, "level": "fail",
                        "msg": "파일이 %d개 사라졌습니다 (%d → %d · 기록 %s)"
                               % (was_n - now_n, was_n, now_n, when)})
        if now_b < was_b:
            out.append({"dir": rel, "level": "fail",
                        "msg": "용량이 %s 줄었습니다 (%s → %s)"
                               % (human(was_b - now_b), human(was_b), human(now_b))})
        if empty:
            out.append({"dir": rel, "level": "warn",
                        "msg": "0바이트 파일 %d개 — 예: %s" % (len(empty), empty[0][:60])})
        if now_n >= was_n and now_b >= was_b and not empty:
            out.append({"dir": rel, "level": "ok",
                        "msg": "%d개 · %s (기록 %s 이후 줄어든 것 없음)"
                               % (now_n, human(now_b), when[:10])})
    return checked, out


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(json.dumps({"error": "대조할 폴더를 지정하십시오"}, ensure_ascii=False))
        sys.exit(1)
    root = sys.argv[1]
    if not os.path.isdir(root):
        print(json.dumps({"error": "폴더가 없습니다: %s" % root}, ensure_ascii=False))
        sys.exit(1)
    checked, results = verify(root)
    bad = sum(1 for r in results if r["level"] == "fail")
    warn = sum(1 for r in results if r["level"] == "warn")
    print(json.dumps({"checked": checked, "fail": bad, "warn": warn,
                      "results": results}, ensure_ascii=False))
