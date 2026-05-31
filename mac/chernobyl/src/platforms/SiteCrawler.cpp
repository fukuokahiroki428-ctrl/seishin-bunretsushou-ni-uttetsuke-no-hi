#include "SiteCrawler.h"
#include "core/MiyoBackend.h"
#include "core/Common.h"
#include "utils/HttpClient.h"
#include "utils/ExcelWriter.h"
#include "utils/ContentSecurityScanner.h"

#include <QWebEnginePage>
#include <QWebEngineProfile>
#include <QWebEngineSettings>
#include <QWebEngineScript>
#include <QWebEngineScriptCollection>
#include <QWebEngineCookieStore>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QUrl>
#include <QCryptographicHash>
#include <QTimer>
#include <QProcess>
#include <QDateTime>

SiteCrawler::SiteCrawler(MiyoBackend *backend, QWebEngineProfile *sharedProfile, QObject *parent)
    : QObject(parent)
    , m_backend(backend)
    , m_http(new HttpClient(this))
{
    m_http->setTimeout(30000);
    m_http->setDownloadTimeout(120000);

    if (sharedProfile) {
        m_profile = sharedProfile;
        m_ownsProfile = false;
    } else {
        m_profile = new QWebEngineProfile(this);
        m_profile->setHttpUserAgent(
            "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) "
            "AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36");
        m_ownsProfile = true;
    }

    m_page = new QWebEnginePage(m_profile, this);

    auto *settings = m_page->settings();
    settings->setAttribute(QWebEngineSettings::JavascriptEnabled, true);
    settings->setAttribute(QWebEngineSettings::AutoLoadImages, true);
    settings->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);

    connect(m_page, &QWebEnginePage::loadFinished, this, &SiteCrawler::onPageLoaded);
}

SiteCrawler::~SiteCrawler() {}

void SiteCrawler::crawl(const QJsonObject &config)
{
    QString startUrl = config["url"].toString().trimmed();
    if (startUrl.isEmpty()) {
        m_backend->log("URL이 비어있습니다", "error", "crawl");
        emit finished();
        return;
    }
    if (!startUrl.startsWith("http")) startUrl = "https://" + startUrl;

    m_saveDir = config["path"].toString();
    m_saveDir.replace("~", QDir::homePath());
    if (m_saveDir.isEmpty()) m_saveDir = QDir::homePath() + "/Downloads/Crawl";

    m_maxDepth  = config["depth"].toInt(3);
    m_maxPages  = config["maxPages"].toInt(1000);
    m_delay     = config["delay"].toDouble(1.0);
    m_sameDomainOnly = config["sameDomain"].toBool(true);
    m_saveExif = config["exif"].toBool(true);
    m_waitLogin = config["waitLogin"].toBool(false);

    QUrl baseUrl(startUrl);
    m_baseDomain = baseUrl.host();

    // 도메인 폴더 생성 + 크롤 세션별 타임스탬프 하위 폴더
    // 구조: {path}/{domain}/{YYYYMMDD_HHmmss}/...
    // - 매번 독립된 폴더라 기존 크롤 덮어쓰지 않음
    // - {path}/index.html 도메인 목록
    // - {path}/{domain}/index.html 해당 도메인 세션 목록
    // - {path}/{domain}/{session}/index.html 실제 크롤 콘텐츠
    QString domainDir = m_baseDomain;
    domainDir.replace(QRegularExpression("[^a-zA-Z0-9._-]"), "_");
    QString sessionStamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
    m_parentDir = m_saveDir;                              // {path}
    m_domainDir = m_saveDir + "/" + domainDir;            // {path}/{domain}
    m_saveDir   = m_domainDir + "/" + sessionStamp;       // {path}/{domain}/{session}
    QDir().mkpath(m_saveDir);
    QDir().mkpath(m_saveDir + "/css");
    QDir().mkpath(m_saveDir + "/js");
    QDir().mkpath(m_saveDir + "/images");
    QDir().mkpath(m_saveDir + "/fonts");
    QDir().mkpath(m_saveDir + "/media");
    QDir().mkpath(m_saveDir + "/_quarantine");
    QDir().mkpath(m_saveDir + "/api");

    m_visited.clear();
    m_urlToLocal.clear();
    m_processedCss.clear();
    m_pageList.clear();
    m_securityFindings = QJsonArray();
    m_pageCount = 0;
    m_resourceCount = 0;
    m_quarantineCount = 0;
    m_scanCount = 0;
    m_running = true;

    // 중지 시 진행 중 HTTP 다운로드 즉시 abort
    if (m_http) m_http->setRunFlag(&m_running);

    // API 호출 모니터링 주입
    injectApiMonitor();

    // 브라우저 프로필의 쿠키를 HttpClient에 동기화
    if (!m_ownsProfile) {
        m_backend->log("브라우저 로그인 세션 사용 (쿠키 동기화)", "info", "crawl");
        QMap<QString, QString> cookies;
        auto *cookieStore = m_profile->cookieStore();
        QEventLoop loop;
        auto conn = QObject::connect(cookieStore, &QWebEngineCookieStore::cookieAdded,
            [&cookies](const QNetworkCookie &cookie) {
                cookies[cookie.name()] = cookie.value();
            });
        cookieStore->loadAllCookies();
        QTimer::singleShot(500, &loop, &QEventLoop::quit);
        loop.exec();
        QObject::disconnect(conn);
        if (!cookies.isEmpty()) {
            m_http->setCookies(cookies);
            m_backend->log(QString("  쿠키 %1개 로드됨").arg(cookies.size()), "info", "crawl");
        }
    }

    m_backend->log("크롤링 시작: " + startUrl, "info", "crawl");
    m_backend->log(QString("깊이: %1 | 최대 페이지: %2 | 딜레이: %3초")
                       .arg(m_maxDepth).arg(m_maxPages).arg(m_delay), "info", "crawl");
    m_backend->log("저장: " + m_saveDir, "info", "crawl");

    m_visited.insert(startUrl);
    m_queue.enqueue({startUrl, 0});

    if (m_waitLogin) {
        // 로그인 대기 모드: 시작 URL만 브라우저에 띄우고, 사용자가
        // 로그인 후 "로그인 완료" 버튼을 누를 때까지 BFS 시작을 보류한다.
        m_waitingForLogin = true;
        m_pendingStartUrl = startUrl;
        m_page->load(QUrl(startUrl));
        m_backend->log("🔐 로그인 대기 중 — 브라우저에서 로그인 후 '로그인 완료' 버튼을 눌러주세요", "warning", "crawl");
        m_backend->runJs("if(window.showCrawlLoginConfirm)window.showCrawlLoginConfirm(true)");
        m_backend->updateStats(0, 0, "로그인 대기", "crawl");
        return;
    }

    processNextInQueue();
}

void SiteCrawler::continueAfterLogin()
{
    if (!m_waitingForLogin) return;
    m_waitingForLogin = false;
    m_backend->log("✅ 로그인 완료 — 크롤링 시작", "success", "crawl");
    m_backend->runJs("if(window.showCrawlLoginConfirm)window.showCrawlLoginConfirm(false)");
    // 쿠키 재동기화 (로그인 후 새로 생긴 세션 쿠키 반영)
    if (!m_ownsProfile && m_profile) {
        QMap<QString, QString> cookies;
        auto *cookieStore = m_profile->cookieStore();
        QEventLoop loop;
        auto conn = QObject::connect(cookieStore, &QWebEngineCookieStore::cookieAdded,
            [&cookies](const QNetworkCookie &cookie) {
                cookies[cookie.name()] = cookie.value();
            });
        cookieStore->loadAllCookies();
        QTimer::singleShot(500, &loop, &QEventLoop::quit);
        loop.exec();
        QObject::disconnect(conn);
        if (!cookies.isEmpty()) {
            m_http->setCookies(cookies);
            m_backend->log(QString("  로그인 후 쿠키 %1개 동기화").arg(cookies.size()), "info", "crawl");
        }
    }
    processNextInQueue();
}

void SiteCrawler::stop()
{
    m_running = false;
    m_waitingForLogin = false;
    m_backend->runJs("if(window.showCrawlLoginConfirm)window.showCrawlLoginConfirm(false)");
    m_page->triggerAction(QWebEnginePage::Stop);
}

void SiteCrawler::processNextInQueue()
{
    if (!m_running || m_queue.isEmpty() || m_pageCount >= m_maxPages) {
        finishCrawl();
        return;
    }

    m_current = m_queue.dequeue();
    m_backend->updateStats(m_pageCount, m_resourceCount, "로딩 중", "crawl");

    m_backend->runJs(QString("if(document.getElementById('browser-url'))document.getElementById('browser-url').value='%1';")
        .arg(QString(m_current.url).replace("'", "\\'")));

    m_page->load(QUrl(m_current.url));

    QTimer::singleShot(30000, this, [this]() {
        if (m_running && m_page->isLoading()) {
            m_backend->log("타임아웃: " + m_current.url, "warning", "crawl");
            m_page->triggerAction(QWebEnginePage::Stop);
        }
    });
}

