#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QPair>
#include <QProcess>
#include <QMutex>
#include <atomic>

class MiyoBackend;
class HttpClient;
class DiskJsonBuffer;

class TwitterCollector : public QObject
{
    Q_OBJECT

public:
    explicit TwitterCollector(MiyoBackend *backend, QObject *parent = nullptr);
    ~TwitterCollector() override;

    void collect(const QJsonObject &config, bool &isRunning);
    void checkNewPosts(const QJsonObject &config, bool &isRunning);
    void stopDaemon();
    // 외부(MiyoBackend)에서 중지 시 프로세스를 즉시 죽이기 위해 노출
    // ★ QProcess 를 건드리지 않는다 — 다른 스레드 것일 수 있다.
    //   pid 는 start 직후 여기에 베껴 두고, 이후로는 이 값만 쓴다.
    qint64 daemonPid() const { return m_daemonPid.load(); }
    // ★ 죽인 뒤에는 반드시 지운다. 안 지우면 두 번째 중지가 '이미 죽은 pid' 에
    //   taskkill /F /T 를 다시 쏜다. 그 사이 OS 가 그 번호를 재사용했다면
    //   남의 프로세스 트리를 통째로 죽인다.
    void forgetDaemonPid() { m_daemonPid.store(0); }
    QString newestTweetId() const { return m_newestTweetId; }

private:
    // Twitter GraphQL API endpoints
    static const QString GRAPHQL_BASE;
    // 외부 서비스 상수를 런타임 교체(api_overrides.json) — X 가 query ID 를 바꿔도 재빌드 불필요
    static QString apiUrl(const QString &key, const QString &builtin);
    // ★ 작성자 이름은 반드시 이 둘로만 읽는다. 직접 ["legacy"]["screen_name"] 을 보지 말 것.
    //   X 가 2025년에 user 객체를 개편해 그 자리를 비웠다 — 아래 .cpp 의 설명 참고.
    static QString screenNameOf(const QJsonObject &userResult);
    static QString displayNameOf(const QJsonObject &userResult);
    static const QString SEARCH_TIMELINE_URL;
    static const QString USER_BY_SCREEN_NAME_URL;
    static const QString USER_TWEETS_URL;

    // Auth & client setup
    void setupClient(const QString &authToken, const QString &ct0);
    QMap<QString, QString> getHeaders() const;
    QMap<QString, QString> getHeadersWithTid(const QString &urlPath);

    // Persistent Python daemon (twikit) - handles TID per request
    bool startDaemon();
    // stopDaemon() is public (above)
    QJsonObject sendDaemonCommand(const QJsonObject &cmd, int timeoutMs = 60000);

    // Legacy: X-Client-Transaction-Id generation (kept as fallback)
    bool initTransactionIds();
    QString getTransactionId(const QString &urlPath);
    QJsonObject callTwikitApi(const QJsonObject &args);

    // API calls
    QJsonObject getUserByScreenName(const QString &screenName);
    // Returns {tweets, nextCursor}
    QPair<QJsonArray, QString> searchTweets(const QString &query, const QString &cursor = QString());
    QPair<QJsonArray, QString> getUserTweets(const QString &userId, const QString &cursor = QString());
    QPair<QJsonArray, QString> getTweetDetail(const QString &tweetId, const QString &cursor = QString());
    // 전체 수집(all) 후속 자동탐지 — 저장된 _complete.xlsx 를 스캔
    void collectSpacesFromTimeline(const QJsonObject &config, const QString &target, const QString &userDir, bool &isRunning);
    void collectThreadsAuto(const QJsonObject &config, const QString &target, const QString &userDir, bool &isRunning);
    QPair<QJsonArray, QString> getLikes(const QString &userId, const QString &cursor = QString());
    QPair<QJsonArray, QString> getBookmarks(const QString &cursor = QString());
    QPair<QJsonArray, QString> getHighlights(const QString &userId, const QString &cursor = QString());
    // Tweet engager lists: who liked / retweeted a specific tweet
    QPair<QJsonArray, QString> getFavoriters(const QString &tweetId, const QString &cursor = QString());
    QPair<QJsonArray, QString> getRetweeters(const QString &tweetId, const QString &cursor = QString());
    QPair<QJsonArray, QString> getListTweets(const QString &listId, const QString &cursor = QString());
    QPair<QJsonArray, QString> getCommunityTweets(const QString &communityId, const QString &cursor = QString());
    QString extractCursorFromEntries(const QJsonArray &entries) const;

