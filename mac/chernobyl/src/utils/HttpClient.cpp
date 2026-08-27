#include "HttpClient.h"
#include <QUrl>
#include "core/Common.h"
#include <QFileInfo>
#include "FileHelper.h"
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
    // ★ 프록시를 켜 두었으면 이 클라이언트도 그 길로 나간다.
    //   여기가 빠지면 API 요청만 진짜 IP 로 새어 '한 세션 두 IP' 가 된다.
    applyGlobalProxy();

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
    // ★ 핸드오프 F: 이어받기 — 이미 받은(존재 + size>0) 파일은 스킵. (실패/빈 파일은 항상 지워지므로 안전)
    { QFileInfo fi(filePath); if (fi.exists() && fi.size() > 0) return true; }
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
    // ★ write 의 반환값을 본다. 디스크가 차면 조용히 모자라게 쓰이고, 그 파일이
    //   '받아 둔 것' 으로 남는다(아래 이어받기 검사가 size>0 만 보기 때문).
    bool writeFailed = false;
    connect(reply, &QNetworkReply::readyRead, [&]() {
        const QByteArray chunk = reply->readAll();
        if (file.write(chunk) != chunk.size()) writeFailed = true;
    });

    connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);

    // 중지 플래그 폴링 — 파일 다운로드 중에도 중단 가능하게
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
        // 중지 요청 — 부분 파일은 곧 아래에서 삭제됨
    } else if (timer.isActive()) {
        timer.stop();
        // Write any remaining data
        const QByteArray remaining = reply->readAll();
        if (!remaining.isEmpty() && file.write(remaining) != remaining.size()) writeFailed = true;

        int statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        success = (reply->error() == QNetworkReply::NoError) && (statusCode >= 200 && statusCode < 300);

        // ★ 서버가 알려준 크기와 실제로 받은 크기를 대조한다.
        //   서버나 중간 장비가 연결을 일찍 끊어도 상태 코드는 200 이고 error 도
        //   NoError 로 오는 경우가 있다. 그러면 잘린 파일이 '완성본' 으로 남고,
        //   맨 위 이어받기 검사(size>0 이면 건너뜀)가 그것을 영원히 완성본으로 본다.
        //   보관 도구에서 가장 나쁜 고장이다 — 몇 년 뒤에나 발견된다.
        //   (압축 전송이면 선언된 길이와 푼 크기가 달라지므로 그때는 대조하지 않는다)
        const QVariant declared = reply->header(QNetworkRequest::ContentLengthHeader);
        const bool encoded = !reply->rawHeader("Content-Encoding").isEmpty();
        if (success && !encoded && declared.isValid() && declared.toLongLong() > 0
            && file.size() != declared.toLongLong()) {
            success = false;
        }
    } else {
        reply->abort();
    }

    if (!file.flush()) writeFailed = true;
    file.close();
    if (writeFailed) success = false;

    // Remove empty/failed files
    if (!success || file.size() == 0) {
        QFile::remove(filePath);
        success = false;
    }

    // macOS 다운로드 메타데이터 (setxattr syscall — 프로세스 스폰 없음)
    if (success) {
        FileHelper::setDownloadMeta(filePath, url);
    }

    reply->deleteLater();
    m_lastRequestTime = QDateTime::currentMSecsSinceEpoch();
    return success;
}

HttpClient::DownloadResult HttpClient::downloadFileEx(const QString &url, const QString &filePath,
                                                      const QMap<QString, QString> &headers)
{
    DownloadResult result;
    // ★ 핸드오프 F: 이어받기 — 이미 받은(존재 + size>0) 파일은 스킵하되,
    //   호출부(중복제거/보안스캔)가 쓰는 sha256/headBytes/contentLength 를 기존 파일에서 재계산해 채운 뒤 return.
    {
        QFileInfo fi(filePath);
        if (fi.exists() && fi.size() > 0) {
            QFile ef(filePath);
            if (ef.open(QIODevice::ReadOnly)) {
                result.headBytes = ef.peek(512);
                QCryptographicHash h(QCryptographicHash::Sha256);
                QByteArray buf;
                while (!(buf = ef.read(1 << 20)).isEmpty()) h.addData(buf);
                ef.close();
                result.sha256 = h.result();
                result.contentLength = fi.size();
                result.statusCode = 200;
                result.success = true;
                return result;   // 동작 동일성 보장 (필드 채워 반환)
            }
        }
    }
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
    // ★ 디스크 쓰기 실패 추적 — NAS 연결 끊김/디스크 가득참이면 write() 가 실패하는데
    //   반환값을 안 보면 "잘린 파일 + success=true" 가 되어 호출부가 정상 산출물로 오인한다.
    bool writeFailed = false;

    QEventLoop loop;
    QTimer timer;
    timer.setSingleShot(true);

    connect(reply, &QNetworkReply::readyRead, [&]() {
        QByteArray chunk = reply->readAll();
        if (file.write(chunk) != chunk.size()) writeFailed = true;
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
        // fallthrough → 아래 파일 삭제
    } else if (timer.isActive()) {
        timer.stop();
        QByteArray remaining = reply->readAll();
        if (!remaining.isEmpty()) {
            if (file.write(remaining) != remaining.size()) writeFailed = true;
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
        // ★ 서버가 알려준 크기와 실제로 받은 크기가 다르면 잘린 파일 → 실패로 처리.
        const QVariant declared = reply->header(QNetworkRequest::ContentLengthHeader);
        if (result.success && declared.isValid() && declared.toLongLong() > 0
            && totalWritten != declared.toLongLong()) {
            result.success = false;
        }
    } else {
        reply->abort();
    }

    if (!file.flush()) writeFailed = true;
    file.close();
    if (writeFailed) result.success = false;

    if (!result.success || totalWritten == 0) {
        QFile::remove(filePath);
        result.success = false;
    }

    if (result.success) {
        FileHelper::setDownloadMeta(filePath, url);
    }

    reply->deleteLater();
    m_lastRequestTime = QDateTime::currentMSecsSinceEpoch();
    return result;
}

void HttpClient::applyGlobalProxy()
{
    const QString u = Common::proxyUrl();
    if (u.isEmpty()) { m_nam->setProxy(QNetworkProxy::NoProxy); return; }
    const QUrl url(u);
    QNetworkProxy p(QNetworkProxy::Socks5Proxy, url.host(), quint16(url.port(1080)));
    if (!url.userName().isEmpty()) { p.setUser(url.userName()); p.setPassword(url.password()); }
    m_nam->setProxy(p);
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

    // 중지 플래그 폴링 — 외부에서 setRunFlag(&isRunning) 해놨으면 100ms마다 확인 후 즉시 abort
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
        response.error = "Cancelled";
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
