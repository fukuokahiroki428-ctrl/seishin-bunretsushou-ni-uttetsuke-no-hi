#pragma once
// ═══════════════════════════════════════════════════════════════════════════
//  디스코드 오류를 사람이 읽을 수 있게 옮긴다.
//
//  왜 따로 두나:
//    디스코드를 부르는 곳이 두 군데다 — 앱 본체(HanishikiBackend)와 별도 창
//    (DiscordCollector). 처음엔 DiscordCollector 에만 이 설명을 넣었는데,
//    사용자가 실제로 쓰는 흐름은 본체 쪽이라 화면에는 여전히 숫자만 떴다.
//    두 곳이 같은 설명을 쓰도록 한 군데로 모은다.
//
//  왜 필요한가:
//    디스코드의 403 은 원인이 여러 가지다(채널을 못 봄 / 권한 없음 / 계정
//    미인증 / 앞단 차단). 디스코드는 그 이유를 '언제나' JSON 본문에 담아
//    보내는데, 그걸 버리고 숫자만 보여 주면 무엇을 해야 할지 알 수가 없다.
// ═══════════════════════════════════════════════════════════════════════════
#include "utils/HttpClient.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>

inline QString describeDiscordError(const HttpResponse &resp)
{
    const QJsonObject o = QJsonDocument::fromJson(resp.data).object();
    const int    code = o.value("code").toInt(-1);
    const QString msg = o.value("message").toString();

    // 본문이 JSON 이 아니면 디스코드가 아니라 그 앞단(Cloudflare)이 막은 것이다.
    // 이 둘은 대처가 완전히 다르므로 반드시 구분해서 알려 준다.
    if (o.isEmpty() && !resp.data.isEmpty()) {
        return QString("HTTP %1 — 디스코드가 아니라 앞단(Cloudflare)이 막았습니다. "
                       "잠시 뒤 다시 하시거나, VPN·프록시를 쓰고 계시면 끄고 해 보십시오.")
               .arg(resp.statusCode);
    }

    QString why;
    switch (code) {
    case 50001: why = "이 계정이 그 채널을 볼 수 없습니다. 서버에 들어가 있는지, "
                      "그 채널이 계정에 보이는지 확인해 주십시오."; break;
    case 50013: why = "권한이 모자랍니다. 채널을 읽을 권한이 있는 계정인지 확인해 주십시오."; break;
    case 40001: why = "토큰이 맞지 않습니다. 설정에서 디스코드 토큰을 다시 넣어 주십시오."; break;
    case 40002: why = "계정 인증이 필요합니다(이메일·전화 확인). 디스코드에서 먼저 마치셔야 합니다."; break;
    case 10003: why = "그런 채널이 없습니다. 채널 ID 를 확인해 주십시오."; break;
    case 10004: why = "그런 서버가 없습니다. 서버 ID 를 확인해 주십시오."; break;
    default:    break;
    }

    if (resp.statusCode == 401 && why.isEmpty())
        why = "토큰이 만료됐거나 잘못됐습니다. 설정에서 다시 넣어 주십시오.";
    if (resp.statusCode == 403 && why.isEmpty())
        why = "접근이 거부됐습니다. 계정이 그 서버·채널에 접근할 수 있는지 확인해 주십시오.";

    QString out = QString("HTTP %1").arg(resp.statusCode);
    if (code >= 0)        out += QString(" (code %1)").arg(code);
    if (!msg.isEmpty())   out += QString(" — %1").arg(msg);
    if (!why.isEmpty())   out += QString("\n      → %1").arg(why);
    return out;
}
