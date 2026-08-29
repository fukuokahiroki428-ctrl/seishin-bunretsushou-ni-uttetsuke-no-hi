#pragma once
// ═════════════════════════════════════════════════════════════════════════
// SelfRepair.h — 앱 자가진단 · 자가복구 + 로컬 LLM 진단 계층 (header-only)
// 설치 위치: windows/src/utils/SelfRepair.h  (mac/predormition/src/utils/ 동일)
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
//    다뤄야 "실제로는 되는데 자가진단만 [FAIL]" 같은 오경보가 안 난다.)
// header-only (Q_OBJECT 없음) → CMakeLists 수정 불필요.
// ═════════════════════════════════════════════════════════════════════════

#include <QAtomicInt>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
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
    //   ANSI 로 받으므로, 사용자 이름이 한글·일본어인 설치 경로에서는 8.3 단축 경로가 필요하다.
    //   여기서만 원본 경로를 넘기면 실제 EXIF 는 써지는데 진단만 실패하는 오경보가 난다.
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
// ═════════════════════════════════════════════════════════════════════════
// 도구 자동 갱신 — 1년을 방치해도 도는 것이 목표라면 이것이 핵심이다.
//
// 왜 필요한가 (실측):
//   2026.08.29 에 유튜브 다운로드가 통째로 멈췄다. 코드 버그가 아니었다.
//   배포본에 딸려 온 yt-dlp 2026.07.04 가 유튜브의 변경을 못 따라가
//   미디어 요청이 전부 403 로 막혔다(형식 목록은 나오는데 본문만 거부).
//   최신 2026.08.19 로 바꾸니 같은 영상을 4초에 받았다. 두 달 만에 정지한 것이다.
//
//   그런데 기존 자가진단은 이 고장을 원리적으로 못 잡는다 — 낡은 yt-dlp 도
//   --version 은 멀쩡히 출력하므로 [OK] 로 통과한다. 그리고 repairTool 은
//   고장 시 '번들본 재복사' 를 하는데, 그것은 더 낡은 것으로 되돌리는 일이다.
//   즉 지금 구조는 낡음에 대해 무방비일 뿐 아니라 낡는 쪽으로 민다.
//
// 어떻게 하는가:
//   yt-dlp 자신의 -U 를 쓴다. 배포처·서명·교체를 yt-dlp 가 알아서 한다.
//   단, -U 는 '자기가 실행된 파일' 을 덮으므로 쓰기 가능한 자리여야 한다.
//   그래서 사용자 도구 폴더의 사본을 갱신한다. findBundledTool 이 그 폴더를
//   먼저 보므로 갱신 결과가 그대로 쓰인다.
//
// 안전 규칙:
//   · 갱신 전에 백업한다. 갱신 후 실행이 안 되면 백업으로 되돌린다.
//     새 것이 망가졌다고 앱이 못 쓰게 되면 자동 갱신은 없느니만 못하다.
//   · 성공했을 때만 시각을 기록한다. 오프라인이면 다음 기회에 다시 시도한다.
//   · 네트워크가 없거나 느려도 앱 시작을 막지 않는다 (낮은 우선순위 스레드).
// ═════════════════════════════════════════════════════════════════════════

// ★ 실행하기 전에 '실행 파일처럼 생겼는지' 부터 본다.
//   망가진 파일을 QProcess 로 돌리면 윈도우가 "이 앱을 실행할 수 없습니다" 대화상자를
//   띄우고, 그러면 자가진단 스레드가 사람이 누를 때까지 그대로 멈춘다.
//   실측: 6바이트 쓰레기로 바꿔 놓고 앱을 띄우니 자가진단이 몇 분씩 끝나지 않았다.
//   (재현이 들쭉날쭉했다 — 더더욱 실행에 맡기면 안 된다는 뜻이다.)
//   PE 서명(MZ)과 최소 크기만 봐도 이 사고는 전부 막힌다.
inline bool looksLikeExecutable(const QString &path, qint64 minBytes = 1024 * 512)
{
    QFileInfo fi(path);
    if (!fi.exists() || fi.size() < minBytes) return false;
#ifdef Q_OS_WIN
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray head = f.read(2);
    f.close();
    return head == "MZ";
#else
    return fi.isExecutable();
#endif
}

inline QString updateStampPath(const QString &name)
{
    const QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                        + "/selfrepair";
    QDir().mkpath(dir);
    return dir + "/last_update_" + name + ".txt";
}

