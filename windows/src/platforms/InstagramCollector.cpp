#include "InstagramCollector.h"
#include "utils/HttpClient.h"
#include "utils/ExcelWriter.h"
#include "utils/FileHelper.h"
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
#include <QJsonDocument>
#include <QScrollBar>
#include <QUrl>
#include <QProcess>
#include <QProcessEnvironment>
#include <QCoreApplication>

InstagramCollector::InstagramCollector(QWidget *parent)
    : QMainWindow(parent)
    , m_signals(new WorkerSignals(this))
    , m_http(new HttpClient(this))
{
    setWindowTitle("Instagram ダウンローダー");
    setMinimumSize(400, 600);
    resize(450, 700);

    setStyleSheet("QMainWindow { background-color: #FAF9F5; }");

    connect(m_signals, &WorkerSignals::log, this, &InstagramCollector::appendLog);
    connect(m_signals, &WorkerSignals::status, this, &InstagramCollector::updateStatus);

    setupUi();
}

void InstagramCollector::setupUi()
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
        QLineEdit:focus { border: 1px solid #E4405F; }
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

    // Session ID
    auto *sessGroup = new QGroupBox("세션 ID", scrollContent);
    auto *sessLayout = new QVBoxLayout(sessGroup);
    auto *sessInfo = new QLabel("Instagram cookies → sessionid 값");
    sessInfo->setStyleSheet("color: #888888; font-size: 11px;");
    sessLayout->addWidget(sessInfo);
    m_sessionIdEntry = new QLineEdit();
    m_sessionIdEntry->setPlaceholderText("세션 ID");
    m_sessionIdEntry->setEchoMode(QLineEdit::Password);
    sessLayout->addWidget(m_sessionIdEntry);
    layout->addWidget(sessGroup);

    // Target
    auto *targetGroup = new QGroupBox("수집 대상", scrollContent);
    auto *targetLayout = new QVBoxLayout(targetGroup);
    targetLayout->setSpacing(8);

    auto *userLabel = new QLabel("사용자명 (@ 제외)");
    userLabel->setStyleSheet("color: #1a1a1a; font-size: 12px;");
    targetLayout->addWidget(userLabel);
    m_usernameEntry = new QLineEdit();
    m_usernameEntry->setPlaceholderText("예: username");
    targetLayout->addWidget(m_usernameEntry);

    auto *countLabel = new QLabel("수집 수");
    countLabel->setStyleSheet("color: #1a1a1a; font-size: 12px;");
    targetLayout->addWidget(countLabel);
    m_countEntry = new QLineEdit();
    m_countEntry->setPlaceholderText("예: 50 (빈칸이면 전체)");
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
    btnBrowse->setStyleSheet("QPushButton { background-color: #E8E7E2; border: none; border-radius: 6px; padding: 10px; }");
    connect(btnBrowse, &QPushButton::clicked, this, &InstagramCollector::browsePath);
    pathRow->addWidget(btnBrowse);
    targetLayout->addLayout(pathRow);
    layout->addWidget(targetGroup);

    // Options
    auto *optGroup = new QGroupBox("수집 유형", scrollContent);
    auto *optLayout = new QVBoxLayout(optGroup);
    m_chkPosts = new QCheckBox("포스트 / 피드");
    m_chkPosts->setChecked(true);
    optLayout->addWidget(m_chkPosts);
    m_chkReels = new QCheckBox("릴스");
    m_chkReels->setChecked(true);
    optLayout->addWidget(m_chkReels);
    m_chkStories = new QCheckBox("스토리");
    optLayout->addWidget(m_chkStories);
    m_chkTagged = new QCheckBox("태그된 게시물");
    optLayout->addWidget(m_chkTagged);
    m_chkExcel = new QCheckBox("Excel 저장");
    m_chkExcel->setChecked(true);
    optLayout->addWidget(m_chkExcel);

    auto *delayRow = new QHBoxLayout();
    auto *delayLabel = new QLabel("요청 딜레이:");
    delayLabel->setStyleSheet("font-size: 12px;");
    delayRow->addWidget(delayLabel);
    m_delaySpin = new QSpinBox();
    m_delaySpin->setRange(1, 60);
    m_delaySpin->setValue(3);
    m_delaySpin->setSuffix(" 초");
    delayRow->addWidget(m_delaySpin);
    delayRow->addStretch();
    optLayout->addLayout(delayRow);
    layout->addWidget(optGroup);

    // Controls
    auto *ctrlRow = new QHBoxLayout();
    m_btnStart = new QPushButton("시작");
    m_btnStart->setStyleSheet(R"(
        QPushButton { background-color: #E4405F; border: none; border-radius: 8px; padding: 12px 24px;
                      color: white; font-weight: bold; font-size: 14px; }
        QPushButton:hover { background-color: #c13650; }
        QPushButton:disabled { background-color: #E8E7E2; color: #999999; }
    )");
    connect(m_btnStart, &QPushButton::clicked, this, &InstagramCollector::startCollection);
    ctrlRow->addWidget(m_btnStart);
    m_btnStop = new QPushButton("중지");
    m_btnStop->setEnabled(false);
    m_btnStop->setStyleSheet("QPushButton { background-color: #E8E7E2; border: none; border-radius: 8px; padding: 12px 24px; }");
    connect(m_btnStop, &QPushButton::clicked, this, &InstagramCollector::stopCollection);
    ctrlRow->addWidget(m_btnStop);
    layout->addLayout(ctrlRow);

    m_statusLabel = new QLabel("");
    m_statusLabel->setAlignment(Qt::AlignCenter);
    m_statusLabel->setStyleSheet("color: #888888; font-size: 12px;");
    layout->addWidget(m_statusLabel);

    // Counters & Log
    auto *counterRow = new QHBoxLayout();
    m_labelMedia = new QLabel("미디어: 0");
    m_labelMedia->setStyleSheet("color: #E4405F; font-size: 12px; font-weight: bold;");
    counterRow->addWidget(m_labelMedia);
    m_labelReels = new QLabel("릴스: 0");
    m_labelReels->setStyleSheet("color: #E4405F; font-size: 12px; font-weight: bold;");
    counterRow->addWidget(m_labelReels);
    m_labelStories = new QLabel("스토리: 0");
    m_labelStories->setStyleSheet("color: #E4405F; font-size: 12px; font-weight: bold;");
    counterRow->addWidget(m_labelStories);
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

void InstagramCollector::browsePath()
{
    QString path = QFileDialog::getExistingDirectory(this, "저장 위치 선택");
    if (!path.isEmpty()) m_pathEntry->setText(path);
}

void InstagramCollector::appendLog(const QString &text)
{
    QString time = QDateTime::currentDateTime().toString("HH:mm:ss");
    m_logText->append(QString("[%1] %2").arg(time, text));
    m_logText->verticalScrollBar()->setValue(m_logText->verticalScrollBar()->maximum());
}

void InstagramCollector::updateStatus(const QString &text)
{
    m_statusLabel->setText(text);
}

void InstagramCollector::updateCounts(int media, int reels, int stories)
{
    QTimer::singleShot(0, this, [this, media, reels, stories]() {
        m_labelMedia->setText(QString("미디어: %1").arg(media));
        m_labelReels->setText(QString("릴스: %1").arg(reels));
        m_labelStories->setText(QString("스토리: %1").arg(stories));
    });
}

void InstagramCollector::startCollection()
{
    QString sessionId = m_sessionIdEntry->text().trimmed();
    QString username = m_usernameEntry->text().trimmed().replace("@", "");

    if (sessionId.isEmpty()) {
        emit m_signals->log("❌ 세션 ID를 입력하세요");
        return;
    }
    if (username.isEmpty()) {
        emit m_signals->log("❌ 사용자명을 입력하세요");
        return;
    }

    m_isRunning = true;
    m_btnStart->setEnabled(false);
    m_btnStop->setEnabled(true);

    emit m_signals->log("🚀 Instagram 수집 시작...");

    QThread *thread = QThread::create([this]() { runCollection(); });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void InstagramCollector::stopCollection()
{
    m_isRunning = false;
    m_btnStop->setEnabled(false);
    m_btnStart->setEnabled(true);
    emit m_signals->log("⏹️ 중지 중...");
}

QString InstagramCollector::login(const QString &sessionId)
{
    // Instagram GraphQL API with session cookie
    QMap<QString, QString> headers;
    headers["Cookie"] = "sessionid=" + sessionId;
    headers["X-IG-App-ID"] = "936619743392459";
    headers["X-Requested-With"] = "XMLHttpRequest";

    HttpResponse resp = m_http->get("https://www.instagram.com/api/v1/accounts/current_user/", headers);
    if (resp.isOk()) {
        QJsonObject user = resp.json()["user"].toObject();
        return user["pk"].toString();
    }
    return QString();
}

QJsonObject InstagramCollector::getUserInfo(const QString &username, const QString &csrfToken,
                                             const QString &sessionId)
{
    QString url = QString("https://www.instagram.com/api/v1/users/web_profile_info/?username=%1").arg(username);

    QMap<QString, QString> headers;
    headers["Cookie"] = "sessionid=" + sessionId;
    if (!csrfToken.isEmpty()) headers["X-CSRFToken"] = csrfToken;
    headers["X-IG-App-ID"] = "936619743392459";
    headers["X-Requested-With"] = "XMLHttpRequest";

    HttpResponse resp = m_http->get(url, headers);
    if (resp.isOk()) {
        return resp.json()["data"].toObject()["user"].toObject();
    }
    return QJsonObject();
}

QJsonArray InstagramCollector::getUserMedia(const QString &userId, const QString &csrfToken,
                                             const QString &sessionId, int maxCount)
{
    QJsonArray allMedia;
    QString endCursor;
    bool hasNext = true;

    double igcDelay = 2.0;
    int igcConsecutiveOk = 0;
    int igcRateLimitHits = 0;

    while (hasNext && m_isRunning && (maxCount <= 0 || allMedia.size() < maxCount)) {
        QString url = QString("https://www.instagram.com/api/v1/feed/user/%1/?count=33").arg(userId);
        if (!endCursor.isEmpty()) {
            url += "&max_id=" + endCursor;
        }

        QMap<QString, QString> headers;
        headers["Cookie"] = "sessionid=" + sessionId;
        headers["X-IG-App-ID"] = "936619743392459";

        HttpResponse resp = m_http->get(url, headers);
        if (!resp.isOk()) {
            if (resp.statusCode == 429) {
                igcRateLimitHits++;
                igcConsecutiveOk = 0;
                igcDelay = qMin(igcDelay * 2.0, 30.0);
                int waitSecs = qMin(60 + (igcRateLimitHits - 1) * 30, 180);
                emit m_signals->log(QString("⚠️ Rate Limit (%1回) - %2秒 대기").arg(igcRateLimitHits).arg(waitSecs));
                for (int r = waitSecs; r > 0 && m_isRunning; --r) {
                    emit m_signals->status(QString("Rate Limit 대기 %1s").arg(r));
                    QThread::sleep(1);
                }
                continue;
            }
            break;
        }

        QJsonObject data = resp.json();
        QJsonArray items = data["items"].toArray();
        if (items.isEmpty()) break;

        for (const auto &item : items) {
            allMedia.append(item);
        }

        hasNext = data["more_available"].toBool(false);
        endCursor = data["next_max_id"].toString();

        emit m_signals->log(QString("  📦 %1개 수집...").arg(allMedia.size()));

        // Adaptive delay
        igcConsecutiveOk++;
        if (igcConsecutiveOk > 5) { igcDelay = qMax(igcDelay * 0.9, 1.0); igcRateLimitHits = 0; }
        QThread::msleep(static_cast<unsigned long>(igcDelay * 1000));
    }

    return allMedia;
}

QJsonArray InstagramCollector::getUserStories(const QString &userId, const QString &sessionId)
{
    QJsonArray result;
    QString url = QString("https://i.instagram.com/api/v1/feed/reels_media/?reel_ids=%1").arg(userId);

    QMap<QString, QString> headers;
    headers["Cookie"] = "sessionid=" + sessionId;
    headers["X-IG-App-ID"] = "936619743392459";
    headers["X-Requested-With"] = "XMLHttpRequest";
    headers["User-Agent"] = "Instagram 275.0.0.27.98 Android";

    HttpResponse resp = m_http->get(url, headers);
    if (resp.isOk()) {
        QJsonObject reels = resp.json()["reels"].toObject();
        QJsonObject reel = reels[userId].toObject();
        QJsonArray items = reel["items"].toArray();
        for (const auto &item : items)
            result.append(item);
    }

    // Also try highlights
    QString hlUrl = QString("https://i.instagram.com/api/v1/highlights/%1/highlights_tray/").arg(userId);
    HttpResponse hlResp = m_http->get(hlUrl, headers);
    if (hlResp.isOk()) {
        QJsonArray trays = hlResp.json()["tray"].toArray();
        for (const auto &tray : trays) {
            QJsonObject highlight = tray.toObject();
            QString hlId = highlight["id"].toString();
            if (hlId.isEmpty()) continue;

            QThread::msleep(1500);
            QString hlDetailUrl = QString("https://i.instagram.com/api/v1/feed/reels_media/?reel_ids=%1").arg(hlId);
            HttpResponse hlDetailResp = m_http->get(hlDetailUrl, headers);
            if (hlDetailResp.isOk()) {
                QJsonObject hlReels = hlDetailResp.json()["reels"].toObject();
                QJsonObject hlReel = hlReels[hlId].toObject();
                QJsonArray hlItems = hlReel["items"].toArray();
                for (const auto &item : hlItems)
                    result.append(item);
            }
        }
    }

    return result;
}

QJsonArray InstagramCollector::getUserReels(const QString &userId, const QString &sessionId, int maxCount)
{
    QJsonArray allReels;
    QString endCursor;
    bool hasNext = true;

    double delay = 2.0;
    int rateLimitHits = 0;

    QMap<QString, QString> headers;
    headers["Cookie"] = "sessionid=" + sessionId;
    headers["X-IG-App-ID"] = "936619743392459";
    headers["X-Requested-With"] = "XMLHttpRequest";
    headers["Content-Type"] = "application/x-www-form-urlencoded";

    while (hasNext && m_isRunning && (maxCount <= 0 || allReels.size() < maxCount)) {
        QString postBody = QString("target_user_id=%1&page_size=18").arg(userId);
        if (!endCursor.isEmpty())
            postBody += "&max_id=" + endCursor;

        HttpResponse resp = m_http->post(
            "https://i.instagram.com/api/v1/clips/user/", postBody.toUtf8(), headers);

        if (!resp.isOk()) {
            if (resp.statusCode == 429) {
                rateLimitHits++;
                delay = qMin(delay * 2.0, 30.0);
                int waitSecs = qMin(60 + (rateLimitHits - 1) * 30, 180);
                emit m_signals->log(QString("⚠️ Rate Limit — %1秒 대기").arg(waitSecs));
                for (int r = waitSecs; r > 0 && m_isRunning; --r)
                    QThread::sleep(1);
                continue;
            }
            break;
        }

        QJsonObject data = resp.json();
        QJsonArray items = data["items"].toArray();
        if (items.isEmpty()) break;

        for (const auto &item : items) {
            QJsonObject media = item.toObject()["media"].toObject();
            if (!media.isEmpty())
                allReels.append(media);
        }

        hasNext = data["paging_info"].toObject()["more_available"].toBool(false);
        endCursor = data["paging_info"].toObject()["max_id"].toString();

        emit m_signals->log(QString("  🎬 릴스 %1개 수집...").arg(allReels.size()));
        QThread::msleep(static_cast<unsigned long>(delay * 1000));
    }

    return allReels;
}

QJsonArray InstagramCollector::getUserTaggedMedia(const QString &userId, const QString &sessionId, int maxCount)
{
    QJsonArray allMedia;
    QString endCursor;
    bool hasNext = true;

    double delay = 2.0;
    int rateLimitHits = 0;

    while (hasNext && m_isRunning && (maxCount <= 0 || allMedia.size() < maxCount)) {
        QString url = QString("https://i.instagram.com/api/v1/usertags/%1/feed/?count=33").arg(userId);
        if (!endCursor.isEmpty())
            url += "&max_id=" + endCursor;

        QMap<QString, QString> headers;
        headers["Cookie"] = "sessionid=" + sessionId;
        headers["X-IG-App-ID"] = "936619743392459";

        HttpResponse resp = m_http->get(url, headers);
        if (!resp.isOk()) {
            if (resp.statusCode == 429) {
                rateLimitHits++;
                delay = qMin(delay * 2.0, 30.0);
                int waitSecs = qMin(60 + (rateLimitHits - 1) * 30, 180);
                emit m_signals->log(QString("⚠️ Rate Limit — %1秒 대기").arg(waitSecs));
                for (int r = waitSecs; r > 0 && m_isRunning; --r)
                    QThread::sleep(1);
                continue;
            }
            break;
        }

        QJsonObject data = resp.json();
        QJsonArray items = data["items"].toArray();
        if (items.isEmpty()) break;

        for (const auto &item : items)
            allMedia.append(item);

        hasNext = data["more_available"].toBool(false);
        endCursor = data["next_max_id"].toString();

        emit m_signals->log(QString("  🏷️ 태그 %1개 수집...").arg(allMedia.size()));
        QThread::msleep(static_cast<unsigned long>(delay * 1000));
    }

    return allMedia;
}

int InstagramCollector::downloadMediaItems(const QJsonArray &items, const QString &username,
                                            const QString &mediaDir, const QString &sessionId,
                                            const QString &typeLabel)
{
    int downloaded = 0;
    Q_UNUSED(sessionId);

    for (int i = 0; i < items.size() && m_isRunning; ++i) {
        QJsonObject post = items[i].toObject();

        QJsonArray candidates;
        if (post.contains("carousel_media")) {
            QJsonArray carousel = post["carousel_media"].toArray();
            for (const auto &item : carousel)
                candidates.append(item);
        } else {
            candidates.append(post);
        }

        for (int j = 0; j < candidates.size(); ++j) {
            QJsonObject media = candidates[j].toObject();
            QString mediaUrl;

            if (media.contains("video_versions")) {
                QJsonArray videos = media["video_versions"].toArray();
                if (!videos.isEmpty())
                    mediaUrl = videos[0].toObject()["url"].toString();
            }
            if (mediaUrl.isEmpty() && media.contains("image_versions2")) {
                QJsonArray images = media["image_versions2"].toObject()["candidates"].toArray();
                if (!images.isEmpty())
                    mediaUrl = images[0].toObject()["url"].toString();
            }

            if (mediaUrl.isEmpty()) {
                QString pk = post["pk"].toVariant().toString();
                emit m_signals->log(QString("  ⚠️ [%1] 미디어 %2/%3 URL 없음 (pk=%4, type=%5)")
                    .arg(typeLabel).arg(j).arg(candidates.size()).arg(pk).arg(media["media_type"].toInt()));
                continue;
            }

            if (!mediaUrl.isEmpty()) {
                QString ext = mediaUrl.contains("mp4") ? ".mp4" : ".jpg";
                QString pk = post["pk"].toVariant().toString();
                if (pk.isEmpty()) pk = post["id"].toVariant().toString();
                QString filename = QString("%1_%2%3").arg(pk).arg(j).arg(ext);
                QString filepath = mediaDir + "/" + filename;

                if (QFileInfo::exists(filepath) && QFileInfo(filepath).size() > 0) {
                    downloaded++;
                    continue;
                }

                if (m_http->downloadFile(mediaUrl, filepath)) {
                    QString takenAt = post["taken_at"].toVariant().toString();
                    QDateTime dt;
                    if (!takenAt.isEmpty()) {
                        dt = QDateTime::fromSecsSinceEpoch(takenAt.toLongLong());
                        Common::setFileTimes(filepath, dt);
                    }

                    QString caption = post["caption"].toObject()["text"].toString().left(200);
                    QString code = post["code"].toString();
                    QString postUrl = code.isEmpty() ? QString() :
                        QString("https://www.instagram.com/p/%1/").arg(code);

                    QString exifDateStr;
                    if (dt.isValid())
                        exifDateStr = dt.toUTC().toString(Qt::ISODate);
                    Common::addExifMetadata(filepath, "@" + username, caption,
                        "Instagram @" + username, postUrl, exifDateStr);
                    if (!postUrl.isEmpty())
                        FileHelper::setFinderComment(filepath, postUrl);

                    downloaded++;
                    emit m_signals->log(QString("  📥 [%1] %2").arg(typeLabel, filename));
                }
            }
        }
    }
    return downloaded;
}

void InstagramCollector::runCollection()
{
    QString sessionId = m_sessionIdEntry->text().trimmed();
    QString username = m_usernameEntry->text().trimmed().replace("@", "");
    QString maxCountStr = m_countEntry->text().trimmed();
    int maxCount = maxCountStr.isEmpty() ? 0 : maxCountStr.toInt();

    emit m_signals->status("로그인 확인 중...");

    // Verify session
    QString myUserId = login(sessionId);
    if (myUserId.isEmpty()) {
        emit m_signals->log("❌ 세션이 만료되었습니다. 새 세션 ID를 입력하세요.");
        emit m_signals->status("인증 실패");
        m_isRunning = false;
        QTimer::singleShot(0, this, [this]() {
            m_btnStart->setEnabled(true);
            m_btnStop->setEnabled(false);
        });
        return;
    }

    emit m_signals->log("✅ 로그인 확인 완료");

    // Get user info
    QJsonObject userInfo = getUserInfo(username, QString(), sessionId);
    if (userInfo.isEmpty()) {
        emit m_signals->log("❌ 사용자를 찾을 수 없습니다: " + username);
        emit m_signals->status("사용자 없음");
        m_isRunning = false;
        QTimer::singleShot(0, this, [this]() {
            m_btnStart->setEnabled(true);
            m_btnStop->setEnabled(false);
        });
        return;
    }

    QString userId = QString::number(userInfo["pk"].toVariant().toLongLong());
    QString fullName = userInfo["full_name"].toString();
    QString biography = userInfo["biography"].toString();
    bool isVerified = userInfo["is_verified"].toBool();
    emit m_signals->log(QString("👤 %1 (%2)%3").arg(fullName, username, isVerified ? " ✓" : ""));
    if (!biography.isEmpty())
        emit m_signals->log(QString("  📝 %1").arg(biography.left(100)));

    // Save path
    QString savePath = m_pathEntry->text().trimmed();
    QString instagramDir = savePath + "/instagram";
    QDir().mkpath(instagramDir);
    QString userDir = instagramDir + "/" + username;
    QDir().mkpath(userDir);
    QString mediaDir = userDir + "/media";
    QDir().mkpath(mediaDir);

    // ── 프로필 다운로드 (아바타, 프로필 정보) ──
    {
        QString dateTag = QDateTime::currentDateTime().toString("yyyyMMdd");
        QString profilePicUrl = userInfo["profile_pic_url_hd"].toString();
        if (profilePicUrl.isEmpty()) {
            QJsonArray hdVersions = userInfo["hd_profile_pic_versions"].toArray();
            int maxW = 0;
            for (const auto &v : hdVersions) {
                int w = v.toObject()["width"].toInt();
                if (w > maxW) { maxW = w; profilePicUrl = v.toObject()["url"].toString(); }
            }
            if (profilePicUrl.isEmpty()) profilePicUrl = userInfo["profile_pic_url"].toString();
        }
        if (!profilePicUrl.isEmpty()) {
            QString ext = "jpg";
            if (profilePicUrl.contains(".png")) ext = "png";
            QString avatarPath = userDir + "/avatar_" + dateTag + "." + ext;
            if (!QFile::exists(avatarPath)) {
                if (m_http->downloadFile(profilePicUrl, avatarPath))
                    emit m_signals->log(QString("  📸 프로필 사진 저장"));
            }
        }

        // profile.json
        QJsonObject profileData;
        profileData["user_id"] = userId;
        profileData["username"] = username;
        profileData["full_name"] = fullName;
        profileData["biography"] = biography;
        profileData["external_url"] = userInfo["external_url"].toString();
        profileData["is_verified"] = isVerified;
        profileData["is_private"] = userInfo["is_private"].toBool();
        profileData["follower_count"] = userInfo["edge_followed_by"].toObject()["count"].toInt();
        profileData["following_count"] = userInfo["edge_follow"].toObject()["count"].toInt();
        profileData["post_count"] = userInfo["edge_owner_to_timeline_media"].toObject()["count"].toInt();
        profileData["profile_pic_url"] = profilePicUrl;
        profileData["fetched_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);

        QFile pf(userDir + "/profile.json");
        if (pf.open(QIODevice::WriteOnly)) {
            pf.write(QJsonDocument(profileData).toJson(QJsonDocument::Indented));
            pf.close();
            emit m_signals->log("  📄 profile.json 저장");
        }
    }

    QJsonArray mediaData;
    int mediaDownloaded = 0, reelsDownloaded = 0, storiesDownloaded = 0, taggedDownloaded = 0;

    // Posts
    if (m_chkPosts->isChecked() && m_isRunning) {
        emit m_signals->log("📸 포스트 수집 중...");
        emit m_signals->status("포스트 수집 중...");

        QJsonArray posts = getUserMedia(userId, QString(), sessionId, maxCount);
        emit m_signals->log(QString("  %1개 포스트 발견").arg(posts.size()));

        mediaDownloaded = downloadMediaItems(posts, username, mediaDir, sessionId, "포스트");

        for (int i = 0; i < posts.size(); ++i) {
            QJsonObject post = posts[i].toObject();
            QJsonObject meta;
            meta["id"] = post["pk"].toVariant().toString();
            meta["type"] = post["media_type"].toInt();
            meta["timestamp"] = post["taken_at"].toVariant().toString();
            meta["caption"] = post["caption"].toObject()["text"].toString().left(500);
            meta["likes"] = post["like_count"].toInt();
            meta["comments"] = post["comment_count"].toInt();
            mediaData.append(meta);
        }

        updateCounts(mediaDownloaded, reelsDownloaded, storiesDownloaded);
    }

    // Reels
    if (m_chkReels->isChecked() && m_isRunning) {
        emit m_signals->log("🎬 릴스 수집 중...");
        emit m_signals->status("릴스 수집 중...");

        QString reelsDir = userDir + "/reels";
        QDir().mkpath(reelsDir);

        QJsonArray reels = getUserReels(userId, sessionId, maxCount);
        emit m_signals->log(QString("  %1개 릴스 발견").arg(reels.size()));

        reelsDownloaded = downloadMediaItems(reels, username, reelsDir, sessionId, "릴스");
        updateCounts(mediaDownloaded, reelsDownloaded, storiesDownloaded);
    }

    // Stories + Highlights
    if (m_chkStories->isChecked() && m_isRunning) {
        emit m_signals->log("📖 스토리/하이라이트 수집 중...");
        emit m_signals->status("스토리 수집 중...");

        QString storiesDir = userDir + "/stories";
        QDir().mkpath(storiesDir);

        QJsonArray stories = getUserStories(userId, sessionId);
        emit m_signals->log(QString("  %1개 스토리/하이라이트 발견").arg(stories.size()));

        storiesDownloaded = downloadMediaItems(stories, username, storiesDir, sessionId, "스토리");
        updateCounts(mediaDownloaded, reelsDownloaded, storiesDownloaded);
    }

    // Tagged
    if (m_chkTagged->isChecked() && m_isRunning) {
        emit m_signals->log("🏷️ 태그된 게시물 수집 중...");
        emit m_signals->status("태그 수집 중...");

        QString taggedDir = userDir + "/tagged";
        QDir().mkpath(taggedDir);

        QJsonArray tagged = getUserTaggedMedia(userId, sessionId, maxCount);
        emit m_signals->log(QString("  %1개 태그 게시물 발견").arg(tagged.size()));

        taggedDownloaded = downloadMediaItems(tagged, username, taggedDir, sessionId, "태그");
    }

    // Final save
    if (m_chkExcel->isChecked() && !mediaData.isEmpty()) {
        emit m_signals->log("📊 Excel 저장...");
        saveToExcel(mediaData, userDir, username);
    }

    emit m_signals->log(QString("🎉 완료! (포스트: %1, 릴스: %2, 스토리: %3, 태그: %4)")
                             .arg(mediaDownloaded).arg(reelsDownloaded)
                             .arg(storiesDownloaded).arg(taggedDownloaded));
    emit m_signals->status("완료!");

    m_isRunning = false;
    QTimer::singleShot(0, this, [this]() {
        m_btnStart->setEnabled(true);
        m_btnStop->setEnabled(false);
    });
}

void InstagramCollector::saveToExcel(const QJsonArray &mediaData, const QString &saveDir,
                                      const QString &username)
{
    ExcelWriter writer;

    QStringList headers = {"ID", "Type", "Timestamp", "Caption", "Likes", "Comments"};
    writer.writeHeader(headers, QColor("#E4405F"));

    int row = 2;
    for (const auto &val : mediaData) {
        QJsonObject m = val.toObject();
        QString typeStr;
        switch (m["type"].toInt()) {
            case 1: typeStr = "Photo"; break;
            case 2: typeStr = "Video"; break;
            case 8: typeStr = "Album"; break;
            default: typeStr = "Other"; break;
        }

        writer.writeRow(row++, {
            m["id"].toString(),
            typeStr,
            m["timestamp"].toString(),
            m["caption"].toString(),
            QString::number(m["likes"].toInt()),
            QString::number(m["comments"].toInt())
        });
    }

    writer.setColumnWidth(1, 20);
    writer.setColumnWidth(2, 10);
    writer.setColumnWidth(3, 20);
    writer.setColumnWidth(4, 50);
    writer.setColumnWidth(5, 10);
    writer.setColumnWidth(6, 10);

    QString filepath = saveDir + "/" + username + "_media.xlsx";
    writer.save(filepath);
    FileHelper::setDownloadMeta(filepath, "ABIWA Instagram");
    emit m_signals->log(QString("  저장: %1").arg(filepath));
}
