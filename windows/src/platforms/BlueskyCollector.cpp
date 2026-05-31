#include "BlueskyCollector.h"
#include "core/MiyoBackend.h"
#include "core/Common.h"

#include <QThread>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QProcess>
#include <QProcessEnvironment>
#include <QDirIterator>
#include <QDateTime>
#include <QFileInfo>
#include <QMap>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QNetworkCookie>
#include "xlsxdocument.h"

BlueskyCollector::BlueskyCollector(MiyoBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
{
}

BlueskyCollector::~BlueskyCollector()
{
    stopDaemon();
}

// ── Python Daemon Management ──

bool BlueskyCollector::startDaemonMulti(const QJsonArray &accounts)
{
    QJsonObject firstAccount = accounts[0].toObject();
    QJsonObject initArgs;
    initArgs["handle"] = firstAccount["handle"].toString();
    initArgs["password"] = firstAccount["password"].toString();

    // Pass all accounts for rotation
    QJsonArray acctArray;
    for (const auto &a : accounts) {
        QJsonObject acct = a.toObject();
        QJsonObject entry;
        entry["handle"] = acct["handle"].toString();
        entry["password"] = acct["password"].toString();
        acctArray.append(entry);
    }
    initArgs["accounts"] = acctArray;

    return startDaemon(firstAccount["handle"].toString(), firstAccount["password"].toString(), initArgs);
}

bool BlueskyCollector::startDaemon(const QString &handle, const QString &password, QJsonObject customInitArgs)
{
    stopDaemon();

    QString scriptPath = Common::bundledToolsDir() + "/bluesky_daemon.py";
    if (!QFile::exists(scriptPath))
        scriptPath = QCoreApplication::applicationDirPath() + "/../../resources/tools/bluesky_daemon.py";
    if (!QFile::exists(scriptPath)) {
        m_backend->log("bluesky_daemon.py not found", "error", "bluesky");
        return false;
    }

    QJsonObject initArgs;
    if (!customInitArgs.isEmpty()) {
        initArgs = customInitArgs;
    } else {
        initArgs["handle"] = handle;
        initArgs["password"] = password;
    }
    QString argsJson = QString::fromUtf8(QJsonDocument(initArgs).toJson(QJsonDocument::Compact));

    m_daemon = new QProcess(this);
    m_daemon->setProcessEnvironment(Common::bundledProcessEnv());

    // 번들 Python 우선 → system fallback
    QStringList pythons = Common::pythonCandidates();
    bool started = false;
    for (const auto &python : pythons) {
        m_daemon->start(python, {scriptPath, argsJson});
        if (m_daemon->waitForStarted(5000)) {
            started = true;
            m_backend->log("Daemon: " + python, "info", "bluesky");
            break;
        }
    }
    if (!started) {
        m_backend->log("Python not found", "error", "bluesky");
        delete m_daemon;
        m_daemon = nullptr;
        return false;
    }

    // Wait for "ready" signal
    if (!m_daemon->waitForReadyRead(30000)) {
        m_backend->log("Daemon: timeout waiting for ready", "error", "bluesky");
        stopDaemon();
        return false;
    }

    while (m_daemon->canReadLine() || m_daemon->waitForReadyRead(3000)) {
        QByteArray line = m_daemon->readLine().trimmed();
        if (line.isEmpty()) continue;
        QJsonObject msg = QJsonDocument::fromJson(line).object();

        if (msg.contains("error")) {
            m_backend->log("Daemon error: " + msg["error"].toString(), "error", "bluesky");
            stopDaemon();
            return false;
        }
        if (msg.contains("log")) {
            m_backend->log("Daemon: " + msg["log"].toString(), "info", "bluesky");
        }
        if (msg["status"].toString() == "ready") {
            m_daemonReady = true;
            m_backend->log("Daemon ready", "success", "bluesky");
            return true;
        }
    }

    m_backend->log("Daemon: never became ready", "error", "bluesky");
    stopDaemon();
    return false;
}

void BlueskyCollector::stopDaemon()
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

void BlueskyCollector::processOutputLines(const QByteArray &data)
{
    // Process intermediate log/progress lines from daemon
    for (const auto &line : data.split('\n')) {
        if (line.trimmed().isEmpty()) continue;
        QJsonObject msg = QJsonDocument::fromJson(line.trimmed()).object();
        if (msg.contains("log")) {
            m_backend->log(msg["log"].toString(), "info", "bluesky");
        }
        if (msg.contains("progress")) {
            QJsonObject p = msg["progress"].toObject();
            m_backend->updateStats(p["count"].toInt(), p["media"].toInt(),
                                   p["status"].toString("수집 중..."), "bluesky");
        }
    }
}

QJsonObject BlueskyCollector::sendCommand(const QJsonObject &cmd, bool &isRunning, int timeoutMs)
{
    if (!m_daemon || !m_daemonReady) return {{"error", "Daemon not available"}};

    int lastCount = 0, lastMedia = 0;  // 마지막 progress 값 (중지 시 반환용)

    QByteArray cmdLine = QJsonDocument(cmd).toJson(QJsonDocument::Compact) + "\n";
    m_daemon->write(cmdLine);

    QByteArray buffer;
    QElapsedTimer timer;
    timer.start();

    // Bluesky collections can take very long (hours for large accounts)
    // Read output lines as they come, looking for a final result
    while (isRunning && timer.elapsed() < timeoutMs) {
        int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) break;

        if (!m_daemon->waitForReadyRead(qMin(remaining, 500))) {
            if (m_daemon->state() != QProcess::Running) {
                m_backend->log("Daemon died", "error", "bluesky");
                stopDaemon();
                return {{"error", "Daemon died"}};
            }
            continue;
        }

        buffer += m_daemon->readAll();

        // Process all complete lines
        while (buffer.contains('\n')) {
            int nlIdx = buffer.indexOf('\n');
            QByteArray line = buffer.left(nlIdx).trimmed();
            buffer = buffer.mid(nlIdx + 1);

            if (line.isEmpty()) continue;

            QJsonObject msg = QJsonDocument::fromJson(line).object();

            // Log messages
            if (msg.contains("log")) {
                QString logMsg = msg["log"].toString();
                // Rate Limit 로그는 Python 데몬이 자체 처리 (대기/중지 모드)
                // C++ 에서는 로그만 전달
                if (logMsg.contains("Rate Limit") || logMsg.contains("rate limit"))
                    m_backend->log(logMsg, "warning", "bluesky");
                else
                    m_backend->log(logMsg, "info", "bluesky");
                continue;
            }
            // Progress updates
            if (msg.contains("progress")) {
                QJsonObject p = msg["progress"].toObject();
                lastCount = p["count"].toInt();
                lastMedia = p["media"].toInt();
                m_backend->updateStats(lastCount, lastMedia,
                                       p["status"].toString("수집 중..."), "bluesky");
                continue;
            }
            // Final result (has "status" or "error" key but NOT "log" or "progress")
            if (msg.contains("status") || msg.contains("error")) {
                return msg;
            }
        }
    }

    // 중지/타임아웃 시에도 마지막 progress 값 반환
    if (!isRunning) {
        QJsonObject r;
        r["error"] = "Stopped by user";
        r["count"] = lastCount;
        r["media"] = lastMedia;
        return r;
    }
    return {{"error", "Timeout"}};
}

