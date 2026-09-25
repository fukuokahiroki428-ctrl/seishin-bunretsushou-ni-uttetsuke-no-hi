#include "MainWindow.h"
#include <QCursor>   // 창 끌기 — 커서를 따라 옮긴다
#include <QAbstractNativeEventFilter>
#include <QRectF>
#include <QSet>
#include <QUrlQuery>
#include <QDateTime>
#include <atomic>
#include "Config.h"
#include <QResizeEvent>
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

// ★ 화면(UI) 전용 프로필 — 화면이 적어 둔 설정이 켤 때마다 남는다.
//   Qt 6 의 기본 프로필은 off-the-record(디스크에 아무것도 남기지 않는다)라, 화면이
//   localStorage 에 적어 둔 설정 — 테마 · 사이드바 접기 · 창 크기 방식 · 웹 검색 키 — 이
//   앱을 켤 때마다 사라졌다. 실측(2026-09-15): '통째로 크기 조절' 로 두고 껐다 켜니 저장값이
//   null. 데이터 폴더에 웹 저장소가 한 번도 생긴 적이 없었다.
//   이름 붙인 프로필은 AppData/QtWebEngine/ui 에 남는다. 캐시는 메모리에만 둔다 — 이 화면은
//   로컬 파일이라 디스크 캐시가 필요 없고, 예전 내장 브라우저처럼 캐시가 불어나지 않게.
//   본 창과 기능 창이 같이 쓴다(같은 설정을 본다). 부모는 qApp — 창(페이지)보다 늦게 사라진다.
static QWebEngineProfile *uiProfile()
{
    static QWebEngineProfile *p = nullptr;
    if (!p) {
        p = new QWebEngineProfile(QStringLiteral("ui"), qApp);
        p->setHttpCacheType(QWebEngineProfile::MemoryHttpCache);
        p->setHttpCacheMaximumSize(16 * 1024 * 1024);
    }
    return p;
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    installWindowDragFilter();   // 창 끌기 — 누르는 순간 창 서버에 맡긴다(아래 설명)
    // 판 이름을 제목에 함께 보인다 — 숫자 버전은 기계용이라 사용자에게 안 보인다.
    //   (CODENAME 이 비어 있으면 이름만 — 빌드 설정이 없어도 깨지지 않게)
    {
        const QString codename = QStringLiteral(PREDORMITION_CODENAME);
        const QString appName = QStringLiteral(APP_NAME_DISPLAY);
        setWindowTitle(codename.isEmpty() ? appName : appName + QStringLiteral(" — ") + codename);
    }
    // ★ 기본 창 크기 — 폭 420px 은 사이드바(136px)를 빼면 본문이 284px 밖에 안 남아
    //   설정 폼·로그·표가 전부 눌린다. 데스크톱 앱 기준으로 넓혔다.
    // ★ 예전엔 820x640 이 최소였다. 그래서 화면에 이미 들어 있던 좁은 폭 규칙
    //   (680px·480px 기준점)에 닿을 방법이 없었다 — 쓰지도 못할 CSS 였다.
    //   창을 핸드폰 폭까지 줄일 수 있게 낮춘다. 그 폭에서 어떻게 보이는지는
    //   실제로 390x844 로 재서 사이드바·툴바·입력칸을 고쳤다.
    setMinimumSize(360, 480);
    resize(1180, 820);   // 저장된 것이 없을 때의 기본. 아래에서 있으면 덮어쓴다.

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

#ifdef Q_OS_MACOS
    // ★ 화면을 타이틀바 밑까지 올린다 — Qt 6.9+ 의 공식 방법.
    //   예전엔 applyDarkTitlebar 에서 NSWindow 에 FullSizeContentView 를 직접 켜고
    //   1px 크기 흔들기로 Qt 레이아웃을 깨우려 했다. 실측(Qt 6.11): 창 820px 에
    //   웹뷰 788px — 웹뷰가 타이틀바 아래에 앉아, 신호등만 있는 흰 띠가 남았다.
    //   사용자 판(9월 7일)에서는 제목 글자와 구분선까지 그대로 보였다.
    //   · ExpandedClientAreaHint  — 창 전체를 그리는 영역으로
    //   · NoTitleBarBackgroundHint — 타이틀바 배경을 그리지 않는다(신호등만 뜬다)
    //   · 안전 영역을 비켜 앉지 않게 — 기본값은 '비켜 앉기' 라, 그대로 두면 레이아웃이
    //     타이틀바 높이만큼 내려앉아 다시 흰 띠가 된다. 신호등 자리는 화면 쪽
    //     (.sidebar-header 52px, 접힘 시 툴바 왼쪽 여백)이 이미 비워 두었다.
    setWindowFlag(Qt::ExpandedClientAreaHint, true);
    setWindowFlag(Qt::NoTitleBarBackgroundHint, true);
    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
    central->setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
#endif

    // WebEngineView (main UI)
    m_webView = new QWebEngineView(this);
    // ★ JS 콘솔 메시지 stderr 로 redirect — debug 용
    m_webView->setPage(new DebugWebEnginePage(uiProfile(), m_webView));   // 설정이 남는 프로필(위 설명)
    // 우클릭 메뉴 비활성화 (Reload/Inspect/View Source 같은 컨텍스트 메뉴 안 뜸)
    m_webView->setContextMenuPolicy(Qt::NoContextMenu);

    // WebChannel
    m_channel = new QWebChannel(this);
    m_backend = new HanishikiBackend(this);
    m_channel->registerObject(QStringLiteral("backend"), m_backend);
    // ★ PEN(사이트 미러 엔진, PenBackend)은 걷어냈다.
    //   여기서 채널에 등록만 해 두고 어느 쪽으로도 이어지지 않았다(전수 대조):
    //     · index.html 의 penBackend 참조   0곳
    //     · PenBackend 의 runJs 호출        18곳
    //     · 그 18곳이 부르는 JS 콜백 9종 중 화면에 정의된 것  0개
    //   그런데 생성자가 Config·WebDavUploader 와 로그 타이머(계속 돈다)를 만든다.
    //   아무도 안 쓰는 것을 매 실행마다 올리고 있었다.
    //   실제 크롤/미러는 m_backend 로 돈다 — SiteCrawler 가 부르는
    //   showCrawlLoginConfirm 은 index.html 에 제대로 있다.
    //   클래스(PenBackend.cpp/.h)는 남겨 둔다. 나중에 미러 UI 를 붙일 생각이면
    //   여기서 다시 등록하고 위 콜백 9종을 화면에 정의하면 된다.
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

    // ★ 화면이 다 뜬 뒤에야 사이드바 항목을 읽을 수 있다. 그때 '기능' 메뉴를 채운다.
    //   페이지가 다시 로드돼도(자가수리·새로고침) 다시 채워지도록 매번 건다.
    //   ★ 이 연결은 반드시 if/else '바깥' 이어야 한다. 처음엔 else 안에 넣는 바람에,
    //     번들 HTML(Resources/html/index.html)을 쓰는 실제 경로에서는 한 번도 걸리지
    //     않았다. 그래서 메뉴가 '목록을 읽는 중…' 에서 멈춰 있었다.
    // ★ 다시 읽는 동안(새로고침·자가수리)에도 JS 상태가 날아가므로 같이 잠근다.
    connect(m_webView, &QWebEngineView::loadStarted, this, [this]() {
        m_uiReady = false;
    });
    connect(m_webView, &QWebEngineView::loadFinished, this, [this](bool ok) {
        if (ok) {
            populatePlatformMenu(); applyZoom();
        } else {
            // 실패해도 큐를 영원히 붙들지 않는다. 붙들면 메모리만 늘고 원인은 안 보인다.
            qWarning() << "[UI] 페이지 로드 실패 — 담아 둔 JS" << m_pendingJs.size()
                       << "개를 그대로 흘린다";
        }
        // ★ 페이지가 뜨기 전에 들어온 JS 를 여기서 흘린다 — MainWindow.h 의 설명 참고.
        m_uiReady = true;
        flushPendingJs();
    });

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
    // ★ 저장해 둔 창 크기·위치를 되살린다.
    //   예전엔 켤 때마다 1180x820 으로 열려서, 좁게 줄여 놔도 다음 실행이면
    //   되돌아갔다 — 좁은 폭 배치를 쓸 수가 없었다.
    //   화면 구성이 바뀌어 창이 화면 밖으로 나가는 일이 없도록, 복원한 창이
    //   지금 화면들과 겹치지 않으면 기본 크기로 되돌린다.
    if (m_backend && m_backend->config()) {
        // ★ 설정은 원래 화면이 다 뜬 뒤에 JS 가 backend.loadConfig() 를 불러야 읽혔다.
        //   창을 만드는 이 시점엔 아직 비어 있어서, 복원할 값이 없어 늘 기본 크기로
        //   열렸다(저장은 되는데 복원만 안 되던 원인). 여기서 먼저 읽는다.
        //   뒤에 JS 가 다시 읽어도 같은 파일이라 문제되지 않는다.
        m_backend->config()->load();
        const QByteArray geo = QByteArray::fromBase64(
            m_backend->config()->windowGeometry().toLatin1());
        if (!geo.isEmpty() && restoreGeometry(geo)) {
            bool onScreen = false;
            const QRect mine = frameGeometry();
            for (QScreen *sc : QGuiApplication::screens())
                if (sc->availableGeometry().intersects(mine)) { onScreen = true; break; }
            if (!onScreen) {
                resize(1180, 820);
                move(QGuiApplication::primaryScreen()->availableGeometry().center()
                     - rect().center());
            }
        }
    }

    m_dockMenu = createDockMenu();

    // ★ 앱 시작 시 sleep 방지 어설션 자동 활성 — collection 안 돌고 있어도 항상 활성
    //   lid close 시 sleep 막는 데 최대 효과. (Apple 정책상 100% 보장은 외부 모니터 필요)
    QTimer::singleShot(500, this, &MainWindow::holdAwake);
}


