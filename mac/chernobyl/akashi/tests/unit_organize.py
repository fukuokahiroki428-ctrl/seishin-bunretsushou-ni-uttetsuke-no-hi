#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""정리하기 도우미 단위 시험 — resources/tools/organize.py 를 불러, 수집기마다의 저장 모양을 흉내 낸 가짜 받은 폴더로
받은 곳 찾기 · 정리 · 까닭(why)을 본다(앱 · 네트워크 없음, 몇 초).

    python3 -B akashi/tests/unit_organize.py

★ 왜 수집기마다 따로 만드나 — r55 까지 정리하기 시험(organize.py)은 트위터 모양만 만들어, 블루스카이('.bsky.social' 을 뗀 폴더) ·
  디스코드(서버 이름/채널 이름_ID) · pixiv(이름을 바꾸면 폴더 둘 · 북마크는 작가 폴더)를 못 찾는 것이 드러나지 않았다
  (사용자 2026-11-27: '정리하기가 옮기지 않는다 · 엉뚱한 폴더 · 중복'). 모양은 각 수집기의 저장 코드를 그대로 따른다:
  twitter/<핸들>/media/<종류>/ + media/_complete/ · bluesky/<짧은 핸들>/media/<작가>/<종류>/ + _complete/ + profiles/<날짜>/ +
  excel/<짧은>_profile.xlsx · discord/<서버 이름>/<채널 이름>_<ID>/ + index.json · pixiv/<ID>_<이름>/illusts/ + profiles/ +
  bookmarks/<작가>/illusts/(r56) · pixiv/<작가>/bookmarks/illusts/(r56 전) + pixiv/<ID>_bookmarks_complete.xlsx ·
  fanbox/<크리에이터>/media/ + posts/<글>.json · instagram/<사용자>/posts/ + profiles/<날짜>/ + profile.json.
