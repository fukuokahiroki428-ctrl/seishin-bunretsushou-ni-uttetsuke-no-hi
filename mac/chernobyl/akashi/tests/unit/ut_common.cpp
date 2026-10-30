// 공통 코드 단위 시험 — 빌드의 Common · FileHelper · HttpClient 오브젝트에 묶어 돌린다(앱 · 네트워크 없음). unit_common.py 가 빌드하고 부른다.
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>
#include <QTimeZone>
#include <atomic>
#include <cstdio>
#include "core/Common.h"
#include "utils/FileHelper.h"
#include "utils/HttpClient.h"

static int g_fail = 0;
static void check(const char *name, bool ok, const QString &detail = QString())
{
    if (!ok) ++g_fail;
    std::printf("  %s %s%s\n", ok ? "✔" : "✘", name,
                detail.isEmpty() ? "" : qPrintable(QStringLiteral("  — ") + detail));
    std::fflush(stdout);
}

static QJsonObject readMark(const QString &dir, const QString &kind)
{
    QFile f(dir + "/.hanishiki_walk_" + kind + ".json");
    if (!f.open(QIODevice::ReadOnly)) return QJsonObject();
    return QJsonDocument::fromJson(f.readAll()).object();
}

static void writeMark(const QString &dir, const QString &kind, const QDateTime &full, int v, qint64 top = 0)
{
    QJsonObject o{{"at", full.toString(Qt::ISODate)}, {"full", full.toString(Qt::ISODate)}, {"v", v}};
    if (top) o["top"] = QString::number(top);
    QFile f(dir + "/.hanishiki_walk_" + kind + ".json");
    if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(o).toJson(QJsonDocument::Compact));
}

// 다른 스레드에서 한 걸음을 돌린다 — 표시 기록은 걸음(스레드)마다 따로라 진짜 스레드가 필요하다
template <typename F> static void onThread(F fn)
{
    QThread *t = QThread::create(fn);
    t->start();
    t->wait();
    delete t;
}

