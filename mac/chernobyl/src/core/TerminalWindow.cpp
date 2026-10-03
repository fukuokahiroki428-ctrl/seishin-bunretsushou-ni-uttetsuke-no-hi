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
#include <QShowEvent>
#include <QMouseEvent>
#include <QWindow>
#include <QDateTime>
#include <QRegularExpression>
#include <QColor>
#include <QTimer>
#include <QDir>
#include <QPixmap>
#include <QPointer>

#ifdef Q_OS_MACOS
#include <objc/objc.h>
#include <objc/message.h>
#endif

// 화면에 겹치지 않게 계단식으로 놓는다 — 트랙이 여럿이면 나란히 보려는 것이 목적이다.
static int g_spawned = 0;
static void snapshotIfAsked(QWidget *w, const QString &key, const QString &tag);   // 아래 — 점검용

// 맥: 신호등 단추가 차지하는 왼쪽 자리(본 창 .sidebar-header 와 같은 생각) — 머리줄 글자가 그 밑에 깔리지 않게.
#ifdef Q_OS_MACOS
static const int kTrafficLightInset = 78;
#else
static const int kTrafficLightInset = 12;
#endif

TerminalWindow::TerminalWindow(const QString &key, const QString &title, Mode mode, QWidget *parent)
    : QWidget(parent, Qt::Window), m_key(key), m_baseTitle(title), m_mode(mode)
{
    setWindowTitle(m_baseTitle);
    setAttribute(Qt::WA_DeleteOnClose, false);   // 닫아도 객체는 남긴다(다시 열 수 있게)
    // ★ 이 창이 떠 있다고 앱이 안 꺼지면 안 된다 — 본 창이 기준이다.
    setAttribute(Qt::WA_QuitOnClose, false);
    setMinimumSize(420, 220);
    resize(760, 460);

#ifdef Q_OS_MACOS
    // ★ 본 창과 같은 일체형 제목줄(MainWindow 생성자와 같은 세 줄) — 화면을 제목줄 밑까지 올리고,
    //   제목줄 배경은 그리지 않는다. 신호등 단추는 머리줄 위에 뜬다. 창 바탕색은 applyNativeChrome 이
    //   테마의 --bg 로 칠해, 제목줄 자리와 머리줄이 한 장으로 이어져 보인다.
    setWindowFlag(Qt::ExpandedClientAreaHint, true);
    setWindowFlag(Qt::NoTitleBarBackgroundHint, true);
    setAttribute(Qt::WA_ContentsMarginsRespectsSafeArea, false);
#endif

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->setSpacing(0);

    // ── 머리줄(= 제목줄): 무엇의 로그인지 · 어디에 저장되는지 · 상태 · 중지 ──
    m_head = new QWidget(this);
    m_head->setObjectName("termHead");
    m_head->setAttribute(Qt::WA_StyledBackground, true);
    m_head->setFixedHeight(40);
    auto *hl = new QHBoxLayout(m_head);
    hl->setContentsMargins(kTrafficLightInset, 0, 10, 0);
    hl->setSpacing(10);
    // 머리줄 제목은 '앱 이름 — ' 를 뗀 짧은 이름. 창 제목(창 메뉴·미션 컨트롤)은 그대로 둔다.
    const int dash = title.indexOf(QStringLiteral(" — "));
    m_title = new QLabel(dash >= 0 ? title.mid(dash + 3) : title, m_head);
    m_title->setObjectName("termTitle");
    m_path = new QLabel(m_head);
    m_path->setObjectName("termPath");
    m_path->setMinimumWidth(0);
    m_path->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);   // 길면 잘린다(창을 밀어내지 않는다)
    m_state = new QLabel(m_head);
    m_state->setObjectName("termState");
    hl->addWidget(m_title, 0);
    hl->addWidget(m_path, 1);
    hl->addWidget(m_state, 0);

    // ── 중지 단추 ──
    //   보고 있는 자리에서 바로 멈출 수 있어야 한다. 닫기는 그냥 접는 것으로 둔다
    //   (머리 주석 참고). 대화형 창에는 멈출 수집이 없으니 두지 않는다.
    if (mode == Log) {
        m_stop = new QPushButton(QStringLiteral("중지"), m_head);
        m_stop->setObjectName("termStop");
        m_stop->setCursor(Qt::PointingHandCursor);
        m_stop->setFocusPolicy(Qt::NoFocus);
        connect(m_stop, &QPushButton::clicked, this, [this]() {
            m_stop->setEnabled(false);
            m_stopped = true;
            setState(Stopped, QStringLiteral("중지 요청됨"));
            emit stopRequested(m_key);
        });
        hl->addWidget(m_stop, 0);
    }
    // 머리줄을 끌어 창을 옮긴다 — 제목줄 배경을 그리지 않으므로 이 띠가 곧 제목줄이다.
    for (QWidget *w : {m_head, static_cast<QWidget *>(m_title), static_cast<QWidget *>(m_path),
                       static_cast<QWidget *>(m_state)})
        w->installEventFilter(this);
    lay->addWidget(m_head, 0);

    // ── 본문 ──
    m_view = new QPlainTextEdit(this);
    m_view->setObjectName("termView");
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
    lay->addWidget(m_view, 1);

    // ── 입력 줄(대화형만) ──
    if (mode == Interactive) {
        m_input = new QLineEdit(this);
        m_input->setObjectName("termInput");
        m_input->setFont(f);
        m_input->setPlaceholderText(QStringLiteral("여기에 적고 Return — 끝내려면 exit"));
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

    setState(Live, mode == Interactive ? QStringLiteral("대화") : QStringLiteral("진행 중"));
    restyle();
}

