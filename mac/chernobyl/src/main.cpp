#include <QApplication>
#include <csignal>
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
#include "utils/SelfRepair.h"   // ★ 자가진단·자가복구 + 로컬 LLM 진단

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
    // ★ 이름은 빌드에서 온다(APP_NAME_ASCII). 여기 박아 두면 CMake 의 이름과
    //   어긋나 데이터 폴더가 갈라진다 — 실제로 그렇게 갈라진 적이 있다.
    //   폴더 이름은 ASCII 로 둔다. 일본어 폴더는 NAS·백업 경로에서 NFC/NFD 로
    //   어긋나 사고가 나기 쉽다(사용자에게 보이는 이름은 번들이 따로 갖는다).
    app.setApplicationName(QStringLiteral(APP_NAME_ASCII));
    // ★ 조직 이름을 두지 않는다.
    //   두면 데이터가 …/Application Support/Miyo/<앱> 처럼 한 겹 더 들어간다.
    //   'Miyo' 는 이 앱이 카메라이던 시절의 흔적일 뿐 지금 이름과 아무 관계가 없어서,
    //   폴더를 열어 본 사람이 무엇인지 알 수 없었다.
    //   비워 두면 Qt 가 …/Application Support/<앱이름> 을 쓴다.
    //   ★ 옛 위치(Miyo/<앱>)의 데이터는 아래 이사 코드가 찾아서 그대로 옮겨 온다 —
    //     설정·토큰뿐 아니라 AI 모델 9GB·색인·크롬 프로필이 거기 있다.
    app.setOrganizationName(QString());

    // ★ 앱 이름이 바뀌면 사용자 데이터 폴더도 바뀐다 — 통째로 이어받는다.
    //
    //   AppDataLocation 은 .../Miyo/<앱이름> 이다. 이름을 바꾸는 순간 새 폴더가
    //   생기고, 예전 것은 그 자리에 남아 아무도 안 본다. 거기에는 설정만 있는 게
    //   아니라 AI 모델(약 9GB)·python_env·크롬 프로필·색인 DB 가 전부 들어 있다.
    //   그대로 두면 사용자는 AI 를 다시 받고 로그인을 다시 해야 한다.
    //
    //   ★ 옛 이름을 코드에 박으면 안 된다. 예전엔 "Chernobyl" 이 박혀 있었는데,
    //     이 앱은 이미 다섯 번째 이름이다(カメラ → チェルノブイリ → Chernobyl →
    //     Predormition → …). 박아 두면 다음 변경 때 또 같은 일이 난다.
    //     그래서 형제 폴더 중 '가장 최근에 쓰던 것' 을 찾아 이어받는다.
    //
    //   같은 디스크 안 rename 이라 9GB 라도 즉시 끝난다(복사가 아니다).
    {
        const QString mine = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        const QString base = QFileInfo(mine).absolutePath();   // 조직 이름을 뺐으므로 …/Application Support
        const bool mineEmpty = !QFileInfo::exists(mine)
                               || QDir(mine).isEmpty(QDir::AllEntries | QDir::NoDotAndDotDot);
        if (mineEmpty) {
            QFileInfo best;
            qint64 bestTime = 0;

            // 후보를 모은다.
            //   ① base 의 형제 폴더 — 앱 이름만 바뀐 경우(…/Application Support/<옛이름>)
            //   ② 옛 조직 폴더 안 — 조직 이름을 쓰던 시절(…/Application Support/Miyo/<앱>)
            //      ★ 이게 없으면 조직 이름을 뺀 순간 옛 데이터를 못 찾는다.
            //        거기엔 설정·토큰뿐 아니라 AI 모델 9GB·색인·크롬 프로필이 들어 있다.
            QList<QFileInfo> cands = QDir(base).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
            for (const char *org : {"Miyo", "ABIWA"}) {
                const QString orgDir = base + "/" + QString::fromLatin1(org);
                if (QFileInfo::exists(orgDir))
                    cands += QDir(orgDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
            }

            const auto dirs = cands;
            for (const QFileInfo &d : dirs) {
                if (d.absoluteFilePath() == QFileInfo(mine).absoluteFilePath()) continue;
                // 이 앱이 쓰던 폴더인지 판별한다.
                //
                //   ★ 반드시 '우리만 쓰는' 이름으로 봐야 한다.
                //     예전엔 llm / python_env_arm64 / script_overrides 같은 흔한 이름도
                //     표식에 넣고 '하나라도 맞으면' 우리 것으로 봤다. 조직 폴더를 쓰던
                //     시절엔 Miyo 아래만 훑어서 문제가 없었지만, 조직 폴더를 빼면서
                //     Application Support 전체(이 맥에서만 103개)를 훑게 됐다.
                //     그 상태로 두면 'llm' 폴더를 가진 다른 앱을 우리 것으로 오인해
                //     그 앱의 데이터 폴더를 통째로 우리 이름으로 rename 해 버린다.
                //     남의 앱을 부수는 사고다.
                //
                //   → 설정 파일이 있는 폴더만 인정한다. 이 이름은 우리만 쓴다.
                //     (앱이 한 번이라도 떴으면 반드시 있다. 없으면 가져올 것도 없다.)
                static const char *marks[] = {"hanishiki_config.json", "miyo_config.json"};
                bool ours = false;
                for (const char *m : marks)
                    if (QFileInfo::exists(d.absoluteFilePath() + "/" + QString::fromLatin1(m))) { ours = true; break; }
                if (!ours) continue;
                const qint64 t = d.lastModified().toMSecsSinceEpoch();
                if (t > bestTime) { bestTime = t; best = d; }
            }
            if (best.exists()) {
                QDir().mkpath(base);
                if (QFileInfo::exists(mine)) QDir(mine).removeRecursively();   // 비어 있는 새 폴더
                if (QDir().rename(best.absoluteFilePath(), mine))
                    qInfo() << "[startup] 사용자 데이터 이어받음:" << best.fileName() << "→" << QFileInfo(mine).fileName();
                else
                    qWarning() << "[startup] 데이터 이어받기 실패 — 옛 폴더는 그대로 둡니다:" << best.absoluteFilePath();
            }
        }
    }
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
        // ★ 옛 이름(カメラ)으로 찾고 있었다 — 지금 창 제목은 "<앱이름> — <판>" 이라
        //   이 호출은 언제나 실패했다. 실제 표시 이름으로 찾는다.
        HWND hwnd = FindWindowW(nullptr,
                                reinterpret_cast<const wchar_t *>(
                                    QStringLiteral(APP_NAME_DISPLAY).utf16()));
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
