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

// Cross-platform path helpers
// macOS: <쓰기가능 복사본>/bin/python3  (아래 activePythonEnvDir 참고)
// Windows: <exe_dir>/python_env/python.exe
QString bundledPythonPath();

// ★ 쓰기가능 python_env 경로 (~/Library/Application Support/.../python_env).
//   macOS 는 .app 번들이 codesign 으로 sealed → 번들 내부 python_env 에 pip install 하면
//   "sealed resource invalid" → macOS 가 앱을 SIGKILL. 그래서 외부 복사본을 두고 사용한다.
QString userPythonEnvDir();
// 실제 사용할 python_env 디렉토리.
//   macOS: 외부 복사본 우선 — 없으면 번들에서 1회 자동 복사(시드). 복사 실패 시 읽기전용 번들.
//   Windows/Linux: 설치 위치의 python_env 그대로 (서명 seal 없음 → 제자리 수정 안전).
QString activePythonEnvDir();

// macOS: Contents/Resources/tools/
// Windows: <exe_dir>/tools/
QString bundledToolsDir();

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
