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

    // ★ 이 버퍼에는 수집한 글이 담긴다. 파일을 못 열거나 쓰기가 모자라면
    //   예전에는 append 가 조용히 돌아갔다 — 수집은 '성공' 인데 결과만 비는,
    //   원인을 찾기 가장 어려운 형태의 손실이다. 부르는 쪽이 물어볼 수 있게 한다.
    bool ok() const { return m_ok; }
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
    bool m_ok = true;
};
