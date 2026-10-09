#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""정리하기 — 받은 파일을 사용자의 정리 기준대로 '따로' 모은다. 원본은 그대로 둔다.

    정리할 곳/                                   ← 대상마다 사용자가 고른 최상위 폴더(예: gaekkin)
      gaekkin・@gaekkin-(X ／ twitter.com)/      ← 표시 이름・@핸들-(서비스 ／ 주소)
        令和7年11月/                              ← 올린 날짜(일본 시간)의 연호 · 해 · 달
          20251103_142000_….jpg                  ← 받은 이름 그대로, 내용이 같은 것은 한 번만

앱(정리하기 탭)이 부른다:  python3 organize.py --stdin-args      (첫 줄 = {"rules":[…], "captures":false})
손으로:                    python3 organize.py --rules-file rules.json [--dry-run]
규칙 하나: id · platform · target(핸들) · name(표시 이름, 비우면 자동) · roots(그 플랫폼의 저장 경로들 — 기본 · 보조) ·
          source(받은 곳, 비우면 roots 아래에서 찾음) · dest(정리할 곳 — 없으면 바로 위 폴더가 있을 때만 하나 만든다)
stdout 은 줄마다 JSON: log · resolved · progress · rule_done · done. stdin 이 닫히거나 SIGTERM 이면 하던 파일까지 끝내고 멈춘다.

★ 사용자 기준(2026-10-09): "최상위 폴더가 gaekkin 이면 거기 아래에 gaekkin・@gaekkin-(X / twitter.com) … 이 안에 令和7年11月 식으로 …
  중복 없이". 빗금은 항상 전각 ／(사용자 선택 — NAS · 윈도우에서도 같은 이름), 날짜는 '올린 날짜', 원본은 남긴다.
★ 날짜 — 앱이 받은 파일 이름 앞에는 올린 시각이 붙어 있다(yyyyMMdd_HHmmss_ · yyyyMMdd_HHmm_, 유튜브 %(upload_date)s_).
  없으면 profile_/banner_ 뒤의 날짜, 그것도 없으면 파일 시각을 일본 시간으로(트위터는 파일 시각을 올린 시각으로 맞춰 둔다).
  이름 속 아무 8자리나 날짜로 보지 않는다 — pixiv 작품 번호(19850612 등)가 날짜로 읽혔다(검토 실측).
  연호는 令和(2019-05-01~) · 平成 · 昭和, 첫해는 元年.
★ 중복 — 내용(SHA-256)이 같으면 한 번만. 지문은 '정리 폴더에 실제로 둔 바이트' 로 잰다 — 앱은 받은 뒤 EXIF 를 다시 써
  원본이 바뀌므로, 원본에서 잰 지문을 적으면 다음 판에 같은 것이 '(2)' 로 또 들어갔다(검토 실측).
  이름만 같고 내용이 다르면 'name (2).jpg'. 계정 폴더의 .hanishiki_organize.json 에 지문을 적어 다시 돌릴 때 빠르다.
  정리 폴더에서 지운 파일은 다음 정리 때 다시 들어온다(받은 곳에 아직 있으면).
★ 남의 것은 넣지 않는다 — 트위터는 profiles/ 아래에 팔로워 · 팔로잉 등 다른 계정의 프로필 사진도 둔다. profiles/ 는 대상 본인
  것(profiles/target/<이름>(@핸들))만 본다.
★ 원본을 건드리지 않는다 — 받은 곳은 읽기만 한다. 같은 APFS 디스크면 clonefile(자리를 거의 안 먹는 복제), 아니면 복사.
  숨은 임시 이름으로 쓴 뒤 '없을 때만' 이름을 단다(renamex_np RENAME_EXCL) — 이미 있는 파일을 덮지 않는다.
  받은 곳과 정리할 곳이 겹치면(한쪽이 다른 쪽 안 — 대소문자 · 한글 정규화 · 링크까지 inode 로 비교) 하지 않는다.
  계정 폴더마다 잠금(flock)을 걸어 두 정리가 같은 곳에 동시에 쓰지 않는다.
