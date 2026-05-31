#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkProxy>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QEventLoop>
#include <QCryptographicHash>
#include <functional>

struct HttpResponse {
    int statusCode = 0;
    QByteArray data;
    QString error;
    bool isOk() const { return statusCode >= 200 && statusCode < 300; }
    QJsonObject json() const { return QJsonDocument::fromJson(data).object(); }
    QJsonArray jsonArray() const { return QJsonDocument::fromJson(data).array(); }
};

class HttpClient : public QObject
{
    Q_OBJECT

public:
    explicit HttpClient(QObject *parent = nullptr);
    ~HttpClient() override;

    // Synchronous requests (blocks until complete)
    HttpResponse get(const QString &url, const QMap<QString, QString> &headers = {});
    HttpResponse post(const QString &url, const QByteArray &body = {},
                      const QMap<QString, QString> &headers = {});
    HttpResponse postJson(const QString &url, const QJsonObject &json,
                          const QMap<QString, QString> &headers = {});

    // Download file (streams to disk, uses separate download timeout)
    bool downloadFile(const QString &url, const QString &filePath,
                      const QMap<QString, QString> &headers = {});

    // Extended download with metadata (보안 검사용)
    struct DownloadResult {
        bool success = false;
        int statusCode = 0;
        QString contentType;
        qint64 contentLength = 0;
        QByteArray sha256;
        QByteArray headBytes; // 첫 512바이트 (매직바이트 검사용)
    };
    DownloadResult downloadFileEx(const QString &url, const QString &filePath,
                                  const QMap<QString, QString> &headers = {});

    // Settings
    void setProxy(const QString &host, int port, QNetworkProxy::ProxyType type = QNetworkProxy::HttpProxy);
    void clearProxy();
    void setTimeout(int msec) { m_timeout = msec; }
    void setDownloadTimeout(int msec) { m_downloadTimeout = msec; }
    void setCookies(const QMap<QString, QString> &cookies);

    // Rate limiting
    void setRateLimit(int requestsPerSecond);

    // 외부 '진행 중' 플래그 — 포인터가 가리키는 값이 false가 되면
    // 현재 네트워크 요청을 100ms 이내에 abort 하여 상위 루프가 즉시 빠져나가게 함.
    // stopCollection 클릭 시 m_isRunning["platform"] = false 되면 HTTP 요청도 즉시 끊김.
    void setRunFlag(const bool *flag) { m_runFlag = flag; }

private:
    QNetworkAccessManager *m_nam;
    int m_timeout = 30000;
    int m_downloadTimeout = 120000; // 2 minutes for file downloads
    int m_rateLimitMs = 0;
    qint64 m_lastRequestTime = 0;
    const bool *m_runFlag = nullptr;

    HttpResponse executeRequest(QNetworkReply *reply);
    void applyHeaders(QNetworkRequest &request, const QMap<QString, QString> &headers);
    void waitForRateLimit();
};
