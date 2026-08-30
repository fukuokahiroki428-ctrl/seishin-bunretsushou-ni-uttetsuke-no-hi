#include "core/Common.h"
#include "HttpClient.h"
#include "FileHelper.h"
#include <QTimer>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QDateTime>
#include <QProcess>
#include <QCoreApplication>

// 파일을 못 연 이유를 사람이 읽을 수 있게 바꾼다.
// ★ 윈도우는 경로가 260자를 넘으면 "파일을 찾을 수 없습니다" 라고 답한다. 폴더가 멀쩡히
//   있는데도 그렇게 말하므로 원인을 오해하기 딱 좋다.
//   실측: 307자 경로가 errno 2 (No such file or directory) 로 실패했다. 이 기계는
//   LongPathsEnabled 가 0 이고 실행 파일에 longPathAware 선언도 없어 한계가 살아 있다.
//   전에는 두 다운로드 함수 모두 아무 말 없이 false 만 돌려줘서, 로그에 흔적조차 없었다.
static QString openFailureReason(const QString &path, const QString &qtError)
{
#ifdef Q_OS_WIN
    if (path.length() >= 250)
        return QString("경로가 너무 깁니다 (%1자 — 윈도우 한계 260자). "
                       "저장 폴더를 더 짧은 곳으로 옮기거나 긴 경로 지원을 켜 주세요. [%2]")
                .arg(path.length()).arg(qtError);
#endif
    return qtError;
}


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
    // ★ 이어받기(resume) — 이미 받은(완료) 파일이면 재다운로드 없이 스킵.
    //   이 함수는 실패/빈 파일을 항상 지우므로(하단), 존재하는 size>0 파일 = 이전 완료분이다.
    //   덕분에 이 헬퍼를 쓰는 모든 수집기(Twitter/Instagram/Discord/Pixiv/Fanbox/Tumblr/
    //   Asked/SpinSpin)가 중단·재실행 시 이미 받은 미디어를 건너뛰고 새 것만 받는다.
    {
        QFileInfo _fi(filePath);
        if (_fi.exists() && _fi.size() > 0) return true;
    }
    waitForRateLimit();

    QNetworkRequest request{QUrl(url)};
    applyHeaders(request, headers);

    QNetworkReply *reply = m_nam->get(request);

    // Stream directly to file to avoid loading entire file into memory
    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "[HttpClient] 저장할 파일을 열지 못했습니다:"
                   << openFailureReason(filePath, file.errorString()) << "-" << filePath;
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
    // ★ 이어받기 — 이미 받은 파일이면 스킵하되, 호출부(SiteCrawler 의 중복제거/보안스캔)가 쓰는
    //   sha256/headBytes/contentLength 는 기존 파일에서 다시 계산해 채운다(동작 동일성 보장).
    {
        QFileInfo _fi(filePath);
        if (_fi.exists() && _fi.size() > 0) {
            QFile _ef(filePath);
            if (_ef.open(QIODevice::ReadOnly)) {
                result.headBytes = _ef.peek(512);
                QCryptographicHash _h(QCryptographicHash::Sha256);
                if (_h.addData(&_ef)) result.sha256 = _h.result();
                _ef.close();
            }
            result.success = true;
            result.statusCode = 200;
            result.contentLength = _fi.size();
            return result;
        }
    }
    waitForRateLimit();

    QNetworkRequest request{QUrl(url)};
    applyHeaders(request, headers);

    QNetworkReply *reply = m_nam->get(request);

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        qWarning() << "[HttpClient] 저장할 파일을 열지 못했습니다:"
                   << openFailureReason(filePath, file.errorString()) << "-" << filePath;
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

void HttpClient::setProxy(const QString &host, int port, QNetworkProxy::ProxyType type)
{
    QNetworkProxy proxy(type, host, port);
    m_nam->setProxy(proxy);
}

void HttpClient::setProxyUrl(const QString &url)
{
    if (url.trimmed().isEmpty()) { clearProxy(); return; }

    // QUrl 이 사용자·비밀번호까지 풀어 준다. 비밀번호에 @ 나 : 가 들어가도
    // 퍼센트 인코딩돼 있으면 정확히 복원된다.
    const QUrl u(url);
    const QString host = u.host();
    if (host.isEmpty()) { clearProxy(); return; }

    const QString scheme = u.scheme().toLower();
    QNetworkProxy::ProxyType type = QNetworkProxy::HttpProxy;
    int defPort = 8080;
    if (scheme.startsWith("socks")) { type = QNetworkProxy::Socks5Proxy; defPort = 1080; }

    QNetworkProxy proxy(type, host, quint16(u.port(defPort)));
    if (!u.userName().isEmpty()) {
        proxy.setUser(u.userName());
        proxy.setPassword(u.password());
    }
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
        request.setHeader(QNetworkRequest::UserAgentHeader, Common::browserUserAgent());
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
