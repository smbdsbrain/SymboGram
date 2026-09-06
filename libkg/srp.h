#ifndef SRP_H
#define SRP_H

#include <QByteArray>
#include <QString>

#include "crypto.h"

// Telegram's SRP-6a password proof, the input to auth.checkPassword.
//
// The password never leaves this translation unit: a caller hands it in and
// receives only the public value A and the proof M1.
//
// The construction is written out in docs/auth.md, and pinned byte for byte by
// the known-answer vector in tests/crypto/main.cpp. Neither is decoration --
// the whole correctness argument is the ORDER of a sequence of hashes, and
// order is exactly what reading the code back cannot confirm.

// Telegram's SRP integers are 2048-bit, and every one of them is hashed and
// sent zero-left-padded to this width. A shorter encoding is a different
// message and a different proof.
const qint32 SRP_SIZE = 256;

struct SrpParams
{
    QByteArray p;
    qint32     g;
    QByteArray salt1;
    QByteArray salt2;
    QByteArray srpB;
    qint64     srpId;

    SrpParams();
};

struct SrpProof
{
    qint64     srpId;
    QByteArray a;   // capital A on the wire, always SRP_SIZE bytes
    QByteArray m1;  // 32 bytes

    SrpProof();
};

// Every entry point returns an empty string on success and a reason on
// failure. A reason and not a bool, because the failure has to reach the user
// as something better than "login failed", and this codebase does not build
// with exceptions.
QString srpCheckParams(const SrpParams &params);
QString srpCompute(const SrpParams &params, const QByteArray &passwordUtf8,
                   SrpProof &out, Pbkdf2Sink *sink = 0);
// Deterministic seam for the known-answer vector. `secret` is the big-endian
// SRP private value a, which in production must be unpredictable -- which is
// the only thing srpCompute() adds over this.
QString srpComputeWithSecret(const SrpParams &params, const QByteArray &passwordUtf8,
                             const QByteArray &secret, SrpProof &out,
                             Pbkdf2Sink *sink = 0);

// Free functions over the same conditions MTProto imposes on dh_prime, g and
// g_a, so that closing the parameter checks in tgtransport.cpp later is a call
// site rather than a move.
QString srpCheckGenerator(const QByteArray &p, qint32 g);
QString srpCheckPublicValue(const char *name, const QByteArray &p, const QByteArray &value);

#endif // SRP_H
