// ═════════════════════════════════════════════════════════════════════════
// PenBackend — 팬을 잘 쓰고 싶다 (Pen)
// 인터랙티브 CDP 크롤러 전용. 사용자가 본인 손으로 페이지 돌아다니며
// SingleFile 캡처. 캡챠/로그인/age-gate는 사용자가 직접 풂.
// ═════════════════════════════════════════════════════════════════════════
#include "PenBackend.h"
#include "MainWindow.h"
#include "Config.h"
#include "core/Common.h"
#include "platforms/RealChromeCrawler.h"
#include "utils/FileHelper.h"
#include "utils/WebDavUploader.h"

#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QJsonDocument>
#include <QJsonArray>
#include <QProcess>
#include <QThread>
#include <QDir>
#include <QStandardPaths>
#include <QDirIterator>
#include <QStorageInfo>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QCoreApplication>
#include <QRegularExpression>
#include <QTimer>
#include <QQueue>
#include <QSet>
#include <QDateTime>
#include <QSysInfo>

// ═════════════════════════════════════════════════════════════════════════
// 작은 헬퍼: filename 안전화 (크로스플랫폼 금칙어 제거)
// ═════════════════════════════════════════════════════════════════════════
static QString safeName(const QString &name, int maxLen = 100)
{
    QString s = name;
    s.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
    s = s.trimmed();
    if (s.length() > maxLen) s = s.left(maxLen);
    if (s.isEmpty()) s = "untitled";
    return s;
}

PenBackend::PenBackend(MainWindow *window, QObject *parent)
    : QObject(parent)
    , m_window(window)
    , m_config(new Config(this))
    , m_webdav(new WebDavUploader(this))
{
    connect(this, &PenBackend::jsSignal, this, &PenBackend::executeJsMainThread);
    connect(this, &PenBackend::logSignal, this, &PenBackend::appendLogMainThread);
    connect(m_webdav, &WebDavUploader::logMessage, this, [this](const QString &msg, const QString &type) {
        log(msg, type);
    });

    m_logFlushTimer = new QTimer(this);
    m_logFlushTimer->setInterval(150);
    connect(m_logFlushTimer, &QTimer::timeout, this, &PenBackend::flushLogs);
    m_logFlushTimer->start();

    // ★ 옛 번들 이름 (팬을 잘 쓰고 싶다) 폴더 정리 — Pen으로 rename 후 잔여 데이터 회수.
    //   config는 이미 마이그레이션 (Config::load 안 candidate 검사). 옛 dir 통째 제거.
    {
        QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        QDir parentDir(QFileInfo(appData).absolutePath());
        QStringList legacy = {"팬을 잘 쓰고 싶다", "ABIWA"};
        qint64 freed = 0;
        for (const QString &name : legacy) {
            QString p = parentDir.absoluteFilePath(name);
            if (QDir(p).exists()) {
                QDirIterator it(p, QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext()) freed += QFileInfo(it.next()).size();
                QDir(p).removeRecursively();
            }
        }
        if (freed > 0) {
            qDebug() << "[Cleanup] Old bundle data:" << freed / 1024 / 1024 << "MB 회수";
        }
    }

    // ★ QtWebEngine cache 자동 정리 — Caches/Pen 누적 방지. 30일 초과 파일만 제거.
    {
        QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDateTime cutoff = QDateTime::currentDateTime().addDays(-30);
        qint64 freed = 0;
        QDirIterator it(cacheRoot, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QFileInfo fi(it.next());
            if (fi.lastModified() < cutoff) {
                qint64 sz = fi.size();
                if (QFile::remove(fi.absoluteFilePath())) freed += sz;
            }
        }
        if (freed > 0) {
            qDebug() << "[Cleanup] Web cache (30d+):" << freed / 1024 / 1024 << "MB 회수";
        }
    }
}

PenBackend::~PenBackend()
{
    if (m_crawlChrome) {
        m_crawlChrome->stop();
        m_crawlChrome->deleteLater();
        m_crawlChrome = nullptr;
    }
}

// ═════════════════════════════════════════════════════════════════════════
// JS bridge / 로그 배칭
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::executeJsMainThread(const QString &js)
{
    if (m_window && m_window->webView()) {
        m_window->webView()->page()->runJavaScript(js);
    }
}

void PenBackend::appendLogMainThread(const QString &message, const QString &type, const QString &platform)
{
    if (!m_window || !m_window->webView()) return;
    m_pendingLogs[platform].append({message, type});
}

void PenBackend::flushLogs()
{
    if (!m_window || !m_window->webView() || m_pendingLogs.isEmpty()) return;

    QString js;
    for (auto it = m_pendingLogs.begin(); it != m_pendingLogs.end(); ++it) {
        const QString &platform = it.key();
        QList<PendingLog> &logs = it.value();
        if (logs.isEmpty()) continue;
        if (logs.size() > 30) logs = logs.mid(logs.size() - 30);
        for (const auto &entry : logs) {
            QString esc = entry.message;
            esc.replace("\\", "\\\\");
            esc.replace("'", "\\'");
            esc.replace("\"", "\\\"");
            esc.replace("\n", " ");
            js += QString("appendLog('%1','%2','%3');").arg(esc, entry.type, platform);
        }
    }
    m_pendingLogs.clear();
    if (!js.isEmpty()) m_window->webView()->page()->runJavaScript(js);
}

void PenBackend::runJs(const QString &js) { emit jsSignal(js); }

void PenBackend::log(const QString &message, const QString &type, const QString &platform)
{
    QString p = platform.isEmpty() ? QStringLiteral("crawl") : platform;
    emit logSignal(message, type, p);
    // ★ 터미널 로그 파일에도 동시 write (Terminal.app 창에 실시간 표시)
    QString prefix;
    if (type == "error") prefix = "[ERROR] ";
    else if (type == "warning") prefix = "[WARN] ";
    else if (type == "success") prefix = "[OK] ";
    writeTerminalLog(prefix + message);
}

// ═════════════════════════════════════════════════════════════════════════
// Config / FormData
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::loadConfig()
{
    m_config->load();
    // WebDAV 설정 복원
    if (m_webdav) {
        m_webdav->setConfig(m_config->webdavUrl(), m_config->webdavUser(), m_config->webdavPass(),
                            QString(), m_config->webdavEnabled());
    }
    QJsonObject cfg = m_config->toJson();
    QString json = QString::fromUtf8(QJsonDocument(cfg).toJson(QJsonDocument::Compact));
    runJs(QString("if(window.onConfigLoaded) onConfigLoaded(%1);").arg(json));
}

void PenBackend::saveConfig(const QString &configJson)
{
    QJsonDocument doc = QJsonDocument::fromJson(configJson.toUtf8());
    if (doc.isObject()) {
        m_config->fromJson(doc.object());
        m_config->save();
    }
}

void PenBackend::saveFormData(const QString &formJson)
{
    QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
    QDir().mkpath(path);
    QFile f(path + "/form.json");
    if (f.open(QIODevice::WriteOnly)) { f.write(formJson.toUtf8()); f.close(); }
}

void PenBackend::loadFormData()
{
    QString path = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation) + "/form.json";
    QFile f(path);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) {
        runJs("if(window.onFormDataLoaded) onFormDataLoaded({});");
        return;
    }
    QString json = QString::fromUtf8(f.readAll());
    f.close();
    runJs(QString("if(window.onFormDataLoaded) onFormDataLoaded(%1);").arg(json));
}

// ═════════════════════════════════════════════════════════════════════════
// 파일 시스템 / 클립보드
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::browseSavePath()
{
    QString defaultPath = QDir::homePath() + "/Downloads/Crawl";
    QString path = QFileDialog::getExistingDirectory(m_window, "저장 폴더 선택", defaultPath);
    if (!path.isEmpty()) {
        runJs(QString("if(window.onSavePathPicked) onSavePathPicked('%1');").arg(path.replace("'", "\\'")));
    }
}

void PenBackend::openFolder(const QString &path)
{
    QString p = path;
    p.replace("~", QDir::homePath());
    if (!QFile::exists(p)) {
        log(QString("폴더 없음: %1").arg(p), "warning");
        return;
    }
#ifdef Q_OS_MACOS
    QProcess::startDetached("open", {p});
#elif defined(Q_OS_WIN)
    QProcess::startDetached("explorer", {QDir::toNativeSeparators(p)});
#else
    QProcess::startDetached("xdg-open", {p});
#endif
}

void PenBackend::pasteToField(const QString &fieldId)
{
    QString text = QApplication::clipboard()->text();
    text.replace("\\", "\\\\");
    text.replace("'", "\\'");
    text.replace("\n", "\\n");
    runJs(QString("var el=document.getElementById('%1'); if(el){el.value='%2'; el.dispatchEvent(new Event('input',{bubbles:true}));}").arg(fieldId, text));
}

// ═════════════════════════════════════════════════════════════════════════
// 시스템 정보
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::getSystemInfo()
{
    QJsonObject info;
    info["os"] = QSysInfo::prettyProductName();
    info["arch"] = QSysInfo::currentCpuArchitecture();
    info["qt"] = QString(qVersion());
    info["app"] = QCoreApplication::applicationVersion();
    QString downloads = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    QStorageInfo si(downloads);
    info["downloadsFreeGB"] = (double)si.bytesAvailable() / (1024.0 * 1024.0 * 1024.0);
    info["downloadsPath"] = downloads;
    QString json = QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact));
    runJs(QString("if(window.onSystemInfo) onSystemInfo(%1);").arg(json));
}

// ═════════════════════════════════════════════════════════════════════════
// SingleFile lib 캐시 (앱 번들 → ~/Library/.../tools/singlefile_extension/lib/single-file.js)
// ═════════════════════════════════════════════════════════════════════════
static QString readSingleFileLib()
{
    static QString cache;
    if (!cache.isEmpty()) return cache;
    QStringList candidates;
    QString resBase = QCoreApplication::applicationDirPath();
#ifdef Q_OS_MACOS
    candidates << resBase + "/../Resources/tools/singlefile_extension/lib/single-file.js";
#endif
    candidates << resBase + "/tools/singlefile_extension/lib/single-file.js";
    candidates << resBase + "/../resources/tools/singlefile_extension/lib/single-file.js";
    for (const QString &p : candidates) {
        QFile f(p);
        if (f.exists() && f.open(QIODevice::ReadOnly)) {
            cache = QString::fromUtf8(f.readAll());
            f.close();
            return cache;
        }
    }
    return QString();
}

// ═════════════════════════════════════════════════════════════════════════
// 인터랙티브 크롤러
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::crawlStart(const QString &startUrl, const QString &savePath, bool useUserProfile)
{
    QMutexLocker lock(&m_crawlMutex);

    QString sp = savePath;
    if (sp.isEmpty()) sp = QDir::homePath() + "/Downloads/Pen";
    sp.replace("~", QDir::homePath());

    // ★ 저장 경로 부모 디렉토리 존재 확인 — 외장 디스크 (예: /Volumes/xxx) 빠진 경우 fallback
    //   부모 디렉토리 없으면 ~/Downloads/Pen 으로 자동 변경. 사용자 데이터 손실 방지.
    QString parent = QFileInfo(sp).absolutePath();
    if (!QDir(parent).exists()) {
        QString fallback = QDir::homePath() + "/Downloads/Pen";
        log(QString("⚠ 저장 경로 부모 디렉토리 없음 (%1) — %2 로 자동 전환").arg(parent, fallback), "warning");
        sp = fallback;
    }

    // ★ 폴더명 — 도메인 + 타임스탬프 (예: manatoki.net_20260517_112523)
    //   URL 에서 호스트 추출 → 더 명확. URL 없으면 그냥 "crawl_타임스탬프".
    QString siteName = "crawl";
    if (!startUrl.isEmpty()) {
        QUrl u(startUrl.contains("://") ? startUrl : "https://" + startUrl);
        QString host = u.host();
        if (host.startsWith("www.")) host = host.mid(4);
        host.replace(QRegularExpression("[^a-zA-Z0-9._-]"), "_");
        if (!host.isEmpty()) siteName = host;
    }
    QString ts = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    sp += "/" + siteName + "_" + ts;
    if (!QDir().mkpath(sp + "/captures")) {
        sp = QDir::homePath() + "/Downloads/Pen/" + siteName + "_" + ts;
        QDir().mkpath(sp + "/captures");
        log(QString("⚠ 저장 경로 생성 실패 → %1 사용").arg(sp), "warning");
    }
    log(QString("📂 저장 폴더: %1").arg(QFileInfo(sp).fileName()), "info");
    m_crawlSavePath = sp;
    m_crawlPageCounter = 0;

    // ★ 터미널 로그 창 띄움 (Chernobyl 패턴) — 별도 Terminal.app 에서 실시간 로그 표시
    openTerminalLog();

    log(QString("크롤 시작: %1 → %2").arg(startUrl.isEmpty() ? "(빈 페이지)" : startUrl, sp), "info");

    // ★ useUserProfile = "사용자 Chrome 로그인 가져오기" 모드.
    //   직접 사용자 프로필 사용은 macOS 락/CDP/암호화 키 문제로 불안정 →
    //   대신 Chrome 시작 전 사용자 Cookies/Login Data 를 펜 프로필로 복사 → 로그인 가져옴.
    //   장점: 락 충돌 없음, 사용자 Chrome 안 종료해도 작동, 펜 프로필 격리 유지.
    if (useUserProfile) {
#ifdef Q_OS_MACOS
        QString userDefault = QDir::homePath() + "/Library/Application Support/Google/Chrome/Default";
        QString penProfile  = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/chrome_capture_profile";
        QString penDefault  = penProfile + "/Default";
        QDir().mkpath(penDefault);

        QProcess pgrep;
        pgrep.start("/bin/sh", {"-c", "pgrep -f 'Google Chrome.app/Contents/MacOS/Google Chrome' | head -1"});
        pgrep.waitForFinished(2000);
        bool chromeRunning = !QString::fromUtf8(pgrep.readAllStandardOutput()).trimmed().isEmpty();
        if (chromeRunning) {
            log("⚠ 사용자 Chrome 실행 중 — Cookies SQLite 락 가능성 (Chrome 종료 후 다시 누르면 더 정확)", "warning");
        }

        int copied = 0;
        QStringList files = {"Cookies", "Cookies-journal", "Login Data", "Login Data-journal", "Web Data", "Web Data-journal"};
        for (const QString &name : files) {
            QString src = userDefault + "/" + name;
            QString dst = penDefault + "/" + name;
            if (QFile::exists(src)) {
                QFile::remove(dst);
                if (QFile::copy(src, dst)) copied++;
            }
        }
        // Local State — Chrome Safe Storage 키 참조 + 프로필 메타 (한 번만 복사)
        QString srcLS = QDir::homePath() + "/Library/Application Support/Google/Chrome/Local State";
        QString dstLS = penProfile + "/Local State";
        if (QFile::exists(srcLS) && !QFile::exists(dstLS)) QFile::copy(srcLS, dstLS);

        log(QString("사용자 Chrome 로그인 가져옴: %1개 파일 복사").arg(copied), "success");
        log("프로필: 펜 전용 (사용자 Cookies/Login Data 임포트됨)", "info");
        useUserProfile = false;  // 펜 프로필로 시작 — 복사된 쿠키 자동 사용
#else
        log("프로필: 펜 전용 영구 프로필", "info");
        useUserProfile = false;
#endif
    } else {
        log("프로필: 펜 전용 영구 프로필 (한번 로그인하면 유지)", "info");
    }

    if (m_crawlChrome) {
        m_crawlChrome->stop();
        m_crawlChrome->deleteLater();
        m_crawlChrome = nullptr;
    }
    m_crawlChrome = new RealChromeCrawler(this, this);
    m_crawlChrome->setUseUserProfile(useUserProfile);

    // ★ Chrome 시작 콜백 — useUserProfile=true 시작 실패 시 자동 fallback (사용자 프로필 권한/락 문제 회피)
    auto onChromeStarted = std::make_shared<std::function<void(bool, bool)>>();
    *onChromeStarted = [this, startUrl, onChromeStarted](bool ok, bool isRetry) {
        if (!ok) {
            // 첫 시도 + useUserProfile 이면 펜 전용 프로필로 1회 자동 재시도
            if (!isRetry && m_crawlChrome && m_crawlChrome->useUserProfile()) {
                log("⚠ 사용자 프로필 Chrome 시작 실패 → 펜 전용 프로필로 자동 재시도", "warning");
                if (m_crawlChrome) { m_crawlChrome->stop(); m_crawlChrome->deleteLater(); m_crawlChrome = nullptr; }
                m_crawlChrome = new RealChromeCrawler(this, this);
                m_crawlChrome->setUseUserProfile(false);
                m_crawlChrome->start([fn = onChromeStarted](bool ok2) { (*fn)(ok2, true); });
                return;
            }
            log("Chrome 시작 실패 — Chrome 설치/권한 확인", "error");
            runJs("if(window.onCrawlStarted) onCrawlStarted(false);");
            return;
        }
        runJs("if(window.onCrawlStarted) onCrawlStarted(true);");
        if (startUrl.isEmpty()) {
            log("Chrome 준비 완료 — URL 입력 후 navigate 또는 사용자 직접 조작", "success");
            return;
        }
        // navigate
        m_crawlChrome->navigate(startUrl, [this](bool navOk) {
            if (navOk) {
                log("페이지 로드 완료 — 사용자 직접 조작 가능", "success");
                log("로그인/캡챠 필요하면 Chrome 창에서 직접 풀고 '준비 완료' 누르거나 그냥 캡쳐 진행", "info");
            }
            // navigate 실패 메시지는 RealChromeCrawler::navigate 안에서 상세 출력 (중복 X)
        });
    };
    m_crawlChrome->start([fn = onChromeStarted](bool ok) { (*fn)(ok, false); });
}

