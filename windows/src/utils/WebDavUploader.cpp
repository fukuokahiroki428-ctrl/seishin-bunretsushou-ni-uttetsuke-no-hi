#include "WebDavUploader.h"
#include <QFile>
#include <QProcess>
#include <QFileInfo>
#include <QUrl>
#include <QThread>
#include <QDir>

WebDavUploader::WebDavUploader(QObject *parent)
    : QObject(parent)
{
}

WebDavUploader::~WebDavUploader()
{
    m_stop.store(true);
    QThread *w = nullptr;
    { QMutexLocker lock(&m_mutex); w = m_worker; m_worker = nullptr; }
    if (w) {
        // 워커는 m_stop 을 보고 스스로 빠져나온다. 실행 중인 curl 이 있으면 최대 몇 초.
        if (!w->wait(8000)) { w->terminate(); w->wait(2000); }
        delete w;
    }
}

void WebDavUploader::setConfig(const QString &baseUrl, const QString &user, const QString &pass,
                                const QString &localBase, bool enabled)
{
    QMutexLocker lock(&m_mutex);
    m_baseUrl = baseUrl;
    while (m_baseUrl.endsWith('/')) m_baseUrl.chop(1);
    m_user = user;
    m_pass = pass;
    m_localBase = localBase;
    while (m_localBase.endsWith('/')) m_localBase.chop(1);
    m_enabled = enabled;
    // 접속 대상이 바뀌면 폴더 캐시·TLS 판정은 무효
    m_madeDirs.clear();
    m_insecureOk.store(false);
}

void WebDavUploader::setSftpKeyFile(const QString &path)
{
    QMutexLocker lock(&m_mutex);
    m_keyFile = path;
}

void WebDavUploader::enqueue(const QString &localPath)
{
    if (!isEnabled()) return;
    if (localPath.isEmpty() || !QFileInfo(localPath).exists()) return;

    QMutexLocker lock(&m_mutex);
    m_queue.enqueue(localPath);
    // 워커가 없을 때만 새로 만든다. m_worker 접근은 항상 뮤텍스 안에서 —
    //   예전엔 워커 스레드가 밖에서 m_worker 를 nullptr 로 덮어써 객체가 새고,
    //   소멸자가 그 스레드를 기다리지 못해 종료 시 크래시(use-after-free) 위험이 있었다.
    if (!m_worker) {
        QThread *old = m_finished;      // 이전에 끝난 워커가 있으면 여기서 정리
        m_finished = nullptr;
        m_worker = QThread::create([this]() { workerLoop(); });
        m_worker->start();
        if (old) { old->wait(2000); delete old; }
    }
}

void WebDavUploader::clear()
{
    QMutexLocker lock(&m_mutex);
    m_queue.clear();
}

int WebDavUploader::queueSize() const
{
    QMutexLocker lock(&m_mutex);
    return m_queue.size();
}

// curl 설정 파일(stdin) 한 줄 escape — 값은 "..." 로 감싸므로 \ 와 " 를 escape.
static QString curlQuote(const QString &s)
{
    QString o = s;
    o.replace('\\', "\\\\");
    o.replace('"', "\\\"");
    return o;
}

// ★ SFTP 지원 — URL 이 sftp:// 로 시작하면 WebDAV 가 아니라 SSH 파일 전송으로 동작한다.
//   curl 이 libssh2 로 sftp 를 지원한다(번들 curl: Protocols ... scp sftp).
//   WebDAV 와 다른 점 세 가지만 갈라주면 나머지 흐름은 그대로 쓴다.
//     1) 폴더 생성이 MKCOL 이 아니다 → --ftp-create-dirs 로 curl 이 중간 폴더를 만든다
//     2) HTTP 상태코드가 없다 → 성공 판정을 curl 종료코드로만 한다
//     3) 실패해도 소용없는 경우가 HTTP 코드가 아니라 curl 코드로 온다(67 로그인 거부 등)
static bool isSftpUrl(const QString &url)
{
    return url.startsWith(QLatin1String("sftp://"), Qt::CaseInsensitive);
}

