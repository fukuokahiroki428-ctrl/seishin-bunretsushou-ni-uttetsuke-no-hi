#include "DiskJsonBuffer.h"
#include <QDir>
#include <QJsonDocument>
#include <QUuid>

DiskJsonBuffer::DiskJsonBuffer(const QString &tempDir, const QString &prefix)
{
    QDir().mkpath(tempDir);
    QString filename = prefix + "_" + QUuid::createUuid().toString(QUuid::Id128).left(8) + ".jsonl";
    m_file.setFileName(tempDir + "/" + filename);
    m_file.open(QIODevice::ReadWrite | QIODevice::Truncate);
}

DiskJsonBuffer::~DiskJsonBuffer()
{
    if (m_file.isOpen()) m_file.close();
    if (m_file.exists()) m_file.remove();
}

void DiskJsonBuffer::append(const QJsonObject &obj)
{
    if (!m_file.isOpen()) return;
    QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
    m_file.write(line);
    m_file.flush();
    m_count++;
}

int DiskJsonBuffer::count() const
{
    return m_count;
}

QJsonArray DiskJsonBuffer::readAll()
{
    QJsonArray result;
    if (!m_file.isOpen()) return result;

    m_file.seek(0);
    while (!m_file.atEnd()) {
        QByteArray line = m_file.readLine().trimmed();
        if (line.isEmpty()) continue;
        QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isNull()) result.append(doc.object());
    }
    return result;
}

void DiskJsonBuffer::resetReader()
{
    if (m_file.isOpen()) {
        m_file.seek(0);
    }
}

bool DiskJsonBuffer::readNext(QJsonObject &out)
{
    if (!m_file.isOpen()) return false;
    while (!m_file.atEnd()) {
        QByteArray line = m_file.readLine().trimmed();
        if (line.isEmpty()) continue;
        QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isNull()) {
            out = doc.object();
            return true;
        }
    }
    return false;
}

void DiskJsonBuffer::clear()
{
    if (m_file.isOpen()) {
        m_file.resize(0);
        m_file.seek(0);
    }
    m_count = 0;
}

QString DiskJsonBuffer::filePath() const
{
    return m_file.fileName();
}
