#include "Config.h"
#include "Common.h"
#include <QJsonDocument>
#include <QDateTime>
#include <algorithm>
#include <QFile>
#include <QFileInfo>
#include <QDir>
#include <QStandardPaths>
#include <QCoreApplication>
#include <QDebug>

QJsonObject AccountInfo::toJson() const
{
    QJsonObject obj;
    if (!name.isEmpty()) obj["name"] = name;
    if (!authToken.isEmpty()) obj["auth_token"] = authToken;
    if (!ct0.isEmpty()) obj["ct0"] = ct0;
    if (!handle.isEmpty()) obj["handle"] = handle;
    if (!password.isEmpty()) obj["password"] = password;
    if (!token.isEmpty()) obj["token"] = token;
    if (!sessionId.isEmpty()) obj["session_id"] = sessionId;
    return obj;
}

AccountInfo AccountInfo::fromJson(const QJsonObject &obj)
{
    AccountInfo info;
    info.name = obj["name"].toString();
    info.authToken = obj["auth_token"].toString();
    info.ct0 = obj["ct0"].toString();
    info.handle = obj["handle"].toString();
    info.password = obj["password"].toString();
    info.token = obj["token"].toString();
    info.sessionId = obj["session_id"].toString();
    return info;
}

Config::Config(QObject *parent)
    : QObject(parent)
{
    m_accounts["twitter"] = QJsonArray();
    m_accounts["bluesky"] = QJsonArray();
    m_accounts["discord"] = QJsonArray();
    m_accounts["instagram"] = QJsonArray();
}

QString Config::defaultConfigPath()
{
    // ★ 외부 user data 위치 — ~/Library/Application Support/Miyo/Chernobyl/miyo_config.json
    //   이전엔 앱 내부(Contents/Resources)에 저장했으나 매 save 시 codesign seal 깨짐
    //   → macOS 보안 정책이 "변조된 앱"으로 판단 → 캡쳐/CDP 등 보안 동작 차단 → 앱 크래시.
    //   외부 저장하면 번들은 read-only 유지 → 서명 유효 → macOS 권한 영구 유지.
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    // ★ 이름을 바꾸되 옛 파일을 잃지 않는다.
    //   그냥 새 이름만 쓰면 설정·계정 토큰이 통째로 사라진 것처럼 보인다.
    //   옛 파일이 있고 새 파일이 없을 때 딱 한 번 옮긴다(rename — 복사가 아니다).
    //   옮기지 못하면(권한 등) 옛 파일을 그대로 쓴다 — 잃는 것보다 낫다.
    const QString cur = dataDir + "/hanishiki_config.json";
    const QString prev = dataDir + "/miyo_config.json";
    // ★ 이 함수는 여러 곳에서 불린다. rename 을 매번 시도하지 않도록 한 번만 판단한다.
    //   (두 스레드가 동시에 들어와도 rename 은 원자적이라 한쪽만 성공하고 나머지는
    //    이미 옮겨진 상태를 본다 — 어느 쪽이든 cur 을 돌려주면 맞다.)
    static bool checked = false;
    static bool useOld  = false;
    if (!checked) {
        checked = true;
        if (!QFileInfo::exists(cur) && QFileInfo::exists(prev)) {
            if (QFile::rename(prev, cur))
                qInfo() << "[config] 설정 파일 이름 옮김: miyo_config.json → hanishiki_config.json";
            else
                useOld = true;   // 옮기지 못하면 옛 파일을 그대로 쓴다 — 잃는 것보다 낫다
        }
    }
    return useOld ? prev : cur;
}

QString Config::backupConfigPath()
{
    // 백업: 같은 디렉토리에 .backup.json 으로 (이전 in-bundle 호환용 path 변환)
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    // 새 이름을 쓰되, 옛 백업이 있고 새것이 없으면 그것을 쓴다.
    //   백업은 설정이 깨졌을 때만 읽히는 최후 수단이라, 이름만 바꿔 놓고
    //   옛 파일을 못 읽으면 정작 필요한 순간에 없는 것이 된다.
    const QString cur = dataDir + "/hanishiki_config.backup.json";
    const QString prev = dataDir + "/miyo_config.backup.json";
    if (!QFileInfo::exists(cur) && QFileInfo::exists(prev)) return prev;
    return cur;
}

