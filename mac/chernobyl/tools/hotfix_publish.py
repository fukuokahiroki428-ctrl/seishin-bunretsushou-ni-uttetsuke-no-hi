#!/usr/bin/env python3
"""고침 꾸러미 올리기 (1단계 — 데이터만).

mac/chernobyl/hotfix/ 의 원본을
  1) 앱과 같은 규칙으로 모양 검사하고(통과 못 하면 멈춘다)
  2) 개인정보를 훑고(하나라도 걸리면 멈춘다)
  3) manifest.json(sha256·크기) 을 만들어
  4) hotfix-mac 브랜치의 hotfix/ 에 커밋한다 — 밀기는 하지 않는다.

  python3 tools/hotfix_publish.py              # 검사 + 커밋
  python3 tools/hotfix_publish.py --out DIR    # 검사 + DIR/hotfix/ 에 쓰기만(시험용, 커밋 없음)

앱 쪽 검사: src/core/Common.cpp 의 applyHotfix. 두 곳의 규칙은 같아야 한다.
"""
import argparse, hashlib, json, os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "hotfix"))
REPO = subprocess.run(["git", "-C", HERE, "rev-parse", "--show-toplevel"],
                      capture_output=True, text=True).stdout.strip()
BRANCH = "hotfix-mac"
FILES = ["api_overrides.json", "shape_aliases.json", "repair_rules.json"]

# ── 앱과 같은 모양 규칙(Common::applyHotfix) ───────────────────────────────
TW_PATH = re.compile(r"^/i/api/graphql/[A-Za-z0-9_-]{10,40}/[A-Za-z]{3,40}$")
BEARER = re.compile(r"^[A-Za-z0-9%_=-]{40,300}$")
DOC_ID = re.compile(r"^\d{8,25}$")
PROVIDERS = re.compile(r"^__relay_internal__pv__[A-Za-z0-9_]+relayprovider(,__relay_internal__pv__[A-Za-z0-9_]+relayprovider){0,30}$")
NAME = re.compile(r"^[A-Za-z0-9_]{1,64}$")
LITERAL_ALTS = re.compile(r"^[^()*+?{}\[\]\\|]{2,80}(\|[^()*+?{}\[\]\\|]{2,80}){0,20}$")
BAD_TEXT = re.compile(r"://|www\.|https?|@|토큰\s*값|비밀번호|password|cookie\s*값|붙여넣", re.I)
ACTIONS = {"", "refreshAllTokens", "updateModules", "repairPython"}


def check_override(key, v):
    if key == "twitter.bearer":
        return bool(BEARER.match(v)), "bearer 모양이 아님"
    if key.startswith("twitter."):
        return bool(TW_PATH.match(v)), "x.com GraphQL 경로 모양이 아님"
    if key.startswith("instagram.") and key.endswith("DocId"):
        return bool(DOC_ID.match(v)), "doc_id 는 숫자여야 함"
    if key.startswith("instagram.") and key.endswith("Providers"):
        return bool(PROVIDERS.match(v)), "provider 이름 목록 모양이 아님"
    return False, "모르는 열쇠"


def validate(data):
    errs = []
    for k, v in data["api_overrides.json"].items():
        ok, why = check_override(k, str(v))
        if not ok:
            errs.append(f"api_overrides: {k} — {why}")
    for k, vs in data["shape_aliases.json"].items():
        if not NAME.match(k) or not isinstance(vs, list) or not all(isinstance(x, str) and NAME.match(x) for x in vs):
            errs.append(f"shape_aliases: {k} — 영숫자·밑줄만")
    for i, r in enumerate(data["repair_rules.json"]):
        if not LITERAL_ALTS.match(r.get("pattern", "")):
            errs.append(f"repair_rules[{i}]: 무늬는 글자 그대로의 대안(가|나)만")
        if r.get("action", "") not in ACTIONS:
            errs.append(f"repair_rules[{i}]: 할 일 {r.get('action')!r} 은 받지 않음")
        for f in ("cause", "advice"):
            t = r.get(f, "")
            if not t or len(t) > 220 or BAD_TEXT.search(t):
                errs.append(f"repair_rules[{i}].{f}: 비었거나 길거나 링크·비밀을 달라는 글")
    return errs


# ── 개인정보 훑기 ─────────────────────────────────────────────────────────
#   이 저장소는 공개다. 올라간 것은 누구나 읽고, 지워도 기록에 남는다.
PRIVATE = [
    (re.compile(r"/Users/|/home/|C:\\\\Users", re.I), "개인 경로"),
    (re.compile(r"[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[A-Za-z]{2,}"), "이메일 주소"),
    (re.compile(r"sessionid|csrftoken|auth_token|\bct0\b|FANBOXSESSID|PHPSESSID|ds_user_id|Authorization|Bearer\s", re.I), "쿠키·토큰 이름"),
    (re.compile(r"\b\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}\b"), "IP 주소"),
]
LONG_SECRET = re.compile(r"[A-Za-z0-9%_+/=-]{41,}")


def local_identities():
    """이 맥의 사용자 이름·git 이름·메일 앞머리. 스크립트에 글자로 박지 않는다 —
    이 파일 자체가 공개 저장소에 올라가기 때문이다. 실행할 때마다 여기서 뽑는다."""
    ids = set()
    for v in (os.environ.get("USER"), os.path.basename(os.path.expanduser("~"))):
        if v:
            ids.add(v)
    for key in ("user.name", "user.email"):
        v = subprocess.run(["git", "config", key], capture_output=True, text=True).stdout.strip()
        if v:
            ids.add(v.split("@")[0])
            ids.update(w for w in re.split(r"\s+", v.split("@")[0]) if len(w) >= 3)
    return {i for i in ids if len(i) >= 3}


