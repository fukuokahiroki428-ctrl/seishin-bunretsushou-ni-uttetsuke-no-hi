#include "core/Common.h"
#include "HttpClient.h"
#include "FileHelper.h"
#include <QUrl>
#include <QTimer>
#include <QElapsedTimer>
#include <atomic>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QDateTime>
#include <QProcess>
#include <QCoreApplication>
#include <QDebug>

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

// 다 받은 조각(.dl_<sha1>.part)을 진짜 이름으로 바꾼다.
// ★ 윈도우는 막 닫힌 파일을 백신 · 색인기 · 탐색기 미리보기가 잠깐 잡고 있으면 rename 이 실패한다.
//   곧 풀리는 경우가 많다 — 한 번만 해 보고 실패로 끝내면 다 받은 파일을 지우고 다시 받게 된다.
//   FileHelper::moveFileSafe 와 같은 재시도(120ms × 5). 맥 · 리눅스는 잠금 때문에 실패하지 않으므로 한 번만.
static bool replacePart(const QString &part, const QString &dst)
{
    // 진짜 이름에 0 바이트 잔재가 있으면 QFile::rename 이 실패하므로 먼저 치운다
    // (size>0 이면 호출부 맨 위에서 이미 돌아갔다 — 여기 오는 것은 '없음' 이거나 0 바이트뿐이다).
    QFile::remove(dst);
    for (int attempt = 0; attempt < 5; ++attempt) {
        if (QFile::rename(part, dst)) return true;
#ifdef Q_OS_WIN
        QThread::msleep(120);      // 잠금이 풀릴 시간을 준다
        QFile::remove(dst);
#else
        break;
#endif
    }
    return false;
}


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
    QThread *const mainThread = QCoreApplication::instance()->thread();
    if (QThread::currentThread() != mainThread) {
        // ★ 만든 스레드가 이미 사라진 NAM 이 온다.
        //
        //   (예전엔) HanishikiBackend::runTwitterCollection 이 수집 워커 스레드에서
        //     delete m_twitterCollector; m_twitterCollector = new TwitterCollector(this);
        //   를 했다 — 지금은 메인에서 만든다(윈도우 9271c57 을 옮김). 병렬 모드의 지역
        //   collector 와 adoptHttpToCurrentThread 가 만든 것은 여전히 워커 소속이다. 그러면 TwitterCollector 의 HttpClient 와 그 안의 NAM 이 그 워커
        //   스레드 소속으로 태어난다. 워커가 끝나면 스레드는 사라지는데 객체는
        //   멤버라 살아남는다 — 그때부터 nam->thread() 가 0 이다.
        //   다음 수집 때 '다른' 워커가 이 소멸자를 부르면, Qt 는 "스레드 없는 객체는
        //   현재 스레드로만 옮길 수 있다" 며 moveToThread 를 거절한다.
        //     QObject::moveToThread: Current thread (...) is not the object's thread (0x0).
        //   거절당하면 바로 아래 deleteLater 가 죽은 스레드의 큐로 들어가 영영 처리되지
        //   않는다. NAM 이 수집 한 번에 하나씩 샌다 — 윈도우 기계에서 실측했다.
        //   内閣会 30분 폴링마다 하나씩, 로그에 같은 경고가 248번 쌓여 있었다.
        //   (SIGBUS 를 막는 위 규칙 자체는 그대로다 — 워커에서 직접 delete 하지 않는다)
        //
        //   그래서 두 걸음으로 옮긴다. 스레드가 없으면 먼저 현재 스레드로 데려오고
        //   (Qt 가 허용하는 유일한 경우다), 그 다음 메인으로 보낸다.
        if (!m_nam->thread())
            m_nam->moveToThread(QThread::currentThread());
        m_nam->moveToThread(mainThread);
        QMetaObject::invokeMethod(m_nam, &QObject::deleteLater);
    } else {
        // ★ 메인에서 지울 때도, 만든 스레드가 이미 사라진 NAM 이 올 수 있다.
        //   TwitterCollector 가 메인 소속이 되면서 그 소멸자(=m_http 정리)가 메인에서 도는데,
        //   그때의 m_http 는 마지막 collect()/checkNewPosts() 워커가 만든 것이고 그 워커는
        //   이미 끝났다. 그대로 delete 하면 QObject 소멸자가 '다른 스레드의 타이머' 를 두고
        //   경고를 낸다. 스레드 없는 객체는 현재 스레드로 데려올 수 있으므로(Qt 가 허용하는
        //   유일한 경우) 메인으로 먼저 옮기고 지운다. (메인에서 지우는 것이라 위의 SIGBUS
        //   규칙과도 어긋나지 않는다)
        if (!m_nam->thread())
            m_nam->moveToThread(mainThread);
        delete m_nam;
    }
    m_nam = nullptr;
}

