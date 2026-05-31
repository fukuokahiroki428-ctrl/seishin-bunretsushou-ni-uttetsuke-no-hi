#pragma once

#include <QObject>
#include <QString>
#include <QJsonObject>
#include <QJsonArray>
#include <QSet>
#include <QStringList>
#include <QUrl>
#include <QQueue>
#include <QTimer>
#include <QMap>
#include <functional>

class MiyoBackend;
class HttpClient;
class QWebEnginePage;
class QWebEngineProfile;
class QWebEngineScript;

class SiteCrawler : public QObject
{
    Q_OBJECT

public:
    explicit SiteCrawler(MiyoBackend *backend, QWebEngineProfile *sharedProfile = nullptr, QObject *parent = nullptr);
    ~SiteCrawler() override;

    void crawl(const QJsonObject &config);
    void stop();
    void continueAfterLogin();
    bool isWaitingForLogin() const { return m_waitingForLogin; }
    QWebEnginePage *page() const { return m_page; }

signals:
    void finished();

private slots:
    void onPageLoaded(bool ok);
    void processNextInQueue();

private:
    // Page processing
    void processRenderedHtml(const QString &html, const QString &url, int depth);
    QSet<QString> extractLinks(const QString &html, const QUrl &baseUrl);
    QSet<QString> extractAllResources(const QString &html, const QUrl &baseUrl);
    QString normalizeUrl(const QString &href, const QUrl &baseUrl);
    bool shouldCrawl(const QString &url);

    // Deep crawl: SPA + 무한스크롤 + AJAX
    void deepScrollAndExtract(int maxScrolls, int scrollWaitMs, std::function<void(const QString&)> callback);
    void extractDynamicLinks(std::function<void(const QSet<QString>&)> callback);
    void injectApiMonitor();
    void collectApiCalls(std::function<void(const QJsonArray&)> callback);

    // Download + 보안 스캔
    QString downloadAndMap(const QString &url);
    QString urlToLocalPath(const QString &url);
    void processCssFile(const QString &cssFilePath, const QUrl &cssBaseUrl);

    // HTML rewriting + CSP
    // currentPageLocal: 현재 저장될 HTML의 root-relative 경로 (예: "sub/inner.html")
    //                   치환할 링크들은 이 경로의 디렉토리 기준으로 상대경로로 변환됨.
    QString rewriteHtml(const QString &html, const QUrl &baseUrl, const QString &currentPageLocal);
    // 크롤 범위 내 URL에 대한 "예약 로컬 경로" 반환.
    // 이미 처리된 URL이면 m_urlToLocal 값, 아직 처리 안됐지만 크롤 대상이면 미래 경로를 예약.
    // 외부 URL/javascript:/mailto: 등은 빈 문자열 반환(원본 유지).
    QString ensureLocalPath(const QString &url);
    // root-relative 경로를 currentPage가 있는 디렉토리 기준 상대경로로 변환
    // 예) fromPage="sub/inner.html", toPath="page.html" → "../page.html"
    QString toPageRelativePath(const QString &fromPage, const QString &toPath);
    // HTML entities 디코딩 (&amp; &lt; &#x2F; 등) — href/src에서 자주 발생
    QString decodeHtmlEntities(const QString &s);
    // 도메인 매칭 — base 와 host 가 같거나, 한쪽이 다른쪽의 서브도메인이면 true
    // 예) base="www.meti.go.jp", host="meti.go.jp"      → true
    //    base="meti.go.jp",     host="api.meti.go.jp" → true
    bool isSameDomain(const QString &host) const;
    // 저장된 CSS 파일에 대해 url() 참조를 최신 m_urlToLocal 기준으로 재치환
    void postProcessCssFile(const QString &cssFilePath, const QString &cssUrl);
    // 크롤 완료 후 저장된 HTML들을 다시 순회하며 아직 절대 URL로 남은 것들을 로컬로 교체
    void postProcessRewrite();

    // Excel / Index / Security Report
    void savePageListExcel(const QString &saveDir);
    void generateMainIndex();
    void generateDomainIndex();  // {domain}/index.html — 세션 목록
    void generateParentIndex();  // {parent}/index.html — 도메인 목록
    void saveSecurityReport();
    void finishCrawl();

    MiyoBackend *m_backend;
    QWebEngineProfile *m_profile = nullptr;
    bool m_ownsProfile = false;
    QWebEnginePage *m_page = nullptr;
    HttpClient *m_http = nullptr;

    // BFS state
    struct CrawlItem { QString url; int depth; };
    QQueue<CrawlItem> m_queue;
    CrawlItem m_current;
    bool m_running = false;

    // Visited / downloaded tracking
    QSet<QString> m_visited;
    QSet<QString> m_processedCss;  // CSS 재귀 방지
    QMap<QString, QString> m_urlToLocal;
    QList<QJsonObject> m_pageList;
    int m_pageCount = 0;
    int m_resourceCount = 0;

    // Security
    QJsonArray m_securityFindings;
    int m_quarantineCount = 0;
    int m_scanCount = 0;

    // Config
    int m_maxDepth = 3;
    int m_maxPages = 1000;
    double m_delay = 1.0;
    bool m_sameDomainOnly = true;
    bool m_deepScroll = true;
    bool m_sandboxJs = true;
    bool m_securityScan = true;
    bool m_saveExif = true;
    bool m_waitLogin = false;
    bool m_waitingForLogin = false;
    QString m_pendingStartUrl;
    int m_maxScrollIterations = 5;
    QString m_baseDomain;
    QString m_saveDir;      // {parentDir}/{domain}/{session}  — 이번 크롤 세션 폴더
    QString m_domainDir;    // {parentDir}/{domain}            — 도메인 폴더 (세션 목록)
    QString m_parentDir;    // {parentDir}                     — 최상위 (도메인 목록)
};
