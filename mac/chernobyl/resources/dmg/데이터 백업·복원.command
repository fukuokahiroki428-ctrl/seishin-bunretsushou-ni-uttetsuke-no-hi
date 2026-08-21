#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════
#  ハンイシキ — 데이터 백업 · 복원
#
#  앱이 사용자 폴더에 두는 것들을 다른 디스크로 통째 옮겨 두거나, 되돌립니다.
#
#      ~/Library/Application Support/Miyo/<앱>/
#        miyo_config.json      설정·계정 토큰      ← 가장 중요
#        llm/                  AI 엔진·모델 (9GB 넘을 수 있음)
#        archive_index.db      산출물 색인
#        script_overrides/     AI 가 고친 스크립트
#        chrome_capture_profile*/  캡쳐용 크롬 프로필(로그인 상태)
#        tools/                yt-dlp 자동 갱신본
#
#  ★ 백업한 뒤에는 반드시 '검증' 합니다 — 항목 수와 핵심 파일 해시를 대조합니다.
#    검증을 통과해야만 원본을 지울 수 있게 했습니다. 복사가 덜 됐는데 지워 버리면
#    되돌릴 수가 없기 때문입니다.
#
#  ★ 지울 때도 영구 삭제하지 않습니다 — 휴지통으로 옮깁니다.
# ═══════════════════════════════════════════════════════════════════════════
set -uo pipefail

SELF_DIR="$(cd "$(dirname "$0")" && pwd)"
BASE="$HOME/Library/Application Support/Miyo"

# 한 줄 읽기 — 더블클릭(Terminal)과 파이프 실행 양쪽에서 동작하게.
#   [ -r /dev/tty ] 가 참인데 실제로 열면 실패하는 경우가 있어 '열어 보고' 판단한다.
ask() {
    local __v=""
    if { : </dev/tty; } 2>/dev/null; then read -r __v </dev/tty || __v=""; fi
    [ -z "$__v" ] && { read -r __v || __v=""; }
    printf '%s' "$__v"
}

