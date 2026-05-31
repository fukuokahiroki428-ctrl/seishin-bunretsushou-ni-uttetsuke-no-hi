#include "ContentSecurityScanner.h"
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QDateTime>

// ── JS 악성코드 패턴 정의 ──

QList<ContentSecurityScanner::Pattern> ContentSecurityScanner::jsPatterns()
{
    static QList<Pattern> patterns;
    if (!patterns.isEmpty()) return patterns;

    auto add = [&](const QString &regex, ScanResult::Severity sev, const QString &desc) {
        patterns.append({QRegularExpression(regex, QRegularExpression::CaseInsensitiveOption), sev, desc});
    };

    // ── Dangerous: 즉시 격리 ──

    // Eval + 인코딩 조합 (난독화 악성코드)
    add(R"(eval\s*\(\s*atob\s*\()", ScanResult::Dangerous, "eval(atob()) — 인코딩된 코드 실행");
    add(R"(eval\s*\(\s*unescape\s*\()", ScanResult::Dangerous, "eval(unescape()) — 난독화 코드 실행");
    add(R"(eval\s*\(\s*String\.fromCharCode)", ScanResult::Dangerous, "eval(String.fromCharCode()) — 문자열 기반 코드 실행");
    add(R"(eval\s*\(\s*function\s*\(\s*\w*\s*,\s*\w*\s*\)\s*\{.*?return)", ScanResult::Dangerous, "eval(function) — 동적 코드 생성/실행");
    add(R"(new\s+Function\s*\(\s*atob)", ScanResult::Dangerous, "new Function(atob()) — 인코딩 코드 동적 실행");
    add(R"(new\s+Function\s*\(\s*unescape)", ScanResult::Dangerous, "new Function(unescape()) — 난독화 동적 실행");

    // 크립토 마이너
    add(R"(coinhive|cryptonight|CoinImp|crypto-?loot|miner\.start\s*\()", ScanResult::Dangerous, "암호화폐 마이너 감지");
    add(R"(wasmBinaryFile.*cryptonight|importScripts.*miner)", ScanResult::Dangerous, "WebAssembly 마이너");

    // 키로거
    add(R"(addEventListener\s*\(\s*['"]key(?:down|press|up)['"]\s*,[\s\S]{0,200}(?:fetch|XMLHttpRequest|sendBeacon|new\s+Image))", ScanResult::Dangerous, "키로거 패턴 감지");
    add(R"(onkey(?:down|press|up)\s*=[\s\S]{0,200}(?:\.src\s*=|fetch\s*\(|XMLHttpRequest))", ScanResult::Dangerous, "키 입력 탈취");

    // 데이터 유출
    add(R"(document\.cookie[\s\S]{0,100}(?:fetch\s*\(|new\s+Image\s*\(\s*\)\.src|XMLHttpRequest|sendBeacon))", ScanResult::Dangerous, "쿠키 유출 시도");
    add(R"(localStorage[\s\S]{0,100}(?:fetch\s*\(|new\s+Image|XMLHttpRequest)[\s\S]{0,50}(?:http|\/\/))", ScanResult::Dangerous, "로컬스토리지 유출 시도");
    add(R"(navigator\.credentials[\s\S]{0,100}(?:fetch|XMLHttpRequest|sendBeacon))", ScanResult::Dangerous, "인증정보 탈취 시도");

    // 난독화된 리다이렉트
    add(R"((?:window|document)\.location\s*=\s*(?:atob|unescape|String\.fromCharCode))", ScanResult::Dangerous, "난독화 리다이렉트");
    add(R"(document\.location\.replace\s*\(\s*(?:atob|unescape))", ScanResult::Dangerous, "인코딩 리다이렉트");

    // 동적 스크립트 삽입 + 외부 URL (GTM, reCAPTCHA 등 정상 사이트도 사용 → Warning)
    add(R"(createElement\s*\(\s*['"]script['"][\s\S]{0,200}\.src\s*=\s*['"]https?://)", ScanResult::Warning, "외부 스크립트 동적 삽입");

    // ── Warning: 주의 필요 ──

    // 일반적인 eval 사용 (라이브러리에서도 사용)
    add(R"(eval\s*\((?!['"]))", ScanResult::Warning, "eval() 사용 감지");
    add(R"(document\.write\s*\()", ScanResult::Warning, "document.write() 사용");
    add(R"(innerHTML\s*=\s*(?:.*(?:location\.(?:search|hash|href)|document\.URL|document\.referrer)))", ScanResult::Warning, "innerHTML XSS 가능성");
    add(R"(window\.open\s*\([\s\S]{0,50}(?:atob|unescape))", ScanResult::Warning, "인코딩 URL 팝업");
    add(R"(\.src\s*=\s*['"]data:text/html)", ScanResult::Warning, "data URI 스크립트 로드");
    add(R"(postMessage\s*\([\s\S]{0,100}\*['"])", ScanResult::Warning, "postMessage 와일드카드");

    return patterns;
}