// 진행 중인 요청 수 — 앱이 끝날 때 waitIdle 이 0 이 되기를 기다린다
static std::atomic<int> s_inFlight{0};
namespace {
struct InFlight {
    InFlight() { s_inFlight.fetch_add(1, std::memory_order_acq_rel); }
    ~InFlight() { s_inFlight.fetch_sub(1, std::memory_order_acq_rel); }
};
}

bool HttpClient::waitIdle(int msecs)
{
    QElapsedTimer t;
    t.start();
    while (s_inFlight.load(std::memory_order_acquire) > 0 && t.elapsed() < msecs)
        QThread::msleep(20);
    return s_inFlight.load(std::memory_order_acquire) == 0;
}

HttpResponse HttpClient::get(const QString &url, const QMap<QString, QString> &headers)
{
    if (Common::appQuitting()) { HttpResponse r; r.error = QStringLiteral("App quitting"); return r; }
    InFlight inFlight;
    waitForRateLimit();

    QNetworkRequest request{QUrl(url)};
    applyHeaders(request, headers);

    QNetworkReply *reply = m_nam->get(request);
    return executeRequest(reply);
}

HttpResponse HttpClient::post(const QString &url, const QByteArray &body,
                               const QMap<QString, QString> &headers)
{
    if (Common::appQuitting()) { HttpResponse r; r.error = QStringLiteral("App quitting"); return r; }
    InFlight inFlight;
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
    if (Common::appQuitting()) return false;
    InFlight inFlight;
    waitForRateLimit();

    QNetworkRequest request{QUrl(url)};
    applyHeaders(request, headers);

    QNetworkReply *reply = m_nam->get(request);

    // Stream directly to file to avoid loading entire file into memory
    // ★ 받는 동안은 같은 폴더의 숨은 조각 파일에 쓰고, 끝까지 받아 아래 검사(상태·Content-Length·쓰기 오류)를
    //   통과한 뒤에만 진짜 이름으로 바꾼다. 진짜 이름에 바로 쓰면 앱이 강제 종료·크래시·전원 차단으로
    //   죽을 때(아래 정리 코드가 돌지 못한다) 잘린 파일이 진짜 이름으로 남고, 맨 위 이어받기
    //   검사(size>0)가 그것을 영원히 완성본으로 본다.
    //   조각 이름은 '<진짜 이름>.part' 로 짓지 않는다 — 트윗 미디어 이름은 UTF-8 255바이트 한계까지 채워
    //   짓는다(TwitterCollector::downloadTweetMedia). 5바이트만 붙여도 바이트로 세는 저장소(리눅스 NAS·SMB)
    //   에서는 "File name too long" 으로 열리지 않아 그 파일을 영영 못 받는다. 진짜 이름의 해시로 짧게 짓는다 —
    //   같은 파일은 늘 같은 조각 이름이라, 강제 종료로 남은 조각은 다음 판이 지우고 새로 받는다.
    const QFileInfo targetInfo(filePath);
    const QString partPath = targetInfo.absolutePath() + QStringLiteral("/.dl_")
        + QString::fromLatin1(QCryptographicHash::hash(targetInfo.fileName().toUtf8(),
                                                       QCryptographicHash::Sha1).toHex().left(20))
        + QStringLiteral(".part");
    QFile::remove(partPath);
    QFile file(partPath);
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
    // ★ 앱이 끝나는 중이어도 멈춘다(Common::appQuitting) — 그래서 m_runFlag 가 없어도 늘 돈다.
    //   뒷일 스레드가 종료 중에 요청을 붙들고 있으면 Qt 를 치우는 사이 죽는다(2026-10-06 충돌 보고).
    {
        cancelPoll.setInterval(100);
        QObject::connect(&cancelPoll, &QTimer::timeout, [&]() {
            if ((m_runFlag && !*m_runFlag) || Common::appQuitting()) {
                cancelled = true;
                reply->abort();
            }
        });
        cancelPoll.start();
    }

    timer.start(m_downloadTimeout);
    loop.exec();
    cancelPoll.stop();
    // ★ 받기도 시간 초과도 아닌데 루프가 끝났다면 바깥에서 끝낸 것이다(앱이 끝날 때 QCoreApplication::exit 는 주 스레드의
    //   모든 루프를 끝낸다). 그때를 '다 받음' 으로 읽으면 잘린 자료가 성공이 된다 — 취소로 다룬다.
    if (!cancelled && timer.isActive() && !reply->isFinished()) { cancelled = true; reply->abort(); }

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
        QFile::remove(partPath);
        success = false;
    } else {
        // 0 바이트 잔재 치우기 · 윈도우 잠금 재시도는 replacePart 설명 참고.
        if (!replacePart(partPath, filePath)) {
            QFile::remove(partPath);
            success = false;
        }
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
    if (Common::appQuitting()) return result;
    InFlight inFlight;
    waitForRateLimit();

    QNetworkRequest request{QUrl(url)};
    applyHeaders(request, headers);

    QNetworkReply *reply = m_nam->get(request);

    // ★ downloadFile 과 같은 까닭·같은 이름 규칙 — 숨은 조각 파일에 받고, 성공했을 때만 진짜 이름으로 바꾼다.
    const QFileInfo targetInfo(filePath);
    const QString partPath = targetInfo.absolutePath() + QStringLiteral("/.dl_")
        + QString::fromLatin1(QCryptographicHash::hash(targetInfo.fileName().toUtf8(),
                                                       QCryptographicHash::Sha1).toHex().left(20))
        + QStringLiteral(".part");
    QFile::remove(partPath);
    QFile file(partPath);
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
    // ★ 앱이 끝나는 중이어도 멈춘다(Common::appQuitting) — 그래서 m_runFlag 가 없어도 늘 돈다.
    //   뒷일 스레드가 종료 중에 요청을 붙들고 있으면 Qt 를 치우는 사이 죽는다(2026-10-06 충돌 보고).
    {
        cancelPoll.setInterval(100);
        QObject::connect(&cancelPoll, &QTimer::timeout, [&]() {
            if ((m_runFlag && !*m_runFlag) || Common::appQuitting()) {
                cancelled = true;
                reply->abort();
            }
        });
        cancelPoll.start();
    }

    timer.start(m_downloadTimeout);
    loop.exec();
    cancelPoll.stop();
    // ★ 받기도 시간 초과도 아닌데 루프가 끝났다면 바깥에서 끝낸 것이다(앱이 끝날 때 QCoreApplication::exit 는 주 스레드의
    //   모든 루프를 끝낸다). 그때를 '다 받음' 으로 읽으면 잘린 자료가 성공이 된다 — 취소로 다룬다.
    if (!cancelled && timer.isActive() && !reply->isFinished()) { cancelled = true; reply->abort(); }

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
        QFile::remove(partPath);
        result.success = false;
    } else {
        // 0 바이트 잔재 치우기 · 윈도우 잠금 재시도 — replacePart 설명 참고.
        if (!replacePart(partPath, filePath)) {
            QFile::remove(partPath);
            result.success = false;
        }
    }

    if (result.success) {
        FileHelper::setDownloadMeta(filePath, url);
    }

    reply->deleteLater();
    m_lastRequestTime = QDateTime::currentMSecsSinceEpoch();
    return result;
}

