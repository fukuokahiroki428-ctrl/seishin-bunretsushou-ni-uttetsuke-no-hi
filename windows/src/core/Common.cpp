#include "Common.h"
#include <atomic>
#include <QReadWriteLock>
#ifndef Q_OS_WIN
#include <csignal>
#include <sys/types.h>
#endif
#include <QHash>
#include <QLockFile>
#include <QAtomicInt>
#include <QProcess>
#include <QUrl>
#include <QCryptographicHash>
#include <QProcessEnvironment>
#include <QTimeZone>
#include <QLocale>
#include <QFile>
#include <QDir>
#include <QSaveFile>
#include <QDirIterator>
#include <QJsonObject>
#include <QJsonDocument>
#include <QMutex>
#include <QDateTime>
#include <QStorageInfo>
#include <QDebug>
#include <QCoreApplication>
#include <QFileInfo>
#include <QStandardPaths>
#include <QTimer>
#include <QThread>
#include <QMutex>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonArray>
#include <QImage>
#include <QImageReader>
#include <QSet>
#include <QVarLengthArray>
#include <QTemporaryFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QStringConverter>

#ifdef Q_OS_WIN
#include <sys/types.h>
#include <sys/utime.h>
#include <windows.h>
#else
#include <sys/stat.h>
#include <utime.h>
#endif

namespace Common {

static const QStringList weekdays = {
    "月曜日", "火曜日", "水曜日", "木曜日", "金曜日", "土曜日", "日曜日"
};

// English locale for parsing Twitter/Bluesky date strings (Mon, Jan, etc.)
static QLocale enLocale(QLocale::English, QLocale::UnitedStates);

QString formatDateJapanese(const QDateTime &dt)
{
    if (!dt.isValid()) return QString();

    QDateTime dtLocal = dt.toUTC().addSecs(9 * 3600);  // UTC+9 (JST)

    int dow = dtLocal.date().dayOfWeek() - 1; // Qt: 1=Mon..7=Sun
    if (dow < 0 || dow > 6) dow = 0;

    return QString("%1年%2月%3日 %4 %5")
        .arg(dtLocal.date().year())
        .arg(dtLocal.date().month())
        .arg(dtLocal.date().day())
        .arg(weekdays[dow])
        .arg(dtLocal.time().toString("HH:mm:ss"));
}

QString formatDateJapanese(const QString &dateStr)
{
    QDateTime dt = parseISODate(dateStr);
    if (!dt.isValid()) return dateStr;
    return formatDateJapanese(dt);
}

// Month name lookup for manual Twitter date parsing
static const QMap<QString, int> monthMap = {
    {"Jan",1},{"Feb",2},{"Mar",3},{"Apr",4},{"May",5},{"Jun",6},
    {"Jul",7},{"Aug",8},{"Sep",9},{"Oct",10},{"Nov",11},{"Dec",12}
};

QDateTime parseISODate(const QString &dateStr)
{
    // ★ 빈 문자열은 정상이다 — 프로필 사진·배너처럼 원래 날짜가 없는 것들이 있다
    //   (addExifMetadata 호출부에서 "" 를 넘긴다). 그런데도 경고를 찍는 바람에
    //   이 기계 로그에 8714 줄이 쌓여 734KB 가 됐고, 진짜 문제(exiftool 실패)가
    //   그 속에 묻혀 있었다. 조용히 무효 날짜를 돌려준다.
    if (dateStr.isEmpty()) return QDateTime();

    // Try various formats
    QDateTime dt;

    // ISO 8601 (Discord, Bluesky, etc.)
    dt = QDateTime::fromString(dateStr, Qt::ISODateWithMs);
    if (dt.isValid()) return dt;

    dt = QDateTime::fromString(dateStr, Qt::ISODate);
    if (dt.isValid()) return dt;

    // Twitter format: "Wed Oct 10 20:19:24 +0000 2018"
    // Manual parsing — QLocale::toDateTime can be unreliable across Qt versions/locales
    {
        QStringList parts = dateStr.split(' ', Qt::SkipEmptyParts);
        // Expected: [Wed, Oct, 10, 20:19:24, +0000, 2018]
        if (parts.size() == 6 && parts[4].startsWith('+') && monthMap.contains(parts[1])) {
            int month = monthMap[parts[1]];
            int day = parts[2].toInt();
            int year = parts[5].toInt();
            QStringList timeParts = parts[3].split(':');
            if (timeParts.size() == 3 && year > 2000 && day > 0 && day <= 31) {
                int hour = timeParts[0].toInt();
                int min = timeParts[1].toInt();
                int sec = timeParts[2].toInt();
                QDate d(year, month, day);
                QTime t(hour, min, sec);
                if (d.isValid() && t.isValid()) {
                    dt = QDateTime(d, t, QTimeZone::utc());
                    return dt;
                }
            }
        }
    }

    // Fallback: QLocale-based parsing
    dt = enLocale.toDateTime(dateStr, "ddd MMM dd HH:mm:ss +0000 yyyy");
    if (dt.isValid()) {
        dt.setTimeZone(QTimeZone::utc());
        return dt;
    }

    dt = enLocale.toDateTime(dateStr, "ddd MMM  d HH:mm:ss +0000 yyyy");
    if (dt.isValid()) {
        dt.setTimeZone(QTimeZone::utc());
        return dt;
    }

    // "yyyy-MM-dd HH:mm:ss"
    dt = QDateTime::fromString(dateStr, "yyyy-MM-dd HH:mm:ss");
    if (dt.isValid()) return dt;

    // "yyyy/MM/dd HH:mm"
    dt = QDateTime::fromString(dateStr, "yyyy/MM/dd HH:mm");
    if (dt.isValid()) return dt;

    qWarning() << "[Common] parseISODate failed for:" << dateStr;
    return QDateTime();
}

void setFileTimes(const QString &filePath, const QDateTime &timestamp)
{
    if (!timestamp.isValid()) {
        qWarning() << "[setFileTimes] Invalid timestamp for:" << filePath;
        return;
    }

    // Use JST for file timestamps displayed in Finder/Explorer
    QDateTime dtLocal = timestamp.toUTC().addSecs(9 * 3600);  // UTC+9 (JST)

    // utime uses UTC epoch
    qint64 epoch = timestamp.toSecsSinceEpoch();

#ifdef Q_OS_WIN
    // Windows: _wutime for Unicode path support
    struct _utimbuf times;
    times.actime = static_cast<time_t>(epoch);
    times.modtime = static_cast<time_t>(epoch);
    // ★ 260자 넘는 경로 — CRT(_wutime)도 원시 CreateFileW 도 접두어 없이는 못 연다.
    //   실측(302자 경로): 접두어를 붙이면 둘 다 통과하고, 없으면 CreateFileW 가
    //   오류 3(ERROR_PATH_NOT_FOUND)으로 죽는다.
    //   이 함수는 받은 파일마다 불린다. 여기서 조용히 실패하면 깊은 폴더의 파일만
    //   시각이 안 박혀서 맥에서 받은 것과 달라진다 — 동일화가 거기서 깨진다.
    const std::wstring wpath = longPathArg(filePath).toStdWString();
    int ret = _wutime(wpath.c_str(), &times);
    if (ret != 0) {
        qWarning() << "[setFileTimes] _wutime failed for:" << filePath;
    }

    // Also set creation time via SetFileTime API
    HANDLE hFile = CreateFileW(wpath.c_str(),
        FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        FILETIME ft;
        ULARGE_INTEGER uli;
        // Convert Unix epoch to Windows FILETIME (100ns intervals since 1601-01-01)
        uli.QuadPart = (epoch + 11644473600LL) * 10000000LL;
        ft.dwLowDateTime = uli.LowPart;
        ft.dwHighDateTime = uli.HighPart;
        SetFileTime(hFile, &ft, nullptr, &ft);  // creation + modification
        CloseHandle(hFile);
    }
#else
    // POSIX: utime
    struct utimbuf times;
    times.actime = epoch;
    times.modtime = epoch;
    int ret = utime(filePath.toUtf8().constData(), &times);
    if (ret != 0) {
        qWarning() << "[setFileTimes] utime failed for:" << filePath;
    }

    // macOS: SetFile for creation date
#ifdef Q_OS_MACOS
    QString dateStr = dtLocal.toString("MM/dd/yyyy HH:mm:ss");
    QProcess proc;
    proc.start("SetFile", {"-d", dateStr, filePath});
    proc.waitForFinished(5000);
#endif
#endif  // Q_OS_WIN
}

void setFileTimes(const QString &filePath, const QString &timestampStr)
{
    QDateTime dt = parseISODate(timestampStr);
    if (dt.isValid()) {
        setFileTimes(filePath, dt);
    }
}

// ═════════════════════════════════════════════════════════════════════════
// ANSI 로 안전한 경로
// ═════════════════════════════════════════════════════════════════════════
// exiftool.exe 는 PAR 로 묶인 perl 실행 파일이라 argv 를 시스템 ANSI 코드페이지로 받는다.
// 경로에 그 코드페이지로 표현할 수 없는 문자가 있으면 '?' 로 뭉개져, 자기 옆의
// exiftool_files\perl5*.dll 도 대상 파일도 찾지 못하고 종료 코드 1 로 죽는다.
//
//   실측 (ACP 1252 인 Windows 11):
//     D:\임시\Predormition\exiftool.exe -ver
//     → Could not find D:\??\Predormition\exiftool_files\perl5*.dll  (exit 1)
//
// 설치 기본 경로가 {localappdata}\Programs\Predormition 이므로 Windows 사용자 이름이
// 한글·일본어면 일반 사용자 환경에서 그대로 재현된다. EXIF 가 조용히 안 써진다.
//
// 8.3 단축 경로는 항상 ASCII 이므로 이를 우회로 쓴다.
#ifdef Q_OS_WIN
static bool ansiRepresentable(const QString &p)
{
    if (GetACP() == CP_UTF8) return true;   // ACP 가 UTF-8 이면 뭉개질 일이 없다
    QVarLengthArray<wchar_t, 512> w(p.length() + 1);
    const int len = p.toWCharArray(w.data());
    w[len] = L'\0';
    const int need = WideCharToMultiByte(CP_ACP, 0, w.data(), -1, nullptr, 0, nullptr, nullptr);
    if (need <= 0) return false;
    QVarLengthArray<char, 1024> buf(need);
    BOOL usedDefault = FALSE;
    const int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, w.data(), -1,
                                      buf.data(), need, nullptr, &usedDefault);
    return n > 0 && !usedDefault;   // 대체 문자가 쓰였다면 표현 불가
}
#endif


#ifdef Q_OS_WIN
// copyTreePreserving 은 아래(파이썬 환경 씨뿌리기 쪽)에 있다. 여기서 먼저 쓰므로 전방 선언한다.
static bool copyTreePreserving(const QString &src, const QString &dst);

// ★ exiftool.exe 를 ANSI 로 표현 가능한 자리로 옮겨 놓고 그 사본을 쓴다.
//
//   exiftool.exe 는 PAR 로 묶인 Perl 프로그램이라, 자기 자신의 경로를 ANSI 로 읽어서
//   그 옆의 exiftool_files\perl5*.dll 을 찾는다. 앱이 한글·일본어가 든 경로에 있으면
//   그 경로가 '?' 로 뭉개져 DLL 을 못 찾고 즉시 죽는다.
//     실측: "Could not find D:\? ?? (2)\...\exiftool_files\perl5*.dll"
//   원래는 8.3 단축 경로로 우회했는데, 이 기계의 D: 는 8.3 생성이 꺼져 있어서
//   (dir /x 로 확인 — 단축 이름 칸이 비어 있다) 우회가 아예 불가능했다.
//
//   그래서 ANSI 로 표현 가능한 임시 폴더에 exiftool.exe 와 exiftool_files 를 한 번 복사해
//   두고 그쪽을 실행한다. 복사는 처음 한 번만 한다.
//   실패하면 원본 경로를 그대로 돌려준다 — 그때는 지금까지처럼 경고만 남는다.
QString asciiSafeExiftool(const QString &exePath)
{
    if (exePath.isEmpty() || ansiRepresentable(exePath)) return exePath;

    static QString cached;
    static bool tried = false;
    if (tried) return cached.isEmpty() ? exePath : cached;
    tried = true;

    // ANSI 로 표현 가능한 후보를 순서대로 — 쓸 수 있는 첫 자리를 쓴다.
    QStringList bases;
    bases << QDir::tempPath()
          << QString::fromLatin1(qgetenv("SystemRoot")) + "/Temp"
          << QString::fromLatin1(qgetenv("ProgramData")) + "/Predormition";
    for (const QString &base : bases) {
        if (base.isEmpty() || !ansiRepresentable(base)) continue;
        const QString dir = base + "/predormition_exiftool";
        if (!QDir().mkpath(dir)) continue;

        const QString dstExe = dir + "/exiftool.exe";
        const QFileInfo srcInfo(exePath);
        // 이미 같은 크기로 복사돼 있으면 다시 복사하지 않는다.
        if (QFileInfo(dstExe).size() != srcInfo.size()) {
            QFile::remove(dstExe);
            if (!QFile::copy(exePath, dstExe)) continue;
        }
        // exiftool_files 는 exe 옆에 있어야 한다. 없으면 exe 만으로는 못 돈다.
        const QString srcFiles = srcInfo.absolutePath() + "/exiftool_files";
        if (QDir(srcFiles).exists()) {
            const QString dstFiles = dir + "/exiftool_files";
            if (!QDir(dstFiles).exists() && !copyTreePreserving(srcFiles, dstFiles)) {
                qWarning() << "[Common] exiftool_files 복사 실패:" << srcFiles << "->" << dstFiles;
                continue;
            }
        }
        qInfo() << "[Common] 경로에 ANSI 로 못 쓰는 글자가 있어 exiftool 을 옮겨 씁니다:" << dstExe;
        cached = dstExe;
        return cached;
    }
    qWarning() << "[Common] exiftool 을 ANSI 안전한 자리로 옮기지 못했습니다 — EXIF 가 실패할 수 있습니다.";
    return exePath;
}
#endif

QString ansiSafePath(const QString &path, const QString &who)
{
#ifdef Q_OS_WIN
    if (path.isEmpty() || ansiRepresentable(path)) return path;

    const QString native = QDir::toNativeSeparators(path);
    QVarLengthArray<wchar_t, 512> w(native.length() + 1);
    const int len = native.toWCharArray(w.data());
    w[len] = L'\0';

    const DWORD need = GetShortPathNameW(w.data(), nullptr, 0);
    if (need == 0) return path;
    QVarLengthArray<wchar_t, 512> shortBuf(static_cast<int>(need));
    if (GetShortPathNameW(w.data(), shortBuf.data(), need) == 0) return path;

    const QString shortPath = QString::fromWCharArray(shortBuf.data());
    // 볼륨에서 8.3 이 꺼져 있으면 원본이 그대로 돌아온다 — 그때는 우회가 불가능하다.
    if (!ansiRepresentable(shortPath)) {
        // ★ 여기서 무조건 exiftool 을 지목하면 안 된다. 이 함수는 ffmpeg·python·rclone
        //   경로에도 불린다. 그것들은 자기 경로를 넓은문자 API 로 받으므로 한글 경로에서도
        //   멀쩡히 돈다 — 이 기계에서 셋 다 실기능 확인을 통과했다.
        //   예전 문구는 그 셋에도 "exiftool 이 실패할 수 있습니다" 라고 찍혀서,
        //   나중에 로그를 보는 사람을 멀쩡한 자리로 보냈다. 겁주는 로그는 없느니만 못하다.
        const bool isExiftool = who.isEmpty() || who == QLatin1String("exiftool");
        if (isExiftool) {
            qWarning() << "[Common] 경로에 ANSI 로 못 쓰는 글자가 있는데 8.3 단축 경로도 없습니다."
                       << "exiftool 이 옆의 exiftool_files\\perl5*.dll 을 못 찾을 수 있습니다:" << path;
        } else {
            qDebug() << "[Common] 8.3 단축 경로가 없어 원본 경로를 그대로 넘깁니다 —"
                     << who << "가 넓은문자 API 를 쓰면 문제 없습니다:" << path;
        }
        return path;
    }
    return shortPath;
#else
    return path;
#endif
}

