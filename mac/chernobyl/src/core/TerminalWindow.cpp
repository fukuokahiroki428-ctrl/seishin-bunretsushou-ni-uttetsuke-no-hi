#include "TerminalWindow.h"

#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QLineEdit>
#include <QFont>
#include <QFontDatabase>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextCursor>
#include <QGuiApplication>
#include <QScreen>
#include <QCursor>
#include <QShortcut>
#include <QKeySequence>
#include <QCloseEvent>
#include <QDateTime>
#include <QRegularExpression>

// 화면에 겹치지 않게 계단식으로 놓는다 — 트랙이 여럿이면 나란히 보려는 것이 목적이다.
static int g_spawned = 0;

static const char *kStateLive = "color:#8B7CF7;font-size:11px;font-weight:600;";
static const char *kStateWarn = "color:#d29922;font-size:11px;font-weight:600;";
static const char *kStateDone = "color:#3fb950;font-size:11px;font-weight:600;";

TerminalWindow::TerminalWindow(const QString &key, const QString &title, Mode mode, QWidget *parent)
    : QWidget(parent, Qt::Window), m_key(key), m_baseTitle(title), m_mode(mode)
{
    setWindowTitle(m_baseTitle);
    setAttribute(Qt::WA_DeleteOnClose, false);   // 닫아도 객체는 남긴다(다시 열 수 있게)
    // ★ 이 창이 떠 있다고 앱이 안 꺼지면 안 된다 — 본 창이 기준이다.
    setAttribute(Qt::WA_QuitOnClose, false);
    resize(760, 440);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // ── 머리줄: 어디에 저장되는지와 진행 상태 ──
    auto *head = new QWidget(this);
    head->setStyleSheet("background:#1c1f26;border-bottom:1px solid #2a2e37;");
    auto *hl = new QHBoxLayout(head);
    hl->setContentsMargins(10, 6, 10, 6);
    m_path = new QLabel(head);
    m_path->setStyleSheet("color:#aab;font-size:11px;");
    m_path->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_state = new QLabel(mode == Interactive ? "대화" : "진행 중", head);
    m_state->setStyleSheet(kStateLive);
    hl->addWidget(m_path, 1);
    hl->addWidget(m_state, 0);

    // ── 중지 단추 ──
    //   보고 있는 자리에서 바로 멈출 수 있어야 한다. 닫기는 그냥 접는 것으로 둔다
    //   (머리 주석 참고). 대화형 창에는 멈출 수집이 없으니 두지 않는다.
    if (mode == Log) {
        m_stop = new QPushButton("⏹ 중지", head);
        m_stop->setCursor(Qt::PointingHandCursor);
        m_stop->setStyleSheet(
            "QPushButton{background:#2a2e37;color:#e6e8ee;border:1px solid #3a3f4a;"
            "border-radius:4px;padding:2px 10px;font-size:11px;}"
            "QPushButton:hover{background:#3a3f4a;}"
            "QPushButton:disabled{color:#666;border-color:#2a2e37;}");
        connect(m_stop, &QPushButton::clicked, this, [this]() {
            m_stop->setEnabled(false);
            m_stopped = true;
            m_state->setText("중지 요청됨");
            m_state->setStyleSheet(kStateWarn);
            emit stopRequested(m_key);
        });
        hl->addSpacing(8);
        hl->addWidget(m_stop, 0);
    }
    lay->addWidget(head, 0);

    // ── 본문 ──
    m_view = new QPlainTextEdit(this);
    m_view->setReadOnly(true);
    m_view->setMaximumBlockCount(5000);          // 오래 돌아도 메모리가 늘지 않게
    m_view->setWordWrapMode(QTextOption::WrapAnywhere);
    m_view->setFrameStyle(QFrame::NoFrame);

    // ★ 글꼴 — 맥의 고정폭부터 고른다. 한글·일본어는 CoreText 가 다른 글꼴에서
    //   가져와 그린다(윈도우 콘솔처럼 '?' 로 깨지지 않는다).
    QFont f;
    const QStringList prefer = { "SF Mono", "Menlo", "Monaco" };
    const QStringList have = QFontDatabase::families();
    for (const QString &name : prefer) {
        if (have.contains(name, Qt::CaseInsensitive)) { f.setFamily(name); break; }
    }
    f.setStyleHint(QFont::Monospace);
    f.setPointSize(12);
    m_view->setFont(f);
    m_view->setStyleSheet("QPlainTextEdit{background:#0f1115;color:#d6d9e0;padding:8px;"
                          "selection-background-color:#3a3f4a;}");
    lay->addWidget(m_view, 1);

    // ── 입력 줄(대화형만) ──
    if (mode == Interactive) {
        m_input = new QLineEdit(this);
        m_input->setFont(f);
        m_input->setPlaceholderText("여기에 적고 Return — 끝내려면 exit");
        m_input->setStyleSheet("QLineEdit{background:#161920;color:#e6e8ee;border:none;"
                               "border-top:1px solid #2a2e37;padding:8px 10px;}");
        connect(m_input, &QLineEdit::returnPressed, this, [this]() {
            const QString text = m_input->text();
            m_input->clear();
            // 터미널이 아니니 입력이 저절로 비치지 않는다 — 프롬프트 뒤에 직접 적어 준다.
            appendChunk(text + "\n");
            emit inputSubmitted(m_key, text);
        });
        lay->addWidget(m_input, 0);
    }

    // ⌘W 로 닫는다 — 다른 맥 창과 같게.
    auto *closeKey = new QShortcut(QKeySequence::Close, this);
    connect(closeKey, &QShortcut::activated, this, &QWidget::close);

    // 계단식 배치 — 마우스가 있는 화면의 오른쪽 위에서 시작해 겹치지 않게 물린다.
    QScreen *sc = QGuiApplication::screenAt(QCursor::pos());
    if (!sc) sc = QGuiApplication::primaryScreen();
    if (sc) {
        const QRect a = sc->availableGeometry();
        const int step = 28 * (g_spawned % 8);
        move(a.right() - width() - 40 - step, a.top() + 60 + step);
    }
    ++g_spawned;
}

