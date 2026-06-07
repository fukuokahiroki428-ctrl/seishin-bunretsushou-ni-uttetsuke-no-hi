#pragma once

#include <QObject>
#include <QString>
#include <QMap>
#include <QJsonObject>
#include <QJsonArray>
#include <QMutex>
#include <QSemaphore>
#include <QNetworkCookie>
#include <QHash>
#include <QThread>
#include <atomic>

class MainWindow;
class Config;
class TwitterCollector;
class BlueskyCollector;
class SiteCrawler;
class RealChromeCrawler;
class HttpClient;
class QTimer;
class WebDavUploader;

class MiyoBackend : public QObject
{
    Q_OBJECT

public:
    explicit MiyoBackend(MainWindow *window, QObject *parent = nullptr);
    ~MiyoBackend() override;

    Config *config() const { return m_config; }

    // Thread-safe JS execution
    void runJs(const QString &js);
    void log(const QString &message, const QString &type = "info", const QString &platform = QString());
    void updateStats(int posts, int media, const QString &status, const QString &platform = QString());

    // 단일 스페이스 URL 을 outDir 에 yt-dlp 로 다운로드(스페이스 자동탐지에서도 재사용). 성공 시 true.
    bool downloadSpaceUrl(const QString &url, const QString &outDir);

signals:
    void jsSignal(const QString &js);
    void logSignal(const QString &message, const QString &type, const QString &platform);

public slots:
    // Config
    void loadConfig();
    void saveConfig(const QString &configJson);
    void saveFormData(const QString &formJson);
    void loadFormData();

    // Check if any collection is running
    bool isAnyRunning() const;

    // Navigation
    void browsePath(const QString &platform);
    void openFolder(const QString &path);
    void pasteToField(const QString &fieldId);
    void pasteClipboard();

    // External windows (kept for backward compat, now no-ops)
    void openYoutubeWindow();
    void openDiscordWindow();
    void openInstagramWindow();

    // ★ 로그인 대기 — captureRealPageCDPLoginAware가 GUI에 알림 보내고 대기.
    //   사용자가 Chrome에서 로그인 푼 다음 GUI '확인' 누르면 이 슬롯 호출 → 진행 재개.
    void confirmLoginDone(const QString &platform);

    // Collection
    void startCollection(const QString &configJson);
    void stopCollection(const QString &platformName);
    void checkNewPosts(const QString &platformName);

    // YouTube
    void startYoutube(const QString &configJson);
    void stopYoutube();
    void analyzeYoutube(const QString &url);

    // Log
    void showLog(const QString &message);

    // Trad (steganography: hide files in PNG)
    void startTrad(const QString &configJson);
    void extractTrad(const QString &configJson);
    void stopTrad();
    void selectTradFiles();
    void selectTradFolder();
    void selectTradCover();
    void getTradCoverBase64();

    // Settings
    void setTempDir(const QString &path);
    void browseTempDir();

    // 보조 저장 경로 (대용량 다운 자동 분산)
    void browseSecondaryPath();
    void getFreeSpaceGB(const QString &path);

    // Browser (crawl embedded browser)
    void showBrowser(bool show);
    void browserNavigate(const QString &url);
    void browserBack();
    void browserForward();
    void browserRefresh();
    void browserStop();
    void downloadPageMedia(const QString &configJson);
    void crawlerContinueAfterLogin();  // 크롤러 로그인 대기 상태 해제

