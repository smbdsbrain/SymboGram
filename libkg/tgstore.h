#ifndef TGSTORE_H
#define TGSTORE_H

#include <QString>

#include "tgstream.h"

// The local cache: peers, the dialog list and message history.
//
// A cache and not a store, and the distinction is the whole design. Qt for
// Symbian may not offer a SQLite driver on every device, so open() is allowed
// to fail and every method below is a no-op or an empty result when it has.
// Nothing may become unavailable because the cache is: what it buys is a chat
// list that appears before the network answers, and lookups that are not a
// linear scan of every peer ever seen.
//
// Rows are QVariant blobs rather than a column per field. The generated schema
// gains fields at every layer, and a column-per-field mapping would need a
// migration for each one; the columns that do exist are the ones something is
// looked up or ordered by.
class TgStore
{
public:
    enum {
        // Rows kept per conversation. A phone with a 64 MB heap ceiling and a
        // shared C: drive cannot hold an unbounded history, and the network is
        // still there for anything older.
        MaxMessagesPerPeer = 200,

        // Bumped when the layout below changes. A mismatch drops every table
        // and starts again: this is a cache, and rebuilding it costs one
        // refetch, which is always cheaper than a migration.
        SchemaVersion = 1
    };

    TgStore();
    ~TgStore();

    // connectionName has to be unique per open database in the process. The
    // per-data-centre clients share one cache, so it is derived from the
    // session name rather than the data centre.
    bool open(const QString &filePath, const QString &connectionName);
    void close();
    bool isOpen() const;

    // Writes are batched into one transaction and committed by flush(), or by
    // the next write that finds the batch large enough. SQLite syncs once per
    // statement outside a transaction, so writing a hundred-message history
    // page row by row is a hundred flash writes.
    void putPeers(const TgList &users, const TgList &chats);
    void putDialogs(const TgList &dialogs, const TgList &messages, qint32 folderId);
    void putMessages(const TgObject &peer, const TgList &messages);

    // kind is TLType::User or TLType::Chat, as TgClient::commonPeerType
    // returns. Both are needed: a user id and a chat id may collide.
    TgObject peer(qint64 peerId, qint32 kind) const;
    TgList dialogs(qint32 folderId, qint32 limit) const;
    TgList messages(qint64 peerId, qint32 kind, qint32 fromId, qint32 limit) const;

    void deleteMessages(qint64 peerId, qint32 kind, const TgList &messageIds);

    void flush();
    void clear();

private:
    bool createSchema();
    void beginBatch();
    void trim(qint64 peerId, qint32 kind);

    static QByteArray pack(const TgVariant &value);
    static TgVariant unpack(const QByteArray &bytes);

    QString _connection;
    bool _open;
    bool _inBatch;
    qint32 _batched;
};

#endif // TGSTORE_H
