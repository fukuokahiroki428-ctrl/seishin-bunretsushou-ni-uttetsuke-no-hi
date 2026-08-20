#include <QApplication>
#include <QLockFile>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QMenu>
#include <QSystemTrayIcon>
#include "core/MainWindow.h"
#include "utils/SelfRepair.h"   // ★ 자가진단·자가복구 + 로컬 LLM 진단

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
//   를 <exe>/chernobyl_log.txt 에 기록. "아무것도 안 됨" 원인을 GUI subsystem 에서도 잡기 위함.
static void chernobylLogHandler(QtMsgType type, const QMessageLogContext &, const QString &msg)
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
    if (logFile && logFile->isOpen()) {
        const QByteArray line = (QDateTime::currentDateTime().toString("HH:mm:ss.zzz")
                                 + " [" + lvl + "] " + msg + "\n").toUtf8();
        logFile->write(line);
        logFile->flush();
    }
    fprintf(stderr, "[%s] %s\n", lvl, msg.toLocal8Bit().constData());
}
#endif

int main(int argc, char *argv[])
{
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
        "--memory-pressure-off "
        "--js-flags=--max-old-space-size=384 "
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
        "--memory-pressure-off "
        "--js-flags=--max-old-space-size=384 "
        "--disk-cache-size=10485760 "
        "--media-cache-size=5242880 "
        "--aggressive-cache-discard "
        "--num-raster-threads=2");
#endif
    }

    QApplication app(argc, argv);
    // ★ 로그 핸들러보다 반드시 먼저 설정한다. Windows 의 AppDataLocation 은
    //   %APPDATA%/<조직명>/<앱이름> 이라, 이 두 줄 전에 첫 메시지가 찍히면 조직명이 빈 채로
    //   경로가 계산되고, 핸들러의 static QFile 이 %APPDATA%/Predormition (Miyo 누락) 으로
    //   프로세스 내내 고정된다. 진단 로그가 앱 데이터 폴더 밖에 쌓여, 크래시 조사 시
    //   문서가 안내하는 경로에는 아무것도 없었다.
    app.setApplicationName("Predormition");
    app.setOrganizationName("Miyo");
#ifdef Q_OS_WIN
    qInstallMessageHandler(chernobylLogHandler);
    qInfo() << "[startup] Predormition starting — exe dir:" << QCoreApplication::applicationDirPath();
#endif

    // ★ 앱 이름 변경(Chernobyl → Predormition) 시 사용자 데이터 자동 이전.
    //   AppDataLocation 이 ...\Miyo\Chernobyl → ...\Miyo\Predormition 로 바뀌므로,
    //   새 폴더가 없고 옛 폴더가 있으면 통째로 옮긴다(설정·토큰·AI 수정본 유지).
    {
        const QString base = QFileInfo(
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)).absolutePath();
        const QString oldDir = base + "/Chernobyl";
        const QString newDir = base + "/Predormition";
        if (!QFileInfo::exists(newDir) && QFileInfo::exists(oldDir)) {
            if (QDir().rename(oldDir, newDir))
                qInfo() << "[startup] user data migrated:" << oldDir << "→" << newDir;
            else
                qWarning() << "[startup] user data migration failed";
        }
    }
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
        // ★ 번들 식별자(com.chernobyl.app) 기반으로 활성화 — 번들 이름(ASCII/일본어) 무관
        system("osascript -e 'tell application id \"com.chernobyl.app\" to activate' &");
#elif defined(Q_OS_WIN)
        // ★ MainWindow::setWindowTitle 과 반드시 같아야 기존 창을 잡는다.
    //   이름을 Predormition 으로 바꿀 때 여기만 빠져서, 이미 켜져 있는데 아이콘을 다시
    //   누르면 창이 앞으로 나오지 않고 아무 일도 안 일어났다.
        //   (/utf-8 컴파일 플래그 덕분에 wide 리터럴 L"Predormition" 가 올바른 UTF-16 으로 들어감)
        HWND hwnd = FindWindowW(nullptr, L"Predormition");
        if (hwnd) {
            SetForegroundWindow(hwnd);
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        }
#endif
        return 0;
    }

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
            class_addMethod(delegateClass,
                sel_registerName("applicationShouldHandleReopen:hasVisibleWindows:"),
                (IMP)appShouldHandleReopen, "B@:@B");
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
