#include <QApplication>
#include <csignal>
#include <cstdio>
#include <QLockFile>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QProcess>   // 맥: 이미 켜져 있을 때 osascript 로 기존 창 깨우기(셸 미경유)
#include "core/MainWindow.h"
#include "core/Common.h"
#include <QSslSocket>
#include <QSslConfiguration>
#include <QTimer>
#include "utils/SelfRepair.h"   // ★ 자가진단·자가복구 + 로컬 LLM 진단
#include <QThread>                 // 스레드 친화성 경고에 스레드 이름을 붙이려고

#ifdef Q_OS_WIN
#include <QFile>
#include <QMutex>
#include <QDateTime>
#include <cstdio>
#endif

#ifdef Q_OS_MACOS
#include <objc/objc.h>
#include <objc/message.h>
#include <objc/runtime.h>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

// ── macOS: Dock 클릭 시 창 다시 열기 ──
#ifdef Q_OS_MACOS
static MainWindow *g_mainWindow = nullptr;

// applicationShouldHandleReopen:hasVisibleWindows: — Dock 아이콘 클릭 이벤트
static BOOL appShouldHandleReopen(id self, SEL _cmd, id app, BOOL hasVisibleWindows) {
    if (g_mainWindow) {
        g_mainWindow->show();
        g_mainWindow->raise();
        g_mainWindow->activateWindow();
    }
    return YES;
}
#endif

#ifdef Q_OS_WIN
// ★ Windows 진단용 — 모든 qDebug/qWarning + JS console(DebugWebEnginePage 가 qDebug 로 redirect)
//   를 <exe>/predormition_log.txt 에 기록. "아무것도 안 됨" 원인을 GUI subsystem 에서도 잡기 위함.
static void predormitionLogHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
{
    static QMutex mtx;
    static QFile *logFile = nullptr;
    QMutexLocker lock(&mtx);
    if (!logFile) {
        // ★ 로그 위치 — 설치 폴더(Program Files)는 일반 사용자 권한으로 쓸 수 없어
        //   진단 로그가 아예 안 남는다. 쓰기 가능한 앱 데이터 폴더로 옮긴다.
        QString dir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (dir.isEmpty()) dir = QCoreApplication::applicationDirPath();
        QDir().mkpath(dir);
        const QString path = dir + "/predormition_log.txt";
        // ★ 무한 증가 방지 — 5MB 넘으면 직전 로그 1개만 남기고 새로 시작.
        //   append 만 하면 오래 쓴 설치본에서 로그가 수 GB 까지 자란다.
        if (QFileInfo(path).size() > 5LL * 1024 * 1024) {
            QFile::remove(path + ".1");
            QFile::rename(path, path + ".1");
        }
        logFile = new QFile(path);
        logFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
    }
    const char *lvl = "D";
    switch (type) {
        case QtWarningMsg:  lvl = "W"; break;
        case QtCriticalMsg: lvl = "C"; break;
        case QtFatalMsg:    lvl = "F"; break;
        default: break;
    }
    // ★ 스레드 친화성 경고는 "누가 냈는지" 를 모르면 쫓을 수가 없다.
    //   Qt 는 주소만 찍어 준다. 주소는 매번 달라서 아무것도 알려주지 않는다.
    //   그래서 이름을 붙여 둔 스레드면 이름을 같이 적는다.
    QString extra;
    if (msg.startsWith(QLatin1String("QObject::moveToThread"))
        || msg.startsWith(QLatin1String("QObject::setParent"))
        || msg.startsWith(QLatin1String("QObject::~QObject"))
        || msg.startsWith(QLatin1String("QObject::killTimer"))
        || msg.startsWith(QLatin1String("QObject::startTimer"))) {
        QThread *cur = QThread::currentThread();
        const QString nm = cur ? cur->objectName() : QString();
        extra = QStringLiteral("  [낸 스레드: %1]")
                    .arg(nm.isEmpty() ? QStringLiteral("(이름없음)") : nm);
    }
    if (logFile && logFile->isOpen()) {
        // ★ 날짜까지 적는다. 시각만 찍으면 어제 22:56 줄과 오늘 22:56 줄을 구분할 수
        //   없다 — 이 앱은 30분마다 폴링하며 몇 달을 내리 도는 물건이라, 로그를 볼
        //   때 "이게 오늘 것인가" 를 매번 헷갈리게 된다(실제로 헷갈렸다).
        //   한 줄에 11자가 늘어 5MB 회전이 그만큼 빨라지지만, 날짜 없는 로그보다는 낫다.
        const QByteArray line = (QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss.zzz")
                                 + " [" + lvl + "] " + msg + extra + "\n").toUtf8();
        logFile->write(line);
        logFile->flush();
    }
    fprintf(stderr, "[%s] %s\n", lvl, msg.toLocal8Bit().constData());
}
#endif
// ── 로그 줄 모양: 날짜 + (스레드 경고면) 낸 스레드 이름 ──
//   ★ 맥에는 로그 처리기가 없어서 Qt 기본 모양(날짜 없음)으로 stderr/통합 로그에 나갔다.
//     이 앱은 30분마다 폴링하며 몇 달을 내리 도는 물건이라, 표준출력을 파일로 받아 볼 때
//     어제 22:56 줄과 오늘 22:56 줄을 구분할 수 없다(윈도우 판에서 실제로 헷갈렸다 — 5a38a2c).
//     줄 모양은 qSetMessagePattern 으로 정하고, 내보내기는 Qt 기본 처리기에 그대로 맡긴다
//     (통합 로그·stderr 갈래를 우리가 다시 만들면 한쪽이 빠진다).
//   ★ 스레드 친화성 경고는 "누가 냈는지" 를 모르면 쫓을 수가 없다. Qt 는 주소만 찍어 주고,
//     주소는 매번 달라서 아무것도 알려주지 않는다(윈도우에서 30분마다 NAM 이 새던 것을
//     이걸로 쫓았다 — e0f1b3d). 그래서 이름을 붙여 둔 스레드면 이름을 같이 적는다.
static QtMessageHandler g_prevLogHandler = nullptr;
static void hanishikiLogHandler(QtMsgType type, const QMessageLogContext &ctx, const QString &msg)
{
    QString extra;
    if (msg.startsWith(QLatin1String("QObject::moveToThread"))
        || msg.startsWith(QLatin1String("QObject::setParent"))
        || msg.startsWith(QLatin1String("QObject::~QObject"))
        || msg.startsWith(QLatin1String("QObject::killTimer"))
        || msg.startsWith(QLatin1String("QObject::startTimer"))) {
        QThread *cur = QThread::currentThread();
        const QString nm = cur ? cur->objectName() : QString();
        extra = QStringLiteral("  [낸 스레드: %1]")
                    .arg(nm.isEmpty() ? QStringLiteral("(이름없음)") : nm);
    }
    if (g_prevLogHandler) g_prevLogHandler(type, ctx, extra.isEmpty() ? msg : msg + extra);
}

