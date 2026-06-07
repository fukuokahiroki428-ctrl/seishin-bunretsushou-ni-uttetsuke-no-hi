#include "FileHelper.h"
#include "core/Common.h"
#include <QDir>
#include <QProcess>
#include <QRegularExpression>
#include <QDateTime>

#ifdef Q_OS_WIN
#include <sys/types.h>
#include <sys/utime.h>
#else
#include <sys/stat.h>
#include <utime.h>
#endif

#ifdef Q_OS_MACOS
#include <sys/xattr.h>
#endif

namespace FileHelper {

void setFileTimes(const QString &filePath, const QDateTime &timestamp)
{
    Common::setFileTimes(filePath, timestamp);
}

void setFileTimes(const QString &filePath, const QString &timestampStr)
{
    Common::setFileTimes(filePath, timestampStr);
}

void setFinderComment(const QString &filePath, const QString &comment)
{
#ifdef Q_OS_MACOS
    // ★ 셸/AppleScript 문자열 보간 금지 — 값은 osascript 의 argv 로 전달한다.
    //   이전엔 sh -c + 따옴표 보간이라 파일명/코멘트에 ' 또는 " 가 있으면
    //   AppleScript 가 깨지거나 셸 명령 주입이 가능했다 (이스케이프 순서도 거꾸로였음).
    static const QString script = QStringLiteral(
        "on run argv\n"
        "  set p to item 1 of argv\n"
        "  set c to item 2 of argv\n"
        "  tell application \"Finder\" to set comment of (POSIX file p as alias) to c\n"
        "end run");

    QProcess proc;
    proc.start("/usr/bin/osascript", {"-e", script, filePath, comment});
    proc.waitForFinished(5000);
#else
    Q_UNUSED(filePath);
    Q_UNUSED(comment);
#endif
}

QString sanitizeFilename(const QString &name, int maxLength)
{
    QString result = name;
    // Remove invalid characters
    result.replace(QRegularExpression("[/\\\\:*?\"<>|\\x00-\\x1f]"), "_");
    // Trim whitespace
    result = result.trimmed();
    // Truncate
    if (result.length() > maxLength) {
        result = result.left(maxLength);
    }
    if (result.isEmpty()) {
        result = "unnamed";
    }
    return result;
}

bool ensureDir(const QString &path)
{
    return QDir().mkpath(path);
}

void setDownloadMeta(const QString &filePath, const QString &url)
{
#ifdef Q_OS_MACOS
    QByteArray pathUtf8 = filePath.toUtf8();

    QString urlPlist = QString(
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
        "<plist version=\"1.0\"><array><string>%1</string></array></plist>"
    ).arg(url.toHtmlEscaped());
    QByteArray urlData = urlPlist.toUtf8();
    setxattr(pathUtf8.constData(), "com.apple.metadata:kMDItemWhereFroms",
             urlData.constData(), urlData.size(), 0, 0);

    QString datePlist = QString(
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
        "<plist version=\"1.0\"><array><date>%1</date></array></plist>"
    ).arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    QByteArray dateData = datePlist.toUtf8();
    setxattr(pathUtf8.constData(), "com.apple.metadata:kMDItemDownloadedDate",
             dateData.constData(), dateData.size(), 0, 0);
#else
    Q_UNUSED(filePath);
    Q_UNUSED(url);
#endif
}

// ──────────────────────────────────────────────────────────
// 공통 저장 정책 helpers
// ──────────────────────────────────────────────────────────

QString uploadOrderPrefix(const QDateTime &uploadTime)
{
    if (!uploadTime.isValid()) return QString();
    return uploadTime.toString("yyyyMMdd_HHmmss_");
}

QString uploadOrderPrefix(qint64 unixSec)
{
    if (unixSec <= 0) return QString();
    return QDateTime::fromSecsSinceEpoch(unixSec).toString("yyyyMMdd_HHmmss_");
}

QString typeFolder(const QString &baseDir, const QString &type)
{
    QString folder;
    if (type == "all" || type == "complete") {
        folder = baseDir + "/_complete";
    } else {
        folder = baseDir + "/" + type;
    }
    QDir().mkpath(folder);
    return folder;
}

QString typeExcelPath(const QString &excelDir, const QString &target, const QString &type)
{
    QDir().mkpath(excelDir);
    QString clean = sanitizeFilename(target, 100);
    if (clean.isEmpty()) clean = "data";
    if (type == "all" || type == "complete") {
        return QString("%1/%2_complete.xlsx").arg(excelDir, clean);
    }
    return QString("%1/%2_%3.xlsx").arg(excelDir, clean, type);
}

void applyPostMetadata(const QString &filePath,
                        const QDateTime &uploadTime,
                        const QString &sourceUrl,
                        const QString &finderComment)
{
    if (uploadTime.isValid()) {
        setFileTimes(filePath, uploadTime);
    }
    if (!sourceUrl.isEmpty()) {
        setDownloadMeta(filePath, sourceUrl);
    }
    if (!finderComment.isEmpty()) {
        setFinderComment(filePath, finderComment);
    }
}

} // namespace FileHelper