// ── Main collect ──

void BlueskyCollector::collect(const QJsonObject &config, bool &isRunning)
{
    // Read first account from accounts array
    QJsonArray accounts = config["accounts"].toArray();
    if (accounts.isEmpty()) {
        m_backend->log("계정을 먼저 추가하세요", "error", "bluesky");
        return;
    }

    QJsonObject firstAccount = accounts[0].toObject();
    QString handle = firstAccount["handle"].toString();
    QString password = firstAccount["password"].toString();

    if (handle.isEmpty() || password.isEmpty()) {
        m_backend->log("핸들과 앱 비밀번호를 입력하세요", "error", "bluesky");
        return;
    }

    // Start Python daemon with all accounts
    m_backend->log(handle + " 로그인 중... (계정 " + QString::number(accounts.size()) + "개)", "info", "bluesky");
    if (!startDaemonMulti(accounts)) {
        m_backend->log("Bluesky daemon start failed", "error", "bluesky");
        return;
    }

    QString target = config["target"].toString();
    if (target.isEmpty()) target = handle;
    target = target.trimmed();
    if (target.startsWith("@")) target = target.mid(1);

    QString type = config["type"].toString("posts");

    // Rate Limit 옵션
    m_rateLimitWait = config["rateLimitWait"].toBool(false);
    m_rateLimitWaitMins = config["rateLimitWaitMins"].toInt(5);
    if (m_rateLimitWaitMins < 1) m_rateLimitWaitMins = 1;
    if (m_rateLimitWaitMins > 60) m_rateLimitWaitMins = 60;

    // Build save path
    QString basePath = config["path"].toString();
    if (basePath.isEmpty()) basePath = QDir::homePath() + "/Downloads";
    basePath.replace("~", QDir::homePath());

    // ── ALL: 전체 수집 모드 ──
    if (type == "all") {
        m_backend->log("═══ 전체 수집 모드 (Rate Limit 자동 대기) ═══", "success", "bluesky");
        QStringList subTypes = {"posts", "replies", "likes", "comments", "followers", "following", "profile"};
        int step = 0;
        int totalCount = 0, totalMedia = 0;

        for (const auto &subType : subTypes) {
            step++;
            if (!isRunning) break;

            m_backend->log(QString("▶ [%1/%2] %3 수집...").arg(step).arg(subTypes.size()).arg(subType), "info", "bluesky");

            QJsonObject cmd;
            cmd["action"] = subType;
            cmd["target"] = target;
            cmd["save_path"] = basePath;
            cmd["download_media"] = config["media"].toBool(true);
            cmd["exif"] = config["exif"].toBool(true);
            // 전체 다운로드 모드: 항상 대기+재시도 (15분)
            cmd["rl_mode"] = "wait";
            cmd["rl_wait_mins"] = 15;

            QJsonObject result = sendCommand(cmd, isRunning, 24 * 3600 * 1000);

            // count/media는 error 시에도 있을 수 있음 (중지 시 마지막 progress)
            int count = result["count"].toInt(result["posts"].toInt(0));
            int media = result["media"].toInt(0);
            totalCount += count;
            totalMedia += media;

            if (result.contains("error")) {
                QString err = result["error"].toString();
                if (count > 0)
                    m_backend->log(QString("%1 중단: %2건").arg(subType).arg(count), "warning", "bluesky");
                else
                    m_backend->log(subType + " 오류: " + err, "warning", "bluesky");
                if (err.contains("Profile not found") || err.contains("not found") || err.contains("InvalidRequest")) {
                    m_backend->log("❌ 사용자를 찾을 수 없습니다. 핸들을 확인해주세요.", "error", "bluesky");
                    break;
                }
            } else {
                m_backend->log(QString("%1 완료: %2건").arg(subType).arg(count), "success", "bluesky");
            }
        }

        m_backend->log(QString("═══ 전체 수집 완료! 합계: %1건, 미디어: %2 ═══").arg(totalCount).arg(totalMedia), "success", "bluesky");
        m_backend->updateStats(totalCount, totalMedia, "완료", "bluesky");
        stopDaemon();
        return;
    }

    // Build command for daemon
    QJsonObject cmd;
    cmd["action"] = type;
    cmd["target"] = target;
    cmd["save_path"] = basePath;
    cmd["download_media"] = config["media"].toBool(true);
    cmd["exif"] = config["exif"].toBool(true);
    // Rate Limit 모드: 사용자 선택에 따라
    cmd["rl_mode"] = m_rateLimitWait ? "wait" : "stop";
    cmd["rl_wait_mins"] = m_rateLimitWaitMins;

    if (type == "search") {
        cmd["query"] = config["query"].toString();
    }

    m_backend->log(type + " 수집 시작 → " + target, "info", "bluesky");

    // Send command and wait for completion (can take hours)
    QJsonObject result = sendCommand(cmd, isRunning, 24 * 3600 * 1000);  // 24h timeout

    if (result.contains("error")) {
        m_backend->log("오류: " + result["error"].toString(), "error", "bluesky");
    } else {
        int count = result["count"].toInt(result["posts"].toInt(0));
        int media = result["media"].toInt(0);
        m_backend->log(QString("완료! %1: %2, Media: %3").arg(type).arg(count).arg(media), "success", "bluesky");
        m_backend->updateStats(count, media, "완료", "bluesky");

        // ★ 진짜 페이지 캡쳐 — 데몬이 만든 Excel에서 post_url 추출 → 브라우저로 방문해서 저장
        if (config["realCapture"].toBool(true) && isRunning) {
            QString shortHandle = target;
            if (shortHandle.endsWith(".bsky.social")) shortHandle.chop(QString(".bsky.social").size());
            QString xlsxName;
            if (type == "posts" || type == "media_only") xlsxName = shortHandle + "_posts.xlsx";
            else if (type == "replies") xlsxName = shortHandle + "_replies.xlsx";
            else if (type == "likes") xlsxName = shortHandle + "_likes.xlsx";
            else xlsxName.clear();

            if (!xlsxName.isEmpty()) {
                QString xlsxPath = basePath + "/bluesky/" + shortHandle + "/excel/" + xlsxName;
                if (QFile::exists(xlsxPath)) {
                    QString capturesDir = basePath + "/bluesky/" + shortHandle + "/captures";
                    QDir().mkpath(capturesDir);
                    QXlsx::Document doc(xlsxPath);
                    int lastRow = doc.dimension().lastRow();
                    // post_url 컬럼 찾기 (헤더 행에서 검색)
                    int urlCol = 0;
                    for (int c = 1; c <= 20; ++c) {
                        QString h = doc.read(1, c).toString().trimmed();
                        if (h == "post_url") { urlCol = c; break; }
                    }
                    if (urlCol > 0) {
                        m_backend->log(QString("진짜 페이지 캡쳐 시작 — %1행").arg(lastRow - 1), "info", "bluesky");
                        int captured = 0;
                        for (int r = 2; r <= lastRow && isRunning; ++r) {
                            QString postUrl = doc.read(r, urlCol).toString().trimmed();
                            if (postUrl.isEmpty() || !postUrl.contains("bsky.app/profile/")) continue;
                            // filename = post_id from URL tail
                            QString postId = postUrl.section('/', -1);
                            if (postId.isEmpty()) continue;
                            // Bluesky cookies — public view 기본은 미로그인 OK.
                            //   사용자가 config["cookie"] 입력 시 (UI bsky-cookie 입력칸) → 로그인 상태 캡쳐
                            QList<QNetworkCookie> bskyCookies;
                            QString rawBsCk = config["cookie"].toString();
                            if (!rawBsCk.isEmpty()) {
                                for (const QString &part : rawBsCk.split(';', Qt::SkipEmptyParts)) {
                                    int eq = part.indexOf('=');
                                    if (eq <= 0) continue;
                                    QNetworkCookie c(part.left(eq).trimmed().toUtf8(),
                                                      part.mid(eq + 1).trimmed().toUtf8());
                                    c.setDomain(".bsky.app"); c.setPath("/"); c.setSecure(true);
                                    bskyCookies << c;
                                }
                            }
                            // ★ Bluesky 로그인 체크 — bsky.app은 보통 미로그인도 보이지만
                            //   private/age-gated 게시물은 로그인 필요
                            static const QString bskyLoginCheck = R"JS(
                                (function(){
                                    if (location.pathname.indexOf('/login') === 0) return true;
                                    // post 본문이 있으면 정상 페이지 (bsky.app은 미로그인도 보통 보임)
                                    if (document.querySelector('[data-testid="postThreadItem"], main article, main [role="link"]')) return false;
                                    return !!document.querySelector('input[type="password"]');
                                })()
                            )JS";
                            bool ok = m_backend->captureRealPageCDPLoginAware(
                                postUrl, capturesDir, postId,
                                bskyLoginCheck, "bluesky", 6000, bskyCookies, config);
                            if (ok) captured++;
                        }
                        m_backend->log(QString("진짜 페이지 캡쳐 완료: %1개").arg(captured), "success", "bluesky");
                    }
                }
            }
        }
    }

    // ★ 다운로드 끝났으면 갤러리 자동 생성 (사용자 폴더 root 에 gallery.html)
    {
        QString shortHandle = target;
        if (shortHandle.endsWith(".bsky.social")) shortHandle.chop(QString(".bsky.social").size());
        QString userDir = basePath + "/bluesky/" + shortHandle;
        if (QDir(userDir).exists()) {
            generateMediaGallery(userDir, shortHandle);
        }
    }

    stopDaemon();
}

