#include "TwitterCollector.h"
#include "core/MiyoBackend.h"
#include "core/Common.h"
#include "utils/HttpClient.h"
#include "utils/ExcelWriter.h"
#include "xlsxdocument.h"
#include "utils/FileHelper.h"
#include "utils/DiskJsonBuffer.h"
#include <QThread>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkCookie>
#include <QUrl>
#include <QUrlQuery>
#include <QDateTime>
#include <QTimeZone>
#include <QProcess>
#include <QProcessEnvironment>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QRegularExpression>
#include <QSet>
#include <QQueue>

const QString TwitterCollector::GRAPHQL_BASE = "https://x.com/i/api/graphql";
const QString TwitterCollector::SEARCH_TIMELINE_URL = "https://x.com/i/api/graphql/flaR-PUMshxFWZWPNpq4zA/SearchTimeline";
const QString TwitterCollector::USER_BY_SCREEN_NAME_URL = "https://x.com/i/api/graphql/NimuplG1OB7Fd2btCLdBOw/UserByScreenName";
const QString TwitterCollector::USER_TWEETS_URL = "https://x.com/i/api/graphql/QWF3SzpHmykQHsQMixG0cg/UserTweets";

TwitterCollector::TwitterCollector(MiyoBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_http(new HttpClient(this))
{
    m_http->setTimeout(30000);
}

TwitterCollector::~TwitterCollector()
{
    // multi-target: MiyoBackend가 매 collect()마다 collector를 새로 만들므로
    // 이전 collector가 destroy될 때 daemon이 leak되지 않도록 명시적으로 정리.
    stopDaemon();
}

void TwitterCollector::setupClient(const QString &authToken, const QString &ct0)
{
    // ★ 토큰이 바뀌면 기존 데몬 강제 종료 — 안 그러면 multi-target × per-account 시나리오에서
    //   1번째 target(계정A)로 시작된 데몬이 2번째 target(계정B) 요청에도 재사용되어
    //   모든 target이 계정A로 수집되는 버그가 발생한다.
    const bool tokenChanged = (m_authToken != authToken) || (m_ct0 != ct0);
    if (tokenChanged && m_daemon && m_daemon->state() == QProcess::Running) {
        stopDaemon();
    }
    m_authToken = authToken;
    m_ct0 = ct0;
    m_csrfToken = ct0;
}

// ── Persistent Python daemon (twikit) ──

bool TwitterCollector::startDaemon()
{
    if (m_daemon && m_daemon->state() == QProcess::Running && m_daemonReady) {
        return true; // Already running
    }

    stopDaemon(); // Clean up any previous

    // Find daemon script: bundled tools dir → dev fallback
    QString scriptPath = Common::bundledToolsDir() + "/twitter_daemon.py";
    if (!QFile::exists(scriptPath)) {
        scriptPath = QCoreApplication::applicationDirPath() + "/../../resources/tools/twitter_daemon.py";
    }
    if (!QFile::exists(scriptPath)) {
        m_backend->log("twitter_daemon.py not found", "error", "twitter");
        return false;
    }

    QJsonObject initArgs;
    initArgs["auth_token"] = m_authToken;
    initArgs["ct0"] = m_ct0;
    QString argsJson = QString::fromUtf8(QJsonDocument(initArgs).toJson(QJsonDocument::Compact));

    m_daemon = new QProcess();  // 부모 없음 — 워커 스레드에서 생성되므로 this(메인스레드) 지정하면 크래시
    m_daemon->setProcessEnvironment(Common::bundledProcessEnv());

    // 번들 Python 우선 → system fallback
    QStringList pythons = Common::pythonCandidates();

    bool started = false;
    for (const auto &python : pythons) {
        m_daemon->start(python, {scriptPath, argsJson});
        if (m_daemon->waitForStarted(5000)) {
            started = true;
            m_backend->log("Daemon: using "+ python, "info", "twitter");
            break;
        }
    }

    if (!started) {
        m_backend->log("Daemon: Python not found", "error", "twitter");
        delete m_daemon;
        m_daemon = nullptr;
        return false;
    }

    // Wait for "ready"signal (up to 60 seconds — may include twikit upgrade)
    if (!m_daemon->waitForReadyRead(60000)) {
        m_backend->log("Daemon: timeout waiting for ready", "error", "twitter");
        stopDaemon();
        return false;
    }

    // 데몬이 info 메시지를 먼저 보낼 수 있음 → "status":"ready"올 때까지 읽기
    for (int attempt = 0; attempt < 40; ++attempt) {
        QByteArray line = m_daemon->readLine().trimmed();
        if (line.isEmpty()) {
            if (!m_daemon->waitForReadyRead(10000)) break;
            continue;
        }
        QJsonObject msg = QJsonDocument::fromJson(line).object();
        if (msg["status"].toString() == "ready") {
            m_daemonReady = true;
            bool tidOk = msg["tid"].toBool(false);
            m_backend->log(QString("Daemon: ready (%1)").arg(tidOk ? "TID active": "cached hashes only"), "success", "twitter");

            // 시작 시 해시 자동 갱신 — 404 방지
            QJsonObject updateCmd;
            updateCmd["action"] = "update_endpoints";
            QJsonObject updateResp = sendDaemonCommand(updateCmd, 30000);
            if (updateResp["status"].toString() == "ok") {
                m_backend->log("GraphQL 해시 최신 상태로 갱신 완료", "success", "twitter");
            }

            return true;
        }
        if (msg.contains("error") && !msg.contains("info")) {
            m_backend->log("Daemon init failed: "+ msg["error"].toString(), "error", "twitter");
            stopDaemon();
            return false;
        }
        // info 메시지는 로그만 하고 계속 읽기
        if (msg.contains("info")) {
            m_backend->log("Daemon: "+ msg["info"].toString(), "info", "twitter");
        }
    }

    m_backend->log("Daemon: ready signal not received", "error", "twitter");
    stopDaemon();
    return false;
}

void TwitterCollector::stopDaemon()
{
    m_daemonReady = false;
    if (m_daemon) {
        if (m_daemon->state() == QProcess::Running) {
            m_daemon->write("{\"action\":\"quit\"}\n");
            m_daemon->waitForFinished(3000);
            if (m_daemon->state() == QProcess::Running) {
                m_daemon->kill();
                m_daemon->waitForFinished(2000);
            }
        }
        delete m_daemon;
        m_daemon = nullptr;
    }
}

QJsonObject TwitterCollector::sendDaemonCommand(const QJsonObject &cmd, int timeoutMs)
{
    // Periodic daemon restart every 2000 requests to prevent Python memory bloat
    if (m_daemon && m_daemonReady && m_daemonRequestCount >= 2000) {
        m_backend->log("Daemon: periodic restart (memory hygiene)", "info", "twitter");
        stopDaemon();
        m_daemonRequestCount = 0;
    }

    if (!m_daemon || m_daemon->state() != QProcess::Running || !m_daemonReady) {
        // Try to restart daemon — with network recovery wait (sleep/wake 복구)
        m_daemonRequestCount = 0;
        for (int retry = 0; retry < 5; ++retry) {
            if (startDaemon()) break;
            // 네트워크 미복구 가능성 — 대기 후 재시도
            m_backend->log(QString("데몬 재시작 실패, %1초 후 재시도 (%2/5)...").arg(10 * (retry + 1)).arg(retry + 1), "warning", "twitter");
            QThread::sleep(10 * (retry + 1));
        }
        if (!m_daemon || m_daemon->state() != QProcess::Running || !m_daemonReady) {
            return QJsonObject{{"error", "Daemon not available after retries"}};
        }
    }
    m_daemonRequestCount++;

    QByteArray cmdLine = QJsonDocument(cmd).toJson(QJsonDocument::Compact) + "\n";
    m_daemon->write(cmdLine);

    // Read complete JSON line — response can be very large (hundreds of KB)
    // Must accumulate chunks until we see a newline
    QByteArray buffer;
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) break;

        if (!m_daemon->waitForReadyRead(qMin(remaining, 5000))) {
            if (m_daemon->state() != QProcess::Running) {
                m_backend->log("Daemon: process died", "error", "twitter");
                stopDaemon();
                return QJsonObject{{"error", "Daemon process died"}};
            }
            continue; // Timeout on this chunk, try again
        }

        buffer += m_daemon->readAll();

        // Check if we have a complete line (JSON ends with newline)
        if (buffer.contains('\n')) break;
    }

    if (!buffer.contains('\n')) {
        m_backend->log(QString("Daemon: incomplete response (%1 bytes, %2ms)")
                           .arg(buffer.size()).arg(timer.elapsed()), "error", "twitter");
        stopDaemon();
        return QJsonObject{{"error", "Daemon incomplete response"}};
    }

    // Process all complete lines — daemon may emit info/log lines before the actual response
    // (e.g., auto-repair messages). The actual response is the last non-info line.
    // CRITICAL: after processing info lines, must keep reading for actual response.
    QJsonObject result;
    bool gotResult = false;

    while (true) {
        // Process all complete lines in buffer
        while (buffer.contains('\n')) {
            int newlineIdx = buffer.indexOf('\n');
            QByteArray line = buffer.left(newlineIdx).trimmed();
            buffer = buffer.mid(newlineIdx + 1);

            if (line.isEmpty()) continue;

            QJsonParseError parseError;
            QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
            if (!doc.isObject()) {
                m_backend->log(QString("Daemon: JSON parse error: %1").arg(parseError.errorString()), "warning", "twitter");
                continue;
            }

            QJsonObject obj = doc.object();

            // Info messages from daemon (auto-repair logs) — display and continue
            if (obj.contains("info")) {
                m_backend->log("Daemon: "+ obj["info"].toString(), "info", "twitter");
                continue;
            }

            // Actual response (has "status", "body", "error", etc.)
            result = obj;
            gotResult = true;
        }

        // If we got a real result, we're done
        if (gotResult) break;

        // No result yet (only info lines so far) — read more data from daemon
        if (timer.elapsed() >= timeoutMs) break;
        int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) break;

        if (!m_daemon->waitForReadyRead(qMin(remaining, 5000))) {
            if (m_daemon->state() != QProcess::Running) {
                m_backend->log("Daemon: process died while waiting for response", "error", "twitter");
                stopDaemon();
                return QJsonObject{{"error", "Daemon process died"}};
            }
            continue; // Keep waiting
        }
        buffer += m_daemon->readAll();
    }

    if (!gotResult) {
        return QJsonObject{{"error", "No valid response from daemon"}};
    }

    return result;
}

QMap<QString, QString> TwitterCollector::getHeaders() const
{
    QMap<QString, QString> headers;
    // Match twikit 2.3.3 _base_headers exactly
    headers["Authorization"] = "Bearer AAAAAAAAAAAAAAAAAAAAANRILgAAAAAAnNwIzUejRCOuH5E6I8xnZz4puTs%3D1Zv7ttfk8LF81IUq16cHjhLTvJu4FA33AGWWjCpTnA";
    headers["Content-Type"] = "application/json";
    headers["X-Twitter-Auth-Type"] = "OAuth2Session";
    headers["X-Twitter-Active-User"] = "yes";
    headers["Referer"] = "https://x.com/";
    headers["User-Agent"] = "Mozilla/5.0 (Macintosh; Intel Mac OS X 14_6_1) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/17.5 Safari/605.1.15";
    headers["Accept-Language"] = "ja";
    headers["X-Twitter-Client-Language"] = "ja";
    // Auth cookies & CSRF
    headers["Cookie"] = QString("auth_token=%1; ct0=%2").arg(m_authToken, m_ct0);
    headers["X-Csrf-Token"] = m_csrfToken;
    return headers;
}

bool TwitterCollector::initTransactionIds()
{
    m_tidInitialized = false;
    m_transactionIds.clear();

    // Find the Python helper script
    QString scriptPath = Common::bundledToolsDir() + "/twitter_tid.py";
    if (!QFile::exists(scriptPath)) {
        scriptPath = QCoreApplication::applicationDirPath() + "/../../resources/tools/twitter_tid.py";
    }
    if (!QFile::exists(scriptPath)) {
        m_backend->log("twitter_tid.py not found, skipping TID", "warning", "twitter");
        return false;
    }

    // All GraphQL API paths that need TIDs
    QJsonArray paths;
    paths.append("/i/api/graphql/NimuplG1OB7Fd2btCLdBOw/UserByScreenName");
    paths.append("/i/api/graphql/QWF3SzpHmykQHsQMixG0cg/UserTweets");
    paths.append("/i/api/graphql/flaR-PUMshxFWZWPNpq4zA/SearchTimeline");
    paths.append("/i/api/graphql/IohM3gxQHfvWePH5E3KuNA/Likes");
    paths.append("/i/api/graphql/qToeLeMs43Q8cr7tRYXmaQ/Bookmarks");
    paths.append("/i/api/graphql/gC_lyAxZOptAMLCJX5UhWw/Followers");
    paths.append("/i/api/graphql/2vUj-_Ek-UmBVDNtd8OnQA/Following");
    paths.append("/i/api/graphql/nBS-WpgA6ZG0CyNHD517JQ/TweetDetail");

    QJsonObject args;
    args["auth_token"] = m_authToken;
    args["ct0"] = m_ct0;
    args["paths"] = paths;

    QString argsJson = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));

    QProcess proc;
    proc.setProcessEnvironment(Common::bundledProcessEnv());

    QStringList pythons = Common::pythonCandidates();
    bool started = false;
    for (const auto &python : pythons) {
        proc.start(python, {scriptPath, argsJson});
        if (proc.waitForStarted(5000)) {
            started = true;
            m_backend->log("TID: using "+ python, "info", "twitter");
            break;
        }
    }

    if (!started) {
        m_backend->log("Python not found for TID generation", "warning", "twitter");
        return false;
    }

    if (!proc.waitForFinished(30000)) {
        proc.kill();
        m_backend->log("TID generation timed out", "warning", "twitter");
        return false;
    }

    QByteArray output = proc.readAllStandardOutput();
    QByteArray errors = proc.readAllStandardError();

    if (proc.exitCode() != 0) {
        QString errMsg = QString::fromUtf8(errors).trimmed();
        QString outMsg = QString::fromUtf8(output).trimmed();
        m_backend->log("TID failed (exit "+ QString::number(proc.exitCode()) + "): "+ (errMsg.isEmpty() ? outMsg : errMsg).left(300), "warning", "twitter");
        return false;
    }

    QJsonDocument doc = QJsonDocument::fromJson(output);
    if (!doc.isObject()) {
        m_backend->log("TID: invalid JSON: "+ QString::fromUtf8(output).left(200), "warning", "twitter");
        return false;
    }

    QJsonObject result = doc.object();
    if (result.contains("error")) {
        m_backend->log("TID error: "+ result["error"].toString(), "warning", "twitter");
        return false;
    }

    for (auto it = result.begin(); it != result.end(); ++it) {
        m_transactionIds[it.key()] = it.value().toString();
    }

    m_tidInitialized = true;
    m_backend->log(QString("Transaction IDs generated: %1 endpoints").arg(m_transactionIds.size()), "success", "twitter");
    return true;
}

QString TwitterCollector::getTransactionId(const QString &urlPath)
{
    // Extract path from full URL if needed
    QString path = urlPath;
    if (path.startsWith("https://")) {
        QUrl url(path);
        path = url.path();
    }
    return m_transactionIds.value(path);
}

QMap<QString, QString> TwitterCollector::getHeadersWithTid(const QString &urlPath)
{
    auto headers = getHeaders();
    if (!m_tidInitialized) {
        initTransactionIds();
    }
    QString tid = getTransactionId(urlPath);
    if (!tid.isEmpty()) {
        headers["X-Client-Transaction-Id"] = tid;
    }
    return headers;
}

QJsonObject TwitterCollector::callTwikitApi(const QJsonObject &args)
{
    // Find the Python API proxy script
    QString scriptPath = Common::bundledToolsDir() + "/twitter_api.py";
    if (!QFile::exists(scriptPath)) {
        scriptPath = QCoreApplication::applicationDirPath() + "/../../resources/tools/twitter_api.py";
    }
    if (!QFile::exists(scriptPath)) {
        return QJsonObject{{"error", "twitter_api.py not found"}};
    }

    // Build args JSON with auth credentials
    QJsonObject fullArgs = args;
    fullArgs["auth_token"] = m_authToken;
    fullArgs["ct0"] = m_ct0;

    QString argsJson = QString::fromUtf8(QJsonDocument(fullArgs).toJson(QJsonDocument::Compact));

    QProcess proc;
    proc.setProcessEnvironment(Common::bundledProcessEnv());

    QStringList pyList = Common::pythonCandidates();
    bool pyStarted = false;
    for (const auto &py : pyList) {
        proc.start(py, {scriptPath, argsJson});
        if (proc.waitForStarted(5000)) { pyStarted = true; break; }
    }
    if (!pyStarted) {
        return QJsonObject{{"error", "Python not found"}};
    }

    if (!proc.waitForFinished(60000)) {
        proc.kill();
        return QJsonObject{{"error", "API call timed out"}};
    }

    QByteArray output = proc.readAllStandardOutput();
    QByteArray errors = proc.readAllStandardError();

    if (proc.exitCode() != 0) {
        QString errMsg = QString::fromUtf8(errors).trimmed();
        QString outMsg = QString::fromUtf8(output).trimmed();
        return QJsonObject{{"error", (errMsg.isEmpty() ? outMsg : errMsg).left(500)}};
    }

    QJsonDocument doc = QJsonDocument::fromJson(output);
    if (!doc.isObject()) {
        return QJsonObject{{"error", "Invalid JSON from API proxy: "+ QString::fromUtf8(output).left(200)}};
    }

    return doc.object();
}

// Matches twikit 2.3.3 FEATURES exactly
static QJsonObject defaultFeatures()
{
    QJsonObject f;
    f["creator_subscriptions_tweet_preview_api_enabled"] = true;
    f["c9s_tweet_anatomy_moderator_badge_enabled"] = true;
    f["tweetypie_unmention_optimization_enabled"] = true;
    f["responsive_web_edit_tweet_api_enabled"] = true;
    f["graphql_is_translatable_rweb_tweet_is_translatable_enabled"] = true;
    f["view_counts_everywhere_api_enabled"] = true;
    f["longform_notetweets_consumption_enabled"] = true;
    f["responsive_web_twitter_article_tweet_consumption_enabled"] = true;
    f["tweet_awards_web_tipping_enabled"] = false;
    f["longform_notetweets_rich_text_read_enabled"] = true;
    f["longform_notetweets_inline_media_enabled"] = true;
    f["rweb_video_timestamps_enabled"] = true;
    f["responsive_web_graphql_exclude_directive_enabled"] = true;
    f["verified_phone_label_enabled"] = false;
    f["freedom_of_speech_not_reach_fetch_enabled"] = true;
    f["standardized_nudges_misinfo"] = true;
    f["tweet_with_visibility_results_prefer_gql_limited_actions_policy_enabled"] = true;
    f["responsive_web_media_download_video_enabled"] = false;
    f["responsive_web_graphql_skip_user_profile_image_extensions_enabled"] = false;
    f["responsive_web_graphql_timeline_navigation_enabled"] = true;
    f["responsive_web_enhance_cards_enabled"] = false;
    return f;
}

QJsonObject TwitterCollector::getUserByScreenName(const QString &screenName)
{
    QJsonObject cmd;
    cmd["action"] = "user_by_screen_name";
    cmd["screen_name"] = screenName;

    // ★ ReadTimeout / TimeoutException 자동 재시도 (최대 3회) — Twitter 가끔 응답 늦음
    QJsonObject resp;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        resp = sendDaemonCommand(cmd);
        if (!resp.contains("error")) break;
        QString errMsg = resp["error"].toString();
        bool isTimeout = errMsg.contains("ReadTimeout", Qt::CaseInsensitive)
                      || errMsg.contains("ConnectTimeout", Qt::CaseInsensitive)
                      || errMsg.contains("TimeoutException", Qt::CaseInsensitive)
                      || errMsg.contains("timeout", Qt::CaseInsensitive);
        if (isTimeout && attempt < 3) {
            int waitSec = 5 * attempt;
            m_backend->log(QString("Twitter API timeout (시도 %1/3) — %2초 대기 후 재시도").arg(attempt).arg(waitSec),
                "warning", "twitter");
            QThread::sleep(waitSec);
            continue;
        }
        break;  // timeout 아닌 에러 또는 마지막 시도 → log + return
    }
    if (resp.contains("error")) {
        QString errMsg = resp["error"].toString();
        if (errMsg.isEmpty()) errMsg = "(empty error)";
        m_backend->log("getUserByScreenName error: "+ errMsg, "error", "twitter");
        if (resp.contains("traceback"))
            m_backend->log("Traceback: "+ resp["traceback"].toString().left(500), "error", "twitter");
        return QJsonObject();
    }

    int status = resp["status"].toInt();
    if (status == 404) {
        m_backend->log("getUserByScreenName HTTP 404 → 엔드포인트 갱신 시도", "warning", "twitter");
        QJsonObject updateCmd;
        updateCmd["action"] = "update_endpoints";
        QJsonObject updateResp = sendDaemonCommand(updateCmd, 30000);
        if (updateResp["status"].toString() == "ok") {
            m_backend->log("GraphQL 해시 갱신 완료, 재시도...", "success", "twitter");
            QJsonObject retryResp = sendDaemonCommand(cmd);
            if (!retryResp.contains("error") && retryResp["status"].toInt() == 200) {
                resp = retryResp;
                status = 200;
            }
        }
    }
    if (status != 200) {
        m_backend->log(QString("getUserByScreenName HTTP %1").arg(status), "error", "twitter");
        QString body = resp["body"].toString();
        if (!body.isEmpty()) m_backend->log("Response: "+ body.left(300), "error", "twitter");
        return QJsonObject();
    }

    // Debug: log response structure
    if (resp.contains("_debug_keys")) {
        m_backend->log("API result keys: "+ resp["_debug_keys"].toString(), "info", "twitter");
        m_backend->log("API legacy keys: "+ resp["_debug_legacy_keys"].toString(), "info", "twitter");
        m_backend->log("API typename: "+ resp["_debug_typename"].toString(), "info", "twitter");
    }

    QJsonObject body = resp["body"].toObject();
    QJsonObject result = body["data"].toObject()["user"].toObject()["result"].toObject();
    if (result.isEmpty()) {
        m_backend->log("API returned empty user data", "error", "twitter");
        QString bodyStr = resp["body"].toString();
        if (!bodyStr.isEmpty()) m_backend->log("body (str): "+ bodyStr.left(300), "error", "twitter");
    } else {
        QJsonObject legacy = result["legacy"].toObject();
        // Twitter API 2025+: name moved out of legacy, try to recover
        if (legacy["name"].toString().isEmpty()) {
            // Try core.user_results.result.legacy.name
            QString coreName = result["core"].toObject()["user_results"].toObject()
                                   ["result"].toObject()["legacy"].toObject()["name"].toString();
            if (!coreName.isEmpty()) {
                legacy["name"] = coreName;
                result["legacy"] = legacy;
            }
        }
        // Twitter API 2025+: profile_image_url_https moved to avatar
        if (legacy["profile_image_url_https"].toString().isEmpty()) {
            QJsonObject avatar = result["avatar"].toObject();
            QString avatarUrl;
            if (!avatar.isEmpty()) {
                // Try known avatar sub-fields
                avatarUrl = avatar["image_url"].toString();
                if (avatarUrl.isEmpty()) avatarUrl = avatar["url"].toString();
                if (avatarUrl.isEmpty()) avatarUrl = avatar["image_url_https"].toString();
                // Log avatar structure for debugging
                QStringList avKeys;
                for (auto it = avatar.begin(); it != avatar.end(); ++it) avKeys << it.key();
                m_backend->log("Avatar keys: "+ avKeys.join(","), "info", "twitter");
                m_backend->log("Avatar JSON: "+ QString(QJsonDocument(avatar).toJson(QJsonDocument::Compact)).left(300), "info", "twitter");
            }
            // Also try result["avatar"] as string
            if (avatarUrl.isEmpty() && result["avatar"].isString()) {
                avatarUrl = result["avatar"].toString();
            }
            if (!avatarUrl.isEmpty()) {
                legacy["profile_image_url_https"] = avatarUrl;
                result["legacy"] = legacy;
            }
        }
    }
    return result;
}

