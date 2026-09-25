#define __ASSERT_MACROS_DEFINE_VERSIONS_WITHOUT_UNDERSCORES 0
#include "HotfixSig.h"
#ifdef Q_OS_MACOS
#include <Security/Security.h>
#endif

namespace HotfixSig {

// 공개 열쇠(X9.63: 04||X||Y). 비밀 열쇠는 ~/.config/hanishiki/hotfix_signing_key.pem 에만 있다 —
//   tools/hotfix_publish.py --genkey 가 만들고 이 값을 알려 준다. 열쇠를 바꾸면 여기도 바꿔 새 판을 낸다.
static const char kPublicKeyHex[] = "04f8c1e53e0edc1919699ff3da0bbacc624b1f0c19439595b0d70fd8e3f3f7538ef4dcf5436ead250edf0a4d5f3d74d17d9d84c00f2f7d7adf3ec48f26e26b85d9";

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
#else
    Q_UNUSED(message); Q_UNUSED(derSignature);
    return false;
#endif
}

} // namespace HotfixSig
