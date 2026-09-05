// Offline checks for the MTProto key derivation and block cipher.
//
// Why this target exists separately from tests/tlcodec: the codec suite proves
// that bytes survive a round trip, and says nothing about whether the bytes
// were derived from the right half of the auth key. A direction bug there is
// invisible to every offline test and presents, live, as a connection that
// rejects every packet the server sends -- or, far worse in the other
// direction, as one that accepts packets it should not.
//
// The expected values below are computed independently, with Python's hashlib
// against the formulas in the MTProto specification, and pasted in. That is the
// point: a test whose expectation is recomputed by the code under test passes
// whenever the code is self-consistent, including when it is consistently
// wrong.

#include <QByteArray>
#include <QCoreApplication>
#include <QTextStream>

#include "crypto.h"
#include "tlcase.h"

// 256 bytes, the length of a real auth key, with a deterministic pattern so
// the vectors above can be reproduced.
static QByteArray referenceAuthKey()
{
    QByteArray key;
    key.resize(256);
    for (int i = 0; i < 256; ++i) {
        key[i] = (char) ((i * 7 + 13) & 0xFF);
    }
    return key;
}

static QByteArray referenceData()
{
    QByteArray data;
    data.resize(64);
    for (int i = 0; i < 64; ++i) {
        data[i] = (char) ((i * 11 + 5) & 0xFF);
    }
    return data;
}

static void expectHex(TlReport &r, const char *name,
                      const QByteArray &got, const char *expectHexStr)
{
    const QByteArray want = QByteArray::fromHex(expectHexStr);
    if (got == want) {
        r.ok(QString::fromLatin1(name));
        return;
    }
    r.fail(QString::fromLatin1(name),
           QString("expected %1, got %2")
               .arg(QString::fromLatin1(want.toHex()))
               .arg(QString::fromLatin1(got.toHex())));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    TlReport r;
    r.planned = 9;

    const QByteArray key  = referenceAuthKey();
    const QByteArray data = referenceData();

    // 1-2. The two directions read different 32-byte fragments of the auth key:
    // substring 88 outbound, 96 inbound. These are known answers, so getting
    // the offset wrong fails here rather than against a live data centre.
    const QByteArray mkClient = calcMessageKey(key, data, true);
    const QByteArray mkServer = calcMessageKey(key, data, false);

    expectHex(r, "msg_key/client direction uses substring 88",
              mkClient, "dfe16816e4cb5abd8d28f569bcda03f3");
    expectHex(r, "msg_key/server direction uses substring 96",
              mkServer, "df6ddf8983bfae36086536dc15085361");

    // 3. The direction argument has to reach the computation. A parameter that
    // is accepted and ignored compiles, passes review and produces one key for
    // both directions -- which is exactly the bug the two cases above cannot
    // distinguish from a coincidence.
    if (mkClient != mkServer) {
        r.ok("msg_key/directions differ");
    } else {
        r.fail("msg_key/directions differ",
               "both directions produced the same key; the argument is ignored");
    }

    // 4-7. Same property for the encryption key and IV, which take the same
    // bit and would be just as silently wrong.
    QByteArray ivClient;
    const QByteArray keyClient = calcEncryptionKey(key, mkClient, ivClient, true);
    QByteArray ivServer;
    const QByteArray keyServer = calcEncryptionKey(key, mkServer, ivServer, false);

    expectHex(r, "enc_key/client",
              keyClient, "4776d004c923f113c18bb0cefd96721786a0a75052c190b1ed61c9ff04822f38");
    expectHex(r, "enc_iv/client",
              ivClient, "2b8b9a88de48f32d3847e3b4aff12fe8b4acd920345e16397d9e0e98bbb8ca7f");
    expectHex(r, "enc_key/server",
              keyServer, "ea8d69dd54cd3591852038ef2258664693a7636602b564cda8394ca3f8f9928c");
    expectHex(r, "enc_iv/server",
              ivServer, "91c41570c175e923ecc0ad916e69966143662bbdc93c3f9ed71fc9a6eecbd443");

    // 8. Round trip through AES-256-IGE. IV is consumed in place by mbedtls, so
    // each direction gets its own copy -- reusing one would decrypt to garbage
    // and the failure would read as a cipher bug.
    {
        QByteArray plain;
        plain.resize(64);
        for (int i = 0; i < 64; ++i) {
            plain[i] = (char) ((i * 3 + 1) & 0xFF);
        }

        QByteArray encIv = ivClient;
        const QByteArray cipher = encryptAES256IGE(plain, encIv, keyClient);

        QByteArray decIv = ivClient;
        const QByteArray back = decryptAES256IGE(cipher, decIv, keyClient);

        if (back == plain) {
            r.ok("aes-ige/round trip");
        } else {
            r.fail("aes-ige/round trip",
                   QString("expected %1, got %2")
                       .arg(QString::fromLatin1(plain.toHex()))
                       .arg(QString::fromLatin1(back.toHex())));
        }
    }

    // 9. Deliberate-failure control.
    //
    // IGE is defined over whole 16-byte blocks and mbedtls refuses anything
    // else. If the return code is dropped, the output buffer is returned
    // uninitialised and whatever happened to be on the heap is parsed as an
    // MTProto message -- which is not a crash, and so is not noticed.
    //
    // A suite with no control prints the same green lines as one that checks
    // nothing; this case is what tells the two apart.
    {
        QByteArray partial;
        partial.resize(17);
        partial.fill('\x41');

        QByteArray iv = ivClient;
        const QByteArray outBytes = decryptAES256IGE(partial, iv, keyClient);

        if (outBytes.isEmpty()) {
            r.ok("control/aes-ige rejects a partial block");
        } else {
            r.fail("control/aes-ige rejects a partial block",
                   QString("returned %1 bytes for a 17-byte input")
                       .arg(outBytes.size()));
        }
    }

    const int rc = r.finish();
    out.flush();
    return rc;
}