QPair<QJsonArray, QString> TwitterCollector::searchTweets(const QString &query, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "search";
    cmd["query"] = query;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("SearchTimeline error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};

    // 401/403/404: 토큰/해시 만료 → 데몬 재시작으로 자동 갱신 (최대 3회)
    if (status == 401 || status == 403 || status == 404) {
        m_backend->log(QString("SearchTimeline HTTP %1 → 자동 갱신 시도 (최대 3회)").arg(status), "warning", "twitter");
        for (int retry = 0; retry < 3; ++retry) {
            if (retry > 0) {
                m_backend->log(QString("재시도 %1/3: 데몬 재시작...").arg(retry + 1), "info", "twitter");
            }
            // 데몬 재시작 → 세션 + 해시 완전 리셋
            stopDaemon();
            QThread::sleep(3);
            if (!startDaemon()) {
                m_backend->log("데몬 재시작 실패", "error", "twitter");
                return {QJsonArray(), QStringLiteral("ERROR")};
            }
            QThread::sleep(2);
            QJsonObject retryResp = sendDaemonCommand(cmd);
            int retryStatus = retryResp["status"].toInt();
            if (!retryResp.contains("error") && retryStatus == 200) {
                m_backend->log("SearchTimeline 복구 성공!", "success", "twitter");
                resp = retryResp;
                status = 200;
                break;
            }
            m_backend->log(QString("재시도 %1 실패 (HTTP %2)").arg(retry + 1).arg(retryStatus), "warning", "twitter");
        }
        if (status != 200) {
            m_backend->log(QString("SearchTimeline HTTP %1 복구 불가").arg(status), "error", "twitter");
            return {QJsonArray(), QStringLiteral("ERROR")};
        }
    }
    if (status != 200) {
        m_backend->log(QString("SearchTimeline HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonObject data = resp["body"].toObject();

    QJsonArray results;
    QString nextCursor;
    QJsonArray instructions = data["data"].toObject()["search_by_raw_query"].toObject()
                                  ["search_timeline"].toObject()["timeline"].toObject()
                                  ["instructions"].toArray();

    for (const auto &inst : instructions) {
        QJsonObject instruction = inst.toObject();
        if (instruction["type"].toString() == "TimelineAddEntries") {
            QJsonArray entries = instruction["entries"].toArray();
            for (const auto &entry : entries) {
                QJsonObject entryObj = entry.toObject();
                QString entryId = entryObj["entryId"].toString();
                if (entryId.startsWith("tweet-")) {
                    QJsonObject tweetResult = entryObj["content"].toObject()
                        ["itemContent"].toObject()["tweet_results"].toObject()["result"].toObject();

                    if (tweetResult.contains("tweet")) {
                        tweetResult = tweetResult["tweet"].toObject();
                    }

                    if (!tweetResult.isEmpty()) {
                        results.append(tweetResult);
                    }
                } else if (entryId.startsWith("cursor-bottom")) {
                    nextCursor = entryObj["content"].toObject()["value"].toString();
                }
            }
        }
    }

    return {results, nextCursor};
}

QPair<QJsonArray, QString> TwitterCollector::getUserTweets(const QString &userId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "user_tweets";
    cmd["user_id"] = userId;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("UserTweets error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};

    // 401/403/404: 토큰/해시 만료 → 데몬 재시작으로 자동 갱신
    if (status == 401 || status == 403 || status == 404) {
        m_backend->log(QString("UserTweets HTTP %1 → 자동 갱신 시도").arg(status), "warning", "twitter");
        for (int retry = 0; retry < 3; ++retry) {
            stopDaemon();
            QThread::sleep(3);
            if (!startDaemon()) {
                m_backend->log("데몬 재시작 실패", "error", "twitter");
                return {QJsonArray(), QStringLiteral("ERROR")};
            }
            QThread::sleep(2);
            QJsonObject retryResp = sendDaemonCommand(cmd);
            int retryStatus = retryResp["status"].toInt();
            if (!retryResp.contains("error") && retryStatus == 200) {
                m_backend->log("UserTweets 복구 성공!", "success", "twitter");
                resp = retryResp;
                status = 200;
                break;
            }
            m_backend->log(QString("재시도 %1 실패 (HTTP %2)").arg(retry + 1).arg(retryStatus), "warning", "twitter");
        }
        if (status != 200) {
            m_backend->log(QString("UserTweets HTTP %1 복구 불가").arg(status), "error", "twitter");
            return {QJsonArray(), QStringLiteral("ERROR")};
        }
    }
    if (status != 200) {
        m_backend->log(QString("UserTweets HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonObject json = resp["body"].toObject();
    QJsonArray results;
    QString nextCursor;

    // Check for API errors
    QJsonArray errors = json["errors"].toArray();
    if (!errors.isEmpty()) {
        for (const auto &e : errors) {
            m_backend->log("API error: "+ e.toObject()["message"].toString(), "error", "twitter");
        }
    }

    // Navigate: data → user → result → timeline_v2 → timeline → instructions
    QJsonObject userData = json["data"].toObject()["user"].toObject()["result"].toObject();
    QJsonArray instructions = userData["timeline_v2"].toObject()["timeline"].toObject()["instructions"].toArray();

    // Fallback path: data → user → result → timeline → timeline → instructions
    if (instructions.isEmpty()) {
        instructions = userData["timeline"].toObject()["timeline"].toObject()["instructions"].toArray();
    }

    if (instructions.isEmpty() && cursor.isEmpty()) {
        QStringList keys;
        for (auto it = userData.begin(); it != userData.end(); ++it) keys << it.key();
        m_backend->log("UserTweets user.result keys: ["+ keys.join(", ") + "]", "warning", "twitter");
    }

    for (const auto &inst : instructions) {
        QJsonObject instruction = inst.toObject();
        QString instType = instruction["type"].toString();
        if (instType == "TimelineAddEntries") {
            QJsonArray entries = instruction["entries"].toArray();
            for (const auto &entry : entries) {
                QJsonObject e = entry.toObject();
                QString eid = e["entryId"].toString();
                if (eid.startsWith("tweet-") || eid.startsWith("profile-conversation-")) {
                    QJsonObject content = e["content"].toObject();
                    QString entryType = content["entryType"].toString();
                    if (entryType == "TimelineTimelineItem"|| content.contains("itemContent")) {
                        QJsonObject tweetResult = content["itemContent"].toObject()
                            ["tweet_results"].toObject()["result"].toObject();
                        if (tweetResult.contains("tweet")) tweetResult = tweetResult["tweet"].toObject();
                        if (!tweetResult.isEmpty()) results.append(tweetResult);
                    } else if (entryType == "TimelineTimelineModule") {
                        QJsonArray items = content["items"].toArray();
                        for (const auto &item : items) {
                            QJsonObject itemContent = item.toObject()["item"].toObject()["itemContent"].toObject();
                            QJsonObject tweetResult = itemContent["tweet_results"].toObject()["result"].toObject();
                            if (tweetResult.contains("tweet")) tweetResult = tweetResult["tweet"].toObject();
                            if (!tweetResult.isEmpty()) results.append(tweetResult);
                        }
                    }
                } else if (eid.startsWith("cursor-bottom")) {
                    nextCursor = e["content"].toObject()["value"].toString();
                }
            }
        }
    }
    return {results, nextCursor};
}

QString TwitterCollector::extractCursorFromEntries(const QJsonArray &entries) const
{
    for (const auto &entry : entries) {
        QJsonObject entryObj = entry.toObject();
        if (entryObj["entryId"].toString().startsWith("cursor-bottom")) {
            return entryObj["content"].toObject()["value"].toString();
        }
    }
    return QString();
}

QPair<QJsonArray, QString> TwitterCollector::getTweetDetail(const QString &tweetId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "tweet_detail";
    cmd["tweet_id"] = tweetId;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd, 30000);
    if (resp.contains("error")) {
        m_backend->log("TweetDetail error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status != 200) {
        m_backend->log(QString("TweetDetail HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonObject json = resp["body"].toObject();
    QJsonArray results;
    QString nextCursor;

    // Navigate: data → threaded_conversation_with_injections_v2 → instructions
    QJsonArray instructions = json["data"].toObject()
        ["threaded_conversation_with_injections_v2"].toObject()
        ["instructions"].toArray();

    for (const auto &inst : instructions) {
        QJsonObject instruction = inst.toObject();
        QString instType = instruction["type"].toString();

        if (instType == "TimelineAddEntries") {
            QJsonArray entries = instruction["entries"].toArray();
            for (const auto &entry : entries) {
                QJsonObject e = entry.toObject();
                QString eid = e["entryId"].toString();

                // conversationthread- entries contain reply items
                if (eid.startsWith("conversationthread-")) {
                    QJsonArray items = e["content"].toObject()["items"].toArray();
                    for (const auto &item : items) {
                        QJsonObject itemContent = item.toObject()["item"].toObject()["itemContent"].toObject();
                        QJsonObject tweetResult = itemContent["tweet_results"].toObject()["result"].toObject();
                        if (tweetResult.contains("tweet")) tweetResult = tweetResult["tweet"].toObject();
                        if (!tweetResult.isEmpty()) results.append(tweetResult);
                    }
                }
                // Individual tweet entries (direct replies)
                else if (eid.startsWith("tweet-")) {
                    QJsonObject content = e["content"].toObject();
                    QJsonObject tweetResult = content["itemContent"].toObject()
                        ["tweet_results"].toObject()["result"].toObject();
                    if (tweetResult.contains("tweet")) tweetResult = tweetResult["tweet"].toObject();
                    // Skip the focal tweet itself
                    QString tid = tweetResult["rest_id"].toString();
                    if (!tweetResult.isEmpty() && tid != tweetId)
                        results.append(tweetResult);
                }
                // Cursor for more replies
                else if (eid.contains("cursor-bottom") || eid.contains("cursor-showmore")) {
                    QString cv = e["content"].toObject()["value"].toString();
                    if (cv.isEmpty())
                        cv = e["content"].toObject()["itemContent"].toObject()["value"].toString();
                    if (!cv.isEmpty()) nextCursor = cv;
                }
            }
        }
        // TimelineAddToModule: additional replies loaded via "Show more replies"
else if (instType == "TimelineAddToModule") {
            QJsonArray items = instruction["moduleItems"].toArray();
            for (const auto &item : items) {
                QJsonObject itemContent = item.toObject()["item"].toObject()["itemContent"].toObject();
                QJsonObject tweetResult = itemContent["tweet_results"].toObject()["result"].toObject();
                if (tweetResult.contains("tweet")) tweetResult = tweetResult["tweet"].toObject();
                if (!tweetResult.isEmpty()) results.append(tweetResult);
            }
        }
    }

    return {results, nextCursor};
}

QPair<QJsonArray, QString> TwitterCollector::getLikes(const QString &userId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "likes";
    cmd["user_id"] = userId;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("Likes error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status == 404) {
        m_backend->log("Likes HTTP 404 → 엔드포인트 갱신 시도", "warning", "twitter");
        QJsonObject updateCmd;
        updateCmd["action"] = "update_endpoints";
        QJsonObject updateResp = sendDaemonCommand(updateCmd, 30000);
        if (updateResp["status"].toString() == "ok") {
            QJsonObject retryResp = sendDaemonCommand(cmd);
            if (!retryResp.contains("error") && retryResp["status"].toInt() == 200) {
                resp = retryResp;
                status = 200;
            }
        }
    }
    if (status != 200) {
        m_backend->log(QString("Likes HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonArray results;
    QString nextCursor;
    QJsonArray instructions = resp["body"].toObject()["data"].toObject()["user"].toObject()["result"].toObject()
        ["timeline_v2"].toObject()["timeline"].toObject()["instructions"].toArray();

    for (const auto &inst : instructions) {
        QJsonObject instruction = inst.toObject();
        if (instruction["type"].toString() == "TimelineAddEntries") {
            QJsonArray entries = instruction["entries"].toArray();
            for (const auto &entry : entries) {
                QJsonObject e = entry.toObject();
                QString eid = e["entryId"].toString();
                if (eid.startsWith("tweet-")) {
                    QJsonObject tweetResult = e["content"].toObject()
                        ["itemContent"].toObject()["tweet_results"].toObject()["result"].toObject();
                    if (tweetResult.contains("tweet")) tweetResult = tweetResult["tweet"].toObject();
                    if (!tweetResult.isEmpty()) results.append(tweetResult);
                } else if (eid.startsWith("cursor-bottom")) {
                    nextCursor = e["content"].toObject()["value"].toString();
                }
            }
        }
    }
    return {results, nextCursor};
}

QPair<QJsonArray, QString> TwitterCollector::getBookmarks(const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "bookmarks";
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("Bookmarks error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status == 404) {
        m_backend->log("Bookmarks HTTP 404 → 엔드포인트 갱신 시도", "warning", "twitter");
        QJsonObject updateCmd;
        updateCmd["action"] = "update_endpoints";
        QJsonObject updateResp = sendDaemonCommand(updateCmd, 30000);
        if (updateResp["status"].toString() == "ok") {
            QJsonObject retryResp = sendDaemonCommand(cmd);
            if (!retryResp.contains("error") && retryResp["status"].toInt() == 200) {
                resp = retryResp;
                status = 200;
            }
        }
    }
    if (status != 200) {
        m_backend->log(QString("Bookmarks HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonArray results;
    QString nextCursor;
    QJsonArray instructions = resp["body"].toObject()["data"].toObject()["bookmark_timeline_v2"].toObject()
        ["timeline"].toObject()["instructions"].toArray();

    for (const auto &inst : instructions) {
        QJsonObject instruction = inst.toObject();
        if (instruction["type"].toString() == "TimelineAddEntries") {
            QJsonArray entries = instruction["entries"].toArray();
            for (const auto &entry : entries) {
                QJsonObject e = entry.toObject();
                QString eid = e["entryId"].toString();
                if (eid.startsWith("tweet-")) {
                    QJsonObject tweetResult = e["content"].toObject()
                        ["itemContent"].toObject()["tweet_results"].toObject()["result"].toObject();
                    if (tweetResult.contains("tweet")) tweetResult = tweetResult["tweet"].toObject();
                    if (!tweetResult.isEmpty()) results.append(tweetResult);
                } else if (eid.startsWith("cursor-bottom")) {
                    nextCursor = e["content"].toObject()["value"].toString();
                }
            }
        }
    }
    return {results, nextCursor};
}

// 공통 헬퍼: 트윗/유저 타임라인에서 tweet-, user-, cursor-bottom 엔트리 파싱
static void _parseTimelineEntries(const QJsonArray &instructions, QJsonArray &results,
                                  QString &nextCursor, bool expectUsers = false)
{
    for (const auto &inst : instructions) {
        QJsonObject instruction = inst.toObject();
        if (instruction["type"].toString() == "TimelineAddEntries") {
            QJsonArray entries = instruction["entries"].toArray();
            for (const auto &entry : entries) {
                QJsonObject e = entry.toObject();
                QString eid = e["entryId"].toString();
                QJsonObject content = e["content"].toObject();
                QString cType = content["entryType"].toString();
                if (cType == "TimelineTimelineCursor") {
                    QString ct = content["cursorType"].toString();
                    if (ct == "Bottom") nextCursor = content["value"].toString();
                    continue;
                }
                if (expectUsers && eid.startsWith("user-")) {
                    QJsonObject userResult = content["itemContent"].toObject()
                        ["user_results"].toObject()["result"].toObject();
                    if (!userResult.isEmpty()) results.append(userResult);
                } else if (!expectUsers && eid.startsWith("tweet-")) {
                    QJsonObject tr = content["itemContent"].toObject()
                        ["tweet_results"].toObject()["result"].toObject();
                    if (tr.contains("tweet")) tr = tr["tweet"].toObject();
                    if (!tr.isEmpty()) results.append(tr);
                } else if (eid.startsWith("cursor-bottom")) {
                    QString cv = content["value"].toString();
                    if (!cv.isEmpty()) nextCursor = cv;
                }
            }
        }
    }
}

QPair<QJsonArray, QString> TwitterCollector::getHighlights(const QString &userId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "highlights";
    cmd["user_id"] = userId;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("Highlights error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }
    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status != 200) {
        m_backend->log(QString("Highlights HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonArray instructions = resp["body"].toObject()["data"].toObject()
        ["user"].toObject()["result"].toObject()
        ["timeline"].toObject()["timeline"].toObject()["instructions"].toArray();
    QJsonArray results;
    QString nextCursor;
    _parseTimelineEntries(instructions, results, nextCursor, false);
    return {results, nextCursor};
}

QPair<QJsonArray, QString> TwitterCollector::getFavoriters(const QString &tweetId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "favoriters";
    cmd["tweet_id"] = tweetId;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("Favoriters error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }
    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status != 200) {
        m_backend->log(QString("Favoriters HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonArray instructions = resp["body"].toObject()["data"].toObject()
        ["favoriters_timeline"].toObject()["timeline"].toObject()["instructions"].toArray();
    QJsonArray results;
    QString nextCursor;
    _parseTimelineEntries(instructions, results, nextCursor, true);
    return {results, nextCursor};
}

QPair<QJsonArray, QString> TwitterCollector::getRetweeters(const QString &tweetId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "retweeters";
    cmd["tweet_id"] = tweetId;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("Retweeters error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }
    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status != 200) {
        m_backend->log(QString("Retweeters HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonArray instructions = resp["body"].toObject()["data"].toObject()
        ["retweeters_timeline"].toObject()["timeline"].toObject()["instructions"].toArray();
    QJsonArray results;
    QString nextCursor;
    _parseTimelineEntries(instructions, results, nextCursor, true);
    return {results, nextCursor};
}

QPair<QJsonArray, QString> TwitterCollector::getListTweets(const QString &listId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "list_tweets";
    cmd["list_id"] = listId;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("ListTweets error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }
    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status != 200) {
        m_backend->log(QString("ListTweets HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonArray instructions = resp["body"].toObject()["data"].toObject()
        ["list"].toObject()["tweets_timeline"].toObject()
        ["timeline"].toObject()["instructions"].toArray();
    QJsonArray results;
    QString nextCursor;
    _parseTimelineEntries(instructions, results, nextCursor, false);
    return {results, nextCursor};
}

QPair<QJsonArray, QString> TwitterCollector::getCommunityTweets(const QString &communityId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "community_tweets";
    cmd["community_id"] = communityId;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("CommunityTweets error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }
    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status != 200) {
        m_backend->log(QString("CommunityTweets HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonArray instructions = resp["body"].toObject()["data"].toObject()
        ["communityResults"].toObject()["result"].toObject()
        ["community_timeline"].toObject()["timeline"].toObject()["instructions"].toArray();
    QJsonArray results;
    QString nextCursor;
    _parseTimelineEntries(instructions, results, nextCursor, false);
    return {results, nextCursor};
}

bool TwitterCollector::downloadMedia(const QString &url, const QString &filePath)
{
    QMap<QString, QString> headers;
    headers["Referer"] = "https://x.com/";
    return m_http->downloadFile(url, filePath, headers);
}

int TwitterCollector::downloadTweetMedia(const QJsonObject &tweet, const QString &mediaDir, int tweetIdx)
{
    // Skip entirely if media download AND exif are both off
    if (!m_downloadMedia && !m_saveExif) return 0;

    int downloaded = 0;
    QJsonObject legacy = tweet["legacy"].toObject();

    // Check if this is a retweet - if so, download media from original tweet
    QJsonObject mediaTweet = tweet;
    QJsonObject mediaLegacy = legacy;
    QJsonObject rtResult;
    if (legacy.contains("retweeted_status_result")) {
        rtResult = legacy["retweeted_status_result"].toObject()["result"].toObject();
    }
    if (rtResult.isEmpty() && tweet.contains("retweeted_status_result")) {
        rtResult = tweet["retweeted_status_result"].toObject()["result"].toObject();
    }
    if (!rtResult.isEmpty()) {
        if (rtResult.contains("tweet")) rtResult = rtResult["tweet"].toObject();
        mediaTweet = rtResult;
        mediaLegacy = rtResult["legacy"].toObject();
    }

    // Also check quoted tweet
    QJsonObject quotedTweet;
    if (tweet.contains("quoted_status_result")) {
        quotedTweet = tweet["quoted_status_result"].toObject()["result"].toObject();
        if (quotedTweet.contains("tweet")) quotedTweet = quotedTweet["tweet"].toObject();
    }

    // Helper lambda to download media from a tweet object
    auto downloadFromTweet = [&](const QJsonObject &tw, const QJsonObject &twLegacy) -> int {
        int count = 0;
        QJsonObject entities = twLegacy["extended_entities"].toObject();
        QJsonArray media = entities["media"].toArray();

        if (media.isEmpty()) {
            entities = twLegacy["entities"].toObject();
            media = entities["media"].toArray();
        }

        // Author username subfolder
        QJsonObject userResult = tw["core"].toObject()["user_results"].toObject()["result"].toObject();
        QString authorUsername = userResult["legacy"].toObject()["screen_name"].toString();
        if (authorUsername.isEmpty()) authorUsername = userResult["screen_name"].toString();
        QString authorMediaDir = mediaDir;
        if (!authorUsername.isEmpty()) {
            authorMediaDir = mediaDir + "/"+ authorUsername;
            QDir().mkpath(authorMediaDir);
        }

        for (int i = 0; i < media.size(); ++i) {
            QJsonObject mediaObj = media[i].toObject();
            QString mediaType = mediaObj["type"].toString();
            QString mediaUrl;
            QString ext;

            if (mediaType == "photo") {
                mediaUrl = mediaObj["media_url_https"].toString();
                if (!mediaUrl.isEmpty()) {
                    // 쿼리 파라미터 제거 후 확장자 판별
                    QString baseUrl = mediaUrl.split('?').first();
                    if (baseUrl.toLower().endsWith(".png")) {
                        ext = ".png";
                    } else {
                        ext = ".jpg";
                    }
                    // ?name=orig만 사용 — format 파라미터 없이 원본 그대로 다운 (품질 열화 방지)
                    mediaUrl = baseUrl + "?name=orig";
                }
            } else if (mediaType == "video"|| mediaType == "animated_gif") {
                QJsonArray variants = mediaObj["video_info"].toObject()["variants"].toArray();
                int bestBitrate = -1;
                for (const auto &v : variants) {
                    QJsonObject variant = v.toObject();
                    if (variant["content_type"].toString() == "video/mp4") {
                        int bitrate = variant["bitrate"].toInt();
                        if (bitrate > bestBitrate) {
                            bestBitrate = bitrate;
                            mediaUrl = variant["url"].toString();
                        }
                    }
                }
                ext = ".mp4";
            }

            if (mediaUrl.isEmpty()) continue;

            QString tweetId = twLegacy["id_str"].toString();
            if (tweetId.isEmpty()) tweetId = tw["rest_id"].toString();

            // 미디어 URL에서 media key 추출 (pbs.twimg.com/media/Gt2UTcbWsAAedIz.jpg → Gt2UTcbWsAAedIz)
            QString mediaKey;
            {
                QString urlPath = QUrl(mediaUrl.split('?').first()).path();  // /media/Gt2UTcbWsAAedIz.jpg
                QString baseName = urlPath.mid(urlPath.lastIndexOf('/') + 1);
                int dotPos = baseName.lastIndexOf('.');
                if (dotPos > 0) baseName = baseName.left(dotPos);
                if (!baseName.isEmpty() && baseName != tweetId)
                    mediaKey = baseName;
            }

            // 파일명: {clean_text}-{tweet_id} ({media_key}).ext
            QString tweetText = twLegacy["full_text"].toString();
            if (tweetText.isEmpty()) tweetText = twLegacy["text"].toString();
            // sanitize: URL 제거, 제어문자 제거, 파일시스템 금지문자 제거
            tweetText.remove(QRegularExpression("https?://\\S+"));
            tweetText.remove(QRegularExpression("[\\x00-\\x1f\\x7f-\\x9f]"));
            tweetText.replace(QRegularExpression("[\\n\\r]"), "");
            tweetText.remove(QRegularExpression("[<>:\"/\\\\|?*]"));
            tweetText.replace(QRegularExpression("\\s+"), "");
            tweetText = tweetText.trimmed();
            if (tweetText.startsWith('.')) tweetText = tweetText.mid(1).trimmed();
            if (tweetText.length() > 100) tweetText = tweetText.left(100).trimmed();

            // Set file time to tweet date (always, even if already downloaded)
            QString createdAt = twLegacy["created_at"].toString();
            if (createdAt.isEmpty()) {
                // Fallback: try parent tweet's created_at
                createdAt = tw["legacy"].toObject()["created_at"].toString();
            }

            // 업로드 시각 prefix (OS 정렬 시 업로드 순 배치)
            QString orderPrefix;
            if (!createdAt.isEmpty()) {
                // Twitter format: "Mon Mar 25 14:58:00 +0000 2024"
QDateTime twDt = QDateTime::fromString(createdAt, "ddd MMM dd HH:mm:ss +0000 yyyy");
                if (twDt.isValid()) {
                    twDt.setTimeSpec(Qt::UTC);
                    orderPrefix = twDt.toUTC().addSecs(9 * 3600).toString("yyyyMMdd_HHmm_");
                }
            }

            // 파일명 형식: {prefix}{text}-{tweetId} ({mediaKey}).ext
            QString mediaKeySuffix = mediaKey.isEmpty() ? "": QString("(%1)").arg(mediaKey);
            QString filename;
            bool multiMedia = (media.size() > 1);
            if (tweetText.isEmpty()) {
                filename = QString("%1%2-%3%4%5").arg(orderPrefix).arg(tweetId).arg(i).arg(mediaKeySuffix, ext);
            } else if (multiMedia || i > 0) {
                // 복수 미디어: {prefix}{text}-{id}-{idx} ({mediaKey}).ext
                filename = QString("%1%2-%3-%4%5%6").arg(orderPrefix, tweetText, tweetId).arg(i).arg(mediaKeySuffix, ext);
            } else {
                // 단일 미디어: {prefix}{text}-{id} ({mediaKey}).ext
                filename = QString("%1%2-%3%4%5").arg(orderPrefix, tweetText, tweetId, mediaKeySuffix, ext);
            }
            QString filepath = authorMediaDir + "/"+ filename;

            if (QFile::exists(filepath)) {
                // Already downloaded — still fix timestamp + EXIF if needed
                if (mediaType == "photo"&& m_saveExif) {
                    addExifMetadata(filepath, tw);
                }
                // setFileTimes를 EXIF 후에 (exiftool이 mtime 변경하므로)
                if (!createdAt.isEmpty()) {
                    Common::setFileTimes(filepath, createdAt);
                }
                count++; continue;
            }

            // Skip download if media download is disabled
            if (!m_downloadMedia) continue;

            if (downloadMedia(mediaUrl, filepath)) {
                count++;

                // Add EXIF metadata (for photos) — configurable
                // EXIF를 먼저! (exiftool이 파일 수정 → mtime 변경되므로)
                if (mediaType == "photo"&& m_saveExif) {
                    addExifMetadata(filepath, tw);
                }

                // Add Finder comment with tweet URL
                QJsonObject userResult = tw["core"].toObject()["user_results"].toObject()["result"].toObject();
                QString screenName = userResult["legacy"].toObject()["screen_name"].toString();
                if (screenName.isEmpty()) screenName = userResult["screen_name"].toString();
                if (screenName.isEmpty()) screenName = userResult["rest_id"].toString();
                QString tweetUrl = QString("https://x.com/%1/status/%2").arg(
                    screenName.isEmpty() ? "i": screenName, tweetId);
                FileHelper::setFinderComment(filepath, tweetUrl);
                // xattr URL 메타데이터 (macOS kMDItemWhereFroms)
                FileHelper::setDownloadMeta(filepath, tweetUrl);

                // 파일 시간 설정 — 맨 마지막에! (EXIF/Finder 수정 후)
                if (!createdAt.isEmpty()) {
                    Common::setFileTimes(filepath, createdAt);
                } else {
                    qWarning() << "[Twitter] created_at empty for tweet:"<< tweetId;
                }

                // _complete 미러 폴더에 복사
                QDir mediaParent(mediaDir);
                mediaParent.cdUp(); // type 폴더의 상위 → media/ 루트
                QString completeDir = mediaParent.absolutePath() + "/_complete";
                QDir().mkpath(completeDir);
                QString completePath = completeDir + "/"+ QFileInfo(filepath).fileName();
                if (!QFile::exists(completePath)) {
                    QFile::copy(filepath, completePath);
                    if (!createdAt.isEmpty()) Common::setFileTimes(completePath, createdAt);
                }
            }
        }
        return count;
    };

    // Download media from main/retweeted tweet
    downloaded += downloadFromTweet(mediaTweet, mediaLegacy);

    // Also download media from quoted tweet if present
    if (!quotedTweet.isEmpty()) {
        downloaded += downloadFromTweet(quotedTweet, quotedTweet["legacy"].toObject());
    }

    return downloaded;
}

void TwitterCollector::downloadUserProfileMedia(const QJsonObject &tweet, const QString &profileDir, const QString &category)
{
    // Extract all unique users from this tweet (author, RT'd user, quoted user)
    auto extractUser = [](const QJsonObject &tw) -> QJsonObject {
        return tw["core"].toObject()["user_results"].toObject()["result"].toObject();
    };

    QList<QJsonObject> users;
    users.append(extractUser(tweet));

    // RT'd user
    QJsonObject legacy = tweet["legacy"].toObject();
    QJsonObject rtResult;
    if (legacy.contains("retweeted_status_result"))
        rtResult = legacy["retweeted_status_result"].toObject()["result"].toObject();
    if (rtResult.isEmpty() && tweet.contains("retweeted_status_result"))
        rtResult = tweet["retweeted_status_result"].toObject()["result"].toObject();
    if (!rtResult.isEmpty()) {
        if (rtResult.contains("tweet")) rtResult = rtResult["tweet"].toObject();
        users.append(extractUser(rtResult));
    }

    // Quoted user
    QJsonObject quotedTweet;
    if (tweet.contains("quoted_status_result"))
        quotedTweet = tweet["quoted_status_result"].toObject()["result"].toObject();
    if (quotedTweet.contains("tweet")) quotedTweet = quotedTweet["tweet"].toObject();
    if (!quotedTweet.isEmpty())
        users.append(extractUser(quotedTweet));

    // 카테고리 디렉토리 (유저별 서브폴더는 아래 루프에서 생성)
    QString categoryDir = profileDir + "/"+ category;

    for (const auto &userObj : users) {
        if (userObj.isEmpty()) continue;

        // ── Twitter API 2025+ 구조: screen_name, name, profile_image_url_https가 legacy에서 제거됨 ──
        // 새 구조: userObj = { rest_id, avatar, core: {screen_name, name, ...}, legacy: {stats만...} }
        QJsonObject uLeg = userObj["legacy"].toObject();
        QJsonObject uCore = userObj["core"].toObject();

        // screen_name 추출 (여러 위치 시도)
        QString screenName = uLeg["screen_name"].toString();
        if (screenName.isEmpty()) screenName = uCore["screen_name"].toString();
        if (screenName.isEmpty()) screenName = userObj["screen_name"].toString();
        // core.user_results.result.legacy (중첩 구조)
        if (screenName.isEmpty()) {
            QJsonObject innerResult = uCore["user_results"].toObject()["result"].toObject();
            screenName = innerResult["legacy"].toObject()["screen_name"].toString();
            if (screenName.isEmpty()) screenName = innerResult["screen_name"].toString();
        }
        // 최후 수단: rest_id
        if (screenName.isEmpty()) screenName = userObj["rest_id"].toString();

        if (screenName.isEmpty()) continue;  // 완전히 비어있으면 스킵

        // name(표시이름) 추출
        QString displayName = uLeg["name"].toString();
        if (displayName.isEmpty()) displayName = uCore["name"].toString();
        if (displayName.isEmpty()) displayName = userObj["name"].toString();

        // profile_image_url 추출
        QString profileImageUrl = uLeg["profile_image_url_https"].toString();
        if (profileImageUrl.isEmpty()) profileImageUrl = uCore["profile_image_url_https"].toString();
        if (profileImageUrl.isEmpty()) profileImageUrl = userObj["profile_image_url_https"].toString();
        // avatar 필드 (새 API 구조)
        if (profileImageUrl.isEmpty()) {
            QJsonValue avatarVal = userObj["avatar"];
            if (avatarVal.isObject()) {
                QJsonObject av = avatarVal.toObject();
                profileImageUrl = av["image_url"].toString();
                if (profileImageUrl.isEmpty()) profileImageUrl = av["url"].toString();
            } else if (avatarVal.isString()) {
                profileImageUrl = avatarVal.toString();
            }
        }
        if (m_downloadedProfiles.contains(screenName)) continue;
        m_downloadedProfiles.insert(screenName);

        // 파일명에 표시이름 포함: "이름(@handle)_profile.jpg"// (displayName은 이미 위에서 추출됨)
        displayName.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
        displayName = displayName.trimmed().left(40);
        QString filePrefix;
        if (!displayName.isEmpty()) {
            filePrefix = QString("%1(@%2)").arg(displayName, screenName);
        } else {
            filePrefix = screenName;
        }

        // 유저별 서브폴더: profiles/tweets/@handle/ or profiles/tweets/이름(@handle)/
        QString userDirName = filePrefix;
        userDirName.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
        QString userProfileDir = categoryDir + "/"+ userDirName;
        QDir().mkpath(userProfileDir);

        // 날짜 접미사 (덮어쓰기 방지 — 프로필/배너 변경 이력 보존)
        QString dateTag = QDateTime::currentDateTime().toString("yyyyMMdd");

        // Profile image (400x400) — profileImageUrl은 이미 위에서 추출됨
        QString profileImgUrl = profileImageUrl;
        if (!profileImgUrl.isEmpty()) profileImgUrl = profileImgUrl.replace("_normal", "_400x400");
        if (!profileImgUrl.isEmpty()) {
            // 날짜별 파일: profile_20260323.jpg (같은 날짜면 건너뜀)
            QString datePath = userProfileDir + "/profile_"+ dateTag + ".jpg";
            if (!QFile::exists(datePath)) {
                m_backend->log(QString("프로필: @%1 → %2").arg(screenName, category), "info", "twitter");
                downloadMedia(profileImgUrl, datePath);
            }
        } else {
            m_backend->log(QString("@%1 프로필 이미지 URL 없음").arg(screenName), "warning", "twitter");
        }

        // Banner (1500x500) — 새 API 대응
        QString bannerUrl = uLeg["profile_banner_url"].toString();
        if (bannerUrl.isEmpty()) bannerUrl = uCore["profile_banner_url"].toString();
        if (bannerUrl.isEmpty()) bannerUrl = userObj["profile_banner_url"].toString();
        if (!bannerUrl.isEmpty()) {
            QString datePath = userProfileDir + "/banner_"+ dateTag + ".jpg";
            if (!QFile::exists(datePath)) {
                m_backend->log(QString("배너: @%1 → %2").arg(screenName, category), "info", "twitter");
                downloadMedia(bannerUrl + "/1500x500", datePath);
            }
        }

        // Collect profile data for Excel export — 새 API 대응
        QJsonObject profileInfo;
        profileInfo["screen_name"] = screenName;
        profileInfo["name"] = displayName;
        QString desc = uLeg["description"].toString();
        if (desc.isEmpty()) desc = uCore["description"].toString();
        profileInfo["description"] = desc;
        profileInfo["followers_count"] = uLeg["followers_count"].toInt();
        profileInfo["friends_count"] = uLeg["friends_count"].toInt();
        profileInfo["statuses_count"] = uLeg["statuses_count"].toInt();
        profileInfo["location"] = uLeg["location"].toString();
        profileInfo["url"] = uLeg["url"].toString();
        profileInfo["created_at"] = Common::formatDateJapanese(uLeg["created_at"].toString());
        profileInfo["verified"] = userObj["is_blue_verified"].toBool() ? "True": "False";
        profileInfo["profile_image_url"] = profileImageUrl.isEmpty() ? QString() : QString(profileImageUrl).replace("_normal", "_400x400");
        profileInfo["profile_banner_url"] = bannerUrl;
        if (m_profileBuffer) m_profileBuffer->append(profileInfo);
    }
}

void TwitterCollector::addExifMetadata(const QString &imagePath, const QJsonObject &tweet)
{
    QJsonObject legacy = tweet["legacy"].toObject();
    QJsonObject core = tweet["core"].toObject()["user_results"].toObject()["result"].toObject()["legacy"].toObject();
    QString screenName = core["screen_name"].toString();
    QString text = legacy["full_text"].toString().left(200);
    QString tweetId = legacy["id_str"].toString();
    if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();
    QString tweetUrl = QString("https://x.com/%1/status/%2").arg(screenName, tweetId);
    QString createdAt = legacy["created_at"].toString();

    // Common::addExifMetadata가 번들 exiftool 경로를 올바르게 탐색
    Common::addExifMetadata(imagePath,
        screenName.isEmpty() ? "": "@"+ screenName,
        text,
        screenName.isEmpty() ? "": "Twitter @"+ screenName,
        tweetUrl,
        createdAt);
}

void TwitterCollector::setFinderComment(const QString &filePath, const QString &comment)
{
    FileHelper::setFinderComment(filePath, comment);
}

void TwitterCollector::saveExcel(const QString &saveDir, const QString &target, const QJsonArray &data, const QString &suffix)
{
    ExcelWriter writer;

    QStringList headers = {"ID", "Text", "URL", "Date", "Author",
                           "RT", "Likes", "Replies", "Quotes", "Views",
                           "Lang", "Source", "Reply To", "Media", "Sensitive"};
    writer.writeHeader(headers, QColor("#d97757"));

    int row = 2;
    for (const auto &val : data) {
        QJsonObject tweet = val.toObject();
        QString viewCount = tweet["view_count"].toString();

        writer.writeRow(row++, {
            tweet["id"].toString(),
            tweet["text"].toString().left(500),
            tweet["url"].toString(),
            Common::formatDateJapanese(tweet["created_at"].toString()),
            tweet["author"].toString(),
            QString::number(tweet["retweet_count"].toInt()),
            QString::number(tweet["favorite_count"].toInt()),
            QString::number(tweet["reply_count"].toInt()),
            QString::number(tweet["quote_count"].toInt()),
            viewCount.isEmpty() ? "": viewCount,
            tweet["lang"].toString(),
            tweet["source"].toString(),
            tweet["in_reply_to"].toString(),
            tweet["media_types"].toString(),
            tweet["sensitive"].toString()
        });
    }

    writer.setColumnWidth(1, 20);
    writer.setColumnWidth(2, 50);
    writer.setColumnWidth(3, 40);
    writer.setColumnWidth(4, 28);
    writer.setColumnWidth(5, 15);
    writer.setColumnWidth(6, 8);
    writer.setColumnWidth(7, 8);
    writer.setColumnWidth(8, 8);
    writer.setColumnWidth(9, 8);
    writer.setColumnWidth(10, 12);
    writer.setColumnWidth(11, 6);
    writer.setColumnWidth(12, 20);
    writer.setColumnWidth(13, 15);
    writer.setColumnWidth(14, 15);
    writer.setColumnWidth(15, 8);

    QString filepath = saveDir + "/"+ target + "_"+ suffix + ".xlsx";
    writer.save(filepath);
    FileHelper::setDownloadMeta(filepath, "ABIWA Twitter");
    m_backend->log("저장: "+ filepath, "success", "twitter");
}

void TwitterCollector::saveExcelStreaming(const QString &saveDir, const QString &target, DiskJsonBuffer &buffer, const QString &suffix)
{
    // 아니포 호환 파일명: _complete.xlsx (tweets일 때)
    QString fileSuffix = suffix;
    if (fileSuffix == "tweets") fileSuffix = "complete";

    QString filepath = saveDir + "/"+ target + "_"+ fileSuffix + ".xlsx";

    // ── Step 1: 기존 Excel에서 ID만 읽기 (행 데이터는 메모리에 올리지 않음) ──
    QSet<QString> existingIds;
    int existingLastRow = 1;  // 1 = 헤더만 있음

    if (QFile::exists(filepath)) {
        QXlsx::Document existingDoc(filepath);
        existingLastRow = existingDoc.dimension().lastRow();
        // ID만 읽기 (1열) — 행 데이터 전체를 메모리에 올리지 않음
        for (int r = 2; r <= existingLastRow; ++r) {
            QString id = existingDoc.read(r, 1).toString().trimmed();
            if (!id.isEmpty() && !id.startsWith("─") && !id.startsWith("===")) {
                existingIds.insert(id);
            }
        }
        if (!existingIds.isEmpty()) {
            m_backend->log(QString("기존 파일: %1개 ID 확인").arg(existingIds.size()), "info", "twitter");
        }
    }

    // ── Step 2: 버퍼에서 새 데이터만 추출 (중복 제거) ──
    QList<QStringList> newRows;
    buffer.resetReader();
    QJsonObject tweet;
    while (buffer.readNext(tweet)) {
        QString id = tweet["id"].toString();
        if (existingIds.contains(id)) continue;
        existingIds.insert(id);

        newRows.append({
            id,
            tweet["tweet_url"].toString(),
            tweet["text"].toString(),
            tweet["language"].toString(),
            tweet["type"].toString(),
            tweet["author_name"].toString(),
            tweet["author_username"].toString(),
            QString::number(tweet["bookmark_count"].toInt()),
            QString::number(tweet["favorite_count"].toInt()),
            QString::number(tweet["retweet_count"].toInt()),
            tweet["retweeted"].toString(),
            QString::number(tweet["reply_count"].toInt()),
            QString::number(tweet["quote_count"].toInt()),
            tweet["view_count"].toString(),
            tweet["created_at"].toString(),
            tweet["retweet_time"].toString(),
            tweet["source"].toString(),
            tweet["hashtags"].toString(),
            tweet["urls"].toString(),
            tweet["media_type"].toString(),
            tweet["media_urls"].toString(),
            tweet["conversation_id"].toString(),
            tweet["in_reply_to"].toString(),
            tweet["is_quote"].toString(),
            tweet["quoted_tweet_url"].toString(),
            tweet["possibly_sensitive"].toString(),
            tweet["user_followers"].toString(),
            tweet["user_following"].toString(),
            tweet["user_verified"].toString(),
            tweet["user_profile_image"].toString()
        });
    }

    // 버퍼 비움 → 디스크 + 메모리 해제 (다음 저장 시 새 데이터만 처리)
    buffer.clear();

    if (newRows.isEmpty()) {
        m_backend->log("새 데이터 없음, 저장 건너뜀", "info", "twitter");
        return;
    }

    // ── Step 3: 기존 Excel에 append (전체 재작성 X) ──
    QStringList headers = {"id", "tweet_url", "text", "language", "type",
                           "author_name", "author_username",
                           "bookmark_count", "favorite_count", "retweet_count",
                           "retweeted", "reply_count", "quote_count", "view_count",
                           "created_at", "retweet_time", "source",
                           "hashtags", "urls", "media_type", "media_urls",
                           "conversation_id", "in_reply_to", "is_quote", "quoted_tweet_url",
                           "possibly_sensitive", "user_followers", "user_following",
                           "user_verified", "user_profile_image"};

    // ── Step 3: 기존 데이터 읽기 + 새 파일로 재작성 (QXlsx append는 openpyxl 파일 깨뜨림) ──
    QList<QStringList> existingRows;

    if (QFile::exists(filepath) && existingLastRow > 1) {
        QXlsx::Document existingDoc(filepath);
        int lastCol = existingDoc.dimension().lastColumn();
        if (lastCol < 21) lastCol = 21;

        for (int r = 2; r <= existingLastRow; ++r) {
            QStringList rowData;
            for (int c = 1; c <= lastCol; ++c) {
                rowData << existingDoc.read(r, c).toString();
            }
            // 30컬럼으로 패딩 (아니포 21컬럼 호환)
            while (rowData.size() < 30) rowData << "";
            // 구분선/빈 행 건너뜀
            if (rowData[0].startsWith("─") || rowData[0].startsWith("===")) continue;
            if (rowData[0].trimmed().isEmpty()) continue;
            existingRows.append(rowData);
        }
    }

    // ── Step 4: 새/기존 데이터 병합 후 created_at 기준 최신순 정렬 ──
    //  (id 기준 중복 제거 — 동일 트윗이면 새 데이터 우선)
    QSet<QString> seenIds;
    QList<QStringList> mergedRows;
    auto pushUnique = [&](const QList<QStringList> &src) {
        for (const auto &r : src) {
            QString id = r.size() > 0 ? r[0] : "";
            if (id.isEmpty() || seenIds.contains(id)) continue;
            seenIds.insert(id);
            mergedRows.append(r);
        }
    };
    pushUnique(newRows);       // 새 데이터 우선
    pushUnique(existingRows);  // 기존 데이터는 중복 아닌 것만

    std::sort(mergedRows.begin(), mergedRows.end(), [](const QStringList &a, const QStringList &b) {
        // created_at 은 인덱스 14 (15번째 컬럼)
        QString dateA = a.size() > 14 ? a[14] : "";
        QString dateB = b.size() > 14 ? b[14] : "";
        return dateA > dateB;  // 최신순 (내림차순)
    });

    // ── Step 5: 새 Excel 파일 생성 (항상 깨끗한 파일) ──
    ExcelWriter writer;
    writer.writeHeader(headers, QColor("#0070C0"));

    int row = 2;
    for (const auto &rowData : mergedRows) {
        writer.writeRow(row++, rowData);
    }

    writer.setColumnWidth(1, 20);   // id
    writer.setColumnWidth(2, 40);   // tweet_url
    writer.setColumnWidth(3, 50);   // text
    writer.setColumnWidth(4, 8);    // language
    writer.setColumnWidth(5, 10);   // type
    writer.setColumnWidth(6, 15);   // author_name
    writer.setColumnWidth(7, 15);   // author_username
    writer.setColumnWidth(8, 8);    // bookmark_count
    writer.setColumnWidth(9, 8);    // favorite_count
    writer.setColumnWidth(10, 8);   // retweet_count
    writer.setColumnWidth(11, 8);   // retweeted
    writer.setColumnWidth(12, 8);   // reply_count
    writer.setColumnWidth(13, 8);   // quote_count
    writer.setColumnWidth(14, 10);  // view_count
    writer.setColumnWidth(15, 18);  // created_at
    writer.setColumnWidth(16, 18);  // retweet_time
    writer.setColumnWidth(17, 20);  // source
    writer.setColumnWidth(18, 20);  // hashtags
    writer.setColumnWidth(19, 30);  // urls
    writer.setColumnWidth(20, 10);  // media_type
    writer.setColumnWidth(21, 40);  // media_urls
    writer.setColumnWidth(22, 20);  // conversation_id
    writer.setColumnWidth(23, 25);  // in_reply_to
    writer.setColumnWidth(24, 8);   // is_quote
    writer.setColumnWidth(25, 40);  // quoted_tweet_url
    writer.setColumnWidth(26, 8);   // possibly_sensitive
    writer.setColumnWidth(27, 10);  // user_followers
    writer.setColumnWidth(28, 10);  // user_following
    writer.setColumnWidth(29, 8);   // user_verified
    writer.setColumnWidth(30, 40);  // user_profile_image

    writer.save(filepath);

    FileHelper::setDownloadMeta(filepath, "(\"https://x.com\", \"ABIWA\")");

    // 기존 행 메모리 즉시 해제
    existingRows.clear();
    existingRows.squeeze();

    int totalCount = existingIds.size();
    m_backend->log(QString("저장: %1 (새 %2개, 총 %3개)")
                       .arg(filepath).arg(newRows.size()).arg(totalCount),
                   "success", "twitter");
}

void TwitterCollector::saveProfileExcel(const QString &saveDir, const QString &target)
{
    if (!m_profileBuffer || m_profileBuffer->count() == 0) return;

    QString dateStr = QDateTime::currentDateTime().toString("yyyyMMdd");

    // ── 날짜별 프로필 Excel: _profile_20260323.xlsx (덮어쓰기 방지) ──
    ExcelWriter writer;
    QStringList headers = {"screen_name", "name", "description", "followers_count",
                           "friends_count", "statuses_count", "location", "url",
                           "created_at", "verified", "profile_image_url", "profile_banner_url"};
    writer.writeHeader(headers, QColor("#57a0d9"));

    int row = 2;
    m_profileBuffer->resetReader();
    QJsonObject p;
    while (m_profileBuffer->readNext(p)) {
        writer.writeRow(row++, {
            p["screen_name"].toString(),
            p["name"].toString(),
            p["description"].toString(),
            QString::number(p["followers_count"].toInt()),
            QString::number(p["friends_count"].toInt()),
            QString::number(p["statuses_count"].toInt()),
            p["location"].toString(),
            p["url"].toString(),
            p["created_at"].toString(),
            p["verified"].toString(),
            p["profile_image_url"].toString(),
            p["profile_banner_url"].toString()
        });
    }

    writer.setColumnWidth(1, 15);   // screen_name
    writer.setColumnWidth(2, 20);   // name
    writer.setColumnWidth(3, 40);   // description
    writer.setColumnWidth(4, 10);   // followers
    writer.setColumnWidth(5, 10);   // friends
    writer.setColumnWidth(6, 10);   // statuses
    writer.setColumnWidth(7, 15);   // location
    writer.setColumnWidth(8, 30);   // url
    writer.setColumnWidth(9, 25);   // created_at
    writer.setColumnWidth(10, 8);   // verified
    writer.setColumnWidth(11, 40);  // profile_image
    writer.setColumnWidth(12, 40);  // banner

    // 날짜 폴더: profiles/20260323/
    QString profileDateDir = saveDir + "/profiles/"+ dateStr;
    QDir().mkpath(profileDateDir);

    // 날짜 폴더 안에 Excel 저장
    QString filepath = profileDateDir + "/"+ target + "_profile.xlsx";
    writer.save(filepath);

    FileHelper::setDownloadMeta(filepath, "(\"https://x.com\", \"ABIWA\")");

    m_backend->log(QString("프로필 Excel 저장: %1명 → %2").arg(m_profileBuffer->count()).arg(filepath), "success", "twitter");

    // ── 프로필 설명 텍스트도 날짜 폴더에 저장 ──
    m_profileBuffer->resetReader();
    while (m_profileBuffer->readNext(p)) {
        QString sn = p["screen_name"].toString();
        if (sn.isEmpty()) continue;

        QString descFile = profileDateDir + "/"+ sn + "_description.txt";
        // 같은 날짜에 이미 저장했으면 건너뜀
        if (QFile::exists(descFile)) continue;

        QFile f(descFile);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << "━━━ @"<< sn << "━━━\n";
            ts << "날짜: "<< QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n";
            ts << "이름: "<< p["name"].toString() << "\n";
            ts << "설명: "<< p["description"].toString() << "\n";
            ts << "팔로워: "<< p["followers_count"].toInt() << "\n";
            ts << "팔로잉: "<< p["friends_count"].toInt() << "\n";
            ts << "트윗 수: "<< p["statuses_count"].toInt() << "\n";
            ts << "위치: "<< p["location"].toString() << "\n";
            ts << "URL: "<< p["url"].toString() << "\n";
            ts << "인증: "<< p["verified"].toString() << "\n";
            f.close();
        }
    }
}

void TwitterCollector::handleRateLimit(const QJsonArray &accounts, int &currentIdx, bool &isRunning)
{
    // ── 계정별 Rate Limit 추적 + 무한 순환 ──
    m_consecutiveOk = 0;
    m_rateLimitHits++;
    qint64 now = QDateTime::currentSecsSinceEpoch();
    m_accountRateLimitTime[currentIdx] = now;

    m_backend->log(QString("Rate Limit 발생 (連続 %1回) — 계정 %2")
        .arg(m_rateLimitHits)
        .arg(accounts[currentIdx].toObject()["name"].toString("Account")), "warning", "twitter");

    auto switchToAccount = [&](int idx) {
        stopDaemon();
        int prevIdx = currentIdx;
        currentIdx = idx;
        QJsonObject account = accounts[idx].toObject();
        setupClient(account["auth_token"].toString(), account["ct0"].toString());
        m_tidInitialized = false;
        m_backend->log(QString("계정 전환: %1 → %2 (%3/%4)")
            .arg(accounts[prevIdx].toObject()["name"].toString("Account"))
            .arg(account["name"].toString("Account"))
            .arg(idx + 1).arg(accounts.size()), "info", "twitter");
        if (!startDaemon()) {
            m_backend->log("데몬 시작 실패, TID fallback", "warning", "twitter");
            initTransactionIds();
        }
    };

    if (accounts.size() > 1) {
        // ── Rate Limit 안 걸린 계정 찾기 (15분 이내 RL 기록 없는 계정) ──
        int freshIdx = -1;
        for (int i = 1; i < accounts.size(); ++i) {
            int candidate = (currentIdx + i) % accounts.size();
            qint64 lastRL = m_accountRateLimitTime.value(candidate, 0);
            if (now - lastRL > 900) { // 15분 이상 경과 → 사용 가능
                freshIdx = candidate;
                break;
            }
        }

        if (freshIdx >= 0) {
            // 사용 가능한 계정 있음 → 전환 + 짧은 대기
            switchToAccount(freshIdx);
            int waitSec = 30; // 계정 전환 쿨다운
            m_backend->log(QString("계정 전환 대기 %1초...").arg(waitSec), "info", "twitter");
            for (int r = waitSec; r > 0 && isRunning; --r) {
                m_backend->updateStats(0, 0, QString("전환 대기 %1s").arg(r), "twitter");
                QThread::sleep(1);
            }
        } else {
            // 모든 계정 RL → 가장 오래된 RL 계정의 쿨다운까지 대기
            qint64 oldestRL = now;
            int oldestIdx = 0;
            for (int i = 0; i < accounts.size(); ++i) {
                qint64 t = m_accountRateLimitTime.value(i, 0);
                if (t < oldestRL) { oldestRL = t; oldestIdx = i; }
            }
            int waitSec = qMax(0, static_cast<int>(900 - (now - oldestRL)));
            if (waitSec < 60) waitSec = 60; // 최소 1분

            m_backend->log(QString("모든 계정 Rate Limit → %1분 대기 후 재시도")
                .arg((waitSec + 59) / 60), "warning", "twitter");

            for (int remaining = waitSec; remaining > 0 && isRunning; --remaining) {
                int mins = remaining / 60;
                int secs = remaining % 60;
                m_backend->updateStats(0, 0, QString("RL待機 %1:%2").arg(mins).arg(secs, 2, 10, QChar('0')), "twitter");
                QThread::sleep(1);
            }

            // 가장 오래된 RL 계정으로 전환 (가장 먼저 풀릴 계정)
            m_accountRateLimitTime.clear(); // 리셋
            switchToAccount(oldestIdx);
            m_rateLimitHits = 0;
            m_backend->log(QString("대기 완료! 재시작: %1 (%2/%3)")
                .arg(accounts[oldestIdx].toObject()["name"].toString("Account"))
                .arg(oldestIdx + 1).arg(accounts.size()), "success", "twitter");
            QThread::sleep(2);
        }
    } else {
        // 계정 1개 → 15분 대기 후 재시도 (무한 반복)
        m_backend->log(QString("15분 대기 후 재시도... (RL %1회)").arg(m_rateLimitHits), "warning", "twitter");

        for (int remaining = 900; remaining > 0 && isRunning; --remaining) {
            int mins = remaining / 60;
            int secs = remaining % 60;
            m_backend->updateStats(0, 0, QString("RL待機 %1:%2").arg(mins).arg(secs, 2, 10, QChar('0')), "twitter");
            QThread::sleep(1);
        }

        // ★ 사용자가 대기 중에 중지 버튼을 눌렀으면 데몬 재시작하지 말고 즉시 빠져나감
        if (!isRunning) {
            m_backend->log("대기 중 중지 요청 감지 — 데몬 재시작 생략", "info", "twitter");
            return;
        }

        stopDaemon();
        // ★ 데몬 재시작 + 리트라이 (5분 sleep 직후엔 일시적 네트워크 미복구 가능 — sleep/wake 시나리오)
        QJsonObject account = accounts[currentIdx].toObject();
        bool restarted = false;
        for (int attempt = 1; attempt <= 3 && isRunning && !restarted; ++attempt) {
            setupClient(account["auth_token"].toString(), account["ct0"].toString());
            m_tidInitialized = false;
            if (startDaemon()) {
                restarted = true;
                break;
            }
            m_backend->log(QString("데몬 재시작 실패 (%1/3) — %2초 후 재시도")
                .arg(attempt).arg(attempt * 5), "warning", "twitter");
            for (int s = attempt * 5; s > 0 && isRunning; --s) QThread::sleep(1);
        }
        if (!restarted) {
            m_backend->log("데몬 재시작 최종 실패 — TID fallback", "warning", "twitter");
            initTransactionIds();
        }

        m_accountRateLimitTime.clear();
        m_rateLimitHits = 0;
        m_backend->log("15분 대기 완료, 再開...", "success", "twitter");
    }
}

// ──────────────────────────────────────────────────────────────────────────
// captureTweet — 모든 type 분기 (likes/bookmarks/reposts/thread_comments/
//   thread/highlights/favoriters/retweeters/list/community/tweets/media/
//   replies/tweets_api)에서 호출되는 통합 캡쳐 헬퍼
//   realCapture=true일 때: SingleFile + CDP로 실제 x.com 페이지 통째 저장
//   false일 때 또는 캡쳐 실패 시: 합성 카드 HTML로 fallback
// ──────────────────────────────────────────────────────────────────────────
void TwitterCollector::captureTweet(const QJsonObject &tweet, const QString &capturesDir, const QJsonObject &config)
{
    if (capturesDir.isEmpty()) return;
    QJsonObject legacy = tweet["legacy"].toObject();
    QString tweetId = legacy["id_str"].toString();
    if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();
    if (tweetId.isEmpty()) return;

    QJsonObject userR = tweet["core"].toObject()["user_results"].toObject()["result"].toObject();
    QString screenName = userR["legacy"].toObject()["screen_name"].toString();
    if (screenName.isEmpty()) screenName = userR["screen_name"].toString();
    if (screenName.isEmpty()) screenName = "i";

    QString tweetUrl = QString("https://x.com/%1/status/%2").arg(screenName, tweetId);
    QString createdAt = legacy["created_at"].toString();
    QString filename = FileHelper::uploadOrderPrefix(Common::parseISODate(createdAt)) + tweetId;

    QDir().mkpath(capturesDir);

    const bool realCapture = config["realCapture"].toBool(true);
    if (realCapture) {
        // x.com 로그인 쿠키 주입 (NSFW/age-gated/private 컨텐츠 통과)
        QList<QNetworkCookie> ckList;
        if (!m_authToken.isEmpty()) {
            QNetworkCookie c1("auth_token", m_authToken.toUtf8());
            c1.setDomain(".x.com"); c1.setPath("/"); c1.setSecure(true);
            ckList << c1;
            QNetworkCookie c1b("auth_token", m_authToken.toUtf8());
            c1b.setDomain(".twitter.com"); c1b.setPath("/"); c1b.setSecure(true);
            ckList << c1b;
        }
        if (!m_ct0.isEmpty()) {
            QNetworkCookie c2("ct0", m_ct0.toUtf8());
            c2.setDomain(".x.com"); c2.setPath("/"); c2.setSecure(true);
            ckList << c2;
            QNetworkCookie c2b("ct0", m_ct0.toUtf8());
            c2b.setDomain(".twitter.com"); c2b.setPath("/"); c2b.setSecure(true);
            ckList << c2b;
        }
        // ★ 사용자가 UI에서 입력한 추가 raw cookie (NSFW/age-gate 통과: kdt, twid, gt 등)
        QString extraCk = config["captureCookie"].toString();
        if (!extraCk.isEmpty()) {
            for (const QString &part : extraCk.split(';', Qt::SkipEmptyParts)) {
                int eq = part.indexOf('=');
                if (eq <= 0) continue;
                QByteArray name = part.left(eq).trimmed().toUtf8();
                QByteArray value = part.mid(eq + 1).trimmed().toUtf8();
                // x.com + twitter.com 두 도메인 모두 set
                QNetworkCookie cx(name, value);
                cx.setDomain(".x.com"); cx.setPath("/"); cx.setSecure(true);
                ckList << cx;
                QNetworkCookie ct(name, value);
                ct.setDomain(".twitter.com"); ct.setPath("/"); ct.setSecure(true);
                ckList << ct;
            }
        }
        // ★ 트위터 로그인 체크 — 쿠키 만료 시 /i/flow/login으로 redirect되면 사용자 직접 처리
        //   article이 이미 있으면 로그인 페이지 아님 (검색창 input과 헷갈리지 않게)
        static const QString twLoginCheck = R"JS(
            (function(){
                if (location.pathname.indexOf('/i/flow/login') === 0
                 || location.pathname.indexOf('/login') === 0) return true;
                // article이 존재하면 로그인된 상태 — false positive 방지
                if (document.querySelector('article[data-testid="tweet"], article[role="article"]')) return false;
                // login 폼 셀렉터 (검색창과 구분: autocomplete=username 또는 password 타입)
                return !!document.querySelector('input[autocomplete="username"], input[type="password"]');
            })()
        )JS";
        if (m_backend->captureRealPageCDPLoginAware(tweetUrl, capturesDir, filename,
                                                     twLoginCheck, "twitter", 8000, ckList, config))
            return;
        // 캡쳐 실패 → 합성 카드로 fallback
    }

    // 합성 아카이브 카드 (realCapture 미사용 또는 CDP 실패)
    bool isRetweet = tweet.contains("retweeted_status_result");
    bool isReply = !legacy["in_reply_to_screen_name"].toString().isEmpty();
    QString authorName = userR["legacy"].toObject()["name"].toString();
    if (authorName.isEmpty()) authorName = screenName;

    QJsonObject meta;
    meta["authorName"]    = authorName;
    meta["handle"]        = screenName;
    meta["tweetId"]       = tweetId;
    meta["tweetUrl"]      = tweetUrl;
    meta["tweetText"]     = legacy["full_text"].toString();
    meta["createdAt"]     = createdAt;
    meta["tweetType"]     = isRetweet ? "Retweet" : (isReply ? "Reply" : "Tweet");
    meta["favoriteCount"] = legacy["favorite_count"].toInt();
    meta["retweetCount"]  = legacy["retweet_count"].toInt();
    meta["replyCount"]    = legacy["reply_count"].toInt();
    FileHelper::generateTweetArchiveHtml(capturesDir, filename, meta);
}

void TwitterCollector::collect(const QJsonObject &config, bool &isRunning)
{
    // 중지 시 진행 중인 미디어 다운로드를 즉시 끊기 위해 HttpClient에 '진행 플래그' 연결
    if (m_http) m_http->setRunFlag(&isRunning);

    QString target = config["target"].toString().replace("@", "");
    QString threadFocalId;  // 스레드 모드: 시작 트윗 ID
    // 스레드 모드: target이 URL이면 screen_name / tweet_id 로 분해
    if (config["type"].toString() == "thread") {
        QRegularExpression rx(R"((?:x\.com|twitter\.com)/([A-Za-z0-9_]+)/status/(\d+))");
        auto m = rx.match(target);
        if (m.hasMatch()) {
            target = m.captured(1);
            threadFocalId = m.captured(2);
        } else {
            // 숫자만 있으면 tweet ID로 간주, target은 비워둠
            QRegularExpression idRx("^\\d+$");
            if (idRx.match(target.trimmed()).hasMatch()) {
                threadFocalId = target.trimmed();
                target = "thread_"+ threadFocalId;
            }
        }
    }
    // URL/ID 기반 타입은 target을 안전한 폴더명으로 변환
    {
        QString _t = config["type"].toString();
        QString rawTarget = config["target"].toString();
        if (_t == "favoriters"|| _t == "retweeters") {
            QRegularExpression rx(R"((?:x\.com|twitter\.com)/[A-Za-z0-9_]+/status/(\d+))");
            auto mm = rx.match(rawTarget);
            QString id = mm.hasMatch() ? mm.captured(1) : rawTarget.trimmed();
            id.remove(QRegularExpression("[^0-9]"));
            if (id.isEmpty()) id = "tweet";
            target = _t + "_"+ id;
        } else if (_t == "list") {
            QRegularExpression rx(R"(/i/lists/(\d+))");
            auto mm = rx.match(rawTarget);
            QString id = mm.hasMatch() ? mm.captured(1) : rawTarget.trimmed();
            id.remove(QRegularExpression("[^0-9]"));
            if (id.isEmpty()) id = "list";
            target = "list_"+ id;
        } else if (_t == "community") {
            QRegularExpression rx(R"(/i/communities/(\d+))");
            auto mm = rx.match(rawTarget);
            QString id = mm.hasMatch() ? mm.captured(1) : rawTarget.trimmed();
            id.remove(QRegularExpression("[^0-9]"));
            if (id.isEmpty()) id = "community";
            target = "community_"+ id;
        }
    }
    QString savePath = config["path"].toString();
    savePath.replace("~", QDir::homePath());
    QJsonArray accounts = config["accounts"].toArray();
    QString mode = config["mode"].toString("all");

    // "all"모드에서 재귀 호출 시 _subCall=true → 상태 초기화 건너뜀
    bool isSubCall = config["_subCall"].toBool(false);

    if (!isSubCall) {
        // 새트윗 확인용 변수 초기화 (최상위 호출만)
        m_currentTarget = target;
        m_newestTweetId.clear();
        // ★ multi-target 큐: 직전 target에서 누적된 RL 추적 상태가 새 target/계정에 잘못 적용되지 않도록 reset
        m_accountRateLimitTime.clear();
        m_rateLimitHits = 0;
    }

    // Adaptive delay — user no longer sets this manually
    m_delay = 2.0;  // base delay, auto-adjusts on rate limits
    m_consecutiveOk = 0;
    m_mediaQuality = config["mediaQuality"].toString("orig");
    m_downloadMedia = config["downloadMedia"].toBool(true);
    m_saveExcel = config["excel"].toBool(true);
    m_saveExif = config["exif"].toBool(true);
    m_saveProgress = config["saveProgress"].toBool(false);  // 이어서 수집 기능 (체크박스)

    if (accounts.isEmpty()) {
        m_backend->log("Add account first", "error", "twitter");
        return;
    }

    QString twitterDir = savePath + "/twitter";
    QDir().mkpath(twitterDir);
    QString userDir = twitterDir + "/"+ target;
    QString mediaDir = userDir + "/media";
    QDir().mkpath(mediaDir);

    if (!isSubCall) {
        m_downloadedProfiles.clear();
        // Create disk-based profile buffer (no RAM bloat for 30000+ profiles)
        QString profTempDir = userDir + "/.tmp_profiles";
        QDir().mkpath(profTempDir);
        if (m_profileBuffer) { delete m_profileBuffer; m_profileBuffer = nullptr; }
        m_profileBuffer = new DiskJsonBuffer(profTempDir, "tw_profiles");
    }

    m_backend->log("Target: @"+ target, "info", "twitter");
    m_backend->log("Save: "+ userDir, "info", "twitter");

    // Setup first account
    QJsonObject firstAccount = accounts[0].toObject();
    setupClient(firstAccount["auth_token"].toString(), firstAccount["ct0"].toString());
    m_backend->log(QString("Account: %1 (1/%2)")
                       .arg(firstAccount["name"].toString("Twitter"))
                       .arg(accounts.size()),
                   "success", "twitter");

    // Start persistent Python daemon (twikit - handles TID per request)
    if (!startDaemon()) {
        m_backend->log("Failed to start twikit daemon, falling back to TID mode", "warning", "twitter");
        initTransactionIds();
    }

    // 일부 타입은 유저 대신 트윗/리스트/커뮤니티 ID 기반이라 user lookup 생략
    QString _prelimType = config["type"].toString("tweets");
    bool skipUserLookup = (_prelimType == "favoriters"|| _prelimType == "retweeters"||
                           _prelimType == "list"|| _prelimType == "community");

    QJsonObject user;
    if (!skipUserLookup) {
        user = getUserByScreenName(target);
        if (user.isEmpty()) {
            m_backend->log("사용자를 찾을 수 없습니다: @"+ target, "error", "twitter");
            m_backend->log("아이디를 확인해주세요.", "info", "twitter");
            m_backend->log("Complete.", "info", "twitter");
            stopDaemon();
            return;
        }
    }

    QJsonObject userLegacy = user["legacy"].toObject();
    QString userName = userLegacy["name"].toString();
    QString userId = user["rest_id"].toString();
    int statusesCount = userLegacy["statuses_count"].toInt();
    int followersCount = userLegacy["followers_count"].toInt();
    int followingCount = userLegacy["friends_count"].toInt();
    if (!skipUserLookup) {
        m_backend->log(QString("User: %1 (@%2)").arg(userName, target), "success", "twitter");
        m_backend->log(QString("Tweets: %1 | Followers: %2 | Following: %3").arg(statusesCount).arg(followersCount).arg(followingCount), "info", "twitter");
    }

    QString type = config["type"].toString("tweets");

    // ── ALL: 전체 수집 (트윗 + 답글 + 좋아요 + 리포스트 + 팔로워 + 팔로잉 + 프로필) ──
    if (type == "all") {
        m_backend->log("═══ 전체 수집 모드 ═══", "success", "twitter");

        // 1. Profile
        m_backend->log("[1/7] 프로필 수집...", "info", "twitter");
        {
            QJsonObject subConfig = config;
            subConfig["type"] = QString("profile");
            // Inline profile save (skip daemon restart)
            ExcelWriter writer;
            QStringList profileHdrs = {"Field", "Value"};
            writer.writeHeader(profileHdrs, QColor("#d97757"));
            int prow = 2;
            auto addRow = [&](const QString &field, const QString &value) {
                writer.writeRow(prow++, {field, value});
            };
            addRow("ID", userId);
            addRow("Handle", "@"+ target);
            addRow("Name", userName);
            addRow("Bio", userLegacy["description"].toString());
            addRow("Location", userLegacy["location"].toString());
            addRow("URL", userLegacy["url"].toString());
            addRow("Verified", user["is_blue_verified"].toBool() ? "Yes": "No");
            addRow("Protected", userLegacy["protected"].toBool() ? "Yes": "No");
            addRow("Followers", QString::number(followersCount));
            addRow("Following", QString::number(followingCount));
            addRow("Tweets", QString::number(statusesCount));
            addRow("Likes", QString::number(userLegacy["favourites_count"].toInt()));
            addRow("Created", Common::formatDateJapanese(userLegacy["created_at"].toString()));
            // Birthday (if available from legacy_extended_profile)
            {
                QJsonObject extProfile = user["legacy_extended_profile"].toObject();
                QJsonObject birthdate = extProfile["birthdate"].toObject();
                if (!birthdate.isEmpty()) {
                    QString bday;
                    int day = birthdate["day"].toInt();
                    int month = birthdate["month"].toInt();
                    int year = birthdate["year"].toInt();
                    if (year > 0) bday = QString("%1年%2月%3日").arg(year).arg(month).arg(day);
                    else if (month > 0 && day > 0) bday = QString("%1月%2日").arg(month).arg(day);
                    if (!bday.isEmpty()) addRow("Birthday", bday);
                }
            }
            addRow("Profile Image", userLegacy["profile_image_url_https"].toString().replace("_normal", "_400x400"));
            addRow("Banner", userLegacy["profile_banner_url"].toString());
            writer.setColumnWidth(1, 20);
            writer.setColumnWidth(2, 60);

            // 통일된 프로필 폴더: profiles/target/{이름(@handle)}/
            QString dateTag = QDate::currentDate().toString("yyyyMMdd");
            QString tDisplayName = userName;
            tDisplayName.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
            tDisplayName = tDisplayName.trimmed().left(40);
            QString tFilePrefix = !tDisplayName.isEmpty() ? QString("%1(@%2)").arg(tDisplayName, target) : target;
            QString tDirName = tFilePrefix;
            tDirName.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
            QString targetProfileDir = userDir + "/profiles/target/"+ tDirName;
            QDir().mkpath(targetProfileDir);

            QString pProfilePath = targetProfileDir + "/"+ target + "_profile.xlsx";
            writer.save(pProfilePath);
            FileHelper::setDownloadMeta(pProfilePath, "ABIWA Twitter");
            QString profileImgUrl = userLegacy["profile_image_url_https"].toString().replace("_normal", "_400x400");
            if (!profileImgUrl.isEmpty()) {
                QString pp = targetProfileDir + "/profile_"+ dateTag + ".jpg";
                if (!QFile::exists(pp)) downloadMedia(profileImgUrl, pp);
            }
            QString bannerUrl = userLegacy["profile_banner_url"].toString();
            if (!bannerUrl.isEmpty()) {
                QString bp = targetProfileDir + "/banner_"+ dateTag + ".jpg";
                if (!QFile::exists(bp)) downloadMedia(bannerUrl + "/1500x500", bp);
            }
            m_backend->log("프로필 완료 ", "success", "twitter");
        }
        if (!isRunning) { stopDaemon(); return; }

        // Helper lambda (unused — kept for reference)
        auto runSubCollect = [&](const QString &subType, const QString &label, int step) {
            m_backend->log(QString("[%1/8] %2 수집...").arg(step).arg(label), "info", "twitter");
        };

        // Helper: "all"재귀 호출 시 _subCall=true → 상태 초기화 건너뜀
        auto makeSubConfig = [&](const QString &subType) -> QJsonObject {
            QJsonObject subConfig = config;
            subConfig["type"] = subType;
            subConfig["_subCall"] = true;
            return subConfig;
        };

        // 2. Tweets
        m_backend->log("[2/8] 트윗 수집...", "info", "twitter");
        collect(makeSubConfig("tweets"), isRunning);
        if (!isRunning) return;

        // Re-setup daemon (previous collect stopped it)
        setupClient(accounts[0].toObject()["auth_token"].toString(), accounts[0].toObject()["ct0"].toString());
        if (!startDaemon()) initTransactionIds();
        QJsonObject user2 = getUserByScreenName(target);
        if (user2.isEmpty()) { stopDaemon(); return; }

        // 3. Replies
        m_backend->log("[3/8] 답글 수집...", "info", "twitter");
        collect(makeSubConfig("replies"), isRunning);
        if (!isRunning) return;

        setupClient(accounts[0].toObject()["auth_token"].toString(), accounts[0].toObject()["ct0"].toString());
        if (!startDaemon()) initTransactionIds();
        getUserByScreenName(target);

        // 4. Likes
        m_backend->log("[4/8] 좋아요 수집...", "info", "twitter");
        collect(makeSubConfig("likes"), isRunning);
        if (!isRunning) return;

        setupClient(accounts[0].toObject()["auth_token"].toString(), accounts[0].toObject()["ct0"].toString());
        if (!startDaemon()) initTransactionIds();
        getUserByScreenName(target);

        // 5. Reposts
        m_backend->log("[5/8] 리포스트 수집...", "info", "twitter");
        collect(makeSubConfig("reposts"), isRunning);
        if (!isRunning) return;

        // ── 리포스트를 _complete.xlsx에도 추가 ──
        {
            // _reposts.xlsx는 이제 complete 형식이므로 직접 읽어서 complete에 병합
            QString repostsPath = userDir + "/"+ target + "_reposts.xlsx";
            if (QFile::exists(repostsPath)) {
                QXlsx::Document rpDoc(repostsPath);
                int rpLastRow = rpDoc.dimension().lastRow();
                int rpColCount = rpDoc.dimension().lastColumn();
                if (rpColCount < 15) rpColCount = 30;

                QString rpTmp = userDir + "/.tmp";
                QDir().mkpath(rpTmp);
                DiskJsonBuffer rpBuf(rpTmp, "tw_rp_merge");

                for (int r = 2; r <= rpLastRow; ++r) {
                    QString rpId = rpDoc.read(r, 1).toString().trimmed();
                    if (rpId.isEmpty() || rpId.startsWith("─") || rpId.startsWith("===")) continue;

                    QJsonObject d;
                    d["id"] = rpId;
                    d["tweet_url"] = rpDoc.read(r, 2).toString();
                    d["text"] = rpDoc.read(r, 3).toString();
                    d["language"] = rpDoc.read(r, 4).toString();
                    d["type"] = rpDoc.read(r, 5).toString();
                    d["author_name"] = rpDoc.read(r, 6).toString();
                    d["author_username"] = rpDoc.read(r, 7).toString();
                    d["bookmark_count"] = rpDoc.read(r, 8).toString().toInt();
                    d["favorite_count"] = rpDoc.read(r, 9).toString().toInt();
                    d["retweet_count"] = rpDoc.read(r, 10).toString().toInt();
                    d["retweeted"] = rpDoc.read(r, 11).toString();
                    d["reply_count"] = rpDoc.read(r, 12).toString().toInt();
                    d["quote_count"] = rpDoc.read(r, 13).toString().toInt();
                    d["view_count"] = rpDoc.read(r, 14).toString();
                    d["created_at"] = rpDoc.read(r, 15).toString();
                    d["retweet_time"] = rpDoc.read(r, 16).toString();
                    d["source"] = rpDoc.read(r, 17).toString();
                    rpBuf.append(d);
                }

                if (rpBuf.count() > 0) {
                    m_backend->log(QString("리포스트 %1개를 complete 파일에 추가...").arg(rpBuf.count()), "info", "twitter");
                    saveExcelStreaming(userDir, target, rpBuf, "tweets");
                }
            }
        }

        setupClient(accounts[0].toObject()["auth_token"].toString(), accounts[0].toObject()["ct0"].toString());
        if (!startDaemon()) initTransactionIds();
        getUserByScreenName(target);

        // 6. Thread Comments (게시물 댓글)
        if (config["threadComments"].toBool(false)) {
            m_backend->log("[6/8] 게시물 댓글 수집...", "info", "twitter");
            collect(makeSubConfig("thread_comments"), isRunning);
            if (!isRunning) return;

            setupClient(accounts[0].toObject()["auth_token"].toString(), accounts[0].toObject()["ct0"].toString());
            if (!startDaemon()) initTransactionIds();
            getUserByScreenName(target);
        } else {
            m_backend->log("[6/8] 게시물 댓글 — 건너뜀 (체크박스 해제)", "info", "twitter");
        }

        // 7. Followers
        m_backend->log("[7/8] 팔로워 수집...", "info", "twitter");
        collect(makeSubConfig("followers"), isRunning);
        if (!isRunning) return;

        setupClient(accounts[0].toObject()["auth_token"].toString(), accounts[0].toObject()["ct0"].toString());
        if (!startDaemon()) initTransactionIds();
        getUserByScreenName(target);

        // 8. Following
        m_backend->log("[8/8] 팔로잉 수집...", "info", "twitter");
        collect(makeSubConfig("following"), isRunning);

        m_backend->log("═══ 전체 수집 완료! ═══", "success", "twitter");
        return;
    }

    // ── Followers / Following (separate flow) ──
    if (type == "followers"|| type == "following") {
        m_backend->log(QString("Collecting %1...").arg(type), "info", "twitter");

        QString twTempDir = config["tempDir"].toString();
        if (twTempDir.isEmpty()) twTempDir = userDir + "/.tmp";
        DiskJsonBuffer allUsers(twTempDir, "tw_"+ type);
        QString cursor;
        int currentAccountIdx = 0;

        while (isRunning) {
            QJsonObject cmd;
            cmd["action"] = type;  // "followers" or "following"
            cmd["user_id"] = userId;  // ★ BUG: 이전엔 주석 끝에 붙어서 실행 안 됨
            cmd["count"] = 40;
            if (!cursor.isEmpty()) cmd["cursor"] = cursor;

            QJsonObject resp = sendDaemonCommand(cmd);
            if (resp.contains("error")) {
                m_backend->log(type + "error: "+ resp["error"].toString(), "error", "twitter");
                break;
            }

            int status = resp["status"].toInt();
            if (status == 429) {
                handleRateLimit(accounts, currentAccountIdx, isRunning);
                continue;
            }
            if (status != 200) {
                m_backend->log(QString("API error: %1").arg(status), "error", "twitter");
                break;
            }

            QJsonObject data = resp["body"].toObject();
            QJsonArray instructions = data["data"].toObject()["user"].toObject()["result"].toObject()
                ["timeline"].toObject()["timeline"].toObject()["instructions"].toArray();

            bool foundEntries = false;
            QString nextCursor;
            for (const auto &inst : instructions) {
                QJsonObject instruction = inst.toObject();
                if (instruction["type"].toString() == "TimelineAddEntries") {
                    QJsonArray entries = instruction["entries"].toArray();
                    for (const auto &entry : entries) {
                        QJsonObject e = entry.toObject();
                        QString eid = e["entryId"].toString();
                        if (eid.startsWith("user-")) {
                            QJsonObject userResult = e["content"].toObject()["itemContent"].toObject()
                                ["user_results"].toObject()["result"].toObject();
                            if (!userResult.isEmpty()) {
                                allUsers.append(userResult);
                                foundEntries = true;
                            }
                        } else if (eid.startsWith("cursor-bottom")) {
                            nextCursor = e["content"].toObject()["value"].toString();
                        }
                    }
                }
            }

            m_backend->updateStats(allUsers.count(), 0, "수집 중...", "twitter");
            m_backend->log(QString("%1: %2명").arg(type).arg(allUsers.count()), "info", "twitter");

            if (nextCursor.isEmpty() || !foundEntries) break;
            cursor = nextCursor;
            m_consecutiveOk++; if (m_consecutiveOk > 5) { m_delay = qMax(m_delay * 0.9, 1.0); m_rateLimitHits = 0; }
            QThread::msleep(static_cast<unsigned long>(m_delay * 1000));
        }

        // Download profile pics & banners — downloadUserProfileMedia() 통일 방식
        if (allUsers.count() > 0) {
            QString profileDir = userDir + "/profiles";
            m_backend->log(QString("프로필/배너 다운로드 중... (%1명)").arg(allUsers.count()), "info", "twitter");
            m_downloadedProfiles.clear(); // 이번 세션 중복 방지 리셋
            allUsers.resetReader();
            QJsonObject pu;
            int dlCount = 0;
            while (allUsers.readNext(pu)) {
                if (!isRunning) break;
                QJsonObject pLeg = pu["legacy"].toObject();
                QJsonObject pCore = pu["core"].toObject();
                QString handle = pLeg["screen_name"].toString();
                if (handle.isEmpty()) handle = pCore["screen_name"].toString();
                if (handle.isEmpty()) handle = pu["screen_name"].toString();
                if (handle.isEmpty()) continue;

                if (m_downloadedProfiles.contains(handle)) continue;
                m_downloadedProfiles.insert(handle);

                QString displayName = pLeg["name"].toString();
                if (displayName.isEmpty()) displayName = pCore["name"].toString();
                if (displayName.isEmpty()) displayName = pu["name"].toString();
                displayName.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
                displayName = displayName.trimmed().left(40);
                QString filePrefix = !displayName.isEmpty() ? QString("%1(@%2)").arg(displayName, handle) : handle;

                // 유저별 서브폴더: profiles/{type}/{이름(@handle)}/
                QString userDirName = filePrefix;
                userDirName.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
                QString userProfileDir = profileDir + "/"+ type + "/"+ userDirName;
                QDir().mkpath(userProfileDir);

                QString dateTag = QDateTime::currentDateTime().toString("yyyyMMdd");

                // Profile image (400x400) — 새 API 구조 대응
                QString profUrl = pLeg["profile_image_url_https"].toString();
                if (profUrl.isEmpty()) profUrl = pCore["profile_image_url_https"].toString();
                if (profUrl.isEmpty()) profUrl = pu["profile_image_url_https"].toString();
                if (profUrl.isEmpty()) {
                    QJsonValue avatarVal = pu["avatar"];
                    if (avatarVal.isObject()) profUrl = avatarVal.toObject()["image_url"].toString();
                    else if (avatarVal.isString()) profUrl = avatarVal.toString();
                }
                if (!profUrl.isEmpty()) {
                    profUrl.replace("_normal", "_400x400");
                    QString path = userProfileDir + "/profile_"+ dateTag + ".jpg";
                    if (!QFile::exists(path)) downloadMedia(profUrl, path);
                }
                // Banner (1500x500)
                QString bannerUrl = pLeg["profile_banner_url"].toString();
                if (!bannerUrl.isEmpty()) {
                    QString path = userProfileDir + "/banner_"+ dateTag + ".jpg";
                    if (!QFile::exists(path)) downloadMedia(bannerUrl + "/1500x500", path);
                }

                // Excel 용 프로필 데이터 수집
                if (m_profileBuffer) {
                    QJsonObject profileInfo;
                    profileInfo["screen_name"] = handle;
                    profileInfo["name"] = displayName;
                    profileInfo["description"] = pLeg["description"].toString();
                    profileInfo["followers_count"] = pLeg["followers_count"].toInt();
                    profileInfo["friends_count"] = pLeg["friends_count"].toInt();
                    profileInfo["statuses_count"] = pLeg["statuses_count"].toInt();
                    profileInfo["location"] = pLeg["location"].toString();
                    profileInfo["url"] = pLeg["url"].toString();
                    profileInfo["created_at"] = Common::formatDateJapanese(pLeg["created_at"].toString());
                    profileInfo["verified"] = pu["is_blue_verified"].toBool() ? "True": "False";
                    profileInfo["profile_image_url"] = profUrl;
                    profileInfo["profile_banner_url"] = bannerUrl;
                    m_profileBuffer->append(profileInfo);
                }

                dlCount++;
                if (dlCount % 50 == 0) {
                    m_backend->updateStats(allUsers.count(), dlCount, "프로필 다운 중...", "twitter");
                }
            }
            m_backend->log(QString("프로필/배너 %1명 다운 완료").arg(dlCount), "success", "twitter");
        }

        // Save Excel (streaming — no readAll)
        if (allUsers.count() > 0) {
            ExcelWriter writer;
            QStringList hdrs = {"ID", "Handle", "Name", "Profile URL", "Bio", "Followers", "Following", "Tweets", "Likes", "Created At", "Verified", "Location", "Profile Image", "Banner"};
            writer.writeHeader(hdrs, QColor("#d97757"));
            int row = 2;
            allUsers.resetReader();
            QJsonObject uo;
            while (allUsers.readNext(uo)) {
                QJsonObject leg = uo["legacy"].toObject();
                QString handle = leg["screen_name"].toString();
                writer.writeRow(row++, {
                    uo["rest_id"].toString(),
                    handle,
                    leg["name"].toString(),
                    QString("https://x.com/%1").arg(handle),
                    leg["description"].toString().left(300),
                    QString::number(leg["followers_count"].toInt()),
                    QString::number(leg["friends_count"].toInt()),
                    QString::number(leg["statuses_count"].toInt()),
                    QString::number(leg["favourites_count"].toInt()),
                    Common::formatDateJapanese(leg["created_at"].toString()),
                    uo["is_blue_verified"].toBool() ? "Yes": "",
                    leg["location"].toString(),
                    leg["profile_image_url_https"].toString().replace("_normal", "_400x400"),
                    leg["profile_banner_url"].toString()
                });
            }
            writer.setColumnWidth(1, 20);   // ID
            writer.setColumnWidth(2, 18);   // Handle
            writer.setColumnWidth(3, 20);   // Name
            writer.setColumnWidth(4, 30);   // Profile URL
            writer.setColumnWidth(5, 40);   // Bio
            writer.setColumnWidth(6, 10);   // Followers
            writer.setColumnWidth(7, 10);   // Following
            writer.setColumnWidth(8, 8);    // Tweets
            writer.setColumnWidth(9, 8);    // Likes
            writer.setColumnWidth(10, 28);  // Created At
            writer.setColumnWidth(11, 8);   // Verified
            writer.setColumnWidth(12, 20);  // Location
            writer.setColumnWidth(13, 50);  // Profile Image URL
            writer.setColumnWidth(14, 50);  // Banner URL
            QString ffPath = userDir + "/"+ target + "_"+ type + ".xlsx";
            writer.save(ffPath);
            FileHelper::setDownloadMeta(ffPath, "ABIWA Twitter");
            m_backend->log("Excel 저장 완료", "success", "twitter");
        }

        m_backend->log(QString("완료! %1: %2명").arg(type).arg(allUsers.count()), "success", "twitter");
        m_backend->updateStats(allUsers.count(), 0, "완료", "twitter");
        stopDaemon();
        return;
    }

    // ── Profile ──
    if (type == "profile") {
        m_backend->log("프로필 수집...", "info", "twitter");

        ExcelWriter writer;
        QStringList profileHdrs = {"Field", "Value"};
        writer.writeHeader(profileHdrs, QColor("#d97757"));

        int prow = 2;
        auto addRow = [&](const QString &field, const QString &value) {
            writer.writeRow(prow++, {field, value});
        };

        addRow("ID", userId);
        addRow("Handle", "@"+ target);
        addRow("Name", userName);
        addRow("Bio", userLegacy["description"].toString());
        addRow("Location", userLegacy["location"].toString());
        addRow("URL", userLegacy["url"].toString());
        addRow("Verified", user["is_blue_verified"].toBool() ? "Yes": "No");
        addRow("Protected", userLegacy["protected"].toBool() ? "Yes": "No");
        addRow("Followers", QString::number(followersCount));
        addRow("Following", QString::number(followingCount));
        addRow("Tweets", QString::number(statusesCount));
        addRow("Likes", QString::number(userLegacy["favourites_count"].toInt()));
        addRow("Created", Common::formatDateJapanese(userLegacy["created_at"].toString()));
        // Birthday
        {
            QJsonObject extProfile = user["legacy_extended_profile"].toObject();
            QJsonObject birthdate = extProfile["birthdate"].toObject();
            if (!birthdate.isEmpty()) {
                QString bday;
                int day = birthdate["day"].toInt();
                int month = birthdate["month"].toInt();
                int year = birthdate["year"].toInt();
                if (year > 0) bday = QString("%1年%2月%3日").arg(year).arg(month).arg(day);
                else if (month > 0 && day > 0) bday = QString("%1月%2日").arg(month).arg(day);
                if (!bday.isEmpty()) addRow("Birthday", bday);
            }
        }
        addRow("Profile Image", userLegacy["profile_image_url_https"].toString().replace("_normal", "_400x400"));
        addRow("Banner", userLegacy["profile_banner_url"].toString());

        writer.setColumnWidth(1, 20);
        writer.setColumnWidth(2, 60);

        // 날짜별 프로필 폴더에 저장 (덮어쓰기 방지)
        {
            QString pdateStr = QDate::currentDate().toString("yyyy-MM-dd");
            QString pDateDir = userDir + "/profiles/"+ pdateStr;
            QDir().mkpath(pDateDir);
            QString pPath2 = pDateDir + "/"+ target + "_profile.xlsx";
            writer.save(pPath2);
            FileHelper::setDownloadMeta(pPath2, "ABIWA Twitter");
            // 프로필 Excel은 profiles/ 폴더 안에만 저장
        }
        m_backend->log("프로필 저장 완료", "success", "twitter");

        // Download profile picture (날짜별 폴더)
        {
            QString pdateStr = QDate::currentDate().toString("yyyy-MM-dd");
            QString pDateDir = userDir + "/profiles/"+ pdateStr;
            QDir().mkpath(pDateDir);
            QString profileImgUrl = userLegacy["profile_image_url_https"].toString().replace("_normal", "_400x400");
            if (!profileImgUrl.isEmpty()) {
                QString pp = pDateDir + "/"+ target + "_profile.jpg";
                if (!QFile::exists(pp) && downloadMedia(profileImgUrl, pp)) {
                    m_backend->log("프로필 사진 다운로드 완료", "success", "twitter");
                }
            }
            QString bannerUrl = userLegacy["profile_banner_url"].toString();
            if (!bannerUrl.isEmpty()) {
                QString bp = pDateDir + "/"+ target + "_banner.jpg";
                if (!QFile::exists(bp)) downloadMedia(bannerUrl + "/1500x500", bp);
            }
        }

        m_backend->updateStats(1, 0, "완료", "twitter");
        stopDaemon();
        return;
    }

    // ── Likes (dedicated GraphQL endpoint) ──
    if (type == "likes") {
        m_backend->log("좋아요 수집 (Likes API)...", "info", "twitter");

        QString likesMediaDir = mediaDir + "/likes";
        QDir().mkpath(likesMediaDir);

        QString likesTempDir = config["tempDir"].toString();
        if (likesTempDir.isEmpty()) likesTempDir = userDir + "/.tmp";
        int tweetCount = 0, mediaCount = 0;
        DiskJsonBuffer collectedData(likesTempDir, "tw_likes");
        QSet<QString> collectedIds;
        int currentAccountIdx = 0;
        QString cursor;
        bool hasMore = true;
        int consecutiveEmptyLikes = 0;

        while (isRunning && hasMore) {
            auto [tweets, nextCursor] = getLikes(userId, cursor);

            if (nextCursor == "RATE_LIMITED") {
                handleRateLimit(accounts, currentAccountIdx, isRunning);
                continue;
            }
            if (nextCursor == "ERROR") {
                m_backend->log("API 오류 — 5초 후 재시도", "warning", "twitter");
                for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                continue;
            }

            if (tweets.isEmpty()) {
                consecutiveEmptyLikes++;
                if (consecutiveEmptyLikes >= 5) break;
                m_backend->log(QString("빈 응답, 재시도 %1/5").arg(consecutiveEmptyLikes), "warning", "twitter");
                QThread::sleep(3);
                continue;
            }
            consecutiveEmptyLikes = 0;

            for (const auto &tweetVal : tweets) {
                if (!isRunning) break;
                QJsonObject tweet = tweetVal.toObject();
                QJsonObject legacy = tweet["legacy"].toObject();
                QString tweetId = legacy["id_str"].toString();
                if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();
                if (collectedIds.contains(tweetId)) continue;
                collectedIds.insert(tweetId);

                tweetCount++;
                m_backend->log(QString("[%1] %2...").arg(tweetCount).arg(legacy["full_text"].toString().left(40)), "info", "twitter");
                m_backend->updateStats(tweetCount, mediaCount, "수집 중...", "twitter");

                int newMedia = downloadTweetMedia(tweet, likesMediaDir, tweetCount);
                mediaCount += newMedia;

                // 캡쳐 (likes 별도 폴더)
                captureTweet(tweet, userDir + "/captures/likes", config);

                // Anipo-style batch progress log
                if (tweetCount % 10 == 0) {
                    m_backend->log(QString("트윗 %1개, 미디어 %2개 다운로드").arg(tweetCount).arg(mediaCount), "info", "twitter");
                }

                QJsonObject core = tweet["core"].toObject()["user_results"].toObject()["result"].toObject()["legacy"].toObject();
                QString screenName = core["screen_name"].toString();

                QJsonObject data;
                data["id"] = tweetId;
                data["text"] = legacy["full_text"].toString();
                data["url"] = QString("https://x.com/%1/status/%2").arg(screenName.isEmpty() ? "i": screenName, tweetId);
                {
                    QDateTime likeDt = Common::parseISODate(legacy["created_at"].toString());
                    if (likeDt.isValid()) {
                        likeDt = likeDt.toUTC().addSecs(9 * 3600);
                        data["created_at"] = likeDt.toString("yyyy/MM/dd HH:mm");
                    } else {
                        data["created_at"] = legacy["created_at"].toString();
                    }
                }
                data["author"] = screenName;
                data["retweet_count"] = legacy["retweet_count"].toInt();
                data["favorite_count"] = legacy["favorite_count"].toInt();
                data["reply_count"] = legacy["reply_count"].toInt();
                data["quote_count"] = legacy["quote_count"].toInt();
                data["view_count"] = tweet["views"].toObject()["count"].toString();
                data["lang"] = legacy["lang"].toString();
                data["in_reply_to"] = legacy["in_reply_to_screen_name"].toString();
                // Media types
                QJsonArray mediaArr = legacy["extended_entities"].toObject()["media"].toArray();
                if (mediaArr.isEmpty()) mediaArr = legacy["entities"].toObject()["media"].toArray();
                QStringList mediaTypes;
                for (const auto &m : mediaArr) {
                    QString mt = m.toObject()["type"].toString();
                    if (!mt.isEmpty() && !mediaTypes.contains(mt)) mediaTypes << mt;
                }
                data["media_types"] = mediaTypes.join(", ");
                data["sensitive"] = legacy["possibly_sensitive"].toBool() ? "Yes": "";
                collectedData.append(data);

                // 좋아요 유저 프로필 다운 (anipo style)
                downloadUserProfileMedia(tweet, userDir + "/profiles", "likes");
            }

            cursor = nextCursor;
            if (cursor.isEmpty()) hasMore = false;
            if (hasMore) { for (int w = 10; w > 0 && isRunning; --w) QThread::sleep(1); }
        }

        if (collectedData.count() > 0) saveExcelStreaming(userDir, target, collectedData, "likes");
        m_backend->log(QString("완료! 좋아요: %1, 미디어: %2").arg(tweetCount).arg(mediaCount), "success", "twitter");
        m_backend->updateStats(tweetCount, mediaCount, "완료", "twitter");
        stopDaemon();
        return;
    }

    // ── Bookmarks (dedicated GraphQL endpoint, own account only) ──
    if (type == "bookmarks") {
        m_backend->log("북마크 수집...", "info", "twitter");

        QString bookmarksMediaDir = mediaDir + "/bookmarks";
        QDir().mkpath(bookmarksMediaDir);

        QString bkmkTempDir = config["tempDir"].toString();
        if (bkmkTempDir.isEmpty()) bkmkTempDir = userDir + "/.tmp";
        int tweetCount = 0, mediaCount = 0;
        DiskJsonBuffer collectedData(bkmkTempDir, "tw_bookmarks");
        QSet<QString> collectedIds;
        int currentAccountIdx = 0;
        QString cursor;
        bool hasMore = true;
        int consecutiveEmptyBkmk = 0;

        while (isRunning && hasMore) {
            auto [tweets, nextCursor] = getBookmarks(cursor);

            if (nextCursor == "RATE_LIMITED") {
                handleRateLimit(accounts, currentAccountIdx, isRunning);
                continue;
            }
            if (nextCursor == "ERROR") {
                m_backend->log("API 오류 — 5초 후 재시도", "warning", "twitter");
                for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                continue;
            }

            if (tweets.isEmpty()) {
                consecutiveEmptyBkmk++;
                if (consecutiveEmptyBkmk >= 5) break;
                m_backend->log(QString("빈 응답, 재시도 %1/5").arg(consecutiveEmptyBkmk), "warning", "twitter");
                QThread::sleep(3);
                continue;
            }
            consecutiveEmptyBkmk = 0;

            for (const auto &tweetVal : tweets) {
                if (!isRunning) break;
                QJsonObject tweet = tweetVal.toObject();
                QJsonObject legacy = tweet["legacy"].toObject();
                QString tweetId = legacy["id_str"].toString();
                if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();
                if (collectedIds.contains(tweetId)) continue;
                collectedIds.insert(tweetId);

                tweetCount++;
                QString tweetText = legacy["full_text"].toString().left(40);
                m_backend->log(QString("[%1] %2...").arg(tweetCount).arg(tweetText), "info", "twitter");
                m_backend->updateStats(tweetCount, mediaCount, "수집 중...", "twitter");

                int newMedia = downloadTweetMedia(tweet, bookmarksMediaDir, tweetCount);
                mediaCount += newMedia;

                // 캡쳐 (bookmarks 별도 폴더)
                captureTweet(tweet, userDir + "/captures/bookmarks", config);

                // Anipo-style batch progress log
                if (tweetCount % 10 == 0) {
                    m_backend->log(QString("트윗 %1개, 미디어 %2개 다운로드").arg(tweetCount).arg(mediaCount), "info", "twitter");
                }

                // Get tweet author info
                QJsonObject core = tweet["core"].toObject()["user_results"].toObject()["result"].toObject()["legacy"].toObject();
                QString screenName = core["screen_name"].toString();

                QJsonObject data;
                data["id"] = tweetId;
                data["text"] = legacy["full_text"].toString();
                data["url"] = QString("https://x.com/%1/status/%2").arg(screenName.isEmpty() ? "i": screenName, tweetId);
                {
                    QDateTime bkmkDt = Common::parseISODate(legacy["created_at"].toString());
                    if (bkmkDt.isValid()) {
                        bkmkDt = bkmkDt.toUTC().addSecs(9 * 3600);
                        data["created_at"] = bkmkDt.toString("yyyy/MM/dd HH:mm");
                    } else {
                        data["created_at"] = legacy["created_at"].toString();
                    }
                }
                data["author"] = screenName;
                data["retweet_count"] = legacy["retweet_count"].toInt();
                data["favorite_count"] = legacy["favorite_count"].toInt();
                data["reply_count"] = legacy["reply_count"].toInt();
                data["quote_count"] = legacy["quote_count"].toInt();
                data["view_count"] = tweet["views"].toObject()["count"].toString();
                data["lang"] = legacy["lang"].toString();
                data["in_reply_to"] = legacy["in_reply_to_screen_name"].toString();
                // Media types
                QJsonArray mediaArr = legacy["extended_entities"].toObject()["media"].toArray();
                if (mediaArr.isEmpty()) mediaArr = legacy["entities"].toObject()["media"].toArray();
                QStringList mediaTypes;
                for (const auto &m : mediaArr) {
                    QString mt = m.toObject()["type"].toString();
                    if (!mt.isEmpty() && !mediaTypes.contains(mt)) mediaTypes << mt;
                }
                data["media_types"] = mediaTypes.join(", ");
                data["sensitive"] = legacy["possibly_sensitive"].toBool() ? "Yes": "";
                collectedData.append(data);
            }

            cursor = nextCursor;
            if (cursor.isEmpty()) hasMore = false;
            if (hasMore) { for (int w = 10; w > 0 && isRunning; --w) QThread::sleep(1); }
        }

        if (collectedData.count() > 0) saveExcelStreaming(userDir, target, collectedData, "bookmarks");
        m_backend->log(QString("완료! 북마크: %1, 미디어: %2").arg(tweetCount).arg(mediaCount), "success", "twitter");
        m_backend->updateStats(tweetCount, mediaCount, "완료", "twitter");
        stopDaemon();
        return;
    }

    // ── Reposts (UserTweets-based, filter for retweets) ──
    if (type == "reposts") {
        m_backend->log("리포스트 수집...", "info", "twitter");

        QString repostsMediaDir = mediaDir + "/reposts";
        QDir().mkpath(repostsMediaDir);

        QString rpTempDir = config["tempDir"].toString();
        if (rpTempDir.isEmpty()) rpTempDir = userDir + "/.tmp";
        int tweetCount = 0, mediaCount = 0;
        DiskJsonBuffer collectedData(rpTempDir, "tw_reposts");
        QSet<QString> collectedIds;
        int currentAccountIdx = 0;
        QString cursor;
        bool hasMore = true;
        int consecutiveEmpty = 0;
        const int MAX_EMPTY = 5;

        // Period filter
        QDateTime sinceDate, untilDate;
        if (mode == "period") {
            QString since = config["since"].toString();
            QString until = config["until"].toString();
            if (!since.isEmpty()) sinceDate = QDateTime::fromString(since + "T00:00:00", Qt::ISODate);
            if (!until.isEmpty()) untilDate = QDateTime::fromString(until + "T23:59:59", Qt::ISODate);
            if (sinceDate.isValid() || untilDate.isValid())
                m_backend->log(QString("기간: %1 ~ %2").arg(since, until), "info", "twitter");
        }

        while (isRunning && hasMore) {
            auto [tweets, nextCursor] = getUserTweets(userId, cursor);

            if (nextCursor == "RATE_LIMITED") {
                handleRateLimit(accounts, currentAccountIdx, isRunning);
                continue;
            }
            if (nextCursor == "ERROR") {
                m_backend->log("API 오류 — 5초 후 재시도", "warning", "twitter");
                for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                continue;
            }

            if (tweets.isEmpty()) {
                consecutiveEmpty++;
                if (consecutiveEmpty >= MAX_EMPTY) {
                    m_backend->log(QString("빈 응답 %1회 연속, 종료").arg(MAX_EMPTY), "warning", "twitter");
                    break;
                }
                m_backend->log(QString("빈 응답, 재시도 %1/%2").arg(consecutiveEmpty).arg(MAX_EMPTY), "warning", "twitter");
                QThread::sleep(3);
                continue;
            }
            consecutiveEmpty = 0;

            bool pastRange = false;
            for (const auto &tweetVal : tweets) {
                if (!isRunning) break;
                QJsonObject tweet = tweetVal.toObject();
                QJsonObject legacy = tweet["legacy"].toObject();

                // Only include retweets
                QJsonObject rtResult;
                if (legacy.contains("retweeted_status_result")) {
                    rtResult = legacy["retweeted_status_result"].toObject()["result"].toObject();
                }
                if (rtResult.isEmpty() && tweet.contains("retweeted_status_result")) {
                    rtResult = tweet["retweeted_status_result"].toObject()["result"].toObject();
                }
                if (rtResult.isEmpty()) continue;  // Not a retweet, skip

                // Handle TweetWithVisibilityResults wrapper
                if (rtResult.contains("tweet")) rtResult = rtResult["tweet"].toObject();

                QString tweetId = legacy["id_str"].toString();
                if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();
                if (collectedIds.contains(tweetId)) continue;

                // Period filter (UserTweets returns reverse chronological)
                QString createdAt = legacy["created_at"].toString();
                QDateTime tweetDate = Common::parseISODate(createdAt);
                if (untilDate.isValid() && tweetDate > untilDate) continue;
                if (sinceDate.isValid() && tweetDate < sinceDate) { pastRange = true; break; }

                collectedIds.insert(tweetId);
                tweetCount++;

                QJsonObject rtLegacy = rtResult["legacy"].toObject();
                QJsonObject rtCore = rtResult["core"].toObject()["user_results"].toObject()["result"].toObject()["legacy"].toObject();
                QString rtScreenName = rtCore["screen_name"].toString();
                QString rtText = rtLegacy["full_text"].toString();
                QString rtId = rtLegacy["id_str"].toString();
                if (rtId.isEmpty()) rtId = rtResult["rest_id"].toString();

                m_backend->log(QString("[%1] RT @%2: %3...").arg(tweetCount).arg(rtScreenName, rtText.left(30)), "info", "twitter");
                m_backend->updateStats(tweetCount, mediaCount, "수집 중...", "twitter");

                // Download media from original tweet
                int newMedia = downloadTweetMedia(rtResult, repostsMediaDir, tweetCount);
                mediaCount += newMedia;

                // 캡쳐 (reposts 별도 폴더, 원글 기준 캡쳐)
                captureTweet(rtResult, userDir + "/captures/reposts", config);

                // Anipo-style batch progress log
                if (tweetCount % 10 == 0) {
                    m_backend->log(QString("트윗 %1개, 미디어 %2개 다운로드").arg(tweetCount).arg(mediaCount), "info", "twitter");
                }

                // Download RT user profile pics & banners (anipo style)
                // tweet = RT 트윗 전체 (RT한 사람 포함), rtResult = 원글 트윗
                {
                    QString profileDir = userDir + "/profiles";
                    // 원글 작성자 프로필 (rtResult에서 core.user_results)
                    downloadUserProfileMedia(rtResult, profileDir, "reposts");
                    // RT한 사람 프로필 (tweet에서 core.user_results)
                    downloadUserProfileMedia(tweet, profileDir, "reposts");
                }

                // ── complete 형식 호환 키 사용 (saveExcelStreaming 호환) ──
                QJsonObject data;
                data["id"] = tweetId;
                data["tweet_url"] = QString("https://x.com/%1/status/%2").arg(rtScreenName, rtId);
                data["text"] = QString("RT @%1: %2").arg(rtScreenName, rtText);
                data["language"] = rtLegacy["lang"].toString();
                data["type"] = "Retweet";

                // Author info (원글 작성자)
                QJsonObject rtUserResult = rtResult["core"].toObject()["user_results"].toObject()["result"].toObject();
                QJsonObject rtUserLeg = rtUserResult["legacy"].toObject();
                data["author_name"] = rtUserLeg["name"].toString();
                data["author_username"] = rtScreenName;

                data["bookmark_count"] = 0;
                data["favorite_count"] = rtLegacy["favorite_count"].toInt();
                data["retweet_count"] = rtLegacy["retweet_count"].toInt();
                data["retweeted"] = "True";
                data["reply_count"] = rtLegacy["reply_count"].toInt();
                data["quote_count"] = rtLegacy["quote_count"].toInt();
                data["view_count"] = rtResult["views"].toObject()["count"].toString();

                // 원본 트윗의 created_at
                QString origCreated = rtLegacy["created_at"].toString();
                QDateTime origDt = Common::parseISODate(origCreated);
                if (origDt.isValid()) {
                    origDt = origDt.toUTC().addSecs(9 * 3600);
                    data["created_at"] = origDt.toString("yyyy/MM/dd HH:mm");
                } else {
                    data["created_at"] = "";
                }

                // RT 시간 (리트윗 버튼을 누른 시각) = createdAt (타임라인 시각)
                QDateTime rpDt = Common::parseISODate(createdAt);
                if (rpDt.isValid()) {
                    rpDt = rpDt.toUTC().addSecs(9 * 3600);
                    data["retweet_time"] = rpDt.toString("yyyy/MM/dd HH:mm");
                } else {
                    data["retweet_time"] = "";
                }

                data["source"] = "";
                // Media types & URLs
                QJsonArray mediaArr = rtLegacy["extended_entities"].toObject()["media"].toArray();
                if (mediaArr.isEmpty()) mediaArr = rtLegacy["entities"].toObject()["media"].toArray();
                QStringList mediaTypes, mediaUrls;
                for (const auto &m : mediaArr) {
                    QJsonObject mo = m.toObject();
                    QString mt = mo["type"].toString();
                    if (!mt.isEmpty() && !mediaTypes.contains(mt)) mediaTypes << mt;
                    QString mu = mo["media_url_https"].toString();
                    if (!mu.isEmpty()) mediaUrls << mu;
                }
                data["media_type"] = mediaTypes.join(", ");
                data["media_urls"] = mediaUrls.join("\n");
                data["possibly_sensitive"] = rtLegacy.value("possibly_sensitive").toBool(false) ? "True": "False";
                data["hashtags"] = "";
                data["urls"] = "";
                data["conversation_id"] = "";
                data["in_reply_to"] = "";
                data["is_quote"] = "False";
                data["quoted_tweet_url"] = "";
                data["user_followers"] = QString::number(rtUserLeg["followers_count"].toInt());
                data["user_following"] = QString::number(rtUserLeg["friends_count"].toInt());
                data["user_verified"] = rtUserResult.value("is_blue_verified").toBool(false) ? "True": "False";
                data["user_profile_image"] = rtUserLeg["profile_image_url_https"].toString();
                collectedData.append(data);
            }

            if (pastRange) break;
            cursor = nextCursor;
            if (cursor.isEmpty()) hasMore = false;
            if (hasMore) { for (int w = 10; w > 0 && isRunning; --w) QThread::sleep(1); }
        }

        if (collectedData.count() > 0) {
            m_backend->log("Excel 저장...", "info", "twitter");
            saveExcelStreaming(userDir, target, collectedData, "reposts");
        }

        m_backend->log(QString("완료! 리포스트: %1, 미디어: %2").arg(tweetCount).arg(mediaCount), "success", "twitter");
        m_backend->updateStats(tweetCount, mediaCount, "완료", "twitter");
        stopDaemon();
        return;
    }

    // ── Thread Comments (TweetDetail API — 게시물에 달린 다른 사람 답글) ──
    if (type == "thread_comments") {
        m_backend->log("5. 게시물 댓글 수집 (다른 사람 답글)...", "info", "twitter");

        QString commentTempDir = config["tempDir"].toString();
        if (commentTempDir.isEmpty()) commentTempDir = userDir + "/.tmp";
        int commentCount = 0, mediaCount = 0;
        DiskJsonBuffer collectedData(commentTempDir, "tw_comments");
        QSet<QString> collectedIds;
        int currentAccountIdx = 0;

        // Step 1: 유저의 모든 트윗 ID 수집 (reply_count > 0인 것만)
        m_backend->log("Phase 1: 댓글 있는 포스트 목록 수집...", "info", "twitter");
        QList<QPair<QString, int>> tweetIdsWithReplies;  // {tweetId, replyCount}
        {
            QString cursor;
            bool hasMore = true;
            int consecutiveEmpty = 0;
            while (isRunning && hasMore) {
                auto [tweets, nextCursor] = getUserTweets(userId, cursor);
                if (nextCursor == "RATE_LIMITED") {
                    handleRateLimit(accounts, currentAccountIdx, isRunning);
                    continue;
                }
                if (nextCursor == "ERROR") {
                    m_backend->log("API 오류 — 5초 후 재시도", "warning", "twitter");
                    for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                    continue;
                }
                if (tweets.isEmpty()) {
                    consecutiveEmpty++;
                    if (consecutiveEmpty >= 5) break;
                    QThread::sleep(3);
                    continue;
                }
                consecutiveEmpty = 0;

                for (const auto &tweetVal : tweets) {
                    QJsonObject tweet = tweetVal.toObject();
                    QJsonObject legacy = tweet["legacy"].toObject();
                    // Skip retweets
                    if (legacy.contains("retweeted_status_result") || tweet.contains("retweeted_status_result"))
                        continue;
                    QString tid = legacy["id_str"].toString();
                    if (tid.isEmpty()) tid = tweet["rest_id"].toString();
                    int replyCount = legacy["reply_count"].toInt();
                    if (replyCount > 0) {
                        tweetIdsWithReplies.append({tid, replyCount});
                    }
                }

                cursor = nextCursor;
                if (cursor.isEmpty()) hasMore = false;
                if (hasMore) QThread::sleep(2);
            }
        }

        int totalPosts = tweetIdsWithReplies.size();
        int expectedReplies = 0;
        for (const auto &p : tweetIdsWithReplies) expectedReplies += p.second;
        m_backend->log(QString("댓글 있는 포스트 %1개 (예상 댓글 %2개)").arg(totalPosts).arg(expectedReplies), "info", "twitter");

        // Step 2: 각 포스트의 TweetDetail → 답글 추출
        m_backend->log("Phase 2: 댓글 수집 중...", "info", "twitter");
        for (int postIdx = 0; postIdx < totalPosts && isRunning; ++postIdx) {
            const QString &focalId = tweetIdsWithReplies[postIdx].first;
            int expectedCount = tweetIdsWithReplies[postIdx].second;

            QString cursor;
            bool hasMore = true;
            int pageReplies = 0;

            while (isRunning && hasMore) {
                auto [replies, nextCursor] = getTweetDetail(focalId, cursor);

                if (nextCursor == "RATE_LIMITED") {
                    handleRateLimit(accounts, currentAccountIdx, isRunning);
                    continue;
                }
                if (nextCursor == "ERROR") {
                    m_backend->log("API 오류 — 5초 후 재시도", "warning", "twitter");
                    for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                    continue;
                }

                if (replies.isEmpty()) break;

                for (const auto &replyVal : replies) {
                    if (!isRunning) break;
                    QJsonObject tweet = replyVal.toObject();
                    QJsonObject legacy = tweet["legacy"].toObject();
                    QString replyId = legacy["id_str"].toString();
                    if (replyId.isEmpty()) replyId = tweet["rest_id"].toString();
                    if (replyId.isEmpty() || collectedIds.contains(replyId)) continue;

                    collectedIds.insert(replyId);
                    commentCount++;
                    pageReplies++;

                    // Author info
                    QJsonObject authorResult = tweet["core"].toObject()["user_results"].toObject()["result"].toObject();
                    QJsonObject authorLeg = authorResult["legacy"].toObject();
                    QString authorName = authorLeg["name"].toString();
                    QString authorUsername = authorLeg["screen_name"].toString();
                    QString replyText = legacy["full_text"].toString();
                    QString replyUrl = QString("https://x.com/%1/status/%2").arg(authorUsername, replyId);
                    QString parentUrl = QString("https://x.com/%1/status/%2").arg(target, focalId);

                    m_backend->log(QString("[%1] @%2: %3...").arg(commentCount).arg(authorUsername, replyText.left(40)), "info", "twitter");
                    m_backend->updateStats(commentCount, mediaCount, QString("댓글 수집 (%1/%2 포스트)").arg(postIdx + 1).arg(totalPosts), "twitter");

                    // Download media
                    int newMediaCmt = 0;
                    if (m_downloadMedia) {
                        QString commentMediaDir = mediaDir + "/comments";
                        QDir().mkpath(commentMediaDir);
                        newMediaCmt = downloadTweetMedia(tweet, commentMediaDir, commentCount);
                        mediaCount += newMediaCmt;
                    }

                    // 캡쳐 (comments 별도 폴더)
                    captureTweet(tweet, userDir + "/captures/comments", config);

                    // Download commenter profile
                    downloadUserProfileMedia(tweet, userDir + "/profiles", "comments");

                    // Build data row
                    QJsonObject data;
                    data["id"] = replyId;
                    data["tweet_url"] = replyUrl;
                    data["text"] = replyText;
                    data["language"] = legacy["lang"].toString();
                    data["type"] = "Reply";
                    data["author_name"] = authorName;
                    data["author_username"] = authorUsername;
                    data["bookmark_count"] = legacy["bookmark_count"].toInt();
                    data["favorite_count"] = legacy["favorite_count"].toInt();
                    data["retweet_count"] = legacy["retweet_count"].toInt();
                    data["retweeted"] = "False";
                    data["reply_count"] = legacy["reply_count"].toInt();
                    data["quote_count"] = legacy["quote_count"].toInt();
                    data["view_count"] = tweet["views"].toObject()["count"].toString();

                    QString createdAt = legacy["created_at"].toString();
                    QDateTime tweetDt = Common::parseISODate(createdAt);
                    if (tweetDt.isValid()) {
                        tweetDt = tweetDt.toUTC().addSecs(9 * 3600);
                        data["created_at"] = tweetDt.toString("yyyy/MM/dd HH:mm");
                    } else {
                        data["created_at"] = createdAt;
                    }
                    data["retweet_time"] = "";

                    QString source = legacy["source"].toString();
                    if (source.contains(">")) {
                        int start = source.indexOf('>') + 1;
                        int end = source.indexOf('<', start);
                        if (end > start) source = source.mid(start, end - start);
                    }
                    data["source"] = source;

                    QJsonArray hashtagArr = legacy["entities"].toObject()["hashtags"].toArray();
                    QStringList hashtags;
                    for (const auto &h : hashtagArr) hashtags << h.toObject()["text"].toString();
                    data["hashtags"] = hashtags.isEmpty() ? "": hashtags.join(", ");

                    QJsonArray urlArr = legacy["entities"].toObject()["urls"].toArray();
                    QStringList urls;
                    for (const auto &u : urlArr) urls << u.toObject()["expanded_url"].toString();
                    data["urls"] = urls.isEmpty() ? "": urls.join(", ");

                    // Media URLs
                    QJsonArray mediaArr = legacy["extended_entities"].toObject()["media"].toArray();
                    if (mediaArr.isEmpty()) mediaArr = legacy["entities"].toObject()["media"].toArray();
                    QStringList mediaUrls;
                    for (const auto &m : mediaArr) {
                        QString mUrl = m.toObject()["media_url_https"].toString();
                        if (!mUrl.isEmpty()) mediaUrls << mUrl;
                    }
                    data["media_urls"] = mediaUrls.isEmpty() ? "": mediaUrls.join(", ");

                    data["conversation_id"] = legacy["conversation_id_str"].toString();
                    data["in_reply_to"] = legacy["in_reply_to_screen_name"].toString();
                    data["is_quote"] = "False";
                    data["quoted_tweet_url"] = "";
                    data["parent_tweet_url"] = parentUrl;

                    collectedData.append(data);
                }

                cursor = nextCursor;
                if (cursor.isEmpty()) hasMore = false;
                if (hasMore) QThread::sleep(1);
            }

            // Progress log every 10 posts
            if ((postIdx + 1) % 10 == 0 || postIdx == totalPosts - 1) {
                m_backend->log(QString("%1/%2 포스트 처리, 댓글 %3개").arg(postIdx + 1).arg(totalPosts).arg(commentCount), "info", "twitter");
            }

            // Intermediate save every 500 comments
            if (commentCount > 0 && commentCount % 500 < 20) {
                saveExcelStreaming(userDir, target, collectedData, "comments");
                m_backend->log(QString("중간 저장: %1건").arg(commentCount), "info", "twitter");
            }

            QThread::sleep(1);  // Gentle rate: 1 post/sec
        }

        if (collectedData.count() > 0) {
            saveExcelStreaming(userDir, target, collectedData, "comments");
            m_backend->log(QString("Excel 저장: %1_comments.xlsx").arg(target), "info", "twitter");
        }

        m_backend->log(QString("완료! 댓글: %1, 미디어: %2 (포스트 %3개 처리)").arg(commentCount).arg(mediaCount).arg(totalPosts), "success", "twitter");
        m_backend->updateStats(commentCount, mediaCount, "완료", "twitter");
        stopDaemon();
        return;
    }

    // ── Tweets / Media / Replies (UserTweets API + SearchTimeline fallback) ──
    QString excelSuffix = "tweets";
    QString logLabel = "트윗";
    bool filterMediaOnly = false;
    bool filterRepliesOnly = false;

    // Period filter
    QDateTime sinceDate, untilDate;
    if (mode == "period") {
        QString since = config["since"].toString();
        QString until = config["until"].toString();
        if (!since.isEmpty()) sinceDate = QDateTime::fromString(since + "T00:00:00", Qt::ISODate);
        if (!until.isEmpty()) untilDate = QDateTime::fromString(until + "T23:59:59", Qt::ISODate);
        if (sinceDate.isValid() || untilDate.isValid())
            m_backend->log(QString("기간: %1 ~ %2").arg(since, until), "info", "twitter");
    }

    if (type == "media") {
        filterMediaOnly = true;
        excelSuffix = "media";
        logLabel = "미디어";
        m_backend->log("미디어만 수집...", "info", "twitter");
    } else if (type == "replies") {
        excelSuffix = "tweets";  // replies도 tweets Excel에 합산 (같은 파일)
        logLabel = "답글";
        m_backend->log("3. 답글 검색 (기존 트윗에 추가)...", "info", "twitter");
    } else if (type == "tweets_api") {
        excelSuffix = "tweets";  // tweets_api도 같은 Excel에 합산
        logLabel = "트윗 보충";
        m_backend->log("2. UserTweets API 보충 (기존 트윗에 추가)...", "info", "twitter");
    } else {
        m_backend->log("1. 트윗 수집 (아니포 검색)...", "info", "twitter");
    }

    QString mainTempDir = config["tempDir"].toString();
    if (mainTempDir.isEmpty()) mainTempDir = userDir + "/.tmp";
    int tweetCount = 0;
    int mediaCount = 0;
    DiskJsonBuffer collectedData(mainTempDir, "tw_"+ excelSuffix);
    QSet<QString> collectedIds;
    int currentAccountIdx = 0;

    // ── 아니포 방식: 기존 Excel에서 ID 로드 + progress 파일에서 진행 상태 로드 ──
    // 아니포 호환: tweets → complete
    QString excelFileSuffix = (excelSuffix == "tweets") ? "complete": excelSuffix;
    QString excelPath = userDir + "/"+ target + "_"+ excelFileSuffix + ".xlsx";
    // fallback: 기존 _tweets.xlsx도 확인
    if (!QFile::exists(excelPath) && excelSuffix == "tweets") {
        QString oldPath = userDir + "/"+ target + "_tweets.xlsx";
        if (QFile::exists(oldPath)) excelPath = oldPath;
    }
    QString progressPath = userDir + "/.progress_"+ excelSuffix + ".json";
    QString resumeOldestDate;  // progress: 가장 오래된 트윗 날짜 (과거 이어서용 since/until)
    QString resumeNewestDate;  // progress: 가장 새로운 트윗 날짜 (현재 이어서용)
    QString resumeNewestId;    // progress: 가장 새로운 트윗 ID (since_id 기반 정확 분리)
    // ★ saveProgress lambda가 캡쳐하므로 lambda 정의 전 미리 선언
    QDateTime newestTweetDate;
    // ★ resume 모드 — UI dropdown 또는 config["resumeMode"]
    //   "past":   resumeOldestDate 이전만 검색 (마지막 다운한 것보다 더 과거 트윗)
    //   "future": resumeNewestDate/Id 이후만 검색 (마지막 다운 후 새로 올라온 트윗)
    //   "both" / 기본: 양방향 (과거로 backtrack + 현재 새 트윗 둘 다)
    QString resumeMode = config["resumeMode"].toString();
    if (resumeMode.isEmpty()) resumeMode = "both";

    if (QFile::exists(excelPath)) {
        QXlsx::Document doc(excelPath);
        int lastRow = doc.dimension().lastRow();
        for (int r = 2; r <= lastRow; ++r) {  // 1행은 헤더
            QVariant val = doc.read(r, 1);  // 1열 = id
            QString id = val.toString().trimmed();
            if (!id.isEmpty()) collectedIds.insert(id);
        }
        if (!collectedIds.isEmpty()) {
            tweetCount = collectedIds.size();
            m_backend->log(QString("기존 파일: %1개 수집됨 → 이어서 수집").arg(tweetCount), "info", "twitter");
        }
    }

    // ★ progress 파일은 m_saveProgress(=이어서 수집 체크박스)가 true일 때만 읽음.
    //   체크 해제 시 사용자가 처음부터 다시 받기를 원하는 의도 → progress.json 무시.
    //   (저장은 항상 — 다음 실행 때 사용자가 체크하면 사용 가능)
    if (m_saveProgress && QFile::exists(progressPath)) {
        QFile pf(progressPath);
        if (pf.open(QIODevice::ReadOnly)) {
            QJsonObject prog = QJsonDocument::fromJson(pf.readAll()).object();
            pf.close();
            resumeOldestDate = prog["oldestDate"].toString();
            resumeNewestDate = prog["newestDate"].toString();
            resumeNewestId = prog["newestId"].toString();
            mediaCount = prog["mediaCount"].toInt();
            // resume 모드별 안내 — 어느 방향으로 이어서 가는지 명확히
            if (resumeMode == "past" && !resumeOldestDate.isEmpty()) {
                m_backend->log(QString("✓ [과거 이어서] %1 이전 트윗만 수집").arg(resumeOldestDate),
                               "info", "twitter");
            } else if (resumeMode == "future" && (!resumeNewestDate.isEmpty() || !resumeNewestId.isEmpty())) {
                m_backend->log(QString("✓ [현재 이어서] %1 이후 새 트윗만 수집")
                                   .arg(resumeNewestDate.isEmpty() ? resumeNewestId : resumeNewestDate),
                               "info", "twitter");
            } else if (resumeMode == "both" && !resumeOldestDate.isEmpty()) {
                m_backend->log(QString("✓ [양방향 이어서] %1 이전 + 새 트윗 둘 다").arg(resumeOldestDate),
                               "info", "twitter");
            }
        }
    }

    // Helper: progress 파일 저장 — oldest/newest 둘 다 누적
    //   기존 파일에 이미 newestDate가 있으면 그보다 새로운 것만 갱신 (newer wins)
    //   기존 oldestDate가 있으면 그보다 오래된 것만 갱신 (older wins)
    auto saveProgress = [&](const QString &oldestDate) {
        QJsonObject prev;
        if (QFile::exists(progressPath)) {
            QFile pfr(progressPath);
            if (pfr.open(QIODevice::ReadOnly)) {
                prev = QJsonDocument::fromJson(pfr.readAll()).object();
                pfr.close();
            }
        }
        // oldestDate: older wins
        QString outOldest = oldestDate;
        QString prevOldest = prev["oldestDate"].toString();
        if (!prevOldest.isEmpty()) {
            QDate prevD = QDate::fromString(prevOldest, "yyyy-MM-dd");
            QDate curD  = QDate::fromString(outOldest, "yyyy-MM-dd");
            if (prevD.isValid() && curD.isValid() && prevD < curD) outOldest = prevOldest;
        }
        // newestDate/Id: newer wins (m_newestTweetId 기준 — 이번 세션의 최신)
        QString outNewestId = m_newestTweetId;
        QString prevNewestId = prev["newestId"].toString();
        if (!prevNewestId.isEmpty()
            && (outNewestId.isEmpty() || prevNewestId.toLongLong() > outNewestId.toLongLong())) {
            outNewestId = prevNewestId;
        }
        // newestDate: newer wins
        QString outNewestDate = prev["newestDate"].toString();
        if (newestTweetDate.isValid()) {
            QString curND = newestTweetDate.toString("yyyy-MM-dd");
            QDate prevD = QDate::fromString(outNewestDate, "yyyy-MM-dd");
            QDate curD  = QDate::fromString(curND, "yyyy-MM-dd");
            if (!prevD.isValid() || (curD.isValid() && curD > prevD)) outNewestDate = curND;
        }

        QJsonObject prog;
        prog["oldestDate"] = outOldest;
        prog["newestDate"] = outNewestDate;
        prog["newestId"]   = outNewestId;
        prog["tweetCount"] = tweetCount;
        prog["mediaCount"] = mediaCount;
        prog["timestamp"]  = QDateTime::currentDateTime().toString(Qt::ISODate);
        QFile pf(progressPath);
        if (pf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            pf.write(QJsonDocument(prog).toJson(QJsonDocument::Compact));
            pf.close();
        }
    };

    m_backend->log("수집 시작...", "info", "twitter");

    // 타겟 프로필 사진/배너 저장 — 통일된 구조: profiles/target/{이름(@handle)}/
    {
        QString dateTag = QDate::currentDate().toString("yyyyMMdd");
        QString tDispName = userName;
        tDispName.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
        tDispName = tDispName.trimmed().left(40);
        QString tPrefix = !tDispName.isEmpty() ? QString("%1(@%2)").arg(tDispName, target) : target;
        QString tDirN = tPrefix;
        tDirN.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
        QString profileDateDir = userDir + "/profiles/target/"+ tDirN;
        QDir().mkpath(profileDateDir);

        // Profile image — try _400x400, fallback
        QString profileImgUrl = userLegacy["profile_image_url_https"].toString();
        if (!profileImgUrl.isEmpty()) {
            QString hiResUrl = profileImgUrl;
            hiResUrl.replace("_normal", "_400x400");
            QString profilePath = profileDateDir + "/profile_"+ dateTag + ".jpg";
            if (!QFile::exists(profilePath)) {
                bool ok = downloadMedia(hiResUrl, profilePath);
                if (!ok) ok = downloadMedia(profileImgUrl.replace("_normal", ""), profilePath);
                if (!ok) downloadMedia(profileImgUrl, profilePath);
            }
        }

        // Banner image
        QString bannerUrl = userLegacy["profile_banner_url"].toString();
        if (!bannerUrl.isEmpty()) {
            QString bannerPath = profileDateDir + "/banner_"+ dateTag + ".jpg";
            if (!QFile::exists(bannerPath)) {
                bool ok = downloadMedia(bannerUrl + "/1500x500", bannerPath);
                if (!ok) downloadMedia(bannerUrl, bannerPath);
            }
        }
    }

    // Lambda: process a batch of tweets (shared by UserTweets and SearchTimeline)
    QDateTime oldestTweetDate; // Track oldest tweet for SearchTimeline fallback
    // newestTweetDate는 위 progress 영역에 미리 선언됨 (saveProgress lambda 캡쳐용)
    auto processTweetBatch = [&](const QJsonArray &tweets) -> bool {
        bool pastRange = false;
        for (const auto &tweetVal : tweets) {
            if (!isRunning) break;

            QJsonObject tweet = tweetVal.toObject();
            QJsonObject legacy = tweet["legacy"].toObject();
            QString tweetId = legacy["id_str"].toString();
            if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();

            // Dedup
            if (collectedIds.contains(tweetId)) continue;

            // Period filter
            QString createdAt = legacy["created_at"].toString();
            QDateTime tweetDate = Common::parseISODate(createdAt);
            if (untilDate.isValid() && tweetDate > untilDate) continue;
            if (sinceDate.isValid() && tweetDate < sinceDate) { pastRange = true; break; }

            // Track oldest tweet date for SearchTimeline fallback
            if (!oldestTweetDate.isValid() || tweetDate < oldestTweetDate) {
                oldestTweetDate = tweetDate;
            }

            // Check if retweet
            bool isRetweet = false;
            QJsonObject rtResult;
            if (legacy.contains("retweeted_status_result")) {
                rtResult = legacy["retweeted_status_result"].toObject()["result"].toObject();
                isRetweet = true;
            }
            if (rtResult.isEmpty() && tweet.contains("retweeted_status_result")) {
                rtResult = tweet["retweeted_status_result"].toObject()["result"].toObject();
                isRetweet = !rtResult.isEmpty();
            }
            if (!rtResult.isEmpty() && rtResult.contains("tweet"))
                rtResult = rtResult["tweet"].toObject();

            // Check if reply
            bool isReply = !legacy["in_reply_to_status_id_str"].toString().isEmpty();

            // Filter: media only
            if (filterMediaOnly) {
                QJsonObject entities = legacy["extended_entities"].toObject();
                QJsonArray mediaArr = entities["media"].toArray();
                if (mediaArr.isEmpty()) {
                    entities = legacy["entities"].toObject();
                    mediaArr = entities["media"].toArray();
                }
                if (mediaArr.isEmpty() && isRetweet && !rtResult.isEmpty()) {
                    QJsonObject rtLeg = rtResult["legacy"].toObject();
                    mediaArr = rtLeg["extended_entities"].toObject()["media"].toArray();
                    if (mediaArr.isEmpty()) mediaArr = rtLeg["entities"].toObject()["media"].toArray();
                }
                if (mediaArr.isEmpty()) continue;
            }

            // Filter: replies only
            if (filterRepliesOnly && !isReply) continue;

            collectedIds.insert(tweetId);
            // Track newest tweet ID + date (for 새트윗확인 + future resume)
            if (m_newestTweetId.isEmpty() || tweetId.toLongLong() > m_newestTweetId.toLongLong()) {
                m_newestTweetId = tweetId;
                if (tweetDate.isValid()) newestTweetDate = tweetDate;
            }
            // ID는 Excel 저장 시 자동 보존됨 (아니포 방식)
            tweetCount++;

            // Helper: extract screen_name from user result object (handles missing legacy.screen_name)
            auto getScreenName = [](const QJsonObject &userResult) -> QString {
                // Try 1: legacy.screen_name (standard)
                QString sn = userResult["legacy"].toObject()["screen_name"].toString();
                if (!sn.isEmpty()) return sn;
                // Try 2: top-level screen_name
                sn = userResult["screen_name"].toString();
                if (!sn.isEmpty()) return sn;
                // Try 3: from rest_id
                return userResult["rest_id"].toString();
            };

            // Get tweet author info
            QJsonObject authorResult = tweet["core"].toObject()["user_results"].toObject()["result"].toObject();
            QString authorName = getScreenName(authorResult);
            if (authorName.isEmpty()) authorName = target;

            QString tweetText = legacy["full_text"].toString();
            // 긴 트윗 (NoteTweet, Premium 최대 25000자) — note_tweet.text 우선
            QJsonObject noteTweet = tweet["note_tweet"].toObject();
            if (!noteTweet.isEmpty()) {
                QString longText = noteTweet["note_tweet_results"].toObject()
                    ["result"].toObject()["text"].toString();
                if (!longText.isEmpty() && longText.length() > tweetText.length()) {
                    tweetText = longText;
                }
            }
            // URL에는 screen_name 사용 (rest_id 같은 숫자면 target으로 대체)
            QString urlAuthor = authorName;
            bool looksLikeId = true;
            for (QChar c : urlAuthor) { if (!c.isDigit()) { looksLikeId = false; break; } }
            if (looksLikeId || urlAuthor.isEmpty()) urlAuthor = target;
            QString tweetUrl = QString("https://x.com/%1/status/%2").arg(urlAuthor, tweetId);

            if (isRetweet && !rtResult.isEmpty()) {
                QJsonObject rtLegacy = rtResult["legacy"].toObject();
                QJsonObject rtUserResult = rtResult["core"].toObject()["user_results"].toObject()["result"].toObject();
                QString rtAuthor = getScreenName(rtUserResult);
                // RT의 원작자 screen_name이 없으면 full_text에서 추출 시도
                if (rtAuthor.isEmpty()) {
                    // "RT @username: ..."패턴에서 추출
                    QString ft = legacy["full_text"].toString();
                    QRegularExpression rx("^RT @(\\w+):");
                    auto m = rx.match(ft);
                    if (m.hasMatch()) rtAuthor = m.captured(1);
                }
                if (rtAuthor.isEmpty()) rtAuthor = "?";
                QString rtId = rtLegacy["id_str"].toString();
                if (rtId.isEmpty()) rtId = rtResult["rest_id"].toString();
                // 원본 RT에도 긴 트윗이 있으면 적용
                QString rtFull = rtLegacy["full_text"].toString();
                QJsonObject rtNote = rtResult["note_tweet"].toObject();
                if (!rtNote.isEmpty()) {
                    QString rtLong = rtNote["note_tweet_results"].toObject()
                        ["result"].toObject()["text"].toString();
                    if (!rtLong.isEmpty() && rtLong.length() > rtFull.length()) rtFull = rtLong;
                }
                tweetText = QString("RT @%1: %2").arg(rtAuthor, rtFull);
                tweetUrl = QString("https://x.com/%1/status/%2").arg(rtAuthor, rtId);
                m_backend->log(QString("[%1] RT @%2: %3...").arg(tweetCount).arg(rtAuthor, rtLegacy["full_text"].toString().left(30)), "info", "twitter");
            } else {
                m_backend->log(QString("[%1] %2...").arg(tweetCount).arg(tweetText.left(40)), "info", "twitter");
            }
            m_backend->updateStats(tweetCount, mediaCount, "수집 중...", "twitter");

            // Download media (타입별 폴더 분리: tweets/replies/reposts)
            QString tweetMediaDir;
            if (isRetweet) tweetMediaDir = mediaDir + "/reposts";
            else if (isReply) tweetMediaDir = mediaDir + "/replies";
            else tweetMediaDir = mediaDir + "/tweets";
            QDir().mkpath(tweetMediaDir);
            int newMedia = downloadTweetMedia(tweet, tweetMediaDir, tweetCount);
            mediaCount += newMedia;

            // 진행 로그
            if (tweetCount > 0 && tweetCount % 100 == 0) {
                m_backend->log(QString("진행: %1개, 미디어 %2개").arg(tweetCount).arg(mediaCount), "info", "twitter");
            } else if (tweetCount % 10 == 0) {
                m_backend->log(QString("트윗 %1개, 미디어 %2개").arg(tweetCount).arg(mediaCount), "info", "twitter");
            }

            // 캡쳐: realCapture=true이면 SingleFile+CDP 실페이지, false면 합성 카드.
            // 통합 헬퍼 captureTweet()으로 위임 — 모든 type 분기 일관 처리.
            // ★ realCaptureMode — "off": skip / "media": 미디어 있을 때만 / "all": 모두 (기본)
            {
                QString capMode = config["realCaptureMode"].toString("all");
                if (capMode != "off" && !(capMode == "media" && newMedia == 0)) {
                    captureTweet(tweet, userDir + "/captures", config);
                }
            }

            // Download RT/quoted user profile pics & banners (anipo style)
            QString profileDir = userDir + "/profiles";
            downloadUserProfileMedia(tweet, profileDir);

            // Collect data — anipo/complete 형식 (21컬럼)
            QJsonObject data;
            data["id"] = tweetId;
            data["tweet_url"] = tweetUrl;
            data["text"] = tweetText;
            data["language"] = legacy["lang"].toString();

            // type: Tweet, Retweet, Reply, Quote Tweet, Poll (anipo 호환)
            bool isQuoteTweet = tweet.contains("quoted_status_result") ||
                                legacy.value("is_quote_status").toBool(false);
            // 투표 카드: card.legacy.name == "poll*choice*"패턴
            QJsonObject cardObj = tweet["card"].toObject();
            QString cardName = cardObj["legacy"].toObject()["name"].toString();
            bool hasPoll = cardName.startsWith("poll");
            // 고정 트윗: 최상위에 pinned = true 마킹
            bool isPinned = tweet["pinned"].toBool(false);
            QString tweetType = "Tweet";
            if (isRetweet) tweetType = "Retweet";
            else if (isQuoteTweet) tweetType = "Quote Tweet";
            else if (isReply) tweetType = "Reply";
            if (hasPoll) tweetType = "Poll";
            if (isPinned) tweetType = ""+ tweetType;
            data["type"] = tweetType;

            // 투표 데이터를 text 뒤에 JSON 형태로 첨부 (분석/기록용)
            if (hasPoll) {
                QJsonArray bindings = cardObj["legacy"].toObject()["binding_values"].toArray();
                QMap<QString, QString> bmap;
                for (const auto &bv : bindings) {
                    QJsonObject b = bv.toObject();
                    QString k = b["key"].toString();
                    QString v = b["value"].toObject()["string_value"].toString();
                    if (!v.isEmpty()) bmap[k] = v;
                }
                QStringList pollLines;
                for (int i = 1; i <= 4; ++i) {
                    QString labelKey = QString("choice%1_label").arg(i);
                    QString countKey = QString("choice%1_count").arg(i);
                    if (bmap.contains(labelKey)) {
                        pollLines << QString("%1. %2 — %3표")
                            .arg(i).arg(bmap[labelKey], bmap.value(countKey, "0"));
                    }
                }
                QString endTime = bmap.value("end_datetime_utc");
                QString state = bmap.value("counts_are_final") == "true"? "종료": "진행중";
                QString pollSummary = QString("\n[투표 %1] %2").arg(state, pollLines.join("/ "));
                if (!endTime.isEmpty()) pollSummary += QString("(마감 %1)").arg(endTime);
                tweetText += pollSummary;
                data["text"] = tweetText;
            }

            // Author info — 여러 경로에서 시도 (GraphQL 구조 변동 대응)
            {
                QJsonObject userResult = tweet["core"].toObject()["user_results"].toObject()["result"].toObject();
                QJsonObject userLeg = userResult["legacy"].toObject();
                QString aName = userLeg["name"].toString();
                QString aUsername = userLeg["screen_name"].toString();
                // fallback: userResult 직접
                if (aName.isEmpty()) aName = userResult["name"].toString();
                if (aUsername.isEmpty()) aUsername = userResult["screen_name"].toString();
                // fallback: authorResult (이미 위에서 추출)
                if (aUsername.isEmpty()) aUsername = authorName;
                data["author_name"] = aName;
                data["author_username"] = aUsername;
            }

            data["bookmark_count"] = legacy["bookmark_count"].toInt();
            data["favorite_count"] = legacy["favorite_count"].toInt();
            data["retweet_count"] = legacy["retweet_count"].toInt();
            data["retweeted"] = isRetweet ? "True": "False";
            data["reply_count"] = legacy["reply_count"].toInt();
            data["quote_count"] = legacy["quote_count"].toInt();

            QJsonObject views = tweet["views"].toObject();
            data["view_count"] = views["count"].toString();

            // 날짜: YYYY/MM/DD HH:MM 형식 (JST)
            QDateTime tweetDt = Common::parseISODate(createdAt);
            if (tweetDt.isValid()) {
                tweetDt = tweetDt.toUTC().addSecs(9 * 3600);
                data["created_at"] = tweetDt.toString("yyyy/MM/dd HH:mm");
            } else {
                data["created_at"] = createdAt;
            }

            // 리트윗 시간 처리 (아니포 호환):
            //   created_at = 원본 트윗 작성 시각
            //   retweet_time = RT 버튼을 누른 시각
            if (isRetweet) {
                // 현재 created_at은 RT 타임라인의 시각 → retweet_time으로 이동
                data["retweet_time"] = data["created_at"].toString();
                // 원본 트윗의 created_at으로 교체
                if (!rtResult.isEmpty()) {
                    QString origCreated = rtResult["legacy"].toObject()["created_at"].toString();
                    QDateTime origDt = Common::parseISODate(origCreated);
                    if (origDt.isValid()) {
                        origDt = origDt.toUTC().addSecs(9 * 3600);
                        data["created_at"] = origDt.toString("yyyy/MM/dd HH:mm");
                    }
                } else {
                    // rtResult 없으면 created_at 구분 불가 → retweet_time만 표시
                    // created_at은 RT시각 그대로 (원본 시각을 모름)
                    data["created_at"] = "";
                }
            }

            // 소스 (앱) — tweet 또는 legacy 양쪽에서 찾기
            QString source = tweet["source"].toString();
            if (source.isEmpty()) source = legacy["source"].toString();
            if (source.contains(">")) {
                int start = source.indexOf('>') + 1;
                int end = source.indexOf('<', start);
                if (end > start) source = source.mid(start, end - start);
            }
            data["source"] = source;

            // 해시태그
            QJsonArray hashtagArr = legacy["entities"].toObject()["hashtags"].toArray();
            QStringList hashtags;
            for (const auto &h : hashtagArr) hashtags << h.toObject()["text"].toString();  // anipo와 동일: # 없이
            data["hashtags"] = hashtags.isEmpty() ? "": hashtags.join(", ");

            // URLs
            QJsonArray urlArr = legacy["entities"].toObject()["urls"].toArray();
            QStringList urls;
            for (const auto &u : urlArr) urls << u.toObject()["expanded_url"].toString();
            data["urls"] = urls.isEmpty() ? "": urls.join(", ");

            // 미디어 타입 + URLs (본문 + RT 원본 모두 확인)
            QJsonArray mediaArr = legacy["extended_entities"].toObject()["media"].toArray();
            if (mediaArr.isEmpty()) mediaArr = legacy["entities"].toObject()["media"].toArray();
            // RT의 경우 원본 트윗의 미디어도 확인
            if (mediaArr.isEmpty() && isRetweet && !rtResult.isEmpty()) {
                QJsonObject rtLeg = rtResult["legacy"].toObject();
                mediaArr = rtLeg["extended_entities"].toObject()["media"].toArray();
                if (mediaArr.isEmpty()) mediaArr = rtLeg["entities"].toObject()["media"].toArray();
            }
            QStringList mediaTypes, mediaUrls;
            for (const auto &m : mediaArr) {
                QJsonObject mo = m.toObject();
                QString mt = mo["type"].toString();
                if (!mt.isEmpty() && !mediaTypes.contains(mt)) mediaTypes << mt;
                QString mu = mo["media_url_https"].toString();
                if (!mu.isEmpty()) mediaUrls << mu;
            }
            data["media_type"] = mediaTypes.join(", ");
            data["media_urls"] = mediaUrls.join("\n");  // anipo와 동일하게 줄바꿈 구분

            // ── 추가 필드 (Twitter에서 가져올 수 있는 모든 정보) ──
            // 대화 스레드 ID
            data["conversation_id"] = legacy["conversation_id_str"].toString();

            // 답글 대상
            QString replyTo = legacy["in_reply_to_screen_name"].toString();
            if (!replyTo.isEmpty()) {
                QString replyToId = legacy["in_reply_to_status_id_str"].toString();
                data["in_reply_to"] = QString("@%1 (%2)").arg(replyTo, replyToId);
            } else {
                data["in_reply_to"] = "";
            }

            // 인용 트윗
            bool isQuote = tweet.contains("quoted_status_result") ||
                           legacy.value("is_quote_status").toBool(false);
            data["is_quote"] = isQuote ? "True": "False";
            if (isQuote) {
                QJsonObject quotedResult = tweet["quoted_status_result"].toObject()["result"].toObject();
                if (quotedResult.contains("tweet")) quotedResult = quotedResult["tweet"].toObject();
                QString qId = quotedResult["rest_id"].toString();
                QJsonObject qLeg = quotedResult["legacy"].toObject();
                QJsonObject qUser = quotedResult["core"].toObject()["user_results"].toObject()["result"].toObject()["legacy"].toObject();
                QString qScreenName = qUser["screen_name"].toString();
                if (!qId.isEmpty() && !qScreenName.isEmpty()) {
                    data["quoted_tweet_url"] = QString("https://x.com/%1/status/%2").arg(qScreenName, qId);
                } else {
                    data["quoted_tweet_url"] = "";
                }
            } else {
                data["quoted_tweet_url"] = "";
            }

            // 민감 콘텐츠
            data["possibly_sensitive"] = legacy.value("possibly_sensitive").toBool(false) ? "True": "False";

            // 작성자 프로필 정보
            {
                QJsonObject uResult = tweet["core"].toObject()["user_results"].toObject()["result"].toObject();
                QJsonObject uLeg = uResult["legacy"].toObject();
                data["user_followers"] = QString::number(uLeg["followers_count"].toInt());
                data["user_following"] = QString::number(uLeg["friends_count"].toInt());
                data["user_verified"] = uResult.value("is_blue_verified").toBool(false) ? "True": "False";
                data["user_profile_image"] = uLeg["profile_image_url_https"].toString();
            }

            collectedData.append(data);

            // Intermediate save every 50 tweets — 메모리 부족 방지 + 크래시 시 데이터 보존
            if (tweetCount % 50 == 0) {
                m_backend->log(QString("저장 (%1개, 미디어 %2개)").arg(tweetCount).arg(mediaCount), "info", "twitter");
                saveExcelStreaming(userDir, target, collectedData, excelSuffix);
            }
        }
        return pastRange;
    };

    // ══════════════════════════════════════════════════════════════
    // 스레드 수집 모드 — 단일 트윗 URL에서 시작해서 동일 작성자의 연결된
    //  대화 전체를 수집 (선조 체인 + focal + 본인 답글 / 스레드)
    // ══════════════════════════════════════════════════════════════
    if (type == "thread") {
        if (threadFocalId.isEmpty()) {
            m_backend->log("스레드 URL 또는 트윗 ID를 입력해주세요", "error", "twitter");
            stopDaemon();
            return;
        }
        m_backend->log(QString("스레드 수집: focal=%1").arg(threadFocalId), "info", "twitter");

        QSet<QString> visitedIds;
        QQueue<QString> toFetch;
        toFetch.enqueue(threadFocalId);
        QJsonArray threadTweets;
        QString threadAuthorId;  // 스레드 작성자(= focal 작성자) 제한
        QString threadAuthorScreen;

        int maxNodes = 500;  // 안전장치

        while (!toFetch.isEmpty() && isRunning && visitedIds.size() < maxNodes) {
            QString curId = toFetch.dequeue();
            if (visitedIds.contains(curId)) continue;
            visitedIds.insert(curId);

            QString cursor;
            int cursorLoops = 0;
            do {
                if (!isRunning) break;
                auto [batch, nextCursor] = getTweetDetail(curId, cursor);

                if (nextCursor == "RATE_LIMITED") {
                    handleRateLimit(accounts, currentAccountIdx, isRunning);
                    continue;
                }
                if (nextCursor == "ERROR") {
                    m_backend->log("TweetDetail 에러 — 5초 후 재시도", "warning", "twitter");
                    for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                    break;
                }

                for (const auto &tv : batch) {
                    QJsonObject tw = tv.toObject();
                    QString id = tw["rest_id"].toString();
                    if (id.isEmpty()) id = tw["legacy"].toObject()["id_str"].toString();
                    if (id.isEmpty()) continue;

                    QJsonObject core = tw["core"].toObject()["user_results"].toObject()["result"].toObject();
                    QString authorId = core["rest_id"].toString();
                    QString authorScreen = core["legacy"].toObject()["screen_name"].toString();

                    // focal 작성자 결정
                    if (threadAuthorId.isEmpty() && id == curId) {
                        threadAuthorId = authorId;
                        threadAuthorScreen = authorScreen;
                    }

                    // 같은 작성자만 수집 (스레드 = 동일 작성자 chain)
                    if (!threadAuthorId.isEmpty() && authorId != threadAuthorId) continue;

                    if (!visitedIds.contains(id)) {
                        threadTweets.append(tw);
                    }

                    // 부모 트윗(선조 체인)도 큐에 추가
                    QString parentId = tw["legacy"].toObject()["in_reply_to_status_id_str"].toString();
                    QString parentAuthorId = tw["legacy"].toObject()["in_reply_to_user_id_str"].toString();
                    if (!parentId.isEmpty() && !visitedIds.contains(parentId)) {
                        // 선조는 스레드 작성자 본인에게만 (다른 사람에게 한 답글은 제외)
                        if (parentAuthorId.isEmpty() || threadAuthorId.isEmpty() ||
                            parentAuthorId == threadAuthorId) {
                            toFetch.enqueue(parentId);
                        }
                    }
                }

                // focal 트윗 자체도 포함 (getTweetDetail은 focal을 skip함 → 재시도)
                if (cursor.isEmpty() && curId == threadFocalId) {
                    // TweetDetail의 첫 응답에서 focal도 별도로 읽어오기 위해
                    // 직접 데몬 호출로 raw body를 가져온다
                    QJsonObject rawCmd;
                    rawCmd["action"] = "tweet_detail";
                    rawCmd["tweet_id"] = curId;
                    QJsonObject rawResp = sendDaemonCommand(rawCmd, 30000);
                    if (rawResp["status"].toInt() == 200) {
                        QJsonArray instructions = rawResp["body"].toObject()["data"].toObject()
                            ["threaded_conversation_with_injections_v2"].toObject()
                            ["instructions"].toArray();
                        for (const auto &inst : instructions) {
                            if (inst.toObject()["type"].toString() != "TimelineAddEntries") continue;
                            QJsonArray entries = inst.toObject()["entries"].toArray();
                            for (const auto &entry : entries) {
                                QJsonObject e = entry.toObject();
                                QString eid = e["entryId"].toString();
                                if (!eid.startsWith("tweet-")) continue;
                                QJsonObject tr = e["content"].toObject()
                                    ["itemContent"].toObject()
                                    ["tweet_results"].toObject()["result"].toObject();
                                if (tr.contains("tweet")) tr = tr["tweet"].toObject();
                                QString tid = tr["rest_id"].toString();
                                if (tid == curId && !visitedIds.contains(tid)) {
                                    QJsonObject fcore = tr["core"].toObject()
                                        ["user_results"].toObject()["result"].toObject();
                                    if (threadAuthorId.isEmpty()) {
                                        threadAuthorId = fcore["rest_id"].toString();
                                        threadAuthorScreen = fcore["legacy"].toObject()["screen_name"].toString();
                                    }
                                    threadTweets.append(tr);
                                    QString parentId2 = tr["legacy"].toObject()["in_reply_to_status_id_str"].toString();
                                    if (!parentId2.isEmpty() && !visitedIds.contains(parentId2))
                                        toFetch.enqueue(parentId2);
                                }
                            }
                        }
                    }
                }

                cursor = nextCursor;
                if (++cursorLoops > 30) break;  // 안전장치
            } while (!cursor.isEmpty() && isRunning);
        }

        m_backend->log(QString("스레드: %1개 트윗 수집됨 (@%2)")
            .arg(threadTweets.size()).arg(threadAuthorScreen), "success", "twitter");

        if (!threadTweets.isEmpty()) {
            // created_at 오름차순으로 정렬 (스레드는 시간순이 자연스러움)
            QList<QJsonObject> sortedList;
            for (const auto &v : threadTweets) sortedList.append(v.toObject());
            std::sort(sortedList.begin(), sortedList.end(), [](const QJsonObject &a, const QJsonObject &b) {
                QString ca = a["legacy"].toObject()["created_at"].toString();
                QString cb = b["legacy"].toObject()["created_at"].toString();
                return Common::parseISODate(ca) < Common::parseISODate(cb);
            });
            QJsonArray sortedArr;
            for (const auto &o : sortedList) sortedArr.append(o);
            excelSuffix = "thread_"+ threadFocalId;
            logLabel = "스레드";
            processTweetBatch(sortedArr);
        }

        // Excel 저장
        if (m_saveExcel && collectedData.count() > 0) {
            saveExcelStreaming(userDir, target, collectedData, excelSuffix);
            m_backend->log(QString("Excel 저장: %1_%2.xlsx").arg(target, excelSuffix), "info", "twitter");
        }

        m_backend->log(QString("완료! 트윗: %1, 미디어: %2").arg(tweetCount).arg(mediaCount),
                       "success", "twitter");
        m_backend->updateStats(tweetCount, mediaCount, "완료", "twitter");
        stopDaemon();
        return;
    }

    // ══════════════════════════════════════════════════════════════
    // 하이라이트 수집 — 유저가 프로필에 강조 표시한 트윗들
    // ══════════════════════════════════════════════════════════════
    if (type == "highlights") {
        excelSuffix = "highlights";
        logLabel = "하이라이트";
        m_backend->log("하이라이트 수집 시작...", "info", "twitter");

        QString hCursor;
        int hEmptyPages = 0;
        while (isRunning) {
            auto [batch, nextCursor] = getHighlights(userId, hCursor);
            if (nextCursor == "RATE_LIMITED") {
                handleRateLimit(accounts, currentAccountIdx, isRunning);
                continue;
            }
            if (nextCursor == "ERROR") {
                m_backend->log("Highlights 에러 — 5초 후 재시도", "warning", "twitter");
                for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                continue;
            }
            if (batch.isEmpty()) {
                if (++hEmptyPages >= 2) break;
            } else {
                hEmptyPages = 0;
            }
            processTweetBatch(batch);
            if (nextCursor.isEmpty()) break;
            hCursor = nextCursor;
            QThread::sleep(1);
        }

        if (m_saveExcel && collectedData.count() > 0) {
            saveExcelStreaming(userDir, target, collectedData, excelSuffix);
            m_backend->log(QString("Excel 저장: %1_%2.xlsx").arg(target, excelSuffix), "info", "twitter");
        }
        m_backend->log(QString("완료! 하이라이트: %1, 미디어: %2").arg(tweetCount).arg(mediaCount),
                       "success", "twitter");
        m_backend->updateStats(tweetCount, mediaCount, "완료", "twitter");
        stopDaemon();
        return;
    }

    // ══════════════════════════════════════════════════════════════
    // 좋아요/리트윗 한 유저 목록 — 특정 트윗 URL에 대해
    // ══════════════════════════════════════════════════════════════
    if (type == "favoriters"|| type == "retweeters") {
        // target 은 트윗 URL 또는 ID
        QString focalId;
        QRegularExpression rx(R"((?:x\.com|twitter\.com)/[A-Za-z0-9_]+/status/(\d+))");
        auto m = rx.match(config["target"].toString());
        if (m.hasMatch()) focalId = m.captured(1);
        else {
            QRegularExpression idRx("^\\d+$");
            if (idRx.match(config["target"].toString().trimmed()).hasMatch())
                focalId = config["target"].toString().trimmed();
        }
        if (focalId.isEmpty()) {
            m_backend->log("트윗 URL 또는 ID를 입력하세요", "error", "twitter");
            stopDaemon();
            return;
        }

        QString action = (type == "favoriters") ? "좋아요한": "리트윗한";
        excelSuffix = type + "_"+ focalId;
        m_backend->log(QString("%1 유저 수집: focal=%2").arg(action, focalId), "info", "twitter");

        QJsonArray users;
        QString uCursor;
        while (isRunning) {
            auto [batch, nextCursor] =
                (type == "favoriters") ? getFavoriters(focalId, uCursor)
                                        : getRetweeters(focalId, uCursor);
            if (nextCursor == "RATE_LIMITED") {
                handleRateLimit(accounts, currentAccountIdx, isRunning);
                continue;
            }
            if (nextCursor == "ERROR") {
                m_backend->log(action + "에러 — 5초 후 재시도", "warning", "twitter");
                for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                continue;
            }
            if (batch.isEmpty() && nextCursor.isEmpty()) break;
            for (const auto &u : batch) users.append(u);
            m_backend->log(QString("%1: %2명").arg(action).arg(users.size()), "info", "twitter");
            m_backend->updateStats(users.size(), 0, "수집 중...", "twitter");
            if (nextCursor.isEmpty()) break;
            uCursor = nextCursor;
            QThread::sleep(1);
        }

        // 유저 리스트를 Excel 로 저장 (간단 포맷)
        if (m_saveExcel && !users.isEmpty()) {
            ExcelWriter writer;
            QStringList headers = {"rest_id", "screen_name", "name", "followers",
                                   "following", "tweets", "verified", "description",
                                   "created_at", "location", "url", "profile_image"};
            writer.writeHeader(headers, QColor("#0070C0"));
            int row = 2;
            for (const auto &uv : users) {
                QJsonObject u = uv.toObject();
                QJsonObject l = u["legacy"].toObject();
                QStringList r;
                r << u["rest_id"].toString()
                  << l["screen_name"].toString()
                  << l["name"].toString()
                  << QString::number(l["followers_count"].toInt())
                  << QString::number(l["friends_count"].toInt())
                  << QString::number(l["statuses_count"].toInt())
                  << (u["is_blue_verified"].toBool() ? "True": "False")
                  << l["description"].toString()
                  << l["created_at"].toString()
                  << l["location"].toString()
                  << l["url"].toString()
                  << l["profile_image_url_https"].toString();
                writer.writeRow(row++, r);
            }
            QString outPath = userDir + "/"+ target + "_"+ excelSuffix + ".xlsx";
            writer.save(outPath);
            m_backend->log(QString("Excel 저장: %1_%2.xlsx (%3명)")
                .arg(target, excelSuffix).arg(users.size()), "info", "twitter");
        }
        m_backend->log(QString("완료! %1: %2명").arg(action).arg(users.size()),
                       "success", "twitter");
        m_backend->updateStats(users.size(), 0, "완료", "twitter");
        stopDaemon();
        return;
    }

    // ══════════════════════════════════════════════════════════════
    // 리스트 트윗 수집 — List ID 기반
    // ══════════════════════════════════════════════════════════════
    if (type == "list") {
        QString listId = config["target"].toString().trimmed();
        QRegularExpression rx(R"(/i/lists/(\d+))");
        auto mm = rx.match(listId);
        if (mm.hasMatch()) listId = mm.captured(1);
        if (listId.isEmpty()) {
            m_backend->log("리스트 ID 또는 URL을 입력하세요", "error", "twitter");
            stopDaemon();
            return;
        }
        excelSuffix = "list_"+ listId;
        m_backend->log(QString("리스트 수집: id=%1").arg(listId), "info", "twitter");

        QString lCursor;
        int lEmpty = 0;
        while (isRunning) {
            auto [batch, nextCursor] = getListTweets(listId, lCursor);
            if (nextCursor == "RATE_LIMITED") { handleRateLimit(accounts, currentAccountIdx, isRunning); continue; }
            if (nextCursor == "ERROR") {
                for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                continue;
            }
            if (batch.isEmpty()) { if (++lEmpty >= 2) break; } else { lEmpty = 0; }
            processTweetBatch(batch);
            if (nextCursor.isEmpty()) break;
            lCursor = nextCursor;
            QThread::sleep(1);
        }
        if (m_saveExcel && collectedData.count() > 0)
            saveExcelStreaming(userDir, target, collectedData, excelSuffix);
        m_backend->log(QString("완료! 리스트 트윗: %1").arg(tweetCount), "success", "twitter");
        stopDaemon();
        return;
    }

    // ══════════════════════════════════════════════════════════════
    // 커뮤니티 트윗 수집 — Community ID 기반
    // ══════════════════════════════════════════════════════════════
    if (type == "community") {
        QString commId = config["target"].toString().trimmed();
        QRegularExpression rx(R"(/i/communities/(\d+))");
        auto mm = rx.match(commId);
        if (mm.hasMatch()) commId = mm.captured(1);
        if (commId.isEmpty()) {
            m_backend->log("커뮤니티 ID 또는 URL을 입력하세요", "error", "twitter");
            stopDaemon();
            return;
        }
        excelSuffix = "community_"+ commId;
        m_backend->log(QString("커뮤니티 수집: id=%1").arg(commId), "info", "twitter");

        QString cCursor;
        int cEmpty = 0;
        while (isRunning) {
            auto [batch, nextCursor] = getCommunityTweets(commId, cCursor);
            if (nextCursor == "RATE_LIMITED") { handleRateLimit(accounts, currentAccountIdx, isRunning); continue; }
            if (nextCursor == "ERROR") {
                for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                continue;
            }
            if (batch.isEmpty()) { if (++cEmpty >= 2) break; } else { cEmpty = 0; }
            processTweetBatch(batch);
            if (nextCursor.isEmpty()) break;
            cCursor = nextCursor;
            QThread::sleep(1);
        }
        if (m_saveExcel && collectedData.count() > 0)
            saveExcelStreaming(userDir, target, collectedData, excelSuffix);
        m_backend->log(QString("완료! 커뮤니티 트윗: %1").arg(tweetCount), "success", "twitter");
        stopDaemon();
        return;
    }

    // ══════════════════════════════════════════════════════════════
    // Phase 1: 아니포 방식 — from:user 검색 + 무한 backtrack (메인)
    // 전략: 1) from:user 전체 검색 → 끝까지 페이지네이션
    //       2) 커서 끝나면 → from:user until:가장_오래된_날짜 → 다시 페이지네이션
    //       3) 반복 (backtrack 횟수 무제한, 연속 빈 결과 시 종료)
    // ══════════════════════════════════════════════════════════════
    bool searchFailed = false;  // Phase 1 검색 실패 플래그 (Phase 2에서 사용)
    // Phase 1: tweets, media, all 일 때만 실행 (tweets_api, replies는 건너뜀)
    if (isRunning && (type == "tweets"|| type == "media"|| type == "all"))
    {
        // Resume support
        int phase1Start = tweetCount;
        int backtrackCount = 0;
        int consecutiveEmpty = 0;
        const int MAX_CONSECUTIVE_EMPTY = 20;

        // ── 아니포 방식 핵심: cursor 끝나면 until:oldest_date 로 반복 ──
        // 전략 (anipo_smart.py 라인 3067~3087 참조):
        //   1) from:user → 커서 끝까지 페이지네이션
        //   2) 커서 끝나면 해당 iteration 의 가장 오래된 트윗 날짜로 until:YYYY-MM-DD
        //   3) 다시 페이지네이션
        //   4) API 가 완전히 빈 결과를 반환하면 7일씩 강제 후퇴 (긴 공백 기간 돌파)
        //   5) 연속 빈 결과 MAX_EMPTY 회 또는 2006년 도달 시 종료

        QDate scanStopDate = QDate(2006, 3, 21);  // Twitter 시작일
        const int MAX_EMPTY = 10;  // 연속 빈 iteration 종료 조건

        if (mode == "period") {
            // period 모드: since~until 사이만 검색
            // ★ BUG FIX: since:/until: 앞 공백 누락 → "usersince:..."로 검색돼서 결과 0개
            QString currentQuery = QString("from:%1").arg(target);
            if (sinceDate.isValid()) currentQuery += QString(" since:%1").arg(sinceDate.toString("yyyy-MM-dd"));
            if (untilDate.isValid()) currentQuery += QString(" until:%1").arg(untilDate.toString("yyyy-MM-dd"));

            m_backend->log(QString("Phase 1: 기간 검색 (%1)").arg(currentQuery), "info", "twitter");

            QString searchCursor;
            bool searchHasMore = true;
            while (isRunning && searchHasMore) {
                auto [tweets, nextCursor] = searchTweets(currentQuery, searchCursor);
                if (nextCursor == "RATE_LIMITED") { handleRateLimit(accounts, currentAccountIdx, isRunning); continue; }
                    if (nextCursor == "ERROR") { m_backend->log("API 오류 — 5초 후 재시도", "warning", "twitter"); for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1); continue; }
                if (tweets.isEmpty()) break;
                processTweetBatch(tweets);
                searchCursor = nextCursor;
                if (searchCursor.isEmpty()) searchHasMore = false;
                if (searchHasMore) { for (int w = qMax(2,(int)m_delay); w > 0 && isRunning; --w) QThread::sleep(1); }
            }
        } else {
            // 아니포 방식: from:user → cursor 끝나면 until:oldest 로 반복
            if (tweetCount > 0) {
                m_backend->log(QString("기존 %1개 수집분 존재 → 중복 자동 스킵").arg(tweetCount), "info", "twitter");
            }
            m_backend->log(QString("Phase 1: 아니포 방식 검색 시작 (from:%1)").arg(target), "info", "twitter");

            // ★★ "both" 모드 — Phase A (future): 새 트윗 1회 검색 후 Phase B (past) 진행
            //   resumeNewestDate / Id가 있을 때만 의미 있음 (없으면 첫 다운 → 일반 backtrack)
            if (resumeMode == "both" && (!resumeNewestDate.isEmpty() || !resumeNewestId.isEmpty())) {
                QString futureQuery = !resumeNewestDate.isEmpty()
                    ? QString("from:%1 since:%2").arg(target, resumeNewestDate)
                    : QString("from:%1 since_id:%2").arg(target, resumeNewestId);
                m_backend->log(QString("[양방향 1단계] %1 이후 새 트윗 검색")
                                   .arg(resumeNewestDate.isEmpty() ? resumeNewestId : resumeNewestDate),
                               "info", "twitter");
                QString futCursor;
                int futureNew = 0;
                int futurePages = 0;
                while (isRunning && futurePages < 50) {  // 안전장치 50페이지
                    auto [tweets, nextCursor] = searchTweets(futureQuery, futCursor);
                    if (nextCursor == "RATE_LIMITED") {
                        handleRateLimit(accounts, currentAccountIdx, isRunning);
                        continue;
                    }
                    if (nextCursor == "ERROR" || tweets.isEmpty()) break;
                    int prev = tweetCount;
                    processTweetBatch(tweets);
                    futureNew += (tweetCount - prev);
                    futurePages++;
                    futCursor = nextCursor;
                    if (futCursor.isEmpty()) break;
                    for (int w = qMax(2, (int)m_delay); w > 0 && isRunning; --w) QThread::sleep(1);
                }
                m_backend->log(QString("[양방향 1단계] %1개 새 트윗 → 2단계 backtrack 시작")
                                   .arg(futureNew), "success", "twitter");
                if (futureNew > 0) {
                    saveExcelStreaming(userDir, target, collectedData, excelSuffix);
                }
            }

            // ★ 초기 쿼리 (Phase B / past) — resumeMode 기준 분기:
            //   "past"   : until:<resumeOldestDate>  (마지막 다운한 것보다 더 과거만)
            //   "future" : since:<resumeNewestDate>  (마지막 다운 후 새로 올라온 것만, backtrack 생략)
            //   "both"   : until:<resumeOldestDate>  (Phase A 끝나고 backtrack 시작)
            QString currentQuery = QString("from:%1").arg(target);
            if (resumeMode == "future") {
                // future 모드: backtracking 안 함 → 새 트윗만 한 번 훑음
                if (!resumeNewestDate.isEmpty()) {
                    currentQuery = QString("from:%1 since:%2").arg(target, resumeNewestDate);
                    m_backend->log(QString("[현재 이어서] %1 이후 새 트윗만 검색")
                                       .arg(resumeNewestDate), "info", "twitter");
                } else if (!resumeNewestId.isEmpty()) {
                    currentQuery = QString("from:%1 since_id:%2").arg(target, resumeNewestId);
                    m_backend->log(QString("[현재 이어서] ID %1 이후 새 트윗만 검색")
                                       .arg(resumeNewestId), "info", "twitter");
                }
            } else if (!resumeOldestDate.isEmpty()) {
                // past / both: oldest 이전부터 backtrack
                QDate rd = QDate::fromString(resumeOldestDate, "yyyy-MM-dd");
                if (rd.isValid()) {
                    currentQuery = QString("from:%1 until:%2").arg(target, resumeOldestDate);
                    QString modeLabel = (resumeMode == "past") ? "[과거 이어서]" : "[양방향 이어서]";
                    m_backend->log(QString("%1 %2 이전부터").arg(modeLabel, resumeOldestDate),
                                   "info", "twitter");
                }
            }

            QDate lastOldestDate;   // 이전 iteration 의 oldest (무한 루프 감지)
            int consecutiveEmpty = 0;
            int iteration = 0;

            while (isRunning && consecutiveEmpty < MAX_EMPTY) {
                iteration++;
                backtrackCount++;
                m_backend->log(QString("[%1] %2").arg(iteration).arg(currentQuery), "info", "twitter");

                QString searchCursor;
                int iterNewCount = 0;
                int iterBatchCount = 0;  // API 가 반환한 총 트윗 수 (중복 포함)
                QDate iterOldestDate;

                while (isRunning) {
                    auto [tweets, nextCursor] = searchTweets(currentQuery, searchCursor);
                    if (nextCursor == "RATE_LIMITED") {
                        handleRateLimit(accounts, currentAccountIdx, isRunning);
                        continue;
                    }
                    if (nextCursor == "ERROR") {
                        m_backend->log("API 오류 — 5초 후 재시도", "warning", "twitter");
                        for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                        continue;
                    }
                    if (tweets.isEmpty()) break;

                    iterBatchCount += tweets.size();

                    // 가장 오래된 날짜 추적 (이 iteration 한정 + 전체)
                    for (const auto &tv : tweets) {
                        QString ca = tv.toObject()["legacy"].toObject()["created_at"].toString();
                        QDateTime td = Common::parseISODate(ca);
                        if (td.isValid()) {
                            QDate d = td.date();
                            if (!iterOldestDate.isValid() || d < iterOldestDate) iterOldestDate = d;
                            if (!oldestTweetDate.isValid() || td < oldestTweetDate) oldestTweetDate = td;
                        }
                    }

                    int prev = tweetCount;
                    processTweetBatch(tweets);
                    iterNewCount += (tweetCount - prev);

                    searchCursor = nextCursor;
                    if (searchCursor.isEmpty()) break;
                    for (int w = qMax(2, (int)m_delay); w > 0 && isRunning; --w) QThread::sleep(1);
                }

                if (iterBatchCount > 0) {
                    consecutiveEmpty = 0;
                    if (iterNewCount > 0) {
                        m_backend->log(QString("+%1개 새 트윗 (누적 %2, 미디어 %3)")
                                           .arg(iterNewCount).arg(tweetCount).arg(mediaCount), "success", "twitter");
                        saveExcelStreaming(userDir, target, collectedData, excelSuffix);
                    } else {
                        m_backend->log(QString("%1개 반환 (모두 기존 수집분)").arg(iterBatchCount), "info", "twitter");
                    }
                } else {
                    consecutiveEmpty++;
                    m_backend->log(QString("빈 결과 %1/%2").arg(consecutiveEmpty).arg(MAX_EMPTY), "info", "twitter");
                }

                // ★ "현재 이어서" 모드는 한 iteration만 — backtrack 안 함
                if (resumeMode == "future") {
                    m_backend->log(QString("[현재 이어서] 1회 검색 완료 — backtrack 생략 (총 %1개 새 트윗)").arg(iterNewCount),
                                   "success", "twitter");
                    break;
                }

                // 다음 iteration 쿼리 결정
                QDate nextUntil;
                if (iterOldestDate.isValid()) {
                    // 진전 있음 — oldest_date 를 until 로 사용 (anipo 방식: 날짜 exclusive)
                    nextUntil = iterOldestDate;
                    // 무한 루프 방지: 이전 iteration 과 같거나 더 최신이면 1일 강제 후퇴
                    if (lastOldestDate.isValid() && nextUntil >= lastOldestDate) {
                        nextUntil = lastOldestDate.addDays(-1);
                    }
                    lastOldestDate = nextUntil;
                } else {
                    // 완전 빈 결과: 현재 until 에서 7일 강제 후퇴 (긴 공백 기간 돌파)
                    QRegularExpression re(R"(until:(\d{4}-\d{2}-\d{2}))");
                    auto qm = re.match(currentQuery);
                    QDate curUntil = qm.hasMatch() ? QDate::fromString(qm.captured(1), "yyyy-MM-dd") : QDate::currentDate();
                    if (!curUntil.isValid()) curUntil = QDate::currentDate();
                    nextUntil = curUntil.addDays(-7);
                }

                if (!nextUntil.isValid() || nextUntil < scanStopDate) {
                    m_backend->log("2006년 Twitter 시작일까지 스캔 완료!", "success", "twitter");
                    break;
                }

                currentQuery = QString("from:%1 until:%2").arg(target, nextUntil.toString("yyyy-MM-dd"));
                saveProgress(nextUntil.toString("yyyy-MM-dd"));
                for (int w = 3; w > 0 && isRunning; --w) QThread::sleep(1);
            }

            if (consecutiveEmpty >= MAX_EMPTY) {
                m_backend->log(QString("%1회 연속 빈 결과 → Phase 1 종료").arg(MAX_EMPTY), "info", "twitter");
            }
        }

        int phase1New = tweetCount - phase1Start;
        searchFailed = (phase1New == 0 && backtrackCount == 0);  // 검색 API 자체가 실패
        m_backend->log(QString("Phase 1 완료: %1개 트윗, %2개 미디어%3")
                           .arg(tweetCount).arg(mediaCount)
                           .arg(searchFailed ? "(검색 API 실패)": ""),
                       searchFailed ? "warning": "success", "twitter");
    }

    // ══════════════════════════════════════════════════════════════
    // Phase 2: UserTweets API (보충 — 검색에서 누락된 트윗 보충)
    // type="tweets_api"또는 type="all"에서만 실행
    // ══════════════════════════════════════════════════════════════
    if (isRunning && (type == "tweets"|| type == "tweets_api"|| type == "all") && mode != "period") {
        m_backend->log(QString("Phase 2: UserTweets API로 누락 보충 (현재 %1개)").arg(tweetCount), "info", "twitter");
        int phase2New = 0;
        QString cursor;
        bool hasMore = true;
        int emptyRetries = 0;
        int phase2Pages = 0;

        while (isRunning && hasMore) {
            auto [tweets, nextCursor] = getUserTweets(userId, cursor);

            if (nextCursor == "RATE_LIMITED") {
                handleRateLimit(accounts, currentAccountIdx, isRunning);
                continue;
            }
            if (nextCursor == "ERROR") {
                m_backend->log("API 오류 — 5초 후 재시도", "warning", "twitter");
                for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                continue;
            }

            if (tweets.isEmpty()) {
                emptyRetries++;
                if (emptyRetries >= 5) break;  // 5회 빈 응답 시 종료
                QThread::sleep(3);
                continue;
            }
            emptyRetries = 0;
            phase2Pages++;

            int prevCount = tweetCount;
            bool pastRange = processTweetBatch(tweets);
            phase2New += (tweetCount - prevCount);
            if (pastRange) break;

            if (phase2Pages % 10 == 0) {
                m_backend->log(QString("UserTweets %1 페이지, +%2개 새 트윗 (총 %3개)")
                                   .arg(phase2Pages).arg(phase2New).arg(tweetCount), "info", "twitter");
                // 중간 저장
                saveExcelStreaming(userDir, target, collectedData, excelSuffix);
            }

            cursor = nextCursor;
            if (cursor.isEmpty()) hasMore = false;
            if (hasMore && isRunning) { for (int w = qMax(2,(int)m_delay); w > 0 && isRunning; --w) QThread::sleep(1); }
        }

        m_backend->log(QString("Phase 2 완료: +%1개 보충 (총 %2개)").arg(phase2New).arg(tweetCount), "success", "twitter");
        if (phase2New > 0) saveExcelStreaming(userDir, target, collectedData, excelSuffix);
    }

    // ══════════════════════════════════════════════════════════════
    // Phase 3: 답글 별도 검색 (filter:replies)
    // type="replies"또는 type="all"에서만 실행
    // ══════════════════════════════════════════════════════════════
    if (isRunning && (type == "replies"|| type == "all") && mode != "period") {
        m_backend->log("Phase 3: 답글 검색 (filter:replies) + 주간 백트래킹...", "info", "twitter");
        int replyNew = 0;

        // Step 1: 최신 답글부터 검색
        {
            QString replyQuery = QString("from:%1 filter:replies").arg(target);
            QString replyCursor;
            bool replyHasMore = true;
            int replyEmptyRetries = 0;

            while (isRunning && replyHasMore) {
                auto [tweets, nextCursor] = searchTweets(replyQuery, replyCursor);
                if (nextCursor == "RATE_LIMITED") {
                    handleRateLimit(accounts, currentAccountIdx, isRunning);
                    continue;
                }
                if (nextCursor == "ERROR") {
                    m_backend->log("API 오류 — 5초 후 재시도", "warning", "twitter");
                    for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                    continue;
                }
                if (tweets.isEmpty()) {
                    replyEmptyRetries++;
                    if (replyEmptyRetries >= 5) break;
                    QThread::sleep(3);
                    continue;
                }
                replyEmptyRetries = 0;

                // 가장 오래된 날짜 추적
                for (const auto &tv : tweets) {
                    QString ca = tv.toObject()["legacy"].toObject()["created_at"].toString();
                    QDateTime td = Common::parseISODate(ca);
                    if (td.isValid() && (!oldestTweetDate.isValid() || td < oldestTweetDate))
                        oldestTweetDate = td;
                }

                int prevCount = tweetCount;
                processTweetBatch(tweets);
                replyNew += (tweetCount - prevCount);

                replyCursor = nextCursor;
                if (replyCursor.isEmpty()) replyHasMore = false;
                if (replyHasMore) { for (int w = qMax(2,(int)m_delay); w > 0 && isRunning; --w) QThread::sleep(1); }
            }
        }

        // Step 2: 아니포 방식 백트래킹 (until:oldest_date 반복)
        {
            QDate replyStopDate(2006, 3, 21);
            QDate replyLastOldest;  // 무한 루프 감지
            int replyConsecutiveEmpty = 0;
            const int REPLY_MAX_EMPTY = 10;

            // 초기 until: Step 1 에서 찾은 가장 오래된 답글 날짜
            QString replyQuery2;
            if (oldestTweetDate.isValid()) {
                replyQuery2 = QString("from:%1 filter:replies until:%2")
                    .arg(target, oldestTweetDate.date().toString("yyyy-MM-dd"));
            } else {
                replyQuery2 = QString("from:%1 filter:replies").arg(target);
            }

            while (isRunning && replyConsecutiveEmpty < REPLY_MAX_EMPTY) {
                QString rCursor;
                int iterNew = 0;
                int iterBatch = 0;
                QDate iterOldest;

                while (isRunning) {
                    auto [tweets, nextCursor] = searchTweets(replyQuery2, rCursor);
                    if (nextCursor == "RATE_LIMITED") {
                        handleRateLimit(accounts, currentAccountIdx, isRunning);
                        continue;
                    }
                    if (nextCursor == "ERROR") {
                        m_backend->log("API 오류 — 5초 후 재시도", "warning", "twitter");
                        for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                        continue;
                    }
                    if (tweets.isEmpty()) break;

                    iterBatch += tweets.size();

                    for (const auto &tv : tweets) {
                        QString ca = tv.toObject()["legacy"].toObject()["created_at"].toString();
                        QDateTime td = Common::parseISODate(ca);
                        if (td.isValid()) {
                            QDate d = td.date();
                            if (!iterOldest.isValid() || d < iterOldest) iterOldest = d;
                            if (!oldestTweetDate.isValid() || td < oldestTweetDate) oldestTweetDate = td;
                        }
                    }

                    int prevCount = tweetCount;
                    processTweetBatch(tweets);
                    iterNew += (tweetCount - prevCount);
                    replyNew += (tweetCount - prevCount);

                    rCursor = nextCursor;
                    if (rCursor.isEmpty()) break;
                    for (int w = qMax(2, (int)m_delay); w > 0 && isRunning; --w) QThread::sleep(1);
                }

                if (iterBatch > 0) {
                    replyConsecutiveEmpty = 0;
                    if (iterNew > 0) saveExcelStreaming(userDir, target, collectedData, excelSuffix);
                } else {
                    replyConsecutiveEmpty++;
                }

                // 다음 쿼리
                QDate nextUntil;
                if (iterOldest.isValid()) {
                    nextUntil = iterOldest;
                    if (replyLastOldest.isValid() && nextUntil >= replyLastOldest) {
                        nextUntil = replyLastOldest.addDays(-1);
                    }
                    replyLastOldest = nextUntil;
                } else {
                    QRegularExpression re(R"(until:(\d{4}-\d{2}-\d{2}))");
                    auto qm = re.match(replyQuery2);
                    QDate curUntil = qm.hasMatch() ? QDate::fromString(qm.captured(1), "yyyy-MM-dd") : QDate::currentDate();
                    if (!curUntil.isValid()) curUntil = QDate::currentDate();
                    nextUntil = curUntil.addDays(-7);
                }

                if (!nextUntil.isValid() || nextUntil < replyStopDate) break;

                replyQuery2 = QString("from:%1 filter:replies until:%2")
                    .arg(target, nextUntil.toString("yyyy-MM-dd"));
                for (int w = 3; w > 0 && isRunning; --w) QThread::sleep(1);
            }
        }

        if (replyNew > 0) {
            m_backend->log(QString("Phase 3 완료: 답글 +%1개").arg(replyNew), "success", "twitter");
            saveExcelStreaming(userDir, target, collectedData, excelSuffix);
        }
    }

    // ══════════════════════════════════════════════════════════════
    // Phase 4: 답글 대화(conversation) 미디어 수집
    // 내가 쓴 답글에서 시작된 대화 스레드 → media/conversations/ 에 저장
    // ══════════════════════════════════════════════════════════════
    if (isRunning && (type == "replies"|| type == "all") && m_downloadMedia) {
        m_backend->log("Phase 4: 답글 대화(conversation) 미디어 수집...", "info", "twitter");
        QString convMediaDir = mediaDir + "/conversations";
        QDir().mkpath(convMediaDir);
        int convMediaCount = 0;
        int convTweetCount = 0;

        // collectedData에서 Reply 타입 + reply_count > 0 인 것들 추출
        QJsonArray allData = collectedData.readAll();
        QList<QString> replyIdsWithConv;
        for (const auto &val : allData) {
            QJsonObject d = val.toObject();
            QString dtype = d["type"].toString();
            if (!dtype.contains("Reply")) continue;
            // 해당 답글의 대화가 있는지: conversation이 있으면 수집
            // ID로 TweetDetail 조회
            QString rid = d["id"].toString();
            if (!rid.isEmpty() && !collectedIds.contains("conv_"+ rid)) {
                replyIdsWithConv.append(rid);
            }
        }

        m_backend->log(QString("대화 가능 답글 %1개 탐색").arg(replyIdsWithConv.size()), "info", "twitter");

        for (int ci = 0; ci < replyIdsWithConv.size() && isRunning; ++ci) {
            const QString &replyId = replyIdsWithConv[ci];
            collectedIds.insert("conv_"+ replyId);

            // TweetDetail로 대화 트리 조회
            QString cursor;
            bool hasMore = true;
            while (isRunning && hasMore) {
                auto [replies, nextCursor] = getTweetDetail(replyId, cursor);
                if (nextCursor == "RATE_LIMITED") {
                    handleRateLimit(accounts, currentAccountIdx, isRunning);
                    continue;
                }
                if (nextCursor == "ERROR"|| replies.isEmpty()) break;

                for (const auto &tv : replies) {
                    QJsonObject tw = tv.toObject();
                    QJsonObject leg = tw["legacy"].toObject();
                    QString tid = leg["id_str"].toString();
                    if (tid.isEmpty()) tid = tw["rest_id"].toString();
                    if (tid.isEmpty() || collectedIds.contains(tid)) continue;
                    collectedIds.insert(tid);

                    // 미디어가 있는 경우만 다운로드
                    QJsonArray mediaArr = leg["extended_entities"].toObject()["media"].toArray();
                    if (mediaArr.isEmpty()) mediaArr = leg["entities"].toObject()["media"].toArray();
                    if (mediaArr.isEmpty()) continue;

                    QJsonObject authorResult = tw["core"].toObject()["user_results"].toObject()["result"].toObject();
                    QString convAuthor = authorResult["legacy"].toObject()["screen_name"].toString();
                    convTweetCount++;

                    int newMedia = downloadTweetMedia(tw, convMediaDir, convTweetCount);
                    convMediaCount += newMedia;
                    mediaCount += newMedia;

                    if (convTweetCount % 5 == 0 || newMedia > 0) {
                        m_backend->log(QString("대화 @%1: +%2 미디어").arg(convAuthor).arg(newMedia), "info", "twitter");
                    }
                }

                cursor = nextCursor;
                if (cursor.isEmpty()) hasMore = false;
                if (hasMore) QThread::sleep(2);
            }

            if (ci % 10 == 0) {
                m_backend->updateStats(tweetCount, mediaCount,
                    QString("대화 탐색 %1/%2").arg(ci + 1).arg(replyIdsWithConv.size()), "twitter");
            }
        }

        if (convMediaCount > 0) {
            m_backend->log(QString("Phase 4 완료: 대화 미디어 +%1개 (%2 대화)").arg(convMediaCount).arg(convTweetCount), "success", "twitter");
        } else {
            m_backend->log("Phase 4: 추가 대화 미디어 없음", "info", "twitter");
        }
    }

    {
        double collectRatio = (statusesCount > 0) ? (double)tweetCount / statusesCount : 1.0;
        m_backend->log(QString("최종: %1개 트윗, %2개 미디어 (수집률 %3%)")
                           .arg(tweetCount).arg(mediaCount).arg(qRound(collectRatio * 100)),
                       "success", "twitter");
    }

    // Save Excel (streaming — no full memory load)
    // 아니포 방식: Excel 자체가 체크포인트 → 항상 저장
    if (collectedData.count() > 0) {
        m_backend->log("Excel 저장...", "info", "twitter");
        saveExcelStreaming(userDir, target, collectedData, excelSuffix);
    }

    // 완료 시 progress 파일은 유지 (다음 이어서 수집에 사용)
    // 최종 상태 저장
    if (oldestTweetDate.isValid()) {
        saveProgress(oldestTweetDate.toString("yyyy-MM-dd"));
    }

    m_backend->log(QString("완료! %1: %2, 미디어: %3").arg(logLabel).arg(tweetCount).arg(mediaCount),
                   "success", "twitter");
    m_backend->updateStats(tweetCount, mediaCount, "완료", "twitter");

    // Save profile Excel (all encountered users: RT, quoted, etc.)
    saveProfileExcel(userDir, target);

    // 새트윗 확인용: userDir 저장
    m_currentUserDir = userDir;

    // Clean up daemon
    stopDaemon();
}

void TwitterCollector::checkNewPosts(const QJsonObject &config, bool &isRunning)
{
    if (m_newestTweetId.isEmpty()) {
        m_backend->log("아직 수집된 트윗이 없습니다", "warning", "twitter");
        return;
    }
    if (m_currentTarget.isEmpty()) {
        m_backend->log("타겟 사용자 정보가 없습니다", "warning", "twitter");
        return;
    }

    m_backend->log("════════════════════════════════", "info", "twitter");
    m_backend->log("새 트윗 체크 중... (리트윗 포함)", "info", "twitter");

    QJsonArray accounts = config["accounts"].toArray();
    if (accounts.isEmpty()) {
        m_backend->log("계정이 없습니다", "error", "twitter");
        return;
    }

    // Use first account
    QJsonObject acct = accounts[0].toObject();
    setupClient(acct["auth_token"].toString(), acct["ct0"].toString());

    if (!startDaemon()) {
        m_backend->log("데몬 시작 실패", "error", "twitter");
        return;
    }

    // UserTweets API로 최신 트윗 가져오기 (리트윗 포함!)
    // SearchTimeline은 from:user 검색 시 리트윗을 빠뜨리므로 UserTweets 사용
    QString userId = config["userId"].toString();
    if (userId.isEmpty()) {
        // userId 없으면 먼저 조회
        QJsonObject userObj = getUserByScreenName(m_currentTarget);
        userId = userObj["rest_id"].toString();
    }

    if (userId.isEmpty()) {
        m_backend->log("유저 ID 조회 실패", "error", "twitter");
        stopDaemon();
        return;
    }

    auto [tweets, nextCursor] = getUserTweets(userId);

    // newest ID보다 큰 것만 필터
    QJsonArray newTweets;
    qlonglong newestIdNum = m_newestTweetId.toLongLong();
    for (const auto &tweetVal : tweets) {
        QJsonObject tweet = tweetVal.toObject();
        QJsonObject legacy = tweet["legacy"].toObject();
        QString tweetId = legacy["id_str"].toString();
        if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();
        if (tweetId.toLongLong() > newestIdNum) {
            newTweets.append(tweet);
        }
    }

    if (newTweets.isEmpty()) {
        m_backend->log("→ 새 트윗 없음", "info", "twitter");
        m_backend->log("════════════════════════════════", "info", "twitter");
        stopDaemon();
        return;
    }

    m_backend->log(QString("새 트윗 %1개 발견!").arg(newTweets.size()), "success", "twitter");

    int newCount = 0;
    int newMedia = 0;
    QString mediaDir = m_currentUserDir + "/media";
    QDir().mkpath(mediaDir);

    for (const auto &tweetVal : newTweets) {
        if (!isRunning) break;
        QJsonObject tweet = tweetVal.toObject();
        QJsonObject legacy = tweet["legacy"].toObject();
        QString tweetId = legacy["id_str"].toString();
        if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();

        // Update newest ID
        if (tweetId.toLongLong() > m_newestTweetId.toLongLong())
            m_newestTweetId = tweetId;

        // RT 여부 체크
        bool isRT = legacy.contains("retweeted_status_result") || tweet.contains("retweeted_status_result");
        QString text = legacy["full_text"].toString();
        if (isRT) {
            QJsonObject rtResult = legacy["retweeted_status_result"].toObject()["result"].toObject();
            if (rtResult.isEmpty()) rtResult = tweet["retweeted_status_result"].toObject()["result"].toObject();
            if (rtResult.contains("tweet")) rtResult = rtResult["tweet"].toObject();
            QString rtAuthor = rtResult["core"].toObject()["user_results"].toObject()["result"].toObject()["legacy"].toObject()["screen_name"].toString();
            m_backend->log(QString("RT @%1: %2...").arg(rtAuthor, rtResult["legacy"].toObject()["full_text"].toString().left(40)), "info", "twitter");
        } else {
            m_backend->log(QString("%1...").arg(text.left(50)), "info", "twitter");
        }

        // Download media (타입별 폴더 분리)
        if (m_downloadMedia) {
            QString newMediaDir = isRT ? (mediaDir + "/reposts") : (mediaDir + "/tweets");
            QDir().mkpath(newMediaDir);
            int dm = downloadTweetMedia(tweet, newMediaDir, newCount + 1);
            newMedia += dm;
        }

        newCount++;
    }

    m_backend->log(QString("새 트윗 %1개 (RT포함), 미디어 %2개").arg(newCount).arg(newMedia), "success", "twitter");
    m_backend->log("════════════════════════════════", "info", "twitter");

    stopDaemon();
}