    // System / Maintenance
    void getSystemInfo();          // 모듈 버전, 디스크 경로, 라이브러리 정보 등
    void updateModules();          // pip upgrade bundled packages
    void upgradePython();          // Python 최신 버전 다운로드 + 재설치
    void repairPython();           // Python 환경 깨졌을 때 자동 복구
    void refreshTwitterTokens();   // Chrome 쿠키에서 토큰 자동 추출
    void refreshInstagramSession(); // Chrome 쿠키에서 Instagram sessionid 자동 추출
    void refreshPixivSession();     // Chrome 쿠키에서 Pixiv PHPSESSID 자동 추출
    void refreshDiscordToken();     // Chrome Local Storage에서 Discord 토큰 자동 추출
    void refreshTumblrCookie();     // Chrome 쿠키에서 Tumblr 세션 자동 추출
    void refreshSpinSpinCookie();   // Chrome 쿠키에서 SpinSpin 세션 자동 추출
    void refreshAskedCookie();      // Chrome 쿠키에서 Asked 세션 자동 추출
    void refreshAllTokens();        // 전체 플랫폼 토큰 자동 갱신
    // 범용: 특정 도메인의 모든 쿠키 추출 → fieldId에 주입
    void refreshDomainCookies(const QString &domain, const QString &fieldId,
                              const QString &platform, const QString &label,
                              const QString &busyJsFn = QString());
    QString extractInstagramSessionSync(); // 동기 방식 세션 추출 (수집 중 401 자동 갱신용)
    void writeStartupLog();        // 앱 시작 시 상세 로그 기록

    // 内閣会 — 지정 사용자 신글 자동 감지/다운로드 (Twitter/Bluesky/Tumblr 공식 API)
    void startNaikakukai(const QString &configJson);
    void stopNaikakukai();
    bool isNaikakukaiRunning() const { return m_naikakukaiRunning; }

    // 시스템 알림 (macOS osascript display notification — 다른 앱 쓰는 중에도 알림 옴)
    void showSystemNotification(const QString &title, const QString &body);

    // 디버그 진단 — 설정 탭에서 호출
    Q_INVOKABLE void getDiagnosticInfo();
    Q_INVOKABLE void killZombieChromes();

    // ★ WebDAV NAS 업로드 (Synology 등)
    Q_INVOKABLE void setWebDavConfig(const QString &url, const QString &user, const QString &pass, bool enabled);
    Q_INVOKABLE void testWebDavConnection();
    void enqueueWebDavUpload(const QString &localPath);  // 캡쳐/다운로드 직후 자동 호출

    // ★ Finder 에 WebDAV 마운트 — macOS AppleScript "mount volume" 사용
    //   마운트되면 /Volumes/<공유폴더> 가 생기고 Finder 사이드바에 뜸.
    //   저장 경로를 그 폴더로 지정해 두면 다운로드가 NAS 로 직행.
    //   사용자에게 권한 거부 없음 (Finder 가 OS 차원에서 처리).
    Q_INVOKABLE void mountWebDavInFinder();
    Q_INVOKABLE void openSecurityPrefs();  // 권한 거부 시 시스템 설정 열기

    // ★ 마운트된 볼륨 목록 — JS UI에 NAS/외장 드롭다운 옵션 채움
    //   결과: window.onMountedVolumes([{path, name, isNetwork}, ...]) 콜백
    Q_INVOKABLE void listMountedVolumes();

    // (옛 NAS 버튼/dialog — 호환 위해 남김, UI 에서 더 이상 노출 안 됨)
    Q_INVOKABLE void pickMountedVolume(const QString &targetInputId);
    Q_INVOKABLE void setAllPathsToNas();

    // ★ 저장 모드 변경 — "local" (각 플랫폼 직접 지정) / "nas" / "external"
    //   nas/external: 마운트된 볼륨 선택 dialog → 모든 플랫폼 input 일괄 변경 + 숨김
    //   local: 입력란 복원, 사용자 직접 지정
    Q_INVOKABLE void setStorageMode(const QString &mode);

