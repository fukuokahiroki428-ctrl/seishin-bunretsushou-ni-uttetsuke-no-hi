#pragma once

#include <QObject>
#include <QString>
#include <QStringList>
#include <QMap>
#include <QMutex>
#include <QSemaphore>
#include <QJsonObject>
#include <QJsonArray>
#include <atomic>

class MainWindow;
class Config;
class QTimer;
class RealChromeCrawler;
class WebDavUploader;

// ═════════════════════════════════════════════════════════════════════════
// PenBackend — 팬을 잘 쓰고 싶다 (Pen)
// 인터랙티브 CDP 크롤러 전용. 사용자가 본인 손으로 페이지 돌아다니면서
// SingleFile 캡처. 캡챠/로그인/age-gate는 사용자가 직접 풂.
// ═════════════════════════════════════════════════════════════════════════
class PenBackend : public QObject
{
    Q_OBJECT

public:
    explicit PenBackend(MainWindow *window, QObject *parent = nullptr);
    ~PenBackend() override;

    void runJs(const QString &js);
    void log(const QString &message, const QString &type = "info", const QString &platform = QString());

signals:
    void jsSignal(const QString &js);
    void logSignal(const QString &message, const QString &type, const QString &platform);

public slots:
    // 설정/저장
    void loadConfig();
    void saveConfig(const QString &configJson);
    void saveFormData(const QString &formJson);
    void loadFormData();

    // 파일 시스템
    void browseSavePath();
    void openFolder(const QString &path);

    // 클립보드
    void pasteToField(const QString &fieldId);

    // 시스템 정보
    void getSystemInfo();

    // ═════════════════════════════════════════════════════════════════
    // 인터랙티브 크롤러 — CDP 기반, 사용자가 직접 조작
    // ═════════════════════════════════════════════════════════════════
    //   savePath: 캡처 저장 폴더 (생략 시 ~/Downloads/Crawl)
    //   useUserProfile: true면 사용자 본인 Chrome 프로필 사용 (저장된 로그인/쿠키 그대로)
    //                   false면 펜 전용 영구 프로필 (로그인 한 번만 하면 다음에도 유지)
    void crawlStart(const QString &startUrl, const QString &savePath = QString(), bool useUserProfile = false);
    void crawlStop();

    // 사용자 손으로 페이지 navigate (자동 캡쳐 안 함)
    void crawlNavigate(const QString &url);
    // 현재 페이지를 SingleFile로 캡쳐 → savePath/captures/{filename}.html
    void crawlCaptureCurrent(const QString &filename = QString());
    // page 내 element 클릭 (CSS selector)
    void crawlClick(const QString &cssSelector);
    // input/textarea에 텍스트 입력 (CSS selector)
    void crawlType(const QString &cssSelector, const QString &text);
    // JS 평가 (디버그/스크립팅용) — 결과는 console.log로 출력
    void crawlEvaluate(const QString &js);
    // 끝까지 스크롤 (lazy-load 강제 트리거)
    void crawlScrollAll();

    // 미러 자동 시도 — primary URL 죽으면 후보 미러 순차 시도
    void crawlTryMirrors(const QStringList &urls, const QString &savePath = QString(), bool useUserProfile = false);

    // ★ 트위터 전용 캡쳐 — virtualized scroll로 모든 article 수집,
    //   정적 DOM으로 재구성한 뒤 SingleFile 캡쳐. 로그인/캡챠 필요하면 자동 일시정지.
    //   트윗 URL이면 펜이 알아서 처리. 일반 페이지는 crawlCaptureCurrent 쓰면 됨.
    void crawlCaptureTweet(const QString &tweetUrl, const QString &filename = QString());

    // ★ 여러 트윗 URL 순차 캡쳐 (한 줄당 하나)
    //   같은 Chrome 세션 재사용 — 첫 트윗에서 로그인 풀면 나머진 자동 진행.
    void crawlCaptureTweetBatch(const QStringList &tweetUrls);

    // 캡쳐 직전 사용자가 캡챠/로그인 풀 시간 — Chrome 띄운 채 아무것도 안 함
    //   사용자가 UI에서 "준비 완료" 누를 때까지 대기 신호 보내기
    void crawlPauseForUser(int seconds = 0);  // 0 = 사용자 명시적 신호까지 무한
    void crawlResumeFromUser();  // UI에서 "준비 완료" 눌렀을 때 호출

    // ★ 자동 크롤 — 시작 URL에서 같은 도메인 링크 따라가며 N페이지 자동 캡쳐
    //   maxPages: 최대 페이지 수 (0 = 무제한)
    //   sameDomain: true면 같은 도메인만, false면 모든 외부 링크
    //   downloadMedia: true면 페이지 내 이미지/비디오 url 따로 다운로드
    void crawlAuto(const QString &startUrl, int maxPages, bool sameDomain, bool downloadMedia);

    // ★ 현재 페이지에서 이미지/비디오만 다운로드 — img/video/source/background-image 자동 추출
    void crawlDownloadMedia();