static void testWalkMarks(const QString &root)
{
    // 형식 번호는 앱이 쓴 표시에서 읽는다
    const QString d0 = root + "/m0"; QDir().mkpath(d0);
    onThread([&]() { Common::takeCleanWalkMarks(d0, "k_"); Common::putCleanWalkMark(d0, "k_a"); });
    const QJsonObject first = readMark(d0, "k_a");
    const int v = first.value("v").toInt();
    const QDateTime now = QDateTime::currentDateTime();
    check("표시 없이 시작한 걸음 = 끝까지 본 걸음(full = 지금)", v > 0 && QDateTime::fromString(first.value("full").toString(), Qt::ISODate).secsTo(now) < 60, QString::number(v));

    const QDateTime old = now.addDays(-10);

    // 1) 같은 대상을 두 줄로 — 뒤에 시작한 걸음이 앞 걸음의 기록을 지우지 않는다
    {
        const QString d = root + "/m1"; QDir().mkpath(d);
        writeMark(d, "k_a", old, v);
        QSemaphore aTook, bDone;
        std::atomic<bool> armedA{false}, armedB{true};
        QThread *a = QThread::create([&]() {
            armedA = Common::takeCleanWalkMarks(d, "k_").contains("k_a");
            aTook.release();
            bDone.acquire();
            Common::putCleanWalkMark(d, "k_a");   // 일찍 멈춘 판이라 치자 — 앞 표시의 때를 물려받아야 한다
        });
        a->start();
        aTook.acquire();
        onThread([&]() { armedB = Common::takeCleanWalkMarks(d, "k_").contains("k_a"); });
        bDone.release();
        a->wait();
        delete a;
        const QDateTime full = QDateTime::fromString(readMark(d, "k_a").value("full").toString(), Qt::ISODate);
        check("앞 걸음은 표시로 시작 · 뒤 걸음은 표시 없이", armedA && !armedB);
        check("뒤 걸음이 시작해도 앞 걸음은 '끝까지 본 때' 를 물려받는다(지금으로 적지 않는다)",
              full.isValid() && qAbs(full.secsTo(old)) < 5, full.toString(Qt::ISODate));
    }

    // 2) 막힌(drop) 걸음은 다른 걸음이 시작해도 표시를 적지 못한다
    {
        const QString d = root + "/m2"; QDir().mkpath(d);
        writeMark(d, "k_a", old, v);
        QSemaphore aTook, go;
        QThread *a = QThread::create([&]() {
            Common::takeCleanWalkMarks(d, "k_");
            aTook.release();
            go.acquire();
            Common::putCleanWalkMark(d, "k_a");
        });
        a->start();
        aTook.acquire();
        Common::dropCleanWalkMarks(d, "k_");
        onThread([&]() { Common::takeCleanWalkMarks(d, "k_"); });   // 다른 걸음의 시작 — 예전엔 여기서 막힘이 풀렸다
        go.release();
        a->wait();
        delete a;
        check("막힌 걸음은 다른 걸음이 시작한 뒤에도 표시를 적지 않는다", readMark(d, "k_a").isEmpty());
    }

    // 3) 끊긴 판은 읽은 그대로 되돌린다(top 까지) · 다른 걸음은 그 기록을 보지 않는다
    {
        const QString d = root + "/m3"; QDir().mkpath(d);
        writeMark(d, "k_a", old, v, 1234567);
        onThread([&]() {
            check("믿을 만한 표시를 읽었다", Common::takeCleanWalkMark(d, "k_a"));
            check("이 걸음의 top 이 보인다", Common::takenWalkMark(d, "k_a").value("top").toString() == "1234567");
            onThread([&]() { check("다른 걸음에게는 보이지 않는다", Common::takenWalkMark(d, "k_a").isEmpty()); });
            check("끊긴 판은 되돌린다", Common::restoreWalkMark(d, "k_a"));
        });
        const QJsonObject back = readMark(d, "k_a");
        check("되돌린 표시 — top · 끝까지 본 때가 그대로", back.value("top").toString() == "1234567"
              && qAbs(QDateTime::fromString(back.value("full").toString(), Qt::ISODate).secsTo(old)) < 5);
    }

    // 4) 끊긴 걸음의 옛 기록을 다음 걸음이 물려받지 않는다
    {
        const QString d = root + "/m4"; QDir().mkpath(d);
        writeMark(d, "k_a", old, v);
        onThread([&]() { Common::takeCleanWalkMarks(d, "k_"); });      // 표시를 가져간 채 끊김(되돌리지 않음)
        onThread([&]() { Common::takeCleanWalkMarks(d, "k_"); Common::putCleanWalkMark(d, "k_a"); });
        const QDateTime full = QDateTime::fromString(readMark(d, "k_a").value("full").toString(), Qt::ISODate);
        check("끊긴 걸음 뒤 표시 없이 끝까지 본 걸음은 full = 지금", full.isValid() && full.secsTo(QDateTime::currentDateTime()) < 60, full.toString(Qt::ISODate));
    }

    // 5) 오래된 표시 · 다른 형식은 믿지 않는다
    {
        const QString d = root + "/m5"; QDir().mkpath(d);
        writeMark(d, "k_old", now.addDays(-40), v);
        writeMark(d, "k_ver", now.addDays(-1), v + 1);
        const QStringList got = Common::takeCleanWalkMarks(d, "k_");
        check("40일 지난 표시 · 다른 형식은 끝까지 본다", got.isEmpty(), got.join(','));
    }

    check("캡처가 한 판 내내 0개면 막힌 길로 본다", !Common::captureFailuresCount(0, 5) && Common::captureFailuresCount(1, 9) && Common::captureFailuresCount(0, 4));
    check("kind 의 캡처 설정", Common::captureKindCode(QJsonObject{}) == 1
          && Common::captureKindCode(QJsonObject{{"realCapture", false}}) == 0
          && Common::captureKindCode(QJsonObject{{"realCaptureMode", "off"}}) == 0
          && Common::captureKindCode(QJsonObject{{"realCaptureMode", "media"}}) == 2);
}

static void testNames(const QString &root)
{
    const QString parent = root + "/profiles";
    QDir().mkpath(parent + "/Foopa(@abc)");
    check("대소문자만 다른 이름 → 디스크의 이름", FileHelper::reuseExistingName(parent, "foopa(@abc)") == "Foopa(@abc)");
    check("정확히 같은 이름 → 그대로", FileHelper::reuseExistingName(parent, "Foopa(@abc)") == "Foopa(@abc)");
    check("없는 이름 → 그대로", FileHelper::reuseExistingName(parent, "NewOne") == "NewOne");
    check("같은 판에서 뒤에 온 변형 → 앞의 이름", FileHelper::reuseExistingName(parent, "newone") == "NewOne");
}

