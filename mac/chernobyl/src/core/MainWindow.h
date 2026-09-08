#pragma once
#include <QPointer>
#include <QHash>

#include <QMainWindow>
#include <QWebEngineView>
#include <QWebChannel>
#include <QMenu>
#include <QProcess>
#include <QStringList>

#ifdef Q_OS_MACOS
#include <IOKit/pwr_mgt/IOPMLib.h>
#elif defined(Q_OS_WIN)
#include <windows.h>
#endif

class HanishikiBackend;
class PenBackend;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    QWebEngineView *webView() const { return m_webView; }

    // ★ 메인 UI 페이지에 JS 를 넣는 단 하나의 통로.
    //   페이지가 아직 안 읽혔으면 담아 두었다가 loadFinished 때 순서대로 흘린다.
    //
    //   왜 필요한가 — setUrl() 은 비동기다. index.html 은 한참 뒤에야 읽힌다
    //   (윈도우 기계 실측 2.6초). 그 사이에 넣은 JS 는 "appendLog is not defined"
    //   로 조용히 사라진다. 윈도우에서는 内閣会 자동 시작이 여기 물려서, 수집은
    //   도는데 화면의 '중지' 버튼이 잠긴 채로 남았다.
    //   맥에는 그 자동 시작이 없지만 시작 직후 로그가 사라지는 것은 똑같다.
    void runJsOnUi(const QString &js);
    bool uiReady() const { return m_uiReady; }
    QWebEngineView *browserView() const { return m_browserView; }
    HanishikiBackend *backend() const { return m_backend; }

    // Sleep prevention for background downloads
    void holdAwake();
    void releaseAwake();

    // Browser window for crawl tab
    void showBrowser(bool show);

    // Dock menu (macOS) — 상태 확인용
    QMenu *createDockMenu();
    void updateDockMenu();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

public:
    // 웹(테마 토글)에서 호출 — 네이티브 창 배경/외관을 앱 상단색과 맞춰 타이틀바 띠를 숨김
    void setChromeTheme(bool dark);

private:
    void flushPendingJs();      // 페이지가 뜬 뒤 담아 둔 JS 를 순서대로 흘린다
    void setupMenu();
    void applyDarkTitlebar();
    void openFolderDialog();

    bool m_uiReady = false;     // index.html 이 다 읽혔나
    QStringList m_pendingJs;    // 그 전에 들어온 JS
    int m_droppedJs = 0;        // 한도를 넘어 버린 개수(조용히 버리지 않기 위해)

    QWebEngineView *m_webView = nullptr;
    QWebEngineView *m_browserView = nullptr;
    QMainWindow *m_browserWindow = nullptr;
    QWebChannel *m_channel = nullptr;
    HanishikiBackend *m_backend = nullptr;
    PenBackend *m_penBackend = nullptr;
    QMenu *m_dockMenu = nullptr;
    QMenu *m_platformMenu = nullptr;   // 상단 막대의 '기능' 메뉴
    // 별도 창으로 여는 기능들(탭이름 → 창). 같은 탭을 다시 열면 새로 만들지 않고
    // 이미 있는 창을 앞으로 가져온다.
    QHash<QString, QPointer<QWidget>> m_featureWindows;
public:
    // 화면이 뜬 뒤 사이드바에서 실제 항목을 읽어 '기능' 메뉴를 채운다.
    void populatePlatformMenu();
    // 창 크기에 맞춰 화면 배율을 맞춘다(글자·여백·아이콘이 한꺼번에 비례한다).
    //   CSS 를 고치지 않는 이유: 이 화면은 px 지정이 1,200곳 넘고 font-size 규칙이
    //   215개다. 하나씩 상대 단위로 바꾸면 빠뜨린 곳이 반드시 생기고, 그때부터
    //   어떤 곳은 커지고 어떤 곳은 그대로인 어긋난 화면이 된다.
    static qreal zoomForWidth(int w);
    void applyZoom();
    // 기능 하나를 별도 창으로 연다(이미 열려 있으면 앞으로).
    void openFeatureWindow(const QString &tabId, const QString &title);
private:

#ifdef Q_OS_MACOS
    IOPMAssertionID m_sleepAssertion = 0;
    bool m_sleepAssertionHeld = false;
    QProcess *m_caffeinate = nullptr;  // caffeinate -dis: 덮개 닫아도 sleep 방지
#elif defined(Q_OS_WIN)
    bool m_sleepAssertionHeld = false;
#endif
};
