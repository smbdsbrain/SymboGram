#ifndef CRYPTO_H
#define CRYPTO_H

#include <QByteArray>
#include <QString>
#include "tgstream.h"

struct DHKey
{
    QByteArray publicKey;
    QByteArray exponent;
    qint64 fingerprint;

    DHKey(QString publicKey, qint64 fingerprint, QString exponent = "010001");
};

qint32 randomInt(qint32 lowerThan);
QByteArray randomBytes(qint32 size);
quint64 findDivider(quint64 number);
QByteArray reverse(QByteArray array);
QByteArray xorArray(QByteArray a, QByteArray b);
QByteArray decryptAES256IGE(QByteArray data, QByteArray iv, QByteArray key);
QByteArray encryptAES256IGE(QByteArray data, QByteArray iv, QByteArray key);
QByteArray encryptRSA(QByteArray data, QByteArray key, QByteArray exp);
QByteArray hashSHA256(QByteArray dataToHash);
QByteArray hashSHA1(QByteArray dataToHash);
// `client` selects which half of the auth key the message key is derived from:
// substring 88 for client-to-server, 96 for server-to-client. It has no default
// on purpose -- the same packet must reach calcEncryptionKey() with the same
// value, and a direction that can be omitted is a direction that gets omitted.
QByteArray calcMessageKey(QByteArray authKey, QByteArray data, bool client);
QByteArray calcEncryptionKey(QByteArray sharedKey, QByteArray msgKey, QByteArray &iv, bool client);
QByteArray rsaPad(QByteArray data, DHKey key);

// Progress and cancellation for a derivation slow enough that the interface
// has to account for it. step() returns false to abandon the run: the SRP
// parameters it feeds expire server-side, so a computation the user has walked
// away from is worth nothing and must not hold the password any longer.
class Pbkdf2Sink
{
public:
    virtual ~Pbkdf2Sink();
    virtual bool step(qint32 done, qint32 total) = 0;
};

QByteArray hashSHA512(QByteArray dataToHash);
QByteArray hmacSHA512(QByteArray key, QByteArray data);
// MBEDTLS_PKCS5_C is off in the pinned vendored configuration, so
// mbedtls_pkcs5_pbkdf2_hmac_ext compiles to nothing, and tools/verify-vendored.py
// checks that tree byte for byte -- so the loop lives here rather than being
// bought with a one-line call. Returns an empty array if the sink cancels.
QByteArray pbkdf2HmacSHA512(QByteArray password, QByteArray salt,
                            qint32 iterations, qint32 outputLength,
                            Pbkdf2Sink *sink = 0);
qint64 randomLong();

#endif // CRYPTO_H
