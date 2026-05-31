#include "MainWindow.h"
#include "PenBackend.h"

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
    setWindowTitle("팬을 잘 쓰고 싶다");  // 윈도우 타이틀 표시용 한국어 유지 (파일시스템 경로엔 미사용)
    setMinimumSize(400, 700);
    resize(420, 850);

    // Dark theme stylesheet
    setStyleSheet(R"(
        QMainWindow {
            background-color: #0A0A0A;
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
    m_webView->setContextMenuPolicy(Qt::NoContextMenu);  // 우클릭 메뉴 비활성화

    // WebChannel
    m_channel = new QWebChannel(this);
    m_backend = new PenBackend(this);
    m_channel->registerObject(QStringLiteral("backend"), m_backend);
    m_webView->page()->setWebChannel(m_channel);

    // ★ file:// → qrc:// 접근 허용 (CORS 우회)
    m_webView->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    m_webView->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);

    // ★ HTML 로딩 — 외부 파일 우선 (AUTORCC 의존성 깨졌을 때 안전)
    QString externalHtml;
    QStringList candidates;
#ifdef Q_OS_MACOS
    candidates << QCoreApplication::applicationDirPath() + "/../Resources/html/index.html";
#endif
    candidates << QCoreApplication::applicationDirPath() + "/../../../resources/html/index.html";
    candidates << QCoreApplication::applicationDirPath() + "/resources/html/index.html";
    for (const QString &p : candidates) {
        if (QFileInfo::exists(p)) { externalHtml = QFileInfo(p).absoluteFilePath(); break; }
    }
    if (!externalHtml.isEmpty()) {
        m_webView->setUrl(QUrl::fromLocalFile(externalHtml));
        qDebug() << "[HTML] external:" << externalHtml;
    } else {
        m_webView->setUrl(QUrl("qrc:/html/index.html"));
    }

    layout->addWidget(m_webView);

    // 펜은 인터랙티브 CDP 크롤러 전용 — 내장 QWebEngine 브라우저 창 안 씀.
    // showBrowser/browserView API는 호환성만 유지 (no-op).
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
}

QMenu *MainWindow::createDockMenu()
{
    auto *menu = new QMenu(this);

    auto *showAction = menu->addAction("팬을 잘 쓰고 싶다 열기");
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

    auto *stopAction = menu->addAction("크롤 중지");
    stopAction->setObjectName("dockStop");
    stopAction->setEnabled(false);
    connect(stopAction, &QAction::triggered, this, [this]() {
        if (m_backend) m_backend->crawlStop();
    });

    return menu;
}

void MainWindow::updateDockMenu()
{
    if (!m_dockMenu) return;

    auto *statusAction = m_dockMenu->findChild<QAction*>("dockStatus");
    auto *stopAction = m_dockMenu->findChild<QAction*>("dockStop");

    if (statusAction) statusAction->setText("상태: 대기");
    if (stopAction) stopAction->setEnabled(false);
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
    // 크롤 중이면 자동 종료 (사용자 직접 조작 모델이라 별도 confirm 안 함)
    if (m_backend) m_backend->crawlStop();

    // 브라우저 창 닫기
    if (m_browserWindow) m_browserWindow->close();

    event->accept();
}

void MainWindow::holdAwake()
{
#ifdef Q_OS_MACOS
    if (m_sleepAssertionHeld) return;

    // 1. IOPMAssertion — idle sleep 방지
    CFStringRef reason = CFSTR("ABIWA collection in progress");
    IOReturn result = IOPMAssertionCreateWithName(
        kIOPMAssertionTypePreventUserIdleSystemSleep,
        kIOPMAssertionLevelOn,
        reason,
        &m_sleepAssertion
    );
    if (result == kIOReturnSuccess) {
        m_sleepAssertionHeld = true;
    }

    // 2. caffeinate -dis — 덮개 닫아도 sleep 방지 (AC 전원 시)
    //    -d: display sleep 방지, -i: idle sleep 방지, -s: system sleep 방지 (AC)
    if (!m_caffeinate) {
        m_caffeinate = new QProcess(this);
        m_caffeinate->start("/usr/bin/caffeinate", {"-dis"});
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
#ifdef Q_OS_MACOS
    // Set dark appearance for macOS titlebar
    id nsApp = reinterpret_cast<id>(objc_getClass("NSApplication"));
    SEL sharedAppSel = sel_registerName("sharedApplication");
    id app = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(nsApp, sharedAppSel);

    // Get NSAppearance
    id nsAppearanceClass = reinterpret_cast<id>(objc_getClass("NSAppearance"));
    SEL appearanceNamedSel = sel_registerName("appearanceNamed:");

    // Create NSString for "NSAppearanceNameDarkAqua"
    id nsStringClass = reinterpret_cast<id>(objc_getClass("NSString"));
    SEL stringWithUTF8Sel = sel_registerName("stringWithUTF8String:");
    id darkName = reinterpret_cast<id (*)(id, SEL, const char*)>(objc_msgSend)(
        nsStringClass, stringWithUTF8Sel, "NSAppearanceNameDarkAqua");

    id darkAppearance = reinterpret_cast<id (*)(id, SEL, id)>(objc_msgSend)(
        nsAppearanceClass, appearanceNamedSel, darkName);

    SEL setAppearanceSel = sel_registerName("setAppearance:");
    reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(app, setAppearanceSel, darkAppearance);
#elif defined(Q_OS_WIN)
    // Windows 10/11: dark titlebar via DwmSetWindowAttribute
    HWND hwnd = reinterpret_cast<HWND>(winId());
    BOOL useDarkMode = TRUE;
    // DWMWA_USE_IMMERSIVE_DARK_MODE = 20 (Windows 11) or 19 (Windows 10 build 18985+)
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &useDarkMode, sizeof(useDarkMode)))) {
        DwmSetWindowAttribute(hwnd, 19, &useDarkMode, sizeof(useDarkMode));
    }
#endif
}