int main(int argc, char *argv[])
{
    // ★ 로그 줄 모양부터 정한다 — 이 아래에서 나는 첫 줄(플러그인 적재 경고 등)부터 날짜가 붙게.
    //   한 줄에 날짜 11자가 늘지만, 날짜 없는 로그보다는 낫다.
    //   QT_MESSAGE_PATTERN 환경변수가 있으면 Qt 가 그쪽을 우선한다(시험할 때 바꿔 볼 수 있다).
    qSetMessagePattern(QStringLiteral(
        "%{time yyyy-MM-dd HH:mm:ss.zzz} "
        "[%{if-debug}D%{endif}%{if-info}I%{endif}%{if-warning}W%{endif}"
        "%{if-critical}C%{endif}%{if-fatal}F%{endif}] "
        "%{if-category}%{category}: %{endif}%{message}"));
    // 앞 처리기(=Qt 기본)를 받아 두고 끝에 그대로 넘긴다 — 출력 갈래는 Qt 가 고른다.
    g_prevLogHandler = qInstallMessageHandler(hanishikiLogHandler);

    // ★ WebEngine — 메모리 절약 + Site isolation 유지 (보안)
    //   ★ GPU 관련 플래그는 플랫폼을 가른다.
    //     이 묶음은 8GB 맥에서 나던 OOM 크래시를 잡으려고 넣은 것인데 윈도우에도 그대로
    //     걸려 있었다. 윈도우는 그 제약을 질 이유가 없어 GPU 계열만 뺀다.
    //     메모리 계열(캐시 상한·JS 힙·site-per-process)은 양쪽 다 그대로 둔다.
    // ★ 밖에서 이미 지정했으면 그것을 존중한다.
    //   qputenv 는 무조건 덮어쓴다. 그래서 예전에는 QTWEBENGINE_CHROMIUM_FLAGS 를 주고
    //   앱을 띄워도 이 줄이 곧바로 지워버려, 플래그를 바꿔 시험하는 것 자체가 불가능했다
    //   (실제로 GPU 를 켜고 재본다고 한 측정이 전부 무의미했다 — 앱이 값을 덮어쓴 뒤였다).
    //   빌드하지 않고도 확인할 수 있어야 원인을 가릴 수 있다.
    if (!qEnvironmentVariableIsSet("QTWEBENGINE_CHROMIUM_FLAGS")) {
#ifdef Q_OS_WIN
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
        "--site-per-process "                      // ★ 보안 격리 유지
        // ★ memory-pressure-off 는 '메모리가 빠듯해도 비우지 말라' 는 뜻이라
        //   저용량 기계에서 켤 설정이 아니었다(정확히 반대다). 뺀다.
        //   힙 상한도 384MB 는 너무 좁아 화면이 무거워지면 그대로 죽는다(맥 ebd9009).
        "--js-flags=--max-old-space-size=768 "
        "--disk-cache-size=10485760 "
        "--media-cache-size=5242880 "
        "--aggressive-cache-discard");
        // --num-raster-threads=2 도 뺐다 — 래스터 스레드를 인위적으로 2개로 묶으면
        // 가속을 켜 놓고도 소프트웨어 경로에서 병목이 남는다. 기본값에 맡긴다.
#else
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
        "--disable-gpu "
        "--disable-gpu-compositing "
        "--disable-software-rasterizer "
        "--disable-accelerated-2d-canvas "
        "--disable-accelerated-video-decode "
        "--site-per-process "                      // ★ 보안 격리 유지
        // ★ memory-pressure-off 는 '메모리가 빠듯해도 비우지 말라' 는 뜻이라
        //   저용량 기계에서 켤 설정이 아니었다(정확히 반대다). 뺀다.
        //   힙 상한도 384MB 는 너무 좁아 화면이 무거워지면 그대로 죽는다.
        "--js-flags=--max-old-space-size=768 "
        "--disk-cache-size=10485760 "
        "--media-cache-size=5242880 "
        "--aggressive-cache-discard "
        "--num-raster-threads=2");
