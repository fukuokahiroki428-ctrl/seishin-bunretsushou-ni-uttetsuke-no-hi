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
#include <csignal>
#include <cstdio>
#include <sys/resource.h>
#include <QCryptographicHash>
#include "core/Common.h"
#include "utils/FileHelper.h"
#include "utils/HttpClient.h"
#include "utils/ExcelWriter.h"
#include "utils/SelfRepair.h"

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

// 이 시험은 TZ 를 뉴욕으로 두고 돈다(unit_common.py) — 맥의 시간대와 무관하게 일본 시간인지 본다
static void testJst()
{
    // 2026-01-01 23:30 UTC = 2026-01-02 08:30 JST
    const qint64 e = 1767310200;
    const QString want = QStringLiteral("20260102_083000_");
    check("이름 앞 날짜 — 트위터 글 시각(UTC 로 읽음) → 일본 시간", FileHelper::uploadOrderPrefix(Common::parseISODate("Thu Jan 01 23:30:00 +0000 2026")) == want,
          FileHelper::uploadOrderPrefix(Common::parseISODate("Thu Jan 01 23:30:00 +0000 2026")));
    check("이름 앞 날짜 — 픽시브 글 시각(+09:00) → 그대로", FileHelper::uploadOrderPrefix(Common::parseISODate("2026-01-02T08:30:00+09:00")) == want);
    check("이름 앞 날짜 — 숫자 시각 → 맥의 시간대와 무관", FileHelper::uploadOrderPrefix(e) == want, FileHelper::uploadOrderPrefix(e));
    check("이름 앞 날짜 — 맥의 현지 시각 값이 들어와도 일본 시간", FileHelper::uploadOrderPrefix(QDateTime::fromSecsSinceEpoch(e)) == want);
    check("jstTime 은 같은 순간 · 일본 시간 표시", Common::jstTime(QDateTime::fromSecsSinceEpoch(e)).toSecsSinceEpoch() == e
          && Common::jstTime(QDateTime::fromSecsSinceEpoch(e)).toString("yyyy-MM-dd HH:mm:ss") == "2026-01-02 08:30:00");
    const QDateTime utcNow = QDateTime::currentDateTimeUtc();
    check("jstToday = UTC + 9시간의 날짜", Common::jstToday() == utcNow.addSecs(9 * 3600).date());
}

static void testCaptureName(const QString &root)
{
    // 2026-01-01 23:30 UTC = 2026-01-02 08:30 JST = 2026-01-01 18:30 뉴욕(이 시험의 맥 시간대)
    const QString dir = root + "/captures";
    QDir().mkpath(dir);
    const auto touch = [&](const QString &name) { QFile f(dir + "/" + name + ".html"); if (f.open(QIODevice::WriteOnly)) f.write("x"); };
    const QDateTime tw = Common::parseISODate("Thu Jan 01 23:30:00 +0000 2026");
    const QDateTime px = QDateTime::fromString("2026-01-01T23:30:00+00:00", Qt::ISODate);   // 픽시브의 실제 값(+00:00)
    const QDateTime jst = QDateTime::fromString("2026-01-02T08:30:00+09:00", Qt::ISODate);
    const QDateTime localDt = QDateTime::fromSecsSinceEpoch(1767310200);
    check("캡처 이름 — 처음 받는 글은 일본 시간", FileHelper::captureNameCompat(dir, px, "a1") == "20260102_083000_a1");
    touch("20260101_233000_b2");
    check("캡처 이름 — 픽시브의 예전 UTC 이름을 그대로 쓴다", FileHelper::captureNameCompat(dir, px, "b2") == "20260101_233000_b2");
    check("캡처 이름 — 트위터의 예전 UTC 이름을 그대로 쓴다", FileHelper::captureNameCompat(dir, tw, "b2") == "20260101_233000_b2");
    check("캡처 이름 — 값이 +09:00 이어도 예전 UTC 이름을 찾는다", FileHelper::captureNameCompat(dir, jst, "b2") == "20260101_233000_b2");
    touch("20260101_183000_c3");
    check("캡처 이름 — 맥의 현지 시각으로 적힌 예전 이름도 찾는다", FileHelper::captureNameCompat(dir, localDt, "c3") == "20260101_183000_c3");
    touch("20260102_083000_d4");
    touch("20260101_233000_d4");
    check("캡처 이름 — 새 이름이 이미 있으면 새 이름", FileHelper::captureNameCompat(dir, px, "d4") == "20260102_083000_d4");
    check("캡처 이름 — 시각을 모르면 id 만", FileHelper::captureNameCompat(dir, QDateTime(), "e5") == "e5");
}

