#include "tgupdates.h"

#include <QCoreApplication>
#include <QSettings>
#include <QTimerEvent>

#include "debug.h"
#include "tgclient.h"
#include "tlschema.h"

using namespace TLType;

TgPendingUpdate::TgPendingUpdate()
    : update()
    , users()
    , chats()
    , messageId(0)
    , channelId(0)
    , pts(0)
    , ptsCount(0)
    , date(0)
    , shortForm(false)
    , parkedAt(0)
{
}

TgUpdatesManager::TgUpdatesManager(TgClient *client)
    : QObject(client)
    , _client(client)
    , _state()
    , _stateRequestId(0)
    , _differenceRequestId(0)
    , _channelRequestId()
    , _channelQueue()
    , _channelAccessHash()
    , _pending()
    , _gapTimer()
    , _tick(0)
    , _lastSave()
{
    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QCoreApplication::organizationName(),
                       QCoreApplication::applicationName() + "_" + _client->sessionName());
    _state.load(settings);

    _lastSave.start();

    kgInfo() << "Update state loaded, pts" << _state.pts()
             << "qts" << _state.qts()
             << "channels" << _state.channels().size();
}

const TgUpdatesState& TgUpdatesManager::state() const
{
    return _state;
}

void TgUpdatesManager::save()
{
    if (!_state.valid()) {
        return;
    }

    QSettings settings(QSettings::IniFormat, QSettings::UserScope,
                       QCoreApplication::organizationName(),
                       QCoreApplication::applicationName() + "_" + _client->sessionName());
    _state.save(settings);

    _lastSave.restart();
}

bool TgUpdatesManager::staleReply(const char *what, qint64 messageId)
{
    // The session id outlives the process -- it is stored beside the auth key
    // -- and MTProto re-delivers any reply the client never acknowledged. So a
    // reply to a request made by a PREVIOUS run of the app arrives moments
    // after the next connect, carrying a state from before whatever has
    // happened since.
    //
    // Claimed rather than passed on as an unknown response, because nothing
    // else in the client asks for these constructors and an unknown response
    // reads as a decoding fault. Ignored rather than applied, because its
    // state would rewind pts to where the sequence was then and everything
    // after it would be refetched or, worse, replayed.
    kgInfo() << "Ignoring a" << what << "reply to a request from an earlier run, msg id" << messageId;
    return true;
}

void TgUpdatesManager::publishState()
{
    _client->dispatchUpdatesState(_state.pts(), _state.qts(), _state.date(), _state.seq());
}

void TgUpdatesManager::maybeSave()
{
    if (_lastSave.elapsed() >= SaveIntervalMs) {
        save();
    }
}

// --- lifecycle --------------------------------------------------------------

void TgUpdatesManager::authorized()
{
    if (!_state.valid()) {
        requestState();
        return;
    }

    // A stored state is a claim about what this client has already seen, so
    // the right move is to ask for what has happened since -- not to ask the
    // server where it is now, which would silently skip everything in between.
    requestDifference();
}

void TgUpdatesManager::initialized()
{
    if (!_client->isAuthorized()) {
        return;
    }

    // Every reconnect is a hole of unknown size: nothing was delivered while
    // the socket was down, and the server does not replay it.
    if (_state.valid()) {
        requestDifference();
    } else {
        requestState();
    }
}

void TgUpdatesManager::disconnected()
{
    // In-flight requests will never be answered; their ids must not stay
    // reserved or the next attempt is discarded as an unexpected reply.
    _stateRequestId = 0;
    _differenceRequestId = 0;
    _channelRequestId.clear();

    save();
}

void TgUpdatesManager::messageChanged(qint64 oldMsg, qint64 newMsg)
{
    if (_stateRequestId == oldMsg) {
        _stateRequestId = newMsg;
    }
    if (_differenceRequestId == oldMsg) {
        _differenceRequestId = newMsg;
    }
    if (_channelRequestId.contains(oldMsg)) {
        _channelRequestId.insert(newMsg, _channelRequestId.take(oldMsg));
    }
}

