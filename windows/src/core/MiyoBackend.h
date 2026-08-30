#pragma once

#include <QObject>
#include <QString>
#include <QMap>
#include <QSet>
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
class QProcess;
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
    // 수집 종료 후 로그 꼬리에서 오류 다발 감지 시 로컬 LLM 진단 (SelfRepair 연동)
    void llmDiagnoseIfBroken(const QString &platformName, const QString &trackKey);
    // 자가진단 보고서를 화면으로 (JSON 으로 감싸서 JS 에 넘긴다)
    void pushSelfDiagnosisReport(const QString &report);
    // 수집 종료 시 UI 시작/정지 버튼 동기화 통지 (CollectionGuard 소멸자에서 호출)
    void notifyCollectionEnded(const QString &platform);

    // 단일 스페이스 URL 을 outDir 에 yt-dlp 로 다운로드(스페이스 자동탐지에서도 재사용). 성공 시 true.
    //   running: 중지 판단용 실행 플래그(병렬 트랙 flag). nullptr 이면 platformRunning("twitter") 사용.
    bool downloadSpaceUrl(const QString &url, const QString &outDir, const bool *running = nullptr);

signals:
    void jsSignal(const QString &js);
    void logSignal(const QString &message, const QString &type, const QString &platform);

public slots:
    // Config
    void loadConfig();
    void saveConfig(const QString &configJson);
    void saveFormData(const QString &formJson);
    void loadFormData();

    // 로컬 AI (자가진단 LLM) — 번들 llama-server 수동 제어 (설정 탭 토글)
    void startLocalLlm(const QString &modelHint);
    void stopLocalLlm();
    bool killLlmOnPort();   // 8737 을 문 llama-server 정리(고아 포함)
    void getLlmStatus();
    void llmChat(const QString &historyJson);       // 로컬 AI 와 대화(수리 도우미) — JS onLlmReply(text)
    // ── AI 방식(로컬/온라인) 설정 — 설정 화면에서 부른다 ────────────────────
    //   온라인은 OpenAI 호환 API 면 무엇이든 붙는다(제공자별 코드 없음).
    void setAiMode(const QString &mode);                                  // "local" / "online"
    void setAiOnlineConfig(const QString &baseUrl, const QString &apiKey, const QString &model);
    void getAiConfig();                                                    // JS onAiConfig(json)
    void testAiOnline();                                                   // 연결 시험 → JS onAiTestResult(ok, msg)
    void openLlmTerminal();                          // 오픈클로를 대화형 터미널 REPL 로 띄움
    void setWindowChrome(bool dark);                 // 웹 테마 토글 → 창 배경 동기화
    void setLlmModel(const QString &hint);           // 드롭다운 선택 모델 기억 (자동기동 경로가 이걸 사용)
    void autoRepair();                               // AI 가 자가진단→수리동작을 스스로 판단해 자동 실행

    // ── 산출물 보관함 (수집해 둔 파일을 색인해 두고 질문으로 찾는다) ──────────
    void archiveStatus();                            // JS onArchiveStatus(json) — 색인 규모·AI 상태
    void archiveIndex(const QString &root);          // 색인 만들기/갱신 (증분). 진행은 onArchiveProgress
    void archiveIndexCancel();                       // 진행 중인 색인 중단
    void archiveAsk(const QString &question);        // 질문 → JS onArchiveAnswer(json)
    void setSearchKey(const QString &key);           // 웹 검색 API 키(Brave) 저장 — 읽기전용 인터넷 검색용
    void setLlmUseWeb(bool on);                       // AI 답변 시 웹 검색 참고 on/off (읽기전용)
    void getScriptSource(const QString &name);       // 편집가능 스크립트 원문 → JS onScriptSource
    void aiPatchScript(const QString &name, const QString &newContent); // 백업→적용→문법검증→실패시 원복
    void revertScript(const QString &name);          // AI override 삭제 → 번들 원본 복원
    void aiFixScript(const QString &name, const QString &problem);      // AI 가 스크립트 읽고 고쳐 적용(안전)

    // Check if any collection is running
    bool isAnyRunning() const;

    // Navigation
    void browsePath(const QString &platform);
    void openFolder(const QString &path);
    // ★ 진단 로그 자리는 운영체제마다 다르다. HTML 에는 전처리기가 없어서
    //   경로를 JS 에 박아 두면 한쪽에서 반드시 틀린다 (실제로 맥 경로가 박혀 있었다).
    //   어느 폴더를 열지는 여기서 정한다.
    void openDiagnosticsFolder();
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

    // 니코니코동화(니코동) — yt-dlp 파이프라인 재사용 (맥에서 온 지령 포팅)
    void startNiconico(const QString &configJson);
    void stopNiconico();

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
    // ── 프록시(VPN) ────────────────────────────────────────────────
    Q_INVOKABLE void getProxyProfiles();                       // UI 로 목록 전달
    Q_INVOKABLE void setProxyProfiles(const QString &json);    // 목록 저장
    Q_INVOKABLE void testProxy(const QString &json);           // 한 개 연결 시험
    //   계정에 붙은 프로필 이름으로 실제 프록시 설정을 찾는다. 없으면 빈 객체.
    QJsonObject proxyForAccount(const QJsonObject &account) const;
    QString resolveProxyUrlFor(const QString &trackKey) const;
    //   프록시를 yt-dlp / Chrome / httpx 가 쓰는 URL 한 줄로 만든다.
    static QString proxyUrl(const QJsonObject &p, bool withCredentials = true);

    void startNaikakukai(const QString &configJson);
    void stopNaikakukai();
    bool isNaikakukaiRunning() const { return m_naikakukaiRunning; }

    // 시스템 알림 (macOS osascript display notification — 다른 앱 쓰는 중에도 알림 옴)
    void showSystemNotification(const QString &title, const QString &body);

    // ── 자가진단 (SelfRepair) — 설정 탭에서 호출 ──────────────────
    //   보고서를 파일에만 적으면 아무도 안 본다. 화면으로 끌어낸다.
    Q_INVOKABLE void runSelfDiagnosis();        // 지금 검사 (네트워크 확인까지 전부)
    Q_INVOKABLE void loadLastSelfDiagnosis();   // 마지막 결과를 화면에 다시 띄우기
    Q_INVOKABLE void openSelfDiagnosisFolder(); // 보고서 폴더 열기

    // 디버그 진단 — 설정 탭에서 호출
    Q_INVOKABLE void getDiagnosticInfo();
    Q_INVOKABLE void killZombieChromes();

    // ★ Frameless 창 컨트롤 — 커스텀 타이틀바(JS)에서 호출
    Q_INVOKABLE void winMinimize();
    Q_INVOKABLE void winToggleMaximize();
    Q_INVOKABLE void winClose();
    Q_INVOKABLE bool winIsMaximized() const;
    Q_INVOKABLE void winStartMove();
    Q_INVOKABLE void winStartResize(int edges);  // 비트: 1=top 2=right 4=bottom 8=left

    // ★ WebDAV NAS 업로드 (Synology 등)
    Q_INVOKABLE void setWebDavConfig(const QString &url, const QString &user, const QString &pass, bool enabled);
    // NAS 업로드 설정 — 주소가 https:// 면 WebDAV, sftp:// 면 SFTP.
    Q_INVOKABLE void setNasConfig(const QString &url, const QString &user, const QString &pass,
                                  const QString &keyFile, bool enabled);
    Q_INVOKABLE void testWebDavConnection();
    // SFTP 연결 확인(rclone). UI 슬롯이 아니라 위에서 스킴을 보고 갈라져 불린다.
    void testSftpConnection(const QString &url, const QString &user, const QString &pass);
    // NAS 파일시스템이 유닉스(ext4/Btrfs)면 true — 특수문자 원문 보존.
    // false(기본)면 윈도우 호환(전각 치환)으로 NTFS·exFAT·윈도우 NAS 에서도 저장된다.
    Q_INVOKABLE void setUnixFilenames(bool on);
    // ★ 시간이 지나면 바뀌는 외부 서비스 상수(X 의 GraphQL query ID·Bearer 등)를 재빌드 없이 교체.
    Q_INVOKABLE void setApiOverride(const QString &key, const QString &value);
    Q_INVOKABLE void getApiOverrides();
    Q_INVOKABLE void openApiOverridesFile();
    void enqueueWebDavUpload(const QString &localPath);  // 캡쳐/다운로드 직후 자동 호출

    // ★ Finder 에 WebDAV 마운트 — macOS AppleScript "mount volume" 사용
    //   마운트되면 /Volumes/<공유폴더> 가 생기고 Finder 사이드바에 뜸.
    //   저장 경로를 그 폴더로 지정해 두면 다운로드가 NAS 로 직행.
    //   사용자에게 권한 거부 없음 (Finder 가 OS 차원에서 처리).
    Q_INVOKABLE void mountWebDavInFinder();
    Q_INVOKABLE void openUrl(const QString &url);   // 안내문 링크 → 기본 브라우저 (http/https 만)
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
    // ★ 원격 백업 업로드 — 사용자가 종류/주소/자격증명 직접 지정해 URL 로 직접 업로드 (마운트 불필요).
    //   webdav(https)·ftp·sftp·s3 지원 + rclone copy = 이어올리기(skip-existing + 끊긴 전송 재개) 내장. 전 플랫폼.
    //   configJson: {type,srcPath,destPath,url|host,port,user,pass,bucket,provider,region,endpoint,accessKey,secret}
    Q_INVOKABLE void startRemoteBackup(const QString &configJson);
    Q_INVOKABLE void stopRemoteBackup();
    Q_INVOKABLE void pickRemoteBackupSrc();  // 원격 백업 소스 폴더 선택 → rbk-src 필드 채움
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
    QProcess *m_llmProc = nullptr;            // 설정에서 켠 번들 로컬 LLM(llama-server) 프로세스
    // ── AI 대상(로컬/온라인)을 한 곳에서 정한다 ────────────────────────────
    //   예전엔 "http://127.0.0.1:8737" 이 16곳에 흩어져 있었다. 온라인을 넣으면서
    //   그중 하나만 빠뜨리면 "대화는 온라인인데 상태 표시는 로컬" 같은 어긋남이 난다.
    QString llmBase() const;                        // 기준 URL (뒤에 /v1/... 을 붙여 쓴다)
    QMap<QString, QString> llmHeaders() const;      // 온라인이면 Authorization, 로컬이면 빔
    QString llmOnlineModel() const;                 // 온라인에서 쓸 모델명(로컬이면 빈 문자열)

    QString m_llmModelHint;                   // 드롭다운에서 고른 모델(부분일치). 자동기동 시 이 모델을 씀.
    QProcess *m_archiveProc = nullptr;        // 산출물 색인 진행 중인 프로세스(중단용)
    std::atomic<bool> m_autoRepairBusy{false};// AI 자동 수리 중복 실행 방지
    QString m_searchKey;                      // 웹 검색 API 키(Brave). 없으면 검색 비활성.
    std::atomic<bool> m_llmUseWeb{false};     // AI 답변 시 웹 검색 참고 여부(읽기전용)
    std::atomic<bool> m_scriptFixBusy{false}; // AI 스크립트 수리 중복 방지
    bool applyScriptPatchImpl(const QString &name, const QString &newContent, QString &err); // 백업·검증·원복
    QString aiRewriteScriptSync(const QString &name, const QString &problem); // AI 가 스크립트 전체 재작성(동기, 워커스레드 전용)
    void resealBundleAfterInstall(const QString &why);  // 앱 내부 설치 후 서명 복구(Windows 는 무동작)

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
    // ★ Windows: .bat 을 자체 콘솔 창 + Predormition 자식 프로세스로 실행 (cmd /c start 분리 대신).
    //   작업관리자에서 Predormition 아래 nested, 앱 종료 시 트리 kill. 다른 OS 에선 no-op.
    void launchChildConsole(const QString &scriptPath);
    QList<QProcess*> m_childConsoleProcs;  // 실행 중인 자식 콘솔 추적 (종료 시 정리)
