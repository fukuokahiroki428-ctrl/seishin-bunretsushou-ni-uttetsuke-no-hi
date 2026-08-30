#include "Config.h"
#include <QDateTime>
#include "Common.h"
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
    // ★ 외부 user data 위치 — ~/Library/Application Support/Miyo/Predormition/miyo_config.json
    //   이전엔 앱 내부(Contents/Resources)에 저장했으나 매 save 시 codesign seal 깨짐
    //   → macOS 보안 정책이 "변조된 앱"으로 판단 → 캡쳐/CDP 등 보안 동작 차단 → 앱 크래시.
    //   외부 저장하면 번들은 read-only 유지 → 서명 유효 → macOS 권한 영구 유지.
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    return dataDir + "/miyo_config.json";
}

QString Config::backupConfigPath()
{
    // 백업: 같은 디렉토리에 .backup.json 으로 (이전 in-bundle 호환용 path 변환)
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    return dataDir + "/miyo_config.backup.json";
}

// 읽어서 '쓸 수 있는 설정인지' 까지 본다.
//   파일이 있다는 것과 읽을 수 있다는 것은 다르다. 0바이트여도 파일은 있다.
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
    c << backupConfigPath();                       // 백업 (우선)
    // ★ 옛 in-bundle config (예전엔 번들 안에 저장했다 — 마이그레이션용)
    c << QCoreApplication::applicationDirPath() + "/../Resources/miyo_config.json";
    const QString sup = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    c << sup + "/ABIWA/miyo_config.json";
    c << QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/ABIWA/miyo_config.json";

    // ★ 앱 이름이 바뀐 뒤의 옛 폴더 — 이게 없어서 이름을 바꿀 때마다 설정이 통째로
    //   버려졌다. 이 앱만 해도 カメラ → チェルノブイリ → Chernobyl → Predormition
    //   으로 네 번 바뀌었다. AppDataLocation 은 .../Miyo/<앱이름> 이라, 이름이 바뀌면
    //   폴더도 바뀌어 계정·쿠키·NAS 설정이 한 번에 사라진 것처럼 보인다.
    //   이름을 목록에 박아 두면 '다음 번' 이름 변경 때 또 같은 일이 난다. 그래서
    //   형제 폴더를 훑어 설정 파일이 있는 것을 찾고 최근에 쓴 것부터 시도한다.
    //   설정 파일 이름으로만 거르므로 남의 앱을 잘못 집지 않는다.
    {
        QDir siblings(QFileInfo(sup).absolutePath());   // .../Miyo
        QFileInfoList found;
        const auto dirs = siblings.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot);
        for (const QFileInfo &d : dirs) {
            if (d.absoluteFilePath() == QFileInfo(sup).absoluteFilePath()) continue;   // 내 폴더
            QFileInfo cfg(d.absoluteFilePath() + "/miyo_config.json");
            if (!cfg.exists()) cfg = QFileInfo(d.absoluteFilePath() + "/hanishiki_config.json");
            if (cfg.exists() && cfg.size() > 0) found << cfg;
        }
        std::sort(found.begin(), found.end(), [](const QFileInfo &a, const QFileInfo &b) {
            return a.lastModified() > b.lastModified();   // 최근에 쓰던 것부터
        });
        for (const QFileInfo &f : found) c << f.absoluteFilePath();
    }
    return c;
}