// 상단 막대의 '기능' 메뉴를 화면의 실제 항목으로 채운다.
//   ★ 목록을 코드에 적지 않는다. 사이드바가 진짜 목록이므로 거기서 읽는다.
//     그래야 탭이 늘거나 이름이 바뀌어도 메뉴만 옛것으로 남지 않는다.
void MainWindow::populatePlatformMenu()
{
    if (!m_platformMenu || !m_webView) return;
    m_webView->page()->runJavaScript(
        "JSON.stringify(Array.from(document.querySelectorAll('.nav-item'))"
        ".map(function(e){var m=(e.getAttribute('onclick')||'').match(/switchTab\\('([a-z]+)'\\)/);"
        "return m ? {id:m[1], label:(e.textContent||'').trim().replace(/\\s+/g,' '),"
        " windowOnly: e.getAttribute('data-window-only')==='1'} : null})"
        ".filter(Boolean))",
        [this](const QVariant &v) {
            const QJsonArray arr = QJsonDocument::fromJson(v.toString().toUtf8()).array();
            if (arr.isEmpty()) {
                // 조용히 돌아가면 메뉴가 '목록을 읽는 중…' 에 멈춘 채로 남는다.
                qWarning() << "[기능메뉴] 사이드바에서 항목을 읽지 못했다 — 메뉴가 비어 있다";
                return;
            }
            m_platformMenu->clear();
            for (const QJsonValue &it : arr) {
                const QJsonObject o = it.toObject();
                const QString id = o.value("id").toString();
                QString label = o.value("label").toString();
                if (id.isEmpty()) continue;
                // 라벨 앞의 짧은 아이콘 글자(T, B, Tu …)를 떼어 낸다.
                const int sp = label.indexOf(' ');
                if (sp > 0 && sp <= 3) label = label.mid(sp + 1).trimmed();
                if (label.isEmpty()) label = id;
                // ★ 별도 창으로만 여는 기능(사이드바에서 감춘 것들)은 창을 띄운다.
                //   판정을 코드에 박지 않고 화면의 표시(data-window-only)를 그대로 쓴다 —
                //   나중에 대상이 늘어도 HTML 한쪽만 고치면 된다.
                const bool windowOnly = o.value("windowOnly").toBool();
                QAction *a = m_platformMenu->addAction(
                    windowOnly ? (label + "  (별도 창)") : label);
                connect(a, &QAction::triggered, this, [this, id, label, windowOnly]() {
                    if (windowOnly) { openFeatureWindow(id, label); return; }
                    show(); raise(); activateWindow();
                    m_webView->page()->runJavaScript(
                        QString("if(window.switchTab) switchTab('%1')").arg(id));
                });
            }
            // ★ 일괄 다운로드 — 모달은 완성돼 있는데 여는 곳이 어디에도 없어
            //   기능 전체가 화면에서 닿을 수 없었다(window.openBatchModal 정의만 존재).
            //   사이드바를 건드리지 않고 메뉴막대에 입구를 낸다.
            m_platformMenu->addSeparator();
            QAction *batch = m_platformMenu->addAction(QStringLiteral("일괄 다운로드…"));
            connect(batch, &QAction::triggered, this, [this]() {
                show(); raise(); activateWindow();
                m_webView->page()->runJavaScript(
                    QStringLiteral("if(window.openBatchModal) openBatchModal();"
                                   "else appendLog('이 판에는 일괄 다운로드가 없습니다','error','settings');"));
            });
            qInfo() << "[기능메뉴] 항목" << arr.size() << "개 채움";
        });
}


