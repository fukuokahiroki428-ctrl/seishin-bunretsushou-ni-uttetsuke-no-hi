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
  것(profiles/target/<이름>(@핸들))만 본다. 블루스카이는 profiles/<날짜>/<짧은 핸들>_avatar|banner 만, 디스코드는 profiles/ 를
  통째로 뺀다(채널 사람들의 것). 인스타그램 · pixiv 의 profiles/ 에는 본인 것만 있어 통째로 넣는다.
★ 사본 폴더는 한 번만 — 트위터 · 블루스카이 · pixiv 의 _complete/ 는 받은 파일을 같은 이름으로 한 벌 더 둔 것이다. 같은 이름의
  원본이 받은 곳에 있으면 _complete 쪽은 넘긴다(앱이 원본의 EXIF 를 다시 쓰면 지문이 달라져 같은 사진이 '(2)' 로 또 들어갔다).
  원본을 지운 _complete 파일만 정리한다.
★ 받은 곳 찾기(r56) — 수집기마다 폴더 이름 규칙이 달라 못 찾던 것: 블루스카이는 '.bsky.social' 을 뗀 이름, 디스코드는
  discord/<서버 이름>/<채널 이름>_<채널 ID>, pixiv 는 '<ID>_<이름>'(이름을 바꾸면 여럿 · 이름을 모르면 '<ID>' 만 — 모두 쓴다).
  r56 전의 pixiv 북마크는 작가 폴더에 있어, 대상의 북마크 엑셀(pixiv/<ID>_bookmarks_complete.xlsx)로 골라 온다.
★ '받은 것 없음' 은 그 탭의 저장 경로가 살아 있을 때만 — 빠진 디스크 · 읽을 수 없는 폴더는 문제로 세고, 까닭(why)을 화면에 남긴다.
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
import subprocess
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
# 폴더 이름에 사용자 번호가 붙는 곳만 — pixiv '<번호>_<이름>'. 다른 곳은 정확히 같은 이름만.
# ★ discord 는 따로 찾는다(find_discord) — 폴더가 서버 이름/채널 이름_채널 ID 라 이 꼴로는 하나도 맞지 않았다(r56 검토)
ID_JOINED = {'pixiv', 'fanbox', 'discord'}
# _complete/ 가 받은 파일의 같은 이름 사본인 수집기(TwitterCollector · bluesky_daemon · pixiv 수집) — walk_files 가 사본을 넘긴다
MIRROR_PLATFORMS = {'twitter', 'bluesky', 'pixiv'}
# profiles/ 에 대상 본인 것만 두는 수집기 — 통째로 넣는다(인스타그램 profiles/<날짜>/ · pixiv profiles/avatar_…)
PROFILES_ALL = {'instagram', 'pixiv'}
# 네트워크 · 클라우드 저장소 뿌리(앱의 NAS 고르기와 같은 곳 — HanishikiBackend 의 duckRoots). 이 아래는 붙어 있는지 따로 확인한다
NET_ROOTS = (('duck', '~/Mountain Duck'),
             ('duck', '~/Library/Containers/io.mountainduck/Data/Library/Application Support/Mountain Duck/Volumes.noindex'),
             ('cloud', '~/Library/Group Containers/G69SCX94XU.duck/Library/Application Support/Cyberduck/Drive'),
             ('cloud', '~/Library/CloudStorage'))
NOT_ATTACHED = '저장 디스크가 연결되어 있지 않습니다'
DENIED = '받은 곳을 읽을 권한이 없습니다'
CANT_WRITE = '정리할 곳에 쓸 수 없습니다'
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


# ── 틀(사용자 2026-10-15 '정리하기를 틀 하나로') — 다른 앱들의 경로 형식 문자열(gallery-dl directory · JDownloader 규칙)처럼
#   정리할 곳 하나 + 틀 하나로 모든 계정을 정리한다. '/' 로 단을 나누고, 날짜 낱말이 없는 앞 단들이 계정 폴더,
#   날짜 낱말이 든 뒤 단들이 달 폴더다. 계정 폴더에는 {name} 이나 {handle} 이 있어야 한다(계정마다 갈라야 하니).
DEFAULT_TEMPLATE = '{name}・@{handle}-({service})/{wareki}{month}月'
DATE_TOKENS = ('wareki', 'era', 'eyear', 'year', 'month', 'mm', 'day', 'dd')
TOKENS = ('name', 'handle', 'service', 'label', 'domain', 'platform') + DATE_TOKENS
TEMPLATE = DEFAULT_TEMPLATE


def split_template(tpl: str):
    segs = [x.strip() for x in (tpl or DEFAULT_TEMPLATE).split('/') if x.strip()]
    acc, date = [], []
    for x in segs:
        if date or any('{' + t + '}' in x for t in DATE_TOKENS):
            date.append(x)
        else:
            acc.append(x)
    return acc, date


def template_problem(tpl: str) -> str:
    unknown = sorted(set(re.findall(r'\{([a-z]+)\}', tpl or '')) - set(TOKENS))
    if unknown:
        return '모르는 낱말이 있습니다 — ' + ', '.join('{%s}' % x for x in unknown)
    acc, _date = split_template(tpl)
    if not acc:
        return '틀의 앞쪽(계정 폴더)에 날짜 없는 단이 하나는 있어야 합니다'
    if not any('{name}' in x or '{handle}' in x for x in acc):
        return '틀의 계정 폴더 쪽에 {name} 이나 {handle} 이 있어야 합니다(계정마다 갈라야 합니다)'
    return ''


def render(seg: str, vals: dict) -> str:
    if not vals.get('handle'):
        seg = seg.replace('・@{handle}', '').replace('@{handle}', '')   # 핸들이 없는 규칙(유튜브 채널 등)
    return re.sub(r'\{([a-z]+)\}', lambda m: str(vals.get(m.group(1), m.group(0))), seg)


def rule_vals(rule: dict, name: str) -> dict:
    plat = rule.get('platform') or ''
    label, domain = PLATFORMS.get(plat, (rule.get('label') or plat or '기타', rule.get('domain') or ''))
    return {'name': name, 'handle': (rule.get('target') or '').strip().lstrip('@'), 'platform': plat,
            'label': label, 'domain': domain, 'service': ('%s ／ %s' % (label, domain)) if domain else label}


def date_vals(d: dt.date) -> dict:
    w = era_folder(d)                     # '令和7年11月' — 연호 · 해만 떼어 쓴다
    wareki = w[:w.index('年') + 1] if '年' in w else '%d年' % d.year
    era = re.match(r'^(\D+?)(元|\d+)年', wareki)
    return {'wareki': wareki, 'era': era.group(1) if era else '', 'eyear': era.group(2) if era else str(d.year),
            'year': str(d.year), 'month': str(d.month), 'mm': '%02d' % d.month, 'day': str(d.day), 'dd': '%02d' % d.day}


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


def _nfc(s: str) -> str:
    return unicodedata.normalize('NFC', s or '')


def bluesky_short(handle: str) -> str:
    """블루스카이 수집기가 쓰는 폴더 이름 — 폴더를 만드는 bluesky_daemon.norm_handle 과 글자 하나까지 같은 규칙
    (h.strip().lstrip('@').replace('.bsky.social', '') — 대소문자를 가리고, 끝이 아닌 곳의 것도 뗀다).
    ★ norm_handle 은 데몬의 함수 안에 들어 있어 불러 쓸 수 없고, 정리하기는 표준 라이브러리만으로 혼자 돌아야 해 같은 줄을 여기 둔다.
      그쪽을 바꾸면 여기도 바꿀 것(unit_organize 가 둘을 맞대어 본다)."""
    return (handle or '').strip().lstrip('@').replace('.bsky.social', '')


