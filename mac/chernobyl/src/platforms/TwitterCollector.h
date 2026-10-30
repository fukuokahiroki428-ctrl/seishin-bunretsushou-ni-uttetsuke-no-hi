#pragma once

#include <QObject>
#include <atomic>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QPair>
#include <QProcess>
#include <QMutex>

class HanishikiBackend;
class HttpClient;
class DiskJsonBuffer;

class TwitterCollector : public QObject
{
    Q_OBJECT

public:
    explicit TwitterCollector(HanishikiBackend *backend, QObject *parent = nullptr);
    ~TwitterCollector() override;

    void collect(const QJsonObject &config, const std::atomic<bool> &isRunning);
    void checkNewPosts(const QJsonObject &config, const std::atomic<bool> &isRunning);
    // 한 사람의 팔로잉 목록만(아무것도 받지 않는다) — 대상 목록 '팔로잉에서 가져오기' 용.
    //   돌려주는 것: [{handle, name, protected}]. maxCount 0 = 끝까지. 문제가 있으면 err 에 까닭.
    //   pidOut: 띄운 도우미의 PID — 그만 · 앱 끄기가 막힌 요청을 바로 끊을 수 있게(0 이면 없음)
    QJsonArray listFollowing(const QJsonObject &config, const QString &handle, int maxCount,
                             const std::atomic<bool> &isRunning, QString *err, std::atomic<qint64> *pidOut = nullptr);
    void stopDaemon();
    // 외부(HanishikiBackend)에서 중지 시 프로세스를 즉시 죽이기 위해 노출
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
    // 외부 서비스 상수를 런타임에 교체(api_overrides.json) — X 가 query ID 를 회전시켜도 재빌드 불필요
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
    void setupClientFor(const QJsonObject &account);    // 이 계정의 출구를 건 뒤 setupClient — 계정을 정해 데몬을 띄우는 곳은 모두 이것으로
    void applyAccountProxy(const QJsonObject &account);
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
    void collectSpacesFromTimeline(const QJsonObject &config, const QString &target, const QString &userDir, const std::atomic<bool> &isRunning);
    void collectThreadsAuto(const QJsonObject &config, const QString &target, const QString &userDir, const std::atomic<bool> &isRunning);
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
    int saveExcelStreaming(const QString &saveDir, const QString &target, DiskJsonBuffer &buffer, const QString &suffix = "tweets");   // 새로 담은 줄 수

    // Rate limit handling
    void handleRateLimit(const QJsonArray &accounts, int &currentIdx, const std::atomic<bool> &isRunning);

    // 통합 캡쳐 헬퍼 — 모든 type 분기에서 호출
    //   tweet: GraphQL/timeline에서 받은 단일 트윗 객체 (legacy + core 포함)
    //   capturesDir: <userDir>/captures (호출자가 mkpath)
    //   config: realCapture 플래그 + 쿠키 컨텍스트
    //   force: 명시적으로 캡쳐할지 (likes/bookmarks 같이 별도 폴더 쓸 때 capturesDir만 다르게)
    QJsonObject standardTweetRow(const QJsonObject &tweet, const QString &fallbackAuthor, bool *isSpaceCard = nullptr);   // 엑셀 한 줄(30칸)
    int captureTweet(const QJsonObject &tweet, const QString &capturesDir, const QJsonObject &config);   // 1 = 이번에 진짜 캡처 · 0 = 할 것 없음(끔 · 이미 받음) · -1 = 못 받음(카드로 대신)

    HanishikiBackend *m_backend;
    // ★ 이 스레드에서 쓸 HttpClient 를 새로 만든다 — TwitterCollector.cpp 의 정의 설명 참고.
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
    bool m_lastPageShaped = false;       // 마지막 타임라인 쪽(UserTweets · 좋아요 · 북마크)이 제 모양이었나(instructions 있고 errors 없음) —
                                         //   트윗 없는 쪽이 '목록 끝' 인지 '막힌 응답' 인지 가른다
    int m_mediaFailures = 0;             // 받지 못한 미디어 수 — 좋아요 · 북마크 · 리포스트 걸음의 '끊김 없이 다 봄' 판정
    QSet<QString> m_allKnownSnapshot;    // '전체' 판을 시작하기 전의 _complete 엑셀 ID — 하위 판(트윗 → 답글)이 서로의 것을 '이미 받은' 으로 세지 않게
    bool m_allKnownSnapshotSet = false;
    QSet<QString> m_allArmedLists;       // '전체' 판을 시작할 때 있던 tw_tweets_* 표시(search · timeline · replies) — 하위 판이 나눠 쓴다
    QString m_currentTarget;
    QString m_currentUserDir;

    // Excel 구분선: 같은 수집 세션 내에서 한 번만 삽입
    QSet<QString> m_sessionSeparatorWritten;  // suffix별 추적

    // 계정별 Rate Limit 추적 (인덱스 → 마지막 RL 시각)
    QMap<int, qint64> m_accountRateLimitTime;
};
