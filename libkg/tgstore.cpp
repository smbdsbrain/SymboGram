#include "tgstore.h"

#include <QDataStream>
#include <QHash>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QStringList>
#include <QVariant>

#include "debug.h"
#include "tlschema.h"

// Batched writes are committed once this many statements have accumulated, so
// a long history page does not sit uncommitted while the user reads it.
static const qint32 kBatchLimit = 200;

// Pinned rather than left at the running Qt's default. The device build is
// Qt 4.7.3 and the desktop one 4.8.7, and a stream written by one has to be
// readable by the other -- and by whatever a later Qt brings. The version is
// recorded in meta as well, so a change here empties the cache instead of
// misreading it.
static const int kStreamVersion = QDataStream::Qt_4_6;

TgStore::TgStore()
    : _connection()
    , _open(false)
    , _inBatch(false)
    , _batched(0)
{
}

TgStore::~TgStore()
{
    close();
}

QByteArray TgStore::pack(const TgVariant &value)
{
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(kStreamVersion);
    stream << value;
    return bytes;
}

TgVariant TgStore::unpack(const QByteArray &bytes)
{
    QByteArray copy(bytes);
    QDataStream stream(&copy, QIODevice::ReadOnly);
    stream.setVersion(kStreamVersion);
    TgVariant value;
    stream >> value;
    return value;
}

bool TgStore::open(const QString &filePath, const QString &connectionName)
{
    close();

    // Qt for Symbian builds the SQLite driver into QtSql rather than shipping
    // it as a loadable plugin, and whether a given device's Qt has it is not
    // something any test tier here can answer. Asked once, reported once, and
    // the app carries on without a cache if the answer is no.
    if (!QSqlDatabase::drivers().contains("QSQLITE")) {
        kgWarning() << "No QSQLITE driver; running without the local cache";
        return false;
    }

    _connection = connectionName;

    // Scoped, because removeDatabase() below warns and disables the connection
    // for anyone still holding a QSqlDatabase copy of it -- including this one.
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", _connection);
        db.setDatabaseName(filePath);

        if (!db.open()) {
            kgWarning() << "Could not open the cache at" << filePath
                        << ":" << db.lastError().text();
        } else {
            _open = true;
        }
    }

    if (!_open) {
        QSqlDatabase::removeDatabase(_connection);
        _connection.clear();
        return false;
    }

    if (!createSchema()) {
        close();
        return false;
    }

    kgInfo() << "Local cache open at" << filePath;
    return true;
}

void TgStore::close()
{
    if (!_open) {
        return;
    }

    flush();

    {
        QSqlDatabase db = QSqlDatabase::database(_connection);
        if (db.isOpen()) {
            db.close();
        }
    }

    // Scoped above, because removeDatabase warns when a QSqlDatabase copy of
    // the connection is still alive.
    QSqlDatabase::removeDatabase(_connection);

    _connection.clear();
    _open = false;
    _inBatch = false;
    _batched = 0;
}

bool TgStore::isOpen() const
{
    return _open;
}