    // ★ NAS 자동 백업 — 로컬 다운로드 완료 후 NAS 마운트 폴더로 cp
    Q_INVOKABLE void setBackupConfig(bool enabled, const QString &path);
    Q_INVOKABLE void pickBackupPath();
    Q_INVOKABLE void testBackup();
    void enqueueBackup(const QString &localPath);
    // ★ 모든 플랫폼 다운로드 폴더 → NAS 로 전체 재sync (수집 끝나고 안전 확인용)
    //   각 platform 의 user-path 의 모든 파일을 idempotent cp (이미 있는 거 skip)
    Q_INVOKABLE void resyncAllFoldersToBackup();
    // ★ 지금 백업 — 백업 toggle off 여도 1회성 즉시 전체 백업.
    //   경로 미설정 시 자동으로 pickBackupPath() 띄움 (사용자 선택 후 진행).
    Q_INVOKABLE void backupNow();
    // ★ 백업 중지 — 진행 중인 모든 워커 즉시 중단 (다음 파일 pick 안 함 + 활성 cp 강제 kill)
    Q_INVOKABLE void stopBackup();
    // ★ Config 내보내기 / 불러오기 — 사용자 입력 정보 (accounts/tokens/paths/forms) 통째 JSON
    Q_INVOKABLE void exportConfig();
    Q_INVOKABLE void importConfig();
    // ★ rclone 백업 — WebDAV / SFTP / S3 / Google Drive 등 50+ protocol 빠른 전송 (mountainduck 호환)
    //   사용자의 WebDAV creds 사용해서 rclone copy 호출 → 8 parallel transfers + HTTP/2 + checksum
    void runRcloneBackup(const QStringList &srcDirs, const QString &destSubPath);
    // ★ 수집 옵션 dump — 사용자가 체크한 옵션 / 입력값 모두 로그에 기록 (디버깅/재현)
    void logCollectionOptions(const QJsonObject &config, const QString &platform);
    // ★ 다운로드 manifest — 폴더 안 모든 파일 통계 (개수 / 사이즈 / 확장자별) JSON + TXT 생성
    //   각 plat 수집 끝나면 자동 호출 — 무결성 검증 + 추후 확인 용
    void writeDownloadManifest(const QString &dir, const QString &platform);

    // ★ 이메일 알림 감시 → 매치 시 内閣会 즉시 실행
    //   30초 간격 IMAP 폴링. 새 메일이 from/subject 필터 매치하면 naikakukaiTick() 트리거.
    Q_INVOKABLE void startEmailWatch(const QString &server, int port,
                                     const QString &user, const QString &pass,
                                     const QString &filterFrom, const QString &filterSubject);
    Q_INVOKABLE void stopEmailWatch();
    Q_INVOKABLE void testEmailWatch();  // 1회 즉시 체크

private slots:
    void executeJsMainThread(const QString &js);
    void appendLogMainThread(const QString &message, const QString &type, const QString &platform);

private:
    MainWindow *m_window;
    Config *m_config;
    HttpClient *m_http;
    QMap<QString, bool> m_isRunning;
    QMap<QString, bool> m_stopRequested;  // 사용자가 명시적으로 중지 버튼을 눌렀는지 추적
    mutable QMutex m_runningMutex;  // m_isRunning 쓰레드 안전 보호
    QMutex m_realCaptureMutex;       // captureRealTweetPage 직렬화 (브라우저 단일 인스턴스)
    bool m_realCaptureCookiesInjected = false;  // 첫 캡쳐 시 한 번만 쿠키 주입
    // 병렬 다중대상에서 워커 스레드가 자기 trackKey("twitter#0", "twitter#1", ...)를
    // 등록 → log/writeTerminalLog가 현재 스레드의 trackKey로 라우팅 → 각 터미널이
    // 자기 대상 로그만 표시.
    QHash<Qt::HANDLE, QString> m_threadTrackKey;
    mutable QMutex m_threadTrackKeyMutex;
public:
    void setThreadTrackKey(const QString &trackKey);
    void clearThreadTrackKey();
    QString currentThreadTrackKey() const;
private:
    std::atomic<bool> m_tradCancelled{false};
    std::atomic<bool> m_pythonBusy{false};    // Python 환경 작업 중 (업그레이드/복구/업데이트 동시 방지)

    // 쓰레드 안전 m_isRunning 접근
    bool platformRunning(const QString &p) const {
        QMutexLocker lock(&m_runningMutex);
        return m_isRunning.value(p, false);
    }
    void setPlatformRunning(const QString &p, bool v) {
        QMutexLocker lock(&m_runningMutex);
        m_isRunning[p] = v;
    }
    QString m_currentPlatform;

    // Terminal log window
    void openTerminalLog(const QString &platform, const QString &savePath = QString());
    // 백업 전용 — 컬러 + 스피너 애니메이션 (clear + tail -n 30 + spinner refresh 200ms)
    void openBackupTerminalLog();
    void writeTerminalLog(const QString &message, const QString &platform = QString());
    void closeTerminalLog(const QString &platform = QString());
public:
    void closeAllTerminalLogs();
    QString m_terminalLogPath;
    QMap<QString, QString> m_terminalLogPaths;
    QMap<QString, qint64> m_lastStatsUpdate;

