#include <QApplication>
#include <QLockFile>
#include <QStandardPaths>
#include <QMenu>
#include <QFile>
#include <QDir>
#include <QStringList>
#include "core/MainWindow.h"

#ifdef Q_OS_MACOS
#include <objc/objc.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <mach-o/dyld.h>   // _NSGetExecutablePath — QApplication 생성 전 실행경로 획득용
#include <QFileInfo>
#endif

#ifdef Q_OS_WIN
#include <windows.h>
#endif

#ifdef Q_OS_MACOS
static MainWindow *g_mainWindow = nullptr;

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
    qputenv("QTWEBENGINE_CHROMIUM_FLAGS",
        "--enable-gpu-rasterization "
        "--enable-zero-copy "
        "--enable-native-gpu-memory-buffers "
        "--ignore-gpu-blocklist "
        "--disable-gpu-driver-bug-workarounds "
        "--num-raster-threads=4");

#ifdef Q_OS_MACOS
    // ★ 플러그인 경로 결정 — 번들(macdeployqt) 우선, 없으면 Homebrew fallback.
    //   번들 PlugIns 가 있는데도 homebrew 를 QT_PLUGIN_PATH 로 강제하면
    //   homebrew QtCore/QtGui 가 같이 로드돼 "two sets of Qt" → SIGABRT 크래시.
    //   번들 cocoa 플러그인이 존재하면 아무것도 안 건드리고 Qt 자동탐색(Contents/PlugIns)에 맡긴다.
    {
        // ★ QApplication 생성 전이라 QCoreApplication::applicationDirPath() 는 빈 문자열.
        //   _NSGetExecutablePath 로 실행파일 경로를 직접 구해 번들 PlugIns 위치 계산.
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
    app.setApplicationName("Pen");
    app.setOrganizationName("Pen");
    app.setQuitOnLastWindowClosed(false);

    QString tmpDir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    QLockFile lockFile(tmpDir + "/PEN_APP.lock");
    lockFile.setStaleLockTime(0);
    if (!lockFile.tryLock(100)) {
#ifdef Q_OS_MACOS
        // bundle identifier 기반 활성화 — 번들 이름과 무관
        system("osascript -e 'tell application id \"com.pen.app\" to activate' &");
#elif defined(Q_OS_WIN)
        HWND hwnd = FindWindowW(nullptr, L"팬을 잘 쓰고 싶다");
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
    {
        QMenu *dockMenu = window.createDockMenu();
        id nsMenu = reinterpret_cast<id>(dockMenu->toNSMenu());
        if (nsMenu) {
            id nsApp = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(
                reinterpret_cast<id>(objc_getClass("NSApplication")),
                sel_registerName("sharedApplication"));
            reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(
                nsApp, sel_registerName("setDockMenu:"), nsMenu);
        }
    }
#endif

    int ret = app.exec();
    lockFile.unlock();
    return ret;
}