#endif
    }

    // ★ Qt 를 다 치운 '뒤' 에 한 줄 — 이 지역 변수는 app 보다 먼저 만들어지므로 app 의 소멸자가 끝난 다음에 소멸한다.
    //   이 줄이 기록에 없으면 앱은 끝나는 도중에 죽은 것이다(충돌 보고서는 몇 분 늦게 써지기도 한다 — 2026-10-06 3분).
    //   공작함 tests/quit_during_hotfix.py 가 이것으로 '깨끗이 끝났는가' 를 잰다.
    struct CleanExitMark { ~CleanExitMark() { std::fprintf(stderr, "[exit] 정상 종료\n"); std::fflush(stderr); } } cleanExitMark;
    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    // ★ 자식 프로세스가 망가졌을 때 윈도우가 띄우는 오류 대화상자를 끈다.
    //   그 창이 뜨면 그것을 띄운 스레드가 사람이 누를 때까지 멈춘다. 자가진단처럼
    //   백그라운드에서 도는 것이 멈추면 아무도 모르는 채로 앱이 반쯤 죽는다.
    //   (실측: 도구 하나를 망가뜨렸더니 자가진단 보고서가 영영 안 나왔다.)
    //   실패는 호출한 쪽이 반환값으로 받아 처리한다 — 그래야 로그에도 남는다.
    SetErrorMode(SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX | SEM_NOGPFAULTERRORBOX);
