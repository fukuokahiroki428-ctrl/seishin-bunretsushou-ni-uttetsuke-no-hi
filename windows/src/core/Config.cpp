#include "Config.h"
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
    return dataDir + "/miyo_config.json";
}

QString Config::backupConfigPath()
{
    // 백업: 같은 디렉토리에 .backup.json 으로 (이전 in-bundle 호환용 path 변환)
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    return dataDir + "/miyo_config.backup.json";
}

void Config::load(const QString &filePath)
{
    m_configPath = filePath.isEmpty() ? defaultConfigPath() : filePath;

    // ★ 자동 복원 — 앱 내부 config 없으면 외부 백업/옛 위치에서 복원
    //   재빌드로 앱 내부 사라져도 사용자 입력 정보 영구 보존.
    if (!QFile::exists(m_configPath)) {
        QStringList candidates;
        candidates << backupConfigPath();   // 백업 (우선)
        // ★ 옛 in-bundle config (이전엔 번들 안에 저장했음 — 마이그레이션용)
        QString appDir = QCoreApplication::applicationDirPath();
        candidates << appDir + "/../Resources/miyo_config.json";
        // 옛 ABIWA 시절 경로들
        candidates << QStandardPaths::writableLocation(QStandardPaths::AppDataLocation) + "/ABIWA/miyo_config.json";
        candidates << QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/ABIWA/miyo_config.json";
        for (const QString &oldPath : candidates) {
            if (QFile::exists(oldPath)) {
                QFile::copy(oldPath, m_configPath);
                qDebug() << "[Config] restored from" << oldPath << "to" << m_configPath;
                break;
            }
        }
    }

    QFile file(m_configPath);
    if (!file.open(QIODevice::ReadOnly)) {
        qDebug() << "Config file not found:" << m_configPath;
        return;
    }

    QJsonParseError error;
    QJsonDocument doc = QJsonDocument::fromJson(file.readAll(), &error);
    file.close();

    if (error.error != QJsonParseError::NoError) {
        qDebug() << "Config parse error:" << error.errorString();
        return;
    }

    fromJson(doc.object());
}

void Config::save(const QString &filePath)
{
    QString path = filePath.isEmpty() ? m_configPath : filePath;
    if (path.isEmpty()) path = defaultConfigPath();

    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonDocument doc(toJson());
    QByteArray bytes = doc.toJson(QJsonDocument::Indented);

    // ★ 주 위치 저장 (앱 내부)
    {
        QFile file(path);
        if (file.open(QIODevice::WriteOnly)) {
            file.write(bytes);
            file.close();
        } else {
            qDebug() << "Cannot write config:" << path;
        }
    }

    // ★ 외부 백업 동시 저장 — 재빌드로 앱 내부 사라지면 자동 복원
    if (filePath.isEmpty()) {
        QString backup = backupConfigPath();
        QDir().mkpath(QFileInfo(backup).absolutePath());
        QFile bf(backup);
        if (bf.open(QIODevice::WriteOnly)) {
            bf.write(bytes);
            bf.close();
        }
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
    root["webdavEnabled"] = m_webdavEnabled;
    if (!m_storageMode.isEmpty()) root["storageMode"] = m_storageMode;
    if (!m_storageRoot.isEmpty()) root["storageRoot"] = m_storageRoot;
    root["backupEnabled"] = m_backupEnabled;
    if (!m_backupPath.isEmpty()) root["backupPath"] = m_backupPath;
    root["ytDlpAutoUpdate"] = m_ytDlpAutoUpdate;
    root["firstRunCompleted"] = m_firstRunCompleted;
    root["nasAutoReconnect"] = m_nasAutoReconnect;
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
    if (obj.contains("webdavEnabled"))  m_webdavEnabled = obj["webdavEnabled"].toBool();
    if (obj.contains("storageMode"))    m_storageMode = obj["storageMode"].toString();
    if (obj.contains("storageRoot"))    m_storageRoot = obj["storageRoot"].toString();
    if (obj.contains("backupEnabled"))  m_backupEnabled = obj["backupEnabled"].toBool();
    if (obj.contains("backupPath"))     m_backupPath = obj["backupPath"].toString();
    if (obj.contains("ytDlpAutoUpdate")) m_ytDlpAutoUpdate = obj["ytDlpAutoUpdate"].toBool();
    if (obj.contains("firstRunCompleted")) m_firstRunCompleted = obj["firstRunCompleted"].toBool();
    if (obj.contains("nasAutoReconnect")) m_nasAutoReconnect = obj["nasAutoReconnect"].toBool();
}
