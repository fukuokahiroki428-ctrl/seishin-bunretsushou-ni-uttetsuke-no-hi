#include "FileHelper.h"
#include "core/Common.h"
#include "HttpClient.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QDateTime>
#include <QUrl>
#include <QCryptographicHash>
#include <QJsonArray>

#ifdef Q_OS_WIN
#include <sys/types.h>
#include <sys/utime.h>
#else
#include <sys/stat.h>
#include <utime.h>
#endif

#ifdef Q_OS_MACOS
#include <sys/xattr.h>
#endif

namespace FileHelper {

void setFileTimes(const QString &filePath, const QDateTime &timestamp)
{
    Common::setFileTimes(filePath, timestamp);
}

void setFileTimes(const QString &filePath, const QString &timestampStr)
{
    Common::setFileTimes(filePath, timestampStr);
}

void setFinderComment(const QString &filePath, const QString &comment)
{
#ifdef Q_OS_MACOS
    // ★ 셸/AppleScript 문자열 보간 금지 — 값은 osascript 의 argv 로 전달한다.
    //   이전엔 sh -c + 따옴표 보간이라 파일명/코멘트에 ' 또는 " 가 있으면
    //   AppleScript 가 깨지거나 셸 명령 주입이 가능했다 (이스케이프 순서도 거꾸로였음).
    static const QString script = QStringLiteral(
        "on run argv\n"
        "  set p to item 1 of argv\n"
        "  set c to item 2 of argv\n"
        "  tell application \"Finder\" to set comment of (POSIX file p as alias) to c\n"
        "end run");

    QProcess proc;
    proc.start("/usr/bin/osascript", {"-e", script, filePath, comment});
    proc.waitForFinished(5000);
#else
    Q_UNUSED(filePath);
    Q_UNUSED(comment);
#endif
}

QString sanitizeFilename(const QString &name, int maxLength)
{
    QString result = name;
    // Remove invalid characters
    result.replace(QRegularExpression("[/\\\\:*?\"<>|\\x00-\\x1f]"), "_");
    // Trim whitespace
    result = result.trimmed();
    // Truncate
    if (result.length() > maxLength) {
        result = result.left(maxLength);
    }
    if (result.isEmpty()) {
        result = "unnamed";
    }
    return result;
}

bool ensureDir(const QString &path)
{
    return QDir().mkpath(path);
}

void setDownloadMeta(const QString &filePath, const QString &url)
{
#ifdef Q_OS_MACOS
    // setxattr syscall — QProcess::execute("xattr"...) 대비 ~100배 빠름
    QByteArray pathUtf8 = filePath.toUtf8();

    // kMDItemWhereFroms — plist format (단일 URL)
    QString urlPlist = QString(
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
        "<plist version=\"1.0\"><array><string>%1</string></array></plist>"
    ).arg(url.toHtmlEscaped());
    QByteArray urlData = urlPlist.toUtf8();
    setxattr(pathUtf8.constData(), "com.apple.metadata:kMDItemWhereFroms",
             urlData.constData(), urlData.size(), 0, 0);

    // kMDItemDownloadedDate — plist format (현재 시각)
    QString datePlist = QString(
        "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
        "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">"
        "<plist version=\"1.0\"><array><date>%1</date></array></plist>"
    ).arg(QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    QByteArray dateData = datePlist.toUtf8();
    setxattr(pathUtf8.constData(), "com.apple.metadata:kMDItemDownloadedDate",
             dateData.constData(), dateData.size(), 0, 0);
#else
    Q_UNUSED(filePath);
    Q_UNUSED(url);
#endif
}

// ──────────────────────────────────────────────────────────
// 공통 저장 정책 helpers
// ──────────────────────────────────────────────────────────

QString uploadOrderPrefix(const QDateTime &uploadTime)
{
    if (!uploadTime.isValid()) return QString();
    return uploadTime.toString("yyyyMMdd_HHmmss_");
}

QString uploadOrderPrefix(qint64 unixSec)
{
    if (unixSec <= 0) return QString();
    return QDateTime::fromSecsSinceEpoch(unixSec).toString("yyyyMMdd_HHmmss_");
}

QString typeFolder(const QString &baseDir, const QString &type)
{
    QString folder;
    if (type == "all" || type == "complete") {
        folder = baseDir + "/_complete";
    } else {
        folder = baseDir + "/" + type;
    }
    QDir().mkpath(folder);
    return folder;
}

QString typeExcelPath(const QString &excelDir, const QString &target, const QString &type)
{
    QDir().mkpath(excelDir);
    QString clean = sanitizeFilename(target, 100);
    if (clean.isEmpty()) clean = "data";
    if (type == "all" || type == "complete") {
        return QString("%1/%2_complete.xlsx").arg(excelDir, clean);
    }
    return QString("%1/%2_%3.xlsx").arg(excelDir, clean, type);
}

void applyPostMetadata(const QString &filePath,
                        const QDateTime &uploadTime,
                        const QString &sourceUrl,
                        const QString &finderComment)
{
    // 1. 파일 시각 (mtime/btime)
    if (uploadTime.isValid()) {
        setFileTimes(filePath, uploadTime);
    }

    // 2. macOS 다운로드 xattr
    if (!sourceUrl.isEmpty()) {
        setDownloadMeta(filePath, sourceUrl);
    }

    // 3. Finder 코멘트
    if (!finderComment.isEmpty()) {
        setFinderComment(filePath, finderComment);
    }
}

// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━
// 経済産業省 연계: 페이지 캡처 (SingleFile 스타일)
// ━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━

QString capturePageHtml(const QString &saveDir,
                        const QString &url,
                        const QString &title,
                        void *httpPtr,
                        const QMap<QString, QString> &headers)
{
    if (url.isEmpty()) return QString();
    QDir().mkpath(saveDir);

    // 파일명 생성
    QString safeName = title.isEmpty() ? QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5).toHex().left(16)
                                       : sanitizeFilename(title, 80);
    QString filePath = saveDir + "/" + safeName + ".html";
    if (QFile::exists(filePath)) return filePath; // 이미 캡처됨

    // HTTP 클라이언트
    HttpClient *http = httpPtr ? static_cast<HttpClient*>(httpPtr) : nullptr;
    bool ownHttp = false;
    if (!http) {
        http = new HttpClient();
        http->setTimeout(30000);
        ownHttp = true;
    }

    // 페이지 가져오기
    HttpResponse resp = http->get(url, headers);
    if (!resp.isOk() || resp.data.isEmpty()) {
        if (ownHttp) delete http;
        return QString();
    }

    QString html = QString::fromUtf8(resp.data);
    QUrl baseUrl(url);

    // [JS SPA 감지] 완전 클라이언트 렌더링 사이트(Twitter/Bluesky/Instagram 등)는
    // HTTP GET으로 받으면 JS 필요 안내 셸만 저장됨 → 저장 생략
    const QString htmlLower = html.toLower();
    bool isJsShell =
        htmlLower.contains("javascript is not available") ||
        htmlLower.contains("enable javascript to") ||
        htmlLower.contains("please enable javascript") ||
        htmlLower.contains("javascript is disabled") ||
        (htmlLower.contains("<noscript") && html.size() < 8192); // 8KB 이하 + noscript = 셸 추정
    if (isJsShell) {
        qDebug() << "[capturePageHtml] JS-required shell detected — skipping save:" << url;
        if (ownHttp) delete http;
        return QString();
    }

    // CSS/이미지를 base64 인라인으로 변환
    // 1) <img src="..."> → data:image/*;base64,...
    // 2) <link rel="stylesheet" href="..."> → <style>인라인</style>
    // 3) url(...) in style → data:...

    // 이미지 인라인
    QRegularExpression imgRe(R"((src|data-src)\s*=\s*["']([^"']+\.(?:jpg|jpeg|png|gif|webp|svg|ico)(?:\?[^"']*)?)["'])",
        QRegularExpression::CaseInsensitiveOption);
    auto imgIt = imgRe.globalMatch(html);
    QMap<QString, QString> replacements;
    int inlineCount = 0;
    const int MAX_INLINE = 50; // 메모리 보호

    while (imgIt.hasNext() && inlineCount < MAX_INLINE) {
        auto m = imgIt.next();
        QString imgUrl = m.captured(2).trimmed();
        if (imgUrl.startsWith("data:")) continue;
        // 절대 URL로 변환
        QString absUrl = imgUrl;
        if (!imgUrl.startsWith("http")) {
            absUrl = baseUrl.resolved(QUrl(imgUrl)).toString();
        }
        if (replacements.contains(imgUrl)) continue;

        HttpResponse imgResp = http->get(absUrl, headers);
        if (imgResp.isOk() && !imgResp.data.isEmpty() && imgResp.data.size() < 5 * 1024 * 1024) { // 5MB 제한
            QString ext = QFileInfo(QUrl(absUrl).path()).suffix().toLower();
            QString mime = "image/jpeg";
            if (ext == "png") mime = "image/png";
            else if (ext == "gif") mime = "image/gif";
            else if (ext == "webp") mime = "image/webp";
            else if (ext == "svg") mime = "image/svg+xml";
            else if (ext == "ico") mime = "image/x-icon";
            QString b64 = QString("data:%1;base64,%2").arg(mime, QString::fromLatin1(imgResp.data.toBase64()));
            replacements[imgUrl] = b64;
            inlineCount++;
        }
    }

    // CSS 인라인
    QRegularExpression cssRe(R"(<link[^>]+rel\s*=\s*["']stylesheet["'][^>]+href\s*=\s*["']([^"']+)["'][^>]*/?>)",
        QRegularExpression::CaseInsensitiveOption);
    auto cssIt = cssRe.globalMatch(html);
    QList<QPair<QString, QString>> cssReplacements; // {원본 태그, 대체 style}

