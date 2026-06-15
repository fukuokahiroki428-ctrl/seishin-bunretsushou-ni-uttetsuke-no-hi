#include "MainWindow.h"
#include "MiyoBackend.h"
#include "PenBackend.h"   // ★ PEN(팬을 잘 쓰고 싶다) 통합

#include <QWebEngineSettings>
#include <QVBoxLayout>
#include <QMenuBar>
#include <QAction>
#include <QFileDialog>
#include <QTimer>
#include <QProcess>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QCloseEvent>
#include <QMimeData>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDebug>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMessageBox>
#include <QApplication>
#include <QWebEngineProfile>
#include <QWebEnginePage>
#include <QStandardPaths>

// ★ JS 콘솔 메시지를 stderr 로 redirect — SyntaxError line 번호 등 추적
class DebugWebEnginePage : public QWebEnginePage {
public:
    using QWebEnginePage::QWebEnginePage;
protected:
    void javaScriptConsoleMessage(JavaScriptConsoleMessageLevel level,
                                  const QString &message, int lineNumber,
                                  const QString &sourceID) override {
        const char *lvl = level == ErrorMessageLevel ? "JS-ERROR"
                        : level == WarningMessageLevel ? "JS-WARN" : "JS-LOG";
        qDebug().noquote() << QString("[%1] %2:%3 — %4")
            .arg(lvl, sourceID.section('/', -1), QString::number(lineNumber), message);
    }
};

#ifdef Q_OS_MACOS
#include <objc/objc.h>
#include <objc/message.h>
#elif defined(Q_OS_WIN)
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")
#endif

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setWindowTitle("カメラ");
    // ★ 새 대시보드 UI 는 와이드 데스크톱 레이아웃 (사이드바 246px + 대시보드 2단 그리드 720+300px).
    //   옛 단일컬럼 시절 420px 기본값이면 내용이 세로로 찌그러짐 → 와이드 기본/최소로 조정.
    setMinimumSize(1024, 660);
    resize(1280, 820);

    // ★ Frameless — 네이티브 흰 타이틀바 제거. HTML 의 다크 툴바가 곧 타이틀바.
    //   드래그/리사이즈는 JS 가 backend.winStartMove()/winStartResize() 로
    //   QWindow::startSystemMove()/startSystemResize() 를 호출 → 네이티브 스냅·이동 유지.
    setWindowFlag(Qt::FramelessWindowHint);

    // QMainWindow 배경 — 흰색 (HTML 페이지 배경과 일치).
    //   이전: #0A0A0A (검은색) → 창 가장자리/타이틀바 주변에 검은 띠가 보임 → 사용자 불만
    setStyleSheet(R"(
        QMainWindow {
            background-color: #ffffff;
        }
        QDockWidget {
            background-color: #1A1A1A;
            color: #FFFFFF;
            font-size: 12px;
        }
        QDockWidget::title {
            background-color: #252525;
            padding: 8px;
            font-weight: bold;
        }
        QTextEdit {
            background-color: #0A0A0A;
            color: #D4D4D4;
            border: none;
            font-family: 'SF Mono', Monaco, 'Courier New', monospace;
            font-size: 11px;
            padding: 10px;
        }
    )");

    // Central widget
    auto *central = new QWidget(this);
    setCentralWidget(central);
    auto *layout = new QVBoxLayout(central);
    layout->setContentsMargins(0, 0, 0, 0);

    // WebEngineView (main UI)
    m_webView = new QWebEngineView(this);
    // ★ JS 콘솔 메시지 stderr 로 redirect — debug 용
    m_webView->setPage(new DebugWebEnginePage(m_webView));
    // 우클릭 메뉴 비활성화 (Reload/Inspect/View Source 같은 컨텍스트 메뉴 안 뜸)
    m_webView->setContextMenuPolicy(Qt::NoContextMenu);

    // WebChannel
    m_channel = new QWebChannel(this);
    m_backend = new MiyoBackend(this);
    m_channel->registerObject(QStringLiteral("backend"), m_backend);
    // ★ PEN(팬을 잘 쓰고 싶다) 통합 — 같은 페이지에 2번째 백엔드 객체로 노출.
    //   UI 의 사이트 미러 탭이 penBackend.crawl* 를 호출. runJs/log 는 jsSignal/logSignal
    //   로 동작(JS 측에서 connect) — MiyoBackend 와 동일 메커니즘, 같은 webView 공유.
    m_penBackend = new PenBackend(this);
    m_channel->registerObject(QStringLiteral("penBackend"), m_penBackend);
    m_webView->page()->setWebChannel(m_channel);

    // ★ file:// 페이지에서 qrc:// 리소스 (qwebchannel.js, 폰트 등) 접근 허용 — CORS 우회
    m_webView->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    m_webView->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);

    // ★ HTML 로딩 — 외부 파일 우선 (개발/디버그 시 즉시 반영), qrc fallback
    //   AUTORCC 의존성 추적이 깨지면 qrc 안 html 이 옛 버전으로 남아 디버깅 지옥.
    //   먼저 app 번들 옆 (또는 resources 폴더) 의 index.html 시도, 없으면 qrc.
    QString externalHtml;
    QStringList candidates;
