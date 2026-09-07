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
    for root, dirs, files in os.walk(d, followlinks=False, onerror=_note_walk_error):
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


HASH_FILE = "__ARCHIVE_HASHES__.json"


def _hash(path, chunk=1 << 20):
    import hashlib
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(chunk)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def _walk_files(d):
    """세는 규칙은 scan() 과 같아야 한다 — 두 곳이 갈라지면 또 거짓 경보가 난다."""
    for root, dirs, files in os.walk(d, followlinks=False, onerror=_note_walk_error):
        for f in files:
            if f.startswith('.') or f.startswith('__ARCHIVE_MANIFEST') \
               or f.startswith('__CHERNOBYL_MANIFEST') or f == HASH_FILE \
               or f in ('Thumbs.db', 'desktop.ini'):
                continue
            if '/.abiwa_' in root or '/.rsync-' in root:
                continue
            p = os.path.join(root, f)
            if os.path.islink(p):
                continue
            yield p


def record_hashes(d):
    """파일별 해시를 남긴다. 이미 있고 크기·수정시각이 그대로면 다시 읽지 않는다.

    ★ 왜 크기·수정시각으로 건너뛰나.
      전부 다시 읽어도 2.5GB 에 6초쯤이라 못 할 일은 아니다(실측). 그래도
      수집이 끝날 때마다 보관함 전체를 읽는 것은 외장 디스크·NAS 에서는 다르다.
      바뀐 것만 읽으면 두 번째부터는 사실상 공짜다.
    ★ 크기·시각이 같은데 내용만 바뀌는 경우는 이 방식으로 못 잡는다. 그건
      '지금 파일이 그때와 같은가' 를 보는 대조 쪽(verify)의 일이다.
    """
    path = os.path.join(d, HASH_FILE)
    old = {}
    try:
        with open(path, encoding="utf-8") as f:
            old = json.load(f).get("files", {})
    except Exception:
        old = {}

    new = {}
    hashed = reused = 0
    for p in _walk_files(d):
        rel = os.path.relpath(p, d)
        try:
            st = os.stat(p)
        except OSError:
            continue
        prev = old.get(rel)
        if prev and prev.get("size") == st.st_size and prev.get("mtime") == int(st.st_mtime):
            new[rel] = prev
            reused += 1
            continue
        try:
            new[rel] = {"size": st.st_size, "mtime": int(st.st_mtime), "sha256": _hash(p)}
            hashed += 1
        except OSError:
            continue

    tmp = path + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump({"created_at": __import__("datetime").datetime.now().isoformat(timespec="seconds"),
                   "count": len(new), "files": new}, f, ensure_ascii=False)
    os.replace(tmp, path)          # 쓰다 죽어도 옛 기록이 남는다
    return len(new), hashed, reused


def check_hashes(d):
    """기록된 해시와 지금 내용을 견준다. 기록이 없으면 (0,[],[]) 를 돌려준다."""
    path = os.path.join(d, HASH_FILE)
    try:
        with open(path, encoding="utf-8") as f:
            rec = json.load(f).get("files", {})
    except Exception:
        return 0, [], []
    changed, missing = [], []
    for rel, info in rec.items():
        p = os.path.join(d, rel)
        if not os.path.exists(p):
            missing.append(rel)
            continue
        try:
            if os.path.getsize(p) != info.get("size") or _hash(p) != info.get("sha256"):
                changed.append(rel)
        except OSError:
            missing.append(rel)
    return len(rec), changed, missing


_walk_errors = []


def _note_walk_error(err):
    """os.walk 가 폴더를 못 읽으면 조용히 빈 것으로 넘긴다.

    ★ 그러면 권한 문제가 '파일이 사라졌습니다' 로 둔갑한다. 보관 앱에서 가장
      나쁜 오보다 — 멀쩡한 자료를 잃은 줄 알고 사람이 엉뚱한 복구를 시작한다.
      못 읽은 것은 못 읽었다고 따로 적는다.
    """
    _walk_errors.append("%s: %s" % (getattr(err, 'filename', '?'), err))


def _excluded_dir(path):
    """세는 쪽(scan·_walk_files)이 통째로 빼는 자리인가.

    ★ 왜 필요한가. 숨은 폴더를 건너뛰지 않게 바꾸면서 verify 가 /.abiwa_ ·/.rsync-
      안까지 들어가게 됐다. 그런데 세는 쪽은 그 안의 파일을 하나도 안 센다.
      그 안에 매니페스트가 하나라도 있으면 '기록 N개 → 지금 0개', 즉 100% 거짓
      '사라졌습니다' 가 난다. 지금 사용자 보관함에는 그런 매니페스트가 없지만,
      규칙이 어긋난 채로 두면 언젠가 그 자리에 하나 생기는 날 터진다.
    """
    return '/.abiwa_' in path or '/.rsync-' in path