// curl 실행 — 자격증명은 명령줄(-u)이 아니라 stdin 설정으로 넘긴다.
//   -u 로 넘기면 같은 PC 의 다른 프로세스가 `ps` 로 NAS 비밀번호를 그대로 볼 수 있다.
//   반환: curl 종료코드. httpCode 에 HTTP 상태코드(없으면 0).
int WebDavUploader::runCurl(const QStringList &args, bool insecure, int *httpCode, QString *output)
{
    QString user, pass;
    { QMutexLocker lock(&m_mutex); user = m_user; pass = m_pass; }

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    QStringList full = {"-sS", "-K", "-"};      // -K - : 설정을 stdin 에서 읽음
    full += args;
    full << "-w" << "\n__HTTPCODE__%{http_code}\n";
    p.start("curl", full);
    if (!p.waitForStarted(5000)) { if (httpCode) *httpCode = 0; return -1; }

    QString cfg;
    if (!user.isEmpty()) cfg += QString("user = \"%1:%2\"\n").arg(curlQuote(user), curlQuote(pass));
    if (insecure)        cfg += "insecure\n";
    p.write(cfg.toUtf8());
    p.closeWriteChannel();

    const bool fin = p.waitForFinished(600 * 1000);
    const QString out = QString::fromUtf8(p.readAll());
    if (output) *output = out;
    if (!fin) { p.kill(); p.waitForFinished(2000); if (httpCode) *httpCode = 0; return -2; }

    // HTTP 코드는 표식이 붙은 마지막 줄에서만 읽는다(본문에 "HTTP 200" 이 있어도 오탐 없게).
    int code = 0;
    const int idx = out.lastIndexOf("__HTTPCODE__");
    if (idx >= 0) code = out.mid(idx + 12).trimmed().left(3).toInt();
    if (httpCode) *httpCode = code;
    return p.exitCode();
}

// 보안 연결 우선 — 자체서명 인증서(curl 60)면 1회만 경고하고 이후 그 호스트는 insecure 로.
//   ★ SFTP 도 같은 60 으로 온다. curl 은 SSH 호스트키를 known_hosts 로 검증하는데,
//     NAS 를 처음 붙이면 등록돼 있지 않아 실패한다. 같은 폴백을 타되 경고 문구만 갈라준다.
int WebDavUploader::curlWithTlsFallback(const QStringList &args, int *httpCode, QString *output)
{
    if (m_insecureOk.load())
        return runCurl(args, true, httpCode, output);

    int rc = runCurl(args, false, httpCode, output);
    if (rc == 60 || rc == 51) {   // 60: 인증서/호스트키 검증 실패, 51: 호스트명 불일치
        bool sftp; { QMutexLocker lock(&m_mutex); sftp = isSftpUrl(m_baseUrl); }
        if (!m_warnedInsecure.exchange(true))
            emit logMessage(sftp
                ? QStringLiteral("[SFTP] 서버의 SSH 호스트키를 확인할 수 없습니다"
                                 "(known_hosts 에 없는 NAS로 보임) — 전송은 암호화되지만 "
                                 "서버 신원 확인 없이 진행합니다.")
                : QStringLiteral("[WebDAV] 인증서를 검증할 수 없습니다(자체서명 NAS로 보임) — "
                                 "이 연결은 암호화는 되지만 서버 신원 확인 없이 진행합니다."), "warning");
        m_insecureOk.store(true);
        rc = runCurl(args, true, httpCode, output);
    }
    return rc;
}

