#include "RealChromeCrawler.h"
#include "core/HanishikiBackend.h"
#include "core/Common.h"
#include "core/Config.h"
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
#include <QDirIterator>
#include <QDir>
#include <QVersionNumber>
#include <algorithm>
#include <QDateTime>
#include <QStandardPaths>
#include <QTimer>
#include <QEventLoop>
#include <QCryptographicHash>
#include <QUrl>
#include <QGuiApplication>
#include <QWindow>

RealChromeCrawler::RealChromeCrawler(HanishikiBackend *backend, QObject *parent)
    : QObject(parent), m_backend(backend), m_nam(new QNetworkAccessManager(this))
{
    m_debugPort = Common::capturePortBase();   // 기본 9223 — 격리 사본은 다른 포트대(Common 설명)
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
    // ★ 번들된 Chromium 최우선 — 앱 자체 동봉 (사용자 시스템 Chrome 의존성 제거).
    //   유니버설 빌드: arch 슬라이스별로 자기 arch Chromium 을 고른다 (Chrome for Testing 은 유니버설 없음).
    const QString resDir = QCoreApplication::applicationDirPath() + "/../Resources/";
    const QString chromeExe = "/Contents/MacOS/Google Chrome for Testing";
#if defined(__aarch64__)
    candidates << resDir + "chromium_arm64/Chromium.app" + chromeExe;
#else
    candidates << resDir + "chromium_x86_64/Chromium.app" + chromeExe;
#endif
    candidates << resDir + "chromium/Chromium.app" + chromeExe;   // 호환 폴백 (단일 arch 번들)
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
    // ★ 요즘 Edge 배치 — ...\Microsoft\EdgeCore\<버전>\msedge.exe
    //   Edge 를 Application 폴더가 아니라 버전별 EdgeCore 폴더에 두는 설치가 있다.
    //   사용자 기계가 그렇다(152.0.4191.19). 위 고정 경로만 보면 Chrome 도 Edge 도
    //   못 찾아 findChromeExecutable() 이 빈 값을 주고, 그러면 '진짜 페이지 캡쳐' 가
    //   통째로 동작하지 않는다(트위터는 이 토글이 기본 켜짐이다).
    //   EdgeCore 의 msedge.exe 는 CDP 브라우저로 멀쩡히 돌아간다 — 직접 띄워
    //   /json/version 이 "Edg/152.0.4191.19" 로 답하는 것을 확인했다.
    for (const QString &base : { programFilesX86, programFiles, localAppData }) {
        if (base.isEmpty()) continue;
        QDir core(base + "\\Microsoft\\EdgeCore");
        if (!core.exists()) continue;
        QStringList vers = core.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        std::sort(vers.begin(), vers.end(), [](const QString &a, const QString &b) {
            return QVersionNumber::fromString(a) > QVersionNumber::fromString(b);
        });
        for (const QString &v : vers)
            candidates << core.absolutePath() + "/" + v + "/msedge.exe";
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

    // ★ 캡쳐 Chromium 은 깨끗한 임시 프로필로 시작.
    //   사용자 Chrome 프로필 복사 안 함 — 토큰은 설정 탭의 [자동 추출] 버튼으로 별도 처리.
    //   CDP Network.setCookie 로 필요한 cookie 만 inject (auth_token, sessionid 등).

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
        // ★ 포트를 쥔 프로세스는 '우리 캡처 프로필' 을 쓰는 Chrome 일 때만 끈다.
        //   예전엔 주인을 묻지 않고 그 포트의 프로세스를 모두 껐다. 같은 맥에서 앱이 둘(사용자 앱 · 시험 사본,
        //   또는 옛 판)이 돌면 서로의 캡처 Chrome 을 '좀비' 로 보고 죽였다. 다른 프로필의 것이면 그대로 두고
        //   알린다 — 그 경우 이 Chrome 은 포트를 못 얻어 시작에 실패하지만, 남의 수집을 끊지는 않는다.
        QProcess lsof;
        lsof.start("lsof", {"-ti", QString(":%1").arg(m_debugPort)});
        lsof.waitForFinished(2000);
        QString out = QString::fromUtf8(lsof.readAllStandardOutput()).trimmed();
        for (const QString &pidStr : out.split('\n', Qt::SkipEmptyParts)) {
            qint64 pid = pidStr.toLongLong();
            if (pid <= 0) continue;
            QProcess psq;
            psq.start("/bin/ps", {"-o", "command=", "-p", QString::number(pid)});
            psq.waitForFinished(2000);
            const QString cmdline = QString::fromUtf8(psq.readAllStandardOutput());
            if (!m_userDataDir.isEmpty() && cmdline.contains(m_userDataDir)) {
                ::kill(static_cast<pid_t>(pid), SIGTERM);
                if (m_backend) m_backend->log(QString("이전 Chrome 좀비 종료 (PID %1)").arg(pid), "info", "crawl");
            } else if (m_backend) {
                m_backend->log(QString("포트 %1 을 다른 프로그램(PID %2)이 쓰고 있어 끄지 않았습니다 — "
                                       "같은 맥에서 앱을 둘 띄웠다면 하나를 닫으십시오").arg(m_debugPort).arg(pid),
                               "warning", "crawl");
            }
        }
        // ★ 포트뿐 아니라 '같은 캡쳐 프로필'을 쓰는 잔존 Chrome 도 정리.
        //   (포트가 안 열린 좀비가 프로필을 쥐고 있으면, 새 Chrome 이 그 인스턴스로 핸드오프되며 즉시 종료 →
        //    캡쳐 시 "로그인 사전 navigate 실패" 가 트윗마다 반복됨). 개인 Chrome 은 경로가 달라 안 건드림.
        if (!m_userDataDir.isEmpty()) {
            QProcess pg;
            pg.start("pgrep", {"-f", m_userDataDir});
            pg.waitForFinished(2000);
            const QString pgo = QString::fromUtf8(pg.readAllStandardOutput()).trimmed();
            for (const QString &pidStr : pgo.split('\n', Qt::SkipEmptyParts)) {
                qint64 pid = pidStr.toLongLong();
                if (pid > 0) {
                    // ★ pgrep -f 는 부분 일치다. 기본 프로필(…/chrome_capture_profile)로 찾으면 트랙별·출구별
                    //   (…/chrome_capture_profile_<b64>) Chrome 까지 걸려, 순차 캡쳐 Chrome(또는 m_realChrome)이 뜰 때
                    //   병렬 트랙들의 캡쳐 Chrome 을 모두 죽였다. --user-data-dir 값이 정확히 같을 때만 끈다.
                    QProcess psq;
                    psq.start("/bin/ps", {"-o", "command=", "-p", QString::number(pid)});
                    psq.waitForFinished(2000);
                    const QString cl = QString::fromUtf8(psq.readAllStandardOutput()).trimmed();
                    const QString key = QStringLiteral("--user-data-dir=") + m_userDataDir;
                    const int at = cl.indexOf(key);
                    if (at < 0 || (at + key.size() < cl.size() && cl.at(at + key.size()) != QLatin1Char(' ')))
                        continue;
                    ::kill(static_cast<pid_t>(pid), SIGTERM);
                    if (m_backend) m_backend->log(QString("이전 Chrome(프로필) 좀비 종료 (PID %1)").arg(pid), "info", "crawl");
                }
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
            // ★ 크로미움은 SOCKS5 의 사용자/비밀번호 인증을 지원하지 않는다.
            //   그래서 인증이 붙은 프록시를 켜면 여기만 진짜 IP 로 새어 나갔다.
            //   앱이 띄운 로컬 중계기(인증 없음 → 상위에 인증)를 가리킨다.
            //   자격증명이 크로미움 명령줄에 실리지 않는 이점도 있다.
            //   ★ 출구가 있어야 하는데 중계 주소를 못 얻으면(파이썬 없음·중계기가 죽음) 띄우지 않는다.
            //     예전엔 그냥 --proxy-server 없이 띄워 그 Chrome 만 이 컴퓨터의 회선으로 나갔다.
            {
                const bool required = m_proxyPinned ? m_proxyRequired : Common::proxyEnabled();
                const QString relay = m_proxyPinned ? m_proxyServer : Common::proxyLocalRelayUrl();
                if (required && relay.isEmpty()) {
                    if (m_backend)
                        m_backend->log("프록시 중계기를 띄우지 못해 Chrome 을 열지 않습니다 — 직접 연결로 새지 않게 멈춥니다",
                                       "error", "crawl");
                    if (done) done(false);
                    return;
                }
                if (!relay.isEmpty()) args << "--proxy-server=" + relay;
                m_proxyServerUsed = relay;
            }
            args << "--user-data-dir=" + m_userDataDir;
            // ★ 시크릿 모드 — 임시 프로필이라도 incognito 윈도우로 띄움. 흔적 안 남음.
            //   CDP Network.setCookie 로 주입하는 토큰 (auth_token/ct0/sessionid 등) 은 정상 작동.
            args << "--incognito";
        }
        // ★ --disable-blink-features=AutomationControlled 제거 — Chrome이 보안 경고 띄움.
        //   대신 onWsConnected에서 Page.addScriptToEvaluateOnNewDocument로 JS 단에서 webdriver 가림.
        // ★ --disable-features 는 한 번만 넘긴다(아래 #endif 뒤). 크로미움은 같은 스위치가 여러 번 오면
        //   마지막 것만 쓴다 — 예전엔 셋(윈도우 갈래는 일곱)으로 나뉘어 마지막 줄만 먹고 나머지는 버려졌다.
        //   ※ 합치면서 둘을 뺐다. 그동안 '안 먹던' 덕에 해가 없었던 것들이다.
        //     · IsolateOrigins · site-per-process — 사이트 격리(보안)를 끈다. 아래 --site-per-process 와 정반대다.
        //     · WebRtcHideLocalIpsWithMdns · WebRTC — 내부 IP 를 감춰 주는 장치를 끈다(주석의 'IP 노출 방지' 와
        //       정반대). WebRTC 로 진짜 IP 가 새는 것은 --force-webrtc-ip-handling-policy 로 막는다.
        QStringList disabledFeatures = {
            QStringLiteral("Translate"), QStringLiteral("OptimizationHints"), QStringLiteral("MediaRouter"),
            QStringLiteral("GlobalMediaControls"),                       // 8GB Mac OOM 방지 — 쓰지 않는 기능
            QStringLiteral("AutofillServerCommunication"), QStringLiteral("OptimizationGuideModelDownloading")};
        args << "--no-first-run"
             << "--no-default-browser-check"
             << "--disable-background-networking"
             << "--disable-component-update"
             << "--disable-domain-reliability"
             << "--disable-sync"
             << "--disable-extensions-http-throttling"
             << "--metrics-recording-only"
             << "--mute-audio"
             << "--disable-backgrounding-occluded-windows"
             << "--disable-renderer-backgrounding"
             // ★ 최소화 중 타이머를 1초 단위로 묶지 않게 — 윈도우처럼 창을 내려 두고 수집하므로 (setWindowMinimized).
             << "--disable-background-timer-throttling"
             // ★ 메모리 설정이 정반대로 되어 있었다. 저용량 기계를 배려한 의도였지만
             //   결과는 "메모리 부족" 으로 페이지가 죽는 것이었다.
             //     · max-old-space-size=384 : V8 힙을 384MB 로 묶었다. 트위터·인스타
             //       타임라인 같은 실제 페이지에는 너무 작아서, 아끼는 게 아니라
             //       한계를 넘겨 OOM 을 '만들어 내는' 값이었다.
             //     · memory-pressure-off    : 시스템이 빠듯해도 캐시를 비우지 말라는
             //       뜻이다. 메모리가 모자란 기계에서 켤 설정이 아니다. 정확히 반대다.
             //   → 힙은 실제 페이지가 돌 만큼 주고(1024MB), 메모리 압박에는 응하게 한다.
             //     디스크·미디어 캐시를 작게 잡고 적극적으로 버리는 것은 그대로 둔다 —
             //     그쪽이 저용량 기계에 실제로 도움이 되는 설정이다.
             << "--js-flags=--max-old-space-size=1024"
             << "--disk-cache-size=10485760"
             << "--media-cache-size=5242880"
             << "--aggressive-cache-discard"
             // ★ 페이지가 스스로 새 탭·창을 열지 못하게 한다. 크롤러는 탭 하나를
             //   재사용하는데, 페이지가 팝업이나 target=_blank 로 탭을 늘리면
             //   그만큼 메모리를 먹고 캡처 대상도 헷갈린다.
             << "--block-new-web-contents";

        // ★ 사용자 임시 디스크 시스템 — Chrome disk cache 도 거기에. /tmp 사용 X.
        //   backend 의 Config 에서 tempDir 받아서 cache dir 강제 set.
        if (m_backend && m_backend->config()) {
            QString userTemp = Common::resolveTempBase(m_backend->config()->tempDir());
            if (!userTemp.isEmpty()) {
                QString chromeCache = userTemp + "/chrome_cache";
                QDir().mkpath(chromeCache);
                args << ("--disk-cache-dir=" + chromeCache);
            }
        }

        args
             // ★ 보안 강화 layer
             << "--site-per-process"                                    // Site isolation (Spectre 방어)
             << "--enable-strict-mixed-content-checking"                // HTTPS 안 HTTP 차단
             << "--block-third-party-cookies"                           // 3rd party 쿠키 차단 (추적 방지)
             // WebRTC 가 프록시 밖(직접 UDP)으로 나가지 않게 — 계정 출구를 쓸 때 진짜 IP 가 새는 길이다
             << "--force-webrtc-ip-handling-policy=disable_non_proxied_udp"
             << "--disable-background-mode"                             // 백그라운드 실행 차단
             << "--disable-default-apps"
             << "--disable-translate"
             << "--no-default-browser-check"
             << "--no-first-run"
             << "--disable-sync";
#ifdef Q_OS_WIN
        // ★ Windows 전용 추가 보안 — macOS 보다 공격 표면이 넓음
        args << "--win-job-object"                              // Windows Job Object 격리 강화
             << "--enforce-strict-secure-origin-for-secure-frames"
             << "--restrict-runtime-allocation"                 // ASLR 강화
             << "--enable-features=NetworkServiceSandbox"       // Network 서비스 sandbox
             << "--block-insecure-private-network-requests";    // 내부망 비보안 요청 차단
        disabledFeatures << QStringLiteral("NtlmV1")            // NTLMv1 인증 차단 (legacy 취약)
                         << QStringLiteral("AsyncDns")          // mDNS 응답 IP 노출 방지
                         << QStringLiteral("ChromeWhatsNewUI")
                         << QStringLiteral("DnsOverHttpsUpgrade");   // DoH 자동 upgrade 차단 (MITM 우회 방지)
#endif
        args << (QStringLiteral("--disable-features=") + disabledFeatures.join(QLatin1Char(',')));
        args << "--disable-gpu"
             << "--disable-software-rasterizer"
             << "--disable-accelerated-2d-canvas"
             << "--disable-accelerated-video-decode"
             << "--disable-gpu-compositing";

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
#ifdef Q_OS_MACOS
        // ★ 맥은 새로 뜬 Chrome 이 스스로 앞으로 나와 포커스를 가져간다 (윈도우는 SW_SHOWMINNOACTIVE 로 막는다).
        //   우리 앱이 앞에 있었다면 그 창을 기억해 두고, CDP 로 최소화한 뒤 돌려준다 (아래 connected 람다).
        m_refocusAfterLaunch = (QGuiApplication::applicationState() == Qt::ApplicationActive)
                                   ? QGuiApplication::focusWindow() : nullptr;
#endif
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
                // ★ Chrome process가 죽거나 ws 끊기면 m_pendingCmds 콜백이 영원히 hang.
                //   모든 대기 중 콜백을 error로 호출 → 호출자(captureRealPageCDP 등)가 자연 종료.
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
                    // ★ SingleFile 은 페이지 안(main world)에서 fetch()·DOMParser 로 자원을 모은다.
                    //   페이지 CSP 의 connect-src 가 CDN 을 막으면 그림이 'data:,' 로 비고,
                    //   Trusted Types(require-trusted-types-for 'script' — 유튜브)면 DOMParser 가 막혀 getPageData 가 통째로 실패했다.
                    //   캡쳐 전용 탭이므로 CSP 를 끈다(다음 navigate 부터 적용).
                    //   ★ 사용자 본인 프로필(setUseUserProfile(true) — 실제 Chrome 모드의 '내 프로필')이면 그 탭은
                    //     캡쳐 전용이 아니고 SingleFile 도 돌지 않는다 — CSP 를 끄지 않는다.
                    if (!m_useUserProfile) {
                        QJsonObject bp;
                        bp["enabled"] = true;
                        sendCommand("Page.setBypassCSP", bp, nullptr);
                    }
                }
                // Network 자동 활성화
                if (!m_responseSaveDir.isEmpty()) {
                    enableNetwork([this](bool){});
                }
                // ★ 윈도우처럼 최소화로 수집한다 — 화면을 가리지 않게.
                //   윈도우는 CreateProcess 의 SW_SHOWMINNOACTIVE 로 처음부터 최소화로 띄운다.
                //   맥은 그런 실행 옵션이 없어 CDP 가 붙자마자 내린다 (잠깐 보였다 내려간다).
                //   직접 띄운 Chrome(m_chromeProc)만 — 이미 떠 있던 사용자 Chrome 에 붙은 경우는 그 창을 건드리지 않는다.
                //   ※ 맥에서 최소화된 창은 '가려진(occluded)' 창으로 취급되고, --disable-backgrounding-occluded-windows
                //     덕분에 페이지는 visible 로 남는다 — 스크롤·SingleFile 캡쳐는 그대로 돈다.
                //     (그리기만 멈춰 rAF/IntersectionObserver 가 약 1초 간격으로 느려질 수 있다.)
                if (m_startMinimized && m_chromeProc) {
                    QPointer<QWindow> back = m_refocusAfterLaunch;
                    m_refocusAfterLaunch = nullptr;
                    setWindowMinimized(true, [this, back](bool ok) {
                        if (!ok) {
                            if (m_backend) m_backend->log("Chrome 창 최소화 실패 — 창이 보이는 채로 수집합니다", "warning", "crawl");
                            return;
                        }
                        // 띄우기 전 우리 앱이 앞에 있었다면 포커스를 돌려준다 (윈도우의 '포커스도 주지 않는다').
                        if (back) { back->raise(); back->requestActivate(); }
                    });
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

void RealChromeCrawler::setWindowMinimized(bool minimized, std::function<void(bool)> done)
{
    if (!m_ready) { if (done) done(false); return; }
    // targetId 를 비우면 이 세션(우리가 붙은 페이지 타겟)이 든 창을 돌려준다.
    const int sent = sendCommand("Browser.getWindowForTarget", QJsonObject(),
        [this, minimized, done](const QJsonValue &result, const QJsonValue &err) {
            const int windowId = result.toObject().value("windowId").toInt(-1);
            if (!err.toObject().isEmpty() || windowId < 0) {
                if (done) done(false);
                return;
            }
            QJsonObject bounds;
            // 'minimized' 는 left/top/width/height 와 같이 줄 수 없다 — 상태만 준다.
            // 'normal' 은 최소화된 창을 원래 크기·자리로 되돌린다 (Chrome 쪽 Restore()).
            bounds["windowState"] = minimized ? "minimized" : "normal";
            QJsonObject p;
            p["windowId"] = windowId;
            p["bounds"] = bounds;
            const int sent2 = sendCommand("Browser.setWindowBounds", p,
                [this, minimized, done](const QJsonValue &, const QJsonValue &err2) {
                    const bool ok = err2.toObject().isEmpty();
                    if (!ok || minimized) { if (done) done(ok); return; }
                    // 올렸으면 앞으로 — 로그인할 창이 다른 창 뒤에 묻히지 않게 (앱 활성화 포함).
                    const int sent3 = sendCommand("Page.bringToFront", QJsonObject(),
                        [done](const QJsonValue &, const QJsonValue &) { if (done) done(true); });
                    if (sent3 < 0 && done) done(true);
                });
            if (sent2 < 0 && done) done(false);
        });
    if (sent < 0 && done) done(false);
}

void RealChromeCrawler::setBypassCsp(bool on)
{
    if (!m_ready) return;
    QJsonObject bp;
    bp["enabled"] = on;
    sendCommand("Page.setBypassCSP", bp, nullptr);
}

void RealChromeCrawler::reloadPage()
{
    if (!m_ready) return;
    sendCommand("Page.reload", QJsonObject(), nullptr);
}

void RealChromeCrawler::setCorsRelax(bool on, std::function<void()> done)
{
    if (!m_ready) { if (done) done(); return; }
    QJsonObject params;
    if (on) {
        QJsonObject pat;
        pat["urlPattern"] = "*";
        pat["resourceType"] = "Fetch";
        pat["requestStage"] = "Response";
        params["patterns"] = QJsonArray{pat};
    }
    const int id = sendCommand(on ? "Fetch.enable" : "Fetch.disable", params,
                               [done](const QJsonValue &, const QJsonValue &) { if (done) done(); });
    if (id < 0 && done) done();
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
    // Network 도메인 enable 후 setCookie (single) 반복 호출.
    //   Network.setCookies (plural)는 일부 Chrome 버전에서 미지원 → 호환성을 위해 singular 사용.
    sendCommand("Network.enable", QJsonObject(), [this, cookies, done](const QJsonValue &, const QJsonValue &) {
        auto remaining = std::make_shared<int>(cookies.size());
        auto okCount = std::make_shared<int>(0);
        for (const QJsonValue &v : cookies) {
            QJsonObject ck = v.toObject();
            // ★ 로그인 세션 쿠키는 사이트가 원래 HttpOnly 로 준다 — 우리가 넣을 때도 같게 해 둔다.
            //   안 그러면 캡쳐 Chrome 안의 페이지 스크립트가 document.cookie 로 읽을 수 있다(캡쳐 때는 CSP 도 꺼 둔다).
            //   ct0·csrftoken 처럼 사이트 스크립트가 직접 읽는 쿠키는 그대로 둔다(막으면 사이트가 로그인 상태로 안 돈다).
            static const QSet<QString> kSessionCookies = {
                QStringLiteral("auth_token"), QStringLiteral("kdt"), QStringLiteral("sessionid"),
                QStringLiteral("PHPSESSID"), QStringLiteral("FANBOXSESSID"), QStringLiteral("user_session"),
                QStringLiteral("user_session_secure")};
            //   ※ 부르는 쪽(HanishikiBackend 의 QNetworkCookie→JSON 변환 세 곳, 실제 Chrome 모드 pushCookie)은 httpOnly 를
            //     늘 넣어 보낸다(QNetworkCookie::isHttpOnly() 기본 false, pushCookie 는 false 고정). '없을 때만' 으로
            //     거르면 한 번도 켜지지 않는다 — 이름이 맞으면 덮어쓴다.
            if (kSessionCookies.contains(ck.value(QStringLiteral("name")).toString()))
                ck[QStringLiteral("httpOnly")] = true;
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
    if (method == "Fetch.requestPaused") {   // setCorsRelax(true) 동안만 온다
        const QString rid = params["requestId"].toString();
        QJsonObject p;
        p["requestId"] = rid;
        if (params.contains("responseErrorReason")) {   // 네트워크 오류 — 그대로 실패시킨다
            p["errorReason"] = params["responseErrorReason"].toString();
            sendCommand("Fetch.failRequest", p, nullptr);
            return;
        }
        QString origin;
        const QJsonObject reqHdrs = params["request"].toObject()["headers"].toObject();
        for (auto it = reqHdrs.begin(); it != reqHdrs.end(); ++it)
            if (it.key().compare(QLatin1String("Origin"), Qt::CaseInsensitive) == 0) origin = it.value().toString();
        QJsonArray respHdrs = params["responseHeaders"].toArray();
        bool hasAcao = false;
        for (const QJsonValue &h : respHdrs)
            if (h.toObject()["name"].toString().compare(QLatin1String("access-control-allow-origin"), Qt::CaseInsensitive) == 0)
                hasAcao = true;
        // ★ 고치는 것은 SingleFile 이 페이지에 넣는 자원(그림·글꼴·CSS·소리·영상)뿐이다 — 이 동안 페이지 자신의
        //   fetch 도 여기를 지나므로, JSON·HTML 같은 다른 응답까지 교차 출처로 읽히게 열어 두지 않는다.
        QString ctype;
        for (const QJsonValue &h : respHdrs)
            if (h.toObject()["name"].toString().compare(QLatin1String("content-type"), Qt::CaseInsensitive) == 0)
                ctype = h.toObject()["value"].toString().trimmed().toLower();
        static const char *const kInlineable[] = {"image/", "font/", "text/css", "audio/", "video/",
                                                  "application/font", "application/x-font",
                                                  "application/vnd.ms-fontobject"};
        bool inlineable = false;
        for (const char *pre : kInlineable)
            if (ctype.startsWith(QLatin1String(pre))) { inlineable = true; break; }
        // application/octet-stream 은 아무 이진 응답(첨부 내려받기·API 페이로드·장비 설정 백업 등)에나 붙는다.
        //   잘못 설정된 CDN 의 글꼴·그림처럼 주소가 자원 파일일 때만 연다.
        if (!inlineable && (ctype.startsWith(QLatin1String("application/octet-stream"))
                             || ctype.startsWith(QLatin1String("binary/octet-stream")))) {   // S3 기본값
            static const char *const kResExt[] = {".woff2", ".woff", ".ttf", ".otf", ".eot", ".png", ".jpg", ".jpeg",
                                                  ".gif", ".webp", ".avif", ".svg", ".ico", ".bmp", ".mp3", ".m4a",
                                                  ".ogg", ".wav", ".mp4", ".webm", ".mov"};
            const QString path = QUrl(params["request"].toObject()["url"].toString()).path().toLower();
            for (const char *ext : kResExt)
                if (path.endsWith(QLatin1String(ext))) { inlineable = true; break; }
        }
        const int status = params["responseStatusCode"].toInt(0);
        // 3xx(리다이렉트)는 머리를 바꾸지 않는다 — 리다이렉트 머리 교체는 거부될 수 있고, 거부되면 요청이 멈춘 채 남는다.
        const bool isRedirect = status >= 300 && status < 400;
        bool modified = false;
        if (inlineable && !origin.isEmpty() && origin != QLatin1String("null") && !hasAcao && status > 0 && !isRedirect) {
            // ★ 서버가 Access-Control-Allow-Credentials: true 만(ACAO 없이) 주는 경우가 있다. 그 위에 ACAO 를 붙이면
            //   자격증명(쿠키) 요청의 응답까지 교차 출처로 읽힌다 — 여는 것은 자격증명 없는 읽기뿐이므로 ACAC 는 뺀다.
            for (int i = respHdrs.size() - 1; i >= 0; --i)
                if (respHdrs.at(i).toObject()["name"].toString()
                        .compare(QLatin1String("access-control-allow-credentials"), Qt::CaseInsensitive) == 0)
                    respHdrs.removeAt(i);
            QJsonObject h;
            h["name"] = "Access-Control-Allow-Origin";
            h["value"] = origin;
            respHdrs.append(h);
            p["responseCode"] = status;
            p["responseHeaders"] = respHdrs;
            modified = true;
        }
        // 고친 머리가 거부되면(오류) 고치지 않은 채로 이어 보낸다 — 멈춘 요청(페이지 자신의 fetch 포함)을 남기지 않는다.
        sendCommand("Fetch.continueResponse", p, [this, rid, modified](const QJsonValue &, const QJsonValue &err) {
            if (!modified || err.toObject().isEmpty()) return;
            QJsonObject q;
            q["requestId"] = rid;
            sendCommand("Fetch.continueRequest", q, nullptr);
        });
        return;
    }
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
    if (!m_ready) { if (done) done(false); return; }
    QJsonObject params;
    params["url"] = url;
    sendCommand("Page.enable", QJsonObject(), nullptr);
    sendCommand("Page.navigate", params, [this, url, done](const QJsonValue &result, const QJsonValue &err) {
        // ★ 연결 거부·DNS·프록시 실패는 CDP 'error' 가 아니라 result.errorText 로 온다.
        //   예전엔 성공으로 보고 크롬 오류 화면(chrome-error://chromewebdata)을 SingleFile 로 저장했다 —
        //   파일이 생겼으니 다음 판에서도 다시 캡쳐하지 않았다.
        const QString navErr = result.toObject().value("errorText").toString();
        if (!navErr.isEmpty() && m_backend)
            m_backend->log(QString("navigate 실패 (%1): %2").arg(navErr, url), "warning", "crawl");
        if (done) done((err.isNull() || err.toObject().isEmpty()) && navErr.isEmpty());
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