#ifdef Q_OS_MACOS
    // .app/Contents/Resources/html/index.html (옆 외부 파일 — codesign 후에도 OK)
    candidates << QCoreApplication::applicationDirPath() + "/../Resources/html/index.html";
#endif
#ifdef Q_OS_WIN
    // ★ Windows: Deploy 단계가 html 을 <exe>/html/ 로 복사 → file:// 로 로드 (mac 과 동일 동작).
    //   qrc:// 폴백 시 WebChannel transport(qt.webChannelTransport)가 origin 차이로 안 붙어
    //   "UI 는 보이는데 모든 버튼이 죽는" 현상 발생 → 반드시 file:// 우선.
    candidates << QCoreApplication::applicationDirPath() + "/html/index.html";
#endif
    // 빌드 디렉토리 옆 — 개발 중 직접 빌드 시 (CMAKE_SOURCE_DIR 의 resources)
    candidates << QCoreApplication::applicationDirPath() + "/../../../resources/html/index.html";
    candidates << QCoreApplication::applicationDirPath() + "/resources/html/index.html";
    for (const QString &p : candidates) {
        if (QFileInfo::exists(p)) { externalHtml = QFileInfo(p).absoluteFilePath(); break; }
    }
    if (!externalHtml.isEmpty()) {
        m_webView->setUrl(QUrl::fromLocalFile(externalHtml));
        qDebug() << "[HTML] loading external:" << externalHtml;
    } else {
        m_webView->setUrl(QUrl("qrc:/html/index.html"));
        qDebug() << "[HTML] loading qrc:/html/index.html (no external found)";
    }

    layout->addWidget(m_webView);

    // ★ 내장 QWebEngine 브라우저 제거 — 캡쳐는 별도 CDP Chrome 사용 중이라 불필요.
    //   이전엔 QWebEngineProfile + 512MB cache + chromium 백그라운드 → ~800MB 메모리.
    //   8GB Mac에서 macOS jetsam이 launch 직후 SIGKILL 원인.
    //   showBrowser/browserView API는 nullptr 체크 (호환).
    m_browserWindow = nullptr;
    m_browserView = nullptr;

    // Enable drag & drop: install event filter on web view's child (RenderWidgetHostViewQtDelegateWidget)
    m_webView->setAcceptDrops(true);
    m_webView->installEventFilter(this);

    // Menu bar
    setupMenu();
#ifdef Q_OS_WIN
    // frameless 에선 네이티브 메뉴바가 콘텐츠 맨 위에 떠서 보기 안 좋음 → 숨김.
    //   (QAction 단축키 Ctrl+O/Ctrl+Q 는 메뉴바를 숨겨도 그대로 동작)
    menuBar()->hide();
#endif

    // Apply dark titlebar after show
    QTimer::singleShot(0, this, &MainWindow::applyDarkTitlebar);

    // Dock 메뉴 생성 (macOS Dock 우클릭 시 표시)
    m_dockMenu = createDockMenu();

    // ★ 앱 시작 시 sleep 방지 어설션 자동 활성 — collection 안 돌고 있어도 항상 활성
    //   lid close 시 sleep 막는 데 최대 효과. (Apple 정책상 100% 보장은 외부 모니터 필요)
    QTimer::singleShot(500, this, &MainWindow::holdAwake);
}

