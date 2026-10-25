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
    // 남이 만든 문서도 같은 방식으로 — 숨은 짧은 이름에 다 쓴 뒤 바꿔 끼운다(덮어 바꾸지 못하는 네트워크 볼륨이면 지우고 바꾼다)
    static bool saveDocument(QXlsx::Document &doc, const QString &filePath);

private:
    QXlsx::Document *m_doc;
    int m_currentRow = 1;
};
