#include "RealChromeCrawler.h"
#include "core/PenBackend.h"
#include "core/Common.h"
#include <QCoreApplication>

#ifndef Q_OS_WIN
#include <signal.h>
#include <sys/types.h>
#endif

#include <QProcess>
#include <QThread>
#include <QWebSocket>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonValue>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>
#include <QStandardPaths>
#include <QTimer>
#include <QEventLoop>
#include <QCryptographicHash>
#include <QUrl>

RealChromeCrawler::RealChromeCrawler(PenBackend *backend, QObject *parent)
    : QObject(parent), m_backend(backend), m_nam(new QNetworkAccessManager(this))
{
}

RealChromeCrawler::~RealChromeCrawler()
{
    stop();
}

QString RealChromeCrawler::findChromeExecutable() const
{
    // 후보 경로 — 사용자가 어떤 Chromium 계열 브라우저든 깔려있을 가능성을 모두 검사
    QStringList candidates;
#ifdef Q_OS_MACOS
    // ★ 번들된 Chromium 최우선 (있으면)
    QString bundledChromium = QCoreApplication::applicationDirPath()
        + "/../Resources/chromium/Chromium.app/Contents/MacOS/Google Chrome for Testing";
    candidates << bundledChromium;
    candidates
        << "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
        << "/Applications/Google Chrome Beta.app/Contents/MacOS/Google Chrome Beta"
        << "/Applications/Google Chrome Canary.app/Contents/MacOS/Google Chrome Canary"
        << "/Applications/Chromium.app/Contents/MacOS/Chromium"
        << "/Applications/Microsoft Edge.app/Contents/MacOS/Microsoft Edge"
        << "/Applications/Brave Browser.app/Contents/MacOS/Brave Browser"
        << "/Applications/Arc.app/Contents/MacOS/Arc";
#elif defined(Q_OS_WIN)
    QString programFiles = qgetenv("ProgramFiles");
    QString programFilesX86 = qgetenv("ProgramFiles(x86)");
    QString localAppData = qgetenv("LOCALAPPDATA");
    if (!programFiles.isEmpty()) {
        candidates << programFiles + "\\Google\\Chrome\\Application\\chrome.exe"
                   << programFiles + "\\Microsoft\\Edge\\Application\\msedge.exe"
                   << programFiles + "\\BraveSoftware\\Brave-Browser\\Application\\brave.exe";
    }
    if (!programFilesX86.isEmpty()) {
        candidates << programFilesX86 + "\\Google\\Chrome\\Application\\chrome.exe"
                   << programFilesX86 + "\\Microsoft\\Edge\\Application\\msedge.exe";
    }
    if (!localAppData.isEmpty()) {
        candidates << localAppData + "\\Google\\Chrome\\Application\\chrome.exe"
                   << localAppData + "\\Microsoft\\Edge\\Application\\msedge.exe";
    }
#else
    candidates
        << "/usr/bin/google-chrome"
        << "/usr/bin/google-chrome-stable"
        << "/usr/bin/chromium"
        << "/usr/bin/chromium-browser"
        << "/usr/bin/microsoft-edge"
        << "/usr/bin/brave-browser";
#endif
    for (const QString &p : candidates) {
        if (QFile::exists(p)) return p;
    }
    return QString();
}

QString RealChromeCrawler::resolveDebuggerWsUrl(int port) const
{
    // ★ /json/list → 페이지 타겟 배열. 첫 번째 page 타겟의 webSocketDebuggerUrl을 사용.
    //   (이전: /json/version → 브라우저 endpoint를 줘서 Page.navigate가 안 먹힘)
    QNetworkAccessManager mgr;
    QNetworkRequest req(QUrl(QString("http://localhost:%1/json/list").arg(port)));
    QNetworkReply *reply = mgr.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QTimer::singleShot(5000, &loop, &QEventLoop::quit);
    loop.exec();
    if (reply->error() != QNetworkReply::NoError) {
        reply->deleteLater();
        return QString();
    }
    QByteArray body = reply->readAll();
    reply->deleteLater();
    QJsonArray arr = QJsonDocument::fromJson(body).array();
    // 첫 번째 page 타입 타겟 찾기
    for (const QJsonValue &v : arr) {
        QJsonObject t = v.toObject();
        if (t["type"].toString() == "page") {
            QString u = t["webSocketDebuggerUrl"].toString();
            if (!u.isEmpty()) return u;
        }
    }
    // page 타겟이 없으면 첫 번째 아무거나
    if (!arr.isEmpty()) {
        return arr[0].toObject()["webSocketDebuggerUrl"].toString();
    }
    return QString();
}

