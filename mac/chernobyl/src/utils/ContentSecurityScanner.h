#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QByteArray>
#include <QRegularExpression>
#include <QJsonObject>
#include <QJsonArray>

struct ScanResult {
    enum Severity { Clean, Warning, Dangerous };
    Severity severity = Clean;
    QStringList findings;
    QString suggestedAction = "pass"; // "pass", "sanitize", "quarantine"
};

class ContentSecurityScanner
{
public:
    // JS 악성코드 패턴 검사
    static ScanResult scanJavaScript(const QByteArray &content, const QString &sourceUrl);

    // 파일 타입 위장 검사 (확장자 vs 실제 매직바이트)
    static ScanResult scanFileType(const QString &filePath, const QByteArray &content);

    // 위험 파일 격리
    static bool quarantineFile(const QString &filePath, const QString &quarantineDir);

    // JS 정화 (Warning 레벨)
    static QByteArray sanitizeJavaScript(const QByteArray &content);

    // 오프라인 뷰어용 JS 샌드박스 헤더
    static QByteArray jsSandboxHeader();

    // HTML에 CSP 메타 태그 삽입
    static QString injectCSP(const QString &html);

    // blocked.html 플레이스홀더 생성
    static void createBlockedPlaceholder(const QString &quarantineDir);

private:
    struct Pattern {
        QRegularExpression regex;
        ScanResult::Severity severity;
        QString description;
    };
    static QList<Pattern> jsPatterns();
};