def privacy_scan(data, notes):
    hits = []
    texts = [("hotfix.json notes", notes)]
    for name in FILES:
        texts.append((name, json.dumps(data[name], ensure_ascii=False)))
    bearer = data["api_overrides.json"].get("twitter.bearer", "")
    me = local_identities()
    me_rx = re.compile("|".join(re.escape(i) for i in me), re.I) if me else None
    for where, text in texts:
        if me_rx and me_rx.search(text):
            hits.append(f"{where}: 이 맥의 사용자 이름")
        for rx, what in PRIVATE:
            m = rx.search(text)
            if m:
                hits.append(f"{where}: {what} ({m.group(0)[:3]}…)")
        for m in LONG_SECRET.finditer(text):
            s = m.group(0)
            if s == bearer or PROVIDERS.match(s) or s.startswith("__relay_internal__pv__"):
                continue
            hits.append(f"{where}: 비밀처럼 보이는 긴 문자열 ({len(s)}자)")
    return hits


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="커밋하지 않고 이 폴더/hotfix 에 쓰기만(시험용)")
    a = ap.parse_args()

    meta = json.load(open(os.path.join(SRC, "hotfix.json"), encoding="utf-8"))
    data = {n: json.load(open(os.path.join(SRC, n), encoding="utf-8")) for n in FILES}

    errs = validate(data)
    if errs:
        print("✗ 모양 검사에서 멈춤 — 앱이 받지 않을 값입니다:")
        for e in errs:
            print("   ", e)
        sys.exit(1)
    hits = privacy_scan(data, meta.get("notes", ""))
    if hits:
        print("✗ 개인정보 훑기에서 멈춤 — 공개 저장소에 올라가면 안 됩니다:")
        for h in hits:
            print("   ", h)
        sys.exit(1)

    raw = {n: (json.dumps(data[n], ensure_ascii=False, indent=2) + "\n").encode("utf-8") for n in FILES}
    manifest = {
        "version": int(meta["version"]),
        "minApp": meta.get("minApp", "0"),
        "notes": meta.get("notes", "")[:200],
        "files": {n: {"sha256": hashlib.sha256(b).hexdigest(), "size": len(b)} for n, b in raw.items()},
    }

    def write_to(dirpath):
        os.makedirs(dirpath, exist_ok=True)
        for n, b in raw.items():
            open(os.path.join(dirpath, n), "wb").write(b)
        open(os.path.join(dirpath, "manifest.json"), "w", encoding="utf-8").write(
            json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        shutil.copy(os.path.join(SRC, "README.md"), os.path.join(dirpath, "README.md"))

    if a.out:
        write_to(os.path.join(a.out, "hotfix"))
        print(f"✓ 검사 통과 · v{manifest['version']} → {a.out}/hotfix (커밋 안 함)")
        return

    # hotfix-mac 브랜치(꾸러미만 든 외톨이 가지)에 커밋 — 임시 작업본에서
    wt = tempfile.mkdtemp(prefix="hotfix-wt-")
    have = subprocess.run(["git", "-C", REPO, "rev-parse", "--verify", "--quiet", BRANCH],
                          capture_output=True).returncode == 0
    remote = subprocess.run(["git", "-C", REPO, "ls-remote", "--exit-code", "--heads", "origin", BRANCH],
                            capture_output=True).returncode == 0
    try:
        if have:
            subprocess.run(["git", "-C", REPO, "worktree", "add", "--quiet", wt, BRANCH], check=True)
        elif remote:
            subprocess.run(["git", "-C", REPO, "fetch", "--quiet", "origin", f"{BRANCH}:{BRANCH}"], check=True)
            subprocess.run(["git", "-C", REPO, "worktree", "add", "--quiet", wt, BRANCH], check=True)
        else:
            subprocess.run(["git", "-C", REPO, "worktree", "add", "--quiet", "--detach", wt], check=True)
            subprocess.run(["git", "-C", wt, "checkout", "--quiet", "--orphan", BRANCH], check=True)
            subprocess.run(["git", "-C", wt, "rm", "-rf", "--quiet", "."], check=True)
        write_to(os.path.join(wt, "hotfix"))
        subprocess.run(["git", "-C", wt, "add", "hotfix"], check=True)
        if subprocess.run(["git", "-C", wt, "diff", "--cached", "--quiet"]).returncode == 0:
            print(f"= 바뀐 것 없음 (v{manifest['version']})")
            return
        msg = (f"고침 꾸러미 v{manifest['version']} — {manifest['notes']}\n\n"
               "데이터만(질의 번호·응답 이름 별명·수리 규칙). 앱이 모양을 검사해 통과한 것만 쓴다.\n")
        subprocess.run(["git", "-C", wt, "commit", "--quiet", "-m", msg], check=True)
        head = subprocess.run(["git", "-C", wt, "rev-parse", "--short", "HEAD"],
                              capture_output=True, text=True).stdout.strip()
        print(f"✓ 검사 통과 · v{manifest['version']} 을 {BRANCH} 에 커밋했습니다 ({head}). 아직 밀지 않았습니다.")
        print(f"  밀기:  git -C {REPO} push origin {BRANCH}")
    finally:
        subprocess.run(["git", "-C", REPO, "worktree", "remove", "--force", wt], capture_output=True)
        subprocess.run(["git", "-C", REPO, "worktree", "prune"], capture_output=True)


if __name__ == "__main__":
    main()
