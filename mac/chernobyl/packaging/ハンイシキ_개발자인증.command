#!/bin/bash
# ============================================================
#   ハンイシキ 개발자 인증 — 코드 서명 다시 하기 (더블클릭)
#   - 서명 인증서가 만료됐거나 새로 받았을 때, 이 Mac 의 인증서로 앱을 다시 서명합니다.
#   - 인증서가 없으면 '이 Mac 에서만' 도는 임시 서명(ad-hoc)을 고를 수 있습니다. 그때 macOS 는 앱을
#     '다른 앱' 으로 보므로 전체 디스크 접근 · 자동화 같은 권한을 다시 허락해야 합니다.
#   - 앱 안에 든 정식 서명 스크립트(codesign_app.sh)로 서명합니다 — 빌드 · 앱의 자동 복구와 같은 방식.
#     (예전 이름: packaging/카메라_개발자인증.command — 앱 이름이 カメラ 이던 때의 것)
#   - 앱이 켜져 있으면 먼저 끄게 합니다(켠 채로 서명하면 앱이 죽습니다).
# ============================================================
cd "$(dirname "$0")" || exit 1

bold(){ printf "\033[1m%s\033[0m\n" "$1"; }
ok(){   printf "  \033[32m✅ %s\033[0m\n" "$1"; }
warn(){ printf "  \033[33m⚠️  %s\033[0m\n" "$1"; }
err(){  printf "  \033[31m❌ %s\033[0m\n" "$1"; }
bye(){  echo; read -r -p "엔터를 누르면 닫습니다..." _; exit "${1:-0}"; }
BUNDLE_ID="com.hanishiki.app"

clear
bold "════════════════════════════════════════════"
bold "   ハンイシキ 개발자 인증 (코드 서명 다시 하기)"
bold "════════════════════════════════════════════"
echo

