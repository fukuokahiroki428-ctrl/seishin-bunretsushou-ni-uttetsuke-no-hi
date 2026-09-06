#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════
#  exiftool 을 돌릴 perl 을 앱 안에 넣는다.
#
#  왜 필요한가.
#    exiftool 은 perl 스크립트다. 지금은 시스템 /usr/bin/perl 로 돌고 있는데,
#    Apple 은 맥에 딸려 오는 스크립트 언어 런타임(perl·python2·ruby)을 언젠가
#    걷어낸다고 예고해 두었다. 그날이 오면 이 앱의 EXIF 기록이 통째로 죽는다.
#    보관이 목적인 앱에서 '누가 찍었고 언제 것인지' 를 못 남기는 것은 큰 손실이다.
#
#    앱 코드(Common::exiftoolProgram)는 진작 번들 perl 을 먼저 찾도록 돼 있었다.
#    정작 넣는 사람이 없어서 그 길이 한 번도 쓰이지 않았을 뿐이다. 그걸 채운다.
#
#  왜 저장소에 담지 않고 빌드 때 뜨나.
#    코어 라이브러리까지 55MB 다. git 에 넣으면 clone 이 그만큼 무거워지고,
#    macOS 판이 올라가면 낡은 사본이 남는다. 빌드하는 기계의 것을 그대로 뜬다.
#
#  없으면 어떻게 되나.
#    조용히 넘어가지 않는다. 경고를 내고 앱은 시스템 perl 로 폴백한다 —
#    지금까지와 같은 상태이지 더 나빠지지는 않는다.
# ═══════════════════════════════════════════════════════════════════════════
set -e
APP="${1:?사용: bundle_perl.sh <앱 번들 경로>}"
DEST="$APP/Contents/Resources/tools/perl"

# ── 어느 perl 을 뜰 것인가 ────────────────────────────────────────────────
#   저장소에 직접 넣어 둔 것이 있으면 그것을 우선한다(홈브루 perl 등을 담고 싶을 때).
SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
SRC_PERL=""
if [ -x "$SELF_DIR/resources/tools/perl/bin/perl" ]; then
    SRC_PERL="$SELF_DIR/resources/tools/perl/bin/perl"
    SRC_LIB="$SELF_DIR/resources/tools/perl/lib"
elif [ -x /usr/bin/perl ]; then
    SRC_PERL="/usr/bin/perl"
fi

if [ -z "$SRC_PERL" ]; then
    echo "⚠ [perl] 뜰 perl 을 찾지 못했습니다 — 번들에 넣지 않습니다."
    echo "   앱은 시스템 perl 로 폴백합니다(지금까지와 같음)."
    exit 0
fi

# ── 코어 라이브러리 위치를 perl 에게 직접 묻는다 ──────────────────────────
#   경로를 손으로 적으면 macOS 판이 오를 때마다 틀린다(5.34 → 5.36 …).
if [ -z "${SRC_LIB:-}" ]; then
    SRC_LIB="$("$SRC_PERL" -V:privlib 2>/dev/null | sed "s/^privlib='//;s/';$//")"
fi
PERL_VER="$("$SRC_PERL" -V:version 2>/dev/null | sed "s/^version='//;s/';$//")"
ARCHNAME="$("$SRC_PERL" -V:archname 2>/dev/null | sed "s/^archname='//;s/';$//")"

if [ ! -d "$SRC_LIB" ]; then
    echo "⚠ [perl] 코어 라이브러리를 찾지 못했습니다: $SRC_LIB"
    echo "   번들에 넣지 않습니다 — 앱은 시스템 perl 로 폴백합니다."
    exit 0
fi

echo "=== perl 번들 ($PERL_VER / $ARCHNAME) ==="
echo "  실행 파일: $SRC_PERL"
echo "  코어 lib : $SRC_LIB"

rm -rf "$DEST"
mkdir -p "$DEST/bin" "$DEST/lib"
cp "$SRC_PERL" "$DEST/bin/perl"
chmod +x "$DEST/bin/perl"

# ★ cp -R 로 뜬다. ditto 는 /System/Library/Perl 밑 일부 폴더에서 권한 오류를 낸다(실측).
if ! cp -R "$SRC_LIB"/ "$DEST/lib/" 2>/dev/null; then
    echo "⚠ [perl] 코어 라이브러리 복사에 실패했습니다 — 넣은 것을 되돌립니다."
    rm -rf "$DEST"
    exit 0
fi

# ── 넣었으면 '진짜 되는지' 본다 ───────────────────────────────────────────
#   시스템 @INC 를 걷어낸 채로 exiftool 을 적재해 본다. 여기서 시스템 것을 하나라도
#   쓰면 이 번들은 자립하지 못한 것이고, 그러면 넣으나 마나다.
EXIFTOOL="$APP/Contents/Resources/tools/exiftool/exiftool"
EXIFLIB="$APP/Contents/Resources/tools/exiftool/lib"
if [ -f "$EXIFTOOL" ]; then
    ARCHDIR=""
    [ -d "$DEST/lib/$ARCHNAME" ] && ARCHDIR="-I$DEST/lib/$ARCHNAME"
    if OUT=$("$DEST/bin/perl" $ARCHDIR -I"$DEST/lib" -I"$EXIFLIB" -e '
        BEGIN { @INC = grep { $_ !~ m{^/System/|^/Library/|^/Network/} } @INC; }
        require Image::ExifTool;
        my @sys = grep { $INC{$_} =~ m{^/System/|^/Library/|^/Network/} } keys %INC;
        die "시스템 모듈 " . scalar(@sys) . "개를 아직 씁니다\n" if @sys;
        print "OK ", Image::ExifTool->VERSION, " (모듈 ", scalar(keys %INC), "개, 시스템 0개)\n";
    ' 2>&1); then
        echo "  자립 확인: $OUT"
    else
        echo "⚠ [perl] 번들 perl 로 exiftool 을 적재하지 못했습니다:"
        echo "$OUT" | sed 's/^/     /'
        echo "   넣은 것을 되돌립니다 — 앱은 시스템 perl 로 폴백합니다."
        rm -rf "$DEST"
        exit 0
    fi
else
    echo "  (exiftool 이 아직 번들에 없어 자립 확인은 건너뜁니다)"
fi

echo "  크기: $(du -sh "$DEST" | cut -f1)"
echo "=== perl 번들 완료 ==="