#endif
    // ★ 본 스레드에도 이름을 단다 — 스레드 경고에 '(이름없음)' 이 찍히면 본 스레드인지
    //   이름 안 단 작업 스레드인지 가를 수가 없다.
    app.thread()->setObjectName(QStringLiteral("main"));
    // ★ 이름은 빌드에서 온다(APP_NAME_ASCII). 여기 박아 두면 CMake 의 이름과
    //   어긋나 데이터 폴더가 갈라진다 — 실제로 그렇게 갈라진 적이 있다.
    //   폴더 이름은 ASCII 로 둔다. 일본어 폴더는 NAS·백업 경로에서 NFC/NFD 로
    //   어긋나 사고가 나기 쉽다(사용자에게 보이는 이름은 번들이 따로 갖는다).
    //   ★ 윈도우: 로그 파일 처리기(predormitionLogHandler)보다 반드시 먼저 정한다.
    //     AppDataLocation 은 %APPDATA%\<앱이름> 이라, 이름을 정하기 전에 첫 메시지가 찍히면
    //     처리기의 static QFile 이 엉뚱한 경로로 프로세스 내내 고정된다
    //     (진단 로그가 앱 데이터 폴더 밖에 쌓여, 문서가 안내하는 경로에는 아무것도 없었다).
    app.setApplicationName(QStringLiteral(APP_NAME_ASCII));
    // ★ 조직 이름을 두지 않는다.
    //   두면 데이터가 …/Application Support/Miyo/<앱>(윈도우 %APPDATA%\Miyo\<앱>) 처럼 한 겹 더 들어간다.
    //   'Miyo' 는 이 앱이 카메라이던 시절의 흔적일 뿐 지금 이름과 아무 관계가 없어서,
    //   폴더를 열어 본 사람이 무엇인지 알 수 없었다.
    //   비워 두면 Qt 가 …/Application Support/<앱이름>(윈도우 %APPDATA%\<앱이름>) 을 쓴다.
    //   ※ 이 앱은 QSettings 를 쓰지 않으므로 조직명은 이 경로 계산 외에 쓰이는 데가 없다.
    //   ★ 옛 위치(Miyo/<앱>)의 데이터는 아래 이사 코드가 찾아서 그대로 옮겨 온다 —
    //     설정·토큰뿐 아니라 AI 모델 9GB·색인·크롬 프로필이 거기 있다.
    app.setOrganizationName(QString());

    // ★ 앱 이름이 바뀌면 사용자 데이터 폴더도 바뀐다 — 통째로 이어받는다.
    //
    //   AppDataLocation 은 <바탕>/<앱이름> 이다. 이름을 바꾸는 순간 새 폴더가
    //   생기고, 예전 것은 그 자리에 남아 아무도 안 본다. 거기에는 설정만 있는 게
    //   아니라 AI 모델(약 9GB)·python_env·크롬 프로필·색인 DB 가 전부 들어 있다.
    //   그대로 두면 사용자는 AI 를 다시 받고 로그인을 다시 해야 한다.
    //
    //   ★ 옛 이름을 코드에 박으면 안 된다. 윈도우 판도 예전엔 옛 자리 셋
    //     (Miyo\Predormition · Miyo\Chernobyl · Chernobyl)을 박아 두었는데, 이 앱은 이미
    //     다섯 번째 이름이다(カメラ → チェルノブイリ → Chernobyl → Predormition → ハンイシキ).
    //     박아 두면 다음 변경 때 또 같은 일이 난다(실제로 %APPDATA%\Predormition 은 그 목록에 없었다).
    //     그래서 형제 폴더와 옛 조직 폴더(Miyo · ABIWA) 안에서 '가장 최근에 쓰던 것' 을 찾아 이어받는다.
    //
    //   ★ 로그 파일 처리기보다 먼저 돌아야 한다(윈도우). 처리기가 로그 파일을 열며
    //     새 폴더를 mkpath 해 버리면, 그 폴더에 잔재가 생겨 아래 판별이 흐려진다.
    //
    //   같은 디스크 안 rename 이라 9GB 라도 즉시 끝난다(복사가 아니다).
    //   실패하면 옛 자리를 그대로 두고 넘어간다. 데이터를 잃느니 두 벌이 낫다.
    {
        const QString mine = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString base = QFileInfo(mine).absolutePath();   // 조직 이름을 뺐으므로 %APPDATA% · …/Application Support
        // 이 앱이 쓰던 폴더인지 판별한다.
        //
        //   ★ 반드시 '우리만 쓰는' 이름으로 봐야 한다.
        //     예전엔 llm / python_env_arm64 / script_overrides 같은 흔한 이름도
        //     표식에 넣고 '하나라도 맞으면' 우리 것으로 봤다. 조직 폴더를 빼면서
        //     바탕 폴더 전체(맥 Application Support 103개 · 윈도우 %APPDATA%)를 훑게 됐고,
        //     그 상태면 'llm' 폴더를 가진 다른 앱을 우리 것으로 오인해 그 앱의 데이터 폴더를
        //     통째로 우리 이름으로 rename 해 버린다. 남의 앱을 부수는 사고다(맥 b121906).
        //
        //   → 설정 파일이 있는 폴더만 인정한다. 이 이름은 우리만 쓴다.
        //     (앱이 한 번이라도 떴으면 반드시 있다. 없으면 가져올 것도 없다.)
        //     둘 다 있으면 최근에 쓴 쪽의 시각으로 본다.
        static const char *const marks[] = {"hanishiki_config.json", "miyo_config.json"};
        auto configIn = [](const QString &dir) -> QFileInfo {
            QFileInfo newest;
            for (const char *m : marks) {
                const QFileInfo f(dir + "/" + QString::fromLatin1(m));
                if (f.exists() && (!newest.exists() || f.lastModified() > newest.lastModified()))
                    newest = f;
            }
            return newest;
        };

        // ★ '새 자리가 비어 있을 때만' 으로는 부족하다(윈도우 실측).
        //   %APPDATA%\Predormition 에는 예전 버그가 남긴 로그 파일 하나가 이미 있었다
        //   (조직명을 정하기 전에 첫 메시지가 찍히던 시절의 잔재, 2026-08-08). 폴더가 '존재'한다는
        //   이유로 이사가 통째로 건너뛰어지고, 앱은 설정도 python_env 도 없는 빈 폴더를 쓰게 된다 —
        //   옛 자리에 11,951개 파일을 두고서. 그래서 '설정 파일이 있는가' 로 본다.
        if (!configIn(mine).exists()) {
            QFileInfo best;
            QDateTime bestTime;

            // 후보를 모은다.
            //   ① base 의 형제 폴더 — 앱 이름만 바뀐 경우(%APPDATA%\Predormition · %APPDATA%\Chernobyl …)
            //   ② 옛 조직 폴더 안 — 조직 이름을 쓰던 시절(%APPDATA%\Miyo\Predormition · Miyo\Chernobyl …)
            //      ★ 이게 없으면 조직 이름을 뺀 순간 옛 데이터를 못 찾는다.
            QList<QFileInfo> cands = QDir(base).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const char *org : {"Miyo", "ABIWA"}) {
                const QString orgDir = base + "/" + QString::fromLatin1(org);
                if (QFileInfo::exists(orgDir))
                    cands += QDir(orgDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
            }

            for (const QFileInfo &d : cands) {
                if (d.absoluteFilePath() == QFileInfo(mine).absoluteFilePath()) continue;
                if (d.fileName().contains(QLatin1String(".before-migration"))) continue;   // 아래에서 치워 둔 잔재
                const QFileInfo cfg = configIn(d.absoluteFilePath());
                if (!cfg.exists()) continue;
                const QDateTime t = cfg.lastModified();   // 최근에 쓰던 것(설정을 마지막으로 저장한 때)
                if (!best.exists() || t > bestTime) { bestTime = t; best = d; }
            }

            if (best.exists()) {
                QDir().mkpath(base);
                // 새 자리에 잔재가 있으면 rename 이 실패한다 — 옆으로 치우고, 옮긴 뒤 안의 것을 되돌린다.
                //   실제 상황(윈도우): AI 설치 스크립트(scripts/win_install_ai.ps1)가 앱 첫 실행보다
                //   먼저 돌면 새 자리\llm 에 모델을 최대 9GB 내려받는다. 비어 있는 새 폴더는 그냥 치운다.
                const QDir::Filters allEntries = QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden;
                QString parked;
                if (QFileInfo::exists(mine)) {
                    if (QDir(mine).isEmpty(allEntries)) {
                        QDir().rmdir(mine);
                    } else {
                        parked = mine + ".before-migration";
                        // 지난번에 되돌리지 못하고 남겨 둔 것이 있으면 지우지 않는다 — 다른 이름으로 치운다.
                        if (QFileInfo::exists(parked))
                            parked += QStringLiteral("-") + QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
                        if (!QDir().rename(mine, parked)) parked.clear();
                    }
                }
                if (QDir().rename(best.absoluteFilePath(), mine)) {
                    // 치워 둔 잔재를 새 폴더 안으로 되돌린다.
                    // ★ 예전에는 QDir::Files 만 되돌리고 곧바로 removeRecursively 를 불렀다.
                    //   그래서 치워 둔 자리에 '폴더'(llm 등)가 있으면 통째로 지워졌다.
                    //   실측: 같은 코드를 떼어 재현하니 model.gguf 가 사라지고 로그 파일만 남았다.
                    //   폴더도 함께 되돌리고, 되돌리지 못한 것이 남으면 지우지 않는다.
                    if (!parked.isEmpty()) {
                        QDir pd(parked);
                        for (const QString &e : pd.entryList(allEntries)) {
                            const QString from = parked + "/" + e;
                            const QString to   = mine + "/" + e;
                            // 옮겨 온 쪽에 같은 이름이 이미 있으면 건드리지 않는다 — 어느 쪽도 잃지 않기 위해서.
                            if (QFileInfo::exists(to)) continue;
                            if (!QDir().rename(from, to))
                                qWarning() << "[startup] 되돌리지 못한 항목:" << from;
                        }
                        if (pd.entryList(allEntries).isEmpty())
                            pd.removeRecursively();
                        else
                            qWarning() << "[startup] 되돌리지 못한 것이 남아 보관합니다 (직접 확인하세요):"
                                       << parked;
                    }
                    qInfo() << "[startup] 사용자 데이터 이어받음:" << best.absoluteFilePath() << "→" << mine;
                    // 비어 버린 옛 조직 폴더(Miyo 등)는 치운다 — 안에 다른 것이 있으면 rmdir 이 실패하므로 안전.
                    if (QFileInfo(best.absolutePath()).absoluteFilePath() != QFileInfo(base).absoluteFilePath())
                        QDir().rmdir(best.absolutePath());
                } else {
                    // 옮기지 못했다(옛 판이 켜져 있어 파일이 잡혀 있는 등) — 치워 둔 잔재를 제자리로 돌려
                    // 다음 실행이 같은 조건에서 다시 시도하게 한다.
                    if (!parked.isEmpty()) QDir().rename(parked, mine);
                    qWarning() << "[startup] 데이터 이어받기 실패 — 옛 폴더는 그대로 둡니다:" << best.absoluteFilePath();
                }
            }
        }
    }

