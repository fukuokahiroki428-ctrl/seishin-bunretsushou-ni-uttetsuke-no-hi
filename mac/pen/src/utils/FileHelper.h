#pragma once

#include <QString>
#include <QDateTime>

namespace FileHelper {

// Set file creation/modification times
void setFileTimes(const QString &filePath, const QDateTime &timestamp);
void setFileTimes(const QString &filePath, const QString &timestampStr);

// Add macOS Finder comment
void setFinderComment(const QString &filePath, const QString &comment);

// Sanitize filename
QString sanitizeFilename(const QString &name, int maxLength = 200);

// Ensure directory exists
bool ensureDir(const QString &path);

// macOS 다운로드 메타데이터 설정 (xattr — QProcess 없이 직접 syscall)
void setDownloadMeta(const QString &filePath, const QString &url);

// ──────────────────────────────────────────────────────────
// 공통 저장 정책 helpers (플랫폼 간 일관된 폴더/파일명 정책)
// ──────────────────────────────────────────────────────────

// 업로드 시각 → 정렬 가능한 파일명 prefix ("20260411_153042_")
QString uploadOrderPrefix(const QDateTime &uploadTime);
QString uploadOrderPrefix(qint64 unixSec);

// 타입별 하위 폴더 경로 (baseDir/type/, type=="all"이면 baseDir/_complete/)
QString typeFolder(const QString &baseDir, const QString &type);

// 타입별 Excel 파일 경로 (excelDir/{target}_{type}.xlsx)
QString typeExcelPath(const QString &excelDir, const QString &target, const QString &type);

// 메타데이터 일괄 수정: mtime/btime + xattr(URL) + Finder 코멘트
void applyPostMetadata(const QString &filePath,
                        const QDateTime &uploadTime,
                        const QString &sourceUrl = QString(),
                        const QString &finderComment = QString());

} // namespace FileHelper
