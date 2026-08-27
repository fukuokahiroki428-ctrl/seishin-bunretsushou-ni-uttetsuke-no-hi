#include "TerminalWindow.h"

#include <QPlainTextEdit>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QFont>
#include <QFontDatabase>
#include <QScrollBar>
#include <QGuiApplication>
#include <QScreen>

// 화면에 겹치지 않게 계단식으로 놓는다 — 트랙이 여럿이면 나란히 보려는 것이 목적이다.
static int g_spawned = 0;

TerminalWindow::TerminalWindow(const QString &trackKey, const QString &savePath, QWidget *parent)
    : QWidget(parent), m_trackKey(trackKey)
{
    setWindowTitle(QString("Predormition — %1").arg(trackKey.toUpper()));
    setAttribute(Qt::WA_DeleteOnClose, false);   // 닫아도 객체는 남긴다(다시 열 수 있게)
    resize(760, 420);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // ── 머리줄: 어디에 저장되는지와 진행 상태 ──
    auto *head = new QWidget(this);
    head->setStyleSheet("background:#1c1f26;border-bottom:1px solid #2a2e37;");
    auto *hl = new QHBoxLayout(head);
    hl->setContentsMargins(10, 6, 10, 6);
    auto *title = new QLabel(savePath.isEmpty() ? trackKey : savePath, head);
    title->setStyleSheet("color:#aab;font-size:11px;");
    title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_state = new QLabel("수집 중", head);
    m_state->setStyleSheet("color:#8B7CF7;font-size:11px;font-weight:600;");
    hl->addWidget(title, 1);
    hl->addWidget(m_state, 0);
    lay->addWidget(head, 0);

    // ── 본문 ──
    m_view = new QPlainTextEdit(this);
    m_view->setReadOnly(true);
    m_view->setMaximumBlockCount(5000);          // 오래 돌아도 메모리가 늘지 않게
    m_view->setWordWrapMode(QTextOption::WrapAnywhere);
    m_view->setFrameStyle(QFrame::NoFrame);

    // ★ 글꼴 — 한글·일본어가 있는 고정폭부터 고른다.
    //   conhost 와 달리 Qt 는 없는 글리프를 다른 글꼴에서 가져오는 폴백이 실제로 동작하지만,
    //   처음부터 있는 것을 고르는 편이 줄 간격이 흐트러지지 않는다.
    QFont f;
    const QStringList prefer = { "Cascadia Mono", "D2Coding", "Malgun Gothic", "MS Gothic", "Consolas" };
    const QStringList have = QFontDatabase::families();
    for (const QString &name : prefer) {
        if (have.contains(name, Qt::CaseInsensitive)) { f.setFamily(name); break; }
    }
    f.setStyleHint(QFont::Monospace);
    f.setPointSize(10);
    m_view->setFont(f);
    m_view->setStyleSheet("QPlainTextEdit{background:#0f1115;color:#d6d9e0;padding:8px;}");
    lay->addWidget(m_view, 1);

    // 계단식 배치 — 오른쪽 아래에서 시작해 겹치지 않게 물린다.
    if (QScreen *sc = QGuiApplication::primaryScreen()) {
        const QRect a = sc->availableGeometry();
        const int step = 28 * (g_spawned % 8);
        move(a.right() - width() - 40 - step, a.top() + 60 + step);
    }
    ++g_spawned;
}

void TerminalWindow::appendLine(const QString &line)
{
    // ★ 항상 맨 아래를 따라간다. 단, 사용자가 위로 스크롤해 읽는 중이면 방해하지 않는다.
    QScrollBar *sb = m_view->verticalScrollBar();
    const bool atBottom = sb->value() >= sb->maximum() - 4;
    m_view->appendPlainText(line);
    if (atBottom) sb->setValue(sb->maximum());
}

void TerminalWindow::markDone()
{
    m_state->setText("완료");
    m_state->setStyleSheet("color:#3fb950;font-size:11px;font-weight:600;");
    setWindowTitle(windowTitle() + "  ✔");
}