// ═════════════════════════════════════════════════════════════════════════
// generateMediaGallery — 사용자 폴더 통째 스캔 → gallery.html 생성
//   모든 jpg/png/gif/webp/mp4/webm 그리드. lightbox 클릭. lazy loading.
// ═════════════════════════════════════════════════════════════════════════
void BlueskyCollector::generateMediaGallery(const QString &userDir, const QString &handle)
{
    QStringList imageExts = {"jpg", "jpeg", "png", "gif", "webp", "avif"};
    QStringList videoExts = {"mp4", "webm", "mov", "m4v"};

    struct MediaItem { QString relPath; QString type; QString folder; qint64 size; };
    QList<MediaItem> items;

    QDirIterator it(userDir, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString full = it.next();
        QString rel = QDir(userDir).relativeFilePath(full);
        if (rel.startsWith("gallery") || rel.endsWith(".html") || rel.endsWith(".xlsx")
            || rel.endsWith(".json") || rel.endsWith(".txt") || rel.endsWith(".log")) continue;
        QString ext = QFileInfo(full).suffix().toLower();
        QString type;
        if (imageExts.contains(ext)) type = "image";
        else if (videoExts.contains(ext)) type = "video";
        else continue;
        MediaItem mi;
        mi.relPath = rel;
        mi.type = type;
        mi.folder = QFileInfo(rel).path();
        mi.size = QFileInfo(full).size();
        items.append(mi);
    }

    if (items.isEmpty()) {
        m_backend->log("🖼 갤러리: 미디어 없음 — 생성 안 함", "info", "bluesky");
        return;
    }

    // 폴더별 그룹 (replies / posts / likes 등)
    QMap<QString, QList<MediaItem>> grouped;
    for (const auto &mi : items) grouped[mi.folder].append(mi);

    QString galleryPath = userDir + "/gallery.html";
    QFile f(galleryPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_backend->log(QString("🖼 갤러리 생성 실패: %1").arg(galleryPath), "warning", "bluesky");
        return;
    }

    QString html;
    html += "<!DOCTYPE html><html><head><meta charset=utf-8>";
    html += "<title>Bluesky 미디어 갤러리 — @" + handle.toHtmlEscaped() + "</title>";
    html += "<meta name=viewport content='width=device-width,initial-scale=1'>";
    html += R"(<style>
        body{font-family:-apple-system,BlinkMacSystemFont,system-ui,sans-serif;background:#0e0e10;color:#e7e7ea;margin:0;padding:20px;}
        h1{font-size:20px;margin:0 0 8px;color:#7c5cff;}
        .meta{color:#888;font-size:13px;margin-bottom:24px;}
        .group{margin-bottom:32px;}
        .group-title{font-size:15px;color:#bbb;margin:16px 0 10px;border-bottom:1px solid #2a2a30;padding-bottom:6px;}
        .grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(180px,1fr));gap:8px;}
        .item{position:relative;background:#1a1a1d;border-radius:6px;overflow:hidden;cursor:pointer;aspect-ratio:1;}
        .item img,.item video{width:100%;height:100%;object-fit:cover;display:block;}
        .item:hover{outline:2px solid #7c5cff;}
        .badge{position:absolute;top:4px;right:4px;background:rgba(0,0,0,0.7);color:#fff;font-size:10px;padding:2px 6px;border-radius:3px;}
        .lightbox{display:none;position:fixed;inset:0;background:rgba(0,0,0,0.95);z-index:9999;align-items:center;justify-content:center;padding:20px;cursor:zoom-out;}
        .lightbox.show{display:flex;}
        .lightbox img,.lightbox video{max-width:95vw;max-height:95vh;object-fit:contain;}
        .nav{position:fixed;top:20px;right:20px;display:flex;gap:8px;}
        .nav button{background:rgba(124,92,255,0.2);color:#7c5cff;border:1px solid #7c5cff44;padding:6px 12px;border-radius:6px;cursor:pointer;font-size:12px;}
        .nav button:hover{background:rgba(124,92,255,0.4);}
        #filter input{background:#1a1a1d;color:#fff;border:1px solid #333;padding:6px 10px;border-radius:6px;width:240px;font-size:13px;}
    </style></head><body>)";
    html += "<h1>🦋 Bluesky 미디어 갤러리 — @" + handle.toHtmlEscaped() + "</h1>";
    html += QString("<div class=meta>미디어 %1개 · %2 그룹 · 생성 %3</div>")
        .arg(items.size()).arg(grouped.size()).arg(QDateTime::currentDateTime().toString());
    html += "<div class=nav><div id=filter><input type=text id=q placeholder='파일명 검색...' oninput='filterItems()'></div></div>";

    int totalIdx = 0;
    for (auto it = grouped.constBegin(); it != grouped.constEnd(); ++it) {
        const QString &folder = it.key();
        const QList<MediaItem> &list = it.value();
        html += "<div class=group>";
        html += QString("<div class=group-title>📁 %1 (%2개)</div>").arg(folder.toHtmlEscaped()).arg(list.size());
        html += "<div class=grid>";
        for (const auto &mi : list) {
            QString relEsc = mi.relPath.toHtmlEscaped();
            QString fileEsc = QFileInfo(mi.relPath).fileName().toHtmlEscaped();
            QString sizeStr = mi.size > 1024 * 1024
                ? QString::number(mi.size / 1024.0 / 1024.0, 'f', 1) + "M"
                : QString::number(mi.size / 1024) + "K";
            if (mi.type == "image") {
                html += QString("<div class=item data-name='%1' onclick='openLb(%2)'>")
                    .arg(fileEsc).arg(totalIdx);
                html += QString("<img loading=lazy src='%1' alt=''>").arg(relEsc);
                html += QString("<span class=badge>%1</span></div>").arg(sizeStr);
            } else {
                html += QString("<div class=item data-name='%1' onclick='openLb(%2)'>")
                    .arg(fileEsc).arg(totalIdx);
                html += QString("<video preload=metadata muted src='%1'></video>").arg(relEsc);
                html += QString("<span class=badge>🎬 %1</span></div>").arg(sizeStr);
            }
            totalIdx++;
        }
        html += "</div></div>";
    }

    // Lightbox + JS
    html += R"(<div class=lightbox id=lb onclick='closeLb()'><div id=lbInner></div></div><script>
        const items=[)";
    bool first = true;
    for (auto it = grouped.constBegin(); it != grouped.constEnd(); ++it) {
        for (const auto &mi : it.value()) {
            if (!first) html += ",";
            html += QString("{path:'%1',type:'%2'}")
                .arg(mi.relPath.toHtmlEscaped().replace("'", "\\'"))
                .arg(mi.type);
            first = false;
        }
    }
    html += R"(];
        let cur=0;
        function openLb(i){cur=i;render();document.getElementById('lb').classList.add('show');}
        function closeLb(){document.getElementById('lb').classList.remove('show');document.getElementById('lbInner').innerHTML='';}
        function render(){
            const m=items[cur];
            const el=document.getElementById('lbInner');
            if(m.type==='image') el.innerHTML='<img src="'+m.path+'">';
            else el.innerHTML='<video src="'+m.path+'" autoplay controls></video>';
        }
        document.addEventListener('keydown',e=>{
            if(!document.getElementById('lb').classList.contains('show'))return;
            if(e.key==='Escape')closeLb();
            if(e.key==='ArrowRight'){cur=(cur+1)%items.length;render();}
            if(e.key==='ArrowLeft'){cur=(cur-1+items.length)%items.length;render();}
        });
        function filterItems(){
            const q=document.getElementById('q').value.toLowerCase();
            document.querySelectorAll('.item').forEach(el=>{
                el.style.display=(el.dataset.name||'').toLowerCase().includes(q)?'':'none';
            });
        }
    </script></body></html>)";
    f.write(html.toUtf8());
    f.close();
    m_backend->log(QString("🖼 갤러리 생성: %1 (미디어 %2개)").arg(galleryPath).arg(items.size()),
                   "success", "bluesky");
}