void RealChromeCrawler::start(std::function<void(bool)> done)
{
    if (m_ready) { if (done) done(true); return; }

    QString chrome = findChromeExecutable();
    if (chrome.isEmpty()) {
        if (m_backend) m_backend->log("Chrome/Edge/Brave 실행파일을 찾을 수 없습니다", "error", "crawl");
        if (done) done(false);
        return;
    }
    if (m_backend) m_backend->log(QString("Chrome 발견: %1").arg(chrome), "info", "crawl");

    // ★ 앱 전용 영구 프로필 — 임시 폴더에 매번 새로 만들지 않고 한 곳에 고정.
    //   m_userDataDir이 외부에서 setUserDataDir로 미리 설정됐으면 그 경로 사용 (병렬 trackKey별 분리).
    if (!m_useUserProfile && m_userDataDir.isEmpty()) {
        QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        m_userDataDir = appData + "/chrome_capture_profile";
        QDir().mkpath(m_userDataDir);
    }

    // ★ 이전 실행에서 좀비 Chrome이 같은 포트에 남아있을 수 있음 → 깨진 세션 재사용 방지.
    //   임시 프로필 모드에서는 항상 fresh start.
    if (!m_useUserProfile) {
#ifdef Q_OS_WIN
        // Windows: netstat → PID → taskkill
        QProcess netstat;
        netstat.start("netstat", {"-ano"});
        netstat.waitForFinished(3000);
        QString netOut = QString::fromUtf8(netstat.readAllStandardOutput());
        QString portStr = QString(":%1 ").arg(m_debugPort);
        for (const QString &line : netOut.split('\n')) {
            if (line.contains(portStr) && line.contains("LISTENING")) {
                QStringList parts = line.simplified().split(' ');
                if (!parts.isEmpty()) {
                    QString pid = parts.last();
                    QProcess::execute("taskkill", {"/PID", pid, "/F"});
                    if (m_backend) m_backend->log(QString("이전 Chrome 좀비 종료 (PID %1)").arg(pid), "info", "crawl");
                }
            }
        }
#else
        QProcess lsof;
        lsof.start("lsof", {"-ti", QString(":%1").arg(m_debugPort)});
        lsof.waitForFinished(2000);
        QString out = QString::fromUtf8(lsof.readAllStandardOutput()).trimmed();
        for (const QString &pidStr : out.split('\n', Qt::SkipEmptyParts)) {
            qint64 pid = pidStr.toLongLong();
            if (pid > 0) {
                ::kill(static_cast<pid_t>(pid), SIGTERM);
                if (m_backend) m_backend->log(QString("이전 Chrome 좀비 종료 (PID %1)").arg(pid), "info", "crawl");
            }
        }
#endif
        QThread::msleep(500);
    }

    // 사용자 프로필 모드일 때만 기존 세션 재사용 (사용자가 일부러 켜놓은 경우)
    QString existingWs;
    if (m_useUserProfile) {
        existingWs = resolveDebuggerWsUrl(m_debugPort);
    }
    if (existingWs.isEmpty()) {
        QStringList args;
        args << QString("--remote-debugging-port=%1").arg(m_debugPort);
        if (!m_useUserProfile) {
            args << "--user-data-dir=" + m_userDataDir;
        }
        // ★ --disable-blink-features=AutomationControlled 제거 — Chrome이 보안 경고 띄움.
        //   대신 onWsConnected에서 Page.addScriptToEvaluateOnNewDocument로 JS 단에서 webdriver 가림.
        args << "--no-first-run"
             << "--no-default-browser-check"
             // ★ 메모리 절감 — 8GB Mac에서 1.5GB 미만으로 작동하게
             << "--disable-features=Translate,OptimizationHints,MediaRouter,VizDisplayCompositor"
             << "--disable-background-networking"
             << "--disable-component-update"
             << "--disable-domain-reliability"
             << "--disable-sync"
             << "--metrics-recording-only"
             << "--mute-audio"
             << "--disable-backgrounding-occluded-windows"
             << "--disable-renderer-backgrounding"
             << "--memory-pressure-off"
             << "--js-flags=--max-old-space-size=384"
             << "--disk-cache-size=10485760"
             << "--media-cache-size=5242880"
             << "--aggressive-cache-discard"
             << "--process-per-site"
             << "--renderer-process-limit=1"
             << "--disable-gpu"
             << "--disable-software-rasterizer"
             << "--disable-accelerated-2d-canvas"
             << "--disable-accelerated-video-decode"
             << "--disable-gpu-compositing"
             << "--disable-dev-shm-usage"
             << "--no-zygote";

        // ★ SingleFile 번들 확장 로드 — 진짜 페이지 캡쳐 (모든 자원 인라인)
        QString sfDir = Common::bundledToolsDir() + "/singlefile_extension";
        if (!QFile::exists(sfDir + "/manifest.json")) {
            // dev fallback
            sfDir = QCoreApplication::applicationDirPath() +
                    "/../../resources/tools/singlefile_extension";
        }
        if (QFile::exists(sfDir + "/manifest.json")) {
            args << "--load-extension=" + sfDir;
            // 확장이 자동 비활성화되지 않게 강제 — 다른 모든 확장 차단 + 이놈만 활성
            args << "--disable-extensions-except=" + sfDir;
            // 개발 확장 경고 비활성화 (UX)
            args << "--silent-debugger-extension-api"
                 << "--disable-extensions-file-access-check";
            if (m_backend) m_backend->log(QString("SingleFile 확장 로드: %1").arg(sfDir), "info", "crawl");
        }

        m_chromeProc = new QProcess(this);
        m_chromeProc->setProgram(chrome);
        m_chromeProc->setArguments(args);
        m_chromeProc->start();
        if (!m_chromeProc->waitForStarted(5000)) {
            if (m_backend) m_backend->log("Chrome 프로세스 시작 실패", "error", "crawl");
            if (done) done(false);
            return;
        }
        if (m_backend) m_backend->log(QString("Chrome 시작 (포트 %1)").arg(m_debugPort), "success", "crawl");
    } else {
        if (m_backend) m_backend->log(QString("기존 Chrome CDP 세션에 연결 (포트 %1)").arg(m_debugPort),
                                       "info", "crawl");
    }

    // CDP 엔드포인트 polling — Chrome이 listen 시작할 때까지 최대 10초 대기
    int attempts = 0;
    QTimer *probe = new QTimer(this);
    probe->setInterval(500);
    QString *wsUrl = new QString();
    QObject::connect(probe, &QTimer::timeout, this, [this, probe, wsUrl, done, attempts]() mutable {
        attempts++;
        QString u = resolveDebuggerWsUrl(m_debugPort);
        if (!u.isEmpty()) {
            *wsUrl = u;
            probe->stop();
            probe->deleteLater();

            m_ws = new QWebSocket(QString(), QWebSocketProtocol::VersionLatest, this);
            connect(m_ws, &QWebSocket::connected, this, &RealChromeCrawler::onWsConnected);
            connect(m_ws, &QWebSocket::textMessageReceived, this, &RealChromeCrawler::onWsTextMessage);
            connect(m_ws, &QWebSocket::disconnected, this, [this](){
                m_ready = false;
                // ★ Chrome 종료/ws 끊김 시 pending 콜백들 error로 호출 (hang 방지)
                QJsonObject errObj;
                errObj["code"] = -32000;
                errObj["message"] = "WebSocket disconnected (Chrome closed?)";
                QJsonValue errVal(errObj);
                auto pending = m_pendingCmds;
                m_pendingCmds.clear();
                for (auto cb : pending) {
                    if (cb) cb(QJsonValue(), errVal);
                }
                emit disconnected();
            });

            // connected 후 done 콜백
            connect(m_ws, &QWebSocket::connected, this, [this, done, wsUrl]() {
                m_ready = true;
                if (m_backend) m_backend->log("Chrome CDP 연결됨", "success", "crawl");
                // ★ webdriver 자동화 시그널 숨김 — 모든 새 문서에 사전 주입되는 JS
                //   --disable-blink-features 플래그 대신 사용 (Chrome 보안 경고 회피)
                {
                    QJsonObject p;
                    p["source"] =
                        "Object.defineProperty(navigator, 'webdriver', {get: () => undefined});"
                        "Object.defineProperty(navigator, 'languages', {get: () => ['ko-KR', 'ko', 'en-US', 'en']});"
                        "Object.defineProperty(navigator, 'plugins', {get: () => [1, 2, 3, 4, 5]});";
                    sendCommand("Page.enable", QJsonObject(), nullptr);
                    sendCommand("Page.addScriptToEvaluateOnNewDocument", p, nullptr);
                }
                // Network 자동 활성화
                if (!m_responseSaveDir.isEmpty()) {
                    enableNetwork([this](bool){});
                }
                if (done) done(true);
                delete wsUrl;
            }, Qt::SingleShotConnection);

            m_ws->open(QUrl(*wsUrl));
            return;
        }
        if (attempts >= 20) {
            probe->stop();
            probe->deleteLater();
            delete wsUrl;
            if (m_backend) m_backend->log("Chrome CDP 엔드포인트 응답 없음 (10초 타임아웃)", "error", "crawl");
            if (done) done(false);
        }
    });
    probe->start();
}

