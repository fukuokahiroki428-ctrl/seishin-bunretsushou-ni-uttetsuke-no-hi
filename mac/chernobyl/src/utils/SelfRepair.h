#pragma once
#include <QStorageInfo>
// ═════════════════════════════════════════════════════════════════════════
// SelfRepair.h — 앱 자가진단 · 자가복구 + 로컬 LLM 진단 계층 (header-only)
// 설치 위치: mac/chernobyl/src/utils/SelfRepair.h
//   ※ windows/src/utils/SelfRepair.h 와 '같은 파일이어야 한다' 는 뜻이 아니다.
//     실제로 두 판은 갈라져 있다 — 맥 쪽엔 서명 자동복구가, 윈도우 쪽엔
//     도구 자동 갱신·실기능 확인이 따로 들어갔다. 전에 여기 "동일" 이라고
//     적혀 있었고, 그것을 믿고 한쪽만 고치면 다른 쪽은 조용히 낡는다.
//     어디가 갈라졌는지는 `python scripts/port_parity.py` 가 매번 말해 준다.
//
// 목적:
//   1) 시작 시 번들 도구(yt-dlp/ffmpeg/exiftool/rclone/python) 존재·실행 자가진단
//   2) 고장 감지 시 자동 복구 — 손상된 사용자 복사본 삭제 → 번들본 재복사 + 실행권한 복원
//   3) 잔존 상태 정리 — 캡쳐 Chrome 프로필 stale lock 제거, temp 재생성
//   4) 남은 문제는 로컬 LLM(OpenAI 호환: Ollama/LM Studio/llama.cpp server,
//      또는 앱 번들 llm/ 폴더의 llama-server)에 보고서를 보내 원인·조치를 진단
//   5) 결과를 AppData/selfrepair/last_report.txt 에 기록
//
// 사용법 (main.cpp):
//   #include "utils/SelfRepair.h"
//   ...
//   SelfRepair::runStartupMaintenanceAsync();   // window.show() 직후 1줄
//
// 정직한 한계: LLM 은 *진단과 조치 안내*까지만 한다. 컴파일된 C++ 를 런타임에
// 스스로 고칠 수는 없다. 실제 자동 "수리"는 2)~3) 의 결정론적 복구 루틴이
// 수행하고, LLM 은 복구 불가 항목의 원인 분석을 보탠다.
// 의존성: Qt Core + Network, 그리고 core/Common.h 의 ansiSafePath 하나.
//   (exiftool 은 argv 를 시스템 ANSI 코드페이지로 받는다. 경로를 앱 본체와 똑같이
//    다뤄야 "실제로는 되는데 자가진단만 [FAIL]" 같은 오경보가 안 난다. 맥에서는 no-op.)
// header-only (Q_OBJECT 없음) → CMakeLists 수정 불필요.
// ═════════════════════════════════════════════════════════════════════════

#include <QAtomicInt>
#include <QCoreApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPointer>
#include <QProcess>
#include <QStandardPaths>
#include <QString>
#include <QStringList>
#include <QThread>
#include <QTimer>
#include <QDebug>

#include "core/Common.h"   // ansiSafePath — 앱 본체와 같은 방식으로 exiftool 경로를 넘기기 위해