# ── 1) 앱 찾기 — 이름이 아니라 번들 식별자로(이름이 바뀌어도 찾게) ─────────────────────
bold "▶ 1단계: 앱 위치"
CANDS=()
for dir in "$HOME/Applications" "/Applications" "$(pwd)" "$HOME/Downloads" "$HOME/Desktop"; do
  for p in "$dir"/*.app; do
    [ -d "$p" ] || continue
    id="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleIdentifier' "$p/Contents/Info.plist" 2>/dev/null)"
    [ "$id" = "$BUNDLE_ID" ] && CANDS+=("$p")
  done
done
APP=""
if [ ${#CANDS[@]} -eq 0 ]; then
  warn "앱을 찾지 못했습니다 — 경로를 직접 넣으십시오(Finder 에서 앱을 이 창으로 끌어다 놓으면 경로가 들어갑니다)."
  read -r -p "  앱 경로: " APP
else
  i=1
  for c in "${CANDS[@]}"; do
    v="$(/usr/libexec/PlistBuddy -c 'Print :CFBundleVersion' "$c/Contents/Info.plist" 2>/dev/null)"
    echo "    $i) $c  (판 ${v:-?})"; i=$((i+1))
  done
  echo "    d) 직접 경로 넣기"
  read -r -p "  번호 (엔터 = 1번): " sel
  if   [ -z "$sel" ]; then APP="${CANDS[0]}"
  elif [ "$sel" = "d" ]; then read -r -p "  앱 경로: " APP
  elif [[ "$sel" =~ ^[0-9]+$ ]] && [ "$sel" -ge 1 ] && [ "$sel" -le ${#CANDS[@]} ]; then APP="${CANDS[$((sel-1))]}"
  fi
fi
# Finder 에서 끌어다 놓으면 공백 앞에 \ 가 붙는다 — 풀어 준다. 따옴표 · 끝의 / · 공백도 떼고
APP="$(printf '%s' "$APP" | sed -E 's/\\(.)/\1/g')"
APP="${APP%\"}"; APP="${APP#\"}"; APP="${APP% }"; APP="${APP%/}"
if [ -z "$APP" ] || [ ! -d "$APP" ]; then err "앱 경로가 아닙니다: $APP"; bye 1; fi
echo "  고른 앱: $APP"
if ! touch "$APP/.write_test" 2>/dev/null; then
  err "이 앱은 읽기 전용 자리(DMG 등)에 있어 서명할 수 없습니다."
  echo "     앱을 '응용 프로그램' 폴더(또는 ~/Applications)로 옮긴 뒤 다시 실행하십시오."
  bye 1
fi
rm -f "$APP/.write_test" 2>/dev/null
echo

# ── 2) 켜져 있으면 끄게 한다 — 켠 채로 서명하면 실행 파일이 바뀌어 앱이 죽는다 ───────────
# 그 앱의 실행 파일로 시작한 프로세스가 있나 — 글자 그대로 앞부분을 견준다(pgrep -f 는 정규식이라 괄호가 든 경로를 놓치고,
#   ~/Applications 와 /Applications 의 두 사본을 헷갈렸다 — 검토)
#   UTF-8 로 찍게 한다 — 그렇지 않은 셸에서는 ps 가 일본어 경로를 바꿔 찍어 견주지 못한다(검토)
app_running(){ LC_ALL=en_US.UTF-8 ps -axo command= | LC_ALL=en_US.UTF-8 awk -v p="$APP/Contents/MacOS/" 'index($0,p)==1{f=1} END{exit !f}'; }
while app_running; do
  warn "앱이 켜져 있습니다. 앱을 끈 뒤 엔터를 누르십시오(그만두려면 q)."
  read -r -p "  " ans
  [ "$ans" = "q" ] && bye 0
done

# ── 3) 격리 표시 지우기 — '손상되어 열 수 없음' 방지 ───────────────────────────────────
bold "▶ 2단계: 격리 표시 지우기"
xattr -cr "$APP" 2>/dev/null
ok "완료"
echo

# ── 4) 인증서 고르기 ───────────────────────────────────────────────────────────────────
bold "▶ 3단계: 서명 인증서 고르기"
# 이름과 지문(SHA-1)을 함께 읽는다 — 서명은 지문으로 한다(갱신하면 같은 이름이 둘 생겨 이름으로는 '모호함' 으로 멈춘다)
NAMES=(); HASHES=()
while IFS= read -r line; do
  [ -z "$line" ] && continue
  NAMES+=("$(printf '%s\n' "$line" | sed -E 's/.*"(.*)".*/\1/')")
  HASHES+=("$(printf '%s\n' "$line" | grep -oE '[0-9A-F]{40}' | head -1)")
