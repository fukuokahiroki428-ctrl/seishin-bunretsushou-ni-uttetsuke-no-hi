#pragma once

#include <QObject>
#include <atomic>
#include <QString>
#include <QJsonObject>
#include <QProcess>
#include <QMutex>

class HanishikiBackend;

class BlueskyCollector : public QObject
{
    Q_OBJECT

public:
    explicit BlueskyCollector(HanishikiBackend *backend, QObject *parent = nullptr);
    ~BlueskyCollector() override;

    void collect(const QJsonObject &config, const std::atomic<bool> &isRunning);
    void stopDaemon();
    // ★ QProcess 를 건드리지 않는다 — 다른 스레드 것일 수 있다.
    //   pid 는 start 직후 여기에 베껴 두고, 이후로는 이 값만 쓴다.
    qint64 daemonPid() const { return m_daemonPid; }

private:
    bool startDaemon(const QString &handle, const QString &password, QJsonObject customInitArgs = QJsonObject());
    bool startDaemonMulti(const QJsonArray &accounts);
    QJsonObject sendCommand(const QJsonObject &cmd, const std::atomic<bool> &isRunning, int timeoutMs = 600000);
    void processOutputLines(const QByteArray &data);
    // ★ 다운로드 끝나면 user 폴더 root 에 gallery.html 자동 생성 — 모든 미디어 grid 로 한눈에
    void generateMediaGallery(const QString &userDir, const QString &handle);

    HanishikiBackend *m_backend;
    QProcess *m_daemon = nullptr;
    qint64 m_daemonPid = 0;   // stopDaemon 이 객체 없이도 끊을 수 있게
    // ★ m_daemon 을 만들고 없애는 것은 여러 스레드가 동시에 한다 — 반드시 이걸 잠그고.
    QMutex m_daemonMutex;
    bool m_daemonReady = false;
    bool m_rateLimitWait = false;   // true: 대기 후 재시도, false: 즉시 중지
    int m_rateLimitWaitMins = 5;    // 대기 시간 (분)
};
