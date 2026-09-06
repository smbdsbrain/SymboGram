#include "crypto.h"

#include <QDateTime>
#include <QtCore>
#include "mtschema.h"
#include "debug.h"
#include <mbedtls/aes.h>
#include <mbedtls/bignum.h>
#include <mbedtls/sha256.h>
#include <mbedtls/sha1.h>
#include <mbedtls/sha512.h>
#include <mbedtls/md.h>
#include <mbedtls/entropy.h>
#include <mbedtls/ctr_drbg.h>

qint64 randomLong()
{
    return qFromLittleEndian<qint64>((const uchar*) randomBytes(INT64_BYTES).constData());
}

DHKey::DHKey(QString publicKey, qint64 fingerprint, QString exponent) :
    publicKey(QByteArray::fromHex(publicKey.toLatin1())),
    exponent(QByteArray::fromHex(exponent.toLatin1())),
    fingerprint(fingerprint)
{
	//TODO: use mbedtls import
    if (!this->publicKey.isEmpty() && this->publicKey.at(0) == 0) {
        this->publicKey.remove(0, 1);
    }

    if (this->fingerprint == 0) {
        TgPacket packet;
        writeByteArray(packet, this->publicKey);
        writeByteArray(packet, this->exponent);

        QByteArray result = hashSHA1(packet.toByteArray()).mid(12, 8);
        this->fingerprint = qFromLittleEndian<qint64>((const uchar*) result.constData());
    }
}

qint32 randomInt(qint32 lowerThan)
{
    if (lowerThan < 1)
        return 0;

    QByteArray array = randomBytes(4);
    if (array.size() < 4) {
        return 0;
    }

    return qAbs(qFromBigEndian<qint32>((uchar*) array.data())) % lowerThan;
}

QByteArray randomBytes(qint32 size)
{
    mbedtls_entropy_context entropy;
    mbedtls_entropy_init(&entropy);

    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_ctr_drbg_init(&ctr_drbg);

    qint32 ret = mbedtls_ctr_drbg_seed(&ctr_drbg, mbedtls_entropy_func, &entropy,
                                       // NB: the length below is hardcoded, so this
                                       // string must stay exactly 20 bytes.
                                       (const unsigned char *) "symbogram_random_lol", 20);
    if (ret != 0)
    {
        kgCritical() << "Mbed TLS random error:" << ret;
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);

        return QByteArray();
    }

    QByteArray array;
    array.resize(size);

    ret = mbedtls_ctr_drbg_random(&ctr_drbg, (unsigned char *) array.data(), array.size());
    if (ret != 0) {
        kgCritical() << "Mbed TLS random error:" << ret;
        array.fill(0);

        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);

        return QByteArray();
    }

    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);

    return array;
}

quint64 gcd(quint64 a, quint64 b) {
	//TODO: use mbedtls RSA
    if (a == 0) {
        return b;
    }
    if (b == 0) {
        return a;
    }

    int shift = 0;
    while ((a & 1) == 0 && (b & 1) == 0) {
        a >>= 1;
        b >>= 1;
        shift++;
    }

    while (true) {
        while ((a & 1) == 0) {
            a >>= 1;
        }
        while ((b & 1) == 0) {
            b >>= 1;
        }
        if (a > b) {
            a -= b;
        } else if (b > a) {
            b -= a;
        } else {
            return a << shift;
        }
    }
}

quint64 findDivider(quint64 number)
{
	//TODO: use mbedtls RSA
    qsrand(QDateTime::currentDateTime().toUTC().toTime_t());
    int it = 0;
    quint64 g = 0;
    for (int i = 0; i < 3 || it < 10000; ++i) {
        const quint64 q = ((qrand() & 15) + 17) % number;
        quint64 x = (quint64) qrand() % (number - 1) + 1;
        quint64 y = x;
        const quint32 lim = 1 << (i + 18);
        for (quint32 j = 1; j < lim; j++) {
            ++it;
            quint64 a = x;
            quint64 b = x;
            quint64 c = q;
            while (b) {
                if (b & 1) {
                    c += a;
                    if (c >= number) {
                        c -= number;
                    }
                }
                a += a;
                if (a >= number) {
                    a -= number;
                }
                b >>= 1;
            }
            x = c;
            const quint64 z = x < y ? number + x - y : x - y;
            g = gcd(z, number);
            if (g != 1) {
                return g;
            }
            if (!(j & (j - 1))) {
                y = x;
            }
        }

        if (g > 1 && g < number) {
            return g;
        }
    }

    return 1;
}

QByteArray reverse(QByteArray array)
{
    for (int low = 0, high = array.size() - 1; low < high; ++low, --high) {
        qSwap(array.data()[low], array.data()[high]);
    }
    return array;
}

QByteArray xorArray(QByteArray a, QByteArray b)
{
    QByteArray result(a.size() > b.size() ? a : b);
    qint32 minLength = a.size() > b.size() ? b.size() : a.size();

    for (qint32 i = 0; i < minLength; ++i) {
        result[i] = (a[i] ^ b[i]);
    }

    return result;
}

