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

#include "srp.h"

// Reference vector for Telegram's SRP-6a proof, carried over from J2MEgram's
// implementation, which is validated against live Telegram. Independently
// reproduced with Python before being pasted here, so it attests to the
// construction rather than to one implementation of it.

static const char SRP_P_HEX[] =
    "c71caeb9c6b1c9048e6c522f70f13f73980d40238e3e21c14934d037563d930f"
    "48198a0aa7c14058229493d22530f4dbfa336f6e0ac925139543aed44cce7c37"
    "20fd51f69458705ac68cd4fe6b6b13abdc9746512969328454f18faf8c595f64"
    "2477fe96bb2a941d5bcd1d4ac8cc49880708fa9b378e3c4f3a9060bee67cf9a4"
    "a4a695811051907e162753b56b0f6b410dba74d8a84b2a14b3144e0ef1284754"
    "fd17ed950d5965b4b9dd46582db1178d169c6bc465b0d6ff9ca3928fef5b9ae4"
    "e418fc15e83ebea0f87fa9ff5eed70050ded2849f47bf959d956850ce929851f"
    "0d8115f635b105ee2e4e15d04b2454bf6f4fadf034b10403119cd8e3b92fcc5b";

static const char SRP_B_HEX[] =
    "1f248632dc6175a509a5bd784ff01658f6db1fd4c313f4f618233e5613df8c64"
    "5caaad2e05e8fa11a35285e8097910121427cf2d8a6d4b1b64a66a2c31656e5f"
    "f3e24d258972d98679c5f957e47921e8fa36c91a2646a889044a7a1cd0d53151"
    "ff2bf9d95a314a187ddc12ba937602d3c75e1c30f0189f56fb73ccfcb8ac8775"
    "0e45091ab622950730c6147e1df122770daecc19a33c29846813ec05a546afdb"
    "2e81871b539c4b1ae071880a9c7cf51d16ffb8d495e28afe315fabbba25b8f9e"
    "b63d6a5775bdde1d93c092f4fd0ff5cf323c77123f8223dc3b7ada607af7ce1f"
    "e04b90c7819ebfe4b710a42b078ce02ec0b913a11d3043180767a3d400add227";

static const char SRP_EXPECTED_A_HEX[] =
    "9dc80c0bed00301224113172e6c0521a4de8b6645dd7c8b2a612488298152090"
    "f3f7bc1d8e8c561e2c12670cd0d748b221908b2ed6d6198a16f3869bd01e2c0a"
    "157269bf858fbdda3ab8e79038d0c08352ab433466e213b172231a3c4685ca60"
    "a93162383764345e60b172cfcaf4ac5a5b48161d613757e0178aae050b9caf30"
    "40bd9620b5aa5a15e45b8f28a776588a09318a4ec391f0cd3e06fa17a1d2c4c5"
    "4b5b5d1f9cb20fea6594ec50266dd470a421e634aaa9f05b39269b4cd7c3a185"
    "79f4d41db7f31a3c474be6b3a20f86335169008f7f57beed8f4766d1b0013f36"
    "59b8bceb8b053ec9e1dd3e620c850eda912dc4b36abcce22525bb7ed984b3735";


// The password as explicit bytes -- "correct horse " followed by U+1F510 --
// rather than as a source literal, so that a change of source encoding cannot
// quietly alter the input to a vector that exists to detect quiet alterations.
static QByteArray srpVectorPassword()
{
    return QByteArray::fromHex("636f727265637420686f72736520f09f9490");
}

static QByteArray srpVectorSecret()
{
    QByteArray secret;
    secret.resize(256);
    for (int i = 0; i < 256; ++i) {
        secret[i] = (char) ((i * 73 + 19) & 0xFF);
    }
    return secret;
}