void PenBackend::crawlStop()
{
    QMutexLocker lock(&m_crawlMutex);
    if (m_crawlChrome) {
        m_crawlChrome->stop();
        m_crawlChrome->deleteLater();
        m_crawlChrome = nullptr;
        log("크롤 종료", "info");
        closeTerminalLog();  // ★ 터미널 창에 [DONE] 마커 → 자동 종료
        runJs("if(window.onCrawlStopped) onCrawlStopped();");
    }
}

void PenBackend::crawlNavigate(const QString &url)
{
    QMutexLocker lock(&m_crawlMutex);
    if (!m_crawlChrome || !m_crawlChrome->isReady()) {
        log("Chrome 준비 안 됨 — 시작 먼저", "warning");
        return;
    }
    m_crawlChrome->navigate(url, [this, url](bool ok) {
        if (ok) log(QString("→ %1").arg(url), "info");
        else log(QString("navigate 실패: %1").arg(url), "warning");
    });
}

void PenBackend::crawlCaptureCurrent(const QString &filename)
{
    QMutexLocker lock(&m_crawlMutex);
    if (!m_crawlChrome || !m_crawlChrome->isReady()) {
        log("Chrome 준비 안 됨", "warning");
        return;
    }
    QString libCode = readSingleFileLib();
    if (libCode.isEmpty()) {
        log("SingleFile lib 로드 실패 (앱 번들 확인)", "error");
        return;
    }
    QString fname = filename;
    if (fname.isEmpty()) {
        m_crawlPageCounter++;
        fname = QString("page_%1").arg(m_crawlPageCounter, 3, 10, QChar('0'));
    }
    QString savePath = m_crawlSavePath + "/captures/" + safeName(fname) + ".html";

    auto *chrome = m_crawlChrome;
    // ★ 캡쳐 직전 grayscale/blur 필터 + sensitive overlay 정리 (트윗 매크로와 동일)
    QString cleanupJs = R"JS(
        (() => {
            try {
                document.querySelectorAll(
                    '[data-testid="sensitive-media-warning"] button,'
                    + ' button[data-testid="sensitiveMediaButton"],'
                    + ' [data-testid="sensitiveMediaButton"]'
                ).forEach(b => { try { b.click(); } catch(e){} });
            } catch(e){}
            for (const el of document.querySelectorAll('*')) {
                try {
                    const cs = window.getComputedStyle(el);
                    if (cs && cs.filter && cs.filter !== 'none' && /grayscale|blur|invert|sepia/i.test(cs.filter)) {
                        el.style.setProperty('filter', 'none', 'important');
                        el.style.setProperty('-webkit-filter', 'none', 'important');
                    }
                    if (el.style && el.style.filter && /grayscale|blur|invert|sepia/i.test(el.style.filter)) {
                        el.style.filter = 'none';
                    }
                } catch(e){}
            }
            try {
                for (const sheet of document.styleSheets) {
                    let rules; try { rules = sheet.cssRules; } catch(e){ continue; }
                    if (!rules) continue;
                    for (let i = 0; i < rules.length; i++) {
                        const r = rules[i];
                        if (r && r.style && r.style.filter && /grayscale|blur|invert|sepia/i.test(r.style.filter)) {
                            r.style.filter = 'none';
                            r.style.removeProperty('-webkit-filter');
                        }
                    }
                }
            } catch(e){}
            document.documentElement.style.filter = 'none';
            document.body.style.filter = 'none';
            return true;
        })()
    )JS";
    chrome->evaluate(cleanupJs, [this, chrome, savePath, libCode](const QJsonValue &) {
    chrome->evaluate(libCode, [this, chrome, savePath](const QJsonValue &) {
        QString call = R"JS(
            (async () => {
                if (typeof singlefile === 'undefined' || !singlefile.getPageData) {
                    return {error: 'SingleFile lib not loaded'};
                }
                try {
                    // ★ 사이즈 최소화 — 큰 페이지 결과 100MB+ 메모리 → OOM 위험.
                    //   사용 안 하는 CSS/폰트/숨김 요소 제거 (보이는 컨텐츠는 그대로).
                    const data = await singlefile.getPageData({
                        removeHiddenElements: true,
                        removeUnusedStyles: true,
                        removeUnusedFonts: true,
                        removeFrames: true,
                        removeImports: true,
                        removeScripts: true,
                        blockScripts: true,
                        saveRawPage: false,
                        insertSingleFileComment: true,
                        compressHTML: true
                    });
                    let html = data.content;
                    html = html.replace(/<meta[^>]+http-equiv\s*=\s*["']?refresh["']?[^>]*>/gi, '');
                    html = html.replace(/<noscript[^>]*>[\s\S]*?<\/noscript>/gi, '');
                    // ★ 큰 HTML은 메모리로 한번에 반환하면 OOM. JS global에 저장하고 size만 반환.
                    window.__penContent = html;
                    return {size: html.length, url: data.url || location.href, title: data.title || document.title, chunked: true};
                } catch (e) {
                    return {error: String(e && e.message || e)};
                }
            })()
        )JS";
        chrome->evaluate(call, [this, chrome, savePath](const QJsonValue &v) {
            QJsonObject obj = v.toObject();
            if (obj.contains("error")) {
                log(QString("[캡쳐] 오류: %1").arg(obj["error"].toString()), "warning");
                return;
            }
            QString sourceUrl = obj["url"].toString();  // ★ 원본 URL — xattr/Finder 코멘트용
            // ★ chunked mode — JS global window.__penContent 1MB씩 가져와 디스크 직접 쓰기
            if (obj["chunked"].toBool()) {
                qint64 totalSize = obj["size"].toVariant().toLongLong();
                obj = QJsonObject();
                if (totalSize <= 0) { log("[캡쳐] 빈 결과", "warning"); return; }
                QFile *outFile = new QFile(savePath);
                if (!outFile->open(QIODevice::WriteOnly)) {
                    log(QString("[캡쳐] 파일 열기 실패: %1").arg(savePath), "error");
                    delete outFile;
                    return;
                }
                const qint64 CHUNK = 1024 * 1024;  // 1MB
                auto offsetPtr = std::make_shared<qint64>(0);
                auto fetchNext = std::make_shared<std::function<void()>>();
                *fetchNext = [this, chrome, outFile, totalSize, offsetPtr, fetchNext, savePath, sourceUrl]() {
                    if (*offsetPtr >= totalSize) {
                        outFile->close();
                        delete outFile;
                        chrome->evaluate("delete window.__penContent; true;", [](const QJsonValue &){});
                        // ★ Chernobyl 식 메타데이터: macOS xattr (where from) + Finder 코멘트
                        if (!sourceUrl.isEmpty()) {
                            FileHelper::setDownloadMeta(savePath, sourceUrl);
                            FileHelper::setFinderComment(savePath, sourceUrl);
                        }
                        log(QString("✅ 캡쳐: %1 (%2KB)")
                            .arg(QFileInfo(savePath).fileName())
                            .arg(totalSize / 1024), "success");
                        enqueueWebDavUpload(savePath);  // ★ NAS 자동 업로드
                        return;
                    }
                    qint64 end = qMin(*offsetPtr + CHUNK, totalSize);
                    QString slice = QString("window.__penContent.slice(%1, %2)").arg(*offsetPtr).arg(end);
                    chrome->evaluate(slice, [outFile, offsetPtr, fetchNext, end](const QJsonValue &cv) {
                        QByteArray bytes = cv.toString().toUtf8();
                        outFile->write(bytes);
                        bytes.clear();
                        *offsetPtr = end;
                        (*fetchNext)();
                    });
                };
                (*fetchNext)();
            }
        });
    });
    });   // close cleanupJs evaluate
}

void PenBackend::crawlClick(const QString &cssSelector)
{
    QMutexLocker lock(&m_crawlMutex);
    if (!m_crawlChrome || !m_crawlChrome->isReady()) return;
    QString sel = QString(cssSelector).replace("'", "\\'");
    QString js = QString(
        "(function(){var el=document.querySelector('%1'); if(el){el.click(); return true;} return false;})()"
    ).arg(sel);
    m_crawlChrome->evaluate(js, [this, cssSelector](const QJsonValue &v) {
        if (v.toBool()) log(QString("클릭: %1").arg(cssSelector), "info");
        else log(QString("클릭 실패 (요소 없음): %1").arg(cssSelector), "warning");
    });
}

void PenBackend::crawlType(const QString &cssSelector, const QString &text)
{
    QMutexLocker lock(&m_crawlMutex);
    if (!m_crawlChrome || !m_crawlChrome->isReady()) return;
    QString sel = QString(cssSelector).replace("'", "\\'");
    QString val = QString(text).replace("'", "\\'");
    QString js = QString(
        "(function(){"
        "  var el=document.querySelector('%1');"
        "  if(!el) return false;"
        "  el.focus(); el.value='%2';"
        "  el.dispatchEvent(new Event('input',{bubbles:true}));"
        "  el.dispatchEvent(new Event('change',{bubbles:true}));"
        "  return true;"
        "})()"
    ).arg(sel, val);
    m_crawlChrome->evaluate(js, [this, cssSelector](const QJsonValue &v) {
        if (v.toBool()) log(QString("입력: %1").arg(cssSelector), "info");
        else log(QString("입력 실패 (요소 없음): %1").arg(cssSelector), "warning");
    });
}

void PenBackend::crawlEvaluate(const QString &js)
{
    QMutexLocker lock(&m_crawlMutex);
    if (!m_crawlChrome || !m_crawlChrome->isReady()) return;
    m_crawlChrome->evaluate(js, [this](const QJsonValue &v) {
        log(QString("[eval] %1").arg(QString::fromUtf8(QJsonDocument::fromVariant(v.toVariant()).toJson(QJsonDocument::Compact))),
            "info");
    });
}

void PenBackend::crawlScrollAll()
{
    QMutexLocker lock(&m_crawlMutex);
    if (!m_crawlChrome || !m_crawlChrome->isReady()) return;
    QString scrollJs = R"JS(
        (async () => {
            const sleep = ms => new Promise(r => setTimeout(r, ms));
            let prev = 0, same = 0;
            for (let i = 0; i < 80; i++) {
                window.scrollTo(0, document.body.scrollHeight);
                await sleep(800);
                const cur = document.body.scrollHeight;
                if (cur === prev) { if (++same >= 3) break; }
                else { same = 0; prev = cur; }
            }
            window.scrollTo(0, 0);
            await sleep(300);
            return prev;
        })()
    )JS";
    m_crawlChrome->evaluate(scrollJs, [this](const QJsonValue &v) {
        log(QString("스크롤 완료 (높이 %1px)").arg(v.toInt()), "info");
    });
}

