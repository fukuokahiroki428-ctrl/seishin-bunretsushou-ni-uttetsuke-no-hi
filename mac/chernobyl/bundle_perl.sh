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

# ★ 여기서부터는 실패하면 반드시 되돌린다.
#   set -e 로 중간에 나가면 반쯤 만들어진 tools/perl 이 번들에 남고,
#   부르는 쪽(build.sh)은 경고 한 줄만 내고 그대로 서명해서 내보낸다.
#   잘린 perl 도 '파일로는 존재' 하므로 앱이 그것을 골라 EXIF 가 통째로 죽는다.
trap 'rm -rf "$DEST"; echo "⚠ [perl] 중간에 실패해 넣던 것을 되돌렸습니다."' ERR

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

# ── ★ 가장 중요한 한 걸음: 시스템 libperl 에 매달린 줄을 끊는다 ──────────
#   perl 실행 파일은 /System/Library/Perl/<판>/<arch>/CORE/libperl.dylib 를
#   '절대경로' 로 적재한다. Apple 이 perl 을 걷어내면 그 dylib 도 같이 사라지고,
#   그러면 번들 perl 조차 못 뜬다 — 들고 다니는 목적이 그 자리에서 무너진다.
#   (실측으로 확인하고 고친 것이다. 이 줄이 없으면 이 스크립트 전체가 헛수고다.)
LIBPERL_OLD="$("$SRC_PERL" -MConfig -e 'print "$Config{archlibexp}/CORE/$Config{libperl}"' 2>/dev/null)"
LIBPERL_NEW="@executable_path/../lib/$ARCHNAME/CORE/$(basename "${LIBPERL_OLD:-libperl.dylib}")"
if [ -n "$LIBPERL_OLD" ] && [ -f "$DEST/lib/$ARCHNAME/CORE/$(basename "$LIBPERL_OLD")" ]; then
    install_name_tool -change "$LIBPERL_OLD" "$LIBPERL_NEW" "$DEST/bin/perl" 2>/dev/null
    # dylib 자신의 이름표도 번들 것으로 — 남이 이걸 링크할 때 시스템을 가리키지 않게.
    install_name_tool -id "$LIBPERL_NEW" "$DEST/lib/$ARCHNAME/CORE/$(basename "$LIBPERL_OLD")" 2>/dev/null
    if otool -L "$DEST/bin/perl" 2>/dev/null | grep -q "^	/System/Library/Perl"; then
        echo "⚠ [perl] 시스템 libperl 의존을 끊지 못했습니다 — 넣은 것을 되돌립니다."
        rm -rf "$DEST"; trap - ERR; exit 0
    fi
    # ★ install_name_tool 은 서명을 무효로 만든다. 무효인 채로 두면 커널이 실행을
    #   거부해 바로 아래 자립 확인이 '빈 출력' 으로 실패한다(실제로 겪었다).
    #   여기서는 ad-hoc 으로 임시 서명만 해 둔다 — 빌드 마지막의 codesign_app.sh 가
    #   번들 전체를 제대로 다시 서명한다.
    codesign -f -s - "$DEST/lib/$ARCHNAME/CORE/$(basename "$LIBPERL_OLD")" 2>/dev/null || true
    codesign -f -s - "$DEST/bin/perl" 2>/dev/null || true
    echo "  적재 경로: $LIBPERL_NEW"
else
    echo "⚠ [perl] libperl 을 찾지 못했습니다 — 넣은 것을 되돌립니다."
    rm -rf "$DEST"; trap - ERR; exit 0
fi

# ── 넣었으면 '진짜 되는지' 본다 ───────────────────────────────────────────
#   시스템 @INC 를 걷어낸 채로 exiftool 을 적재해 본다. 여기서 시스템 것을 하나라도
#   쓰면 이 번들은 자립하지 못한 것이고, 그러면 넣으나 마나다.
EXIFTOOL="$APP/Contents/Resources/tools/exiftool/exiftool"
EXIFLIB="$APP/Contents/Resources/tools/exiftool/lib"
if [ -f "$EXIFTOOL" ]; then
    # ★ -I 를 배열로 담는다. 예전엔 $ARCHDIR 를 따옴표 없이 넘겨서, 경로에 공백이
    #   있으면(이 저장소 경로가 그렇다) 두 인자로 쪼개져 시험이 거짓 실패하고
    #   멀쩡한 번들을 통째로 지웠다.
    INCS=()
    [ -d "$DEST/lib/$ARCHNAME" ] && INCS+=(-I"$DEST/lib/$ARCHNAME")
    INCS+=(-I"$DEST/lib" -I"$EXIFLIB")

    # ★ 적재만 보지 않는다. exiftool 은 태그를 '쓸 때' 모듈을 더 불러온다
    #   (Image::ExifTool::Exif·Writer.pl 등). require 만 보고 OK 라 적으면
    #   반쪽 lib 을 통과시키고, 시스템 perl 이 사라지는 그날 한꺼번에 드러난다.
    #   실제 사용과 같은 순서로 — 32x32 JPEG 을 만들어 태그를 쓰고 되읽는다.
    #   그림은 SelfRepair 의 연막 시험이 쓰는 것과 같은 1x1 JPEG(base64)을 쓴다.
    #   바이트를 손으로 적으면 어딘가에서 반드시 뭉개진다(실제로 한 번 뭉갰다).
    PROBE_DIR="$(mktemp -d)"
    PROBE="$PROBE_DIR/probe.jpg"
    printf '%s' '/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRofHh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAFAABAAAAAAAAAAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAD8AKp//2Q==' \
        | base64 --decode > "$PROBE"

    if OUT=$("$DEST/bin/perl" "${INCS[@]}" -e '
        BEGIN { @INC = grep { $_ !~ m{^/System/|^/Library/|^/Network/} } @INC; }
        my ($tool, $img) = (shift, shift);
        require Image::ExifTool;
        my $et = Image::ExifTool->new;
        $et->SetNewValue("Artist", "자립시험");
        $et->SetNewValue("ImageDescription", "日本語");
        my $n = $et->WriteInfo($img);
        die "태그를 쓰지 못했습니다\n" unless $n;
        my $back = Image::ExifTool->new->ImageInfo($img);
        die "되읽기가 어긋납니다\n" unless ($back->{Artist} // "") eq "자립시험";
        my @sys = grep { $INC{$_} =~ m{^/System/|^/Library/|^/Network/} } keys %INC;
        die "시스템 모듈 " . scalar(@sys) . "개를 아직 씁니다: @sys[0..2]\n" if @sys;
        print "OK ", Image::ExifTool->VERSION, " (쓰기·되읽기 통과 · 모듈 ",
              scalar(keys %INC), "개 · 시스템 0개)\n";
    ' "$EXIFTOOL" "$PROBE" 2>&1); then
        echo "  자립 확인: $OUT"
        rm -rf "$PROBE_DIR"
    else
        echo "⚠ [perl] 번들 perl 로 exiftool 을 적재하지 못했습니다:"
        echo "$OUT" | sed 's/^/     /'
        echo "   넣은 것을 되돌립니다 — 앱은 시스템 perl 로 폴백합니다."
        rm -rf "$DEST" "$PROBE_DIR"; trap - ERR
        exit 0
    fi
else
    echo "  (exiftool 이 아직 번들에 없어 자립 확인은 건너뜁니다)"
fi
trap - ERR

echo "  크기: $(du -sh "$DEST" | cut -f1)"
echo "  아키텍처: $(lipo -archs "$DEST/bin/perl" 2>/dev/null)"
echo "=== perl 번들 완료 ==="
