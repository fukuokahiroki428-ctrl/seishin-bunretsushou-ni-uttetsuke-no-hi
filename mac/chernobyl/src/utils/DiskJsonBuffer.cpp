#include "DiskJsonBuffer.h"
#include <QDebug>
#include <QDir>
#include <QJsonDocument>
#include <QUuid>

DiskJsonBuffer::DiskJsonBuffer(const QString &tempDir, const QString &prefix)
{
    QDir().mkpath(tempDir);
    QString filename = prefix + "_" + QUuid::createUuid().toString(QUuid::Id128).left(8) + ".jsonl";
    m_file.setFileName(tempDir + "/" + filename);
    if (!m_file.open(QIODevice::ReadWrite | QIODevice::Truncate)) {
        // 예전엔 반환값을 안 봤다. 못 열면 이후 append 가 전부 조용히 버려진다.
        m_ok = false;
        qWarning() << "[DiskJsonBuffer] 버퍼 파일을 열지 못했습니다 —"
                   << m_file.fileName() << m_file.errorString();
    }
}

DiskJsonBuffer::~DiskJsonBuffer()
{
    if (m_file.isOpen()) m_file.close();
    if (m_file.exists()) m_file.remove();
}

void DiskJsonBuffer::append(const QJsonObject &obj)
{
    if (!m_file.isOpen()) { m_ok = false; return; }
    // ★ 읽기와 쓰기가 같은 파일 핸들을 쓴다. readNext 로 중간까지만 읽어 둔 상태에서
    //   append 하면 그 자리에 덮어써 앞의 기록이 깨진다. 지금 부르는 순서로는
    //   그런 조합이 없지만, 한 줄로 막아 둔다.
    if (!m_file.seek(m_file.size())) { m_ok = false; return; }

    const QByteArray line = QJsonDocument(obj).toJson(QJsonDocument::Compact) + "\n";
    if (m_file.write(line) != line.size() || !m_file.flush()) {
        // 디스크가 차면 open 은 성공하고 write 만 모자라게 쓴다.
        m_ok = false;
        qWarning() << "[DiskJsonBuffer] 버퍼 쓰기 실패 —" << m_file.fileName()
                   << m_file.errorString();
        return;
    }
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
