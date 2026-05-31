#pragma once

#include <QMainWindow>
#include <QLineEdit>
#include <QTextEdit>
#include <QLabel>
#include <QPushButton>
#include <QCheckBox>
#include <QComboBox>
#include <QProgressBar>
#include <QRadioButton>
#include <QButtonGroup>
#include <QProcess>

#include "core/WorkerSignals.h"

class YouTubeDownloader : public QMainWindow
{
    Q_OBJECT

public:
    explicit YouTubeDownloader(QWidget *parent = nullptr);
    ~YouTubeDownloader() override = default;

public slots:
    void startDownload();
    void stopDownload();
    void analyzeUrl();

private:
    void setupUi();
    void browsePath();
    void appendLog(const QString &text);
    void updateStatus(const QString &text);
    void updateProgress(int value);
    void checkYtdlp();

    void runDownload();

    // UI
    QCheckBox *m_chkProxy = nullptr;
    QLineEdit *m_proxyInput = nullptr;
    QTextEdit *m_urlInput = nullptr;
    QLineEdit *m_pathEntry = nullptr;
    QComboBox *m_qualityCombo = nullptr;
    QComboBox *m_formatCombo = nullptr;
    QRadioButton *m_radioVideo = nullptr;
    QRadioButton *m_radioAudio = nullptr;
    QRadioButton *m_radioThumb = nullptr;
    QButtonGroup *m_typeGroup = nullptr;
    QComboBox *m_audioFormatCombo = nullptr;
    QCheckBox *m_chkThumbnail = nullptr;
    QCheckBox *m_chkSubtitles = nullptr;
    QCheckBox *m_chkMetadata = nullptr;
    QCheckBox *m_chkSponsor = nullptr;
    QCheckBox *m_chkPlaylist = nullptr;
    QPushButton *m_btnAnalyze = nullptr;
    QPushButton *m_btnDownload = nullptr;
    QPushButton *m_btnStop = nullptr;
    QLabel *m_statusLabel = nullptr;
    QProgressBar *m_progressBar = nullptr;
    QTextEdit *m_logText = nullptr;

    WorkerSignals *m_signals = nullptr;
    bool m_isDownloading = false;
    QProcess *m_currentProcess = nullptr;
};