QMenu *MainWindow::createDockMenu()
{
    auto *menu = new QMenu(this);

    auto *showAction = menu->addAction("カメラ 열기");
    connect(showAction, &QAction::triggered, this, [this]() {
        show();
        raise();
        activateWindow();
    });

    menu->addSeparator();

    // 상태 표시 (업데이트됨)
    auto *statusAction = menu->addAction("상태: 대기");
    statusAction->setEnabled(false);
    statusAction->setObjectName("dockStatus");

    auto *statsAction = menu->addAction("");
    statsAction->setEnabled(false);
    statsAction->setObjectName("dockStats");
    statsAction->setVisible(false);

    menu->addSeparator();

    auto *stopAction = menu->addAction("수집 중지");
    stopAction->setObjectName("dockStop");
    stopAction->setEnabled(false);
    connect(stopAction, &QAction::triggered, this, [this]() {
        if (m_backend) {
            // 모든 실행 중인 플랫폼 중지
            for (const auto &p : {"twitter", "bluesky", "youtube", "discord", "instagram", "crawl"}) {
                m_backend->stopCollection(p);
            }
        }
    });

    return menu;
}

void MainWindow::updateDockMenu()
{
    if (!m_dockMenu) return;

    auto *statusAction = m_dockMenu->findChild<QAction*>("dockStatus");
    auto *statsAction = m_dockMenu->findChild<QAction*>("dockStats");
    auto *stopAction = m_dockMenu->findChild<QAction*>("dockStop");

    bool running = m_backend && m_backend->isAnyRunning();

    if (statusAction) {
        statusAction->setText(running ? "상태: 수집 중..." : "상태: 대기");
    }
    if (stopAction) {
        stopAction->setEnabled(running);
    }
}

void MainWindow::showBrowser(bool show)
{
    if (!m_browserWindow) return;
    if (show) {
        m_browserWindow->show();
        m_browserWindow->raise();
        m_browserWindow->activateWindow();
    } else {
        m_browserWindow->hide();
    }
}

MainWindow::~MainWindow()
{
    releaseAwake();
    QApplication::quit();
}

void MainWindow::setupMenu()
{
    auto *menubar = menuBar();

    // File menu
    auto *fileMenu = menubar->addMenu("File");

    auto *openFolder = new QAction("Open Folder...", this);
    openFolder->setShortcut(QKeySequence("Ctrl+O"));
    connect(openFolder, &QAction::triggered, this, &MainWindow::openFolderDialog);
    fileMenu->addAction(openFolder);

    fileMenu->addSeparator();

    auto *quitAction = new QAction("Quit", this);
    quitAction->setShortcut(QKeySequence("Ctrl+Q"));
    connect(quitAction, &QAction::triggered, this, &QMainWindow::close);
    fileMenu->addAction(quitAction);

    // ★ Tools menu (anipo / AINU) 제거 — companion apps 미사용 + 번들에 포함 안 됨.
    //   소스 폴더 (485MB) 도 삭제됨. openExternalApp 함수도 같이 제거.
}

void MainWindow::openFolderDialog()
{
    QString folder = QFileDialog::getExistingDirectory(this, "Open Folder");
    if (!folder.isEmpty()) {
#ifdef Q_OS_MACOS
        QProcess::startDetached("open", {folder});
#elif defined(Q_OS_WIN)
        QProcess::startDetached("explorer", {folder});
#else
        QProcess::startDetached("xdg-open", {folder});
#endif
    }
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    if (m_backend && m_backend->isAnyRunning()) {
        auto reply = QMessageBox::question(
            this,
            "カメラ",
            "수집이 진행 중입니다. 종료하시겠습니까?",
            QMessageBox::Yes | QMessageBox::No,
            QMessageBox::No
        );
        if (reply == QMessageBox::No) {
            event->ignore();
            return;
        }
    }

    // 브라우저 창 닫기
    if (m_browserWindow) m_browserWindow->close();

    // 모든 열린 터미널 로그 닫기 ([DONE] 마커 → 스크립트 자동 종료)
    if (m_backend) {
        m_backend->closeAllTerminalLogs();
    }

    // macOS: Terminal.app에서 ABIWA 관련 탭/창 닫기
#ifdef Q_OS_MACOS
    QProcess::startDetached("/usr/bin/osascript", {"-e",
        "tell application \"Terminal\"\n"
        "  repeat with w in windows\n"
        "    repeat with t in tabs of w\n"
        "      if name of t contains \"miyo_\" or name of t contains \"ABIWA\" then\n"
        "        do script \"exit\" in t\n"
        "      end if\n"
        "    end repeat\n"
        "  end repeat\n"
        "end tell"
    });
#endif

    event->accept();
}

