#include <QApplication>
#include <QLockFile>
#include <QStandardPaths>
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
        logFile = new QFile(QCoreApplication::applicationDirPath() + "/chernobyl_log.txt");
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

    QApplication app(argc, argv);
#ifdef Q_OS_WIN
    qInstallMessageHandler(chernobylLogHandler);
    qInfo() << "[startup] Chernobyl starting — exe dir:" << QCoreApplication::applicationDirPath();
#endif
    app.setApplicationName("Chernobyl");
    app.setOrganizationName("Miyo");
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
        // 윈도우 제목은 MainWindow 에서 "カメラ" 로 설정됨 — 여기 문자열과 반드시 일치해야 기존 창을 잡음.
        //   (/utf-8 컴파일 플래그 덕분에 wide 리터럴 L"カメラ" 가 올바른 UTF-16 으로 들어감)
        HWND hwnd = FindWindowW(nullptr, L"カメラ");
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
