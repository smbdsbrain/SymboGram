// Offline TL codec tests.
//
// The layer bump's failure mode is silent. Generated readers are a switch on
// the constructor id with no default: case, TL has no length prefixes, and
// nothing validates. Feed a 229 reader 166-shaped bytes and it does not throw,
// does not return an error and does not stop -- it consumes the wrong number of
// bytes, and every field after that point in the packet is garbage. This suite
// exists to turn that into a named failing test.
//
// Vectors here are hand-written hex, read off the schema, not captured. That is
// deliberate: bytes we produced from the .tl by hand are independent of our own
// writer, so a round-trip through them tests something. Captured vectors are
// worth adding on top -- they cover what a server actually emits, which
// hand-writing cannot -- but only from a Telegram TEST DC account. A production
// messages.dialogs is somebody's chat list and does not belong in a public
// repository.

#include <QCoreApplication>

#include "tgstream.h"
#include "tlschema.h"
#include "tlcase.h"

// All little-endian, as TL is throughout.

// boolTrue#997275b5 = Bool;
#define HEX_BOOL_TRUE "b5757299"

// peerUser#59511722 user_id:long = Peer;      user_id = 12345
#define HEX_PEER_USER "221751593930000000000000"

// inputPeerUser#dde8a54c user_id:long access_hash:long = InputPeer;
#define HEX_INPUT_PEER_USER "4ca5e8dd07000000000000002a00000000000000"

// messageEntityBold#bd610bc9 offset:int length:int = MessageEntity;
#define HEX_ENTITY_BOLD "c90b61bd0500000003000000"

// vector#1cb5c415, two elements: messageEntityBold(5,3), messageEntityCode(9,4)
#define HEX_ENTITY_VECTOR \
    "15c4b51c" "02000000" \
    "c90b61bd" "05000000" "03000000" \
    "7105a228" "09000000" "04000000"

// The invariant, made concrete. Same vector, but the first element carries a
// constructor id no layer defines. readTLMessageEntity's switch matches
// nothing, consumes its 4 bytes, returns an empty object -- and the reader then
// takes the bold entity's `offset` field as the next element's constructor id.
// Everything after is garbage, and only the top-level atEnd() check notices.
#define HEX_ENTITY_VECTOR_UNKNOWN \
    "15c4b51c" "02000000" \
    "efbeadde" "05000000" "03000000" \
    "7105a228" "09000000" "04000000"

static const TlCase kCases[] = {
    // Fixed-shape constructors with no flags word, so exact byte equality is
    // the right assertion; anything weaker would be hiding something.
    { "Bool/boolTrue",           &readTLBool,          &writeTLBool,          0, 0, HEX_BOOL_TRUE,       TlExact },
    { "Peer/peerUser",           &readTLPeer,          &writeTLPeer,          0, 0, HEX_PEER_USER,       TlExact },
    { "InputPeer/inputPeerUser", &readTLInputPeer,     &writeTLInputPeer,     0, 0, HEX_INPUT_PEER_USER, TlExact },
    { "MessageEntity/bold",      &readTLMessageEntity, &writeTLMessageEntity, 0, 0, HEX_ENTITY_BOLD,     TlExact },
    { "Vector<MessageEntity>",   &readVector,          &writeVector,
      (void*) &readTLMessageEntity, (void*) &writeTLMessageEntity, HEX_ENTITY_VECTOR, TlExact },
};

// Cases whose expected outcome is a FAILURE. A suite that asserts nothing
// prints green lines too; these are what tell the two apart, so add one
// alongside every new assertion.
struct TlControl {
    const char  *name;
    READ_METHOD  read;
    void        *cb;
    const char  *hex;
    const char  *expect;   // substring the failure reason must contain
};

static const TlControl kControls[] = {
    // Truncated: the reader runs off the end. QDataStream records ReadPastEnd
    // rather than telling anyone, which is why we have to ask.
    { "control/truncated -> ReadPastEnd",
      &readTLPeer, 0, "2217515939300000", "ReadPastEnd" },

    // Well-formed object with junk appended: proves the atEnd() check fires.
    { "control/trailing bytes -> !atEnd",
      &readTLPeer, 0, HEX_PEER_USER "deadbeef", "trailing byte" },

    // Top-level constructor id this layer does not define, and nothing after
    // it. Deliberately just the four id bytes: with a payload following, the
    // atEnd() check fires first and reports trailing bytes, which is correct
    // but tests a different branch than the one meant here.
    { "control/unknown top-level id -> empty",
      &readTLPeer, 0, "efbeadde", "not in this layer" },

    // The real thing: an unknown id NESTED inside a vector. This is the failure
    // the whole suite is built around, and it is invisible without atEnd().
    { "control/unknown nested id -> desync",
      &readVector, (void*) &readTLMessageEntity, HEX_ENTITY_VECTOR_UNKNOWN, "trailing byte" },
};

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    TlReport r;

    for (int i = 0; i < int(sizeof(kCases) / sizeof(kCases[0])); ++i)
        tlRunCase(r, kCases[i]);

    for (int i = 0; i < int(sizeof(kControls) / sizeof(kControls[0])); ++i) {
        const TlControl &c = kControls[i];
        QVariant obj;
        QString why;
        const bool decoded = tlDecodeStrict(QByteArray::fromHex(QByteArray(c.hex)),
                                            c.read, c.cb, obj, why);
        if (decoded)
            r.fail(c.name, "decoded cleanly -- the check under test is not working");
        else if (!why.contains(QString::fromLatin1(c.expect)))
            r.fail(c.name, QString("failed for the wrong reason: %1").arg(why));
        else
            r.ok(c.name, why);
    }

    return r.finish();
}