QString TerminalWindow::stripAnsi(const QString &s)
{
    // 색(…m)만이 아니라 지우기·커서 이동(…J, …K, …H)도 뗀다. clear 가 남기는 것이 그것이다.
    static const QRegularExpression kEsc(QStringLiteral("\x1B\\[[0-9;?]*[A-Za-z]|\x1B[()][0-9A-Za-z]"));
    QString out = s;
    out.remove(kEsc);
    return out;
}

// ── 테마 ─────────────────────────────────────────────────────────────────
//   색은 앱 화면(index.html)의 토큰을 그대로 받는다 — 화면과 이 창이 같은 색이어야 한 앱으로 보인다.
//   받기 전(창이 먼저 뜬 경우)에는 맥 판 라이트 토큰(refine-office 층)을 쓴다.
QString TerminalWindow::tok(const char *name, const char *lightFallback, const char *darkFallback) const
{
    const QString v = m_pal.value(QLatin1String(name)).toString().trimmed();
    if (!v.isEmpty() && QColor::isValidColorName(v)) return v;
    return QString::fromLatin1(m_pal.value("dark").toBool() ? darkFallback : lightFallback);
}

void TerminalWindow::applyTheme(const QJsonObject &pal)
{
    m_pal = pal;
    restyle();
    snapshotIfAsked(this, m_key, pal.value("dark").toBool() ? QStringLiteral("dark") : QStringLiteral("light"));
}

void TerminalWindow::restyle()
{
    const QString bg      = tok("bg",            "#FFFFFF", "#16181C");
    const QString bg2     = tok("bgSecondary",   "#F7F8F9", "#1D2025");
    const QString bg3     = tok("bgTertiary",    "#EEF0F2", "#262A30");
    const QString card    = tok("card",          "#FFFFFF", "#1D2025");
    const QString text    = tok("text",          "#16181C", "#E8EAED");
    const QString text2   = tok("textSecondary", "#5A6069", "#A0A6AE");
    const QString text3   = tok("textTertiary",  "#6C717B", "#838A94");
    const QString border  = tok("border",        "#D8DCE0", "#333840");
    const QString accent  = tok("accent",        "#2C5FA8", "#6E9FE0");
    const QString success = tok("success",       "#16693F", "#5FD79A");
    const QString danger  = tok("danger",        "#C0392B", "#F08B80");
    const QString warning = tok("warning",       "#9A6700", "#E3B341");
    const QString stateColor = m_stateKind == Done ? success : (m_stateKind == Stopped ? warning : accent);
    Q_UNUSED(danger);

    // 창 바탕 = 앱 바탕(--bg). 머리줄도 같은 색이라 신호등 자리와 한 장으로 이어진다(일체형 제목줄).
    setStyleSheet(QStringLiteral(
        "TerminalWindow{background:%1;}"
        "#termHead{background:%1;border-bottom:1px solid %2;}"
        "#termTitle{color:%3;font-size:13px;font-weight:600;}"
        "#termPath{color:%4;font-size:11px;}"
        "#termState{color:%5;font-size:11px;font-weight:600;}"
        "#termStop{background:%6;color:%3;border:1px solid %2;border-radius:6px;padding:3px 12px;font-size:12px;}"
        "#termStop:hover{background:%7;}"
        "#termStop:pressed{background:%2;}"
        "#termStop:disabled{color:%4;}"
        "#termView{background:%8;color:%3;border:none;padding:10px 12px;"
        "  selection-background-color:%9;selection-color:%3;}"
        "#termInput{background:%6;color:%3;border:none;border-top:1px solid %2;padding:9px 12px;}"
        "QScrollBar:vertical{background:transparent;width:10px;margin:2px;}"
        "QScrollBar::handle:vertical{background:%2;border-radius:4px;min-height:24px;}"
        "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
        "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}")
        .arg(bg, border, text, text3, stateColor, card, bg3, bg2)
        .arg(QColor(accent).lighter(m_pal.value("dark").toBool() ? 60 : 185).name()));
    Q_UNUSED(text2);
    applyNativeChrome();
}

