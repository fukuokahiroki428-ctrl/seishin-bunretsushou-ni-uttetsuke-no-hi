#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
exif_repair_newlines.py — 줄바꿈 때문에 첫 줄만 저장된 EXIF 설명을 되살린다.

무슨 일이 있었나
----------------
윈도우판은 exiftool 인자를 UTF-8 argfile 로 넘긴다(한글·일본어가 ANSI 로 뭉개지지
않게). 그런데 argfile 은 '한 줄에 인자 하나' 다. 게시물 본문에는 줄바꿈이 흔하다.
그래서 두 번째 줄부터를 exiftool 이 파일 이름으로 읽었고,
    Error: File not found - 두 번째 줄 https://t.co/...
쓰기가 통째로 실패했다.

★ 처음엔 '첫 줄만 저장됐다' 고 생각했는데, 실제 파일을 열어 보니 설명이
  아예 비어 있었다. exiftool 이 그 실행 전체를 오류로 끝냈기 때문이다.
  짐작으로 분류를 짜면 멀쩡한 파일을 건드리거나 망가진 파일을 놓친다.
  그래서 두 꼴을 다 받아들이되, 그 둘만 받아들인다.

앱 쪽은 고쳤지만(값을 파일로 넘기게) 이미 받아 둔 자료는 그대로다.
이 스크립트가 그것을 되살린다.

무엇을 근거로 되살리나
----------------------
계정 폴더의 *_complete.xlsx / *_comments.xlsx / *_reposts.xlsx 에 게시물 원문이
줄바꿈까지 온전히 남아 있다. 파일 이름에는 게시물 ID 가 들어 있다:
    20251116_1845_#maimai_art-1989993301450342570(G53gXG8aAAEMpch).jpg
                               ^^^^^^^^^^^^^^^^^^^ 이것
그래서 파일 → ID → 엑셀 → 원문 으로 되찾을 수 있다. 없는 것을 지어내지 않는다.

무엇만 건드리나
---------------
'망가진 꼴' 이 정확히 맞는 것만 고친다 — 기대값(원문)에 줄바꿈이 있고,
지금 저장된 값이 다음 둘 중 하나일 때:
  · 비어 있다        (쓰기가 통째로 실패한 경우 — 실제로 이게 대부분이다)
  · 원문의 첫 줄과 같다 (일부만 들어간 경우)
그 외에는 손대지 않는다. 사람이 직접 고쳐 둔 것이나, 원래부터 한 줄인 것을
덮어쓰면 안 된다. 애매하면 건너뛰고 '확인 필요' 로 센다.

쓰는 법
-------
    python scripts/exif_repair_newlines.py <폴더> [...]          # 세어만 본다(기본)
    python scripts/exif_repair_newlines.py <폴더> --apply        # 실제로 고친다
    python scripts/exif_repair_newlines.py <폴더> --limit 50     # 표본만