inline bool updateDue(const QString &name, int everyDays)
{
    QFile f(updateStampPath(name));
    if (!f.open(QIODevice::ReadOnly)) return true;          // 기록이 없으면 해야 한다
    const qint64 last = QString::fromUtf8(f.readAll()).trimmed().toLongLong();
    f.close();
    if (last <= 0) return true;
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    return (now - last) >= qint64(everyDays) * 86400;
}

inline void markUpdated(const QString &name)
{
    QFile f(updateStampPath(name));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        f.write(QByteArray::number(QDateTime::currentSecsSinceEpoch()));
}

// yt-dlp 를 최신으로. 이미 최신이면 아무 일도 하지 않는다.
// 반환: 사람에게 보여 줄 한 줄 (빈 문자열이면 이번엔 할 일이 없었다는 뜻)
inline QString updateYtDlpIfDue(int everyDays = 7, bool force = false)
{
    const QString name = "yt-dlp";
    if (!force && !updateDue(name, everyDays)) return QString();

    const QString userCopy = userToolsDir() + "/" + name + exeSuffix();

    // 사용자 폴더에 사본이 없으면 번들본을 씨앗으로 깐다 (-U 는 쓰기 가능한 자리가 필요하다)
    if (!QFile::exists(userCopy)) {
        QString bundled;
        for (const QString &p : toolCandidates(name)) {
            if (p.startsWith(userToolsDir())) continue;
            if (QFile::exists(p)) { bundled = p; break; }
        }
        if (bundled.isEmpty()) return QStringLiteral("[UPD]  yt-dlp — 원본이 없어 갱신을 건너뜁니다\n");
        if (!QFile::copy(bundled, userCopy))
            return QStringLiteral("[UPD]  yt-dlp — 갱신용 사본을 만들지 못했습니다\n");
        makeExecutable(userCopy);
    }

    // 망가진 사본이면 갱신을 시도하지 않는다 — 아래 자가복구가 번들본으로 되살린다.
    if (!looksLikeExecutable(userCopy))
        return QStringLiteral("[UPD]  yt-dlp — 사본이 온전하지 않아 갱신을 건너뜁니다 (복구에 맡김)\n");

    const QString before = checkTool(name).version;

    // 백업 — 갱신이 잘못되면 되돌린다
    const QString backup = userCopy + ".prev";
    QFile::remove(backup);
    QFile::copy(userCopy, backup);

    QProcess p;
    p.start(Common::ansiSafePath(userCopy), QStringList() << "-U");
    if (!p.waitForStarted(5000) || !p.waitForFinished(180000)) {
        p.kill();
        QFile::remove(backup);
        return QStringLiteral("[UPD]  yt-dlp — 갱신 시도가 응답하지 않아 중단했습니다 (기존 것 유지)\n");
    }
    const QString out = QString::fromUtf8(p.readAllStandardOutput() + p.readAllStandardError()).trimmed();

    // 갱신 뒤 반드시 확인한다 — 새 것이 안 돌면 되돌린다
    const ToolStatus after = checkTool(name);
    if (!after.runs) {
        QFile::remove(userCopy);
        if (QFile::copy(backup, userCopy)) makeExecutable(userCopy);
        QFile::remove(backup);
        return QStringLiteral("[UPD]  yt-dlp — 갱신본이 실행되지 않아 이전 것으로 되돌렸습니다\n");
    }
    QFile::remove(backup);
    markUpdated(name);

    if (!before.isEmpty() && after.version == before)
        return QString("[UPD]  yt-dlp — 이미 최신 (%1)\n").arg(after.version);
    return QString("[UPD]  yt-dlp — 갱신됨: %1 → %2\n")
               .arg(before.isEmpty() ? QStringLiteral("(모름)") : before, after.version);
}

