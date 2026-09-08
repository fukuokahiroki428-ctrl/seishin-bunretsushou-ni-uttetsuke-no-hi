#pragma once

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

class MiyoBackend;
class PenBackend;   // ★ PEN(팬을 잘 쓰고 싶다) 통합 — 2번째 백엔드 객체

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
    //   왜 필요한가 — setUrl() 은 비동기다. index.html 은 이 기계에서 2.6초 걸린다.
    //   그 사이에 넣은 JS 는 "appendLog is not defined" 로 조용히 사라졌다.
    //   실측으로 内閣会 자동 시작이 여기 물렸다: 수집은 도는데 setNaikakukaiRunning(true)
    //   가 사라져서, 화면의 '중지' 버튼이 잠긴 채로 남아 멈출 수가 없었다.
    //   "돌고 있는데 화면은 아니라고 한다" 는 조용한 고장이라 더 나쁘다.
    void runJsOnUi(const QString &js);
    bool uiReady() const { return m_uiReady; }
    QWebEngineView *browserView() const { return m_browserView; }
    void setChromeTheme(bool dark);   // 창 배경을 HTML 테마(--bg)에 맞춘다
    MiyoBackend *backend() const { return m_backend; }

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
    MiyoBackend *m_backend = nullptr;
    PenBackend *m_penBackend = nullptr;   // ★ PEN 통합
    QMenu *m_dockMenu = nullptr;

#ifdef Q_OS_MACOS
    IOPMAssertionID m_sleepAssertion = 0;
    bool m_sleepAssertionHeld = false;
    QProcess *m_caffeinate = nullptr;  // caffeinate -dis: 덮개 닫아도 sleep 방지
#elif defined(Q_OS_WIN)
    bool m_sleepAssertionHeld = false;
#endif
};
