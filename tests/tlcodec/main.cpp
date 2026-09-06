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


// account.password#957b50fb, flags = has_password | hint, carrying the SHA256/
// PBKDF2 KDF. Truncated p and srp_B: this asserts the wire shape and that
// srp_B/srp_id land under the keys the SRP code reads, not the arithmetic.
#define HEX_ACCOUNT_PASSWORD \
    "fb507b950c0000004a2d913a1000112233445566778899aabbccddeeff000000" \
    "10102132435465768798a9bacbdcedfe0f0000000300000008c71caeb9c6b1c9" \
    "04000000081f248632dc6175a50000000807060504030201076d792068696e74" \
    "96b05ad437854a0000000000"

// inputCheckPasswordSRP#d27ff082 srp_id:long A:bytes M1:bytes. No flags word,
// so exact byte equality is the right assertion -- including the padding of
// the two bytes fields, which is invisible at full width.
#define HEX_INPUT_CHECK_SRP \
    "82f07fd2080706050403020104aabbccdd0000000411223344000000"

// auth.checkPassword#d18b4d16 password:InputCheckPasswordSRP.
#define HEX_AUTH_CHECK_PASSWORD \
    "164d8bd182f07fd2080706050403020104aabbccdd0000000411223344000000"


// messages.affectedMessages#84d19185 pts:int pts_count:int, and the history
// form that carries an offset as well. Both are replies to a change this
// client made, and the pts in them is the only notice of it there will be.
static const char HEX_AFFECTED_MESSAGES[] =
    "8591d184761a020002000000";

static const char HEX_AFFECTED_HISTORY[] =
    "d1695cb47a1a02000500000000000000";

// inputReplyToMessage#3bd4b7c2, with every optional field absent.
static const char HEX_INPUT_REPLY_TO[] =
    "c2b7d43b0000000092100000";

// Request shapes, asserted against bytes for the reason tlExpectBytes gives.
static const char HEX_DELETE_MESSAGES[] =
    "d2958ee50100000015c4b51c020000000b00000016000000";

static const char HEX_CHANNELS_DELETE[] =
    "4efdc18428ec5af3d2040000000000002e1600000000000015c4b51c02000000"
    "0b00000016000000";

static const char HEX_READ_HISTORY[] =
    "3a6d300e4ca5e8dd07000000000000002a0000000000000063000000";

static const char HEX_CHANNELS_READ[] =
    "374910cc28ec5af3d2040000000000002e1600000000000063000000";

static const char HEX_EDIT_MESSAGE[] =
    "6ce606b1000800004ca5e8dd07000000000000002a0000000000000092100000"
    "0665646974656400";

