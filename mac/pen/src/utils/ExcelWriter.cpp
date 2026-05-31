#include "ExcelWriter.h"
#include "xlsxdocument.h"
#include "xlsxformat.h"

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
    return m_doc->saveAs(filePath);
}
