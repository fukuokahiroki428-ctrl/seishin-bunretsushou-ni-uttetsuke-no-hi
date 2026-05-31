#include "MainWindow.h"
#include "MiyoBackend.h"

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
    setWindowTitle("Chernobyl");
    setMinimumSize(400, 700);
    resize(420, 850);

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

    auto *showAction = menu->addAction("Chernobyl 열기");
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
            "Chernobyl",
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
    // Windows 10/11: light titlebar
    HWND hwnd = reinterpret_cast<HWND>(winId());
    BOOL useDarkMode = FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &useDarkMode, sizeof(useDarkMode)))) {
        DwmSetWindowAttribute(hwnd, 19, &useDarkMode, sizeof(useDarkMode));
    }
#endif
}