    // Media download
    bool downloadMedia(const QString &url, const QString &filePath);
    int downloadTweetMedia(const QJsonObject &tweet, const QString &mediaDir, int tweetIdx);

    // RT user profile/banner download (anipo style)
    void downloadUserProfileMedia(const QJsonObject &tweet, const QString &profileDir, const QString &category = "tweets");
    void saveProfileExcel(const QString &saveDir, const QString &target);
    QSet<QString> m_downloadedProfiles;  // track downloaded user profile pics
    DiskJsonBuffer *m_profileBuffer = nullptr;  // disk-based profile data (no RAM bloat)

    // EXIF metadata
    void addExifMetadata(const QString &imagePath, const QJsonObject &tweet);
    void setFinderComment(const QString &filePath, const QString &comment);

    // Excel export — streaming from DiskJsonBuffer (no readAll)
    void saveExcel(const QString &saveDir, const QString &target, const QJsonArray &data, const QString &suffix = "tweets");
    void saveExcelStreaming(const QString &saveDir, const QString &target, DiskJsonBuffer &buffer, const QString &suffix = "tweets");

    // Rate limit handling
    void handleRateLimit(const QJsonArray &accounts, int &currentIdx, bool &isRunning);

    // 통합 캡쳐 헬퍼 — 모든 type 분기에서 호출
    //   tweet: GraphQL/timeline에서 받은 단일 트윗 객체 (legacy + core 포함)
    //   capturesDir: <userDir>/captures (호출자가 mkpath)
    //   config: realCapture 플래그 + 쿠키 컨텍스트
    //   force: 명시적으로 캡쳐할지 (likes/bookmarks 같이 별도 폴더 쓸 때 capturesDir만 다르게)
    void captureTweet(const QJsonObject &tweet, const QString &capturesDir, const QJsonObject &config);

    MiyoBackend *m_backend;
    // ★ 이 스레드에서 쓸 HttpClient 를 새로 만든다 — 아래 정의의 설명 참고.
    void adoptHttpToCurrentThread();

    HttpClient *m_http;

    // Adaptive delay — auto-adjusts based on rate limit responses
    double m_delay = 2.0;
    int m_consecutiveOk = 0;  // consecutive successful requests → decrease delay
    int m_rateLimitHits = 0;  // consecutive rate limit hits → increase wait time
    QString m_mediaQuality = "orig";    // orig, 4096x4096, large, medium, small
    bool m_downloadMedia = true;
    bool m_saveExcel = true;
    bool m_saveExif = true;
    bool m_saveProgress = false;  // 이어서 수집 기능 (진행 상태 파일 저장)

    // Current auth
    QString m_authToken;
    QString m_ct0;
    // 프록시(VPN) URL — 계정별로 다르게 나가게 하는 값.
    QString m_proxyUrl;
    public: void setProxy(const QString &u) { m_proxyUrl = u; }

    QString m_csrfToken;
    // 수집 중 발견한 스페이스 카드 트윗의 status URL (전체수집 끝에 yt-dlp 로 일괄 다운로드)
    QSet<QString> m_spaceCardTweetUrls;

    // Transaction IDs cache (legacy)
    QMap<QString, QString> m_transactionIds;
    bool m_tidInitialized = false;

    // Persistent daemon process
    QProcess *m_daemon = nullptr;
    // 워커가 쓰고 메인이 읽고 지운다 — atomic 이어야 한다.
    std::atomic<qint64> m_daemonPid{0};   // stopDaemon 이 객체 없이도 끊을 수 있게
    // ★ m_daemon 을 만들고 없애는 것은 여러 스레드가 동시에 한다 — 반드시 이걸 잠그고.
    QMutex m_daemonMutex;
    bool m_daemonReady = false;
    int m_daemonRequestCount = 0;  // periodic restart for memory hygiene

    // 새트윗 확인용
    QString m_newestTweetId;
    QString m_currentTarget;
    QString m_currentUserDir;

    // Excel 구분선: 같은 수집 세션 내에서 한 번만 삽입
    QSet<QString> m_sessionSeparatorWritten;  // suffix별 추적

    // 계정별 Rate Limit 추적 (인덱스 → 마지막 RL 시각)
    QMap<int, qint64> m_accountRateLimitTime;
};