void MainWindow::holdAwake()
{
#ifdef Q_OS_MACOS
    if (m_sleepAssertionHeld) return;

    // 1. IOPMAssertion — Prevent System Sleep
    CFStringRef reason = CFSTR("Chernobyl active — preventing sleep");
    IOReturn result = IOPMAssertionCreateWithName(
        kIOPMAssertionTypePreventSystemSleep,
        kIOPMAssertionLevelOn,
        reason,
        &m_sleepAssertion
    );
    if (result == kIOReturnSuccess) {
        m_sleepAssertionHeld = true;
    }

    // 2. UserIsActive 어설션 — idle 카운터 리셋 (lid close 직전 효과)
    static IOPMAssertionID userActiveAssertion = 0;
    IOPMAssertionCreateWithName(
        kIOPMAssertionTypeNoIdleSleep,  // 두 번째 layer
        kIOPMAssertionLevelOn,
        CFSTR("Chernobyl user activity"),
        &userActiveAssertion
    );

    // 3. caffeinate — 가능한 모든 sleep 방지 + lid close (AC 전원 시)
    //    -d display, -i idle, -m disk, -s system, -u user-active
    if (!m_caffeinate) {
        m_caffeinate = new QProcess(this);
        QStringList args{"-dimsu", "-w", QString::number(QCoreApplication::applicationPid())};
        m_caffeinate->start("/usr/bin/caffeinate", args);
        if (!m_caffeinate->waitForStarted(3000)) {
            delete m_caffeinate;
            m_caffeinate = nullptr;
        }
    }
#elif defined(Q_OS_WIN)
    if (m_sleepAssertionHeld) return;
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    m_sleepAssertionHeld = true;
#endif
}

void MainWindow::releaseAwake()
{
#ifdef Q_OS_MACOS
    if (!m_sleepAssertionHeld) return;

    IOPMAssertionRelease(m_sleepAssertion);
    m_sleepAssertion = 0;
    m_sleepAssertionHeld = false;

    // caffeinate 종료
    if (m_caffeinate) {
        m_caffeinate->terminate();
        m_caffeinate->waitForFinished(2000);
        if (m_caffeinate->state() == QProcess::Running) m_caffeinate->kill();
        delete m_caffeinate;
        m_caffeinate = nullptr;
    }
#elif defined(Q_OS_WIN)
    if (!m_sleepAssertionHeld) return;
    SetThreadExecutionState(ES_CONTINUOUS);
    m_sleepAssertionHeld = false;
#endif
}

bool MainWindow::eventFilter(QObject *obj, QEvent *event)
{
    if (event->type() == QEvent::DragEnter) {
        auto *e = static_cast<QDragEnterEvent *>(event);
        if (e->mimeData()->hasUrls()) {
            e->acceptProposedAction();
            return true;
        }
    } else if (event->type() == QEvent::Drop) {
        auto *e = static_cast<QDropEvent *>(event);
        if (e->mimeData()->hasUrls()) {
            QJsonArray fileArray;
            for (const QUrl &url : e->mimeData()->urls()) {
                QString filePath = url.toLocalFile();
                if (filePath.isEmpty()) continue;
                QFileInfo info(filePath);
                if (!info.exists()) continue;
                if (info.isDir()) {
                    // 폴더: 하위 파일 전부 추가
                    QDirIterator it(info.absoluteFilePath(), QDir::Files, QDirIterator::Subdirectories);
                    while (it.hasNext()) {
                        it.next();
                        QFileInfo fi = it.fileInfo();
                        QJsonObject obj;
                        obj["name"] = fi.fileName();
                        obj["path"] = fi.absoluteFilePath();
                        obj["size"] = fi.size();
                        fileArray.append(obj);
                    }
                    continue;
                }
                QJsonObject obj;
                obj["name"] = info.fileName();
                obj["path"] = info.absoluteFilePath();
                obj["size"] = info.size();
                fileArray.append(obj);
            }
            if (!fileArray.isEmpty()) {
                QByteArray jsonBytes = QJsonDocument(fileArray).toJson(QJsonDocument::Compact);
                QString b64 = QString::fromLatin1(jsonBytes.toBase64());
                m_backend->runJs(QString("setTradFiles(b64toUtf8('%1'))").arg(b64));
            }
            e->acceptProposedAction();
            return true;
        }
    } else if (event->type() == QEvent::ChildAdded) {
        // Install event filter on child widgets (QWebEngineView creates child widgets for rendering)
        auto *child = static_cast<QChildEvent *>(event)->child();
        if (child->isWidgetType()) {
            child->installEventFilter(this);
        }
    }
    return QMainWindow::eventFilter(obj, event);
}