// ── JS 스캔 ──

ScanResult ContentSecurityScanner::scanJavaScript(const QByteArray &content, const QString &sourceUrl)
{
    Q_UNUSED(sourceUrl);
    ScanResult result;
    QString text = QString::fromUtf8(content);

    for (const auto &p : jsPatterns()) {
        if (p.regex.match(text).hasMatch()) {
            if (p.severity > result.severity) result.severity = p.severity;
            result.findings.append(p.description);
        }
    }

    if (result.severity == ScanResult::Dangerous)
        result.suggestedAction = "quarantine";
    else if (result.severity == ScanResult::Warning)
        result.suggestedAction = "sanitize";
    else
        result.suggestedAction = "pass";

    return result;
}

// ── 파일 타입 위장 검사 ──

ScanResult ContentSecurityScanner::scanFileType(const QString &filePath, const QByteArray &content)
{
    ScanResult result;
    QString ext = QFileInfo(filePath).suffix().toLower();

    // 이중 확장자 검사 (단, webpack/vite 해시 패턴은 제외)
    // 정상: 61540.f790fb642cd46ee5.js, locale.ja.b23f302ac854e2bc.js, app-cf001e8c137c06d8.js
    // 위험: image.jpg.exe, document.pdf.scr
    QString basename = QFileInfo(filePath).fileName();
    QRegularExpression doubleExt(R"(\.\w+\.(exe|bat|sh|php|html|htm|vbs|ps1|cmd|scr|pif)$)",
        QRegularExpression::CaseInsensitiveOption);
    // .js는 이중 확장자에서 제외 — 웹팩/번들러가 hash.chunk.js 패턴을 정상적으로 사용
    if (doubleExt.match(basename).hasMatch()) {
        // 해시 패턴 예외: 숫자.hex해시.ext 또는 이름.hex해시.ext (번들러 출력)
        QRegularExpression hashPattern(R"(\.[0-9a-f]{8,}\.\w+$)", QRegularExpression::CaseInsensitiveOption);
        if (!hashPattern.match(basename).hasMatch()) {
            result.severity = ScanResult::Dangerous;
            result.findings.append("이중 확장자 감지: " + basename);
            result.suggestedAction = "quarantine";
            return result;
        }
    }

    if (content.isEmpty()) return result;

    // 매직바이트 vs 확장자 불일치 검사
    struct MagicByte { QByteArray prefix; QString expectedType; QStringList validExts; };
    static const QList<MagicByte> magicBytes = {
        {QByteArray("\x89PNG\r\n\x1a\n", 8), "image/png", {"png"}},
        {QByteArray("\xFF\xD8\xFF", 3), "image/jpeg", {"jpg", "jpeg"}},
        {QByteArray("GIF87a", 6), "image/gif", {"gif"}},
        {QByteArray("GIF89a", 6), "image/gif", {"gif"}},
        {QByteArray("%PDF", 4), "application/pdf", {"pdf"}},
        {QByteArray("PK\x03\x04", 4), "application/zip", {"zip", "xlsx", "docx", "pptx", "jar"}},
        {QByteArray("RIFF", 4), "audio/video", {"wav", "webp", "avi"}},
        {QByteArray("\x1a\x45\xdf\xa3", 4), "video/webm", {"webm", "mkv"}},
    };

    bool isImageExt = QStringList({"png","jpg","jpeg","gif","webp","svg","bmp","ico","avif"}).contains(ext);
    bool isFontExt = QStringList({"woff","woff2","ttf","eot","otf"}).contains(ext);

    // 이미지/폰트 확장자인데 HTML/JS 내용
    if (isImageExt || isFontExt) {
        QString textContent = QString::fromUtf8(content.left(500)).trimmed();
        if (textContent.startsWith("<html", Qt::CaseInsensitive) ||
            textContent.startsWith("<!DOCTYPE", Qt::CaseInsensitive) ||
            textContent.startsWith("<script", Qt::CaseInsensitive) ||
            textContent.contains("eval(") ||
            textContent.contains("function(")) {
            result.severity = ScanResult::Dangerous;
            result.findings.append("파일 위장: " + ext + " 확장자에 HTML/JS 내용");
            result.suggestedAction = "quarantine";
            return result;
        }
    }

    // 매직바이트 불일치
    for (const auto &mb : magicBytes) {
        if (content.startsWith(mb.prefix)) {
            if (!mb.validExts.contains(ext) && !ext.isEmpty()) {
                result.severity = ScanResult::Warning;
                result.findings.append(QString("확장자 불일치: .%1이지만 실제 %2 형식").arg(ext, mb.expectedType));
                result.suggestedAction = "sanitize";
            }
            break;
        }
    }

    return result;
}

// ── 격리 ──

