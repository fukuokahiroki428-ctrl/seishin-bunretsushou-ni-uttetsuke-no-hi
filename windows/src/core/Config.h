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

    // 주 위치: app/Contents/Resources/miyo_config.json (앱 내부 — 사용자 요청)
    static QString defaultConfigPath();
    // 백업: ~/Library/Application Support/Miyo/.../miyo_config.backup.json (재빌드 시 복원용)
    static QString backupConfigPath();

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

    QJsonObject platformTargets() const;
    void setPlatformTargets(const QJsonObject &data);

    bool debugLogs() const { return m_debugLogs; }
    void setDebugLogs(bool b) { m_debugLogs = b; }

    QString secondaryPath() const { return m_secondaryPath; }
    void setSecondaryPath(const QString &p) { m_secondaryPath = p; }

    // 内閣会 감시 목록 + interval 영구 저장
    QJsonArray naikakukaiWatches() const { return m_naikakukaiWatches; }
    void setNaikakukaiWatches(const QJsonArray &arr) { m_naikakukaiWatches = arr; }
    int naikakukaiInterval() const { return m_naikakukaiInterval; }
    void setNaikakukaiInterval(int min) { m_naikakukaiInterval = min; }

    // WebDAV 업로드 설정 (시놀로지 등 NAS)
    QString webdavUrl() const { return m_webdavUrl; }
    QString webdavUser() const { return m_webdavUser; }
    QString webdavPass() const { return m_webdavPass; }
    bool webdavEnabled() const { return m_webdavEnabled; }
    void setWebdavUrl(const QString &u) { m_webdavUrl = u; }
    void setWebdavUser(const QString &u) { m_webdavUser = u; }
    void setWebdavPass(const QString &p) { m_webdavPass = p; }
    void setWebdavEnabled(bool b) { m_webdavEnabled = b; }

    // ★ 저장 모드 — "local" / "nas" / "external"
    QString storageMode() const { return m_storageMode.isEmpty() ? "local" : m_storageMode; }
    QString storageRoot() const { return m_storageRoot; }
    void setStorageMode(const QString &m) { m_storageMode = m; }
    void setStorageRoot(const QString &r) { m_storageRoot = r; }

    // ★ NAS 자동 백업 — 로컬 다운로드 완료 후 NAS 마운트 폴더로 cp
    bool backupEnabled() const { return m_backupEnabled; }
    QString backupPath() const { return m_backupPath; }
    void setBackupEnabled(bool b) { m_backupEnabled = b; }
    void setBackupPath(const QString &p) { m_backupPath = p; }

    // ★ yt-dlp 자동 업데이트 — 보안 위험 (GitHub 변조 가능성) — 기본 OFF
    bool ytDlpAutoUpdate() const { return m_ytDlpAutoUpdate; }
    void setYtDlpAutoUpdate(bool b) { m_ytDlpAutoUpdate = b; }

    // ★ 첫 실행 setup wizard 완료 여부
    bool firstRunCompleted() const { return m_firstRunCompleted; }
    void setFirstRunCompleted(bool b) { m_firstRunCompleted = b; }

    // ★ NAS 자동 재연결 — 마운트 끊겼을 때 자동 remount
    bool nasAutoReconnect() const { return m_nasAutoReconnect; }
    void setNasAutoReconnect(bool b) { m_nasAutoReconnect = b; }

private:
    QMap<QString, QJsonArray> m_accounts;
    QString m_configPath;
    QString m_tempDir;
    QString m_tradCoverPath;
    QJsonObject m_formData;
    QJsonObject m_platformTargets;
    bool m_debugLogs = false;
    QString m_secondaryPath;
    QJsonArray m_naikakukaiWatches;
    int m_naikakukaiInterval = 30;
    QString m_webdavUrl;
    QString m_webdavUser;
    QString m_webdavPass;
    bool m_webdavEnabled = false;
    QString m_storageMode;  // "local" / "nas" / "external"
    QString m_storageRoot;  // /Volumes/X (mode != local 일 때)
    bool m_backupEnabled = false;
    QString m_backupPath;   // NAS 마운트 경로 (예: /Volumes/공유폴더/Chernobyl_Backup)
    bool m_ytDlpAutoUpdate = false;  // 보안 위험 — 사용자 명시적 ON 필요
    bool m_firstRunCompleted = false;
    bool m_nasAutoReconnect = true;  // 기본 ON (대부분 원하는 동작)
};
