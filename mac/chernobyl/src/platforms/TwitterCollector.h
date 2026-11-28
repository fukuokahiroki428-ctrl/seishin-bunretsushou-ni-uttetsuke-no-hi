#pragma once

#include <QElapsedTimer>
#include <QObject>
#include <atomic>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include <QSet>
#include <QHash>
#include <QStringList>
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
    // 글 하나만(TweetDetail 의 그 글 자신) — status: OK · RATE_LIMITED · ERROR · GONE(답은 왔는데 그 글이 없다 — 지워짐 · 막힘). r56 미디어 다시 받기
    QJsonObject fetchTweetById(const QString &tweetId, QString *status);
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
    int addExifMetadata(const QString &imagePath, const QJsonObject &tweet);   // Common::addExifMetadata 의 값(1 = 적었다)
    void setFinderComment(const QString &filePath, const QString &comment);

    // Excel export — streaming from DiskJsonBuffer (no readAll)
    void saveExcel(const QString &saveDir, const QString &target, const QJsonArray &data, const QString &suffix = "tweets");
    int saveExcelStreaming(const QString &saveDir, const QString &target, DiskJsonBuffer &buffer, const QString &suffix = "tweets");   // 새로 담은 줄 수

    // Rate limit handling
    void handleRateLimit(const QJsonArray &accounts, int &currentIdx, const std::atomic<bool> &isRunning);
    // 계정을 idx 로 바꾼다(출구 · 토큰 · 데몬) — 레이트 리밋 · 정지 계정 · 남은 요청 0 이 함께 쓴다(r56)
    void switchToAccount(const QJsonArray &accounts, int idx);
    // 이 계정의 X 상태(account_status)를 판마다 한 번 묻는다 — 정지면 이 판의 검색에서 뺀다(r56)
    void checkAccountStatus(int idx);
    // 지금 계정의 남은 요청이 0 이고 풀리기 전이면, 쉬는 다른 계정으로 미리 바꾼다(429 를 받기 전에 — r56)
    void rotateBeforeLimit(const QString &action, bool forSearch);

    // 통합 캡쳐 헬퍼 — 모든 type 분기에서 호출
    //   tweet: GraphQL/timeline에서 받은 단일 트윗 객체 (legacy + core 포함)
    //   capturesDir: <userDir>/captures (호출자가 mkpath)
    //   config: realCapture 플래그 + 쿠키 컨텍스트
    //   force: 명시적으로 캡쳐할지 (likes/bookmarks 같이 별도 폴더 쓸 때 capturesDir만 다르게)
    QJsonObject standardTweetRow(const QJsonObject &tweet, const QString &fallbackAuthor, bool *isSpaceCard = nullptr);   // 엑셀 한 줄(38칸 — tweetColumns)
    int captureTweet(const QJsonObject &tweet, const QString &capturesDir, const QJsonObject &config);   // 1 = 이번에 진짜 캡처 · 0 = 할 것 없음(끔 · 이미 받음) · -1 = 못 받음(카드로 대신)
    // ★ r56 — 카드에 붙일 미디어 색인(트윗 ID → 파일). 예전엔 카드마다 media/ 를 통째로(_complete 까지) 훑었다 —
    //   판마다 한 번 훑고, 그 뒤엔 그 글의 작성자 폴더 중 바뀐 것만 다시 본다(그 판에 새로 받은 미디어).
    struct CardMediaIndex {
        QHash<QString, QStringList> byId;   // 트윗 ID → 절대 경로들
        QHash<QString, qint64> dirStamp;    // 본 폴더 → 그때의 수정 시각(ms)
        QHash<QString, qint64> scanAt;      // 본 폴더 → 훑은 시각(ms) — 시각이 거친 디스크에서 같은 초에 들어온 것을 다시 본다
        QStringList kindDirs;               // media/ 바로 밑 갈래 폴더(_complete 제외)
        QSet<QString> forced;               // 있어야 할 수보다 적어 시각과 상관없이 다시 본 글(판마다 한 번)
        bool built = false;
    };
    QHash<QString, CardMediaIndex> m_cardMediaIndex;   // media 뿌리 → 색인 (최상위 판마다 비운다)
    // expected: 이 글에 있어야 할 미디어 수(모르면 -1). 찾은 것이 적으면 그 글의 폴더를 한 번 더 본다(r56 검토)
    QStringList cardMediaFor(const QString &mediaRoot, const QStringList &tweetIds, const QStringList &authors,
                             int expected = -1);

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
    // ★ 이 계정의 X 사용자 번호(설정 → 계정 토큰 자동 추출 → Twitter 가 적어 둔 것)와 그 번호가 어느 토큰의 것인지(sha256 앞 16자).
    //   데몬의 쿠키 자동 갱신은 이 번호와 같은 Chrome 로그인만 받는다 — 토큰과 짝이 맞을 때만 넘긴다(r54).
    QString m_expectedUid;
    QString m_expectedUidTok;
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
    quint64 m_daemonSeq = 0;       // 데몬 명령 번호(_rid) — 번호가 다른 답(앞 명령의 늦은 답)을 버린다(r54)
    int m_accountSwitchCount = 0;  // 레이트 리밋으로 계정을 바꾼 횟수 — 바닥 표시는 계정이 바뀐 걸음에서 적지 않는다(r55)

    // 새트윗 확인용
    QString m_newestTweetId;
    // 이번 판에 엑셀 저장이 실패했다 — 이어받기 표시를 앞으로 옮기지 않는다(다음 판이 그 구간을 다시 받아 엑셀에 채우게)
    bool m_excelSaveFailed = false;
    // ★ 이번 판에 마지막으로 쓴 엑셀 — 그 파일이 그대로면(경로 · 크기 · 시각) 다음 저장에서 다시 읽지 않는다. 예전엔 저장마다
    //   기존 파일을 통째로 두 번 읽고 다시 써, 큰 계정에서 판 전체가 제곱으로 느려졌다(장기 지원 점검). 줄이 아주 많으면 기억하지 않는다.
    QString m_xlCachePath;
    qint64 m_xlCacheSize = -1;
    qint64 m_xlCacheMtimeMs = 0;
    QList<QStringList> m_xlCacheRows;
    // 중간 저장(50개마다 · 몇 쪽마다)의 간격 — 지난 저장에 걸린 시간의 다섯 배, 적어도 60초(excelCheckpointDue)
    QElapsedTimer m_xlSinceSave;      // 단조 시계 — 지난 저장부터(무효 = 이번 판에 아직 저장 안 함)
    qint64 m_xlLastSaveCostMs = 0;
    bool excelCheckpointDue() const;
    // 지금 판의 중지 깃발(백엔드의 깃발 맵에 산다 — 지워지지 않는다). 데몬을 띄우는 곳에서 중지를 보려고
    const std::atomic<bool> *m_runFlag = nullptr;
    bool m_lastPageShaped = false;       // 마지막 타임라인 쪽(UserTweets · 좋아요 · 북마크)이 제 모양이었나(instructions 있고 errors 없음) —
                                         //   트윗 없는 쪽이 '목록 끝' 인지 '막힌 응답' 인지 가른다
    int m_mediaFailures = 0;             // 받지 못한 미디어 수 — 좋아요 · 북마크 · 리포스트 걸음의 '끊김 없이 다 봄' 판정
    QSet<QString> m_allKnownSnapshot;    // '전체' 판을 시작하기 전의 _complete 엑셀 ID — 하위 판(트윗 → 답글)이 서로의 것을 '이미 받은' 으로 세지 않게
    bool m_allKnownSnapshotSet = false;
    QStringList m_runReplyIds;           // 이번 판(전체면 하위 판 모두)에 새로 받은 답글 ID — Phase 4(대화 미디어)가 읽는다(r56: 저장이 비운 버퍼를 읽던 것)
    bool m_lastThreadClean = false;      // 마지막 스레드 판(type=thread)이 끊김 없이 다 봤나 — 스레드 자동탐지가 '다 엮음' 으로 적을지 가른다(r56, 검토 B-1)
    bool m_subUserMissing = false;       // '전체' 의 하위 판이 사용자를 찾지 못했다 — '전체' 를 멈춘다(r56: 하위 판 사이의 다시 찾기를 뺐다)
    QSet<QString> m_allArmedLists;       // '전체' 판을 시작할 때 있던 tw_tweets_* 표시(search · timeline · replies) — 하위 판이 나눠 쓴다
    QString m_currentTarget;
    QString m_currentUserDir;

    // Excel 구분선: 같은 수집 세션 내에서 한 번만 삽입
    QSet<QString> m_sessionSeparatorWritten;  // suffix별 추적

    // 계정별 Rate Limit 추적 (인덱스 → 마지막 RL 시각)
    QMap<int, qint64> m_accountRateLimitTime;

    // ── r56 계정 상태 · 레이트 리밋 머리 ──
    QJsonArray m_accounts;               // 이번 판의 계정들(collect 가 넣는다 — 새 트윗 확인 · 팔로잉 가져오기에서는 비운다: 계정을 돌리지 않는다)
    int m_activeAccountIdx = 0;          // 지금 데몬이 쓰는 계정(돌릴 때마다 바뀐다 — 각 단계의 currentAccountIdx 는 이것에 맞춘다)
    QSet<int> m_accountChecked;          // 이번 판에 account_status 를 물은 계정
    QSet<int> m_searchBlocked;           // 이번 판에 검색이 막힌 계정(X 가 정지 상태로 보아 검색 문을 닫았다)
    bool m_searchAllBlockedLogged = false;   // '모든 계정이 검색을 막혔다' 를 이번 판에 이미 알렸다
    int m_rlRemaining = -1;              // 마지막 답의 x-rate-limit-remaining(-1 = 모름)
    qint64 m_rlReset = 0;                // 마지막 답의 x-rate-limit-reset(유닉스 초, 0 = 모름)
    QString m_rlAction;                  // 그 답의 동작(search · user_tweets …) — 머리는 끝점마다 따로다
    QString m_lastApiAction;             // 마지막으로 보낸 명령의 동작 — 429 의 풀림 시각 · 검색이 막힌 계정 건너뛰기를 그 동작에 맞춘다
    QMap<int, qint64> m_accountFreeAt;   // 계정이 다시 쓸 수 있게 되는 때(유닉스 초) — 알면 풀림 시각 + 5초, 모르면 429 뒤 15분
    QSet<QString> m_pinnedIds;           // 대상 계정이 고정한 트윗 ID(사용자 조회의 pinned_tweet_ids_str) — 엑셀 종류 앞에 📌
    bool m_pinnedKnown = false;          // 사용자 조회가 pinned_tweet_ids_str 를 실제로 읽었다 — 그때만 엑셀의 📌 를 지금 고정한 글에 맞춘다(r56 검토 DC-2)
};
