#!/usr/bin/env python3
"""고침 꾸러미 올리기 (1단계 데이터 + 2단계 서명한 파이썬 도우미).

mac/chernobyl/hotfix/ 의 원본을
  1) 앱과 같은 규칙으로 모양 검사하고(통과 못 하면 멈춘다)
  2) 개인정보를 훑고(하나라도 걸리면 멈춘다)
  3) manifest.json(sha256·크기) 을 만들어
  4) hotfix-mac 브랜치의 hotfix/ 에 커밋한다 — 밀기는 하지 않는다.

  python3 tools/hotfix_publish.py              # 검사 + 커밋
  python3 tools/hotfix_publish.py --out DIR    # 검사 + DIR/hotfix/ 에 쓰기만(시험용, 커밋 없음)
  python3 tools/hotfix_publish.py --genkey     # 서명 열쇠 만들기(한 번만) — 공개 열쇠를 앱에 넣는다

서명 — manifest.json 을 ECDSA P-256(SHA-256)으로 서명해 manifest.sig 로 둔다. 앱은 컴파일해 둔
공개 열쇠로 검증하고, 맞지 않으면 꾸러미를 통째로 버린다. 비밀 열쇠는 저장소 밖
~/.config/hanishiki/hotfix_signing_key.pem 에 이 맥 사용자만 읽게 둔다(깃헙에 올라가면 안 된다).
열쇠를 잃으면 새 공개 열쇠를 넣은 새 판을 내야 한다 — 백업해 두라.

앱 쪽 검사: src/core/Common.cpp 의 applyHotfix. 두 곳의 규칙은 같아야 한다.
"""
import argparse, hashlib, json, os, re, shutil, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SRC = os.path.normpath(os.path.join(HERE, "..", "hotfix"))
REPO = subprocess.run(["git", "-C", HERE, "rev-parse", "--show-toplevel"],
                      capture_output=True, text=True).stdout.strip()
BRANCH = "hotfix-mac"
FILES = ["api_overrides.json", "shape_aliases.json", "repair_rules.json"]
KEY = os.path.expanduser("~/.config/hanishiki/hotfix_signing_key.pem")
BUNDLED_TOOLS = os.path.normpath(os.path.join(HERE, "..", "resources", "tools"))
TOOL_MAX = 512 * 1024

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


def allowed_tool(name):
    """번들에 이미 있는 파이썬 도우미만 갈아 끼울 수 있다(새 파일을 들이지 못한다)."""
    return (re.match(r"^[a-z0-9_]{2,40}\.py$", name) is not None and
            (os.path.exists(os.path.join(BUNDLED_TOOLS, name)) or
             os.path.exists(os.path.join(BUNDLED_TOOLS, "archive", name))))


def check_tools(tools):
    errs = []
    for name, path in tools.items():
        if not allowed_tool(name):
            errs.append(f"tools/{name}: 번들에 없는 도우미는 들일 수 없음")
            continue
        if os.path.getsize(path) > TOOL_MAX:
            errs.append(f"tools/{name}: {TOOL_MAX // 1024}KB 넘음")
        try:
            compile(open(path, encoding="utf-8").read(), path, "exec")   # 문법만 본다(파일을 쓰지 않는다)
        except SyntaxError as e:
            errs.append(f"tools/{name}: 문법 오류 — {e.msg} (줄 {e.lineno})")
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


CODE_PRIVATE = [
    (re.compile(r"/Users/[A-Za-z0-9._-]+|/home/[A-Za-z0-9._-]+", re.I), "개인 경로"),
    (re.compile(r"[A-Za-z0-9._%+-]+@(?!example\.)[A-Za-z0-9.-]+\.[A-Za-z]{2,}"), "이메일 주소"),
]


def privacy_scan_code(tools):
    hits = []
    me = local_identities()
    me_rx = re.compile("|".join(re.escape(i) for i in me), re.I) if me else None
    for name, path in tools.items():
        text = open(path, encoding="utf-8", errors="replace").read()
        if me_rx and me_rx.search(text):
            hits.append(f"tools/{name}: 이 맥의 사용자 이름")
        for rx, what in CODE_PRIVATE:
            for m in rx.finditer(text):
                if "noreply" in m.group(0) or m.group(0).startswith("/Users/Shared"):
                    continue
                hits.append(f"tools/{name}: {what}")
                break
    return hits


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