    while (cssIt.hasNext() && inlineCount < MAX_INLINE + 20) {
        auto m = cssIt.next();
        QString cssUrl = m.captured(1).trimmed();
        if (cssUrl.startsWith("data:")) continue;
        QString absUrl = cssUrl.startsWith("http") ? cssUrl : baseUrl.resolved(QUrl(cssUrl)).toString();

        HttpResponse cssResp = http->get(absUrl, headers);
        if (cssResp.isOk() && !cssResp.data.isEmpty() && cssResp.data.size() < 1024 * 1024) { // 1MB
            QString cssContent = QString::fromUtf8(cssResp.data);
            cssReplacements.append({m.captured(0), "<style>/* " + absUrl + " */\n" + cssContent + "\n</style>"});
            inlineCount++;
        }
    }

    // 치환 적용
    for (auto it = replacements.constBegin(); it != replacements.constEnd(); ++it) {
        html.replace(it.key(), it.value());
    }
    for (const auto &p : cssReplacements) {
        html.replace(p.first, p.second);
    }

    // 메타데이터 주입: 원본 URL + 캡처 시각
    QString metaTag = QString(
        "\n<!-- SingleFile capture by ABIWA -->\n"
        "<!-- source: %1 -->\n"
        "<!-- captured: %2 -->\n")
        .arg(url.toHtmlEscaped(), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    int headEnd = html.indexOf("</head>", 0, Qt::CaseInsensitive);
    if (headEnd > 0) {
        html.insert(headEnd, metaTag);
    } else {
        html.prepend(metaTag);
    }

    // 저장
    QFile f(filePath);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(html.toUtf8());
        f.close();
        // xattr + Finder comment
        setDownloadMeta(filePath, url);
        setFinderComment(filePath, url);
    }