표준 라이브러리만 쓴다.
"""
from __future__ import annotations

import argparse
import ctypes
import datetime as dt
import errno
import fcntl
import hashlib
import json
import os
import re
import signal
import stat
import sys
import threading
import time
import unicodedata

STOP = threading.Event()
INDEX_NAME = '.hanishiki_organize.json'
LOCK_NAME = '.hanishiki_organize.lock'
NAMES_NAME = '.hanishiki_organize_names.json'   # 정리할 곳(최상위)에 — 한 번 정한 표시 이름을 다음에도 쓴다
JST = dt.timezone(dt.timedelta(hours=9))
NAME_MAX = 255                                   # APFS · NAS(리눅스 · SMB) 모두 이름 하나 UTF-8 255바이트

PLATFORMS = {   # 서비스 표시 — (이름, 주소). 사용자 예: "(X / twitter.com)"
    'twitter': ('X', 'twitter.com'), 'bluesky': ('Bluesky', 'bsky.app'), 'instagram': ('Instagram', 'instagram.com'),
    'pixiv': ('pixiv', 'pixiv.net'), 'fanbox': ('pixivFANBOX', 'fanbox.cc'), 'tumblr': ('Tumblr', 'tumblr.com'),
    'spinspin': ('SpinSpin', 'spin-spin.com'), 'asked': ('Asked', 'asked.kr'), 'youtube': ('YouTube', 'youtube.com'),
    'niconico': ('ニコニコ', 'nicovideo.jp'), 'discord': ('Discord', 'discord.com'), 'misskey': ('Misskey', 'misskey.io'),
}
PLATFORM_DIRS = {'bluesky': ['bluesky', 'bsky'], 'twitter': ['twitter', 'x']}
# 폴더 이름에 사용자 번호가 붙는 곳만 — pixiv '<번호>_<이름>', discord '<이름>_<번호>'. 다른 곳은 정확히 같은 이름만.
ID_JOINED = {'pixiv', 'fanbox', 'discord'}
MEDIA_EXT = {'jpg', 'jpeg', 'jfif', 'png', 'gif', 'webp', 'avif', 'heic', 'heif', 'bmp', 'tif', 'tiff',
             'mp4', 'mov', 'm4v', 'webm', 'mkv', 'avi', 'ts', 'flv',
             'mp3', 'm4a', 'aac', 'ogg', 'opus', 'wav', 'flac'}
CAPTURE_EXT = {'html', 'htm', 'mhtml', 'pdf'}
SKIP_SUFFIX = ('.part', '.ytdl', '.tmp', '.download', '.crdownload')
FULLWIDTH = {'/': '／', '\\': '＼', ':': '：', '*': '＊', '?': '？', '"': '＂', '<': '＜', '>': '＞', '|': '｜'}
SAVE_EVERY = 30.0   # 지문 장부는 30초마다 · 끝에 — 50개마다 통째로 쓰면 첫 정리에 NAS 로 수십 GB 를 썼다(검토 실측)


def emit(ev: str, **kw) -> None:
    kw['ev'] = ev
    try:
        sys.stdout.write(json.dumps(kw, ensure_ascii=False) + '\n')
        sys.stdout.flush()
    except (BrokenPipeError, ValueError):
        pass


def log(msg: str, level: str = 'info') -> None:
    emit('log', level=level, msg=msg)


def _trim_bytes(s: str, max_bytes: int) -> str:
    while len(s.encode('utf-8')) > max_bytes:
        s = s[:-1]
    return s


def safe_name(s: str, max_bytes: int = NAME_MAX) -> str:
    """앱의 FileHelper::sanitizeFilename(윈도우 호환)과 같은 치환. 길면 확장자는 두고 앞부분만 줄인다."""
    s = unicodedata.normalize('NFC', s or '')
    s = ''.join(FULLWIDTH.get(c, c) for c in s)
    s = re.sub(r'[\x00-\x1f]', '_', s).strip()
    while s.endswith('.') or s.endswith(' '):
        s = s[:-1]
    if not s:
        return '_'
    stem, dot, ext = s.rpartition('.')
    if dot and stem and len(ext) <= 8:
        return _trim_bytes(stem, max_bytes - len(('.' + ext).encode('utf-8'))) + '.' + ext
    return _trim_bytes(s, max_bytes)


def era_folder(d: dt.date) -> str:
    """令和7年11月 · 令和元年5月 · 平成31年4月 · 昭和64年1月."""
    if d >= dt.date(2019, 5, 1):
        era, n = '令和', d.year - 2018
    elif d >= dt.date(1989, 1, 8):
        era, n = '平成', d.year - 1988
    elif d >= dt.date(1926, 12, 25):
        era, n = '昭和', d.year - 1925
    else:
        return '%d年%d月' % (d.year, d.month)
    return '%s%s年%d月' % (era, '元' if n == 1 else str(n), d.month)


PREFIX_DATE = re.compile(r'^(\d{4})(\d{2})(\d{2})(?=[_\- .]|$)')
PROFILE_DATE = re.compile(r'(?:^|[_\-])(?:profile|banner|avatar|header|icon)_(\d{4})(\d{2})(\d{2})(?!\d)', re.I)


def _valid(y: str, m: str, d: str):
    try:
        v = dt.date(int(y), int(m), int(d))
    except ValueError:
        return None
    return v if 1980 <= v.year <= 2100 else None


def file_date(path: str, st):
    """(날짜, 어디서) — 이름 앞 → profile_/banner_ 뒤 → 파일 시각(일본 시간)."""
    base = os.path.basename(path)
    m = PREFIX_DATE.match(base)
    if m and _valid(*m.groups()):
        return _valid(*m.groups()), 'name'
    m = PROFILE_DATE.search(base)
    if m and _valid(*m.groups()):
        return _valid(*m.groups()), 'name'
    return dt.datetime.fromtimestamp(st.st_mtime, JST).date(), 'mtime'


def sha256_of(path: str) -> str:
    h = hashlib.sha256()
    with open(path, 'rb') as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


_libc = None


def _lib():
    global _libc
    if _libc is None and sys.platform == 'darwin':
        try:
            _libc = ctypes.CDLL('/usr/lib/libSystem.B.dylib', use_errno=True)
        except OSError:
            _libc = False
    return _libc or None


def clone_or_copy(src: str, tmp: str) -> str:
    """src → tmp(새 이름). APFS 면 clonefile, 아니면 1MB 씩 복사(STOP 을 보며). 시각은 원본대로. 'clone' | 'copy'."""
    lib = _lib()
    if lib is not None:
        try:
            if lib.clonefile(os.fsencode(src), os.fsencode(tmp), 0x0001) == 0:   # CLONE_NOFOLLOW
                st = os.stat(src)
                os.utime(tmp, ns=(st.st_atime_ns, st.st_mtime_ns))
                return 'clone'
        except (AttributeError, OSError):
            pass
    with open(src, 'rb') as fi, open(tmp, 'xb') as fo:
        while True:
            if STOP.is_set():
                raise InterruptedError
            b = fi.read(1 << 20)
            if not b:
                break
            fo.write(b)
    st = os.stat(src)
    os.utime(tmp, ns=(st.st_atime_ns, st.st_mtime_ns))
    return 'copy'


def publish(tmp: str, dst: str) -> bool:
    """tmp 를 dst 이름으로 — dst 가 이미 있으면 False(덮지 않는다)."""
    lib = _lib()
    if lib is not None and hasattr(lib, 'renamex_np'):
        if lib.renamex_np(os.fsencode(tmp), os.fsencode(dst), 0x00000004) == 0:   # RENAME_EXCL
            return True
        e = ctypes.get_errno()
        if e == errno.EEXIST:
            return False
        if e not in (errno.ENOTSUP, errno.EINVAL, errno.ENOSYS):
            raise OSError(e, os.strerror(e), dst)
    try:   # renamex_np 가 안 되는 파일 시스템(일부 NAS) — 하드 링크는 '없을 때만' 성공한다
        os.link(tmp, dst)
        os.unlink(tmp)
        return True
    except FileExistsError:
        return False
    except OSError:
        if os.path.lexists(dst):
            return False
        os.rename(tmp, dst)
        return True


def wanted(name: str, captures: bool) -> bool:
    if name.startswith('.') or name.lower().endswith(SKIP_SUFFIX):
        return False
    ext = name.rsplit('.', 1)[-1].lower() if '.' in name else ''
    return ext in MEDIA_EXT or (captures and ext in CAPTURE_EXT)


def walk_files(root: str, captures: bool, handle: str, errors: list):
    """받은 곳의 파일 — 숨은 것 · 임시는 빼고, profiles/ 는 대상 본인 것만. 링크 · 특수 파일은 따라가지 않는다."""
    own = re.compile(r'\(@' + re.escape(handle) + r'\)$', re.I) if handle else None
    root_n = os.path.normpath(root)

    def onerror(e):
        errors.append('%s — %s' % (getattr(e, 'filename', '?'), e.strerror or e))
    for dirpath, dirnames, filenames in os.walk(root, onerror=onerror):
        if STOP.is_set():
            return
        rel = os.path.relpath(dirpath, root_n)
        parts = [] if rel == '.' else rel.split(os.sep)
        keep = []
        for d in sorted(dirnames):
            if d.startswith('.') or d in ('_tmp', 'abiwa_tmp'):
                continue
            nd = unicodedata.normalize('NFC', d)
            if not parts and nd == 'profiles':
                keep.append(d)
            elif parts == ['profiles']:
                if nd == 'target':
                    keep.append(d)
            elif parts == ['profiles', 'target']:
                if own and own.search(nd):
                    keep.append(d)
            else:
                keep.append(d)
        dirnames[:] = keep
        for fn in sorted(filenames):
            if not wanted(fn, captures):
                continue
            p = os.path.join(dirpath, fn)
            try:
                st = os.lstat(p)
            except OSError as e:
                errors.append('%s — %s' % (p, e.strerror))
                continue
            if stat.S_ISREG(st.st_mode):   # 링크 · FIFO · 장치는 건너뛴다(밖의 파일을 끌어오거나 멈춘다)
                yield p, st


def find_sources(rule: dict):
    """규칙의 받은 곳들. source 가 있으면 그것, 없으면 roots(기본 · 보조 저장 경로) 아래에서 앱의 이름 규칙대로."""
    src = (rule.get('source') or '').strip()
    if src:
        src = os.path.expanduser(src)
        return ([src] if os.path.isdir(src) else []), None
    roots = [os.path.expanduser(r.strip()) for r in (rule.get('roots') or [rule.get('root') or '']) if r and r.strip()]
    target = (rule.get('target') or '').strip().lstrip('@')
    plat = rule.get('platform') or ''
    if not target:
        return [], None
    want = {target.lower(), safe_name(target, 150).lower()}
    found, ambiguous = [], None
    for root in dict.fromkeys(roots):
        for sub in PLATFORM_DIRS.get(plat, [plat]):
            base = os.path.join(root, sub)
            if not os.path.isdir(base):
                continue
            names = os.listdir(base)
            exact = [n for n in names if unicodedata.normalize('NFC', n).lower() in want]
            if exact:
                found.append(os.path.join(base, exact[0]))
                continue
            if plat in ID_JOINED:
                pat = [re.compile(r'^\d+_' + re.escape(w) + r'$') for w in want] + \
                      [re.compile(r'^' + re.escape(w) + r'_\d+$') for w in want] + \
                      [re.compile(r'^' + re.escape(w) + r'_.+$') for w in want if w.isdigit()]   # pixiv: 대상이 번호
                cand = [n for n in names if any(p.match(unicodedata.normalize('NFC', n).lower()) for p in pat)]
                if len(cand) == 1:
                    found.append(os.path.join(base, cand[0]))
                elif len(cand) > 1:
                    ambiguous = sorted(cand)[:4]
    return [s for s in dict.fromkeys(found) if os.path.isdir(s)], ambiguous


def overlap(a: str, b: str) -> bool:
    """a 와 b 가 같거나 한쪽이 다른 쪽 안인가 — 대소문자 · 한글 정규화(NFC/NFD) · 링크와 무관하게 inode 로."""
    def chain(p):
        out, cur = [], os.path.realpath(p)
        while True:
            try:
                st = os.stat(cur)
                out.append((st.st_dev, st.st_ino))
            except OSError:
                pass
            nxt = os.path.dirname(cur)
            if nxt == cur:
                return out
            cur = nxt
    try:
        sa, sb = os.stat(a), os.stat(b)
    except OSError:
        return False
    ka, kb = (sa.st_dev, sa.st_ino), (sb.st_dev, sb.st_ino)
    return ka == kb or ka in chain(b) or kb in chain(a)


def svc_suffix(rule: dict) -> str:
    plat = rule.get('platform') or ''
    label, domain = PLATFORMS.get(plat, (rule.get('label') or plat or '기타', rule.get('domain') or ''))
    return safe_name('(%s ／ %s)' % (label, domain) if domain else '(%s)' % label)


def account_folder(rule: dict, name: str) -> str:
    handle = (rule.get('target') or '').strip().lstrip('@')
    who = name + ('・@' + handle if handle else '')
    return safe_name('%s-%s' % (who, svc_suffix(rule)))


def pick_name(rule: dict, dest: str, sources: list) -> str:
    """표시 이름 — 규칙에 적힌 것 > 정리할 곳에 이미 있는 그 핸들의 폴더 > 전에 정한 이름 > 받은 곳의 profiles/target > 핸들.
    이름을 바꿔 가며 폴더가 새로 생기면 전부를 다시 복사한다(검토 실측) — 한 번 정한 것을 계속 쓴다."""
    name = (rule.get('name') or '').strip()
    handle = (rule.get('target') or '').strip().lstrip('@')
    if name:
        return name
    tail = ('・@' + handle + '-' + svc_suffix(rule)) if handle else ('-' + svc_suffix(rule))
    try:
        for n in sorted(os.listdir(dest)):
            nn = unicodedata.normalize('NFC', n)
            if nn.lower().endswith(tail.lower()) and os.path.isdir(os.path.join(dest, n)) and len(nn) > len(tail):
                return nn[:-len(tail)]
    except OSError:
        pass
    key = '%s:%s' % (rule.get('platform') or '', handle.lower())
    try:
        with open(os.path.join(dest, NAMES_NAME), encoding='utf-8') as f:
            known = json.load(f).get(key)
        if known:
            return known
    except (OSError, ValueError):
        pass
    if handle:
        pat = re.compile(r'^(.+)\(@' + re.escape(handle) + r'\)$', re.I)
        best = None
        for src in sources:
            base = os.path.join(src, 'profiles', 'target')
            try:
                for d in os.listdir(base):
                    m = pat.match(unicodedata.normalize('NFC', d))
                    if m and m.group(1).strip():
                        mt = os.stat(os.path.join(base, d)).st_mtime
                        if best is None or mt > best[0]:
                            best = (mt, m.group(1).strip())   # 이름을 바꾼 계정이면 가장 새것
            except OSError:
                pass
        if best:
            return best[1]
    return handle or os.path.basename(sources[0]) if sources else (handle or '이름 없음')


def remember_name(rule: dict, dest: str, name: str) -> None:
    handle = (rule.get('target') or '').strip().lstrip('@')
    if (rule.get('name') or '').strip() or not handle:
        return
    path = os.path.join(dest, NAMES_NAME)
    try:
        with open(path, encoding='utf-8') as f:
            d = json.load(f)
    except (OSError, ValueError):
        d = {}
    key = '%s:%s' % (rule.get('platform') or '', handle.lower())
    if d.get(key) == name:
        return
    d[key] = name
    tmp = path + '.%d.part' % os.getpid()
    with open(tmp, 'w', encoding='utf-8') as f:
        json.dump(d, f, ensure_ascii=False)
    os.replace(tmp, path)


class Index:
    """계정 폴더의 지문 장부.
    dest: 정리 폴더 안 상대 경로 → [크기, 시각, ctime, 지문] — 지금 있는 파일만, 바뀐 것은 다시 잰다.
    src : 받은 곳의 파일 → [크기, 시각, ctime, inode, 지문, 정리 폴더의 상대 경로] — 같은 크기로 다시 쓰고 시각을 되돌려도
          ctime 이 바뀌므로 다시 잰다. 한 번 정리한 원본(같은 경로)은 내용이 바뀌어도 다시 두지 않는다 — 앱은 다음 수집 때
          이미 받은 사진의 EXIF 를 다시 쓴다. 다시 두면 같은 사진이 메타데이터만 다른 채 '(2)' 로 쌓인다.
          정리 폴더에서 그 파일을 지웠으면 다시 넣는다."""

    def __init__(self, folder: str):
        self.folder = folder
        self.path = os.path.join(folder, INDEX_NAME)
        self.dest: dict = {}
        self.src: dict = {}
        self.hashes: dict = {}
        self.last_save = time.time()
        old_dest, old_src = {}, {}
        try:
            with open(self.path, encoding='utf-8') as f:
                d = json.load(f)
            if d.get('version') == 2:
                old_dest, old_src = d.get('dest', {}), d.get('src', {})
        except (OSError, ValueError):
            pass
        self.src = old_src
        for dirpath, dirnames, filenames in os.walk(folder):
            if STOP.is_set():
                break
            dirnames[:] = [x for x in dirnames if not x.startswith('.')]
            for fn in filenames:
                full = os.path.join(dirpath, fn)
                if fn.startswith('.org-') and fn.endswith('.part'):   # 지난번에 끊긴 임시 파일
                    try:
                        os.unlink(full)
                    except OSError:
                        pass
                    continue
                if fn.startswith('.') or fn.endswith('.part'):
                    continue
                rel = os.path.relpath(full, folder)
                try:
                    st = os.lstat(full)
                    if not stat.S_ISREG(st.st_mode):
                        continue
                    o = old_dest.get(rel)
                    if o and o[0] == st.st_size and o[1] == st.st_mtime_ns and o[2] == st.st_ctime_ns:
                        h = o[3]
                    else:
                        h = sha256_of(full)   # 새로 보이거나 바뀐 파일 — 손으로 넣은 것도 중복 판정에 넣는다
                    self.dest[rel] = [st.st_size, st.st_mtime_ns, st.st_ctime_ns, h]
                    self.hashes.setdefault(h, rel)
                except OSError:
                    pass
                self.maybe_save()

    @staticmethod
    def src_key(path: str, base: str) -> str:
        return '%s|%s' % (base, os.path.relpath(path, base) if base else path)

    def placed(self, key: str):
        """이 원본을 전에 정리해 둔 곳(지금도 있으면)."""
        c = self.src.get(key)
        return c[5] if c and len(c) > 5 and c[5] in self.dest else None

    def src_digest(self, path: str, st, key: str) -> str:
        c = self.src.get(key)
        self.seen.add(key)
        if c and c[:4] == [st.st_size, st.st_mtime_ns, st.st_ctime_ns, st.st_ino]:
            return c[4]
        h = sha256_of(path)
        self.src[key] = [st.st_size, st.st_mtime_ns, st.st_ctime_ns, st.st_ino, h] + (c[5:6] if c and len(c) > 5 else [])
        return h

    def link(self, key: str, rel: str) -> None:
        c = self.src.get(key)
        if c:
            self.src[key] = c[:5] + [rel]

    seen: set = set()

    def add(self, rel: str, h: str) -> None:
        st = os.lstat(os.path.join(self.folder, rel))
        self.dest[rel] = [st.st_size, st.st_mtime_ns, st.st_ctime_ns, h]
        self.hashes.setdefault(h, rel)

    def maybe_save(self) -> None:
        if time.time() - self.last_save >= SAVE_EVERY:
            self.save()

    def save(self, prune: bool = False) -> None:
        if prune:   # 이번에 본 원본만 남긴다 — 장부가 끝없이 자라지 않게
            self.src = {k: v for k, v in self.src.items() if k in self.seen}
        tmp = self.path + '.%d.part' % os.getpid()
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump({'version': 2, 'dest': self.dest, 'src': self.src}, f, ensure_ascii=False)
        os.replace(tmp, self.path)
        self.last_save = time.time()


def place(idx: Index, path: str, st, month_dir: str, counter: list) -> tuple:
    """원본 → 정리 폴더. 둔 바이트의 지문으로 중복을 판정한다. ('copied' | 'dup', 'clone' | 'copy' | '')."""
    counter[0] += 1
    tmp = os.path.join(month_dir, '.org-%d-%d.part' % (os.getpid(), counter[0]))
    try:
        how = clone_or_copy(path, tmp)
        h = sha256_of(tmp)
        if h in idx.hashes:
            os.unlink(tmp)
            return 'dup', h, ''
        name = safe_name(os.path.basename(path))
        stem, dot, ext = name.rpartition('.')
        if not dot:
            stem, ext = name, ''
        k = 1
        while True:
            cand = name if k == 1 else '%s (%d)%s' % (_trim_bytes(stem, NAME_MAX - len((' (%d).%s' % (k, ext)).encode('utf-8'))),
                                                      k, ('.' + ext) if ext else '')
            dst = os.path.join(month_dir, cand)
            if publish(tmp, dst):
                idx.add(os.path.relpath(dst, idx.folder), h)
                return 'copied', h, how
            k += 1
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


def run_rule(i: int, rule: dict, captures: bool, dry: bool) -> dict:
    res = {'index': i, 'id': rule.get('id') or '', 'copied': 0, 'clone': 0, 'dup': 0, 'mtime_date': 0, 'errors': 0}
    dest = os.path.expanduser((rule.get('dest') or '').strip())
    label = rule.get('target') or rule.get('name') or '?'
    if not dest:
        log('규칙 %d (%s): 정리할 곳이 비어 있습니다' % (i + 1, label), 'error')
        res['errors'] += 1
        return res
    # 빠진 NAS 자리(/Volumes/<이름> 이 마운트가 아님)에는 쓰지 않는다 — 내장 디스크에 써 버린다
    m = re.match(r'^(/Volumes/[^/]+)', os.path.realpath(os.path.dirname(dest.rstrip('/')) or dest))
    if m and not os.path.ismount(m.group(1)):
        log('규칙 %d (%s): %s 가 붙어 있지 않습니다 — 디스크를 연결한 뒤 다시' % (i + 1, label, m.group(1)), 'error')
        res['errors'] += 1
        return res
    if not os.path.isdir(dest):
        # 최상위 폴더(예: …/gaekkin) 하나는 만들어 준다 — 바로 위 폴더가 있을 때만. 깊은 경로를 통째로 만들지 않는다
        parent = os.path.dirname(dest.rstrip('/'))
        if parent and os.path.isdir(parent) and not dry:
            try:
                os.mkdir(dest)
                log('규칙 %d (%s): 정리할 곳을 만들었습니다 — %s' % (i + 1, label, dest))
            except OSError as ex:
                log('규칙 %d (%s): 정리할 곳을 만들지 못했습니다 — %s' % (i + 1, label, ex.strerror or ex), 'error')
                res['errors'] += 1
                return res
        else:
            log('규칙 %d (%s): 정리할 곳이 없고 그 위 폴더도 없습니다(디스크가 빠졌거나 지워짐) — %s' % (i + 1, label, dest), 'error')
            res['errors'] += 1
            return res
    sources, ambiguous = find_sources(rule)
    if ambiguous:
        log('규칙 %d (%s): 받은 곳 후보가 여럿입니다(%s) — 정리하기 탭에서 받은 곳을 직접 고르십시오' % (i + 1, label, ', '.join(ambiguous)), 'warning')
    if not sources:
        log('규칙 %d (%s): 받은 곳을 찾지 못했습니다 — 정리하기 탭에서 받은 곳을 직접 고르십시오' % (i + 1, label), 'warning')
        res['errors'] += 1
        return res
    for s in sources:
        if overlap(s, dest):
            log('규칙 %d (%s): 받은 곳과 정리할 곳이 겹칩니다(한쪽이 다른 쪽 안) — 따로 떨어진 곳을 고르십시오' % (i + 1, label), 'error')
            res['errors'] += 1
            return res
    handle = (rule.get('target') or '').strip().lstrip('@')
    name = pick_name(rule, dest, sources)
    folder = os.path.join(dest, account_folder(rule, name))
    emit('resolved', index=i, id=res['id'], source=' · '.join(sources), name=name, folder=folder)
    errors: list = []
    if dry:
        n = sum(1 for s in sources for _ in walk_files(s, captures, handle, errors))
        log('규칙 %d: (시험) 파일 %d개 → %s' % (i + 1, n, folder))
        return res
    os.makedirs(folder, exist_ok=True)
    remember_name(rule, dest, name)
    lockf = open(os.path.join(folder, LOCK_NAME), 'a')
    try:
        try:
            fcntl.flock(lockf, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError:
            log('규칙 %d (%s): 다른 정리가 같은 폴더를 쓰고 있습니다 — 건너뜁니다' % (i + 1, label), 'warning')
            res['errors'] += 1
            return res
        idx = Index(folder)
        idx.seen = set()
        counter = [0]
        n = 0
        for src in sources:
            for path, st in walk_files(src, captures, handle, errors):
                if STOP.is_set():
                    break
                n += 1
                try:
                    key = Index.src_key(path, src)
                    h = None
                    if idx.placed(key):   # 전에 정리한 원본 — 내용이 바뀌었어도(EXIF 재기록) 다시 두지 않는다
                        idx.seen.add(key)
                        res['dup'] += 1
                    else:
                        h = idx.src_digest(path, st, key)
                    if h is None:
                        pass
                    elif h in idx.hashes:
                        idx.link(key, idx.hashes[h])
                        res['dup'] += 1
                    else:
                        d, how = file_date(path, st)
                        if how == 'mtime':
                            res['mtime_date'] += 1
                        month_dir = os.path.join(folder, era_folder(d))
                        os.makedirs(month_dir, exist_ok=True)
                        kind, h2, made = place(idx, path, st, month_dir, counter)
                        idx.link(key, idx.hashes.get(h2, ''))
                        if kind == 'dup':
                            res['dup'] += 1
                        else:
                            res['copied'] += 1
                            if made == 'clone':
                                res['clone'] += 1
                except InterruptedError:
                    break
                except OSError as ex:
                    res['errors'] += 1
                    if res['errors'] <= 5:
                        log('규칙 %d: %s — %s' % (i + 1, os.path.basename(path), ex.strerror or ex), 'warning')
                if n % 50 == 0:
                    emit('progress', index=i, id=res['id'], seen=n, copied=res['copied'], dup=res['dup'])
                idx.maybe_save()
        idx.save(prune=not STOP.is_set())
    finally:
        lockf.close()
    for e in errors[:5]:
        log('규칙 %d: 읽을 수 없는 곳 — %s' % (i + 1, e), 'warning')
    res['errors'] += len(errors)
    return res


def main() -> int:
    ap = argparse.ArgumentParser(description='정리하기 — 받은 파일을 연호 · 달 폴더로(원본은 그대로)')
    ap.add_argument('--stdin-args', action='store_true')
    ap.add_argument('--rules-file')
    ap.add_argument('--dry-run', action='store_true')
    ap.add_argument('--captures', action='store_true')
    a = ap.parse_args()
    if a.stdin_args:
        cfg = json.loads(sys.stdin.readline() or '{}')

        def watch_stdin():   # 앱이 stdin 을 닫으면(중지 · 앱이 죽음) 하던 파일까지 끝내고 멈춘다
            for _ in sys.stdin:
                pass
            STOP.set()
        threading.Thread(target=watch_stdin, daemon=True).start()
    elif a.rules_file:
        with open(a.rules_file, encoding='utf-8') as f:
            cfg = json.load(f)
    else:
        ap.error('--stdin-args 또는 --rules-file')
    signal.signal(signal.SIGTERM, lambda *_: STOP.set())
    signal.signal(signal.SIGINT, lambda *_: STOP.set())
    rules = [r for r in (cfg.get('rules') or []) if isinstance(r, dict)]
    captures = bool(cfg.get('captures') or a.captures)
    dry = bool(cfg.get('dry_run') or a.dry_run)
    tot = {'copied': 0, 'clone': 0, 'dup': 0, 'mtime_date': 0, 'errors': 0}
    for i, rule in enumerate(rules):
        if STOP.is_set():
            break
        try:
            r = run_rule(i, rule, captures, dry)
        except OSError as ex:   # 한 규칙의 문제(권한 · 디스크)가 나머지 규칙을 막지 않게
            log('규칙 %d: %s' % (i + 1, ex), 'error')
            r = {'index': i, 'id': rule.get('id') or '', 'copied': 0, 'clone': 0, 'dup': 0, 'mtime_date': 0, 'errors': 1}
        emit('rule_done', **r)
        for k in tot:
            tot[k] += r[k]
    emit('done', stopped=STOP.is_set(), rules=len(rules), **tot)
    return 0


if __name__ == '__main__':
    sys.exit(main())
