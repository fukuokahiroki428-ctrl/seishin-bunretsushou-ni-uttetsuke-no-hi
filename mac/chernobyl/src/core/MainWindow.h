#pragma once
#include <QPointer>
#include <QHash>

#include <QMainWindow>
#include <QWebEngineView>
#include <QWebChannel>
#include <QMenu>
#include <QProcess>

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

public:
    // 웹(테마 토글)에서 호출 — 네이티브 창 배경/외관을 앱 상단색과 맞춰 타이틀바 띠를 숨김
    void setChromeTheme(bool dark);

private:
    void setupMenu();
    void applyDarkTitlebar();
    void openFolderDialog();

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