void SiteCrawler::onPageLoaded(bool ok)
{
    if (!m_running) { finishCrawl(); return; }

    // 로그인 대기 모드: 첫 페이지 로드 후 사용자가 직접 로그인 → '로그인 완료' 대기
    if (m_waitingForLogin) {
        return;
    }

    if (!ok) {
        m_backend->log(QString("로드 실패: %1").arg(m_current.url), "warning", "crawl");
        int ms = static_cast<int>(m_delay * 1000);
        QTimer::singleShot(ms, this, &SiteCrawler::processNextInQueue);
        return;
    }

    // 정부/느린 사이트용으로 JS 초기 렌더 대기 시간 3초로 증가
    // (React/Vue 앱은 loadFinished 이후에도 DOM이 계속 업데이트됨)
    QTimer::singleShot(3000, this, [this]() {
        if (!m_running) { finishCrawl(); return; }

        if (m_deepScroll) {
            deepScrollAndExtract(m_maxScrollIterations, 2000, [this](const QString &html) {
                if (!m_running) { finishCrawl(); return; }
                extractDynamicLinks([this, html](const QSet<QString> &dynamicLinks) {
                    for (const QString &link : dynamicLinks) {
                        if (m_visited.contains(link) || !shouldCrawl(link)) continue;
                        m_visited.insert(link);
                        m_queue.enqueue({link, m_current.depth + 1});
                    }
                    collectApiCalls([this, html](const QJsonArray &apiCalls) {
                        if (!apiCalls.isEmpty()) {
                            for (const auto &call : apiCalls) {
                                QJsonObject c = call.toObject();
                                QString apiUrl = normalizeUrl(c["url"].toString(), QUrl(m_current.url));
                                if (!apiUrl.isEmpty() && !m_visited.contains(apiUrl) && c["method"].toString() == "GET") {
                                    QString safeName = QCryptographicHash::hash(apiUrl.toUtf8(), QCryptographicHash::Md5).toHex().left(16) + ".json";
                                    QString apiPath = m_saveDir + "/api/" + safeName;
                                    m_http->downloadFile(apiUrl, apiPath);
                                }
                            }
                        }
                        // ★ 트위터: 스크롤 중 누적된 tweet 버퍼를 드레인 → tweets/{id}.json + 미디어 다운로드
                        QUrl curUrl(m_current.url);
                        QString host = curUrl.host().toLower();
                        bool isTwitter = (host == "x.com" || host == "twitter.com" ||
                                          host.endsWith(".x.com") || host.endsWith(".twitter.com"));
                        if (isTwitter) {
                            m_page->runJavaScript(
                                "(function(){var b=window.__crawl_tweet_buffer||{};window.__crawl_tweet_buffer={};return JSON.stringify(b);})()",
                                [this, html](const QVariant &bufVar) {
                                    QJsonObject buf = QJsonDocument::fromJson(bufVar.toString().toUtf8()).object();
                                    if (!buf.isEmpty()) {
                                        QString tweetsDir = m_saveDir + "/tweets";
                                        QString mediaDir = m_saveDir + "/media";
                                        QDir().mkpath(tweetsDir);
                                        QDir().mkpath(mediaDir);
                                        int savedTweets = 0;
                                        int savedMedia = 0;
                                        for (auto it = buf.constBegin(); it != buf.constEnd(); ++it) {
                                            QString tid = it.key();
                                            QJsonObject tw = it.value().toObject();
                                            QString tpath = tweetsDir + "/" + tid + ".json";
                                            if (!QFile::exists(tpath)) {
                                                QFile tf(tpath);
                                                if (tf.open(QIODevice::WriteOnly)) {
                                                    tf.write(QJsonDocument(tw).toJson(QJsonDocument::Indented));
                                                    tf.close();
                                                    savedTweets++;
                                                }
                                            }
                                            // 미디어 다운로드 (orig 화질)
                                            QJsonArray imgs = tw["images"].toArray();
                                            int mi = 0;
                                            for (const auto &iv : imgs) {
                                                QString iurl = iv.toString();
                                                if (iurl.isEmpty()) continue;
                                                QString ext = "jpg";
                                                if (iurl.contains("format=png")) ext = "png";
                                                else if (iurl.contains("format=webp")) ext = "webp";
                                                QString fname = QString("%1_%2.%3").arg(tid).arg(++mi).arg(ext);
                                                QString fp = mediaDir + "/" + fname;
                                                if (!QFile::exists(fp) && m_http->downloadFile(iurl, fp)) {
                                                    savedMedia++;
                                                }
                                            }
                                            QJsonArray vids = tw["videos"].toArray();
                                            for (const auto &vv : vids) {
                                                QString vurl = vv.toString();
                                                if (vurl.isEmpty()) continue;
                                                QString fname = QString("%1_%2.mp4").arg(tid).arg(++mi);
                                                QString fp = mediaDir + "/" + fname;
                                                if (!QFile::exists(fp) && m_http->downloadFile(vurl, fp)) {
                                                    savedMedia++;
                                                }
                                            }
                                        }
                                        if (savedTweets > 0 || savedMedia > 0) {
                                            m_backend->log(QString("트위터: 트윗 %1개 + 미디어 %2개 저장")
                                                .arg(savedTweets).arg(savedMedia), "success", "crawl");
                                        }
                                    }
                                    processRenderedHtml(html, m_current.url, m_current.depth);
                                });
                            return;
                        }
                        processRenderedHtml(html, m_current.url, m_current.depth);
                    });
                });
            });
        } else {
            m_page->toHtml([this](const QString &html) {
                processRenderedHtml(html, m_current.url, m_current.depth);
            });
        }
    });
}

void SiteCrawler::processRenderedHtml(const QString &html, const QString &url, int depth)
{
    if (!m_running) { finishCrawl(); return; }

    m_pageCount++;
    m_backend->updateStats(m_pageCount, m_resourceCount, "크롤링 중", "crawl");

    if (m_pageCount % 100 == 0) {
        m_backend->log(QString("메모리 정리 (%1 페이지 처리됨)").arg(m_pageCount), "info", "crawl");
        if (!m_pageList.isEmpty()) {
            savePageListExcel(m_saveDir);
            m_pageList.clear();
        }
    }

    // 타이틀 추출
    QString title;
    QRegularExpression titleRe("<title[^>]*>(.*?)</title>",
        QRegularExpression::CaseInsensitiveOption | QRegularExpression::DotMatchesEverythingOption);
    auto titleMatch = titleRe.match(html);
    if (titleMatch.hasMatch()) {
        title = titleMatch.captured(1).trimmed();
        title.replace("&amp;", "&").replace("&lt;", "<").replace("&gt;", ">").replace("&quot;", "\"");
    }

    m_backend->log(QString("[%1] %2 — %3").arg(m_pageCount).arg(title.left(50)).arg(url), "info", "crawl");

    // <base href=...> 가 있으면 상대 URL 기준점을 그 값으로 치환 — HTML 스펙상 필수
    QUrl baseUrl(url);
    {
        static const QRegularExpression baseRe(
            "<base\\b[^>]*\\bhref\\s*=\\s*[\"']([^\"']+)[\"']",
            QRegularExpression::CaseInsensitiveOption);
        auto baseMatch = baseRe.match(html);
        if (baseMatch.hasMatch()) {
            QString baseHref = decodeHtmlEntities(baseMatch.captured(1)).trimmed();
            if (!baseHref.isEmpty()) {
                QUrl resolved = baseUrl.resolved(QUrl(baseHref));
                if (resolved.isValid() && !resolved.host().isEmpty())
                    baseUrl = resolved;
            }
        }
    }

    // 1. 모든 리소스 다운로드
    QSet<QString> resources = extractAllResources(html, baseUrl);
    for (const QString &resUrl : resources) {
        if (!m_running) break;
        if (m_urlToLocal.contains(resUrl)) continue;
        downloadAndMap(resUrl);
    }

    // 2. 현재 페이지 저장 경로 먼저 결정 (rewriteHtml에 넘기기 위해)
    QString pagePath = urlToLocalPath(url);
    if (!pagePath.endsWith(".html") && !pagePath.endsWith(".htm")) {
        if (pagePath.endsWith("/") || !pagePath.contains("."))
            pagePath += "/index.html";
    }

    // 3. HTML 내 URL을 "현재 페이지 기준 상대경로"로 변환
    QString rewrittenHtml = rewriteHtml(html, baseUrl, pagePath);

    // 4. HTML 저장
    QString fullPath = m_saveDir + "/" + pagePath;
    QDir().mkpath(QFileInfo(fullPath).absolutePath());
    QFile f(fullPath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(rewrittenHtml.toUtf8());
        f.close();
#ifdef Q_OS_MACOS
        QProcess::execute("xattr", {"-w", "com.apple.metadata:kMDItemDownloadedDate",
            QString("(\"%1\")").arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate)), fullPath});
        QProcess::execute("xattr", {"-w", "com.apple.metadata:kMDItemWhereFroms",
            url, fullPath});
