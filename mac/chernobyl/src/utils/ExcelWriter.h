#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QColor>

namespace QXlsx {
class Document;
class Format;
}

class ExcelWriter
{
public:
    ExcelWriter();
    ~ExcelWriter();

    void createSheet(const QString &name);
    void selectSheet(const QString &name);

    // Write data
    void writeHeader(const QStringList &headers, const QColor &bgColor = QColor("#d97757"),
                     const QColor &textColor = QColor(Qt::white));
    void writeRow(int row, const QStringList &values);
    void writeRow(int row, const QStringList &values, const QColor &bgColor);

    // Styling
    void setColumnWidth(int col, double width);
    void autoFitColumns(const QStringList &headers);

    // Save
    bool save(const QString &filePath);

private:
    QXlsx::Document *m_doc;
    int m_currentRow = 1;
};
