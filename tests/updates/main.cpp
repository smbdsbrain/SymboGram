// Offline checks for the update sequence rules.
//
// These decide, for every update Telegram pushes, whether it is applied,
// dropped as already seen, or treated as evidence that something in front of
// it never arrived. Getting the third case wrong is the expensive one: too
// eager and the client refetches the whole difference on every message, too
// slow and messages are lost with nothing to indicate it.
//
// None of this needs a server, so none of it is tested against one. What a
// live tier adds is whether the values Telegram actually sends match the shape
// assumed here -- not whether the arithmetic is right.

#include <QCoreApplication>
#include <QSettings>
#include <QTemporaryFile>
#include <QTextStream>

#include "tgupdatesstate.h"
#include "tlcase.h"
#include "tlschema.h"

static const char* verdictName(TgUpdatesState::Verdict v)
{
    switch (v) {
    case TgUpdatesState::Apply:     return "Apply";
    case TgUpdatesState::Duplicate: return "Duplicate";
    case TgUpdatesState::Gap:       return "Gap";
    }
    return "?";
}

static const char* seqVerdictName(TgUpdatesState::SeqVerdict v)
{
    switch (v) {
    case TgUpdatesState::SeqIgnore:    return "SeqIgnore";
    case TgUpdatesState::SeqApply:     return "SeqApply";
    case TgUpdatesState::SeqDuplicate: return "SeqDuplicate";
    case TgUpdatesState::SeqGap:       return "SeqGap";
    }
    return "?";
}

struct PtsCase {
    const char *name;
    // The two counters are set independently. A case that leaves the common
    // pts at some placeholder cannot tell "the channel used its own counter"
    // apart from "the channel fell back to the common one", because both give
    // the same verdict when the two happen to be close together.
    qint32 commonPts;
    qint64 channelId;       // 0: the update belongs to the common sequence
    qint32 channelPts;      // 0: this channel has never been polled
    qint32 pts;
    qint32 ptsCount;
    TgUpdatesState::Verdict expect;
};

struct SeqCase {
    const char *name;
    qint32 localSeq;
    qint32 seqStart;
    qint32 seq;
    TgUpdatesState::SeqVerdict expect;
};

static void runPtsCase(TlReport &r, const PtsCase &c)
{
    TgUpdatesState s;
    s.reset(c.commonPts, 0, 0, 0);
    if (c.channelId && c.channelPts) {
        s.setChannelPts(c.channelId, c.channelPts);
    }

    const TgUpdatesState::Verdict got = s.check(c.channelId, c.pts, c.ptsCount);
    if (got == c.expect) {
        r.ok(QString::fromLatin1(c.name), QString::fromLatin1(verdictName(got)));
        return;
    }
    r.fail(QString::fromLatin1(c.name),
           QString("expected %1, got %2")
               .arg(QString::fromLatin1(verdictName(c.expect)))
               .arg(QString::fromLatin1(verdictName(got))));
}