def bluesky_shorts(handle: str) -> set:
    """받은 곳을 찾을 때 볼 블루스카이 이름들(소문자) — 데몬의 폴더 이름(bluesky_short)과, BlueskyCollector(C++)가 엑셀 ·
    갤러리를 찾을 때 쓰는 '끝의 .bsky.social 만 뗀' 이름. 폴더 비교는 대소문자를 가리지 않으므로 끝은 대소문자 없이 뗀다
    (대상을 'Alice.Bsky.Social' 로 적었으면 데몬은 떼지 못해 그 이름 그대로 폴더가 된다 — 그것은 대상 이름으로 찾는다)."""
    h = (handle or '').strip().lstrip('@')
    tail = h[:-len('.bsky.social')] if h.lower().endswith('.bsky.social') else h
    return {bluesky_short(h).lower(), tail.lower()}


def walk_files(root: str, captures: bool, handle: str, errors: list, plat: str = '', stats=None):
    """받은 곳의 파일 — 숨은 것 · 임시는 빼고, profiles/ 는 대상 본인 것만. 링크 · 특수 파일은 따라가지 않는다.
    plat 마다 profiles/ 규칙이 다르다(머리말). _complete/ 사본은 같은 이름의 원본이 있으면 넘기고 stats['mirror'] 에 센다."""
    own = re.compile(r'\(@' + re.escape(handle) + r'\)$', re.I) if handle else None
    bsky_own = re.compile(r'^' + re.escape(_nfc(handle)) + r'_(?:avatar|banner)\.', re.I) if handle else None
    root_n = os.path.normpath(root)
    mirror = plat in MIRROR_PLATFORMS
    deferred = []      # (_complete 폴더, 그 부모) — 원본을 다 본 뒤에 본다
    names_at = {}      # 폴더 → 거기서 내놓은 파일 이름(NFC) — _complete 사본인지 가리는 데

    def onerror(e):
        errors.append('%s — %s' % (getattr(e, 'filename', '?'), e.strerror or e))

    def files_in(dirpath, filenames, parts):
        for fn in sorted(filenames):
            if not wanted(fn, captures):
                continue
            if plat == 'bluesky' and parts and parts[0] == 'profiles' and not (bsky_own and bsky_own.match(_nfc(fn))):
                continue   # ★ 블루스카이 profiles/<날짜>/ 에는 팔로워의 <핸들>_avatar 도 함께 있다 — 대상 본인 것만
            p = os.path.join(dirpath, fn)
            try:
                st = os.lstat(p)
            except OSError as e:
                errors.append('%s — %s' % (p, e.strerror))
                continue
            if stat.S_ISREG(st.st_mode):   # 링크 · FIFO · 장치는 건너뛴다(밖의 파일을 끌어오거나 멈춘다)
                yield p, st

    for dirpath, dirnames, filenames in os.walk(root, onerror=onerror):
        if STOP.is_set():
            return
        rel = os.path.relpath(dirpath, root_n)
        parts = [] if rel == '.' else [_nfc(x) for x in rel.split(os.sep)]
        keep = []
        for d in sorted(dirnames):
            if d.startswith('.') or d in ('_tmp', 'abiwa_tmp'):
                continue
            nd = _nfc(d)
            if mirror and nd == '_complete':
                deferred.append((os.path.join(dirpath, d), os.path.normpath(dirpath)))
                continue
            if plat == 'discord':
                if nd != 'profiles':   # ★ 디스코드 profiles/ 는 채널에서 글을 쓴 사람들의 사진 — 대상(채널)의 것이 아니다
                    keep.append(d)
            elif plat in PROFILES_ALL or plat == 'bluesky':
                keep.append(d)         # 블루스카이는 profiles/ 안의 파일을 files_in 에서 거른다
            elif not parts and nd == 'profiles':
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
        for p, st in files_in(dirpath, filenames, parts):
            if mirror:
                names_at.setdefault(os.path.normpath(dirpath), set()).add(_nfc(os.path.basename(p)))
            yield p, st
    # ★ _complete 는 받은 파일의 같은 이름 사본 — 원본이 있으면 넘긴다. 내용으로만 가리면 앱이 원본의 EXIF 를 다시 쓴 뒤로
    #   지문이 달라 같은 사진이 '(2)' 로 한 번 더 들어갔다(사용자 2026-11 '중복'). 원본을 지운 사본만 정리한다.
    for cdir, parent in deferred:
        have = set()
        for dp, ns in names_at.items():
            if dp == parent or dp.startswith(parent + os.sep):
                have |= ns
        for dirpath, dirnames, filenames in os.walk(cdir, onerror=onerror):
            if STOP.is_set():
                return
            dirnames[:] = [d for d in sorted(dirnames) if not d.startswith('.')]
            for p, st in files_in(dirpath, filenames, ['_complete']):
                if _nfc(os.path.basename(p)) in have:
                    if stats is not None:
                        stats['mirror'] = stats.get('mirror', 0) + 1
                    continue
                yield p, st


_ROOT_STATE: dict = {}
# ★ 떨어진 뿌리를 로그에 이미 알린 것 — 한 번 정리하는 동안 뿌리마다 한 번만 적는다(두 번째 저장 경로가 빠진 외장 디스크면
#   받은 것이 있는 규칙마다 같은 줄이 수백 번 나와 로그가 묻힌다 — 검토). 규칙마다의 까닭(why)에는 그대로 남는다.
_DEAD_LOGGED: set = set()


def _net_mount(path: str):
    """경로가 놓인 붙였다 뗐다 하는 자리 — ('volume' | 'duck' | 'cloud', 그 자리) 또는 (None, None)."""
    p = os.path.abspath(os.path.expanduser(path))
    m = re.match(r'^(/Volumes/[^/]+)', p)
    if m:
        return 'volume', m.group(1)
    for kind, r in NET_ROOTS:
        r = os.path.expanduser(r)
        if p.startswith(r + '/'):
            return kind, os.path.join(r, p[len(r) + 1:].split('/', 1)[0])
    return None, None


def _probe_alive(mp: str, timeout: float = 8.0) -> bool:
    """★ 따로 띄운 stat 으로 본다(앱의 probeMountAlive 와 같은 방식) — 끊긴 네트워크 마운트에 이 프로세스가 stat 을 걸면
    커널에서 끝없이 기다려 정리 전체가 멈춘다. 시간 안에 답이 없으면 떨어진 것으로 본다."""
    try:
        r = subprocess.run(['/usr/bin/stat', '-f', '%z', mp], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=timeout)
        return r.returncode == 0
    except subprocess.TimeoutExpired:
        return False
    except OSError:
        return os.path.exists(mp)


