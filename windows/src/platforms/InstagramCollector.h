#pragma once

#include <QMainWindow>
#include <QLineEdit>
#include <QTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QSpinBox>
#include <QJsonArray>
#include <QJsonObject>
#include <QMap>

#include "core/WorkerSignals.h"

class HttpClient;

class InstagramCollector : public QMainWindow
{
    Q_OBJECT

public:
    explicit InstagramCollector(QWidget *parent = nullptr);
    ~InstagramCollector() override = default;

public slots:
    void startCollection();
    void stopCollection();

private:
    void setupUi();
    void browsePath();
    void appendLog(const QString &text);
    void updateStatus(const QString &text);
    void updateCounts(int media, int reels, int stories);

    void runCollection();
    void saveToExcel(const QJsonArray &mediaData, const QString &saveDir, const QString &username);

    // Instagram API (direct HTTP)
    // ★ 브라우저처럼 보이게 하는 공통 헤더 — UA/X-ASBD-ID/WWW-Claim 누락 시 Instagram 이
    //   401 대신 빈 200(소프트 차단)을 줘서 "게시물 0개"가 됨.
    QMap<QString, QString> igWebHeaders(const QString &sessionId, const QString &csrfToken = QString());
    QString login(const QString &sessionId);
    QJsonObject getUserInfo(const QString &username, const QString &csrfToken, const QString &sessionId);
    QJsonArray getUserMedia(const QString &userId, const QString &csrfToken,
                            const QString &sessionId, int maxCount);
    QJsonArray getUserStories(const QString &userId, const QString &sessionId);
    QJsonArray getUserReels(const QString &userId, const QString &sessionId, int maxCount);
    QJsonArray getUserTaggedMedia(const QString &userId, const QString &sessionId, int maxCount);

    // Media download helper
    int downloadMediaItems(const QJsonArray &items, const QString &username,
                           const QString &mediaDir, const QString &sessionId,
                           const QString &typeLabel);

    // UI
    QLineEdit *m_sessionIdEntry = nullptr;
    QLineEdit *m_usernameEntry = nullptr;
    QLineEdit *m_countEntry = nullptr;
    QLineEdit *m_pathEntry = nullptr;
    QCheckBox *m_chkPosts = nullptr;
    QCheckBox *m_chkReels = nullptr;
    QCheckBox *m_chkStories = nullptr;
    QCheckBox *m_chkTagged = nullptr;
    QCheckBox *m_chkExcel = nullptr;
    QSpinBox *m_delaySpin = nullptr;
    QPushButton *m_btnStart = nullptr;
    QPushButton *m_btnStop = nullptr;
    QLabel *m_statusLabel = nullptr;
    QLabel *m_labelMedia = nullptr;
    QLabel *m_labelReels = nullptr;
    QLabel *m_labelStories = nullptr;
    QTextEdit *m_logText = nullptr;

    WorkerSignals *m_signals = nullptr;
    HttpClient *m_http = nullptr;
    bool m_isRunning = false;
};
