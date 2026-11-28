#include "TwitterCollector.h"
#include <QJsonArray>
#include <QFileInfo>
#include "core/HanishikiBackend.h"
#include "core/Common.h"
#include <QDirIterator>
#include "utils/HttpClient.h"
#include "utils/ExcelWriter.h"
#include "xlsxdocument.h"
#include "utils/FileHelper.h"
#include "utils/DiskJsonBuffer.h"
#include <QThread>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QNetworkCookie>
#include <QUrl>
#include <QUrlQuery>
#include <QDateTime>
#include <QTimeZone>
#include <QSaveFile>
#include <algorithm>
#include <vector>
#include <limits>
#include <QProcess>
#include <QProcessEnvironment>
#include <QCoreApplication>
#include <QStandardPaths>
#include <QElapsedTimer>
#include <QCryptographicHash>
#include <QRegularExpression>
#include <QSet>
#include <QQueue>
#include <functional>
#include <QHash>
#ifdef Q_OS_MACOS
#include <sys/xattr.h>
#endif

#include "utils/JsonShape.h"
#include "utils/SelfRepair.h"

// 화면의 '기간 지정' — 일본 시간의 하루 단위. 0 = 기간 안(또는 기간 판이 아님 · 시각을 모름), 1 = 더 새것, -1 = 더 옛것.
//   좋아요 · 북마크처럼 날짜 순이 아닌 목록에서 쓴다(본 걸음은 processTweetBatch 가 거른다).
static int tweetPeriodSide(const QJsonObject &config, const QString &createdAt)
{
    if (config["mode"].toString() != QLatin1String("period")) return 0;
    const QDateTime t = Common::parseISODate(createdAt);
    if (!t.isValid()) return 0;
    const QString since = config["since"].toString(), until = config["until"].toString();
    if (!until.isEmpty() && t > QDateTime::fromString(until + "T23:59:59+09:00", Qt::ISODate)) return 1;
    if (!since.isEmpty() && t < QDateTime::fromString(since + "T00:00:00+09:00", Qt::ISODate)) return -1;
    return 0;
}

// ── X 응답을 '이름 하나' 로 읽지 않는다 ─────────────────────────────────────
//
// X 는 응답 이름을 주기적으로 바꾼다. 이 파일에도 그 흔적이 남아 있다 —
//   bookmark_timeline → bookmark_timeline_v2
//   threaded_conversation_with_injections → …_v2
//   timeline → timeline_v2
// 한 번 바뀐 이름은 또 바뀐다. 그런데 정작 알맹이를 꺼내는 세 이름
// (itemContent · tweet_results · legacy) 은 이 파일 안에 열두 벌 넘게 복사돼
// 박혀 있었다. 하나만 바뀌면 검색·유저트윗·좋아요·북마크·팔로워·스레드가
// 동시에 0개가 되고, 화면에는 '수집 완료 0개' 라고만 뜬다.
// 팬박스가 그런 식으로 여덟 달을 조용히 비어 있었다(2026-09-24).
//
// 그래서 꺼내는 자리를 여기 한 곳에 모으고, 이름을 여러 개 댄다.
namespace {

// 타임라인의 instructions — 껍데기 이름이 갈래마다 다르고 자주 바뀐다.
//   roots  : data 바로 아래에서 찾을 이름들
//   howOut : 아는 이름으로 못 찾아 모양을 훑어 찾았을 때 그 자리를 적어 준다(빈 문자열이면 정상)
QJsonArray xInstructions(const QJsonObject &body, const QStringList &roots, QString *howOut = nullptr)
{
    if (howOut) howOut->clear();
    const QJsonObject data = body.value("data").toObject();
    // data → (root) → [result] → (timeline 계열) → [timeline] → instructions
    QJsonObject cur = JsonShape::pickObject(data, roots);
    if (cur.isEmpty() && !data.isEmpty()) cur = data;
    cur = JsonShape::unwrap(cur, {"result"});
    QJsonObject tl = JsonShape::pickObject(cur, {"timeline_v2", "timeline", "timeline_v3",
                                                 "timeline_response", "search_timeline",
                                                 "community_timeline", "favoriters_timeline",
                                                 "retweeters_timeline", "bookmark_timeline_v2",
                                                 "bookmark_timeline"});
    if (!tl.isEmpty()) cur = tl;
    cur = JsonShape::unwrap(cur, {"timeline"});
    QJsonArray ins = JsonShape::pickArray(cur, {"instructions"});
    if (!ins.isEmpty()) return ins;
    // 아는 이름이 하나도 없다 — 모양을 훑어 instructions 처럼 생긴 배열을 찾는다.
    QString foundAt;
    const QJsonArray guess = JsonShape::findArrayOfObjects(data, &foundAt, 6);
    if (howOut) *howOut = foundAt;
    return guess;
}

// entry 하나에서 트윗 알맹이 — itemContent / tweet_results / result 세 이름.
QJsonObject xTweetResult(const QJsonObject &entryOrItem)
{
    QJsonObject c = JsonShape::pickObject(entryOrItem, {"content", "item"});
    if (c.isEmpty()) c = entryOrItem;
    c = JsonShape::unwrap(c, {"item"});                       // item.itemContent 꼴
    QJsonObject ic = JsonShape::pickObject(c, {"itemContent", "content"});
    if (ic.isEmpty()) ic = c;
    QJsonObject tr = JsonShape::pickObject(ic, {"tweet_results", "tweetResults", "tweet_result"});
    tr = JsonShape::unwrap(tr, {"result"});
    // TweetWithVisibilityResults 껍데기 — 예전부터 있던 변종
    tr = JsonShape::unwrap(tr, {"tweet"});
    return tr;
}

// entry 하나에서 사용자 알맹이 — user_results / result.
QJsonObject xUserResult(const QJsonObject &entryOrItem)
{
    QJsonObject c = JsonShape::pickObject(entryOrItem, {"content", "item"});
    if (c.isEmpty()) c = entryOrItem;
    c = JsonShape::unwrap(c, {"item"});
    QJsonObject ic = JsonShape::pickObject(c, {"itemContent", "content"});
    if (ic.isEmpty()) ic = c;
    QJsonObject ur = JsonShape::pickObject(ic, {"user_results", "userResults", "user_result"});
    return JsonShape::unwrap(ur, {"result"});
}

// 트윗/사용자의 옛 필드 묶음 — X 가 2023년부터 조금씩 밖으로 꺼내고 있다.
//   legacy 가 사라져도 core / __typename 옆에 같은 이름들이 남는 꼴을 여러 번 봤다.
QJsonObject xLegacy(const QJsonObject &result)
{
    QJsonObject lg = JsonShape::pickObject(result, {"legacy", "tweet_legacy", "user_legacy"});
    if (!lg.isEmpty()) return lg;
    // legacy 가 없는데 알맹이가 이미 펼쳐져 있는 꼴이면 그대로 쓴다.
    if (result.contains("full_text") || result.contains("screen_name")
        || result.contains("created_at") || result.contains("entities"))
        return result;
    // core.user_results.result.legacy 처럼 한 겹 안쪽에 있는 꼴
    const QJsonObject core = result.value("core").toObject();
    if (!core.isEmpty()) {
        QJsonObject u = JsonShape::unwrap(JsonShape::pickObject(core, {"user_results", "user_result"}), {"result"});
        QJsonObject ul = JsonShape::pickObject(u, {"legacy"});
        if (!ul.isEmpty()) return ul;
    }
    return QJsonObject();
}

// 사용자 알맹이의 legacy — 2026-08 새 모양도 채워서 준다.
//   X 는 2026-08 초 user 객체에서 legacy 를 통째로 뺐다. 필드가
//   core / profile_bio / relationship_counts / tweet_counts / action_counts /
//   banner / avatar / location / privacy / verification / website 로 흩어졌다.
//   ★ legacy 에 이미 있는 값은 그대로 둔다 — 없거나 빈 키만 새 자리에서 채운다.
//     데몬(twitter_daemon.py _normalize_user_result)과 같은 지도. 데몬이 옛 판일 때의 그물.
QJsonObject xUserLegacy(const QJsonObject &user)
{
    QJsonObject lg = xLegacy(user);
    auto sub = [&user](const QString &k) { return user.value(k).toObject(); };
    auto fill = [&lg](const QString &key, const QJsonValue &v) {
        if (v.isUndefined() || v.isNull()) return;
        if (v.isString() && v.toString().isEmpty()) return;
        const QJsonValue cur = lg.value(key);
        if (cur.isUndefined() || cur.isNull() || (cur.isString() && cur.toString().isEmpty()))
            lg.insert(key, v);
    };
    const QJsonObject core = sub("core");
    fill("name", core.value("name"));
    fill("screen_name", core.value("screen_name"));
    fill("created_at", core.value("created_at"));
    const QJsonObject bio = sub("profile_bio");
    fill("description", bio.value("description"));
    fill("entities", bio.value("entities"));
    const QJsonValue loc = user.value("location");
    fill("location", loc.isObject() ? loc.toObject().value("location") : loc);
    fill("url", sub("website").value("url"));
    fill("protected", sub("privacy").value("protected"));
    fill("verified", sub("verification").value("verified"));
    const QJsonObject rc = sub("relationship_counts");
    fill("followers_count", rc.value("followers"));
    fill("friends_count", rc.value("following"));
    const QJsonObject tc = sub("tweet_counts");
    fill("statuses_count", tc.value("tweets"));
    fill("media_count", tc.value("media_tweets"));
    fill("favourites_count", sub("action_counts").value("favorites_count"));
    fill("profile_banner_url", sub("banner").value("image_url"));
    const QJsonValue av = user.value("avatar");
    fill("profile_image_url_https", av.isObject() ? av.toObject().value("image_url") : av);
    return lg;
}

} // namespace

const QString TwitterCollector::GRAPHQL_BASE = "https://x.com/i/api/graphql";
// ★ 아래 3개는 '코드에 박힌 기본값'일 뿐이다. 실제 사용 값은 apiUrl() 이 런타임에 고른다.
//   X 는 프런트엔드 배포마다 GraphQL query ID(22자 해시)를 회전시킨다 — 바뀌면 404 가 나고
//   예전엔 앱을 다시 빌드해야만 고칠 수 있었다. 이제 api_overrides.json 으로 교체 가능하다.
const QString TwitterCollector::SEARCH_TIMELINE_URL = "https://x.com/i/api/graphql/flaR-PUMshxFWZWPNpq4zA/SearchTimeline";
const QString TwitterCollector::USER_BY_SCREEN_NAME_URL = "https://x.com/i/api/graphql/NimuplG1OB7Fd2btCLdBOw/UserByScreenName";
const QString TwitterCollector::USER_TWEETS_URL = "https://x.com/i/api/graphql/QWF3SzpHmykQHsQMixG0cg/UserTweets";

// 런타임 조회 — api_overrides.json 에 값이 있으면 그것, 없으면 기본값.
//   키 이름은 사람이 알아보기 쉽게: twitter.searchTimeline / twitter.userByScreenName / ...
QString TwitterCollector::apiUrl(const QString &key, const QString &builtin)
{
    return Common::apiOverride("twitter." + key, builtin);
}

// ── 작성자 이름 (screen_name / name) ─────────────────────────────────────────
// X 는 2025년에 user 객체를 개편했다. 예전에는 전부 legacy 안에 있었다:
//     result = { rest_id, legacy: { screen_name, name, ... } }
// 지금은 core 로 옮겨 갔다:
//     result = { rest_id, avatar, core: { screen_name, name, ... }, legacy: { 통계만 } }
//
// ★ 이걸 놓치면 티가 안 난다. 없는 키를 읽으면 예외가 아니라 빈 문자열이 나오고,
//   빈 문자열은 아무 데도 신고되지 않은 채 그대로 결과물이 된다.
//   이 기계에 쌓인 18567개 파일에서 실측한 피해:
//     · media 폴더 34개 전부에 작성자 하위폴더가 없다 (authorUsername 이 빈 값이라 안 만들어졌다)
//     · 사진 표본 300장 중 EXIF Artist·Copyright 가 있는 것 0장
//       (Common::addExifMetadata 는 값이 비면 그 태그를 아예 안 쓴다)
//     · 엑셀의 tweet_url 이 "https://x.com//status/2073974981504237891" — 슬래시가 겹쳤다
//     · 캡쳐 HTML 에 "author:i" (i 는 이름을 못 찾았을 때의 대체값)
//
// 그래서 읽는 자리를 여기 하나로 모은다. X 가 또 옮기면 이 함수만 고치면 된다.
// 순서: 옛 자리 → 새 자리 → 최상위 → 한 겹 더 들어간 판.
QString TwitterCollector::screenNameOf(const QJsonObject &userResult)
{
    QString sn = xLegacy(userResult)["screen_name"].toString();
    if (!sn.isEmpty()) return sn;
    sn = userResult["core"].toObject()["screen_name"].toString();
    if (!sn.isEmpty()) return sn;
    sn = userResult["screen_name"].toString();
    if (!sn.isEmpty()) return sn;
    // 응답에 따라 user_results 가 한 겹 더 감싸여 오는 경우가 있다.
    const QJsonObject inner = userResult["core"].toObject()["user_results"]
                                  .toObject()["result"].toObject();
    if (!inner.isEmpty()) {
        sn = xLegacy(inner)["screen_name"].toString();
        if (!sn.isEmpty()) return sn;
        sn = inner["core"].toObject()["screen_name"].toString();
        if (!sn.isEmpty()) return sn;
    }
    return QString();
}

// 표시 이름(@아이디가 아닌 사람 이름). 옮겨 간 자리가 screen_name 과 같다.
QString TwitterCollector::displayNameOf(const QJsonObject &userResult)
{
    QString n = xLegacy(userResult)["name"].toString();
    if (!n.isEmpty()) return n;
    n = userResult["core"].toObject()["name"].toString();
    if (!n.isEmpty()) return n;
    n = userResult["name"].toString();
    if (!n.isEmpty()) return n;
    const QJsonObject inner = userResult["core"].toObject()["user_results"]
                                  .toObject()["result"].toObject();
    if (!inner.isEmpty()) {
        n = xLegacy(inner)["name"].toString();
        if (!n.isEmpty()) return n;
        n = inner["core"].toObject()["name"].toString();
        if (!n.isEmpty()) return n;
    }
    return QString();
}

TwitterCollector::TwitterCollector(HanishikiBackend *backend, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    // ★ 부모(this)를 주지 않는다. collector 는 메인 스레드 소속인데(HanishikiBackend::
    //   runTwitterCollection 이 메인에서 만든다) m_http 는 collect()/checkNewPosts() 가 도는
    //   워커 스레드에서 다시 만들어진다(adoptHttpToCurrentThread). 그때 부모를 주면 Qt 가
    //     QObject: Cannot create children for a parent that is in a different thread.
    //   라며 거절하고, 부모가 안 붙은 채로 남아 정리도 안 된다(윈도우 쪽에서 실측한 경고).
    //   수명은 adoptHttpToCurrentThread() 와 소멸자가 직접 책임진다.
    , m_http(new HttpClient())
{
    m_http->setTimeout(30000);
}

// ★ QNetworkAccessManager 는 자기를 만든 스레드에서만 써야 한다(Qt 규칙).
//
//   이 객체는 메인 스레드에서 만들어지지만 collect() 는 수집 워커에서, checkNewPosts() 는
//   또 다른 워커('새 트윗 확인' 의 QThread::create)에서 돈다. 앞서 만든 HttpClient 를
//   그대로 쓰면 그 안의 NAM 은 엉뚱한 스레드, 혹은 이미 사라진 스레드(nam->thread() == 0)에
//   묶여 있다. 소켓 알림이 죽은 이벤트 디스패처에 걸리면 응답이 영영 안 오고 30초
//   타임아웃으로만 드러난다 — '새 트윗 확인이 느리다' 로 보일 자리다.
//
//   그래서 진입할 때마다 지금 도는 스레드 것으로 새로 만든다.
//   ※ 맥에서는 이것이 계정별 프록시도 맞춘다. HttpClient 생성자의 applyGlobalProxy() 가
//     '이 스레드' 의 프록시(Common::setThreadProxy)를 읽기 때문이다. 메인에서 만든 것은
//     전역 설정만 보므로, 이 줄이 없으면 미디어 다운로드만 계정 프록시를 벗어나
//     '한 세션 두 IP' 가 된다.
void TwitterCollector::adoptHttpToCurrentThread()
{
    delete m_http;                       // 옛것 정리 — ~HttpClient 가 다른/사라진 스레드의 NAM 도 처리한다
    m_http = new HttpClient();           // 부모 없음 — 생성자의 설명 참고
    m_http->setTimeout(30000);
}

TwitterCollector::~TwitterCollector()
{
    // multi-target: HanishikiBackend가 매 collect()마다 collector를 새로 만들므로
    // 이전 collector가 destroy될 때 daemon이 leak되지 않도록 명시적으로 정리.
    stopDaemon();
    // ★ 프로필 버퍼도 여기서 지운다. 지우는 자리가 '다음 수집 시작' 하나뿐이었는데,
    //   MiyoBackend 는 수집할 때마다 collector 를 새로 만든다 — 그래서 수집이 끝날
    //   때마다 버퍼가 하나씩 새고, 임시 파일이 디스크에 남고, 파일 핸들도 남았다.
    //   (실측: 사용자 보관함에 tw_profiles_*.jsonl 48개. 앱이 떠 있는 동안에는
    //    "다른 프로세스가 사용 중" 이라 지워지지도 않았다)
    //   DiskJsonBuffer 소멸자가 닫고 지운다 — 불러 주기만 하면 된다.
    delete m_profileBuffer;
    m_profileBuffer = nullptr;
    // ★ m_http 는 부모가 없다(위 생성자 설명) — 여기서 직접 지운다.
    //   메인에서 지우든(멤버 collector) 워커에서 지우든(병렬 모드의 지역 collector)
    //   ~HttpClient 가 NAM 을 맞는 스레드로 넘겨 정리한다.
    delete m_http;
    m_http = nullptr;
}

// 이 계정의 출구를 이 스레드에 건다 — 반드시 setupClient · startDaemon 보다 먼저(데몬은 뜨는 시점의 출구로 나간다).
//   ★ 계정이 돌 때만 걸면 안 된다. 'all' 의 다음 갈래 · 대기 뒤 재시작 · 스레드 되짚기는 0번(또는 그때의 계정)으로 돌아오는데,
//     출구는 마지막으로 돌던 계정의 것이 남아 A 의 쿠키가 B 의 IP(또는 이 컴퓨터의 회선)로 나갔다(검토).
//   계정은 프로필 이름(proxy)만 든다 — 이름 → 주소 · 비밀번호는 백엔드가 푼다(HanishikiBackend::proxyForAccount — 옛 모양의 계정도 받는다).
//   이미 그 출구면 기록을 다시 남기지 않는다(수집 시작 때 백엔드가 건 것과 같다).
void TwitterCollector::applyAccountProxy(const QJsonObject &account)
{
    const QString before = Common::proxyExitTag();
    const QJsonObject prof = m_backend ? m_backend->proxyForAccount(account) : QJsonObject();
    const QString want = account.value("proxy").toString();
    if (!prof.isEmpty()) {
        Common::setThreadProxy(true, prof["host"].toString(), prof["port"].toInt(),
                               prof["user"].toString(), prof["pass"].toString(),
                               prof["type"].toString());
        if (m_backend && Common::proxyExitTag() != before)
            m_backend->log(QString("🔒 %1 은 프로필 '%2' (%3:%4) 로 나갑니다")
                               .arg(account["name"].toString("Account"), prof["name"].toString(),
                                    prof["host"].toString()).arg(prof["port"].toInt()),
                           "info", "twitter");
    } else if (!want.isEmpty()) {
        // ★ 고른 프로필이 목록에 없다(지웠거나 이름을 바꿨다). 직접 연결로 조용히 새지 않게
        //   닫힌 길(127.0.0.1:9 — 아무것도 듣지 않는다)을 건다.
        //   이 계정의 요청은 실패하고, 까닭은 이 줄로 남는다.
        Common::setThreadProxy(true, QStringLiteral("127.0.0.1"), 9, QString(), QString());
        if (m_backend && Common::proxyExitTag() != before)
            m_backend->log(QString("⚠ %1 이 고른 프록시 '%2' 이 목록에 없습니다 — 이 계정은 나가지 않습니다. "
                                   "'프록시 (VPN)' 탭에서 다시 등록하거나 계정의 출구를 다시 고르십시오.")
                               .arg(account["name"].toString("Account"), want),
                           "error", "twitter");
    } else {
        // 이 계정엔 지정이 없다 — 앞 계정 것을 물려받지 않도록 반드시 지운다.
        Common::clearThreadProxy();
    }
}

void TwitterCollector::setupClientFor(const QJsonObject &account)
{
    const QString before = Common::proxyExitTag();
    applyAccountProxy(account);
    // ★ 미디어를 받는 m_http 는 만들 때의 출구에 묶인다(HttpClient 생성자) — 출구가 바뀌면 새로 만든다.
    //   안 그러면 B 계정의 API 는 B 의 출구로, 미디어는 앞 계정의 출구로 나가 한 세션이 두 IP 가 된다(검토).
    if (Common::proxyExitTag() != before) {
        const std::atomic<bool> *runFlag = m_http ? m_http->runFlag() : nullptr;
        adoptHttpToCurrentThread();
        if (m_http && runFlag) m_http->setRunFlag(runFlag);
        // ★ 데몬은 뜰 때의 출구로 나간다 — 출구가 바뀌었으면 켜 둔 데몬을 끈다(다음 startDaemon 이 새 출구로 띄운다).
        //   r56 부터 '전체' 의 하위 판 사이에 데몬을 켜 두므로, 토큰이 같고 출구만 다르면 옛 출구로 나갈 수 있었다.
        if (m_daemon && m_daemon->state() == QProcess::Running) stopDaemon();
    }
    m_expectedUid = account["uid"].toString();
    m_expectedUidTok = account["uid_tok"].toString();
    setupClient(account["auth_token"].toString(), account["ct0"].toString());
}

void TwitterCollector::setupClient(const QString &authToken, const QString &ct0)
{
    // ★ 토큰이 바뀌면 기존 데몬 강제 종료 — 안 그러면 multi-target × per-account 시나리오에서
    //   1번째 target(계정A)로 시작된 데몬이 2번째 target(계정B) 요청에도 재사용되어
    //   모든 target이 계정A로 수집되는 버그가 발생한다.
    const bool tokenChanged = (m_authToken != authToken) || (m_ct0 != ct0);
    if (tokenChanged && m_daemon && m_daemon->state() == QProcess::Running) {
        stopDaemon();
    }
    m_authToken = authToken;
    m_ct0 = ct0;
    m_csrfToken = ct0;
}

// ── Persistent Python daemon (twikit) ──

bool TwitterCollector::startDaemon()
{
    if (m_daemon && m_daemon->state() == QProcess::Running && m_daemonReady) {
        return true; // Already running
    }

    stopDaemon(); // Clean up any previous

    // Find daemon script: bundled tools dir → dev fallback
    QString scriptPath = Common::activeToolScriptPath(QStringLiteral("twitter_daemon.py"));
    if (!QFile::exists(scriptPath)) {
        scriptPath = QCoreApplication::applicationDirPath() + "/../../resources/tools/twitter_daemon.py";
    }
    if (!QFile::exists(scriptPath)) {
        m_backend->log("twitter_daemon.py not found", "error", "twitter");
        return false;
    }

    QJsonObject initArgs;
    initArgs["auth_token"] = m_authToken;
    initArgs["ct0"] = m_ct0;
    // ★ 쿠키 자동 갱신이 '같은 X 계정' 인지 가를 번호 — 그 번호를 적을 때의 토큰이 지금 토큰일 때만 넘긴다
    //   (토큰을 손으로 바꿨는데 옛 번호가 남아 있으면 옛 계정의 로그인을 받게 된다).
    if (!m_expectedUid.isEmpty() && !m_authToken.isEmpty()
        && m_expectedUidTok == QString::fromLatin1(
               QCryptographicHash::hash(m_authToken.toUtf8(), QCryptographicHash::Sha256).toHex().left(16)))
        initArgs["expected_uid"] = m_expectedUid;
    // ★ 프록시(VPN) 출구를 데몬에 직접 준다(윈도우와 같다). 데몬의 세션 계층(x_session)은
    //   전송기를 직접 넣어 httpx 가 환경 변수의 프록시를 보지 않는다 — 환경만 믿으면
    //   계정에 프록시를 걸어도 트위터 API 요청이 이 컴퓨터의 회선으로 나갔다.
    //   값은 지금 이 스레드에 걸린 출구다(계정을 바꿀 때 setThreadProxy 뒤에 데몬을 다시 띄운다).
    //   HTTPS 프로필이면 중계기 주소, 중계기를 못 띄웠으면 닫힌 길로 막는다.
    //   자격증명이 든 주소일 수 있으나 아래처럼 stdin 첫 줄로만 가므로 ps 에 남지 않는다.
    if (Common::proxyEnabled()) {
        QString proxy = Common::proxyUrl();
        if (proxy.isEmpty()) proxy = QStringLiteral("socks5://127.0.0.1:9");   // 닫힌 길
        initArgs["proxy"] = proxy;
    }
    // ★ 브라우저 흉내는 앱의 다른 요청 · 캡처 Chrome 과 같은 것으로(설치된 Chrome 의 판). 데몬의 고정값은
    //   '윈도우 Chrome 153' 이라, 같은 계정의 쿠키가 두 브라우저에서 오는 것처럼 보였다.
    initArgs["user_agent"] = Common::browserUserAgent();
    {
        QJsonObject hints;
        const auto ch = Common::browserClientHints();
        for (const char *k : {"sec-ch-ua", "sec-ch-ua-mobile", "sec-ch-ua-platform"})
            if (ch.contains(QLatin1String(k))) hints[QLatin1String(k)] = ch.value(QLatin1String(k));
        initArgs["client_hints"] = hints;
    }
    // ★ 자격증명을 명령줄로 넘기지 않는다.
    //   macOS 에서 프로세스 명령줄은 ps 로 같은 기계의 아무 프로세스나 읽을 수 있다.
    //   예전에는 여기 JSON 에 auth_token·ct0 세션 자격증명 이 그대로 들어가 ps 출력에 찍혔다(실제로 확인했다).
    //   데몬은 어차피 stdin 으로 명령을 주고받으므로, 첫 줄로 넘긴다.
    QString argsJson = QString::fromUtf8(QJsonDocument(initArgs).toJson(QJsonDocument::Compact));

    {
        QMutexLocker lock(&m_daemonMutex);   // 없애는 쪽과 같은 자물쇠
        m_daemon = new QProcess();
    }  // 부모 없음 — 워커 스레드에서 생성되므로 this(메인스레드) 지정하면 크래시
    m_daemon->setProcessEnvironment(Common::bundledProcessEnv());

    // 번들 Python 우선 → system fallback
    QStringList pythons = Common::pythonCandidates();

    bool started = false;
    for (const auto &python : pythons) {
        m_daemon->start(python, {scriptPath, QStringLiteral("--stdin-args")});
        if (m_daemon->waitForStarted(5000)) {
            // 첫 줄 = 초기화 인자. 이후 줄은 평소대로 명령 통로.
            m_daemon->write(argsJson.toUtf8() + "\n");
            m_daemon->waitForBytesWritten(3000);
            m_daemonPid = m_daemon->processId();   // 이 스레드에서만 읽는다
            started = true;
            m_backend->log("Daemon: using "+ python, "info", "twitter");
            break;
        }
    }

    if (!started) {
        m_backend->log("Daemon: Python not found", "error", "twitter");
        delete m_daemon;
        m_daemon = nullptr;
        return false;
    }

    // Wait for "ready"signal (up to 60 seconds — may include twikit upgrade)
    if (!m_daemon->waitForReadyRead(60000)) {
        m_backend->log("Daemon: timeout waiting for ready", "error", "twitter");
        stopDaemon();
        return false;
    }

    // 데몬이 info 메시지를 먼저 보낼 수 있음 → "status":"ready"올 때까지 읽기
    for (int attempt = 0; attempt < 40; ++attempt) {
        QByteArray line = m_daemon->readLine().trimmed();
        if (line.isEmpty()) {
            if (!m_daemon->waitForReadyRead(10000)) break;
            continue;
        }
        QJsonObject msg = QJsonDocument::fromJson(line).object();
        if (msg["status"].toString() == "ready") {
            m_daemonReady = true;
            bool tidOk = msg["tid"].toBool(false);
            m_backend->log(QString("Daemon: ready (%1)").arg(tidOk ? "TID active": "cached hashes only"), "success", "twitter");
            // ★ X 서명을 만들지 못했으면 서명 부품부터 최신으로 맞춰 보고 데몬을 다시 띄운다(12시간에 한 번 —
            //   다시 띄운 데몬이 또 실패해도 되풀이하지 않는다). X 가 계산법을 바꾸면 이것 없이는 수집이 401/404 로 멈춘다.
            if (!tidOk && SelfRepair::xSigningRepairDue() && (!m_runFlag || m_runFlag->load())) {   // 중지된 판이면 하지 않는다
                m_backend->log("X 서명을 만들지 못했습니다 — 서명 부품(xclienttransaction)을 최신으로 맞춰 봅니다...", "warning", "twitter");
                QString why;
                const bool healed = SelfRepair::repairXSigningIfDue(Common::bundledPythonPath(), &why);
                if (!why.isEmpty()) m_backend->log("  " + why, healed ? "success" : "info", "twitter");
                if (healed && (!m_runFlag || m_runFlag->load())) {   // 고치는 사이 중지됐으면 다시 띄우지 않는다(다음 판이 새 부품을 쓴다)
                    stopDaemon();
                    return startDaemon();
                }
            }

            // 시작 시 해시 자동 갱신 — 404 방지
            QJsonObject updateCmd;
            updateCmd["action"] = "update_endpoints";
            QJsonObject updateResp = sendDaemonCommand(updateCmd, 30000);
            if (updateResp["status"].toString() == "ok") {
                // ★ 12시간 안에 받은 해시면 데몬이 긁지 않고 캐시로 답한다(r56) — 해시가 정말 바뀌면 404 수리 길이 force 로 다시 받는다
                if (updateResp.value(QStringLiteral("cached")).toBool())
                    m_backend->log("GraphQL 해시: 12시간 안에 받은 것을 씁니다", "info", "twitter");
                else
                    m_backend->log("GraphQL 해시 최신 상태로 갱신 완료", "success", "twitter");
            }

            return true;
        }
        if (msg.contains("error") && !msg.contains("info")) {
            m_backend->log("Daemon init failed: "+ msg["error"].toString(), "error", "twitter");
            stopDaemon();
            return false;
        }
        // info 메시지는 로그만 하고 계속 읽기
        if (msg.contains("info")) {
            m_backend->log("Daemon: "+ msg["info"].toString(), "info", "twitter");
        }
    }

    m_backend->log("Daemon: ready signal not received", "error", "twitter");
    stopDaemon();
    return false;
}

void TwitterCollector::stopDaemon()
{
    m_daemonReady = false;

    // ★ 이 함수에는 두 스레드가 동시에 들어온다.
    //   실측 로그(2026-09-11 23:19:14.158 과 .187)에서 같은 pid 로 0.03초 안에 두 번
    //   진입했다 — 하나는 수집 워커가 끝나면서, 하나는 메인 스레드가 collector 를
    //   지우면서 불렀다. 둘 다 같은 포인터를 들고 정리로 달려가면 이중 해제가 된다.
    //   실측: Qt6Core.dll 접근 위반 0xc0000005.
    //   그래서 잠그고 포인터를 '가져가면서' 비운다 — 두 번째로 들어온 쪽은 빈손이 된다.
    QProcess *daemon = nullptr;
    qint64 pid = 0;
    {
        QMutexLocker lock(&m_daemonMutex);
        daemon = m_daemon;
        pid = m_daemonPid;
        m_daemon = nullptr;
        m_daemonPid = 0;
    }
    if (!daemon) return;

    // ★ QProcess 는 '만든 스레드' 에서만 만져야 한다.
    //   이 데몬은 수집 워커 스레드에서 new/start 된다. 그런데 stopDaemon() 은 다른
    //   스레드에서도 불린다 — 다음 수집의 워커, 소멸자, 메인 스레드의 중지 버튼.
    //   그대로 write/waitForFinished/kill/delete 를 하면 Qt 가 이렇게 경고하고 죽는다:
    //     QWinEventNotifier: Event notifiers cannot be enabled or disabled from another thread
    //   실측: 内閣会 폴링 도중 중지 → 수집 시작 이면 매번 재현됐다(0xc000000d).
    //   HttpClient 의 QNetworkAccessManager 와 같은 종류의 문제다.
    QThread *const owner = daemon->thread();
    QThread *const here = QThread::currentThread();
    qDebug() << "[Twitter] stopDaemon 진입 — 만든 스레드:" << (owner ? owner->objectName() : QString("(사라짐)"))
             << "지금 스레드:" << here->objectName() << "pid:" << pid
             << "만든 스레드가 도는 중:" << (owner ? owner->isRunning() : false);

    if (owner && owner != here) {
        // 만든 스레드가 아직 살아 있다 — 객체는 건드리지 않는다.
        //   프로세스는 pid 로 끊고(그래야 상대가 붙들고 있던 대기가 풀린다),
        //   객체 정리는 제 스레드에 맡긴다.
        Common::killProcessByPid(pid);
        qDebug() << "[Twitter] stopDaemon: pid 로 끊음 — 이제 제 스레드에 정리를 맡긴다";
        QMetaObject::invokeMethod(daemon, &QObject::deleteLater);
        qDebug() << "[Twitter] stopDaemon: deleteLater 예약 완료";
        return;
    }

    // 만든 스레드가 이미 사라졌다면 thread() 가 0 이다. 그때는 지금 스레드로 데려올 수
    // 있다 — Qt 가 명시적으로 허용하는 유일한 경우(스레드 없는 객체 → 현재 스레드).
    if (!owner) {
        qDebug() << "[Twitter] stopDaemon: 만든 스레드가 사라져 지금 스레드로 데려온다";
        daemon->moveToThread(here);
    }

    if (daemon->state() == QProcess::Running) {
        daemon->write("{\"action\":\"quit\"}\n");
        daemon->waitForFinished(3000);
        if (daemon->state() == QProcess::Running) {
            daemon->kill();
            daemon->waitForFinished(2000);
        }
    }
    delete daemon;
    qDebug() << "[Twitter] stopDaemon: 같은 스레드에서 정리 완료";
}

QJsonObject TwitterCollector::sendDaemonCommand(const QJsonObject &cmd, int timeoutMs)
{
    // Periodic daemon restart every 2000 requests to prevent Python memory bloat
    if (m_daemon && m_daemonReady && m_daemonRequestCount >= 2000) {
        m_backend->log("Daemon: periodic restart (memory hygiene)", "info", "twitter");
        stopDaemon();
        m_daemonRequestCount = 0;
    }

    if (!m_daemon || m_daemon->state() != QProcess::Running || !m_daemonReady) {
        // Try to restart daemon — with network recovery wait (sleep/wake 복구)
        m_daemonRequestCount = 0;
        for (int retry = 0; retry < 5; ++retry) {
            if (startDaemon()) break;
            // 네트워크 미복구 가능성 — 대기 후 재시도
            m_backend->log(QString("데몬 재시작 실패, %1초 후 재시도 (%2/5)...").arg(10 * (retry + 1)).arg(retry + 1), "warning", "twitter");
            QThread::sleep(10 * (retry + 1));
        }
        if (!m_daemon || m_daemon->state() != QProcess::Running || !m_daemonReady) {
            return QJsonObject{{"error", "Daemon not available after retries"}};
        }
    }
    m_daemonRequestCount++;

    // ★ 명령마다 번호(_rid)를 붙이고, 데몬이 돌려준 번호가 다른 답은 버린다(r54). 키체인 창 · 해시 수리처럼 이 기다림보다
    //   오래 걸린 명령의 답이 늦게 오면, 예전엔 그 줄이 '다음 명령의 답' 으로 읽혔다(엉뚱한 401, 또는 앞 명령의 200 본문).
    //   번호 없이 온 답(번호를 모르는 옛 데몬 · 고침 꾸러미의 시차)은 예전처럼 받는다.
    const QString rid = QString::number(++m_daemonSeq);
    QJsonObject cmdOut = cmd;
    cmdOut["_rid"] = rid;
    QByteArray cmdLine = QJsonDocument(cmdOut).toJson(QJsonDocument::Compact) + "\n";
    m_daemon->write(cmdLine);

    // Read complete JSON line — response can be very large (hundreds of KB)
    // Must accumulate chunks until we see a newline
    QByteArray buffer;
    QElapsedTimer timer;
    timer.start();

    while (timer.elapsed() < timeoutMs) {
        int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) break;

        if (!m_daemon->waitForReadyRead(qMin(remaining, 5000))) {
            if (m_daemon->state() != QProcess::Running) {
                m_backend->log("Daemon: process died", "error", "twitter");
                stopDaemon();
                return QJsonObject{{"error", "Daemon process died"}};
            }
            continue; // Timeout on this chunk, try again
        }

        buffer += m_daemon->readAll();

        // Check if we have a complete line (JSON ends with newline)
        if (buffer.contains('\n')) break;
    }

    if (!buffer.contains('\n')) {
        m_backend->log(QString("Daemon: incomplete response (%1 bytes, %2ms)")
                           .arg(buffer.size()).arg(timer.elapsed()), "error", "twitter");
        stopDaemon();
        return QJsonObject{{"error", "Daemon incomplete response"}};
    }

    // Process all complete lines — daemon may emit info/log lines before the actual response
    // (e.g., auto-repair messages). The actual response is the last non-info line.
    // CRITICAL: after processing info lines, must keep reading for actual response.
    QJsonObject result;
    bool gotResult = false;

    while (true) {
        // Process all complete lines in buffer
        while (buffer.contains('\n')) {
            int newlineIdx = buffer.indexOf('\n');
            QByteArray line = buffer.left(newlineIdx).trimmed();
            buffer = buffer.mid(newlineIdx + 1);

            if (line.isEmpty()) continue;

            QJsonParseError parseError;
            QJsonDocument doc = QJsonDocument::fromJson(line, &parseError);
            if (!doc.isObject()) {
                m_backend->log(QString("Daemon: JSON parse error: %1").arg(parseError.errorString()), "warning", "twitter");
                continue;
            }

            QJsonObject obj = doc.object();

            // Info messages from daemon (auto-repair logs) — display and continue
            if (obj.contains("info")) {
                m_backend->log("Daemon: "+ obj["info"].toString(), "info", "twitter");
                continue;
            }

            // 앞 명령의 늦은 답 — 버리고 이 명령의 답을 계속 기다린다
            if (obj.contains("_rid") && obj.value("_rid").toString() != rid) {
                m_backend->log(QString("Daemon: 앞 명령(#%1)의 늦은 답을 버렸습니다").arg(obj.value("_rid").toString()),
                               "info", "twitter");
                continue;
            }
            obj.remove("_rid");

            // Actual response (has "status", "body", "error", etc.)
            result = obj;
            gotResult = true;
        }

        // If we got a real result, we're done
        if (gotResult) break;

        // No result yet (only info lines so far) — read more data from daemon
        if (timer.elapsed() >= timeoutMs) break;
        int remaining = timeoutMs - static_cast<int>(timer.elapsed());
        if (remaining <= 0) break;

        if (!m_daemon->waitForReadyRead(qMin(remaining, 5000))) {
            if (m_daemon->state() != QProcess::Running) {
                m_backend->log("Daemon: process died while waiting for response", "error", "twitter");
                stopDaemon();
                return QJsonObject{{"error", "Daemon process died"}};
            }
            continue; // Keep waiting
        }
        buffer += m_daemon->readAll();
    }

    if (!gotResult) {
        return QJsonObject{{"error", "No valid response from daemon"}};
    }

    m_lastApiAction = cmd.value(QStringLiteral("action")).toString();
    // ★ 레이트 리밋 머리(데몬이 _rl 로 싣는다 — r56). 429 뒤 고정 15분 대신 X 가 알려 준 풀림 시각까지만 기다리고,
    //   남은 요청이 0 이면 429 를 받기 전에 쉬는 계정으로 돌린다(handleRateLimit · rotateBeforeLimit).
    if (result.contains(QStringLiteral("_rl"))) {
        const QJsonObject rl = result.value(QStringLiteral("_rl")).toObject();
        m_rlRemaining = rl.contains(QStringLiteral("remaining")) ? rl.value(QStringLiteral("remaining")).toInt(-1) : -1;
        m_rlReset = rl.value(QStringLiteral("reset")).toVariant().toLongLong();
        m_rlAction = cmd.value(QStringLiteral("action")).toString();
    }

    return result;
}

QMap<QString, QString> TwitterCollector::getHeaders() const
{
    QMap<QString, QString> headers;
    // Match twikit 2.3.3 _base_headers exactly
    // ★ Bearer 토큰도 X 가 바꿀 수 있다 → 오버라이드 가능(기본값은 twikit 2.3.3 와 동일)
    headers["Authorization"] = "Bearer " + apiUrl("bearer",
        "AAAAAAAAAAAAAAAAAAAAANRILgAAAAAAnNwIzUejRCOuH5E6I8xnZz4puTs%3D1Zv7ttfk8LF81IUq16cHjhLTvJu4FA33AGWWjCpTnA");
    headers["Content-Type"] = "application/json";
    headers["X-Twitter-Auth-Type"] = "OAuth2Session";
    headers["X-Twitter-Active-User"] = "yes";
    headers["Referer"] = "https://x.com/";
    headers["User-Agent"] = Common::browserUserAgent();
    headers["Accept-Language"] = "ja";
    headers["X-Twitter-Client-Language"] = "ja";
    // Auth cookies & CSRF
    headers["Cookie"] = QString("auth_token=%1; ct0=%2").arg(m_authToken, m_ct0);
    headers["X-Csrf-Token"] = m_csrfToken;
    return headers;
}

bool TwitterCollector::initTransactionIds()
{
    m_tidInitialized = false;
    m_transactionIds.clear();

    // Find the Python helper script
    QString scriptPath = Common::activeToolScriptPath(QStringLiteral("twitter_tid.py"));
    if (!QFile::exists(scriptPath)) {
        scriptPath = QCoreApplication::applicationDirPath() + "/../../resources/tools/twitter_tid.py";
    }
    if (!QFile::exists(scriptPath)) {
        m_backend->log("twitter_tid.py not found, skipping TID", "warning", "twitter");
        return false;
    }

    // All GraphQL API paths that need TIDs
    //   ★ 각 경로도 api_overrides.json 으로 교체 가능(X 가 query ID 를 바꿔도 재빌드 불필요)
    QJsonArray paths;
    paths.append(apiUrl("userByScreenName", "/i/api/graphql/NimuplG1OB7Fd2btCLdBOw/UserByScreenName"));
    paths.append(apiUrl("userTweets",       "/i/api/graphql/QWF3SzpHmykQHsQMixG0cg/UserTweets"));
    paths.append(apiUrl("searchTimeline",   "/i/api/graphql/flaR-PUMshxFWZWPNpq4zA/SearchTimeline"));
    paths.append(apiUrl("likes",            "/i/api/graphql/IohM3gxQHfvWePH5E3KuNA/Likes"));
    paths.append(apiUrl("bookmarks",        "/i/api/graphql/qToeLeMs43Q8cr7tRYXmaQ/Bookmarks"));
    paths.append(apiUrl("followers",        "/i/api/graphql/gC_lyAxZOptAMLCJX5UhWw/Followers"));
    paths.append(apiUrl("following",        "/i/api/graphql/2vUj-_Ek-UmBVDNtd8OnQA/Following"));
    paths.append(apiUrl("tweetDetail",      "/i/api/graphql/nBS-WpgA6ZG0CyNHD517JQ/TweetDetail"));

    QJsonObject args;
    args["auth_token"] = m_authToken;
    args["ct0"] = m_ct0;
    args["paths"] = paths;

    QString argsJson = QString::fromUtf8(QJsonDocument(args).toJson(QJsonDocument::Compact));

    QProcess proc;
    proc.setProcessEnvironment(Common::bundledProcessEnv());

    QStringList pythons = Common::pythonCandidates();
    bool started = false;
    for (const auto &python : pythons) {
        // 자격증명은 stdin 으로 — 명령줄은 ps 로 남이 읽는다.
        proc.start(python, {scriptPath, QStringLiteral("--stdin-args")});
        if (proc.waitForStarted(5000)) {
            proc.write(argsJson.toUtf8());
            proc.closeWriteChannel();
            started = true;
            m_backend->log("TID: using "+ python, "info", "twitter");
            break;
        }
    }

    if (!started) {
        m_backend->log("Python not found for TID generation", "warning", "twitter");
        return false;
    }

    if (!proc.waitForFinished(30000)) {
        proc.kill();
        m_backend->log("TID generation timed out", "warning", "twitter");
        return false;
    }

    QByteArray output = proc.readAllStandardOutput();
    QByteArray errors = proc.readAllStandardError();

    if (proc.exitCode() != 0) {
        QString errMsg = QString::fromUtf8(errors).trimmed();
        QString outMsg = QString::fromUtf8(output).trimmed();
        m_backend->log("TID failed (exit "+ QString::number(proc.exitCode()) + "): "+ (errMsg.isEmpty() ? outMsg : errMsg).left(300), "warning", "twitter");
        return false;
    }

    QJsonDocument doc = QJsonDocument::fromJson(output);
    if (!doc.isObject()) {
        m_backend->log("TID: invalid JSON: "+ QString::fromUtf8(output).left(200), "warning", "twitter");
        return false;
    }

    QJsonObject result = doc.object();
    if (result.contains("error")) {
        m_backend->log("TID error: "+ result["error"].toString(), "warning", "twitter");
        return false;
    }

    for (auto it = result.begin(); it != result.end(); ++it) {
        m_transactionIds[it.key()] = it.value().toString();
    }

    m_tidInitialized = true;
    m_backend->log(QString("Transaction IDs generated: %1 endpoints").arg(m_transactionIds.size()), "success", "twitter");
    return true;
}

QString TwitterCollector::getTransactionId(const QString &urlPath)
{
    // Extract path from full URL if needed
    QString path = urlPath;
    if (path.startsWith("https://")) {
        QUrl url(path);
        path = url.path();
    }
    return m_transactionIds.value(path);
}

QMap<QString, QString> TwitterCollector::getHeadersWithTid(const QString &urlPath)
{
    auto headers = getHeaders();
    if (!m_tidInitialized) {
        initTransactionIds();
    }
    QString tid = getTransactionId(urlPath);
    if (!tid.isEmpty()) {
        headers["X-Client-Transaction-Id"] = tid;
    }
    return headers;
}

QJsonObject TwitterCollector::callTwikitApi(const QJsonObject &args)
{
    // Find the Python API proxy script
    QString scriptPath = Common::activeToolScriptPath(QStringLiteral("twitter_api.py"));
    if (!QFile::exists(scriptPath)) {
        scriptPath = QCoreApplication::applicationDirPath() + "/../../resources/tools/twitter_api.py";
    }
    if (!QFile::exists(scriptPath)) {
        return QJsonObject{{"error", "twitter_api.py not found"}};
    }

    // Build args JSON with auth credentials
    QJsonObject fullArgs = args;
    fullArgs["auth_token"] = m_authToken;
    fullArgs["ct0"] = m_ct0;

    QString argsJson = QString::fromUtf8(QJsonDocument(fullArgs).toJson(QJsonDocument::Compact));

    QProcess proc;
    proc.setProcessEnvironment(Common::bundledProcessEnv());

    QStringList pyList = Common::pythonCandidates();
    bool pyStarted = false;
    for (const auto &py : pyList) {
        // 자격증명은 stdin 으로 — 명령줄은 ps 로 남이 읽는다.
        proc.start(py, {scriptPath, QStringLiteral("--stdin-args")});
        if (proc.waitForStarted(5000)) {
            proc.write(argsJson.toUtf8());
            proc.closeWriteChannel();
            pyStarted = true; break;
        }
    }
    if (!pyStarted) {
        return QJsonObject{{"error", "Python not found"}};
    }

    if (!proc.waitForFinished(60000)) {
        proc.kill();
        return QJsonObject{{"error", "API call timed out"}};
    }

    QByteArray output = proc.readAllStandardOutput();
    QByteArray errors = proc.readAllStandardError();

    if (proc.exitCode() != 0) {
        QString errMsg = QString::fromUtf8(errors).trimmed();
        QString outMsg = QString::fromUtf8(output).trimmed();
        return QJsonObject{{"error", (errMsg.isEmpty() ? outMsg : errMsg).left(500)}};
    }

    QJsonDocument doc = QJsonDocument::fromJson(output);
    if (!doc.isObject()) {
        return QJsonObject{{"error", "Invalid JSON from API proxy: "+ QString::fromUtf8(output).left(200)}};
    }

    return doc.object();
}

// Matches twikit 2.3.3 FEATURES exactly
// ── 좋아요 · 북마크 · 리포스트의 '다 받은 곳에서 일찍 멈추기'. 이 목록들은 누른 순서라 ID 경계로는 못 자르고,
//   이번 판을 시작하기 전 엑셀(1열 = id)에 있던 것이 이어지는지만 센다. n = 0 이면 세기만 하고 멈추지 않는다.
// 범위를 벗어날 때 한 번 — 중간에 돌아가는 길(중지 · 오류)에서도 표시를 되돌리려고
struct ScopeExit {
    std::function<void()> f;
    ~ScopeExit() { if (f) f(); }
};
struct KnownRun {
    QSet<QString> known;
    int n = 0, streak = 0, stopped = 0, fresh = 0;
    bool see(const QString &id) {   // 받은 것 하나 — true 면 여기서 멈춘다
        if (known.contains(id)) {
            if (n > 0 && ++streak >= n) { stopped = streak; return true; }
        } else {
            streak = 0;
            ++fresh;
        }
        return false;
    }
};
static QSet<QString> excelIdsAt(const QString &path)
{
    QSet<QString> ids;
    if (!QFile::exists(path)) return ids;
    QXlsx::Document doc(path);
    const int lastRow = doc.dimension().lastRow();
    for (int r = 2; r <= lastRow; ++r) {   // 1행은 머리
        const QString id = doc.read(r, 1).toString().trimmed();
        if (!id.isEmpty()) ids.insert(id);
    }
    return ids;
}

static QJsonObject defaultFeatures()
{
    QJsonObject f;
    f["creator_subscriptions_tweet_preview_api_enabled"] = true;
    f["c9s_tweet_anatomy_moderator_badge_enabled"] = true;
    f["tweetypie_unmention_optimization_enabled"] = true;
    f["responsive_web_edit_tweet_api_enabled"] = true;
    f["graphql_is_translatable_rweb_tweet_is_translatable_enabled"] = true;
    f["view_counts_everywhere_api_enabled"] = true;
    f["longform_notetweets_consumption_enabled"] = true;
    f["responsive_web_twitter_article_tweet_consumption_enabled"] = true;
    f["tweet_awards_web_tipping_enabled"] = false;
    f["longform_notetweets_rich_text_read_enabled"] = true;
    f["longform_notetweets_inline_media_enabled"] = true;
    f["rweb_video_timestamps_enabled"] = true;
    f["responsive_web_graphql_exclude_directive_enabled"] = true;
    f["verified_phone_label_enabled"] = false;
    f["freedom_of_speech_not_reach_fetch_enabled"] = true;
    f["standardized_nudges_misinfo"] = true;
    f["tweet_with_visibility_results_prefer_gql_limited_actions_policy_enabled"] = true;
    f["responsive_web_media_download_video_enabled"] = false;
    f["responsive_web_graphql_skip_user_profile_image_extensions_enabled"] = false;
    f["responsive_web_graphql_timeline_navigation_enabled"] = true;
    f["responsive_web_enhance_cards_enabled"] = false;
    return f;
}

QJsonObject TwitterCollector::getUserByScreenName(const QString &screenName)
{
    QJsonObject cmd;
    cmd["action"] = "user_by_screen_name";
    cmd["screen_name"] = screenName;

    // ★ ReadTimeout / TimeoutException 자동 재시도 (최대 3회) — Twitter 가끔 응답 늦음
    QJsonObject resp;
    for (int attempt = 1; attempt <= 3; ++attempt) {
        resp = sendDaemonCommand(cmd);
        if (!resp.contains("error")) break;
        QString errMsg = resp["error"].toString();
        bool isTimeout = errMsg.contains("ReadTimeout", Qt::CaseInsensitive)
                      || errMsg.contains("ConnectTimeout", Qt::CaseInsensitive)
                      || errMsg.contains("TimeoutException", Qt::CaseInsensitive)
                      || errMsg.contains("timeout", Qt::CaseInsensitive);
        if (isTimeout && attempt < 3) {
            int waitSec = 5 * attempt;
            m_backend->log(QString("Twitter API timeout (시도 %1/3) — %2초 대기 후 재시도").arg(attempt).arg(waitSec),
                "warning", "twitter");
            QThread::sleep(waitSec);
            continue;
        }
        break;  // timeout 아닌 에러 또는 마지막 시도 → log + return
    }
    if (resp.contains("error")) {
        QString errMsg = resp["error"].toString();
        if (errMsg.isEmpty()) errMsg = "(empty error)";
        m_backend->log("getUserByScreenName error: "+ errMsg, "error", "twitter");
        if (resp.contains("traceback"))
            m_backend->log("Traceback: "+ resp["traceback"].toString().left(500), "error", "twitter");
        return QJsonObject();
    }

    int status = resp["status"].toInt();
    if (status == 404) {
        m_backend->log("getUserByScreenName HTTP 404 → 엔드포인트 갱신 시도", "warning", "twitter");
        QJsonObject updateCmd;
        updateCmd["action"] = "update_endpoints";
        updateCmd["force"] = true;   // 캐시가 젊어도 긁는다(r56 — 404 수리 길)
        QJsonObject updateResp = sendDaemonCommand(updateCmd, 30000);
        if (updateResp["status"].toString() == "ok") {
            m_backend->log("GraphQL 해시 갱신 완료, 재시도...", "success", "twitter");
            QJsonObject retryResp = sendDaemonCommand(cmd);
            if (!retryResp.contains("error") && retryResp["status"].toInt() == 200) {
                resp = retryResp;
                status = 200;
            }
        }
    }
    if (status != 200) {
        m_backend->log(QString("getUserByScreenName HTTP %1").arg(status), "error", "twitter");
        QString body = resp["body"].toString();
        if (!body.isEmpty()) m_backend->log("Response: "+ body.left(300), "error", "twitter");
        return QJsonObject();
    }

    // Debug: log response structure
    if (resp.contains("_debug_keys")) {
        m_backend->log("API result keys: "+ resp["_debug_keys"].toString(), "info", "twitter");
        m_backend->log("API legacy keys: "+ resp["_debug_legacy_keys"].toString(), "info", "twitter");
        m_backend->log("API typename: "+ resp["_debug_typename"].toString(), "info", "twitter");
    }

    QJsonObject body = resp["body"].toObject();
    QJsonObject result = body["data"].toObject()["user"].toObject()["result"].toObject();
    if (result.isEmpty()) {
        m_backend->log("API returned empty user data", "error", "twitter");
        QString bodyStr = resp["body"].toString();
        if (!bodyStr.isEmpty()) m_backend->log("body (str): "+ bodyStr.left(300), "error", "twitter");
    } else {
        QJsonObject legacy = xLegacy(result);
        // Twitter API 2025+: name moved out of legacy, try to recover
        if (legacy["name"].toString().isEmpty()) {
            // Try core.user_results.result.legacy.name
            QString coreName = JsonShape::pickString(xLegacy(result), {"name", "screen_name"});
            if (coreName.isEmpty())
                coreName = JsonShape::pickString(result.value("core").toObject(), {"name", "screen_name"});
            if (!coreName.isEmpty()) {
                legacy["name"] = coreName;
                result["legacy"] = legacy;
            }
        }
        // Twitter API 2025+: profile_image_url_https moved to avatar
        if (legacy["profile_image_url_https"].toString().isEmpty()) {
            QJsonObject avatar = result["avatar"].toObject();
            QString avatarUrl;
            if (!avatar.isEmpty()) {
                // Try known avatar sub-fields
                avatarUrl = avatar["image_url"].toString();
                if (avatarUrl.isEmpty()) avatarUrl = avatar["url"].toString();
                if (avatarUrl.isEmpty()) avatarUrl = avatar["image_url_https"].toString();
                // Log avatar structure for debugging
                QStringList avKeys;
                for (auto it = avatar.begin(); it != avatar.end(); ++it) avKeys << it.key();
                m_backend->log("Avatar keys: "+ avKeys.join(","), "info", "twitter");
                m_backend->log("Avatar JSON: "+ QString(QJsonDocument(avatar).toJson(QJsonDocument::Compact)).left(300), "info", "twitter");
            }
            // Also try result["avatar"] as string
            if (avatarUrl.isEmpty() && result["avatar"].isString()) {
                avatarUrl = result["avatar"].toString();
            }
            if (!avatarUrl.isEmpty()) {
                legacy["profile_image_url_https"] = avatarUrl;
                result["legacy"] = legacy;
            }
        }
    }
    return result;
}

// ★ 다음 쪽 커서는 TimelineAddEntries 에만 있지 않다(r56) — X 는 검색의 2쪽부터 cursor-bottom 을 TimelineReplaceEntry 의
//   entry 에 싣는다(instruction.entry.content.value · 옛 모양은 content.operation.cursor.value). 데몬이 AddEntries 로 옮겨 주지만
//   (twitter_daemon.py _normalize_timeline) 고침 꾸러미의 시차로 옛 데몬이 올 수 있어 여기서도 읽는다(두 번째 방어선).
//   이것을 못 읽어 모든 검색이 두 쪽(약 40개)에서 끝났다 — '기간 지정' 이 그 달의 마지막 며칠만 받던 까닭.
static QString cursorValueOf(const QJsonObject &content)
{
    QString v = content.value(QStringLiteral("value")).toString();
    if (v.isEmpty())
        v = content.value(QStringLiteral("operation")).toObject().value(QStringLiteral("cursor")).toObject().value(QStringLiteral("value")).toString();
    if (v.isEmpty())
        v = content.value(QStringLiteral("itemContent")).toObject().value(QStringLiteral("value")).toString();
    return v;
}
static QString replaceEntryBottomCursor(const QJsonObject &instruction)
{
    if (instruction.value(QStringLiteral("type")).toString() != QLatin1String("TimelineReplaceEntry")) return QString();
    const QJsonObject e = instruction.value(QStringLiteral("entry")).toObject();
    if (!e.value(QStringLiteral("entryId")).toString().startsWith(QLatin1String("cursor-bottom"))
        && !instruction.value(QStringLiteral("entry_id_to_replace")).toString().startsWith(QLatin1String("cursor-bottom")))
        return QString();
    return cursorValueOf(e.value(QStringLiteral("content")).toObject());
}

QPair<QJsonArray, QString> TwitterCollector::searchTweets(const QString &query, const QString &cursor)
{
    // ★ 정지된 계정(r56) — 이 판에 처음 쓰는 계정이면 상태부터 한 번 묻고, 남은 요청이 0 이면 쉬는 계정으로 미리 돌린다.
    //   검색이 막힌 계정이면 묻지 않고 바로 SUSPENDED — 같은 문에 같은 질의를 되풀이하지 않는다(부르는 쪽이 다른 계정으로 돌린다).
    //   계정 목록이 없는 곳(새 트윗 확인 · 팔로잉 가져오기)에서는 하지 않는다.
    if (!m_accounts.isEmpty()) {
        checkAccountStatus(m_activeAccountIdx);
        rotateBeforeLimit(QStringLiteral("search"), true);
        if (m_searchBlocked.contains(m_activeAccountIdx)) {
            m_lastPageShaped = false;
            return {QJsonArray(), QStringLiteral("SUSPENDED")};
        }
    }

    QJsonObject cmd;
    cmd["action"] = "search";
    cmd["query"] = query;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;
    // ★ 이 쪽이 제 모양이었나(instructions 가 있고 errors 가 없나) — 검색도 다른 목록처럼 적는다. 예전엔 검색이 이 값을 건드리지 않아
    //   앞선 타임라인 · 좋아요의 값이 남았다(r55 의 바닥 표시가 '모양 없는 빈 결과' 를 가려야 해서 필요해졌다)
    m_lastPageShaped = false;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("SearchTimeline error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};

    if (status == 404 && resp.value(QStringLiteral("_cause")).toString() == QLatin1String("no_tid")) {
        m_backend->log("SearchTimeline 404 — X 서명(x-client-transaction-id)을 만들지 못했습니다: "
                       + resp.value(QStringLiteral("_tid_reason")).toString().left(200)
                       + " (해시·토큰 문제 아님)", "error", "twitter");
        // 서명 부품을 최신으로 맞출 수 있으면 맞추고 데몬을 다시 띄워 한 번 더 묻는다(12시간에 한 번)
        QString why;
        const auto stillRunning = [this]() { return !m_runFlag || m_runFlag->load(); };
        if (stillRunning() && SelfRepair::xSigningRepairDue()
            && SelfRepair::repairXSigningIfDue(Common::bundledPythonPath(), &why) && stillRunning()) {
            m_backend->log("  " + why + " — 데몬을 다시 띄우고 한 번 더 묻습니다", "success", "twitter");
            stopDaemon();
            if (startDaemon()) {
                resp = sendDaemonCommand(cmd);
                status = resp.contains("error") ? 0 : resp["status"].toInt();
            }
        } else if (!why.isEmpty()) {
            m_backend->log("  " + why, "info", "twitter");
        }
        if (status != 200) return {QJsonArray(), QStringLiteral("ERROR")};
    }
    // ★ 404 하나로는 해시가 바뀐 증거가 아니다(r56) — 데몬이 이미 GET · POST 를 둘 다 물었다(X 는 2026-11-21 부터 검색의 GET 에
    //   빈 404). 예전엔 데몬을 세 번 다시 띄웠다(그때마다 해시 긁기 · 서명 초기화가 X 로 나갔다). 이제 해시만 force 로 새로 받아
    //   한 번 더 묻고, 그래도 안 되면 오류로 돌려준다(검색 루프의 '5번 이어지면 멈춤' 이 받는다).
    if (status == 404) {
        m_backend->log("SearchTimeline HTTP 404 — GET · POST 둘 다 404 입니다. 해시를 새로 받아 한 번 더 묻습니다", "warning", "twitter");
        QJsonObject updateCmd{{"action", "update_endpoints"}, {"force", true}};
        const QJsonObject updateResp = sendDaemonCommand(updateCmd, 30000);
        if (updateResp.value(QStringLiteral("status")).toString() == QLatin1String("ok")) {
            resp = sendDaemonCommand(cmd);
            status = resp.contains("error") ? 0 : resp["status"].toInt();
        }
        if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
        if (status != 200) {
            m_backend->log(QString("SearchTimeline HTTP %1 — 해시를 새로 받아도 풀리지 않았습니다").arg(status ? status : 404), "error", "twitter");
            return {QJsonArray(), QStringLiteral("ERROR")};
        }
        m_backend->log("SearchTimeline 복구 성공!", "success", "twitter");
    }
    // 401/403: 토큰 만료 → 데몬 재시작으로 자동 갱신 (최대 3회)
    if (status == 401 || status == 403) {
        m_backend->log(QString("SearchTimeline HTTP %1 → 자동 갱신 시도 (최대 3회)").arg(status), "warning", "twitter");
        for (int retry = 0; retry < 3; ++retry) {
            if (retry > 0) {
                m_backend->log(QString("재시도 %1/3: 데몬 재시작...").arg(retry + 1), "info", "twitter");
            }
            // 데몬 재시작 → 세션 + 해시 완전 리셋
            stopDaemon();
            QThread::sleep(3);
            if (!startDaemon()) {
                m_backend->log("데몬 재시작 실패", "error", "twitter");
                return {QJsonArray(), QStringLiteral("ERROR")};
            }
            QThread::sleep(2);
            QJsonObject retryResp = sendDaemonCommand(cmd);
            int retryStatus = retryResp["status"].toInt();
            if (!retryResp.contains("error") && retryStatus == 200) {
                m_backend->log("SearchTimeline 복구 성공!", "success", "twitter");
                resp = retryResp;
                status = 200;
                break;
            }
            m_backend->log(QString("재시도 %1 실패 (HTTP %2)").arg(retry + 1).arg(retryStatus), "warning", "twitter");
        }
        if (status != 200) {
            m_backend->log(QString("SearchTimeline HTTP %1 복구 불가").arg(status), "error", "twitter");
            return {QJsonArray(), QStringLiteral("ERROR")};
        }
    }
    if (status != 200) {
        m_backend->log(QString("SearchTimeline HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }
    // ★ 정지된 계정의 검색 문(r56) — X 가 200 + errors '...DenySuspendedUsers' 를 준다(데몬이 _cause 로 알린다).
    //   해시 · 서명 · 토큰 문제가 아니다 — 데몬을 다시 띄우지도 해시를 받지도 않는다. 이 계정을 이 판의 검색에서 빼고 SUSPENDED.
    if (resp.value(QStringLiteral("_cause")).toString() == QLatin1String("account_suspended")) {
        if (!m_accounts.isEmpty() && !m_searchBlocked.contains(m_activeAccountIdx)) {
            m_searchBlocked.insert(m_activeAccountIdx);
            const QString nm = m_accounts.at(m_activeAccountIdx).toObject().value(QStringLiteral("name")).toString();
            m_backend->log(QString("계정 %1%2: X 가 정지 상태로 보아 검색을 막았습니다(%3) — 이 판에서는 이 계정으로 검색하지 않습니다")
                               .arg(m_activeAccountIdx + 1).arg(nm.isEmpty() ? QString() : QStringLiteral("(") + nm + QStringLiteral(")"))
                               .arg(resp.value(QStringLiteral("_gate")).toString()), "warning", "twitter");
        }
        m_lastPageShaped = false;
        return {QJsonArray(), QStringLiteral("SUSPENDED")};
    }

    QJsonObject data = resp["body"].toObject();

    QJsonArray results;
    QString nextCursor;
    QString insHow;
    QJsonArray instructions = xInstructions(data, {"search_by_raw_query", "search", "search_timeline"}, &insHow);
    if (!insHow.isEmpty())
        m_backend->log(QString("X 검색 응답 모양이 바뀐 듯합니다 — '%1' 에서 찾았습니다. 받기는 계속합니다.")
                           .arg(insHow), "warning", "twitter");

    for (const auto &inst : instructions) {
        QJsonObject instruction = inst.toObject();
        if (instruction["type"].toString() == "TimelineAddEntries") {
            QJsonArray entries = instruction["entries"].toArray();
            for (const auto &entry : entries) {
                QJsonObject entryObj = entry.toObject();
                QString entryId = entryObj["entryId"].toString();
                if (entryId.startsWith("tweet-")) {
                    QJsonObject tweetResult = xTweetResult(entryObj);

                    if (tweetResult.contains("tweet")) {
                        tweetResult = tweetResult["tweet"].toObject();
                    }

                    if (!tweetResult.isEmpty()) {
                        results.append(tweetResult);
                    }
                } else if (entryId.startsWith("cursor-bottom")) {
                    const QString cv = cursorValueOf(entryObj["content"].toObject());
                    if (!cv.isEmpty()) nextCursor = cv;
                }
            }
        } else if (const QString rc = replaceEntryBottomCursor(instruction); !rc.isEmpty()) {
            nextCursor = rc;   // 2쪽부터의 X 모양(위 설명)
        }
    }

    m_lastPageShaped = !instructions.isEmpty() && data.value(QStringLiteral("errors")).toArray().isEmpty();
    return {results, nextCursor};
}

QPair<QJsonArray, QString> TwitterCollector::getUserTweets(const QString &userId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "user_tweets";
    cmd["user_id"] = userId;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;
    m_lastPageShaped = false;
    if (!m_accounts.isEmpty()) rotateBeforeLimit(QStringLiteral("user_tweets"), false);   // 남은 요청 0 — 429 전에 쉬는 계정으로(r56)

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("UserTweets error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};

    // ★ 404: 데몬이 이미 GET · POST 를 둘 다 물었다 — 해시만 force 로 새로 받아 한 번 더(r56). 기동 때는 캐시가 젊으면 긁지 않으므로
    //   데몬을 다시 띄우는 것으로는 해시가 새로 오지 않는다.
    if (status == 404) {
        m_backend->log("UserTweets HTTP 404 → 해시를 새로 받아 한 번 더 묻습니다", "warning", "twitter");
        QJsonObject updateCmd{{"action", "update_endpoints"}, {"force", true}};
        if (sendDaemonCommand(updateCmd, 30000).value(QStringLiteral("status")).toString() == QLatin1String("ok")) {
            const QJsonObject retryResp = sendDaemonCommand(cmd);
            if (!retryResp.contains("error") && retryResp["status"].toInt() == 200) {
                m_backend->log("UserTweets 복구 성공!", "success", "twitter");
                resp = retryResp;
                status = 200;
            }
        }
    }
    // 401/403(그리고 해시로 풀리지 않은 404): 토큰 만료 → 데몬 재시작으로 자동 갱신
    if (status == 401 || status == 403 || status == 404) {
        m_backend->log(QString("UserTweets HTTP %1 → 자동 갱신 시도").arg(status), "warning", "twitter");
        for (int retry = 0; retry < 3; ++retry) {
            stopDaemon();
            QThread::sleep(3);
            if (!startDaemon()) {
                m_backend->log("데몬 재시작 실패", "error", "twitter");
                return {QJsonArray(), QStringLiteral("ERROR")};
            }
            QThread::sleep(2);
            QJsonObject retryResp = sendDaemonCommand(cmd);
            int retryStatus = retryResp["status"].toInt();
            if (!retryResp.contains("error") && retryStatus == 200) {
                m_backend->log("UserTweets 복구 성공!", "success", "twitter");
                resp = retryResp;
                status = 200;
                break;
            }
            m_backend->log(QString("재시도 %1 실패 (HTTP %2)").arg(retry + 1).arg(retryStatus), "warning", "twitter");
        }
        if (status != 200) {
            m_backend->log(QString("UserTweets HTTP %1 복구 불가").arg(status), "error", "twitter");
            return {QJsonArray(), QStringLiteral("ERROR")};
        }
    }
    if (status != 200) {
        m_backend->log(QString("UserTweets HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonObject json = resp["body"].toObject();
    QJsonArray results;
    QString nextCursor;

    // Check for API errors
    QJsonArray errors = json["errors"].toArray();
    if (!errors.isEmpty()) {
        for (const auto &e : errors) {
            m_backend->log("API error: "+ e.toObject()["message"].toString(), "error", "twitter");
        }
    }

    // data → user → result → (timeline_v2|timeline|…) → timeline → instructions
    //   ★ 물러설 이름을 손으로 두 개 적어 두었던 자리다. 이제 xInstructions 가
    //     아는 이름을 전부 훑고, 그래도 없으면 모양으로 찾는다.
    QJsonObject userData = json["data"].toObject()["user"].toObject()["result"].toObject();
    QString utHow;
    QJsonArray instructions = xInstructions(json, {"user", "user_result"}, &utHow);
    if (!utHow.isEmpty())
        m_backend->log(QString("X 타임라인 응답 모양이 바뀐 듯합니다 — '%1' 에서 찾았습니다.")
                           .arg(utHow), "warning", "twitter");

    if (instructions.isEmpty() && cursor.isEmpty()) {
        QStringList keys;
        for (auto it = userData.begin(); it != userData.end(); ++it) keys << it.key();
        m_backend->log("UserTweets user.result keys: ["+ keys.join(", ") + "]", "warning", "twitter");
    }

    QJsonObject pinnedTweet;   // ★ 고정 트윗 — 첫 쪽의 TimelinePinEntry 로 온다(2026-11-27 실측). 예전엔 이 지시를 버려 📌 표시가 한 번도 붙지 않았다(r56)
    for (const auto &inst : instructions) {
        QJsonObject instruction = inst.toObject();
        QString instType = instruction["type"].toString();
        if (instType == "TimelinePinEntry") {
            QJsonObject tr = xTweetResult(instruction["entry"].toObject()["content"].toObject());
            if (tr.contains("tweet")) tr = tr["tweet"].toObject();
            if (!tr.isEmpty()) { tr["pinned"] = true; pinnedTweet = tr; }
        } else if (const QString rc = replaceEntryBottomCursor(instruction); !rc.isEmpty()) {
            nextCursor = rc;
        }
        if (instType == "TimelineAddEntries") {
            QJsonArray entries = instruction["entries"].toArray();
            for (const auto &entry : entries) {
                QJsonObject e = entry.toObject();
                QString eid = e["entryId"].toString();
                if (eid.startsWith("tweet-") || eid.startsWith("profile-conversation-")) {
                    QJsonObject content = e["content"].toObject();
                    QString entryType = content["entryType"].toString();
                    if (entryType == "TimelineTimelineItem"|| content.contains("itemContent")) {
                        QJsonObject tweetResult = xTweetResult(content);
                        if (tweetResult.contains("tweet")) tweetResult = tweetResult["tweet"].toObject();
                        if (!tweetResult.isEmpty()) results.append(tweetResult);
                    } else if (entryType == "TimelineTimelineModule") {
                        QJsonArray items = content["items"].toArray();
                        for (const auto &item : items) {
                            QJsonObject tweetResult = xTweetResult(item.toObject());
                            if (tweetResult.contains("tweet")) tweetResult = tweetResult["tweet"].toObject();
                            if (!tweetResult.isEmpty()) results.append(tweetResult);
                        }
                    }
                } else if (eid.startsWith("cursor-bottom")) {
                    const QString cv = cursorValueOf(e["content"].toObject());
                    if (!cv.isEmpty()) nextCursor = cv;
                }
            }
        }
    }
    // 고정 트윗은 쪽의 맨 앞에 — 대개 옛 글이라, 쪽의 '마지막(가장 옛) 글' 로 끝을 가르는 곳(기간 판의 타임라인)을 흐리지 않게.
    //   processTweetBatch 는 pinned 글을 '위쪽 이어짐' · 일찍 멈춤 셈에 넣지 않는다(옛 ID 로 '지난번 가장 새 글에 닿았다' 고 보지 않게)
    //   X 가 같은 글을 제자리(AddEntries)에도 실으면 둘을 싣지 않는다 — 제자리의 것에 표시만(답글 모으기가 같은 글을 두 번 묻지 않게)
    if (!pinnedTweet.isEmpty()) {
        const QString pid = pinnedTweet.value(QStringLiteral("rest_id")).toString();
        bool inPlace = false;
        for (int i = 0; i < results.size() && !pid.isEmpty(); ++i) {
            QJsonObject o = results.at(i).toObject();
            if (o.value(QStringLiteral("rest_id")).toString() != pid) continue;
            o[QStringLiteral("pinned")] = true;
            results[i] = o;
            inPlace = true;
            break;
        }
        if (!inPlace) results.prepend(pinnedTweet);
    }
    m_lastPageShaped = !instructions.isEmpty() && errors.isEmpty();
    return {results, nextCursor};
}

QString TwitterCollector::extractCursorFromEntries(const QJsonArray &entries) const
{
    for (const auto &entry : entries) {
        QJsonObject entryObj = entry.toObject();
        if (entryObj["entryId"].toString().startsWith("cursor-bottom")) {
            return entryObj["content"].toObject()["value"].toString();
        }
    }
    return QString();
}

QPair<QJsonArray, QString> TwitterCollector::getTweetDetail(const QString &tweetId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "tweet_detail";
    cmd["tweet_id"] = tweetId;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd, 30000);
    if (resp.contains("error")) {
        m_backend->log("TweetDetail error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status != 200) {
        m_backend->log(QString("TweetDetail HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonObject json = resp["body"].toObject();
    QJsonArray results;
    QString nextCursor;

    // Navigate: data → threaded_conversation_with_injections_v2 → instructions
    QJsonArray instructions = xInstructions(json, {"threaded_conversation_with_injections_v2",
        "threaded_conversation_with_injections", "threaded_conversation_with_injections_v3",
        "threaded_conversation", "timeline_response"});

    for (const auto &inst : instructions) {
        QJsonObject instruction = inst.toObject();
        QString instType = instruction["type"].toString();

        if (instType == "TimelineAddEntries") {
            QJsonArray entries = instruction["entries"].toArray();
            for (const auto &entry : entries) {
                QJsonObject e = entry.toObject();
                QString eid = e["entryId"].toString();

                // conversationthread- entries contain reply items
                if (eid.startsWith("conversationthread-")) {
                    QJsonArray items = e["content"].toObject()["items"].toArray();
                    for (const auto &item : items) {
                        QJsonObject tweetResult = xTweetResult(item.toObject());
                        if (tweetResult.contains("tweet")) tweetResult = tweetResult["tweet"].toObject();
                        if (!tweetResult.isEmpty()) results.append(tweetResult);
                    }
                }
                // Individual tweet entries (direct replies)
                else if (eid.startsWith("tweet-")) {
                    QJsonObject content = e["content"].toObject();
                    QJsonObject tweetResult = xTweetResult(content);
                    if (tweetResult.contains("tweet")) tweetResult = tweetResult["tweet"].toObject();
                    // Skip the focal tweet itself
                    QString tid = tweetResult["rest_id"].toString();
                    if (!tweetResult.isEmpty() && tid != tweetId)
                        results.append(tweetResult);
                }
                // Cursor for more replies
                else if (eid.contains("cursor-bottom") || eid.contains("cursor-showmore")) {
                    QString cv = e["content"].toObject()["value"].toString();
                    if (cv.isEmpty())
                        cv = e["content"].toObject()["itemContent"].toObject()["value"].toString();
                    if (!cv.isEmpty()) nextCursor = cv;
                }
            }
        }
        // TimelineAddToModule: additional replies loaded via "Show more replies"
else if (instType == "TimelineAddToModule") {
            QJsonArray items = instruction["moduleItems"].toArray();
            for (const auto &item : items) {
                QJsonObject tweetResult = xTweetResult(item.toObject());
                if (tweetResult.contains("tweet")) tweetResult = tweetResult["tweet"].toObject();
                if (!tweetResult.isEmpty()) results.append(tweetResult);
            }
        }
    }

    return {results, nextCursor};
}

// ★ 글 하나를 다시 받는다(r56 — 지난 판에 미디어를 받지 못한 글을 다시 받을 때, COV-3). getTweetDetail 은 그 글 자신(focal)을
//   빼고 답글만 주므로 따로 둔다. 데몬의 tweet_detail 이 그대로 돌려준 몸에서 rest_id 가 그 글인 것을 찾는다(대화 묶음 안 · 낱개 둘 다).
QJsonObject TwitterCollector::fetchTweetById(const QString &tweetId, QString *status)
{
    auto setStatus = [&](const char *s) { if (status) *status = QString::fromLatin1(s); };
    QJsonObject cmd;
    cmd["action"] = "tweet_detail";
    cmd["tweet_id"] = tweetId;
    const QJsonObject resp = sendDaemonCommand(cmd, 30000);
    if (resp.contains("error")) { setStatus("ERROR"); return QJsonObject(); }
    const int st = resp["status"].toInt();
    if (st == 429) { setStatus("RATE_LIMITED"); return QJsonObject(); }
    if (st != 200) { setStatus("ERROR"); return QJsonObject(); }
    const QJsonArray instructions = xInstructions(resp["body"].toObject(), {"threaded_conversation_with_injections_v2",
        "threaded_conversation_with_injections", "threaded_conversation_with_injections_v3",
        "threaded_conversation", "timeline_response"});
    if (instructions.isEmpty()) { setStatus("ERROR"); return QJsonObject(); }   // 모양 없는 답 — 다음에 다시
    auto pick = [&](const QJsonObject &holder) -> QJsonObject {
        QJsonObject t = xTweetResult(holder);
        if (t.contains("tweet")) t = t["tweet"].toObject();
        QString id = xLegacy(t)["id_str"].toString();
        if (id.isEmpty()) id = t["rest_id"].toString();
        return id == tweetId ? t : QJsonObject();
    };
    for (const auto &inst : instructions) {
        const QJsonObject instruction = inst.toObject();
        QJsonArray entries = instruction["entries"].toArray();
        if (instruction.contains("entry")) entries.append(instruction["entry"]);
        for (const auto &ev : entries) {
            const QJsonObject e = ev.toObject();
            const QJsonObject content = e["content"].toObject();
            if (QJsonObject t = pick(content); !t.isEmpty()) { setStatus("OK"); return t; }
            for (const auto &item : content["items"].toArray())
                if (QJsonObject t = pick(item.toObject()); !t.isEmpty()) { setStatus("OK"); return t; }
        }
    }
    setStatus("GONE");
    return QJsonObject();
}

QPair<QJsonArray, QString> TwitterCollector::getLikes(const QString &userId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "likes";
    cmd["user_id"] = userId;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;
    m_lastPageShaped = false;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("Likes error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status == 404) {
        m_backend->log("Likes HTTP 404 → 엔드포인트 갱신 시도", "warning", "twitter");
        QJsonObject updateCmd;
        updateCmd["action"] = "update_endpoints";
        updateCmd["force"] = true;   // 캐시가 젊어도 긁는다(r56 — 404 수리 길)
        QJsonObject updateResp = sendDaemonCommand(updateCmd, 30000);
        if (updateResp["status"].toString() == "ok") {
            QJsonObject retryResp = sendDaemonCommand(cmd);
            if (!retryResp.contains("error") && retryResp["status"].toInt() == 200) {
                resp = retryResp;
                status = 200;
            }
        }
    }
    if (status != 200) {
        m_backend->log(QString("Likes HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonArray results;
    QString nextCursor;
    QJsonArray instructions = xInstructions(resp["body"].toObject(), {"user", "user_result"});

    for (const auto &inst : instructions) {
        QJsonObject instruction = inst.toObject();
        if (instruction["type"].toString() == "TimelineAddEntries") {
            QJsonArray entries = instruction["entries"].toArray();
            for (const auto &entry : entries) {
                QJsonObject e = entry.toObject();
                QString eid = e["entryId"].toString();
                if (eid.startsWith("tweet-")) {
                    QJsonObject tweetResult = xTweetResult(e);
                    if (tweetResult.contains("tweet")) tweetResult = tweetResult["tweet"].toObject();
                    if (!tweetResult.isEmpty()) results.append(tweetResult);
                } else if (eid.startsWith("cursor-bottom")) {
                    nextCursor = e["content"].toObject()["value"].toString();
                }
            }
        }
    }
    m_lastPageShaped = !instructions.isEmpty() && resp["body"].toObject()["errors"].toArray().isEmpty();
    return {results, nextCursor};
}

QPair<QJsonArray, QString> TwitterCollector::getBookmarks(const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "bookmarks";
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;
    m_lastPageShaped = false;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("Bookmarks error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status == 404) {
        m_backend->log("Bookmarks HTTP 404 → 엔드포인트 갱신 시도", "warning", "twitter");
        QJsonObject updateCmd;
        updateCmd["action"] = "update_endpoints";
        updateCmd["force"] = true;   // 캐시가 젊어도 긁는다(r56 — 404 수리 길)
        QJsonObject updateResp = sendDaemonCommand(updateCmd, 30000);
        if (updateResp["status"].toString() == "ok") {
            QJsonObject retryResp = sendDaemonCommand(cmd);
            if (!retryResp.contains("error") && retryResp["status"].toInt() == 200) {
                resp = retryResp;
                status = 200;
            }
        }
    }
    if (status != 200) {
        m_backend->log(QString("Bookmarks HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonArray results;
    QString nextCursor;
    QJsonArray instructions = xInstructions(resp["body"].toObject(),
        {"bookmark_timeline_v2", "bookmark_timeline", "bookmark_timeline_v3", "bookmark_collection_timeline"});

    for (const auto &inst : instructions) {
        QJsonObject instruction = inst.toObject();
        if (instruction["type"].toString() == "TimelineAddEntries") {
            QJsonArray entries = instruction["entries"].toArray();
            for (const auto &entry : entries) {
                QJsonObject e = entry.toObject();
                QString eid = e["entryId"].toString();
                if (eid.startsWith("tweet-")) {
                    QJsonObject tweetResult = xTweetResult(e);
                    if (tweetResult.contains("tweet")) tweetResult = tweetResult["tweet"].toObject();
                    if (!tweetResult.isEmpty()) results.append(tweetResult);
                } else if (eid.startsWith("cursor-bottom")) {
                    nextCursor = e["content"].toObject()["value"].toString();
                }
            }
        }
    }
    m_lastPageShaped = !instructions.isEmpty() && resp["body"].toObject()["errors"].toArray().isEmpty();
    return {results, nextCursor};
}

// 공통 헬퍼: 트윗/유저 타임라인에서 tweet-, user-, cursor-bottom 엔트리 파싱
static void _parseTimelineEntries(const QJsonArray &instructions, QJsonArray &results,
                                  QString &nextCursor, bool expectUsers = false)
{
    for (const auto &inst : instructions) {
        QJsonObject instruction = inst.toObject();
        if (const QString rc = replaceEntryBottomCursor(instruction); !rc.isEmpty()) {
            nextCursor = rc;   // 2쪽부터 ReplaceEntry 에 오는 커서(r56 — searchTweets 위 설명)
            continue;
        }
        if (instruction["type"].toString() == "TimelineAddEntries") {
            QJsonArray entries = instruction["entries"].toArray();
            for (const auto &entry : entries) {
                QJsonObject e = entry.toObject();
                QString eid = e["entryId"].toString();
                QJsonObject content = e["content"].toObject();
                QString cType = content["entryType"].toString();
                if (cType == "TimelineTimelineCursor") {
                    QString ct = content["cursorType"].toString();
                    if (ct == "Bottom") nextCursor = content["value"].toString();
                    continue;
                }
                if (expectUsers && eid.startsWith("user-")) {
                    QJsonObject userResult = xUserResult(content);
                    if (!userResult.isEmpty()) results.append(userResult);
                } else if (!expectUsers && eid.startsWith("tweet-")) {
                    QJsonObject tr = xTweetResult(content);
                    if (tr.contains("tweet")) tr = tr["tweet"].toObject();
                    if (!tr.isEmpty()) results.append(tr);
                } else if (eid.startsWith("cursor-bottom")) {
                    QString cv = content["value"].toString();
                    if (!cv.isEmpty()) nextCursor = cv;
                }
            }
        }
    }
}

QPair<QJsonArray, QString> TwitterCollector::getHighlights(const QString &userId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "highlights";
    cmd["user_id"] = userId;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("Highlights error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }
    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status != 200) {
        m_backend->log(QString("Highlights HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonArray instructions = xInstructions(resp["body"].toObject(), {"user", "user_result"});
    QJsonArray results;
    QString nextCursor;
    _parseTimelineEntries(instructions, results, nextCursor, false);
    return {results, nextCursor};
}

QPair<QJsonArray, QString> TwitterCollector::getFavoriters(const QString &tweetId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "favoriters";
    cmd["tweet_id"] = tweetId;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("Favoriters error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }
    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status != 200) {
        m_backend->log(QString("Favoriters HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonArray instructions = xInstructions(resp["body"].toObject(), {"favoriters_timeline", "liking_users_timeline"});
    QJsonArray results;
    QString nextCursor;
    _parseTimelineEntries(instructions, results, nextCursor, true);
    return {results, nextCursor};
}

QPair<QJsonArray, QString> TwitterCollector::getRetweeters(const QString &tweetId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "retweeters";
    cmd["tweet_id"] = tweetId;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("Retweeters error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }
    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status != 200) {
        m_backend->log(QString("Retweeters HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonArray instructions = xInstructions(resp["body"].toObject(), {"retweeters_timeline", "retweeted_by_timeline"});
    QJsonArray results;
    QString nextCursor;
    _parseTimelineEntries(instructions, results, nextCursor, true);
    return {results, nextCursor};
}

QPair<QJsonArray, QString> TwitterCollector::getListTweets(const QString &listId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "list_tweets";
    cmd["list_id"] = listId;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("ListTweets error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }
    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status != 200) {
        m_backend->log(QString("ListTweets HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonArray instructions = xInstructions(
        QJsonObject{{"data", resp["body"].toObject()["data"].toObject()["list"].toObject()}},
        {"tweets_timeline", "timeline"});
    QJsonArray results;
    QString nextCursor;
    _parseTimelineEntries(instructions, results, nextCursor, false);
    return {results, nextCursor};
}

QPair<QJsonArray, QString> TwitterCollector::getCommunityTweets(const QString &communityId, const QString &cursor)
{
    QJsonObject cmd;
    cmd["action"] = "community_tweets";
    cmd["community_id"] = communityId;
    cmd["count"] = 40;
    if (!cursor.isEmpty()) cmd["cursor"] = cursor;

    QJsonObject resp = sendDaemonCommand(cmd);
    if (resp.contains("error")) {
        m_backend->log("CommunityTweets error: "+ resp["error"].toString(), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }
    int status = resp["status"].toInt();
    if (status == 429) return {QJsonArray(), "RATE_LIMITED"};
    if (status != 200) {
        m_backend->log(QString("CommunityTweets HTTP %1").arg(status), "error", "twitter");
        return {QJsonArray(), QStringLiteral("ERROR")};
    }

    QJsonArray instructions = xInstructions(resp["body"].toObject(), {"communityResults", "community_results"});
    QJsonArray results;
    QString nextCursor;
    _parseTimelineEntries(instructions, results, nextCursor, false);
    return {results, nextCursor};
}

// ═══════════════════════════════════════════════════════════════════════════
// r56 미디어 · 엑셀 칸 도우미 — 글 한 개에서 '올라온 것을 빠짐없이' 꺼내는 데 쓴다
// ═══════════════════════════════════════════════════════════════════════════

// 엑셀 한 줄의 칸 — 차례 그대로 둔다. ★ 새 칸은 늘 '끝에' 붙인다(r56: alt_text … quoted_author). 옛 파일(30칸 · 아니포 21칸)은
//   읽을 때 빈칸으로 채워 새 머리로 다시 쓴다. 다른 곳(스페이스 · 스레드 자동탐지 · 보기 도구)이 칸 번호 · 머리 이름으로 읽는다.
static const QStringList &tweetColumns()
{
    static const QStringList cols = {
        "id", "tweet_url", "text", "language", "type",
        "author_name", "author_username",
        "bookmark_count", "favorite_count", "retweet_count",
        "retweeted", "reply_count", "quote_count", "view_count",
        "created_at", "retweet_time", "source",
        "hashtags", "urls", "media_type", "media_urls",
        "conversation_id", "in_reply_to", "is_quote", "quoted_tweet_url",
        "possibly_sensitive", "user_followers", "user_following",
        "user_verified", "user_profile_image",
        // r56 — 올라와 있는데 담지 않던 것
        "alt_text", "card_title", "card_url", "article_title", "article_text",
        "edit_ids", "quoted_text", "quoted_author"};
    return cols;
}
// 숫자로 담는 칸(줄 객체에는 int 로 들어 있다 — 글자로 읽으면 0 이 된다)
static bool tweetIntColumn(const QString &c)
{
    return c == QLatin1String("bookmark_count") || c == QLatin1String("favorite_count") || c == QLatin1String("retweet_count")
        || c == QLatin1String("reply_count") || c == QLatin1String("quote_count");
}
// 줄 객체 → 엑셀 한 줄(tweetColumns 차례)
static QStringList tweetRowCells(const QJsonObject &row)
{
    QStringList out;
    out.reserve(tweetColumns().size());
    for (const QString &c : tweetColumns())
        out << (tweetIntColumn(c) ? QString::number(row.value(c).toInt()) : row.value(c).toString());
    return out;
}

// 리트윗이면 원글(TweetWithVisibilityResults 껍질은 벗긴다), 아니면 빈 것
static QJsonObject retweetedOf(const QJsonObject &tweet)
{
    QJsonObject rt = xLegacy(tweet)["retweeted_status_result"].toObject()["result"].toObject();
    if (rt.isEmpty()) rt = tweet["retweeted_status_result"].toObject()["result"].toObject();
    if (rt.contains("tweet")) rt = rt["tweet"].toObject();
    return rt;
}
// 인용한 글(껍질은 벗긴다)
static QJsonObject quotedOf(const QJsonObject &tweet)
{
    QJsonObject q = tweet["quoted_status_result"].toObject()["result"].toObject();
    if (q.contains("tweet")) q = q["tweet"].toObject();
    return q;
}
// 긴 글(note_tweet) 의 알맹이 — text · entity_set
static QJsonObject noteTweetOf(const QJsonObject &tweet)
{
    return tweet["note_tweet"].toObject()["note_tweet_results"].toObject()["result"].toObject();
}
// 글의 온전한 본문 — 긴 글이면 그쪽(280자에서 잘리지 않게)
static QString fullTextOf(const QJsonObject &tweet)
{
    QString t = xLegacy(tweet)["full_text"].toString();
    const QString lt = noteTweetOf(tweet)["text"].toString();
    if (lt.length() > t.length()) t = lt;
    return t;
}

// 영상의 가장 좋은 mp4 — 없고 HLS(m3u8)만 있으면 hlsOnly 를 세우고 m3u8 주소를 m3u8Out 에 준다
static QString bestMp4Url(const QJsonObject &mediaObj, bool *hlsOnly = nullptr, QString *m3u8Out = nullptr)
{
    if (hlsOnly) *hlsOnly = false;
    const QJsonArray variants = mediaObj["video_info"].toObject()["variants"].toArray();
    int bestBitrate = -1;
    QString best, m3u8;
    for (const auto &v : variants) {
        const QJsonObject variant = v.toObject();
        const QString ct = variant["content_type"].toString();
        if (ct == QLatin1String("video/mp4")) {
            const int bitrate = variant["bitrate"].toInt();
            if (bitrate > bestBitrate) { bestBitrate = bitrate; best = variant["url"].toString(); }
        } else if (m3u8.isEmpty() && (ct.contains(QLatin1String("mpegurl"), Qt::CaseInsensitive)
                                      || variant["url"].toString().contains(QLatin1String(".m3u8")))) {
            m3u8 = variant["url"].toString();
        }
    }
    if (best.isEmpty() && !variants.isEmpty() && !m3u8.isEmpty()) {
        if (hlsOnly) *hlsOnly = true;
        if (m3u8Out) *m3u8Out = m3u8;
    }
    return best;
}

// 카드(링크 미리보기)의 binding_values — 열쇠 → 값 객체
static QHash<QString, QJsonObject> cardBindings(const QJsonObject &tweet)
{
    QHash<QString, QJsonObject> m;
    const QJsonObject card = tweet["card"].toObject();
    for (const auto &bv : xLegacy(card)["binding_values"].toArray()) {
        const QJsonObject b = bv.toObject();
        const QString k = b["key"].toString();
        if (!k.isEmpty()) m.insert(k, b["value"].toObject());
    }
    return m;
}
// 통합 카드(unified_card — 앱 · 동영상 광고형 카드)는 JSON 글자 한 덩이로 온다
static QJsonObject unifiedCardOf(const QHash<QString, QJsonObject> &bind)
{
    const QString s = bind.value(QStringLiteral("unified_card"))["string_value"].toString();
    return s.isEmpty() ? QJsonObject() : QJsonDocument::fromJson(s.toUtf8()).object();
}
// 카드의 큰 그림 — 원본 크기를 먼저
static QString cardImageUrl(const QHash<QString, QJsonObject> &bind)
{
    for (const char *k : {"thumbnail_image_original", "photo_image_full_size_original", "summary_photo_image_original",
                          "player_image_original", "thumbnail_image_large", "summary_photo_image_large", "player_image_large"}) {
        const QString u = bind.value(QLatin1String(k))["image_value"].toObject()["url"].toString();
        if (!u.isEmpty()) return u;
    }
    return QString();
}
// X 글(Articles)
static QJsonObject articleOf(const QJsonObject &tweet)
{
    return tweet["article"].toObject()["article_results"].toObject()["result"].toObject();
}

// ★ EXIF 를 다 적은 파일의 표시(r56) — 받아 둔 그림을 다시 볼 때마다 exiftool(perl)을 띄워 파일을 통째로 다시 쓰던 것을 건너뛴다
//   (좋아요 1만 개를 다시 돌면 30~60분이 그것이었다 — 속도 점검 F3). 표시는 addExifMetadata 가 성공한 뒤에만 단다. 시각(mtime)만 보면
//   예전에 EXIF 를 못 적은 파일도 건너뛰게 된다(setFileTimes 는 늘 마지막에 돈다) — 둘 다 맞아야 건너뛴다.
static const char *const kExifDoneXattr = "com.hanishiki.exif";
static bool exifDoneMarked(const QString &path)
{
#ifdef Q_OS_MACOS
    return getxattr(QFile::encodeName(path).constData(), kExifDoneXattr, nullptr, 0, 0, XATTR_NOFOLLOW) > 0;
#else
    Q_UNUSED(path);
    return false;
#endif
}
static void markExifDone(const QString &path)
{
#ifdef Q_OS_MACOS
    setxattr(QFile::encodeName(path).constData(), kExifDoneXattr, "1", 1, 0, XATTR_NOFOLLOW);
#else
    Q_UNUSED(path);
#endif
}
// 받아 둔 파일이 이미 다 손본 것인가 — 시각이 글의 시각과 같고, (EXIF 를 적을 그림이면) 표시가 있다
static bool mediaAlreadyFinished(const QString &path, const QString &createdAt, bool wantsExif)
{
    if (createdAt.isEmpty()) return false;
    const QDateTime t = Common::parseISODate(createdAt);
    if (!t.isValid() || QFileInfo(path).lastModified().toSecsSinceEpoch() != t.toSecsSinceEpoch()) return false;
    return !wantsExif || exifDoneMarked(path);
}

// 프로필 사진의 원본 주소 — _normal · _bigger · _400x400 … 크기 꼬리를 뗀다(그 주소가 올린 그대로의 원본이다)
static QString originalAvatarUrl(const QString &url)
{
    static const QRegularExpression re(QStringLiteral("_(?:normal|bigger|mini|reasonably_small|x96|\\d+x\\d+)(\\.[A-Za-z0-9]+)?$"));
    QString u = url;
    const QString path = u.split('?').first();
    const QRegularExpressionMatch m = re.match(path);
    if (!m.hasMatch()) return u;
    return path.left(m.capturedStart()) + m.captured(1);
}

// 작은 JSON 파일 — 다시 받을 목록 · 프로필 주소 기억 · 대화 미디어 남은 목록
static QJsonObject readJsonFile(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QJsonObject();
    return QJsonDocument::fromJson(f.readAll()).object();
}
static bool writeJsonFile(const QString &path, const QJsonObject &o)
{
    if (o.isEmpty()) { QFile::remove(path); return true; }   // 비면 파일을 남기지 않는다
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
    return f.commit();
}

// ★ 프로필 사진 · 배너는 주소가 바뀌었을 때만 새로 받는다(r56). 예전엔 날짜를 붙인 이름(profile_20261127.jpg)으로 날마다 새로 받아,
//   같은 그림이 날마다 한 장씩 쌓였다(사용자에겐 '정리가 안 된다' 로 보였다). X 는 그림을 바꾸면 주소도 바뀐다 — 마지막으로 받은
//   주소를 그 사람 폴더의 확장 속성(xattr)에 적어 두고, 같으면 받지 않는다. 처음 보는 폴더(예전 판이 만든 것)는 받아 본 뒤 바로 앞
//   그림과 바이트가 같으면 지운다(그래서 올려 둔 첫 판에도 한 장이 더 생기지 않는다). 바뀐 그림은 예전처럼 날짜 이름으로 남는다(변경 이력).
static QString readPathXattr(const QString &path, const char *name)
{
#ifdef Q_OS_MACOS
    char buf[2048];
    const ssize_t n = getxattr(QFile::encodeName(path).constData(), name, buf, sizeof(buf), 0, 0);
    return n > 0 ? QString::fromUtf8(buf, int(n)) : QString();
#else
    Q_UNUSED(path); Q_UNUSED(name);
    return QString();
#endif
}
static void writePathXattr(const QString &path, const char *name, const QString &value)
{
#ifdef Q_OS_MACOS
    const QByteArray v = value.toUtf8();
    setxattr(QFile::encodeName(path).constData(), name, v.constData(), size_t(v.size()), 0, 0);
#else
    Q_UNUSED(path); Q_UNUSED(name); Q_UNUSED(value);
#endif
}
static bool sameFileBytes(const QString &a, const QString &b)
{
    QFile fa(a), fb(b);
    if (fa.size() != fb.size() || fa.size() <= 0) return false;
    if (!fa.open(QIODevice::ReadOnly) || !fb.open(QIODevice::ReadOnly)) return false;
    while (!fa.atEnd()) {
        if (fa.read(1 << 20) != fb.read(1 << 20)) return false;
    }
    return true;
}
// kind = "profile" · "banner". srcUrl = 기억할 주소(X 가 준 그대로) · candidates = 차례로 받아 볼 주소(원본 먼저).
//   1 = 새 그림을 받았다 · 0 = 받을 것 없음(같은 주소 · 같은 그림 · 오늘 것이 이미 있음) · -1 = 받지 못했다
//   legacyUrl = r56 전 판이 받던 꼴(프로필 사진은 _400x400). 처음 보는 폴더(xattr 없음 · 예전 그림 있음)에서는 먼저 그 꼴로 받아
//   바로 앞 그림과 견준다 — 원본과 400x400 은 서로 다른 인코딩이라 원본끼리만 견주면 바이트가 늘 달라, 올린 첫 판에 모든 사람의
//   그림이 한 장씩 더 생겼다(r56 검토 DC-1). 같으면 그대로 두고 주소만 적는다. 다르면(그림이 정말 바뀌었다) 원본으로 받는다.
static int saveProfileImage(const QString &dir, const QString &kind, const QString &srcUrl, const QStringList &candidates,
                            const QString &dateTag, const std::function<bool(const QString &, const QString &)> &dl,
                            const QString &legacyUrl = QString())
{
    if (srcUrl.isEmpty() || dir.isEmpty()) return 0;
    const QByteArray xa = QByteArrayLiteral("com.hanishiki.") + kind.toLatin1() + QByteArrayLiteral("_url");
    const QStringList olds = QDir(dir).entryList({kind + "_*.jpg"}, QDir::Files, QDir::Name);   // 이름의 날짜 차례 = 시간 차례
    const QString lastUrl = readPathXattr(dir, xa.constData());
    if (!olds.isEmpty() && lastUrl == srcUrl) return 0;
    const QString path = dir + "/" + kind + "_" + dateTag + ".jpg";
    if (QFile::exists(path)) {   // 오늘 이미 받았다(예전 판) — 그 주소로 받은 것으로 본다
        if (lastUrl.isEmpty()) writePathXattr(dir, xa.constData(), srcUrl);
        return 0;
    }
    if (lastUrl.isEmpty() && !olds.isEmpty() && !legacyUrl.isEmpty()) {
        if (dl(legacyUrl, path)) {
            if (sameFileBytes(dir + "/" + olds.last(), path)) {
                QFile::remove(path);
                writePathXattr(dir, xa.constData(), srcUrl);
                return 0;
            }
            QFile::remove(path);   // 그림이 바뀌었다 — 아래에서 원본으로 받는다
        }
    }
    bool ok = false;
    for (const QString &u : candidates) {
        if (!u.isEmpty() && dl(u, path)) { ok = true; break; }
    }
    if (!ok) return -1;
    writePathXattr(dir, xa.constData(), srcUrl);
    for (int i = int(olds.size()) - 1; i >= 0; --i) {
        const QString prev = dir + "/" + olds[i];
        if (prev == path) continue;
        if (sameFileBytes(prev, path)) { QFile::remove(path); return 0; }   // 주소만 달랐지 같은 그림 — 한 장 더 두지 않는다
        break;   // 바로 앞 그림만 본다
    }
    return 1;
}
// r56 전 판이 받던 프로필 사진 꼴(_normal → _400x400) — saveProfileImage 의 legacyUrl
static QString avatarLegacyUrl(const QString &url)
{
    QString big = url;
    big.replace("_normal", "_400x400");
    return big;
}
// 프로필 사진 주소 → 받아 볼 차례(원본 → 400x400 → 받은 그대로) — COV-11
static QStringList avatarCandidates(const QString &url)
{
    if (url.isEmpty()) return {};
    QString big = url;
    big.replace("_normal", "_400x400");
    QStringList c{originalAvatarUrl(url), big, url};
    c.removeDuplicates();
    return c;
}

// ★ HLS(m3u8)만 있는 영상(긴 영상 · 방송 다시보기 일부) — 번들 yt-dlp 로 글 주소에서 받아 mp4 로 묶는다(스페이스와 같은 도구 ·
//   같은 ffmpeg 찾기). 예전엔 mp4 가 없으면 말없이 건너뛰어 그 영상은 영영 받지 않았고 실패로도 세지 않았다(r56, COV-5).
//   숨은 조각 이름으로 받고 다 받은 뒤에만 진짜 이름으로 바꾼다(HttpClient::downloadFile 과 같은 까닭). 자격증명은 넘기지 않는다
//   (명령줄은 ps 로 보인다) — 로그인이 있어야 보이는 영상은 실패로 남고 다음 판에 다시 해 본다.
static bool ytDlpToFile(const QString &url, const QString &filePath, const std::atomic<bool> *running)
{
    const QFileInfo target(filePath);
    const QString dir = target.absolutePath();
    const QString stem = QStringLiteral(".dl_")
        + QString::fromLatin1(QCryptographicHash::hash(target.fileName().toUtf8(), QCryptographicHash::Sha1).toHex().left(20))
        + QStringLiteral(".ytdl");
    auto sweep = [&]() {
        for (const QString &n : QDir(dir).entryList({stem + "*"}, QDir::Files | QDir::Hidden)) QFile::remove(dir + "/" + n);
    };
    sweep();
    QStringList args;
    args << "--no-mtime" << "--newline" << "--no-playlist" << "--no-part" << "--remux-video" << "mp4";
    const QString appDir = QCoreApplication::applicationDirPath();
    for (const QString &ff : QStringList{appDir + "/ffmpeg", "/opt/homebrew/bin/ffmpeg", "/usr/local/bin/ffmpeg", "/usr/bin/ffmpeg"}) {
        if (QFile::exists(ff)) { args << "--ffmpeg-location" << ff; break; }
    }
    args << "-o" << (dir + "/" + stem + ".%(ext)s") << url;
    QProcess proc;
    proc.setProcessEnvironment(Common::bundledProcessEnv());
    proc.setProcessChannelMode(QProcess::MergedChannels);
    proc.start(Common::ytDlpExecutable(), args);
    if (!proc.waitForStarted(10000)) return false;
    QElapsedTimer stall;
    stall.start();
    while (proc.state() != QProcess::NotRunning) {
        if ((running && !running->load()) || Common::appQuitting() || stall.elapsed() > 120000) {   // 중지 · 끄기 · 2분 동안 출력 없음
            proc.terminate();
            if (!proc.waitForFinished(3000)) proc.kill();
            proc.waitForFinished(2000);
            sweep();
            return false;
        }
        if (proc.waitForReadyRead(400)) { proc.readAll(); stall.restart(); }
    }
    const bool ok = proc.exitStatus() == QProcess::NormalExit && proc.exitCode() == 0;
    const QString got = dir + "/" + stem + ".mp4";
    if (!ok || QFileInfo(got).size() <= 0) { sweep(); return false; }
    QFile::remove(filePath);   // 0 바이트 잔재 — HttpClient::downloadFile 의 같은 자리 설명
    if (!QFile::rename(got, filePath)) { sweep(); return false; }
    sweep();
    return true;
}

// ★ 목록 걸음의 API 오류 상한(r56, 깊이 점검 F6) — 좋아요 · 북마크 · 리포스트 · 하이라이트 · 리스트 · 게시물 댓글은 오류가 나면 5초 쉬고
//   끝없이 다시 물었다. 같은 403 이 이어지면 '전체' 가 그 단계에서 영영 멈춰 팔로워 · 팔로잉 · 스페이스 단계가 돌지 않았다. 검색 ·
//   커뮤니티와 같이 5번 이어지면 그 목록을 멈추고(끝까지 못 봤으니 일찍 멈춤 표시는 적지 않는다) 다음 단계로 간다. true = 그만.
static bool listErrorGiveUp(HanishikiBackend *be, int &streak, const QString &what, const std::atomic<bool> &isRunning)
{
    if (++streak >= 5) {
        be->log(QString("API 오류가 5번 이어졌습니다 — %1 수집을 여기서 멈추고 다음 단계로 갑니다(끝까지 보지 못해 일찍 멈춤 표시는 적지 않습니다)").arg(what),
                "error", "twitter");
        return true;
    }
    be->log(QString("API 오류 — 5초 후 재시도 (%1/5)").arg(streak), "warning", "twitter");
    for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
    return false;
}

bool TwitterCollector::downloadMedia(const QString &url, const QString &filePath)
{
    QMap<QString, QString> headers;
    headers["Referer"] = "https://x.com/";
    return m_http->downloadFile(url, filePath, headers);
}

int TwitterCollector::downloadTweetMedia(const QJsonObject &tweet, const QString &mediaDir, int tweetIdx)
{
    // Skip entirely if media download AND exif are both off
    if (!m_downloadMedia && !m_saveExif) return 0;

    int downloaded = 0;
    QJsonObject legacy = xLegacy(tweet);

    // Check if this is a retweet - if so, download media from original tweet
    QJsonObject mediaTweet = tweet;
    QJsonObject mediaLegacy = legacy;
    const QJsonObject rtResult = retweetedOf(tweet);
    if (!rtResult.isEmpty()) {
        mediaTweet = rtResult;
        mediaLegacy = xLegacy(rtResult);
    }

    // Also check quoted tweet
    // ★ 리트윗이면 인용은 원글 아래에 있다(retweeted_status_result.result.quoted_status_result) — 예전엔 바깥 리트윗에서만 찾아,
    //   인용 글을 리트윗한 것의 인용 그림 · 영상을 '트윗' · '미디어' 걸음에서 받지 않았다(r56, COV-7). 원글에 없으면 바깥에서.
    QJsonObject quotedTweet = quotedOf(mediaTweet);
    if (quotedTweet.isEmpty()) quotedTweet = quotedOf(tweet);

    // Helper lambda to download media from a tweet object
    auto downloadFromTweet = [&](const QJsonObject &tw, const QJsonObject &twLegacy) -> int {
        int count = 0;
        QJsonObject entities = twLegacy["extended_entities"].toObject();
        QJsonArray media = entities["media"].toArray();

        if (media.isEmpty()) {
            entities = twLegacy["entities"].toObject();
            media = entities["media"].toArray();
        }
        // ★ 글의 미디어 칸 밖에 올라온 그림도 받는다(r56, COV-8) — 링크 카드의 큰 그림 · 통합 카드(unified_card)의 그림 · 영상 ·
        //   X 글(Articles)의 표지와 본문 그림. 이름 규칙은 같다(글 ID 가 든다). 진짜 미디어 뒤에 붙이고 multiMedia 는 진짜 미디어
        //   수로만 정한다 — 그래야 이미 받아 둔 파일의 이름이 바뀌지 않는다. 이것들을 받지 못한 것은 실패로 세지 않는다(카드
        //   그림은 시간이 지나면 지워지기도 한다 — 그것 때문에 목록의 '끝까지 봄' 이 영영 막히지 않게).
        const int realCount = media.size();
        {
            const QHash<QString, QJsonObject> bind = cardBindings(tw);
            const QString cardImg = cardImageUrl(bind);
            if (!cardImg.isEmpty())
                media.append(QJsonObject{{"type", "photo"}, {"media_url_https", cardImg}, {"_exact", true}});
            const QJsonObject uc = unifiedCardOf(bind);
            for (const auto &mv : uc["media_entities"].toObject()) {
                QJsonObject mo = mv.toObject();
                if (mo["type"].toString().isEmpty()) continue;
                media.append(mo);
            }
            const QJsonObject art = articleOf(tw);
            QStringList artImgs;
            const QString cover = art["cover_media"].toObject()["media_info"].toObject()["original_img_url"].toString();
            if (!cover.isEmpty()) artImgs << cover;
            for (const auto &me : art["media_entities"].toArray()) {
                const QString u = me.toObject()["media_info"].toObject()["original_img_url"].toString();
                if (!u.isEmpty() && !artImgs.contains(u)) artImgs << u;
            }
            for (const QString &u : std::as_const(artImgs))
                media.append(QJsonObject{{"type", "photo"}, {"media_url_https", u}});
        }

        // Author username subfolder
        QJsonObject userResult = tw["core"].toObject()["user_results"].toObject()["result"].toObject();
        QString authorUsername = screenNameOf(userResult);
        QString authorMediaDir = mediaDir;
        if (!authorUsername.isEmpty()) {
            // 아이디도 대소문자를 바꿀 수 있다 — 작성자 폴더가 둘로 갈리지 않게
            authorMediaDir = mediaDir + "/"+ FileHelper::reuseExistingName(mediaDir, authorUsername);
            QDir().mkpath(authorMediaDir);
        }

        for (int i = 0; i < media.size(); ++i) {
            QJsonObject mediaObj = media[i].toObject();
            QString mediaType = mediaObj["type"].toString();
            QString mediaUrl;
            QString ext;
            const bool isExtra = i >= realCount;   // 카드 · 글(Articles) 그림 — 위 설명
            bool hlsOnly = false;                  // mp4 없이 m3u8 만 있는 영상
            QString hlsUrl;

            if (mediaType == "photo") {
                mediaUrl = mediaObj["media_url_https"].toString();
                if (!mediaUrl.isEmpty() && mediaObj["_exact"].toBool()) {
                    // 카드 그림(card_img/…?format=jpg&name=orig) — 경로에 확장자가 없어 주소를 그대로 쓴다(꼬리를 떼면 받을 수 없다)
                    const QString fmt = QUrlQuery(QUrl(mediaUrl)).queryItemValue(QStringLiteral("format")).toLower();
                    ext = (fmt == QLatin1String("png") || mediaUrl.split('?').first().toLower().endsWith(".png")) ? ".png" : ".jpg";
                } else if (!mediaUrl.isEmpty()) {
                    // 쿼리 파라미터 제거 후 확장자 판별
                    QString baseUrl = mediaUrl.split('?').first();
                    if (baseUrl.toLower().endsWith(".png")) {
                        ext = ".png";
                    } else {
                        ext = ".jpg";
                    }
                    // ?name=orig만 사용 — format 파라미터 없이 원본 그대로 다운 (품질 열화 방지)
                    mediaUrl = baseUrl + "?name=orig";
                }
            } else if (mediaType == "video"|| mediaType == "animated_gif") {
                mediaUrl = bestMp4Url(mediaObj, &hlsOnly, &hlsUrl);
                ext = ".mp4";
                if (isExtra) hlsOnly = false;   // 통합 카드의 영상은 글 주소로 고를 수 없다 — mp4 가 없으면 건너뛴다
            }

            // HLS 만 있는 영상은 이름(미디어 키)을 m3u8 주소에서 짓는다 — 판마다 같은 이름이 되게
            const QString keyUrl = hlsOnly ? hlsUrl : mediaUrl;
            if (keyUrl.isEmpty()) continue;

            QString tweetId = twLegacy["id_str"].toString();
            if (tweetId.isEmpty()) tweetId = tw["rest_id"].toString();

            // 미디어 URL에서 media key 추출 (pbs.twimg.com/media/Gt2UTcbWsAAedIz.jpg → Gt2UTcbWsAAedIz)
            QString mediaKey;
            {
                QString urlPath = QUrl(keyUrl.split('?').first()).path();  // /media/Gt2UTcbWsAAedIz.jpg
                QString baseName = urlPath.mid(urlPath.lastIndexOf('/') + 1);
                int dotPos = baseName.lastIndexOf('.');
                if (dotPos > 0) baseName = baseName.left(dotPos);
                if (!baseName.isEmpty() && baseName != tweetId)
                    mediaKey = baseName;
            }

            // 파일명: {clean_text}-{tweet_id} ({media_key}).ext
            QString tweetText = twLegacy["full_text"].toString();
            if (tweetText.isEmpty()) tweetText = twLegacy["text"].toString();
            // sanitize: URL 제거, 제어문자 제거, 파일시스템 금지문자 제거
            tweetText.remove(QRegularExpression("https?://\\S+"));
            tweetText.remove(QRegularExpression("[\\x00-\\x1f\\x7f-\\x9f]"));
            tweetText.replace(QRegularExpression("[\\n\\r]"), "");
            tweetText.remove(QRegularExpression("[<>:\"/\\\\|?*]"));
            tweetText.replace(QRegularExpression("\\s+"), "");
            // ★ NFC 로 모은다. HFS+(맥 옛 디스크)는 파일명을 NFD 로 강제하므로,
            //   맞춰 두지 않으면 같은 트윗인데 맥과 윈도우의 파일명 바이트가 달라진다.
            //   (길이·서로게이트는 아래에서 이미 다룬다 — 윈도우판 4b87220 과 같은 자리)
            tweetText = tweetText.normalized(QString::NormalizationForm_C);
            tweetText = tweetText.trimmed();
            if (tweetText.startsWith('.')) tweetText = tweetText.mid(1).trimmed();
            // ★ 글자 중간에서 자르지 않는다. 이모지는 UTF-16 두 칸(서로게이트 쌍)이라
            //   left(100) 이 그 사이를 지나면 짝 잃은 반쪽이 남는다. 그 반쪽은 UTF-8 로
            //   인코딩할 수 없다 — 윈도우(NTFS, UTF-16)는 그대로 저장하지만 맥(APFS, UTF-8)은
            //   못 써서 대체문자로 바뀐다. 같은 트윗인데 두 판의 파일명이 달라진다.
            //   (실측: 이 기계 보관함 18567개 중 8개가 그 상태였다)
            if (tweetText.length() > 100) {
                tweetText = tweetText.left(100);
                if (!tweetText.isEmpty() && tweetText.back().isHighSurrogate()) tweetText.chop(1);
                tweetText = tweetText.trimmed();
            }

            // Set file time to tweet date (always, even if already downloaded)
            QString createdAt = twLegacy["created_at"].toString();
            if (createdAt.isEmpty()) {
                // Fallback: try parent tweet's created_at
                createdAt = xLegacy(tw)["created_at"].toString();
            }

            // 업로드 시각 prefix (OS 정렬 시 업로드 순 배치)
            QString orderPrefix;
            if (!createdAt.isEmpty()) {
                // Twitter format: "Mon Mar 25 14:58:00 +0000 2024"
QDateTime twDt = QDateTime::fromString(createdAt, "ddd MMM dd HH:mm:ss +0000 yyyy");
                if (twDt.isValid()) {
                    twDt.setTimeZone(QTimeZone::utc());   // 받은 칸은 UTC 다(+0000) — Qt 6.12 에서 setTimeSpec 은 낡은 것
                    orderPrefix = twDt.toUTC().addSecs(9 * 3600).toString("yyyyMMdd_HHmm_");
                }
            }

            // 파일명 형식: {prefix}{text}-{tweetId} ({mediaKey}).ext
            QString mediaKeySuffix = mediaKey.isEmpty() ? "": QString("(%1)").arg(mediaKey);
            bool multiMedia = (realCount > 1);   // 진짜 미디어 수로만 — 위 설명(덧붙인 카드 그림이 옛 파일 이름을 바꾸지 않게)
            auto composeName = [&](const QString &text) -> QString {
                if (text.isEmpty())
                    return QString("%1%2-%3%4%5").arg(orderPrefix).arg(tweetId).arg(i).arg(mediaKeySuffix, ext);
                if (multiMedia || i > 0 || isExtra)  // 복수 미디어: {prefix}{text}-{id}-{idx}({mediaKey}).ext
                    return QString("%1%2-%3-%4%5%6").arg(orderPrefix, text, tweetId).arg(i).arg(mediaKeySuffix, ext);
                return QString("%1%2-%3%4%5").arg(orderPrefix, text, tweetId, mediaKeySuffix, ext);  // 단일 미디어
            };
            QString filename = composeName(tweetText);

            // ★ 파일명 한 칸의 한계는 '글자 수' 가 아니라 'UTF-8 255바이트' 다 —
            //   맥(APFS)·리눅스(ext4)·삼바 공유가 전부 그렇다. 윈도우(NTFS)만 UTF-16
            //   255칸이라 일본어·한글이 두 배 넉넉하다. 그래서 위의 100'자' 제한만으로는
            //   일본어 100자 = 300바이트가 되어, 같은 트윗을 받아도 윈도우에만 파일이
            //   생기고 맥·NAS 에서는 생성 자체가 실패한다.
            //   (실측: 이 기계 보관함 18567개 중 340개가 255바이트 초과. 최대 356)
            //   줄이는 것은 앞의 본문뿐이다. 뒤쪽(트윗 ID·미디어 키·확장자)은 파일을
            //   식별하는 부분이라 절대 건드리지 않는다.
            {
                constexpr int kMaxNameBytes = 255;
                QString shortText = tweetText;
                while (filename.toUtf8().size() > kMaxNameBytes && !shortText.isEmpty()) {
                    shortText.chop(1);
                    if (!shortText.isEmpty() && shortText.back().isHighSurrogate()) shortText.chop(1);
                    filename = composeName(shortText.trimmed());
                }
            }
            QString filepath = authorMediaDir + "/"+ filename;

            if (QFile::exists(filepath)) {
                // Already downloaded — still fix timestamp + EXIF if needed
                // ★ 이미 다 손본 파일(시각이 글의 시각 · EXIF 표시가 있음)은 exiftool 도 시각 맞추기도 하지 않는다(r56 — 위 mediaAlreadyFinished)
                const bool wantsExif = (mediaType == "photo" && m_saveExif);
                if (!mediaAlreadyFinished(filepath, createdAt, wantsExif)) {
                    if (wantsExif && addExifMetadata(filepath, tw) == 1) markExifDone(filepath);
                    // setFileTimes를 EXIF 후에 (exiftool이 mtime 변경하므로)
                    if (!createdAt.isEmpty()) {
                        Common::setFileTimes(filepath, createdAt);
                    }
                }
                count++; continue;
            }

            // Skip download if media download is disabled
            if (!m_downloadMedia) continue;

            bool dlOk = false;
            if (hlsOnly) {
                QString sn = screenNameOf(tw["core"].toObject()["user_results"].toObject()["result"].toObject());
                const QString postUrl = QString("https://x.com/%1/status/%2").arg(sn.isEmpty() ? QStringLiteral("i") : sn, tweetId)
                                        // ★ /video/N 의 N 은 '몇 번째 영상' 이 아니라 미디어 칸의 자리다(yt-dlp 가 extended_entities.media[N-1] 을 고른다) —
                                        //   영상 수로 세면 그림 + 영상이 섞인 글에서 그림을 골라 늘 실패했다(검토 B-2). i 는 같은 목록의 자리, HLS 는 i < realCount 에서만.
                                        + (realCount > 1 ? QString("/video/%1").arg(i + 1) : QString());
                m_backend->log(QString("영상에 mp4 가 없고 HLS(m3u8) 만 있습니다 — yt-dlp 로 받아 봅니다: %1").arg(postUrl), "warning", "twitter");
                dlOk = ytDlpToFile(postUrl, filepath, m_runFlag);
                if (!dlOk && (!m_runFlag || m_runFlag->load()))
                    m_backend->log(QString("HLS 영상을 받지 못했습니다(%1) — 받지 못한 미디어로 남겨 다음 판에 다시 해 봅니다").arg(postUrl), "warning", "twitter");
            } else {
                dlOk = downloadMedia(mediaUrl, filepath);
            }
            if (!dlOk && !isExtra) ++m_mediaFailures;   // 받지 못했다 — 그 목록을 '끊김 없이 다 봄' 으로 적지 않는다
            if (dlOk) {
                count++;

                // Add EXIF metadata (for photos) — configurable
                // EXIF를 먼저! (exiftool이 파일 수정 → mtime 변경되므로)
                if (mediaType == "photo"&& m_saveExif) {
                    if (addExifMetadata(filepath, tw) == 1) markExifDone(filepath);   // 다음 판이 다시 적지 않게(위 설명)
                }

                // Add Finder comment with tweet URL
                QJsonObject userResult = tw["core"].toObject()["user_results"].toObject()["result"].toObject();
                QString screenName = screenNameOf(userResult);
                if (screenName.isEmpty()) screenName = userResult["rest_id"].toString();
                QString tweetUrl = QString("https://x.com/%1/status/%2").arg(
                    screenName.isEmpty() ? "i": screenName, tweetId);
                FileHelper::setFinderComment(filepath, tweetUrl);
                // xattr URL 메타데이터 (macOS kMDItemWhereFroms)
                FileHelper::setDownloadMeta(filepath, tweetUrl);

                // 파일 시간 설정 — 맨 마지막에! (EXIF/Finder 수정 후)
                if (!createdAt.isEmpty()) {
                    Common::setFileTimes(filepath, createdAt);
                } else {
                    qWarning() << "[Twitter] created_at empty for tweet:"<< tweetId;
                }

                // _complete 미러 폴더에 복사
                QDir mediaParent(mediaDir);
                mediaParent.cdUp(); // type 폴더의 상위 → media/ 루트
                QString completeDir = mediaParent.absolutePath() + "/_complete";
                QDir().mkpath(completeDir);
                QString completePath = completeDir + "/"+ QFileInfo(filepath).fileName();
                if (!QFile::exists(completePath)) {
                    QFile::copy(filepath, completePath);
                    if (!createdAt.isEmpty()) Common::setFileTimes(completePath, createdAt);
                }
            }
        }
        return count;
    };

    // Download media from main/retweeted tweet
    downloaded += downloadFromTweet(mediaTweet, mediaLegacy);

    // Also download media from quoted tweet if present
    if (!quotedTweet.isEmpty()) {
        downloaded += downloadFromTweet(quotedTweet, xLegacy(quotedTweet));
    }

    return downloaded;
}

void TwitterCollector::downloadUserProfileMedia(const QJsonObject &tweet, const QString &profileDir, const QString &category)
{
    // Extract all unique users from this tweet (author, RT'd user, quoted user)
    auto extractUser = [](const QJsonObject &tw) -> QJsonObject {
        return tw["core"].toObject()["user_results"].toObject()["result"].toObject();
    };

    QList<QJsonObject> users;
    users.append(extractUser(tweet));

    // RT'd user
    QJsonObject legacy = xLegacy(tweet);
    QJsonObject rtResult;
    if (legacy.contains("retweeted_status_result"))
        rtResult = legacy["retweeted_status_result"].toObject()["result"].toObject();
    if (rtResult.isEmpty() && tweet.contains("retweeted_status_result"))
        rtResult = tweet["retweeted_status_result"].toObject()["result"].toObject();
    if (!rtResult.isEmpty()) {
        if (rtResult.contains("tweet")) rtResult = rtResult["tweet"].toObject();
        users.append(extractUser(rtResult));
    }

    // Quoted user — 리트윗이면 인용은 원글 아래에 있다(r56 검토 — 바깥 리트윗에서만 찾아 인용한 사람의 프로필이 빠졌다)
    QJsonObject quotedTweet = rtResult.isEmpty() ? QJsonObject() : quotedOf(rtResult);
    if (quotedTweet.isEmpty()) quotedTweet = quotedOf(tweet);
    if (!quotedTweet.isEmpty())
        users.append(extractUser(quotedTweet));

    // 카테고리 디렉토리 (유저별 서브폴더는 아래 루프에서 생성)
    QString categoryDir = profileDir + "/"+ category;

    for (const auto &userObj : users) {
        if (userObj.isEmpty()) continue;

        // ── Twitter API 2025+ 구조: screen_name, name, profile_image_url_https가 legacy에서 제거됨 ──
        // 새 구조: userObj = { rest_id, avatar, core: {screen_name, name, ...}, legacy: {stats만...} }
        QJsonObject uLeg = xUserLegacy(userObj);
        QJsonObject uCore = userObj["core"].toObject();

        // screen_name 추출 (여러 위치 시도)
        QString screenName = uLeg["screen_name"].toString();
        if (screenName.isEmpty()) screenName = uCore["screen_name"].toString();
        if (screenName.isEmpty()) screenName = userObj["screen_name"].toString();
        // core.user_results.result.legacy (중첩 구조)
        if (screenName.isEmpty()) {
            QJsonObject innerResult = xUserResult(uCore);
            screenName = xLegacy(innerResult)["screen_name"].toString();
            if (screenName.isEmpty()) screenName = innerResult["screen_name"].toString();
        }
        // 최후 수단: rest_id
        if (screenName.isEmpty()) screenName = userObj["rest_id"].toString();

        if (screenName.isEmpty()) continue;  // 완전히 비어있으면 스킵

        // name(표시이름) 추출
        QString displayName = uLeg["name"].toString();
        if (displayName.isEmpty()) displayName = uCore["name"].toString();
        if (displayName.isEmpty()) displayName = userObj["name"].toString();

        // profile_image_url 추출
        QString profileImageUrl = uLeg["profile_image_url_https"].toString();
        if (profileImageUrl.isEmpty()) profileImageUrl = uCore["profile_image_url_https"].toString();
        if (profileImageUrl.isEmpty()) profileImageUrl = userObj["profile_image_url_https"].toString();
        // avatar 필드 (새 API 구조)
        if (profileImageUrl.isEmpty()) {
            QJsonValue avatarVal = userObj["avatar"];
            if (avatarVal.isObject()) {
                QJsonObject av = avatarVal.toObject();
                profileImageUrl = av["image_url"].toString();
                if (profileImageUrl.isEmpty()) profileImageUrl = av["url"].toString();
            } else if (avatarVal.isString()) {
                profileImageUrl = avatarVal.toString();
            }
        }
        if (m_downloadedProfiles.contains(screenName)) continue;
        m_downloadedProfiles.insert(screenName);

        // 파일명에 표시이름 포함: "이름(@handle)_profile.jpg"// (displayName은 이미 위에서 추출됨)
        displayName.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
        displayName = displayName.trimmed().left(40);
        QString filePrefix;
        if (!displayName.isEmpty()) {
            filePrefix = QString("%1(@%2)").arg(displayName, screenName);
        } else {
            filePrefix = screenName;
        }
        // 어느 파일시스템에 저장해도 같은 이름이 되게 (FileHelper.h 설명)
        filePrefix = FileHelper::portableName(filePrefix);

        // 유저별 서브폴더: profiles/tweets/@handle/ or profiles/tweets/이름(@handle)/
        QString userDirName = filePrefix;
        userDirName.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
        // 표시이름 대소문자만 바뀐 계정이 폴더를 하나 더 만들지 않게 — FileHelper.h 설명
        userDirName = FileHelper::reuseExistingName(categoryDir, userDirName);
        QString userProfileDir = categoryDir + "/"+ userDirName;
        QDir().mkpath(userProfileDir);

        // 날짜 접미사 (덮어쓰기 방지 — 프로필/배너 변경 이력 보존)
        QString dateTag = Common::jstNow().toString("yyyyMMdd");

        // Profile image — 원본 먼저, 안 되면 400x400(COV-11). 주소가 바뀌었을 때만 날짜 이름으로 새로 받는다(saveProfileImage 설명)
        const auto dl = [this](const QString &u, const QString &f) { return downloadMedia(u, f); };
        if (!profileImageUrl.isEmpty()) {
            if (saveProfileImage(userProfileDir, QStringLiteral("profile"), profileImageUrl, avatarCandidates(profileImageUrl), dateTag, dl, avatarLegacyUrl(profileImageUrl)) > 0)
                m_backend->log(QString("프로필: @%1 → %2").arg(screenName, category), "info", "twitter");
        } else {
            m_backend->log(QString("@%1 프로필 이미지 URL 없음").arg(screenName), "warning", "twitter");
        }

        // Banner (1500x500) — 새 API 대응
        QString bannerUrl = uLeg["profile_banner_url"].toString();
        if (bannerUrl.isEmpty()) bannerUrl = uCore["profile_banner_url"].toString();
        if (bannerUrl.isEmpty()) bannerUrl = userObj["profile_banner_url"].toString();
        if (!bannerUrl.isEmpty()) {
            if (saveProfileImage(userProfileDir, QStringLiteral("banner"), bannerUrl, {bannerUrl + "/1500x500", bannerUrl}, dateTag, dl) > 0)
                m_backend->log(QString("배너: @%1 → %2").arg(screenName, category), "info", "twitter");
        }

        // Collect profile data for Excel export — 새 API 대응
        QJsonObject profileInfo;
        profileInfo["screen_name"] = screenName;
        profileInfo["name"] = displayName;
        QString desc = uLeg["description"].toString();
        if (desc.isEmpty()) desc = uCore["description"].toString();
        profileInfo["description"] = desc;
        profileInfo["followers_count"] = uLeg["followers_count"].toInt();
        profileInfo["friends_count"] = uLeg["friends_count"].toInt();
        profileInfo["statuses_count"] = uLeg["statuses_count"].toInt();
        profileInfo["location"] = uLeg["location"].toString();
        profileInfo["url"] = uLeg["url"].toString();
        profileInfo["created_at"] = Common::formatDateJapanese(uLeg["created_at"].toString());
        profileInfo["verified"] = userObj["is_blue_verified"].toBool() ? "True": "False";
        profileInfo["profile_image_url"] = profileImageUrl.isEmpty() ? QString() : QString(profileImageUrl).replace("_normal", "_400x400");
        profileInfo["profile_banner_url"] = bannerUrl;
        if (m_profileBuffer) m_profileBuffer->append(profileInfo);
    }
}

int TwitterCollector::addExifMetadata(const QString &imagePath, const QJsonObject &tweet)
{
    QJsonObject legacy = xLegacy(tweet);
    QString screenName = screenNameOf(tweet["core"].toObject()["user_results"]
                                          .toObject()["result"].toObject());
    QString text = legacy["full_text"].toString().left(200);
    QString tweetId = legacy["id_str"].toString();
    if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();
    QString tweetUrl = QString("https://x.com/%1/status/%2").arg(screenName, tweetId);
    QString createdAt = legacy["created_at"].toString();

    // Common::addExifMetadata가 번들 exiftool 경로를 올바르게 탐색(1 = 적었다 — 그때만 '다 적음' 표시를 단다)
    return Common::addExifMetadata(imagePath,
        screenName.isEmpty() ? "": "@"+ screenName,
        text,
        screenName.isEmpty() ? "": "Twitter @"+ screenName,
        tweetUrl,
        createdAt);
}

void TwitterCollector::setFinderComment(const QString &filePath, const QString &comment)
{
    FileHelper::setFinderComment(filePath, comment);
}

void TwitterCollector::saveExcel(const QString &saveDir, const QString &target, const QJsonArray &data, const QString &suffix)
{
    ExcelWriter writer;

    QStringList headers = {"ID", "Text", "URL", "Date", "Author",
                           "RT", "Likes", "Replies", "Quotes", "Views",
                           "Lang", "Source", "Reply To", "Media", "Sensitive"};
    writer.writeHeader(headers, QColor("#d97757"));

    int row = 2;
    for (const auto &val : data) {
        QJsonObject tweet = val.toObject();
        QString viewCount = tweet["view_count"].toString();

        writer.writeRow(row++, {
            tweet["id"].toString(),
            tweet["text"].toString().left(500),
            tweet["url"].toString(),
            Common::formatDateJapanese(tweet["created_at"].toString()),
            tweet["author"].toString(),
            QString::number(tweet["retweet_count"].toInt()),
            QString::number(tweet["favorite_count"].toInt()),
            QString::number(tweet["reply_count"].toInt()),
            QString::number(tweet["quote_count"].toInt()),
            viewCount.isEmpty() ? "": viewCount,
            tweet["lang"].toString(),
            tweet["source"].toString(),
            tweet["in_reply_to"].toString(),
            tweet["media_types"].toString(),
            tweet["sensitive"].toString()
        });
    }

    writer.setColumnWidth(1, 20);
    writer.setColumnWidth(2, 50);
    writer.setColumnWidth(3, 40);
    writer.setColumnWidth(4, 28);
    writer.setColumnWidth(5, 15);
    writer.setColumnWidth(6, 8);
    writer.setColumnWidth(7, 8);
    writer.setColumnWidth(8, 8);
    writer.setColumnWidth(9, 8);
    writer.setColumnWidth(10, 12);
    writer.setColumnWidth(11, 6);
    writer.setColumnWidth(12, 20);
    writer.setColumnWidth(13, 15);
    writer.setColumnWidth(14, 15);
    writer.setColumnWidth(15, 8);

    QString filepath = saveDir + "/"+ target + "_"+ suffix + ".xlsx";
    if (!writer.save(filepath)) return;      // 실패는 저장 쪽이 기록에 적었다 — '저장' 이라 적지 않는다
    FileHelper::setDownloadMeta(filepath, QStringLiteral(APP_NAME_DISPLAY) + QStringLiteral(" Twitter"));
    m_backend->log("저장: "+ filepath, "success", "twitter");
}

// 중간 저장을 할 때인가 — 판의 첫 중간 저장은 곧바로, 그 뒤로는 지난 저장에 걸린 시간의 다섯 배(적어도 60초)가 지났을 때만.
//   판의 끝 · 이어받기 표시를 옮기기 전의 저장은 이것을 보지 않는다(엑셀보다 표시가 앞서지 않게).
bool TwitterCollector::excelCheckpointDue() const
{
    // 단조 시계로 잰다 — 시계를 뒤로 돌려도(수동 · NTP) 저장이 그만큼 멎지 않게(검토)
    if (!m_xlSinceSave.isValid()) return true;
    return m_xlSinceSave.elapsed() >= qMax<qint64>(60000, 5 * m_xlLastSaveCostMs);
}

int TwitterCollector::saveExcelStreaming(const QString &saveDir, const QString &target, DiskJsonBuffer &buffer, const QString &suffix)
{
    // 아니포 호환 파일명: _complete.xlsx (tweets일 때)
    QString fileSuffix = suffix;
    if (fileSuffix == "tweets") fileSuffix = "complete";

    QString filepath = saveDir + "/"+ target + "_"+ fileSuffix + ".xlsx";

    // ── Step 1: 기존 Excel 읽기 — 한 번만. 이번 판에 우리가 마지막으로 쓴 그 파일이 그대로면(경로 · 크기 · 시각) 다시 읽지 않고
    //   기억해 둔 줄을 쓴다. 예전엔 저장마다 ID 를 읽느라 한 번, 다시 쓰느라 한 번, 파일 전체를 두 번 열었다(장기 지원 점검).
    QElapsedTimer xlTimer;
    xlTimer.start();
    QSet<QString> existingIds;
    QList<QStringList> existingRows;
    {
        const QFileInfo xfi(filepath);
        const bool cacheHit = xfi.exists() && filepath == m_xlCachePath && xfi.size() == m_xlCacheSize
                              && xfi.lastModified().toMSecsSinceEpoch() == m_xlCacheMtimeMs;
        if (cacheHit) {
            existingRows = m_xlCacheRows;   // 암묵적 공유 — 고칠 때만 복사된다
        } else if (xfi.exists()) {
            QXlsx::Document existingDoc(filepath);
            // ★ 있는 파일을 읽지 못했으면(잠깐의 입출력 오류 · 깨진 파일) 덮어쓰지 않는다 — QXlsx 는 실패해도 빈 시트를 주어,
            //   그대로 쓰면 이번 판의 줄만 남고 그 대상의 기록을 잃었다(검토). 줄은 버퍼에 그대로 두어 다음 저장이 다시 해 본다.
            if (!existingDoc.isLoadPackage()) {
                m_excelSaveFailed = true;
                m_xlLastSaveCostMs = xlTimer.elapsed();
                m_xlSinceSave.start();
                m_backend->log(QString("엑셀을 읽지 못해 덮어쓰지 않습니다: %1 — 파일이 깨졌으면 다른 이름으로 옮겨 두고 다시 받으십시오"
                                       "(이어받기 · 일찍 멈춤 표시는 옮기지 않습니다)").arg(filepath), "error", "twitter");
                return 0;
            }
            const int existingLastRow = existingDoc.dimension().lastRow();
            int lastCol = existingDoc.dimension().lastColumn();
            if (lastCol < 21) lastCol = 21;
            const int nCols = int(tweetColumns().size());
            for (int r = 2; r <= existingLastRow; ++r) {
                QStringList rowData;
                rowData.reserve(qMax(lastCol, nCols));
                for (int c = 1; c <= lastCol; ++c) rowData << existingDoc.read(r, c).toString();
                // 지금 칸 수로 패딩 (아니포 21컬럼 · r55 까지의 30컬럼 호환 — 새 칸은 빈칸으로 두었다가 다시 받으면 채운다)
                while (rowData.size() < nCols) rowData << "";
                // 구분선/빈 행 건너뜀
                if (rowData[0].startsWith("─") || rowData[0].startsWith("===")) continue;
                if (rowData[0].trimmed().isEmpty()) continue;
                existingRows.append(rowData);
            }
            // 읽은 그대로를 기억한다 — 이번에 쓸 것이 없어 건너뛰어도 다음 저장이 다시 읽지 않게(아래 빈칸 채우기는 사본을 고친다)
            if (existingRows.size() <= 150000) {
                m_xlCachePath = filepath;
                m_xlCacheSize = xfi.size();
                m_xlCacheMtimeMs = xfi.lastModified().toMSecsSinceEpoch();
                m_xlCacheRows = existingRows;
            }
        }
        for (const QStringList &r : std::as_const(existingRows)) existingIds.insert(r[0].trimmed());
        if (!cacheHit && !existingIds.isEmpty())
            m_backend->log(QString("기존 파일: %1개 ID 확인").arg(existingIds.size()), "info", "twitter");
    }

    // ── Step 2: 버퍼에서 새 데이터만 추출 (중복 제거) ──
    QList<QStringList> newRows;
    // ★ 엑셀에 이미 있는 ID 를 다시 받았으면 그 줄의 빈칸만 채운다(차 있는 칸은 그대로). 예전 좋아요 · 북마크 줄은 다른 이름으로
    //   담겨 주소 · 작성자 · 미디어 칸이 비어 있었다 — 다시 볼 때 메운다(사용자 2026-10-29 '엑셀에 빠진 항목')
    QHash<QString, QStringList> refill;
    buffer.resetReader();
    QJsonObject tweet;
    while (buffer.readNext(tweet)) {
        QString id = tweet["id"].toString();
        if (id.isEmpty()) continue;
        const bool already = existingIds.contains(id);
        if (already && refill.contains(id)) continue;
        if (!already) existingIds.insert(id);

        const QStringList rowOut = tweetRowCells(tweet);   // tweetColumns 차례(새 칸은 끝에)
        if (already) refill.insert(id, rowOut);
        else newRows.append(rowOut);
    }

    // 버퍼는 다 쓴 뒤에야 비운다 — 쓰다 실패하면 그대로 두어 다음 저장이 그 줄을 다시 넣는다(예전엔 쓰기 전에 비워
    //   저장에 실패하면 지난 저장 이후의 줄을 잃었다 — 검토). 쓸 것이 없으면 여기서 비운다.
    if (newRows.isEmpty() && refill.isEmpty()) {
        buffer.clear();
        m_backend->log("새 데이터 없음, 저장 건너뜀", "info", "twitter");
        return 0;
    }

    // ── Step 3: 기존 Excel에 append (전체 재작성 X) ──
    const QStringList headers = tweetColumns();

    // ── Step 3: 새 파일로 재작성 (QXlsx append는 openpyxl 파일 깨뜨림) — 기존 줄은 Step 1 에서 이미 읽었다 ──
    // 있던 줄의 빈칸 채우기 — 다시 받은 줄에서. 그래도 비는 글 주소는 ID 로(x.com/i/status/<ID> 도 그 글로 열린다)
    bool refilled = false;
    static const QRegularExpression reNumId(QStringLiteral("^\\d{5,25}$"));
    // ★ 📌 는 처음 적힌 때에 굳어 있었다(r56 검토 DC-2) — 고정한 글은 대개 오래전에 받은 글이라, 받아 둔 엑셀에선 그 줄을 다시 쓰지 않아
    //   📌 가 끝내 붙지 않았고, 고정을 바꿔도 옛 글의 📌 가 남았다. 대상 자신의 글 파일(_complete · _media · _narrow)에서만,
    //   사용자 조회가 고정 칸을 실제로 읽었을 때만, 종류 칸(5번째)의 머리를 지금 고정한 글에 맞춘다(좋아요 · 북마크는 건드리지 않는다).
    const QString pinHead = QStringLiteral("\U0001F4CC ");
    const bool pinTrack = m_pinnedKnown && (suffix == QLatin1String("tweets") || suffix == QLatin1String("media")
                                            || suffix == QLatin1String("narrow"));
    for (QStringList &r : existingRows) {
        const QString id = r[0].trimmed();
        if (pinTrack && r.size() > 4) {
            const bool want = m_pinnedIds.contains(id), has = r[4].startsWith(pinHead);
            if (want && !has) { r[4] = pinHead + r[4]; refilled = true; }
            else if (!want && has) { r[4] = r[4].mid(pinHead.size()); refilled = true; }
        }
        const auto it = refill.constFind(id);
        if (it != refill.constEnd()) {
            for (int c = 1; c < it->size() && c < r.size(); ++c) {
                // 옛 좋아요 · 북마크 줄의 bookmark_count(7번 칸)는 값이 없어도 빈칸이 아니라 0 으로 적혀 있다 — 그것도 빈칸으로 본다
                const bool blank = r[c].trimmed().isEmpty() || (c == 7 && r[c].trimmed() == QLatin1String("0"));
                if (blank && !it->at(c).trimmed().isEmpty() && r[c] != it->at(c)) { r[c] = it->at(c); refilled = true; }
            }
        }
        if (r[1].trimmed().isEmpty() && reNumId.match(id).hasMatch()) {
            r[1] = QStringLiteral("https://x.com/i/status/") + id;
            refilled = true;
        }
    }
    if (newRows.isEmpty() && !refilled) {
        buffer.clear();
        m_backend->log("새 데이터 없음, 저장 건너뜀", "info", "twitter");
        return 0;
    }

    // ── Step 4: 새/기존 데이터 병합 후 created_at 기준 최신순 정렬 ──
    //  (id 기준 중복 제거 — 동일 트윗이면 새 데이터 우선)
    QSet<QString> seenIds;
    QList<QStringList> mergedRows;
    auto pushUnique = [&](const QList<QStringList> &src) {
        for (const auto &r : src) {
            QString id = r.size() > 0 ? r[0] : "";
            if (id.isEmpty() || seenIds.contains(id)) continue;
            seenIds.insert(id);
            mergedRows.append(r);
        }
    };
    pushUnique(newRows);       // 새 데이터 우선
    pushUnique(existingRows);  // 기존 데이터는 중복 아닌 것만

    std::sort(mergedRows.begin(), mergedRows.end(), [](const QStringList &a, const QStringList &b) {
        // created_at 은 인덱스 14 (15번째 컬럼)
        QString dateA = a.size() > 14 ? a[14] : "";
        QString dateB = b.size() > 14 ? b[14] : "";
        return dateA > dateB;  // 최신순 (내림차순)
    });

    // ── Step 5: 새 Excel 파일 생성 (항상 깨끗한 파일) ──
    ExcelWriter writer;
    writer.writeHeader(headers, QColor("#0070C0"));

    int row = 2;
    for (const auto &rowData : mergedRows) {
        writer.writeRow(row++, rowData);
    }

    writer.setColumnWidth(1, 20);   // id
    writer.setColumnWidth(2, 40);   // tweet_url
    writer.setColumnWidth(3, 50);   // text
    writer.setColumnWidth(4, 8);    // language
    writer.setColumnWidth(5, 10);   // type
    writer.setColumnWidth(6, 15);   // author_name
    writer.setColumnWidth(7, 15);   // author_username
    writer.setColumnWidth(8, 8);    // bookmark_count
    writer.setColumnWidth(9, 8);    // favorite_count
    writer.setColumnWidth(10, 8);   // retweet_count
    writer.setColumnWidth(11, 8);   // retweeted
    writer.setColumnWidth(12, 8);   // reply_count
    writer.setColumnWidth(13, 8);   // quote_count
    writer.setColumnWidth(14, 10);  // view_count
    writer.setColumnWidth(15, 18);  // created_at
    writer.setColumnWidth(16, 18);  // retweet_time
    writer.setColumnWidth(17, 20);  // source
    writer.setColumnWidth(18, 20);  // hashtags
    writer.setColumnWidth(19, 30);  // urls
    writer.setColumnWidth(20, 10);  // media_type
    writer.setColumnWidth(21, 40);  // media_urls
    writer.setColumnWidth(22, 20);  // conversation_id
    writer.setColumnWidth(23, 25);  // in_reply_to
    writer.setColumnWidth(24, 8);   // is_quote
    writer.setColumnWidth(25, 40);  // quoted_tweet_url
    writer.setColumnWidth(26, 8);   // possibly_sensitive
    writer.setColumnWidth(27, 10);  // user_followers
    writer.setColumnWidth(28, 10);  // user_following
    writer.setColumnWidth(29, 8);   // user_verified
    writer.setColumnWidth(30, 40);  // user_profile_image
    writer.setColumnWidth(31, 40);  // alt_text
    writer.setColumnWidth(32, 30);  // card_title
    writer.setColumnWidth(33, 40);  // card_url
    writer.setColumnWidth(34, 30);  // article_title
    writer.setColumnWidth(35, 60);  // article_text
    writer.setColumnWidth(36, 30);  // edit_ids
    writer.setColumnWidth(37, 50);  // quoted_text
    writer.setColumnWidth(38, 15);  // quoted_author

    // 파일을 바꾸기 직전에 잊는다 — 쓰다 실패하면 비운 채로 두어 다음 저장이 파일을 읽게
    m_xlCachePath.clear();
    m_xlCacheRows.clear();
    const bool xlSaved = writer.save(filepath);
    m_xlSinceSave.start();
    m_xlLastSaveCostMs = xlTimer.elapsed();
    if (!xlSaved) {            // 실패는 저장 쪽이 기록에 적었다 — 새 줄 수 · '저장' 을 적지 않는다
        m_excelSaveFailed = true;
        return 0;
    }

    buffer.clear();   // 다 썼다 — 디스크 버퍼 비움(다음 저장은 새 줄만)
    FileHelper::setDownloadMeta(filepath, "(\"https://x.com\", \"Hanishiki\")");

    // 이번에 쓴 것을 기억한다 — 다음 저장이 파일을 다시 읽지 않게. 줄이 아주 많으면(메모리) 기억하지 않는다.
    if (mergedRows.size() <= 150000) {
        const QFileInfo wrote(filepath);
        m_xlCachePath = filepath;
        m_xlCacheSize = wrote.size();
        m_xlCacheMtimeMs = wrote.lastModified().toMSecsSinceEpoch();
        m_xlCacheRows = mergedRows;
    }

    // 기존 행 메모리 즉시 해제
    existingRows.clear();
    existingRows.squeeze();

    int totalCount = existingIds.size();
    m_backend->log(QString("저장: %1 (새 %2개, 총 %3개)")
                       .arg(filepath).arg(newRows.size()).arg(totalCount),
                   "success", "twitter");
    return int(newRows.size());
}

void TwitterCollector::saveProfileExcel(const QString &saveDir, const QString &target)
{
    if (!m_profileBuffer || m_profileBuffer->count() == 0) return;

    QString dateStr = Common::jstNow().toString("yyyyMMdd");

    // ── 날짜별 프로필 Excel: _profile_20260323.xlsx (덮어쓰기 방지) ──
    ExcelWriter writer;
    QStringList headers = {"screen_name", "name", "description", "followers_count",
                           "friends_count", "statuses_count", "location", "url",
                           "created_at", "verified", "profile_image_url", "profile_banner_url"};
    writer.writeHeader(headers, QColor("#57a0d9"));

    int row = 2;
    m_profileBuffer->resetReader();
    QJsonObject p;
    while (m_profileBuffer->readNext(p)) {
        writer.writeRow(row++, {
            p["screen_name"].toString(),
            p["name"].toString(),
            p["description"].toString(),
            QString::number(p["followers_count"].toInt()),
            QString::number(p["friends_count"].toInt()),
            QString::number(p["statuses_count"].toInt()),
            p["location"].toString(),
            p["url"].toString(),
            p["created_at"].toString(),
            p["verified"].toString(),
            p["profile_image_url"].toString(),
            p["profile_banner_url"].toString()
        });
    }

    writer.setColumnWidth(1, 15);   // screen_name
    writer.setColumnWidth(2, 20);   // name
    writer.setColumnWidth(3, 40);   // description
    writer.setColumnWidth(4, 10);   // followers
    writer.setColumnWidth(5, 10);   // friends
    writer.setColumnWidth(6, 10);   // statuses
    writer.setColumnWidth(7, 15);   // location
    writer.setColumnWidth(8, 30);   // url
    writer.setColumnWidth(9, 25);   // created_at
    writer.setColumnWidth(10, 8);   // verified
    writer.setColumnWidth(11, 40);  // profile_image
    writer.setColumnWidth(12, 40);  // banner

    // 날짜 폴더: profiles/20260323/
    QString profileDateDir = saveDir + "/profiles/"+ dateStr;
    QDir().mkpath(profileDateDir);

    // 날짜 폴더 안에 Excel 저장
    QString filepath = profileDateDir + "/"+ target + "_profile.xlsx";
    if (!writer.save(filepath)) return;

    FileHelper::setDownloadMeta(filepath, "(\"https://x.com\", \"Hanishiki\")");

    m_backend->log(QString("프로필 Excel 저장: %1명 → %2").arg(m_profileBuffer->count()).arg(filepath), "success", "twitter");

    // ── 프로필 설명 텍스트도 날짜 폴더에 저장 ──
    m_profileBuffer->resetReader();
    while (m_profileBuffer->readNext(p)) {
        QString sn = p["screen_name"].toString();
        if (sn.isEmpty()) continue;

        QString descFile = profileDateDir + "/"+ sn + "_description.txt";
        // 같은 날짜에 이미 저장했으면 건너뜀
        if (QFile::exists(descFile)) continue;

        QFile f(descFile);
        if (f.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream ts(&f);
            ts << "━━━ @"<< sn << "━━━\n";
            ts << "날짜: "<< Common::jstNow().toString("yyyy-MM-dd HH:mm:ss") << "\n";
            ts << "이름: "<< p["name"].toString() << "\n";
            ts << "설명: "<< p["description"].toString() << "\n";
            ts << "팔로워: "<< p["followers_count"].toInt() << "\n";
            ts << "팔로잉: "<< p["friends_count"].toInt() << "\n";
            ts << "트윗 수: "<< p["statuses_count"].toInt() << "\n";
            ts << "위치: "<< p["location"].toString() << "\n";
            ts << "URL: "<< p["url"].toString() << "\n";
            ts << "인증: "<< p["verified"].toString() << "\n";
            f.close();
        }
    }
}

// 계정을 idx 로 바꾼다 — 출구(프록시) · 토큰 · 데몬을 함께(r56: 레이트 리밋 · 정지 계정 · 남은 요청 0 이 함께 쓴다).
void TwitterCollector::switchToAccount(const QJsonArray &accounts, int idx)
{
    if (idx < 0 || idx >= accounts.size()) return;
    stopDaemon();
    ++m_accountSwitchCount;
    const int prevIdx = (m_activeAccountIdx >= 0 && m_activeAccountIdx < accounts.size()) ? m_activeAccountIdx : 0;
    m_activeAccountIdx = idx;
    QJsonObject account = accounts[idx].toObject();

    // ★ 계정이 바뀌면 나가는 길도 같이 바꾼다.
    //   토큰만 갈아 끼우면 B 계정 세션이 A 계정 프록시로 나간다 — 계정마다
    //   다른 IP 를 쓰려는 목적이 그 자리에서 깨진다. 수집 시작 때 한 번만
    //   걸어 두고 있었는데, 레이트리밋으로 계정이 도는 순간부터 어긋났다.
    //   여기는 수집 스레드 안이라 thread_local 이 이 수집에만 적용된다.
    //   반드시 setupClient·startDaemon 보다 먼저 해야 한다 — 데몬은 뜨는
    //   시점의 환경변수로 프록시를 물려받기 때문이다.
    setupClientFor(account);
    m_tidInitialized = false;
    m_rlRemaining = -1;   // 앞 계정의 남은 요청 수 — 새 계정에는 맞지 않는다
    m_rlReset = 0;
    m_backend->log(QString("계정 전환: %1 → %2 (%3/%4)")
        .arg(accounts[prevIdx].toObject()["name"].toString("Account"))
        .arg(account["name"].toString("Account"))
        .arg(idx + 1).arg(accounts.size()), "info", "twitter");
    if (!startDaemon()) {
        m_backend->log("데몬 시작 실패, TID fallback", "warning", "twitter");
        initTransactionIds();
    }
}

// ★ 이 계정의 X 상태를 판마다 한 번(r56). 정지된 계정은 검색만 막힌다(200 + DenySuspendedUsers) — 타임라인 · 사용자 조회는 된다
//   (2026-11-27 실측). 그래서 정지면 이 판의 검색에서만 빼고, 다른 단계는 그대로 쓴다. 토큰은 찍지 않는다.
//   옛 데몬(고침 꾸러미의 시차)은 이 동작을 몰라 error 로 답한다 — 그때는 모르는 것으로 두고 검색 답의 _cause 로 가린다.
void TwitterCollector::checkAccountStatus(int idx)
{
    if (m_accounts.isEmpty() || idx < 0 || idx >= m_accounts.size() || m_accountChecked.contains(idx)) return;
    m_accountChecked.insert(idx);
    const QJsonObject r = sendDaemonCommand(QJsonObject{{"action", "account_status"}}, 30000);
    if (r.contains(QStringLiteral("error")) || !r.contains(QStringLiteral("suspended"))) return;
    const QString nm = m_accounts.at(idx).toObject().value(QStringLiteral("name")).toString();
    const QString who = QString("계정 %1%2").arg(idx + 1).arg(nm.isEmpty() ? QString() : QStringLiteral("(") + nm + QStringLiteral(")"));
    if (r.value(QStringLiteral("suspended")).toBool()) {
        m_searchBlocked.insert(idx);
        m_backend->log(who + ": X 가 이 계정을 정지 상태로 봅니다 — 이 판에서는 이 계정으로 검색하지 않습니다(타임라인은 받습니다). "
                             "X 앱에서 계정 상태를 확인하십시오", "warning", "twitter");
    } else if (r.value(QStringLiteral("locked")).toBool()) {
        m_backend->log(who + ": X 가 이 계정을 잠갔습니다(오류 326) — x.com 에 로그인해 잠금을 푸십시오", "warning", "twitter");
    } else if (r.value(QStringLiteral("needs_phone_verification")).toBool()) {
        m_backend->log(who + ": X 가 전화번호 확인을 요구합니다 — 막히면 X 앱에서 확인하십시오", "info", "twitter");
    }
}

// ★ 남은 요청이 0 이면 429 를 받기 전에 쉬는 계정으로 돌린다(r56). 머리(_rl)는 끝점마다 따로라, 같은 동작의 마지막 답일 때만 믿는다.
//   계정이 하나뿐이거나 쉬는 계정이 없으면 아무것도 하지 않는다 — 다음 요청이 429 를 받으면 handleRateLimit 이 풀림 시각까지 기다린다.
void TwitterCollector::rotateBeforeLimit(const QString &action, bool forSearch)
{
    if (m_accounts.size() < 2 || m_rlRemaining != 0 || m_rlAction != action) return;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    if (m_rlReset <= now || m_rlReset - now > 3600) return;
    m_accountFreeAt[m_activeAccountIdx] = m_rlReset + 5;
    for (int i = 1; i < m_accounts.size(); ++i) {
        const int cand = (m_activeAccountIdx + i) % m_accounts.size();
        if (m_accountFreeAt.value(cand, 0) > now) continue;
        if (forSearch && m_searchBlocked.contains(cand)) continue;
        m_backend->log(QString("이 계정의 남은 요청이 0 입니다(%1초 뒤 풀림) — 막히기 전에 계정 %2 로 바꿉니다")
                           .arg(m_rlReset - now).arg(cand + 1), "info", "twitter");
        switchToAccount(m_accounts, cand);
        return;
    }
}

void TwitterCollector::handleRateLimit(const QJsonArray &accounts, int &currentIdx, const std::atomic<bool> &isRunning)
{
    // ── 계정별 Rate Limit 추적 + 무한 순환 ──
    // ★ 지금 쓰는 계정은 m_activeAccountIdx 가 안다 — 검색 · 타임라인이 스스로 계정을 돌릴 수 있어(정지 계정 · 남은 요청 0, r56)
    //   각 단계의 currentIdx 가 뒤처질 수 있다. 여기서 맞춘다.
    if (m_activeAccountIdx >= 0 && m_activeAccountIdx < accounts.size()) currentIdx = m_activeAccountIdx;
    m_consecutiveOk = 0;
    m_rateLimitHits++;
    qint64 now = QDateTime::currentSecsSinceEpoch();
    m_accountRateLimitTime[currentIdx] = now;
    // ★ X 가 알려 준 풀림 시각(x-rate-limit-reset — 429 답의 머리, r56). 알면 그때 + 5초까지만 기다린다. 예전엔 늘 15분이었다 —
    //   X 의 창은 첫 요청부터 15분이라, 몇 분 만에 다 쓰면 그만큼 더 기다렸다. 모르면(옛 데몬 · 머리 없음) 예전처럼 15분.
    //   머리는 끝점마다 따로라 이 429 를 낸 동작의 것일 때만 믿는다(다른 동작의 머리가 남아 있을 수 있다).
    const qint64 knownReset = (m_rlAction == m_lastApiAction && m_rlReset > now && m_rlReset - now <= 3600) ? m_rlReset : 0;
    // ★ 검색이 429 를 받았으면 검색이 막힌 계정(정지 — m_searchBlocked)으로는 돌리지 않는다(r56). 돌리면 그 계정이 곧바로 SUSPENDED 를 내고
    //   검색 루프가 막히지 않은 이 계정으로 되돌려, 30초 대기와 429 를 끝없이 오갔다.
    const bool forSearch = (m_lastApiAction == QLatin1String("search"));
    auto usable = [&](int i) { return !forSearch || !m_searchBlocked.contains(i); };
    m_accountFreeAt[currentIdx] = knownReset ? qMin(knownReset + 5, now + 900) : now + 900;
    m_rlRemaining = -1;
    m_rlReset = 0;

    m_backend->log(QString("Rate Limit 발생 (連続 %1回) — 계정 %2")
        .arg(m_rateLimitHits)
        .arg(accounts[currentIdx].toObject()["name"].toString("Account")), "warning", "twitter");

    auto switchTo = [&](int idx) {
        switchToAccount(accounts, idx);
        currentIdx = idx;
    };

    if (accounts.size() > 1) {
        // ── Rate Limit 안 걸린 계정 찾기 (풀림 시각이 지났거나 기록 없는 계정) ──
        int freshIdx = -1;
        for (int i = 1; i < accounts.size(); ++i) {
            int candidate = (currentIdx + i) % accounts.size();
            if (usable(candidate) && m_accountFreeAt.value(candidate, 0) <= now) {   // 풀림 시각을 지났다(모르면 429 뒤 15분) → 사용 가능
                freshIdx = candidate;
                break;
            }
        }

        if (freshIdx >= 0) {
            // 사용 가능한 계정 있음 → 전환 + 짧은 대기
            switchTo(freshIdx);
            int waitSec = 30; // 계정 전환 쿨다운
            m_backend->log(QString("계정 전환 대기 %1초...").arg(waitSec), "info", "twitter");
            for (int r = waitSec; r > 0 && isRunning; --r) {
                m_backend->updateStats(0, 0, QString("전환 대기 %1s").arg(r), "twitter");
                QThread::sleep(1);
            }
        } else {
            // 모든 계정 RL → 가장 먼저 풀릴 계정까지 대기
            qint64 soonest = std::numeric_limits<qint64>::max();
            int oldestIdx = currentIdx;
            for (int i = 0; i < accounts.size(); ++i) {
                if (!usable(i)) continue;
                const qint64 t = m_accountFreeAt.value(i, 0);
                if (t < soonest) { soonest = t; oldestIdx = i; }
            }
            int waitSec = static_cast<int>(qBound(qint64(0), soonest - now, qint64(900)));
            if (waitSec < 10) waitSec = 10; // 최소 10초(풀림 시각을 아는 때 — 모르면 위에서 15분으로 적었다)

            m_backend->log(QString("모든 계정 Rate Limit → %1 대기 후 재시도")
                .arg(waitSec >= 60 ? QString("%1분").arg((waitSec + 59) / 60) : QString("%1초").arg(waitSec)), "warning", "twitter");

            for (int remaining = waitSec; remaining > 0 && isRunning; --remaining) {
                int mins = remaining / 60;
                int secs = remaining % 60;
                m_backend->updateStats(0, 0, QString("RL待機 %1:%2").arg(mins).arg(secs, 2, 10, QChar('0')), "twitter");
                QThread::sleep(1);
            }

            // 가장 먼저 풀릴 계정으로 전환
            m_accountRateLimitTime.clear(); // 리셋
            m_accountFreeAt.remove(oldestIdx);
            switchTo(oldestIdx);
            m_rateLimitHits = 0;
            m_backend->log(QString("대기 완료! 재시작: %1 (%2/%3)")
                .arg(accounts[oldestIdx].toObject()["name"].toString("Account"))
                .arg(oldestIdx + 1).arg(accounts.size()), "success", "twitter");
            QThread::sleep(2);
        }
    } else {
        // 계정 1개 → 풀림 시각(모르면 15분)까지 대기 후 재시도 (무한 반복)
        const int waitSec = knownReset ? static_cast<int>(qBound(qint64(5), knownReset + 5 - now, qint64(900))) : 900;
        if (knownReset)
            m_backend->log(QString("X 가 알려 준 풀림 시각까지 %1초 대기 후 재시도... (RL %2회)").arg(waitSec).arg(m_rateLimitHits), "warning", "twitter");
        else
            m_backend->log(QString("15분 대기 후 재시도... (RL %1회)").arg(m_rateLimitHits), "warning", "twitter");

        for (int remaining = waitSec; remaining > 0 && isRunning; --remaining) {
            int mins = remaining / 60;
            int secs = remaining % 60;
            m_backend->updateStats(0, 0, QString("RL待機 %1:%2").arg(mins).arg(secs, 2, 10, QChar('0')), "twitter");
            QThread::sleep(1);
        }

        // ★ 사용자가 대기 중에 중지 버튼을 눌렀으면 데몬 재시작하지 말고 즉시 빠져나감
        if (!isRunning) {
            m_backend->log("대기 중 중지 요청 감지 — 데몬 재시작 생략", "info", "twitter");
            return;
        }

        stopDaemon();
        // ★ 데몬 재시작 + 리트라이 (5분 sleep 직후엔 일시적 네트워크 미복구 가능 — sleep/wake 시나리오)
        QJsonObject account = accounts[currentIdx].toObject();
        bool restarted = false;
        for (int attempt = 1; attempt <= 3 && isRunning && !restarted; ++attempt) {
            setupClientFor(account);
            m_tidInitialized = false;
            if (startDaemon()) {
                restarted = true;
                break;
            }
            m_backend->log(QString("데몬 재시작 실패 (%1/3) — %2초 후 재시도")
                .arg(attempt).arg(attempt * 5), "warning", "twitter");
            for (int s = attempt * 5; s > 0 && isRunning; --s) QThread::sleep(1);
        }
        if (!restarted) {
            m_backend->log("데몬 재시작 최종 실패 — TID fallback", "warning", "twitter");
            initTransactionIds();
        }

        m_accountRateLimitTime.clear();
        m_accountFreeAt.clear();
        m_rateLimitHits = 0;
        m_backend->log("대기 완료, 再開...", "success", "twitter");
    }
}

// ──────────────────────────────────────────────────────────────────────────
// captureTweet — 모든 type 분기 (likes/bookmarks/reposts/thread_comments/
//   thread/highlights/favoriters/retweeters/list/community/tweets/media/
//   replies/tweets_api)에서 호출되는 통합 캡쳐 헬퍼
//   realCapture=true일 때: SingleFile + CDP로 실제 x.com 페이지 통째 저장
//   false일 때 또는 캡쳐 실패 시: 합성 카드 HTML로 fallback
// ──────────────────────────────────────────────────────────────────────────
// 트윗 하나를 엑셀 한 줄로 — saveExcelStreaming 의 칸(tweetColumns — r56 부터 38칸)을 같은 뜻으로 채운다. 본 걸음 · 좋아요 · 북마크가 함께 쓴다.
//   ★ 좋아요 · 북마크는 예전에 따로 url · author · lang · media_types · sensitive 로 담아, 엑셀의 글 주소 · 작성자 · 언어 ·
//   미디어 종류 · 민감 칸이 비었고 종류 · 해시태그 · 미디어 주소 · 인용 같은 칸은 아예 없었다(사용자 2026-10-29 '엑셀에 빠진 항목').
//   fallbackAuthor: 작성자 이름을 못 찾을 때 글 주소에 쓸 이름(본 걸음은 그 계정, 남의 글은 i — x.com/i/status/<ID> 도 열린다)
//   isSpaceCard: 스페이스(음성) 카드 트윗이면 true
QJsonObject TwitterCollector::standardTweetRow(const QJsonObject &tweet, const QString &fallbackAuthor, bool *isSpaceCard)
{
    if (isSpaceCard) *isSpaceCard = false;
    const QJsonObject legacy = xLegacy(tweet);
    QString tweetId = legacy["id_str"].toString();
    if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();
    const QString createdAt = legacy["created_at"].toString();

    bool isRetweet = false;
    QJsonObject rtResult;
    if (legacy.contains("retweeted_status_result")) {
        rtResult = legacy["retweeted_status_result"].toObject()["result"].toObject();
        isRetweet = true;
    }
    if (rtResult.isEmpty() && tweet.contains("retweeted_status_result")) {
        rtResult = tweet["retweeted_status_result"].toObject()["result"].toObject();
        isRetweet = !rtResult.isEmpty();
    }
    if (!rtResult.isEmpty() && rtResult.contains("tweet"))
        rtResult = rtResult["tweet"].toObject();
    const bool isReply = !legacy["in_reply_to_status_id_str"].toString().isEmpty();
    // ★ 글의 내용(인용 · 카드 · 글(Articles) · 대체 글 · 고친 이력)은 리트윗이면 원글에 있다 — 바깥 리트윗 껍질에는 없다(r56, COV-7/8)
    const QJsonObject contentTw = (isRetweet && !rtResult.isEmpty()) ? rtResult : tweet;
    const QJsonObject contentLeg = xLegacy(contentTw);

    // Helper: extract screen_name from user result object (handles missing legacy.screen_name)
    auto getScreenName = [](const QJsonObject &userResult) -> QString {
        const QString sn = screenNameOf(userResult);
        return sn.isEmpty() ? userResult["rest_id"].toString() : sn;
    };

    // Get tweet author info
    QJsonObject authorResult = tweet["core"].toObject()["user_results"].toObject()["result"].toObject();
    QString authorName = getScreenName(authorResult);
    if (authorName.isEmpty()) authorName = fallbackAuthor;

    QString tweetText = legacy["full_text"].toString();
    // 긴 트윗 (NoteTweet, Premium 최대 25000자) — note_tweet.text 우선
    QJsonObject noteTweet = tweet["note_tweet"].toObject();
    if (!noteTweet.isEmpty()) {
        QString longText = noteTweet["note_tweet_results"].toObject()
            ["result"].toObject()["text"].toString();
        if (!longText.isEmpty() && longText.length() > tweetText.length()) {
            tweetText = longText;
        }
    }
    // URL에는 screen_name 사용 (rest_id 같은 숫자면 target으로 대체)
    QString urlAuthor = authorName;
    bool looksLikeId = true;
    for (QChar c : urlAuthor) { if (!c.isDigit()) { looksLikeId = false; break; } }
    if (looksLikeId || urlAuthor.isEmpty()) urlAuthor = fallbackAuthor;
    QString tweetUrl = QString("https://x.com/%1/status/%2").arg(urlAuthor, tweetId);

    if (isRetweet && !rtResult.isEmpty()) {
        QJsonObject rtLegacy = xLegacy(rtResult);
        QJsonObject rtUserResult = rtResult["core"].toObject()["user_results"].toObject()["result"].toObject();
        QString rtAuthor = getScreenName(rtUserResult);
        // RT의 원작자 screen_name이 없으면 full_text에서 추출 시도
        if (rtAuthor.isEmpty()) {
            // "RT @username: ..."패턴에서 추출
            QString ft = legacy["full_text"].toString();
            QRegularExpression rx("^RT @(\\w+):");
            auto m = rx.match(ft);
            if (m.hasMatch()) rtAuthor = m.captured(1);
        }
        if (rtAuthor.isEmpty()) rtAuthor = "?";
        QString rtId = rtLegacy["id_str"].toString();
        if (rtId.isEmpty()) rtId = rtResult["rest_id"].toString();
        // 원본 RT에도 긴 트윗이 있으면 적용
        QString rtFull = rtLegacy["full_text"].toString();
        QJsonObject rtNote = rtResult["note_tweet"].toObject();
        if (!rtNote.isEmpty()) {
            QString rtLong = rtNote["note_tweet_results"].toObject()
                ["result"].toObject()["text"].toString();
            if (!rtLong.isEmpty() && rtLong.length() > rtFull.length()) rtFull = rtLong;
        }
        tweetText = QString("RT @%1: %2").arg(rtAuthor, rtFull);
        tweetUrl = QString("https://x.com/%1/status/%2").arg(rtAuthor, rtId);
    }
    // Collect data — anipo/complete 형식 (21컬럼)
    QJsonObject data;
    data["id"] = tweetId;
    data["tweet_url"] = tweetUrl;
    data["text"] = tweetText;
    data["language"] = legacy["lang"].toString();

    // type: Tweet, Retweet, Reply, Quote Tweet, Poll (anipo 호환)
    bool isQuoteTweet = tweet.contains("quoted_status_result") ||
                        legacy.value("is_quote_status").toBool(false);
    // 투표 카드: card.legacy.name == "poll*choice*"패턴
    QJsonObject cardObj = tweet["card"].toObject();
    QString cardName = xLegacy(cardObj)["name"].toString();
    bool hasPoll = cardName.startsWith("poll");
    // 🎙️ 스페이스 카드 트윗 — status URL 을 모아 전체수집 끝에 yt-dlp 로 추출/다운로드.
    //   (호스팅/공유한 스페이스는 URL 엔티티가 아니라 카드라서, status URL 을 yt-dlp 에 넘겨 추출)
    if (cardName.contains("audiospace", Qt::CaseInsensitive)
        || cardName.contains("broadcast", Qt::CaseInsensitive)) {
        if (isSpaceCard) *isSpaceCard = true;
    }
    // 고정 트윗: 최상위에 pinned = true 마킹(타임라인의 TimelinePinEntry) 또는 대상 계정이 고정한 ID(사용자 조회 — 검색으로 먼저 받은 때)
    //   ★ 고정 ID 는 대상의 제 글 목록 줄에만 — 좋아요 · 북마크(작성자 대신 "i")도 이 줄을 쓰는데, 대상이 제 고정 글을 좋아요하면
    //     그 줄에도 📌 가 붙어 옛 트위터 보기가 고정 글로 보였다(검토 XAPI-4)
    const bool ownTimelineRow = fallbackAuthor != QLatin1String("i");
    bool isPinned = tweet["pinned"].toBool(false) || (ownTimelineRow && !tweetId.isEmpty() && m_pinnedIds.contains(tweetId));
    QString tweetType = "Tweet";
    if (isRetweet) tweetType = "Retweet";
    else if (isQuoteTweet) tweetType = "Quote Tweet";
    else if (isReply) tweetType = "Reply";
    if (hasPoll) tweetType = "Poll";
    // ★ 📌(U+1F4CC) + 빈칸 — 옛 트위터 보기(twitter_viewer.py)가 이 머리로 고정 글을 가른다. 예전엔 머리가 빈 글자라 한 번도 안 보였다(r56)
    if (isPinned) tweetType = QStringLiteral("\U0001F4CC ") + tweetType;
    data["type"] = tweetType;

    // 투표 데이터를 text 뒤에 JSON 형태로 첨부 (분석/기록용)
    if (hasPoll) {
        QJsonArray bindings = xLegacy(cardObj)["binding_values"].toArray();
        QMap<QString, QString> bmap;
        for (const auto &bv : bindings) {
            QJsonObject b = bv.toObject();
            QString k = b["key"].toString();
            QString v = b["value"].toObject()["string_value"].toString();
            if (!v.isEmpty()) bmap[k] = v;
        }
        QStringList pollLines;
        for (int i = 1; i <= 4; ++i) {
            QString labelKey = QString("choice%1_label").arg(i);
            QString countKey = QString("choice%1_count").arg(i);
            if (bmap.contains(labelKey)) {
                pollLines << QString("%1. %2 — %3표")
                    .arg(i).arg(bmap[labelKey], bmap.value(countKey, "0"));
            }
        }
        QString endTime = bmap.value("end_datetime_utc");
        QString state = bmap.value("counts_are_final") == "true"? "종료": "진행중";
        QString pollSummary = QString("\n[투표 %1] %2").arg(state, pollLines.join("/ "));
        if (!endTime.isEmpty()) pollSummary += QString("(마감 %1)").arg(endTime);
        tweetText += pollSummary;
        data["text"] = tweetText;
    }

    // Author info — 여러 경로에서 시도 (GraphQL 구조 변동 대응)
    {
        QJsonObject userResult = tweet["core"].toObject()["user_results"].toObject()["result"].toObject();
        QString aName = displayNameOf(userResult);
        QString aUsername = screenNameOf(userResult);
        // fallback: authorResult (이미 위에서 추출)
        if (aUsername.isEmpty()) aUsername = authorName;
        data["author_name"] = aName;
        data["author_username"] = aUsername;
    }

    data["bookmark_count"] = legacy["bookmark_count"].toInt();
    data["favorite_count"] = legacy["favorite_count"].toInt();
    data["retweet_count"] = legacy["retweet_count"].toInt();
    data["retweeted"] = isRetweet ? "True": "False";
    data["reply_count"] = legacy["reply_count"].toInt();
    data["quote_count"] = legacy["quote_count"].toInt();

    QJsonObject views = tweet["views"].toObject();
    data["view_count"] = views["count"].toString();

    // 날짜: YYYY/MM/DD HH:MM 형식 (JST)
    QDateTime tweetDt = Common::parseISODate(createdAt);
    if (tweetDt.isValid()) {
        tweetDt = tweetDt.toUTC().addSecs(9 * 3600);
        data["created_at"] = tweetDt.toString("yyyy/MM/dd HH:mm");
    } else {
        data["created_at"] = createdAt;
    }

    // 리트윗 시간 처리 (아니포 호환):
    //   created_at = 원본 트윗 작성 시각
    //   retweet_time = RT 버튼을 누른 시각
    if (isRetweet) {
        // 현재 created_at은 RT 타임라인의 시각 → retweet_time으로 이동
        data["retweet_time"] = data["created_at"].toString();
        // 원본 트윗의 created_at으로 교체
        if (!rtResult.isEmpty()) {
            QString origCreated = xLegacy(rtResult)["created_at"].toString();
            QDateTime origDt = Common::parseISODate(origCreated);
            if (origDt.isValid()) {
                origDt = origDt.toUTC().addSecs(9 * 3600);
                data["created_at"] = origDt.toString("yyyy/MM/dd HH:mm");
            }
        } else {
            // rtResult 없으면 created_at 구분 불가 → retweet_time만 표시
            // created_at은 RT시각 그대로 (원본 시각을 모름)
            data["created_at"] = "";
        }
    }

    // 소스 (앱) — tweet 또는 legacy 양쪽에서 찾기
    QString source = tweet["source"].toString();
    if (source.isEmpty()) source = legacy["source"].toString();
    if (source.contains(">")) {
        int start = source.indexOf('>') + 1;
        int end = source.indexOf('<', start);
        if (end > start) source = source.mid(start, end - start);
    }
    data["source"] = source;

    // 해시태그
    QJsonArray hashtagArr = legacy["entities"].toObject()["hashtags"].toArray();
    QStringList hashtags;
    for (const auto &h : hashtagArr) hashtags << h.toObject()["text"].toString();  // anipo와 동일: # 없이
    // ★ 긴 글(note_tweet)의 280자 뒤 해시태그 · 링크는 legacy.entities 에 없고 note_tweet 의 entity_set 에만 있다 · 리트윗은 원글의
    //   것이 온전하다(r56, COV-9). 예전 칸(위)은 그대로 두고 없는 것만 뒤에 덧붙인다.
    const QJsonObject noteSet = noteTweetOf(contentTw)["entity_set"].toObject();
    {
        QList<QJsonArray> more;
        if (isRetweet) more << contentLeg["entities"].toObject()["hashtags"].toArray();
        more << noteSet["hashtags"].toArray();
        for (const QJsonArray &arr : std::as_const(more))
            for (const auto &h : arr) {
                const QString t = h.toObject()["text"].toString();
                if (!t.isEmpty() && !hashtags.contains(t)) hashtags << t;
            }
    }
    data["hashtags"] = hashtags.isEmpty() ? "": hashtags.join(", ");

    // URLs
    QJsonArray urlArr = legacy["entities"].toObject()["urls"].toArray();
    QStringList urls;
    QHash<QString, QString> tcoToUrl;   // t.co → 펼친 주소(아래 카드 주소에도 쓴다)
    for (const auto &u : urlArr) urls << u.toObject()["expanded_url"].toString();
    {
        QList<QJsonArray> all{urlArr};
        if (isRetweet) all << contentLeg["entities"].toObject()["urls"].toArray();
        all << noteSet["urls"].toArray();
        for (int k = 0; k < all.size(); ++k)
            for (const auto &uv : all[k]) {
                const QJsonObject u = uv.toObject();
                const QString ex = u["expanded_url"].toString();
                if (!u["url"].toString().isEmpty() && !ex.isEmpty()) tcoToUrl.insert(u["url"].toString(), ex);
                if (k > 0 && !ex.isEmpty() && !urls.contains(ex)) urls << ex;   // 첫 묶음(위)은 예전 그대로
            }
    }
    data["urls"] = urls.isEmpty() ? "": urls.join(", ");

    // 미디어 타입 + URLs (본문 + RT 원본 모두 확인)
    QJsonArray mediaArr = legacy["extended_entities"].toObject()["media"].toArray();
    if (mediaArr.isEmpty()) mediaArr = legacy["entities"].toObject()["media"].toArray();
    // RT의 경우 원본 트윗의 미디어도 확인
    if (mediaArr.isEmpty() && isRetweet && !rtResult.isEmpty()) {
        QJsonObject rtLeg = xLegacy(rtResult);
        mediaArr = rtLeg["extended_entities"].toObject()["media"].toArray();
        if (mediaArr.isEmpty()) mediaArr = rtLeg["entities"].toObject()["media"].toArray();
    }
    QStringList mediaTypes, mediaUrls, altTexts;
    bool anyAlt = false;
    for (const auto &m : mediaArr) {
        QJsonObject mo = m.toObject();
        QString mt = mo["type"].toString();
        if (!mt.isEmpty() && !mediaTypes.contains(mt)) mediaTypes << mt;
        QString mu = mo["media_url_https"].toString();
        // ★ 영상 · GIF 는 실제로 받는 mp4 주소를 적는다(r56, COV-9) — media_url_https 는 영상의 미리보기 그림이다. mp4 가 없으면 예전 그대로.
        if (mt == QLatin1String("video") || mt == QLatin1String("animated_gif")) {
            const QString mp4 = bestMp4Url(mo);
            if (!mp4.isEmpty()) mu = mp4;
        }
        if (!mu.isEmpty()) mediaUrls << mu;
        // 대체 글(그림 설명) — 미디어 차례대로, 없는 것은 빈칸(아래에서 " | " 로 잇는다)
        const QString alt = mo["ext_alt_text"].toString();
        altTexts << alt;
        if (!alt.isEmpty()) anyAlt = true;
    }
    data["media_type"] = mediaTypes.join(", ");
    data["media_urls"] = mediaUrls.join("\n");  // anipo와 동일하게 줄바꿈 구분
    data["alt_text"] = anyAlt ? altTexts.join(" | ") : QString();

    // ── 추가 필드 (Twitter에서 가져올 수 있는 모든 정보) ──
    // 대화 스레드 ID
    data["conversation_id"] = legacy["conversation_id_str"].toString();

    // 답글 대상
    QString replyTo = legacy["in_reply_to_screen_name"].toString();
    if (!replyTo.isEmpty()) {
        QString replyToId = legacy["in_reply_to_status_id_str"].toString();
        data["in_reply_to"] = QString("@%1 (%2)").arg(replyTo, replyToId);
    } else {
        data["in_reply_to"] = "";
    }

    // 인용 트윗
    // ★ 리트윗한 인용 글이면 인용은 원글 아래에 있다 — 원글 것을 먼저, 없으면 바깥 것(r56, COV-7)
    QJsonObject quotedResult = quotedOf(contentTw);
    if (quotedResult.isEmpty()) quotedResult = quotedOf(tweet);
    bool isQuote = tweet.contains("quoted_status_result") ||
                   legacy.value("is_quote_status").toBool(false) ||
                   !quotedResult.isEmpty();
    data["is_quote"] = isQuote ? "True": "False";
    data["quoted_text"] = "";
    data["quoted_author"] = "";
    if (isQuote) {
        QString qId = quotedResult["rest_id"].toString();
        QJsonObject qLeg = xLegacy(quotedResult);
        // ★ screen_name 은 글의 legacy 가 아니라 작성자(core.user_results)에 있다 — 예전엔 글의 legacy 에서 찾아 이 칸이 늘 비었다(검토)
        const QString qScreenName = screenNameOf(quotedResult["core"].toObject()["user_results"].toObject()["result"].toObject());
        data["quoted_tweet_url"] = qId.isEmpty() ? QString()
            : QString("https://x.com/%1/status/%2").arg(qScreenName.isEmpty() ? QStringLiteral("i") : qScreenName, qId);
        // 인용한 글의 본문 · 작성자 — 주소만으로는 그 글이 지워진 뒤에 무엇을 인용했는지 알 수 없다(r56, COV-9)
        if (!quotedResult.isEmpty()) {
            data["quoted_text"] = fullTextOf(quotedResult);
            data["quoted_author"] = qScreenName;
        }
    } else {
        data["quoted_tweet_url"] = "";
    }

    // ── r56: 올라와 있는데 담지 않던 것 — 카드 · X 글(Articles) · 고친 이력(COV-8) ──
    {
        const QHash<QString, QJsonObject> bind = cardBindings(contentTw);
        QString cTitle = bind.value(QStringLiteral("title"))["string_value"].toString();
        QString cUrl = bind.value(QStringLiteral("card_url"))["string_value"].toString();
        if (cUrl.isEmpty()) cUrl = xLegacy(contentTw["card"].toObject())["url"].toString();
        const QJsonObject uc = unifiedCardOf(bind);   // 통합 카드 — 제목 · 가는 곳이 그 안의 JSON 에 있다
        if (!uc.isEmpty()) {
            for (const auto &cv : uc["component_objects"].toObject()) {
                const QString t = cv.toObject()["data"].toObject()["title"].toObject()["content"].toString();
                if (!t.isEmpty()) { if (cTitle.isEmpty()) cTitle = t; break; }
            }
            for (const auto &dv : uc["destination_objects"].toObject()) {
                const QString u = dv.toObject()["data"].toObject()["url_data"].toObject()["url"].toString();
                if (!u.isEmpty()) { if (cUrl.isEmpty()) cUrl = u; break; }
            }
        }
        // 투표 · 스페이스 카드는 위(투표 글 · 스페이스 모음)가 다룬다 — 여기에는 링크 카드만
        const QString contentCard = xLegacy(contentTw["card"].toObject())["name"].toString();   // 리트윗이면 원글의 카드
        if (hasPoll || contentCard.startsWith(QLatin1String("poll"))) { cTitle.clear(); cUrl.clear(); }
        data["card_title"] = cTitle;
        data["card_url"] = tcoToUrl.value(cUrl, cUrl);   // t.co 면 펼친 주소로

        const QJsonObject art = articleOf(contentTw);
        data["article_title"] = art["title"].toString();
        QString artText = art["plain_text"].toString();   // withArticlePlainText 를 켠 데몬이 준다 — 없으면 미리보기 글
        if (artText.isEmpty()) artText = art["preview_text"].toString();
        data["article_text"] = artText;

        QJsonObject ec = contentTw["edit_control"].toObject();
        QJsonArray editIds = ec["edit_tweet_ids"].toArray();
        if (editIds.isEmpty()) editIds = ec["edit_control_initial"].toObject()["edit_tweet_ids"].toArray();
        QStringList eids;
        for (const auto &e : editIds) if (!e.toString().isEmpty()) eids << e.toString();
        data["edit_ids"] = eids.size() > 1 ? eids.join(", ") : QString();   // 하나뿐이면 고친 적이 없다
    }

    // 민감 콘텐츠
    data["possibly_sensitive"] = legacy.value("possibly_sensitive").toBool(false) ? "True": "False";

    // 작성자 프로필 정보
    {
        QJsonObject uResult = tweet["core"].toObject()["user_results"].toObject()["result"].toObject();
        QJsonObject uLeg = xUserLegacy(uResult);
        data["user_followers"] = QString::number(uLeg["followers_count"].toInt());
        data["user_following"] = QString::number(uLeg["friends_count"].toInt());
        data["user_verified"] = uResult.value("is_blue_verified").toBool(false) ? "True": "False";
        data["user_profile_image"] = uLeg["profile_image_url_https"].toString();
    }

    return data;
}

// 이 글을 캡처할까 — realCaptureMode "off": 안 함 / "media": 미디어 있는 글만 / "all": 모두(기본).
//   미디어 유무는 이번에 새로 받은 수가 아니라 글 자체로 본다 — 예전 판이 받아 둔 글(다시 볼 때 새로 받은 수 0)도,
//   미디어를 받지 않는 판의 글도 '미디어 있는 글' 이다. 리트윗은 원문의 미디어를 본다(downloadTweetMedia 와 같다).
static bool tweetWantsCapture(const QJsonObject &tweet, const QJsonObject &config)
{
    const QString capMode = config["realCaptureMode"].toString("all");
    if (capMode == QLatin1String("off")) return false;
    if (capMode != QLatin1String("media")) return true;
    QJsonObject legacy = xLegacy(tweet);
    QJsonObject rt = legacy["retweeted_status_result"].toObject()["result"].toObject();
    if (rt.isEmpty()) rt = tweet["retweeted_status_result"].toObject()["result"].toObject();
    if (!rt.isEmpty()) {
        if (rt.contains("tweet")) rt = rt["tweet"].toObject();
        legacy = xLegacy(rt);
    }
    return !legacy["extended_entities"].toObject()["media"].toArray().isEmpty()
        || !legacy["entities"].toObject()["media"].toArray().isEmpty();
}

// ★ r56 — 미디어 파일 이름 속 트윗 ID. 이름 꼴: {시각}{본문}-{ID}[-{순번}]({미디어 키}).{확장자}, 본문이 비면 {시각}{ID}-{순번}(키).{확장자}
//   (downloadTweetMedia). 끝에서 읽는다 — 본문 속 숫자에 속지 않게. 8자리 넘는 숫자 덩이도 함께 열쇠로 둔다(꼴이 다른 옛 이름).
//   맨 앞의 날짜(yyyyMMdd_)는 뺀다.
static QStringList mediaNameTweetIds(const QString &name)
{
    QStringList ids;
    static const QRegularExpression tail(QStringLiteral(R"((?:^|[-_])(\d+)(?:-\d+)?(?:\([^()]*\))?\.[A-Za-z0-9]+$)"));
    const auto m = tail.match(name);
    if (m.hasMatch()) ids << m.captured(1);
    static const QRegularExpression runs(QStringLiteral(R"(\d{8,})"));
    for (auto it = runs.globalMatch(name); it.hasNext();) {
        const auto r = it.next();
        if (r.capturedStart() == 0 && r.capturedLength() == 8 && name.mid(8, 1) == QLatin1String("_")) continue;   // 날짜 머리
        if (!ids.contains(r.captured())) ids << r.captured();
    }
    return ids;
}

static qint64 dirStampOf(const QString &dir) { return QFileInfo(dir).lastModified().toMSecsSinceEpoch(); }

QStringList TwitterCollector::cardMediaFor(const QString &mediaRoot, const QStringList &tweetIds, const QStringList &authors,
                                           int expected)
{
    CardMediaIndex &ix = m_cardMediaIndex[mediaRoot];
    auto addFile = [&ix](const QFileInfo &fi) {
        const QString fn = fi.fileName();
        if (fn.startsWith(QLatin1Char('.')) || fn.endsWith(QLatin1String(".part")) || fi.size() <= 0) return;   // 받다 만 것
        const QString path = fi.absoluteFilePath();
        for (const QString &id : mediaNameTweetIds(fn)) {
            QStringList &l = ix.byId[id];
            if (!l.contains(path)) l << path;
        }
    };
    auto scanFlat = [&ix, &addFile](const QString &dir) {
        ix.dirStamp[dir] = dirStampOf(dir);
        ix.scanAt[dir] = QDateTime::currentMSecsSinceEpoch();
        for (const QFileInfo &fi : QDir(dir).entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) addFile(fi);
    };
    auto refreshKinds = [&ix, &mediaRoot]() {
        ix.dirStamp[mediaRoot] = dirStampOf(mediaRoot);
        ix.scanAt[mediaRoot] = QDateTime::currentMSecsSinceEpoch();
        for (const QFileInfo &k : QDir(mediaRoot).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
            if (k.fileName() != QLatin1String("_complete") && !ix.kindDirs.contains(k.absoluteFilePath()))
                ix.kindDirs << k.absoluteFilePath();
    };
    // ★ r56 검토 — 수정 시각이 1~2초 단위인 디스크(HFS+ · exFAT)와 NAS 연결(시각이 늦게 바뀌거나 안 바뀐다)에서는 지난번 훑은
    //   그 초에 들어온 파일이 폴더 시각을 바꾸지 못해 이번 판 카드에서 빠졌다(downloadTweetMedia 바로 뒤에 captureTweet 이 돈다).
    //   시각이 바뀌었거나, 훑은 때와 2.5초 안으로 가까우면 다시 본다.
    auto changed = [&ix](const QString &d) {
        const qint64 st = dirStampOf(d);
        return st != ix.dirStamp.value(d, -1) || st >= ix.scanAt.value(d, 0) - 2500;
    };
    const bool firstBuild = !ix.built;
    if (!ix.built) {
        // 판마다 한 번만 통째로 — _complete(받은 파일 모두의 사본)는 건너뛴다(예전엔 그것까지 카드마다 훑었다)
        ix.built = true;
        if (QDir(mediaRoot).exists()) {
            refreshKinds();
            for (const QFileInfo &fi : QDir(mediaRoot).entryInfoList(QDir::Files | QDir::NoDotAndDotDot)) addFile(fi);
            QStringList stack = ix.kindDirs;
            while (!stack.isEmpty()) {
                const QString d = stack.takeLast();
                ix.dirStamp[d] = dirStampOf(d);
                ix.scanAt[d] = QDateTime::currentMSecsSinceEpoch();
                for (const QFileInfo &e : QDir(d).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot)) {
                    if (e.isDir()) { if (!e.fileName().startsWith(QLatin1Char('.'))) stack << e.absoluteFilePath(); }
                    else addFile(e);
                }
            }
        }
    } else if (QDir(mediaRoot).exists()) {
        // 이번 판에 새로 받은 것 — 갈래 폴더와 그 글의 작성자 폴더 가운데 수정 시각이 바뀐 것만 다시 본다(파일이 붙으면 폴더 시각이 바뀐다)
        if (changed(mediaRoot)) refreshKinds();
        for (const QString &kd : ix.kindDirs) {
            QStringList dirs{kd};
            for (const QString &a : authors)
                if (!a.isEmpty()) dirs << kd + QLatin1Char('/') + FileHelper::reuseExistingName(kd, a);
            for (const QString &d : dirs)
                if (QFileInfo(d).isDir() && changed(d)) scanFlat(d);
        }
    }
    auto collectOut = [&ix, &tweetIds]() {
        QStringList out;
        for (const QString &id : tweetIds)
            for (const QString &p : ix.byId.value(id))
                if (!out.contains(p) && QFileInfo(p).size() > 0) out << p;   // 그새 옮겨지거나 지워진 것은 뺀다
        out.sort();   // 순서를 고정한다
        return out;
    };
    QStringList out = collectOut();
    // ★ r56 검토 — 그래도 있어야 할 수보다 적으면(폴더 시각이 아예 안 바뀌는 NAS 연결) 이 글의 갈래 · 작성자 폴더를 시각과 상관없이
    //   한 번 더 본다. 글마다 한 판에 한 번만 — 미디어 받기를 끈 판에 카드마다 다시 훑지 않게.
    if (!firstBuild && expected > out.size() && !tweetIds.isEmpty() && !ix.forced.contains(tweetIds.first())
        && QDir(mediaRoot).exists()) {
        ix.forced.insert(tweetIds.first());
        refreshKinds();
        for (const QString &kd : ix.kindDirs) {
            if (!QFileInfo(kd).isDir()) continue;
            scanFlat(kd);
            for (const QString &a : authors) {
                if (a.isEmpty()) continue;
                const QString d = kd + QLatin1Char('/') + FileHelper::reuseExistingName(kd, a);
                if (QFileInfo(d).isDir()) scanFlat(d);
            }
        }
        out = collectOut();
    }
    return out;
}

int TwitterCollector::captureTweet(const QJsonObject &tweet, const QString &capturesDir, const QJsonObject &config)
{
    if (capturesDir.isEmpty()) return 0;
    QJsonObject legacy = xLegacy(tweet);
    QString tweetId = legacy["id_str"].toString();
    if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();
    if (tweetId.isEmpty()) return 0;

    QJsonObject userR = tweet["core"].toObject()["user_results"].toObject()["result"].toObject();
    QString screenName = screenNameOf(userR);
    if (screenName.isEmpty()) screenName = "i";

    QString tweetUrl = QString("https://x.com/%1/status/%2").arg(screenName, tweetId);
    QString createdAt = legacy["created_at"].toString();
    QString filename = FileHelper::captureNameCompat(capturesDir, Common::parseISODate(createdAt), tweetId);

    // ★ r56 — 폴더를 못 만들면 알린다(한 판에 폴더마다 한 번). 진짜 캡쳐는 그래도 임시 디스크에 받아 둘 수 있다(외장 · NAS).
    //   ★ r56 검토 — 저장 디스크 · NAS 연결이 빠져 있으면 만들지 않는다. 빈 연결 자리(~/Mountain Duck/<연결> · CloudStorage)에
    //     만들면 내장 디스크에 가짜 폴더가 생기고, saveVolumePresent 가 그것을 '연결됨' 으로 봐 임시 디스크의 캡쳐가 그리로 옮겨졌다.
    const bool capVolGone = Common::isOffLocalPath(capturesDir) && !Common::saveVolumePresent(capturesDir);
    if (!capVolGone && !QDir().mkpath(capturesDir)) {
        static QSet<QString> s_toldDirs;
        static QMutex s_toldMu;
        QMutexLocker lk(&s_toldMu);
        if (!s_toldDirs.contains(capturesDir)) {
            s_toldDirs.insert(capturesDir);
            m_backend->log(QString("캡쳐 폴더를 만들 수 없습니다: %1 — 저장 디스크 연결 · 폴더 권한을 확인하십시오").arg(capturesDir),
                           "warning", "twitter");
        }
    }
    const QString capPath = capturesDir + "/" + filename + ".html";

    // ★ 이미 받은 진짜 캡처는 그대로 둔다. 예전엔 캡처를 끄고 다시 도는 판이 그 파일을 합성 카드로 덮어써,
    //   그 사이 지워진 글의 페이지를 영영 잃었다(검토 — 캡처 설정이 바뀌면 목록을 끝까지 보므로 목록 전체가 그렇게 된다).
    if (m_backend->realCaptureFinished(capturesDir, filename)) return 0;
    // ★ r56 — 진짜 캡쳐가 임시 디스크에서 이 자리로 옮겨지기를 기다린다 — 카드로 덮지도, 다시 받지도 않는다
    if (m_backend->stagedCapturePending(capPath)) return 0;

    // ★ r56 — 카드에 붙일 미디어: 리트윗이면 원문, 인용이면 인용된 글의 것까지(파일 이름에는 그 글의 ID 가 든다 — downloadTweetMedia 와 같다).
    //   예전엔 바깥 글의 ID 만 찾아 리트윗 카드에는 그림이 하나도 붙지 않았다.
    QStringList mediaIds{tweetId}, mediaAuthors;
    int mediaExpected = 0;
    {
        QJsonObject mediaTw = tweet;
        QJsonObject rt = legacy["retweeted_status_result"].toObject()["result"].toObject();
        if (rt.isEmpty()) rt = tweet["retweeted_status_result"].toObject()["result"].toObject();
        if (rt.contains("tweet")) rt = rt["tweet"].toObject();
        if (!rt.isEmpty()) mediaTw = rt;
        // 인용은 리트윗이면 원글 아래에 있다 — downloadTweetMedia · standardTweetRow 와 같은 순서로 찾는다(검토)
        QJsonObject qt = quotedOf(mediaTw);
        if (qt.isEmpty()) qt = quotedOf(tweet);
        for (const QJsonObject &tw : {mediaTw, qt}) {
            if (tw.isEmpty()) continue;
            const QJsonObject lg = xLegacy(tw);
            QString id = lg["id_str"].toString();
            if (id.isEmpty()) id = tw["rest_id"].toString();
            if (!id.isEmpty() && !mediaIds.contains(id)) mediaIds << id;
            const QString au = screenNameOf(tw["core"].toObject()["user_results"].toObject()["result"].toObject());
            if (!au.isEmpty() && !mediaAuthors.contains(au)) mediaAuthors << au;
            QJsonArray md = lg["extended_entities"].toObject()["media"].toArray();
            if (md.isEmpty()) md = lg["entities"].toObject()["media"].toArray();
            mediaExpected += md.size();
        }
    }
    const QString mediaRoot = [&capturesDir]() {
        // <userDir>/captures[/likes …] → <userDir>/media. 예전엔 capturesDir 의 부모 + /media 라 captures/likes 의 카드는
        //   있지도 않은 captures/media 를 찾아 그림이 늘 빠졌다.
        const int at = capturesDir.lastIndexOf(QLatin1String("/captures"));
        return (at > 0 ? capturesDir.left(at) : QFileInfo(capturesDir).absolutePath()) + QStringLiteral("/media");
    }();

    const bool realCapture = config["realCapture"].toBool(true);
    // ★ r56 — 캡쳐를 끈 판: 지금 꼴의 카드가 이미 있고 그 뒤로 붙일 미디어가 늘지 않았으면 다시 쓰지 않는다.
    //   예전엔 판마다 모든 카드를 다시 쓰며 카드마다 미디어 폴더를 통째로 훑었다.
    if (!realCapture) {
        int have = 0, want = 0;
        if (FileHelper::readTweetCardMark(capPath, &have, &want)
            && (have >= want || cardMediaFor(mediaRoot, mediaIds, mediaAuthors).size() <= have))
            return 0;
    }
    bool stagedNoCard = false;   // 진짜 캡쳐는 임시 디스크에 있고 제 자리엔 아직 아무것도 없다
    if (realCapture) {
        // x.com 로그인 쿠키 주입 (NSFW/age-gated/private 컨텐츠 통과)
        QList<QNetworkCookie> ckList;
        if (!m_authToken.isEmpty()) {
            QNetworkCookie c1("auth_token", m_authToken.toUtf8());
            c1.setDomain(".x.com"); c1.setPath("/"); c1.setSecure(true);
            ckList << c1;
            QNetworkCookie c1b("auth_token", m_authToken.toUtf8());
            c1b.setDomain(".twitter.com"); c1b.setPath("/"); c1b.setSecure(true);
            ckList << c1b;
        }
        if (!m_ct0.isEmpty()) {
            QNetworkCookie c2("ct0", m_ct0.toUtf8());
            c2.setDomain(".x.com"); c2.setPath("/"); c2.setSecure(true);
            ckList << c2;
            QNetworkCookie c2b("ct0", m_ct0.toUtf8());
            c2b.setDomain(".twitter.com"); c2b.setPath("/"); c2b.setSecure(true);
            ckList << c2b;
        }
        // ★ 사용자가 UI에서 입력한 추가 raw cookie (NSFW/age-gate 통과: kdt, twid, gt 등)
        QString extraCk = config["captureCookie"].toString();
        if (!extraCk.isEmpty()) {
            for (const QString &part : extraCk.split(';', Qt::SkipEmptyParts)) {
                int eq = part.indexOf('=');
                if (eq <= 0) continue;
                QByteArray name = part.left(eq).trimmed().toUtf8();
                QByteArray value = part.mid(eq + 1).trimmed().toUtf8();
                // x.com + twitter.com 두 도메인 모두 set
                QNetworkCookie cx(name, value);
                cx.setDomain(".x.com"); cx.setPath("/"); cx.setSecure(true);
                ckList << cx;
                QNetworkCookie ct(name, value);
                ct.setDomain(".twitter.com"); ct.setPath("/"); ct.setSecure(true);
                ckList << ct;
            }
        }
        // ★ 트위터 로그인 체크 — 쿠키 만료 시 /i/flow/login으로 redirect되면 사용자 직접 처리
        //   article이 이미 있으면 로그인 페이지 아님 (검색창 input과 헷갈리지 않게)
        static const QString twLoginCheck = R"JS(
            (function(){
                if (location.pathname.indexOf('/i/flow/login') === 0
                 || location.pathname.indexOf('/login') === 0) return true;
                // article이 존재하면 로그인된 상태 — false positive 방지
                if (document.querySelector('article[data-testid="tweet"], article[role="article"]')) return false;
                // login 폼 셀렉터 (검색창과 구분: autocomplete=username 또는 password 타입)
                return !!document.querySelector('input[autocomplete="username"], input[type="password"]');
            })()
        )JS";
        HanishikiBackend::CaptureResult capRes = HanishikiBackend::CaptureResult::Failed;
        if (m_backend->captureRealPageCDPLoginAware(tweetUrl, capturesDir, filename,
                                                     twLoginCheck, "twitter", 8000, ckList, config, &capRes))
            return 1;
        // ★ r56 — 진짜 캡쳐는 받았지만 저장 폴더로 아직 못 옮겼다(임시 디스크에 옆 기록과 함께 있다). 그 자리에 있는 것
        //   (예전 카드 · 반쪽)은 덮지 않는다 — 옮길 때 진짜로 바뀐다. 아무것도 없으면 볼 것이 있게 카드를 두고, 옮길 때 바뀐다.
        //   진짜를 받은 것이므로 '받음' 으로 센다(옮기기는 앱 시작 · 다음 수집 시작 때 이어진다).
        if (capRes == HanishikiBackend::CaptureResult::Staged) {
            if (QFileInfo::exists(capPath)) return 1;
            stagedNoCard = true;
        }
        // 캡쳐 실패 → 합성 카드로 fallback(진짜 캡처는 못 받았다 — false)
    }

    // ★ r56 검토 — 저장 디스크 · NAS 연결이 빠져 있으면 카드를 쓰지 않는다. 카드 쓰기의 mkpath 가 빈 연결 자리에 가짜 폴더를 만들어
    //   (위 capVolGone 과 같은 함정) 임시 디스크의 진짜 캡쳐가 내장 디스크로 옮겨졌다. 다음 판(연결된 뒤)에 쓴다.
    if (capVolGone) return stagedNoCard ? 1 : (realCapture ? -1 : 0);

    // 합성 아카이브 카드 (realCapture 미사용 또는 CDP 실패)
    bool isRetweet = tweet.contains("retweeted_status_result");
    bool isReply = !legacy["in_reply_to_screen_name"].toString().isEmpty();
    QString authorName = displayNameOf(userR);
    if (authorName.isEmpty()) authorName = screenName;

    QJsonObject meta;
    meta["authorName"]    = authorName;
    meta["handle"]        = screenName;
    meta["tweetId"]       = tweetId;
    meta["tweetUrl"]      = tweetUrl;
    meta["tweetText"]     = legacy["full_text"].toString();
    meta["createdAt"]     = createdAt;
    meta["tweetType"]     = isRetweet ? "Retweet" : (isReply ? "Reply" : "Tweet");
    meta["favoriteCount"] = legacy["favorite_count"].toInt();
    meta["retweetCount"]  = legacy["retweet_count"].toInt();
    meta["replyCount"]    = legacy["reply_count"].toInt();

    // ★ 이미 내려받아 둔 이 트윗의 미디어를 카드에 붙인다 — 디스크에 실제로 있는 것만(받다 만 것은 제외).
    //   ★ r56 — 판마다 한 번 만든 색인에서 찾고(카드마다 media/ 를 통째로 훑지 않는다), 카드 자리에서 본 상대 경로로 적는다
    //     (captures/likes 의 카드는 ../../media/…). 카드는 붙인 수와 있어야 할 수를 함께 적어, 빠진 것은 글로 알린다.
    {
        QJsonArray rel;
        const QDir capDir(capturesDir);
        // 진짜 캡쳐를 바랐던 판의 대신 카드는 다음 판에도 그대로 남는다 — 빠짐이 있으면 그 글의 폴더를 한 번 더 본다(cardMediaFor)
        for (const QString &abs : cardMediaFor(mediaRoot, mediaIds, mediaAuthors, realCapture ? mediaExpected : -1))
            rel.append(capDir.relativeFilePath(abs));
        if (!rel.isEmpty()) meta["mediaRelPaths"] = rel;
        meta["mediaExpected"] = mediaExpected;
        // 카드가 '왜' 합성인지 남긴다 — 카드만 봐서는 원래 글만 있었는지
        // 캡쳐가 실패했는지 구분이 안 됐다.
        meta["cardReason"] = stagedNoCard
            ? QStringLiteral("실제 페이지 캡쳐는 임시 디스크에 있습니다 — 저장 디스크로 옮기면 이 카드가 진짜 페이지로 바뀝니다")
            : realCapture ? QStringLiteral("실제 페이지 캡쳐에 실패해 만든 카드입니다")
                          : QStringLiteral("캡쳐를 끄고 수집해 만든 카드입니다");
    }
    if (stagedNoCard) {
        // ★ r56 검토 — 위에서 '비어 있음' 을 본 뒤 카드를 쓰기까지(미디어 색인 · 메타) 그사이에 옮기기(앱 시작 스레드 · 나란히 도는
        //   다른 트위터 수집 · 크롤 시작)가 진짜 캡쳐를 이 자리에 넣으면, 예전엔 카드가 그것을 Truncate 로 덮어 진짜가 영영 사라졌다.
        //   옮기기와 같은 잠금 안에서 다시 보고, 그래도 비어 있을 때만 새 파일로(NewOnly) 쓴다.
        QMutexLocker dl(&m_backend->stagedDeliveryMutex());
        if (!QFileInfo::exists(capPath))
            FileHelper::generateTweetArchiveHtml(capturesDir, filename, meta, /*noOverwrite=*/true);
        return 1;     // 진짜 캡쳐는 받아 두었다(옮겨지기를 기다림)
    }
    FileHelper::generateTweetArchiveHtml(capturesDir, filename, meta);
    return realCapture ? -1 : 0;   // 진짜 캡처를 바랐는데 카드로 대신했으면 -1
}

// ── 전체 수집(all) 후속: 타임라인 트윗에 공유된 스페이스 링크 자동탐지 → yt-dlp 다운로드 ──
//    저장된 _complete.xlsx 의 text(3)/urls(19)/quoted_tweet_url(25) 컬럼을 스캔한다.
//    (트위터는 "유저의 과거 스페이스 목록" API 가 없어, 타임라인에 공유된 것 + 녹화본만 가능)
void TwitterCollector::collectSpacesFromTimeline(const QJsonObject &config, const QString &target,
                                                 const QString &userDir, const std::atomic<bool> &isRunning)
{
    Q_UNUSED(config);
    if (!isRunning) return;
    m_backend->log("🎙️ 스페이스 자동탐지: 게시물에서 스페이스 검색...", "info", "twitter");

    QStringList targets;
    QSet<QString> seen;
    // 1) (주 신호) 수집 중 발견한 '스페이스 카드' 트윗의 status URL — 유저가 호스팅/공유한 스페이스.
    //    yt-dlp 가 status URL 에서 스페이스를 추출한다(검증됨).
    for (const QString &u : m_spaceCardTweetUrls) {
        if (!u.isEmpty() && !seen.contains(u)) { seen.insert(u); targets << u; }
    }
    // 2) (보강) 저장된 _complete.xlsx 의 text/urls/quoted_tweet_url 에 박힌 i/spaces|broadcasts 링크
    //    — 리트윗/인용/직접 링크된 스페이스.
    const QString completePath = userDir + "/" + target + "_complete.xlsx";
    if (QFile::exists(completePath)) {
        QRegularExpression rx(R"((?:x\.com|twitter\.com)/i/(?:spaces|broadcasts)/([A-Za-z0-9]+))");
        QXlsx::Document doc(completePath);
        const int lastRow = doc.dimension().lastRow();
        for (int r = 2; r <= lastRow; ++r) {
            const QString blob = doc.read(r, 3).toString() + " "
                               + doc.read(r, 19).toString() + " "
                               + doc.read(r, 25).toString();
            auto it = rx.globalMatch(blob);
            while (it.hasNext()) {
                const QString url = "https://x.com/i/spaces/" + it.next().captured(1);
                if (!seen.contains(url)) { seen.insert(url); targets << url; }
            }
        }
    }

    if (targets.isEmpty()) {
        m_backend->log("🎙️ 스페이스 자동탐지: 발견된 스페이스 없음.", "info", "twitter");
        return;
    }
    m_backend->log(QString("🎙️ 스페이스 %1개 발견 — 다운로드 시작 (녹화본만 가능).").arg(targets.size()),
                   "success", "twitter");

    const QString outDir = userDir + "/spaces";
    int ok = 0;
    for (const QString &u : targets) {
        if (!isRunning) break;
        if (m_backend->downloadSpaceUrl(u, outDir, &isRunning)) ok++;
    }
    m_backend->log(QString("🎙️ 스페이스 자동탐지 완료: %1/%2 성공.").arg(ok).arg(targets.size()),
                   ok > 0 ? "success" : "warning", "twitter");
}

// ── 전체 수집(all) 후속: 유저의 자기-답글 체인(스레드)을 자동탐지 → 기존 thread 재구성 재사용 ──
//    _complete.xlsx 에서 author_username(7)==target 이고 in_reply_to(23)==target 인 행의
//    conversation_id(22) 를 스레드 루트로 간주(트위터 conversation_id == 루트 트윗 id).
void TwitterCollector::collectThreadsAuto(const QJsonObject &config, const QString &target,
                                          const QString &userDir, const std::atomic<bool> &isRunning)
{
    if (!isRunning) return;
    const QString completePath = userDir + "/" + target + "_complete.xlsx";
    if (!QFile::exists(completePath)) return;
    m_backend->log("🧵 스레드 자동탐지: 자기-답글 체인 검색...", "info", "twitter");

    // conv → 그 스레드에서 가장 새 자기-답글 ID · 가장 오래된 자기-답글이 답한 글(스레드의 첫 글)
    QHash<QString, quint64> newestSelfReply;
    QHash<QString, quint64> oldestSelfReply;
    QHash<QString, QString> threadHead;
    {
        // ★ in_reply_to 칸은 '@이름 (글 ID)' 로 적힌다(standardTweetRow) — 그 칸을 이름과 그대로 견줘 한 번도 맞지 않았다
        //   ('발견된 스레드 없음' 이 늘 찍혔다 — r56, COV-6). 앞의 @이름만 떼어 견준다(옛 줄이 이름만 적었어도 맞는다).
        static const QRegularExpression reReplyTo(QStringLiteral("^@?([A-Za-z0-9_]+)(?:\\s*\\((\\d+)\\))?"));
        QXlsx::Document doc(completePath);
        const int lastRow = doc.dimension().lastRow();
        for (int r = 2; r <= lastRow; ++r) {
            const QString author = doc.read(r, 7).toString().trimmed();
            const QRegularExpressionMatch rm = reReplyTo.match(doc.read(r, 23).toString().trimmed());
            const QString inReplyTo = rm.captured(1);   // 답글 대상 screen name
            const QString conv = doc.read(r, 22).toString().trimmed();
            if (conv.isEmpty()) continue;
            if (author.compare(target, Qt::CaseInsensitive) == 0 &&
                inReplyTo.compare(target, Qt::CaseInsensitive) == 0) {
                // 자기 자신에게 답글 = 스레드
                const quint64 id = doc.read(r, 1).toString().trimmed().toULongLong();
                newestSelfReply[conv] = qMax(newestSelfReply.value(conv), id);
                const quint64 old = oldestSelfReply.value(conv);
                if (!oldestSelfReply.contains(conv) || (id && (old == 0 || id < old))) {
                    oldestSelfReply[conv] = id;
                    threadHead[conv] = rm.captured(2);   // 옛 줄(이름만)이면 비고 — 그때는 대화 뿌리에서 시작
                }
            }
        }
    }
    if (newestSelfReply.isEmpty()) {
        m_backend->log("🧵 스레드 자동탐지: 발견된 스레드 없음.", "info", "twitter");
        return;
    }

    // ★ 이미 다 엮은 스레드는 다시 엮지 않는다(r56, 검토 B-1). 예전엔 판마다 같은 최신 30개를 TweetDetail 로 처음부터 다시 걸었다 —
    //   글은 이미 _complete 에 있어 새로 담기는 것이 없는데 시간과 API 만 썼다. 끊김 없이 다 엮은 스레드는 그때의 가장 새 자기-답글 ID 를
    //   엑셀 옆 작은 파일(.hanishiki_threads_done_<대상>.json — {대화 ID: 자기-답글 ID})에 적고, 그보다 새 자기-답글이 생긴 것만 다시 엮는다.
    const QString donePath = userDir + "/.hanishiki_threads_done_" + target + ".json";
    QJsonObject threadsDone = readJsonFile(donePath);
    QStringList convList;
    for (auto it = newestSelfReply.cbegin(); it != newestSelfReply.cend(); ++it)
        if (it.value() > threadsDone.value(it.key()).toString().toULongLong()) convList << it.key();
    if (convList.isEmpty()) {
        m_backend->log(QString("🧵 스레드 %1개 — 새 자기-답글 없음, 다시 엮지 않습니다.").arg(newestSelfReply.size()), "info", "twitter");
        return;
    }

    // 새 스레드부터(대화 ID = 뿌리 글 ID 는 시각 순) — 상한에 걸려도 최근 것을 먼저 다시 엮는다
    std::sort(convList.begin(), convList.end(), [](const QString &a, const QString &b) { return a.toULongLong() > b.toULongLong(); });
    const int MAX_THREADS = 30;    // 과도한 재구성(데몬 재시작 반복으로 먹통처럼 보임) 방지 상한 — 남은 것은 다음 판에
    if (convList.size() > MAX_THREADS) {
        m_backend->log(QString("🧵 엮을 스레드 %1개(전체 %2개) — 이번 판은 %3개, 나머지는 다음 판에.")
                       .arg(convList.size()).arg(newestSelfReply.size()).arg(MAX_THREADS), "warning", "twitter");
        convList = convList.mid(0, MAX_THREADS);
    } else {
        m_backend->log(QString("🧵 엮을 스레드 %1개(전체 %2개) — 재구성 시작.").arg(convList.size()).arg(newestSelfReply.size()),
                       "success", "twitter");
    }

    const QJsonArray accounts = config["accounts"].toArray();
    int done = 0;
    for (const QString &conv : convList) {
        if (!isRunning) break;
        // 스레드의 첫 글 — 가장 오래된 자기-답글이 답한 이 사람의 글. 대화 뿌리가 남의 글(남에게 단 답글에 이어 쓴 스레드)이어도
        //   이 사람의 글에서 시작해 이 사람의 글만 담는다.
        const QString head = threadHead.value(conv).isEmpty() ? conv : threadHead.value(conv);
        const QString rootUrl = QString("https://x.com/%1/status/%2").arg(target, head);
        // 기존 thread 재구성 경로 재사용. 'all' 패턴대로 매 호출 전 daemon 재설정.
        if (!accounts.isEmpty()) {
            setupClientFor(accounts[0].toObject());
            if (!startDaemon()) initTransactionIds();
        }
        QJsonObject tcfg = config;
        tcfg["type"] = QString("thread");
        tcfg["target"] = rootUrl;
        tcfg["_subCall"] = true;
        tcfg["_threadAuthor"] = target;
        m_lastThreadClean = false;
        collect(tcfg, isRunning);
        done++;
        if (m_lastThreadClean && isRunning) {
            threadsDone[conv] = QString::number(newestSelfReply.value(conv));
            if (!writeJsonFile(donePath, threadsDone))
                m_backend->log("다 엮은 스레드 목록을 적지 못했습니다: " + donePath + " — 다음 판에 다시 엮습니다", "warning", "twitter");
        }
    }
    m_backend->log(QString("🧵 스레드 자동탐지 완료: %1개 재구성.").arg(done), "success", "twitter");
}

void TwitterCollector::collect(const QJsonObject &config, const std::atomic<bool> &isRunning)
{
    m_runFlag = &isRunning;
    // ★ 이 워커의 HttpClient 로 갈아 끼운다 — adoptHttpToCurrentThread 설명 참고.
    //   collect() 가 자기를 다시 부르는 경우(전체 모드의 하위 수집, 스레드 자동탐지)에도
    //   같은 워커라 그대로 맞다. m_http 는 downloadMedia() 에서만 꺼내 쓰므로 옛 포인터를
    //   쥐고 있는 곳이 없다.
    adoptHttpToCurrentThread();

    // 중지 시 진행 중인 미디어 다운로드를 즉시 끊기 위해 HttpClient에 '진행 플래그' 연결
    if (m_http) m_http->setRunFlag(&isRunning);

    QString target = config["target"].toString().replace("@", "");
    QString threadFocalId;  // 스레드 모드: 시작 트윗 ID
    // 스레드 모드: target이 URL이면 screen_name / tweet_id 로 분해
    if (config["type"].toString() == "thread") {
        QRegularExpression rx(R"((?:x\.com|twitter\.com)/([A-Za-z0-9_]+)/status/(\d+))");
        auto m = rx.match(target);
        if (m.hasMatch()) {
            target = m.captured(1);
            threadFocalId = m.captured(2);
        } else {
            // 숫자만 있으면 tweet ID로 간주, target은 비워둠
            QRegularExpression idRx("^\\d+$");
            if (idRx.match(target.trimmed()).hasMatch()) {
                threadFocalId = target.trimmed();
                target = "thread_"+ threadFocalId;
            }
        }
    }
    // URL/ID 기반 타입은 target을 안전한 폴더명으로 변환
    {
        QString _t = config["type"].toString();
        QString rawTarget = config["target"].toString();
        if (_t == "favoriters"|| _t == "retweeters") {
            QRegularExpression rx(R"((?:x\.com|twitter\.com)/[A-Za-z0-9_]+/status/(\d+))");
            auto mm = rx.match(rawTarget);
            QString id = mm.hasMatch() ? mm.captured(1) : rawTarget.trimmed();
            id.remove(QRegularExpression("[^0-9]"));
            if (id.isEmpty()) id = "tweet";
            target = _t + "_"+ id;
        } else if (_t == "list") {
            QRegularExpression rx(R"(/i/lists/(\d+))");
            auto mm = rx.match(rawTarget);
            QString id = mm.hasMatch() ? mm.captured(1) : rawTarget.trimmed();
            id.remove(QRegularExpression("[^0-9]"));
            if (id.isEmpty()) id = "list";
            target = "list_"+ id;
        } else if (_t == "community") {
            QRegularExpression rx(R"(/i/communities/(\d+))");
            auto mm = rx.match(rawTarget);
            QString id = mm.hasMatch() ? mm.captured(1) : rawTarget.trimmed();
            id.remove(QRegularExpression("[^0-9]"));
            if (id.isEmpty()) id = "community";
            target = "community_"+ id;
        }
    }
    QString savePath = config["path"].toString();
    savePath.replace("~", QDir::homePath());
    QJsonArray accounts = config["accounts"].toArray();
    QString mode = config["mode"].toString("all");

    // "all"모드에서 재귀 호출 시 _subCall=true → 상태 초기화 건너뜀
    bool isSubCall = config["_subCall"].toBool(false);
    // 판이 끝나면 엑셀 줄 기억을 놓는다 — 수집기는 '새 트윗 확인' 때문에 다음 판까지 남아, 큰 계정이면 수백 MB 를 쥐고 있었다(검토)
    struct XlCacheRelease {
        TwitterCollector *c; bool on;
        ~XlCacheRelease() { if (on) { c->m_xlCacheRows.clear(); c->m_xlCacheRows.squeeze(); c->m_xlCachePath.clear(); } }
    } _xlCacheRelease{this, !isSubCall};

    if (!isSubCall) {
        // 새트윗 확인용 변수 초기화 (최상위 호출만)
        m_currentTarget = target;
        m_newestTweetId.clear();
        m_excelSaveFailed = false;
        m_xlCachePath.clear(); m_xlCacheRows.clear(); m_xlCacheSize = -1; m_xlCacheMtimeMs = 0;
        m_xlSinceSave.invalidate(); m_xlLastSaveCostMs = 0;
        // ★ multi-target 큐: 직전 target에서 누적된 RL 추적 상태가 새 target/계정에 잘못 적용되지 않도록 reset
        m_accountRateLimitTime.clear();
        m_rateLimitHits = 0;
        // ★ '전체' 판이 중지 · 오류로 중간에 돌아가면 아래의 '이 판까지만' 정리가 돌지 않는다 — 다음 대상(다른 계정)이
        //   그 계정의 ID · 표시를 제 것으로 쓰지 않게 최상위 판마다 비운다
        m_allKnownSnapshotSet = false;
        m_allKnownSnapshot.clear();
        m_allArmedLists.clear();
        // ★ 계정 상태 · 레이트 리밋 머리는 판마다 새로(r56) — '전체' 의 하위 판은 나눠 쓴다(계정마다 account_status 한 번)
        m_accountChecked.clear();
        m_searchBlocked.clear();
        m_searchAllBlockedLogged = false;
        m_accountFreeAt.clear();
        m_rlRemaining = -1;
        m_rlReset = 0;
        m_rlAction.clear();
        m_runReplyIds.clear();   // Phase 4 가 읽을 이번 판의 답글 — '전체' 의 하위 판끼리는 나눠 쓴다
    }
    if (!isSubCall) {
        // ★ r56 — 카드용 미디어 색인은 판마다 새로 만든다(그사이 사람이 옮기거나 지운 파일을 붙이지 않게)
        m_cardMediaIndex.clear();
        // ★ r56 — 지난번에 저장 폴더로 못 옮긴 진짜 캡쳐(임시 디스크 .abiwa_cap)를 먼저 제 자리로 옮긴다. 옆 기록이 없는
        //   옛 것은 이 트위터 저장 경로(<경로>/twitter/*/captures) 안에서만 바꿀 카드를 찾는다.
        if (!savePath.isEmpty()) m_backend->deliverStagedCaptures(savePath + "/twitter");
    }

    // Adaptive delay — user no longer sets this manually
    m_delay = 2.0;  // base delay, auto-adjusts on rate limits
    m_consecutiveOk = 0;
    m_mediaQuality = config["mediaQuality"].toString("orig");
    m_downloadMedia = config["downloadMedia"].toBool(true);
    m_saveExcel = config["excel"].toBool(true);
    m_saveExif = config["exif"].toBool(true);
    m_saveProgress = config["saveProgress"].toBool(false);  // 이어서 수집 기능 (체크박스)
    // ★ 최대 수집 수 — 화면의 '최대 수집 수(0 = 전체)'. 이번 판에 새로 받은 것만 센다.
    //   전에는 이 값을 어디서도 읽지 않아 8 을 넣어도 406개를 끝까지 받았다(실측 2026-09-13).
    //   트윗·좋아요·북마크·리포스트·하이라이트·리스트·커뮤니티·좁히기·스레드·좋아요/RT 한 유저에 걸린다.
    const int maxCount = qMax(0, config["count"].toInt(0));
    // ★ 다 받은 곳에서 일찍 멈추기(사용자 2026-10-24 — 대상이 수백 개일 때, gallery-dl 의 abort:N).
    //   이번 판을 시작하기 '전' 에 받아 둔 트윗이 이만큼 이어지면 그 단계를 끝낸다(0 = 끝까지 — 예전 그대로).
    //   전체(all)의 하위 종류는 makeSubConfig 가 config 를 그대로 넘겨 같은 값을 쓴다.
    const int stopAfterKnown = qMax(0, config["stopAfterKnown"].toInt(0));

    if (accounts.isEmpty()) {
        m_backend->log("Add account first", "error", "twitter");
        return;
    }

    QString twitterDir = savePath + "/twitter";
    QDir().mkpath(twitterDir);
    QString userDir = twitterDir + "/"+ target;
    QString mediaDir = userDir + "/media";
    QDir().mkpath(mediaDir);

    if (!isSubCall) {
        m_downloadedProfiles.clear();
        // Create disk-based profile buffer (no RAM bloat for 30000+ profiles)
        QString profTempDir = userDir + "/.tmp_profiles";
        QDir().mkpath(profTempDir);
        if (m_profileBuffer) { delete m_profileBuffer; m_profileBuffer = nullptr; }

        // ★ 앞선 판이 남긴 고아 임시 파일을 치운다.
        //   소멸자가 지우게 고쳤지만, 전원이 나가거나 앱이 강제 종료되면 소멸자가
        //   돌지 않는다. 1년을 무인으로 돌 앱에서는 그쪽이 오히려 흔하다.
        //
        //   ★★ 지우는 것은 '빈 파일' 뿐이다. 내용이 한 줄이라도 있으면 손대지 않는다.
        //     보통은 그 내용이 프로필 엑셀에도 들어가 있어 사본에 불과하다
        //     (실측: 남아 있던 22개가 전부 엑셀에도 있었다). 하지만 엑셀을 쓰기 전에
        //     끊긴 판이라면 그 파일에만 있는 자료가 된다. 그 경우를 파일 하나 크기로
        //     구별할 수 없으므로, 구별이 안 되면 지우지 않는다.
        //     빈 파일은 어느 경우에도 잃을 것이 없다.
        //
        //   한 시간 넘게 손대지 않은 것만 본다 — 지금 다른 수집이 막 만든 파일도
        //   아직 비어 있을 수 있어서다(같은 대상을 병렬로 돌리는 경우).
        {
            const QDir ptd(profTempDir);
            const QDateTime cutoff = QDateTime::currentDateTime().addSecs(-3600);
            int swept = 0, kept = 0;
            for (const QFileInfo &fi : ptd.entryInfoList({"tw_profiles_*.jsonl"}, QDir::Files)) {
                if (fi.lastModified() > cutoff) continue;
                if (fi.size() > 0) { ++kept; continue; }        // 내용이 있으면 그대로 둔다
                if (QFile::remove(fi.absoluteFilePath())) ++swept;
            }
            if (swept > 0 || kept > 0)
                m_backend->log(QString("이전에 남은 임시 파일 — 빈 것 %1개 치움, 내용 있는 것 %2개 그대로 둠")
                                   .arg(swept).arg(kept), "info", "twitter");
        }

        m_profileBuffer = new DiskJsonBuffer(profTempDir, "tw_profiles");
    }

    m_backend->log("Target: @"+ target, "info", "twitter");
    m_backend->log("Save: "+ userDir, "info", "twitter");

    // Setup first account
    QJsonObject firstAccount = accounts[0].toObject();
    setupClientFor(firstAccount);
    m_accounts = accounts;        // 검색 · 타임라인이 스스로 계정을 돌릴 때 쓴다(정지 계정 · 남은 요청 0 — r56)
    m_activeAccountIdx = 0;
    m_backend->log(QString("Account: %1 (1/%2)")
                       .arg(firstAccount["name"].toString("Twitter"))
                       .arg(accounts.size()),
                   "success", "twitter");

    // Start persistent Python daemon (twikit - handles TID per request)
    if (!startDaemon()) {
        m_backend->log("Failed to start twikit daemon, falling back to TID mode", "warning", "twitter");
        initTransactionIds();
    }

    // 일부 타입은 유저 대신 트윗/리스트/커뮤니티 ID 기반이라 user lookup 생략
    QString _prelimType = config["type"].toString("tweets");
    bool skipUserLookup = (_prelimType == "favoriters"|| _prelimType == "retweeters"||
                           _prelimType == "list"|| _prelimType == "community");
    // ★ 검색을 쓰는 종류면 데몬이 뜬 바로 뒤 이 계정의 X 상태를 묻는다(판마다 계정당 한 번 — 정지면 검색에서 뺀다, r56).
    //   다른 계정은 돌려 쓸 때 searchTweets 가 처음 묻는다.
    if (_prelimType == "tweets" || _prelimType == "media" || _prelimType == "replies" || _prelimType == "narrow"
        || _prelimType == "all" || (_prelimType == "tweets_api" && mode == "period"))
        checkAccountStatus(0);

    QJsonObject user;
    if (!skipUserLookup) {
        user = getUserByScreenName(target);
        if (user.isEmpty()) {
            m_backend->log("사용자를 찾을 수 없습니다: @"+ target, "error", "twitter");
            m_backend->log("아이디를 확인해주세요.", "info", "twitter");
            m_backend->log("Complete.", "info", "twitter");
            if (isSubCall) m_subUserMissing = true;   // '전체' 가 다음 하위 판으로 가지 않게(예전 user2 확인)
            else stopDaemon();
            return;
        }
    }

    QJsonObject userLegacy = xUserLegacy(user);   // 2026-08 legacy 없는 모양도 채운다
    // 대상이 고정한 트윗 — 검색이 먼저 받아도 엑셀 종류 앞에 📌 가 붙게(standardTweetRow, r56)
    m_pinnedIds.clear();
    m_pinnedKnown = userLegacy.contains(QStringLiteral("pinned_tweet_ids_str"));   // 칸이 없으면(모양이 바뀜) 있던 📌 를 지우지 않는다
    for (const auto &pv : userLegacy.value(QStringLiteral("pinned_tweet_ids_str")).toArray())
        if (!pv.toString().isEmpty()) m_pinnedIds.insert(pv.toString());
    QString userName = userLegacy["name"].toString();
    QString userId = user["rest_id"].toString();
    int statusesCount = userLegacy["statuses_count"].toInt();
    int followersCount = userLegacy["followers_count"].toInt();
    int followingCount = userLegacy["friends_count"].toInt();
    if (!skipUserLookup) {
        m_backend->log(QString("User: %1 (@%2)").arg(userName, target), "success", "twitter");
        m_backend->log(QString("Tweets: %1 | Followers: %2 | Following: %3").arg(statusesCount).arg(followersCount).arg(followingCount), "info", "twitter");
    }

    QString type = config["type"].toString("tweets");

    // ── ALL: 전체 수집 (트윗 + 답글 + 좋아요 + 리포스트 + 팔로워 + 팔로잉 + 프로필) ──
    if (type == "all") {
        m_backend->log("═══ 전체 수집 모드 ═══", "success", "twitter");
        m_spaceCardTweetUrls.clear();   // 이번 전체수집에서 발견할 스페이스 카드 트윗 누적용 초기화

        // 1. Profile
        m_backend->log("[1/7] 프로필 수집...", "info", "twitter");
        {
            QJsonObject subConfig = config;
            subConfig["type"] = QString("profile");
            // Inline profile save (skip daemon restart)
            ExcelWriter writer;
            QStringList profileHdrs = {"Field", "Value"};
            writer.writeHeader(profileHdrs, QColor("#d97757"));
            int prow = 2;
            auto addRow = [&](const QString &field, const QString &value) {
                writer.writeRow(prow++, {field, value});
            };
            addRow("ID", userId);
            addRow("Handle", "@"+ target);
            addRow("Name", userName);
            addRow("Bio", userLegacy["description"].toString());
            addRow("Location", userLegacy["location"].toString());
            addRow("URL", userLegacy["url"].toString());
            addRow("Verified", user["is_blue_verified"].toBool() ? "Yes": "No");
            addRow("Protected", userLegacy["protected"].toBool() ? "Yes": "No");
            addRow("Followers", QString::number(followersCount));
            addRow("Following", QString::number(followingCount));
            addRow("Tweets", QString::number(statusesCount));
            addRow("Likes", QString::number(userLegacy["favourites_count"].toInt()));
            addRow("Created", Common::formatDateJapanese(userLegacy["created_at"].toString()));
            // Birthday (if available from legacy_extended_profile)
            {
                QJsonObject extProfile = user["legacy_extended_profile"].toObject();
                QJsonObject birthdate = extProfile["birthdate"].toObject();
                if (!birthdate.isEmpty()) {
                    QString bday;
                    int day = birthdate["day"].toInt();
                    int month = birthdate["month"].toInt();
                    int year = birthdate["year"].toInt();
                    if (year > 0) bday = QString("%1年%2月%3日").arg(year).arg(month).arg(day);
                    else if (month > 0 && day > 0) bday = QString("%1月%2日").arg(month).arg(day);
                    if (!bday.isEmpty()) addRow("Birthday", bday);
                }
            }
            addRow("Profile Image", userLegacy["profile_image_url_https"].toString().replace("_normal", "_400x400"));
            addRow("Banner", userLegacy["profile_banner_url"].toString());
            writer.setColumnWidth(1, 20);
            writer.setColumnWidth(2, 60);

            // 통일된 프로필 폴더: profiles/target/{이름(@handle)}/
            QString dateTag = Common::jstToday().toString("yyyyMMdd");
            QString tDisplayName = userName;
            tDisplayName.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
            tDisplayName = tDisplayName.trimmed().left(40);
            QString tFilePrefix = !tDisplayName.isEmpty() ? QString("%1(@%2)").arg(tDisplayName, target) : target;
            tFilePrefix = FileHelper::portableName(tFilePrefix);   // FileHelper.h 설명
            QString tDirName = tFilePrefix;
            tDirName.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
            tDirName = FileHelper::reuseExistingName(userDir + "/profiles/target", tDirName);
            QString targetProfileDir = userDir + "/profiles/target/"+ tDirName;
            QDir().mkpath(targetProfileDir);

            QString pProfilePath = targetProfileDir + "/"+ target + "_profile.xlsx";
            writer.save(pProfilePath);
            FileHelper::setDownloadMeta(pProfilePath, QStringLiteral(APP_NAME_DISPLAY) + QStringLiteral(" Twitter"));
            // 프로필 사진(원본 먼저) · 배너 — 주소가 바뀌었을 때만 새로 받는다(saveProfileImage 설명)
            const auto dl = [this](const QString &u, const QString &f) { return downloadMedia(u, f); };
            const QString avatarUrl = userLegacy["profile_image_url_https"].toString();
            saveProfileImage(targetProfileDir, QStringLiteral("profile"), avatarUrl, avatarCandidates(avatarUrl), dateTag, dl, avatarLegacyUrl(avatarUrl));
            QString bannerUrl = userLegacy["profile_banner_url"].toString();
            if (!bannerUrl.isEmpty())
                saveProfileImage(targetProfileDir, QStringLiteral("banner"), bannerUrl, {bannerUrl + "/1500x500", bannerUrl}, dateTag, dl);
            m_backend->log("프로필 완료 ", "success", "twitter");
        }
        if (!isRunning) { stopDaemon(); return; }

        // Helper lambda (unused — kept for reference)
        auto runSubCollect = [&](const QString &subType, const QString &label, int step) {
            m_backend->log(QString("[%1/8] %2 수집...").arg(step).arg(label), "info", "twitter");
        };

        // Helper: "all"재귀 호출 시 _subCall=true → 상태 초기화 건너뜀
        // 하위 판(트윗 → 답글 …)이 같은 _complete 엑셀을 쓴다 — 답글 판이 방금 트윗 판이 받은 것을 '이미 받은' 으로 세어
        //   일찍 멈추지 않게, 이 '전체' 판을 시작하기 전의 ID 를 한 번 찍어 둔다(검토)
        m_allKnownSnapshot = stopAfterKnown > 0
            ? excelIdsAt(userDir + "/" + target + "_complete.xlsx") + excelIdsAt(userDir + "/" + target + "_tweets.xlsx")
            : QSet<QString>();   // 일찍 멈춤을 끄면 쓰지 않는다 — 큰 엑셀을 한 번 더 읽지 않는다
        m_allKnownSnapshotSet = true;
        m_allArmedLists.clear();   // 그 엑셀의 표시도 여기서 한 번에 가져간다 — 하위 판은 다 본 목록만 다시 적는다
        for (const QString &k : Common::takeCleanWalkMarks(userDir, "tw_tweets_")) m_allArmedLists.insert(k.mid(10));
        // 옛 _tweets.xlsx 만 있는 폴더 — 트윗 하위 판처럼 표시를 믿지도 되돌리지도 않는다(새 _complete 에 이번 것만 담긴다 — 검토)
        if (!QFile::exists(userDir + "/" + target + "_complete.xlsx") && QFile::exists(userDir + "/" + target + "_tweets.xlsx"))
            m_allArmedLists.clear();
        // 하위 판이 다시 적지 않은 것(끊긴 걸음 · 걷지 않은 목록 · 중간에 돌아감)은 끝에 읽은 그대로 되돌린다 — top 위로 들어온 줄은 세지 않는다
        ScopeExit allRestore{[userDir, lists = m_allArmedLists]() {
            for (const QString &l : lists) Common::restoreWalkMark(userDir, "tw_tweets_" + l);
        }};
        auto makeSubConfig = [&](const QString &subType) -> QJsonObject {
            QJsonObject subConfig = config;
            subConfig["type"] = subType;
            subConfig["_subCall"] = true;
            return subConfig;
        };

        // ★ 하위 판 사이에 데몬을 다시 띄우지 않는다(r56 — 속도 점검 F4). 예전엔 하위 판마다 끝에 데몬을 끄고, 여기서 다시 띄우고
        //   (계정 다시 걸기 · GraphQL 해시 다시 긁기 — x.com 첫 쪽 + JS 묶음 최대 20개) 사용자를 다시 찾은 뒤, 하위 판이 또 같은 일을
        //   했다(대상 하나에 데몬 시작 약 14번 · 같은 사용자 찾기 약 14번). 이제 하위 판은 데몬을 켜 둔 채 돌아오고, 다음 하위 판이
        //   맨 앞에서 0번 계정 · 그 출구를 다시 건다 — 계정이나 출구가 바뀌었으면 그때 데몬을 새로 띄운다(setupClient · setupClientFor).
        //   사용자 찾기는 하위 판이 한다. 찾지 못하면 그 판이 m_subUserMissing 을 세워 '전체' 를 멈춘다(예전의 user2 확인과 같다).
        //   '전체' 가 어디서 돌아가든 마지막에 데몬을 끈다.
        m_subUserMissing = false;
        ScopeExit allDaemonStop{[this]() { stopDaemon(); }};
        auto subAborted = [&]() { return !isRunning || m_subUserMissing; };

        // 2. Tweets
        m_backend->log("[2/8] 트윗 수집...", "info", "twitter");
        collect(makeSubConfig("tweets"), isRunning);
        if (subAborted()) return;

        // 3. Replies
        m_backend->log("[3/8] 답글 수집...", "info", "twitter");
        collect(makeSubConfig("replies"), isRunning);
        if (subAborted()) return;

        // 4. Likes
        m_backend->log("[4/8] 좋아요 수집...", "info", "twitter");
        collect(makeSubConfig("likes"), isRunning);
        if (subAborted()) return;

        // 5. Reposts
        m_backend->log("[5/8] 리포스트 수집...", "info", "twitter");
        collect(makeSubConfig("reposts"), isRunning);
        if (subAborted()) return;

        // ── 리포스트를 _complete.xlsx에도 추가 ──
        {
            // _reposts.xlsx는 이제 complete 형식이므로 직접 읽어서 complete에 병합
            // ★ 모든 칸을 옮긴다(r56, COV-9) — 예전엔 1~17번 칸만 옮겨 해시태그 · 링크 · 미디어 · 인용 · 작성자 정보가 _complete 에서 비었다.
            //   칸은 머리 이름으로 찾는다(옛 30칸 파일이면 새 칸은 빈 채로). 머리를 못 알아보면 차례대로 읽는다.
            QString repostsPath = userDir + "/"+ target + "_reposts.xlsx";
            if (QFile::exists(repostsPath)) {
                QXlsx::Document rpDoc(repostsPath);
                int rpLastRow = rpDoc.dimension().lastRow();
                int rpColCount = rpDoc.dimension().lastColumn();
                QStringList rpCols;   // 칸 번호(0부터) → 줄 객체의 열쇠
                for (int c = 1; c <= rpColCount; ++c) rpCols << rpDoc.read(1, c).toString().trimmed();
                if (!rpCols.contains(QStringLiteral("id")) || !rpCols.contains(QStringLiteral("tweet_url"))) rpCols = tweetColumns();

                QString rpTmp = userDir + "/.tmp";
                QDir().mkpath(rpTmp);
                DiskJsonBuffer rpBuf(rpTmp, "tw_rp_merge");

                for (int r = 2; r <= rpLastRow; ++r) {
                    QString rpId = rpDoc.read(r, 1).toString().trimmed();
                    if (rpId.isEmpty() || rpId.startsWith("─") || rpId.startsWith("===")) continue;

                    QJsonObject d;
                    for (int c = 0; c < rpCols.size(); ++c) {
                        const QString &key = rpCols[c];
                        if (key.isEmpty() || !tweetColumns().contains(key)) continue;
                        const QString v = rpDoc.read(r, c + 1).toString();
                        if (tweetIntColumn(key)) d[key] = v.toInt();
                        else d[key] = v;
                    }
                    d["id"] = rpId;
                    rpBuf.append(d);
                }

                if (rpBuf.count() > 0) {
                    m_backend->log(QString("리포스트 %1개를 complete 파일에 추가...").arg(rpBuf.count()), "info", "twitter");
                    // 타임라인 표시를 적은 뒤에 넣는 리트윗이라도, 그 top 보다 새것은 다음 타임라인 걸음이 세지 않는다(top 아래는 이미 받아 둔 곳)
                    saveExcelStreaming(userDir, target, rpBuf, "tweets");
                }
            }
        }

        // 6. Thread Comments (게시물 댓글)
        if (config["threadComments"].toBool(false)) {
            m_backend->log("[6/8] 게시물 댓글 수집...", "info", "twitter");
            collect(makeSubConfig("thread_comments"), isRunning);
            if (subAborted()) return;
        } else {
            m_backend->log("[6/8] 게시물 댓글 — 건너뜀 (체크박스 해제)", "info", "twitter");
        }

        // 7. Followers
        m_backend->log("[7/8] 팔로워 수집...", "info", "twitter");
        collect(makeSubConfig("followers"), isRunning);
        if (subAborted()) return;

        // 8. Following
        m_backend->log("[8/8] 팔로잉 수집...", "info", "twitter");
        collect(makeSubConfig("following"), isRunning);

        // 9. 🎙️ 스페이스 자동탐지 (타임라인에 공유된 스페이스 링크 → yt-dlp)
        collectSpacesFromTimeline(config, target, userDir, isRunning);

        // 10. 🧵 스레드 자동탐지 (자기-답글 체인 루트 → thread 재구성)
        collectThreadsAuto(config, target, userDir, isRunning);

        // 11. 🔎 포스트 좁히기 자동 — '전체 시 좁히기 키워드'가 설정된 경우에만 실행.
        //     (키워드 없으면 step 2 의 from:user 전수 백필과 중복이므로 건너뜀)
        if (isRunning && !config["narrowQuery"].toString().trimmed().isEmpty()) {
            m_backend->log(QString("[자동] 🔎 포스트 좁히기: \"%1\"").arg(config["narrowQuery"].toString().trimmed()), "info", "twitter");
            collect(makeSubConfig("narrow"), isRunning);
        }

        m_backend->log("═══ 전체 수집 완료! ═══", "success", "twitter");
        m_allKnownSnapshotSet = false;   // 이 '전체' 판의 찍어 둔 ID 는 여기까지만
        return;
    }

    // ── Followers / Following (separate flow) ──
    if (type == "followers"|| type == "following") {
        m_backend->log(QString("Collecting %1...").arg(type), "info", "twitter");

        QString twTempDir = config["tempDir"].toString();
        if (twTempDir.isEmpty()) twTempDir = userDir + "/.tmp";
        DiskJsonBuffer allUsers(twTempDir, "tw_"+ type);
        QString cursor;
        int currentAccountIdx = 0;

        // ★ 최대 수집 수 — 목록도 그 개수에서 멈춘다. 빠져 있어서 '전체' 로 3개를 주면 팔로워 목록을
        //   끝까지 모으고 한 명씩 프로필 사진·배너까지 받았다(실측 2026-09-19: 14초에 2,208파일).
        auto usersCapped = [&]() { return maxCount > 0 && allUsers.count() >= maxCount; };
        // ★ 오류 한 번에 목록이 잘려 이전의 온전한 엑셀을 덮던 것(r56, 깊이 점검 F5) — 오류(데몬 오류 · 200 이 아닌 답 · 모양 없는 쪽)는
        //   쉬었다가(5 · 10 · 20 · 40 · 60초) 5번까지 다시 묻는다. 그래도 끊기면(또는 중지 · 최대 수) '일부만 받음' 으로 보고, 아래에서
        //   기존 엑셀과 합친다(옛 줄은 남긴다). 끝까지 받았을 때만 새 목록으로 바꾼다(그래야 언팔로우한 사람이 빠진다).
        int ffErrTries = 0;
        bool ffBroken = false;
        auto ffRetry = [&](const QString &why) -> bool {   // true = 다시 묻는다 · false = 그만(끊김)
            if (++ffErrTries > 5) {
                m_backend->log(QString("%1: %2 — 5번 다시 물어도 안 돼 여기서 멈춥니다(받은 만큼은 기존 엑셀과 합칩니다)").arg(type, why), "error", "twitter");
                ffBroken = true;
                return false;
            }
            const int waitS = qMin(60, 5 << (ffErrTries - 1));
            m_backend->log(QString("%1: %2 — %3초 뒤 다시 묻습니다(%4/5)").arg(type, why).arg(waitS).arg(ffErrTries), "warning", "twitter");
            for (int w = waitS; w > 0 && isRunning; --w) QThread::sleep(1);
            return isRunning;
        };
        while (isRunning && !usersCapped()) {
            QJsonObject cmd;
            cmd["action"] = type;  // "followers" or "following"
            cmd["user_id"] = userId;  // ★ BUG: 이전엔 주석 끝에 붙어서 실행 안 됨
            cmd["count"] = 40;
            if (!cursor.isEmpty()) cmd["cursor"] = cursor;

            QJsonObject resp = sendDaemonCommand(cmd);
            if (resp.contains("error")) {
                m_backend->log(type + "error: "+ resp["error"].toString(), "error", "twitter");
                if (ffRetry(QStringLiteral("도우미 오류"))) continue;
                break;
            }

            int status = resp["status"].toInt();
            if (status == 429) {
                handleRateLimit(accounts, currentAccountIdx, isRunning);
                continue;
            }
            if (status != 200) {
                m_backend->log(QString("API error: %1").arg(status), "error", "twitter");
                if (ffRetry(QString("HTTP %1").arg(status))) continue;
                break;
            }

            QJsonObject data = resp["body"].toObject();
            QJsonArray instructions = xInstructions(data, {"user", "user_result"});
            if (instructions.isEmpty()) {   // 200 인데 목록 모양이 아니다(errors 만 · 빈 답) — 목록 끝이 아니라 막힌 답이다
                // 첫 쪽부터 그렇다면 볼 수 없는 목록(잠긴 계정 등)이다 — 예전처럼 바로 끝낸다(받은 것이 없어 엑셀은 건드리지 않는다)
                if (cursor.isEmpty() && allUsers.count() == 0) {
                    m_backend->log(QString("%1: 목록을 볼 수 없습니다(잠긴 계정이거나 X 가 막았습니다) — 건너뜁니다").arg(type), "warning", "twitter");
                    break;
                }
                if (ffRetry(QStringLiteral("목록 모양이 아닌 답"))) continue;
                break;
            }
            ffErrTries = 0;

            bool foundEntries = false;
            QString nextCursor;
            for (const auto &inst : instructions) {
                QJsonObject instruction = inst.toObject();
                if (instruction["type"].toString() == "TimelineAddEntries") {
                    QJsonArray entries = instruction["entries"].toArray();
                    for (const auto &entry : entries) {
                        QJsonObject e = entry.toObject();
                        QString eid = e["entryId"].toString();
                        if (eid.startsWith("user-")) {
                            QJsonObject userResult = xUserResult(e);
                            if (!userResult.isEmpty() && !usersCapped()) {
                                allUsers.append(userResult);
                                foundEntries = true;
                            }
                        } else if (eid.startsWith("cursor-bottom")) {
                            nextCursor = e["content"].toObject()["value"].toString();
                        }
                    }
                }
            }

            m_backend->updateStats(allUsers.count(), 0, "수집 중...", "twitter");
            m_backend->log(QString("%1: %2명").arg(type).arg(allUsers.count()), "info", "twitter");

            if (nextCursor.isEmpty() || !foundEntries) break;
            cursor = nextCursor;
            m_consecutiveOk++; if (m_consecutiveOk > 5) { m_delay = qMax(m_delay * 0.9, 1.0); m_rateLimitHits = 0; }
            QThread::msleep(static_cast<unsigned long>(m_delay * 1000));
        }

        // Download profile pics & banners — downloadUserProfileMedia() 통일 방식
        if (allUsers.count() > 0) {
            QString profileDir = userDir + "/profiles";
            m_backend->log(QString("프로필/배너 다운로드 중... (%1명)").arg(allUsers.count()), "info", "twitter");
            m_downloadedProfiles.clear(); // 이번 세션 중복 방지 리셋
            allUsers.resetReader();
            QJsonObject pu;
            int dlCount = 0;
            while (allUsers.readNext(pu)) {
                if (!isRunning) break;
                QJsonObject pLeg = xUserLegacy(pu);
                QJsonObject pCore = pu["core"].toObject();
                QString handle = pLeg["screen_name"].toString();
                if (handle.isEmpty()) handle = pCore["screen_name"].toString();
                if (handle.isEmpty()) handle = pu["screen_name"].toString();
                if (handle.isEmpty()) continue;

                if (m_downloadedProfiles.contains(handle)) continue;
                m_downloadedProfiles.insert(handle);

                QString displayName = pLeg["name"].toString();
                if (displayName.isEmpty()) displayName = pCore["name"].toString();
                if (displayName.isEmpty()) displayName = pu["name"].toString();
                displayName.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
                displayName = displayName.trimmed().left(40);
                QString filePrefix = !displayName.isEmpty() ? QString("%1(@%2)").arg(displayName, handle) : handle;
                filePrefix = FileHelper::portableName(filePrefix);   // FileHelper.h 설명

                // 유저별 서브폴더: profiles/{type}/{이름(@handle)}/
                QString userDirName = filePrefix;
                userDirName.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
                // 표시이름 대소문자만 바뀐 계정이 폴더를 하나 더 만들지 않게(실측 4묶음이 여기서 났다)
                userDirName = FileHelper::reuseExistingName(profileDir + "/"+ type, userDirName);
                QString userProfileDir = profileDir + "/"+ type + "/"+ userDirName;
                QDir().mkpath(userProfileDir);

                QString dateTag = Common::jstNow().toString("yyyyMMdd");

                // Profile image (400x400) — 새 API 구조 대응
                QString profUrl = pLeg["profile_image_url_https"].toString();
                if (profUrl.isEmpty()) profUrl = pCore["profile_image_url_https"].toString();
                if (profUrl.isEmpty()) profUrl = pu["profile_image_url_https"].toString();
                if (profUrl.isEmpty()) {
                    QJsonValue avatarVal = pu["avatar"];
                    if (avatarVal.isObject()) profUrl = avatarVal.toObject()["image_url"].toString();
                    else if (avatarVal.isString()) profUrl = avatarVal.toString();
                }
                // 원본 먼저 · 주소가 바뀌었을 때만 새로 받는다(saveProfileImage 설명). 엑셀 칸에는 예전처럼 400x400 주소
                const auto dl = [this](const QString &u, const QString &f) { return downloadMedia(u, f); };
                saveProfileImage(userProfileDir, QStringLiteral("profile"), profUrl, avatarCandidates(profUrl), dateTag, dl, avatarLegacyUrl(profUrl));
                if (!profUrl.isEmpty()) profUrl.replace("_normal", "_400x400");
                // Banner (1500x500)
                QString bannerUrl = pLeg["profile_banner_url"].toString();
                if (!bannerUrl.isEmpty())
                    saveProfileImage(userProfileDir, QStringLiteral("banner"), bannerUrl, {bannerUrl + "/1500x500", bannerUrl}, dateTag, dl);

                // Excel 용 프로필 데이터 수집
                if (m_profileBuffer) {
                    QJsonObject profileInfo;
                    profileInfo["screen_name"] = handle;
                    profileInfo["name"] = displayName;
                    profileInfo["description"] = pLeg["description"].toString();
                    profileInfo["followers_count"] = pLeg["followers_count"].toInt();
                    profileInfo["friends_count"] = pLeg["friends_count"].toInt();
                    profileInfo["statuses_count"] = pLeg["statuses_count"].toInt();
                    profileInfo["location"] = pLeg["location"].toString();
                    profileInfo["url"] = pLeg["url"].toString();
                    profileInfo["created_at"] = Common::formatDateJapanese(pLeg["created_at"].toString());
                    profileInfo["verified"] = pu["is_blue_verified"].toBool() ? "True": "False";
                    profileInfo["profile_image_url"] = profUrl;
                    profileInfo["profile_banner_url"] = bannerUrl;
                    m_profileBuffer->append(profileInfo);
                }

                dlCount++;
                if (dlCount % 50 == 0) {
                    m_backend->updateStats(allUsers.count(), dlCount, "프로필 다운 중...", "twitter");
                }
            }
            m_backend->log(QString("프로필/배너 %1명 다운 완료").arg(dlCount), "success", "twitter");
        }

        // Save Excel (streaming — no readAll)
        // ★ 끝까지 받지 못한 판(오류로 끊김 · 중지 · 최대 수)은 기존 엑셀을 덮지 않고 합친다 — 새로 받은 사람(새 값) + 기존 엑셀에만
        //   있는 사람(ID 로 가른다). 예전엔 3쪽째 오류 한 번에 1만 명 엑셀이 120명으로 줄었다(r56, F5).
        const bool ffPartial = ffBroken || !isRunning || usersCapped();
        const QString ffPath = userDir + "/"+ target + "_"+ type + ".xlsx";
        QList<QStringList> ffKeep;   // 합칠 기존 줄
        bool ffSkipSave = false;
        if (allUsers.count() > 0 && ffPartial && QFile::exists(ffPath)) {
            QXlsx::Document old(ffPath);
            if (!old.isLoadPackage()) {
                ffSkipSave = true;
                m_backend->log(QString("%1 목록을 끝까지 받지 못했고 기존 엑셀도 읽지 못해 저장하지 않습니다(기존 파일은 그대로): %2").arg(type, ffPath), "error", "twitter");
            } else {
                QSet<QString> newIds;
                allUsers.resetReader();
                QJsonObject nu;
                while (allUsers.readNext(nu)) newIds.insert(nu["rest_id"].toString());
                const int lastRow = old.dimension().lastRow();
                for (int r = 2; r <= lastRow; ++r) {
                    const QString oid = old.read(r, 1).toString().trimmed();
                    if (oid.isEmpty() || newIds.contains(oid)) continue;
                    QStringList cells;
                    for (int c = 1; c <= 14; ++c) cells << old.read(r, c).toString();
                    ffKeep << cells;
                }
                m_backend->log(QString("%1 목록을 끝까지 받지 못해 일부만 받았습니다 — 기존 엑셀의 %2명은 그대로 두고 새로 받은 %3명을 합칩니다")
                                   .arg(type).arg(ffKeep.size()).arg(allUsers.count()), "warning", "twitter");
            }
        } else if (ffPartial && allUsers.count() > 0) {
            m_backend->log(QString("%1 목록을 끝까지 받지 못해 일부(%2명)만 저장합니다").arg(type).arg(allUsers.count()), "warning", "twitter");
        }
        if (allUsers.count() > 0 && !ffSkipSave) {
            ExcelWriter writer;
            QStringList hdrs = {"ID", "Handle", "Name", "Profile URL", "Bio", "Followers", "Following", "Tweets", "Likes", "Created At", "Verified", "Location", "Profile Image", "Banner"};
            writer.writeHeader(hdrs, QColor("#d97757"));
            int row = 2;
            allUsers.resetReader();
            QJsonObject uo;
            while (allUsers.readNext(uo)) {
                QJsonObject leg = xUserLegacy(uo);
                QString handle = screenNameOf(uo);
                writer.writeRow(row++, {
                    uo["rest_id"].toString(),
                    handle,
                    leg["name"].toString(),
                    QString("https://x.com/%1").arg(handle),
                    leg["description"].toString().left(300),
                    QString::number(leg["followers_count"].toInt()),
                    QString::number(leg["friends_count"].toInt()),
                    QString::number(leg["statuses_count"].toInt()),
                    QString::number(leg["favourites_count"].toInt()),
                    Common::formatDateJapanese(leg["created_at"].toString()),
                    uo["is_blue_verified"].toBool() ? "Yes": "",
                    leg["location"].toString(),
                    leg["profile_image_url_https"].toString().replace("_normal", "_400x400"),
                    leg["profile_banner_url"].toString()
                });
            }
            writer.setColumnWidth(1, 20);   // ID
            writer.setColumnWidth(2, 18);   // Handle
            writer.setColumnWidth(3, 20);   // Name
            writer.setColumnWidth(4, 30);   // Profile URL
            writer.setColumnWidth(5, 40);   // Bio
            writer.setColumnWidth(6, 10);   // Followers
            writer.setColumnWidth(7, 10);   // Following
            writer.setColumnWidth(8, 8);    // Tweets
            writer.setColumnWidth(9, 8);    // Likes
            writer.setColumnWidth(10, 28);  // Created At
            writer.setColumnWidth(11, 8);   // Verified
            writer.setColumnWidth(12, 20);  // Location
            writer.setColumnWidth(13, 50);  // Profile Image URL
            writer.setColumnWidth(14, 50);  // Banner URL
            for (const QStringList &cells : std::as_const(ffKeep)) writer.writeRow(row++, cells);   // 끊긴 판 — 기존 엑셀에만 있던 사람(위 설명)
            writer.save(ffPath);
            FileHelper::setDownloadMeta(ffPath, QStringLiteral(APP_NAME_DISPLAY) + QStringLiteral(" Twitter"));
            m_backend->log("Excel 저장 완료", "success", "twitter");
        }

        m_backend->log(QString("완료! %1: %2명").arg(type).arg(allUsers.count()), "success", "twitter");
        m_backend->updateStats(allUsers.count(), 0, "완료", "twitter");
        if (!isSubCall) stopDaemon();   // '전체' 의 하위 판은 데몬을 켜 둔 채 돌아간다(r56 — '전체' 설명)
        return;
    }

    // ── Profile ──
    if (type == "profile") {
        m_backend->log("프로필 수집...", "info", "twitter");

        ExcelWriter writer;
        QStringList profileHdrs = {"Field", "Value"};
        writer.writeHeader(profileHdrs, QColor("#d97757"));

        int prow = 2;
        auto addRow = [&](const QString &field, const QString &value) {
            writer.writeRow(prow++, {field, value});
        };

        addRow("ID", userId);
        addRow("Handle", "@"+ target);
        addRow("Name", userName);
        addRow("Bio", userLegacy["description"].toString());
        addRow("Location", userLegacy["location"].toString());
        addRow("URL", userLegacy["url"].toString());
        addRow("Verified", user["is_blue_verified"].toBool() ? "Yes": "No");
        addRow("Protected", userLegacy["protected"].toBool() ? "Yes": "No");
        addRow("Followers", QString::number(followersCount));
        addRow("Following", QString::number(followingCount));
        addRow("Tweets", QString::number(statusesCount));
        addRow("Likes", QString::number(userLegacy["favourites_count"].toInt()));
        addRow("Created", Common::formatDateJapanese(userLegacy["created_at"].toString()));
        // Birthday
        {
            QJsonObject extProfile = user["legacy_extended_profile"].toObject();
            QJsonObject birthdate = extProfile["birthdate"].toObject();
            if (!birthdate.isEmpty()) {
                QString bday;
                int day = birthdate["day"].toInt();
                int month = birthdate["month"].toInt();
                int year = birthdate["year"].toInt();
                if (year > 0) bday = QString("%1年%2月%3日").arg(year).arg(month).arg(day);
                else if (month > 0 && day > 0) bday = QString("%1月%2日").arg(month).arg(day);
                if (!bday.isEmpty()) addRow("Birthday", bday);
            }
        }
        addRow("Profile Image", userLegacy["profile_image_url_https"].toString().replace("_normal", "_400x400"));
        addRow("Banner", userLegacy["profile_banner_url"].toString());

        writer.setColumnWidth(1, 20);
        writer.setColumnWidth(2, 60);

        // 날짜별 프로필 폴더에 저장 (덮어쓰기 방지)
        {
            QString pdateStr = Common::jstToday().toString("yyyy-MM-dd");
            QString pDateDir = userDir + "/profiles/"+ pdateStr;
            QDir().mkpath(pDateDir);
            QString pPath2 = pDateDir + "/"+ target + "_profile.xlsx";
            writer.save(pPath2);
            FileHelper::setDownloadMeta(pPath2, QStringLiteral(APP_NAME_DISPLAY) + QStringLiteral(" Twitter"));
            // 프로필 Excel은 profiles/ 폴더 안에만 저장
        }
        m_backend->log("프로필 저장 완료", "success", "twitter");

        // Download profile picture (날짜별 폴더)
        {
            QString pdateStr = Common::jstToday().toString("yyyy-MM-dd");
            QString pDateDir = userDir + "/profiles/"+ pdateStr;
            QDir().mkpath(pDateDir);
            // 원본 먼저, 안 되면 400x400(COV-11) — '프로필' 판은 날짜 폴더에 그날의 모습을 남기는 판이라 같은 그림이어도 받는다
            const QString avatarUrl = userLegacy["profile_image_url_https"].toString();
            if (!avatarUrl.isEmpty()) {
                QString pp = pDateDir + "/"+ target + "_profile.jpg";
                if (!QFile::exists(pp)) {
                    for (const QString &u : avatarCandidates(avatarUrl)) {
                        if (downloadMedia(u, pp)) { m_backend->log("프로필 사진 다운로드 완료", "success", "twitter"); break; }
                    }
                }
            }
            QString bannerUrl = userLegacy["profile_banner_url"].toString();
            if (!bannerUrl.isEmpty()) {
                QString bp = pDateDir + "/"+ target + "_banner.jpg";
                if (!QFile::exists(bp)) downloadMedia(bannerUrl + "/1500x500", bp);
            }
        }

        m_backend->updateStats(1, 0, "완료", "twitter");
        if (!isSubCall) stopDaemon();
        return;
    }

    // ── Likes (dedicated GraphQL endpoint) ──
    if (type == "likes") {
        m_backend->log("좋아요 수집 (Likes API)...", "info", "twitter");

        // ★ X 는 2024년 6월부터 좋아요를 본인 것만 보여 준다 — 다른 계정의 좋아요는 빈 목록이 온다(r56, 깊이 점검 F9). 예전엔 빈 쪽을
        //   다섯 번 물은 뒤 아무 말 없이 '완료 0' 이었다. 로그인한 계정의 번호(설정 → 계정 토큰 자동 추출이 적어 둔 것 — 그 토큰의 것일
        //   때만)를 알면 처음부터 건너뛰고(일찍 멈춤 표시는 건드리지 않는다), 모르면 첫 걸음에서 빈 쪽이 다섯 번 이어질 때 같은 까닭을 알린다(아래).
        static const QString kLikesPrivate = QStringLiteral(
            "X 는 2024년 6월부터 좋아요를 본인 것만 보여 줍니다 — @%1 의 좋아요는 로그인한 계정의 것이 아니라 받을 수 없습니다"
            "(로그인한 계정의 좋아요는 그 계정을 대상으로 넣으면 받습니다)");
        const bool uidTrusted = !m_expectedUid.isEmpty() && !m_authToken.isEmpty()
            && m_expectedUidTok == QString::fromLatin1(QCryptographicHash::hash(m_authToken.toUtf8(), QCryptographicHash::Sha256).toHex().left(16));
        if (uidTrusted && !userId.isEmpty() && userId != m_expectedUid) {
            m_backend->log(kLikesPrivate.arg(target) + " — 건너뜁니다", "warning", "twitter");
            m_backend->updateStats(0, 0, "완료", "twitter");
            if (!isSubCall) stopDaemon();
            return;
        }

        QString likesMediaDir = mediaDir + "/likes";
        QDir().mkpath(likesMediaDir);

        QString likesTempDir = config["tempDir"].toString();
        if (likesTempDir.isEmpty()) likesTempDir = userDir + "/.tmp";
        int tweetCount = 0, mediaCount = 0;
        DiskJsonBuffer collectedData(likesTempDir, "tw_likes");
        QSet<QString> collectedIds;
        KnownRun kr;   // 다 받은 곳에서 일찍 멈추기 — 이번 판 전의 엑셀 ID
        kr.known = excelIdsAt(userDir + "/" + target + "_likes.xlsx");
        // 지난번에 이 목록을 끊김 없이 다 본 적이 있어야 일찍 멈춘다(끊긴 판 뒤에는 한 번 끝까지 보아 사이를 메운다)
        //   표시는 미디어를 받는 판 · 안 받는 판 따로(미디어를 끈 판의 표시로 켠 판이 일찍 멈추면 미디어를 영영 안 받았다 — 검토)
        //   진짜 페이지 캡처를 받는 판 · 안 받는 판도 따로 — 캡처를 끈 판의 표시로 켠 판이 일찍 멈추면 옛 글은 캡처를 영영 안 받았다
        //   _r2: 엑셀 칸을 바로잡은 판(2026-10-29) — 옛 표시를 믿지 않고 한 번 끝까지 보아 옛 줄의 빈칸을 메운다
        const QString walkKind = QString("tw_likes_r2_m%1_c%2").arg(m_downloadMedia ? 1 : 0).arg(Common::captureKindCode(config));
        kr.n = Common::takeCleanWalkMarks(userDir, "tw_likes").contains(walkKind) ? stopAfterKnown : 0;
        // 두 판 잇따라 미디어를 못 받은 글은 영영 안 되는 것으로 본다(지워짐 · 막힘) — 처음 실패한 것만 표시를 막는다
        const QSet<QString> prevFailed = Common::takeWalkFailures(userDir, walkKind);
        QSet<QString> failedNow, capFailed;   // 미디어를 못 받은 글 · 캡처를 못 받은 글
        int capDone = 0;                      // 이번에 새로 받은 캡처 수
        // 최대 수 — 일찍 멈추기를 쓰면 새로 받은 것만 센다(받아 둔 것까지 세면 늘 최대 수에서 끊겨 표시가 영영 안 적혔다 — 검토)
        auto capHit = [&]() { return maxCount > 0 && (stopAfterKnown > 0 ? kr.fresh : tweetCount) >= maxCount; };
        m_mediaFailures = 0;
        int currentAccountIdx = 0;
        QString cursor;
        bool hasMore = true;
        int consecutiveEmptyLikes = 0;
        int listPages = 0;   // 트윗을 든 쪽 수

        int listErr = 0;   // 이어진 API 오류(listErrorGiveUp)
        while (isRunning && hasMore && !kr.stopped && !capHit()) {
            auto [tweets, nextCursor] = getLikes(userId, cursor);

            if (nextCursor == "RATE_LIMITED") {
                handleRateLimit(accounts, currentAccountIdx, isRunning);
                continue;
            }
            if (nextCursor == "ERROR") {
                if (listErrorGiveUp(m_backend, listErr, QStringLiteral("좋아요"), isRunning)) break;
                continue;
            }
            listErr = 0;

            if (tweets.isEmpty()) {
                consecutiveEmptyLikes++;
                if (consecutiveEmptyLikes >= 5 && listPages == 0 && tweetCount == 0 && cursor.isEmpty())   // 첫 쪽부터 다섯 번 비었다 — 위 설명
                    m_backend->log(kLikesPrivate.arg(target) + "(첫 쪽부터 빈 목록 — 본인 계정인데도 이러면 좋아요가 정말 없거나 X 가 막은 것입니다)", "warning", "twitter");
                if (consecutiveEmptyLikes >= 5) { if (listPages > 0 && m_lastPageShaped) hasMore = false; break; }   // 쪽을 받은 뒤 같은 커서가 5번 비면 목록 끝이다(X 는 끝에서 트윗 없는 쪽을 준다)
                m_backend->log(QString("빈 응답, 재시도 %1/5").arg(consecutiveEmptyLikes), "warning", "twitter");
                QThread::sleep(3);
                continue;
            }
            consecutiveEmptyLikes = 0;
            ++listPages;

            for (const auto &tweetVal : tweets) {
                if (!isRunning || capHit()) break;
                QJsonObject tweet = tweetVal.toObject();
                QJsonObject legacy = xLegacy(tweet);
                QString tweetId = legacy["id_str"].toString();
                if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();
                if (collectedIds.contains(tweetId)) continue;
                if (tweetPeriodSide(config, legacy["created_at"].toString()) != 0) continue;   // 기간 지정 — 그 글을 쓴 때로
                if (kr.see(tweetId)) break;   // 받아 둔 것이 이어졌다 — 여기서 멈춘다(멈춘 그 글은 보지 않았다 — 실패 기록에서 빼지 않는다)
                collectedIds.insert(tweetId);

                tweetCount++;
                m_backend->log(QString("[%1] %2...").arg(tweetCount).arg(legacy["full_text"].toString().left(40)), "info", "twitter");
                m_backend->updateStats(tweetCount, mediaCount, "수집 중...", "twitter");

                const int mfBefore = m_mediaFailures;
                int newMedia = downloadTweetMedia(tweet, likesMediaDir, tweetCount);
                if (m_mediaFailures > mfBefore && isRunning && !Common::appQuitting()) failedNow.insert(tweetId);   // 중지로 끊긴 받기는 실패가 아니다
                mediaCount += newMedia;

                // 캡쳐 (likes 별도 폴더)
                // 캡처를 못 받은 것도 받지 못한 것이다(두 판 잇따르면 넘어간다 · 캡처 길이 통째로 막힌 판은 빼고 — 판 끝에 더한다)
                if (tweetWantsCapture(tweet, config)) {
                    if (const int cap = captureTweet(tweet, userDir + "/captures/likes", config); cap > 0) ++capDone;
                    else if (cap < 0 && isRunning && !Common::appQuitting()) capFailed.insert(tweetId);
                }

                // Anipo-style batch progress log
                if (tweetCount % 10 == 0) {
                    m_backend->log(QString("트윗 %1개, 미디어 %2개 다운로드").arg(tweetCount).arg(mediaCount), "info", "twitter");
                }

                // 본 걸음과 같은 칸으로(tweetColumns — 예전엔 다른 이름으로 담아 엑셀의 주소 · 작성자 · 미디어 칸이 비었다)
                collectedData.append(standardTweetRow(tweet, QStringLiteral("i")));

                // 좋아요 유저 프로필 다운 (anipo style)
                downloadUserProfileMedia(tweet, userDir + "/profiles", "likes");
            }

            cursor = nextCursor;
            if (cursor.isEmpty()) hasMore = false;
            if (hasMore && !kr.stopped) { for (int w = 10; w > 0 && isRunning; --w) QThread::sleep(1); }
        }

        if (kr.stopped)
            m_backend->log(QString("이미 받은 좋아요가 %1개 이어져 여기서 멈춥니다 — 새로 %2개").arg(kr.stopped).arg(kr.fresh), "info", "twitter");
        // 끊김 없이 끝났으면(목록 끝 · 일찍 멈춤 — 중지 · 최대 수 · 빈 응답 · 기간으로 끊긴 것은 아님) 다음 판이 일찍 멈출 수 있게
        if (Common::captureFailuresCount(capDone, int(capFailed.size()))) failedNow += capFailed;
        else if (stopAfterKnown > 0)
            m_backend->log(QString("캡처가 하나도 되지 않았습니다(%1개 실패) — 캡처 길이 막힌 듯해 이 판의 캡처 실패로는 일찍 멈춤을 막지 않습니다(4~5주마다 끝까지 볼 때 다시 해 봅니다). 캡처 브라우저의 X 로그인을 확인하십시오").arg(capFailed.size()), "warning", "twitter");
        const QSet<QString> newFails = failedNow - prevFailed;
        // 일찍 멈춤을 쓸 때만 안내한다 — 끄면 판마다 끝까지 보며 다시 받으니 넘어가는 것이 없다
        if (stopAfterKnown > 0 && !newFails.isEmpty())
            m_backend->log(QString("미디어를 받지 못한 %1 %2개 — 다음 판은 끝까지 보며 다시 받습니다").arg(QStringLiteral("좋아요")).arg(newFails.size()), "warning", "twitter");
        if (const int gaveUp = stopAfterKnown > 0 ? int((failedNow & prevFailed).size()) : 0)
            m_backend->log(QString("두 판 잇따라 미디어를 받지 못한 %1 %2개는 일찍 멈춤을 더는 막지 않습니다(지워졌거나 막힌 듯 — 4~5주마다 끝까지 볼 때 다시 받아 봅니다)").arg(QStringLiteral("좋아요")).arg(gaveUp), "info", "twitter");
        Common::putWalkFailures(userDir, walkKind, failedNow + (prevFailed - collectedIds));   // 이번 판에 보지 못한 것은 그대로 둔다
        // ★ 이 목록의 엑셀을 먼저 저장하고, 그 저장이 실패했으면 표시를 옮기지 않는다 — 예전엔 표시를 적은 뒤에 저장해, 저장에
        //   실패해도 표시가 남아 다음 판이 빠진 줄을 건너뛰었다(검토). 앞선 다른 파일(트윗 엑셀)의 실패와는 따로 본다(실패 사실은 그대로).
        const bool xlFailedBefore = m_excelSaveFailed;
        m_excelSaveFailed = false;
        if (collectedData.count() > 0) saveExcelStreaming(userDir, target, collectedData, "likes");
        const bool listXlFailed = m_excelSaveFailed;
        m_excelSaveFailed = xlFailedBefore || listXlFailed;
        if (isRunning && (kr.stopped || !hasMore) && !capHit() && newFails.isEmpty() && !listXlFailed
            && config["mode"].toString() != QLatin1String("period"))   // 기간 판은 목록 전체를 본 것이 아니다
            Common::putCleanWalkMark(userDir, walkKind);
        m_backend->noteTrackFresh(kr.fresh);
        if (kr.stopped) m_backend->noteTrackEarlyStop(kr.stopped);
        m_backend->log(QString("완료! 좋아요: %1, 미디어: %2").arg(tweetCount).arg(mediaCount), "success", "twitter");
        m_backend->updateStats(tweetCount, mediaCount, "완료", "twitter");
        if (!isSubCall) stopDaemon();
        return;
    }

    // ── Bookmarks (dedicated GraphQL endpoint, own account only) ──
    if (type == "bookmarks") {
        m_backend->log("북마크 수집...", "info", "twitter");

        QString bookmarksMediaDir = mediaDir + "/bookmarks";
        QDir().mkpath(bookmarksMediaDir);

        QString bkmkTempDir = config["tempDir"].toString();
        if (bkmkTempDir.isEmpty()) bkmkTempDir = userDir + "/.tmp";
        int tweetCount = 0, mediaCount = 0;
        DiskJsonBuffer collectedData(bkmkTempDir, "tw_bookmarks");
        QSet<QString> collectedIds;
        KnownRun kr;   // 다 받은 곳에서 일찍 멈추기 — 이번 판 전의 엑셀 ID
        kr.known = excelIdsAt(userDir + "/" + target + "_bookmarks.xlsx");
        // 지난번에 이 목록을 끊김 없이 다 본 적이 있어야 일찍 멈춘다(끊긴 판 뒤에는 한 번 끝까지 보아 사이를 메운다)
        //   표시는 미디어를 받는 판 · 안 받는 판 따로(미디어를 끈 판의 표시로 켠 판이 일찍 멈추면 미디어를 영영 안 받았다 — 검토)
        //   진짜 페이지 캡처를 받는 판 · 안 받는 판도 따로 — 캡처를 끈 판의 표시로 켠 판이 일찍 멈추면 옛 글은 캡처를 영영 안 받았다
        //   _r2: 엑셀 칸을 바로잡은 판(2026-10-29) — 옛 표시를 믿지 않고 한 번 끝까지 보아 옛 줄의 빈칸을 메운다
        const QString walkKind = QString("tw_bookmarks_r2_m%1_c%2").arg(m_downloadMedia ? 1 : 0).arg(Common::captureKindCode(config));
        kr.n = Common::takeCleanWalkMarks(userDir, "tw_bookmarks").contains(walkKind) ? stopAfterKnown : 0;
        // 두 판 잇따라 미디어를 못 받은 글은 영영 안 되는 것으로 본다(지워짐 · 막힘) — 처음 실패한 것만 표시를 막는다
        const QSet<QString> prevFailed = Common::takeWalkFailures(userDir, walkKind);
        QSet<QString> failedNow, capFailed;   // 미디어를 못 받은 글 · 캡처를 못 받은 글
        int capDone = 0;                      // 이번에 새로 받은 캡처 수
        // 최대 수 — 일찍 멈추기를 쓰면 새로 받은 것만 센다(받아 둔 것까지 세면 늘 최대 수에서 끊겨 표시가 영영 안 적혔다 — 검토)
        auto capHit = [&]() { return maxCount > 0 && (stopAfterKnown > 0 ? kr.fresh : tweetCount) >= maxCount; };
        m_mediaFailures = 0;
        int currentAccountIdx = 0;
        QString cursor;
        bool hasMore = true;
        int consecutiveEmptyBkmk = 0;
        int listPages = 0;   // 트윗을 든 쪽 수

        int listErr = 0;   // 이어진 API 오류(listErrorGiveUp)
        while (isRunning && hasMore && !kr.stopped && !capHit()) {
            auto [tweets, nextCursor] = getBookmarks(cursor);

            if (nextCursor == "RATE_LIMITED") {
                handleRateLimit(accounts, currentAccountIdx, isRunning);
                continue;
            }
            if (nextCursor == "ERROR") {
                if (listErrorGiveUp(m_backend, listErr, QStringLiteral("북마크"), isRunning)) break;
                continue;
            }
            listErr = 0;

            if (tweets.isEmpty()) {
                consecutiveEmptyBkmk++;
                if (consecutiveEmptyBkmk >= 5) { if (listPages > 0 && m_lastPageShaped) hasMore = false; break; }   // 쪽을 받은 뒤 같은 커서가 5번 비면 목록 끝이다(X 는 끝에서 트윗 없는 쪽을 준다)
                m_backend->log(QString("빈 응답, 재시도 %1/5").arg(consecutiveEmptyBkmk), "warning", "twitter");
                QThread::sleep(3);
                continue;
            }
            consecutiveEmptyBkmk = 0;
            ++listPages;

            for (const auto &tweetVal : tweets) {
                if (!isRunning || capHit()) break;
                QJsonObject tweet = tweetVal.toObject();
                QJsonObject legacy = xLegacy(tweet);
                QString tweetId = legacy["id_str"].toString();
                if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();
                if (collectedIds.contains(tweetId)) continue;
                if (tweetPeriodSide(config, legacy["created_at"].toString()) != 0) continue;   // 기간 지정 — 그 글을 쓴 때로
                if (kr.see(tweetId)) break;   // 받아 둔 것이 이어졌다 — 여기서 멈춘다(멈춘 그 글은 보지 않았다 — 실패 기록에서 빼지 않는다)
                collectedIds.insert(tweetId);

                tweetCount++;
                QString tweetText = legacy["full_text"].toString().left(40);
                m_backend->log(QString("[%1] %2...").arg(tweetCount).arg(tweetText), "info", "twitter");
                m_backend->updateStats(tweetCount, mediaCount, "수집 중...", "twitter");

                const int mfBefore = m_mediaFailures;
                int newMedia = downloadTweetMedia(tweet, bookmarksMediaDir, tweetCount);
                if (m_mediaFailures > mfBefore && isRunning && !Common::appQuitting()) failedNow.insert(tweetId);   // 중지로 끊긴 받기는 실패가 아니다
                mediaCount += newMedia;

                // 캡쳐 (bookmarks 별도 폴더)
                // 캡처를 못 받은 것도 받지 못한 것이다(두 판 잇따르면 넘어간다 · 캡처 길이 통째로 막힌 판은 빼고 — 판 끝에 더한다)
                if (tweetWantsCapture(tweet, config)) {
                    if (const int cap = captureTweet(tweet, userDir + "/captures/bookmarks", config); cap > 0) ++capDone;
                    else if (cap < 0 && isRunning && !Common::appQuitting()) capFailed.insert(tweetId);
                }

                // Anipo-style batch progress log
                if (tweetCount % 10 == 0) {
                    m_backend->log(QString("트윗 %1개, 미디어 %2개 다운로드").arg(tweetCount).arg(mediaCount), "info", "twitter");
                }

                // Get tweet author info
                // 본 걸음과 같은 칸으로(tweetColumns — 예전엔 다른 이름으로 담아 엑셀의 주소 · 작성자 · 미디어 칸이 비었다)
                collectedData.append(standardTweetRow(tweet, QStringLiteral("i")));
            }

            cursor = nextCursor;
            if (cursor.isEmpty()) hasMore = false;
            if (hasMore && !kr.stopped) { for (int w = 10; w > 0 && isRunning; --w) QThread::sleep(1); }
        }

        if (kr.stopped)
            m_backend->log(QString("이미 받은 북마크가 %1개 이어져 여기서 멈춥니다 — 새로 %2개").arg(kr.stopped).arg(kr.fresh), "info", "twitter");
        // 끊김 없이 끝났으면(목록 끝 · 일찍 멈춤 — 중지 · 최대 수 · 빈 응답 · 기간으로 끊긴 것은 아님) 다음 판이 일찍 멈출 수 있게
        if (Common::captureFailuresCount(capDone, int(capFailed.size()))) failedNow += capFailed;
        else if (stopAfterKnown > 0)
            m_backend->log(QString("캡처가 하나도 되지 않았습니다(%1개 실패) — 캡처 길이 막힌 듯해 이 판의 캡처 실패로는 일찍 멈춤을 막지 않습니다(4~5주마다 끝까지 볼 때 다시 해 봅니다). 캡처 브라우저의 X 로그인을 확인하십시오").arg(capFailed.size()), "warning", "twitter");
        const QSet<QString> newFails = failedNow - prevFailed;
        // 일찍 멈춤을 쓸 때만 안내한다 — 끄면 판마다 끝까지 보며 다시 받으니 넘어가는 것이 없다
        if (stopAfterKnown > 0 && !newFails.isEmpty())
            m_backend->log(QString("미디어를 받지 못한 %1 %2개 — 다음 판은 끝까지 보며 다시 받습니다").arg(QStringLiteral("북마크")).arg(newFails.size()), "warning", "twitter");
        if (const int gaveUp = stopAfterKnown > 0 ? int((failedNow & prevFailed).size()) : 0)
            m_backend->log(QString("두 판 잇따라 미디어를 받지 못한 %1 %2개는 일찍 멈춤을 더는 막지 않습니다(지워졌거나 막힌 듯 — 4~5주마다 끝까지 볼 때 다시 받아 봅니다)").arg(QStringLiteral("북마크")).arg(gaveUp), "info", "twitter");
        Common::putWalkFailures(userDir, walkKind, failedNow + (prevFailed - collectedIds));   // 이번 판에 보지 못한 것은 그대로 둔다
        // ★ 이 목록의 엑셀을 먼저 저장하고, 그 저장이 실패했으면 표시를 옮기지 않는다 — 예전엔 표시를 적은 뒤에 저장해, 저장에
        //   실패해도 표시가 남아 다음 판이 빠진 줄을 건너뛰었다(검토). 앞선 다른 파일(트윗 엑셀)의 실패와는 따로 본다(실패 사실은 그대로).
        const bool xlFailedBefore = m_excelSaveFailed;
        m_excelSaveFailed = false;
        if (collectedData.count() > 0) saveExcelStreaming(userDir, target, collectedData, "bookmarks");
        const bool listXlFailed = m_excelSaveFailed;
        m_excelSaveFailed = xlFailedBefore || listXlFailed;
        if (isRunning && (kr.stopped || !hasMore) && !capHit() && newFails.isEmpty() && !listXlFailed
            && config["mode"].toString() != QLatin1String("period"))   // 기간 판은 목록 전체를 본 것이 아니다
            Common::putCleanWalkMark(userDir, walkKind);
        m_backend->noteTrackFresh(kr.fresh);
        if (kr.stopped) m_backend->noteTrackEarlyStop(kr.stopped);
        m_backend->log(QString("완료! 북마크: %1, 미디어: %2").arg(tweetCount).arg(mediaCount), "success", "twitter");
        m_backend->updateStats(tweetCount, mediaCount, "완료", "twitter");
        if (!isSubCall) stopDaemon();
        return;
    }

    // ── Reposts (UserTweets-based, filter for retweets) ──
    if (type == "reposts") {
        m_backend->log("리포스트 수집...", "info", "twitter");

        QString repostsMediaDir = mediaDir + "/reposts";
        QDir().mkpath(repostsMediaDir);

        QString rpTempDir = config["tempDir"].toString();
        if (rpTempDir.isEmpty()) rpTempDir = userDir + "/.tmp";
        int tweetCount = 0, mediaCount = 0;
        DiskJsonBuffer collectedData(rpTempDir, "tw_reposts");
        QSet<QString> collectedIds;
        KnownRun kr;   // 다 받은 곳에서 일찍 멈추기 — 이번 판 전의 엑셀 ID
        kr.known = excelIdsAt(userDir + "/" + target + "_reposts.xlsx");
        // 지난번에 이 목록을 끊김 없이 다 본 적이 있어야 일찍 멈춘다(끊긴 판 뒤에는 한 번 끝까지 보아 사이를 메운다)
        //   표시는 미디어를 받는 판 · 안 받는 판 따로(미디어를 끈 판의 표시로 켠 판이 일찍 멈추면 미디어를 영영 안 받았다 — 검토)
        //   진짜 페이지 캡처를 받는 판 · 안 받는 판도 따로 — 캡처를 끈 판의 표시로 켠 판이 일찍 멈추면 옛 글은 캡처를 영영 안 받았다
        const QString walkKind = QString("tw_reposts_m%1_c%2").arg(m_downloadMedia ? 1 : 0).arg(Common::captureKindCode(config));
        kr.n = Common::takeCleanWalkMarks(userDir, "tw_reposts").contains(walkKind) ? stopAfterKnown : 0;
        // 두 판 잇따라 미디어를 못 받은 글은 영영 안 되는 것으로 본다(지워짐 · 막힘) — 처음 실패한 것만 표시를 막는다
        const QSet<QString> prevFailed = Common::takeWalkFailures(userDir, walkKind);
        QSet<QString> failedNow, capFailed;   // 미디어를 못 받은 글 · 캡처를 못 받은 글
        int capDone = 0;                      // 이번에 새로 받은 캡처 수
        // 최대 수 — 일찍 멈추기를 쓰면 새로 받은 것만 센다(받아 둔 것까지 세면 늘 최대 수에서 끊겨 표시가 영영 안 적혔다 — 검토)
        auto capHit = [&]() { return maxCount > 0 && (stopAfterKnown > 0 ? kr.fresh : tweetCount) >= maxCount; };
        m_mediaFailures = 0;
        int currentAccountIdx = 0;
        QString cursor;
        bool hasMore = true;
        int consecutiveEmpty = 0;
        const int MAX_EMPTY = 5;
        int listPages = 0;   // 트윗을 든 쪽 수

        // Period filter
        QDateTime sinceDate, untilDate;
        if (mode == "period") {
            QString since = config["since"].toString();
            QString until = config["until"].toString();
            if (!since.isEmpty()) sinceDate = QDateTime::fromString(since + "T00:00:00+09:00", Qt::ISODate);
            if (!until.isEmpty()) untilDate = QDateTime::fromString(until + "T23:59:59+09:00", Qt::ISODate);
            if (sinceDate.isValid() || untilDate.isValid())
                m_backend->log(QString("기간: %1 ~ %2").arg(since, until), "info", "twitter");
        }

        int listErr = 0;   // 이어진 API 오류(listErrorGiveUp)
        while (isRunning && hasMore && !kr.stopped && !capHit()) {
            auto [tweets, nextCursor] = getUserTweets(userId, cursor);

            if (nextCursor == "RATE_LIMITED") {
                handleRateLimit(accounts, currentAccountIdx, isRunning);
                continue;
            }
            if (nextCursor == "ERROR") {
                if (listErrorGiveUp(m_backend, listErr, QStringLiteral("리포스트"), isRunning)) break;
                continue;
            }
            listErr = 0;

            if (tweets.isEmpty()) {
                consecutiveEmpty++;
                if (consecutiveEmpty >= MAX_EMPTY) {
                    if (listPages > 0 && m_lastPageShaped) hasMore = false;   // 쪽을 받은 뒤 같은 커서가 5번 제 모양으로 비면 목록 끝이다(X 는 끝에서 트윗 없는 쪽을 준다)
                    else m_backend->log(QString("빈 응답 %1회 연속, 종료").arg(MAX_EMPTY), "warning", "twitter");
                    break;
                }
                m_backend->log(QString("빈 응답, 재시도 %1/%2").arg(consecutiveEmpty).arg(MAX_EMPTY), "warning", "twitter");
                QThread::sleep(3);
                continue;
            }
            consecutiveEmpty = 0;
            ++listPages;

            bool pastRange = false;
            for (const auto &tweetVal : tweets) {
                if (!isRunning || capHit()) break;
                QJsonObject tweet = tweetVal.toObject();
                QJsonObject legacy = xLegacy(tweet);

                // Only include retweets
                QJsonObject rtResult;
                if (legacy.contains("retweeted_status_result")) {
                    rtResult = legacy["retweeted_status_result"].toObject()["result"].toObject();
                }
                if (rtResult.isEmpty() && tweet.contains("retweeted_status_result")) {
                    rtResult = tweet["retweeted_status_result"].toObject()["result"].toObject();
                }
                if (rtResult.isEmpty()) continue;  // Not a retweet, skip

                // Handle TweetWithVisibilityResults wrapper
                if (rtResult.contains("tweet")) rtResult = rtResult["tweet"].toObject();

                QString tweetId = legacy["id_str"].toString();
                if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();
                if (collectedIds.contains(tweetId)) continue;

                // Period filter (UserTweets returns reverse chronological)
                QString createdAt = legacy["created_at"].toString();
                QDateTime tweetDate = Common::parseISODate(createdAt);
                if (untilDate.isValid() && tweetDate > untilDate) continue;
                if (sinceDate.isValid() && tweetDate < sinceDate) { pastRange = true; break; }

                if (kr.see(tweetId)) break;   // 받아 둔 리포스트가 이어졌다 — 여기서 멈춘다(멈춘 그 글은 보지 않았다)
                collectedIds.insert(tweetId);
                tweetCount++;

                QJsonObject rtLegacy = xLegacy(rtResult);
                QString rtScreenName = screenNameOf(rtResult["core"].toObject()["user_results"]
                                                        .toObject()["result"].toObject());
                QString rtText = rtLegacy["full_text"].toString();

                m_backend->log(QString("[%1] RT @%2: %3...").arg(tweetCount).arg(rtScreenName, rtText.left(30)), "info", "twitter");
                m_backend->updateStats(tweetCount, mediaCount, "수집 중...", "twitter");

                // Download media from original tweet
                const int mfBefore = m_mediaFailures;
                int newMedia = downloadTweetMedia(rtResult, repostsMediaDir, tweetCount);
                if (m_mediaFailures > mfBefore && isRunning && !Common::appQuitting()) failedNow.insert(tweetId);   // 중지로 끊긴 받기는 실패가 아니다
                mediaCount += newMedia;

                // 캡쳐 (reposts 별도 폴더, 원글 기준 캡쳐)
                // 캡처를 못 받은 것도 받지 못한 것이다(두 판 잇따르면 넘어간다 · 캡처 길이 통째로 막힌 판은 빼고 — 판 끝에 더한다)
                // ★ 같은 리트윗을 두 번 캡처하지 않는다(r56 — 속도 점검 '전체' 에서 리트윗마다 진짜 페이지 캡처 두 번). 트윗 걸음
                //   (Phase 2 · processTweetBatch)은 리트윗을 그 리트윗 ID 로 captures/ 에 받는다(그 주소는 원글로 열린다 — 같은 페이지).
                //   그 캡처가 이미 다 받아져 있으면 여기서는 다시 받지 않는다. 미디어는 같은 이름 · 같은 폴더(media/reposts)라 이미 있으면
                //   받지 않고, EXIF 도 '다 적음' 표시(mediaAlreadyFinished)로 다시 적지 않는다.
                //   캡처가 받아져 아직 임시 자리(.abiwa_cap)에서 옮겨지기를 기다리는 것도 받은 것이다(검토 — 그 자리엔 대신 카드나
                //   아무것도 없어 '안 받음' 으로 보였고, 같은 페이지를 captures/reposts 로 한 번 더 받았다).
                const QString walkCapDir = userDir + "/captures";
                const QString walkCapName = FileHelper::captureNameCompat(walkCapDir, Common::parseISODate(createdAt), tweetId);
                const bool capturedByWalk = m_backend->realCaptureFinished(walkCapDir, walkCapName)
                    || m_backend->stagedCapturePending(walkCapDir + "/" + walkCapName + ".html");
                if (!capturedByWalk && tweetWantsCapture(rtResult, config)) {
                    if (const int cap = captureTweet(rtResult, userDir + "/captures/reposts", config); cap > 0) ++capDone;
                    else if (cap < 0 && isRunning && !Common::appQuitting()) capFailed.insert(tweetId);
                }

                // Anipo-style batch progress log
                if (tweetCount % 10 == 0) {
                    m_backend->log(QString("트윗 %1개, 미디어 %2개 다운로드").arg(tweetCount).arg(mediaCount), "info", "twitter");
                }

                // Download RT user profile pics & banners (anipo style)
                // tweet = RT 트윗 전체 (RT한 사람 포함), rtResult = 원글 트윗
                {
                    QString profileDir = userDir + "/profiles";
                    // 원글 작성자 프로필 (rtResult에서 core.user_results)
                    downloadUserProfileMedia(rtResult, profileDir, "reposts");
                    // RT한 사람 프로필 (tweet에서 core.user_results)
                    downloadUserProfileMedia(tweet, profileDir, "reposts");
                }

                // ── 엑셀 한 줄 — 트윗 · 좋아요와 같은 standardTweetRow 로(r56, COV-9). 예전엔 따로 줄였다 — 긴 글(note_tweet)은 280자에서
                //   잘리고 해시태그 · 링크 · 인용 · 카드 칸이 비었다. 이 파일의 뜻은 예전 그대로 둔다: 작성자 · 숫자 · 작성자 정보는 원글의 것
                //   (standardTweetRow 는 리트윗한 사람 · 리트윗 껍질의 숫자를 적는다), 출처는 빈칸.
                QJsonObject data = standardTweetRow(tweet, target);
                QJsonObject rtUserResult = rtResult["core"].toObject()["user_results"].toObject()["result"].toObject();
                // 팔로워 수 등 통계는 아직 legacy 에 남아 있다 — 이름만 옮겨 갔다.
                QJsonObject rtUserLeg = xUserLegacy(rtUserResult);
                data["id"] = tweetId;
                data["type"] = "Retweet";
                data["language"] = rtLegacy["lang"].toString();
                data["author_name"] = displayNameOf(rtUserResult);
                data["author_username"] = rtScreenName;
                data["bookmark_count"] = rtLegacy["bookmark_count"].toInt();
                data["favorite_count"] = rtLegacy["favorite_count"].toInt();
                data["retweet_count"] = rtLegacy["retweet_count"].toInt();
                data["reply_count"] = rtLegacy["reply_count"].toInt();
                data["quote_count"] = rtLegacy["quote_count"].toInt();
                data["view_count"] = rtResult["views"].toObject()["count"].toString();
                data["source"] = "";
                data["conversation_id"] = rtLegacy["conversation_id_str"].toString();
                data["possibly_sensitive"] = rtLegacy.value("possibly_sensitive").toBool(false) ? "True": "False";
                data["user_followers"] = QString::number(rtUserLeg["followers_count"].toInt());
                data["user_following"] = QString::number(rtUserLeg["friends_count"].toInt());
                data["user_verified"] = rtUserResult.value("is_blue_verified").toBool(false) ? "True": "False";
                data["user_profile_image"] = rtUserLeg["profile_image_url_https"].toString();
                collectedData.append(data);
            }

            if (pastRange) break;
            cursor = nextCursor;
            if (cursor.isEmpty()) hasMore = false;
            if (hasMore && !kr.stopped) { for (int w = 10; w > 0 && isRunning; --w) QThread::sleep(1); }
        }

        if (kr.stopped)
            m_backend->log(QString("이미 받은 리포스트가 %1개 이어져 여기서 멈춥니다 — 새로 %2개").arg(kr.stopped).arg(kr.fresh), "info", "twitter");
        // 끊김 없이 끝났으면(목록 끝 · 일찍 멈춤 — 중지 · 최대 수 · 빈 응답 · 기간으로 끊긴 것은 아님) 다음 판이 일찍 멈출 수 있게
        if (Common::captureFailuresCount(capDone, int(capFailed.size()))) failedNow += capFailed;
        else if (stopAfterKnown > 0)
            m_backend->log(QString("캡처가 하나도 되지 않았습니다(%1개 실패) — 캡처 길이 막힌 듯해 이 판의 캡처 실패로는 일찍 멈춤을 막지 않습니다(4~5주마다 끝까지 볼 때 다시 해 봅니다). 캡처 브라우저의 X 로그인을 확인하십시오").arg(capFailed.size()), "warning", "twitter");
        const QSet<QString> newFails = failedNow - prevFailed;
        // 일찍 멈춤을 쓸 때만 안내한다 — 끄면 판마다 끝까지 보며 다시 받으니 넘어가는 것이 없다
        if (stopAfterKnown > 0 && !newFails.isEmpty())
            m_backend->log(QString("미디어를 받지 못한 %1 %2개 — 다음 판은 끝까지 보며 다시 받습니다").arg(QStringLiteral("리포스트")).arg(newFails.size()), "warning", "twitter");
        if (const int gaveUp = stopAfterKnown > 0 ? int((failedNow & prevFailed).size()) : 0)
            m_backend->log(QString("두 판 잇따라 미디어를 받지 못한 %1 %2개는 일찍 멈춤을 더는 막지 않습니다(지워졌거나 막힌 듯 — 4~5주마다 끝까지 볼 때 다시 받아 봅니다)").arg(QStringLiteral("리포스트")).arg(gaveUp), "info", "twitter");
        Common::putWalkFailures(userDir, walkKind, failedNow + (prevFailed - collectedIds));   // 이번 판에 보지 못한 것은 그대로 둔다
        // ★ 이 목록의 엑셀을 먼저 저장하고, 그 저장이 실패했으면 표시를 옮기지 않는다 — 예전엔 표시를 적은 뒤에 저장해, 저장에
        //   실패해도 표시가 남아 다음 판이 빠진 줄을 건너뛰었다(검토). 앞선 다른 파일(트윗 엑셀)의 실패와는 따로 본다(실패 사실은 그대로).
        const bool xlFailedBefore = m_excelSaveFailed;
        m_excelSaveFailed = false;
        if (collectedData.count() > 0) {
            m_backend->log("Excel 저장...", "info", "twitter");
            saveExcelStreaming(userDir, target, collectedData, "reposts");
        }
        const bool listXlFailed = m_excelSaveFailed;
        m_excelSaveFailed = xlFailedBefore || listXlFailed;
        if (isRunning && (kr.stopped || !hasMore) && !capHit() && newFails.isEmpty() && !listXlFailed
            && !sinceDate.isValid() && !untilDate.isValid())   // 기간으로 거른 걸음은 목록 전체를 본 것이 아니다
            Common::putCleanWalkMark(userDir, walkKind);
        m_backend->noteTrackFresh(kr.fresh);
        if (kr.stopped) m_backend->noteTrackEarlyStop(kr.stopped);

        m_backend->log(QString("완료! 리포스트: %1, 미디어: %2").arg(tweetCount).arg(mediaCount), "success", "twitter");
        m_backend->updateStats(tweetCount, mediaCount, "완료", "twitter");
        if (!isSubCall) stopDaemon();
        return;
    }

    // ── Thread Comments (TweetDetail API — 게시물에 달린 다른 사람 답글) ──
    if (type == "thread_comments") {
        m_backend->log("5. 게시물 댓글 수집 (다른 사람 답글)...", "info", "twitter");

        QString commentTempDir = config["tempDir"].toString();
        if (commentTempDir.isEmpty()) commentTempDir = userDir + "/.tmp";
        int commentCount = 0, mediaCount = 0;
        DiskJsonBuffer collectedData(commentTempDir, "tw_comments");
        QSet<QString> collectedIds;
        int currentAccountIdx = 0;

        // Step 1: 유저의 모든 트윗 ID 수집 (reply_count > 0인 것만)
        m_backend->log("Phase 1: 댓글 있는 포스트 목록 수집...", "info", "twitter");
        QList<QPair<QString, int>> tweetIdsWithReplies;  // {tweetId, replyCount}
        {
            QString cursor;
            bool hasMore = true;
            int consecutiveEmpty = 0;
            int tcErr = 0;   // 이어진 API 오류(listErrorGiveUp)
            while (isRunning && hasMore) {
                auto [tweets, nextCursor] = getUserTweets(userId, cursor);
                if (nextCursor == "RATE_LIMITED") {
                    handleRateLimit(accounts, currentAccountIdx, isRunning);
                    continue;
                }
                if (nextCursor == "ERROR") {
                    if (listErrorGiveUp(m_backend, tcErr, QStringLiteral("게시물 목록"), isRunning)) break;
                    continue;
                }
                tcErr = 0;
                if (tweets.isEmpty()) {
                    consecutiveEmpty++;
                    if (consecutiveEmpty >= 5) break;
                    QThread::sleep(3);
                    continue;
                }
                consecutiveEmpty = 0;

                for (const auto &tweetVal : tweets) {
                    QJsonObject tweet = tweetVal.toObject();
                    QJsonObject legacy = xLegacy(tweet);
                    // Skip retweets
                    if (legacy.contains("retweeted_status_result") || tweet.contains("retweeted_status_result"))
                        continue;
                    QString tid = legacy["id_str"].toString();
                    if (tid.isEmpty()) tid = tweet["rest_id"].toString();
                    int replyCount = legacy["reply_count"].toInt();
                    if (replyCount > 0) {
                        tweetIdsWithReplies.append({tid, replyCount});
                    }
                }

                cursor = nextCursor;
                if (cursor.isEmpty()) hasMore = false;
                if (hasMore) QThread::sleep(2);
            }
        }

        int totalPosts = tweetIdsWithReplies.size();
        int expectedReplies = 0;
        for (const auto &p : tweetIdsWithReplies) expectedReplies += p.second;
        m_backend->log(QString("댓글 있는 포스트 %1개 (예상 댓글 %2개)").arg(totalPosts).arg(expectedReplies), "info", "twitter");

        // Step 2: 각 포스트의 TweetDetail → 답글 추출
        m_backend->log("Phase 2: 댓글 수집 중...", "info", "twitter");
        for (int postIdx = 0; postIdx < totalPosts && isRunning; ++postIdx) {
            const QString &focalId = tweetIdsWithReplies[postIdx].first;
            int expectedCount = tweetIdsWithReplies[postIdx].second;

            QString cursor;
            bool hasMore = true;
            int pageReplies = 0;
            int tcErr = 0;   // 이어진 API 오류(listErrorGiveUp) — 5번이면 댓글 단계를 통째로 멈춘다
            bool tcGaveUp = false;

            while (isRunning && hasMore) {
                auto [replies, nextCursor] = getTweetDetail(focalId, cursor);

                if (nextCursor == "RATE_LIMITED") {
                    handleRateLimit(accounts, currentAccountIdx, isRunning);
                    continue;
                }
                if (nextCursor == "ERROR") {
                    if (listErrorGiveUp(m_backend, tcErr, QStringLiteral("게시물 댓글"), isRunning)) { tcGaveUp = true; break; }
                    continue;
                }
                tcErr = 0;

                if (replies.isEmpty()) break;

                for (const auto &replyVal : replies) {
                    if (!isRunning) break;
                    QJsonObject tweet = replyVal.toObject();
                    QJsonObject legacy = xLegacy(tweet);
                    QString replyId = legacy["id_str"].toString();
                    if (replyId.isEmpty()) replyId = tweet["rest_id"].toString();
                    if (replyId.isEmpty() || collectedIds.contains(replyId)) continue;

                    collectedIds.insert(replyId);
                    commentCount++;
                    pageReplies++;

                    // Author info
                    QJsonObject authorResult = tweet["core"].toObject()["user_results"].toObject()["result"].toObject();
                    QString authorUsername = screenNameOf(authorResult);
                    QString replyText = legacy["full_text"].toString();
                    QString parentUrl = QString("https://x.com/%1/status/%2").arg(target, focalId);

                    m_backend->log(QString("[%1] @%2: %3...").arg(commentCount).arg(authorUsername, replyText.left(40)), "info", "twitter");
                    m_backend->updateStats(commentCount, mediaCount, QString("댓글 수집 (%1/%2 포스트)").arg(postIdx + 1).arg(totalPosts), "twitter");

                    // Download media
                    int newMediaCmt = 0;
                    if (m_downloadMedia) {
                        QString commentMediaDir = mediaDir + "/comments";
                        QDir().mkpath(commentMediaDir);
                        newMediaCmt = downloadTweetMedia(tweet, commentMediaDir, commentCount);
                        mediaCount += newMediaCmt;
                    }

                    // 캡쳐 (comments 별도 폴더)
                    if (tweetWantsCapture(tweet, config)) captureTweet(tweet, userDir + "/captures/comments", config);

                    // Download commenter profile
                    downloadUserProfileMedia(tweet, userDir + "/profiles", "comments");

                    // ★ 엑셀 한 줄 — 트윗 · 리포스트와 같은 standardTweetRow 로(r56, 검토 B-3). 예전엔 손으로 채워 새 칸(대체 글 · 카드 · 글(Articles) ·
                    //   고친 이력 · 인용 본문)이 늘 비었고, 긴 댓글(note_tweet)은 280자에서 잘렸으며 media_urls 엔 영상 대신 미리보기 그림이 적혔다.
                    //   댓글만의 값(종류 = Reply · 어느 글의 댓글인가)만 덧씌운다.
                    QJsonObject data = standardTweetRow(tweet, QStringLiteral("i"));
                    data["type"] = "Reply";
                    data["parent_tweet_url"] = parentUrl;

                    collectedData.append(data);
                }

                cursor = nextCursor;
                if (cursor.isEmpty()) hasMore = false;
                if (hasMore) QThread::sleep(1);
            }

            if (tcGaveUp) break;   // 같은 오류가 다음 글에서도 이어진다 — 받은 만큼 저장하고 다음 단계로

            // Progress log every 10 posts
            if ((postIdx + 1) % 10 == 0 || postIdx == totalPosts - 1) {
                m_backend->log(QString("%1/%2 포스트 처리, 댓글 %3개").arg(postIdx + 1).arg(totalPosts).arg(commentCount), "info", "twitter");
            }

            // Intermediate save every 500 comments
            if (commentCount > 0 && commentCount % 500 < 20) {
                saveExcelStreaming(userDir, target, collectedData, "comments");
                m_backend->log(QString("중간 저장: %1건").arg(commentCount), "info", "twitter");
            }

            QThread::sleep(1);  // Gentle rate: 1 post/sec
        }

        if (collectedData.count() > 0) {
            saveExcelStreaming(userDir, target, collectedData, "comments");
            m_backend->log(QString("Excel 저장: %1_comments.xlsx").arg(target), "info", "twitter");
        }

        m_backend->log(QString("완료! 댓글: %1, 미디어: %2 (포스트 %3개 처리)").arg(commentCount).arg(mediaCount).arg(totalPosts), "success", "twitter");
        m_backend->updateStats(commentCount, mediaCount, "완료", "twitter");
        if (!isSubCall) stopDaemon();
        return;
    }

    // ── Tweets / Media / Replies (UserTweets API + SearchTimeline fallback) ──
    QString excelSuffix = "tweets";
    QString logLabel = "트윗";
    bool filterMediaOnly = false;
    bool filterRepliesOnly = false;

    // Period filter
    QDateTime sinceDate, untilDate;
    if (mode == "period") {
        QString since = config["since"].toString();
        QString until = config["until"].toString();
        if (!since.isEmpty()) sinceDate = QDateTime::fromString(since + "T00:00:00+09:00", Qt::ISODate);
        if (!until.isEmpty()) untilDate = QDateTime::fromString(until + "T23:59:59+09:00", Qt::ISODate);
        if (sinceDate.isValid() || untilDate.isValid())
            m_backend->log(QString("기간: %1 ~ %2").arg(since, until), "info", "twitter");
    }

    if (type == "media") {
        filterMediaOnly = true;
        excelSuffix = "media";
        logLabel = "미디어";
        m_backend->log("미디어만 수집...", "info", "twitter");
    } else if (type == "replies") {
        excelSuffix = "tweets";  // replies도 tweets Excel에 합산 (같은 파일)
        logLabel = "답글";
        m_backend->log("3. 답글 검색 (기존 트윗에 추가)...", "info", "twitter");
    } else if (type == "tweets_api") {
        excelSuffix = "tweets";  // tweets_api도 같은 Excel에 합산
        logLabel = "트윗 보충";
        m_backend->log("2. UserTweets API 보충 (기존 트윗에 추가)...", "info", "twitter");
    } else if (type == "narrow") {
        // 포스트 좁히기 — from:user + 키워드/기간 검색 (별도 _narrow.xlsx)
        excelSuffix = "narrow";
        logLabel = "포스트 좁히기";
        // mode 무관하게 since/until 을 읽음 (기간 좁히기)
        const QString nSince = config["since"].toString();
        const QString nUntil = config["until"].toString();
        if (!nSince.isEmpty()) sinceDate = QDateTime::fromString(nSince + "T00:00:00+09:00", Qt::ISODate);
        if (!nUntil.isEmpty()) untilDate = QDateTime::fromString(nUntil + "T23:59:59+09:00", Qt::ISODate);
        m_backend->log("🔎 포스트 좁히기 (키워드/기간 검색)...", "info", "twitter");
    } else {
        m_backend->log("1. 트윗 수집 (아니포 검색)...", "info", "twitter");
    }

    QString mainTempDir = config["tempDir"].toString();
    if (mainTempDir.isEmpty()) mainTempDir = userDir + "/.tmp";
    int tweetCount = 0;
    int mediaCount = 0;
    DiskJsonBuffer collectedData(mainTempDir, "tw_"+ excelSuffix);
    QSet<QString> collectedIds;
    int currentAccountIdx = 0;

    // ── 아니포 방식: 기존 Excel에서 ID 로드 + progress 파일에서 진행 상태 로드 ──
    // 아니포 호환: tweets → complete
    QString excelFileSuffix = (excelSuffix == "tweets") ? "complete": excelSuffix;
    QString excelPath = userDir + "/"+ target + "_"+ excelFileSuffix + ".xlsx";
    // fallback: 기존 _tweets.xlsx도 확인
    //   ★ 옛 엑셀에서 ID 를 읽은 판은 새 _complete 에 이번 것만 담는다 — 그 판의 표시로 다음 판이 일찍 멈추면 옛 줄이 _complete 로
    //   영영 안 옮겨졌다(검토). 그런 판은 표시를 적지도 믿지도 않는다(끝까지 보아 _complete 를 채운다)
    bool legacyExcel = false;
    if (!QFile::exists(excelPath) && excelSuffix == "tweets") {
        QString oldPath = userDir + "/"+ target + "_tweets.xlsx";
        if (QFile::exists(oldPath)) { excelPath = oldPath; legacyExcel = true; }
    }
    QString progressPath = userDir + "/.progress_"+ excelSuffix + ".json";
    QString resumeOldestDate;  // progress: 가장 오래된 트윗 날짜 (과거 이어서용 since/until)
    QString resumeNewestDate;  // progress: 가장 새로운 트윗 날짜 (현재 이어서용)
    QString resumeNewestId;    // progress: 가장 새로운 트윗 ID (since_id 기반 정확 분리)
    // ★ saveProgress lambda가 캡쳐하므로 lambda 정의 전 미리 선언
    QDateTime newestTweetDate;
    bool searchSeenRun = false;   // 이번 판의 검색이 이 계정의 글을 하나라도 돌려줬나 — 진행 기록에 searchSeen 으로 남긴다(r56, 아래 설명)
    // ★ resume 모드 — UI dropdown 또는 config["resumeMode"]
    //   "past":   resumeOldestDate 이전만 검색 (마지막 다운한 것보다 더 과거 트윗)
    //   "future": resumeNewestDate/Id 이후만 검색 (마지막 다운 후 새로 올라온 트윗)
    //   "both" / 기본: 양방향 (과거로 backtrack + 현재 새 트윗 둘 다)
    QString resumeMode = config["resumeMode"].toString();
    if (resumeMode.isEmpty()) resumeMode = "both";
    // ★ 화면의 방식 '새 글만'(mode=incremental). 예전엔 아무도 이 값을 읽지 않아 '전체(2006년부터)' 와
    //   똑같이 과거 끝까지 훑었다. 지난번 가장 새 트윗 이후만 본다(= 이어서 수집 + 현재에서 이어서).
    if (mode == "incremental") { resumeMode = "future"; m_saveProgress = true; }

    if (QFile::exists(excelPath)) {
        QXlsx::Document doc(excelPath);
        int lastRow = doc.dimension().lastRow();
        for (int r = 2; r <= lastRow; ++r) {  // 1행은 헤더
            QVariant val = doc.read(r, 1);  // 1열 = id
            QString id = val.toString().trimmed();
            if (!id.isEmpty()) collectedIds.insert(id);
        }
        if (!collectedIds.isEmpty()) {
            tweetCount = collectedIds.size();
            m_backend->log(QString("기존 파일: %1개 수집됨 → 이어서 수집").arg(tweetCount), "info", "twitter");
        }
    }

    // ★ 미디어를 받지 못한 글은 다음 판에 다시 받는다(r56, COV-3). 예전엔 줄은 엑셀에 들어가고(그 규칙은 그대로 — 엑셀이 먼저다)
    //   다음 판은 엑셀의 ID 를 '받은 것' 으로 건너뛰어, 시간 초과 · 끊긴 회선 · 받는 도중의 중지로 놓친 그림 · 영상을 영영 받지 않았다.
    //   이제 그런 글을 엑셀 옆의 작은 파일(.hanishiki_media_retry_<엑셀 이름>.json — {ID: {tries, last, dir, orig}})에 적고, 다음 판이
    //   걸음을 시작하기 전에 그 글만 TweetDetail 로 다시 받아 미디어를 받아 본다(아래 '다시 받기'). 걸음이 그 글에 다시 닿지 않아도 된다.
    //   받으면 지우고, 다섯 번 다시 해도 안 되면 한 줄 알리고 그만둔다(지워진 글 · 막힌 영상).
    const QString mediaRetryPath = userDir + "/.hanishiki_media_retry_" + target + "_" + excelFileSuffix + ".json";
    QJsonObject mediaRetry = readJsonFile(mediaRetryPath);
    bool mediaRetryWriteFailed = false;   // 적지 못했다 — 그 판은 일찍 멈춤 표시를 남기지 않는다(다음 판이 끝까지 보며 다시 받게)
    auto saveMediaRetry = [&]() {
        if (writeJsonFile(mediaRetryPath, mediaRetry)) return;
        if (!mediaRetryWriteFailed)
            m_backend->log("받지 못한 미디어 목록을 적지 못했습니다: " + mediaRetryPath + " — 이번 판은 일찍 멈춤 표시를 남기지 않습니다(다음 판이 끝까지 보며 다시 받습니다)",
                           "error", "twitter");
        mediaRetryWriteFailed = true;
    };
    auto noteMediaFailure = [&](const QString &id, const QString &origId, const QString &sub) {
        if (id.isEmpty()) return;
        QJsonObject e = mediaRetry.value(id).toObject();
        if (!e.contains("tries")) e["tries"] = 0;   // 다시 받기를 한 횟수(처음 놓친 것은 세지 않는다)
        e["last"] = Common::jstNow().toString(Qt::ISODate);
        e["dir"] = sub;                              // media/ 아래 어느 갈래에 받았어야 했나(tweets · replies · reposts)
        if (!origId.isEmpty()) e["orig"] = origId;   // 리트윗이면 원글 — 미디어는 원글에 있다
        mediaRetry[id] = e;
        saveMediaRetry();   // 놓친 그 자리에서 바로 적는다 — 판이 죽어도 남게(엑셀보다 먼저 적혀도 다시 받기만 할 뿐 줄을 만들지 않는다)
    };

    // 최대 수집 수는 이번 판에 새로 받은 것만 센다 — 위에서 불러온 기존 개수는 뺀다.
    const int runStartCount = tweetCount;
    auto capReached = [&]() { return maxCount > 0 && tweetCount - runStartCount >= maxCount; };

    // ── 다 받은 곳에서 일찍 멈추기 — '이미 받은' 은 이번 판을 시작하기 전의 엑셀 ID 뿐이다.
    //   이번 판에 처음 받은 것은 세지 않는다: 2단계(UserTweets)는 1단계(검색)가 방금 받은 것을 다시 보고,
    //   검색 되짚기도 날짜 경계에서 겹친다. 그것까지 세면 사이사이 끼인 리트윗에 닿기 전에 멈춘다.
    //   대화 묶음(profile-conversation)은 옛 앞 글(대개 받아 둔 것)과 새 답글을 섞어 오므로 5개보다 작게 두지 않는다.
    const QSet<QString> knownAtStart = (m_allKnownSnapshotSet && excelSuffix == "tweets" && stopAfterKnown > 0) ? m_allKnownSnapshot : collectedIds;
    // ★ 단계마다(검색 · 타임라인 · 답글 검색) 따로 — 지난번에 그 목록을 끊김 없이 다 본 적이 있어야 그 단계가 일찍 멈춘다.
    //   표시는 그 단계를 시작할 때 지우고, 그 단계가 끊김 없이 끝나면(끝까지 · 일찍 멈춤) 다시 적는다. 끊긴 판(중지 · 최대 수 ·
    //   오류)이 새 글의 위쪽만 받아 두면 다음 판이 그 위에서 멈춰 아래쪽 새 글을 영영 놓쳤다. 한 표시로 세 목록을 함께 다루면
    //   깨끗이 끝난 답글 검색이 끊긴 타임라인을 '다 봤다' 고 적었다(검토).
    const int knownN = stopAfterKnown > 0 ? qMax(5, stopAfterKnown) : 0;
    //   표시 이름 = tw_<엑셀>_<목록>_m<미디어> — 미디어를 끈 판의 표시로 켠 판이 일찍 멈추면 미디어를 영영 안 받았다(검토)
    const QString mScope = m_downloadMedia ? "_m1" : "_m0";
    auto walkKind = [&](const QString &list) { return "tw_" + excelSuffix + "_" + list + mScope; };
    // ★ 그 엑셀에 줄을 넣는 판은 시작할 때 그 엑셀의 표시를 모두 가져간다(search · timeline · replies). 표시에는 그 목록이
    //   끊김 없이 다 본 가장 새 트윗 ID(top)가 함께 적힌다 — 트윗 ID 는 시각 순이고 이 셋은 새것부터 내려온다. 일찍 멈춤은 받아 둔
    //   트윗 가운데 top 이하만 센다. 그보다 새로 받아 둔 것(다른 목록 · 기간 · 새 글만 · 양방향 · 끊긴 판이 넣은 위쪽 조각)은 그 목록이
    //   본 적 없는 사이 위에 있을 수 있어 세지 않고 지나간다. 그래서 끊긴 판 · 걷지 않은 목록의 표시는 읽은 그대로 되돌려 둔다
    //   (아래 restoreGuard). 예전엔 그런 판마다 표시를 지워 다음 판이 끝까지 보았고, 한 계정을 '트윗' · '답글' 두 줄로 두면 서로의
    //   표시를 지워 일찍 멈춤이 듣지 않았다(검토). '전체' 판은 하위 판(트윗 → 답글)이 같은 엑셀을 쓰므로 '전체' 가 한 번 가져가 나눠 쓴다.
    const bool walkTypes = (type == "tweets" || type == "media" || type == "replies" || type == "tweets_api");
    const bool sharedWalk = walkTypes && m_allKnownSnapshotSet && excelSuffix == "tweets";
    const QString walkPre = "tw_" + excelSuffix + "_";
    QSet<QString> armedLists;               // 이름은 '<목록><미디어 범위>' (search_m1 …)
    QHash<QString, qulonglong> listTop;     // 그 목록이 끊김 없이 다 본 가장 새 트윗 ID — 없으면(옛 표시) 이번엔 끝까지 본다
    if (walkTypes) {
        if (sharedWalk) armedLists = m_allArmedLists;
        else for (const QString &k : Common::takeCleanWalkMarks(userDir, walkPre)) armedLists.insert(k.mid(walkPre.size()));
        if (legacyExcel) armedLists.clear();
        for (const QString &l : armedLists)
            listTop.insert(l, Common::takenWalkMark(userDir, walkPre + l).value("top").toString().toULongLong());
    }
    // 끊긴 걸음 · 걷지 않은 목록 · 중간에 돌아가는 판 — 읽은 표시를 그대로 되돌린다(이미 다시 적은 것은 건너뛴다).
    //   '전체' 의 하위 판은 '전체' 가 끝에 되돌린다. 옛 엑셀에서 읽은 판은 되돌리지 않는다(지운 채 — 다음 판이 끝까지 본다)
    ScopeExit restoreGuard{(walkTypes && !sharedWalk && !legacyExcel)
        ? std::function<void()>([userDir, walkPre, lists = armedLists]() {
              for (const QString &l : lists) Common::restoreWalkMark(userDir, walkPre + l);
          })
        : std::function<void()>()};
    auto takeWalk = [&](const QString &list) -> bool {
        const bool armed = armedLists.contains(list + mScope) && listTop.value(list + mScope) > 0;
        if (knownN > 0 && !armed && !knownAtStart.isEmpty())
            m_backend->log(QString("%1: 지난번에 끝까지 가지 못해(또는 처음이라) 이번엔 끝까지 봅니다 — 빠진 사이를 메웁니다").arg(list), "info", "twitter");
        return armed;
    };
    qulonglong walkCurTop = 0;    // 지금 걷는 목록의 top — 이 이하의 받아 둔 트윗만 센다
    qulonglong walkSeenTop = 0;   // 이번 걸음에서 본 가장 새 트윗 ID — 끊김 없이 끝나면 새 top
    qint64 walkStartMs = 0;       // 이 목록의 걸음을 시작한 때
    QString walkList;
    auto putWalk = [&](const QString &list) {
        if (legacyExcel) return;
        if (mediaRetryWriteFailed) return;   // 받지 못한 미디어를 기억하지 못한 판 — 위 noteMediaFailure 설명
        // ★ 표시는 지금까지 모은 줄을 다 저장한 뒤에만 남긴다 — 중간 저장을 띄엄띄엄 하게 되어(excelCheckpointDue) 표시만 남고
        //   줄은 빠지는 틈이 커졌다(저장 실패 · 끄기 · 죽음 — 검토). 저장에 실패한 판은 표시를 옮기지 않는다(saveProgress 와 같다).
        if (collectedData.count() > 0) saveExcelStreaming(userDir, target, collectedData, excelSuffix);
        if (m_excelSaveFailed) {
            m_backend->log(QString("'%1' 의 일찍 멈춤 표시는 그대로 둡니다 — 이번 판에 엑셀을 저장하지 못해, 다음 판이 끝까지 봅니다").arg(list), "warning", "twitter");
            return;
        }
        qulonglong seen = list == walkList ? walkSeenTop : 0ULL;
        if (list != "timeline") {
            // 검색(search · replies)은 색인이 늦게 붙는 글이 있다 — 걸음을 시작하기 6시간 전보다 새것은 top 에 넣지 않는다. 그 사이는
            //   다음 판이 세지 않고 다시 본다(늦게 붙은 글이 그 사이에 나타나면 받는다)
            const qint64 lagMs = walkStartMs - 6LL * 3600 * 1000 - 1288834974657LL;
            seen = qMin(seen, lagMs > 0 ? (qulonglong(lagMs) << 22) : 0ULL);
        }
        const qulonglong top = qMax(listTop.value(list + mScope), seen);
        Common::putCleanWalkMark(userDir, walkKind(list), QJsonObject{{"top", QString::number(top)}});
    };
    bool streakOn = false;      // 이 단계가 맨 위(새 글)부터 내려오는가 — 과거 쪽 이어가기 · 기간 · 현재 이어서(since:)는 세지 않는다
    int knownStreak = 0;
    QSet<QString> streakCounted;   // 이 단계에서 이미 센 글
    bool knownStop = false;     // 이 단계를 여기서 끝낸다
    int knownStopMax = 0;       // 이번 판에 일찍 멈춘 적이 있으면 그때의 연속 수(대상 목록에 남김)
    qint64 newestActivityMs = 0;   // 이 계정의 가장 새 활동(제 글 · 리트윗) — 트윗 ID 는 시각을 품는다
    auto beginStreak = [&](bool on, const QString &list) {
        streakOn = on && knownN > 0;
        knownStreak = 0;
        knownStop = false;
        streakCounted.clear();
        walkCurTop = listTop.value(list + mScope);
        if (list != walkList) { walkList = list; walkSeenTop = 0; walkStartMs = QDateTime::currentMSecsSinceEpoch(); }   // 답글은 두 걸음(맨 위 · 되짚기)이 한 목록이다
    };
    auto stopNow = [&]() { return capReached() || knownStop; };

    // ★ '마지막으로 본 트윗 ID' 만은 체크박스와 무관하게 읽는다.
    //   m_newestTweetId 는 이번 판에서 실제로 처리한 트윗에서만 채워진다. 그런데
    //   이 앱의 보통 쓰임인 '현재 이어서' 는 새 글이 0개인 날이 대부분이라,
    //   그런 날엔 이 값이 빈 채로 남는다. 그러면 '새 트윗 확인' 이 입구에서
    //   "아직 수집된 트윗이 없습니다" 로 돌아선다 — 받아 둔 것이 수백 개여도 그렇다.
    //   (실측 2026-09-11: 757개를 받아 둔 폴더에서 수집 직후에도 여섯 번 연속 그 메시지)
    //   이 값은 '무엇을 받을지' 를 정하지 않는다. 아래 resume 범위 계산과는 무관하고,
    //   오직 새 트윗 확인이 어디서부터 볼지에만 쓰인다. 그래서 체크박스와 분리한다.
    if (m_newestTweetId.isEmpty() && QFile::exists(progressPath)) {
        QFile nf(progressPath);
        if (nf.open(QIODevice::ReadOnly)) {
            const QString savedNewest =
                QJsonDocument::fromJson(nf.readAll()).object()["newestId"].toString();
            nf.close();
            if (!savedNewest.isEmpty()) m_newestTweetId = savedNewest;
        }
    }

    // ★ progress 파일은 m_saveProgress(=이어서 수집 체크박스)가 true일 때만 읽음.
    //   체크 해제 시 사용자가 처음부터 다시 받기를 원하는 의도 → progress.json 무시.
    //   (저장은 항상 — 다음 실행 때 사용자가 체크하면 사용 가능)
    if (m_saveProgress && QFile::exists(progressPath)) {
        QFile pf(progressPath);
        if (pf.open(QIODevice::ReadOnly)) {
            QJsonObject prog = QJsonDocument::fromJson(pf.readAll()).object();
            pf.close();
            resumeOldestDate = prog["oldestDate"].toString();
            resumeNewestDate = prog["newestDate"].toString();
            resumeNewestId = prog["newestId"].toString();
            mediaCount = prog["mediaCount"].toInt();
            // resume 모드별 안내 — 어느 방향으로 이어서 가는지 명확히
            if (resumeMode == "past" && !resumeOldestDate.isEmpty()) {
                m_backend->log(QString("✓ [과거 이어서] %1 이전 트윗만 수집").arg(resumeOldestDate),
                               "info", "twitter");
            } else if (resumeMode == "future" && (!resumeNewestDate.isEmpty() || !resumeNewestId.isEmpty())) {
                m_backend->log(QString("✓ [현재 이어서] %1 이후 새 트윗만 수집")
                                   .arg(resumeNewestDate.isEmpty() ? resumeNewestId : resumeNewestDate),
                               "info", "twitter");
            } else if (resumeMode == "both" && !resumeOldestDate.isEmpty()) {
                m_backend->log(QString("✓ [양방향 이어서] %1 이전 + 새 트윗 둘 다").arg(resumeOldestDate),
                               "info", "twitter");
            }
        }
    }

    // ★ 위쪽(새 글 쪽) 이어짐. progress 의 newestDate/Id 는 '이보다 새 것은 다 받았다' 는 약속이다.
    //   예전엔 이번 판에서 본 가장 새 트윗으로 무조건 올렸다 — 새 글 쪽 훑기가 오류·중지·50쪽 안전장치·
    //   기간 지정 때문에 지난번 가장 새 트윗까지 내려오지 못했어도 그랬다. 그러면 그 사이가 다음
    //   '현재 이어서/양방향' 에서 영영 빠진다(since: 는 새 값 위에서 시작하고, 과거 쪽은 oldest 밑만 본다).
    //   → 맨 위에서 이어 내려오며 지난번 가장 새 트윗(이하)을 실제로 만났거나, since: 구간을 끝까지
    //     봤을 때만 올린다. 아니면 예전 값을 지킨다 — 다음 판이 그 자리부터 다시 본다(중복은 ID 로 걸러진다).
    QString prevNewestBoundary;
    bool prevSearchSeen = false;   // 앞선 판의 검색이 이 계정의 글을 본 적이 있다(진행 기록의 searchSeen — r56)
    if (QFile::exists(progressPath)) {
        QFile bf(progressPath);
        if (bf.open(QIODevice::ReadOnly)) {
            const QJsonObject bo = QJsonDocument::fromJson(bf.readAll()).object();
            prevNewestBoundary = bo["newestId"].toString();
            prevSearchSeen = bo["searchSeen"].toBool();
            bf.close();
        }
    }
    bool forwardScan = true;                                // 지금 훑는 것이 맨 위에서 이어 내려오는 중인가
    bool reachedPrevNewest = prevNewestBoundary.isEmpty();  // 지난 기록이 없으면 지킬 것도 없다
    // ★ 새 글만 · 양방향 1단계의 걸음이 끊겼으면(오류 · 구멍 · 모양 없는 응답 — r56) 이번 판은 가장 새 쪽 표시를 올리지 않는다.
    //   그 뒤의 타임라인 단계가 지난번 가장 새 글을 만나도 마찬가지다 — 타임라인에는 답글이 없어, 검색이 놓친 새 답글을 메우지 못한다.
    bool newestLocked = false;

    // Helper: progress 파일 저장 — oldest/newest 둘 다 누적
    //   기존 파일에 이미 newestDate가 있으면 그보다 새로운 것만 갱신 (newer wins)
    //   기존 oldestDate가 있으면 그보다 오래된 것만 갱신 (older wins)
    auto saveProgress = [&](const QString &oldestDate) {
        // ★ 기간 판은 표시를 옮기지 않는다 — 기간만 본 판의 가장 옛 날짜를 적으면(옛것이 이긴다) 다음 '과거에서 이어서' 가
        //   지난 경계와 그 기간 사이를 통째로 건너뛰었다(검토 — tweets_api 기간 판도 이제 같은 파일을 쓴다). 가장 새 쪽도 같다.
        if (mode == QLatin1String("period")) {
            m_backend->log("기간 판이라 이어받기 표시는 그대로 둡니다(기간 밖은 보지 않았습니다)", "info", "twitter");
            return;
        }
        // ★ 엑셀을 저장하지 못한 판은 표시를 옮기지 않는다 — 옮기면 다음 판이 '지난번 이후' 만 보아 이번 판의 글이 엑셀에서 영영 빠진다
        if (m_excelSaveFailed) {
            m_backend->log("이어받기 표시를 그대로 둡니다 — 이번 판에 엑셀을 저장하지 못해, 다음 판이 이 구간을 다시 받습니다", "warning", "twitter");
            return;
        }
        QJsonObject prev;
        if (QFile::exists(progressPath)) {
            QFile pfr(progressPath);
            if (pfr.open(QIODevice::ReadOnly)) {
                prev = QJsonDocument::fromJson(pfr.readAll()).object();
                pfr.close();
            }
        }
        // oldestDate: older wins
        // ★ 중지로 끝난 판의 '가장 오래된 날짜' 는 믿을 수 없다. 검색 루프는 처리하기 전에 배치 전체
        //   (한 쪽 최대 40개 — 드물게 올리는 계정이면 몇 달 치)의 날짜로 iterOldestDate·oldestTweetDate 를 정하고,
        //   중지는 processTweetBatch 가 그 배치 한가운데서 끊는다. 그 날짜를 적으면 다음 '과거 이어서' 의
        //   until:(그 날 제외) 가 처리 못 한 트윗들과 그 날의 나머지를 건너뛴다. 루프 안 저장(nextUntil)도
        //   중지된 판에서 이 날짜를 적으므로 마지막 저장만 고쳐서는 안 된다 — 여기서 함께 막는다.
        //   중지된 판은 지난 값을 지킨다(끝까지 돈 iteration 은 돌던 중에 이미 적었다).
        QString outOldest = isRunning ? oldestDate : prev["oldestDate"].toString();
        QString prevOldest = prev["oldestDate"].toString();
        if (outOldest.isEmpty()) outOldest = prevOldest;   // 빈 값 = 지난 값(일찍 멈춘 판) — 빈 칸으로 덮지 않는다
        if (!prevOldest.isEmpty()) {
            QDate prevD = QDate::fromString(prevOldest, "yyyy-MM-dd");
            QDate curD  = QDate::fromString(outOldest, "yyyy-MM-dd");
            if (prevD.isValid() && curD.isValid() && prevD < curD) outOldest = prevOldest;
        }
        // newestDate/Id: newer wins (m_newestTweetId 기준 — 이번 세션의 최신)
        QString outNewestId = m_newestTweetId;
        QString prevNewestId = prev["newestId"].toString();
        if (!prevNewestId.isEmpty()
            && (outNewestId.isEmpty() || !reachedPrevNewest
                || prevNewestId.toLongLong() > outNewestId.toLongLong())) {
            outNewestId = prevNewestId;
        }
        // newestDate: newer wins
        QString outNewestDate = prev["newestDate"].toString();
        if (newestTweetDate.isValid() && reachedPrevNewest) {
            QString curND = newestTweetDate.toString("yyyy-MM-dd");
            QDate prevD = QDate::fromString(outNewestDate, "yyyy-MM-dd");
            QDate curD  = QDate::fromString(curND, "yyyy-MM-dd");
            if (!prevD.isValid() || (curD.isValid() && curD > prevD)) outNewestDate = curND;
        }

        QJsonObject prog;
        prog["oldestDate"] = outOldest;
        prog["newestDate"] = outNewestDate;
        prog["newestId"]   = outNewestId;
        prog["tweetCount"] = tweetCount;
        prog["mediaCount"] = mediaCount;
        // ★ 검색이 이 계정을 본 적이 있다는 표시(r56 · tw-depth F8). 검색 오류로 끝난 걸음은 바닥 · 걸음 표시를 남기지 않아, 다음 '과거에서
        //   이어서' 가 공백을 만나면 '검색에 안 보이는 계정' 으로 보고 빈 결과 10번(70일)에서 멈췄다. 한 번 본 적이 있으면 그것도 증거로 친다.
        prog["searchSeen"] = prev["searchSeen"].toBool() || searchSeenRun;
        prog["timestamp"]  = Common::jstNow().toString(Qt::ISODate);
        QFile pf(progressPath);
        if (pf.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            pf.write(QJsonDocument(prog).toJson(QJsonDocument::Compact));
            pf.close();
        }
    };

    m_backend->log("수집 시작...", "info", "twitter");

    // 타겟 프로필 사진/배너 저장 — 통일된 구조: profiles/target/{이름(@handle)}/
    {
        QString dateTag = Common::jstToday().toString("yyyyMMdd");
        QString tDispName = userName;
        tDispName.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
        tDispName = tDispName.trimmed().left(40);
        QString tPrefix = !tDispName.isEmpty() ? QString("%1(@%2)").arg(tDispName, target) : target;
        tPrefix = FileHelper::portableName(tPrefix);   // FileHelper.h 설명
        QString tDirN = tPrefix;
        tDirN.remove(QRegularExpression("[/\\\\:*?\"<>|]"));
        tDirN = FileHelper::reuseExistingName(userDir + "/profiles/target", tDirN);
        QString profileDateDir = userDir + "/profiles/target/"+ tDirN;
        QDir().mkpath(profileDateDir);

        // Profile image — 원본 먼저, 안 되면 400x400 · 받은 그대로(COV-11). 주소가 바뀌었을 때만 새로 받는다(saveProfileImage 설명)
        const auto dl = [this](const QString &u, const QString &f) { return downloadMedia(u, f); };
        const QString profileImgUrl = userLegacy["profile_image_url_https"].toString();
        saveProfileImage(profileDateDir, QStringLiteral("profile"), profileImgUrl, avatarCandidates(profileImgUrl), dateTag, dl, avatarLegacyUrl(profileImgUrl));

        // Banner image
        QString bannerUrl = userLegacy["profile_banner_url"].toString();
        if (!bannerUrl.isEmpty())
            saveProfileImage(profileDateDir, QStringLiteral("banner"), bannerUrl, {bannerUrl + "/1500x500", bannerUrl}, dateTag, dl);
    }

    // Lambda: process a batch of tweets (shared by UserTweets and SearchTimeline)
    QDateTime oldestTweetDate; // Track oldest tweet for SearchTimeline fallback
    // newestTweetDate는 위 progress 영역에 미리 선언됨 (saveProgress lambda 캡쳐용)
    auto processTweetBatch = [&](const QJsonArray &tweets) -> bool {
        bool pastRange = false;
        for (const auto &tweetVal : tweets) {
            if (!isRunning || capReached()) break;

            QJsonObject tweet = tweetVal.toObject();
            QJsonObject legacy = xLegacy(tweet);
            QString tweetId = legacy["id_str"].toString();
            if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();

            // ★ 고정 트윗(타임라인 첫 쪽의 TimelinePinEntry — r56)은 시간 순서 밖이다. 대개 옛 글이라 '지난번 가장 새 트윗에 닿았다' ·
            //   '받아 둔 것이 이어진다' 로 세면 위쪽이 비었는데도 이어짐을 적거나 일찍 멈춘다 — 그 둘에는 넣지 않는다.
            const bool pinnedItem = tweet.value(QStringLiteral("pinned")).toBool();
            // ★ 위쪽 이어짐 — 지난번 가장 새 트윗(이하)을 만났는가. 이미 받은 트윗도 센다(그래서 Dedup 앞).
            if (forwardScan && !reachedPrevNewest && !newestLocked && !pinnedItem && !tweetId.isEmpty()
                && tweetId.toLongLong() <= prevNewestBoundary.toLongLong())
                reachedPrevNewest = true;

            // 가장 새 활동 — 받아 둔 것이든 새것이든(뜸한 계정 판정). 스노플레이크 ID: (id >> 22) + 1288834974657 ms
            if (const qint64 idNum = tweetId.toLongLong(); idNum > 0)
                newestActivityMs = qMax(newestActivityMs, (idNum >> 22) + 1288834974657LL);
            const qulonglong idU = tweetId.toULongLong();
            if (idU > walkSeenTop) walkSeenTop = idU;
            // ★ top 보다 새것을 만나면 아직 top 아래에 닿지 않은 것이다 — 연속을 다시 센다. 타임라인의 대화 묶음은 옛 뿌리(받아 둔 것 · top 이하)를
            //   그 새 답글 위에 두어, 묶음 다섯이면 그 아래의 리트윗(검색에 안 나온다)에 닿기 전에 멈추고 top 을 그 위로 올렸다(검토)
            if (streakOn && idU > walkCurTop && !pinnedItem) knownStreak = 0;

            // 타임라인의 고정 칸(TimelinePinEntry)으로 온 글은 받아 둔 글이어도 고정한 글이다 — 저장 때 📌 를 맞춘다(r56 검토 DC-2)
            if (pinnedItem && !tweetId.isEmpty()) m_pinnedIds.insert(tweetId);

            // Dedup
            if (collectedIds.contains(tweetId)) {
                // 받아 둔 것 가운데 이 목록의 top 이하만 센다 — 그 위의 받아 둔 것은 다른 걸음이 넣은 것이라 그 아래에 사이가 있을 수 있다
                //   한 글은 한 번만 — 검색이 그 날을 다시 포함해 이으므로(D+1) 같은 글을 또 만난다. 두 번 세면 N 개가 아닌데도 멈췄다(검토)
                if (streakOn && !pinnedItem && knownAtStart.contains(tweetId) && idU <= walkCurTop && !streakCounted.contains(tweetId)) {
                    streakCounted.insert(tweetId);
                    if (++knownStreak >= knownN) {
                        knownStop = true;
                        knownStopMax = qMax(knownStopMax, knownStreak);
                        break;
                    }
                }
                continue;
            }

            // Period filter
            QString createdAt = legacy["created_at"].toString();
            QDateTime tweetDate = Common::parseISODate(createdAt);
            if (untilDate.isValid() && tweetDate > untilDate) continue;
            // ★ 고정 글은 시간 순서 밖이라(대개 기간보다 옛 글) '기간의 시작보다 옛 글에 닿았다' 로 치지 않는다 — 타임라인 첫 쪽의 맨 앞에 와서
            //   그 판을 첫 글에서 끝냈다(검토 XAPI-1, 기간 타임라인)
            if (sinceDate.isValid() && tweetDate < sinceDate) { if (pinnedItem) continue; pastRange = true; break; }

            // Track oldest tweet date for SearchTimeline fallback
            if (!oldestTweetDate.isValid() || tweetDate < oldestTweetDate) {
                oldestTweetDate = tweetDate;
            }

            // Check if retweet
            bool isRetweet = false;
            QJsonObject rtResult;
            if (legacy.contains("retweeted_status_result")) {
                rtResult = legacy["retweeted_status_result"].toObject()["result"].toObject();
                isRetweet = true;
            }
            if (rtResult.isEmpty() && tweet.contains("retweeted_status_result")) {
                rtResult = tweet["retweeted_status_result"].toObject()["result"].toObject();
                isRetweet = !rtResult.isEmpty();
            }
            if (!rtResult.isEmpty() && rtResult.contains("tweet"))
                rtResult = rtResult["tweet"].toObject();

            // Check if reply
            bool isReply = !legacy["in_reply_to_status_id_str"].toString().isEmpty();

            // Filter: media only
            if (filterMediaOnly) {
                QJsonObject entities = legacy["extended_entities"].toObject();
                QJsonArray mediaArr = entities["media"].toArray();
                if (mediaArr.isEmpty()) {
                    entities = legacy["entities"].toObject();
                    mediaArr = entities["media"].toArray();
                }
                if (mediaArr.isEmpty() && isRetweet && !rtResult.isEmpty()) {
                    QJsonObject rtLeg = xLegacy(rtResult);
                    mediaArr = rtLeg["extended_entities"].toObject()["media"].toArray();
                    if (mediaArr.isEmpty()) mediaArr = rtLeg["entities"].toObject()["media"].toArray();
                }
                if (mediaArr.isEmpty()) continue;
            }

            // Filter: replies only
            if (filterRepliesOnly && !isReply) continue;

            collectedIds.insert(tweetId);
            knownStreak = 0;   // 새 트윗 — 이어짐이 끊겼다(기간 · 미디어만 거르기로 넘긴 것은 세지도 끊지도 않는다)
            // Track newest tweet ID + date (for 새트윗확인 + future resume)
            if (m_newestTweetId.isEmpty() || tweetId.toLongLong() > m_newestTweetId.toLongLong()) {
                m_newestTweetId = tweetId;
                if (tweetDate.isValid()) newestTweetDate = tweetDate;
            }
            // ID는 Excel 저장 시 자동 보존됨 (아니포 방식)
            tweetCount++;

            // 엑셀 한 줄 — 좋아요 · 북마크와 같은 함수로 만든다(같은 칸 · 같은 뜻)
            bool isSpaceCard = false;
            QJsonObject data = standardTweetRow(tweet, target, &isSpaceCard);
            const QString tweetUrl = data["tweet_url"].toString();
            m_backend->log(QString("[%1] %2...").arg(tweetCount).arg(data["text"].toString().left(40)), "info", "twitter");
            m_backend->updateStats(tweetCount, mediaCount, "수집 중...", "twitter");

            // Download media (타입별 폴더 분리: tweets/replies/reposts)
            QString tweetMediaDir;
            if (isRetweet) tweetMediaDir = mediaDir + "/reposts";
            else if (isReply) tweetMediaDir = mediaDir + "/replies";
            else tweetMediaDir = mediaDir + "/tweets";
            QDir().mkpath(tweetMediaDir);
            const int mfBefore = m_mediaFailures;
            int newMedia = downloadTweetMedia(tweet, tweetMediaDir, tweetCount);
            mediaCount += newMedia;
            // 받지 못한 미디어(받는 도중의 중지 · 끄기도) — 다음 판에 다시 받는다(위 noteMediaFailure 설명)
            if (m_mediaFailures > mfBefore) {
                QString origId = xLegacy(rtResult)["id_str"].toString();
                if (origId.isEmpty()) origId = rtResult["rest_id"].toString();
                noteMediaFailure(tweetId, isRetweet ? origId : QString(),
                                 isRetweet ? QStringLiteral("reposts") : isReply ? QStringLiteral("replies") : QStringLiteral("tweets"));
            }
            if (isReply) m_runReplyIds << tweetId;   // Phase 4(대화 미디어)가 읽는다 — 저장이 버퍼를 비워도 남게(r56, F10)

            // 진행 로그
            if (tweetCount > 0 && tweetCount % 100 == 0) {
                m_backend->log(QString("진행: %1개, 미디어 %2개").arg(tweetCount).arg(mediaCount), "info", "twitter");
            } else if (tweetCount % 10 == 0) {
                m_backend->log(QString("트윗 %1개, 미디어 %2개").arg(tweetCount).arg(mediaCount), "info", "twitter");
            }

            // 캡쳐: realCapture=true이면 SingleFile+CDP 실페이지, false면 합성 카드.
            // 통합 헬퍼 captureTweet()으로 위임 — 모든 type 분기 일관 처리.
            // ★ realCaptureMode — "off": skip / "media": 미디어 있을 때만 / "all": 모두 (기본)
            if (tweetWantsCapture(tweet, config)) captureTweet(tweet, userDir + "/captures", config);

            // Download RT/quoted user profile pics & banners (anipo style)
            QString profileDir = userDir + "/profiles";
            downloadUserProfileMedia(tweet, profileDir);

            // 🎙️ 스페이스 카드 트윗 — status URL 을 모아 전체수집 끝에 yt-dlp 로 추출/다운로드.
            //   (호스팅/공유한 스페이스는 URL 엔티티가 아니라 카드라서, status URL 을 yt-dlp 에 넘겨 추출)
            if (isSpaceCard && !tweetUrl.isEmpty()) m_spaceCardTweetUrls.insert(tweetUrl);

            collectedData.append(data);

            // 중간 저장 — 크래시 때 잃는 것을 줄인다. 예전엔 50개마다 저장해, 큰 계정에서는 저장마다 파일 전체를 다시 써 판 전체가
            //   제곱으로 느려졌다(10만 개면 2,000번). 이제 50개가 쌓였고 저장할 때가 됐을 때만(excelCheckpointDue). 쌓인 것은
            //   디스크 버퍼(DiskJsonBuffer)에 있어 메모리는 그대로이고, 판의 끝 · 이어받기 표시 앞의 저장은 그대로다.
            if (tweetCount % 50 == 0 && excelCheckpointDue()) {
                m_backend->log(QString("저장 (%1개, 미디어 %2개)").arg(tweetCount).arg(mediaCount), "info", "twitter");
                saveExcelStreaming(userDir, target, collectedData, excelSuffix);
            }
        }
        return pastRange || stopNow();   // 최대 수집 수 · 일찍 멈춤에 닿아도 '그만' 신호
    };

    // ── r56: 지난 판에 받지 못한 미디어 다시 받기(COV-3 — 위 noteMediaFailure 설명) ──
    //   걸음을 시작하기 전에 한다. '전체' 에서는 트윗 하위 판이 한 번만(답글 · 스레드 하위 판이 같은 파일을 또 세지 않게).
    if ((!isSubCall || type == "tweets") && m_downloadMedia && !mediaRetry.isEmpty() && isRunning) {
        const QStringList retryIds = mediaRetry.keys();
        m_backend->log(QString("지난 판에 미디어를 받지 못한 글 %1개 — 다시 받아 봅니다").arg(retryIds.size()), "info", "twitter");
        int fixed = 0;
        for (const QString &id : retryIds) {
            if (!isRunning) break;
            QJsonObject e = mediaRetry.value(id).toObject();
            const QString fetchId = e["orig"].toString().isEmpty() ? id : e["orig"].toString();
            QString st;
            const QJsonObject tw = fetchTweetById(fetchId, &st);
            if (st == QLatin1String("RATE_LIMITED")) {
                m_backend->log("다시 받기: 요청 한도에 닿았습니다 — 남은 것은 다음 판에 다시 받습니다", "warning", "twitter");
                break;
            }
            bool ok = false;
            if (st == QLatin1String("OK")) {
                QString sub = e["dir"].toString();
                if (sub != QLatin1String("replies") && sub != QLatin1String("reposts")) sub = QStringLiteral("tweets");
                const QString dir = mediaDir + "/" + sub;
                QDir().mkpath(dir);
                const int mfBefore = m_mediaFailures;
                mediaCount += downloadTweetMedia(tw, dir, 0);
                ok = (m_mediaFailures == mfBefore);
            }
            if (!isRunning) break;   // 중지로 끊긴 것은 횟수에 넣지 않는다
            if (ok) { mediaRetry.remove(id); ++fixed; continue; }
            const int tries = e["tries"].toInt() + 1;
            if (tries >= 5) {
                m_backend->log(QString("미디어를 5번 다시 받아도 받지 못해 그만둡니다: https://x.com/i/status/%1%2")
                                   .arg(id, st == QLatin1String("GONE") ? QStringLiteral(" (글이 지워졌거나 볼 수 없습니다)") : QString()),
                               "warning", "twitter");
                mediaRetry.remove(id);
                continue;
            }
            e["tries"] = tries;
            e["last"] = Common::jstNow().toString(Qt::ISODate);
            mediaRetry[id] = e;
            QThread::msleep(1000);   // TweetDetail 을 몰아 묻지 않는다
        }
        saveMediaRetry();
        m_backend->log(QString("다시 받기: %1개 받음 · %2개는 다음 판에 다시").arg(fixed).arg(mediaRetry.size()),
                       fixed > 0 ? "success" : "info", "twitter");
    }

    // ══════════════════════════════════════════════════════════════
    // 스레드 수집 모드 — 단일 트윗 URL에서 시작해서 동일 작성자의 연결된
    //  대화 전체를 수집 (선조 체인 + focal + 본인 답글 / 스레드)
    // ══════════════════════════════════════════════════════════════
    if (type == "thread") {
        if (threadFocalId.isEmpty()) {
            m_backend->log("스레드 URL 또는 트윗 ID를 입력해주세요", "error", "twitter");
            if (!isSubCall) stopDaemon();
            return;
        }
        m_backend->log(QString("스레드 수집: focal=%1").arg(threadFocalId), "info", "twitter");
        m_lastThreadClean = false;
        bool threadClean = true;   // 오류 · 중지 없이 다 봤나(m_lastThreadClean)

        QSet<QString> visitedIds;
        QQueue<QString> toFetch;
        QJsonArray threadTweets;
        QString threadAuthorId;  // 스레드 작성자(= focal 작성자) 제한
        QString threadAuthorScreen;

        // ★ 첫 글(focal)을 먼저 읽어 작성자를 정한다(r56, 검토 B-1). getTweetDetail 은 focal 을 빼고 주므로 예전엔 첫 쪽을 다 돈 뒤에야
        //   작성자가 정해졌고, 그 첫 쪽(뿌리 글에 달린 남의 답글 전부)은 거르지 않고 담겼다 — 남의 답글이 thread 엑셀과
        //   media/replies · captures 에 쌓였다. 작성자를 모르면 거를 수 없으니 아무것도 담지 않고 멈춘다.
        {
            QJsonObject focal;
            QString st;
            for (int attempt = 0; attempt < 3 && isRunning && focal.isEmpty(); ++attempt) {
                focal = fetchTweetById(threadFocalId, &st);
                if (st == QLatin1String("RATE_LIMITED")) { handleRateLimit(accounts, currentAccountIdx, isRunning); continue; }
                if (st == QLatin1String("GONE")) break;   // 지워졌거나 볼 수 없는 글 — 다시 물어도 같다
                if (focal.isEmpty()) for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
            }
            const QJsonObject fcore = focal["core"].toObject()["user_results"].toObject()["result"].toObject();
            threadAuthorId = fcore["rest_id"].toString();
            threadAuthorScreen = screenNameOf(fcore);
            // 스레드 자동탐지가 '이 사람의 스레드' 로 부른 것인데 첫 글이 남의 것이면 담지 않는다(남의 글이 그 사람 폴더에 쌓이지 않게)
            const QString wantAuthor = config["_threadAuthor"].toString();
            const bool otherAuthor = !wantAuthor.isEmpty() && !threadAuthorScreen.isEmpty()
                                     && threadAuthorScreen.compare(wantAuthor, Qt::CaseInsensitive) != 0;
            if (threadAuthorId.isEmpty() || otherAuthor) {
                if (otherAuthor) {
                    m_backend->log(QString("스레드 첫 글이 @%1 의 글이 아닙니다(@%2) — 건너뜁니다").arg(wantAuthor, threadAuthorScreen), "info", "twitter");
                    m_lastThreadClean = true;   // 다시 엮을 것이 없다
                } else if (st == QLatin1String("GONE")) {
                    m_backend->log(QString("스레드 첫 글(%1)이 지워졌거나 볼 수 없습니다 — 건너뜁니다").arg(threadFocalId), "warning", "twitter");
                    m_lastThreadClean = true;   // 다시 물어도 같다
                } else if (isRunning) {
                    m_backend->log(QString("스레드 첫 글(%1)을 읽지 못했습니다 — 잠시 뒤 다시 해 주세요").arg(threadFocalId), "warning", "twitter");
                }
                if (!isSubCall) stopDaemon();
                return;
            }
            threadTweets.append(focal);
            // 선조 체인 — 스레드 작성자 본인에게 한 답글일 때만(아래 걸음과 같은 규칙)
            const QString parentId = xLegacy(focal)["in_reply_to_status_id_str"].toString();
            const QString parentAuthorId = xLegacy(focal)["in_reply_to_user_id_str"].toString();
            if (!parentId.isEmpty() && (parentAuthorId.isEmpty() || parentAuthorId == threadAuthorId))
                toFetch.enqueue(parentId);
        }
        toFetch.prepend(threadFocalId);   // focal 의 답글(스레드 아래쪽)부터

        int maxNodes = 500;  // 안전장치
        int rateRetries = 0;  // 한도 걸림 뒤 같은 쪽을 다시 묻는 횟수(무한 반복 방지)

        while (!toFetch.isEmpty() && isRunning && visitedIds.size() < maxNodes) {
            QString curId = toFetch.dequeue();
            if (visitedIds.contains(curId)) continue;
            visitedIds.insert(curId);

            QString cursor;
            int cursorLoops = 0;
            do {
                if (!isRunning) break;
                auto [batch, nextCursor] = getTweetDetail(curId, cursor);

                if (nextCursor == "RATE_LIMITED") {
                    handleRateLimit(accounts, currentAccountIdx, isRunning);
                    // ★ 예전엔 continue 가 do-while 조건(cursor 가 빔)에 걸려 첫 쪽이 한도에 걸리면 그 글을 통째로 건너뛰었다
                    if (++rateRetries > 5) { threadClean = false; break; }
                    if (cursor.isEmpty()) { visitedIds.remove(curId); toFetch.prepend(curId); break; }
                    continue;
                }
                if (nextCursor == "ERROR") {
                    m_backend->log("TweetDetail 에러 — 5초 후 재시도", "warning", "twitter");
                    threadClean = false;
                    for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                    break;
                }

                for (const auto &tv : batch) {
                    QJsonObject tw = tv.toObject();
                    QString id = tw["rest_id"].toString();
                    if (id.isEmpty()) id = xLegacy(tw)["id_str"].toString();
                    if (id.isEmpty()) continue;

                    QJsonObject core = tw["core"].toObject()["user_results"].toObject()["result"].toObject();
                    QString authorId = core["rest_id"].toString();

                    // 같은 작성자만 수집 (스레드 = 동일 작성자 chain) — 작성자는 위에서 focal 로 정했다
                    if (authorId != threadAuthorId) continue;

                    if (!visitedIds.contains(id)) {
                        threadTweets.append(tw);
                    }

                    // 부모 트윗(선조 체인)도 큐에 추가
                    QString parentId = xLegacy(tw)["in_reply_to_status_id_str"].toString();
                    QString parentAuthorId = xLegacy(tw)["in_reply_to_user_id_str"].toString();
                    if (!parentId.isEmpty() && !visitedIds.contains(parentId)) {
                        // 선조는 스레드 작성자 본인에게만 (다른 사람에게 한 답글은 제외)
                        if (parentAuthorId.isEmpty() || parentAuthorId == threadAuthorId) {
                            toFetch.enqueue(parentId);
                        }
                    }
                }

                cursor = nextCursor;
                if (++cursorLoops > 30) break;  // 안전장치
            } while (!cursor.isEmpty() && isRunning);
        }
        m_lastThreadClean = threadClean && isRunning;

        m_backend->log(QString("스레드: %1개 트윗 수집됨 (@%2)")
            .arg(threadTweets.size()).arg(threadAuthorScreen), "success", "twitter");

        if (!threadTweets.isEmpty()) {
            // created_at 오름차순으로 정렬 (스레드는 시간순이 자연스러움)
            QList<QJsonObject> sortedList;
            for (const auto &v : threadTweets) sortedList.append(v.toObject());
            std::sort(sortedList.begin(), sortedList.end(), [](const QJsonObject &a, const QJsonObject &b) {
                QString ca = xLegacy(a)["created_at"].toString();
                QString cb = xLegacy(b)["created_at"].toString();
                return Common::parseISODate(ca) < Common::parseISODate(cb);
            });
            QJsonArray sortedArr;
            for (const auto &o : sortedList) sortedArr.append(o);
            excelSuffix = "thread_"+ threadFocalId;
            logLabel = "스레드";
            processTweetBatch(sortedArr);
        }

        // Excel 저장
        if (m_saveExcel && collectedData.count() > 0) {
            saveExcelStreaming(userDir, target, collectedData, excelSuffix);
            m_backend->log(QString("Excel 저장: %1_%2.xlsx").arg(target, excelSuffix), "info", "twitter");
        }

        m_backend->log(QString("완료! 트윗: %1, 미디어: %2").arg(tweetCount).arg(mediaCount),
                       "success", "twitter");
        m_backend->updateStats(tweetCount, mediaCount, "완료", "twitter");
        if (!isSubCall) stopDaemon();
        return;
    }

    // ══════════════════════════════════════════════════════════════
    // 하이라이트 수집 — 유저가 프로필에 강조 표시한 트윗들
    // ══════════════════════════════════════════════════════════════
    if (type == "highlights") {
        excelSuffix = "highlights";
        logLabel = "하이라이트";
        m_backend->log("하이라이트 수집 시작...", "info", "twitter");

        QString hCursor;
        int hEmptyPages = 0;
        int listErr = 0;   // 이어진 API 오류(listErrorGiveUp)
        while (isRunning && !capReached()) {
            auto [batch, nextCursor] = getHighlights(userId, hCursor);
            if (nextCursor == "RATE_LIMITED") {
                handleRateLimit(accounts, currentAccountIdx, isRunning);
                continue;
            }
            if (nextCursor == "ERROR") {
                if (listErrorGiveUp(m_backend, listErr, QStringLiteral("하이라이트"), isRunning)) break;
                continue;
            }
            listErr = 0;
            if (batch.isEmpty()) {
                if (++hEmptyPages >= 2) break;
            } else {
                hEmptyPages = 0;
            }
            processTweetBatch(batch);
            if (nextCursor.isEmpty()) break;
            hCursor = nextCursor;
            QThread::sleep(1);
        }

        if (m_saveExcel && collectedData.count() > 0) {
            saveExcelStreaming(userDir, target, collectedData, excelSuffix);
            m_backend->log(QString("Excel 저장: %1_%2.xlsx").arg(target, excelSuffix), "info", "twitter");
        }
        m_backend->log(QString("완료! 하이라이트: %1, 미디어: %2").arg(tweetCount).arg(mediaCount),
                       "success", "twitter");
        m_backend->updateStats(tweetCount, mediaCount, "완료", "twitter");
        if (!isSubCall) stopDaemon();
        return;
    }

    // ══════════════════════════════════════════════════════════════
    // 좋아요/리트윗 한 유저 목록 — 특정 트윗 URL에 대해
    // ══════════════════════════════════════════════════════════════
    if (type == "favoriters"|| type == "retweeters") {
        // target 은 트윗 URL 또는 ID
        QString focalId;
        QRegularExpression rx(R"((?:x\.com|twitter\.com)/[A-Za-z0-9_]+/status/(\d+))");
        auto m = rx.match(config["target"].toString());
        if (m.hasMatch()) focalId = m.captured(1);
        else {
            QRegularExpression idRx("^\\d+$");
            if (idRx.match(config["target"].toString().trimmed()).hasMatch())
                focalId = config["target"].toString().trimmed();
        }
        if (focalId.isEmpty()) {
            m_backend->log("트윗 URL 또는 ID를 입력하세요", "error", "twitter");
            if (!isSubCall) stopDaemon();
            return;
        }

        QString action = (type == "favoriters") ? "좋아요한": "리트윗한";
        excelSuffix = type + "_"+ focalId;
        m_backend->log(QString("%1 유저 수집: focal=%2").arg(action, focalId), "info", "twitter");

        QJsonArray users;
        QString uCursor;
        while (isRunning && !(maxCount > 0 && users.size() >= maxCount)) {
            auto [batch, nextCursor] =
                (type == "favoriters") ? getFavoriters(focalId, uCursor)
                                        : getRetweeters(focalId, uCursor);
            if (nextCursor == "RATE_LIMITED") {
                handleRateLimit(accounts, currentAccountIdx, isRunning);
                continue;
            }
            if (nextCursor == "ERROR") {
                m_backend->log(action + "에러 — 5초 후 재시도", "warning", "twitter");
                for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                continue;
            }
            if (batch.isEmpty() && nextCursor.isEmpty()) break;
            for (const auto &u : batch) users.append(u);
            m_backend->log(QString("%1: %2명").arg(action).arg(users.size()), "info", "twitter");
            m_backend->updateStats(users.size(), 0, "수집 중...", "twitter");
            if (nextCursor.isEmpty()) break;
            uCursor = nextCursor;
            QThread::sleep(1);
        }
        while (maxCount > 0 && users.size() > maxCount) users.removeLast();   // 페이지째 받아 넘친 만큼

        // 유저 리스트를 Excel 로 저장 (간단 포맷)
        if (m_saveExcel && !users.isEmpty()) {
            ExcelWriter writer;
            QStringList headers = {"rest_id", "screen_name", "name", "followers",
                                   "following", "tweets", "verified", "description",
                                   "created_at", "location", "url", "profile_image"};
            writer.writeHeader(headers, QColor("#0070C0"));
            int row = 2;
            for (const auto &uv : users) {
                QJsonObject u = uv.toObject();
                QJsonObject l = xUserLegacy(u);
                QStringList r;
                r << u["rest_id"].toString()
                  << l["screen_name"].toString()
                  << l["name"].toString()
                  << QString::number(l["followers_count"].toInt())
                  << QString::number(l["friends_count"].toInt())
                  << QString::number(l["statuses_count"].toInt())
                  << (u["is_blue_verified"].toBool() ? "True": "False")
                  << l["description"].toString()
                  << Common::formatDateJapanese(l["created_at"].toString())
                  << l["location"].toString()
                  << l["url"].toString()
                  << l["profile_image_url_https"].toString();
                writer.writeRow(row++, r);
            }
            QString outPath = userDir + "/"+ target + "_"+ excelSuffix + ".xlsx";
            writer.save(outPath);
            m_backend->log(QString("Excel 저장: %1_%2.xlsx (%3명)")
                .arg(target, excelSuffix).arg(users.size()), "info", "twitter");
        }
        m_backend->log(QString("완료! %1: %2명").arg(action).arg(users.size()),
                       "success", "twitter");
        m_backend->updateStats(users.size(), 0, "완료", "twitter");
        if (!isSubCall) stopDaemon();
        return;
    }

    // ══════════════════════════════════════════════════════════════
    // 리스트 트윗 수집 — List ID 기반
    // ══════════════════════════════════════════════════════════════
    if (type == "list") {
        QString listId = config["target"].toString().trimmed();
        QRegularExpression rx(R"(/i/lists/(\d+))");
        auto mm = rx.match(listId);
        if (mm.hasMatch()) listId = mm.captured(1);
        if (listId.isEmpty()) {
            m_backend->log("리스트 ID 또는 URL을 입력하세요", "error", "twitter");
            if (!isSubCall) stopDaemon();
            return;
        }
        excelSuffix = "list_"+ listId;
        m_backend->log(QString("리스트 수집: id=%1").arg(listId), "info", "twitter");

        QString lCursor;
        int lEmpty = 0;
        int listErr = 0;   // 이어진 API 오류(listErrorGiveUp)
        while (isRunning && !capReached()) {
            auto [batch, nextCursor] = getListTweets(listId, lCursor);
            if (nextCursor == "RATE_LIMITED") { handleRateLimit(accounts, currentAccountIdx, isRunning); continue; }
            if (nextCursor == "ERROR") {
                if (listErrorGiveUp(m_backend, listErr, QStringLiteral("리스트"), isRunning)) break;
                continue;
            }
            listErr = 0;
            if (batch.isEmpty()) { if (++lEmpty >= 2) break; } else { lEmpty = 0; }
            processTweetBatch(batch);
            if (nextCursor.isEmpty()) break;
            lCursor = nextCursor;
            QThread::sleep(1);
        }
        if (m_saveExcel && collectedData.count() > 0)
            saveExcelStreaming(userDir, target, collectedData, excelSuffix);
        m_backend->log(QString("완료! 리스트 트윗: %1").arg(tweetCount), "success", "twitter");
        if (!isSubCall) stopDaemon();
        return;
    }

    // ══════════════════════════════════════════════════════════════
    // 커뮤니티 트윗 수집 — Community ID 기반
    // ══════════════════════════════════════════════════════════════
    if (type == "community") {
        QString commId = config["target"].toString().trimmed();
        QRegularExpression rx(R"(/i/communities/(\d+))");
        auto mm = rx.match(commId);
        if (mm.hasMatch()) commId = mm.captured(1);
        if (commId.isEmpty()) {
            m_backend->log("커뮤니티 ID 또는 URL을 입력하세요", "error", "twitter");
            if (!isSubCall) stopDaemon();
            return;
        }
        excelSuffix = "community_"+ commId;
        m_backend->log(QString("커뮤니티 수집: id=%1").arg(commId), "info", "twitter");

        QString cCursor;
        int cEmpty = 0;
        int cErr = 0;   // ★ 오류가 끝없이 이어지면(서명·해시 문제는 기다려도 안 풀린다) 5번째에 멈춘다
        while (isRunning && !capReached()) {
            auto [batch, nextCursor] = getCommunityTweets(commId, cCursor);
            if (nextCursor == "RATE_LIMITED") { handleRateLimit(accounts, currentAccountIdx, isRunning); continue; }
            if (nextCursor == "ERROR") {
                if (++cErr >= 5) { m_backend->log("API 오류가 5번 이어졌습니다 — 커뮤니티 수집을 멈춥니다", "error", "twitter"); break; }
                for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                continue;
            }
            cErr = 0;
            if (batch.isEmpty()) { if (++cEmpty >= 2) break; } else { cEmpty = 0; }
            processTweetBatch(batch);
            if (nextCursor.isEmpty()) break;
            cCursor = nextCursor;
            QThread::sleep(1);
        }
        if (m_saveExcel && collectedData.count() > 0)
            saveExcelStreaming(userDir, target, collectedData, excelSuffix);
        m_backend->log(QString("완료! 커뮤니티 트윗: %1").arg(tweetCount), "success", "twitter");
        if (!isSubCall) stopDaemon();
        return;
    }

    // ══════════════════════════════════════════════════════════════
    // Phase 1: 아니포 방식 — from:user 검색 + 무한 backtrack (메인)
    // 전략: 1) from:user 전체 검색 → 끝까지 페이지네이션
    //       2) 커서 끝나면 → from:user until:가장_오래된_날짜 → 다시 페이지네이션
    //       3) 반복 (backtrack 횟수 무제한, 연속 빈 결과 시 종료)
    // ══════════════════════════════════════════════════════════════
    bool searchFailed = false;  // Phase 1 검색 실패 플래그 (Phase 2에서 사용)
    // ★ 검색이 "ERROR" 를 돌려주면 5초 쉬고 끝없이 다시 했다. 서명(TID)·해시 문제는 기다려도 안 풀려서
    //   같은 요청만 수백 번 나갔다. 5번 이어지면 그 단계를 멈추고 다음 단계(Phase 2 등)로 넘긴다.
    int searchErrStreak = 0;
    bool searchErrored = false;
    auto searchErrorGiveUp = [&]() -> bool {
        if (++searchErrStreak < 5) {
            m_backend->log(QString("API 오류 — 5초 후 재시도 (%1/5)").arg(searchErrStreak), "warning", "twitter");
            for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
            return false;
        }
        m_backend->log("API 오류가 5번 이어졌습니다 — 검색 단계를 멈춥니다", "error", "twitter");
        searchErrStreak = 0;
        searchErrored = true;
        return true;
    };
    // ★ 정지된 계정의 검색 문(r56) — searchTweets 가 "SUSPENDED" 를 주면 지금 계정은 이미 이 판의 검색에서 빠졌다(m_searchBlocked).
    //   검색이 막히지 않은 다른 계정으로 바꿔 같은 질의를 다시 묻는다(쉬는 계정을 먼저 — 없으면 막히지 않은 아무 계정, 429 면 handleRateLimit 이
    //   풀림 시각까지 기다린다). 모두 막혔으면 한 번만 알리고 검색 단계를 오류로 끝낸다(searchErrored — 바닥 · 걸음 표시를 남기지 않고,
    //   진행 기록도 보지 못한 구간 너머로 옮기지 않는다). 타임라인 단계(Phase 2)는 그대로 돈다 — 정지 계정도 UserTweets 는 받는다(2026-11-27 실측).
    //   데몬을 다시 띄우거나 해시를 다시 받지 않는다 — 해시 · 서명 · 토큰 문제가 아니다.
    //   ★ 기간 판에는 타임라인 단계가 없다 — 모두 막혔으면 Phase 1 뒤의 '기간 타임라인' 이 그 기간을 타임라인으로 받아 본다(검토 XAPI-1).
    bool searchAllBlocked = false;   // 이 판의 검색이 모든 계정에서 막혀 끝났다
    // ★ 검색에서 모양 있는 쪽을 한 번이라도 받았나 · 받기 전에 정지 계정을 피해 바꾼 횟수(r56 검토 E2E-2) — 비공개 대상의
    //   '계정이 바뀐 걸음' 가드는 보는 눈이 섞인 걸음만 막아야 한다. 첫 쪽을 받기도 전에 정지 계정을 건너뛴 것은 한 계정으로만 본 걸음이다
    //   (첫 계정이 정지면 판마다 첫 검색에서 바꾸게 되어, 비공개 대상은 공백을 못 건너고 바닥 표시도 영영 적지 못했다).
    bool searchGotShaped = false;
    int suspendRotationsBeforeData = 0;
    bool periodRtEmpty = false;      // 기간 판의 리트윗 걸음이 하나도 못 찾았다 — 기간 타임라인이 타임라인으로 받아 본다(A9)
    auto searchSuspendedGiveUp = [&]() -> bool {
        const qint64 now = QDateTime::currentSecsSinceEpoch();
        int pick = -1;
        if (m_searchBlocked.contains(m_activeAccountIdx)) {
            for (int pass = 0; pass < 2 && pick < 0; ++pass)
                for (int i = 1; i < accounts.size() && pick < 0; ++i) {
                    const int c = (m_activeAccountIdx + i) % accounts.size();
                    if (!m_searchBlocked.contains(c) && (pass == 1 || m_accountFreeAt.value(c, 0) <= now)) pick = c;
                }
        }
        if (pick >= 0) {
            m_backend->log(QString("검색이 막히지 않은 계정 %1 로 바꿔 다시 묻습니다").arg(pick + 1), "info", "twitter");
            if (!searchGotShaped) ++suspendRotationsBeforeData;
            switchToAccount(accounts, pick);
            currentAccountIdx = pick;
            return false;
        }
        // 맨 위에서 지난번 가장 새 글까지 검색으로 내려가지 못했다 — 뒤의 타임라인 단계가 지난번 가장 새 글을 만나도 가장 새 쪽 표시를
        //   올리지 않는다(타임라인에는 답글이 없어, 그 사이의 답글을 못 본 채 표시만 넘어간다 — 보지 못한 구간 너머로 진행 기록을 옮기지 않는다)
        if (!reachedPrevNewest) newestLocked = true;
        searchAllBlocked = true;
        if (!m_searchAllBlockedLogged) {
            m_searchAllBlockedLogged = true;
            // ★ 판마다 받을 수 있는 것이 다르다 — 예전엔 기간 판에도 '타임라인으로 받는다' 고 했지만 기간 판은 타임라인을 돌지 않아 0개였다(검토 XAPI-1)
            const bool tlType = type == QLatin1String("tweets") || type == QLatin1String("tweets_api") || type == QLatin1String("all");
            QString what;
            if (!tlType)
                what = QStringLiteral("'미디어만' · '답글' 판은 검색으로만 받아 이번엔 받지 못합니다 — '트윗' 판으로 받으십시오. ");
            else if (mode == QLatin1String("period"))
                what = QStringLiteral("검색 단계는 건너뛰고, 그 기간을 타임라인으로 받아 봅니다(타임라인은 최근 약 3200개까지만 닿고 답글은 없습니다). ");
            else
                what = QStringLiteral("검색 단계는 건너뛰고, 타임라인으로 받을 수 있는 것(최근 약 3200개)만 받습니다. ");
            m_backend->log(QStringLiteral("X 가 계정을 정지 상태로 보아 검색을 막았습니다 — ") + what
                               + QStringLiteral("X 앱에서 계정 상태를 확인하십시오"),
                           "error", "twitter");
        }
        searchErrStreak = 0;
        searchErrored = true;
        return true;
    };
    // Phase 1: tweets, media, all 일 때만 실행 (tweets_api, replies는 건너뜀)
    // ── 포스트 좁히기 (narrow) — from:user [키워드] [since/until] SearchTimeline ──
    //   복잡한 아니포 백트래킹 없이, 좁힌 쿼리로 커서 페이지네이션만 한다.
    if (isRunning && type == "narrow") {
        const QString kw = config["narrowQuery"].toString().trimmed();
        // ★ 기간은 일본 시간의 하루인데 X 검색의 since · until 은 UTC 날짜이고 until 은 그 날을 뺀다 — 앞뒤로 하루씩 넓혀 묻고 글마다 거른다
        //   (예전엔 끝 날의 글을 검색이 돌려주지 않았고, 시작 날의 일본 시간 아침 9시 전 글도 빠졌다)
        QString q = QString("from:%1").arg(target);
        if (!kw.isEmpty()) q += " " + kw;
        if (sinceDate.isValid()) q += QString(" since:%1").arg(sinceDate.addDays(-1).toString("yyyy-MM-dd"));
        if (untilDate.isValid()) q += QString(" until:%1").arg(untilDate.addDays(1).toString("yyyy-MM-dd"));
        m_backend->log(QString("🔎 포스트 좁히기 검색: %1").arg(q), "info", "twitter");
        const int before = collectedIds.size();
        QString cur;
        int pages = 0;
        const int NARROW_MAX_PAGES = 400;   // 과도한 페이지네이션 상한
        while (isRunning && !capReached() && pages < NARROW_MAX_PAGES) {
            auto [tweets, nextCursor] = searchTweets(q, cur);
            if (nextCursor == "RATE_LIMITED") { handleRateLimit(accounts, currentAccountIdx, isRunning); continue; }
            if (nextCursor == "SUSPENDED") { if (searchSuspendedGiveUp()) break; continue; }
            if (nextCursor == "ERROR") { if (searchErrorGiveUp()) break; continue; }
            searchErrStreak = 0;
            if (tweets.isEmpty()) break;
            processTweetBatch(tweets);
            pages++;
            if (pages % 5 == 0) {
                if (excelCheckpointDue()) saveExcelStreaming(userDir, target, collectedData, excelSuffix);   // 저장은 할 때가 됐을 때만
                m_backend->log(QString("🔎 좁히기 진행: %1페이지 · %2개")
                                   .arg(pages).arg(collectedIds.size() - before), "info", "twitter");
            }
            if (nextCursor.isEmpty() || nextCursor == cur) break;
            cur = nextCursor;
            QThread::sleep(1);
        }
        saveExcelStreaming(userDir, target, collectedData, excelSuffix);
        m_backend->log(QString("🔎 포스트 좁히기 완료: %1개 (%2)")
                           .arg(collectedIds.size() - before).arg(q), "success", "twitter");
        // 미디어는 processTweetBatch 내부 downloadTweetMedia 로 트윗별 자동 다운로드됨.
    }

    // ── 과거로 되짚는 검색의 바닥 · 공백 건너기 (r55) ───────────────────────────────────────────────
    //   바닥 = 트위터 시작일(2006-03-21)과 이 계정을 만든 날 중 늦은 것 — 그 전에는 글이 있을 수 없다.
    //   ★ 예전엔 검색이 비면 7일씩 물리다 '빈 결과 10번(70일)' 이면 멈춰, 70일 넘게 쉰 계정은 그 공백 너머의 옛 글을 받지 못했다
    //     ('전체(2006년부터)' 라고 해 놓고). 이제 바닥(계정을 만든 날)을 알고, 이 계정이 검색에 보인다는 증거가 있으면 — 이 걸음이
    //     글을 하나라도 찾았거나, 받아 둔 글이 있는 이어받기 · 받아 둔 가장 새 글보다 위인 구간 — 빈 결과가 이어져도 바닥까지 간다.
    //     증거가 없을 때(검색이 이 계정을 못 보는 때 — 타임라인 단계가 받는다)와 바닥을 모를 때만 예전처럼 10번에서 멈춘다.
    //   ★ 비용 — 검색은 긴 공백을 한 번에 못 건너 7일씩 물린다(몇 년 공백이면 수백 번). 그래서 바닥까지 끊김 없이 7일씩 걸은
    //     걸음은 표시(.hanishiki_floor_tw_<엑셀>_<목록>.json — 바닥 · 가장 오래된 날 · 그 걸음이 덮은 맨 위 날 · 그 아래 받아 둔 글 수)를
    //     남기고, 1년(대상마다 330~400일) 동안 다음 걸음은
    //     (1) 가장 오래된 날 아래로는 내려가지 않고(과거에 새 글이 생길 수는 없다 — 받아 둔 글이 그 아래 있어도: 지워졌거나 검색에 안 보이는 글),
    //     (2) 그 걸음이 덮은 맨 위 날 아래의 공백은 받아 둔 옛 글(엑셀의 ID — 트윗 ID 에 시각이 들어 있다)의 날로 바로 건너뛴다.
    //         맨 위 날보다 새 구간(그 뒤에 올라온 글)은 7일씩 걷는다 — 거기서 건너뛰면 그 뒤에 올라온 글을 놓친다(검토).
    //     표시는 줄을 다 저장한 뒤에만, 옛 엑셀 · 저장 실패 · 계정이 바뀐 걸음 · 모양 없는 응답 · 건너뛴 걸음이면 남기지 않는다.
    //     엑셀이 줄었으면(맨 위 날 아래 받아 둔 글이 표시보다 적으면) 표시를 믿지 않는다.
    const QDate kTwitterEpoch(2006, 3, 21);
    const QDate accountCreated = Common::parseISODate(userLegacy["created_at"].toString()).date();
    const bool floorKnown = accountCreated.isValid() && accountCreated >= kTwitterEpoch && accountCreated <= QDate::currentDate();
    const QDate walkFloor = floorKnown ? accountCreated : kTwitterEpoch;
    static const qint64 kSfEpoch = 1288834974657LL;   // 트윗 ID 의 시각 기준(2010-11-04)
    static const qulonglong kSfMin = 30000000000ULL;  // 이보다 작은 ID 는 스노플레이크 이전(시각이 없다)
    std::vector<qulonglong> archIds;   // 이 판을 시작할 때 받아 둔 트윗 ID(오름차순)
    archIds.reserve(static_cast<size_t>(collectedIds.size()));
    for (const QString &id : std::as_const(collectedIds)) {
        bool ok = false;
        const qulonglong v = id.toULongLong(&ok);
        if (ok) archIds.push_back(v);
    }
    std::sort(archIds.begin(), archIds.end());
    // 그 날 0시(UTC) 보다 이른 트윗 ID 의 상한(스노플레이크 시대 전이면 0)
    auto idBoundOf = [](const QDate &d) -> qulonglong {
        const qint64 ms = QDateTime(d, QTime(0, 0), QTimeZone::UTC).toMSecsSinceEpoch();
        return ms > kSfEpoch ? (static_cast<qulonglong>(ms - kSfEpoch) << 22) : 0ULL;
    };
    // 스노플레이크 이전(2010-11-04 전) 글은 ID 에 시각이 없다 — 표시가 그 걸음에서 본 그 시대의 가장 새 글의 날을 알면 그것을 쓴다(아래)
    QDate preEpochNewest;
    // before 보다 이른 날에 받아 둔 글 가운데 가장 새것의 날(없으면 무효).
    auto archivedBefore = [&](const QDate &before) -> QDate {
        if (!before.isValid()) return QDate();
        const qulonglong bound = idBoundOf(before);
        if (bound > 0) {
            auto it = std::lower_bound(archIds.begin(), archIds.end(), bound);
            if (it == archIds.begin()) return QDate();
            const qulonglong id = *(--it);
            if (id >= kSfMin) return QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(id >> 22) + kSfEpoch, QTimeZone::UTC).date();
        }
        // 아래에 남은 것은 스노플레이크 이전 글뿐(또는 before 가 그 시대) — 시각을 모른다
        if (archIds.empty() || archIds.front() >= kSfMin) return QDate();
        if (preEpochNewest.isValid() && preEpochNewest < before) return preEpochNewest;
        return bound > 0 ? QDate(2010, 11, 3) : QDate();   // 그보다 이르다는 것만 안다
    };
    // 그 날까지(그 날 포함)의 트윗 ID 수 — ids 는 아무 순서(받아 둔 것 + 이번 판)
    auto countUpTo = [&](const QDate &upto, const auto &ids) -> qint64 {
        const qulonglong bound = idBoundOf(upto.addDays(1));
        qint64 n = 0;
        for (const auto &x : ids) {
            bool ok = true;
            qulonglong v;
            if constexpr (std::is_same_v<std::decay_t<decltype(x)>, QString>) v = x.toULongLong(&ok); else v = x;
            if (ok && (v < kSfMin || bound == 0 || v < bound)) ++n;
        }
        return n;
    };
    auto floorMarkPath = [&](const QString &list) {
        return userDir + QStringLiteral("/.hanishiki_floor_tw_") + excelSuffix + QLatin1Char('_') + list + QStringLiteral(".json");
    };
    auto floorMarkDays = [](const QString &path) {   // 330~400일 — 같은 날 처음 받은 대상들이 한날에 다시 바닥까지 걷지 않게
        quint32 h = 2166136261u;
        for (const char ch : path.toUtf8()) { h ^= static_cast<quint8>(ch); h *= 16777619u; }
        return 330 + static_cast<int>(h % 71u);
    };
    struct FloorMark { QDate oldest, top, pre; bool valid = false; };
    auto readFloorMark = [&](const QString &list) -> FloorMark {
        FloorMark m;
        if (!floorKnown || legacyExcel) return m;
        const QString path = floorMarkPath(list);
        QFile f(path);
        if (!f.open(QIODevice::ReadOnly)) return m;
        const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
        if (o.value("v").toInt() != 2 || o.value("floor").toString() != walkFloor.toString(Qt::ISODate)) return m;
        if (QDateTime::currentMSecsSinceEpoch() - o.value("at").toVariant().toLongLong() > qint64(floorMarkDays(path)) * 24 * 3600 * 1000) return m;
        const QDate oldest = QDate::fromString(o.value("oldest").toString(), Qt::ISODate);
        const QDate top = QDate::fromString(o.value("top").toString(), Qt::ISODate);
        if (!oldest.isValid() || !top.isValid() || oldest > top) return m;
        if (countUpTo(top, archIds) < o.value("n").toVariant().toLongLong()) return m;   // 엑셀이 줄었거나 바뀌었다 — 믿지 않는다
        m.oldest = oldest; m.top = top; m.valid = true;
        m.pre = QDate::fromString(o.value("pre").toString(), Qt::ISODate);   // 그 걸음이 본 2010-11-04 전 글의 가장 새 날(없으면 무효)
        return m;
    };
    auto putFloorMark = [&](const QString &list, const QDate &oldest, const QDate &top, const QDate &pre) {
        if (!floorKnown || legacyExcel || !oldest.isValid() || !top.isValid() || oldest > top) return;
        if (collectedData.count() > 0) saveExcelStreaming(userDir, target, collectedData, excelSuffix);   // 줄을 다 저장한 뒤에만(putWalk 와 같다)
        if (m_excelSaveFailed) return;
        QJsonObject o{{"floor", walkFloor.toString(Qt::ISODate)}, {"oldest", oldest.toString(Qt::ISODate)},
                      {"top", top.toString(Qt::ISODate)}, {"n", countUpTo(top, collectedIds)},
                      {"at", QDateTime::currentMSecsSinceEpoch()}, {"v", 2}};
        if (pre.isValid()) o["pre"] = pre.toString(Qt::ISODate);
        QSaveFile f(floorMarkPath(list));
        if (f.open(QIODevice::WriteOnly)) { f.write(QJsonDocument(o).toJson(QJsonDocument::Compact)); f.commit(); }
    };
    // 받아 둔 글 가운데 가장 새것의 날 — 그보다 위(그 뒤)의 구간이 비어도 이 계정이 검색에 보인다는 증거로 친다(이 걸음이 아무것도 못 찾았어도)
    const QDate newestArch = archivedBefore(QDate::currentDate().addDays(2));

    // ── since: 로 막힌 아래로 걷기(r56) ───────────────────────────────────────────────────────────────
    //   기간 판(제 글 · 리트윗 · 답글)과 새 글만 · 양방향 1단계가 쓴다. 예전엔 셋 다 질의 하나를 커서로만 넘겼다 — 커서가 마르면(X 의 깊이 ·
    //   2쪽부터 ReplaceEntry 에 오던 커서를 못 읽던 것) 그 구간을 '다 봤다' 로 쳐 '기간 지정' 이 그 기간의 마지막 며칠만 받았고, 새 글만 판은
    //   못 받은 새 글 위로 가장 새 쪽 표시를 올렸다(tw-depth F3 · F4).
    //   이제 1단계 걸음과 같은 규칙으로 걷는다 — since:S 는 늘 단 채, 커서가 마르면 그 판의 가장 옛 날 + 1 을 until: 로(그 날을 다시 포함),
    //   빈 결과면 until: 을 7일씩 물리고, until 이 S 에 닿으면 끝. 한 날이 질의 하나에 다 들지 않으면 max_id: 로 그 날을 잇는다(아래 dayOverflow).
    //   stopAtId 가 있으면 그 ID 이하의 글을 만난 데서 멈춘다(지난번 가장 새 글 — 그 위는 끊김 없이 다 봤다).
    //   clean = 끊김 없이 끝났다(S 아래 · 멈출 ID 에 닿았다 · 제 모양의 빈 결과로 S 까지) — 오류 · 중지 · 최대 수 · 구멍 · 모양 없는 응답이 없었다.
    //   ★ 빈 결과가 10번 이어지면 멈춘다(1단계 걸음의 MAX_EMPTY 와 같다 — 검토 XAPI-2). 예전엔 7일씩 끝없이 물려, 검색에 안 보이는 계정이나
    //     리트윗이 검색에 안 나오는 계정의 1년 기간이 빈 질의 약 52번(쪽마다 3초 — 15분에 50번인 검색 한도도 넘는다)이었다.
    //     이 걸음이 글을 찾았거나(trustSeen 이면 앞선 판의 검색이 이 계정의 글을 본 적이 있어도) 계정을 만든 날을 알면 since 까지 간다.
    //     리트윗 · 답글 걸음은 from: 의 증거를 믿지 않는다(trustSeen=false) — 계정이 보여도 리트윗 · 답글이 없는 기간은 흔하다.
    //     상한으로 멈춘 걸음은 clean 이 아니다(새 글 쪽 걸음이 가장 새 쪽 표시를 올리지 않는다).
    struct BoundedWalk { int got = 0; int added = 0; bool clean = false; bool reachedStop = false; bool holed = false; };
    // 앞선 판의 검색이 이 계정의 글을 본 적이 있다(1단계의 searchSeenBefore 와 같은 증거 — 바닥 표시 · 검색 걸음 표시 · 진행 기록의 searchSeen)
    const bool searchSeenEver = prevSearchSeen || listTop.value(QStringLiteral("search") + mScope) > 0 || readFloorMark(QStringLiteral("search")).valid;
    auto boundedWalk = [&](const QString &base, const QDate &sinceQ, const QDate &untilQ, qulonglong stopAtId,
                           const QString &label, bool trustSeen) -> BoundedWalk {
        BoundedWalk r;
        if (!sinceQ.isValid()) return r;
        const int MAX_PAGES = 400;     // 한 질의의 쪽 상한(커서가 끝없이 이어지는 때)
        QDate until = untilQ;          // 무효 = until 없이 맨 위부터
        qulonglong maxId = 0;          // 0 = max_id 없음(한 날이 넘칠 때만 단다)
        QDate maxDay;                  // max_id 로 잇는 그 날(UTC)
        // 앞 걸음 뒤에 정한 until — 같은 날에 머무는지 본다. ★ 첫 걸음의 until(없으면 내일 UTC)로 시작한다 — 비워 두면 첫 질의가 until 전날의
        //   글만 돌려줄 때 하루 넘침을 못 알아보고 같은 질의를 쪽마다 한 번 더 물었다(검토 XAPI-3)
        QDate lastUntil = untilQ.isValid() ? untilQ : QDateTime::currentDateTimeUtc().date().addDays(1);
        const int MAX_EMPTY_WALK = 10;
        int emptyStreak = 0;           // 제 모양의 빈 결과가 이어진 수(max_id 로 잇는 그 날의 나머지는 세지 않는다)
        bool afterEmptyMaxId = false;  // 앞 질의가 max_id 로 그 날을 이었는데 비었다 — 이번 질의(until:그 날)가 무엇을 주면 max_id 가 듣지 않은 것이다
        bool unshapedSeen = false;
        int iteration = 0;
        while (isRunning && !stopNow()) {
            ++iteration;
            QString q = base + QStringLiteral(" since:") + sinceQ.toString("yyyy-MM-dd");
            if (until.isValid()) q += QStringLiteral(" until:") + until.toString("yyyy-MM-dd");
            if (maxId) q += QStringLiteral(" max_id:") + QString::number(maxId);
            m_backend->log(QString("[%1 %2] %3").arg(label).arg(iteration).arg(q), "info", "twitter");
            QString cur;
            int pages = 0, iterBatch = 0, iterNew = 0;
            QDate iterOldest;
            qulonglong iterMinId = 0;
            bool past = false, gaveUp = false;
            while (isRunning && !stopNow() && pages < MAX_PAGES) {
                auto [tweets, nextCursor] = searchTweets(q, cur);
                if (nextCursor == "RATE_LIMITED") { handleRateLimit(accounts, currentAccountIdx, isRunning); continue; }
                if (nextCursor == "SUSPENDED") { if (searchSuspendedGiveUp()) { gaveUp = true; break; } continue; }
                if (nextCursor == "ERROR") { if (searchErrorGiveUp()) { gaveUp = true; break; } continue; }
                // 모양 없는 빈 쪽(errors 만 든 200)은 '끝' 이 아니라 오류다 — 같은 질의를 다시 묻는다(1단계 걸음과 같다)
                if (tweets.isEmpty() && !m_lastPageShaped) { unshapedSeen = true; if (searchErrorGiveUp()) { gaveUp = true; break; } continue; }
                searchErrStreak = 0;
                searchGotShaped = true;
                if (tweets.isEmpty()) break;
                ++pages;
                iterBatch += tweets.size();
                for (const auto &tv : tweets) {
                    const QJsonObject to = tv.toObject();
                    const QDateTime td = Common::parseISODate(xLegacy(to)["created_at"].toString());
                    if (td.isValid() && (!iterOldest.isValid() || td.date() < iterOldest)) iterOldest = td.date();
                    if (td.isValid() && (!oldestTweetDate.isValid() || td < oldestTweetDate)) oldestTweetDate = td;
                    QString tid = xLegacy(to)["id_str"].toString();
                    if (tid.isEmpty()) tid = to["rest_id"].toString();
                    const qulonglong idv = tid.toULongLong();
                    if (idv > 0 && (iterMinId == 0 || idv < iterMinId)) iterMinId = idv;
                    if (stopAtId && idv > 0 && idv <= stopAtId) r.reachedStop = true;
                }
                const int before = tweetCount;
                past = processTweetBatch(tweets);
                iterNew += tweetCount - before;
                if (past) break;
                if (nextCursor.isEmpty() || nextCursor == cur) break;   // 같은 커서가 다시 오면 이 질의는 끝(끝없이 같은 쪽을 묻지 않게)
                cur = nextCursor;
                for (int w = qMax(2, (int)m_delay); w > 0 && isRunning && !stopNow(); --w) QThread::sleep(1);
            }
            r.got += iterBatch;
            r.added += iterNew;
            if (iterBatch > 0) emptyStreak = 0;
            // ★ max_id 질의(since:S until:U max_id:X)가 비었으면 [S, U) 에 X 보다 옛 글이 없어야 한다 — 그 뒤의 until:그 날 질의가 글을 주면
            //   X 가 max_id 를 글자로 읽은 것이다(늘 0개). 예전엔 이 빈 결과를 '그 날의 나머지가 비었다' 로만 쳐, 그 날의 나머지가 빠져도
            //   구멍으로 적지 않고 바닥 표시까지 남길 수 있었다(검토 A8)
            if (afterEmptyMaxId && !maxId) {
                afterEmptyMaxId = false;
                if (iterBatch > 0 && !r.holed) {
                    r.holed = true;
                    m_backend->log(QString("%1 하루에 글이 많아 다 받지 못했을 수 있습니다(max_id 가 듣지 않음) — 그 아래로 넘어갑니다")
                                       .arg(maxDay.toString("yyyy-MM-dd")), "warning", "twitter");
                }
            }
            if (iterNew > 0) {
                m_backend->log(QString("+%1개 (누적 %2, 미디어 %3)").arg(iterNew).arg(tweetCount).arg(mediaCount), "success", "twitter");
                saveExcelStreaming(userDir, target, collectedData, excelSuffix);
            }
            if (gaveUp || !isRunning) return r;
            const auto finish = [&]() { r.clean = !r.holed && !unshapedSeen; return r; };
            if (past && !stopNow()) return finish();          // 기간의 시작보다 옛 글에 닿았다(새 것부터 온다)
            // 지난번 가장 새 글에 닿았다 — 받아 둔 것에서 일찍 멈춘 것(knownStop)과 겹쳐도 끝까지 본 것이다(최대 수로 멈춘 것만 아니면)
            if (stopAtId && r.reachedStop && !capReached()) return finish();
            if (stopNow()) return r;

            QDate nextUntil;
            qulonglong nextMaxId = 0;
            if (maxId) {
                if (iterBatch == 0) {
                    nextUntil = maxDay;   // 그 날의 나머지가 비었다 — until:D 로 그 아래를 잇는다(구멍이 아니다 — 다음 질의가 확인한다, 위 afterEmptyMaxId)
                    afterEmptyMaxId = true;
                } else if (iterMinId == 0 || iterMinId > maxId) {
                    // max_id 가 듣지 않았다(새것이 없다) — 예전의 강제 후퇴(until:D). 그 날의 나머지가 빠졌을 수 있다(clean 아님)
                    nextUntil = maxDay;
                    r.holed = true;
                    m_backend->log(QString("%1 하루에 글이 많아 다 받지 못했을 수 있습니다(max_id 가 듣지 않음) — 그 아래로 넘어갑니다")
                                       .arg(maxDay.toString("yyyy-MM-dd")), "warning", "twitter");
                } else if (iterOldest.isValid() && iterOldest >= maxDay) {
                    nextUntil = until;                 // 아직 그 날 — 더 옛 ID 로 잇는다
                    nextMaxId = iterMinId - 1;
                } else {
                    nextUntil = iterOldest.isValid() ? iterOldest.addDays(1) : maxDay;   // 그 날을 넘었다 — 보통 걸음으로
                }
            } else if (iterBatch == 0 || !iterOldest.isValid()) {
                if (iterBatch == 0 && ++emptyStreak >= MAX_EMPTY_WALK) {
                    // 빈 결과 상한(위 설명) — 증거가 없으면 멈춘다. 계정을 만든 날을 모르는 바닥(2006)까지의 걸음은 1단계처럼 증거가 있어도 멈춘다
                    const bool seen = (r.got > 0 || (trustSeen && (searchSeenEver || searchSeenRun))) && (floorKnown || sinceQ > walkFloor);
                    if (!seen) {
                        m_backend->log(QString("[%1] 빈 결과가 %2번 이어져 멈춥니다 — 검색에 보이지 않는 것 같습니다(%3 전은 확인하지 못했습니다)")
                                           .arg(label).arg(emptyStreak)
                                           .arg(until.isValid() ? until.toString("yyyy-MM-dd") : QStringLiteral("오늘")),
                                       "warning", "twitter");
                        return r;   // clean 아님
                    }
                }
                const QDate curU = until.isValid() ? until : QDateTime::currentDateTimeUtc().date().addDays(1);
                nextUntil = curU.addDays(-7);          // 빈 결과 — 7일 물린다(긴 공백 건너기)
            } else {
                nextUntil = iterOldest.addDays(1);     // 그 날을 다시 포함해 잇는다(D+1 — until 은 그 날을 뺀다)
                if (lastUntil.isValid() && nextUntil >= lastUntil) {
                    // ★ 한 날이 질의 하나에 다 들지 않았다(tw-depth F2) — 예전엔 until:D 로 강제 후퇴해 그 날의 나머지를 영영 건너뛰었다.
                    //   이제 그 날을 max_id:<본 것 가운데 가장 옛 ID - 1> 로 잇는다(트윗 ID 는 시각 순). 2010-11-04 전 글은 ID 에 시각이 없어
                    //   예전처럼 물리고 구멍으로 친다.
                    if (iterMinId >= kSfMin) {
                        nextUntil = until.isValid() ? until : iterOldest.addDays(1);
                        nextMaxId = iterMinId - 1;
                        maxDay = iterOldest;
                        m_backend->log(QString("%1 하루에 글이 많습니다 — 그 날을 max_id 로 이어 받습니다").arg(iterOldest.toString("yyyy-MM-dd")),
                                       "info", "twitter");
                    } else {
                        nextUntil = lastUntil.addDays(-1);
                        r.holed = true;
                    }
                }
            }
            maxId = nextMaxId;
            if (!maxId) lastUntil = nextUntil;
            if (!nextUntil.isValid() || nextUntil <= sinceQ) return finish();   // until 이 since 에 닿았다 — 그 기간을 끝까지 봤다
            until = nextUntil;
            for (int w = 3; w > 0 && isRunning; --w) QThread::sleep(1);
        }
        return r;
    };

    if (isRunning && (type == "tweets"|| type == "media"|| type == "all" || (type == "tweets_api" && mode == "period")))
    {
        // Resume support
        int phase1Start = tweetCount;
        int backtrackCount = 0;
        int consecutiveEmpty = 0;
        const int MAX_CONSECUTIVE_EMPTY = 20;

        // ── 아니포 방식 핵심: cursor 끝나면 until:oldest_date 로 반복 ──
        // 전략 (anipo_smart.py 라인 3067~3087 참조):
        //   1) from:user → 커서 끝까지 페이지네이션
        //   2) 커서 끝나면 해당 iteration 의 가장 오래된 트윗 날짜로 until:YYYY-MM-DD
        //   3) 다시 페이지네이션
        //   4) API 가 완전히 빈 결과를 반환하면 7일씩 강제 후퇴 (긴 공백 기간 돌파)
        //   5) 연속 빈 결과 MAX_EMPTY 회 또는 2006년 도달 시 종료

        const QDate scanStopDate = walkFloor;  // 바닥 — 트위터 시작일 또는 계정을 만든 날(위 설명)
        const int MAX_EMPTY = 10;  // 연속 빈 iteration 종료 조건 — 걸음이 아무것도 못 찾았거나 바닥을 모를 때만(위 설명)

        if (mode == "period") {
            // period 모드: since~until 사이만 검색
            // ★ BUG FIX: since:/until: 앞 공백 누락 → "usersince:..."로 검색돼서 결과 0개
            // ★ 기간은 일본 시간의 하루인데 X 의 since · until 은 UTC 날짜이고 until 은 그 날을 뺀다 — 앞뒤로 하루씩 넓혀 묻고 글마다 거른다.
            //   시작 날을 안 주면 바닥(계정을 만든 날)까지. 예전엔 질의 하나를 커서로만 넘겨 그 기간의 마지막 며칠만 받았다 —
            //   이제 1단계 걸음처럼 until: 을 물려 가며 그 기간의 시작까지 내려간다(boundedWalk 위 설명, r56).
            const QDate pSince = sinceDate.isValid() ? sinceDate.date().addDays(-1) : walkFloor.addDays(-1);
            const QDate pUntil = untilDate.isValid() ? untilDate.date().addDays(1) : QDate();
            m_backend->log(QString("Phase 1: 기간 검색 — from:%1 since:%2%3 를 until: 로 물려 가며 끝까지 봅니다")
                               .arg(target, pSince.toString("yyyy-MM-dd"),
                                    pUntil.isValid() ? QStringLiteral(" until:") + pUntil.toString("yyyy-MM-dd") : QString()),
                           "info", "twitter");
            const BoundedWalk pw = boundedWalk(QString("from:%1").arg(target), pSince, pUntil, 0, QStringLiteral("기간"), true);
            if (!searchErrored) backtrackCount++;   // 검색이 돌았다('검색 API 실패' 로 적지 않게)
            if (pw.holed)
                m_backend->log("기간 안의 어떤 날은 글이 너무 많아 다 받지 못했을 수 있습니다 — 다시 돌리면 받아 둔 것은 건너뜁니다", "warning", "twitter");
            // ★ 리트윗(r56 · COV-2) — from: 검색은 리트윗을 돌려주지 않고, 기간 판은 타임라인 단계(Phase 2)를 돌지 않아 기간 판의 '트윗' 에는
            //   리트윗이 한 번도 없었다. 리트윗만 돌려주는 질의로 같은 걸음을 한 번 더 걷는다(줄 · 미디어는 Phase 2 의 리트윗과 같이 — processTweetBatch).
            if (isRunning && !capReached() && !searchErrored && (type == "tweets" || type == "all" || type == "tweets_api")) {
                const BoundedWalk rw = boundedWalk(QString("from:%1 filter:nativeretweets include:nativeretweets").arg(target),
                                                   pSince, pUntil, 0, QStringLiteral("기간 리트윗"), false);
                m_backend->log(QString("기간 리트윗: %1개 반환 · 새로 %2개").arg(rw.got).arg(rw.added), "info", "twitter");
                // ★ 리트윗이 검색에 안 나오는 계정이 많다 — 예전엔 statusesCount > 3200 이면 '옛 리트윗은 빠졌을 수 있다' 고만 했지만 기간 판은
                //   타임라인을 돌지 않아 최근 리트윗도 빠졌다. 아래 기간 타임라인이 타임라인으로 받아 보고, 어디까지 닿았는지 실제로 알린다(검토 A9)
                if (rw.got == 0 && isRunning && !searchErrored && !capReached()) {
                    periodRtEmpty = true;
                    m_backend->log("기간 안의 리트윗을 검색으로 찾지 못했습니다 — 타임라인이 닿는 데까지(최근 약 3200개) 타임라인으로 받아 봅니다", "info", "twitter");
                }
            }
        } else {
            // 아니포 방식: from:user → cursor 끝나면 until:oldest 로 반복
            bool p1GotAny = false;        // 검색이 하나라도 돌려줬나 — 전부 빈 걸음은 '다 봤다' 가 아니다
            bool p1TopEmpty = false;      // 맨 위(until: 없는) 걸음이 비었나 — 그러면 7일을 건너뛴다. 받아 둔 것이 있는 계정이면 다 본 것이 아니다
            if (tweetCount > 0) {
                m_backend->log(QString("기존 %1개 수집분 존재 → 중복 자동 스킵").arg(tweetCount), "info", "twitter");
            }
            m_backend->log(QString("Phase 1: 아니포 방식 검색 시작 (from:%1)").arg(target), "info", "twitter");
            if (resumeMode != "future")
                m_backend->log(floorKnown
                                   ? QString("  ↳ 바닥: 계정을 만든 날 %1 — 이 계정이 검색에 보이면(글을 찾거나 받아 둔 글이 있으면) 그날까지 내려갑니다(긴 공백도 건너서)")
                                         .arg(walkFloor.toString("yyyy-MM-dd"))
                                   : QStringLiteral("  ↳ 계정을 만든 날을 몰라 2006-03-21 까지 · 빈 결과가 10번 이어지면 멈춥니다"),
                               "info", "twitter");
            const FloorMark p1Mark = readFloorMark(QStringLiteral("search"));   // 앞선 판이 바닥까지 끊김 없이 걸었으면(위 설명)
            bool p1ReachedFloor = false;      // 바닥에 닿아 끝났다(표시를 적는다)
            bool p1FloorByMark = false;       // 표시의 가장 오래된 날 아래라 멈췄다(표시는 그대로 — 1년 뒤엔 다시 바닥까지)
            bool p1Jumped = false;            // 표시를 믿고 공백을 건너뛰었다 — 이 걸음은 사이를 밟지 않았으니 표시를 새로 적지 않는다
            bool p1Unshaped = false;          // 모양 없는 응답(errors 만 든 200)을 빈 결과로 넘긴 적이 있다 — 표시를 적지 않는다
            bool p1FirstEmpty = false;        // 첫 걸음(맨 위)이 비었다 — 그 7일은 확인하지 못했다
            bool p1Holed = false;             // 한 날이 질의 하나에 다 들지 않아 그 날의 나머지를 건너뛰었을 수 있다(max_id 가 듣지 않음 · 2010-11 전) — 표시를 적지 않는다(r56)
            const int p1AccSwitchStart = m_accountSwitchCount;   // 비공개 계정은 보는 계정마다 보이는 것이 달라, 계정이 바뀐 걸음은 표시를 적지 않는다
            const bool targetProtected = userLegacy["protected"].toBool();   // 공개 계정은 어느 계정으로 봐도 같다 — 계정이 돌아도 적는다
            QDate p1PreNewest;                // 이 걸음이 본 2010-11-04 전 글의 가장 새 날(표시에 적는다 — 그 시대의 ID 에는 시각이 없다)
            if (p1Mark.valid && p1Mark.pre.isValid()) preEpochNewest = p1Mark.pre;

            // ★ 새 글 쪽 걸음(새 글만 · 양방향 1단계 — r56 · tw-depth F4) — 지난번 가장 새 글의 날(since:)까지 until: 로 물려 가며 내려간다.
            //   예전엔 since: 질의 하나를 커서로만 넘기고 '끝까지 봤다' 로 쳐, 지난번 뒤로 올라온 글이 질의 하나의 깊이(약 40개)를 넘으면
            //   그 아래를 영영 놓치고 가장 새 쪽 표시만 올렸다(다음 since: 가 그 위에서 시작한다 — 답글 · 미디어 판은 타임라인으로도 못 메운다).
            //   이제 지난번 가장 새 글(이하)을 만났거나 제 모양의 빈 결과로 since: 까지 끊김 없이 내려갔을 때만 '다 봤다' 로 친다.
            const QString prevNewestStr = !resumeNewestId.isEmpty() ? resumeNewestId : prevNewestBoundary;
            QDate fwSince = QDate::fromString(resumeNewestDate, "yyyy-MM-dd");
            if (!fwSince.isValid() && prevNewestStr.toULongLong() >= kSfMin)   // 날이 없으면 ID 의 시각으로(스노플레이크)
                fwSince = QDateTime::fromMSecsSinceEpoch(static_cast<qint64>(prevNewestStr.toULongLong() >> 22) + kSfEpoch, QTimeZone::UTC).date();
            auto forwardWalk = [&](const QString &tag) -> BoundedWalk {
                const BoundedWalk fw = boundedWalk(QString("from:%1").arg(target), fwSince, QDate(), prevNewestStr.toULongLong(), tag, true);
                if (!prevNewestBoundary.isEmpty() || !prevNewestStr.isEmpty()) {
                    reachedPrevNewest = fw.clean && isRunning && !capReached();
                    newestLocked = !reachedPrevNewest;   // 뒤의 타임라인 단계가 지난번 가장 새 글을 만나도 올리지 않는다(위 설명)
                }
                if (!fw.clean && isRunning && !capReached())
                    m_backend->log(QString("%1: 지난번 가장 새 글까지 끊김 없이 내려가지 못했습니다 — 가장 새 쪽 표시는 그대로 둡니다(다음 판이 다시 봅니다)").arg(tag),
                                   "warning", "twitter");
                return fw;
            };
            const bool fwPossible = fwSince.isValid() && (!resumeNewestDate.isEmpty() || !resumeNewestId.isEmpty());

            // ★★ "both" 모드 — Phase A (future): 새 트윗 걸음 후 Phase B (past) 진행
            //   resumeNewestDate / Id가 있을 때만 의미 있음 (없으면 첫 다운 → 일반 backtrack)
            if (resumeMode == "both" && fwPossible) {
                m_backend->log(QString("[양방향 1단계] %1 이후 새 트윗 검색")
                                   .arg(resumeNewestDate.isEmpty() ? resumeNewestId : resumeNewestDate),
                               "info", "twitter");
                const BoundedWalk fa = forwardWalk(QStringLiteral("양방향 1단계"));
                if (fa.got > 0) searchSeenRun = true;
                m_backend->log(QString("[양방향 1단계] %1개 새 트윗 → 2단계 backtrack 시작")
                                   .arg(fa.added), "success", "twitter");
                if (fa.added > 0) {
                    saveExcelStreaming(userDir, target, collectedData, excelSuffix);
                }
            }

            // ★ 초기 쿼리 (Phase B / past) — resumeMode 기준 분기:
            //   "past"   : until:<resumeOldestDate>  (마지막 다운한 것보다 더 과거만)
            //   "future" : since:<resumeNewestDate>  (마지막 다운 후 새로 올라온 것만, backtrack 생략)
            //   "both"   : until:<resumeOldestDate>  (Phase A 끝나고 backtrack 시작)
            QString currentQuery = QString("from:%1").arg(target);
            bool futureWalked = false;   // 새 글만 판을 위의 새 글 쪽 걸음으로 끝냈다(아래 되짚기 루프는 돌지 않는다)
            if (resumeMode == "future" && fwPossible) {
                // future 모드: 과거로 되짚지 않는다 — 지난번 가장 새 글까지만 내려간다(위 forwardWalk)
                m_backend->log(QString("[현재 이어서] %1 이후 새 트윗만 검색")
                                   .arg(resumeNewestDate.isEmpty() ? resumeNewestId : resumeNewestDate), "info", "twitter");
                const BoundedWalk fu = forwardWalk(QStringLiteral("현재 이어서"));
                futureWalked = true;
                if (!searchErrored) backtrackCount++;   // 검색이 돌았다('검색 API 실패' 로 적지 않게)
                if (fu.got > 0) { p1GotAny = true; searchSeenRun = true; }
                if (fu.added > 0) {
                    m_backend->log(QString("[현재 이어서] 새 트윗 %1개 수집 (과거 backtrack 생략)").arg(fu.added), "success", "twitter");
                } else if (isRunning && !searchErrored) {
                    // 새 트윗 0개 — '아무것도 안 함'처럼 보이지 않게 명확히 안내.
                    m_backend->log("[현재 이어서] 새로 올라온 트윗이 없습니다 — 이미 최신 상태입니다.", "success", "twitter");
                    m_backend->log("  ↳ 빠진 과거 트윗을 더 받으려면 '이어서 수집' 방향을 '과거에서 이어서' 또는 '양방향'으로 선택하세요.",
                                   "info", "twitter");
                }
            } else if (resumeMode == "future") {
                // future 모드: backtracking 안 함 → 새 트윗만 한 번 훑음
                if (!resumeNewestDate.isEmpty()) {
                    currentQuery = QString("from:%1 since:%2").arg(target, resumeNewestDate);
                    m_backend->log(QString("[현재 이어서] %1 이후 새 트윗만 검색")
                                       .arg(resumeNewestDate), "info", "twitter");
                } else if (!resumeNewestId.isEmpty()) {
                    currentQuery = QString("from:%1 since_id:%2").arg(target, resumeNewestId);
                    m_backend->log(QString("[현재 이어서] ID %1 이후 새 트윗만 검색")
                                       .arg(resumeNewestId), "info", "twitter");
                }
            } else if (!resumeOldestDate.isEmpty()) {
                // past / both: oldest 이전부터 backtrack
                QDate rd = QDate::fromString(resumeOldestDate, "yyyy-MM-dd");
                if (rd.isValid()) {
                    currentQuery = QString("from:%1 until:%2").arg(target, resumeOldestDate);
                    forwardScan = false;   // 여기부터는 과거 쪽 이어가기 — 위쪽 이어짐 판정에 쓰지 않는다
                    QString modeLabel = (resumeMode == "past") ? "[과거 이어서]" : "[양방향 이어서]";
                    m_backend->log(QString("%1 %2 이전부터").arg(modeLabel, resumeOldestDate),
                                   "info", "twitter");
                }
            }

            QDate lastOldestDate;   // 이전 iteration 의 oldest (무한 루프 감지)
            qulonglong p1MaxId = 0;  // 0 이 아니면 지금 질의는 그 날(p1MaxDay)을 max_id:<이 값> 으로 잇는 중(r56 — 아래 하루 넘침)
            QDate p1MaxDay;
            bool p1AfterEmptyMaxId = false;   // 앞 질의가 max_id 로 그 날을 이었는데 비었다 — 이번 until:그 날 질의가 글을 주면 max_id 가 듣지 않았다(boundedWalk 와 같다)
            int consecutiveEmpty = 0;
            int iteration = 0;
            // 맨 위에서 2006년까지 되짚는 걸음만 표시를 다룬다(과거 이어가기 · since: 는 목록의 한 조각이라 건드리지 않는다)
            const bool p1Walk = forwardScan && resumeMode != "future";
            beginStreak(p1Walk && takeWalk("search"), "search");
            static const QRegularExpression kUntilRe(QStringLiteral(R"(until:(\d{4}-\d{2}-\d{2}))"));
            auto untilOf = [](const QString &q) -> QDate {   // 질의의 until: 날(없으면 오늘)
                const auto qm = kUntilRe.match(q);
                const QDate d = qm.hasMatch() ? QDate::fromString(qm.captured(1), "yyyy-MM-dd") : QDate();
                return d.isValid() ? d : QDate::currentDate();
            };
            // 이 걸음이 덮기 시작한 맨 위 날(표시에 적는다) — 맨 위부터면 오늘(UTC) 하루 전(색인이 늦게 붙는 글), 이어받기면 그 until: 의 전날
            const QDate p1Top = kUntilRe.match(currentQuery).hasMatch() ? untilOf(currentQuery).addDays(-1)
                                                                       : QDateTime::currentDateTimeUtc().date().addDays(-1);
            // 이 계정이 검색에 보인다는 증거가 있으면 빈 결과가 이어져도 바닥까지 간다(위 설명).
            //   ★ 증거는 검색에서 온 것만 — 이 걸음이 찾았거나, 앞선 판의 바닥 표시 · 검색 걸음 표시가 있다. 엑셀만으로는 안 된다:
            //     검색에 안 보이는 계정(민감 글 · 검색 제한 · 팔로하지 않은 비공개)도 타임라인 단계가 엑셀을 채워, 판마다 수백 번 헛걸음했다(검토).
            //     진행 기록의 searchSeen(앞선 판의 검색이 이 계정의 글을 돌려준 적이 있다 — 오류로 끝나 표시를 못 남긴 걸음도, r56 · F8)도 친다.
            const bool searchSeenBefore = p1Mark.valid || listTop.value(QStringLiteral("search") + mScope) > 0 || prevSearchSeen;
            const bool p1ResumeEvidence = !forwardScan && resumeMode != "future" && !archIds.empty() && searchSeenBefore;
            const QDate p1WalkStart = untilOf(currentQuery);
            auto throughEmpty = [&]() {
                if (!floorKnown) return false;
                if (targetProtected && m_accountSwitchCount - suspendRotationsBeforeData != p1AccSwitchStart) return false;   // 다른 계정은 이 비공개 계정을 못 볼 수 있다
                if (p1GotAny || p1ResumeEvidence) return true;
                // 받아 둔 가장 새 글 근처까지는 내려가 본다(맨 위가 비는 쉬는 계정) — 검색이 본 적이 없으면 13주까지만
                const QDate cur = untilOf(currentQuery);
                return newestArch.isValid() && cur > newestArch.addDays(-7) && (searchSeenBefore || cur > p1WalkStart.addDays(-91));
            };

            // 위 걸음(양방향 1단계)이 오류로 끝났으면 되짚지 않는다 — 같은 오류를 되풀이할 뿐이다(그 판은 표시 · 진행 기록을 남기지 않는다)
            while (!futureWalked && !searchErrored && isRunning && !stopNow() && (consecutiveEmpty < MAX_EMPTY || throughEmpty())) {
                iteration++;
                backtrackCount++;
                m_backend->log(QString("[%1] %2").arg(iteration).arg(currentQuery), "info", "twitter");

                QString searchCursor;
                int iterNewCount = 0;
                int iterBatchCount = 0;  // API 가 반환한 총 트윗 수 (중복 포함)
                QDate iterOldestDate;
                qulonglong iterMinId = 0;    // 이 iteration 이 본 가장 옛(작은) 트윗 ID — 하루 넘침을 max_id 로 잇는다
                const bool wasMaxId = p1MaxId != 0;
                int iterPages = 0;
                bool iterShapedEnd = false;  // 이 질의가 제 모양의 빈 쪽으로 끝났다(커서가 마른 것과 다르다 — 아래 '현재 이어서' 가 쓴다)
                const int P1_MAX_PAGES = 400;   // ★ 한 질의의 쪽 상한 · 같은 커서면 끝(r56) — 커서를 ReplaceEntry 에서도 읽게 되어 X 가 같은 커서를 되풀이하면 끝없이 돌 수 있다

                while (isRunning && !stopNow() && iterPages < P1_MAX_PAGES) {
                    auto [tweets, nextCursor] = searchTweets(currentQuery, searchCursor);
                    if (nextCursor == "RATE_LIMITED") {
                        handleRateLimit(accounts, currentAccountIdx, isRunning);
                        continue;
                    }
                    if (nextCursor == "SUSPENDED") {   // 정지 계정 — 다른 계정으로 다시, 모두 막혔으면 오류로 끝(searchSuspendedGiveUp 설명)
                        if (searchSuspendedGiveUp()) break;
                        continue;
                    }
                    if (nextCursor == "ERROR") {
                        if (searchErrorGiveUp()) break;
                        continue;
                    }
                    // ★ 모양 없는 빈 쪽(errors 만 든 200 · 빈 몸)은 '공백' 이 아니라 오류다 — 같은 질의를 다시 묻는다(r55 검토).
                    //   공백으로 치면 빈 결과가 이어져도 바닥까지 가는 걸음이 X 가 오류를 내는 동안 7일씩 넘어가며 그 구간을 '본 것' 으로
                    //   진행 기록에 적었다. 5번 이어지면 검색 단계를 오류로 끝낸다(표시 · 진행 기록을 남기지 않는다 — 타임라인 단계가 받는다)
                    if (tweets.isEmpty() && !m_lastPageShaped) {
                        if (searchErrorGiveUp()) break;
                        continue;
                    }
                    searchErrStreak = 0;
                    searchGotShaped = true;
                    if (tweets.isEmpty()) { iterShapedEnd = true; break; }

                    iterBatchCount += tweets.size();
                    ++iterPages;

                    // 가장 오래된 날짜 추적 (이 iteration 한정 + 전체)
                    for (const auto &tv : tweets) {
                        QString ca = xLegacy(tv.toObject())["created_at"].toString();
                        QDateTime td = Common::parseISODate(ca);
                        if (td.isValid()) {
                            QDate d = td.date();
                            if (!iterOldestDate.isValid() || d < iterOldestDate) iterOldestDate = d;
                            if (!oldestTweetDate.isValid() || td < oldestTweetDate) oldestTweetDate = td;
                            if (d < QDate(2010, 11, 4) && (!p1PreNewest.isValid() || d > p1PreNewest)) p1PreNewest = d;
                        }
                        const QJsonObject to = tv.toObject();
                        QString tid = xLegacy(to)["id_str"].toString();
                        if (tid.isEmpty()) tid = to["rest_id"].toString();
                        if (const qulonglong idv = tid.toULongLong(); idv > 0 && (iterMinId == 0 || idv < iterMinId)) iterMinId = idv;
                    }

                    int prev = tweetCount;
                    processTweetBatch(tweets);
                    iterNewCount += (tweetCount - prev);

                    if (nextCursor.isEmpty() || nextCursor == searchCursor) break;   // 같은 커서가 다시 오면 이 질의는 끝
                    searchCursor = nextCursor;
                    for (int w = qMax(2, (int)m_delay); w > 0 && isRunning && !stopNow(); --w) QThread::sleep(1);
                }
                if (searchErrored) break;   // 오류로 멈춘 판 — 날짜를 물리며 같은 오류를 되풀이하지 않는다

                if (iteration == 1 && iterBatchCount == 0 && !knownAtStart.isEmpty()) p1TopEmpty = true;
                if (iteration == 1 && iterBatchCount == 0) p1FirstEmpty = true;   // 표시의 맨 위 날을 낮춘다(처음 받는 계정도)
                // ★ max_id 질의가 비었으면 그보다 옛 글이 없어야 한다 — 바로 뒤 until:그 날 질의가 글을 주면 X 가 max_id 를 글자로 읽은 것이다.
                //   그 날의 나머지가 빠졌을 수 있어 표시를 적지 않는다(검토 A8 — 예전엔 '그 날의 나머지가 비었다' 로만 쳤다)
                if (p1AfterEmptyMaxId && !wasMaxId) {
                    p1AfterEmptyMaxId = false;
                    if (iterBatchCount > 0 && !p1Holed) {
                        p1Holed = true;
                        m_backend->log(QString("%1 하루에 글이 많아 다 받지 못했을 수 있습니다(max_id 가 듣지 않음) — 그 아래로 넘어갑니다")
                                           .arg(p1MaxDay.toString("yyyy-MM-dd")), "warning", "twitter");
                    }
                }
                if (iterBatchCount > 0) {
                    consecutiveEmpty = 0;
                    p1GotAny = true;
                    searchSeenRun = true;   // 진행 기록의 searchSeen(위 searchSeenBefore 설명)
                    if (iterNewCount > 0) {
                        m_backend->log(QString("+%1개 새 트윗 (누적 %2, 미디어 %3)")
                                           .arg(iterNewCount).arg(tweetCount).arg(mediaCount), "success", "twitter");
                        saveExcelStreaming(userDir, target, collectedData, excelSuffix);
                    } else {
                        m_backend->log(QString("%1개 반환 (모두 기존 수집분)").arg(iterBatchCount), "info", "twitter");
                    }
                } else if (wasMaxId) {
                    // max_id 로 잇던 날의 나머지가 비었다 — 빈 결과로 세지 않는다(그 날 안의 이어짐이다 · 아래에서 until:그 날 로 잇는다)
                    if (!m_lastPageShaped) p1Unshaped = true;
                } else {
                    consecutiveEmpty++;
                    if (!m_lastPageShaped) p1Unshaped = true;
                    if (!throughEmpty())
                        m_backend->log(QString("빈 결과 %1/%2").arg(consecutiveEmpty).arg(MAX_EMPTY), "info", "twitter");
                    else if (consecutiveEmpty == 1 || consecutiveEmpty % 10 == 0)
                        m_backend->log(QString("빈 결과 — 공백을 지나는 중(%1번째) · 계정을 만든 날 %2 까지 봅니다")
                                           .arg(consecutiveEmpty).arg(walkFloor.toString("yyyy-MM-dd")), "info", "twitter");
                }

                // ★ "현재 이어서" 모드는 한 iteration만 — backtrack 안 함
                if (resumeMode == "future") {
                    // 오류로 멈춘 판은 바로 위에서 이미 빠져나갔다.
                    // ★ 커서가 마른 것만으로는 since: 구간을 끝까지 본 것이 아니다(r56 · tw-depth F4 — X 의 한 질의 깊이에서 끊긴다).
                    //   제 모양의 빈 쪽으로 끝났을 때만 '다 봤다' 로 친다. 아니면 가장 새 쪽 표시를 그대로 둔다(다음 판이 다시 본다).
                    //   (날 · 스노플레이크 ID 가 있는 보통의 새 글만 판은 위의 forwardWalk 가 맡는다 — 여기는 2010-11 전 ID 만 남은 드문 때)
                    if (isRunning && !capReached() && iterShapedEnd && (!resumeNewestDate.isEmpty() || !resumeNewestId.isEmpty()))
                        reachedPrevNewest = true;
                    if (iterNewCount > 0) {
                        m_backend->log(QString("[현재 이어서] 새 트윗 %1개 수집 (과거 backtrack 생략)").arg(iterNewCount),
                                       "success", "twitter");
                    } else {
                        // 새 트윗 0개 — '아무것도 안 함'처럼 보이지 않게 명확히 안내.
                        m_backend->log("[현재 이어서] 새로 올라온 트윗이 없습니다 — 이미 최신 상태입니다.", "success", "twitter");
                        m_backend->log("  ↳ 빠진 과거 트윗을 더 받으려면 '이어서 수집' 방향을 '과거에서 이어서' 또는 '양방향'으로 선택하세요.",
                                       "info", "twitter");
                    }
                    break;
                }

                // ★ 최대 수집 수에 닿으면 여기서 멈춘다. 이 판의 가장 오래된 날짜(iterOldestDate)는
                //   아직 처리하지 못한 트윗까지 보고 정한 값이라 progress 에 적으면 안 된다.
                //   일찍 멈춤도 같다 — 받아 둔 구간에서 멈췄으니 그 날짜를 '여기까지 봤다' 로 적지 않는다.
                if (stopNow()) {
                    if (knownStop)
                        m_backend->log(QString("이미 받은 트윗이 %1개 이어져 여기서 멈춥니다 — 새 트윗 %2개")
                                           .arg(knownStreak).arg(tweetCount - runStartCount), "info", "twitter");
                    break;
                }

                // 다음 iteration 쿼리 결정
                QDate nextUntil;
                qulonglong nextMaxId = 0;
                if (wasMaxId) {
                    // ── 하루 넘침을 max_id 로 잇던 걸음(아래 설명) ──
                    if (iterBatchCount == 0) {
                        nextUntil = p1MaxDay;   // 그 날의 나머지가 비었다 — until:그 날 로 그 아래를 잇는다(구멍이 아니다 — 다음 질의가 확인한다, 위 p1AfterEmptyMaxId)
                        p1AfterEmptyMaxId = true;
                    } else if (iterMinId == 0 || iterMinId > p1MaxId) {
                        // max_id 가 듣지 않았다(새것이 없다) — 예전의 강제 후퇴(until:그 날). 그 날의 나머지가 빠졌을 수 있어 표시를 적지 않는다
                        nextUntil = p1MaxDay;
                        p1Holed = true;
                        m_backend->log(QString("%1 하루에 글이 많아 다 받지 못했을 수 있습니다(max_id 가 듣지 않음) — 그 아래로 넘어갑니다")
                                           .arg(p1MaxDay.toString("yyyy-MM-dd")), "warning", "twitter");
                    } else if (iterOldestDate.isValid() && iterOldestDate >= p1MaxDay) {
                        nextUntil = untilOf(currentQuery);   // 아직 그 날 — 더 옛 ID 로 잇는다
                        nextMaxId = iterMinId - 1;
                    } else {
                        nextUntil = iterOldestDate.isValid() ? iterOldestDate.addDays(1) : p1MaxDay;   // 그 날을 넘었다 — 보통 걸음으로(그 날을 다시 포함해)
                    }
                    if (!nextMaxId) lastOldestDate = nextUntil;
                } else if (iterOldestDate.isValid()) {
                    // ★ 그 날을 다시 포함해 잇는다(D+1 — until 은 그 날을 뺀다). 검색의 커서는 날 한가운데서 끊기는데,
                    //   예전엔 until:D 로 이어(anipo 방식) D 의 나머지 — 끊긴 시각보다 이른 글 — 를 영영 건너뛰었다(검토).
                    //   겹치는 글은 ID 로 걸러지고, 맨 위부터 다 본 걸음의 표시(top)도 이제 말 그대로다.
                    nextUntil = iterOldestDate.addDays(1);
                    // 이전 iteration 과 같거나 더 최신이면 그 날 안에서 더 나아가지 못했다 — 한 날이 질의 하나에 다 들지 않는다(tw-depth F2).
                    //   ★ 예전엔 1일 강제 후퇴(until:D)로 그 날의 나머지를 영영 건너뛰었다(질의의 깊이가 약 40개면 100개 날에서 60개를 잃었다).
                    //   이제 그 날을 max_id:<본 것 가운데 가장 옛 ID - 1> 로 잇는다(트윗 ID 는 시각 순 — until 은 그대로 둔다).
                    //   2010-11-04 전 글은 ID 에 시각이 없어 예전처럼 물리고, 구멍이 남았을 수 있어 표시를 적지 않는다.
                    if (lastOldestDate.isValid() && nextUntil >= lastOldestDate) {
                        if (iterMinId >= kSfMin) {
                            nextMaxId = iterMinId - 1;
                            p1MaxDay = iterOldestDate;
                            m_backend->log(QString("%1 하루에 글이 많습니다 — 그 날을 max_id 로 이어 받습니다").arg(iterOldestDate.toString("yyyy-MM-dd")),
                                           "info", "twitter");
                        } else {
                            nextUntil = lastOldestDate.addDays(-1);
                            p1Holed = true;
                        }
                    }
                    if (!nextMaxId) lastOldestDate = nextUntil;
                } else {
                    // 완전 빈 결과: 현재 until 에서 7일 강제 후퇴 (긴 공백 기간 돌파)
                    QRegularExpression re(R"(until:(\d{4}-\d{2}-\d{2}))");
                    auto qm = re.match(currentQuery);
                    QDate curUntil = qm.hasMatch() ? QDate::fromString(qm.captured(1), "yyyy-MM-dd") : QDate::currentDate();
                    if (!curUntil.isValid()) curUntil = QDate::currentDate();
                    nextUntil = curUntil.addDays(-7);
                    // ★ (2) 앞선 판이 바닥까지 걸은 구간(표시의 맨 위 날 아래)이면 받아 둔 옛 글의 날로 바로 건너뛴다 — 그 걸음이 사이를 다 밟았다.
                    //   맨 위 날보다 새 구간 · 표시가 없을 때는 7일씩 걷는다(그 뒤에 올라온 글 · '기간' 판처럼 사이를 걷지 않고 들어온 글)
                    if (p1Mark.valid && nextUntil <= p1Mark.top) {
                        const QDate arch = archivedBefore(nextUntil);
                        if (arch.isValid() && arch >= p1Mark.oldest && arch.addDays(1) < nextUntil) {
                            nextUntil = arch.addDays(1);
                            p1Jumped = true;
                            m_backend->log(QString("공백 — 받아 둔 옛 글(%1)로 건너뜁니다").arg(arch.toString("yyyy-MM-dd")), "info", "twitter");
                        }
                    }
                    // ★ (1) 표시의 가장 오래된 날 아래로는 내려가지 않는다(받아 둔 글이 그 아래 있어도 — 지워졌거나 검색에 안 보이는 글)
                    if (p1Mark.valid && nextUntil < p1Mark.oldest) {
                        m_backend->log(QString("앞선 판이 계정을 만든 날(%1)까지 확인한 구간입니다 — %2 아래는 다시 보지 않습니다")
                                           .arg(walkFloor.toString("yyyy-MM-dd"), p1Mark.oldest.toString("yyyy-MM-dd")),
                                       "success", "twitter");
                        p1FloorByMark = true;
                        break;
                    }
                }

                if (!nextUntil.isValid() || nextUntil < scanStopDate) {
                    m_backend->log(floorKnown
                                       ? QString("계정을 만든 날(%1)까지 스캔 완료!").arg(walkFloor.toString("yyyy-MM-dd"))
                                       : QStringLiteral("2006년 Twitter 시작일까지 스캔 완료!"),
                                   "success", "twitter");
                    p1ReachedFloor = true;
                    break;
                }

                currentQuery = QString("from:%1 until:%2").arg(target, nextUntil.toString("yyyy-MM-dd"));
                if (nextMaxId) currentQuery += QString(" max_id:%1").arg(nextMaxId);
                p1MaxId = nextMaxId;
                saveProgress(nextUntil.toString("yyyy-MM-dd"));
                for (int w = 3; w > 0 && isRunning; --w) QThread::sleep(1);
            }

            if (!p1ReachedFloor && !p1FloorByMark && consecutiveEmpty >= MAX_EMPTY && isRunning && !stopNow()) {
                m_backend->log(QString("%1회 연속 빈 결과 → Phase 1 종료(%2)").arg(MAX_EMPTY)
                                   .arg(!floorKnown ? QStringLiteral("계정을 만든 날을 몰라서")
                                                    : QStringLiteral("검색이 이 계정의 글을 돌려주지 않아서 — 타임라인 단계가 받습니다")),
                               "info", "twitter");
            }
            // 끊김 없이 끝났다(2006년까지 · 빈 결과로 끝 · 일찍 멈춤 — 중지 · 최대 수 · 검색 오류 없이)
            //   전부 빈 걸음(검색이 이 계정을 못 찾는 때 · 검색 장애)은 다 본 것으로 치지 않는다 — 그 사이 타임라인 단계가 넣은
            //   제 글에서 다음 판이 멈춰, 검색으로만 받는 답글을 놓쳤다(검토)
            const bool p1Clean = isRunning && !capReached() && !searchErrored;
            //   맨 위에서 2006년까지 되짚지 않은 걸음(새 글만 · 과거 이어서 · 양방향)은 표시를 건드리지 않는다 — 끝에 읽은 그대로
            //   되돌린다. 그 걸음이 넣은 줄은 top 위면 다음 판이 세지 않고, top 아래면 이미 받아 둔 곳이다
            if (p1Walk && p1Clean && p1GotAny && !p1TopEmpty && !p1Holed) putWalk("search");
            // 바닥까지 끊김 없이(건너뛰지 않고 · 제 모양의 응답으로 · 비공개 계정이면 같은 계정으로) 내려갔으면 표시 — 다음 판부터 1년 동안(위 설명).
            //   맨 위 걸음이 비었으면(p1TopEmpty) 그 7일만 확인하지 못했다 — 맨 위 날을 7일 낮춰 적는다
            if (p1ReachedFloor && p1Clean && p1GotAny && !p1Jumped && !p1Unshaped && !p1Holed
                && (!targetProtected || m_accountSwitchCount - suspendRotationsBeforeData == p1AccSwitchStart) && oldestTweetDate.isValid())
                putFloorMark(QStringLiteral("search"), oldestTweetDate.date(), p1FirstEmpty ? p1Top.addDays(-7) : p1Top, p1PreNewest);
        }

        int phase1New = tweetCount - phase1Start;
        searchFailed = (phase1New == 0 && backtrackCount == 0)  // 검색 API 자체가 실패
                       || (phase1New == 0 && searchErrored);   // 오류가 이어져 멈춤 — Phase 2(UserTweets)로 받는다
        m_backend->log(QString("Phase 1 완료: %1개 트윗, %2개 미디어%3")
                           .arg(tweetCount).arg(mediaCount)
                           .arg(searchFailed ? "(검색 API 실패)": ""),
                       searchFailed ? "warning": "success", "twitter");
    }

    // ── 기간 타임라인(r56 · 검토 XAPI-1) ──────────────────────────────────────────────────────────────
    //   ★ 기간 판은 검색으로만 받고 타임라인 단계(Phase 2)를 돌지 않는다 — 검색이 모든 계정에서 막혔거나(정지 계정) 오류가 이어져 멈추면
    //     그 기간을 하나도 못 받은 채 끝났다(정지된 계정은 검색이 막힌다 — 2026-11-27 실측). 리트윗 걸음이 하나도 못 찾았을 때도
    //     (리트윗이 검색에 안 나오는 계정) 그 기간의 리트윗이 통째로 빠졌다. 그 두 때만 타임라인(UserTweets — 정지 계정도
    //     받는다)을 맨 위부터 내려가며 그 기간의 글만 넣는다 — processTweetBatch 가 untilDate 위는 건너뛰고 sinceDate 아래에서 멈춘다
    //     (첫 쪽 맨 앞의 고정 글은 그 셈에서 뺀다). 타임라인은 최근 약 3200개까지만 닿고 답글이 없다 — 어디까지 닿았는지 알린다.
    //     표시 · 진행 기록은 건드리지 않는다(기간 판은 원래 적지 않는다).
    if (isRunning && !capReached() && mode == "period" && (searchErrored || periodRtEmpty) && !userId.isEmpty()
        && (type == "tweets" || type == "tweets_api" || type == "all")) {
        m_backend->log(searchErrored ? QStringLiteral("기간 타임라인: 검색이 막혀 그 기간을 타임라인으로 받아 봅니다(최근 약 3200개까지 · 답글 없음)")
                                     : QStringLiteral("기간 타임라인: 그 기간의 리트윗을 타임라인으로 받아 봅니다(최근 약 3200개까지)"),
                       "info", "twitter");
        QString cursor;
        int tlPages = 0, tlEmpty = 0, tlErr = 0, tlNew = 0;
        QDateTime tlOldest;   // 타임라인이 닿은 가장 옛 글(고정 글 빼고)
        while (isRunning && !stopNow()) {
            auto [tweets, nextCursor] = getUserTweets(userId, cursor);
            if (nextCursor == "RATE_LIMITED") { handleRateLimit(accounts, currentAccountIdx, isRunning); continue; }
            if (nextCursor == "ERROR") {
                if (++tlErr >= 5) {
                    m_backend->log("API 오류가 5번 이어졌습니다 — 기간 타임라인을 멈춥니다", "error", "twitter");
                    break;
                }
                m_backend->log(QString("API 오류 — 5초 후 재시도 (%1/5)").arg(tlErr), "warning", "twitter");
                for (int w = 5; w > 0 && isRunning; --w) QThread::sleep(1);
                continue;
            }
            tlErr = 0;
            if (tweets.isEmpty()) {   // 목록 끝은 트윗 없이 커서만 든 쪽이다(Phase 2 와 같다)
                if (++tlEmpty >= 5) break;
                QThread::sleep(3);
                continue;
            }
            tlEmpty = 0;
            ++tlPages;
            QDateTime pageNewest;   // 이 쪽에서 가장 새 글(고정 글 빼고)
            for (const auto &tv : tweets) {
                const QJsonObject to = tv.toObject();
                if (to.value(QStringLiteral("pinned")).toBool()) continue;
                const QDateTime td = Common::parseISODate(xLegacy(to)["created_at"].toString());
                if (td.isValid() && (!tlOldest.isValid() || td < tlOldest)) tlOldest = td;
                if (td.isValid() && (!pageNewest.isValid() || td > pageNewest)) pageNewest = td;
            }
            const int before = tweetCount;
            const bool past = processTweetBatch(tweets);
            tlNew += tweetCount - before;
            // ★ 쪽 전체가 기간보다 옛것이면 멈춘다(검토 E2E-1) — processTweetBatch 는 이미 받아 둔 글을 기간 확인보다 먼저 건너뛰어,
            //   다 받아 둔 계정에선 기간 아래의 글이 모두 '이미 있음' 으로 지나가 past 가 서지 않았다(타임라인 끝까지 · 약 80쪽을 걸었다).
            //   쪽의 가장 옛 글이 아니라 가장 새 글로 본다 — 타임라인의 대화 묶음은 옛 뿌리 글을 새 답글 위에 붙여 오기 때문이다.
            if (past || (sinceDate.isValid() && pageNewest.isValid() && pageNewest < sinceDate)) break;
            if (tlPages % 10 == 0) {
                m_backend->log(QString("기간 타임라인 %1쪽 — %2 까지, +%3개")
                                   .arg(tlPages).arg(tlOldest.isValid() ? tlOldest.date().toString("yyyy-MM-dd") : QStringLiteral("?")).arg(tlNew),
                               "info", "twitter");
                if (excelCheckpointDue()) saveExcelStreaming(userDir, target, collectedData, excelSuffix);
            }
            if (nextCursor.isEmpty() || nextCursor == cursor) break;
            cursor = nextCursor;
            for (int w = qMax(2, (int)m_delay); w > 0 && isRunning && !stopNow(); --w) QThread::sleep(1);
        }
        if (tlNew > 0) saveExcelStreaming(userDir, target, collectedData, excelSuffix);
        if (isRunning && !capReached()) {
            if (sinceDate.isValid() && tlOldest.isValid() && tlOldest < sinceDate)
                m_backend->log(QString("기간 타임라인: +%1개 — 그 기간의 시작까지 닿았습니다%2").arg(tlNew)
                                   .arg(searchErrored ? QStringLiteral("(답글은 검색이 풀린 뒤 '답글' 판으로 받으십시오)") : QString()),
                               "success", "twitter");
            else if (tlOldest.isValid())
                m_backend->log(QString("기간 타임라인: +%1개 — 타임라인은 %2 까지만 닿았습니다. 그보다 옛 %3")
                                   .arg(tlNew).arg(tlOldest.date().toString("yyyy-MM-dd"))
                                   .arg(searchErrored ? QStringLiteral("글은 이번에 받지 못했습니다(검색이 풀리면 다시 받으십시오)")
                                                      : QStringLiteral("리트윗은 빠졌을 수 있습니다(X 가 타임라인을 최근 약 3200개까지만 줍니다)")),
                               "warning", "twitter");
            else
                m_backend->log(searchErrored ? QStringLiteral("기간 타임라인: 타임라인에서 아무것도 받지 못했습니다 — 이번 판에는 그 기간을 받지 못했습니다")
                                             : QStringLiteral("기간 타임라인: 타임라인에서 아무것도 받지 못했습니다 — 그 기간의 리트윗은 빠졌을 수 있습니다"),
                               "warning", "twitter");
        }
    }

    // ══════════════════════════════════════════════════════════════
    // Phase 2: UserTweets API (보충 — 검색에서 누락된 트윗 보충)
    // type="tweets_api"또는 type="all"에서만 실행
    // ══════════════════════════════════════════════════════════════
    if (isRunning && !capReached() && (type == "tweets"|| type == "tweets_api"|| type == "all") && mode != "period") {
        m_backend->log(QString("Phase 2: UserTweets API로 누락 보충 (현재 %1개)").arg(tweetCount), "info", "twitter");
        int phase2New = 0;
        QString cursor;
        bool hasMore = true;
        int emptyRetries = 0;
        int phase2Pages = 0;
        int phase2Err = 0;   // ★ 검색 단계와 같은 상한 — 오류가 5번 이어지면 이 단계를 멈춘다(끝없이 같은 요청을 보냈다)
        beginStreak(takeWalk("timeline"), "timeline");   // 맨 위부터 — 조용한 계정이면 여기서 몇 년 치(약 80쪽 · 쪽마다 2초 넘게)를 훑었다
        bool p2Broken = false;   // 오류 · 빈 응답으로 멈췄나

        while (isRunning && !stopNow() && hasMore) {
            auto [tweets, nextCursor] = getUserTweets(userId, cursor);

            if (nextCursor == "RATE_LIMITED") {
                handleRateLimit(accounts, currentAccountIdx, isRunning);
                continue;
            }
            if (nextCursor == "ERROR") {
                if (++phase2Err >= 5) {
                    m_backend->log("API 오류가 5번 이어졌습니다 — 타임라인 단계를 멈춥니다", "error", "twitter");
                    p2Broken = true;
                    break;
                }
                m_backend->log(QString("API 오류 — 5초 후 재시도 (%1/5)").arg(phase2Err), "warning", "twitter");
                for (int s = 5; s > 0 && isRunning; --s) QThread::sleep(1);
                continue;
            }
            phase2Err = 0;

            if (tweets.isEmpty()) {
                emptyRetries++;
                // ★ X 의 UserTweets 는 목록 끝에서 트윗 없이 커서만 든 쪽을 준다 — 그게 보통의 끝이다. 같은 커서로 5번 다시
                //   물어도 비면 끝으로 본다(쪽을 받은 뒤라면 다 본 것). 첫 쪽부터 비면 막힌 것인지 알 수 없어 다 본 것으로 치지 않는다.
                //   예전엔 이것도 '끊김' 으로 쳐 타임라인 표시가 한 번도 적히지 않았다(검토).
                if (emptyRetries >= 5) { if (phase2Pages == 0 || !m_lastPageShaped) p2Broken = true; break; }   // errors 만 든 200 · 모양 없는 응답도 끝이 아니다
                QThread::sleep(3);
                continue;
            }
            emptyRetries = 0;
            phase2Pages++;

            int prevCount = tweetCount;
            bool pastRange = processTweetBatch(tweets);
            phase2New += (tweetCount - prevCount);
            if (pastRange) break;

            if (phase2Pages % 10 == 0) {
                m_backend->log(QString("UserTweets %1 페이지, +%2개 새 트윗 (총 %3개)")
                                   .arg(phase2Pages).arg(phase2New).arg(tweetCount), "info", "twitter");
                // 중간 저장 — 할 때가 됐을 때만(excelCheckpointDue)
                if (excelCheckpointDue()) saveExcelStreaming(userDir, target, collectedData, excelSuffix);
            }

            cursor = nextCursor;
            if (cursor.isEmpty()) hasMore = false;
            if (hasMore && isRunning) { for (int w = qMax(2,(int)m_delay); w > 0 && isRunning && !stopNow(); --w) QThread::sleep(1); }
        }

        if (knownStop)
            m_backend->log(QString("Phase 2: 이미 받은 트윗이 %1개 이어져 여기서 멈춥니다").arg(knownStreak), "info", "twitter");
        m_backend->log(QString("Phase 2 완료: +%1개 보충 (총 %2개)").arg(phase2New).arg(tweetCount), "success", "twitter");
        if (collectedData.count() > 0) saveExcelStreaming(userDir, target, collectedData, excelSuffix);   // 저장한 뒤에 표시
        if (isRunning && !capReached() && !p2Broken)   // 목록 끝 · 일찍 멈춤
            putWalk("timeline");
    }

    // ══════════════════════════════════════════════════════════════
    // Phase 3: 답글 별도 검색 (filter:replies)
    // type="replies"또는 type="all"에서만 실행
    // ══════════════════════════════════════════════════════════════
    // ★ 기간 지정 — 예전엔 이 단계가 기간 판에서 통째로 빠져, '답글' 을 기간으로 받으면 0개였다(검토).
    //   기간 검색처럼 앞뒤로 하루씩 넓혀 묻고(X 의 날짜는 UTC, until 은 그 날을 뺀다) 글마다 일본 시간의 하루로 거른다.
    if (isRunning && !capReached() && (type == "replies"|| type == "all") && mode == "period") {
        //   ★ 질의 하나를 커서로만 넘기지 않고 기간의 시작까지 until: 로 물려 가며 걷는다(r56 — 1단계 기간 검색과 같다, boundedWalk)
        const QDate rSince = sinceDate.isValid() ? sinceDate.date().addDays(-1) : walkFloor.addDays(-1);
        const QDate rUntil = untilDate.isValid() ? untilDate.date().addDays(1) : QDate();
        m_backend->log(QString("Phase 3: 답글 기간 검색 (from:%1 filter:replies since:%2%3)")
                           .arg(target, rSince.toString("yyyy-MM-dd"),
                                rUntil.isValid() ? QStringLiteral(" until:") + rUntil.toString("yyyy-MM-dd") : QString()),
                       "info", "twitter");
        const BoundedWalk rp = boundedWalk(QString("from:%1 filter:replies").arg(target), rSince, rUntil, 0, QStringLiteral("답글 기간"), false);
        if (rp.holed)
            m_backend->log("기간 안의 어떤 날은 답글이 너무 많아 다 받지 못했을 수 있습니다", "warning", "twitter");
    }
    if (isRunning && !capReached() && (type == "replies"|| type == "all") && mode != "period") {
        m_backend->log("Phase 3: 답글 검색 (filter:replies) + 주간 백트래킹...", "info", "twitter");
        int replyNew = 0;
        bool replyErr = false;   // ★ 답글 검색도 오류가 5번 이어지면 멈춘다(Phase 1 과 같은 상한 — searchErrored 는 Phase 1 판정용이라 따로 둔다)
        const bool p3Armed = takeWalk("replies");
        bool p3GotAny = false;   // 답글 검색이 하나라도 돌려줬나 — 전부 빈 걸음은 '다 봤다' 가 아니다
        // ★ 모양 없는 빈 쪽(errors 만 든 200)은 '끝' · '빈 결과' 가 아니라 오류다 — 같은 질의를 다시 묻고(5번이면 멈춤) 빈 결과로 세지 않는다.
        //   한 번이라도 보였으면 이 판은 '답글' 걸음 표시를 적지 않는다(r56 — 예전엔 X 가 오류를 내는 구간을 빈 주로 넘기고도 끊김 없이 다 봤다고 적었다)
        bool p3Unshaped = false;
        bool p3Holed = false;    // 한 날이 넘쳐 그 날의 나머지를 건너뛰었을 수 있다(1단계 걸음의 p1Holed 와 같다)
        const int P3_MAX_PAGES = 400;   // 한 질의의 쪽 상한 · 같은 커서면 끝(1단계와 같다)

        // Step 1: 최신 답글부터 검색
        {
            QString replyQuery = QString("from:%1 filter:replies").arg(target);
            QString replyCursor;
            bool replyHasMore = true;
            int replyEmptyRetries = 0;
            beginStreak(p3Armed, "replies");

            int replyPages = 0;
            while (isRunning && !stopNow() && replyHasMore && replyPages < P3_MAX_PAGES) {
                auto [tweets, nextCursor] = searchTweets(replyQuery, replyCursor);
                if (nextCursor == "RATE_LIMITED") {
                    handleRateLimit(accounts, currentAccountIdx, isRunning);
                    continue;
                }
                if (nextCursor == "SUSPENDED") {
                    if (searchSuspendedGiveUp()) { replyErr = true; break; }
                    continue;
                }
                if (nextCursor == "ERROR") {
                    if (searchErrorGiveUp()) { replyErr = true; break; }
                    continue;
                }
                if (tweets.isEmpty() && !m_lastPageShaped) {
                    p3Unshaped = true;
                    if (searchErrorGiveUp()) { replyErr = true; break; }
                    continue;
                }
                searchErrStreak = 0;
                if (tweets.isEmpty()) {
                    replyEmptyRetries++;
                    if (replyEmptyRetries >= 5) break;
                    QThread::sleep(3);
                    continue;
                }
                replyEmptyRetries = 0;
                p3GotAny = true;

                // 가장 오래된 날짜 추적
                for (const auto &tv : tweets) {
                    QString ca = xLegacy(tv.toObject())["created_at"].toString();
                    QDateTime td = Common::parseISODate(ca);
                    if (td.isValid() && (!oldestTweetDate.isValid() || td < oldestTweetDate))
                        oldestTweetDate = td;
                }

                int prevCount = tweetCount;
                processTweetBatch(tweets);
                replyNew += (tweetCount - prevCount);
                ++replyPages;

                if (nextCursor == replyCursor) replyHasMore = false;   // 같은 커서가 다시 오면 끝
                replyCursor = nextCursor;
                if (replyCursor.isEmpty()) replyHasMore = false;
                if (replyHasMore) { for (int w = qMax(2,(int)m_delay); w > 0 && isRunning && !stopNow(); --w) QThread::sleep(1); }
            }
        }

        // Step 2: 아니포 방식 백트래킹 (until:oldest_date 반복)
        {
            const QDate replyStopDate = walkFloor;   // 계정을 만든 날 전에는 답글도 없다(답글은 1단계 from: 검색이 이미 함께 받는다 — 이 걸음은 보충)
            QDate replyLastOldest;  // 무한 루프 감지
            qulonglong replyMaxId = 0;   // 하루 넘침을 max_id 로 잇는 중(1단계 걸음과 같다 — r56)
            QDate replyMaxDay;
            int replyConsecutiveEmpty = 0;
            const int REPLY_MAX_EMPTY = 10;

            // 초기 until: Step 1 에서 찾은 가장 오래된 답글 날짜
            QString replyQuery2;
            if (oldestTweetDate.isValid()) {
                // 그 날을 다시 포함해 잇는다(D+1) — 1걸음이 날 한가운데서 끊겼을 수 있다(본 걸음과 같은 까닭)
                replyQuery2 = QString("from:%1 filter:replies until:%2")
                    .arg(target, oldestTweetDate.date().addDays(1).toString("yyyy-MM-dd"));
            } else {
                replyQuery2 = QString("from:%1 filter:replies").arg(target);
            }

            const bool replyStep1Stopped = knownStop;   // 1걸음이 받아 둔 곳에 닿았으면 2걸음(과거로 되짚기)은 하지 않는다
            beginStreak(p3Armed, "replies");
            if (replyStep1Stopped) knownStop = true;
            // ★ 이 보충 걸음은 예전처럼 빈 결과 10번에서 멈춘다 — 공백 건너기는 1단계(from: 검색 — 답글도 함께 받는다)만 한다(r55).
            //   '전체' 판은 답글을 하위 판(type=replies)으로 다시 부르므로, 여기서도 건너면 판마다 답글 역사를 처음부터 다시 걸었다(검토).
            while (isRunning && !stopNow() && !replyErr && replyConsecutiveEmpty < REPLY_MAX_EMPTY) {
                QString rCursor;
                int iterNew = 0;
                int iterBatch = 0;
                QDate iterOldest;
                qulonglong iterMinId = 0;
                const bool wasMaxId = replyMaxId != 0;
                int rPages = 0;

                while (isRunning && !stopNow() && rPages < P3_MAX_PAGES) {
                    auto [tweets, nextCursor] = searchTweets(replyQuery2, rCursor);
                    if (nextCursor == "RATE_LIMITED") {
                        handleRateLimit(accounts, currentAccountIdx, isRunning);
                        continue;
                    }
                    if (nextCursor == "SUSPENDED") {
                        if (searchSuspendedGiveUp()) { replyErr = true; break; }
                        continue;
                    }
                    if (nextCursor == "ERROR") {
                        if (searchErrorGiveUp()) { replyErr = true; break; }
                        continue;
                    }
                    if (tweets.isEmpty() && !m_lastPageShaped) {   // 오류 — 빈 결과로 세지 않는다(위 p3Unshaped 설명)
                        p3Unshaped = true;
                        if (searchErrorGiveUp()) { replyErr = true; break; }
                        continue;
                    }
                    searchErrStreak = 0;
                    if (tweets.isEmpty()) break;

                    iterBatch += tweets.size();
                    ++rPages;

                    for (const auto &tv : tweets) {
                        QString ca = xLegacy(tv.toObject())["created_at"].toString();
                        QDateTime td = Common::parseISODate(ca);
                        if (td.isValid()) {
                            QDate d = td.date();
                            if (!iterOldest.isValid() || d < iterOldest) iterOldest = d;
                            if (!oldestTweetDate.isValid() || td < oldestTweetDate) oldestTweetDate = td;
                        }
                        const QJsonObject to = tv.toObject();
                        QString tid = xLegacy(to)["id_str"].toString();
                        if (tid.isEmpty()) tid = to["rest_id"].toString();
                        if (const qulonglong idv = tid.toULongLong(); idv > 0 && (iterMinId == 0 || idv < iterMinId)) iterMinId = idv;
                    }

                    int prevCount = tweetCount;
                    processTweetBatch(tweets);
                    iterNew += (tweetCount - prevCount);
                    replyNew += (tweetCount - prevCount);

                    if (nextCursor.isEmpty() || nextCursor == rCursor) break;   // 같은 커서가 다시 오면 이 질의는 끝
                    rCursor = nextCursor;
                    for (int w = qMax(2, (int)m_delay); w > 0 && isRunning && !stopNow(); --w) QThread::sleep(1);
                }

                if (replyErr) break;
                if (iterBatch > 0) {
                    replyConsecutiveEmpty = 0;
                    p3GotAny = true;
                    if (iterNew > 0) saveExcelStreaming(userDir, target, collectedData, excelSuffix);
                } else if (!wasMaxId) {   // max_id 로 잇던 날의 나머지가 빈 것은 빈 결과로 세지 않는다
                    replyConsecutiveEmpty++;
                }

                // 다음 쿼리 — 하루 넘침은 max_id 로 잇는다(1단계 걸음의 설명과 같다)
                QDate nextUntil;
                qulonglong nextMaxId = 0;
                if (wasMaxId) {
                    if (iterBatch == 0) {
                        nextUntil = replyMaxDay;
                    } else if (iterMinId == 0 || iterMinId > replyMaxId) {
                        nextUntil = replyMaxDay;   // max_id 가 듣지 않았다 — 예전의 강제 후퇴, 구멍이 남았을 수 있다
                        p3Holed = true;
                    } else if (iterOldest.isValid() && iterOldest >= replyMaxDay) {
                        const auto qm = QRegularExpression(R"(until:(\d{4}-\d{2}-\d{2}))").match(replyQuery2);
                        nextUntil = qm.hasMatch() ? QDate::fromString(qm.captured(1), "yyyy-MM-dd") : replyMaxDay.addDays(1);
                        nextMaxId = iterMinId - 1;
                    } else {
                        nextUntil = iterOldest.isValid() ? iterOldest.addDays(1) : replyMaxDay;
                    }
                    if (!nextMaxId) replyLastOldest = nextUntil;
                } else if (iterOldest.isValid()) {
                    nextUntil = iterOldest.addDays(1);   // 그 날을 다시 포함해 잇는다(본 걸음과 같은 까닭)
                    if (replyLastOldest.isValid() && nextUntil >= replyLastOldest) {
                        if (iterMinId >= kSfMin) {
                            nextMaxId = iterMinId - 1;
                            replyMaxDay = iterOldest;
                        } else {
                            nextUntil = replyLastOldest.addDays(-1);
                            p3Holed = true;
                        }
                    }
                    if (!nextMaxId) replyLastOldest = nextUntil;
                } else {
                    QRegularExpression re(R"(until:(\d{4}-\d{2}-\d{2}))");
                    auto qm = re.match(replyQuery2);
                    QDate curUntil = qm.hasMatch() ? QDate::fromString(qm.captured(1), "yyyy-MM-dd") : QDate::currentDate();
                    if (!curUntil.isValid()) curUntil = QDate::currentDate();
                    nextUntil = curUntil.addDays(-7);
                }

                if (!nextUntil.isValid() || nextUntil < replyStopDate) break;

                replyQuery2 = QString("from:%1 filter:replies until:%2")
                    .arg(target, nextUntil.toString("yyyy-MM-dd"));
                if (nextMaxId) replyQuery2 += QString(" max_id:%1").arg(nextMaxId);
                replyMaxId = nextMaxId;
                for (int w = 3; w > 0 && isRunning; --w) QThread::sleep(1);
            }
            if (replyConsecutiveEmpty >= REPLY_MAX_EMPTY && isRunning && !stopNow() && !replyErr)
                m_backend->log(QStringLiteral("답글 검색: 빈 결과 10번 → 여기서 멈춥니다(더 오래된 답글은 '트윗' · '전체' 판의 from: 검색이 계정을 만든 날까지 받습니다)"),
                               "info", "twitter");
        }

        if (replyNew > 0) {
            m_backend->log(QString("Phase 3 완료: 답글 +%1개").arg(replyNew), "success", "twitter");
            saveExcelStreaming(userDir, target, collectedData, excelSuffix);
        }
        if (isRunning && !capReached() && !replyErr && p3GotAny && !p3Unshaped && !p3Holed)   // 두 걸음 다 끝까지(또는 일찍 멈춤) — 전부 빈 걸음 · 오류 쪽 · 구멍은 다 본 것이 아니다
            putWalk("replies");
    }

    // ══════════════════════════════════════════════════════════════
    // Phase 4: 답글 대화(conversation) 미디어 수집
    // 내가 쓴 답글에서 시작된 대화 스레드 → media/conversations/ 에 저장
    // ══════════════════════════════════════════════════════════════
    if (isRunning && (type == "replies"|| type == "all") && m_downloadMedia) {
        m_backend->log("Phase 4: 답글 대화(conversation) 미디어 수집...", "info", "twitter");
        QString convMediaDir = mediaDir + "/conversations";
        QDir().mkpath(convMediaDir);
        int convMediaCount = 0;
        int convTweetCount = 0;

        // ★ 이번 판에 새로 받은 답글(m_runReplyIds — '전체' 면 트윗 하위 판이 받은 답글도) + 지난 판에 다 보지 못하고 남긴 것(r56, F10).
        //   예전엔 수집 버퍼(collectedData)에서 읽었는데, 그 앞의 엑셀 저장이 버퍼를 비워 거의 늘 0개였다(대화 미디어를 받지 않았다).
        //   한 판에 PHASE4_MAX 개까지만 본다 — 처음 받는 큰 계정이면 답글이 수만 개라 TweetDetail 만 몇 시간이 된다. 남은 것은 엑셀 옆
        //   작은 파일(.hanishiki_conv_pending_<엑셀 이름>.json)에 남겨 다음 판이 이어 본다.
        const int PHASE4_MAX = 200;
        const QString convPendingPath = userDir + "/.hanishiki_conv_pending_" + target + "_" + excelFileSuffix + ".json";
        QList<QString> replyIdsWithConv;
        {
            QSet<QString> seenConv;
            QStringList cand = m_runReplyIds;
            std::sort(cand.begin(), cand.end(), [](const QString &a, const QString &b) { return a.toULongLong() > b.toULongLong(); });   // 새것부터
            for (const auto &v : readJsonFile(convPendingPath)["ids"].toArray()) cand << v.toString();
            for (const QString &rid : std::as_const(cand)) {
                if (rid.isEmpty() || seenConv.contains(rid) || collectedIds.contains("conv_" + rid)) continue;
                seenConv.insert(rid);
                replyIdsWithConv.append(rid);
            }
        }
        const int convTotal = int(replyIdsWithConv.size());
        QStringList convLeft;   // 이번에 보지 못한 것 — 다음 판에
        if (replyIdsWithConv.size() > PHASE4_MAX) {
            convLeft = replyIdsWithConv.mid(PHASE4_MAX);
            replyIdsWithConv = replyIdsWithConv.mid(0, PHASE4_MAX);
        }

        m_backend->log(QString("대화 가능 답글 %1개 탐색%2").arg(replyIdsWithConv.size())
                           .arg(convLeft.isEmpty() ? QString() : QString(" (남은 %1개는 다음 판에 — 한 판에 %2개까지)").arg(convLeft.size()).arg(PHASE4_MAX)),
                       "info", "twitter");
        int convDone = 0;   // 끝까지 본 대화 수(그 앞까지만 '봤다' 로 친다)

        for (int ci = 0; ci < replyIdsWithConv.size() && isRunning; ++ci) {
            const QString &replyId = replyIdsWithConv[ci];
            collectedIds.insert("conv_"+ replyId);
            convDone = ci;   // 이것을 보는 중 — 중지되면 이것부터 남긴다

            // TweetDetail로 대화 트리 조회
            QString cursor;
            bool hasMore = true;
            while (isRunning && hasMore) {
                auto [replies, nextCursor] = getTweetDetail(replyId, cursor);
                if (nextCursor == "RATE_LIMITED") {
                    handleRateLimit(accounts, currentAccountIdx, isRunning);
                    continue;
                }
                if (nextCursor == "ERROR"|| replies.isEmpty()) break;

                for (const auto &tv : replies) {
                    QJsonObject tw = tv.toObject();
                    QJsonObject leg = xLegacy(tw);
                    QString tid = leg["id_str"].toString();
                    if (tid.isEmpty()) tid = tw["rest_id"].toString();
                    if (tid.isEmpty() || collectedIds.contains(tid)) continue;
                    collectedIds.insert(tid);

                    // 미디어가 있는 경우만 다운로드
                    QJsonArray mediaArr = leg["extended_entities"].toObject()["media"].toArray();
                    if (mediaArr.isEmpty()) mediaArr = leg["entities"].toObject()["media"].toArray();
                    if (mediaArr.isEmpty()) continue;

                    QJsonObject authorResult = tw["core"].toObject()["user_results"].toObject()["result"].toObject();
                    QString convAuthor = screenNameOf(authorResult);   // 2026-08 부터 작성자에 legacy 가 없다
                    convTweetCount++;

                    int newMedia = downloadTweetMedia(tw, convMediaDir, convTweetCount);
                    convMediaCount += newMedia;
                    mediaCount += newMedia;

                    if (convTweetCount % 5 == 0 || newMedia > 0) {
                        m_backend->log(QString("대화 @%1: +%2 미디어").arg(convAuthor).arg(newMedia), "info", "twitter");
                    }
                }

                cursor = nextCursor;
                if (cursor.isEmpty()) hasMore = false;
                if (hasMore) QThread::sleep(2);
            }

            if (ci % 10 == 0) {
                m_backend->updateStats(tweetCount, mediaCount,
                    QString("대화 탐색 %1/%2").arg(ci + 1).arg(replyIdsWithConv.size()), "twitter");
            }
            if (isRunning) convDone = ci + 1;
        }
        // 보지 못한 것(중지로 끊긴 뒤의 것 + 한 판 상한 밖)을 남긴다 — 다 봤으면 파일을 지운다
        {
            QJsonArray left;
            for (int k = convDone; k < replyIdsWithConv.size(); ++k) left.append(replyIdsWithConv[k]);
            for (const QString &rid : std::as_const(convLeft)) left.append(rid);
            writeJsonFile(convPendingPath, left.isEmpty() ? QJsonObject() : QJsonObject{{"ids", left}});
            if (!left.isEmpty() && convTotal > 0)
                m_backend->log(QString("대화 미디어: %1개 답글은 다음 판에 이어 봅니다").arg(left.size()), "info", "twitter");
        }

        if (convMediaCount > 0) {
            m_backend->log(QString("Phase 4 완료: 대화 미디어 +%1개 (%2 대화)").arg(convMediaCount).arg(convTweetCount), "success", "twitter");
        } else {
            m_backend->log("Phase 4: 추가 대화 미디어 없음", "info", "twitter");
        }
    }

    if (capReached())
        m_backend->log(QString("최대 수집 수 %1개에 닿아 멈췄습니다 — 다음 '이어서 수집' 은 이번에 멈춘 자리를 다시 훑고, 받아 둔 것은 건너뜁니다.")
                           .arg(maxCount), "info", "twitter");

    {
        double collectRatio = (statusesCount > 0) ? (double)tweetCount / statusesCount : 1.0;
        m_backend->log(QString("최종: %1개 트윗, %2개 미디어 (수집률 %3%)")
                           .arg(tweetCount).arg(mediaCount).arg(qRound(collectRatio * 100)),
                       "success", "twitter");
    }

    // Save Excel (streaming — no full memory load)
    // 아니포 방식: Excel 자체가 체크포인트 → 항상 저장
    if (collectedData.count() > 0) {
        m_backend->log("Excel 저장...", "info", "twitter");
        saveExcelStreaming(userDir, target, collectedData, excelSuffix);
    }

    // 완료 시 progress 파일은 유지 (다음 이어서 수집에 사용)
    // 최종 상태 저장
    // ★ 최대 수집 수에서 멈춘 판은 progress 를 적지 않는다. 이 판의 가장 오래된·새 날짜는
    //   끝까지 못 본 구간을 덮고 있어, 적으면 다음 '이어서' 가 그 사이를 영영 건너뛴다.
    //   안 적으면 같은 자리를 다시 훑고, 받아 둔 것은 엑셀 ID 로 건너뛰므로 빠짐없이 이어진다.
    if (oldestTweetDate.isValid() && !capReached() && !searchErrored) {   // ★ 검색 오류 상한으로 멈춘 판도 최대 수와 같다 — 끝까지 못 본 구간의 날짜를 적지 않는다(끝난 iteration 은 루프 안 saveProgress(nextUntil) 가 이미 적었다)
        // ★ 일찍 멈춘 판은 '가장 새' 만 올리고 '가장 오래된' 은 지킨다(빈 값 = 지난 값) — 받아 둔 구간에서 멈춘 날짜를
        //   적으면 다음 '과거에서 이어서' 가 받아 둔 한가운데서 시작해 곧바로 또 멈췄다
        saveProgress(knownStopMax > 0 ? QString() : oldestTweetDate.toString("yyyy-MM-dd"));   // 빈 값 = 지난 값(지난 값도 없으면 빈 채)
    }
    // 대상 목록에 남길 이번 판 결과 — 새 트윗 수 · 이 계정의 가장 새 활동 · 일찍 멈춤
    m_backend->noteTrackFresh(tweetCount - runStartCount);
    if (newestActivityMs > 0) m_backend->noteTrackNewest(newestActivityMs);
    if (knownStopMax > 0) m_backend->noteTrackEarlyStop(knownStopMax);

    m_backend->log(QString("완료! %1: %2, 미디어: %3").arg(logLabel).arg(tweetCount).arg(mediaCount),
                   "success", "twitter");
    m_backend->updateStats(tweetCount, mediaCount, "완료", "twitter");

    // Save profile Excel (all encountered users: RT, quoted, etc.)
    saveProfileExcel(userDir, target);

    // 새트윗 확인용: userDir 저장
    m_currentUserDir = userDir;

    // Clean up daemon
    if (!isSubCall) stopDaemon();
}

void TwitterCollector::checkNewPosts(const QJsonObject &config, const std::atomic<bool> &isRunning)
{
    m_runFlag = &isRunning;
    // ★ 여기가 문제의 자리다 — 이 함수는 collect() 를 돌린 워커와 '다른' 워커에서 온다
    //   (HanishikiBackend::checkNewPosts 의 QThread::create). 앞서 만든 것을 그대로 쓰면
    //   죽은 스레드의 NAM 이다. 새로 만들었으니 중지 플래그도 다시 단다
    //   (안 달면 '새 트윗 확인' 중 중지를 눌러도 미디어 다운로드가 안 끊긴다).
    adoptHttpToCurrentThread();
    if (m_http) m_http->setRunFlag(&isRunning);

    if (m_newestTweetId.isEmpty()) {
        m_backend->log("아직 수집된 트윗이 없습니다", "warning", "twitter");
        return;
    }
    if (m_currentTarget.isEmpty()) {
        m_backend->log("타겟 사용자 정보가 없습니다", "warning", "twitter");
        return;
    }

    m_backend->log("════════════════════════════════", "info", "twitter");
    m_backend->log("새 트윗 체크 중... (리트윗 포함)", "info", "twitter");

    QJsonArray accounts = config["accounts"].toArray();
    if (accounts.isEmpty()) {
        m_backend->log("계정이 없습니다", "error", "twitter");
        return;
    }

    // Use first account
    QJsonObject acct = accounts[0].toObject();
    setupClientFor(acct);
    m_accounts = QJsonArray();   // 계정을 돌리지 않는다(r56 — 정지 계정 · 남은 요청 0 의 자동 전환은 수집 판에서만)
    m_activeAccountIdx = 0;

    if (!startDaemon()) {
        m_backend->log("데몬 시작 실패", "error", "twitter");
        return;
    }

    // UserTweets API로 최신 트윗 가져오기 (리트윗 포함!)
    // SearchTimeline은 from:user 검색 시 리트윗을 빠뜨리므로 UserTweets 사용
    QString userId = config["userId"].toString();
    if (userId.isEmpty()) {
        // userId 없으면 먼저 조회
        QJsonObject userObj = getUserByScreenName(m_currentTarget);
        userId = userObj["rest_id"].toString();
    }

    if (userId.isEmpty()) {
        m_backend->log("유저 ID 조회 실패", "error", "twitter");
        stopDaemon();
        return;
    }

    auto [tweets, nextCursor] = getUserTweets(userId);

    // newest ID보다 큰 것만 필터
    QJsonArray newTweets;
    qlonglong newestIdNum = m_newestTweetId.toLongLong();
    for (const auto &tweetVal : tweets) {
        QJsonObject tweet = tweetVal.toObject();
        QJsonObject legacy = xLegacy(tweet);
        QString tweetId = legacy["id_str"].toString();
        if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();
        if (tweetId.toLongLong() > newestIdNum) {
            newTweets.append(tweet);
        }
    }

    if (newTweets.isEmpty()) {
        m_backend->log("→ 새 트윗 없음", "info", "twitter");
        m_backend->log("════════════════════════════════", "info", "twitter");
        stopDaemon();
        return;
    }

    m_backend->log(QString("새 트윗 %1개 발견!").arg(newTweets.size()), "success", "twitter");

    int newCount = 0;
    int newMedia = 0;
    QString mediaDir = m_currentUserDir + "/media";
    QDir().mkpath(mediaDir);

    for (const auto &tweetVal : newTweets) {
        if (!isRunning) break;
        QJsonObject tweet = tweetVal.toObject();
        QJsonObject legacy = xLegacy(tweet);
        QString tweetId = legacy["id_str"].toString();
        if (tweetId.isEmpty()) tweetId = tweet["rest_id"].toString();

        // Update newest ID
        if (tweetId.toLongLong() > m_newestTweetId.toLongLong())
            m_newestTweetId = tweetId;

        // RT 여부 체크
        bool isRT = legacy.contains("retweeted_status_result") || tweet.contains("retweeted_status_result");
        QString text = legacy["full_text"].toString();
        if (isRT) {
            QJsonObject rtResult = legacy["retweeted_status_result"].toObject()["result"].toObject();
            if (rtResult.isEmpty()) rtResult = tweet["retweeted_status_result"].toObject()["result"].toObject();
            if (rtResult.contains("tweet")) rtResult = rtResult["tweet"].toObject();
            QString rtAuthor = xLegacy(rtResult)["screen_name"].toString();
            m_backend->log(QString("RT @%1: %2...").arg(rtAuthor, xLegacy(rtResult)["full_text"].toString().left(40)), "info", "twitter");
        } else {
            m_backend->log(QString("%1...").arg(text.left(50)), "info", "twitter");
        }

        // Download media (타입별 폴더 분리)
        if (m_downloadMedia) {
            QString newMediaDir = isRT ? (mediaDir + "/reposts") : (mediaDir + "/tweets");
            QDir().mkpath(newMediaDir);
            int dm = downloadTweetMedia(tweet, newMediaDir, newCount + 1);
            newMedia += dm;
        }

        newCount++;
    }

    m_backend->log(QString("새 트윗 %1개 (RT포함), 미디어 %2개").arg(newCount).arg(newMedia), "success", "twitter");
    m_backend->log("════════════════════════════════", "info", "twitter");

    stopDaemon();
}

// ─────────────────────────────────────────────────────────────────────────
// 팔로잉 목록만 — 사용자(2026-10-15): 계정이 3~4개면 괜찮은데 대용량이라 정말 불편. 다른 앱들(gallery-dl 의 following,
//   tmd 의 --foll)처럼 '누구의 팔로잉' 을 한 번에 대상 목록으로 가져온다. 'following' 수집과 같은 API 를 쓰되,
//   사람마다 프로필 사진 · 배너를 받지 않고 아이디와 이름만 모은다(그쪽은 3천 명이면 수천 개 파일을 받는다).
// ─────────────────────────────────────────────────────────────────────────
QJsonArray TwitterCollector::listFollowing(const QJsonObject &config, const QString &handle, int maxCount,
                                           const std::atomic<bool> &isRunning, QString *err, std::atomic<qint64> *pidOut)
{
    QJsonArray out;
    adoptHttpToCurrentThread();
    const QJsonArray accounts = config["accounts"].toArray();
    if (accounts.isEmpty()) {
        if (err) *err = QStringLiteral("트위터 계정을 먼저 추가하십시오");
        return out;
    }
    const QJsonObject acc = accounts[0].toObject();
    setupClientFor(acc);
    m_accounts = QJsonArray();   // 계정을 돌리지 않는다(r56)
    m_activeAccountIdx = 0;
    if (!startDaemon()) {
        if (err) *err = QStringLiteral("트위터 도우미를 띄우지 못했습니다");
        return out;
    }
    if (pidOut) pidOut->store(daemonPid());
    // 그만 · 끄기 — 도중에 멈추면 도우미를 내리고 PID 를 지운다(지운 뒤엔 남이 그 PID 를 죽이지 않게)
    auto finish = [&]() {
        if (pidOut) pidOut->store(0);
        stopDaemon();
        return out;
    };
    auto alive = [&]() { return isRunning && !Common::appQuitting(); };
    if (!alive()) return finish();
    const QJsonObject user = getUserByScreenName(handle);
    if (user.isEmpty()) {
        if (err && alive()) *err = QStringLiteral("사용자를 찾을 수 없습니다: @") + handle;
        return finish();
    }
    const QString userId = user["rest_id"].toString();
    m_delay = 2.0;
    m_consecutiveOk = 0;
    QString cursor;
    QSet<QString> seen;
    auto capped = [&]() { return maxCount > 0 && out.size() >= maxCount; };
    while (alive() && !capped()) {
        QJsonObject cmd;
        cmd["action"] = QStringLiteral("following");
        cmd["user_id"] = userId;
        cmd["count"] = 40;
        if (!cursor.isEmpty()) cmd["cursor"] = cursor;
        const QJsonObject resp = sendDaemonCommand(cmd);
        if (!alive()) break;   // 그만 · 끄기로 도우미를 끊었다 — 그 오류는 알리지 않는다
        if (resp.contains("error")) {
            if (err) *err = resp["error"].toString();
            break;
        }
        const int status = resp["status"].toInt();
        if (status == 429) {
            // ★ handleRateLimit 은 쓰지 않는다 — 그것은 트위터 탭의 상태 칸을 'RL待機' 로 바꾸고 되돌리지 않아, 가져오기가
            //   끝난 뒤에도 트위터가 '돌고 있음' 으로 보였고 돌던 수집의 상태도 덮었다(검토). 여기선 기록에만 적고 기다린다.
            m_backend->log(QStringLiteral("팔로잉 가져오기: X 의 속도 제한 — 15분 기다렸다 잇습니다(지금까지 %1명 · 그만으로 멈출 수 있음)").arg(out.size()),
                           "warning", "twitter");
            for (int i = 0; i < 9000 && alive(); ++i) QThread::msleep(100);
            continue;
        }
        if (status != 200) {
            if (err) *err = QString("X 가 %1 로 답했습니다").arg(status);
            break;
        }
        const QJsonArray instructions = xInstructions(resp["body"].toObject(), {"user", "user_result"});
        bool found = false;
        QString nextCursor;
        for (const auto &inst : instructions) {
            const QJsonObject instruction = inst.toObject();
            if (instruction["type"].toString() != QLatin1String("TimelineAddEntries")) continue;
            for (const auto &entry : instruction["entries"].toArray()) {
                const QJsonObject e = entry.toObject();
                const QString eid = e["entryId"].toString();
                if (eid.startsWith("user-")) {
                    const QJsonObject ur = xUserResult(e);
                    const QString sn = screenNameOf(ur);
                    if (sn.isEmpty() || seen.contains(sn.toLower()) || capped()) continue;
                    seen.insert(sn.toLower());
                    QJsonObject u;
                    u["handle"] = sn;
                    u["name"] = displayNameOf(ur);
                    u["protected"] = xUserLegacy(ur)["protected"].toBool();
                    out.append(u);
                    found = true;
                } else if (eid.startsWith("cursor-bottom")) {
                    nextCursor = e["content"].toObject()["value"].toString();
                }
            }
        }
        if (!alive()) break;
        m_backend->log(QString("팔로잉 가져오는 중: %1명").arg(out.size()), "info", "twitter");
        if (nextCursor.isEmpty() || !found || nextCursor == cursor) break;
        cursor = nextCursor;
        for (int i = 0; i < static_cast<int>(m_delay * 10) && alive(); ++i) QThread::msleep(100);   // 쪽 사이 — 그만에 곧 멈추게
    }
    return finish();
}