// ═════════════════════════════════════════════════════════════════════════
// 사용자 일시정지 — 캡챠/로그인/age-gate 풀 시간
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::crawlPauseForUser(int seconds)
{
    if (m_waitingForUser.exchange(true)) return;  // 이미 대기 중
    log("⏸ 사용자 대기 — Chrome 창에서 캡챠/로그인/age-gate 처리 후 UI에서 '준비 완료' 누르세요", "warning");
    runJs("if(window.onCrawlPaused) onCrawlPaused();");

    // 별도 스레드에서 세마포 acquire (UI 안 막히게)
    int timeoutMs = (seconds > 0) ? seconds * 1000 : -1;
    QThread *t = QThread::create([this, timeoutMs]() {
        bool got = false;
        if (timeoutMs > 0) got = m_userPauseSem.tryAcquire(1, timeoutMs);
        else { m_userPauseSem.acquire(); got = true; }
        m_waitingForUser = false;
        QMetaObject::invokeMethod(this, [this, got]() {
            log(got ? "▶ 재개" : "⏰ 대기 타임아웃 — 강제 재개", got ? "success" : "warning");
            runJs("if(window.onCrawlResumed) onCrawlResumed();");
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

void PenBackend::crawlResumeFromUser()
{
    if (m_waitingForUser.load()) m_userPauseSem.release();
}

// ═════════════════════════════════════════════════════════════════════════
// crawlCaptureTweet — 트위터 전용 매크로
//   1) navigate
//   2) 로그인/캡챠 필요한지 (article 없는지) 감지 → 사용자 대기 + 배너
//   3) virtualized scroll로 모든 <article> 수집 → 정적 DOM 재구성
//      (스크롤 다시 위로 가도 처음 본 트윗이 사라지지 않음)
//   4) SingleFile로 인라인 HTML 저장
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::crawlCaptureTweet(const QString &tweetUrl, const QString &filename)
{
    {
        QMutexLocker lock(&m_crawlMutex);
        if (!m_crawlChrome || !m_crawlChrome->isReady()) {
            log("Chrome 준비 안 됨 — 시작 먼저", "warning");
            return;
        }
    }
    if (tweetUrl.isEmpty()) {
        log("트윗 URL 비어있음", "warning");
        return;
    }
    QString libCode = readSingleFileLib();
    if (libCode.isEmpty()) {
        log("SingleFile lib 로드 실패", "error");
        return;
    }
    // 파일명 — 빈값이면 URL 끝의 status id
    QString fname = filename;
    if (fname.isEmpty()) {
        QRegularExpression rx("status/(\\d+)");
        auto m = rx.match(tweetUrl);
        fname = m.hasMatch() ? QString("tweet_%1").arg(m.captured(1)) : "tweet";
    }
    QString savePath = m_crawlSavePath + "/captures/" + safeName(fname) + ".html";

    log(QString("[트윗 매크로] navigate: %1").arg(tweetUrl), "info");

    // 매크로는 워커 스레드에서 직렬 실행 (CDP 호출은 메인으로 invoke)
    QThread *t = QThread::create([this, tweetUrl, savePath, libCode]() {
        auto callJs = [this](const QString &js, int timeoutMs = 15000) -> QJsonValue {
            auto sem = std::make_shared<QSemaphore>(0);
            auto result = std::make_shared<QJsonValue>();
            QMetaObject::invokeMethod(this, [this, js, sem, result]() {
                if (!m_crawlChrome) { sem->release(); return; }
                m_crawlChrome->evaluate(js, [sem, result](const QJsonValue &v) {
                    *result = v; sem->release();
                });
            }, Qt::QueuedConnection);
            sem->tryAcquire(1, timeoutMs);
            return *result;
        };
        auto navigate = [this](const QString &url) -> bool {
            auto sem = std::make_shared<QSemaphore>(0);
            auto ok = std::make_shared<bool>(false);
            QMetaObject::invokeMethod(this, [this, url, sem, ok]() {
                if (!m_crawlChrome) { sem->release(); return; }
                m_crawlChrome->navigate(url, [sem, ok](bool r){ *ok = r; sem->release(); });
            }, Qt::QueuedConnection);
            sem->tryAcquire(1, 30000);
            return *ok;
        };

        // 현재 URL이 이미 그 트윗이면 navigate 생략 (이중 새로고침 방지)
        QString curUrl = callJs("location.href").toString();
        QString tweetIdInUrl;
        {
            QRegularExpression rx("status/(\\d+)");
            auto m = rx.match(tweetUrl);
            if (m.hasMatch()) tweetIdInUrl = m.captured(1);
        }
        bool alreadyOnTweet = !tweetIdInUrl.isEmpty() && curUrl.contains("status/" + tweetIdInUrl);
        if (!alreadyOnTweet) {
            bool navOk = navigate(tweetUrl);
            if (!navOk) {
                QMetaObject::invokeMethod(this, [this]() {
                    log("[트윗 매크로] navigate 실패", "error");
                }, Qt::QueuedConnection);
                return;
            }
            QThread::sleep(5);  // 페이지 안정화
        }

        // 감지: article (트윗 셀) / 로그인 프롬프트 / age-gate
        QString detectJs = R"JS(
            (function(){
                var hasArticle = !!document.querySelector(
                    'article[data-testid="tweet"], article[role="article"], article'
                );
                var path = location.pathname;
                var loginPrompt =
                    path.indexOf('/login') === 0 ||
                    path.indexOf('/i/flow/login') === 0 ||
                    !!document.querySelector('input[autocomplete="username"], input[name="text"][type="text"]');
                var bodyText = (document.body && document.body.innerText) || '';
                var ageGate =
                    bodyText.indexOf('age-restricted') !== -1 ||
                    bodyText.indexOf('Yes, view profile') !== -1 ||
                    bodyText.indexOf('연령 제한') !== -1;
                return {hasArticle: hasArticle, loginPrompt: loginPrompt,
                        ageGate: ageGate, url: location.href, title: document.title};
            })()
        )JS";
        QJsonObject status = callJs(detectJs).toObject();
        bool needsUser = !status["hasArticle"].toBool()
                      || status["loginPrompt"].toBool()
                      || status["ageGate"].toBool();

        if (needsUser) {
            QString why = status["loginPrompt"].toBool() ? "로그인 필요"
                       : status["ageGate"].toBool()    ? "age-gate"
                       :                                  "콘텐츠 미로드 (캡챠?)";
            QMetaObject::invokeMethod(this, [this, why]() {
                log(QString("[트윗 매크로] 사용자 작업 필요: %1 — Chrome 창에서 처리 후 '준비 완료'").arg(why), "warning");
                crawlPauseForUser(0);  // 무한 대기
            }, Qt::QueuedConnection);

            // 사용자가 풀고 resume 누를 때까지 대기 + 1초 간격으로 article 자동 재감지
            for (int i = 0; i < 1800; ++i) {  // 최대 30분
                QThread::sleep(1);
                if (!m_waitingForUser.load()) break;
                // 사용자가 명시적으로 resume 안 눌러도 article 떴으면 자동 재개
                if (i > 0 && i % 3 == 0) {
                    QJsonObject re = callJs(detectJs, 5000).toObject();
                    if (re["hasArticle"].toBool() && !re["loginPrompt"].toBool() && !re["ageGate"].toBool()) {
                        QMetaObject::invokeMethod(this, [this]() {
                            log("[트윗 매크로] article 감지됨 — 자동 재개", "success");
                            crawlResumeFromUser();
                        }, Qt::QueuedConnection);
                        QThread::sleep(1);
                        break;
                    }
                }
            }
            // 사용자가 다른 곳 갔으면 다시 navigate
            QString curAfter = callJs("location.href").toString();
            if (!tweetIdInUrl.isEmpty() && !curAfter.contains("status/" + tweetIdInUrl)) {
                navigate(tweetUrl);
                QThread::sleep(5);
            }
        }

        // virtualized scroll → 모든 article 수집 → 정적 DOM 재구성
        QMetaObject::invokeMethod(this, [this]() {
            log("[트윗 매크로] 스크롤 + article 수집 중…", "info");
        }, Qt::QueuedConnection);

        QString collectJs = R"JS(
            (async () => {
                const sleep = ms => new Promise(r => setTimeout(r, ms));
                const collected = new Map();  // tweetId -> outerHTML
                const collectVisible = () => {
                    document.querySelectorAll('article').forEach(art => {
                        const link = art.querySelector('a[href*="/status/"]');
                        const m = link && link.href.match(/status\/(\d+)/);
                        const id = m ? m[1] : ('idx_' + collected.size);
                        if (!collected.has(id)) collected.set(id, art.outerHTML);
                    });
                };
                window.scrollTo(0, 0);
                await sleep(800);
                collectVisible();
                let prev = 0, same = 0;
                for (let i = 0; i < 50; i++) {
                    window.scrollBy(0, window.innerHeight * 0.85);
                    await sleep(700);
                    collectVisible();
                    const cur = document.body.scrollHeight;
                    if (cur === prev) { if (++same >= 3) break; } else { same = 0; prev = cur; }
                    if (collected.size > 200) break;  // 안전장치
                }
                window.scrollTo(0, 0);
                await sleep(500);
                // 정적 DOM 재구성: <main> 안에 모든 수집된 article 박아넣음
                const main = document.querySelector('main') || document.body;
                const wrap = document.createElement('div');
                wrap.id = '__pen_static_capture__';
                wrap.style.cssText = 'max-width:600px;margin:0 auto;padding:8px';
                for (const html of collected.values()) {
                    const tmp = document.createElement('div');
                    tmp.innerHTML = html;
                    while (tmp.firstChild) wrap.appendChild(tmp.firstChild);
                }
                // 기존 dynamic main 비우고 새 정적 wrap 넣기
                main.innerHTML = '';
                main.appendChild(wrap);
                return collected.size;
            })()
        )JS";
        int collected = callJs(collectJs, 60000).toInt();
        QMetaObject::invokeMethod(this, [this, collected]() {
            log(QString("[트윗 매크로] %1개 article 수집 → 정적 DOM 재구성").arg(collected), "info");
        }, Qt::QueuedConnection);

        // ★ grayscale/blur 필터 + sensitive overlay 정리 (트위터 절전/필터 모드 대비)
        callJs(R"JS(
            (() => {
                try {
                    document.querySelectorAll(
                        '[data-testid="sensitive-media-warning"] button,'
                        + ' button[data-testid="sensitiveMediaButton"],'
                        + ' [data-testid="sensitiveMediaButton"]'
                    ).forEach(b => { try { b.click(); } catch(e){} });
                } catch(e){}
                for (const el of document.querySelectorAll('*')) {
                    try {
                        const cs = window.getComputedStyle(el);
                        if (cs && cs.filter && cs.filter !== 'none' && /grayscale|blur|invert|sepia/i.test(cs.filter)) {
                            el.style.setProperty('filter', 'none', 'important');
                            el.style.setProperty('-webkit-filter', 'none', 'important');
                        }
                        if (el.style && el.style.filter && /grayscale|blur|invert|sepia/i.test(el.style.filter)) {
                            el.style.filter = 'none';
                        }
                    } catch(e){}
                }
                try {
                    for (const sheet of document.styleSheets) {
                        let rules; try { rules = sheet.cssRules; } catch(e){ continue; }
                        if (!rules) continue;
                        for (let i = 0; i < rules.length; i++) {
                            const r = rules[i];
                            if (r && r.style && r.style.filter && /grayscale|blur|invert|sepia/i.test(r.style.filter)) {
                                r.style.filter = 'none';
                                r.style.removeProperty('-webkit-filter');
                            }
                        }
                    }
                } catch(e){}
                document.documentElement.style.filter = 'none';
                document.body.style.filter = 'none';
                return true;
            })()
        )JS", 10000);

        // SingleFile lib 주입 → getPageData 호출 → 저장
        callJs(libCode, 30000);
        QString sfCall = R"JS(
            (async () => {
                if (typeof singlefile === 'undefined') return {error: 'SingleFile lib not loaded'};
                try {
                    // ★ 사이즈 최소화 (메모리 OOM 방지)
                    const data = await singlefile.getPageData({
                        removeHiddenElements:true,
                        removeUnusedStyles:true,
                        removeUnusedFonts:true,
                        removeFrames:true,
                        removeImports:true,
                        removeScripts:true,
                        blockScripts:true,
                        saveRawPage:false,
                        insertSingleFileComment:true,
                        compressHTML:true
                    });
                    let html = data.content;
                    html = html.replace(/<meta[^>]+http-equiv\s*=\s*["']?refresh["']?[^>]*>/gi, '');
                    html = html.replace(/<noscript[^>]*>[\s\S]*?<\/noscript>/gi, '');
                    return {content: html, url: data.url || location.href};
                } catch (e) {
                    return {error: String(e && e.message || e)};
                }
            })()
        )JS";
        QJsonObject result = callJs(sfCall, 60000).toObject();
        if (result.contains("error")) {
            QMetaObject::invokeMethod(this, [this, err = result["error"].toString()]() {
                log(QString("[트윗 매크로] 캡쳐 오류: %1").arg(err), "error");
            }, Qt::QueuedConnection);
            return;
        }
        QString content = result["content"].toString();
        result = QJsonObject();  // 즉시 해제
        if (content.isEmpty()) {
            QMetaObject::invokeMethod(this, [this]() {
                log("[트윗 매크로] 빈 결과", "warning");
            }, Qt::QueuedConnection);
            return;
        }
        QFile f(savePath);
        if (f.open(QIODevice::WriteOnly)) {
            QByteArray bytes = content.toUtf8();
            int sz = bytes.size();
            f.write(bytes);
            f.close();
            bytes.clear();
            content = QString();
            // ★ Chernobyl 식 메타데이터
            FileHelper::setDownloadMeta(savePath, tweetUrl);
            FileHelper::setFinderComment(savePath, tweetUrl);
            QMetaObject::invokeMethod(this, [this, savePath, kb = sz / 1024]() {
                log(QString("✅ 캡쳐: %1 (%2KB)")
                    .arg(QFileInfo(savePath).fileName()).arg(kb), "success");
                enqueueWebDavUpload(savePath);  // ★ NAS 자동 업로드
            }, Qt::QueuedConnection);
        }
    });
    // 워커 끝나면 batch 컨트롤러에 신호 (있으면)
    connect(t, &QThread::finished, this, [this]() {
        if (m_batchDoneSem) m_batchDoneSem->release();
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

// ═════════════════════════════════════════════════════════════════════════
// crawlCaptureTweetBatch — 여러 트윗 순차 캡쳐
//   crawlCaptureTweet는 자체 워커를 띄움. 그래서 여기선 단순히
//   비동기로 하나씩 큐잉하지 말고, 별도 워커에서 동기적으로 capture 호출.
//   하지만 captureTweet도 워커를 만드므로, 더 간단하게는 timer로 슬롯 호출.
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::crawlCaptureTweetBatch(const QStringList &tweetUrls)
{
    if (tweetUrls.isEmpty()) {
        log("배치 캡쳐: 트윗 URL 없음", "warning");
        return;
    }
    {
        QMutexLocker lock(&m_crawlMutex);
        if (!m_crawlChrome || !m_crawlChrome->isReady()) {
            log("Chrome 준비 안 됨 — 시작 먼저", "warning");
            return;
        }
    }
    log(QString("배치 캡쳐 시작: 트윗 %1개").arg(tweetUrls.size()), "info");

    // crawlCaptureTweet은 자체 워커 → 워커 finished 시그널이 m_batchDoneSem release.
    // 컨트롤러는 sem.acquire로 대기 → 캡쳐 끝나면 즉시 다음 시작 (90초 고정 sleep 제거)
    QStringList list = tweetUrls;
    QThread *t = QThread::create([this, list]() {
        // 배치 sem 활성화 — crawlCaptureTweet 워커 끝나면 release됨
        QSemaphore sem(0);
        m_batchDoneSem = &sem;

        for (int i = 0; i < list.size(); ++i) {
            const QString &u = list[i];
            QMetaObject::invokeMethod(this, [this, i, total = list.size(), u]() {
                log(QString("[배치 %1/%2] %3").arg(i+1).arg(total).arg(u), "info");
                crawlCaptureTweet(u);
            }, Qt::QueuedConnection);
            // 캡쳐 워커 finished 시그널까지 대기 (안전장치 5분 timeout)
            if (!sem.tryAcquire(1, 5 * 60 * 1000)) {
                QMetaObject::invokeMethod(this, [this, i]() {
                    log(QString("[배치] %1번 캡쳐 timeout — 다음으로").arg(i+1), "warning");
                }, Qt::QueuedConnection);
            }
        }

        m_batchDoneSem = nullptr;
        QMetaObject::invokeMethod(this, [this]() {
            log("배치 캡쳐 완료", "success");
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

void PenBackend::crawlTryMirrors(const QStringList &urls, const QString &savePath, bool useUserProfile)
{
    if (urls.isEmpty()) {
        log("미러 URL 없음", "error");
        return;
    }
    crawlStart(QString(), savePath, useUserProfile);

    QStringList urlList = urls;
    QThread *t = QThread::create([this, urlList]() {
        for (int i = 0; i < 30 && (!m_crawlChrome || !m_crawlChrome->isReady()); ++i) QThread::sleep(1);
        if (!m_crawlChrome || !m_crawlChrome->isReady()) {
            QMetaObject::invokeMethod(this, [this]() {
                log("Chrome 준비 실패 — 미러 시도 중단", "error");
            }, Qt::QueuedConnection);
            return;
        }
        for (const QString &url : urlList) {
            QMetaObject::invokeMethod(this, [this, url]() {
                log(QString("미러 시도: %1").arg(url), "info");
            }, Qt::QueuedConnection);
            auto reach = std::make_shared<bool>(false);
            QMetaObject::invokeMethod(this, [this, url, reach]() {
                if (m_crawlChrome) m_crawlChrome->navigate(url, [reach](bool ok){ *reach = ok; });
            }, Qt::QueuedConnection);
            QThread::sleep(6);
            auto sem = std::make_shared<QSemaphore>(0);
            auto title = std::make_shared<QString>();
            QMetaObject::invokeMethod(this, [this, sem, title]() {
                if (!m_crawlChrome) { sem->release(); return; }
                m_crawlChrome->evaluate("document.title", [sem, title](const QJsonValue &v) {
                    *title = v.toString(); sem->release();
                });
            }, Qt::QueuedConnection);
            sem->tryAcquire(1, 5000);
            if (!title->isEmpty() && !title->contains("404", Qt::CaseInsensitive)
                && !title->contains("not found", Qt::CaseInsensitive)) {
                QMetaObject::invokeMethod(this, [this, url, title]() {
                    log(QString("✓ 미러 OK: %1 (%2)").arg(url, *title), "success");
                }, Qt::QueuedConnection);
                return;
            }
            QMetaObject::invokeMethod(this, [this, url]() {
                log(QString("✗ 미러 실패: %1").arg(url), "warning");
            }, Qt::QueuedConnection);
        }
        QMetaObject::invokeMethod(this, [this]() {
            log("모든 미러 실패", "error");
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

// ═════════════════════════════════════════════════════════════════════════
// crawlAuto — 자동 크롤 (BFS, 같은 도메인, maxPages 제한)
//   동작: 시작 URL → 캡쳐 → 페이지 안 링크 추출 → 큐에 추가 → 다음 URL 캡쳐 → 반복
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::crawlAuto(const QString &startUrl, int maxPages, bool sameDomain, bool downloadMedia)
{
    if (startUrl.isEmpty()) {
        log("자동 크롤: 시작 URL 필요", "error");
        return;
    }
    {
        QMutexLocker lock(&m_crawlMutex);
        if (!m_crawlChrome || !m_crawlChrome->isReady()) {
            log("Chrome 준비 안 됨 — 시작 먼저", "warning");
            return;
        }
    }
    m_autoCrawlStop.store(false);
    m_autoPageCount = 0;
    m_autoMediaCount = 0;
    log(QString("자동 크롤 시작: %1 (최대 %2페이지, %3, 미디어=%4)")
        .arg(startUrl).arg(maxPages > 0 ? QString::number(maxPages) : "무제한")
        .arg(sameDomain ? "같은 도메인만" : "모든 외부 링크")
        .arg(downloadMedia ? "ON" : "OFF"), "info");

    int maxP = maxPages;
    QString start = startUrl;
    bool sd = sameDomain;
    bool dm = downloadMedia;

    QThread *t = QThread::create([this, start, maxP, sd, dm]() {
        QSet<QString> visited;
        QQueue<QString> queue;
        queue.enqueue(start);
        QUrl startQ(start);
        QString rootHost = startQ.host();

        while (!queue.isEmpty() && !m_autoCrawlStop.load()) {
            QString url = queue.dequeue();
            if (visited.contains(url)) continue;
            visited.insert(url);

            QMetaObject::invokeMethod(this, [this, url]() {
                log(QString("[%1] navigate → %2").arg(m_autoPageCount + 1).arg(url), "info");
            }, Qt::QueuedConnection);

            // 1) navigate
            auto navSem = std::make_shared<QSemaphore>(0);
            auto navOk = std::make_shared<bool>(false);
            QMetaObject::invokeMethod(this, [this, url, navSem, navOk]() {
                if (!m_crawlChrome) { navSem->release(); return; }
                m_crawlChrome->navigate(url, [navSem, navOk](bool ok) {
                    *navOk = ok; navSem->release();
                });
            }, Qt::QueuedConnection);
            navSem->tryAcquire(1, 30000);
            if (!*navOk) {
                QMetaObject::invokeMethod(this, [this, url]() {
                    log(QString("✗ navigate 실패: %1").arg(url), "warning");
                }, Qt::QueuedConnection);
                continue;
            }

            // 2) readyState complete + 안정화 대기
            QThread::sleep(2);

            // 3) 캡쳐 (현재 페이지 → captures/auto_NNN.html)
            QString filename = QString("auto_%1").arg(++m_autoPageCount, 4, 10, QChar('0'));
            QMetaObject::invokeMethod(this, [this, filename]() {
                crawlCaptureCurrent(filename);
            }, Qt::QueuedConnection);
            QThread::sleep(3);  // 캡쳐 완료 대기 (chunked I/O 비동기라 적당히)

            // 4) 미디어 다운로드 (옵션)
            if (dm) {
                QMetaObject::invokeMethod(this, [this]() {
                    crawlDownloadMedia();
                }, Qt::QueuedConnection);
                QThread::sleep(2);
            }

            // 5) 다음 페이지 도달 — 링크 추출
            if (maxP > 0 && m_autoPageCount >= maxP) {
                QMetaObject::invokeMethod(this, [this]() {
                    log(QString("✓ 최대 페이지 %1 도달 — 종료").arg(m_autoPageCount), "success");
                }, Qt::QueuedConnection);
                break;
            }
            auto linkSem = std::make_shared<QSemaphore>(0);
            auto linkList = std::make_shared<QJsonArray>();
            QString linkJs = R"JS(
                Array.from(document.querySelectorAll('a[href]'))
                    .map(a => a.href)
                    .filter(u => u && u.startsWith('http'))
                    .slice(0, 200)
            )JS";
            QMetaObject::invokeMethod(this, [this, linkJs, linkSem, linkList]() {
                if (!m_crawlChrome) { linkSem->release(); return; }
                m_crawlChrome->evaluate(linkJs, [linkSem, linkList](const QJsonValue &v) {
                    *linkList = v.toArray();
                    linkSem->release();
                });
            }, Qt::QueuedConnection);
            linkSem->tryAcquire(1, 10000);

            int added = 0;
            for (const auto &v : *linkList) {
                QString u = v.toString();
                if (u.isEmpty() || visited.contains(u) || u.contains('#') ) continue;
                if (sd) {
                    QUrl qu(u);
                    if (qu.host() != rootHost) continue;
                }
                queue.enqueue(u);
                if (++added >= 50) break;  // 한 페이지당 최대 50개 링크만
            }
            QMetaObject::invokeMethod(this, [this, added]() {
                log(QString("  → 새 링크 %1개 큐 추가").arg(added), "info");
            }, Qt::QueuedConnection);
        }

        QMetaObject::invokeMethod(this, [this]() {
            log(QString("━━ 자동 크롤 종료 — 페이지 %1, 미디어 %2 ━━")
                .arg(m_autoPageCount).arg(m_autoMediaCount), "success");
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

void PenBackend::crawlAutoStop()
{
    m_autoCrawlStop.store(true);
    log("⏹ 중지 요청 — Chrome 작업 강제 중단", "warning");
    // ★ 즉시 중단 — 진행 중인 navigate/evaluate 모두 about:blank 로 abort.
    //   현재 SingleFile/chunk fetch 콜백이 빈 페이지에서 즉시 fail → 큐 다음 루프에서 stop flag 체크 → 종료.
    QMetaObject::invokeMethod(this, [this]() {
        if (m_crawlChrome && m_crawlChrome->isReady()) {
            m_crawlChrome->navigate("about:blank", [](bool){});
        }
    }, Qt::QueuedConnection);
    // 사용자 UI 상태 즉시 갱신 (정지 상태로 보이게)
    runJs("if(window.onCrawlStopped) onCrawlStopped();");
    closeTerminalLog();
}

// ═════════════════════════════════════════════════════════════════════════
// crawlDownloadMedia — 현재 페이지의 img/video src 추출 + URL 다운로드
//   savePath/media/ 에 저장. 원본 URL 기반 파일명.
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::crawlDownloadMedia()
{
    QMutexLocker lock(&m_crawlMutex);
    if (!m_crawlChrome || !m_crawlChrome->isReady()) {
        log("Chrome 준비 안 됨", "warning");
        return;
    }
    QString mediaDir = m_crawlSavePath + "/media";
    QDir().mkpath(mediaDir);
    auto *chrome = m_crawlChrome;

    QString js = R"JS(
        (() => {
            const urls = new Set();
            // img src + srcset
            document.querySelectorAll('img').forEach(img => {
                if (img.src && img.src.startsWith('http')) urls.add(img.src);
                if (img.srcset) {
                    img.srcset.split(',').forEach(s => {
                        const u = s.trim().split(' ')[0];
                        if (u && u.startsWith('http')) urls.add(u);
                    });
                }
            });
            // video src + source[src]
            document.querySelectorAll('video, source').forEach(v => {
                if (v.src && v.src.startsWith('http')) urls.add(v.src);
            });
            // background-image (computed style)
            document.querySelectorAll('*').forEach(el => {
                try {
                    const bg = getComputedStyle(el).backgroundImage;
                    const m = bg && bg.match(/url\(["']?(https?:\/\/[^"')]+)/);
                    if (m) urls.add(m[1]);
                } catch(e) {}
            });
            return Array.from(urls).slice(0, 500);
        })()
    )JS";

    chrome->evaluate(js, [this, mediaDir](const QJsonValue &v) {
        QJsonArray urls = v.toArray();
        if (urls.isEmpty()) {
            log("미디어 URL 없음", "info");
            return;
        }
        log(QString("미디어 %1개 다운로드 시작").arg(urls.size()), "info");

        // 백그라운드 다운로드 — Qt6 QNetworkAccessManager
        QStringList urlList;
        for (const auto &u : urls) urlList << u.toString();

        QThread *t = QThread::create([this, urlList, mediaDir]() {
            int ok = 0, fail = 0;
            for (const QString &u : urlList) {
                if (m_autoCrawlStop.load()) break;
                // sanitize filename from URL path
                QUrl qu(u);
                QString fname = qu.fileName();
                if (fname.isEmpty()) fname = "media_" + QString::number(ok + fail);
                fname.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
                if (fname.length() > 100) fname = fname.left(100);
                QString outPath = mediaDir + "/" + fname;
                if (QFile::exists(outPath)) { ok++; continue; }
                // curl로 다운로드 (Qt 의존성 최소화)
                QProcess curl;
                curl.start("curl", {"-sSL", "-A",
                    "Mozilla/5.0 (Macintosh; Intel Mac OS X) AppleWebKit/605.1.15",
                    "-o", outPath, u, "--max-time", "30"});
                if (curl.waitForFinished(35000) && curl.exitCode() == 0 && QFileInfo(outPath).size() > 100) {
                    ok++;
                    m_autoMediaCount++;
                    // ★ Chernobyl 식 메타데이터 — 원본 URL 박기
                    FileHelper::setDownloadMeta(outPath, u);
                    FileHelper::setFinderComment(outPath, u);
                    // ★ WebDAV 자동 업로드
                    QString outCopy = outPath;
                    QMetaObject::invokeMethod(this, [this, outCopy]() {
                        enqueueWebDavUpload(outCopy);
                    }, Qt::QueuedConnection);
                } else {
                    fail++;
                    QFile::remove(outPath);
                }
            }
            QMetaObject::invokeMethod(this, [this, ok, fail]() {
                log(QString("미디어 다운: 성공 %1 / 실패 %2").arg(ok).arg(fail), ok > 0 ? "success" : "warning");
            }, Qt::QueuedConnection);
        });
        connect(t, &QThread::finished, t, &QThread::deleteLater);
        t->start();
    });
}

// ═════════════════════════════════════════════════════════════════════════
// WebDAV NAS 업로드 (Synology 등)
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::setWebDavConfig(const QString &url, const QString &user, const QString &pass, bool enabled)
{
    m_config->setWebdavUrl(url);
    m_config->setWebdavUser(user);
    m_config->setWebdavPass(pass);
    m_config->setWebdavEnabled(enabled);
    m_config->save();
    if (m_webdav) {
        m_webdav->setConfig(url, user, pass, QString(), enabled);
    }
    log(QString("WebDAV 설정 저장: %1 (활성화=%2)").arg(url).arg(enabled ? "ON" : "OFF"), "success");
}

void PenBackend::testWebDavConnection()
{
    QString url  = m_config->webdavUrl();
    QString user = m_config->webdavUser();
    QString pass = m_config->webdavPass();
    if (url.isEmpty()) { log("WebDAV URL 미설정", "warning"); return; }
    log(QString("WebDAV 연결 테스트: %1").arg(url), "info");
    QThread *t = QThread::create([this, url, user, pass]() {
        QProcess curl;
        curl.start("curl", {
            "-sS", "-k", "-X", "OPTIONS",
            "-u", user + ":" + pass,
            "--max-time", "10",
            "-w", "HTTP_CODE=%{http_code}",
            url
        });
        bool fin = curl.waitForFinished(15000);
        QString out = QString::fromUtf8(curl.readAllStandardOutput());
        QMetaObject::invokeMethod(this, [this, fin, out]() {
            if (!fin) { log("WebDAV 연결 타임아웃", "error"); return; }
            if (out.contains("HTTP_CODE=200") || out.contains("HTTP_CODE=207"))
                log("✅ WebDAV 연결 성공!", "success");
            else if (out.contains("HTTP_CODE=401"))
                log("❌ 인증 실패 — 사용자/비번 확인", "error");
            else if (out.contains("HTTP_CODE=404"))
                log("❌ URL 경로 없음 — base URL 확인", "error");
            else
                log(QString("❌ WebDAV 응답 비정상: %1").arg(out.left(150)), "error");
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

void PenBackend::enqueueWebDavUpload(const QString &localPath)
{
    if (m_webdav && m_webdav->isEnabled()) {
        m_webdav->enqueue(localPath);
    }
}

// ═════════════════════════════════════════════════════════════════════════
// Finder에 WebDAV 마운트 — AppleScript "mount volume" 호출.
//   Finder가 OS 차원에서 권한/인증 처리 → "권한 없음" 우회.
//   마운트되면 /Volumes/<공유폴더> 생성 → Finder 사이드바 표시.
//   사용자가 그 폴더를 저장 경로로 선택하면 다운로드가 NAS 로 직행.
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::mountWebDavInFinder()
{
    if (!m_config) { log("Config 미초기화", "error", "crawl"); return; }
    QString url  = m_config->webdavUrl();
    QString user = m_config->webdavUser();
    QString pass = m_config->webdavPass();
    if (url.isEmpty()) {
        log("WebDAV URL 미설정 — 먼저 URL 입력하세요", "warning", "crawl");
        return;
    }
    log(QString("Finder에 마운트 시도: %1").arg(url), "info", "crawl");

    auto esc = [](const QString &s) {
        QString r = s;
        r.replace("\\", "\\\\");
        r.replace("\"", "\\\"");
        return r;
    };

    QString script;
    if (user.isEmpty()) {
        script = QString("mount volume \"%1\"").arg(esc(url));
    } else {
        script = QString("mount volume \"%1\" as user name \"%2\" with password \"%3\"")
            .arg(esc(url), esc(user), esc(pass));
    }

    QThread *t = QThread::create([this, script]() {
        QStringList before;
        { QDir d("/Volumes"); before = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot); }

        QProcess osa;
        osa.start("osascript", {"-e", script});
        bool fin = osa.waitForFinished(60000);
        QString stderrS = QString::fromUtf8(osa.readAllStandardError()).trimmed();
        int code = osa.exitCode();

        QStringList after;
        { QDir d("/Volumes"); after = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot); }
        QStringList newVols;
        for (const QString &n : after) if (!before.contains(n)) newVols << n;

        QMetaObject::invokeMethod(this, [this, fin, code, stderrS, newVols]() {
            if (!fin) {
                log("❌ 마운트 타임아웃 (60초)", "error", "crawl");
                return;
            }
            if (code != 0) {
                QString hint;
                if (stderrS.contains("-128")) hint = " (사용자 취소)";
                else if (stderrS.contains("-1714") || stderrS.contains("invalid")) hint = " — URL/사용자/비번 확인";
                else if (stderrS.contains("-43")) hint = " — URL 경로 확인";
                log(QString("❌ 마운트 실패%1: %2").arg(hint, stderrS.left(150)), "error", "crawl");
                return;
            }
            if (!newVols.isEmpty()) {
                QString p = "/Volumes/" + newVols.first();
                log(QString("✅ Finder에 마운트됨: %1").arg(p), "success", "crawl");
                log("   ► 저장 경로를 위 경로로 변경하면 다운로드가 NAS로 직행", "info", "crawl");
                QJsonObject info{{"path", p}, {"name", newVols.first()}};
                runJs(QString("if(window.onWebDavMounted) onWebDavMounted(%1);")
                      .arg(QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Compact))));
            } else {
                log("✅ 마운트 명령 성공 — Finder 사이드바 확인", "success", "crawl");
            }
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

void PenBackend::openSecurityPrefs()
{
#ifdef Q_OS_MACOS
    QProcess::startDetached("open", {"x-apple.systempreferences:com.apple.preference.security?Privacy_FilesAndFolders"});
#endif
    log("시스템 설정 → 개인정보 보호 및 보안 → 파일 및 폴더 → Pen 권한 ✅", "info", "crawl");
}

// 마운트된 볼륨 목록 (NAS/외장)
void PenBackend::listMountedVolumes()
{
    QJsonArray vols;
#ifdef Q_OS_MACOS
    QDir d("/Volumes");
    QStringList entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);

    QProcess df;
    df.start("df", {"-T", "webdav,smbfs,afpfs,nfs,fuse"});
    df.waitForFinished(5000);
    QString dfOut = QString::fromUtf8(df.readAllStandardOutput());

    QSet<QString> networkMounts;
    for (const QString &line : dfOut.split('\n', Qt::SkipEmptyParts)) {
        QStringList cols = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (cols.size() >= 2) {
            QString mp = cols.last();
            if (mp.startsWith("/Volumes/")) {
                networkMounts.insert(mp.mid(QString("/Volumes/").length()));
            }
        }
    }

    for (const QString &name : entries) {
        QString path = "/Volumes/" + name;
        QFileInfo fi(path);
        if (!fi.isDir() || !fi.isWritable()) continue;
        if (QFileInfo("/Volumes/" + name + "/System/Library").exists()) continue;

        QJsonObject o;
        o["path"] = path;
        o["name"] = name;
        o["isNetwork"] = networkMounts.contains(name);
        vols.append(o);
    }
#endif
    QString json = QString::fromUtf8(QJsonDocument(vols).toJson(QJsonDocument::Compact));
    runJs(QString("if(window.onMountedVolumes) onMountedVolumes(%1);").arg(json));
}

// Qt native dialog 로 마운트된 볼륨 선택
#include <QInputDialog>
void PenBackend::pickMountedVolume(const QString &targetInputId)
{
    QStringList paths, items;
#ifdef Q_OS_MACOS
    QDir d("/Volumes");
    QStringList entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
    QProcess df;
    df.start("df", {"-T", "webdav,smbfs,afpfs,nfs,fuse"});
    df.waitForFinished(5000);
    QString dfOut = QString::fromUtf8(df.readAllStandardOutput());
    QSet<QString> networkMounts;
    for (const QString &line : dfOut.split('\n', Qt::SkipEmptyParts)) {
        QStringList cols = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (cols.size() >= 2) {
            QString mp = cols.last();
            if (mp.startsWith("/Volumes/")) networkMounts.insert(mp.mid(QString("/Volumes/").length()));
        }
    }
    for (const QString &name : entries) {
        QString path = "/Volumes/" + name;
        QFileInfo fi(path);
        if (!fi.isDir() || !fi.isWritable()) continue;
        if (QFileInfo("/Volumes/" + name + "/System/Library").exists()) continue;
        paths << path;
        items << QString("%1 %2  (%3)").arg(networkMounts.contains(name) ? "🌐" : "💾", name, path);
    }
#endif
    if (items.isEmpty()) {
        log("마운트된 NAS/외장 없음 — WebDAV 카드에서 Finder 마운트 먼저", "warning", "crawl");
        runJs("alert('마운트된 NAS/외장 디스크가 없습니다.\\n\\nWebDAV 카드 → \"📂 Finder 마운트\" 먼저 누르세요.');");
        return;
    }
    QMetaObject::invokeMethod(this, [this, items, paths, targetInputId]() {
        // 1단계: 볼륨 선택
        bool ok = false;
        QString chosen = QInputDialog::getItem(m_window, "1단계: NAS/외장 볼륨 선택",
            "저장 경로 — 어느 볼륨?", items, 0, false, &ok);
        if (!ok || chosen.isEmpty()) return;
        int idx = items.indexOf(chosen);
        if (idx < 0 || idx >= paths.size()) return;
        QString volumeRoot = paths[idx];

        // 2단계: 그 볼륨 안에서 폴더 선택 (Finder dialog — 새 폴더 만들기 가능)
        QString chosenPath = QFileDialog::getExistingDirectory(
            m_window,
            "2단계: 폴더 선택 (없으면 새로 만드세요)",
            volumeRoot,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
        );
        if (chosenPath.isEmpty()) return;
        if (!chosenPath.startsWith(volumeRoot)) {
            log(QString("⚠ 선택한 폴더가 %1 안에 없음 — 무시").arg(volumeRoot), "warning", "crawl");
            return;
        }
        QDir().mkpath(chosenPath);
        log(QString("[저장경로] 선택: %1").arg(chosenPath), "success", "crawl");
        QString js = QString(
            "(function(){var el=document.getElementById(%1);"
            "if(el){el.value=%2;el.dispatchEvent(new Event('change'));el.dispatchEvent(new Event('input'));"
            "el.style.transition='background 0.3s';el.style.background='#1f3a1f';"
            "setTimeout(function(){el.style.background='';},600);}})();"
        ).arg(
            QString::fromUtf8(QJsonDocument::fromVariant(targetInputId).toJson(QJsonDocument::Compact)),
            QString::fromUtf8(QJsonDocument::fromVariant(chosenPath).toJson(QJsonDocument::Compact))
        );
        runJs(js);
    }, Qt::QueuedConnection);
}

// ═════════════════════════════════════════════════════════════════════════
// crawlSiteMirror — 사이트 통째 미러 (wget -mk / HTTrack 현대식)
//   1) BFS — 같은 도메인 링크 따라가며 각 페이지 Chrome 으로 navigate
//   2) SingleFile 로 self-contained HTML 캡쳐 (이미지/CSS 인라인)
//   3) 모든 페이지 캡쳐 끝나면 각 HTML 의 a[href] 재작성 → 로컬 mirror 안에서 클릭 가능
//   4) index.html 생성 (전체 페이지 목록)
//   crawlAuto 와 차이: 링크 재작성 + 인덱스 → 진짜 미러 (오프라인 네비게이션 가능)
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::crawlSiteMirror(const QString &startUrl, int maxPages, bool sameDomain, bool downloadMedia, int maxDepth)
{
    if (startUrl.isEmpty()) {
        log("사이트 미러: 시작 URL 필요", "error");
        return;
    }
    {
        QMutexLocker lock(&m_crawlMutex);
        if (!m_crawlChrome || !m_crawlChrome->isReady()) {
            log("Chrome 준비 안 됨 — 시작 먼저 누르세요", "warning");
            return;
        }
    }
    m_autoCrawlStop.store(false);
    m_autoPageCount = 0;
    m_autoMediaCount = 0;
    m_mirrorUrlMap.clear();

    log(QString("━━ 사이트 미러 시작: %1 (최대 %2페이지, 깊이 %3) ━━").arg(startUrl)
            .arg(maxPages > 0 ? QString::number(maxPages) : "무제한")
            .arg(maxDepth >= 999 ? "∞" : QString::number(maxDepth)), "info");

    int maxP = maxPages;
    int maxD = maxDepth;
    QString start = startUrl;
    bool sd = sameDomain;
    bool dm = downloadMedia;
    QString savePath = m_crawlSavePath;

    QThread *t = QThread::create([this, start, maxP, maxD, sd, dm, savePath]() {
        QSet<QString> visited;
        QQueue<QPair<QString, int>> queue;  // <URL, depth>
        queue.enqueue({start, 0});
        QUrl startQ(start);
        QString rootHost = startQ.host();

        // URL → 로컬 파일 경로 매핑 빌더
        auto urlToLocalPath = [&savePath](const QString &url) -> QString {
            QUrl qu(url);
            QString host = qu.host();
            QString path = qu.path();
            if (path.isEmpty() || path == "/") path = "/index.html";
            else if (path.endsWith("/")) path += "index.html";
            else if (!path.contains('.')) path += ".html";
            // 안전 파일명
            path.replace(QRegularExpression("[?#:*<>|\"\\\\]"), "_");
            return savePath + "/mirror/" + host + path;
        };

        // 1) BFS 캡쳐 — depth 추적
        while (!queue.isEmpty() && !m_autoCrawlStop.load()) {
            auto [url, curDepth] = queue.dequeue();
            if (visited.contains(url)) continue;
            visited.insert(url);

            QString localPath = urlToLocalPath(url);
            QDir().mkpath(QFileInfo(localPath).absolutePath());

            // URL → 로컬 경로 매핑 저장 (재작성 시 사용)
            m_mirrorUrlMap[url] = localPath;
            if (url.endsWith("/")) m_mirrorUrlMap[url.left(url.length() - 1)] = localPath;
            else m_mirrorUrlMap[url + "/"] = localPath;

            QMetaObject::invokeMethod(this, [this, url, curDepth]() {
                log(QString("[%1] (d=%2) %3").arg(m_autoPageCount + 1).arg(curDepth).arg(url), "info");
            }, Qt::QueuedConnection);

            // navigate
            auto navSem = std::make_shared<QSemaphore>(0);
            auto navOk = std::make_shared<bool>(false);
            QMetaObject::invokeMethod(this, [this, url, navSem, navOk]() {
                if (!m_crawlChrome) { navSem->release(); return; }
                m_crawlChrome->navigate(url, [navSem, navOk](bool ok) {
                    *navOk = ok; navSem->release();
                });
            }, Qt::QueuedConnection);
            navSem->tryAcquire(1, 30000);
            if (!*navOk) {
                QMetaObject::invokeMethod(this, [this, url]() {
                    log(QString("✗ navigate 실패: %1").arg(url), "warning");
                }, Qt::QueuedConnection);
                continue;
            }
            QThread::sleep(2);

            // 캡쳐 — 직접 chrome 호출 (savePath 가 매번 다름)
            auto capSem = std::make_shared<QSemaphore>(0);
            QString libCode;
            {
                QMutexLocker lock(&m_crawlMutex);
                libCode = s_singleFileLibCode;
            }
            if (libCode.isEmpty()) {
                // 캐시 로드
                QString p = QCoreApplication::applicationDirPath() + "/../Resources/tools/singlefile_extension/lib/single-file.js";
                QFile f(p);
                if (f.exists() && f.open(QIODevice::ReadOnly)) {
                    libCode = QString::fromUtf8(f.readAll());
                    f.close();
                }
            }
            QString sfCall = R"JS(
                (async () => {
                    if (typeof singlefile === 'undefined') return {error:'no lib'};
                    try {
                        const data = await singlefile.getPageData({
                            removeHiddenElements:true, removeUnusedStyles:true,
                            removeUnusedFonts:true, removeFrames:true,
                            removeImports:true, removeScripts:true,
                            blockScripts:true, compressHTML:true,
                            insertSingleFileComment:true
                        });
                        window.__penContent = data.content;
                        return {size: data.content.length, url: data.url || location.href, chunked: true};
                    } catch(e) { return {error: String(e)}; }
                })()
            )JS";
            QString chromeLocalPath = localPath;
            QString pageUrl = url;  // structured binding → 일반 변수로 복사 (lambda capture 용)
            QMetaObject::invokeMethod(this, [this, libCode, sfCall, chromeLocalPath, pageUrl, capSem]() {
                if (!m_crawlChrome) { capSem->release(); return; }
                m_crawlChrome->evaluate(libCode, [this, sfCall, chromeLocalPath, pageUrl, capSem](const QJsonValue &) {
                    if (!m_crawlChrome) { capSem->release(); return; }
                    m_crawlChrome->evaluate(sfCall, [this, chromeLocalPath, capSem, pageUrl](const QJsonValue &v) {
                        QJsonObject obj = v.toObject();
                        if (obj.contains("error") || !obj["chunked"].toBool()) {
                            capSem->release();
                            return;
                        }
                        qint64 total = obj["size"].toVariant().toLongLong();
                        if (total <= 0) { capSem->release(); return; }
                        QFile *out = new QFile(chromeLocalPath);
                        if (!out->open(QIODevice::WriteOnly)) { delete out; capSem->release(); return; }
                        const qint64 CHUNK = 1024 * 1024;
                        auto off = std::make_shared<qint64>(0);
                        auto fn = std::make_shared<std::function<void()>>();
                        *fn = [this, out, total, off, fn, capSem, chromeLocalPath, pageUrl]() {
                            if (*off >= total) {
                                out->close(); delete out;
                                m_crawlChrome->evaluate("delete window.__penContent;", [](const QJsonValue&){});
                                // ★ Chernobyl 식 메타데이터
                                if (!pageUrl.isEmpty()) {
                                    FileHelper::setDownloadMeta(chromeLocalPath, pageUrl);
                                    FileHelper::setFinderComment(chromeLocalPath, pageUrl);
                                }
                                // ★ WebDAV 자동 업로드
                                QMetaObject::invokeMethod(this, [this, chromeLocalPath]() {
                                    enqueueWebDavUpload(chromeLocalPath);
                                }, Qt::QueuedConnection);
                                capSem->release();
                                return;
                            }
                            qint64 end = qMin(*off + 1024 * 1024, total);
                            QString slice = QString("window.__penContent.slice(%1, %2)").arg(*off).arg(end);
                            m_crawlChrome->evaluate(slice, [out, off, fn, end](const QJsonValue &cv) {
                                QByteArray b = cv.toString().toUtf8();
                                out->write(b);
                                *off = end;
                                (*fn)();
                            });
                        };
                        (*fn)();
                    });
                });
            }, Qt::QueuedConnection);
            capSem->tryAcquire(1, 60000);
            m_autoPageCount++;

            if (maxP > 0 && m_autoPageCount >= maxP) break;

            // 다음 페이지 링크 추출
            auto linkSem = std::make_shared<QSemaphore>(0);
            auto linkList = std::make_shared<QJsonArray>();
            QMetaObject::invokeMethod(this, [this, linkSem, linkList]() {
                if (!m_crawlChrome) { linkSem->release(); return; }
                m_crawlChrome->evaluate(
                    "Array.from(document.querySelectorAll('a[href]')).map(a=>a.href).filter(u=>u && u.startsWith('http')).slice(0,200)",
                    [linkSem, linkList](const QJsonValue &v) {
                        *linkList = v.toArray();
                        linkSem->release();
                    });
            }, Qt::QueuedConnection);
            linkSem->tryAcquire(1, 10000);

            // 현재 depth 가 maxD 도달하면 자식 링크 enqueue 안 함 (BFS 깊이 제한)
            if (curDepth >= maxD) {
                continue;
            }
            int added = 0;
            for (const auto &v : *linkList) {
                QString u = v.toString();
                if (u.isEmpty() || visited.contains(u)) continue;
                int hashIdx = u.indexOf('#');
                if (hashIdx > 0) u = u.left(hashIdx);
                if (visited.contains(u)) continue;
                if (sd) {
                    QUrl qu(u);
                    if (qu.host() != rootHost) continue;
                }
                queue.enqueue({u, curDepth + 1});
                if (++added >= 50) break;
            }
        }

        // 2) 링크 재작성 — 각 HTML 의 a[href] 와 src 를 로컬 경로로
        QMetaObject::invokeMethod(this, [this]() {
            log(QString("[2/3] 링크 재작성 중... (%1개 파일)").arg(m_mirrorUrlMap.size()), "info");
        }, Qt::QueuedConnection);

        for (auto it = m_mirrorUrlMap.constBegin(); it != m_mirrorUrlMap.constEnd(); ++it) {
            if (m_autoCrawlStop.load()) break;
            const QString &localPath = it.value();
            QFile f(localPath);
            if (!f.exists() || !f.open(QIODevice::ReadOnly)) continue;
            QByteArray content = f.readAll();
            f.close();

            QString html = QString::fromUtf8(content);
            // 매핑된 URL → 상대 경로 치환
            QString baseDir = QFileInfo(localPath).absolutePath();
            for (auto m = m_mirrorUrlMap.constBegin(); m != m_mirrorUrlMap.constEnd(); ++m) {
                const QString &srcUrl = m.key();
                const QString &dstPath = m.value();
                if (srcUrl == it.key()) continue;  // 자기 자신 제외
                QString relPath = QDir(baseDir).relativeFilePath(dstPath);
                // href 와 src 안의 정확 매칭 (URL 인코딩 차이 회피 위해 단순 치환)
                QString quoted1 = "\"" + srcUrl + "\"";
                QString quoted2 = "'" + srcUrl + "'";
                if (html.contains(quoted1)) html.replace(quoted1, "\"" + relPath + "\"");
                if (html.contains(quoted2)) html.replace(quoted2, "'" + relPath + "'");
            }

            // 다시 쓰기
            if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                f.write(html.toUtf8());
                f.close();
            }
        }

        // 3) index.html 생성 — 전체 페이지 리스트
        QString indexPath = savePath + "/mirror/index.html";
        QFile idx(indexPath);
        if (idx.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QString html = "<!DOCTYPE html><html><head><meta charset=utf-8>";
            html += "<title>사이트 미러 인덱스</title>";
            html += "<style>body{font-family:system-ui;padding:20px;background:#0e0e10;color:#e7e7ea}";
            html += "a{color:#7c5cff;text-decoration:none;display:block;padding:6px 0}";
            html += "a:hover{text-decoration:underline}";
            html += "h1{font-size:18px;border-bottom:1px solid #333;padding-bottom:8px}";
            html += ".count{color:#888;font-size:13px;margin-bottom:16px}";
            html += "</style></head><body>";
            html += "<h1>사이트 미러</h1>";
            html += QString("<div class=count>총 %1개 페이지 — %2</div>")
                .arg(m_mirrorUrlMap.size()).arg(QDateTime::currentDateTime().toString());

            for (auto it = m_mirrorUrlMap.constBegin(); it != m_mirrorUrlMap.constEnd(); ++it) {
                QString relPath = QDir(savePath + "/mirror").relativeFilePath(it.value());
                QString displayUrl = it.key();
                html += QString("<a href=\"%1\">%2</a>").arg(relPath, displayUrl.toHtmlEscaped());
            }
            html += "</body></html>";
            idx.write(html.toUtf8());
            idx.close();
            // ★ WebDAV 자동 업로드 (index 도)
            QMetaObject::invokeMethod(this, [this, indexPath]() {
                enqueueWebDavUpload(indexPath);
            }, Qt::QueuedConnection);
        }

        QMetaObject::invokeMethod(this, [this, indexPath]() {
            log(QString("━━ 사이트 미러 완료 — 페이지 %1개").arg(m_autoPageCount), "success");
            log(QString("  인덱스: %1").arg(indexPath), "info");
            log("  index.html 더블클릭 → 브라우저에서 로컬 mirror 네비게이션 가능", "info");
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

// ═════════════════════════════════════════════════════════════════════════
// crawlDeepMirror — 진짜 풀 미러 (wget -mkp 동등 + Chrome JS 렌더)
//   wget/HTTrack 의 한계 (정적 HTML 만) 극복 — Chrome 으로 렌더 후 모든 리소스 캡쳐.
//   페이지마다:
//     1) navigate → render → 안정화 대기
//     2) JS 로 모든 리소스 URL 수집 (img/script/link/source + performance.getEntriesByType + CSS bg)
//     3) curl 로 각 리소스 다운로드 → 원본 도메인/path 구조 그대로 로컬 저장
//     4) raw HTML 가져오기 (SingleFile inline X — 원본 그대로)
//     5) HTML 안의 URL → 로컬 상대경로 치환 (절대 URL + 도메인 매칭)
//     6) 저장
//   BFS 로 같은 도메인 다음 페이지 → 반복.
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::crawlDeepMirror(const QString &startUrl, int maxPages, bool sameDomain, int maxDepth)
{
    if (startUrl.isEmpty()) {
        log("딥 미러: 시작 URL 필요", "error");
        return;
    }
    {
        QMutexLocker lock(&m_crawlMutex);
        if (!m_crawlChrome || !m_crawlChrome->isReady()) {
            log("Chrome 준비 안 됨 — 시작 먼저 누르세요", "warning");
            return;
        }
    }
    m_autoCrawlStop.store(false);
    m_autoPageCount = 0;
    m_autoMediaCount = 0;
    m_mirrorUrlMap.clear();

    log(QString("━━ 딥 미러 시작: %1 (페이지 %2, 깊이 %3) ━━").arg(startUrl)
            .arg(maxPages).arg(maxDepth >= 999 ? "∞" : QString::number(maxDepth)), "info");

    int maxP = maxPages;
    int maxD = maxDepth;
    QString start = startUrl;
    bool sd = sameDomain;
    QString savePath = m_crawlSavePath;
    QString mirrorRoot = savePath + "/deep_mirror";
    QDir().mkpath(mirrorRoot);

    QThread *t = QThread::create([this, start, maxP, maxD, sd, mirrorRoot]() {
        QSet<QString> visitedPages;
        QSet<QString> downloadedRes;
        QQueue<QPair<QString, int>> pageQueue;  // <URL, depth>
        pageQueue.enqueue({start, 0});
        QUrl startQ(start);
        QString rootHost = startQ.host();

        // URL → 로컬 파일 경로 매핑 (도메인/path 구조 보존)
        auto urlToLocal = [&mirrorRoot](const QString &url) -> QString {
            QUrl qu(url);
            QString host = qu.host();
            if (host.isEmpty()) return QString();
            QString path = qu.path();
            // query 도 파일명에 포함 (단, 안전화)
            QString query = qu.query();
            if (!query.isEmpty()) {
                QString qs = QString::fromUtf8(QCryptographicHash::hash(query.toUtf8(), QCryptographicHash::Md5).toHex().left(8));
                path += "_" + qs;
            }
            if (path.isEmpty() || path == "/") path = "/index.html";
            else if (path.endsWith("/")) path += "index.html";
            else if (!QFileInfo(path).fileName().contains('.')) path += ".html";
            // 안전화
            path.replace(QRegularExpression("[?#:*<>|\"\\\\]"), "_");
            return mirrorRoot + "/" + host + path;
        };

        while (!pageQueue.isEmpty() && !m_autoCrawlStop.load()) {
            auto [pageUrl, curDepth] = pageQueue.dequeue();
            if (visitedPages.contains(pageUrl)) continue;
            visitedPages.insert(pageUrl);
            m_autoPageCount++;

            if (maxP > 0 && m_autoPageCount > maxP) break;

            QString pageLocal = urlToLocal(pageUrl);
            if (pageLocal.isEmpty()) continue;
            QDir().mkpath(QFileInfo(pageLocal).absolutePath());
            m_mirrorUrlMap[pageUrl] = pageLocal;

            QMetaObject::invokeMethod(this, [this, pageUrl, n = m_autoPageCount]() {
                log(QString("[%1] navigate → %2").arg(n).arg(pageUrl), "info");
            }, Qt::QueuedConnection);

            // 1) navigate
            auto navSem = std::make_shared<QSemaphore>(0);
            auto navOk = std::make_shared<bool>(false);
            QMetaObject::invokeMethod(this, [this, pageUrl, navSem, navOk]() {
                if (!m_crawlChrome) { navSem->release(); return; }
                m_crawlChrome->navigate(pageUrl, [navSem, navOk](bool ok) {
                    *navOk = ok; navSem->release();
                });
            }, Qt::QueuedConnection);
            navSem->tryAcquire(1, 30000);
            if (!*navOk) {
                QMetaObject::invokeMethod(this, [this, pageUrl]() {
                    log(QString("✗ navigate 실패: %1").arg(pageUrl), "warning");
                }, Qt::QueuedConnection);
                continue;
            }
            QThread::sleep(3);  // JS render + 모든 리소스 로드 대기

            // 2) 모든 리소스 URL + raw HTML + 페이지 안 링크 한 번에 추출
            auto extractSem = std::make_shared<QSemaphore>(0);
            auto extractResult = std::make_shared<QJsonObject>();
            QString extractJs = R"JS(
                (() => {
                    const urls = new Set();
                    // img/script/link/source/video/audio
                    document.querySelectorAll('img[src],script[src],link[href],source[src],video[src],audio[src]').forEach(el => {
                        const u = el.src || el.href;
                        if (u && u.startsWith('http')) urls.add(u);
                    });
                    // srcset
                    document.querySelectorAll('[srcset]').forEach(el => {
                        (el.srcset || '').split(',').forEach(s => {
                            const u = s.trim().split(' ')[0];
                            if (u && u.startsWith('http')) urls.add(u);
                        });
                    });
                    // performance API — 실제 로드된 모든 리소스
                    try {
                        performance.getEntriesByType('resource').forEach(e => {
                            if (e.name && e.name.startsWith('http')) urls.add(e.name);
                        });
                    } catch(e){}
                    // CSS background-image (computed)
                    document.querySelectorAll('*').forEach(el => {
                        try {
                            const bg = getComputedStyle(el).backgroundImage;
                            const m = bg && bg.match(/url\(["']?(https?:\/\/[^"')]+)/);
                            if (m) urls.add(m[1]);
                        } catch(e){}
                    });
                    // 페이지 링크 (BFS 다음용)
                    const links = Array.from(document.querySelectorAll('a[href]'))
                        .map(a => a.href)
                        .filter(u => u && u.startsWith('http'))
                        .slice(0, 200);
                    return {
                        resources: Array.from(urls).slice(0, 500),
                        links: links,
                        html: document.documentElement.outerHTML
                    };
                })()
            )JS";
            QMetaObject::invokeMethod(this, [this, extractJs, extractSem, extractResult]() {
                if (!m_crawlChrome) { extractSem->release(); return; }
                m_crawlChrome->evaluate(extractJs, [extractSem, extractResult](const QJsonValue &v) {
                    *extractResult = v.toObject();
                    extractSem->release();
                });
            }, Qt::QueuedConnection);
            extractSem->tryAcquire(1, 30000);

            QJsonArray resources = (*extractResult)["resources"].toArray();
            QJsonArray links = (*extractResult)["links"].toArray();
            QString html = (*extractResult)["html"].toString();
            *extractResult = QJsonObject();

            QMetaObject::invokeMethod(this, [this, n = resources.size()]() {
                log(QString("  리소스 %1개 발견").arg(n), "info");
            }, Qt::QueuedConnection);

            // 3) 각 리소스 다운로드 — curl 병렬 (4개씩)
            QStringList urlList;
            for (const auto &v : resources) urlList << v.toString();

            int dlOk = 0, dlSkip = 0;
            for (const QString &resUrl : urlList) {
                if (m_autoCrawlStop.load()) break;
                if (downloadedRes.contains(resUrl)) { dlSkip++; continue; }
                downloadedRes.insert(resUrl);
                QString resLocal = urlToLocal(resUrl);
                if (resLocal.isEmpty()) continue;
                m_mirrorUrlMap[resUrl] = resLocal;
                if (QFile::exists(resLocal)) { dlSkip++; continue; }
                QDir().mkpath(QFileInfo(resLocal).absolutePath());

                QProcess curl;
                curl.start("curl", {
                    "-sSL", "-k",
                    "-A", "Mozilla/5.0 (Macintosh; Intel Mac OS X) AppleWebKit/605.1.15",
                    "-o", resLocal,
                    "--max-time", "30",
                    resUrl
                });
                if (curl.waitForFinished(35000) && curl.exitCode() == 0
                    && QFileInfo(resLocal).size() > 0) {
                    dlOk++;
                } else {
                    QFile::remove(resLocal);
                }
            }
            QMetaObject::invokeMethod(this, [this, dlOk, dlSkip]() {
                log(QString("  다운로드 %1개 (스킵 %2개)").arg(dlOk).arg(dlSkip), "info");
            }, Qt::QueuedConnection);

            // 4) HTML 안의 URL → 로컬 상대 경로 치환
            QString pageBaseDir = QFileInfo(pageLocal).absolutePath();
            for (auto it = m_mirrorUrlMap.constBegin(); it != m_mirrorUrlMap.constEnd(); ++it) {
                if (it.key() == pageUrl) continue;
                QString rel = QDir(pageBaseDir).relativeFilePath(it.value());
                // 정확 매칭 우선 (quoted)
                if (html.contains("\"" + it.key() + "\"")) html.replace("\"" + it.key() + "\"", "\"" + rel + "\"");
                if (html.contains("'" + it.key() + "'")) html.replace("'" + it.key() + "'", "'" + rel + "'");
                // 비-quote 도 (srcset 등)
                if (html.contains(it.key() + " ")) html.replace(it.key() + " ", rel + " ");
            }

            // 5) HTML 저장
            QFile pf(pageLocal);
            if (pf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                pf.write(html.toUtf8());
                pf.close();
                // ★ Chernobyl 식 메타데이터
                FileHelper::setDownloadMeta(pageLocal, pageUrl);
                FileHelper::setFinderComment(pageLocal, pageUrl);
                // ★ WebDAV 자동 업로드
                QMetaObject::invokeMethod(this, [this, pageLocal]() {
                    enqueueWebDavUpload(pageLocal);
                }, Qt::QueuedConnection);
            }

            // 6) BFS 다음 페이지 큐 추가 — depth 한계 도달 시 자식 enqueue 안 함
            if (curDepth >= maxD) continue;
            int added = 0;
            for (const auto &v : links) {
                QString u = v.toString();
                if (u.isEmpty() || visitedPages.contains(u)) continue;
                int hashIdx = u.indexOf('#');
                if (hashIdx > 0) u = u.left(hashIdx);
                if (visitedPages.contains(u)) continue;
                if (sd) {
                    QUrl qu(u);
                    if (qu.host() != rootHost) continue;
                }
                pageQueue.enqueue({u, curDepth + 1});
                if (++added >= 50) break;
            }
        }

        // 인덱스 생성
        QString indexPath = mirrorRoot + "/index.html";
        QFile idx(indexPath);
        if (idx.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QString html = "<!DOCTYPE html><html><head><meta charset=utf-8>";
            html += "<title>딥 미러 인덱스</title>";
            html += "<style>body{font-family:system-ui;padding:20px;background:#0e0e10;color:#e7e7ea}";
            html += "a{color:#7c5cff;text-decoration:none;display:block;padding:6px 0}";
            html += "a:hover{text-decoration:underline}";
            html += "h1{font-size:18px;border-bottom:1px solid #333;padding-bottom:8px}";
            html += ".count{color:#888;font-size:13px;margin-bottom:16px}";
            html += "</style></head><body>";
            html += "<h1>딥 미러 — 소스 + 모든 리소스</h1>";
            int pageCount = 0;
            for (auto it = m_mirrorUrlMap.constBegin(); it != m_mirrorUrlMap.constEnd(); ++it) {
                if (it.value().endsWith(".html")) pageCount++;
            }
            html += QString("<div class=count>페이지 %1개 — %2</div>").arg(pageCount).arg(QDateTime::currentDateTime().toString());
            for (auto it = m_mirrorUrlMap.constBegin(); it != m_mirrorUrlMap.constEnd(); ++it) {
                if (!it.value().endsWith(".html")) continue;
                QString rel = QDir(mirrorRoot).relativeFilePath(it.value());
                html += QString("<a href=\"%1\">%2</a>").arg(rel, it.key().toHtmlEscaped());
            }
            html += "</body></html>";
            idx.write(html.toUtf8());
            idx.close();
            // ★ WebDAV 자동 업로드 (index 도)
            QMetaObject::invokeMethod(this, [this, indexPath]() {
                enqueueWebDavUpload(indexPath);
            }, Qt::QueuedConnection);
        }

        QMetaObject::invokeMethod(this, [this, indexPath]() {
            log(QString("━━ 딥 미러 완료 — 페이지 %1개, 리소스 %2개")
                .arg(m_autoPageCount).arg(m_mirrorUrlMap.size() - m_autoPageCount), "success");
            log(QString("  index: %1").arg(indexPath), "info");
            log("  index.html 더블클릭 → 로컬 네비게이션 + 모든 이미지/CSS/JS 로컬 로드", "info");
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

// ═════════════════════════════════════════════════════════════════════════
// 터미널 로그 — 별도 Terminal.app 창에서 실시간 로그 표시 (Chernobyl 동일 패턴)
//   crawlStart 시 호출 → .command 스크립트 만들고 Terminal.app 으로 띄움
//   writeTerminalLog 가 로그 파일에 append → tail -f 가 실시간 표시
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::openTerminalLog()
{
    // 디스크 설정 + 임시 폴더 우선, 없으면 savePath
    QString scriptDir;
    if (m_config && !m_config->tempDir().isEmpty() && QDir(m_config->tempDir()).exists()) {
        scriptDir = m_config->tempDir();
    } else if (!m_crawlSavePath.isEmpty()) {
        scriptDir = m_crawlSavePath;
    } else {
        scriptDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    }
    QDir().mkpath(scriptDir);

    m_terminalLogPath = scriptDir + "/.pen_log.txt";
    QFile::remove(m_terminalLogPath);
    QFile f(m_terminalLogPath);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write("=========================================\n");
    f.write("  팬을 잘 쓰고 싶다 - 크롤 로그\n");
    f.write("=========================================\n\n");
    f.close();

#ifdef Q_OS_WIN
    QString scriptPath = scriptDir + "/pen_tail.bat";
    QFile script(scriptPath);
    if (script.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QString content;
        content += "@echo off\r\n";
        content += "chcp 65001 >nul\r\n";
        content += "title 팬을 잘 쓰고 싶다 - Log\r\n";
        content += ":loop\r\n";
        content += "cls\r\n";
        content += "type \"" + QDir::toNativeSeparators(m_terminalLogPath) + "\"\r\n";
        content += "timeout /t 2 /nobreak >nul\r\n";
        content += "findstr /C:\"[DONE]\" \"" + QDir::toNativeSeparators(m_terminalLogPath) + "\" >/dev/null 2>&1\r\n";
        content += "if %errorlevel%==0 (\r\n";
        content += "  echo.\r\n";
        content += "  echo 터미널을 닫아도 됩니다.\r\n";
        content += "  pause >nul\r\n";
        content += "  exit /b 0\r\n";
        content += ")\r\n";
        content += "goto loop\r\n";
        script.write(content.toUtf8());
        script.close();
    }
    QProcess::startDetached("cmd.exe", {"/c", "start", "팬을 잘 쓰고 싶다", QDir::toNativeSeparators(scriptPath)});
#else
    QString scriptPath = scriptDir + "/pen_tail.command";
    QFile script(scriptPath);
    if (script.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QString content;
        content += "#!/bin/bash\n";
        content += "clear\n";
        content += "cat '" + m_terminalLogPath + "'\n";
        content += "tail -f -n +0 '" + m_terminalLogPath + "' &\n";
        content += "TAIL_PID=$!\n";
        content += "while true; do\n";
        content += "  if grep -q '\\[DONE\\]' '" + m_terminalLogPath + "' 2>/dev/null; then\n";
        content += "    kill $TAIL_PID 2>/dev/null\n";
        content += "    echo ''\n";
        content += "    echo '터미널을 닫아도 됩니다.'\n";
        content += "    read -n 1\n";
        content += "    exit 0\n";
        content += "  fi\n";
        content += "  sleep 1\n";
        content += "done\n";
        script.write(content.toUtf8());
        script.close();
        script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                              QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                              QFileDevice::ReadOther | QFileDevice::ExeOther);
    }
    // xattr 제거 (TCC/quarantine 회피)
    QProcess::execute("xattr", {"-c", scriptPath});
    // Terminal.app 으로 열기 — fallback 단계별 시도
    bool opened = QProcess::startDetached("/usr/bin/open", {"-a", "Terminal.app", scriptPath});
    if (!opened) opened = QProcess::startDetached("/usr/bin/open", {scriptPath});
    if (!opened) {
        QString esc = QString(scriptPath).replace("\\", "\\\\").replace("\"", "\\\"");
        QString appleScript = QString(
            "tell application \"Terminal\"\n"
            "  activate\n"
            "  do script \"clear; '%1'; exit\"\n"
            "end tell"
        ).arg(esc);
        QProcess::startDetached("/usr/bin/osascript", {"-e", appleScript});
    }
#endif
}

void PenBackend::writeTerminalLog(const QString &message)
{
    if (m_terminalLogPath.isEmpty()) return;
    QFile f(m_terminalLogPath);
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        f.write((message + "\n").toUtf8());
        f.close();
    }
}

void PenBackend::closeTerminalLog()
{
    if (m_terminalLogPath.isEmpty()) return;
    QFile f(m_terminalLogPath);
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        f.write("\n[DONE] 크롤 종료\n");
        f.close();
    }
}

// ═════════════════════════════════════════════════════════════════════════
// crawlTwitterMirror — Twitter/X 사용자 프로필 통째 미러 (기능 보존)
//   1) 프로필 navigate + 자동 스크롤 → 모든 트윗 DOM 로드
//   2) article 에서 트윗 URL 추출 (/status/<id>)
//   3) 프로필 페이지 SingleFile 캡쳐
//   4) 각 트윗 URL 순차 캡쳐
//   5) 프로필 HTML 안의 트윗 링크 → 로컬 파일 경로 치환 → 클릭 작동
// ═════════════════════════════════════════════════════════════════════════
void PenBackend::crawlTwitterMirror(const QString &profileUrl, int maxScrolls)
{
    if (profileUrl.isEmpty()) {
        log("Twitter 미러: 프로필 URL 필요 (예: https://x.com/username)", "error");
        return;
    }
    {
        QMutexLocker lock(&m_crawlMutex);
        if (!m_crawlChrome || !m_crawlChrome->isReady()) {
            log("Chrome 준비 안 됨 — 시작 먼저 누르세요", "warning");
            return;
        }
    }
    m_autoCrawlStop.store(false);

    QString profile = profileUrl;
    int scrolls = qMax(5, maxScrolls);
    QString savePath = m_crawlSavePath;
    QString mirrorRoot = savePath + "/twitter_mirror";
    QDir().mkpath(mirrorRoot);

    log(QString("━━ Twitter 미러 시작: %1 (스크롤 %2회) ━━").arg(profile).arg(scrolls), "info");

    QThread *t = QThread::create([this, profile, scrolls, mirrorRoot]() {
        // 1) navigate 프로필
        QMetaObject::invokeMethod(this, [this, profile]() {
            log(QString("프로필 navigate: %1").arg(profile), "info");
        }, Qt::QueuedConnection);

        auto navSem = std::make_shared<QSemaphore>(0);
        auto navOk = std::make_shared<bool>(false);
        QMetaObject::invokeMethod(this, [this, profile, navSem, navOk]() {
            if (!m_crawlChrome) { navSem->release(); return; }
            m_crawlChrome->navigate(profile, [navSem, navOk](bool ok) {
                *navOk = ok; navSem->release();
            });
        }, Qt::QueuedConnection);
        navSem->tryAcquire(1, 30000);
        if (!*navOk) {
            QMetaObject::invokeMethod(this, [this]() { log("프로필 navigate 실패", "error"); }, Qt::QueuedConnection);
            return;
        }
        QThread::sleep(4);  // 트위터 첫 렌더 대기

        // 2) 자동 스크롤 — 트윗 lazy-load 트리거
        QMetaObject::invokeMethod(this, [this, scrolls]() {
            log(QString("자동 스크롤 (최대 %1회) — 트윗 lazy-load").arg(scrolls), "info");
        }, Qt::QueuedConnection);

        QString scrollJs = QString(R"JS(
            (async () => {
                const sleep = ms => new Promise(r => setTimeout(r, ms));
                let prevH = 0, same = 0;
                for (let i = 0; i < %1; i++) {
                    window.scrollTo(0, document.body.scrollHeight);
                    await sleep(800);
                    const curH = document.body.scrollHeight;
                    if (curH === prevH) { if (++same >= 4) break; }
                    else { same = 0; prevH = curH; }
                }
                // "더 보기" 버튼들 한 번 더 클릭
                document.querySelectorAll('button, [role="button"]').forEach(b => {
                    const t = (b.textContent || '').trim();
                    if (t.match(/^(더 보기|Show more|もっと見る|Load more)/i)) {
                        try { b.click(); } catch(e){}
                    }
                });
                await sleep(2000);
                return document.body.scrollHeight;
            })()
        )JS").arg(scrolls);

        auto scrollSem = std::make_shared<QSemaphore>(0);
        QMetaObject::invokeMethod(this, [this, scrollJs, scrollSem]() {
            if (!m_crawlChrome) { scrollSem->release(); return; }
            m_crawlChrome->evaluate(scrollJs, [scrollSem](const QJsonValue &) {
                scrollSem->release();
            });
        }, Qt::QueuedConnection);
        scrollSem->tryAcquire(1, 300000);  // 최대 5분

        // 3) 트윗 URL 추출 — article 안의 /status/<id> 링크
        QMetaObject::invokeMethod(this, [this]() { log("트윗 URL 추출 중...", "info"); }, Qt::QueuedConnection);

        QString extractJs = R"JS(
            (() => {
                const tweets = new Set();
                document.querySelectorAll('article a[href*="/status/"]').forEach(a => {
                    const m = a.href.match(/^(https:\/\/(?:x|twitter)\.com\/[^/]+\/status\/\d+)/);
                    if (m) tweets.add(m[1]);
                });
                return Array.from(tweets);
            })()
        )JS";
        auto extractSem = std::make_shared<QSemaphore>(0);
        auto tweetUrls = std::make_shared<QJsonArray>();
        QMetaObject::invokeMethod(this, [this, extractJs, extractSem, tweetUrls]() {
            if (!m_crawlChrome) { extractSem->release(); return; }
            m_crawlChrome->evaluate(extractJs, [extractSem, tweetUrls](const QJsonValue &v) {
                *tweetUrls = v.toArray();
                extractSem->release();
            });
        }, Qt::QueuedConnection);
        extractSem->tryAcquire(1, 30000);

        int tweetCount = tweetUrls->size();
        QMetaObject::invokeMethod(this, [this, tweetCount]() {
            log(QString("트윗 %1개 발견").arg(tweetCount), "success");
        }, Qt::QueuedConnection);

        // 4) 프로필 페이지 SingleFile 캡쳐
        QString profileFilename = "profile";
        QUrl pq(profile);
        QString user = pq.path();
        while (user.startsWith("/")) user = user.mid(1);
        if (!user.isEmpty()) profileFilename = "@" + user;

        QString profileLocal = mirrorRoot + "/" + profileFilename + ".html";
        // 직접 캡쳐 호출 (mirrorRoot 경로로)
        // crawlCaptureCurrent 는 m_crawlSavePath/captures/ 에 저장하므로 직접 안 부르고
        // 프로필 캡쳐 후 mirrorRoot 로 이동
        auto profCapSem = std::make_shared<QSemaphore>(0);
        QString libCode;
        QString p = QCoreApplication::applicationDirPath() + "/../Resources/tools/singlefile_extension/lib/single-file.js";
        QFile lf(p);
        if (lf.exists() && lf.open(QIODevice::ReadOnly)) {
            libCode = QString::fromUtf8(lf.readAll());
            lf.close();
        }
        QString sfCall = R"JS(
            (async () => {
                if (typeof singlefile === 'undefined') return {error:'no lib'};
                try {
                    const data = await singlefile.getPageData({
                        removeHiddenElements:false, removeUnusedStyles:false,
                        removeUnusedFonts:true, removeFrames:true,
                        removeImports:true, removeScripts:true,
                        blockScripts:true, compressHTML:true
                    });
                    window.__penContent = data.content;
                    return {size: data.content.length, chunked: true};
                } catch(e) { return {error: String(e)}; }
            })()
        )JS";
        QString profileSrcUrl = profile;
        QMetaObject::invokeMethod(this, [this, libCode, sfCall, profileLocal, profileSrcUrl, profCapSem]() {
            if (!m_crawlChrome) { profCapSem->release(); return; }
            m_crawlChrome->evaluate(libCode, [this, sfCall, profileLocal, profileSrcUrl, profCapSem](const QJsonValue &) {
                if (!m_crawlChrome) { profCapSem->release(); return; }
                m_crawlChrome->evaluate(sfCall, [this, profileLocal, profileSrcUrl, profCapSem](const QJsonValue &v) {
                    QJsonObject obj = v.toObject();
                    if (!obj["chunked"].toBool()) { profCapSem->release(); return; }
                    qint64 total = obj["size"].toVariant().toLongLong();
                    if (total <= 0) { profCapSem->release(); return; }
                    QFile *out = new QFile(profileLocal);
                    if (!out->open(QIODevice::WriteOnly)) { delete out; profCapSem->release(); return; }
                    auto off = std::make_shared<qint64>(0);
                    auto fn = std::make_shared<std::function<void()>>();
                    *fn = [this, out, total, off, fn, profileLocal, profileSrcUrl, profCapSem]() {
                        if (*off >= total) {
                            out->close(); delete out;
                            m_crawlChrome->evaluate("delete window.__penContent;", [](const QJsonValue&){});
                            // 체르노빌 패턴 — Where From + Finder comment
                            FileHelper::setDownloadMeta(profileLocal, profileSrcUrl);
                            FileHelper::setFinderComment(profileLocal, profileSrcUrl);
                            // ★ WebDAV 자동 업로드
                            QMetaObject::invokeMethod(this, [this, profileLocal]() {
                                enqueueWebDavUpload(profileLocal);
                            }, Qt::QueuedConnection);
                            profCapSem->release();
                            return;
                        }
                        qint64 end = qMin(*off + 1024*1024, total);
                        QString slice = QString("window.__penContent.slice(%1, %2)").arg(*off).arg(end);
                        m_crawlChrome->evaluate(slice, [out, off, fn, end](const QJsonValue &cv) {
                            out->write(cv.toString().toUtf8());
                            *off = end;
                            (*fn)();
                        });
                    };
                    (*fn)();
                });
            });
        }, Qt::QueuedConnection);
        profCapSem->tryAcquire(1, 60000);

        QMetaObject::invokeMethod(this, [this, profileLocal]() {
            log(QString("✅ 프로필 캡쳐: %1").arg(QFileInfo(profileLocal).fileName()), "success");
        }, Qt::QueuedConnection);

        // 5) 각 트윗 순차 캡쳐 → mirrorRoot/tweets/<id>.html
        QString tweetsDir = mirrorRoot + "/tweets";
        QDir().mkpath(tweetsDir);
        QMap<QString, QString> tweetUrlMap;  // tweet URL → 로컬 파일 경로

        int idx = 0;
        for (const auto &v : *tweetUrls) {
            if (m_autoCrawlStop.load()) break;
            QString tweetUrl = v.toString();
            QRegularExpression idRe(R"(/status/(\d+))");
            auto match = idRe.match(tweetUrl);
            QString tweetId = match.hasMatch() ? match.captured(1) : QString::number(idx);
            QString tweetLocal = tweetsDir + "/" + tweetId + ".html";
            tweetUrlMap[tweetUrl] = tweetLocal;
            idx++;

            QMetaObject::invokeMethod(this, [this, idx, tweetCount, tweetUrl]() {
                log(QString("[%1/%2] %3").arg(idx).arg(tweetCount).arg(tweetUrl), "info");
            }, Qt::QueuedConnection);

            // navigate
            auto tnSem = std::make_shared<QSemaphore>(0);
            QMetaObject::invokeMethod(this, [this, tweetUrl, tnSem]() {
                if (!m_crawlChrome) { tnSem->release(); return; }
                m_crawlChrome->navigate(tweetUrl, [tnSem](bool){ tnSem->release(); });
            }, Qt::QueuedConnection);
            tnSem->tryAcquire(1, 30000);
            QThread::sleep(3);

            // 캡쳐
            auto tcSem = std::make_shared<QSemaphore>(0);
            QMetaObject::invokeMethod(this, [this, libCode, sfCall, tweetLocal, tweetUrl, tcSem]() {
                if (!m_crawlChrome) { tcSem->release(); return; }
                m_crawlChrome->evaluate(libCode, [this, sfCall, tweetLocal, tweetUrl, tcSem](const QJsonValue &) {
                    if (!m_crawlChrome) { tcSem->release(); return; }
                    m_crawlChrome->evaluate(sfCall, [this, tweetLocal, tweetUrl, tcSem](const QJsonValue &v) {
                        QJsonObject obj = v.toObject();
                        if (!obj["chunked"].toBool()) { tcSem->release(); return; }
                        qint64 total = obj["size"].toVariant().toLongLong();
                        if (total <= 0) { tcSem->release(); return; }
                        QFile *out = new QFile(tweetLocal);
                        if (!out->open(QIODevice::WriteOnly)) { delete out; tcSem->release(); return; }
                        auto off = std::make_shared<qint64>(0);
                        auto fn = std::make_shared<std::function<void()>>();
                        *fn = [this, out, total, off, fn, tweetLocal, tweetUrl, tcSem]() {
                            if (*off >= total) {
                                out->close(); delete out;
                                m_crawlChrome->evaluate("delete window.__penContent;", [](const QJsonValue&){});
                                // 체르노빌 패턴 — Where From + Finder comment
                                FileHelper::setDownloadMeta(tweetLocal, tweetUrl);
                                FileHelper::setFinderComment(tweetLocal, tweetUrl);
                                // ★ WebDAV 자동 업로드
                                QMetaObject::invokeMethod(this, [this, tweetLocal]() {
                                    enqueueWebDavUpload(tweetLocal);
                                }, Qt::QueuedConnection);
                                tcSem->release();
                                return;
                            }
                            qint64 end = qMin(*off + 1024*1024, total);
                            QString slice = QString("window.__penContent.slice(%1, %2)").arg(*off).arg(end);
                            m_crawlChrome->evaluate(slice, [out, off, fn, end](const QJsonValue &cv) {
                                out->write(cv.toString().toUtf8());
                                *off = end;
                                (*fn)();
                            });
                        };
                        (*fn)();
                    });
                });
            }, Qt::QueuedConnection);
            tcSem->tryAcquire(1, 60000);
        }

        // 6) 프로필 HTML 안 트윗 링크 → 로컬 경로 재작성
        QMetaObject::invokeMethod(this, [this]() {
            log("프로필 페이지 링크 재작성 중...", "info");
        }, Qt::QueuedConnection);

        QFile pf(profileLocal);
        if (pf.exists() && pf.open(QIODevice::ReadOnly)) {
            QString html = QString::fromUtf8(pf.readAll());
            pf.close();
            QString profDir = QFileInfo(profileLocal).absolutePath();
            for (auto it = tweetUrlMap.constBegin(); it != tweetUrlMap.constEnd(); ++it) {
                QString rel = QDir(profDir).relativeFilePath(it.value());
                html.replace("\"" + it.key() + "\"", "\"" + rel + "\"");
                html.replace("'" + it.key() + "'", "'" + rel + "'");
            }
            if (pf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                pf.write(html.toUtf8());
                pf.close();
            }
        }

        // 7) index.html 생성
        QString indexPath = mirrorRoot + "/index.html";
        QFile idxf(indexPath);
        if (idxf.open(QIODevice::WriteOnly)) {
            QString h = "<!DOCTYPE html><html><head><meta charset=utf-8>";
            h += "<title>Twitter 미러 — " + profile.toHtmlEscaped() + "</title>";
            h += "<style>body{font-family:system-ui;padding:20px;background:#0e0e10;color:#e7e7ea}";
            h += "a{color:#7c5cff;text-decoration:none;display:block;padding:6px 0}";
            h += "h1{font-size:18px;border-bottom:1px solid #333;padding-bottom:8px}";
            h += ".count{color:#888;font-size:13px;margin-bottom:16px}</style></head><body>";
            h += "<h1>Twitter 미러 — " + profile.toHtmlEscaped() + "</h1>";
            h += QString("<div class=count>프로필 1 + 트윗 %1개 — %2</div>")
                .arg(tweetUrlMap.size()).arg(QDateTime::currentDateTime().toString());
            QString profRel = QDir(mirrorRoot).relativeFilePath(profileLocal);
            h += QString("<a href=\"%1\"><strong>📋 프로필 페이지</strong></a>").arg(profRel);
            h += "<hr style='margin:12px 0;border-color:#333'>";
            for (auto it = tweetUrlMap.constBegin(); it != tweetUrlMap.constEnd(); ++it) {
                QString rel = QDir(mirrorRoot).relativeFilePath(it.value());
                h += QString("<a href=\"%1\">%2</a>").arg(rel, it.key().toHtmlEscaped());
            }
            h += "</body></html>";
            idxf.write(h.toUtf8());
            idxf.close();
            // ★ WebDAV 자동 업로드 (index 도)
            QMetaObject::invokeMethod(this, [this, indexPath]() {
                enqueueWebDavUpload(indexPath);
            }, Qt::QueuedConnection);
        }

        QMetaObject::invokeMethod(this, [this, indexPath, tweetCount]() {
            log(QString("━━ Twitter 미러 완료 — 트윗 %1개").arg(tweetCount), "success");
            log(QString("  index: %1").arg(indexPath), "info");
            log("  프로필 페이지에서 트윗 클릭 → 로컬 트윗 페이지 정상 작동", "info");
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}
