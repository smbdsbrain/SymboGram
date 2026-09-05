#ifndef TLCASE_H
#define TLCASE_H

// Assertion and reporting for the TL codec tests.
//
// Not QtTest, although Qt 4.8.7 ships it and this target never builds for
// Symbian. During a layer bump the useful output is not pass/fail, it is
// "case messages.dialogsSlice: 44 trailing bytes; first divergence at 0x1a4".
// QCOMPARE on a QByteArray truncates exactly the thing you need to see, and a
// _data() slot would wrap a table we already have in moc boilerplate for no
// gain. TAP on stdout instead, so one parser serves this and the e2e harness.

#include <QByteArray>
#include <QString>
#include <QVariant>

#include "tgstream.h"

// What a re-encode is allowed to prove.
//
// Exact byte equality is the strong claim and it does NOT hold everywhere:
// writers recompute the flags word from which keys are present, so a bit the
// server set for a field this layer does not know about is dropped on the way
// back out. Structural is therefore the default -- decode(encode(x)) equals
// decode(x) with flags stripped -- and Exact is opt-in per case.
enum TlMode {
    TlStructural = 0,
    TlExact      = 1
};

struct TlCase {
    const char  *name;
    READ_METHOD  read;
    WRITE_METHOD write;
    // Vector<T> passes its element method through the callback argument, and
    // read and write need DIFFERENT ones -- readVector wants a READ_METHOD,
    // writeVector a WRITE_METHOD. One shared field would compile fine and
    // serialise nonsense. Both are 0 for ordinary constructors.
    void        *rcb;
    void        *wcb;
    const char  *hex;
    int          mode;
};

class TlReport
{
public:
    TlReport();

    void ok(const QString &name, const QString &note = QString());
    void fail(const QString &name, const QString &why);

    // Emits the TAP plan line and returns the process exit code.
    int finish();

    int planned;

private:
    int _n;
    int _failed;
    QString _body;
};

// Recursively drops "flags"/"flags2" from maps and lists.
//
// Mandatory, not cosmetic: the writer assigns its recomputed flags into the
// object it serialises, so decoding the re-encoded bytes yields a different
// flags value than the first decode even when every real field matches. Without
// this every flags-bearing case fails for a reason that is not the bug you are
// looking for.
QVariant tlStripFlags(const QVariant &v);

// Decodes, checking that the reader consumed exactly the buffer.
//
// This is the whole point of the suite. Generated readers are a switch on the
// constructor id with no default: case, and TL carries no length prefixes, so
// an unrecognised id consumes its 4 bytes, returns an empty object and leaves
// the stream parked mid-object. Nothing throws and nothing returns an error --
// the remaining fields are simply garbage. Trailing bytes at the top level is
// the only signal that this happened somewhere inside.
bool tlDecodeStrict(const QByteArray &in, READ_METHOD read, void *cb,
                    QVariant &out, QString &why);

// "offset 0x1a4: expected 37 30 66 c4, got 00 00 00 00"
QString tlFirstDifference(const QByteArray &a, const QByteArray &b);

void tlRunCase(TlReport &r, const TlCase &c);

#endif // TLCASE_H
