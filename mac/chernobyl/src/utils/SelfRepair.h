#pragma once
#include <QStorageInfo>
// ═════════════════════════════════════════════════════════════════════════
// SelfRepair.h — 앱 자가진단 · 자가복구 + 로컬 LLM 진단 계층 (header-only)
// 설치 위치: windows/src/utils/SelfRepair.h  (mac/chernobyl/src/utils/ 동일)
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

    // 2) AI 엔진·모델 — 없으면 자가수리·보관함 질의가 아예 못 돈다.
    {
        const QString llm = appData + "/llm";
        const bool srv = QFile::exists(llm + "/llama-server");
        const int models = QDir(llm).entryList(QStringList() << "*.gguf", QDir::Files).size();
        if (!srv || models == 0)
            out += QString("[WARN] AI 엔진 미설치 (엔진 %1 · 모델 %2개) — 'AI 설치' 를 실행하세요\n")
                       .arg(srv ? "있음" : "없음").arg(models);
        else
            out += QString("[OK]   AI 엔진 · 모델 %1개\n").arg(models);
    }

    // 3) 산출물 색인 — 없으면 보관함 질문이 전부 "자료 없음" 이 된다.
    {
        const QFileInfo db(appData + "/archive_index.db");
        if (!db.exists())
            out += "[WARN] 산출물 색인 없음 — 설정에서 '색인 만들기' 를 한 번 실행하세요\n";
        else {
            const int days = db.lastModified().daysTo(QDateTime::currentDateTime());
            out += QString("[OK]   산출물 색인 %1MB (%2일 전 갱신)%3\n")
                       .arg(db.size() / 1024 / 1024).arg(days)
                       .arg(days > 30 ? "  ← 오래됐습니다. 갱신을 권합니다" : "");
        }
    }

    // 4) 설정 파일 권한 — NAS 비밀번호·쿠키·토큰이 들어 있다.
    {
        const QString cfg = appData + "/miyo_config.json";
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
    report += checkEnvironment();

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
                report += "[SEAL] 코드 서명 봉인이 깨져 있습니다 — 자동 복구를 시도합니다"
                          " (번들이 커서 수 분 걸릴 수 있습니다)…\n";
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
    t->start(QThread::LowPriority);
}

} // namespace SelfRepair
