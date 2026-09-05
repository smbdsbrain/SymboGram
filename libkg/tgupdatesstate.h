#ifndef TGUPDATESSTATE_H
#define TGUPDATESSTATE_H

#include <QHash>
#include <QList>
#include "tgstream.h"

class QSettings;

// The pts/qts/seq bookkeeping, with no I/O, no QObject and no TgClient.
//
// Split out from TgUpdatesManager so the rules can be exercised offline. A
// target that links this file needs tgstream.cpp and the generated schema and
// nothing else; pulling in TgClient would pull in tgtransport.cpp, which
// includes apisecrets.h and refuses to compile without credentials. A gap rule
// that can only be checked against a live data centre is a gap rule nobody
// checks.
class TgUpdatesState
{
public:
    // What to do with an update carrying a pts.
    enum Verdict {
        Apply,      // in sequence; hand it on and advance
        Duplicate,  // already seen; drop it
        Gap         // something in front of it is missing; fetch the difference
    };

    // The same question for a container carrying a seq.
    enum SeqVerdict {
        SeqIgnore,    // seq 0: the server is not sequencing this one
        SeqApply,
        SeqDuplicate,
        SeqGap
    };

    TgUpdatesState();

    void reset(qint32 pts, qint32 qts, qint32 date, qint32 seq);
    void clear();

    // False until updates.getState answers or a stored state is loaded.
    // Nothing may be applied before then: with no local pts to compare
    // against, every verdict below is meaningless.
    bool valid() const;

    qint32 pts() const;
    qint32 qts() const;
    qint32 date() const;
    qint32 seq() const;

    void setPts(qint32 pts);
    void setQts(qint32 qts);
    void setDate(qint32 date);

    bool knowsChannel(qint64 channelId) const;
    qint32 channelPts(qint64 channelId) const;
    void setChannelPts(qint64 channelId, qint32 pts);
    void forgetChannel(qint64 channelId);
    void clearChannels();
    QList<qint64> channels() const;

    // channelId 0 means the common sequence.
    Verdict check(qint64 channelId, qint32 pts, qint32 ptsCount) const;
    void advance(qint64 channelId, qint32 pts);

    SeqVerdict checkSeq(qint32 seqStart, qint32 seq) const;
    void advanceSeq(qint32 seq, qint32 date);

    // Which sequence an update belongs to, and whether it carries a pts at
    // all. Both read the object rather than switching on the constructor id,
    // so a layer that adds another channel-scoped update needs no change here.
    static qint64 channelIdOf(const TgObject &update);
    static bool hasPts(const TgObject &update);

    void load(QSettings &settings);
    void save(QSettings &settings) const;

private:
    qint32 _pts;
    qint32 _qts;
    qint32 _date;
    qint32 _seq;
    bool _valid;
    QHash<qint64, qint32> _channelPts;
};

#endif // TGUPDATESSTATE_H
