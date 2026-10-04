#include "ExcelWriter.h"
#include "xlsxdocument.h"
#include "xlsxformat.h"
#include <QFile>
#include <QFileInfo>
#include <QCryptographicHash>
#include <cstdio>

ExcelWriter::ExcelWriter()
    : m_doc(new QXlsx::Document())
{
}

ExcelWriter::~ExcelWriter()
{
    delete m_doc;
}

void ExcelWriter::createSheet(const QString &name)
{
    m_doc->addSheet(name);
    m_currentRow = 1;
}

void ExcelWriter::selectSheet(const QString &name)
{
    m_doc->selectSheet(name);
    m_currentRow = 1;
}

void ExcelWriter::writeHeader(const QStringList &headers, const QColor &bgColor, const QColor &textColor)
{
    QXlsx::Format fmt;
    fmt.setFontBold(true);
    fmt.setFontColor(textColor);
    fmt.setPatternBackgroundColor(bgColor);
    fmt.setFontSize(11);

    for (int col = 0; col < headers.size(); ++col) {
        m_doc->write(1, col + 1, headers[col], fmt);
    }
    m_currentRow = 2;
}

void ExcelWriter::writeRow(int row, const QStringList &values)
{
    for (int col = 0; col < values.size(); ++col) {
        m_doc->write(row, col + 1, values[col]);
    }
}

void ExcelWriter::writeRow(int row, const QStringList &values, const QColor &bgColor)
{
    QXlsx::Format fmt;
    fmt.setPatternBackgroundColor(bgColor);

    for (int col = 0; col < values.size(); ++col) {
        m_doc->write(row, col + 1, values[col], fmt);
    }
}

void ExcelWriter::setColumnWidth(int col, double width)
{
    m_doc->setColumnWidth(col, col, width);
}

void ExcelWriter::autoFitColumns(const QStringList &headers)
{
    for (int i = 0; i < headers.size(); ++i) {
        double width = qMax(12.0, static_cast<double>(headers[i].length()) * 1.5);
        setColumnWidth(i + 1, width);
    }
}

bool ExcelWriter::save(const QString &filePath)
{
    // ★ 진짜 이름에 바로 쓰지 않는다. 쓰는 도중 앱이 죽으면 반쯤 쓴 xlsx 가 남고, 다음 판의
    //   '기존 파일 + 새 데이터 합치기'(트위터·인스타·디스코드·픽시브)가 그것을 못 읽어 기존 행 0개로
    //   본 뒤 이번 판 행만으로 덮어써 예전 기록을 통째로 잃는다. 같은 폴더의 숨은 짧은 이름에 다 쓴 뒤
    //   한 번에 바꾼다(같은 폴더 rename(2) 는 원자적이다). 이름에 .part 를 붙이지 않는 까닭은
    //   HttpClient::downloadFile 의 같은 자리 설명(255바이트 한계).
    const QFileInfo target(filePath);
    const QString tmp = target.absolutePath() + QStringLiteral("/.xlsx_")
        + QString::fromLatin1(QCryptographicHash::hash(target.fileName().toUtf8(),
                                                       QCryptographicHash::Sha1).toHex().left(20))
        + QStringLiteral(".part");
    QFile::remove(tmp);
    if (!m_doc->saveAs(tmp)) { QFile::remove(tmp); return false; }
    if (std::rename(QFile::encodeName(tmp).constData(), QFile::encodeName(filePath).constData()) == 0)
        return true;
    // rename(2) 로 덮어쓰지 못하는 곳(일부 네트워크 볼륨)은 지우고 바꾼다. 그래도 못 바꾸면 조각은
    // 지우지 않는다 — 원본을 이미 지웠으므로 새 데이터가 남은 유일한 사본이다.
    QFile::remove(filePath);
    return QFile::rename(tmp, filePath);
}