기본이 '세어만 보기' 인 이유: 남의 자료를 고치는 일이라 무엇이 바뀔지 먼저
눈으로 보고 결정해야 한다.
"""

import argparse
import json
import os
import re
import subprocess
import sys
import tempfile

# 파일 이름 속 게시물 ID — 15자리 이상 숫자 덩어리 중 마지막 것.
#   "20251116_1845_..." 의 날짜(8자리)와 섞이지 않게 길이로 거른다.
ID_RE = re.compile(r"(\d{15,})")
IMAGE_EXT = {".jpg", ".jpeg", ".png", ".webp", ".tiff", ".tif"}


def qt_left(s: str, n: int) -> str:
    """
    QString::left(n) 과 같게 자른다.

    ★ 파이썬의 s[:n] 과 다르다. Qt 는 UTF-16 단위로 세고 파이썬은 코드포인트로
      센다. 이모지는 UTF-16 에서 2칸을 먹으므로, 이모지가 섞인 본문에서는 두
      방식의 결과가 달라진다. 그 차이를 '망가졌다' 고 오해하면 멀쩡한 파일을
      건드리게 된다. 앱이 자른 것과 똑같이 잘라야 비교가 의미를 갖는다.
    """
    b = s.encode("utf-16-le")[: n * 2]
    try:
        return b.decode("utf-16-le")
    except UnicodeDecodeError:
        return b[:-2].decode("utf-16-le", "ignore")   # 서러게이트 쌍이 잘렸을 때


def load_texts(account_dir: str) -> dict:
    """계정 폴더의 엑셀에서 {게시물ID: 본문} 을 모은다."""
    try:
        import openpyxl
    except ImportError:
        sys.exit("openpyxl 이 필요합니다. 앱의 번들 파이썬으로 실행하세요.")

    out = {}
    for name in sorted(os.listdir(account_dir)):
        if not name.lower().endswith(".xlsx"):
            continue
        path = os.path.join(account_dir, name)
        try:
            wb = openpyxl.load_workbook(path, read_only=True, data_only=True)
        except Exception as e:
            print("  ! 엑셀을 못 읽었습니다: %s (%s)" % (name, e))
            continue
        for ws in wb.worksheets:
            rows = ws.iter_rows(values_only=True)
            try:
                hdr = [str(h or "") for h in next(rows)]
            except StopIteration:
                continue
            if "id" not in hdr or "text" not in hdr:
                continue
            i_id, i_text = hdr.index("id"), hdr.index("text")
            for r in rows:
                if len(r) <= max(i_id, i_text):
                    continue
                rid, txt = r[i_id], r[i_text]
                if rid is None or txt is None:
                    continue
                rid = str(rid).strip()
                if rid and rid not in out:
                    out[rid] = str(txt)
        wb.close()
    return out


def read_descriptions(exiftool: str, paths: list) -> dict:
    """여러 파일의 ImageDescription 을 한 번에 읽는다 (파일마다 부르면 너무 느리다)."""
    if not paths:
        return {}
    # ★ 바이너리로 쓴다. 파일 이름에 짝 없는 서러게이트(\ud83c 같은)가 들어 있는
    #   자료가 실제로 있었다 — 이모지가 잘린 채 파일명이 된 것이다. 보통의 UTF-8
    #   인코딩은 거기서 죽는다. surrogatepass 로 바이트를 그대로 흘려보낸다.
    with tempfile.NamedTemporaryFile("wb", suffix=".args", delete=False) as f:
        f.write(b"-charset\nfilename=UTF8\n-charset\nUTF8\n-j\n-ImageDescription\n")
        for p in paths:
            f.write((p + "\n").encode("utf-8", "surrogatepass"))
        argfile = f.name
    try:
        r = subprocess.run([exiftool, "-@", argfile], capture_output=True)
        out = r.stdout.decode("utf-8", "replace")
        try:
            data = json.loads(out or "[]")
        except json.JSONDecodeError:
            data = []
        # ★ 도구가 안 돈 것과 자료가 없는 것은 다르다.
        #   전에는 여기서 조용히 {} 를 돌려줬고, 그러면 모든 파일이 '설명이 비어
        #   있음' 으로 보였다. 실제로는 exiftool 이 자기 DLL 을 못 찾아 한 건도
        #   못 읽고 있었다. 그 상태로 --apply 를 돌렸으면 멀쩡한 설명을 덮어썼다.
        #   읽어 온 것이 없으면 조용히 넘어가지 않고 멈춘다.
        if len(data) < len(paths):
            err = r.stderr.decode("utf-8", "replace").strip()
            raise SystemExit(
                "exiftool 이 %d개 중 %d개만 읽었습니다 — 멈춥니다.\n"
                "  종료코드 %s\n  %s" % (len(paths), len(data), r.returncode, err[:400]))
        return {os.path.normcase(os.path.abspath(d.get("SourceFile", ""))):
                d.get("ImageDescription") for d in data}
    finally:
        os.unlink(argfile)


def write_description(exiftool: str, path: str, value: str) -> bool:
    """앱과 같은 방식으로 되살린다 — 값은 파일로 넘긴다(그래야 줄바꿈이 산다)."""
    d = tempfile.mkdtemp(prefix="exifrepair_")
    try:
        vp = os.path.join(d, "v.txt")
        with open(vp, "wb") as f:
            f.write(value.encode("utf-8"))
        ap = os.path.join(d, "a.args")
        with open(ap, "wb") as f:   # 위와 같은 이유로 바이너리
            f.write(b"-charset\nfilename=UTF8\n-charset\nUTF8\n-overwrite_original\n")
            f.write(("-ImageDescription<=" + vp + "\n").encode("utf-8", "surrogatepass"))
            f.write((path + "\n").encode("utf-8", "surrogatepass"))
        r = subprocess.run([exiftool, "-@", ap], capture_output=True)
        return r.returncode == 0
    finally:
        for n in ("v.txt", "a.args"):
            try: os.unlink(os.path.join(d, n))
            except OSError: pass
        try: os.rmdir(d)
        except OSError: pass


def ansi_safe_exiftool(path: str) -> str:
    """
    경로에 ANSI 로 못 쓰는 글자(한글·일본어)가 있으면 exiftool 을 옮겨서 그 경로를 준다.

    ★ 이걸 빼먹고 한참 헤맸다. exiftool.exe 는 자기 옆의 exiftool_files\\perl5*.dll 을
      시스템 ANSI 코드페이지로 찾는다. 경로가 'D:\\새 폴더 (2)\\...' 면 못 찾고
        Could not find D:\\? ?? (2)\\...\\exiftool_files\\perl5*.dll
      로 죽는다. 그런데 이 스크립트는 그 실패를 '설명이 비어 있음' 으로 읽었다 —
      읽기가 통째로 실패했는데 자료가 비었다고 결론 낸 것이다.
      도구가 안 돈 것과 자료가 없는 것은 다르다. 앱 본체(asciiSafeExiftool)가
      하는 것과 같은 일을 여기서도 한다.
    """
    try:
        path.encode("mbcs")
        return path                      # ANSI 로 쓸 수 있으면 그대로
    except (UnicodeEncodeError, LookupError):
        pass
    import shutil
    dst_dir = os.path.join(tempfile.gettempdir(), "predormition_exiftool")
    dst = os.path.join(dst_dir, "exiftool.exe")
    src_files = os.path.join(os.path.dirname(path), "exiftool_files")
    dst_files = os.path.join(dst_dir, "exiftool_files")
    os.makedirs(dst_dir, exist_ok=True)
    if not os.path.exists(dst) or os.path.getsize(dst) != os.path.getsize(path):
        shutil.copy2(path, dst)
    if os.path.isdir(src_files) and not os.path.isdir(dst_files):
        shutil.copytree(src_files, dst_files)
    return dst


def find_exiftool() -> str:
    here = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    for c in [os.path.join(here, "windows", "build_local", "exiftool.exe"),
              os.path.join(here, "windows", "resources", "tools", "exiftool.exe")]:
        if os.path.exists(c):
            return ansi_safe_exiftool(c)
    return "exiftool"


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("roots", nargs="+", help="훑을 폴더 (계정 폴더의 상위여도 된다)")
    ap.add_argument("--apply", action="store_true", help="실제로 고친다 (기본은 세어만 봄)")
    ap.add_argument("--limit", type=int, default=0, help="이만큼만 처리 (표본 확인용)")
    ap.add_argument("--also-missing", action="store_true",
                    help="줄바꿈과 무관하게 설명이 빈 것도 채운다")
    ap.add_argument("--exiftool", default=None)
    args = ap.parse_args()

    exiftool = args.exiftool or find_exiftool()
    # ★ 시작하자마자 도구가 실제로 도는지 본다. 안 돌면 여기서 멈춘다 —
    #   안 도는 도구로 훑으면 '자료가 전부 비었다' 는 그럴듯한 거짓 보고가 나온다.
    try:
        v = subprocess.run([exiftool, "-ver"], capture_output=True, timeout=30)
        ver = v.stdout.decode("utf-8", "replace").strip()
        if v.returncode != 0 or not ver:
            raise RuntimeError(v.stderr.decode("utf-8", "replace").strip() or "출력 없음")
    except Exception as e:
        sys.exit("exiftool 을 실행하지 못했습니다: %s\n  %s" % (exiftool, e))
    print("exiftool:", exiftool, "(%s)" % ver)
    print("모드    :", "실제로 고칩니다" if args.apply else "세어만 봅니다 (--apply 로 실행)")
    print()

    # 계정 폴더 = 엑셀이 있는 폴더. 그 아래 이미지들이 이 엑셀을 근거로 삼는다.
    account_dirs = []
    for root in args.roots:
        for dirpath, _dirs, files in os.walk(root):
            if any(f.lower().endswith(".xlsx") for f in files):
                account_dirs.append(dirpath)
    if not account_dirs:
        print("엑셀이 있는 계정 폴더를 못 찾았습니다 — 되살릴 근거가 없습니다.")
        return 0

    total = damaged = fixed = failed = no_text = ok = unclear = 0
    empty_damaged = partial_damaged = missing = not_post = 0
    for adir in sorted(account_dirs):
        texts = load_texts(adir)
        images = []
        for dirpath, _dirs, files in os.walk(adir):
            for fn in files:
                if os.path.splitext(fn)[1].lower() in IMAGE_EXT:
                    images.append(os.path.join(dirpath, fn))
        if not images:
            continue

        print("● %s — 이미지 %d개, 엑셀 원문 %d건" % (adir, len(images), len(texts)))
        # 설명 읽기는 500개씩 묶어서 (인자 파일이 너무 커지지 않게)
        for i in range(0, len(images), 500):
            chunk = images[i:i + 500]
            got = read_descriptions(exiftool, chunk)
            for p in chunk:
                total += 1
                cur = got.get(os.path.normcase(os.path.abspath(p)))
                m = ID_RE.findall(os.path.basename(p))
                if not m:
                    # 프로필 사진(profile_20260827.jpg) 처럼 게시물 미디어가 아닌 것.
                    #   전에는 이것을 '판단 보류' 에 넣었더니 한 계정에서만 418건이
                    #   쌓여, 진짜 보류가 몇 건인지 안 보였다. 따로 센다.
                    not_post += 1
                    continue
                want_full = texts.get(m[-1])
                if want_full is None:
                    no_text += 1
                    continue
                want = qt_left(want_full, 200)
                cur = "" if cur is None else str(cur)
                if cur == want:
                    ok += 1
                    continue

                has_nl = ("\n" in want) or ("\r" in want)
                if not has_nl:
                    # 줄바꿈이 없는데 값이 비어 있다 = 이 버그가 아닌 다른 이유다
                    #   (EXIF 끄고 받았거나, EXIF 를 넣기 전에 받은 자료거나).
                    #   되살릴 수는 있지만 이 스크립트가 고치겠다고 한 것은 아니다.
                    #   세어서 보여만 주고, 사용자가 --also-missing 을 주면 그때 한다.
                    if cur == "" and want:
                        missing += 1
                        if args.also_missing:
                            damaged += 1
                        else:
                            continue
                    else:
                        unclear += 1
                        continue
                else:
                    # 이 버그의 꼴 둘: 통째로 비었거나, 첫 줄만 들어갔거나
                    first = re.split(r"\r\n|\n|\r", want)[0]
                    if cur == "":
                        empty_damaged += 1
                    elif cur.strip() == first.strip():
                        partial_damaged += 1
                    else:
                        unclear += 1
                        continue
                    damaged += 1
                if args.apply:
                    if write_description(exiftool, p, want):
                        fixed += 1
                    else:
                        failed += 1
                if args.limit and damaged >= args.limit:
                    break
            if args.limit and damaged >= args.limit:
                break
        if args.limit and damaged >= args.limit:
            print("  (--limit %d 에 도달해 멈춥니다)" % args.limit)
            break

    print()
    print("═══ 결과 ═══")
    print("  훑은 이미지        %d" % total)
    print("  이미 온전함        %d" % ok)
    print("  원문을 못 찾음     %d  (엑셀에 그 게시물이 없음)" % no_text)
    print("  게시물 미디어 아님 %d  (프로필 사진 등 — 대상이 아님)" % not_post)
    print("  판단 보류          %d  (망가진 꼴이 아님 — 건드리지 않음)" % unclear)
    print("  설명이 비어 있음   %d  (줄바꿈과 무관 — --also-missing 을 줘야 고침)" % missing)
    print("  되살릴 대상        %d" % damaged)
    print("     ├ 통째로 비었던 것 %d" % empty_damaged)
    print("     └ 첫 줄만 있던 것  %d" % partial_damaged)
    if args.apply:
        print("  되살림             %d" % fixed)
        print("  실패               %d" % failed)
    else:
        print()
        print("  실제로 고치려면 같은 명령에 --apply 를 붙이세요.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