// 한 파일을 읽어 JSON 으로 해석해 본다. 없거나 비었거나 깨졌으면 false.
static bool readConfigJson(const QString &path, QJsonObject *out)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return false;
    const QByteArray raw = f.readAll();
    f.close();
    if (raw.trimmed().isEmpty()) return false;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(raw, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) return false;
    *out = doc.object();
    return true;
}

// 설정을 되찾아 올 자리들 — 가까운 것부터.
QStringList Config::recoveryCandidates() const
{
    QStringList c;
    c << backupConfigPath();
    const QString appDir = QCoreApplication::applicationDirPath();
    c << appDir + "/../Resources/miyo_config.json";          // 옛 in-bundle 저장 시절
    const QString sup = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    c << sup + "/ABIWA/miyo_config.json";
    c << QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/ABIWA/miyo_config.json";

    // ★ 앱 이름이 바뀐 뒤의 옛 폴더. 이름을 목록에 박아 두면 '다음 번' 개명 때 또
    //   같은 일이 난다 — 이 앱만 해도 다섯 번째 이름이다. 그래서 형제 폴더를 훑어
    //   설정 파일이 있는 것을 찾고 가장 최근에 쓰인 것부터 시도한다.
    //   설정 파일 이름으로만 거르므로 남의 앱을 잘못 집지 않는다.
    QFileInfoList found;
    const auto dirs = QDir(QFileInfo(sup).absolutePath()).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QFileInfo &d : dirs) {
        if (d.absoluteFilePath() == QFileInfo(sup).absoluteFilePath()) continue;
        QFileInfo cfg(d.absoluteFilePath() + "/hanishiki_config.json");
        if (!cfg.exists()) cfg = QFileInfo(d.absoluteFilePath() + "/miyo_config.json");
        if (cfg.exists() && cfg.size() > 0) found << cfg;
    }
    std::sort(found.begin(), found.end(), [](const QFileInfo &a, const QFileInfo &b) {
        return a.lastModified() > b.lastModified();
    });
    for (const QFileInfo &f : found) c << f.absoluteFilePath();
    return c;
}

