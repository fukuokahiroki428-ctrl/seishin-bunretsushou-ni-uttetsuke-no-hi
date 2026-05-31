#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonDocument>
#include <QString>
#include <QMap>

struct AccountInfo {
    QString name;
    QString authToken;
    QString ct0;
    QString handle;
    QString password;
    QString token;
    QString sessionId;

    QJsonObject toJson() const;
    static AccountInfo fromJson(const QJsonObject &obj);
};

class Config : public QObject
{
    Q_OBJECT

public:
    explicit Config(QObject *parent = nullptr);

    void load(const QString &filePath = QString());
    void save(const QString &filePath = QString());

    // Returns absolute path: ~/Library/Application Support/ABIWA/miyo_config.json
    static QString defaultConfigPath();

    QJsonArray getAccounts(const QString &platform) const;
    void setAccounts(const QString &platform, const QJsonArray &accounts);
    void addAccount(const QString &platform, const QJsonObject &account);
    void removeAccount(const QString &platform, int index);

    QJsonObject toJson() const;
    void fromJson(const QJsonObject &obj);

    QString tempDir() const;
    void setTempDir(const QString &dir);

    QString tradCoverPath() const;
    void setTradCoverPath(const QString &path);

    QJsonObject formData() const;
    void setFormData(const QJsonObject &data);

    // WebDAV NAS 업로드 설정
    QString webdavUrl() const { return m_webdavUrl; }
    QString webdavUser() const { return m_webdavUser; }
    QString webdavPass() const { return m_webdavPass; }
    bool webdavEnabled() const { return m_webdavEnabled; }
    void setWebdavUrl(const QString &u) { m_webdavUrl = u; }
    void setWebdavUser(const QString &u) { m_webdavUser = u; }
    void setWebdavPass(const QString &p) { m_webdavPass = p; }
    void setWebdavEnabled(bool b) { m_webdavEnabled = b; }

private:
    QMap<QString, QJsonArray> m_accounts;
    QString m_configPath;
    QString m_tempDir;
    QString m_tradCoverPath;
    QJsonObject m_formData;
    QString m_webdavUrl;
    QString m_webdavUser;
    QString m_webdavPass;
    bool m_webdavEnabled = false;
};
