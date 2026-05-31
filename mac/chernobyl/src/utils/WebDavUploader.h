#pragma once
// ═════════════════════════════════════════════════════════════════════════
// WebDavUploader — 캡쳐 파일을 NAS (Synology 등) 의 WebDAV 폴더로 업로드
//   백그라운드 큐 — 메인 스레드 블록 X.
//   curl 사용 (macOS/Linux 기본 설치 — 의존성 추가 X).
//
// 사용:
//   WebDavUploader uploader;
//   uploader.setConfig("https://nas.synology.me:5006/captures", "user", "password");
//   uploader.enqueue("/Users/shio/Downloads/file.html");
// ═════════════════════════════════════════════════════════════════════════

#include <QObject>
#include <QString>
#include <QQueue>
#include <QMutex>
#include <QThread>
#include <atomic>

class WebDavUploader : public QObject
{
    Q_OBJECT
public:
    explicit WebDavUploader(QObject *parent = nullptr);
    ~WebDavUploader() override;

    // 설정 — URL, 사용자명, 비번, 로컬 base (이 prefix는 remote URL 만들 때 제거)
    void setConfig(const QString &baseUrl, const QString &user, const QString &pass,
                   const QString &localBase = QString(), bool enabled = true);
    bool isEnabled() const { return m_enabled && !m_baseUrl.isEmpty(); }

    // 로컬 경로 → 자동으로 remote URL 계산 → PUT 업로드 큐에 추가
    //   localPath: 절대 경로 (예: /Users/shio/Downloads/twitter/user/captures/x.html)
    //   localBase 가 prefix면 제거하고 baseUrl 에 붙임
    void enqueue(const QString &localPath);

    // 큐 비우기 (중지)
    void clear();

    int queueSize() const;
    int uploadedCount() const { return m_uploadedCount.load(); }
    int failedCount() const { return m_failedCount.load(); }

signals:
    // 외부에서 로그 받기 위함 (MiyoBackend::log 로 라우팅)
    void logMessage(const QString &message, const QString &type);

private:
    void workerLoop();

    QString m_baseUrl;      // e.g. https://nas.synology.me:5006/captures
    QString m_user;
    QString m_pass;
    QString m_localBase;    // e.g. /Users/shio/Downloads (이 prefix 제거)
    bool m_enabled = false;

    QQueue<QString> m_queue;
    mutable QMutex m_mutex;
    QThread *m_worker = nullptr;
    std::atomic<bool> m_stop{false};

    std::atomic<int> m_uploadedCount{0};
    std::atomic<int> m_failedCount{0};
};