// --- requests ---------------------------------------------------------------

void TgUpdatesManager::requestState()
{
    if (_stateRequestId != 0) {
        return;
    }

    kgInfo() << "Requesting updates.getState";
    _stateRequestId = _client->updatesGetState().toLongLong();
}

void TgUpdatesManager::requestDifference()
{
    if (!_state.valid()) {
        requestState();
        return;
    }

    if (_differenceRequestId != 0) {
        return;
    }

    kgInfo() << "Requesting updates.getDifference from pts" << _state.pts();
    _differenceRequestId = _client->updatesGetDifference(
                _state.pts(), _state.date(), _state.qts()).toLongLong();
}

void TgUpdatesManager::enqueueChannel(qint64 channelId)
{
    if (channelId == 0) {
        return;
    }

    // Already being fetched, or already waiting.
    if (_channelRequestId.values().contains(channelId)
            || _channelQueue.contains(channelId)) {
        return;
    }

    _channelQueue.append(channelId);
    pumpChannelQueue();
}

void TgUpdatesManager::pumpChannelQueue()
{
    while (!_channelQueue.isEmpty() && _channelRequestId.size() < MaxChannelPolls) {
        requestChannelDifference(_channelQueue.takeFirst());
    }
}

void TgUpdatesManager::requestChannelDifference(qint64 channelId)
{
    if (!_channelAccessHash.contains(channelId)) {
        // Nothing can be asked about a channel whose access hash is unknown.
        // The next full difference carries the chat, and the poll can be
        // retried then; guessing a hash produces CHANNEL_INVALID.
        kgWarning() << "No access hash for channel" << channelId << ", skipping its difference";
        return;
    }

    TGOBJECT(InputChannel, inputChannel);
    inputChannel["channel_id"] = channelId;
    inputChannel["access_hash"] = _channelAccessHash.value(channelId);

    const qint64 messageId = _client->updatesGetChannelDifference(
                inputChannel, _state.channelPts(channelId), DifferenceLimit).toLongLong();

    if (messageId != 0) {
        _channelRequestId.insert(messageId, channelId);
    }
}

void TgUpdatesManager::rememberChats(const TgList &chats)
{
    for (qint32 i = 0; i < chats.size(); ++i) {
        const TgObject chat = chats[i].toMap();
        if (ID(chat) != Channel && ID(chat) != ChannelForbidden) {
            continue;
        }
        if (!chat.contains("access_hash")) {
            continue;
        }
        _channelAccessHash.insert(chat["id"].toLongLong(), chat["access_hash"].toLongLong());
    }
}

// --- ingress ----------------------------------------------------------------

void TgUpdatesManager::processUpdates(TgObject updates, qint64 messageId)
{
    const TgList users = updates["users"].toList();
    const TgList chats = updates["chats"].toList();
    rememberChats(chats);

    switch (ID(updates)) {
    case UpdatesTooLong:
        // The server is saying the backlog is too large to push. Not an
        // error, and not something to route to unknownResponse: it is the
        // documented way of being told to fetch the difference.
        kgInfo() << "updatesTooLong, fetching the difference";
        requestDifference();
        return;

    case UpdateShort:
    {
        const qint32 date = updates["date"].toInt();
        _state.setDate(date);
        applyOne(updates["update"].toMap(), TgList(), TgList(), date, messageId, false);
        return;
    }

    case Updates:
    case UpdatesCombined:
    {
        const qint32 date = updates["date"].toInt();
        const qint32 seq = updates["seq"].toInt();
        const qint32 seqStart = updates["seq_start"].toInt();

        switch (_state.checkSeq(seqStart, seq)) {
        case TgUpdatesState::SeqDuplicate:
            kgDebug() << "Dropping an already applied updates container, seq" << seq;
            return;
        case TgUpdatesState::SeqGap:
            kgInfo() << "Container sequence gap at seq" << seq << ", fetching the difference";
            requestDifference();
            return;
        case TgUpdatesState::SeqApply:
            _state.advanceSeq(seq, date);
            break;
        case TgUpdatesState::SeqIgnore:
            _state.setDate(date);
            break;
        }

        const TgList list = updates["updates"].toList();
        for (qint32 i = 0; i < list.size(); ++i) {
            applyOne(list[i].toMap(), users, chats, date, messageId, false);
        }
        return;
    }

    case UpdateShortMessage:
    case UpdateShortChatMessage:
    case UpdateShortSentMessage:
        // These are containers that are their own update, and they carry a
        // pts of their own. Checked like any other rather than emitted
        // unconditionally, which is how a resent message used to appear twice.
        applyOne(updates, TgList(), TgList(), updates["date"].toInt(), messageId, true);
        return;

    default:
        kgWarning() << "Unhandled updates constructor" << ID(updates);
        return;
    }
}

