// Offline checks for the local cache.
//
// The cache is optional by design -- a device whose Qt has no SQLite driver
// must behave exactly as it did before it existed -- so the degradation path
// is tested as carefully as the working one. A cache that throws or crashes
// when it cannot open is worse than no cache at all, because it takes the
// client with it.

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QTextStream>

#include "tgstore.h"
#include "tlcase.h"
#include "tlschema.h"

static TgObject makeUser(qint64 id, const QString &firstName)
{
    TGOBJECT(TLType::User, user);
    user["id"] = id;
    user["access_hash"] = (qint64) (id * 31);
    user["first_name"] = firstName;
    return user;
}

static TgObject makeChannel(qint64 id, const QString &title)
{
    TGOBJECT(TLType::Channel, chat);
    chat["id"] = id;
    chat["access_hash"] = (qint64) (id * 17);
    chat["title"] = title;
    chat["broadcast"] = true;
    return chat;
}

static TgObject makeMessage(qint32 id, qint64 peerId, qint32 date, const QString &text)
{
    TGOBJECT(TLType::Message, message);
    TGOBJECT(TLType::PeerUser, peer);
    peer["user_id"] = peerId;
    message["id"] = id;
    message["peer_id"] = peer;
    message["date"] = date;
    message["message"] = text;
    return message;
}