void WebDavUploader::workerLoop()
{
    while (!m_stop.load()) {
        QString path;
        {
            QMutexLocker lock(&m_mutex);
            if (m_queue.isEmpty()) {
                lock.unlock();
                // 5초간 조용하면 워커 종료(다음 enqueue 가 다시 만든다).
                int waited = 0;
                while (waited < 5000 && !m_stop.load()) {
                    QThread::msleep(200);
                    waited += 200;
                    QMutexLocker chk(&m_mutex);
                    if (!m_queue.isEmpty()) break;
                }
                QMutexLocker chk(&m_mutex);
                if (m_queue.isEmpty()) {
                    // ★ 자기 자신을 삭제하지 않는다 — m_finished 로 넘겨 다음 enqueue 가 정리.
                    m_finished = m_worker;
                    m_worker = nullptr;
                    return;
                }
                continue;
            }
            path = m_queue.dequeue();
        }

        QFileInfo fi(path);
        if (!fi.exists()) continue;

        // 로컬 경로 → remote URL 매핑 (localBase prefix 제거)
        // ★ Windows 경로 규칙 대응 — 구분자('\\' vs '/')와 대소문자가 달라도 같은 폴더다.
        //   비교 전에 '/' 로 통일하고, Windows 에선 대소문자를 무시한다. 또 경계가 '/' 인지
        //   확인해 "…/Photo" 가 "…/PhotoBackup" 에 잘못 매칭되는 것을 막는다.
        QString base, relPath = QDir::fromNativeSeparators(path);
        { QMutexLocker lock(&m_mutex); base = m_baseUrl;
          QString lb = QDir::fromNativeSeparators(m_localBase);
          while (lb.endsWith('/')) lb.chop(1);
#ifdef Q_OS_WIN
          const Qt::CaseSensitivity cs = Qt::CaseInsensitive;
#else
          const Qt::CaseSensitivity cs = Qt::CaseSensitive;
#endif
          if (!lb.isEmpty() && relPath.startsWith(lb, cs)
              && (relPath.length() == lb.length() || relPath.at(lb.length()) == QLatin1Char('/')))
              relPath = relPath.mid(lb.length());
          else relPath = "/" + fi.fileName(); }
        while (relPath.startsWith('/')) relPath = relPath.mid(1);

        // ★ NAS 로 보내는 이름은 NFC 로 통일.
        //   macOS 는 파일명을 NFD(자모 분리)로 저장한다 — 그대로 올리면 리눅스 NAS 에서
        //   '한글' 이 분해된 형태로 남아 검색·정렬이 어긋나고 일부 앱은 파일을 못 찾는다.
        //   (이 수정 이전에 저장된 기존 파일도 업로드 시점에 바로잡힌다.)
        QStringList encoded;
        for (const QString &p : relPath.split('/'))
            encoded << QString::fromUtf8(
                QUrl::toPercentEncoding(p.normalized(QString::NormalizationForm_C)));
        const QString remoteUrl = base + "/" + encoded.join('/');

        const bool sftp = isSftpUrl(base);

        // 부모 폴더 생성(MKCOL) — 시놀로지 등은 중간 폴더를 자동 생성하지 않는다.
        //   ★ 이미 만든 폴더는 건너뛴다. 예전엔 파일마다 전부 다시 MKCOL 해서
        //     같은 폴더에 100개 올리면 수백 번 왕복했다.
        //   ★ SFTP 에는 MKCOL 이 없다. 아래 업로드에서 --ftp-create-dirs 로 curl 이
        //     중간 폴더를 만들므로 이 왕복 자체가 필요 없다.
        if (!sftp) {
            QString cur = base;
            for (int i = 0; i < encoded.size() - 1; ++i) {
                cur += "/" + encoded[i];
                { QMutexLocker lock(&m_mutex); if (m_madeDirs.contains(cur)) continue; }
                int code = 0;
                curlWithTlsFallback({"-X", "MKCOL", "--max-time", "15", cur}, &code, nullptr);
                // 201=생성, 405=이미 있음 → 둘 다 성공으로 보고 캐시
                if (code == 201 || code == 405 || code == 301 || code == 200) {
                    QMutexLocker lock(&m_mutex); m_madeDirs.insert(cur);
                }
            }
        }

        // PUT 업로드 — 일시적 장애(네트워크 순단·5xx)면 재시도.
        //   예전엔 한 번 실패하면 그대로 버려서 파일이 조용히 유실됐다.
        bool ok = false; int code = 0; QString out; int rc = 0;
        QStringList putArgs = {"-T", path, "--max-time", "600"};
        //   ★ SFTP: 중간 폴더를 curl 이 만들게 한다(WebDAV 의 MKCOL 을 대신한다).
        if (sftp) {
            putArgs << "--ftp-create-dirs";
            // 개인키가 지정돼 있으면 그것으로 붙는다 — 비밀번호를 저장하지 않아도 된다.
            QString key; { QMutexLocker lock(&m_mutex); key = m_keyFile; }
            if (!key.isEmpty() && QFile::exists(key)) putArgs << "--key" << key;
        }
        putArgs << remoteUrl;
        for (int attempt = 1; attempt <= 3 && !m_stop.load(); ++attempt) {
            rc = curlWithTlsFallback(putArgs, &code, &out);
            //   ★ SFTP 에는 HTTP 상태코드가 없다(code 는 늘 0) → 종료코드로만 판정한다.
            ok = sftp ? (rc == 0)
                      : (rc == 0 && (code == 200 || code == 201 || code == 204));
            if (ok) break;
            // 인증/권한/경로 문제는 재시도해도 소용없다 → 즉시 중단
            //   SFTP 는 curl 종료코드로 온다: 67 로그인 거부, 9 접근 거부, 78 파일 없음
            if (sftp) { if (rc == 67 || rc == 9 || rc == 78) break; }
            else if (code == 401 || code == 403 || code == 404 || code == 409 || code == 507) break;
            if (attempt < 3) {
                emit logMessage(QString("[%1] %2 실패(%3) — %4초 후 재시도 %5/3")
                                    .arg(sftp ? "SFTP" : "WebDAV", fi.fileName(),
                                         sftp ? QString("curl %1").arg(rc) : QString("HTTP %1").arg(code))
                                    .arg(attempt * 3).arg(attempt + 1), "info");
                for (int s = 0; s < attempt * 3 * 5 && !m_stop.load(); ++s) QThread::msleep(200);
            }
        }

        const QString proto = sftp ? QStringLiteral("SFTP") : QStringLiteral("WebDAV");
        if (ok) {
            m_uploadedCount++;
            emit logMessage(QString("[%1] ✓ %2").arg(proto, fi.fileName()), "success");
        } else {
            m_failedCount++;
            QString why;
            if (sftp) {
                // SFTP 는 HTTP 코드가 없다 — curl 종료코드로 원인을 가른다.
                switch (rc) {
                    case 67: why = "인증 실패 — 사용자/비밀번호(또는 SSH 키) 확인"; break;
                    case 9:  why = "권한 없음 — NAS 폴더 쓰기 권한 확인"; break;
                    case 78: why = "경로 없음 — sftp:// URL 의 경로 확인"; break;
                    case 7:  why = "연결 실패 — 호스트·포트 확인(기본 22)"; break;
                    case 28: why = "타임아웃"; break;
                    case -2: why = "타임아웃"; break;
                    default: why = QString("전송 실패(curl %1)").arg(rc);
                }
            } else {
                switch (code) {
                    case 401: why = "인증 실패 — 사용자/비밀번호 확인"; break;
                    case 403: why = "권한 없음 — NAS 폴더 쓰기 권한 확인"; break;
                    case 404: why = "경로 없음 — WebDAV base URL 확인"; break;
                    case 409: why = "상위 폴더 없음(MKCOL 실패)"; break;
                    case 507: why = "NAS 저장공간 부족"; break;
                    case 0:   why = (rc == -2 ? "타임아웃" : QString("연결 실패(curl %1)").arg(rc)); break;
                    default:  why = QString("HTTP %1").arg(code);
                }
            }
            emit logMessage(QString("[%1] ✗ %2 — %3").arg(proto, fi.fileName(), why), "warning");
        }
    }
}