// exiftool 실행 방법 — 한 곳에서만 정한다(선언부 주석 참고).
namespace {
// exiftool 실행 방법을 한 번만 정해서 들고 있는다.
//   ★ 왜 구조체 하나로 묶어 함수 지역 static 으로 두나.
//     예전엔 static QString 넷을 두고 "비었으면 채운다" 로 썼다. 그런데 EXIF 는
//     여러 수집 스레드가 동시에 쓴다. 두 스레드가 같이 '비었다' 를 보면 둘 다
//     채우기에 들어가고, QString 대입은 원자적이지 않아 찢어진 값을 읽을 수 있다.
//     C++11 부터 함수 지역 static 의 '초기화' 는 한 번만·스레드 안전하게 일어난다.
//     그 보장을 받으려면 대입이 아니라 초기화여야 한다.
struct ExiftoolWay {
    QString program;        // 실제로 띄울 것 (perl 또는 exiftool)
    QStringList lead;       // 그 앞에 붙일 인자 (-I… + exiftool 스크립트 경로)
};

ExiftoolWay resolveExiftoolWay()
{
    ExiftoolWay w;
    const QString bundledExiftool = bundledResourcesDir() + "/tools/exiftool/exiftool";
    if (!QFile::exists(bundledExiftool)) {
        QStringList candidates = {
#ifdef Q_OS_WIN
            QCoreApplication::applicationDirPath() + "/exiftool.exe",
#endif
            "/opt/homebrew/bin/exiftool",
            "/usr/local/bin/exiftool",
            "/usr/bin/exiftool",
        };
        for (const QString &c : candidates)
            if (QFile::exists(c)) { w.program = c; break; }
        if (w.program.isEmpty()) w.program = QStringLiteral("exiftool");
        qInfo() << "[Common] system exiftool:" << w.program;
        return w;                       // lead 비어 있음 = perl 안 거침
    }

#ifdef Q_OS_WIN
    // 윈도우 번들은 exe 라 perl 을 거치지 않는다.
    w.program = bundledExiftool;
    return w;
#else
    // ★ 예전엔 exiftool 모듈 경로로 ".../lib/perl5" 를 넘겼는데 그런 폴더는 없다.
    //   실제 위치는 .../tools/exiftool/lib (그 밑에 Image/ · File/).
    //   exiftool 스크립트가 스스로 자기 lib 을 찾아 주는 덕에 티가 안 났을 뿐,
    //   우리가 준 -I 는 내내 허공을 가리키고 있었다.
    const QString exiftoolLib = bundledResourcesDir() + "/tools/exiftool/lib";

    // ★ 번들 perl 우선. Apple 은 /usr/bin/perl 을 언젠가 뺀다고 예고했다.
    //   그날 EXIF 기록이 통째로 죽지 않도록 perl 을 들고 다닌다.
    QString perl = bundledResourcesDir() + "/tools/perl/bin/perl";
    QStringList coreIncs;
    // ★ '파일이 있다' 와 '실제로 뜬다' 는 다르다. 서명이 깨졌거나 libperl 이 빠지면
    //   파일은 그대로 있는데 커널이 실행을 거부한다. 그때 시스템 perl 로 물러나지
    //   않으면 EXIF 가 통째로 죽는다 — 들고 다니는 것이 오히려 해가 된다.
    //   한 번만 물어본다(결과는 이 함수째로 캐시된다).
    auto perlRuns = [](const QString &p) {
        QProcess t;
        t.setProcessEnvironment(bundledProcessEnv());
        t.start(p, {"-e", "1"});
        if (!t.waitForStarted(3000)) return false;
        if (!t.waitForFinished(5000)) { t.kill(); return false; }
        return t.exitStatus() == QProcess::NormalExit && t.exitCode() == 0;
    };
    if (QFile::exists(perl) && !perlRuns(perl)) {
        qWarning() << "[Common] 번들 perl 이 실행되지 않습니다 — 시스템 perl 로 물러납니다:" << perl;
        perl.clear();
    }
    if (!perl.isEmpty() && QFile::exists(perl)) {
        const QString core = bundledResourcesDir() + "/tools/perl/lib";
        // 아키텍처 폴더 이름(darwin-thread-multi-2level)을 코드에 박지 않는다.
        // perl 판이 바뀌면 이름도 바뀐다 — 실제로 있는 것을 찾아 넣는다.
        for (const QString &d : QDir(core).entryList(QStringList() << "darwin*",
                                                     QDir::Dirs | QDir::NoDotAndDotDot))
            coreIncs << (core + "/" + d);
        coreIncs << core;
    } else {
        perl.clear();
        for (const QString &p : {QStringLiteral("/usr/bin/perl"),
                                 QStringLiteral("/usr/local/bin/perl"),
                                 QStringLiteral("/opt/homebrew/bin/perl")}) {
            if (QFile::exists(p)) { perl = p; break; }
        }
        if (perl.isEmpty()) perl = QStringLiteral("perl");
    }
    w.program = perl;
    for (const QString &inc : coreIncs) w.lead << ("-I" + inc);
    w.lead << ("-I" + exiftoolLib);
    w.lead << bundledExiftool;
    qInfo() << "[Common] exiftool:" << bundledExiftool << "perl:" << perl
            << (coreIncs.isEmpty() ? "(시스템 perl)" : "(번들 perl)");
    return w;
#endif
}
} // namespace

QString exiftoolProgram(QStringList *leadingArgs)
{
    static const ExiftoolWay way = resolveExiftoolWay();   // 1회·스레드 안전
    if (leadingArgs) *leadingArgs = way.lead;
    return way.program;
}

void addExifMetadata(const QString &imagePath, const QString &artist,
                     const QString &description, const QString &copyright,
                     const QString &comment, const QString &dateStr)
{
    // Process image + video files (exiftool supports both)
    QString lower = imagePath.toLower();
    bool isImage = lower.endsWith(".jpg") || lower.endsWith(".jpeg") || lower.endsWith(".png") ||
                   lower.endsWith(".tiff") || lower.endsWith(".webp");
    bool isVideo = lower.endsWith(".mp4") || lower.endsWith(".mov") || lower.endsWith(".webm") ||
                   lower.endsWith(".avi") || lower.endsWith(".mkv");
    if (!isImage && !isVideo) return;

    // Check exiftool availability (only once)
    static int exiftoolAvailable = -1;
    if (exiftoolAvailable == 0) return;

    // Parse date → JST EXIF format
    QDateTime dt = parseISODate(dateStr);
    QString exifDate;
    if (dt.isValid()) {
        dt = dt.toUTC().addSecs(9 * 3600);  // UTC+9 (JST)
        exifDate = dt.toString("yyyy:MM:dd HH:mm:ss");
    }

    QStringList args = {"-overwrite_original"};
    if (!artist.isEmpty())     args << ("-Artist=" + artist);
    if (!description.isEmpty()) args << ("-ImageDescription=" + description);
    if (!copyright.isEmpty())  args << ("-Copyright=" + copyright);
    if (!comment.isEmpty())    args << ("-UserComment=" + comment);
    if (!exifDate.isEmpty()) {
        if (isVideo) {
            // 비디오: CreateDate + MediaCreateDate
            args << ("-CreateDate=" + exifDate);
            args << ("-MediaCreateDate=" + exifDate);
            args << ("-ModifyDate=" + exifDate);
        } else {
            args << ("-DateTimeOriginal=" + exifDate);
        }
    }
    // ★ 대상 파일 경로는 반드시 원본(긴 경로)을 넘긴다. 8.3 단축 경로를 넘기면
    //   -overwrite_original 이 그 이름으로 되돌려 써서 사용자 파일이 8.3 이름으로
    //   개명된다 (실측: 테스트이미지.jpg → 75CF~1.JPG). 아카이빙 앱에서는 자료 손상이다.
    //   경로·값의 인코딩은 아래 UTF-8 argfile 로 해결한다.
    args << imagePath;

    QProcess proc;
    proc.setProcessEnvironment(bundledProcessEnv());
#ifdef Q_OS_WIN
    // ★ argfile 은 exiftool 이 다 읽을 때까지 살아 있어야 한다.
    //   전에는 아래 else 블록 안에서 만들어서, 블록이 끝나는 순간 소멸자가
    //   (setAutoRemove(true) 때문에) 파일을 지워 버렸다. proc.start() 는 비동기라
    //   exiftool 이 뜨기도 전에 파일이 사라져서 "Error opening arg file" 로 매번 실패했다.
    //   실측: 이 기계의 로그에 1668건 전부 그 오류. EXIF 가 하나도 안 쓰이고 있었다.
    //   여기서 만들면 함수가 끝날 때까지 — waitForFinished 이후까지 — 살아 있다.
    //   (임시 파일 이름은 맥과 같게 앱 이름에서 — APP_NAME_ASCII)
    QTemporaryFile argFile(QDir::tempPath() + "/" + QStringLiteral(APP_NAME_ASCII).toLower() + "_exif_XXXXXX.args");
    // ★ 줄바꿈이 든 값(트윗 본문 등)을 담을 파일들. argfile 과 같은 수명이어야 한다 —
    //   exiftool 이 다 읽기 전에 사라지면 안 된다(argfile 이 그래서 한 번 물렸다).
    QTemporaryDir valDir(QDir::tempPath() + "/" + QStringLiteral(APP_NAME_ASCII).toLower() + "_exifval_XXXXXX");
#endif
    QStringList lead;
    const QString prog = exiftoolProgram(&lead);
    if (!lead.isEmpty()) {
        // 번들 exiftool: perl -I<코어> -I<exiftool lib> exiftool <args>
        QStringList perlArgs = lead;
        perlArgs.append(args);
        proc.start(prog, perlArgs);
    } else {
#ifdef Q_OS_WIN
        // ★ Windows: 인자를 UTF-8 argfile 로 넘긴다.
        //   exiftool.exe 는 argv 를 시스템 ANSI 코드페이지로 받으므로, 그대로 넘기면
        //   일본어·한글 태그 값과 파일 경로가 '?' 로 뭉개진다.
        //   (실측: -Artist=일반사용자 → Artist : ?????)
        //   -@ 로 주면 exiftool 이 파일에서 직접 읽어 온전히 전달된다.
        //   실행 파일 경로만은 8.3 로 넘긴다 — exiftool.exe 는 자기 경로를 기준으로
        //   exiftool_files\perl5*.dll 을 찾으므로 여기가 뭉개지면 시작조차 못 한다.
        argFile.setAutoRemove(true);
        if (!argFile.open()) {
            qWarning() << "[Common] exiftool argfile 을 만들지 못했습니다 — EXIF 기록을 건너뜁니다";
            return;
        }
        {
            QTextStream ts(&argFile);
            ts.setEncoding(QStringConverter::Utf8);
            ts << "-charset\nfilename=UTF8\n-charset\nUTF8\n";

            // ★ argfile 은 '한 줄에 인자 하나' 다. 그런데 트윗 본문에는 줄바꿈이 흔하다.
            //   그대로 쓰면 두 번째 줄부터 exiftool 이 파일 이름으로 읽는다:
            //     Error: File not found - 두 번째 줄 https://t.co/...
            //   그리고 설명은 첫 줄만 남는다. 내각회 로그가 이 오류로 뒤덮여 있었다.
            //   (실측: 줄바꿈 하나당 오류 한 줄, 한 게시물에 여러 건)
            //
            //   해결: 값에 줄바꿈이 있으면 그 값을 파일에 담고 '-태그<=파일' 로 넘긴다.
            //   exiftool 이 파일 내용을 그대로 값으로 쓴다 — 줄바꿈까지 원문대로.
            //   (실측: 64바이트 원문과 되읽은 값이 바이트 단위로 완전히 동일)
            //   줄바꿈을 공백으로 바꿔 버리는 쪽이 더 쉽지만, 그러면 저장한 것이
            //   게시물 원문과 달라진다. 보관이 목적인 앱에서 그건 손실이다.
            int valIdx = 0;
            for (const QString &a : args) {
                const int eq = a.indexOf('=');
                const bool isTagValue = a.startsWith('-') && eq > 1;
                const QString val = isTagValue ? a.mid(eq + 1) : QString();
                if (isTagValue && (val.contains('\n') || val.contains('\r')) && valDir.isValid()) {
                    const QString vp = valDir.filePath(QString("v%1.txt").arg(valIdx++));
                    QFile vf(vp);
                    if (vf.open(QIODevice::WriteOnly)) {
                        vf.write(val.toUtf8());   // 그대로. 끝에 줄바꿈을 붙이지 않는다.
                        vf.close();
                        // 경로도 argfile 안에 있으므로 -charset filename=UTF8 이 적용된다.
                        ts << a.left(eq) << "<=" << vp << "\n";
                        continue;
                    }
                    qWarning() << "[Common] EXIF 값 파일을 못 만들어 한 줄로 줄입니다:" << vp;
                }
                ts << a << "\n";
            }
        }
        argFile.close();
        // argfile 자체는 exiftool 이 읽기만 하므로 8.3 로 넘겨도 안전하다.
        // 실행 파일은 asciiSafeExiftool — 경로에 ANSI 로 못 쓰는 글자가 있으면 옮겨 둔 사본을 띄운다
        // (8.3 단축 경로만으로는 8.3 이 꺼진 드라이브에서 exiftool_files 를 못 찾는다 — Common.h 설명).
        proc.start(asciiSafeExiftool(prog), {"-@", ansiSafePath(argFile.fileName())});
#else
        proc.start(prog, args);
#endif
    }
    if (!proc.waitForStarted(3000)) {
        if (exiftoolAvailable == -1) {
            qWarning() << "[Common] exiftool not found — EXIF metadata disabled"
                       << "prog:" << prog << "lead:" << lead.join(' ')
                       << "error:" << proc.errorString();
            exiftoolAvailable = 0;
        }
        return;
    }
    exiftoolAvailable = 1;
    proc.waitForFinished(10000);
    if (proc.exitCode() != 0) {
        qWarning() << "[Common] exiftool error:" << proc.readAllStandardError().trimmed();
    }
}

// ─── Cross-platform path helpers ───

