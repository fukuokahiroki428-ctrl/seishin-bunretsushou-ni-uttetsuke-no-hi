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
    #   @executable_path/../Frameworks/X 형태의 ID 도 함께 정리한다. 실행 시점에는
    #   '참조' 쪽 load command 만 쓰이므로 ID 가 남아도 당장은 돌지만, 남겨 두면
    #   아래 최종 검사에 걸리고 이 번들을 링크하는 쪽에 그 경로가 그대로 박힌다.
    selfid="$(otool -D "$f" 2>/dev/null | tail -n +2)"
    if [ -n "$selfid" ] && { [ "${selfid#/opt/homebrew}" != "$selfid" ] \
            || [ "${selfid#/usr/local/opt}" != "$selfid" ] \
            || [ "${selfid#@executable_path/../Frameworks/}" != "$selfid" ]; }; then
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

    # ③ @executable_path/../Frameworks/... → @rpath/...
    #
    #   macdeployqt 는 라이브러리 참조를 @executable_path/../Frameworks/X 로 바꾼다.
    #   메인 실행부에서는 맞다(Contents/MacOS → Contents/Frameworks).
    #   그런데 WebEngine 렌더러는 별도 프로세스이고, 그쪽에서 @executable_path 는
    #     .../Helpers/QtWebEngineProcess.app/Contents/MacOS
    #   라서 자기 앱 안의 없는 Frameworks 를 가리킨다. 그러면 렌더러가 ICU 를 못 찾고
    #   죽고, 창은 뜨는데 화면이 통째로 비어 버린다(실제로 발생했다 — dyld:
    #   "Library not loaded: @executable_path/../Frameworks/libicui18n.78.dylib").
    #
    #   @rpath 로 바꾸면 둘 다 풀린다. 메인 실행부에는 @executable_path/../Frameworks
    #   가, 헬퍼에는 @loader_path/../../../../../../../ 가 이미 LC_RPATH 로 들어 있어
    #   각자 같은 Contents/Frameworks 에 닿는다.
    while IFS= read -r ref; do
        [ "$ref" = "$selfid" ] && continue
        base="$(basename "$ref")"
        if newref="$(locate_in_bundle "$base")"; then
            install_name_tool -change "$ref" "$newref" "$f" 2>/dev/null && fixed=$((fixed + 1))
        fi
    done < <(otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' | grep '^@executable_path/\.\./Frameworks/')
done < <(find "$APP/Contents/MacOS" "$FW" -type f 2>/dev/null)

echo "[rpath] 참조 수정 $fixed 건, 번들 누락 $missing 건"

# 최종 확인 — 실행부/프레임워크에 외부 참조가 남으면 실패로 본다.
remain=0
while IFS= read -r f; do
    file "$f" 2>/dev/null | grep -q "Mach-O" || continue
    otool -L "$f" 2>/dev/null | tail -n +2 | awk '{print $1}' \
        | grep -q '^/opt/homebrew\|^/usr/local/opt\|^@executable_path/\.\./Frameworks/' && remain=$((remain + 1))
done < <(find "$APP/Contents/MacOS" "$FW" -type f 2>/dev/null)

if [ "$remain" -gt 0 ]; then
    echo "[rpath] ❌ 아직 못 고친 참조가 남은 파일 $remain 개 — 다른 맥에서 안 뜨거나 화면이 빈 채로 뜹니다."
    exit 1
fi
echo "[rpath] ✅ 실행부·프레임워크 외부 의존 없음 (자립형)"
