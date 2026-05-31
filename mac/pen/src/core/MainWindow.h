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

class PenBackend;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

    QWebEngineView *webView() const { return m_webView; }
    QWebEngineView *browserView() const { return m_browserView; }
    PenBackend *backend() const { return m_backend; }

    // Sleep prevention for background downloads
    void holdAwake();
    void releaseAwake();

    // Browser window (legacy — 펜 앱은 사용하지 않으나 API는 유지)
    void showBrowser(bool show);

    // Dock menu (macOS) — 상태 확인용
    QMenu *createDockMenu();
    void updateDockMenu();

protected:
    bool eventFilter(QObject *obj, QEvent *event) override;
    void closeEvent(QCloseEvent *event) override;

private:
    void setupMenu();
    void applyDarkTitlebar();
    void openFolderDialog();

    QWebEngineView *m_webView = nullptr;
    QWebEngineView *m_browserView = nullptr;
    QMainWindow *m_browserWindow = nullptr;
    QWebChannel *m_channel = nullptr;
    PenBackend *m_backend = nullptr;
    QMenu *m_dockMenu = nullptr;

#ifdef Q_OS_MACOS
    IOPMAssertionID m_sleepAssertion = 0;
    bool m_sleepAssertionHeld = false;
    QProcess *m_caffeinate = nullptr;  // caffeinate -dis: 덮개 닫아도 sleep 방지
#elif defined(Q_OS_WIN)
    bool m_sleepAssertionHeld = false;
#endif
};
