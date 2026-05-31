#include "HttpClient.h"
#include <QTimer>
#include <QFile>
#include <QThread>
#include <QDateTime>
#include <QProcess>
#include <QCoreApplication>

HttpClient::HttpClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(nullptr))  // no parent — prevent double-delete race
{
    // Follow redirects automatically (critical for media CDN URLs)
    m_nam->setRedirectPolicy(QNetworkRequest::NoLessSafeRedirectPolicy);
}

HttpClient::~HttpClient()
{
    if (!m_nam) return;

    // macOS CFSocket 콜백이 메인 스레드 RunLoop에 등록되어 있으므로,
    // 워커 스레드에서 직접 삭제하면 dangling callback → SIGBUS 크래시 발생.
    // NAM을 메인 스레드로 이동시켜서 안전하게 정리.
    if (QThread::currentThread() != QCoreApplication::instance()->thread()) {
        m_nam->moveToThread(QCoreApplication::instance()->thread());
        QMetaObject::invokeMethod(m_nam, &QObject::deleteLater);
    } else {
        delete m_nam;
    }
    m_nam = nullptr;
}

HttpResponse HttpClient::get(const QString &url, const QMap<QString, QString> &headers)
{
    waitForRateLimit();

    QNetworkRequest request{QUrl(url)};
    applyHeaders(request, headers);

    QNetworkReply *reply = m_nam->get(request);
    return executeRequest(reply);
}

HttpResponse HttpClient::post(const QString &url, const QByteArray &body,
                               const QMap<QString, QString> &headers)
{
    waitForRateLimit();

    QNetworkRequest request{QUrl(url)};
    applyHeaders(request, headers);

    QNetworkReply *reply = m_nam->post(request, body);
    return executeRequest(reply);
}

HttpResponse HttpClient::postJson(const QString &url, const QJsonObject &json,
                                   const QMap<QString, QString> &headers)
{
    QMap<QString, QString> hdrs = headers;
    hdrs["Content-Type"] = "application/json";

    QJsonDocument doc(json);
    return post(url, doc.toJson(QJsonDocument::Compact), hdrs);
}

bool HttpClient::downloadFile(const QString &url, const QString &filePath,
                               const QMap<QString, QString> &headers)
{
    waitForRateLimit();

    QNetworkRequest request{QUrl(url)};
    applyHeaders(request, headers);

    QNetworkReply *reply = m_nam->get(request);

    // Stream directly to file to avoid loading entire file into memory
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        reply->abort();
        reply->deleteLater();
        return false;
    }

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

    // Write data as it arrives (streaming)
    connect(reply, &QNetworkReply::readyRead, [&]() {
        file.write(reply->readAll());
    });

    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    // 중지 플래그 폴링 — 다운로드 중 플래그가 false되면 즉시 abort
    QTimer cancelPoll;
    bool cancelled = false;
    if (m_runFlag) {
        cancelPoll.setInterval(100);
        QObject::connect(&cancelPoll, &QTimer::timeout, [&]() {
            if (m_runFlag && !*m_runFlag) {
                cancelled = true;
                reply->abort();
            }
        });
        cancelPoll.start();
    }

    timer.start(m_downloadTimeout);
    loop.exec();
    cancelPoll.stop();

    bool success = false;
    if (cancelled) {
        reply->abort();
    } else if (timer.isActive()) {
        timer.stop();
        // Write any remaining data
        QByteArray remaining = reply->readAll();
        if (!remaining.isEmpty()) file.write(remaining);

        int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        success = (reply->error() == QNetworkReply::NoError) && (statusCode >= 200 && statusCode < 300);
    } else {
        reply->abort();
    }

    file.close();

    // Remove empty/failed files
    if (!success || file.size() == 0) {
        QFile::remove(filePath);
        success = false;
    }

#ifdef Q_OS_MACOS
    // macOS 확장 속성 설정 — "사용자 다운로드"로 인식시켜 시스템 데이터 분류 방지
    if (success) {
        // kMDItemWhereFroms (다운로드 출처)
        QProcess::execute("xattr", {"-w", "com.apple.metadata:kMDItemWhereFroms", url, filePath});
        // kMDItemDownloadedDate (다운로드 날짜) — plist 형식
        QString datePlist = QString("(\"%1\")").arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        QProcess::execute("xattr", {"-w", "com.apple.metadata:kMDItemDownloadedDate", datePlist, filePath});
    }
#endif

    reply->deleteLater();
    m_lastRequestTime = QDateTime::currentMSecsSinceEpoch();
    return success;
}