QString TerminalWindow::stripAnsi(const QString &s)
{
    // 색(…m)만이 아니라 지우기·커서 이동(…J, …K, …H)도 뗀다. clear 가 남기는 것이 그것이다.
    static const QRegularExpression kEsc(QStringLiteral("\x1B\\[[0-9;?]*[A-Za-z]|\x1B[()][0-9A-Za-z]"));
    QString out = s;
    out.remove(kEsc);
    return out;
}

void TerminalWindow::begin(const QString &savePath)
{
    if (!savePath.isEmpty()) m_path->setText(savePath);
    m_state->setText(m_mode == Interactive ? "대화" : "진행 중");
    m_state->setStyleSheet(kStateLive);
    if (m_stop) m_stop->setEnabled(true);
    if (m_input) m_input->setEnabled(true);
    m_stopped = false;
    setWindowTitle(m_baseTitle);
    if (!m_view->document()->isEmpty()) {
        m_liveActive = false;
        m_view->appendPlainText(QString("──────── %1 새로 시작 ────────")
                                    .arg(QDateTime::currentDateTime().toString("HH:mm:ss")));
    }
}

bool TerminalWindow::atBottom() const
{
    const QScrollBar *sb = m_view->verticalScrollBar();
    return sb->value() >= sb->maximum() - 4;
}

void TerminalWindow::followBottom(bool wasAtBottom)
{
    // ★ 맨 아래를 보고 있었을 때만 따라간다. 위로 올려 읽는 중이면 방해하지 않는다.
    if (wasAtBottom) {
        QScrollBar *sb = m_view->verticalScrollBar();
        sb->setValue(sb->maximum());
    }
}

void TerminalWindow::replaceLastBlock(const QString &text)
{
    QTextCursor c(m_view->document()->lastBlock());
    c.movePosition(QTextCursor::StartOfBlock);
    c.movePosition(QTextCursor::EndOfBlock, QTextCursor::KeepAnchor);
    c.insertText(text);
}

void TerminalWindow::appendLine(const QString &line)
{
    const bool bottom = atBottom();
    if (m_liveActive) {
        replaceLastBlock(line);          // 진행률 줄이 떠 있던 자리를 마저 채운다
        m_liveActive = false;
    } else {
        m_view->appendPlainText(line);
    }
    followBottom(bottom);
}

void TerminalWindow::setLiveLine(const QString &line)
{
    const bool bottom = atBottom();
    if (m_liveActive) {
        replaceLastBlock(line);
    } else {
        m_view->appendPlainText(line);
        m_liveActive = true;
    }
    followBottom(bottom);
}

void TerminalWindow::appendChunk(const QString &text)
{
    if (text.isEmpty()) return;
    const bool bottom = atBottom();
    m_liveActive = false;
    QTextCursor c(m_view->document());
    c.movePosition(QTextCursor::End);
    QString t = text;
    t.remove(QLatin1Char('\r'));
    c.insertText(t);
    followBottom(bottom);
}

void TerminalWindow::markDone(const QString &label, bool stopped)
{
    if (m_stop) m_stop->setEnabled(false);   // 끝났으면 누를 것이 없다
    if (m_input) m_input->setEnabled(false);
    if (stopped) m_stopped = true;
    const bool asStopped = m_stopped;
    QString l = label.isEmpty() ? QStringLiteral("완료") : label;
    if (asStopped && !stopped) l = QStringLiteral("중지됨");   // 멈춘 판에 뒤늦게 온 '완료'
    m_state->setText(l);
    m_state->setStyleSheet(asStopped ? kStateWarn : kStateDone);
    setWindowTitle(m_baseTitle + (asStopped ? "  ⏹" : "  ✔"));
}

void TerminalWindow::setInputEnabled(bool on)
{
    if (m_input) {
        m_input->setEnabled(on);
        if (on) m_input->setFocus();
    }
}

void TerminalWindow::closeEvent(QCloseEvent *e)
{
    // 닫기는 접기다 — 객체는 남기고 숨긴다. 대화형이면 백엔드가 이 신호로 대화를 끝낸다.
    e->accept();
    emit closed(m_key);
}
