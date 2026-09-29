#include "platform/crypto.h"

#include <windows.h>
#include <bcrypt.h>

namespace {

// An AES key under one CNG chaining mode. CTR builds on single-block ECB, so the counter
// arithmetic stays in Aes::ctr.
class CngAes {
public:
    CngAes(const QByteArray &key, const wchar_t *mode) {
        if (BCryptOpenAlgorithmProvider(&m_alg, BCRYPT_AES_ALGORITHM, nullptr, 0) != 0) return;
        if (BCryptSetProperty(m_alg, BCRYPT_CHAINING_MODE,
                              reinterpret_cast<PUCHAR>(const_cast<wchar_t *>(mode)),
                              ULONG((wcslen(mode) + 1) * sizeof(wchar_t)), 0) != 0) return;
        if (BCryptGenerateSymmetricKey(m_alg, &m_key, nullptr, 0,
                                       reinterpret_cast<PUCHAR>(const_cast<char *>(key.constData())),
                                       key.size(), 0) != 0) return;
        m_ok = true;
    }
    ~CngAes() {
        if (m_key) BCryptDestroyKey(m_key);
        if (m_alg) BCryptCloseAlgorithmProvider(m_alg, 0);
    }
    CngAes(const CngAes &) = delete;
    CngAes &operator=(const CngAes &) = delete;

    bool ok() const { return m_ok; }
    BCRYPT_KEY_HANDLE handle() const { return m_key; }

    bool encryptBlock(const unsigned char *in, unsigned char *out) {
        ULONG n = 0;
        return BCryptEncrypt(m_key, const_cast<PUCHAR>(in), 16, nullptr,
                             nullptr, 0, out, 16, &n, 0) == 0;
    }

private:
    bool m_ok = false;
    BCRYPT_ALG_HANDLE m_alg = nullptr;
    BCRYPT_KEY_HANDLE m_key = nullptr;
};

BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO gcmInfo(const QByteArray &iv, unsigned char *tag) {
    BCRYPT_AUTHENTICATED_CIPHER_MODE_INFO info;
    BCRYPT_INIT_AUTH_MODE_INFO(info);
    info.pbNonce = reinterpret_cast<PUCHAR>(const_cast<char *>(iv.constData()));
    info.cbNonce = ULONG(iv.size());
    info.pbTag   = tag;
    info.cbTag   = 16;
    return info;
}

}

QByteArray Aes::ctr(const QByteArray &key, QByteArray counter, const QByteArray &input) {
    if (key.size() != 32 || counter.size() != 16) return {};
    CngAes enc(key, BCRYPT_CHAIN_MODE_ECB);
    if (!enc.ok()) return {};

    QByteArray out(input.size(), Qt::Uninitialized);
    unsigned char ks[16];
    for (int off = 0; off < input.size(); off += 16) {
        if (!enc.encryptBlock(reinterpret_cast<const unsigned char *>(counter.constData()), ks))
            return {};
        const int blk = qMin(16, input.size() - off);
        for (int i = 0; i < blk; ++i)
            out[off + i] = static_cast<char>(static_cast<unsigned char>(input[off + i]) ^ ks[i]);
        for (int i = 15; i >= 0; --i) {   // big-endian increment
            unsigned char v = static_cast<unsigned char>(counter[i]) + 1;
            counter[i] = static_cast<char>(v);
            if (v != 0) break;
        }
    }
    return out;
}

QByteArray Aes::gcmDecrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &ctWithTag) {
    constexpr int tagLen = 16;
    if (ctWithTag.size() < tagLen) return {};
    CngAes aes(key, BCRYPT_CHAIN_MODE_GCM);
    if (!aes.ok()) return {};

    const int ctLen = ctWithTag.size() - tagLen;
    const auto *ct = reinterpret_cast<const unsigned char *>(ctWithTag.constData());
    auto info = gcmInfo(iv, const_cast<unsigned char *>(ct + ctLen));
    QByteArray plaintext(ctLen, 0);
    ULONG resultLen = 0;
    if (BCryptDecrypt(aes.handle(), const_cast<PUCHAR>(ct), ctLen, &info, nullptr, 0,
                      reinterpret_cast<PUCHAR>(plaintext.data()), ctLen, &resultLen, 0) != 0)
        return {};
    plaintext.resize(resultLen);
    return plaintext;
}

QByteArray Aes::gcmEncrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &plaintext) {
    CngAes aes(key, BCRYPT_CHAIN_MODE_GCM);
    if (!aes.ok()) return {};

    QByteArray out(plaintext.size() + 16, 0);
    auto *tag = reinterpret_cast<unsigned char *>(out.data()) + plaintext.size();
    auto info = gcmInfo(iv, tag);
    ULONG resultLen = 0;
    if (BCryptEncrypt(aes.handle(),
                      reinterpret_cast<PUCHAR>(const_cast<char *>(plaintext.constData())),
                      ULONG(plaintext.size()), &info, nullptr, 0,
                      reinterpret_cast<PUCHAR>(out.data()), ULONG(plaintext.size()), &resultLen, 0) != 0)
        return {};
    return out;
}

QByteArray Aes::cbcDecrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &input) {
    if (iv.size() != 16 || input.isEmpty() || input.size() % 16 != 0) return {};
    CngAes aes(key, BCRYPT_CHAIN_MODE_CBC);
    if (!aes.ok()) return {};

    // CNG advances the IV it is handed, so it gets a copy.
    QByteArray chain = iv;
    QByteArray plain(input.size(), Qt::Uninitialized);
    ULONG length = 0;
    if (BCryptDecrypt(aes.handle(), reinterpret_cast<PUCHAR>(const_cast<char *>(input.constData())),
                      ULONG(input.size()), nullptr, reinterpret_cast<PUCHAR>(chain.data()), ULONG(chain.size()),
                      reinterpret_cast<PUCHAR>(plain.data()), ULONG(plain.size()), &length,
                      BCRYPT_BLOCK_PADDING) != 0)
        return {};
    plain.resize(qsizetype(length));
    return plain;
}