static void testJstText()
{
    // 2026-01-01 23:30 UTC = 2026-01-02 08:30 JST
    check("jstText — 디스코드 시각(+00:00 · 마이크로초)", Common::jstText("2026-01-01T23:30:00.123456+00:00", "yyyy-MM-dd HH:mm:ss") == "2026-01-02 08:30:00",
          Common::jstText("2026-01-01T23:30:00.123456+00:00", "yyyy-MM-dd HH:mm:ss"));
    check("jstText — 페이지의 시각(Z)", Common::jstText("2026-01-01T23:30:00.000Z", "yyyy/MM/dd HH:mm") == "2026/01/02 08:30");
    check("jstText — 비면 빈 그대로", Common::jstText("", "yyyy") == "");
    check("jstText — 못 읽으면 받은 그대로", Common::jstText("어제", "yyyy") == "어제");
}

static void testPickPython()
{
    const auto asset = [](const QString &ver, const QString &arch, const QString &kind) {
        return QString("{\"browser_download_url\": \"https://github.com/astral-sh/python-build-standalone/releases/download/20261101/cpython-%1+20261101-%2-%3.tar.gz\"}")
            .arg(ver, arch, kind);
    };
    const QString arm = "aarch64-apple-darwin";
    const QString json = "[" + QStringList{
        asset("3.14.2", arm, "install_only"), asset("3.14.5", arm, "install_only"), asset("3.15.1", arm, "install_only"),
        asset("3.14.6", "x86_64-apple-darwin", "install_only"), asset("3.14.7", arm, "freethreaded-install_only"),
        asset("3.14.8b1", arm, "install_only"), asset("3.14.9", arm, "install_only_stripped")}.join(",") + "]";
    QString url, ver;
    const bool ok = Common::pickPythonRelease(json, arm, 3, 14, &url, &ver);
    check("파이썬 고르기 — 같은 minor 의 가장 높은 안정판(다음 minor · 다른 arch · freethreaded · 베타 · stripped 는 빼고)",
          ok && ver == "3.14.5" && url.contains("cpython-3.14.5+"), ver + " " + url);
    QString url2, ver2;
    check("파이썬 고르기 — 그 minor 가 없으면 고르지 않는다", !Common::pickPythonRelease(json, arm, 3, 13, &url2, &ver2) && url2.isEmpty());
}

static void testOverrides(const QString &root)
{
    const QString dir = root + "/ov";
    QDir().mkpath(dir);
    const auto put = [](const QString &p, const QByteArray &b) { QFile f(p); if (f.open(QIODevice::WriteOnly)) f.write(b); };
    const QString bundled = dir + "/bundled.py", ov = dir + "/over.py";
    put(bundled, "print('원본')\n");
    put(ov, "print('고친 것')\n");
    put(ov + ".base", Common::fileSha256Hex(bundled).toLatin1());
    check("자가수리 사본 — 고칠 때의 원본 그대로면 쓴다", Common::scriptOverrideStillValid(ov, bundled));
    put(bundled, "print('새 판의 원본')\n");
    check("자가수리 사본 — 원본이 바뀌었으면(새 판) 쓰지 않는다", !Common::scriptOverrideStillValid(ov, bundled));
    QFile::remove(ov + ".base");
    const QDateTime old = QDateTime::currentDateTime().addDays(-3);
    { QFile f(ov); if (f.open(QIODevice::ReadWrite)) f.setFileTime(old, QFileDevice::FileModificationTime); }
    check("자가수리 사본 — 표시 없는 옛 사본이 원본보다 오래면 쓰지 않는다", !Common::scriptOverrideStillValid(ov, bundled));
    { QFile f(ov); if (f.open(QIODevice::ReadWrite)) f.setFileTime(QDateTime::currentDateTime().addSecs(60), QFileDevice::FileModificationTime); }
    check("자가수리 사본 — 표시 없는 사본이 원본보다 나중이면 쓴다", Common::scriptOverrideStillValid(ov, bundled));
    check("판 번호 — 번들이 아니면 VERSION 값", Common::appBuildVersion() == QStringLiteral(PREDORMITION_VERSION_STR), Common::appBuildVersion());
}