    // Log batching — 로그 배치 처리로 UI 부하 감소
    struct PendingLog { QString message; QString type; };
    QMap<QString, QList<PendingLog>> m_pendingLogs;
    QTimer *m_logFlushTimer = nullptr;
    void flushLogs();

    // 자동 유지보수 — 시작 시 정리, 종료 시 자식 프로세스 정리, 주기적 메모리 모니터
    void performStartupCleanup();
    void killChildProcesses();
    void memoryMonitorTick();
    QTimer *m_memoryMonitorTimer = nullptr;
    qint64 m_peakRssMB = 0;

    // 内閣会 내부 상태
    void naikakukaiTick();
    QTimer *m_naikakukaiTimer = nullptr;
    QJsonArray m_naikakukaiWatches;
    int m_naikakukaiIntervalMin = 30;
    int m_naikakukaiCursor = 0;
    std::atomic<bool> m_naikakukaiRunning{false};

    // NAS 백업 워커 — 큐 + 진행률 + 2-thread 병렬
    // ★ 메모리 큐 X — 사용자 임시 디스크에 append-only 텍스트 큐 파일 사용.
    //   10만+ 파일 백업해도 메모리 안 먹음 (한 줄씩 stream 처리).
    //   cursor file (offset) 로 진행 위치 추적 → 워커 재시작해도 이어서.
    QString m_backupQueuePath;    // <tempBase>/abiwa_backup_queue.txt (append-only)
    QString m_backupOffsetPath;   // <tempBase>/abiwa_backup_queue.offset (8 bytes qint64)
    qint64  m_backupQueueOffset = 0;  // 다음에 읽을 byte offset (메모리 cache, 항상 file 과 동기)
    QMutex  m_backupQueueMutex;       // 큐 파일 + offset + counts 동시 보호
    QList<QThread*> m_backupThreads;
    std::atomic<bool> m_backupRunning{false};
    // 진행률 추적 (file 큐와 별도로 atomic — UI emit 시 매번 file 스캔 안 함)
    std::atomic<qint64> m_backupTotalBytes{0};
    std::atomic<qint64> m_backupDoneBytes{0};
    std::atomic<int> m_backupTotalCount{0};
    std::atomic<int> m_backupDoneCount{0};
    std::atomic<qint64> m_backupLastProgressMs{0};
    // ★ 터미널 진행률 표시 — backupNow / resync 시 외부 Terminal.app 열어서 자세히 표시
    std::atomic<bool> m_backupTerminalActive{false};
    std::atomic<qint64> m_backupStartMs{0};    // 시작 시각 (속도/ETA 계산)
    std::atomic<qint64> m_backupSkipCount{0};  // skip 카운트 (이미 백업된 파일)
    std::atomic<qint64> m_backupFailCount{0};  // 실패 카운트
    QMutex m_backupTerminalMutex;              // 터미널 write 동시성 보호
    // 디스크 큐 helpers
    void initBackupQueuePaths();           // tempBase 변경되거나 첫 호출 시 path 설정
    void enqueueBackupItem(const QString &localPath, qint64 size);  // append + size atomic 증가
    bool dequeueBackupItem(QString &outPath);  // 다음 줄 읽기 → offset advance + 저장
    void resetBackupQueueIfDrained();      // EOF + 새 enqueue 없으면 truncate + offset=0
    void backupWorker();
    void recalcBackupTotals();  // 큐 전체 용량 사전 분석 (file 한 번 스캔)
    void emitBackupProgress();  // UI 에 진행률 전송

    // ★ NAS watchdog — 30초마다 마운트 상태 체크, 끊기면 자동 재마운트
    QTimer *m_nasWatchdogTimer = nullptr;
    bool m_nasReconnectInProgress = false;
    void nasWatchdogTick();
    void silentRemountWebDav();  // prompt 없는 재마운트 시도