#endif
    }

    m_urlToLocal[url] = pagePath;

    QJsonObject pageInfo;
    pageInfo["url"] = url;
    pageInfo["title"] = title;
    pageInfo["depth"] = depth;
    pageInfo["local_path"] = pagePath;
    pageInfo["resources"] = static_cast<int>(resources.size());
    m_pageList.append(pageInfo);

    // BFS: 링크 추출 후 큐에 추가
    if (depth < m_maxDepth) {
        QSet<QString> links = extractLinks(html, baseUrl);
        for (const QString &link : links) {
            if (m_pageCount >= m_maxPages) break;
            if (m_visited.contains(link)) continue;
            if (!shouldCrawl(link)) continue;
            m_visited.insert(link);
            m_queue.enqueue({link, depth + 1});
        }
    }

    int ms = static_cast<int>(m_delay * 1000);
    QTimer::singleShot(ms, this, &SiteCrawler::processNextInQueue);
}

// ── 모든 리소스 URL 추출 ──

QSet<QString> SiteCrawler::extractAllResources(const QString &html, const QUrl &baseUrl)
{
    QSet<QString> urls;

    QRegularExpression attrRe(
        "(?:src|href|data-src|data-original|data-lazy-src|data-href|data-url|data-link|data-to|poster|background)"
        "\\s*=\\s*[\"']([^\"'\\s,]+)[\"']",
        QRegularExpression::CaseInsensitiveOption);
    auto it = attrRe.globalMatch(html);
    while (it.hasNext()) {
        QString val = it.next().captured(1).trimmed();
        QString norm = normalizeUrl(val, baseUrl);
        if (!norm.isEmpty()) urls.insert(norm);
    }

    QRegularExpression srcsetRe("srcset\\s*=\\s*[\"']([^\"']+)[\"']",
        QRegularExpression::CaseInsensitiveOption);
    it = srcsetRe.globalMatch(html);
    while (it.hasNext()) {
        QString srcset = it.next().captured(1);
        for (const QString &part : srcset.split(",", Qt::SkipEmptyParts)) {
            QString src = part.trimmed().split(" ").first().trimmed();
            QString norm = normalizeUrl(src, baseUrl);
            if (!norm.isEmpty()) urls.insert(norm);
        }
    }

    QRegularExpression urlRe("url\\([\"']?([^\"')]+)[\"']?\\)",
        QRegularExpression::CaseInsensitiveOption);
    it = urlRe.globalMatch(html);
    while (it.hasNext()) {
        QString val = it.next().captured(1).trimmed();
        if (val.startsWith("data:")) continue;
        QString norm = normalizeUrl(val, baseUrl);
        if (!norm.isEmpty()) urls.insert(norm);
    }

    return urls;
}

// ── 리소스 다운로드 + 로컬 경로 매핑 ──

QString SiteCrawler::downloadAndMap(const QString &url)
{
    if (m_urlToLocal.contains(url)) return m_urlToLocal[url];

    QString localPath = urlToLocalPath(url);
    QString fullPath = m_saveDir + "/" + localPath;

    if (QFile::exists(fullPath)) {
        m_urlToLocal[url] = localPath;
        return localPath;
    }

    QDir().mkpath(QFileInfo(fullPath).absolutePath());

    auto dlResult = m_http->downloadFileEx(url, fullPath);
    if (!dlResult.success) {
        m_urlToLocal[url] = url;
        return url;
    }

    // EXIF 메타데이터 기록 (이미지 파일만)
    if (m_saveExif) {
        QString lower = fullPath.toLower();
        if (lower.endsWith(".jpg") || lower.endsWith(".jpeg") ||
            lower.endsWith(".png") || lower.endsWith(".webp") ||
            lower.endsWith(".tiff") || lower.endsWith(".tif")) {
            Common::addExifMetadata(fullPath, m_baseDomain, QString(),
                QString("Crawl: %1").arg(m_baseDomain),
                url, QString());
        }
    }

    m_resourceCount++;
    m_backend->updateStats(m_pageCount, m_resourceCount, "크롤링 중", "crawl");

    // ── 보안 스캔 ──
    if (m_securityScan) {
        auto typeResult = ContentSecurityScanner::scanFileType(fullPath, dlResult.headBytes);
        if (typeResult.severity == ScanResult::Dangerous) {
            m_backend->log("격리: " + typeResult.findings.join(", ") + " — " + url, "error", "crawl");
            ContentSecurityScanner::quarantineFile(fullPath, m_saveDir + "/_quarantine");
            m_quarantineCount++;
            m_urlToLocal[url] = "_quarantine/blocked.html";

            QJsonObject finding;
            finding["url"] = url;
            finding["severity"] = "dangerous";
            finding["findings"] = QJsonArray::fromStringList(typeResult.findings);
            finding["action"] = "quarantined";
            m_securityFindings.append(finding);
            return m_urlToLocal[url];
        }

        if (localPath.endsWith(".js")) {
            QFile f(fullPath);
            if (f.open(QIODevice::ReadOnly)) {
                QByteArray content = f.readAll();
                f.close();

                auto jsResult = ContentSecurityScanner::scanJavaScript(content, url);
                m_scanCount++;

                if (jsResult.severity == ScanResult::Dangerous) {
                    m_backend->log("JS 격리: " + jsResult.findings.join(", ") + " — " + url, "error", "crawl");
                    ContentSecurityScanner::quarantineFile(fullPath, m_saveDir + "/_quarantine");
                    m_quarantineCount++;
                    m_urlToLocal[url] = "_quarantine/blocked.html";

                    QJsonObject finding;
                    finding["url"] = url;
                    finding["severity"] = "dangerous";
                    finding["findings"] = QJsonArray::fromStringList(jsResult.findings);
                    finding["action"] = "quarantined";
                    m_securityFindings.append(finding);
                    return m_urlToLocal[url];

                } else if (jsResult.severity == ScanResult::Warning) {
                    m_backend->log("JS 정화: " + jsResult.findings.join(", "), "warning", "crawl");
                    QByteArray sanitized = ContentSecurityScanner::sanitizeJavaScript(content);
                    QFile out(fullPath);
                    if (out.open(QIODevice::WriteOnly)) { out.write(sanitized); out.close(); }

                    QJsonObject finding;
                    finding["url"] = url;
                    finding["severity"] = "warning";
                    finding["findings"] = QJsonArray::fromStringList(jsResult.findings);
                    finding["action"] = "sanitized";
                    m_securityFindings.append(finding);
                }

                if (m_sandboxJs) {
                    QFile sf(fullPath);
                    if (sf.open(QIODevice::ReadOnly)) {
                        QByteArray current = sf.readAll();
                        sf.close();
                        if (sf.open(QIODevice::WriteOnly)) {
                            sf.write(ContentSecurityScanner::jsSandboxHeader());
                            sf.write(current);
                            sf.close();
                        }
                    }
                }
            }
        }
    }

    // CSS 재귀 처리
    if (localPath.endsWith(".css") && !m_processedCss.contains(url)) {
        processCssFile(fullPath, QUrl(url));
    }

    m_urlToLocal[url] = localPath;
    return localPath;
}

// ── URL → 로컬 상대 경로 변환 ──

QString SiteCrawler::urlToLocalPath(const QString &url)
{
    QUrl parsed(url);
    QString path = parsed.path();
    if (path.isEmpty() || path == "/") path = "/index.html";

    QString lower = path.toLower();
    QString subdir;
    if (lower.endsWith(".css")) subdir = "css";
    else if (lower.endsWith(".js")) subdir = "js";
    else if (lower.endsWith(".woff") || lower.endsWith(".woff2") || lower.endsWith(".ttf") ||
             lower.endsWith(".eot") || lower.endsWith(".otf")) subdir = "fonts";
    else if (lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".png") ||
             lower.endsWith(".gif") || lower.endsWith(".webp") || lower.endsWith(".svg") ||
             lower.endsWith(".ico") || lower.endsWith(".avif") || lower.endsWith(".bmp")) subdir = "images";
    else if (lower.endsWith(".mp4") || lower.endsWith(".webm") || lower.endsWith(".mp3") ||
             lower.endsWith(".ogg") || lower.endsWith(".wav") || lower.endsWith(".m4a")) subdir = "media";

    QString filename = QFileInfo(path).fileName();
    if (filename.isEmpty()) {
        filename = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5).toHex().left(16);
        if (lower.contains(".css")) filename += ".css";
        else if (lower.contains(".js")) filename += ".js";
        else filename += ".html";
    }

    filename.replace(QRegularExpression("[?#].*"), "");
    filename.replace(QRegularExpression("[^a-zA-Z0-9._-]"), "_");
    if (filename.length() > 200) filename = filename.left(190) + "." + QFileInfo(filename).suffix();

    QString localPath = subdir.isEmpty() ? filename : (subdir + "/" + filename);
    QString fullCheck = m_saveDir + "/" + localPath;
    if (QFile::exists(fullCheck) && !m_urlToLocal.contains(url)) {
        QString hash = QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5).toHex().left(8);
        QString base = QFileInfo(filename).baseName();
        QString ext = QFileInfo(filename).suffix();
        filename = base + "_" + hash + (ext.isEmpty() ? "" : "." + ext);
        localPath = subdir.isEmpty() ? filename : (subdir + "/" + filename);
    }

    return localPath;
}

// ── HTML 내 URL을 로컬 경로로 치환 ──

