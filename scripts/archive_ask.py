#!/usr/bin/env python3
"""
산출물 질의응답 — 색인에서 근거를 찾아 로컬 AI 가 답한다.

동작:
    질문 → 색인 검색(관련 파일 찾기) → 찾은 것만 AI 에게 넘김 → 답 + 근거 목록

왜 이렇게 하나:
    수십만 건을 통째로 AI 에게 줄 수 없다(문맥 한계). 먼저 검색으로 몇십 건을
    추리고 그것만 넘긴다. 그래서 AI 는 '기억' 이 아니라 '찾은 자료' 를 근거로
    답하며, 없는 걸 지어내면 근거 목록과 어긋나므로 바로 드러난다.

한계(분명히 해둘 것):
    - 그림 '내용' 은 못 본다. 번들 모델이 글자 전용이라 EXIF·파일명·엑셀 등
      글자로 남은 것만 근거가 된다. ("빨간 옷 입은 사람" 같은 질문은 불가)
    - 색인에 없는 파일은 답에 안 나온다. archive_index.py 를 먼저 돌려야 한다.

사용:
    python3 archive_ask.py "질문"              # 한 번 묻고 끝
    python3 archive_ask.py --search "검색어"    # AI 없이 검색만
"""
import argparse, json, os, re, sqlite3, sys, urllib.request
from pathlib import Path

DEFAULT_DB = Path.home() / 'Library/Application Support/Miyo/Predormition/archive_index.db'
LLM_URL    = 'http://127.0.0.1:8737/v1/chat/completions'
TOP_K      = 40           # AI 에게 넘길 근거 개수
SNIPPET    = 300          # 근거 하나당 본문 발췌 길이

STOP = {'뭐','뭔','무슨','어떤','어느','있어','있나','있었','알려','줘','해줘','보여',
        '찾아','찾아줘','언제','누가','어디','그거','저거','이거','좀','에서','에게',
        '하고','그리고','관련','파일','사진','이미지','그림','전부','모두','개','것',
        'what','which','who','when','where','show','find','me','the','a','an','of','is','are'}


def tokens(q: str) -> list[str]:
    """검색어 뽑기 — 조사·의문사를 빼고 의미가 있는 조각만 남긴다."""
    parts = re.split(r'[\s,./?!"\'()\[\]{}<>:;]+', q)
    out = []
    for p in parts:
        p = p.strip()
        if not p or p in STOP or len(p) < 2:
            continue
        out.append(p)
    return out[:8]


def search(db: sqlite3.Connection, query: str, limit: int = TOP_K) -> list[dict]:
    """
    trigram FTS 로 찾되, 2글자 이하 검색어는 trigram 원리상 안 걸리므로 LIKE 로 보완한다.
    검색어 여러 개면 각각 찾아 합치고, 여러 번 걸린 파일을 위로 올린다.
    """
    terms = tokens(query) or [query.strip()]
    hits: dict[str, dict] = {}
    for t in terms:
        rows = []
        if len(t) >= 3:
            try:
                rows = db.execute("""
                    SELECT f.path,f.name,f.kind,f.platform,f.artist,f.title,
                           f.description,f.source_url,f.taken,f.mtime,substr(f.body,1,?)
                    FROM files_fts s JOIN files f ON f.rowid = s.rowid
                    WHERE files_fts MATCH ? ORDER BY rank LIMIT ?""",
                    (SNIPPET, f'"{t}"', limit)).fetchall()
            except sqlite3.OperationalError:
                rows = []
        if not rows:   # 짧은 검색어 또는 FTS 실패 → LIKE 폴백
            like = f'%{t}%'
            rows = db.execute("""
                SELECT path,name,kind,platform,artist,title,description,source_url,
                       taken,mtime,substr(body,1,?)
                FROM files
                WHERE name LIKE ? OR artist LIKE ? OR title LIKE ?
                   OR description LIKE ? OR body LIKE ?
                LIMIT ?""", (SNIPPET, like, like, like, like, like, limit)).fetchall()
        for r in rows:
            d = hits.setdefault(r[0], {
                'path': r[0], 'name': r[1], 'kind': r[2], 'platform': r[3],
                'artist': r[4], 'title': r[5], 'description': r[6],
                'source_url': r[7], 'taken': r[8], 'mtime': r[9],
                'body': r[10], 'score': 0})
            d['score'] += 1
    ranked = sorted(hits.values(), key=lambda d: (-d['score'], -(d['mtime'] or 0)))
    # 같은 작품이 원본·_complete 미러로 두 번 저장돼 있어 결과가 중복된다.
    # 이름+작가+제목이 같으면 한 건으로 접는다(경로는 첫 번째 것을 남긴다).
    seen, out = set(), []
    for d in ranked:
        key = (d['name'], d['artist'], d['title'])
        if key in seen:
            continue
        seen.add(key); out.append(d)
        if len(out) >= limit:
            break
    return out


