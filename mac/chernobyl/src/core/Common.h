#pragma once

#include <QString>
#include <QStringList>
#include <QDateTime>
#include <QProcessEnvironment>

namespace Common {

// 날짜를 일본어 형식으로 변환
QString formatDateJapanese(const QDateTime &dt);
QString formatDateJapanese(const QString &dateStr);

// 파일의 생성일/수정일을 원본 날짜로 설정
void setFileTimes(const QString &filePath, const QDateTime &timestamp);
void setFileTimes(const QString &filePath, const QString &timestampStr);

// ISO 날짜 문자열 파싱
QDateTime parseISODate(const QString &dateStr);

// EXIF 메타데이터 기록 (exiftool 사용, 미설치 시 자동 스킵)
void addExifMetadata(const QString &imagePath, const QString &artist,
                     const QString &description, const QString &copyright,
                     const QString &comment, const QString &dateStr);

// ★ ANSI 로 안전한 경로 — exiftool.exe 처럼 argv 를 시스템 ANSI 코드페이지로 받는
//   프로그램에 경로를 넘길 때 쓴다. 표현 불가능한 문자가 있으면 8.3 단축 경로(항상 ASCII)
//   로 바꿔 준다. 바꿀 수 없으면(8.3 이 꺼진 볼륨 등) 원본을 그대로 돌려준다.
//   Windows 외에서는 항상 원본을 그대로 돌려준다(맥에서는 no-op).
QString ansiSafePath(const QString &path);

// Cross-platform path helpers
// macOS: <쓰기가능 복사본>/bin/python3  (아래 activePythonEnvDir 참고)
// Windows: <exe_dir>/python_env/python.exe
QString bundledPythonPath();

// 실제 사용할 python_env 디렉토리.
//   macOS: 앱 내부(번들) 고정. 예전엔 '외부 복사본 우선, 없으면 시드' 였는데
//     그 두 갈래를 층마다 다르게 가정해 서로 어긋났다(업그레이드가 늘 중단되는
//     고장이 실제로 있었다). 지금은 한 갈래다 — 번들에 설치하고, 설치 직후
//     resealAppBundle() 로 다시 서명해 codesign 봉인을 복구한다.
//     번들에 쓸 수 없으면 몰래 다른 곳으로 새지 않고 호출부가 그 사실을 알린다.
//   Windows/Linux: 설치 위치의 python_env 그대로 (서명 seal 없음).
QString activePythonEnvDir();

// macOS: Contents/Resources/tools/
// Windows: <exe_dir>/tools/
QString bundledToolsDir();
QString scriptOverrideDir();                       // AI 가 고친 스크립트 저장 위치(쓰기가능)
QString activeToolScriptPath(const QString &name); // override 있으면 그것, 없으면 번들 원본

// ★ 시간이 지나면 바뀌는 외부 서비스 상수(X 의 GraphQL query ID, Bearer 토큰 등)를
//   재빌드 없이 교체하기 위한 런타임 오버라이드.
//     - 값은 쓰기가능 위치의 api_overrides.json 에 저장된다.
//     - 없으면 코드에 박힌 기본값을 그대로 쓴다(동작 무변화).
//   X 가 query ID 를 회전시키면 예전엔 앱을 다시 빌드해야 했다 — 이제 이 파일만 고치면 되고,
//   로컬 AI(ハニワ)나 사용자가 설정 화면에서 바로 바꿀 수 있다.
QString apiOverride(const QString &key, const QString &builtinDefault);
bool setApiOverride(const QString &key, const QString &value);   // 빈 값이면 해당 키 삭제(기본값 복귀)
QString apiOverridesPath();
QString apiOverridesJson();                                       // 현재 오버라이드 전체(JSON 문자열)

// ★ macOS: 앱 번들 경로(.../Chernobyl.app). 번들이 아니면 빈 문자열.
QString appBundlePath();
// ★ macOS: 앱 번들 재서명 — 번들 안에 파일이 추가/변경되면(모듈 설치 등) codesign 봉인이 깨져
//   macOS 가 앱을 SIGKILL 할 수 있다. 설치 직후 이걸 호출해 봉인을 복구한다.
//   유효한 서명 아이덴티티가 있으면 그걸 쓰고, 없으면 ad-hoc(-) 서명. 검증까지 통과해야 true.
//   Windows/Linux 는 서명 봉인이 없어 항상 true(무동작).
bool resealAppBundle(QString *err = nullptr);

// 지금 재서명이 돌고 있나. 종료할 때 이것만 기다린다 —
// 서명 도중에 잘리면 번들이 무효인 채로 남기 때문이다.
bool resealInFlight();
// 디렉토리에 실제로 쓸 수 있는지(임시파일 생성/삭제로 확인).
bool isDirWritable(const QString &dir);

// ★ 사용자 도구 폴더 (~/Library/Application Support/Chernobyl/tools/)
//   yt-dlp 자동 업데이트 시 여기에 저장. 우선순위 더 높음.
QString userToolsDir();
// ★ yt-dlp 경로 — 사용자 폴더 우선, 없으면 번들.
QString ytDlpExecutable();
// ★ 앱 시작 시 호출. 사용자 폴더에 yt-dlp 없으면 번들 복사.
//   autoUpdate=true 일 때만 yt-dlp --update-to stable 시도 (사용자 명시 ON).
//   업데이트 후 sanity check 실패 시 번들로 자동 복원 — 변조 binary 차단.
void ensureYtDlpReady(bool autoUpdate = false);

// ★ JS string literal 안전 생성 — backslash, quote, newline 등 모두 escape.
//   결과: "..." 형태의 JS literal (QJsonDocument array 사용 → 빈 string 도 안전).
//   QJsonDocument::fromVariant 가 단순 string root 안 받는 Qt6 한계 우회.
QString jsStringLiteral(const QString &s);

// ★ 사용자 임시 디스크 우선 — Config 의 tempDir 가 있고 NAS 아니고 존재하면 그것 사용.
//   아니면 시스템 /tmp fallback. 캡쳐 buffer / script / chunked write 등에 사용.
//   "메모리 대신 임시 디스크에 swap" 전략 — 큰 buffer 가 메모리 안 차고 디스크 사용.
QString resolveTempBase(const QString &userConfigTempDir);

// ★ 파일 무결성 검사 — 다운로드된 파일이 손상되지 않았는지 확인.
//   type 별 검사:
//     - 이미지 (jpg/png/gif/webp): QImage 로 load 시도
//     - 비디오 (mp4/webm/mov): 파일 크기 + magic byte
//     - HTML: <html> tag 존재 확인
//     - 그 외: 0 byte 또는 너무 작은 파일 (< 100 byte) 검사
//   결과: 정상이면 빈 string, 손상이면 원인 메시지.
QString checkFileIntegrity(const QString &filePath);

// macOS: Contents/Resources/
// Windows: <exe_dir>/
QString bundledResourcesDir();
// requirements.txt 의 패키지 목록.
//   stripPins=false : 'yt-dlp==2026.7.4' 그대로. 환경을 '다시 만들' 때 쓴다(재현성).
//   stripPins=true  : 'yt-dlp' 만. '최신으로 올릴' 때 쓴다.
// ★ 예전엔 이 구분이 없어서 갱신도 고정본을 그대로 pip 에 넘겼다.
//   pip 는 "Requirement already satisfied" 로 끝내고, 앱은 그걸 "최신" 이라고
//   보고했다. 실제로는 한 판도 올라가지 않는데 올라간 줄 알게 되는 형태였다.
// 파일을 '통째로 바뀌거나, 전혀 안 바뀌거나' 로 쓴다.
// ★ 왜 필요한가. 지금까지 설정·색인·override 를 QIODevice::WriteOnly 로 열어 썼다.
//   그 순간 파일은 0바이트로 잘린다. 쓰기가 끝나기 전에 앱이 죽거나 전원이 나가면
//   남는 것은 잘린 파일이다 — 계정·토큰·저장된 입력값이 통째로 사라진다.
//   백업본도 바로 뒤이어 같은 방식으로 덮어써서, 둘 다 상하는 창이 있었다.
//   QSaveFile 은 임시 파일에 다 쓴 뒤 원자적으로 갈아 끼운다. 중간 상태가 없다.
// 요청에 실을 User-Agent.
// ★ 왜 코드에 박으면 안 되나.
//   이 앱은 사용자의 '진짜 브라우저 세션 쿠키' 로 요청한다. 그런데 UA 는
//   Chrome/120·Chrome/131·Safari 17.5 로 박혀 있었다. 실제 브라우저는 Chrome 151 이다.
//   쿠키를 만든 브라우저와 UA 가 어긋나면 그 자체가 자동화 신호다 —
//   플랫폼이 계정을 묶어 정지시키는 근거가 된다. 몇 년 지난 UA 는 더 눈에 띈다.
//   → 설치된 브라우저에서 실제 판을 읽어 만든다. 한 번 만들고 캐시한다.
QString browserUserAgent();

// ── 프록시(VPN) ────────────────────────────────────────────────────────────
// ★ 이 앱은 요청을 다섯 갈래로 내보낸다 — Qt(HttpClient)·파이썬 데몬·yt-dlp·
//   rclone·번들 크로미움. 프록시를 쓰기로 했으면 다섯이 전부 같은 길로 나가야 한다.
//   하나라도 빠지면 같은 세션이 두 IP 에서 온 것처럼 보여 오히려 더 눈에 띈다.
//   그래서 설정을 한 곳에 두고, 각 경로가 여기서 받아 간다.
//
//   자격증명은 명령줄로 넘기지 않는다(ps 로 남이 읽는다). 환경변수와 stdin 만 쓴다.
void  setProxyConfig(bool enabled, const QString &host, int port,
                     const QString &user, const QString &pass);
bool  proxyEnabled();
// "socks5://user:pass@host:port" — 없으면 빈 문자열.
QString proxyUrl();
// 크로미움처럼 '인증을 못 넣는' 프로그램용. 로컬 중계기를 띄우고 그 주소를 준다.
// (인증 없는 127.0.0.1 → 중계기가 상위에 인증해 연결)
QString proxyLocalRelayUrl();
void  stopProxyRelay();


bool writeFileAtomic(const QString &path, const QByteArray &bytes, QString *err = nullptr);

QStringList bundledRequirements(bool stripPins = false);   // 번들 requirements.txt 의 패키지 목록(버전 고정 포함)


// Get list of Python candidates (bundled first, then system fallbacks)
QStringList pythonCandidates();

// Cross-platform bundled environment (PATH, library paths, etc.)
QProcessEnvironment bundledProcessEnv();

// 저장 경로의 남은 공간 (바이트). 경로 없거나 마운트 안 됐으면 0.
qint64 freeSpace(const QString &path);

// 저장 경로 2개 중 공간 충분한 곳 선택. primary가 thresholdGB 이상이면 그대로 primary,
// 아니면 secondary 사용. secondary도 부족하면 그래도 primary 반환 (사용자가 알게).
//   primary, secondary: 후보 경로 (~ expand 된 절대 경로)
//   thresholdGB: 이 값 이상 free일 때만 primary 사용 (기본 10GB)
QString pickSavePath(const QString &primary, const QString &secondary, double thresholdGB = 10.0);

} // namespace Common