void TerminalWindow::applyNativeChrome()
{
#ifdef Q_OS_MACOS
    // ★ MainWindow::setChromeTheme 과 같은 일 — 창 외관(라이트/다크 신호등·스크롤바)과 창 바탕색.
    //   네이티브 창이 아직 없으면(처음 뜨기 전) showEvent 가 다시 부른다.
    if (!testAttribute(Qt::WA_WState_Created)) return;
    id nsView = reinterpret_cast<id>(winId());
    if (!nsView) return;
    id win = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend)(nsView, sel_registerName("window"));
    if (!win) return;
    reinterpret_cast<void (*)(id, SEL, BOOL)>(objc_msgSend)(
        win, sel_registerName("setTitlebarAppearsTransparent:"), YES);
    reinterpret_cast<void (*)(id, SEL, long)>(objc_msgSend)(
        win, sel_registerName("setTitleVisibility:"), 1 /* NSWindowTitleHidden */);
    const bool dark = m_pal.value("dark").toBool();
    id nm = reinterpret_cast<id (*)(id, SEL, const char *)>(objc_msgSend)(
        reinterpret_cast<id>(objc_getClass("NSString")), sel_registerName("stringWithUTF8String:"),
        dark ? "NSAppearanceNameDarkAqua" : "NSAppearanceNameAqua");
    id appr = reinterpret_cast<id (*)(id, SEL, id)>(objc_msgSend)(
        reinterpret_cast<id>(objc_getClass("NSAppearance")), sel_registerName("appearanceNamed:"), nm);
    reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(win, sel_registerName("setAppearance:"), appr);
    const QColor bg(tok("bg", "#FFFFFF", "#16181C"));
    id color = reinterpret_cast<id (*)(id, SEL, double, double, double, double)>(objc_msgSend)(
        reinterpret_cast<id>(objc_getClass("NSColor")),
        sel_registerName("colorWithSRGBRed:green:blue:alpha:"), bg.redF(), bg.greenF(), bg.blueF(), 1.0);
    reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend)(win, sel_registerName("setBackgroundColor:"), color);
#endif
}

// 점검용 — HANISHIKI_SNAPSHOT_DIR 이 있을 때만 이 창의 그림을 남긴다(공작함이 모양을 재려고 쓴다).
//   네이티브 창은 화면 녹화 권한 없이는 밖에서 찍을 수 없다. 평소에는 아무 일도 하지 않는다.
static void snapshotIfAsked(QWidget *w, const QString &key, const QString &tag)
{
    const QString dir = qEnvironmentVariable("HANISHIKI_SNAPSHOT_DIR");
    if (dir.isEmpty()) return;
    QPointer<QWidget> wp(w);
    QTimer::singleShot(700, w, [wp, dir, key, tag]() {
        if (!wp || !wp->isVisible()) return;
        QDir().mkpath(dir);
        QString k = key; k.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]")), QStringLiteral("_"));
        wp->grab().save(dir + "/terminal_" + k + "_" + tag + ".png");
    });
}

void TerminalWindow::showEvent(QShowEvent *e)
{
    QWidget::showEvent(e);
    applyNativeChrome();   // 네이티브 창이 이제 있다 — 제목줄·바탕색을 맞춘다
    snapshotIfAsked(this, m_key, QStringLiteral("show"));
}

bool TerminalWindow::eventFilter(QObject *obj, QEvent *ev)
{
    // 머리줄 끌기 = 창 옮기기, 두 번 누르기 = 확대/되돌리기(맥 제목줄과 같은 동작).
    if (ev->type() == QEvent::MouseButtonPress) {
        auto *me = static_cast<QMouseEvent *>(ev);
        if (me->button() == Qt::LeftButton && windowHandle()) {
            windowHandle()->startSystemMove();
            return true;
        }
    } else if (ev->type() == QEvent::MouseButtonDblClick) {
        auto *me = static_cast<QMouseEvent *>(ev);
        if (me->button() == Qt::LeftButton) {
            if (isMaximized()) showNormal(); else showMaximized();
            return true;
        }
    }
    return QWidget::eventFilter(obj, ev);
}

void TerminalWindow::setState(State s, const QString &text)
{
    m_stateKind = s;
    m_state->setText(text);
    restyle();
}

void TerminalWindow::begin(const QString &savePath)
{
    if (!savePath.isEmpty()) {
        m_path->setText(savePath);
        m_path->setToolTip(savePath);
    }
    if (m_stop) m_stop->setEnabled(true);
    if (m_input) m_input->setEnabled(true);
    m_stopped = false;
    setWindowTitle(m_baseTitle);
    setState(Live, m_mode == Interactive ? QStringLiteral("대화") : QStringLiteral("진행 중"));
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
    setState(asStopped ? Stopped : Done, l);
    setWindowTitle(m_baseTitle + (asStopped ? QStringLiteral(" — 중지됨") : QStringLiteral(" — 완료")));
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