namespace SelfRepair {

struct ToolStatus {
    QString name;
    QString path;      // 최종 해석된 경로 (빈 값 = 미발견)
    bool    found = false;
    bool    runs  = false;
    QString version;
    QString error;
};

// ── 경로 해석 (번들 우선 — "모듈 경로는 앱 내부") ───────────────────────

inline QString appDir() { return QCoreApplication::applicationDirPath(); }

inline QString resourcesDir()
{
#ifdef Q_OS_MACOS
    return appDir() + "/../Resources";
#else
    return appDir();
#endif
}

inline QString userToolsDir()
{
    QString p = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/tools";
    QDir().mkpath(p);
    return p;
}

inline QString exeSuffix()
{
#ifdef Q_OS_WIN
    return QStringLiteral(".exe");
#else
    return QString();
#endif
}

// 도구별 후보 경로 — 항상 앱 내부(번들) 우선, 시스템은 마지막 폴백.
inline QStringList toolCandidates(const QString &name)
{
    const QString sfx = exeSuffix();
    QStringList c;
    if (name == "yt-dlp") {
        c << userToolsDir() + "/yt-dlp" + sfx;          // 자동 업데이트본
        c << appDir() + "/yt-dlp" + sfx;                // mac: Contents/MacOS
        c << resourcesDir() + "/tools/yt-dlp" + sfx;    // 번들 tools/
    } else if (name == "python") {
#ifdef Q_OS_WIN
        c << resourcesDir() + "/python_env/python.exe";
#else
        c << resourcesDir() + "/python_env/bin/python3";
#endif
    } else {  // ffmpeg / exiftool / rclone 공통 패턴
        c << appDir() + "/" + name + sfx;
        c << resourcesDir() + "/tools/" + name + sfx;
        c << resourcesDir() + "/tools/" + name + "/" + name + sfx;
    }
#ifndef Q_OS_WIN
    c << "/opt/homebrew/bin/" + name << "/usr/local/bin/" + name;
#endif
    return c;
}

inline QString resolveTool(const QString &name)
{
    const QStringList cands = toolCandidates(name);
    for (const QString &p : cands)
        if (QFileInfo(p).isFile()) return p;   // ★ 디렉토리 제외 — exiftool 후보 'tools/exiftool'(폴더)가
    return QString();                          //   매칭돼 perl 이 폴더를 스크립트로 열려다 실패하던 문제
}

// ── 자가진단 ─────────────────────────────────────────────────────────────

inline QStringList versionArgs(const QString &name)
{
    if (name == "ffmpeg")   return {"-version"};
    if (name == "exiftool") return {"-ver"};
    if (name == "rclone")   return {"version"};
    return {"--version"};   // yt-dlp, python
}

// 실행에 쓸 경로. 맥에서는 그대로다.
//   윈도우 판에는 여기서 ANSI 안전 경로로 바꾸는 처리가 있다(한글 경로 때문).
//   맥에는 그 문제가 없지만 이름을 같게 둔다 — 다음에 무언가를 옮길 때
//   양쪽 모양이 다르면 그 자리에서 또 갈라진다.
inline QString launchPath(const QString &name, const QString &path)
{
    Q_UNUSED(name);
    return path;
}

inline ToolStatus checkTool(const QString &name)
{
    ToolStatus st; st.name = name;
    st.path = resolveTool(name);
    if (st.path.isEmpty()) { st.error = "not found (bundle/user/system)"; return st; }
    st.found = true;

    QProcess p;
#ifndef Q_OS_WIN
    if (name == "exiftool") {
        // exiftool 은 perl 스크립트 — 앱도 perl 로 실행한다. 스크립트를 직접 execve 하면
        // 서명 직후 첫 실행에서 Gatekeeper 평가로 'execve: Permission denied' 위양성이 나
        // 매 실행마다 LLM 진단을 불필요하게 스폰했다. perl 경유로 실제 사용과 일치시킨다.
        p.start("/usr/bin/perl", QStringList() << st.path << versionArgs(name));
    } else
#endif
    // ★ 앱 본체(Common::addExifMetadata)와 같은 경로 처리를 쓴다. exiftool 은 argv 를
    //   ANSI 로 받으므로, 사용자 이름이 한글·일본어인 Windows 설치 경로에서는 8.3 단축
    //   경로가 필요하다. 여기서만 원본을 넘기면 진단만 실패하는 오경보가 난다. (맥은 no-op)
    p.start(Common::ansiSafePath(st.path), versionArgs(name));
    if (!p.waitForStarted(4000)) { st.error = "failed to start: " + p.errorString(); return st; }
    if (!p.waitForFinished(20000)) { p.kill(); st.error = "version check timeout"; return st; }
    const QString out = QString::fromUtf8(p.readAllStandardOutput()
                                          + p.readAllStandardError()).trimmed();
    // ★ exitStatus() 는 정상종료/크래시만 구분한다 — 종료 코드 1로 죽어도 NormalExit 이다.
    //   코드를 같이 보지 않아, 에러를 뱉고 실패한 도구가 [OK] 로 보고되고 그 에러문이
    //   '버전'으로 찍혔다 (한글 경로에서 perl5*.dll 을 못 찾는 Windows exiftool 이 실제 사례).
    //   저장소의 다른 QProcess 검사들과 같은 판정식으로 맞춘다.
    const int code = p.exitCode();
    st.runs = (p.exitStatus() == QProcess::NormalExit && code == 0 && !out.isEmpty());
    st.version = st.runs ? out.section('\n', 0, 0).left(120) : QString();
    if (!st.runs)
        st.error = QStringLiteral("abnormal exit (code %1)%2").arg(code)
                       .arg(out.isEmpty() ? QString()
                                          : ": " + out.section('\n', 0, 0).left(160));
    return st;
}

// ── 자동 복구 ────────────────────────────────────────────────────────────

inline void makeExecutable(const QString &path)
{
    QFile::setPermissions(path,
        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
        QFileDevice::ReadGroup | QFileDevice::ExeGroup |
        QFileDevice::ReadOther | QFileDevice::ExeOther);
}

// 손상된 사용자 복사본 삭제 → 번들본 재복사 + 실행권한 복원 → 재검증.
inline bool repairTool(ToolStatus &st)
{
    // 실행 안 되는 사용자 복사본 제거
    const QString userCopy = userToolsDir() + "/" + st.name + exeSuffix();
    if (!st.runs && st.path == userCopy)
        QFile::remove(userCopy);

    // 번들본 탐색 (사용자 폴더 제외)
    QString bundled;
    const QStringList cands = toolCandidates(st.name);
    for (const QString &p : cands) {
        if (p.startsWith(userToolsDir())) continue;
        if (QFile::exists(p)) { bundled = p; break; }
    }
    if (bundled.isEmpty()) return false;

    // 실행권한 복원 (dmg 복사·zip 해제 시 권한이 잘리는 사례 복구)
    makeExecutable(bundled);

    // 재검사
    ToolStatus re = checkTool(st.name);
    if (re.runs) { st = re; return true; }

    // 그래도 안 되면 사용자 폴더로 복사해 재시도
    QFile::remove(userCopy);
    if (QFile::copy(bundled, userCopy)) {
        makeExecutable(userCopy);
        re = checkTool(st.name);
        if (re.runs) { st = re; return true; }
    }
    return false;
}

// 잔존 상태 정리 — 캡쳐 Chrome 프로필 stale lock, temp 폴더
inline QStringList cleanStaleState()
{
    QStringList cleaned;
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    // Chrome 캡쳐 프로필 SingletonLock 제거 (잔존 Chrome 핸드오프-즉시종료 버그 완화)
    const QStringList profileDirs = { base + "/chrome_capture_profile",
                                      QDir::homePath() + "/chrome_capture_profile" };
    for (const QString &d : profileDirs) {
        if (!QDir(d).exists()) continue;
        const QStringList locks = {QStringLiteral("SingletonLock"),
                                   QStringLiteral("SingletonSocket"),
                                   QStringLiteral("SingletonCookie")};
        for (const QString &f : locks) {
            const QString p = d + "/" + f;
            if (QFile::exists(p) && QFile::remove(p)) cleaned << p;
        }
    }
    // 앱 전용 temp 재생성
    QDir().mkpath(base + "/temp");
    return cleaned;
}

// ── 로컬 LLM 진단 (OpenAI 호환 API) ─────────────────────────────────────

inline QByteArray httpGet(const QString &url, int timeoutMs)
{
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    QNetworkReply *rep = nam.get(req);
    QEventLoop loop;
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QByteArray out = (rep->isFinished() && rep->error() == QNetworkReply::NoError)
                     ? rep->readAll() : QByteArray();
    rep->abort(); rep->deleteLater();
    return out;
}

inline QByteArray httpPostJson(const QString &url, const QByteArray &body, int timeoutMs)
{
    QNetworkAccessManager nam;
    QNetworkRequest req{QUrl(url)};
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    QNetworkReply *rep = nam.post(req, body);
    QEventLoop loop;
    QTimer::singleShot(timeoutMs, &loop, &QEventLoop::quit);
    QObject::connect(rep, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QByteArray out = (rep->isFinished() && rep->error() == QNetworkReply::NoError)
                     ? rep->readAll() : QByteArray();
    rep->abort(); rep->deleteLater();
    return out;
}

// 번들 LLM 서버 기동 — <Resources>/llm/llama-server + *.gguf 가 있으면 스폰
inline bool spawnBundledLlm(int port)
{
    // ★ 앱 세션당 1회만 스폰 — 모델 로딩 중(503)을 죽은 것으로 오판해
    //   서버를 중복 기동하는 사고 방지 (동시 진입 대비 atomic).
    static QAtomicInt spawned{0};
    if (!spawned.testAndSetOrdered(0, 1)) return false;

    const QString dir = resourcesDir() + "/llm";
    QString server = dir + "/llama-server" + exeSuffix();
    if (!QFile::exists(server)) { spawned.storeRelease(0); return false; }
    // ★ 여러 모델 지원 — llm/ 의 *.gguf 중 '모델 헤드'만 나열(분할 파일은 00001-of 만 진입점).
    const QStringList allGgufs = QDir(dir).entryList({"*.gguf"}, QDir::Files, QDir::Name);
    QStringList heads;
    for (const QString &g : allGgufs) {
        const int ofIdx = g.indexOf("-of-");
        if (ofIdx >= 5) {
            const QString part = g.mid(ofIdx - 5, 5);
            bool digits = (part.size() == 5);
            for (const QChar &c : part) if (!c.isDigit()) digits = false;
            if (digits && part != QLatin1String("00001")) continue;  // 분할 continuation 파트 제외
        }
        heads << g;
    }
    if (heads.isEmpty()) { spawned.storeRelease(0); return false; }
    // 선택: env CHERNOBYL_LLM_MODEL(부분일치) > 첫 헤드(알파벳순=가장 작은=빠른 기본).
    QString chosen = heads.first();
    const QString wantModel = qEnvironmentVariable("CHERNOBYL_LLM_MODEL");
    if (!wantModel.isEmpty())
        for (const QString &h : heads)
            if (h.contains(wantModel, Qt::CaseInsensitive)) { chosen = h; break; }
    qInfo().noquote() << "[SelfRepair] 번들 LLM" << heads.size() << "개 모델 중 선택:" << chosen
                      << "(env CHERNOBYL_LLM_MODEL 로 전환 가능)";
    makeExecutable(server);
    qint64 pid = 0;
    const bool ok = QProcess::startDetached(server,
        {"-m", dir + "/" + chosen, "--port", QString::number(port),
         "--host", "127.0.0.1", "-c", "4096"}, dir, &pid);
    if (ok && pid > 0) {
        // ★ 앱 종료 시 같이 종료 — 수 GB 모델을 든 고아 프로세스 잔존 방지.
        //   (functor-only connect 는 emit 스레드(메인)에서 실행되어 워커에서 걸어도 안전)
        QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit, [pid]() {
#ifdef Q_OS_WIN
            QProcess::execute("taskkill", {"/F", "/PID", QString::number(pid)});
#else
            QProcess::execute("/bin/kill", {"-TERM", QString::number(pid)});
#endif
        });
    }
    if (!ok) spawned.storeRelease(0);
    return ok;
}

// 살아있는 OpenAI 호환 엔드포인트 탐색. env CHERNOBYL_LLM_ENDPOINT 최우선.
inline QString findLlmEndpoint()
{
    QStringList bases;
    const QString envEp = qEnvironmentVariable("CHERNOBYL_LLM_ENDPOINT");
    if (!envEp.isEmpty()) bases << envEp;
    bases << "http://127.0.0.1:11434"    // Ollama
          << "http://127.0.0.1:1234"     // LM Studio
          << "http://127.0.0.1:8080"     // llama.cpp server
          << "http://127.0.0.1:8737";    // 번들 llama-server (spawnBundledLlm)
    for (const QString &b : bases)
        if (!httpGet(b + "/v1/models", 700).isEmpty()) return b;
    // 아무것도 없으면 번들 LLM 스폰 시도 후 재확인
    if (spawnBundledLlm(8737)) {
        for (int i = 0; i < 20; ++i) {
            QThread::msleep(500);
            if (!httpGet("http://127.0.0.1:8737/v1/models", 700).isEmpty())
                return QStringLiteral("http://127.0.0.1:8737");
        }
    }
    return QString();
}

inline QString llmDiagnose(const QString &reportText)
{
    const QString base = findLlmEndpoint();
    if (base.isEmpty()) return QString();

    // 모델명: /v1/models 첫 항목 (Ollama 는 실제 모델명 필수)
    QString model = QStringLiteral("default");
    const QJsonDocument md = QJsonDocument::fromJson(httpGet(base + "/v1/models", 1500));
    const QJsonArray data = md.object().value("data").toArray();
    if (!data.isEmpty()) model = data.first().toObject().value("id").toString("default");

    QJsonObject sys{{"role", "system"},
        {"content", "당신은 Hanishiki 데스크톱 앱의 유지보수 진단가다. "
                    "아래 자가진단 보고서를 읽고, 실패 항목의 가장 유력한 원인과 "
                    "사용자가 취할 구체적 조치를 한국어 5줄 이내로 답하라."}};
    QJsonObject usr{{"role", "user"}, {"content", reportText}};
    QJsonObject body{{"model", model},
                     {"messages", QJsonArray{sys, usr}},
                     {"temperature", 0.2},
                     {"max_tokens", 400}};
    const QByteArray resp = httpPostJson(base + "/v1/chat/completions",
                                         QJsonDocument(body).toJson(QJsonDocument::Compact),
                                         30000);
    if (resp.isEmpty()) return QString();
    return QJsonDocument::fromJson(resp).object()
        .value("choices").toArray().first().toObject()
        .value("message").toObject().value("content").toString().trimmed();
}


// ── 앱 상태 점검 ─────────────────────────────────────────────────────────
//   ★ 예전 진단서는 도구 5개(yt-dlp/ffmpeg/python/exiftool/rclone)만 봤다.
//     그런데 오늘 실제로 사람을 막은 고장들은 전부 그 밖에 있었다:
//       파이썬 패키지 누락(수집이 통째로 죽는다) · AI 엔진 미설치 ·
//       색인 없음 · 설정 파일 권한이 열림 · 디스크 여유 부족
//     진단서에 안 나오니 자가수리가 "이상 없음" 이라고 답할 수밖에 없었다.
//     고칠 수 있는지와 별개로, '보이기는 해야' 사람이 다음 수를 둔다.
// ── 실제로 되는지 본다 (smoke test) ─────────────────────────────────────────
//
// 왜 필요한가.
//   버전이 찍히는지만 보는 검사는 '낡아서 안 되는' 고장을 원리적으로 못 잡는다.
//   유튜브를 통째로 세운 yt-dlp 2026.07.04 도 --version 은 멀쩡히 찍혔다.
//   그래서 도구마다 '가장 작은 진짜 작업' 을 한 번 시켜 본다.
//   (윈도우 판에 있던 것을 옮겼다 — scripts/port_parity.py 가 짚어 준 격차다)

enum SmokeVerdict { SmokeSkip = 0, SmokePass, SmokeFail };

struct SmokeResult {
    SmokeVerdict verdict = SmokeSkip;
    QString      detail;
};

inline SmokeResult smokePass(const QString &d) { SmokeResult r; r.verdict = SmokePass; r.detail = d; return r; }
inline SmokeResult smokeFail(const QString &d) { SmokeResult r; r.verdict = SmokeFail; r.detail = d; return r; }
inline SmokeResult smokeSkip(const QString &d) { SmokeResult r; r.verdict = SmokeSkip; r.detail = d; return r; }

inline QString smokeDir()
{
    const QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                      + "/selfrepair/smoke";
    QDir().mkpath(d);
    return d;
}

// ── exiftool: 진짜로 태그를 쓰고 되읽는다 ────────────────────────────────
//   시험용 그림을 어디서 받아 오면 그 다운로드 자체가 새 고장 요인이 된다.
//   그래서 1x1 JPEG(160바이트)를 코드 안에 넣어 둔다 — 바깥에 의존하지 않는다.
//   값에는 일부러 한글·일본어를 넣는다. exiftool.exe 는 argv 를 시스템 ANSI 로
//   받으므로 여기서 뭉개지는 것이 실제 고장 지점이었다.
inline SmokeResult smokeExiftool(const QString &exe)
{
    static const char kJpeg1x1[] =
        "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRof"
        "Hh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAFAAB"
        "AAAAAAAAAAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAD8AKp//2Q==";

    const QString dir = smokeDir();
    const QString jpg = dir + "/exif_probe.jpg";
    QFile::remove(jpg);
    {
        QFile f(jpg);
        if (!f.open(QIODevice::WriteOnly)) return smokeFail("검사용 그림 파일을 만들지 못했습니다");
        f.write(QByteArray::fromBase64(QByteArray(kJpeg1x1)));
    }

    const QString marker = QStringLiteral("자가진단-テスト-")
                           + QString::number(QDateTime::currentSecsSinceEpoch());

    // 앱 본체(Common::addExifMetadata)와 똑같은 방식 — UTF-8 argfile + -charset.
    // 다른 방식으로 시험하면 '시험은 되는데 앱은 안 되는' 상태를 못 잡는다.
    auto runWithArgs = [&](const QStringList &lines, QByteArray *out) -> bool {
        const QString argPath = dir + "/exif_probe.args";
        QFile a(argPath);
        if (!a.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
        {
            QTextStream ts(&a);
            ts.setEncoding(QStringConverter::Utf8);
            ts << "-charset\nfilename=UTF8\n-charset\nUTF8\n";
            for (const QString &l : lines) ts << l << "\n";
        }
        a.close();
        QProcess p;
        p.setProcessEnvironment(Common::bundledProcessEnv());
        p.start(launchPath(QStringLiteral("exiftool"), exe),
                QStringList() << "-@" << Common::ansiSafePath(argPath));
        if (!p.waitForStarted(5000)) return false;
        if (!p.waitForFinished(20000)) { p.kill(); return false; }
        if (out) *out = p.readAllStandardOutput();
        return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
    };

    if (!runWithArgs({"-overwrite_original", "-ImageDescription=" + marker, jpg}, nullptr))
        return smokeFail("태그를 쓰지 못했습니다");

    QByteArray back;
    if (!runWithArgs({"-s", "-s", "-s", "-ImageDescription", jpg}, &back))
        return smokeFail("쓴 태그를 되읽지 못했습니다");

    const QString got = QString::fromUtf8(back).trimmed();
    if (got != marker)
        return smokeFail(QString("되읽은 값이 다릅니다 (쓴 값 %1 / 읽은 값 %2)")
                             .arg(marker, got.isEmpty() ? QStringLiteral("(빈 값)") : got));
    QFile::remove(jpg);
    return smokePass("한글·일본어 태그 쓰기·되읽기 확인");
}

// ── ffmpeg: 실제로 인코딩해 파일을 만든다 ────────────────────────────────
//   앱이 쓰는 두 경로를 그대로 시킨다 — 우고이라 GIF(palettegen/paletteuse)와
//   mp4 먹싱. 코덱 하나만 빠져도 여기서 드러난다.
inline SmokeResult smokeFfmpeg(const QString &exe)
{
    const QString dir = smokeDir();
    auto make = [&](const QString &outFile, const QStringList &extra) -> qint64 {
        QFile::remove(outFile);
        QProcess p;
        p.setProcessEnvironment(Common::bundledProcessEnv());
        QStringList args;
        args << "-hide_banner" << "-loglevel" << "error" << "-nostdin" << "-y"
             << "-f" << "lavfi" << "-i" << "testsrc=size=32x32:rate=10:duration=0.3";
        args += extra;
        args << outFile;
        p.start(exe, args);
        if (!p.waitForStarted(5000)) return -1;
        if (!p.waitForFinished(60000)) { p.kill(); return -1; }
        if (p.exitCode() != 0) return -1;
        return QFileInfo(outFile).size();
    };

    const qint64 gif = make(dir + "/ff_probe.gif",
        {"-vf", "split[a][b];[a]palettegen=max_colors=32[p];[b][p]paletteuse"});
    if (gif <= 0) return smokeFail("GIF 변환이 되지 않습니다 (우고이라 변환이 쓰는 경로)");

    const qint64 mp4 = make(dir + "/ff_probe.mp4", {"-c:v", "mpeg4", "-pix_fmt", "yuv420p"});
    if (mp4 <= 0) return smokeFail("mp4 만들기가 되지 않습니다 (유튜브 영상·음성 합치기가 쓰는 경로)");

    QFile::remove(dir + "/ff_probe.gif");
    QFile::remove(dir + "/ff_probe.mp4");
    return smokePass(QString("GIF %1B · mp4 %2B 생성 확인").arg(gif).arg(mp4));
}

// ── python: 앱 스크립트가 실제로 import 하는 것들을 불러 본다 ────────────
//   pip 환경이 조용히 깨지는 것은 오래 방치한 앱에서 가장 흔한 고장이다.
//   한글 출력도 같이 본다 — 예전에 자식 파이썬이 한글을 찍다가 죽었다.
inline SmokeResult smokePython(const QString &exe)
{
    const QString dir = smokeDir();
    const QString py  = dir + "/py_probe.py";
    {
        QFile f(py);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
            return smokeFail("검사용 스크립트를 만들지 못했습니다");
        QTextStream ts(&f);
        ts.setEncoding(QStringConverter::Utf8);
        ts << "# -*- coding: utf-8 -*-\n"
              "import sys, importlib\n"
              "mods = ['ssl','sqlite3','json','twikit','httpx','atproto','openpyxl',\n"
              "        'PIL','piexif','bs4','lxml','websockets','m3u8','browser_cookie3',\n        'x_client_transaction']\n"
              "missing = []\n"
              "for m in mods:\n"
              "    try: importlib.import_module(m)\n"
              "    except Exception: missing.append(m)\n"
              "try: sys.stdout.reconfigure(encoding='utf-8')\n"
              "except Exception: pass\n"
              "print('KOREAN:자가진단')\n"
              "print('MISSING:' + ','.join(missing))\n";
    }

    QProcess p;
    p.setProcessEnvironment(Common::bundledProcessEnv());
    p.start(exe, QStringList() << py);
    if (!p.waitForStarted(5000)) return smokeFail("파이썬을 실행하지 못했습니다");
    if (!p.waitForFinished(120000)) { p.kill(); return smokeFail("파이썬이 응답하지 않습니다"); }
    const QString out = QString::fromUtf8(p.readAllStandardOutput());

    if (!out.contains(QStringLiteral("KOREAN:자가진단")))
        return smokeFail("한글 출력이 깨집니다 (수집 스크립트가 같은 자리에서 죽는다)");

    QString missing;
    for (const QString &line : out.split('\n'))
        if (line.startsWith("MISSING:")) missing = line.mid(8).trimmed();
    if (!missing.isEmpty())
        return smokeFail("빠진 모듈: " + missing + " — 설정 → 모듈 업데이트로 받으세요");
    return smokePass("필수 모듈 15개 import · 한글 출력 확인");
}

// ── 인터넷에 닿는지 ──────────────────────────────────────────────────────
//   실패를 '도구 고장' 으로 적기 전에 반드시 확인한다.
inline bool internetReachable()
{
    return !httpGet(QStringLiteral("https://www.youtube.com/robots.txt"), 5000).isEmpty();
}

// ── yt-dlp: 실제로 미디어 바이트를 받아 본다 ─────────────────────────────
//   형식 목록만 보는 검사는 2026.07.04 고장을 못 잡는다 — 목록은 나왔고
//   본문만 403 이었다. 그래서 '받아진다' 를 직접 확인한다.
//   기준 영상은 사라질 가능성이 가장 낮은 둘을 쓰고, 둘 다 실패했을 때만
//   네트워크를 의심한다.
inline SmokeResult smokeYtDlp(const QString &exe)
{
    // 기준 영상 — 사라질 가능성이 가장 낮은 둘을 쓴다.
    //   그래도 10년 뒤에는 모른다. 그때 코드를 못 고치는 사람도 쓸 수 있도록
    //   환경변수로 갈아끼울 수 있게 열어 둔다 (쉼표로 여러 개).
    //   1년을 방치할 앱이라면 '내가 박아 둔 상수' 도 언젠가 틀린다고 봐야 한다.
    QStringList urls;
    const QString custom = qEnvironmentVariable("HANISHIKI_SMOKE_YT");
    if (!custom.isEmpty()) urls = custom.split(',', Qt::SkipEmptyParts);
    else urls << QStringLiteral("https://www.youtube.com/watch?v=jNQXAC9IVRw")   // 2005년, 유튜브 첫 영상
              << QStringLiteral("https://www.youtube.com/watch?v=BaW_jenozKc");  // yt-dlp 가 자기 시험에 쓰는 영상

    const qint64 kNeed = 32768;   // 32KB 면 '본문이 흐른다' 는 증거로 충분하다

    QString lastErr;
    for (const QString &url : urls) {
        QProcess p;
        p.setProcessEnvironment(Common::bundledProcessEnv());
        p.start(launchPath(QStringLiteral("yt-dlp"), exe), QStringList()
                << "--no-warnings" << "--no-progress" << "--no-playlist"
                << "-f" << "worstaudio/worst" << "-o" << "-" << url.trimmed());
        if (!p.waitForStarted(8000)) { lastErr = p.errorString(); continue; }

        qint64 got = 0;
        QElapsedTimer t; t.start();
        while (t.elapsed() < 90000 && got < kNeed) {
            if (!p.waitForReadyRead(3000)) {
                if (p.state() != QProcess::Running) break;
                continue;
            }
            got += p.readAllStandardOutput().size();
        }
        const QString err = QString::fromUtf8(p.readAllStandardError()).trimmed();
        p.kill();
        p.waitForFinished(5000);

        if (got >= kNeed)
            return smokePass(QString("영상 데이터 %1KB 수신 확인").arg(got / 1024));
        lastErr = err.section('\n', -1).left(160);
    }

    if (!internetReachable())
        return smokeSkip("인터넷에 닿지 않아 건너뜁니다 (도구 문제가 아닙니다)");
    return smokeFail(QStringLiteral("영상 데이터를 받지 못했습니다")
                     + (lastErr.isEmpty() ? QString() : " — " + lastErr));
}

// ── 꾸러미 신선도: 낡은 것을 찾아 스스로 맞춘다 ────────────────────────────
//
// 왜 필요한가 (실측).
//   2026-08 에 유튜브 다운로드가 통째로 멈췄다. 코드 버그가 아니었다.
//   배포본에 딸려 온 yt-dlp 2026.07.04 가 유튜브의 변경을 못 따라가 미디어 요청이
//   전부 403 으로 막혔다(형식 목록은 나오는데 본문만 거부). 2026.08.19 로 바꾸니
//   같은 영상을 2초에 받았다. 두 달 만에 정지한 것이다.
//
//   기존 자가진단은 이 고장을 원리적으로 못 잡는다 — 낡은 yt-dlp 도 --version 은
//   멀쩡히 출력하므로 [OK] 로 통과한다. 게다가 고장 시 '번들본 재복사' 를 하는데,
//   그것은 더 낡은 쪽으로 되돌리는 일이다. 낡음에 무방비일 뿐 아니라 낡는 쪽으로 민다.
//
// 어떻게 하는가.
//   · 번들을 직접 고치지 않는다. pip 로 번들 python_env 에 설치하면 codesign 봉인이
//     깨지고, 재서명이 실패하면 앱 자체가 안 뜬다. 갱신하려다 앱을 못 쓰게 되는 것은
//     갱신을 안 하느니만 못하다.
//   · 쓰기 가능한 덧씌우기 폴더에 새 판을 깔고 PYTHONPATH 로 먼저 읽게 한다.
//     번들은 그대로라 봉인이 유지되고, 덧씌운 것을 지우면 즉시 원래대로 돌아온다.
//   · 갱신 뒤 실제로 불러 보고, 안 되면 지워서 되돌린다.
//     새 것이 망가졌는데 그대로 두면 자동 갱신은 없느니만 못하다.
//   · 대상은 '바깥 서비스를 따라가야 하는 것' 만. 나머지는 낡아도 멈추지 않는다.

inline QString freshStampPath()
{
    const QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                      + "/selfrepair";
    QDir().mkpath(d);
    return d + "/last_pkg_check.txt";
}

inline bool freshCheckDue(int everyDays)
{
    QFile f(freshStampPath());
    if (!f.open(QIODevice::ReadOnly)) return true;
    const qint64 last = QString::fromUtf8(f.readAll()).trimmed().toLongLong();
    f.close();
    if (last <= 0) return true;
    return (QDateTime::currentSecsSinceEpoch() - last) >= qint64(everyDays) * 86400;
}

inline void markFreshChecked()
{
    QFile f(freshStampPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QByteArray::number(QDateTime::currentSecsSinceEpoch()));
}

// 낡으면 '느려지는' 게 아니라 '안 되는' 것들. 목록을 좁게 잡는다 —
// 전부 최신으로 밀면 멀쩡하던 것이 깨질 위험이 오히려 커진다.
inline QStringList serviceFollowingPackages()
{
    return {"yt-dlp", "twikit", "atproto", "discord.py", "browser_cookie3"};
}

inline QString updatePackagesIfDue(const QString &python, bool allowNetwork,
                                   int everyDays = 3, bool force = false)
{
    if (!allowNetwork) return QString();
    if (!force && !freshCheckDue(everyDays)) return QString();
    if (python.isEmpty() || !QFile::exists(python)) return QString();

    const QString overlay = Common::userPyOverlayDir();
    QString out;
    int updated = 0, failed = 0;

    for (const QString &pkg : serviceFollowingPackages()) {
        // 지금 실제로 쓰이는 판 (덧씌우기가 있으면 그것이 잡힌다)
        auto versionNow = [&]() -> QString {
            QProcess q;
            QProcessEnvironment e = Common::bundledProcessEnv();
            q.setProcessEnvironment(e);
            q.start(python, {"-c",
                "import importlib.metadata as m,sys\n"
                "try: print(m.version(sys.argv[1]))\n"
                "except Exception: print('')", pkg});
            q.waitForFinished(20000);
            return QString::fromUtf8(q.readAllStandardOutput()).trimmed();
        };
        const QString before = versionNow();

        // 최신으로 덧씌운다. --target 이라 번들은 손대지 않는다.
        QProcess pip;
        pip.setProcessEnvironment(Common::bundledProcessEnv());
        pip.start(python, {"-m", "pip", "install", "--quiet", "--upgrade",
                           "--target", overlay, "--no-input", pkg});
        if (!pip.waitForFinished(300000)) { pip.kill(); pip.waitForFinished(3000); continue; }
        if (pip.exitCode() != 0) continue;      // 오프라인 등 — 다음 기회에 다시

        const QString after = versionNow();
        if (after.isEmpty() || after == before) continue;   // 바뀐 게 없으면 조용히 넘어간다

        // ★ 갱신했으면 반드시 '실제로 되는지' 본다. 안 되면 되돌린다.
        QProcess chk;
        chk.setProcessEnvironment(Common::bundledProcessEnv());
        const QString mod = QString(pkg).replace('-', '_');
        chk.start(python, {"-c", QString("import %1").arg(mod)});
        chk.waitForFinished(30000);
        if (chk.exitStatus() != QProcess::NormalExit || chk.exitCode() != 0) {
            // 덧씌운 것만 지우면 번들의 옛 판으로 즉시 돌아간다.
            QDir(overlay).removeRecursively();
            QDir().mkpath(overlay);
            out += QString("[UPD]  %1 %2 → %3 이 불러오기에 실패해 되돌렸습니다\n")
                       .arg(pkg, before.isEmpty() ? "?" : before, after);
            ++failed;
            continue;
        }
        out += QString("[UPD]  %1 %2 → %3 으로 맞췄습니다\n")
                   .arg(pkg, before.isEmpty() ? "(없음)" : before, after);
        ++updated;
    }

    markFreshChecked();
    if (updated == 0 && failed == 0)
        return QStringLiteral("[UPD]  꾸러미 신선도 확인 — 맞출 것이 없었습니다\n");
    return out;
}

inline QString checkEnvironment()
{
    QString out;
    const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    // 1) 파이썬 패키지 — 하나만 빠져도 그 플랫폼 수집이 통째로 죽는다.
    {
        const QString py = Common::bundledPythonPath();
        if (QFile::exists(py)) {
            QProcess pc;
            pc.setProcessEnvironment(Common::bundledProcessEnv());
            pc.start(py, {"-c",
                "import importlib.util as u\n"
                "mods=['twikit','httpx','atproto','openpyxl','PIL','piexif',"
                "'browser_cookie3','bs4','websockets','lxml','m3u8','cryptography']\n"
                "miss=[m for m in mods if u.find_spec(m) is None]\n"
                "print(','.join(miss))"});
            pc.waitForFinished(20000);
            const QString miss = QString::fromUtf8(pc.readAllStandardOutput()).trimmed();
            if (!miss.isEmpty())
                out += "[FAIL] 파이썬 패키지 없음: " + miss + " — 수집이 실패합니다\n";
            else
                out += "[OK]   파이썬 패키지 12종 정상\n";
        }
    }

    // ★ '없다' 고 말하기 전에 한 단계 아래를 본다.
    //   이사(앱 이름이 네 번 바뀌었다)를 하다 보면 옛 데이터 한 벌이 통째로
    //   하위 폴더에 남는 일이 실제로 있었다. 그때 진단서는 "AI 엔진 미설치 —
    //   'AI 설치' 를 실행하세요" 라고 안내했는데, 정작 9.4GB 짜리 모델이 바로
    //   한 칸 아래 있었다. 그대로 따랐으면 있는 것을 다시 내려받았을 것이다.
    //   그러니 없으면 '정말 없는지' 부터 확인하고, 있으면 그 자리를 알려 준다.
    auto findOrphan = [&appData](const QString &relative) -> QString {
        const QStringList subs = QDir(appData).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QString &sub : subs) {
            const QString cand = appData + "/" + sub + "/" + relative;
            if (QFileInfo::exists(cand)) return appData + "/" + sub;
        }
        return QString();
    };

    // 2) AI 엔진·모델 — 없으면 자가수리·보관함 질의가 아예 못 돈다.
    {
        const QString llm = appData + "/llm";
        const bool srv = QFile::exists(llm + "/llama-server");
        const int models = QDir(llm).entryList(QStringList() << "*.gguf", QDir::Files).size();
        if (!srv || models == 0) {
            const QString orphan = findOrphan("llm/llama-server");
            if (!orphan.isEmpty()) {
                const int om = QDir(orphan + "/llm").entryList(QStringList() << "*.gguf", QDir::Files).size();
                out += QString("[WARN] AI 엔진이 제자리에 없습니다 — 다만 %1 에 한 벌(모델 %2개)이 있습니다.\n"
                               "       다시 내려받지 마시고 그 안의 llm 폴더를 %3 로 옮기십시오.\n")
                           .arg(QDir(orphan).dirName()).arg(om).arg(appData);
            } else {
                out += QString("[WARN] AI 엔진 미설치 (엔진 %1 · 모델 %2개) — 'AI 설치' 를 실행하세요\n")
                           .arg(srv ? "있음" : "없음").arg(models);
            }
        } else {
            out += QString("[OK]   AI 엔진 · 모델 %1개\n").arg(models);
        }
    }

    // 3) 산출물 색인 — 없으면 보관함 질문이 전부 "자료 없음" 이 된다.
    {
        const QFileInfo db(appData + "/archive_index.db");
        if (!db.exists()) {
            const QString orphan = findOrphan("archive_index.db");
            if (!orphan.isEmpty())
                out += QString("[WARN] 산출물 색인이 제자리에 없습니다 — %1 에 있습니다.\n"
                               "       다시 만들지 마시고 archive_index.db 를 %2 로 옮기십시오.\n")
                           .arg(QDir(orphan).dirName()).arg(appData);
            else
                out += "[WARN] 산출물 색인 없음 — 설정에서 '색인 만들기' 를 한 번 실행하세요\n";
        }
        else {
            const int days = db.lastModified().daysTo(QDateTime::currentDateTime());
            out += QString("[OK]   산출물 색인 %1MB (%2일 전 갱신)%3\n")
                       .arg(db.size() / 1024 / 1024).arg(days)
                       .arg(days > 30 ? "  ← 오래됐습니다. 갱신을 권합니다" : "");
        }
    }

    // 4) 설정 파일 권한 — NAS 비밀번호·쿠키·토큰이 들어 있다.
    {
        // 이름이 바뀌는 중이라 둘 다 본다(하나만 보면 '설정 없음' 오경보가 난다).
        QString cfg = appData + "/hanishiki_config.json";
        if (!QFile::exists(cfg)) cfg = appData + "/miyo_config.json";
        if (QFile::exists(cfg)) {
            const QFile::Permissions pm = QFile::permissions(cfg);
            const bool others = pm & (QFile::ReadGroup | QFile::ReadOther);
            out += others
                ? "[WARN] 설정 파일을 다른 사용자도 읽을 수 있습니다 — 저장하면 자동으로 조여집니다\n"
                : "[OK]   설정 파일 권한 (본인만 읽기)\n";
        }
    }

    // 5) 저장 디스크 여유 — 가득 차면 수집이 조용히 반쪽 파일을 남긴다.
    {
        const QString dir = Common::resolveTempBase(QString());
        if (!dir.isEmpty()) {
            QStorageInfo si(dir);
            if (si.isValid() && si.bytesTotal() > 0) {
                const double freeGB = si.bytesAvailable() / (1024.0 * 1024 * 1024);
                const double pct = 100.0 * si.bytesAvailable() / si.bytesTotal();
                out += QString(freeGB < 5 ? "[FAIL] " : (pct < 10 ? "[WARN] " : "[OK]   "))
                     + QString("저장 디스크 여유 %1GB (%2%)\n")
                           .arg(freeGB, 0, 'f', 1).arg(pct, 0, 'f', 0);
            }
        }
    }
    return out;
}

// ── 오케스트레이터 ───────────────────────────────────────────────────────

inline QString runStartupMaintenance()
{
    QString report;
    report += "═ SelfRepair 자가진단 " + QDateTime::currentDateTime().toString(Qt::ISODate) + " ═\n";
    report += "appDir: " + appDir() + "\n";

    const QStringList tools = {"yt-dlp", "ffmpeg", "python", "exiftool", "rclone"};
    int broken = 0;
    for (const QString &t : tools) {
        ToolStatus st = checkTool(t);
        if (!st.runs) {
            repairTool(st);   // 발견 여부와 무관하게 번들 재복사 경로로 복구 시도
            if (st.runs) {
                report += QString("[OK*]  %1 — %2 (자동 복구됨: %3)\n")
                              .arg(t, st.version, st.path);
            } else {
                report += QString("[FAIL] %1 — %2\n").arg(t, st.error);
                ++broken;
            }
        } else {
            report += QString("[OK]   %1 — %2 (%3)\n").arg(t, st.version, st.path);
        }
    }

    // 도구 밖의 상태도 본다 — 실제로 사람을 막는 것은 대부분 여기다.
    // ★ 실제로 되는지 본다 — 버전만 찍히는 검사가 못 잡는 고장이 있다.
    //   유튜브를 세운 낡은 yt-dlp 도 --version 은 멀쩡했다. 도구마다 '가장 작은
    //   진짜 작업' 을 한 번 시켜 보고, 인터넷이 없으면 도구 탓으로 적지 않는다.
    {
        struct { const char *name; SmokeResult (*fn)(const QString &); } smokes[] = {
            {"exiftool", &smokeExiftool},
            {"ffmpeg",   &smokeFfmpeg},
            {"yt-dlp",   &smokeYtDlp},
        };
        for (const auto &sm : smokes) {
            const QString tool = QString::fromUtf8(sm.name);
            const ToolStatus st = checkTool(tool);
            if (!st.runs) {
                // ★ 조용히 넘기지 않는다. 처음엔 continue 만 했더니 yt-dlp 줄이 통째로
                //   사라졌는데, 위쪽 [OK] 에는 멀쩡히 있어서 왜 빠졌는지 알 수가 없었다.
                report += QString("[건너뜀] %1 — 도구를 다시 확인하지 못했습니다%2\n")
                              .arg(tool, st.error.isEmpty() ? QString() : (" (" + st.error + ")"));
                continue;
            }
            const SmokeResult r = sm.fn(st.path);
            // ★ 태그는 한글이다. fromLatin1 로 만들면 깨진다 — 처음에 [실행] 이
            //   [ì¤í] 로 나왔다. 소스가 UTF-8 이므로 fromUtf8 이 맞다.
            const QString tag = (r.verdict == SmokePass) ? QStringLiteral("[실행]")
                              : (r.verdict == SmokeFail) ? QStringLiteral("[실패]")
                                                         : QStringLiteral("[건너뜀]");
            report += QString("%1 %2 — %3\n").arg(tag, tool, r.detail);
        }
        {
            const QString py = Common::bundledPythonPath();
            if (!py.isEmpty() && QFile::exists(py)) {
                const SmokeResult r = smokePython(py);
                const QString tag = (r.verdict == SmokePass) ? QStringLiteral("[실행]")
                                  : (r.verdict == SmokeFail) ? QStringLiteral("[실패]")
                                                             : QStringLiteral("[건너뜀]");
                report += QString("%1 python — %2\n").arg(tag, r.detail);
            }
        }
    }

    report += checkEnvironment();

    // ★ 낡음 확인 — 며칠에 한 번, 바깥 서비스를 따라가야 하는 꾸러미만.
    //   유튜브를 통째로 세운 것이 코드 버그가 아니라 '낡은 꾸러미' 였다.
    //   버전이 찍히는지만 보는 진단은 그 고장을 원리적으로 못 잡는다.
    {
        const QString py = Common::bundledPythonPath();
        const QString upd = updatePackagesIfDue(py, /*allowNetwork=*/true);
        if (!upd.isEmpty()) report += upd;
    }

#ifdef Q_OS_MACOS
    // ── 코드 서명 봉인 자동 복구 ─────────────────────────────────────────────
    //   앱 내부에 pip 설치·스크립트 갱신이 일어나면 codesign 봉인이 깨진다.
    //   깨진 채로도 지금 서명 구성(Apple Development, 하드닝 런타임 없음)에서는
    //   앱이 실행된다 — 실제로 확인했다. 하지만 그대로 두면
    //     · codesign --verify 가 계속 실패하고
    //     · 공증·배포로 넘어가는 순간 막히며
    //     · 하드닝 런타임을 켜는 날 갑자기 실행이 안 된다.
    //   그러니 기동할 때 조용히 확인하고, 깨져 있으면 스스로 다시 서명한다.
    //   재서명은 번들에 동봉한 codesign_app.sh(빌드가 쓰는 그 스크립트)가 한다.
    {
        const QString app = Common::appBundlePath();
        if (!app.isEmpty()) {
            QProcess vf;
            vf.start("/usr/bin/codesign", {"--verify", "--deep", "--strict", app});
            vf.waitForFinished(300000);
            if (vf.exitCode() != 0) {
                // ★ 이 줄은 report 에만 담으면 안 된다. 보고서는 진단이 다 끝난 뒤에야
                //   한꺼번에 출력된다(맨 아래 qInfo). 그런데 재서명은 1분쯤 걸린다.
                //   그동안 화면에도 로그에도 아무 것도 안 뜨니, 사용자는 앱이 그냥
                //   멈춘 줄 알고 끈다. 끄면 재서명이 중간에 잘려 번들이 오히려
                //   무효 상태로 남는다(실측: 시작 9초 → 완주 60초, 중간에 끊기면 무효).
                //   그러니 지금 당장 밖으로 내보낸다.
                qInfo().noquote() << "[SEAL] 코드 서명 봉인이 깨져 있습니다 — 지금 복구합니다."
                                     " 1분쯤 걸립니다. 끝날 때까지 앱을 끄지 마십시오.";
                report += "[SEAL] 코드 서명 봉인이 깨져 있습니다 — 자동 복구를 시도합니다"
                          " (1분쯤 걸립니다 — 끄지 마십시오)…\n";
                QString serr;
                if (Common::resealAppBundle(&serr))
                    report += "[SEAL] ✅ 서명 복구 완료.\n";
                else
                    report += "[SEAL] ⚠️ 서명 복구 실패: " + serr.left(200) + "\n"
                              "       앱은 계속 쓸 수 있지만, 배포·공증 전에 다시 빌드하십시오.\n";
            }
        }
    }
#endif

    const QStringList cleaned = cleanStaleState();
    for (const QString &c : cleaned)
        report += "[CLEAN] stale lock 제거: " + c + "\n";

    if (broken > 0) {
        report += QString("\n%1개 도구 복구 실패 — 로컬 LLM 진단 시도…\n").arg(broken);
        const QString diag = llmDiagnose(report);
        report += diag.isEmpty()
            ? QStringLiteral("(로컬 LLM 미가동 — Ollama/LM Studio/llama.cpp 실행 또는 "
                             "Resources/llm/ 에 llama-server+model.gguf 배치 시 자동 진단)\n")
            : "┌ LLM 진단 ┐\n" + diag + "\n└──────────┘\n";
    }

    // 보고서 저장
    const QString outDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                           + "/selfrepair";
    QDir().mkpath(outDir);
    QFile f(outDir + "/last_report.txt");
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        f.write(report.toUtf8());
    qInfo().noquote() << report;
    return report;
}

inline void runStartupMaintenanceAsync()
{
    QThread *t = QThread::create([] { runStartupMaintenance(); });
    QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);