// ── User-Agent ───────────────────────────────────────────────────────────
// 설치된 크로미움 계열 브라우저의 판을 읽는다.
//   윈도우 크롬/엣지는 설치 폴더 밑에 판 번호를 폴더 이름으로 둔다:
//     C:\Program Files\Google\Chrome\Application\151.0.7204.101\
//   그래서 실행 파일을 돌려 볼 필요 없이 폴더 이름만 읽으면 된다 — 빠르고,
//   백신이 실행을 가로막는 기계에서도 실패하지 않는다.
//   못 찾으면 exe 를 --version 으로 물어보고, 그것도 안 되면 보수적 기본값을 쓴다.
static QString detectChromeMajor()
{
    QStringList appDirs;
#ifdef Q_OS_WIN
    const QStringList roots = {
        QString::fromLocal8Bit(qgetenv("ProgramFiles")),
        QString::fromLocal8Bit(qgetenv("ProgramFiles(x86)")),
        QString::fromLocal8Bit(qgetenv("LOCALAPPDATA")),
    };
    for (const QString &r : roots) {
        if (r.isEmpty()) continue;
        appDirs << r + "/Google/Chrome/Application"
                << r + "/Google/Chrome Beta/Application"
                << r + "/Microsoft/Edge/Application"
                // ★ 요즘 엣지는 여기 깔린다. 예전 자리(Edge/Application)만 보면
                //   엣지만 깔린 기계에서 판을 못 읽는다 — 이 기계가 실제로 그렇다.
                //   (EdgeWebView 는 브라우저가 아니지만 같은 세대의 크로미움이다.)
                << r + "/Microsoft/EdgeCore"
                << r + "/Microsoft/EdgeWebView/Application"
                << r + "/BraveSoftware/Brave-Browser/Application";
    }
    // 앱이 들고 다니는 크로미움도 본다 — 사용자 브라우저가 아예 없을 수 있다.
    appDirs << QCoreApplication::applicationDirPath() + "/chromium";
#endif

    int best = 0;
    static const QRegularExpression verDir(QStringLiteral("^(\\d+)\\.\\d+\\.\\d+\\.\\d+$"));
    // 번들 크로미움은 판을 폴더가 아니라 "152.0.7977.42.manifest" 파일 이름으로 둔다.
    static const QRegularExpression verManifest(
        QStringLiteral("^(\\d+)\\.\\d+\\.\\d+\\.\\d+\\.manifest$"));
    for (const QString &d : appDirs) {
        QDir dir(d);
        if (!dir.exists()) continue;
        for (const QString &name : dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
            const auto m = verDir.match(name);
            if (!m.hasMatch()) continue;
            const int major = m.captured(1).toInt();
            if (major > best) best = major;
        }
        for (const QString &name : dir.entryList({QStringLiteral("*.manifest")}, QDir::Files)) {
            const auto m = verManifest.match(name);
            if (!m.hasMatch()) continue;
            const int major = m.captured(1).toInt();
            if (major > best) best = major;
        }
    }
    if (best >= 100) return QString::number(best);

    // 폴더 이름으로 못 찾았으면 실행 파일에 물어본다.
    QStringList exes;
#ifdef Q_OS_WIN
    for (const QString &d : appDirs) {
        for (const QString &n : {QStringLiteral("chrome.exe"), QStringLiteral("msedge.exe"),
                                 QStringLiteral("msedgewebview2.exe"), QStringLiteral("brave.exe")}) {
            const QString p = d + "/" + n;
            if (QFile::exists(p)) exes << p;
        }
    }
#endif
    for (const QString &exe : exes) {
        QProcess p;
        p.start(exe, {"--version"});
        if (!p.waitForStarted(3000)) continue;
        if (!p.waitForFinished(5000)) { p.kill(); continue; }
        // "Google Chrome 151.0.7204.101" / "Microsoft Edge 151.0.3..."
        const QString out = QString::fromUtf8(p.readAllStandardOutput()).trimmed();
        static const QRegularExpression verAny(QStringLiteral("(\\d+)\\.\\d+\\.\\d+\\.\\d+"));
        const auto m = verAny.match(out);
        if (!m.hasMatch()) continue;
        const int major = m.captured(1).toInt();
        if (major >= 100) return QString::number(major);
    }
    return QString();
}

QString browserUserAgent()
{
    static QString cached;
    if (!cached.isEmpty()) return cached;

    // 탈출구 — 특정 판이 필요하면 환경변수로 갈아끼운다.
    //   (기준 영상과 같은 이유다. 박아 둔 값은 언젠가 틀린다.)
    const QString forced = qEnvironmentVariable("PREDORMITION_USER_AGENT");
    if (!forced.isEmpty()) { cached = forced; return cached; }

    QString major = detectChromeMajor();
    if (major.isEmpty()) major = QStringLiteral("140");   // 아무것도 못 읽었을 때의 보수적 기본값

#ifdef Q_OS_WIN
    cached = QStringLiteral(
        "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/%1.0.0.0 Safari/537.36").arg(major);
#else
    // Chrome 은 macOS 판을 10_15_7 로 고정해 보낸다(UA 고정 정책). 그대로 맞춘다.
    cached = QStringLiteral(
        "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/537.36 "
        "(KHTML, like Gecko) Chrome/%1.0.0.0 Safari/537.36").arg(major);
#endif
    qInfo().noquote() << "[UA] 요청에 쓸 User-Agent — Chrome" << major;
    return cached;
}

// ★ 진짜 Chrome 은 UA 와 함께 늘 이 힌트들을 보낸다. 우리는 UA 만 흉내 내고 이것들을 빼먹었다.
//   그 상태로 인스타 웹 API 를 부르면 JSON 대신 21KB HTML + 429 가 온다 — 로그인·확인 화면이
//   아니라 '브라우저가 아닌 것' 으로 본 응답이다(맥 실측 2026-09-20).
//   판 번호는 UA 와 반드시 같아야 한다. 어긋나면 그 자체가 봇 표식이 된다.
//   플랫폼은 이 트리가 도는 곳을 적는다 — 맥은 "macOS", 여기는 "Windows".
//   UA 가 Windows 라고 말하는데 힌트가 macOS 라고 하면 그것도 어긋남이다.
QMap<QString, QString> browserClientHints()
{
    const QString ua = browserUserAgent();
    QString major = QStringLiteral("140");
    {
        static const QRegularExpression re(QStringLiteral("Chrome/(\\d+)\\."));
        const auto m = re.match(ua);
        if (m.hasMatch()) major = m.captured(1);
    }
    QMap<QString, QString> h;
    h["sec-ch-ua"] = QString("\"Chromium\";v=\"%1\", \"Google Chrome\";v=\"%1\", \"Not=A?Brand\";v=\"24\"").arg(major);
    h["sec-ch-ua-mobile"] = "?0";
    h["sec-ch-ua-platform"] = "\"Windows\"";
    h["Sec-Fetch-Site"] = "same-origin";
    h["Sec-Fetch-Mode"] = "cors";
    h["Sec-Fetch-Dest"] = "empty";
    h["Accept-Language"] = "ko-KR,ko;q=0.9,en-US;q=0.8,en;q=0.7";
    return h;
}

bool writeFileAtomic(const QString &path, const QByteArray &bytes, QString *err)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    // ★ write 의 반환값을 본다. 디스크가 꽉 차면 open 은 성공하고 write 만 모자라게
    //   쓰는데, 그것을 무시하면 반쪽짜리 설정이 조용히 남는다.
    if (f.write(bytes) != bytes.size()) {
        if (err) *err = QStringLiteral("쓴 크기가 모자랍니다(디스크 공간?)");
        f.cancelWriting();
        return false;
    }
    if (!f.commit()) {                 // 여기서야 실제 파일이 갈아 끼워진다
        if (err) *err = f.errorString();
        return false;
    }
    return true;
}

QString bundledResourcesDir()
{
    QString appDir = QCoreApplication::applicationDirPath();
#ifdef Q_OS_MACOS
    return appDir + "/../Resources";
#else
    // Windows/Linux: resources are next to the executable
    return appDir;
#endif
}

// ★ 유니버설 빌드: arch 슬라이스별로 #if 가 각각 컴파일되므로 런타임 arch 에 맞는 접미사가 박힌다.
//   (arm64 슬라이스 → _arm64, x86_64 슬라이스 → _x86_64). Intel/Apple Silicon 둘 다 자체 python 사용.
#if defined(Q_OS_MACOS) && defined(__aarch64__)
#  define KAMERA_PY_ARCH "_arm64"
#elif defined(Q_OS_MACOS)
#  define KAMERA_PY_ARCH "_x86_64"
#else
#  define KAMERA_PY_ARCH ""
#endif

QString userPythonEnvDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
           + "/python_env" KAMERA_PY_ARCH;
}

// ★ 이 두 헬퍼는 맥 전용 가드 안에 있었다. activePythonEnvDir 의 윈도우 갈래도 쓰게 되면서
//   윈도우까지 열었다 — 가드를 그대로 두면 error C3861(identifier not found)로 빌드가 깨진다.
#if defined(Q_OS_MACOS) || defined(Q_OS_WIN)
// 심볼릭 링크/실행권한 보존이 필요(standalone python 은 symlink 포함) → cp -a 로 복사.
static bool copyTreePreserving(const QString &src, const QString &dst)
{
#ifdef Q_OS_WIN
    // robocopy 는 윈도우에 기본 탑재라 별도 의존성이 없고, 깊은 경로와 많은 파일에 강하다.
    //   ★ 종료 코드가 특이하다 — 0~7 이 성공(1=복사함, 2=여분 있음, …), 8 이상이 실패다.
    //     0 이 아니면 실패로 보면 정상 복사를 실패로 오인한다.
    QProcess rc;
    rc.start("robocopy", {QDir::toNativeSeparators(src), QDir::toNativeSeparators(dst),
                          "/E", "/NFL", "/NDL", "/NJH", "/NJS", "/NC", "/NS", "/R:1", "/W:1"});
    if (!rc.waitForStarted(5000)) return false;
    if (!rc.waitForFinished(600000)) { rc.kill(); rc.waitForFinished(2000); return false; }
    return rc.exitStatus() == QProcess::NormalExit && rc.exitCode() < 8;
#else
    QProcess cp;
    cp.start("/bin/cp", {"-a", src, dst});
    if (!cp.waitForStarted(5000)) return false;
    if (!cp.waitForFinished(180000)) { cp.kill(); cp.waitForFinished(2000); return false; }
    return cp.exitStatus() == QProcess::NormalExit && cp.exitCode() == 0;
#endif
}

// 외부 python_env 가 '완전한지' 검증 — bin/python3 는 있는데 핵심 패키지(twikit)가 빠진
// 깨진/구 복사본(예: 옛 설치의 다른 파이썬 버전 · 패키지 0개)이면 번들에서 재시드해야 한다.
// python 실행 없이 site-packages/twikit 디렉토리 존재로 판단(빠름).
static bool pythonEnvHasCorePackages(const QString &envDir)
{
    // ★ 윈도우 배치는 유닉스와 다르다 — <env>\Lib\site-packages\twikit 로 python3.x 층이 없다.
    //   유닉스 모양(lib/python3*/site-packages)만 보던 탓에 윈도우에서는 멀쩡한 env 를 두고도
    //   언제나 false 를 돌려줬다. 지금은 맥 전용 경로에서만 불려서 드러나지 않았을 뿐이다.
    if (QFileInfo::exists(envDir + "/Lib/site-packages/twikit"))
        return true;
    QDir libDir(envDir + "/lib");
    const QStringList pyDirs = libDir.entryList(QStringList() << "python3*", QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &pd : pyDirs) {
        if (QFileInfo::exists(envDir + "/lib/" + pd + "/site-packages/twikit"))
            return true;
    }
    return false;
}
#endif

QString activePythonEnvDir()
{
#ifdef Q_OS_MACOS
    // 번들은 codesign 으로 sealed → 외부 복사본을 쓴다. 없으면 번들에서 1회 시드.
    static QMutex seedMutex;
    const QString ext = userPythonEnvDir();
    const QString extPy = ext + "/bin/python3";

    QString bundled = bundledResourcesDir() + "/python_env" KAMERA_PY_ARCH;
    // 호환: arch 별 디렉토리가 없으면 단일 python_env 로 폴백 (구 번들/단일 arch 빌드).
    if (!QFile::exists(bundled + "/bin/python3"))
        bundled = bundledResourcesDir() + "/python_env";
    const bool bundleReady = QFile::exists(bundled + "/bin/python3") && pythonEnvHasCorePackages(bundled);

    // ★★ 앱 내부(번들) 우선 — 모듈/라이브러리를 앱 안에 두고 앱 안에 설치한다.
    //   번들에 쓰면 codesign 봉인이 깨지지만, 설치·수리 직후 Common::resealAppBundle() 이
    //   자동 재서명해 복구하므로 안전하다(재서명 실패 시에는 아래 외부 복사본 경로로 자연 폴백).
    //   번들이 읽기전용(권한 없는 위치·검역 등)이면 예전처럼 외부 복사본을 쓴다.
    if (bundleReady && isDirWritable(bundled))
        return bundled;

    // ★ 외부본이 존재하되 핵심 패키지(twikit)까지 있어야 그대로 사용한다.
    //   bin/python3 만 있고 패키지가 빠진 깨진 복사본(옛 설치의 3.15b·패키지 0개 등)은
    //   재시드 대상 — 이게 'twikit not installed' 로 트위터 수집이 죽던 근본 원인이었다.
    //   (번들에 완전한 env 가 없으면 재시드해도 소용없으니 그때는 외부본을 그대로 둔다.)
    if (QFile::exists(extPy) && (pythonEnvHasCorePackages(ext) || !bundleReady))
        return ext;

    QMutexLocker lock(&seedMutex);
    if (QFile::exists(extPy) && (pythonEnvHasCorePackages(ext) || !bundleReady))
        return ext;   // 락 대기 중 다른 스레드가 끝냈을 수 있음
    if (!bundleReady)
        return ext;   // 번들에도 완전한 env 없음 → 외부 경로 반환(새 설치/복구 대상)

    QDir().mkpath(QFileInfo(ext).absolutePath());
    const QString tmp = ext + ".seeding";
    QDir(tmp).removeRecursively();
    if (copyTreePreserving(bundled, tmp) && QFile::exists(tmp + "/bin/python3")) {
        QDir(ext).removeRecursively();          // 깨진 기존 외부본이 있으면 교체
        if (QDir().rename(tmp, ext) && QFile::exists(extPy)) {
            qDebug() << "[Common] python_env seeded to writable location:" << ext;
            return ext;
        }
    }
    QDir(tmp).removeRecursively();
    qWarning() << "[Common] python_env seed failed — using read-only bundle (upgrade disabled)";
    return bundled;   // 복사 실패 → 읽기전용 번들 (upgrade/repair 가 거부함)
#elif defined(Q_OS_WIN)
    // ★ 앱 안(번들) 우선 — 기본 설치 위치는 {localappdata}\Programs\Predormition 이고
    //   PrivilegesRequired=lowest 라 앱 폴더가 쓰기 가능하다. 그때는 예전처럼 그대로 쓴다.
    //   문제는 그렇지 않은 경우다 — 포터블 zip 을 읽기전용 위치에 풀거나, 관리자가
    //   Program Files 에 넣었거나, 폴더 권한이 잠긴 경우. 예전 코드는 쓰기 가능 여부를
    //   보지도 않고 앱 안 경로를 돌려줘서, pip 설치·패키지 복구가 조용히 실패했다.
    //   맥은 진작 이 갈래를 갖고 있었다(거기선 codesign 봉인 때문). 같은 모양으로 맞춘다.
    static QMutex seedMutex;
    const QString ext     = userPythonEnvDir();
    const QString extPy   = ext + "/python.exe";
    const QString bundled = bundledResourcesDir() + "/python_env";
    const bool bundleReady = QFile::exists(bundled + "/python.exe") && pythonEnvHasCorePackages(bundled);

    if (bundleReady && isDirWritable(bundled))
        return bundled;

    // 외부본이 있으면 쓰되, 핵심 패키지(twikit)까지 있어야 한다 — 깨진 복사본은 재시드 대상.
    if (QFile::exists(extPy) && (pythonEnvHasCorePackages(ext) || !bundleReady))
        return ext;

    QMutexLocker lock(&seedMutex);
    if (QFile::exists(extPy) && (pythonEnvHasCorePackages(ext) || !bundleReady))
        return ext;          // 락 대기 중 다른 스레드가 끝냈을 수 있음
    if (!bundleReady)
        return ext;          // 번들에도 온전한 env 가 없음 → 복구 대상 경로를 돌려준다

    QDir().mkpath(QFileInfo(ext).absolutePath());
    const QString tmp = ext + ".seeding";
    QDir(tmp).removeRecursively();
    if (copyTreePreserving(bundled, tmp) && QFile::exists(tmp + "/python.exe")) {
        QDir(ext).removeRecursively();
        if (QDir().rename(tmp, ext) && QFile::exists(extPy)) {
            qDebug() << "[Common] python_env seeded to writable location:" << ext;
            return ext;
        }
    }
    QDir(tmp).removeRecursively();
    qWarning() << "[Common] python_env seed failed — using read-only bundle (upgrade disabled)";
    return bundled;
#else
    // Linux: 서명 seal 없음 → 설치 위치의 python_env 그대로.
    return bundledResourcesDir() + "/python_env";
#endif
}