static TgObject makeDialog(qint64 peerId, qint32 topMessage, bool pinned)
{
    TGOBJECT(TLType::Dialog, dialog);
    TGOBJECT(TLType::PeerUser, peer);
    peer["user_id"] = peerId;
    dialog["peer"] = peer;
    dialog["top_message"] = topMessage;
    if (pinned) dialog["pinned"] = true;
    return dialog;
}

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    QTextStream out(stdout);

    TlReport r;
    r.planned = 8;

    // The desktop toolchain ships the driver. Without it there is nothing here
    // to test but the degradation path, and reporting the rest as passing
    // would be a lie -- so the suite says so and skips.
    const bool haveDriver = QSqlDatabase::drivers().contains("QSQLITE");

    const QString path = QDir::temp().absoluteFilePath("symbogram_store_test.db");
    QFile::remove(path);

    if (!haveDriver) {
        out << "TAP version 13\n";
        out << "1..0 # SKIP no QSQLITE driver in this Qt build\n";
        out.flush();
        return 77;
    }

    // 1. Opening creates the schema and reports success.
    {
        TgStore store;
        if (store.open(path, "test_open") && store.isOpen()) {
            r.ok("open/creates the cache");
        } else {
            r.fail("open/creates the cache", "open() returned false");
        }
    }

    // 2-4. Peers, dialogs and messages survive a round trip through a blob.
    {
        TgStore store;
        store.open(path, "test_roundtrip");

        TgList users;
        users.append(makeUser(10, "Ada"));
        TgList chats;
        chats.append(makeChannel(20, "A channel"));
        store.putPeers(users, chats);

        TgList messages;
        messages.append(makeMessage(100, 10, 1700000000, "first"));
        messages.append(makeMessage(101, 10, 1700000100, "second"));
        store.putMessages(TgObject(), messages);

        TgList dialogs;
        dialogs.append(makeDialog(10, 101, false));
        store.putDialogs(dialogs, messages, 0);

        store.flush();

        const TgObject user = store.peer(10, TLType::User);
        if (user["first_name"].toString() == "Ada" && user["access_hash"].toLongLong() == 310) {
            r.ok("peers/round trip");
        } else {
            r.fail("peers/round trip", QString("read back %1").arg(user["first_name"].toString()));
        }

        const TgObject channel = store.peer(20, TLType::Chat);
        if (channel["title"].toString() == "A channel" && channel["broadcast"].toBool()) {
            r.ok("peers/a channel keeps its flags");
        } else {
            r.fail("peers/a channel keeps its flags", "flags or title lost");
        }

        const TgList back = store.messages(10, TLType::User, 0, 10);
        if (back.size() == 2 && back[0].toMap()["id"].toInt() == 101) {
            r.ok("messages/newest first");
        } else {
            r.fail("messages/newest first",
                   QString("got %1 rows, first id %2")
                       .arg(back.size())
                       .arg(back.isEmpty() ? 0 : back[0].toMap()["id"].toInt()));
        }
    }

    // 5. Pinned dialogs sort first, then by the date of their top message.
    {
        TgStore store;
        store.open(path, "test_order");

        TgList messages;
        messages.append(makeMessage(200, 30, 1700000000, "older"));
        messages.append(makeMessage(300, 40, 1700009999, "newer"));
        messages.append(makeMessage(400, 50, 1700000001, "pinned"));
        store.putMessages(TgObject(), messages);

        TgList dialogs;
        dialogs.append(makeDialog(30, 200, false));
        dialogs.append(makeDialog(40, 300, false));
        dialogs.append(makeDialog(50, 400, true));
        store.putDialogs(dialogs, messages, 0);
        store.flush();

        const TgList list = store.dialogs(0, 10);
        QString order;
        for (qint32 i = 0; i < list.size(); ++i) {
            order += QString::number(list[i].toMap()["top_message"].toInt()) + " ";
        }

        // 400 is pinned, then 300 (newest), then 200, then the dialog written
        // by the round-trip block above.
        if (list.size() >= 3
                && list[0].toMap()["top_message"].toInt() == 400
                && list[1].toMap()["top_message"].toInt() == 300) {
            r.ok("dialogs/pinned first then by date");
        } else {
            r.fail("dialogs/pinned first then by date", "order was " + order.trimmed());
        }
    }

    // 6. Trimming. An unbounded history file on a phone's C: drive is its own
    // failure, so a conversation keeps a fixed number of rows.
    {
        TgStore store;
        store.open(path, "test_trim");

        TgList messages;
        for (qint32 i = 1; i <= TgStore::MaxMessagesPerPeer + 50; ++i) {
            messages.append(makeMessage(1000 + i, 60, 1700000000 + i, "m"));
        }
        store.putMessages(TgObject(), messages);
        store.flush();

        const TgList back = store.messages(60, TLType::User, 0, TgStore::MaxMessagesPerPeer + 100);
        const qint32 newest = back.isEmpty() ? 0 : back[0].toMap()["id"].toInt();

        if (back.size() == TgStore::MaxMessagesPerPeer
                && newest == 1000 + TgStore::MaxMessagesPerPeer + 50) {
            r.ok("messages/trimmed to the newest rows");
        } else {
            r.fail("messages/trimmed to the newest rows",
                   QString("kept %1 rows, newest id %2").arg(back.size()).arg(newest));
        }
    }

    // 7. Deliberate-failure control.
    //
    // A cache that cannot be opened has to leave the client working. Every
    // method below is called on a closed store: each must be a no-op or an
    // empty result, and none may crash. This is the path a device without the
    // driver takes for its whole life.
    {
        TgStore store;
        const bool opened = store.open("/this/path/does/not/exist/cache.db", "test_closed");

        TgList users;
        users.append(makeUser(1, "nobody"));
        store.putPeers(users, TgList());
        store.putDialogs(TgList(), TgList(), 0);
        store.putMessages(TgObject(), users);
        store.deleteMessages(1, TLType::User, TgList());
        store.flush();
        store.clear();

        const bool quiet = !opened
                && !store.isOpen()
                && store.peer(1, TLType::User).isEmpty()
                && store.dialogs(0, 10).isEmpty()
                && store.messages(1, TLType::User, 0, 10).isEmpty();

        if (quiet) {
            r.ok("control/a cache that will not open stays silent");
        } else {
            r.fail("control/a cache that will not open stays silent",
                   opened ? "open() claimed success on an unwritable path"
                          : "a closed store returned data");
        }
    }

    // 8. A layout change empties the cache rather than misreading it.
    {
        {
            QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "test_bump_setup");
            db.setDatabaseName(path);
            db.open();
            QSqlQuery q(db);
            q.exec("INSERT OR REPLACE INTO meta (key, value) VALUES ('schema_version', '999')");
            db.close();
        }
        QSqlDatabase::removeDatabase("test_bump_setup");

        TgStore store;
        store.open(path, "test_bump");
        const TgObject user = store.peer(10, TLType::User);

        if (user.isEmpty()) {
            r.ok("control/a changed layout discards the cache");
        } else {
            r.fail("control/a changed layout discards the cache",
                   "rows from the previous layout were still readable");
        }
    }

    QFile::remove(path);

    const int rc = r.finish();
    out.flush();
    return rc;
}
