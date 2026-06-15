#pragma once

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

class MiyoBackend;
class PenBackend;   // ★ PEN(팬을 잘 쓰고 싶다) 통합 — 2번째 백엔드 객체

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    QWebEngineView *webView() const { return m_webView; }
    QWebEngineView *browserView() const { return m_browserView; }
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
    void setupMenu();
    void applyDarkTitlebar();
    void openFolderDialog();

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
