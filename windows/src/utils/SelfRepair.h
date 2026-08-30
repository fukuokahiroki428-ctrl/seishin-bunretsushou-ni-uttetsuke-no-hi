#pragma once
// ═════════════════════════════════════════════════════════════════════════
// SelfRepair.h — 앱 자가진단 · 자가복구 + 로컬 LLM 진단 계층 (header-only)
// 설치 위치: windows/src/utils/SelfRepair.h
//   ※ mac/chernobyl/src/utils/SelfRepair.h 와 '같은 파일이어야 한다' 는 뜻이 아니다.
//     실제로 두 판은 갈라져 있다 — 윈도우 쪽엔 도구 자동 갱신·실기능 확인이,
//     맥 쪽엔 서명 자동복구가 따로 들어갔다. 전에 여기 "동일" 이라고 적혀 있었고
//     그것을 믿고 한쪽만 고치면 다른 쪽은 조용히 낡는다.
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
//    다뤄야 "실제로는 되는데 자가진단만 [FAIL]" 같은 오경보가 안 난다.)
// header-only (Q_OBJECT 없음) → CMakeLists 수정 불필요.
// ═════════════════════════════════════════════════════════════════════════

#include <QAtomicInt>
#include <QElapsedTimer>
#include <QMutex>
#include <QTextStream>
#include <functional>
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
        // ★ 앱 본체와 같은 후보를 본다. 예전엔 여기서 번들 python_env 하나만 봐서,
        //   번들이 없고 시스템 파이썬으로 잘 도는 기계에서도 [FAIL] 이 떴다.
        //   진단이 앱과 다른 것을 보면 그 진단은 앱 얘기가 아니다.
        c << Common::pythonCandidates();
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
    for (const QString &p : cands) {
        if (QFileInfo(p).isFile()) return p;   // ★ 디렉토리 제외 — exiftool 후보 'tools/exiftool'(폴더)가
                                               //   매칭돼 perl 이 폴더를 스크립트로 열려다 실패하던 문제
        // ★ 'py' 처럼 경로가 아니라 이름만 온 후보는 PATH 에서 찾는다.
        //   앱 본체(pythonCandidates)가 그렇게 쓰므로 진단도 같아야 한다.
        //   안 그러면 번들 파이썬이 없고 시스템 파이썬으로 잘 도는 기계에서
        //   앱은 멀쩡한데 진단만 [FAIL] 을 찍는다.
        if (!p.contains('/') && !p.contains('\\')) {
            const QString found = QStandardPaths::findExecutable(p);
            if (!found.isEmpty()) return found;
        }
    }
    return QString();
}

// ── 자가진단 ─────────────────────────────────────────────────────────────

inline QStringList versionArgs(const QString &name)
{
    if (name == "ffmpeg")   return {"-version"};
    if (name == "exiftool") return {"-ver"};
    if (name == "rclone")   return {"version"};
    return {"--version"};   // yt-dlp, python
}

// ★ 크기 하한은 '잘린 파일' 만 거를 만큼만 둔다. 처음에 512KB 로 잡았다가
//   멀쩡한 도구를 손상이라고 보고했다 — 실측: python.exe 91KB, exiftool.exe 58KB.
//   잘못된 [FAIL] 은 잘못된 [OK] 만큼 나쁘다. 판단은 MZ 서명에 맡긴다.
inline bool looksLikeExecutable(const QString &path, qint64 minBytes = 1024)
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

// ★ 도구를 실행할 때 쓸 경로 — 앱 본체와 똑같은 규칙으로 정한다.
//
//   실측으로 물린 자리다. exiftool 은 자기 경로 옆의 exiftool_files\perl5*.dll 을
//   ANSI 코드페이지로 찾는다. 경로에 한글이 있으면 못 찾는다. 앱 본체는 그래서
//   exiftool 을 ANSI 로 쓸 수 있는 자리로 통째로 옮겨서 쓴다(asciiSafeExiftool).
//   그런데 자가진단만 8.3 단축 경로(ansiSafePath)를 쓰고 있었고, 이 기계의 D: 는
//   8.3 이름 생성이 꺼져 있어 단축 경로가 안 만들어진다 — 그래서
//     [FAIL] exiftool — Could not find ...\exiftool_files\perl5*.dll
//   이 떴다. 앱의 EXIF 는 멀쩡히 써지고 있는데도 그랬다.
//   잘못된 [FAIL] 은 잘못된 [OK] 만큼 나쁘다. 진단은 앱이 하는 것을 그대로 해야 한다.
inline QString launchPath(const QString &name, const QString &path)
{
#ifdef Q_OS_WIN
    if (name == "exiftool") return Common::asciiSafeExiftool(path);
    return Common::ansiSafePath(path);
#else
    Q_UNUSED(name);
    return path;
#endif
}