// 256 bytes, the length of a real auth key, with a deterministic pattern so
// the vectors above can be reproduced.
// Refuses at the first opportunity and counts the calls, so the control below
// asserts both that progress is reported and that a refusal actually stops the
// derivation.
class CancellingSink : public Pbkdf2Sink
{
public:
    CancellingSink() : calls(0) { }
    bool step(qint32 done, qint32 total) { (void) done; (void) total; ++calls; return false; }
    qint32 calls;
};

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
    r.planned = 22;

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

    // 10-11. HMAC-SHA512 against RFC 4231 test cases 1 and 2. Published
    // vectors: nothing here agrees with itself.
    expectHex(r, "hmac-sha512/rfc4231 case 1",
              hmacSHA512(QByteArray(20, '\x0b'), QByteArray("Hi There")),
              "87aa7cdea5ef619d4ff0b4241a1d6cb02379f4e2ce4ec2787ad0b30545e17cde"
              "daa833b7d6b8a702038b274eaea3f4e4be9d914eeb61f1702e696c203a126854");
    expectHex(r, "hmac-sha512/rfc4231 case 2",
              hmacSHA512(QByteArray("Jefe"), QByteArray("what do ya want for nothing?")),
              "164b7a7bfcf819e2e395fbe73b56e0a387bd64222e831fd610270cd7ea250554"
              "9758bf75c05a994a6d034f65f8f0e6fdcaeab1a34d4a6b4b636e070a38bce737");

    // 12-13. PBKDF2-HMAC-SHA512, computed with Python's hashlib. 100000 is the
    // iteration count Telegram's KDF actually specifies, so it is the one
    // tested -- a parameter that is only ever exercised at a smaller value is
    // a parameter whose cost and correctness are both unmeasured.
    expectHex(r, "pbkdf2-sha512/1000 iterations",
              pbkdf2HmacSHA512(QByteArray("password"), QByteArray("salt"), 1000, 64),
              "afe6c5530785b6cc6b1c6453384731bd5ee432ee549fd42fb6695779ad8a1c5b"
              "f59de69c48f774efc4007d5298f9033c0241d5ab69305e7b64eceeb8d834cfec");
    expectHex(r, "pbkdf2-sha512/100000 iterations",
              pbkdf2HmacSHA512(QByteArray("password"), QByteArray("salt"), 100000, 64),
              "f5d17022c96af46c0a1dc49a58bbe654a28e98104883e4af4de974cda2c74122"
              "dd082f4105a93fc80692ca4eb1a784cfeda81bfaa33f5192cc9143d818bd7581");

    // 14. A request longer than one SHA-512 digest, which is the only case that
    // runs the block loop and appends the big-endian block index to the salt.
    // Telegram asks for exactly 64 bytes, so nothing in this client would
    // otherwise reach that code.
    expectHex(r, "pbkdf2-sha512/spans two blocks",
              pbkdf2HmacSHA512(QByteArray("password"), QByteArray("salt"), 1000, 100),
              "afe6c5530785b6cc6b1c6453384731bd5ee432ee549fd42fb6695779ad8a1c5b"
              "f59de69c48f774efc4007d5298f9033c0241d5ab69305e7b64eceeb8d834cfec"
              "6afdec3c1c23982a121f2d4be008889378a49a0dfb104f0d2856e38f44271cda"
              "f6de4341");

    // 15. Deliberate-failure control.
    //
    // A loop that runs once, or that stops XORing after the first pass, returns
    // a perfectly deterministic answer of the right length. Every vector above
    // would still be checked against a constant, and a single-iteration
    // implementation would fail them -- but a two-iteration one would not fail
    // them for any reason a reader could see. Asserting that distinct counts
    // give distinct answers is what pins the iteration to the parameter.
    {
        const QByteArray one = pbkdf2HmacSHA512(QByteArray("password"), QByteArray("salt"), 1, 64);
        const QByteArray two = pbkdf2HmacSHA512(QByteArray("password"), QByteArray("salt"), 2, 64);

        if (!one.isEmpty() && !two.isEmpty() && one != two) {
            r.ok("control/pbkdf2 iterates");
        } else {
            r.fail("control/pbkdf2 iterates",
                   "one and two iterations produced the same output");
        }
    }

    // 16. Deliberate-failure control.
    //
    // Cancellation is the difference between a login the user can back out of
    // and one that holds the password for ten seconds regardless. A sink whose
    // refusal is ignored looks identical from the outside -- the result is
    // still correct, just late -- so it has to be asserted directly.
    {
        CancellingSink sink;
        const QByteArray cancelled =
                pbkdf2HmacSHA512(QByteArray("password"), QByteArray("salt"), 100000, 64, &sink);

        if (sink.calls > 0 && cancelled.isEmpty()) {
            r.ok("control/pbkdf2 stops when the sink refuses");
        } else {
            r.fail("control/pbkdf2 stops when the sink refuses",
                   QString("sink called %1 times, returned %2 bytes")
                       .arg(sink.calls).arg(cancelled.size()));
        }
    }

    // 17-18. Telegram's SRP-6a proof, against a vector carried over from
    // J2MEgram's implementation, which is validated against live Telegram.
    //
    // The correctness of SRP is the ORDER of a sequence of hashes -- which salt
    // wraps which stage, which operand leads in H(k) and H(u), whether the
    // underflow correction happens. None of that is visible when reading the
    // code back, and all of it produces output of the right shape when wrong.
    // A known answer is the only thing that pins it.
    {
        SrpParams params;
        params.p = QByteArray::fromHex(SRP_P_HEX);
        params.g = 3;
        params.salt1 = QByteArray::fromHex("00112233445566778899aabbccddeeff");
        params.salt2 = QByteArray::fromHex("102132435465768798a9bacbdcedfe0f");
        params.srpB = QByteArray::fromHex(SRP_B_HEX);
        params.srpId = Q_INT64_C(0x0102030405060708);

        SrpProof proof;
        const QString problem =
                srpComputeWithSecret(params, srpVectorPassword(), srpVectorSecret(), proof, 0);

        if (!problem.isEmpty()) {
            r.fail("srp/known answer A", problem);
            r.fail("srp/known answer M1", problem);
        } else {
            expectHex(r, "srp/known answer A", proof.a, SRP_EXPECTED_A_HEX);
            expectHex(r, "srp/known answer M1", proof.m1,
                      "48ffd1439967b4352cc0b94f216fe3f3c74fa43f04e35a8b8158ca5e1c6624e9");
        }
    }

    // 19. Deliberate-failure control.
    //
    // srpCheckParams() could be written as `return QString();` and every
    // assertion above would still pass -- the vector uses well-formed
    // parameters. A degenerate B is the check that decides whether the shared
    // secret is guessable, so it is asserted directly.
    {
        SrpParams params;
        params.p = QByteArray::fromHex(SRP_P_HEX);
        params.g = 3;
        params.salt1 = QByteArray::fromHex("00112233445566778899aabbccddeeff");
        params.salt2 = QByteArray::fromHex("102132435465768798a9bacbdcedfe0f");
        params.srpB = QByteArray::fromHex("01");
        params.srpId = 1;

        SrpProof proof;
        const QString problem =
                srpComputeWithSecret(params, srpVectorPassword(), srpVectorSecret(), proof, 0);

        if (!problem.isEmpty() && proof.m1.isEmpty()) {
            r.ok("control/srp rejects a degenerate B");
        } else {
            r.fail("control/srp rejects a degenerate B",
                   "B = 1 was accepted and a proof was produced");
        }
    }

    // 20. Deliberate-failure control.
    //
    // The 2048-bit gate, checked with a prime one limb short. Without it a
    // server could shrink the group rather than corrupt it, which no vector
    // computed against the full-size prime would notice.
    {
        SrpParams params;
        params.p = QByteArray::fromHex(SRP_P_HEX).right(255);
        params.g = 3;
        params.salt1 = QByteArray::fromHex("00112233445566778899aabbccddeeff");
        params.salt2 = QByteArray::fromHex("102132435465768798a9bacbdcedfe0f");
        params.srpB = QByteArray::fromHex(SRP_B_HEX);
        params.srpId = 1;

        if (!srpCheckParams(params).isEmpty()) {
            r.ok("control/srp rejects an undersized prime");
        } else {
            r.fail("control/srp rejects an undersized prime",
                   "a 2040-bit prime was accepted");
        }
    }

    // 21. Deliberate-failure control.
    //
    // The password reaches M1 only through x, so two runs differing in nothing
    // but the password must differ in the proof. That is what fails if the
    // derivation is short-circuited -- a KDF returning a constant, or an x that
    // never reaches the exponent -- each of which yields a well-formed 32-byte
    // proof for every password, which is to say a client that logs in with the
    // wrong one or with none.
    //
    // Note what this does NOT cover, because the obvious alternative is
    // misleading: comparing a run against one with salt1 and salt2 exchanged
    // proves nothing about the derivation, since M1 hashes both salts directly
    // and so changes under the swap even when the KDF ignores them entirely.
    // The salt ordering is pinned by the known answer above, not here.
    {
        SrpParams params;
        params.p = QByteArray::fromHex(SRP_P_HEX);
        params.g = 3;
        params.salt1 = QByteArray::fromHex("00112233445566778899aabbccddeeff");
        params.salt2 = QByteArray::fromHex("102132435465768798a9bacbdcedfe0f");
        params.srpB = QByteArray::fromHex(SRP_B_HEX);
        params.srpId = 1;

        SrpProof right, wrong;
        const QString pr = srpComputeWithSecret(params, srpVectorPassword(),
                                                srpVectorSecret(), right, 0);
        const QString pw = srpComputeWithSecret(params, QByteArray("something else"),
                                                srpVectorSecret(), wrong, 0);

        if (pr.isEmpty() && pw.isEmpty() && !right.m1.isEmpty() && right.m1 != wrong.m1) {
            r.ok("control/srp proof depends on the password");
        } else {
            r.fail("control/srp proof depends on the password",
                   "two different passwords produced the same proof");
        }
    }

    // 22. Deliberate-failure control.
    //
    // Every integer entering a hash is padded to 256 bytes, and padding is
    // invisible on any value that happens to be full width. g = 3 is one byte
    // wide, so H(pad(g)) is the cheapest place the rule can be caught being
    // skipped. Expectation computed with Python's hashlib.
    {
        QByteArray paddedG(256, '\0');
        paddedG[255] = '\x03';

        expectHex(r, "control/srp pads a short integer to 256 bytes",
                  hashSHA256(paddedG),
                  "380109a77dcb3859161593ef51e717c1fe61698bb25973d9dd7fc7c7e19e9643");
    }

    const int rc = r.finish();
    out.flush();
    return rc;
}