void Config::load(const QString &filePath)
{
    m_configPath = filePath.isEmpty() ? defaultConfigPath() : filePath;

    QJsonObject obj;
    if (readConfigJson(m_configPath, &obj)) { fromJson(obj); return; }

    // ★ 여기가 조용히 데이터를 버리던 자리다.
    //
    //   예전 윈도우판은 복구를 '파일이 아예 없을 때' 만 돌렸다. 파일이 있는데
    //   깨져 있으면 파싱 실패를 qDebug 한 줄로 흘리고 그냥 돌아갔다. 앱은 빈
    //   설정으로 뜨고, 그 다음 저장이 백업까지 빈 값으로 덮어썼다 — 계정·토큰·
    //   저장해 둔 입력값이 그렇게 사라진다. (맥판에서 실제로 겪은 일이다:
    //   16키→11, 계정 8→4, 입력값 140→0.)
    //
    //   저장을 원자적으로 바꾸면서 백업을 먼저 쓰게 했으므로, 이걸 안 고치면
    //   오히려 더 빨리 잃는다. 두 변경은 같이 가야 한다.
    //
    //   → 깨진 경우에도 복구를 돌린다. 그리고 깨진 원본은 지우지 않고 옆으로
    //     치운다. 사람이 나중에 열어 볼 수 있어야 한다.
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

    // ★ 쓰는 방식과 순서가 둘 다 중요하다.
    //
    //   방식: 예전엔 QFile 을 WriteOnly 로 열어 썼다. 그 순간 파일이 0바이트로
    //   잘린다. 쓰기가 끝나기 전에 앱이 죽거나 전원이 나가면 남는 것은 잘린
    //   파일이고, 그 안에 있던 계정·토큰·저장해 둔 입력값이 통째로 사라진다.
    //   writeFileAtomic 은 임시 파일에 다 쓴 뒤 갈아 끼우므로 중간 상태가 없다.
    //
    //   순서: 백업을 먼저 쓴다. 예전엔 주 파일을 먼저 덮고 백업을 뒤에 썼는데,
    //   그 사이에 죽으면 주 파일은 새것(어쩌면 잘린 것)이고 백업은 옛것도 아닌
    //   어중간한 상태가 된다. 백업이 먼저 온전해지면 되돌릴 자리가 항상 남는다.
    //
    //   (맥판은 이미 이렇게 하고 있었다. 윈도우판만 옛 방식으로 남아 있었다.)
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
        // ★ 조용히 넘어가지 않는다. 예전엔 qDebug 한 줄이라 릴리즈 빌드에서는
        //   아무 흔적도 없이 설정이 저장되지 않았다.
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
    if (!m_proxyProfiles.isEmpty()) root["proxyProfiles"] = m_proxyProfiles;
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
    root["unixFilenames"] = m_unixFilenames;
    root["maxConcurrent"] = m_maxConcurrent;
    root["windowGeometry"] = m_windowGeometry;
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
    if (obj.contains("proxyProfiles")) m_proxyProfiles = obj["proxyProfiles"].toArray();
    if (obj.contains("naikakukaiWatches")) m_naikakukaiWatches = obj["naikakukaiWatches"].toArray();
    if (obj.contains("naikakukaiInterval")) m_naikakukaiInterval = obj["naikakukaiInterval"].toInt(30);
    if (obj.contains("webdavUrl"))      m_webdavUrl  = obj["webdavUrl"].toString();
    if (obj.contains("webdavUser"))     m_webdavUser = obj["webdavUser"].toString();
    if (obj.contains("webdavPass"))     m_webdavPass = obj["webdavPass"].toString();
    if (obj.contains("aiMode"))         m_aiMode     = obj["aiMode"].toString();
    if (obj.contains("aiBaseUrl"))      m_aiBaseUrl  = obj["aiBaseUrl"].toString();
    if (obj.contains("aiApiKey"))       m_aiApiKey   = obj["aiApiKey"].toString();
    if (obj.contains("aiModel"))        m_aiModel    = obj["aiModel"].toString();
    if (obj.contains("sftpKeyFile"))    m_sftpKeyFile = obj["sftpKeyFile"].toString();
    if (obj.contains("webdavEnabled"))  m_webdavEnabled = obj["webdavEnabled"].toBool();
    if (obj.contains("storageMode"))    m_storageMode = obj["storageMode"].toString();
    if (obj.contains("storageRoot"))    m_storageRoot = obj["storageRoot"].toString();
    if (obj.contains("backupEnabled"))  m_backupEnabled = obj["backupEnabled"].toBool();
    if (obj.contains("backupPath"))     m_backupPath = obj["backupPath"].toString();
    if (obj.contains("ytDlpAutoUpdate")) m_ytDlpAutoUpdate = obj["ytDlpAutoUpdate"].toBool();
    if (obj.contains("firstRunCompleted")) m_firstRunCompleted = obj["firstRunCompleted"].toBool();
    if (obj.contains("nasAutoReconnect")) m_nasAutoReconnect = obj["nasAutoReconnect"].toBool();
    if (obj.contains("unixFilenames")) m_unixFilenames = obj["unixFilenames"].toBool();
    if (obj.contains("maxConcurrent")) m_maxConcurrent = obj["maxConcurrent"].toInt(0);
    if (obj.contains("windowGeometry")) m_windowGeometry = obj["windowGeometry"].toString();
}