# ── 어느 앱의 데이터인가 ──────────────────────────────────────────────────
#   ★ 폴더 이름은 .app 파일명이 아니라 실행 파일 이름(CFBundleExecutable)이다.
#     .app 은 Finder 표시용이라 카타카나(ハンイシキ.app)지만 데이터 폴더는 ASCII 다.
#     이걸 헷갈리면 엉뚱한 곳을 백업하거나, 복원해도 앱이 못 찾는다.
DATA_NAME=""
for CAND in "$SELF_DIR"/*.app "/Applications"/*.app "$HOME/Applications"/*.app; do
    [ -d "$CAND" ] || continue
    ID="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$CAND/Contents/Info.plist" 2>/dev/null || echo "")"
    case "$ID" in
        com.hanishiki.*|com.miyo.*|com.predormition.*)
            DATA_NAME="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleExecutable' "$CAND/Contents/Info.plist" 2>/dev/null || echo "")"
            [ -n "$DATA_NAME" ] && break ;;
    esac
done
if [ -z "$DATA_NAME" ]; then
    N=$(/bin/ls -1d "$BASE"/*/ 2>/dev/null | wc -l | tr -d ' ')
    [ "$N" = "1" ] && DATA_NAME="$(basename "$(/bin/ls -1d "$BASE"/*/ | head -1)")"
fi
[ -z "$DATA_NAME" ] && DATA_NAME="Hanishiki"
DIR="$BASE/$DATA_NAME"

echo ""
echo "  ハンイシキ — 데이터 백업 · 복원"
echo "  ═════════════════════════════════════════════"
echo ""
echo "  대상 폴더: $DIR"
echo ""
echo "    1) 백업   — 다른 디스크로 통째 복사하고 검증합니다."
echo "    2) 복원   — 백업에서 되돌립니다."
echo "    0) 그만둔다"
echo ""
printf "  번호를 입력하고 Enter: "
MODE="$(ask)"; MODE="$(printf '%s' "${MODE:-0}" | tr -d '[:space:]')"

# ══════════════════════════════════════════════════════════════════════════
#  백업
# ══════════════════════════════════════════════════════════════════════════
do_backup() {
    if [ ! -d "$DIR" ] || [ -z "$(/bin/ls -A "$DIR" 2>/dev/null)" ]; then
        echo ""; echo "  백업할 것이 없습니다 — $DIR 가 비어 있습니다."; echo ""
        return 1
    fi

    echo ""
    echo "  들어 있는 것:"
    du -sh "$DIR"/* 2>/dev/null | sort -rh | head -12 | while read -r sz path; do
        printf "    %-8s %s\n" "$sz" "$(basename "$path")"
    done
    local NEED_MB; NEED_MB="$(du -sm "$DIR" 2>/dev/null | cut -f1)"
    echo ""
    echo "  합계: $(du -sh "$DIR" 2>/dev/null | cut -f1)"
    echo ""

    # 붙어 있는 디스크를 보여주고 고르게 한다.
    echo "  어디에 백업할까요?"
    echo ""
    local i=0; local -a VOLS=()
    for v in /Volumes/*; do
        [ -d "$v" ] || continue
        local free_mb; free_mb="$(df -m "$v" 2>/dev/null | tail -1 | awk '{print $4}')"
        [ -z "$free_mb" ] && continue
        # ★ 마운트된 DMG·읽기전용 볼륨은 뺀다.
        #   DMG 를 열어 둔 채로 이 파일을 실행하면 그 DMG 가 목록에 뜬다(여유 0GB).
        #   골라도 쓰지 못하는 항목을 보여 주면 "골랐는데 안 된다" 가 된다.
        #   실제로 써 보고 되는 곳만 남긴다 — df 만으로는 읽기전용을 못 가린다.
        if ! ( : > "$v/.__write_test" ) 2>/dev/null; then continue; fi
        rm -f "$v/.__write_test" 2>/dev/null
        [ "$free_mb" -lt 100 ] && continue          # 100MB 도 없으면 의미 없다
        i=$((i+1)); VOLS[$i]="$v"
        printf "    %d) %-28s 여유 %sGB\n" "$i" "$(basename "$v")" "$((free_mb/1024))"
    done
    i=$((i+1)); VOLS[$i]="$HOME/Desktop"
    printf "    %d) %-28s (바탕화면)\n" "$i" "Desktop"
    echo "    0) 그만둔다"
    echo ""
    printf "  번호: "
    local pick; pick="$(ask)"; pick="$(printf '%s' "${pick:-0}" | tr -d '[:space:]')"
    case "$pick" in ''|0|*[!0-9]*) echo "  그만두었습니다."; return 1 ;; esac
    local VOL="${VOLS[$pick]:-}"
    [ -z "$VOL" ] && { echo "  잘못 고르셨습니다."; return 1; }

    local FREE_MB; FREE_MB="$(df -m "$VOL" 2>/dev/null | tail -1 | awk '{print $4}')"
    if [ "${FREE_MB:-0}" -lt "$((NEED_MB + 500))" ]; then
        echo ""
        echo "  ✘ 공간이 모자랍니다 — 필요 $((NEED_MB/1024))GB, 여유 $((FREE_MB/1024))GB"
        return 1
    fi

    local DEST="$VOL/ハンイシキ_백업_$(date +%Y-%m-%d_%H%M)"
    echo ""
    echo "  백업 위치: $DEST"
    echo "  복사 중… (용량이 크면 수 분 걸립니다)"
    mkdir -p "$DEST" || { echo "  ✘ 폴더를 만들지 못했습니다."; return 1; }
    if ! cp -Rp "$DIR" "$DEST/데이터"; then
        echo "  ✘ 복사에 실패했습니다. 원본은 그대로 있습니다."
        return 1
    fi

    # ── 검증 — 이걸 통과해야만 지울 수 있게 한다 ──────────────────────────
    echo ""
    echo "  검증 중…"
    local A B; A="$(find "$DIR" | wc -l | tr -d ' ')"; B="$(find "$DEST/데이터" | wc -l | tr -d ' ')"
    printf "    항목 수  원본 %s / 백업 %s  " "$A" "$B"
    [ "$A" = "$B" ] && echo "✔" || { echo "✘ 불일치"; echo "  ✘ 검증 실패 — 원본을 지우지 마십시오."; return 1; }

    local ok=1
    for f in miyo_config.json api_overrides.json archive_index.db; do
        [ -f "$DIR/$f" ] || continue
        local h1 h2
        h1="$(shasum -a256 "$DIR/$f" 2>/dev/null | cut -d' ' -f1)"
        h2="$(shasum -a256 "$DEST/데이터/$f" 2>/dev/null | cut -d' ' -f1)"
        if [ "$h1" = "$h2" ] && [ -n "$h1" ]; then
            printf "    %-22s ✔\n" "$f"
        else
            printf "    %-22s ✘ 불일치\n" "$f"; ok=0
        fi
    done
    [ "$ok" = "1" ] || { echo "  ✘ 검증 실패 — 원본을 지우지 마십시오."; return 1; }

    # ── 되돌리는 법을 백업 옆에 적어 둔다 ─────────────────────────────────
    cat > "$DEST/읽어주십시오_복원안내.txt" <<TXT
ハンイシキ 데이터 백업 — $(date '+%Y-%m-%d %H:%M')
════════════════════════════════════════════════════════

원본: $DIR
크기: $(du -sh "$DEST/데이터" 2>/dev/null | cut -f1)

되돌리는 법
────────────────────────────────────────────────────────
이 폴더의 "데이터 백업·복원.command" 를 두 번 눌러 2) 복원 을 고르시거나,
터미널에서 아래 한 줄을 실행하십시오.

  cp -Rp "$DEST/데이터" "$DIR"

※ 폴더 이름이 반드시 $DATA_NAME (로마자) 여야 합니다.
   앱이 그 이름으로 찾습니다. 카타카나로 두면 못 찾습니다.

설정만 되돌리고 AI 모델은 새로 받으실 거면 llm/ 만 빼고 복사하십시오.
TXT
    cp -p "$SELF_DIR/$(basename "$0")" "$DEST/" 2>/dev/null || true

    echo ""
    echo "  ═════════════════════════════════════════════"
    echo "  ✔ 백업 완료 — 검증까지 통과했습니다."
    echo "    $DEST"
    echo ""
    printf "  원본을 휴지통으로 옮길까요? (y 를 입력하면 옮깁니다): "
    local yn; yn="$(ask)"
    case "$(printf '%s' "$yn" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]')" in
        y|yes)
            local dst="$HOME/.Trash/${DATA_NAME}_$(date +%Y%m%d_%H%M%S)"
            if mv "$DIR" "$dst" 2>/dev/null; then
                echo "  ✔ 원본을 휴지통으로 옮겼습니다(영구 삭제 아님)."
            else
                echo "  ✘ 옮기지 못했습니다. 앱이 실행 중이면 종료한 뒤 다시 해 보십시오."
            fi ;;
        *) echo "  원본을 그대로 두었습니다." ;;
    esac
    return 0
}

# ══════════════════════════════════════════════════════════════════════════
#  복원
# ══════════════════════════════════════════════════════════════════════════
do_restore() {
    echo ""
    echo "  백업 폴더를 찾는 중…"
    local i=0; local -a CANDS=()
    for v in /Volumes/* "$HOME/Desktop" "$SELF_DIR"; do
        [ -d "$v" ] || continue
        for b in "$v"/ハンイシキ_백업_*; do
            [ -d "$b/데이터" ] || continue
            i=$((i+1)); CANDS[$i]="$b"
            printf "    %d) %-44s %s\n" "$i" "$(basename "$b")" "$(du -sh "$b/데이터" 2>/dev/null | cut -f1)"
        done
    done
    if [ "$i" = "0" ]; then
        echo "    ✘ 백업을 찾지 못했습니다."
        echo "      (디스크가 연결돼 있는지, 폴더 이름이 ハンイシキ_백업_… 인지 확인해 주십시오)"
        return 1
    fi
    echo "    0) 그만둔다"
    echo ""
    printf "  번호: "
    local pick; pick="$(ask)"; pick="$(printf '%s' "${pick:-0}" | tr -d '[:space:]')"
    case "$pick" in ''|0|*[!0-9]*) echo "  그만두었습니다."; return 1 ;; esac
    local SRC="${CANDS[$pick]:-}"
    [ -z "$SRC" ] && { echo "  잘못 고르셨습니다."; return 1; }

    if [ -d "$DIR" ] && [ -n "$(/bin/ls -A "$DIR" 2>/dev/null)" ]; then
        echo ""
        echo "  ⚠ 지금 데이터가 이미 있습니다: $DIR"
        printf "     덮어쓰기 전에 휴지통으로 옮깁니다. 계속할까요? (y): "
        local yn; yn="$(ask)"
        case "$(printf '%s' "$yn" | tr '[:upper:]' '[:lower:]' | tr -d '[:space:]')" in
            y|yes) mv "$DIR" "$HOME/.Trash/${DATA_NAME}_교체전_$(date +%Y%m%d_%H%M%S)" 2>/dev/null \
                     && echo "     ✔ 기존 것을 휴지통으로 옮겼습니다." ;;
            *) echo "  그만두었습니다."; return 1 ;;
        esac
    fi

    echo ""
    echo "  복원 중…"
    mkdir -p "$BASE"
    if cp -Rp "$SRC/데이터" "$DIR"; then
        echo "  ✔ 복원 완료 — $DIR"
        echo "    항목 $(find "$DIR" | wc -l | tr -d ' ')개"
    else
        echo "  ✘ 복원에 실패했습니다."
        return 1
    fi
    return 0
}

case "$MODE" in
    1) do_backup ;;
    2) do_restore ;;
    *) echo ""; echo "  그만두었습니다." ;;
esac

echo ""
echo "  창을 닫으셔도 됩니다."
echo ""