static const TlCase kCases[] = {
    // Fixed-shape constructors with no flags word, so exact byte equality is
    // the right assertion; anything weaker would be hiding something.
    { "Bool/boolTrue",           &readTLBool,          &writeTLBool,          0, 0, HEX_BOOL_TRUE,       TlExact },
    { "Peer/peerUser",           &readTLPeer,          &writeTLPeer,          0, 0, HEX_PEER_USER,       TlExact },
    { "InputPeer/inputPeerUser", &readTLInputPeer,     &writeTLInputPeer,     0, 0, HEX_INPUT_PEER_USER, TlExact },
    { "MessageEntity/bold",      &readTLMessageEntity, &writeTLMessageEntity, 0, 0, HEX_ENTITY_BOLD,     TlExact },
    { "Vector<MessageEntity>",   &readVector,          &writeVector,
      (void*) &readTLMessageEntity, (void*) &writeTLMessageEntity, HEX_ENTITY_VECTOR, TlExact },
    { "InputCheckPasswordSRP",   &readTLInputCheckPasswordSRP, &writeTLInputCheckPasswordSRP,
      0, 0, HEX_INPUT_CHECK_SRP, TlExact },

    // Structural: account.password carries a flags word, and the writer
    // recomputes it from the keys present.
    { "account.password",        &readTLAccountPassword, &writeTLAccountPassword,
      0, 0, HEX_ACCOUNT_PASSWORD, TlStructural },
    { "messages.affectedMessages", &readTLMessagesAffectedMessages,
      &writeTLMessagesAffectedMessages, 0, 0, HEX_AFFECTED_MESSAGES, TlExact },
    { "messages.affectedHistory",  &readTLMessagesAffectedHistory,
      &writeTLMessagesAffectedHistory, 0, 0, HEX_AFFECTED_HISTORY, TlExact },
    { "InputReplyTo/toMessage",    &readTLInputReplyTo, &writeTLInputReplyTo,
      0, 0, HEX_INPUT_REPLY_TO, TlStructural },
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

    // Request shapes, which TlCase cannot express -- see tlExpectBytes.
    {
        TgObject srp;
        srp["_"] = TLType::InputCheckPasswordSRP;
        srp["srp_id"] = Q_INT64_C(0x0102030405060708);
        srp["A"] = QByteArray::fromHex("aabbccdd");
        srp["M1"] = QByteArray::fromHex("11223344");

        TgObject method;
        method["_"] = TLType::AuthCheckPasswordMethod;
        method["password"] = srp;

        tlExpectBytes(r, "auth.checkPassword/request",
                      &writeTLMethodAuthCheckPassword, 0, method,
                      HEX_AUTH_CHECK_PASSWORD);

        // Deliberate-failure control.
        //
        // The generated writer reads obj["password"] and switches on the
        // inner object's "_" with no default case. Under any other key it
        // finds an empty map, matches nothing, and writes NOTHING after the
        // method id -- a four-byte request that is not rejected as malformed
        // so much as read as a different message. That silent truncation is
        // the failure this whole assertion exists to catch, so it is worth
        // proving the assertion can see it.
        TgObject misnamed;
        misnamed["_"] = TLType::AuthCheckPasswordMethod;
        misnamed["pwd"] = srp;

        TgPacket packet;
        writeTLMethodAuthCheckPassword(packet, misnamed, 0);
        const QByteArray truncated = packet.toByteArray();

        if (truncated.size() == 4) {
            r.ok("control/a misnamed field truncates the request");
        } else {
            r.fail("control/a misnamed field truncates the request",
                   QString("expected 4 bytes from an unmatched inner object, got %1")
                       .arg(truncated.size()));
        }
    }

    // The message operations, whose requests carry the same hazards: a peer
    // written in the wrong place, a flag set for a field that is then not
    // written, or an id vector that addresses the wrong space.
    {
        TgObject inputPeer;
        inputPeer["_"] = TLType::InputPeerUser;
        inputPeer["user_id"] = Q_INT64_C(7);
        inputPeer["access_hash"] = Q_INT64_C(42);

        TgObject channel;
        channel["_"] = TLType::InputChannel;
        channel["channel_id"] = Q_INT64_C(1234);
        channel["access_hash"] = Q_INT64_C(5678);

        TgList ids;
        ids.append(11);
        ids.append(22);

        {
            TgObject method;
            method["_"] = TLType::MessagesDeleteMessagesMethod;
            method["revoke"] = true;
            method["id"] = ids;

            tlExpectBytes(r, "messages.deleteMessages/request",
                          &writeTLMethodMessagesDeleteMessages, 0, method,
                          HEX_DELETE_MESSAGES);
        }

        {
            TgObject method;
            method["_"] = TLType::ChannelsDeleteMessagesMethod;
            method["channel"] = channel;
            method["id"] = ids;

            tlExpectBytes(r, "channels.deleteMessages/request",
                          &writeTLMethodChannelsDeleteMessages, 0, method,
                          HEX_CHANNELS_DELETE);
        }

        {
            TgObject method;
            method["_"] = TLType::MessagesReadHistoryMethod;
            method["peer"] = inputPeer;
            method["max_id"] = 99;

            tlExpectBytes(r, "messages.readHistory/request",
                          &writeTLMethodMessagesReadHistory, 0, method,
                          HEX_READ_HISTORY);
        }

        {
            TgObject method;
            method["_"] = TLType::ChannelsReadHistoryMethod;
            method["channel"] = channel;
            method["max_id"] = 99;

            tlExpectBytes(r, "channels.readHistory/request",
                          &writeTLMethodChannelsReadHistory, 0, method,
                          HEX_CHANNELS_READ);
        }

        {
            TgObject method;
            method["_"] = TLType::MessagesEditMessageMethod;
            method["peer"] = inputPeer;
            method["id"] = 4242;
            method["message"] = QString("edited");

            tlExpectBytes(r, "messages.editMessage/request",
                          &writeTLMethodMessagesEditMessage, 0, method,
                          HEX_EDIT_MESSAGE);
        }

        // Deliberate-failure control.
        //
        // Generated writers recompute the flags word from which keys are
        // present, and an empty map counts as present. Setting reply_to to one
        // sets the bit and then writes nothing for it, because the inner
        // switch matches no constructor -- a request that is shorter than it
        // claims and is read as something else from that point on. This is
        // why messagesSendMessage inserts the key only when there is an id.
        {
            TgObject method;
            method["_"] = TLType::MessagesSendMessageMethod;
            method["peer"] = inputPeer;
            method["message"] = QString("hi");
            method["random_id"] = Q_INT64_C(1);
            method["reply_to"] = TgObject();

            TgPacket packet;
            writeTLMethodMessagesSendMessage(packet, method, 0);
            const QByteArray truncated = packet.toByteArray();

            TgObject without = method;
            without.remove("reply_to");

            TgPacket packet2;
            writeTLMethodMessagesSendMessage(packet2, without, 0);
            const QByteArray plain = packet2.toByteArray();

            // Same length, different flags: the bit is set and nothing follows
            // it, so every field after the flags word has moved.
            if (truncated.size() == plain.size() && truncated != plain) {
                r.ok("control/an empty reply_to sets a flag it does not write");
            } else {
                r.fail("control/an empty reply_to sets a flag it does not write",
                       QString("expected the same length with different flags, got %1 and %2")
                           .arg(truncated.size()).arg(plain.size()));
            }
        }
    }

    return r.finish();
}