void RealChromeCrawler::onWsConnected()
{
    // m_ready 등은 start()의 connected 람다에서 설정 — 여기선 no-op
}

void RealChromeCrawler::onWsTextMessage(const QString &msg)
{
    QJsonDocument doc = QJsonDocument::fromJson(msg.toUtf8());
    if (!doc.isObject()) return;
    QJsonObject obj = doc.object();

    // 응답: {id, result|error}
    if (obj.contains("id")) {
        int id = obj["id"].toInt();
        if (m_pendingCmds.contains(id)) {
            auto cb = m_pendingCmds.take(id);
            cb(obj.value("result"), obj.value("error"));
        }
        return;
    }
    // 이벤트: {method, params}
    if (obj.contains("method")) {
        handleEvent(obj["method"].toString(), obj["params"].toObject());
    }
}

void RealChromeCrawler::onWsError()
{
    if (m_backend && m_ws) {
        m_backend->log(QString("CDP WebSocket 에러: %1").arg(m_ws->errorString()), "error", "crawl");
    }
}

void RealChromeCrawler::setDownloadPath(const QString &path, std::function<void(bool)> done)
{
    if (!m_ready) { if (done) done(false); return; }
    QJsonObject params;
    params["behavior"] = "allow";
    params["downloadPath"] = path;
    sendCommand("Browser.setDownloadBehavior", params,
                [done](const QJsonValue &, const QJsonValue &err) {
                    if (done) done(err.isNull() || err.isUndefined());
                });
}

