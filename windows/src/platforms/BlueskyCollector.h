#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QProcess>

class MiyoBackend;

class BlueskyCollector : public QObject
{
    Q_OBJECT

public:
    explicit BlueskyCollector(MiyoBackend *backend, QObject *parent = nullptr);
    ~BlueskyCollector() override;

    void collect(const QJsonObject &config, bool &isRunning);
    void stopDaemon();
    qint64 daemonPid() const { return m_daemon ? m_daemon->processId() : 0; }

private:
    bool startDaemon(const QString &handle, const QString &password, QJsonObject customInitArgs = QJsonObject());
    bool startDaemonMulti(const QJsonArray &accounts);
    QJsonObject sendCommand(const QJsonObject &cmd, bool &isRunning, int timeoutMs = 600000);
    void processOutputLines(const QByteArray &data);
    // ★ 다운로드 끝나면 user 폴더 root 에 gallery.html 자동 생성 — 모든 미디어 grid 로 한눈에
    void generateMediaGallery(const QString &userDir, const QString &handle);

    MiyoBackend *m_backend;
    QProcess *m_daemon = nullptr;
    bool m_daemonReady = false;
    bool m_rateLimitWait = false;   // true: 대기 후 재시도, false: 즉시 중지
    int m_rateLimitWaitMins = 5;    // 대기 시간 (분)
};