bool ContentSecurityScanner::quarantineFile(const QString &filePath, const QString &quarantineDir)
{
    QDir().mkpath(quarantineDir);
    createBlockedPlaceholder(quarantineDir);

    QString filename = QFileInfo(filePath).fileName();
    QString destPath = quarantineDir + "/" + filename;

    // 중복 방지
    int i = 1;
    while (QFile::exists(destPath)) {
        destPath = quarantineDir + "/" + QString::number(i++) + "_" + filename;
    }

    // 파일 이동
    bool moved = QFile::rename(filePath, destPath);
    if (!moved) {
        QFile::copy(filePath, destPath);
        QFile::remove(filePath);
    }

    // 보고서 생성
    QFile report(destPath + ".report.txt");
    if (report.open(QIODevice::WriteOnly)) {
        report.write(QString("Quarantined: %1\nOriginal: %2\nDate: %3\n")
            .arg(filename, filePath, QDateTime::currentDateTime().toString(Qt::ISODate)).toUtf8());
        report.close();
    }

    return true;
}

// ── JS 정화 (Warning 레벨) ──

QByteArray ContentSecurityScanner::sanitizeJavaScript(const QByteArray &content)
{
    QString text = QString::fromUtf8(content);

    // eval(atob(...)) → /* BLOCKED: eval(atob(...)) */
    text.replace(QRegularExpression(R"(eval\s*\(\s*atob\s*\([^)]*\)\s*\))"),
        "/* ABIWA_BLOCKED: eval+atob removed */");
    text.replace(QRegularExpression(R"(eval\s*\(\s*unescape\s*\([^)]*\)\s*\))"),
        "/* ABIWA_BLOCKED: eval+unescape removed */");

    // document.cookie 접근 차단
    text.replace(QRegularExpression(R"(document\.cookie)"), "/* ABIWA_BLOCKED: document.cookie */''");

    // WebSocket 차단
    text.replace(QRegularExpression(R"(new\s+WebSocket\s*\()"), "/* ABIWA_BLOCKED */ new (function(){this.send=function(){};this.close=function(){}})(");

    return text.toUtf8();
}

// ── JS 샌드박스 헤더 ──

QByteArray ContentSecurityScanner::jsSandboxHeader()
{
    return QByteArray(
        "/* ABIWA Offline Sandbox — 위험 API 비활성화 */\n"
        "(function(){\n"
        "  'use strict';\n"
        "  var noop=function(){return Promise.resolve(new Response('',{status:0}));};\n"
        "  try{Object.defineProperty(window,'fetch',{value:noop,writable:false});}catch(e){}\n"
        "  try{window.XMLHttpRequest=function(){this.open=function(){};this.send=function(){};this.setRequestHeader=function(){};};}catch(e){}\n"
        "  try{window.WebSocket=function(){this.send=function(){};this.close=function(){};};}catch(e){}\n"
        "  try{navigator.sendBeacon=function(){return false;};}catch(e){}\n"
        "  try{var _ce=document.createElement.bind(document);document.createElement=function(t){\n"
        "    var el=_ce(t);if(t.toLowerCase()==='script'){\n"
        "      Object.defineProperty(el,'src',{set:function(){},get:function(){return '';}});\n"
        "    }return el;};}catch(e){}\n"
        "})();\n"
    );
}

// ── CSP 삽입 ──

QString ContentSecurityScanner::injectCSP(const QString &html)
{
    QString csp = "<meta http-equiv=\"Content-Security-Policy\" content=\""
                  "default-src 'self' 'unsafe-inline' data: blob:; "
                  "script-src 'self' 'unsafe-inline' 'unsafe-eval'; "
                  "connect-src 'none'; "
                  "form-action 'none'; "
                  "frame-ancestors 'self';\">\n";

    int headPos = html.indexOf("<head", 0, Qt::CaseInsensitive);
    if (headPos == -1) return html;
    int headEnd = html.indexOf(">", headPos);
    if (headEnd == -1) return html;

    QString result = html;
    result.insert(headEnd + 1, "\n" + csp);
    return result;
}

// ── blocked.html 플레이스홀더 ──

void ContentSecurityScanner::createBlockedPlaceholder(const QString &quarantineDir)
{
    QString path = quarantineDir + "/blocked.html";
    if (QFile::exists(path)) return;

    QFile f(path);
    if (f.open(QIODevice::WriteOnly)) {
        f.write("<!DOCTYPE html><html><head><meta charset='UTF-8'></head><body style='background:#1a1a1a;color:#ff4444;"
                "font-family:sans-serif;display:flex;align-items:center;justify-content:center;height:100vh;margin:0;'>"
                "<div style='text-align:center;'><h2>⚠ 차단됨</h2><p>보안 검사에서 위험 콘텐츠가 감지되어 격리되었습니다.</p>"
                "<p style='color:#888;font-size:12px;'>_quarantine 폴더에서 상세 보고서를 확인하세요.</p></div></body></html>");
        f.close();
    }
}