void RealChromeCrawler::dispatchKey(const QString &key, int modifiers, std::function<void()> done)
{
    if (!m_ready) { if (done) done(); return; }
    // ★ Chrome 확장의 chrome.commands까지 도달하려면 windowsVirtualKeyCode + code 필요.
    //   y/Y 만 우선 지원 (SingleFile 단축키용)
    QString upper = key.toUpper();
    int vk = 0;
    QString code;
    if (upper.length() == 1 && upper[0].isLetter()) {
        vk = 'A' + (upper[0].toLatin1() - 'A');  // 'Y' → 0x59
        code = QString("Key%1").arg(upper);
    }
    auto buildEvent = [&](const QString &type) {
        QJsonObject e;
        e["type"] = type;  // "rawKeyDown" / "keyUp"
        e["modifiers"] = modifiers;
        e["key"] = (modifiers & 8) ? upper : key;  // Shift면 대문자
        e["code"] = code;
        e["windowsVirtualKeyCode"] = vk;
        e["nativeVirtualKeyCode"] = vk;
        e["isKeypad"] = false;
        e["autoRepeat"] = false;
        return e;
    };
    sendCommand("Input.dispatchKeyEvent", buildEvent("rawKeyDown"),
        [this, buildEvent, done](const QJsonValue &, const QJsonValue &) {
            sendCommand("Input.dispatchKeyEvent", buildEvent("keyUp"),
                [done](const QJsonValue &, const QJsonValue &) {
                    if (done) done();
                });
        });
}