QByteArray decryptAES256IGE(QByteArray data, QByteArray iv, QByteArray key)
{
    QByteArray output;
    output.resize(data.size());

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, (const unsigned char*) key.constData(), 256);
    // IGE requires a whole number of 16-byte blocks and mbedtls refuses
    // anything else. Returning the buffer regardless would hand the caller
    // uninitialised memory to parse as a message.
    qint32 rc = mbedtls_aes_crypt_ige(&aes, MBEDTLS_AES_DECRYPT, data.size(), (unsigned char*) iv.data(), (const unsigned char*) data.constData(), (unsigned char*) output.data());
    mbedtls_aes_free(&aes);

    if (rc != 0) {
        return QByteArray();
    }

    return output;
}

QByteArray encryptAES256IGE(QByteArray data, QByteArray iv, QByteArray key)
{
    QByteArray output;
    output.resize(data.size());

    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, (const unsigned char*) key.constData(), 256);
    mbedtls_aes_crypt_ige(&aes, MBEDTLS_AES_ENCRYPT, data.size(), (unsigned char*) iv.data(), (const unsigned char*) data.constData(), (unsigned char*) output.data());
    mbedtls_aes_free(&aes);

    return output;
}

QByteArray encryptRSA(QByteArray data, QByteArray key, QByteArray exp)
{
	//TODO: use mbedtls RSA
    mbedtls_mpi a, e, n, r;
    mbedtls_mpi_init(&a);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&r);

    mbedtls_mpi_read_binary(&a, (const unsigned char*) data.constData(), data.size());
    mbedtls_mpi_read_binary(&e, (const unsigned char*) exp.constData(),  exp.size());
    mbedtls_mpi_read_binary(&n, (const unsigned char*) key.constData(),  key.size());

    QByteArray resultArray;

    qint32 result = mbedtls_mpi_exp_mod(&r, &a, &e, &n, 0);
    if (result == 0) {
        resultArray.resize(mbedtls_mpi_size(&r));
        mbedtls_mpi_write_binary(&r, (unsigned char*) resultArray.data(), resultArray.size());
    }

    mbedtls_mpi_free(&a);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&n);
    mbedtls_mpi_free(&r);

    return resultArray;
}

QByteArray hashSHA256(QByteArray dataToHash)
{
    QByteArray hash;
    hash.resize(32);

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, false);
    mbedtls_sha256_update(&ctx, (const unsigned char*) dataToHash.constData(), dataToHash.size());
    mbedtls_sha256_finish(&ctx, (unsigned char*) hash.data());
    mbedtls_sha256_free(&ctx);

    return hash;
}

QByteArray hashSHA1(QByteArray dataToHash)
{
    QByteArray hash;
    hash.resize(20);

    mbedtls_sha1_context ctx;
    mbedtls_sha1_init(&ctx);
    mbedtls_sha1_starts(&ctx);
    mbedtls_sha1_update(&ctx, (const unsigned char*) dataToHash.constData(), dataToHash.size());
    mbedtls_sha1_finish(&ctx, (unsigned char*) hash.data());
    mbedtls_sha1_free(&ctx);

    return hash;
}

QByteArray calcMessageKey(QByteArray authKey, QByteArray data, bool client)
{
    // The two directions derive the message key from different 32-byte
    // fragments of the auth key, so verifying an inbound packet with the
    // outbound fragment rejects every legitimate message.
    return hashSHA256(authKey.mid(client ? 88 : 96, 32) + data).mid(8, 16);
}

QByteArray calcEncryptionKey(QByteArray authKey, QByteArray msgKey, QByteArray &iv, bool client)
{
    qint32 x = client ? 0 : 8;

    QByteArray sha256A = hashSHA256(msgKey + authKey.mid(x, 36));
    QByteArray sha256B = hashSHA256(authKey.mid(40 + x, 36) + msgKey);

    iv = sha256B.mid(0, 8) + sha256A.mid(8, 16) + sha256B.mid(24, 8);

    return sha256A.mid(0, 8) + sha256B.mid(8, 16) + sha256A.mid(24, 8);
}

qint8 compareAsBigEndian(QByteArray a, QByteArray b)
{
    if (a.length() != b.length())
        return a.length() > b.length() ? 1 : -1;

    for (qint32 i = 0; i < a.length(); ++i) {
        if ((quint8) a[i] > (quint8) b[i])
            return 1;
        if ((quint8) b[i] > (quint8) a[i])
            return -1;
    }

    return 0;
}