QString bundledPythonPath()
{
#ifdef Q_OS_WIN
    return activePythonEnvDir() + "/python.exe";
#else
    return activePythonEnvDir() + "/bin/python3";
#endif
}

QString bundledToolsDir()
{
    return bundledResourcesDir() + "/tools";
}

// ── AI 자가수리: 편집가능 스크립트의 쓰기가능 override 위치 ──
//   AI 가 고친 스크립트는 여기에 저장된다(번들은 서명 봉인이라 못 씀). override 가 있으면
//   그걸 쓰고, 없으면 번들 원본을 쓴다(= 아무 것도 안 바꾸면 동작 무변화 → 안전).
QString scriptOverrideDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/script_overrides";
}
QString activeToolScriptPath(const QString &name)
{
    // 쓰는 순서: 서명이 맞은 고침 꾸러미 > 이 맥의 자가수리가 고친 것 > 번들 원본
    if (hotfixTools().contains(name)) {
        const QString hf = hotfixToolsDir() + "/" + name;
        if (QFileInfo::exists(hf)) return hf;
    }
    const QString ov = scriptOverrideDir() + "/" + name;
    if (QFileInfo::exists(ov)) return ov;
    return bundledToolsDir() + "/" + name;
}

// ── 외부 서비스 상수 런타임 오버라이드 ────────────────────────────────────────
//   X(트위터)의 GraphQL query ID·Bearer 토큰처럼 '언젠가 반드시 바뀌는' 값을 코드에서
//   분리한다. 파일이 없거나 키가 없으면 코드 기본값을 그대로 쓰므로 기존 동작과 동일하다.
QString apiOverridesPath()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/api_overrides.json";
}

static QJsonObject loadApiOverrides()
{
    static QMutex mu;
    static QJsonObject cache;
    static QDateTime cachedAt;
    QMutexLocker lock(&mu);

    const QString p = apiOverridesPath();
    const QFileInfo fi(p);
    if (!fi.exists()) { cache = QJsonObject(); return cache; }
    // 파일이 바뀌었을 때만 다시 읽는다(수집 루프에서 자주 불리므로).
    if (cachedAt.isValid() && fi.lastModified() <= cachedAt) return cache;

    QFile f(p);
    if (f.open(QIODevice::ReadOnly)) {
        const QJsonDocument d = QJsonDocument::fromJson(f.readAll());
        f.close();
        if (d.isObject()) { cache = d.object(); cachedAt = fi.lastModified(); }
    }
    return cache;
}

QString apiOverride(const QString &key, const QString &builtinDefault)
{
    // 순서: 이 맥에서 손으로(또는 스스로 되살리며) 넣은 값 > 고침 꾸러미 > 코드에 박힌 기본값
    const QJsonObject o = loadApiOverrides();
    const QString v = o.value(key).toString();
    if (!v.isEmpty()) return v;
    const QString r = remoteOverride(key);
    return r.isEmpty() ? builtinDefault : r;
}

// ── 고침 꾸러미 ────────────────────────────────────────────────────────────
namespace {
QReadWriteLock g_hotfixLock;
QHash<QString, QString> g_remoteOverrides;
QHash<QString, QStringList> g_shapeAliases;
QList<HotfixRule> g_remoteRules;
std::atomic<bool> g_hasAliases{false};

// 열쇠마다 받아도 되는 값의 모양. 여기 없는 열쇠는 받지 않는다.
bool overrideValueOk(const QString &key, const QString &v, QString *why)
{
    static const QRegularExpression twPath(R"(^/i/api/graphql/[A-Za-z0-9_-]{10,40}/[A-Za-z]{3,40}$)");
    static const QRegularExpression bearer(R"(^[A-Za-z0-9%_=-]{40,300}$)");
    static const QRegularExpression docId(R"(^\d{8,25}$)");
    static const QRegularExpression providers(R"(^__relay_internal__pv__[A-Za-z0-9_]+relayprovider(,__relay_internal__pv__[A-Za-z0-9_]+relayprovider){0,30}$)");
    if (key == QLatin1String("twitter.bearer")) {
        if (bearer.match(v).hasMatch()) return true;
        *why = QStringLiteral("bearer 모양이 아님"); return false;
    }
    if (key.startsWith(QLatin1String("twitter."))) {
        // 경로만 받는다 — 주소를 통째로 받으면 요청(과 쿠키)을 딴 곳으로 돌릴 수 있다
        if (twPath.match(v).hasMatch()) return true;
        *why = QStringLiteral("x.com GraphQL 경로 모양이 아님"); return false;
    }
    if (key.startsWith(QLatin1String("instagram.")) && key.endsWith(QLatin1String("DocId"))) {
        if (docId.match(v).hasMatch()) return true;
        *why = QStringLiteral("doc_id 는 숫자여야 함"); return false;
    }
    if (key.startsWith(QLatin1String("instagram.")) && key.endsWith(QLatin1String("Providers"))) {
        if (providers.match(v).hasMatch()) return true;
        *why = QStringLiteral("provider 이름 목록 모양이 아님"); return false;
    }
    *why = QStringLiteral("모르는 열쇠");
    return false;
}

// 수리 규칙의 글 — 링크·연락처·비밀을 달라는 글이 끼면 받지 않는다(낚시 막기)
bool ruleTextOk(const QString &s)
{
    static const QRegularExpression bad(R"(://|www\.|https?|@|토큰\s*값|비밀번호|password|cookie\s*값|붙여넣)",
                                        QRegularExpression::CaseInsensitiveOption);
    return !s.isEmpty() && s.size() <= 220 && !bad.match(s).hasMatch();
}
} // namespace

HotfixApplyResult applyHotfix(const QJsonObject &overrides, const QJsonObject &aliases,
                              const QJsonArray &rules, bool dryRun)
{
    HotfixApplyResult res;
    QHash<QString, QString> ov;
    for (auto it = overrides.constBegin(); it != overrides.constEnd(); ++it) {
        const QString v = it.value().toString();
        QString why;
        if (overrideValueOk(it.key(), v, &why)) ov.insert(it.key(), v);
        else res.rejected << QStringLiteral("값 %1 — %2").arg(it.key(), why);
    }
    static const QRegularExpression name(R"(^[A-Za-z0-9_]{1,64}$)");
    QHash<QString, QStringList> al;
    for (auto it = aliases.constBegin(); it != aliases.constEnd() && al.size() < 500; ++it) {
        if (!name.match(it.key()).hasMatch()) { res.rejected << QStringLiteral("별명 열쇠 %1").arg(it.key().left(40)); continue; }
        QStringList vs;
        for (const auto &v : it.value().toArray()) {
            const QString s = v.toString();
            if (name.match(s).hasMatch() && !vs.contains(s) && vs.size() < 40) vs << s;
            else res.rejected << QStringLiteral("별명 %1→%2").arg(it.key(), s.left(40));
        }
        if (!vs.isEmpty()) al.insert(it.key(), vs);
    }
    // 무늬는 글자 그대로의 대안(가|나|다)만 — 겹친 반복 같은 복잡한 정규식은 앱을 멈추게 할 수 있다
    static const QRegularExpression literalAlts(R"(^[^()*+?{}\[\]\\|]{2,80}(\|[^()*+?{}\[\]\\|]{2,80}){0,20}$)");
    static const QStringList okActions = {QString(), QStringLiteral("refreshAllTokens"),
                                          QStringLiteral("updateModules"), QStringLiteral("repairPython")};
    QList<HotfixRule> rl;
    for (const auto &v : rules) {
        const QJsonObject o = v.toObject();
        const QString pat = o.value("pattern").toString();
        const QString cause = o.value("cause").toString(), advice = o.value("advice").toString();
        const QString action = o.value("action").toString();
        if (!literalAlts.match(pat).hasMatch()) { res.rejected << QStringLiteral("규칙 무늬 %1").arg(pat.left(40)); continue; }
        if (!okActions.contains(action)) { res.rejected << QStringLiteral("규칙 할 일 %1").arg(action.left(40)); continue; }
        if (!ruleTextOk(cause) || !ruleTextOk(advice)) { res.rejected << QStringLiteral("규칙 글 %1").arg(cause.left(40)); continue; }
        QRegularExpression re(pat, QRegularExpression::CaseInsensitiveOption);
        if (!re.isValid()) { res.rejected << QStringLiteral("규칙 무늬 %1").arg(pat.left(40)); continue; }
        rl.append({re, cause, advice, action});
        if (rl.size() >= 200) break;
    }
    res.overrides = ov.size(); res.aliases = al.size(); res.rules = rl.size();
    if (!dryRun) {
        QWriteLocker lk(&g_hotfixLock);
        g_remoteOverrides = ov; g_shapeAliases = al; g_remoteRules = rl;
        g_hasAliases.store(!al.isEmpty());
    }
    return res;
}

QString remoteOverride(const QString &key)
{
    QReadLocker lk(&g_hotfixLock);
    return g_remoteOverrides.value(key);
}

QStringList shapeAliasesFor(const QStringList &keys)
{
    if (!g_hasAliases.load(std::memory_order_relaxed)) return keys;   // 대개 여기서 끝난다(빠른 길)
    QStringList out = keys;
    QReadLocker lk(&g_hotfixLock);
    for (const QString &k : keys)
        for (const QString &a : g_shapeAliases.value(k))
            if (!out.contains(a)) out << a;
    return out;
}

QList<HotfixRule> remoteRepairRules()
{
    QReadLocker lk(&g_hotfixLock);
    return g_remoteRules;
}

QString hotfixDir()
{
    return QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/hotfix";
}

namespace { QHash<QString, QString> g_hotfixTools; }
QString hotfixToolsDir() { return hotfixDir() + "/tools"; }
void setHotfixTools(const QHash<QString, QString> &m) { QWriteLocker lk(&g_hotfixLock); g_hotfixTools = m; }
QHash<QString, QString> hotfixTools() { QReadLocker lk(&g_hotfixLock); return g_hotfixTools; }

// ── 프록시(VPN) ────────────────────────────────────────────────────────────
namespace {
struct ProxyCfg {
    bool enabled = false;
    QString host, user, pass;
    int port = 1080;
    // ★ 연결 방식 — 이름 붙은 프로필에는 HTTP 프록시도 들어온다(윈도우 프로필의 type).
    //   예전엔 SOCKS5 만 받았으므로 비면 socks5 로 본다.
    QString type = QStringLiteral("socks5");
};
QMutex   g_proxyMutex;
ProxyCfg g_proxy;                 // 전역(기본) 프록시 — 계정별 지정이 없을 때 쓴다

// ★ 계정마다 다른 프록시를 쓰려면 '스레드마다 다른 값' 이어야 한다.
//   수집은 스레드로 병렬로 돈다(병렬 다중 수집). 전역 하나만 두고 계정마다
//   바꿔 끼우면, 동시에 도는 두 수집이 서로의 설정을 덮어써서 엉뚱한 IP 로 나간다.
//   그래서 수집 스레드가 자기 것을 들고, 지정이 없으면 전역으로 떨어진다.
thread_local ProxyCfg *t_proxy = nullptr;

// 중계기는 '상위 프록시마다' 하나씩 둔다. 계정별로 상위가 다르면 중계기도 달라야
// 한다 — 하나를 돌려쓰면 모든 계정이 같은 IP 로 나간다(계정별 지정이 무의미해진다).
struct Relay { QProcess *proc = nullptr; int port = 0; };
QHash<QString, Relay> g_relays;   // 키: 상위 프록시 서명(host:port:user)
}

// 지금 이 스레드에 적용되는 프록시. 스레드 지정이 있으면 그것, 없으면 전역.
static ProxyCfg effectiveProxy()
{
    if (t_proxy) return *t_proxy;
    QMutexLocker lock(&g_proxyMutex);
    return g_proxy;
}

static QString proxySignature(const ProxyCfg &c)
{
    // ★ 종류도 서명에 넣는다 — 같은 주소라도 SOCKS5 와 HTTP 는 다른 중계기여야 한다.
    // ★ 비밀번호의 지문도 넣는다(값 자체는 넣지 않는다). 프로필의 비밀번호를 고친 뒤에도 옛 중계기가
    //   옛 비밀번호로 계속 쓰였다 — 연결 시험은 새 비밀번호로 통과하는데 캡처만 실패했다.
    const QString passTag = QString::fromLatin1(
        QCryptographicHash::hash(c.pass.toUtf8(), QCryptographicHash::Sha256).toHex().left(12));
    return c.type + QStringLiteral("://") + c.host + QLatin1Char(':') + QString::number(c.port)
         + QLatin1Char(':') + c.user + QLatin1Char('#') + passTag;
}

void setThreadProxy(bool enabled, const QString &host, int port,
                    const QString &user, const QString &pass, const QString &type)
{
    clearThreadProxy();
    t_proxy = new ProxyCfg{enabled, host, user, pass, port,
                           type.isEmpty() ? QStringLiteral("socks5") : type.toLower()};
}

void clearThreadProxy()
{
    delete t_proxy;
    t_proxy = nullptr;
}

void setProxyConfig(bool enabled, const QString &host, int port,
                    const QString &user, const QString &pass, const QString &type)
{
    const QString t = type.isEmpty() ? QStringLiteral("socks5") : type.toLower();
    QMutexLocker lock(&g_proxyMutex);
    const bool changed = (g_proxy.enabled != enabled) || (g_proxy.host != host)
                      || (g_proxy.port != port) || (g_proxy.user != user)
                      || (g_proxy.pass != pass) || (g_proxy.type != t);
    g_proxy = {enabled, host, user, pass, port, t};
    if (changed) {     // 설정이 바뀌면 중계기를 모두 정리한다(다음 요청 때 새로 뜬다)
        for (auto it = g_relays.begin(); it != g_relays.end(); ++it) {
            if (!it->proc) continue;
            it->proc->kill();
            it->proc->waitForFinished(2000);
            it->proc->deleteLater();
        }
        g_relays.clear();
    }
}

