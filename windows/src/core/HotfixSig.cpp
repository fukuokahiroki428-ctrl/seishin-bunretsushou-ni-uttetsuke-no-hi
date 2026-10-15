#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#include "HotfixSig.h"
#ifdef Q_OS_MACOS
#include <Security/Security.h>
#elif defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <bcrypt.h>
#include <QCryptographicHash>
#endif

namespace HotfixSig {

// 공개 열쇠(X9.63: 04||X||Y). 비밀 열쇠는 ~/.config/hanishiki/hotfix_signing_key.pem 에만 있다 —
//   tools/hotfix_publish.py --genkey 가 만들고 이 값을 알려 준다. 열쇠를 바꾸면 여기도 바꿔 새 판을 낸다.
//   ★ 맥과 윈도우가 같은 꾸러미(hotfix-mac)를 받으므로 두 판의 이 값은 언제나 같아야 한다.
static const char kPublicKeyHex[] = "04f8c1e53e0edc1919699ff3da0bbacc624b1f0c19439595b0d70fd8e3f3f7538ef4dcf5436ead250edf0a4d5f3d74d17d9d84c00f2f7d7adf3ec48f26e26b85d9";

#ifdef Q_OS_WIN
// DER 의 INTEGER 하나를 32바이트 큰 끝 수로. 앞의 0 은 떼고(부호 바이트), 모자라면 앞을 0 으로 채운다.
static bool derInt32(const unsigned char *&p, const unsigned char *end, unsigned char out[32])
{
    if (end - p < 2 || p[0] != 0x02) return false;
    const int len = p[1];
    p += 2;
    if (len < 1 || len > 33 || end - p < len) return false;
    const unsigned char *v = p;
    int n = len;
    p += len;
    while (n > 0 && v[0] == 0x00) { ++v; --n; }
    if (n > 32) return false;
    memset(out, 0, 32);
    memcpy(out + (32 - n), v, n);
    return true;
}

// 서명은 DER(SEQUENCE{INTEGER r, INTEGER s}) 로 온다 — CNG 는 r||s 64바이트를 받는다.
static bool derToRaw(const QByteArray &der, unsigned char raw[64])
{
    const unsigned char *p = reinterpret_cast<const unsigned char *>(der.constData());
    const unsigned char *end = p + der.size();
    if (end - p < 2 || p[0] != 0x30) return false;
    int seqLen = p[1];
    p += 2;
    if (seqLen & 0x80) {                       // P-256 서명은 72바이트 안팎이라 긴 꼴은 0x81 하나뿐
        if (seqLen != 0x81 || end - p < 1) return false;
        seqLen = p[0];
        p += 1;
    }
    if (end - p != seqLen) return false;       // 뒤에 덧붙은 바이트가 있으면 받지 않는다
    if (!derInt32(p, end, raw) || !derInt32(p, end, raw + 32)) return false;
    return p == end;
}
#endif

bool verify(const QByteArray &message, const QByteArray &derSignature)
{
#ifdef Q_OS_MACOS
    if (message.isEmpty() || derSignature.isEmpty() || derSignature.size() > 256) return false;
    const QByteArray pub = QByteArray::fromHex(kPublicKeyHex);
    if (pub.size() != 65) return false;

    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                             &kCFTypeDictionaryValueCallBacks);
    CFDictionarySetValue(attrs, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
    CFDictionarySetValue(attrs, kSecAttrKeyClass, kSecAttrKeyClassPublic);
    int bits = 256;
    CFNumberRef nbits = CFNumberCreate(nullptr, kCFNumberIntType, &bits);
    CFDictionarySetValue(attrs, kSecAttrKeySizeInBits, nbits);

    CFDataRef keyData = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(pub.constData()), pub.size());
    CFErrorRef err = nullptr;
    SecKeyRef key = SecKeyCreateWithData(keyData, attrs, &err);
    bool ok = false;
    if (key) {
        CFDataRef msg = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(message.constData()), message.size());
        CFDataRef sig = CFDataCreate(nullptr, reinterpret_cast<const UInt8 *>(derSignature.constData()), derSignature.size());
        CFErrorRef verr = nullptr;
        ok = SecKeyVerifySignature(key, kSecKeyAlgorithmECDSASignatureMessageX962SHA256, msg, sig, &verr);
        if (verr) CFRelease(verr);
        CFRelease(msg); CFRelease(sig); CFRelease(key);
    }
    if (err) CFRelease(err);
    CFRelease(keyData); CFRelease(nbits); CFRelease(attrs);
    return ok;
#elif defined(Q_OS_WIN)
    if (message.isEmpty() || derSignature.isEmpty() || derSignature.size() > 256) return false;
    const QByteArray pub = QByteArray::fromHex(kPublicKeyHex);
    if (pub.size() != 65 || static_cast<unsigned char>(pub[0]) != 0x04) return false;
    unsigned char rawSig[64];
    if (!derToRaw(derSignature, rawSig)) return false;

    // BCRYPT_ECCKEY_BLOB 머리 + X(32) + Y(32)
    QByteArray blob(int(sizeof(BCRYPT_ECCKEY_BLOB)) + 64, '\0');
    auto *hdr = reinterpret_cast<BCRYPT_ECCKEY_BLOB *>(blob.data());
    hdr->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    hdr->cbKey = 32;
    memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), pub.constData() + 1, 64);

    const QByteArray digest = QCryptographicHash::hash(message, QCryptographicHash::Sha256);

    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_KEY_HANDLE key = nullptr;
    bool ok = false;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0))) {
        if (BCRYPT_SUCCESS(BCryptImportKeyPair(alg, nullptr, BCRYPT_ECCPUBLIC_BLOB, &key,
                                               reinterpret_cast<PUCHAR>(blob.data()), ULONG(blob.size()), 0))) {
            ok = BCRYPT_SUCCESS(BCryptVerifySignature(
                key, nullptr,
                reinterpret_cast<PUCHAR>(const_cast<char *>(digest.constData())), ULONG(digest.size()),
                rawSig, ULONG(sizeof(rawSig)), 0));
            BCryptDestroyKey(key);
        }
        BCryptCloseAlgorithmProvider(alg, 0);
    }
    return ok;
#else
    Q_UNUSED(message); Q_UNUSED(derSignature);
    return false;
#endif
}

} // namespace HotfixSig
