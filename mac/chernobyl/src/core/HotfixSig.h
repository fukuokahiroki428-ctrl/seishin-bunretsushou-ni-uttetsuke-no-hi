#pragma once
#include <QByteArray>

// 고침 꾸러미 서명 검증 — ECDSA P-256 / SHA-256, macOS Security.framework.
//   manifest.json 바이트 그대로에 대한 서명(manifest.sig, DER)을 앱에 박아 둔 공개 열쇠로 검증한다.
//   Security 머리 파일이 check·verify 같은 짧은 매크로를 뿌려 Qt 코드와 부딪힐 수 있어 이 파일로 떼어 둔다.
namespace HotfixSig {
bool verify(const QByteArray &message, const QByteArray &derSignature);
}