QByteArray rsaPad(QByteArray data, DHKey key)
{
    QByteArray dataWithPadding = data + randomBytes(192 - data.length());
    QByteArray dataPadReversed = reverse(dataWithPadding);

    QByteArray keyAesEncrypted;
    do {
        QByteArray tempKey = randomBytes(32);
        QByteArray dataWithHash = dataPadReversed + hashSHA256(tempKey + dataWithPadding);
        QByteArray aesEncrypted = encryptAES256IGE(dataWithHash, QByteArray(32, 0), tempKey);
        QByteArray tempKeyXor = xorArray(tempKey, hashSHA256(aesEncrypted));
        keyAesEncrypted = tempKeyXor + aesEncrypted;
    } while (compareAsBigEndian(keyAesEncrypted, key.publicKey) != -1);

    return encryptRSA(keyAesEncrypted, key.publicKey, key.exponent);
}

Pbkdf2Sink::~Pbkdf2Sink()
{
}

QByteArray hashSHA512(QByteArray dataToHash)
{
    QByteArray hash;
    hash.resize(64);

    mbedtls_sha512_context ctx;
    mbedtls_sha512_init(&ctx);
    mbedtls_sha512_starts(&ctx, false);
    mbedtls_sha512_update(&ctx, (const unsigned char*) dataToHash.constData(), dataToHash.size());
    mbedtls_sha512_finish(&ctx, (unsigned char*) hash.data());
    mbedtls_sha512_free(&ctx);

    return hash;
}

QByteArray hmacSHA512(QByteArray key, QByteArray data)
{
    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    if (info == 0) {
        return QByteArray();
    }

    QByteArray mac;
    mac.resize(64);

    if (mbedtls_md_hmac(info,
                        (const unsigned char*) key.constData(), key.size(),
                        (const unsigned char*) data.constData(), data.size(),
                        (unsigned char*) mac.data()) != 0) {
        return QByteArray();
    }

    return mac;
}

QByteArray pbkdf2HmacSHA512(QByteArray password, QByteArray salt,
                            qint32 iterations, qint32 outputLength,
                            Pbkdf2Sink *sink)
{
    const qint32 HASH_LENGTH = 64;
    const qint32 REPORT_EVERY = 5000;

    const mbedtls_md_info_t *info = mbedtls_md_info_from_type(MBEDTLS_MD_SHA512);
    if (info == 0 || iterations <= 0 || outputLength <= 0) {
        return QByteArray();
    }

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);

    //The 1 asks for an HMAC context. It is what lets the key be scheduled once
    //by hmac_starts and reused by hmac_reset: at 100000 iterations, expanding
    //ipad and opad per iteration is the difference between a wait and a hang.
    if (mbedtls_md_setup(&ctx, info, 1) != 0) {
        mbedtls_md_free(&ctx);
        return QByteArray();
    }

    const qint32 blocks = (outputLength + HASH_LENGTH - 1) / HASH_LENGTH;
    const qint32 totalSteps = blocks * iterations;

    QByteArray out;
    out.resize(outputLength);

    unsigned char u[64];
    unsigned char t[64];
    unsigned char counter[4];

    qint32 stepsDone = 0;
    bool failed = false;

    for (qint32 block = 1; block <= blocks && !failed; ++block) {
        //The block index is appended to the salt big-endian, per RFC 2898.
        counter[0] = (unsigned char) ((block >> 24) & 0xFF);
        counter[1] = (unsigned char) ((block >> 16) & 0xFF);
        counter[2] = (unsigned char) ((block >> 8) & 0xFF);
        counter[3] = (unsigned char) (block & 0xFF);

        if (mbedtls_md_hmac_starts(&ctx, (const unsigned char*) password.constData(), password.size()) != 0
                || mbedtls_md_hmac_update(&ctx, (const unsigned char*) salt.constData(), salt.size()) != 0
                || mbedtls_md_hmac_update(&ctx, counter, 4) != 0
                || mbedtls_md_hmac_finish(&ctx, u) != 0) {
            failed = true;
            break;
        }

        memcpy(t, u, HASH_LENGTH);
        ++stepsDone;

        for (qint32 i = 1; i < iterations; ++i) {
            if (mbedtls_md_hmac_reset(&ctx) != 0
                    || mbedtls_md_hmac_update(&ctx, u, HASH_LENGTH) != 0
                    || mbedtls_md_hmac_finish(&ctx, u) != 0) {
                failed = true;
                break;
            }

            for (qint32 j = 0; j < HASH_LENGTH; ++j) {
                t[j] ^= u[j];
            }

            ++stepsDone;

            //Reporting every iteration would post one event per HMAC and cost
            //more than the HMAC does.
            if (sink != 0 && (stepsDone % REPORT_EVERY) == 0
                    && !sink->step(stepsDone, totalSteps)) {
                failed = true;
                break;
            }
        }

        if (failed) {
            break;
        }

        const qint32 offset = (block - 1) * HASH_LENGTH;
        const qint32 take = qMin(HASH_LENGTH, outputLength - offset);
        memcpy(out.data() + offset, t, take);
    }

    //u and t carry the derived key, which is the password in another form.
    memset(u, 0, sizeof(u));
    memset(t, 0, sizeof(t));
    mbedtls_md_free(&ctx);

    if (failed) {
        memset(out.data(), 0, out.size());
        return QByteArray();
    }

    return out;
}
