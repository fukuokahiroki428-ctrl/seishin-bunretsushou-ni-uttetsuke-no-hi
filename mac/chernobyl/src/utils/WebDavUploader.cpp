#include "WebDavUploader.h"
#include <QCoreApplication>
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
    if (m_keyFile == path) return;
    m_keyFile = path;
    m_sftpConfFor.clear();      // 설정이 바뀌었으니 conf 를 다시 만들게 한다
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
int WebDavUploader::curlWithTlsFallback(const QStringList &args, int *httpCode, QString *output)
{
    if (m_insecureOk.load())
        return runCurl(args, true, httpCode, output);

    int rc = runCurl(args, false, httpCode, output);
    if (rc == 60 || rc == 51) {   // 60: 인증서 검증 실패, 51: 호스트명 불일치
        if (!m_warnedInsecure.exchange(true))
            emit logMessage("[WebDAV] 인증서를 검증할 수 없습니다(자체서명 NAS로 보임) — "
                            "이 연결은 암호화는 되지만 서버 신원 확인 없이 진행합니다.", "warning");
        m_insecureOk.store(true);
        rc = runCurl(args, true, httpCode, output);
    }
    return rc;
}


// ═════════════════════════════════════════════════════════════════════════
//  SFTP 전송 — 번들 rclone 사용
//
//  왜 curl 이 아닌가: macOS 기본 curl 은 libssh2 없이 빌드돼 sftp 프로토콜이
//  아예 없다. rclone 은 이미 백업용으로 번들에 실려 있고 sftp 를 지원한다.
// ═════════════════════════════════════════════════════════════════════════
QString WebDavUploader::rclonePath() const
{
    QStringList cands;
#ifdef Q_OS_WIN
    cands << QCoreApplication::applicationDirPath() + "/resources/tools/rclone.exe"
          << QCoreApplication::applicationDirPath() + "/tools/rclone.exe"
          << QCoreApplication::applicationDirPath() + "/rclone.exe";
#else
    cands << QCoreApplication::applicationDirPath() + "/../Resources/tools/rclone"
          << QCoreApplication::applicationDirPath() + "/../../../resources/tools/rclone";
#endif
    for (const QString &c : cands)
        if (QFile::exists(c)) return QFileInfo(c).canonicalFilePath();
    return QString();
}

QString WebDavUploader::ensureSftpConf()
{
    QString url, user, pass, key;
    { QMutexLocker lock(&m_mutex); url = m_baseUrl; user = m_user; pass = m_pass; key = m_keyFile; }

    // 자격증명이 바뀌면 conf 를 다시 만든다 — 안 그러면 옛 비번으로 계속 실패한다.
    const QString sig = url + "\x1f" + user + "\x1f" + key + "\x1f" + QString::number(qHash(pass));
    if (!m_sftpConf.isEmpty() && m_sftpConfFor == sig && QFile::exists(m_sftpConf))
        return m_sftpConf;

    const QString rc = rclonePath();
    if (rc.isEmpty()) return QString();

    // sftp://[user@]host[:port]/경로  →  host, port 를 뽑는다.
    QUrl u(url);
    const QString host = u.host();
    const int port = u.port(22);
    if (host.isEmpty()) return QString();
    if (user.isEmpty() && !u.userName().isEmpty()) user = u.userName();

    // 비밀번호는 rclone 이 요구하는 obscure 형식이어야 한다(평문이면 거부한다).
    // 키 파일이 있으면 비밀번호를 아예 쓰지 않는다 — 저장할 비밀이 없는 쪽이 낫다.
    const bool useKey = !key.isEmpty() && QFile::exists(key);
    QString obscured;
    if (!useKey && !pass.isEmpty()) {
        QProcess obs;
        obs.start(rc, {"obscure", pass});
        obs.waitForFinished(5000);
        obscured = QString::fromUtf8(obs.readAllStandardOutput()).trimmed();
        if (obscured.isEmpty()) return QString();
    }

    const QString path = QDir::tempPath() + "/" + QStringLiteral(APP_NAME_ASCII).toLower() + "_sftp.conf";
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    QString conf = QString("[nas_sftp]\ntype = sftp\nhost = %1\nport = %2\nuser = %3\n")
                       .arg(host).arg(port).arg(user);
    if (useKey)                   conf += "key_file = " + key + "\n";
    else if (!obscured.isEmpty()) conf += "pass = " + obscured + "\n";
    else                          conf += "key_use_agent = true\n";  // 둘 다 없으면 ssh-agent 의 키로
    // 처음 붙는 NAS 의 호스트키를 사람이 확인해 줄 방법이 앱 안에 없다.
    // 물어보면 그 자리에서 멈춰 버리므로, 받아들이되 그 사실을 로그에 남긴다.
    conf += "known_hosts_file =\n";
    f.write(conf.toUtf8());
    f.close();
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);   // 0600

    m_sftpConf = path;
    m_sftpConfFor = sig;
    return path;
}

