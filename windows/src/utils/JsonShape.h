#pragma once

// ── 응답 모양이 바뀌어도 버티는 읽기 ────────────────────────────────────────
//
// 바깥 서비스는 예고 없이 이름을 바꾼다. 2026-09-24 하루에만 이만큼 겪었다:
//   Fanbox  post.listCreator: body.items → body.posts, nextUrl 사라짐(paginateCreator 로 이사)
//           post.info       : body.<내용> → body.post.<내용> (껍데기 한 겹 추가)
//   Instagram              : REST 세 끝점이 통째로 죽고 GraphQL 로 이사
//
// 한 이름만 보고 읽으면 그날로 '0개 수집 완료' 가 된다. 오류도 아니고 빈손도 아닌,
// 가장 알아채기 어려운 고장이다 — 팬박스는 그렇게 여덟 달을 조용히 비어 있었다.
//
// 그래서 읽을 때 이름을 '여러 개' 댄다. 그래도 못 찾으면 모양을 훑어 찾아보고,
// 그때는 반드시 소리를 낸다(부르는 쪽이 로그를 남긴다). 조용히 넘어가지 않는 것이 핵심이다.

#include <QJsonArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QStringList>

// 고침 꾸러미의 별명 — 바깥이 이름을 또 바꾸면 새 판 없이 후보를 덧붙인다(Common.cpp).
namespace Common { QStringList shapeAliasesFor(const QStringList &keys); }

namespace JsonShape {

// 여러 이름 중 먼저 '있는' 것을 준다(null 은 없는 것으로 본다).
inline QJsonValue pick(const QJsonObject &o, const QStringList &keys)
{
    for (const QString &k : Common::shapeAliasesFor(keys)) {
        const QJsonValue v = o.value(k);
        if (!v.isUndefined() && !v.isNull()) return v;
    }
    return QJsonValue();
}

inline QJsonArray pickArray(const QJsonObject &o, const QStringList &keys)
{
    for (const QString &k : Common::shapeAliasesFor(keys)) {
        const QJsonValue v = o.value(k);
        if (v.isArray()) return v.toArray();
    }
    return QJsonArray();
}

inline QJsonObject pickObject(const QJsonObject &o, const QStringList &keys)
{
    for (const QString &k : Common::shapeAliasesFor(keys)) {
        const QJsonValue v = o.value(k);
        if (v.isObject()) return v.toObject();
    }
    return QJsonObject();
}

inline QString pickString(const QJsonObject &o, const QStringList &keys)
{
    for (const QString &k : Common::shapeAliasesFor(keys)) {
        const QJsonValue v = o.value(k);
        if (v.isString() && !v.toString().isEmpty()) return v.toString();
        if (v.isDouble()) return QString::number(static_cast<qint64>(v.toDouble()));
    }
    return QString();
}

// 껍데기를 한 겹씩 벗긴다. body.post.body 처럼 중간이 하나 늘어도 따라간다.
//   벗길 이름을 못 만나면 그 자리에서 멈춘다(원래 것을 그대로 돌려준다).
inline QJsonObject unwrap(const QJsonObject &o, const QStringList &wrappers, int maxDepth = 4)
{
    QJsonObject cur = o;
    for (int d = 0; d < maxDepth; ++d) {
        bool moved = false;
        for (const QString &w : Common::shapeAliasesFor(wrappers)) {
            const QJsonValue v = cur.value(w);
            if (v.isObject()) { cur = v.toObject(); moved = true; break; }
        }
        if (!moved) break;
    }
    return cur;
}

// 마지막 수단 — 이 객체 안에서 '객체들의 배열' 중 가장 긴 것을 찾는다.
//   이름이 무엇으로 바뀌었는지 몰라도 목록은 목록이다. 찾으면 keyOut 에 이름을 적어
//   부르는 쪽이 "이 이름으로 찾았습니다" 라고 말할 수 있게 한다.
inline QJsonArray findArrayOfObjects(const QJsonObject &o, QString *keyOut = nullptr,
                                     int maxDepth = 3, int minSize = 1)
{
    QJsonArray best;
    QString bestKey;
    struct Walker {
        int maxDepth; int minSize; QJsonArray *best; QString *bestKey;
        void walk(const QJsonObject &obj, int depth, const QString &path) {
            if (depth > maxDepth) return;
            for (auto it = obj.constBegin(); it != obj.constEnd(); ++it) {
                const QString here = path.isEmpty() ? it.key() : path + "." + it.key();
                const QJsonValue v = it.value();
                if (v.isArray()) {
                    const QJsonArray a = v.toArray();
                    if (a.size() >= minSize && a.first().isObject() && a.size() > best->size()) {
                        *best = a; *bestKey = here;
                    }
                } else if (v.isObject()) {
                    walk(v.toObject(), depth + 1, here);
                }
            }
        }
    } w{maxDepth, minSize, &best, &bestKey};
    w.walk(o, 0, QString());
    if (keyOut) *keyOut = bestKey;
    return best;
}

// 무엇이 들어 있었는지 한 줄로 — 모양이 바뀌었을 때 로그에 적어 두면
// 다음 사람이 추측하지 않아도 된다.
inline QString describe(const QJsonObject &o, int maxKeys = 12)
{
    QStringList parts;
    for (auto it = o.constBegin(); it != o.constEnd() && parts.size() < maxKeys; ++it) {
        const QJsonValue v = it.value();
        QString t = v.isArray()  ? QString("배열%1").arg(v.toArray().size())
                  : v.isObject() ? QStringLiteral("묶음")
                  : v.isString() ? QStringLiteral("글")
                  : v.isDouble() ? QStringLiteral("수")
                  : v.isBool()   ? QStringLiteral("참거짓")
                                 : QStringLiteral("빔");
        parts << it.key() + "(" + t + ")";
    }
    if (o.size() > parts.size()) parts << QString("…외 %1").arg(o.size() - parts.size());
    return parts.join(", ");
}

// 목록이 '배열' 로도 '아이디를 열쇠로 쓴 묶음' 으로도 올 수 있다(Pixiv 의 illusts 가 그 꼴이다).
//   어느 쪽이 와도 아이디 목록을 준다 — 한쪽만 읽다가 상대가 꼴을 바꾸면 그날로 0개가 된다.
inline QStringList idsFromMapOrArray(const QJsonValue &v, const QStringList &idKeys = {"id", "illustId", "workId"})
{
    QStringList out;
    if (v.isObject()) {
        const QJsonObject o = v.toObject();
        for (auto it = o.constBegin(); it != o.constEnd(); ++it) out << it.key();
    } else if (v.isArray()) {
        for (const auto &e : v.toArray()) {
            if (e.isString()) { out << e.toString(); continue; }
            if (e.isDouble()) { out << QString::number(static_cast<qint64>(e.toDouble())); continue; }
            const QString id = pickString(e.toObject(), idKeys);
            if (!id.isEmpty()) out << id;
        }
    }
    return out;
}

} // namespace JsonShape