bool proxyEnabled()
{
    const ProxyCfg c = effectiveProxy();
    return c.enabled && !c.host.isEmpty() && c.port > 0;
}

QString proxyUrl()
{
    const ProxyCfg c = effectiveProxy();
    if (!c.enabled || c.host.isEmpty() || c.port <= 0) return QString();
    // ★ HTTPS 프록시(프록시까지 TLS)는 Qt 도 파이썬 urllib 도 말하지 못한다 — 그대로 넘기면 평문으로
    //   붙어 아이디·비밀번호가 그대로 흘렀다. 중계기(socks_relay.py)만 상위에 TLS 로 붙는다.
    //   그래서 https 는 언제나 중계기 주소(127.0.0.1 · 자격증명 없음)를 준다.
    //   중계기를 못 띄우면 빈 값이다 — 부르는 쪽(HttpClient·bundledProcessEnv)이 닫힌 길로 막는다.
    if (c.type == QLatin1String("https")) return proxyLocalRelayUrl();
    QString auth;
    if (!c.user.isEmpty()) {
        auth = QString::fromLatin1(QUrl::toPercentEncoding(c.user) + ":"
                                   + QUrl::toPercentEncoding(c.pass) + "@");
    }
    // 프로필의 종류를 그대로 scheme 으로 쓴다 — httpx·yt-dlp·curl 이 모두 이 모양을 읽는다.
    // ★ 이어 붙인다. arg() 를 겹쳐 쓰면 비밀번호의 퍼센트 코드(%2F · %3A …)를 다음 arg() 가
    //   자리표시로 읽어 비밀번호와 포트가 함께 깨졌다('/' 나 ':' 가 든 비밀번호).
    return (c.type.isEmpty() ? QStringLiteral("socks5") : c.type) + QStringLiteral("://")
         + auth + c.host + QLatin1Char(':') + QString::number(c.port);
}

QString proxyExitTag()
{
    if (!proxyEnabled()) return QString();
    const ProxyCfg c = effectiveProxy();
    const QString id = c.type + QStringLiteral("://") + c.host + QLatin1Char(':')
                     + QString::number(c.port) + QLatin1Char(':') + c.user;
    return QString::fromLatin1(QCryptographicHash::hash(id.toUtf8(), QCryptographicHash::Sha256).toHex().left(10));
}

QString proxyLocalRelayUrl()
{
    if (!proxyEnabled()) return QString();
    const ProxyCfg cfg = effectiveProxy();
    // ★ 인증 없는 HTTP(S) 프록시는 중계기가 필요 없다 — 크로미움이 그대로 받는다.
    //   인증이 붙은 HTTP 프록시는 중계기가 상위로 HTTP CONNECT 를 쓴다(socks_relay.py).
    //   HTTPS 는 인증이 없어도 중계기로 간다 — Qt·파이썬 쪽(proxyUrl)이 이 주소를 같이 쓰기 때문이다.
    if (cfg.type == QLatin1String("http") && cfg.user.isEmpty())
        return QStringLiteral("http://") + cfg.host + QLatin1Char(':') + QString::number(cfg.port);
    const QString sig = proxySignature(cfg);
    {
        QMutexLocker lock(&g_proxyMutex);
        const Relay r = g_relays.value(sig);
        if (r.proc && r.proc->state() != QProcess::NotRunning && r.port > 0)
            return QStringLiteral("socks5://127.0.0.1:%1").arg(r.port);
    }

    const QString script = activeToolScriptPath(QStringLiteral("socks_relay.py"));
    const QString python = bundledPythonPath();
    if (!QFileInfo::exists(script) || !QFileInfo::exists(python)) {
        qWarning() << "[proxy] 중계기를 띄울 수 없습니다 — 스크립트나 파이썬이 없습니다";
        return QString();
    }

    // ★ 잠금을 쥐지 않은 채 띄운다.
    //   예전엔 여기서 잠금을 잡고 기동(최대 5초)과 준비 신호(최대 8초)를 기다렸다.
    //   그동안 다른 수집 스레드의 effectiveProxy() 조회가 전부 이 잠금에서 멈췄다 —
    //   병렬 수집이 최대 13초씩 통째로 얼어붙는다.
    //   대신 마지막에 잠금을 짧게 잡고, 그 사이 남이 먼저 만들었으면 내 것을 버린다.
    auto *p = new QProcess();
    p->setProcessEnvironment(QProcessEnvironment::systemEnvironment());
    p->start(python, {script, QStringLiteral("--stdin-args")});
    if (!p->waitForStarted(5000)) { p->deleteLater(); return QString(); }

    // ★ 자격증명은 stdin 첫 줄로만 넘긴다 — 명령줄에 실으면 ps 로 남이 읽는다.
    // type 은 상위 프록시의 종류 — 중계기가 SOCKS5 와 HTTP CONNECT 를 이것으로 가른다.
    QJsonObject init{{"listen_port", 0}, {"type", cfg.type},
                     {"host", cfg.host}, {"port", cfg.port},
                     {"user", cfg.user}, {"pass", cfg.pass}};
    p->write(QJsonDocument(init).toJson(QJsonDocument::Compact) + "\n");
    p->waitForBytesWritten(3000);

    // 준비 신호를 기다린다.
    int port = 0;
    QByteArray acc;
    while (p->waitForReadyRead(8000)) {
        acc += p->readAllStandardOutput();
        const int nl = acc.indexOf('\n');
        if (nl < 0) continue;
        const QJsonObject o = QJsonDocument::fromJson(acc.left(nl)).object();
        if (o.value("ready").toBool()) { port = o.value("port").toInt(); }
        break;
    }
    if (port <= 0) {
        qWarning() << "[proxy] 중계기가 준비되지 않았습니다";
        p->kill(); p->waitForFinished(2000); p->deleteLater();
        return QString();
    }
    {
        QMutexLocker lock(&g_proxyMutex);
        // 기다리는 사이에 다른 스레드가 같은 상위 프록시로 먼저 띄웠을 수 있다.
        const Relay other = g_relays.value(sig);
        if (other.proc && other.proc->state() != QProcess::NotRunning && other.port > 0) {
            lock.unlock();
            p->kill(); p->waitForFinished(2000); p->deleteLater();
            return QStringLiteral("socks5://127.0.0.1:%1").arg(other.port);
        }
        g_relays.insert(sig, Relay{p, port});
    }
    qInfo() << "[proxy] 로컬 중계기 준비 — 127.0.0.1:" << port
            << "(상위" << cfg.host << ")";
    return QStringLiteral("socks5://127.0.0.1:%1").arg(port);
}

void stopProxyRelay()
{
    QMutexLocker lock(&g_proxyMutex);
    for (auto it = g_relays.begin(); it != g_relays.end(); ++it) {
        if (!it->proc) continue;
        it->proc->kill();
        it->proc->waitForFinished(2000);
        it->proc->deleteLater();
    }
    g_relays.clear();
}

bool setApiOverride(const QString &key, const QString &value)
{
    const QString p = apiOverridesPath();
    QDir().mkpath(QFileInfo(p).absolutePath());

    QJsonObject o;
    { QFile f(p);
      if (f.open(QIODevice::ReadOnly)) {
          const QJsonDocument d = QJsonDocument::fromJson(f.readAll()); f.close();
          if (d.isObject()) o = d.object();
      } }

    if (value.isEmpty()) o.remove(key);      // 빈 값 = 기본값으로 되돌리기
    else                 o[key] = value;

    QFile f(p);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) return false;
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    f.close();
    return true;
}

QString apiOverridesJson()
{
    return QString::fromUtf8(QJsonDocument(loadApiOverrides()).toJson(QJsonDocument::Indented));
}

bool isDirWritable(const QString &dir)
{
    if (dir.isEmpty() || !QFileInfo::exists(dir)) return false;
    QFile probe(dir + "/.kamera_write_test");
    if (!probe.open(QIODevice::WriteOnly)) return false;
    probe.write("1"); probe.close(); probe.remove();
    return true;
}

QString appBundlePath()
{
#ifdef Q_OS_MACOS
    // <...>/Predormition.app/Contents/MacOS  →  <...>/Predormition.app
    QDir d(QCoreApplication::applicationDirPath());
    if (!d.cdUp()) return QString();          // Contents
    if (!d.cdUp()) return QString();          // *.app
    const QString p = d.absolutePath();
    return p.endsWith(".app") ? p : QString();
#else
    return QString();
#endif
}

// 번들 안에 파일을 쓴 뒤 봉인을 복구한다. 실패하면 false + 사유.
static QAtomicInt g_resealInFlight(0);
bool resealInFlight() { return g_resealInFlight.loadAcquire() != 0; }

static std::atomic<bool> g_appQuitting{false};
void markAppQuitting() { g_appQuitting.store(true, std::memory_order_release); }
bool appQuitting() { return g_appQuitting.load(std::memory_order_acquire); }

int capturePortBase()
{
    bool ok = false;
    const int v = qEnvironmentVariableIntValue("HANISHIKI_CAPTURE_PORT", &ok);
    return (ok && v >= 1024 && v <= 65000) ? v : 9223;
}

#ifdef Q_OS_WIN
// ── 윈도우: --user-data-dir 전체 경로로 캡처 Chrome 고르기 ─────────────────────
// ★ 맥의 `ps -axww -o pid=,command=` 자리. taskkill 은 명령줄로 거르지 못하므로
//   (HanishikiBackend 의 killByCommandLine 설명) WMI(Win32_Process)의 CommandLine 을 읽는다.
//   ★ 실행 파일 이름(chrome.exe · msedge.exe …)으로 거르지 않는다 — 맥처럼 --user-data-dir 값만 본다.
//     번들 Chromium · Chrome · Edge · Brave 무엇으로 떴든 같은 프로필이면 잡히고, 다른 프로필이면 안 잡힌다.
//   ★ 명령줄은 base64(UTF-8) 로 받는다. 파이프로 나오는 PowerShell 글은 콘솔 코드페이지라
//     사용자 이름의 한글·일본어가 '?' 로 뭉개진다 — 그러면 전체 경로 비교가 늘 어긋나 아무것도 못 고른다.
//   ★ 스크립트는 -EncodedCommand 로 넘기고 경로는 스크립트에 넣지 않는다. 비교는 아래 C++ 에서
//     글자 그대로 한다 — 명령줄 인용 규칙 · -like 의 [ ] * 에 경로가 걸릴 일이 없다.
namespace {
struct ProcCmd { qint64 pid = 0; QString cmd; };

QList<ProcCmd> processesWithUserDataDir()
{
    QList<ProcCmd> out;
    const QString script = QStringLiteral(
        "$ErrorActionPreference='SilentlyContinue';$ProgressPreference='SilentlyContinue';"
        "Get-CimInstance Win32_Process -Filter \"CommandLine LIKE '%--user-data-dir=%'\" | "
        "ForEach-Object { [string]$_.ProcessId + ' ' + "
        "[Convert]::ToBase64String([Text.Encoding]::UTF8.GetBytes([string]$_.CommandLine)) }");
    const QByteArray utf16le(reinterpret_cast<const char *>(script.utf16()),
                             static_cast<int>(script.size()) * 2);
    QProcess ps;
    ps.start(QStringLiteral("powershell"),
             {QStringLiteral("-NoProfile"), QStringLiteral("-NonInteractive"),
              QStringLiteral("-EncodedCommand"), QString::fromLatin1(utf16le.toBase64())});
    if (!ps.waitForStarted(5000)) return out;
    if (!ps.waitForFinished(20000)) { ps.kill(); ps.waitForFinished(1000); return out; }
    const QList<QByteArray> lines = ps.readAllStandardOutput().split('\n');
    for (const QByteArray &raw : lines) {
        const QByteArray line = raw.trimmed();
        const int sp = line.indexOf(' ');
        if (sp <= 0) continue;
        const qint64 pid = line.left(sp).toLongLong();
        if (pid <= 0) continue;
        ProcCmd pc;
        pc.pid = pid;
        pc.cmd = QString::fromUtf8(QByteArray::fromBase64(line.mid(sp + 1)));
        out.append(pc);
    }
    return out;
}

// --user-data-dir= 값이 시작하는 자리. 값만 따옴표로 감싼 꼴(--user-data-dir="C:\…" — Chrome 이 자식
// 프로세스에 넘기는 꼴)이면 그 따옴표 뒤. 인자 전체를 감싼 꼴("--user-data-dir=C:/a b/…" — QProcess 가
// 빈칸 든 인자를 넘기는 꼴)은 '=' 바로 뒤다.
int userDataDirValueStart(const QString &cmd, int keyAt, int keyLen)
{
    int vs = keyAt + keyLen;
    if (vs < cmd.size() && cmd.at(vs) == QLatin1Char('"')) ++vs;
    return vs;
}

// 경로가 거기서 끝나는가 — 다음 글자가 '"' · 빈칸 · 끝.
bool atValueEnd(const QString &cmd, int i)
{
    return i >= cmd.size() || cmd.at(i) == QLatin1Char('"') || cmd.at(i) == QLatin1Char(' ');
}

// 찾은 프로세스를 맥의 SIGTERM → 0.3초 → SIGKILL 과 같은 순서로 끝낸다.
//   먼저 핸들을 쥔다 — 쥐고 있는 동안 그 pid 는 다른 프로세스에 다시 쓰이지 않으므로, 0.3초 뒤에
//   다시 훑지 않고도 처음 고른 프로세스만 끊는다.
//   taskkill(/F 없이)은 창에 닫으라고 보낸다 — 창이 있는 Chrome 은 프로필을 정리하고 끝난다
//   (창 없는 자식 · 헤드리스는 거절하고 남는다). 0.3초 뒤에도 살아 있는 것은 TerminateProcess.
void endProcessesGently(const QList<qint64> &pids)
{
    QList<HANDLE> handles;
    QStringList soft;
    for (qint64 pid : pids) {
        HANDLE h = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
        if (!h) continue;                                    // 이미 끝났다
        handles << h;
        soft << QStringLiteral("/PID") << QString::number(pid);
    }
    if (handles.isEmpty()) return;
    QProcess::execute(QStringLiteral("taskkill"), soft);
    QThread::msleep(300);                                    // 얌전히 끝날 틈
    for (HANDLE h : handles) {
        if (WaitForSingleObject(h, 0) == WAIT_TIMEOUT) TerminateProcess(h, 1);
        CloseHandle(h);
    }
}
} // namespace
#endif

