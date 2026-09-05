#include "tgupdatesstate.h"

#include <QSettings>
#include <QStringList>

#include "tlschema.h"

TgUpdatesState::TgUpdatesState()
    : _pts(0)
    , _qts(0)
    , _date(0)
    , _seq(0)
    , _valid(false)
    , _channelPts()
{
}

void TgUpdatesState::reset(qint32 pts, qint32 qts, qint32 date, qint32 seq)
{
    _pts = pts;
    _qts = qts;
    _date = date;
    _seq = seq;
    _valid = true;
}

void TgUpdatesState::clear()
{
    _pts = _qts = _date = _seq = 0;
    _valid = false;
    _channelPts.clear();
}

bool TgUpdatesState::valid() const
{
    return _valid;
}

qint32 TgUpdatesState::pts() const
{
    return _pts;
}

qint32 TgUpdatesState::qts() const
{
    return _qts;
}

qint32 TgUpdatesState::date() const
{
    return _date;
}

qint32 TgUpdatesState::seq() const
{
    return _seq;
}

void TgUpdatesState::setPts(qint32 pts)
{
    _pts = pts;
    _valid = true;
}

void TgUpdatesState::setQts(qint32 qts)
{
    // Never backwards, for the same reason as advance(): the qts a difference
    // reports is authoritative, but an update that arrives out of order must
    // not rewind it.
    if (qts > _qts) {
        _qts = qts;
    }
}

void TgUpdatesState::setDate(qint32 date)
{
    if (date > _date) {
        _date = date;
    }
}

bool TgUpdatesState::knowsChannel(qint64 channelId) const
{
    return _channelPts.contains(channelId);
}

qint32 TgUpdatesState::channelPts(qint64 channelId) const
{
    return _channelPts.value(channelId, 0);
}

void TgUpdatesState::setChannelPts(qint64 channelId, qint32 pts)
{
    _channelPts.insert(channelId, pts);
}

void TgUpdatesState::forgetChannel(qint64 channelId)
{
    _channelPts.remove(channelId);
}

void TgUpdatesState::clearChannels()
{
    _channelPts.clear();
}

QList<qint64> TgUpdatesState::channels() const
{
    return _channelPts.keys();
}

TgUpdatesState::Verdict TgUpdatesState::check(qint64 channelId, qint32 pts, qint32 ptsCount) const
{
    const qint32 local = channelId ? _channelPts.value(channelId, 0) : _pts;

    // A channel never polled has no local pts, so there is nothing to be
    // behind: adopt what the server sends. Requesting a difference instead
    // would mean one round trip per channel the account has ever been in,
    // before a single message could be shown.
    if (local == 0) {
        return Apply;
    }

    // Tested before the duplicate rule below, and the order is not cosmetic.
    // An update with pts_count 0 does not advance pts, so local + 0 == pts is
    // a legitimate apply -- and the duplicate rule, reached first, would
    // swallow every one of them.
    if (local + ptsCount == pts) {
        return Apply;
    }

    if (pts <= local) {
        return Duplicate;
    }

    return Gap;
}

void TgUpdatesState::advance(qint64 channelId, qint32 pts)
{
    // Never backwards. An update applied out of order must not rewind the
    // sequence, or everything after it reads as a gap and the client refetches
    // the same difference forever.
    if (channelId) {
        if (pts > _channelPts.value(channelId, 0)) {
            _channelPts.insert(channelId, pts);
        }
        return;
    }

    if (pts > _pts) {
        _pts = pts;
    }
}

TgUpdatesState::SeqVerdict TgUpdatesState::checkSeq(qint32 seqStart, qint32 seq) const
{
    // seq 0 says the server is deliberately not sequencing this container.
    if (seq == 0) {
        return SeqIgnore;
    }

    // `updates` carries no seq_start at all, so the generated reader leaves
    // the key absent and it reads as 0. Substituted here rather than at each
    // call site, because getting it wrong makes every such container a gap.
    if (seqStart == 0) {
        seqStart = seq;
    }

    if (_seq + 1 == seqStart) {
        return SeqApply;
    }

    if (seqStart <= _seq) {
        return SeqDuplicate;
    }

    return SeqGap;
}

void TgUpdatesState::advanceSeq(qint32 seq, qint32 date)
{
    if (seq > _seq) {
        _seq = seq;
    }
    setDate(date);
}

qint64 TgUpdatesState::channelIdOf(const TgObject &update)
{
    // Read from the object rather than switched on the constructor id: the
    // set of channel-scoped updates grows with every layer, and a switch would
    // silently route a new one onto the common sequence -- where its pts, from
    // a different counter entirely, reads as an enormous gap.
    if (update.contains("channel_id")) {
        return update["channel_id"].toLongLong();
    }

    const TgObject message = update["message"].toMap();
    if (!message.isEmpty()) {
        const TgObject peer = message["peer_id"].toMap();
        if (ID(peer) == TLType::PeerChannel) {
            return peer["channel_id"].toLongLong();
        }
    }

    return 0;
}

bool TgUpdatesState::hasPts(const TgObject &update)
{
    return update.contains("pts");
}

void TgUpdatesState::load(QSettings &settings)
{
    settings.beginGroup("UpdateState");

    _pts = settings.value("Pts").toInt();
    _qts = settings.value("Qts").toInt();
    _date = settings.value("Date").toInt();
    _seq = settings.value("Seq").toInt();

    // pts is the one value the sequence cannot work without. Treating a stored
    // zero as valid would mean applying updates against a local pts of zero,
    // which check() reads as "adopt anything".
    _valid = _pts != 0;

    _channelPts.clear();
    settings.beginGroup("ChannelPts");
    const QStringList keys = settings.childKeys();
    for (qint32 i = 0; i < keys.size(); ++i) {
        const qint32 pts = settings.value(keys[i]).toInt();
        if (pts != 0) {
            _channelPts.insert(keys[i].toLongLong(), pts);
        }
    }
    settings.endGroup();

    settings.endGroup();
}

void TgUpdatesState::save(QSettings &settings) const
{
    settings.beginGroup("UpdateState");

    settings.setValue("Pts", _pts);
    settings.setValue("Qts", _qts);
    settings.setValue("Date", _date);
    settings.setValue("Seq", _seq);

    // Removed and rewritten rather than merged: a channel left behind keeps a
    // stale pts that would be treated as authoritative on the next run.
    settings.remove("ChannelPts");
    settings.beginGroup("ChannelPts");
    for (QHash<qint64, qint32>::const_iterator it = _channelPts.constBegin();
         it != _channelPts.constEnd(); ++it) {
        settings.setValue(QString::number(it.key()), it.value());
    }
    settings.endGroup();

    settings.endGroup();
}
