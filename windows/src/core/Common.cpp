#include "Common.h"
#include <QProcess>
#include <QProcessEnvironment>
#include <QTimeZone>
#include <QLocale>
#include <QFile>
#include <QDir>
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
#include <QMutex>
#include <QRegularExpression>
#include <QJsonDocument>
#include <QJsonArray>
#include <QImage>
#include <QSet>
#include <QVarLengthArray>
#include <QTemporaryFile>
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
    int ret = _wutime(filePath.toStdWString().c_str(), &times);
    if (ret != 0) {
        qWarning() << "[setFileTimes] _wutime failed for:" << filePath;
    }

    // Also set creation time via SetFileTime API
    HANDLE hFile = CreateFileW(filePath.toStdWString().c_str(),
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
static QString asciiSafeExiftool(const QString &exePath)
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

QString ansiSafePath(const QString &path)
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
        qWarning() << "[Common] 경로에 ANSI 로 표현 못 하는 문자가 있는데 8.3 단축 경로도 없습니다."
                   << "exiftool 이 실패할 수 있습니다:" << path;
        return path;
    }
    return shortPath;
#else
    return path;
#endif
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

    // exiftool 경로 탐색 (번들 → homebrew → system)
    static QString exiftoolPath;
    static QString exiftoolPerl;  // 번들 exiftool용 Perl 인터프리터
    static QString exiftoolPerlLib;
    static QString exiftoolPerlCoreLib;  // ★ 번들 Perl 의 코어 @INC (자체완결 perl 사용 시)
    if (exiftoolPath.isEmpty()) {
        // 번들된 exiftool (Resources/tools/exiftool/exiftool)
        // ★ Windows 배포본은 perl 스크립트가 아니라 단독 실행 파일(exiftool.exe)을 쓴다.
        //   유닉스용 perl 스크립트를 먼저 집으면 perl 이 없는 PC 에서 메타데이터가 통째로 실패한다.
#ifdef Q_OS_WIN
        QString bundledExiftool = bundledResourcesDir() + "/tools/exiftool/exiftool.exe";
        if (!QFile::exists(bundledExiftool))
            bundledExiftool = QCoreApplication::applicationDirPath() + "/exiftool.exe";
#else
        QString bundledExiftool = bundledResourcesDir() + "/tools/exiftool/exiftool";
#endif
        qDebug() << "[Common] exiftool probe:" << bundledExiftool
                 << "exists=" << QFile::exists(bundledExiftool);
        if (QFile::exists(bundledExiftool)) {
            exiftoolPath = bundledExiftool;
#ifdef Q_OS_WIN
            // exiftool.exe 는 perl 을 내장한 단독 실행 파일 — perl 인터프리터/@INC 설정이 필요 없다.
            qDebug() << "[Common] bundled exiftool.exe:" << exiftoolPath;
#else
            exiftoolPerlLib = bundledResourcesDir() + "/tools/exiftool/lib/perl5";
            // ★ 번들 Perl 우선 (자체완결 — 시스템 perl 없어도 동작). 없으면 시스템 perl 폴백.
            QString bundledPerl = bundledResourcesDir() + "/tools/perl/bin/perl";
            if (QFile::exists(bundledPerl)) {
                exiftoolPerl = bundledPerl;
                exiftoolPerlCoreLib = bundledResourcesDir() + "/tools/perl/lib";  // 번들 perl 코어 @INC
            } else {
                QStringList perls = {"/usr/bin/perl", "/usr/bin/perl5.34", "/usr/bin/perl5.30"};
                for (const QString &p : perls) {
                    if (QFile::exists(p)) { exiftoolPerl = p; break; }
                }
                if (exiftoolPerl.isEmpty()) exiftoolPerl = "perl";
            }
            qDebug() << "[Common] bundled exiftool:" << exiftoolPath
                     << "perl:" << exiftoolPerl << "lib:" << exiftoolPerlLib;
#endif
        } else {
            QStringList candidates = {
#ifdef Q_OS_WIN
                QCoreApplication::applicationDirPath() + "/exiftool.exe",
#endif
                "/opt/homebrew/bin/exiftool",
                "/usr/local/bin/exiftool",
                "/usr/bin/exiftool",
            };
            for (const QString &c : candidates) {
                if (QFile::exists(c)) { exiftoolPath = c; break; }
            }
            if (exiftoolPath.isEmpty()) exiftoolPath = "exiftool";
            qDebug() << "[Common] system exiftool:" << exiftoolPath;
        }
    }

    QProcess proc;
    proc.setProcessEnvironment(bundledProcessEnv());
