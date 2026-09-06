#include "srp.h"

#include <string.h>

#include <mbedtls/bignum.h>

#include "debug.h"

// The prime Telegram serves today, from core.telegram.org/mtproto/auth_key,
// verified there as a safe 2048-bit prime. A byte-exact match is what licenses
// skipping the primality test below: proving a 2048-bit number prime -- twice,
// for p and (p-1)/2 -- costs tens of modular exponentiations, and on the ARM11
// this targets each one of those is seconds.
static const char KNOWN_GOOD_PRIME[] =
    "c71caeb9c6b1c9048e6c522f70f13f73980d40238e3e21c14934d037563d930f"
    "48198a0aa7c14058229493d22530f4dbfa336f6e0ac925139543aed44cce7c37"
    "20fd51f69458705ac68cd4fe6b6b13abdc9746512969328454f18faf8c595f64"
    "2477fe96bb2a941d5bcd1d4ac8cc49880708fa9b378e3c4f3a9060bee67cf9a4"
    "a4a695811051907e162753b56b0f6b410dba74d8a84b2a14b3144e0ef1284754"
    "fd17ed950d5965b4b9dd46582db1178d169c6bc465b0d6ff9ca3928fef5b9ae4"
    "e418fc15e83ebea0f87fa9ff5eed70050ded2849f47bf959d956850ce929851f"
    "0d8115f635b105ee2e4e15d04b2454bf6f4fadf034b10403119cd8e3b92fcc5b";

// Fixed by Telegram's KDF; not ours to tune.
static const qint32 PBKDF2_ITERATIONS = 100000;

// Rounds used only when the server sends a prime that is not the known one.
// The rounds are not where the security is: a random composite dies in the
// small-prime sieve or the first round, and a composite chosen against us is
// what the sieve plus any round already answers. More rounds would mostly
// lengthen a wait the user is already staring at.
static const int MILLER_RABIN_ROUNDS = 8;

// The lower bound MTProto recommends for a public value: 2^(2048-64). A B just
// above 1, or just below p, leaves the shared secret in a set small enough to
// enumerate.
static const size_t SAFE_BITS = 2048 - 64;

static const qint32 MAX_SECRET_ATTEMPTS = 8;

// Marks the one failure srpCompute() answers by drawing again rather than by
// giving up, and is matched against rather than shown.
static const char UNSAFE_A[] = "SRP-A-UNSAFE";

SrpParams::SrpParams() : g(0), srpId(0)
{
}

SrpProof::SrpProof() : srpId(0)
{
}

static int srpRandom(void *context, unsigned char *output, size_t length)
{
    (void) context;

    const QByteArray bytes = randomBytes((qint32) length);
    if (bytes.size() != (int) length) {
        return -1;
    }

    memcpy(output, bytes.constData(), length);
    return 0;
}

// SHA256(salt | data | salt). The salt is on BOTH sides; a one-sided hash is a
// different function that looks equally plausible on the page.
static QByteArray saltedHash(const QByteArray &data, const QByteArray &salt)
{
    return hashSHA256(salt + data + salt);
}

static void wipe(QByteArray &value)
{
    if (!value.isEmpty()) {
        memset(value.data(), 0, value.size());
    }

    value.clear();
}

static bool padTo256(const mbedtls_mpi *value, QByteArray &out)
{
    out.resize(SRP_SIZE);
    return mbedtls_mpi_write_binary(value, (unsigned char*) out.data(), SRP_SIZE) == 0;
}

QString srpCheckGenerator(const QByteArray &p, qint32 g)
{
    // g has to generate the large prime-order subgroup, or the shared secret
    // falls into a small set. For the generators Telegram uses that reduces to
    // a residue condition on p, which is one division rather than a
    // discrete-log argument at runtime.
    mbedtls_mpi prime;
    mbedtls_mpi_init(&prime);

    QString problem;

    if (mbedtls_mpi_read_binary(&prime, (const unsigned char*) p.constData(), p.size()) != 0) {
        problem = "The server's SRP prime could not be read.";
    } else {
        mbedtls_mpi_uint residue = 0;
        bool ok = false;

        switch (g) {
        case 2:
            ok = mbedtls_mpi_mod_int(&residue, &prime, 8) == 0 && residue == 7;
            break;
        case 3:
            ok = mbedtls_mpi_mod_int(&residue, &prime, 3) == 0 && residue == 2;
            break;
        case 4:
            ok = true;
            break;
        case 5:
            ok = mbedtls_mpi_mod_int(&residue, &prime, 5) == 0 && (residue == 1 || residue == 4);
            break;
        case 6:
            ok = mbedtls_mpi_mod_int(&residue, &prime, 24) == 0 && (residue == 19 || residue == 23);
            break;
        case 7:
            ok = mbedtls_mpi_mod_int(&residue, &prime, 7) == 0
                    && (residue == 3 || residue == 5 || residue == 6);
            break;
        default:
            ok = false;
            break;
        }

        if (!ok) {
            problem = QString("The server's SRP generator %1 is not usable with its prime.").arg(g);
        }
    }

    mbedtls_mpi_free(&prime);
    return problem;
}