QList<qint64> captureChromePids(const QString &profileDir, bool prefix)
{
    QList<qint64> out;
#ifndef Q_OS_WIN
    if (profileDir.isEmpty()) return out;
    // pkill/pgrep -f 는 정규식이라 경로의 공백·괄호를 다루기 어렵다 — ps 를 한 번 읽어 글자 그대로 비교한다.
    QProcess ps;
    ps.start("/bin/ps", {"-axww", "-o", "pid=,command="});
    if (!ps.waitForFinished(3000)) { ps.kill(); ps.waitForFinished(1000); return out; }
    const QString key = QStringLiteral("--user-data-dir=") + profileDir;
    const qint64 self = QCoreApplication::applicationPid();
    const QStringList lines = QString::fromUtf8(ps.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        const int sp = line.indexOf(QLatin1Char(' '));
        if (sp <= 0) continue;
        const qint64 pid = line.left(sp).toLongLong();
        if (pid <= 0 || pid == self) continue;
        const QString cmd = line.mid(sp + 1);
        const int at = cmd.indexOf(key);
        if (at < 0) continue;
        const int end = at + key.size();
        // 경로가 거기서 끝나야 한다(다음 글자가 빈칸이거나 끝). prefix 면 트랙·PEN 프로필(…_<이름>)도.
        if (end < cmd.size()) {
            const QChar c = cmd.at(end);
            if (c != QLatin1Char(' ') && !(prefix && c == QLatin1Char('_'))) continue;
        }
        out << pid;
    }
#else
    // 윈도우 — 맥과 같은 기준(--user-data-dir 의 전체 경로)으로 고른다. 대소문자는 보지 않고
    // '\' 와 '/' 는 같게 본다(우리는 '/' 로 넘기는데 Chrome 은 자식에게 역슬래시로 적어 넘긴다).
    if (profileDir.isEmpty()) return out;
    const QString want = QDir::cleanPath(QDir::fromNativeSeparators(profileDir));
    const QString key = QStringLiteral("--user-data-dir=");
    const qint64 self = QCoreApplication::applicationPid();
    for (const ProcCmd &pc : processesWithUserDataDir()) {
        if (pc.pid == self) continue;
        const QString cmd = QDir::fromNativeSeparators(pc.cmd);
        for (int at = cmd.indexOf(key, 0, Qt::CaseInsensitive); at >= 0;
             at = cmd.indexOf(key, at + key.size(), Qt::CaseInsensitive)) {
            const int vs = userDataDirValueStart(cmd, at, key.size());
            if (cmd.mid(vs, want.size()).compare(want, Qt::CaseInsensitive) != 0) continue;
            const int end = vs + want.size();
            // 경로가 거기서 끝나야 한다('"' · 빈칸 · 끝). prefix 면 트랙·PEN 프로필(…_<이름>)도.
            if (!atValueEnd(cmd, end) && !(prefix && cmd.at(end) == QLatin1Char('_'))) continue;
            out << pc.pid;
            break;
        }
    }
#endif
    return out;
}

int killCaptureChromes(const QString &profileDir, bool prefix)
{
#ifndef Q_OS_WIN
    const QList<qint64> found = captureChromePids(profileDir, prefix);
    if (found.isEmpty()) return 0;
    for (qint64 pid : found) ::kill(static_cast<pid_t>(pid), SIGTERM);
    QThread::msleep(300);                                    // 얌전히 끝날 틈
    for (qint64 pid : captureChromePids(profileDir, prefix)) ::kill(static_cast<pid_t>(pid), SIGKILL);
    return found.size();
#else
    const QList<qint64> found = captureChromePids(profileDir, prefix);
    if (found.isEmpty()) return 0;
    endProcessesGently(found);
    return found.size();
#endif
}

QList<qint64> orphanedCaptureChromePids(const QString &appSupportRoot)
{
    QList<qint64> out;
#ifndef Q_OS_WIN
    if (appSupportRoot.isEmpty()) return out;
    const QString root = QDir::cleanPath(appSupportRoot) + QLatin1Char('/');
    QProcess ps;
    ps.start("/bin/ps", {"-axww", "-o", "pid=,command="});
    if (!ps.waitForFinished(3000)) { ps.kill(); ps.waitForFinished(1000); return out; }
    const QString key = QStringLiteral("--user-data-dir=");
    const QString mark = QStringLiteral("/chrome_capture_profile");
    const qint64 self = QCoreApplication::applicationPid();
    const QStringList lines = QString::fromUtf8(ps.readAllStandardOutput()).split('\n', Qt::SkipEmptyParts);
    for (const QString &raw : lines) {
        const QString line = raw.trimmed();
        const int sp = line.indexOf(QLatin1Char(' '));
        if (sp <= 0) continue;
        const qint64 pid = line.left(sp).toLongLong();
        if (pid <= 0 || pid == self) continue;
        const QString cmd = line.mid(sp + 1);
        const int at = cmd.indexOf(key);
        if (at < 0) continue;
        const int vs = at + key.size();
        const int mk = cmd.indexOf(mark, vs);
        if (mk < 0) continue;
        // 경로가 …/chrome_capture_profile 또는 …/chrome_capture_profile_<이름> 에서 끝나야 한다(경로엔 빈칸이 있을 수 있다)
        int end = mk + mark.size();
        if (end < cmd.size() && cmd.at(end) == QLatin1Char('_'))
            while (end < cmd.size() && cmd.at(end) != QLatin1Char(' ') && cmd.at(end) != QLatin1Char('/')) ++end;
        if (end < cmd.size() && cmd.at(end) != QLatin1Char(' ')) continue;
        if (!cmd.mid(vs, end - vs).startsWith(root)) continue;        // 이 홈의 Application Support 밖(격리 사본 등)은 두고
        if (QFileInfo::exists(cmd.mid(vs, mk - vs))) continue;        // 데이터 폴더가 살아 있으면 고아가 아니다
        out << pid;
    }
#else
    // 윈도우 — appSupportRoot 는 %APPDATA%(조직 폴더가 있으면 그 아래). 그 밑 …/chrome_capture_profile[_…] 인데
    // 데이터 폴더가 이제 없는 것만. 앱 이름이 바뀌어 데이터 폴더가 옮겨진 뒤 옛 판이 남긴 Chrome 이 여기 걸린다.
    if (appSupportRoot.isEmpty()) return out;
    const QString root = QDir::cleanPath(QDir::fromNativeSeparators(appSupportRoot)) + QLatin1Char('/');
    const QString key = QStringLiteral("--user-data-dir=");
    const QString mark = QStringLiteral("/chrome_capture_profile");
    const qint64 self = QCoreApplication::applicationPid();
    for (const ProcCmd &pc : processesWithUserDataDir()) {
        if (pc.pid == self) continue;
        const QString cmd = QDir::fromNativeSeparators(pc.cmd);
        const int at = cmd.indexOf(key, 0, Qt::CaseInsensitive);
        if (at < 0) continue;
        const int vs = userDataDirValueStart(cmd, at, key.size());
        const int mk = cmd.indexOf(mark, vs, Qt::CaseInsensitive);
        if (mk < 0) continue;
        // 경로가 …/chrome_capture_profile 또는 …/chrome_capture_profile_<이름> 에서 끝나야 한다(경로엔 빈칸이 있을 수 있다)
        int end = mk + mark.size();
        if (end < cmd.size() && cmd.at(end) == QLatin1Char('_'))
            while (!atValueEnd(cmd, end) && cmd.at(end) != QLatin1Char('/')) ++end;
        if (!atValueEnd(cmd, end)) continue;
        if (!cmd.mid(vs, end - vs).startsWith(root, Qt::CaseInsensitive)) continue;   // 이 사용자의 앱 데이터 밖은 두고
        if (QFileInfo::exists(cmd.mid(vs, mk - vs))) continue;                       // 데이터 폴더가 살아 있으면 고아가 아니다
        out << pc.pid;
    }
#endif
    return out;
}

int killOrphanedCaptureChromes(const QString &appSupportRoot)
{
#ifndef Q_OS_WIN
    const QList<qint64> found = orphanedCaptureChromePids(appSupportRoot);
    if (found.isEmpty()) return 0;
    for (qint64 pid : found) ::kill(static_cast<pid_t>(pid), SIGTERM);
    QThread::msleep(300);
    for (qint64 pid : orphanedCaptureChromePids(appSupportRoot)) ::kill(static_cast<pid_t>(pid), SIGKILL);
    return found.size();
#else
    const QList<qint64> found = orphanedCaptureChromePids(appSupportRoot);
    if (found.isEmpty()) return 0;
    endProcessesGently(found);
    return found.size();
#endif
}

SealState checkAppSeal(QString *detail)
{
#ifdef Q_OS_MACOS
    const QString app = appBundlePath();
    if (app.isEmpty()) return SealState::NotBundle;
    auto verify = [&app](bool strict, QString *errOut) {
        QProcess vf;
        QStringList args{QStringLiteral("--verify"), QStringLiteral("--deep")};
        if (strict) args << QStringLiteral("--strict");
        args << app;
        vf.start(QStringLiteral("/usr/bin/codesign"), args);
        if (!vf.waitForFinished(900000)) { vf.kill(); vf.waitForFinished(3000); }
        if (errOut) *errOut = QString::fromUtf8(vf.readAllStandardError());
        return vf.exitStatus() == QProcess::NormalExit && vf.exitCode() == 0;
    };
    QString e;
    if (verify(true, &e)) return SealState::Ok;
    if (e.contains(QLatin1String("detritus"))) {
        // 지울 수 있는 표시는 지운다 — 확장 속성만 지우고 파일 내용은 건드리지 않는다.
        //   (없는 파일마다 'No such xattr' 를 찍으므로 출력은 버린다)
        for (const char *attr : {"com.apple.FinderInfo", "com.apple.ResourceFork"}) {
            QProcess x;
            x.setStandardOutputFile(QProcess::nullDevice());
            x.setStandardErrorFile(QProcess::nullDevice());
            x.start(QStringLiteral("/usr/bin/xattr"), {QStringLiteral("-rd"), QString::fromLatin1(attr), app});
            if (!x.waitForFinished(120000)) { x.kill(); x.waitForFinished(3000); }
        }
        if (verify(true, &e)) return SealState::Ok;
        QString loose;
        if (e.contains(QLatin1String("detritus")) && verify(false, &loose)) {
            if (detail) *detail = e.left(300);
            return SealState::OkFinderDetritus;
        }
    }
    if (detail) *detail = e.left(300);
    return SealState::Broken;
#else
    Q_UNUSED(detail);
    return SealState::NotBundle;
#endif
}

bool resealAppBundle(QString *err)
{
#ifdef Q_OS_MACOS
    const QString app = appBundlePath();
    if (app.isEmpty()) { if (err) *err = "앱 번들이 아님(개발 실행)"; return true; }  // 번들 아니면 서명 무관

    // 파이썬 바이트코드 캐시는 서명 후에도 계속 생겨 봉인을 깨뜨린다 → 서명 전에 제거.
    {
        QDirIterator it(app + "/Contents/Resources", QStringList() << "__pycache__",
                        QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        QStringList caches;
        while (it.hasNext()) caches << it.next();
        for (const QString &c : caches) QDir(c).removeRecursively();
    }

    // 서명 아이덴티티 탐색 — 있으면 그걸로, 없으면 ad-hoc(-). 로컬 실행에는 ad-hoc 로 충분하다.
    QString identity = "-";
    {
        QProcess find;
        find.start("/usr/bin/security", {"find-identity", "-v", "-p", "codesigning"});
        if (find.waitForFinished(10000)) {
            const QString out = QString::fromUtf8(find.readAllStandardOutput());
            const QRegularExpression re("\\b([0-9A-F]{40})\\b");
            const QRegularExpressionMatch m = re.match(out);
            if (m.hasMatch()) identity = m.captured(1);
        }
    }

    QProcess cs;
    cs.start("/usr/bin/codesign", {"-f", "-s", identity, "--deep", app});
    if (!cs.waitForFinished(900000)) {                    // 번들이 크면(모델 포함) 수 분 걸릴 수 있음
        cs.kill(); cs.waitForFinished(3000);
        if (err) *err = "codesign 시간 초과";
        return false;
    }
    if (cs.exitCode() != 0) {
        // ★ 스크립트는 마지막에 엄격 검사를 한다. iCloud 폴더의 앱은 서명은 다 됐어도 앱 폴더의
        //   Finder 표시 때문에 거기서만 걸린다(checkAppSeal 설명) — 그것이면 서명은 된 것이다.
        if (checkAppSeal() == SealState::OkFinderDetritus) return true;
        // 스크립트가 실패 사유를 표준출력에도 적는다(개별 서명 실패 목록 등).
        const QString out = QString::fromUtf8(cs.readAllStandardOutput()).right(400);
        const QString e   = QString::fromUtf8(cs.readAllStandardError()).left(300);
        if (err) *err = (e.isEmpty() ? out : e);
        return false;
    }

    QString why;
    if (checkAppSeal(&why) == SealState::Broken) {
        if (err) *err = "재서명 후 검증 실패: " + why;
        return false;
    }
    return true;
#else
    Q_UNUSED(err);
    return true;   // 서명 봉인 없음(Windows/Linux) — 앱 내부 수정이 그대로 안전
#endif
}

// ═════════════════════════════════════════════════════════════════════════
// yt-dlp 자동 업데이트 — 사용자 폴더 우선, GitHub 죽어도 번들 fallback
// ═════════════════════════════════════════════════════════════════════════
QString userToolsDir()
{
    QString p = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/tools";
    QDir().mkpath(p);
    return p;
}

// 번들 yt-dlp 위치 탐색 — MacOS/ 또는 Resources/tools/
static QString findBundledYtDlp()
{
    QStringList paths;
#ifdef Q_OS_MACOS
    paths << QCoreApplication::applicationDirPath() + "/yt-dlp";  // Contents/MacOS/yt-dlp
    paths << bundledToolsDir() + "/yt-dlp";                        // Contents/Resources/tools/yt-dlp
#else
    // ★ Windows 배포본은 .exe 도구를 exe 루트에 둔다(워크플로 Deploy 참고).
    //   예전엔 tools/ 하위만 봐서 번들 yt-dlp 를 못 찾았다.
    paths << QCoreApplication::applicationDirPath() + "/yt-dlp.exe";
    paths << bundledToolsDir() + "/yt-dlp.exe";
    paths << bundledToolsDir() + "/yt-dlp";
#endif
    for (const QString &p : paths) {
        if (QFile::exists(p)) return p;
    }
    return QString();
}

// 사용자 폴더의 yt-dlp 경로 — 읽는 쪽(ytDlpExecutable)과 쓰는 쪽(ensureYtDlpReady)이
// 갈라지지 않도록 한 곳에서만 만든다. Windows 는 확장자가 없으면 실행 파일이 아니다.
static QString userYtDlpPath()
{
#ifdef Q_OS_WIN
    return userToolsDir() + "/yt-dlp.exe";
#else
    return userToolsDir() + "/yt-dlp";
#endif
}

QString ytDlpExecutable()
{
    // 1) 사용자 폴더 (자동 업데이트된 최신본)
    //   ★ Windows 는 확장자가 있어야 실행 파일로 인식된다. 예전엔 확장자 없는 이름만 봐서
    //     자동 업데이트본이 있어도 항상 무시되고, ensureYtDlpReady 의 복사본도 못 쓰였다.
    QString userBin = userYtDlpPath();
    if (QFile::exists(userBin)) {
        QFileInfo fi(userBin);
        if (fi.size() > 1000000 && fi.isExecutable()) return userBin;
    }
    // 2) 번들 (시작 시 자동으로 사용자 폴더에 복사됨, fallback)
    QString bundled = findBundledYtDlp();
    if (!bundled.isEmpty()) return bundled;
    // 3) system fallback
    return "yt-dlp";
}

// ensureYtDlpReady(autoUpdate):
//   1) 사용자 폴더에 yt-dlp 없으면 번들 복사 (앱이 검증한 버전 — 안전)
//   2) autoUpdate=true 면 yt-dlp --update-to stable (사용자 명시 ON 시만)
//   3) 검증: 업데이트 후 `--version` 호출. 비정상 출력이면 파일 삭제 + 번들 fallback.
//   4) GitHub 죽거나 변조되어도 → 번들 yt-dlp 항상 작동 보장.
void ensureYtDlpReady(bool autoUpdate)
{
    // ★ 읽는 쪽과 반드시 같은 경로여야 한다. 여기만 확장자 없이 저장하던 탓에 Windows 에서는
    //   번들본이 tools/yt-dlp (실행 불가) 로 복사되고, ytDlpExecutable 은 tools/yt-dlp.exe 를
    //   찾다 못 찾아 늘 번들로 폴백했다 — 자동 업데이트본은 한 번도 쓰이지 못했다.
    QString userBin = userYtDlpPath();
    QString bundled = findBundledYtDlp();

#ifdef Q_OS_WIN
    // 확장자 없이 저장되던 시절의 잔재 제거 — 실행되지 않으면서 18MB 를 차지한다.
    QFile::remove(userToolsDir() + "/yt-dlp");
#endif

    // 1) 사용자 폴더에 yt-dlp 없거나 너무 작으면 번들 복사 (앱과 함께 출하된 검증된 버전)
    // ★ 번들이 1MB 도 안 되는 '껍데기'(맥: python -m yt_dlp 로 넘기는 셸 스크립트)면 복사하지 않는다.
    //   읽는 쪽(ytDlpExecutable)은 1MB 미만 사본을 쓰지 않으니 복사해 봐야 쓰이지 않고,
    //   이 껍데기는 번들 옆에서만 제 파이썬을 찾는다 — 데이터 폴더로 옮기면 시스템 파이썬(3.9)으로
    //   떨어져 "unsupported version of Python" 으로 죽는다(실측 2026-09-19). 자가진단은 그 사본을 집어
    //   yt-dlp 를 '건너뜀' 으로 적었다. 이미 생긴 작은 사본도 치운다.
    if (!bundled.isEmpty() && QFileInfo(bundled).size() < 1000000) {
        if (QFile::exists(userBin) && QFileInfo(userBin).size() < 1000000) QFile::remove(userBin);
        if (!autoUpdate) return;
    }
    bool needCopy = !QFile::exists(userBin) || QFileInfo(userBin).size() < 1000000;
    if (needCopy && !bundled.isEmpty() && QFileInfo(bundled).size() >= 1000000) {
        QFile::remove(userBin);
        if (QFile::copy(bundled, userBin)) {
            QFile::setPermissions(userBin,
                QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                QFileDevice::ReadOther | QFileDevice::ExeOther);
        }
    }

    if (!autoUpdate) return;  // ★ 사용자가 명시적으로 켜야만 GitHub 다운로드 시도

    // 2) 백그라운드로 yt-dlp 자체 --update 호출 (stable 채널)
    //    yt-dlp 는 자체적으로 GitHub Release 의 SHA256SUMS 비교 → 변조 시 거부.
    //    실패해도 기존 binary 유지.
    if (!QFile::exists(userBin)) return;

    QProcess *p = new QProcess();
    p->setProgram(userBin);
    p->setArguments({"--update-to", "stable", "--no-warnings", "--quiet"});
    QObject::connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        p, [p, userBin, bundled](int code, QProcess::ExitStatus) {
            // 3) 업데이트 후 sanity check — yt-dlp --version 정상 출력 확인
            QProcess verify;
            verify.start(userBin, {"--version"});
            verify.waitForFinished(5000);
            QString ver = QString::fromUtf8(verify.readAllStandardOutput()).trimmed();
            // 정상 버전 패턴: 2024.xx.xx 또는 2025.xx.xx 형식
            QRegularExpression verRe(R"(^\d{4}\.\d{1,2}\.\d{1,2})");
            bool valid = verRe.match(ver).hasMatch();
            if (!valid) {
                // 의심스러움 — 번들로 복원
                QFile::remove(userBin);
                if (!bundled.isEmpty()) {
                    QFile::copy(bundled, userBin);
                    QFile::setPermissions(userBin,
                        QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                        QFileDevice::ReadGroup | QFileDevice::ExeGroup |
                        QFileDevice::ReadOther | QFileDevice::ExeOther);
                }
            }
            (void)code;
            p->deleteLater();
        });
    p->start();
    QTimer::singleShot(300000, p, [p]() {
        if (p->state() != QProcess::NotRunning) { p->kill(); p->deleteLater(); }
    });
}