HttpClient::DownloadResult HttpClient::downloadFileEx(const QString &url, const QString &filePath,
                                                      const QMap<QString, QString> &headers)
{
    DownloadResult result;
    waitForRateLimit();

    QNetworkRequest request{QUrl(url)};
    applyHeaders(request, headers);

    QNetworkReply *reply = m_nam->get(request);

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        reply->abort();
        reply->deleteLater();
        return result;
    }

    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 totalWritten = 0;

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

    connect(reply, &QNetworkReply::readyRead, [&]() {
        QByteArray chunk = reply->readAll();
        file.write(chunk);
        hash.addData(chunk);
        // 첫 512바이트 캡처
        if (totalWritten < 512) {
            int need = qMin((qint64)512 - totalWritten, (qint64)chunk.size());
            result.headBytes.append(chunk.left(need));
        }
        totalWritten += chunk.size();
    });

    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    // 중지 플래그 폴링
    QTimer cancelPoll;
    bool cancelled = false;
    if (m_runFlag) {
        cancelPoll.setInterval(100);
        QObject::connect(&cancelPoll, &QTimer::timeout, [&]() {
            if (m_runFlag && !*m_runFlag) {
                cancelled = true;
                reply->abort();
            }
        });
        cancelPoll.start();
    }

    timer.start(m_downloadTimeout);
    loop.exec();
    cancelPoll.stop();

    if (cancelled) {
        reply->abort();
    } else if (timer.isActive()) {
        timer.stop();
        QByteArray remaining = reply->readAll();
        if (!remaining.isEmpty()) {
            file.write(remaining);
            hash.addData(remaining);
            if (totalWritten < 512) {
                int need = qMin((qint64)512 - totalWritten, (qint64)remaining.size());
                result.headBytes.append(remaining.left(need));
            }
            totalWritten += remaining.size();
        }
        result.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        result.contentType = reply->header(QNetworkRequest::ContentTypeHeader).toString();
        result.contentLength = totalWritten;
        result.sha256 = hash.result();
        result.success = (reply->error() == QNetworkReply::NoError) && (result.statusCode >= 200 && result.statusCode < 300);
    } else {
        reply->abort();
    }

    file.close();

    if (!result.success || totalWritten == 0) {
        QFile::remove(filePath);
        result.success = false;
    }

#ifdef Q_OS_MACOS
    if (result.success) {
        QProcess::execute("xattr", {"-w", "com.apple.metadata:kMDItemWhereFroms", url, filePath});
        QString datePlist = QString("(\"%1\")").arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
        QProcess::execute("xattr", {"-w", "com.apple.metadata:kMDItemDownloadedDate", datePlist, filePath});
    }
#endif

    reply->deleteLater();
    m_lastRequestTime = QDateTime::currentMSecsSinceEpoch();
    return result;
}

void HttpClient::setProxy(const QString &host, int port, QNetworkProxy::ProxyType type)
{
    QNetworkProxy proxy(type, host, port);
    m_nam->setProxy(proxy);
}

void HttpClient::clearProxy()
{
    m_nam->setProxy(QNetworkProxy::NoProxy);
}

void HttpClient::setCookies(const QMap<QString, QString> &cookies)
{
    Q_UNUSED(cookies);
    // Cookies are managed per-request via headers
}

void HttpClient::setRateLimit(int requestsPerSecond)
{
    if (requestsPerSecond > 0) {
        m_rateLimitMs = 1000 / requestsPerSecond;
    } else {
        m_rateLimitMs = 0;
    }
}

HttpResponse HttpClient::executeRequest(QNetworkReply *reply)
{
    HttpResponse response;

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    // 중지 플래그 폴링 — 100ms 마다 m_runFlag 검사해서 false면 즉시 abort
    QTimer cancelPoll;
    bool cancelled = false;
    if (m_runFlag) {
        cancelPoll.setInterval(100);
        QObject::connect(&cancelPoll, &QTimer::timeout, [&]() {
            if (m_runFlag && !*m_runFlag) {
                cancelled = true;
                reply->abort();
            }
        });
        cancelPoll.start();
    }

    timer.start(m_timeout);
    loop.exec();
    cancelPoll.stop();

    if (cancelled) {
        response.error = "Cancelled by user";
    } else if (timer.isActive()) {
        timer.stop();
        response.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.data = reply->readAll();
        if (reply->error() != QNetworkReply::NoError) {
            response.error = reply->errorString();
        }
    } else {
        reply->abort();
        response.error = "Request timed out";
    }

    reply->deleteLater();
    m_lastRequestTime = QDateTime::currentMSecsSinceEpoch();
    return response;
}

void HttpClient::applyHeaders(QNetworkRequest &request, const QMap<QString, QString> &headers)
{
    // Don't set a default User-Agent here; let callers provide their own via headers map
    // This avoids conflicts with platform-specific User-Agents (e.g. twikit Safari UA)
    for (auto it = headers.constBegin(); it != headers.constEnd(); ++it) {
        request.setRawHeader(it.key().toUtf8(), it.value().toUtf8());
    }
    // Fallback if no User-Agent was provided
    if (!headers.contains("User-Agent")) {
        request.setHeader(QNetworkRequest::UserAgentHeader,
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_6_1) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Safari/605.1.15");
    }
}

void HttpClient::waitForRateLimit()
{
    if (m_rateLimitMs <= 0) return;

    qint64 now = QDateTime::currentMSecsSinceEpoch();
    qint64 elapsed = now - m_lastRequestTime;
    if (elapsed < m_rateLimitMs) {
        QThread::msleep(m_rateLimitMs - elapsed);
    }
}
