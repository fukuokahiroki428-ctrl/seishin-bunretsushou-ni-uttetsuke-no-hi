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
// macOS: Contents/Resources/python_env/bin/python3
// Windows: <exe_dir>/python_env/python.exe
QString bundledPythonPath();

// macOS: Contents/Resources/tools/
// Windows: <exe_dir>/tools/
QString bundledToolsDir();

// macOS: Contents/Resources/
// Windows: <exe_dir>/
QString bundledResourcesDir();

// Get list of Python candidates (bundled first, then system fallbacks)
QStringList pythonCandidates();

// Cross-platform bundled environment (PATH, library paths, etc.)
QProcessEnvironment bundledProcessEnv();

} // namespace Common
