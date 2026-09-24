#!/usr/bin/env python3
"""인스타 GraphQL 질의 번호(doc_id)와 제공자 깃발 이름을 다시 떠 온다.

메타는 제 판을 올릴 때마다 doc_id 를 바꾼다. 그러면 앱은 200/400 에 "execution error" 만 받고
아무것도 못 한다. 그 값은 인스타가 내려주는 JS 꾸러미 안에 그대로 적혀 있으므로, 로그인도
브라우저도 없이 공개 프로필 한 장만 받아 오면 다시 얻을 수 있다(실측 2026-09-24, 5.5초).

stdout 에 JSON 하나만 찍는다:
  {"PolarisProfilePostsQuery": {"doc_id": "…", "providers": ["__relay_internal__pv__…", …]}, …}
찾지 못하면 빈 객체({})를 찍는다 — 앱은 그것을 '못 떴다' 로 읽는다.
"""
import json, re, sys

WANT = ("PolarisProfilePostsQuery", "PolarisProfilePageContentQuery", "PolarisProfileReelsTabContentQuery")
PROFILE_URL = "https://www.instagram.com/instagram/"      # 공개 계정이면 무엇이든 된다
UA = ("Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
      "(KHTML, like Gecko) Chrome/152.0.0.0 Safari/537.36")

def get(url, client):
    r = client.get(url, headers={"User-Agent": UA, "Accept-Language": "en-US,en;q=0.9"}, timeout=30)
    return r.text if r.status_code == 200 else ""

def main():
    try:
        import httpx
    except Exception:
        print("{}"); return
    out = {}
    with httpx.Client(follow_redirects=True) as c:
        page = get(PROFILE_URL, c)
        if not page:
            print("{}"); return
        # 꾸러미 주소 — 같은 것을 두 번 받지 않는다
        bundles = []
        for m in re.finditer(r'https://static\.cdninstagram\.com/rsrc\.php/[^"\'\s]+?\.js', page):
            u = m.group(0)
            if u not in bundles:
                bundles.append(u)
        for url in bundles[:12]:
            js = get(url, c)
            if not js:
                continue
            # doc_id: __d("<이름>_instagramRelayOperation",[],…exports="<숫자>"
            for m in re.finditer(r'__d\("(\w+Query)_instagramRelayOperation".{0,400}?exports\s*=\s*"(\d{8,})"', js, re.S):
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
            if all(n in out and "doc_id" in out[n] for n in WANT):
                break
    # doc_id 없는 항목은 버린다 — 반쪽을 주면 앱이 그걸 믿고 또 실패한다
    out = {k: v for k, v in out.items() if v.get("doc_id")}
    print(json.dumps(out, ensure_ascii=False))

if __name__ == "__main__":
    try:
        main()
    except Exception:
        print("{}")