    if (ownHttp) delete http;
    return filePath;
}

// ──────────────────────────────────────────────────────────
// 브라우저-렌더된 HTML → 단일 .html 저장 (JS SPA 대응)
// ──────────────────────────────────────────────────────────
//
// QWebEngineView::page()->toHtml() 콜백에서 받은 HTML을 그대로 저장.
// HTTP GET이 아니라 브라우저가 실행한 후의 실제 DOM이므로 Twitter/asked.kr
// 같은 SPA도 사용자 화면 그대로 캡처된다. CSS/이미지 인라인은 best-effort
// (inlineHttp가 주어지면 5MB 미만 이미지/1MB 미만 CSS만 base64로 박음).

QString capturePageHtmlFromContent(const QString &saveDir,
                                    const QString &url,
                                    const QString &title,
                                    const QString &renderedHtml,
                                    void *inlineHttp,
                                    const QMap<QString, QString> &headers)
{
    if (renderedHtml.isEmpty()) return QString();
    QDir().mkpath(saveDir);

    QString safeName = title.isEmpty()
        ? QCryptographicHash::hash(url.toUtf8(), QCryptographicHash::Md5).toHex().left(16)
        : sanitizeFilename(title, 80);
    QString filePath = saveDir + "/" + safeName + ".html";

    QString html = renderedHtml;
    QUrl baseUrl(url);

    // best-effort: CSS/이미지 인라인 (HttpClient가 주어졌을 때만)
    if (inlineHttp) {
        HttpClient *http = static_cast<HttpClient*>(inlineHttp);

        // 이미지 인라인
        QRegularExpression imgRe(
            R"((src|data-src)\s*=\s*["']([^"']+\.(?:jpg|jpeg|png|gif|webp|svg|ico)(?:\?[^"']*)?)["'])",
            QRegularExpression::CaseInsensitiveOption);
        auto imgIt = imgRe.globalMatch(html);
        QMap<QString, QString> replacements;
        int inlineCount = 0;
        const int MAX_INLINE = 50;
        while (imgIt.hasNext() && inlineCount < MAX_INLINE) {
            auto m = imgIt.next();
            QString imgUrl = m.captured(2).trimmed();
            if (imgUrl.startsWith("data:")) continue;
            QString absUrl = imgUrl.startsWith("http")
                ? imgUrl
                : baseUrl.resolved(QUrl(imgUrl)).toString();
            if (replacements.contains(imgUrl)) continue;
            HttpResponse imgResp = http->get(absUrl, headers);
            if (imgResp.isOk() && !imgResp.data.isEmpty()
                && imgResp.data.size() < 5 * 1024 * 1024) {
                QString ext = QFileInfo(QUrl(absUrl).path()).suffix().toLower();
                QString mime = "image/jpeg";
                if (ext == "png") mime = "image/png";
                else if (ext == "gif") mime = "image/gif";
                else if (ext == "webp") mime = "image/webp";
                else if (ext == "svg") mime = "image/svg+xml";
                else if (ext == "ico") mime = "image/x-icon";
                QString b64 = QString("data:%1;base64,%2")
                    .arg(mime, QString::fromLatin1(imgResp.data.toBase64()));
                replacements[imgUrl] = b64;
                inlineCount++;
            }
        }

        // CSS 인라인
        QRegularExpression cssRe(
            R"(<link[^>]+rel\s*=\s*["']stylesheet["'][^>]+href\s*=\s*["']([^"']+)["'][^>]*/?>)",
            QRegularExpression::CaseInsensitiveOption);
        auto cssIt = cssRe.globalMatch(html);
        QList<QPair<QString, QString>> cssReplacements;
        while (cssIt.hasNext() && inlineCount < MAX_INLINE + 20) {
            auto m = cssIt.next();
            QString cssUrl = m.captured(1).trimmed();
            if (cssUrl.startsWith("data:")) continue;
            QString absUrl = cssUrl.startsWith("http")
                ? cssUrl
                : baseUrl.resolved(QUrl(cssUrl)).toString();
            HttpResponse cssResp = http->get(absUrl, headers);
            if (cssResp.isOk() && !cssResp.data.isEmpty()
                && cssResp.data.size() < 1024 * 1024) {
                QString cssContent = QString::fromUtf8(cssResp.data);
                cssReplacements.append({m.captured(0),
                    "<style>/* " + absUrl + " */\n" + cssContent + "\n</style>"});
                inlineCount++;
            }
        }

        for (auto it = replacements.constBegin(); it != replacements.constEnd(); ++it)
            html.replace(it.key(), it.value());
        for (const auto &p : cssReplacements)
            html.replace(p.first, p.second);
    }

    // 메타데이터 주입
    QString metaTag = QString(
        "\n<!-- ABIWA browser capture (rendered DOM) -->\n"
        "<!-- source: %1 -->\n"
        "<!-- captured: %2 -->\n")
        .arg(url.toHtmlEscaped(), QDateTime::currentDateTimeUtc().toString(Qt::ISODate));
    int headEnd = html.indexOf("</head>", 0, Qt::CaseInsensitive);
    if (headEnd > 0) html.insert(headEnd, metaTag);
    else html.prepend(metaTag);

    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly)) return QString();
    f.write(html.toUtf8());
    f.close();
    setDownloadMeta(filePath, url);
    setFinderComment(filePath, url);
    return filePath;
}