void Config::load(const QString &filePath)
{
    m_configPath = filePath.isEmpty() ? defaultConfigPath() : filePath;

    QJsonObject obj;
    if (readConfigJson(m_configPath, &obj)) { fromJson(obj); return; }

    // ★ 여기가 예전에 조용히 데이터를 버리던 자리다.
    //   복구는 '파일이 아예 없을 때' 만 돌았고, 파일이 있는데 깨져 있으면
    //   파싱 실패를 qDebug 한 줄로 흘리고 그냥 돌아갔다. 앱은 빈 설정으로 뜨고,
    //   그 다음 저장이 백업까지 빈 값으로 덮어썼다. 계정·토큰·저장된 입력값이
    //   그렇게 사라진다(실제로 16키→11, 계정 8→4, 입력값 140→0 을 겪었다).
    //   → 깨진 경우도 복구를 돌린다. 그리고 깨진 원본을 지우지 않고 옆으로 치운다.
    if (QFile::exists(m_configPath) && QFileInfo(m_configPath).size() > 0) {
        const QString aside = m_configPath + QStringLiteral(".damaged_%1")
                                                 .arg(QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss"));
        if (QFile::rename(m_configPath, aside))
            qWarning() << "[config] 설정이 깨져 있어 옆으로 옮겼습니다(지우지 않았습니다):" << aside;
        else
            qWarning() << "[config] 설정이 깨져 있는데 옮기지도 못했습니다:" << m_configPath;
    }

    const QStringList cands = recoveryCandidates();
    for (const QString &c : cands) {
        if (!readConfigJson(c, &obj)) continue;      // 후보도 깨졌으면 다음 것
        QDir().mkpath(QFileInfo(m_configPath).absolutePath());
        QFile::copy(c, m_configPath);
        QFile::setPermissions(m_configPath, QFile::ReadOwner | QFile::WriteOwner);
        qInfo() << "[config] 설정을 되찾았습니다:" << c;
        fromJson(obj);
        return;
    }

    qWarning() << "[config] 읽을 수 있는 설정이 없습니다 — 기본값으로 시작합니다:" << m_configPath;
}

void Config::save(const QString &filePath)
{
    QString path = filePath.isEmpty() ? m_configPath : filePath;
    if (path.isEmpty()) path = defaultConfigPath();

    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonDocument doc(toJson());
    QByteArray bytes = doc.toJson(QJsonDocument::Indented);

    // ★ 이 파일에는 NAS 비밀번호·쿠키·토큰이 들어간다. 기본 권한(0644)이면
    //   같은 기기의 다른 계정이나 아무 프로세스나 그대로 읽을 수 있다.
    //   본인만 읽고 쓰게 0600 으로 조인다. (윈도우에서는 ReadOwner/WriteOwner 가
    //   NTFS ACL 로 옮겨지지 않으므로, 그쪽은 사용자 프로필 폴더의 보호에 기댄다.)
    auto lockDown = [](const QString &p) {
        QFile::setPermissions(p, QFile::ReadOwner | QFile::WriteOwner);
    };

    // ★ 쓰는 순서와 방식이 둘 다 중요하다.
    //
    //   방식: 예전엔 QFile 을 WriteOnly 로 열어 썼다. 그 순간 파일이 0바이트로
    //   잘린다. 쓰기가 끝나기 전에 앱이 죽거나 전원이 나가면 남는 것은 잘린
    //   파일이고, 그 안에 있던 계정·토큰·저장된 입력값(140개)이 통째로 사라진다.
    //   writeFileAtomic 은 임시 파일에 다 쓴 뒤 갈아 끼우므로 중간 상태가 없다.
    //
    //   순서: 백업을 먼저 쓴다. 예전엔 주 파일을 먼저 덮어쓰고 백업을 뒤에 썼는데,
    //   그 사이에 죽으면 주 파일은 새것(어쩌면 잘린 것)이고 백업은 옛것도 아닌
    //   어중간한 상태가 된다. 백업이 먼저 온전해지면, 주 파일 쓰기가 어떻게 되든
    //   되돌릴 자리가 항상 하나 남는다.
    if (filePath.isEmpty()) {
        const QString backup = backupConfigPath();
        QString berr;
        if (Common::writeFileAtomic(backup, bytes, &berr)) lockDown(backup);
        else qWarning() << "[config] 백업 저장 실패:" << backup << berr;
    }

    QString err;
    if (Common::writeFileAtomic(path, bytes, &err)) {
        lockDown(path);
    } else {
        // ★ 조용히 넘어가지 않는다. 예전엔 qDebug 한 줄이라 릴리즈에서는 아무
        //   흔적도 없이 설정이 저장되지 않았다.
        qWarning() << "[config] 설정 저장 실패:" << path << err;
    }
}

QJsonArray Config::getAccounts(const QString &platform) const
{
    return m_accounts.value(platform, QJsonArray());
}

void Config::setAccounts(const QString &platform, const QJsonArray &accounts)
{
    m_accounts[platform] = accounts;
}

void Config::addAccount(const QString &platform, const QJsonObject &account)
{
    QJsonArray arr = m_accounts.value(platform, QJsonArray());
    arr.append(account);
    m_accounts[platform] = arr;
}

void Config::removeAccount(const QString &platform, int index)
{
    QJsonArray arr = m_accounts.value(platform, QJsonArray());
    if (index >= 0 && index < arr.size()) {
        arr.removeAt(index);
        m_accounts[platform] = arr;
    }
}

QString Config::tempDir() const { return m_tempDir; }
void Config::setTempDir(const QString &dir) { m_tempDir = dir; }
QString Config::tradCoverPath() const { return m_tradCoverPath; }
void Config::setTradCoverPath(const QString &path) { m_tradCoverPath = path; }
QJsonObject Config::formData() const { return m_formData; }
void Config::setFormData(const QJsonObject &data) { m_formData = data; }

QJsonObject Config::platformTargets() const { return m_platformTargets; }
void Config::setPlatformTargets(const QJsonObject &data) { m_platformTargets = data; }

QJsonObject Config::toJson() const
{
    QJsonObject accounts;
    for (auto it = m_accounts.constBegin(); it != m_accounts.constEnd(); ++it) {
        accounts[it.key()] = it.value();
    }
    QJsonObject root;
    root["accounts"] = accounts;
    if (!m_tempDir.isEmpty()) root["tempDir"] = m_tempDir;
    if (!m_tradCoverPath.isEmpty()) root["tradCoverPath"] = m_tradCoverPath;
    if (!m_formData.isEmpty()) root["formData"] = m_formData;
    if (!m_platformTargets.isEmpty()) root["platformTargets"] = m_platformTargets;
    root["debugLogs"] = m_debugLogs;
    if (!m_secondaryPath.isEmpty()) root["secondaryPath"] = m_secondaryPath;
    if (!m_naikakukaiWatches.isEmpty()) root["naikakukaiWatches"] = m_naikakukaiWatches;
    root["naikakukaiInterval"] = m_naikakukaiInterval;
    if (!m_webdavUrl.isEmpty())  root["webdavUrl"]  = m_webdavUrl;
    if (!m_webdavUser.isEmpty()) root["webdavUser"] = m_webdavUser;
    if (!m_webdavPass.isEmpty()) root["webdavPass"] = m_webdavPass;
    if (!m_sftpKeyFile.isEmpty()) root["sftpKeyFile"] = m_sftpKeyFile;
    if (!m_aiMode.isEmpty())    root["aiMode"]    = m_aiMode;
    if (!m_aiBaseUrl.isEmpty()) root["aiBaseUrl"] = m_aiBaseUrl;
    if (!m_aiApiKey.isEmpty())  root["aiApiKey"]  = m_aiApiKey;
    if (!m_aiModel.isEmpty())   root["aiModel"]   = m_aiModel;
    root["webdavEnabled"] = m_webdavEnabled;
    if (!m_storageMode.isEmpty()) root["storageMode"] = m_storageMode;
    if (!m_storageRoot.isEmpty()) root["storageRoot"] = m_storageRoot;
    root["backupEnabled"] = m_backupEnabled;
    if (!m_backupPath.isEmpty()) root["backupPath"] = m_backupPath;
    root["ytDlpAutoUpdate"] = m_ytDlpAutoUpdate;
    root["firstRunCompleted"] = m_firstRunCompleted;
    root["nasAutoReconnect"] = m_nasAutoReconnect;
    root["naikakukaiResume"] = m_naikakukaiResume;
    root["emailWatchResume"] = m_emailWatchResume;
    root["unixFilenames"] = m_unixFilenames;
    root["maxConcurrent"] = m_maxConcurrent;
    root["windowGeometry"] = m_windowGeometry;
    // ★ 이름 붙은 프록시 프로필(윈도우와 같은 키). 비어 있어도 적는다 — 이 키가 있으면
    //   '새 모양으로 쓴 파일' 이라는 표시가 되어, 아래 옛 키를 다시 옮기지 않는다(fromJson).
    root["proxyProfiles"] = proxyProfiles();   // 잠금을 쥐고 베낀다
    // 옛 키는 읽은 그대로 남긴다 — 옛 판(전역 하나만 아는 판)으로 되돌려도 예전처럼 돌게.
    //   새 판은 이 값을 쓰지 않는다(옮긴 결과는 위 프로필과 계정의 proxy 이름에 있다).
    root["proxyEnabled"] = m_proxyEnabled;
    root["proxyHost"]    = m_proxyHost;
    root["proxyPort"]    = m_proxyPort;
    root["proxyUser"]    = m_proxyUser;
    root["proxyPass"]    = m_proxyPass;
    return root;
}

void Config::fromJson(const QJsonObject &obj)
{
    if (obj.contains("accounts")) {
        QJsonObject accounts = obj["accounts"].toObject();
        for (auto it = accounts.constBegin(); it != accounts.constEnd(); ++it) {
            m_accounts[it.key()] = it.value().toArray();
        }
    }
    // 키가 있을 때만 덮어씀 — JS에서 accounts만 보내도 다른 필드 보존
    if (obj.contains("tempDir")) m_tempDir = obj["tempDir"].toString();
    if (obj.contains("tradCoverPath")) m_tradCoverPath = obj["tradCoverPath"].toString();
    if (obj.contains("formData")) m_formData = obj["formData"].toObject();
    if (obj.contains("platformTargets")) m_platformTargets = obj["platformTargets"].toObject();
    if (obj.contains("debugLogs")) m_debugLogs = obj["debugLogs"].toBool();
    if (obj.contains("secondaryPath")) m_secondaryPath = obj["secondaryPath"].toString();
    if (obj.contains("naikakukaiWatches")) m_naikakukaiWatches = obj["naikakukaiWatches"].toArray();
    if (obj.contains("naikakukaiInterval")) m_naikakukaiInterval = obj["naikakukaiInterval"].toInt(30);
    if (obj.contains("webdavUrl"))      m_webdavUrl  = obj["webdavUrl"].toString();
    if (obj.contains("webdavUser"))     m_webdavUser = obj["webdavUser"].toString();
    if (obj.contains("webdavPass"))     m_webdavPass = obj["webdavPass"].toString();
    if (obj.contains("sftpKeyFile"))    m_sftpKeyFile = obj["sftpKeyFile"].toString();
    if (obj.contains("aiMode"))         m_aiMode    = obj["aiMode"].toString();
    if (obj.contains("aiBaseUrl"))      m_aiBaseUrl = obj["aiBaseUrl"].toString();
    if (obj.contains("aiApiKey"))       m_aiApiKey  = obj["aiApiKey"].toString();
    if (obj.contains("aiModel"))        m_aiModel   = obj["aiModel"].toString();
    if (obj.contains("webdavEnabled"))  m_webdavEnabled = obj["webdavEnabled"].toBool();
    if (obj.contains("storageMode"))    m_storageMode = obj["storageMode"].toString();
    if (obj.contains("storageRoot"))    m_storageRoot = obj["storageRoot"].toString();
    if (obj.contains("backupEnabled"))  m_backupEnabled = obj["backupEnabled"].toBool();
    if (obj.contains("backupPath"))     m_backupPath = obj["backupPath"].toString();
    if (obj.contains("ytDlpAutoUpdate")) m_ytDlpAutoUpdate = obj["ytDlpAutoUpdate"].toBool();
    if (obj.contains("firstRunCompleted")) m_firstRunCompleted = obj["firstRunCompleted"].toBool();
    if (obj.contains("nasAutoReconnect")) m_nasAutoReconnect = obj["nasAutoReconnect"].toBool();
    if (obj.contains("naikakukaiResume")) m_naikakukaiResume = obj["naikakukaiResume"].toBool();
    if (obj.contains("emailWatchResume")) m_emailWatchResume = obj["emailWatchResume"].toBool();
    if (obj.contains("unixFilenames")) m_unixFilenames = obj["unixFilenames"].toBool();
    if (obj.contains("maxConcurrent")) m_maxConcurrent = obj["maxConcurrent"].toInt(0);
    if (obj.contains("windowGeometry")) m_windowGeometry = obj["windowGeometry"].toString();
    if (obj.contains("proxyEnabled")) m_proxyEnabled = obj["proxyEnabled"].toBool(false);
    if (obj.contains("proxyHost"))    m_proxyHost    = obj["proxyHost"].toString();
    if (obj.contains("proxyPort"))    m_proxyPort    = obj["proxyPort"].toInt(1080);
    if (obj.contains("proxyUser"))    m_proxyUser    = obj["proxyUser"].toString();
    if (obj.contains("proxyPass"))    m_proxyPass    = obj["proxyPass"].toString();
    // ★ 키가 있을 때만 덮어쓴다. 화면은 이 키를 보내지 않는다 — 화면은 비밀번호를
    //   갖고 있지 않으므로, 보내면 그 순간 저장된 비밀번호가 전부 지워진다.
    //   (프로필 저장은 HanishikiBackend::setProxyProfiles 한 곳에서만 한다.)
    if (obj.contains("proxyProfiles")) setProxyProfiles(obj["proxyProfiles"].toArray());
    // 옛 모양이면 프로필로 옮긴다. 'proxyHost 는 있는데 proxyProfiles 가 없다' 가 옛 판이 쓴
    //   파일(또는 옛 판에서 내보낸 파일)의 표시다. 화면이 보내는 saveConfig 에는 둘 다 없다.
    //   계정마다 적힌 옛 주소는 그것만 보고 옮긴다(전역 키가 없는 파일이어도).
    migrateLegacyProxy(obj.contains("proxyHost") && !obj.contains("proxyProfiles"));
}

QJsonObject Config::proxyProfileByName(const QString &name) const
{
    if (name.isEmpty()) return QJsonObject();
    QMutexLocker l(&m_proxyMutex);
    for (const QJsonValue &v : m_proxyProfiles) {
        const QJsonObject p = v.toObject();
        if (p["name"].toString() == name) return p;
    }
    return QJsonObject();
}

// 옛 프록시 설정을 이름 붙은 프로필로 옮긴다(윈도우 모델 — 계정마다 이름으로 고른다).
//   ★ 비밀번호는 옮기기만 한다 — 버리지도, 로그에 적지도 않는다. 같은 출구(종류·주소·
//     포트·아이디·비밀번호가 모두 같은 것)는 프로필 하나로 합친다.
//   ★ 계정에 붙어 있던 proxyHost/proxyPort/proxyUser/proxyPass 는 지우고 이름(proxy)만 남긴다.
//     계정 목록은 화면(setConfig)으로 그대로 가고, 예전엔 그 비밀번호가 계정 편집 칸의
//     value 로 DOM 에 박혔다.
//   ★ 예전 '전역 프록시 켬' 은 '모든 요청이 이 길로' 였다. 새 모델에는 앱 전체 출구가 없다 —
//     프로필을 고르지 않은 계정은 직접 연결이다(윈도우와 같다). 그래서 옮길 때 그 길을 타던
//     계정(자기 주소가 따로 없던 계정)에 '기본' 프로필을 직접 붙여, 계정이 나가던 길을 지킨다.
//     계정 없이 도는 요청(유튜브 등)은 이제 직접 연결이다 — 그 사실을 알림으로 남긴다.
void Config::migrateLegacyProxy(bool legacyGlobal)
{
    QMutexLocker lock(&m_proxyMutex);   // 재귀 잠금 — 안에서 proxyProfileByName 이 다시 잠근다
    auto same = [](const QJsonObject &p, const QString &h, int port, const QString &u, const QString &pw) {
        return p["type"].toString(QStringLiteral("socks5")) == QLatin1String("socks5")
            && p["host"].toString() == h && p["port"].toInt() == port
            && p["user"].toString() == u && p["pass"].toString() == pw;
    };
    auto uniqueName = [this](const QString &base) {
        QString n = base; int k = 2;
        while (!proxyProfileByName(n).isEmpty()) n = QStringLiteral("%1 (%2)").arg(base).arg(k++);
        return n;
    };
    // 같은 출구가 이미 있으면 그 이름을, 없으면 새로 만들어 그 이름을 돌려준다.
    auto adopt = [&](const QString &base, const QString &h, int port, const QString &u, const QString &pw) {
        for (const QJsonValue &v : m_proxyProfiles) {
            const QJsonObject p = v.toObject();
            if (same(p, h, port, u, pw)) return p["name"].toString();
        }
        const QString n = uniqueName(base);
        m_proxyProfiles.append(QJsonObject{
            {"provider", h.endsWith(QLatin1String(".nordhold.net")) ? QStringLiteral("nordvpn") : QStringLiteral("other-socks")},
            {"name", n}, {"type", QStringLiteral("socks5")}, {"host", h}, {"port", port},
            {"user", u}, {"pass", pw}});
        return n;
    };

    // 1) 전역 하나 → 프로필 '기본'. 꺼져 있었어도 만든다 — 자격증명을 잃지 않게(붙이지는 않는다).
    QString globalName;
    if (legacyGlobal && !m_proxyHost.trimmed().isEmpty() && m_proxyPort > 0) {
        globalName = adopt(QStringLiteral("기본"), m_proxyHost.trimmed(), m_proxyPort,
                           m_proxyUser, m_proxyPass);
        // 니코동 칸의 '프록시 (VPN) 탭 설정'(global) 은 이제 그 프로필 이름이다.
        if (m_formData.value("niconico-proxy").toString() == QLatin1String("global"))
            m_formData["niconico-proxy"] = globalName;
    }
    const bool globalWasOn = legacyGlobal && m_proxyEnabled && !globalName.isEmpty();

    // 2) 계정마다 적혀 있던 주소 → 프로필 이름. 자기 주소가 없던 계정은 전역이 켜져 있었으면
    //    '기본' 을 붙인다(예전에 그 길로 나갔다). 이미 이름이 있는 계정은 건드리지 않는다.
    int perAccount = 0, viaGlobal = 0;
    for (auto it = m_accounts.begin(); it != m_accounts.end(); ++it) {
        QJsonArray arr = it.value();
        bool touched = false;
        for (int i = 0; i < arr.size(); ++i) {
            QJsonObject a = arr[i].toObject();
            const bool legacyKeys = a.contains("proxyHost") || a.contains("proxyPort")
                                 || a.contains("proxyUser") || a.contains("proxyPass");
            const QString h = a.value("proxyHost").toString().trimmed();
            // 옛 주소가 있고, 이름이 없거나 그 이름의 프로필이 없으면(옛 판으로 되돌렸다가 다시
            //   올라온 경우 — 옛 판은 프로필 목록을 지운다) 옛 주소를 프로필로 다시 들인다.
            const QString named = a.value("proxy").toString();
            if (!h.isEmpty() && (named.isEmpty() || proxyProfileByName(named).isEmpty())) {
                const int port = a.value("proxyPort").toInt() > 0 ? a.value("proxyPort").toInt() : 1080;
                a["proxy"] = adopt(QStringLiteral("%1:%2").arg(h).arg(port), h, port,
                                   a.value("proxyUser").toString(), a.value("proxyPass").toString());
                ++perAccount;
            } else if (globalWasOn && a.value("proxy").toString().isEmpty()) {
                a["proxy"] = globalName;
                ++viaGlobal;
            } else if (!legacyKeys) {
                continue;
            }
            a.remove("proxyHost"); a.remove("proxyPort"); a.remove("proxyUser"); a.remove("proxyPass");
            arr[i] = a; touched = true;
        }
        if (touched) it.value() = arr;
    }
    if (perAccount || viaGlobal || !globalName.isEmpty()) {
        m_proxyMigrationNote = QStringLiteral("예전 프록시 설정을 이름 붙은 프로필로 옮겼습니다 — 프로필 %1개, "
                                              "계정별 주소 %2개, 전역 프록시를 타던 계정 %3개")
                                   .arg(m_proxyProfiles.size()).arg(perAccount).arg(viaGlobal);
        if (globalWasOn)
            m_proxyMigrationNote += QStringLiteral(". 계정 없이 도는 요청(유튜브 등)은 이제 직접 연결입니다 — "
                                                   "필요하면 니코동 칸처럼 출구를 골라 주십시오");
    }
}
