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

def default_db() -> Path:
    """
    색인 DB 위치 — 앱이 설정을 두는 곳과 같은 자리.

    ★ 예전엔 맥 경로가 그대로 박혀 있어서 윈도우에서는 엉뚱한 데를 보고
      "색인이 없습니다" 만 냈다. Qt 의 AppDataLocation 과 같은 규칙으로 맞춘다.
    """
    if sys.platform == 'darwin':
        base = Path.home() / 'Library/Application Support'
    elif os.name == 'nt':
        base = Path(os.environ.get('APPDATA') or (Path.home() / 'AppData/Roaming'))
    else:
        base = Path(os.environ.get('XDG_DATA_HOME') or (Path.home() / '.local/share'))
    return base / 'Miyo/Predormition/archive_index.db'


DEFAULT_DB = default_db()
LLM_URL    = 'http://127.0.0.1:8737/v1/chat/completions'
TOP_K      = 40           # AI 에게 근거로 보여 줄 개수
SNIPPET    = 300          # 근거 하나당 본문 발췌 길이
SCAN_CAP   = 4000         # 셈을 위해 훑어 볼 최대 건수 (보여 주는 건 TOP_K 까지)
CTX_CHARS  = 2600         # AI 에게 넘길 자료의 글자 상한 — 아래 주석 참고

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


