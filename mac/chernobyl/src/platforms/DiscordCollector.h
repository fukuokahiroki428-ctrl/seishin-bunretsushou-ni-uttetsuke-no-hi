#pragma once

#include <QMainWindow>
#include <QWidget>
#include <QLineEdit>
#include <QTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QSpinBox>
#include <QJsonArray>
#include <QJsonObject>

#include "core/WorkerSignals.h"

class HttpClient;
class DiskJsonBuffer;

class DiscordCollector : public QMainWindow
{
    Q_OBJECT

public:
    explicit DiscordCollector(QWidget *parent = nullptr);
    ~DiscordCollector() override = default;

public slots:
    void startCollection();
    void stopCollection();

private:
    void setupUi();
    void browsePath();
    void appendLog(const QString &text);
    void updateStatus(const QString &text);
    void updateCounts(int messages, int media);

    // API calls
    QJsonArray fetchMessages(const QString &channelId, const QString &token,
                             int limit = 100, const QString &before = QString());
    QJsonObject fetchChannelInfo(const QString &channelId, const QString &token);
    QJsonArray fetchGuildChannels(const QString &guildId, const QString &token);

    // Collection for a single channel (used by both single-channel and server modes)
    void collectChannel(const QString &channelId, const QString &channelName,
                        const QString &token, const QString &savePath,
                        int maxCount, int delay, bool autosave);

    // Collection
    void runCollection();
    void saveToExcel(DiskJsonBuffer &buffer, const QString &saveDir, const QString &channelName);

    // UI elements
    QLineEdit *m_tokenEntry = nullptr;
    QLineEdit *m_channelIdEntry = nullptr;
    QLineEdit *m_countEntry = nullptr;
    QLineEdit *m_pathEntry = nullptr;
    QCheckBox *m_chkMedia = nullptr;
    QCheckBox *m_chkExcel = nullptr;
    QCheckBox *m_chkAutosave = nullptr;
    QSpinBox *m_delaySpin = nullptr;
    QPushButton *m_btnStart = nullptr;
    QPushButton *m_btnStop = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_labelMessages = nullptr;
    QLabel *m_labelMedia = nullptr;
    QTextEdit *m_logText = nullptr;

    WorkerSignals *m_signals = nullptr;
    HttpClient *m_http = nullptr;
    bool m_isRunning = false;
    QString m_terminalLogPath;
};