    // 자동 크롤 중지 (crawlStop과 별도 — 큐 정리만)
    void crawlAutoStop();

    // ★ WebDAV NAS 업로드
    void setWebDavConfig(const QString &url, const QString &user, const QString &pass, bool enabled);
    void testWebDavConnection();
    void enqueueWebDavUpload(const QString &localPath);

    // ★ Finder에 WebDAV 마운트 (macOS) — AppleScript "mount volume"
    //   /Volumes/<공유폴더> 생성 + 사이드바 표시 → 그 폴더를 저장 경로로 쓰면 NAS 직행.
    //   OS 가 인증/권한 처리 → "권한 없음" 우회.
    void mountWebDavInFinder();
    void openSecurityPrefs();
    void listMountedVolumes();  // NAS/외장 드롭다운 채움
    void pickMountedVolume(const QString &targetInputId);  // Qt native dialog 선택

    // ★ 사이트 통째 미러 (wget -mk / HTTrack 현대식 — Chrome 으로 JS-rendered 페이지도 캡쳐)
    //   BFS 로 같은 도메인 링크 따라가며 각 페이지 SingleFile 저장 →
    //   완료 후 각 HTML 의 a[href] 재작성 → 로컬 mirror 안에서 클릭으로 네비게이션 가능 →
    //   index.html 생성 (전체 페이지 목록).
    void crawlSiteMirror(const QString &startUrl, int maxPages, bool sameDomain, bool downloadMedia, int maxDepth = 999);

    // ★ 진짜 풀 미러 — 소스 코드 그대로 분리 파일 구조로 보존 (wget -mkp 동등 + Chrome JS 렌더).
    //   각 페이지 raw HTML + 페이지 안 모든 리소스 (CSS/JS/이미지/폰트) 별도 파일로 다운로드 →
    //   URL → 로컬 상대경로 치환 → 원본 사이트 디렉토리 구조 그대로 보존.
    //   결과: domain.com/path/file.html 처럼 wget -m 동일 구조 + JS-rendered 컨텐츠도 포함.
    void crawlDeepMirror(const QString &startUrl, int maxPages, bool sameDomain, int maxDepth = 999);

    // ★ Twitter/X 통째 미러 — 사용자 프로필 자동 스크롤 + 모든 트윗 캡쳐 + 링크 재작성.
    //   원본 트위터 사이트 클릭 동작 그대로 (트윗 클릭 → 해당 트윗 페이지) 로컬에서 작동.
    //   입력: https://x.com/username (또는 twitter.com/...)
    //   동작:
    //     1) 프로필 페이지 navigate + 자동 스크롤 (maxScrolls 까지)
    //     2) DOM 에서 모든 트윗 URL (article[data-testid=tweet] a) 추출
    //     3) 프로필 페이지 SingleFile 캡쳐
    //     4) 각 트윗 URL 순차 캡쳐 (crawlCaptureTweet 동일 로직)
    //     5) 프로필 HTML 안의 트윗 링크 → 로컬 파일 경로 치환 → 클릭으로 네비게이션
    void crawlTwitterMirror(const QString &profileUrl, int maxScrolls);

private slots:
    void executeJsMainThread(const QString &js);
    void appendLogMainThread(const QString &message, const QString &type, const QString &platform);

private:
    MainWindow *m_window;
    Config *m_config;

    // Log batching
    struct PendingLog { QString message; QString type; };
    QMap<QString, QList<PendingLog>> m_pendingLogs;
    QTimer *m_logFlushTimer = nullptr;
    void flushLogs();

    // 크롤러 인스턴스
    RealChromeCrawler *m_crawlChrome = nullptr;
    QString m_crawlSavePath;
    QMutex m_crawlMutex;
    int m_crawlPageCounter = 0;
    QString s_singleFileLibCode;  // SingleFile lib 캐시

    // 사용자 일시정지 (캡챠/로그인 대기) — QSemaphore로 동기화
    QSemaphore m_userPauseSem{0};
    std::atomic<bool> m_waitingForUser{false};

    // 배치 캡쳐 — crawlCaptureTweet 워커가 끝났을 때 신호 (90초 고정 대기 회피)
    QSemaphore *m_batchDoneSem = nullptr;

    // ★ 자동 크롤 큐 상태
    std::atomic<bool> m_autoCrawlStop{false};
    int m_autoPageCount = 0;
    int m_autoMediaCount = 0;

    // WebDAV uploader
    WebDavUploader *m_webdav = nullptr;

    // 사이트 미러 — URL → 로컬 파일 경로 매핑 (링크 재작성용)
    QMap<QString, QString> m_mirrorUrlMap;

    // 터미널 로그 — 별도 Terminal.app 창에서 실시간 로그 표시 (Chernobyl 패턴)
    QString m_terminalLogPath;
    void openTerminalLog();
    void writeTerminalLog(const QString &message);
    void closeTerminalLog();
};