QStringList pythonCandidates()
{
    QStringList list;
    QString bundled = bundledPythonPath();
    if (QFile::exists(bundled)) {
        list << bundled;
    }
#ifdef Q_OS_WIN
    // ★ 'python' 은 Microsoft Store 리다이렉터 스텁으로 풀리는 경우가 많다.
    //   실측(이 기계): PATH 의 python 이
    //     %LOCALAPPDATA%\Microsoft\WindowsApps\python.exe
    //       → AppInstallerPythonRedirector.exe
    //   로 풀리고, 실행하면 스크립트를 돌리지 않고 "Python" 한 줄만 찍고 0 으로 끝난다.
    //   QProcess 는 이것을 '정상 기동' 으로 보므로, 데몬은 영영 응답하지 않는데
    //   앱은 파이썬을 찾았다고 믿는다 — 가장 진단하기 어려운 형태의 실패다.
    //   진짜 런처인 py.exe 를 먼저 보고(py 는 'py 스크립트.py 인자' 를 그대로 받는다),
    //   WindowsApps 아래로 풀리는 스텁은 후보에서 뺀다.
    list << "py";
    for (const QString &name : {QStringLiteral("python"), QStringLiteral("python3")}) {
        const QString resolved = QStandardPaths::findExecutable(name);
        if (resolved.isEmpty()) continue;
        if (resolved.contains("/WindowsApps/", Qt::CaseInsensitive)
            || resolved.contains("\\WindowsApps\\", Qt::CaseInsensitive)) {
            qWarning() << "[Common] 스토어 스텁이라 건너뜁니다:" << resolved;
            continue;
        }
        list << resolved;
    }
#else
    list << "/opt/homebrew/bin/python3.14" << "/opt/homebrew/bin/python3" << "python3";
#endif
    return list;
}

// ★ 번들된 requirements.txt 를 읽어 설치할 패키지 목록을 만든다.
//   목록을 코드에 박아 두면 requirements.txt 와 갈라지고, 실제로 갈라져 있었다
//   (browser_cookie3·cryptography 누락 → Python 업그레이드 후 쿠키 추출이 조용히 죽음).
//   버전 고정(==)도 그대로 살려 넘긴다 — 고정해 둔 취지가 업그레이드 때 무너지지 않게.
//   파일을 못 찾으면 빈 목록이 아니라 최소 필수 목록을 돌려준다(전부 실패보다 낫다).
QStringList bundledRequirements()
{
    QStringList out;
    QStringList cands;
    cands << bundledResourcesDir() + "/requirements.txt"
          << QCoreApplication::applicationDirPath() + "/requirements.txt"
          << QCoreApplication::applicationDirPath() + "/../requirements.txt";
    for (const QString &c : cands) {
        QFile f(c);
        if (!f.exists() || !f.open(QIODevice::ReadOnly | QIODevice::Text)) continue;
        while (!f.atEnd()) {
            QString line = QString::fromUtf8(f.readLine()).trimmed();
            if (line.isEmpty() || line.startsWith('#')) continue;
            out << line;
        }
        f.close();
        if (!out.isEmpty()) {
            qDebug() << "[Common] requirements:" << c << out.size() << "packages";
            return out;
        }
    }
    qWarning() << "[Common] requirements.txt 를 찾지 못했습니다 — 최소 목록으로 진행";
    return {"twikit", "httpx", "atproto", "openpyxl", "Pillow",
            "beautifulsoup4", "websockets", "lxml", "m3u8", "yt-dlp",
            "browser_cookie3", "cryptography"};
}

// pid 로만 끊는다. QProcess 객체는 손대지 않는다 — 남의 스레드 것일 수 있다.
void killProcessByPid(qint64 pid)
{
    if (pid <= 0) return;
#ifdef Q_OS_WIN
    // /T 는 자식까지. 파이썬 데몬이 또 무언가를 띄웠을 수 있다.
    QProcess::execute("taskkill", {"/PID", QString::number(pid), "/F", "/T"});
#else
    ::kill(static_cast<pid_t>(pid), SIGKILL);
#endif
}

// 사용자 쪽 꾸러미 덧씌우기 폴더.
//   ★ 왜 번들을 직접 안 고치나.
//     번들 python_env 에 pip 로 설치하면 codesign 봉인이 깨진다("a sealed resource is
//     missing or invalid"). 그러면 앱이 스스로 재서명해야 하고, 그 과정이 실패하면
//     앱 자체가 안 뜬다. 갱신하려다 앱을 못 쓰게 되는 것은 갱신을 안 하느니만 못하다.
//   → 쓰기 가능한 곳에 새 판을 깔고 PYTHONPATH 로 '먼저' 읽게 한다. 번들은 손대지
//     않으므로 봉인이 유지되고, 덧씌운 것을 지우면 즉시 원래 판으로 돌아온다.
//   (윈도우에는 봉인이 없지만 맥과 같은 자리 · 같은 순서를 쓴다 — 자가수리 · 모듈 갱신이 이것을 부른다.)
QString userPyOverlayDir()
{
    const QString d = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation)
                      + "/pypkgs";
    QDir().mkpath(d);
    return d;
}

QString longPathTemplate(const QString &dir, const QString &tail)
{
    QString t = tail;
    while (t.startsWith(QLatin1Char('/')) || t.startsWith(QLatin1Char('\\'))) t.remove(0, 1);

    const QString d = longPathArg(dir);          // 깊지 않으면 원본 그대로 돌아온다
    QString out = d + QLatin1Char('/') + t;
#ifdef Q_OS_WIN
    // 접두사가 붙었을 때만 구분자를 맞춘다. 안 붙었으면 예전과 한 글자도 다르지 않다.
    if (d.startsWith(QLatin1String("\\\\?\\"))) out = QDir::toNativeSeparators(out);
#endif
    return out;
}

QString longPathArg(const QString &path, int reserveChars)
{
#ifndef Q_OS_WIN
    Q_UNUSED(reserveChars);
    return path;   // 맥·리눅스에는 260자 한계가 없다
#else
    if (path.isEmpty()) return path;
    if (path.startsWith(QLatin1String("\\\\?\\"))) return path;   // 이미 붙어 있다

    // ★ \\?\ 는 '경로 정규화를 끄는' 접두어다. 슬래시(/)도, . 이나 .. 도 안 고쳐
    //   준다 — 커널에 그대로 넘어간다. 그래서 먼저 네이티브 절대경로로 펴야 한다.
    //   이 앱은 내부적으로 슬래시 경로를 쓰므로 이 단계가 없으면 조용히 실패한다.
    const QString abs = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
    const QString n = QDir::toNativeSeparators(abs);

    const bool unc   = n.startsWith(QLatin1String("\\\\"));
    const bool drive = n.size() >= 3 && n.at(1) == QLatin1Char(':')
                                     && n.at(2) == QLatin1Char('\\');
    if (!unc && !drive) return path;   // 붙일 수 없는 모양이면 건드리지 않는다

    // 짧으면 그대로 둔다 — 로그·엑셀·manifest 에 \\?\ 가 새어 나가지 않게.
    //   248 은 폴더 생성 한계(MAX_PATH - 12)다. 파일은 259 까지지만 짧은 쪽에 맞춘다.
    if (n.size() + reserveChars < 248) return path;

    if (unc) return QLatin1String("\\\\?\\UNC\\") + n.mid(2);   // \\nas\share -> \\?\UNC\nas\share
    return QLatin1String("\\\\?\\") + n;
#endif
}

// ★ 맥 bundledProcessEnv 에 윈도우 것을 얹은 하나뿐인 정의다. 아래(maskSecret 뒤)에 남아 있던
//   윈도우 옛 정의는 지운다 — 둘이면 컴파일이 깨진다.
//   맥에서 온 것: 번들 파이썬 경로(HANISHIKI_PYTHON) · 파이썬 경로(덧씌운 꾸러미 > 고침 꾸러미 도우미 >
//   자가수리 도우미 > 번들 도우미) · 프록시 환경변수(출구를 못 얻으면 닫힌 길) · HANISHIKI_APP_NAME.
//   윈도우에서 지킨 것: PYTHONPATH 구분자 ';' · 데이터 폴더의 네이티브 구분자와 PREDORMITION_DATA_DIR ·
//   PYTHONUTF8 / PYTHONIOENCODING.
QProcessEnvironment bundledProcessEnv()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString appDir = QCoreApplication::applicationDirPath();

    // ★ 번들 파이썬 스크립트도 앱과 같은 데이터 폴더를 쓰게 한다.
    //   스크립트가 경로를 스스로 지으면 앱 이름이 바뀔 때마다 갈라진다.
    //   실제로 twitter_daemon.py 는 최초 이름("~/Library/Application Support/カメラ")에
    //   멈춰 있어서, Miyo/ 밑도 아닌 곳에 캐시를 쌓고 있었다(앱을 지워도 남고
    //   백업·정리 도구도 못 본다).
    //
    // ★ 그런데 '이름' 만 넘기는 것으로는 부족했다. 스크립트가 그 이름으로 경로를
    //   다시 조립하는데, 조직 폴더(Miyo/)를 없앤 뒤로 그 조립식이 틀려졌다.
    //   실제로 캐시가 다시 Application Support/Miyo/<앱>/ 에 쌓이고 있었다.
    //   → 조립하지 말고 완성된 경로를 넘긴다. 앱이 실제로 쓰는 그 폴더다.
    // ★ 프록시를 켰으면 자식 프로세스도 전부 같은 길로 내보낸다.
    //   httpx(파이썬 데몬)·yt-dlp·rclone 은 모두 이 표준 환경변수를 읽는다.
    //   명령줄이 아니라 환경으로 주는 이유: 명령줄은 같은 기계의 아무 프로세스나
    //   ps 로 읽지만, 환경은 같은 사용자만 볼 수 있다.
    {
        // 번들 파이썬 경로 — 도구 껍데기(tools/yt-dlp 등)가 어디로 복사돼 불려도 이것을 먼저 쓴다.
        //   앱이 외장 디스크에 있으면 껍데기의 '/Applications 훑기' 가 번들을 못 찾았다.
        {
            const QString py = bundledPythonPath();
            if (!py.isEmpty() && QFileInfo(py).isExecutable()) env.insert("HANISHIKI_PYTHON", py);
        }
        // 파이썬 경로 — 앞에 있을수록 먼저 읽는다.
        //   덧씌운 꾸러미(새 모듈) > 고침 꾸러미 도우미 > 자가수리가 고친 도우미 > 번들 도우미.
        //   ★ 도우미가 옆 파일을 부른다(twitter_daemon→x_session, bluesky_daemon→exif_tags).
        //     도우미를 번들 밖(고침 꾸러미·자가수리 폴더)에서 돌리면 파이썬은 그 폴더만 먼저 보므로
        //     옆 파일을 못 찾았다. 번들 도우미 폴더까지 뒤에 붙여 둔다 — 쓰는 순서는 activeToolScriptPath 와 같다.
        //   ★ 구분자는 QDir::listSeparator() 다(맥 ':' · 윈도우 ';'). 윈도우 파이썬은 PYTHONPATH 를 ';' 로 나눈다 —
        //     맥처럼 ':' 로 이으면 'C:/…' 의 콜론과 섞여 엉뚱한 경로 몇 조각이 되고, 덧씌운 꾸러미 · 고침 꾸러미 ·
        //     번들 도우미가 하나도 안 보인다.
        {
            QStringList parts;
            const QString ov = userPyOverlayDir();
            if (QDir(ov).exists()) parts << ov;
            if (!hotfixTools().isEmpty() && QDir(hotfixToolsDir()).exists()) parts << hotfixToolsDir();
            if (QDir(scriptOverrideDir()).exists()) parts << scriptOverrideDir();
            parts << bundledToolsDir();
            const QString cur = env.value("PYTHONPATH");
            if (!cur.isEmpty()) parts << cur;
            env.insert("PYTHONPATH", parts.join(QDir::listSeparator()));
        }

        QString pu = proxyUrl();
        // ★ 출구가 있어야 하는데 주소를 못 얻었으면(HTTPS 중계기를 못 띄움) 닫힌 길을 준다.
        //   비워 두면 데몬·rclone·curl 이 이 컴퓨터의 회선으로 조용히 나간다.
        if (pu.isEmpty() && proxyEnabled()) pu = QStringLiteral("socks5://127.0.0.1:9");
        if (!pu.isEmpty()) {
            env.insert("ALL_PROXY",   pu);
            env.insert("all_proxy",   pu);
            env.insert("HTTP_PROXY",  pu);
            env.insert("HTTPS_PROXY", pu);
            env.insert("http_proxy",  pu);
            env.insert("https_proxy", pu);
            // 로컬 통신까지 프록시로 보내면 데몬 제어가 끊긴다.
            env.insert("NO_PROXY",    "127.0.0.1,localhost");
            env.insert("no_proxy",    "127.0.0.1,localhost");
        }
    }

    // ★ 파이썬 도구가 자기 데이터를 어디에 둘지 여기서 알려 준다.
    //   맥 쪽은 이미 이 계약(HANISHIKI_DATA_DIR)을 쓰는데 윈도우 사본은 안 받고 있어서,
    //   twitter_daemon.py 가 맥 경로를 그대로 써 C:\Users\<사용자>\Library\
    //   Application Support\カメラ\ 라는 가짜 트리를 사용자 홈에 만들고 있었다
    //   (실측: 이 기계에 그 폴더와 graphql_hashes.json 이 실제로 있었다).
    //   이름도 옛 코드네임이었다. 앱과 같은 자리를 쓰게 한다.
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    env.insert("PREDORMITION_DATA_DIR", QDir::toNativeSeparators(dataDir));
    env.insert("HANISHIKI_DATA_DIR", QDir::toNativeSeparators(dataDir));   // 맥 쪽과 같은 이름도 함께
    env.insert("HANISHIKI_APP_NAME", QStringLiteral(APP_NAME_ASCII));
    env.insert("MIYO_APP_NAME", QStringLiteral(APP_NAME_ASCII));   // 옛 이름 — 아직 읽는 스크립트가 있을 수 있다

