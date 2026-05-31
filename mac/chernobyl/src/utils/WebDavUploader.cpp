#include "WebDavUploader.h"
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
    if (m_worker) {
        m_worker->quit();
        m_worker->wait(3000);
        delete m_worker;
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
}

void WebDavUploader::enqueue(const QString &localPath)
{
    if (!isEnabled()) return;
    if (localPath.isEmpty() || !QFileInfo(localPath).exists()) return;

    {
        QMutexLocker lock(&m_mutex);
        m_queue.enqueue(localPath);
    }

    // 첫 enqueue 면 워커 스레드 시작
    if (!m_worker) {
        m_worker = QThread::create([this]() { workerLoop(); });
        m_worker->start();
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

void WebDavUploader::workerLoop()
{
    while (!m_stop.load()) {
        QString path;
        {
            QMutexLocker lock(&m_mutex);
            if (m_queue.isEmpty()) {
                // 큐 비면 잠시 후 다시 (sleep + retry)
                lock.unlock();
                QThread::msleep(500);
                // 5초 동안 빈 채로 있으면 워커 종료 — 다음 enqueue 시 다시 시작
                int waited = 0;
                while (waited < 5000 && !m_stop.load()) {
                    QThread::msleep(200);
                    waited += 200;
                    QMutexLocker chk(&m_mutex);
                    if (!m_queue.isEmpty()) break;
                }
                QMutexLocker chk(&m_mutex);
                if (m_queue.isEmpty()) {
                    m_worker = nullptr;  // 다음 enqueue 가 새 워커 만들도록
                    return;
                }
                continue;
            }
            path = m_queue.dequeue();
        }

        QFileInfo fi(path);
        if (!fi.exists()) continue;

        // 로컬 경로 → remote URL 매핑
        // localBase 가 prefix 면 제거하고 baseUrl 에 붙임
        QString relPath = path;
        if (!m_localBase.isEmpty() && relPath.startsWith(m_localBase)) {
            relPath = relPath.mid(m_localBase.length());
        } else {
            // localBase 가 매칭 안 되면 그냥 파일명만
            relPath = "/" + fi.fileName();
        }
        while (relPath.startsWith('/')) relPath = relPath.mid(1);

        // URL 인코딩 — 경로 컴포넌트만 (슬래시는 유지)
        QStringList parts = relPath.split('/');
        QStringList encoded;
        for (const QString &p : parts) {
            encoded << QUrl::toPercentEncoding(p);
        }
        QString remoteUrl = m_baseUrl + "/" + encoded.join('/');

        // 부모 폴더 생성 (MKCOL) — 시놀로지 WebDAV 는 중간 폴더 자동 생성 안 함.
        //   각 dir 단계마다 MKCOL 시도. 이미 있으면 405 (그래도 진행).
        QStringList dirs;
        QString cur = m_baseUrl;
        for (int i = 0; i < encoded.size() - 1; ++i) {
            cur += "/" + encoded[i];
            dirs << cur;
        }
        for (const QString &dirUrl : dirs) {
            QProcess mkcol;
            mkcol.start("curl", {
                "-sS", "-k",  // -k: 자체서명 인증서 허용
                "-X", "MKCOL",
                "-u", m_user + ":" + m_pass,
                "--max-time", "10",
                dirUrl
            });
            mkcol.waitForFinished(15000);
            // 응답 무시 — 폴더 있으면 405, 없었으면 201
        }

        // PUT 업로드
        QProcess put;
        put.setProcessChannelMode(QProcess::MergedChannels);
        QStringList args = {
            "-sS", "-k",
            "-T", path,
            "-u", m_user + ":" + m_pass,
            "--max-time", "600",  // 10분 (대용량 대비)
            "-w", "HTTP %{http_code}\n",
            remoteUrl
        };
        put.start("curl", args);
        bool finished = put.waitForFinished(600 * 1000);
        QString output = QString::fromUtf8(put.readAll()).trimmed();

        if (!finished) {
            m_failedCount++;
            emit logMessage(QString("[WebDAV] 타임아웃: %1").arg(fi.fileName()), "warning");
            continue;
        }

        // HTTP 응답 코드 파싱
        bool ok = false;
        if (output.contains("HTTP 201") || output.contains("HTTP 204") || output.contains("HTTP 200")) {
            ok = true;
        }
        if (ok) {
            m_uploadedCount++;
            emit logMessage(QString("[WebDAV] ✓ %1").arg(fi.fileName()), "success");
        } else {
            m_failedCount++;
            QString errInfo = output.left(150);
            emit logMessage(QString("[WebDAV] ✗ %1: %2").arg(fi.fileName(), errInfo), "warning");
        }
    }
}