void TgUpdatesManager::applyOne(const TgObject &update, const TgList &users,
                                const TgList &chats, qint32 date, qint64 messageId,
                                bool shortForm)
{
    const qint64 channelId = TgUpdatesState::channelIdOf(update);

    // The server saying a channel is too far behind to push. Its own pts is
    // not usable from here, so ask for that channel's difference.
    if (ID(update) == UpdateChannelTooLong) {
        kgInfo() << "updateChannelTooLong for" << channelId;
        dropPending(channelId);
        enqueueChannel(channelId);
        return;
    }

    // Typing notifications, user status, and everything else outside the
    // pts sequence. There is nothing to order them against.
    if (!TgUpdatesState::hasPts(update)) {
        emitOne(update, users, chats, date, messageId, shortForm);
        return;
    }

    const qint32 pts = update["pts"].toInt();
    const qint32 ptsCount = update["pts_count"].toInt();

    if (!_state.valid()) {
        // No local pts to judge against. Holding the update and asking where
        // the sequence is loses nothing; applying it blind would set a pts
        // this client never actually reached.
        park(update, users, chats, channelId, pts, ptsCount, date, messageId, shortForm);
        requestState();
        return;
    }

    switch (_state.check(channelId, pts, ptsCount)) {
    case TgUpdatesState::Apply:
        _state.advance(channelId, pts);
        emitOne(update, users, chats, date, messageId, shortForm);
        if (channelId == 0) {
            publishState();
        }
        maybeSave();
        drainPending();
        return;

    case TgUpdatesState::Duplicate:
        kgDebug() << "Dropping a duplicate update, pts" << pts;
        return;

    case TgUpdatesState::Gap:
        kgInfo() << "Gap before pts" << pts << "on channel" << channelId;
        park(update, users, chats, channelId, pts, ptsCount, date, messageId, shortForm);
        if (channelId) {
            enqueueChannel(channelId);
        } else {
            requestDifference();
        }
        return;
    }
}

void TgUpdatesManager::emitOne(const TgObject &update, const TgList &users,
                               const TgList &chats, qint32 date, qint64 messageId,
                               bool shortForm)
{
    if (shortForm) {
        _client->dispatchMessageUpdate(update, messageId);
        return;
    }

    _client->dispatchUpdate(update, users, chats, date);
}

// --- the pending queue ------------------------------------------------------

void TgUpdatesManager::park(const TgObject &update, const TgList &users,
                            const TgList &chats, qint64 channelId, qint32 pts,
                            qint32 ptsCount, qint32 date, qint64 messageId,
                            bool shortForm)
{
    if (_pending.size() >= MaxPending) {
        // Dropped whole rather than trimmed: what is being held is a
        // contiguous run waiting on one missing update, and half a run is
        // worth nothing. The difference below refetches all of it.
        kgWarning() << "Pending update queue full, dropping" << _pending.size() << "and refetching";
        _pending.clear();
        requestDifference();
        return;
    }

    TgPendingUpdate item;
    item.update = update;
    item.users = users;
    item.chats = chats;
    item.messageId = messageId;
    item.channelId = channelId;
    item.pts = pts;
    item.ptsCount = ptsCount;
    item.date = date;
    item.shortForm = shortForm;
    item.parkedAt = _tick;

    _pending.append(item);

    if (!_gapTimer.isActive()) {
        _gapTimer.start(GapTimeoutMs, this);
    }
}