def _manifest_in(files, cur=None):
    """폴더에 남은 매니페스트 파일 이름을 돌려준다. 없으면 None.

    ★ 옛 이름도 알아본다. 앱 이름이 네 번 바뀌는 동안 사용자의 보관함에는
      __CHERNOBYL_MANIFEST__.json 이 이미 쌓여 있다. C++ 쪽(isManifestName)은
      진작 두 이름을 다 알아보는데 여기만 새 이름을 고집하고 있었다.
      그 바람에 옛 이름을 가진 폴더 30 개가 통째로 대조에서 빠지고도
      '문제 0 건' 이 나왔다 — 보고도 못 본 것이니, 보관 앱에서 가장 나쁜 고장이다.
      세는 규칙(scan·_walk_files)은 진작 두 이름을 다 제외하고 있었다.
      제외는 맞춰 놓고 탐색만 빠뜨린 것이라 더 오래 숨었다.
      둘 다 있으면 새 이름을 쓴다 — 그쪽이 나중에 쓰인 것이다.
    """
    have = [n for n in ("__ARCHIVE_MANIFEST__.json", "__CHERNOBYL_MANIFEST__.json")
            if n in files]
    if not have:
        return None
    if len(have) == 1 or cur is None:
        return have[0]
    # ★ 둘 다 있으면 '새 이름' 이 아니라 '나중에 쓴 것' 을 고른다.
    #   이름만 보고 새 이름을 집으면, 옛 이름 쪽이 더 최근인 폴더에서 낡은 기준으로
    #   대조하게 된다 — 그 사이에 늘어난 파일을 못 보거나, 줄어든 것을 못 잡는다.
    #   기준이 틀리면 대조 자체가 거짓말이 된다.
    def _when(n):
        try:
            with open(os.path.join(cur, n), encoding="utf-8") as f:
                return json.load(f).get("created_at", "")
        except Exception:
            return ""
    return max(have, key=_when)


def verify(root):
    out = []
    checked = 0
    # ★ 숨은 폴더를 건너뛰지 않는다. '.・🫀︴hibiki ꨄ︎…' 처럼 점으로 시작하는
    #   진짜 보관 폴더가 있다. 건너뛰면 그 안의 매니페스트를 영영 못 찾는다.
    #   세는 쪽(scan·_walk_files)도 폴더가 아니라 '파일 이름' 으로만 거른다.
    for cur, dirs, files in os.walk(root, followlinks=False, onerror=_note_walk_error):
        if _excluded_dir(cur):
            continue
        mname = _manifest_in(files, cur)
        if mname is None:
            continue
        checked += 1
        mpath = os.path.join(cur, mname)
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
        # 해시 기록이 있으면 '내용까지' 본다 — 크기가 같은 손상은 이것으로만 잡힌다.
        n_rec, changed, missing = check_hashes(cur)
        if n_rec:
            if missing:
                out.append({"dir": rel, "level": "fail",
                            "msg": "기록된 파일 %d개가 없습니다 — 예: %s"
                                   % (len(missing), missing[0][:60])})
            if changed:
                out.append({"dir": rel, "level": "fail",
                            "msg": "내용이 바뀐 파일 %d개 — 예: %s"
                                   % (len(changed), changed[0][:60])})
            if not missing and not changed:
                out.append({"dir": rel, "level": "ok",
                            "msg": "해시 %d개 전부 일치" % n_rec})

        if now_n >= was_n and now_b >= was_b and not empty:
            out.append({"dir": rel, "level": "ok",
                        "msg": "%d개 · %s (기록 %s 이후 줄어든 것 없음)"
                               % (now_n, human(now_b), when[:10])})
    return checked, out


if __name__ == "__main__":
    if len(sys.argv) < 2:
        print(json.dumps({"error": "대조할 폴더를 지정하십시오"}, ensure_ascii=False))
        sys.exit(1)
    if sys.argv[1] == "--record":
        if len(sys.argv) < 3:
            print(json.dumps({"error": "폴더를 지정하십시오"}, ensure_ascii=False)); sys.exit(1)
        base = sys.argv[2]
        done = []
        failed = []
        for cur, dirs, files in os.walk(base, followlinks=False, onerror=_note_walk_error):
            if _excluded_dir(cur) or _manifest_in(files, cur) is None:
                continue
            # ★ 한 폴더가 막혔다고 나머지를 통째로 버리지 않는다. 그리고 실패를
            #   삼킨 채 '✅ 완료' 라고 보고하지 않는다 — 해시를 남긴 줄 알고 넘어가면
            #   정작 대조할 기준이 없다는 것을 몇 달 뒤에야 알게 된다.
            try:
                n, hashed, reused = record_hashes(cur)
                done.append({"dir": os.path.relpath(cur, base),
                             "total": n, "hashed": hashed, "reused": reused})
            except Exception as e:
                failed.append({"dir": os.path.relpath(cur, base),
                               "error": "%s: %s" % (type(e).__name__, e)})
        out = {"recorded": done}
        if failed:
            out["failed"] = failed
            out["failed_total"] = len(failed)
        if _walk_errors:
            out["unreadable"] = _walk_errors[:20]
            out["unreadable_total"] = len(_walk_errors)
        print(json.dumps(out, ensure_ascii=False))
        sys.exit(0)

    root = sys.argv[1]
    if not os.path.isdir(root):
        print(json.dumps({"error": "폴더가 없습니다: %s" % root}, ensure_ascii=False))
        sys.exit(1)
    checked, results = verify(root)
    bad = sum(1 for r in results if r["level"] == "fail")
    warn = sum(1 for r in results if r["level"] == "warn")
    out = {"checked": checked, "fail": bad, "warn": warn, "results": results}
    if _walk_errors:
        # ★ '못 읽었다' 를 '없어졌다' 와 절대 섞지 않는다.
        out["unreadable"] = _walk_errors[:20]
        out["unreadable_total"] = len(_walk_errors)
    print(json.dumps(out, ensure_ascii=False))