def make_context(items: list[dict]) -> str:
    lines = []
    for i, d in enumerate(items, 1):
        bits = [f"[{i}] {d['name']}"]
        if d['artist']:     bits.append(f"작가={d['artist']}")
        if d['title']:      bits.append(f"제목={d['title']}")
        if d['platform']:   bits.append(f"출처={d['platform']}")
        if d['taken']:      bits.append(f"날짜={d['taken']}")
        if d['source_url']: bits.append(f"URL={d['source_url']}")
        if d['description']: bits.append(f"설명={d['description'][:160]}")
        if d['body']:       bits.append(f"내용={d['body'][:200]}")
        lines.append(' · '.join(bits))
    return '\n'.join(lines)


def ask_llm(question: str, context: str, timeout: int = 180) -> str:
    sys_p = (
        "너는 사용자가 수집해 둔 자료 보관함의 사서다. "
        "아래 '자료' 에 있는 내용만 근거로 답한다. 자료에 없으면 없다고 말한다. "
        "추측하거나 지어내지 않는다. 답에는 근거 번호([1], [2])를 붙인다. "
        "한국어로, 간결하게 답한다."
    )
    payload = {
        "messages": [
            {"role": "system", "content": sys_p},
            {"role": "user", "content": f"자료:\n{context}\n\n질문: {question}"},
        ],
        "temperature": 0.2, "max_tokens": 700,
    }
    req = urllib.request.Request(
        LLM_URL, data=json.dumps(payload).encode(),
        headers={'Content-Type': 'application/json'})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        d = json.loads(r.read().decode())
    return d['choices'][0]['message']['content'].strip()


def llm_alive() -> bool:
    try:
        with urllib.request.urlopen('http://127.0.0.1:8737/health', timeout=3) as r:
            return r.status == 200
    except Exception:
        return False


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument('question', nargs='?', help='질문')
    ap.add_argument('--db', default=str(DEFAULT_DB))
    ap.add_argument('--search', help='AI 없이 검색만')
    ap.add_argument('--top', type=int, default=TOP_K)
    args = ap.parse_args()

    dbp = Path(args.db)
    if not dbp.exists():
        print(f"색인이 없습니다: {dbp}\n먼저 archive_index.py 를 돌려 주세요."); return 1
    db = sqlite3.connect(str(dbp))

    if args.search:
        for i, d in enumerate(search(db, args.search, args.top), 1):
            print(f"[{i}] {d['name']}")
            if d['artist'] or d['title']:
                print(f"     {d['artist']} — {d['title'][:60]}")
            if d['source_url']:
                print(f"     {d['source_url']}")
        return 0

    if not args.question:
        ap.print_help(); return 1

    items = search(db, args.question, args.top)
    if not items:
        print("색인에서 관련 자료를 찾지 못했습니다."); return 0

    if not llm_alive():
        print("로컬 AI 가 꺼져 있습니다 (127.0.0.1:8737). 앱에서 'AI 켜기' 를 눌러 주세요.")
        print("\n대신 검색 결과만 보여드립니다:")
        for i, d in enumerate(items[:12], 1):
            print(f"  [{i}] {d['name']}  {d['artist']} — {d['title'][:50]}")
        return 0

    answer = ask_llm(args.question, make_context(items))
    print(answer)
    print("\n── 근거 ──")
    for i, d in enumerate(items[:12], 1):
        line = f"  [{i}] {d['name']}"
        if d['source_url']:
            line += f"  {d['source_url']}"
        print(line)
    return 0


if __name__ == '__main__':
    sys.exit(main())