void TgUpdatesManager::drainPending()
{
    if (_pending.isEmpty()) {
        return;
    }

    // Repeated until a full pass applies nothing: applying one update can make
    // the next one in the queue contiguous, and the queue is not in order.
    bool progress = true;
    while (progress) {
        progress = false;
        for (qint32 i = 0; i < _pending.size(); ++i) {
            const TgPendingUpdate &item = _pending[i];
            const TgUpdatesState::Verdict verdict =
                    _state.check(item.channelId, item.pts, item.ptsCount);

            if (verdict == TgUpdatesState::Gap) {
                continue;
            }

            const TgPendingUpdate taken = _pending.takeAt(i);
            --i;
            progress = true;

            if (verdict == TgUpdatesState::Duplicate) {
                continue;
            }

            _state.advance(taken.channelId, taken.pts);
            emitOne(taken.update, taken.users, taken.chats, taken.date,
                    taken.messageId, taken.shortForm);
        }
    }

    if (_pending.isEmpty()) {
        _gapTimer.stop();
    }
}

void TgUpdatesManager::expirePending()
{
    bool expired = false;

    for (qint32 i = 0; i < _pending.size(); ++i) {
        if (_tick - _pending[i].parkedAt < 1) {
            continue;
        }
        _pending.removeAt(i);
        --i;
        expired = true;
    }

    if (!expired) {
        return;
    }

    // A gap that has not closed in a full tick is not going to close by
    // waiting longer. Whatever was dropped comes back in the difference.
    kgWarning() << "Gap did not close, refetching the difference";
    requestDifference();

    if (_pending.isEmpty()) {
        _gapTimer.stop();
    }
}

void TgUpdatesManager::dropPending(qint64 channelId)
{
    for (qint32 i = 0; i < _pending.size(); ++i) {
        if (_pending[i].channelId != channelId) {
            continue;
        }
        _pending.removeAt(i);
        --i;
    }

    if (_pending.isEmpty()) {
        _gapTimer.stop();
    }
}

void TgUpdatesManager::clearPending()
{
    _pending.clear();
    _gapTimer.stop();
}

void TgUpdatesManager::timerEvent(QTimerEvent *event)
{
    if (event->timerId() != _gapTimer.timerId()) {
        return;
    }

    ++_tick;
    expirePending();
}

// --- replies ----------------------------------------------------------------

bool TgUpdatesManager::handleState(TgObject updatesState, qint64 messageId)
{
    if (_stateRequestId == 0 || _stateRequestId != messageId) {
        return staleReply("updates.state", messageId);
    }
    _stateRequestId = 0;

    if (EMPTY(updatesState)) {
        // The reader matched no constructor, which means the stream was
        // already out of step. Adopting the empty values would set pts to
        // zero and make every later update look like the first one.
        kgCritical() << "updates.getState decoded to nothing";
        return true;
    }

    _state.reset(updatesState["pts"].toInt(),
                 updatesState["qts"].toInt(),
                 updatesState["date"].toInt(),
                 updatesState["seq"].toInt());

    kgInfo() << "Update state seeded, pts" << _state.pts();

    save();
    publishState();
    drainPending();
    return true;
}

