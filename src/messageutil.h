#ifndef MESSAGEUTIL_H
#define MESSAGEUTIL_H

#include <QHash>
#include <QList>

#include "tgstream.h"

class TgStore;

// Peers seen recently, by id.
//
// Replaces two process-global lists that were appended to without checking for
// duplicates and searched with a loop per rendered row -- so a long session
// paid O(peers) per message and never gave any of it back. Bounded, because it
// is a cache and not a record: the durable copy is in TgStore and a miss falls
// through to it. On a phone whose heap starts at 4 MB, an index of every peer
// ever seen is the thing that eventually runs it out.
class TgPeerCache
{
public:
    enum { MaxEntries = 512 };

    TgPeerCache();

    void setStore(TgStore *store);

    void put(const TgList &users, const TgList &chats);

    // Users before chats, which is the order the scans this replaces used. Ids
    // are drawn from different spaces and a user is the more likely sender.
    TgObject byId(qint64 peerId);

    void clear();

private:
    void remember(QHash<qint64, TgObject> &into, QList<qint64> &order,
                  const TgList &list);

    QHash<qint64, TgObject> _users;
    QHash<qint64, TgObject> _chats;
    QList<qint64> _userOrder;
    QList<qint64> _chatOrder;
    TgStore *_store;
};

TgPeerCache& peerCache();

// Resolves a peer for a message that names one by id: the list that arrived
// with the response first, because it is the freshest, then the cache.
TgObject resolvePeer(const TgList &users, const TgList &chats, qint64 peerId);
QString prepareDialogItemMessage(QString text, TgList entities);
QString messageToHtml(QString text, TgList entities);
void handleMessageAction(TgObject &row, TgObject message, TgObject sender, TgList users, TgList chats);

#endif // MESSAGEUTIL_H