    // ★ 무결성 검사 워커 — config["integrityCheck"]=true 일 때 enqueue.
    void enqueueIntegrityCheck(const QString &localPath, const QString &platform);
    void setIntegrityActiveForPlatform(const QString &platform, bool enabled);  // collector 시작 시 호출
    struct IntegrityItem { QString path; QString platform; };
    QList<IntegrityItem> m_integrityQueue;
    QMutex m_integrityQueueMutex;
    QSet<QString> m_integrityActivePlatforms;  // 토글 ON 한 플랫폼
    QThread *m_integrityThread = nullptr;
    std::atomic<bool> m_integrityRunning{false};
    void integrityWorker();

    // 이메일 IMAP 감시 상태 (内閣会 알림 트리거)
    QTimer *m_emailWatchTimer = nullptr;
    QString m_emailServer;
    int m_emailPort = 993;
    QString m_emailUser;
    QString m_emailPass;
    QString m_emailFilterFrom;
    QString m_emailFilterSubject;
    int m_emailLastUid = 0;
    void emailWatchTick();

    // Per-platform threads — 동시 병렬 수집
    QMap<QString, QThread*> m_collectionThreads;

    // Collection runners (all inline in main app)
    void runTwitterCollection(const QJsonObject &config);
    void runTwitterSpace(const QJsonObject &config);   // 트위터 스페이스(오디오) — yt-dlp 다운로드
    void runBlueskyCollection(const QJsonObject &config);
    void runDiscordCollection(const QJsonObject &config);
    void runInstagramCollection(const QJsonObject &config);
    void runYoutubeDownload(const QJsonObject &config);
    void runPixivCollection(const QJsonObject &config);
    void runWebCrawlCollection(const QJsonObject &config);  // 웹 크롤 모드 (API 대체)

    // 経済産業省 연계: 실제 페이지 캡쳐 — 워커 스레드에서 호출, 메인 스레드 QWebEngine으로
    //   tweetUrl을 로드하고 렌더된 outerHTML을 saveDir/filename.html에 저장.
    //   첫 호출 시 cookies 인자의 쿠키를 cookieStore에 주입 (logged-in 상태 보장).
    //   동시에 여러 워커가 호출하면 m_realCaptureMutex로 직렬화.
    //   returns true on success (file saved).
    bool captureRealTweetPage(const QString &tweetUrl,
                              const QString &saveDir,
                              const QString &filename,
                              const QList<QNetworkCookie> &cookies = {},
                              int waitMs = 5000);

    // CDP (실제 Chrome) 기반 캡쳐 — QWebEngine으로는 x.com 등이 anti-bot shell 반환.
    //   실제 Chrome으로 navigate → outerHTML → 저장. 같은 m_captureChrome 인스턴스를
    //   이어서 재사용 (한 번 띄우면 batch 내내 살려둠 → 페이지마다 새로 띄우는 비용 회피).
    bool captureRealPageCDP(const QString &url,
                            const QString &saveDir,
                            const QString &filename,
                            int waitMs = 6000,
                            const QList<QNetworkCookie> &cookies = {});

    // ★ 로그인 대기 가능 캡쳐 — 첫 navigate 후 로그인 페이지 감지하면 사용자가
    //   Chrome 창에서 직접 로그인 후 GUI '확인' 버튼 누를 때까지 대기.
    //   loginCheckJs: 로그인 안 됐을 때 true 반환하는 JS 표현식
    //   platform: 로그인 대기 알림 그룹 (instagram, pixiv, ...)
    //   config: 있으면 cookiesForCapture(platform, config)가 자동으로 계정 쿠키 주입
    //           → 캡쳐 시작 시 모든 플랫폼이 항상 "로그인된 상태"로 시작
    bool captureRealPageCDPLoginAware(const QString &url,
                                      const QString &saveDir,
                                      const QString &filename,
                                      const QString &loginCheckJs,
                                      const QString &platform,
                                      int waitMs = 8000,
                                      const QList<QNetworkCookie> &cookies = {},
                                      const QJsonObject &config = {});

