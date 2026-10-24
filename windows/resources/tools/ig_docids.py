#!/usr/bin/env python3
"""인스타 GraphQL 질의 번호(doc_id)와 제공자 깃발 이름을 다시 떠 온다.

메타는 제 판을 올릴 때마다 doc_id 를 바꾼다. 그러면 앱은 200/400 에 "execution error" 만 받고
아무것도 못 한다. 그 값은 인스타가 내려주는 JS 꾸러미 안에 그대로 적혀 있으므로, 로그인도
브라우저도 없이 공개 프로필 한 장만 받아 오면 다시 얻을 수 있다(실측 2026-09-24, 5.5초).

★ 릴스 탭 질의(PolarisProfileReelsTabContentQuery)는 로그인하지 않은 사람에게 아예 내려오지 않는다.
  실측 2026-10-24: 로그아웃 상태의 /instagram/reels/ 가 가리키는 꾸러미 427개(바로 6 · 나중에 받을 421)를 모두
  훑어도 그 이름조차 없었다(윈도우 전보 2026-10-17 도 같은 것을 보았다). 그래서 앱이 수집에 쓰는 세션을 stdin 으로
  주면(한 줄 JSON {"headers": {...}, "username": "..."} — 앱이 그 세션으로 문서를 받을 때 쓰는 머리 그대로: 쿠키 ·
  브라우저 UA · 클라이언트 힌트 · Sec-Fetch), 그 세션으로 그 사람의 릴스 탭을 받아 거기의 꾸러미를 훑는다.
  세션은 명령줄 · 출력에 남기지 않는다. 출구는 앱이 환경(ALL_PROXY …)으로 준 수집의 것만 쓴다 — 없으면 바로(앱의
  Qt 도 바로 나간다). httpx 가 macOS 시스템 프록시로 새면 한 세션이 두 출구에서 보였다(검토).
  stdin 에 0.5초 안에 아무것도 없으면(옛 앱 · 손으로 돌림) 로그아웃 상태로만 뜬다 — 기다리다 멈추지 않게.

stdout 에 JSON 하나만 찍는다:
  {"PolarisProfilePostsQuery": {"doc_id": "…", "providers": ["__relay_internal__pv__…", …]}, …}
찾지 못하면 빈 객체({})를 찍는다 — 앱은 그것을 '못 떴다' 로 읽는다.
"""
import json, re, select, sys, time

WANT = ("PolarisProfilePostsQuery", "PolarisProfilePageContentQuery", "PolarisProfileReelsTabContentQuery")
REELS = "PolarisProfileReelsTabContentQuery"
PROFILE_URL = "https://www.instagram.com/instagram/"      # 공개 계정이면 무엇이든 된다
UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/152.0.0.0 Safari/537.36")
DEADLINE = 75.0        # 앱은 90초를 기다린다 — 모든 요청을 이 안에 끝내고 찾은 것까지 낸다(요청마다 남은 시간만큼)
RAW = re.compile(r'https://static\.cdninstagram\.com/rsrc\.php/[^"\'\s\\]+?\.js')
ESC = re.compile(r'https:\\/\\/static\.cdninstagram\.com\\/rsrc\.php\\/[^"\'\s]+?\.js')
OP = re.compile(r'__d\("(\w+Query)_instagramRelayOperation".{0,400}?exports\s*=\s*"(\d{8,})"', re.S)


def read_session():
    """앱이 준 세션(있으면). 없거나 tty 면 빈 것 — 절대 기다리며 멈추지 않는다."""
    try:
        if sys.stdin is None or sys.stdin.isatty():
            return {}
        ready, _, _ = select.select([sys.stdin], [], [], 0.5)
        if not ready:
            return {}
        line = sys.stdin.readline()
        d = json.loads(line) if line.strip() else {}
        return d if isinstance(d, dict) else {}
    except Exception:
        return {}


T0 = time.time()