QString SiteCrawler::ensureLocalPath(const QString &url)
{
    if (url.isEmpty()) return QString();

    // 이미 매핑된 URL
    if (m_urlToLocal.contains(url)) {
        const QString &p = m_urlToLocal[url];
        // p가 url 그대로이면 외부(쿠킷 불가능) 리소스로 기록된 것 → 빈 문자열
        return (p == url) ? QString() : p;
    }

    // 같은 도메인이 아니면 외부 URL로 기록하고 건너뜀 (원본 유지)
    QUrl parsed(url);
    if (m_sameDomainOnly && !isSameDomain(parsed.host())) {
        m_urlToLocal[url] = url;  // external marker
        return QString();
    }

    // 같은 도메인의 "리소스"는 즉시 다운로드+매핑 (이미지/CSS/JS/폰트/미디어)
    // shouldCrawl()은 HTML 페이지만 크롤 대상으로 봄. false이면 리소스로 간주.
    if (!shouldCrawl(url)) {
        QString local = downloadAndMap(url);
        // downloadAndMap 내부에서 실패 시 m_urlToLocal[url]=url로 기록 → 외부 취급
        if (local.isEmpty() || local == url) return QString();
        return local;
    }

    // 크롤 대상 HTML — 미래에 처리될 로컬 경로를 지금 예약
    QString local = urlToLocalPath(url);
    m_urlToLocal[url] = local;  // 예약 (processRenderedHtml 진입 시 같은 경로로 재계산됨)
    return local;
}

QString SiteCrawler::rewriteHtml(const QString &html, const QUrl &baseUrl, const QString &currentPageLocal)
{
    QString result = html;

    // src/href/data-*/action 속성 (SPA 링크용 data-href/data-url/data-link/data-to 포함)
    QRegularExpression attrRe(
        "((?:src|href|data-src|data-original|data-lazy-src|data-href|data-url|data-link|data-to|poster|action|background|cite|longdesc|formaction|xlink:href)\\s*=\\s*[\"'])([^\"']+)([\"'])",
        QRegularExpression::CaseInsensitiveOption);

    QList<QRegularExpressionMatch> matches;
    auto it = attrRe.globalMatch(result);
    while (it.hasNext()) matches.append(it.next());

    for (int i = matches.size() - 1; i >= 0; --i) {
        const auto &match = matches[i];
        QString val = match.captured(2).trimmed();
        // 앵커/자바스크립트/메일/전화/데이터 URL은 건너뜀 — normalizeUrl이 비어있는 값 반환
        QString norm = normalizeUrl(val, baseUrl);
        if (norm.isEmpty()) continue;

        QString localPath = ensureLocalPath(norm);
        if (localPath.isEmpty()) continue;  // 외부 URL은 원본 유지

        // 현재 페이지 위치 기준 상대경로로 변환 — 깊이 다른 하위 페이지 간 링크가 깨지지 않도록
        QString rel = toPageRelativePath(currentPageLocal, localPath);
        QString replacement = match.captured(1) + rel + match.captured(3);
        result.replace(match.capturedStart(), match.capturedLength(), replacement);
    }

    // <a href> 중 앵커만 있는 경우 (같은 페이지 내 #section) 는 normalizeUrl이 걸러냄
    // → 유지됨. 정상.

    // srcset 속성 (반응형 이미지) — 콤마로 구분된 여러 URL
    QRegularExpression srcsetRe(
        "(srcset\\s*=\\s*[\"'])([^\"']+)([\"'])",
        QRegularExpression::CaseInsensitiveOption);
    matches.clear();
    it = srcsetRe.globalMatch(result);
    while (it.hasNext()) matches.append(it.next());
    for (int i = matches.size() - 1; i >= 0; --i) {
        const auto &match = matches[i];
        QString val = match.captured(2);
        QStringList parts = val.split(',');
        bool changed = false;
        for (QString &part : parts) {
            QString trimmed = part.trimmed();
            int sp = trimmed.indexOf(' ');
            QString url = (sp > 0) ? trimmed.left(sp) : trimmed;
            QString desc = (sp > 0) ? trimmed.mid(sp) : QString();
            QString norm = normalizeUrl(url, baseUrl);
            if (norm.isEmpty()) continue;
            QString localPath = ensureLocalPath(norm);
            if (localPath.isEmpty()) continue;
            QString rel = toPageRelativePath(currentPageLocal, localPath);
            part = " " + rel + desc;
            changed = true;
        }
        if (changed) {
            QString replacement = match.captured(1) + parts.join(",").trimmed() + match.captured(3);
            result.replace(match.capturedStart(), match.capturedLength(), replacement);
        }
    }

    // url(...) — CSS inline (style 속성, <style> 블록)
    QRegularExpression urlRe("(url\\([\"']?)([^\"')]+)([\"']?\\))",
        QRegularExpression::CaseInsensitiveOption);
    matches.clear();
    it = urlRe.globalMatch(result);
    while (it.hasNext()) matches.append(it.next());

    for (int i = matches.size() - 1; i >= 0; --i) {
        const auto &match = matches[i];
        QString val = match.captured(2).trimmed();
        if (val.startsWith("data:")) continue;
        QString norm = normalizeUrl(val, baseUrl);
        if (norm.isEmpty()) continue;

        QString localPath = ensureLocalPath(norm);
        if (localPath.isEmpty()) continue;

        QString rel = toPageRelativePath(currentPageLocal, localPath);
        QString replacement = match.captured(1) + rel + match.captured(3);
        result.replace(match.capturedStart(), match.capturedLength(), replacement);
    }

    // <meta http-equiv="refresh" content="0; url=..."> 도 치환 (정부 사이트 리다이렉트용)
    QRegularExpression metaRe(
        "(<meta[^>]+http-equiv\\s*=\\s*[\"']refresh[\"'][^>]+content\\s*=\\s*[\"'][^\"']*url=)([^\"']+)([\"'])",
        QRegularExpression::CaseInsensitiveOption);
    matches.clear();
    it = metaRe.globalMatch(result);
    while (it.hasNext()) matches.append(it.next());
    for (int i = matches.size() - 1; i >= 0; --i) {
        const auto &match = matches[i];
        QString val = match.captured(2).trimmed();
        QString norm = normalizeUrl(val, baseUrl);
        if (norm.isEmpty()) continue;
        QString localPath = ensureLocalPath(norm);
        if (localPath.isEmpty()) continue;
        QString rel = toPageRelativePath(currentPageLocal, localPath);
        QString replacement = match.captured(1) + rel + match.captured(3);
        result.replace(match.capturedStart(), match.capturedLength(), replacement);
    }

    if (m_securityScan) {
        result = ContentSecurityScanner::injectCSP(result);
    }

    return result;
}

// 저장된 CSS 파일의 url()/@import 참조를 최신 m_urlToLocal 기준으로 재치환.
// processCssFile 는 다운로드 시점에 한번만 실행되므로, 그 뒤에 추가로 매핑된
// 리소스(예: 같은 url() 이 여러 CSS에 걸쳐 있는 경우)는 놓칠 수 있다.
void SiteCrawler::postProcessCssFile(const QString &cssFilePath, const QString &cssUrl)
{
    QFile f(cssFilePath);
    if (!f.exists() || !f.open(QIODevice::ReadOnly)) return;
    QString css = QString::fromUtf8(f.readAll());
    f.close();

    QString cssRel = cssFilePath;
    if (cssRel.startsWith(m_saveDir + "/")) cssRel = cssRel.mid(m_saveDir.length() + 1);

    QUrl cssBase(cssUrl);
    bool modified = false;

    QRegularExpression urlRe(R"(url\s*\(\s*["']?([^"')]+)["']?\s*\))",
        QRegularExpression::CaseInsensitiveOption);
    QList<QRegularExpressionMatch> matches;
    auto it = urlRe.globalMatch(css);
    while (it.hasNext()) matches.append(it.next());

    for (int i = matches.size() - 1; i >= 0; --i) {
        QString val = matches[i].captured(1).trimmed();
        if (val.startsWith("data:")) continue;
        // 이미 상대경로면 스킵 (../foo, ./foo, foo/bar 형태)
        if (!val.startsWith("http://") && !val.startsWith("https://") && !val.startsWith("//")) continue;
        QString resUrl = normalizeUrl(val, cssBase);
        if (resUrl.isEmpty()) continue;
        if (!m_urlToLocal.contains(resUrl) || m_urlToLocal[resUrl] == resUrl) continue;
        QString rel = toPageRelativePath(cssRel, m_urlToLocal[resUrl]);
        css.replace(matches[i].capturedStart(1), matches[i].capturedLength(1), rel);
        modified = true;
    }

    if (modified) {
        QFile out(cssFilePath);
        if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            out.write(css.toUtf8());
            out.close();
        }
    }
}