모든 자료는 지어낸 것이다.
"""
from __future__ import annotations

import ast
import errno
import importlib.util
import json
import os
import shutil
import sys
import time
import zipfile
from pathlib import Path
from xml.sax.saxutils import escape

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
import _common as C  # noqa: E402

CHERNOBYL = HERE.parent.parent
spec = importlib.util.spec_from_file_location('organize_tool', str(CHERNOBYL / 'resources' / 'tools' / 'organize.py'))
org = importlib.util.module_from_spec(spec)
spec.loader.exec_module(org)

EVENTS: list = []
org.emit = lambda ev, **kw: EVENTS.append(dict(kw, ev=ev))   # 줄마다 JSON 을 stdout 에 쓰는 대신 모은다


def mk(p: Path, data: bytes = b'x', when: float = 0) -> Path:
    p.parent.mkdir(parents=True, exist_ok=True)
    p.write_bytes(data)
    if when:
        os.utime(p, (when, when))
    return p


def xlsx(p: Path, rows: list) -> Path:
    """앱(QXlsx · openpyxl)이 쓰는 꼴의 아주 작은 엑셀 — 글은 공유 문자열로."""
    shared, idx = [], {}
    body = []
    for ri, row in enumerate(rows, 1):
        cells = []
        for ci, v in enumerate(row):
            ref = chr(65 + ci) + str(ri)
            v = str(v)
            if v not in idx:
                idx[v] = len(shared)
                shared.append(v)
            cells.append('<c r="%s" t="s"><v>%d</v></c>' % (ref, idx[v]))
        body.append('<row r="%d">%s</row>' % (ri, ''.join(cells)))
    ns = 'http://schemas.openxmlformats.org/spreadsheetml/2006/main'
    p.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(p, 'w') as z:
        z.writestr('xl/worksheets/sheet1.xml', '<worksheet xmlns="%s"><sheetData>%s</sheetData></worksheet>' % (ns, ''.join(body)))
        z.writestr('xl/sharedStrings.xml', '<sst xmlns="%s">%s</sst>' % (ns, ''.join('<si><t>%s</t></si>' % escape(s) for s in shared)))
    return p


def tree(d: Path) -> list:
    return sorted(str(p.relative_to(d)) for p in d.rglob('*') if p.is_file() and not p.name.startswith('.'))


def rel(paths, base: Path) -> list:
    return sorted(os.path.relpath(p, base) for p in paths)


def build(R: Path) -> None:
    t0 = time.mktime((2025, 11, 3, 12, 0, 0, 0, 0, -1))
    # 트위터 — media/<종류>/ + _complete/ 사본(앱이 원본 EXIF 를 다시 써 바이트가 다름) + 원본을 지운 사본 하나 + 남의 프로필
    tw = R / 'twitter' / 'tw_demo'
    mk(tw / 'media' / 'tweets' / '20251103_142000_1.jpg', b'TW1-rewritten')
    mk(tw / 'media' / '_complete' / '20251103_142000_1.jpg', b'TW1-original')
    mk(tw / 'media' / '_complete' / '20251104_000000_only.jpg', b'TW-only-in-complete')
    mk(tw / 'profiles' / 'followers' / 'Stranger(@someone)' / 'profile_20261003.jpg', b'STRANGER')
    # 블루스카이 — 대상 'alice.bsky.social' 의 폴더는 bluesky/alice
    bs = R / 'bluesky' / 'alice'
    mk(bs / 'media' / 'alice' / 'posts' / '20251103_1200_hello (abc).jpg', b'BS-post-exif')
    mk(bs / 'media' / 'alice' / '_complete' / '20251103_1200_hello (abc).jpg', b'BS-post-plain')
    mk(bs / 'profiles' / '2026-11-20' / 'alice_avatar.jpg', b'BS-AVATAR')
    mk(bs / 'profiles' / '2026-11-21' / 'alice_avatar.jpg', b'BS-AVATAR')            # 날마다 같은 사진 — 한 번만
    mk(bs / 'profiles' / '2026-11-20' / 'bob.bsky.social_avatar.jpg', b'BOB-AVATAR')  # 팔로워의 사진 — 빼기
    xlsx(bs / 'excel' / 'alice_profile.xlsx', [['Item', 'Value'], ['Handle', 'alice.bsky.social'], ['Display Name', 'Alice ★']])
    # 디스코드 — discord/<서버 이름>/<채널 이름>_<채널 ID>/ · 서버 이름을 못 얻은 판은 discord/<서버 ID>/
    ds = R / 'discord' / 'My Server'
    mk(ds / 'general_222' / 'messages' / '20251103_120000_x.jpg', b'DC-general')
    mk(ds / 'general_222' / 'profiles' / 'someone' / '2026-11-20' / 'avatar.png', b'DC-OTHER-AVATAR')
    (ds / 'general_222' / 'index.json').write_text(json.dumps({'channel_id': '222', 'channel_name': 'general', 'guild_id': '111'}))
    mk(ds / 'art_333' / 'messages' / '20251104_120000_y.png', b'DC-art')
    (ds / 'art_333' / 'index.json').write_text(json.dumps({'channel_id': '333', 'channel_name': 'art', 'guild_id': '',
                                                          'channel_info': {'guild_id': '111'}}))
    mk(R / 'discord' / '999' / 'chan_444' / 'messages' / '20251105_000000_z.jpg', b'DC-999')
    # pixiv — 이름을 바꿔 폴더 둘 + 이름을 못 얻은 판의 '<ID>' 폴더 + 새 자리의 북마크 + _complete 사본
    px = R / 'pixiv'
    mk(px / '123_Old' / 'illusts' / '20200101_0000_(Old)t (1001).jpg', b'PX-1001', t0 - 86400 * 400)
    os.utime(px / '123_Old', (t0 - 86400 * 400,) * 2)
    mk(px / '123_New' / 'illusts' / '20250101_0000_(New)t (1002).jpg', b'PX-1002-exif')
    mk(px / '123_New' / '_complete' / '20250101_0000_(New)t (1002).jpg', b'PX-1002-plain')
    mk(px / '123_New' / 'bookmarks' / '777_Author' / 'illusts' / '20250102_0000_(Author)nb (5003).jpg', b'PX-BM-5003')
    (px / '123_New' / 'profiles').mkdir(parents=True)
    (px / '123_New' / 'profiles' / 'profile.json').write_text(json.dumps({'user_id': '123', 'user_name': 'New／Name'}), encoding='utf-8')
    mk(px / '123' / 'profiles' / 'avatar_20261101.jpg', b'PX-AVATAR')
    # r56 전의 북마크 — 작가 폴더에. 대상 123 의 북마크 엑셀에 5001 만 있다(5002 는 다른 사람의 북마크)
    mk(px / '777_Author' / 'bookmarks' / 'illusts' / '20240101_0000_(Author)bm (5001_0).png', b'PX-BM-5001')
    mk(px / '777_Author' / 'bookmarks' / 'illusts' / '20240101_0000_(Author)other (5002).png', b'PX-BM-5002')
    mk(px / '888_X' / 'bookmarks' / 'illusts' / '20240101_0000_(X)x (5001).png', b'PX-BM-888')   # 엑셀의 작가가 아님 — 빼기
    xlsx(px / '123_bookmarks_complete.xlsx', [['ID', 'Title', 'Author', 'Author ID'], ['5001', 'bm', 'Author', '777']])
    mk(px / '456_Artist' / 'illusts' / '20250101_0000_(Artist)a (2001).jpg', b'PX-2001')
    # fanbox · instagram
    mk(R / 'fanbox' / 'creator' / 'media' / '20251103_120000_f.jpg', b'FB-1')
    mk(R / 'fanbox' / 'creator' / 'posts' / '1.json', json.dumps({'user': {'name': 'Creator Name'}}).encode())
    ig = R / 'instagram' / 'insta_user'
    mk(ig / 'posts' / '20251103_120000_i.jpg', b'IG-1')
    mk(ig / 'profiles' / '2026-11-20' / 'avatar_20261120.jpg', b'IG-AVATAR')
    (ig / 'profile.json').write_text(json.dumps({'username': 'insta_user', 'full_name': 'Insta Name'}), encoding='utf-8')


def rule(plat, target, R, D, **kw):
    r = {'id': 't:%s:%s' % (plat, target.lower()), 'platform': plat, 'target': target, 'name': '', 'source': '',
         'dest': str(D), 'auto': True, 'roots': [str(R)], 'primary': str(R)}
    r.update(kw)
    return r


def run(r, i=0):
    EVENTS.clear()
    return org.run_rule(i, r, False, False)


def logs(level=None):
    return [e['msg'] for e in EVENTS if e.get('ev') == 'log' and (level is None or e.get('level') == level)]


def main() -> int:
    check = C.Checks()
    work = C.work_dir('unit_organize')
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    R, D = work / 'dl', work / 'out'
    D.mkdir()
    build(R)
    org._ROOT_STATE.clear()

    # ── 받은 곳 찾기
    fs = lambda plat, t: rel(org.find_sources({'platform': plat, 'target': t, 'roots': [str(R)]})[0], R)
    check("블루스카이 — 'alice.bsky.social' 은 수집기가 뗀 이름 bluesky/alice 에서", fs('bluesky', 'alice.bsky.social') == ['bluesky/alice'],
          fs('bluesky', 'alice.bsky.social'))
    # 폴더 이름 규칙이 데몬의 norm_handle 과 같은가 — 데몬 함수 안의 그 함수를 소스에서 떼어 맞대어 본다(규칙이 갈라지면 여기서 걸린다)
    dsrc = (CHERNOBYL / 'resources' / 'tools' / 'bluesky_daemon.py').read_text(encoding='utf-8')
    fn = next(n for n in ast.walk(ast.parse(dsrc)) if isinstance(n, ast.FunctionDef) and n.name == 'norm_handle')
    ns: dict = {}
    exec(compile(ast.Module(body=[fn], type_ignores=[]), 'norm_handle', 'exec'), ns)
    hs = ['alice.bsky.social', '@alice.bsky.social ', 'Alice.Bsky.Social', 'x.bsky.social.example.com', 'did:plc:abc', 'carol']
    diff = [(h, ns['norm_handle'](h)[0], org.bluesky_short(h)) for h in hs if ns['norm_handle'](h)[0] != org.bluesky_short(h)]
    check('bluesky_short 는 데몬 norm_handle 의 폴더 이름과 같음', not diff, diff)
    check('블루스카이 — 대소문자가 달라도(수집기는 replace 로 떼지만 디스크는 대소문자 무시)', fs('bluesky', 'Alice.BSKY.social') == ['bluesky/alice'])
    check("디스코드 — '서버/채널' 은 끝이 _<채널 ID> 인 채널 폴더", fs('discord', '111/222') == ['discord/My Server/general_222'], fs('discord', '111/222'))
    check("디스코드 — '/채널'(서버 칸을 비운 대상)도", fs('discord', '/222') == ['discord/My Server/general_222'])
    check("디스코드 — '서버' 만이면 index.json 의 서버 ID 가 같은 채널들(guild_id · channel_info.guild_id)",
          fs('discord', '111') == ['discord/My Server/art_333', 'discord/My Server/general_222'], fs('discord', '111'))
    check("디스코드 — 서버 이름을 못 얻은 판의 discord/<서버 ID>/ 아래 채널들", fs('discord', '999') == ['discord/999/chan_444'])
    px = fs('pixiv', '123')
    check("pixiv — 이름을 바꾼 두 폴더 + '<ID>' 폴더를 모두(후보 여럿으로 멈추지 않음 · '<ID>' 가 가리지 않음)",
          px == ['pixiv/123', 'pixiv/123_New', 'pixiv/123_Old'], px)
    srcs, amb = org.find_sources({'platform': 'pixiv', 'target': '123', 'roots': [str(R)]})
    check('pixiv — 후보 여럿(ambiguous) 아님', amb is None, amb)
    check('fanbox · instagram — 같은 이름(대소문자 무시)', fs('fanbox', 'creator') == ['fanbox/creator'] and fs('instagram', 'Insta_User') == ['instagram/insta_user'])

    # ── 정리 — 수집기마다
    r = run(rule('twitter', 'tw_demo', R, D))
    acc = 'tw_demo・@tw_demo-(X ／ twitter.com)'
    got = tree(D / acc)
    check("트위터 — _complete 사본은 원본이 있으면 넘기고(EXIF 를 다시 쓴 원본과 바이트가 달라도), 원본을 지운 사본만 · 남의 프로필 빼기",
          got == ['令和7年11月/20251103_142000_1.jpg', '令和7年11月/20251104_000000_only.jpg'] and r['copied'] == 2 and r['dup'] == 1, (got, r))

    r = run(rule('bluesky', 'alice.bsky.social', R, D))
    acc = 'Alice ★・@alice.bsky.social-(Bluesky ／ bsky.app)'
    got = tree(D / acc) if (D / acc).is_dir() else sorted(os.listdir(D))
    check("블루스카이 — 이름은 엑셀의 'Display Name' · 글 하나 + 대상 사진 하나(날마다 같은 사진은 한 번 · 팔로워 사진 빼기) · 사본 넘김",
          len(got) == 2 and any(g.endswith('alice_avatar.jpg') for g in got) and any('hello' in g for g in got)
          and r['copied'] == 2 and r['dup'] == 2 and r['errors'] == 0 and r['missing'] == 0, (got, r))

    r = run(rule('discord', '111/222', R, D))
    dacc = [n for n in os.listdir(D) if 'Discord' in n]
    check("디스코드 — 채널 대상 정리(이름 '서버 #채널') · 채널 사람들의 profiles/ 는 빼기",
          r['copied'] == 1 and dacc == ['My Server #general・@111／222-(Discord ／ discord.com)'], (r, dacc))

    r = run(rule('pixiv', '123', R, D))
    pacc = D / 'New／Name・@123-(pixiv ／ pixiv.net)'
    got = tree(pacc) if pacc.is_dir() else sorted(os.listdir(D))
    want = sorted(['令和2年1月/20200101_0000_(Old)t (1001).jpg', '令和7年1月/20250101_0000_(New)t (1002).jpg',
                   '令和7年1月/20250102_0000_(Author)nb (5003).jpg', '令和6年1月/20240101_0000_(Author)bm (5001_0).png'])
    got_media = sorted(g for g in got if 'avatar_' not in g)
    check("pixiv — 두 이름 폴더 · 새 자리 북마크 · r56 전 북마크(엑셀의 5001 만, 다른 사람 것 · 엑셀에 없는 작가 빼기) · 이름은 profile.json",
          got_media == want and any('avatar_20261101' in g for g in got) and r['errors'] == 0, (got, r))
    check('pixiv — r56 전 북마크를 함께 정리한다고 알림', any('북마크 파일 1개' in m for m in logs()), logs())

    r = run(rule('pixiv', '456', R, D))
    check("pixiv — profile.json 이 없으면 폴더 '<ID>_<이름>' 의 이름", (D / 'Artist・@456-(pixiv ／ pixiv.net)').is_dir(), sorted(os.listdir(D)))
    r = run(rule('fanbox', 'creator', R, D))
    check("fanbox — 이름은 가장 최근 글 json 의 user.name", (D / 'Creator Name・@creator-(pixivFANBOX ／ fanbox.cc)').is_dir() and r['copied'] == 1, r)
    r = run(rule('instagram', 'insta_user', R, D))
    check("instagram — profiles/<날짜>/ 의 대상 사진도 · 이름은 profile.json 의 full_name",
          r['copied'] == 2 and (D / 'Insta Name・@insta_user-(Instagram ／ instagram.com)').is_dir(), r)

    # ── 다시 돌려도 새로 0 (같은 내용은 어디에도 두 번 두지 않는다)
    before = tree(D)
    again = [run(rule(p, t, R, D)) for p, t in (('twitter', 'tw_demo'), ('bluesky', 'alice.bsky.social'), ('discord', '111/222'),
                                                  ('pixiv', '123'), ('fanbox', 'creator'), ('instagram', 'insta_user'))]
    check('다시 정리하면 새로 0 · 파일 수 그대로', all(x['copied'] == 0 for x in again) and tree(D) == before, [x['copied'] for x in again])

    # ── '(n)' 이름 — 그 이름에 같은 바이트가 이미 있으면 한 벌 더 두지 않는다
    f = D / 'collide'
    f.mkdir()
    idx = org.Index(str(f))
    src = mk(work / 'c' / '20251103_000000_same.jpg', b'SAME-BYTES')
    month = f / 'm'
    month.mkdir()
    mk(month / '20251103_000000_same.jpg', b'SAME-BYTES')        # 장부(Index)가 모르는 같은 파일
    kind, _h, _how = org.place(idx, str(src), os.stat(src), str(month), [0])
    check("'(n)' 으로 같은 바이트를 또 두지 않는다", kind == 'dup' and sorted(os.listdir(month)) == ['20251103_000000_same.jpg'], (kind, os.listdir(month)))
    mk(work / 'c2' / '20251103_000000_same.jpg', b'OTHER-BYTES')
    kind, _h, _how = org.place(idx, str(work / 'c2' / '20251103_000000_same.jpg'), os.stat(work / 'c2' / '20251103_000000_same.jpg'), str(month), [1])
    check("이름만 같고 내용이 다르면 '(2)'", kind == 'copied' and (month / '20251103_000000_same (2).jpg').exists(), os.listdir(month))

    # ── 까닭(why)
    r = run(rule('bluesky', 'alicex', R, D))
    check("받은 것 없음 — 그 탭의 저장 경로가 살아 있을 때만 · 까닭에 살핀 수와 비슷한 폴더 · 손볼 것이라 경고 한 줄",
          r['missing'] == 1 and r['errors'] == 0 and '비슷한 폴더' in r.get('why', '') and 'bluesky/alice' in r.get('why', '')
          and logs('warning'), (r, logs()))
    r = run(rule('twitter', 'never_dl', R, D))
    check("받은 것 없음 — 비슷한 것이 없으면 로그는 조용히(수백 계정) · 까닭은 남김", r['missing'] == 1 and not logs() and '살폈지만' in r.get('why', ''), (r, logs()))
    gone = str(work / 'no-such-root')
    org._ROOT_STATE.clear()
    r = run(rule('twitter', 'never_dl', R, D, roots=[gone, str(R)], primary=gone))
    check("그 탭의 저장 경로가 없으면(보조 경로는 있어도) '받은 것 없음' 이 아니라 문제 · 까닭", r['missing'] == 0 and r['errors'] == 1
          and '저장 경로가 없습니다' in r.get('why', ''), r)
    # 보조 경로가 빠진 디스크면 받은 것이 있는 규칙마다 같은 경고가 나오던 것 — 한 번 정리에 뿌리마다 한 번만(검토 ORG-R2)
    org._ROOT_STATE.clear()
    org._DEAD_LOGGED.clear()
    warned = []
    for k, t in enumerate(('alice.bsky.social', 'alice.bsky.social')):
        run(rule('bluesky', t, R, work / 'out_dead', roots=[str(R), gone]), k)
        warned += [m for m in logs('warning') if '살피지 못한 저장 경로' in m]
    check('떨어진 보조 경로 경고는 정리 한 번에 한 번만', len(warned) == 1 and gone in warned[0], warned)
    org._DEAD_LOGGED.clear()
    duck = work / 'duck' / 'conn'
    duck.mkdir(parents=True)                                   # 끊긴 Mountain Duck 자리 — 빈 폴더
    real_net, real_probe = org._net_mount, org._probe_alive
    org._net_mount = lambda p: ('duck', str(duck)) if str(p).startswith(str(duck)) else (None, None)
    org._probe_alive = lambda mp, timeout=8.0: True
    org._ROOT_STATE.clear()
    r = run(rule('fanbox', 'creator', R, D, roots=[str(duck / 'save')], primary=str(duck / 'save')))
    check("네트워크 저장소가 끊겼으면 '%s'" % org.NOT_ATTACHED, r['errors'] == 1 and org.NOT_ATTACHED in r.get('why', ''), r)
    org._probe_alive = lambda mp, timeout=8.0: False
    mk(duck / 'save' / 'fanbox' / 'creator' / 'media' / '20251103_120000_f.jpg', b'FB-1')
    org._ROOT_STATE.clear()
    r = run(rule('fanbox', 'creator', R, D, roots=[str(duck / 'save')], primary=str(duck / 'save')))
    check('응답 없는 마운트는 들여다보지 않고 문제로(멈추지 않게)', r['errors'] == 1 and org.NOT_ATTACHED in r.get('why', ''), r)
    # 목록만 막힌 Mountain Duck 자리(다른 앱의 컨테이너 보호 — 읽기 없이 들어가기만 됨)는 '연결 안 됨' 이 아니다
    org._probe_alive = lambda mp, timeout=8.0: True
    os.chmod(duck, 0o311)
    try:
        org._ROOT_STATE.clear()
        r = run(rule('fanbox', 'creator', R, work / 'out_duck', roots=[str(duck / 'save')], primary=str(duck / 'save')))
    finally:
        os.chmod(duck, 0o755)
    check("목록만 막힌 네트워크 자리는 붙은 것으로 — '%s' 로 잘못 말하지 않음" % org.NOT_ATTACHED,
          r['copied'] == 1 and org.NOT_ATTACHED not in r.get('why', ''), r)
    org._net_mount, org._probe_alive = real_net, real_probe
    org._ROOT_STATE.clear()

    # 받은 곳 목록을 읽을 권한이 없음(EPERM) — 규칙 밖으로 새지 않고 까닭을 남긴다
    R2 = work / 'dl2'
    mk(R2 / 'fanbox' / 'creator' / 'media' / '20251103_120000_f.jpg', b'FB-2')
    os.chmod(R2 / 'fanbox', 0)
    try:
        r = run(rule('fanbox', 'creator', R2, D))
    except OSError as ex:
        r = {'escaped': str(ex)}
    finally:
        os.chmod(R2 / 'fanbox', 0o755)
    check("받은 곳을 읽을 권한이 없으면 '%s' · 다른 규칙을 막지 않음" % org.DENIED, r.get('errors') == 1 and org.DENIED in r.get('why', ''), r)

    # 잠금(flock) — 이미 잡힘만 '다른 정리', 잠금을 못 거는 디스크는 잠금 없이, 읽기 전용은 '쓸 수 없음'
    real_flock = org.fcntl.flock

    def flock_err(code):
        def f(*_a):
            raise OSError(code, os.strerror(code))
        return f
    try:
        D2 = work / 'out2'
        D2.mkdir()
        org.fcntl.flock = flock_err(errno.ENOTSUP)
        r = run(rule('fanbox', 'creator', R, D2))
        check('잠금을 지원하지 않는 디스크(ENOTSUP)는 알리고 잠금 없이 정리', r['copied'] == 1 and r['errors'] == 0
              and any('잠금 없이' in m for m in logs('warning')), (r, logs()))
        org.fcntl.flock = flock_err(errno.EWOULDBLOCK)
        r = run(rule('fanbox', 'creator', R, D2))
        check("이미 잡힌 잠금(EWOULDBLOCK)만 '다른 정리가 같은 폴더를 쓰고 있습니다'", r['errors'] == 1 and '다른 정리' in r.get('why', ''), r)
        org.fcntl.flock = flock_err(errno.EROFS)
        r = run(rule('fanbox', 'creator', R, D2))
        check("읽기 전용(EROFS)은 '%s'" % org.CANT_WRITE, r['errors'] == 1 and org.CANT_WRITE in r.get('why', ''), r)
    finally:
        org.fcntl.flock = real_flock

    # pixiv — 북마크로만 받은 대상(제 폴더엔 수집기가 북마크 판에도 만드는 profiles/ 뿐 · 북마크 엑셀 없음)이면 작가 폴더의 r56 전
    #   북마크를 고르지 못했다고 알린다(조용히 넘기지 않는다). 제 작품이 있는 대상 · 한 번도 받지 않은 대상에는 붙이지 않는다(검토 COMB-D1)
    R3 = work / 'dl3'
    mk(R3 / 'pixiv' / '777_Author' / 'bookmarks' / 'illusts' / '20240101_0000_(Author)bm (5001).png', b'BM')
    mk(R3 / 'pixiv' / '321_Me' / 'profiles' / 'profile.json', b'{}')
    org._ROOT_STATE.clear()
    r = run(rule('pixiv', '321', R3, D))
    check('pixiv 북마크로만 받은 대상(엑셀 없음) — 작가 폴더의 옛 북마크를 고르지 못했다고 알림',
          r['missing'] == 0 and '북마크 엑셀이 없어' in r.get('why', '') and any('북마크 엑셀이 없어' in m for m in logs('warning')), (r, logs()))
    org._ROOT_STATE.clear()
    r = run(rule('pixiv', '654', R3, D))
    check('한 번도 받지 않은 pixiv 대상에는 옛 북마크 알림을 붙이지 않는다', r['missing'] == 1 and '북마크' not in r.get('why', ''), r)

    # pixiv — r56 전 북마크 작품의 캡처(작가 폴더의 captures/<날짜>_<작품ID>.html)도 '캡처까지' 면 함께(검토 spec_gap).
    #   작가 본인 작품(6001)의 캡처 · 엑셀에 없는 북마크(5002)의 캡처는 빼고, '캡처까지' 가 꺼져 있으면 캡처는 안 가져온다
    R4 = work / 'dl4'
    a4 = R4 / 'pixiv' / '777_Author'
    mk(a4 / 'bookmarks' / 'illusts' / '20240101_0000_(Author)bm (5001).png', b'BM4')
    mk(a4 / 'captures' / '20240101_000000_5001.html', b'<html>5001</html>')
    mk(a4 / 'captures' / '20240101_000000_5002.html', b'<html>5002</html>')
    mk(a4 / 'captures' / '20240101_000000_6001.html', b'<html>6001</html>')
    xlsx(R4 / 'pixiv' / '123_bookmarks_complete.xlsx', [['ID', 'Title', 'Author', 'Author ID'], ['5001', 'bm', 'Author', '777']])
    org._ROOT_STATE.clear()
    org._PX_LEGACY.clear()
    EVENTS.clear()
    r = org.run_rule(0, rule('pixiv', '123', R4, work / 'out4'), True, False)
    got = [g.split('/', 1)[-1] for g in tree(work / 'out4')]
    check("pixiv — r56 전 북마크의 캡처도(엑셀의 작품만)", r['copied'] == 2 and sorted(got) == sorted(
        ['令和6年1月/20240101_0000_(Author)bm (5001).png', '令和6年1月/20240101_000000_5001.html']), (r, got))
    org._ROOT_STATE.clear()
    org._PX_LEGACY.clear()
    r = run(rule('pixiv', '123', R4, work / 'out4b'))
    check("pixiv — '캡처까지' 가 꺼져 있으면 옛 북마크의 캡처는 안 가져옴", r['copied'] == 1, r)

    # 다른 플랫폼은 예전 그대로 — profiles/ 는 profiles/target/<이름>(@핸들) 만(유튜브 등 plat 이 없는 부름)
    tmpw = work / 'plain'
    mk(tmpw / 'profiles' / 'target' / 'Me(@me)' / 'profile_20261003.jpg', b'ME')
    mk(tmpw / 'profiles' / 'followers' / 'X(@x)' / 'profile_20261003.jpg', b'X')
    mk(tmpw / 'media' / '_complete' / 'a.jpg', b'A')
    errs: list = []
    got = sorted(os.path.relpath(p, tmpw) for p, _st in org.walk_files(str(tmpw), False, 'me', errs))
    check('plat 없이 부르면 예전 규칙(_complete 도 그대로 · 남의 프로필 빼기)',
          got == ['media/_complete/a.jpg', 'profiles/target/Me(@me)/profile_20261003.jpg'], got)
    return check.summary()


if __name__ == '__main__':
    sys.exit(main())