bool TgUpdatesManager::handleDifference(TgObject difference, qint64 messageId)
{
    if (_differenceRequestId == 0 || _differenceRequestId != messageId) {
        return staleReply("updates.Difference", messageId);
    }
    _differenceRequestId = 0;

    if (EMPTY(difference)) {
        kgCritical() << "updates.getDifference decoded to nothing";
        requestState();
        return true;
    }

    switch (ID(difference)) {
    case UpdatesDifferenceEmpty:
        _state.advanceSeq(difference["seq"].toInt(), difference["date"].toInt());
        drainPending();
        return true;

    case UpdatesDifference:
    case UpdatesDifferenceSlice:
    {
        applyDifferenceBody(difference, 0);

        const TgObject newState = difference.contains("intermediate_state")
                ? difference["intermediate_state"].toMap()
                : difference["state"].toMap();

        _state.reset(newState["pts"].toInt(),
                     newState["qts"].toInt(),
                     newState["date"].toInt(),
                     newState["seq"].toInt());

        save();
        publishState();
        drainPending();

        // A slice is a partial answer with a state to resume from. Stopping
        // here would leave the rest of the backlog unfetched and the client
        // believing it is caught up.
        if (ID(difference) == UpdatesDifferenceSlice) {
            requestDifference();
        }
        return true;
    }

    case UpdatesDifferenceTooLong:
        // The backlog is past the point where the server will enumerate it.
        // Everything held is stale and the dialog list this client shows is no
        // longer known to be current.
        kgWarning() << "differenceTooLong, resetting to pts" << difference["pts"].toInt();
        clearPending();
        _state.setPts(difference["pts"].toInt());
        _state.clearChannels();
        _channelQueue.clear();
        save();
        _client->dispatchUpdatesReset();
        return true;

    default:
        kgWarning() << "Unhandled updates.Difference constructor" << ID(difference);
        return true;
    }
}

bool TgUpdatesManager::handleChannelDifference(TgObject difference, qint64 messageId)
{
    if (!_channelRequestId.contains(messageId)) {
        return staleReply("updates.ChannelDifference", messageId);
    }
    const qint64 channelId = _channelRequestId.take(messageId);

    if (EMPTY(difference)) {
        kgCritical() << "updates.getChannelDifference decoded to nothing";
        pumpChannelQueue();
        return true;
    }

    switch (ID(difference)) {
    case UpdatesChannelDifferenceEmpty:
        _state.setChannelPts(channelId, difference["pts"].toInt());
        break;

    case UpdatesChannelDifference:
    {
        applyDifferenceBody(difference, channelId);
        _state.setChannelPts(channelId, difference["pts"].toInt());

        // final is a flag field, so it is absent rather than false when the
        // server has more to give.
        if (!difference["final"].toBool()) {
            _channelQueue.append(channelId);
        }
        break;
    }

    case UpdatesChannelDifferenceTooLong:
    {
        // The channel's backlog is past enumeration. The messages carried here
        // are a fresh window, not a sequence of events, so they are not
        // replayed as new-message updates -- that would duplicate whatever the
        // model already holds. The model is told to reload instead.
        const TgObject dialog = difference["dialog"].toMap();
        rememberChats(difference["chats"].toList());
        _state.setChannelPts(channelId, dialog["pts"].toInt());
        dropPending(channelId);
        _client->dispatchChannelReset(channelId);
        break;
    }

    default:
        kgWarning() << "Unhandled updates.ChannelDifference constructor" << ID(difference);
        break;
    }

    save();
    drainPending();
    pumpChannelQueue();
    return true;
}

void TgUpdatesManager::applyDifferenceBody(const TgObject &difference, qint64 channelId)
{
    const TgList users = difference["users"].toList();
    const TgList chats = difference["chats"].toList();
    rememberChats(chats);

    const qint32 date = _state.date();

    // Messages first, then the other updates: the models resolve a sender out
    // of the users and chats that arrive alongside, and an update referring to
    // a message is only meaningful once the message exists.
    const TgList newMessages = difference["new_messages"].toList();
    for (qint32 i = 0; i < newMessages.size(); ++i) {
        _client->dispatchUpdate(wrapNewMessage(newMessages[i].toMap()), users, chats, date);
    }

    const TgList otherUpdates = difference["other_updates"].toList();
    for (qint32 i = 0; i < otherUpdates.size(); ++i) {
        const TgObject update = otherUpdates[i].toMap();

        // The difference is the authority on what happened, so these are not
        // put through check() -- but the channel counters they carry still
        // have to be recorded, or the next push on that channel reads as a gap.
        if (TgUpdatesState::hasPts(update)) {
            const qint64 updateChannel = TgUpdatesState::channelIdOf(update);
            if (updateChannel) {
                _state.advance(updateChannel, update["pts"].toInt());
            }
        }

        _client->dispatchUpdate(update, users, chats, date);
    }

    (void) channelId;
}

