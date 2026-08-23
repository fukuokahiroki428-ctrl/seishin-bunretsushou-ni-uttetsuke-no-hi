#pragma once

#include <QObject>
#include <atomic>
#include <QString>
#include <QJsonObject>
#include <QProcess>

class HanishikiBackend;

class BlueskyCollector : public QObject
{
    Q_OBJECT

public:
    explicit BlueskyCollector(HanishikiBackend *backend, QObject *parent = nullptr);
    ~BlueskyCollector() override;

    void collect(const QJsonObject &config, const std::atomic<bool> &isRunning);
    void stopDaemon();
    qint64 daemonPid() const { return m_daemon ? m_daemon->processId() : 0; }

private:
    bool startDaemon(const QString &handle, const QString &password, QJsonObject customInitArgs = QJsonObject());
    bool startDaemonMulti(const QJsonArray &accounts);
    QJsonObject sendCommand(const QJsonObject &cmd, const std::atomic<bool> &isRunning, int timeoutMs = 600000);
    void processOutputLines(const QByteArray &data);
    // ★ 다운로드 끝나면 user 폴더 root 에 gallery.html 자동 생성 — 모든 미디어 grid 로 한눈에
    void generateMediaGallery(const QString &userDir, const QString &handle);

    HanishikiBackend *m_backend;
    QProcess *m_daemon = nullptr;
    bool m_daemonReady = false;
    bool m_rateLimitWait = false;   // true: 대기 후 재시도, false: 즉시 중지
    int m_rateLimitWaitMins = 5;    // 대기 시간 (분)
};
