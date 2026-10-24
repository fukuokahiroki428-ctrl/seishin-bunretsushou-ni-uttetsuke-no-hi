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
class PenBackend;   // ★ PEN(팬을 잘 쓰고 싶다) 통합 — 2번째 백엔드 객체

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    QWebEngineView *webView() const { return m_webView; }

    // ★ 본 창 + 메뉴막대로 띄운 기능 창 전부의 웹뷰.
    //   백엔드가 화면에 무언가를 보낼 때 본 창에만 보내면, 기능 창은 명령은
    //   보낼 수 있는데(채널 공유) 로그·진행·결과는 하나도 못 받는 반쪽이 된다.
    QList<QWebEngineView *> allWebViews() const;

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
    void setChromeTheme(bool dark);   // 창 배경을 HTML 테마(--bg)에 맞춘다
    HanishikiBackend *backend() const { return m_backend; }

    // Sleep prevention for background downloads
    //   holdAwake()      — 수집: 맥 + 화면
    //   holdAwake(false) — 内閣会 켜짐: 맥만(폴링 사이에 화면까지 켜 둘 까닭이 없다)
    //   releaseAwake     — 수집이 다 끝났으면 화면을 놓고, 内閣会도 꺼져 있고 이어 도는 일(자동 정리 ·
    //                      백업 대기열 · 손으로 시작한 백업)도 없으면 몇 초 뒤 전부 놓는다(그 사이 시작하는
    //                      일을 위해).
    //   이어 도는 일은 시작할 때 holdAwake(false) 로 붙들고 끝나면 releaseAwake 를 부른다.
    void holdAwake(bool display = true);
    void releaseAwake();

    // Browser window for crawl tab
    void showBrowser(bool show);

    // Dock menu (macOS) — 상태 확인용
    QMenu *createDockMenu();
    void updateDockMenu();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;
    void changeEvent(QEvent *event) override;   // 최대화/복원 시 JS 버튼 아이콘 동기화
#ifdef Q_OS_WIN
    // frameless 최대화가 작업표시줄을 덮지 않도록 WM_GETMINMAXINFO 처리
    bool nativeEvent(const QByteArray &eventType, void *message, qintptr *result) override;
#endif

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
    PenBackend *m_penBackend = nullptr;   // ★ PEN 통합
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
    // 설정 '창 크기를 바꿀 때' — 배치 바꾸기 / 둘 다(기본) / 통째로 크기 조절
    enum ScaleMode { ScaleLayout, ScaleBoth, ScaleWhole };
    void setScaleMode(ScaleMode mode);
    // 기능 하나를 별도 창으로 연다(이미 열려 있으면 앞으로).
    void openFeatureWindow(const QString &tabId, const QString &title);
    // 창 끌기 — JS 가 끌 수 있는 빈 곳에서 5px 넘게 끌면 부른다. 그때부터 왼쪽 버튼을
    // 떼기까지 커서를 따라 커서 아래 창을 옮긴다(MainWindow.cpp 설명).
    void armWindowMove();
private:
    void dragTick();
    void startDragTimer();                 // 옛 방식(8ms 타이머) — 창 서버 끌기가 안 먹을 때만
public:
    // 화면(JS)이 '마우스가 끌 수 있는 곳 위에 있다/없다' 를 알린다. 누르는 순간 이 값으로 판단한다.
    static void setDragHover(bool on);
    // 화면이 끌 자리(사이드바·위쪽 띠)와 그 안 단추 자리를 CSS 픽셀 사각형으로 넘긴다.
    static void setDragRegions(const QString &key, const QString &json);
    static void noteNativeDrag();
    static void installWindowDragFilter();
protected:
    // 창 폭이 바뀌면 화면 배율(applyZoom)을 50ms 에 한 번 맞춘다 — MainWindow.cpp 설명.
    //   (맥 갈래에는 원래 위 protected 칸에 있던 선언이다. 윈도우 머리에는 없어서 여기서 함께 넣는다.)
    void resizeEvent(QResizeEvent *event) override;
private:
    void dropAwake();                      // 묻지 않고 놓는다 — releaseAwake 와 앱 종료가 쓴다
    class QTimer *m_releaseTimer = nullptr;   // releaseAwake 의 유예 — 다 기다리면 다시 보고 놓는다
    static constexpr int kReleaseGraceMs = 5000;
    static qint64 s_nativeDragAt;
    QPointer<QWidget> m_dragWin;           // 지금 끌고 있는 창(본 창 또는 기능 창)
    QPoint m_dragOffset;                   // 커서 - 창 왼쪽 위
    class QTimer *m_dragTimer = nullptr;   // 끄는 동안 8ms 마다 따라간다
    class QTimer *m_zoomTimer = nullptr;   // 창 크기 바뀔 때 배율 맞추기를 50ms 에 한 번으로

#ifdef Q_OS_MACOS
    IOPMAssertionID m_sleepAssertion = 0;
    bool m_sleepAssertionHeld = false;
    QProcess *m_caffeinate = nullptr;  // caffeinate -dims / -ims: 덮개 닫아도 sleep 방지(AC)
    bool m_awakeDisplay = false;       // 지금 caffeinate 가 화면(-d)까지 붙드나
    void startCaffeinate(bool display);
    void stopCaffeinate();
#elif defined(Q_OS_WIN)
    bool m_sleepAssertionHeld = false;
#endif
};