TgObject TgUpdatesManager::wrapNewMessage(const TgObject &message)
{
    const TgObject peer = message["peer_id"].toMap();
    const bool isChannel = ID(peer) == PeerChannel;

    TGOBJECT(isChannel ? UpdateNewChannelMessage : UpdateNewMessage, update);
    update["message"] = message;
    // pts and pts_count are deliberately absent. The difference has already
    // moved the state, and leaving them out means this cannot be mistaken for
    // a pushed update and checked against a sequence it never belonged to.
    return update;
}

void TgUpdatesManager::expectAffected(qint64 messageId, qint64 channelId)
{
    if (messageId != 0) {
        _affectedChannel.insert(messageId, channelId);
    }
}

bool TgUpdatesManager::handleAffected(TgObject affected, qint64 messageId)
{
    if (!_affectedChannel.contains(messageId)) {
        // Someone else's reply, or one answered to a previous run of this
        // process. Applying its pts would move a sequence on evidence that
        // does not belong to it.
        return false;
    }

    const qint64 channelId = _affectedChannel.take(messageId);
    const qint32 pts = affected["pts"].toInt();
    const qint32 ptsCount = affected["pts_count"].toInt();

    switch (_state.check(channelId, pts, ptsCount)) {
    case TgUpdatesState::Apply:
        _state.advance(channelId, pts);
        maybeSave();

        if (channelId == 0) {
            _client->dispatchUpdatesState(_state.pts(), _state.qts(),
                                          _state.date(), _state.seq());
        }

        // Anything parked behind this may now be contiguous.
        drainPending();
        break;

    case TgUpdatesState::Duplicate:
        break;

    case TgUpdatesState::Gap:
        // Something else moved the sequence while this request was in flight.
        if (channelId == 0) {
            requestDifference();
        } else {
            enqueueChannel(channelId);
        }
        break;
    }

    return true;
}

bool TgUpdatesManager::handleRpcError(qint32 errorCode, QString errorMessage, qint64 messageId)
{
    (void) errorCode;

    if (_stateRequestId == messageId) {
        _stateRequestId = 0;
        return true;
    }

    if (_differenceRequestId == messageId) {
        _differenceRequestId = 0;

        // The stored pts or date is no longer one the server will accept --
        // usually because the session sat idle past the retention window.
        // Asking where the sequence is now is the only way forward.
        if (errorMessage.startsWith("PERSISTENT_TIMESTAMP")) {
            kgWarning() << "Stored update state rejected:" << errorMessage;
            _state.clear();
            clearPending();
            requestState();
        }
        return true;
    }

    if (_channelRequestId.contains(messageId)) {
        const qint64 channelId = _channelRequestId.take(messageId);

        // No longer reachable: stop tracking it rather than retrying a poll
        // that cannot succeed.
        if (errorMessage.startsWith("CHANNEL_PRIVATE")
                || errorMessage.startsWith("CHANNEL_INVALID")) {
            kgWarning() << "Dropping channel" << channelId << ":" << errorMessage;
            _state.forgetChannel(channelId);
            _channelAccessHash.remove(channelId);
            dropPending(channelId);
        }

        // Unconditional: the slot this request held has to be released
        // whatever the error was, or the cap leaks and channel polling stops.
        pumpChannelQueue();
        return true;
    }

    return false;
}