bool TgStore::createSchema()
{
    QSqlDatabase db = QSqlDatabase::database(_connection);
    QSqlQuery q(db);

    // A rollback journal rather than WAL: WAL needs a shared memory mapping
    // and buys nothing for one connection on one thread.
    q.exec("PRAGMA journal_mode = TRUNCATE");
    q.exec("PRAGMA synchronous = NORMAL");
    q.exec("PRAGMA temp_store = MEMORY");

    if (!q.exec("CREATE TABLE IF NOT EXISTS meta ("
                "key TEXT PRIMARY KEY, value TEXT)")) {
        kgWarning() << "Cache schema failed:" << q.lastError().text();
        return false;
    }

    qint32 version = 0;
    int streamVersion = 0;
    if (q.exec("SELECT key, value FROM meta")) {
        while (q.next()) {
            const QString key = q.value(0).toString();
            if (key == "schema_version") version = q.value(1).toInt();
            if (key == "stream_version") streamVersion = q.value(1).toInt();
        }
    }

    const bool stale = (version != 0 && version != SchemaVersion)
            || (streamVersion != 0 && streamVersion != kStreamVersion);

    if (stale) {
        kgInfo() << "Cache layout changed, discarding it";
        q.exec("DROP TABLE IF EXISTS peers");
        q.exec("DROP TABLE IF EXISTS dialogs");
        q.exec("DROP TABLE IF EXISTS messages");
    }

    bool ok = true;

    // peer_kind is TLType::User or TLType::Chat, and it is part of every key
    // below: a user id and a chat id are drawn from different spaces and may
    // collide, which is why TgClient::peersEqual compares both.
    ok = ok && q.exec("CREATE TABLE IF NOT EXISTS peers ("
                      "peer_id INTEGER NOT NULL,"
                      "peer_kind INTEGER NOT NULL,"
                      "title TEXT,"
                      "blob BLOB NOT NULL,"
                      "PRIMARY KEY (peer_id, peer_kind))");

    ok = ok && q.exec("CREATE TABLE IF NOT EXISTS dialogs ("
                      "peer_id INTEGER NOT NULL,"
                      "peer_kind INTEGER NOT NULL,"
                      "folder_id INTEGER NOT NULL DEFAULT 0,"
                      "top_message INTEGER NOT NULL DEFAULT 0,"
                      "order_date INTEGER NOT NULL DEFAULT 0,"
                      "pinned INTEGER NOT NULL DEFAULT 0,"
                      "blob BLOB NOT NULL,"
                      "PRIMARY KEY (peer_id, peer_kind))");

    // Covers the only order the dialog list is ever read in.
    ok = ok && q.exec("CREATE INDEX IF NOT EXISTS dialogs_order "
                      "ON dialogs(folder_id, pinned DESC, order_date DESC)");

    ok = ok && q.exec("CREATE TABLE IF NOT EXISTS messages ("
                      "peer_id INTEGER NOT NULL,"
                      "peer_kind INTEGER NOT NULL,"
                      "message_id INTEGER NOT NULL,"
                      "date INTEGER NOT NULL DEFAULT 0,"
                      "blob BLOB NOT NULL,"
                      "PRIMARY KEY (peer_id, peer_kind, message_id))");

    if (!ok) {
        kgWarning() << "Cache schema failed:" << q.lastError().text();
        return false;
    }

    q.prepare("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)");
    q.addBindValue("schema_version");
    q.addBindValue(SchemaVersion);
    q.exec();
    q.prepare("INSERT OR REPLACE INTO meta (key, value) VALUES (?, ?)");
    q.addBindValue("stream_version");
    q.addBindValue(kStreamVersion);
    q.exec();

    return true;
}

void TgStore::beginBatch()
{
    if (_inBatch) {
        return;
    }
    QSqlDatabase::database(_connection).transaction();
    _inBatch = true;
    _batched = 0;
}

void TgStore::flush()
{
    if (!_open || !_inBatch) {
        return;
    }
    QSqlDatabase::database(_connection).commit();
    _inBatch = false;
    _batched = 0;
}

void TgStore::clear()
{
    if (!_open) {
        return;
    }

    flush();

    QSqlQuery q(QSqlDatabase::database(_connection));
    q.exec("DELETE FROM peers");
    q.exec("DELETE FROM dialogs");
    q.exec("DELETE FROM messages");
}

// --- writes -----------------------------------------------------------------

void TgStore::putPeers(const TgList &users, const TgList &chats)
{
    if (!_open || (users.isEmpty() && chats.isEmpty())) {
        return;
    }

    beginBatch();

    QSqlQuery q(QSqlDatabase::database(_connection));
    q.prepare("INSERT OR REPLACE INTO peers (peer_id, peer_kind, title, blob) "
              "VALUES (?, ?, ?, ?)");

    for (qint32 pass = 0; pass < 2; ++pass) {
        const TgList &list = pass == 0 ? users : chats;
        const qint32 kind = pass == 0 ? TLType::User : TLType::Chat;

        for (qint32 i = 0; i < list.size(); ++i) {
            const TgObject item = list[i].toMap();
            const qint64 id = item["id"].toLongLong();
            if (id == 0) {
                continue;
            }

            // userEmpty and chatEmpty carry an id and nothing else. Storing one
            // over a full record would replace a usable peer with a stub.
            if (ID(item) == TLType::UserEmpty || ID(item) == TLType::ChatEmpty) {
                continue;
            }

            // QT_USE_FAST_CONCATENATION makes operator+ yield a QStringBuilder
            // expression, which has no string methods of its own; it has to be
            // materialised before anything can be done with it.
            QString title = item["title"].toString();
            if (pass == 0) {
                title = QString(item["first_name"].toString() + " "
                                + item["last_name"].toString()).trimmed();
            }

            q.addBindValue(id);
            q.addBindValue(kind);
            q.addBindValue(title);
            q.addBindValue(pack(item));
            q.exec();
            ++_batched;
        }
    }

    if (_batched >= kBatchLimit) {
        flush();
    }
}

