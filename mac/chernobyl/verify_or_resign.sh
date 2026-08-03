#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════
# 빌드 끝마다 도는 서명 정합성 확인 — 깨져 있을 때만 다시 서명한다.
#
#  왜 필요한가:
#    codesign 은 POST_BUILD 로 붙어 있는데, POST_BUILD 는 "타겟이 실제로 다시
#    링크될 때만" 돈다. 그런데 Info.plist 는 cmake 를 다시 '구성'하는 것만으로도
#    새로 만들어진다(예: VERSION 파일만 고친 경우). 그러면 소스가 안 바뀌어
#    링크가 없고 → 서명 단계도 안 돌아, 서명이 plist 와 어긋난 채로 남는다.
#    macOS 는 이 앱을 실행조차 시키지 않는다:
#      "invalid Info.plist (plist or signature have been modified)"
#      "Launch failed. NSPOSIXErrorDomain Code=163"
#    실제로 v3.9.1 작업 중 이 상태가 나와 앱이 안 켜졌다.
#
#  비용: 정상이면 검증만 하고 끝난다(9.8GB 번들 기준 약 5초).
# ═══════════════════════════════════════════════════════════════════════════
APP="$1"
[ -d "$APP" ] || exit 0
DIR="$(cd "$(dirname "$0")" && pwd)"

resign() {
    echo "[codesign] ⚠ $1 — 다시 서명합니다"
    bash "$DIR/codesign_app.sh" "$APP"
    exit $?
}

PLIST="$APP/Contents/Info.plist"
SEAL="$APP/Contents/_CodeSignature/CodeResources"

# ① 봉인이 아예 없음
[ -f "$SEAL" ] || resign "서명이 없습니다"

# ② Info.plist 가 봉인보다 새것 — 위에서 설명한 바로 그 경우(즉시 판정, 비용 0)
if [ "$PLIST" -nt "$SEAL" ]; then
    resign "Info.plist 가 서명보다 최신입니다(구성 단계에서 재생성됨)"
fi

# ③ 그 외 손상 — 전체 검증
if ! codesign --verify --deep --strict "$APP" 2>/dev/null; then
    resign "서명 검증 실패"
fi

echo "[codesign] 서명 유효 — 재서명 불필요"