def get(url, client, headers=None):
    left = DEADLINE - (time.time() - T0)
    if left < 1:
        return ""
    h = headers or {"User-Agent": UA, "Accept-Language": "en-US,en;q=0.9"}
    r = client.get(url, headers=h, timeout=min(20.0, left))
    return r.text if r.status_code == 200 else ""


def session_headers(sess):
    """앱이 준 문서 요청 머리(쿠키 포함) — 글자 값만. 옛 모양({"cookie": …})도 받는다."""
    h = sess.get("headers")
    if isinstance(h, dict):
        h = {str(k): str(v) for k, v in h.items() if isinstance(v, str) and v and "\n" not in v and "\r" not in v}
        return h if any(k.lower() == "cookie" for k in h) else {}
    ck = str(sess.get("cookie") or "").strip()
    return {"User-Agent": UA, "Accept-Language": "en-US,en;q=0.9", "Cookie": ck} if ck else {}


def bundles_of(page, escaped):
    out = []
    for m in RAW.finditer(page):
        if m.group(0) not in out:
            out.append(m.group(0))
    if escaped:   # 나중에 받는 꾸러미 목록(JSON 안 — 빗금이 \/ 로 적힘)
        for m in ESC.finditer(page):
            u = m.group(0).replace('\\/', '/')
            if u not in out:
                out.append(u)
    return out


def scan(js, out):
    for m in OP.finditer(js):
        name, doc = m.group(1), m.group(2)
        if name in WANT:
            out.setdefault(name, {})["doc_id"] = doc
    # 제공자 깃발 이름 — 질의 이름 둘레에서 긁는다(값은 뜻이 없고 이름만 쓴다)
    for name in WANT:
        i = js.find('name:"%s"' % name)
        if i < 0:
            i = js.find('"%s"' % name)
        if i < 0:
            continue
        window = js[max(0, i - 3000): i + 3000]
        provs = sorted(set(re.findall(r'__relay_internal__pv__\w+relayprovider', window)))
        if provs:
            out.setdefault(name, {})["providers"] = provs


def has(out, name):
    return name in out and "doc_id" in out[name]


def main():
    sess = read_session()
    try:
        import httpx
    except Exception:
        print("{}"); return
    out = {}
    seen = set()
    import os
    proxy = os.environ.get("ALL_PROXY") or os.environ.get("HTTPS_PROXY") or None
    with httpx.Client(follow_redirects=True, trust_env=False, proxy=proxy) as c:
        # 1) 로그아웃 상태 — 게시물 · 프로필 질의(언제나 이것부터, 세션을 쓰지 않고 되는 만큼)
        page = get(PROFILE_URL, c)
        for url in bundles_of(page, False)[:12] if page else []:
            seen.add(url)
            js = get(url, c)
            if js:
                scan(js, out)
            if all(has(out, n) for n in WANT):
                break
        # 2) 릴스 탭 질의가 아직 없고 세션이 있으면 — 그 사람의 릴스 탭을 로그인한 채로 받아 훑는다.
        #    바로 받는 꾸러미부터, 그다음 나중에 받는 것. 시간 안에서 찾자마자 그친다.
        sh = session_headers(sess)
        user = re.sub(r'[^A-Za-z0-9._]', '', str(sess.get("username") or "")) or "instagram"
        if sh and not has(out, REELS):
            try:
                reels = get("https://www.instagram.com/%s/reels/" % user, c, sh)
                for url in bundles_of(reels, True) if reels else []:
                    if time.time() - T0 > DEADLINE - 1 or has(out, REELS):
                        break
                    if url in seen:
                        continue
                    seen.add(url)
                    js = get(url, c)     # 꾸러미는 공개 CDN — 세션을 붙이지 않는다
                    if js:
                        scan(js, out)
            except Exception:
                pass                     # 세션 쪽이 실패해도 1) 에서 찾은 것은 낸다
    # doc_id 없는 항목은 버린다 — 반쪽을 주면 앱이 그걸 믿고 또 실패한다
    out = {k: v for k, v in out.items() if v.get("doc_id")}
    print(json.dumps(out, ensure_ascii=False))


if __name__ == "__main__":
    try:
        main()
    except Exception:
        print("{}")