// 기능 하나를 별도 창으로 연다.
//   ★ 왜 같은 index.html 을 다시 띄우나.
//     탭 본문·입력칸·저장 로직이 전부 그 문서에 얹혀 있다. 따로 만들면 두 벌을
//     유지해야 하고, 이 저장소에서 그런 이중 관리는 늘 한쪽만 갱신되어 어긋났다.
//     같은 문서를 띄우고 #window=<탭> 으로 '이 창은 이 기능만' 이라고 알려 준다.
//   ★ 백엔드는 같은 QWebChannel 을 공유한다 — JS→C++ 호출은 이 창에서도 된다.
//     그런데 C++→JS 는 아니었다. jsSignal 은 채널로 나가는 게 아니라 백엔드가
//     자기 자신에게 연결해 두고 페이지 하나를 골라 실행하는 구조라, 오래도록
//     본 창 페이지만 받고 있었다. 이 창은 명령은 보내는데 로그·진행·결과는
//     한 줄도 못 받는 반쪽이었다. 지금은 MainWindow::allWebViews() 로 모든 창에
//     보낸다 — 이 주석이 원래 말하려던 상태가 이제야 사실이 되었다.
void MainWindow::openFeatureWindow(const QString &tabId, const QString &title)
{
    if (QWidget *w = m_featureWindows.value(tabId).data()) {
        w->show(); w->raise(); w->activateWindow();
        return;
    }

    auto *win = new QWidget(nullptr);
    win->setAttribute(Qt::WA_DeleteOnClose);
    win->setWindowTitle(QStringLiteral(APP_NAME_DISPLAY) + " — " + title);
    win->resize(900, 700);
    win->setMinimumSize(360, 420);

    auto *view = new QWebEngineView(win);
    view->setPage(new DebugWebEnginePage(uiProfile(), view));   // 본 창과 같은 프로필 — 같은 설정을 본다
    view->setContextMenuPolicy(Qt::NoContextMenu);
    view->page()->setWebChannel(m_channel);        // 같은 채널 — 백엔드 공유
    view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessRemoteUrls, true);
    view->settings()->setAttribute(QWebEngineSettings::LocalContentCanAccessFileUrls, true);

    auto *lay = new QVBoxLayout(win);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addWidget(view);

    QUrl url = m_webView ? m_webView->url() : QUrl("qrc:/html/index.html");
    url.setFragment("window=" + tabId);
    view->setUrl(url);

    // 별도 창도 자기 크기에 맞춰 배율을 맞춘다 — 본 창만 맞추면 이 창은 늘 100%%다.
    view->setZoomFactor(zoomForWidth(win->width()));
    win->installEventFilter(this);
    // 웹뷰에도 건다 — 마우스는 웹뷰가 나중에 붙이는 그리기 위젯으로 가는데,
    // 그 위젯은 ChildAdded 로만 잡힌다. 창에만 걸면 이 창의 상단 띠 끌기가 안 먹는다.
    view->installEventFilter(this);

    connect(win, &QObject::destroyed, this, [this, tabId]() { m_featureWindows.remove(tabId); });
    m_featureWindows.insert(tabId, win);
    win->show(); win->raise(); win->activateWindow();
}

