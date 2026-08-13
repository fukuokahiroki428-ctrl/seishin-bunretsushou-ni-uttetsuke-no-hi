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
#include <QSet>
#include <QStringList>
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
    // SFTP 에서 비밀번호 대신 쓸 SSH 개인키. 비워 두면 비밀번호로 붙는다.
    // 키를 쓰면 저장해 둘 비밀번호 자체가 없어지므로 가능하면 이쪽이 낫다.
    void setSftpKeyFile(const QString &path);

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
    // curl 실행 — 자격증명은 stdin 설정으로 전달(명령줄 -u 는 ps 로 비밀번호가 노출된다).
    int runCurl(const QStringList &args, bool insecure, int *httpCode, QString *output);
    // TLS 검증 우선, 자체서명 인증서면 1회 경고 후 그 세션 동안만 검증 생략.
    int curlWithTlsFallback(const QStringList &args, int *httpCode, QString *output);

    QString m_baseUrl;      // e.g. https://nas.synology.me:5006/captures
    QString m_user;
    QString m_pass;
    QString m_keyFile;      // SFTP SSH 개인키(선택)
    QString m_localBase;    // e.g. /Users/shio/Downloads (이 prefix 제거)
    bool m_enabled = false;

    QQueue<QString> m_queue;
    mutable QMutex m_mutex;
    QThread *m_worker = nullptr;
    QThread *m_finished = nullptr;      // 종료한 워커 — 다음 enqueue 가 정리(자기 자신 delete 금지)
    QSet<QString> m_madeDirs;           // MKCOL 완료한 폴더 URL 캐시 (파일마다 반복 방지)
    std::atomic<bool> m_insecureOk{false};   // 자체서명이라 검증 생략하기로 판정됨
    std::atomic<bool> m_warnedInsecure{false};
    std::atomic<bool> m_stop{false};

    std::atomic<int> m_uploadedCount{0};
    std::atomic<int> m_failedCount{0};
};