QString srpCheckPublicValue(const char *name, const QByteArray &p, const QByteArray &value)
{
    mbedtls_mpi prime, candidate, lower, upper;
    mbedtls_mpi_init(&prime);
    mbedtls_mpi_init(&candidate);
    mbedtls_mpi_init(&lower);
    mbedtls_mpi_init(&upper);

    QString problem;

    do {
        if (mbedtls_mpi_read_binary(&prime, (const unsigned char*) p.constData(), p.size()) != 0
                || mbedtls_mpi_read_binary(&candidate, (const unsigned char*) value.constData(), value.size()) != 0) {
            problem = "The server's SRP parameters could not be read.";
            break;
        }

        if (mbedtls_mpi_lset(&lower, 1) != 0
                || mbedtls_mpi_shift_l(&lower, SAFE_BITS) != 0
                || mbedtls_mpi_sub_mpi(&upper, &prime, &lower) != 0) {
            problem = "The SRP bounds could not be computed.";
            break;
        }

        if (mbedtls_mpi_cmp_mpi(&candidate, &lower) < 0
                || mbedtls_mpi_cmp_mpi(&candidate, &upper) > 0) {
            problem = QString("%1 is outside the safe SRP range.").arg(QString::fromLatin1(name));
            break;
        }
    } while (0);

    mbedtls_mpi_free(&prime);
    mbedtls_mpi_free(&candidate);
    mbedtls_mpi_free(&lower);
    mbedtls_mpi_free(&upper);
    return problem;
}

QString srpCheckParams(const SrpParams &params)
{
    if (params.p.isEmpty() || params.salt1.isEmpty() || params.salt2.isEmpty()
            || params.srpB.isEmpty()) {
        return "The server sent incomplete password parameters.";
    }

    if (params.p.size() > SRP_SIZE || params.srpB.size() > SRP_SIZE) {
        return "The server sent an oversized SRP integer.";
    }

    mbedtls_mpi prime, known, halved;
    mbedtls_mpi_init(&prime);
    mbedtls_mpi_init(&known);
    mbedtls_mpi_init(&halved);

    QString problem;

    do {
        if (mbedtls_mpi_read_binary(&prime, (const unsigned char*) params.p.constData(), params.p.size()) != 0) {
            problem = "The server's SRP prime could not be read.";
            break;
        }

        if (mbedtls_mpi_bitlen(&prime) != 2048) {
            problem = "The server's SRP prime is not 2048 bits.";
            break;
        }

        if (mbedtls_mpi_read_string(&known, 16, KNOWN_GOOD_PRIME) != 0) {
            problem = "The built-in SRP prime could not be read.";
            break;
        }

        if (mbedtls_mpi_cmp_mpi(&prime, &known) != 0) {
            // Slow, and correct. The alternative to having this path is either
            // trusting whatever prime the server sends -- which makes the proof
            // theatre -- or refusing to log in the day Telegram rotates it.
            kgInfo() << "SRP prime is not the built-in one; verifying it";

            if (mbedtls_mpi_is_prime_ext(&prime, MILLER_RABIN_ROUNDS, &srpRandom, 0) != 0) {
                problem = "The server's SRP prime is not prime.";
                break;
            }

            if (mbedtls_mpi_sub_int(&halved, &prime, 1) != 0
                    || mbedtls_mpi_shift_r(&halved, 1) != 0) {
                problem = "The SRP prime could not be halved.";
                break;
            }

            if (mbedtls_mpi_is_prime_ext(&halved, MILLER_RABIN_ROUNDS, &srpRandom, 0) != 0) {
                problem = "The server's SRP prime is not a safe prime.";
                break;
            }
        }
    } while (0);

    mbedtls_mpi_free(&prime);
    mbedtls_mpi_free(&known);
    mbedtls_mpi_free(&halved);

    if (!problem.isEmpty()) {
        return problem;
    }

    problem = srpCheckGenerator(params.p, params.g);
    if (!problem.isEmpty()) {
        return problem;
    }

    return srpCheckPublicValue("The server's SRP B", params.p, params.srpB);
}

