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
    // ★ 외부 user data 위치 — ~/Library/Application Support/Pen/Pen/miyo_config.json
    //   이전엔 앱 내부(Contents/Resources)에 저장했으나 매 save 시 codesign seal 깨짐
    //   → macOS 보안 정책이 "변조된 앱"으로 판단 → 캡쳐/CDP 등 보안 동작 차단 → 앱 크래시.
    //   외부 저장하면 번들 read-only 유지 → 서명 유효 → macOS 권한 영구 유지.
    QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QDir().mkpath(dataDir);
    return dataDir + "/miyo_config.json";
}

void Config::load(const QString &filePath)
{
    m_configPath = filePath.isEmpty() ? defaultConfigPath() : filePath;

    // 기존 in-bundle/legacy 경로에서 마이그레이션
    if (!QFile::exists(m_configPath)) {
        QStringList candidates;
        // 옛 in-bundle (이전 버전 호환)
        QString appDir = QCoreApplication::applicationDirPath();
        candidates << appDir + "/../Resources/miyo_config.json";
        // 옛 ABIWA 경로
        candidates << QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation) + "/ABIWA/miyo_config.json";
        for (const QString &oldPath : candidates) {
            if (QFile::exists(oldPath)) {
                QFile::copy(oldPath, m_configPath);
                qDebug() << "Config migrated from" << oldPath << "to" << m_configPath;
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
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly)) {
        qDebug() << "Cannot write config:" << path;
        return;
    }

    QJsonDocument doc(toJson());
    file.write(doc.toJson(QJsonDocument::Indented));
    file.close();
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
    if (!m_webdavUrl.isEmpty())  root["webdavUrl"]  = m_webdavUrl;
    if (!m_webdavUser.isEmpty()) root["webdavUser"] = m_webdavUser;
    if (!m_webdavPass.isEmpty()) root["webdavPass"] = m_webdavPass;
    root["webdavEnabled"] = m_webdavEnabled;
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
    if (obj.contains("webdavUrl"))     m_webdavUrl  = obj["webdavUrl"].toString();
    if (obj.contains("webdavUser"))    m_webdavUser = obj["webdavUser"].toString();
    if (obj.contains("webdavPass"))    m_webdavPass = obj["webdavPass"].toString();
    if (obj.contains("webdavEnabled")) m_webdavEnabled = obj["webdavEnabled"].toBool();
}
