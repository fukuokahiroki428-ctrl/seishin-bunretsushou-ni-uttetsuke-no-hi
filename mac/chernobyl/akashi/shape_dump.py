#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""응답 모양 보기 — 저장해 둔 JSON 응답의 '모양'(이름·형·개수)만 보여 준다. 값은 찍지 않는다.

    python3 akashi/shape_dump.py resp.json                 # 나무 모양
    python3 akashi/shape_dump.py resp.json --find posts    # 'posts' 가 어디 있나
    python3 akashi/shape_dump.py resp.json --arrays        # 객체 배열(목록 후보)들
    python3 akashi/shape_dump.py old.json --compare new.json   # 무엇이 바뀌었나
    pbpaste | python3 akashi/shape_dump.py - --extract     # HTML·'for (;;);' 섞인 것에서 JSON 을 찾아

★ 왜 값을 안 찍나
  응답에는 계정 아이디·토큰·쿠키·남의 글이 섞여 있다. 모양만 있으면 고치는 데 충분하고, 그대로 기록에
  붙이거나 남에게 보여 줘도 새지 않는다. --safe-values 도 참거짓·null 과 글자 수만 보인다.

★ 이것으로 무엇을 고치나 — 「완료: 0개」
  바깥 서비스가 칸 이름을 바꾸면(팬박스 items → posts 처럼) 앱은 옛 이름을 찾다 0개를 읽는다. 옛 응답과
  새 응답을 --compare 하면 사라진 길과 새로 생긴 길이 나란히 나온다. 새 이름을 찾았으면 새 판을 굽지 않고
  고침 꾸러미의 shape_aliases.json 에 별명으로 넣으면 된다(docs/응답모양.md · docs/고침꾸러미.md).
  앱이 읽는 방식은 src/utils/JsonShape.h — pick(여러 이름) · findArrayOfObjects(이름을 몰라도 목록 찾기) ·
  describe(무엇이 왔는지 칸 이름을 나열) 와 같은 눈으로 본다.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import OrderedDict

TYPE = {dict: "객체", list: "배열", str: "글", bool: "참거짓", int: "수", float: "수", type(None): "null"}


def tname(v) -> str:
    return TYPE.get(type(v), type(v).__name__)


