#include "DiscordCollector.h"
#include "utils/HttpClient.h"
#include "utils/ExcelWriter.h"
#include "utils/FileHelper.h"
#include "utils/DiskJsonBuffer.h"
#include "core/Common.h"
#include "core/Config.h"

#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QGroupBox>
#include <QScrollArea>
#include <QFileDialog>
#include <QThread>
#include <QTimer>
#include <QDir>
#include <QDateTime>
#include <QJsonDocument>
#include <QScrollBar>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QProcessEnvironment>
#include <QFileDevice>
#include <QProcessEnvironment>
#include <QCoreApplication>

DiscordCollector::DiscordCollector(QWidget *parent)
    : QMainWindow(parent)
    , m_signals(new WorkerSignals(this))
    , m_http(new HttpClient(this))
{
    setWindowTitle("Discord ダウンローダー");
    setMinimumSize(400, 600);
    resize(450, 700);

    setStyleSheet("QMainWindow { background-color: #FAF9F5; }");

    connect(m_signals, &WorkerSignals::log, this, &DiscordCollector::appendLog);
    connect(m_signals, &WorkerSignals::status, this, &DiscordCollector::updateStatus);

    setupUi();
}

void DiscordCollector::setupUi()
{
    auto *central = new QWidget(this);
    central->setStyleSheet(R"(
        QWidget { background-color: #FAF9F5; color: #1a1a1a; font-family: 'Apple SD Gothic Neo', sans-serif; }
        QGroupBox { background-color: #FAF9F5; color: #1a1a1a; font-size: 13px; font-weight: bold;
                    border: 1px solid #E8E7E2; border-radius: 8px; margin-top: 12px; padding-top: 8px; }
        QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 8px; }
        QLabel { background-color: transparent; color: #1a1a1a; }
        QLineEdit { background-color: #FFFFFF; border: 1px solid #E8E7E2; border-radius: 6px;
                    padding: 10px; color: #1a1a1a; font-size: 13px; }
        QLineEdit:focus { border: 1px solid #5865F2; }
        QCheckBox { color: #1a1a1a; font-size: 12px; }
    )");
    setCentralWidget(central);

    auto *scroll = new QScrollArea(central);
    scroll->setWidgetResizable(true);
    scroll->setStyleSheet("QScrollArea { border: none; background-color: #FAF9F5; }");

    auto *scrollContent = new QWidget();
    auto *layout = new QVBoxLayout(scrollContent);
    layout->setContentsMargins(32, 24, 32, 24);
    layout->setSpacing(20);

    // ===== Token section =====
    auto *tokenGroup = new QGroupBox("사용자 토큰", scrollContent);
    auto *tokenLayout = new QVBoxLayout(tokenGroup);
    tokenLayout->setSpacing(8);

    auto *infoLabel = new QLabel("F12 → Network → Headers → Authorization");
    infoLabel->setStyleSheet("color: #888888; font-size: 11px;");
    tokenLayout->addWidget(infoLabel);

    m_tokenEntry = new QLineEdit();
    m_tokenEntry->setPlaceholderText("사용자 토큰");
    m_tokenEntry->setEchoMode(QLineEdit::Password);
    tokenLayout->addWidget(m_tokenEntry);
    layout->addWidget(tokenGroup);

    // ===== Target section =====
    auto *targetGroup = new QGroupBox("수집 대상", scrollContent);
    auto *targetLayout = new QVBoxLayout(targetGroup);
    targetLayout->setSpacing(8);

    auto *channelLabel = new QLabel("채널 ID");
    channelLabel->setStyleSheet("color: #1a1a1a; font-size: 12px;");
    targetLayout->addWidget(channelLabel);

    m_channelIdEntry = new QLineEdit();
    m_channelIdEntry->setPlaceholderText("예: 1234567890123456789");
    targetLayout->addWidget(m_channelIdEntry);

    auto *countLabel = new QLabel("수집 수");
    countLabel->setStyleSheet("color: #1a1a1a; font-size: 12px;");
    targetLayout->addWidget(countLabel);

    m_countEntry = new QLineEdit();
    m_countEntry->setPlaceholderText("예: 1000 (빈칸이면 전체)");
    targetLayout->addWidget(m_countEntry);

    auto *pathLabel = new QLabel("저장 경로");
    pathLabel->setStyleSheet("color: #1a1a1a; font-size: 12px;");
    targetLayout->addWidget(pathLabel);

    auto *pathRow = new QHBoxLayout();
    m_pathEntry = new QLineEdit();
    m_pathEntry->setText(QDir::homePath() + "/Downloads");
    pathRow->addWidget(m_pathEntry);

    auto *btnBrowse = new QPushButton("...");
    btnBrowse->setFixedWidth(40);
    btnBrowse->setStyleSheet(R"(
        QPushButton { background-color: #E8E7E2; border: none; border-radius: 6px; padding: 10px; color: #1a1a1a; }
        QPushButton:hover { background-color: #D8D6D0; }
    )");
    connect(btnBrowse, &QPushButton::clicked, this, &DiscordCollector::browsePath);
    pathRow->addWidget(btnBrowse);
    targetLayout->addLayout(pathRow);
    layout->addWidget(targetGroup);

    // ===== Options section =====
    auto *optGroup = new QGroupBox("옵션", scrollContent);
    auto *optLayout = new QVBoxLayout(optGroup);

    m_chkMedia = new QCheckBox("미디어 다운로드");
    m_chkMedia->setChecked(true);
    optLayout->addWidget(m_chkMedia);

    m_chkExcel = new QCheckBox("Excel 저장");
    m_chkExcel->setChecked(true);
    optLayout->addWidget(m_chkExcel);

    m_chkAutosave = new QCheckBox("중간저장 (100개마다)");
    m_chkAutosave->setChecked(true);
    optLayout->addWidget(m_chkAutosave);

    auto *delayRow = new QHBoxLayout();
    auto *delayLabel = new QLabel("요청 딜레이:");
    delayLabel->setStyleSheet("color: #1a1a1a; font-size: 12px;");
    delayRow->addWidget(delayLabel);

    m_delaySpin = new QSpinBox();
    m_delaySpin->setRange(0, 60);
    m_delaySpin->setValue(1);
    m_delaySpin->setSuffix(" 초");
    m_delaySpin->setStyleSheet("color: #1a1a1a; font-size: 12px; padding: 4px;");
    delayRow->addWidget(m_delaySpin);
    delayRow->addStretch();
    optLayout->addLayout(delayRow);
    layout->addWidget(optGroup);

    // ===== Control section =====
    auto *ctrlRow = new QHBoxLayout();
    m_btnStart = new QPushButton("시작");
    m_btnStart->setStyleSheet(R"(
        QPushButton { background-color: #5865F2; border: none; border-radius: 8px; padding: 12px 24px;
                      color: white; font-weight: bold; font-size: 14px; }
        QPushButton:hover { background-color: #4752c4; }
        QPushButton:disabled { background-color: #E8E7E2; color: #999999; }
    )");
    connect(m_btnStart, &QPushButton::clicked, this, &DiscordCollector::startCollection);
    ctrlRow->addWidget(m_btnStart);

    m_btnStop = new QPushButton("중지");
    m_btnStop->setEnabled(false);
    m_btnStop->setStyleSheet(R"(
        QPushButton { background-color: #E8E7E2; border: none; border-radius: 8px; padding: 12px 24px;
                      color: #1a1a1a; font-weight: bold; font-size: 14px; }
        QPushButton:hover { background-color: #D8D6D0; }
    )");
    connect(m_btnStop, &QPushButton::clicked, this, &DiscordCollector::stopCollection);
    ctrlRow->addWidget(m_btnStop);
    layout->addLayout(ctrlRow);

    m_statusLabel = new QLabel("");
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet("color: #888888; font-size: 12px;");
    layout->addWidget(m_statusLabel);

    // ===== Counter & Log section =====
    auto *counterRow = new QHBoxLayout();
    m_labelMessages = new QLabel("메시지: 0");
    m_labelMessages->setStyleSheet("color: #5865F2; font-size: 12px; font-weight: bold;");
    counterRow->addWidget(m_labelMessages);
    m_labelMedia = new QLabel("미디어: 0");
    m_labelMedia->setStyleSheet("color: #5865F2; font-size: 12px; font-weight: bold;");
    counterRow->addWidget(m_labelMedia);
    counterRow->addStretch();
    layout->addLayout(counterRow);

    m_logText = new QTextEdit();
    m_logText->setReadOnly(true);
    m_logText->setMinimumHeight(120);
    m_logText->setStyleSheet(R"(
        QTextEdit { background-color: #f9fafb; border: 1px solid #e5e7eb; padding: 10px;
                    color: #333333; font-family: 'Courier New', monospace; font-size: 10px; }
    )");
    layout->addWidget(m_logText);

    layout->addStretch();
    scroll->setWidget(scrollContent);

    auto *mainLayout = new QVBoxLayout(central);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->addWidget(scroll);
}

void DiscordCollector::browsePath()
{
    QString path = QFileDialog::getExistingDirectory(this, "저장 위치 선택");
    if (!path.isEmpty()) {
        m_pathEntry->setText(path);
    }
}

void DiscordCollector::appendLog(const QString &text)
{
    QString time = QDateTime::currentDateTime().toString("HH:mm:ss");
    QString formatted = QString("[%1] %2").arg(time, text);
    m_logText->append(formatted);
    m_logText->verticalScrollBar()->setValue(m_logText->verticalScrollBar()->maximum());
    // Also write to terminal log file
    if (!m_terminalLogPath.isEmpty()) {
        QFile f(m_terminalLogPath);
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            f.write((text + "\n").toUtf8());
            f.close();
        }
    }
}

void DiscordCollector::updateStatus(const QString &text)
{
    m_statusLabel->setText(text);
}

void DiscordCollector::updateCounts(int messages, int media)
{
    QTimer::singleShot(0, this, [this, messages, media]() {
        m_labelMessages->setText(QString("메시지: %1").arg(messages));
        m_labelMedia->setText(QString("미디어: %1").arg(media));
    });
}

void DiscordCollector::startCollection()
{
    emit m_signals->log("🎮 Discord 시작 버튼 클릭됨...");

    QString token = m_tokenEntry->text().trimmed();
    QString channelId = m_channelIdEntry->text().trimmed();

    if (token.isEmpty()) {
        emit m_signals->log("❌ 토큰을 입력하세요");
        return;
    }
    if (channelId.isEmpty()) {
        emit m_signals->log("❌ 채널 ID를 입력하세요");
        return;
    }

    emit m_signals->log(QString("✅ 준비 완료 - 채널: %1").arg(channelId));
    m_isRunning = true;
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);

    emit m_signals->log("🚀 수집 스레드 시작...");

    QThread *thread = QThread::create([this]() { runCollection(); });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void DiscordCollector::stopCollection()
{
    m_isRunning = false;
    m_btnStop->setEnabled(false);
    m_btnStart->setEnabled(true);
    emit m_signals->log("⏹️ 중지 중...");
    emit m_signals->status("중지");
}

QJsonArray DiscordCollector::fetchMessages(const QString &channelId, const QString &token,
                                            int limit, const QString &before)
{
    QString url = QString("https://discord.com/api/v10/channels/%1/messages?limit=%2")
                      .arg(channelId).arg(limit);
    if (!before.isEmpty()) {
        url += "&before=" + before;
    }

    QMap<QString, QString> headers;
    headers["Authorization"] = token;
    headers["Content-Type"] = "application/json";

    HttpResponse resp = m_http->get(url, headers);
    if (resp.isOk()) {
        return QJsonDocument::fromJson(resp.data).array();
    }

    if (resp.statusCode == 429) {
        // Parse retry-after from response
        QJsonObject errObj = QJsonDocument::fromJson(resp.data).object();
        double retryAfter = errObj["retry_after"].toDouble(5.0);
        int waitSec = qMax(1, qMin(60, (int)retryAfter + 1));
        emit m_signals->log(QString("⏳ Rate limit - %1초 대기...").arg(waitSec));
        // Wait in 1-second increments so we can check m_isRunning
        for (int i = 0; i < waitSec && m_isRunning; ++i) {
            QThread::sleep(1);
        }
        if (!m_isRunning) return QJsonArray();
        // Retry iteratively (not recursive to avoid stack overflow)
        HttpResponse retryResp = m_http->get(url, headers);
        if (retryResp.isOk()) {
            return QJsonDocument::fromJson(retryResp.data).array();
        }
        // Still rate limited — wait longer and try once more
        if (retryResp.statusCode == 429) {
            emit m_signals->log("⏳ Rate limit 계속 - 30초 대기...");
            for (int i = 0; i < 30 && m_isRunning; ++i) QThread::sleep(1);
            if (!m_isRunning) return QJsonArray();
            HttpResponse retry2 = m_http->get(url, headers);
            if (retry2.isOk()) return QJsonDocument::fromJson(retry2.data).array();
        }
        emit m_signals->log(QString("❌ HTTP %1 에러").arg(retryResp.statusCode));
    } else {
        emit m_signals->log(QString("❌ HTTP %1 에러").arg(resp.statusCode));
    }

    return QJsonArray();
}

QJsonObject DiscordCollector::fetchChannelInfo(const QString &channelId, const QString &token)
{
    QString url = QString("https://discord.com/api/v10/channels/%1").arg(channelId);

    QMap<QString, QString> headers;
    headers["Authorization"] = token;

    HttpResponse resp = m_http->get(url, headers);
    if (resp.isOk()) {
        return resp.json();
    }
    return QJsonObject();
}

QJsonArray DiscordCollector::fetchGuildChannels(const QString &guildId, const QString &token)
{
    QString url = QString("https://discord.com/api/v10/guilds/%1/channels").arg(guildId);

    QMap<QString, QString> headers;
    headers["Authorization"] = token;

    HttpResponse resp = m_http->get(url, headers);
    if (resp.isOk()) {
        return QJsonDocument::fromJson(resp.data).array();
    }
    emit m_signals->log(QString("❌ 서버 채널 목록 가져오기 실패 (HTTP %1)").arg(resp.statusCode));
    return QJsonArray();
}

void DiscordCollector::runCollection()
{
    QString token = m_tokenEntry->text().trimmed();
    QString channelId = m_channelIdEntry->text().trimmed();
    QString maxCountStr = m_countEntry->text().trimmed();
    int maxCount = maxCountStr.isEmpty() ? 0 : maxCountStr.toInt();
    int delay = m_delaySpin->value();
    bool autosave = m_chkAutosave->isChecked();

    // ★ .command/log 는 사용자 AppData 폴더에 (절대 NAS 아님, 실행권한 보존)
    QString savePath = m_pathEntry->text().trimmed();
    QString tempDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/abiwa_discord";
    QDir().mkpath(tempDir);
    m_terminalLogPath = tempDir + "/miyo_discord_log.txt";
    {
        QFile f(m_terminalLogPath);
        f.open(QIODevice::WriteOnly);
        f.write("=========================================\n");
        f.write("  ABIWA - DISCORD\n");
        f.write("=========================================\n\n");
        f.close();
    }
    // Create and open tail command
#ifdef Q_OS_WIN
    QString scriptPath = tempDir + "/miyo_discord_tail.bat";
    {
        QFile script(scriptPath);
        if (script.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QString logNative = QDir::toNativeSeparators(m_terminalLogPath);
            QString content;
            content += "@echo off\r\nchcp 65001 >nul\r\ncls\r\n";
            content += "type \"" + logNative + "\"\r\n";
            content += ":loop\r\n";
            content += "timeout /t 1 >nul\r\n";
            content += "findstr /c:\"[DONE]\" \"" + logNative + "\" >nul 2>&1\r\n";
            content += "if %errorlevel%==0 (\r\n";
            content += "  echo.\r\n  echo 터미널을 닫아도 됩니다.\r\n  pause >nul\r\n  exit /b 0\r\n)\r\n";
            content += "goto loop\r\n";
            script.write(content.toUtf8());
            script.close();
        }
        QProcess::startDetached("cmd.exe", {"/c", "start", "ABIWA-Discord", QDir::toNativeSeparators(scriptPath)});
    }
#else
    QString scriptPath = tempDir + "/miyo_discord_tail.command";
    {
        QFile script(scriptPath);
        if (script.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QString content;
            content += "#!/bin/bash\nclear\n";
            content += "cat '" + m_terminalLogPath + "'\n";
            content += "tail -f -n +0 '" + m_terminalLogPath + "' &\n";
            content += "TAIL_PID=$!\n";
            content += "while true; do\n";
            content += "  if grep -q '\\[DONE\\]' '" + m_terminalLogPath + "' 2>/dev/null; then\n";
            content += "    kill $TAIL_PID 2>/dev/null\n";
            content += "    echo ''\necho '터미널을 닫아도 됩니다.'\nread -n 1\nexit 0\n";
            content += "  fi\n  sleep 1\ndone\n";
            script.write(content.toUtf8());
            script.close();
            script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                                  QFileDevice::ReadGroup | QFileDevice::ExeGroup);
        }
        QProcess::startDetached("/usr/bin/open", {scriptPath});
    }
#endif

    emit m_signals->log("Discord 수집 시작...");
    emit m_signals->status("연결 중...");

    // 먼저 채널 ID인지 서버(길드) ID인지 확인
    QJsonObject channelInfo = fetchChannelInfo(channelId, token);
    if (channelInfo.isEmpty()) {
        // 채널로 접근 실패 → 서버 ID로 시도
        emit m_signals->log("채널 접근 실패. 서버 ID로 시도합니다...");
        QJsonArray guildChannels = fetchGuildChannels(channelId, token);
        if (!guildChannels.isEmpty()) {
            // 텍스트 채널만 필터링 (type 0 = text, type 5 = announcement)
            QJsonArray textChannels;
            for (const auto &ch : guildChannels) {
                QJsonObject channel = ch.toObject();
                int type = channel["type"].toInt(-1);
                if (type == 0 || type == 5) {
                    textChannels.append(channel);
                }
            }

            emit m_signals->log(QString("📋 서버에서 텍스트 채널 %1개 발견").arg(textChannels.size()));

            for (int ci = 0; ci < textChannels.size() && m_isRunning; ++ci) {
                QJsonObject ch = textChannels[ci].toObject();
                QString chId = ch["id"].toString();
                QString chName = ch["name"].toString(chId);
                emit m_signals->log(QString("▶ [%1/%2] #%3 수집...").arg(ci + 1).arg(textChannels.size()).arg(chName));
                collectChannel(chId, chName, token, savePath, maxCount, delay, autosave);
            }

            emit m_signals->log("═══ 서버 전체 수집 완료! ═══");
            emit m_signals->status("완료!");
            m_isRunning = false;
            QTimer::singleShot(0, this, [this]() {
                m_btnStart->setEnabled(true);
                m_btnStop->setEnabled(false);
            });
            return;
        } else {
            emit m_signals->log("❌ 채널 또는 서버 ID를 확인해주세요.");
            m_isRunning = false;
            QTimer::singleShot(0, this, [this]() {
                m_btnStart->setEnabled(true);
                m_btnStop->setEnabled(false);
            });
            return;
        }
    }

    // 단일 채널 모드
    QString channelName = channelInfo["name"].toString(channelId);
    emit m_signals->log(QString("📢 채널: %1").arg(channelName));
    collectChannel(channelId, channelName, token, savePath, maxCount, delay, autosave);

    emit m_signals->log("🎉 완료!");
    emit m_signals->log("Complete.");
    // Write DONE marker for terminal tail
    if (!m_terminalLogPath.isEmpty()) {
        QFile f(m_terminalLogPath);
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            f.write("\n[DONE]\n");
            f.close();
        }
    }
    emit m_signals->status("완료!");

    m_isRunning = false;
    QTimer::singleShot(0, this, [this]() {
        m_btnStart->setEnabled(true);
        m_btnStop->setEnabled(false);
    });
}

void DiscordCollector::collectChannel(const QString &channelId, const QString &channelName,
                                       const QString &token, const QString &savePath,
                                       int maxCount, int delay, bool autosave)
{
    QString discordDir = savePath + "/discord";
    QDir().mkpath(discordDir);
    QString channelDir = discordDir + "/" + channelName + "_" + channelId;
    QDir().mkpath(channelDir);

    QString mediaDir = channelDir + "/media";
    if (m_chkMedia->isChecked()) {
        QDir().mkpath(mediaDir);
    }

    emit m_signals->status(QString("#%1 수집 중...").arg(channelName));
    updateCounts(0, 0);

    QString tmpDir = channelDir + "/.tmp_messages";
    QDir().mkpath(tmpDir);
    DiskJsonBuffer messages(tmpDir, "dc_messages");

    int mediaCount = 0;
    QString before;
    int lastSaveCount = 0;

    while (m_isRunning) {
        QJsonArray batch = fetchMessages(channelId, token, 100, before);
        if (batch.isEmpty()) break;

        for (const auto &val : batch) {
            QJsonObject msg = val.toObject();
            messages.append(msg);

            QString author = msg["author"].toObject()["username"].toString("Unknown");
            QString content = msg["content"].toString().left(50);
            emit m_signals->log(QString("💬 [%1] %2...").arg(author, content));

            // Download media
            if (m_chkMedia->isChecked()) {
                QJsonArray attachments = msg["attachments"].toArray();
                for (const auto &attVal : attachments) {
                    QJsonObject att = attVal.toObject();
                    QString url = att["url"].toString();
                    if (url.isEmpty()) continue;

                    QString filename = msg["id"].toString() + "_" + att["filename"].toString("file");
                    QString filepath = mediaDir + "/" + filename;

                    if (m_http->downloadFile(url, filepath)) {
                        Common::setFileTimes(filepath, msg["timestamp"].toString());

                        QString msgUrl = QString("https://discord.com/channels/_/%1/%2")
                                             .arg(channelId, msg["id"].toString());
                        Common::addExifMetadata(filepath,
                            "@" + author, content,
                            "Discord @" + author, msgUrl,
                            msg["timestamp"].toString());
                        FileHelper::setFinderComment(filepath, msgUrl);

                        mediaCount++;
                        emit m_signals->log(QString("  📥 %1").arg(att["filename"].toString("file")));
                    }
                }
            }
        }

        before = batch.last().toObject()["id"].toString();
        updateCounts(messages.count(), mediaCount);
        emit m_signals->status(QString("#%1: %2개").arg(channelName).arg(messages.count()));

        if (autosave && messages.count() - lastSaveCount >= 500) {
            emit m_signals->log(QString("💾 중간저장... (%1개)").arg(messages.count()));
            saveToExcel(messages, channelDir, channelName);
            lastSaveCount = messages.count();
        }

        if (maxCount > 0 && messages.count() >= maxCount) break;
        if (delay > 0) QThread::sleep(delay);
    }

    emit m_signals->log(QString("✅ #%1: %2건 수집").arg(channelName).arg(messages.count()));
    updateCounts(messages.count(), mediaCount);

    if (m_chkExcel->isChecked()) {
        saveToExcel(messages, channelDir, channelName);
    }
}

void DiscordCollector::saveToExcel(DiskJsonBuffer &buffer, const QString &saveDir, const QString &channelName)
{
    ExcelWriter writer;

    QStringList headers = {"Message ID", "Timestamp", "Author", "Content", "Attachments"};
    writer.writeHeader(headers, QColor("#5865F2"));

    int row = 2;
    buffer.resetReader();
    QJsonObject msg;
    while (buffer.readNext(msg)) {

        QStringList attachments;
        for (const auto &att : msg["attachments"].toArray()) {
            attachments << att.toObject()["filename"].toString();
        }

        QJsonObject author = msg["author"].toObject();
        QString authorName = author["username"].toString() + "#" + author["discriminator"].toString("0");
        QString timestamp = msg["timestamp"].toString().left(19).replace("T", " ");
        QString content = msg["content"].toString().left(500);

        writer.writeRow(row++, {
            msg["id"].toString(),
            timestamp,
            authorName,
            content,
            attachments.join(", ")
        });
    }

    writer.setColumnWidth(1, 20);
    writer.setColumnWidth(2, 20);
    writer.setColumnWidth(3, 20);
    writer.setColumnWidth(4, 50);
    writer.setColumnWidth(5, 30);

    QString filepath = saveDir + "/" + channelName + "_messages.xlsx";
    writer.save(filepath);
    FileHelper::setDownloadMeta(filepath, "ABIWA Discord");
    emit m_signals->log(QString("  저장: %1").arg(filepath));
}