bool WebDavUploader::sftpUpload(const QString &localPath, const QString &relPath, QString *why)
{
    const QString rc = rclonePath();
    if (rc.isEmpty()) { if (why) *why = "번들 rclone 이 없습니다"; return false; }
    const QString conf = ensureSftpConf();
    if (conf.isEmpty()) { if (why) *why = "SFTP 설정을 만들지 못했습니다(호스트/자격증명 확인)"; return false; }

    QString url; { QMutexLocker lock(&m_mutex); url = m_baseUrl; }
    // sftp://host:port/원격/기준경로 에서 '기준경로' 부분만 떼어낸다.
    QString basePath = QUrl(url).path();
    while (basePath.endsWith('/')) basePath.chop(1);
    QString remote = basePath + "/" + relPath;
    while (remote.startsWith('/')) remote = remote.mid(1);

    QProcess p;
    p.setProcessChannelMode(QProcess::MergedChannels);
    // copyto = 파일 하나를 '이 이름으로' 올린다. 중간 폴더는 rclone 이 알아서 만든다
    // (WebDAV 쪽에서 MKCOL 을 일일이 하던 일이 여기선 필요 없다).
    p.start(rc, {"copyto", localPath, QString("nas_sftp:%1").arg(remote),
                 "--config", conf, "--retries", "1", "--low-level-retries", "3",
                 // ★ --timeout 은 '전송이 멎었을 때' 의 한도라, 접속 자체가 막힌 주소에는
                 //   전혀 듣지 않는다(실측: 없는 IP 로 10분 넘게 매달렸다).
                 //   접속 한도는 --contimeout 으로 따로 줘야 한다.
                 "--contimeout", "20s",
                 "--timeout", "120s", "--stats", "0", "--log-level", "ERROR"});
    if (!p.waitForStarted(5000)) { if (why) *why = "rclone 실행 실패"; return false; }
    if (!p.waitForFinished(600000)) { p.kill(); if (why) *why = "타임아웃(10분)"; return false; }

    if (p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0) return true;
    if (why) {
        const QString out = QString::fromUtf8(p.readAll()).trimmed();
        *why = out.isEmpty() ? QString("rclone 종료코드 %1").arg(p.exitCode())
                             : out.section('\n', -1).left(160);
    }
    return false;
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

        // ── SFTP 면 여기서 갈라진다. 큐·이름 정규화까지는 똑같이 쓰고 전송만 다르다.
        if (isSftp()) {
            bool sok = false; QString swhy;
            for (int attempt = 1; attempt <= 3 && !m_stop.load(); ++attempt) {
                sok = sftpUpload(path, encoded.join('/'), &swhy);
                if (sok) break;
                // 인증·권한 문제는 몇 번을 해도 같다 — 바로 포기하고 사유를 보여 준다.
                if (swhy.contains("permission", Qt::CaseInsensitive)
                    || swhy.contains("auth", Qt::CaseInsensitive)
                    || swhy.contains("denied", Qt::CaseInsensitive)) break;
                if (attempt < 3) {
                    emit logMessage(QString("[SFTP] %1 실패 — %2초 후 재시도 %3/3")
                                        .arg(fi.fileName()).arg(attempt * 3).arg(attempt + 1), "info");
                    for (int s = 0; s < attempt * 3 * 5 && !m_stop.load(); ++s) QThread::msleep(200);
                }
            }
            if (sok) { m_uploadedCount++; emit logMessage(QString("[SFTP] ✓ %1").arg(fi.fileName()), "success"); }
            else     { m_failedCount++;   emit logMessage(QString("[SFTP] ✗ %1 — %2").arg(fi.fileName(), swhy), "warning"); }
            continue;
        }

        // 부모 폴더 생성(MKCOL) — 시놀로지 등은 중간 폴더를 자동 생성하지 않는다.
        //   ★ 이미 만든 폴더는 건너뛴다. 예전엔 파일마다 전부 다시 MKCOL 해서
        //     같은 폴더에 100개 올리면 수백 번 왕복했다.
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

        // PUT 업로드 — 일시적 장애(네트워크 순단·5xx)면 재시도.
        //   예전엔 한 번 실패하면 그대로 버려서 파일이 조용히 유실됐다.
        bool ok = false; int code = 0; QString out; int rc = 0;
        for (int attempt = 1; attempt <= 3 && !m_stop.load(); ++attempt) {
            rc = curlWithTlsFallback({"-T", path, "--max-time", "600", remoteUrl}, &code, &out);
            ok = (rc == 0 && (code == 200 || code == 201 || code == 204));
            if (ok) break;
            // 인증/권한/경로 문제는 재시도해도 소용없다 → 즉시 중단
            if (code == 401 || code == 403 || code == 404 || code == 409 || code == 507) break;
            if (attempt < 3) {
                emit logMessage(QString("[WebDAV] %1 실패(HTTP %2) — %3초 후 재시도 %4/3")
                                    .arg(fi.fileName()).arg(code).arg(attempt * 3).arg(attempt + 1), "info");
                for (int s = 0; s < attempt * 3 * 5 && !m_stop.load(); ++s) QThread::msleep(200);
            }
        }

        if (ok) {
            m_uploadedCount++;
            emit logMessage(QString("[WebDAV] ✓ %1").arg(fi.fileName()), "success");
        } else {
            m_failedCount++;
            QString why;
            switch (code) {
                case 401: why = "인증 실패 — 사용자/비밀번호 확인"; break;
                case 403: why = "권한 없음 — NAS 폴더 쓰기 권한 확인"; break;
                case 404: why = "경로 없음 — WebDAV base URL 확인"; break;
                case 409: why = "상위 폴더 없음(MKCOL 실패)"; break;
                case 507: why = "NAS 저장공간 부족"; break;
                case 0:   why = (rc == -2 ? "타임아웃" : QString("연결 실패(curl %1)").arg(rc)); break;
                default:  why = QString("HTTP %1").arg(code);
            }
            emit logMessage(QString("[WebDAV] ✗ %1 — %2").arg(fi.fileName(), why), "warning");
        }
    }
}
