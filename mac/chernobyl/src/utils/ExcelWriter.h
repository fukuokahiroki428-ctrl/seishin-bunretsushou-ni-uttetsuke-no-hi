#pragma once

#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QColor>
#include <functional>

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
    // 저장에 실패하면 부르는 것(앱이 화면 기록으로 잇는다) — 부르는 쪽 대부분이 반환값을 보지 않아 실패가 묻혔다
    static void setSaveFailureHook(std::function<void(const QString &path, const QString &why)> hook);
    // 끝까지 다 쓴 xlsx(zip)인가 — 끝 표시(EOCD)가 제자리(파일 끝 · 가운데 목록 바로 뒤)에 있는가
    static bool xlsxWrittenToEnd(const QString &path);
    // ★ 시험 전용 — 숨은 이름에 다 쓴 뒤 · 다 썼는지 보기 전에 부른다(단위 시험이 잘린 파일을 흉내 낸다). 앱은 쓰지 않는다.
    static void setAfterWriteHookForTest(std::function<void(const QString &tmpPath)> hook);

private:
    QXlsx::Document *m_doc;
    int m_currentRow = 1;
};