def search(db: sqlite3.Connection, query: str, limit: int = TOP_K,
           want_total: bool = False):
    """
    trigram FTS 로 찾되, 2글자 이하 검색어는 trigram 원리상 안 걸리므로 LIKE 로 보완한다.
    검색어 여러 개면 각각 찾아 합치고, 여러 번 걸린 파일을 위로 올린다.

    ★ want_total=True 면 (보여 줄 목록, 실제 총 건수) 를 돌려준다.
      예전엔 상한(40건)까지만 세어서 "몇 개 있어?" 에 반드시 틀린 답을 했다
      — 실제 676건인데 24건이라고 답하는 식이었다. 그래서 셈은 넉넉히
      훑어(SCAN_CAP) 따로 세고, AI 에게는 그 '숫자' 를 알려 준다.
    """
    terms = tokens(query) or [query.strip()]
    scan = max(limit, SCAN_CAP) if want_total else limit
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
                    (SNIPPET, f'"{t}"', scan)).fetchall()
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
                LIMIT ?""", (SNIPPET, like, like, like, like, like, scan)).fetchall()
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
    total = len(out)
    # ★ 통계는 '찾은 것 전부' 로 낸다. 예전엔 보여 줄 상위 몇 건으로만 계산해서,
    #   상위가 엑셀·텍스트로 채워지면 작가·기간이 통째로 비어 버렸다.
    return (out[:limit], total, out) if want_total else out[:limit]


def summarize(items: list[dict], total: int) -> str:
    """
    자료의 요약을 파이썬이 미리 계산한다.

    왜:
        작은 로컬 모델은 목록을 세지 못한다. 40줄을 주고 "몇 개냐" 물으면
        번호만 나열하거나 아무 숫자나 말한다(실제로 그랬다).
        숫자·기간·작가 분포는 코드가 정확히 구하고, AI 는 그걸 문장으로
        옮기기만 하면 된다. 그러면 셈이 틀릴 여지가 없다.
    """
    from collections import Counter
    artists = Counter(d['artist'] for d in items if d['artist'])
    plats   = Counter(d['platform'] for d in items if d['platform'])
    kinds   = Counter(d['kind'] for d in items if d['kind'])
    dates   = sorted(d['taken'][:10] for d in items if d['taken'])
    titles  = [d['title'] for d in items if d['title'] and d['title'] != '無題']

    # ★ 여기에 '보여 주는 건수' 같은 다른 숫자를 절대 적지 않는다.
    #   예전엔 "대표 40건만 실었다" 를 함께 넣었더니, 개수를 물었을 때 모델이
    #   435건 대신 40건이라고 답했다 — 숫자가 둘 있으면 작은 모델은 고른다.
    parts = [f"질문에 해당하는 자료는 정확히 {total}건이다"
             " (개수를 물으면 반드시 이 숫자로 답한다)"]
    if artists:
        parts.append("작가: " + ", ".join(f"{a}({c}건)" for a, c in artists.most_common(5)))
    if plats:
        parts.append("출처: " + ", ".join(f"{p}({c})" for p, c in plats.most_common(5)))
    if kinds:
        parts.append("종류: " + ", ".join(f"{k}({c})" for k, c in kinds.most_common(4)))
    if dates:
        parts.append(f"기간: {dates[0]} ~ {dates[-1]}")
    if titles:
        uniq = list(dict.fromkeys(titles))[:6]
        parts.append("제목 예: " + " / ".join(t[:30] for t in uniq))
    return "\n".join("- " + x for x in parts)


def make_context(items: list[dict], budget: int = CTX_CHARS) -> str:
    """
    ★ 글자 예산을 반드시 지킨다.
      서버는 -c 로 정한 만큼만 기억한다. 넘치면 앞쪽(=지시문과 요약)이 잘려 나가서
      AI 가 무엇을 하라고 했는지조차 모르는 채 답한다 — 실제로 이 때문에
      "[1], [2], [3] … 번째 그림이 있습니다" 같은 답이 나왔다.
      일본어·한국어는 글자당 토큰이 커서, 넉넉히 잡아 여기서 미리 자른다.
    """
    lines, used = [], 0
    for i, d in enumerate(items, 1):
        bits = [f"[{i}] {d['name']}"]
        if d['artist']:     bits.append(f"작가={d['artist']}")
        if d['title']:      bits.append(f"제목={d['title']}")
        if d['platform']:   bits.append(f"출처={d['platform']}")
        if d['taken']:      bits.append(f"날짜={d['taken']}")
        if d['source_url']: bits.append(f"URL={d['source_url']}")
        if d['description']: bits.append(f"설명={d['description'][:160]}")
        if d['body']:       bits.append(f"내용={d['body'][:200]}")
        line = ' · '.join(bits)
        if used + len(line) > budget:
            break
        lines.append(line); used += len(line)
    return '\n'.join(lines)


def ask_llm(question: str, context: str, timeout: int = 180, summary: str = '') -> str:
    """
    ★ 지시문을 다시 썼다.
      예전 지시문은 "답에는 근거 번호([1],[2])를 붙인다" 였다. 작은 모델은 이걸
      곧이곧대로 따라서 "[1], [2], [3] … 번째 그림이 있습니다" 만 내놓았다
      — 번호만 나열하고 정작 아무 내용도 말하지 않는 답이다.
      그래서 (가) 숫자는 아래 '요약' 에 계산해 둔 것을 그대로 쓰게 하고,
      (나) 번호는 2~3개만 붙이게 제한하고, (다) 무엇을 답해야 하는지를
      항목으로 못 박았다.
    """
    sys_p = (
        "너는 사용자가 모아 둔 자료 보관함의 사서다. 한국어로 3~5문장으로 답한다.\n"
        "규칙:\n"
        "1. 건수·기간·작가 같은 숫자는 반드시 '요약' 에 적힌 값을 그대로 쓴다. "
        "직접 세지 마라 — 요약의 숫자가 정확하다.\n"
        "2. 자료에 없는 것은 없다고 말한다. 지어내지 않는다.\n"
        "3. 번호 나열만 하지 마라. 무엇이 얼마나, 언제, 누구 것인지를 문장으로 설명한다. "
        "근거 번호는 꼭 필요할 때 2~3개만 붙인다.\n"
        "4. 파일명을 그대로 옮겨 적지 마라. 근거 목록은 이미 사용자에게 따로 보인다."
    )
    body = (f"요약(정확한 값):\n{summary}\n\n" if summary else "") + \
           f"근거 자료:\n{context}\n\n질문: {question}"
    payload = {
        "messages": [
            {"role": "system", "content": sys_p},
            {"role": "user", "content": body},
        ],
        "temperature": 0.2, "max_tokens": 500,
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
    ap.add_argument('--json', action='store_true', help='기계가 읽을 형태로 출력(앱이 쓴다)')
    args = ap.parse_args()

    dbp = Path(args.db)

    if not dbp.exists():
        msg = f"색인이 없습니다: {dbp}\n먼저 색인 만들기를 실행해 주세요."
        print(json.dumps({'ok': False, 'error': msg}, ensure_ascii=False) if args.json else msg)
        return 1
    # 읽기 전용으로 연다 — 색인 작업이 동시에 돌아도 서로 방해하지 않는다.
    try:
        db = sqlite3.connect(f'file:{dbp}?mode=ro', uri=True)
    except sqlite3.OperationalError:
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

    items, total, allhits = search(db, args.question, args.top, want_total=True)
    if not items:
        if args.json:
            print(json.dumps({'ok': True, 'answer': '색인에서 관련 자료를 찾지 못했습니다.',
                              'sources': []}, ensure_ascii=False)); return 0
        print("색인에서 관련 자료를 찾지 못했습니다."); return 0

    if args.json:
        srcs = [{'name': d['name'], 'artist': d['artist'] or '', 'title': d['title'] or '',
                 'url': d['source_url'] or '', 'path': d['path']} for d in items[:12]]
        if not llm_alive():
            # AI 가 꺼져 있어도 검색 결과는 준다 — 아무것도 못 하는 것보다 낫다.
            print(json.dumps({'ok': True, 'noai': True, 'sources': srcs, 'total': total,
                              'answer': "로컬 AI 가 꺼져 있어 검색 결과만 보여 드립니다. "
                                        "위의 'AI 켜기' 를 눌러 주세요."}, ensure_ascii=False))
            return 0
        try:
            ans = ask_llm(args.question, make_context(items),
                          summary=summarize(allhits, total))
        except Exception as e:
            print(json.dumps({'ok': False, 'error': f'AI 응답 실패: {type(e).__name__}: {e}',
                              'sources': srcs}, ensure_ascii=False)); return 1
        print(json.dumps({'ok': True, 'answer': ans, 'sources': srcs, 'total': total},
                         ensure_ascii=False))
        return 0

    if not llm_alive():
        print("로컬 AI 가 꺼져 있습니다 (127.0.0.1:8737). 앱에서 'AI 켜기' 를 눌러 주세요.")
        print("\n대신 검색 결과만 보여드립니다:")
        for i, d in enumerate(items[:12], 1):
            print(f"  [{i}] {d['name']}  {d['artist']} — {d['title'][:50]}")
        return 0

    answer = ask_llm(args.question, make_context(items),
                     summary=summarize(allhits, total))
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
