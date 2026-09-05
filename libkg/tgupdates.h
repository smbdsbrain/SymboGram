#ifndef TGUPDATES_H
#define TGUPDATES_H

#include <QBasicTimer>
#include <QHash>
#include <QList>
#include <QObject>
#include <QTime>

#include "tgupdatesstate.h"

class TgClient;

// One update held back because the sequence in front of it has not arrived.
struct TgPendingUpdate
{
    TgPendingUpdate();

    TgObject update;
    TgList users;
    TgList chats;
    qint64 messageId;
    qint64 channelId;
    qint32 pts;
    qint32 ptsCount;
    qint32 date;
    // The three updateShort*Message forms are containers, not Update objects,
    // and reach the models through a different signal. Carried here so a
    // parked one is re-emitted the way it would have been.
    bool shortForm;
    qint32 parkedAt;
};

// The update pipeline: sequence checking, gap recovery, and difference
// polling for one authorized session.
//
// Owned by the main TgClient only. The children getClientForDc() creates exist
// to move file parts and never receive updates, so giving each one a manager
// would cost an updates.getState per data centre for state nobody reads.
class TgUpdatesManager : public QObject
{
    Q_OBJECT
public:
    enum {
        // Telegram polls every channel separately, and an account in two
        // hundred of them would otherwise put two hundred requests on one
        // socket at once -- which the data centre answers with FLOOD_WAIT, on
        // a phone whose minimum heap is 4 MB. The rest wait their turn.
        MaxChannelPolls = 10,

        // A gap that never closes must not pin updates in memory for the life
        // of the session. Past this the queue is dropped whole and a
        // difference refetches whatever it held.
        MaxPending = 64,

        // How long a parked update waits for the one in front of it. Beyond
        // this the gap is presumed permanent and a difference is forced, so
        // the worst case for a delayed message is seconds rather than the rest
        // of the session.
        GapTimeoutMs = 5000,

        DifferenceLimit = 100,

        // Writing the state on every applied update would be a flash write per
        // message. Losing up to this much costs one extra getDifference at the
        // next start, which is the thing this pipeline exists to do well.
        SaveIntervalMs = 30000
    };

    explicit TgUpdatesManager(TgClient *client);

    const TgUpdatesState& state() const;

public slots:
    // Entry point for every Updates constructor, including updatesTooLong and
    // the three short-message forms.
    void processUpdates(TgObject updates, qint64 messageId);

    // Replies to the three methods this class issues. Each returns false when
    // the message id is not one it asked for, so TgClient::handleObject can
    // fall through to unknownResponse rather than swallowing someone else's
    // reply -- the same correlation the models do against their request ids.
    bool handleState(TgObject updatesState, qint64 messageId);
    bool handleDifference(TgObject difference, qint64 messageId);
    bool handleChannelDifference(TgObject difference, qint64 messageId);
    bool handleRpcError(qint32 errorCode, QString errorMessage, qint64 messageId);

    void authorized();
    void initialized();
    void disconnected();

    // Message ids are rewritten on data-centre migration and on flood retry,
    // so every map keyed by one has to be renamed with it. Same rename
    // TgClient::handleMessageChanged does for filePackets and migrationForDc.
    void messageChanged(qint64 oldMsg, qint64 newMsg);

    void save();

protected:
    void timerEvent(QTimerEvent *event);

private:
    void applyOne(const TgObject &update, const TgList &users, const TgList &chats,
                  qint32 date, qint64 messageId, bool shortForm);
    void emitOne(const TgObject &update, const TgList &users, const TgList &chats,
                 qint32 date, qint64 messageId, bool shortForm);

    void park(const TgObject &update, const TgList &users, const TgList &chats,
              qint64 channelId, qint32 pts, qint32 ptsCount, qint32 date,
              qint64 messageId, bool shortForm);
    void drainPending();
    void expirePending();
    void dropPending(qint64 channelId);
    void clearPending();

    void requestState();
    void requestDifference();
    void enqueueChannel(qint64 channelId);
    void pumpChannelQueue();
    void requestChannelDifference(qint64 channelId);

    bool staleReply(const char *what, qint64 messageId);
    void rememberChats(const TgList &chats);
    void publishState();
    void applyDifferenceBody(const TgObject &difference, qint64 channelId);
    void maybeSave();

    // A message from a difference is wrapped in the update constructor the
    // models already handle, rather than reaching them through a signal of its
    // own. Nothing about a difference-sourced message differs once it is in a
    // model, and a second path would mean a second branch in each of them.
    static TgObject wrapNewMessage(const TgObject &message);

    TgClient *_client;
    TgUpdatesState _state;

    qint64 _stateRequestId;
    qint64 _differenceRequestId;
    QHash<qint64, qint64> _channelRequestId;   // msg_id -> channel_id
    QList<qint64> _channelQueue;

    // channel_id -> access_hash, learned from the chats lists that arrive with
    // updates and differences. updates.getChannelDifference needs the hash and
    // libkg holds no peer cache of its own; a channel not in here is skipped
    // until a full difference brings it.
    QHash<qint64, qint64> _channelAccessHash;

    QList<TgPendingUpdate> _pending;
    QBasicTimer _gapTimer;
    qint32 _tick;
    QTime _lastSave;
};

#endif // TGUPDATES_H
