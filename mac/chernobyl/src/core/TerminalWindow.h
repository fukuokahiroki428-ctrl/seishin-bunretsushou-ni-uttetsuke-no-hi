#pragma once

// ═══════════════════════════════════════════════════════════════════════════
// 앱 안 터미널 창 — 트랙마다 하나씩 뜨는 별도 창.
//
// 예전 맥은 .command 스크립트를 만들어 macOS 터미널(Terminal.app)로 띄웠다.
// 그 창은 앱 바깥이라 우리 규칙이 아니라 터미널 앱의 규칙을 따랐다.
//
//   권한   터미널을 osascript 로 부리면 '자동화' 허락을 따로 물었고, 앱을 끌 때 창을
//          닫으려고 제목으로 탭을 훑었다(앱 이름이 바뀔 때마다 그 목록이 낡았다).
//   지연   로그 파일을 tail 하는 구조라 줄이 늦게 나타났고, 프로세스가 늘었다.
//   남음   끝나면 키 입력을 기다리며(read -n 1) 창이 계속 남았다.
//   소속   유튜브·니코동은 터미널이 yt-dlp 를 돌려, 앱이 꺼져도 따로 돌았다.
//
// 윈도우판이 먼저 같은 길로 갔다(TerminalWindow, a03567d · c0b90fd). 맥도 맞춘다 —
// 줄은 생기는 즉시 도착하고, 글꼴은 앱이 고르고, 중지는 창의 단추로 한다.
//
// ※ 창을 '닫는' 것은 중지가 아니다. 예전 맥은 터미널을 닫으면 수집이 멈췄지만,
//   여기서 닫기는 로그를 접는 가벼운 동작이다(윈도우와 같다). 로그만 치우려던
//   사용자가 수집을 잃으면 안 된다. 멈추려면 ⏹ 중지를 누른다.
// ═══════════════════════════════════════════════════════════════════════════

#include <QWidget>

class QPlainTextEdit;
class QLabel;
class QPushButton;
class QLineEdit;

class TerminalWindow : public QWidget
{
    Q_OBJECT
public:
    enum Mode {
        Log,          // 한 방향 — 로그를 보여 주고 중지 단추를 둔다
        Interactive   // 두 방향 — 아래에 입력 줄을 두고 중지 단추는 없다(ハニワ 대화)
    };

    TerminalWindow(const QString &key, const QString &title, Mode mode = Log,
                   QWidget *parent = nullptr);

    QString key() const { return m_key; }

    // 새 판을 시작한다 — 상태와 중지 단추를 되살리고, 앞 판 기록이 있으면 구분선을 긋는다.
    //   창은 판이 끝나도 남겨 두고 다음 판에 다시 쓴다(끝난 뒤에도 읽을 수 있어야 한다).
    void begin(const QString &savePath);

    void appendLine(const QString &line);   // 한 줄 — 진행률 줄이 떠 있으면 그 자리를 채운다
    void setLiveLine(const QString &line);  // \r 로 덮어쓰는 진행률 줄(콘솔에서 보던 모양)
    void appendChunk(const QString &text);  // 줄바꿈 없이 이어 붙인다(대화형 스트리밍)
    // 판이 끝났다. stopped 면 '멈춤' 으로 보인다. 한 번 멈춘 판은 뒤늦게 '완료' 가 와도
    //   멈춤으로 남는다 — 중지 뒤에도 수집 스레드는 마저 정리하고 '끝' 을 알리기 때문이다.
    void markDone(const QString &label = QString(), bool stopped = false);
    void setInputEnabled(bool on);

    // ANSI 색·커서 코드를 뗀다. 콘솔이 아니면 해석되지 않고 "[0m" 같은 날것이 보인다.
    static QString stripAnsi(const QString &s);

signals:
    void stopRequested(const QString &key);
    void inputSubmitted(const QString &key, const QString &text);
    void closed(const QString &key);

protected:
    void closeEvent(QCloseEvent *e) override;

private:
    void replaceLastBlock(const QString &text);
    bool atBottom() const;
    void followBottom(bool wasAtBottom);

    QString         m_key;
    QString         m_baseTitle;
    Mode            m_mode;
    QPlainTextEdit *m_view  = nullptr;
    QLabel         *m_path  = nullptr;
    QLabel         *m_state = nullptr;
    QPushButton    *m_stop  = nullptr;
    QLineEdit      *m_input = nullptr;
    bool            m_liveActive = false;   // 마지막 블록이 덮어쓸 진행률 줄인가
    bool            m_stopped = false;      // 이번 판을 사용자가 멈췄는가
};
