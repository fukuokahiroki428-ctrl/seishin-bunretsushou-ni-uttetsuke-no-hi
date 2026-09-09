#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""exif_tags.py — JPEG 에 EXIF 태그 몇 개를 쓰는 얇은 계층.

왜 만들었나
-----------
piexif 를 쓰고 있었는데 최신 배포가 2019-07 이다. 7년째 새 판이 없다.
그런데 우리가 쓰는 것은 태그 여섯 개뿐이었다:
  Artist / ImageDescription / Copyright / DateTime / DateTimeOriginal / UserComment

EXIF 는 바깥 서비스가 아니라 1998년에 굳은 파일 규격이다. yt-dlp 처럼 남을
따라다녀야 하는 물건이 아니다. 여섯 개를 위해 방치된 꾸러미를 들고 있느니
직접 쓰는 편이 낫다 — 앞으로 바뀔 것이 없으므로 이 파일도 낡지 않는다.

무엇을 하나
-----------
· 있던 EXIF 는 최대한 살린다. 우리가 건드리는 태그만 갈아 끼운다.
  (새로 만들어 덮으면 카메라가 남긴 정보가 사라진다)
· APP1 이 없으면 새로 만든다.
· 바이트 순서는 읽을 때 원본을 따르고, 새로 만들 때는 little-endian 을 쓴다.

정직한 한계
-----------
· JPEG 만 다룬다. PNG·WebP 는 대상이 아니다(호출부도 JPEG 에만 쓴다).
· 우리가 쓰는 여섯 개와, 원래 있던 값을 보존하는 것까지만 한다.
  EXIF 전체를 다루는 물건을 만들려는 게 아니다 — 그러면 '두 번째 piexif' 가 된다.