void TgStore::putDialogs(const TgList &dialogs, const TgList &messages, qint32 folderId)
{
    if (!_open || dialogs.isEmpty()) {
        return;
    }

    beginBatch();

    // The date a dialog sorts by lives on its top message, not on the dialog,
    // so the messages that arrived alongside are indexed first.
    QHash<qint32, qint32> dateForMessage;
    for (qint32 i = 0; i < messages.size(); ++i) {
        const TgObject message = messages[i].toMap();
        dateForMessage.insert(message["id"].toInt(), message["date"].toInt());
    }

    QSqlQuery q(QSqlDatabase::database(_connection));
    q.prepare("INSERT OR REPLACE INTO dialogs "
              "(peer_id, peer_kind, folder_id, top_message, order_date, pinned, blob) "
              "VALUES (?, ?, ?, ?, ?, ?, ?)");

    for (qint32 i = 0; i < dialogs.size(); ++i) {
        const TgObject dialog = dialogs[i].toMap();
        const TgObject peer = dialog["peer"].toMap();

        qint64 peerId = 0;
        qint32 kind = 0;
        switch (ID(peer)) {
        case TLType::PeerUser:
            peerId = peer["user_id"].toLongLong();
            kind = TLType::User;
            break;
        case TLType::PeerChat:
            peerId = peer["chat_id"].toLongLong();
            kind = TLType::Chat;
            break;
        case TLType::PeerChannel:
            peerId = peer["channel_id"].toLongLong();
            kind = TLType::Chat;
            break;
        default:
            continue;
        }

        const qint32 topMessage = dialog["top_message"].toInt();

        q.addBindValue(peerId);
        q.addBindValue(kind);
        q.addBindValue(folderId);
        q.addBindValue(topMessage);
        q.addBindValue(dateForMessage.value(topMessage, 0));
        q.addBindValue(dialog["pinned"].toBool() ? 1 : 0);
        q.addBindValue(pack(dialog));
        q.exec();
        ++_batched;
    }

    if (_batched >= kBatchLimit) {
        flush();
    }
}

void TgStore::putMessages(const TgObject &peer, const TgList &messages)
{
    if (!_open || messages.isEmpty()) {
        return;
    }

    beginBatch();

    QSqlQuery q(QSqlDatabase::database(_connection));
    q.prepare("INSERT OR REPLACE INTO messages "
              "(peer_id, peer_kind, message_id, date, blob) VALUES (?, ?, ?, ?, ?)");

    // Every conversation touched by this batch, so each is trimmed once at the
    // end rather than after every row.
    QHash<qint64, qint32> touched;

    for (qint32 i = 0; i < messages.size(); ++i) {
        const TgObject message = messages[i].toMap();
        const qint32 messageId = message["id"].toInt();
        if (messageId == 0) {
            continue;
        }

        // Read from the message where it has one: a history page is a single
        // conversation, but a difference carries several at once.
        const TgObject messagePeer = message["peer_id"].toMap();
        const TgObject source = EMPTY(messagePeer) ? peer : messagePeer;

        qint64 id = 0;
        qint32 kind = 0;
        switch (ID(source)) {
        case TLType::PeerUser:    id = source["user_id"].toLongLong();    kind = TLType::User; break;
        case TLType::PeerChat:    id = source["chat_id"].toLongLong();    kind = TLType::Chat; break;
        case TLType::PeerChannel: id = source["channel_id"].toLongLong(); kind = TLType::Chat; break;
        default: continue;
        }

        q.addBindValue(id);
        q.addBindValue(kind);
        q.addBindValue(messageId);
        q.addBindValue(message["date"].toInt());
        q.addBindValue(pack(message));
        q.exec();
        ++_batched;

        touched.insert(id, kind);
    }

    for (QHash<qint64, qint32>::const_iterator it = touched.constBegin();
         it != touched.constEnd(); ++it) {
        trim(it.key(), it.value());
    }

    if (_batched >= kBatchLimit) {
        flush();
    }
}

