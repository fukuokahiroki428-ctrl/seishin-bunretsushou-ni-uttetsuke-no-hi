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
    return saveDocument(*m_doc, filePath);
}

static std::function<void(const QString &, const QString &)> &saveFailureHook()
{
    static std::function<void(const QString &, const QString &)> h;
    return h;
}

void ExcelWriter::setSaveFailureHook(std::function<void(const QString &path, const QString &why)> hook)
{
    saveFailureHook() = std::move(hook);
}

static bool reportSaveFailure(const QString &path, const QString &why)
{
    if (saveFailureHook()) saveFailureHook()(path, why);
    return false;
}

static std::function<void(const QString &)> &afterWriteHook()
{
    static std::function<void(const QString &)> h;
    return h;
}

void ExcelWriter::setAfterWriteHookForTest(std::function<void(const QString &tmpPath)> hook)
{
    afterWriteHook() = std::move(hook);
}

// zip 의 끝 표시(EOCD)가 제자리에 있는가 — 끝까지 다 쓴 xlsx 에만 있다.
//   표시 네 글자만 찾으면 압축된 내용 안의 우연한 네 바이트에 속을 수 있어, 표시 뒤 길이(22 + 주석)가 파일 끝과 맞고
//   가운데 목록(central directory)이 표시 바로 앞에서 끝나는지까지 본다(QXlsx 가 쓰는 보통 zip — 4GB 넘는 zip64 는 없다).
bool ExcelWriter::xlsxWrittenToEnd(const QString &path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly) || f.size() < 22) return false;
    const qint64 size = f.size();
    const qint64 tail = qMin<qint64>(size, 22 + 65535);
    if (!f.seek(size - tail)) return false;
    const QByteArray b = f.read(tail);
    if (b.size() != tail) return false;
    const auto u16 = [&](qsizetype o) { return quint32(quint8(b[o])) | (quint32(quint8(b[o + 1])) << 8); };
    const auto u32 = [&](qsizetype o) { return u16(o) | (u16(o + 2) << 16); };
    for (qsizetype i = b.lastIndexOf(QByteArrayLiteral("PK\x05\x06")); i >= 0;
         i = (i > 0) ? b.lastIndexOf(QByteArrayLiteral("PK\x05\x06"), i - 1) : -1) {
        if (i + 22 > b.size()) continue;
        const qint64 eocdPos = size - tail + i;
        if (eocdPos + 22 + qint64(u16(i + 20)) != size) continue;          // 주석 길이까지 더해 파일 끝과 맞아야
        if (qint64(u32(i + 16)) + qint64(u32(i + 12)) != eocdPos) continue;   // 가운데 목록(위치 + 크기)이 표시 바로 앞에서 끝나야
        return true;
    }
    return false;
}

bool ExcelWriter::saveDocument(QXlsx::Document &doc, const QString &filePath)
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
    // ★ QXlsx 는 쓰기 실패를 알리지 않는다(savePackage 는 끝에서 늘 true). 디스크가 차서 잘린 파일이 원본을 덮으면,
    //   다음 판이 기존 줄을 0개로 읽고 이번 판 줄만으로 다시 써 그 대상의 엑셀 기록을 통째로 잃는다 — 다 썼는지 보고 바꾼다.
    const bool wrote = doc.saveAs(tmp);
    if (wrote && afterWriteHook()) afterWriteHook()(tmp);
    if (!wrote || !xlsxWrittenToEnd(tmp)) {
        QFile::remove(tmp);
        return reportSaveFailure(filePath, QStringLiteral("끝까지 쓰지 못했습니다(디스크 공간 · 저장 위치를 확인하십시오)"));
    }
    if (std::rename(QFile::encodeName(tmp).constData(), QFile::encodeName(filePath).constData()) == 0)
        return true;
    // rename(2) 로 덮어쓰지 못하는 곳(일부 네트워크 볼륨)은 지우고 바꾼다. 그래도 못 바꾸면 조각은
    // 지우지 않는다 — 원본을 이미 지웠으므로 새 데이터가 남은 유일한 사본이다.
    QFile::remove(filePath);
    if (QFile::rename(tmp, filePath)) return true;
    return reportSaveFailure(filePath, QStringLiteral("새 파일로 바꿔 넣지 못했습니다 — 쓴 내용은 %1 에 있습니다").arg(QFileInfo(tmp).fileName()));
}