#ifdef Q_OS_WIN
    qInstallMessageHandler(predormitionLogHandler);
    qInfo().noquote() << "[startup]" << QStringLiteral(APP_NAME_DISPLAY) << "starting — exe dir:"
                      << QCoreApplication::applicationDirPath();
#endif

    // ★ Mac 전용: Dock 컨벤션 — 창 닫아도 앱 살아있고 Dock 클릭으로 재오픈.
    //   Windows 에선 X 누르면 죽는 게 정상이라 기본값(true) 유지.
#ifdef Q_OS_MACOS
    app.setQuitOnLastWindowClosed(false);
#endif

    // Single instance lock — setStaleLockTime(0) ensures stale locks are auto-removed
    QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QLockFile lockFile(tmpDir + "/ABIWA.lock");
    lockFile.setStaleLockTime(0);  // Instantly detect dead process locks
    if (!lockFile.tryLock(100)) {
        // Already running — activate existing window
#ifdef Q_OS_MACOS
        // ★ 기존 인스턴스 활성화 — 실제 번들 ID 를 런타임에 읽어서 사용.
        //   하드코딩(com.chernobyl.app)은 -DAPP_ID 가 기본값(com.miyo.app)으로 빌드되면
        //   대상 없음으로 조용히 실패했음. NSBundle.bundleIdentifier 로 빌드와 무관하게 일치.
        id nsBundle = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(
            reinterpret_cast<id>(objc_getClass("NSBundle")), sel_registerName("mainBundle"));
        id nsId = nsBundle ? reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(
            nsBundle, sel_registerName("bundleIdentifier")) : nullptr;
        const char *bidC = nsId ? reinterpret_cast<const char *(*)(id, SEL)>(objc_msgSend)(
            nsId, sel_registerName("UTF8String")) : nullptr;
        const QString bundleId = bidC ? QString::fromUtf8(bidC) : QString();
        if (!bundleId.isEmpty()) {
            // 셸 미경유(QProcess argv). 번들 ID 는 우리 Info.plist 값(ASCII)이라 안전.
            QProcess::startDetached("/usr/bin/osascript",
                {"-e", QStringLiteral("tell application id \"%1\" to activate").arg(bundleId)});
        }
#elif defined(Q_OS_WIN)
        // ★ MainWindow 생성자의 setWindowTitle 과 같은 식이어야 기존 창을 잡는다.
        //   FindWindowW 는 제목이 글자까지 같아야 찾는다. 창 제목은 '<표시 이름> — <판 이름>' 이라
        //   표시 이름만으로 찾으면(맥 ueno 의 이 줄) 언제나 실패한다. 예전엔 옛 이름
        //   L"Predormition" 으로 찾고 있어, 이미 켜져 있는데 아이콘을 다시 누르면 아무 일도 없었다.
        const QString codename = QStringLiteral(PREDORMITION_CODENAME);
        QString title = QStringLiteral(APP_NAME_DISPLAY);
        if (!codename.isEmpty()) title += QStringLiteral(" — ") + codename;
        HWND hwnd = FindWindowW(nullptr, reinterpret_cast<const wchar_t *>(title.utf16()));
        if (hwnd) {
            SetForegroundWindow(hwnd);
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        }
#endif
        return 0;
    }

    // ★ 종료 신호를 Qt 의 정상 종료로 바꾼다.
    //   SIGTERM 은 기본 동작이 '즉시 죽음' 이라 aboutToQuit 이 돌지 않는다.
    //   그러면 뒤에서 돌던 서명 복구가 한창일 때 잘려 번들이 무효로 남는다
    //   (실측: 재서명 시작 5초 뒤 SIGTERM → 2초 만에 프로세스가 끝나고 봉인은 깨진 채).
    //   사람이 ⌘Q 로 끄면 원래 정상 경로지만, 시스템이 재시동·로그아웃으로 보내는
    //   SIGTERM 도 같은 대접을 받아야 한다.
    {
        auto onSignal = [](int) {
            // 신호 처리기 안에서는 Qt 를 직접 부르면 안 된다 — 안전한 방법으로 깨운다.
            QMetaObject::invokeMethod(QCoreApplication::instance(), "quit", Qt::QueuedConnection);
        };
        std::signal(SIGTERM, onSignal);
        std::signal(SIGINT,  onSignal);
    }

    // ★ 끌 때 SOCKS5 중계기를 반드시 거둔다.
    //   stopProxyRelay() 는 사용자가 프록시를 '끌 때' 만 불리고 있었다. 켠 채로
    //   앱을 끄면 중계기(socks_relay.py)가 그대로 남는다 — 인증 없는 로컬 프록시가
    //   계속 열려 있게 되고, 다음에 앱을 켜면 또 하나가 붙어 쌓인다.
    //   (실측: 8월 28일에 뜬 중계기가 9월 6일까지 살아 있었다.)
    //   위에서 SIGTERM 을 정상 종료로 바꿔 두었으므로 재시동·로그아웃도 여기를 탄다.
    QObject::connect(&app, &QCoreApplication::aboutToQuit,
                     []() { Common::markAppQuitting(); Common::stopProxyRelay(); });
    // ↑ 끝나는 중 표시를 맨 먼저 — 이 연결이 가장 먼저 걸리므로 다른 aboutToQuit 처리보다 앞선다(Common::appQuitting).

    // ★ TLS 를 주 스레드에서 미리 준비한다. 처음 쓰는 쪽이 뒷일 스레드면 그 스레드가 TLS 플러그인을 찾아 올리는데
    //   (QFactoryLoader), 그게 종료와 겹치면 치워지는 Qt 를 건드려 죽었다(2026-10-06 충돌 보고 — 고침 꾸러미 확인이
    //   켠 지 20초 뒤 첫 HTTPS 를 시작한 순간 앱이 끝나던 중). 이벤트 루프가 돌자마자 한 번.
    //   ★ 시스템 인증서까지 여기서 읽는다(defaultConfiguration → 키체인을 도는 긴 고리). 플러그인만 올려 두면 소용없다 —
    //     Qt 는 앱이 끝날 때 TLS 플러그인을 다시 치우고, 그 사이 뒷일 스레드가 인증서를 읽고 있으면 그 스레드가
    //     플러그인을 다시 찾다 죽는다(리뷰에서 잡힘). 인증서를 여기서 다 읽어 두면 뒷일 스레드의 TLS 준비는 순식간이다.
    QTimer::singleShot(0, &app, []() {
        const QString be = QSslSocket::activeBackend();
        const bool ok = QSslSocket::supportsSsl();
        const int cas = ok ? QSslConfiguration::defaultConfiguration().caCertificates().size() : 0;
        qInfo().noquote() << "[TLS] 주 스레드에서 미리 준비:" << (be.isEmpty() ? QStringLiteral("(없음)") : be)
                          << (ok ? QStringLiteral("· 인증서 %1개").arg(cas) : QStringLiteral("· 사용 불가"));
    });

    MainWindow window;
    window.show();

    // ★ 자가진단·자가복구 (백그라운드) — 번들 도구 검증→복구, stale lock 정리,
    //   복구 실패 항목은 로컬 LLM(Ollama/LM Studio/llama.cpp/번들 llm/)이 원인 진단.
    SelfRepair::runStartupMaintenanceAsync();