def genkey():
    if os.path.exists(KEY):
        print(f"= 열쇠가 이미 있습니다: {KEY} — 새로 만들지 않습니다(바꾸면 앱에 든 공개 열쇠도 바꿔야 한다)")
    else:
        os.makedirs(os.path.dirname(KEY), mode=0o700, exist_ok=True)
        old = os.umask(0o077)
        try:
            subprocess.run(["/usr/bin/openssl", "ecparam", "-genkey", "-name", "prime256v1", "-noout", "-out", KEY],
                           check=True, capture_output=True)
        finally:
            os.umask(old)
        os.chmod(KEY, 0o600)
        print(f"✓ 서명 열쇠를 만들었습니다: {KEY} (이 맥 사용자만 읽음) — 백업해 두십시오")
    print("공개 열쇠(앱의 HotfixSig.cpp 에 넣는다):", public_key_hex())


def public_key_hex():
    der = subprocess.run(["/usr/bin/openssl", "ec", "-in", KEY, "-pubout", "-outform", "DER"],
                         check=True, capture_output=True).stdout
    return der[-65:].hex()          # SPKI 끝 65바이트 = 04||X||Y (X9.63)


def sign(path):
    if not os.path.exists(KEY):
        print(f"✗ 서명 열쇠가 없습니다: {KEY} — 먼저 --genkey"); sys.exit(1)
    if os.path.commonpath([os.path.realpath(KEY), os.path.realpath(REPO)]) == os.path.realpath(REPO):
        print("✗ 서명 열쇠가 저장소 안에 있습니다 — 공개로 새어 나갑니다. 멈춥니다."); sys.exit(1)
    return subprocess.run(["/usr/bin/openssl", "dgst", "-sha256", "-sign", KEY, path],
                          check=True, capture_output=True).stdout


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", help="커밋하지 않고 이 폴더/hotfix 에 쓰기만(시험용)")
    ap.add_argument("--genkey", action="store_true", help="서명 열쇠 만들기(한 번만)")
    a = ap.parse_args()
    if a.genkey:
        genkey(); return

    meta = json.load(open(os.path.join(SRC, "hotfix.json"), encoding="utf-8"))
    data = {n: json.load(open(os.path.join(SRC, n), encoding="utf-8")) for n in FILES}

    tdir = os.path.join(SRC, "tools")
    tools = {n: os.path.join(tdir, n) for n in sorted(os.listdir(tdir))
             if n.endswith(".py")} if os.path.isdir(tdir) else {}

    errs = validate(data) + check_tools(tools)
    if errs:
        print("✗ 모양 검사에서 멈춤 — 앱이 받지 않을 값입니다:")
        for e in errs:
            print("   ", e)
        sys.exit(1)
    hits = privacy_scan(data, meta.get("notes", "")) + privacy_scan_code(tools)
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
    traw = {n: open(p, "rb").read() for n, p in tools.items()}
    if traw:
        manifest["tools"] = {n: {"sha256": hashlib.sha256(b).hexdigest(), "size": len(b)} for n, b in traw.items()}

    def write_to(dirpath):
        os.makedirs(dirpath, exist_ok=True)
        for n, b in raw.items():
            open(os.path.join(dirpath, n), "wb").write(b)
        shutil.rmtree(os.path.join(dirpath, "tools"), ignore_errors=True)
        if traw:
            os.makedirs(os.path.join(dirpath, "tools"), exist_ok=True)
            for n, b in traw.items():
                open(os.path.join(dirpath, "tools", n), "wb").write(b)
        mpath = os.path.join(dirpath, "manifest.json")
        open(mpath, "w", encoding="utf-8").write(json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
        # 앱은 받은 manifest.json 바이트 그대로를 검증한다 — 쓴 뒤의 파일에 서명한다
        open(os.path.join(dirpath, "manifest.sig"), "wb").write(sign(mpath))
        shutil.copy(os.path.join(SRC, "README.md"), os.path.join(dirpath, "README.md"))

    if a.out:
        write_to(os.path.join(a.out, "hotfix"))
        print(f"✓ 검사 통과 · 서명함 · v{manifest['version']} (도우미 {len(traw)}개) → {a.out}/hotfix (커밋 안 함)")
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