void MainWindow::applyDarkTitlebar()
{
    // ★ 사용자 요청: 타이틀바를 LIGHT(흰색)로 — HTML 콘텐츠와 색상 일치
    //   이전: dark titlebar로 강제 → 흰 콘텐츠 위에 검은 띠가 보여서 거슬림
#ifdef Q_OS_MACOS
    id nsApp = reinterpret_cast<id>(objc_getClass("NSApplication"));
    SEL sharedAppSel = sel_registerName("sharedApplication");
    id app = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(nsApp, sharedAppSel);

    id nsAppearanceClass = reinterpret_cast<id>(objc_getClass("NSAppearance"));
    SEL appearanceNamedSel = sel_registerName("appearanceNamed:");

    id nsStringClass = reinterpret_cast<id>(objc_getClass("NSString"));
    SEL stringWithUTF8Sel = sel_registerName("stringWithUTF8String:");
    id lightName = reinterpret_cast<id (*)(id, SEL, const char*)>(objc_msgSend)(
        nsStringClass, stringWithUTF8Sel, "NSAppearanceNameAqua");

    id lightAppearance = reinterpret_cast<id (*)(id, SEL, id)>(objc_msgSend)(
        nsAppearanceClass, appearanceNamedSel, lightName);

    SEL setAppearanceSel = sel_registerName("setAppearance:");
    reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(app, setAppearanceSel, lightAppearance);
#elif defined(Q_OS_WIN)
    HWND hwnd = reinterpret_cast<HWND>(winId());
    // (frameless 라 네이티브 타이틀바는 없지만, 만약을 위해 남겨둠)
    BOOL useDarkMode = FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &useDarkMode, sizeof(useDarkMode)))) {
        DwmSetWindowAttribute(hwnd, 19, &useDarkMode, sizeof(useDarkMode));
    }
    // ★ Windows 11 네이티브 둥근 모서리 — Win32 DWM (HTML/CSS border-radius 도, Qt region 마스크도 아님).
    //   OS 가 창을 둥글게 클리핑 + 네이티브 테두리/그림자를 그림. 다른 Win11 앱과 동일한 방식.
    //   DWMWA_WINDOW_CORNER_PREFERENCE=33, DWMWCP_ROUND=2 (Win10 에선 무시 → 무해).
    //   frameless(WS_POPUP) 는 자동 라운딩이 안 되므로 명시 호출 필요.
    {
        DWORD cornerPref = 2; // DWMWCP_ROUND
        DwmSetWindowAttribute(hwnd, 33, &cornerPref, sizeof(cornerPref));
    }
#endif
}

// ★ 창 상태(최대화/복원) 변경 시 JS 의 최대화 버튼 아이콘 갱신 (Win+↑ 등 외부 변경도 반영)
void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::WindowStateChange && m_backend) {
        const bool maxed = isMaximized();
        m_backend->runJs(QStringLiteral("window.__winMaxChanged && window.__winMaxChanged(%1)")
                             .arg(maxed ? "true" : "false"));
    }
    QMainWindow::changeEvent(event);
}

#ifdef Q_OS_WIN
// frameless 창을 최대화하면 기본적으로 모니터 전체(작업표시줄 위까지)를 덮음 →
//   WM_GETMINMAXINFO 에서 최대화 크기를 작업영역(rcWork)으로 제한해 작업표시줄을 가리지 않게 함.
bool MainWindow::nativeEvent(const QByteArray &eventType, void *message, qintptr *result)
{
    MSG *msg = static_cast<MSG *>(message);
    if (msg && msg->message == WM_GETMINMAXINFO) {
        HMONITOR mon = MonitorFromWindow(msg->hwnd, MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi; mi.cbSize = sizeof(mi);
        if (GetMonitorInfo(mon, &mi)) {
            MINMAXINFO *mmi = reinterpret_cast<MINMAXINFO *>(msg->lParam);
            mmi->ptMaxPosition.x = mi.rcWork.left - mi.rcMonitor.left;
            mmi->ptMaxPosition.y = mi.rcWork.top  - mi.rcMonitor.top;
            mmi->ptMaxSize.x     = mi.rcWork.right  - mi.rcWork.left;
            mmi->ptMaxSize.y     = mi.rcWork.bottom - mi.rcWork.top;
            mmi->ptMinTrackSize.x = minimumWidth();
            mmi->ptMinTrackSize.y = minimumHeight();
            if (result) *result = 0;
            return true;
        }
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}
#endif