#ifdef Q_OS_MACOS
    // NSApplication delegate에 applicationShouldHandleReopen 메서드 추가
    // → Dock 아이콘 클릭 시 창이 다시 나타남
    g_mainWindow = &window;
    {
        id nsApp = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(
            reinterpret_cast<id>(objc_getClass("NSApplication")),
            sel_registerName("sharedApplication"));
        id delegate = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(nsApp, sel_registerName("delegate"));
        if (delegate) {
            Class delegateClass = object_getClass(delegate);
            SEL sel = sel_registerName("applicationShouldHandleReopen:hasVisibleWindows:");
            // ★ class_addMethod 는 그 클래스가 '이미 그 메서드를 갖고 있으면' 아무 일도
            //   하지 않고 NO 만 돌려준다. Qt 의 QCocoaApplicationDelegate 가 바로 이
            //   메서드를 이미 구현하고 있어서, 우리 처리기는 한 번도 설치된 적이 없었다.
            //   그래서 창을 닫으면 Dock 아이콘을 눌러도 창이 돌아오지 않았다 —
            //   앱은 살아 있는데 손이 닿지 않는 상태다. 사용자에겐 '튕긴 것' 으로 보인다.
            //   (실측: open -a 를 해도 CPU 가 0% 에서 꿈쩍하지 않았다)
            //   → class_replaceMethod 로 반드시 우리 것이 걸리게 한다.
            class_replaceMethod(delegateClass, sel, (IMP)appShouldHandleReopen, "B@:@B");
            const bool installed =
                class_getMethodImplementation(delegateClass, sel) == (IMP)appShouldHandleReopen;
            qInfo() << "[reopen] Dock 클릭 처리기 설치:" << (installed ? "성공" : "실패");
        } else {
            qWarning() << "[reopen] NSApplication delegate 없음 — Dock 클릭으로 창을 되살릴 수 없다";
        }
    }
#endif

    // Dock 클릭 시 창 다시 표시
    QObject::connect(&app, &QApplication::applicationStateChanged, [&window](Qt::ApplicationState state) {
        if (state == Qt::ApplicationActive) {
            if (!window.isVisible()) {
                window.show();
                window.raise();
                window.activateWindow();
            }
        }
    });

#ifdef Q_OS_MACOS
    // macOS Dock 우클릭 메뉴 등록 via NSApplication setDockMenu:
    {
        QMenu *dockMenu = window.createDockMenu();
        // QMenu::toNSMenu() returns native NSMenu*
        id nsMenu = reinterpret_cast<id>(dockMenu->toNSMenu());
        if (nsMenu) {
            id nsApp = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(
                reinterpret_cast<id>(objc_getClass("NSApplication")),
                sel_registerName("sharedApplication"));
            // [NSApp setDockMenu:nsMenu]
            reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(
                nsApp, sel_registerName("setDockMenu:"), nsMenu);
        }
    }
#endif

    int ret = app.exec();
    lockFile.unlock();
    return ret;
}