// 크롤 완료 후, 저장된 HTML들을 다시 순회하며 남은 절대 URL을 로컬 경로로 치환.
// 크롤 중에 예약되지 않은(=나중에 발견된) 링크가 있을 수 있어 마지막 정리 단계 필요.
void SiteCrawler::postProcessRewrite()
{
    int fixedHtml = 0;
    int fixedCss = 0;
    for (auto it = m_urlToLocal.constBegin(); it != m_urlToLocal.constEnd(); ++it) {
        const QString &pageUrl = it.key();
        const QString &pagePath = it.value();
        // 외부로 표시된 항목은 건너뜀
        if (pagePath == pageUrl) continue;
        QString fullPath = m_saveDir + "/" + pagePath;

        if (pagePath.endsWith(".html", Qt::CaseInsensitive) || pagePath.endsWith(".htm", Qt::CaseInsensitive)) {
            QFile f(fullPath);
            if (!f.exists() || !f.open(QIODevice::ReadOnly)) continue;
            QString html = QString::fromUtf8(f.readAll());
            f.close();

            QUrl baseUrl(pageUrl);
            QString rewritten = rewriteHtml(html, baseUrl, pagePath);
            if (rewritten != html) {
                QFile out(fullPath);
                if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                    out.write(rewritten.toUtf8());
                    out.close();
                    fixedHtml++;
                }
            }
        } else if (pagePath.endsWith(".css", Qt::CaseInsensitive)) {
            qint64 before = QFileInfo(fullPath).size();
            postProcessCssFile(fullPath, pageUrl);
            qint64 after = QFileInfo(fullPath).size();
            if (before != after) fixedCss++;
        }
    }
    if (fixedHtml > 0 || fixedCss > 0) {
        m_backend->log(QString("후처리: HTML %1개, CSS %2개 링크 재교정").arg(fixedHtml).arg(fixedCss), "info", "crawl");
    }
}

// ── 링크 추출 (BFS용) ──

QSet<QString> SiteCrawler::extractLinks(const QString &html, const QUrl &baseUrl)
{
    QSet<QString> links;

    QRegularExpression re("href\\s*=\\s*[\"']([^\"'#]+)[\"']",
        QRegularExpression::CaseInsensitiveOption);
    auto it = re.globalMatch(html);
    while (it.hasNext()) {
        QString href = it.next().captured(1).trimmed();
        QString normalized = normalizeUrl(href, baseUrl);
        if (!normalized.isEmpty()) links.insert(normalized);
    }

    QRegularExpression dataRe("data-(?:href|url|link|to)\\s*=\\s*[\"']([^\"'#]+)[\"']",
        QRegularExpression::CaseInsensitiveOption);
    it = dataRe.globalMatch(html);
    while (it.hasNext()) {
        QString href = it.next().captured(1).trimmed();
        QString normalized = normalizeUrl(href, baseUrl);
        if (!normalized.isEmpty()) links.insert(normalized);
    }

    return links;
}

// ── URL 헬퍼 ──

QString SiteCrawler::decodeHtmlEntities(const QString &s)
{
    if (!s.contains('&')) return s;
    QString r = s;
    // 숫자 형태 (&#123; &#x1F; 등)
    static const QRegularExpression numRe("&#(x[0-9a-fA-F]+|[0-9]+);");
    auto it = numRe.globalMatch(r);
    QList<QRegularExpressionMatch> matches;
    while (it.hasNext()) matches.append(it.next());
    for (int i = matches.size() - 1; i >= 0; --i) {
        QString cap = matches[i].captured(1);
        bool ok = false;
        int code = cap.startsWith('x') || cap.startsWith('X')
            ? cap.mid(1).toInt(&ok, 16)
            : cap.toInt(&ok, 10);
        if (ok) r.replace(matches[i].capturedStart(), matches[i].capturedLength(), QChar(code));
    }
    // 명명된 엔티티 — URL에 자주 나오는 것만
    r.replace("&amp;", "&")
     .replace("&lt;", "<")
     .replace("&gt;", ">")
     .replace("&quot;", "\"")
     .replace("&apos;", "'")
     .replace("&#39;", "'")
     .replace("&nbsp;", " ");
    return r;
}

QString SiteCrawler::normalizeUrl(const QString &href, const QUrl &baseUrl)
{
    if (href.isEmpty() || href.startsWith("#") || href.startsWith("javascript:") ||
        href.startsWith("mailto:") || href.startsWith("tel:") || href.startsWith("data:") ||
        href.startsWith("blob:"))
        return QString();

    // HTML entities 디코딩 — 정부 사이트는 &amp; 가 많음
    QString decoded = decodeHtmlEntities(href).trimmed();
    if (decoded.isEmpty()) return QString();

    QUrl resolved = baseUrl.resolved(QUrl(decoded));
    resolved.setFragment(QString());

    QString url = resolved.toString();
    if (!url.startsWith("http://") && !url.startsWith("https://"))
        return QString();

    return url;
}

// root 기준 로컬경로(예: "sub/inner.html")를 fromPage의 디렉토리 기준 상대경로로 변환
// fromPage="sub/inner.html", toPath="page.html" → "../page.html"
// fromPage="page.html"(루트), toPath="css/foo.css" → "css/foo.css"
QString SiteCrawler::toPageRelativePath(const QString &fromPage, const QString &toPath)
{
    if (toPath.isEmpty()) return toPath;
    QString fromDir = QFileInfo(fromPage).path();
    if (fromDir.isEmpty() || fromDir == ".") return toPath;  // fromPage가 루트에 있음
    // QDir::relativeFilePath 는 절대경로 입력에서 제대로 동작하므로 "/" prefix 붙여서 계산
    QDir d("/" + fromDir);
    QString rel = d.relativeFilePath("/" + toPath);
    return rel;
}

// ── CSS 재귀 처리 ──

void SiteCrawler::processCssFile(const QString &cssFilePath, const QUrl &cssBaseUrl)
{
    m_processedCss.insert(cssBaseUrl.toString());

    QFile f(cssFilePath);
    if (!f.open(QIODevice::ReadOnly)) return;
    QString css = QString::fromUtf8(f.readAll());
    f.close();

    // CSS 파일의 root-relative 경로 계산 (상대경로 기준점)
    QString cssRelativePath = cssFilePath;
    if (cssRelativePath.startsWith(m_saveDir + "/")) {
        cssRelativePath = cssRelativePath.mid(m_saveDir.length() + 1);
    }

    bool modified = false;

    QRegularExpression importRe(R"(@import\s+(?:url\s*\()?\s*["']([^"']+)["']\s*\)?\s*;)",
        QRegularExpression::CaseInsensitiveOption);
    auto it = importRe.globalMatch(css);
    QList<QRegularExpressionMatch> matches;
    while (it.hasNext()) matches.append(it.next());

    for (int i = matches.size() - 1; i >= 0; --i) {
        QString importUrl = normalizeUrl(matches[i].captured(1), cssBaseUrl);
        if (importUrl.isEmpty()) continue;
        QString localPath = downloadAndMap(importUrl);
        if (localPath != importUrl) {
            // CSS 파일 위치 기준 상대경로로 변환
            QString rel = toPageRelativePath(cssRelativePath, localPath);
            css.replace(matches[i].capturedStart(1), matches[i].capturedLength(1), rel);
            modified = true;
        }
    }

    QRegularExpression urlRe(R"(url\s*\(\s*["']?([^"')]+)["']?\s*\))",
        QRegularExpression::CaseInsensitiveOption);
    matches.clear();
    it = urlRe.globalMatch(css);
    while (it.hasNext()) matches.append(it.next());

    for (int i = matches.size() - 1; i >= 0; --i) {
        QString val = matches[i].captured(1).trimmed();
        if (val.startsWith("data:")) continue;
        QString resUrl = normalizeUrl(val, cssBaseUrl);
        if (resUrl.isEmpty()) continue;
        if (!m_urlToLocal.contains(resUrl)) downloadAndMap(resUrl);
        if (m_urlToLocal.contains(resUrl) && m_urlToLocal[resUrl] != resUrl) {
            // CSS 파일 위치 기준 상대경로로 변환 (url(../images/foo.png) 처럼)
            QString rel = toPageRelativePath(cssRelativePath, m_urlToLocal[resUrl]);
            css.replace(matches[i].capturedStart(1), matches[i].capturedLength(1), rel);
            modified = true;
        }
    }

    if (modified) {
        QFile out(cssFilePath);
        if (out.open(QIODevice::WriteOnly)) {
            out.write(css.toUtf8());
            out.close();
        }
    }
}

// ── Deep Crawl: SPA 링크 추출 ──