    // ★ 플랫폼별 계정 쿠키 빌드 — 캡쳐 chrome 으로 로그인 상태 보장.
    //   config["accounts"][0] 우선 → fallback config 최상단 키 (sessionId/cookie/auth_token 등)
    //   추가로 config["captureCookie"] (사용자 입력 raw cookie) 까지 합쳐 반환.
    QList<QNetworkCookie> cookiesForCapture(const QString &platform, const QJsonObject &config) const;

    // 첫 호출 시 Network.setCookie로 쿠키 주입 (login 세션). 호출 시점에 세팅하고 이후 재사용.
    //   chromePtr: 어떤 Chrome 인스턴스에 주입할지 (null이면 현재 thread의 chromePtr).
    void injectCdpCookies(const QList<QNetworkCookie> &cookies);

    // 새 플랫폼 (Tumblr, SpinSpin, Asked, 経済産業省/크롤링)
    void runTumblrCollection(const QJsonObject &config);
    void runSpinSpinCollection(const QJsonObject &config);
    void runAskedCollection(const QJsonObject &config);
    void runCrawlCollection(const QJsonObject &config);
    // ★ Pixiv Fanbox (멤버십 컨텐츠 다운)
    void runFanboxCollection(const QJsonObject &config);
    Q_INVOKABLE void refreshFanboxSession();  // FANBOXSESSID 자동 추출
    SiteCrawler *m_crawler = nullptr;

    // 실제 Chrome 브라우저(CDP) 모드 — config["method"] == "chrome" 일 때 사용
    void runRealChromeCollection(const QJsonObject &config);
    RealChromeCrawler *m_realChrome = nullptr;
    // captureRealPageCDP — 단일 모드용 (sequential 캡쳐)
    RealChromeCrawler *m_captureChrome = nullptr;
    QMutex m_captureChromeMutex;       // 단일 Chrome 사용 시 직렬화
    bool m_captureChromeStarted = false;
    // ★ 병렬 모드: 각 trackKey마다 자기 Chrome 인스턴스 — 다른 디버그 포트로 격리
    QMap<QString, RealChromeCrawler*> m_captureChromesPerThread;
    QMutex m_capChromeMapMutex;
    int m_nextCapPort = 9223;
    // ★ 캡쳐 카운터 — N 회마다 Chrome 재시작 (메모리 누수 방지, 60+개 다운 안정성)
    QMap<QString, int> m_captureCountsPerKey;
    // RAM 제한 — 동시 실행 Chromium 1개로 제한 (메모리 우선)
    // ★ 8GB Mac OOM 방지 + 디스크 마운트 해제 방지 — 캡쳐 quality 그대로, 속도만 느려짐
    QSemaphore m_chromeCapacitySem{1};

    // ★ 동시 수집 작업 자체 제한 — 여러 플랫폼 동시 돌릴 때 메모리 중첩 방지
    //   2개 동시 OK. 3번째는 큐에서 대기. 끝나면 자동으로 다음 시작.
    QSemaphore m_collectionCapacitySem{2};  // 옛 통합 sem (호환용)
    // ★ Platform 별 동시성 제한 — 각 기능 특성에 맞춤
    //   YouTube: yt-dlp/ffmpeg 무거움 → 1
    //   Instagram: rate limit / login pause → 1
    //   Pixiv/Fanbox: 이미지 + API → 2
    //   Twitter/Bluesky: API rate limit → 2
    //   Tumblr/Discord/SpinSpin/Asked/Crawl: 비교적 가벼움 → 3
    QMap<QString, QSemaphore*> m_platformSems;
    QMutex m_platformSemsMutex;
    QSemaphore* platformSem(const QString &platform);  // 동적 생성 / 조회

    // ★ 로그인 대기 — platform별로 분리 (Instagram/Pixiv 동시 대기 가능)
    QMap<QString, QSemaphore*> m_loginPauseSems;
    QMutex m_loginPauseMutex;

    // 새트윗/새글 확인용
    QMap<QString, QJsonObject> m_lastConfig;     // platform → last collection config
    class TwitterCollector *m_twitterCollector = nullptr;
    BlueskyCollector *m_blueskyCollector = nullptr;

    // WebDAV 업로더 (NAS — Synology 등)
    WebDavUploader *m_webdav = nullptr;
};