def root_state(path: str):
    """저장 경로(뿌리)가 살아 있나 — (True, '') 또는 (False, 까닭). 한 번 정리하는 동안 뿌리마다 한 번만 잰다
    (떨어진 NAS 를 계정마다 8초씩 기다리면 수백 계정에 한 시간이 걸린다)."""
    path = os.path.expanduser((path or '').strip())
    if not path:
        return False, '그 탭의 저장 경로가 비어 있습니다'
    if path in _ROOT_STATE:
        return _ROOT_STATE[path]
    kind, mp = _net_mount(path)
    res = (True, '')
    if mp and not _probe_alive(mp):
        res = (False, '%s — %s' % (NOT_ATTACHED, mp))
    elif kind == 'volume':
        real = os.path.realpath(mp)   # '/Volumes/Macintosh HD' 는 / 로 가는 링크 — 마운트 자리가 아니어도 붙어 있다
        if real.startswith('/Volumes/') and not os.path.ismount(real):
            res = (False, '%s — %s' % (NOT_ATTACHED, mp))
    elif kind == 'duck':
        try:   # 연결이 끊긴 Mountain Duck 자리는 빈 폴더로 남는다
            empty = not os.listdir(mp)
        except PermissionError:
            # ★ 다른 앱의 컨테이너 보호(~/Library/Containers/io.mountainduck)로 목록만 막힌 것 — 빠진 것이 아니다.
            #   붙어 있다고 보고, 받은 곳을 읽을 때 _listdir 이 '읽을 권한이 없습니다' 로 알린다(까닭을 바꿔 말하지 않게)
            empty = False
        except OSError:
            empty = True
        if empty:
            res = (False, '%s — %s' % (NOT_ATTACHED, mp))
    if res[0] and not os.path.isdir(path):
        res = (False, ('%s — %s' % (NOT_ATTACHED, path)) if mp else ('저장 경로가 없습니다 — %s (그 탭의 저장 경로를 확인하십시오)' % path))
    _ROOT_STATE[path] = res
    return res


def _listdir(base: str, info: dict):
    """폴더 목록 — 읽을 권한이 없으면(EPERM · EACCES — 다른 앱의 컨테이너 보호 · 막힌 마운트) info['denied'] 에 적고 빈 목록.
    ★ 예전엔 이 오류가 규칙 밖으로 새어 '문제 1개' 로만 남았다(검토: fanbox · instagram 의 errors:1)."""
    try:
        return os.listdir(base)
    except PermissionError:
        info.setdefault('denied', []).append(base)
    except OSError as ex:
        if ex.errno in (errno.EPERM, errno.EACCES):
            info.setdefault('denied', []).append(base)
        else:
            info.setdefault('unreadable', []).append('%s — %s' % (base, ex.strerror or ex))
    return []


def _discord_guild(chan_dir: str) -> str:
    """채널 폴더의 index.json 에 적힌 서버 ID(수집기가 guild_id · channel_info.guild_id 로 적는다)."""
    try:
        with open(os.path.join(chan_dir, 'index.json'), encoding='utf-8') as f:
            d = json.load(f)
        return str(d.get('guild_id') or (d.get('channel_info') or {}).get('guild_id') or '')
    except (OSError, ValueError, AttributeError):
        return ''


def find_discord(roots: list, target: str, info: dict) -> list:
    """디스코드 대상('서버' · '서버/채널' · '/채널')의 받은 곳 — 수집기는 discord/<서버 이름>/<채널 이름>_<채널 ID>/ 에 둔다
    (서버 이름을 못 얻으면 <서버 ID>, DM 은 'DM'). 채널이 있으면 끝이 _<채널 ID> 인 채널 폴더, 서버만이면 discord/<서버 ID>/
    아래 채널 폴더들과 index.json 의 서버 ID 가 같은 채널 폴더들."""
    server, _sep, channel = target.partition('/')
    server, channel = server.strip(), channel.strip().strip('/')
    out = []
    for root in roots:
        base = os.path.join(root, 'discord')
        if not os.path.isdir(base):
            continue
        info['bases'] = info.get('bases', 0) + 1
        for srv in sorted(_listdir(base, info)):
            sp = os.path.join(base, srv)
            if srv.startswith('.') or not os.path.isdir(sp):
                continue
            whole = bool(server) and not channel and _nfc(srv) == server
            for ch in sorted(_listdir(sp, info)):
                cp = os.path.join(sp, ch)
                if ch.startswith('.') or ch == 'profiles' or not os.path.isdir(cp):
                    continue
                if channel:
                    ok = _nfc(ch).endswith('_' + channel)
                else:
                    ok = whole or (bool(server) and _discord_guild(cp) == server)
                if ok:
                    out.append(cp)
    return out


def _near(names: list, want: set, limit: int = 3) -> list:
    """찾는 이름과 비슷한 폴더 — 앞 네 글자가 같거나 서로 들어 있거나, 번호 대상이면 그 번호가 든 것."""
    out = []
    for n in sorted(names):
        nl = _nfc(n).lower()
        if n.startswith('.'):
            continue
        for w in want:
            if not w:
                continue
            if (len(w) >= 4 and (nl.startswith(w[:4]) or w in nl or (len(nl) >= 4 and nl in w))) or (w.isdigit() and w in nl):
                out.append(n)
                break
        if len(out) >= limit:
            break
    return out


def find_sources(rule: dict, info=None):
    """규칙의 받은 곳들. source 가 있으면 그것, 없으면 roots(기본 · 보조 저장 경로) 아래에서 앱의 이름 규칙대로.
    (받은 곳들, 후보가 여럿일 때 그 이름들). info 를 주면 살핀 폴더 수(bases) · 비슷한 이름(near) · 못 읽은 곳(denied) ·
    떨어진 뿌리(dead)를 적는다 — '받은 것 없음' 과 '못 찾음' 을 가르고 까닭을 남기는 데 쓴다."""
    info = {} if info is None else info
    info.setdefault('bases', 0)
    info.setdefault('near', [])
    info.setdefault('denied', [])
    info.setdefault('dead', [])
    src = (rule.get('source') or '').strip()
    if src:
        src = os.path.expanduser(src)
        return ([src] if os.path.isdir(src) else []), None
    roots = [os.path.expanduser(r.strip()) for r in (rule.get('roots') or [rule.get('root') or '']) if r and r.strip()]
    target = (rule.get('target') or '').strip().lstrip('@')
    plat = rule.get('platform') or ''
    if not target:
        return [], None
    live = []
    for root in dict.fromkeys(roots):
        ok, why = root_state(root)
        if ok:
            live.append(root)
        else:   # ★ 떨어진 마운트는 들여다보지 않는다 — 끊긴 NAS 에 isdir · listdir 을 걸면 멈춘다
            info['dead'].append(why)
    if plat == 'discord':
        found = find_discord(live, target, info)
        return [s for s in dict.fromkeys(found) if os.path.isdir(s)], None
    want = {target.lower(), safe_name(target, 150).lower()}
    if plat == 'bluesky':   # ★ 수집기는 '.bsky.social' 을 뗀 이름으로 폴더를 만든다(alice.bsky.social → bluesky/alice)
        for short in bluesky_shorts(target):
            want |= {short, safe_name(short, 150).lower()}
    numeric = target.isdigit()
    found, ambiguous = [], None
    for root in live:
        for sub in PLATFORM_DIRS.get(plat, [plat]):
            base = os.path.join(root, sub)
            if not os.path.isdir(base):
                continue
            info['bases'] += 1
            names = _listdir(base, info)
            exact = [n for n in names if _nfc(n).lower() in want]
            if exact and not (plat in ID_JOINED and numeric):
                # ★ 블루스카이는 'alice' 와 'alice.bsky.social' 이 같은 계정이다(뗀 이름 · 다 쓴 이름 — 둘 다 있으면 둘 다 받은 곳)
                found.extend(os.path.join(base, n) for n in (sorted(exact) if plat == 'bluesky' else exact[:1]))
                continue
            cand = []
            if plat in ID_JOINED:
                pat = [re.compile(r'^\d+_' + re.escape(w) + r'$') for w in want] + \
                      [re.compile(r'^' + re.escape(w) + r'_\d+$') for w in want] + \
                      [re.compile(r'^' + re.escape(w) + r'_.*$') for w in want if w.isdigit()]   # pixiv: 대상이 번호
                cand = [n for n in names if n not in exact and os.path.isdir(os.path.join(base, n))
                        and any(p.match(_nfc(n).lower()) for p in pat)]
            if numeric and plat in ID_JOINED:
                # ★ 번호 대상(pixiv)은 '<번호>' 와 '<번호>_<이름>' 들을 모두 쓴다 — 이름을 바꾸면 '<번호>_<새 이름>' 이 하나 더 생기고
                #   (예전: 후보 여럿이라 매번 문제), 이름을 못 얻은 판은 '<번호>' 만의 폴더를 만든다(예전: 그것이 작품 폴더를 가림).
                #   같은 사람의 폴더들이라 모두 받은 곳으로 — 같은 내용은 지문으로 한 번만 둔다.
                own = [n for n in cand if _nfc(n).lower().startswith(target.lower() + '_')]
                if own or exact:
                    found.extend(os.path.join(base, n) for n in exact[:1] + sorted(own))
                    continue
            if len(cand) == 1:
                found.append(os.path.join(base, cand[0]))
            elif len(cand) > 1:
                ambiguous = sorted(cand)[:4]
            elif not exact:
                info['near'].extend(os.path.join(sub, n) for n in _near(names, want))
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