inline ToolStatus checkTool(const QString &name)
{
    ToolStatus st; st.name = name;
    st.path = resolveTool(name);
    if (st.path.isEmpty()) { st.error = "not found (bundle/user/system)"; return st; }
    st.found = true;

    // ★ 실행하기 전에 실행 파일 꼴인지 본다.
    //   망가진 파일을 QProcess 로 돌리면 윈도우가 "이 앱을 실행할 수 없습니다" 대화상자를
    //   띄울 수 있고, 그러면 이 스레드가 사람이 누를 때까지 멈춘다. 자가진단이 멈추면
    //   보고서도 안 나오므로 "고쳐 놓고 말을 안 하는" 상태가 된다 — 실제로 그랬다.
    //   실측: yt-dlp 를 6바이트로 바꾸고 앱을 띄우면 복구는 되는데 보고서가 60~90초가
    //   지나도 안 나왔다. 대화상자가 뜰지 말지는 SmartScreen·백신 상태에 따라 달라져서
    //   재현이 들쭉날쭉했다. 실행에 맡기지 않고 여기서 먼저 거른다.
    if (!looksLikeExecutable(st.path)) {
        st.error = "손상됨 (실행 파일 형식이 아님)";
        return st;   // runs=false → 아래 repairTool 이 번들본으로 되살린다
    }

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
    p.start(launchPath(name, st.path), versionArgs(name));
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

// ═════════════════════════════════════════════════════════════════════════
// 실기능 확인 (smoke test) — "버전이 찍힌다" 와 "실제로 된다" 는 다른 얘기다.
//
// 왜 필요한가 — 실제로 당한 두 건:
//   · 2026.08.29 유튜브 다운로드 전면 정지. yt-dlp 2026.07.04 는 --version 을
//     멀쩡히 출력했고 형식 목록도 나왔다. 미디어 요청만 전부 HTTP 403 이었다.
//   · EXIF 1668건 전부 실패. exiftool -ver 는 [OK] 였다. 실제로는 argfile 이
//     먼저 지워져 한 건도 안 써지고 있었다.
//   두 번 다 자가진단은 "이상 없음" 이라고 말하고 있었다. 조용한 고장보다
//   "괜찮다고 말하는 고장" 이 더 나쁘다 — 사람이 딴 데를 찾게 만든다.
//
// 그래서 각 도구에 '앱이 실제로 시키는 일' 을 그대로 시켜 보고 결과를 확인한다.
//
// 판정은 셋이다 — 통과 / 실패 / 건너뜀.
//   '건너뜀' 이 중요하다. 네트워크가 없을 때 yt-dlp 를 실패로 적으면 비행기 안에서
//   앱을 켠 사람에게 "도구가 망가졌다" 고 거짓말하는 셈이다. 1년을 방치할 앱에서
//   거짓 경보는 진짜 경보를 묻어 버린다. 모르면 모른다고 적는다.
// ═════════════════════════════════════════════════════════════════════════

// httpGet 의 정의는 아래 LLM 절에 있다. 여기서 먼저 쓰므로 선언만 앞세운다.
inline QByteArray httpGet(const QString &url, int timeoutMs);

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
              "        'PIL','piexif','bs4','lxml','websockets','m3u8','browser_cookie3']\n"
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
    return smokePass("필수 모듈 14개 import · 한글 출력 확인");
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
    const QString custom = qEnvironmentVariable("PREDORMITION_SMOKE_YT");
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

// ═════════════════════════════════════════════════════════════════════════
// 꾸러미 신선도 — 파이썬 쪽의 '낡아서 멈추는 고장' 을 미리 본다.
//
// 왜 있는가:
//   2026.08.29 에 유튜브가 통째로 멈춘 것은 코드 버그가 아니라 낡은 yt-dlp 였다.
//   그 뒤 사람이 손으로 고정판 14개를 전부 최신과 대조해 넷을 올렸다(커밋 1c14aa2).
//   맞는 일이었다. 그런데 그건 사람이 기억할 때만 되는 일이다 — 6개월 뒤엔 아무도
//   안 한다. 이 앱의 목표가 '1년 방치' 라면 그 대조는 앱이 해야 한다.
//
// 세 자리를 본다. 셋이 다 다를 수 있고, 각각 뜻이 다르다:
//   1) 실제 설치된 판   — 지금 이 기계에서 진짜로 도는 것
//   2) requirements 고정판 — 빌드가 넣기로 한 것
//   3) PyPI 최신        — 세상에 나와 있는 것
//   1<2 면 이 기계만 뒤처진 것(설정 → 모듈 업데이트).
//   2<3 면 고정 자체가 낡은 것(커밋이 필요하다).
//
// 그리고 하나 더 — '최신인데 상류가 멈춘' 경우도 본다.
//   twikit 이 그렇다(최신 2.3.3, 2025-02 배포). 최신을 깔아도 그 라이브러리가
//   더 이상 안 나오면, X 가 바뀌는 날 고쳐 줄 사람이 없다는 뜻이다.
//   이건 업데이트로 못 고친다. 그래도 '모르고 있다가 당하는' 것보다는 낫다.
//
// 비용: 하루에 한 번, 꾸러미당 HTTP 한 번. 그마저도 네트워크 확인일에만 한다.
// ═════════════════════════════════════════════════════════════════════════

// "1.2.10" vs "1.2.9" 를 숫자로 비교한다. 문자열 비교면 9 가 10보다 크다.
inline int compareVersions(const QString &a, const QString &b)
{
    const QStringList pa = a.split('.'), pb = b.split('.');
    for (int i = 0; i < qMax(pa.size(), pb.size()); ++i) {
        const QString xa = i < pa.size() ? pa[i] : QStringLiteral("0");
        const QString xb = i < pb.size() ? pb[i] : QStringLiteral("0");
        bool oka = false, okb = false;
        const int na = xa.toInt(&oka), nb = xb.toInt(&okb);
        if (oka && okb) { if (na != nb) return na < nb ? -1 : 1; }
        else            { if (xa != xb) return xa < xb ? -1 : 1; }
    }
    return 0;
}

// ★ '상류가 조용하다' 가 위험한 꾸러미는 따로 있다.
//
//   처음엔 고정판이 최신이면서 1년 넘게 새 배포가 없는 것을 전부 경고했다.
//   14개 중 6개가 걸렸다 — piexif(87개월), openpyxl(26개월) 까지. 그런데 그건
//   위험이 아니라 '다 만들어져서 더 고칠 게 없는' 것이다. JPEG 태그 형식은
//   안 바뀐다. 그렇게 적어 놓으니 정작 중요한 twikit 이 그 목록에 묻혔다.
//   또 같은 실수였다 — 틀린 경보는 맞는 경보를 묻는다.
//
//   진짜 위험한 것은 '바깥 서비스를 따라가야 하는' 꾸러미다. 그 서비스가 바뀌는데
//   라이브러리가 안 나오면, 바뀌는 날 고쳐 줄 사람이 없다는 뜻이다.
//   그런 것만, 왜 그런지와 함께 적는다.
inline QString serviceTracker(const QString &pkg)
{
    if (pkg.compare("twikit", Qt::CaseInsensitive) == 0)          return "X(트위터)";
    if (pkg.compare("atproto", Qt::CaseInsensitive) == 0)         return "블루스카이";
    if (pkg.compare("yt-dlp", Qt::CaseInsensitive) == 0)          return "동영상 사이트들";
    if (pkg.compare("browser_cookie3", Qt::CaseInsensitive) == 0) return "브라우저 쿠키 저장 형식";
    if (pkg.compare("discord.py", Qt::CaseInsensitive) == 0)      return "디스코드";
    return QString();   // 나머지는 오래 안 나와도 정상이다
}

// 모듈 이름과 배포 이름이 다른 것들 (import PIL / pip install Pillow)
inline QString distNameOf(const QString &pkg)
{
    if (pkg.compare("browser_cookie3", Qt::CaseInsensitive) == 0) return "browser-cookie3";
    if (pkg.compare("discord.py", Qt::CaseInsensitive) == 0)      return "discord.py";
    return pkg;
}

inline QString checkModuleFreshness(const QString &python, bool allowNetwork)
{
    if (!allowNetwork) return QString();

    // requirements.txt 의 고정 목록을 그대로 쓴다 — 코드에 목록을 또 박으면 갈라진다.
    const QStringList reqs = Common::bundledRequirements();
    if (reqs.isEmpty()) return QStringLiteral("[MOD]  requirements.txt 를 찾지 못해 신선도 확인을 건너뜁니다\n");

    QMap<QString, QString> pinned;          // 이름 → 고정판
    QStringList names;
    for (const QString &r : reqs) {
        const int eq = r.indexOf("==");
        if (eq <= 0) continue;
        const QString n = r.left(eq).trimmed();
        pinned[n] = r.mid(eq + 2).trimmed();
        names << n;
    }
    if (names.isEmpty()) return QString();

    // 이 기계에 실제로 깔린 판을 묻는다.
    QMap<QString, QString> installed;
    {
        const QString dir = smokeDir();
        const QString py = dir + "/mod_probe.py";
        QFile f(py);
        if (f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QTextStream ts(&f);
            ts.setEncoding(QStringConverter::Utf8);
            ts << "import sys, importlib.metadata as M\n"
                  "for n in sys.argv[1:]:\n"
                  "    try: print(n + '=' + M.version(n))\n"
                  "    except Exception: print(n + '=?')\n";
            f.close();
            QProcess p;
            p.setProcessEnvironment(Common::bundledProcessEnv());
            p.start(python, QStringList() << py << names);
            if (p.waitForStarted(5000) && p.waitForFinished(60000)) {
                const QString out = QString::fromUtf8(p.readAllStandardOutput());
                for (const QString &line : out.split('\n')) {
                    const int eq = line.indexOf('=');
                    if (eq > 0) installed[line.left(eq).trimmed()] = line.mid(eq + 1).trimmed();
                }
            }
        }
    }

    QStringList behindLocal, behindPin, upstreamQuiet;
    const QDateTime now = QDateTime::currentDateTimeUtc();
    int checked = 0;

    for (const QString &n : names) {
        const QByteArray raw = httpGet("https://pypi.org/pypi/" + distNameOf(n) + "/json", 8000);
        if (raw.isEmpty()) continue;                 // 못 물어봤으면 조용히 넘어간다
        const QJsonObject o = QJsonDocument::fromJson(raw).object();
        const QString latest = o.value("info").toObject().value("version").toString();
        if (latest.isEmpty()) continue;
        ++checked;

        const QString pin = pinned.value(n);
        const QString have = installed.value(n);

        if (!have.isEmpty() && have != "?" && compareVersions(have, pin) < 0)
            behindLocal << QString("%1 설치 %2 < 고정 %3").arg(n, have, pin);
        if (compareVersions(pin, latest) < 0)
            behindPin << QString("%1 고정 %2 < 최신 %3").arg(n, pin, latest);

        // 최신판이 나온 지 얼마나 됐나 — 상류가 멈춘 것도 위험이다.
        const QJsonArray files = o.value("releases").toObject().value(latest).toArray();
        if (!files.isEmpty()) {
            const QString up = files.first().toObject().value("upload_time").toString();
            const QDateTime rel = QDateTime::fromString(up, Qt::ISODate);
            if (rel.isValid()) {
                const qint64 days = rel.daysTo(now);
                const QString tracks = serviceTracker(n);
                if (days > 365 && !tracks.isEmpty() && compareVersions(pin, latest) >= 0)
                    upstreamQuiet << QString("%1 (%2) — %3 변화를 따라가야 하는 꾸러미인데 %4개월째 새 배포가 없습니다")
                                         .arg(n, latest, tracks).arg(days / 30);
            }
        }
    }

    if (checked == 0) return QStringLiteral("[MOD]  꾸러미 신선도 — PyPI 에 닿지 못해 건너뜁니다\n");

    QString r = QString("[MOD]  꾸러미 %1개 확인").arg(checked);
    if (behindLocal.isEmpty() && behindPin.isEmpty() && upstreamQuiet.isEmpty())
        return r + " — 전부 최신\n";
    r += "\n";
    for (const QString &l : behindLocal)
        r += "       └ " + l + " — 설정 → 모듈 업데이트로 받으세요\n";
    for (const QString &l : behindPin)
        r += "       └ " + l + " — requirements.txt 를 올려야 합니다\n";
    for (const QString &l : upstreamQuiet)
        r += "       └ " + l + "\n";
    return r;
}

inline SmokeResult smokeTest(const QString &name, const QString &path, bool allowNetwork)
{
    if (name == "exiftool") return smokeExiftool(path);
    if (name == "ffmpeg")   return smokeFfmpeg(path);
    if (name == "python")   return smokePython(path);
    if (name == "yt-dlp")   return allowNetwork ? smokeYtDlp(path)
                                                : smokeSkip("최근에 확인해서 이번엔 건너뜁니다");
    // rclone 은 원격 저장소가 있어야 의미 있는 시험이 된다.
    // 없는 시험을 있는 척하지 않는다 — 그게 [OK] 를 못 믿게 만드는 지름길이다.
    return smokeSkip("실기능 확인 항목 없음 (버전 확인만)");
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
    p.start(launchPath(name, userCopy), QStringList() << "-U");
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

// ── 결과를 바깥(화면)으로 내보내는 통로 ─────────────────────────────────
//
// 예전에는 AppData 안의 텍스트 파일에만 남겼다. 그러면 아무도 안 본다.
// EXIF 가 1668건 전부 실패하는 동안 앱은 화면에 한 마디도 하지 않았고,
// 사람은 잘 되고 있다고 믿었다. 파일에만 적는 진단은 진단이 아니라 기록이다.
//
// 그래서 한 줄짜리 요약을 앱에 넘길 통로를 둔다. 넘기는 쪽은 백그라운드
// 스레드이므로, 받는 쪽(백엔드)이 메인 스레드로 넘겨서 화면을 건드린다.
// ─────────────────────────────────────────────────────────────────────────

inline QMutex &stateMutex()      { static QMutex m;   return m; }
inline QString &lastReportRef()  { static QString s;  return s; }

using Notifier = std::function<void(QString, QString)>;   // (한 줄, 등급: success/error/info)
inline Notifier &notifierRef()   { static Notifier f;  return f; }

inline void setNotifier(Notifier f)
{
    QMutexLocker lk(&stateMutex());
    notifierRef() = std::move(f);
}

inline void notify(const QString &line, const QString &level)
{
    Notifier f;
    { QMutexLocker lk(&stateMutex()); f = notifierRef(); }
    if (f) f(line, level);
}

inline QString reportPath()
{
    const QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                      + "/selfrepair";
    QDir().mkpath(d);
    return d + "/last_report.txt";
}

// 마지막 보고서 — 메모리에 있으면 그것, 없으면 지난 실행이 남긴 파일.
// (앱을 막 켠 직후 화면이 물어보면 아직 이번 것이 없다. 그때는 지난 것이라도 보여 준다.)
inline QString lastReport()
{
    { QMutexLocker lk(&stateMutex());
      if (!lastReportRef().isEmpty()) return lastReportRef(); }
    QFile f(reportPath());
    if (f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString::fromUtf8(f.readAll());
    return QString();
}

// ── 오케스트레이터 ───────────────────────────────────────────────────────

// deep       = 사용자가 화면에서 직접 눌렀다는 뜻.
//              그때는 하루 한 번 제한을 무시하고 네트워크 확인까지 전부 다시 한다.
// cleanLocks = 캡쳐 Chrome 의 잔존 잠금 파일을 지울지.
//              앱을 켤 때는 지운다(지난 세션의 찌꺼기다). 그런데 켜 둔 채로 도는
//              주기 점검에서 지우면 지금 돌고 있는 캡쳐의 잠금을 뺏을 수 있다.
//              시작할 때만 지운다.
inline QString runStartupMaintenance(bool deep = false, bool cleanLocks = true)
{
    QString report;
    report += "═ SelfRepair 자가진단 " + QDateTime::currentDateTime().toString(Qt::ISODate) + " ═\n";
    report += "appDir: " + appDir() + "\n";
    // ★ 요청에 실을 User-Agent 도 적어 둔다.
    //   이건 '가만히 두면 저절로 낡는' 값이라 눈에 보이는 데 있어야 한다.
    //   예전엔 코드에 Chrome/120·131, 심지어 맥 사파리로 박혀 있었다 —
    //   윈도우에서 도는 앱이 맥인 척하면서 윈도우 크롬 쿠키를 쓰고 있었다.
    report += "User-Agent: " + Common::browserUserAgent() + "\n";

    // 네트워크가 필요한 확인은 하루에 한 번만 한다.
    //   앱을 자주 켜는 사람에게 매번 유튜브를 두드리게 하면 그것 자체가 민폐다.
    //   사용자가 버튼을 누른 경우(deep)는 무조건 한다 — 지금 확인하고 싶은 것이니까.
    const bool netCheck = deep || updateDue("smoke_net", 1);
    bool netCheckRan = false;

    // ★ 검사보다 갱신을 먼저 한다. 낡은 것을 검사해 [OK] 를 찍어 봐야 소용없다.
    //   (실측: 두 달 묵은 yt-dlp 도 --version 은 통과하는데 유튜브는 전부 403 이었다)
    report += updateYtDlpIfDue(7, deep);

    const QStringList tools = {"yt-dlp", "ffmpeg", "python", "exiftool", "rclone"};
    int broken = 0, repaired = 0, smokeBad = 0, moduleStale = 0;
    QStringList badNames;

    for (const QString &t : tools) {
        ToolStatus st = checkTool(t);
        if (!st.runs) {
            repairTool(st);   // 발견 여부와 무관하게 번들 재복사 경로로 복구 시도
            if (st.runs) {
                report += QString("[OK*]  %1 — %2 (자동 복구됨: %3)\n").arg(t, st.version, st.path);
                ++repaired;
                // ★ 복구는 낡은 쪽으로 되돌린다 — 번들본은 배포 시점에 굳은 물건이다.
                //   yt-dlp 는 바깥 서비스를 따라가야 하는 도구라서, 낡은 것으로 되살려 놓고
                //   다음 갱신일(7일 뒤)까지 두면 그 사이 내내 안 되는 상태로 있는다.
                //   되살렸으면 곧바로 최신으로 끌어올린다. '복구' 가 '퇴행' 이 되면 안 된다.
                if (t == "yt-dlp") {
                    report += updateYtDlpIfDue(0, true);
                    ToolStatus re = checkTool(t);
                    if (re.runs) st = re;
                }
            } else {
                report += QString("[FAIL] %1 — %2\n").arg(t, st.error);
                ++broken;
                badNames << t;
                continue;     // 실행도 안 되는 것에 실기능 확인은 의미가 없다
            }
        } else {
            report += QString("[OK]   %1 — %2 (%3)\n").arg(t, st.version, st.path);
        }

        // ★ 여기부터가 핵심 — 버전이 아니라 '실제로 되는가' 를 본다.
        SmokeResult sm = smokeTest(t, st.path, netCheck);

        if (t == "yt-dlp" && sm.verdict != SmokeSkip) netCheckRan = true;

        if (sm.verdict == SmokeFail && t == "yt-dlp") {
            // 유튜브가 바뀌어서 낡은 yt-dlp 가 막힌 것일 수 있다 — 실제로 그랬다.
            // 사람이 없어도 여기서 스스로 최신을 받아 다시 해 본다.
            // 이 한 덩어리가 "1년을 방치해도 도는가" 의 대부분이다.
            report += "[UPD]  yt-dlp — 실기능 확인 실패, 최신본을 받아 다시 해 봅니다\n";
            report += updateYtDlpIfDue(0, true);
            st = checkTool(t);
            if (st.runs) sm = smokeTest(t, st.path, true);
        }

        if (sm.verdict == SmokePass) {
            report += "       └ 실기능 확인: 통과 — " + sm.detail + "\n";
            // 파이썬이 실제로 도는 것이 확인됐을 때만 꾸러미 신선도를 본다.
            //   안 도는 파이썬에 판을 물어봐야 답이 없다.
            if (t == "python") {
                const QString mod = checkModuleFreshness(st.path, netCheck);
                if (!mod.isEmpty()) {
                    report += mod;
                    if (netCheck) netCheckRan = true;
                    // 꾸러미가 뒤처진 것은 '고장' 은 아니지만 조용히 넘길 일도 아니다.
                    //   실제로 낡은 꾸러미 하나가 유튜브를 통째로 세운 적이 있다.
                    for (const QString &ln : mod.split('\n'))
                        if (ln.contains("모듈 업데이트")) ++moduleStale;
                }
            }
        } else if (sm.verdict == SmokeSkip) {
            report += "       └ 실기능 확인: 건너뜀 — " + sm.detail + "\n";
        } else {
            report += "       └ 실기능 확인: 실패 — " + sm.detail + "\n";
            ++smokeBad;
            if (!badNames.contains(t)) badNames << t;
        }
    }

    // 네트워크 확인이 실제로 이뤄졌을 때만 시각을 적는다.
    //   오프라인으로 켠 것 때문에 하루치를 써 버리면, 정작 온라인일 때 안 본다.
    if (netCheckRan) markUpdated("smoke_net");

    if (cleanLocks) {
        const QStringList cleaned = cleanStaleState();
        for (const QString &c : cleaned)
            report += "[CLEAN] stale lock 제거: " + c + "\n";
    }

    if (broken > 0) {
        report += QString("\n%1개 도구 복구 실패 — 로컬 LLM 진단 시도…\n").arg(broken);
        const QString diag = llmDiagnose(report);
        report += diag.isEmpty()
            ? QStringLiteral("(로컬 LLM 미가동 — Ollama/LM Studio/llama.cpp 실행 또는 "
                             "Resources/llm/ 에 llama-server+model.gguf 배치 시 자동 진단)\n")
            : "┌ LLM 진단 ┐\n" + diag + "\n└──────────┘\n";
    }

    // 한 줄 요약 — 이게 화면에 뜨는 문장이다. 길면 안 읽는다.
    QString summary, level;
    if (broken == 0 && smokeBad == 0) {
        summary = QString("자가진단 이상 없음 — 도구 %1개가 실제로 동작합니다").arg(tools.size());
        if (repaired > 0) summary += QString(" (%1개는 자동 복구했습니다)").arg(repaired);
        if (moduleStale > 0) {
            // 도구는 다 도는데 파이썬 꾸러미가 뒤처진 상태. 지금 당장은 되지만
            // 그대로 두면 언젠가 멈춘다 — '주의' 로 알린다(빨간 띠는 안 띄운다).
            summary += QString(". 다만 파이썬 꾸러미 %1개가 뒤처져 있습니다 — "
                               "설정 → 모듈 업데이트").arg(moduleStale);
            level = QStringLiteral("warning");
        } else {
            level = QStringLiteral("success");
        }
    } else {
        summary = QString("자가진단에서 문제를 찾았습니다 — %1 (설정 → 자가진단에서 자세히)")
                      .arg(badNames.join(", "));
        level = QStringLiteral("error");
    }
    report += "\n" + summary + "\n";

    { QMutexLocker lk(&stateMutex()); lastReportRef() = report; }
    QFile f(reportPath());
    if (f.open(QIODevice::WriteOnly | QIODevice::Text))
        f.write(report.toUtf8());
    qInfo().noquote() << report;

    notify(summary, level);
    return report;
}

inline void runStartupMaintenanceAsync()
{
    QThread *t = QThread::create([] { runStartupMaintenance(); });
    QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start(QThread::LowPriority);
}

// ★ 켜 두고 쓰는 기계를 위한 진입점.
//
//   시작 시 한 번만 보면, 한 달 내내 켜 둔 앱은 그 한 달 동안 아무도 안 본다.
//   유튜브가 바뀌어 다운로드가 막혀도 앱은 모른 채로 계속 돈다 — 실제로 그랬다.
//   그래서 하루에 한 번 '전부' 다시 본다. 갱신만이 아니라 실기능 확인까지.
//   (전에는 여기서 yt-dlp 갱신만 했다. 갱신은 고장을 고칠 뿐 고장을 알려주진 않는다.)
//
//   비용은 걱정하지 않아도 된다 — 안쪽에서 다시 따진다.
//     · yt-dlp 갱신: 7일에 한 번만 실제 통신
//     · 유튜브 실기능 확인: 하루에 한 번만
//     · 나머지(ffmpeg·exiftool·python)는 전부 오프라인이고 5초 안에 끝난다
//   잠금 파일 정리는 하지 않는다 — 지금 돌고 있는 캡쳐의 잠금을 뺏을 수 있다.
inline void runPeriodicUpdateAsync()
{
    QThread *t = QThread::create([] {
        runStartupMaintenance(/*deep=*/false, /*cleanLocks=*/false);
    });
    QObject::connect(t, &QThread::finished, t, &QObject::deleteLater);
    t->start(QThread::LowPriority);
}

} // namespace SelfRepair