#ifdef Q_OS_WIN
    // ★ argfile 은 exiftool 이 다 읽을 때까지 살아 있어야 한다.
    //   전에는 아래 else 블록 안에서 만들어서, 블록이 끝나는 순간 소멸자가
    //   (setAutoRemove(true) 때문에) 파일을 지워 버렸다. proc.start() 는 비동기라
    //   exiftool 이 뜨기도 전에 파일이 사라져서 "Error opening arg file" 로 매번 실패했다.
    //   실측: 이 기계의 로그에 1668건 전부 그 오류. EXIF 가 하나도 안 쓰이고 있었다.
    //   여기서 만들면 함수가 끝날 때까지 — waitForFinished 이후까지 — 살아 있다.
    QTemporaryFile argFile(QDir::tempPath() + "/predormition_exif_XXXXXX.args");
#endif
    if (!exiftoolPerl.isEmpty()) {
        // 번들 exiftool: perl -I<lib> exiftool <args>
        QStringList perlArgs;
        // ★ 번들 perl 코어 @INC (arch lib + base) 먼저 — 시스템 /System/Library/Perl 없이도 동작.
        if (!exiftoolPerlCoreLib.isEmpty()) {
            perlArgs << "-I" + exiftoolPerlCoreLib + "/darwin-thread-multi-2level";
            perlArgs << "-I" + exiftoolPerlCoreLib;
        }
        if (!exiftoolPerlLib.isEmpty())
            perlArgs << "-I" + exiftoolPerlLib;
        perlArgs << exiftoolPath;
        perlArgs.append(args);
        proc.start(exiftoolPerl, perlArgs);
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
            for (const QString &a : args) ts << a << "\n";
        }
        argFile.close();
        // argfile 자체는 exiftool 이 읽기만 하므로 8.3 로 넘겨도 안전하다.
        proc.start(asciiSafeExiftool(exiftoolPath), {"-@", ansiSafePath(argFile.fileName())});
#else
        proc.start(exiftoolPath, args);
#endif
    }
    if (!proc.waitForStarted(3000)) {
        if (exiftoolAvailable == -1) {
            qWarning() << "[Common] exiftool not found — EXIF metadata disabled"
                       << "path:" << exiftoolPath << "perl:" << exiftoolPerl
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
    const QJsonObject o = loadApiOverrides();
    const QString v = o.value(key).toString();
    return v.isEmpty() ? builtinDefault : v;
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
        if (err) *err = QString::fromUtf8(cs.readAllStandardError()).left(300);
        return false;
    }

    QProcess vf;
    vf.start("/usr/bin/codesign", {"--verify", "--deep", "--strict", app});
    vf.waitForFinished(900000);
    if (vf.exitCode() != 0) {
        if (err) *err = "재서명 후 검증 실패: " + QString::fromUtf8(vf.readAllStandardError()).left(300);
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
    bool needCopy = !QFile::exists(userBin) || QFileInfo(userBin).size() < 1000000;
    if (needCopy && !bundled.isEmpty()) {
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
    list << "python" << "python3";
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
    return {"twikit", "httpx", "atproto", "openpyxl", "Pillow", "piexif",
            "beautifulsoup4", "websockets", "lxml", "m3u8", "yt-dlp",
            "browser_cookie3", "cryptography"};
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
