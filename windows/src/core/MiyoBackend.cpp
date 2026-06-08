#include "MiyoBackend.h"
#include "MainWindow.h"
#include "Config.h"
#include "utils/WebDavUploader.h"
#include "platforms/TwitterCollector.h"
#include "platforms/BlueskyCollector.h"
#include "platforms/SiteCrawler.h"
#include "platforms/RealChromeCrawler.h"
#include "utils/HttpClient.h"
#include "utils/ExcelWriter.h"
#include "xlsxdocument.h"
#include "utils/DiskJsonBuffer.h"
#include "utils/FileHelper.h"
#include "core/Common.h"
using FileHelper::sanitizeFilename;

#include <QApplication>
#include <QClipboard>
#include <QFileDialog>
#include <QJsonDocument>
#include <QInputDialog>
#include <QProcess>
#include <QThread>
#include <QDir>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QImage>
#include <QStorageInfo>
#include <QWebEngineView>
#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineCookieStore>
#include <QNetworkCookie>
#include <QSemaphore>
#include <QSet>
#include <QScopeGuard>
#include <QDebug>
#include <QFile>
#include <QFileInfo>
#include <QUrl>
#include <QCoreApplication>
#ifndef Q_OS_WIN
#include <signal.h>
#endif
#include <QRegularExpression>

// ★ 수집 동시성 가드 — RAII. 시작 시 semaphore 대기, 종료 시 자동 release.
namespace {
struct CollectionGuard {
    QSemaphore *sem;
    MiyoBackend *backend;
    QString platform;
    CollectionGuard(QSemaphore *s, MiyoBackend *b, const QString &p) : sem(s), backend(b), platform(p) {
        if (!sem->tryAcquire(1, 50)) {
            if (backend) backend->log(QString("⏳ 다른 [%1] 수집 진행 중 — 큐에서 대기...").arg(platform), "info", platform);
            sem->acquire(1);
            if (backend) backend->log(QString("▶ [%1] 수집 시작 (큐 해제)").arg(platform), "info", platform);
        }
    }
    ~CollectionGuard() { sem->release(1); }
};
}

// ★ Platform 별 capacity — 기능 특성에 맞춤
QSemaphore* MiyoBackend::platformSem(const QString &platform)
{
    QMutexLocker lock(&m_platformSemsMutex);
    if (m_platformSems.contains(platform)) return m_platformSems[platform];
    int cap = 2;  // default
    // platform 별 capacity (동시 작업 한도)
    if (platform == "youtube") cap = 1;        // yt-dlp/ffmpeg 무거움, NAS write 부담
    else if (platform == "instagram") cap = 1; // login pause / rate limit
    else if (platform == "pixiv") cap = 2;     // API + 이미지
    else if (platform == "fanbox") cap = 2;    // 멤버십 컨텐츠
    else if (platform == "twitter") cap = 2;   // API rate limit
    else if (platform == "bluesky") cap = 2;   // API rate limit
    else if (platform == "tumblr") cap = 3;
    else if (platform == "spinspin") cap = 3;
    else if (platform == "asked") cap = 3;
    else if (platform == "discord") cap = 3;
    else if (platform == "crawl") cap = 3;
    else if (platform == "naikakukai") cap = 2;
    else if (platform == "trad") cap = 1;      // 압축 + ZIP 무거움
    QSemaphore *sem = new QSemaphore(cap);
    m_platformSems[platform] = sem;
    return sem;
}
#include <QTimer>
#include <QDateTime>
#include <QCryptographicHash>
#include <QSysInfo>
#ifdef Q_OS_WIN
#include <windows.h>
#include <psapi.h>
#else
#include <sys/resource.h>
#endif

// Forward declaration — 정의는 upgradePython() 앞에
static QStringList diagnosePythonEnv(const QString &python);

MiyoBackend::MiyoBackend(MainWindow *window, QObject *parent)
    : QObject(parent)
    , m_window(window)
    , m_config(new Config(this))
    , m_http(new HttpClient(this))
    , m_currentPlatform("twitter")
    , m_webdav(new WebDavUploader(this))
{
    // WebDAV uploader 로그 라우팅
    connect(m_webdav, &WebDavUploader::logMessage, this, [this](const QString &msg, const QString &type) {
        log(msg, type, "settings");
    });
    // ★ 앱 시작 시 이전 세션의 좀비 capture Chrome 청소
    //   매치 패턴 3종 — 일반 Chrome 영향 없음:
    //     1) chrome_capture_profile (capture 전용 폴더)
    //     2) Chrome for Testing (앱 내부 번들 Chromium)
    //     3) chrome_crashpad_handler (Chromium crashpad)
#ifdef Q_OS_MACOS
    QProcess::execute("/usr/bin/pkill", {"-f", "chrome_capture_profile"});
    QProcess::execute("/usr/bin/pkill", {"-9", "-f", "Chrome for Testing"});
    QProcess::execute("/usr/bin/pkill", {"-9", "-f", "chrome_crashpad_handler"});
    // ★ 앱 시작 시 이전 세션의 좀비 tail script process 청소 (Terminal window 중복 방지)
    //   사용자가 본 "터미널 2개 떠요" 의 원인 — 옛 tail.command process 가 안 죽고 남아서.
    QProcess::execute("/usr/bin/pkill", {"-f", "miyo_.*_tail.command"});
    QProcess::execute("/usr/bin/pkill", {"-f", "miyo_backup_tail.command"});
#elif defined(Q_OS_WIN)
    QProcess::execute("taskkill", {"/F", "/IM", "chrome.exe", "/FI",
                                    "COMMANDLINE eq *chrome_capture_profile*"});
    QProcess::execute("taskkill", {"/F", "/IM", "Chrome for Testing.exe"});
    QProcess::execute("taskkill", {"/F", "/IM", "chrome_crashpad_handler.exe"});
    // 옛 tail script 청소
    QProcess::execute("taskkill", {"/F", "/IM", "cmd.exe", "/FI",
                                    "COMMANDLINE eq *miyo_*_tail.bat*"});
#endif

    // ★ Chrome capture profile cache 자동 정리 (로그인 cookie는 보존)
    //   사용자가 자주 쓰면 profile 누적되어 메모리 폭주 → OOM kill.
    //   2 위치 모두 정리: AppData (cookie/storage) + Caches (cache 전용 — chromium 자체 만듦)
    //   Cookies/Local Storage 는 보존 (로그인 유지).
    {
        QStringList scanRoots;
        scanRoots << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        scanRoots << QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QStringList cacheSubdirs = {
            "Default/Cache", "Default/Code Cache", "Default/GPUCache",
            "Default/Service Worker", "Default/Worker Cache",
            "GrShaderCache", "ShaderCache", "GraphiteDawnCache"
        };
        qint64 freedBytes = 0;
        for (const QString &root : scanRoots) {
            QDir profilesDir(root);
            QStringList profiles = profilesDir.entryList(
                QStringList() << "chrome_capture_profile*", QDir::Dirs | QDir::NoDotAndDotDot);
            for (const QString &profile : profiles) {
                // Caches 위치는 통째로 안전 (cache만이라 cookie 없음)
                if (root == QStandardPaths::writableLocation(QStandardPaths::CacheLocation)) {
                    QDir d(root + "/" + profile);
                    if (d.exists()) {
                        QDirIterator it(d.path(), QDir::Files, QDirIterator::Subdirectories);
                        while (it.hasNext()) { freedBytes += QFileInfo(it.next()).size(); }
                        d.removeRecursively();
                    }
                    continue;
                }
                // AppData 위치는 cookie 보존 위해 cache subdirs만 정리
                for (const QString &sub : cacheSubdirs) {
                    QDir d(root + "/" + profile + "/" + sub);
                    if (d.exists()) {
                        QDirIterator it(d.path(), QDir::Files, QDirIterator::Subdirectories);
                        while (it.hasNext()) { freedBytes += QFileInfo(it.next()).size(); }
                        d.removeRecursively();
                    }
                }
            }
        }
        if (freedBytes > 0) {
            log(QString("Chrome capture cache 정리: %1MB 회수 (로그인은 보존)")
                .arg(freedBytes / 1024 / 1024), "info", "settings");
        }
    }

    // ★ 옛 번들 이름 (チェルノブイリ) 폴더 정리 — Chernobyl로 rename 후 남은 잔여 데이터
    //   사용자 데이터 (config) 는 이미 새 위치로 마이그레이션 완료. 옛 dir 통째 제거.
    {
        QString appDataParent = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        // AppData = ~/Library/Application Support/Miyo/Chernobyl (현재)
        //   → 부모 ~/Library/Application Support/Miyo 안에 옛 チェルノブイリ dir
        QDir parentDir(QFileInfo(appDataParent).absolutePath());
        QStringList legacy = {"チェルノブイリ", "ABIWA"};
        qint64 freedLegacy = 0;
        for (const QString &name : legacy) {
            QString p = parentDir.absoluteFilePath(name);
            if (QDir(p).exists()) {
                QDirIterator it(p, QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext()) freedLegacy += QFileInfo(it.next()).size();
                QDir(p).removeRecursively();
            }
        }
        if (freedLegacy > 0) {
            log(QString("옛 번들 이름 데이터 정리: %1MB 회수").arg(freedLegacy / 1024 / 1024),
                "info", "settings");
        }
    }

    // ★ QtWebEngine cache 주기 청소 — Caches/Miyo 가 100MB+ 까지 자동 증식.
    //   매 앱 시작 시 30일 초과 cache 파일만 제거 (최근 데이터 보존 — 속도 영향 최소화).
    {
        QString cacheRoot = QStandardPaths::writableLocation(QStandardPaths::CacheLocation);
        QDateTime cutoff = QDateTime::currentDateTime().addDays(-30);
        qint64 freedCache = 0;
        QDirIterator it(cacheRoot, QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            QFileInfo fi(it.next());
            if (fi.lastModified() < cutoff) {
                qint64 sz = fi.size();
                if (QFile::remove(fi.absoluteFilePath())) freedCache += sz;
            }
        }
        if (freedCache > 0) {
            log(QString("QtWebEngine cache 정리 (30일 초과): %1MB 회수")
                .arg(freedCache / 1024 / 1024), "info", "settings");
        }
    }

    connect(this, &MiyoBackend::jsSignal, this, &MiyoBackend::executeJsMainThread);
    connect(this, &MiyoBackend::logSignal, this, &MiyoBackend::appendLogMainThread);

    // Log batch flush timer — 300ms 간격으로 모아서 한번에 전송 (UI 부하 감소)
    m_logFlushTimer = new QTimer(this);
    m_logFlushTimer->setInterval(300);
    connect(m_logFlushTimer, &QTimer::timeout, this, &MiyoBackend::flushLogs);
    m_logFlushTimer->start();

    // Browser view signals → JS UI updates
    auto *bv = m_window->browserView();
    if (bv) {
        connect(bv, &QWebEngineView::urlChanged, this, [this](const QUrl &url) {
            QString escaped = url.toString();
            escaped.replace("'", "\\'");
            runJs(QString("updateBrowserUrl('%1')").arg(escaped));
        });
        connect(bv, &QWebEngineView::titleChanged, this, [this](const QString &title) {
            QString escaped = title;
            escaped.replace("'", "\\'");
            runJs(QString("updateBrowserTitle('%1')").arg(escaped));
        });
        // loadProgress 디바운싱 — 매 5% 또는 완료(100%) 때만 업데이트
        connect(bv, &QWebEngineView::loadProgress, this, [this](int pct) {
            static int lastPct = -1;
            if (pct == 100 || pct == 0 || (pct - lastPct) >= 5) {
                lastPct = pct;
                runJs(QString("updateBrowserLoading(%1)").arg(pct));
            }
        });
    }

    // [자동 유지보수] 앱 시작 시 오래된 임시 파일 / 스크립트 정리
    performStartupCleanup();

    // [자동 유지보수] 주기적 메모리 모니터 — 5분마다 RSS 체크, 임계치 초과 시 경고 + 캐시 정리
    m_memoryMonitorTimer = new QTimer(this);
    m_memoryMonitorTimer->setInterval(5 * 60 * 1000);  // 5분
    connect(m_memoryMonitorTimer, &QTimer::timeout, this, &MiyoBackend::memoryMonitorTick);
    m_memoryMonitorTimer->start();

    // 앱 종료 직전 hook — 자식 프로세스 정리
    connect(qApp, &QCoreApplication::aboutToQuit, this, [this]() {
        killChildProcesses();
    });
}

MiyoBackend::~MiyoBackend()
{
    // 内閣会 타이머 정리
    m_naikakukaiRunning = false;
    if (m_naikakukaiTimer) { m_naikakukaiTimer->stop(); }

    // 명시 정리 — 각 collector 소멸자가 자기 QProcess 데몬을 kill
    delete m_twitterCollector;  m_twitterCollector = nullptr;
    delete m_blueskyCollector;  m_blueskyCollector = nullptr;
    delete m_crawler;           m_crawler = nullptr;

    // ★ 캡쳐용 Chrome 인스턴스들 정리 (Chrome process 누적 leak 방지 — crash 원인)
    if (m_captureChrome) {
        m_captureChrome->stop();
        m_captureChrome->deleteLater();
        m_captureChrome = nullptr;
    }
    {
        QMutexLocker mapLock(&m_capChromeMapMutex);
        for (auto it = m_captureChromesPerThread.begin(); it != m_captureChromesPerThread.end(); ++it) {
            if (it.value()) {
                it.value()->stop();
                it.value()->deleteLater();
            }
        }
        m_captureChromesPerThread.clear();
    }

    // ★ 로그인 대기 sem map 정리 (per-platform QSemaphore*)
    {
        QMutexLocker lock(&m_loginPauseMutex);
        qDeleteAll(m_loginPauseSems);
        m_loginPauseSems.clear();
    }
}

// ─────────────────────────────────────────────
// 자동 유지보수
// ─────────────────────────────────────────────

void MiyoBackend::performStartupCleanup()
{
    if (!m_config) return;
    QString td = m_config->tempDir();
    if (td.isEmpty()) return;
    QDir dir(td);
    if (!dir.exists()) return;

    int removed = 0;
    QDateTime cutoff = QDateTime::currentDateTime().addDays(-7);

    // 1) 오래된 임시 파일 (7일 초과) 삭제
    for (const QFileInfo &fi : dir.entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) {
        if (fi.lastModified() < cutoff) {
            if (QFile::remove(fi.absoluteFilePath())) removed++;
        }
    }

    // 2) 이전 세션 잔여 스크립트 / status 파일 즉시 삭제
    QStringList stalePatterns = {"miyo_*.command", "miyo_*.bat",
                                 "miyo_*status.txt", "miyo_*status.txt.stop",
                                 "*.daemon.log"};
    for (const QFileInfo &fi : dir.entryInfoList(stalePatterns, QDir::Files)) {
        if (QFile::remove(fi.absoluteFilePath())) removed++;
    }

    // 3) 빈 서브 디렉토리 정리
    for (const QFileInfo &fi : dir.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        QDir sub(fi.absoluteFilePath());
        if (sub.isEmpty()) sub.removeRecursively();
    }

    if (removed > 0) {
        log(QString("[자동정리] 오래된 임시 파일 %1개 삭제").arg(removed), "info");
    }
}

void MiyoBackend::killChildProcesses()
{
    // TwitterCollector / BlueskyCollector / SiteCrawler 내부 QProcess 는
    // 각 collector 소멸자가 정리. 여기선 고아 상태(크래시 잔여)만 처리.
    // 현재 실행 중인 수집이 있으면 먼저 중지 플래그 세움.
    {
        QMutexLocker lock(&m_runningMutex);
        for (auto it = m_isRunning.begin(); it != m_isRunning.end(); ++it) {
            it.value() = false;
        }
    }

    // 살아있는 collector 인스턴스 있으면 자기 데몬 kill
    if (m_blueskyCollector) {
        qint64 pid = m_blueskyCollector->daemonPid();
        if (pid > 0) {
#ifdef Q_OS_WIN
            QProcess::execute("taskkill", {"/PID", QString::number(pid), "/F"});
#else
            ::kill(static_cast<pid_t>(pid), SIGTERM);
#endif
        }
    }

    // 모든 터미널 로그 창 닫기
    closeAllTerminalLogs();
}

void MiyoBackend::memoryMonitorTick()
{
    qint64 rssMB = -1;
#ifdef Q_OS_WIN
    PROCESS_MEMORY_COUNTERS pmc;
    if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc))) {
        rssMB = static_cast<qint64>(pmc.WorkingSetSize / (1024 * 1024));
    }
#else
    struct rusage r;
    if (getrusage(RUSAGE_SELF, &r) == 0) {
    #ifdef Q_OS_MACOS
        rssMB = static_cast<qint64>(r.ru_maxrss) / (1024 * 1024);  // macOS: bytes
    #else
        rssMB = static_cast<qint64>(r.ru_maxrss) / 1024;  // Linux: KB
    #endif
    }
#endif
    if (rssMB < 0) return;

    if (rssMB > m_peakRssMB) m_peakRssMB = rssMB;

    // 임계치 1: 1.5 GB — 경고만
    if (rssMB > 1500) {
        log(QString("[메모리] 사용량 %1 MB (피크 %2 MB) — 높음").arg(rssMB).arg(m_peakRssMB),
            "warning");
    }

    // 임계치 2: 2.5 GB — 자동 복구 시도 (HTTP 캐시 + 방문 링크 정리)
    if (rssMB > 2500) {
        log(QString("[메모리] %1 MB 초과 — 자동 캐시 정리 실행").arg(rssMB), "warning");
        if (m_window && m_window->webView() && m_window->webView()->page()) {
            auto *profile = m_window->webView()->page()->profile();
            if (profile) {
                profile->clearHttpCache();
                profile->clearAllVisitedLinks();
            }
        }
        // 로그 배치 버퍼도 비우기
        m_pendingLogs.clear();
    }
}

// ─────────────────────────────────────────────
// 内閣会 — 신글 자동 감지/다운로드 백그라운드 서비스
//   지원 플랫폼: Twitter(공식 API) / Bluesky(AT Proto) / Tumblr(OAuth v2)
//   전략: 주기적으로 watchlist 순회 → 각 대상 최근 N개 수집(기존 collector 재사용)
//         수집기는 이미 파일 존재 여부로 중복 스킵 → 신글만 자동 추가됨
// ─────────────────────────────────────────────

void MiyoBackend::startNaikakukai(const QString &configJson)
{
    QJsonDocument doc = QJsonDocument::fromJson(configJson.toUtf8());
    if (doc.isNull() || !doc.isObject()) {
        log("内閣会: 설정 JSON 파싱 실패", "error", "naikakukai");
        return;
    }
    QJsonObject cfg = doc.object();
    // ★ 터미널 등록 — 다른 platform 의 로그가 섞이는 거 방지
    openTerminalLog("naikakukai", QString());
    m_naikakukaiWatches = cfg["watches"].toArray();
    m_naikakukaiIntervalMin = qMax(5, cfg["intervalMin"].toInt(30));

    if (m_naikakukaiWatches.isEmpty()) {
        log("内閣会: 감시 대상이 없습니다", "warning", "naikakukai");
        return;
    }

    // 지원하지 않는 플랫폼 필터링
    QJsonArray filtered;
    for (const auto &v : m_naikakukaiWatches) {
        QJsonObject w = v.toObject();
        QString p = w["platform"].toString();
        if (p == "twitter" || p == "bluesky" || p == "tumblr") {
            filtered.append(w);
        } else {
            log(QString("内閣会: %1 은(는) 지원 안 함 — 건너뜀").arg(p), "warning", "naikakukai");
        }
    }
    m_naikakukaiWatches = filtered;
    if (m_naikakukaiWatches.isEmpty()) {
        log("内閣会: 지원 플랫폼(Twitter/Bluesky/Tumblr) 대상이 없습니다", "error", "naikakukai");
        return;
    }

    m_naikakukaiCursor = 0;
    m_naikakukaiRunning = true;

    if (!m_naikakukaiTimer) {
        m_naikakukaiTimer = new QTimer(this);
        connect(m_naikakukaiTimer, &QTimer::timeout, this, &MiyoBackend::naikakukaiTick);
    }
    m_naikakukaiTimer->setInterval(m_naikakukaiIntervalMin * 60 * 1000);
    m_naikakukaiTimer->start();

    log(QString("内閣会 시작 — 대상 %1개 | 주기 %2분")
        .arg(m_naikakukaiWatches.size()).arg(m_naikakukaiIntervalMin), "success", "naikakukai");
    runJs("setNaikakukaiRunning(true)");

    // 즉시 1회 실행 (30분 기다리지 말고)
    QTimer::singleShot(1000, this, &MiyoBackend::naikakukaiTick);
}

void MiyoBackend::stopNaikakukai()
{
    m_naikakukaiRunning = false;
    if (m_naikakukaiTimer) m_naikakukaiTimer->stop();
    log("内閣会 중지됨", "warning", "naikakukai");
    runJs("setNaikakukaiRunning(false)");
    // 터미널 종료
    if (m_terminalLogPaths.contains("naikakukai")) {
        QFile lf(m_terminalLogPaths["naikakukai"]);
        if (lf.open(QIODevice::Append | QIODevice::Text)) {
            lf.write("\n\033[1;31m⏹ 사용자 중지\033[0m\n[DONE]\n"); lf.close();
        }
    }
    QTimer::singleShot(1500, this, []() {
#ifdef Q_OS_MACOS
        QProcess::execute("/usr/bin/pkill", {"-f", "miyo_naikakukai_tail.command"});
#endif
    });
}

// ═════════════════════════════════════════════════════════════════════════
// NAS 자동 백업 — 로컬 다운로드 완료 후 NAS 마운트 폴더로 cp
//   동기적 PUT (WebDAV) 와 다름: 그냥 파일 시스템 cp. 빠르고 안정.
// ═════════════════════════════════════════════════════════════════════════
void MiyoBackend::setBackupConfig(bool enabled, const QString &path)
{
    m_config->setBackupEnabled(enabled);
    if (!path.isEmpty()) m_config->setBackupPath(path);
    m_config->save();
    log(QString("📦 NAS 백업 %1 — 경로: %2")
        .arg(enabled ? "✅ 활성" : "❌ 비활성", m_config->backupPath().isEmpty() ? "(미설정)" : m_config->backupPath()),
        enabled ? "success" : "info", "settings");

    // 워커 thread 가 죽었으면 다시 시작
    if (enabled && !m_backupRunning.load()) {
        m_backupRunning = true;
        { QThread *_bt = QThread::create([this]() { backupWorker(); }); m_backupThreads.append(_bt);
        connect(_bt, &QThread::finished, _bt, &QThread::deleteLater);
        _bt->start(); }
    } else if (!enabled) {
        // ★ OFF 누르면 즉시 모든 백업 활동 중지 — "껐는데도 계속 올라간다" 버그 방지
        m_backupRunning = false;
        // 큐 비우기 (자동 enqueue 된 잔여 항목 폐기)
        {
            QMutexLocker lock(&m_backupQueueMutex);
            initBackupQueuePaths();
            QFile::remove(m_backupQueuePath);
            QFile::remove(m_backupOffsetPath);
            m_backupQueueOffset = 0;
            m_backupTotalBytes = 0;
            m_backupDoneBytes = 0;
            m_backupTotalCount = 0;
            m_backupDoneCount = 0;
        }
        // 진행 중인 자식 cp / ditto / rsync 강제 종료
#ifdef Q_OS_MACOS
        QString pidStr = QString::number(QCoreApplication::applicationPid());
        QProcess::execute("/usr/bin/pkill", {"-TERM", "-P", pidStr, "ditto"});
        QProcess::execute("/usr/bin/pkill", {"-TERM", "-P", pidStr, "cp"});
        QProcess::execute("/usr/bin/pkill", {"-TERM", "-P", pidStr, "rsync"});
#endif
        log("🧹 자동 백업 큐 비움 + 활성 cp 종료", "info", "settings");
    }
}

void MiyoBackend::pickBackupPath()
{
    QStringList paths, items;
#ifdef Q_OS_MACOS
    // 1) /Volumes 의 마운트된 NAS / 외장
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
    // 2) Mountain Duck — 여러 버전 / 마운트 방식 모두 지원
    //   - 옛 (FUSE):     ~/Mountain Duck/<name>/
    //   - 옛 (Cyberduck): ~/Library/Group Containers/G69SCX94XU.duck/.../Drive/
    //   - 신 (5.x NFS):   ~/Library/Containers/io.mountainduck/Data/Library/Application Support/Mountain Duck/Volumes.noindex/<name>.localized/
    //   - macOS 12+ CloudStorage: ~/Library/CloudStorage/<name>/
    QStringList duckRoots = {
        QDir::homePath() + "/Mountain Duck",
        QDir::homePath() + "/Library/Group Containers/G69SCX94XU.duck/Library/Application Support/Cyberduck/Drive",
        QDir::homePath() + "/Library/Containers/io.mountainduck/Data/Library/Application Support/Mountain Duck/Volumes.noindex",
        QDir::homePath() + "/Library/CloudStorage"
    };
    for (const QString &duckRoot : duckRoots) {
        if (!QDir(duckRoot).exists()) continue;
        QDir dr(duckRoot);
        const QStringList connections = dr.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const QString &conn : connections) {
            QString path = duckRoot + "/" + conn;
            QFileInfo fi(path);
            if (!fi.isDir()) continue;
            // .localized suffix 제거 (display 만)
            QString displayName = conn;
            if (displayName.endsWith(".localized")) displayName.chop(QString(".localized").size());
            paths << path;
            items << QString("🦆 %1  (Mountain Duck — %2)").arg(displayName, path);
        }
    }
#endif
    // ★ "직접 선택" 옵션을 맨 위 (default) 으로 — 사용자가 안 누르고 OK 해도 안전하게 동작
    paths.prepend("__MANUAL__");
    items.prepend("📂 직접 폴더 선택 (Finder dialog — 어디든 navigate, 가장 안전)");

    QMetaObject::invokeMethod(this, [this, items, paths]() {
        // 1단계: 마운트된 볼륨 선택 (또는 default "직접 선택")
        bool ok = false;
        QString chosen = QInputDialog::getItem(m_window, "📦 백업 위치 — 1단계: 볼륨 선택",
            "백업할 NAS/외장/Mountain Duck 볼륨 (또는 직접 선택):", items, 0, false, &ok);
        if (!ok) {
            log("백업 경로 선택 취소됨", "info", "settings");
            return;
        }
        if (chosen.isEmpty()) {
            log("⚠ 빈 선택 — default 옵션 사용 ('📂 직접 폴더 선택')", "warning", "settings");
            chosen = items.first();
        }
        int idx = items.indexOf(chosen);
        if (idx < 0 || idx >= paths.size()) {
            // 일치 안 함 — fallback 으로 manual 모드
            log("⚠ 선택 일치 안 함 — Finder dialog 로 직접 선택", "warning", "settings");
            idx = 0;  // __MANUAL__
        }
        QString volumeRoot = paths[idx];

        // ★ "직접 선택" 모드 — 시작 위치 = home (사용자가 어디든 navigate 가능)
        bool manualMode = (volumeRoot == "__MANUAL__");
        QString startDir = manualMode ? QDir::homePath() : volumeRoot;

        // 2단계: Finder dialog 로 폴더 navigate
        QString backupPath = QFileDialog::getExistingDirectory(
            m_window,
            manualMode
                ? "📦 백업 위치 — 직접 선택 (Mountain Duck / NAS / 외장 어디든)"
                : "📦 백업 위치 — 2단계: 폴더 선택 (없으면 새로 만드세요)",
            startDir,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
        );
        if (backupPath.isEmpty()) return;
        // 수동 모드 가 아니면 — 선택한 폴더가 마운트 볼륨 안인지 검증 (실수 방지)
        if (!manualMode && !backupPath.startsWith(volumeRoot)) {
            log(QString("⚠ 선택한 폴더가 %1 안에 없음 — 무시").arg(volumeRoot), "warning", "settings");
            return;
        }
        QDir().mkpath(backupPath);
        m_config->setBackupPath(backupPath);
        // ★ 경로만 저장 — 활성화는 사용자가 명시적으로 체크박스 ON 했을 때만.
        //   (예전에 자동 setBackupEnabled(true) 해서 "껐는데도 백업 됨" 버그 원인)
        m_config->save();
        log(QString("📦 백업 경로 저장: %1 (활성화는 체크박스 ON 해야 작동)").arg(backupPath), "info", "settings");

        // JS UI 갱신 — 체크박스는 현재 상태 유지 (true 강제 X)
        QString safe = Common::jsStringLiteral(backupPath);
        runJs(QString("if(window.onBackupConfigChanged) onBackupConfigChanged(%1, %2);")
            .arg(m_config->backupEnabled() ? "true" : "false").arg(safe));
    }, Qt::QueuedConnection);
}

void MiyoBackend::testBackup()
{
    QString path = m_config->backupPath();
    if (!m_config->backupEnabled() || path.isEmpty()) {
        log("백업 미설정", "warning", "settings");
        return;
    }
    // 테스트 파일 작성
    QString testFile = Common::resolveTempBase(m_config ? m_config->tempDir() : QString()) + "/chernobyl_backup_test.txt";
    QFile f(testFile);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QString("백업 테스트 — %1\n").arg(QDateTime::currentDateTime().toString()).toUtf8());
        f.close();
        enqueueBackup(testFile);
        log("📦 테스트 파일 큐 추가 — 잠시 후 백업 로그 확인", "info", "settings");
    }
}

// ═════════════════════════════════════════════════════════════════════════
// NAS watchdog — 30초마다 backupPath 마운트 체크, 끊기면 자동 재마운트
// ═════════════════════════════════════════════════════════════════════════
void MiyoBackend::nasWatchdogTick()
{
    if (!m_config) return;
    if (!m_config->nasAutoReconnect()) return;
    // 체크 대상 — backupPath 가 NAS 경로이면 그 부모 마운트 확인
    QString path = m_config->backupPath();
    if (path.isEmpty() || !path.startsWith("/Volumes/")) return;
    // /Volumes/X 의 X 마운트 디렉토리 존재 + 읽기 가능 여부
    int slash = path.indexOf('/', QString("/Volumes/").length());
    QString mountPoint = slash > 0 ? path.left(slash) : path;
    QDir d(mountPoint);
    if (d.exists() && QFileInfo(mountPoint).isReadable()) return;  // 마운트 OK
    // 끊김 — 자동 재마운트 시도
    if (m_nasReconnectInProgress) return;
    log(QString("⚠ NAS 마운트 끊김 감지: %1 — 자동 재연결 시도").arg(mountPoint), "warning", "settings");
    silentRemountWebDav();
}

void MiyoBackend::silentRemountWebDav()
{
    if (!m_config) return;
    QString url  = m_config->webdavUrl();
    QString user = m_config->webdavUser();
    QString pass = m_config->webdavPass();
    if (url.isEmpty() || user.isEmpty() || pass.isEmpty()) {
        log("⚠ NAS 자동 재연결 — WebDAV 자격증명 미설정", "warning", "settings");
        return;
    }
    m_nasReconnectInProgress = true;

    auto esc = [](const QString &s) {
        QString r = s; r.replace("\\", "\\\\"); r.replace("\"", "\\\""); return r;
    };
    QString script = QString("mount volume \"%1\" as user name \"%2\" with password \"%3\"")
        .arg(esc(url), esc(user), esc(pass));

    QThread *t = QThread::create([this, script]() {
        QProcess osa;
        osa.start("osascript", {"-e", script});
        bool fin = osa.waitForFinished(45000);
        QString err = QString::fromUtf8(osa.readAllStandardError()).trimmed();
        int code = osa.exitCode();
        QMetaObject::invokeMethod(this, [this, fin, code, err]() {
            m_nasReconnectInProgress = false;
            if (fin && code == 0) {
                log("✅ NAS 자동 재연결 성공", "success", "settings");
                runJs("if(window.onNasReconnected) onNasReconnected();");
            } else {
                log(QString("❌ NAS 자동 재연결 실패: %1").arg(err.left(120)), "warning", "settings");
            }
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

void MiyoBackend::resyncAllFoldersToBackup()
{
    // 전체 재sync 는 이제 backupNow 와 같은 rsync 폴더 통째 모드 호출 (사용자 의도 같음 — idempotent 전체 백업)
    backupNow();
}

// ═════════════════════════════════════════════════════════════════════════
// runRcloneBackup — rclone copy 로 WebDAV/SFTP 빠른 전송 (Mountain Duck 와 동일 효과).
//   bundle 된 rclone binary 사용. 사용자의 WebDAV creds (m_config) 로 임시 config 생성.
//   --transfers=8 --multi-thread-streams=4 --progress --stats=2s.
// ═════════════════════════════════════════════════════════════════════════
void MiyoBackend::runRcloneBackup(const QStringList &srcDirs, const QString &destSubPath)
{
    QString rclonePath = QCoreApplication::applicationDirPath() + "/../Resources/tools/rclone";
#ifdef Q_OS_WIN
    rclonePath = QCoreApplication::applicationDirPath() + "/resources/tools/rclone.exe";
#endif
    if (!QFile::exists(rclonePath)) {
        // dev mode fallback
        rclonePath = QCoreApplication::applicationDirPath() + "/../../../resources/tools/rclone";
#ifdef Q_OS_WIN
        if (!QFile::exists(rclonePath)) rclonePath = QCoreApplication::applicationDirPath() + "/../../../resources/tools/rclone.exe";
#endif
    }
    if (!QFile::exists(rclonePath)) {
        log("❌ rclone binary 없음 — backup 일반 모드로 fallback", "error", "settings");
        return;
    }

    QString url = m_config->webdavUrl();
    QString user = m_config->webdavUser();
    QString pass = m_config->webdavPass();
    if (url.isEmpty() || user.isEmpty() || pass.isEmpty()) {
        log("❌ WebDAV 자격증명 없음 — 설정 → WebDAV 자동 업로드 카드에 URL/사용자명/비번 입력 후 [저장] 누르세요", "error", "settings");
        runJs("alert('❌ rclone 백업 위해 WebDAV 자격증명 필요\\n\\n설정 → WebDAV 자동 업로드 카드\\n  • URL: https://your-nas.com/path\\n  • 사용자명\\n  • 비밀번호\\n\\n입력 후 [저장] 누르고 다시 시도');");
        return;
    }

    QString tempBase = Common::resolveTempBase(m_config->tempDir());
    QString confPath = tempBase + "/abiwa_rclone.conf";

    // 1) rclone obscure 로 비번 obfuscate
    QString obscuredPass;
    {
        QProcess obs;
        obs.start(rclonePath, {"obscure", pass});
        obs.waitForFinished(5000);
        obscuredPass = QString::fromUtf8(obs.readAllStandardOutput()).trimmed();
    }
    if (obscuredPass.isEmpty()) {
        log("❌ rclone obscure 실패", "error", "settings");
        return;
    }

    // 2) 임시 rclone.conf 생성
    {
        QFile cf(confPath);
        if (!cf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            log(QString("❌ rclone conf 생성 실패: %1").arg(confPath), "error", "settings");
            return;
        }
        QString conf = QString(
            "[abiwa_nas]\n"
            "type = webdav\n"
            "url = %1\n"
            "vendor = other\n"
            "user = %2\n"
            "pass = %3\n"
        ).arg(url, user, obscuredPass);
        cf.write(conf.toUtf8());
        cf.close();
        QFile::setPermissions(confPath, QFile::ReadOwner | QFile::WriteOwner);  // 600
    }

    log(QString("📦 rclone 백업 시작 — %1 폴더 → %2:%3").arg(srcDirs.size()).arg(url, destSubPath), "info", "settings");

    // 3) 각 src 폴더별 rclone copy 호출 (각각 자기 sub-folder 로)
    QThread *t = QThread::create([this, srcDirs, destSubPath, rclonePath, confPath]() {
        m_backupTerminalActive = true;
        m_backupStartMs = QDateTime::currentMSecsSinceEpoch();
        for (const QString &src : srcDirs) {
            if (!m_backupTerminalActive.load()) break;
            QString platName = QFileInfo(src).fileName();
            QString remote = QString("abiwa_nas:%1/%2").arg(destSubPath, platName);
            writeTerminalLog(QString("\033[1;34m[rclone] %1 → %2\033[0m").arg(src, remote), "backup");

            QProcess rc;
            rc.setProcessChannelMode(QProcess::MergedChannels);
            QStringList args;
            args << "copy" << src << remote
                 << "--config" << confPath
                 << "--transfers" << "8"
                 << "--multi-thread-streams" << "4"
                 << "--progress" << "--stats" << "2s" << "--stats-one-line"
                 << "--retries" << "5"
                 << "--low-level-retries" << "10"
                 << "--exclude" << ".abiwa_**"
                 << "--exclude" << ".DS_Store"
                 << "--exclude" << ".git/**"
                 << "--exclude" << "__CHERNOBYL_MANIFEST__*";
            rc.start(rclonePath, args);
            if (!rc.waitForStarted(5000)) {
                writeTerminalLog("\033[31m[rclone] 시작 실패\033[0m", "backup");
                continue;
            }
            // stdout stream → terminal
            QByteArray leftover;
            while (rc.state() != QProcess::NotRunning) {
                if (!m_backupTerminalActive.load()) {
                    rc.terminate();
                    if (!rc.waitForFinished(3000)) rc.kill();
                    break;
                }
                if (!rc.waitForReadyRead(500)) continue;
                QByteArray chunk = rc.readAll();
                chunk.replace('\r', '\n');
                leftover += chunk;
                int nl;
                while ((nl = leftover.indexOf('\n')) >= 0) {
                    QByteArray line = leftover.left(nl);
                    leftover.remove(0, nl + 1);
                    QString s = QString::fromUtf8(line).trimmed();
                    if (s.isEmpty()) continue;
                    writeTerminalLog(QString("  \033[90m│\033[0m %1").arg(s), "backup");
                }
            }
            if (rc.exitCode() == 0) {
                writeTerminalLog(QString("\033[1;32m[rclone] ✓ %1 완료\033[0m").arg(platName), "backup");
            } else {
                writeTerminalLog(QString("\033[1;31m[rclone] ✗ %1 실패 (exit=%2)\033[0m").arg(platName).arg(rc.exitCode()), "backup");
            }
        }
        // conf 삭제 (보안 — pass obfuscated 만 있어도 노출 X)
        QFile::remove(confPath);
        writeTerminalLog("\033[1;36m═══════════════════════════════════════════════════════════════\033[0m", "backup");
        writeTerminalLog("\033[1;32m✅ rclone 백업 완료\033[0m", "backup");
        writeTerminalLog("[DONE]", "backup");
        QMetaObject::invokeMethod(this, [this]() {
            m_backupTerminalActive = false;
            log("✅ rclone 백업 완료", "success", "settings");
            closeTerminalLog("backup");
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

// ═════════════════════════════════════════════════════════════════════════
// backupNow — 1회성 즉시 백업 (toggle off 여도 강제 실행).
//   경로 미설정 시 안내. 모든 플랫폼 폴더 → backup 경로 통째 enqueue.
// ═════════════════════════════════════════════════════════════════════════
void MiyoBackend::backupNow()
{
    QString backupRoot = m_config->backupPath();
    if (backupRoot.isEmpty()) {
        log("⚠ 백업 경로 미설정 — 먼저 [📂 경로 선택] 누르세요", "warning", "settings");
        runJs("alert('📦 백업 경로가 설정되지 않았습니다.\\n\\n[📂 경로 선택] 버튼으로 백업할 폴더부터 지정해주세요.');");
        return;
    }
    if (!QDir(backupRoot).exists()) {
        log(QString("⚠ 백업 경로 없음: %1 (마운트 끊김 / 옛 경로 / 권한 X)").arg(backupRoot), "warning", "settings");
        // ★ 자동 pickBackupPath dialog — 사용자가 [경로 선택] 다시 누르는 수고 줄임
        //   Mountain Duck / WebDAV / 외장 모두 listing 자동 등장
        runJs("alert('📦 백업 경로에 접근할 수 없습니다 — 마운트 끊김 또는 옛 경로.\\n\\n[확인] 누르면 새 경로 선택 dialog 자동 표시');");
        QMetaObject::invokeMethod(this, [this]() {
            pickBackupPath();  // 사용자가 새 path 선택 → 다음 백업 시 사용
        }, Qt::QueuedConnection);
        return;
    }
    // 백업 경로 OK — 사용 자제 (dead code skip)
    if (false) {
        // 진단 — 어디까지 존재하는지 표시 (mount 끊긴 지점 파악)
        QString diagnostic;
        QString check = backupRoot;
        QStringList segs;
        while (check.contains('/')) {
            segs.prepend(check);
            int slash = check.lastIndexOf('/');
            if (slash <= 0) break;
            check = check.left(slash);
        }
        QString lastExisting;
        QString firstMissing;
        for (const QString &seg : segs) {
            if (QDir(seg).exists()) lastExisting = seg;
            else { firstMissing = seg; break; }
        }
        if (!lastExisting.isEmpty()) diagnostic += "✅ 존재: " + lastExisting + "\\n";
        if (!firstMissing.isEmpty()) diagnostic += "❌ 없음: " + firstMissing + "\\n";
        log(QString("진단 — %1 / %2").arg(lastExisting).arg(firstMissing), "info", "settings");

        QString safeRoot = Common::jsStringLiteral(backupRoot);
        safeRoot = safeRoot.mid(1).chopped(1);  // strip surrounding quotes
        runJs(QString(
            "alert('📦 백업 경로에 접근할 수 없습니다:\\n%1\\n\\n%2\\n원인 가능성:\\n"
            "  1) NAS / WebDAV 마운트가 끊김 — Finder 에서 다시 마운트\\n"
            "  2) Mountain Duck 연결 중지됨 — Mountain Duck 앱 확인\\n"
            "  3) NAS 자체 꺼짐 / 네트워크 끊김\\n"
            "  4) 경로가 옛 마운트 — [📂 경로 선택] 다시 누르고 새 경로 지정\\n\\n"
            "👉 [📂 경로 선택] 버튼 다시 누르세요.');").arg(safeRoot).arg(diagnostic));
        return;
    }
    if (m_backupTerminalActive.load()) {
        log("⚠ 이미 백업 진행 중 — 끝난 후 다시 시도", "warning", "settings");
        runJs("alert('📦 이미 백업이 진행 중입니다.\\n터미널 창에서 진행 상황을 확인하세요.');");
        return;
    }

    log(QString("📦 지금 백업 시작 (병렬 cp 모드, 한국어/일본어 path NAS 호환) → %1").arg(backupRoot), "info", "settings");

    QJsonObject form = m_config->formData();
    const QStringList platKeys = {"twitter-path","bsky-path","youtube-path","discord-path",
                                   "instagram-path","pixiv-path","fanbox-path","tumblr-path","spinspin-path",
                                   "asked-path","crawl-path","trad-path","naikakukai-path"};
    QStringList platformDirs;
    for (const QString &k : platKeys) {
        QString p = form[k].toString();
        if (p.isEmpty()) continue;
        p.replace("~", QDir::homePath());
        if (QDir(p).exists() && p != backupRoot) {
            platformDirs << p;
        }
    }
    if (platformDirs.isEmpty()) {
        log("⚠ 다운로드 폴더 없음 — 수집 먼저 진행하거나 플랫폼별 저장 경로 확인", "warning", "settings");
        runJs("alert('📦 백업할 다운로드 폴더가 없습니다.\\n\\n각 플랫폼 탭에서 저장 경로를 확인하거나 수집을 먼저 진행하세요.');");
        return;
    }

    // ★ 실제 파일이 있는 폴더만 — 빈 폴더는 백업 대상에서 제외 (안 쓰는 platform 제거)
    {
        QStringList nonEmpty;
        for (const QString &p : platformDirs) {
            QDirIterator it(p, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
            bool hasFile = false;
            while (it.hasNext()) {
                QString f = it.next();
                if (f.contains("/.abiwa_") || f.contains("/.DS_Store") || f.contains("/.git/")) continue;
                hasFile = true; break;
            }
            if (hasFile) nonEmpty << p;
        }
        if (nonEmpty.isEmpty()) {
            log("⚠ 모든 다운로드 폴더가 비어있음 — 수집 먼저 진행", "warning", "settings");
            runJs("alert('📦 모든 다운로드 폴더가 비어있습니다.\\n\\n수집 먼저 진행하세요.');");
            return;
        }
        platformDirs = nonEmpty;
    }

    // ★ 공통 부모 폴더명 — dst 경로에만 prefix 로 추가 (src 는 platform 폴더 각각 별개)
    //   예: src = ~/Downloads/내각회/Twitter, ~/Downloads/내각회/Pixiv
    //       commonParent = ~/Downloads/내각회 → parentName = "내각회"
    //       dst = <backup>/내각회/Twitter, <backup>/내각회/Pixiv
    //   ★ 이전엔 platformDirs 를 commonParent 로 교체 → 부모 폴더 안 다른 파일까지 백업 ← 버그
    //   ★ 지금은 platform 폴더만 listing, dst 만 parentName prefix
    QString commonParentName;
    {
        QString commonParent;
        if (platformDirs.size() > 1) {
            QString prefix = platformDirs.first();
            for (const QString &p : platformDirs) {
                int i = 0;
                while (i < prefix.length() && i < p.length() && prefix[i] == p[i]) i++;
                prefix = prefix.left(i);
            }
            int slash = prefix.lastIndexOf('/');
            if (slash > 0) commonParent = prefix.left(slash);
        }
        QString home = QDir::homePath();
        QStringList genericParents = {home, home + "/Downloads", home + "/Documents", home + "/Desktop",
                                       "/", "/Users", "/Volumes", ""};
        bool useCommonParent = !commonParent.isEmpty()
            && !genericParents.contains(commonParent)
            && commonParent.length() > home.length() + 1
            && QDir(commonParent).exists();
        if (useCommonParent) {
            commonParentName = QFileInfo(commonParent).fileName();  // e.g. "내각회"
            log(QString("💡 공통 부모 감지: %1 — NAS dst 에 \"%2/\" prefix 사용 (그 폴더 자체는 백업 안 함, 안의 platform 폴더만)")
                .arg(commonParent, commonParentName), "info", "settings");
        }
    }

    // 터미널 창 (애니메이션 + 컬러)
    openBackupTerminalLog();
    m_backupTerminalActive = true;
    m_backupStartMs = QDateTime::currentMSecsSinceEpoch();

    // ★ rclone 자동 사용 — WebDAV creds 있고 backup path 가 *Apple* WebDAV mount 면
    //   Mountain Duck path 면 절대 rclone 안 씀 (Duck 가 자체 transfer 더 효율적).
    //   rclone copy 가 Apple native cp 보다 5-50배 빠름 (HTTP/2 multiplex, 8 parallel, resume).
    {
        QString url = m_config->webdavUrl();
        QString user = m_config->webdavUser();
        QString pass = m_config->webdavPass();
        bool _isAppleWebDav = false;
        bool _isDuckPath = backupRoot.contains("Mountain Duck", Qt::CaseInsensitive)
                       || backupRoot.contains("io.mountainduck")
                       || backupRoot.contains("Volumes.noindex")
                       || backupRoot.contains("/Library/CloudStorage/")
                       || backupRoot.contains("/Cyberduck/", Qt::CaseInsensitive);
#ifdef Q_OS_MACOS
        if (!_isDuckPath) {
            QProcess _mc; _mc.start("/sbin/mount", QStringList());
            if (_mc.waitForFinished(3000)) {
                QString out = QString::fromUtf8(_mc.readAllStandardOutput());
                for (const QString &line : out.split('\n')) {
                    int onIdx = line.indexOf(" on ");
                    int parenIdx = line.indexOf(" (", onIdx);
                    if (onIdx < 0 || parenIdx < 0) continue;
                    QString mp = line.mid(onIdx + 4, parenIdx - onIdx - 4);
                    if (!backupRoot.startsWith(mp)) continue;
                    QString opts = line.mid(parenIdx + 2);
                    if (opts.contains("webdav") && !opts.contains("osxfuse") && !opts.contains("macfuse")
                        && !mp.contains("io.mountainduck")) {
                        _isAppleWebDav = true;
                    }
                    break;
                }
            }
        }
#endif
        if (_isAppleWebDav && !url.isEmpty() && !user.isEmpty() && !pass.isEmpty()) {
            // backupRoot 에서 webdav mount root 제외한 sub path 추출
            QString destSub = backupRoot;
            // mount 의 root 가 보통 /Volumes/<host>/ — 그 뒤가 backup path 안 sub
            // 사용자 path 그대로 destSub 으로 (rclone 가 알아서 처리)
            writeTerminalLog("\033[1;32m🚀 rclone 모드 — Apple WebDAV 보다 5-50배 빠름 (Mountain Duck 와 동일 원리)\033[0m", "backup");
            writeTerminalLog("", "backup");
            // rclone 의 dest path = backup path 안의 sub-folder structure
            //   여기선 webdav root 의 / 부터 시작 (사용자가 NAS 안 어디든)
            //   너무 복잡해서 그냥 / 사용 — 사용자가 rclone config 의 url 마지막에 path 포함시킨 거 가정
            //   ★ 향후 개선: backupRoot 와 mount root 비교해서 정확한 sub path 추출
            QString destPath = "/";  // rclone url 자체에 path 있으면 root
            QStringList srcDirs;
            // 공통 부모 + 빈 폴더 제외 (기존 로직 복제)
            QStringList _platformDirs = platformDirs;
            QStringList _nonEmpty;
            for (const QString &p : _platformDirs) {
                QDirIterator it(p, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
                bool hasFile = false;
                while (it.hasNext()) { QString f = it.next();
                    if (!f.contains("/.abiwa_") && !f.contains("/.DS_Store")) { hasFile = true; break; } }
                if (hasFile) _nonEmpty << p;
            }
            if (_nonEmpty.isEmpty()) {
                m_backupTerminalActive = false;
                log("⚠ 빈 폴더만 있음 — 백업 중단", "warning", "settings");
                return;
            }
            runRcloneBackup(_nonEmpty, destPath);
            return;  // rclone thread 가 마무리
        }
    }

    // ★ 워커 수 — NAS mount type 자동 감지:
    //   - Mountain Duck (osxfuse/macfuse + 경로명) → 8 (local cache, write 즉시 응답)
    //   - SMB/AFP/NFS → 4 (병렬 효과 있음)
    //   - WebDAV (Apple native, 자격증명 X) → 1 (latency 곱셈 회피)
    int CONCURRENT = 4;
    bool isWebDav = false;
    bool isMountainDuck = false;
#ifdef Q_OS_MACOS
    {
        // 1) 경로 패턴으로 Mountain Duck 감지 (가장 robust — mount 명령보다 신뢰)
        const QStringList duckPatterns = {
            "Mountain Duck", "/Cyberduck/", "io.mountainduck",
            "Volumes.noindex", "/Library/CloudStorage/"
        };
        for (const QString &p : duckPatterns) {
            if (backupRoot.contains(p, Qt::CaseInsensitive)) {
                isMountainDuck = true; break;
            }
        }
        // 2) mount fstype 체크 — webdav / osxfuse / macfuse / 또는 Mountain Duck NFS server
        QProcess mountCmd;
        mountCmd.start("/sbin/mount", QStringList());
        if (mountCmd.waitForFinished(3000)) {
            QString mountOut = QString::fromUtf8(mountCmd.readAllStandardOutput());
            for (const QString &line : mountOut.split('\n')) {
                int onIdx = line.indexOf(" on ");
                if (onIdx < 0) continue;
                int parenIdx = line.indexOf(" (", onIdx);
                if (parenIdx < 0) continue;
                QString mountPath = line.mid(onIdx + 4, parenIdx - onIdx - 4);
                if (!backupRoot.startsWith(mountPath)) continue;
                QString opts = line.mid(parenIdx + 2);
                // Mountain Duck 5+ NFS 가상 서버 — mountPath 가 io.mountainduck 안이면 Duck
                if (mountPath.contains("io.mountainduck")
                    || mountPath.contains("Mountain Duck", Qt::CaseInsensitive)
                    || mountPath.contains("Volumes.noindex")
                    || opts.contains("osxfuse") || opts.contains("macfuse")
                    || opts.contains("mountainduck", Qt::CaseInsensitive)) {
                    isMountainDuck = true;
                }
                if (opts.contains("webdav") && !isMountainDuck) {
                    isWebDav = true;
                }
                break;
            }
        }
        if (isMountainDuck) {
            CONCURRENT = 8;  // Mountain Duck = local cache → write 즉시 응답, background upload
        } else if (isWebDav) {
            CONCURRENT = 1;  // Apple native WebDAV = latency 곱셈
        }
    }
#endif

    // ★ 모니터 종료 시 STOP sentinel 파일 만들어짐 (script trap) → 워치독이 발견하면 백업 중지
    QString stopSentinel = m_terminalLogPaths.value("backup") + ".STOP";
    QFile::remove(stopSentinel);
    writeTerminalLog("\033[1;36m📦 ABIWA 백업 시작 — 파일별 병렬 cp 모드 (한국어/일본어/NAS 안전)\033[0m", "backup");
    writeTerminalLog(QString("\033[90m  📂 백업 위치:\033[0m %1").arg(backupRoot), "backup");
    writeTerminalLog(QString("\033[90m  🕐 시작 시각:\033[0m %1").arg(QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss")), "backup");
    writeTerminalLog(QString("\033[90m  📁 대상 폴더:\033[0m %1 개  /  \033[90m🚀 병렬 워커:\033[0m %2").arg(platformDirs.size()).arg(CONCURRENT), "backup");
    if (isMountainDuck) {
        writeTerminalLog("\033[1;32m  🦆 Mountain Duck 마운트 감지\033[0m — local cache 사용 (write 즉시 응답)", "backup");
        writeTerminalLog("\033[32m     ★ cp 는 cache 에 빠르게 → 실제 NAS upload 는 Mountain Duck 가 background 진행\033[0m", "backup");
        writeTerminalLog("\033[32m     ★ 워커 8개 (cache write 빠름) · timeout 길게 (cache 동기화 대기)\033[0m", "backup");
        writeTerminalLog("\033[33m     ⚠ 백업 \"완료\" 후에도 Mountain Duck 앱에서 실제 upload 진행 — Duck 아이콘 확인\033[0m", "backup");
    } else if (isWebDav) {
        writeTerminalLog("\033[1;33m  🌐 WebDAV mount 감지\033[0m — 각 파일별 HTTP latency 곱셈 → 본질적으로 느림", "backup");
        writeTerminalLog("\033[33m     ★ NAS 가 본인 소유 아니면 변경 못 함. 다음 옵션 권장:\033[0m", "backup");
        writeTerminalLog("\033[33m       1) ★ Mountain Duck 설치 (https://mountainduck.io) — Apple WebDAV 보다 100배 빠름\033[0m", "backup");
        writeTerminalLog("\033[33m       2) Finder 에서 폴더 통째 drag&drop (Finder 가 약간 빠름)\033[0m", "backup");
        writeTerminalLog("\033[33m       3) trad 로 1개 ZIP 만들고 그 1개만 NAS 에 업로드 (latency 1회)\033[0m", "backup");
    }
    writeTerminalLog("\033[90m  🛡  안전 모드:\033[0m temp file 에 받고 완료 시 rename (\033[32m연결 끊겨도 깨진 파일 안 남음\033[0m)", "backup");
    writeTerminalLog("", "backup");

    // 백그라운드 — 사전 스캔 + 파일 단위 병렬 cp
    QThread *coordinator = QThread::create([this, platformDirs, backupRoot, CONCURRENT, isMountainDuck, commonParentName]() {
        // 사이즈 포맷
        auto fmtBytes = [](qint64 b) {
            if (b < 1024) return QString::number(b) + "B";
            if (b < 1024LL*1024) return QString::number(b/1024.0, 'f', 1) + "KB";
            if (b < 1024LL*1024*1024) return QString::number(b/1024.0/1024, 'f', 1) + "MB";
            return QString::number(b/1024.0/1024/1024, 'f', 2) + "GB";
        };
        auto fmtTime = [](qint64 secs) {
            if (secs < 0) return QString("--");
            if (secs < 60) return QString("%1초").arg(secs);
            if (secs < 3600) return QString("%1분 %2초").arg(secs/60).arg(secs%60);
            if (secs < 86400) return QString("%1시간 %2분").arg(secs/3600).arg((secs%3600)/60);
            // 24시간 초과 — 일 단위 + ⚠ (너무 느림 의미)
            return QString("⚠ %1일 %2시간").arg(secs/86400).arg((secs/3600)%24);
        };

        // 0-pre) ★ 백업 위치 기존 데이터 측정 — src/dst 사이즈 다른 이유 진단
        //   "src 55GB 인데 backup 104GB 왜?" → 옛 백업 잔재 + 다른 사용자 데이터 등 확인
        writeTerminalLog("\033[33m📏 백업 위치 기존 데이터 측정 중...\033[0m", "backup");
        qint64 existingBytes = 0;
        int existingFiles = 0;
        {
            QDirIterator dit(backupRoot, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
            while (dit.hasNext()) {
                QString f = dit.next();
                QString fn = QFileInfo(f).fileName();
                if (fn.startsWith(".abiwa_write_test") || fn.startsWith("__CHERNOBYL_MANIFEST")) continue;
                existingBytes += QFileInfo(f).size();
                existingFiles++;
            }
        }
        if (existingFiles > 0) {
            writeTerminalLog(QString("  \033[90m▸ 기존 NAS 데이터: %1 파일 (%2)\033[0m")
                .arg(existingFiles).arg(fmtBytes(existingBytes)), "backup");
        } else {
            writeTerminalLog("  \033[90m▸ 기존 NAS 데이터: 없음 (빈 폴더)\033[0m", "backup");
        }
        writeTerminalLog("", "backup");

        // 0) ★ NAS write 권한 검증 — 마운트는 됐지만 write 안 되는 경우 (read-only / 권한 / 잘못된 비번)
        //   1 파일 쓰기 시도 → 실패면 명확한 에러 + 백업 중단 (빈 폴더만 만드는 ㅈㄹ 방지)
        writeTerminalLog("\033[33m🔐 NAS write 권한 테스트 중...\033[0m", "backup");
        QString testFile = backupRoot + "/.abiwa_write_test_" + QString::number(QDateTime::currentMSecsSinceEpoch());
        {
            QFile tf(testFile);
            bool writeOk = false;
            QString writeErr;
            if (tf.open(QIODevice::WriteOnly)) {
                if (tf.write("abiwa_test_4byte") == 16) {
                    tf.close();
                    if (QFile::exists(testFile) && QFileInfo(testFile).size() == 16) {
                        writeOk = true;
                    } else {
                        writeErr = "쓰기 후 파일 검증 실패 (NAS silent fail)";
                    }
                } else {
                    writeErr = "tf.write 실패: " + tf.errorString();
                }
                QFile::remove(testFile);
            } else {
                writeErr = "tf.open 실패: " + tf.errorString();
            }
            if (!writeOk) {
                writeTerminalLog(QString("\033[1;31m❌ NAS write 권한 없음 — 백업 중단\033[0m"), "backup");
                writeTerminalLog(QString("  \033[33m└─ 위치:\033[0m %1").arg(backupRoot), "backup");
                writeTerminalLog(QString("  \033[33m└─ 원인:\033[0m %2").arg(writeErr), "backup");
                writeTerminalLog("", "backup");
                writeTerminalLog("\033[33m💡 해결책:\033[0m", "backup");
                writeTerminalLog("  1) Finder 에서 백업 폴더 우클릭 → '정보 가져오기' → '권한' 확인", "backup");
                writeTerminalLog("  2) WebDAV 라면 마운트 시 '쓰기 가능' 옵션 확인 (사용자명/비번)", "backup");
                writeTerminalLog("  3) NAS 관리자 페이지에서 공유 폴더 권한 'Read+Write' 확인", "backup");
                writeTerminalLog("[DONE]", "backup");
                QMetaObject::invokeMethod(this, [this, backupRoot, writeErr]() {
                    m_backupTerminalActive = false;
                    log(QString("❌ NAS write 권한 없음: %1\n   원인: %2\n   Finder 에서 권한 확인 후 다시 시도하세요.")
                        .arg(backupRoot).arg(writeErr), "error", "settings");
                    closeTerminalLog("backup");
                }, Qt::QueuedConnection);
                return;
            }
            writeTerminalLog(QString("\033[1;32m✅ NAS write 권한 OK\033[0m"), "backup");
            writeTerminalLog("", "backup");
        }

        // 1) 사전 스캔 — 파일 list + 플랫폼별 통계
        //   ★ platDstRoot 의 mkpath 는 여기서 안 함 — worker 가 파일 cp 직전에만 (빈 폴더 안 남기게)
        writeTerminalLog("\033[33m🔍 스캔 중... (파일 목록 작성)\033[0m", "backup");
        struct Job { QString src; QString dst; qint64 bytes; };
        QList<Job> jobs;
        QSet<QString> dstDirsToCleanup;  // cp 실패 시 빈 폴더 정리할 후보
        qint64 grandBytes = 0;
        int grandFiles = 0;
        for (const QString &platDir : platformDirs) {
            QString platName = QFileInfo(platDir).fileName();
            // dst = <backup>/<parent?>/<platName>  (parent 있으면 prefix)
            QString platDstRoot = commonParentName.isEmpty()
                ? backupRoot + "/" + platName
                : backupRoot + "/" + commonParentName + "/" + platName;
            qint64 platSz = 0; int platN = 0;
            // ★ 상위 sub-folder 별 사이즈 누적 — 175GB 같은 큰 양일 때 어디가 큰지 사용자가 봄
            QMap<QString, QPair<int, qint64>> subFolderStats;  // subName → (count, bytes)
            QDirIterator it(platDir, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
            while (it.hasNext()) {
                QString f = it.next();
                if (f.contains("/.abiwa_") || f.contains("/.DS_Store") || f.contains("/.git/")) continue;
                if (f.contains("/.rsync-partial") || f.contains("/.rsync-tmp")) continue;
                if (f.startsWith(backupRoot)) continue;  // 자기 자신 skip
                // 상대 경로 — platDir 기준
                QString rel = f.mid(platDir.length());
                if (rel.startsWith("/")) rel = rel.mid(1);
                QString dst = platDstRoot + "/" + rel;
                qint64 sz = QFileInfo(f).size();
                jobs.append({f, dst, sz});
                platSz += sz; platN++;
                // sub-folder breakdown
                int firstSlash = rel.indexOf('/');
                QString subName = firstSlash > 0 ? rel.left(firstSlash) : "(root files)";
                subFolderStats[subName].first++;
                subFolderStats[subName].second += sz;
            }
            if (platN > 0) {
                grandBytes += platSz;
                grandFiles += platN;
                dstDirsToCleanup.insert(platDstRoot);
                writeTerminalLog(QString("  \033[1;36m▸\033[0m \033[1m%1\033[0m — %2 파일 (\033[1;33m%3\033[0m) → %4")
                    .arg(platName).arg(platN).arg(fmtBytes(platSz)).arg(platDstRoot), "backup");
                // 큰 sub-folder Top 10 표시 (175GB 같은 양일 때 어디가 큰지 즉시 보임)
                QList<QPair<QString, QPair<int, qint64>>> subList;
                for (auto sit = subFolderStats.constBegin(); sit != subFolderStats.constEnd(); ++sit)
                    subList.append({sit.key(), sit.value()});
                std::sort(subList.begin(), subList.end(),
                    [](const auto &a, const auto &b){ return a.second.second > b.second.second; });
                int showN = qMin(10, subList.size());
                for (int i = 0; i < showN; ++i) {
                    const auto &s = subList[i];
                    writeTerminalLog(QString("      \033[90m└─\033[0m %1/   %2 파일   %3")
                        .arg(s.first.left(50), -50).arg(s.second.first, 6).arg(fmtBytes(s.second.second)), "backup");
                }
                if (subList.size() > showN) {
                    qint64 restSz = 0; int restN = 0;
                    for (int i = showN; i < subList.size(); ++i) {
                        restN += subList[i].second.first;
                        restSz += subList[i].second.second;
                    }
                    writeTerminalLog(QString("      \033[90m└─\033[0m ... 외 %1 개 폴더   %2 파일   %3")
                        .arg(subList.size() - showN).arg(restN).arg(fmtBytes(restSz)), "backup");
                }
            }
        }
        writeTerminalLog("", "backup");
        writeTerminalLog(QString("\033[1;32m✅ 스캔 완료 — 총 %1 파일 (%2)\033[0m")
            .arg(grandFiles).arg(fmtBytes(grandBytes)), "backup");
        writeTerminalLog("", "backup");
        writeTerminalLog(QString("\033[1;35m━━━━━━━━━━ 백업 시작 (병렬 %1 워커) ━━━━━━━━━━\033[0m").arg(CONCURRENT), "backup");
        writeTerminalLog("", "backup");

        // 2) 병렬 cp workers
        std::atomic<int> nextIdx(0);
        std::atomic<qint64> doneBytes(0);
        std::atomic<int> doneFiles(0);
        std::atomic<int> skipCount(0);
        std::atomic<int> failCount(0);
        std::atomic<bool> stoppedByUser(false);

        // 워치독 — STOP sentinel 폴링
        QString stopSentinelLocal = m_terminalLogPaths.value("backup") + ".STOP";
        QThread *watchdog = QThread::create([this, stopSentinelLocal, &stoppedByUser]() {
            while (m_backupTerminalActive.load()) {
                if (QFile::exists(stopSentinelLocal)) {
                    stoppedByUser = true;
                    m_backupTerminalActive = false;
                    QFile::remove(stopSentinelLocal);
                    break;
                }
                QThread::msleep(300);
            }
        });
        watchdog->start();

        // 진행률 정기 출력 thread (2초마다, 첫 출력은 즉시)
        std::atomic<bool> progressRunning(true);
        std::atomic<bool> slowWarned(false);
        QThread *progressTh = QThread::create([this, &doneBytes, &doneFiles, &skipCount, &failCount, &progressRunning, &slowWarned, grandBytes, grandFiles, fmtBytes, fmtTime, isMountainDuck]() {
            bool firstTick = true;
            while (progressRunning.load()) {
                // 첫 tick 은 1초 후 (워커 시작 보임), 그 다음은 2초마다
                QThread::msleep(firstTick ? 1000 : 2000);
                firstTick = false;
                if (!progressRunning.load()) break;
                // ★ 너무 느림 자동 중단 — 평균 속도 < 100KB/s 60초 지속 → ETA 계산상 1주일+
                //   사용자가 93일 기다리지 않게 강제 중단 + 명확한 대안 제시
                //   ★ Mountain Duck 은 cache 사용 — 느려보여도 실제 upload background. skip.
                qint64 _db = doneBytes.load();
                qint64 _elapsed = (QDateTime::currentMSecsSinceEpoch() - m_backupStartMs.load()) / 1000;
                if (!isMountainDuck && !slowWarned.load() && _elapsed > 60 && _db > 0) {
                    qint64 sp = _db / _elapsed;
                    if (sp < 100 * 1024) {
                        slowWarned = true;
                        writeTerminalLog("", "backup");
                        writeTerminalLog("\033[1;31m═══════════════════════════════════════════════════════════════\033[0m", "backup");
                        writeTerminalLog("\033[1;31m  🛑 자동 중단 — NAS 속도 너무 느림 (60초 평균 < 100KB/s)\033[0m", "backup");
                        writeTerminalLog("\033[1;31m═══════════════════════════════════════════════════════════════\033[0m", "backup");
                        writeTerminalLog(QString("\033[33m  현재 속도: %1/s · 남은 데이터 ETA: 수십 일\033[0m").arg(fmtBytes(sp)), "backup");
                        writeTerminalLog("", "backup");
                        writeTerminalLog("\033[1;36m  💡 대안 (NAS 가 본인 소유 아니면 NAS 설정 변경 불가):\033[0m", "backup");
                        writeTerminalLog("\033[33m  1) ★ Finder 에서 src 폴더 통째 drag&drop (가장 빠름 — Finder 최적화)\033[0m", "backup");
                        writeTerminalLog("\033[33m  2) trad 탭에서 폴더 → 1개 PNG/ZIP → 그 1개만 NAS 에 drag&drop\033[0m", "backup");
                        writeTerminalLog("\033[33m  3) 다른 NAS / 외장 SSD 백업 (USB/Thunderbolt = 50배 빠름)\033[0m", "backup");
                        writeTerminalLog("\033[33m  4) NAS 소유자에게 SMB/AFP 마운트 요청 (WebDAV 100배 느림)\033[0m", "backup");
                        writeTerminalLog("\033[1;31m═══════════════════════════════════════════════════════════════\033[0m", "backup");
                        // 자동 중단
                        m_backupTerminalActive = false;
                        // GUI dialog
                        QMetaObject::invokeMethod(this, [this]() {
                            runJs("alert('🛑 백업 자동 중단 — NAS 속도 너무 느림 (< 100KB/s)\\n\\n남은 백업 ETA: 수십 일\\n\\n💡 대안:\\n  1) Finder 에서 폴더 통째 drag&drop\\n  2) trad 로 1개 ZIP 묶어서 그것만 업로드\\n  3) USB 외장 SSD 사용 (50배 빠름)\\n  4) NAS 소유자에게 SMB/AFP 변경 요청');");
                            log("🛑 백업 자동 중단 — NAS 속도 < 100KB/s. Finder drag&drop 또는 trad 모드 권장.", "error", "settings");
                        }, Qt::QueuedConnection);
                    }
                }
                qint64 db = doneBytes.load();
                int dn = doneFiles.load();
                qint64 elapsed = (QDateTime::currentMSecsSinceEpoch() - m_backupStartMs.load()) / 1000;
                int overallPct = grandBytes > 0 ? int(db * 100 / grandBytes) : 0;
                qint64 speed = elapsed > 0 ? db / elapsed : 0;
                qint64 remain = grandBytes - db;
                qint64 eta = (speed > 0 && remain > 0) ? (remain / speed) : -1;
                // ★ 정밀 진행률 — 1% 미만도 보이게:
                //   - bar: 40 character, 각 char 안에서도 8단계 (▏▎▍▌▋▊▉█) 로 fractional
                //   - 175GB 의 0.06% 도 시각적으로 채워짐
                int barWidth = 40;
                double pctExact = grandBytes > 0 ? (double(db) * 100.0 / double(grandBytes)) : 0.0;
                double charsTotal = (pctExact * barWidth) / 100.0;
                int fullChars = int(charsTotal);
                int fractionLevel = int((charsTotal - fullChars) * 8);
                static const QString partials[] = {"", "▏", "▎", "▍", "▌", "▋", "▊", "▉"};
                QString bar = QString("█").repeated(fullChars);
                if (fullChars < barWidth) {
                    bar += partials[fractionLevel];
                    bar += QString("░").repeated(barWidth - fullChars - 1);
                }
                // 퍼센트는 < 1% 면 소수점 2자리, 그 이상은 1자리
                QString pctStr;
                if (pctExact < 1.0) pctStr = QString("%1%%").arg(pctExact, 0, 'f', 2);
                else if (pctExact < 10.0) pctStr = QString("%1%%").arg(pctExact, 0, 'f', 1);
                else pctStr = QString("%1%%").arg(int(pctExact));
                writeTerminalLog(QString("\033[1;36m  [%1] \033[1;35m%2\033[0m  \033[90m(%3 / %4)\033[0m")
                    .arg(bar).arg(pctStr).arg(fmtBytes(db)).arg(fmtBytes(grandBytes)), "backup");
                // GUI 로그에도 progress 출력 (사용자가 앱에서 진행 봄)
                int dn2 = dn, total2 = grandFiles, pct2 = overallPct;
                qint64 db2 = db, gb2 = grandBytes, sp2 = speed, et2 = elapsed, eta2 = eta;
                QMetaObject::invokeMethod(this, [this, pct2, db2, gb2, sp2, et2, eta2, dn2, total2, fmtBytes, fmtTime]() {
                    log(QString("📊 진행 %1%% · %2/%3 · 속도 %4/s · 경과 %5 · ETA %6 · 파일 %7/%8")
                        .arg(pct2).arg(fmtBytes(db2)).arg(fmtBytes(gb2))
                        .arg(fmtBytes(sp2)).arg(fmtTime(et2)).arg(fmtTime(eta2))
                        .arg(dn2).arg(total2),
                        "info", "settings");
                }, Qt::QueuedConnection);
                writeTerminalLog(QString("\033[35m  📊 속도 %1/s · 경과 %2 · ETA %3 · 파일 %4/%5 · \033[32m✓%6\033[35m / \033[33m⏭%7\033[35m / \033[31m✗%8\033[0m")
                    .arg(fmtBytes(speed)).arg(fmtTime(elapsed)).arg(fmtTime(eta))
                    .arg(dn).arg(grandFiles)
                    .arg(dn - skipCount.load() - failCount.load()).arg(skipCount.load()).arg(failCount.load()), "backup");
                // GUI progress bar 도 갱신 (settings 탭의 backup progress bar)
                m_backupDoneBytes = db;
                m_backupTotalBytes = grandBytes;
                m_backupDoneCount = dn;
                m_backupTotalCount = grandFiles;
                m_backupLastProgressMs = 0;  // throttle 무시 — 즉시 emit
                emitBackupProgress();
            }
        });
        progressTh->start();

        // 워커들
        QList<QThread*> workers;
        for (int i = 0; i < CONCURRENT; ++i) {
            QThread *w = QThread::create([this, i, &jobs, &nextIdx, &doneBytes, &doneFiles, &skipCount, &failCount, fmtBytes]() {
                // 워커 시작 표시 — 터미널 + GUI 양쪽
                writeTerminalLog(QString("\033[1;34m[W%1] 워커 시작\033[0m").arg(i+1), "backup");
                QMetaObject::invokeMethod(this, [this, i]() {
                    log(QString("📦 백업 워커 %1 시작").arg(i+1), "info", "settings");
                }, Qt::QueuedConnection);
                while (m_backupTerminalActive.load()) {
                    int idx = nextIdx.fetch_add(1);
                    if (idx >= jobs.size()) break;
                    const Job &j = jobs[idx];

                    // dst 디렉토리 보장
                    QDir().mkpath(QFileInfo(j.dst).absolutePath());

                    // idempotent skip — 이미 같은 size 면 skip
                    if (QFile::exists(j.dst)) {
                        qint64 dstSz = QFileInfo(j.dst).size();
                        if (dstSz == j.bytes) {
                            doneBytes.fetch_add(j.bytes);
                            doneFiles.fetch_add(1);
                            skipCount.fetch_add(1);
                            continue;
                        }
                        // 사이즈 다르면 (partial) — 삭제 후 다시 cp
                        QFile::remove(j.dst);
                    }

                    // ★ 직접 dst 에 cp + 실제 사이즈 검증 + 3회 재시도
                    //   (이전 .abiwa_partial + atomic rename 방식은 NAS WebDAV/SMB 에서 rename fail)
                    //   ditto exit=0 이라도 NAS 가 silently fail 할 수 있음 → 반드시 dst 존재 + size 검증
                    bool ok = false;
                    QString method;
                    QString lastErr;
                    qint64 lastDstSize = -1;
                    // ★ Dynamic timeout — 파일 크기 비례. 1MB/s 보장 + 10초 buffer (15s ~ 20분 cap)
                    //   작은 파일 hang 시 빠르게 fail → 다음 파일 진행. 큰 파일은 충분히 기다림.
                    int dynTimeout = 10000 + int(j.bytes / (1024 * 1024)) * 1000;
                    if (dynTimeout < 15000) dynTimeout = 15000;
                    if (dynTimeout > 1200000) dynTimeout = 1200000;  // max 20분
                    for (int attempt = 1; attempt <= 3 && !ok && m_backupTerminalActive.load(); ++attempt) {
                        // 이전 attempt 잔재 정리
                        if (QFile::exists(j.dst)) QFile::remove(j.dst);

                        // [1] ditto — macOS native, mmap, xattr 보존
#ifdef Q_OS_MACOS
                        {
                            QProcess p;
                            p.setProcessChannelMode(QProcess::MergedChannels);
                            p.start("/usr/bin/ditto", {j.src, j.dst});
                            if (p.waitForFinished(dynTimeout) && p.exitCode() == 0) {
                                if (QFile::exists(j.dst)) {
                                    qint64 dSz = QFileInfo(j.dst).size();
                                    if (dSz == j.bytes) { ok = true; method = QString("ditto try%1").arg(attempt); }
                                    else { lastErr = QString("ditto exit=0 인데 size 불일치 (%1 vs %2)").arg(dSz).arg(j.bytes); lastDstSize = dSz; }
                                } else {
                                    lastErr = "ditto exit=0 인데 dst 파일 없음 (NAS silent fail)";
                                }
                            } else {
                                p.kill();
                                p.waitForFinished(2000);
                                QString errOut = QString::fromUtf8(p.readAll()).trimmed();
                                lastErr = QString("ditto timeout %1s 또는 exit=%2: %3")
                                    .arg(dynTimeout/1000).arg(p.exitCode()).arg(errOut.left(120));
                                // 첫 hang 은 사용자 즉시 표시 — 디버깅 위해
                                if (attempt == 1) {
                                    writeTerminalLog(QString("  \033[33m[W%1] ⏱ ditto 첫 시도 %2s 안 끝남 — cp 로 재시도:\033[0m %3 (%4)")
                                        .arg(i+1).arg(dynTimeout/1000).arg(QFileInfo(j.src).fileName()).arg(fmtBytes(j.bytes)),
                                        "backup");
                                }
                            }
                        }
#endif
                        // [2] cp -p
                        if (!ok) {
                            if (QFile::exists(j.dst)) QFile::remove(j.dst);
                            QProcess p;
                            p.setProcessChannelMode(QProcess::MergedChannels);
                            p.start("/bin/cp", {"-p", j.src, j.dst});
                            if (p.waitForFinished(dynTimeout) && p.exitCode() == 0) {
                                if (QFile::exists(j.dst)) {
                                    qint64 dSz = QFileInfo(j.dst).size();
                                    if (dSz == j.bytes) { ok = true; method = QString("cp try%1").arg(attempt); }
                                    else { lastErr = QString("cp exit=0 인데 size 불일치 (%1 vs %2)").arg(dSz).arg(j.bytes); lastDstSize = dSz; }
                                } else {
                                    lastErr = "cp exit=0 인데 dst 파일 없음 (NAS silent fail)";
                                }
                            } else {
                                p.kill();
                                p.waitForFinished(2000);
                                QString errOut = QString::fromUtf8(p.readAll()).trimmed();
                                lastErr = QString("cp timeout %1s 또는 exit=%2: %3")
                                    .arg(dynTimeout/1000).arg(p.exitCode()).arg(errOut.left(120));
                            }
                        }
                        // [3] QFile::copy
                        if (!ok) {
                            if (QFile::exists(j.dst)) QFile::remove(j.dst);
                            if (QFile::copy(j.src, j.dst)) {
                                if (QFile::exists(j.dst)) {
                                    qint64 dSz = QFileInfo(j.dst).size();
                                    if (dSz == j.bytes) { ok = true; method = QString("Qt try%1").arg(attempt); }
                                    else { lastErr = QString("QFile::copy 후 size 불일치 (%1 vs %2)").arg(dSz).arg(j.bytes); lastDstSize = dSz; }
                                }
                            } else {
                                lastErr = "QFile::copy 실패 (권한/디스크?)";
                            }
                        }
                        // 재시도 전 잠깐 sleep (NAS 회복 시간)
                        if (!ok && attempt < 3) QThread::msleep(1000);
                    }

                    if (ok) {
                        doneBytes.fetch_add(j.bytes);
                        doneFiles.fetch_add(1);
                        // 처음 20개 + 매 100개마다 출력 (사용자가 진행 확실히 봄)
                        int dn = doneFiles.load();
                        if (dn <= 20 || (dn % 100) == 0) {
                            writeTerminalLog(QString("  \033[90m[W%1]\033[0m \033[32m✓\033[0m %2 (%3, %4)")
                                .arg(i+1).arg(QFileInfo(j.src).fileName()).arg(fmtBytes(j.bytes)).arg(method),
                                "backup");
                        }
                        // 처음 5개 / 1000개마다 GUI 에도 출력 (사용자가 앱에서 진행 확인)
                        if (dn <= 5 || (dn % 1000) == 0) {
                            QString fnameCopy = QFileInfo(j.src).fileName();
                            QString methodCopy = method;
                            qint64 szCopy = j.bytes;
                            int iCopy = i;
                            int dnCopy = dn;
                            QMetaObject::invokeMethod(this, [this, dnCopy, iCopy, fnameCopy, szCopy, methodCopy, fmtBytes]() {
                                log(QString("✓ [%1] W%2 %3 (%4, %5)")
                                    .arg(dnCopy).arg(iCopy+1).arg(fnameCopy).arg(fmtBytes(szCopy)).arg(methodCopy),
                                    "success", "settings");
                            }, Qt::QueuedConnection);
                        }
                    } else {
                        // 부분 복사된 dst 가 있으면 (잘못된 size) 삭제 — 다음 호출 시 깨끗하게 재시도
                        if (QFile::exists(j.dst) && QFileInfo(j.dst).size() != j.bytes) QFile::remove(j.dst);
                        failCount.fetch_add(1);
                        QString reason;
                        if (!QFile::exists(j.src)) reason = "src 없음";
                        else if (!QDir(QFileInfo(j.dst).absolutePath()).exists()) reason = "dst 디렉토리 생성 실패 (NAS 마운트 끊김?)";
                        else reason = lastErr.isEmpty() ? "ditto/cp/Qt 3회 모두 실패" : lastErr;
                        writeTerminalLog(QString("\033[31m[W%1✗]\033[0m %2 (\033[33m%3\033[0m)\n      \033[90m└─ src:\033[0m %4\n      \033[90m└─ dst:\033[0m %5\n      \033[33m└─ 원인:\033[0m %6")
                            .arg(i+1).arg(QFileInfo(j.src).fileName()).arg(fmtBytes(j.bytes), j.src, j.dst, reason),
                            "backup");
                    }
                }
            });
            workers.append(w);
            w->start();
        }
        // 모든 워커 끝나길 wait
        for (QThread *w : workers) {
            w->wait();
            w->deleteLater();
        }
        m_backupTerminalActive = false;
        watchdog->wait();
        watchdog->deleteLater();
        progressRunning = false;
        progressTh->wait();
        progressTh->deleteLater();

        // 3-pre) ★ 사이즈 차이 분석 — src 와 dst 비교 (사용자가 "왜 src 55GB 인데 dst 104GB?" 의문 해결)
        {
            qint64 finalDstBytes = 0; int finalDstFiles = 0;
            QSet<QString> srcRelPaths;
            for (const Job &j : jobs) {
                // dst 경로의 상대 path (backupRoot 기준)
                QString rel = j.dst.mid(backupRoot.length());
                if (rel.startsWith("/")) rel = rel.mid(1);
                srcRelPaths.insert(rel);
            }
            qint64 orphanBytes = 0; int orphanFiles = 0;
            QDirIterator dit(backupRoot, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
            while (dit.hasNext()) {
                QString f = dit.next();
                QString fn = QFileInfo(f).fileName();
                if (fn.startsWith(".abiwa_write_test") || fn.startsWith("__CHERNOBYL_MANIFEST")) continue;
                qint64 sz = QFileInfo(f).size();
                finalDstBytes += sz;
                finalDstFiles++;
                QString rel = f.mid(backupRoot.length());
                if (rel.startsWith("/")) rel = rel.mid(1);
                if (!srcRelPaths.contains(rel)) {
                    orphanBytes += sz;
                    orphanFiles++;
                }
            }
            writeTerminalLog("", "backup");
            writeTerminalLog("\033[1;36m═══ 사이즈 진단 ═══\033[0m", "backup");
            writeTerminalLog(QString("  \033[36msrc (원본):\033[0m %1 파일 (%2)").arg(grandFiles).arg(fmtBytes(grandBytes)), "backup");
            writeTerminalLog(QString("  \033[36mdst (NAS):\033[0m  %1 파일 (%2)").arg(finalDstFiles).arg(fmtBytes(finalDstBytes)), "backup");
            if (orphanFiles > 0) {
                writeTerminalLog(QString("  \033[1;33m⚠ NAS 에만 있고 src 에 없는 파일: %1 개 (%2)\033[0m")
                    .arg(orphanFiles).arg(fmtBytes(orphanBytes)), "backup");
                writeTerminalLog("    → 옛 백업 잔재 또는 다른 데이터. 수동 정리 권장 (Finder 에서 확인)", "backup");
                QMetaObject::invokeMethod(this, [this, orphanFiles, orphanBytes, fmtBytes, backupRoot]() {
                    log(QString("⚠ NAS 에 src 와 무관한 파일 %1 개 (%2) 있음 — Finder 에서 %3 확인 후 수동 정리")
                        .arg(orphanFiles).arg(fmtBytes(orphanBytes)).arg(backupRoot),
                        "warning", "settings");
                }, Qt::QueuedConnection);
            } else {
                writeTerminalLog("  \033[32m✅ src 와 dst 완벽 일치 (orphan 없음)\033[0m", "backup");
            }
            writeTerminalLog("", "backup");
        }

        // 3) ★ 빈 폴더 cleanup — cp 실패한 경우 dst 부모 폴더만 mkpath 된 채 남는 거 정리
        //   백업 dst 폴더 (예: backupRoot/Twitter/) 안 recursive 로 빈 dir 찾아서 삭제
        std::function<void(const QString&)> rmEmptyDirs = [&](const QString &dir) {
            QDir d(dir);
            const QStringList subs = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
            for (const QString &s : subs) rmEmptyDirs(dir + "/" + s);
            // 이제 sub-dir 다 처리 후 본인 비었는지 체크
            QDir again(dir);
            if (again.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
                QDir().rmdir(dir);
            }
        };
        int cleanedDirs = 0;
        for (const QString &topDst : dstDirsToCleanup) {
            QDir before(topDst);
            int beforeN = before.entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot).size();
            rmEmptyDirs(topDst);
            // top dst 도 비었으면 삭제
            QDir after(topDst);
            if (!QFile::exists(topDst) || after.isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden)) {
                if (QFile::exists(topDst)) QDir().rmdir(topDst);
                if (beforeN == 0) cleanedDirs++;  // 원래 비어있던 거
            }
        }
        if (cleanedDirs > 0) {
            writeTerminalLog(QString("\033[90m🧹 빈 폴더 cleanup: %1 개 삭제\033[0m").arg(cleanedDirs), "backup");
        }

        qint64 elapsed = (QDateTime::currentMSecsSinceEpoch() - m_backupStartMs.load()) / 1000;
        qint64 avgSpeed = elapsed > 0 ? doneBytes.load() / elapsed : 0;
        int finalFail = failCount.load();
        int finalDone = doneFiles.load();
        int finalSkip = skipCount.load();
        bool wasStopped = stoppedByUser.load();
        writeTerminalLog("", "backup");
        writeTerminalLog("\033[1;36m═══════════════════════════════════════════════════════════════\033[0m", "backup");
        if (wasStopped) {
            writeTerminalLog(QString("\033[1;33m  ⛔ 사용자 중지 — 모니터 종료로 백업 중단 (완료 %1/%2 파일)\033[0m")
                .arg(finalDone).arg(grandFiles), "backup");
        } else if (finalFail == 0) {
            writeTerminalLog(QString("\033[1;32m  ✅ 백업 완료 — %1 파일 (%2)  [신규 %3 / skip %4]\033[0m")
                .arg(grandFiles).arg(fmtBytes(grandBytes))
                .arg(finalDone - finalSkip).arg(finalSkip), "backup");
        } else {
            writeTerminalLog(QString("\033[1;33m  ⚠  백업 완료 (실패 %1 파일) — 성공 %2/%3 파일\033[0m")
                .arg(finalFail).arg(finalDone).arg(grandFiles), "backup");
        }
        writeTerminalLog(QString("  \033[90m⏱  소요 시간:\033[0m %1").arg(fmtTime(elapsed)), "backup");
        writeTerminalLog(QString("  \033[90m🚀 평균 속도:\033[0m %1/s  (%2 워커 합산)").arg(fmtBytes(avgSpeed)).arg(CONCURRENT), "backup");
        writeTerminalLog(QString("  \033[90m📂 백업 위치:\033[0m %1").arg(backupRoot), "backup");
        writeTerminalLog("\033[1;36m═══════════════════════════════════════════════════════════════\033[0m", "backup");
        writeTerminalLog("[DONE]", "backup");

        QMetaObject::invokeMethod(this, [this, grandFiles, grandBytes, finalFail, finalDone, finalSkip, elapsed, avgSpeed, wasStopped, fmtBytes, fmtTime, backupRoot]() {
            m_backupTerminalActive = false;
            if (wasStopped) {
                log(QString("⛔ 백업 중단 — 모니터 종료 (완료 %1/%2 파일 · %3 · 평균 %4/s)")
                    .arg(finalDone).arg(grandFiles).arg(fmtTime(elapsed)).arg(fmtBytes(avgSpeed)),
                    "warning", "settings");
            } else if (finalFail == 0) {
                log(QString("✅ 백업 완료 — %1 파일 (%2) [skip %3] · %4 · 평균 %5/s → %6")
                    .arg(grandFiles).arg(fmtBytes(grandBytes)).arg(finalSkip)
                    .arg(fmtTime(elapsed)).arg(fmtBytes(avgSpeed)).arg(backupRoot),
                    "success", "settings");
            } else {
                log(QString("⚠ 백업 완료 — 실패 %1 파일 (성공 %2/%3 · %4)")
                    .arg(finalFail).arg(finalDone).arg(grandFiles).arg(fmtBytes(grandBytes)),
                    "warning", "settings");
            }
            closeTerminalLog("backup");
        }, Qt::QueuedConnection);
    });
    connect(coordinator, &QThread::finished, coordinator, &QThread::deleteLater);
    coordinator->start();
}

// ═════════════════════════════════════════════════════════════════════════
// logCollectionOptions — 사용자가 선택한 모든 옵션 (체크박스 / 입력값) 로그에 기록.
//   재현 + 디버깅 용. 시작 시 한 번 호출.
// ═════════════════════════════════════════════════════════════════════════
// ═════════════════════════════════════════════════════════════════════════
// writeDownloadManifest — 폴더 안 모든 파일 통계 manifest 생성.
//   JSON (__CHERNOBYL_MANIFEST__.json) + TXT (__CHERNOBYL_MANIFEST__.txt) 둘 다 생성.
//   각 platform 수집 끝나면 자동 호출. 사용자 / 백업 검증용.
//   대용량 (10만 파일+) 도 streaming 으로 처리 — 메모리 안 먹음.
// ═════════════════════════════════════════════════════════════════════════
void MiyoBackend::writeDownloadManifest(const QString &dir, const QString &platform)
{
    if (dir.isEmpty() || !QDir(dir).exists()) return;

    auto fmtBytes = [](qint64 b) -> QString {
        if (b < 1024) return QString::number(b) + " B";
        if (b < 1024LL*1024) return QString::number(b/1024.0, 'f', 1) + " KB";
        if (b < 1024LL*1024*1024) return QString::number(b/1024.0/1024, 'f', 1) + " MB";
        if (b < 1024LL*1024*1024*1024) return QString::number(b/1024.0/1024/1024, 'f', 2) + " GB";
        return QString::number(b/1024.0/1024/1024/1024, 'f', 2) + " TB";
    };

    // 1) Scan — 모든 파일 walk + 통계 누적
    QMap<QString, QPair<int, qint64>> byExt;       // 확장자 → (count, total size)
    QMap<QString, QPair<int, qint64>> byFolder;    // 상위 sub-folder → (count, size)
    QList<QPair<qint64, QString>> topFiles;        // (size, rel path) — 가장 큰 50개 추적
    int totalCount = 0;
    qint64 totalBytes = 0;
    QDirIterator it(dir, QDir::Files | QDir::NoSymLinks, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        QString f = it.next();
        QString fname = QFileInfo(f).fileName();
        // 시스템 / 매니페스트 자체 skip
        if (fname.startsWith(".") || fname.startsWith("__CHERNOBYL_MANIFEST")
            || fname == "Thumbs.db" || fname == "desktop.ini") continue;
        if (f.contains("/.abiwa_") || f.contains("/.rsync-")) continue;
        qint64 sz = QFileInfo(f).size();
        QString ext = QFileInfo(f).suffix().toLower();
        if (ext.isEmpty()) ext = "(no ext)";
        totalCount++;
        totalBytes += sz;
        byExt[ext].first++;
        byExt[ext].second += sz;
        // 상위 폴더 (root 바로 아래 segment)
        QString rel = f.mid(dir.length());
        if (rel.startsWith("/")) rel = rel.mid(1);
        int firstSlash = rel.indexOf('/');
        QString topFolder = firstSlash > 0 ? rel.left(firstSlash) : "(root)";
        byFolder[topFolder].first++;
        byFolder[topFolder].second += sz;
        // top files (size 큰 순, 50개 유지)
        topFiles.append({sz, rel});
        if (topFiles.size() > 200) {
            // 너무 많아지면 정리 (top 100 만 유지)
            std::partial_sort(topFiles.begin(), topFiles.begin() + 100, topFiles.end(),
                [](const auto &a, const auto &b){ return a.first > b.first; });
            topFiles.resize(100);
        }
    }
    if (totalCount == 0) return;  // 빈 폴더면 manifest 안 만듦

    // top 50 정렬
    std::sort(topFiles.begin(), topFiles.end(), [](const auto &a, const auto &b){ return a.first > b.first; });
    if (topFiles.size() > 50) topFiles.resize(50);

    // 2) JSON manifest
    QJsonObject root;
    root["version"] = 1;
    root["app"] = "Chernobyl";
    root["platform"] = platform;
    root["dir"] = dir;
    root["created_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    root["created_unix"] = QDateTime::currentSecsSinceEpoch();

    QJsonObject summary;
    summary["total_files"] = totalCount;
    summary["total_size_bytes"] = totalBytes;
    summary["total_size_human"] = fmtBytes(totalBytes);
    root["summary"] = summary;

    // 확장자별 (count 큰 순)
    QList<QPair<QString, QPair<int, qint64>>> extList;
    for (auto eit = byExt.constBegin(); eit != byExt.constEnd(); ++eit) extList.append({eit.key(), eit.value()});
    std::sort(extList.begin(), extList.end(), [](const auto &a, const auto &b){ return a.second.first > b.second.first; });
    QJsonArray extArr;
    for (const auto &e : extList) {
        QJsonObject eo;
        eo["ext"] = e.first;
        eo["count"] = e.second.first;
        eo["size_bytes"] = e.second.second;
        eo["size_human"] = fmtBytes(e.second.second);
        eo["percent"] = totalBytes > 0 ? double(e.second.second) * 100.0 / double(totalBytes) : 0.0;
        extArr.append(eo);
    }
    root["by_extension"] = extArr;

    // 폴더별 (size 큰 순)
    QList<QPair<QString, QPair<int, qint64>>> folderList;
    for (auto fit = byFolder.constBegin(); fit != byFolder.constEnd(); ++fit) folderList.append({fit.key(), fit.value()});
    std::sort(folderList.begin(), folderList.end(), [](const auto &a, const auto &b){ return a.second.second > b.second.second; });
    QJsonArray folderArr;
    for (const auto &f : folderList) {
        QJsonObject fo;
        fo["folder"] = f.first;
        fo["count"] = f.second.first;
        fo["size_bytes"] = f.second.second;
        fo["size_human"] = fmtBytes(f.second.second);
        folderArr.append(fo);
    }
    root["by_folder"] = folderArr;

    // Top files (가장 큰 50)
    QJsonArray topArr;
    for (const auto &tf : topFiles) {
        QJsonObject to;
        to["path"] = tf.second;
        to["size_bytes"] = tf.first;
        to["size_human"] = fmtBytes(tf.first);
        topArr.append(to);
    }
    root["top_files"] = topArr;

    // JSON 저장
    QString jsonPath = dir + "/__CHERNOBYL_MANIFEST__.json";
    QFile jf(jsonPath);
    if (jf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        jf.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
        jf.close();
    }

    // 3) TXT manifest (사람이 읽기 좋게)
    QString txtPath = dir + "/__CHERNOBYL_MANIFEST__.txt";
    QFile tf(txtPath);
    if (tf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QTextStream out(&tf);
        out.setEncoding(QStringConverter::Utf8);
        out << "═══════════════════════════════════════════════════════════════\n";
        out << "  📦 ABIWA Chernobyl — " << platform.toUpper() << " 다운로드 manifest\n";
        out << "═══════════════════════════════════════════════════════════════\n";
        out << "생성: " << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n";
        out << "위치: " << dir << "\n";
        out << "\n";
        out << QString("전체 파일: %1 개  ·  총 사이즈: %2\n")
            .arg(QLocale::system().toString(totalCount))
            .arg(fmtBytes(totalBytes));
        out << "\n";

        out << "─── 확장자별 ─────────────────────────────────────────────\n";
        for (const auto &e : extList) {
            double pct = totalBytes > 0 ? double(e.second.second) * 100.0 / double(totalBytes) : 0.0;
            out << QString("  .%1   %2 개   %3   (%4%)\n")
                .arg(e.first, -10)
                .arg(QLocale::system().toString(e.second.first), -8)
                .arg(fmtBytes(e.second.second), -12)
                .arg(QString::number(pct, 'f', 1));
        }
        out << "\n";

        out << "─── 폴더별 (사이즈 큰 순) ────────────────────────────────\n";
        int folderShow = qMin(20, folderList.size());
        for (int i = 0; i < folderShow; ++i) {
            const auto &f = folderList[i];
            out << QString("  %1/   %2 파일   %3\n")
                .arg(f.first, -40)
                .arg(QLocale::system().toString(f.second.first), -8)
                .arg(fmtBytes(f.second.second));
        }
        if (folderList.size() > folderShow) {
            out << QString("  ... 외 %1 개 폴더\n").arg(folderList.size() - folderShow);
        }
        out << "\n";

        out << "─── 가장 큰 파일 Top 20 ─────────────────────────────────\n";
        int topShow = qMin(20, topFiles.size());
        for (int i = 0; i < topShow; ++i) {
            out << QString("  %1. %2  (%3)\n")
                .arg(i+1, 2)
                .arg(topFiles[i].second.left(70))
                .arg(fmtBytes(topFiles[i].first));
        }
        out << "\n";

        out << "═══════════════════════════════════════════════════════════════\n";
        out << "💡 무결성 확인: 파일 개수 / 사이즈 / 확장자별 카운트가 다음 백업과 일치해야 OK\n";
        out << "💡 상세 정보: __CHERNOBYL_MANIFEST__.json 에 모든 통계 + Top 50 파일 list\n";
        out << "═══════════════════════════════════════════════════════════════\n";
        tf.close();
    }

    log(QString("📋 [%1] manifest 생성 — %2 파일 (%3) · 확장자 %4종")
        .arg(platform).arg(totalCount).arg(fmtBytes(totalBytes)).arg(extList.size()),
        "info", platform);
}

void MiyoBackend::logCollectionOptions(const QJsonObject &config, const QString &platform)
{
    log(QString("━━━ [%1] 사용자 선택 옵션 ━━━").arg(platform.toUpper()), "info", platform);
    // 1) Boolean 옵션 (체크박스)
    QStringList onOpts, offOpts;
    for (auto it = config.constBegin(); it != config.constEnd(); ++it) {
        if (it.value().isBool()) {
            if (it.value().toBool()) onOpts << it.key();
            else offOpts << it.key();
        }
    }
    if (!onOpts.isEmpty()) {
        std::sort(onOpts.begin(), onOpts.end());
        log(QString("  ☑ ON  : %1").arg(onOpts.join(", ")), "info", platform);
    }
    if (!offOpts.isEmpty()) {
        std::sort(offOpts.begin(), offOpts.end());
        log(QString("  ☐ OFF : %1").arg(offOpts.join(", ")), "info", platform);
    }
    // 2) String / Number 입력값 (sessionId / token 류는 마스킹)
    QStringList kvs;
    const QStringList sensitiveKeys = {"sessionId","auth_token","ct0","cookie","captureCookie","extraCookie",
                                        "token","apiKey","password","webdavPass","pixivExtraCookie"};
    for (auto it = config.constBegin(); it != config.constEnd(); ++it) {
        if (it.value().isBool()) continue;
        if (it.value().isObject() || it.value().isArray()) continue;
        QString k = it.key();
        QString v;
        if (it.value().isString()) v = it.value().toString();
        else if (it.value().isDouble()) v = QString::number(it.value().toDouble());
        if (v.isEmpty()) continue;
        // 마스킹
        if (sensitiveKeys.contains(k) || k.contains("token", Qt::CaseInsensitive)
            || k.contains("password", Qt::CaseInsensitive) || k.contains("secret", Qt::CaseInsensitive)) {
            if (v.length() > 8) v = v.left(4) + "...***";
            else v = "***";
        }
        // 너무 긴 값은 자름
        if (v.length() > 80) v = v.left(77) + "...";
        kvs << QString("%1=%2").arg(k, v);
    }
    if (!kvs.isEmpty()) {
        std::sort(kvs.begin(), kvs.end());
        log(QString("  📋 입력 : %1").arg(kvs.join(" · ")), "info", platform);
    }
    // 3) 계정 개수 (마스킹된 형태)
    QJsonArray accs = config["accounts"].toArray();
    if (!accs.isEmpty()) {
        QStringList accNames;
        for (const auto &v : accs) {
            QJsonObject a = v.toObject();
            QString n = a["name"].toString();
            if (n.isEmpty()) n = "(이름 없음)";
            accNames << n;
        }
        log(QString("  👤 계정 (%1개): %2").arg(accs.size()).arg(accNames.join(", ")), "info", platform);
    }
    log("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━", "info", platform);
}

// ═════════════════════════════════════════════════════════════════════════
// stopBackup — 사용자가 GUI 버튼 누름. 모든 백업 워커 즉시 종료 + 활성 cp kill.
//   ★ backupNow (수동) + 자동 enqueue 백업 (다운로드 직후) 둘 다 중지
// ═════════════════════════════════════════════════════════════════════════
// exportConfig / importConfig — 사용자 입력 정보 (accounts/tokens/paths/forms) JSON
//   파일 1개에 모든 설정 백업. 다른 PC 로 옮길 때 / 재설치 시 복원.
// ═════════════════════════════════════════════════════════════════════════
void MiyoBackend::exportConfig()
{
    QString defaultName = QString("chernobyl_config_%1.json")
        .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
    QString path = QFileDialog::getSaveFileName(m_window,
        "💾 사용자 정보 내보내기 — JSON 파일 저장",
        QDir::homePath() + "/" + defaultName,
        "JSON files (*.json);;All files (*)");
    if (path.isEmpty()) return;
    if (!path.endsWith(".json", Qt::CaseInsensitive)) path += ".json";

    // m_config 의 모든 정보 toJson — accounts, paths, formData, webdav 등 다 포함
    QJsonObject root = m_config->toJson();
    // 메타 추가 — version + 생성 시각 + app
    QJsonObject meta;
    meta["app"] = "Chernobyl";
    meta["version"] = QCoreApplication::applicationVersion().isEmpty() ? "1.0" : QCoreApplication::applicationVersion();
    meta["exported_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    meta["exported_unix"] = QDateTime::currentSecsSinceEpoch();
    meta["platform"] = QSysInfo::prettyProductName();
    root["__meta__"] = meta;

    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        log(QString("❌ 내보내기 실패 — 파일 열기 실패: %1").arg(path), "error", "settings");
        runJs(QString("alert('❌ 파일 열기 실패:\\n%1');").arg(Common::jsStringLiteral(path).mid(1).chopped(1)));
        return;
    }
    QByteArray bytes = QJsonDocument(root).toJson(QJsonDocument::Indented);
    f.write(bytes);
    f.close();

    QString sizeStr = bytes.size() < 1024 ? QString::number(bytes.size()) + "B"
                    : QString::number(bytes.size()/1024.0, 'f', 1) + "KB";
    log(QString("💾 내보내기 완료: %1 (%2)").arg(path, sizeStr), "success", "settings");
    runJs(QString("alert('💾 사용자 정보 내보내기 완료\\n\\n경로: %1\\n사이즈: %2\\n\\n⚠ 이 파일에 계정 토큰/비밀번호 포함 — 안전한 곳에 보관하세요.');")
        .arg(Common::jsStringLiteral(path).mid(1).chopped(1)).arg(sizeStr));
}

void MiyoBackend::importConfig()
{
    QString path = QFileDialog::getOpenFileName(m_window,
        "📂 사용자 정보 불러오기 — JSON 파일 선택",
        QDir::homePath(),
        "JSON files (*.json);;All files (*)");
    if (path.isEmpty()) return;

    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        log(QString("❌ 파일 열기 실패: %1").arg(path), "error", "settings");
        return;
    }
    QByteArray bytes = f.readAll();
    f.close();

    QJsonParseError err;
    QJsonDocument doc = QJsonDocument::fromJson(bytes, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        log(QString("❌ JSON 파싱 실패: %1").arg(err.errorString()), "error", "settings");
        runJs(QString("alert('❌ JSON 파싱 실패:\\n%1\\n\\n올바른 Chernobyl config 파일이 아닙니다.');").arg(err.errorString()));
        return;
    }
    QJsonObject root = doc.object();
    // 메타 검증 (있으면)
    QString fromApp = root["__meta__"].toObject()["app"].toString();
    QString fromVer = root["__meta__"].toObject()["version"].toString();
    QString fromTime = root["__meta__"].toObject()["exported_at"].toString();
    if (!fromApp.isEmpty() && fromApp != "Chernobyl") {
        log(QString("⚠ 다른 앱의 config (%1) — 그래도 시도").arg(fromApp), "warning", "settings");
    }

    // 확인 dialog — 덮어쓰기 경고
    QString msg = QString("📂 사용자 정보 불러오기\\n\\n파일: %1\\n")
        .arg(Common::jsStringLiteral(path).mid(1).chopped(1));
    if (!fromTime.isEmpty()) msg += QString("내보낸 시각: %1\\n").arg(fromTime);
    if (!fromVer.isEmpty()) msg += QString("버전: %1\\n").arg(fromVer);
    msg += "\\n⚠ 현재 설정 (계정/토큰/경로/form) 모두 덮어씌워집니다.\\n진행하시겠습니까?";
    // Qt native dialog 못 띄우니까 alert + confirm 흉내 (사실 alert만)
    // → 그냥 곧장 진행, GUI 로그에 명시
    log(QString("📂 불러오기 시작: %1").arg(path), "info", "settings");

    // m_config->fromJson 호출 — Config 가 알아서 모든 필드 복원
    m_config->fromJson(root);
    m_config->save();

    // UI 갱신 — formData / accounts / paths 모두 다시 load
    runJs("if(window.loadConfig) loadConfig(); if(window.loadFormData) loadFormData();");
    // 백엔드에서 직접 호출 (JS 의 loadConfig 가 backend.loadConfig 다시 부르는 케이스 대비)
    loadConfig();
    loadFormData();

    log(QString("✅ 불러오기 완료 — 모든 사용자 정보 복원됨"), "success", "settings");
    runJs(QString("alert('✅ 사용자 정보 불러오기 완료\\n\\n%1\\n%2개 필드 복원됨\\n\\n탭별로 확인해주세요.');")
        .arg(fromTime.isEmpty() ? "" : ("내보낸 시각: " + fromTime + "\\n"))
        .arg(root.size()));
}

void MiyoBackend::stopBackup()
{
    bool wasManualActive = m_backupTerminalActive.load();
    bool wasAutoActive = m_backupRunning.load();
    if (!wasManualActive && !wasAutoActive) {
        log("⚠ 진행 중인 백업 없음", "warning", "settings");
        return;
    }
    log("🛑 백업 중지 요청 — 활성 cp 강제 종료 중...", "warning", "settings");

    // 두 모드 모두 중지 플래그 false
    m_backupTerminalActive = false;
    m_backupRunning = false;

    // 진행 중인 ditto/cp 자식 프로세스 강제 종료 (큰 파일 cp 중에도 즉시 끊김)
    //   이 앱 PID 의 자식 프로세스 중 ditto/cp 만 SIGTERM → 2초 후 안 죽으면 SIGKILL
#ifdef Q_OS_MACOS
    QString pidStr = QString::number(QCoreApplication::applicationPid());
    QProcess::execute("/usr/bin/pkill", {"-TERM", "-P", pidStr, "ditto"});
    QProcess::execute("/usr/bin/pkill", {"-TERM", "-P", pidStr, "cp"});
    QProcess::execute("/usr/bin/pkill", {"-TERM", "-P", pidStr, "rsync"});
    QThread::msleep(1500);
    QProcess::execute("/usr/bin/pkill", {"-KILL", "-P", pidStr, "ditto"});
    QProcess::execute("/usr/bin/pkill", {"-KILL", "-P", pidStr, "cp"});
    QProcess::execute("/usr/bin/pkill", {"-KILL", "-P", pidStr, "rsync"});
#endif

    // 자동 enqueue 큐도 비우기 (남은 항목 모두 폐기)
    {
        QMutexLocker lock(&m_backupQueueMutex);
        initBackupQueuePaths();
        QFile::remove(m_backupQueuePath);
        QFile::remove(m_backupOffsetPath);
        m_backupQueueOffset = 0;
        m_backupTotalBytes = 0;
        m_backupDoneBytes = 0;
        m_backupTotalCount = 0;
        m_backupDoneCount = 0;
    }

    // 터미널에도 표시
    if (m_terminalLogPaths.contains("backup")) {
        QMutexLocker tlock(&m_backupTerminalMutex);
        writeTerminalLog("", "backup");
        writeTerminalLog("\033[1;31m🛑 사용자 중지 요청 — 백업 종료 + 자동 큐 초기화\033[0m", "backup");
    }
    log(QString("✅ 백업 중지 완료 (수동: %1, 자동: %2)")
        .arg(wasManualActive ? "중단" : "없었음")
        .arg(wasAutoActive ? "중단 + 큐 비움" : "없었음"),
        "success", "settings");
}

void MiyoBackend::enqueueIntegrityCheck(const QString &localPath, const QString &platform)
{
    // platform 명시 안 됐으면 ("auto"), 활성화된 platform 1개라도 있으면 검사
    if (platform == "auto" && m_integrityActivePlatforms.isEmpty()) return;
    // 명시 platform 이지만 활성화 안 되어 있으면 skip
    if (platform != "auto" && !m_integrityActivePlatforms.contains(platform)) return;
    if (!QFile::exists(localPath)) return;
    {
        QMutexLocker lock(&m_integrityQueueMutex);
        m_integrityQueue.append({localPath, platform});
    }
    if (!m_integrityRunning.load()) {
        m_integrityRunning = true;
        m_integrityThread = QThread::create([this]() { integrityWorker(); });
        connect(m_integrityThread, &QThread::finished, m_integrityThread, &QThread::deleteLater);
        m_integrityThread->start();
    }
}

void MiyoBackend::setIntegrityActiveForPlatform(const QString &platform, bool enabled)
{
    if (enabled) m_integrityActivePlatforms.insert(platform);
    else m_integrityActivePlatforms.remove(platform);
}

void MiyoBackend::integrityWorker()
{
    while (m_integrityRunning.load()) {
        IntegrityItem it;
        {
            QMutexLocker lock(&m_integrityQueueMutex);
            if (m_integrityQueue.isEmpty()) {
                lock.unlock();
                QThread::msleep(500);
                continue;
            }
            it = m_integrityQueue.takeFirst();
        }
        QString error = Common::checkFileIntegrity(it.path);
        QString file = QFileInfo(it.path).fileName();
        QString plat = it.platform;
        QMetaObject::invokeMethod(this, [this, error, file, plat, p = it.path]() {
            if (error.isEmpty()) {
                log(QString("✅ 무결성 OK: %1").arg(file), "info", plat);
            } else {
                log(QString("⚠ 무결성 실패: %1 — %2\n   파일: %3").arg(file, error, p),
                    "warning", plat);
            }
        }, Qt::QueuedConnection);
    }
}

// ═════════════════════════════════════════════════════════════════════════
// 디스크 큐 — 사용자 임시 디스크에 append-only 텍스트 파일.
//   10만개+ 파일 백업해도 메모리 거의 안 먹음. 한 줄 = 하나 경로.
//   offset 파일로 다음 읽을 위치 추적 → 워커 stream 처리.
// ═════════════════════════════════════════════════════════════════════════
void MiyoBackend::initBackupQueuePaths()
{
    if (!m_backupQueuePath.isEmpty()) return;
    QString base = Common::resolveTempBase(m_config ? m_config->tempDir() : QString());
    QDir().mkpath(base);
    m_backupQueuePath = base + "/abiwa_backup_queue.txt";
    m_backupOffsetPath = base + "/abiwa_backup_queue.offset";
    // offset file 복원 (앱 재시작해도 큐 이어서)
    QFile of(m_backupOffsetPath);
    if (of.open(QIODevice::ReadOnly)) {
        QByteArray data = of.readAll();
        m_backupQueueOffset = data.trimmed().toLongLong();
        of.close();
    } else {
        m_backupQueueOffset = 0;
    }
}

void MiyoBackend::enqueueBackupItem(const QString &localPath, qint64 size)
{
    // 호출자가 m_backupQueueMutex 잡고 있어야 함
    initBackupQueuePaths();
    QFile f(m_backupQueuePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Append)) {
        log("⚠ 백업 큐 파일 open 실패: " + m_backupQueuePath, "warning", "settings");
        return;
    }
    f.write(localPath.toUtf8());
    f.write("\n");
    f.close();
    m_backupTotalBytes.fetch_add(size);
    m_backupTotalCount.fetch_add(1);
}

bool MiyoBackend::dequeueBackupItem(QString &outPath)
{
    // 호출자가 m_backupQueueMutex 잡고 있어야 함
    initBackupQueuePaths();
    QFile f(m_backupQueuePath);
    if (!f.exists() || f.size() == 0) return false;
    if (!f.open(QIODevice::ReadOnly)) return false;
    if (!f.seek(m_backupQueueOffset)) { f.close(); return false; }
    QByteArray line = f.readLine();
    qint64 newOffset = f.pos();
    f.close();
    if (line.isEmpty()) return false;
    // offset 저장 — 워커 크래시/재시작해도 이어서
    QFile of(m_backupOffsetPath);
    if (of.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        of.write(QByteArray::number(newOffset));
        of.close();
    }
    m_backupQueueOffset = newOffset;
    QByteArray trimmed = line;
    while (!trimmed.isEmpty() && (trimmed.endsWith('\n') || trimmed.endsWith('\r'))) trimmed.chop(1);
    outPath = QString::fromUtf8(trimmed);
    return !outPath.isEmpty();
}

void MiyoBackend::resetBackupQueueIfDrained()
{
    // 호출자가 m_backupQueueMutex 잡고 있어야 함
    initBackupQueuePaths();
    QFile f(m_backupQueuePath);
    if (!f.exists()) return;
    if (m_backupQueueOffset >= f.size() && f.size() > 0) {
        // 모두 처리 — 파일 비우기 + offset 초기화 (다음 enqueue 가 다시 시작)
        QFile::remove(m_backupQueuePath);
        QFile::remove(m_backupOffsetPath);
        m_backupQueueOffset = 0;
        m_backupDoneBytes = 0;
        m_backupDoneCount = 0;
        m_backupTotalBytes = 0;
        m_backupTotalCount = 0;
    }
}

void MiyoBackend::enqueueBackup(const QString &localPath)
{
    // ★ 3중 안전망 — 사용자가 명시적으로 백업 켜야만 작동:
    //   1) config 의 backupEnabled = true 여야 함 (체크박스 ON 한 상태)
    //   2) backup 경로 설정되어 있어야 함
    //   3) m_backupRunning = true 여야 함 (워커 이미 시작됨 — setBackupConfig 가 시작)
    //   3번 = "사용자가 명시적으로 토글 ON 누른 적이 있다" 의미 — 자동 시작 안 함
    if (!m_config->backupEnabled()) return;
    if (m_config->backupPath().isEmpty()) return;
    if (!m_backupRunning.load()) return;  // ★ 워커 안 떠있으면 enqueue 자체 거부
    if (!QFile::exists(localPath)) return;
    qint64 sz = QFileInfo(localPath).size();
    {
        QMutexLocker lock(&m_backupQueueMutex);
        enqueueBackupItem(localPath, sz);
    }
    emitBackupProgress();
}

void MiyoBackend::recalcBackupTotals()
{
    // 큐 파일 한 번 스캔 — 남은 파일 각각의 size 합산 + done 더해서 total
    QMutexLocker lock(&m_backupQueueMutex);
    initBackupQueuePaths();
    QFile f(m_backupQueuePath);
    qint64 total = 0;
    int count = 0;
    if (f.exists() && f.open(QIODevice::ReadOnly)) {
        f.seek(m_backupQueueOffset);
        while (!f.atEnd()) {
            QByteArray line = f.readLine();
            while (!line.isEmpty() && (line.endsWith('\n') || line.endsWith('\r'))) line.chop(1);
            if (line.isEmpty()) continue;
            QString p = QString::fromUtf8(line);
            if (QFile::exists(p)) {
                total += QFileInfo(p).size();
                count++;
            }
        }
        f.close();
    }
    m_backupTotalBytes = total + m_backupDoneBytes.load();
    m_backupTotalCount = count + m_backupDoneCount.load();
}

void MiyoBackend::emitBackupProgress()
{
    // 200ms throttle — 너무 자주 UI 갱신 안 함
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - m_backupLastProgressMs.load() < 200) return;
    m_backupLastProgressMs = now;
    qint64 total = m_backupTotalBytes.load();
    qint64 done = m_backupDoneBytes.load();
    int doneN = m_backupDoneCount.load();
    int totalN = m_backupTotalCount.load();
    int pct = total > 0 ? int(done * 100 / total) : 0;
    QString js = QString("if(window.onBackupProgress) onBackupProgress(%1,%2,%3,%4,%5);")
        .arg(pct).arg(done).arg(total).arg(doneN).arg(totalN);
    runJs(js);
}

void MiyoBackend::backupWorker()
{
    while (m_backupRunning.load()) {
        QString item;
        bool got = false;
        {
            QMutexLocker lock(&m_backupQueueMutex);
            got = dequeueBackupItem(item);
            if (!got) {
                // 큐 비면 fully drained 체크 (counts/offset 리셋) 후 sleep
                resetBackupQueueIfDrained();
            }
        }
        if (!got) {
            QThread::msleep(1000);
            continue;
        }
        // ★ worker 는 큐만 처리 — enabled 토글은 enqueueBackup (자동 enqueue) 에서 결정.
        //   "지금 백업" 같은 1회성은 enabled off 여도 큐에 들어와있으면 처리해줘야 함.
        QString backupRoot = m_config->backupPath();
        if (backupRoot.isEmpty()) continue;
        if (!QFile::exists(item)) continue;

        // ★ 상대 경로 보존 — 여러 base 후보 시도, 가장 긴 prefix 매치 사용
        //   1) tempDir, 2) 각 플랫폼 path, 3) home, 4) fallback = 파일명
        QStringList candidateBases;
        if (!m_config->tempDir().isEmpty()) candidateBases << m_config->tempDir();
        // 각 플랫폼 path 도 base 후보 (사용자가 직접 입력한 다운로드 폴더)
        QJsonObject form = m_config->formData();
        const QStringList platKeys = {"twitter-path","bsky-path","youtube-path","discord-path",
                                       "instagram-path","pixiv-path","fanbox-path","tumblr-path","spinspin-path",
                                       "asked-path","crawl-path","trad-path","naikakukai-path"};
        for (const QString &k : platKeys) {
            QString p = form[k].toString();
            if (p.isEmpty()) continue;
            p.replace("~", QDir::homePath());
            // backup 대상 자체가 NAS 면 그 path 가 backupRoot 일 수도 — skip
            if (!p.isEmpty() && p != backupRoot) candidateBases << p;
        }
        candidateBases << QDir::homePath();

        // 가장 긴 prefix 매치
        QString bestBase;
        for (const QString &base : candidateBases) {
            if (item.startsWith(base) && base.length() > bestBase.length()) {
                bestBase = base;
            }
        }
        QString relPath;
        if (!bestBase.isEmpty()) {
            relPath = item.mid(bestBase.length());
            if (relPath.startsWith("/")) relPath = relPath.mid(1);
            // base 자체 이름이 generic ("Downloads", "Documents" 등) 이면 base 마지막 segment 도 prefix 로 붙임
            QString baseLast = QFileInfo(bestBase).fileName();
            if (!baseLast.isEmpty() && bestBase != QDir::homePath() && relPath.indexOf('/') > 0) {
                // 이미 폴더 구조 있음 — base 이름은 안 붙여도 됨
            }
        } else {
            relPath = QFileInfo(item).fileName();
        }
        QString dst = backupRoot + "/" + relPath;
        QDir().mkpath(QFileInfo(dst).absolutePath());

        qint64 itemSize = QFileInfo(item).size();
        // 사이즈/속도/ETA 포맷 helper
        auto fmtBytes = [](qint64 b) {
            if (b < 1024) return QString::number(b) + "B";
            if (b < 1024LL*1024) return QString::number(b/1024.0, 'f', 1) + "KB";
            if (b < 1024LL*1024*1024) return QString::number(b/1024.0/1024, 'f', 1) + "MB";
            return QString::number(b/1024.0/1024/1024, 'f', 2) + "GB";
        };
        auto fmtTime = [](qint64 secs) {
            if (secs < 0 || secs > 99999) return QString("--");
            if (secs < 60) return QString("%1초").arg(secs);
            if (secs < 3600) return QString("%1분 %2초").arg(secs/60).arg(secs%60);
            return QString("%1시간 %2분").arg(secs/3600).arg((secs%3600)/60);
        };
        // skip / dup 처리 결과를 terminal 에 기록하는 helper
        auto writeTerminalProgress = [&](const QString &status, const QString &symbol, const QString &extraInfo = QString()) {
            if (!m_backupTerminalActive.load()) return;
            QMutexLocker tlock(&m_backupTerminalMutex);
            int doneN  = m_backupDoneCount.load();
            int totalN = m_backupTotalCount.load();
            qint64 done = m_backupDoneBytes.load();
            qint64 total = m_backupTotalBytes.load();
            int pct = total > 0 ? int(done * 100 / total) : 0;
            qint64 startMs = m_backupStartMs.load();
            qint64 elapsed = startMs > 0 ? (QDateTime::currentMSecsSinceEpoch() - startMs) / 1000 : 0;
            qint64 speed = (elapsed > 0) ? (done / elapsed) : 0;
            qint64 remain = total - done;
            qint64 eta = (speed > 0 && remain > 0) ? (remain / speed) : -1;
            QString fname = QFileInfo(item).fileName();
            QString line = QString("[%1/%2 = %3%] %4 %5 (%6)")
                .arg(doneN, 5).arg(totalN, -5).arg(pct, 3)
                .arg(symbol, fname, fmtBytes(itemSize));
            if (!extraInfo.isEmpty()) line += " · " + extraInfo;
            writeTerminalLog(line, "backup");
            // 매 10개마다 요약 라인 추가
            if ((doneN % 10) == 0 || doneN == totalN) {
                QString summary = QString("    ━━ 처리 %1/%2 · 진행 %3 / %4 · 속도 %5/s · 경과 %6 · ETA %7 ━━")
                    .arg(fmtBytes(done), fmtBytes(total),
                         fmtBytes(speed),
                         fmtTime(elapsed),
                         fmtTime(eta))
                    .arg(""); // placeholder
                // 위 인자 순서 맞춤 — 다시 정리
                summary = QString("    ━━ 진행: %1 / %2 · 속도: %3/s · 경과: %4 · ETA: %5 · skip: %6, fail: %7 ━━")
                    .arg(fmtBytes(done), fmtBytes(total), fmtBytes(speed),
                         fmtTime(elapsed), fmtTime(eta))
                    .arg(m_backupSkipCount.load()).arg(m_backupFailCount.load());
                writeTerminalLog(summary, "backup");
            }
        };

        // src == dst 면 skip
        if (item == dst) {
            m_backupDoneBytes.fetch_add(itemSize);
            m_backupDoneCount.fetch_add(1);
            m_backupSkipCount.fetch_add(1);
            writeTerminalProgress("self", "⏭ ", "src=dst skip");
            continue;
        }
        // 같은 크기면 skip (idempotent)
        if (QFile::exists(dst) && QFileInfo(dst).size() == itemSize) {
            m_backupDoneBytes.fetch_add(itemSize);
            m_backupDoneCount.fetch_add(1);
            m_backupSkipCount.fetch_add(1);
            emitBackupProgress();
            writeTerminalProgress("dup", "⏭ ", "이미 백업됨 skip");
            continue;
        }
        QFile::remove(dst);
        // ★ macOS 의 ditto 사용 — Qt::copy 보다 빠름 (메타데이터 + xattr 보존, mmap)
        //   fallback 으로 cp -p, 그 다음 QFile::copy
        bool ok = false;
        QString method;
        QElapsedTimer copyTimer;
        copyTimer.start();
#ifdef Q_OS_MACOS
        if (QProcess::execute("/usr/bin/ditto", {item, dst}) == 0) { ok = true; method = "ditto"; }
#endif
        if (!ok) {
            QDir().mkpath(QFileInfo(dst).absolutePath());
#ifdef Q_OS_MACOS
            if (QProcess::execute("/bin/cp", {"-p", item, dst}) == 0) { ok = true; method = "cp"; }
#endif
        }
        if (!ok) {
            QDir().mkpath(QFileInfo(dst).absolutePath());
            ok = QFile::copy(item, dst);
            if (ok) method = "QFile::copy";
        }
        qint64 copyMs = copyTimer.elapsed();
        QString errReason;
        if (ok) {
            m_backupDoneBytes.fetch_add(itemSize);
            m_backupDoneCount.fetch_add(1);
            emitBackupProgress();
            // 속도 = itemSize / copyMs
            qint64 perSec = (copyMs > 0) ? (itemSize * 1000 / copyMs) : 0;
            QString extra = QString("→ %1 · %2 (%3/s)").arg(QFileInfo(dst).absolutePath(), method, fmtBytes(perSec));
            writeTerminalProgress("ok", "✅", extra);
        } else {
            m_backupFailCount.fetch_add(1);
            if (!QFile::exists(item)) errReason = "src 없음";
            else if (!QDir(QFileInfo(dst).absolutePath()).exists()) errReason = "dst 디렉토리 생성 실패 (NAS 마운트 끊김?)";
            else if (!QFileInfo(QFileInfo(dst).absolutePath()).isWritable()) errReason = "dst 권한 없음";
            else errReason = "QFile::copy 실패 (NAS 응답 거부)";
            writeTerminalProgress("fail", "❌", "실패: " + errReason);
        }
        // GUI 로그는 성공/실패 한 줄 (지나치게 verbose 안 됨)
        QMetaObject::invokeMethod(this, [this, ok, item, dst, errReason]() {
            if (ok) {
                log(QString("📦 백업: %1 → %2").arg(QFileInfo(item).fileName(), dst), "success", "settings");
            } else {
                log(QString("✗ 백업 실패: %1\n   src: %2\n   dst: %3\n   원인: %4")
                    .arg(QFileInfo(item).fileName(), item, dst, errReason), "warning", "settings");
            }
        }, Qt::QueuedConnection);

        // ★ 큐 끝까지 처리됐으면 — 터미널 마무리 (마지막 worker 가 처리)
        bool drained = false;
        {
            QMutexLocker lock(&m_backupQueueMutex);
            QFile qf(m_backupQueuePath);
            if (!qf.exists() || m_backupQueueOffset >= qf.size()) drained = true;
        }
        if (drained && m_backupTerminalActive.load()) {
            // 큐 빈 상태에서 잠깐 대기 — 다른 worker 가 마지막 파일 쓰는 중일 수도
            QThread::msleep(500);
            QMutexLocker lock(&m_backupQueueMutex);
            QFile qf(m_backupQueuePath);
            if (!qf.exists() || m_backupQueueOffset >= qf.size()) {
                // 완료 배너
                QMutexLocker tlock(&m_backupTerminalMutex);
                qint64 startMs = m_backupStartMs.load();
                qint64 elapsed = startMs > 0 ? (QDateTime::currentMSecsSinceEpoch() - startMs) / 1000 : 0;
                writeTerminalLog("", "backup");
                writeTerminalLog("==========================================================", "backup");
                writeTerminalLog(QString("  ✅ 백업 완료 — 총 %1 파일 (%2)")
                    .arg(m_backupDoneCount.load()).arg(fmtBytes(m_backupDoneBytes.load())), "backup");
                writeTerminalLog(QString("  ⏭  skip (이미 백업됨): %1 개")
                    .arg(m_backupSkipCount.load()), "backup");
                if (m_backupFailCount.load() > 0)
                    writeTerminalLog(QString("  ❌ 실패: %1 개").arg(m_backupFailCount.load()), "backup");
                writeTerminalLog(QString("  ⏱  소요 시간: %1").arg(fmtTime(elapsed)), "backup");
                writeTerminalLog(QString("  📂 백업 위치: %1").arg(m_config->backupPath()), "backup");
                writeTerminalLog("==========================================================", "backup");
                writeTerminalLog("[DONE]", "backup");  // tail script 종료 시그널
                m_backupTerminalActive = false;
                m_backupSkipCount = 0;
                m_backupFailCount = 0;
                QMetaObject::invokeMethod(this, [this]() { closeTerminalLog("backup"); }, Qt::QueuedConnection);
            }
        }
    }
}

// ═════════════════════════════════════════════════════════════════════════
// 이메일 IMAP 감시 — 새 메일 매치 시 内閣会 즉시 실행
// ═════════════════════════════════════════════════════════════════════════
void MiyoBackend::startEmailWatch(const QString &server, int port,
                                  const QString &user, const QString &pass,
                                  const QString &filterFrom, const QString &filterSubject)
{
    if (server.isEmpty() || user.isEmpty() || pass.isEmpty()) {
        log("이메일 감시: 서버/사용자/비번 필수", "warning", "naikakukai");
        return;
    }
    m_emailServer = server;
    m_emailPort = port > 0 ? port : 993;
    m_emailUser = user;
    m_emailPass = pass;
    m_emailFilterFrom = filterFrom;
    m_emailFilterSubject = filterSubject;
    m_emailLastUid = 0;  // 시작 후 도착하는 메일만 처리

    if (!m_emailWatchTimer) {
        m_emailWatchTimer = new QTimer(this);
        connect(m_emailWatchTimer, &QTimer::timeout, this, &MiyoBackend::emailWatchTick);
    }
    m_emailWatchTimer->setInterval(30000);  // 30초
    m_emailWatchTimer->start();
    log(QString("📧 이메일 감시 시작 — %1:%2 (from=%3, subject=%4)")
        .arg(server).arg(port).arg(filterFrom.isEmpty() ? "*" : filterFrom)
        .arg(filterSubject.isEmpty() ? "*" : filterSubject), "success", "naikakukai");

    // 즉시 1회 baseline (last_uid 초기화 용)
    QTimer::singleShot(500, this, &MiyoBackend::emailWatchTick);
}

void MiyoBackend::stopEmailWatch()
{
    if (m_emailWatchTimer) m_emailWatchTimer->stop();
    log("📧 이메일 감시 중지", "warning", "naikakukai");
}

void MiyoBackend::testEmailWatch()
{
    if (m_emailServer.isEmpty()) {
        log("📧 먼저 시작 버튼으로 설정 저장", "warning", "naikakukai");
        return;
    }
    log("📧 즉시 체크...", "info", "naikakukai");
    emailWatchTick();
}

void MiyoBackend::emailWatchTick()
{
    if (m_emailServer.isEmpty()) return;

    // Python script 경로 — 번들 우선
    QString scriptPath = QCoreApplication::applicationDirPath() + "/../Resources/tools/email_watch.py";
    if (!QFileInfo::exists(scriptPath)) {
        scriptPath = QCoreApplication::applicationDirPath() + "/../../../resources/tools/email_watch.py";
    }
    if (!QFileInfo::exists(scriptPath)) {
        log("email_watch.py 못 찾음", "error", "naikakukai");
        return;
    }

    // Python 인터프리터 — 쓰기가능 복사본(번들 seal 보호) 또는 시스템
    QString python = Common::bundledPythonPath();
    if (!QFileInfo::exists(python)) python = "/usr/bin/python3";

    QString server = m_emailServer;
    int port = m_emailPort;
    QString user = m_emailUser, pass = m_emailPass;
    QString ff = m_emailFilterFrom, fs = m_emailFilterSubject;
    int lastUid = m_emailLastUid;

    QThread *t = QThread::create([this, scriptPath, python, server, port, user, pass, ff, fs, lastUid]() {
        QProcess p;
        QStringList args{scriptPath, server, QString::number(port), user, pass, ff, fs, QString::number(lastUid)};
        p.start(python, args);
        bool ok = p.waitForFinished(20000);
        QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
        QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();

        QMetaObject::invokeMethod(this, [this, ok, out, err]() {
            if (!ok) {
                log(QString("📧 IMAP 타임아웃: %1").arg(err.left(100)), "error", "naikakukai");
                return;
            }
            QJsonDocument doc = QJsonDocument::fromJson(out.toUtf8());
            if (doc.isNull() || !doc.isObject()) {
                log(QString("📧 응답 파싱 실패: %1").arg(out.left(150)), "error", "naikakukai");
                return;
            }
            QJsonObject obj = doc.object();
            if (obj.contains("error")) {
                log(QString("📧 IMAP 에러: %1").arg(obj["error"].toString()), "error", "naikakukai");
                return;
            }
            int lastUid = obj["last_uid"].toInt();
            if (lastUid > m_emailLastUid) m_emailLastUid = lastUid;

            bool found = obj["found"].toBool();
            int count = obj["count"].toInt();
            if (!found) return;

            // 매치된 메일 발견 → 内閣会 즉시 실행
            log(QString("📧 새 메일 매치 %1개 — 内閣会 즉시 실행").arg(count), "success", "naikakukai");
            QJsonArray samples = obj["samples"].toArray();
            for (const auto &v : samples) {
                QJsonObject s = v.toObject();
                log(QString("  ↳ %1 | %2").arg(s["from"].toString().left(50), s["subject"].toString().left(80)),
                    "info", "naikakukai");
            }

            // 内閣会 watches 가 설정되어 있어야 의미 있음
            if (m_naikakukaiWatches.isEmpty()) {
                log("⚠ 内閣会 감시 대상 미설정 — 알림만 받음", "warning", "naikakukai");
                return;
            }
            // 전체 watch 1회 실행 (커서 무시 — 알림 받았으니 전부 체크)
            int saved = m_naikakukaiCursor;
            for (int i = 0; i < m_naikakukaiWatches.size(); ++i) {
                m_naikakukaiCursor = i;
                naikakukaiTick();
            }
            m_naikakukaiCursor = saved;
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

void MiyoBackend::naikakukaiTick()
{
    if (!m_naikakukaiRunning || m_naikakukaiWatches.isEmpty()) return;

    // 이번 tick 에서 처리할 watch 선택 (순환 커서)
    if (m_naikakukaiCursor >= m_naikakukaiWatches.size()) m_naikakukaiCursor = 0;
    QJsonObject watch = m_naikakukaiWatches.at(m_naikakukaiCursor).toObject();
    m_naikakukaiCursor++;

    QString platform = watch["platform"].toString();
    QString target = watch["target"].toString();

    // 이미 해당 플랫폼이 다른 작업 중이면 이번 tick은 건너뜀
    if (m_isRunning.value(platform, false)) {
        log(QString("内閣会: %1 은(는) 현재 수집 중 — 다음 tick으로 연기")
            .arg(platform), "info", "naikakukai");
        return;
    }

    log(QString("内閣会 폴링 → %1 / %2").arg(platform, target), "info", "naikakukai");

    // ── 각 플랫폼 config 구성 ──
    // watch 에는 platform/target/path 밖에 없으므로, 계정 정보와 이전 수집 설정을
    // 여기서 보충해야 실제로 다운로드가 동작한다.
    QJsonObject runConfig = watch;
    runConfig["platform"] = platform;
    if (!runConfig.contains("count")) runConfig["count"] = 10;
    if (!runConfig.contains("type"))  runConfig["type"] = "tweets";  // 기본: 유저 타임라인
    if (!runConfig.contains("mode"))  runConfig["mode"] = "tweets";

    // 1) 계정 주입 — Config 저장소 우선, 없으면 이전 수집 config fallback
    QJsonArray accounts = m_config ? m_config->getAccounts(platform) : QJsonArray();
    if (accounts.isEmpty()) {
        accounts = m_lastConfig.value(platform)["accounts"].toArray();
    }
    if (accounts.isEmpty() && (platform == "twitter" || platform == "bluesky" || platform == "tumblr")) {
        log(QString("内閣会: %1 계정 미등록 → @%2 건너뜀 (설정 탭에서 계정 추가 필요)")
            .arg(platform, target), "error", "naikakukai");
        setPlatformRunning(platform, false);
        return;
    }
    runConfig["accounts"] = accounts;

    // 2) 이전 수집 config의 옵션 보존 (downloadMedia/exif/excel 등) — watch 우선
    QJsonObject lastCfg = m_lastConfig.value(platform);
    if (!lastCfg.isEmpty()) {
        for (auto it = lastCfg.constBegin(); it != lastCfg.constEnd(); ++it) {
            // target/path/accounts 는 watch/위에서 설정한 값 유지
            if (it.key() == "target" || it.key() == "path" ||
                it.key() == "accounts" || it.key() == "platform") continue;
            if (!runConfig.contains(it.key())) runConfig[it.key()] = it.value();
        }
    }
    // 3) 기본값 — 미디어 다운로드는 내각회의 존재 이유
    if (!runConfig.contains("downloadMedia")) runConfig["downloadMedia"] = true;
    if (!runConfig.contains("media"))         runConfig["media"] = true;  // Bluesky/Tumblr key
    if (!runConfig.contains("excel"))         runConfig["excel"] = false; // 자동 수집은 Excel 불필요

    // ★ "알람 오면 다운" 모드 — 새 글 있을 때만 다운 + 시스템 알림
    //   resumeMode=future로 since:newestDate 검색 → 새 글 0개면 collection 자체를 skip
    if (!runConfig.contains("resumeMode")) runConfig["resumeMode"] = "future";

    // 백그라운드 스레드에서 실행
    setPlatformRunning(platform, true);
    QThread *thread = QThread::create([this, runConfig, platform, target]() {
        const QString p = platform;
        // 실행 전 트윗 카운트 비교 위해 m_lastConfig의 newestId 저장
        QString prevNewest;
        if (p == "twitter" && m_twitterCollector) {
            prevNewest = m_twitterCollector->newestTweetId();
        }

        if (p == "twitter") runTwitterCollection(runConfig);
        else if (p == "bluesky") runBlueskyCollection(runConfig);
        else if (p == "tumblr") runTumblrCollection(runConfig);
        setPlatformRunning(p, false);

        // 새 글 발견 여부 — 시스템 알림
        QString newNewest;
        if (p == "twitter" && m_twitterCollector) {
            newNewest = m_twitterCollector->newestTweetId();
        }
        bool foundNew = !newNewest.isEmpty() && newNewest != prevNewest;
        QMetaObject::invokeMethod(this, [this, p, target, foundNew]() {
            if (foundNew) {
                log(QString("📢 内閣会: %1/%2 새 글 발견 — 다운 완료").arg(p, target),
                    "success", "naikakukai");
                showSystemNotification(
                    QString("내각회 — %1").arg(target),
                    QString("@%1 새 글 발견 → 자동 다운로드 완료").arg(target));
            } else {
                log(QString("内閣会: %1/%2 새 글 없음").arg(p, target), "info", "naikakukai");
            }
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

bool MiyoBackend::isAnyRunning() const
{
    QMutexLocker lock(&m_runningMutex);
    for (auto it = m_isRunning.constBegin(); it != m_isRunning.constEnd(); ++it) {
        if (it.value()) return true;
    }
    return false;
}

void MiyoBackend::executeJsMainThread(const QString &js)
{
    if (m_window && m_window->webView()) {
        m_window->webView()->page()->runJavaScript(js);
    }
}

void MiyoBackend::appendLogMainThread(const QString &message, const QString &type, const QString &platform)
{
    if (!m_window || !m_window->webView()) return;
    // 배치 큐에 추가 — flushLogs()에서 모아서 전송
    m_pendingLogs[platform].append({message, type});
}

static QString escapeJsString(const QString &s)
{
    QString out;
    out.reserve(s.size() + 16);
    for (const QChar &c : s) {
        switch (c.unicode()) {
        case '\\': out += QLatin1String("\\\\"); break;
        case '\'': out += QLatin1String("\\'"); break;
        case '"':  out += QLatin1String("\\\""); break;
        case '\n': out += QLatin1Char(' '); break;
        case '\r': break;
        default:   out += c; break;
        }
    }
    return out;
}

void MiyoBackend::flushLogs()
{
    if (!m_window || !m_window->webView()) return;
    if (m_pendingLogs.isEmpty()) return;

    // 모든 플랫폼의 대기 로그를 한 번의 JS 호출로 전송
    QString js;
    js.reserve(4096);
    for (auto it = m_pendingLogs.begin(); it != m_pendingLogs.end(); ++it) {
        const QString &platform = it.key();
        QList<PendingLog> &logs = it.value();
        if (logs.isEmpty()) continue;

        // 최대 50개까지만 (너무 많으면 오래된 것 버림)
        if (logs.size() > 50) {
            int skip = logs.size() - 50;
            logs = logs.mid(skip);
        }

        for (const auto &entry : logs) {
            js += QStringLiteral("appendLog('")
                + escapeJsString(entry.message)
                + QStringLiteral("','") + entry.type
                + QStringLiteral("','") + platform
                + QStringLiteral("');");
        }
    }
    m_pendingLogs.clear();

    if (!js.isEmpty()) {
        m_window->webView()->page()->runJavaScript(js);
    }
}

void MiyoBackend::runJs(const QString &js)
{
    emit jsSignal(js);
}

void MiyoBackend::log(const QString &message, const QString &type, const QString &platform)
{
    QString p = platform.isEmpty() ? m_currentPlatform : platform;
    // JS log box는 platform 단위 (탭 하나에 모든 병렬 로그 표시)
    emit logSignal(message, type, p);
    // 터미널 파일 — platform 명시된 경우만 write (섞임 방지)
    //   m_currentPlatform fallback 은 GUI 로그만, 터미널엔 안 보냄
    if (!platform.isEmpty()) {
        writeTerminalLog(message, p);
    }
}

void MiyoBackend::setThreadTrackKey(const QString &trackKey)
{
    QMutexLocker lock(&m_threadTrackKeyMutex);
    m_threadTrackKey[QThread::currentThreadId()] = trackKey;
}
void MiyoBackend::clearThreadTrackKey()
{
    QMutexLocker lock(&m_threadTrackKeyMutex);
    m_threadTrackKey.remove(QThread::currentThreadId());
}
QString MiyoBackend::currentThreadTrackKey() const
{
    QMutexLocker lock(&m_threadTrackKeyMutex);
    return m_threadTrackKey.value(QThread::currentThreadId());
}

// ═════════════════════════════════════════════════════════════════════════
// openBackupTerminalLog — 백업 전용 애니메이션 터미널.
//   스피너 ⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏ + 컬러 ANSI + clear + tail -n 30 매 150ms refresh.
// ═════════════════════════════════════════════════════════════════════════
void MiyoBackend::openBackupTerminalLog()
{
    QString platform = "backup";
    QString scriptDir = Common::resolveTempBase(m_config ? m_config->tempDir() : QString()) + "/abiwa_" + platform;
    QDir().mkpath(scriptDir);
    QString logPath = scriptDir + "/.miyo_" + platform + "_log.txt";
    m_terminalLogPaths[platform] = logPath;
    m_terminalLogPath = logPath;
    QFile::remove(logPath);
    // 시작 마커
    QFile f(logPath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write("");
        f.close();
    }

#ifdef Q_OS_WIN
    // Windows — 단순 tail (애니메이션 X — PowerShell ANSI 제약)
    QString scriptPath = scriptDir + "/miyo_backup_tail.bat";
    QFile script(scriptPath);
    if (script.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QString content;
        content += "@echo off\r\n";
        content += "chcp 65001 >nul\r\n";
        content += "title ABIWA Backup Monitor\r\n";
        content += ":loop\r\n";
        content += "cls\r\n";
        content += "echo ===============================================\r\n";
        content += "echo   📦 ABIWA 백업 진행 모니터\r\n";
        content += "echo ===============================================\r\n";
        content += "powershell -NoProfile -Command \"Get-Content -Tail 30 '" + QDir::toNativeSeparators(logPath) + "'\"\r\n";
        content += "findstr /C:\"[DONE]\" \"" + QDir::toNativeSeparators(logPath) + "\" >nul 2>&1\r\n";
        content += "if %errorlevel%==0 (\r\n";
        content += "  echo.\r\n";
        content += "  echo 백업 완료 — 터미널을 닫아도 됩니다.\r\n";
        content += "  pause >nul\r\n";
        content += "  exit /b 0\r\n";
        content += ")\r\n";
        content += "timeout /t 1 /nobreak >nul\r\n";
        content += "goto loop\r\n";
        script.write(content.toUtf8());
        script.close();
    }
    QProcess::startDetached("cmd.exe", {"/c", "start", "ABIWA-Backup", QDir::toNativeSeparators(scriptPath)});
#else
    // macOS — 컬러 + 스피너 애니메이션, 150ms refresh
    QString scriptPath = scriptDir + "/miyo_backup_tail.command";
    QFile script(scriptPath);
    if (script.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QString content;
        content += "#!/bin/bash\n";
        content += "LOG='" + logPath + "'\n";
        content += "BOLD='\\033[1m'\n";
        content += "GREEN='\\033[32m'\n";
        content += "YELLOW='\\033[33m'\n";
        content += "CYAN='\\033[36m'\n";
        content += "MAGENTA='\\033[35m'\n";
        content += "GRAY='\\033[90m'\n";
        content += "RESET='\\033[0m'\n";
        content += "SPINNER='⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏'\n";
        content += "sp_i=0\n";
        // 터미널 사이즈 (가능하면)
        // ★ 모니터 종료(Ctrl+C / 창 닫기 / 정상 exit) 시 STOP sentinel 만들기 → 앱 워커가 polling 으로 감지 후 백업 중지
        content += "trap 'touch \"${LOG}.STOP\" 2>/dev/null; exit 0' SIGINT SIGTERM HUP EXIT\n";
        content += "while true; do\n";
        content += "  clear\n";
        content += "  ROWS=$(tput lines 2>/dev/null || echo 40)\n";
        content += "  TAIL_N=$((ROWS - 8))\n";
        content += "  echo -e \"${BOLD}${CYAN}═══════════════════════════════════════════════════════════════${RESET}\"\n";
        content += "  echo -e \"${BOLD}${CYAN}  📦 ABIWA 백업 진행 모니터  ${GRAY}$(date '+%H:%M:%S')${RESET}\"\n";
        content += "  echo -e \"${BOLD}${CYAN}═══════════════════════════════════════════════════════════════${RESET}\"\n";
        content += "  if [ -f \"$LOG\" ]; then\n";
        content += "    tail -n $TAIL_N \"$LOG\"\n";
        content += "  fi\n";
        content += "  if grep -q '\\[DONE\\]' \"$LOG\" 2>/dev/null; then\n";
        content += "    echo \"\"\n";
        content += "    echo -e \"${BOLD}${GREEN}✅ 백업 완료 — 아무 키나 누르면 종료${RESET}\"\n";
        content += "    read -n 1\n";
        content += "    exit 0\n";
        content += "  fi\n";
        content += "  sp=${SPINNER:$sp_i:1}\n";
        content += "  echo \"\"\n";
        content += "  echo -e \"${CYAN}${BOLD}${sp}${RESET} ${MAGENTA}백업 진행 중...${RESET} ${YELLOW}(Ctrl+C 또는 창 닫기 = 백업 중지)${RESET}\"\n";
        content += "  sp_i=$(( (sp_i + 1) % 10 ))\n";
        content += "  sleep 0.15\n";
        content += "done\n";
        script.write(content.toUtf8());
        script.close();
        script.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                              QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                              QFileDevice::ReadOther | QFileDevice::ExeOther);
        QProcess::execute("/bin/chmod", {"+x", scriptPath});
        QProcess::execute("/usr/bin/xattr", {"-c", scriptPath});
    }
    bool opened = QProcess::startDetached("/usr/bin/open", {"-a", "Terminal.app", scriptPath});
    if (!opened) opened = QProcess::startDetached("/usr/bin/open", {scriptPath});
    if (!opened) {
        QString esc = QString(scriptPath).replace("\\", "\\\\").replace("\"", "\\\"");
        QString appleScript = QString(
            "tell application \"Terminal\"\n"
            "  activate\n"
            "  do script \"clear; '%1'\"\n"
            "end tell"
        ).arg(esc);
        QProcess::startDetached("/usr/bin/osascript", {"-e", appleScript});
    }
#endif
}

void MiyoBackend::openTerminalLog(const QString &platform, const QString &savePath)
{
    // ★ .command 는 로컬 temp 에 (NAS/외장 마운트는 POSIX 실행권한 보존 X)
    //   사용자 tempDir 이 NAS 면 .command 실행 실패. 로컬 /tmp 사용.
    QString scriptDir = Common::resolveTempBase(m_config ? m_config->tempDir() : QString()) + "/abiwa_" + platform;
    QDir().mkpath(scriptDir);

    QString logPath = scriptDir + "/.miyo_" + platform + "_log.txt";
    m_terminalLogPaths[platform] = logPath;
    m_terminalLogPath = logPath;  // backward compat
    QFile::remove(logPath);

    // Create the log file
    QFile f(m_terminalLogPath);
    if (!f.open(QIODevice::WriteOnly)) return;
    f.write("=========================================\n");
    f.write(QString("  ABIWA - %1\n").arg(platform.toUpper()).toUtf8());
    f.write("=========================================\n\n");
    f.close();

#ifdef Q_OS_WIN
    // Windows: create a .bat script that tails the log file
    QString scriptPath = scriptDir + "/miyo_" + platform + "_tail.bat";
    QFile script(scriptPath);
    if (script.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QString content;
        content += "@echo off\r\n";
        content += "chcp 65001 >nul\r\n";
        content += "title ABIWA - " + platform.toUpper() + "\r\n";
        content += ":loop\r\n";
        content += "cls\r\n";
        content += "type \"" + QDir::toNativeSeparators(logPath) + "\"\r\n";
        content += "timeout /t 2 /nobreak >nul\r\n";
        content += "findstr /C:\"[DONE]\" \"" + QDir::toNativeSeparators(logPath) + "\" >nul 2>&1\r\n";
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
    QProcess::startDetached("cmd.exe", {"/c", "start", "ABIWA", QDir::toNativeSeparators(scriptPath)});
#else
    // macOS/Linux: 단순 tail -f — kernel inotify/kqueue 사용, CPU 거의 0
    // ★ 옛 애니메이션 (150ms clear+refresh) 은 CPU/메모리 부담 큼 → 사용자 요청으로 단순화
    QString scriptPath = scriptDir + "/miyo_" + platform + "_tail.command";
    QFile script(scriptPath);
    if (script.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QString content;
        content += "#!/bin/bash\n";
        content += "clear\n";
        content += "cat '" + logPath + "'\n";
        content += "tail -f -n +0 '" + logPath + "' &\n";
        content += "TAIL_PID=$!\n";
        content += "# Wait for DONE marker\n";
        content += "while true; do\n";
        content += "  if grep -q '\\[DONE\\]' '" + logPath + "' 2>/dev/null; then\n";
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
        // ★ NAS/외장 fallback — chmod 직접 + quarantine 제거
        QProcess::execute("/bin/chmod", {"+x", scriptPath});
        QProcess::execute("/usr/bin/xattr", {"-d", "com.apple.quarantine", scriptPath});
    }
    // Terminal.app으로 직접 열기 — 여러 방법 시도 (TCC/quarantine/provenance/sandbox 문제 회피)

    // 1) 실행을 막을 수 있는 확장 속성들 모두 제거
    QProcess::execute("xattr", {"-c", scriptPath});  // 모든 xattr 제거가 가장 확실

    // 2) open -a Terminal.app 으로 시도
    bool opened = QProcess::startDetached("/usr/bin/open", {"-a", "Terminal.app", scriptPath});

    // 3) 실패하면 기본 핸들러로 (.command는 Terminal이 기본값)
    if (!opened) {
        opened = QProcess::startDetached("/usr/bin/open", {scriptPath});
    }

    // 4) 그래도 실패하면 osascript 로 Terminal 에 직접 명령
    if (!opened) {
        QString esc = QString(scriptPath).replace("\\", "\\\\").replace("\"", "\\\"");
        QString appleScript = QString(
            "tell application \"Terminal\"\n"
            "  activate\n"
            "  do script \"clear; '%1'; exit\"\n"
            "end tell"
        ).arg(esc);
        opened = QProcess::startDetached("/usr/bin/osascript", {"-e", appleScript});
    }

    if (!opened) {
        qWarning() << "[openTerminalLog] Failed to launch Terminal for" << platform << "script:" << scriptPath;
    }

    // ★ STOP sentinel 워치독 — 사용자가 터미널에서 Ctrl+C / 창 닫기 시 platform 수집 중지
    //   매 500ms 폴링, sentinel 발견 → m_isRunning[platform] = false → collector 자연 종료
    QString stopSentinel = logPath + ".STOP";
    QFile::remove(stopSentinel);  // stale 정리
    QThread *watchdog = QThread::create([this, platform, stopSentinel]() {
        while (m_isRunning.value(platform, false)) {
            if (QFile::exists(stopSentinel)) {
                QFile::remove(stopSentinel);
                QMetaObject::invokeMethod(this, [this, platform]() {
                    log(QString("🛑 터미널 종료 → [%1] 수집 중지").arg(platform), "warning", platform);
                    {
                        QMutexLocker lock(&m_runningMutex);
                        // platform + 모든 trackKey (병렬 모드) 다 중지
                        for (auto it = m_isRunning.begin(); it != m_isRunning.end(); ++it) {
                            if (it.key() == platform || it.key().startsWith(platform + "#")) {
                                it.value() = false;
                            }
                            m_stopRequested[it.key()] = true;
                        }
                    }
                }, Qt::QueuedConnection);
                break;
            }
            QThread::msleep(500);
        }
    });
    connect(watchdog, &QThread::finished, watchdog, &QThread::deleteLater);
    watchdog->start();
#endif
}

void MiyoBackend::writeTerminalLog(const QString &message, const QString &platform)
{
    // 1) 현재 스레드가 trackKey 등록했으면 (병렬 모드) 그 키의 터미널 파일에만 write
    QString trackKey = currentThreadTrackKey();
    if (!trackKey.isEmpty() && m_terminalLogPaths.contains(trackKey)) {
        QFile f(m_terminalLogPaths[trackKey]);
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            f.write((message + "\n").toUtf8());
            f.close();
        }
        return;
    }
    // 2) platform 자체로 매핑된 파일 — platform 명시 안 됐으면 그냥 skip (★ 섞임 방지)
    //   이전엔 m_terminalLogPath 로 fallback → 마지막 열린 터미널이 모든 platform-less 호출 받아서
    //   백업 터미널에 youtube/yt-dlp 등이 섞임. 그 fallback 제거.
    if (platform.isEmpty()) return;
    if (!m_terminalLogPaths.contains(platform)) return;
    QFile f(m_terminalLogPaths[platform]);
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        f.write((message + "\n").toUtf8());
        f.close();
    }
}

void MiyoBackend::closeTerminalLog(const QString &platform)
{
    QString path;
    if (!platform.isEmpty() && m_terminalLogPaths.contains(platform)) {
        path = m_terminalLogPaths[platform];
        m_terminalLogPaths.remove(platform);
    } else {
        path = m_terminalLogPath;
        m_terminalLogPath.clear();
    }
    if (path.isEmpty()) return;
    QFile f(path);
    if (f.open(QIODevice::Append | QIODevice::Text)) {
        f.write("\n[DONE]\n");
        f.close();
    }
}

void MiyoBackend::closeAllTerminalLogs()
{
    // 모든 플랫폼별 터미널 로그에 [DONE] 마커 쓰기
    for (auto it = m_terminalLogPaths.begin(); it != m_terminalLogPaths.end(); ++it) {
        QFile f(it.value());
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            f.write("\n[DONE]\n");
            f.close();
        }
    }
    m_terminalLogPaths.clear();

    // 공통 로그 경로
    if (!m_terminalLogPath.isEmpty()) {
        QFile f(m_terminalLogPath);
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            f.write("\n[DONE]\n");
            f.close();
        }
        m_terminalLogPath.clear();
    }
}

void MiyoBackend::updateStats(int posts, int media, const QString &status, const QString &platform)
{
    QString p = platform.isEmpty() ? m_currentPlatform : platform;
    // ★ 병렬 모드: 워커 스레드가 trackKey 등록했으면 platform 자리를 trackKey로 치환.
    //   collector 코드는 platform="twitter"로 보내지만 JS는 "twitter#0" 같은 키를
    //   _multi.statsByKey로 라우팅한다. trackKey가 platform으로 시작할 때만 치환.
    QString tk = currentThreadTrackKey();
    if (!tk.isEmpty() && (tk == p || tk.startsWith(p + "#"))) {
        p = tk;
    }
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    bool forceUpdate = status.contains("Done") || status.contains("완료") || status.contains("待機") || status.contains("대기");
    if (!forceUpdate && m_lastStatsUpdate.contains(p) && (now - m_lastStatsUpdate[p]) < 1000) {
        return;
    }
    m_lastStatsUpdate[p] = now;
    runJs(QString("updateStats(%1, %2, '%3', '%4')").arg(posts).arg(media).arg(status, p));
}

void MiyoBackend::showLog(const QString &message)
{
    log(message, "warning");
}

void MiyoBackend::loadConfig()
{
    m_config->load();
    QJsonDocument doc(m_config->toJson());
    QString configStr = QString::fromUtf8(doc.toJson(QJsonDocument::Compact));
    // ★ JS literal 안전 전달 — base64 인코딩으로 모든 특수문자 우회.
    //   JS 의 atob() 가 풀어줌. ASCII-only 라서 JS string literal 깨질 일 없음.
    QString b64 = QString::fromUtf8(configStr.toUtf8().toBase64());
    runJs(QString("setConfig(decodeURIComponent(escape(atob('%1'))))").arg(b64));

    // Check temp dir: prompt if not set OR path no longer exists
    {
        QString td = m_config->tempDir();
        if (!td.isEmpty() && !QDir(td).exists()) {
            log(QString("⚠️ 디스크 경로가 존재하지 않습니다: %1").arg(td), "warning", "settings");
            m_config->setTempDir("");
            m_config->save();
            td = "";
        }
        runJs(QString("checkDiskSetup('%1')").arg(td));
    }

    // Restore form inputs (대상, 경로, 유형, 옵션 등)
    loadFormData();

    // WebDAV 설정 복원
    if (m_webdav) {
        m_webdav->setConfig(m_config->webdavUrl(), m_config->webdavUser(), m_config->webdavPass(),
                            m_config->tempDir(), m_config->webdavEnabled());
    }

    // ★ yt-dlp — 사용자 폴더에 번들 복사 보장 (안전: 앱과 함께 출하된 검증 버전)
    //   자동 업데이트는 사용자가 토글 ON 했을 때만 (GitHub 변조 위험 → 기본 OFF)
    //   업데이트 후 sanity check 실패 시 번들로 자동 복원.
    Common::ensureYtDlpReady(m_config->ytDlpAutoUpdate());

    // ★ NAS watchdog 자동 시작 — 30초마다 마운트 체크 + 자동 재연결
    if (!m_nasWatchdogTimer) {
        m_nasWatchdogTimer = new QTimer(this);
        connect(m_nasWatchdogTimer, &QTimer::timeout, this, &MiyoBackend::nasWatchdogTick);
        m_nasWatchdogTimer->start(30000);  // 30초
    }

    // ★ 백업 워커 — 앱 시작 시 자동 시작 X.
    //   "껐는데도 백업 됨" 버그 방지 — 사용자가 명시적으로 체크박스 ON 누를 때만 시작 (setBackupConfig).
    //   config 에 backupEnabled=true 가 저장돼 있어도 이번 세션엔 false 로 reset.
    if (m_config->backupEnabled()) {
        m_config->setBackupEnabled(false);
        m_config->save();
        log("ℹ 백업 활성화 상태로 종료됐었음 → 이번 세션엔 비활성으로 시작 (체크박스 다시 ON 해야 작동)", "info", "settings");
    }

    // ★ 저장 모드 복원 (앱 시작 시 — 이전에 NAS/외장 체크해뒀으면 자동 적용)
    QTimer::singleShot(500, this, [this]() {
        // 옛 NAS UI 잔재 청소
        runJs("if(window._forceAttachNasButtons) _forceAttachNasButtons();");
        // 저장 모드 복원 — backend 콜백 (JSON array 로 wrap 해서 안전 인코딩)
        QString mode = m_config->storageMode();
        QString root = m_config->storageRoot();
        QJsonArray args; args.append(mode); args.append(root);
        QString js = QString("if(window.onStorageModeChanged) onStorageModeChanged.apply(null, %1);")
            .arg(QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact)));
        runJs(js);
    });

    // Load trad cover preview
    getTradCoverBase64();

    // Startup log (background)
    writeStartupLog();

    // 앱 시작 시 자동 유지보수
    QTimer::singleShot(1000, this, [this]() {
        // 1. Chrome에서 모든 플랫폼 토큰/세션 자동 추출
        refreshAllTokens();

        // 2. Python 환경 자동 진단/복구/업데이트 — ★ 자동 실행 비활성화
        //   원인: pip install이 앱 번들 안 python_env/site-packages에 새 파일 추가 → codesign seal 깨짐
        //         → "sealed resource is missing/invalid" → macOS 하드닝 정책에 따라 CDP/subprocess 시
        //            앱이 SIGKILL 당함. 한 달간 매 실행마다 자기 서명 깨뜨리고 있었음.
        //   해결: 자동 update 끄고, 사용자가 직접 "설정 → Python 환경 진단/업데이트" 누를 때만 실행.
        //         그 경우 사용자가 작업 완료 후 앱 재빌드/재서명해서 해결.
        log("Python 자동 업데이트 비활성화 — 설정 탭에서 수동 실행", "info", "settings");
    });
}

void MiyoBackend::saveConfig(const QString &configJson)
{
    QJsonDocument doc = QJsonDocument::fromJson(configJson.toUtf8());
    if (doc.isNull()) return;

    m_config->fromJson(doc.object());
    m_config->save();
}

void MiyoBackend::saveFormData(const QString &formJson)
{
    QJsonDocument doc = QJsonDocument::fromJson(formJson.toUtf8());
    if (doc.isNull()) return;
    m_config->setFormData(doc.object());
    m_config->save();
}

void MiyoBackend::loadFormData()
{
    QJsonObject data = m_config->formData();
    if (data.isEmpty()) return;
    QString json = QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Compact));
    // ★ base64 우회 — 모든 특수문자 안전
    QString b64 = QString::fromUtf8(json.toUtf8().toBase64());
    runJs(QString("restoreFormData(decodeURIComponent(escape(atob('%1'))))").arg(b64));
}

void MiyoBackend::browsePath(const QString &platform)
{
    // ★ 시작 디렉토리 /Volumes — Finder 처럼 NAS/외장 목록부터 보임
    QString startDir = QDir("/Volumes").exists() ? "/Volumes" : QDir::homePath();
    QString folder = QFileDialog::getExistingDirectory(m_window, "저장 경로 선택", startDir);
    if (!folder.isEmpty()) {
        runJs(QString("setPath('%1', '%2')").arg(platform, folder));
    }
}

// ★ 모든 플랫폼 저장 경로를 NAS 로 일괄 변경
//   마운트된 첫 NAS/외장 자동 감지 → 각 플랫폼 input 에 /Volumes/X/<Platform>/ 자동 입력 + 저장
// (QInputDialog include는 위 pickMountedVolume 에서 이미 들어옴)
void MiyoBackend::setAllPathsToNas()
{
    QString chosenPath;
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
        log("마운트된 NAS/외장 없음 — 설정 → WebDAV → Finder 마운트 먼저", "warning", "settings");
        runJs("alert('마운트된 NAS/외장 디스크가 없습니다.\\n\\nWebDAV → \"📂 Finder에 마운트\" 먼저.');");
        return;
    }

    QMetaObject::invokeMethod(this, [this, items, paths]() {
        bool ok = false;
        QString chosen = QInputDialog::getItem(m_window,
            "🌐 NAS 일괄 적용",
            "모든 플랫폼의 저장 경로를 어느 볼륨으로 변경?\n(각 플랫폼은 그 아래 자동 하위 폴더 생성)",
            items, 0, false, &ok);
        if (!ok || chosen.isEmpty()) return;
        int idx = items.indexOf(chosen);
        if (idx < 0 || idx >= paths.size()) return;
        QString root = paths[idx];

        // 플랫폼 → 하위 폴더 매핑
        struct PlatformDir { const char *inputId; const char *subdir; };
        const PlatformDir mappings[] = {
            {"twitter-path",     "Twitter"},
            {"bsky-path",        "Bluesky"},
            {"youtube-path",     "YouTube"},
            {"discord-path",     "Discord"},
            {"instagram-path",   "Instagram"},
            {"pixiv-path",       "Pixiv"},
            {"fanbox-path",      "Fanbox"},
            {"tumblr-path",      "Tumblr"},
            {"spinspin-path",    "SpinSpin"},
            {"asked-path",       "Asked"},
            {"crawl-path",       "Crawl"},
            {"trad-path",        "Trad"},
            {"naikakukai-path",  "Naikakukai"},
        };

        log(QString("🌐 NAS 일괄 적용: %1").arg(root), "success", "settings");
        // 각 input 값 일괄 갱신
        QString js = "(function(){ var c=0;";
        for (const auto &m : mappings) {
            QString fullPath = root + "/Chernobyl/" + m.subdir;
            QDir().mkpath(fullPath);  // 미리 폴더 생성
            QString p = fullPath;
            QString safeInput  = Common::jsStringLiteral(QString(m.inputId));
            QString safePath   = Common::jsStringLiteral(p);
            js += QString(
                " { var el=document.getElementById(%1);"
                "   if(el){el.value=%2;el.dispatchEvent(new Event('change'));el.dispatchEvent(new Event('input'));c++;}"
                " }"
            ).arg(safeInput, safePath);
        }
        js += " if(typeof saveForm==='function') saveForm();"
              " alert('🌐 NAS 일괄 적용 완료 ('+c+'개 플랫폼)\\n경로: " + root + "/Chernobyl/...');"
              " })();";
        runJs(js);
    }, Qt::QueuedConnection);
}

// ═════════════════════════════════════════════════════════════════════════
// 저장 모드 — local / nas / external
//   체크박스 한 번이면 모든 플랫폼 일괄 NAS/외장 사용 + 개별 input 숨김
// ═════════════════════════════════════════════════════════════════════════
void MiyoBackend::setStorageMode(const QString &mode)
{
    if (mode == "local") {
        // 로컬 모드 — 각 플랫폼 input 다시 보임 + 저장
        m_config->setStorageMode("local");
        m_config->setStorageRoot("");
        m_config->save();
        log("📂 저장 모드: 로컬 — 각 플랫폼 개별 경로 사용", "info", "settings");
        runJs("if(window.onStorageModeChanged) onStorageModeChanged('local','');");
        return;
    }

    // nas / external — 마운트된 볼륨 선택
    bool wantNetwork = (mode == "nas");
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
        bool isNet = networkMounts.contains(name);
        // 모드 별 필터: nas → 네트워크 마운트만, external → 로컬 외장만
        if (wantNetwork && !isNet) continue;
        if (!wantNetwork && isNet) continue;
        paths << path;
        items << QString("%1 %2  (%3)").arg(isNet ? "🌐" : "💾", name, path);
    }
#endif
    if (items.isEmpty()) {
        QString hint = wantNetwork
            ? "WebDAV → 'Finder에 마운트' 먼저 누르세요"
            : "외장 디스크 (USB/SSD) 연결 후 다시 시도";
        log(QString("마운트된 %1 없음 — %2").arg(wantNetwork ? "NAS" : "외장", hint), "warning", "settings");
        runJs(QString("alert('마운트된 %1 없음.\\n\\n%2');"
                      "if(window.onStorageModeChanged) onStorageModeChanged('local','');")
              .arg(wantNetwork ? "NAS" : "외장 디스크", hint));
        return;
    }

    QMetaObject::invokeMethod(this, [this, mode, items, paths, wantNetwork]() {
        // 1단계: 볼륨 선택
        bool ok = false;
        QString title = wantNetwork ? "🌐 NAS 선택 — 1단계: 볼륨" : "💾 외장 디스크 선택 — 1단계: 볼륨";
        QString prompt = wantNetwork
            ? "모든 플랫폼의 저장 경로를 어느 NAS 로?"
            : "모든 플랫폼의 저장 경로를 어느 외장 디스크로?";
        QString chosen = QInputDialog::getItem(m_window, title, prompt, items, 0, false, &ok);
        if (!ok || chosen.isEmpty()) {
            runJs("if(window.onStorageModeChanged) onStorageModeChanged('local','');");
            return;
        }
        int idx = items.indexOf(chosen);
        if (idx < 0 || idx >= paths.size()) {
            runJs("if(window.onStorageModeChanged) onStorageModeChanged('local','');");
            return;
        }
        QString volumeRoot = paths[idx];

        // 2단계: 그 볼륨 안에서 폴더 선택 (Finder dialog — 새 폴더 만들기 가능)
        QString chosenFolder = QFileDialog::getExistingDirectory(
            m_window,
            QString("%1 — 2단계: 폴더 선택 (모든 플랫폼이 이 폴더 아래 저장됨)")
                .arg(wantNetwork ? "🌐 NAS" : "💾 외장"),
            volumeRoot,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
        );
        if (chosenFolder.isEmpty() || !chosenFolder.startsWith(volumeRoot)) {
            runJs("if(window.onStorageModeChanged) onStorageModeChanged('local','');");
            return;
        }
        QString root = chosenFolder;  // ★ 이제 root = 사용자가 선택한 폴더 (볼륨 root 아님)

        struct PlatformDir { const char *inputId; const char *subdir; };
        const PlatformDir mappings[] = {
            {"twitter-path",    "Twitter"},
            {"bsky-path",       "Bluesky"},
            {"youtube-path",    "YouTube"},
            {"discord-path",    "Discord"},
            {"instagram-path",  "Instagram"},
            {"pixiv-path",      "Pixiv"},
            {"fanbox-path",     "Fanbox"},
            {"tumblr-path",     "Tumblr"},
            {"spinspin-path",   "SpinSpin"},
            {"asked-path",      "Asked"},
            {"crawl-path",      "Crawl"},
            {"trad-path",       "Trad"},
            {"naikakukai-path", "Naikakukai"},
        };
        // ★ 사용자가 명시한 폴더 그대로 사용 (Chernobyl/ subdir 자동 추가 X)
        //   각 플랫폼은 <chosenFolder>/<Platform> 으로
        for (const auto &m : mappings) {
            QDir().mkpath(root + "/" + m.subdir);
        }

        m_config->setStorageMode(mode);
        m_config->setStorageRoot(root);
        m_config->save();

        log(QString("%1 모드: %2 — 모든 플랫폼 경로 일괄 변경").arg(wantNetwork ? "🌐 NAS" : "💾 외장", root),
            "success", "settings");

        QString safeRoot = Common::jsStringLiteral(root);
        QString safeMode = Common::jsStringLiteral(mode);
        QString js = "(function(){ var c=0;";
        for (const auto &m : mappings) {
            QString fullPath = root + "/" + m.subdir;
            QString safeInput = Common::jsStringLiteral(QString(m.inputId));
            QString safePath  = Common::jsStringLiteral(fullPath);
            js += QString(
                " { var el=document.getElementById(%1);"
                "   if(el){el.value=%2;el.dispatchEvent(new Event('change'));el.dispatchEvent(new Event('input'));c++;} }"
            ).arg(safeInput, safePath);
        }
        js += QString(" if(typeof saveForm==='function') saveForm();"
                      " if(window.onStorageModeChanged) onStorageModeChanged(%1, %2);"
                      " })();").arg(safeMode, safeRoot);
        runJs(js);
    }, Qt::QueuedConnection);
}

void MiyoBackend::openFolder(const QString &path)
{
    QString folderPath = path;
    folderPath.replace("~", QDir::homePath());
    QDir().mkpath(folderPath);

#ifdef Q_OS_MACOS
    QProcess::startDetached("open", {folderPath});
#elif defined(Q_OS_WIN)
    QProcess::startDetached("explorer", {folderPath});
#else
    QProcess::startDetached("xdg-open", {folderPath});
#endif
}

void MiyoBackend::pasteToField(const QString &fieldId)
{
    QString text = QApplication::clipboard()->text().trimmed();
    if (!text.isEmpty()) {
        QString escaped = text;
        escaped.replace("`", "\\`");
        escaped.replace("$", "\\$");
        runJs(QString("document.getElementById('%1').value = `%2`").arg(fieldId, escaped));
    }
}

void MiyoBackend::pasteClipboard()
{
    QString text = QApplication::clipboard()->text().trimmed();
    if (!text.isEmpty()) {
        runJs(QString("appendYoutubeUrl(`%1`)").arg(text));
    }
}

// ★ openExternalApp 제거됨 — anipo/AINU companion apps 더 이상 번들/사용 안 함.

// No-ops: everything is now inline in the main app
void MiyoBackend::openYoutubeWindow() {}
void MiyoBackend::openDiscordWindow() {}
void MiyoBackend::openInstagramWindow() {}

// Helper: find bundled tool path, fallback to system
static QString findBundledTool(const QString &name)
{
    QString appDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_WIN
    // Windows: tools are next to the exe, check with .exe extension too
    QString bundled = appDir + "/" + name;
    if (QFile::exists(bundled)) return bundled;
    if (!name.endsWith(".exe")) {
        QString withExe = appDir + "/" + name + ".exe";
        if (QFile::exists(withExe)) return withExe;
    }
#else
    // macOS: tools inside Contents/MacOS/
    QString bundled = appDir + "/" + name;
    if (QFile::exists(bundled)) return bundled;
#endif
    // Fallback to system PATH
    return name;
}

// Use Common::bundledProcessEnv() instead of local bundledEnv()
static QProcessEnvironment bundledEnv()
{
    return Common::bundledProcessEnv();
}

void MiyoBackend::startCollection(const QString &configJson)
{
    // [DEBUG] window._debugLogsEnabled가 true일 때만 [CPP] 라인 출력 — 직접 runJs로 호출
    auto dbg = [this](const QString &msg, const QString &platform = "twitter") {
        QString safe = QString(msg).replace("'", "\\'").replace("\n", " ");
        runJs(QString("window.cppDbgLog && cppDbgLog('%1','%2')").arg(safe, platform));
    };
    dbg("startCollection ENTERED");

    QJsonDocument doc = QJsonDocument::fromJson(configJson.toUtf8());
    if (doc.isNull()) { dbg("config JSON parse fail — early return"); return; }

    QJsonObject config = doc.object();
    QString platformName = config["platform"].toString();
    // ★ 병렬 모드: JS가 _parallelKey ("plat#idx")를 보냈으면 그 키로 unique tracking
    //   - 같은 platform에 여러 worker thread 허용 (각각 독립 isRunning, terminal log)
    //   - updateStats / setRunning / log는 _parallelKey를 platform 자리에 넣어 호출
    //   - JS는 platform 문자열에 '#'이 있으면 병렬 키로 인식해서 statsByKey에 라우팅
    const QString parallelKey = config["_parallelKey"].toString();
    const bool isParallel = !parallelKey.isEmpty();
    const QString trackKey = isParallel ? parallelKey : platformName;
    dbg(QString("platform=%1, trackKey=%2").arg(platformName, trackKey), platformName);

    // 디스크 설정 필수 체크 + 경로 존재 여부
    {
        QString td = m_config->tempDir();
        if (!td.isEmpty() && !QDir(td).exists()) {
            log(QString("⚠️ 디스크 경로가 사라졌습니다: %1").arg(td), "warning", platformName);
            m_config->setTempDir("");
            m_config->save();
            td = "";
        }
        if (td.isEmpty()) {
            dbg("EARLY RETURN: 디스크 설정 없음", platformName);
            log("디스크 설정을 먼저 해주세요!", "error", platformName);
            runJs(QString("setRunning('%1', false)").arg(platformName));
            runJs("showDiskModal()");
            return;
        }
    }
    dbg(QString("disk OK: %1").arg(m_config->tempDir()), platformName);

    m_currentPlatform = platformName;

    // ★ 보조 저장 경로 자동 분산 — 1번 디스크 free < 10GB이면 2번으로 전환.
    //    config의 모든 부분에 path를 picked path로 교체해서 collector들이 자동 사용.
    {
        QString primary = config["path"].toString();
        QString secondary = m_config->secondaryPath();
        QString picked = Common::pickSavePath(primary, secondary, 10.0);
        if (picked != primary) {
            log(QString("저장 경로 자동 전환: %1 → %2 (1번 디스크 공간 부족)")
                .arg(primary, picked), "warning", platformName);
            config["path"] = picked;
        }
    }

    // Open terminal log window — 병렬이면 각 trackKey마다 별도 터미널 (개별 .command)
    QString savePath = config["path"].toString();
    openTerminalLog(trackKey, savePath);

    // Prevent system sleep during collection
    if (m_window) m_window->holdAwake();

    // 중복 방지 — '진짜 실행 중'일 때만 막는다.
    //   끝났거나(완료 정리 레이스) 중지된(stale) 스레드 엔트리는 정리하고 새로 시작
    //   → "이미 수집 중" 거짓양성 방지. (실행 플래그가 꺼졌으면 더 이상 수집 중이 아님)
    if (m_collectionThreads.contains(trackKey)) {
        QThread *existing = m_collectionThreads[trackKey];
        const bool reallyRunning = existing && !existing->isFinished()
                                   && (platformRunning(trackKey) || platformRunning(platformName));
        if (reallyRunning) {
            dbg("EARLY RETURN: 이미 수집 중", platformName);
            log(QString("이미 수집 중입니다! (%1)").arg(trackKey), "warning", platformName);
            return;
        }
        // stale 정리: 끝난 스레드만 직접 deleteLater. (실행 중인데 플래그만 꺼진 경우엔
        //   finished→deleteLater 연결에 맡기고 맵 참조만 제거 — 러닝 스레드 삭제/이중삭제 방지)
        if (existing && existing->isFinished()) existing->deleteLater();
        m_collectionThreads.remove(trackKey);
        dbg("stale collection thread cleared — 재시작 허용", platformName);
    }
    dbg("collection thread check OK", platformName);

    // Crawl은 QWebEnginePage 사용 → 메인 스레드에서 실행
    if (platformName == "crawl") {
        setPlatformRunning(platformName, true);
        runCrawlCollection(config);
        log("크롤링 시작", "info", platformName);
        return;
    }

    // 플랫폼별 독립 스레드 생성 — 병렬: 같은 platform에 여러 thread 가능
    {
        QMutexLocker lock(&m_runningMutex);
        m_isRunning[trackKey] = true;       // 병렬 키별 isRunning
        m_isRunning[platformName] = true;   // platform 단위도 true (collector 코드 호환)
    }
    runJs(QString("setRunning('%1', true)").arg(platformName));
    m_stopRequested[trackKey] = false;
    m_stopRequested[platformName] = false;
    dbg(QString("about to create QThread (%1)").arg(trackKey), platformName);
    QThread *thread = QThread::create([this, config, platformName, trackKey, isParallel]() {
        // ★ 병렬 모드: 워커 스레드가 자기 trackKey 등록 → 모든 log() 호출이
        //   thread-local lookup을 통해 trackKey의 터미널 파일에만 라우팅된다.
        //   이게 없으면 모든 터미널이 platform="twitter"로 수렴된 같은 로그를 보게 됨.
        if (isParallel) setThreadTrackKey(trackKey);
        QString safe = trackKey;
        QMetaObject::invokeMethod(this, [this, safe]() {
            runJs(QString("window.cppDbgLog && cppDbgLog('WORKER THREAD ENTERED','%1')").arg(safe));
        }, Qt::QueuedConnection);
        // ★ 사용자가 선택한 옵션 (체크박스/입력값) 모두 로그에 기록 — 디버깅/재현용
        logCollectionOptions(config, platformName);
        if (platformName == "twitter") {
            runTwitterCollection(config);
        } else if (platformName == "bluesky") {
            runBlueskyCollection(config);
        } else if (platformName == "discord") {
            runDiscordCollection(config);
        } else if (platformName == "instagram") {
            runInstagramCollection(config);
        } else if (platformName == "pixiv") {
            runPixivCollection(config);
        } else if (platformName == "tumblr") {
            runTumblrCollection(config);
        } else if (platformName == "spinspin") {
            runSpinSpinCollection(config);
        } else if (platformName == "asked") {
            runAskedCollection(config);
        } else if (platformName == "fanbox") {
            runFanboxCollection(config);
        }
        // ★ 수집 끝 — manifest 생성 (무결성 검증 + 통계 파일)
        //   path 가 root, 그 안의 모든 파일 walk 해서 __CHERNOBYL_MANIFEST__.json + .txt
        QString plPath = config["path"].toString();
        plPath.replace("~", QDir::homePath());
        if (!plPath.isEmpty() && QDir(plPath).exists()) {
            writeDownloadManifest(plPath, platformName);
        }
        // 워커 스레드 종료 직전 trackKey 등록 해제
        if (isParallel) clearThreadTrackKey();
        // 완료 처리 (메인 스레드)
        QMetaObject::invokeMethod(this, [this, platformName, trackKey, isParallel]() {
            {
                QMutexLocker lock(&m_runningMutex);
                m_isRunning[trackKey] = false;
                // 같은 platform에 다른 trackKey가 아직 running이면 platform 단위 isRunning 유지
                bool anyRunning = false;
                for (auto it = m_isRunning.constBegin(); it != m_isRunning.constEnd(); ++it) {
                    const QString k = it.key();
                    if (k == platformName) continue;  // 자기자신 제외
                    if ((k == platformName || k.startsWith(platformName + "#")) && it.value()) {
                        anyRunning = true; break;
                    }
                }
                if (!anyRunning) m_isRunning[platformName] = false;
            }
            const bool wasStopped = m_stopRequested.value(trackKey, false)
                                 || m_stopRequested.value(platformName, false);
            const QString statsKey = isParallel ? trackKey : platformName;
            if (wasStopped) {
                runJs(QString("updateStats(0, 0, '중단됨', '%1')").arg(statsKey));
                m_lastStatsUpdate[statsKey] = QDateTime::currentMSecsSinceEpoch();
                log(QString("⏹ 수집이 사용자 요청으로 중단되었습니다. (%1)").arg(trackKey), "warning", platformName);
            } else {
                updateStats(0, 0, "Done", statsKey);
                log(QString("Complete. (%1)").arg(trackKey), "success", platformName);
            }
            // 병렬: platform 단위 setRunning(false)는 모든 trackKey가 끝났을 때만
            bool platformIdle = !m_isRunning.value(platformName, false);
            if (platformIdle) {
                runJs(QString("setRunning('%1', false)").arg(platformName));
            }
            m_stopRequested[trackKey] = false;
            closeTerminalLog(trackKey);
            m_collectionThreads.remove(trackKey);
            // 모든 수집이 끝났으면 절전 해제
            if (m_collectionThreads.isEmpty()) {
                if (m_window) m_window->releaseAwake();
            }
        });
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    m_collectionThreads[trackKey] = thread;
    thread->start();
    log(QString("수집 시작 (%1)").arg(trackKey), "info", platformName);
}

void MiyoBackend::stopCollection(const QString &platformName)
{
    // 1) 플래그 즉시 내림 (모든 폴링 지점에서 다음 체크 시 종료)
    //    병렬 모드에서는 platform#0, platform#1, ... 도 함께 false로 내려야 함
    {
        QMutexLocker lock(&m_runningMutex);
        m_isRunning[platformName] = false;
        const QString prefix = platformName + "#";
        for (auto it = m_isRunning.begin(); it != m_isRunning.end(); ++it) {
            if (it.key().startsWith(prefix)) it.value() = false;
        }
    }
    m_stopRequested[platformName] = true;
    {
        const QString prefix = platformName + "#";
        for (auto it = m_collectionThreads.constBegin(); it != m_collectionThreads.constEnd(); ++it) {
            if (it.key().startsWith(prefix)) m_stopRequested[it.key()] = true;
        }
    }
    log("⏹ 중단 요청 — 진행 중인 작업을 종료합니다...", "warning", platformName);

    // ★ 터미널 동기화 — GUI 중지 누르면 해당 platform 의 tail script 도 즉시 종료
    //   1) [DONE] 마커 write — tail script 가 정상 종료 path 로 가서 사용자에게 안내 표시
    //   2) tail .command process 강제 kill — 사용자가 확인 안 눌러도 닫힘
    {
        QStringList platKeys;
        platKeys << platformName;
        QMutexLocker lock(&m_runningMutex);
        for (auto it = m_isRunning.constBegin(); it != m_isRunning.constEnd(); ++it) {
            if (it.key().startsWith(platformName + "#")) platKeys << it.key();
        }
        lock.unlock();
        for (const QString &pk : platKeys) {
            if (m_terminalLogPaths.contains(pk)) {
                QString logPath = m_terminalLogPaths[pk];
                // [DONE] 마커 write — tail script wakeup → 종료 path
                QFile lf(logPath);
                if (lf.open(QIODevice::Append | QIODevice::Text)) {
                    lf.write("\n\033[1;31m⏹ 사용자 중지 — 터미널 종료 중\033[0m\n[DONE]\n");
                    lf.close();
                }
            }
        }
        // 1초 후 tail script process 강제 kill (사용자 키 입력 안 해도 닫힘)
        QTimer::singleShot(1500, this, [this, platformName]() {
#ifdef Q_OS_MACOS
            QProcess::execute("/usr/bin/pkill", {"-f", "miyo_" + platformName + "_tail.command"});
#elif defined(Q_OS_WIN)
            QProcess::execute("taskkill", {"/F", "/IM", "cmd.exe", "/FI",
                                            "COMMANDLINE eq *miyo_" + platformName + "_tail.bat*"});
#endif
        });
    }

    // 2) 플랫폼별 "블로킹 지점" 즉시 해제 — 데몬 프로세스에 SIGTERM 쏴서
    //    대기중인 HTTP/twikit 호출을 끊어냄. 안 그러면 sendDaemonCommand(60초 timeout),
    //    QNetworkReply 대기 등에 걸려 최대 수십 초까지 terminal 로그가 계속 나옴.
    auto killPid = [&](qint64 pid) {
        if (pid <= 0) return;
#ifdef Q_OS_WIN
        QProcess::execute("taskkill", {"/PID", QString::number(pid), "/F", "/T"});
#else
        ::kill(static_cast<pid_t>(pid), SIGTERM);
#endif
    };

    // Crawl: SiteCrawler 즉시 중지
    if (platformName == "crawl" && m_crawler) {
        m_crawler->stop();
    }

    // Twitter: twikit 데몬 즉시 종료 — 다음 sendDaemonCommand()가 실패하며 루프 탈출
    if (platformName == "twitter" && m_twitterCollector) {
        killPid(m_twitterCollector->daemonPid());
    }

    // Bluesky: 데몬 프로세스 즉시 종료
    if (platformName == "bluesky" && m_blueskyCollector) {
        killPid(m_blueskyCollector->daemonPid());
    }

    // ★ stop 시 capture Chrome의 process만 종료 (객체 자체는 destroy 안 함)
    //   진행 중 lambda chain이 chromePtr 통해 접근 중 → 즉시 deleteLater하면 dangling crash.
    //   stop() 호출 → ws disconnect → pending callback이 error로 호출 → lambda chain 자연 종료.
    //   객체 자체는 살아있다가 다음 capture 시 재시작 (start()) 또는 destructor에서 정리.
    {
        QMutexLocker mapLock(&m_capChromeMapMutex);
        if (m_captureChromesPerThread.contains(platformName)) {
            auto *c = m_captureChromesPerThread.value(platformName);
            if (c) c->stop();   // process kill만, 객체는 유지
        }
        const QString prefix = platformName + "#";
        for (auto it = m_captureChromesPerThread.constBegin(); it != m_captureChromesPerThread.constEnd(); ++it) {
            if (it.key().startsWith(prefix) && it.value()) it.value()->stop();
        }
    }

    // 3) UI/터미널 즉시 동기화 — 시작 버튼 활성화, 중지 버튼 비활성화,
    //    상태 배지를 "중단됨"으로 바꿔서 실제 스레드 종료 전에 시각적 피드백 제공
    runJs(QString("setRunning('%1', false)").arg(platformName));
    // updateStats를 직접 돌려 상태를 "중단됨"으로 바꿈 — throttle 우회 위해 runJs 직접 호출
    runJs(QString("updateStats(0, 0, '중단됨', '%1')").arg(platformName));
    m_lastStatsUpdate[platformName] = QDateTime::currentMSecsSinceEpoch();
}

// ═════════════════════════════════════════════════════════════════════════
// showSystemNotification — macOS 시스템 알림 (osascript display notification)
//   다른 앱 쓰는 중에도 사용자에게 새 글 알림이 도달
// ═════════════════════════════════════════════════════════════════════════
// ═════════════════════════════════════════════════════════════════════════
// 디버그 진단 — 설정 탭의 진단 버튼들이 호출
// ═════════════════════════════════════════════════════════════════════════
void MiyoBackend::getDiagnosticInfo()
{
    QString info;
#ifdef Q_OS_MACOS
    QProcess p;
    p.start("/bin/sh", {"-c", "ps aux | grep -iE 'Chrome.*chrome_capture_profile' | grep -v grep | wc -l"});
    p.waitForFinished(2000);
    int chromeCount = QString::fromUtf8(p.readAllStandardOutput()).trimmed().toInt();
    info += QString("● 좀비/활성 캡쳐 Chrome 프로세스: %1개\n").arg(chromeCount);

    QProcess vm;
    vm.start("/bin/sh", {"-c", "vm_stat | head -5"});
    vm.waitForFinished(1000);
    info += "● vm_stat:\n" + QString::fromUtf8(vm.readAllStandardOutput());
#endif
    info += QString("● m_captureChromesPerThread: %1개\n").arg(m_captureChromesPerThread.size());
    info += QString("● 활성 collection thread: %1개\n").arg(m_collectionThreads.size());
    info += QString("● 디스크 path: %1\n").arg(m_config ? m_config->tempDir() : "(none)");
    info.replace("'", "\\'").replace("\n", "\\n");
    runJs(QString("if(window.onDiagInfo) onDiagInfo('%1');").arg(info));
}

void MiyoBackend::killZombieChromes()
{
    // 좀비 정리 대상:
    //   1) chrome_capture_profile (사용자 Chrome args 매치) — 기존
    //   2) 앱 내부 번들 Chromium (Chrome for Testing) + 모든 helper + crashpad — 새로 추가
    //   사용자 일반 Chrome (/Applications/Google Chrome.app) 은 영향 없음
    int killed = 0;
#ifdef Q_OS_MACOS
    // 패턴 1: 우리 capture profile 사용하는 chrome
    if (QProcess::execute("/usr/bin/pkill", {"-f", "chrome_capture_profile"}) == 0) killed++;
    // 패턴 2: 앱 번들 안 Chromium (Chrome for Testing) — main + helper + crashpad
    //   사용자 시스템에 Chrome for Testing 별도 설치 안 했으면 안전
    if (QProcess::execute("/usr/bin/pkill", {"-9", "-f", "Chrome for Testing"}) == 0) killed++;
    if (QProcess::execute("/usr/bin/pkill", {"-9", "-f", "chrome_crashpad_handler"}) == 0) killed++;
    // 패턴 3: 앱 번들 경로 기준 — 절대 경로로 우리 거 확실하게
    QString appDir = QCoreApplication::applicationDirPath();
    QString chromiumPath = appDir + "/../Resources/chromium";
    QFileInfo fi(chromiumPath);
    if (fi.exists()) {
        QString absPath = fi.absoluteFilePath();
        if (QProcess::execute("/usr/bin/pkill", {"-9", "-f", absPath}) == 0) killed++;
    }
#elif defined(Q_OS_WIN)
    QProcess::execute("taskkill", {"/F", "/IM", "chrome.exe", "/FI",
                                    "COMMANDLINE eq *chrome_capture_profile*"});
    QProcess::execute("taskkill", {"/F", "/IM", "Chrome for Testing.exe"});
    QProcess::execute("taskkill", {"/F", "/IM", "chrome_crashpad_handler.exe"});
#endif
    log(QString("좀비 정리 완료 — capture chrome + 앱 내부 Chromium + helper/crashpad").arg(killed),
        "success", "settings");
    runJs("if(window.onDiagInfo) onDiagInfo('좀비 정리 완료.\\n"
          "• capture chrome (chrome_capture_profile 매치)\\n"
          "• 앱 내부 Chromium (Chrome for Testing)\\n"
          "• Chromium helper / crashpad handler\\n"
          "사용자 일반 Chrome 은 영향 없음.');");
}

// ═════════════════════════════════════════════════════════════════════════
// WebDAV NAS 업로드 — Synology 등
// ═════════════════════════════════════════════════════════════════════════
void MiyoBackend::setWebDavConfig(const QString &url, const QString &user, const QString &pass, bool enabled)
{
    m_config->setWebdavUrl(url);
    m_config->setWebdavUser(user);
    m_config->setWebdavPass(pass);
    m_config->setWebdavEnabled(enabled);
    m_config->save();
    if (m_webdav) {
        m_webdav->setConfig(url, user, pass, m_config->tempDir(), enabled);
    }
    log(QString("WebDAV 설정 저장: %1 (활성화=%2)").arg(url).arg(enabled ? "ON" : "OFF"), "success", "settings");
}

void MiyoBackend::testWebDavConnection()
{
    QString url  = m_config->webdavUrl();
    QString user = m_config->webdavUser();
    QString pass = m_config->webdavPass();
    if (url.isEmpty()) {
        log("WebDAV URL 미설정", "warning", "settings");
        return;
    }
    log(QString("WebDAV 연결 테스트: %1").arg(url), "info", "settings");
    QThread *t = QThread::create([this, url, user, pass]() {
        // PROPFIND 또는 OPTIONS 로 연결 확인
        QProcess curl;
        curl.start("curl", {
            "-sS", "-k",
            "-X", "OPTIONS",
            "-u", user + ":" + pass,
            "--max-time", "10",
            "-w", "HTTP_CODE=%{http_code}",
            url
        });
        bool fin = curl.waitForFinished(15000);
        QString out = QString::fromUtf8(curl.readAllStandardOutput());
        QMetaObject::invokeMethod(this, [this, fin, out]() {
            if (!fin) {
                log("WebDAV 연결 타임아웃 (10초)", "error", "settings");
                return;
            }
            if (out.contains("HTTP_CODE=200") || out.contains("HTTP_CODE=207")) {
                log("✅ WebDAV 연결 성공!", "success", "settings");
            } else if (out.contains("HTTP_CODE=401")) {
                log("❌ 인증 실패 — 사용자/비번 확인", "error", "settings");
            } else if (out.contains("HTTP_CODE=404")) {
                log("❌ URL 경로 없음 — base URL 확인 (예: /webdav/공유폴더)", "error", "settings");
            } else {
                log(QString("❌ WebDAV 응답 비정상: %1").arg(out.left(150)), "error", "settings");
            }
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

void MiyoBackend::enqueueWebDavUpload(const QString &localPath)
{
    if (m_webdav && m_webdav->isEnabled()) {
        m_webdav->enqueue(localPath);
    }
    enqueueBackup(localPath);
    // ★ 무결성 검사 자동 트리거 — 어떤 플랫폼이든 토글 ON 이면 검사
    enqueueIntegrityCheck(localPath, "auto");
}

// ═════════════════════════════════════════════════════════════════════════
// Finder에 WebDAV 마운트 — AppleScript "mount volume" 호출.
//   Finder가 OS 차원에서 권한/인증 처리 → "권한 없음" 에러 우회.
//   마운트되면 /Volumes/<공유폴더> 생성 + 사이드바 표시.
//   사용자가 그 폴더를 저장 경로로 선택하면 다운로드가 NAS 로 직행.
// ═════════════════════════════════════════════════════════════════════════
void MiyoBackend::mountWebDavInFinder()
{
    QString url  = m_config->webdavUrl();
    QString user = m_config->webdavUser();
    QString pass = m_config->webdavPass();
    if (url.isEmpty()) {
        log("WebDAV URL 미설정 — 먼저 URL 입력하세요", "warning", "settings");
        return;
    }
    log(QString("Finder에 마운트 시도: %1").arg(url), "info", "settings");

    // AppleScript escape (백슬래시, 큰따옴표)
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
        // 마운트 전 /Volumes 상태 스냅샷
        QStringList before;
        {
            QDir d("/Volumes");
            before = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        }

        QProcess osa;
        osa.start("osascript", {"-e", script});
        bool fin = osa.waitForFinished(60000);  // Finder 인증 다이얼로그 떴을 수 있음 → 60초
        QString stdoutS = QString::fromUtf8(osa.readAllStandardOutput()).trimmed();
        QString stderrS = QString::fromUtf8(osa.readAllStandardError()).trimmed();
        int code = osa.exitCode();

        QStringList after;
        {
            QDir d("/Volumes");
            after = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        }
        // 새로 생긴 볼륨 = NAS
        QStringList newVols;
        for (const QString &n : after) {
            if (!before.contains(n)) newVols << n;
        }

        QMetaObject::invokeMethod(this, [this, fin, code, stdoutS, stderrS, newVols]() {
            if (!fin) {
                log("❌ 마운트 타임아웃 (60초) — Finder 인증창 답 안 됐을 수도", "error", "settings");
                return;
            }
            if (code != 0) {
                QString hint;
                if (stderrS.contains("-128") || stderrS.contains("User canceled"))
                    hint = " (사용자 취소)";
                else if (stderrS.contains("-1714") || stderrS.contains("invalid"))
                    hint = " — URL/사용자/비번 확인";
                else if (stderrS.contains("-43") || stderrS.contains("not found"))
                    hint = " — URL 경로 확인 (예: /webdav/공유폴더)";
                log(QString("❌ 마운트 실패%1: %2").arg(hint, stderrS.left(150)), "error", "settings");
                return;
            }
            if (!newVols.isEmpty()) {
                QString p = "/Volumes/" + newVols.first();
                log(QString("✅ Finder에 마운트됨: %1").arg(p), "success", "settings");
                log("   ► 저장 경로를 위 경로로 변경하면 다운로드가 NAS 로 직행", "info", "settings");
                // UI 에 새 마운트 포인트 알리기
                runJs(QString("if(window.onWebDavMounted) onWebDavMounted(%1);")
                      .arg(QJsonDocument(QJsonObject{{"path", p}, {"name", newVols.first()}}).toJson(QJsonDocument::Compact).constData()));
            } else {
                log("✅ 마운트 명령 성공 — Finder 사이드바 확인", "success", "settings");
            }
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

void MiyoBackend::openSecurityPrefs()
{
#ifdef Q_OS_MACOS
    // macOS 13+ : 시스템 설정 → 개인정보 보호 및 보안 → 파일 및 폴더
    QProcess::startDetached("open", {"x-apple.systempreferences:com.apple.preference.security?Privacy_FilesAndFolders"});
    log("시스템 설정 → 개인정보 보호 및 보안 → 파일 및 폴더 → 앱 권한 ✅", "info", "settings");
#elif defined(Q_OS_WIN)
    // Windows 는 mac 의 TCC(per-app 파일 권한) 같은 게 없음 — 별도 설정 불필요.
    log("Windows 는 별도 권한 설정이 필요 없습니다 ✅", "info", "settings");
#else
    log("권한 설정 정보 없음", "info", "settings");
#endif
}

// ═════════════════════════════════════════════════════════════════════════
// 마운트된 볼륨 목록 (NAS/외장/CD/USB 등) — UI 드롭다운 채움용
//   network=true 표시: WebDAV/SMB/AFP/NFS 등 (df 의 fstype 으로 판정)
// ═════════════════════════════════════════════════════════════════════════
void MiyoBackend::listMountedVolumes()
{
    qDebug() << "[NAS] listMountedVolumes called";
    QJsonArray vols;
#ifdef Q_OS_MACOS
    // /Volumes 의 폴더들 (시스템 볼륨 제외)
    QDir d("/Volumes");
    QStringList entries = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);

    // df로 네트워크/로컬 판정 — fstype 가 webdav, smbfs, afpfs, nfs 면 network
    QProcess df;
    df.start("df", {"-T", "webdav,smbfs,afpfs,nfs,fuse"});
    df.waitForFinished(5000);
    QString dfOut = QString::fromUtf8(df.readAllStandardOutput());

    QSet<QString> networkMounts;
    for (const QString &line : dfOut.split('\n', Qt::SkipEmptyParts)) {
        // df 출력: Filesystem ... Mounted on
        // 마운트 포인트는 보통 마지막 컬럼
        QStringList cols = line.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
        if (cols.size() >= 2) {
            QString mp = cols.last();
            if (mp.startsWith("/Volumes/")) {
                networkMounts.insert(mp.mid(QString("/Volumes/").length()));
            }
        }
    }

    for (const QString &name : entries) {
        // 시스템 볼륨 제외 (Macintosh HD 같은 거)
        QString path = "/Volumes/" + name;
        QFileInfo fi(path);
        if (!fi.isDir() || !fi.isWritable()) continue;
        // 시스템 루트 볼륨도 /Volumes 에 뜨는데, /System 폴더가 있으면 시스템 디스크
        if (QFileInfo("/Volumes/" + name + "/System/Library").exists()) continue;

        QJsonObject o;
        o["path"] = path;
        o["name"] = name;
        o["isNetwork"] = networkMounts.contains(name);
        vols.append(o);
    }
#elif defined(Q_OS_WIN)
    // Windows: QStorageInfo 로 모든 마운트된 드라이브 열거. C: 시스템 드라이브 제외, UNC(\\server\share) 포함.
    QString sysRoot = QString::fromLocal8Bit(qgetenv("WINDIR")).left(2);  // 보통 "C:"
    for (const QStorageInfo &si : QStorageInfo::mountedVolumes()) {
        if (!si.isValid() || !si.isReady() || si.isReadOnly()) continue;
        QString root = si.rootPath();                                     // "C:/", "D:/", "//server/share"
        QString nativeRoot = QDir::toNativeSeparators(root);
        if (nativeRoot.length() >= 2 && nativeRoot.left(2).compare(sysRoot, Qt::CaseInsensitive) == 0) continue;
        QString name = si.displayName();
        if (name.trimmed().isEmpty()) name = nativeRoot;
        QString fsType = QString::fromUtf8(si.fileSystemType()).toLower();
        bool isNet = nativeRoot.startsWith("\\\\")                        // UNC 경로
                     || fsType.contains("smb") || fsType.contains("nfs") || fsType.contains("webdav");
        QJsonObject o;
        o["path"] = root;
        o["name"] = name;
        o["isNetwork"] = isNet;
        vols.append(o);
    }
#endif
    QString json = QString::fromUtf8(QJsonDocument(vols).toJson(QJsonDocument::Compact));
    QStringList names;
    for (const auto &v : vols) names << v.toObject()["name"].toString();
    qDebug() << "[NAS] sending" << vols.size() << "volumes:" << json;
    log(QString("[NAS] %1개 마운트 감지: %2").arg(vols.size()).arg(names.join(", ")),
        "info", "settings");
    runJs(QString("if(window.onMountedVolumes) onMountedVolumes(%1);").arg(json));
}

// ═════════════════════════════════════════════════════════════════════════
// Qt native dialog 로 마운트된 볼륨 선택 → input 직접 갱신
// ═════════════════════════════════════════════════════════════════════════
#include <QInputDialog>
void MiyoBackend::pickMountedVolume(const QString &targetInputId)
{
    qDebug() << "[pickMountedVolume] called targetInputId=" << targetInputId;
    log(QString("[NAS] pickMountedVolume(%1) 호출됨").arg(targetInputId), "info", "settings");
    QStringList paths;
    QStringList items;
#ifdef Q_OS_MACOS
    // 1) /Volumes 마운트 (Apple WebDAV / SMB / AFP / NFS / 외장)
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
    // 2) Mountain Duck / Cyberduck / CloudStorage
    QStringList duckRoots = {
        QDir::homePath() + "/Mountain Duck",
        QDir::homePath() + "/Library/Group Containers/G69SCX94XU.duck/Library/Application Support/Cyberduck/Drive",
        QDir::homePath() + "/Library/Containers/io.mountainduck/Data/Library/Application Support/Mountain Duck/Volumes.noindex",
        QDir::homePath() + "/Library/CloudStorage"
    };
    for (const QString &duckRoot : duckRoots) {
        if (!QDir(duckRoot).exists()) continue;
        QDir dr(duckRoot);
        const QStringList connections = dr.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        for (const QString &conn : connections) {
            QString path = duckRoot + "/" + conn;
            if (!QFileInfo(path).isDir()) continue;
            QString displayName = conn;
            if (displayName.endsWith(".localized")) displayName.chop(QString(".localized").size());
            paths << path;
            items << QString("🦆 %1  (Mountain Duck — %2)").arg(displayName, path);
        }
    }
#endif
    // 3) 항상 직접 선택 옵션 — Default 로 맨 위에 (사용자가 안 만져도 안전)
    paths.prepend("__MANUAL__");
    items.prepend("📂 직접 폴더 선택 (Finder dialog — Mountain Duck/NAS/외장 어디든)");

    qDebug() << "[pickMountedVolume] items=" << items.size();

    QMetaObject::invokeMethod(this, [this, items, paths, targetInputId]() {
        qDebug() << "[pickMountedVolume] showing volume dialog with" << items.size() << "items";
        if (m_window) {
            m_window->raise();
            m_window->activateWindow();
        }
        // 1단계: 볼륨 / 직접 선택
        bool ok = false;
        QString chosen = QInputDialog::getItem(m_window, "저장 경로 — 1단계: 볼륨 선택",
            QString("저장할 NAS/외장/Mountain Duck (또는 직접 선택):"),
            items, 0, false, &ok);
        qDebug() << "[pickMountedVolume] volume dialog returned ok=" << ok << "chosen=" << chosen;
        if (!ok) { log("저장 경로 선택 취소됨", "info", "settings"); return; }
        if (chosen.isEmpty()) chosen = items.first();
        int idx = items.indexOf(chosen);
        if (idx < 0) idx = 0;
        QString volumeRoot = paths[idx];

        bool manualMode = (volumeRoot == "__MANUAL__");
        QString startDir = manualMode ? QDir::homePath() : volumeRoot;

        // 2단계: Finder dialog
        QString chosenPath = QFileDialog::getExistingDirectory(
            m_window,
            manualMode
                ? "저장 경로 — 직접 선택 (어디든 navigate)"
                : "저장 경로 — 2단계: 폴더 선택 (없으면 새로 만드세요)",
            startDir,
            QFileDialog::ShowDirsOnly | QFileDialog::DontResolveSymlinks
        );
        if (chosenPath.isEmpty()) return;
        // manual mode 가 아니면 볼륨 안 검증
        if (!manualMode && !chosenPath.startsWith(volumeRoot)) {
            log(QString("⚠ 선택한 폴더가 %1 안에 없음 — 무시").arg(volumeRoot), "warning", "settings");
            return;
        }
        QDir().mkpath(chosenPath);
        log(QString("[저장경로] 선택됨: %1 → %2").arg(targetInputId, chosenPath), "success", "settings");
        // input 값 직접 갱신 + change 이벤트
        QString js = QString(
            "(function(){"
            " var el = document.getElementById(%1);"
            " if (el) { el.value = %2; "
            "   el.dispatchEvent(new Event('change'));"
            "   el.dispatchEvent(new Event('input'));"
            "   el.style.transition = 'background 0.3s';"
            "   el.style.background = '#1f3a1f';"
            "   setTimeout(function(){el.style.background='';}, 600);"
            " }"
            "})();"
        ).arg(
            Common::jsStringLiteral(targetInputId),
            Common::jsStringLiteral(chosenPath)
        );
        runJs(js);
    }, Qt::QueuedConnection);
}

void MiyoBackend::showSystemNotification(const QString &title, const QString &body)
{
#ifdef Q_OS_MACOS
    // 작은따옴표 escape
    QString safeTitle = title; safeTitle.replace("\"", "\\\"");
    QString safeBody  = body;  safeBody.replace("\"", "\\\"");
    QString script = QString(
        "display notification \"%1\" with title \"%2\" sound name \"Glass\""
    ).arg(safeBody, safeTitle);
    QProcess::startDetached("osascript", {"-e", script});
#elif defined(Q_OS_WIN)
    // Windows: PowerShell BurntToast 또는 systray balloon — 간단히 systray 활용
    Q_UNUSED(title); Q_UNUSED(body);
    // (구현 미정 — Windows 사용자용 fallback은 GUI 로그로 충분)
#endif
}

void MiyoBackend::checkNewPosts(const QString &platformName)
{
    if (platformName == "twitter") {
        if (!m_twitterCollector) {
            log("먼저 수집을 실행하세요", "warning", "twitter");
            return;
        }
        if (m_twitterCollector->newestTweetId().isEmpty()) {
            log("아직 수집된 트윗이 없습니다", "warning", "twitter");
            return;
        }
        // 이미 실행 중이면 무시 (동시 접근 방지)
        if (m_isRunning.value("twitter", false)) {
            log("이미 실행 중입니다", "warning", "twitter");
            return;
        }
        QJsonObject config = m_lastConfig.value("twitter");
        if (config.isEmpty()) {
            log("설정 정보 없음", "warning", "twitter");
            return;
        }
        m_isRunning["twitter"] = true;
        QThread *thread = QThread::create([this, config]() {
            // m_isRunning["twitter"]를 직접 참조 (QMap value ref는 재할당 안 하면 안정)
            m_twitterCollector->checkNewPosts(config, m_isRunning["twitter"]);
            m_isRunning["twitter"] = false;
            QMetaObject::invokeMethod(this, [this]() {
                updateStats(0, 0, "완료", "twitter");
            }, Qt::QueuedConnection);
        });
        connect(thread, &QThread::finished, thread, &QThread::deleteLater);
        thread->start();
    } else if (platformName == "bluesky") {
        log("새 글 체크 — 블루스카이는 아직 미구현", "warning", "bluesky");
    }
}

void MiyoBackend::startYoutube(const QString &configJson)
{
    QJsonDocument doc = QJsonDocument::fromJson(configJson.toUtf8());
    if (doc.isNull()) return;

    QJsonObject config = doc.object();
    m_isRunning["youtube"] = true;

    if (m_window) m_window->holdAwake();

    // ★ YouTube 는 yt-dlp 가 자체 터미널 (miyo_yt_download.command) 띄움 — 우리 tail 안 띄움.
    //   대신 m_terminalLogPaths 에 dummy 등록만 (writeTerminalLog 의 fallback skip 위해)
    //   yt-dlp 자체 출력은 yt-dlp script 가 직접 stdout 으로 보여줌.
    QString ytSavePath = config["path"].toString();
    Q_UNUSED(ytSavePath);
    // 의도적으로 openTerminalLog 호출 안 함 — 터미널 2개 뜨는 거 방지

    QThread *thread = QThread::create([this, config]() {
        runYoutubeDownload(config);
        // 완료 처리 — 메인 스레드에서 실행 (QProcess/QSocketNotifier는 cross-thread 접근 불가)
        QMetaObject::invokeMethod(this, [this]() {
            m_isRunning["youtube"] = false;
            if (!isAnyRunning() && m_window) m_window->releaseAwake();
        });
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void MiyoBackend::stopYoutube()
{
    m_isRunning["youtube"] = false;
    // Signal terminal script to stop — 마지막 config의 path에서 찾기
    QString ytPath = m_lastConfig.value("youtube")["path"].toString();
    ytPath.replace("~", QDir::homePath());
    QString ytBaseDir = ytPath + "/youtube";
    // ★ 시스템 /tmp 안 씀 — 사용자가 지정한 임시 디스크 사용 (없으면 ytBaseDir)
    QString tempDir = ytPath.isEmpty()
        ? Common::resolveTempBase(m_config ? m_config->tempDir() : QString()) + "/abiwa_yt"
        : ytBaseDir + "/.abiwa_tmp";
    QFile stopFile(tempDir + "/miyo_yt_status.txt.stop");
    if (stopFile.open(QIODevice::WriteOnly)) {
        stopFile.write("STOP");
        stopFile.close();
    }

    // ── yt-dlp 프로세스 직접 kill (즉시 중단) ──
#ifdef Q_OS_WIN
    // Windows: taskkill로 yt-dlp, ffmpeg 즉시 종료
    QProcess::execute("taskkill", {"/F", "/IM", "yt-dlp.exe"});
    QProcess::execute("taskkill", {"/F", "/IM", "ffmpeg.exe"});
#else
    // macOS/Linux: pkill로 yt-dlp, ffmpeg 즉시 종료
    QProcess::execute("pkill", {"-f", "yt-dlp"});
    QProcess::execute("pkill", {"-f", "ffmpeg.*abiwa_tmp"});
    // 다운로드 스크립트도 종료
    QProcess::execute("pkill", {"-f", "miyo_yt_download.command"});
#endif

    // 상태 파일에 DONE 기록 → 모니터링 루프 즉시 탈출
    QString statusFile = tempDir + "/miyo_yt_status.txt";
    QFile sf(statusFile);
    if (sf.open(QIODevice::WriteOnly)) {
        sf.write("DONE:0:0");
        sf.close();
    }

    log("YouTube 다운로드 중지됨", "warning", "youtube");
    updateStats(0, 0, "중지됨", "youtube");
    runJs("setYoutubeProgress(0)");
}

void MiyoBackend::analyzeYoutube(const QString &url)
{
    QThread *thread = QThread::create([this, url]() {
        QStringList urls;
        for (const auto &line : url.split('\n')) {
            QString trimmed = line.trimmed();
            if (trimmed.startsWith("http")) urls.append(trimmed);
        }

        int totalVideos = 0;
        for (int i = 0; i < urls.size(); ++i) {
            log(QString("Analyzing [%1/%2]...").arg(i + 1).arg(urls.size()), "info", "youtube");

            QProcess proc;
            proc.setProcessEnvironment(bundledEnv());
            proc.start(findBundledTool("yt-dlp"), {"--flat-playlist", "--dump-json", "--no-warnings", urls[i]});

            // Read output incrementally to avoid timeout with large channels
            int count = 0;
            while (proc.waitForReadyRead(600000)) {
                while (proc.canReadLine()) {
                    QByteArray line = proc.readLine();
                    if (!line.trimmed().isEmpty()) {
                        count++;
                        if (count % 100 == 0)
                            log(QString("  %1 videos...").arg(count), "info", "youtube");
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
                log(QString("Found %1 videos").arg(count), "success", "youtube");
            } else {
                log("Analysis failed: " + QString::fromUtf8(proc.readAllStandardError()).left(60), "error", "youtube");
            }
        }
        log(QString("Total: %1 videos").arg(totalVideos), "info", "youtube");
        runJs(QString("onAnalyzeComplete(%1)").arg(totalVideos));
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// ═══════════════════════════════════════════════════════════════════════════
// 経済産業省 연계 — 실제 트위터 페이지 캡쳐 (per-tweet 브라우저 navigation)
// ═══════════════════════════════════════════════════════════════════════════
//
// 워커 스레드(TwitterCollector)에서 호출. 메인 스레드의 QWebEngine으로 tweetUrl을
// 로드하고 페이지가 완전히 렌더된 후 outerHTML을 가져와 saveDir/filename.html로 저장.
// 동시 호출은 m_realCaptureMutex로 직렬화 — 단일 브라우저 인스턴스라 동시 navigation 불가.
//
// 첫 호출에서만 cookies를 cookieStore에 주입한다 (이후엔 재사용 — 매번 주입하면 느림).

bool MiyoBackend::captureRealTweetPage(const QString &tweetUrl,
                                       const QString &saveDir,
                                       const QString &filename,
                                       const QList<QNetworkCookie> &cookies,
                                       int waitMs)
{
    if (tweetUrl.isEmpty() || saveDir.isEmpty()) return false;

    // 동시 캡쳐 직렬화
    QMutexLocker captureLock(&m_realCaptureMutex);

    QDir().mkpath(saveDir);
    QString filePath = saveDir + "/" + filename + ".html";
    if (QFile::exists(filePath)) return true;  // 이미 캡쳐됨

    // 메인 스레드에서 처리할 작업 스케줄 + 워커 스레드 차단
    auto done = std::make_shared<QSemaphore>(0);
    auto resultOk = std::make_shared<bool>(false);

    QMetaObject::invokeMethod(this, [this, tweetUrl, saveDir, filename, filePath,
                                       cookies, waitMs, done, resultOk]() {
        auto *bv = m_window ? m_window->browserView() : nullptr;
        if (!bv) {
            log("브라우저를 사용할 수 없어 캡쳐 스킵", "warning", "twitter");
            done->release();
            return;
        }
        // 브라우저 표시 — 안 그러면 일부 사이트가 invisible iframe으로 판단해서
        // throttle/lazy-render 거는 경우 있음. 사용자가 보는 것도 진짜로 페이지가
        // 뜨는지 확인 가능.
        showBrowser(true);
        log(QString("실제 캡쳐 navigate: %1").arg(tweetUrl), "info", "twitter");

        // 첫 캡쳐 시 쿠키 주입 (logged-in 컨텐츠 보장)
        //   cookieStore->setCookie는 비동기 — setCookie 직후 load(URL) 하면 cookie가
        //   아직 반영 안 된 상태로 navigate해서 logged-out 페이지를 받는다.
        //   → 주입했으면 800ms 대기 후 navigate.
        const bool injectedNow = (!m_realCaptureCookiesInjected && !cookies.isEmpty());
        if (injectedNow) {
            auto *cookieStore = bv->page()->profile()->cookieStore();
            for (const QNetworkCookie &c : cookies) {
                cookieStore->setCookie(c);
            }
            m_realCaptureCookiesInjected = true;
            log(QString("실제 캡쳐: 쿠키 %1개 주입 — 800ms 대기 후 navigate").arg(cookies.size()), "info", "twitter");
        }

        // 페이지 로드 — loadFinished 시그널 일회성 연결
        QMetaObject::Connection *conn = new QMetaObject::Connection;
        QTimer *fallbackTimer = new QTimer();
        fallbackTimer->setSingleShot(true);
        fallbackTimer->setInterval(waitMs + 15000);

        auto cleanup = [conn, fallbackTimer]() {
            QObject::disconnect(*conn);
            delete conn;
            fallbackTimer->stop();
            fallbackTimer->deleteLater();
        };

        auto onLoadDone = [this, bv, filePath, tweetUrl, waitMs, done, resultOk, cleanup](bool ok) {
            cleanup();
            if (!ok) {
                log("페이지 로드 실패 — 캡쳐 스킵", "warning", "twitter");
                done->release();
                return;
            }
            // navigator.webdriver = undefined 주입 — Twitter의 자동화 탐지 우회
            bv->page()->runJavaScript(
                "Object.defineProperty(navigator, 'webdriver', {get:()=>undefined}); true;");
            // 렌더 안정화 대기 후 outerHTML 추출 (twitter는 JS 렌더가 느려서 대기 필요)
            QTimer::singleShot(waitMs, this, [this, bv, filePath, tweetUrl, done, resultOk]() {
                bv->page()->toHtml([this, filePath, tweetUrl, done, resultOk](const QString &html) {
                    if (html.isEmpty()) {
                        log("HTML 캡쳐 실패 (빈 응답)", "warning", "twitter");
                        done->release();
                        return;
                    }
                    QString out = html;
                    QString metaTag = QString(
                        "\n<!-- ABIWA real Twitter capture (rendered DOM) -->\n"
                        "<!-- source: %1 -->\n"
                        "<!-- captured: %2 -->\n")
                        .arg(tweetUrl.toHtmlEscaped(),
                             QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
                    int headEnd = out.indexOf("</head>", 0, Qt::CaseInsensitive);
                    if (headEnd > 0) out.insert(headEnd, metaTag);
                    else out.prepend(metaTag);

                    QFile f(filePath);
                    if (f.open(QIODevice::WriteOnly)) {
                        f.write(out.toUtf8());
                        f.close();
                        FileHelper::setDownloadMeta(filePath, tweetUrl);
                        FileHelper::setFinderComment(filePath, tweetUrl);
                        *resultOk = true;
                    }
                    done->release();
                });
            });
        };

        *conn = connect(bv, &QWebEngineView::loadFinished, this, onLoadDone);
        connect(fallbackTimer, &QTimer::timeout, this, [this, done, cleanup]() {
            cleanup();
            log("페이지 로드 타임아웃 — 캡쳐 스킵", "warning", "twitter");
            done->release();
        });
        fallbackTimer->start();

        // 쿠키 주입 직후엔 800ms 대기 후 navigate (cookieStore가 비동기로 반영되도록)
        const int navDelay = injectedNow ? 800 : 0;
        QTimer::singleShot(navDelay, this, [bv, tweetUrl]() {
            bv->load(QUrl(tweetUrl));
        });
    }, Qt::QueuedConnection);

    // 안전망: waitMs + 60초 (느린 사이트 대비)
    if (!done->tryAcquire(1, waitMs + 60000 + 1000)) {  // +1000: cookie 대기분
        log("captureRealTweetPage 타임아웃", "warning", "twitter");
        return false;
    }
    return *resultOk;
}

// ═══════════════════════════════════════════════════════════════════════════
// CDP (실제 Chrome) 기반 페이지 캡쳐 — Twitter 등 봇 탐지가 강한 사이트용
// ═══════════════════════════════════════════════════════════════════════════
//
// QWebEngine으로 x.com 등을 방문하면 anti-bot이 "JavaScript is disabled" shell만 돌려준다.
// 실제 Chrome을 CDP로 조종하면 사용자의 로그인 세션 + 일반적인 fingerprint를 그대로 써서
// 정상 페이지가 받아진다. m_captureChrome 인스턴스를 한 번 띄우면 batch 내내 재사용.

bool MiyoBackend::captureRealPageCDP(const QString &url,
                                      const QString &saveDir,
                                      const QString &filename,
                                      int waitMs,
                                      const QList<QNetworkCookie> &cookies)
{
    if (url.isEmpty() || saveDir.isEmpty()) return false;

    QDir().mkpath(saveDir);
    QString filePath = saveDir + "/" + filename + ".html";
    if (QFile::exists(filePath)) return true;

    // ★ RAM 제한 — 동시 Chrome 인스턴스 1개로 제한 (8GB Mac OOM 방지).
    //   슬롯 확보까지 최대 30분 대기, 함수 끝에서 release.
    if (!m_chromeCapacitySem.tryAcquire(1, 30 * 60 * 1000)) {
        log("Chrome 캡쳐 슬롯 30분 대기 타임아웃 — 캡쳐 스킵", "warning", "twitter");
        return false;
    }
    struct CapacityGuard {
        QSemaphore *sem;
        ~CapacityGuard() { if (sem) sem->release(); }
    } _guard{&m_chromeCapacitySem};

    // ★ 병렬 캡쳐 복구 — 각 trackKey 마다 Chrome 인스턴스 + 고유 포트(9224+)
    //   m_captureChrome (port 9223) 은 LoginAware 도 같은 trackKey 의 per-track chrome 쓰니까
    //   사용 안 됨. 포트 충돌 없음.
    QString trackKey = currentThreadTrackKey();
    RealChromeCrawler **chromePtr = nullptr;
    QMutex *chromeMutex = nullptr;
    int debugPort = 9223;

    if (trackKey.isEmpty()) {
        chromePtr = &m_captureChrome;
        chromeMutex = &m_captureChromeMutex;
    } else {
        QMutexLocker mapLock(&m_capChromeMapMutex);
        if (!m_captureChromesPerThread.contains(trackKey)) {
            m_captureChromesPerThread[trackKey] = nullptr;  // placeholder
            debugPort = m_nextCapPort++;
        }
        chromePtr = &m_captureChromesPerThread[trackKey];
    }
    // sequential 모드만 직렬화 잠금 (병렬은 각자 자기 Chrome)
    std::unique_ptr<QMutexLocker<QMutex>> seqLock;
    if (chromeMutex) seqLock = std::make_unique<QMutexLocker<QMutex>>(chromeMutex);

    // ★ Chrome 인스턴스 동시 개수 제한은 위 line 1447 tryAcquire + CapacityGuard가 이미 처리함.
    //    중복 acquire하면 hang/튕김 발생 (sem 시작 2인데 호출당 2 차지 → 동시성 0).

    // 1) Chrome 초기화 (lazy) — chromePtr이 가리키는 인스턴스 (단일 또는 trackKey별).
    //    ★ 병렬 모드는 각자 다른 디버그 포트 → 동시에 Chrome 여러 개 실행, 각자 자기 캡쳐.
    //    프로필도 trackKey별로 분리 (~/Library/Application Support/ABIWA/chrome_capture_profile_<trackKey>)
    {
        auto needStart = std::make_shared<bool>(false);
        auto checkDone = std::make_shared<QSemaphore>(0);
        int port = debugPort;
        // suffix 미리 sanitize (lambda 안에서는 const라 .replace 못 함)
        QString tkSuffix;
        if (!trackKey.isEmpty()) {
            QString b64 = QString::fromLatin1(trackKey.toUtf8().toBase64());
            b64.replace("=", "").replace("/", "_").replace("+", "_");
            tkSuffix = "_" + b64;
        }
        QMetaObject::invokeMethod(this, [this, chromePtr, needStart, checkDone, port, tkSuffix]() {
            if (!*chromePtr) {
                *chromePtr = new RealChromeCrawler(this, this);
                (*chromePtr)->setUseUserProfile(false);
                (*chromePtr)->setDebugPort(port);
                if (!tkSuffix.isEmpty()) {
                    QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
                    QString perThreadDir = appData + "/chrome_capture_profile" + tkSuffix;
                    QDir().mkpath(perThreadDir);
                    (*chromePtr)->setUserDataDir(perThreadDir);
                }
            }
            *needStart = !(*chromePtr)->isReady();
            checkDone->release();
        }, Qt::QueuedConnection);
        checkDone->tryAcquire(1, 5000);

        if (*needStart) {
            log(QString("실제 Chrome 시작 (포트 %1%2)")
                .arg(port).arg(trackKey.isEmpty() ? "" : QString(", trackKey=%1").arg(trackKey)),
                "info", "twitter");
            auto startDone = std::make_shared<QSemaphore>(0);
            auto startOk = std::make_shared<bool>(false);
            QMetaObject::invokeMethod(this, [this, chromePtr, startDone, startOk]() {
                (*chromePtr)->start([this, startDone, startOk](bool ok) {
                    *startOk = ok;
                    if (!ok) log("실제 Chrome 시작 실패 — Chrome/Edge 설치 확인", "error", "twitter");
                    startDone->release();
                });
            }, Qt::QueuedConnection);
            if (!startDone->tryAcquire(1, 30000) || !*startOk) return false;
        }
    }

    // 1.5) ★ 로그인 쿠키 주입 (매 호출, idempotent) — safety 처리된 컨텐츠도 정상 로드.
    //       Chrome 시작 직후 쿠키 없는 상태로 navigate하면 Twitter가 NSFW/age-gated 페이지를 숨김.
    if (!cookies.isEmpty()) {
        QJsonArray cookieArr;
        for (const QNetworkCookie &c : cookies) {
            QJsonObject ck;
            ck["name"] = QString::fromUtf8(c.name());
            ck["value"] = QString::fromUtf8(c.value());
            QString d = c.domain();
            QString p = c.path().isEmpty() ? "/" : c.path();
            if (!d.isEmpty()) {
                ck["domain"] = d;
                // ★ Network.setCookie 일부 Chrome 버전: domain만 있으면 거부 → url 같이 줘야 적용
                QString cleanD = d.startsWith(".") ? d.mid(1) : d;
                ck["url"] = QString(c.isSecure() ? "https://" : "http://") + cleanD + p;
            }
            ck["path"] = p;
            ck["secure"] = c.isSecure();
            ck["httpOnly"] = c.isHttpOnly();
            // sameSite 없으면 일부 사이트가 거부 — 기본 None (cross-site OK)
            ck["sameSite"] = "None";
            cookieArr.append(ck);
        }
        auto cookieDone = std::make_shared<QSemaphore>(0);
        QMetaObject::invokeMethod(this, [this, chromePtr, cookieArr, cookieDone]() {
            (*chromePtr)->setCookies(cookieArr, [cookieDone](bool){ cookieDone->release(); });
        }, Qt::QueuedConnection);
        cookieDone->tryAcquire(1, 5000);  // 쿠키 주입 5초 안에 끝남 보장
    }

    // 2) navigate → 렌더 대기 → SingleFile 라이브러리 주입 → getPageData() 호출 → HTML 저장.
    //    Chrome.commands API는 합성 키이벤트를 받지 않는 경우가 많아서 키보드 단축키 대신 직접 주입.
    auto done = std::make_shared<QSemaphore>(0);
    auto resultOk = std::make_shared<bool>(false);

    // SingleFile lib 코드 미리 로드 (캐시: 첫 캡쳐에서만 디스크 읽음)
    static QString s_sfLibCode;
    if (s_sfLibCode.isEmpty()) {
        QString sfLibPath = Common::bundledToolsDir() + "/singlefile_extension/lib/single-file.js";
        if (!QFile::exists(sfLibPath)) {
            sfLibPath = QCoreApplication::applicationDirPath() +
                "/../../resources/tools/singlefile_extension/lib/single-file.js";
        }
        QFile lib(sfLibPath);
        if (lib.open(QIODevice::ReadOnly)) {
            s_sfLibCode = QString::fromUtf8(lib.readAll());
            lib.close();
        }
    }
    if (s_sfLibCode.isEmpty()) {
        log("SingleFile lib 로드 실패 — captureRealPageCDP 스킵", "error", "twitter");
        return false;
    }

    QString libCode = s_sfLibCode;
    QMetaObject::invokeMethod(this, [this, chromePtr, url, filePath, libCode, waitMs, done, resultOk]() {
        (*chromePtr)->navigate(url, [this, chromePtr, filePath, url, libCode, waitMs, done, resultOk](bool navOk) {
            if (!navOk) {
                log("CDP navigate 실패 — 캡쳐 스킵", "warning", "twitter");
                done->release();
                return;
            }
            // ★ 고정 waitMs 대신 readyState complete + min(waitMs, 8s) 안정화로 동적 대기
            QString readyJs = QString(R"JS(
                (async () => {
                    const sleep = ms => new Promise(r => setTimeout(r, ms));
                    for (let i = 0; i < 80; i++) {
                        if (document.readyState === 'complete') break;
                        await sleep(100);
                    }
                    await sleep(%1);  // 추가 안정화
                    return document.readyState;
                })()
            )JS").arg(qMin(waitMs, 3000));
            (*chromePtr)->evaluate(readyJs, [this, chromePtr, filePath, url, libCode, done, resultOk](const QJsonValue &) {
                // 원래 QTimer::singleShot 람다 내부 그대로 호출 — 매크로 이어짐
                // ★ 단순 캡쳐 — 끝까지 스크롤 + 댓글 펼치기. virtualized DOM 재구성 제거 (메모리↓)
                //   사용자 의도: "그냥 캡쳐만 해서 다운하는 방식. 댓글까지 전부 보이게"
                QString scrollJs = R"JS(
                    (async () => {
                        const sleep = ms => new Promise(r => setTimeout(r, ms));
                        // 1) 끝까지 스크롤 (lazy-load + 댓글 트리거)
                        let prevH = 0, same = 0;
                        for (let i = 0; i < 80; i++) {
                            window.scrollTo(0, document.body.scrollHeight);
                            await sleep(700);
                            const curH = document.body.scrollHeight;
                            if (curH === prevH) { if (++same >= 3) break; }
                            else { same = 0; prevH = curH; }
                        }
                        // 2) "더 보기" 버튼 자동 클릭 (댓글 펼치기)
                        try {
                            document.querySelectorAll('button, [role="button"]').forEach(b => {
                                const t = (b.textContent || '').trim();
                                if (t.match(/^(더 보기|더보기|모두 보기|모두보기|Show more|See more|See all|View all|Load more|More replies|댓글|답글|もっと見る)/i)) {
                                    try { b.click(); } catch(e){}
                                }
                            });
                        } catch(e){}
                        await sleep(1000);
                        // 3) 다시 한 번 끝까지 (펼친 댓글 lazy-load)
                        for (let i = 0; i < 30; i++) {
                            window.scrollTo(0, document.body.scrollHeight);
                            await sleep(500);
                        }
                        // 4) 페이지 맨 위 (캡쳐 상태 정상화)
                        window.scrollTo(0, 0);
                        await sleep(500);
                        return 1;
                    })()
                )JS";
                (*chromePtr)->evaluate(scrollJs, [this, chromePtr, filePath, url, libCode, done, resultOk](const QJsonValue &v) {
                    log(QString("[캡쳐] virtualized 스크롤 완료 — article %1개 정적 DOM 재구성").arg(v.toInt()), "info", "twitter");

                    // ★ 트위터의 절전 모드 / sensitive overlay / grayscale/blur 필터 강제 제거
                    //   계속 스크롤하면 트위터가 페이지에 grayscale 필터 거는 경우 있음 (사용자 캡쳐 흑백 현상)
                    QString clearFiltersJs = R"JS(
                        (() => {
                            // 1) sensitive media overlay 클릭으로 풀기
                            try {
                                document.querySelectorAll(
                                    '[data-testid="sensitive-media-warning"] button,'
                                    + ' button[data-testid="sensitiveMediaButton"],'
                                    + ' [data-testid="sensitiveMediaButton"]'
                                ).forEach(b => { try { b.click(); } catch(e){} });
                            } catch(e){}
                            // 2) 인라인 style + computed style의 grayscale/blur 필터 제거
                            const all = document.querySelectorAll('*');
                            for (const el of all) {
                                try {
                                    const cs = window.getComputedStyle(el);
                                    const f = cs && cs.filter;
                                    if (f && f !== 'none' && /grayscale|blur|invert|sepia/i.test(f)) {
                                        el.style.setProperty('filter', 'none', 'important');
                                        el.style.setProperty('-webkit-filter', 'none', 'important');
                                    }
                                    if (el.style && el.style.filter && /grayscale|blur|invert|sepia/i.test(el.style.filter)) {
                                        el.style.filter = 'none';
                                    }
                                } catch(e){}
                            }
                            // 3) <style>/CSS rule 안의 grayscale/blur도 정리 (sheet 단위)
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
                            // 4) html/body에 직접 박힌 클래스로 인한 grayscale 처리 (트위터 절전 모드 대비)
                            document.documentElement.style.filter = 'none';
                            document.body.style.filter = 'none';
                            return true;
                        })()
                    )JS";
                    (*chromePtr)->evaluate(clearFiltersJs, [this, chromePtr, filePath, url, libCode, done, resultOk](const QJsonValue &) {
                    log("[캡쳐] grayscale/blur 필터 정리 완료", "info", "twitter");
                    // 1) SingleFile lib 주입
                    (*chromePtr)->evaluate(libCode, [this, chromePtr, filePath, url, done, resultOk](const QJsonValue &) {
                    // 2) singlefile.getPageData() 호출 (await Promise)
                    QString call = R"JS(
                        (async () => {
                            if (typeof singlefile === 'undefined' || !singlefile.getPageData) {
                                return {error: 'SingleFile lib not loaded'};
                            }
                            try {
                                // ★ "웹 페이지에서 본 그대로" — DOM/CSS/이미지/iframe 그대로 보존
                                //   스크립트만 보안 + redirect 방지로 제거. 압축/최적화 안 함.
                                // ★ 사이즈 최소화 — 한 페이지 결과 HTML 100MB+ → 8GB Mac OOM 크래시 원인.
                                //   사용 안 하는 CSS/폰트/숨김 요소/광고 iframe 제거. 보이는 컨텐츠는 그대로.
                                const data = await singlefile.getPageData({
                                    removeHiddenElements: true,   // 안 보이는 DOM 제거 (광고/숨김 메뉴)
                                    removeUnusedStyles: true,     // 현재 DOM 안 쓰는 CSS rule 제거 (보통 70%↓)
                                    removeUnusedFonts: true,      // 안 쓰는 폰트 변형 제거 (base64 큰 거)
                                    removeFrames: true,           // iframe 제거 (광고/추적기 — 페이지 본문엔 영향 X)
                                    removeImports: true,
                                    removeScripts: true,          // 보안
                                    removeAudioSrc: false,
                                    removeVideoSrc: false,
                                    saveRawPage: false,
                                    insertSingleFileComment: true,
                                    insertMetaCSP: false,
                                    blockMixedContent: false,
                                    blockScripts: true,           // redirect 방지
                                    blockAudios: false,
                                    blockVideos: false,
                                    backgroundSave: false,
                                    compressHTML: true            // HTML minify (가독성↓ 사이즈↓ 30%)
                                });
                                let html = data.content;
                                // 후처리: meta refresh / X-UA-Compatible 등 자동 새로고침 유발 태그 제거
                                html = html.replace(/<meta[^>]+http-equiv\s*=\s*["']?refresh["']?[^>]*>/gi, '');
                                // <noscript> 안의 자동 redirect 제거
                                html = html.replace(/<noscript[^>]*>[\s\S]*?<\/noscript>/gi, '');
                                // service worker register 흔적 제거 (script removed지만 안전망)
                                html = html.replace(/navigator\.serviceWorker[^;]*;?/g, '');
                                // ★ 큰 HTML은 메모리로 한 번에 반환하면 8GB Mac에서 OOM 크래시.
                                //   JS global에 저장하고 size만 반환 → C++가 청크 단위로 가져가 디스크에 직접 write.
                                window.__capContent = html;
                                return {size: html.length, url: data.url || location.href, title: data.title || document.title, chunked: true};
                            } catch (e) {
                                return {error: String(e && e.message || e)};
                            }
                        })()
                    )JS";
                    (*chromePtr)->evaluate(call, [this, chromePtr, filePath, url, done, resultOk](const QJsonValue &v) {
                        QJsonObject obj = v.toObject();
                        if (obj.contains("error")) {
                            log(QString("[SingleFile] 호출 오류: %1").arg(obj["error"].toString()), "warning", "twitter");
                            done->release();
                            return;
                        }
                        // ★ chunked mode — JS global window.__capContent 에 HTML 저장됨. size만 받음.
                        //   8GB Mac OOM 방지: 1MB씩 가져와서 바로 디스크에 추가 쓰기.
                        if (obj["chunked"].toBool()) {
                            qint64 totalSize = obj["size"].toVariant().toLongLong();
                            obj = QJsonObject();
                            if (totalSize <= 0) {
                                log("[SingleFile] 빈 결과", "warning", "twitter");
                                done->release();
                                return;
                            }
                            // ★ NAS/외장 path 면 사용자 임시 디스크에 먼저 쓰고 → 끝나면 NAS 로 cp.
                            QString writeTarget = filePath;
                            bool useLocalTemp = filePath.startsWith("/Volumes/");
                            if (useLocalTemp) {
                                QString baseTemp = Common::resolveTempBase(m_config ? m_config->tempDir() : QString());
                                if (baseTemp.isEmpty()) {
                                    log("⚠ 임시 디스크 미설정 — 설정 → 디스크 설정에서 경로 지정 필요", "error", "twitter");
                                    done->release();
                                    return;
                                }
                                QString tmpDir = baseTemp + "/.abiwa_cap";
                                QDir().mkpath(tmpDir);
                                writeTarget = tmpDir + "/" +
                                    QString::number(QDateTime::currentMSecsSinceEpoch()) + "_" +
                                    QFileInfo(filePath).fileName();
                            } else {
                                QDir().mkpath(QFileInfo(filePath).absolutePath());
                            }
                            QFile *outFile = new QFile(writeTarget);
                            if (!outFile->open(QIODevice::WriteOnly)) {
                                log(QString("[SingleFile] 파일 열기 실패: %1 (target=%2)")
                                    .arg(writeTarget, filePath), "error", "twitter");
                                delete outFile;
                                done->release();
                                return;
                            }
                            const qint64 CHUNK = 1024 * 1024;  // 1MB
                            auto offsetPtr = std::make_shared<qint64>(0);
                            auto fetchNext = std::make_shared<std::function<void()>>();
                            *fetchNext = [this, chromePtr, outFile, totalSize, offsetPtr, fetchNext, filePath, writeTarget, useLocalTemp, url, done, resultOk]() {
                                if (*offsetPtr >= totalSize) {
                                    outFile->close();
                                    delete outFile;
                                    // JS global 해제
                                    (*chromePtr)->evaluate("delete window.__capContent; true;", [](const QJsonValue &){});

                                    // ★ 로컬 temp 에 쓴 경우 → NAS 로 cp + 로컬 삭제
                                    bool finalOk = true;
                                    if (useLocalTemp) {
                                        QDir().mkpath(QFileInfo(filePath).absolutePath());
                                        QFile::remove(filePath);
                                        if (!QFile::copy(writeTarget, filePath)) {
                                            log(QString("[SingleFile] ⚠ NAS 복사 실패 (로컬에 남김): %1").arg(writeTarget),
                                                "warning", "twitter");
                                            finalOk = false;
                                        } else {
                                            QFile::remove(writeTarget);  // 로컬 temp 삭제
                                        }
                                    }
                                    FileHelper::setDownloadMeta(filePath, url);
                                    FileHelper::setFinderComment(filePath, url);
                                    *resultOk = finalOk;
                                    log(QString("[SingleFile] ✅ 캡쳐 완료: %1 (%2KB)")
                                        .arg(QFileInfo(filePath).fileName())
                                        .arg(totalSize / 1024), "success", "twitter");
                                    enqueueWebDavUpload(filePath);
                                    enqueueBackup(filePath);

                                    // ★ N회 캡쳐마다 Chrome 재시작 — 메모리 누수 방지 (60+개 안정성)
                                    static const int CHROME_REUSE_LIMIT = 25;
                                    QString tk = currentThreadTrackKey();
                                    int cnt = ++m_captureCountsPerKey[tk];
                                    if (cnt >= CHROME_REUSE_LIMIT) {
                                        log(QString("Chrome %1회 재사용 — 메모리 청소 위해 재시작").arg(cnt),
                                            "info", "twitter");
                                        m_captureCountsPerKey[tk] = 0;
                                        if (*chromePtr) {
                                            (*chromePtr)->stop();
                                            (*chromePtr)->deleteLater();
                                            *chromePtr = nullptr;
                                        }
                                    }
                                    done->release();
                                    return;
                                }
                                qint64 end = qMin(*offsetPtr + CHUNK, totalSize);
                                QString slice = QString("window.__capContent.slice(%1, %2)").arg(*offsetPtr).arg(end);
                                (*chromePtr)->evaluate(slice, [outFile, offsetPtr, fetchNext, end](const QJsonValue &cv) {
                                    QByteArray bytes = cv.toString().toUtf8();
                                    outFile->write(bytes);
                                    bytes.clear();
                                    *offsetPtr = end;
                                    (*fetchNext)();
                                });
                            };
                            (*fetchNext)();
                            return;
                        }
                        // 옛 path (chunked=false) — 그대로 메모리에 받기 (fallback)
                        QString content = obj["content"].toString();
                        obj = QJsonObject();
                        if (content.isEmpty()) {
                            log("[SingleFile] 빈 결과", "warning", "twitter");
                            done->release();
                            return;
                        }
                        // disk stream write — content.toUtf8() 임시 사본 회피
                        QFile f(filePath);
                        if (f.open(QIODevice::WriteOnly)) {
                            QByteArray bytes = content.toUtf8();
                            int sz = bytes.size();
                            f.write(bytes);
                            f.close();
                            bytes.clear();          // 즉시 해제
                            content = QString();    // 큰 string 즉시 해제
                            FileHelper::setDownloadMeta(filePath, url);
                            FileHelper::setFinderComment(filePath, url);
                            *resultOk = true;
                            log(QString("[SingleFile] ✅ 캡쳐 완료: %1 (%2KB)")
                                .arg(QFileInfo(filePath).fileName())
                                .arg(sz / 1024), "success", "twitter");
                        }
                        // ★ chrome 메모리 해제는 다음 capture 시작 시 navigate가 덮어씀.
                        //   (이전엔 about:blank 호출로 즉시 해제 시도 → lambda chromePtr capture 누락 빌드 에러 → 제거)
                        done->release();
                    });
                });   // close evaluate(libCode)
                });   // close evaluate(clearFiltersJs)
                });   // close evaluate(scrollJs)
            });   // close evaluate(readyJs) — readyState 동적 대기 wrapper
        });   // close navigate
    }, Qt::QueuedConnection);

    if (!done->tryAcquire(1, waitMs + 120000)) {  // +120s: 스크롤 lazy-load 시간 확보
        log("captureRealPageCDP 타임아웃", "warning", "twitter");
        return false;
    }
    return *resultOk;
}

// ═════════════════════════════════════════════════════════════════════════
// cookiesForCapture — 플랫폼별 저장된 계정 정보로 캡쳐 chrome 로그인 쿠키 생성.
//   각 캡쳐 호출 시 captureRealPageCDPLoginAware 가 자동으로 호출해서
//   기존 cookies 인자와 병합 → 사용자는 매번 따로 쿠키 빌드 안 해도 됨.
// ═════════════════════════════════════════════════════════════════════════
QList<QNetworkCookie> MiyoBackend::cookiesForCapture(const QString &platform, const QJsonObject &config) const
{
    QList<QNetworkCookie> out;
    if (platform.isEmpty()) return out;

    auto pushCk = [&out](const QByteArray &name, const QString &val, const QString &domain) {
        if (val.isEmpty()) return;
        QNetworkCookie c(name, val.toUtf8());
        c.setDomain(domain); c.setPath("/"); c.setSecure(true);
        out << c;
    };
    auto pushRawCookieString = [&pushCk](const QString &raw, const QString &domain) {
        if (raw.isEmpty()) return;
        for (const QString &part : raw.split(';', Qt::SkipEmptyParts)) {
            int eq = part.indexOf('=');
            if (eq <= 0) continue;
            pushCk(part.left(eq).trimmed().toUtf8(), part.mid(eq + 1).trimmed(), domain);
        }
    };

    QJsonArray accs = config["accounts"].toArray();
    QJsonObject acct = accs.isEmpty() ? QJsonObject() : accs.first().toObject();
    QString extra = config["captureCookie"].toString();

    if (platform == "twitter") {
        QString at  = acct["auth_token"].toString(); if (at.isEmpty())  at  = config["auth_token"].toString();
        QString ct0 = acct["ct0"].toString();        if (ct0.isEmpty()) ct0 = config["ct0"].toString();
        pushCk("auth_token", at,  ".x.com");
        pushCk("auth_token", at,  ".twitter.com");
        pushCk("ct0",        ct0, ".x.com");
        pushCk("ct0",        ct0, ".twitter.com");
        pushRawCookieString(extra, ".x.com");
        pushRawCookieString(extra, ".twitter.com");
    } else if (platform == "instagram") {
        QString sid = acct["sessionId"].toString();
        if (sid.isEmpty()) sid = config["sessionId"].toString();
        pushCk("sessionid", sid, ".instagram.com");
        pushRawCookieString(extra, ".instagram.com");
    } else if (platform == "pixiv") {
        QString sid = acct["sessionId"].toString();
        if (sid.isEmpty()) sid = config["sessionId"].toString();
        pushCk("PHPSESSID", sid, ".pixiv.net");
        QString px = config["extraCookie"].toString();
        if (!px.isEmpty()) pushRawCookieString(px, ".pixiv.net");
        if (!extra.isEmpty()) pushRawCookieString(extra, ".pixiv.net");
    } else if (platform == "fanbox") {
        // Fanbox 는 pixiv 계정으로 인증 — FANBOXSESSID + PHPSESSID 둘 다 시도
        QString fsid = config["sessionId"].toString();
        if (fsid.isEmpty()) fsid = acct["sessionId"].toString();
        pushCk("FANBOXSESSID", fsid, ".fanbox.cc");
        QString phpSid = acct["pixivSession"].toString();
        if (phpSid.isEmpty()) phpSid = config["pixivSession"].toString();
        pushCk("PHPSESSID", phpSid, ".pixiv.net");
        pushRawCookieString(extra, ".fanbox.cc");
    } else if (platform == "tumblr") {
        QString raw = acct["cookie"].toString();
        if (raw.isEmpty()) raw = config["cookie"].toString();
        pushRawCookieString(raw, ".tumblr.com");
        if (!extra.isEmpty()) pushRawCookieString(extra, ".tumblr.com");
    } else if (platform == "spinspin") {
        QString raw = acct["cookie"].toString();
        if (raw.isEmpty()) raw = config["cookie"].toString();
        pushRawCookieString(raw, ".spin-spin.com");
        if (!extra.isEmpty()) pushRawCookieString(extra, ".spin-spin.com");
    } else if (platform == "asked") {
        QString raw = acct["cookie"].toString();
        if (raw.isEmpty()) raw = config["cookie"].toString();
        pushRawCookieString(raw, ".asked.kr");
        if (!extra.isEmpty()) pushRawCookieString(extra, ".asked.kr");
    } else if (platform == "discord") {
        QString token = acct["token"].toString();
        if (token.isEmpty()) token = config["token"].toString();
        pushCk("__Secure-recent_mfa", token, ".discord.com"); // 무해 — 실패해도 무시
        pushRawCookieString(extra, ".discord.com");
    } else if (platform == "bluesky") {
        // Bluesky 는 access token JWT 방식 — 별도 raw cookie 입력 외 자동 추출 없음
        QString raw = acct["cookie"].toString();
        if (raw.isEmpty()) raw = config["cookie"].toString();
        pushRawCookieString(raw, ".bsky.app");
        if (!extra.isEmpty()) pushRawCookieString(extra, ".bsky.app");
    } else if (platform == "crawl" || platform == "trad" || platform == "naikakukai") {
        // 범용 크롤 — UI 입력된 raw cookie 가 있으면 그대로 사용 (도메인 미정 → URL 도메인 기반)
        QString raw = config["cookie"].toString();
        QString d = config["cookieDomain"].toString();
        if (!d.isEmpty()) {
            pushRawCookieString(raw, d);
            if (!extra.isEmpty()) pushRawCookieString(extra, d);
        }
    }
    return out;
}

// ═════════════════════════════════════════════════════════════════════════
// captureRealPageCDPLoginAware — 로그인 페이지 감지 + 사용자 GUI 확인 대기 후 캡쳐
// ═════════════════════════════════════════════════════════════════════════
bool MiyoBackend::captureRealPageCDPLoginAware(const QString &url,
                                                const QString &saveDir,
                                                const QString &filename,
                                                const QString &loginCheckJs,
                                                const QString &platform,
                                                int waitMs,
                                                const QList<QNetworkCookie> &cookies,
                                                const QJsonObject &config)
{
    if (url.isEmpty() || saveDir.isEmpty()) return false;
    QString p = platform.isEmpty() ? "general" : platform;

    // ★ 캡쳐 시작 전 — 저장된 계정 쿠키 자동 주입 (전 플랫폼 공통).
    //   호출자가 추가 cookies 를 넘기면 그것도 병합 → 사용자 입력 쿠키 보존.
    //   계정 쿠키 먼저 → captureCookie 나중 (같은 이름이면 호출자 cookies 가 override 됨)
    QList<QNetworkCookie> mergedCookies = cookiesForCapture(p, config);
    mergedCookies.append(cookies);
    if (!mergedCookies.isEmpty() && mergedCookies.size() > cookies.size()) {
        log(QString("🔐 [%1] 캡쳐 시작 — 저장된 계정 쿠키 %2개 자동 주입 (로그인 상태 보장)")
            .arg(p).arg(mergedCookies.size() - cookies.size()),
            "info", p);
    }
    // 이하 함수 본문은 'cookies' 가 아닌 'mergedCookies' 를 사용하도록 변환.
    const QList<QNetworkCookie> &effectiveCookies = mergedCookies;

    // 1) 캡쳐 chrome 시작 보장 + cookie inject (captureRealPageCDP가 처음에 알아서 함)
    //    여긴 그냥 단발 navigate + 로그인 체크만 함.
    //    (captureRealPageCDP는 "이미 로그인된 상태"라는 가정 하에 그대로 호출)

    // RealChromeCrawler 통해 navigate + JS 평가 — 첫 호출이면 chrome 띄우기까지
    auto sem = std::make_shared<QSemaphore>(0);
    auto needsLogin = std::make_shared<bool>(false);
    auto navOk = std::make_shared<bool>(false);

    // ★ QNetworkCookie 리스트 → CDP setCookie 형식 JsonArray (메인 스레드에서 직접 사용)
    QJsonArray cookieArr;
    for (const QNetworkCookie &c : effectiveCookies) {
        QJsonObject ck;
        ck["name"] = QString::fromUtf8(c.name());
        ck["value"] = QString::fromUtf8(c.value());
        QString d = c.domain();
        QString pth = c.path().isEmpty() ? "/" : c.path();
        if (!d.isEmpty()) {
            ck["domain"] = d;
            QString cleanD = d.startsWith(".") ? d.mid(1) : d;
            ck["url"] = QString(c.isSecure() ? "https://" : "http://") + cleanD + pth;
        }
        ck["path"] = pth;
        ck["secure"] = c.isSecure();
        ck["httpOnly"] = c.isHttpOnly();
        ck["sameSite"] = "None";
        cookieArr.append(ck);
    }

    // ★ 병렬 안전 — trackKey 별 chrome 인스턴스 사용 (captureRealPageCDP 와 공유).
    //   trackKey 먼저 fetch — invokeMethod 후 메인 스레드에선 thread-local 못 읽음.
    QString trackKey = currentThreadTrackKey();
    int reservedPort = 9223;
    {
        QMutexLocker mapLock(&m_capChromeMapMutex);
        if (!trackKey.isEmpty()) {
            if (!m_captureChromesPerThread.contains(trackKey)) {
                m_captureChromesPerThread[trackKey] = nullptr;
                reservedPort = m_nextCapPort++;
            }
        }
    }

    QMetaObject::invokeMethod(this, [this, url, p, loginCheckJs, cookieArr, sem, needsLogin, navOk, trackKey, reservedPort]() {
        // chrome 인스턴스 결정 — trackKey 없으면 singleton, 있으면 per-track map
        auto getChromePtr = [this, trackKey]() -> RealChromeCrawler** {
            if (trackKey.isEmpty()) return &m_captureChrome;
            QMutexLocker mapLock(&m_capChromeMapMutex);
            return &m_captureChromesPerThread[trackKey];
        };
        auto chromePP = getChromePtr();

        auto afterNav = [this, loginCheckJs, sem, needsLogin, chromePP](bool nOk) {
            *needsLogin = false;
            if (!nOk) { sem->release(); return; }
            QString waitJs = R"JS(
                (async () => {
                    const sleep = ms => new Promise(r => setTimeout(r, ms));
                    for (let i = 0; i < 80; i++) {
                        if (document.readyState === 'complete') break;
                        await sleep(100);
                    }
                    await sleep(1500);
                    return true;
                })()
            )JS";
            if (!*chromePP) { sem->release(); return; }
            (*chromePP)->evaluate(waitJs, [chromePP, loginCheckJs, sem, needsLogin](const QJsonValue &) {
                if (!*chromePP) { sem->release(); return; }
                (*chromePP)->evaluate(loginCheckJs, [sem, needsLogin](const QJsonValue &v) {
                    *needsLogin = v.toBool();
                    sem->release();
                });
            });
        };

        auto doNavigate = [chromePP, url, sem, navOk, afterNav]() {
            if (!*chromePP) { sem->release(); return; }
            (*chromePP)->navigate(url, [sem, navOk, afterNav](bool nOk) {
                *navOk = nOk;
                afterNav(nOk);
            });
        };

        auto doCookiesThenNavigate = [this, chromePP, cookieArr, doNavigate, sem, p]() {
            if (cookieArr.isEmpty()) { doNavigate(); return; }
            if (!*chromePP) { sem->release(); return; }
            (*chromePP)->setCookies(cookieArr, [this, doNavigate, cookieArr, p](bool ok) {
                if (ok) log(QString("CDP 쿠키 %1개 주입 성공").arg(cookieArr.size()), "info", p);
                else log("CDP 쿠키 주입 실패 (계속 진행)", "warning", p);
                doNavigate();
            });
        };

        if (!*chromePP) {
            *chromePP = new RealChromeCrawler(this, this);
            (*chromePP)->setUseUserProfile(false);
            (*chromePP)->setDebugPort(reservedPort);
            if (!trackKey.isEmpty()) {
                QString b64 = QString::fromLatin1(trackKey.toUtf8().toBase64());
                b64.replace("=", "").replace("/", "_").replace("+", "_");
                QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
                QString perThreadDir = appData + "/chrome_capture_profile_" + b64;
                QDir().mkpath(perThreadDir);
                (*chromePP)->setUserDataDir(perThreadDir);
            }
            (*chromePP)->start([sem, doCookiesThenNavigate](bool ok) {
                if (!ok) { sem->release(); return; }
                doCookiesThenNavigate();
            });
            return;
        }
        doCookiesThenNavigate();
    }, Qt::QueuedConnection);

    if (!sem->tryAcquire(1, 60000)) {
        log("로그인 사전 검사 타임아웃", "warning", p);
        return false;
    }
    if (!*navOk) {
        log("로그인 사전 navigate 실패", "warning", p);
        return false;
    }

    // 2) 로그인 페이지면 사용자 대기
    if (*needsLogin) {
        log(QString("⚠ 로그인 필요 — Chrome 창에서 직접 로그인 후 우측 패널 '%1 로그인 확인' 버튼 누르세요").arg(p),
            "warning", p);
        // GUI 측에 platform별 배너 띄우기
        runJs(QString("if(window.onLoginPause) onLoginPause('%1');").arg(p));

        // platform별 sem 만들기
        QSemaphore *waitSem = nullptr;
        {
            QMutexLocker lock(&m_loginPauseMutex);
            waitSem = m_loginPauseSems.value(p, nullptr);
            if (!waitSem) {
                waitSem = new QSemaphore(0);
                m_loginPauseSems[p] = waitSem;
            }
        }
        // 사용자 confirm 대기 — 최대 1시간
        if (!waitSem->tryAcquire(1, 3600 * 1000)) {
            log("로그인 대기 타임아웃 (1시간)", "warning", p);
            runJs(QString("if(window.onLoginResume) onLoginResume('%1');").arg(p));
            return false;
        }
        runJs(QString("if(window.onLoginResume) onLoginResume('%1');").arg(p));
        log("로그인 확인됨 — 캡쳐 진행", "success", p);
    }

    // 3) 본 캡쳐 호출 (captureRealPageCDP가 navigate를 다시 해도 같은 페이지면 깜빡임 적음)
    return captureRealPageCDP(url, saveDir, filename, waitMs, effectiveCookies);
}

void MiyoBackend::confirmLoginDone(const QString &platform)
{
    QString p = platform.isEmpty() ? "general" : platform;
    QMutexLocker lock(&m_loginPauseMutex);
    QSemaphore *s = m_loginPauseSems.value(p, nullptr);
    if (s) {
        s->release();
        log(QString("로그인 확인 신호 수신: %1").arg(p), "info", p);
    }
}

void MiyoBackend::injectCdpCookies(const QList<QNetworkCookie> &cookies)
{
    if (cookies.isEmpty()) return;
    // QNetworkCookie 리스트 → CDP Network.setCookie 형식 JsonArray 변환
    QJsonArray arr;
    for (const QNetworkCookie &c : cookies) {
        QJsonObject ck;
        ck["name"] = QString::fromUtf8(c.name());
        ck["value"] = QString::fromUtf8(c.value());
        QString d = c.domain();
        QString p = c.path().isEmpty() ? "/" : c.path();
        if (!d.isEmpty()) {
            ck["domain"] = d;
            QString cleanD = d.startsWith(".") ? d.mid(1) : d;
            ck["url"] = QString(c.isSecure() ? "https://" : "http://") + cleanD + p;
        }
        ck["path"] = p;
        ck["secure"] = c.isSecure();
        ck["httpOnly"] = c.isHttpOnly();
        ck["sameSite"] = "None";
        arr.append(ck);
    }
    auto done = std::make_shared<QSemaphore>(0);
    auto okFlag = std::make_shared<bool>(false);
    QMetaObject::invokeMethod(this, [this, arr, done, okFlag]() {
        if (!m_captureChrome || !m_captureChrome->isReady()) {
            done->release();
            return;
        }
        m_captureChrome->setCookies(arr, [done, okFlag](bool ok) {
            *okFlag = ok;
            done->release();
        });
    }, Qt::QueuedConnection);
    done->tryAcquire(1, 5000);
    if (*okFlag) log(QString("CDP 쿠키 %1개 주입 성공").arg(cookies.size()), "info", "twitter");
    else log("CDP 쿠키 주입 실패", "warning", "twitter");
}

// ===== Web Crawl Engine (API 대체 수집 — 정책 변경 대비) =====

void MiyoBackend::runWebCrawlCollection(const QJsonObject &config)
{
    // ★ 워커 스레드 차단용 세마포어. 다중대상 시나리오에서 1번째 세션이 다 끝나기
    //    전에 워커 스레드가 리턴 → m_isRunning[platform]=false → 메인 스레드의 스크롤
    //    타이머가 첫 tick에서 빠져나감 → 아무것도 다운로드 안 됨.
    //    체인 끝(에러/정상완료/스크롤 종료)에서 release()를 부른다.
    auto crawlDone = std::make_shared<QSemaphore>(0);

    // QWebEngine 기반 웹 크롤 수집: 실제 브라우저로 페이지 로드 → JS로 스크롤 + 미디어 추출
    QString platform = config["platform"].toString();
    QString savePath = config["path"].toString();
    savePath.replace("~", QDir::homePath());

    // 플랫폼별 URL 생성
    QString targetUrl;
    QString target = config["target"].toString();
    if (platform == "twitter") {
        QString type = config["type"].toString("posts");
        if (type == "likes") targetUrl = QString("https://x.com/%1/likes").arg(target);
        else if (type == "following") targetUrl = QString("https://x.com/%1/following").arg(target);
        else if (type == "followers") targetUrl = QString("https://x.com/%1/followers").arg(target);
        else if (type == "media") targetUrl = QString("https://x.com/%1/media").arg(target);
        else if (type == "thread_comments") targetUrl = QString("https://x.com/%1").arg(target);
        else targetUrl = QString("https://x.com/%1").arg(target);
    } else if (platform == "bluesky") {
        if (target.contains("bsky.app"))
            targetUrl = target;
        else
            targetUrl = QString("https://bsky.app/profile/%1").arg(target);
    } else if (platform == "instagram") {
        targetUrl = QString("https://www.instagram.com/%1/").arg(config["username"].toString());
    } else if (platform == "pixiv") {
        targetUrl = QString("https://www.pixiv.net/users/%1").arg(target);
    } else if (platform == "discord") {
        targetUrl = QString("https://discord.com/channels/%1/%2").arg(
            config["server"].toString(), config["channel"].toString());
    } else if (platform == "tumblr") {
        targetUrl = QString("https://%1.tumblr.com/").arg(target);
    } else if (platform == "spinspin") {
        if (target.contains("spin-spin.com"))
            targetUrl = target;
        else
            targetUrl = QString("https://spin-spin.com/%1").arg(target);
    } else if (platform == "asked") {
        if (target.contains("asked.kr"))
            targetUrl = target;
        else
            targetUrl = QString("https://asked.kr/%1").arg(target);
    } else if (platform == "youtube") {
        if (target.startsWith("http"))
            targetUrl = target;
        else
            targetUrl = QString("https://www.youtube.com/@%1/videos").arg(target);
    }

    if (targetUrl.isEmpty()) {
        log("웹 크롤: 대상 URL을 생성할 수 없습니다", "error", platform);
        crawlDone->release();
        return;
    }

    log(QString("═══ 웹 크롤 모드 ═══"), "success", platform);
    log(QString("  URL: %1").arg(targetUrl), "info", platform);
    log("  브라우저 로드 중...", "info", platform);

    // ── 트위터 thread_comments 전용: 멀티스텝 크롤링 ──
    // (이 분기는 기존 비동기 콜백을 그대로 유지 — 워커 스레드는 30분 안전망 후 풀린다)
    if (platform == "twitter" && config["type"].toString() == "thread_comments") {
        log("게시물 댓글 크롤링 (웹 크롤 모드)...", "info", platform);

        QMetaObject::invokeMethod(this, [this, targetUrl, platform, savePath, config]() {
            auto *bv = m_window->browserView();
            if (!bv) { log("브라우저를 사용할 수 없습니다", "error", platform); return; }

            showBrowser(true);
            bv->load(QUrl(targetUrl));

            // State machine: Phase 1 = collect tweet URLs, Phase 2 = visit each tweet, collect replies
            auto *state = new QJsonObject();
            (*state)["phase"] = 1;  // 1: tweet URL 수집, 2: 답글 수집
            (*state)["scrollCount"] = 0;
            (*state)["tweetIdx"] = 0;
            (*state)["commentCount"] = 0;
            (*state)["mediaCount"] = 0;
            auto *tweetUrls = new QStringList();
            auto *collectedReplies = new QJsonArray();
            auto *mediaUrls = new QSet<QString>();
            auto *processedUrls = new QSet<QString>();

            auto *timer = new QTimer(this);
            timer->setInterval(2500);

            QString target = config["target"].toString();
            int maxScrolls = config["count"].toInt(0);
            if (maxScrolls <= 0) maxScrolls = 200;

            connect(timer, &QTimer::timeout, this, [=]() mutable {
                if (!m_isRunning.value(platform, false)) {
                    timer->stop(); timer->deleteLater();
                    delete state; delete tweetUrls; delete collectedReplies; delete mediaUrls; delete processedUrls;
                    return;
                }

                int phase = (*state)["phase"].toInt();

                if (phase == 1) {
                    // Phase 1: 프로필 페이지 스크롤하며 트윗 URL 수집
                    int sc = (*state)["scrollCount"].toInt();
                    if (sc >= maxScrolls / 5 + 30) {
                        // Phase 1 완료 → Phase 2
                        (*state)["phase"] = 2;
                        (*state)["tweetIdx"] = 0;
                        log(QString("Phase 1 완료: 트윗 %1개 발견").arg(tweetUrls->size()), "success", platform);
                        if (tweetUrls->isEmpty()) {
                            log("트윗을 찾지 못했습니다", "warning", platform);
                            timer->stop(); timer->deleteLater();
                            delete state; delete tweetUrls; delete collectedReplies; delete mediaUrls; delete processedUrls;
                            return;
                        }
                        // 첫 번째 트윗 페이지로 이동
                        bv->load(QUrl(tweetUrls->at(0)));
                        return;
                    }

                    (*state)["scrollCount"] = sc + 1;
                    updateStats(tweetUrls->size(), 0, QString("트윗 URL 수집 (스크롤 %1)").arg(sc + 1), platform);

                    // JS: 트윗 링크 추출 + 스크롤
                    QString js = QString(
                        "(function(){"
                        "  var urls=[];"
                        "  document.querySelectorAll('a[href*=\"/status/\"]').forEach(function(a){"
                        "    var h=a.href;"
                        "    if(h.match(/\\/status\\/\\d+$/) && h.includes('/%1/')) urls.push(h);"
                        "  });"
                        "  window.scrollBy(0, window.innerHeight * 2);"
                        "  return JSON.stringify([...new Set(urls)]);"
                        "})()"
                    ).arg(target);

                    bv->page()->runJavaScript(js, [tweetUrls, processedUrls, this, platform](const QVariant &result) {
                        QJsonArray urls = QJsonDocument::fromJson(result.toString().toUtf8()).array();
                        for (const auto &u : urls) {
                            QString url = u.toString();
                            if (!url.isEmpty() && !processedUrls->contains(url)) {
                                processedUrls->insert(url);
                                tweetUrls->append(url);
                            }
                        }
                    });

                } else if (phase == 2) {
                    // Phase 2: 현재 트윗 페이지에서 답글 추출
                    int idx = (*state)["tweetIdx"].toInt();
                    int sc = (*state)["scrollCount"].toInt();

                    // 스크롤 5회 후 다음 트윗으로
                    if (sc >= 5) {
                        idx++;
                        (*state)["tweetIdx"] = idx;
                        (*state)["scrollCount"] = 0;

                        if (idx >= tweetUrls->size()) {
                            // 모든 트윗 처리 완료
                            timer->stop(); timer->deleteLater();

                            // 결과 저장
                            int commentCount = (*state)["commentCount"].toInt();
                            int mediaCount = (*state)["mediaCount"].toInt();

                            if (commentCount > 0 || !mediaUrls->isEmpty()) {
                                // Excel 저장
                                QString userDir = savePath + "/twitter/" + sanitizeFilename(target, 50);
                                QDir().mkpath(userDir);

                                if (collectedReplies->size() > 0) {
                                    ExcelWriter writer;
                                    QStringList hdrs = {"reply_url", "author", "text", "date", "likes", "parent_tweet"};
                                    writer.writeHeader(hdrs, QColor("#d97757"));
                                    for (int r = 0; r < collectedReplies->size(); ++r) {
                                        QJsonObject row = collectedReplies->at(r).toObject();
                                        writer.writeRow(r + 2, {
                                            row["reply_url"].toString(),
                                            row["author"].toString(),
                                            row["text"].toString(),
                                            row["date"].toString(),
                                            row["likes"].toString(),
                                            row["parent_tweet"].toString()
                                        });
                                    }
                                    writer.save(userDir + "/" + target + "_comments_crawl.xlsx");
                                    log(QString("Excel 저장: %1_comments_crawl.xlsx").arg(target), "info", platform);
                                }

                                // 미디어 다운로드
                                if (!mediaUrls->isEmpty()) {
                                    QString commentMediaDir = userDir + "/media/comments";
                                    QDir().mkpath(commentMediaDir);
                                    HttpClient http;
                                    http.setRunFlag(&m_isRunning[platform]);  // 중지 시 즉시 abort
                                    int dl = 0;
                                    for (const QString &url : *mediaUrls) {
                                        if (!m_isRunning.value(platform, false)) break;
                                        QString fn = QUrl(url).fileName();
                                        if (fn.isEmpty() || fn.length() > 100) fn = QString("media_%1").arg(dl + 1);
                                        if (!fn.contains('.')) fn += ".jpg";
                                        QString fp = commentMediaDir + "/" + fn;
                                        if (!QFile::exists(fp) && http.downloadFile(url, fp)) {
                                            FileHelper::setDownloadMeta(fp, url);
                                            dl++;
                                        }
                                    }
                                    log(QString("미디어 %1개 다운로드").arg(dl), "info", platform);
                                }
                            }

                            log(QString("✅ 웹 크롤 댓글 완료: %1개 댓글, %2개 트윗 처리")
                                .arg(commentCount).arg(tweetUrls->size()), "success", platform);
                            updateStats(commentCount, mediaCount, "완료", platform);

                            delete state; delete tweetUrls; delete collectedReplies; delete mediaUrls; delete processedUrls;
                            return;
                        }

                        // 다음 트윗 페이지로 이동
                        log(QString("  [%1/%2] %3").arg(idx + 1).arg(tweetUrls->size()).arg(tweetUrls->at(idx)), "info", platform);
                        bv->load(QUrl(tweetUrls->at(idx)));
                        return;
                    }

                    (*state)["scrollCount"] = sc + 1;
                    int commentCount = (*state)["commentCount"].toInt();
                    updateStats(commentCount, (*state)["mediaCount"].toInt(),
                        QString("댓글 수집 (%1/%2 트윗)").arg(idx + 1).arg(tweetUrls->size()), platform);

                    // JS: 답글 텍스트 + 미디어 추출 + 스크롤
                    QString parentUrl = (idx < tweetUrls->size()) ? tweetUrls->at(idx) : "";
                    QString js = QString(
                        "(function(){"
                        "  var replies=[];"
                        "  var mediaUrls=[];"
                        "  document.querySelectorAll('article[data-testid=\"tweet\"]').forEach(function(article, i){"
                        "    if(i===0) return;"  // 첫 번째는 원본 트윗
                        "    var textEl=article.querySelector('[data-testid=\"tweetText\"]');"
                        "    var text=textEl?textEl.textContent:'';"
                        "    var authorEl=article.querySelector('[data-testid=\"User-Name\"] a');"
                        "    var author=authorEl?authorEl.textContent:'';"
                        "    var timeEl=article.querySelector('time');"
                        "    var date=timeEl?timeEl.getAttribute('datetime'):'';"
                        "    var linkEl=article.querySelector('a[href*=\"/status/\"]');"
                        "    var replyUrl=linkEl?linkEl.href:'';"
                        "    var likesEl=article.querySelector('[data-testid=\"like\"]');"
                        "    var likes=likesEl?likesEl.textContent:'0';"
                        "    replies.push({author:author,text:text,date:date,reply_url:replyUrl,likes:likes,parent_tweet:'%1'});"
                        "    article.querySelectorAll('img[src*=\"pbs.twimg.com/media\"]').forEach(function(img){"
                        "      mediaUrls.push(img.src.replace(/&name=\\w+/,'&name=orig'));"
                        "    });"
                        "  });"
                        "  window.scrollBy(0, window.innerHeight * 2);"
                        "  return JSON.stringify({replies:replies,media:mediaUrls});"
                        "})()"
                    ).arg(parentUrl.replace("'", "\\'"));

                    bv->page()->runJavaScript(js, [state, collectedReplies, mediaUrls, processedUrls, this, platform](const QVariant &result) {
                        QJsonObject data = QJsonDocument::fromJson(result.toString().toUtf8()).object();
                        QJsonArray replies = data["replies"].toArray();
                        QJsonArray media = data["media"].toArray();

                        int newReplies = 0;
                        for (const auto &r : replies) {
                            QJsonObject reply = r.toObject();
                            QString rUrl = reply["reply_url"].toString();
                            if (rUrl.isEmpty() || processedUrls->contains(rUrl)) continue;
                            processedUrls->insert(rUrl);
                            collectedReplies->append(reply);
                            newReplies++;
                        }

                        for (const auto &m : media) {
                            QString url = m.toString();
                            if (!url.isEmpty()) mediaUrls->insert(url);
                        }

                        if (newReplies > 0) {
                            (*state)["commentCount"] = (*state)["commentCount"].toInt() + newReplies;
                            (*state)["mediaCount"] = mediaUrls->size();
                        }
                    });
                }
            });

            QTimer::singleShot(3000, this, [timer]() { timer->start(); });
        }, Qt::QueuedConnection);
        // thread_comments 분기는 자체 비동기 체인 — 30분 안전망 acquire에 맡긴다.
        // (이 분기에서 release하지 않으면 워커가 30분 후에 타임아웃으로 풀린다)
        if (!crawlDone->tryAcquire(1, 30 * 60 * 1000)) {
            log("웹 크롤(thread_comments) 30분 타임아웃", "warning", platform);
        }
        return;
    }

    // 메인 스레드에서 브라우저 조작 — QWebEngine은 메인 스레드에서만 사용 가능
    QMetaObject::invokeMethod(this, [this, crawlDone, targetUrl, platform, savePath, config]() {
        auto *bv = m_window->browserView();
        if (!bv) {
            log("브라우저를 사용할 수 없습니다", "error", platform);
            crawlDone->release();
            return;
        }

        // ★ 로그인 쿠키 주입 — 익명 크롤은 로그인 필요한 컨텐츠를 못 보니까
        //   계정에 저장된 auth_token/sessionId/cookie를 브라우저 쿠키 스토어에 박는다.
        auto *profile = bv->page()->profile();
        auto *cookieStore = profile->cookieStore();
        auto setCookie = [&](const QString &name, const QString &val,
                              const QString &domain, const QString &path = "/") {
            if (val.isEmpty()) return;
            QNetworkCookie ck(name.toUtf8(), val.toUtf8());
            ck.setDomain(domain);
            ck.setPath(path);
            ck.setSecure(true);
            ck.setHttpOnly(false);
            cookieStore->setCookie(ck);
        };

        // accounts 배열 첫 번째 또는 단일 필드에서 가져옴
        QJsonObject acct;
        QJsonArray accs = config["accounts"].toArray();
        if (!accs.isEmpty()) acct = accs[0].toObject();

        int injectedCount = 0;
        if (platform == "twitter") {
            QString authTok = acct["auth_token"].toString();
            QString ct0 = acct["ct0"].toString();
            if (!authTok.isEmpty()) {
                setCookie("auth_token", authTok, ".x.com");
                setCookie("auth_token", authTok, ".twitter.com");
                injectedCount++;
            }
            if (!ct0.isEmpty()) {
                setCookie("ct0", ct0, ".x.com");
                setCookie("ct0", ct0, ".twitter.com");
                injectedCount++;
            }
        } else if (platform == "instagram") {
            QString sid = acct["sessionId"].toString();
            if (sid.isEmpty()) sid = config["sessionId"].toString();
            if (!sid.isEmpty()) {
                setCookie("sessionid", sid, ".instagram.com");
                injectedCount++;
            }
        } else if (platform == "pixiv") {
            QString sid = acct["sessionId"].toString();
            if (!sid.isEmpty()) {
                setCookie("PHPSESSID", sid, ".pixiv.net");
                injectedCount++;
            }
        } else if (platform == "asked" || platform == "spinspin") {
            // 사용자가 브라우저에서 추출한 raw Cookie 문자열을 통째로 파싱
            QString rawCookie = acct["cookie"].toString();
            if (rawCookie.isEmpty()) rawCookie = config["cookie"].toString();
            if (!rawCookie.isEmpty()) {
                QString domain = (platform == "asked") ? ".asked.kr" : ".spin-spin.com";
                for (const QString &part : rawCookie.split(';', Qt::SkipEmptyParts)) {
                    int eq = part.indexOf('=');
                    if (eq <= 0) continue;
                    QString name = part.left(eq).trimmed();
                    QString val = part.mid(eq + 1).trimmed();
                    setCookie(name, val, domain);
                    injectedCount++;
                }
            }
        }
        // bluesky는 핸들/패스워드 → API로 로그인하므로 쿠키 주입 불가
        // tumblr는 API key 기반이므로 쿠키 불필요

        if (injectedCount > 0) {
            log(QString("로그인 쿠키 %1개 주입 (%2)").arg(injectedCount).arg(platform),
                "success", platform);
        } else {
            log("로그인 쿠키 없음 — 익명 크롤로 진행 (계정 추가하면 로그인된 페이지 받음)",
                "warning", platform);
        }

        // 브라우저 표시 + 네비게이션 (쿠키 주입 후 약간 대기 — cookieStore는 비동기)
        showBrowser(true);
        QTimer::singleShot(injectedCount > 0 ? 500 : 0, this, [bv, targetUrl]() {
            bv->load(QUrl(targetUrl));
        });

        int maxCount = config["count"].toInt(0);
        int scrollCount = 0;
        int maxScrolls = maxCount > 0 ? maxCount / 5 + 50 : 500;  // 예상 스크롤 횟수

        // 페이지 로드 완료 후 자동 스크롤 시작
        auto *scrollTimer = new QTimer(this);
        scrollTimer->setInterval(2000);  // 2초 간격 스크롤

        auto *mediaUrls = new QSet<QString>();

        connect(scrollTimer, &QTimer::timeout, this, [=]() mutable {
            if (!m_isRunning.value(platform, false) || scrollCount >= maxScrolls) {
                scrollTimer->stop();
                scrollTimer->deleteLater();

                // 수집된 미디어 다운로드
                log(QString("스크롤 완료 — %1개 미디어 URL 발견").arg(mediaUrls->size()), "success", platform);

                // 플랫폼별 저장 경로 (캡쳐도 여기에 저장)
                QString platformDir = savePath + "/" + platform;
                QString target = config["target"].toString();
                if (target.isEmpty()) target = config["username"].toString();
                QString userDir = platformDir + "/" + sanitizeFilename(target, 50);
                QDir().mkpath(userDir);

                // ★ 브라우저 렌더된 HTML을 그대로 저장 (HTTP GET이 아니라 toHtml)
                //   HTTP 기반 capturePageHtml은 JS shell만 받아서 skip하던 버그 회피
                bv->page()->toHtml([userDir, target, targetUrl, this, platform](const QString &renderedHtml) {
                    if (renderedHtml.isEmpty()) {
                        log("브라우저 HTML 캡쳐 실패 (빈 응답)", "warning", platform);
                        return;
                    }
                    QString capturesDir = userDir + "/captures";
                    QString safeTitle = sanitizeFilename(target, 60);
                    QString saved = FileHelper::capturePageHtmlFromContent(
                        capturesDir, targetUrl, safeTitle, renderedHtml,
                        nullptr, QMap<QString, QString>());
                    if (!saved.isEmpty()) {
                        log(QString("✅ 페이지 캡쳐 저장: %1 (%2KB)")
                            .arg(QFileInfo(saved).fileName())
                            .arg(renderedHtml.size() / 1024), "success", platform);
                    } else {
                        log("페이지 캡쳐 저장 실패", "warning", platform);
                    }
                });

                if (!mediaUrls->isEmpty()) {
                    int downloaded = 0;
                    HttpClient http;
                    http.setRunFlag(&m_isRunning[platform]);  // 중지 시 즉시 abort
                    for (const QString &url : *mediaUrls) {
                        if (!m_isRunning.value(platform, false)) break;
                        QString filename = QUrl(url).fileName();
                        if (filename.isEmpty() || filename.length() > 100)
                            filename = QString("media_%1").arg(downloaded + 1);
                        // 확장자 보정
                        if (!filename.contains('.')) {
                            if (url.contains(".jpg") || url.contains("format=jpg")) filename += ".jpg";
                            else if (url.contains(".png")) filename += ".png";
                            else if (url.contains(".mp4")) filename += ".mp4";
                            else if (url.contains(".webp")) filename += ".webp";
                            else filename += ".jpg";
                        }
                        QString filePath = userDir + "/" + filename;
                        if (QFile::exists(filePath)) continue;

                        if (http.downloadFile(url, filePath)) {
                            FileHelper::setDownloadMeta(filePath, url);
                            downloaded++;
                            updateStats(downloaded, mediaUrls->size(),
                                QString("다운로드 %1/%2").arg(downloaded).arg(mediaUrls->size()), platform);
                        }
                    }
                    log(QString("✅ 웹 크롤 완료: %1개 다운로드").arg(downloaded), "success", platform);
                }

                delete mediaUrls;
                crawlDone->release();  // ★ 워커 스레드 unblock — 멀티타겟 큐가 다음 대상으로 진행
                return;
            }

            scrollCount++;
            updateStats(mediaUrls->size(), scrollCount,
                QString("스크롤 %1/%2").arg(scrollCount).arg(maxScrolls), platform);

            // 현재 페이지에서 미디어 URL 추출 + 스크롤
            QString extractJs;
            if (platform == "twitter") {
                extractJs =
                    "(function(){"
                    "  var urls=[];"
                    "  document.querySelectorAll('img[src*=\"pbs.twimg.com/media\"]').forEach(function(i){"
                    "    var u=i.src.replace(/&name=\\w+/,'&name=orig').replace(/name=\\w+/,'name=orig');"
                    "    urls.push(u);"
                    "  });"
                    "  document.querySelectorAll('video source, video[src]').forEach(function(v){"
                    "    if(v.src) urls.push(v.src);"
                    "  });"
                    "  window.scrollBy(0, window.innerHeight * 2);"
                    "  return JSON.stringify(urls);"
                    "})()";
            } else if (platform == "bluesky") {
                extractJs =
                    "(function(){"
                    "  var urls=[];"
                    "  document.querySelectorAll('img[src*=\"cdn.bsky.app\"]').forEach(function(i){"
                    "    urls.push(i.src);"
                    "  });"
                    "  document.querySelectorAll('video source, video[src]').forEach(function(v){"
                    "    if(v.src) urls.push(v.src);"
                    "  });"
                    "  window.scrollBy(0, window.innerHeight * 2);"
                    "  return JSON.stringify(urls);"
                    "})()";
            } else if (platform == "instagram") {
                extractJs =
                    "(function(){"
                    "  var urls=[];"
                    "  document.querySelectorAll('img[src*=\"cdninstagram\"], img[src*=\"scontent\"]').forEach(function(i){"
                    "    if(i.width>100) urls.push(i.src);"
                    "  });"
                    "  document.querySelectorAll('video source, video[src]').forEach(function(v){"
                    "    if(v.src) urls.push(v.src);"
                    "  });"
                    "  window.scrollBy(0, window.innerHeight * 2);"
                    "  return JSON.stringify(urls);"
                    "})()";
            } else if (platform == "pixiv") {
                extractJs =
                    "(function(){"
                    "  var urls=[];"
                    "  document.querySelectorAll('img[src*=\"i.pximg.net\"]').forEach(function(i){"
                    "    var u=i.src.replace('/c/','/img-original/').replace(/_master\\d+/,'');"
                    "    urls.push(i.src);"
                    "  });"
                    "  document.querySelectorAll('a[href*=\"/artworks/\"]').forEach(function(a){"
                    "    urls.push('ARTWORKS:'+a.href);"
                    "  });"
                    "  window.scrollBy(0, window.innerHeight * 2);"
                    "  return JSON.stringify(urls);"
                    "})()";
            } else if (platform == "asked") {
                // asked.kr: 프로필 아바타 + 질문/답변 안의 이미지/동영상
                extractJs =
                    "(function(){"
                    "  var urls=[];"
                    "  document.querySelectorAll('img').forEach(function(i){"
                    "    var s=i.src||i.dataset.src||'';"
                    "    if(!s||s.startsWith('data:')) return;"
                    "    if(s.includes('asked') || s.includes('cdn') || s.includes('cloudfront') || "
                    "       s.includes('amazonaws') || s.includes('googleusercontent') || "
                    "       s.includes('cloudinary') || s.includes('imgix') || "
                    "       (i.naturalWidth>120 && i.naturalHeight>120))"
                    "      urls.push(s);"
                    "  });"
                    "  document.querySelectorAll('video source, video[src]').forEach(function(v){"
                    "    if(v.src) urls.push(v.src);"
                    "  });"
                    "  window.scrollBy(0, window.innerHeight * 2);"
                    "  return JSON.stringify(urls);"
                    "})()";
            } else {
                extractJs =
                    "(function(){"
                    "  var urls=[];"
                    "  document.querySelectorAll('img[src], video source, video[src], a[href$=\".png\"], a[href$=\".jpg\"]').forEach(function(e){"
                    "    urls.push(e.src||e.href);"
                    "  });"
                    "  window.scrollBy(0, window.innerHeight * 2);"
                    "  return JSON.stringify(urls);"
                    "})()";
            }

            bv->page()->runJavaScript(extractJs, [mediaUrls, this, platform](const QVariant &result) {
                QJsonArray urls = QJsonDocument::fromJson(result.toString().toUtf8()).array();
                int newCount = 0;
                for (const auto &u : urls) {
                    QString url = u.toString();
                    if (!url.isEmpty() && !url.startsWith("data:") && !mediaUrls->contains(url)) {
                        mediaUrls->insert(url);
                        newCount++;
                    }
                }
                if (newCount > 0) {
                    log(QString("  +%1 URL (총 %2)").arg(newCount).arg(mediaUrls->size()), "info", platform);
                }
            });
        });

        // 3초 후 스크롤 시작 (페이지 로드 대기)
        QTimer::singleShot(3000, this, [scrollTimer]() {
            scrollTimer->start();
        });

    }, Qt::QueuedConnection);

    // ★ 워커 스레드 차단 — 메인 스레드의 비동기 체인이 crawlDone->release()를 부를 때까지.
    //    이게 없으면 워커가 즉시 리턴 → m_isRunning=false → 메인 스레드의 scrollTimer 첫 tick에
    //    "!m_isRunning" 분기로 빠져 아무 작업도 안 됨 (다중대상 1개도 다운 안 되던 원인).
    //    안전망: 30분 후 강제 풀림 (페이지가 영구히 멎으면 멀티타겟 큐가 막히는 것 방지)
    if (!crawlDone->tryAcquire(1, 30 * 60 * 1000)) {
        log("웹 크롤 30분 타임아웃 — 워커 스레드 강제 풀림", "warning", platform);
    }
}

// ───────────────────────────────────────────────────────────
// 실제 Chrome (CDP) 모드 — config["method"] == "chrome"
// ───────────────────────────────────────────────────────────
//
// 임베디드 QWebEngine은 navigator.webdriver / 자동화 시그널로 차단되는
// 사이트가 늘어났다. 사용자의 실제 Chrome 브라우저를 CDP로 조종하면
// 사용자 쿠키/로그인/익스텐션을 그대로 사용하므로 사람으로 인식된다.
//
// config:
//   target / url   — 수집 대상
//   path           — 저장 경로
//   maxScrolls     — 스크롤 횟수 (기본 30)
//   useUserProfile — true면 사용자 기본 Chrome 프로필 사용 (로그인 공유)
//                    false면 임시 프로필 (기본값, 다른 Chrome과 충돌 방지)

void MiyoBackend::runRealChromeCollection(const QJsonObject &config)
{
    // ★ 워커 스레드를 차단하기 위한 세마포어.
    //    다중대상 시나리오에서 비동기 체인이 끝나기 전에 워커 스레드가 리턴하면
    //    상위 QThread가 "Done" 시그널을 쏴서 큐가 다음 대상으로 넘어감 → 다음
    //    Chrome 세션이 m_realChrome을 덮어씀 → 1번째 세션이 잘림.
    //    체인 끝(또는 stop 요청)에서 done.release()로 풀어준다.
    auto done = std::make_shared<QSemaphore>(0);

    QMetaObject::invokeMethod(this, [this, config, done]() {
        QString platform = config["platform"].toString();
        QString target = config["target"].toString();
        QString savePath = config["path"].toString();
        savePath.replace("~", QDir::homePath());

        // URL 생성 (runWebCrawlCollection과 동일한 패턴)
        QString targetUrl;
        if (platform == "twitter") targetUrl = "https://x.com/" + target;
        else if (platform == "bluesky") {
            targetUrl = target.contains("bsky.app") ? target : "https://bsky.app/profile/" + target;
        }
        else if (platform == "asked") {
            targetUrl = target.contains("asked.kr") ? target : "https://asked.kr/" + target;
        }
        else if (platform == "instagram") targetUrl = "https://www.instagram.com/" + target + "/";
        else if (platform == "pixiv") targetUrl = "https://www.pixiv.net/users/" + target;
        else if (platform == "tumblr") targetUrl = "https://" + target + ".tumblr.com/";
        else if (platform == "spinspin") {
            targetUrl = target.contains("spin-spin.com") ? target : "https://spin-spin.com/" + target;
        }
        else targetUrl = target;  // 임의 URL

        if (targetUrl.isEmpty()) {
            log("실제 Chrome: URL 생성 실패", "error", platform);
            done->release();
            return;
        }

        QString userDir = savePath + "/" + platform + "/" + sanitizeFilename(target, 50);
        QString capturesDir = userDir + "/captures";
        QString mediaDir = userDir + "/media";
        QString apiRespDir = userDir + "/api_responses";
        QDir().mkpath(capturesDir);
        QDir().mkpath(mediaDir);
        QDir().mkpath(apiRespDir);

        log("═══ 실제 Chrome 모드 ═══", "success", platform);
        log("URL: " + targetUrl, "info", platform);
        log("저장: " + userDir, "info", platform);

        if (m_realChrome) { m_realChrome->stop(); m_realChrome->deleteLater(); m_realChrome = nullptr; }
        m_realChrome = new RealChromeCrawler(this, this);

        // ★ useUserProfile=true 인데 사용자 Chrome 이미 실행 중이면 락 충돌 → CDP 응답 없음.
        //   감지 후 자동으로 펜/체르노빌 전용 프로필로 fallback.
        bool useUP = config["useUserProfile"].toBool(false);
#ifdef Q_OS_MACOS
        if (useUP) {
            QProcess pgrep;
            pgrep.start("/bin/sh", {"-c", "pgrep -f 'Google Chrome.app/Contents/MacOS/Google Chrome' | head -1"});
            pgrep.waitForFinished(2000);
            QString out = QString::fromUtf8(pgrep.readAllStandardOutput()).trimmed();
            if (!out.isEmpty()) {
                log("⚠ 사용자 Chrome 이미 실행 중 — 락 충돌 회피 위해 전용 프로필로 자동 전환", "warning", platform);
                log("  (사용자 본인 프로필 쓰려면 Chrome 완전 종료 후 다시 시작)", "info", platform);
                useUP = false;
            }
        }
#endif
        m_realChrome->setUseUserProfile(useUP);
        m_realChrome->setResponseSaveDir(apiRespDir);

        int maxScrolls = config["maxScrolls"].toInt(30);
        QString platformCopy = platform;
        QString targetCopy = target;
        QString userDirCopy = userDir;
        QString capturesDirCopy = capturesDir;
        QString mediaDirCopy = mediaDir;
        QString targetUrlCopy = targetUrl;

        // ★ 플랫폼별 로그인 쿠키 추출 — Chrome 시작 후 navigate 전 주입
        QJsonArray loginCookies;
        auto pushCookie = [&](const QString &name, const QString &val, const QString &domain) {
            if (val.isEmpty()) return;
            QJsonObject c;
            c["name"] = name; c["value"] = val; c["domain"] = domain;
            c["path"] = "/"; c["secure"] = true; c["httpOnly"] = false;
            loginCookies.append(c);
        };
        QJsonArray accs = config["accounts"].toArray();
        QJsonObject acct = accs.isEmpty() ? QJsonObject() : accs[0].toObject();
        if (platform == "twitter") {
            QString at = acct["auth_token"].toString();
            QString ct0 = acct["ct0"].toString();
            pushCookie("auth_token", at, ".x.com");
            pushCookie("auth_token", at, ".twitter.com");
            pushCookie("ct0", ct0, ".x.com");
            pushCookie("ct0", ct0, ".twitter.com");
        } else if (platform == "instagram") {
            QString sid = acct["sessionId"].toString();
            if (sid.isEmpty()) sid = config["sessionId"].toString();
            pushCookie("sessionid", sid, ".instagram.com");
        } else if (platform == "pixiv") {
            QString sid = acct["sessionId"].toString();
            if (sid.isEmpty()) sid = config["sessionId"].toString();
            pushCookie("PHPSESSID", sid, ".pixiv.net");
        } else if (platform == "asked" || platform == "spinspin") {
            QString rawCookie = acct["cookie"].toString();
            if (rawCookie.isEmpty()) rawCookie = config["cookie"].toString();
            QString domain = (platform == "asked") ? ".asked.kr" : ".spin-spin.com";
            for (const QString &part : rawCookie.split(';', Qt::SkipEmptyParts)) {
                int eq = part.indexOf('=');
                if (eq <= 0) continue;
                pushCookie(part.left(eq).trimmed(), part.mid(eq + 1).trimmed(), domain);
            }
        }

        m_realChrome->start([this, done, platformCopy, targetCopy, userDirCopy, capturesDirCopy,
                              mediaDirCopy, targetUrlCopy, maxScrolls, loginCookies](bool ok) {
            if (!ok) {
                log("실제 Chrome 시작 실패 — Chrome/Edge가 설치돼있는지 확인", "error", platformCopy);
                updateStats(0, 0, "오류", platformCopy);
                done->release();
                return;
            }
            // 쿠키 주입 (있으면) → 그 후 navigate
            auto afterCookies = [this, done, platformCopy, targetCopy, userDirCopy, capturesDirCopy,
                                  mediaDirCopy, targetUrlCopy, maxScrolls]() {
                m_realChrome->navigate(targetUrlCopy, [this, done, platformCopy, targetCopy, userDirCopy,
                                                         capturesDirCopy, mediaDirCopy,
                                                         targetUrlCopy, maxScrolls](bool navOk) {
                if (!navOk) {
                    log("페이지 이동 실패", "error", platformCopy);
                    done->release();
                    return;
                }
                log("페이지 로드 시작 — 5초 대기", "info", platformCopy);
                QTimer::singleShot(5000, this, [this, done, platformCopy, targetCopy, userDirCopy,
                                                 capturesDirCopy, mediaDirCopy,
                                                 targetUrlCopy, maxScrolls]() {
                    if (!m_realChrome) { done->release(); return; }

                    auto scrollCounter = std::make_shared<int>(0);
                    auto prevHeight = std::make_shared<int>(0);
                    auto scrollLoop = std::make_shared<std::function<void()>>();
                    *scrollLoop = [this, done, scrollLoop, scrollCounter, prevHeight,
                                    platformCopy, targetCopy, userDirCopy, capturesDirCopy,
                                    mediaDirCopy, targetUrlCopy, maxScrolls]() {
                        if (!m_isRunning.value(platformCopy, false) || *scrollCounter >= maxScrolls) {
                            // 스크롤 종료 → HTML 캡쳐 + 미디어 추출
                            log(QString("스크롤 완료 (%1회)").arg(*scrollCounter), "success", platformCopy);
                            m_realChrome->getRenderedHtml([this, done, platformCopy, targetCopy, capturesDirCopy,
                                                            mediaDirCopy, userDirCopy, targetUrlCopy](const QString &html) {
                                if (html.isEmpty()) {
                                    log("HTML 캡쳐 실패", "warning", platformCopy);
                                } else {
                                    QString fname = sanitizeFilename(targetCopy, 60) + ".html";
                                    QString path = capturesDirCopy + "/" + fname;
                                    QFile f(path);
                                    if (f.open(QIODevice::WriteOnly)) {
                                        f.write(html.toUtf8());
                                        f.close();
                                        log(QString("✅ 페이지 캡쳐: %1 (%2KB)").arg(fname).arg(html.size() / 1024),
                                            "success", platformCopy);
                                    }
                                }
                                QString mediaJs = R"JS(
                                    (function(){
                                        var urls = [];
                                        document.querySelectorAll('img').forEach(function(im){
                                            var s = im.src || im.dataset.src || '';
                                            if (!s || s.startsWith('data:')) return;
                                            s = s.replace(/&name=\w+/, '&name=orig')
                                                 .replace(/\?name=\w+/, '?name=orig');
                                            if (im.naturalWidth > 100 || s.includes('media')) urls.push(s);
                                        });
                                        document.querySelectorAll('video source, video[src]').forEach(function(v){
                                            if (v.src) urls.push(v.src);
                                        });
                                        return JSON.stringify([...new Set(urls)]);
                                    })();
                                )JS";
                                m_realChrome->evaluate(mediaJs, [this, done, platformCopy, mediaDirCopy](const QJsonValue &v) {
                                    QJsonArray urls = QJsonDocument::fromJson(v.toString().toUtf8()).array();
                                    if (urls.isEmpty()) {
                                        log("미디어 URL 없음", "warning", platformCopy);
                                        updateStats(0, 0, "Done", platformCopy);
                                        done->release();
                                        return;
                                    }
                                    HttpClient http;
                                    http.setRunFlag(&m_isRunning[platformCopy]);
                                    int dl = 0;
                                    for (int i = 0; i < urls.size(); ++i) {
                                        if (!m_isRunning.value(platformCopy, false)) break;
                                        QString url = urls[i].toString();
                                        if (url.isEmpty()) continue;
                                        QString fn = QUrl(url).fileName();
                                        if (fn.isEmpty() || fn.length() > 100) fn = QString("media_%1").arg(dl + 1);
                                        if (!fn.contains('.')) fn += ".jpg";
                                        QString fp = mediaDirCopy + "/" + fn;
                                        if (QFile::exists(fp)) continue;
                                        if (http.downloadFile(url, fp)) {
                                            FileHelper::setDownloadMeta(fp, url);
                                            dl++;
                                            updateStats(dl, urls.size(),
                                                QString("다운로드 %1/%2").arg(dl).arg(urls.size()), platformCopy);
                                        }
                                    }
                                    log(QString("✅ 실제 Chrome 완료: 미디어 %1개").arg(dl), "success", platformCopy);
                                    updateStats(dl, urls.size(), "Done", platformCopy);
                                    done->release();
                                });
                            });
                            return;
                        }

                        m_realChrome->evaluate("document.body.scrollHeight", [this, scrollLoop, scrollCounter,
                                                  prevHeight, platformCopy, maxScrolls](const QJsonValue &v) {
                            int h = v.toInt();
                            *prevHeight = h;
                            m_realChrome->scrollToBottom([this, scrollLoop, scrollCounter, prevHeight,
                                                           platformCopy, maxScrolls]() {
                                (*scrollCounter)++;
                                updateStats(0, 0, QString("스크롤 %1/%2").arg(*scrollCounter).arg(maxScrolls),
                                    platformCopy);
                                QTimer::singleShot(2000, this, [scrollLoop]() { (*scrollLoop)(); });
                            });
                        });
                    };
                    (*scrollLoop)();
                });
            });
            };  // close afterCookies lambda
            // 쿠키 있으면 주입 후 afterCookies, 없으면 바로 afterCookies
            if (!loginCookies.isEmpty()) {
                log(QString("로그인 쿠키 주입 (%1개)").arg(loginCookies.size()), "info", platformCopy);
                m_realChrome->setCookies(loginCookies, [afterCookies](bool){ afterCookies(); });
            } else {
                afterCookies();
            }
        });
    }, Qt::QueuedConnection);

    // ★ 워커 스레드 차단 — 메인 스레드의 비동기 체인이 done->release()를 호출할 때까지 대기.
    //    체인이 stop / 에러 / 정상완료 모두 release를 부른다 (탈출 보장).
    //    안전망: 30분 타임아웃 (Chrome이 멎거나 사용자가 stop 안 할 때 영구 hang 방지)
    if (!done->tryAcquire(1, 30 * 60 * 1000)) {
        log("실제 Chrome 모드 30분 타임아웃 — 강제 종료", "warning", config["platform"].toString());
        QMetaObject::invokeMethod(this, [this]() {
            if (m_realChrome) m_realChrome->stop();
        }, Qt::QueuedConnection);
    }
    // 다음 collector가 새 m_realChrome을 만들 수 있도록 메인 스레드에서 정리
    QMetaObject::invokeMethod(this, [this]() {
        if (m_realChrome) {
            m_realChrome->stop();
            m_realChrome->deleteLater();
            m_realChrome = nullptr;
        }
    }, Qt::QueuedConnection);
}

// ===== Collection Runners =====

// ═══════════════════════════════════════════════════════════════════════════
// 트위터 스페이스(오디오 라이브) 다운로드 — yt-dlp 사용.
//   twikit 텍스트/미디어 수집과 무관. config["target"] 에 스페이스 URL 이 온다.
//   트위터 탭의 로그/중지 버튼/실행상태와 일관되게 platform="twitter" 로 동작.
// ═══════════════════════════════════════════════════════════════════════════
// 단일 스페이스 URL → yt-dlp 다운로드. 스페이스 자동탐지(전체 수집)에서도 재사용.
bool MiyoBackend::downloadSpaceUrl(const QString &urlIn, const QString &outDir)
{
    const QString url = urlIn.trimmed();
    if (url.isEmpty()) return false;
    QDir().mkpath(outDir);

    const QString ytdlp = Common::ytDlpExecutable();
    const QString appDir = QCoreApplication::applicationDirPath();

    QStringList args;
    args << "--no-mtime" << "--newline" << "--no-playlist";
    // ★ ffmpeg 위치 — 스페이스는 m3u8(HLS) 라 ffmpeg 필수. 번들→시스템 순으로 '실제 존재하는' 것만 지정.
    //   (이전엔 ffmpeg 없는 번들 디렉토리를 무조건 가리켜 "ffmpeg could not be found" 로 실패했음)
    for (const QString &ff : QStringList{ appDir + "/ffmpeg", appDir + "/ffmpeg.exe",
                                          "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg" }) {
        if (QFile::exists(ff)) { args << "--ffmpeg-location" << ff; break; }
    }
    // 위 후보가 모두 없으면 --ffmpeg-location 생략 → PATH 에서 탐색.
    args << "--embed-metadata"
         << "-o" << (outDir + "/%(title).180s [%(id)s].%(ext)s")
         << url;

    log(QString("🎙️ 스페이스 다운로드: %1").arg(url), "info", "twitter");

    QProcess proc;
    proc.setProcessEnvironment(Common::bundledProcessEnv());
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(ytdlp, args);
    if (!proc.waitForStarted(10000)) {
        log("yt-dlp 실행 실패 — 번들/설치 상태를 확인하세요.", "error", "twitter");
        return false;
    }
    while (proc.state() != QProcess::NotRunning) {
        if (!platformRunning("twitter")) {            // 중지 버튼 → m_isRunning["twitter"]=false
            proc.terminate();
            if (!proc.waitForFinished(3000)) proc.kill();
            log("⏹ 스페이스 다운로드를 중단했습니다.", "warning", "twitter");
            return false;
        }
        if (proc.waitForReadyRead(400)) {
            const QStringList lines = QString::fromUtf8(proc.readAll()).split('\n', Qt::SkipEmptyParts);
            for (const QString &ln : lines) {
                const QString t = ln.trimmed();
                if (!t.isEmpty()) log(t, "info", "twitter");
            }
        }
    }
    const QString tail = QString::fromUtf8(proc.readAll()).trimmed();
    if (!tail.isEmpty()) log(tail, "info", "twitter");

    const bool ok = (proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0);
    log(ok ? "✅ 스페이스 다운로드 완료."
           : QString("❌ 스페이스 다운로드 실패 (종료코드 %1). 종료/만료됐거나 비공개일 수 있습니다.").arg(proc.exitCode()),
        ok ? "success" : "error", "twitter");
    return ok;
}

void MiyoBackend::runTwitterSpace(const QJsonObject &config)
{
    const QString url = config["target"].toString().trimmed();
    if (url.isEmpty()) {
        log("스페이스 URL을 입력하세요. (예: https://x.com/i/spaces/...)", "error", "twitter");
        return;
    }
    QString savePath = config["path"].toString();
    savePath.replace("~", QDir::homePath());
    if (savePath.isEmpty()) { log("저장 경로가 없습니다.", "error", "twitter"); return; }
    downloadSpaceUrl(url, savePath + "/twitter/spaces");
}

void MiyoBackend::runTwitterCollection(const QJsonObject &config)
{
    CollectionGuard _cg(platformSem("twitter"), this, "twitter");
    // ★ 스페이스(오디오 라이브) — twikit 경로가 아니라 yt-dlp 로 직접 다운로드
    if (config["type"].toString() == "space") {
        runTwitterSpace(config);
        return;
    }
    setIntegrityActiveForPlatform("twitter", config["integrityCheck"].toBool(false));
    // 실제 Chrome (CDP) 모드 — 사이트 봇 차단 회피
    if (config["method"].toString() == "chrome") {
        runRealChromeCollection(config);
        return;
    }
    // 임베디드 웹 크롤 모드
    if (config["method"].toString() == "web") {
        runWebCrawlCollection(config);
        return;
    }

    QJsonObject enrichedConfig = config;
    // 임시파일 — 사용자 임시 디스크 우선, 없으면 저장 경로 안 .abiwa_tmp
    //   (사용자가 빠른 SSD 를 임시 디스크로 지정했으면 그것 사용 — NAS write 회피)
    QString savePath = config["path"].toString();
    savePath.replace("~", QDir::homePath());
    QString tempDir;
    QString userTmp = m_config ? m_config->tempDir() : QString();
    if (!userTmp.isEmpty() && QDir(userTmp).exists()) {
        tempDir = userTmp + "/abiwa_twitter_tmp";
    } else {
        tempDir = savePath + "/.abiwa_tmp";
    }
    QDir().mkpath(tempDir);
    enrichedConfig["tempDir"] = tempDir;

    // 병렬 모드: _parallelKey가 있으면 thread-local collector + 그 키의 isRunning 사용
    //   - 멤버 m_twitterCollector 덮어쓰기 race 방지
    //   - "새 트윗 확인" 기능은 sequential (단일) 수집 후에만 의미 있으므로 멤버 유지 안 함
    const QString parallelKey = config["_parallelKey"].toString();
    if (!parallelKey.isEmpty()) {
        TwitterCollector localCollector(this);
        // m_isRunning[parallelKey]를 참조 — startCollection에서 true로 세팅됨
        if (!m_isRunning.contains(parallelKey)) m_isRunning[parallelKey] = true;
        localCollector.collect(enrichedConfig, m_isRunning[parallelKey]);
        return;
    }

    // Sequential 모드: 기존 멤버 collector 사용 (새 트윗 확인용으로 유지)
    delete m_twitterCollector;
    m_twitterCollector = new TwitterCollector(this);
    m_lastConfig["twitter"] = enrichedConfig;
    m_twitterCollector->collect(enrichedConfig, m_isRunning["twitter"]);
}

void MiyoBackend::runBlueskyCollection(const QJsonObject &config)
{
    CollectionGuard _cg(platformSem("bluesky"), this, "bluesky");
    setIntegrityActiveForPlatform("bluesky", config["integrityCheck"].toBool(false));
    if (config["method"].toString() == "chrome") {
        runRealChromeCollection(config);
        return;
    }
    if (config["method"].toString() == "web") {
        runWebCrawlCollection(config);
        return;
    }

    QJsonObject enrichedConfig = config;
    QString savePath = config["path"].toString();
    savePath.replace("~", QDir::homePath());
    QString tempDir;
    {
        QString userTmp = m_config ? m_config->tempDir() : QString();
        if (!userTmp.isEmpty() && QDir(userTmp).exists()) tempDir = userTmp + "/abiwa_bluesky_tmp";
        else tempDir = savePath + "/.abiwa_tmp";
    }
    QDir().mkpath(tempDir);
    enrichedConfig["tempDir"] = tempDir;
    // 병렬 모드: trackKey 별 isRunning 참조 + thread-local collector
    const QString parallelKey = config["_parallelKey"].toString();
    if (!parallelKey.isEmpty()) {
        BlueskyCollector localCollector(this);
        if (!m_isRunning.contains(parallelKey)) m_isRunning[parallelKey] = true;
        localCollector.collect(enrichedConfig, m_isRunning[parallelKey]);
        return;
    }
    delete m_blueskyCollector;
    m_blueskyCollector = new BlueskyCollector(this);
    m_blueskyCollector->collect(enrichedConfig, m_isRunning["bluesky"]);
}

void MiyoBackend::runDiscordCollection(const QJsonObject &config)
{
    CollectionGuard _cg(platformSem("discord"), this, "discord");
    if (config["method"].toString() == "chrome") {
        runRealChromeCollection(config);
        return;
    }
    if (config["method"].toString() == "web") {
        runWebCrawlCollection(config);
        return;
    }
    QString token = config["token"].toString();
    QString channelId = config["channel"].toString();
    QString serverId = config["server"].toString();
    QString savePath = config["path"].toString();
    savePath.replace("~", QDir::homePath());
    int maxCount = config["count"].toInt(0); // 0 = 무제한
    bool downloadMedia = config["media"].toBool(true);
    bool saveExcel = config["excel"].toBool(true);
    bool downloadProfiles = config["profiles"].toBool(true);
    QString discordType = config["type"].toString("messages");

    // Adaptive delay system
    double dcDelay = 1.0;       // base delay
    int dcConsecutiveOk = 0;
    int dcRateLimitHits = 0;

    // ── 채널 ID 없이 서버 ID만 있으면 자동으로 server-all 모드 ──
    if (channelId.isEmpty() && !serverId.isEmpty() && discordType != "all" && discordType != "server-all") {
        log("채널 ID 없음 → 서버 전체 수집 모드로 전환", "info", "discord");
        discordType = "server-all";
    }

    log(QString("Discord 수집 시작 | 유형: %1").arg(discordType), "info", "discord");
    if (!serverId.isEmpty()) log(QString("서버 ID: %1").arg(serverId), "info", "discord");
    if (!channelId.isEmpty()) log(QString("채널 ID: %1").arg(channelId), "info", "discord");
    log(QString("설정: 미디어=%1, Excel=%2, 프로필=%3, 최대=%4")
        .arg(downloadMedia ? "O" : "X")
        .arg(saveExcel ? "O" : "X")
        .arg(downloadProfiles ? "O" : "X")
        .arg(maxCount > 0 ? QString::number(maxCount) : "무제한"), "info", "discord");

    HttpClient http;
    http.setRunFlag(&m_isRunning["discord"]);  // 중지 요청 시 진행 중 HTTP 즉시 abort

    // ── ALL: 전체 수집 (메시지 + 고정 메시지) ──
    if (discordType == "all") {
        log("═══ 전체 수집 모드 ═══", "success", "discord");
        QStringList subTypes = {"messages", "pins"};
        for (int i = 0; i < subTypes.size(); ++i) {
            if (!m_isRunning.value("discord", false)) break;
            log(QString("▶ [%1/%2] %3 수집...").arg(i+1).arg(subTypes.size()).arg(subTypes[i]), "info", "discord");
            QJsonObject subConfig = config;
            subConfig["type"] = subTypes[i];
            runDiscordCollection(subConfig);
        }
        log("═══ 전체 수집 완료! ═══", "success", "discord");
        return;
    }

    // ── Server-all mode: fetch all channels, then collect each ──
    if (discordType == "server-all" && !serverId.isEmpty()) {
        // 서버 이름 조회
        QString srvName;
        {
            QString guildInfoUrl = QString("https://discord.com/api/v10/guilds/%1").arg(serverId);
            QMap<QString, QString> gh;
            gh["Authorization"] = token;
            HttpResponse gResp = http.get(guildInfoUrl, gh);
            if (gResp.isOk()) srvName = gResp.json()["name"].toString();
        }
        if (srvName.isEmpty()) srvName = serverId;
        log(QString("서버: %1 (%2)").arg(srvName, serverId), "info", "discord");
        log(QString("채널 목록 조회 중..."), "info", "discord");

        QString url = QString("https://discord.com/api/v10/guilds/%1/channels").arg(serverId);
        QMap<QString, QString> headers;
        headers["Authorization"] = token;
        HttpResponse resp = http.get(url, headers);
        if (!resp.isOk()) {
            log(QString("서버 채널 목록 조회 실패: HTTP %1").arg(resp.statusCode), "error", "discord");
            updateStats(0, 0, "오류", "discord");
            return;
        }
        QJsonArray channels = QJsonDocument::fromJson(resp.data).array();

        // 카테고리(type 4)와 텍스트 채널(type 0, 5) 분리
        QMap<QString, QString> categoryNames; // id → name
        QList<QJsonObject> textChannelList;
        for (const auto &ch : channels) {
            QJsonObject chObj = ch.toObject();
            int type = chObj["type"].toInt(-1);
            if (type == 4) {
                // Category
                categoryNames[chObj["id"].toString()] = chObj["name"].toString();
            } else if (type == 0 || type == 5) {
                textChannelList.append(chObj);
            }
        }

        // position 기준 정렬 (카테고리 → 채널 순서)
        std::sort(textChannelList.begin(), textChannelList.end(),
            [](const QJsonObject &a, const QJsonObject &b) {
                QString aPid = a["parent_id"].toString();
                QString bPid = b["parent_id"].toString();
                // 같은 카테고리 내에서는 position 순서
                if (aPid == bPid) {
                    return a["position"].toInt() < b["position"].toInt();
                }
                // 다른 카테고리면 parent_id 순서 (없는 것이 먼저)
                if (aPid.isEmpty()) return true;
                if (bPid.isEmpty()) return false;
                return aPid < bPid;
            });

        // 채널 목록 로그
        log(QString("텍스트 채널 %1개 발견 (전체 %2개 중)").arg(textChannelList.size()).arg(channels.size()), "success", "discord");
        QString currentCategory;
        for (const auto &ch : textChannelList) {
            QString parentId = ch["parent_id"].toString();
            QString catName = categoryNames.value(parentId, "");
            if (catName != currentCategory) {
                currentCategory = catName;
                if (!catName.isEmpty())
                    log(QString("📁 %1").arg(catName), "info", "discord");
            }
            log(QString("  #%1 (%2)").arg(ch["name"].toString(), ch["id"].toString()), "info", "discord");
        }

        int totalMsgs = 0, totalMedia = 0;
        for (int ci = 0; ci < textChannelList.size(); ++ci) {
            if (!m_isRunning.value("discord", false)) break;
            const QJsonObject &ch = textChannelList[ci];
            QString chId = ch["id"].toString();
            QString chName = ch["name"].toString(chId);
            QString parentId = ch["parent_id"].toString();
            QString catName = categoryNames.value(parentId, "");
            QString catPrefix = catName.isEmpty() ? "" : QString("[%1] ").arg(catName);
            log(QString("━━ [%1/%2] %3#%4 (%5) ━━").arg(ci+1).arg(textChannelList.size()).arg(catPrefix, chName, chId), "info", "discord");

            // Build sub-config for this channel (서버 이름 전달하여 중복 API 호출 방지)
            QJsonObject subConfig = config;
            subConfig["channel"] = chId;
            subConfig["type"] = "messages";
            subConfig["_serverName"] = srvName;
            runDiscordCollection(subConfig);

            log(QString("채널 #%1 완료").arg(chName), "success", "discord");
        }
        log(QString("서버 전체 수집 완료 (%1개 채널)").arg(textChannelList.size()), "success", "discord");
        updateStats(0, 0, "완료", "discord");
        return;
    }

    // ── 채널/서버 ID 둘 다 없으면 에러 ──
    if (channelId.isEmpty() && serverId.isEmpty()) {
        log("채널 ID 또는 서버 ID를 입력하세요.", "error", "discord");
        updateStats(0, 0, "오류", "discord");
        return;
    }

    // ── If no channelId but serverId given, use server as guildId ──
    QString guildId = serverId;
    QString channelName = channelId;
    QJsonObject channelInfo;

    if (!channelId.isEmpty()) {
        log(QString("채널 정보 조회 중... (%1)").arg(channelId), "info", "discord");
        QString url = QString("https://discord.com/api/v10/channels/%1").arg(channelId);
        QMap<QString, QString> headers;
        headers["Authorization"] = token;
        HttpResponse resp = http.get(url, headers);
        if (resp.isOk()) {
            channelInfo = resp.json();
            channelName = channelInfo["name"].toString(channelId);
            if (guildId.isEmpty()) guildId = channelInfo["guild_id"].toString();
            log(QString("채널: #%1 (서버: %2)").arg(channelName, guildId), "success", "discord");
        } else {
            log(QString("채널 정보 조회 실패: HTTP %1").arg(resp.statusCode), "error", "discord");
        }
    }

    // ── 폴더 구조: discord/{서버이름}/{채널이름}_{채널ID}/ ──
    QString discordBaseDir = savePath + "/discord";
    QDir().mkpath(discordBaseDir);

    // 서버 이름 조회 (server-all에서 전달받은 경우 스킵)
    QString serverName = config["_serverName"].toString();
    if (serverName.isEmpty() && !guildId.isEmpty()) {
        QString guildUrl = QString("https://discord.com/api/v10/guilds/%1").arg(guildId);
        QMap<QString, QString> headers;
        headers["Authorization"] = token;
        HttpResponse guildResp = http.get(guildUrl, headers);
        if (guildResp.isOk()) {
            serverName = guildResp.json()["name"].toString();
        }
    }
    if (serverName.isEmpty()) serverName = guildId.isEmpty() ? "DM" : guildId;
    serverName = sanitizeFilename(serverName, 50);

    QString serverDir = discordBaseDir + "/" + serverName;
    QDir().mkpath(serverDir);

    QString channelDirName = sanitizeFilename(channelName, 50) + "_" + channelId;
    QString channelDir = serverDir + "/" + channelDirName;
    QDir().mkpath(channelDir);
    // 타입별 폴더 분리: messages/, pins/, threads/, forum/
    QString mediaDir = FileHelper::typeFolder(channelDir, discordType);
    if (downloadMedia) QDir().mkpath(mediaDir);

    // Use disk-based buffer — 저장 경로 안에 생성 (시스템 데이터 방지)
    QString bufTempDir;
    {
        QString userTmp = m_config ? m_config->tempDir() : QString();
        if (!userTmp.isEmpty() && QDir(userTmp).exists()) bufTempDir = userTmp + "/abiwa_discord_" + channelId;
        else bufTempDir = channelDir + "/.abiwa_tmp";
    }
    QDir().mkpath(bufTempDir);
    DiskJsonBuffer allMessages(bufTempDir, "discord");
    int mediaCount = 0;
    QMap<QString, QJsonObject> uniqueUsers;

    // 파일명 생성: [업로드순서prefix] 내용 (원본파일명).확장자
    auto dcFilename = [](const QJsonObject &msg, const QString &origFilename, int idx = -1) -> QString {
        QString content = msg["content"].toString().left(40).trimmed();
        content.replace('\n', ' ');
        content = sanitizeFilename(content, 40);
        if (content.isEmpty()) content = msg["author"].toObject()["username"].toString();

        QString ts = msg["timestamp"].toString();
        QDateTime dt = QDateTime::fromString(ts, Qt::ISODate);
        // 업로드 순서 prefix (KST 기준)
        QString prefix = FileHelper::uploadOrderPrefix(dt.isValid() ? dt.toUTC().addSecs(9*3600) : QDateTime());

        QString ext = QFileInfo(origFilename).suffix();
        QString baseName = QFileInfo(origFilename).completeBaseName();
        if (ext.isEmpty()) ext = "bin";

        QString name = prefix + content + " (" + baseName;
        if (idx >= 0) name += "_" + QString::number(idx);
        name += ")." + ext;
        return sanitizeFilename(name, 200);
    };

    log(QString("저장 경로: %1").arg(channelDir), "info", "discord");

    if (discordType == "pins") {
        // ── Pinned Messages ──
        log("고정 메시지 수집 중...", "info", "discord");
        QString pinsUrl = QString("https://discord.com/api/v10/channels/%1/pins").arg(channelId);
        QMap<QString, QString> headers;
        headers["Authorization"] = token;
        headers["Content-Type"] = "application/json";

        HttpResponse resp = http.get(pinsUrl, headers);

        if (resp.statusCode == 429) {
            QJsonObject rateLimitBody = QJsonDocument::fromJson(resp.data).object();
            int retryAfter = qMax(static_cast<int>(rateLimitBody["retry_after"].toDouble(30.0)), 5);
            log(QString("Rate limited, waiting %1s...").arg(retryAfter), "warning", "discord");
            for (int r = retryAfter; r > 0 && m_isRunning.value("discord", false); --r) {
                updateStats(0, 0, QString("대기 %1s").arg(r), "discord");
                QThread::sleep(1);
            }
            // Retry once
            resp = http.get(pinsUrl, headers);
        }

        if (!resp.isOk()) {
            log(QString("API error: %1").arg(resp.statusCode), "error", "discord");
        } else {
            QJsonArray pins = QJsonDocument::fromJson(resp.data).array();
            for (const auto &val : pins) {
                if (!m_isRunning.value("discord", false)) break;
                QJsonObject msg = val.toObject();
                allMessages.append(msg);

                // Track unique user
                {
                    QJsonObject author = msg["author"].toObject();
                    QString authorId = author["id"].toString();
                    if (!authorId.isEmpty() && !uniqueUsers.contains(authorId))
                        uniqueUsers[authorId] = author;
                }

                if (downloadMedia) {
                    QJsonArray attachments = msg["attachments"].toArray();
                    for (const auto &attVal : attachments) {
                        QJsonObject att = attVal.toObject();
                        QString attUrl = att["url"].toString();
                        if (attUrl.isEmpty()) continue;
                        QString origName = att["filename"].toString("file");
                        QString filename = dcFilename(msg, origName);
                        QString filepath = mediaDir + "/" + filename;
                        if (http.downloadFile(attUrl, filepath)) {
                            QString author = msg["author"].toObject()["username"].toString();
                            QString content = msg["content"].toString().left(200);
                            Common::addExifMetadata(filepath, "@" + author, content,
                                "Discord @" + author, attUrl, msg["timestamp"].toString());
                            FileHelper::setFinderComment(filepath, attUrl);
                            FileHelper::setDownloadMeta(filepath, attUrl);
                            Common::setFileTimes(filepath, msg["timestamp"].toString());
                            // _complete 미러
                            QString dcCompleteDir = FileHelper::typeFolder(channelDir, "complete");
                            QString cp = dcCompleteDir + "/" + filename;
                            if (!QFile::exists(cp)) { QFile::copy(filepath, cp); Common::setFileTimes(cp, msg["timestamp"].toString()); }
                            mediaCount++;
                        }
                    }

                    // Download embeds (images/thumbnails)
                    QJsonArray embeds = msg["embeds"].toArray();
                    for (int ei = 0; ei < embeds.size(); ++ei) {
                        QJsonObject embed = embeds[ei].toObject();
                        QString imgUrl = embed["image"].toObject()["url"].toString();
                        if (imgUrl.isEmpty()) imgUrl = embed["thumbnail"].toObject()["url"].toString();
                        if (!imgUrl.isEmpty()) {
                            QString ext = imgUrl.contains(".gif") ? ".gif" : ".jpg";
                            QString filename = dcFilename(msg, "embed" + ext, ei);
                            QString filepath = mediaDir + "/" + filename;
                            if (http.downloadFile(imgUrl, filepath)) {
                                QString author = msg["author"].toObject()["username"].toString();
                                Common::addExifMetadata(filepath, "@" + author, "",
                                    "Discord @" + author, imgUrl, msg["timestamp"].toString());
                                FileHelper::setFinderComment(filepath, imgUrl);
                                FileHelper::setDownloadMeta(filepath, imgUrl);
                                Common::setFileTimes(filepath, msg["timestamp"].toString());
                                QString dcCompleteDir = FileHelper::typeFolder(channelDir, "complete");
                                QString cp = dcCompleteDir + "/" + filename;
                                if (!QFile::exists(cp)) { QFile::copy(filepath, cp); Common::setFileTimes(cp, msg["timestamp"].toString()); }
                                mediaCount++;
                            }
                        }
                    }
                }
                updateStats(allMessages.count(), mediaCount, "수집 중", "discord");
            }
            log(QString("고정 메시지: %1개, 미디어: %2개").arg(allMessages.count()).arg(mediaCount), "success", "discord");
        }
    } else {
        // ── Regular Messages ──
        QString before;
        while (m_isRunning.value("discord", false)) {
            QString url = QString("https://discord.com/api/v10/channels/%1/messages?limit=100").arg(channelId);
            if (!before.isEmpty()) url += "&before=" + before;

            QMap<QString, QString> headers;
            headers["Authorization"] = token;
            headers["Content-Type"] = "application/json";

            HttpResponse resp = http.get(url, headers);

            if (resp.statusCode == 429) {
                dcRateLimitHits++;
                dcConsecutiveOk = 0;
                dcDelay = qMin(dcDelay * 2.0, 15.0);
                QJsonObject rateLimitBody = QJsonDocument::fromJson(resp.data).object();
                int retryAfter = qMax(static_cast<int>(rateLimitBody["retry_after"].toDouble(30.0)), 5);
                log(QString("⚠️ Rate Limit (%1回) - %2秒 대기 (適応ﾃﾞｨﾚｲ: %3秒)")
                    .arg(dcRateLimitHits).arg(retryAfter).arg(dcDelay, 0, 'f', 1), "warning", "discord");
                for (int r = retryAfter; r > 0 && m_isRunning.value("discord", false); --r) {
                    updateStats(allMessages.count(), mediaCount, QString("대기 %1s").arg(r), "discord");
                    QThread::sleep(1);
                }
                continue;
            }

            if (!resp.isOk()) {
                log(QString("API error: %1").arg(resp.statusCode), "error", "discord");
                break;
            }

            QJsonArray batch = QJsonDocument::fromJson(resp.data).array();
            if (batch.isEmpty()) break;

            int batchAttach = 0, batchEmbed = 0, batchSticker = 0, batchImg = 0, batchVid = 0;
            QString batchFirstDate, batchLastDate;

            for (const auto &val : batch) {
                QJsonObject msg = val.toObject();
                allMessages.append(msg);

                // Track date range
                QString ts = msg["timestamp"].toString();
                if (batchFirstDate.isEmpty()) batchFirstDate = ts;
                batchLastDate = ts;

                // Track unique user
                QJsonObject author = msg["author"].toObject();
                QString authorName = author["username"].toString();
                {
                    QString authorId = author["id"].toString();
                    if (!authorId.isEmpty() && !uniqueUsers.contains(authorId))
                        uniqueUsers[authorId] = author;
                }

                // Per-message detailed log
                {
                    QString content = msg["content"].toString().left(80);
                    content.replace('\n', ' ');
                    int attCount = msg["attachments"].toArray().size();
                    int embedCount = msg["embeds"].toArray().size();
                    QDateTime dt = Common::parseISODate(ts);
                    QString dateStr;
                    if (dt.isValid()) {
                        dt = dt.toUTC().addSecs(9 * 3600);
                        dateStr = dt.toString("MM/dd HH:mm");
                    }
                    QString mediaTags;
                    if (attCount > 0) mediaTags += QString(" 📎%1").arg(attCount);
                    if (embedCount > 0) mediaTags += QString(" 🔗%1").arg(embedCount);
                    log(QString("[%1] %2 @%3%4 %5")
                        .arg(allMessages.count()).arg(dateStr, authorName, mediaTags, content), "info", "discord");
                }

                if (downloadMedia) {
                    // Download attachments
                    QJsonArray attachments = msg["attachments"].toArray();
                    for (const auto &attVal : attachments) {
                        QJsonObject att = attVal.toObject();
                        QString attUrl = att["url"].toString();
                        if (attUrl.isEmpty()) continue;

                        QString origName = att["filename"].toString("file");
                        QString filename = dcFilename(msg, origName);
                        QString filepath = mediaDir + "/" + filename;

                        // Classify attachment type
                        QString ct = att["content_type"].toString().toLower();
                        bool isVideo = ct.startsWith("video") || filepath.endsWith(".mp4") || filepath.endsWith(".webm") || filepath.endsWith(".mov");

                        if (http.downloadFile(attUrl, filepath)) {
                            QString author = msg["author"].toObject()["username"].toString();
                            QString content = msg["content"].toString().left(200);
                            Common::addExifMetadata(filepath, "@" + author, content,
                                "Discord @" + author, attUrl, msg["timestamp"].toString());
                            FileHelper::setFinderComment(filepath, attUrl);
                            FileHelper::setDownloadMeta(filepath, attUrl);
                            Common::setFileTimes(filepath, msg["timestamp"].toString());
                            // _complete 미러
                            QString dcCompleteDir = FileHelper::typeFolder(channelDir, "complete");
                            QString cp = dcCompleteDir + "/" + filename;
                            if (!QFile::exists(cp)) { QFile::copy(filepath, cp); Common::setFileTimes(cp, msg["timestamp"].toString()); }
                            mediaCount++;
                            batchAttach++;
                            if (isVideo) batchVid++; else batchImg++;
                        }
                    }

                    // Download embeds (images/thumbnails/videos)
                    QJsonArray embeds = msg["embeds"].toArray();
                    for (int ei = 0; ei < embeds.size(); ++ei) {
                        QJsonObject embed = embeds[ei].toObject();
                        QString imgUrl = embed["image"].toObject()["url"].toString();
                        if (imgUrl.isEmpty()) imgUrl = embed["thumbnail"].toObject()["url"].toString();
                        if (imgUrl.isEmpty()) imgUrl = embed["video"].toObject()["url"].toString();
                        if (!imgUrl.isEmpty()) {
                            bool isVid = imgUrl.contains(".mp4") || imgUrl.contains(".webm") || embed.contains("video");
                            QString ext = imgUrl.contains(".gif") ? ".gif"
                                        : imgUrl.contains(".mp4") ? ".mp4"
                                        : imgUrl.contains(".webm") ? ".webm" : ".jpg";
                            QString filename = dcFilename(msg, "embed" + ext, ei);
                            QString filepath = mediaDir + "/" + filename;
                            if (http.downloadFile(imgUrl, filepath)) {
                                QString author = msg["author"].toObject()["username"].toString();
                                Common::addExifMetadata(filepath, "@" + author, "",
                                    "Discord @" + author, imgUrl, msg["timestamp"].toString());
                                FileHelper::setFinderComment(filepath, imgUrl);
                                FileHelper::setDownloadMeta(filepath, imgUrl);
                                Common::setFileTimes(filepath, msg["timestamp"].toString());
                                QString dcCompleteDir = FileHelper::typeFolder(channelDir, "complete");
                                QString cp = dcCompleteDir + "/" + filename;
                                if (!QFile::exists(cp)) { QFile::copy(filepath, cp); Common::setFileTimes(cp, msg["timestamp"].toString()); }
                                mediaCount++;
                                batchEmbed++;
                                if (isVid) batchVid++; else batchImg++;
                            }
                        }
                    }

                    // Download stickers
                    QJsonArray stickers = msg["sticker_items"].toArray();
                    for (int si = 0; si < stickers.size(); ++si) {
                        QJsonObject sticker = stickers[si].toObject();
                        QString stickerId = sticker["id"].toString();
                        if (stickerId.isEmpty()) continue;
                        QString stickerUrl = QString("https://media.discordapp.net/stickers/%1.png?size=1024").arg(stickerId);
                        QString filename = dcFilename(msg, "sticker.png", si);
                        QString filepath = mediaDir + "/" + filename;
                        if (http.downloadFile(stickerUrl, filepath)) {
                            QString author = msg["author"].toObject()["username"].toString();
                            Common::addExifMetadata(filepath, "@" + author, "",
                                "Discord @" + author, stickerUrl, msg["timestamp"].toString());
                            FileHelper::setFinderComment(filepath, stickerUrl);
                            FileHelper::setDownloadMeta(filepath, stickerUrl);
                            Common::setFileTimes(filepath, msg["timestamp"].toString());
                            QString dcCompleteDir = FileHelper::typeFolder(channelDir, "complete");
                            QString cp = dcCompleteDir + "/" + filename;
                            if (!QFile::exists(cp)) { QFile::copy(filepath, cp); Common::setFileTimes(cp, msg["timestamp"].toString()); }
                            mediaCount++;
                            batchSticker++;
                            batchImg++;
                        }
                    }
                }
            }

            before = batch.last().toObject()["id"].toString();
            updateStats(allMessages.count(), mediaCount, "수집 중", "discord");

            // Date range for this batch
            QDateTime dtFirst = Common::parseISODate(batchFirstDate);
            QDateTime dtLast = Common::parseISODate(batchLastDate);
            QString dateRange;
            if (dtFirst.isValid() && dtLast.isValid()) {
                QString d1 = dtFirst.toUTC().addSecs(9*3600).toString("yyyy/MM/dd HH:mm");
                QString d2 = dtLast.toUTC().addSecs(9*3600).toString("yyyy/MM/dd HH:mm");
                dateRange = (d1 == d2) ? d1 : d2 + " ～ " + d1;
            }

            // Detailed batch log
            QStringList mediaDetails;
            if (batchImg > 0) mediaDetails << QString("画像%1").arg(batchImg);
            if (batchVid > 0) mediaDetails << QString("動画%1").arg(batchVid);
            if (batchSticker > 0) mediaDetails << QString("ｽﾀﾝﾌﾟ%1").arg(batchSticker);
            QString mediaSummary = mediaDetails.isEmpty() ? "" : " [" + mediaDetails.join("/") + "]";

            log(QString("メッセージ %1件 | ﾒﾃﾞｨｱ %2件%3 | ﾕｰｻﾞｰ %4人")
                .arg(allMessages.count()).arg(mediaCount).arg(mediaSummary).arg(uniqueUsers.size()), "info", "discord");
            if (!dateRange.isEmpty())
                log(QString("  📅 %1").arg(dateRange), "info", "discord");

            if (maxCount > 0 && allMessages.count() >= maxCount) break;

            // 중간 저장: 500메시지마다 Excel 체크포인트 (강제 종료 대비)
            if (saveExcel && allMessages.count() > 0 && allMessages.count() % 500 < 100) {
                log(QString("💾 중간 저장 (%1건)").arg(allMessages.count()), "info", "discord");
                ExcelWriter tmpWriter;
                QStringList tmpHdrs = {"Message ID", "Message URL", "Timestamp", "Type",
                                       "Author", "Author ID", "Content",
                                       "Attachments", "Embeds", "Reactions",
                                       "Reply To", "Edited At", "Stickers", "Thread"};
                tmpWriter.writeHeader(tmpHdrs, QColor("#5865F2"));
                allMessages.resetReader();
                QJsonObject tmpMsg;
                int tmpRow = 2;
                while (allMessages.readNext(tmpMsg)) {
                    QString tmpMsgUrl;
                    if (!guildId.isEmpty())
                        tmpMsgUrl = QString("https://discord.com/channels/%1/%2/%3").arg(guildId, channelId, tmpMsg["id"].toString());
                    else
                        tmpMsgUrl = QString("https://discord.com/channels/@me/%1/%2").arg(channelId, tmpMsg["id"].toString());
                    QStringList tmpAtts;
                    for (const auto &att : tmpMsg["attachments"].toArray())
                        tmpAtts << att.toObject()["filename"].toString();
                    QStringList tmpReactions;
                    for (const auto &r : tmpMsg["reactions"].toArray()) {
                        QJsonObject ro = r.toObject();
                        tmpReactions << QString("%1 x%2").arg(ro["emoji"].toObject()["name"].toString()).arg(ro["count"].toInt());
                    }
                    int mt = tmpMsg["type"].toInt(0);
                    QString ts;
                    switch (mt) { case 0: ts="Default"; break; case 6: ts="Pin"; break; case 7: ts="Join"; break;
                                  case 8: ts="Boost"; break; case 19: ts="Reply"; break; default: ts=QString("Type %1").arg(mt); }
                    tmpWriter.writeRow(tmpRow++, {
                        tmpMsg["id"].toString(), tmpMsgUrl,
                        tmpMsg["timestamp"].toString().left(19).replace("T", " "), ts,
                        tmpMsg["author"].toObject()["username"].toString(),
                        tmpMsg["author"].toObject()["id"].toString(),
                        tmpMsg["content"].toString().left(500),
                        tmpAtts.join(", "), "", tmpReactions.join(", "),
                        "", tmpMsg["edited_timestamp"].isNull() ? "" : tmpMsg["edited_timestamp"].toString().left(19).replace("T", " "),
                        "", ""
                    });
                }
                tmpWriter.autoFitColumns(tmpHdrs);
                QString tmpSuffix = discordType == "pins" ? "pins_complete" : "messages_complete";
                tmpWriter.save(channelDir + "/" + channelName + "_" + tmpSuffix + ".xlsx");
            }

            // Adaptive delay: success → decay
            dcConsecutiveOk++;
            if (dcConsecutiveOk > 5) {
                dcDelay = qMax(dcDelay * 0.9, 0.5);
                dcRateLimitHits = 0;
            }
            QThread::msleep(static_cast<unsigned long>(dcDelay * 1000));
        }
    }

    log("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━", "success", "discord");
    log(QString("収集完了: メッセージ %1件, メディア %2件, ユーザー %3人")
        .arg(allMessages.count()).arg(mediaCount).arg(uniqueUsers.size()), "success", "discord");
    log(QString("  保存先: %1").arg(channelDir), "info", "discord");

    if (saveExcel && allMessages.count() > 0) {
        log("Excel 저장 중...", "info", "discord");
        QStringList hdrs = {"Message ID", "Message URL", "Timestamp", "Type",
                            "Author", "Author ID", "Content",
                            "Attachments", "Embeds", "Reactions",
                            "Reply To", "Edited At", "Stickers", "Thread"};

        // 메시지 → 행 변환 람다
        auto msgToRow = [&](const QJsonObject &msg) -> QStringList {
            QString msgUrl;
            if (!guildId.isEmpty())
                msgUrl = QString("https://discord.com/channels/%1/%2/%3").arg(guildId, channelId, msg["id"].toString());
            else
                msgUrl = QString("https://discord.com/channels/@me/%1/%2").arg(channelId, msg["id"].toString());
            QStringList atts;
            for (const auto &att : msg["attachments"].toArray()) atts << att.toObject()["filename"].toString();
            QStringList embedDescs;
            for (const auto &emb : msg["embeds"].toArray()) {
                QJsonObject e = emb.toObject();
                QString desc = e["title"].toString();
                if (desc.isEmpty()) desc = e["description"].toString().left(80);
                if (desc.isEmpty()) desc = e["type"].toString();
                if (!desc.isEmpty()) embedDescs << desc;
            }
            QStringList reactions;
            for (const auto &r : msg["reactions"].toArray()) {
                QJsonObject ro = r.toObject();
                reactions << QString("%1 x%2").arg(ro["emoji"].toObject()["name"].toString()).arg(ro["count"].toInt());
            }
            QJsonObject refMsg = msg["referenced_message"].toObject();
            QString replyTo;
            if (!refMsg.isEmpty())
                replyTo = refMsg["author"].toObject()["username"].toString() + ": " + refMsg["content"].toString().left(50);
            int mt = msg["type"].toInt(0);
            QString typeStr;
            switch (mt) {
                case 0: typeStr = "Default"; break; case 6: typeStr = "Pin"; break;
                case 7: typeStr = "Join"; break; case 8: typeStr = "Boost"; break;
                case 19: typeStr = "Reply"; break; case 20: typeStr = "Slash Command"; break;
                case 21: typeStr = "Thread"; break; default: typeStr = QString("Type %1").arg(mt);
            }
            QStringList stickerNames;
            for (const auto &s : msg["sticker_items"].toArray()) stickerNames << s.toObject()["name"].toString();
            return {
                msg["id"].toString(), msgUrl,
                msg["timestamp"].toString().left(19).replace("T", " "), typeStr,
                msg["author"].toObject()["username"].toString(), msg["author"].toObject()["id"].toString(),
                msg["content"].toString().left(500), atts.join(", "), embedDescs.join(", "),
                reactions.join(", "), replyTo,
                msg["edited_timestamp"].isNull() ? "" : msg["edited_timestamp"].toString().left(19).replace("T", " "),
                stickerNames.join(", "), msg["thread"].toObject()["name"].toString()
            };
        };

        // Complete 방식: 기존 파일 + 새 데이터 합치기
        // 타입별 Excel 분리: excel/{channelName}_{type}.xlsx
        QString dcExcelDir = channelDir + "/excel";
        QDir().mkpath(dcExcelDir);
        QString discordExcelPath = FileHelper::typeExcelPath(dcExcelDir, channelName, discordType);

        // 기존 파일 읽기
        QSet<QString> existingIds;
        QList<QStringList> oldRows;
        if (QFile::exists(discordExcelPath)) {
            QXlsx::Document existDoc(discordExcelPath);
            int lastRow = existDoc.dimension().lastRow();
            for (int r = 2; r <= lastRow; ++r) {
                QString mid = existDoc.read(r, 1).toString().trimmed();
                if (mid.isEmpty() || mid.startsWith("─") || mid.startsWith("═")) continue;
                existingIds.insert(mid);
                QStringList cols;
                for (int c = 1; c <= 14; ++c) cols << existDoc.read(r, c).toString();
                oldRows.append(cols);
            }
        }

        // 새 데이터 변환 + 중복 제거
        QList<QStringList> newRows;
        allMessages.resetReader();
        QJsonObject msg;
        while (allMessages.readNext(msg)) {
            QString mid = msg["id"].toString();
            if (!existingIds.contains(mid)) {
                newRows.append(msgToRow(msg));
            }
        }

        // 시간 정렬 (Timestamp = col[2])
        std::sort(newRows.begin(), newRows.end(), [](const QStringList &a, const QStringList &b) { return a[2] > b[2]; });
        std::sort(oldRows.begin(), oldRows.end(), [](const QStringList &a, const QStringList &b) { return a[2] > b[2]; });

        ExcelWriter writer;
        writer.writeHeader(hdrs, QColor("#5865F2"));
        int row = 2;
        for (const auto &r : newRows) writer.writeRow(row++, r);
        if (!newRows.isEmpty() && !oldRows.isEmpty()) {
            writer.writeRow(row++, {QString("═══ %1 新規 %2件 ═══").arg(QDateTime::currentDateTime().toString("yyyy/MM/dd HH:mm")).arg(newRows.size())});
        }
        for (const auto &r : oldRows) writer.writeRow(row++, r);
        writer.autoFitColumns(hdrs);
        writer.save(discordExcelPath);
        FileHelper::setDownloadMeta(discordExcelPath, "https://discord.com");
        log(QString("Excel 저장: +%1개 (총 %2개)").arg(newRows.size()).arg(newRows.size() + oldRows.size()), "success", "discord");
    }

    // ── Download user profiles (avatars + banners) — 유저별 → 날짜별 정리 ──
    if (downloadProfiles && !uniqueUsers.isEmpty() && m_isRunning.value("discord", false)) {
        log(QString("사용자 프로필 다운로드... (%1명)").arg(uniqueUsers.size()), "info", "discord");
        QString profilesBaseDir = channelDir + "/profiles";
        QString dateStr = QDate::currentDate().toString("yyyy-MM-dd");

        int profileDone = 0;
        int profileNew = 0, profileUpdated = 0, profileSkipped = 0;
        for (auto it = uniqueUsers.begin(); it != uniqueUsers.end() && m_isRunning.value("discord", false); ++it) {
            QString dcUserId = it.key();
            QJsonObject author = it.value();
            QString username = author["username"].toString();
            QString displayName = author["global_name"].toString();
            if (displayName.isEmpty()) displayName = username;
            QString avatarHash = author["avatar"].toString();

            // 유저별 → 날짜별 폴더: profiles/{username}_{userId}/{날짜}/
            QString userProfileDir = profilesBaseDir + "/" + sanitizeFilename(username, 30) + "_" + dcUserId;
            QString dateDirPath = userProfileDir + "/" + dateStr;
            QDir().mkpath(dateDirPath);

            bool alreadyExists = QFile::exists(dateDirPath + "/profile.json");

            // Download avatar + EXIF 메타데이터
            if (!avatarHash.isEmpty()) {
                QString ext = avatarHash.startsWith("a_") ? ".gif" : ".png";
                QString avatarUrl = QString("https://cdn.discordapp.com/avatars/%1/%2%3?size=1024")
                    .arg(dcUserId, avatarHash, ext);
                QString avatarPath = dateDirPath + "/avatar" + ext;
                if (!QFile::exists(avatarPath)) {
                    if (http.downloadFile(avatarUrl, avatarPath)) {
                        Common::addExifMetadata(avatarPath, "@" + username, displayName,
                            "Discord @" + username, "", "");
                        log(QString("  아바타: %1").arg(username), "info", "discord");
                    }
                }
            }

            // Fetch full user profile for banner
            {
                QMap<QString, QString> headers;
                headers["Authorization"] = token;
                HttpResponse profileResp = http.get(
                    QString("https://discord.com/api/v10/users/%1").arg(dcUserId), headers);

                if (profileResp.statusCode == 429) {
                    QJsonObject rl = QJsonDocument::fromJson(profileResp.data).object();
                    int wait = qMax(static_cast<int>(rl["retry_after"].toDouble(5.0)), 2);
                    log(QString("  Rate limit - %1초 대기 (%2)").arg(wait).arg(username), "warning", "discord");
                    QThread::sleep(wait);
                    profileResp = http.get(
                        QString("https://discord.com/api/v10/users/%1").arg(dcUserId), headers);
                }

                if (profileResp.isOk()) {
                    QJsonObject profileData = profileResp.json();
                    QString bannerHash = profileData["banner"].toString();
                    if (!bannerHash.isEmpty()) {
                        QString ext = bannerHash.startsWith("a_") ? ".gif" : ".png";
                        QString bannerUrl = QString("https://cdn.discordapp.com/banners/%1/%2%3?size=1024")
                            .arg(dcUserId, bannerHash, ext);
                        QString bannerPath = dateDirPath + "/banner" + ext;
                        if (!QFile::exists(bannerPath)) {
                            if (http.downloadFile(bannerUrl, bannerPath)) {
                                Common::addExifMetadata(bannerPath, "@" + username, displayName,
                                    "Discord @" + username, "", "");
                                log(QString("  배너: %1").arg(username), "info", "discord");
                            }
                        }
                    }

                    // Save user profile JSON (날짜별)
                    QFile pf(dateDirPath + "/profile.json");
                    if (pf.open(QIODevice::WriteOnly)) {
                        pf.write(QJsonDocument(profileData).toJson(QJsonDocument::Indented));
                        pf.close();
                    }

                    if (alreadyExists) profileUpdated++;
                    else profileNew++;
                } else {
                    profileSkipped++;
                }
            }

            profileDone++;
            updateStats(allMessages.count(), mediaCount,
                        QString("프로필 %1/%2").arg(profileDone).arg(uniqueUsers.size()), "discord");
            QThread::msleep(500);
        }
        log(QString("프로필 완료: %1명 (신규 %2, 갱신 %3, 실패 %4)")
            .arg(uniqueUsers.size()).arg(profileNew).arg(profileUpdated).arg(profileSkipped),
            "success", "discord");
    }

    // ── Save channel index ──
    {
        QJsonObject index;
        index["channel_id"] = channelId;
        index["channel_name"] = channelName;
        index["guild_id"] = guildId;
        index["collection_type"] = discordType;
        index["total_messages"] = allMessages.count();
        index["total_media"] = mediaCount;
        index["unique_users"] = uniqueUsers.size();
        index["collected_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);

        // Date range
        if (allMessages.count() > 0) {
            allMessages.resetReader();
            QJsonObject tmsg;
            QString firstDate, lastDate;
            while (allMessages.readNext(tmsg)) {
                QString ts = tmsg["timestamp"].toString();
                if (firstDate.isEmpty() || ts < firstDate) firstDate = ts;
                if (lastDate.isEmpty() || ts > lastDate) lastDate = ts;
            }
            index["date_from"] = firstDate.left(19).replace("T", " ");
            index["date_to"] = lastDate.left(19).replace("T", " ");
        }

        // User list
        QJsonArray userList;
        for (auto it = uniqueUsers.begin(); it != uniqueUsers.end(); ++it) {
            QJsonObject u;
            u["id"] = it.key();
            u["username"] = it.value()["username"].toString();
            u["global_name"] = it.value()["global_name"].toString();
            u["avatar"] = it.value()["avatar"].toString();
            userList.append(u);
        }
        index["users"] = userList;

        // Channel info
        if (!channelInfo.isEmpty())
            index["channel_info"] = channelInfo;

        QFile indexFile(channelDir + "/index.json");
        if (indexFile.open(QIODevice::WriteOnly)) {
            indexFile.write(QJsonDocument(index).toJson(QJsonDocument::Indented));
            indexFile.close();
        }
        log("인덱스 저장 완료.", "success", "discord");
    }
}

void MiyoBackend::runInstagramCollection(const QJsonObject &config)
{
    CollectionGuard _cg(platformSem("instagram"), this, "instagram");
    setIntegrityActiveForPlatform("instagram", config["integrityCheck"].toBool(false));
    if (config["method"].toString() == "chrome") {
        runRealChromeCollection(config);
        return;
    }
    if (config["method"].toString() == "web") {
        // ★ 내부 QWebEngine 대신 실제 Chrome (CDP)로 라우팅 — 사용자 로그인 세션 사용 + 봇 탐지 우회
        runRealChromeCollection(config);
        return;
    }
    QString sessionId = config["sessionId"].toString();
    QString username = config["username"].toString().replace("@", "");
    QString savePath = config["path"].toString();
    savePath.replace("~", QDir::homePath());
    int maxCount = config["count"].toInt(0); // 0 = all
    bool saveExcel = config["excel"].toBool(true);
    QString igType = config["type"].toString("posts");

    // "all" 모드: reels/stories/highlights 전부 활성화
    QJsonObject effectiveConfig = config;
    if (igType == "all") {
        effectiveConfig["reels"] = true;
        effectiveConfig["stories"] = true;
        effectiveConfig["highlights"] = true;
    }

    // Adaptive delay system
    double igDelay = 2.0;       // base delay
    int igConsecutiveOk = 0;
    int igRateLimitHits = 0;

    if (igType == "all")
        log("═══ 전체 수집 모드 (포스트+릴스+스토리+하이라이트) ═══", "success", "instagram");
    log("Connecting to Instagram...", "info", "instagram");

    HttpClient http;
    http.setRunFlag(&m_isRunning["instagram"]);  // 중지 시 즉시 abort
    QMap<QString, QString> baseHeaders;
    baseHeaders["User-Agent"] = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36";
    baseHeaders["Cookie"] = "sessionid=" + sessionId;
    baseHeaders["X-IG-App-ID"] = "936619743392459";

    // ★ Cookie 우선순위:
    //   1) config["captureCookie"] — 사용자가 [Instagram] 버튼으로 추출한 전체 cookie (UI 표시됨)
    //      또는 직접 입력한 raw cookie
    //   2) extractInstagramSessionSync() — 위 둘 다 없으면 즉시 Chrome 에서 자동 추출
    //   3) UI 의 sessionid input — fallback (이게 부족하면 401)
    QString userCaptureCookie = config["captureCookie"].toString();
    if (!userCaptureCookie.isEmpty() && userCaptureCookie.contains("sessionid=")) {
        baseHeaders["Cookie"] = userCaptureCookie;
        log(QString("🍪 사용자 입력 cookie 사용 (%1개)").arg(userCaptureCookie.count(';') + 1),
            "info", "instagram");
        for (const QString &part : userCaptureCookie.split(';', Qt::SkipEmptyParts)) {
            QString p = part.trimmed();
            if (p.startsWith("sessionid=")) { sessionId = p.mid(10); break; }
        }
    } else {
        QString preFullCookie = extractInstagramSessionSync();
        if (!preFullCookie.isEmpty()) {
            baseHeaders["Cookie"] = preFullCookie;
            log(QString("🍪 Chrome 에서 인스타 쿠키 자동 추출 (%1개)").arg(preFullCookie.count(';') + 1),
                "info", "instagram");
            for (const QString &part : preFullCookie.split(';', Qt::SkipEmptyParts)) {
                QString p = part.trimmed();
                if (p.startsWith("sessionid=")) { sessionId = p.mid(10); break; }
            }
        }
    }

    // Get user info
    QString userInfoUrl = QString("https://i.instagram.com/api/v1/users/web_profile_info/?username=%1").arg(username);
    HttpResponse resp = http.get(userInfoUrl, baseHeaders);

    // ★ 401 시 한 번 더 자동 갱신 + 재시도
    if (!resp.isOk() && resp.statusCode == 401) {
        log("get user info 401 — 세션 자동 갱신 시도", "warning", "instagram");
        QString fullCookie = extractInstagramSessionSync();
        QString currentCookie = baseHeaders["Cookie"];
        if (!fullCookie.isEmpty() && fullCookie != currentCookie) {
            baseHeaders["Cookie"] = fullCookie;
            log("✅ 갱신 후 재시도...", "info", "instagram");
            resp = http.get(userInfoUrl, baseHeaders);
        }
    }

    if (!resp.isOk()) {
        log(QString("Failed to get user info (HTTP %1)").arg(resp.statusCode), "error", "instagram");
        if (resp.statusCode == 401) {
            log("  → Chrome 에서 instagram.com 로그인 상태 확인 필요", "info", "instagram");
            log("  → 또는 인스타 탭 → 'capture cookie' 필드에 직접 입력 (sessionid + csrftoken 등)", "info", "instagram");
        }
        return;
    }

    QJsonObject userData = resp.json()["data"].toObject()["user"].toObject();
    QString userId = userData["id"].toString();
    QString fullName = userData["full_name"].toString();
    int totalPosts = userData["edge_owner_to_timeline_media"].toObject()["count"].toInt();
    int followerCount = userData["edge_followed_by"].toObject()["count"].toInt();
    int followingCount = userData["edge_follow"].toObject()["count"].toInt();
    QString biography = userData["biography"].toString();
    QString externalUrl = userData["external_url"].toString();
    bool isVerified = userData["is_verified"].toBool();
    bool isPrivate = userData["is_private"].toBool();

    log(QString("User: %1 (%2) - %3 posts%4%5")
        .arg(username, fullName).arg(totalPosts)
        .arg(isVerified ? " ✓" : "")
        .arg(isPrivate ? " [비공개]" : ""), "success", "instagram");
    if (!biography.isEmpty()) {
        log(QString("  📝 %1").arg(biography.left(100)), "info", "instagram");
    }

    QString instagramDir = savePath + "/instagram";
    QDir().mkpath(instagramDir);
    QString userDir = instagramDir + "/" + username;
    QDir().mkpath(userDir);
    // ── 타입별 폴더 분리 (posts/reels/stories/highlights) ──
    QString mediaDir      = FileHelper::typeFolder(userDir, "posts");       // 포스트 (기존 "media" → "posts")
    QString reelsDir      = FileHelper::typeFolder(userDir, "reels");       // 릴스
    QString storiesDir    = FileHelper::typeFolder(userDir, "stories");     // 스토리
    QString highlightsDir = FileHelper::typeFolder(userDir, "highlights");  // 하이라이트
    QString completeDir   = FileHelper::typeFolder(userDir, "all");         // _complete (ALL 모드)

    // ── 프로필 다운로드 (아바타, 설명, 프로필 정보) — 날짜별 보관 ──
    {
        QString dateTag = QDateTime::currentDateTime().toString("yyyyMMdd");
        QString profileDir = userDir + "/profiles/" + QDate::currentDate().toString("yyyy-MM-dd");
        QDir().mkpath(profileDir);

        // 프로필 사진 (HD)
        QString profilePicUrl = userData["profile_pic_url_hd"].toString();
        if (profilePicUrl.isEmpty()) {
            QJsonArray hdVersions = userData["hd_profile_pic_versions"].toArray();
            if (!hdVersions.isEmpty()) {
                int maxW = 0;
                for (const auto &v : hdVersions) {
                    int w = v.toObject()["width"].toInt();
                    if (w > maxW) {
                        maxW = w;
                        profilePicUrl = v.toObject()["url"].toString();
                    }
                }
            }
            if (profilePicUrl.isEmpty())
                profilePicUrl = userData["profile_pic_url"].toString();
        }

        if (!profilePicUrl.isEmpty()) {
            QString ext = "jpg";
            if (profilePicUrl.contains(".png")) ext = "png";
            else if (profilePicUrl.contains(".webp")) ext = "webp";
            // 날짜별 프로필 폴더에 저장
            QString avatarPath = profileDir + "/avatar_" + dateTag + "." + ext;
            if (!QFile::exists(avatarPath)) {
                if (http.downloadFile(profilePicUrl, avatarPath, baseHeaders)) {
                    Common::addExifMetadata(avatarPath, "@" + username, fullName + " - " + biography.left(100),
                        "Instagram @" + username, "https://instagram.com/" + username, "");
                    log(QString("📸 프로필 사진 저장: profiles/%1/avatar_%2.%3")
                        .arg(QDate::currentDate().toString("yyyy-MM-dd"), dateTag, ext), "success", "instagram");
                }
            }
        }

        // 프로필 정보 JSON 저장 (날짜별)
        QJsonObject profileData;
        profileData["user_id"] = userId;
        profileData["username"] = username;
        profileData["full_name"] = fullName;
        profileData["biography"] = biography;
        profileData["external_url"] = externalUrl;
        profileData["is_verified"] = isVerified;
        profileData["is_private"] = isPrivate;
        profileData["follower_count"] = followerCount;
        profileData["following_count"] = followingCount;
        profileData["post_count"] = totalPosts;
        profileData["profile_pic_url"] = profilePicUrl;
        profileData["category_name"] = userData["category_name"].toString();
        profileData["pronouns"] = QString::fromUtf8(QJsonDocument(userData["pronouns"].toArray()).toJson(QJsonDocument::Compact));
        profileData["fetched_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);

        // 날짜별 profile.json
        QString profileJsonPath = profileDir + "/profile.json";
        QFile pf(profileJsonPath);
        if (pf.open(QIODevice::WriteOnly)) {
            pf.write(QJsonDocument(profileData).toJson(QJsonDocument::Indented));
            pf.close();
            log("📄 profile.json 저장", "success", "instagram");
        }

        // 최신 profile.json도 루트에 유지 (빠른 참조용)
        QString rootProfilePath = userDir + "/profile.json";
        QFile rpf(rootProfilePath);
        if (rpf.open(QIODevice::WriteOnly)) {
            rpf.write(QJsonDocument(profileData).toJson(QJsonDocument::Indented));
            rpf.close();
        }
    }

    // 저장 경로 안에 생성 (시스템 데이터 방지)
    QString igBufTempDir;
    {
        QString userTmp = m_config ? m_config->tempDir() : QString();
        if (!userTmp.isEmpty() && QDir(userTmp).exists()) igBufTempDir = userTmp + "/abiwa_instagram_" + username;
        else igBufTempDir = userDir + "/.abiwa_tmp";
    }
    QDir().mkpath(igBufTempDir);
    DiskJsonBuffer allMedia(igBufTempDir, "instagram");
    int mediaDownloaded = 0;
    int imgCount = 0, vidCount = 0, carouselCount = 0;
    QString nextMaxId;
    bool hasMore = true;

    // Use API v1 feed endpoint (more reliable than GraphQL query_hash)
    while (hasMore && m_isRunning.value("instagram", false)) {
        QString feedUrl = QString("https://i.instagram.com/api/v1/feed/user/%1/?count=12").arg(userId);
        if (!nextMaxId.isEmpty()) feedUrl += "&max_id=" + nextMaxId;

        HttpResponse mediaResp = http.get(feedUrl, baseHeaders);
        if (!mediaResp.isOk()) {
            if (mediaResp.statusCode == 429) {
                igRateLimitHits++;
                igConsecutiveOk = 0;
                igDelay = qMin(igDelay * 2.0, 30.0);
                int waitSecs = qMin(60 + (igRateLimitHits - 1) * 30, 180);
                log(QString("⚠️ Rate Limit (%1回) - %2秒 대기 (適応ﾃﾞｨﾚｲ: %3秒)")
                    .arg(igRateLimitHits).arg(waitSecs).arg(igDelay, 0, 'f', 1), "warning", "instagram");
                for (int r = waitSecs; r > 0 && m_isRunning.value("instagram", false); --r) {
                    updateStats(allMedia.count(), mediaDownloaded, QString("대기 %1s").arg(r), "instagram");
                    QThread::sleep(1);
                }
                continue;
            }
            // 401: 세션 만료 → Chrome에서 자동 갱신 시도
            if (mediaResp.statusCode == 401) {
                log("세션 만료 (401) → Chrome에서 세션 자동 갱신 중...", "warning", "instagram");
                // ★ 반환값이 이제 전체 Cookie header (sessionid+csrftoken+ds_user_id+ig_did 등)
                QString fullCookie = extractInstagramSessionSync();
                if (!fullCookie.isEmpty()) {
                    // sessionid 부분 따로 추출 (UI 표시용)
                    QString sid;
                    for (const QString &part : fullCookie.split(';', Qt::SkipEmptyParts)) {
                        QString p = part.trimmed();
                        if (p.startsWith("sessionid=")) { sid = p.mid(10); break; }
                    }
                    // 두 번째 시도면서 같은 sessionid 라면 정말 실패
                    if (sid.isEmpty() || sid == sessionId) {
                        log("세션 자동 갱신 실패 — Chrome 에서 instagram.com 로그인 상태 확인. 또는 csrftoken 등 부족.",
                            "error", "instagram");
                        log("  대안: 인스타 탭 → 'sessionid + 추가 cookie' 직접 입력 (capture cookie 필드)",
                            "info", "instagram");
                        break;
                    }
                    sessionId = sid;
                    baseHeaders["Cookie"] = fullCookie;  // ★ 전체 Cookie header (모든 인스타 쿠키)
                    log(QString("✅ 세션 갱신 성공! (쿠키 %1개) 이어서 수집...")
                        .arg(fullCookie.count(';') + 1), "success", "instagram");
                    // UI 업데이트 — JS-safe 인코딩
                    QString safeSid = Common::jsStringLiteral(sessionId);
                    runJs(QString("document.getElementById('instagram-session-id').value=%1;"
                                  "if(accounts.instagram && accounts.instagram.length>0) accounts.instagram[0].session_id=%1;"
                                  "saveConfig();").arg(safeSid));
                    continue;  // 갱신된 세션으로 재시도
                }
                log("세션 자동 갱신 실패. Chrome에서 Instagram에 로그인 후 다시 시도하세요.", "error", "instagram");
            } else {
                log(QString("API error: %1").arg(mediaResp.statusCode), "error", "instagram");
            }
            break;
        }

        QJsonObject respData = mediaResp.json();
        hasMore = respData["more_available"].toBool(false);
        nextMaxId = respData["next_max_id"].toString();

        QJsonArray items = respData["items"].toArray();
        if (items.isEmpty()) break;

        for (const auto &itemVal : items) {
            if (!m_isRunning.value("instagram", false)) break;
            QJsonObject node = itemVal.toObject();

            int mediaType = node["media_type"].toInt();
            QString code = node["code"].toString();
            // Timestamp for file metadata
            qint64 takenAt = node["taken_at"].toVariant().toLongLong();
            if (takenAt == 0) takenAt = node["taken_at_timestamp"].toVariant().toLongLong();
            QDateTime postDt = takenAt > 0 ? QDateTime::fromSecsSinceEpoch(takenAt, QTimeZone::utc()) : QDateTime();
            QString postDateStr = postDt.isValid() ? postDt.toString(Qt::ISODate) : "";
            // Caption for EXIF
            QString caption;
            QJsonObject capObj = node["caption"].toObject();
            if (!capObj.isEmpty()) caption = capObj["text"].toString().left(200);
            QString postUrl = code.isEmpty() ? "" : QString("https://www.instagram.com/p/%1").arg(code);

            // ★ 진짜 페이지 캡쳐 — config["realCapture"]=true일 때 브라우저로 방문해서 저장
            if (config["realCapture"].toBool(true) && !postUrl.isEmpty() && !code.isEmpty()) {
                QString igCapturesDir = userDir + "/captures";
                QString igFilename = code;  // {shortcode}.html
                QList<QNetworkCookie> igCookies;
                QString sid = config["sessionId"].toString();
                if (sid.isEmpty() && !config["accounts"].toArray().isEmpty()) {
                    sid = config["accounts"].toArray()[0].toObject()["sessionId"].toString();
                }
                if (!sid.isEmpty()) {
                    QNetworkCookie c("sessionid", sid.toUtf8());
                    c.setDomain(".instagram.com"); c.setPath("/"); c.setSecure(true);
                    igCookies << c;
                }
                // ★ 사용자가 UI에서 입력한 추가 raw cookie (NSFW/private 통과: ds_user_id, csrftoken 등)
                QString extraIg = config["captureCookie"].toString();
                if (!extraIg.isEmpty()) {
                    for (const QString &part : extraIg.split(';', Qt::SkipEmptyParts)) {
                        int eq = part.indexOf('=');
                        if (eq <= 0) continue;
                        QNetworkCookie ec(part.left(eq).trimmed().toUtf8(),
                                           part.mid(eq + 1).trimmed().toUtf8());
                        ec.setDomain(".instagram.com"); ec.setPath("/"); ec.setSecure(true);
                        igCookies << ec;
                    }
                }
                // ★ 로그인 대기 가능 캡쳐 — 로그인 페이지 감지 시 사용자 GUI 확인 대기
                //   인스타 로그인 체크: /accounts/login/ 경로 또는 username 입력란 존재
                static const QString igLoginCheck = R"JS(
                    (function(){
                        if (location.pathname.indexOf('/accounts/login') === 0) return true;
                        // 포스트 페이지가 정상 로드됐으면 로그인된 상태 (article 또는 main image)
                        if (document.querySelector('article, main img[alt][src*="cdninstagram"]')) return false;
                        return !!document.querySelector('input[name="username"]');
                    })()
                )JS";
                captureRealPageCDPLoginAware(postUrl, igCapturesDir, igFilename,
                                              igLoginCheck, "instagram", 8000, igCookies, config);
            }

            auto setIgMeta = [&](const QString &fp) {
                // EXIF → Finder comment → xattr+mtime
                Common::addExifMetadata(fp, "@" + username, caption,
                    "Instagram @" + username, postUrl, postDateStr);
                if (!postUrl.isEmpty()) FileHelper::setFinderComment(fp, postUrl);
                if (postDt.isValid())
                    FileHelper::applyPostMetadata(fp, postDt, postUrl);
                else if (!postUrl.isEmpty())
                    FileHelper::setDownloadMeta(fp, postUrl);
            };
            // _complete mirror helper
            auto mirrorToComplete = [&](const QString &fp) {
                QString fname = QFileInfo(fp).fileName();
                QString cp = completeDir + "/" + fname;
                if (!QFile::exists(cp)) {
                    QFile::copy(fp, cp);
                    if (postDt.isValid()) FileHelper::applyPostMetadata(cp, postDt, postUrl);
                    if (!postUrl.isEmpty()) FileHelper::setFinderComment(cp, postUrl);
                }
            };

            // 파일명: [업로드순서prefix] 캡션 (코드).확장자
            // prefix로 업로드 순서 정렬 가능 ("20260411_153042_")
            auto igFilename = [&](const QString &ext, int idx = -1) -> QString {
                QString title = caption.left(40).trimmed();
                title.replace('\n', ' ');
                title = sanitizeFilename(title, 40);
                if (title.isEmpty()) title = username;
                QString prefix = FileHelper::uploadOrderPrefix(postDt.isValid() ? postDt.toUTC().addSecs(9*3600) : QDateTime());
                QString name = prefix + title + " (" + code;
                if (idx >= 0) name += "_" + QString::number(idx);
                name += ")." + ext;
                return sanitizeFilename(name, 200);
            };

            bool nodeDownloaded = false; // Excel용: 실제 다운로드 성공 여부

            if (mediaType == 8) {
                // Carousel
                carouselCount++;
                QJsonArray carousel = node["carousel_media"].toArray();
                if (carousel.isEmpty()) {
                    log(QString("⚠️ 카루셀인데 carousel_media 비어있음 (code=%1)").arg(code), "warning", "instagram");
                }
                for (int ci = 0; ci < carousel.size(); ++ci) {
                    QJsonObject cItem = carousel[ci].toObject();
                    int cType = cItem["media_type"].toInt();
                    QString cUrl;
                    QString cExt;
                    if (cType == 2) {
                        QJsonArray vv = cItem["video_versions"].toArray();
                        if (!vv.isEmpty()) cUrl = vv[0].toObject()["url"].toString();
                        cExt = "mp4";
                    }
                    if (cUrl.isEmpty()) {
                        QJsonArray ic = cItem["image_versions2"].toObject()["candidates"].toArray();
                        if (!ic.isEmpty()) {
                            cUrl = ic[0].toObject()["url"].toString();
                            if (cType != 2) cExt = "jpg";
                        }
                    }
                    if (cUrl.isEmpty()) {
                        log(QString("⚠️ 카루셀 아이템 %1/%2 URL 없음 (code=%3, type=%4)")
                            .arg(ci).arg(carousel.size()).arg(code).arg(cType), "warning", "instagram");
                        continue;
                    }
                    if (cExt.isEmpty()) cExt = cUrl.contains("mp4") ? "mp4" : "jpg";
                    QString fp = mediaDir + "/" + igFilename(cExt, ci);
                    if (QFile::exists(fp)) {
                        nodeDownloaded = true; // 이미 존재하는 파일도 성공으로 간주
                    } else if (http.downloadFile(cUrl, fp)) {
                        setIgMeta(fp);
                        mirrorToComplete(fp);
                        mediaDownloaded++;
                        nodeDownloaded = true;
                        if (cType == 2) vidCount++; else imgCount++;
                    }
                }
            } else if (mediaType == 2) {
                // Video
                QString vUrl;
                QJsonArray vv = node["video_versions"].toArray();
                if (!vv.isEmpty()) {
                    vUrl = vv[0].toObject()["url"].toString();
                }
                if (vUrl.isEmpty()) {
                    QJsonArray ic = node["image_versions2"].toObject()["candidates"].toArray();
                    if (!ic.isEmpty()) {
                        vUrl = ic[0].toObject()["url"].toString();
                        log(QString("⚠️ 비디오 video_versions 없음, 썸네일 저장 (code=%1)").arg(code), "warning", "instagram");
                        QString fp = mediaDir + "/" + igFilename("jpg");
                        if (QFile::exists(fp)) {
                            nodeDownloaded = true;
                        } else if (!vUrl.isEmpty() && http.downloadFile(vUrl, fp)) {
                            setIgMeta(fp);
                            mirrorToComplete(fp);
                            mediaDownloaded++;
                            imgCount++;
                            nodeDownloaded = true;
                        }
                        vUrl.clear();
                    } else {
                        log(QString("⚠️ 비디오 URL 없음 (code=%1)").arg(code), "warning", "instagram");
                    }
                }
                if (!vUrl.isEmpty()) {
                    QString fp = mediaDir + "/" + igFilename("mp4");
                    if (QFile::exists(fp)) {
                        nodeDownloaded = true;
                    } else if (http.downloadFile(vUrl, fp)) {
                        setIgMeta(fp);
                        mirrorToComplete(fp);
                        mediaDownloaded++;
                        vidCount++;
                        nodeDownloaded = true;
                    }
                }
            } else {
                // Photo (mediaType 1 or unknown)
                QJsonArray ic = node["image_versions2"].toObject()["candidates"].toArray();
                if (ic.isEmpty()) {
                    log(QString("⚠️ 이미지 candidates 비어있음 (code=%1, type=%2)").arg(code).arg(mediaType), "warning", "instagram");
                }
                if (!ic.isEmpty()) {
                    QString iUrl = ic[0].toObject()["url"].toString();
                    QString fp = mediaDir + "/" + igFilename("jpg");
                    if (QFile::exists(fp)) {
                        nodeDownloaded = true;
                    } else if (!iUrl.isEmpty() && http.downloadFile(iUrl, fp)) {
                        setIgMeta(fp);
                        mirrorToComplete(fp);
                        mediaDownloaded++;
                        imgCount++;
                        nodeDownloaded = true;
                    }
                }
            }

            // 経済産業省 연계: 포스트 페이지 캡처 — SingleFile CDP + 로그인 쿠키 (private/age-gated 컨텐츠)
            // ★ 미디어 유무 무관 — 모든 포스트 캡쳐
            if (!postUrl.isEmpty() && config["realCapture"].toBool(true)) {
                QString capturesDir = userDir + "/captures";
                QList<QNetworkCookie> ckList;
                QString sid = config["sessionId"].toString();
                if (!sid.isEmpty()) {
                    QNetworkCookie c("sessionid", sid.toUtf8());
                    c.setDomain(".instagram.com"); c.setPath("/"); c.setSecure(true);
                    ckList << c;
                }
                // ★ 추가 raw cookie 합치기
                QString extraIg2 = config["captureCookie"].toString();
                if (!extraIg2.isEmpty()) {
                    for (const QString &part : extraIg2.split(';', Qt::SkipEmptyParts)) {
                        int eq = part.indexOf('=');
                        if (eq <= 0) continue;
                        QNetworkCookie ec(part.left(eq).trimmed().toUtf8(),
                                           part.mid(eq + 1).trimmed().toUtf8());
                        ec.setDomain(".instagram.com"); ec.setPath("/"); ec.setSecure(true);
                        ckList << ec;
                    }
                }
                static const QString igLoginCheck2 = R"JS(
                    (function(){
                        if (location.pathname.indexOf('/accounts/login') === 0) return true;
                        // 포스트 페이지가 정상 로드됐으면 로그인된 상태 (article 또는 main image)
                        if (document.querySelector('article, main img[alt][src*="cdninstagram"]')) return false;
                        return !!document.querySelector('input[name="username"]');
                    })()
                )JS";
                captureRealPageCDPLoginAware(postUrl, capturesDir,
                    FileHelper::uploadOrderPrefix(postDt) + code,
                    igLoginCheck2, "instagram", 8000, ckList, config);
            }

            // Excel: 다운로드 성공 또는 이미 존재하는 항목만 포함
            if (nodeDownloaded) {
                allMedia.append(node);
            }

            updateStats(allMedia.count(), mediaDownloaded, "수집 中", "instagram");
            if (maxCount > 0 && allMedia.count() >= maxCount) { hasMore = false; break; }
        }

        // Detailed batch log with date — use last item's date
        QDateTime batchDt;
        if (!items.isEmpty()) {
            qint64 lastTa = items.last().toObject()["taken_at"].toVariant().toLongLong();
            if (lastTa > 0) batchDt = QDateTime::fromSecsSinceEpoch(lastTa, QTimeZone::utc());
        }
        QString dateInfo;
        if (batchDt.isValid()) {
            dateInfo = QString(" | 📅 %1").arg(batchDt.toUTC().addSecs(9*3600).toString("yyyy/MM/dd HH:mm"));
        }
        QStringList typeInfo;
        if (imgCount > 0) typeInfo << QString("画像%1").arg(imgCount);
        if (vidCount > 0) typeInfo << QString("動画%1").arg(vidCount);
        if (carouselCount > 0) typeInfo << QString("ｶﾙｰｾﾙ%1").arg(carouselCount);
        QString typeSummary = typeInfo.isEmpty() ? "" : " [" + typeInfo.join("/") + "]";

        log(QString("投稿 %1/%2件 | ﾒﾃﾞｨｱ %3件%4%5")
            .arg(allMedia.count()).arg(totalPosts).arg(mediaDownloaded).arg(typeSummary).arg(dateInfo), "info", "instagram");

        // 중간 저장: 100개마다 Excel 체크포인트 (강제 종료 대비)
        if (saveExcel && allMedia.count() > 0 && allMedia.count() % 100 < 12) {
            log(QString("💾 중간 저장 (%1건)").arg(allMedia.count()), "info", "instagram");
            QJsonArray tmpArray = allMedia.readAll();
            ExcelWriter tmpWriter;
            QStringList tmpHdrs = {"Shortcode", "Timestamp", "Type", "Likes", "Comments", "Caption", "URL"};
            tmpWriter.writeHeader(tmpHdrs, QColor("#E4405F"));
            int tmpRow = 2;
            for (const auto &val : tmpArray) {
                QJsonObject nd = val.toObject();
                QString cap;
                QJsonArray capEdges = nd["edge_media_to_caption"].toObject()["edges"].toArray();
                if (!capEdges.isEmpty()) cap = capEdges[0].toObject()["node"].toObject()["text"].toString().left(200);
                if (cap.isEmpty()) cap = nd["caption"].toObject()["text"].toString().left(200);
                QString sc = nd["shortcode"].toString();
                if (sc.isEmpty()) sc = nd["code"].toString();
                qint64 ts = nd["taken_at_timestamp"].toVariant().toLongLong();
                if (ts == 0) ts = nd["taken_at"].toVariant().toLongLong();
                tmpWriter.writeRow(tmpRow++, {
                    sc,
                    ts > 0 ? QDateTime::fromSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm:ss") : "",
                    nd["is_video"].toBool() || nd["media_type"].toInt() == 2 ? "Video" : "Photo",
                    QString::number(nd["edge_liked_by"].toObject()["count"].toInt() + nd["like_count"].toInt()),
                    QString::number(nd["edge_media_to_comment"].toObject()["count"].toInt() + nd["comment_count"].toInt()),
                    cap,
                    sc.isEmpty() ? "" : "https://www.instagram.com/p/" + sc
                });
            }
            tmpWriter.autoFitColumns(tmpHdrs);
            QString tmpExcelDir = userDir + "/excel";
            QDir().mkpath(tmpExcelDir);
            QString tmpExcelPath = FileHelper::typeExcelPath(tmpExcelDir, username, igType);
            tmpWriter.save(tmpExcelPath);
        }

        igConsecutiveOk++; if (igConsecutiveOk > 5) { igDelay = qMax(igDelay * 0.9, 1.0); igRateLimitHits = 0; } QThread::msleep(static_cast<unsigned long>(igDelay * 1000));
    }

    {
        QStringList finalTypes;
        if (imgCount > 0) finalTypes << QString("画像%1").arg(imgCount);
        if (vidCount > 0) finalTypes << QString("動画%1").arg(vidCount);
        if (carouselCount > 0) finalTypes << QString("ｶﾙｰｾﾙ%1").arg(carouselCount);
        QString finalSummary = finalTypes.isEmpty() ? "" : " [" + finalTypes.join("/") + "]";
        log("━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━", "success", "instagram");
        log(QString("投稿収集完了: %1/%2件 | ﾒﾃﾞｨｱ %3件%4")
            .arg(allMedia.count()).arg(totalPosts).arg(mediaDownloaded).arg(finalSummary), "success", "instagram");
    }

    // ── Reels 수집 ──
    if (effectiveConfig["reels"].toBool(false) && m_isRunning.value("instagram", false)) {
        log("릴스 수집 중...", "info", "instagram");
        QString reelsUrl = QString("https://i.instagram.com/api/v1/clips/user/?target_user_id=%1&page_size=12").arg(userId);
        int reelsCount = 0;
        QString reelsMaxId;

        while (m_isRunning.value("instagram", false)) {
            QString reqUrl = reelsUrl;
            if (!reelsMaxId.isEmpty()) reqUrl += "&max_id=" + reelsMaxId;

            HttpResponse reelsResp = http.get(reqUrl, baseHeaders);
            if (!reelsResp.isOk()) break;

            QJsonObject reelsData = reelsResp.json();
            QJsonArray items = reelsData["items"].toArray();
            if (items.isEmpty()) break;

            for (const auto &item : items) {
                QJsonObject media = item.toObject()["media"].toObject();
                QString videoUrl;
                QJsonArray videoVersions = media["video_versions"].toArray();
                if (!videoVersions.isEmpty()) {
                    videoUrl = videoVersions[0].toObject()["url"].toString();
                }
                if (!videoUrl.isEmpty()) {
                    QString code = media["code"].toString();
                    qint64 reelTa = media["taken_at"].toVariant().toLongLong();
                    QDateTime reelDt = reelTa > 0 ? QDateTime::fromSecsSinceEpoch(reelTa, QTimeZone::utc()) : QDateTime();
                    QString reelDate = reelDt.isValid() ? reelDt.toUTC().addSecs(9*3600).toString("yyyyMMdd_HHmm") : "";
                    QString reelName = sanitizeFilename(username + " (" + code + (reelDate.isEmpty() ? "" : "_" + reelDate) + "_reel).mp4", 200);
                    QString reelPostUrl = code.isEmpty() ? "" : QString("https://www.instagram.com/reel/%1/").arg(code);
                    QString filepath = reelsDir + "/" + reelName;
                    if (!QFile::exists(filepath) && http.downloadFile(videoUrl, filepath)) {
                        qint64 ta = media["taken_at"].toVariant().toLongLong();
                        QDateTime rDt = ta > 0 ? QDateTime::fromSecsSinceEpoch(ta, QTimeZone::utc()) : QDateTime();
                        // EXIF → Finder comment → xattr+mtime
                        Common::addExifMetadata(filepath, "@" + username, "",
                            "Instagram @" + username, reelPostUrl, rDt.isValid() ? rDt.toString(Qt::ISODate) : "");
                        if (!reelPostUrl.isEmpty()) FileHelper::setFinderComment(filepath, reelPostUrl);
                        if (rDt.isValid()) FileHelper::applyPostMetadata(filepath, rDt, reelPostUrl);
                        // _complete mirror
                        QString cp = completeDir + "/" + reelName;
                        if (!QFile::exists(cp)) {
                            QFile::copy(filepath, cp);
                            if (rDt.isValid()) FileHelper::applyPostMetadata(cp, rDt, reelPostUrl);
                            if (!reelPostUrl.isEmpty()) FileHelper::setFinderComment(cp, reelPostUrl);
                        }
                        mediaDownloaded++;
                        reelsCount++;
                    }
                }
                allMedia.append(media);
            }

            reelsMaxId = reelsData["paging_info"].toObject()["max_id"].toString();
            if (reelsMaxId.isEmpty() || !reelsData["paging_info"].toObject()["more_available"].toBool()) break;
            updateStats(allMedia.count(), mediaDownloaded, "수집 중", "instagram");
            igConsecutiveOk++; if (igConsecutiveOk > 5) { igDelay = qMax(igDelay * 0.9, 1.0); igRateLimitHits = 0; } QThread::msleep(static_cast<unsigned long>(igDelay * 1000));
        }
        log(QString("릴스: %1개").arg(reelsCount), "success", "instagram");
    }

    // ── Stories 수집 ──
    if (effectiveConfig["stories"].toBool(false) && m_isRunning.value("instagram", false)) {
        log("스토리 수집 중...", "info", "instagram");
        QString storiesUrl = QString("https://i.instagram.com/api/v1/feed/reels_media/?reel_ids=%1").arg(userId);
        HttpResponse storiesResp = http.get(storiesUrl, baseHeaders);
        if (storiesResp.isOk()) {
            QJsonArray reels = storiesResp.json()["reels_media"].toArray();
            int storiesCount = 0;
            for (const auto &reel : reels) {
                QJsonArray storyItems = reel.toObject()["items"].toArray();
                for (const auto &storyItem : storyItems) {
                    QJsonObject story = storyItem.toObject();
                    QString storyUrl;
                    if (story["media_type"].toInt() == 2) {
                        QJsonArray vv = story["video_versions"].toArray();
                        if (!vv.isEmpty()) storyUrl = vv[0].toObject()["url"].toString();
                    } else {
                        QJsonArray ic = story["image_versions2"].toObject()["candidates"].toArray();
                        if (!ic.isEmpty()) storyUrl = ic[0].toObject()["url"].toString();
                    }
                    if (!storyUrl.isEmpty()) {
                        QString ext = story["media_type"].toInt() == 2 ? "mp4" : "jpg";
                        qint64 storyTa = story["taken_at"].toVariant().toLongLong();
                        QDateTime storyDt = storyTa > 0 ? QDateTime::fromSecsSinceEpoch(storyTa, QTimeZone::utc()) : QDateTime();
                        QString storyDate = storyDt.isValid() ? storyDt.toUTC().addSecs(9*3600).toString("yyyyMMdd_HHmm") : "";
                        QString storyName = sanitizeFilename(username + " (" + story["pk"].toString() + (storyDate.isEmpty() ? "" : "_" + storyDate) + "_story)." + ext, 200);
                        QString storyPostUrl = QString("https://www.instagram.com/stories/%1/").arg(username);
                        QString filepath = storiesDir + "/" + storyName;
                        if (!QFile::exists(filepath) && http.downloadFile(storyUrl, filepath)) {
                            qint64 ta = story["taken_at"].toVariant().toLongLong();
                            QDateTime sDt = ta > 0 ? QDateTime::fromSecsSinceEpoch(ta, QTimeZone::utc()) : QDateTime();
                            // EXIF → Finder comment → xattr+mtime
                            Common::addExifMetadata(filepath, "@" + username, "",
                                "Instagram @" + username, storyPostUrl, sDt.isValid() ? sDt.toString(Qt::ISODate) : "");
                            FileHelper::setFinderComment(filepath, storyPostUrl);
                            if (sDt.isValid()) FileHelper::applyPostMetadata(filepath, sDt, storyPostUrl);
                            // _complete mirror
                            QString cp = completeDir + "/" + storyName;
                            if (!QFile::exists(cp)) {
                                QFile::copy(filepath, cp);
                                if (sDt.isValid()) FileHelper::applyPostMetadata(cp, sDt, storyPostUrl);
                                FileHelper::setFinderComment(cp, storyPostUrl);
                            }
                            mediaDownloaded++;
                            storiesCount++;
                        }
                    }
                }
            }
            log(QString("스토리: %1개").arg(storiesCount), "success", "instagram");
        }
    }

    // ── Highlights 수집 ──
    if (effectiveConfig["highlights"].toBool(false) && m_isRunning.value("instagram", false)) {
        log("하이라이트 수집 중...", "info", "instagram");

        // Step 1: Get highlight tray (list of highlight reels)
        QString highlightsTrayUrl = QString("https://i.instagram.com/api/v1/highlights/%1/highlights_tray/").arg(userId);
        HttpResponse hlResp = http.get(highlightsTrayUrl, baseHeaders);
        int highlightsMediaCount = 0;

        if (hlResp.isOk()) {
            QJsonArray tray = hlResp.json()["tray"].toArray();
            log(QString("하이라이트: %1개 발견").arg(tray.size()), "info", "instagram");

            for (int hi = 0; hi < tray.size() && m_isRunning.value("instagram", false); ++hi) {
                QJsonObject highlight = tray[hi].toObject();
                QString highlightId = highlight["id"].toString();
                QString highlightTitle = highlight["title"].toString();
                if (highlightTitle.isEmpty()) highlightTitle = QString("highlight_%1").arg(hi);

                log(QString("  [%1/%2] %3").arg(hi + 1).arg(tray.size()).arg(highlightTitle), "info", "instagram");

                // Step 2: Get items for each highlight reel
                QString hlItemsUrl = QString("https://i.instagram.com/api/v1/feed/reels_media/?reel_ids=highlight:%1").arg(highlightId.contains(":") ? highlightId.split(":").last() : highlightId);
                HttpResponse hlItemsResp = http.get(hlItemsUrl, baseHeaders);

                if (hlItemsResp.isOk()) {
                    QJsonObject reelsMedia = hlItemsResp.json()["reels_media"].toArray().isEmpty()
                        ? QJsonObject()
                        : hlItemsResp.json()["reels_media"].toArray()[0].toObject();

                    if (reelsMedia.isEmpty()) {
                        // Try with the "reels" key
                        QJsonObject reels = hlItemsResp.json()["reels"].toObject();
                        QString reelKey = QString("highlight:%1").arg(highlightId.contains(":") ? highlightId.split(":").last() : highlightId);
                        reelsMedia = reels[reelKey].toObject();
                    }

                    QJsonArray hlItems = reelsMedia["items"].toArray();
                    for (const auto &hlItem : hlItems) {
                        if (!m_isRunning.value("instagram", false)) break;
                        QJsonObject story = hlItem.toObject();
                        QString storyUrl;
                        QString ext;

                        if (story["media_type"].toInt() == 2) {
                            QJsonArray vv = story["video_versions"].toArray();
                            if (!vv.isEmpty()) storyUrl = vv[0].toObject()["url"].toString();
                            ext = "mp4";
                        } else {
                            QJsonArray ic = story["image_versions2"].toObject()["candidates"].toArray();
                            if (!ic.isEmpty()) storyUrl = ic[0].toObject()["url"].toString();
                            ext = "jpg";
                        }

                        if (!storyUrl.isEmpty()) {
                            qint64 hlTa = story["taken_at"].toVariant().toLongLong();
                            QDateTime hlDt = hlTa > 0 ? QDateTime::fromSecsSinceEpoch(hlTa, QTimeZone::utc()) : QDateTime();
                            QString hlDate = hlDt.isValid() ? hlDt.toUTC().addSecs(9*3600).toString("yyyyMMdd_HHmm") : "";
                            // 하이라이트 제목별 서브폴더
                            QString hlSubDir = highlightsDir + "/" + sanitizeFilename(highlightTitle, 80);
                            QDir().mkpath(hlSubDir);
                            QString hlName = sanitizeFilename(story["pk"].toString() + (hlDate.isEmpty() ? "" : "_" + hlDate) + "." + ext, 200);
                            QString hlPostUrl = QString("https://www.instagram.com/stories/highlights/%1/")
                                .arg(highlightId.contains(":") ? highlightId.split(":").last() : highlightId);
                            QString filepath = hlSubDir + "/" + hlName;
                            if (!QFile::exists(filepath) && http.downloadFile(storyUrl, filepath)) {
                                qint64 ta = story["taken_at"].toVariant().toLongLong();
                                QDateTime hDt = ta > 0 ? QDateTime::fromSecsSinceEpoch(ta, QTimeZone::utc()) : QDateTime();
                                // EXIF → Finder comment → xattr+mtime
                                Common::addExifMetadata(filepath, "@" + username, highlightTitle,
                                    "Instagram @" + username, hlPostUrl, hDt.isValid() ? hDt.toString(Qt::ISODate) : "");
                                FileHelper::setFinderComment(filepath, hlPostUrl);
                                if (hDt.isValid()) FileHelper::applyPostMetadata(filepath, hDt, hlPostUrl);
                                // _complete mirror
                                QString cp = completeDir + "/" + hlName;
                                if (!QFile::exists(cp)) {
                                    QFile::copy(filepath, cp);
                                    if (hDt.isValid()) FileHelper::applyPostMetadata(cp, hDt, hlPostUrl);
                                    FileHelper::setFinderComment(cp, hlPostUrl);
                                }
                                mediaDownloaded++;
                                highlightsMediaCount++;
                            }
                        }
                    }
                }

                updateStats(allMedia.count(), mediaDownloaded, "수집 중", "instagram");
                igConsecutiveOk++; if (igConsecutiveOk > 5) { igDelay = qMax(igDelay * 0.9, 1.0); igRateLimitHits = 0; } QThread::msleep(static_cast<unsigned long>(igDelay * 1000));
            }
            log(QString("하이라이트: %1개 다운로드").arg(highlightsMediaCount), "success", "instagram");
        } else {
            log(QString("하이라이트 목록 가져오기 실패 (HTTP %1)").arg(hlResp.statusCode), "error", "instagram");
        }
    }

    updateStats(allMedia.count(), mediaDownloaded, "저장 중", "instagram");

    if (saveExcel && allMedia.count() > 0) {
        log("Excel 저장...", "info", "instagram");
        QStringList hdrs = {"Shortcode", "Timestamp", "Type", "Likes", "Comments", "Caption", "URL"};
        // 타입별 Excel: igType별로 다른 파일명
        // all → _complete.xlsx, posts/reels/stories/highlights → 각 타입 이름
        QString excelDir = userDir + "/excel";
        QDir().mkpath(excelDir);
        QString igExcelPath = FileHelper::typeExcelPath(excelDir, username, igType);

        // 새 데이터 준비
        QJsonArray allMediaArray = allMedia.readAll();
        auto extractIgRow = [](const QJsonObject &node) -> QStringList {
            QString caption;
            QJsonArray capEdges = node["edge_media_to_caption"].toObject()["edges"].toArray();
            if (!capEdges.isEmpty()) caption = capEdges[0].toObject()["node"].toObject()["text"].toString().left(200);
            if (caption.isEmpty()) caption = node["caption"].toObject()["text"].toString().left(200);
            QString sc = node["shortcode"].toString();
            if (sc.isEmpty()) sc = node["code"].toString();
            qint64 ts = node["taken_at_timestamp"].toVariant().toLongLong();
            if (ts == 0) ts = node["taken_at"].toVariant().toLongLong();
            return {
                sc,
                ts > 0 ? QDateTime::fromSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm:ss") : "",
                node["is_video"].toBool() || node["media_type"].toInt() == 2 ? "Video" : "Photo",
                QString::number(node["edge_liked_by"].toObject()["count"].toInt() + node["like_count"].toInt()),
                QString::number(node["edge_media_to_comment"].toObject()["count"].toInt() + node["comment_count"].toInt()),
                caption,
                sc.isEmpty() ? "" : "https://www.instagram.com/p/" + sc
            };
        };

        // 기존 파일 읽기
        QSet<QString> existingCodes;
        QList<QStringList> oldRows;
        if (QFile::exists(igExcelPath)) {
            QXlsx::Document existDoc(igExcelPath);
            int lastRow = existDoc.dimension().lastRow();
            for (int r = 2; r <= lastRow; ++r) {
                QString scode = existDoc.read(r, 1).toString().trimmed();
                if (scode.isEmpty() || scode.startsWith("─") || scode.startsWith("═")) continue;
                existingCodes.insert(scode);
                QStringList cols;
                for (int c = 1; c <= 7; ++c) cols << existDoc.read(r, c).toString();
                oldRows.append(cols);
            }
        }

        // 새 데이터 중복 제거 + 날짜 정렬
        QList<QStringList> newRows;
        for (const auto &val : allMediaArray) {
            QJsonObject node = val.toObject();
            QString sc = node["shortcode"].toString();
            if (sc.isEmpty()) sc = node["code"].toString();
            if (!existingCodes.contains(sc)) {
                newRows.append(extractIgRow(node));
            }
        }
        // 날짜 내림차순 정렬
        std::sort(newRows.begin(), newRows.end(), [](const QStringList &a, const QStringList &b) { return a[1] > b[1]; });
        std::sort(oldRows.begin(), oldRows.end(), [](const QStringList &a, const QStringList &b) { return a[1] > b[1]; });

        ExcelWriter writer;
        writer.writeHeader(hdrs, QColor("#E4405F"));
        int row = 2;
        for (const auto &r : newRows) writer.writeRow(row++, r);
        if (!newRows.isEmpty() && !oldRows.isEmpty()) {
            writer.writeRow(row++, {QString("═══ %1 新規 %2件 ═══").arg(QDateTime::currentDateTime().toString("yyyy/MM/dd HH:mm")).arg(newRows.size())});
        }
        for (const auto &r : oldRows) writer.writeRow(row++, r);
        writer.autoFitColumns(hdrs);
        writer.save(igExcelPath);
        FileHelper::setDownloadMeta(igExcelPath, "https://instagram.com");
        log(QString("Excel 저장: +%1개 (총 %2개)").arg(newRows.size()).arg(newRows.size() + oldRows.size()), "success", "instagram");
    }
}

void MiyoBackend::runYoutubeDownload(const QJsonObject &config)
{
    QString url = config["url"].toString();
    QString path = config["path"].toString();
    path.replace("~", QDir::homePath());

    QString quality = config["quality"].toString("1080p");
    QString type = config["type"].toString("video");

    // 공통 저장 정책: youtube/{video|audio|thumbnail}/ + youtube/_complete/
    QString ytBaseDir = path + "/youtube";
    QDir().mkpath(ytBaseDir);
    QString ytTypeDir = FileHelper::typeFolder(ytBaseDir, type);
    QString ytCompleteDir = FileHelper::typeFolder(ytBaseDir, "complete");
    QString ytExcelDir = ytBaseDir + "/excel";
    QDir().mkpath(ytExcelDir);

    QStringList urls;
    for (const auto &line : url.split('\n')) {
        QString trimmed = line.trimmed();
        if (trimmed.startsWith("http")) urls.append(trimmed);
    }

    if (urls.isEmpty()) {
        log("No valid URLs", "error", "youtube");
        return;
    }

    // Build yt-dlp arguments
    QString ytdlpPath = findBundledTool("yt-dlp");
    QString appDir = QCoreApplication::applicationDirPath();

    QStringList baseArgs;
    baseArgs << "--no-mtime";
    baseArgs << "--no-restrict-filenames";   // ★ 유니코드(한/일 등) 제목 그대로 보존
    // ★ ffmpeg 위치 — 번들→시스템 순으로 '실제 존재하는' 것만 지정(오디오 mp3 추출/영상 병합에 필수).
    //   이전엔 ffmpeg 없는 디렉토리를 무조건 가리켜 "ffmpeg could not be found" 로 실패했음(윈도우 오디오 오류 원인).
    for (const QString &ff : QStringList{ appDir + "/ffmpeg.exe", appDir + "/ffmpeg",
                                          appDir + "/tools/ffmpeg.exe",
                                          "C:/ffmpeg/bin/ffmpeg.exe" }) {
        if (QFile::exists(ff)) { baseArgs << "--ffmpeg-location" << ff; break; }
    }
    // (위 후보가 없으면 --ffmpeg-location 생략 → PATH 에서 탐색)
    // Rate limit 방지: 영상 간 딜레이
    baseArgs << "--sleep-interval" << "3" << "--max-sleep-interval" << "8";
    baseArgs << "--sleep-requests" << "1";

    if (type == "audio") {
        baseArgs << "-x" << "--audio-format" << "mp3";
    } else if (type == "thumbnail") {
        baseArgs << "--write-thumbnail" << "--skip-download";
    } else {
        QMap<QString, QString> qMap = {
            {"4K","bestvideo[height<=2160]+bestaudio/best"},
            {"1440p","bestvideo[height<=1440]+bestaudio/best"},
            {"1080p","bestvideo[height<=1080]+bestaudio/best"},
            {"720p","bestvideo[height<=720]+bestaudio/best"},
            {"480p","bestvideo[height<=480]+bestaudio/best"},
            {"360p","bestvideo[height<=360]+bestaudio/best"},
        };
        baseArgs << "-f" << qMap.value(quality, "bestvideo+bestaudio/best");
        baseArgs << "--merge-output-format" << "mp4";
    }

    if (config["subs"].toBool()) baseArgs << "--write-subs" << "--sub-langs" << "ko,en,ja";
    if (config["thumb"].toBool()) baseArgs << "--write-thumbnail";
    if (config["metadata"].toBool()) baseArgs << "--embed-metadata";
    if (config["playlist"].toBool()) baseArgs << "--yes-playlist"; else baseArgs << "--no-playlist";
    if (config["sponsor"].toBool()) baseArgs << "--sponsorblock-remove" << "all";

    // 썸네일을 동영상에 임베드 + 설명 저장 + info JSON (Excel 생성용)
    baseArgs << "--embed-thumbnail";
    baseArgs << "--write-description";
    baseArgs << "--write-info-json";

    // ★ 임시 script/status 는 로컬 temp 에 (NAS 는 POSIX 실행권한 보존 안 함 → .command 실행 실패).
    //   yt-dlp output 은 ytBaseDir (NAS 가능) 로 그대로.
    QString tempDir = Common::resolveTempBase(m_config ? m_config->tempDir() : QString()) + "/abiwa_yt";
    QDir().mkpath(tempDir);

    QString statusFile = tempDir + "/miyo_yt_status.txt";
    QString stopMarker = statusFile + ".stop";
    QFile::remove(statusFile);
    QFile::remove(stopMarker);

#ifdef Q_OS_WIN
    // Windows: generate .bat script
    QString scriptPath = tempDir + "/miyo_yt_download.bat";
    QFile::remove(scriptPath);

    // Windows-escape helper (double quotes)
    auto esc = [](const QString &s) -> QString {
        return "\"" + s + "\"";
    };

    QString argsStr;
    for (const QString &a : baseArgs) argsStr += esc(a) + " ";

    QString script;
    script += "@echo off\r\nchcp 65001 >nul\r\n";
    script += "title ABIWA - YouTube\r\n";
    script += "set \"STATUS=" + QDir::toNativeSeparators(statusFile) + "\"\r\n";
    script += "set \"STOP_MARKER=" + QDir::toNativeSeparators(stopMarker) + "\"\r\n";
    script += "echo STARTED > \"%STATUS%\"\r\n";
    script += "echo =========================================\r\n";
    script += "echo   ABIWA - YouTube Download\r\n";
    script += QString("echo   총 %1개 URL\r\n").arg(urls.size());
    script += "echo =========================================\r\n";
    script += "echo.\r\nset SUCCESS=0\r\nset FAIL=0\r\n\r\n";

    for (int i = 0; i < urls.size(); i++) {
        script += "if exist \"%STOP_MARKER%\" (\r\n";
        script += "  echo 사용자에 의해 중지됨\r\n";
        script += "  echo DONE:%SUCCESS%:%FAIL% > \"%STATUS%\"\r\n";
        script += "  del /f \"%STOP_MARKER%\" 2>nul\r\n";
        script += "  pause >nul\r\n  exit /b 0\r\n)\r\n";
        script += QString("echo PROGRESS:%1:%2 > \"%STATUS%\"\r\n").arg(i + 1).arg(urls.size());
        script += QString("echo [%1/%2] %3\r\n").arg(i + 1).arg(urls.size()).arg(urls[i]);
        script += "echo -----------------------------------------\r\n";
        script += "set RETRY=0\r\n";
        script += ":RETRY_LOOP_" + QString::number(i) + "\r\n";
        script += esc(ytdlpPath) + " -o " + esc(ytTypeDir + "/%(channel,uploader)s/%(upload_date)s_%(title)s.%(ext)s") + " " + argsStr + esc(urls[i]) + "\r\n";
        script += "if %errorlevel%==0 (\r\n  set /a SUCCESS+=1\r\n  echo >> 완료\r\n) else (\r\n";
        script += "  set /a RETRY+=1\r\n";
        script += "  if %RETRY% LEQ 3 (\r\n";
        script += "    set /a WAIT_SEC=60*%RETRY%\r\n";
        script += "    echo >> Rate limit / 실패 — %WAIT_SEC%초 대기 후 재시도 ^(%RETRY%/3^)\r\n";
        script += "    timeout /t %WAIT_SEC% /nobreak >nul\r\n";
        script += "    goto RETRY_LOOP_" + QString::number(i) + "\r\n";
        script += "  ) else (\r\n    set /a FAIL+=1\r\n    echo >> 실패\r\n  )\r\n)\r\n";
        script += "echo.\r\n";
    }
    script += "echo =========================================\r\n";
    script += "echo   완료! 성공: %SUCCESS%, 실패: %FAIL%\r\n";
    script += "echo   저장 경로: " + ytBaseDir + "\r\n";
    script += "echo =========================================\r\n";
    script += "echo DONE:%SUCCESS%:%FAIL% > \"%STATUS%\"\r\n";
    script += "del /f \"%STOP_MARKER%\" 2>nul\r\n";
    script += "echo.\r\necho 터미널을 닫아도 됩니다.\r\npause >nul\r\n";
#else
    // macOS/Linux: generate .command script
    QString scriptPath = tempDir + "/miyo_yt_download.command";
    QFile::remove(scriptPath);

    auto esc = [](const QString &s) -> QString {
        QString r = s;
        r.replace("'", "'\\''");
        return "'" + r + "'";
    };

    QString argsStr;
    for (const QString &a : baseArgs) argsStr += esc(a) + " ";

    QString script;
    script += "#!/bin/bash\n";
    script += "export PATH=" + esc(appDir) + ":/opt/homebrew/bin:/usr/local/bin:\"$PATH\"\n";
    script += "STATUS=" + esc(statusFile) + "\n";
    script += "STOP_MARKER=" + esc(stopMarker) + "\n";
    script += "echo 'STARTED' > \"$STATUS\"\n\n";
    script += "clear\n";
    script += "echo '========================================='\n";
    script += "echo '  ABIWA - YouTube ダウンロード'\n";
    script += QString("echo '  총 %1개 URL'\n").arg(urls.size());
    script += "echo '========================================='\n";
    script += "echo ''\n\n";
    script += "SUCCESS=0\nFAIL=0\n\n";

    for (int i = 0; i < urls.size(); i++) {
        script += "if [ -f \"$STOP_MARKER\" ]; then\n";
        script += "  echo ''\n  echo '사용자에 의해 중지됨'\n";
        script += "  echo \"DONE:$SUCCESS:$FAIL\" > \"$STATUS\"\n";
        script += "  rm -f \"$STOP_MARKER\"\n";
        script += "  echo ''\n  echo '터미널을 닫아도 됩니다.'\n  read -n 1\n  exit 0\nfi\n\n";

        script += QString("echo 'PROGRESS:%1:%2' > \"$STATUS\"\n").arg(i + 1).arg(urls.size());
        script += QString("echo '[%1/%2] %3'\n").arg(i + 1).arg(urls.size()).arg(urls[i]);
        script += "echo '-----------------------------------------'\n";

        // Rate limit 자동 재시도 (최대 3회, 60초 대기)
        script += "RETRY=0\n";
        script += "MAX_RETRY=3\n";
        script += "while [ $RETRY -le $MAX_RETRY ]; do\n";
        script += "  " + esc(ytdlpPath) + " -o " + esc(ytTypeDir + "/%(channel,uploader)s/%(upload_date)s_%(title)s.%(ext)s") + " " + argsStr + esc(urls[i]) + " &\n";
        script += "  YT_PID=$!\n";
        script += "  while kill -0 $YT_PID 2>/dev/null; do\n";
        script += "    if [ -f \"$STOP_MARKER\" ]; then\n";
        script += "      kill $YT_PID 2>/dev/null\n";
        script += "      pkill -P $YT_PID 2>/dev/null\n";
        script += "      wait $YT_PID 2>/dev/null\n";
        script += "      echo ''\n      echo '사용자에 의해 중지됨'\n";
        script += "      echo \"DONE:$SUCCESS:$FAIL\" > \"$STATUS\"\n";
        script += "      rm -f \"$STOP_MARKER\"\n";
        script += "      echo ''\n      echo '터미널을 닫아도 됩니다.'\n      read -n 1\n      exit 0\n";
        script += "    fi\n";
        script += "    sleep 1\n";
        script += "  done\n";
        script += "  wait $YT_PID\n";
        script += "  YT_EXIT=$?\n";
        script += "  if [ $YT_EXIT -eq 0 ]; then\n";
        script += "    break\n";
        script += "  fi\n";
        script += "  RETRY=$((RETRY+1))\n";
        script += "  if [ $RETRY -le $MAX_RETRY ]; then\n";
        script += "    WAIT_SEC=$((60 * RETRY))\n";
        script += "    echo ''\n";
        script += "    echo \">> Rate limit / 실패 — ${WAIT_SEC}초 대기 후 재시도 ($RETRY/$MAX_RETRY)\"\n";
        script += "    for i in $(seq $WAIT_SEC -1 1); do\n";
        script += "      printf \"\\r  대기 중... %ds \" $i\n";
        script += "      sleep 1\n";
        script += "      if [ -f \"$STOP_MARKER\" ]; then break 2; fi\n";
        script += "    done\n";
        script += "    echo ''\n";
        script += "  fi\n";
        script += "done\n";

        script += "if [ $YT_EXIT -eq 0 ]; then\n";
        script += "  SUCCESS=$((SUCCESS+1))\n";
        script += "  echo ''\n  echo '>> 완료'\n";
        script += "else\n";
        script += "  FAIL=$((FAIL+1))\n";
        script += "  echo ''\n  echo '>> 실패'\n";
        script += "fi\necho ''\n\n";
    }

    script += "echo '========================================='\n";
    script += "echo \"  완료! 성공: $SUCCESS, 실패: $FAIL\"\n";
    script += "echo '  저장 경로: " + ytBaseDir + "'\n";
    script += "echo '========================================='\n";
    script += "echo \"DONE:$SUCCESS:$FAIL\" > \"$STATUS\"\n";
    script += "rm -f \"$STOP_MARKER\"\n";
    script += "echo ''\necho '터미널을 닫아도 됩니다.'\nread -n 1\n";
#endif

    // Write script
    QFile scriptFile(scriptPath);
    if (!scriptFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        log("스크립트 생성 실패", "error", "youtube");
        return;
    }
    scriptFile.write(script.toUtf8());
    scriptFile.close();
#ifndef Q_OS_WIN
    scriptFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                              QFileDevice::ReadGroup | QFileDevice::ExeGroup);
    // ★ Qt setPermissions 가 NAS 등에서 실패할 수 있음 → chmod 직접 + quarantine 제거
    QProcess::execute("/bin/chmod", {"+x", scriptPath});
    QProcess::execute("/usr/bin/xattr", {"-d", "com.apple.quarantine", scriptPath});
#endif

    // Launch terminal with script
    log(QString("터미널에서 %1개 URL 다운로드 시작...").arg(urls.size()), "info", "youtube");
#ifdef Q_OS_WIN
    QProcess::startDetached("cmd.exe", {"/c", "start", "ABIWA-YouTube", QDir::toNativeSeparators(scriptPath)});
#else
    QProcess::startDetached("/usr/bin/open", {scriptPath});
#endif

    // Monitor progress from status file
    while (m_isRunning.value("youtube", false)) {
        QThread::sleep(1);

        QFile sf(statusFile);
        if (!sf.open(QIODevice::ReadOnly)) continue;
        QString status = QString::fromUtf8(sf.readAll()).trimmed();
        sf.close();

        if (status.startsWith("DONE:")) {
            QStringList parts = status.split(":");
            int success = parts.value(1).toInt();
            int fail = parts.value(2).toInt();
            runJs("setYoutubeProgress(100)");
            log(QString("Complete! Success: %1, Failed: %2").arg(success).arg(fail), "success", "youtube");
            log("Path: " + path, "info", "youtube");
            updateStats(success, fail, "Done", "youtube");

            // ── Post-processing: _complete 미러 + Excel 생성 ──
            QDirIterator it(ytTypeDir, {"*.info.json"}, QDir::Files, QDirIterator::Subdirectories);
            QJsonArray rows;
            while (it.hasNext()) {
                QString infoPath = it.next();
                QFile f(infoPath);
                if (!f.open(QIODevice::ReadOnly)) continue;
                QJsonObject info = QJsonDocument::fromJson(f.readAll()).object();
                f.close();

                QString uploadDate = info["upload_date"].toString(); // YYYYMMDD
                QDateTime dt;
                if (uploadDate.size() == 8) {
                    dt = QDateTime::fromString(uploadDate, "yyyyMMdd");
                    dt.setTimeZone(QTimeZone::utc());
                }

                // 미디어 파일명 찾기 (info.json → .mp4/.mp3/.jpg 등)
                QString base = infoPath;
                base.chop(QString(".info.json").size());
                QStringList candidates = {
                    base + ".mp4", base + ".mkv", base + ".webm", base + ".mov",
                    base + ".mp3", base + ".m4a", base + ".opus",
                    base + ".jpg", base + ".png", base + ".webp"
                };
                QString mediaPath;
                for (const QString &c : candidates) {
                    if (QFileInfo::exists(c)) { mediaPath = c; break; }
                }

                // 메타데이터 적용 + _complete 미러
                if (!mediaPath.isEmpty()) {
                    QString ytUrl = info["webpage_url"].toString();
                    QString uploader = info["uploader"].toString();
                    QString title = info["title"].toString().left(200);
                    // EXIF → Finder comment → xattr → mtime 순서
                    Common::addExifMetadata(mediaPath,
                        uploader.isEmpty() ? "" : "@" + uploader,
                        title, "YouTube @" + uploader, ytUrl,
                        dt.isValid() ? dt.toString("yyyy:MM:dd HH:mm:ss") : "");
                    FileHelper::setFinderComment(mediaPath, ytUrl);
                    FileHelper::applyPostMetadata(mediaPath, dt, ytUrl);
                    // _complete 미러 (채널별 서브폴더 유지)
                    QString channelName = info["channel"].toString();
                    if (channelName.isEmpty()) channelName = info["uploader"].toString();
                    if (channelName.isEmpty()) channelName = "_unknown";
                    // 파일시스템 안전 문자로 치환
                    channelName.replace(QRegularExpression("[\\\\/:*?\"<>|]"), "_");
                    QString mirrorChannelDir = ytCompleteDir + "/" + channelName;
                    QDir().mkpath(mirrorChannelDir);
                    QString mirrorPath = mirrorChannelDir + "/" + QFileInfo(mediaPath).fileName();
                    if (!QFile::exists(mirrorPath)) QFile::copy(mediaPath, mirrorPath);
                    if (QFile::exists(mirrorPath)) {
                        FileHelper::setFinderComment(mirrorPath, ytUrl);
                        FileHelper::applyPostMetadata(mirrorPath, dt, ytUrl);
                    }
                    // 経済産業省 연계: 페이지 캡처
                    QString ytCapturesDir = ytBaseDir + "/captures";
                    FileHelper::capturePageHtml(ytCapturesDir, ytUrl,
                        FileHelper::uploadOrderPrefix(dt) + info["id"].toString());
                }

                // Excel row
                QJsonObject row;
                row["id"] = info["id"].toString();
                row["title"] = info["title"].toString();
                row["uploader"] = info["uploader"].toString();
                row["uploader_id"] = info["uploader_id"].toString();
                row["upload_date"] = uploadDate;
                row["duration"] = info["duration"].toDouble();
                row["view_count"] = info["view_count"].toDouble();
                row["like_count"] = info["like_count"].toDouble();
                row["channel"] = info["channel"].toString();
                row["channel_url"] = info["channel_url"].toString();
                row["tags"] = QJsonArray::fromStringList(QStringList());
                QStringList tagList;
                for (const auto &t : info["tags"].toArray()) tagList << t.toString();
                row["tag_str"] = tagList.join(", ");
                row["webpage_url"] = info["webpage_url"].toString();
                row["description"] = info["description"].toString().left(500);
                rows.append(row);
            }

            // Excel 쓰기: type별 + complete
            QStringList hdrs = {"ID", "Title", "Uploader", "Upload Date", "Duration",
                                 "Views", "Likes", "Channel", "Tags", "URL", "Description"};
            auto writeExcel = [&](const QString &excelPath) {
                ExcelWriter writer;
                writer.writeHeader(hdrs, QColor("#FF0000"));
                int r = 2;
                for (const auto &v : rows) {
                    QJsonObject o = v.toObject();
                    writer.writeRow(r++, {
                        o["id"].toString(), o["title"].toString(), o["uploader"].toString(),
                        o["upload_date"].toString(),
                        QString::number(o["duration"].toDouble(), 'f', 0),
                        QString::number(o["view_count"].toDouble(), 'f', 0),
                        QString::number(o["like_count"].toDouble(), 'f', 0),
                        o["channel"].toString(),
                        o["tag_str"].toString(),
                        o["webpage_url"].toString(),
                        o["description"].toString()
                    });
                }
                writer.autoFitColumns(hdrs);
                writer.save(excelPath);
            };
            if (!rows.isEmpty()) {
                writeExcel(FileHelper::typeExcelPath(ytExcelDir, "youtube", type));
                writeExcel(FileHelper::typeExcelPath(ytExcelDir, "youtube", "complete"));
                log(QString("📊 Excel 저장: %1개 항목").arg(rows.size()), "success", "youtube");
            }
            break;
        } else if (status.startsWith("PROGRESS:")) {
            QStringList parts = status.split(":");
            int current = parts.value(1).toInt();
            int total = parts.value(2).toInt();
            int pct = (current * 100) / qMax(total, 1);
            runJs(QString("setYoutubeProgress(%1)").arg(pct));
            updateStats(current, 0, "Downloading", "youtube");
        }
    }

    // Cleanup temp files
    QFile::remove(statusFile);
    QFile::remove(stopMarker);
    // Don't delete scriptPath immediately - Terminal may still be reading it
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ── Pixiv ──
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void MiyoBackend::runPixivCollection(const QJsonObject &config)
{
    CollectionGuard _cg(platformSem("pixiv"), this, "pixiv");
    setIntegrityActiveForPlatform("pixiv", config["integrityCheck"].toBool(false));
    if (config["method"].toString() == "chrome") {
        runRealChromeCollection(config);
        return;
    }
    if (config["method"].toString() == "web") {
        // ★ 내부 QWebEngine 대신 실제 Chrome (CDP)로 라우팅 — PHPSESSID 자동 주입 + 봇 탐지 우회
        runRealChromeCollection(config);
        return;
    }
    QString sessionId = config["sessionId"].toString();
    QString target = config["target"].toString().trimmed();
    QString savePath = config["path"].toString();
    savePath.replace("~", QDir::homePath());
    int maxCount = config["count"].toInt(0);
    double delay = config["delay"].toDouble(1.5);
    bool ugoiraGif = config["ugoiraGif"].toBool(true);
    bool saveExcel = config["excel"].toBool(true);
    QString type = config["type"].toString("user"); // user, bookmarks, illust, all, novels, novels_bookmarks

    // ★ realCapture 시 사용할 Pixiv 쿠키 (PHPSESSID) — R-18 / fanbox-only 컨텐츠 표시용
    QList<QNetworkCookie> pxCookies;
    if (!sessionId.isEmpty()) {
        QNetworkCookie c("PHPSESSID", sessionId.toUtf8());
        c.setDomain(".pixiv.net"); c.setPath("/"); c.setSecure(true);
        pxCookies << c;
    }

    // ── 소설 전용 모드 ──
    bool novelsOnly = false;
    if (type == "novels" || type == "novels_bookmarks") {
        novelsOnly = true;
        type = (type == "novels_bookmarks") ? "bookmarks" : "user";
    }

    // ── ALL: 전체 수집 (유저 작품 + 북마크) ──
    if (type == "all") {
        log("═══ 전체 수집 모드 ═══", "success", "pixiv");
        QStringList subTypes = {"user", "bookmarks"};
        for (int i = 0; i < subTypes.size(); ++i) {
            if (!m_isRunning.value("pixiv", false)) break;
            log(QString("▶ [%1/%2] %3 수집...").arg(i+1).arg(subTypes.size()).arg(subTypes[i]), "info", "pixiv");
            QJsonObject subConfig = config;
            subConfig["type"] = subTypes[i];
            runPixivCollection(subConfig);
        }
        log("═══ 전체 수집 완료! ═══", "success", "pixiv");
        return;
    }

    log("Pixiv 연결 중...", "info", "pixiv");

    HttpClient http;
    http.setTimeout(30000);
    http.setDownloadTimeout(120000);
    http.setRunFlag(&m_isRunning["pixiv"]);  // 중지 시 즉시 abort

    QMap<QString, QString> apiHeaders;
    apiHeaders["User-Agent"] = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0.0.0 Safari/537.36";
    // ★ NSFW (R-18) 보강:
    //   1) PHPSESSID 외에 사용자가 직접 추가 raw cookie 줄 수 있음 (config["pixivExtraCookie"])
    //      예: "device_token=xxx; user_token=yyy" — Pixiv가 일부 R-18에 추가 쿠키 검증
    //   2) Accept-Language 일본어 우선 (Pixiv가 지역별 컨텐츠 제한 다름 — JP 가장 관대)
    QString cookieStr = "PHPSESSID=" + sessionId;
    QString extraCk = config["pixivExtraCookie"].toString().trimmed();
    if (!extraCk.isEmpty()) {
        if (!cookieStr.endsWith(';')) cookieStr += "; ";
        cookieStr += extraCk;
    }
    apiHeaders["Cookie"] = cookieStr;
    apiHeaders["Referer"] = "https://www.pixiv.net/";
    apiHeaders["Accept"] = "application/json";
    apiHeaders["Accept-Language"] = "ja,en-US;q=0.9,en;q=0.8";  // R-18 가시성 ↑

    QMap<QString, QString> dlHeaders;
    dlHeaders["User-Agent"] = apiHeaders["User-Agent"];
    dlHeaders["Referer"] = "https://www.pixiv.net/";
    dlHeaders["Accept"] = "image/avif,image/webp,image/apng,image/svg+xml,image/*,*/*;q=0.8";
    dlHeaders["Accept-Language"] = "ja,en-US;q=0.9,en;q=0.8";

    // Parse target: could be user ID, user URL, illust URL, or NOVEL URL
    QString userId;
    QString illustId;
    QString singleNovelId;
    QRegularExpression illustUrlRe(R"(artworks/(\d+))");
    QRegularExpression userUrlRe(R"(users/(\d+))");
    QRegularExpression novelUrlRe(R"(novel/show\.php.*?[?&]id=(\d+))");
    QRegularExpression novelShortRe(R"(/novel/(\d+))");
    QRegularExpression numRe(R"(^\d+$)");

    auto illustMatch = illustUrlRe.match(target);
    auto userMatch = userUrlRe.match(target);
    auto novelMatch = novelUrlRe.match(target);
    auto novelShortMatch = novelShortRe.match(target);

    if (novelMatch.hasMatch() || novelShortMatch.hasMatch()) {
        // 단일 소설 URL 직접 감지 → novel 모드
        singleNovelId = novelMatch.hasMatch() ? novelMatch.captured(1) : novelShortMatch.captured(1);
        type = "novel";
    } else if (type == "illust" || illustMatch.hasMatch()) {
        // Single illustration mode
        illustId = illustMatch.hasMatch() ? illustMatch.captured(1) : target;
        type = "illust";
    } else if (userMatch.hasMatch()) {
        userId = userMatch.captured(1);
    } else if (numRe.match(target).hasMatch()) {
        userId = target;
    } else {
        log("올바른 유저 ID, 일러스트/소설 URL, 또는 유저 URL을 입력하세요.", "error", "pixiv");
        return;
    }

    QDir().mkpath(savePath);

    // ── 유저 프로필 다운로드 (아바타, 배경, 설명) ──
    auto downloadPixivProfile = [&](const QString &uid, const QString &userSaveDir) {
        QString profileUrl = QString("https://www.pixiv.net/ajax/user/%1").arg(uid);
        HttpResponse profResp = http.get(profileUrl, apiHeaders);
        if (!profResp.isOk()) {
            log(QString("유저 프로필 API 실패 (HTTP %1)").arg(profResp.statusCode), "warning", "pixiv");
            return;
        }
        QJsonObject profBody = profResp.json()["body"].toObject();
        QString userName = profBody["name"].toString();
        QString comment = profBody["comment"].toString();       // 자기소개
        QString commentHtml = profBody["commentHtml"].toString();
        QString imageBig = profBody["imageBig"].toString();     // 아바타 (큰 사이즈)
        if (imageBig.isEmpty()) imageBig = profBody["image"].toString();
        QJsonObject bg = profBody["background"].toObject();
        QString bgUrl = bg["url"].toString();                   // 배경 이미지
        bool isOfficialAccount = profBody["isOfficialAccount"].toBool();
        bool isPremium = profBody["premium"].toBool();

        log(QString("👤 프로필: %1 (%2)%3%4")
            .arg(userName, uid)
            .arg(isPremium ? " [Premium]" : "")
            .arg(isOfficialAccount ? " [公式]" : ""), "info", "pixiv");
        if (!comment.isEmpty()) {
            log(QString("  📝 %1").arg(comment.left(100)), "info", "pixiv");
        }

        // 프로필 폴더
        QDir().mkpath(userSaveDir);
        QString dateTag = QDateTime::currentDateTime().toString("yyyyMMdd");

        // 아바타 다운로드
        if (!imageBig.isEmpty()) {
            QString ext = QFileInfo(QUrl(imageBig).path()).suffix();
            if (ext.isEmpty()) ext = "jpg";
            QString avatarPath = userSaveDir + "/avatar_" + dateTag + "." + ext;
            if (!QFile::exists(avatarPath)) {
                if (http.downloadFile(imageBig, avatarPath, dlHeaders)) {
                    log(QString("📸 아바타 저장: avatar_%1.%2").arg(dateTag, ext), "success", "pixiv");
                }
            }
        }

        // 배경 이미지 다운로드
        if (!bgUrl.isEmpty()) {
            QString ext = QFileInfo(QUrl(bgUrl).path()).suffix();
            if (ext.isEmpty()) ext = "jpg";
            QString bgPath = userSaveDir + "/background_" + dateTag + "." + ext;
            if (!QFile::exists(bgPath)) {
                if (http.downloadFile(bgUrl, bgPath, dlHeaders)) {
                    log(QString("🖼️ 배경 저장: background_%1.%2").arg(dateTag, ext), "success", "pixiv");
                }
            }
        }

        // 프로필 정보 JSON 저장
        QString profileJsonPath = userSaveDir + "/profile.json";
        QJsonObject profileData;
        profileData["user_id"] = uid;
        profileData["user_name"] = userName;
        profileData["comment"] = comment;
        profileData["comment_html"] = commentHtml;
        profileData["avatar_url"] = imageBig;
        profileData["background_url"] = bgUrl;
        profileData["is_premium"] = isPremium;
        profileData["is_official"] = isOfficialAccount;
        profileData["webpage"] = profBody["webpage"].toString();
        profileData["region"] = profBody["region"].toObject()["name"].toString();
        profileData["gender"] = profBody["gender"].toString();
        profileData["birth_day"] = profBody["birthDay"].toString();
        // social 링크
        QJsonObject social = profBody["social"].toObject();
        if (!social.isEmpty()) profileData["social"] = social;
        profileData["fetched_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);

        QFile pf(profileJsonPath);
        if (pf.open(QIODevice::WriteOnly)) {
            pf.write(QJsonDocument(profileData).toJson(QJsonDocument::Indented));
            pf.close();
            log("📄 profile.json 저장", "success", "pixiv");
        }
    };

    // filename helper: (유저네임 / 픽시브사용자명)제목 (일러스트ID_날짜_인덱스).확장자
    auto pxFilename = [](const QString &userName, const QString &title, const QString &illustIdStr, const QString &dateStr, int idx, const QString &ext) -> QString {
        QString t = title.left(40).trimmed();
        t.replace('\n', ' ');
        t = sanitizeFilename(t, 40);
        if (t.isEmpty()) t = "untitled";
        QString u = sanitizeFilename(userName.left(30).trimmed(), 30);
        QString uprefix;
        if (!u.isEmpty()) uprefix = "(" + u + ")";
        // 업로드 시각 prefix(yyyyMMdd_HHmm_) → OS 기본 정렬로 업로드 순 배치
        QString orderPrefix;
        if (!dateStr.isEmpty()) orderPrefix = dateStr + "_";
        QString name = orderPrefix + uprefix + t + " (" + illustIdStr;
        if (idx >= 0) name += "_" + QString::number(idx);
        name += ")." + ext;
        return sanitizeFilename(name, 200);
    };

    // Excel complete data (downloadIllust에서 추가)
    QJsonArray pxExcelData;

    // Download a single illustration (single, manga, ugoira)
    auto downloadIllust = [&](const QString &iid) -> bool {
        if (!m_isRunning.value("pixiv", false)) return false;

        // Get illust metadata
        QString metaUrl = QString("https://www.pixiv.net/ajax/illust/%1").arg(iid);
        HttpResponse metaResp = http.get(metaUrl, apiHeaders);
        if (!metaResp.isOk()) {
            log(QString("일러스트 %1 메타데이터 실패 (HTTP %2)").arg(iid).arg(metaResp.statusCode), "error", "pixiv");
            if (metaResp.statusCode == 403 || metaResp.statusCode == 401) {
                log("PHPSESSID가 만료되었을 수 있습니다. 다시 로그인 후 쿠키를 갱신하세요.", "error", "pixiv");
            }
            return false;
        }

        QJsonObject body = metaResp.json()["body"].toObject();
        QString title = body["title"].toString();
        QString pixivUserName = body["userName"].toString();
        int illustType = body["illustType"].toInt(); // 0=illust, 1=manga, 2=ugoira
        QString createDate = body["createDate"].toString();
        int pageCount = body["pageCount"].toInt(1);

        // ★ NSFW (R-18) 감지 + 로그 — 받으려면 Pixiv 계정 설정에서 R-18/R-18G 보기 켜야
        //   xRestrict: 0 = SFW, 1 = R-18, 2 = R-18G
        int xRestrict = body["xRestrict"].toInt(0);
        bool isAiGenerated = body["aiType"].toInt(0) == 2;
        if (xRestrict > 0) {
            QString lvl = (xRestrict == 1) ? "R-18" : "R-18G";
            log(QString("[%1] %2 NSFW — Pixiv 계정 설정 'R-18 작품 표시' 활성 필요").arg(iid, lvl),
                "info", "pixiv");
        }
        // body가 비어있거나 urls 누락이면 NSFW 차단 가능성
        if (body.isEmpty() || (xRestrict > 0 && !body.contains("urls") && !body.contains("body"))) {
            log(QString("[%1] NSFW 메타데이터 막힘 — pixivExtraCookie에 device_token 추가하거나 R-18 설정 켜세요")
                    .arg(iid), "warning", "pixiv");
        }
        int likeCount = body["likeCount"].toInt();
        int bookmarkCount = body["bookmarkCount"].toInt();
        int viewCount = body["viewCount"].toInt();
        int commentCount = body["commentCount"].toInt();
        QString description = body["description"].toString().left(200);
        description.remove(QRegularExpression("<[^>]*>"));  // HTML 태그 제거

        // 태그 추출
        QJsonArray tagArr = body["tags"].toObject()["tags"].toArray();
        QStringList pxTags;
        for (const auto &t : tagArr) pxTags << t.toObject()["tag"].toString();

        // Parse date
        QString dateStr;
        QDateTime dt = QDateTime::fromString(createDate, Qt::ISODate);
        if (dt.isValid()) {
            dateStr = dt.toUTC().addSecs(9 * 3600).toString("yyyyMMdd_HHmm");
        }

        // Excel data 추가
        if (saveExcel) {
            QJsonObject exRow;
            exRow["id"] = iid;
            exRow["title"] = title;
            exRow["author"] = pixivUserName;
            exRow["author_id"] = body["userId"].toString();
            exRow["type"] = illustType == 0 ? "illust" : (illustType == 1 ? "manga" : "ugoira");
            exRow["pages"] = pageCount;
            exRow["created_at"] = dt.isValid() ? dt.toUTC().addSecs(9*3600).toString("yyyy/MM/dd HH:mm") : createDate;
            exRow["likes"] = likeCount;
            exRow["bookmarks"] = bookmarkCount;
            exRow["views"] = viewCount;
            exRow["comments"] = commentCount;
            exRow["tags"] = pxTags.join(", ");
            exRow["description"] = description;
            exRow["url"] = QString("https://www.pixiv.net/artworks/%1").arg(iid);
            pxExcelData.append(exRow);
        }

        // Save to pixiv/{userId}_{userName}/{subdir}/ subfolder
        QString pixivUserId = body["userId"].toString();
        QString pixivDir = savePath + "/pixiv";
        QString userBaseDir = pixivDir + "/" + pixivUserId + "_" + sanitizeFilename(pixivUserName.left(30).trimmed(), 30);
        // 타입별 서브폴더: bookmarks → bookmarks/illusts/, user → illusts/, illust → illusts/
        QString userSaveDir;
        if (type == "bookmarks") {
            userSaveDir = userBaseDir + "/bookmarks/illusts";
        } else {
            userSaveDir = userBaseDir + "/illusts";
        }
        QDir().mkpath(userSaveDir);

        log(QString("[%1] %2 (type:%3, pages:%4)").arg(iid, title).arg(illustType).arg(pageCount), "info", "pixiv");

        if (illustType == 2) {
            // ── Ugoira (animated) ──
            QString ugoiraUrl = QString("https://www.pixiv.net/ajax/illust/%1/ugoira_meta").arg(iid);
            HttpResponse uResp = http.get(ugoiraUrl, apiHeaders);
            if (!uResp.isOk()) {
                log(QString("우고이라 메타데이터 실패: %1").arg(iid), "error", "pixiv");
                return false;
            }
            QJsonObject uBody = uResp.json()["body"].toObject();
            QString zipUrl = uBody["originalSrc"].toString();
            if (zipUrl.isEmpty()) zipUrl = uBody["src"].toString();
            QJsonArray frames = uBody["frames"].toArray();

            if (zipUrl.isEmpty()) {
                log("우고이라 ZIP URL 없음", "error", "pixiv");
                return false;
            }

            // Download ZIP
            QString zipPath = userSaveDir + "/.tmp_ugoira_" + iid + ".zip";
            if (!http.downloadFile(zipUrl, zipPath, dlHeaders)) {
                log("우고이라 ZIP 다운로드 실패", "error", "pixiv");
                return false;
            }

            if (ugoiraGif) {
                // Convert to GIF using ffmpeg
                QString tmpDir = userSaveDir + "/.tmp_ugoira_" + iid;
                QDir().mkpath(tmpDir);

                // Extract ZIP
                QProcess unzip;
                unzip.start("unzip", {"-o", "-q", zipPath, "-d", tmpDir});
                unzip.waitForFinished(30000);

                // Build ffmpeg concat file with frame delays
                QString concatPath = tmpDir + "/concat.txt";
                QFile concatFile(concatPath);
                if (concatFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                    QTextStream ts(&concatFile);
                    for (const auto &f : frames) {
                        QJsonObject frame = f.toObject();
                        QString file = frame["file"].toString();
                        double dur = frame["delay"].toDouble(100) / 1000.0;
                        ts << "file '" << tmpDir << "/" << file << "'\n";
                        ts << "duration " << QString::number(dur, 'f', 4) << "\n";
                    }
                    // ffmpeg needs last frame repeated
                    if (!frames.isEmpty()) {
                        ts << "file '" << tmpDir << "/" << frames.last().toObject()["file"].toString() << "'\n";
                    }
                    concatFile.close();
                }

                QString gifName = pxFilename(pixivUserName, title, iid, dateStr, -1, "gif");
                QString gifPath = userSaveDir + "/" + gifName;

                QString appDir = QCoreApplication::applicationDirPath();
                QProcess ffmpeg;
                QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
                env.insert("PATH", appDir + ":" + env.value("PATH"));
                ffmpeg.setProcessEnvironment(env);
                ffmpeg.start(appDir + "/ffmpeg", {
                    "-y", "-f", "concat", "-safe", "0", "-i", concatPath,
                    "-vf", "split[s0][s1];[s0]palettegen=max_colors=256:stats_mode=diff[p];[s1][p]paletteuse=dither=bayer:bayer_scale=5",
                    "-loop", "0",
                    gifPath
                });
                ffmpeg.waitForFinished(120000);

                if (QFileInfo::exists(gifPath) && QFileInfo(gifPath).size() > 0) {
                    QString pxUrl = QString("https://www.pixiv.net/artworks/%1").arg(iid);
                    FileHelper::setFinderComment(gifPath, pxUrl);
                    FileHelper::applyPostMetadata(gifPath, dt, pxUrl);
                    // _complete 미러
                    QString pxCompleteDir = userBaseDir + "/_complete";
                    QDir().mkpath(pxCompleteDir);
                    QString cpPath = pxCompleteDir + "/" + gifName;
                    if (!QFile::exists(cpPath)) { QFile::copy(gifPath, cpPath); FileHelper::applyPostMetadata(cpPath, dt, pxUrl); }
                    log(QString("GIF 변환 완료: %1").arg(gifName), "success", "pixiv");
                    // 経済産業省 연계: SingleFile CDP 캡쳐 (realCapture=true일 때만)
                    if (config["realCapture"].toBool(true)) {
                        QString capturesDir = userBaseDir + "/captures";
                        // ★ Pixiv 로그인 체크 — 호스트가 accounts.pixiv.net이거나 로그인 폼
                        static const QString pxLoginCheck = R"JS(
                            (function(){
                                if (location.hostname.indexOf('accounts.pixiv.net') === 0) return true;
                                // artwork 메인 이미지나 figure가 보이면 정상 로드된 상태
                                if (document.querySelector('main figure img, [role="presentation"] img[src*="i.pximg.net"]')) return false;
                                return !!document.querySelector('input[name="pixiv_id"], input[autocomplete="username"]');
                            })()
                        )JS";
                        captureRealPageCDPLoginAware(pxUrl, capturesDir,
                            FileHelper::uploadOrderPrefix(dt) + iid,
                            pxLoginCheck, "pixiv", 8000, pxCookies, config);
                    }
                } else {
                    // Fallback: keep ZIP
                    QString zipName = pxFilename(pixivUserName, title, iid, dateStr, -1, "zip");
                    QFile::rename(zipPath, userSaveDir + "/" + zipName);
                    log(QString("GIF 변환 실패, ZIP 저장: %1").arg(zipName), "warning", "pixiv");
                }

                // Cleanup
                QDir(tmpDir).removeRecursively();
                QFile::remove(zipPath);
            } else {
                // Save as ZIP directly
                QString zipName = pxFilename(pixivUserName, title, iid, dateStr, -1, "zip");
                QString finalZipPath = userSaveDir + "/" + zipName;
                QFile::rename(zipPath, finalZipPath);
                QString pxUrl = QString("https://www.pixiv.net/artworks/%1").arg(iid);
                FileHelper::setFinderComment(finalZipPath, pxUrl);
                FileHelper::applyPostMetadata(finalZipPath, dt, pxUrl);
                // _complete 미러
                QString pxCompleteDir = userBaseDir + "/_complete";
                QDir().mkpath(pxCompleteDir);
                QString cpPath = pxCompleteDir + "/" + zipName;
                if (!QFile::exists(cpPath)) { QFile::copy(finalZipPath, cpPath); FileHelper::applyPostMetadata(cpPath, dt, pxUrl); }
                log(QString("저장: %1").arg(zipName), "success", "pixiv");
                // 経済産業省 연계: SingleFile CDP 캡쳐 (realCapture=true일 때만)
                if (config["realCapture"].toBool(true)) {
                    QString capturesDir = userBaseDir + "/captures";
                    static const QString pxLoginCheck2 = R"JS(
                        (location.hostname.indexOf('accounts.pixiv.net') === 0
                         || !!document.querySelector('input[name="pixiv_id"], input[autocomplete="username"]'))
                    )JS";
                    captureRealPageCDPLoginAware(pxUrl, capturesDir,
                        FileHelper::uploadOrderPrefix(dt) + iid,
                        pxLoginCheck2, "pixiv", 8000, pxCookies, config);
                }
            }
            return true;

        } else {
            // ── Single image or Manga (multi-page) ──
            QString pagesUrl = QString("https://www.pixiv.net/ajax/illust/%1/pages").arg(iid);
            HttpResponse pResp = http.get(pagesUrl, apiHeaders);
            if (!pResp.isOk()) {
                log(QString("페이지 정보 실패: %1").arg(iid), "error", "pixiv");
                return false;
            }

            QJsonArray pages = pResp.json()["body"].toArray();
            int total = pages.size();

            for (int i = 0; i < total; i++) {
                if (!m_isRunning.value("pixiv", false)) return false;

                QJsonObject page = pages[i].toObject();
                QJsonObject urls = page["urls"].toObject();
                QString imgUrl = urls["original"].toString();
                if (imgUrl.isEmpty()) imgUrl = urls["regular"].toString();
                if (imgUrl.isEmpty()) imgUrl = urls["small"].toString();
                if (imgUrl.isEmpty()) imgUrl = urls["thumb_mini"].toString();

                if (imgUrl.isEmpty()) {
                    log(QString("⚠️ 페이지 %1/%2 URL 없음 (illust %3) keys: %4")
                        .arg(i).arg(total).arg(iid)
                        .arg(QStringList(urls.keys()).join(",")), "warning", "pixiv");
                    continue;
                }

                QString ext = QFileInfo(QUrl(imgUrl).path()).suffix();
                if (ext.isEmpty()) ext = "jpg";

                int idx = (total > 1) ? i : -1;
                QString filename = pxFilename(pixivUserName, title, iid, dateStr, idx, ext);
                QString filePath = userSaveDir + "/" + filename;

                if (QFileInfo::exists(filePath)) {
                    log(QString("스킵 (존재): %1").arg(filename), "info", "pixiv");
                    continue;
                }

                if (http.downloadFile(imgUrl, filePath, dlHeaders)) {
                    QString pxUrl = QString("https://www.pixiv.net/artworks/%1").arg(iid);
                    Common::addExifMetadata(filePath, pixivUserName, title,
                        "Pixiv @" + pixivUserName, pxUrl, createDate);
                    FileHelper::setFinderComment(filePath, pxUrl);
                    FileHelper::applyPostMetadata(filePath, dt, pxUrl);
                    // _complete 미러
                    QString pxCompleteDir = userBaseDir + "/_complete";
                    QDir().mkpath(pxCompleteDir);
                    QString cpPath = pxCompleteDir + "/" + filename;
                    if (!QFile::exists(cpPath)) {
                        QFile::copy(filePath, cpPath);
                        FileHelper::applyPostMetadata(cpPath, dt, pxUrl);
                    }
                    log(QString("저장: %1").arg(filename), "success", "pixiv");
                } else {
                    log(QString("다운로드 실패: %1").arg(imgUrl), "error", "pixiv");
                }

                if (i < total - 1) {
                    QThread::msleep(static_cast<unsigned long>(delay * 300));
                }
            }
            // 経済産業省 연계: SingleFile CDP 캡쳐 (realCapture=true일 때만, Pixiv 쿠키 포함)
            if (config["realCapture"].toBool(true)) {
                QString pxUrl = QString("https://www.pixiv.net/artworks/%1").arg(iid);
                QString capturesDir = userBaseDir + "/captures";
                static const QString pxLoginCheck3 = R"JS(
                    (function(){
                        if (location.hostname.indexOf('accounts.pixiv.net') === 0) return true;
                        if (document.querySelector('main figure img, [role="presentation"] img[src*="i.pximg.net"]')) return false;
                        return !!document.querySelector('input[name="pixiv_id"], input[autocomplete="username"]');
                    })()
                )JS";
                captureRealPageCDPLoginAware(pxUrl, capturesDir,
                    FileHelper::uploadOrderPrefix(dt) + iid,
                    pxLoginCheck3, "pixiv", 8000, pxCookies, config);
            }
            return true;
        }
    };

    // ── 소설 다운로드 람다 (표지 + 개별 폴더 + 삽입 이미지) ──
    auto downloadNovel = [&](const QString &nid) -> bool {
        if (!m_isRunning.value("pixiv", false)) return false;

        QString novelUrl = QString("https://www.pixiv.net/ajax/novel/%1").arg(nid);
        HttpResponse nResp = http.get(novelUrl, apiHeaders);
        if (!nResp.isOk()) {
            log(QString("소설 %1 메타데이터 실패 (HTTP %2)").arg(nid).arg(nResp.statusCode), "error", "pixiv");
            // 401/404 → 언리스티드(unlisted) 경로 재시도
            if (nResp.statusCode == 401 || nResp.statusCode == 404) {
                QString unlistedUrl = QString("https://www.pixiv.net/ajax/novel/unlisted/%1").arg(nid);
                nResp = http.get(unlistedUrl, apiHeaders);
                if (!nResp.isOk()) return false;
                log("  (unlisted 경로로 복구)", "info", "pixiv");
            } else {
                return false;
            }
        }
        QJsonObject nRoot = nResp.json();
        if (nRoot["error"].toBool(false)) {
            QString errMsg = nRoot["message"].toString();
            log(QString("소설 %1 오류: %2").arg(nid, errMsg), "error", "pixiv");
            return false;
        }
        QJsonObject nBody = nRoot["body"].toObject();
        QString nTitle = nBody["title"].toString();
        QString nUserName = nBody["userName"].toString();
        QString nContent = nBody["content"].toString();
        QString nCreateDate = nBody["createDate"].toString();
        QString nDescription = nBody["description"].toString();
        int nCharCount = nBody["characterCount"].toInt();
        int nWordCount = nBody["wordCount"].toInt();
        QString nSeriesTitle = nBody["seriesNavData"].toObject()["title"].toString();

        // 표지 URL
        QString coverUrl = nBody["coverUrl"].toString();
        if (coverUrl.isEmpty()) coverUrl = nBody["url"].toString();  // 대표 이미지

        // 태그 추출
        QJsonArray tagArr = nBody["tags"].toObject()["tags"].toArray();
        QStringList tags;
        for (const auto &t : tagArr) tags << t.toObject()["tag"].toString();

        QString nDateStr;
        QDateTime nDt = QDateTime::fromString(nCreateDate, Qt::ISODate);
        if (nDt.isValid()) nDateStr = nDt.toUTC().addSecs(9 * 3600).toString("yyyyMMdd_HHmm");

        // 저장 경로: pixiv/{userId}_{userName}/{novels|bookmarks/novels}/{제목}_{ID}/
        QString nPixivUserId = nBody["userId"].toString();
        QString nPixivDir = savePath + "/pixiv";
        QString nUserDir = nPixivDir + "/" + nPixivUserId + "_" + sanitizeFilename(nUserName.left(30).trimmed(), 30);
        QString novelFolderName = sanitizeFilename(nTitle.left(50).trimmed(), 50) + "_" + nid;
        QString novelsBase = (type == "bookmarks") ? "/bookmarks/novels/" : "/novels/";
        QString novelDir = nUserDir + novelsBase + novelFolderName;
        QDir().mkpath(novelDir);

        // ── 삽입 이미지 처리: [uploadedimage:XXX] / [pixivimage:XXX[-pageNum]] ──
        // 1) [uploadedimage:XXX] — novel body 내 textEmbeddedImages 객체에 URL 포함
        QString imagesDir = novelDir + "/images";
        QJsonObject embeddedImgs = nBody["textEmbeddedImages"].toObject();
        int embeddedDownloaded = 0;
        if (!embeddedImgs.isEmpty()) {
            QDir().mkpath(imagesDir);
            for (auto it = embeddedImgs.begin(); it != embeddedImgs.end(); ++it) {
                QString imgId = it.key();
                QJsonObject imgObj = it.value().toObject();
                QJsonObject imgUrls = imgObj["urls"].toObject();
                QString imgUrl = imgUrls["original"].toString();
                if (imgUrl.isEmpty()) imgUrl = imgUrls["1200x1200"].toString();
                if (imgUrl.isEmpty()) imgUrl = imgUrls["480mw"].toString();
                if (imgUrl.isEmpty()) continue;

                QString ext = QFileInfo(QUrl(imgUrl).path()).suffix();
                if (ext.isEmpty()) ext = "jpg";
                QString imgFile = QString("uploaded_%1.%2").arg(imgId, ext);
                QString imgPath = imagesDir + "/" + imgFile;
                if (!QFile::exists(imgPath)) {
                    if (http.downloadFile(imgUrl, imgPath, dlHeaders)) {
                        embeddedDownloaded++;
                        if (nDt.isValid()) Common::setFileTimes(imgPath, nDt);
                        Common::addExifMetadata(imgPath, nUserName, nTitle,
                            "Pixiv Novel @" + nUserName,
                            "https://www.pixiv.net/novel/show.php?id=" + nid, nCreateDate);
                    }
                }
                // 본문 내 placeholder → 상대 경로로 치환
                nContent.replace(QString("[uploadedimage:%1]").arg(imgId),
                                 QString("\n[삽입 이미지: images/%1]\n").arg(imgFile));
            }
        }

        // 2) [pixivimage:XXX] 또는 [pixivimage:XXX-N] — 외부 일러스트 참조
        QRegularExpression pixivImgRe(R"(\[pixivimage:(\d+)(?:-(\d+))?\])");
        QStringList pixivImgIds;
        auto matches = pixivImgRe.globalMatch(nContent);
        while (matches.hasNext()) {
            auto m = matches.next();
            QString id = m.captured(1);
            if (!pixivImgIds.contains(id)) pixivImgIds.append(id);
        }
        if (!pixivImgIds.isEmpty()) {
            QDir().mkpath(imagesDir);
            QString params;
            for (const QString &id : pixivImgIds) params += "&id%5B%5D=" + id;
            QString insertUrl = QString("https://www.pixiv.net/ajax/novel/%1/insert_illusts?%2")
                .arg(nid, params.mid(1));
            HttpResponse insertResp = http.get(insertUrl, apiHeaders);
            if (insertResp.isOk()) {
                QJsonObject insertBody = insertResp.json()["body"].toObject();
                for (auto it = insertBody.begin(); it != insertBody.end(); ++it) {
                    QString illustId = it.key();
                    QJsonObject illustInfo = it.value().toObject();
                    QJsonObject illustMeta = illustInfo["illust"].toObject();
                    QString origUrl = illustMeta["images"].toObject()["original"].toString();
                    if (origUrl.isEmpty()) {
                        origUrl = illustInfo["url"].toString();
                    }
                    if (origUrl.isEmpty()) continue;

                    QString ext = QFileInfo(QUrl(origUrl).path()).suffix();
                    if (ext.isEmpty()) ext = "jpg";
                    QString imgFile = QString("pixivimage_%1.%2").arg(illustId, ext);
                    QString imgPath = imagesDir + "/" + imgFile;
                    if (!QFile::exists(imgPath)) {
                        if (http.downloadFile(origUrl, imgPath, dlHeaders)) {
                            embeddedDownloaded++;
                            if (nDt.isValid()) Common::setFileTimes(imgPath, nDt);
                            Common::addExifMetadata(imgPath, nUserName, nTitle,
                                "Pixiv Novel @" + nUserName,
                                "https://www.pixiv.net/artworks/" + illustId, nCreateDate);
                        }
                    }
                    nContent.replace(QRegularExpression(QString(R"(\[pixivimage:%1(?:-\d+)?\])").arg(illustId)),
                                     QString("\n[외부 이미지: images/%1 (artworks/%2)]\n").arg(imgFile, illustId));
                }
            } else {
                log(QString("insert_illusts 실패 (HTTP %1)").arg(insertResp.statusCode), "warning", "pixiv");
            }
        }

        if (embeddedDownloaded > 0) {
            log(QString("📎 삽입 이미지 %1개 저장").arg(embeddedDownloaded), "success", "pixiv");
        }

        // Pixiv 소설 태그 변환 (삽입 이미지 처리 이후)
        nContent.replace("[newpage]", "\n\n--- 次のページ ---\n\n");
        nContent.replace(QRegularExpression(R"(\[chapter:(.*?)\])"), "\n\n== \\1 ==\n\n");
        nContent.replace(QRegularExpression(R"(\[jump:(\d+)\])"), "\n[ページジャンプ: \\1]\n");
        nContent.replace(QRegularExpression(R"(\[\[jumpuri:(.*?)>(.*?)\]\])"), "\\1 (\\2)");
        nContent.replace(QRegularExpression(R"(\[\[rb:(.*?)>(.*?)\]\])"), "\\1(\\2)");

        // 표지 다운로드
        if (!coverUrl.isEmpty()) {
            QString coverExt = QFileInfo(QUrl(coverUrl).path()).suffix();
            if (coverExt.isEmpty()) coverExt = "jpg";
            QString coverPath = novelDir + "/cover." + coverExt;
            if (!QFile::exists(coverPath)) {
                if (http.downloadFile(coverUrl, coverPath, dlHeaders)) {
                    if (nDt.isValid()) Common::setFileTimes(coverPath, nDt);
                    Common::addExifMetadata(coverPath, nUserName, nTitle,
                        "Pixiv Novel @" + nUserName, "https://www.pixiv.net/novel/show.php?id=" + nid, nCreateDate);
                    FileHelper::applyPostMetadata(coverPath, nDt,
                        QString("https://www.pixiv.net/novel/show.php?id=%1").arg(nid));
                    log(QString("📖 표지 저장: %1").arg(nTitle.left(30)), "success", "pixiv");
                }
            }
        }

        // 소설 본문 저장 (TXT + HTML)
        QString nFileName = pxFilename(nUserName, nTitle, nid, nDateStr, -1, "txt");
        QString nFilePath = novelDir + "/" + nFileName;

        if (QFile::exists(nFilePath)) {
            log(QString("스킵 (존재): %1").arg(nTitle.left(30)), "info", "pixiv");
            return true;
        }

        // TXT 저장
        QFile nFile(nFilePath);
        if (nFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&nFile);
            ts.setEncoding(QStringConverter::Utf8);
            ts << "Title: " << nTitle << "\n";
            ts << "Author: " << nUserName << "\n";
            ts << "ID: " << nid << "\n";
            ts << "URL: https://www.pixiv.net/novel/show.php?id=" << nid << "\n";
            ts << "Date: " << nCreateDate << "\n";
            if (!nSeriesTitle.isEmpty()) ts << "Series: " << nSeriesTitle << "\n";
            if (nCharCount > 0) ts << "Characters: " << nCharCount << "\n";
            if (!tags.isEmpty()) ts << "Tags: " << tags.join(", ") << "\n";
            ts << "\n━━━━━━━━━━━━━━━━━━━━\n\n";
            ts << nContent;
            nFile.close();
            log(QString("📝 소설 저장: %1").arg(nTitle.left(30)), "success", "pixiv");
        }

        // SingleFile 스타일 자체 완결 HTML 저장 (이미지 base64 인라인)
        QString htmlFileName = pxFilename(nUserName, nTitle, nid, nDateStr, -1, "html");
        QString htmlFilePath = novelDir + "/" + htmlFileName;
        if (!QFile::exists(htmlFilePath)) {
            // 이미지 base64 변환 헬퍼
            auto imgToBase64 = [&](const QString &localPath) -> QString {
                QFile f(localPath);
                if (!f.open(QIODevice::ReadOnly)) return QString();
                QByteArray data = f.readAll();
                f.close();
                QString ext = QFileInfo(localPath).suffix().toLower();
                QString mime = "image/jpeg";
                if (ext == "png") mime = "image/png";
                else if (ext == "gif") mime = "image/gif";
                else if (ext == "webp") mime = "image/webp";
                return QString("data:%1;base64,%2").arg(mime, QString::fromLatin1(data.toBase64()));
            };

            // HTML 본문 생성
            QString htmlContent = nContent;
            htmlContent.replace("&", "&amp;");
            htmlContent.replace("<", "&lt;");
            htmlContent.replace(">", "&gt;");
            htmlContent.replace("\n", "<br>\n");
            // 삽입 이미지를 base64 인라인으로 변환
            QRegularExpression embedRe(R"(\[외부 이미지: (images/[^\]]+) \(artworks/(\d+)\)\])");
            auto embedIt = embedRe.globalMatch(htmlContent);
            while (embedIt.hasNext()) {
                auto em = embedIt.next();
                QString relPath = em.captured(1);
                QString artId = em.captured(2);
                QString absPath = novelDir + "/" + relPath;
                QString b64 = imgToBase64(absPath);
                if (!b64.isEmpty()) {
                    htmlContent.replace(em.captured(0),
                        QString("<div style=\"text-align:center;margin:16px 0\"><img src=\"%1\" style=\"max-width:100%;border-radius:4px\" alt=\"artworks/%2\"></div>").arg(b64, artId));
                }
            }
            // 페이지/챕터 태그 변환
            htmlContent.replace("--- 次のページ ---", "<hr style=\"border:none;border-top:2px dashed #ccc;margin:32px 0\">");
            htmlContent.replace(QRegularExpression(R"(== (.*?) ==)"), "<h2 style=\"font-size:20px;margin:28px 0 12px;padding-bottom:4px;border-bottom:1px solid #eee\">\\1</h2>");

            // 표지 base64
            QString coverB64;
            QString coverLocalPath = novelDir + "/cover." + QFileInfo(QUrl(coverUrl).path()).suffix();
            if (coverLocalPath.endsWith(".")) coverLocalPath += "jpg";
            coverB64 = imgToBase64(coverLocalPath);

            QFile hFile(htmlFilePath);
            if (hFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
                QTextStream hs(&hFile);
                hs.setEncoding(QStringConverter::Utf8);
                hs << "<!DOCTYPE html>\n<html lang=\"ja\">\n<head>\n";
                hs << "<meta charset=\"UTF-8\">\n";
                hs << "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1.0\">\n";
                hs << "<title>" << nTitle.toHtmlEscaped() << " - " << nUserName.toHtmlEscaped() << "</title>\n";
                hs << "<style>\n";
                hs << "body{max-width:720px;margin:40px auto;padding:0 20px;font-family:'Hiragino Mincho ProN','Yu Mincho','Noto Serif JP',serif;line-height:1.9;color:#333;background:#fafaf8}\n";
                hs << ".meta{border-bottom:1px solid #ddd;padding-bottom:16px;margin-bottom:24px;color:#666;font-size:14px}\n";
                hs << ".meta h1{color:#222;font-size:24px;margin:0 0 8px}\n";
                hs << ".meta a{color:#0096fa;text-decoration:none}\n";
                hs << ".tags span{background:#f0f0f0;padding:2px 8px;border-radius:3px;margin-right:4px;font-size:12px}\n";
                hs << ".cover{text-align:center;margin:16px 0}.cover img{max-width:300px;border-radius:6px;box-shadow:0 2px 8px rgba(0,0,0,.15)}\n";
                hs << ".content{font-size:16px}\n";
                hs << "</style>\n</head>\n<body>\n";
                hs << "<div class=\"meta\">\n";
                hs << "<h1>" << nTitle.toHtmlEscaped() << "</h1>\n";
                hs << "<div>Author: <a href=\"https://www.pixiv.net/users/" << nPixivUserId << "\">" << nUserName.toHtmlEscaped() << "</a></div>\n";
                hs << "<div>Date: " << nCreateDate << "</div>\n";
                hs << "<div>URL: <a href=\"https://www.pixiv.net/novel/show.php?id=" << nid << "\">pixiv.net/novel/" << nid << "</a></div>\n";
                if (!nSeriesTitle.isEmpty())
                    hs << "<div>Series: " << nSeriesTitle.toHtmlEscaped() << "</div>\n";
                if (nCharCount > 0)
                    hs << "<div>" << nCharCount << " characters</div>\n";
                if (!tags.isEmpty()) {
                    hs << "<div class=\"tags\" style=\"margin-top:8px\">";
                    for (const QString &t : tags) hs << "<span>" << t.toHtmlEscaped() << "</span>";
                    hs << "</div>\n";
                }
                hs << "</div>\n";
                if (!coverB64.isEmpty()) {
                    hs << "<div class=\"cover\"><img src=\"" << coverB64 << "\" alt=\"cover\"></div>\n";
                }
                hs << "<div class=\"content\">\n" << htmlContent << "\n</div>\n";
                hs << "</body>\n</html>";
                hFile.close();
                log(QString("📄 SingleFile HTML: %1").arg(nTitle.left(30)), "success", "pixiv");
            }
        }

        return true;
    };

    int totalDownloaded = 0;
    int totalIllusts = 0;

    if (type == "illust") {
        // Single illustration
        if (downloadIllust(illustId)) {
            totalDownloaded++;
        }
        totalIllusts = 1;
    } else if (type == "novel") {
        // Single novel URL 직접 다운로드
        log(QString("📖 단일 소설 다운로드: %1").arg(singleNovelId), "info", "pixiv");
        if (downloadNovel(singleNovelId)) {
            totalDownloaded++;
        }
        totalIllusts = 1;
    } else {
        // User works or bookmarks
        log(QString("유저 %1의 %2 가져오는 중...").arg(userId, type == "bookmarks" ? "북마크" : "작품 목록"), "info", "pixiv");

        // ── 프로필 다운로드 (유저 단위 1회) ──
        {
            QString pixivDir = savePath + "/pixiv";
            // 먼저 프로필 API로 이름 확인
            QString profileApiUrl = QString("https://www.pixiv.net/ajax/user/%1").arg(userId);
            HttpResponse nameResp = http.get(profileApiUrl, apiHeaders);
            QString pixivUserName;
            if (nameResp.isOk()) {
                pixivUserName = nameResp.json()["body"].toObject()["name"].toString();
            }
            QString userDirName = userId;
            if (!pixivUserName.isEmpty()) userDirName += "_" + sanitizeFilename(pixivUserName.left(30).trimmed(), 30);
            QString profileSaveDir = pixivDir + "/" + userDirName + "/profiles";
            QDir().mkpath(profileSaveDir);
            downloadPixivProfile(userId, profileSaveDir);
        }

        QList<QString> illustIds;

        if (type == "bookmarks") {
            // Fetch bookmarks via API (소설 전용이면 건너뜀)
            if (novelsOnly) {
                log("📖 소설 전용 모드 — 일러스트 북마크 건너뜀", "info", "pixiv");
            }
            int offset = 0;
            int limit = 48;
            bool hasMore = !novelsOnly;

            while (hasMore && m_isRunning.value("pixiv", false)) {
                QString bmUrl = QString("https://www.pixiv.net/ajax/user/%1/illusts/bookmarks?tag=&offset=%2&limit=%3&rest=show")
                    .arg(userId).arg(offset).arg(limit);
                HttpResponse bmResp = http.get(bmUrl, apiHeaders);
                if (!bmResp.isOk()) {
                    if (bmResp.statusCode == 403 || bmResp.statusCode == 401) {
                        log("북마크 접근 권한 없음. PHPSESSID를 확인하세요.", "error", "pixiv");
                    } else {
                        log(QString("북마크 가져오기 실패 (HTTP %1)").arg(bmResp.statusCode), "error", "pixiv");
                    }
                    break;
                }

                QJsonObject bmBody = bmResp.json()["body"].toObject();
                QJsonArray works = bmBody["works"].toArray();
                if (works.isEmpty()) break;

                for (const auto &w : works) {
                    QString wid = w.toObject()["id"].toString();
                    if (!wid.isEmpty()) illustIds.append(wid);
                }
                int total = bmBody["total"].toInt(0);
                offset += works.size();
                hasMore = (offset < total);

                log(QString("북마크 %1/%2 로드...").arg(offset).arg(total), "info", "pixiv");
                QThread::msleep(static_cast<unsigned long>(delay * 1000));
            }

            // ── 북마크 소설도 수집 ──
            if (m_isRunning.value("pixiv", false)) {
                log("북마크 소설 가져오는 중...", "info", "pixiv");
                int nOffset = 0;
                int nLimit = 48;
                bool nHasMore = true;
                QList<QString> bmNovelIds;

                while (nHasMore && m_isRunning.value("pixiv", false)) {
                    QString bnUrl = QString("https://www.pixiv.net/ajax/user/%1/novels/bookmarks?tag=&offset=%2&limit=%3&rest=show")
                        .arg(userId).arg(nOffset).arg(nLimit);
                    HttpResponse bnResp = http.get(bnUrl, apiHeaders);
                    if (!bnResp.isOk()) {
                        if (bnResp.statusCode == 403 || bnResp.statusCode == 401) {
                            log("소설 북마크 접근 권한 없음.", "warning", "pixiv");
                        } else {
                            log(QString("소설 북마크 실패 (HTTP %1)").arg(bnResp.statusCode), "warning", "pixiv");
                        }
                        break;
                    }

                    QJsonObject bnBody = bnResp.json()["body"].toObject();
                    QJsonArray nWorks = bnBody["works"].toArray();
                    if (nWorks.isEmpty()) break;

                    for (const auto &nw : nWorks) {
                        QString nwid = nw.toObject()["id"].toString();
                        if (!nwid.isEmpty()) bmNovelIds.append(nwid);
                    }
                    int nTotal = bnBody["total"].toInt(0);
                    nOffset += nWorks.size();
                    nHasMore = (nOffset < nTotal);

                    log(QString("소설 북마크 %1/%2 로드...").arg(nOffset).arg(nTotal), "info", "pixiv");
                    QThread::msleep(static_cast<unsigned long>(delay * 1000));
                }

                // 북마크 소설 다운로드 (표지 + 개별 폴더)
                if (!bmNovelIds.isEmpty()) {
                    log(QString("북마크 소설 %1개 다운로드 시작...").arg(bmNovelIds.size()), "info", "pixiv");
                    for (int ni = 0; ni < bmNovelIds.size(); ni++) {
                        if (!m_isRunning.value("pixiv", false)) break;
                        if (downloadNovel(bmNovelIds[ni])) totalDownloaded++;
                        updateStats(totalDownloaded, illustIds.size() + ni + 1,
                            QString("북마크 소설 %1/%2").arg(ni + 1).arg(bmNovelIds.size()), "pixiv");
                        QThread::msleep(static_cast<unsigned long>(delay * 1000));
                    }
                }
            }
        } else {
            // Fetch all user works
            QString profileUrl = QString("https://www.pixiv.net/ajax/user/%1/profile/all").arg(userId);
            HttpResponse profResp = http.get(profileUrl, apiHeaders);
            if (!profResp.isOk()) {
                log(QString("유저 프로필 실패 (HTTP %1)").arg(profResp.statusCode), "error", "pixiv");
                return;
            }

            QJsonObject profBody = profResp.json()["body"].toObject();
            QJsonObject illusts = profBody["illusts"].toObject();
            QJsonObject manga = profBody["manga"].toObject();
            QJsonObject novels = profBody["novels"].toObject();

            // Collect all IDs (illusts + manga) — 소설 전용이면 건너뜀
            if (!novelsOnly) {
                for (auto it = illusts.begin(); it != illusts.end(); ++it) {
                    illustIds.append(it.key());
                }
                for (auto it = manga.begin(); it != manga.end(); ++it) {
                    if (!illustIds.contains(it.key()))
                        illustIds.append(it.key());
                }
            }

            // Sort by ID descending (newest first)
            std::sort(illustIds.begin(), illustIds.end(), [](const QString &a, const QString &b) {
                return a.toLongLong() > b.toLongLong();
            });

            // Collect novel IDs
            QList<QString> novelIds;
            for (auto it = novels.begin(); it != novels.end(); ++it) {
                novelIds.append(it.key());
            }
            std::sort(novelIds.begin(), novelIds.end(), [](const QString &a, const QString &b) {
                return a.toLongLong() > b.toLongLong();
            });

            log(QString("총 %1개 작품 + %2개 소설 발견").arg(illustIds.size()).arg(novelIds.size()), "info", "pixiv");

            // Download novels (표지 + 개별 폴더)
            if (!novelIds.isEmpty()) {
                log(QString("소설 %1개 다운로드 시작...").arg(novelIds.size()), "info", "pixiv");
                for (int ni = 0; ni < novelIds.size(); ni++) {
                    if (!m_isRunning.value("pixiv", false)) break;
                    if (downloadNovel(novelIds[ni])) totalDownloaded++;
                    updateStats(totalDownloaded, illustIds.size() + ni + 1,
                        QString("소설 %1/%2").arg(ni + 1).arg(novelIds.size()), "pixiv");
                    QThread::msleep(static_cast<unsigned long>(delay * 1000));
                }
            }
        }

        // 소설 전용 모드면 일러스트 건너뛰기
        if (!novelsOnly) {
            totalIllusts = illustIds.size();
            if (maxCount > 0 && totalIllusts > maxCount) {
                illustIds = illustIds.mid(0, maxCount);
                totalIllusts = maxCount;
                log(QString("최대 %1개까지 다운로드합니다.").arg(maxCount), "info", "pixiv");
            }

            for (int i = 0; i < illustIds.size(); i++) {
                if (!m_isRunning.value("pixiv", false)) {
                    log("사용자에 의해 중지됨.", "warning", "pixiv");
                    break;
                }

                const QString &iid = illustIds[i];
                updateStats(totalDownloaded, i + 1, QString("다운로드 중 %1/%2").arg(i + 1).arg(totalIllusts), "pixiv");

                if (downloadIllust(iid)) {
                    totalDownloaded++;
                }

                // Rate limit delay
                if (i < illustIds.size() - 1) {
                    QThread::msleep(static_cast<unsigned long>(delay * 1000));
                }
            }
        } else {
            log(QString("📖 소설 전용 모드 — 일러스트 %1개 건너뜀").arg(illustIds.size()), "info", "pixiv");
        }

        // ── Complete Excel: 기존 파일 + 새 데이터 합치기 ──
        if (saveExcel && !pxExcelData.isEmpty()) {
            QStringList hdrs = {"ID", "Title", "Author", "Author ID", "Type", "Pages",
                                "Created At", "Likes", "Bookmarks", "Views", "Comments",
                                "Tags", "Description", "URL"};

            // 고정 파일명 (complete 방식)
            QString pxUserDir = savePath + "/pixiv";
            QDir().mkpath(pxUserDir);
            QString excelSuffix = (type == "bookmarks") ? "_bookmarks_complete.xlsx" : "_complete.xlsx";
            QString excelPath = pxUserDir + "/" + userId + excelSuffix;

            // 기존 파일에서 ID 수집 + 기존 행 보존
            QSet<QString> existingIds;
            QJsonArray oldRows;
            if (QFile::exists(excelPath)) {
                QXlsx::Document existDoc(excelPath);
                int lastRow = existDoc.dimension().lastRow();
                for (int r = 2; r <= lastRow; ++r) {
                    QString eid = existDoc.read(r, 1).toString().trimmed();
                    if (eid.isEmpty() || eid.startsWith("─") || eid.startsWith("===")) continue;
                    existingIds.insert(eid);
                    QJsonObject eRow;
                    eRow["id"] = eid;
                    eRow["title"] = existDoc.read(r, 2).toString();
                    eRow["author"] = existDoc.read(r, 3).toString();
                    eRow["author_id"] = existDoc.read(r, 4).toString();
                    eRow["type"] = existDoc.read(r, 5).toString();
                    eRow["pages"] = existDoc.read(r, 6).toString().toInt();
                    eRow["created_at"] = existDoc.read(r, 7).toString();
                    eRow["likes"] = existDoc.read(r, 8).toString().toInt();
                    eRow["bookmarks"] = existDoc.read(r, 9).toString().toInt();
                    eRow["views"] = existDoc.read(r, 10).toString().toInt();
                    eRow["comments"] = existDoc.read(r, 11).toString().toInt();
                    eRow["tags"] = existDoc.read(r, 12).toString();
                    eRow["description"] = existDoc.read(r, 13).toString();
                    eRow["url"] = existDoc.read(r, 14).toString();
                    oldRows.append(eRow);
                }
                log(QString("기존 Excel: %1개 (새 %2개)").arg(existingIds.size()).arg(pxExcelData.size()), "info", "pixiv");
            }

            // 새 데이터에서 중복 제거
            QJsonArray newRows;
            for (const auto &v : pxExcelData) {
                QJsonObject r = v.toObject();
                if (!existingIds.contains(r["id"].toString())) {
                    newRows.append(r);
                }
            }

            // 날짜 정렬 함수
            auto sortByDate = [](QJsonArray &arr) {
                QList<QJsonObject> list;
                for (const auto &v : arr) list.append(v.toObject());
                std::sort(list.begin(), list.end(), [](const QJsonObject &a, const QJsonObject &b) {
                    return a["created_at"].toString() > b["created_at"].toString();
                });
                arr = QJsonArray();
                for (const auto &o : list) arr.append(o);
            };
            sortByDate(newRows);
            sortByDate(oldRows);

            // Excel 쓰기: 새 데이터 → 구분선 → 기존 데이터
            ExcelWriter excel;
            excel.writeHeader(hdrs, QColor("#0096FA"));
            int row = 2;

            auto writeRow = [&](const QJsonObject &r) {
                excel.writeRow(row++, {
                    r["id"].toString(), r["title"].toString(), r["author"].toString(),
                    r["author_id"].toString(), r["type"].toString(),
                    QString::number(r["pages"].toInt()), r["created_at"].toString(),
                    QString::number(r["likes"].toInt()), QString::number(r["bookmarks"].toInt()),
                    QString::number(r["views"].toInt()), QString::number(r["comments"].toInt()),
                    r["tags"].toString(), r["description"].toString(), r["url"].toString()
                });
            };

            for (const auto &v : newRows) writeRow(v.toObject());

            // 구분선
            if (!newRows.isEmpty() && !oldRows.isEmpty()) {
                QString sep = QString("═══ %1 新規 %2件 ═══")
                    .arg(QDateTime::currentDateTime().toString("yyyy/MM/dd HH:mm"))
                    .arg(newRows.size());
                excel.writeRow(row++, {sep});
            }

            for (const auto &v : oldRows) writeRow(v.toObject());

            excel.setColumnWidth(1, 12);  excel.setColumnWidth(2, 30);
            excel.setColumnWidth(3, 15);  excel.setColumnWidth(4, 12);
            excel.setColumnWidth(5, 8);   excel.setColumnWidth(6, 6);
            excel.setColumnWidth(7, 18);  excel.setColumnWidth(14, 35);

            if (excel.save(excelPath)) {
                int totalNew = newRows.size();
                int totalAll = newRows.size() + oldRows.size();
                log(QString("Excel 저장: +%1개 (총 %2개) → %3").arg(totalNew).arg(totalAll).arg(QFileInfo(excelPath).fileName()), "success", "pixiv");
            }
        }
    }

    updateStats(totalDownloaded, totalIllusts, "완료", "pixiv");
    log(QString("Pixiv 다운로드 완료! %1개 저장").arg(totalDownloaded), "success", "pixiv");
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ── Disk / TempDir Settings ──
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void MiyoBackend::browseTempDir()
{
    QString dir = QFileDialog::getExistingDirectory(m_window, "임시 저장 디스크 선택");
    if (!dir.isEmpty()) {
        setTempDir(dir);
    }
}

void MiyoBackend::browseSecondaryPath()
{
    QString dir = QFileDialog::getExistingDirectory(m_window, "보조 저장 경로 선택 (1번 가득 찼을 때 사용)");
    if (!dir.isEmpty()) {
        m_config->setSecondaryPath(dir);
        m_config->save();
        runJs(QString("setSecondaryPath('%1')").arg(dir));
    }
}

void MiyoBackend::getFreeSpaceGB(const QString &path)
{
    QString p = path;
    p.replace("~", QDir::homePath());
    qint64 bytes = Common::freeSpace(p);
    double gb = bytes / 1024.0 / 1024.0 / 1024.0;
    runJs(QString("onSecondaryFreeSpaceResult(%1)").arg(gb, 0, 'f', 2));
}

void MiyoBackend::setTempDir(const QString &path)
{
    m_config->setTempDir(path);
    m_config->save();
    runJs(QString("setTempDirPath('%1')").arg(path));
    runJs(QString("closeDiskModal()"));

    // 디스크 여유 공간 표시
    QStorageInfo storage(path);
    qint64 avail = storage.bytesAvailable();
    qint64 total = storage.bytesTotal();
    auto fmtSize = [](qint64 b) -> QString {
        if (b < qint64(1024)*1024*1024) return QString::number(b/1024.0/1024.0, 'f', 1) + " MB";
        return QString::number(b/1024.0/1024.0/1024.0, 'f', 1) + " GB";
    };
    QString diskInfo = QString("디스크: %1 — 여유 %2 / 전체 %3")
        .arg(storage.displayName(), fmtSize(avail), fmtSize(total));
    runJs(QString("document.getElementById('settings-disk-info').textContent='%1'").arg(diskInfo));
    log("임시 디스크 설정: " + path + " (" + fmtSize(avail) + " 여유)", "success", "trad");
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ── trad (steganography: hide files in PNG) ──
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void MiyoBackend::getTradCoverBase64()
{
    QString coverPath = m_config->tradCoverPath();
    QFile coverFile;

    if (!coverPath.isEmpty() && QFile::exists(coverPath)) {
        coverFile.setFileName(coverPath);
    } else {
        coverFile.setFileName(":/trad_cover.png");
    }

    if (coverFile.open(QIODevice::ReadOnly)) {
        QByteArray data = coverFile.readAll();
        coverFile.close();
        QString base64 = QString::fromLatin1(data.toBase64());
        runJs(QString("setTradCover('%1')").arg(base64));
    }
}

void MiyoBackend::selectTradCover()
{
    QString path = QFileDialog::getOpenFileName(m_window, "커버 이미지 선택", QString(), "Images (*.png *.jpg *.jpeg)");
    if (path.isEmpty()) return;

    m_config->setTradCoverPath(path);
    m_config->save();
    getTradCoverBase64();
    log("커버 변경: " + path, "success", "trad");
}

void MiyoBackend::selectTradFiles()
{
    QStringList files = QFileDialog::getOpenFileNames(m_window, "파일 선택");
    if (files.isEmpty()) return;

    QJsonArray fileArray;
    for (const QString &f : files) {
        QFileInfo info(f);
        QJsonObject obj;
        obj["name"] = info.fileName();
        obj["path"] = info.absoluteFilePath();
        obj["size"] = info.size();
        fileArray.append(obj);
    }

    // Base64 인코딩으로 특수문자/따옴표 문제 방지
    QByteArray jsonBytes = QJsonDocument(fileArray).toJson(QJsonDocument::Compact);
    QString b64 = QString::fromLatin1(jsonBytes.toBase64());
    runJs(QString("setTradFiles(b64toUtf8('%1'))").arg(b64));
    log(QString("%1개 파일 선택").arg(files.size()), "info", "trad");
}

void MiyoBackend::selectTradFolder()
{
    QString dir = QFileDialog::getExistingDirectory(m_window, "폴더 선택");
    if (dir.isEmpty()) return;

    QJsonArray fileArray;
    QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        it.next();
        QFileInfo info = it.fileInfo();
        QJsonObject obj;
        obj["name"] = info.fileName();
        obj["path"] = info.absoluteFilePath();
        obj["size"] = info.size();
        fileArray.append(obj);
    }

    if (fileArray.isEmpty()) {
        log("폴더에 파일이 없습니다", "warning", "trad");
        return;
    }

    // Base64 인코딩으로 특수문자/따옴표 문제 방지
    QByteArray jsonBytes = QJsonDocument(fileArray).toJson(QJsonDocument::Compact);
    QString b64 = QString::fromLatin1(jsonBytes.toBase64());
    runJs(QString("setTradFiles(b64toUtf8('%1'))").arg(b64));
    log(QString("폴더에서 %1개 파일 추가: %2").arg(fileArray.size()).arg(dir), "info", "trad");
}

void MiyoBackend::stopTrad()
{
    m_tradCancelled.store(true);
    log("중단 요청됨", "warning", "trad");
}

void MiyoBackend::startTrad(const QString &configJson)
{
    m_tradCancelled.store(false);
    QJsonObject config = QJsonDocument::fromJson(configJson.toUtf8()).object();
    QString savePath = config["path"].toString();
    savePath.replace("~", QDir::homePath());
    QDir().mkpath(savePath);
    QJsonArray files = config["files"].toArray();
    QString outputFilename = config["filename"].toString();

    if (files.isEmpty()) {
        log("파일을 선택하세요", "error", "trad");
        runJs("setTradBusy(false)");
        return;
    }
    // ★ 터미널 등록 — 다른 platform 의 로그가 trad 터미널에 섞이는 거 방지
    openTerminalLog("trad", savePath);

    if (outputFilename.isEmpty()) outputFilename = "result.png";
    if (!outputFilename.endsWith(".png")) outputFilename += ".png";

    // 임시파일 경로: 설정된 디스크 > 저장 경로 (시스템 폴더 사용 안 함)
    QString configuredTempDir = m_config->tempDir();
    QString tempDir;
    if (!configuredTempDir.isEmpty() && QDir(configuredTempDir).exists()) {
        tempDir = configuredTempDir + "/.trad_tmp";
    } else {
        tempDir = savePath + "/.trad_tmp";
    }
    QDir().mkpath(tempDir);

    // Run in thread
    QThread *thread = QThread::create([this, files, savePath, tempDir, outputFilename]() {
        auto progress = [this](int pct, const QString &text) {
            runJs(QString("setTradProgress(%1, '%2')").arg(pct).arg(text));
        };

        // 중단 체크 헬퍼
        auto cancelled = [this, &progress, &tempDir]() -> bool {
            if (!m_tradCancelled.load()) return false;
            log("중단됨", "warning", "trad");
            QDir(tempDir).removeRecursively();
            progress(-1, "");
            runJs("setTradBusy(false)");
            return true;
        };

        auto formatSize = [](qint64 bytes) -> QString {
            if (bytes < qint64(1024)*1024)
                return QString::number(bytes/1024.0, 'f', 1) + " KB";
            if (bytes < qint64(1024)*1024*1024)
                return QString::number(bytes/1024.0/1024.0, 'f', 1) + " MB";
            if (bytes < qint64(1024)*1024*1024*1024)
                return QString::number(bytes/1024.0/1024.0/1024.0, 'f', 2) + " GB";
            return QString::number(bytes/1024.0/1024.0/1024.0/1024.0, 'f', 2) + " TB";
        };

        if (cancelled()) return;
        progress(5, "준비 중...");
        log(QString("파일 %1개 처리 시작").arg(files.size()), "info", "trad");

        // 일반 파일 확장자 목록 (이미지/동영상/문서/음악/아카이브)
        static const QSet<QString> standardExts = {
            // 이미지
            "png", "jpg", "jpeg", "gif", "bmp", "webp", "svg", "ico", "tiff", "tif", "heic", "avif",
            // 동영상
            "mp4", "mkv", "avi", "mov", "wmv", "flv", "webm", "m4v", "ts", "m3u8",
            // 음악
            "mp3", "wav", "flac", "aac", "ogg", "m4a", "wma", "opus",
            // 문서
            "pdf", "doc", "docx", "xls", "xlsx", "ppt", "pptx", "txt", "csv", "rtf", "odt",
            // 아카이브 (이미 압축됨)
            "zip", "rar", "7z", "tar", "gz", "bz2", "xz", "zst",
            // 기타 일반
            "html", "htm", "xml", "json", "psd", "ai", "eps"
        };

        // Calculate total input size, collect paths, auto-compress non-standard files
        qint64 totalInputSize = 0;
        QStringList filePaths;
        QStringList autoCompressedTempFiles;  // 자동 압축된 임시파일 (나중에 삭제)

        QString pyPath = Common::bundledPythonPath();
        if (!QFile::exists(pyPath)) pyPath = "python3";

        for (const auto &f : files) {
            QString fpath = f.toObject()["path"].toString();
            QFileInfo fi(fpath);
            QString ext = fi.suffix().toLower();

            if (!standardExts.contains(ext) && !ext.isEmpty()) {
                // 비표준 파일 → 자동 zip 압축
                QString compPath = tempDir + "/" + fi.fileName() + ".zip";
                // 같은 이름 충돌 방지
                int dupIdx = 1;
                while (QFile::exists(compPath)) {
                    compPath = tempDir + "/" + fi.completeBaseName() + "_" + QString::number(dupIdx++) + "." + ext + ".zip";
                }
                log("  ⚙ 압축: " + fi.fileName() + " → " + fi.fileName() + ".zip", "info", "trad");

                QProcess zipProc;
                zipProc.start(pyPath, {"-c",
                    "import zipfile,sys,os\n"
                    "with zipfile.ZipFile(sys.argv[1],'w',zipfile.ZIP_DEFLATED,allowZip64=True) as z:\n"
                    "    z.write(sys.argv[2],os.path.basename(sys.argv[2]))\n",
                    compPath, fpath});
                // 대용량 파일 대응: 무제한 대기 (10GB 이상이면 수 분 이상 소요)
                if (zipProc.waitForFinished(-1) && zipProc.exitCode() == 0) {
                    QFileInfo ci(compPath);
                    totalInputSize += ci.size();
                    filePaths << compPath;
                    autoCompressedTempFiles << compPath;
                    log("  + " + fi.fileName() + ".zip (" + formatSize(ci.size()) + ")", "info", "trad");
                } else {
                    // 압축 실패 시 원본 그대로
                    totalInputSize += fi.size();
                    filePaths << fpath;
                    log("  + " + fi.fileName() + " (" + formatSize(fi.size()) + ") [압축 실패, 원본 사용]", "warning", "trad");
                }
            } else {
                qint64 fsize = fi.size();
                totalInputSize += fsize;
                filePaths << fpath;
                log("  + " + fi.fileName() + " (" + formatSize(fsize) + ")", "info", "trad");
            }
        }
        log(QString("총 입력 크기: %1").arg(formatSize(totalInputSize)), "info", "trad");

        // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
        // 매니페스트 JSON 생성 — 7zip/zip 스타일 무결성 정보
        // __TRAD_MANIFEST__.json 으로 ZIP 맨 앞에 삽입됨
        // ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
        QString manifestPath = tempDir + "/__TRAD_MANIFEST__.json";
        {
            QJsonObject manifest;
            manifest["version"] = "1";
            manifest["app"] = "Chernobyl";
            manifest["app_version"] = QCoreApplication::applicationVersion().isEmpty()
                ? QString("1.0") : QCoreApplication::applicationVersion();
            manifest["format"] = "PNG+ZIP Polyglot";
            manifest["platform"] = QSysInfo::prettyProductName();
            manifest["kernel"] = QSysInfo::kernelType() + " " + QSysInfo::kernelVersion();
            manifest["arch"] = QSysInfo::currentCpuArchitecture();
            manifest["created_at"] = QDateTime::currentDateTime().toString(Qt::ISODate);
            manifest["created_unix"] = QDateTime::currentSecsSinceEpoch();
            manifest["file_count"] = static_cast<int>(filePaths.size());
            manifest["total_size"] = static_cast<qint64>(totalInputSize);
            manifest["output_filename"] = outputFilename;
            // Cross-OS 추출 가이드 (사용자가 manifest 보면 다른 OS 에서도 쉽게 추출 가능)
            QJsonObject extractGuide;
            extractGuide["macos"] = "확장자를 .zip 으로 변경 후 더블클릭 (Archive Utility) 또는 unzip 명령";
            extractGuide["windows"] = "동봉된 .zip 파일을 더블클릭 (탐색기) 또는 7-Zip / WinRAR. .png 자체도 7-Zip 으로 열림";
            extractGuide["linux"] = "unzip *.zip 또는 file-roller / Ark / xarchiver";
            extractGuide["polyglot_note"] = "이 파일은 PNG 헤더 + ZIP 데이터 결합 — 둘 다 valid. 이미지 뷰어에선 그림으로 보이고 ZIP 도구로 열면 파일 추출 가능";
            manifest["extract_guide"] = extractGuide;

            QJsonArray manifestFiles;
            for (int idx = 0; idx < filePaths.size(); ++idx) {
                const QString &fp = filePaths[idx];
                QFileInfo fi(fp);
                QJsonObject entry;
                entry["name"] = fi.fileName();
                entry["size"] = fi.size();
                entry["index"] = idx;

                // 빠른 무결성을 위한 CRC32 (첫 1MB + 마지막 1MB)
                // 대용량 파일 전체를 해시하면 매우 느리므로 샘플 기반 체크
                QFile fh(fp);
                if (fh.open(QIODevice::ReadOnly)) {
                    qint64 fsz = fh.size();
                    QCryptographicHash hasher(QCryptographicHash::Sha1);
                    const qint64 SAMPLE = 1024 * 1024; // 1MB
                    if (fsz <= SAMPLE * 2) {
                        // 작은 파일: 전체 해시
                        hasher.addData(fh.readAll());
                    } else {
                        // 큰 파일: 앞 1MB + 뒤 1MB 샘플 해시
                        QByteArray head = fh.read(SAMPLE);
                        hasher.addData(head);
                        fh.seek(fsz - SAMPLE);
                        QByteArray tail = fh.read(SAMPLE);
                        hasher.addData(tail);
                    }
                    fh.close();
                    entry["sha1"] = QString::fromLatin1(hasher.result().toHex());
                } else {
                    entry["sha1"] = "";
                }
                manifestFiles.append(entry);
            }
            manifest["files"] = manifestFiles;

            // 매니페스트를 파일로 저장 (ZIP에 포함할 용도)
            QFile mf(manifestPath);
            if (mf.open(QIODevice::WriteOnly)) {
                mf.write(QJsonDocument(manifest).toJson(QJsonDocument::Indented));
                mf.close();
                log(QString("매니페스트 생성 (%1개 파일)").arg(filePaths.size()), "info", "trad");

                // 매니페스트를 filePaths 맨 앞에 추가 (ZIP의 첫 엔트리로)
                filePaths.prepend(manifestPath);
                autoCompressedTempFiles << manifestPath; // 추출 후 삭제

                // UI 미리보기 업데이트
                QString b64 = QString::fromUtf8(
                    QJsonDocument(manifest).toJson(QJsonDocument::Compact).toBase64());
                runJs(QString("setTradManifest(JSON.parse(b64toUtf8('%1')))").arg(b64));
            } else {
                log("매니페스트 파일 생성 실패 — 매니페스트 없이 진행", "warning", "trad");
            }
        }

        // Disk space check: ensure save path has enough room
        QStorageInfo saveStorage(savePath);
        qint64 availableSpace = saveStorage.bytesAvailable();
        qint64 requiredSpace = totalInputSize + qint64(100)*1024*1024; // input size + 100MB margin
        log(QString("저장 디스크 여유: %1 / 필요: %2").arg(formatSize(availableSpace)).arg(formatSize(requiredSpace)), "info", "trad");
        if (availableSpace < requiredSpace) {
            log(QString("디스크 공간 부족! 여유: %1, 필요: %2 — 다른 디스크를 선택하세요").arg(formatSize(availableSpace)).arg(formatSize(requiredSpace)), "error", "trad");
            progress(-1, ""); runJs("setTradBusy(false)");
            return;
        }

        // ★ 시스템 /tmp 안 봄 — 사용자 임시 디스크 (Common::resolveTempBase) 와 비교
        QString userTmpBase = Common::resolveTempBase(m_config ? m_config->tempDir() : QString());
        QStorageInfo userTmpStorage(userTmpBase);
        if (saveStorage.rootPath() != userTmpStorage.rootPath()) {
            log(QString("임시 디스크 별도 사용: %1 — save 디스크 (%2) 부담 분산")
                .arg(userTmpStorage.rootPath()).arg(saveStorage.rootPath()), "info", "trad");
        } else {
            log("임시 디스크 = save 디스크 — 같은 마운트 (cross-fs rename 0)", "info", "trad");
        }

        if (cancelled()) return;
        // Step 1: Determine format before creating ZIP
        // Load cover image info first to decide polyglot vs ZIP-only
        QString coverPath = m_config->tradCoverPath();
        QString actualCoverPath;
        bool needsCleanup = false;

        if (!coverPath.isEmpty() && QFile::exists(coverPath)) {
            if (coverPath.toLower().endsWith(".jpg") || coverPath.toLower().endsWith(".jpeg")) {
                QString convertedPath = tempDir + "/trad_cover_converted.png";
#ifdef Q_OS_MACOS
                QProcess sips;
                sips.start("/usr/bin/sips", {"-s", "format", "png", coverPath, "--out", convertedPath});
                if (sips.waitForFinished(30000) && sips.exitCode() == 0) {
                    actualCoverPath = convertedPath;
                    needsCleanup = true;
                    log("JPEG→PNG 변환 완료", "info", "trad");
                } else {
                    log("JPEG→PNG 변환 실패, 기본 커버 사용", "warning", "trad");
                    actualCoverPath = ":/trad_cover.png";
                }
#else
                // Windows/Linux: QImage로 JPEG→PNG 변환
                QImage img(coverPath);
                if (!img.isNull() && img.save(convertedPath, "PNG")) {
                    actualCoverPath = convertedPath;
                    needsCleanup = true;
                    log("JPEG→PNG 변환 완료", "info", "trad");
                } else {
                    log("JPEG→PNG 변환 실패, 기본 커버 사용", "warning", "trad");
                    actualCoverPath = ":/trad_cover.png";
                }
#endif
            } else {
                actualCoverPath = coverPath;
            }
        } else {
            actualCoverPath = ":/trad_cover.png";
        }

        QFile coverFile(actualCoverPath);
        if (!coverFile.open(QIODevice::ReadOnly)) {
            log("커버 이미지 열기 실패", "error", "trad");
            progress(-1, ""); runJs("setTradBusy(false)");
            return;
        }
        qint64 imgLen = coverFile.size();

        // Estimate ZIP size (store mode = input size + ~100 bytes per file for headers)
        qint64 estimatedZipLen = totalInputSize + files.size() * 200 + 1024;
        // PNG+ZIP polyglot은 항상 가능 (ZIP64 오프셋 대응, 스트리밍 I/O)
        bool usePolyglot = true;

        QString outputPath = savePath + "/" + outputFilename;

        if (cancelled()) return;
        // Step 2: Create ZIP
        // For polyglot: temp dir (will be modified then combined with cover)
        // For ZIP-only: directly in output dir (rename to final name, no copy!)
        QString zipPath = tempDir + "/trad_temp.zip";
        QFile::remove(zipPath);

        progress(10, "ZIP 압축 중...");
        log(QString("파일 %1개 ZIP 압축 중...").arg(files.size()), "info", "trad");

        // ZIP 생성: 번들 Python zipfile (UTF-8 파일명 보장)
        {
            QString pyBundled2 = Common::bundledPythonPath();
            QString pyZipCmd = QFile::exists(pyBundled2) ? pyBundled2 : "python3";

            // 파일 목록을 임시 파일로 전달 (셸 이스케이프 문제 방지)
            QString fileListPath = savePath + "/.trad_filelist.txt";
            {
                QFile fl(fileListPath);
                if (fl.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    for (const QString &fp : filePaths)
                        fl.write((fp + "\n").toUtf8());
                    fl.close();
                }
            }

            QString pyScript = QString(
                "import zipfile, os, sys, stat\n"
                "zp = sys.argv[1]\n"
                "fl = sys.argv[2]\n"
                "CHUNK = 8 * 1024 * 1024  # 8MB streaming chunks\n"
                "try:\n"
                "    with open(fl, 'r', encoding='utf-8') as f:\n"
                "        files = [l.strip() for l in f if l.strip()]\n"
                "    used = {}\n"
                "    with zipfile.ZipFile(zp, 'w', zipfile.ZIP_STORED, allowZip64=True) as zf:\n"
                "        for idx, fp in enumerate(files):\n"
                "            name = os.path.basename(fp)\n"
                "            base, ext = os.path.splitext(name)\n"
                "            if name in used:\n"
                "                used[name] += 1\n"
                "                name = f'{base}_{used[name]}{ext}'\n"
                "            else:\n"
                "                used[name] = 0\n"
                "            fsize = os.path.getsize(fp)\n"
                "            if fsize > 512 * 1024 * 1024:\n"
                "                # Large file: stream into ZIP to avoid memory spike\n"
                "                info = zipfile.ZipInfo(name)\n"
                "                info.compress_type = zipfile.ZIP_STORED\n"
                "                info.file_size = fsize\n"
                "                with zf.open(info, 'w') as dest:\n"
                "                    with open(fp, 'rb') as src:\n"
                "                        while True:\n"
                "                            chunk = src.read(CHUNK)\n"
                "                            if not chunk:\n"
                "                                break\n"
                "                            dest.write(chunk)\n"
                "            else:\n"
                "                zf.write(fp, name)\n"
                "            sys.stdout.write(f'PROGRESS {idx+1}/{len(files)}\\n')\n"
                "            sys.stdout.flush()\n"
                "    print(f'OK {len(files)}')\n"
                "except Exception as e:\n"
                "    print(f'ERROR: {e}', file=sys.stderr)\n"
                "    sys.exit(1)\n"
            );

            QProcess zipProc;
            zipProc.setProcessChannelMode(QProcess::MergedChannels);
            zipProc.start(pyZipCmd, {"-c", pyScript, zipPath, fileListPath});
            log(QString("ZIP 생성: 번들 Python (UTF-8), %1개 파일...").arg(filePaths.size()), "info", "trad");

            // 대용량 파일 대응: 진행 상황 모니터링 + 중단 체크
            bool zipFinished = false;
            while (!zipFinished) {
                if (m_tradCancelled.load()) {
                    zipProc.kill();
                    zipProc.waitForFinished(5000);
                    QFile::remove(fileListPath);
                    QFile::remove(zipPath);
                    cancelled();
                    return;
                }
                zipFinished = zipProc.waitForFinished(3000); // 3초마다 체크
                if (!zipFinished && zipProc.state() == QProcess::Running) {
                    // ZIP 진행 중 — 크기 확인으로 진행률 표시
                    QFileInfo zipCheck(zipPath);
                    if (zipCheck.exists()) {
                        qint64 currentSize = zipCheck.size();
                        int pct = 10 + static_cast<int>((currentSize * 20) / qMax(totalInputSize, qint64(1)));
                        if (pct > 29) pct = 29;
                        progress(pct, QString("ZIP 압축 중... %1 / %2").arg(formatSize(currentSize), formatSize(totalInputSize)));
                    }
                }
                // Read stdout for PROGRESS lines
                while (zipProc.canReadLine()) {
                    QString line = QString::fromUtf8(zipProc.readLine()).trimmed();
                    if (line.startsWith("PROGRESS "))
                        log(QString("ZIP 파일 진행: %1").arg(line.mid(9)), "info", "trad");
                }
            }
            if (zipProc.exitCode() != 0) {
                QString errStr = QString::fromUtf8(zipProc.readAll()).left(500);
                if (errStr.isEmpty()) errStr = QString("exit code: %1").arg(zipProc.exitCode());
                log("ZIP 생성 실패: " + errStr, "error", "trad");
                QFile::remove(fileListPath);
                coverFile.close();
                progress(-1, ""); runJs("setTradBusy(false)");
                return;
            }
            QFile::remove(fileListPath);
        }

        QFileInfo zipInfo(zipPath);
        qint64 zipLen = zipInfo.size();
        log(QString("ZIP 생성 완료 (%1)").arg(formatSize(zipLen)), "success", "trad");

        // ZIP-only 모드 제거 — 항상 polyglot (PNG) 생성 (ZIP64 오프셋 대응)
        if (false) {
            // ZIP-only: just rename to output path (same filesystem = instant, no copy)
            coverFile.close();
            if (needsCleanup) QFile::remove(actualCoverPath);

            // Change extension to .zip for ZIP-only mode
            if (outputPath.endsWith(".png")) {
                outputPath = outputPath.left(outputPath.length() - 4) + ".zip";
            }
            log("대용량 파일 → ZIP 포맷으로 저장: " + QFileInfo(outputPath).fileName(), "info", "trad");

            progress(80, "파일 이동 중...");
            QFile::remove(outputPath);
            if (!QFile::rename(zipPath, outputPath)) {
                // Cross-device fallback: stream copy
                log("같은 디스크가 아님, 복사 중...", "info", "trad");
                QFile src(zipPath);
                QFile dst(outputPath);
                if (!src.open(QIODevice::ReadOnly) || !dst.open(QIODevice::WriteOnly)) {
                    log("파일 복사 실패", "error", "trad");
                    QFile::remove(zipPath);
                    progress(-1, ""); runJs("setTradBusy(false)");
                    return;
                }
                const int CHUNK = 8 * 1024 * 1024; // 8MB chunks for large files
                qint64 written = 0;
                while (!src.atEnd()) {
                    if (m_tradCancelled.load()) {
                        src.close(); dst.close();
                        QFile::remove(outputPath); QFile::remove(zipPath);
                        cancelled(); return;
                    }
                    QByteArray chunk = src.read(CHUNK);
                    dst.write(chunk);
                    written += chunk.size();
                    int pct = 80 + static_cast<int>((written * 10) / qMax(zipLen, qint64(1)));
                    if (pct > 90) pct = 90;
                    progress(pct, QString("복사 중... %1 / %2").arg(formatSize(written), formatSize(zipLen)));
                }
                src.close();
                dst.close();
                QFile::remove(zipPath);
            }
        } else {
            // Polyglot: PNG + ZIP (append-after-IEND 방식)
            // 구조: [완전한 PNG (IEND 포함)][전체 ZIP (오프셋 조정됨)]
            // → 이미지 뷰어: IEND까지 읽고 정상 PNG로 표시
            // → unzip: 파일 끝에서 EOCD 탐색 → CD → LFH → 정상 추출
            // → 웹 플랫폼이 PNG를 재인코딩해도 파일 첨부로 올리면 원본 보존
            // → 디스코드/구글드라이브/메가 등 파일 첨부 시 바이트 그대로 보존
            progress(30, "PNG+ZIP 결합 준비 중...");

            // Read cover PNG (small file — safe to load)
            coverFile.seek(0);
            QByteArray pngData = coverFile.readAll();
            coverFile.close();
            qint64 pngSize = pngData.size();

            // Open ZIP for reading structure
            QFile zipFile(zipPath);
            if (!zipFile.open(QIODevice::ReadOnly)) {
                log("ZIP 파일 열기 실패", "error", "trad");
                progress(-1, ""); runJs("setTradBusy(false)");
                return;
            }
            qint64 zipFileSize = zipFile.size();

            // Find EOCD by reading last 64KB
            qint64 searchSize = qMin(zipFileSize, qint64(65536));
            zipFile.seek(zipFileSize - searchSize);
            QByteArray tailData = zipFile.read(searchSize);

            qint64 eocdPosInTail = -1;
            for (qint64 i = tailData.size() - 22; i >= 0; --i) {
                if (static_cast<unsigned char>(tailData[i]) == 0x50 &&
                    static_cast<unsigned char>(tailData[i+1]) == 0x4b &&
                    static_cast<unsigned char>(tailData[i+2]) == 0x05 &&
                    static_cast<unsigned char>(tailData[i+3]) == 0x06) {
                    eocdPosInTail = i;
                    break;
                }
            }

            if (eocdPosInTail < 0) {
                log("ZIP EOCD를 찾을 수 없습니다", "error", "trad");
                zipFile.close(); QFile::remove(zipPath);
                progress(-1, ""); runJs("setTradBusy(false)");
                return;
            }

            qint64 eocdPos = (zipFileSize - searchSize) + eocdPosInTail;

            // Read central directory offset from EOCD (32-bit field)
            quint32 cdrOffset32 = static_cast<quint32>(
                (static_cast<unsigned char>(tailData[eocdPosInTail + 16])) |
                (static_cast<unsigned char>(tailData[eocdPosInTail + 17]) << 8) |
                (static_cast<unsigned char>(tailData[eocdPosInTail + 18]) << 16) |
                (static_cast<unsigned char>(tailData[eocdPosInTail + 19]) << 24));

            // ZIP64 지원: cdrOffset32 == 0xFFFFFFFF이면 ZIP64 EOCD에서 실제 64비트 오프셋을 읽어야 함
            qint64 cdrOffset64 = static_cast<qint64>(cdrOffset32);
            bool isZip64 = (cdrOffset32 == 0xFFFFFFFF);

            if (isZip64) {
                // ZIP64 EOCD Locator: EOCD 바로 앞 20바이트 (sig PK\x06\x07)
                // tailData 내에서 찾기
                if (eocdPosInTail >= 20) {
                    qint64 locPos = eocdPosInTail - 20;
                    if (static_cast<unsigned char>(tailData[locPos]) == 0x50 &&
                        static_cast<unsigned char>(tailData[locPos+1]) == 0x4b &&
                        static_cast<unsigned char>(tailData[locPos+2]) == 0x06 &&
                        static_cast<unsigned char>(tailData[locPos+3]) == 0x07) {
                        // ZIP64 EOCD Locator → offset to ZIP64 EOCD (8 bytes at locPos+8)
                        quint64 z64EocdAbsOff = 0;
                        for (int i = 0; i < 8; i++)
                            z64EocdAbsOff |= static_cast<quint64>(static_cast<unsigned char>(tailData[locPos + 8 + i])) << (i * 8);

                        // ZIP64 EOCD 읽기 (파일 절대 위치)
                        zipFile.seek(static_cast<qint64>(z64EocdAbsOff));
                        QByteArray z64Eocd = zipFile.read(56);
                        if (z64Eocd.size() >= 56 &&
                            static_cast<unsigned char>(z64Eocd[0]) == 0x50 &&
                            static_cast<unsigned char>(z64Eocd[1]) == 0x4b &&
                            static_cast<unsigned char>(z64Eocd[2]) == 0x06 &&
                            static_cast<unsigned char>(z64Eocd[3]) == 0x06) {
                            // CD offset: ZIP64 EOCD 내 offset 48 (8 bytes)
                            quint64 realCdrOff = 0;
                            for (int i = 0; i < 8; i++)
                                realCdrOff |= static_cast<quint64>(static_cast<unsigned char>(z64Eocd[48 + i])) << (i * 8);
                            cdrOffset64 = static_cast<qint64>(realCdrOff);
                            log(QString("ZIP64: CD offset = %1 (0x%2)").arg(cdrOffset64).arg(cdrOffset64, 0, 16), "info", "trad");
                        } else {
                            log("ZIP64 EOCD 시그니처 불일치", "error", "trad");
                            zipFile.close(); QFile::remove(zipPath);
                            progress(-1, ""); runJs("setTradBusy(false)");
                            return;
                        }
                    } else {
                        log("ZIP64 EOCD Locator를 찾을 수 없습니다", "error", "trad");
                        zipFile.close(); QFile::remove(zipPath);
                        progress(-1, ""); runJs("setTradBusy(false)");
                        return;
                    }
                } else {
                    log("ZIP64 EOCD Locator 공간 부족", "error", "trad");
                    zipFile.close(); QFile::remove(zipPath);
                    progress(-1, ""); runJs("setTradBusy(false)");
                    return;
                }
            }

            // CD+EOCD section 읽기 (실제 64비트 오프셋 사용)
            zipFile.seek(cdrOffset64);
            QByteArray cdSection = zipFile.readAll();
            zipFile.close();

            if (cdSection.isEmpty()) {
                log("CD section 읽기 실패", "error", "trad");
                QFile::remove(zipPath);
                progress(-1, ""); runJs("setTradBusy(false)");
                return;
            }

            qint64 lfhSize = cdrOffset64; // LFH section: 0..cdrOffset64
            qint64 offsetAdj = pngSize;   // ZIP offsets shift by PNG size

            progress(40, "오프셋 조정 중...");

            // Adjust CD entries: local file header offsets += pngSize
            qint64 cdPos = 0;
            qint64 cdEocdLocalPos = eocdPos - cdrOffset64;
            while (cdPos < cdEocdLocalPos) {
                if (cdPos + 46 > cdSection.size()) break;
                if (static_cast<unsigned char>(cdSection[cdPos]) != 0x50 ||
                    static_cast<unsigned char>(cdSection[cdPos+1]) != 0x4b ||
                    static_cast<unsigned char>(cdSection[cdPos+2]) != 0x01 ||
                    static_cast<unsigned char>(cdSection[cdPos+3]) != 0x02) break;

                quint16 nameLen = static_cast<quint16>(
                    static_cast<unsigned char>(cdSection[cdPos+28]) | (static_cast<unsigned char>(cdSection[cdPos+29]) << 8));
                quint16 extraLen = static_cast<quint16>(
                    static_cast<unsigned char>(cdSection[cdPos+30]) | (static_cast<unsigned char>(cdSection[cdPos+31]) << 8));
                quint16 commentLen = static_cast<quint16>(
                    static_cast<unsigned char>(cdSection[cdPos+32]) | (static_cast<unsigned char>(cdSection[cdPos+33]) << 8));

                // Adjust local file header offset (offset 42, 4 bytes)
                // ZIP64: 값이 0xFFFFFFFF면 extra field에 64비트 offset 존재
                quint32 oldLfhOff32 = static_cast<quint32>(
                    static_cast<unsigned char>(cdSection[cdPos+42]) |
                    (static_cast<unsigned char>(cdSection[cdPos+43]) << 8) |
                    (static_cast<unsigned char>(cdSection[cdPos+44]) << 16) |
                    (static_cast<unsigned char>(cdSection[cdPos+45]) << 24));

                if (oldLfhOff32 == 0xFFFFFFFF && extraLen >= 4) {
                    // ZIP64 extra field: 파싱하여 64비트 offset 조정
                    qint64 extraStart = cdPos + 46 + nameLen;
                    if (extraStart + extraLen <= cdSection.size()) {
                        qint64 ep = 0;
                        while (ep + 4 <= extraLen) {
                            quint16 eTag = static_cast<quint16>(
                                static_cast<unsigned char>(cdSection[extraStart + ep]) |
                                (static_cast<unsigned char>(cdSection[extraStart + ep + 1]) << 8));
                            quint16 eSize = static_cast<quint16>(
                                static_cast<unsigned char>(cdSection[extraStart + ep + 2]) |
                                (static_cast<unsigned char>(cdSection[extraStart + ep + 3]) << 8));
                            if (eTag == 0x0001) { // ZIP64 extended info
                                // ZIP64 extra 구조: [uncomp_size:8][comp_size:8][offset:8][disk:4]
                                // 실제 필드 순서는 원본 필드가 0xFFFFFFFF인 것만 포함
                                quint32 origUncomp = static_cast<quint32>(
                                    static_cast<unsigned char>(cdSection[cdPos+24]) |
                                    (static_cast<unsigned char>(cdSection[cdPos+25]) << 8) |
                                    (static_cast<unsigned char>(cdSection[cdPos+26]) << 16) |
                                    (static_cast<unsigned char>(cdSection[cdPos+27]) << 24));
                                quint32 origComp = static_cast<quint32>(
                                    static_cast<unsigned char>(cdSection[cdPos+20]) |
                                    (static_cast<unsigned char>(cdSection[cdPos+21]) << 8) |
                                    (static_cast<unsigned char>(cdSection[cdPos+22]) << 16) |
                                    (static_cast<unsigned char>(cdSection[cdPos+23]) << 24));
                                // offIdx = extra field 내 offset 필드의 상대 위치 (ep 기준)
                                int offRel = 4; // eTag(2) + eSize(2) 이후
                                if (origUncomp == 0xFFFFFFFF) offRel += 8;
                                if (origComp == 0xFFFFFFFF) offRel += 8;
                                // 범위 체크: extra field 내에 8바이트가 들어가는지 + cdSection 절대 범위
                                if (offRel + 8 <= 4 + eSize &&
                                    extraStart + ep + offRel + 8 <= cdSection.size()) {
                                    qint64 absIdx = extraStart + ep + offRel;
                                    quint64 oldOff64 = 0;
                                    for (int b = 0; b < 8; b++)
                                        oldOff64 |= static_cast<quint64>(static_cast<unsigned char>(cdSection[absIdx + b])) << (b * 8);
                                    quint64 newOff64 = oldOff64 + static_cast<quint64>(offsetAdj);
                                    for (int b = 0; b < 8; b++)
                                        cdSection[absIdx + b] = static_cast<char>((newOff64 >> (b * 8)) & 0xFF);
                                }
                                break;
                            }
                            ep += 4 + eSize;
                        }
                    }
                } else {
                    // 일반 32비트 오프셋 조정
                    quint64 newLfhOff = static_cast<quint64>(oldLfhOff32) + static_cast<quint64>(offsetAdj);
                    if (newLfhOff > 0xFFFFFFFF) {
                        // 4GB 초과 → 0xFFFFFFFF 마킹 (ZIP64 extra가 없으면 문제지만,
                        // Python zipfile allowZip64=True가 이미 ZIP64 extra를 생성)
                        cdSection[cdPos+42] = static_cast<char>(0xFF);
                        cdSection[cdPos+43] = static_cast<char>(0xFF);
                        cdSection[cdPos+44] = static_cast<char>(0xFF);
                        cdSection[cdPos+45] = static_cast<char>(0xFF);
                    } else {
                        quint32 nOff = static_cast<quint32>(newLfhOff);
                        cdSection[cdPos+42] = static_cast<char>(nOff & 0xFF);
                        cdSection[cdPos+43] = static_cast<char>((nOff >> 8) & 0xFF);
                        cdSection[cdPos+44] = static_cast<char>((nOff >> 16) & 0xFF);
                        cdSection[cdPos+45] = static_cast<char>((nOff >> 24) & 0xFF);
                    }
                }

                cdPos += 46 + nameLen + extraLen + commentLen;
            }

            // Adjust EOCD: central directory offset
            qint64 newCdStart = pngSize + lfhSize;
            if (newCdStart > 0xFFFFFFFF) {
                // 4GB 초과 → EOCD 32비트 필드는 0xFFFFFFFF (ZIP64 EOCD가 실제 값 보유)
                cdSection[cdEocdLocalPos + 16] = static_cast<char>(0xFF);
                cdSection[cdEocdLocalPos + 17] = static_cast<char>(0xFF);
                cdSection[cdEocdLocalPos + 18] = static_cast<char>(0xFF);
                cdSection[cdEocdLocalPos + 19] = static_cast<char>(0xFF);
            } else {
                quint32 newCdrOff32 = static_cast<quint32>(newCdStart);
                cdSection[cdEocdLocalPos + 16] = static_cast<char>(newCdrOff32 & 0xFF);
                cdSection[cdEocdLocalPos + 17] = static_cast<char>((newCdrOff32 >> 8) & 0xFF);
                cdSection[cdEocdLocalPos + 18] = static_cast<char>((newCdrOff32 >> 16) & 0xFF);
                cdSection[cdEocdLocalPos + 19] = static_cast<char>((newCdrOff32 >> 24) & 0xFF);
            }

            // Handle ZIP64 EOCD Locator and ZIP64 EOCD
            if (cdEocdLocalPos >= 20) {
                qint64 z64LocPos = cdEocdLocalPos - 20;
                if (static_cast<unsigned char>(cdSection[z64LocPos]) == 0x50 &&
                    static_cast<unsigned char>(cdSection[z64LocPos+1]) == 0x4b &&
                    static_cast<unsigned char>(cdSection[z64LocPos+2]) == 0x06 &&
                    static_cast<unsigned char>(cdSection[z64LocPos+3]) == 0x07) {

                    quint64 z64EocdOff = 0;
                    for (int i = 0; i < 8; i++)
                        z64EocdOff |= static_cast<quint64>(static_cast<unsigned char>(cdSection[z64LocPos + 8 + i])) << (i * 8);

                    qint64 z64EocdLocalPos = static_cast<qint64>(z64EocdOff) - cdrOffset64;

                    quint64 newZ64EocdOff = z64EocdOff + static_cast<quint64>(newCdStart) - static_cast<quint64>(cdrOffset64);
                    for (int i = 0; i < 8; i++)
                        cdSection[z64LocPos + 8 + i] = static_cast<char>((newZ64EocdOff >> (i * 8)) & 0xFF);

                    if (z64EocdLocalPos >= 0 && z64EocdLocalPos + 56 <= cdSection.size() &&
                        static_cast<unsigned char>(cdSection[z64EocdLocalPos]) == 0x50 &&
                        static_cast<unsigned char>(cdSection[z64EocdLocalPos+1]) == 0x4b &&
                        static_cast<unsigned char>(cdSection[z64EocdLocalPos+2]) == 0x06 &&
                        static_cast<unsigned char>(cdSection[z64EocdLocalPos+3]) == 0x06) {

                        quint64 newCdrOff64 = static_cast<quint64>(newCdStart);
                        for (int i = 0; i < 8; i++)
                            cdSection[z64EocdLocalPos + 48 + i] = static_cast<char>((newCdrOff64 >> (i * 8)) & 0xFF);
                    }
                }
            }

            if (cancelled()) { QFile::remove(zipPath); return; }

            // Write output: [PNG][LFH (streamed)][CD+EOCD (adjusted)]
            progress(50, "PNG+ZIP 결합 중...");

            QFile outputFile(outputPath);
            if (!outputFile.open(QIODevice::WriteOnly)) {
                log("출력 파일 생성 실패: " + outputPath, "error", "trad");
                QFile::remove(zipPath);
                progress(-1, ""); runJs("setTradBusy(false)");
                return;
            }

            qint64 totalBytes = pngSize + zipFileSize;
            qint64 written = 0;

            // 1) 완전한 PNG (IEND 포함)
            outputFile.write(pngData);
            written += pngSize;
            pngData.clear(); // 메모리 해제
            progress(55, QString("결합 중... %1").arg(formatSize(written)));

            // 2) ZIP LFH section (streaming)
            {
                QFile zipLfhFile(zipPath);
                if (!zipLfhFile.open(QIODevice::ReadOnly)) {
                    log("ZIP LFH 재오픈 실패", "error", "pixiv");
                    outputFile.close();
                    QFile::remove(outputPath);
                    return;
                }
                const int WCHUNK = 8 * 1024 * 1024; // 8MB
                qint64 lfhWritten = 0;
                while (lfhWritten < lfhSize) {
                    if (m_tradCancelled.load()) {
                        zipLfhFile.close(); outputFile.close();
                        QFile::remove(outputPath); QFile::remove(zipPath);
                        cancelled(); return;
                    }
                    qint64 toRead = qMin(static_cast<qint64>(WCHUNK), lfhSize - lfhWritten);
                    QByteArray chunk = zipLfhFile.read(toRead);
                    outputFile.write(chunk);
                    lfhWritten += chunk.size();
                    written += chunk.size();
                    int pct = 55 + static_cast<int>((written * 35) / totalBytes);
                    if (pct > 90) pct = 90;
                    progress(pct, QString("결합 중... %1 / %2").arg(formatSize(written), formatSize(totalBytes)));
                }
                zipLfhFile.close();
            }

            // 3) CD section + EOCD (adjusted offsets)
            outputFile.write(cdSection);
            written += cdSection.size();
            progress(90, QString("저장 중... %1").arg(formatSize(written)));

            outputFile.close();

            QFile::remove(zipPath);
            if (needsCleanup) QFile::remove(actualCoverPath);

            log("PNG+ZIP 폴리글롯 — 파일 첨부로 업로드 시 원본 그대로 보존", "info", "trad");

            // ★ Cross-OS 호환: .zip 복사본도 같이 만듬 — Windows/Linux 사용자가 그냥 추출 가능.
            //   같은 바이트 (polyglot이 valid ZIP 이라서) → hard link 로 디스크 추가 사용 0.
            //   1TB 파일도 hard link 면 디스크 추가 0 (같은 inode 가리킴).
            //   hard link 실패 시 (cross-device / NAS 등) full copy 로 fallback.
            //   Mac → .png (Quicklook), Windows → .zip (탐색기), Linux → .zip (Archive Manager)
            {
                QString zipAltPath = outputPath;
                if (zipAltPath.endsWith(".png", Qt::CaseInsensitive)) {
                    zipAltPath = zipAltPath.left(zipAltPath.length() - 4) + ".zip";
                } else {
                    zipAltPath = zipAltPath + ".zip";
                }
                QFile::remove(zipAltPath);  // 기존 거 지움
                bool linkOk = false;
#ifdef Q_OS_WIN
                // Windows: mklink /H — cmd 내장 (hard link, NTFS 필요)
                QProcess lnProc;
                lnProc.start("cmd.exe", {"/c", "mklink", "/H",
                    QDir::toNativeSeparators(zipAltPath),
                    QDir::toNativeSeparators(outputPath)});
                if (lnProc.waitForFinished(5000) && lnProc.exitCode() == 0) {
                    linkOk = true;
                    log(QString(".zip 복사본 (hard link, NTFS): %1 — 디스크 추가 0").arg(QFileInfo(zipAltPath).fileName()),
                        "info", "trad");
                }
#else
                // macOS / Linux: ln (POSIX hard link)
                QProcess lnProc;
                lnProc.start("/bin/ln", {outputPath, zipAltPath});
                if (lnProc.waitForFinished(5000) && lnProc.exitCode() == 0) {
                    linkOk = true;
                    log(QString(".zip 복사본 (hard link): %1 — 디스크 추가 0").arg(QFileInfo(zipAltPath).fileName()),
                        "info", "trad");
                }
#endif
                if (!linkOk) {
                    // hard link 실패 (cross-device / NAS / FAT32) → 1TB 같으면 디스크 부담 큼
                    QFileInfo sizeInfo(outputPath);
                    if (sizeInfo.size() > qint64(10) * 1024 * 1024 * 1024) {  // 10GB+
                        log(QString(".zip hard link 실패 — copy 는 %1 추가 디스크 필요. 사용자가 .png → .zip 으로 rename 권장")
                            .arg(formatSize(sizeInfo.size())), "warning", "trad");
                    } else {
                        if (QFile::copy(outputPath, zipAltPath)) {
                            log(QString(".zip 복사본 (copy): %1 — Windows/Linux 호환").arg(QFileInfo(zipAltPath).fileName()),
                                "info", "trad");
                        }
                    }
                }
            }
        }

        // Step 5: Set custom file icon (platform-specific)
        progress(92, "아이콘 설정 중...");
#ifdef Q_OS_MACOS
        {
            // macOS: osascript로 Finder 아이콘 설정
            QString iconSourcePath = m_config->tradCoverPath();
            QString tempIconPath;
            bool iconCleanup = false;

            if (iconSourcePath.isEmpty() || !QFile::exists(iconSourcePath)) {
                tempIconPath = tempDir + "/trad_icon_temp.png";
                QFile::remove(tempIconPath);
                QFile::copy(":/trad_cover.png", tempIconPath);
                QFile::setPermissions(tempIconPath, QFileDevice::ReadUser | QFileDevice::WriteUser);
                iconSourcePath = tempIconPath;
                iconCleanup = true;
            }

            // JPEG→PNG for icon if needed (sips: macOS only)
            if (iconSourcePath.toLower().endsWith(".jpg") || iconSourcePath.toLower().endsWith(".jpeg")) {
                QString convertedIcon = tempDir + "/trad_icon_converted.png";
                QProcess sips;
                sips.start("/usr/bin/sips", {"-s", "format", "png", iconSourcePath, "--out", convertedIcon});
                if (sips.waitForFinished(30000) && sips.exitCode() == 0) {
                    if (iconCleanup) QFile::remove(iconSourcePath);
                    iconSourcePath = convertedIcon;
                    iconCleanup = true;
                }
            }

            QString escapedIcon = iconSourcePath;
            escapedIcon.replace("\\", "\\\\").replace("\"", "\\\"");
            QString escapedOut = outputPath;
            escapedOut.replace("\\", "\\\\").replace("\"", "\\\"");

            QString appleScript = QString(
                "use framework \"AppKit\"\n"
                "set img to current application's NSImage's alloc()'s initWithContentsOfFile:\"%1\"\n"
                "if img is not missing value then\n"
                "    set ws to current application's NSWorkspace's sharedWorkspace()\n"
                "    ws's setIcon:img forFile:\"%2\" options:0\n"
                "end if"
            ).arg(escapedIcon, escapedOut);

            QProcess iconProc;
            iconProc.start("/usr/bin/osascript", {"-e", appleScript});
            if (iconProc.waitForFinished(30000) && iconProc.exitCode() == 0) {
                log("Finder 아이콘 설정 완료", "info", "trad");
            } else {
                log("Finder 아이콘 설정 실패 (무시)", "warning", "trad");
            }
            if (iconCleanup) QFile::remove(iconSourcePath);
        }
#endif

        // Verify using bundled Python zipfile (리스트만 — 추출하지 않음, 1TB도 안전)
        {
            QString vBundledPy = Common::bundledPythonPath();
            QString vPyCmd = QFile::exists(vBundledPy) ? vBundledPy : "python3";
            QString verifyScript =
                "import zipfile, sys\n"
                "try:\n"
                "    with zipfile.ZipFile(sys.argv[1]) as zf:\n"
                "        nl = zf.namelist()\n"
                "        bad = zf.testzip()\n"
                "        if bad:\n"
                "            print(f'BAD {bad}')\n"
                "        else:\n"
                "            total = sum(i.file_size for i in zf.infolist())\n"
                "            print(f'OK {len(nl)} {total}')\n"
                "except Exception as e:\n"
                "    print(f'ERR {e}')\n";
            QProcess verifyProc;
            verifyProc.start(vPyCmd, {"-c", verifyScript, outputPath});
            if (verifyProc.waitForFinished(-1)) {
                QString vout = QString::fromUtf8(verifyProc.readAllStandardOutput()).trimmed();
                if (vout.startsWith("OK ")) {
                    QStringList parts = vout.mid(3).split(' ');
                    int vcnt = parts.value(0).toInt();
                    qint64 vtotal = parts.value(1).toLongLong();
                    log(QString("검증 OK (%1개 파일, 원본 %2)").arg(vcnt).arg(formatSize(vtotal)), "info", "trad");
                } else if (vout.startsWith("BAD ")) {
                    log("경고: ZIP 검증 실패 — 손상된 파일: " + vout.mid(4), "warning", "trad");
                } else {
                    log("경고: ZIP 검증 실패 — " + vout, "warning", "trad");
                }
            } else {
                log("경고: ZIP 검증 프로세스 실패!", "warning", "trad");
            }
        }

        QFileInfo outInfo(outputPath);
        QString sizeStr = formatSize(outInfo.size());

        // 자동 압축 임시파일 정리
        for (const QString &tmp : autoCompressedTempFiles) {
            QFile::remove(tmp);
        }
        // 임시 폴더 정리 (.trad_tmp)
        QDir(tempDir).removeRecursively();

        FileHelper::setDownloadMeta(outputPath, "ABIWA trad");

        // ★ WebDAV 자동 업로드 (활성화 시)
        QMetaObject::invokeMethod(this, [this, outputPath]() {
            enqueueWebDavUpload(outputPath);
            enqueueBackup(outputPath);
        }, Qt::QueuedConnection);

        progress(100, "완료!");
        log(QString("완료! %1 (%2)").arg(QFileInfo(outputPath).fileName(), sizeStr), "success", "trad");
        log(QString("경로: %1").arg(outputPath), "info", "trad");
        log(QString("파일 %1개 → %2").arg(files.size()).arg(QFileInfo(outputPath).fileName()), "info", "trad");
        if (outputPath.endsWith(".png")) {
            log("PNG+ZIP 폴리글롯 — 웹/SNS에 파일로 업로드하면 변형 없음", "info", "trad");
            log("추출: unzip 파일.png -d 폴더/", "info", "trad");
        } else {
            log("ZIP 포맷 — 웹/클라우드에 업로드해도 변형 없음", "info", "trad");
            log("추출: unzip 파일.zip -d 폴더/", "info", "trad");
        }
        runJs("setTradBusy(false)");
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void MiyoBackend::extractTrad(const QString &configJson)
{
    QJsonObject config = QJsonDocument::fromJson(configJson.toUtf8()).object();

    // Select trad file to extract (PNG with embedded ZIP)
    QString pngPath = QFileDialog::getOpenFileName(m_window, "추출할 trad 파일 선택", QString(), "Trad Files (*.png)");
    if (pngPath.isEmpty()) {
        runJs("setTradProgress(-1, '')");
        runJs("setTradBusy(false)");
        return;
    }

    // 추출 경로 선택 다이얼로그
    QString defaultDir = config["path"].toString();
    defaultDir.replace("~", QDir::homePath());
    QString outputDir = QFileDialog::getExistingDirectory(m_window, "추출할 폴더 선택", defaultDir);
    if (outputDir.isEmpty()) {
        runJs("setTradProgress(-1, '')");
        runJs("setTradBusy(false)");
        return;
    }

    QThread *thread = QThread::create([this, pngPath, outputDir]() {
        auto progress = [this](int pct, const QString &text) {
            runJs(QString("setTradProgress(%1, '%2')").arg(pct).arg(text));
        };

        progress(10, "파일 확인 중...");
        log("trad 파일 추출 중...", "info", "trad");
        log("파일: " + pngPath, "info", "trad");

        QFileInfo pngInfo(pngPath);
        qint64 fileSize = pngInfo.size();
        QString sizeStr = fileSize < 1024*1024
            ? QString::number(fileSize/1024.0, 'f', 1) + " KB"
            : fileSize < qint64(1024)*1024*1024
                ? QString::number(fileSize/1024.0/1024.0, 'f', 1) + " MB"
                : QString::number(fileSize/1024.0/1024.0/1024.0, 'f', 2) + " GB";
        log(QString("파일 크기: %1").arg(sizeStr), "info", "trad");

        // Detect format: check first 4 bytes
        // PK\x03\x04 = ZIP-only format (new format)
        // \x89PNG = PNG+ZIP polyglot (old format)
        QFile f(pngPath);
        if (!f.open(QIODevice::ReadOnly)) {
            log("파일 열기 실패", "error", "trad");
            progress(-1, ""); runJs("setTradBusy(false)");
            return;
        }
        QByteArray header = f.read(4);
        f.close();

        bool isZipOnly = (header.size() >= 4 &&
            static_cast<unsigned char>(header[0]) == 0x50 &&
            static_cast<unsigned char>(header[1]) == 0x4b &&
            static_cast<unsigned char>(header[2]) == 0x03 &&
            static_cast<unsigned char>(header[3]) == 0x04);

        if (isZipOnly) {
            log("ZIP-only 포맷 감지", "info", "trad");
        } else {
            log("PNG+ZIP polyglot 포맷 감지", "info", "trad");
        }

        progress(30, "압축 해제 중...");
        QDir().mkpath(outputDir);

        // Helper lambda: try multiple unzip methods on a given zip file path
        // 번들된 Python 경로 (trad 추출에서도 사용)
        QString bundledPy = Common::bundledPythonPath();
        QString pyCmd = QFile::exists(bundledPy) ? bundledPy : "python3";

        auto tryExtract = [this, pyCmd](const QString &zipFilePath, const QString &destDir) -> QPair<bool, int> {
            auto countFiles = [](const QString &dir) -> int {
                QDirIterator it(dir, QDir::Files, QDirIterator::Subdirectories);
                int cnt = 0;
                while (it.hasNext()) { it.next(); cnt++; }
                return cnt;
            };

            // 번들 Python zipfile로 추출 (시스템 도구 의존 없음)
            {
                log("추출 시도: python3 zipfile...", "info", "trad");
                QString extractScript =
                    "import zipfile, sys\n"
                    "try:\n"
                    "    with zipfile.ZipFile(sys.argv[1]) as zf:\n"
                    "        zf.extractall(sys.argv[2])\n"
                    "except Exception as e:\n"
                    "    print(f'ERROR: {e}', file=sys.stderr)\n"
                    "    sys.exit(1)\n";
                QProcess py;
                py.start(pyCmd, {"-c", extractScript, zipFilePath, destDir});
                if (py.waitForFinished(-1) && py.exitCode() == 0) {
                    int cnt = countFiles(destDir);
                    if (cnt > 0) {
                        log(QString("추출 성공 (%1개 파일)").arg(cnt), "success", "trad");
                        return {true, cnt};
                    }
                    log("추출: 성공했으나 파일 없음", "warning", "trad");
                } else {
                    QString pyErr = QString::fromUtf8(py.readAllStandardError()).trimmed().left(300);
                    log("추출 실패: " + pyErr, "warning", "trad");
                }
            }
            return {false, 0};
        };

        // ━━ 매니페스트 검증 헬퍼 ━━
        // 추출된 __TRAD_MANIFEST__.json 을 읽고 각 파일의 크기/SHA1 검증
        auto verifyManifest = [this](const QString &destDir) -> void {
            QString mfPath = destDir + "/__TRAD_MANIFEST__.json";
            QFile mfFile(mfPath);
            if (!mfFile.exists() || !mfFile.open(QIODevice::ReadOnly)) {
                // 매니페스트가 없는 구형 trad 파일도 있으므로 조용히 무시
                runJs("clearTradManifest()");
                return;
            }
            QByteArray mfBytes = mfFile.readAll();
            mfFile.close();

            QJsonParseError pe;
            QJsonDocument doc = QJsonDocument::fromJson(mfBytes, &pe);
            if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
                log("매니페스트 파싱 실패: " + pe.errorString(), "warning", "trad");
                runJs("clearTradManifest()");
                return;
            }
            QJsonObject manifest = doc.object();
            QJsonArray files = manifest["files"].toArray();

            log(QString("매니페스트 발견 — %1개 파일 검증 중").arg(files.size()), "info", "trad");

            // 각 파일 검증
            int verifiedCnt = 0;
            int failedCnt = 0;
            QJsonArray verifiedFiles;
            for (int i = 0; i < files.size(); ++i) {
                QJsonObject f = files[i].toObject();
                QString name = f["name"].toString();
                qint64 expectedSize = static_cast<qint64>(f["size"].toDouble());
                QString expectedSha = f["sha1"].toString();

                QString actualPath = destDir + "/" + name;
                QFileInfo fi(actualPath);
                bool exists = fi.exists();
                bool sizeOk = exists && fi.size() == expectedSize;
                bool shaOk = true;

                if (sizeOk && !expectedSha.isEmpty()) {
                    QFile af(actualPath);
                    if (af.open(QIODevice::ReadOnly)) {
                        QCryptographicHash hasher(QCryptographicHash::Sha1);
                        qint64 fsz = af.size();
                        const qint64 SAMPLE = 1024 * 1024;
                        if (fsz <= SAMPLE * 2) {
                            hasher.addData(af.readAll());
                        } else {
                            hasher.addData(af.read(SAMPLE));
                            af.seek(fsz - SAMPLE);
                            hasher.addData(af.read(SAMPLE));
                        }
                        af.close();
                        QString actualSha = QString::fromLatin1(hasher.result().toHex());
                        shaOk = (actualSha == expectedSha);
                    } else {
                        shaOk = false;
                    }
                }

                bool ok = exists && sizeOk && shaOk;
                f["verified"] = ok;
                f["exists"] = exists;
                if (ok) verifiedCnt++; else failedCnt++;
                verifiedFiles.append(f);

                if (!ok) {
                    log(QString("  ✗ %1 — %2").arg(
                        name,
                        !exists ? "없음" : !sizeOk ? "크기 불일치" : "SHA1 불일치"),
                        "warning", "trad");
                }
            }
            manifest["files"] = verifiedFiles;
            manifest["verified_ok"] = verifiedCnt;
            manifest["verified_failed"] = failedCnt;

            if (failedCnt > 0) {
                log(QString("⚠ 무결성 검사: %1개 손상/누락").arg(failedCnt), "error", "trad");
            } else {
                log(QString("✓ 무결성 검사 완료 (%1/%1)").arg(verifiedCnt), "success", "trad");
            }

            // 검증된 매니페스트를 UI 에 전송
            QString b64 = QString::fromUtf8(
                QJsonDocument(manifest).toJson(QJsonDocument::Compact).toBase64());
            runJs(QString("setTradManifest(JSON.parse(b64toUtf8('%1')))").arg(b64));

            // 매니페스트 파일은 추출 후 삭제 (사용자에게 보이지 않게)
            QFile::remove(mfPath);
        };

        if (isZipOnly) {
            // ZIP-only: extract directly
            auto [ok, count] = tryExtract(pngPath, outputDir);
            if (ok) {
                progress(100, "완료!");
                log(QString("추출 완료! %1개 파일 → %2").arg(count).arg(outputDir), "success", "trad");
                verifyManifest(outputDir);
                runJs("setTradBusy(false)");
                return;
            }

            // Fallback: maybe this is a polyglot file that starts with PK inside PNG data
            // Try scanning for alternative ZIP structures
            log("직접 추출 실패, ZIP 구조 탐색 중...", "warning", "trad");
            progress(50, "ZIP 구조 탐색 중...");

            QFile checkFile(pngPath);
            if (checkFile.open(QIODevice::ReadOnly)) {
                // Check if there's an EOCD signature at the end
                qint64 checkSize = checkFile.size();
                qint64 checkStart = qMax(qint64(0), checkSize - 65557);
                checkFile.seek(checkStart);
                QByteArray checkTail = checkFile.read(checkSize - checkStart);
                bool hasEocd = false;
                for (qint64 ci = checkTail.size() - 22; ci >= 0; --ci) {
                    if (static_cast<unsigned char>(checkTail[ci]) == 0x50 &&
                        static_cast<unsigned char>(checkTail[ci+1]) == 0x4b &&
                        static_cast<unsigned char>(checkTail[ci+2]) == 0x05 &&
                        static_cast<unsigned char>(checkTail[ci+3]) == 0x06) {
                        hasEocd = true;
                        break;
                    }
                }
                checkFile.close();

                if (hasEocd) {
                    // Has EOCD but extraction failed → offsets may need fixing
                    // Try python3 with offset-aware extraction
                    log("EOCD 발견, 오프셋 복원 시도 중...", "info", "trad");
                    QString pyScript = QString(
                        "import zipfile, sys\n"
                        "try:\n"
                        "    with zipfile.ZipFile('%1') as z:\n"
                        "        z.extractall('%2')\n"
                        "        print(len(z.namelist()))\n"
                        "except Exception as e:\n"
                        "    print(f'ERROR: {e}', file=sys.stderr)\n"
                        "    sys.exit(1)\n"
                    ).arg(pngPath, outputDir);
                    QProcess py2;
                    py2.start(pyCmd, {"-c", pyScript});
                    if (py2.waitForFinished(-1) && py2.exitCode() == 0) {
                        QString out = QString::fromUtf8(py2.readAllStandardOutput()).trimmed();
                        int cnt2 = out.toInt();
                        if (cnt2 > 0) {
                            progress(100, "완료!");
                            log(QString("추출 완료! %1개 파일 → %2").arg(cnt2).arg(outputDir), "success", "trad");
                            verifyManifest(outputDir);
                            runJs("setTradBusy(false)");
                            return;
                        }
                    } else {
                        QString pyErr = QString::fromUtf8(py2.readAllStandardError()).trimmed().left(300);
                        log("python3 직접 추출도 실패: " + pyErr, "warning", "trad");
                    }
                }
            }

            log("추출 실패 - 파일이 손상되었거나 trad 파일이 아닙니다", "error", "trad");
        } else {
            // Check if it's actually a PNG with embedded ZIP (not just a regular PNG)
            bool hasZipData = false;
            {
                QFile checkFile(pngPath);
                if (checkFile.open(QIODevice::ReadOnly)) {
                    // Quick check: scan last 65KB for EOCD signature
                    qint64 checkSize = checkFile.size();
                    qint64 checkStart = qMax(qint64(0), checkSize - 65557);
                    checkFile.seek(checkStart);
                    QByteArray checkTail = checkFile.read(checkSize - checkStart);
                    for (qint64 ci = checkTail.size() - 22; ci >= 0; --ci) {
                        if (static_cast<unsigned char>(checkTail[ci]) == 0x50 &&
                            static_cast<unsigned char>(checkTail[ci+1]) == 0x4b &&
                            static_cast<unsigned char>(checkTail[ci+2]) == 0x05 &&
                            static_cast<unsigned char>(checkTail[ci+3]) == 0x06) {
                            hasZipData = true;
                            break;
                        }
                    }
                    checkFile.close();
                }
            }

            if (!hasZipData) {
                // EOCD가 없음 — 디스코드/SNS에서 IEND 이후가 잘린 경우
                // tRAd PNG 청크에서 ZIP LFH 데이터 복구 시도
                log("EOCD 없음 — tRAd 청크에서 복구 시도", "info", "trad");
                progress(40, "tRAd 청크 검색 중...");

                QFile tradFile(pngPath);
                if (!tradFile.open(QIODevice::ReadOnly)) {
                    log("파일 열기 실패", "error", "trad");
                    progress(-1, ""); runJs("setTradBusy(false)");
                    return;
                }
                QByteArray allData = tradFile.readAll();
                tradFile.close();

                // Search for tRAd chunk: scan for "tRAd" chunk type
                qint64 tRadPos = -1;
                for (qint64 i = 8; i < allData.size() - 8; ++i) {
                    if (allData[i+4] == 't' && allData[i+5] == 'R' &&
                        allData[i+6] == 'A' && allData[i+7] == 'd') {
                        // Read chunk length (4 bytes big-endian at position i)
                        quint32 chunkLen = (static_cast<unsigned char>(allData[i]) << 24) |
                                           (static_cast<unsigned char>(allData[i+1]) << 16) |
                                           (static_cast<unsigned char>(allData[i+2]) << 8) |
                                           static_cast<unsigned char>(allData[i+3]);
                        if (chunkLen > 0 && i + 8 + chunkLen <= allData.size()) {
                            tRadPos = i;
                            break;
                        }
                    }
                }

                if (tRadPos < 0) {
                    log("이 파일은 trad 파일이 아닙니다 (ZIP 데이터 없음)", "error", "trad");
                    progress(-1, ""); runJs("setTradBusy(false)");
                    return;
                }

                // Extract LFH data from tRAd chunk
                quint32 tRadDataLen = (static_cast<unsigned char>(allData[tRadPos]) << 24) |
                                      (static_cast<unsigned char>(allData[tRadPos+1]) << 16) |
                                      (static_cast<unsigned char>(allData[tRadPos+2]) << 8) |
                                      static_cast<unsigned char>(allData[tRadPos+3]);
                QByteArray lfhData = allData.mid(static_cast<int>(tRadPos + 8), static_cast<int>(tRadDataLen));

                log(QString("tRAd 청크 발견 (%1)").arg(
                    tRadDataLen < 1024*1024 ? QString::number(tRadDataLen/1024.0, 'f', 1) + " KB"
                                            : QString::number(tRadDataLen/1024.0/1024.0, 'f', 1) + " MB"),
                    "success", "trad");

                progress(50, "ZIP 재구축 중...");

                // Rebuild a valid ZIP from LFH data:
                // Walk LFH entries, build central directory, append EOCD
                QByteArray centralDir;
                quint16 fileCount = 0;
                qint64 lfhPos = 0;

                while (lfhPos + 30 <= lfhData.size()) {
                    // Check LFH signature PK\x03\x04
                    if (static_cast<unsigned char>(lfhData[lfhPos]) != 0x50 ||
                        static_cast<unsigned char>(lfhData[lfhPos+1]) != 0x4b ||
                        static_cast<unsigned char>(lfhData[lfhPos+2]) != 0x03 ||
                        static_cast<unsigned char>(lfhData[lfhPos+3]) != 0x04) break;

                    // Read LFH fields
                    quint16 versionNeeded = static_cast<quint16>(
                        static_cast<unsigned char>(lfhData[lfhPos+4]) | (static_cast<unsigned char>(lfhData[lfhPos+5]) << 8));
                    quint16 flags = static_cast<quint16>(
                        static_cast<unsigned char>(lfhData[lfhPos+6]) | (static_cast<unsigned char>(lfhData[lfhPos+7]) << 8));
                    quint16 method = static_cast<quint16>(
                        static_cast<unsigned char>(lfhData[lfhPos+8]) | (static_cast<unsigned char>(lfhData[lfhPos+9]) << 8));
                    quint16 modTime = static_cast<quint16>(
                        static_cast<unsigned char>(lfhData[lfhPos+10]) | (static_cast<unsigned char>(lfhData[lfhPos+11]) << 8));
                    quint16 modDate = static_cast<quint16>(
                        static_cast<unsigned char>(lfhData[lfhPos+12]) | (static_cast<unsigned char>(lfhData[lfhPos+13]) << 8));
                    quint32 crc32val = 0;
                    for (int bi = 0; bi < 4; bi++)
                        crc32val |= static_cast<quint32>(static_cast<unsigned char>(lfhData[lfhPos+14+bi])) << (bi*8);
                    quint32 compSize = 0;
                    for (int bi = 0; bi < 4; bi++)
                        compSize |= static_cast<quint32>(static_cast<unsigned char>(lfhData[lfhPos+18+bi])) << (bi*8);
                    quint32 uncompSize = 0;
                    for (int bi = 0; bi < 4; bi++)
                        uncompSize |= static_cast<quint32>(static_cast<unsigned char>(lfhData[lfhPos+22+bi])) << (bi*8);
                    quint16 nameLen = static_cast<quint16>(
                        static_cast<unsigned char>(lfhData[lfhPos+26]) | (static_cast<unsigned char>(lfhData[lfhPos+27]) << 8));
                    quint16 extraLen = static_cast<quint16>(
                        static_cast<unsigned char>(lfhData[lfhPos+28]) | (static_cast<unsigned char>(lfhData[lfhPos+29]) << 8));

                    QByteArray fileName = lfhData.mid(static_cast<int>(lfhPos + 30), nameLen);

                    // Build central directory entry (46 + nameLen bytes)
                    QByteArray cdEntry(46 + nameLen, 0);
                    cdEntry[0] = 0x50; cdEntry[1] = 0x4b; cdEntry[2] = 0x01; cdEntry[3] = 0x02; // CD signature
                    cdEntry[4] = 0x14; cdEntry[5] = 0x03; // version made by (Unix, 2.0)
                    cdEntry[6] = versionNeeded & 0xFF; cdEntry[7] = (versionNeeded >> 8) & 0xFF;
                    cdEntry[8] = flags & 0xFF; cdEntry[9] = (flags >> 8) & 0xFF;
                    cdEntry[10] = method & 0xFF; cdEntry[11] = (method >> 8) & 0xFF;
                    cdEntry[12] = modTime & 0xFF; cdEntry[13] = (modTime >> 8) & 0xFF;
                    cdEntry[14] = modDate & 0xFF; cdEntry[15] = (modDate >> 8) & 0xFF;
                    for (int bi = 0; bi < 4; bi++) cdEntry[16+bi] = (crc32val >> (bi*8)) & 0xFF;
                    for (int bi = 0; bi < 4; bi++) cdEntry[20+bi] = (compSize >> (bi*8)) & 0xFF;
                    for (int bi = 0; bi < 4; bi++) cdEntry[24+bi] = (uncompSize >> (bi*8)) & 0xFF;
                    cdEntry[28] = nameLen & 0xFF; cdEntry[29] = (nameLen >> 8) & 0xFF;
                    // extra, comment, disk, internal attr, external attr = 0
                    // local header offset (at byte 42)
                    quint32 lfhOffset = static_cast<quint32>(lfhPos);
                    for (int bi = 0; bi < 4; bi++) cdEntry[42+bi] = (lfhOffset >> (bi*8)) & 0xFF;
                    // copy filename
                    for (int bi = 0; bi < nameLen; bi++) cdEntry[46+bi] = fileName[bi];

                    centralDir.append(cdEntry);
                    fileCount++;

                    // Advance to next LFH
                    lfhPos += 30 + nameLen + extraLen + compSize;
                }

                // Build EOCD (22 bytes)
                quint32 cdSize = static_cast<quint32>(centralDir.size());
                quint32 cdOffset = static_cast<quint32>(lfhData.size());
                QByteArray eocd(22, 0);
                eocd[0] = 0x50; eocd[1] = 0x4b; eocd[2] = 0x05; eocd[3] = 0x06;
                eocd[8] = fileCount & 0xFF; eocd[9] = (fileCount >> 8) & 0xFF;
                eocd[10] = fileCount & 0xFF; eocd[11] = (fileCount >> 8) & 0xFF;
                for (int bi = 0; bi < 4; bi++) eocd[12+bi] = (cdSize >> (bi*8)) & 0xFF;
                for (int bi = 0; bi < 4; bi++) eocd[16+bi] = (cdOffset >> (bi*8)) & 0xFF;

                // Write reconstructed ZIP to temp file
                QString tempZip = outputDir + "/.trad_extract_temp.zip";
                QFile::remove(tempZip);
                {
                    QFile tz(tempZip);
                    if (tz.open(QIODevice::WriteOnly)) {
                        tz.write(lfhData);
                        tz.write(centralDir);
                        tz.write(eocd);
                        tz.close();
                    }
                }

                log(QString("ZIP 재구축 완료 (%1개 파일)").arg(fileCount), "success", "trad");
                progress(70, "압축 해제 중...");

                auto [ok, count] = tryExtract(tempZip, outputDir);
                QFile::remove(tempZip);

                if (ok && count > 0) {
                    progress(100, "완료!");
                    log(QString("tRAd 청크에서 복구 성공! %1개 파일 → %2").arg(count).arg(outputDir), "success", "trad");
                    verifyManifest(outputDir);
                    runJs("setTradBusy(false)");
                    return;
                }

                log("tRAd 청크 복구 실패 — 파일이 손상되었습니다", "error", "trad");
                progress(-1, ""); runJs("setTradBusy(false)");
                return;
            }

            // PNG+ZIP polyglot: try direct extract first
            {
                auto [ok, count] = tryExtract(pngPath, outputDir);
                if (ok && count > 0) {
                    progress(100, "완료!");
                    log(QString("추출 완료! %1개 파일 → %2").arg(count).arg(outputDir), "success", "trad");
                    verifyManifest(outputDir);
                    runJs("setTradBusy(false)");
                    return;
                }
                log("직접 추출로 파일을 가져오지 못함, ZIP 분리 추출로 전환", "info", "trad");
            }

            // Fallback: find ZIP data inside PNG and extract separately
            log("직접 추출 실패, ZIP 데이터 위치 탐색 중...", "warning", "trad");
            progress(40, "ZIP 위치 탐색 중...");

            QFile srcFile(pngPath);
            if (!srcFile.open(QIODevice::ReadOnly)) {
                log("파일 열기 실패", "error", "trad");
                progress(-1, ""); runJs("setTradBusy(false)");
                return;
            }

            // Search for PK\x03\x04 after first 8 bytes (skip PNG magic)
            qint64 zipStart = -1;
            const qint64 SEARCH_CHUNK = 1024 * 1024;
            QByteArray prevTail;

            srcFile.seek(8);
            while (!srcFile.atEnd()) {
                QByteArray chunk = srcFile.read(SEARCH_CHUNK);
                QByteArray searchBuf = prevTail + chunk;
                for (qint64 i = 0; i < searchBuf.size() - 3; ++i) {
                    if (static_cast<unsigned char>(searchBuf[i]) == 0x50 &&
                        static_cast<unsigned char>(searchBuf[i+1]) == 0x4b &&
                        static_cast<unsigned char>(searchBuf[i+2]) == 0x03 &&
                        static_cast<unsigned char>(searchBuf[i+3]) == 0x04) {
                        zipStart = srcFile.pos() - chunk.size() - prevTail.size() + i;
                        break;
                    }
                }
                if (zipStart >= 0) break;
                prevTail = chunk.right(3);
            }
            srcFile.close();

            if (zipStart < 0) {
                log("ZIP 데이터를 찾을 수 없습니다", "error", "trad");
                progress(-1, ""); runJs("setTradBusy(false)");
                return;
            }

            log(QString("ZIP 데이터 발견 (offset: %1)").arg(zipStart), "info", "trad");
            progress(50, "ZIP 데이터 추출 중...");

            // Use output dir for temp ZIP to avoid filling system disk with large files
            QString tempZip = outputDir + "/.trad_extract_temp.zip";
            QFile::remove(tempZip);

            QFile src(pngPath);
            QFile dst(tempZip);
            if (!src.open(QIODevice::ReadOnly) || !dst.open(QIODevice::WriteOnly)) {
                log("임시 파일 생성 실패", "error", "trad");
                progress(-1, ""); runJs("setTradBusy(false)");
                return;
            }

            src.seek(zipStart);
            qint64 remaining = fileSize - zipStart;
            const int BUF = 65536;
            while (remaining > 0) {
                QByteArray buf = src.read(qMin(qint64(BUF), remaining));
                if (buf.isEmpty()) break;
                dst.write(buf);
                remaining -= buf.size();
            }
            src.close();
            dst.close();

            // Reverse offset adjustments: the polyglot creation added
            // zipStart to all offsets, so we need to subtract it back
            progress(60, "ZIP 오프셋 복원 중...");
            {
                QFile fixZip(tempZip);
                if (fixZip.open(QIODevice::ReadWrite)) {
                    qint64 fixZipLen = fixZip.size();

                    // Find EOCD in extracted ZIP
                    qint64 fixEocdPos = -1;
                    qint64 fixSearchStart = qMax(qint64(0), fixZipLen - 65557);
                    fixZip.seek(fixSearchStart);
                    QByteArray fixTail = fixZip.read(fixZipLen - fixSearchStart);
                    for (qint64 i = fixTail.size() - 22; i >= 0; --i) {
                        if (static_cast<unsigned char>(fixTail[i]) == 0x50 &&
                            static_cast<unsigned char>(fixTail[i+1]) == 0x4b &&
                            static_cast<unsigned char>(fixTail[i+2]) == 0x05 &&
                            static_cast<unsigned char>(fixTail[i+3]) == 0x06) {
                            fixEocdPos = fixSearchStart + i;
                            break;
                        }
                    }

                    if (fixEocdPos >= 0) {
                        // Read current central directory offset (32-bit)
                        fixZip.seek(fixEocdPos + 16);
                        QByteArray fixCdrBytes = fixZip.read(4);
                        quint32 fixCdrOffset32 = static_cast<quint32>(
                            static_cast<unsigned char>(fixCdrBytes[0]) |
                            (static_cast<unsigned char>(fixCdrBytes[1]) << 8) |
                            (static_cast<unsigned char>(fixCdrBytes[2]) << 16) |
                            (static_cast<unsigned char>(fixCdrBytes[3]) << 24));

                        // ZIP64: 0xFFFFFFFF이면 ZIP64 EOCD에서 실제 오프셋 읽기
                        qint64 fixCdrOffset = static_cast<qint64>(fixCdrOffset32);
                        if (fixCdrOffset32 == 0xFFFFFFFF && fixEocdPos >= 20) {
                            fixZip.seek(fixEocdPos - 20);
                            QByteArray fLocSig = fixZip.read(4);
                            if (fLocSig.size() == 4 &&
                                static_cast<unsigned char>(fLocSig[0]) == 0x50 &&
                                static_cast<unsigned char>(fLocSig[1]) == 0x4b &&
                                static_cast<unsigned char>(fLocSig[2]) == 0x06 &&
                                static_cast<unsigned char>(fLocSig[3]) == 0x07) {
                                fixZip.seek(fixEocdPos - 20 + 8);
                                QByteArray fLocOff = fixZip.read(8);
                                quint64 fZ64EocdOff = 0;
                                for (int i = 0; i < 8; i++)
                                    fZ64EocdOff |= static_cast<quint64>(static_cast<unsigned char>(fLocOff[i])) << (i * 8);
                                fixZip.seek(static_cast<qint64>(fZ64EocdOff));
                                QByteArray fZ64Eocd = fixZip.read(56);
                                if (fZ64Eocd.size() >= 56 &&
                                    static_cast<unsigned char>(fZ64Eocd[0]) == 0x50 &&
                                    static_cast<unsigned char>(fZ64Eocd[1]) == 0x4b &&
                                    static_cast<unsigned char>(fZ64Eocd[2]) == 0x06 &&
                                    static_cast<unsigned char>(fZ64Eocd[3]) == 0x06) {
                                    quint64 realOff = 0;
                                    for (int i = 0; i < 8; i++)
                                        realOff |= static_cast<quint64>(static_cast<unsigned char>(fZ64Eocd[48 + i])) << (i * 8);
                                    fixCdrOffset = static_cast<qint64>(realOff);
                                }
                            }
                        }

                        // Subtract the PNG prefix offset (64비트 안전)
                        qint64 imgOffset64 = static_cast<qint64>(zipStart);
                        if (fixCdrOffset >= imgOffset64) {
                            qint64 restoredCdr64 = fixCdrOffset - imgOffset64;

                            // EOCD 32비트 필드 복원
                            if (restoredCdr64 <= 0xFFFFFFFF) {
                                quint32 restoredCdr = static_cast<quint32>(restoredCdr64);
                                QByteArray newFixCdr(4, 0);
                                newFixCdr[0] = restoredCdr & 0xFF;
                                newFixCdr[1] = (restoredCdr >> 8) & 0xFF;
                                newFixCdr[2] = (restoredCdr >> 16) & 0xFF;
                                newFixCdr[3] = (restoredCdr >> 24) & 0xFF;
                                fixZip.seek(fixEocdPos + 16);
                                fixZip.write(newFixCdr);
                            }
                            // 32비트 초과 시 0xFFFFFFFF 유지 (ZIP64 EOCD가 처리)

                            // Walk central directory and fix local file header offsets
                            qint64 fixPos = restoredCdr64;
                            while (fixPos < fixEocdPos) {
                                fixZip.seek(fixPos);
                                QByteArray fixSig = fixZip.read(4);
                                if (fixSig.size() < 4) break;
                                if (static_cast<unsigned char>(fixSig[0]) != 0x50 ||
                                    static_cast<unsigned char>(fixSig[1]) != 0x4b ||
                                    static_cast<unsigned char>(fixSig[2]) != 0x01 ||
                                    static_cast<unsigned char>(fixSig[3]) != 0x02) break;

                                fixZip.seek(fixPos + 28);
                                QByteArray fixLenBytes = fixZip.read(4);
                                quint16 fixNameLen = static_cast<quint16>(
                                    static_cast<unsigned char>(fixLenBytes[0]) | (static_cast<unsigned char>(fixLenBytes[1]) << 8));
                                quint16 fixExtraLen = static_cast<quint16>(
                                    static_cast<unsigned char>(fixLenBytes[2]) | (static_cast<unsigned char>(fixLenBytes[3]) << 8));
                                fixZip.seek(fixPos + 32);
                                QByteArray fixCommentLenBytes = fixZip.read(2);
                                quint16 fixCommentLen = static_cast<quint16>(
                                    static_cast<unsigned char>(fixCommentLenBytes[0]) | (static_cast<unsigned char>(fixCommentLenBytes[1]) << 8));

                                fixZip.seek(fixPos + 42);
                                QByteArray fixLfhBytes = fixZip.read(4);
                                quint32 fixOldLfh = static_cast<quint32>(
                                    static_cast<unsigned char>(fixLfhBytes[0]) |
                                    (static_cast<unsigned char>(fixLfhBytes[1]) << 8) |
                                    (static_cast<unsigned char>(fixLfhBytes[2]) << 16) |
                                    (static_cast<unsigned char>(fixLfhBytes[3]) << 24));

                                // ZIP64 extra field에 64비트 LFH offset이 있는 경우 처리
                                if (fixOldLfh == 0xFFFFFFFF && fixExtraLen >= 4) {
                                    // extra field에서 ZIP64 extended info 찾기
                                    fixZip.seek(fixPos + 46 + fixNameLen);
                                    QByteArray extraData = fixZip.read(fixExtraLen);
                                    qint64 eOff = 0;
                                    while (eOff + 4 <= extraData.size()) {
                                        quint16 eTag = static_cast<quint16>(
                                            static_cast<unsigned char>(extraData[eOff]) | (static_cast<unsigned char>(extraData[eOff+1]) << 8));
                                        quint16 eSize = static_cast<quint16>(
                                            static_cast<unsigned char>(extraData[eOff+2]) | (static_cast<unsigned char>(extraData[eOff+3]) << 8));
                                        if (eTag == 0x0001) {
                                            // origUncomp/origComp 필드 확인
                                            fixZip.seek(fixPos + 24);
                                            QByteArray szBytes = fixZip.read(8);
                                            quint32 oUncomp = static_cast<quint32>(
                                                static_cast<unsigned char>(szBytes[0]) | (static_cast<unsigned char>(szBytes[1]) << 8) |
                                                (static_cast<unsigned char>(szBytes[2]) << 16) | (static_cast<unsigned char>(szBytes[3]) << 24));
                                            quint32 oComp = static_cast<quint32>(
                                                static_cast<unsigned char>(szBytes[4]) | (static_cast<unsigned char>(szBytes[5]) << 8) |
                                                (static_cast<unsigned char>(szBytes[6]) << 16) | (static_cast<unsigned char>(szBytes[7]) << 24));
                                            int oRel = 4;
                                            if (oUncomp == 0xFFFFFFFF) oRel += 8;
                                            if (oComp == 0xFFFFFFFF) oRel += 8;
                                            if (eOff + oRel + 8 <= extraData.size()) {
                                                quint64 oldOff64 = 0;
                                                for (int b = 0; b < 8; b++)
                                                    oldOff64 |= static_cast<quint64>(static_cast<unsigned char>(extraData[eOff + oRel + b])) << (b * 8);
                                                if (static_cast<qint64>(oldOff64) >= imgOffset64) {
                                                    quint64 newOff64 = oldOff64 - static_cast<quint64>(imgOffset64);
                                                    QByteArray newOffBytes(8, 0);
                                                    for (int b = 0; b < 8; b++)
                                                        newOffBytes[b] = static_cast<char>((newOff64 >> (b * 8)) & 0xFF);
                                                    fixZip.seek(fixPos + 46 + fixNameLen + eOff + oRel);
                                                    fixZip.write(newOffBytes);
                                                }
                                            }
                                            break;
                                        }
                                        eOff += 4 + eSize;
                                    }
                                } else if (fixOldLfh >= static_cast<quint32>(qMin(imgOffset64, qint64(0xFFFFFFFF)))) {
                                    quint32 restoredLfh = fixOldLfh - static_cast<quint32>(imgOffset64);
                                    QByteArray newFixLfh(4, 0);
                                    newFixLfh[0] = restoredLfh & 0xFF;
                                    newFixLfh[1] = (restoredLfh >> 8) & 0xFF;
                                    newFixLfh[2] = (restoredLfh >> 16) & 0xFF;
                                    newFixLfh[3] = (restoredLfh >> 24) & 0xFF;
                                    fixZip.seek(fixPos + 42);
                                    fixZip.write(newFixLfh);
                                }

                                fixPos += 46 + fixNameLen + fixExtraLen + fixCommentLen;
                            }

                            // Handle ZIP64 EOCD Locator
                            if (fixEocdPos >= 20) {
                                fixZip.seek(fixEocdPos - 20);
                                QByteArray z64LocSig = fixZip.read(4);
                                if (z64LocSig.size() == 4 &&
                                    static_cast<unsigned char>(z64LocSig[0]) == 0x50 &&
                                    static_cast<unsigned char>(z64LocSig[1]) == 0x4b &&
                                    static_cast<unsigned char>(z64LocSig[2]) == 0x06 &&
                                    static_cast<unsigned char>(z64LocSig[3]) == 0x07) {
                                    fixZip.seek(fixEocdPos - 20 + 8);
                                    QByteArray z64Off = fixZip.read(8);
                                    quint64 z64Val = 0;
                                    for (int i = 0; i < 8; i++)
                                        z64Val |= static_cast<quint64>(static_cast<unsigned char>(z64Off[i])) << (i * 8);
                                    if (z64Val >= static_cast<quint64>(zipStart)) {
                                        quint64 newZ64Val = z64Val - static_cast<quint64>(zipStart);
                                        QByteArray newZ64(8, 0);
                                        for (int i = 0; i < 8; i++)
                                            newZ64[i] = static_cast<char>((newZ64Val >> (i * 8)) & 0xFF);
                                        fixZip.seek(fixEocdPos - 20 + 8);
                                        fixZip.write(newZ64);

                                        // Fix ZIP64 EOCD central directory offset
                                        fixZip.seek(static_cast<qint64>(newZ64Val));
                                        QByteArray z64EocdSig = fixZip.read(4);
                                        if (z64EocdSig.size() == 4 &&
                                            static_cast<unsigned char>(z64EocdSig[0]) == 0x50 &&
                                            static_cast<unsigned char>(z64EocdSig[1]) == 0x4b &&
                                            static_cast<unsigned char>(z64EocdSig[2]) == 0x06 &&
                                            static_cast<unsigned char>(z64EocdSig[3]) == 0x06) {
                                            fixZip.seek(static_cast<qint64>(newZ64Val) + 48);
                                            QByteArray z64Cdr = fixZip.read(8);
                                            quint64 z64CdrVal = 0;
                                            for (int i = 0; i < 8; i++)
                                                z64CdrVal |= static_cast<quint64>(static_cast<unsigned char>(z64Cdr[i])) << (i * 8);
                                            if (z64CdrVal >= static_cast<quint64>(zipStart)) {
                                                quint64 newZ64Cdr = z64CdrVal - static_cast<quint64>(zipStart);
                                                QByteArray newZ64CdrBytes(8, 0);
                                                for (int i = 0; i < 8; i++)
                                                    newZ64CdrBytes[i] = static_cast<char>((newZ64Cdr >> (i * 8)) & 0xFF);
                                                fixZip.seek(static_cast<qint64>(newZ64Val) + 48);
                                                fixZip.write(newZ64CdrBytes);
                                            }
                                        }
                                    }
                                }
                            }
                        }
                        fixZip.flush();
                        log("ZIP 오프셋 복원 완료", "info", "trad");
                    }
                    fixZip.close();
                }
            }

            progress(70, "압축 해제 중...");
            auto [ok, count] = tryExtract(tempZip, outputDir);
            QFile::remove(tempZip);

            if (ok) {
                progress(100, "완료!");
                log(QString("추출 완료! %1개 파일 → %2").arg(count).arg(outputDir), "success", "trad");
                verifyManifest(outputDir);
                runJs("setTradBusy(false)");
                return;
            }
            log("모든 추출 방법 실패", "error", "trad");
        }
        progress(-1, "");
        runJs("setTradBusy(false)");
    });

    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ── Browser (crawl embedded browser) ──
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void MiyoBackend::showBrowser(bool show)
{
    m_window->showBrowser(show);
}

void MiyoBackend::browserNavigate(const QString &url)
{
    auto *view = m_window->browserView();
    if (!view) return;
    QString finalUrl = url;
    if (!finalUrl.startsWith("http://") && !finalUrl.startsWith("https://")) {
        finalUrl = "https://" + finalUrl;
    }
    view->setUrl(QUrl(finalUrl));
}

void MiyoBackend::browserBack()
{
    auto *view = m_window->browserView();
    if (view) view->back();
}

void MiyoBackend::browserForward()
{
    auto *view = m_window->browserView();
    if (view) view->forward();
}

void MiyoBackend::browserRefresh()
{
    auto *view = m_window->browserView();
    if (view) view->reload();
}

void MiyoBackend::browserStop()
{
    auto *view = m_window->browserView();
    if (view) view->stop();
}

void MiyoBackend::crawlerContinueAfterLogin()
{
    if (m_crawler && m_crawler->isWaitingForLogin()) {
        m_crawler->continueAfterLogin();
    } else {
        log("크롤러가 로그인 대기 상태가 아닙니다", "warning", "crawl");
    }
}

void MiyoBackend::downloadPageMedia(const QString &configJson)
{
    auto *view = m_window->browserView();
    if (!view || !view->page()) {
        log("브라우저가 열려있지 않습니다", "error", "crawl");
        return;
    }

    QJsonObject config = QJsonDocument::fromJson(configJson.toUtf8()).object();
    QString savePath = config["path"].toString();
    if (savePath.startsWith("~")) {
        savePath = QDir::homePath() + savePath.mid(1);
    }
    QDir().mkpath(savePath);

    bool wantImages = config["images"].toBool(true);
    bool wantVideos = config["videos"].toBool(false);
    bool wantDocs = config["documents"].toBool(false);

    log("현재 페이지 미디어 추출 중...", "info", "crawl");

    // Extract media URLs from the current page via JavaScript
    QString extractJs = QString(R"(
        (function() {
            var urls = [];
            if (%1) {
                document.querySelectorAll('img[src]').forEach(function(img) {
                    var src = img.src;
                    if (src && !src.startsWith('data:') && img.naturalWidth > 50) {
                        urls.push({url: src, type: 'image'});
                    }
                });
            }
            if (%2) {
                document.querySelectorAll('video source[src], video[src]').forEach(function(v) {
                    var src = v.src;
                    if (src) urls.push({url: src, type: 'video'});
                });
            }
            if (%3) {
                document.querySelectorAll('a[href]').forEach(function(a) {
                    var href = a.href;
                    if (href && /\.(pdf|doc|docx|xls|xlsx|ppt|pptx|zip|rar)$/i.test(href)) {
                        urls.push({url: href, type: 'document'});
                    }
                });
            }
            return JSON.stringify(urls);
        })()
    )").arg(wantImages ? "true" : "false",
            wantVideos ? "true" : "false",
            wantDocs ? "true" : "false");

    view->page()->runJavaScript(extractJs, [this, savePath](const QVariant &result) {
        QJsonArray mediaArray = QJsonDocument::fromJson(result.toString().toUtf8()).array();
        if (mediaArray.isEmpty()) {
            log("미디어를 찾을 수 없습니다", "warning", "crawl");
            return;
        }
        log(QString("%1개 미디어 발견, 다운로드 시작...").arg(mediaArray.size()), "info", "crawl");

        // Copy data for thread
        QJsonArray mediaCopy = mediaArray;
        QString pathCopy = savePath;

        auto *thread = QThread::create([this, mediaCopy, pathCopy]() {
            int downloaded = 0;
            for (const auto &item : mediaCopy) {
                QJsonObject obj = item.toObject();
                QString url = obj["url"].toString();
                if (url.isEmpty()) continue;

                QString filename = QUrl(url).fileName();
                if (filename.isEmpty() || filename.length() > 200) {
                    filename = QString("media_%1").arg(downloaded + 1);
                }
                int qpos = filename.indexOf('?');
                if (qpos > 0) filename = filename.left(qpos);

                QString filepath = pathCopy + "/" + filename;
                if (QFile::exists(filepath)) {
                    QFileInfo fi(filepath);
                    filepath = pathCopy + "/" + fi.baseName() + "_" +
                               QString::number(QDateTime::currentMSecsSinceEpoch()) + "." + fi.suffix();
                }

                if (m_http->downloadFile(url, filepath)) {
                    downloaded++;
                    if (downloaded % 10 == 0) {
                        log(QString("다운로드 중... %1/%2").arg(downloaded).arg(mediaCopy.size()), "info", "crawl");
                    }
                }
            }
            log(QString("✅ %1개 미디어 다운로드 완료 → %2").arg(downloaded).arg(pathCopy), "success", "crawl");
        });
        connect(thread, &QThread::finished, thread, &QThread::deleteLater);
        thread->start();
    });
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ── System / Maintenance ──
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void MiyoBackend::getSystemInfo()
{
    QThread *thread = QThread::create([this]() {
        QString python = Common::bundledPythonPath();
        QProcessEnvironment env = Common::bundledProcessEnv();

        QStringList info;
        info << "═══════════════════════════════════";
        info << "  ABIWA 시스템 정보";
        info << "═══════════════════════════════════";

        // App version
        info << QString("앱 경로: %1").arg(QCoreApplication::applicationDirPath());
        info << QString("디스크 경로: %1").arg(m_config->tempDir().isEmpty() ? "(미설정)" : m_config->tempDir());

        // Disk info
        if (!m_config->tempDir().isEmpty()) {
            QStorageInfo storage(m_config->tempDir());
            if (storage.isValid()) {
                double freeGB = storage.bytesAvailable() / (1024.0*1024.0*1024.0);
                double totalGB = storage.bytesTotal() / (1024.0*1024.0*1024.0);
                info << QString("디스크: %1 (여유: %2 GB / 전체: %3 GB)")
                    .arg(storage.displayName())
                    .arg(freeGB, 0, 'f', 1)
                    .arg(totalGB, 0, 'f', 1);
            }
        }

        // OS info
#ifdef Q_OS_MACOS
        info << "OS: macOS";
#elif defined(Q_OS_WIN)
        info << "OS: Windows";
#else
        info << "OS: Linux";
#endif
        info << QString("Qt: %1").arg(QT_VERSION_STR);

        // Python version
        {
            QProcess proc;
            proc.setProcessEnvironment(env);
            proc.start(python, {"--version"});
            proc.waitForFinished(5000);
            QString ver = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
            if (ver.isEmpty()) ver = QString::fromUtf8(proc.readAllStandardError()).trimmed();
            info << QString("Python: %1").arg(ver.isEmpty() ? "(없음)" : ver);
            info << QString("Python 경로: %1").arg(QFile::exists(python) ? python : "(번들 없음)");
        }

        // Package versions
        {
            QProcess proc;
            proc.setProcessEnvironment(env);
            proc.start(python, {"-c",
                "import json, importlib.metadata as md\n"
                "pkgs = ['twikit','httpx','atproto','openpyxl','Pillow','piexif','bs4','websockets','lxml','m3u8']\n"
                "result = {}\n"
                "for p in pkgs:\n"
                "    try:\n"
                "        result[p] = md.version(p)\n"
                "    except:\n"
                "        try:\n"
                "            m = __import__(p)\n"
                "            result[p] = getattr(m, '__version__', '?')\n"
                "        except:\n"
                "            result[p] = '(미설치)'\n"
                "print(json.dumps(result))\n"
            });
            proc.waitForFinished(15000);
            QString output = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
            QJsonObject pkgs = QJsonDocument::fromJson(output.toUtf8()).object();
            info << "";
            info << "── 패키지 버전 ──";
            for (auto it = pkgs.constBegin(); it != pkgs.constEnd(); ++it) {
                info << QString("  %1: %2").arg(it.key(), it.value().toString());
            }
        }

        // Bundled tools
        info << "";
        info << "── 번들 도구 ──";
        QString appDir = QCoreApplication::applicationDirPath();
        QStringList tools = {"yt-dlp", "ffmpeg", "ffprobe"};
        for (const QString &tool : tools) {
#ifdef Q_OS_WIN
            QString path = appDir + "/" + tool + ".exe";
#else
            QString path = appDir + "/" + tool;
#endif
            if (QFile::exists(path)) {
                QFileInfo fi(path);
                info << QString("  %1: %2 MB").arg(tool).arg(fi.size() / (1024.0*1024.0), 0, 'f', 1);
            } else {
                info << QString("  %1: (없음)").arg(tool);
            }
        }

        // Python scripts
        QString toolsDir = Common::bundledToolsDir();
        QStringList scripts = {"twitter_daemon.py", "twitter_api.py", "twitter_tid.py", "bluesky_daemon.py"};
        for (const QString &s : scripts) {
            info << QString("  %1: %2").arg(s, QFile::exists(toolsDir + "/" + s) ? "OK" : "(없음)");
        }

        // Send to JS
        QString infoStr = info.join("\\n");
        infoStr.replace("'", "\\'");
        runJs(QString("showSystemInfo('%1')").arg(infoStr));

        // Write log file
        writeStartupLog();
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void MiyoBackend::updateModules()
{
    if (m_pythonBusy.load()) {
        log("Python 작업 진행 중 — 모듈 업데이트 대기", "info", "settings");
        return;
    }
    log("모듈 업데이트 시작...", "info", "settings");
    runJs("setModuleUpdating(true)");

    QThread *thread = QThread::create([this]() {
        QString python = Common::bundledPythonPath();
        QProcessEnvironment env = Common::bundledProcessEnv();

        if (!QFile::exists(python)) {
            log("번들 Python을 찾을 수 없습니다", "error", "settings");
            runJs("setModuleUpdating(false)");
            return;
        }

        // First install pip if needed
        {
            QProcess proc;
            proc.setProcessEnvironment(env);
            proc.start(python, {"-m", "ensurepip", "--default-pip"});
            proc.waitForFinished(60000);
        }

        QStringList packages = {"twikit", "httpx", "atproto", "openpyxl", "Pillow", "piexif",
                                "beautifulsoup4", "websockets", "lxml", "m3u8"};

        int updated = 0;
        int failed = 0;
        for (const QString &pkg : packages) {
            log(QString("  업데이트 중: %1...").arg(pkg), "info", "settings");

            QProcess proc;
            proc.setProcessEnvironment(env);
            proc.start(python, {"-m", "pip", "install", "--upgrade", "--no-input", pkg});
            proc.waitForFinished(120000);

            QString output = QString::fromUtf8(proc.readAllStandardOutput());
            QString err = QString::fromUtf8(proc.readAllStandardError());

            if (proc.exitCode() == 0) {
                if (output.contains("Successfully installed")) {
                    log(QString("  ✅ %1 업데이트됨").arg(pkg), "success", "settings");
                    updated++;
                } else {
                    log(QString("  ✅ %1 최신").arg(pkg), "info", "settings");
                }
            } else {
                log(QString("  ❌ %1 실패: %2").arg(pkg, err.left(100)), "error", "settings");
                failed++;
            }
        }

        log(QString("모듈 업데이트 완료 (업데이트: %1, 실패: %2)").arg(updated).arg(failed),
            failed > 0 ? "warning" : "success", "settings");
        runJs("setModuleUpdating(false)");

        // Refresh system info
        QMetaObject::invokeMethod(this, "getSystemInfo", Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// ── Python 환경 진단 (내부 헬퍼) ──
static QStringList diagnosePythonEnv(const QString &python)
{
    QStringList problems;
    if (!QFile::exists(python)) {
        problems << "python_missing";
        return problems;
    }

    // pip 확인
    QProcess proc;
    proc.setProcessEnvironment(Common::bundledProcessEnv());
    proc.start(python, {"-m", "pip", "--version"});
    proc.waitForFinished(10000);
    if (proc.exitCode() != 0) problems << "pip_broken";

    // 핵심 패키지 확인
    QStringList required = {"twikit", "httpx", "atproto", "openpyxl", "PIL", "piexif", "bs4", "lxml", "websockets", "m3u8"};
    for (const QString &mod : required) {
        QProcess check;
        check.setProcessEnvironment(Common::bundledProcessEnv());
        check.start(python, {"-c", QString("import %1").arg(mod)});
        check.waitForFinished(10000);
        if (check.exitCode() != 0) problems << ("missing:" + mod);
    }
    return problems;
}

void MiyoBackend::upgradePython()
{
    if (m_pythonBusy.exchange(true)) {
        log("Python 작업이 이미 진행 중입니다.", "warning", "settings");
        return;
    }
    log("Python 업그레이드 시작...", "info", "settings");
    runJs("setPythonEnvBusy(true, '업그레이드 중...')");

    QThread *thread = QThread::create([this]() {
        // ★ 번들 내부가 아니라 쓰기가능 외부 python_env 에 설치 (번들 codesign seal 보호).
        QString pythonDir = Common::activePythonEnvDir();

#ifdef Q_OS_MACOS
        // 외부 복사본을 만들 수 없어 번들을 가리키면 중단 — 번들에 쓰면 seal 깨져 SIGKILL.
        if (pythonDir != Common::userPythonEnvDir()) {
            log("⚠️ python_env 쓰기가능 복사본을 만들 수 없어 업그레이드를 중단합니다 (번들 서명 보호). 디스크 공간을 확인하세요.", "error", "settings");
            m_pythonBusy = false;
            runJs("setPythonEnvBusy(false, '중단')");
            return;
        }
#endif

        // 1. 현재 패키지 목록 저장 (복원용)
        QString python = pythonDir + "/bin/python3";
        QStringList frozenPkgs;
        if (QFile::exists(python)) {
            QProcess freeze;
            freeze.setProcessEnvironment(Common::bundledProcessEnv());
            freeze.start(python, {"-m", "pip", "freeze"});
            freeze.waitForFinished(30000);
            frozenPkgs = QString::fromUtf8(freeze.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
            log(QString("  현재 패키지 %1개 백업").arg(frozenPkgs.size()), "info", "settings");
        }

        // 2. 기존 환경 삭제
        log("  기존 Python 환경 삭제 중...", "info", "settings");
        QDir(pythonDir).removeRecursively();

        // 3. 최신 standalone Python 다운로드 + 설치
        log("  최신 Python 다운로드 중... (시간이 걸릴 수 있습니다)", "info", "settings");

        // astral-sh에서 최신 릴리스 URL 조회
        // 임시 다운로드 경로 (앱 번들 밖)
        QString tarballDir = QStandardPaths::writableLocation(QStandardPaths::CacheLocation) + "/python_standalone";
        QDir().mkpath(tarballDir);

        // 기존 tarball 삭제 → 강제 재다운로드
        QDirIterator tarIt(tarballDir, {"cpython-*.tar.gz"}, QDir::Files);
        while (tarIt.hasNext()) {
            QFile::remove(tarIt.next());
        }

        // GitHub API로 최신 릴리스 찾기
        QString arch = "aarch64-apple-darwin";
#ifdef Q_OS_WIN
        arch = "x86_64-pc-windows-msvc-shared";
#elif defined(Q_OS_LINUX)
        arch = "x86_64-unknown-linux-gnu";
#endif

        // curl로 최신 릴리스 검색
        QProcess curlProc;
        curlProc.start("curl", {"-sL", "https://api.github.com/repos/astral-sh/python-build-standalone/releases/latest"});
        curlProc.waitForFinished(30000);
        QString releaseJson = QString::fromUtf8(curlProc.readAllStandardOutput());

        QString downloadUrl;
        QString newVersion;
        // install_only 타입의 URL 전부 수집 → 안정 버전 중 최신 선택
        QRegularExpression urlRe(
            QString("\"browser_download_url\"\\s*:\\s*\"(https://[^\"]*cpython-(\\d+\\.\\d+\\.\\d+)[^\"]*%1-install_only\\.tar\\.gz)\"")
                .arg(QRegularExpression::escape(arch)));
        auto it = urlRe.globalMatch(releaseJson);
        int bestMajor = 0, bestMinor = 0, bestPatch = 0;
        while (it.hasNext()) {
            auto m = it.next();
            QString url = m.captured(1);
            QString ver = m.captured(2);
            // 알파/베타/rc 건너뛰기 (freethreaded도 제외 — 이미 패턴에서 제외됨)
            if (url.contains("freethreaded")) continue;
            QStringList parts = ver.split('.');
            int major = parts[0].toInt(), minor = parts[1].toInt(), patch = parts[2].toInt();
            // 최신 안정 버전 선택 (major.minor.patch 비교)
            if (major > bestMajor || (major == bestMajor && minor > bestMinor) ||
                (major == bestMajor && minor == bestMinor && patch > bestPatch)) {
                bestMajor = major; bestMinor = minor; bestPatch = patch;
                downloadUrl = url;
                newVersion = ver;
            }
        }

        if (downloadUrl.isEmpty()) {
            log("  ❌ 최신 Python 릴리스를 찾을 수 없습니다. 복구 모드로 전환...", "error", "settings");
            m_pythonBusy = false;
            runJs("setPythonEnvBusy(false, '실패')");
            QMetaObject::invokeMethod(this, "repairPython", Qt::QueuedConnection);
            return;
        }

        log(QString("  다운로드: Python %1").arg(newVersion), "info", "settings");

        QString tarball = tarballDir + "/cpython-latest-" + arch + ".tar.gz";
        QProcess dlProc;
        dlProc.start("curl", {"-L", "-o", tarball, downloadUrl});
        dlProc.waitForFinished(300000);  // 5분
        if (dlProc.exitCode() != 0 || !QFile::exists(tarball)) {
            log("  ❌ 다운로드 실패", "error", "settings");
            m_pythonBusy = false;
            runJs("setPythonEnvBusy(false, '다운로드 실패')");
            return;
        }

        // 4. 압축 해제
        log("  압축 해제 중...", "info", "settings");
        QDir().mkpath(pythonDir);
        QProcess tarProc;
        tarProc.start("tar", {"xzf", tarball, "-C", pythonDir, "--strip-components=1"});
        tarProc.waitForFinished(120000);

        python = pythonDir + "/bin/python3";
        if (!QFile::exists(python)) {
            log("  ❌ Python 바이너리를 찾을 수 없습니다", "error", "settings");
            m_pythonBusy = false;
            runJs("setPythonEnvBusy(false, '설치 실패')");
            return;
        }

        // 버전 확인
        QProcess verProc;
        verProc.setProcessEnvironment(Common::bundledProcessEnv());
        verProc.start(python, {"--version"});
        verProc.waitForFinished(5000);
        QString installedVer = QString::fromUtf8(verProc.readAllStandardOutput()).trimmed();
        log(QString("  ✅ %1 설치 완료").arg(installedVer), "success", "settings");

        // 5. pip 초기화
        log("  pip 초기화 중...", "info", "settings");
        QProcess pipInit;
        pipInit.setProcessEnvironment(Common::bundledProcessEnv());
        pipInit.start(python, {"-m", "ensurepip", "--default-pip"});
        pipInit.waitForFinished(60000);

        // 6. 패키지 재설치
        QStringList packages = {"twikit", "httpx", "atproto", "openpyxl", "Pillow", "piexif",
                                "beautifulsoup4", "websockets", "lxml", "m3u8"};

        log(QString("  패키지 %1개 설치 중...").arg(packages.size()), "info", "settings");
        int installed = 0, failed = 0;
        for (const QString &pkg : packages) {
            QProcess inst;
            inst.setProcessEnvironment(Common::bundledProcessEnv());
            inst.start(python, {"-m", "pip", "install", "--no-cache-dir", "--no-input", pkg});
            inst.waitForFinished(120000);
            if (inst.exitCode() == 0) {
                installed++;
            } else {
                QString err = QString::fromUtf8(inst.readAllStandardError()).left(80);
                log(QString("  ❌ %1 실패: %2").arg(pkg, err), "error", "settings");
                failed++;
            }
        }

        // 7. 정리
        log("  정리 중...", "info", "settings");
        // ★ rm -rf 안전 가드 — pythonDir 이 비었거나 예상 밖 경로면 정리 스킵.
        //   (pythonDir 이 빈 문자열이면 rm -rf '/share' 같은 사고가 날 수 있음)
        if (pythonDir.endsWith("/python_env") && QDir(pythonDir + "/bin").exists()) {
            QProcess cleanProc;
            cleanProc.start("bash", {"-c", QString(
                "find '%1' -name tests -type d -exec rm -rf {} + 2>/dev/null;"
                "find '%1' -name test -type d -exec rm -rf {} + 2>/dev/null;"
                "rm -rf '%1/share' '%1/include' 2>/dev/null;"
                "find '%1' -name __pycache__ -type d -exec rm -rf {} + 2>/dev/null"
            ).arg(pythonDir)});
            cleanProc.waitForFinished(30000);
        }

        // marker
        QFile marker(pythonDir + "/.bundled_ok");
        if (marker.open(QIODevice::WriteOnly)) {
            marker.write(QString("upgraded python %1 %2\n").arg(installedVer, QDateTime::currentDateTime().toString()).toUtf8());
            marker.close();
        }

        QString size;
        QProcess duProc;
        duProc.start("du", {"-sh", pythonDir});
        duProc.waitForFinished(10000);
        size = QString::fromUtf8(duProc.readAllStandardOutput()).split('\t').first().trimmed();

        log(QString("✅ Python 업그레이드 완료! %1, 패키지 %2개 설치, 크기: %3")
            .arg(installedVer).arg(installed).arg(size),
            failed > 0 ? "warning" : "success", "settings");
        if (failed > 0)
            log(QString("  ⚠️ %1개 패키지 실패 — '환경 복구' 버튼으로 재시도하세요").arg(failed), "warning", "settings");

        m_pythonBusy = false;
        runJs("setPythonEnvBusy(false, '완료')");
        QMetaObject::invokeMethod(this, "getSystemInfo", Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void MiyoBackend::repairPython()
{
    if (m_pythonBusy.exchange(true)) {
        log("Python 작업이 이미 진행 중입니다.", "warning", "settings");
        return;
    }
    log("Python 환경 복구 시작...", "info", "settings");
    runJs("setPythonEnvBusy(true, '복구 중...')");

    QThread *thread = QThread::create([this]() {
        QString python = Common::bundledPythonPath();
        // ★ 쓰기가능 외부 python_env (번들 codesign seal 보호 — 번들엔 절대 쓰지 않음).
        QString pythonDir = Common::activePythonEnvDir();

        // 1. 진단
        log("  환경 진단 중...", "info", "settings");
        QStringList problems = diagnosePythonEnv(python);

        if (problems.isEmpty()) {
            log("✅ Python 환경에 문제가 없습니다!", "success", "settings");
            m_pythonBusy = false;
            runJs("setPythonEnvBusy(false, '정상')");
            return;
        }

        log(QString("  발견된 문제 %1개:").arg(problems.size()), "warning", "settings");
        for (const QString &p : problems) {
            log(QString("    • %1").arg(p), "warning", "settings");
        }

        // 2. Python 자체가 없으면 → 번들에서 외부로 재시드 (번들엔 절대 쓰지 않음 — seal 보호)
        if (problems.contains("python_missing")) {
            log("  Python 바이너리가 없습니다. 번들에서 재설치 시도...", "warning", "settings");

            // 깨진 외부본 표식 제거 후, activePythonEnvDir() 가 번들→외부 재복사(시드)를 수행하도록 유도.
            QFile::remove(pythonDir + "/.bundled_ok");
            Common::activePythonEnvDir();
            python = Common::bundledPythonPath();

            if (!QFile::exists(python)) {
                log("  ❌ 재설치 실패. 'Python 업그레이드'로 새로 받으세요.", "error", "settings");
                m_pythonBusy = false;
                runJs("setPythonEnvBusy(false, '복구 실패')");
                return;
            }
            // 재진단
            problems = diagnosePythonEnv(python);
        }

        // 3. pip 복구
        if (problems.contains("pip_broken")) {
            log("  pip 복구 중...", "info", "settings");
            QProcess proc;
            proc.setProcessEnvironment(Common::bundledProcessEnv());
            proc.start(python, {"-m", "ensurepip", "--default-pip"});
            proc.waitForFinished(60000);

            // pip upgrade
            QProcess upg;
            upg.setProcessEnvironment(Common::bundledProcessEnv());
            upg.start(python, {"-m", "pip", "install", "--upgrade", "pip"});
            upg.waitForFinished(60000);

            if (upg.exitCode() == 0) {
                log("  ✅ pip 복구 완료", "success", "settings");
                problems.removeAll("pip_broken");
            } else {
                log("  ❌ pip 복구 실패", "error", "settings");
            }
        }

        // 4. 누락 패키지 설치
        QStringList missingPkgs;
        QMap<QString, QString> modToPkg = {
            {"PIL", "Pillow"}, {"bs4", "beautifulsoup4"},
            {"twikit", "twikit"}, {"httpx", "httpx"}, {"atproto", "atproto"},
            {"openpyxl", "openpyxl"}, {"piexif", "piexif"},
            {"lxml", "lxml"}, {"websockets", "websockets"}, {"m3u8", "m3u8"}
        };

        for (const QString &p : problems) {
            if (p.startsWith("missing:")) {
                QString mod = p.mid(8);
                QString pkg = modToPkg.value(mod, mod);
                if (!missingPkgs.contains(pkg)) missingPkgs << pkg;
            }
        }

        if (!missingPkgs.isEmpty()) {
            log(QString("  누락 패키지 %1개 설치 중...").arg(missingPkgs.size()), "info", "settings");
            int fixed = 0;
            for (const QString &pkg : missingPkgs) {
                log(QString("    설치: %1").arg(pkg), "info", "settings");
                QProcess inst;
                inst.setProcessEnvironment(Common::bundledProcessEnv());
                inst.start(python, {"-m", "pip", "install", "--no-cache-dir", "--no-input", pkg});
                inst.waitForFinished(120000);
                if (inst.exitCode() == 0) {
                    log(QString("    ✅ %1 설치됨").arg(pkg), "success", "settings");
                    fixed++;
                } else {
                    QString err = QString::fromUtf8(inst.readAllStandardError()).left(80);
                    log(QString("    ❌ %1 실패: %2").arg(pkg, err), "error", "settings");
                }
            }
            log(QString("  패키지 복구: %1/%2 성공").arg(fixed).arg(missingPkgs.size()),
                fixed == missingPkgs.size() ? "success" : "warning", "settings");
        }

        // 5. 최종 검증
        QStringList remaining = diagnosePythonEnv(python);
        m_pythonBusy = false;
        if (remaining.isEmpty()) {
            log("✅ Python 환경 복구 완료! 모든 문제 해결됨.", "success", "settings");
            runJs("setPythonEnvBusy(false, '복구 완료')");
        } else {
            log(QString("⚠️ %1개 문제가 남아있습니다. Python 업그레이드를 시도하세요.").arg(remaining.size()), "warning", "settings");
            for (const QString &r : remaining) log(QString("    • %1").arg(r), "warning", "settings");
            runJs("setPythonEnvBusy(false, '일부 실패')");
        }

        QMetaObject::invokeMethod(this, "getSystemInfo", Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void MiyoBackend::refreshTwitterTokens()
{
    log("Chrome에서 Twitter 토큰 추출 중...", "info", "settings");
    runJs("setTokenRefreshing(true)");

    QThread *thread = QThread::create([this]() {
        QString python = Common::bundledPythonPath();
        QProcessEnvironment env = Common::bundledProcessEnv();

        QProcess proc;
        proc.setProcessEnvironment(env);
        proc.start(python, {"-c",
            "import json, sys\n"
            "try:\n"
            "    import browser_cookie3 as bc3\n"
            "    found = None\n"
            "    errors = []\n"
            "    import sys\n"
            "    _browsers_mac = ['firefox', 'librewolf', 'chrome', 'edge', 'brave', 'arc', 'vivaldi', 'opera']\n"
            "    _browsers_pc  = ['chrome', 'edge', 'brave', 'arc', 'opera', 'opera_gx', 'vivaldi', 'firefox', 'librewolf', 'chromium']\n"
            "    browsers = _browsers_mac if sys.platform == 'darwin' else _browsers_pc\n"
            "    for fn_name in browsers:\n"
            "        fn = getattr(bc3, fn_name, None)\n"
            "        if not fn: continue\n"
            "        for dom in ['x.com', 'twitter.com']:\n"
            "            try:\n"
            "                cj = fn(domain_name=dom)\n"
            "                toks = {c.name: c.value for c in cj if c.name in ('auth_token', 'ct0')}\n"
            "                if toks.get('auth_token') and toks.get('ct0'):\n"
            "                    found = {'auth_token': toks['auth_token'], 'ct0': toks['ct0'], 'browser': fn_name}\n"
            "                    break\n"
            "            except Exception as e:\n"
            "                errors.append(f'{fn_name}/{dom}: {type(e).__name__}')\n"
            "        if found: break\n"
            "    if found:\n"
            "        print(json.dumps(found))\n"
            "    else:\n"
            "        print(json.dumps({'error': 'Token not found. 브라우저에서 Twitter/X 로그인 확인. (' + '; '.join(errors[:3]) + ')'}))\n"
            "except Exception as e:\n"
            "    print(json.dumps({'error': f'{type(e).__name__}: {e}'}))\n"
        });
        proc.waitForFinished(30000);

        QString output = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        QJsonObject result = QJsonDocument::fromJson(output.toUtf8()).object();

        QString authToken = result.contains("error") ? QString() : result["auth_token"].toString();
        QString ct0 = result.contains("error") ? QString() : result["ct0"].toString();
        QString error = result["error"].toString();

        QMetaObject::invokeMethod(this, [this, authToken, ct0, error]() {
            if (!error.isEmpty()) {
                // ★ Chrome 127+ App-Bound Encryption — Windows 에서 일반 권한으론 못 풂.
                //   rookiepy 등 다른 라이브러리도 동일하게 막힘 (Chrome 의 의도된 보안).
                //   유일한 해결은 수동 추출 → 사용자에게 정확한 단계 안내.
                if (error.contains("RequiresAdminError")) {
                    log("⚠ Chrome 127+ 의 App-Bound Encryption 으로 자동 추출이 차단됐습니다.", "warning", "settings");
                    log("  📋 수동 방법:", "info", "settings");
                    log("    1) Chrome 에서 https://x.com 로그인 상태 확인", "info", "settings");
                    log("    2) F12 (개발자도구) → Application 탭 → Cookies → x.com 선택", "info", "settings");
                    log("    3) 'auth_token' 행 클릭 → Value 값 복사 → 계정 설정에 붙여넣기", "info", "settings");
                    log("    4) 'ct0' 행도 같은 방법으로 복사 → 붙여넣기", "info", "settings");
                    log("  (또는 Firefox 에 x.com 로그인 되어있으면 자동 추출 시도 가능 — Firefox 는 App-Bound 없음)", "info", "settings");
                } else {
                    log("토큰 추출 실패: " + error, "error", "settings");
                }
                runJs("setTokenRefreshing(false)");
                return;
            }
            if (authToken.isEmpty() || ct0.isEmpty()) {
                log("토큰을 찾을 수 없습니다. Chrome에서 Twitter/X에 로그인되어 있는지 확인하세요.", "error", "settings");
                runJs("setTokenRefreshing(false)");
                return;
            }

            log("✅ Chrome에서 토큰 추출 성공!", "success", "settings");
            log(QString("  auth_token: %1...").arg(authToken.left(8)), "info", "settings");
            log(QString("  ct0: %1...").arg(ct0.left(8)), "info", "settings");

            // JS 측에서 계정 업데이트 + 저장
            QString js = QString(
                "if (!accounts.twitter) accounts.twitter = [];"
                "if (accounts.twitter.length > 0) {"
                "  accounts.twitter[0].auth_token = '%1';"
                "  accounts.twitter[0].ct0 = '%2';"
                "  appendLog('✅ 첫 번째 계정 토큰 갱신됨', 'success', 'settings');"
                "} else {"
                "  accounts.twitter.push({name:'Chrome', auth_token:'%1', ct0:'%2'});"
                "  appendLog('✅ Chrome 계정 자동 추가됨', 'success', 'settings');"
                "}"
                "renderAccounts('twitter'); saveConfig();"
                "setTokenRefreshing(false);"
            ).arg(authToken, ct0);
            runJs(js);

            // ★ Twitter 의 전체 cookie 도 capture cookie input (twitter-cookie) 에 채움
            //   age-gate/NSFW 통과용 보조 cookie (kdt, twid, gt 등) 포함
            refreshDomainCookies("x.com", "twitter-cookie", "twitter", "Twitter capture", "");
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void MiyoBackend::refreshInstagramSession()
{
    log("Chrome에서 Instagram 세션 추출 중...", "info", "settings");
    runJs("setIgSessionRefreshing(true)");

    QThread *thread = QThread::create([this]() {
        QString python = Common::bundledPythonPath();
        QProcessEnvironment env = Common::bundledProcessEnv();

        QProcess proc;
        proc.setProcessEnvironment(env);
        proc.start(python, {"-c",
            "import json, sys\n"
            "try:\n"
            "    import browser_cookie3 as bc3\n"
            "    found = None\n"
            "    errors = []\n"
            "    import sys\n"
            "    _browsers_mac = ['firefox', 'librewolf', 'chrome', 'edge', 'brave', 'arc', 'vivaldi', 'opera']\n"
            "    _browsers_pc  = ['chrome', 'edge', 'brave', 'arc', 'opera', 'opera_gx', 'vivaldi', 'firefox', 'librewolf']\n"
            "    browsers = _browsers_mac if sys.platform == 'darwin' else _browsers_pc\n"
            "    for fn_name in browsers:\n"
            "        fn = getattr(bc3, fn_name, None)\n"
            "        if not fn: continue\n"
            "        try:\n"
            "            cj = fn(domain_name='instagram.com')\n"
            "            cookies = {}\n"
            "            for c in cj:\n"
            "                if c.value: cookies[c.name] = c.value\n"
            "            if cookies.get('sessionid'):\n"
            "                # 인스타 검증 필수 쿠키 먼저 + 나머지\n"
            "                wanted = ['sessionid','csrftoken','ds_user_id','ig_did','mid','rur','ig_nrcb']\n"
            "                parts = [f'{k}={cookies[k]}' for k in wanted if k in cookies]\n"
            "                for k, v in cookies.items():\n"
            "                    if k not in wanted: parts.append(f'{k}={v}')\n"
            "                found = {\n"
            "                    'sessionid': cookies['sessionid'],\n"
            "                    'full_cookie': '; '.join(parts),\n"
            "                    'cookie_count': len(cookies),\n"
            "                    'browser': fn_name\n"
            "                }\n"
            "                break\n"
            "        except Exception as e:\n"
            "            errors.append(f'{fn_name}: {type(e).__name__}')\n"
            "    if found:\n"
            "        print(json.dumps(found))\n"
            "    else:\n"
            "        print(json.dumps({'error': 'sessionid not found. 브라우저에서 Instagram 로그인 확인. (' + '; '.join(errors[:3]) + ')'}))\n"
            "except Exception as e:\n"
            "    print(json.dumps({'error': f'{type(e).__name__}: {e}'}))\n"
        });
        proc.waitForFinished(30000);

        QString output = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        QJsonObject result = QJsonDocument::fromJson(output.toUtf8()).object();
        QString sessionId = result["sessionid"].toString();
        QString fullCookie = result["full_cookie"].toString();
        int cookieCount = result["cookie_count"].toInt();
        QString browser = result["browser"].toString();
        QString error = result["error"].toString();

        QMetaObject::invokeMethod(this, [this, sessionId, fullCookie, cookieCount, browser, error]() {
            if (!error.isEmpty()) {
                log("Instagram 세션 추출 실패: " + error, "error", "settings");
                runJs("setIgSessionRefreshing(false)");
                return;
            }
            if (sessionId.isEmpty()) {
                log("sessionid를 찾을 수 없습니다. Chrome에서 Instagram에 로그인하세요.", "error", "settings");
                runJs("setIgSessionRefreshing(false)");
                return;
            }

            log(QString("✅ Instagram 세션 추출 성공! [%1] 쿠키 %2개").arg(browser).arg(cookieCount), "success", "settings");
            log(QString("  sessionid: %1...").arg(sessionId.left(10)), "info", "settings");
            log(QString("  포함: %1").arg(fullCookie.left(120) + "..."), "info", "settings");

            // ★ JS-safe 인코딩 — sessionid + 전체 cookie 모두 UI 에 반영
            QString jSid  = Common::jsStringLiteral(sessionId);
            QString jFull = Common::jsStringLiteral(fullCookie);
            QString js = QString(
                "if (!accounts.instagram) accounts.instagram = [];"
                "if (accounts.instagram.length > 0) {"
                "  accounts.instagram[0].session_id = %1;"
                "  accounts.instagram[0].full_cookie = %2;"
                "  appendLog('✅ Instagram 세션 갱신됨 (전체 쿠키 적용)', 'success', 'settings');"
                "} else {"
                "  accounts.instagram.push({name:'Chrome', session_id:%1, full_cookie:%2});"
                "  appendLog('✅ Instagram Chrome 계정 자동 추가됨', 'success', 'settings');"
                "}"
                "// 인스타 탭 sessionid input 도 즉시 동기화\n"
                "var sidEl = document.getElementById('instagram-session-id');"
                "if (sidEl) sidEl.value = %1;"
                "// capture cookie input — 전체 cookie 박기 (collector 가 우선 사용)\n"
                "var ckEl = document.getElementById('instagram-cookie');"
                "if (ckEl) ckEl.value = %2;"
                "renderAccounts('instagram'); saveConfig();"
                "setIgSessionRefreshing(false);"
            ).arg(jSid, jFull);
            runJs(js);
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// Instagram 세션 자동 갱신 (Chrome에서 추출) - 수집 중 401 시 호출
QString MiyoBackend::extractInstagramSessionSync()
{
    // ★ Instagram API 는 sessionid 외에도 csrftoken, ds_user_id, ig_did 같은
    //   다른 쿠키들을 검증. 모두 추출해서 합친 Cookie header 반환.
    QString python = Common::bundledPythonPath();
    QProcessEnvironment env = Common::bundledProcessEnv();

    QProcess proc;
    proc.setProcessEnvironment(env);
    proc.start(python, {"-c",
        "import sys\n"
        "try:\n"
        "    import browser_cookie3 as bc3\n"
        "    # 우선순위: chrome > edge > brave > arc > opera ...\n"
        "    import sys\n"
        "    _browsers_mac = ['firefox', 'chrome', 'edge', 'brave', 'arc', 'vivaldi']\n"
        "    _browsers_pc  = ['chrome', 'edge', 'brave', 'arc', 'opera', 'vivaldi', 'firefox']\n"
        "    browsers = _browsers_mac if sys.platform == 'darwin' else _browsers_pc\n"
        "    for fn_name in browsers:\n"
        "        fn = getattr(bc3, fn_name, None)\n"
        "        if not fn: continue\n"
        "        try:\n"
        "            cj = fn(domain_name='instagram.com')\n"
        "            # 모든 인스타 쿠키 dict 로 모음\n"
        "            cookies = {}\n"
        "            for c in cj:\n"
        "                if c.value: cookies[c.name] = c.value\n"
        "            # sessionid 가 있고 비어있지 않으면 성공\n"
        "            if cookies.get('sessionid'):\n"
        "                # 인스타 검증에 필요한 쿠키 우선 — sessionid + csrftoken + ds_user_id + ig_did + mid + rur\n"
        "                wanted = ['sessionid','csrftoken','ds_user_id','ig_did','mid','rur','ig_nrcb']\n"
        "                parts = []\n"
        "                for k in wanted:\n"
        "                    if k in cookies: parts.append(f'{k}={cookies[k]}')\n"
        "                # 나머지 인스타 쿠키도 추가\n"
        "                for k, v in cookies.items():\n"
        "                    if k not in wanted: parts.append(f'{k}={v}')\n"
        "                print('; '.join(parts))\n"
        "                sys.exit(0)\n"
        "        except Exception: continue\n"
        "except Exception: pass\n"
    });
    proc.waitForFinished(15000);
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
}

void MiyoBackend::refreshPixivSession()
{
    log("Chrome에서 Pixiv 세션 추출 중...", "info", "settings");
    runJs("setPixivRefreshing(true)");

    QThread *thread = QThread::create([this]() {
        QString python = Common::bundledPythonPath();
        QProcessEnvironment env = Common::bundledProcessEnv();

        QProcess proc;
        proc.setProcessEnvironment(env);
        proc.start(python, {"-c",
            "import json, sys\n"
            "try:\n"
            "    import browser_cookie3 as bc3\n"
            "    found = None\n"
            "    errors = []\n"
            "    import sys\n"
            "    _browsers_mac = ['firefox', 'librewolf', 'chrome', 'edge', 'brave', 'arc', 'vivaldi', 'opera']\n"
            "    _browsers_pc  = ['chrome', 'edge', 'brave', 'arc', 'opera', 'opera_gx', 'vivaldi', 'firefox', 'librewolf', 'chromium']\n"
            "    browsers = _browsers_mac if sys.platform == 'darwin' else _browsers_pc\n"
            "    for fn_name in browsers:\n"
            "        fn = getattr(bc3, fn_name, None)\n"
            "        if not fn: continue\n"
            "        try:\n"
            "            cj = fn(domain_name='pixiv.net')\n"
            "            for c in cj:\n"
            "                if c.name == 'PHPSESSID' and c.value:\n"
            "                    found = {'phpsessid': c.value, 'browser': fn_name}\n"
            "                    break\n"
            "            if found: break\n"
            "        except Exception as e:\n"
            "            errors.append(f'{fn_name}: {type(e).__name__}')\n"
            "    if found:\n"
            "        print(json.dumps(found))\n"
            "    else:\n"
            "        print(json.dumps({'error': 'PHPSESSID not found. 브라우저에서 Pixiv 로그인 확인. (' + '; '.join(errors[:3]) + ')'}))\n"
            "except Exception as e:\n"
            "    print(json.dumps({'error': f'{type(e).__name__}: {e}'}))\n"
        });
        proc.waitForFinished(30000);

        QString output = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        QJsonObject result = QJsonDocument::fromJson(output.toUtf8()).object();
        QString phpsessid = result.contains("error") ? QString() : result["phpsessid"].toString();
        QString error = result["error"].toString();

        QMetaObject::invokeMethod(this, [this, phpsessid, error]() {
            runJs("setPixivRefreshing(false)");
            if (!error.isEmpty()) {
                log("Pixiv 세션 추출 실패: " + error, "warning", "settings");
                return;
            }
            log("✅ Pixiv 세션 추출 성공!", "success", "settings");
            log(QString("  PHPSESSID: %1...").arg(phpsessid.left(10)), "info", "settings");

            QString js = QString(
                "if (!accounts.pixiv) accounts.pixiv = [];"
                "if (accounts.pixiv.length > 0) {"
                "  accounts.pixiv[0].session_id = '%1';"
                "  appendLog('✅ Pixiv 세션 갱신됨', 'success', 'settings');"
                "} else {"
                "  accounts.pixiv.push({name:'Chrome', session_id:'%1'});"
                "  appendLog('✅ Pixiv Chrome 계정 자동 추가됨', 'success', 'settings');"
                "}"
                "renderAccounts('pixiv'); saveConfig();"
            ).arg(phpsessid);
            runJs(js);

            // ★ Pixiv 의 전체 cookie 도 capture cookie input 에 채움 (R-18/fanbox-only 통과용)
            refreshDomainCookies("pixiv.net", "pixiv-extra-cookie", "pixiv", "Pixiv capture", "");
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// ═════════════════════════════════════════════════════════════════════════
// Pixiv Fanbox — 멤버십 컨텐츠 자동 추출 (FANBOXSESSID)
// ═════════════════════════════════════════════════════════════════════════
void MiyoBackend::refreshFanboxSession()
{
    log("Chrome에서 Fanbox 세션 추출 중...", "info", "settings");
    runJs("var b=document.getElementById('fanbox-session-refresh-btn');if(b){b.disabled=true;b.dataset._origText=b.dataset._origText||b.textContent;b.textContent='추출 중...';}");

    QThread *t = QThread::create([this]() {
        QProcess proc;
        proc.setProcessEnvironment(Common::bundledProcessEnv());
        // ★ Fanbox 쿠키 추출 — pixiv 패턴과 동일하게 모든 도메인 cookie 받아서 이름으로 필터.
        //   FANBOXSESSID 는 .fanbox.cc 도메인에 set 됨 — domain_name=None 으로 전체 받아서 매칭.
        //   여러 browser 시도 + 각 단계 에러 로깅으로 silent fail 방지.
        proc.start(Common::bundledPythonPath(), {"-c",
            "import json, sys\n"
            "try:\n"
            "    import browser_cookie3 as bc3\n"
            "    found = None\n"
            "    errors = []\n"
            "    diag = []\n"
            "    import sys\n"
            "    _browsers_mac = ['firefox', 'librewolf', 'chrome', 'edge', 'brave', 'arc', 'vivaldi', 'opera']\n"
            "    _browsers_pc  = ['chrome', 'edge', 'brave', 'arc', 'opera', 'opera_gx', 'vivaldi', 'firefox', 'librewolf', 'chromium']\n"
            "    browsers = _browsers_mac if sys.platform == 'darwin' else _browsers_pc\n"
            "    for fn_name in browsers:\n"
            "        fn = getattr(bc3, fn_name, None)\n"
            "        if not fn: continue\n"
            "        # 1) 명시 도메인 시도 → 실패 시 전체 받아서 이름 매칭 (browser_cookie3 일부 버전 도메인 필터 버그)\n"
            "        for dom in ['fanbox.cc', '.fanbox.cc', None]:\n"
            "            try:\n"
            "                cj = fn(domain_name=dom) if dom else fn()\n"
            "                fanbox_cookies = {}\n"
            "                for c in cj:\n"
            "                    if c.value and ('fanbox' in (c.domain or '').lower() or dom):\n"
            "                        fanbox_cookies[c.name] = c.value\n"
            "                # 전체 받았으면 fanbox.cc 만 필터링\n"
            "                if dom is None:\n"
            "                    fanbox_cookies = {c.name: c.value for c in cj if c.value and 'fanbox' in (c.domain or '').lower()}\n"
            "                diag.append(f'{fn_name}({dom or \"all\"}): {len(fanbox_cookies)} fanbox cookies')\n"
            "                if fanbox_cookies.get('FANBOXSESSID'):\n"
            "                    parts = [f'{k}={v}' for k, v in fanbox_cookies.items()]\n"
            "                    found = {\n"
            "                        'sessid': fanbox_cookies['FANBOXSESSID'],\n"
            "                        'full': '; '.join(parts),\n"
            "                        'count': len(fanbox_cookies),\n"
            "                        'browser': fn_name\n"
            "                    }\n"
            "                    break\n"
            "            except Exception as e:\n"
            "                errors.append(f'{fn_name}({dom or \"all\"}): {type(e).__name__}: {str(e)[:60]}')\n"
            "                continue\n"
            "        if found: break\n"
            "    if found:\n"
            "        print(json.dumps(found))\n"
            "    else:\n"
            "        msg = 'FANBOXSESSID 못 찾음. Chrome 에서 fanbox.cc 직접 로그인 확인.'\n"
            "        if diag: msg += ' [scan: ' + '; '.join(diag[:5]) + ']'\n"
            "        if errors: msg += ' [errors: ' + '; '.join(errors[:3]) + ']'\n"
            "        print(json.dumps({'error': msg}))\n"
            "except Exception as e:\n"
            "    print(json.dumps({'error': f'{type(e).__name__}: {e}'}))\n"
        });
        proc.waitForFinished(30000);
        QString outRaw = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        QString errRaw = QString::fromUtf8(proc.readAllStandardError()).trimmed();
        QJsonObject r = QJsonDocument::fromJson(outRaw.toUtf8()).object();
        QString sid = r["sessid"].toString();
        QString full = r["full"].toString();
        int cnt = r["count"].toInt();
        QString br = r["browser"].toString();
        QString err = r["error"].toString();
        if (r.isEmpty() && !outRaw.isEmpty()) err = "JSON parse fail: " + outRaw.left(150);
        if (r.isEmpty() && outRaw.isEmpty()) err = "Python 출력 없음 — " + (errRaw.isEmpty() ? QString("타임아웃 또는 환경 오류") : errRaw.left(200));
        QMetaObject::invokeMethod(this, [this, sid, full, cnt, br, err]() {
            runJs("var b=document.getElementById('fanbox-session-refresh-btn');if(b){b.disabled=false;if(b.dataset._origText){b.textContent=b.dataset._origText;}}");
            if (!err.isEmpty()) {
                log("Fanbox 세션 추출 실패: " + err, "warning", "settings");
                log("  → 수동 방법: Chrome → fanbox.cc 로그인 → F12 → Application → Cookies → FANBOXSESSID 값 복사 → Fanbox 탭에 붙여넣기", "info", "settings");
                return;
            }
            log(QString("✅ Fanbox 추출 성공 [%1] 쿠키 %2개").arg(br).arg(cnt), "success", "settings");
            log(QString("  FANBOXSESSID: %1...").arg(sid.left(10)), "info", "settings");
            QString jSid = Common::jsStringLiteral(sid);
            QString jFull = Common::jsStringLiteral(full);
            runJs(QString(
                "var s=document.getElementById('fanbox-session');if(s){s.value=%1;s.dispatchEvent(new Event('input'));s.dispatchEvent(new Event('change'));}"
                "var c=document.getElementById('fanbox-cookie');if(c){c.value=%2;c.dispatchEvent(new Event('input'));c.dispatchEvent(new Event('change'));}"
                "if(typeof saveFormDataToBackend==='function') saveFormDataToBackend();"
                "else if(typeof saveForm==='function') saveForm();"
            ).arg(jSid, jFull));
        }, Qt::QueuedConnection);
    });
    connect(t, &QThread::finished, t, &QThread::deleteLater);
    t->start();
}

void MiyoBackend::runFanboxCollection(const QJsonObject &config)
{
    CollectionGuard _cg(platformSem("fanbox"), this, "fanbox");
    setIntegrityActiveForPlatform("fanbox", config["integrityCheck"].toBool(false));

    QString target = config["target"].toString().trimmed();
    if (target.startsWith("@")) target = target.mid(1);
    // URL 도 처리: https://www.fanbox.cc/@user 또는 https://user.fanbox.cc
    if (target.contains("fanbox.cc/")) {
        target = target.section("fanbox.cc/", 1).section('/', 0, 0).section('?', 0, 0);
        if (target.startsWith("@")) target = target.mid(1);
    } else if (target.contains(".fanbox.cc")) {
        target = target.section(".fanbox.cc", 0, 0).section("//", 1).section('.', -1);
    }
    if (target.isEmpty()) {
        log("Fanbox: creator ID 필요 (예: \"username\" 또는 https://www.fanbox.cc/@user)", "error", "fanbox");
        return;
    }

    QString cookie = config["cookie"].toString();
    // ★ sessionId 만 있어도 OK — cookie 자동 빌드
    if (!cookie.contains("FANBOXSESSID")) {
        QString sid = config["sessionId"].toString();
        if (sid.isEmpty() && !config["accounts"].toArray().isEmpty()) {
            sid = config["accounts"].toArray()[0].toObject()["sessionId"].toString();
        }
        if (!sid.isEmpty()) {
            cookie = QString("FANBOXSESSID=%1").arg(sid);
            if (!config["cookie"].toString().isEmpty()) cookie += "; " + config["cookie"].toString();
        }
    }
    if (cookie.isEmpty() || !cookie.contains("FANBOXSESSID")) {
        log("Fanbox: FANBOXSESSID 필요 — 설정 → 토큰 자동 추출 → Fanbox 또는 fanbox 탭에서 직접 입력", "error", "fanbox");
        return;
    }

    QString savePath = config["path"].toString();
    savePath.replace("~", QDir::homePath());
    QString creatorDir = savePath + "/fanbox/" + target;
    QDir().mkpath(creatorDir);
    QString mediaDir = creatorDir + "/media";
    QString postsDir = creatorDir + "/posts";
    QDir().mkpath(mediaDir);
    QDir().mkpath(postsDir);

    bool saveExcel = config["excel"].toBool(true);
    bool downloadMedia = config["downloadMedia"].toBool(true);
    int maxPosts = config["count"].toInt(0);

    QString bufTmp = Common::resolveTempBase(m_config ? m_config->tempDir() : QString()) + "/abiwa_fanbox_" + target;
    QDir().mkpath(bufTmp);
    DiskJsonBuffer allPosts(bufTmp, "fanbox");

    HttpClient http;
    http.setRunFlag(&m_isRunning["fanbox"]);
    QMap<QString, QString> headers;
    headers["User-Agent"] = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36";
    headers["Cookie"] = cookie;
    headers["Origin"] = "https://www.fanbox.cc";
    headers["Referer"] = "https://www.fanbox.cc/";

    log(QString("━━ Fanbox 수집 시작: @%1 ━━").arg(target), "info", "fanbox");

    QString nextUrl = QString("https://api.fanbox.cc/post.listCreator?creatorId=%1&limit=10").arg(target);
    int postCount = 0;
    int mediaCount = 0;
    int page = 0;

    while (!nextUrl.isEmpty() && m_isRunning.value("fanbox", false)) {
        HttpResponse resp = http.get(nextUrl, headers);
        if (!resp.isOk()) {
            if (resp.statusCode == 401 || resp.statusCode == 403) {
                log(QString("인증 실패 (HTTP %1) — FANBOXSESSID 만료. 토큰 재추출 필요").arg(resp.statusCode), "error", "fanbox");
            } else {
                log(QString("listCreator 실패 (HTTP %1)").arg(resp.statusCode), "error", "fanbox");
            }
            break;
        }
        QJsonObject body = resp.json()["body"].toObject();
        QJsonArray items = body["items"].toArray();
        nextUrl = body["nextUrl"].toString();
        page++;
        log(QString("페이지 %1 — 포스트 %2개").arg(page).arg(items.size()), "info", "fanbox");

        for (const auto &v : items) {
            if (!m_isRunning.value("fanbox", false)) break;
            QJsonObject post = v.toObject();
            QString postId = post["id"].toString();
            QString title = post["title"].toString();
            int fee = post["feeRequired"].toInt(0);
            bool isRestricted = post["isRestricted"].toBool(false);
            postCount++;
            updateStats(postCount, mediaCount, "수집 중...", "fanbox");

            if (isRestricted) {
                log(QString("[%1] %2 (₩%3 구독 필요 — skip)").arg(postCount).arg(title.left(40)).arg(fee), "warning", "fanbox");
                continue;
            }

            // post 상세 가져오기
            QString detailUrl = QString("https://api.fanbox.cc/post.info?postId=%1").arg(postId);
            HttpResponse detResp = http.get(detailUrl, headers);
            if (!detResp.isOk()) {
                log(QString("[%1] post.info 실패 (HTTP %2)").arg(postCount).arg(detResp.statusCode), "warning", "fanbox");
                continue;
            }
            QJsonObject detBody = detResp.json()["body"].toObject();
            QJsonObject content = detBody["body"].toObject();

            log(QString("[%1] %2 (₩%3)").arg(postCount).arg(title.left(40)).arg(fee), "info", "fanbox");

            // 이미지/파일 다운로드
            if (downloadMedia) {
                QString postDir = mediaDir + "/" + postId;
                QDir().mkpath(postDir);
                // imageMap
                QJsonObject imgMap = content["imageMap"].toObject();
                for (auto it = imgMap.constBegin(); it != imgMap.constEnd(); ++it) {
                    QJsonObject img = it.value().toObject();
                    QString origUrl = img["originalUrl"].toString();
                    QString ext = img["extension"].toString();
                    if (origUrl.isEmpty()) continue;
                    QString fname = QString("%1.%2").arg(it.key()).arg(ext);
                    QString out = postDir + "/" + fname;
                    if (QFile::exists(out)) { mediaCount++; continue; }
                    QProcess curl;
                    curl.start("curl", {"-sSL", "-H", "Origin: https://www.fanbox.cc",
                                        "-H", "Referer: https://www.fanbox.cc/", "-H", "Cookie: " + cookie,
                                        "-o", out, origUrl, "--max-time", "60"});
                    if (curl.waitForFinished(65000) && curl.exitCode() == 0 && QFileInfo(out).size() > 100) {
                        mediaCount++;
                        FileHelper::setDownloadMeta(out, detailUrl);
                        enqueueWebDavUpload(out);
                    }
                }
                // fileMap (zip/pdf 등)
                QJsonObject fileMap = content["fileMap"].toObject();
                for (auto it = fileMap.constBegin(); it != fileMap.constEnd(); ++it) {
                    QJsonObject f = it.value().toObject();
                    QString url = f["url"].toString();
                    QString name = f["name"].toString();
                    QString ext = f["extension"].toString();
                    if (url.isEmpty()) continue;
                    QString fname = name.isEmpty() ? QString("%1.%2").arg(it.key()).arg(ext) : (name + "." + ext);
                    QString out = postDir + "/" + fname;
                    if (QFile::exists(out)) { mediaCount++; continue; }
                    QProcess curl;
                    curl.start("curl", {"-sSL", "-H", "Origin: https://www.fanbox.cc",
                                        "-H", "Referer: https://www.fanbox.cc/", "-H", "Cookie: " + cookie,
                                        "-o", out, url, "--max-time", "120"});
                    if (curl.waitForFinished(125000) && curl.exitCode() == 0 && QFileInfo(out).size() > 100) {
                        mediaCount++;
                        FileHelper::setDownloadMeta(out, detailUrl);
                        enqueueWebDavUpload(out);
                    }
                }
            }

            // 메타 저장
            QJsonObject row;
            row["id"] = postId;
            row["title"] = title;
            row["fee"] = fee;
            row["url"] = QString("https://www.fanbox.cc/@%1/posts/%2").arg(target, postId);
            row["published"] = post["publishedDatetime"].toString();
            row["type"] = post["type"].toString();
            row["like_count"] = post["likeCount"].toInt(0);
            row["comment_count"] = post["commentCount"].toInt(0);
            row["cover_image"] = post["coverImageUrl"].toString();
            allPosts.append(row);

            // 포스트 JSON 저장 (post 자체 백업)
            QFile pj(postsDir + "/" + postId + ".json");
            if (pj.open(QIODevice::WriteOnly)) {
                pj.write(QJsonDocument(detBody).toJson(QJsonDocument::Indented));
                pj.close();
            }

            QThread::msleep(500);  // rate limit 회피
            if (maxPosts > 0 && postCount >= maxPosts) { nextUrl.clear(); break; }
        }
    }

    // Excel 저장
    if (saveExcel && allPosts.count() > 0) {
        QString xlsxPath = creatorDir + "/" + target + "_fanbox.xlsx";
        QXlsx::Document doc;
        QStringList headers2 = {"id","title","fee","url","published","type","like_count","comment_count","cover_image"};
        for (int c = 0; c < headers2.size(); ++c) doc.write(1, c+1, headers2[c]);
        int row = 2;
        QJsonArray all = allPosts.readAll();
        for (const auto &v : all) {
            QJsonObject o = v.toObject();
            int c = 1;
            for (const QString &h : headers2) {
                QJsonValue val = o[h];
                doc.write(row, c++, val.isString() ? val.toString() : QString::number(val.toDouble()));
            }
            row++;
        }
        if (doc.saveAs(xlsxPath)) {
            log(QString("✅ Excel 저장: %1").arg(QFileInfo(xlsxPath).fileName()), "success", "fanbox");
            enqueueWebDavUpload(xlsxPath);
        }
    }

    log(QString("━━ Fanbox 완료: 포스트 %1 / 미디어 %2 ━━").arg(postCount).arg(mediaCount), "success", "fanbox");
    updateStats(postCount, mediaCount, "완료", "fanbox");
}

void MiyoBackend::refreshDiscordToken()
{
    log("Chrome에서 Discord 토큰 추출 중...", "info", "settings");
    runJs("setDiscordRefreshing(true)");

    QThread *thread = QThread::create([this]() {
        QString python = Common::bundledPythonPath();
        QProcessEnvironment env = Common::bundledProcessEnv();

        QProcess proc;
        proc.setProcessEnvironment(env);
        proc.start(python, {"-c",
            "import json, sys, os, re, glob\n"
            "try:\n"
            "    token = ''\n"
            "    token_re = re.compile(r'[\\\"\\']([\\w-]{24,26}\\.[\\w-]{6}\\.[\\w-]{25,110})[\\\"\\']')\n"
            "    mfa_re = re.compile(r'mfa\\.[\\w-]{80,}')\n"
            "    # 검색 대상: 모든 주요 브라우저 + Discord 데스크톱 앱들\n"
            "    search_dirs = []\n"
            "    if sys.platform == 'darwin':\n"
            "        home = os.path.expanduser('~/Library/Application Support')\n"
            "        browsers = [\n"
            "            'Google/Chrome', 'Google/Chrome Beta', 'Google/Chrome Canary',\n"
            "            'Microsoft Edge', 'BraveSoftware/Brave-Browser', 'Arc/User Data',\n"
            "            'Vivaldi', 'com.operasoftware.Opera', 'Chromium'\n"
            "        ]\n"
            "        for b in browsers:\n"
            "            for profile in glob.glob(f'{home}/{b}/Default') + glob.glob(f'{home}/{b}/Profile *'):\n"
            "                search_dirs.append(os.path.join(profile, 'Local Storage', 'leveldb'))\n"
            "        # Discord desktop apps\n"
            "        for app in ['discord', 'discordcanary', 'discordptb']:\n"
            "            search_dirs += glob.glob(f'{home}/{app}/Local Storage/leveldb')\n"
            "    elif sys.platform == 'win32':\n"
            "        la = os.environ.get('LOCALAPPDATA', '')\n"
            "        rm = os.environ.get('APPDATA', '')\n"
            "        browsers = [\n"
            "            f'{la}\\\\Google\\\\Chrome\\\\User Data', f'{la}\\\\Google\\\\Chrome Beta\\\\User Data',\n"
            "            f'{la}\\\\Microsoft\\\\Edge\\\\User Data', f'{la}\\\\BraveSoftware\\\\Brave-Browser\\\\User Data',\n"
            "            f'{la}\\\\Vivaldi\\\\User Data', f'{la}\\\\Chromium\\\\User Data'\n"
            "        ]\n"
            "        for b in browsers:\n"
            "            for profile in glob.glob(f'{b}\\\\Default') + glob.glob(f'{b}\\\\Profile *'):\n"
            "                search_dirs.append(os.path.join(profile, 'Local Storage', 'leveldb'))\n"
            "        for app in ['discord', 'discordcanary', 'discordptb']:\n"
            "            search_dirs += glob.glob(f'{rm}\\\\{app}\\\\Local Storage\\\\leveldb')\n"
            "    else:\n"
            "        print(json.dumps({'error': 'Unsupported OS'}))\n"
            "        sys.exit(0)\n"
            "    candidates = []\n"
            "    for d in search_dirs:\n"
            "        if not os.path.isdir(d): continue\n"
            "        for ext in ['*.ldb', '*.log']:\n"
            "            for fpath in glob.glob(os.path.join(d, ext)):\n"
            "                try:\n"
            "                    with open(fpath, 'rb') as f:\n"
            "                        data = f.read().decode('utf-8', errors='ignore')\n"
            "                    for m in token_re.finditer(data):\n"
            "                        t = m.group(1)\n"
            "                        parts = t.split('.')\n"
            "                        if len(parts) == 3 and len(parts[0]) >= 18:\n"
            "                            candidates.append(t)\n"
            "                    for m in mfa_re.finditer(data):\n"
            "                        candidates.append(m.group(0))\n"
            "                except: pass\n"
            "    if candidates:\n"
            "        token = max(candidates, key=len)\n"
            "    if token:\n"
            "        print(json.dumps({'token': token}))\n"
            "    else:\n"
            "        print(json.dumps({'error': 'Discord 토큰을 찾을 수 없습니다. 브라우저 또는 Discord 앱에 로그인하세요.'}))\n"
            "except Exception as e:\n"
            "    print(json.dumps({'error': str(e)}))\n"
        });
        proc.waitForFinished(30000);

        QString output = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        QJsonObject result = QJsonDocument::fromJson(output.toUtf8()).object();
        QString token = result.contains("error") ? QString() : result["token"].toString();
        QString error = result["error"].toString();

        QMetaObject::invokeMethod(this, [this, token, error]() {
            runJs("setDiscordRefreshing(false)");
            if (!error.isEmpty()) {
                log("Discord 토큰 추출 실패: " + error, "warning", "settings");
                return;
            }
            log("✅ Discord 토큰 추출 성공!", "success", "settings");
            log(QString("  token: %1...").arg(token.left(12)), "info", "settings");

            QString escapedToken = token;
            escapedToken.replace("'", "\\'");
            QString js = QString(
                "if (!accounts.discord) accounts.discord = [];"
                "if (accounts.discord.length > 0) {"
                "  accounts.discord[0].token = '%1';"
                "  appendLog('✅ Discord 토큰 갱신됨', 'success', 'settings');"
                "} else {"
                "  accounts.discord.push({name:'Chrome', token:'%1'});"
                "  appendLog('✅ Discord Chrome 계정 자동 추가됨', 'success', 'settings');"
                "}"
                "renderAccounts('discord'); saveConfig();"
            ).arg(escapedToken);
            runJs(js);
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 범용 Chrome 도메인 쿠키 추출기
// 특정 도메인의 모든 쿠키를 복호화 → "name=value; name=value" 형태로 HTML 필드에 주입
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
void MiyoBackend::refreshDomainCookies(const QString &domain, const QString &fieldId,
                                        const QString &platform, const QString &label,
                                        const QString &busyJsFn)
{
    log(QString("Chrome에서 %1 쿠키 추출 중...").arg(label), "info", platform);
    if (!busyJsFn.isEmpty()) runJs(busyJsFn + "(true)");

    QThread *thread = QThread::create([this, domain, fieldId, platform, label, busyJsFn]() {
        QString python = Common::bundledPythonPath();
        QProcessEnvironment env = Common::bundledProcessEnv();

        // Python 스크립트에 파라미터 전달 — argv[1] = domain
        QProcess proc;
        proc.setProcessEnvironment(env);
        proc.start(python, {"-c",
            "import json, sys\n"
            "domain = sys.argv[1]\n"
            "try:\n"
            "    import browser_cookie3 as bc3\n"
            "    cookies = {}\n"
            "    used_browser = ''\n"
            "    errors = []\n"
            "    import sys\n"
            "    _browsers_mac = ['firefox', 'librewolf', 'chrome', 'edge', 'brave', 'arc', 'vivaldi', 'opera']\n"
            "    _browsers_pc  = ['chrome', 'edge', 'brave', 'arc', 'opera', 'opera_gx', 'vivaldi', 'firefox', 'librewolf', 'chromium']\n"
            "    browsers = _browsers_mac if sys.platform == 'darwin' else _browsers_pc\n"
            "    for fn_name in browsers:\n"
            "        fn = getattr(bc3, fn_name, None)\n"
            "        if not fn: continue\n"
            "        try:\n"
            "            cj = fn(domain_name=domain)\n"
            "            tmp = {c.name: c.value for c in cj if c.value}\n"
            "            if tmp:\n"
            "                cookies = tmp\n"
            "                used_browser = fn_name\n"
            "                break\n"
            "        except Exception as e:\n"
            "            errors.append(f'{fn_name}: {type(e).__name__}')\n"
            "    if cookies:\n"
            "        cookie_str = '; '.join(f'{k}={v}' for k, v in cookies.items())\n"
            "        print(json.dumps({'cookie': cookie_str, 'count': len(cookies), 'browser': used_browser}))\n"
            "    else:\n"
            "        print(json.dumps({'error': f'{domain} 쿠키 없음. 브라우저 로그인 확인. (' + '; '.join(errors[:3]) + ')'}))\n"
            "except Exception as e:\n"
            "    print(json.dumps({'error': f'{type(e).__name__}: {e}'}))\n",
            domain
        });
        proc.waitForFinished(30000);

        QString output = QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
        QJsonObject result = QJsonDocument::fromJson(output.toUtf8()).object();
        QString cookie = result["cookie"].toString();
        QString error = result["error"].toString();
        int count = result["count"].toInt();

        QMetaObject::invokeMethod(this, [this, cookie, error, count, fieldId, platform, label, busyJsFn]() {
            if (!busyJsFn.isEmpty()) runJs(busyJsFn + "(false)");
            if (!error.isEmpty() || cookie.isEmpty()) {
                log(QString("%1 쿠키 추출 실패: %2").arg(label, error.isEmpty() ? "쿠키 없음" : error),
                    "warning", platform);
                return;
            }
            log(QString("✅ %1 쿠키 추출 성공! (%2개)").arg(label).arg(count), "success", platform);
            log(QString("  cookie: %1...").arg(cookie.left(60)), "info", platform);

            // JS escape: backslash + single quote
            QString escaped = cookie;
            escaped.replace("\\", "\\\\").replace("'", "\\'");
            QString js = QString(
                "(function(){var el=document.getElementById('%1');"
                "if(el){el.value='%2';"
                "if(typeof saveFormData==='function')saveFormData();"
                "appendLog('✅ %3 쿠키 자동 입력됨','success','%4');}"
                "else{appendLog('필드 %1 없음','warning','%4');}})();"
            ).arg(fieldId, escaped, label, platform);
            runJs(js);
        }, Qt::QueuedConnection);
    });
    connect(thread, &QThread::finished, thread, &QThread::deleteLater);
    thread->start();
}

void MiyoBackend::refreshTumblrCookie()
{
    // Tumblr API는 developer API Key(consumer key)가 필수 → 브라우저에서 추출 불가
    // 대신 tumblr.com 세션 쿠키를 추출해서 참고용으로 저장 (로그인 상태 유지시 내부 API 활용 가능)
    log("Tumblr: 공식 API는 Consumer Key가 필요합니다 (수동 입력).", "info", "tumblr");
    log("  https://www.tumblr.com/oauth/apps 에서 발급받으세요.", "info", "tumblr");
    // tumblr.com 쿠키도 추출 (향후 웹 스크래핑 대체 경로용)
    refreshDomainCookies("tumblr.com", "tumblr-apikey-cookie-hint", "tumblr", "Tumblr", "setTumblrRefreshing");
}

void MiyoBackend::refreshSpinSpinCookie()
{
    refreshDomainCookies("spin-spin.com", "spinspin-cookie", "spinspin", "SpinSpin", "setSpinSpinRefreshing");
}

void MiyoBackend::refreshAskedCookie()
{
    refreshDomainCookies("asked.kr", "asked-cookie", "asked", "Asked", "setAskedRefreshing");
}

void MiyoBackend::refreshAllTokens()
{
    refreshTwitterTokens();
    refreshInstagramSession();
    refreshPixivSession();
    refreshFanboxSession();      // ★ Fanbox 도 같이 자동 추출 (FANBOXSESSID + cookie)
    refreshDiscordToken();
    refreshSpinSpinCookie();
    refreshAskedCookie();
    refreshTumblrCookie();
    // Bluesky: handle + 앱 비밀번호 → 브라우저에 저장되지 않음 (수동 입력 필수)
    log("Bluesky: 핸들 + 앱 비밀번호는 브라우저에 저장되지 않아 자동 추출 불가. 수동 입력 필요.", "info", "settings");
}

void MiyoBackend::writeStartupLog()
{
    QString logDir = m_config->tempDir();
    if (logDir.isEmpty()) logDir = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/ABIWA";
    QDir().mkpath(logDir);

    QString logPath = logDir + "/abiwa_system.log";
    QFile f(logPath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return;

    QTextStream ts(&f);
    ts << "═══════════════════════════════════\n";
    ts << "  ABIWA System Log\n";
    ts << "  " << QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss") << "\n";
    ts << "═══════════════════════════════════\n\n";

    ts << "App Dir: " << QCoreApplication::applicationDirPath() << "\n";
    ts << "Temp Dir: " << (m_config->tempDir().isEmpty() ? "(not set)" : m_config->tempDir()) << "\n";

    if (!m_config->tempDir().isEmpty()) {
        QStorageInfo storage(m_config->tempDir());
        if (storage.isValid()) {
            ts << "Disk: " << storage.displayName()
               << " (Free: " << (storage.bytesAvailable() / (1024*1024)) << " MB"
               << " / Total: " << (storage.bytesTotal() / (1024*1024)) << " MB)\n";
        }
    }

#ifdef Q_OS_MACOS
    ts << "OS: macOS\n";
#elif defined(Q_OS_WIN)
    ts << "OS: Windows\n";
#else
    ts << "OS: Linux\n";
#endif
    ts << "Qt: " << QT_VERSION_STR << "\n";

    QString python = Common::bundledPythonPath();
    ts << "Python: " << python << " (exists: " << (QFile::exists(python) ? "yes" : "no") << ")\n";

    // Get package versions
    {
        QProcess proc;
        proc.setProcessEnvironment(Common::bundledProcessEnv());
        proc.start(python, {"-m", "pip", "list", "--format=columns"});
        proc.waitForFinished(15000);
        ts << "\n── Installed Packages ──\n";
        ts << QString::fromUtf8(proc.readAllStandardOutput()) << "\n";
    }

    // Bundled tools
    ts << "── Bundled Tools ──\n";
    QString appDir = QCoreApplication::applicationDirPath();
    for (const QString &tool : {"yt-dlp", "ffmpeg", "ffprobe"}) {
#ifdef Q_OS_WIN
        QString path = appDir + "/" + tool + ".exe";
#else
        QString path = appDir + "/" + tool;
#endif
        QFileInfo fi(path);
        ts << "  " << tool << ": " << (fi.exists() ? QString("%1 MB").arg(fi.size()/(1024.0*1024.0), 0, 'f', 1) : "(missing)") << "\n";
    }

    f.close();
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ── Tumblr Collector (API v2) — Twitter 스타일 저장 ──
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void MiyoBackend::runTumblrCollection(const QJsonObject &config)
{
    CollectionGuard _cg(platformSem("tumblr"), this, "tumblr");
    setIntegrityActiveForPlatform("tumblr", config["integrityCheck"].toBool(false));
    if (config["method"].toString() == "chrome") {
        runRealChromeCollection(config);
        return;
    }
    if (config["method"].toString() == "web") {
        runWebCrawlCollection(config);
        return;
    }
    QString apiKey = config["apiKey"].toString();
    QString blogName = config["target"].toString().trimmed();
    QString savePath = config["path"].toString();
    savePath.replace("~", QDir::homePath());
    bool saveExcel = config["excel"].toBool(true);
    bool downloadMediaFlag = config["downloadMedia"].toBool(true);
    int maxCount = config["count"].toInt(0);

    // 계정 로테이션용 API key 목록
    QStringList apiKeys;
    QJsonArray accs = config["accounts"].toArray();
    for (const auto &a : accs) {
        QString k = a.toObject()["apiKey"].toString().trimmed();
        if (!k.isEmpty()) apiKeys << k;
    }
    if (apiKeys.isEmpty() && !apiKey.isEmpty()) apiKeys << apiKey;
    int currentKeyIdx = 0;
    if (!apiKeys.isEmpty()) apiKey = apiKeys[0];

    // Parse blog name from URL
    QRegularExpression blogUrlRe(R"((?:https?://)?([^./]+)\.tumblr\.com)");
    auto match = blogUrlRe.match(blogName);
    if (match.hasMatch()) blogName = match.captured(1);
    QRegularExpression blogUrlRe2(R"(tumblr\.com/([^/?]+))");
    auto match2 = blogUrlRe2.match(blogName);
    if (match2.hasMatch()) blogName = match2.captured(1);

    if (apiKey.isEmpty()) {
        log("API Key를 입력하세요.", "error", "tumblr");
        return;
    }
    if (blogName.isEmpty()) {
        log("블로그 이름을 입력하세요.", "error", "tumblr");
        return;
    }

    log(QString("Tumblr 수집 시작: %1").arg(blogName), "info", "tumblr");

    HttpClient http;
    http.setTimeout(30000);
    http.setDownloadTimeout(120000);
    http.setRunFlag(&m_isRunning["tumblr"]);  // 중지 시 즉시 abort

    QString blogId = blogName + ".tumblr.com";

    // Get blog info
    QString infoUrl = QString("https://api.tumblr.com/v2/blog/%1/info?api_key=%2").arg(blogId, apiKey);
    HttpResponse infoResp = http.get(infoUrl);
    if (!infoResp.isOk()) {
        log(QString("블로그 정보 가져오기 실패 (HTTP %1)").arg(infoResp.statusCode), "error", "tumblr");
        if (infoResp.statusCode == 401) log("API Key가 잘못되었습니다.", "error", "tumblr");
        if (infoResp.statusCode == 404) log("블로그를 찾을 수 없습니다.", "error", "tumblr");
        return;
    }

    QJsonObject blogInfo = infoResp.json()["response"].toObject()["blog"].toObject();
    QString blogTitle = blogInfo["title"].toString();
    int totalPosts = blogInfo["total_posts"].toInt();

    log(QString("블로그: %1 (%2) — 전체 %3 포스트").arg(blogTitle, blogName).arg(totalPosts), "success", "tumblr");

    // ── 프로필 저장 (아바타/배너 + profile.json) ── 날짜별 아카이브
    {
        QString tumblrProfileDir = savePath + "/tumblr/" + sanitizeFilename(blogName, 100);
        QDir().mkpath(tumblrProfileDir);
        QString dateTag = QDate::currentDate().toString("yyyyMMdd");
        // 아바타
        QString avatarUrl = blogInfo["avatar"].toArray().isEmpty() ? QString()
            : blogInfo["avatar"].toArray().first().toObject()["url"].toString();
        if (avatarUrl.isEmpty()) {
            // fallback: standard avatar endpoint
            avatarUrl = QString("https://api.tumblr.com/v2/blog/%1/avatar/512").arg(blogId);
        }
        if (!avatarUrl.isEmpty()) {
            QString avatarPath = tumblrProfileDir + "/avatar_" + dateTag + ".jpg";
            if (!QFile::exists(avatarPath)) {
                http.downloadFile(avatarUrl, avatarPath);
            }
        }
        // profile.json
        QJsonObject profileData;
        profileData["name"] = blogName;
        profileData["title"] = blogTitle;
        profileData["description"] = blogInfo["description"].toString();
        profileData["total_posts"] = totalPosts;
        profileData["url"] = blogInfo["url"].toString();
        profileData["updated"] = blogInfo["updated"].toVariant().toLongLong();
        profileData["fetched_at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
        QString profileJsonPath = tumblrProfileDir + "/profile.json";
        QFile pf(profileJsonPath);
        if (pf.open(QIODevice::WriteOnly)) {
            pf.write(QJsonDocument(profileData).toJson(QJsonDocument::Indented));
            pf.close();
        }
    }

    // 공통 저장 정책: savePath/tumblr/{blogname}/{type}/
    //   type ∈ {photo, video, audio, _complete}
    //   excel: savePath/tumblr/{blogname}/excel/{blog}_{type}.xlsx
    QString tumblrDir = savePath + "/tumblr";
    QString blogDir = tumblrDir + "/" + sanitizeFilename(blogName, 100);
    QDir().mkpath(blogDir);
    QString photoDir = FileHelper::typeFolder(blogDir, "photo");
    QString videoDir = FileHelper::typeFolder(blogDir, "video");
    QString audioDir = FileHelper::typeFolder(blogDir, "audio");
    QString completeDir = FileHelper::typeFolder(blogDir, "complete");
    QString excelDir = blogDir + "/excel";
    if (saveExcel) QDir().mkpath(excelDir);

    // DiskJsonBuffer for streaming
    QString bufTmpDir;
    {
        QString userTmp = m_config ? m_config->tempDir() : QString();
        if (!userTmp.isEmpty() && QDir(userTmp).exists()) bufTmpDir = userTmp + "/abiwa_tumblr_" + blogName;
        else bufTmpDir = blogDir + "/.abiwa_tmp";
    }
    QDir().mkpath(bufTmpDir);
    DiskJsonBuffer allPosts(bufTmpDir, "tumblr");
    int mediaDownloaded = 0;
    int offset = 0;
    int limit = 20;

    while (platformRunning("tumblr")) {
        QString postsUrl = QString("https://api.tumblr.com/v2/blog/%1/posts?api_key=%2&offset=%3&limit=%4&npf=true")
            .arg(blogId, apiKey).arg(offset).arg(limit);

        HttpResponse postsResp = http.get(postsUrl);
        if (!postsResp.isOk()) {
            if (postsResp.statusCode == 429) {
                // 계정 로테이션: 다른 API key가 있으면 전환
                if (apiKeys.size() > 1) {
                    currentKeyIdx = (currentKeyIdx + 1) % apiKeys.size();
                    apiKey = apiKeys[currentKeyIdx];
                    log(QString("Rate Limit → API Key 전환 (%1/%2)").arg(currentKeyIdx + 1).arg(apiKeys.size()), "warning", "tumblr");
                    QThread::sleep(3);
                    continue;
                }
                log("Rate Limit — 60초 대기...", "warning", "tumblr");
                for (int w = 60; w > 0 && platformRunning("tumblr"); --w) {
                    updateStats(allPosts.count(), mediaDownloaded, QString("대기 %1s").arg(w), "tumblr");
                    QThread::sleep(1);
                }
                continue;
            }
            log(QString("포스트 가져오기 실패 (HTTP %1)").arg(postsResp.statusCode), "error", "tumblr");
            break;
        }

        QJsonArray posts = postsResp.json()["response"].toObject()["posts"].toArray();
        if (posts.isEmpty()) break;

        for (const auto &postVal : posts) {
            if (!platformRunning("tumblr")) break;
            QJsonObject post = postVal.toObject();
            allPosts.append(post);

            if (downloadMediaFlag) {
                QString postId = QString::number(post["id"].toVariant().toLongLong());
                qint64 ts = post["timestamp"].toVariant().toLongLong();
                QDateTime postDt;
                QString orderPrefix;
                if (ts > 0) {
                    postDt = QDateTime::fromSecsSinceEpoch(ts, QTimeZone::utc());
                    orderPrefix = FileHelper::uploadOrderPrefix(ts);
                }
                QString summary = post["summary"].toString().left(40).trimmed().replace('\n', ' ');
                QString cleanSummary = sanitizeFilename(summary, 40);
                if (cleanSummary.isEmpty()) cleanSummary = postId;
                QString postUrl = post["post_url"].toString();

                auto downloadToBoth = [&](const QString &url, const QString &typeDir, const QString &ext,
                                          const QString &tag, int idx) {
                    if (url.isEmpty()) return;
                    QString fname = QString("%1%2 (%3_%4%5).%6")
                                        .arg(orderPrefix, cleanSummary, postId).arg(tag).arg(idx).arg(ext);
                    fname = sanitizeFilename(fname, 200);
                    QString typePath = typeDir + "/" + fname;
                    QString completePath = completeDir + "/" + fname;
                    if (!QFile::exists(typePath) && http.downloadFile(url, typePath)) {
                        Common::addExifMetadata(typePath, blogName, summary,
                            QString("Tumblr/%1").arg(blogName), postUrl, post["date"].toString());
                        FileHelper::setFinderComment(typePath, postUrl);
                        FileHelper::applyPostMetadata(typePath, postDt, postUrl);
                        // mirror to _complete
                        if (!QFile::exists(completePath)) {
                            QFile::copy(typePath, completePath);
                            FileHelper::setFinderComment(completePath, postUrl);
                            FileHelper::applyPostMetadata(completePath, postDt, postUrl);
                        }
                        mediaDownloaded++;
                    }
                };

                // NPF content blocks — extract media
                QJsonArray content = post["content"].toArray();
                int imgIdx = 0, vIdx = 0, aIdx = 0;
                for (const auto &blockVal : content) {
                    QJsonObject block = blockVal.toObject();
                    QString btype = block["type"].toString();

                    if (btype == "image") {
                        QJsonArray media = block["media"].toArray();
                        if (!media.isEmpty()) {
                            QString imgUrl = media[0].toObject()["url"].toString();
                            int maxW = media[0].toObject()["width"].toInt();
                            for (const auto &m : media) {
                                int w = m.toObject()["width"].toInt();
                                if (w > maxW) { imgUrl = m.toObject()["url"].toString(); maxW = w; }
                            }
                            QString ext = "jpg";
                            if (imgUrl.contains(".png")) ext = "png";
                            else if (imgUrl.contains(".gif")) ext = "gif";
                            else if (imgUrl.contains(".webp")) ext = "webp";
                            downloadToBoth(imgUrl, photoDir, ext, "P", imgIdx++);
                        }
                    } else if (btype == "video") {
                        QString videoUrl = block["url"].toString();
                        if (videoUrl.isEmpty()) videoUrl = block["media"].toObject()["url"].toString();
                        downloadToBoth(videoUrl, videoDir, "mp4", "V", vIdx++);
                    } else if (btype == "audio") {
                        QString audioUrl = block["url"].toString();
                        if (audioUrl.isEmpty()) audioUrl = block["media"].toObject()["url"].toString();
                        downloadToBoth(audioUrl, audioDir, "mp3", "A", aIdx++);
                    }
                }

                // Legacy (non-NPF) fallback
                if (content.isEmpty()) {
                    QJsonArray photos = post["photos"].toArray();
                    for (int pi = 0; pi < photos.size(); ++pi) {
                        QString photoUrl = photos[pi].toObject()["original_size"].toObject()["url"].toString();
                        QString ext = "jpg";
                        if (photoUrl.contains(".png")) ext = "png";
                        else if (photoUrl.contains(".gif")) ext = "gif";
                        downloadToBoth(photoUrl, photoDir, ext, "P", pi);
                    }
                    QString videoUrl = post["video_url"].toString();
                    if (!videoUrl.isEmpty()) downloadToBoth(videoUrl, videoDir, "mp4", "V", 0);
                }

                // 経済産業省 연계: SingleFile CDP 캡쳐
                // ★ 미디어 유무 무관 — 모든 포스트 캡쳐
                if (!postUrl.isEmpty() && config["realCapture"].toBool(true)) {
                    QString capturesDir = blogDir + "/captures";
                    static const QString tumblrLoginCheck = R"JS(
                        (function(){
                            if (location.pathname.indexOf('/login') === 0
                             || location.hostname.indexOf('login.tumblr.com') === 0) return true;
                            // 포스트 본문/미디어가 있으면 정상 페이지
                            if (document.querySelector('article, [data-post-id], figure, main img')) return false;
                            return !!document.querySelector('input[name="user[email]"], input[type="password"]');
                        })()
                    )JS";
                    // ★ raw cookie 파싱 (config["cookie"] — UI에서 사용자 입력) → NSFW/private 캡쳐 시 로그인 상태
                    QList<QNetworkCookie> tumblrCookies;
                    QString rawTk = config["cookie"].toString();
                    if (!rawTk.isEmpty()) {
                        for (const QString &part : rawTk.split(';', Qt::SkipEmptyParts)) {
                            int eq = part.indexOf('=');
                            if (eq <= 0) continue;
                            QNetworkCookie c(part.left(eq).trimmed().toUtf8(),
                                              part.mid(eq + 1).trimmed().toUtf8());
                            c.setDomain(".tumblr.com"); c.setPath("/"); c.setSecure(true);
                            tumblrCookies << c;
                        }
                    }
                    captureRealPageCDPLoginAware(postUrl, capturesDir, orderPrefix + postId,
                        tumblrLoginCheck, "tumblr", 8000, tumblrCookies, config);
                }
            }

            updateStats(allPosts.count(), mediaDownloaded, "수집 중", "tumblr");
            if (maxCount > 0 && allPosts.count() >= maxCount) break;
        }

        offset += posts.size();
        if (offset >= totalPosts) break;
        if (maxCount > 0 && allPosts.count() >= maxCount) break;

        log(QString("포스트 %1/%2 | 미디어 %3").arg(allPosts.count()).arg(totalPosts).arg(mediaDownloaded), "info", "tumblr");
        QThread::msleep(500);
    }

    // Excel — type별 + complete
    if (saveExcel && allPosts.count() > 0) {
        log("Excel 저장 중...", "info", "tumblr");
        QJsonArray allPostsArray = allPosts.readAll();
        QStringList hdrs = {"ID", "Date", "Type", "Slug", "Summary", "Tags", "Notes", "URL"};

        // helper: write a subset of posts to an xlsx file
        auto writeExcel = [&](const QString &path, const QJsonArray &rows) {
            ExcelWriter writer;
            writer.writeHeader(hdrs, QColor("#36465d"));
            int row = 2;
            for (const auto &val : rows) {
                QJsonObject p = val.toObject();
                qint64 ts = p["timestamp"].toVariant().toLongLong();
                QStringList tags;
                for (const auto &t : p["tags"].toArray()) tags << t.toString();
                writer.writeRow(row++, {
                    QString::number(p["id"].toVariant().toLongLong()),
                    ts > 0 ? QDateTime::fromSecsSinceEpoch(ts).toString("yyyy-MM-dd HH:mm:ss") : "",
                    p["type"].toString(),
                    p["slug"].toString(),
                    p["summary"].toString().left(200),
                    tags.join(", "),
                    QString::number(p["note_count"].toInt()),
                    p["post_url"].toString()
                });
            }
            writer.autoFitColumns(hdrs);
            writer.save(path);
        };

        // _complete (all)
        writeExcel(FileHelper::typeExcelPath(excelDir, blogName, "complete"), allPostsArray);

        // per-type (photo/video/audio/text/quote/link/chat/answer)
        QMap<QString, QJsonArray> byType;
        for (const auto &val : allPostsArray) {
            QString t = val.toObject()["type"].toString();
            if (t.isEmpty()) t = "other";
            byType[t].append(val);
        }
        for (auto it = byType.begin(); it != byType.end(); ++it) {
            writeExcel(FileHelper::typeExcelPath(excelDir, blogName, it.key()), it.value());
        }
        log(QString("Excel 저장 완료 (%1 types)").arg(byType.size() + 1), "success", "tumblr");
    }

    log(QString("━━ Tumblr 수집 완료: %1 포스트, %2 미디어 ━━").arg(allPosts.count()).arg(mediaDownloaded), "success", "tumblr");
    updateStats(allPosts.count(), mediaDownloaded, "완료", "tumblr");
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ── SpinSpin Collector (web-api.spin-spin.com) ──
// API endpoints (reverse-engineered from _app.js chunk):
//   Base URL: https://web-api.spin-spin.com
//   GET /api/requestbox/getBoxInfo?handle={handle}
//     → {success, boxInfo:[{_id, profileImg, nickname, handle, bio, boxName, backImage, ...}]}
//   GET /api/requestbox/getRepliedLetters?boxId={_id}&page={N}
//     → {success, replyInfo:[{_id, text, image, reply:[{_id, text, imageList[], createdAt, ...}]}],
//        next, isLast}
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void MiyoBackend::runSpinSpinCollection(const QJsonObject &config)
{
    CollectionGuard _cg(platformSem("spinspin"), this, "spinspin");
    setIntegrityActiveForPlatform("spinspin", config["integrityCheck"].toBool(false));
    if (config["method"].toString() == "chrome") {
        runRealChromeCollection(config);
        return;
    }
    if (config["method"].toString() == "web") {
        runWebCrawlCollection(config);
        return;
    }
    QString target = config["target"].toString().trimmed();
    if (target.startsWith("@")) target = target.mid(1);
    // Allow user to paste a URL
    if (target.contains("spin-spin.com/")) {
        target = target.section("spin-spin.com/", 1).section('/', 0, 0).section('?', 0, 0);
    }
    QString cookie = config["cookie"].toString();
    QString savePath = config["path"].toString();
    savePath.replace("~", QDir::homePath());
    bool saveExcel = config["excel"].toBool(true);
    bool downloadMedia = config["downloadMedia"].toBool(true);

    // 계정 로테이션용 쿠키 목록
    QStringList cookies;
    QJsonArray spAccs = config["accounts"].toArray();
    for (const auto &a : spAccs) {
        QString c = a.toObject()["cookie"].toString().trimmed();
        if (!c.isEmpty()) cookies << c;
    }
    if (cookies.isEmpty() && !cookie.isEmpty()) cookies << cookie;
    int currentCookieIdx = 0;
    if (!cookies.isEmpty()) cookie = cookies[0];

    if (target.isEmpty()) {
        log("유저 핸들을 입력하세요.", "error", "spinspin");
        return;
    }

    log(QString("SpinSpin 수집 시작: %1").arg(target), "info", "spinspin");

    HttpClient http;
    http.setTimeout(30000);
    http.setRunFlag(&m_isRunning["spinspin"]);  // 중지 시 즉시 abort

    const QString API_BASE = "https://web-api.spin-spin.com";

    QMap<QString, QString> headers;
    headers["User-Agent"] = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
                             "(KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36";
    headers["Accept"] = "application/json, text/plain, */*";
    headers["Accept-Language"] = "ko-KR,ko;q=0.9,en;q=0.8";
    headers["Origin"] = "https://spin-spin.com";
    headers["Referer"] = QString("https://spin-spin.com/%1").arg(target);
    if (!cookie.isEmpty()) headers["Cookie"] = cookie;

    // ── Step 1: getBoxInfo ──
    QString boxInfoUrl = QString("%1/api/requestbox/getBoxInfo?handle=%2").arg(API_BASE, target);
    HttpResponse boxResp = http.get(boxInfoUrl, headers);
    if (!boxResp.isOk()) {
        log(QString("getBoxInfo 실패 (HTTP %1)").arg(boxResp.statusCode), "error", "spinspin");
        return;
    }

    QJsonObject boxJson = boxResp.json();
    QJsonArray boxInfoArr = boxJson["boxInfo"].toArray();
    if (boxInfoArr.isEmpty()) {
        log(QString("핸들을 찾을 수 없음: %1").arg(target), "error", "spinspin");
        return;
    }
    QJsonObject boxInfo = boxInfoArr[0].toObject();
    QString boxId = boxInfo["_id"].toString();
    QString nickname = boxInfo["nickname"].toString();
    QString boxName = boxInfo["boxName"].toString();
    QString profileImg = boxInfo["profileImg"].toString();
    QString backImage = boxInfo["backImage"].toString();

    log(QString("박스: %1 (%2) — id=%3").arg(nickname, boxName, boxId), "info", "spinspin");

    // ── 저장 폴더 구조 ──
    //   spinspin/{handle}/
    //     profile/   (profile+back image)
    //     replies/   (letter + reply media, 업로드 순 prefix)
    //     excel/{handle}_replies.xlsx
    //     profile.json
    QString spinDir = savePath + "/spinspin/" + sanitizeFilename(target, 100);
    QDir().mkpath(spinDir);
    QString profileDir = FileHelper::typeFolder(spinDir, "profile");
    QString repliesDir = FileHelper::typeFolder(spinDir, "replies");
    QString excelDir = spinDir + "/excel";
    if (saveExcel) QDir().mkpath(excelDir);

    // Save profile.json
    {
        QFile pf(spinDir + "/profile.json");
        if (pf.open(QIODevice::WriteOnly)) {
            pf.write(QJsonDocument(boxInfo).toJson(QJsonDocument::Indented));
            pf.close();
        }
    }

    int mediaCount = 0;

    // Download profile + back image (date-tagged archiving)
    if (downloadMedia) {
        auto saveImg = [&](const QString &url, const QString &baseName) {
            if (url.isEmpty() || !url.startsWith("http")) return;
            QString ext = QFileInfo(QUrl(url).path()).suffix();
            if (ext.isEmpty()) ext = "jpg";
            // date-tagged filename: profile_20260412.jpg
            QString dateTag = QDateTime::currentDateTime().toString("yyyyMMdd");
            QString fname = QString("%1_%2.%3").arg(baseName, dateTag, ext);
            QString path = profileDir + "/" + fname;
            if (!QFile::exists(path) && http.downloadFile(url, path, headers)) {
                mediaCount++;
                // EXIF
                Common::addExifMetadata(path, nickname, "",
                    QString("SpinSpin @%1").arg(target),
                    QString("https://spin-spin.com/@%1").arg(target), QString());
                // Finder comment + xattr + mtime
                FileHelper::setFinderComment(path, QString("https://spin-spin.com/@%1").arg(target));
                FileHelper::applyPostMetadata(path, QDateTime::currentDateTimeUtc(),
                    QString("https://spin-spin.com/@%1").arg(target));
            }
        };
        saveImg(profileImg, "profile");
        saveImg(backImage, "back");
    }

    // ── Step 2: getRepliedLetters 페이지네이션 ──
    QJsonArray allLetters;
    int page = 0;
    const int MAX_PAGES = 500;
    while (m_isRunning.value("spinspin", false) && page < MAX_PAGES) {
        QString listUrl = QString("%1/api/requestbox/getRepliedLetters?boxId=%2&page=%3")
                              .arg(API_BASE, boxId).arg(page);
        HttpResponse listResp = http.get(listUrl, headers);
        if (!listResp.isOk()) {
            if (listResp.statusCode == 429 || listResp.statusCode == 403) {
                // 계정 로테이션
                if (cookies.size() > 1) {
                    currentCookieIdx = (currentCookieIdx + 1) % cookies.size();
                    cookie = cookies[currentCookieIdx];
                    headers["Cookie"] = cookie;
                    log(QString("Rate Limit → 쿠키 전환 (%1/%2)").arg(currentCookieIdx + 1).arg(cookies.size()), "warning", "spinspin");
                    QThread::sleep(3);
                    continue;
                }
                log("Rate Limit — 30초 대기...", "warning", "spinspin");
                for (int w = 30; w > 0 && m_isRunning.value("spinspin", false); --w) {
                    updateStats(allLetters.size(), mediaCount, QString("대기 %1s").arg(w), "spinspin");
                    QThread::sleep(1);
                }
                continue;
            }
            log(QString("getRepliedLetters page=%1 실패 (HTTP %2)").arg(page).arg(listResp.statusCode),
                "warning", "spinspin");
            break;
        }
        QJsonObject lj = listResp.json();
        QJsonArray replyInfo = lj["replyInfo"].toArray();
        if (replyInfo.isEmpty()) break;
        for (const auto &v : replyInfo) allLetters.append(v);
        log(QString("page=%1 수신 (%2건, 누적 %3)").arg(page).arg(replyInfo.size()).arg(allLetters.size()),
            "info", "spinspin");
        updateStats(allLetters.size(), mediaCount, QString("수집 중 (page %1)").arg(page), "spinspin");
        bool isLast = lj["isLast"].toBool();
        if (isLast) break;
        page++;
        QThread::msleep(800); // polite rate limit
    }

    log(QString("📬 총 편지 %1건 수신").arg(allLetters.size()), "success", "spinspin");

    // Save raw feed as JSON
    {
        QFile rf(spinDir + "/replies_raw.json");
        if (rf.open(QIODevice::WriteOnly)) {
            rf.write(QJsonDocument(allLetters).toJson(QJsonDocument::Indented));
            rf.close();
        }
    }

    // ── Step 3: 편지별 미디어 다운로드 + Excel 행 생성 ──
    QJsonArray excelRows;
    auto stripHtml = [](QString s) {
        s.replace("<br>", "\n", Qt::CaseInsensitive);
        s.replace("<br/>", "\n", Qt::CaseInsensitive);
        s.replace("<br />", "\n", Qt::CaseInsensitive);
        s.remove(QRegularExpression("<[^>]+>"));
        return s.trimmed();
    };

    int letterIdx = 0;
    for (const auto &lv : allLetters) {
        if (!m_isRunning.value("spinspin", false)) break;
        letterIdx++;

        QJsonObject letter = lv.toObject();
        QString letterId = letter["_id"].toString();
        QString letterText = stripHtml(letter["text"].toString());
        QString letterImage = letter["image"].toString();

        // reply can be array or object
        QJsonArray replyArr;
        QJsonValue replyVal = letter["reply"];
        if (replyVal.isArray()) replyArr = replyVal.toArray();
        else if (replyVal.isObject()) replyArr.append(replyVal);

        QString firstReplyDate;
        QStringList replyTexts;
        QStringList replyImgs;
        for (const auto &rv : replyArr) {
            QJsonObject r = rv.toObject();
            QString rtext = stripHtml(r["text"].toString());
            if (!rtext.isEmpty()) replyTexts << rtext;
            if (firstReplyDate.isEmpty()) firstReplyDate = r["createdAt"].toString();
            QString rimg = r["image"].toString();
            if (!rimg.isEmpty() && rimg.startsWith("http")) replyImgs << rimg;
            QJsonArray imgList = r["imageList"].toArray();
            for (const auto &iv : imgList) {
                QString u = iv.toString();
                if (u.startsWith("http")) replyImgs << u;
            }
        }

        // 업로드 시각 prefix
        QDateTime dt = QDateTime::fromString(firstReplyDate, Qt::ISODate);
        QString orderPrefix;
        if (dt.isValid()) {
            orderPrefix = dt.toUTC().addSecs(9 * 3600).toString("yyyyMMdd_HHmm_");
        }

        // Excel 행
        if (saveExcel) {
            QJsonObject row;
            row["letterId"] = letterId;
            row["letter_text"] = letterText;
            row["letter_image"] = letterImage;
            row["replied_at"] = firstReplyDate;
            row["reply_text"] = replyTexts.join("\n---\n");
            row["reply_image_count"] = replyImgs.size();
            row["reply_images"] = replyImgs.join("\n");
            row["url"] = QString("https://spin-spin.com/q/%1").arg(letterId);
            excelRows.append(row);
        }

        // 미디어 다운로드
        if (downloadMedia) {
            auto dlReplyImg = [&](const QString &url, int idx) {
                if (url.isEmpty() || !url.startsWith("http")) return;
                QString ext = QFileInfo(QUrl(url).path()).suffix();
                if (ext.isEmpty()) ext = "jpg";
                QString title = letterText.left(30).trimmed().replace('\n', ' ');
                title = sanitizeFilename(title, 30);
                if (title.isEmpty()) title = "reply";
                QString fname = QString("%1%2 (%3_%4).%5")
                                    .arg(orderPrefix, title, letterId.right(6))
                                    .arg(idx).arg(ext);
                fname = sanitizeFilename(fname, 200);
                QString filepath = repliesDir + "/" + fname;
                if (!QFile::exists(filepath) && http.downloadFile(url, filepath, headers)) {
                    mediaCount++;
                    QString postUrl = QString("https://spin-spin.com/q/%1").arg(letterId);
                    // EXIF → Finder comment → xattr+mtime → _complete mirror
                    Common::addExifMetadata(filepath, nickname, letterText.left(200),
                        QString("SpinSpin @%1").arg(target), postUrl,
                        dt.isValid() ? dt.toString(Qt::ISODate) : QString());
                    FileHelper::setFinderComment(filepath, postUrl);
                    if (dt.isValid()) {
                        FileHelper::applyPostMetadata(filepath, dt, postUrl);
                    }
                    // _complete mirror
                    QString completeDir = FileHelper::typeFolder(spinDir, "complete");
                    QString completePath = completeDir + "/" + fname;
                    if (!QFile::exists(completePath)) {
                        QFile::copy(filepath, completePath);
                        if (dt.isValid()) FileHelper::applyPostMetadata(completePath, dt, postUrl);
                        FileHelper::setFinderComment(completePath, postUrl);
                    }
                }
            };

            int imgIdx = 0;
            // Letter image (question attachment) — save with "Q" tag
            if (!letterImage.isEmpty() && letterImage.startsWith("http")) {
                QString ext = QFileInfo(QUrl(letterImage).path()).suffix();
                if (ext.isEmpty()) ext = "jpg";
                QString title = letterText.left(30).trimmed().replace('\n', ' ');
                title = sanitizeFilename(title, 30);
                if (title.isEmpty()) title = "question";
                QString fname = QString("%1%2 (%3_Q).%4")
                                    .arg(orderPrefix, title, letterId.right(6), ext);
                fname = sanitizeFilename(fname, 200);
                QString fp = repliesDir + "/" + fname;
                if (!QFile::exists(fp) && http.downloadFile(letterImage, fp, headers)) {
                    mediaCount++;
                    QString postUrl = QString("https://spin-spin.com/q/%1").arg(letterId);
                    // EXIF → Finder comment → xattr+mtime → _complete mirror
                    Common::addExifMetadata(fp, nickname, letterText.left(200),
                        QString("SpinSpin @%1").arg(target), postUrl,
                        dt.isValid() ? dt.toString(Qt::ISODate) : QString());
                    FileHelper::setFinderComment(fp, postUrl);
                    if (dt.isValid()) {
                        FileHelper::applyPostMetadata(fp, dt, postUrl);
                    }
                    // _complete mirror
                    QString completeDir = FileHelper::typeFolder(spinDir, "complete");
                    QString completePath = completeDir + "/" + fname;
                    if (!QFile::exists(completePath)) {
                        QFile::copy(fp, completePath);
                        if (dt.isValid()) FileHelper::applyPostMetadata(completePath, dt, postUrl);
                        FileHelper::setFinderComment(completePath, postUrl);
                    }
                }
            }
            // Reply images
            for (const QString &u : replyImgs) {
                dlReplyImg(u, imgIdx++);
            }

            // 経済産業省 연계: 페이지 캡처 (편지별 1회)
            if (mediaCount > 0) {
                QString capturesDir = spinDir + "/captures";
                QString postUrl = QString("https://spin-spin.com/q/%1").arg(letterId);
                FileHelper::capturePageHtml(capturesDir, postUrl,
                    orderPrefix + letterId, &http, headers);
            }
        }

        if (letterIdx % 10 == 0) {
            updateStats(letterIdx, mediaCount,
                        QString("처리 중 %1/%2").arg(letterIdx).arg(allLetters.size()), "spinspin");
        }
    }

    // ── Step 4: Excel 저장 ──
    if (saveExcel && !excelRows.isEmpty()) {
        ExcelWriter xw;
        xw.createSheet("replies");
        xw.selectSheet("replies");
        QStringList hdr = {"letterId", "letter_text", "letter_image", "replied_at",
                           "reply_text", "reply_image_count", "reply_images", "url"};
        xw.writeHeader(hdr);
        for (int i = 0; i < excelRows.size(); ++i) {
            QJsonObject row = excelRows[i].toObject();
            QStringList vals;
            for (const QString &k : hdr) {
                QJsonValue v = row[k];
                if (v.isDouble()) vals << QString::number(v.toInt());
                else vals << v.toString();
            }
            xw.writeRow(i + 2, vals);
        }
        xw.autoFitColumns(hdr);
        QString excelPath = FileHelper::typeExcelPath(excelDir, target, "replies");
        if (xw.save(excelPath)) {
            log(QString("📊 Excel 저장: %1").arg(excelPath), "success", "spinspin");
        }
    }

    updateStats(allLetters.size(), mediaCount, "완료", "spinspin");
    log(QString("━━ 수집 완료: 편지 %1건, 미디어 %2개 ━━")
            .arg(allLetters.size()).arg(mediaCount), "success", "spinspin");
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ── Asked Collector (HTTP + Cookie) — Twitter 스타일 저장 ──
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

namespace {

// JSON 트리 재귀 — 주어진 키 중 하나라도 가진 첫 번째 객체 반환
// minRequired: 키 중 최소 몇 개를 가져야 하는지 (기본 1)
QJsonObject askedFindFirstObjectWithKeys(const QJsonValue &v,
                                          const QStringList &requireOne,
                                          const QStringList &requireBonus,
                                          int maxDepth = 12)
{
    if (maxDepth <= 0) return QJsonObject();
    if (v.isObject()) {
        QJsonObject o = v.toObject();
        // 자기 자신이 매치하는지 확인
        bool hasReq = false;
        for (const QString &k : requireOne) {
            if (o.contains(k) && !o.value(k).isNull() && !o.value(k).toString().isEmpty())
                { hasReq = true; break; }
        }
        if (hasReq) {
            int bonus = 0;
            for (const QString &k : requireBonus) {
                if (o.contains(k) && !o.value(k).isNull()) bonus++;
            }
            // 보너스 키가 없어도 OK — 단, 다른 자식이 더 좋으면 그쪽 우선
            if (bonus >= 1 || requireBonus.isEmpty()) return o;
        }
        // 자식 재귀
        for (auto it = o.begin(); it != o.end(); ++it) {
            QJsonObject found = askedFindFirstObjectWithKeys(it.value(), requireOne, requireBonus, maxDepth - 1);
            if (!found.isEmpty()) return found;
        }
        // 자식에서 못 찾았는데 자기가 약하게라도 매치하면 반환
        if (hasReq) return o;
    } else if (v.isArray()) {
        QJsonArray a = v.toArray();
        for (const auto &item : a) {
            QJsonObject found = askedFindFirstObjectWithKeys(item, requireOne, requireBonus, maxDepth - 1);
            if (!found.isEmpty()) return found;
        }
    }
    return QJsonObject();
}

// 객체 배열 중, 항목들이 itemKeys 중 하나라도 가지면 매치
// minSize: 최소 배열 크기
QJsonArray askedFindFirstArrayOfPosts(const QJsonValue &v,
                                       const QStringList &itemKeys,
                                       int minSize = 1,
                                       int maxDepth = 14)
{
    if (maxDepth <= 0) return QJsonArray();
    if (v.isArray()) {
        QJsonArray a = v.toArray();
        if (a.size() >= minSize) {
            int matchCount = 0;
            int sample = qMin(a.size(), 5);
            for (int i = 0; i < sample; ++i) {
                if (!a[i].isObject()) { matchCount = 0; break; }
                QJsonObject o = a[i].toObject();
                bool itemHas = false;
                for (const QString &k : itemKeys) {
                    if (o.contains(k)) { itemHas = true; break; }
                }
                if (itemHas) matchCount++;
            }
            if (sample > 0 && matchCount == sample) return a;
        }
        // 배열 안의 중첩 구조에도 재귀
        for (const auto &item : a) {
            QJsonArray found = askedFindFirstArrayOfPosts(item, itemKeys, minSize, maxDepth - 1);
            if (!found.isEmpty()) return found;
        }
    } else if (v.isObject()) {
        QJsonObject o = v.toObject();
        for (auto it = o.begin(); it != o.end(); ++it) {
            QJsonArray found = askedFindFirstArrayOfPosts(it.value(), itemKeys, minSize, maxDepth - 1);
            if (!found.isEmpty()) return found;
        }
    }
    return QJsonArray();
}

// 객체에서 첫 번째 비어있지 않은 문자열 키 추출
QString askedFirstNonEmpty(const QJsonObject &o, const QStringList &keys)
{
    for (const QString &k : keys) {
        if (o.contains(k)) {
            QJsonValue v = o.value(k);
            if (v.isString()) {
                QString s = v.toString().trimmed();
                if (!s.isEmpty()) return s;
            } else if (v.isDouble()) {
                return QString::number(v.toDouble(), 'f', 0);
            }
        }
    }
    return QString();
}

QString askedHtmlEscape(const QString &s)
{
    QString out;
    out.reserve(s.size());
    for (QChar c : s) {
        switch (c.unicode()) {
            case '&':  out += "&amp;"; break;
            case '<':  out += "&lt;";  break;
            case '>':  out += "&gt;";  break;
            case '"':  out += "&quot;"; break;
            case '\'': out += "&#39;";  break;
            default:   out += c;
        }
    }
    return out;
}

QString askedLinkify(const QString &raw)
{
    QString s = askedHtmlEscape(raw);
    static const QRegularExpression urlRe(R"((https?://[^\s<]+))");
    s.replace(urlRe, R"(<a href="\1" target="_blank" rel="noopener">\1</a>)");
    s.replace('\n', "<br>");
    return s;
}

// Asked 포스트 아카이브 HTML 생성 (질문 + 답변 카드 형식)
QString generateAskedPostArchiveHtml(const QString &saveDir,
                                      const QString &filename,
                                      const QJsonObject &meta)
{
    if (filename.isEmpty()) return QString();
    QDir().mkpath(saveDir);
    const QString filePath = saveDir + "/" + filename + ".html";

    const QString authorName  = meta.value("authorName").toString();
    const QString handle      = meta.value("handle").toString(authorName);
    const QString postId      = meta.value("postId").toString();
    const QString postUrl     = meta.value("postUrl").toString();
    const QString question    = meta.value("question").toString();
    const QString answer      = meta.value("answer").toString();
    const QString askerName   = meta.value("askerName").toString();
    const QString createdAt   = meta.value("createdAt").toString();
    const int likeCount       = meta.value("likeCount").toInt();
    const QJsonArray mediaArr = meta.value("mediaRelPaths").toArray();
    const QString avatarRel   = meta.value("avatarRelPath").toString();

    QString mediaBlock;
    if (!mediaArr.isEmpty()) {
        mediaBlock += "<div class=\"media\">\n";
        for (const auto &v : mediaArr) {
            const QString rel = askedHtmlEscape(v.toString());
            const QString low = v.toString().toLower();
            if (low.endsWith(".mp4") || low.endsWith(".mov") || low.endsWith(".webm")) {
                mediaBlock += QString("  <video src=\"%1\" controls preload=\"metadata\"></video>\n").arg(rel);
            } else {
                mediaBlock += QString("  <a href=\"%1\" target=\"_blank\"><img src=\"%1\" loading=\"lazy\" alt=\"\"></a>\n").arg(rel);
            }
        }
        mediaBlock += "</div>\n";
    }

    QString preview = answer.left(40).trimmed();
    preview.replace('\n', ' ');
    QString title = authorName.isEmpty() ? handle : authorName;
    if (!handle.isEmpty() && handle != authorName) title += " (@" + handle + ")";
    if (!preview.isEmpty()) title += " — " + preview;

    QString html;
    html += "<!DOCTYPE html>\n<html lang=\"ko\">\n<head>\n";
    html += "<meta charset=\"utf-8\">\n";
    html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    html += QString("<title>%1</title>\n").arg(askedHtmlEscape(title));
    html += "<style>\n"
            "  :root { color-scheme: light dark; }\n"
            "  * { box-sizing: border-box; }\n"
            "  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', 'Pretendard',\n"
            "         'Apple SD Gothic Neo', 'Noto Sans KR', sans-serif;\n"
            "         background: #f5f6f8; margin: 0; padding: 20px; color: #1a1a1a; }\n"
            "  @media (prefers-color-scheme: dark) {\n"
            "    body { background: #15171a; color: #e7e9ea; }\n"
            "    .card { background: #1e2126; border-color: #2f3336; }\n"
            "    .ask, .ans { background: #25292f; }\n"
            "    .ans { background: #1f2630; }\n"
            "    .meta, .handle, .stats { color: #8b9095; }\n"
            "    .sep { border-color: #2f3336; }\n"
            "  }\n"
            "  .card { max-width: 640px; margin: 0 auto; background: #fff;\n"
            "          border: 1px solid #e1e8ed; border-radius: 16px; padding: 20px; }\n"
            "  .head { display: flex; align-items: center; gap: 10px; flex-wrap: wrap; }\n"
            "  .avatar { width: 44px; height: 44px; border-radius: 50%; object-fit: cover;\n"
            "            border: 1px solid #e1e8ed; }\n"
            "  .author { font-weight: 700; font-size: 15px; }\n"
            "  .handle { color: #536471; font-size: 14px; }\n"
            "  .badge { display: inline-block; padding: 2px 8px; border-radius: 4px;\n"
            "           font-size: 12px; background: #ffe9ec; color: #e0245e; margin-left: auto; }\n"
            "  .ask { background: #f0f3f5; padding: 12px 14px; border-radius: 12px;\n"
            "         margin: 14px 0 6px; }\n"
            "  .ask-label { font-size: 12px; color: #8b9095; margin-bottom: 4px; font-weight: 600; }\n"
            "  .ask-body { font-size: 15px; line-height: 1.5; white-space: pre-wrap; word-wrap: break-word; }\n"
            "  .ans { background: #fff5f7; padding: 12px 14px; border-radius: 12px;\n"
            "         margin: 6px 0 12px; border-left: 3px solid #e0245e; }\n"
            "  .ans-label { font-size: 12px; color: #e0245e; margin-bottom: 4px; font-weight: 600; }\n"
            "  .ans-body { font-size: 15px; line-height: 1.55; white-space: pre-wrap; word-wrap: break-word; }\n"
            "  .ask a, .ans a { color: #1d9bf0; text-decoration: none; }\n"
            "  .ask a:hover, .ans a:hover { text-decoration: underline; }\n"
            "  .media { display: grid; grid-template-columns: repeat(auto-fit, minmax(200px, 1fr));\n"
            "           gap: 4px; border-radius: 12px; overflow: hidden; margin: 10px 0;\n"
            "           background: #e1e8ed; }\n"
            "  .media img, .media video { width: 100%; height: auto; display: block; }\n"
            "  .sep { border: none; border-top: 1px solid #e1e8ed; margin: 12px 0; }\n"
            "  .meta { color: #536471; font-size: 13px; }\n"
            "  .stats { display: flex; gap: 16px; margin-top: 8px; color: #536471;\n"
            "           font-size: 13px; flex-wrap: wrap; }\n"
            "  .link { display: inline-block; margin-top: 12px; color: #e0245e;\n"
            "          text-decoration: none; font-size: 14px; }\n"
            "  .link:hover { text-decoration: underline; }\n"
            "</style>\n</head>\n<body>\n";
    html += "<article class=\"card\">\n  <div class=\"head\">\n";
    if (!avatarRel.isEmpty()) {
        html += QString("    <img class=\"avatar\" src=\"%1\" alt=\"\">\n").arg(askedHtmlEscape(avatarRel));
    }
    html += QString("    <span class=\"author\">%1</span>\n").arg(askedHtmlEscape(authorName.isEmpty() ? handle : authorName));
    if (!handle.isEmpty() && handle != authorName) {
        html += QString("    <span class=\"handle\">@%1</span>\n").arg(askedHtmlEscape(handle));
    }
    html += "    <span class=\"badge\">Asked</span>\n  </div>\n";
    if (!question.isEmpty()) {
        QString askLabel = askerName.isEmpty() ? "질문" : QString("%1 님의 질문").arg(askedHtmlEscape(askerName));
        html += "  <div class=\"ask\">\n";
        html += QString("    <div class=\"ask-label\">%1</div>\n").arg(askLabel);
        html += QString("    <div class=\"ask-body\">%1</div>\n").arg(askedLinkify(question));
        html += "  </div>\n";
    }
    if (!answer.isEmpty()) {
        html += "  <div class=\"ans\">\n";
        html += "    <div class=\"ans-label\">답변</div>\n";
        html += QString("    <div class=\"ans-body\">%1</div>\n").arg(askedLinkify(answer));
        html += "  </div>\n";
    }
    html += mediaBlock;
    html += "  <hr class=\"sep\">\n";
    if (!createdAt.isEmpty()) {
        html += QString("  <div class=\"meta\"><time datetime=\"%1\">%1</time></div>\n")
                    .arg(askedHtmlEscape(createdAt));
    }
    html += "  <div class=\"stats\">\n";
    html += QString("    <span>♥ %1</span>\n").arg(likeCount);
    html += "  </div>\n";
    if (!postUrl.isEmpty()) {
        html += QString("  <a class=\"link\" href=\"%1\" target=\"_blank\" rel=\"noopener\">원본 보기 · View on asked.kr</a>\n")
                    .arg(askedHtmlEscape(postUrl));
    }
    html += "</article>\n";
    html += QString("<!-- asked-post:%1 author:%2 -->\n")
                .arg(askedHtmlEscape(postId), askedHtmlEscape(handle));
    html += "</body>\n</html>\n";

    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    f.write(html.toUtf8());
    f.close();
    return filePath;
}

// 프로필 아카이브 HTML (이름, 핸들, 아바타, 자기소개)
QString generateAskedProfileArchiveHtml(const QString &saveDir,
                                         const QString &filename,
                                         const QJsonObject &meta)
{
    if (filename.isEmpty()) return QString();
    QDir().mkpath(saveDir);
    const QString filePath = saveDir + "/" + filename + ".html";

    const QString authorName = meta.value("authorName").toString();
    const QString handle     = meta.value("handle").toString(authorName);
    const QString profileUrl = meta.value("profileUrl").toString();
    const QString bio        = meta.value("bio").toString();
    const QString avatarRel  = meta.value("avatarRelPath").toString();
    const int answerCount    = meta.value("answerCount").toInt();
    const int followerCount  = meta.value("followerCount").toInt();

    QString title = authorName.isEmpty() ? handle : authorName;
    if (!handle.isEmpty() && handle != authorName) title += " (@" + handle + ")";
    title += " — Asked Profile";

    QString html;
    html += "<!DOCTYPE html>\n<html lang=\"ko\">\n<head>\n";
    html += "<meta charset=\"utf-8\">\n";
    html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    html += QString("<title>%1</title>\n").arg(askedHtmlEscape(title));
    html += "<style>\n"
            "  :root { color-scheme: light dark; }\n"
            "  body { font-family: -apple-system, BlinkMacSystemFont, 'Pretendard',\n"
            "         'Apple SD Gothic Neo', 'Noto Sans KR', sans-serif;\n"
            "         background: #f5f6f8; margin: 0; padding: 24px; color: #1a1a1a; }\n"
            "  @media (prefers-color-scheme: dark) {\n"
            "    body { background: #15171a; color: #e7e9ea; }\n"
            "    .card { background: #1e2126; border-color: #2f3336; }\n"
            "    .meta, .handle { color: #8b9095; }\n"
            "  }\n"
            "  .card { max-width: 520px; margin: 0 auto; background: #fff;\n"
            "          border: 1px solid #e1e8ed; border-radius: 18px; padding: 24px; text-align: center; }\n"
            "  .avatar-lg { width: 120px; height: 120px; border-radius: 50%; object-fit: cover;\n"
            "               border: 2px solid #e1e8ed; margin: 0 auto 16px; display: block; }\n"
            "  .author { font-weight: 700; font-size: 22px; margin: 4px 0; }\n"
            "  .handle { color: #536471; font-size: 15px; margin-bottom: 16px; }\n"
            "  .bio { font-size: 15px; line-height: 1.5; white-space: pre-wrap; word-wrap: break-word;\n"
            "         text-align: left; padding: 12px; background: #f0f3f5; border-radius: 10px; }\n"
            "  @media (prefers-color-scheme: dark) { .bio { background: #25292f; } }\n"
            "  .stats { display: flex; justify-content: center; gap: 24px; margin-top: 16px;\n"
            "           color: #536471; font-size: 14px; }\n"
            "  .stat-num { font-weight: 700; color: inherit; display: block; font-size: 18px; }\n"
            "  .link { display: inline-block; margin-top: 16px; color: #e0245e;\n"
            "          text-decoration: none; font-size: 14px; }\n"
            "</style>\n</head>\n<body>\n";
    html += "<article class=\"card\">\n";
    if (!avatarRel.isEmpty()) {
        html += QString("  <img class=\"avatar-lg\" src=\"%1\" alt=\"\">\n").arg(askedHtmlEscape(avatarRel));
    }
    html += QString("  <div class=\"author\">%1</div>\n").arg(askedHtmlEscape(authorName.isEmpty() ? handle : authorName));
    if (!handle.isEmpty() && handle != authorName) {
        html += QString("  <div class=\"handle\">@%1</div>\n").arg(askedHtmlEscape(handle));
    }
    if (!bio.isEmpty()) {
        html += QString("  <div class=\"bio\">%1</div>\n").arg(askedLinkify(bio));
    }
    if (answerCount > 0 || followerCount > 0) {
        html += "  <div class=\"stats\">\n";
        if (answerCount > 0) html += QString("    <div><span class=\"stat-num\">%1</span>답변</div>\n").arg(answerCount);
        if (followerCount > 0) html += QString("    <div><span class=\"stat-num\">%1</span>팔로워</div>\n").arg(followerCount);
        html += "  </div>\n";
    }
    if (!profileUrl.isEmpty()) {
        html += QString("  <a class=\"link\" href=\"%1\" target=\"_blank\" rel=\"noopener\">원본 보기 · View on asked.kr</a>\n")
                    .arg(askedHtmlEscape(profileUrl));
    }
    html += "</article>\n";
    html += "</body>\n</html>\n";

    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    f.write(html.toUtf8());
    f.close();
    return filePath;
}

// URL → 확장자 추출 ("a/b/c.png?x=1" → "png")
QString askedExtFromUrl(const QString &url, const QString &fallback = "jpg")
{
    QString clean = url;
    int q = clean.indexOf('?');
    if (q >= 0) clean = clean.left(q);
    int dot = clean.lastIndexOf('.');
    if (dot < 0) return fallback;
    QString ext = clean.mid(dot + 1).toLower();
    if (ext.size() < 2 || ext.size() > 5) return fallback;
    return ext;
}

// __NUXT__ 파싱: window.__NUXT__ = (function(...){return {...}})(...)
// 인라인 함수 형태일 때는 JSON.parse가 안 되므로, return 뒤의 객체 리터럴만 추출 시도
QJsonObject parseNuxtFlexible(const QString &nuxtStr)
{
    // 단순 객체로 시도
    QJsonDocument doc = QJsonDocument::fromJson(nuxtStr.toUtf8());
    if (doc.isObject()) return doc.object();

    // (function(...){return {...}})(...) 형태
    QRegularExpression returnRe(R"(return\s*(\{.*\})\s*\}\s*\()");
    auto m = returnRe.match(nuxtStr);
    if (m.hasMatch()) {
        QString inner = m.captured(1);
        QJsonDocument d2 = QJsonDocument::fromJson(inner.toUtf8());
        if (d2.isObject()) return d2.object();
    }
    return QJsonObject();
}

} // anonymous namespace

void MiyoBackend::runAskedCollection(const QJsonObject &config)
{
    CollectionGuard _cg(platformSem("asked"), this, "asked");
    setIntegrityActiveForPlatform("asked", config["integrityCheck"].toBool(false));
    if (config["method"].toString() == "chrome") {
        runRealChromeCollection(config);
        return;
    }
    if (config["method"].toString() == "web") {
        runWebCrawlCollection(config);
        return;
    }
    QString target = config["target"].toString().trimmed();
    if (target.startsWith("@")) target = target.mid(1);
    QString cookie = config["cookie"].toString();
    QString savePath = config["path"].toString();
    savePath.replace("~", QDir::homePath());
    bool saveExcel = config["excel"].toBool(true);
    bool downloadMediaFlag = config["downloadMedia"].toBool(true);
    bool saveExif = config["exif"].toBool(true);

    // 계정 로테이션용 쿠키 목록
    QStringList askedCookies;
    QJsonArray askAccs = config["accounts"].toArray();
    for (const auto &a : askAccs) {
        QString c = a.toObject()["cookie"].toString().trimmed();
        if (!c.isEmpty()) askedCookies << c;
    }
    if (askedCookies.isEmpty() && !cookie.isEmpty()) askedCookies << cookie;
    int currentAskCookieIdx = 0;
    if (!askedCookies.isEmpty()) cookie = askedCookies[0];

    if (target.isEmpty()) {
        log("유저 아이디를 입력하세요.", "error", "asked");
        return;
    }

    log(QString("Asked 수집 시작: %1").arg(target), "info", "asked");

    HttpClient http;
    http.setTimeout(30000);
    http.setRunFlag(&m_isRunning["asked"]);  // 중지 시 즉시 abort

    QMap<QString, QString> headers;
    headers["User-Agent"] = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36";
    headers["Accept"] = "application/json, text/html";
    if (!cookie.isEmpty()) headers["Cookie"] = cookie;

    // Asked.kr — Nuxt.js → fetch page, extract __NUXT__
    QString userUrl = QString("https://asked.kr/%1").arg(target);
    HttpResponse pageResp = http.get(userUrl, headers);

    if (!pageResp.isOk()) {
        log(QString("페이지 가져오기 실패 (HTTP %1)").arg(pageResp.statusCode), "error", "asked");
        return;
    }

    // 공통 저장 정책: savePath/asked/{target}/{type}/
    //   type ∈ {answers, media, _complete}
    QString askedDir = savePath + "/asked";
    QString userDir = askedDir + "/" + sanitizeFilename(target, 100);
    QDir().mkpath(userDir);
    QString mediaDir = FileHelper::typeFolder(userDir, "media");
    QString completeDir = FileHelper::typeFolder(userDir, "complete");
    QString excelDir = userDir + "/excel";
    if (saveExcel) QDir().mkpath(excelDir);

    QString html = QString::fromUtf8(pageResp.data);

    // Cloudflare 차단 감지 — 다른 쿠키로 재시도
    if (html.contains("Just a moment") || html.contains("cf-chl") || html.contains("_cf_chl_opt")) {
        if (askedCookies.size() > 1) {
            for (int ci = 1; ci < askedCookies.size(); ++ci) {
                cookie = askedCookies[ci];
                headers["Cookie"] = cookie;
                log(QString("Cloudflare 차단 → 쿠키 전환 (%1/%2)").arg(ci + 1).arg(askedCookies.size()), "warning", "asked");
                QThread::sleep(2);
                pageResp = http.get(userUrl, headers);
                if (pageResp.isOk()) {
                    html = QString::fromUtf8(pageResp.data);
                    if (!html.contains("Just a moment") && !html.contains("cf-chl")) {
                        currentAskCookieIdx = ci;
                        break;
                    }
                }
            }
        }
        if (html.contains("Just a moment") || html.contains("cf-chl") || html.contains("_cf_chl_opt")) {
            log("Cloudflare 차단 감지 — 브라우저에서 로그인 후 '쿠키 새로고침'을 먼저 실행하세요.",
                "error", "asked");
            return;
        }
    }

    // __NUXT__ state extraction
    QRegularExpression nuxtRe(R"(window\.__NUXT__\s*=\s*(\{.*?\})\s*;?\s*</script>)");
    auto nuxtMatch = nuxtRe.match(html);

    QJsonObject nuxtState;
    QString nuxtRaw;
    if (nuxtMatch.hasMatch()) {
        nuxtRaw = nuxtMatch.captured(1);
        QFile dataFile(userDir + "/nuxt_state.js");
        if (dataFile.open(QIODevice::WriteOnly)) {
            dataFile.write(("window.__NUXT__ = " + nuxtRaw + ";").toUtf8());
            dataFile.close();
        }
        log(QString("__NUXT__ 추출 (%1 bytes)").arg(nuxtRaw.size()), "success", "asked");
        nuxtState = parseNuxtFlexible(nuxtRaw);
    }

    // Save raw HTML
    QFile htmlFile(userDir + "/page.html");
    if (htmlFile.open(QIODevice::WriteOnly)) {
        htmlFile.write(pageResp.data);
        htmlFile.close();
    }

    QString capturesDir = userDir + "/captures";

    // 経済産業省 연계: SingleFile CDP 캡쳐 (realCapture=true일 때만, Asked 쿠키 포함)
    if (config["realCapture"].toBool(true)) {
        QList<QNetworkCookie> askedCookies;
        QString rawCookie = config["cookie"].toString();
        if (!rawCookie.isEmpty()) {
            for (const QString &part : rawCookie.split(';', Qt::SkipEmptyParts)) {
                int eq = part.indexOf('=');
                if (eq <= 0) continue;
                QNetworkCookie c(part.left(eq).trimmed().toUtf8(),
                                  part.mid(eq + 1).trimmed().toUtf8());
                c.setDomain(".asked.kr"); c.setPath("/"); c.setSecure(true);
                askedCookies << c;
            }
        }
        static const QString askedLoginCheck = R"JS(
            (function(){
                if (location.pathname.indexOf('/login') === 0
                 || location.pathname.indexOf('/auth') === 0) return true;
                // 답변 컨테이너/본문이 있으면 정상 페이지
                if (document.querySelector('main, article, [data-testid*="answer"], [class*="Answer"]')) return false;
                return !!document.querySelector('input[type="password"]');
            })()
        )JS";
        captureRealPageCDPLoginAware(userUrl, capturesDir, sanitizeFilename(target, 50),
            askedLoginCheck, "asked", 8000, askedCookies, config);
    }

    // Next.js __NEXT_DATA__ (asked가 Next.js로 마이그레이션했을 수 있음)
    QRegularExpression nextRe(R"(<script\s+id="__NEXT_DATA__"[^>]*>(.*?)</script>)");
    auto nextMatch = nextRe.match(html);
    QJsonObject nextState;
    if (nextMatch.hasMatch()) {
        QString jsonStr = nextMatch.captured(1);
        QJsonDocument nd = QJsonDocument::fromJson(jsonStr.toUtf8());
        if (nd.isObject()) {
            nextState = nd.object();
            QFile f(userDir + "/next_data.json");
            if (f.open(QIODevice::WriteOnly)) {
                f.write(nd.toJson(QJsonDocument::Indented));
                f.close();
            }
            log(QString("__NEXT_DATA__ 추출 (%1 bytes)").arg(jsonStr.size()), "success", "asked");
        }
    }

    // ──────────────────────────────────────────
    // 프로필 추출 (이름, 핸들, 아바타, 자기소개)
    // ──────────────────────────────────────────
    int mediaCount = 0;
    int answerCount = 0;

    QJsonObject profileObj;
    if (!nuxtState.isEmpty()) {
        profileObj = askedFindFirstObjectWithKeys(QJsonValue(nuxtState),
            QStringList{"nickname","name","displayName","username","userName","handle"},
            QStringList{"profileImage","profileImg","avatar","avatarUrl","image","bio","description","intro","introduction","about"});
    }
    if (profileObj.isEmpty() && !nextState.isEmpty()) {
        profileObj = askedFindFirstObjectWithKeys(QJsonValue(nextState),
            QStringList{"nickname","name","displayName","username","userName","handle"},
            QStringList{"profileImage","profileImg","avatar","avatarUrl","image","bio","description","intro","introduction","about"});
    }

    QString profName   = askedFirstNonEmpty(profileObj, {"nickname","name","displayName","fullName"});
    QString profHandle = askedFirstNonEmpty(profileObj, {"username","userName","handle","slug","screen_name"});
    if (profHandle.isEmpty()) profHandle = target;
    QString avatarUrl  = askedFirstNonEmpty(profileObj, {"profileImage","profileImg","avatar","avatarUrl","image","photo","photoUrl"});
    QString bio        = askedFirstNonEmpty(profileObj, {"bio","description","introduction","intro","about","status_message"});
    int followerCount  = profileObj.value("followers").toInt() + profileObj.value("followerCount").toInt();
    int profAnswerCnt  = profileObj.value("answerCount").toInt() + profileObj.value("answersCount").toInt();

    // profile.json
    if (!profileObj.isEmpty()) {
        QFile pf(userDir + "/profile.json");
        if (pf.open(QIODevice::WriteOnly)) {
            pf.write(QJsonDocument(profileObj).toJson(QJsonDocument::Indented));
            pf.close();
        }
        log(QString("프로필 추출: %1 (@%2)").arg(profName.isEmpty() ? target : profName, profHandle),
            "success", "asked");
    } else {
        log("프로필 객체를 __NUXT__/__NEXT_DATA__에서 찾지 못함 — HTML 메타에서 시도",
            "warning", "asked");
        // Fallback: og:title / og:description
        QRegularExpression ogTitleRe(R"og(<meta[^>]+property="og:title"[^>]+content="([^"]+)")og");
        QRegularExpression ogDescRe(R"og(<meta[^>]+property="og:description"[^>]+content="([^"]+)")og");
        QRegularExpression ogImgRe(R"og(<meta[^>]+property="og:image"[^>]+content="([^"]+)")og");
        auto m1 = ogTitleRe.match(html);
        auto m2 = ogDescRe.match(html);
        auto m3 = ogImgRe.match(html);
        if (m1.hasMatch()) profName = m1.captured(1).trimmed();
        if (m2.hasMatch()) bio = m2.captured(1).trimmed();
        if (m3.hasMatch()) avatarUrl = m3.captured(1).trimmed();
    }

    // profile_description.txt
    if (!bio.isEmpty()) {
        QFile bf(userDir + "/profile_description.txt");
        if (bf.open(QIODevice::WriteOnly)) {
            bf.write(bio.toUtf8());
            bf.close();
        }
    }

    // 아바타 다운로드
    QString profilesDir = userDir + "/profiles";
    QString avatarRelForCaptures;  // captures 기준 상대경로 (../profiles/...)
    if (downloadMediaFlag && !avatarUrl.isEmpty()) {
        QDir().mkpath(profilesDir);
        QString avExt = askedExtFromUrl(avatarUrl);
        QString avFname = sanitizeFilename(QString("avatar_%1.%2").arg(profHandle, avExt), 200);
        QString avPath = profilesDir + "/" + avFname;
        if (!QFile::exists(avPath)) {
            if (http.downloadFile(avatarUrl, avPath, headers)) {
                mediaCount++;
                avatarRelForCaptures = "../profiles/" + avFname;
                FileHelper::applyPostMetadata(avPath, QDateTime::currentDateTimeUtc(), avatarUrl);
                FileHelper::setFinderComment(avPath, userUrl);
                if (saveExif) {
                    Common::addExifMetadata(avPath, "@" + profHandle, "",
                        QString("Asked Profile @%1").arg(profHandle), userUrl, QString());
                }
                log(QString("아바타 다운로드: %1").arg(avFname), "success", "asked");
            }
        } else {
            avatarRelForCaptures = "../profiles/" + avFname;
        }
    }

    // 프로필 아카이브 HTML
    {
        QJsonObject pmeta;
        pmeta["authorName"] = profName.isEmpty() ? profHandle : profName;
        pmeta["handle"] = profHandle;
        pmeta["profileUrl"] = userUrl;
        pmeta["bio"] = bio;
        pmeta["avatarRelPath"] = avatarRelForCaptures;
        pmeta["answerCount"] = profAnswerCnt;
        pmeta["followerCount"] = followerCount;
        QString p = generateAskedProfileArchiveHtml(capturesDir,
            sanitizeFilename(QString("profile_%1").arg(profHandle), 100), pmeta);
        if (!p.isEmpty()) log("프로필 아카이브 HTML 생성", "success", "asked");
    }

    // ──────────────────────────────────────────
    // 포스트(질문/답변) 추출 — NUXT/NEXT/API
    // ──────────────────────────────────────────
    QJsonArray postsArr;
    QStringList postItemKeys = {"question","answer","content","body","q","a","questionContent","answerContent"};

    if (!nuxtState.isEmpty()) {
        postsArr = askedFindFirstArrayOfPosts(QJsonValue(nuxtState), postItemKeys, 1);
    }
    if (postsArr.isEmpty() && !nextState.isEmpty()) {
        postsArr = askedFindFirstArrayOfPosts(QJsonValue(nextState), postItemKeys, 1);
    }
    if (!postsArr.isEmpty()) {
        log(QString("포스트 %1개 (state에서 추출)").arg(postsArr.size()), "success", "asked");
    }

    // API attempts (페이지네이션 포함)
    QStringList apiAttempts = {
        QString("https://asked.kr/api/user/%1/answers").arg(target),
        QString("https://asked.kr/api/answers/%1").arg(target),
        QString("https://asked.kr/api/%1/answers").arg(target),
        QString("https://asked.kr/api/v1/answers/%1").arg(target),
        QString("https://asked.kr/api/user/%1").arg(target),
        QString("https://asked.kr/api/profile/%1").arg(target),
        QString("https://asked.kr/api/v1/user/%1").arg(target),
        QString("https://asked.kr/api/v1/profile/%1").arg(target),
        QString("https://asked.kr/api/%1").arg(target),
        QString("https://api.asked.kr/user/%1").arg(target),
    };

    bool apiFound = false;
    for (const QString &apiUrl : apiAttempts) {
        if (!platformRunning("asked")) break;
        HttpResponse apiResp = http.get(apiUrl, headers);
        if (!apiResp.isOk()) { QThread::msleep(300); continue; }
        QJsonDocument doc = QJsonDocument::fromJson(apiResp.data);
        if (doc.isNull()) { QThread::msleep(300); continue; }

        if (!apiFound) {
            log(QString("API 응답: %1").arg(apiUrl), "success", "asked");
            apiFound = true;
            QFile apiFile(userDir + "/api_response.json");
            if (apiFile.open(QIODevice::WriteOnly)) {
                apiFile.write(doc.toJson(QJsonDocument::Indented));
                apiFile.close();
            }
        }

        // API에서 포스트 배열 추출 시도
        if (postsArr.isEmpty()) {
            QJsonArray apiPosts;
            if (doc.isArray()) {
                apiPosts = doc.array();
            } else {
                apiPosts = askedFindFirstArrayOfPosts(QJsonValue(doc.object()), postItemKeys, 1);
            }
            if (!apiPosts.isEmpty()) {
                postsArr = apiPosts;
                log(QString("포스트 %1개 (API에서 추출)").arg(postsArr.size()), "success", "asked");

                // 페이지네이션 시도 — 같은 endpoint에 ?page=N
                for (int page = 2; page <= 30 && platformRunning("asked"); ++page) {
                    QString pageUrl = apiUrl + (apiUrl.contains('?') ? "&page=" : "?page=") + QString::number(page);
                    HttpResponse pgResp = http.get(pageUrl, headers);
                    if (!pgResp.isOk()) break;
                    QJsonDocument pgDoc = QJsonDocument::fromJson(pgResp.data);
                    QJsonArray pgArr;
                    if (pgDoc.isArray()) pgArr = pgDoc.array();
                    else if (pgDoc.isObject())
                        pgArr = askedFindFirstArrayOfPosts(QJsonValue(pgDoc.object()), postItemKeys, 1);
                    if (pgArr.isEmpty()) break;
                    for (const auto &v : pgArr) postsArr.append(v);
                    log(QString("페이지 %1: +%2개").arg(page).arg(pgArr.size()), "info", "asked");
                    QThread::msleep(400);
                }
                break;
            }
        }
        QThread::msleep(300);
    }
    if (!apiFound) {
        log("공개 API 없음 — HTML/state 기반으로 진행", "warning", "asked");
    }

    // 포스트 저장
    QString postsDir = userDir + "/posts";
    if (!postsArr.isEmpty()) QDir().mkpath(postsDir);

    QDateTime now = QDateTime::currentDateTimeUtc();
    QString orderPrefix = FileHelper::uploadOrderPrefix(now);

    QJsonArray postsForExcel;  // {id, question, answer, askerName, createdAt, likes, mediaCount}

    QRegularExpression mediaInTextRe(
        R"re((https?://[^\s"'<>]+\.(?:jpg|jpeg|png|gif|webp|mp4|mov|webm))(?:\?[^\s"'<>]*)?)re",
        QRegularExpression::CaseInsensitiveOption);

    for (int i = 0; i < postsArr.size(); ++i) {
        if (!platformRunning("asked")) break;
        if (!postsArr[i].isObject()) continue;
        QJsonObject p = postsArr[i].toObject();

        QString postId = askedFirstNonEmpty(p, {"id","_id","postId","answerId","questionId","uuid"});
        if (postId.isEmpty()) postId = QString::number(i + 1);
        QString question  = askedFirstNonEmpty(p, {"question","q","questionContent","ask","askContent","query"});
        QString answer    = askedFirstNonEmpty(p, {"answer","a","answerContent","reply","content","body","text"});
        QString askerName = askedFirstNonEmpty(p, {"askerName","asker","fromName","authorName","fromUser"});
        QString createdAt = askedFirstNonEmpty(p, {"createdAt","created_at","date","timestamp","createTime","created"});
        int likes = p.value("like_count").toInt() + p.value("likeCount").toInt() + p.value("likes").toInt();

        // 포스트 raw JSON 저장
        QString jsonName = sanitizeFilename(QString("%1_%2.json").arg(orderPrefix, postId), 200);
        QFile jf(postsDir + "/" + jsonName);
        if (jf.open(QIODevice::WriteOnly)) {
            jf.write(QJsonDocument(p).toJson(QJsonDocument::Indented));
            jf.close();
        }

        // 본문/답변 안의 미디어 URL 추출
        QStringList postMediaUrls;
        for (const QString &text : {question, answer}) {
            auto it = mediaInTextRe.globalMatch(text);
            while (it.hasNext()) postMediaUrls << it.next().captured(1);
        }
        // 객체 내 attachments / images / files 같은 배열도 검사
        for (const QString &k : QStringList{"images","attachments","files","media","photos"}) {
            if (p.contains(k) && p.value(k).isArray()) {
                for (const auto &v : p.value(k).toArray()) {
                    if (v.isString()) postMediaUrls << v.toString();
                    else if (v.isObject()) {
                        QString u = askedFirstNonEmpty(v.toObject(),
                            {"url","src","image","imageUrl","filePath","path"});
                        if (!u.isEmpty()) postMediaUrls << u;
                    }
                }
            }
        }
        postMediaUrls.removeDuplicates();

        // 미디어 다운로드
        QStringList postMediaRel;  // captures 기준 상대경로
        if (downloadMediaFlag) {
            int mIdx = 0;
            for (const QString &mUrl : postMediaUrls) {
                if (!platformRunning("asked")) break;
                QString ext = askedExtFromUrl(mUrl, "jpg");
                QString fname = sanitizeFilename(
                    QString("%1%2_%3_%4.%5").arg(orderPrefix, target, postId).arg(mIdx++).arg(ext), 200);
                QString filepath = mediaDir + "/" + fname;
                if (!QFile::exists(filepath) && http.downloadFile(mUrl, filepath, headers)) {
                    mediaCount++;
                    if (saveExif) {
                        Common::addExifMetadata(filepath, "@" + target, "",
                            QString("Asked @%1").arg(target), userUrl, QString());
                    }
                    FileHelper::setFinderComment(filepath, userUrl);
                    FileHelper::applyPostMetadata(filepath, now, mUrl);
                    QString completePath = completeDir + "/" + fname;
                    if (!QFile::exists(completePath)) {
                        QFile::copy(filepath, completePath);
                        FileHelper::applyPostMetadata(completePath, now, mUrl);
                        FileHelper::setFinderComment(completePath, userUrl);
                    }
                }
                if (QFile::exists(filepath)) {
                    postMediaRel << ("../media/" + fname);
                }
            }
        }

        // 포스트 아카이브 HTML
        QJsonObject hmeta;
        hmeta["authorName"] = profName.isEmpty() ? profHandle : profName;
        hmeta["handle"] = profHandle;
        hmeta["postId"] = postId;
        hmeta["postUrl"] = QString("https://asked.kr/%1/%2").arg(target, postId);
        hmeta["question"] = question;
        hmeta["answer"] = answer;
        hmeta["askerName"] = askerName;
        hmeta["createdAt"] = createdAt;
        hmeta["likeCount"] = likes;
        hmeta["avatarRelPath"] = avatarRelForCaptures;
        QJsonArray relArr;
        for (const QString &r : postMediaRel) relArr.append(r);
        hmeta["mediaRelPaths"] = relArr;

        QString postFname = sanitizeFilename(QString("post_%1_%2").arg(orderPrefix, postId), 200);
        generateAskedPostArchiveHtml(capturesDir, postFname, hmeta);

        QJsonObject row;
        row["id"] = postId;
        row["question"] = question;
        row["answer"] = answer;
        row["askerName"] = askerName;
        row["createdAt"] = createdAt;
        row["likeCount"] = likes;
        row["mediaCount"] = postMediaUrls.size();
        postsForExcel.append(row);

        answerCount++;
    }

    // ──────────────────────────────────────────
    // 추가: HTML에 박혀 있는 미디어 폴백 (state/posts에 없는 경우 대비)
    // ──────────────────────────────────────────
    if (downloadMediaFlag && postsArr.isEmpty()) {
        QRegularExpression imgRe(R"re((?:src|data-src)="(https?://[^"]+\.(?:jpg|jpeg|png|gif|webp))")re");
        auto imgIt = imgRe.globalMatch(html);
        int idx = 0;
        while (imgIt.hasNext() && platformRunning("asked")) {
            auto imgMatch = imgIt.next();
            QString imgUrl = imgMatch.captured(1);
            QString ext = askedExtFromUrl(imgUrl, "jpg");
            QString fname = sanitizeFilename(
                QString("%1%2_%3.%4").arg(orderPrefix, target).arg(idx++).arg(ext), 200);
            QString filepath = mediaDir + "/" + fname;
            if (!QFile::exists(filepath) && http.downloadFile(imgUrl, filepath, headers)) {
                mediaCount++;
                if (saveExif) {
                    Common::addExifMetadata(filepath, "@" + target, "",
                        QString("Asked @%1").arg(target), userUrl, QString());
                }
                FileHelper::setFinderComment(filepath, userUrl);
                FileHelper::applyPostMetadata(filepath, now, imgUrl);
                QString completePath = completeDir + "/" + fname;
                if (!QFile::exists(completePath)) {
                    QFile::copy(filepath, completePath);
                    FileHelper::applyPostMetadata(completePath, now, imgUrl);
                    FileHelper::setFinderComment(completePath, userUrl);
                }
            }
        }
    }

    // ──────────────────────────────────────────
    // Excel — 프로필 + 포스트
    // ──────────────────────────────────────────
    if (saveExcel) {
        // posts 시트
        QStringList postHdr = {"id","question","answer","askerName","createdAt","likeCount","mediaCount"};
        auto writePostsExcel = [&](const QString &path) {
            ExcelWriter writer;
            writer.writeHeader(postHdr, QColor("#e0245e"));
            for (int i = 0; i < postsForExcel.size(); ++i) {
                QJsonObject r = postsForExcel[i].toObject();
                writer.writeRow(i + 2, {
                    r.value("id").toString(),
                    r.value("question").toString(),
                    r.value("answer").toString(),
                    r.value("askerName").toString(),
                    r.value("createdAt").toString(),
                    QString::number(r.value("likeCount").toInt()),
                    QString::number(r.value("mediaCount").toInt())
                });
            }
            writer.autoFitColumns(postHdr);
            writer.save(path);
        };

        // complete (profile + summary)
        QStringList cHdr = {"User","Name","Handle","URL","Bio","Avatar","Posts","Media","API"};
        auto writeCompleteExcel = [&](const QString &path) {
            ExcelWriter writer;
            writer.writeHeader(cHdr, QColor("#ff6b6b"));
            writer.writeRow(2, {
                target,
                profName,
                profHandle,
                userUrl,
                bio,
                avatarUrl,
                QString::number(answerCount),
                QString::number(mediaCount),
                apiFound ? "Yes" : "No"
            });
            writer.autoFitColumns(cHdr);
            writer.save(path);
        };

        writeCompleteExcel(FileHelper::typeExcelPath(excelDir, target, "complete"));
        writePostsExcel(FileHelper::typeExcelPath(excelDir, target, "posts"));
        writePostsExcel(FileHelper::typeExcelPath(excelDir, target, "answers"));
        log("Excel 저장 완료 (complete + posts + answers)", "success", "asked");
    }

    updateStats(answerCount, mediaCount, "완료", "asked");
    log(QString("━━ Asked 수집 완료: 포스트 %1개, 미디어 %2개 ━━")
            .arg(answerCount).arg(mediaCount), "success", "asked");
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// ── 経済産業省 / Site Crawler (웹사이트 크롤링) ──
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

void MiyoBackend::runCrawlCollection(const QJsonObject &config)
{
    // ★ "실제 Chrome (CDP)" 모드 — 사용자 Chrome 프로필로 navigate → SingleFile 캡쳐 (모든 자원 인라인)
    //   QWebEngine으로는 봇 탐지 / JS-shell 페이지 → 실제 Chrome으로 우회.
    if (config["useRealChrome"].toBool(false) || config["method"].toString() == "chrome") {
        QString savePath = config["path"].toString();
        savePath.replace("~", QDir::homePath());
        QString urls = config["url"].toString();
        QStringList urlList;
        for (const QString &u : urls.split(QRegularExpression("[\\s,;]+"), Qt::SkipEmptyParts)) {
            QString t = u.trimmed();
            if (!t.isEmpty()) urlList << t;
        }
        if (urlList.isEmpty()) {
            log("크롤 URL 없음", "error", "crawl");
            updateStats(0, 0, "Error", "crawl");
            return;
        }
        QString crawlDir = savePath + "/crawl_" + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
        QString capturesDir = crawlDir + "/captures";
        QDir().mkpath(capturesDir);
        log(QString("실제 Chrome 크롤 시작: %1개 URL").arg(urlList.size()), "info", "crawl");

        // 각 URL을 별도 워커 스레드에서 처리. config["loginCheckJs"] 있으면 LoginAware 사용
        //   사용자가 UI에서 "로그인 체크 JS" 옵션 입력 시 → 첫 URL에서 로그인 페이지 감지 시 사용자 대기
        QString loginCheckJs = config["loginCheckJs"].toString();
        // ★ 캡쳐용 raw cookie — 도메인은 첫 URL의 호스트로 자동 추정
        QString loginCookie = config["loginCookie"].toString();
        QList<QNetworkCookie> crawlCookies;
        if (!loginCookie.isEmpty() && !urlList.isEmpty()) {
            QString host = QUrl(urlList.first()).host();
            if (!host.isEmpty()) {
                QString domain = host.startsWith("www.") ? host.mid(3) : ("." + host);
                if (!domain.startsWith(".")) domain = "." + domain;
                for (const QString &part : loginCookie.split(';', Qt::SkipEmptyParts)) {
                    int eq = part.indexOf('=');
                    if (eq <= 0) continue;
                    QNetworkCookie c(part.left(eq).trimmed().toUtf8(),
                                      part.mid(eq + 1).trimmed().toUtf8());
                    c.setDomain(domain); c.setPath("/"); c.setSecure(true);
                    crawlCookies << c;
                }
                log(QString("크롤 쿠키 %1개 (도메인: %2)").arg(crawlCookies.size()).arg(domain), "info", "crawl");
            }
        }
        QThread *thread = QThread::create([this, urlList, capturesDir, loginCheckJs, crawlCookies, config]() {
            int saved = 0;
            for (int i = 0; i < urlList.size(); ++i) {
                if (!m_isRunning.value("crawl", true)) break;
                QString url = urlList[i];
                QString filename = QString("page_%1_%2").arg(i+1, 3, 10, QChar('0'))
                                       .arg(QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5).toHex().left(8));
                log(QString("[%1/%2] %3").arg(i+1).arg(urlList.size()).arg(url), "info", "crawl");
                bool ok;
                if (!loginCheckJs.isEmpty()) {
                    ok = captureRealPageCDPLoginAware(url, capturesDir, filename,
                                                       loginCheckJs, "crawl", 8000, crawlCookies, config);
                } else {
                    ok = captureRealPageCDP(url, capturesDir, filename, 8000, crawlCookies);
                }
                if (ok) saved++;
                updateStats(saved, urlList.size(),
                    QString("진행 %1/%2").arg(i+1).arg(urlList.size()), "crawl");
            }
            QMetaObject::invokeMethod(this, [this, saved, total = urlList.size(), capturesDir]() {
                setPlatformRunning("crawl", false);
                updateStats(saved, total, "Done", "crawl");
                log(QString("✅ 크롤 완료: %1/%2 저장 → %3").arg(saved).arg(total).arg(capturesDir), "success", "crawl");
                runJs("setRunning('crawl', false)");
                if (m_window) m_window->releaseAwake();
            }, Qt::QueuedConnection);
        });
        connect(thread, &QThread::finished, thread, &QThread::deleteLater);
        thread->start();
        return;
    }

    // Clean up previous crawler
    if (m_crawler) {
        m_crawler->stop();
        m_crawler->deleteLater();
        m_crawler = nullptr;
    }

    // Use browser's shared profile for cookie sharing
    QWebEngineProfile *browserProfile = nullptr;
    if (m_window && m_window->browserView() && m_window->browserView()->page()) {
        browserProfile = m_window->browserView()->page()->profile();
    }

    m_crawler = new SiteCrawler(this, browserProfile, this);
    connect(m_crawler, &SiteCrawler::finished, this, [this]() {
        setPlatformRunning("crawl", false);
        updateStats(0, 0, "Done", "crawl");
        log("Complete.", "success", "crawl");
        closeTerminalLog("crawl");

        if (m_window) m_window->releaseAwake();

        if (m_crawler) {
            m_crawler->deleteLater();
            m_crawler = nullptr;
        }
        runJs("setRunning('crawl', false)");
    });

    // Show crawled page in browser view
    if (m_window && m_window->browserView() && m_crawler->page()) {
        m_window->browserView()->setPage(m_crawler->page());
        m_window->showBrowser(true);
    }

    m_crawler->crawl(config);
}