done < <(security find-identity -v -p codesigning 2>/dev/null | grep '"')
MATCH=""
if [ ${#NAMES[@]} -eq 0 ]; then
  warn "이 Mac 에 쓸 수 있는 서명 인증서가 없습니다(만료됐거나 아직 받지 않음)."
  echo "     새 인증서를 받으셨다면 키체인에 넣은 뒤 이 파일을 다시 실행하십시오."
  echo "     인증서 없이 '이 Mac 에서만' 도는 임시 서명(ad-hoc)도 할 수 있습니다."
  echo "     그때는 macOS 가 앱을 다른 앱으로 보아 권한(전체 디스크 접근 · 자동화 등)을 다시 허락해야 합니다."
  read -r -p "     임시 서명으로 할까요? (y/n): " ans
  if [ "$ans" = "y" ] || [ "$ans" = "Y" ]; then MATCH="-"; else bye 0; fi
else
  i=1; for n in "${NAMES[@]}"; do echo "    $i) $n  [${HASHES[$((i-1))]:0:8}…]"; i=$((i+1)); done
  echo "    a) 임시 서명(ad-hoc) — 권한을 다시 허락해야 합니다"
  read -r -p "  번호 (엔터 = 1번): " sel
  if   [ -z "$sel" ]; then MATCH="${HASHES[0]}"; MATCH_NAME="${NAMES[0]}"
  elif [ "$sel" = "a" ]; then MATCH="-"
  elif [[ "$sel" =~ ^[0-9]+$ ]] && [ "$sel" -ge 1 ] && [ "$sel" -le ${#NAMES[@]} ]; then
    MATCH="${HASHES[$((sel-1))]}"; MATCH_NAME="${NAMES[$((sel-1))]}"
  fi
fi
[ -z "$MATCH" ] && { err "고른 인증서가 없습니다."; bye 1; }
[ "$MATCH" = "-" ] && echo "  임시 서명(ad-hoc)으로 합니다" || echo "  쓸 인증서: $MATCH_NAME"
echo

# ── 5) 서명 — 앱에 든 정식 스크립트로(중첩 코드를 안쪽부터 · 마지막에 엄격 검사) ──────────
bold "▶ 4단계: 앱 서명 (1~2분)"
SIGNER="$APP/Contents/Resources/tools/codesign_app.sh"
RC=1
if [ -f "$SIGNER" ]; then
  # 서명하는 동안 번들 안의 스크립트를 읽지 않게 바깥에 옮겨 놓고 돌린다(entitlements 도 함께)
  WORK="$(mktemp -d)"
  cp "$SIGNER" "$WORK/codesign_app.sh"
  [ -f "$APP/Contents/Resources/tools/hanishiki.entitlements" ] && cp "$APP/Contents/Resources/tools/hanishiki.entitlements" "$WORK/"
  # 이 파일로 직접 고른 임시 서명은 허락한다(앱이 혼자 임시 서명으로 바꾸는 것만 막는 장치 — ALLOW_ADHOC)
  if [ "$MATCH" = "-" ]; then ADH=1; else ADH=0; fi
  SIGN_ID="$MATCH" ALLOW_ADHOC="$ADH" bash "$WORK/codesign_app.sh" "$APP" 2>&1 | sed 's/^/    /' | tail -15
  RC=${PIPESTATUS[0]}
  rm -rf "$WORK"
else
  warn "앱 안에 정식 서명 스크립트가 없습니다 — 옛 방식(안쪽부터 + --deep)으로 서명합니다."
  find "$APP/Contents" \( -name "*.dylib" -o -name "*.so" \) -type f -print0 2>/dev/null \
    | xargs -0 -I{} codesign --force --sign "$MATCH" {} 2>/dev/null
  codesign --force --deep --sign "$MATCH" "$APP" 2>&1 | tail -3 | sed 's/^/    /'
  RC=${PIPESTATUS[0]}
fi
echo

# ── 6) 확인 ─────────────────────────────────────────────────────────────────────────────
bold "▶ 5단계: 확인"
if codesign --verify --deep --strict "$APP" 2>/dev/null; then
  ok "서명이 맞습니다"
  WHO="$(codesign -dvv "$APP" 2>&1 | sed -n 's/^Authority=//p' | head -1)"
  [ -n "$WHO" ] && echo "     서명: $WHO" || echo "     서명: 임시 서명(ad-hoc)"
  xattr -dr com.apple.quarantine "$APP" 2>/dev/null
  echo
  bold "════════════════════════════════════════════"
  ok "끝났습니다. 이제 앱을 여십시오."
  bold "════════════════════════════════════════════"
  if [ "$MATCH" = "-" ]; then
    echo "  임시 서명이라 권한을 다시 허락해야 합니다:"
    echo "    시스템 설정 → 개인정보 보호 및 보안 → 전체 디스크 접근 권한 · 자동화 · 화면 기록 에서 ハンイシキ 를 켜 주십시오."
  fi
  bye 0
else
  err "서명 확인에 실패했습니다(코드 $RC)."
  echo "     iCloud 로 동기화되는 폴더(문서 · 데스크탑)에 있으면 macOS 가 붙이는 표시 때문일 수 있습니다 —"
  echo "     앱을 ~/Applications 로 옮긴 뒤 다시 실행해 보십시오."
  bye 1
fi
