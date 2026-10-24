#include "MainWindow.h"
#include <QCursor>   // 창 끌기 — 커서를 따라 옮긴다
#include <QAbstractNativeEventFilter>
#include <QRectF>
#include <QSet>
#include <QUrlQuery>
#include <QDateTime>
#include <atomic>
#include "Config.h"   // 창 크기 복원 — config()->windowGeometry()
#include <QResizeEvent>
#include <QMouseEvent>   // 상단 띠 더블클릭(eventFilter 의 맥 갈래)
#include "HanishikiBackend.h"
#include "PenBackend.h"   // ★ PEN(팬을 잘 쓰고 싶다) 통합

#include <QWebEngineSettings>
#include <QVBoxLayout>
#include <QMenuBar>
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
    //   (680px 기준점)에 닿을 방법이 자체가 없었다 — 쓰지도 못할 CSS 였다.
    //   창을 핸드폰 폭까지 줄일 수 있게 낮춘다. 맥과 같은 값(360x480).
    setMinimumSize(360, 480);
    // ★ 기본 창 크기 — 좁으면 사이드바를 뺀 본문이 눌려 폼·로그가 다 깨진다.
    resize(1180, 820);

    // ★ Frameless — 네이티브 흰 타이틀바 제거. HTML 의 다크 툴바가 곧 타이틀바.
    //   드래그/리사이즈는 JS 가 backend.winStartMove()/winStartResize() 로
    //   QWindow::startSystemMove()/startSystemResize() 를 호출 → 네이티브 스냅·이동 유지.
    setWindowFlag(Qt::FramelessWindowHint);

    // QMainWindow 배경 — HTML 페이지 배경(--bg)과 일치시킨다.
    //   ★ 예전엔 흰색으로 고정이었다. 어두운 테마를 골라도 창 배경은 흰색이라
    //     가장자리·리사이즈 중에 흰 섬광이 보였다. 이제 테마를 따라간다.
    setChromeTheme(false);

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

    // ★ 페이지가 읽히기 전에 넣은 JS 를 잃지 않는다 — MainWindow.h 의 설명 참고.
    //   ※ setUrl() 보다 먼저 연결해야 한다. 뒤에 연결하면 첫 loadStarted 를 놓친다.
    connect(m_webView, &QWebEngineView::loadStarted, this, [this]() {
        // 다시 읽는 동안(새로고침 등)에도 JS 상태가 날아가므로 같이 잠근다.
        m_uiReady = false;
    });
    connect(m_webView, &QWebEngineView::loadFinished, this, [this](bool ok) {
        if (!ok) {
            // 실패해도 큐를 영원히 붙들지 않는다. 붙들면 메모리만 늘고 원인은 안 보인다.
            // 그냥 흘려서 예전처럼 콘솔 에러로 드러나게 두는 편이 낫다.
            qWarning() << "[UI] 페이지 로드 실패 — 담아 둔 JS" << m_pendingJs.size()
                       << "개를 그대로 흘린다";
        }
        m_uiReady = true;
        flushPendingJs();
    });

    // WebChannel
    m_channel = new QWebChannel(this);
    m_backend = new HanishikiBackend(this);
    m_channel->registerObject(QStringLiteral("backend"), m_backend);

    // ★ 지난번 창 크기·위치를 되살린다.
    //   전엔 켤 때마다 1180x820 으로 열려서, 좁게 줄여 놔도 다음 실행이면
    //   되돌아갔다 — 화면에 넣어 둔 좁은 폭(680px) 배치를 쓸 수가 없었다.
    //
    //   ※ 왜 Qt 의 saveGeometry/restoreGeometry 를 안 쓰나.
    //     처음엔 그걸 썼다. 그런데 이 빌드(Qt 6.7.3)에서 restoreGeometry 가
    //     자기가 방금 만든 66바이트 blob 을 되읽지 못하고 false 를 돌려줬다.
    //     저장은 되는데 복원만 조용히 안 되는, 가장 알아채기 어려운 형태였다.
    //     그리고 그 blob 은 Qt 판이 올라가면 형식이 바뀔 수 있다 — 1년을
    //     방치할 앱에서 '언젠가 못 읽게 되는 값' 을 쓸 이유가 없다.
    //     그래서 사람이 읽을 수 있는 숫자 넷으로 적는다: "x,y,너비,높이,최대화".
    //     설정 파일을 열어 보면 무엇이 저장됐는지 바로 보이고, 이상하면 손으로
    //     고칠 수도 있다.
    //
    //   ※ 설정은 원래 화면이 다 뜬 뒤 JS 가 backend.loadConfig() 를 불러야 읽힌다.
    //     창을 만드는 이 시점엔 아직 비어 있으므로 여기서 먼저 읽는다.
    //   ※ 모니터 구성이 바뀌어 창이 화면 밖에 놓이면 그냥 기본값으로 둔다 —
    //     안 그러면 보이지 않는 자리에서 열려 앱이 안 뜬 것처럼 보인다.
    if (m_backend && m_backend->config()) {
        m_backend->config()->load();
        const QStringList p = m_backend->config()->windowGeometry().split(',');
        bool restored = false;
        if (p.size() >= 4) {
            const int x = p[0].trimmed().toInt(), y = p[1].trimmed().toInt();
            const int w = p[2].trimmed().toInt(), h = p[3].trimmed().toInt();
            const bool wantMax = (p.size() >= 5 && p[4].trimmed().toInt() != 0);
            const QRect want(x, y, w, h);
            // 최소 크기보다 작거나, 지금 붙어 있는 화면 어디와도 안 겹치면 버린다.
            bool onScreen = false;
            for (QScreen *sc : QGuiApplication::screens())
                if (sc->availableGeometry().intersects(want)) { onScreen = true; break; }
            if (w >= minimumWidth() && h >= minimumHeight() && onScreen) {
                setGeometry(want);
                if (wantMax) setWindowState(windowState() | Qt::WindowMaximized);
                restored = true;
            }
        }
        qInfo().noquote() << QString("창 크기 복원: 저장값 '%1' → %2 (%3x%4)")
                                 .arg(m_backend->config()->windowGeometry(),
                                      restored ? QStringLiteral("복원함")
                                               : QStringLiteral("기본값 사용"))
                                 .arg(width()).arg(height());
    }

    // ★ PEN(팬을 잘 쓰고 싶다) 통합 — 같은 페이지에 2번째 백엔드 객체로 노출.
    //   UI 의 사이트 미러 탭이 penBackend.crawl* 를 호출. runJs/log 는 jsSignal/logSignal
    //   로 동작(JS 측에서 connect) — HanishikiBackend 와 동일 메커니즘, 같은 webView 공유.
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

    // ★ 화면이 다 뜬 뒤에야 사이드바 항목을 읽을 수 있다. 그때 '기능' 메뉴를 채운다.
    //   페이지가 다시 로드돼도(자가수리·새로고침) 다시 채워지도록 매번 건다.
    //   ★ 이 연결은 반드시 if/else '바깥' 이어야 한다. 처음엔 else 안에 넣는 바람에,
    //     번들 HTML(Resources/html/index.html)을 쓰는 실제 경로에서는 한 번도 걸리지
    //     않았다. 그래서 메뉴가 '목록을 읽는 중…' 에서 멈춰 있었다.
    //   ※ 잠금(m_uiReady)과 담아 둔 JS 흘리기는 위에서 setUrl() 보다 먼저 걸어 두었다
    //     (뒤에 걸면 첫 loadStarted 를 놓친다). 여기서 또 걸면 같은 큐를 두 번 다룬다.
    connect(m_webView, &QWebEngineView::loadFinished, this, [this](bool ok) {
        if (ok) { populatePlatformMenu(); applyZoom(); }
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
    // ★ 이미 생성돼 있는 자식 렌더 위젯들에도 드롭 허용 + 필터 설치 (ChildAdded 는 이후 자식만 잡음).
    //   Windows 에서 자식이 setAcceptDrops(false) 면 OS 드롭을 거부해 trad 드래그앤드롭이 안 먹는다.
    for (QObject *c : m_webView->children()) {
        if (c->isWidgetType()) {
            static_cast<QWidget *>(c)->setAcceptDrops(true);
            c->installEventFilter(this);
        }
    }

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

    // ★ 앱을 켜자마자 잠 방지를 걸지 않는다. 예전엔 0.5초 뒤 수집이 없어도 걸었고, 첫 수집이
    //   끝나기 전까지는 화면도 맥도 잠들지 못했다(caffeinate -d 가 화면까지 붙들었다).
    //   이제 일이 있을 때만 붙든다 — 수집·유튜브·니코동이 돌 때와 内閣会가 켜져 있을 때.
    //   内閣会를 이어 돌리게 해 두었으면 앱이 켜질 때 startNaikakukai 가 붙든다.
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

// 창 배경을 테마에 맞춘다 — UI 가 backend.setWindowChrome(dark) 로 부른다.
//   ★ 맥처럼 setStyleSheet 로 통째로 갈아끼우면 도크·터미널 규칙까지 날아간다.
//     배경 규칙만 앞에 붙이고 나머지는 그대로 유지한다.
void MainWindow::setChromeTheme(bool dark)
{
    // HTML 의 --bg 와 같은 값이어야 창 가장자리가 본문과 이어져 보인다.
    //   (라이트 #FFFFFF / 다크 #16181C — index.html 의 darkroom-graft 토큰과 일치. 맥과 같은 값)
    //   예전 다크 값 #101114 는 토큰이 바뀐 뒤 따라오지 않아, 창 가장자리·크기 조절 중에
    //   본문과 다른 색 띠가 비쳤다.
    const QString bg = dark ? QStringLiteral("#16181C") : QStringLiteral("#FFFFFF");
    setStyleSheet(QStringLiteral("QMainWindow { background-color: %1; }").arg(bg) + R"(

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
    dropAwake();   // 앱이 끝난다 — 아직 도는 일이 있어도 놓는다
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

    // ── 기능 메뉴 ─────────────────────────────────────────────────────────
    //   ★ 왜 필요한가. 각 플랫폼으로 들어가는 길이 앱 안 사이드바 하나뿐이었다.
    //     창을 좁히면 그 목록이 자리를 많이 먹고, macOS 상단 막대에는 File·창밖에
    //     없어서 '어디로 가야 하는지' 가 앱 안에만 있었다.
    //     상단 막대에서도 바로 들어갈 수 있게 한다.
    //
    //   ★ 이름을 코드에 박지 않는다. 탭이 늘거나 이름이 바뀌면 메뉴만 옛것으로
    //     남는다 — 이 저장소에서 그런 어긋남을 여러 번 겪었다.
    //     화면이 다 뜬 뒤 사이드바에서 실제 항목을 읽어 메뉴를 만든다.
    //   ※ 윈도우는 테두리 없는 창이라 메뉴막대를 숨긴다(생성자). 그래서 이 메뉴는 윈도우에서
    //     눈에 보이지 않는다 — 별도 창 전용 기능(data-window-only)으로 가는 길은 화면 쪽이 낸다.
    {
        m_platformMenu = menubar->addMenu("기능");
        auto *loading = m_platformMenu->addAction("목록을 읽는 중…");
        loading->setEnabled(false);
    }

    // ── 창(Window) 메뉴 ────────────────────────────────────────────────────
    //   ★ Windows 판은 FramelessWindowHint 라 OS 가 주는 최대화 버튼이 아예 없다.
    //     메뉴와 단축키로 확대 경로를 만들어 둔다.
    auto *winMenu = menubar->addMenu("창");

    auto *zoomAction = new QAction("확대 / 원래대로", this);
    zoomAction->setShortcut(QKeySequence("Ctrl+Shift+Z"));
    connect(zoomAction, &QAction::triggered, this, [this]() {
        if (isMaximized()) showNormal(); else showMaximized();
    });
    winMenu->addAction(zoomAction);

    auto *fullAction = new QAction("전체화면", this);
    fullAction->setShortcut(QKeySequence("Ctrl+Shift+F"));
    connect(fullAction, &QAction::triggered, this, [this]() {
        if (isFullScreen()) showNormal(); else showFullScreen();
    });
    winMenu->addAction(fullAction);

    winMenu->addSeparator();

    auto *fitAction = new QAction("화면에 맞추기", this);
    connect(fitAction, &QAction::triggered, this, [this]() {
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
#ifdef Q_OS_MACOS
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
#else
    // 윈도우는 테두리 없는 창을 화면(JS)→winStartMove()→startSystemMove() 로 끈다.
    //   끌 자리 좌표를 읽는 것은 맥의 네이티브 거름망(WindowDragFilter)뿐이라 받아 둘 곳이 없다.
    //   (DragRegions · g_dragRegions 는 위 #ifdef Q_OS_MACOS 안에만 있다 — 그대로 두면 MSVC 가 못 찾는다.)
    Q_UNUSED(key);
    Q_UNUSED(json);
#endif
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
    //   최대화 상태에서는 normalGeometry() 를 적는다 — 최대화된 화면 전체 크기를
    //   적어 두면, 최대화를 풀었을 때 돌아갈 자리가 없어진다.
    if (m_backend && m_backend->config()) {
        const QRect g = isMaximized() ? normalGeometry() : geometry();
        m_backend->config()->setWindowGeometry(
            QString("%1,%2,%3,%4,%5").arg(g.x()).arg(g.y())
                .arg(g.width()).arg(g.height()).arg(isMaximized() ? 1 : 0));
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

    // ★ 앱 안 터미널 창도 같이 닫는다(기능 창과 같은 이유 — 본 창이 사라지면 빈 창이다).
    //   예전엔 Terminal.app 을 osascript 로 부려 제목으로 탭을 찾아 닫았다. 그 길은 '자동화'
    //   허락을 물었고, 앱 이름이 바뀔 때마다 찾을 제목 목록이 낡았다. 이제 터미널 앱을 쓰지 않는다.
    if (m_backend) m_backend->closeAllTerminalWindows();

    event->accept();
}

void MainWindow::holdAwake(bool display)
{
    // 놓으려고 기다리던 것이 있으면 거둔다 — 새 일이 붙들었다(releaseAwake 의 유예)
    if (m_releaseTimer) m_releaseTimer->stop();
#ifdef Q_OS_MACOS
    // 1. IOPMAssertion — Prevent System Sleep
    if (!m_sleepAssertionHeld) {
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
    }

    // ★ 예전엔 여기서 NoIdleSleep('Hanishiki user activity') 를 하나 더 만들었다. 함수 안
    //   static 변수에 받아 두고 놓지 않아서, 수집을 시작할 때마다 하나씩 쌓였다(실측
    //   2026-10-17 — 한 프로세스에 셋이 살아 있었다). 첫 수집이 끝난 뒤로는 앱이 켜져
    //   있는 내내 맥이 유휴 잠에 들지 못했다. 유휴 잠은 위의 PreventSystemSleep 과
    //   caffeinate -i 가 이미 막는다. 따로 둘 까닭이 없어 없앴다.

    // 2. caffeinate — 덮개를 닫아도(AC 전원 시) 잠들지 않게. 이미 돌고 있어도 화면 몫은 올린다 —
    //    内閣会 대기(화면 없이) 중에 수집이 시작되면 그 수집 동안은 화면까지 붙든다.
    //    내리지는 않는다: 수집 중에 内閣会를 켜도 그 수집의 화면은 그대로 둔다.
    if (!m_caffeinate || (display && !m_awakeDisplay)) startCaffeinate(display);
#elif defined(Q_OS_WIN)
    Q_UNUSED(display);
    if (m_sleepAssertionHeld) return;
    SetThreadExecutionState(ES_CONTINUOUS | ES_SYSTEM_REQUIRED | ES_DISPLAY_REQUIRED);
    m_sleepAssertionHeld = true;
#endif
}

#ifdef Q_OS_MACOS
// caffeinate 를 (다시) 띄운다. -w 로 앱이 죽으면 저절로 끝난다.
//    -d display, -i idle, -m disk, -s system
//    ★ -u(사용자가 손댔다고 알림)는 뺐다. 수집을 시작할 때마다 맥을 깨워 화면을 켰고,
//      덮개를 닫은 채면 10초 FullWake → 'Clamshell Sleep' → DarkWake 를 되풀이했다.
//      그 넘어가는 순간 CDP 연결이 끊겼다(실측 2026-10-17, powerd 로그).
//    ★ -d 는 수집 동안만 둔다. 화면이 잠들면 macOS 가 그리기를 멈춰, 캡쳐 Chrome 이 늦게
//      뜨는 그림(지연 이미지)을 못 받는다. 덮개를 닫았을 때는 어차피 효과가 없다.
//      内閣会가 다음 폴링을 기다리는 동안에는 맥만 깨워 둔다 — 밤새 화면을 켜 둘 까닭이 없다.
void MainWindow::startCaffeinate(bool display)
{
    stopCaffeinate();
    m_caffeinate = new QProcess(this);
    QStringList args{display ? QStringLiteral("-dims") : QStringLiteral("-ims"),
                     "-w", QString::number(QCoreApplication::applicationPid())};
    m_caffeinate->start("/usr/bin/caffeinate", args);
    if (!m_caffeinate->waitForStarted(3000)) {
        delete m_caffeinate;
        m_caffeinate = nullptr;
        return;
    }
    m_awakeDisplay = display;
}

void MainWindow::stopCaffeinate()
{
    if (!m_caffeinate) return;
    m_caffeinate->terminate();
    m_caffeinate->waitForFinished(2000);
    if (m_caffeinate->state() == QProcess::Running) m_caffeinate->kill();
    delete m_caffeinate;
    m_caffeinate = nullptr;
    m_awakeDisplay = false;
}
#endif

void MainWindow::releaseAwake()
{
    // ★ 아직 깨어 있어야 할 일이 남았으면 놓지 않는다. 부르는 쪽은 '자기' 일이 끝났을 때
    //   부른다 — 예전엔 크롤이 끝나면 트위터가 아직 받고 있어도 놓았다.
    //   부르는 곳은 모두 자기 실행 깃발을 먼저 내리고 부른다. 그래서 isAnyRunning 이
    //   자기 자신을 '아직 도는 일' 로 세지 않는다.
    if (m_backend && m_backend->isAnyRunning()) return;
    // ★ 内閣会는 몇 분마다 깨어나 폴링한다. 그 사이 맥이 잠들면 다음 폴링이 오지 않으므로
    //   켜져 있는 동안은 맥을 붙든다(예전엔 위에서 새던 어설션이 우연히 이 몫을 했다).
    //   화면은 놓는다 — 수집이 끝났다.
    // ★ 수집이 끝난 뒤 이어 도는 일(자동 정리 · 백업 대기열)과 손으로 시작한 백업(지금 백업 · rclone ·
    //   원격)도 끝날 때까지 맥을 붙든다. 화면은 놓는다.
    //   예전엔 새던 NoIdleSleep 어설션이 이것들까지 우연히 지켰다. 없애고 나니 수집이 끝나고 몇 분 뒤
    //   (전원: 마지막 입력에서 약 11분) 맥이 잠들어 정리 · 백업이 멈췄다.
    if (m_backend && (m_backend->isNaikakukaiRunning() || m_backend->isAfterWorkRunning())) {
#ifdef Q_OS_MACOS
        if (m_caffeinate && m_awakeDisplay) startCaffeinate(false);
#endif
        return;
    }
    // ★ 바로 놓지 않고 몇 초 기다린다. 자동 정리는 수집이 끝났다는 알림을 받은 화면이 부르므로 이 함수보다
    //   늦게 시작하고, 줄 서 있던 다음 정리도 앞 정리가 끝난 뒤에 시작한다. 그 사이에 놓으면, 사람 손이 오래
    //   없던 맥은 바로 잠에 들 수 있다. 화면은 지금 놓는다 — 수집이 끝났다.
    //   기다리는 사이 새 일이 붙들면(holdAwake) 거두고, 다 기다리면 그때의 상태를 다시 본다.
#ifdef Q_OS_MACOS
    if (!m_sleepAssertionHeld && !m_caffeinate) return;   // 붙든 것이 없다
    if (m_caffeinate && m_awakeDisplay) startCaffeinate(false);
#elif defined(Q_OS_WIN)
    if (!m_sleepAssertionHeld) return;
#endif
    if (!m_releaseTimer) {
        m_releaseTimer = new QTimer(this);
        m_releaseTimer->setSingleShot(true);
        m_releaseTimer->setInterval(kReleaseGraceMs);
        connect(m_releaseTimer, &QTimer::timeout, this, [this]() {
            if (m_backend && (m_backend->isAnyRunning() || m_backend->isNaikakukaiRunning()
                              || m_backend->isAfterWorkRunning())) {
                releaseAwake();   // 그 사이 시작한 일이 있다 — 그 일에 맞춰 화면 몫만 정한다
                return;
            }
            dropAwake();
        });
    }
    m_releaseTimer->start();
}

void MainWindow::dropAwake()
{
    if (m_releaseTimer) m_releaseTimer->stop();
#ifdef Q_OS_MACOS
    if (m_sleepAssertionHeld) {
        IOPMAssertionRelease(m_sleepAssertion);
        m_sleepAssertion = 0;
        m_sleepAssertionHeld = false;
    }
    stopCaffeinate();
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

    // ★ 윈도우는 이 갈래를 쓰지 않는다. 테두리 없는 창이라 화면(JS)이 .toolbar·.sidebar-header 의
    //   더블클릭을 winToggleMaximize() 로 이미 받는다 — 단추·입력칸은 isInteractive 로 거른다.
    //   여기서도 받으면 이 거름망이 먼저 먹어(return true) 화면 쪽은 아무것도 못 받고,
    //   위 띠의 최대화 단추를 두 번 누르면 키웠다가 곧바로 되돌린다.
#ifndef Q_OS_WIN
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
#endif

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
            // ★ Windows: QWebEngine 실제 드롭 타깃은 lazily 생성되는 자식 렌더 위젯인데
            //   setAcceptDrops 기본값이 false 라 OS 드롭을 거부 → eventFilter 의 DragEnter accept
            //   가 와도 무의미했다(= trad 파일 드래그앤드롭이 Windows 에서 안 먹던 원인).
            //   자식 위젯도 드롭을 허용하게 하고 필터를 단다.
            static_cast<QWidget *>(child)->setAcceptDrops(true);
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
//   (runJsOnUi · flushPendingJs 는 맥과 같은 자리 — updateDockMenu 바로 뒤 — 에 하나만 둔다.)
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