// Deliberately one function rather than a set of helpers over QByteArray.
// Every helper boundary would be a 256-byte copy in and out of an mbedtls_mpi
// and -- the reason that decides it -- would leave x, the verifier and the
// shared secret sitting in heap buffers that then have to be found and wiped
// one at a time. Here they live in mpis on one frame, released by one block.
QString srpComputeWithSecret(const SrpParams &params, const QByteArray &passwordUtf8,
                             const QByteArray &secret, SrpProof &out, Pbkdf2Sink *sink)
{
    QString problem = srpCheckParams(params);
    if (!problem.isEmpty()) {
        return problem;
    }

    if (secret.size() != SRP_SIZE) {
        return "The SRP secret must be 256 bytes.";
    }

    mbedtls_mpi p, g, a, bigA, bigB, x, k, v, kv, u, t, e, s, precRR;
    mbedtls_mpi_init(&p);
    mbedtls_mpi_init(&g);
    mbedtls_mpi_init(&a);
    mbedtls_mpi_init(&bigA);
    mbedtls_mpi_init(&bigB);
    mbedtls_mpi_init(&x);
    mbedtls_mpi_init(&k);
    mbedtls_mpi_init(&v);
    mbedtls_mpi_init(&kv);
    mbedtls_mpi_init(&u);
    mbedtls_mpi_init(&t);
    mbedtls_mpi_init(&e);
    mbedtls_mpi_init(&s);
    mbedtls_mpi_init(&precRR);

    QByteArray padP, padG, padA, padB, padS;
    QByteArray ph1, ph2base, xBytes;

    do {
        if (mbedtls_mpi_read_binary(&p, (const unsigned char*) params.p.constData(), params.p.size()) != 0
                || mbedtls_mpi_lset(&g, params.g) != 0
                || mbedtls_mpi_read_binary(&bigB, (const unsigned char*) params.srpB.constData(), params.srpB.size()) != 0
                || mbedtls_mpi_read_binary(&a, (const unsigned char*) secret.constData(), secret.size()) != 0) {
            problem = "The SRP parameters could not be read.";
            break;
        }

        if (mbedtls_mpi_cmp_int(&a, 0) == 0) {
            problem = UNSAFE_A;
            break;
        }

        // A = g^a mod p. precRR is a Montgomery helper keyed on p alone, so
        // filling it here spares the setup for the two exponentiations below.
        if (mbedtls_mpi_exp_mod(&bigA, &g, &a, &p, &precRR) != 0 || !padTo256(&bigA, padA)) {
            problem = "The SRP public value could not be computed.";
            break;
        }

        if (!srpCheckPublicValue("A", params.p, padA).isEmpty()) {
            problem = UNSAFE_A;
            break;
        }

        // x = SH(PBKDF2(SH(SH(password, salt1), salt2), salt1), salt2).
        // salt1 is the PBKDF2 salt and salt2 wraps the outer hashes; the shape
        // invites swapping them, and a swap still yields a plausible 32 bytes.
        ph1 = saltedHash(saltedHash(passwordUtf8, params.salt1), params.salt2);
        ph2base = pbkdf2HmacSHA512(ph1, params.salt1, PBKDF2_ITERATIONS, 64, sink);

        if (ph2base.isEmpty()) {
            // The sink withdrew, which is a decision and not a fault.
            problem = QString();
            break;
        }

        xBytes = saltedHash(ph2base, params.salt2);

        if (mbedtls_mpi_read_binary(&x, (const unsigned char*) xBytes.constData(), xBytes.size()) != 0
                || mbedtls_mpi_cmp_int(&x, 0) == 0) {
            problem = "The SRP password hash is unusable.";
            break;
        }

        if (!padTo256(&p, padP) || !padTo256(&g, padG) || !padTo256(&bigB, padB)) {
            problem = "An SRP integer could not be padded.";
            break;
        }

        // k = H(pad(p) | pad(g)), where pad(g) for g = 3 is 255 zero bytes then
        // 0x03 -- not the single byte 0x03.
        const QByteArray kBytes = hashSHA256(padP + padG);
        const QByteArray uBytes = hashSHA256(padA + padB);

        if (mbedtls_mpi_read_binary(&k, (const unsigned char*) kBytes.constData(), kBytes.size()) != 0
                || mbedtls_mpi_exp_mod(&v, &g, &x, &p, &precRR) != 0
                || mbedtls_mpi_mul_mpi(&kv, &k, &v) != 0
                || mbedtls_mpi_mod_mpi(&kv, &kv, &p) != 0) {
            problem = "The SRP verifier could not be computed.";
            break;
        }

        if (mbedtls_mpi_read_binary(&u, (const unsigned char*) uBytes.constData(), uBytes.size()) != 0) {
            problem = "The SRP scrambler could not be computed.";
            break;
        }

        if (mbedtls_mpi_cmp_int(&u, 0) == 0) {
            problem = "The server's SRP parameters give u = 0.";
            break;
        }

        // t = B - k*v, corrected explicitly rather than by mod_mpi: sub_mpi is
        // signed, and leaning on exp_mod's treatment of a negative base would
        // be leaning on behaviour its contract does not state.
        if (mbedtls_mpi_sub_mpi(&t, &bigB, &kv) != 0) {
            problem = "The SRP base could not be computed.";
            break;
        }

        if (mbedtls_mpi_cmp_int(&t, 0) < 0 && mbedtls_mpi_add_mpi(&t, &t, &p) != 0) {
            problem = "The SRP base could not be corrected.";
            break;
        }

        if (mbedtls_mpi_cmp_int(&t, 0) == 0) {
            problem = "The server's SRP parameters give an empty base.";
            break;
        }

        // The exponent a + u*x is left unreduced, as the construction defines
        // it. Reducing it mod p-1 would be equivalent and is not what the
        // proof it has to match does.
        if (mbedtls_mpi_mul_mpi(&e, &u, &x) != 0
                || mbedtls_mpi_add_mpi(&e, &e, &a) != 0
                || mbedtls_mpi_exp_mod(&s, &t, &e, &p, &precRR) != 0
                || !padTo256(&s, padS)) {
            problem = "The SRP shared secret could not be computed.";
            break;
        }

        out.srpId = params.srpId;
        out.a = padA;
        out.m1 = hashSHA256(xorArray(hashSHA256(padP), hashSHA256(padG))
                            + hashSHA256(params.salt1)
                            + hashSHA256(params.salt2)
                            + padA
                            + padB
                            + hashSHA256(padS));
        problem = QString();
    } while (0);

    mbedtls_mpi_free(&p);
    mbedtls_mpi_free(&g);
    mbedtls_mpi_free(&a);
    mbedtls_mpi_free(&bigA);
    mbedtls_mpi_free(&bigB);
    mbedtls_mpi_free(&x);
    mbedtls_mpi_free(&k);
    mbedtls_mpi_free(&v);
    mbedtls_mpi_free(&kv);
    mbedtls_mpi_free(&u);
    mbedtls_mpi_free(&t);
    mbedtls_mpi_free(&e);
    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&precRR);

    // The shared secret and the three derivation stages are the password in
    // another form.
    wipe(padS);
    wipe(ph1);
    wipe(ph2base);
    wipe(xBytes);

    return problem;
}

QString srpCompute(const SrpParams &params, const QByteArray &passwordUtf8,
                   SrpProof &out, Pbkdf2Sink *sink)
{
    QString problem;

    // A is held to the same range rule as B, so a draw landing outside it is
    // redrawn rather than reported: the parameters are fine, this particular
    // secret is not.
    for (qint32 attempt = 0; attempt < MAX_SECRET_ATTEMPTS; ++attempt) {
        QByteArray secret = randomBytes(SRP_SIZE);
        problem = srpComputeWithSecret(params, passwordUtf8, secret, out, sink);
        wipe(secret);

        if (problem != QString::fromLatin1(UNSAFE_A)) {
            return problem;
        }
    }

    return "A safe SRP secret could not be generated.";
}