#ifdef Q_OS_WIN
    // Windows: add exe dir to PATH for bundled tools
    QString path = appDir + ";" + env.value("PATH");
    env.insert("PATH", path);
#else
    // macOS/Linux: add Frameworks dir to DYLD_LIBRARY_PATH, bundled + homebrew to PATH
    QString frameworksDir = appDir + "/../Frameworks";
    QString existing = env.value("DYLD_LIBRARY_PATH");
    env.insert("DYLD_LIBRARY_PATH", existing.isEmpty() ? frameworksDir : frameworksDir + ":" + existing);
    // ★ 번들 tools 도 PATH 에 — yt-dlp/rclone 이 여기 있다. 이게 빠져 있어서
    //   이름만으로 실행할 때 홈브루 것이 먼저 잡혔다(번들 우선 원칙 위반).
    QString extraPaths = appDir + ":" + bundledToolsDir() + ":/opt/homebrew/bin:/usr/local/bin";
    env.insert("PATH", extraPaths + ":" + env.value("PATH"));
#endif

    // ★ 파이썬 자식은 반드시 UTF-8 로 말하게 한다.
    //   윈도우에서 파이썬 3.14 는 파이프로 나갈 때 시스템 ANSI 코드페이지를 쓴다.
    //   이 기계는 cp1252 라서, 파이썬이 한글·일본어를 print 하는 순간
    //   UnicodeEncodeError 로 데몬이 통째로 죽는다 (실측으로 재현했다).
    //   C++ 쪽은 QString::fromUtf8 로 읽으므로 애초에 UTF-8 이어야 맞다.
    //   지금 주요 데몬이 살아 있는 것은 json.dumps 가 기본값으로 \uXXXX 이스케이프를
    //   하기 때문일 뿐이다 — email_watch.py 처럼 ensure_ascii=False 를 쓰는 곳은 실제로 죽고,
    //   앞으로 누가 한글을 print 하면 또 죽는다. 원인 자체를 없앤다.
    env.insert("PYTHONUTF8", "1");
    env.insert("PYTHONIOENCODING", "utf-8");
    env.insert("PYTHONDONTWRITEBYTECODE", "1");
    return env;
}

// 자격증명은 값 대신 모양만 남긴다. 위 헤더의 설명 참고.
QString maskSecret(const QString &value, const QString &who)
{
    const QString name = who.isEmpty() ? QStringLiteral("값") : who;
    if (value.isEmpty())
        return QStringLiteral("%1: (없음)").arg(name);
    return QStringLiteral("%1: 있음 (%2자, 가려짐)").arg(name).arg(value.length());
}

QProcessEnvironment bundledProcessEnv()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    QString appDir = QCoreApplication::applicationDirPath();

#ifdef Q_OS_WIN
    // Windows: add exe dir to PATH for bundled tools
    QString path = appDir + ";" + env.value("PATH");
    env.insert("PATH", path);
#else
    // macOS/Linux: add Frameworks dir to DYLD_LIBRARY_PATH, bundled + homebrew to PATH
    QString frameworksDir = appDir + "/../Frameworks";
    QString existing = env.value("DYLD_LIBRARY_PATH");
    env.insert("DYLD_LIBRARY_PATH", existing.isEmpty() ? frameworksDir : frameworksDir + ":" + existing);
    QString extraPaths = appDir + ":/opt/homebrew/bin:/usr/local/bin";
    env.insert("PATH", extraPaths + ":" + env.value("PATH"));
#endif

    // ★ 파이썬 도구가 자기 데이터를 어디에 둘지 여기서 알려 준다.
    //   맥 쪽은 이미 이 계약(HANISHIKI_DATA_DIR)을 쓰는데 윈도우 사본은 안 받고 있어서,
    //   twitter_daemon.py 가 맥 경로를 그대로 써 C:\Users\<사용자>\Library\
    //   Application Support\カメラ\ 라는 가짜 트리를 사용자 홈에 만들고 있었다
    //   (실측: 이 기계에 그 폴더와 graphql_hashes.json 이 실제로 있었다).
    //   이름도 옛 코드네임이었다. 앱과 같은 자리를 쓰게 한다.
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    env.insert("PREDORMITION_DATA_DIR", QDir::toNativeSeparators(dataDir));
    env.insert("HANISHIKI_DATA_DIR", QDir::toNativeSeparators(dataDir));   // 맥 쪽과 같은 이름도 함께

    // ★ 파이썬 자식은 반드시 UTF-8 로 말하게 한다.
    //   윈도우에서 파이썬 3.14 는 파이프로 나갈 때 시스템 ANSI 코드페이지를 쓴다.
    //   이 기계는 cp1252 라서, 파이썬이 한글·일본어를 print 하는 순간
    //   UnicodeEncodeError 로 데몬이 통째로 죽는다 (실측으로 재현했다).
    //   C++ 쪽은 QString::fromUtf8 로 읽으므로 애초에 UTF-8 이어야 맞다.
    //   지금 주요 데몬이 살아 있는 것은 json.dumps 가 기본값으로 \uXXXX 이스케이프를
    //   하기 때문일 뿐이다 — email_watch.py 처럼 ensure_ascii=False 를 쓰는 곳은 실제로 죽고,
    //   앞으로 누가 한글을 print 하면 또 죽는다. 원인 자체를 없앤다.
    env.insert("PYTHONUTF8", "1");
    env.insert("PYTHONIOENCODING", "utf-8");
    env.insert("PYTHONDONTWRITEBYTECODE", "1");
    return env;
}

qint64 freeSpace(const QString &path)
{
    if (path.isEmpty()) return 0;
    QStorageInfo si(path);
    if (!si.isValid() || !si.isReady()) return 0;
    return si.bytesAvailable();
}

QString pickSavePath(const QString &primary, const QString &secondary, double thresholdGB)
{
    auto expand = [](const QString &p) {
        QString r = p;
        if (r.startsWith(QLatin1Char('~'))) r.replace(0, 1, QDir::homePath());
        return r;
    };
    QString p1 = expand(primary);
    QString p2 = expand(secondary);
    const qint64 thresholdBytes = static_cast<qint64>(thresholdGB * 1024.0 * 1024.0 * 1024.0);

    // p1이 비어있으면 p2 (있으면), 아니면 빈 문자열
    if (p1.isEmpty()) return p2;
    // p1이 충분하거나 p2가 비어있으면 p1
    if (freeSpace(p1) >= thresholdBytes || p2.isEmpty()) return p1;
    // p2가 충분하면 p2, 아니면 p1 (사용자에게 fallback 노출)
    if (freeSpace(p2) >= thresholdBytes) return p2;
    return p1;
}

QString jsStringLiteral(const QString &s)
{
    QJsonArray a; a.append(s);
    QString j = QString::fromUtf8(QJsonDocument(a).toJson(QJsonDocument::Compact));
    if (j.length() >= 2 && j.startsWith('[') && j.endsWith(']'))
        return j.mid(1, j.length() - 2);
    return "\"\"";
}

QString checkFileIntegrity(const QString &filePath)
{
    QFileInfo fi(filePath);
    if (!fi.exists()) return "파일 없음";
    qint64 size = fi.size();
    if (size == 0) return "0 byte (다운 실패)";

    QString ext = fi.suffix().toLower();
    static const QSet<QString> imgExts = {"jpg","jpeg","png","gif","webp","avif","bmp","tiff","heic"};
    static const QSet<QString> vidExts = {"mp4","mov","webm","mkv","avi","m4v"};

    // 이미지 — QImage 로 load 시도 (빠른 검증)
    if (imgExts.contains(ext)) {
        if (size < 100) return QString("너무 작음 (%1 byte)").arg(size);

        // ★ 디코더가 없는 형식을 '손상' 이라고 부르면 안 된다.
        //   Qt 는 webp/tiff 를 qtimageformats 플러그인으로 읽는데, 그 플러그인이 배포본에
        //   빠져 있으면 QImage::load 가 그냥 실패한다. avif/heic 는 Qt 6.7 에 디코더가
        //   아예 없다. 그런데도 "이미지 디코딩 실패 (손상)" 을 찍으면, 아카이빙 앱에서
        //   멀쩡한 자료를 사용자가 지우게 만든다.
        //   실측: 플러그인 없는 배포본에서 정상 16KB webp 와 TIFF 가 '손상' 으로 나왔다.
        //   읽을 수 없는 형식이면 매직 바이트만 보고 정직하게 보고한다.
        static const QSet<QByteArray> readable = []{
            QSet<QByteArray> s;
            for (const QByteArray &f : QImageReader::supportedImageFormats()) s.insert(f);
            return s;
        }();
        const QByteArray extBa = ext.toLatin1();
        const bool canDecode = readable.contains(extBa)
                            || (ext == "jpg"  && readable.contains("jpeg"))
                            || (ext == "tiff" && readable.contains("tif"));
        if (!canDecode) {
            QFile f(filePath);
            if (!f.open(QIODevice::ReadOnly)) return "파일 열기 실패";
            const QByteArray head = f.read(16);
            f.close();
            if (head.size() < 12) return "헤더 읽기 실패";
            bool magicOk = false;
            if (ext == "webp")
                magicOk = head.startsWith("RIFF") && head.mid(8, 4) == "WEBP";
            else if (ext == "avif" || ext == "heic")
                magicOk = head.mid(4, 4) == "ftyp";
            else if (ext == "tiff" || ext == "tif")
                magicOk = head.startsWith("II*") || head.startsWith("MM");
            else
                magicOk = true;   // 판단 근거가 없으면 손상이라 부르지 않는다
            if (!magicOk) return QString("%1 헤더가 아님 (손상)").arg(ext.toUpper());
            return QString();     // 더 볼 수단이 없다 — 정상으로 둔다
        }

        QImage img;
        if (!img.load(filePath)) return "이미지 디코딩 실패 (손상)";
        if (img.width() < 1 || img.height() < 1) return "이미지 크기 0";
        return QString();  // OK
    }

    // 비디오 — 최소 크기 + magic byte
    if (vidExts.contains(ext)) {
        if (size < 1024) return QString("비디오 너무 작음 (%1 byte)").arg(size);
        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly)) return "파일 열기 실패";
        QByteArray head = f.read(12);
        f.close();
        if (head.size() < 12) return "헤더 읽기 실패";
        // MP4: ftyp box (offset 4)
        if (ext == "mp4" || ext == "m4v" || ext == "mov") {
            if (head.mid(4, 4) != "ftyp") return "MP4 헤더 손상 (ftyp 없음)";
        }
        // WebM/MKV: EBML magic 0x1A 45 DF A3
        if (ext == "webm" || ext == "mkv") {
            if (static_cast<unsigned char>(head[0]) != 0x1A
                || static_cast<unsigned char>(head[1]) != 0x45
                || static_cast<unsigned char>(head[2]) != 0xDF
                || static_cast<unsigned char>(head[3]) != 0xA3) {
                return "WebM/MKV 헤더 손상";
            }
        }
        return QString();
    }

    // HTML — <html 또는 <!DOCTYPE 존재 확인
    if (ext == "html" || ext == "htm") {
        if (size < 100) return QString("HTML 너무 작음 (%1 byte)").arg(size);
        QFile f(filePath);
        if (!f.open(QIODevice::ReadOnly)) return "파일 열기 실패";
        QByteArray head = f.read(8192).toLower();
        f.close();
        if (!head.contains("<html") && !head.contains("<!doctype")) {
            return "HTML 태그 없음 (손상 또는 잘못된 파일)";
        }
        return QString();
    }

    // 기타 — 100 byte 이상이면 OK
    if (size < 100) return QString("너무 작음 (%1 byte)").arg(size);
    return QString();
}

QString resolveTempBase(const QString &userConfigTempDir)
{
    // ★ 사용자 임시 디스크 시스템 — 시스템 /tmp 절대 사용 X.
    //   1순위: 사용자가 설정한 tempDir (외장 SSD / NAS 등)
    //   2순위: AppDataLocation (~/Library/Application Support/Miyo/Predormition/temp)
    //          시스템 /tmp 가 아니라 앱 전용 영구 폴더 — 권한 + 자동 정리 안 됨
    //   ★ /tmp 같은 시스템 temp 는 macOS 가 주기적으로 청소 → 작업 중 파일 사라질 수 있음
    if (!userConfigTempDir.isEmpty() && QDir(userConfigTempDir).exists()) {
        return userConfigTempDir;
    }
    if (!userConfigTempDir.isEmpty()) {
        if (QDir().mkpath(userConfigTempDir)) return userConfigTempDir;
    }
    // Fallback — app data 안 temp (시스템 /tmp 안 씀, 청소 안 됨)
    QString fallback = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/temp";
    QDir().mkpath(fallback);
    return fallback;
}

} // namespace Common