"""

import struct

# ── 태그 번호 (EXIF 규격) ──────────────────────────────────────────────
IMAGE_DESCRIPTION = 0x010E
ARTIST            = 0x013B
COPYRIGHT         = 0x8298
DATETIME          = 0x0132
EXIF_IFD_POINTER  = 0x8769
DATETIME_ORIGINAL = 0x9003
USER_COMMENT      = 0x9286

TYPE_ASCII = 2
TYPE_UNDEF = 7


def _u(b, off, n, big):
    return int.from_bytes(b[off:off + n], "big" if big else "little")


def _read_ifd(buf, base, off, big):
    """IFD 하나를 읽어 {tag: (type, raw_bytes)} 로 돌려준다. 다음 IFD 위치도."""
    out = {}
    if off + 2 > len(buf):
        return out, 0
    count = _u(buf, off, 2, big)
    p = off + 2
    for _ in range(count):
        if p + 12 > len(buf):
            break
        tag  = _u(buf, p, 2, big)
        typ  = _u(buf, p + 2, 2, big)
        num  = _u(buf, p + 4, 4, big)
        size = {1:1,2:1,3:2,4:4,5:8,7:1,9:4,10:8}.get(typ, 1) * num
        if size <= 4:
            raw = buf[p + 8:p + 8 + size]
        else:
            vo = _u(buf, p + 8, 4, big)
            raw = buf[vo:vo + size] if vo + size <= len(buf) else b""
        out[tag] = (typ, raw)
        p += 12
    nxt = _u(buf, p, 4, big) if p + 4 <= len(buf) else 0
    return out, nxt


def _pack_ifd(entries, start_offset, big=False):
    """{tag: (type, bytes)} → (IFD 바이트, 뒤에 붙일 값 바이트).
    start_offset 은 TIFF 헤더 기준으로 이 IFD 가 놓이는 자리다."""
    e = ">" if big else "<"
    tags = sorted(entries)                       # 규격상 태그 번호 오름차순
    n = len(tags)
    # IFD 크기 = 2(개수) + 12*n + 4(다음 IFD)
    values_at = start_offset + 2 + 12 * n + 4
    body, values = b"", b""
    # 형식별 한 개의 크기 — 개수는 '바이트 수 / 한 개 크기' 다.
    #   ★ 처음엔 전부 num=len(raw) 로 썼다. ASCII·UNDEFINED 는 그게 맞지만
    #     LONG(4바이트)은 아니다. ExifIFD 포인터가 '개수 4' 로 적혀
    #     exiftool 이 "Bad value for ExifOffset" 을 냈다.
    UNIT = {1:1, 2:1, 3:2, 4:4, 5:8, 7:1, 9:4, 10:8}
    for t in tags:
        typ, raw = entries[t]
        num = len(raw) // UNIT.get(typ, 1)
        if len(raw) <= 4:
            field = raw + b"\x00" * (4 - len(raw))
        else:
            field = struct.pack(e + "I", values_at + len(values))
            values += raw
            if len(raw) % 2:                     # 짝수 정렬
                values += b"\x00"
        body += struct.pack(e + "HHI", t, typ, num) + field
    return struct.pack(e + "H", n) + body + struct.pack(e + "I", 0), values


def _ascii(s):
    if isinstance(s, bytes):
        b = s
    else:
        b = str(s).encode("utf-8", "replace")
    return b if b.endswith(b"\x00") else b + b"\x00"


def _user_comment(s):
    """UserComment 는 앞 8바이트가 문자 코드 표시다. 유니코드는 UNICODE\\0 + UTF-16."""
    if isinstance(s, bytes):
        return s
    return b"UNICODE\x00" + str(s).encode("utf-16-be")


def build_exif(existing_app1=None, *, artist=None, description=None,
               copyright=None, datetime=None, datetime_original=None,
               user_comment=None):
    """APP1(Exif) 세그먼트 '본문' 을 만든다(앞의 'Exif\\0\\0' 포함)."""
    ifd0, exififd, big = {}, {}, False

    # 있던 것을 살린다
    if existing_app1 and existing_app1[:6] == b"Exif\x00\x00":
        tiff = existing_app1[6:]
        if len(tiff) >= 8 and tiff[:2] in (b"II", b"MM"):
            big = (tiff[:2] == b"MM")
            first = _u(tiff, 4, 4, big)
            ifd0, _ = _read_ifd(tiff, 0, first, big)
            ptr = ifd0.pop(EXIF_IFD_POINTER, None)
            if ptr:
                po = int.from_bytes(ptr[1], "big" if big else "little")
                exififd, _ = _read_ifd(tiff, 0, po, big)

    # 우리 것으로 갈아 끼운다
    if description       is not None: ifd0[IMAGE_DESCRIPTION] = (TYPE_ASCII, _ascii(description))
    if artist            is not None: ifd0[ARTIST]            = (TYPE_ASCII, _ascii(artist))
    if copyright         is not None: ifd0[COPYRIGHT]         = (TYPE_ASCII, _ascii(copyright))
    if datetime          is not None: ifd0[DATETIME]          = (TYPE_ASCII, _ascii(datetime))
    if datetime_original is not None: exififd[DATETIME_ORIGINAL] = (TYPE_ASCII, _ascii(datetime_original))
    if user_comment      is not None: exififd[USER_COMMENT]      = (TYPE_UNDEF, _user_comment(user_comment))

    e = ">" if big else "<"
    # 새로 쓸 때는 little-endian 으로 통일 — 읽기는 원본을 따랐지만 쓰기는 하나로.
    if not existing_app1:
        big, e = False, "<"

    # ExifIFD 를 먼저 배치하고, IFD0 에 그 자리를 가리키는 값을 넣는다.
    head = struct.pack(e + "2sHI", b"MM" if big else b"II", 42, 8)
    if exififd:
        # IFD0 크기를 알아야 ExifIFD 자리가 정해진다 — 포인터를 넣은 상태로 크기를 잰다.
        ifd0[EXIF_IFD_POINTER] = (4, struct.pack(e + "I", 0))
        probe, probe_vals = _pack_ifd(ifd0, 8, big)
        exif_at = 8 + len(probe) + len(probe_vals)
        ifd0[EXIF_IFD_POINTER] = (4, struct.pack(e + "I", exif_at))
        b0, v0 = _pack_ifd(ifd0, 8, big)
        b1, v1 = _pack_ifd(exififd, exif_at, big)
        body = b0 + v0 + b1 + v1
    else:
        b0, v0 = _pack_ifd(ifd0, 8, big)
        body = b0 + v0
    return b"Exif\x00\x00" + head + body


def set_tags(jpeg: bytes, **kw) -> bytes:
    """JPEG 바이트에 태그를 써서 새 JPEG 바이트를 돌려준다."""
    if jpeg[:2] != b"\xff\xd8":
        raise ValueError("JPEG 가 아닙니다")

    out = bytearray(b"\xff\xd8")
    i = 2
    existing = None
    segs = []
    while i < len(jpeg) - 1:
        if jpeg[i] != 0xFF:
            break
        marker = jpeg[i + 1]
        if marker == 0xDA:                       # 이미지 데이터 시작 — 여기부터는 그대로
            break
        if marker in (0xD8, 0xD9):
            i += 2
            continue
        seglen = struct.unpack(">H", jpeg[i + 2:i + 4])[0]
        seg = jpeg[i + 4:i + 2 + seglen]
        if marker == 0xE1 and seg[:6] == b"Exif\x00\x00":
            existing = seg                       # 기존 EXIF — 살려서 합친다
        else:
            segs.append((marker, seg))
        i += 2 + seglen

    app1 = build_exif(existing, **kw)
    out += b"\xff\xe1" + struct.pack(">H", len(app1) + 2) + app1
    for marker, seg in segs:
        out += bytes([0xFF, marker]) + struct.pack(">H", len(seg) + 2) + seg
    out += jpeg[i:]                              # SOS 이후 전부
    return bytes(out)