def account_folder(rule: dict, name: str) -> str:
    """정리할 곳 아래의 계정 폴더(상대 경로) — 틀의 날짜 없는 앞 단들."""
    acc, _date = split_template(TEMPLATE)
    vals = rule_vals(rule, name)
    return os.path.join(*[safe_name(render(x, vals)) for x in acc])


def month_folder(rule: dict, name: str, d: dt.date) -> str:
    """계정 폴더 아래의 달 폴더(상대 경로) — 틀의 날짜 단들. 날짜 단이 없으면 계정 폴더 바로 아래."""
    _acc, date = split_template(TEMPLATE)
    vals = dict(rule_vals(rule, name), **date_vals(d))
    return os.path.join(*[safe_name(render(x, vals)) for x in date]) if date else ''


def _last_seg_pattern(rule: dict):
    """계정 폴더 마지막 단에서 이름 자리를 뺀 꼴 — 이미 있는 그 계정의 폴더를 찾고, 거기서 이름을 읽는다.
    (이름이 마지막 단에만 있을 때 — 기본 틀이 그렇다. 위 단에 이름이 있으면 찾지 않는다.)"""
    acc, _date = split_template(TEMPLATE)
    if '{name}' not in acc[-1] or any('{name}' in x for x in acc[:-1]):
        return None, None
    mark = '\ue000'   # 사용자 영역 글자 — safe_name 이 바꾸지 않는다(제어 글자는 '_' 로 바뀐다)
    vals = rule_vals(rule, mark)
    whole = safe_name(render(acc[-1], vals))
    if mark not in whole:
        return None, None
    pre, post = whole.split(mark, 1)
    parent = os.path.join(*([safe_name(render(x, vals)) for x in acc[:-1]] or ['']))
    return parent, re.compile('^' + re.escape(unicodedata.normalize('NFC', pre)) + '(.+)'
                              + re.escape(unicodedata.normalize('NFC', post)) + '$', re.I)


def _xlsx_rows(path: str):
    """앱이 쓴 엑셀의 첫 시트 — 줄마다 칸의 글(문자열) 목록. 표준 라이브러리(zip + XML)만으로. 못 읽으면 빈 목록."""
    import zipfile
    import xml.etree.ElementTree as ET
    ns = '{http://schemas.openxmlformats.org/spreadsheetml/2006/main}'
    out = []
    try:
        with zipfile.ZipFile(path) as z:
            shared = []
            if 'xl/sharedStrings.xml' in z.namelist():
                for si in ET.fromstring(z.read('xl/sharedStrings.xml')).iter(ns + 'si'):
                    shared.append(''.join(t.text or '' for t in si.iter(ns + 't')))
            sheets = sorted(n for n in z.namelist() if n.startswith('xl/worksheets/sheet') and n.endswith('.xml'))
            if not sheets:
                return []
            for row in ET.fromstring(z.read(sheets[0])).iter(ns + 'row'):
                vals = []
                for c in row.iter(ns + 'c'):
                    ref = re.match(r'^([A-Z]+)', c.get('r') or '')
                    if ref:   # 빈 칸을 건너뛴 줄(B1 다음 D1) — 칸 자리를 맞춘다
                        col = 0
                        for ch in ref.group(1):
                            col = col * 26 + (ord(ch) - 64)
                        while len(vals) < col - 1:
                            vals.append('')
                    t, v = c.get('t'), c.find(ns + 'v')
                    if t == 's' and v is not None and (v.text or '').isdigit() and int(v.text) < len(shared):
                        vals.append(shared[int(v.text)])
                    elif t == 'inlineStr':
                        vals.append(''.join(x.text or '' for x in c.iter(ns + 't')))
                    else:
                        vals.append(v.text if v is not None and v.text else '')
                out.append(vals)
    except (OSError, zipfile.BadZipFile, ET.ParseError, ValueError, KeyError):
        return []
    return out


def _xlsx_field(path: str, field: str) -> str:
    """앱이 쓰는 프로필 엑셀(Field | Value 두 칸)에서 한 줄의 값."""
    for vals in _xlsx_rows(path):
        if len(vals) >= 2 and vals[0].strip().lower() == field.lower():
            return vals[1].strip()
    return ''


def _newest(paths):
    """가장 최근에 쓴 것(파일 시각) — 없으면 ''."""
    best, when = '', -1.0
    for p in paths:
        try:
            t = os.stat(p).st_mtime
        except OSError:
            continue
        if t > when:
            best, when = p, t
    return best


def _json_at(path: str) -> dict:
    try:
        with open(path, encoding='utf-8') as f:
            d = json.load(f)
        return d if isinstance(d, dict) else {}
    except (OSError, ValueError):
        return {}


def other_profile_name(plat: str, sources: list, handle: str) -> str:
    """트위터 밖 수집기의 '지금' 이름 — 각 수집기가 남기는 프로필 기록에서(r56 검토 ORG-5: 모두 핸들로 떨어졌다).
    bluesky: excel/<짧은 핸들>_profile.xlsx 의 'Display Name' · pixiv: profiles/profile.json 의 user_name >
    가장 최근 폴더 '<ID>_<이름>' 의 이름 · instagram: profile.json(뿌리 · profiles/<날짜>/)의 full_name ·
    fanbox: 가장 최근 posts/<글>.json 의 user.name · discord: '<서버 폴더 이름> #<채널 이름>'(index.json)."""
    if plat == 'bluesky':
        shorts = {sh + '_profile.xlsx' for sh in bluesky_shorts(handle)}
        xs = []
        for src in sources:
            ex = os.path.join(src, 'excel')
            for fn in (os.listdir(ex) if os.path.isdir(ex) else []):
                if fn.lower().endswith('_profile.xlsx') and not fn.startswith('.'):
                    xs.append((0 if fn.lower() in shorts else 1, os.path.join(ex, fn)))
        mine = [p for k, p in xs if k == 0] or [p for _k, p in xs]
        x = _newest(mine)
        return _xlsx_field(x, 'Display Name') if x else ''
    if plat == 'pixiv':
        pj = _newest(os.path.join(src, 'profiles', 'profile.json') for src in sources)
        name = str(_json_at(pj).get('user_name') or '').strip() if pj else ''
        if name:
            return name
        pat = re.compile(r'^' + re.escape(handle) + r'_(.+)$')
        named = [s for s in sources if pat.match(_nfc(os.path.basename(s)))]
        n = _newest(named)
        return pat.match(_nfc(os.path.basename(n))).group(1).strip() if n else ''
    if plat == 'instagram':
        js = []
        for src in sources:
            js.append(os.path.join(src, 'profile.json'))
            pd = os.path.join(src, 'profiles')
            for d in (os.listdir(pd) if os.path.isdir(pd) else []):
                js.append(os.path.join(pd, d, 'profile.json'))
        pj = _newest(js)
        return str(_json_at(pj).get('full_name') or '').strip() if pj else ''
    if plat == 'fanbox':
        posts = []
        for src in sources:
            pd = os.path.join(src, 'posts')
            try:
                posts.extend(os.path.join(pd, fn) for fn in os.listdir(pd) if fn.endswith('.json') and not fn.startswith('.'))
            except OSError:
                pass
        pj = _newest(posts)
        u = _json_at(pj).get('user') if pj else None
        return str(u.get('name') or '').strip() if isinstance(u, dict) else ''
    if plat == 'discord' and sources:
        chans = []
        for src in sources:
            name = str(_json_at(os.path.join(src, 'index.json')).get('channel_name') or '').strip()
            chans.append(name or re.sub(r'_\d+$', '', _nfc(os.path.basename(src))))
        server = _nfc(os.path.basename(os.path.dirname(sources[0])))
        if '/' in handle and handle.split('/', 1)[1].strip():
            return '%s #%s' % (server, chans[0]) if server and not server.isdigit() else '#' + chans[0]
        return server if server and not server.isdigit() else ''
    return ''