void RealChromeCrawler::setCookies(const QJsonArray &cookies, std::function<void(bool)> done)
{
    if (!m_ready) { if (done) done(false); return; }
    if (cookies.isEmpty()) { if (done) done(true); return; }
    // ★ 호환성 보강: 호출자가 url 안 보낸 경우 domain → url 자동 도출
    //    Network.setCookie 일부 Chrome 버전이 domain만 있으면 cookie 거부
    QJsonArray patched;
    for (const QJsonValue &v : cookies) {
        QJsonObject ck = v.toObject();
        if (!ck.contains("url") && ck.contains("domain")) {
            QString d = ck["domain"].toString();
            QString p = ck.contains("path") ? ck["path"].toString() : "/";
            if (p.isEmpty()) p = "/";
            QString cleanD = d.startsWith(".") ? d.mid(1) : d;
            bool sec = ck["secure"].toBool();
            ck["url"] = QString(sec ? "https://" : "http://") + cleanD + p;
            if (!ck.contains("path")) ck["path"] = p;
        }
        if (!ck.contains("sameSite")) ck["sameSite"] = "None";
        patched.append(ck);
    }
    // Network 도메인 enable 후 setCookie (single) 반복 호출.
    //   Network.setCookies (plural)는 일부 Chrome 버전에서 미지원 → 호환성을 위해 singular 사용.
    sendCommand("Network.enable", QJsonObject(), [this, cookies = patched, done](const QJsonValue &, const QJsonValue &) {
        auto remaining = std::make_shared<int>(cookies.size());
        auto okCount = std::make_shared<int>(0);
        for (const QJsonValue &v : cookies) {
            QJsonObject ck = v.toObject();
            sendCommand("Network.setCookie", ck,
                        [this, remaining, okCount, total = cookies.size(), done](const QJsonValue &result, const QJsonValue &err) {
                            bool ok = err.isNull() || err.isUndefined() || (err.isObject() && err.toObject().isEmpty());
                            if (ok) (*okCount)++;
                            (*remaining)--;
                            if (*remaining == 0) {
                                if (m_backend) m_backend->log(QString("[CDP] 쿠키 %1/%2 설정됨").arg(*okCount).arg(total), "info", "twitter");
                                if (done) done(*okCount > 0);
                            }
                        });
        }
    });
}

int RealChromeCrawler::sendCommand(const QString &method, const QJsonObject &params,
                                    std::function<void(const QJsonValue &, const QJsonValue &)> cb)
{
    if (!m_ws || !m_ready) return -1;
    int id = m_nextCmdId++;
    QJsonObject cmd;
    cmd["id"] = id;
    cmd["method"] = method;
    cmd["params"] = params;
    if (cb) m_pendingCmds.insert(id, cb);
    m_ws->sendTextMessage(QString::fromUtf8(QJsonDocument(cmd).toJson(QJsonDocument::Compact)));
    return id;
}

