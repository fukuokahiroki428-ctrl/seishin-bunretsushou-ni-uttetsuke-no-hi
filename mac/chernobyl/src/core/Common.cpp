#include "Common.h"
#include <QLockFile>
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
#include <QSaveFile>
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
// ANSI 로 안전한 경로 (Windows 전용 — 맥에서는 no-op)
// ═════════════════════════════════════════════════════════════════════════
// exiftool.exe 는 PAR 로 묶인 perl 실행 파일이라 argv 를 시스템 ANSI 코드페이지로 받는다.
// 경로에 그 코드페이지로 표현할 수 없는 문자가 있으면 '?' 로 뭉개져, 자기 옆의
// exiftool_files\perl5*.dll 도 대상 파일도 찾지 못하고 종료 코드 1 로 죽는다.
// 설치 기본 경로가 {localappdata}\Programs\Predormition 이라 Windows 사용자 이름이
// 한글·일본어면 일반 사용자 환경에서 그대로 재현된다.
// 맥은 argv 가 UTF-8 이라 이 문제가 없다. 두 트리를 같은 모양으로 두기 위해 함께 둔다.
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
    args << imagePath;

    // exiftool 경로 탐색 (번들 → homebrew → system)
    static QString exiftoolPath;
    static QString exiftoolPerl;  // 번들 exiftool용 Perl 인터프리터
    static QString exiftoolPerlLib;
    static QString exiftoolPerlCoreLib;  // ★ 번들 Perl 의 코어 @INC (자체완결 perl 사용 시)
    if (exiftoolPath.isEmpty()) {
        // 번들된 exiftool (Resources/tools/exiftool/exiftool)
        QString bundledExiftool = bundledResourcesDir() + "/tools/exiftool/exiftool";
        qDebug() << "[Common] exiftool probe:" << bundledExiftool
                 << "exists=" << QFile::exists(bundledExiftool);
        if (QFile::exists(bundledExiftool)) {
            exiftoolPath = bundledExiftool;
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
        // ★ Windows: 인자를 UTF-8 argfile 로 넘긴다. exiftool.exe 는 argv 를 시스템 ANSI
        //   코드페이지로 받으므로 그대로 넘기면 일본어·한글 값과 경로가 '?' 로 뭉개진다.
        //   실행 파일 경로만 8.3 로 넘긴다(자기 exiftool_files 를 찾아야 하므로).
        //   맥은 argv 가 UTF-8 이고 perl 경유라 이 경로를 타지 않는다.
        QTemporaryFile argFile(QDir::tempPath() + "/" + QStringLiteral(APP_NAME_ASCII).toLower() + "_exif_XXXXXX.args");
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
        proc.start(ansiSafePath(exiftoolPath), {"-@", ansiSafePath(argFile.fileName())});
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


QString activePythonEnvDir()
{
#ifdef Q_OS_MACOS
    // ★★ 앱 내부(번들) 고정 — 외부 복사본은 쓰지 않는다.
    //
    //   예전엔 '번들이 쓰기가능하면 번들, 아니면 외부로 복사해서 그걸 쓴다' 였다.
    //   그런데 그 두 갈래가 층마다 다르게 가정돼 서로 어긋나 있었다. 실제로
    //   upgradePythonEnv() 는 "외부가 아니면 중단" 이라 번들을 쓰는 지금 상태에서는
    //   Python 업그레이드가 '항상' 중단됐다. 갈래가 둘이면 이런 어긋남이 계속 난다.
    //
    //   번들에 쓰면 codesign 봉인이 깨지지만, 설치 직후 resealAppBundle() 이 다시
    //   서명해 복구한다(인증서가 있으면 그것으로, 없으면 ad-hoc — 둘 다 실제로
    //   검증까지 통과하는 것을 확인했다).
    //
    //   번들이 읽기전용이면 예전처럼 몰래 외부로 새지 않는다. 그대로 번들을
    //   돌려주고, 설치·업그레이드 쪽에서 '쓸 수 없다' 고 분명히 알린다.
    //   조용히 다른 곳을 쓰는 것보다 안 되는 이유를 말하는 편이 낫다.
    QString bundled = bundledResourcesDir() + "/python_env" KAMERA_PY_ARCH;
    if (!QFile::exists(bundled + "/bin/python3"))
        bundled = bundledResourcesDir() + "/python_env";   // 구 번들/단일 arch 폴백
    return bundled;
#elif defined(Q_OS_WIN)
    return bundledResourcesDir() + "/python_env";
#else
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

    // 번들 원본 찾기.
    //   tools/ 바로 밑에만 있는 게 아니다 — CMake 는 보관함 스크립트를
    //   tools/archive/ 하위에 넣는다(archive_ask.py, archive_index.py).
    //   예전엔 tools/ 만 봐서 그 둘의 원본을 못 찾았고, AI 자가수리 화면이
    //   빈 편집기를 띄웠다(내용이 빈 문자열로 왔다). 하위 폴더도 본다.
    const QString tools = bundledToolsDir();
    const QStringList subs = { QString(), QStringLiteral("/archive") };
    for (const QString &sub : subs) {
        const QString p = tools + sub + "/" + name;
        if (QFileInfo::exists(p)) return p;
    }
    // 못 찾으면 예전과 같은 경로를 돌려준다(호출부가 존재 여부를 다시 확인한다).
    return tools + "/" + name;
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

bool writeFileAtomic(const QString &path, const QByteArray &bytes, QString *err)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    // ★ write 의 반환값을 본다. 디스크가 꽉 차면 open 은 성공하고 write 만 모자라게
    //   쓰는데, 예전 코드는 그것을 무시했다 → 반쪽짜리 설정이 조용히 남았다.
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
    if (!writeFileAtomic(p, QJsonDocument(o).toJson(QJsonDocument::Indented))) return false;
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
    // <...>/Chernobyl.app/Contents/MacOS  →  <...>/Chernobyl.app
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

    // ★ 번들에 동봉한 codesign_app.sh 로 재서명한다.
    //   빌드가 쓰는 바로 그 스크립트다 — inside-out 으로 컴포넌트부터 서명하고
    //   마지막에 --deep --strict 로 검증한다. 인증서 선택(Developer ID → Apple
    //   Development → ad-hoc)과 entitlements 처리도 그 안에 이미 들어 있다.
    //
    //   예전엔 여기서 `codesign -f -s <id> --deep` 로 직접 서명했다. 동작은 했지만
    //   빌드는 --deep 서명을 일부러 피하는데(애플도 배포용이 아니라고 한다) 런타임만
    //   다른 방식을 쓰고 있었다. 두 갈래로 두면 언젠가 결과가 갈린다.
    const QString signer = bundledToolsDir() + "/codesign_app.sh";
    if (!QFile::exists(signer)) {
        if (err) *err = QString("재서명 도구가 번들에 없습니다: %1").arg(signer);
        return false;
    }

    // ★ 재서명은 한 번에 하나만. 같은 번들에 codesign 두 개가 동시에 붙으면
    //   서로의 중간 결과를 덮어써서 번들이 확실히 망가진다. 앱을 두 개 띄우거나,
    //   앞선 재서명이 아직 끝나지 않았는데 또 시작하는 경우가 실제로 있다.
    //   잠금은 번들 밖(임시 폴더)에 둔다 — 번들 안에 파일을 만들면 그것이 또 봉인을 깬다.
    QLockFile lock(QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                   + "/hanishiki_reseal.lock");
    lock.setStaleLockTime(30 * 60 * 1000);      // 30분이면 죽은 잠금으로 본다
    if (!lock.tryLock(0)) {
        if (err) *err = "다른 재서명이 이미 진행 중입니다 — 그것이 끝나기를 기다리십시오.";
        return false;
    }

    QProcess cs;
    cs.start("/bin/bash", {signer, app});
    if (!cs.waitForFinished(1800000)) {         // 번들이 크면(모델 포함) 수 분 걸린다
        cs.kill(); cs.waitForFinished(3000);
        if (err) *err = "재서명 시간 초과";
        return false;
    }
    if (cs.exitCode() != 0) {
        // 스크립트가 실패 사유를 표준출력에도 적는다(개별 서명 실패 목록 등).
        const QString out = QString::fromUtf8(cs.readAllStandardOutput()).right(400);
        const QString e   = QString::fromUtf8(cs.readAllStandardError()).left(300);
        if (err) *err = (e.isEmpty() ? out : e);
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

    if (!autoUpdate) return;   // 꺼 두면 확인조차 하지 않는다

    // ── 2) 새 판이 있는지 '확인만' 한다 ────────────────────────────────────
    //
    // ★ 예전엔 여기서 `yt-dlp --update-to stable` 을 불렀다. 그것은 이 앱에서
    //   구조적으로 절대 성공할 수 없는 호출이었다.
    //     · 번들 yt-dlp 는 단일 실행파일이 아니라 번들 파이썬의 패키지다.
    //       tools/yt-dlp 는 `python -m yt_dlp` 로 넘기는 1.3KB 셸 래퍼일 뿐이다.
    //     · 그래서 yt-dlp 의 자체 갱신기는 variant 를 unknown 으로 보고
    //       "manual build 나 패키지 관리자로 설치했으니 그쪽으로 갱신하라" 며 거부한다.
    //   그런데 그 뒤의 sanity check 는 `--version` 이 여전히 잘 나오니 통과했고,
    //   실패를 알리는 곳도 없었다. 결과: 장기지원의 핵심 장치가 몇 달 동안
    //   아무 일도 하지 않으면서 '정상' 으로 보였다.
    //
    // ★ 그러면 왜 여기서 바로 pip 로 올리지 않나.
    //   패키지가 앱 번들 안에 있어서, 올리는 순간 codesign 봉인이 깨진다.
    //   그러면 다음 기동 때 자동 재서명(1분 남짓)이 돌고, 그 동안 앱을 끄면
    //   번들이 무효로 남는다. 기동할 때마다 조용히 그 위험을 감수할 일이 아니다.
    //   → 확인은 자동으로, 적용은 사용자가 '모듈 업데이트' 를 눌러서.
    //     확인은 하루 한 번이면 충분하다.
    const QString stampPath =
        QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/ytdlp_check.stamp";
    {
        QFileInfo st(stampPath);
        if (st.exists() && st.lastModified().secsTo(QDateTime::currentDateTime()) < 24 * 3600)
            return;                     // 오늘 이미 확인했다
    }

    const QString python = bundledPythonPath();
    if (python.isEmpty() || !QFile::exists(python)) return;

    QProcess *p = new QProcess();
    p->setProcessEnvironment(bundledProcessEnv());
    p->setProgram(python);
    // --dry-run 이라 아무것도 설치하지 않는다. 봉인도 그대로다.
    p->setArguments({"-m", "pip", "install", "--upgrade", "--no-input",
                     "--dry-run", "--quiet", "--report", "-", "yt-dlp"});
    QObject::connect(p, QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        p, [p, stampPath](int, QProcess::ExitStatus) {
            const QString out = QString::fromUtf8(p->readAllStandardOutput());
            // 올릴 것이 있으면 리포트에 yt-dlp 가 '설치 대상' 으로 들어온다.
            static const QRegularExpression verRe(R"RX("name":\s*"yt[-_]dlp".*?"version":\s*"([^"]+)")RX",
                                                  QRegularExpression::DotMatchesEverythingOption);
            const auto m = verRe.match(out);
            if (m.hasMatch()) {
                qInfo().noquote()
                    << QString("[yt-dlp] 새 판이 있습니다: %1 — 설정 → '모듈 업데이트' 를 누르면 올라갑니다.")
                           .arg(m.captured(1));
            }
            QFile stamp(stampPath);
            if (stamp.open(QIODevice::WriteOnly | QIODevice::Text)) { stamp.write("ok"); stamp.close(); }
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
QStringList bundledRequirements(bool stripPins)
{
    // 'pkg[extra]==1.2.3 ; marker' → 'pkg[extra]'
    auto nameOnly = [](const QString &line) {
        static const QRegularExpression re(R"(^([A-Za-z0-9._\-]+(?:\[[^\]]+\])?))");
        const auto m = re.match(line);
        return m.hasMatch() ? m.captured(1) : line;
    };
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
            out << (stripPins ? nameOnly(line) : line);
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
    env.insert("HANISHIKI_DATA_DIR",
               QStandardPaths::writableLocation(QStandardPaths::AppDataLocation));
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
    //   2순위: AppDataLocation (~/Library/Application Support/Miyo/Chernobyl/temp)
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