inline QStringList cleanStaleState()
{
    QStringList cleaned;
    const QString base = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);

    // Chrome 캡쳐 프로필의 잔존 락 제거 (Chrome 이 "이미 실행 중" 으로 오인해 즉시 종료하는 문제).
    //   ★ 스레드별 프로필(chrome_capture_profile_<key>)까지 훑는다 — 예전엔 고정 이름 2개만 봐서
    //     실제로 쓰이는 폴더는 하나도 정리되지 않았다.
    //   ★ 락 파일 이름은 OS 마다 다르다: 유닉스는 SingletonLock/Socket/Cookie(심볼릭 링크),
    //     Windows 는 프로필 안의 lockfile. 양쪽 다 지운다.
    QStringList profileDirs;
    for (const QString &parent : {base, QDir::homePath()}) {
        QDirIterator it(parent, {QStringLiteral("chrome_capture_profile*")},
                        QDir::Dirs | QDir::NoDotAndDotDot);
        while (it.hasNext()) profileDirs << it.next();
    }
    const QStringList locks = {QStringLiteral("SingletonLock"),
                               QStringLiteral("SingletonSocket"),
                               QStringLiteral("SingletonCookie"),
                               QStringLiteral("lockfile")};
    for (const QString &d : profileDirs) {
        for (const QString &f : locks) {
            const QString p = d + "/" + f;
            // 심볼릭 링크(유닉스 Singleton*)는 exists() 가 대상 기준이라 false 일 수 있어
            // QFileInfo(isSymLink) 로도 확인한다.
            const QFileInfo fi(p);
            if ((fi.exists() || fi.isSymLink()) && QFile::remove(p)) cleaned << p;
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

    // ★ 설치 스크립트(win_install_ai.ps1)는 모델을 <APPDATA>\Predormition\llm 에 넣는다.
    //   앱 본체의 llmDir() 도 그 자리를 먼저 본다. 그런데 여기만 <exe>/llm 만 봐서,
    //   AI 를 설치해도 자가수리의 원인 진단용 LLM 은 영영 뜨지 않았다.
    //   앱 본체와 같은 순서로 맞춘다 — 사용자 설치본 우선, 없으면 번들.
    QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/llm";
    if (!QDir(dir).exists() || QDir(dir).entryList(QStringList() << "*.gguf", QDir::Files).isEmpty())
        dir = resourcesDir() + "/llm";
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
    // 선택: env PREDORMITION_LLM_MODEL(부분일치, 예전 이름 CHERNOBYL_LLM_MODEL 도 받음) > 첫 헤드(알파벳순=가장 작은=빠른 기본).
    QString chosen = heads.first();
    // ★ 새 이름을 먼저 보고, 없으면 예전 이름을 받는다 — 쓰던 사람이 깨지지 않게.
    QString wantModel = qEnvironmentVariable("PREDORMITION_LLM_MODEL");
    if (wantModel.isEmpty()) wantModel = qEnvironmentVariable("CHERNOBYL_LLM_MODEL");
    if (!wantModel.isEmpty())
        for (const QString &h : heads)
            if (h.contains(wantModel, Qt::CaseInsensitive)) { chosen = h; break; }
    qInfo().noquote() << "[SelfRepair] 번들 LLM" << heads.size() << "개 모델 중 선택:" << chosen
                      << "(env PREDORMITION_LLM_MODEL 로 전환 가능)";
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

// 살아있는 OpenAI 호환 엔드포인트 탐색. env PREDORMITION_LLM_ENDPOINT 최우선(예전 이름도 받음).
inline QString findLlmEndpoint()
{
    QStringList bases;
    QString envEp = qEnvironmentVariable("PREDORMITION_LLM_ENDPOINT");
    if (envEp.isEmpty()) envEp = qEnvironmentVariable("CHERNOBYL_LLM_ENDPOINT");   // 예전 이름 폴백
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
        {"content", "당신은 Predormition/Pen 데스크톱 앱의 유지보수 진단가다. "
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

// ── 오케스트레이터 ───────────────────────────────────────────────────────

inline QString runStartupMaintenance()
{
    QString report;
    report += "═ SelfRepair 자가진단 " + QDateTime::currentDateTime().toString(Qt::ISODate) + " ═\n";
    report += "appDir: " + appDir() + "\n";

    // ★ 검사보다 갱신을 먼저 한다. 낡은 것을 검사해 [OK] 를 찍어 봐야 소용없다.
    //   (실측: 두 달 묵은 yt-dlp 도 --version 은 통과하는데 유튜브는 전부 403 이었다)
    report += updateYtDlpIfDue();

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

// ★ 켜 두고 쓰는 기계를 위한 진입점.
//   시작 시 한 번만 보면, 한 달 내내 켜 둔 앱은 그 한 달 동안 낡은 채로 있는다.
//   백엔드가 하루에 한 번 이것을 부른다. 갱신 주기(7일)는 안에서 다시 따지므로
//   매일 불러도 실제 통신은 일주일에 한 번뿐이다.
inline void runPeriodicUpdateAsync()
{
    QThread *t = QThread::create([] {
        const QString line = updateYtDlpIfDue();
        if (!line.isEmpty()) qInfo().noquote() << "[SelfRepair]" << line.trimmed();
    });
    QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start(QThread::LowPriority);
}

} // namespace SelfRepair
