#!/bin/bash
# ═══════════════════════════════════════════════════════════════════════════
#  번들 안에 남은 외부(Homebrew) 경로 참조를 @rpath 로 바꾼다.
#
#  왜 필요한가:
#    macdeployqt 는 프레임워크를 Contents/Frameworks 로 복사하지만, 이 번들에는
#    python_env 안에 파이썬 확장 모듈(.so)이 수천 개 있어 macdeployqt 가 그걸
#    훑다가 rpath 해석 오류를 내며 일부 참조를 끝까지 못 고친다.
#    그러면 프레임워크는 번들에 있는데 서로를 /opt/homebrew/... 로 참조해서,
#    Homebrew 를 지운 맥에서는 앱이 켜지지 않는다.
#
#    여기서는 '이미 번들 안에 존재하는' 라이브러리에 대한 외부 참조만 바꾼다.
#    번들에 없는 것은 건드리지 않고 경고만 남긴다(무엇이 빠졌는지 드러나도록).
#
#  codesign 앞에 실행해야 한다 — install_name_tool 이 서명을 깨뜨리기 때문.
# ═══════════════════════════════════════════════════════════════════════════
APP="$1"
[ -d "$APP" ] || { echo "[rpath] 사용법: $0 <app-bundle>"; exit 1; }

FW="$APP/Contents/Frameworks"
fixed=0; missing=0

# 번들 안에서 '이 이름'의 라이브러리가 어디 있는지 찾는다.
locate_in_bundle() {
    local base="$1"
    # 프레임워크: QtCore → QtCore.framework/Versions/A/QtCore
    if [ -f "$FW/$base.framework/Versions/A/$base" ]; then
        echo "@rpath/$base.framework/Versions/A/$base"; return 0
    fi
    # 평범한 dylib
    if [ -f "$FW/$base" ]; then echo "@rpath/$base"; return 0; fi
    return 1
}

# MacOS/ 와 Frameworks/ 의 Mach-O 만 대상 (python_env 의 .so 는 파이썬이 직접
# 로드하므로 건드리지 않는다 — 잘못 손대면 파이썬이 깨진다).
while IFS= read -r f; do
    file "$f" 2>/dev/null | grep -q "Mach-O" || continue

    # ① 자기 자신의 install name(LC_ID_DYLIB) — 이건 -change 가 아니라 -id 로 바꾼다.
    #   otool -L 의 첫 항목이 이것이라, 구분하지 않으면 영원히 안 고쳐진다.
    #   (실행 파일은 ID 가 없어 otool -D 결과가 비어 있다.)
    selfid="$(otool -D "$f" 2>/dev/null | tail -n +2)"
    if [ -n "$selfid" ] && { [ "${selfid#/opt/homebrew}" != "$selfid" ] || [ "${selfid#/usr/local/opt}" != "$selfid" ]; }; then
        if newid="$(locate_in_bundle "$(basename "$selfid")")"; then
            install_name_tool -id "$newid" "$f" 2>/dev/null && fixed=$((fixed + 1))
        fi
    fi

    # ② 의존 참조 — 자기 ID 는 제외하고 -change 로 바꾼다.
    while IFS= read -r ref; do
        [ "$ref" = "$selfid" ] && continue
        base="$(basename "$ref")"
        if newref="$(locate_in_bundle "$base")"; then
            install_name_tool -change "$ref" "$newref" "$f" 2>/dev/null && fixed=$((fixed + 1))
        else
            echo "[rpath] ⚠ 번들에 없음: $base  (참조: ${f#$APP/Contents/})"
            missing=$((missing + 1))
        fi
    done < <(otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep '^/opt/homebrew\|^/usr/local/opt')
done < <(find "$APP/Contents/MacOS" "$FW" -type f 2>/dev/null)

echo "[rpath] 참조 수정 $fixed 건, 번들 누락 $missing 건"

# 최종 확인 — 실행부/프레임워크에 외부 참조가 남으면 실패로 본다.
remain=0
while IFS= read -r f; do
    file "$f" 2>/dev/null | grep -q "Mach-O" || continue
    otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' \
        | grep -q '^/opt/homebrew\|^/usr/local/opt' && remain=$((remain + 1))
done < <(find "$APP/Contents/MacOS" "$FW" -type f 2>/dev/null)

if [ "$remain" -gt 0 ]; then
    echo "[rpath] ❌ 아직 외부 의존이 남은 파일 $remain 개 — 다른 맥에서 실행되지 않습니다."
    exit 1
fi
echo "[rpath] ✅ 실행부·프레임워크 외부 의존 없음 (자립형)"