// ★ 프록시를 켜 두었으면(전역 설정 · 이 스레드의 계정 프록시 — Common::setThreadProxy) 이 클라이언트도 그 길로 나간다.
//   생성자가 부른다. 여기가 빠지면 API 요청만 진짜 IP 로 새어 '한 세션 두 IP' 가 된다(맥 ueno 와 같은 본문).
//   (윈도우의 옛 setProxyUrl — 프로필 URL 을 호출부가 직접 넣던 것 — 은 이것과 같은 일이라 지웠다.
//    프록시는 이제 Common 한 곳에서만 정하고, 계정별은 Common::setThreadProxy 로 이 스레드에 건다.)
void HttpClient::applyGlobalProxy()
{
    const QString u = Common::proxyUrl();
    if (u.isEmpty()) {
        // ★ 출구가 있어야 하는데 주소를 못 얻었으면(HTTPS 프록시의 중계기를 못 띄움) 닫힌 길을 건다
        //   (127.0.0.1:9 — 아무것도 듣지 않는다). 그냥 NoProxy 로 두면 이 요청만 직접 연결로 샌다.
        if (Common::proxyEnabled())
            m_nam->setProxy(QNetworkProxy(QNetworkProxy::Socks5Proxy, QStringLiteral("127.0.0.1"), 9));
        else
            m_nam->setProxy(QNetworkProxy::NoProxy);
        return;
    }
    const QUrl url(u);
    // ★ 프로필이 HTTP 프록시일 수도 있다(윈도우 HttpClient::setProxyUrl 과 같은 판정).
    //   HTTPS 프록시는 여기까지 오지 않는다 — Qt 에는 '프록시까지 TLS' 종류가 없어서
    //   Common::proxyUrl 이 중계기(socks5://127.0.0.1) 주소로 바꿔 준다.
    const bool socks = url.scheme().startsWith(QLatin1String("socks"));
    QNetworkProxy p(socks ? QNetworkProxy::Socks5Proxy : QNetworkProxy::HttpProxy,
                    url.host(), quint16(url.port(socks ? 1080 : 8080)));
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
    // ★ 앱이 끝나는 중이어도 멈춘다(Common::appQuitting) — 그래서 m_runFlag 가 없어도 늘 돈다.
    //   뒷일 스레드가 종료 중에 요청을 붙들고 있으면 Qt 를 치우는 사이 죽는다(2026-10-06 충돌 보고).
    {
        cancelPoll.setInterval(100);
        QObject::connect(&cancelPoll, &QTimer::timeout, [&]() {
            if ((m_runFlag && !*m_runFlag) || Common::appQuitting()) {
                cancelled = true;
                reply->abort();
            }
        });
        cancelPoll.start();
    }

    timer.start(m_timeout);
    loop.exec();
    cancelPoll.stop();
    // ★ 받기도 시간 초과도 아닌데 루프가 끝났다면 바깥에서 끝낸 것이다(앱이 끝날 때 QCoreApplication::exit 는 주 스레드의
    //   모든 루프를 끝낸다). 그때를 '다 받음' 으로 읽으면 잘린 자료가 성공이 된다 — 취소로 다룬다.
    if (!cancelled && timer.isActive() && !reply->isFinished()) { cancelled = true; reply->abort(); }

    if (cancelled) {
        response.error = "Cancelled";
    } else if (timer.isActive()) {
        timer.stop();
        response.statusCode = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        response.http2 = reply->attribute(QNetworkRequest::Http2WasUsedAttribute).toBool();
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
    // ★ Qt 의 기본 쿠키 항아리(QNetworkCookieJar)는 응답의 Set-Cookie 를 저장해 두었다가, 같은
    //   NAM 의 '두 번째' 요청부터 우리가 직접 넣은 Cookie 헤더를 통째로 갈아 끼운다 — 병합이
    //   아니라 교체다. 인스타는 첫 응답에 Set-Cookie: csrftoken·mid 를 주므로, 곧바로 다시 묻는
    //   요청은 sessionid·ds_user_id 가 빠진 채 나갔고, 인스타는 그 '로그인 안 된 요청' 에 401 을
    //   줬다. 그 401 을 우리는 '세션이 죽었다' 로 읽었다(맥이 찾음, ueno 501c924).
    //   윈도우 Qt 6.7.3 에서도 따로 재 봤다(2026-09-24):
    //       넣은 것      sessionid=MINE123; ds_user_id=42
    //       서버가 받은 것 csrftoken=JAR; mid=JAR          ← 우리 쿠키가 통째로 사라짐
    //       (아래 고침 뒤) sessionid=MINE123; ds_user_id=42 ← 그대로 감
    //   인스타만이 아니다 — 쿠키를 직접 넣는 수집기(팬박스·픽시브·스핀스핀·애스크드…)가 같은
    //   HttpClient 로 두 번째 요청을 보내는 순간부터 로그인 없이 돌 수 있었다.
    //   → Cookie 를 직접 준 요청은 항아리를 읽지도 채우지도 않는다. 안 준 요청은 예전 그대로.
    if (headers.contains("Cookie")) {
        request.setAttribute(QNetworkRequest::CookieLoadControlAttribute, QNetworkRequest::Manual);
        request.setAttribute(QNetworkRequest::CookieSaveControlAttribute, QNetworkRequest::Manual);
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