void SiteCrawler::extractDynamicLinks(std::function<void(const QSet<QString>&)> callback)
{
    QString js = R"JS(
    (function() {
        var links = [];
        document.querySelectorAll('a[href], [data-href], [data-url], [data-link]').forEach(function(el) {
            var href = el.getAttribute('href') || el.dataset.href || el.dataset.url || el.dataset.link;
            if (href) links.push(href);
        });
        document.querySelectorAll('[onclick]').forEach(function(el) {
            var m = el.getAttribute('onclick').match(/['"]((?:https?:\/\/|\/)[^'"]+)['"]/g);
            if (m) m.forEach(function(u) { links.push(u.replace(/['"]/g, '')); });
        });
        document.querySelectorAll('iframe[src]').forEach(function(el) {
            links.push(el.getAttribute('src'));
        });
        return JSON.stringify(links);
    })()
    )JS";

    m_page->runJavaScript(js, [this, callback](const QVariant &result) {
        QSet<QString> links;
        QJsonArray arr = QJsonDocument::fromJson(result.toString().toUtf8()).array();
        QUrl baseUrl(m_current.url);
        for (const auto &v : arr) {
            QString norm = normalizeUrl(v.toString(), baseUrl);
            if (!norm.isEmpty()) links.insert(norm);
        }
        callback(links);
    });
}

// ── Deep Scroll (무한 스크롤 지원) ──

void SiteCrawler::deepScrollAndExtract(int maxScrolls, int scrollWaitMs,
                                        std::function<void(const QString &)> callback)
{
    // x.com (Twitter) / 가상 리스트 사이트는 scrollHeight가 안정적이지 않다.
    // → tweet article 개수 + scrollHeight 둘 다 체크해서 둘 다 변동 없을 때만 종료.
    //
    // 또한 트위터는 스크롤로 옛 트윗이 unmount되므로, 매 스크롤마다 현재 보이는
    // article들의 데이터 (tweet ID, 텍스트, 미디어 URL)를 버퍼에 누적시킨다.
    QString probeJs = R"JS(
    (function(){
        var h = document.body.scrollHeight;
        var articles = document.querySelectorAll('article[data-testid="tweet"], article[role="article"]').length;

        // 트위터: 현재 화면의 트윗 데이터를 누적 버퍼에 추가
        if (location.hostname === 'x.com' || location.hostname === 'twitter.com' ||
            location.hostname.endsWith('.x.com') || location.hostname.endsWith('.twitter.com')) {
            if (!window.__crawl_tweet_buffer) window.__crawl_tweet_buffer = {};
            document.querySelectorAll('article[data-testid="tweet"]').forEach(function(art){
                try {
                    var tweetLink = art.querySelector('a[href*="/status/"]');
                    var href = tweetLink ? tweetLink.href : '';
                    var idMatch = href.match(/\/status\/(\d+)/);
                    if (!idMatch) return;
                    var tid = idMatch[1];
                    if (window.__crawl_tweet_buffer[tid]) return;

                    var textEl = art.querySelector('[data-testid="tweetText"]');
                    var text = textEl ? textEl.textContent : '';
                    var timeEl = art.querySelector('time');
                    var dt = timeEl ? timeEl.getAttribute('datetime') : '';
                    var authorA = art.querySelector('[data-testid="User-Name"] a');
                    var author = authorA ? authorA.textContent : '';
                    var imgs = [];
                    art.querySelectorAll('img[src*="pbs.twimg.com/media"]').forEach(function(im){
                        // 풀 화질로 업그레이드
                        var u = im.src
                            .replace(/&name=\w+/, '&name=orig')
                            .replace(/\?name=\w+/, '?name=orig')
                            .replace(/:(small|medium|large|thumb|360x360|600x600)$/, ':orig');
                        imgs.push(u);
                    });
                    var vids = [];
                    art.querySelectorAll('video source, video[src]').forEach(function(v){
                        if (v.src) vids.push(v.src);
                    });
                    window.__crawl_tweet_buffer[tid] = {
                        id: tid, url: href, author: author, text: text,
                        datetime: dt, images: imgs, videos: vids,
                        outerHtml: art.outerHTML.slice(0, 50000)  // 50KB 캡
                    };
                } catch(e) {}
            });
        }

        return JSON.stringify({h: h, articles: articles});
    })();
    )JS";

    m_page->runJavaScript(probeJs, [=](const QVariant &probeVar) {
        QJsonObject probe = QJsonDocument::fromJson(probeVar.toString().toUtf8()).object();
        int prevHeight = probe["h"].toInt();
        int prevArticles = probe["articles"].toInt();

        m_page->runJavaScript("window.scrollTo(0, document.body.scrollHeight)");

        QTimer::singleShot(scrollWaitMs, this, [=]() {
            if (!m_running) return;
            m_page->runJavaScript(probeJs, [=](const QVariant &newProbeVar) {
                QJsonObject newProbe = QJsonDocument::fromJson(newProbeVar.toString().toUtf8()).object();
                int newHeight = newProbe["h"].toInt();
                int newArticles = newProbe["articles"].toInt();
                bool grew = (newHeight > prevHeight) || (newArticles > prevArticles);
                if (grew && maxScrolls > 1) {
                    deepScrollAndExtract(maxScrolls - 1, scrollWaitMs, callback);
                } else {
                    m_page->toHtml(callback);
                }
            });
        });
    });
}

// ── API 호출 모니터링 ──

void SiteCrawler::injectApiMonitor()
{
    // ── 강화된 API 모니터: URL뿐 아니라 응답 BODY까지 캡처 ──
    // x.com / Twitter / 모든 GraphQL SPA는 JSON 응답에 모든 데이터가 들어있다.
    // 응답 본문을 잡아 디스크에 저장하면 DOM 파싱보다 훨씬 안정적이다.
    //
    // 메모리 보호: __crawl_api_responses는 전역 큐로 두고, 메인 측에서
    // collectApiCalls 호출할 때 비운다. 응답이 커서 1MB 넘으면 head만 저장.
    QWebEngineScript script;
    script.setName("abiwa_api_monitor");
    script.setInjectionPoint(QWebEngineScript::DocumentCreation);
    script.setWorldId(QWebEngineScript::MainWorld);
    script.setSourceCode(R"JS(
    (function() {
        if (window.__crawl_installed) return;
        window.__crawl_installed = true;
        window.__crawl_api_calls = [];
        window.__crawl_api_responses = [];     // {url, method, status, body, contentType, ts}
        window.__crawl_max_body = 2 * 1024 * 1024;   // 응답 본문 2MB까지만 캡처

        function recordResp(url, method, status, contentType, body) {
            try {
                if (typeof body !== 'string') return;
                if (body.length > window.__crawl_max_body) body = body.slice(0, window.__crawl_max_body);
                // JSON 응답만 캡처 (HTML/이미지는 skip — 너무 커짐)
                var ct = (contentType || '').toLowerCase();
                var looksJson = ct.indexOf('json') >= 0 || (body.length > 0 && (body[0] === '{' || body[0] === '['));
                if (!looksJson) return;
                window.__crawl_api_responses.push({
                    url: url, method: method, status: status,
                    contentType: ct, body: body, ts: Date.now()
                });
            } catch(e) {}
        }

        // XHR hook
        var XHR = XMLHttpRequest.prototype;
        var origOpen = XHR.open, origSend = XHR.send;
        XHR.open = function(method, url) {
            try {
                this.__crawl_method = method;
                this.__crawl_url = (typeof url === 'string') ? url : url.toString();
                window.__crawl_api_calls.push({method: method, url: this.__crawl_url, type: 'xhr'});
            } catch(e) {}
            return origOpen.apply(this, arguments);
        };
        XHR.send = function() {
            var self = this;
            this.addEventListener('load', function() {
                try {
                    var ct = self.getResponseHeader('content-type') || '';
                    recordResp(self.__crawl_url, self.__crawl_method, self.status, ct, self.responseText);
                } catch(e) {}
            });
            return origSend.apply(this, arguments);
        };

        // fetch hook
        if (window.fetch) {
            var origFetch = window.fetch;
            window.fetch = function(input, init) {
                var url = (typeof input === 'string') ? input : (input && input.url ? input.url : '');
                var method = (init && init.method) ? init.method : ((input && input.method) ? input.method : 'GET');
                window.__crawl_api_calls.push({method: method, url: url, type: 'fetch'});
                return origFetch.apply(this, arguments).then(function(resp) {
                    try {
                        var ct = resp.headers && resp.headers.get ? (resp.headers.get('content-type') || '') : '';
                        if (ct.toLowerCase().indexOf('json') >= 0 || ct === '') {
                            // clone — 원본 stream은 그대로 둠
                            resp.clone().text().then(function(body) {
                                recordResp(url, method, resp.status, ct, body);
                            }).catch(function(){});
                        }
                    } catch(e) {}
                    return resp;
                });
            };
        }
    })();
    )JS");
    m_page->scripts().insert(script);
}

void SiteCrawler::collectApiCalls(std::function<void(const QJsonArray &)> callback)
{
    // URL 목록 + 응답 본문 큐를 한꺼번에 가져와서 비운다 (메모리 보호)
    QString js =
        "(function(){"
        "  var calls = window.__crawl_api_calls || [];"
        "  var resps = window.__crawl_api_responses || [];"
        "  window.__crawl_api_calls = [];"
        "  window.__crawl_api_responses = [];"
        "  return JSON.stringify({calls: calls, responses: resps});"
        "})()";
    m_page->runJavaScript(js, [this, callback](const QVariant &result) {
        QJsonObject obj = QJsonDocument::fromJson(result.toString().toUtf8()).object();
        QJsonArray calls = obj["calls"].toArray();
        QJsonArray responses = obj["responses"].toArray();

        // 캡처된 JSON 응답을 디스크에 저장 (api/responses/{md5}.json)
        if (!responses.isEmpty()) {
            QString respDir = m_saveDir + "/api/responses";
            QDir().mkpath(respDir);
            int saved = 0;
            for (const auto &v : responses) {
                QJsonObject r = v.toObject();
                QString url = r["url"].toString();
                QString body = r["body"].toString();
                if (url.isEmpty() || body.isEmpty()) continue;
                QString hash = QCryptographicHash::hash(
                    (url + QString::number(r["ts"].toVariant().toLongLong())).toUtf8(),
                    QCryptographicHash::Md5).toHex().left(16);
                // x.com GraphQL은 URL에 endpoint 이름이 있어서 식별 쉬움
                QString endpoint;
                QRegularExpression gqlRe(R"(/graphql/[^/]+/(\w+))");
                auto gm = gqlRe.match(url);
                if (gm.hasMatch()) endpoint = gm.captured(1);
                QString fname = endpoint.isEmpty()
                    ? hash + ".json"
                    : endpoint + "_" + hash + ".json";
                QString path = respDir + "/" + fname;
                QFile f(path);
                if (f.open(QIODevice::WriteOnly)) {
                    QJsonObject wrapped;
                    wrapped["url"] = url;
                    wrapped["method"] = r["method"];
                    wrapped["status"] = r["status"];
                    wrapped["contentType"] = r["contentType"];
                    wrapped["capturedAt"] = QDateTime::fromMSecsSinceEpoch(r["ts"].toVariant().toLongLong())
                        .toString(Qt::ISODate);
                    QJsonDocument bodyDoc = QJsonDocument::fromJson(body.toUtf8());
                    if (bodyDoc.isObject() || bodyDoc.isArray()) {
                        wrapped["body"] = bodyDoc.isObject()
                            ? QJsonValue(bodyDoc.object())
                            : QJsonValue(bodyDoc.array());
                    } else {
                        wrapped["bodyRaw"] = body;
                    }
                    f.write(QJsonDocument(wrapped).toJson(QJsonDocument::Indented));
                    f.close();
                    saved++;
                }
            }
            if (saved > 0) {
                m_backend->log(QString("API 응답 캡처: %1건 저장").arg(saved), "info", "crawl");
            }
        }
        callback(calls);
    });
}

// ── 보안 리포트 저장 ──

void SiteCrawler::saveSecurityReport()
{
    if (m_securityFindings.isEmpty()) return;

    QJsonObject report;
    report["domain"] = m_baseDomain;
    report["date"] = QDateTime::currentDateTime().toString(Qt::ISODate);
    report["totalScanned"] = m_scanCount;
    report["quarantined"] = m_quarantineCount;
    report["findings"] = m_securityFindings;

    QString path = m_saveDir + "/security_report.json";
    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
        f.close();
    }
    m_backend->log(QString("보안 보고서: %1건 발견, %2건 격리").arg(m_securityFindings.size()).arg(m_quarantineCount), "warning", "crawl");
}

bool SiteCrawler::isSameDomain(const QString &host) const
{
    if (host.isEmpty() || m_baseDomain.isEmpty()) return false;
    if (host == m_baseDomain) return true;
    // host 가 base 의 서브도메인 (api.meti.go.jp ⊂ meti.go.jp)
    if (host.endsWith("." + m_baseDomain)) return true;
    // base 가 host 의 서브도메인 (base=www.meti.go.jp 기준 host=meti.go.jp 도 같은 사이트로 취급)
    if (m_baseDomain.endsWith("." + host)) return true;
    return false;
}

bool SiteCrawler::shouldCrawl(const QString &url)
{
    QUrl parsed(url);
    if (m_sameDomainOnly && !isSameDomain(parsed.host())) return false;

    QString path = parsed.path().toLower();
    static const QStringList skipExts = {
        ".jpg", ".jpeg", ".png", ".gif", ".webp", ".svg", ".bmp", ".ico", ".avif",
        ".mp4", ".webm", ".avi", ".mov", ".mkv",
        ".mp3", ".wav", ".ogg", ".flac",
        ".pdf", ".doc", ".docx", ".xls", ".xlsx",
        ".zip", ".rar", ".7z", ".tar", ".gz",
        ".css", ".js", ".woff", ".woff2", ".ttf", ".eot",
        ".map", ".json"
    };
    for (const auto &ext : skipExts) {
        if (path.endsWith(ext)) return false;
    }

    return true;
}

// ── 완료 ──

void SiteCrawler::finishCrawl()
{
    if (!m_running && m_pageCount == 0) {
        m_backend->log("크롤링 중단됨", "warning", "crawl");
        emit finished();
        return;
    }
    m_running = false;

    if (!m_pageList.isEmpty()) {
        savePageListExcel(m_saveDir);
    }

    // 후처리: 크롤 중 미리 예약되지 않은 링크(예: 마지막 페이지들 간 상호 참조)를
    // 최종 m_urlToLocal 맵으로 재교정 — 이게 핵심 수정.
    postProcessRewrite();

    generateMainIndex();
    generateDomainIndex();
    generateParentIndex();
    saveSecurityReport();

    m_backend->log(QString("크롤링 완료! 페이지: %1 | 리소스: %2 | 스캔: %3 | 격리: %4")
        .arg(m_pageCount).arg(m_resourceCount).arg(m_scanCount).arg(m_quarantineCount), "success", "crawl");
    m_backend->log("index.html을 열면 실제 사이트처럼 탐색 가능", "success", "crawl");
    m_backend->log("저장: " + m_saveDir, "success", "crawl");
    emit finished();
}

void SiteCrawler::generateMainIndex()
{
    QString firstPage;
    if (!m_pageList.isEmpty()) {
        firstPage = m_pageList.first()["local_path"].toString();
    }

    QString sidebarItems;
    for (const auto &page : m_pageList) {
        QString title = page["title"].toString();
        QString localPath = page["local_path"].toString();
        int depth = page["depth"].toInt();
        if (title.isEmpty()) title = page["url"].toString();
        QString depthClass = depth > 0 ? QString(" class=\"depth-%1\"").arg(depth) : "";
        sidebarItems += QString(
            "<a href=\"#\" onclick=\"loadPage('%1');return false;\"%2 title=\"%3\">"
            "<span class=\"d\">%4</span>%5</a>\n"
        ).arg(localPath, depthClass, page["url"].toString(),
              depth > 0 ? "└ " : "", title.left(60));
    }

    QString html = QString(R"HTML(<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="UTF-8">
<title>%1 — ABIWA Offline</title>
<style>
* { margin:0; padding:0; box-sizing:border-box; }
body { font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif; display:flex; height:100vh; background:#1a1a1a; color:#ccc; }
#sidebar {
    width:280px; min-width:200px; max-width:400px; background:#222; border-right:1px solid #333;
    display:flex; flex-direction:column; overflow:hidden; flex-shrink:0;
}
#sidebar-header { padding:12px; border-bottom:1px solid #333; }
#sidebar-header h2 { font-size:13px; color:#fff; margin-bottom:6px; }
#sidebar-header .info { font-size:10px; color:#888; }
#search { width:100%; padding:6px 8px; background:#2a2a2a; border:1px solid #444; border-radius:4px;
           color:#fff; font-size:11px; outline:none; margin-top:6px; }
#search:focus { border-color:#4a9eff; }
#page-list { flex:1; overflow-y:auto; padding:4px 0; }
#page-list a {
    display:block; padding:5px 10px; text-decoration:none; color:#aaa; font-size:11px;
    border-left:2px solid transparent; transition:all 0.15s; white-space:nowrap;
    overflow:hidden; text-overflow:ellipsis;
}
#page-list a:hover { background:#2a2a2a; color:#fff; }
#page-list a.active { background:#2a2a2a; color:#4a9eff; border-left-color:#4a9eff; }
#page-list a .d { color:#555; font-size:9px; }
.depth-1 { padding-left:22px !important; }
.depth-2 { padding-left:34px !important; }
.depth-3 { padding-left:46px !important; }
#resizer { width:4px; cursor:col-resize; background:#333; }
#resizer:hover { background:#4a9eff; }
#content { flex:1; display:flex; flex-direction:column; }
#toolbar { height:32px; background:#252525; border-bottom:1px solid #333; display:flex; align-items:center; padding:0 10px; gap:8px; }
#toolbar .url { flex:1; font-size:10px; color:#888; overflow:hidden; text-overflow:ellipsis; white-space:nowrap; }
#toolbar button { background:#333; border:1px solid #444; color:#ccc; padding:3px 8px; border-radius:3px; font-size:10px; cursor:pointer; }
#toolbar button:hover { background:#444; color:#fff; }
iframe { flex:1; border:none; background:#fff; }
</style>
</head>
<body>
<div id="sidebar">
    <div id="sidebar-header">
        <h2>%1</h2>
        <div class="info">%2 페이지 · %3 리소스</div>
        <input type="text" id="search" placeholder="검색..." oninput="filterPages(this.value)">
    </div>
    <div id="page-list">
%4
    </div>
</div>
<div id="resizer"></div>
<div id="content">
    <div id="toolbar">
        <span class="url" id="current-url">%5</span>
        <button onclick="window.open(document.getElementById('viewer').src)">새 탭</button>
        <button onclick="document.getElementById('viewer').contentWindow.history.back()">←</button>
        <button onclick="document.getElementById('viewer').contentWindow.history.forward()">→</button>
    </div>
    <iframe id="viewer" src="%6"></iframe>
</div>
<script>
function loadPage(path) {
    document.getElementById('viewer').src = path;
    document.getElementById('current-url').textContent = path;
    document.querySelectorAll('#page-list a').forEach(a => a.classList.remove('active'));
    event.target.closest('a').classList.add('active');
}
function filterPages(q) {
    q = q.toLowerCase();
    document.querySelectorAll('#page-list a').forEach(a => {
        a.style.display = (a.textContent.toLowerCase().includes(q) || a.title.toLowerCase().includes(q)) ? '' : 'none';
    });
}
(function(){
    const r=document.getElementById('resizer'), s=document.getElementById('sidebar');
    let dragging=false, startX, startW;
    r.addEventListener('mousedown', e=>{dragging=true;startX=e.clientX;startW=s.offsetWidth;document.body.style.cursor='col-resize';e.preventDefault();});
    document.addEventListener('mousemove', e=>{if(!dragging)return;s.style.width=Math.max(150,Math.min(500,startW+e.clientX-startX))+'px';});
    document.addEventListener('mouseup', ()=>{dragging=false;document.body.style.cursor='';});
})();
document.querySelector('#page-list a')?.classList.add('active');
</script>
</body>
</html>)HTML")
        .arg(m_baseDomain)
        .arg(m_pageCount)
        .arg(m_resourceCount)
        .arg(sidebarItems)
        .arg(firstPage.isEmpty() ? "" : firstPage)
        .arg(firstPage.isEmpty() ? "about:blank" : firstPage);

    QString indexPath = m_saveDir + "/index.html";
    QFile f(indexPath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(html.toUtf8());
        f.close();
        m_backend->log("메인 페이지 생성: index.html", "success", "crawl");
    }
}

// {domain}/index.html — 같은 도메인의 과거 크롤 세션 목록
void SiteCrawler::generateDomainIndex()
{
    QDir domainDir(m_domainDir);
    QStringList sessions = domainDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);

    QString cards;
    int validSessions = 0;
    for (const QString &s : sessions) {
        QString sessionIndex = m_domainDir + "/" + s + "/index.html";
        if (!QFile::exists(sessionIndex)) continue;
        validSessions++;
        QDir sdir(m_domainDir + "/" + s);
        int fileCount = 0;
        QDirIterator it(sdir.absolutePath(), QDir::Files, QDirIterator::Subdirectories);
        while (it.hasNext()) { it.next(); fileCount++; }
        // 세션명 YYYYMMDD_HHmmss 을 읽기 좋게: 2026-04-22 15:30:45
        QString pretty = s;
        if (s.length() == 15 && s.at(8) == '_') {
            pretty = s.left(4) + "-" + s.mid(4,2) + "-" + s.mid(6,2) + " " +
                     s.mid(9,2) + ":" + s.mid(11,2) + ":" + s.mid(13,2);
        }
        cards += QString(
            "<a class=\"site\" href=\"%1/index.html\">"
            "<div class=\"name\">%2</div>"
            "<div class=\"info\">%3개 파일</div>"
            "</a>\n"
        ).arg(s, pretty).arg(fileCount);
    }

    if (validSessions == 0) return;

    QString html = QString(R"HTML(<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="UTF-8">
<title>%1 — 크롤 세션 목록</title>
<style>
* { margin:0; padding:0; box-sizing:border-box; }
body { font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif; background:#111; color:#eee; padding:40px; min-height:100vh; }
h1 { font-size:24px; margin-bottom:8px; }
a.back { display:inline-block; color:#4a9eff; text-decoration:none; font-size:13px; margin-bottom:16px; }
.subtitle { color:#888; margin-bottom:32px; font-size:14px; }
.grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(260px,1fr)); gap:16px; }
.site { display:block; background:#1e1e1e; border:1px solid #333; border-radius:12px; padding:20px;
        text-decoration:none; color:#eee; transition:all .2s; }
.site:hover { background:#2a2a2a; border-color:#555; transform:translateY(-2px); }
.site .name { font-size:15px; font-weight:600; margin-bottom:8px; word-break:break-all; font-family:'SF Mono',Monaco,monospace; }
.site .info { font-size:12px; color:#888; }
</style>
</head>
<body>
<a class="back" href="../index.html">← 전체 사이트 목록</a>
<h1>%1</h1>
<p class="subtitle">%2회 크롤 세션 · 최신순</p>
<div class="grid">
%3
</div>
</body>
</html>)HTML").arg(m_baseDomain).arg(validSessions).arg(cards);

    QString indexPath = m_domainDir + "/index.html";
    QFile f(indexPath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(html.toUtf8());
        f.close();
        m_backend->log("도메인 세션 목록 생성: " + indexPath, "success", "crawl");
    }
}

// {parent}/index.html — 전체 도메인 목록 (각 도메인별 가장 최근 세션 시각 포함)
void SiteCrawler::generateParentIndex()
{
    QDir parentDir(m_parentDir);
    QStringList dirs = parentDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name);

    QStringList sites;
    QString siteCards;
    for (const QString &dir : dirs) {
        QString domainPath = m_parentDir + "/" + dir;
        QDir d(domainPath);
        // 세션 디렉토리 목록 (YYYYMMDD_HHmmss 형식이어야 함)
        QStringList sessions = d.entryList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name | QDir::Reversed);
        QString latestSession;
        int totalFiles = 0;
        int sessionCount = 0;
        for (const QString &s : sessions) {
            if (!QFile::exists(domainPath + "/" + s + "/index.html")) continue;
            if (latestSession.isEmpty()) latestSession = s;
            sessionCount++;
            QDirIterator it(domainPath + "/" + s, QDir::Files, QDirIterator::Subdirectories);
            while (it.hasNext()) { it.next(); totalFiles++; }
        }
        if (sessionCount == 0) continue;
        sites.append(dir);

        QString pretty = latestSession;
        if (latestSession.length() == 15 && latestSession.at(8) == '_') {
            pretty = latestSession.left(4) + "-" + latestSession.mid(4,2) + "-" + latestSession.mid(6,2) + " " +
                     latestSession.mid(9,2) + ":" + latestSession.mid(11,2);
        }
        siteCards += QString(
            "<a class=\"site\" href=\"%1/index.html\">"
            "<div class=\"name\">%1</div>"
            "<div class=\"info\">%2회 세션 · %3 파일</div>"
            "<div class=\"latest\">최근: %4</div>"
            "</a>\n"
        ).arg(dir).arg(sessionCount).arg(totalFiles).arg(pretty);
    }

    if (sites.isEmpty()) return;

    QString html = QString(R"HTML(<!DOCTYPE html>
<html lang="ko">
<head>
<meta charset="UTF-8">
<title>크롤링 사이트 목록</title>
<style>
* { margin:0; padding:0; box-sizing:border-box; }
body { font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',sans-serif; background:#111; color:#eee; padding:40px; min-height:100vh; }
h1 { font-size:24px; margin-bottom:8px; }
.subtitle { color:#888; margin-bottom:32px; font-size:14px; }
.grid { display:grid; grid-template-columns:repeat(auto-fill,minmax(260px,1fr)); gap:16px; }
.site {
    display:block; background:#1e1e1e; border:1px solid #333; border-radius:12px; padding:20px;
    text-decoration:none; color:#eee; transition:all .2s;
}
.site:hover { background:#2a2a2a; border-color:#555; transform:translateY(-2px); }
.site .name { font-size:16px; font-weight:600; margin-bottom:8px; word-break:break-all; }
.site .info { font-size:12px; color:#888; }
.site .latest { font-size:11px; color:#666; margin-top:6px; font-family:'SF Mono',Monaco,monospace; }
</style>
</head>
<body>
<h1>크롤링 사이트 목록</h1>
<p class="subtitle">%1개 사이트 · 카드 클릭 시 해당 도메인의 세션 목록</p>
<div class="grid">
%2
</div>
</body>
</html>)HTML").arg(sites.size()).arg(siteCards);

    QString indexPath = m_parentDir + "/index.html";
    QFile f(indexPath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(html.toUtf8());
        f.close();
        m_backend->log("통합 사이트 목록 생성: " + indexPath, "success", "crawl");
    }
}

void SiteCrawler::savePageListExcel(const QString &saveDir)
{
    ExcelWriter xl;
    xl.writeHeader({"URL", "Title", "Depth", "Local Path", "Resources"});

    int row = 2;
    for (const auto &page : m_pageList) {
        xl.writeRow(row++, {
            page["url"].toString(),
            page["title"].toString(),
            QString::number(page["depth"].toInt()),
            page["local_path"].toString(),
            QString::number(page["resources"].toInt())
        });
    }

    QString excelPath = saveDir + "/crawl_pages.xlsx";
    xl.save(excelPath);

#ifdef Q_OS_MACOS
    QProcess::execute("xattr", {"-w", "com.apple.metadata:kMDItemDownloadedDate",
        QString("(\"%1\")").arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate)), excelPath});
    QProcess::execute("xattr", {"-w", "com.apple.metadata:kMDItemWhereFroms",
        "(\"ABIWA crawl\")", excelPath});
#endif

    m_backend->log("Excel 저장: crawl_pages.xlsx", "success", "crawl");
}