def current_profile_name(sources: list, handle: str, plat: str = '') -> str:
    """그 계정의 '지금' 이름 — 앱이 수집 때마다 profiles/target/<이름>(@핸들)/ 을 남긴다(이름을 바꾸면 폴더가 하나 더 생긴다).
    가장 최근 것을 고른다: 폴더 안 profile_/banner_YYYYMMDD 의 날짜와 프로필 엑셀의 시각 중 늦은 것. 이름은 엑셀의
    'Name' 줄(원래 글자 그대로) > 폴더 이름의 앞부분(앱이 40자로 자르고 일부 글자를 뺀 것).
    트위터 모양이 아닌 수집기(bluesky · pixiv · instagram · fanbox · discord)는 other_profile_name."""
    if not handle:
        return ''
    if plat in ('bluesky', 'pixiv', 'instagram', 'fanbox', 'discord'):
        try:
            return other_profile_name(plat, sources, handle)
        except OSError:
            return ''
    pat = re.compile(r'^(.+)\(@' + re.escape(handle) + r'\)$', re.I)
    best = None   # (최근 시각, 엑셀 경로, 폴더 이름의 이름 부분)
    for src in sources:
        base = os.path.join(src, 'profiles', 'target')
        try:
            names = os.listdir(base)
        except OSError:
            continue
        for d in names:
            mm = pat.match(unicodedata.normalize('NFC', d))
            if not mm:
                continue
            folder = os.path.join(base, d)
            when, xlsx = 0.0, ''
            try:
                for fn in os.listdir(folder):
                    full = os.path.join(folder, fn)
                    dm = PROFILE_DATE.search(fn)
                    if dm and _valid(*dm.groups()):
                        v = _valid(*dm.groups())
                        when = max(when, dt.datetime(v.year, v.month, v.day, tzinfo=JST).timestamp())
                    if fn.lower().endswith('_profile.xlsx'):
                        xlsx = full
                        when = max(when, os.stat(full).st_mtime)
                if not when:
                    when = os.stat(folder).st_mtime
            except OSError:
                continue
            if best is None or when > best[0]:
                best = (when, xlsx, mm.group(1).strip())
    if not best:
        return ''
    return (_xlsx_field(best[1], 'Name') if best[1] else '') or best[2]


def owner_key(rule: dict) -> str:
    handle = (rule.get('target') or '').strip().lstrip('@').lower()
    return '%s:%s' % (rule.get('platform') or '', handle) if handle else ''


def folder_owners(folder: str):
    """계정 폴더의 장부에 적힌 주인들 — 장부가 없으면 None, r42 장부(주인 칸 없음)면 []."""
    try:
        with open(os.path.join(folder, INDEX_NAME), encoding='utf-8') as f:
            d = json.load(f)
        return [str(x) for x in (d.get('owners') or []) if x]
    except (OSError, ValueError, AttributeError):
        return None


def existing_account_folder(rule: dict, dest: str):
    """정리할 곳에 이미 있는 '그 계정의' 폴더 — (계정 폴더의 상대 경로, 그 안의 이름) 또는 ('', '').
    ★ 이름 자리를 뺀 꼴로 찾으면 다른 계정의 폴더나 사용자의 폴더도 걸린다(검토: 핸들 없는 규칙이 남의 계정 폴더를 제 이름으로
      바꿈 · '{name}/…' 틀이 ~/Pictures/Screenshots 를 바꿈 · 'Foo_x_abc' 가 'abc' 에 걸림). 그래서 그 폴더의 장부에 이 규칙이
      주인으로 적혀 있을 때만 그 계정의 것으로 본다. 주인 칸이 없는 폴더(r42 장부 · 장부 없음)는 기본 틀일 때만 —
      기본 틀은 '・@핸들-(서비스)' 로 끝나 핸들이 다른 계정과 섞이지 않는다."""
    me = owner_key(rule)
    if not me:
        return '', ''
    parent, pat = _last_seg_pattern(rule)
    if pat is None:
        return '', ''
    base = os.path.join(dest, parent) if parent else dest
    legacy_ok = TEMPLATE == DEFAULT_TEMPLATE
    owned, legacy = None, None
    try:
        for n in sorted(os.listdir(base)):
            mm = pat.match(unicodedata.normalize('NFC', n))
            full = os.path.join(base, n)
            if not mm or not os.path.isdir(full):
                continue
            hit = ((os.path.join(parent, n) if parent else n), mm.group(1))
            owners = folder_owners(full)
            if owners and me in owners:
                owned = owned or hit
            elif not owners and legacy_ok:
                legacy = legacy or hit
    except OSError:
        pass
    return owned or legacy or ('', '')


def pick_name(rule: dict, dest: str, sources: list) -> str:
    """표시 이름 — 규칙에 직접 적은 이름 > 그 계정의 지금 이름(가장 최근 수집의 프로필) > 정리할 곳에 이미 있는 폴더의 이름 >
    전에 정한 이름 > 핸들. 사용자 기준(2026-10-15): 'gaekkin' 자리에는 그 계정이 지금 트위터에서 쓰는 이름이 들어가야 한다.
    이름이 바뀌면 run_rule 이 옛 폴더의 이름만 바꾼다(다시 복사하지 않는다)."""
    name = (rule.get('name') or '').strip()
    handle = (rule.get('target') or '').strip().lstrip('@')
    if name:
        return name
    cur = current_profile_name(sources, handle, rule.get('platform') or '')
    if cur:
        return cur
    old, old_name = existing_account_folder(rule, dest)
    if old_name:
        return old_name
    key = '%s:%s' % (rule.get('platform') or '', handle.lower())
    try:
        with open(os.path.join(dest, NAMES_NAME), encoding='utf-8') as f:
            known = json.load(f).get(key)
        if known:
            return known
    except (OSError, ValueError):
        pass
    if handle:
        return handle
    return os.path.basename(sources[0]) if sources else '이름 없음'


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
        self.owners: list = []
        self.last_save = time.time()
        old_dest, old_src = {}, {}
        try:
            with open(self.path, encoding='utf-8') as f:
                d = json.load(f)
            if d.get('version') == 2:
                old_dest, old_src = d.get('dest', {}), d.get('src', {})
                self.owners = [str(x) for x in (d.get('owners') or []) if x]
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

    def save(self, prune: bool = False, bases=()) -> None:
        if prune:   # 이번에 훑은 받은 곳에서 안 보인 원본만 지운다 — 장부가 끝없이 자라지 않게.
            # ★ 다른 받은 곳(같은 계정 폴더를 쓰는 다른 규칙)의 것은 남긴다 — 지우면 그 규칙이 EXIF 를 다시 쓴 원본을
            #   '(2)' 로 또 두었다(검토: 틀에 {service} 가 없고 같은 핸들이 두 플랫폼에 있을 때)
            mine = set(bases)
            self.src = {k: v for k, v in self.src.items() if k in self.seen or k.split('|', 1)[0] not in mine}
        tmp = self.path + '.%d.part' % os.getpid()
        with open(tmp, 'w', encoding='utf-8') as f:
            json.dump({'version': 2, 'owners': self.owners, 'dest': self.dest, 'src': self.src}, f, ensure_ascii=False)
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
            # ★ 그 이름에 이미 '같은 바이트' 가 있으면(장부가 아직 모르는 파일 — 손으로 넣음 · 장부를 다 못 읽은 판) '(n)' 으로 한 벌 더
            #   두지 않는다. 같은 내용은 어디에도 두 번 두지 않는다(사용자 2026-11 '중복')
            try:
                if os.lstat(dst).st_size == os.lstat(tmp).st_size and sha256_of(dst) == h:
                    os.unlink(tmp)
                    idx.add(os.path.relpath(dst, idx.folder), h)
                    return 'dup', h, ''
            except OSError:
                pass
            k += 1
    except BaseException:
        try:
            os.unlink(tmp)
        except OSError:
            pass
        raise


