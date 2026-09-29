#pragma once
#include <QByteArray>

// AES over Windows CNG.
namespace Aes {

// `counter` is the 16-byte initial block, incrementing big-endian.
QByteArray ctr(const QByteArray &key, QByteArray counter, const QByteArray &input);

// ciphertext + 16-byte tag (WebCrypto layout); empty on tag mismatch
QByteArray gcmDecrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &ctWithTag);
QByteArray gcmEncrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &plaintext);

// PKCS#7 padding removed; empty if the key, IV or padding is wrong.
QByteArray cbcDecrypt(const QByteArray &key, const QByteArray &iv, const QByteArray &input);

}