public:
    void closeAllTerminalLogs();
    QString m_terminalLogPath;
    QMap<QString, QString> m_terminalLogPaths;
    // 수집 모니터 창 — 트랙마다 하나씩 띄우는 별도 창(cmd 아님).
    QMap<QString, class TerminalWindow *> m_terminalWindows;
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
    // 트랙별 프록시 URL — 수집과 캐쉬가 같은 출구를 쓰게 한다.
    QMap<QString, QString> m_proxyPerTrack;
    mutable QMutex m_proxyPerTrackMutex;
    QJsonArray m_naikakukaiWatches;
    int m_naikakukaiIntervalMin = 30;
    int m_naikakukaiCursor = 0;
    std::atomic<bool> m_naikakukaiRunning{false};
    // 감시가 돌려놓은 수집인가 — 맞으면 그 수집 로그를 内閣会 창에도 같이 보여 준다.
    std::atomic<bool> m_naikakukaiMirroring{false};

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
    // ★ trackKey → 디버그 포트 고정. 예전엔 맵에 트랙을 처음 넣을 때만 포트를 배정해서,
    //   Chrome 이 죽어 재생성될 때 기본값 9223 으로 되돌아갔다. start() 는 그 포트를 쓰는
    //   프로세스를 죽이므로 다른 트랙의 멀쩡한 Chrome 을 잡아버린다.
    QMap<QString, int> m_capPortPerThread;
    int capturePortFor(const QString &trackKey);
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
    QMap<QString, int> m_platformSemCap;   // 현재 논리적 capacity (설정 변경 시 라이브 조정용)
    QMutex m_platformSemsMutex;
    QSemaphore* platformSem(const QString &platform);  // 동적 생성 / 조회 (설정 maxConcurrent 반영)

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