QMenu *MainWindow::createDockMenu()
{
    // ★ 이 함수는 두 번 불린다 — 생성자에서 한 번(m_dockMenu 에 담으려고),
    //   main.cpp 에서 한 번(NSApp setDockMenu: 에 넘기려고).
    //   예전엔 부를 때마다 새로 만들었다. 그래서 Dock 에 실제로 붙은 메뉴와
    //   updateDockMenu() 가 고치는 메뉴가 서로 달랐고, Dock 우클릭 메뉴의
    //   상태·통계는 처음 만든 그대로 "상태: 대기" 에서 영영 멈춰 있었다.
    //   한 번 만든 것을 계속 돌려준다.
    if (m_dockMenu) return m_dockMenu;

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
            // ★ tumblr 가 빠져 있었다 — 内閣会는 tumblr 도 돌리는데 이 경로로는
            //   영영 안 멈췄다. 목록에 없는 플랫폼은 중지 버튼이 없는 것과 같다.
            for (const auto &p : {"twitter", "bluesky", "tumblr", "youtube",
                                  "discord", "instagram", "crawl"}) {
                m_backend->stopCollection(p);
            }
        }
    });

    m_dockMenu = menu;
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

// ★ 메인 UI 페이지에 JS 를 넣는 단 하나의 통로 — MainWindow.h 의 설명 참고.
void MainWindow::runJsOnUi(const QString &js)
{
    if (!m_webView || js.isEmpty()) return;
    if (m_uiReady) {
        m_webView->page()->runJavaScript(js);
        return;
    }
    // 아직 안 읽혔다 — 담아 둔다.
    //   한도를 둔다. 페이지가 끝내 안 뜨는 경우 무한히 쌓이면 그게 다음 고장이다.
    //   넘치면 오래된 것부터 버린다(로그 배칭이 이미 쓰는 규칙과 같게).
    constexpr int kMaxPending = 500;
    m_pendingJs.append(js);
    if (m_pendingJs.size() > kMaxPending) {
        m_pendingJs.removeFirst();
        ++m_droppedJs;
    }
}