PX_ID_IN_NAME = re.compile(r'\((\d+)(?:_\d+)?\)\.[A-Za-z0-9]+$')   # pixiv 파일 이름 끝의 '(작품ID[_쪽])' — 수집기의 reId 와 같다
PX_CAPTURE_NAME = re.compile(r'^\d{8}_\d{6}_(\d+)\.[A-Za-z0-9]+$')   # pixiv 캡처 'yyyyMMdd_HHmmss_<작품ID>.html' — FileHelper::captureNameCompat


_PX_LEGACY: dict = {}


def _px_legacy_dirs(base: str, info: dict) -> list:
    """pixiv/ 아래 r56 전 자리의 북마크 폴더들 [(폴더 이름, 작가 폴더, …/bookmarks/illusts)] — 뿌리마다 한 번만 훑는다
    (★ pixiv 대상이 수백이면 대상마다 pixiv/ 의 폴더 수천 개를 다시 살피게 된다 — NAS 에서 특히 느리다)."""
    if base not in _PX_LEGACY:
        out = []
        for n in sorted(_listdir(base, info)):
            if n.startswith('.'):
                continue
            d = os.path.join(base, n)
            bm = os.path.join(d, 'bookmarks', 'illusts')
            if os.path.isdir(bm):
                out.append((n, d, bm))
        _PX_LEGACY[base] = out
    return _PX_LEGACY[base]


def pixiv_old_bookmarks(rule: dict, sources: list, info: dict, captures: bool = False):
    """r56 전의 pixiv 북마크 — 수집기가 북마크한 '작가' 의 폴더(pixiv/<작가ID>_<이름>/bookmarks/illusts/)에 두었다. 대상(북마크한
    사람)의 북마크 엑셀 pixiv/<대상ID>_bookmarks_complete.xlsx 에 적힌 작품 ID(1칸) · 작가 ID('Author ID' 칸)로 그 파일들만 골라 온다.
    [(작가 폴더, [(경로, stat), …]), …]. 엑셀이 없는데 작가 폴더에 북마크가 있으면 info['bm_unknown'] 에 적는다(조용히 '없음' 으로
    두지 않는다). r56 부터 수집기는 북마크를 대상 폴더 아래(pixiv/<대상>/bookmarks/<작가>/)에 두므로 그것은 find_sources 가 찾는다.
    captures 면 그 작품들의 캡처(작가 폴더의 captures/<날짜>_<작품ID>.html)도 — 북마크 작품의 캡처도 작가 폴더에 떴다.
    ★ r56 전의 북마크 소설(작가 폴더의 bookmarks/novels/)은 고르지 못한다 — 엑셀에 소설 줄이 없어 누가 북마크했는지 알 수 없다."""
    target = (rule.get('target') or '').strip().lstrip('@')
    if (rule.get('platform') or '') != 'pixiv' or not target.isdigit() or (rule.get('source') or '').strip():
        return []
    roots = [os.path.expanduser(r.strip()) for r in (rule.get('roots') or [rule.get('root') or '']) if r and r.strip()]
    mine = {os.path.normpath(s) for s in sources}
    out = []
    for root in dict.fromkeys(roots):
        if not root_state(root)[0]:
            continue
        base = os.path.join(root, 'pixiv')
        if not os.path.isdir(base):
            continue
        rows = _xlsx_rows(os.path.join(base, target + '_bookmarks_complete.xlsx'))
        ids, authors = set(), set()
        if rows:
            head = [str(x).strip().lower() for x in rows[0]]
            ac = head.index('author id') if 'author id' in head else -1
            for r in rows[1:]:
                iid = str(r[0]).strip() if r else ''
                if iid.endswith('.0'):
                    iid = iid[:-2]
                if iid.isdigit():
                    ids.add(iid)
                    if 0 <= ac < len(r) and str(r[ac]).strip().split('.')[0].isdigit():
                        authors.add(str(r[ac]).strip().split('.')[0])
        legacy = []
        for n, d, bm in _px_legacy_dirs(base, info):
            if os.path.normpath(d) in mine:
                continue
            if authors and n.split('_', 1)[0] not in authors:
                continue
            legacy.append((d, bm))
        if not ids:
            # ★ '북마크로만 받은 대상' 일 때만 알린다(r56 검토 COMB-D1) — 대상 폴더는 있는데(수집기가 북마크 판에도 profiles/ 를 만든다)
            #   그 안에 제 작품(illusts/ · novels/)이 없고 북마크 엑셀도 없을 때. 예전엔 엑셀이 없는 모든 pixiv 대상에 붙였고,
            #   그것을 보여 주는 '받은 것 없음' 쪽으로는 그런 대상이 오지 않아 정작 알릴 때는 말이 없었다.
            own_works = any(os.path.isdir(os.path.join(s, sub)) for s in sources for sub in ('illusts', 'novels'))
            if legacy and not rows and sources and not own_works:
                info['bm_unknown'] = info.get('bm_unknown', 0) + len(legacy)
            continue
        for d, bm in legacy:
            files = []
            looks = [(bm, PX_ID_IN_NAME, False)]
            cap = os.path.join(d, 'captures')
            if captures and os.path.isdir(cap):
                # 작가 폴더의 captures/ 에는 작가 본인 작품의 캡처도 있다 — 북마크 엑셀의 작품 ID 인 것만(이름 끝 '_<작품ID>')
                looks.append((cap, PX_CAPTURE_NAME, True))
            for where, pat, is_cap in looks:
                for fn in sorted(_listdir(where, info)):
                    m = pat.search(fn)
                    if not m or m.group(1) not in ids or not wanted(fn, is_cap):
                        continue
                    if is_cap and fn.rsplit('.', 1)[-1].lower() not in CAPTURE_EXT:
                        continue
                    p = os.path.join(where, fn)
                    try:
                        st = os.lstat(p)
                    except OSError:
                        continue
                    if stat.S_ISREG(st.st_mode):
                        files.append((p, st))
            if files:
                out.append((d, files))
    return out


def _cant_write(ex: OSError) -> bool:
    return ex.errno in (errno.EROFS, errno.EACCES, errno.EPERM)