static QByteArray readAllOf(const QString &path)
{
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

static bool writeAllTo(const QString &path, const QByteArray &b)
{
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(b) == b.size();
}

static QStringList partLeftovers(const QString &dir)
{
    return QDir(dir).entryList({QStringLiteral(".xlsx_*.part")}, QDir::Files | QDir::Hidden);
}

static void testExcelSave(const QString &root)
{
    QStringList hooked;
    ExcelWriter::setSaveFailureHook([&](const QString &p, const QString &) { hooked << QFileInfo(p).fileName(); });
    ExcelWriter w;
    w.writeHeader({"id", "text"});
    w.writeRow(2, {"1", "가나다"});
    const QString dir = root + "/xl";
    QDir().mkpath(dir);
    const QString ok = dir + "/keep.xlsx";
    const bool saved = w.save(ok);
    const QByteArray bytes = readAllOf(ok);
    check("엑셀 저장 — 정상이면 끝까지 쓴 zip(EOCD)", saved && ExcelWriter::xlsxWrittenToEnd(ok) && bytes.right(22).startsWith(QByteArrayLiteral("PK\x05\x06")));

    // 끝 검사 자체 — 잘린 것 · 끝에 덧붙은 것 · 표시 글자만 흉내 낸 것은 '다 쓰지 않았다', 주석이 달린 zip 은 '다 썼다'
    {
        const QString t = dir + "/probe.xlsx";
        writeAllTo(t, bytes.left(bytes.size() - 1));
        const bool cut1 = ExcelWriter::xlsxWrittenToEnd(t);
        writeAllTo(t, bytes.left(bytes.size() / 2));
        const bool cutHalf = ExcelWriter::xlsxWrittenToEnd(t);
        writeAllTo(t, bytes + QByteArray(7, 'x'));
        const bool junk = ExcelWriter::xlsxWrittenToEnd(t);
        writeAllTo(t, bytes.left(bytes.size() / 2) + QByteArrayLiteral("PK\x05\x06") + QByteArray(18, '\0'));
        const bool fakeMark = ExcelWriter::xlsxWrittenToEnd(t);
        QByteArray withComment = bytes;
        withComment[withComment.size() - 2] = char(5);   // 주석 길이 5(작은 끝 쪽 먼저)
        withComment[withComment.size() - 1] = char(0);
        writeAllTo(t, withComment + QByteArrayLiteral("hello"));
        const bool commented = ExcelWriter::xlsxWrittenToEnd(t);
        QFile::remove(t);
        check("엑셀 끝 검사 — 한 바이트 · 반이 잘린 것 · 끝에 덧붙은 것 · 표시만 흉내 낸 것은 거절, 주석 달린 zip 은 받음",
              !cut1 && !cutHalf && !junk && !fakeMark && commented,
              QString("cut1=%1 half=%2 junk=%3 fake=%4 comment=%5").arg(int(cut1)).arg(int(cutHalf)).arg(int(junk)).arg(int(fakeMark)).arg(int(commented)));
    }

    // QXlsx 가 '다 썼다' 고 했는데 파일이 잘린 때(디스크가 참) — 바꾸지 않고 알린다. 그 갈래(EOCD)를 직접 탄다.
    {
        hooked.clear();
        ExcelWriter::setAfterWriteHookForTest([](const QString &tmp) {
            QFile f(tmp);
            if (f.open(QIODevice::ReadWrite)) f.resize(f.size() / 2);
        });
        w.writeRow(3, {"2", "라마바"});
        const bool savedCut = w.save(ok);
        ExcelWriter::setAfterWriteHookForTest({});
        check("엑셀 저장 — 다 썼다는데 잘린 파일이면 바꾸지 않고 알린다(원본 그대로 · 조각 없음)",
              !savedCut && readAllOf(ok) == bytes && hooked.contains("keep.xlsx") && partLeftovers(dir).isEmpty(),
              hooked.join(",") + " | " + partLeftovers(dir).join(","));
    }

    // 진짜로 잘려 쓰이게 — 이 프로세스가 쓸 수 있는 파일 크기를 16KB 로 줄인다(RLIMIT_FSIZE · 디스크가 찬 것과 같다).
    //   QXlsx 는 쓰기 실패를 알리지 않으므로 검사가 없으면 잘린 파일이 원본을 덮는다.
    {
        hooked.clear();
        const QString full = dir + "/full.xlsx";
        const bool smallOk = w.save(full);
        const QByteArray before = readAllOf(full);
        ExcelWriter big;
        big.writeHeader({"id", "text"});
        for (int r = 2; r < 3000; ++r) {
            const QByteArray h = QCryptographicHash::hash(QByteArray::number(r), QCryptographicHash::Sha256).toHex();
            big.writeRow(r, {QString::number(r), QString::fromLatin1(h + h.toUpper())});
        }
        struct rlimit old {};
        getrlimit(RLIMIT_FSIZE, &old);
        struct rlimit lim = old;
        lim.rlim_cur = 16 * 1024;
        auto prevSig = std::signal(SIGXFSZ, SIG_IGN);
        const bool limited = setrlimit(RLIMIT_FSIZE, &lim) == 0;
        const bool savedBig = big.save(full);
        setrlimit(RLIMIT_FSIZE, &old);
        std::signal(SIGXFSZ, prevSig);
        check("엑셀 저장 — 디스크가 찬 것처럼 잘려 쓰이면 원본을 덮지 않고 알린다",
              smallOk && limited && !savedBig && readAllOf(full) == before && ExcelWriter::xlsxWrittenToEnd(full)
                  && hooked.contains("full.xlsx") && partLeftovers(dir).isEmpty(),
              QString("limited=%1 saved=%2 hooked=%3").arg(int(limited)).arg(int(savedBig)).arg(hooked.join(",")));
        const bool savedAfter = big.save(full);
        check("엑셀 저장 — 공간이 돌아오면 그 큰 문서도 저장된다", savedAfter && ExcelWriter::xlsxWrittenToEnd(full) && readAllOf(full).size() > 16 * 1024);
    }

    // 쓸 수 없는 폴더 — 실패를 알리고(훅) 원래 파일은 그대로
    hooked.clear();
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
    w.writeRow(4, {"3", "사아자"});
    const bool saved2 = w.save(ok);
    const QByteArray after = readAllOf(ok);
    QFile::setPermissions(dir, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
    check("엑셀 저장 — 못 쓰면 실패를 알린다(기록에 남게)", !saved2 && hooked.contains("keep.xlsx"), hooked.join(","));
    check("엑셀 저장 — 실패해도 원래 파일은 그대로", after == bytes);
    ExcelWriter::setSaveFailureHook({});
}

// 새 판으로 바꿀 때 받지 못한 꾸러미만 전의 덧씌우기에서 옮겨 온다 — RECORD 에 적힌 파일만 · 폴더 밖은 안 됨 · 있는 파일(새로 받은 의존)은 덮지 않음
static void testCarryOver(const QString &root)
{
    const QString prev = root + "/co/pypkgs.prev", ov = root + "/co/pypkgs";
    QDir().mkpath(prev + "/fake_pkg-1.0.dist-info");
    QDir().mkpath(prev + "/fake_pkg/sub");
    QDir().mkpath(ov);
    writeAllTo(prev + "/fake_pkg/__init__.py", "X = 1\n");
    writeAllTo(prev + "/fake_pkg/sub/m.py", "Y = 2\n");
    writeAllTo(prev + "/fake_pkg-1.0.dist-info/METADATA", "Metadata-Version: 2.1\nName: fake-pkg\nVersion: 1.0\n");
    writeAllTo(prev + "/shared_dep.py", "OLD\n");
    writeAllTo(ov + "/shared_dep.py", "NEW\n");                 // 새로 받은 의존 — 덮지 않아야
    writeAllTo(root + "/co/outside.txt", "keep\n");             // 폴더 밖 — 건드리지 않아야
    writeAllTo(prev + "/fake_pkg-1.0.dist-info/RECORD",
               "fake_pkg/__init__.py,sha256=x,6\nfake_pkg/sub/m.py,,\nfake_pkg-1.0.dist-info/METADATA,,\n"
               "fake_pkg-1.0.dist-info/RECORD,,\nshared_dep.py,,\n../outside.txt,,\nfake_pkg/../../outside.txt,,\n/etc/hosts,,\n");
    const QString py = QStringLiteral("/usr/bin/python3");
    const QString r1 = SelfRepair::carryOverOverlayPackage(py, prev, ov, QStringLiteral("fake-pkg"));
    const bool copied = readAllOf(ov + "/fake_pkg/__init__.py") == "X = 1\n" && readAllOf(ov + "/fake_pkg/sub/m.py") == "Y = 2\n"
                        && QFile::exists(ov + "/fake_pkg-1.0.dist-info/RECORD");
    const bool keptNew = readAllOf(ov + "/shared_dep.py") == "NEW\n";
    const bool stayedIn = readAllOf(root + "/co/outside.txt") == "keep\n" && !QFile::exists(ov + "/outside.txt")
                          && !QDir(ov + "/etc").exists();
    const QString r2 = SelfRepair::carryOverOverlayPackage(py, prev, ov, QStringLiteral("Fake_Pkg"));   // 이름 모양이 달라도 같은 꾸러미 — 이미 있다
    const QString r3 = SelfRepair::carryOverOverlayPackage(py, prev, ov, QStringLiteral("not-there"));
    check("모듈 옮겨 오기 — RECORD 의 파일만 옮기고, 새로 받은 의존은 덮지 않고, 폴더 밖은 건드리지 않는다",
          r1 == QLatin1String("OK") && copied && keptNew && stayedIn,
          QString("r1=%1 copied=%2 keptNew=%3 stayedIn=%4").arg(r1).arg(int(copied)).arg(int(keptNew)).arg(int(stayedIn)));
    check("모듈 옮겨 오기 — 이미 있으면 HAVE · 전에도 없던 것은 MISS", r2 == QLatin1String("HAVE") && r3 == QLatin1String("MISS"),
          QString("r2=%1 r3=%2").arg(r2, r3));
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
    qputenv("PYTHONDONTWRITEBYTECODE", "1");   // 앱과 같다(main.cpp) — 시험이 부르는 파이썬도 .pyc 를 남기지 않게
    QCoreApplication app(argc, argv);
    QTemporaryDir td;
    testWalkMarks(td.path());
    testNames(td.path());
    testIntegrity(td.path());
    testFileTimes(td.path());
    testJst();
    testCaptureName(td.path());
    testJstText();
    testPickPython();
    testOverrides(td.path());
    testExcelSave(td.path());
    testCarryOver(td.path());
    testUa();
    std::printf("%s\n", g_fail ? "실패" : "통과");
    return g_fail ? 1 : 0;
}