void MainWindow::flushPendingJs()
{
    if (m_droppedJs > 0) {
        // 버린 것을 말하지 않으면 "다 전달됐다" 로 읽힌다.
        qWarning() << "[UI] 페이지가 뜨기 전 JS" << m_droppedJs << "개를 버렸다(한도 초과)";
        m_droppedJs = 0;
    }
    if (m_pendingJs.isEmpty()) return;
    const QStringList queued = m_pendingJs;
    m_pendingJs.clear();
    // 한 덩어리로 합치지 않는다 — 중간 하나가 던지면 뒤가 통째로 죽는다.
    for (const QString &js : queued)
        m_webView->page()->runJavaScript(js);
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
    // ── 기능 메뉴 ─────────────────────────────────────────────────────────
    //   ★ 왜 필요한가. 각 플랫폼으로 들어가는 길이 앱 안 사이드바 하나뿐이었다.
    //     창을 좁히면 그 목록이 자리를 많이 먹고, macOS 상단 막대에는 File·창밖에
    //     없어서 '어디로 가야 하는지' 가 앱 안에만 있었다.
    //     상단 막대에서도 바로 들어갈 수 있게 한다.
    //
    //   ★ 이름을 코드에 박지 않는다. 탭이 늘거나 이름이 바뀌면 메뉴만 옛것으로
    //     남는다 — 이 저장소에서 그런 어긋남을 여러 번 겪었다.
    //     화면이 다 뜬 뒤 사이드바에서 실제 항목을 읽어 메뉴를 만든다.
    {
        m_platformMenu = menubar->addMenu("기능");
        auto *loading = m_platformMenu->addAction("목록을 읽는 중…");
        loading->setEnabled(false);
    }

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


// 화면 배율 — 창 크기를 바꿀 때 어떻게 할지 설정에서 고른다(2026-09-15·18).
//   둘 다(기본) — 창 폭에 비례해 커지고 작아지다가(기준 1180px), 80% 에 닿으면 거기서 멈추고
//     그보다 좁으면 배치가 바뀐다(칸이 세로로 쌓이고 560px 아래에선 사이드바가 접힌다).
//     사용자: "A·B 둘 다" → 고르는 것이 아니라 두 가지가 같이 일어나게.
//   배치 바꾸기 — 배율 100%. 좁아지면 배치만 바뀐다(index.html 의 @media).
//   통째로 크기 조절 — 0.5~1.6배. 배치는 그대로 두고 화면 전체가 커지고 작아진다.
//   고른 값은 화면(localStorage 'uiScaleMode')이 기억해, 켤 때마다 setUiScaleMode 로 알려 준다.
static MainWindow::ScaleMode g_scaleMode = MainWindow::ScaleBoth;

qreal MainWindow::zoomForWidth(int w)
{
    if (g_scaleMode == ScaleLayout) return 1.0;
    const qreal floor = (g_scaleMode == ScaleBoth) ? 0.80 : 0.50;
    qreal z = w / 1180.0;
    if (z < floor) z = floor;
    if (z > 1.60) z = 1.60;
    return z;
}

void MainWindow::setScaleMode(ScaleMode mode)
{
    if (g_scaleMode == mode) return;
    g_scaleMode = mode;
    applyZoom();
    for (auto it = m_featureWindows.begin(); it != m_featureWindows.end(); ++it) {   // 기능 창도 같이
        if (QWidget *w = it->data())
            if (auto *v = w->findChild<QWebEngineView *>()) v->setZoomFactor(zoomForWidth(w->width()));
    }
}

void MainWindow::applyZoom()
{
    if (!m_webView) return;
    const qreal z = zoomForWidth(width());
    if (qAbs(m_webView->zoomFactor() - z) < 0.01) return;   // 미세한 변화로 다시 그리지 않는다
    m_webView->setZoomFactor(z);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    // ★ 창을 끄는 동안 크기 이벤트가 초당 수십 번 온다. 그때마다 배율을 바꾸면 화면
    //   전체를 매번 다시 배치해 끌기가 버벅인다(렉). 50ms 에 한 번만 맞춘다 —
    //   끄는 동안에도 따라가고, 손을 떼면 50ms 안에 마지막 크기로 맞는다.
    if (!m_zoomTimer) {
        m_zoomTimer = new QTimer(this);
        m_zoomTimer->setSingleShot(true);
        m_zoomTimer->setInterval(50);
        connect(m_zoomTimer, &QTimer::timeout, this, &MainWindow::applyZoom);
    }
    if (!m_zoomTimer->isActive()) m_zoomTimer->start();
}

// ★ 창 끌기 — 커서를 직접 따라 옮긴다.
//   예전엔 startSystemMove()(맥: performWindowDragWithEvent)에 맡겼는데 창이 꿈쩍하지
//   않았다(사용자 확인 2026-09-15, 두 판 연속). 화면 쪽은 멀쩡했다 — 격리 앱에 마우스를
//   흘려 보니 툴바·제목·사이드바 머리에서 winStartMove 가 한 번씩 불렸다. 막힌 곳은
//   네이티브 쪽이다: Qt 는 마우스 이벤트를 모아 두었다 나중에 처리해, 그 순간
//   NSApp.currentEvent 가 마우스 이벤트가 아니면 startSystemMove 는 아무것도 안 한다.
//   → 이벤트에 기대지 않는다. 걸린 순간부터 왼쪽 버튼을 뗄 때까지 8ms 마다 커서 위치를
//     읽어 커서 아래 창(본 창이든 기능 창이든)을 그만큼 옮긴다. 버튼 상태는 OS 에
//     직접 묻는다 — Qt 가 기억하는 상태는 이벤트를 처리한 뒤에야 바뀌어 늦다.
static bool leftMouseDown()
{
#ifdef Q_OS_MACOS
    const unsigned long b = reinterpret_cast<unsigned long (*)(id, SEL)>(objc_msgSend)(
        reinterpret_cast<id>(objc_getClass("NSEvent")), sel_registerName("pressedMouseButtons"));
    return b & 1;
#else
    return QGuiApplication::mouseButtons() & Qt::LeftButton;
#endif
}

#ifdef Q_OS_MACOS
// ★ 창 끌기를 macOS 창 서버에 맡긴다 — 클로드 창(Electron)과 같은 길.
//   사용자(2026-09-25): "클로드 창은 렉 없이 부드러운데 한이시키는 딜레이·렉".
//   아래 8ms 타이머는 앱이 직접 창을 옮기므로 화면 갱신과 박자가 맞지 않고, 수집 중엔
//   메인 스레드가 바빠 틱이 밀렸다.
//
//   첫 시도(같은 날)는 실패했다: 누름이 끝난 뒤 '가짜 누름' 을 만들어 performWindowDrag 에
//   건넸더니 창 서버가 받아 주지 않았다(기록: "창 서버 끌기가 먹지 않아" 두 번). 창 서버는
//   '지금 막 처리 중인 진짜 누름' 에만 끌기를 허락한다.
//
//   그래서 Electron 처럼 한다. 화면(JS)은 마우스가 '끌 수 있는 곳' 위에 있는지를 미리
//   알려 두고(setDragHover), 누르는 그 순간 Qt 가 NSEvent 를 나눠 주기 전에 여기서 잡아
//   진짜 누름을 그대로 창 서버에 넘긴다. 다리(WebChannel) 왕복도, 3px 문턱도 없다.
//   이 누름은 화면으로 보내지 않는다(끄는 자리는 원래 누를 것이 없는 빈 바탕이다).
struct NsPt { double x, y; };            // CGPoint 와 같은 모양
struct NsRect { double x, y, w, h; };    // CGRect 와 같은 모양

static std::atomic<bool> g_dragHover{false};

template <typename R, typename... A>
static inline R msg(id obj, const char *sel, A... a)
{
    return reinterpret_cast<R (*)(id, SEL, A...)>(objc_msgSend)(obj, sel_registerName(sel), a...);
}

// ★ 끌 자리를 좌표로도 받아 둔다(Electron 의 drag region 과 같은 생각).
//   마우스 움직임 신호(g_dragHover)만 믿으면, 창이 뒤에 있다 앞으로 오는 첫 누름이나
//   재빨리 잡는 경우에 신호가 늦어 옛 타이머로 빠졌다(사용자: "부드럽네요가 아니죠" —
//   같은 창을 '어디든 끌림' 시험 상태로 두자 "우왕 부드럽다". 길은 맞고 타이밍이 문제였다).
//   화면이 사이드바·위쪽 띠의 자리와 그 안의 단추 자리를 CSS 픽셀로 넘긴다. 누르는 순간
//   커서가 끌 자리 안이고 단추 자리 밖이면 끈다 — 움직임과 상관없다.
struct DragRegions { QList<QRectF> zones, holes; };
static QHash<QString, DragRegions> g_dragRegions;   // 창 열쇠("main" 또는 ?window= 값) → 자리

static bool pointInDragZone()
{
    const QPoint gp = QCursor::pos();
    QWebEngineView *v = nullptr;
    for (QWidget *p = QApplication::widgetAt(gp); p; p = p->parentWidget())
        if ((v = qobject_cast<QWebEngineView *>(p))) break;
    if (!v) return false;
    // 기능 창은 #window=<탭>(주소 조각)으로 열린다 — 조각을 먼저, 없으면 ?window= 를 본다
    QString key = QUrlQuery(v->url().fragment()).queryItemValue(QStringLiteral("window"));
    if (key.isEmpty()) key = QUrlQuery(v->url()).queryItemValue(QStringLiteral("window"));
    if (key.isEmpty()) key = QStringLiteral("main");
    const auto it = g_dragRegions.constFind(key);
    if (it == g_dragRegions.constEnd()) return false;
    const double z = v->zoomFactor() > 0 ? v->zoomFactor() : 1.0;
    const QPointF local = v->mapFromGlobal(gp);
    const QPointF css(local.x() / z, local.y() / z);
    for (const QRectF &h : it->holes) if (h.contains(css)) return false;
    for (const QRectF &r : it->zones) if (r.contains(css)) return true;
    return false;
}

class WindowDragFilter : public QAbstractNativeEventFilter
{
public:
    bool nativeEventFilter(const QByteArray &type, void *message, qintptr *) override
    {
        if (type != QByteArrayLiteral("mac_generic_NSEvent")) return false;
        id ev = static_cast<id>(message);
        if (!ev) return false;
        if (msg<unsigned long>(ev, "type") != 1ul) return false;          // NSEventTypeLeftMouseDown
        // 좌표로 본 끌 자리(사이드바·위쪽 띠) — 움직임 신호가 늦어도 된다.
        // 그 밖(본문의 빈 바탕)은 화면이 보낸 움직임 신호로 판단한다.
        if (!pointInDragZone() && !g_dragHover.load(std::memory_order_relaxed)) return false;
        if (msg<long>(ev, "clickCount") != 1) return false;               // 두 번 누름은 건드리지 않는다
        id win = msg<id>(ev, "window");
        if (!win) return false;
        // 전체 화면 창은 옮기지 않는다(OS 와 같다)
        if (msg<unsigned long>(win, "styleMask") & (1ul << 14)) return false;
        const NsPt p = msg<NsPt>(ev, "locationInWindow");
        const NsRect f = msg<NsRect>(win, "frame");
        // 창 가장자리는 크기 조절 자리다 — 끌기로 가로채지 않는다
        constexpr double edge = 6.0;
        if (p.x < edge || p.y < edge || p.x > f.w - edge || p.y > f.h - edge) return false;
        // 신호등(닫기·최소화·확대) 같은 네이티브 단추 위면 건드리지 않는다.
        //   화면의 '끌 수 있는 곳' 신호는 그 단추 밑의 웹 바탕에서 온 것이라 참일 수 있다.
        id content = msg<id>(win, "contentView");
        id root = content ? msg<id>(content, "superview") : nullptr;
        id hit = msg<id>(root ? root : content, "hitTest:", p);
        if (hit && msg<bool>(hit, "isKindOfClass:", reinterpret_cast<id>(objc_getClass("NSButton"))))
            return false;
        msg<void>(win, "performWindowDragWithEvent:", ev);
        MainWindow::noteNativeDrag();
        return true;                                                       // 화면으로는 보내지 않는다
    }
};
#endif

void MainWindow::setDragHover(bool on)
{
#ifdef Q_OS_MACOS
    g_dragHover.store(on, std::memory_order_relaxed);
#else
    Q_UNUSED(on);
#endif
}

void MainWindow::setDragRegions(const QString &key, const QString &json)
{
    DragRegions r;
    const QJsonObject o = QJsonDocument::fromJson(json.toUtf8()).object();
    auto read = [](const QJsonArray &a, QList<QRectF> &out) {
        for (const auto &v : a) {
            const QJsonArray q = v.toArray();
            if (q.size() == 4) out << QRectF(q[0].toDouble(), q[1].toDouble(), q[2].toDouble(), q[3].toDouble());
        }
    };
    read(o.value("z").toArray(), r.zones);
    read(o.value("h").toArray(), r.holes);
    const QString k = key.isEmpty() ? QStringLiteral("main") : key;
    static QSet<QString> logged;
    if (!logged.contains(k)) {
        logged.insert(k);
        qInfo().noquote() << QString("[창 끌기] 끌 자리 %1곳 · 단추 자리 %2곳 받음 (%3)")
                                 .arg(r.zones.size()).arg(r.holes.size()).arg(k);
    }
    g_dragRegions.insert(k, r);
}

qint64 MainWindow::s_nativeDragAt = 0;
void MainWindow::noteNativeDrag()
{
    s_nativeDragAt = QDateTime::currentMSecsSinceEpoch();
    static int logged = 0;
    if (logged < 3) { ++logged; qInfo() << "[창 끌기] 창 서버에 맡김"; }
}

void MainWindow::installWindowDragFilter()
{
#ifdef Q_OS_MACOS
    static WindowDragFilter *f = nullptr;
    if (!f) { f = new WindowDragFilter; qApp->installNativeEventFilter(f); }
#endif
}

void MainWindow::armWindowMove()
{
    // 창 서버가 방금 이 누름을 가져갔다면 옛 방식이 끼어들 까닭이 없다(둘이 싸운다).
    if (QDateTime::currentMSecsSinceEpoch() - s_nativeDragAt < 1500) return;
    if (!leftMouseDown()) return;                  // 이미 뗐다 — 늦게 온 요청은 버린다
    QWidget *w = QApplication::widgetAt(QCursor::pos());
    w = w ? w->window() : QApplication::activeWindow();
    if (!w || w->isFullScreen()) return;          // 전체 화면 창은 옮기지 않는다(OS 와 같다)
    // 여기까지 왔다면 화면이 '끌 수 있는 곳' 신호를 못 보낸 드문 경우다 — 옛 방식으로 옮긴다.
    m_dragWin = w;
    m_dragOffset = QCursor::pos() - w->pos();
    startDragTimer();
}

void MainWindow::startDragTimer()
{
    if (!m_dragTimer) {
        m_dragTimer = new QTimer(this);
        m_dragTimer->setInterval(8);
        connect(m_dragTimer, &QTimer::timeout, this, &MainWindow::dragTick);
    }
    m_dragTimer->start();
}

void MainWindow::dragTick()
{
    if (!m_dragWin || !leftMouseDown()) { m_dragTimer->stop(); m_dragWin = nullptr; return; }
    const QPoint to = QCursor::pos() - m_dragOffset;
    if (to != m_dragWin->pos()) m_dragWin->move(to);
}

QList<QWebEngineView *> MainWindow::allWebViews() const
{
    QList<QWebEngineView *> views;
    if (m_webView) views << m_webView;
    for (const auto &ref : m_featureWindows) {
        QWidget *w = ref.data();
        if (!w) continue;                       // 이미 닫힌 창
        if (auto *v = w->findChild<QWebEngineView *>()) views << v;
    }
    return views;
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

    // ★ 창 크기·위치를 남긴다. 다음에 켤 때 그대로 열린다.
    if (m_backend && m_backend->config()) {
        m_backend->config()->setWindowGeometry(
            QString::fromLatin1(saveGeometry().toBase64()));
        m_backend->config()->save();
    }

    // 브라우저 창 닫기
    if (m_browserWindow) m_browserWindow->close();

    // ★ 메뉴막대로 띄운 기능 창도 같이 닫는다.
    //   main.cpp 에서 setQuitOnLastWindowClosed(false) 라 본 창을 닫아도 앱은 살아 있다.
    //   그래서 이 창들이 화면에 그대로 남는데, 본 창이 사라진 뒤엔 아무것도 못 하는
    //   빈 창이 된다. 목록을 복사해서 도는 이유는 close() 가 destroyed 를 타고
    //   m_featureWindows 에서 자기를 지우기 때문이다(도는 중에 지우면 반복자가 깨진다).
    const auto featureWins = m_featureWindows.values();
    for (const auto &ref : featureWins) {
        if (QWidget *w = ref.data()) w->close();
    }

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
    // 별도 창(기능 창)도 자기 폭에 맞춰 배율을 따라간다.
    //   본 창만 맞추면 이 창들은 늘 100% 로 남아, 같은 앱인데 글자 크기가 다르다.
    if (event->type() == QEvent::Resize) {
        for (auto it = m_featureWindows.begin(); it != m_featureWindows.end(); ++it) {
            QWidget *w = it->data();
            if (w != obj) continue;
            if (auto *v = w->findChild<QWebEngineView *>()) {
                const qreal z = zoomForWidth(w->width());
                if (qAbs(v->zoomFactor() - z) >= 0.01) v->setZoomFactor(z);
            }
            break;
        }
    }

    // ★ 상단 띠(신호등이 있는 52px) 더블클릭 → 확대/원복.
    //   macOS 의 기본 동작이지만, 이 앱은 타이틀바를 투명하게 만들고
    //   (FullSizeContentView) 그 자리를 웹뷰가 덮고 있어 더블클릭이 창까지
    //   가지 못한다. 여기서 직접 받아 처리한다.
    //   · 본 창에서만 — 기능 창은 보통 타이틀바가 있어 OS 가 알아서 한다
    //     (예전엔 기능 창 위쪽을 더블클릭하면 본 창이 커졌다).
    //   · 띠 높이는 화면 배율을 따른다 — 배율 0.7 이면 36px. 고정 52px 이면
    //     툴바 바로 아래 본문을 더블클릭해도 창이 커졌다.
    if (event->type() == QEvent::MouseButtonDblClick
        && obj->isWidgetType() && static_cast<QWidget *>(obj)->window() == this) {
        auto *me = static_cast<QMouseEvent *>(event);
        if (me->button() == Qt::LeftButton) {
            const QPoint inWindow = mapFromGlobal(me->globalPosition().toPoint());
            // index.html 의 .sidebar-header / .toolbar 높이(52 CSS px) × 화면 배율
            const int kTitleStrip = qRound(52 * (m_webView ? m_webView->zoomFactor() : 1.0));
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

    // ★ 통합(일체형) 타이틀바 — 콘텐츠를 타이틀바 밑까지 올리는 일은 이제 생성자의
    //   ExpandedClientAreaHint 가 한다(Qt 가 레이아웃까지 맞춘다). 여기서는 제목 글자만
    //   감춘다 — 배경을 안 그려도 글자는 남아 신호등 옆에 '앱 이름 — 上野' 가 뜬다.
    //   (예전의 styleMask 직접 조작 + 1px 크기 흔들기는 걷어냈다: 흰 띠가 남았고,
    //    켤 때마다 창이 한 번 출렁였다.)
    {
        id nsView = reinterpret_cast<id>(winId());
        if (nsView) {
            id win = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(nsView, sel_registerName("window"));
            if (win) {
                reinterpret_cast<void (*)(id, SEL, BOOL)>(objc_msgSend)(
                    win, sel_registerName("setTitlebarAppearsTransparent:"), YES);
                reinterpret_cast<void (*)(id, SEL, long)>(objc_msgSend)(
                    win, sel_registerName("setTitleVisibility:"), 1 /* NSWindowTitleHidden */);
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
    //   (라이트 #FFFFFF / 다크 #16181C — index.html 의 darkroom-graft 토큰과 일치)
    //   예전 값(라이트 창 배경 #EDE9E1 베이지, 다크 #101114·#0F1115)은 토큰이 바뀐 뒤
    //   따라오지 않아, 창을 키우거나 처음 뜰 때 위쪽에 다른 색 띠가 잠깐씩 비쳤다.
    const QString bg = dark ? QStringLiteral("#16181C") : QStringLiteral("#FFFFFF");
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
    const double r = (dark ? 0x16 : 0xFF) / 255.0;
    const double g = (dark ? 0x18 : 0xFF) / 255.0;
    const double b = (dark ? 0x1C : 0xFF) / 255.0;
    id color = reinterpret_cast<id (*)(id, SEL, double, double, double, double)>(objc_msgSend)(
        reinterpret_cast<id>(objc_getClass("NSColor")),
        sel_registerName("colorWithSRGBRed:green:blue:alpha:"), r, g, b, 1.0);
    reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(win, sel_registerName("setBackgroundColor:"), color);
#endif
}