def run_rule(i: int, rule: dict, captures: bool, dry: bool) -> dict:
    res = {'index': i, 'id': rule.get('id') or '', 'copied': 0, 'clone': 0, 'dup': 0, 'mtime_date': 0, 'errors': 0, 'missing': 0}
    dest = os.path.expanduser((rule.get('dest') or '').strip())
    label = rule.get('target') or rule.get('name') or '?'

    # ★ 까닭(why)을 rule_done 에 실어 화면(정리하기 표의 ✕ · '받은 것 없음' 풍선)에 남긴다 — 예전엔 수만 남아, 앱을 다시 열면
    #   '문제 1개 — 로그를 보십시오' 의 로그가 사라져 까닭을 알 길이 없었다(검토 ORG-4). 첫 문제의 글이 그 계정의 까닭이다.
    def note(msg: str) -> None:
        if not res.get('why'):
            res['why'] = msg

    def fail(msg: str, level: str = 'error', n: int = 1) -> dict:
        log('규칙 %d (%s): %s' % (i + 1, label, msg), level)
        note(msg)
        res['errors'] += n
        return res

    if not dest:
        return fail('정리할 곳이 비어 있습니다')
    # 빠진 NAS 자리(/Volumes/<이름> 이 마운트가 아님)에는 쓰지 않는다 — 내장 디스크에 써 버린다
    m = re.match(r'^(/Volumes/[^/]+)', os.path.realpath(os.path.dirname(dest.rstrip('/')) or dest))
    if m and not os.path.ismount(m.group(1)):
        return fail('%s 가 붙어 있지 않습니다 — 디스크를 연결한 뒤 다시' % m.group(1))
    if not os.path.isdir(dest):
        # 최상위 폴더(예: …/gaekkin) 하나는 만들어 준다 — 바로 위 폴더가 있을 때만. 깊은 경로를 통째로 만들지 않는다
        parent = os.path.dirname(dest.rstrip('/'))
        if parent and os.path.isdir(parent) and not dry:
            try:
                os.mkdir(dest)
                log('규칙 %d (%s): 정리할 곳을 만들었습니다 — %s' % (i + 1, label, dest))
            except OSError as ex:
                if _cant_write(ex):
                    return fail('%s(읽기 전용이거나 권한 없음) — %s' % (CANT_WRITE, dest))
                return fail('정리할 곳을 만들지 못했습니다 — %s' % (ex.strerror or ex))
        else:
            return fail('정리할 곳이 없고 그 위 폴더도 없습니다(디스크가 빠졌거나 지워짐) — %s' % dest)
    info: dict = {}
    sources, ambiguous = find_sources(rule, info)
    old_bm = pixiv_old_bookmarks(rule, sources, info, captures)
    has_src = bool(sources or old_bm)
    plat = rule.get('platform') or ''
    manual = bool((rule.get('source') or '').strip())
    if ambiguous:
        fail('받은 곳 후보가 여럿입니다(%s) — 정리하기 탭에서 받은 곳을 직접 고르십시오' % ', '.join(ambiguous), 'warning', 0)
    if info.get('denied') and not has_src:
        # ★ 읽을 권한이 없는 곳(다른 앱의 컨테이너 보호 · 막힌 마운트) — '받은 것 없음' 이 아니다
        return fail('%s — %s (시스템 설정 › 개인정보 보호 및 보안 › 전체 디스크 접근 권한에서 이 앱을 허용하십시오)' % (DENIED, info['denied'][0]))
    if info.get('denied'):
        fail('%s — %s · 읽을 수 있는 곳만 정리합니다' % (DENIED, info['denied'][0]), 'warning')
    if not has_src:
        roots = [r for r in (rule.get('roots') or []) if r and str(r).strip()]
        # ★ '받은 것 없음' 은 그 탭의 저장 경로(첫 뿌리)가 살아 있을 때만 — 예전엔 아무 뿌리(보조 경로)라도 있으면 그렇게 세어,
        #   빠진 외장 디스크 · 끊긴 NAS 가 '아직 안 받음' 으로 보였다(검토 ORG-3)
        primary = str(rule['primary']).strip() if 'primary' in rule else (str(roots[0]).strip() if roots else '')
        p_ok, p_why = root_state(primary)
        if manual:
            return fail('받은 곳이 없습니다(디스크가 빠졌거나 옮겨짐) — %s' % os.path.expanduser(rule['source'].strip()), 'warning')
        if not p_ok:
            return fail(p_why)
        bits = ['저장 경로 %d곳의 %s 폴더 %d곳을 살폈지만 이 계정의 폴더가 없습니다' % (len(dict.fromkeys(roots)) or 1, plat, info.get('bases', 0))]
        if info.get('near'):
            bits.append('비슷한 폴더: ' + ', '.join(info['near'][:3]))
        if info.get('dead'):
            bits.append('살피지 못한 곳: ' + info['dead'][0])
        why = ' · '.join(bits)
        if rule.get('auto') and not ambiguous:
            # 틀로 만든 계정(대상 목록의 모든 대상) — 아직 한 번도 받지 않은 대상이 흔하다. 문제가 아니라 '받은 것 없음' 으로 센다.
            # 후보가 여럿이면 문제로 센다. 비슷한 폴더가 있으면 알린다 — 이름 규칙이 어긋난 것일 수 있다(r56: 블루스카이 · 디스코드)
            res['missing'] = 1
            note(why)
            if info.get('near'):   # 대상이 수백이면 '받은 것 없음' 마다 적으면 로그가 묻힌다 — 손볼 것이 있을 때만
                log('규칙 %d (%s): 받은 것 없음 — %s' % (i + 1, label, why), 'warning')
            return res
        if not ambiguous:
            log('규칙 %d (%s): %s' % (i + 1, label, why))
        return fail('받은 곳을 찾지 못했습니다 — 정리하기 탭에서 받은 곳을 직접 고르십시오', 'warning')
    if info.get('bm_unknown'):
        _bm_msg = ('북마크 엑셀이 없어 r56 전 작가 폴더에 받은 북마크 %d곳은 고르지 못했습니다'
                   '(이 대상을 북마크로 한 번 다시 받으면 엑셀이 생겨 다음 정리부터 함께 옮깁니다)' % info['bm_unknown'])
        log('규칙 %d (%s): %s' % (i + 1, label, _bm_msg), 'warning')
        note(_bm_msg)
    for s in sources + [b for b, _f in old_bm]:
        if overlap(s, dest):
            return fail('받은 곳과 정리할 곳이 겹칩니다(한쪽이 다른 쪽 안) — 따로 떨어진 곳을 고르십시오')
    for dead in info.get('dead') or []:   # 까닭 글에 그 경로가 들어 있다 — 뿌리마다 한 번
        if dead not in _DEAD_LOGGED:
            _DEAD_LOGGED.add(dead)
            log('규칙 %d (%s): 살피지 못한 저장 경로 — %s (이번 정리에서 이 경로는 다시 알리지 않습니다)' % (i + 1, label, dead), 'warning')
    if old_bm:
        log('규칙 %d (%s): r56 전에 작가 폴더에 받은 북마크 파일 %d개를 함께 정리합니다' % (i + 1, label, sum(len(f) for _b, f in old_bm)))
    handle = (rule.get('target') or '').strip().lstrip('@')
    walk_handle = bluesky_short(handle) if plat == 'bluesky' else handle
    name = pick_name(rule, dest, sources)
    folder = os.path.join(dest, account_folder(rule, name))
    # 그 계정이 이름을 바꿨으면 옛 계정 폴더의 이름만 바꾼다 — 새 폴더를 만들어 전부를 다시 복사하지 않는다
    old, _oldname = existing_account_folder(rule, dest)
    if old and not dry and os.path.join(dest, old) != folder:
        same = unicodedata.normalize('NFC', os.path.basename(old)).casefold() == unicodedata.normalize('NFC', os.path.basename(folder)).casefold()
        shared = [o for o in (folder_owners(os.path.join(dest, old)) or []) if o != owner_key(rule)]
        if shared:
            # 다른 규칙과 함께 쓰는 폴더 — 이름을 바꾸면 그 규칙의 폴더도 바뀐다. 옛 폴더에 그대로 둔다
            log('규칙 %d (%s): 이름이 바뀌었지만 다른 규칙(%s)과 함께 쓰는 폴더라 옛 이름 그대로 둡니다' % (i + 1, label, ', '.join(shared[:3])), 'warning')
            folder = os.path.join(dest, old)
        elif not os.path.exists(folder) or (same and os.path.basename(old) != os.path.basename(folder)):
            # 대소문자만 바뀐 이름도 바꾼다 — 대소문자를 가리지 않는 디스크(APFS · SMB)에선 새 이름이 '이미 있음' 으로 보인다
            try:
                os.rename(os.path.join(dest, old), folder)
                log('규칙 %d (%s): 이름이 바뀌어 폴더 이름을 바꿨습니다 — %s → %s' % (i + 1, label, os.path.basename(old), os.path.basename(folder)))
            except OSError as ex:
                log('규칙 %d (%s): 폴더 이름을 바꾸지 못해 옛 이름으로 둡니다 — %s' % (i + 1, label, ex.strerror or ex), 'warning')
                folder = os.path.join(dest, old)
        elif not same:
            log('규칙 %d (%s): 옛 이름의 폴더(%s)와 지금 이름의 폴더가 둘 다 있습니다 — 지금 이름 쪽에 둡니다' % (i + 1, label, old), 'warning')
    emit('resolved', index=i, id=res['id'], source=' · '.join(sources + [b for b, _f in old_bm][:3]), name=name, folder=folder)
    errors: list = []
    stats: dict = {}

    def groups():
        for s in sources:
            yield s, walk_files(s, captures, walk_handle, errors, plat, stats)
        for b, files in old_bm:
            yield b, iter(files)
    if dry:
        n = sum(1 for _b, it in groups() for _ in it)
        log('규칙 %d: (시험) 파일 %d개 → %s' % (i + 1, n, folder))
        return res
    try:
        os.makedirs(folder, exist_ok=True)
        remember_name(rule, dest, name)
        lockf = open(os.path.join(folder, LOCK_NAME), 'a')
    except OSError as ex:
        if _cant_write(ex):
            return fail('%s(읽기 전용 디스크 · 권한 없음) — %s' % (CANT_WRITE, ex.strerror or ex))
        raise
    try:
        try:
            fcntl.flock(lockf, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except OSError as ex:
            # ★ 잠금이 '이미 잡혀 있음' 일 때만 다른 정리가 쓰는 것이다. 잠금을 못 거는 파일 시스템(exFAT · 일부 SMB/NAS — ENOTSUP ·
            #   ENOLCK)까지 그렇게 알려 그 디스크에는 영영 정리가 안 됐다(검토 ORG-4). 그런 곳은 알리고 잠금 없이 한다 — 앱은 정리를
            #   한 번에 하나만 돌린다(organizeRun 의 busy).
            if ex.errno in (errno.EWOULDBLOCK, errno.EAGAIN):
                return fail('다른 정리가 같은 폴더를 쓰고 있습니다 — 건너뜁니다', 'warning')
            if ex.errno in (errno.ENOTSUP, errno.EOPNOTSUPP, errno.ENOLCK):
                log('규칙 %d (%s): 이 디스크는 잠금을 지원하지 않아 잠금 없이 정리합니다(같은 폴더에 다른 정리를 함께 돌리지 마십시오)' % (i + 1, label), 'warning')
            elif _cant_write(ex):
                return fail('%s(읽기 전용 디스크 · 권한 없음) — %s' % (CANT_WRITE, folder))
            else:
                return fail('정리 폴더를 잠그지 못했습니다 — %s' % (ex.strerror or ex))
        idx = Index(folder)
        idx.seen = set()
        me = owner_key(rule)
        if me and me not in idx.owners:
            idx.owners.append(me)
            idx.save()
        counter = [0]
        n = 0
        for src, files in groups():
            for path, st in files:
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
                        month_dir = os.path.join(folder, month_folder(rule, name, d))
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
                    note('%s — %s' % (os.path.basename(path), ex.strerror or ex))
                    if res['errors'] <= 5:
                        log('규칙 %d: %s — %s' % (i + 1, os.path.basename(path), ex.strerror or ex), 'warning')
                if n % 50 == 0:
                    emit('progress', index=i, id=res['id'], seen=n, copied=res['copied'], dup=res['dup'])
                idx.maybe_save()
        res['dup'] += stats.get('mirror', 0)   # _complete 사본(같은 이름의 원본이 있어 넘긴 것)도 '중복' 으로 센다
        idx.save(prune=not STOP.is_set(), bases=sources + [b for b, _f in old_bm])
    finally:
        lockf.close()
    for e in errors[:5]:
        log('규칙 %d: 읽을 수 없는 곳 — %s' % (i + 1, e), 'warning')
    if errors:
        note('읽을 수 없는 곳 — ' + errors[0])
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
    global TEMPLATE
    TEMPLATE = (cfg.get('template') or '').strip() or DEFAULT_TEMPLATE
    bad = template_problem(TEMPLATE)
    if bad:
        log('틀이 맞지 않습니다 — ' + bad + ' (지금: ' + TEMPLATE + ')', 'error')
        emit('done', stopped=False, failed=True, rules=0, copied=0, clone=0, dup=0, mtime_date=0, errors=1)
        return 0
    rules = [r for r in (cfg.get('rules') or []) if isinstance(r, dict)]
    captures = bool(cfg.get('captures') or a.captures)
    dry = bool(cfg.get('dry_run') or a.dry_run)
    tot = {'copied': 0, 'clone': 0, 'dup': 0, 'mtime_date': 0, 'errors': 0, 'missing': 0}
    for i, rule in enumerate(rules):
        if STOP.is_set():
            break
        try:
            r = run_rule(i, rule, captures, dry)
        except OSError as ex:   # 한 규칙의 문제(권한 · 디스크)가 나머지 규칙을 막지 않게
            why = (CANT_WRITE + ' — ' if _cant_write(ex) else '') + str(ex)
            log('규칙 %d: %s' % (i + 1, why), 'error')
            r = {'index': i, 'id': rule.get('id') or '', 'copied': 0, 'clone': 0, 'dup': 0, 'mtime_date': 0, 'errors': 1, 'missing': 0,
                 'why': why}
        emit('rule_done', **r)
        for k in tot:
            tot[k] += r[k]
    if tot['missing']:
        # ★ '받은 것 없음' 의 까닭(살핀 폴더 수 · 비슷한 폴더)은 계정마다 남기되 로그엔 정리 한 번에 한 줄 — 대상이 수백이면 계정마다
        #   적은 줄에 로그가 묻힌다. 비슷한 폴더가 있는 계정(손볼 것)은 위에서 따로 경고했다.
        log("'받은 것 없음' 의 까닭(계정마다 살핀 폴더 수 · 비슷한 폴더)은 정리하기 탭의 표에서 그 칸에 마우스를 올리면 보입니다")
    emit('done', stopped=STOP.is_set(), rules=len(rules), **tot)
    return 0


if __name__ == '__main__':
    sys.exit(main())