# ── 읽기 ────────────────────────────────────────────────────────────────
def load_text(src: str) -> str:
    if src == "-":
        return sys.stdin.read()
    with open(src, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def extract_json(text: str):
    """앞에 붙은 'for (;;);' · ')]}\\'' 를 떼고, 안 되면 글 안에서 가장 큰 JSON 덩어리를 찾는다."""
    t = text.lstrip()
    for pre in ("for (;;);", ")]}'", "while(1);"):
        if t.startswith(pre):
            t = t[len(pre):].lstrip()
    try:
        return json.loads(t)
    except ValueError:
        pass
    best, dec = None, json.JSONDecoder()
    for m in re.finditer(r"[\{\[]", text):
        try:
            obj, end = dec.raw_decode(text, m.start())
        except ValueError:
            continue
        size = end - m.start()
        if isinstance(obj, (dict, list)) and (best is None or size > best[0]):
            best = (size, obj)
    if best is None:
        raise ValueError("JSON 덩어리를 찾지 못했습니다")
    return best[1]


def load(src: str, extract: bool):
    text = load_text(src)
    return extract_json(text) if extract else json.loads(text)


# ── 모양 ────────────────────────────────────────────────────────────────
def merge_shape(items, k: int):
    """배열의 앞 k 개를 하나의 모양으로 — 칸마다 몇 개에 있었는지도 센다."""
    objs = [x for x in items[:k] if isinstance(x, dict)]
    if not objs:
        kinds = sorted({tname(x) for x in items[:k]})
        return None, kinds
    keys = OrderedDict()
    for o in objs:
        for key, v in o.items():
            keys.setdefault(key, []).append(v)
    return (keys, len(objs)), []


def leaf(v, safe: bool) -> str:
    if isinstance(v, str):
        return "글(%d자)" % len(v) if safe else "글"
    if isinstance(v, bool) and safe:
        return "참거짓=%s" % ("참" if v else "거짓")
    return tname(v)


def tree(v, a, depth=0, indent="", out=None):
    out = [] if out is None else out
    if isinstance(v, dict):
        if depth >= a.depth:
            out.append(indent + "… (칸 %d개 — 더 깊이는 --depth)" % len(v))
            return out
        for key, x in v.items():
            head = "%s%s: " % (indent, key)
            if isinstance(x, dict):
                out.append(head + "객체 {%d}" % len(x))
                tree(x, a, depth + 1, indent + "  ", out)
            elif isinstance(x, list):
                out.append(head + "배열 [%d]" % len(x))
                arr(x, a, depth + 1, indent + "  ", out)
            else:
                out.append(head + leaf(x, a.safe_values))
    elif isinstance(v, list):
        out.append(indent + "배열 [%d]" % len(v))
        arr(v, a, depth + 1, indent + "  ", out)
    else:
        out.append(indent + leaf(v, a.safe_values))
    return out


def arr(x, a, depth, indent, out):
    if not x:
        return
    merged, kinds = merge_shape(x, a.sample)
    if merged is None:
        out.append(indent + "[] " + " | ".join(kinds))
        return
    keys, n = merged
    out.append(indent + "[] 객체 — 앞 %d개를 합친 모양" % n)
    if depth >= a.depth:
        out.append(indent + "  … (칸 %d개)" % len(keys))
        return
    for key, vals in keys.items():
        miss = "" if len(vals) == n else "  (%d/%d 에만)" % (len(vals), n)
        kinds = sorted({tname(v) for v in vals})
        first = vals[0]
        if isinstance(first, dict):
            out.append("%s  %s: 객체 {%d}%s" % (indent, key, len(first), miss))
            tree(first, a, depth + 1, indent + "    ", out)
        elif isinstance(first, list):
            out.append("%s  %s: 배열 [%d]%s" % (indent, key, len(first), miss))
            arr(first, a, depth + 1, indent + "    ", out)
        else:
            out.append("%s  %s: %s%s" % (indent, key, " | ".join(kinds) if len(kinds) > 1 else leaf(first, a.safe_values), miss))


# ── 길 목록 ─────────────────────────────────────────────────────────────
def walk(v, path="$", k=3):
    """(길, 형) 을 모두 낸다. 배열은 앞 k 개를 [] 하나로 합친다."""
    yield path, tname(v)
    if isinstance(v, dict):
        for key, x in v.items():
            yield from walk(x, "%s.%s" % (path, key), k)
    elif isinstance(v, list):
        seen = set()
        for x in v[:k]:
            for p, t in walk(x, path + "[]", k):
                if (p, t) not in seen:
                    seen.add((p, t))
                    yield p, t


def find(v, name: str, k: int):
    return sorted({p for p, _ in walk(v, k=k) if p.rsplit(".", 1)[-1].replace("[]", "") == name})


def arrays_of_objects(v, path="$", out=None):
    """객체 배열의 길 · 길이 · 모두에 있는 칸(앞 5개 기준). findArrayOfObjects 가 고를 후보들."""
    out = [] if out is None else out
    if isinstance(v, dict):
        for key, x in v.items():
            arrays_of_objects(x, "%s.%s" % (path, key), out)
    elif isinstance(v, list):
        objs = [x for x in v[:5] if isinstance(x, dict)]
        if objs:
            common = set(objs[0])
            for o in objs[1:]:
                common &= set(o)
            out.append((path, len(v), sorted(common)))
        for x in v[:3]:
            arrays_of_objects(x, path + "[]", out)
    return out


def compare(a, b, k: int):
    pa, pb = dict(walk(a, k=k)), dict(walk(b, k=k))
    gone = sorted(set(pa) - set(pb))
    new = sorted(set(pb) - set(pa))
    changed = sorted(p for p in set(pa) & set(pb) if pa[p] != pb[p])
    return gone, new, [(p, pa[p], pb[p]) for p in changed]


def top_level(paths):
    """자식 길은 빼고 가장 윗길만 — '무엇이 사라졌나' 를 짧게."""
    s = set(paths)
    return [p for p in paths if not any(q != p and (p.startswith(q + ".") or p.startswith(q + "[]")) for q in s)]


def main() -> int:
    ap = argparse.ArgumentParser(description="응답 모양 보기 — 이름·형·개수만, 값은 찍지 않는다")
    ap.add_argument("file", help="JSON 파일 (- 은 표준 입력)")
    ap.add_argument("--extract", action="store_true", help="HTML·'for (;;);' 등이 섞인 글에서 가장 큰 JSON 을 찾는다")
    ap.add_argument("--depth", type=int, default=6, help="나무를 펼칠 깊이 (기본 6)")
    ap.add_argument("--sample", type=int, default=5, help="배열에서 합쳐 볼 앞 원소 수 (기본 5)")
    ap.add_argument("--safe-values", action="store_true", help="참거짓·null 과 글자 수만 보인다(글·수 값은 여전히 안 찍음)")
    ap.add_argument("--find", metavar="이름", help="이 이름의 칸이 있는 길을 모두")
    ap.add_argument("--arrays", action="store_true", help="객체 배열(목록 후보)의 길·길이·공통 칸")
    ap.add_argument("--compare", metavar="새.json", help="이 파일(옛)과 새 파일의 길을 견준다")
    a = ap.parse_args()
    try:
        data = load(a.file, a.extract)
        other = load(a.compare, a.extract) if a.compare else None
    except (OSError, ValueError) as e:
        print("읽지 못함 — %s" % e)
        return 2

    if a.find:
        hits = find(data, a.find, a.sample)
        for p in hits:
            print(p)
        print(("'%s' — %d곳" % (a.find, len(hits))) if hits else "'%s' 이(가) 없습니다" % a.find)
        return 0 if hits else 1
    if a.arrays:
        rows = arrays_of_objects(data)
        for p, n, common in sorted(rows, key=lambda r: -r[1]):
            print("%-60s [%d]  %s" % (p, n, ", ".join(common[:12]) + (" …" if len(common) > 12 else "")))
        print("객체 배열 %d개" % len(rows))
        return 0 if rows else 1
    if other is not None:
        gone, new, changed = compare(data, other, a.sample)
        for p in top_level(gone):
            print("- 사라짐  " + p)
        for p in top_level(new):
            print("+ 생김    " + p)
        for p, x, y in changed:
            print("~ 형 바뀜 %s  (%s → %s)" % (p, x, y))
        if not (gone or new or changed):
            print("모양이 같습니다")
            return 0
        print("사라짐 %d · 생김 %d · 형 바뀜 %d (길 수 — 윗길만 보임)" % (len(gone), len(new), len(changed)))
        return 1
    for line in tree(data, a):
        print(line)
    return 0


if __name__ == "__main__":
    sys.exit(main())