static void runSeqCase(TlReport &r, const SeqCase &c)
{
    TgUpdatesState s;
    s.reset(1, 0, 0, c.localSeq);

    const TgUpdatesState::SeqVerdict got = s.checkSeq(c.seqStart, c.seq);
    if (got == c.expect) {
        r.ok(QString::fromLatin1(c.name), QString::fromLatin1(seqVerdictName(got)));
        return;
    }
    r.fail(QString::fromLatin1(c.name),
           QString("expected %1, got %2")
               .arg(QString::fromLatin1(seqVerdictName(c.expect)))
               .arg(QString::fromLatin1(seqVerdictName(got))));
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    TlReport r;

    static const PtsCase kPts[] = {
        //                                 common  ch  chPts   pts  count
        // In sequence.
        { "pts/one message in order",         100,  0,     0,  101, 1, TgUpdatesState::Apply },
        { "pts/a batch of three in order",    100,  0,     0,  103, 3, TgUpdatesState::Apply },
        // pts_count 0 at the head of the sequence. Ordinary for read receipts
        // and edits, and the case an obvious duplicate rule gets wrong.
        { "pts/no-op update at the head",     100,  0,     0,  100, 0, TgUpdatesState::Apply },
        // Already applied.
        { "pts/behind the head",              100,  0,     0,   99, 1, TgUpdatesState::Duplicate },
        { "pts/exact replay",                 100,  0,     0,  100, 1, TgUpdatesState::Duplicate },
        // Something is missing in front of it.
        { "pts/one update missing",           100,  0,     0,  102, 1, TgUpdatesState::Gap },
        { "pts/count short of the jump",      100,  0,     0,  110, 3, TgUpdatesState::Gap },
        // Channels keep their own counter. The common pts is deliberately far
        // away in all three, so a channel silently reading it is visible.
        { "pts/unseen channel adopts",       1000,  7,     0,   50, 1, TgUpdatesState::Apply },
        { "pts/channel in order",            1000,  7,   100,  101, 1, TgUpdatesState::Apply },
        { "pts/channel gap",                 1000,  7,   100,  110, 1, TgUpdatesState::Gap },
    };

    static const SeqCase kSeq[] = {
        { "seq/unsequenced container",         5, 0, 0, TgUpdatesState::SeqIgnore },
        { "seq/next in order",                 5, 6, 6, TgUpdatesState::SeqApply },
        // `updates` has no seq_start field, so it decodes as 0 and must be
        // read as equal to seq.
        { "seq/absent seq_start means seq",    5, 0, 6, TgUpdatesState::SeqApply },
        { "seq/combined range in order",       5, 6, 8, TgUpdatesState::SeqApply },
        { "seq/already applied",               5, 4, 4, TgUpdatesState::SeqDuplicate },
        { "seq/container missing in front",    5, 8, 8, TgUpdatesState::SeqGap },
    };

    // Controls. Each asserts that a rule REJECTS something, which is the half a
    // suite loses when it only ever checks the happy path -- and a suite that
    // checks nothing prints the same green lines as one that works.
    static const PtsCase kPtsControls[] = {
        //                                 common  ch  chPts   pts  count
        // Collapsing Duplicate into Gap makes every replayed update trigger a
        // full difference fetch: right verdict list, ruinous behaviour.
        { "control/duplicate is not a gap",   100,  0,     0,   99, 1, TgUpdatesState::Duplicate },
        // An implementation that ignores channelId checks this against the
        // common pts of 1000 and calls it a duplicate, so every channel
        // message would be dropped in silence.
        { "control/a channel update is not checked against the common sequence",
                                             1000,  7,   100,  101, 1, TgUpdatesState::Apply },
        // An unseen channel must adopt rather than inherit. Falling back to a
        // common pts of 1000 turns the channel's first message into a
        // duplicate, and the channel never starts.
        { "control/an unseen channel does not inherit the common pts",
                                             1000,  7,     0,   50, 1, TgUpdatesState::Apply },
    };

    r.planned = (int) (sizeof(kPts) / sizeof(kPts[0]))
              + (int) (sizeof(kSeq) / sizeof(kSeq[0]))
              + (int) (sizeof(kPtsControls) / sizeof(kPtsControls[0]))
              + 5;

    for (unsigned i = 0; i < sizeof(kPts) / sizeof(kPts[0]); ++i) {
        runPtsCase(r, kPts[i]);
    }
    for (unsigned i = 0; i < sizeof(kSeq) / sizeof(kSeq[0]); ++i) {
        runSeqCase(r, kSeq[i]);
    }
    for (unsigned i = 0; i < sizeof(kPtsControls) / sizeof(kPtsControls[0]); ++i) {
        runPtsCase(r, kPtsControls[i]);
    }

    // advance() must not move the sequence backwards. Without this an update
    // applied out of order rewinds pts, and everything after it reads as a gap
    // -- a client that refetches the same difference forever.
    {
        TgUpdatesState s;
        s.reset(100, 0, 0, 0);
        s.advance(0, 105);
        s.advance(0, 102);
        if (s.pts() == 105) {
            r.ok("advance/never moves backwards");
        } else {
            r.fail("advance/never moves backwards",
                   QString("expected pts 105, got %1").arg(s.pts()));
        }
    }

    {
        TgUpdatesState s;
        s.reset(1, 0, 0, 0);
        s.setChannelPts(7, 50);
        s.advance(7, 60);
        s.advance(7, 55);
        if (s.channelPts(7) == 60) {
            r.ok("advance/channel never moves backwards");
        } else {
            r.fail("advance/channel never moves backwards",
                   QString("expected channel pts 60, got %1").arg(s.channelPts(7)));
        }
    }

    // Which sequence an update belongs to is read out of the object, so that a
    // layer adding another channel-scoped update does not silently route it
    // onto the common counter -- where a channel pts reads as a vast gap.
    {
        TGOBJECT(TLType::UpdateNewChannelMessage, update);
        TGOBJECT(TLType::Message, message);
        TGOBJECT(TLType::PeerChannel, peer);
        peer["channel_id"] = (qint64) 1234;
        message["peer_id"] = peer;
        update["message"] = message;

        const qint64 got = TgUpdatesState::channelIdOf(update);
        if (got == 1234) {
            r.ok("channelIdOf/reads a channel message");
        } else {
            r.fail("channelIdOf/reads a channel message",
                   QString("expected 1234, got %1").arg(got));
        }
    }

    // Control: a private-chat update must land on the common sequence. If this
    // returned a channel id, every ordinary message would be checked against a
    // counter that does not exist.
    {
        TGOBJECT(TLType::UpdateNewMessage, update);
        TGOBJECT(TLType::Message, message);
        TGOBJECT(TLType::PeerUser, peer);
        peer["user_id"] = (qint64) 99;
        message["peer_id"] = peer;
        update["message"] = message;

        const qint64 got = TgUpdatesState::channelIdOf(update);
        if (got == 0) {
            r.ok("control/channelIdOf leaves private chats on the common sequence");
        } else {
            r.fail("control/channelIdOf leaves private chats on the common sequence",
                   QString("expected 0, got %1").arg(got));
        }
    }

    // The state has to survive a restart, or every launch refetches from
    // whatever the server last reported rather than from where this client
    // actually got to.
    {
        QTemporaryFile file;
        file.open();
        const QString path = file.fileName();
        file.close();

        {
            TgUpdatesState s;
            s.reset(4321, 77, 1700000000, 12);
            s.setChannelPts(500, 90);
            s.setChannelPts(600, 91);
            QSettings settings(path, QSettings::IniFormat);
            s.save(settings);
            settings.sync();
        }

        TgUpdatesState back;
        QSettings settings(path, QSettings::IniFormat);
        back.load(settings);

        QString why;
        if (!back.valid())                     why = "did not come back valid";
        else if (back.pts() != 4321)           why = QString("pts %1").arg(back.pts());
        else if (back.qts() != 77)             why = QString("qts %1").arg(back.qts());
        else if (back.date() != 1700000000)    why = QString("date %1").arg(back.date());
        else if (back.seq() != 12)             why = QString("seq %1").arg(back.seq());
        else if (back.channelPts(500) != 90)   why = QString("channel 500 pts %1").arg(back.channelPts(500));
        else if (back.channelPts(600) != 91)   why = QString("channel 600 pts %1").arg(back.channelPts(600));

        if (why.isEmpty()) {
            r.ok("state/survives save and load");
        } else {
            r.fail("state/survives save and load", why);
        }
    }

    const int rc = r.finish();
    out.flush();
    return rc;
}
