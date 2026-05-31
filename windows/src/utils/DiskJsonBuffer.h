#pragma once

#include <QObject>
#include <QJsonObject>
#include <QJsonArray>
#include <QFile>
#include <QTextStream>

class DiskJsonBuffer
{
public:
    explicit DiskJsonBuffer(const QString &tempDir, const QString &prefix = "miyo_buf");
    ~DiskJsonBuffer();

    void append(const QJsonObject &obj);
    int count() const;
    QJsonArray readAll();
    void clear();
    QString filePath() const;

    // Streaming reader: iterate without loading all into memory
    void resetReader();
    bool readNext(QJsonObject &out);

private:
    QFile m_file;
    int m_count = 0;
};
