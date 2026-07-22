#include <QApplication>
#include <QLockFile>
#include <QStandardPaths>
#include <QMenu>
#include <QSystemTrayIcon>
#include <QProcess>
#include <QFile>
#include <QDir>
#include <QFileInfo>
#include <QStringList>
#include "core/MainWindow.h"

#ifdef Q_OS_MACOS
#include <objc/objc.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <mach-o/dyld.h>   // _NSGetExecutablePath — QApplication 생성 전 실행경로 획득용
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

#ifdef Q_OS_MACOS
    // ★ 플러그인 경로 결정 — 번들(macdeployqt) 우선, 없으면 Homebrew fallback.
    //   번들 PlugIns 가 있는데 homebrew 를 QT_PLUGIN_PATH 로 강제하면 homebrew QtCore/QtGui 가
    //   같이 로드돼 "two sets of Qt" → SIGABRT 로 죽는다. (Pen 에는 있던 처리가 Chernobyl 엔 빠져 있었음)
    //   번들 cocoa 플러그인이 존재하면 아무것도 안 건드리고 Qt 자동탐색(Contents/PlugIns)에 맡긴다.
    {
        // QApplication 생성 전이라 applicationDirPath() 는 빈 문자열 → _NSGetExecutablePath 로 직접 계산.
        QString exeDir;
        {
            char buf[4096]; uint32_t sz = sizeof(buf);
            if (_NSGetExecutablePath(buf, &sz) == 0)
                exeDir = QFileInfo(QString::fromLocal8Bit(buf)).canonicalPath();  // .../Contents/MacOS
        }
        const QString bundledPlugins = exeDir + "/../PlugIns";
        const bool hasBundledCocoa =
            !exeDir.isEmpty() && QFile::exists(bundledPlugins + "/platforms/libqcocoa.dylib");
        if (hasBundledCocoa) {
            // 배포 모드: 번들 플러그인만 사용 (homebrew 오염 차단)
            qputenv("QT_PLUGIN_PATH", QDir(bundledPlugins).absolutePath().toUtf8());
        } else if (qEnvironmentVariableIsEmpty("QT_PLUGIN_PATH")) {
            // 개발 모드: 번들 없음 → 시스템 Homebrew Qt 플러그인 fallback
            const QStringList candidates = {
                "/opt/homebrew/share/qt/plugins",
                "/usr/local/share/qt/plugins"
            };
            for (const QString &p : candidates) {
                if (QFile::exists(p + "/platforms/libqcocoa.dylib")) {
                    qputenv("QT_PLUGIN_PATH", p.toUtf8());
                    qputenv("QT_QPA_PLATFORM_PLUGIN_PATH", (p + "/platforms").toUtf8());
                    break;
                }
            }
        }
    }
#endif

    QApplication app(argc, argv);
    app.setApplicationName("Chernobyl");
    app.setOrganizationName("Miyo");
    app.setQuitOnLastWindowClosed(false);

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
        HWND hwnd = FindWindowW(nullptr, L"ABIWA");
        if (hwnd) {
            SetForegroundWindow(hwnd);
            if (IsIconic(hwnd)) ShowWindow(hwnd, SW_RESTORE);
        }
#endif
        return 0;
    }

    MainWindow window;
    window.show();

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