void RealChromeCrawler::handleEvent(const QString &method, const QJsonObject &params)
{
    if (method == "Network.requestWillBeSent") {
        QString reqId = params["requestId"].toString();
        QJsonObject req = params["request"].toObject();
        QJsonObject meta;
        meta["url"] = req["url"];
        meta["method"] = req["method"];
        meta["ts"] = QDateTime::currentMSecsSinceEpoch();
        m_requestMeta.insert(reqId, meta);
    }
    else if (method == "Network.responseReceived") {
        QString reqId = params["requestId"].toString();
        QJsonObject resp = params["response"].toObject();
        QString mime = resp["mimeType"].toString().toLower();
        QString url = resp["url"].toString();

        // JSON 응답만 자동 저장 (이미지/HTML은 너무 큼)
        bool isJson = mime.contains("json") || mime.contains("javascript");
        if (m_responseSaveDir.isEmpty() || !isJson) {
            emit responseReceived(resp);
            return;
        }

        // 본문 가져와서 저장
        getResponseBody(reqId, [this, url, resp](const QString &body, const QString &mt) {
            if (body.isEmpty()) return;
            QDir().mkpath(m_responseSaveDir);
            QString hash = QCryptographicHash::hash(
                (url + QString::number(QDateTime::currentMSecsSinceEpoch())).toUtf8(),
                QCryptographicHash::Md5).toHex().left(16);
            // x.com GraphQL 엔드포인트면 이름을 살림
            QString endpoint;
            QRegularExpression gqlRe(R"(/graphql/[^/]+/(\w+))");
            auto gm = gqlRe.match(url);
            if (gm.hasMatch()) endpoint = gm.captured(1);
            QString fname = endpoint.isEmpty() ? hash + ".json" : endpoint + "_" + hash + ".json";
            QString path = m_responseSaveDir + "/" + fname;
            QFile f(path);
            if (!f.open(QIODevice::WriteOnly)) return;
            QJsonObject wrapped;
            wrapped["url"] = url;
            wrapped["mimeType"] = mt;
            wrapped["status"] = resp["status"];
            wrapped["capturedAt"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
            QJsonDocument bodyDoc = QJsonDocument::fromJson(body.toUtf8());
            if (bodyDoc.isObject()) wrapped["body"] = bodyDoc.object();
            else if (bodyDoc.isArray()) wrapped["body"] = bodyDoc.array();
            else wrapped["bodyRaw"] = body;
            f.write(QJsonDocument(wrapped).toJson(QJsonDocument::Indented));
            f.close();
            m_capturedRespFiles.append(path);
            emit networkResponseSaved(path);
        });

        emit responseReceived(resp);
    }
}

void RealChromeCrawler::navigate(const QString &url, std::function<void(bool)> done)
{
    if (!m_ready) {
        if (m_backend) m_backend->log("navigate: Chrome 준비 안 됨 (CDP 연결 실패)", "error", "crawl");
        if (done) done(false);
        return;
    }
    // ★ URL 자동 보정 — scheme 없으면 https:// 추가 (사용자가 example.com 만 입력해도 동작)
    QString fixedUrl = url.trimmed();
    if (fixedUrl.isEmpty()) {
        if (m_backend) m_backend->log("navigate: URL 이 비어있음", "error", "crawl");
        if (done) done(false);
        return;
    }
    if (!fixedUrl.contains("://") && !fixedUrl.startsWith("about:") && !fixedUrl.startsWith("data:")) {
        fixedUrl = "https://" + fixedUrl;
        if (m_backend) m_backend->log(QString("URL 자동 보정: %1 → %2").arg(url, fixedUrl), "info", "crawl");
    }

    QJsonObject params;
    params["url"] = fixedUrl;
    sendCommand("Page.enable", QJsonObject(), nullptr);
    sendCommand("Page.navigate", params, [this, done, fixedUrl](const QJsonValue &result, const QJsonValue &err) {
        bool ok = err.isNull() || err.toObject().isEmpty();
        if (!ok) {
            // CDP 에러 상세 출력
            QJsonObject e = err.toObject();
            QString msg = e["message"].toString();
            int code = e["code"].toInt();
            if (m_backend) m_backend->log(QString("navigate 실패 (%1): %2 — URL: %3")
                .arg(code).arg(msg, fixedUrl), "error", "crawl");
        } else {
            // result 에 errorText 있으면 그것도 실패
            QJsonObject r = result.toObject();
            QString errText = r["errorText"].toString();
            if (!errText.isEmpty() && errText != "OK") {
                if (m_backend) m_backend->log(QString("navigate 실패 — %1 (URL: %2)").arg(errText, fixedUrl), "error", "crawl");
                ok = false;
            }
        }
        if (done) done(ok);
    });
}

void RealChromeCrawler::evaluate(const QString &expr, std::function<void(const QJsonValue &)> done)
{
    if (!m_ready) { if (done) done(QJsonValue()); return; }
    QJsonObject params;
    params["expression"] = expr;
    params["returnByValue"] = true;
    params["awaitPromise"] = true;
    sendCommand("Runtime.evaluate", params, [done](const QJsonValue &result, const QJsonValue &) {
        if (!done) return;
        QJsonObject obj = result.toObject();
        QJsonObject inner = obj["result"].toObject();
        done(inner.value("value"));
    });
}

void RealChromeCrawler::getRenderedHtml(std::function<void(const QString &)> done)
{
    evaluate("document.documentElement.outerHTML",
        [done](const QJsonValue &v) {
            if (done) done(v.toString());
        });
}

void RealChromeCrawler::enableNetwork(std::function<void(bool)> done)
{
    sendCommand("Network.enable", QJsonObject(), [done](const QJsonValue &, const QJsonValue &err) {
        if (done) done(err.isNull() || err.toObject().isEmpty());
    });
}

void RealChromeCrawler::getResponseBody(const QString &requestId,
                                         std::function<void(const QString &, const QString &)> done)
{
    QJsonObject params;
    params["requestId"] = requestId;
    sendCommand("Network.getResponseBody", params,
        [done](const QJsonValue &result, const QJsonValue &err) {
            if (!done) return;
            if (!err.isNull() && !err.toObject().isEmpty()) {
                done(QString(), QString());
                return;
            }
            QJsonObject obj = result.toObject();
            QString body = obj["body"].toString();
            bool b64 = obj["base64Encoded"].toBool();
            if (b64) body = QString::fromUtf8(QByteArray::fromBase64(body.toUtf8()));
            done(body, QString());  // mimeType는 호출자가 이미 알고 있음
        });
}

void RealChromeCrawler::scrollToBottom(std::function<void()> done)
{
    evaluate("window.scrollTo(0, document.body.scrollHeight); 1",
        [done](const QJsonValue &) {
            if (done) done();
        });
}

void RealChromeCrawler::captureScreenshot(std::function<void(const QByteArray &)> done)
{
    QJsonObject params;
    params["format"] = "png";
    sendCommand("Page.captureScreenshot", params,
        [done](const QJsonValue &result, const QJsonValue &) {
            if (!done) return;
            QString b64 = result.toObject()["data"].toString();
            done(QByteArray::fromBase64(b64.toUtf8()));
        });
}

void RealChromeCrawler::stop()
{
    if (m_ws) {
        m_ws->close();
        m_ws->deleteLater();
        m_ws = nullptr;
    }
    if (m_chromeProc) {
        if (m_chromeProc->state() != QProcess::NotRunning) {
            m_chromeProc->terminate();
            if (!m_chromeProc->waitForFinished(3000)) m_chromeProc->kill();
        }
        m_chromeProc->deleteLater();
        m_chromeProc = nullptr;
    }
    m_ready = false;
    // 임시 프로필은 그대로 남겨둠 (다음 실행에서 캐시 활용 가능)
}
