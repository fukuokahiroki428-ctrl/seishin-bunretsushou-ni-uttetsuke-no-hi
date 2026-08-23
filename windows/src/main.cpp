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
    //   %APPDATA%/<조직명>/<앱이름> 이라, 이름을 정하기 전에 첫 메시지가 찍히면
    //   핸들러의 static QFile 이 엉뚱한 경로로 프로세스 내내 고정된다.
    //   진단 로그가 앱 데이터 폴더 밖에 쌓여, 문서가 안내하는 경로에는 아무것도 없었다.
    app.setApplicationName("Predormition");
    // ★ 조직명을 비운다 — 사용자 데이터 경로를 이름과 일치시킨다.
    //   Qt 는 %APPDATA%/<조직>/<앱> 을 쓰므로, 조직이 있으면 ...\Miyo\Predormition 이 되고
    //   조직을 "Predormition" 으로 두면 ...\Predormition\Predormition 으로 겹친다.
    //   비우면 %APPDATA%\Predormition 하나로 떨어진다.
    //   ※ 이 앱은 QSettings 를 쓰지 않으므로 조직명은 이 경로 계산 외에 쓰이는 데가 없다.
    app.setOrganizationName(QString());
    // ★ 로그 핸들러보다 먼저 이전해야 한다.
    //   핸들러가 로그 파일을 열면서 AppDataLocation 을 mkpath 해 버리는데,
    //   그러면 아래 '새 자리가 없을 때만 옮긴다' 조건이 거짓이 돼 이전이 통째로 건너뛰어진다.
    // ★ 사용자 데이터 자동 이전 — 이름이 바뀌어도 설정·토큰·AI 모델을 잃지 않는다.
    //   이 앱은 カメラ → チェルノブイリ → Chernobyl → Predormition 으로 네 번 이름이 바뀌었고,
    //   그때마다 AppDataLocation 이 따라 움직였다. 여기서 옛 자리를 새 자리로 옮긴다.
    //
    //   ★ 같은 볼륨 안 이동이라 QDir::rename 은 즉시 끝난다 — AI 모델이 9GB 라도 복사하지 않는다.
    //     실패하면 옛 자리를 그대로 두고 넘어간다. 데이터를 잃느니 두 벌이 낫다.
    {
        const QString appData = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString roaming = QFileInfo(appData).absolutePath();   // %APPDATA%
        const QStringList olds = {
            roaming + "/Miyo/Predormition",   // 조직명이 Miyo 이던 시절
            roaming + "/Miyo/Chernobyl",      // 그 전 이름
            roaming + "/Chernobyl",           // 조직명이 없던 더 이전
        };
        // ★ '새 자리가 없을 때만' 으로는 부족하다.
        //   실측: 이 기계의 %APPDATA%\Predormition 에는 예전 버그가 남긴 로그 파일 하나가
        //   이미 있었다(조직명을 정하기 전에 첫 메시지가 찍히던 시절의 잔재, 2026-08-08).
        //   폴더가 '존재'한다는 이유로 이전이 통째로 건너뛰어지고, 앱은 설정도 python_env 도
        //   없는 빈 폴더를 쓰게 된다 — 옛 자리에 11,951개 파일을 두고서.
        //
        //   그래서 '설정 파일이 있는가' 로 본다. miyo_config.json 은 우리만 쓰는 이름이고,
        //   앱이 한 번이라도 떴으면 반드시 있다. 없으면 아직 이사 전이다.
        //   ※ 맥 쪽은 이 판별을 폴더 이름 목록으로 넓혔다가 남의 앱 폴더를 가져가는 사고를
        //     냈다(b121906). 여기서는 옛 자리 목록이 세 개로 고정이라 그 위험은 없지만,
        //     판별 기준은 같은 이유로 설정 파일 하나로 좁힌다.
        const bool alreadyMoved = QFileInfo::exists(appData + "/miyo_config.json");
        if (!alreadyMoved) {
            for (const QString &oldDir : olds) {
                if (!QFileInfo::exists(oldDir)) continue;
                QDir().mkpath(QFileInfo(appData).absolutePath());
                // 잔재 폴더가 있으면 rename 이 실패한다 — 옆으로 치우고 옮긴 뒤 안의 것을 되돌린다.
                QString parked;
                if (QFileInfo::exists(appData)) {
                    parked = appData + ".before-migration";
                    QDir(parked).removeRecursively();
                    if (!QDir().rename(appData, parked)) parked.clear();
                }
                if (QDir().rename(oldDir, appData)) {
                    // 치워 둔 잔재(옛 로그 등)를 새 폴더 안으로 되돌린다. 실패해도 치명적이지 않다.
                    if (!parked.isEmpty()) {
                        QDir pd(parked);
                        for (const QString &f : pd.entryList(QDir::Files | QDir::Hidden))
                            QFile::rename(parked + "/" + f, appData + "/" + f);
                        pd.removeRecursively();
                    }
                    qInfo() << "[startup] user data migrated:" << oldDir << "->" << appData;
                    // 비어 버린 옛 조직 폴더는 치운다 (안에 다른 앱이 있으면 rmdir 이 실패하므로 안전)
                    QDir().rmdir(roaming + "/Miyo");
                } else {
                    qWarning() << "[startup] user data migration failed (옛 자리를 그대로 둡니다):" << oldDir;
                }
                break;
            }
        }
    }

#ifdef Q_OS_WIN
    qInstallMessageHandler(predormitionLogHandler);
    qInfo() << "[startup] Predormition starting — exe dir:" << QCoreApplication::applicationDirPath();
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