// ──────────────────────────────────────────────────────────
// 트윗 JSON → 정적 아카이브 HTML
// ──────────────────────────────────────────────────────────

static QString htmlEscape(const QString &s)
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

// 트윗 본문의 URL, @mention, #hashtag를 하이퍼링크로 변환 (줄바꿈은 <br>로)
static QString linkifyTweetText(const QString &raw)
{
    QString escaped = htmlEscape(raw);
    // http(s)://... URL → <a>
    static const QRegularExpression urlRe(R"((https?://[^\s<]+))");
    escaped.replace(urlRe, R"(<a href="\1" target="_blank" rel="noopener">\1</a>)");
    // @username → X 링크 (단, 이미 <a> 안에 있는 건 건드리지 않도록 주의)
    static const QRegularExpression mentionRe(R"((^|[^a-zA-Z0-9_>])@([A-Za-z0-9_]{1,15}))");
    escaped.replace(mentionRe,
        R"(\1<a href="https://x.com/\2" target="_blank" rel="noopener">@\2</a>)");
    // #tag → X 검색 링크
    static const QRegularExpression hashRe(R"((^|[^&a-zA-Z0-9_>])#([^\s<#]+))");
    escaped.replace(hashRe,
        R"(\1<a href="https://x.com/hashtag/\2" target="_blank" rel="noopener">#\2</a>)");
    // 줄바꿈
    escaped.replace('\n', "<br>");
    return escaped;
}

QString generateTweetArchiveHtml(const QString &saveDir,
                                  const QString &filename,
                                  const QJsonObject &meta)
{
    if (filename.isEmpty()) return QString();
    QDir().mkpath(saveDir);
    const QString filePath = saveDir + "/" + filename + ".html";
    // 덮어쓰기 허용 — JS 셸로 저장된 기존 파일을 교체하기 위함

    const QString authorName   = meta.value("authorName").toString();
    const QString handle       = meta.value("handle").toString(authorName);
    const QString tweetId      = meta.value("tweetId").toString();
    const QString tweetUrl     = meta.value("tweetUrl").toString();
    const QString tweetText    = meta.value("tweetText").toString();
    const QString createdAt    = meta.value("createdAt").toString();
    const QString tweetType    = meta.value("tweetType").toString("Tweet");
    const int favoriteCount    = meta.value("favoriteCount").toInt();
    const int retweetCount     = meta.value("retweetCount").toInt();
    const int replyCount       = meta.value("replyCount").toInt();
    const QJsonArray mediaArr  = meta.value("mediaRelPaths").toArray();

    // 미디어 블록 생성 — 이미지/비디오 구분
    QString mediaBlock;
    if (!mediaArr.isEmpty()) {
        mediaBlock += "<div class=\"media\">\n";
        for (const auto &v : mediaArr) {
            const QString rel = htmlEscape(v.toString());
            const QString low = v.toString().toLower();
            if (low.endsWith(".mp4") || low.endsWith(".mov") || low.endsWith(".webm")) {
                mediaBlock += QString("  <video src=\"%1\" controls preload=\"metadata\"></video>\n").arg(rel);
            } else {
                mediaBlock += QString("  <a href=\"%1\" target=\"_blank\"><img src=\"%1\" loading=\"lazy\" alt=\"\"></a>\n").arg(rel);
            }
        }
        mediaBlock += "</div>\n";
    }

    // 제목 — "이름 (@handle) : 첫 40자"
    QString preview = tweetText.left(40).trimmed();
    preview.replace('\n', ' ');
    QString title = authorName;
    if (!handle.isEmpty() && handle != authorName) title += " (@" + handle + ")";
    if (!preview.isEmpty()) title += " — " + preview;

    // HTML 생성
    QString html;
    html += "<!DOCTYPE html>\n";
    html += "<html lang=\"ja\">\n";
    html += "<head>\n";
    html += "<meta charset=\"utf-8\">\n";
    html += "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n";
    html += QString("<title>%1</title>\n").arg(htmlEscape(title));
    html += "<style>\n"
            "  :root { color-scheme: light dark; }\n"
            "  * { box-sizing: border-box; }\n"
            "  body { font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', 'Hiragino Sans',\n"
            "         'Noto Sans CJK JP', sans-serif; background: #f7f9fa; margin: 0; padding: 20px;\n"
            "         color: #0f1419; }\n"
            "  @media (prefers-color-scheme: dark) {\n"
            "    body { background: #000; color: #e7e9ea; }\n"
            "    .card { background: #16181c; border-color: #2f3336; }\n"
            "    .meta, .handle, .stats { color: #71767b; }\n"
            "    .sep { border-color: #2f3336; }\n"
            "  }\n"
            "  .card { max-width: 620px; margin: 0 auto; background: #fff;\n"
            "          border: 1px solid #e1e8ed; border-radius: 16px; padding: 20px; }\n"
            "  .head { display: flex; align-items: baseline; gap: 8px; flex-wrap: wrap; }\n"
            "  .author { font-weight: 700; font-size: 15px; }\n"
            "  .handle { color: #536471; font-size: 15px; }\n"
            "  .badge { display: inline-block; padding: 2px 8px; border-radius: 4px;\n"
            "           font-size: 12px; background: #e8f5fe; color: #1d9bf0; }\n"
            "  .text { white-space: pre-wrap; word-wrap: break-word; font-size: 17px;\n"
            "          line-height: 1.45; margin: 14px 0; }\n"
            "  .text a { color: #1d9bf0; text-decoration: none; }\n"
            "  .text a:hover { text-decoration: underline; }\n"
            "  .media { display: grid; grid-template-columns: repeat(auto-fit, minmax(240px, 1fr));\n"
            "           gap: 4px; border-radius: 16px; overflow: hidden; margin: 12px 0;\n"
            "           background: #e1e8ed; }\n"
            "  .media img, .media video { width: 100%; height: auto; display: block; }\n"
            "  .sep { border: none; border-top: 1px solid #e1e8ed; margin: 14px 0; }\n"
            "  .meta { color: #536471; font-size: 13px; }\n"
            "  .stats { display: flex; gap: 20px; margin-top: 8px; color: #536471;\n"
            "           font-size: 13px; flex-wrap: wrap; }\n"
            "  .link { display: inline-block; margin-top: 12px; color: #1d9bf0;\n"
            "          text-decoration: none; font-size: 14px; }\n"
            "  .link:hover { text-decoration: underline; }\n"
            "</style>\n";
    html += "</head>\n<body>\n";
    html += "<article class=\"card\">\n";
    html += "  <div class=\"head\">\n";
    html += QString("    <span class=\"author\">%1</span>\n").arg(htmlEscape(authorName));
    if (!handle.isEmpty() && handle != authorName) {
        html += QString("    <span class=\"handle\">@%1</span>\n").arg(htmlEscape(handle));
    }
    html += QString("    <span class=\"badge\">%1</span>\n").arg(htmlEscape(tweetType));
    html += "  </div>\n";
    if (!tweetText.isEmpty()) {
        html += QString("  <div class=\"text\">%1</div>\n").arg(linkifyTweetText(tweetText));
    }
    html += mediaBlock;
    html += "  <hr class=\"sep\">\n";
    if (!createdAt.isEmpty()) {
        html += QString("  <div class=\"meta\"><time datetime=\"%1\">%1</time></div>\n")
                    .arg(htmlEscape(createdAt));
    }
    html += "  <div class=\"stats\">\n";
    html += QString("    <span>Replies %1</span>\n").arg(replyCount);
    html += QString("    <span>Reposts %1</span>\n").arg(retweetCount);
    html += QString("    <span>Likes %1</span>\n").arg(favoriteCount);
    html += "  </div>\n";
    if (!tweetUrl.isEmpty()) {
        html += QString("  <a class=\"link\" href=\"%1\" target=\"_blank\" rel=\"noopener\">원본 보기 · View on X</a>\n")
                    .arg(htmlEscape(tweetUrl));
    }
    html += "</article>\n";
    // 검색용 메타 — 크롤러가 찾기 쉽도록 마지막에 숨김 정보
    html += QString("<!-- tweet:%1 author:%2 -->\n")
                .arg(htmlEscape(tweetId), htmlEscape(handle));
    html += "</body>\n</html>\n";

    QFile f(filePath);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return QString();
    f.write(html.toUtf8());
    f.close();

    if (!tweetUrl.isEmpty()) {
        setDownloadMeta(filePath, tweetUrl);
        setFinderComment(filePath, tweetUrl);
    }
    return filePath;
}

} // namespace FileHelper
