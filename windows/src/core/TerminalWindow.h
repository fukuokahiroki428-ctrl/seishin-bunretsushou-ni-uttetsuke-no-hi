#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// 수집 모니터 창 — 트랙마다 하나씩 뜨는 별도 창.
//
// 예전에는 cmd.exe 를 CREATE_NEW_CONSOLE 로 띄워 로그 파일을 tail 했다. 그 창은
// 우리 규칙이 아니라 conhost 의 규칙을 따라서, 세 가지가 계속 문제였다.
//
//   글꼴   기본이 Consolas 라 한글·일본어 글리프가 없어 전부 '?' 로 그려졌다.
//          창마다 SetCurrentConsoleFontEx 로 바꿔야 했고, CONSOLE_FONT_INFOEX 가
//          84바이트인데 4바이트만 어긋나도 ERROR_INVALID_PARAMETER 로 조용히 실패했다.
//   지연   파일을 폴링해 읽는 구조라 새 줄이 폴링 간격만큼 늦게 나타났다.
//   낭비   트랙마다 cmd + conhost 두 프로세스가 붙었다.
//
// 앱이 직접 그리면 셋 다 없다. 글꼴은 우리가 고르고, 줄은 생기는 즉시 도착하고,
// 프로세스는 늘지 않는다. 창은 여전히 따로 떠서 나란히 놓고 볼 수 있다.
// ═══════════════════════════════════════════════════════════════════════════

#include <QWidget>

class QPlainTextEdit;
class QLabel;
class QPushButton;

class TerminalWindow : public QWidget
{
    Q_OBJECT
public:
    explicit TerminalWindow(const QString &trackKey, const QString &savePath,
                            QWidget *parent = nullptr);

    void appendLine(const QString &line);
    void markDone();

signals:
    // ★ 이 창에서 '중지' 를 눌렀다. 맥의 STOP sentinel 워치독과 같은 목적이다 —
    //   수집을 보고 있는 자리에서 바로 멈출 수 있어야 한다.
    //   ※ 창을 '닫는' 것은 중지가 아니다. 맥에서 터미널을 닫는 것은 파이프라인을
    //     끊는 것이지만, 여기서 닫기는 로그를 접는 가벼운 동작이다. 로그만 치우려던
    //     사용자가 수집을 잃으면 안 된다.
    void stopRequested(const QString &trackKey);

private:
    QString         m_trackKey;
    QPlainTextEdit *m_view  = nullptr;
    QLabel         *m_state = nullptr;
    QPushButton    *m_stop  = nullptr;
};
