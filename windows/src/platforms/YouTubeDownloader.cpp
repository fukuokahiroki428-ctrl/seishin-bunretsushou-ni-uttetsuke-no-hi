#include "YouTubeDownloader.h"
#include "core/Common.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QFileDialog>
#include <QThread>
#include <QTimer>
#include <QDir>
#include <QDateTime>
#include <QScrollBar>
#include <QRegularExpression>

YouTubeDownloader::YouTubeDownloader(QWidget *parent)
    : QMainWindow(parent)
    , m_signals(new WorkerSignals(this))
{
    setWindowTitle("YouTube ダウンローダー");
    setMinimumSize(400, 600);
    resize(450, 750);
    setStyleSheet("QMainWindow { background-color: #FAF9F5; }");

    connect(m_signals, &WorkerSignals::log, this, &YouTubeDownloader::appendLog);
    connect(m_signals, &WorkerSignals::status, this, &YouTubeDownloader::updateStatus);

    setupUi();
    checkYtdlp();
}

void YouTubeDownloader::setupUi()
{
    auto *central = new QWidget(this);
    central->setStyleSheet(R"(
        QWidget { background-color: #FAF9F5; color: #1a1a1a; font-family: 'Apple SD Gothic Neo', sans-serif; }
        QGroupBox { background-color: #FAF9F5; color: #1a1a1a; font-size: 13px; font-weight: bold;
                    border: 1px solid #E8E7E2; border-radius: 8px; margin-top: 12px; padding-top: 8px; }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 8px; }
        QRadioButton, QCheckBox { background-color: #FAF9F5; color: #1a1a1a; font-size: 12px; }
        QLabel { background-color: transparent; color: #1a1a1a; }
        QComboBox { background-color: #FFFFFF; border: 1px solid #E8E7E2; border-radius: 6px;
                    padding: 8px; color: #1a1a1a; font-size: 12px; }
    )");
    setCentralWidget(central);

    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(20, 20, 20, 20);
    layout->setSpacing(12);

    // Proxy
    auto *proxyRow = new QHBoxLayout();
    m_chkProxy = new QCheckBox("프록시");
    proxyRow->addWidget(m_chkProxy);
    m_proxyInput = new QLineEdit();
    m_proxyInput->setPlaceholderText("socks5://user:pass@host:port");
    m_proxyInput->setStyleSheet("QLineEdit { background-color: #FFFFFF; border: 1px solid #E8E7E2; border-radius: 4px; padding: 6px; font-size: 12px; }");
    proxyRow->addWidget(m_proxyInput);
    layout->addLayout(proxyRow);

    // URL input
    auto *urlGroup = new QGroupBox("URL", central);
    auto *urlLayout = new QVBoxLayout(urlGroup);
    m_urlInput = new QTextEdit();
    m_urlInput->setPlaceholderText("YouTube URL을 입력하세요 (여러 줄 가능)");
    m_urlInput->setMaximumHeight(100);
    m_urlInput->setStyleSheet("QTextEdit { background-color: #FFFFFF; border: 1px solid #E8E7E2; border-radius: 6px; padding: 8px; font-size: 13px; }");
    urlLayout->addWidget(m_urlInput);

    auto *urlBtnRow = new QHBoxLayout();
    m_btnAnalyze = new QPushButton("분석");
    m_btnAnalyze->setStyleSheet("QPushButton { background-color: #E8E7E2; border: none; border-radius: 6px; padding: 8px 16px; font-size: 12px; }");
    connect(m_btnAnalyze, &QPushButton::clicked, this, &YouTubeDownloader::analyzeUrl);
    urlBtnRow->addWidget(m_btnAnalyze);
    urlBtnRow->addStretch();
    urlLayout->addLayout(urlBtnRow);
    layout->addWidget(urlGroup);

    // Save path
    auto *pathRow = new QHBoxLayout();
    auto *pathLabel = new QLabel("저장 경로:");
    pathLabel->setStyleSheet("font-size: 12px;");
    pathRow->addWidget(pathLabel);
    m_pathEntry = new QLineEdit();
    m_pathEntry->setText(QDir::homePath() + "/Downloads/YouTube");
    m_pathEntry->setStyleSheet("QLineEdit { background-color: #FFFFFF; border: 1px solid #E8E7E2; border-radius: 4px; padding: 6px; font-size: 12px; }");
    pathRow->addWidget(m_pathEntry);
    auto *btnBrowse = new QPushButton("...");
    btnBrowse->setFixedWidth(40);
    btnBrowse->setStyleSheet("QPushButton { background-color: #E8E7E2; border: none; border-radius: 4px; padding: 6px; }");
    connect(btnBrowse, &QPushButton::clicked, this, &YouTubeDownloader::browsePath);
    pathRow->addWidget(btnBrowse);
    layout->addLayout(pathRow);

    // Download type
    auto *typeGroup = new QGroupBox("다운로드 유형", central);
    auto *typeLayout = new QHBoxLayout(typeGroup);
    m_typeGroup = new QButtonGroup(this);
    m_radioVideo = new QRadioButton("영상");
    m_radioVideo->setChecked(true);
    m_radioAudio = new QRadioButton("오디오");
    m_radioThumb = new QRadioButton("썸네일");
    m_typeGroup->addButton(m_radioVideo, 0);
    m_typeGroup->addButton(m_radioAudio, 1);
    m_typeGroup->addButton(m_radioThumb, 2);
    typeLayout->addWidget(m_radioVideo);
    typeLayout->addWidget(m_radioAudio);
    typeLayout->addWidget(m_radioThumb);
    layout->addWidget(typeGroup);

    // Quality
    auto *qualGroup = new QGroupBox("품질 설정", central);
    auto *qualLayout = new QVBoxLayout(qualGroup);
    auto *qualRow = new QHBoxLayout();
    auto *qualLabel = new QLabel("화질:");
    qualLabel->setStyleSheet("font-size: 12px;");
    qualRow->addWidget(qualLabel);
    m_qualityCombo = new QComboBox();
    m_qualityCombo->addItems({"최고 화질 (4K/8K)", "2160p (4K)", "1440p (2K)",
                              "1080p (Full HD)", "720p (HD)", "480p", "360p"});
    m_qualityCombo->setCurrentIndex(3); // 1080p
    qualRow->addWidget(m_qualityCombo);
    qualLayout->addLayout(qualRow);

    auto *fmtRow = new QHBoxLayout();
    auto *fmtLabel = new QLabel("형식:");
    fmtLabel->setStyleSheet("font-size: 12px;");
    fmtRow->addWidget(fmtLabel);
    m_formatCombo = new QComboBox();
    m_formatCombo->addItems({"MP4", "MKV", "WEBM"});
    fmtRow->addWidget(m_formatCombo);
    qualLayout->addLayout(fmtRow);

    auto *audioRow = new QHBoxLayout();
    auto *audioLabel = new QLabel("오디오:");
    audioLabel->setStyleSheet("font-size: 12px;");
    audioRow->addWidget(audioLabel);
    m_audioFormatCombo = new QComboBox();
    m_audioFormatCombo->addItems({"MP3", "M4A", "FLAC", "WAV", "OGG"});
    audioRow->addWidget(m_audioFormatCombo);
    qualLayout->addLayout(audioRow);
    layout->addWidget(qualGroup);

    // Options
    auto *optGroup = new QGroupBox("추가 옵션", central);
    auto *optLayout = new QVBoxLayout(optGroup);
    m_chkThumbnail = new QCheckBox("썸네일 저장");
    optLayout->addWidget(m_chkThumbnail);
    m_chkSubtitles = new QCheckBox("자막 다운로드 (ko/en/ja)");
    m_chkSubtitles->setChecked(true);
    optLayout->addWidget(m_chkSubtitles);
    m_chkMetadata = new QCheckBox("메타데이터 임베드");
    m_chkMetadata->setChecked(true);
    optLayout->addWidget(m_chkMetadata);
    m_chkSponsor = new QCheckBox("SponsorBlock (광고 제거)");
    optLayout->addWidget(m_chkSponsor);
    m_chkPlaylist = new QCheckBox("재생목록 전체 다운로드");
    optLayout->addWidget(m_chkPlaylist);
    layout->addWidget(optGroup);

    // Progress & controls
    m_progressBar = new QProgressBar();
    m_progressBar->setValue(0);
    m_progressBar->setStyleSheet(R"(
        QProgressBar { border: 1px solid #E8E7E2; border-radius: 4px; background: #f5f5f5; text-align: center; height: 20px; }
        QProgressBar::chunk { background: #ff0000; border-radius: 3px; }
    )");
    layout->addWidget(m_progressBar);

    auto *ctrlRow = new QHBoxLayout();
    m_btnDownload = new QPushButton("다운로드");
    m_btnDownload->setStyleSheet(R"(
        QPushButton { background-color: #ff0000; border: none; border-radius: 8px; padding: 12px 24px;
                      color: white; font-weight: bold; font-size: 14px; }
        QPushButton:hover { background-color: #cc0000; }
        QPushButton:disabled { background-color: #E8E7E2; color: #999999; }
    )");
    connect(m_btnDownload, &QPushButton::clicked, this, &YouTubeDownloader::startDownload);
    ctrlRow->addWidget(m_btnDownload);

    m_btnStop = new QPushButton("중지");
    m_btnStop->setEnabled(false);
    m_btnStop->setStyleSheet("QPushButton { background-color: #E8E7E2; border: none; border-radius: 8px; padding: 12px 24px; font-size: 14px; }");
    connect(m_btnStop, &QPushButton::clicked, this, &YouTubeDownloader::stopDownload);
    ctrlRow->addWidget(m_btnStop);
    layout->addLayout(ctrlRow);

    m_statusLabel = new QLabel("Ready");
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet("color: #888888; font-size: 12px;");
    layout->addWidget(m_statusLabel);

    // Log
    m_logText = new QTextEdit();
    m_logText->setReadOnly(true);
    m_logText->setMaximumHeight(150);
    m_logText->setStyleSheet(R"(
        QTextEdit { background-color: #f9fafb; border: 1px solid #e5e7eb; padding: 10px;
                    color: #333333; font-family: 'Courier New', monospace; font-size: 10px; }
    )");
    layout->addWidget(m_logText);
}

void YouTubeDownloader::browsePath()
{
    QString path = QFileDialog::getExistingDirectory(this, "저장 위치 선택");
    if (!path.isEmpty()) m_pathEntry->setText(path);
}

void YouTubeDownloader::appendLog(const QString &text)
{
    m_logText->append(text);
    m_logText->verticalScrollBar()->setValue(m_logText->verticalScrollBar()->maximum());
}

void YouTubeDownloader::updateStatus(const QString &text)
{
    m_statusLabel->setText(text);
}

void YouTubeDownloader::updateProgress(int value)
{
    m_progressBar->setValue(value);
}

void YouTubeDownloader::checkYtdlp()
{
    QProcess proc;
    proc.start(Common::ytDlpExecutable(), {"--version"});
    proc.waitForFinished(5000);
    if (proc.exitCode() == 0) {
        QString ver = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        appendLog("yt-dlp " + ver + " ready");
    } else {
        appendLog("⚠️ yt-dlp not found. Please install: brew install yt-dlp");
    }
}

void YouTubeDownloader::analyzeUrl()
{
    QString urlText = m_urlInput->toPlainText().trimmed();
    if (urlText.isEmpty()) return;

    m_btnAnalyze->setEnabled(false);
    emit m_signals->log("분석 중...");

    QThread *thread = QThread::create([this, urlText]() {
        QStringList urls;
        for (const auto &line : urlText.split('\n')) {
            QString trimmed = line.trimmed();
            if (trimmed.startsWith("http")) urls.append(trimmed);
        }

        int totalVideos = 0;
        for (int i = 0; i < urls.size(); ++i) {
            emit m_signals->log(QString("분석 [%1/%2]...").arg(i + 1).arg(urls.size()));

            QProcess proc;
            proc.start(Common::ytDlpExecutable(), {"--flat-playlist", "--dump-json", "--no-warnings", urls[i]});

            // Read output incrementally to avoid timeout with large channels
            int count = 0;
            while (proc.waitForReadyRead(600000)) {
                while (proc.canReadLine()) {
                    QByteArray line = proc.readLine();
                    if (!line.trimmed().isEmpty()) {
                        count++;
                        if (count % 100 == 0)
                            emit m_signals->log(QString("  %1개...").arg(count));
                    }
                }
            }
            proc.waitForFinished(10000);

            // Read any remaining buffered data
            QByteArray remaining = proc.readAll();
            if (!remaining.trimmed().isEmpty()) {
                for (const auto &line : remaining.split('\n')) {
                    if (!line.trimmed().isEmpty()) count++;
                }
            }

            if (proc.exitCode() == 0) {
                totalVideos += count;
                emit m_signals->log(QString("  %1개 발견").arg(count));
            } else {
                emit m_signals->log("  분석 실패: " + QString::fromUtf8(proc.readAllStandardError()).left(60));
            }
        }

        emit m_signals->log(QString("총 %1개 동영상").arg(totalVideos));

        QTimer::singleShot(0, this, [this]() { m_btnAnalyze->setEnabled(true); });
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void YouTubeDownloader::startDownload()
{
    QString urlText = m_urlInput->toPlainText().trimmed();
    if (urlText.isEmpty()) {
        appendLog("URL을 입력하세요");
        return;
    }

    m_isDownloading = true;
    m_btnDownload->setEnabled(false);
    m_btnStop->setEnabled(true);
    m_progressBar->setValue(0);

    QThread *thread = QThread::create([this]() { runDownload(); });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void YouTubeDownloader::stopDownload()
{
    m_isDownloading = false;
    if (m_currentProcess) {
        m_currentProcess->terminate();
    }
    appendLog("⚠️ 중지 요청...");
    m_statusLabel->setText("중지 중...");
}

void YouTubeDownloader::runDownload()
{
    QString urlText = m_urlInput->toPlainText().trimmed();
    QString savePath = m_pathEntry->text().trimmed();
    QDir().mkpath(savePath);

    QStringList urls;
    for (const auto &line : urlText.split('\n')) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("http")) urls.append(trimmed);
    }

    if (urls.isEmpty()) {
        emit m_signals->log("유효한 URL이 없습니다");
        m_isDownloading = false;
        QTimer::singleShot(0, this, [this]() {
            m_btnDownload->setEnabled(true);
            m_btnStop->setEnabled(false);
        });
        return;
    }

    // Build yt-dlp arguments
    QStringList baseArgs;
    // 업로드 시각 prefix → OS 기본 정렬로 업로드 순 배치
    baseArgs << "-o" << savePath + "/%(upload_date>%Y%m%d)s_%(title)s.%(ext)s";
    baseArgs << "--no-mtime";

    // Proxy
    if (m_chkProxy->isChecked() && !m_proxyInput->text().trimmed().isEmpty()) {
        baseArgs << "--proxy" << m_proxyInput->text().trimmed();
    }

    // Download type
    if (m_radioAudio->isChecked()) {
        QString audioFmt = m_audioFormatCombo->currentText().toLower();
        baseArgs << "-x" << "--audio-format" << audioFmt << "--audio-quality" << "0";
    } else if (m_radioThumb->isChecked()) {
        baseArgs << "--write-thumbnail" << "--skip-download";
    } else {
        // Video
        QMap<QString, QString> qualityMap = {
            {"최고 화질 (4K/8K)", "bestvideo+bestaudio/best"},
            {"2160p (4K)", "bestvideo[height<=2160]+bestaudio/best[height<=2160]"},
            {"1440p (2K)", "bestvideo[height<=1440]+bestaudio/best[height<=1440]"},
            {"1080p (Full HD)", "bestvideo[height<=1080]+bestaudio/best[height<=1080]"},
            {"720p (HD)", "bestvideo[height<=720]+bestaudio/best[height<=720]"},
            {"480p", "bestvideo[height<=480]+bestaudio/best[height<=480]"},
            {"360p", "bestvideo[height<=360]+bestaudio/best[height<=360]"},
        };
        QString quality = m_qualityCombo->currentText();
        baseArgs << "-f" << qualityMap.value(quality, "bestvideo+bestaudio/best");

        QString outputFmt = m_formatCombo->currentText().toLower();
        baseArgs << "--merge-output-format" << outputFmt;
    }

    // Additional options
    if (m_chkThumbnail->isChecked()) baseArgs << "--write-thumbnail";
    if (m_chkSubtitles->isChecked()) baseArgs << "--write-subs" << "--sub-langs" << "ko,en,ja";
    if (m_chkMetadata->isChecked()) baseArgs << "--embed-metadata";
    if (m_chkSponsor->isChecked()) {
        baseArgs << "--sponsorblock-remove" << "sponsor,selfpromo,interaction,intro,outro";
    }
    if (m_chkPlaylist->isChecked()) baseArgs << "--yes-playlist";
    else baseArgs << "--no-playlist";

    // 썸네일을 동영상에 임베드 + 설명 저장
    baseArgs << "--embed-thumbnail";
    baseArgs << "--write-description";

    int successCount = 0, failCount = 0;

    for (int i = 0; i < urls.size(); ++i) {
        if (!m_isDownloading) break;

        emit m_signals->log(QString("🚀 [%1/%2] 다운로드 중: %3").arg(i + 1).arg(urls.size()).arg(urls[i].left(60)));
        emit m_signals->status(QString("다운로드 중 [%1/%2]").arg(i + 1).arg(urls.size()));

        QStringList args = baseArgs;
        args << "--newline"; // Progress parsing
        args << urls[i];

        QProcess proc;
        m_currentProcess = &proc;
        proc.start(Common::ytDlpExecutable(), args);

        // Read output line by line
        while (proc.waitForReadyRead(600000)) {
            while (proc.canReadLine()) {
                QString line = QString::fromUtf8(proc.readLine()).trimmed();
                if (line.contains("[download]") && line.contains("%")) {
                    // Parse progress: "[download]  45.2% of ~100MiB"
                    QRegularExpression rx("(\\d+\\.?\\d*)%");
                    auto match = rx.match(line);
                    if (match.hasMatch()) {
                        int pct = static_cast<int>(match.captured(1).toDouble());
                        int totalProgress = ((i * 100) + pct) / urls.size();
                        QTimer::singleShot(0, this, [this, totalProgress]() {
                            updateProgress(totalProgress);
                        });
                    }
                } else if (!line.isEmpty() && !line.startsWith("[debug]")) {
                    emit m_signals->log("  " + line.left(80));
                }
            }
        }

        proc.waitForFinished(600000);
        m_currentProcess = nullptr;

        if (proc.exitCode() == 0) {
            successCount++;
        } else {
            failCount++;
            QString err = QString::fromUtf8(proc.readAllStandardError()).trimmed().left(80);
            if (!err.isEmpty()) emit m_signals->log("❌ " + err);
        }
    }

    if (m_isDownloading) {
        QTimer::singleShot(0, this, [this]() { updateProgress(100); });
        emit m_signals->log("=" + QString(49, '='));
        emit m_signals->log(QString("✅ 다운로드 완료! (성공: %1, 실패: %2)").arg(successCount).arg(failCount));
        emit m_signals->log(QString("📁 저장 위치: %1").arg(savePath));
        emit m_signals->status(QString("✅ 완료! (%1/%2)").arg(successCount).arg(urls.size()));
    } else {
        emit m_signals->log("⚠️ 사용자에 의해 중지됨");
        emit m_signals->status("중지됨");
    }

    m_isDownloading = false;
    m_currentProcess = nullptr;
    QTimer::singleShot(0, this, [this]() {
        m_btnDownload->setEnabled(true);
        m_btnStop->setEnabled(false);
    });
}