    // ★ 종료할 때 이 스레드를 아무도 기다리지 않았다.
    //   진단 대부분은 읽기라서 중간에 버려도 그만이지만, 재서명은 다르다.
    //   1분 남짓 걸리는데 그 도중에 프로세스가 끝나면 번들이 무효인 채로 남는다
    //   (안쪽 부품은 새로 서명됐는데 바깥 봉인은 옛것). 다음 실행 때 또 재서명을
    //   시작하고, 또 끄면 또 무효 — 고리가 된다. 실제로 그 상태를 겪었다.
    //   그러니 '재서명이 도는 중일 때만' 기다린다. 다른 작업은 그냥 버린다.
    QPointer<QThread> guard(t);
    QObject::connect(QCoreApplication::instance(), &QCoreApplication::aboutToQuit,
                     [guard]() {
        if (!guard || !guard->isRunning() || !Common::resealInFlight()) return;
        qInfo().noquote() << "[SEAL] 서명 복구가 끝나기를 기다립니다 — 잠시만 기다려 주십시오.";
        // 넉넉히 기다리되 무한정은 아니다. 못 끝내면 다음 실행이 이어서 고친다.
        if (!guard->wait(240000))
            qWarning().noquote() << "[SEAL] 시간 안에 끝내지 못했습니다 — 다음 실행 때 다시 복구합니다.";
    });

    t->start(QThread::LowPriority);
}

} // namespace SelfRepair