static void testIntegrity(const QString &root)
{
    const auto put = [&](const QString &name, const QByteArray &data) {
        const QString p = root + "/" + name;
        QFile f(p); if (f.open(QIODevice::WriteOnly)) { f.write(data); f.close(); }
        return p;
    };
    static const char kJpeg1x1[] =
        "/9j/4AAQSkZJRgABAQEAYABgAAD/2wBDAAgGBgcGBQgHBwcJCQgKDBQNDAsLDBkSEw8UHRof"
        "Hh0aHBwgJC4nICIsIxwcKDcpLDAxNDQ0Hyc5PTgyPC4zNDL/wAALCAABAAEBAREA/8QAFAAB"
        "AAAAAAAAAAAAAAAAAAAACf/EABQQAQAAAAAAAAAAAAAAAAAAAAD/2gAIAQEAAD8AKp//2Q==";
    QByteArray avif = QByteArray::fromHex("0000001c") + "ftypavif" + QByteArray(240, '\x01');
    QString r = Common::checkFileIntegrity(put("ok.avif", avif));
    check("멀쩡한 AVIF 헤더 → 손상 아님", r.isEmpty(), r);
    r = Common::checkFileIntegrity(put("bad.avif", QByteArray(300, 'x')));
    check("AVIF 도 그림도 아닌 내용 → 손상", r.contains("AVIF"), r);
    r = Common::checkFileIntegrity(put("jpeg.avif", QByteArray::fromBase64(QByteArray(kJpeg1x1))));
    check("이름만 .avif 이고 내용은 JPEG → 손상 아님", r.isEmpty(), r);
    r = Common::checkFileIntegrity(put("bad.jpg", QByteArray(300, 'y')));
    check("깨진 JPEG → 디코딩 실패", r.contains("디코딩"), r);
}

static void testFileTimes(const QString &root)
{
    const QString p = root + "/t.bin";
    { QFile f(p); if (f.open(QIODevice::WriteOnly)) f.write("x"); }
    const qint64 epoch = 1556712000;   // 2019-05-01 12:00:00 UTC
    Common::setFileTimes(p, QDateTime::fromSecsSinceEpoch(epoch, QTimeZone::UTC));
    const QFileInfo fi(p);
    check("수정 날짜 = 글의 시각", fi.lastModified().toSecsSinceEpoch() == epoch);
    check("만든 날짜 = 글의 시각(시간대와 무관 — TZ 를 뉴욕으로 두고 돌린다)", fi.birthTime().toSecsSinceEpoch() == epoch,
          QString::number(fi.birthTime().toSecsSinceEpoch()));
    // 만든 날짜를 앞으로 옮기는 것도 된다(utime 만으로는 뒤로만 간다)
    Common::setFileTimes(p, QDateTime::fromSecsSinceEpoch(epoch + 86400, QTimeZone::UTC));
    check("다시 받아 더 새 시각이 되어도 만든 날짜가 따라온다", QFileInfo(p).birthTime().toSecsSinceEpoch() == epoch + 86400);
}

static void testUa()
{
    QStringList got;
    QMutex mu;
    QList<QThread *> ts;
    for (int i = 0; i < 8; ++i) {
        ts << QThread::create([&]() {
            for (int k = 0; k < 200; ++k) {
                const QString ua = Common::browserUserAgent();
                if (k == 0) { QMutexLocker lk(&mu); got << ua; }
            }
        });
    }
    for (QThread *t : ts) t->start();
    for (QThread *t : ts) { t->wait(); delete t; }
    bool same = !got.isEmpty();
    for (const QString &u : got) same = same && u == got.first() && u.contains("Chrome/");
    check("여러 스레드가 동시에 불러도 같은 Chrome UA", same && got.size() == 8, got.value(0));

    QTcpServer srv;
    srv.listen(QHostAddress::LocalHost, 0);
    const quint16 port = srv.serverPort();
    QByteArray seen;
    QThread *t = QThread::create([&]() {
        HttpClient http;
        http.get(QString("http://127.0.0.1:%1/ua").arg(port));
    });
    t->start();
    if (srv.waitForNewConnection(15000)) {
        QTcpSocket *s = srv.nextPendingConnection();
        while (!seen.contains("\r\n\r\n") && s->waitForReadyRead(5000)) seen += s->readAll();
        s->write("HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok");
        s->flush();
        s->waitForBytesWritten(2000);
        s->disconnectFromHost();
    }
    t->wait(20000);
    delete t;
    QString ua;
    bool hints = false;
    for (const QByteArray &line : seen.split('\n')) {
        if (line.toLower().startsWith("user-agent:")) ua = QString::fromUtf8(line.mid(11)).trimmed();
        if (line.toLower().startsWith("sec-ch-ua")) hints = true;
    }
    check("UA 없이 보낸 요청 → 설치된 Chrome 판의 UA(Safari 17.5 가 아니다)", ua == Common::browserUserAgent() && !ua.contains("Version/17.5"), ua);
    check("http 요청에는 힌트를 싣지 않는다(진짜 Chrome 과 같다)", !hints);
    const QMap<QString, QString> h = Common::browserClientHints();
    check("https 에 실을 힌트의 판 번호가 UA 와 같다", ua.contains("Chrome/" + h.value("sec-ch-ua").section("v=\"", 1, 1).section('"', 0, 0) + "."), h.value("sec-ch-ua"));
}

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir td;
    testWalkMarks(td.path());
    testNames(td.path());
    testIntegrity(td.path());
    testFileTimes(td.path());
    testUa();
    std::printf("%s\n", g_fail ? "실패" : "통과");
    return g_fail ? 1 : 0;
}
