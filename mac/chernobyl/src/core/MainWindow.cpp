#include "MainWindow.h"
#include "HanishikiBackend.h"
#include "PenBackend.h"

#include <QWebEngineSettings>
#include <QVBoxLayout>
#include <QMenuBar>
#include <QMouseEvent>
#include <QGuiApplication>
#include <QScreen>
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
    // 판 이름을 제목에 함께 보인다 — 숫자 버전은 기계용이라 사용자에게 안 보인다.
    //   (CODENAME 이 비어 있으면 이름만 — 빌드 설정이 없어도 깨지지 않게)
    {
        const QString codename = QStringLiteral(PREDORMITION_CODENAME);
        const QString appName = QStringLiteral(APP_NAME_DISPLAY);
        setWindowTitle(codename.isEmpty() ? appName : appName + QStringLiteral(" — ") + codename);
    }
    // ★ 기본 창 크기 — 폭 420px 은 사이드바(136px)를 빼면 본문이 284px 밖에 안 남아
    //   설정 폼·로그·표가 전부 눌린다. 데스크톱 앱 기준으로 넓혔다.
    setMinimumSize(820, 640);
    resize(1180, 820);

    // QMainWindow 배경 — HTML 페이지 배경(--bg)과 반드시 같아야 한다.
    //   이 색이 신호등 주변 타이틀바 띠로 그대로 보이기 때문에, 다르면 창 위쪽에
    //   다른 색 띠가 생긴다(예전엔 주석만 '흰색' 이고 값은 베이지 #EDE9E1 이라
    //   본문은 흰데 타이틀바만 베이지로 남아 있었다).
    setStyleSheet(R"(
        QMainWindow {
            background-color: #FFFFFF;
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
    m_backend = new HanishikiBackend(this);
    m_channel->registerObject(QStringLiteral("backend"), m_backend);
    // ★ PEN(팬을 잘 쓰고 싶다) 통합 — 사이트 미러 엔진을 2번째 WebChannel 객체로 등록.
    //   UI 의 미러/캡쳐 탭이 penBackend.crawl* 를 호출. PenBackend 는 같은 m_webView 를 공유
    //   (생성자에서 jsSignal/logSignal 을 자가 연결 → MainWindow::webView() 페이지에서 실행).
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

    // Apply titlebar styling after show. 0ms 는 너무 일러 Qt 가 styleMask 를 되돌릴 수
    // 있어(FullSizeContentView 풀림 → 흰 띠) 창이 완전히 realize 된 뒤 한 번 더 적용.
    QTimer::singleShot(0, this, &MainWindow::applyDarkTitlebar);
    QTimer::singleShot(500, this, &MainWindow::applyDarkTitlebar);

    // Dock 메뉴 생성 (macOS Dock 우클릭 시 표시)
    m_dockMenu = createDockMenu();

    // ★ 앱 시작 시 sleep 방지 어설션 자동 활성 — collection 안 돌고 있어도 항상 활성
    //   lid close 시 sleep 막는 데 최대 효과. (Apple 정책상 100% 보장은 외부 모니터 필요)
    QTimer::singleShot(500, this, &MainWindow::holdAwake);
}

QMenu *MainWindow::createDockMenu()
{
    auto *menu = new QMenu(this);

    auto *showAction = menu->addAction(QStringLiteral(APP_NAME_DISPLAY) + QStringLiteral(" 열기"));
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

    // ── 창(Window) 메뉴 ────────────────────────────────────────────────────
    //   ★ 이게 없어서 창을 키울 방법이 사실상 없었다.
    //     macOS 에서 초록 버튼은 기본이 '전체화면' 이고, '확대' 는 ⌥+클릭이나
    //     타이틀바 더블클릭인데 — 이 앱은 타이틀바를 투명하게 만들어(FullSizeContentView)
    //     그 자리를 웹뷰가 덮고 있어 더블클릭이 먹지 않는다.
    //     메뉴와 단축키로 확실한 경로를 만든다.
    auto *winMenu = menubar->addMenu("창");

    auto *zoomAction = new QAction("확대 / 원래대로", this);
    zoomAction->setShortcut(QKeySequence("Ctrl+Shift+Z"));   // mac 에선 ⌘⇧Z
    connect(zoomAction, &QAction::triggered, this, [this]() {
        if (isMaximized()) showNormal(); else showMaximized();
    });
    winMenu->addAction(zoomAction);

    auto *fullAction = new QAction("전체화면", this);
    fullAction->setShortcut(QKeySequence("Ctrl+Shift+F"));   // mac 에선 ⌘⇧F
    connect(fullAction, &QAction::triggered, this, [this]() {
        if (isFullScreen()) showNormal(); else showFullScreen();
    });
    winMenu->addAction(fullAction);

    winMenu->addSeparator();

    auto *fitAction = new QAction("화면에 맞추기", this);
    connect(fitAction, &QAction::triggered, this, [this]() {
        // 화면의 작업 영역(메뉴바·독 제외)에 꽉 채운다. 최대화가 막힌 환경에서도
        // 확실히 커지는 경로를 하나 더 둔다.
        if (QScreen *sc = screen() ? screen() : QGuiApplication::primaryScreen())
            setGeometry(sc->availableGeometry());
    });
    winMenu->addAction(fitAction);

    auto *resetAction = new QAction("기본 크기로", this);
    connect(resetAction, &QAction::triggered, this, [this]() {
        showNormal();
        resize(1180, 820);
    });
    winMenu->addAction(resetAction);

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
            QStringLiteral(APP_NAME_DISPLAY),
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

    // macOS: 이 앱이 띄운 Terminal.app 탭/창 닫기.
    //   ★ 제목으로 찾으므로 '지금 이름' 이 반드시 목록에 있어야 한다.
    //     앱 이름이 カメラ → Chernobyl → Predormition → ハンイシキ 로 바뀌는 동안
    //     여기가 옛 이름에 멈춰 있으면, 정작 지금 띄운 터미널을 못 닫는다.
    //     옛 이름도 함께 둔다 — 예전 판이 남긴 창까지 정리하려면 필요하다.
#ifdef Q_OS_MACOS
    QProcess::startDetached("/usr/bin/osascript", {"-e",
        "tell application \"Terminal\"\n"
        "  repeat with w in windows\n"
        "    repeat with t in tabs of w\n"
        "      if name of t contains \"miyo_\" or name of t contains \"ABIWA\""
        "         or name of t contains \"" APP_NAME_DISPLAY "\""
        "         or name of t contains \"" APP_NAME_ASCII "\""
        "         or name of t contains \"カメラ\" or name of t contains \"Predormition\" then\n"
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
    CFStringRef reason = CFSTR("Hanishiki active — preventing sleep");
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
        CFSTR("Hanishiki user activity"),
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
    // ★ 상단 띠(신호등이 있는 52px) 더블클릭 → 확대/원복.
    //   macOS 의 기본 동작이지만, 이 앱은 타이틀바를 투명하게 만들고
    //   (FullSizeContentView) 그 자리를 웹뷰가 덮고 있어 더블클릭이 창까지
    //   가지 못한다. 여기서 직접 받아 처리한다.
    if (event->type() == QEvent::MouseButtonDblClick) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            const QPoint inWindow = mapFromGlobal(me->globalPosition().toPoint());
            constexpr int kTitleStrip = 52;   // index.html 의 .sidebar-header / .toolbar 높이
            if (inWindow.y() >= 0 && inWindow.y() < kTitleStrip) {
                if (isMaximized()) showNormal(); else showMaximized();
                return true;
            }
        }
    }

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

    // ★ 통합(일체형) 타이틀바 — 흰 타이틀바 띠를 없애고 콘텐츠가
    //   창 최상단까지 올라오게 한다. 신호등(빨/노/초) 버튼은 콘텐츠 위에 떠 있고,
    //   그 자리는 .sidebar-header 의 padding-top:52px 가 이미 비워 두었다.
    //     titlebarAppearsTransparent=YES + titleVisibility=Hidden + FullSizeContentView
    {
        id nsView = reinterpret_cast<id>(winId());
        if (nsView) {
            id win = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(nsView, sel_registerName("window"));
            if (win) {
                reinterpret_cast<void (*)(id, SEL, BOOL)>(objc_msgSend)(
                    win, sel_registerName("setTitlebarAppearsTransparent:"), YES);
                reinterpret_cast<void (*)(id, SEL, long)>(objc_msgSend)(
                    win, sel_registerName("setTitleVisibility:"), 1 /* NSWindowTitleHidden */);
                unsigned long mask = reinterpret_cast<unsigned long (*)(id, SEL)>(objc_msgSend)(
                    win, sel_registerName("styleMask"));
                reinterpret_cast<void (*)(id, SEL, unsigned long)>(objc_msgSend)(
                    win, sel_registerName("setStyleMask:"), mask | (1UL << 15) /* FullSizeContentView */);
                // ★ FullSizeContentView 만으론 Qt 가 중앙 위젯을 '전체' 컨텐츠 영역으로 재배치하지
                //   않아 상단에 타이틀바 높이(~28px)만큼 빈 띠가 남는다(웹 innerHeight < 창높이).
                //   1px resize nudge 로 Qt 의 레이아웃을 강제 갱신 → 콘텐츠가 최상단까지 올라옴.
                const QSize sz = size();
                resize(sz.width(), sz.height() + 1);
                resize(sz);
            }
        }
    }
#elif defined(Q_OS_WIN)
    // Windows 10/11: light titlebar
    HWND hwnd = reinterpret_cast<HWND>(winId());
    BOOL useDarkMode = FALSE;
    if (FAILED(DwmSetWindowAttribute(hwnd, 20, &useDarkMode, sizeof(useDarkMode)))) {
        DwmSetWindowAttribute(hwnd, 19, &useDarkMode, sizeof(useDarkMode));
    }
#endif
}

// ─────────────────────────────────────────────────────────────────────
// setChromeTheme — 웹의 테마 토글(라이트/다크)에서 호출된다.
//   투명 타이틀바 + '네이티브 창 배경색 = 앱 상단색' 으로 맞춰, 콘텐츠 위 흰 띠가
//   앱 색과 동화되어 사라진 것처럼 보이게 한다(신호등 버튼은 그 위에 뜬다).
//   FullSizeContentView 가 Qt 에서 콘텐츠를 끝까지 안 올려도 이 방식은 확실히 동작.
// ─────────────────────────────────────────────────────────────────────
void MainWindow::setChromeTheme(bool dark)
{
    // ★ HTML 의 --bg 와 같은 값이어야 타이틀바 띠가 본문과 이어져 보인다.
    //   (라이트 #FFFFFF / 다크 #101114 — index.html 의 darkroom-graft 토큰과 일치)
    const QString bg = dark ? QStringLiteral("#101114") : QStringLiteral("#FFFFFF");
    setStyleSheet(QStringLiteral("QMainWindow { background-color: %1; }").arg(bg));
#ifdef Q_OS_MACOS
    id nsView = reinterpret_cast<id>(winId());
    if (!nsView) return;
    id win = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(nsView, sel_registerName("window"));
    if (!win) return;
    // 창 외관(신호등·타이틀바 material·네이티브 스크롤바) 을 테마에 맞춤
    id nm = reinterpret_cast<id (*)(id, SEL, const char*)>(objc_msgSend)(
        reinterpret_cast<id>(objc_getClass("NSString")), sel_registerName("stringWithUTF8String:"),
        dark ? "NSAppearanceNameDarkAqua" : "NSAppearanceNameAqua");
    id appr = reinterpret_cast<id (*)(id, SEL, id)>(objc_msgSend)(
        reinterpret_cast<id>(objc_getClass("NSAppearance")), sel_registerName("appearanceNamed:"), nm);
    reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(win, sel_registerName("setAppearance:"), appr);
    // 창 배경색 = 앱 상단색 → 투명 타이틀바 영역이 앱과 같은 색
    const double r = (dark ? 0x0F : 0xED) / 255.0;
    const double g = (dark ? 0x11 : 0xE9) / 255.0;
    const double b = (dark ? 0x15 : 0xE1) / 255.0;
    id color = reinterpret_cast<id (*)(id, SEL, double, double, double, double)>(objc_msgSend)(
        reinterpret_cast<id>(objc_getClass("NSColor")),
        sel_registerName("colorWithSRGBRed:green:blue:alpha:"), r, g, b, 1.0);
    reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(win, sel_registerName("setBackgroundColor:"), color);
#endif
}