void TgStore::trim(qint64 peerId, qint32 kind)
{
    QSqlQuery q(QSqlDatabase::database(_connection));
    q.prepare("DELETE FROM messages WHERE peer_id = ? AND peer_kind = ? AND message_id NOT IN "
              "(SELECT message_id FROM messages WHERE peer_id = ? AND peer_kind = ? "
              " ORDER BY message_id DESC LIMIT ?)");
    q.addBindValue(peerId);
    q.addBindValue(kind);
    q.addBindValue(peerId);
    q.addBindValue(kind);
    q.addBindValue((qint32) MaxMessagesPerPeer);
    q.exec();
}

void TgStore::deleteMessages(qint64 peerId, qint32 kind, const TgList &messageIds)
{
    if (!_open || messageIds.isEmpty()) {
        return;
    }

    beginBatch();

    QSqlQuery q(QSqlDatabase::database(_connection));
    q.prepare("DELETE FROM messages WHERE peer_id = ? AND peer_kind = ? AND message_id = ?");

    for (qint32 i = 0; i < messageIds.size(); ++i) {
        q.addBindValue(peerId);
        q.addBindValue(kind);
        q.addBindValue(messageIds[i].toInt());
        q.exec();
        ++_batched;
    }
}

// --- reads ------------------------------------------------------------------

TgObject TgStore::peer(qint64 peerId, qint32 kind) const
{
    if (!_open) {
        return TgObject();
    }

    QSqlQuery q(QSqlDatabase::database(_connection));
    q.prepare("SELECT blob FROM peers WHERE peer_id = ? AND peer_kind = ?");
    q.addBindValue(peerId);
    q.addBindValue(kind);

    if (!q.exec() || !q.next()) {
        return TgObject();
    }

    return unpack(q.value(0).toByteArray()).toMap();
}

TgList TgStore::dialogs(qint32 folderId, qint32 limit) const
{
    TgList out;
    if (!_open) {
        return out;
    }

    QSqlQuery q(QSqlDatabase::database(_connection));
    q.prepare("SELECT blob FROM dialogs WHERE folder_id = ? "
              "ORDER BY pinned DESC, order_date DESC LIMIT ?");
    q.addBindValue(folderId);
    q.addBindValue(limit);

    if (!q.exec()) {
        return out;
    }

    while (q.next()) {
        out.append(unpack(q.value(0).toByteArray()));
    }

    return out;
}

TgList TgStore::messages(qint64 peerId, qint32 kind, qint32 fromId, qint32 limit) const
{
    TgList out;
    if (!_open) {
        return out;
    }

    QSqlQuery q(QSqlDatabase::database(_connection));

    // fromId 0 means the newest page. Otherwise everything strictly older,
    // which is the direction history is paged in.
    if (fromId == 0) {
        q.prepare("SELECT blob FROM messages WHERE peer_id = ? AND peer_kind = ? "
                  "ORDER BY message_id DESC LIMIT ?");
        q.addBindValue(peerId);
        q.addBindValue(kind);
        q.addBindValue(limit);
    } else {
        q.prepare("SELECT blob FROM messages WHERE peer_id = ? AND peer_kind = ? "
                  "AND message_id < ? ORDER BY message_id DESC LIMIT ?");
        q.addBindValue(peerId);
        q.addBindValue(kind);
        q.addBindValue(fromId);
        q.addBindValue(limit);
    }

    if (!q.exec()) {
        return out;
    }

    while (q.next()) {
        out.append(unpack(q.value(0).toByteArray()));
    }

    return out;
}
