#!/bin/bash
# ============================================================
#   カメラ 앱 개발자 인증 (코드 서명) 도구
#   - 다운로드한 앱의 '손상됨/미확인 개발자' 차단을 풀고,
#     본인 Mac 의 Apple 개발자 ID 로 서명해 정상 실행되게 합니다.
#   - 개발자 인증서/앱 경로를 '자동 탐지 + 번호 선택' 으로 고를 수 있습니다.
#   - 사용법: 이 파일을 더블클릭 (또는 터미널에서 bash 로 실행).
# ============================================================
cd "$(dirname "$0")"

bold(){ printf "\033[1m%s\033[0m\n" "$1"; }
ok(){   printf "  \033[32m✅ %s\033[0m\n" "$1"; }
warn(){ printf "  \033[33m⚠️  %s\033[0m\n" "$1"; }
err(){  printf "  \033[31m❌ %s\033[0m\n" "$1"; }

clear
bold "════════════════════════════════════════════"
bold "     カメラ 앱 개발자 인증 (코드 서명)"
bold "════════════════════════════════════════════"
echo

# ── 1) 앱 경로 선택 (자동 탐지 + 번호 선택 + 직접 입력) ──────
bold "▶ 1단계: 앱(カメラ.app) 위치 선택"
CANDS=()
for p in "/Applications/カメラ.app" "$HOME/Applications/カメラ.app" \
         "$(dirname "$0")/カメラ.app" "$HOME/Downloads/カメラ.app" \
         "$HOME/Desktop/カメラ.app"; do
  [ -d "$p" ] && CANDS+=("$p")
done
APP=""
if [ ${#CANDS[@]} -eq 0 ]; then
  warn "자동 탐색 실패 — 앱 경로를 직접 입력하세요. (Finder 에서 앱을 터미널로 드래그하면 경로가 입력됩니다)"
  read -p "  カメラ.app 경로: " APP
else
  i=1; for c in "${CANDS[@]}"; do echo "    $i) $c"; i=$((i+1)); done
  echo "    d) 직접 경로 입력"
  read -p "  번호 선택 (엔터=1번 자동): " sel
  if   [ -z "$sel" ]; then APP="${CANDS[0]}"
  elif [ "$sel" = "d" ]; then read -p "  경로 입력: " APP
  else APP="${CANDS[$((sel-1))]}"; fi
fi
# 따옴표/공백 정리
APP="${APP%\"}"; APP="${APP#\"}"; APP="${APP%/}"
if [ -z "$APP" ] || [ ! -d "$APP" ]; then
  err "유효한 앱 경로가 아닙니다: $APP"; echo; read -p "엔터를 누르면 종료..." _; exit 1
fi
echo "  선택된 앱: $APP"

# 읽기전용(DMG 안)이면 서명 불가 → 안내
if ! touch "$APP/.write_test" 2>/dev/null; then
  echo; err "이 앱은 읽기전용 위치(DMG 등)에 있어 서명할 수 없습니다."
  echo "     'カメラ.app' 을 '응용 프로그램(Applications)' 으로 복사한 뒤 다시 실행하세요."
  echo; read -p "엔터를 누르면 종료..." _; exit 1
fi
rm -f "$APP/.write_test" 2>/dev/null
echo

# ── 2) 격리(quarantine) 제거 — '손상되어 열 수 없음' 방지 ────
bold "▶ 2단계: 격리 속성 제거 (다운로드 차단 해제)"
xattr -cr "$APP" 2>/dev/null
xattr -dr com.apple.quarantine "$APP" 2>/dev/null
ok "완료"
echo

# ── 3) 개발자 인증서 자동 탐지 + 번호 선택 ──────────────────
bold "▶ 3단계: 개발자 인증서 선택 (자동 탐지)"
NAMES=()
while IFS= read -r line; do
  [ -z "$line" ] && continue
  NAMES+=("$(echo "$line" | sed -E 's/.*"(.*)".*/\1/')")
done < <(security find-identity -v -p codesigning 2>/dev/null | grep '"')

MATCH=""
if [ ${#NAMES[@]} -eq 0 ]; then
  warn "이 Mac 에 Apple 개발자 인증서가 없습니다."
  echo "     인증서 없이 '이 Mac 에서만' 실행되게 임시 서명(ad-hoc)할 수 있습니다."
  read -p "     임시 서명으로 진행할까요? (y/n): " ans
  if [ "$ans" = "y" ] || [ "$ans" = "Y" ]; then
    echo "  서명 중..."
    codesign --force --deep --sign - "$APP" 2>/dev/null && ok "임시 서명 완료 — 이 Mac 에서 실행 가능" || err "임시 서명 실패"
  fi
  echo; read -p "엔터를 누르면 종료..." _; exit 0
else
  i=1; for n in "${NAMES[@]}"; do echo "    $i) $n"; i=$((i+1)); done
  echo "    t) 직접 ID 문자열 입력"
  if [ ${#NAMES[@]} -eq 1 ]; then
    echo
    echo "  인증서가 1개 발견됨 → 엔터를 누르면 자동으로 이 인증서로 서명합니다."
  fi
  read -p "  번호 선택 (엔터=자동 1번): " sel
  if   [ -z "$sel" ]; then MATCH="${NAMES[0]}"            # 자동 선택
  elif [ "$sel" = "t" ]; then
    read -p "  개발자 ID(이메일/문자열): " DEVID
    MATCH="$(security find-identity -v -p codesigning 2>/dev/null | grep '"' | grep -F "$DEVID" | head -1 | sed -E 's/.*"(.*)".*/\1/')"
  else MATCH="${NAMES[$((sel-1))]}"; fi
fi
if [ -z "$MATCH" ]; then
  err "선택된 인증서가 없습니다."; echo; read -p "엔터를 누르면 종료..." _; exit 1
fi
echo "  사용할 인증서: $MATCH"
echo

# ── 4) 코드 서명 (inside-out: 중첩 코드 먼저) ────────────────
bold "▶ 4단계: 앱 서명 (수십 초 ~ 1분 소요)"
echo "  중첩 바이너리 서명 중..."
find "$APP/Contents" \( -name "*.dylib" -o -name "*.so" \) -type f -print0 2>/dev/null \
  | xargs -0 -I{} codesign --force --sign "$MATCH" {} 2>/dev/null
codesign --force --deep --sign "$MATCH" "$APP" 2>/tmp/kamera_sign.log \
  && ok "서명 완료" \
  || { warn "일부 경고 (대개 실행엔 무관):"; tail -3 /tmp/kamera_sign.log | sed 's/^/      /'; }
echo

# ── 5) 검증 ─────────────────────────────────────────────────
bold "▶ 5단계: 검증"
codesign --verify --deep --strict "$APP" 2>/dev/null \
  && ok "서명 검증 통과!" \
  || warn "검증 일부 경고 — 그래도 실행은 보통 됩니다."
xattr -dr com.apple.quarantine "$APP" 2>/dev/null
echo
bold "════════════════════════════════════════════"
ok "인증 완료! 이제 '$APP' 을 더블클릭해 실행하세요."
bold "════════════════════════════════════════════"
echo "  (처음 실행 시 경고가 나오면: 우클릭 → 열기 → 열기)"
echo
read -p "엔터를 누르면 종료합니다..." _
