#include "dialogsmodel.h"

#include "debug.h"

#include "tlschema.h"
#include <QMutexLocker>
#include <QColor>
#include <QDateTime>
#include "messageutil.h"

//TODO archived chats

DialogsModel::DialogsModel(QObject *parent)
    : QAbstractListModel(parent)
    , _mutex(QMutex::Recursive)
    , _dialogs()
    , _client(0)
    , _userId(0)
    , _requestId(0)
    , _offsets()
    , _avatarDownloader(0)
    , _folders(0)
    , _lastPinnedIndex(-1)
    , _fromCache(false)
{
#if QT_VERSION < 0x050000
    setRoleNames(roleNames());
#endif
}

void DialogsModel::setFolders(QObject *model)
{
    QMutexLocker lock(&_mutex);

    if (_folders) {
        _folders->disconnect(this);
    }

    _folders = dynamic_cast<FoldersModel*>(model);
    foldersChanged(_folders ? _folders->folders() : QList<TgObject>());

    if (!_folders) return;

    connect(_folders, SIGNAL(foldersChanged(QList<TgObject>)), this, SLOT(foldersChanged(QList<TgObject>)));
}

QObject* DialogsModel::folders() const
{
    return _folders;
}

void DialogsModel::foldersChanged(QList<TgObject> folders)
{
    QMutexLocker lock(&_mutex);

    for (qint32 i = 0; i < _dialogs.size(); ++i) {
        TgObject row = _dialogs[i];

        TgList dialogFolders;

        for (qint32 j = 0; j < folders.size(); ++j) {
            if (FoldersModel::matchesFilter(folders[j], qDeserialize(row["peerBytes"].toByteArray()).toMap())) {
                dialogFolders << j;
            }
        }

        row["folders"] = dialogFolders;

        _dialogs[i] = row;

        emit dataChanged(index(i), index(i));
    }
}

void DialogsModel::resetState()
{
    if (!_dialogs.isEmpty()) {
        beginRemoveRows(QModelIndex(), 0, _dialogs.size() - 1);
        _dialogs.clear();
        endRemoveRows();
    }

    _requestId = 0;
    _offsets = TgObject();
    _offsets["_start"] = true;
    _lastPinnedIndex = -1;
    _fromCache = false;
}

QHash<int, QByteArray> DialogsModel::roleNames() const
{
    static QHash<int, QByteArray> roles;

    if (!roles.isEmpty())
        return roles;

    roles[TitleRole] = "title";
    roles[ThumbnailColorRole] = "thumbnailColor";
    roles[ThumbnailTextRole] = "thumbnailText";
    roles[AvatarRole] = "avatar";
    roles[MessageTimeRole] = "messageTime";
    roles[MessageTextRole] = "messageText";
    roles[TooltipRole] = "tooltip";
    roles[PeerBytesRole] = "peerBytes";
    roles[MessageSenderNameRole] = "messageSenderName";
    roles[MessageSenderColorRole] = "messageSenderColor";

    return roles;
}

void DialogsModel::setClient(QObject *client)
{
    QMutexLocker lock(&_mutex);

    if (_client) {
        _client->disconnect(this);
    }

    _client = dynamic_cast<TgClient*>(client);
    _userId = 0;

    resetState();

    if (!_client) return;

    peerCache().setStore(_client->store());

    connect(_client, SIGNAL(authorized(TgLongVariant)), this, SLOT(authorized(TgLongVariant)));
    connect(_client, SIGNAL(messagesDialogsResponse(TgObject,TgLongVariant)), this, SLOT(messagesGetDialogsResponse(TgObject,TgLongVariant)));
    connect(_client, SIGNAL(gotUpdate(TgObject,TgLongVariant,TgList,TgList,qint32,qint32,qint32)), this, SLOT(gotUpdate(TgObject,TgLongVariant,TgList,TgList,qint32,qint32,qint32)));
    connect(_client, SIGNAL(updatesReset()), this, SLOT(updatesReset()));
}

QObject* DialogsModel::client() const
{
    return _client;
}

void DialogsModel::setAvatarDownloader(QObject *avatarDownloader)
{
    QMutexLocker lock(&_mutex);

    if (_avatarDownloader) {
        _avatarDownloader->disconnect(this);
    }

    _avatarDownloader = dynamic_cast<AvatarDownloader*>(avatarDownloader);

    if (!_avatarDownloader) return;

    connect(_avatarDownloader, SIGNAL(avatarDownloaded(TgLongVariant,QString)), this, SLOT(avatarDownloaded(TgLongVariant,QString)));
}

QObject* DialogsModel::avatarDownloader() const
{
    return _avatarDownloader;
}

int DialogsModel::rowCount(const QModelIndex &parent) const
{
    return _dialogs.size();
}

QVariant DialogsModel::data(const QModelIndex &index, int role) const
{
    if (index.row() < 0) //TODO why this is even calling
        return QVariant();

    return _dialogs[index.row()][roleNames()[role]];
}

bool DialogsModel::canFetchMoreDownwards() const
{
    return _client && _client->isAuthorized() && !_requestId.toLongLong() && !_offsets.isEmpty();
}

void DialogsModel::fetchMoreDownwards()
{
    QMutexLocker lock(&_mutex);

    _requestId = _client->messagesGetDialogsWithOffsets(_offsets, 40);
}

void DialogsModel::authorized(TgLongVariant userId)
{
    QMutexLocker lock(&_mutex);

    if (_userId != userId) {
        resetState();
        _userId = userId;
        seedFromCache();
        fetchMoreDownwards();
    }
}

void DialogsModel::messagesGetDialogsResponse(TgObject data, TgLongVariant messageId)
{
    QMutexLocker lock(&_mutex);

    if (_requestId != messageId) {
        return;
    }

    _requestId = 0;

    switch (GETID(data)) {
    case TLType::MessagesDialogs:
    case TLType::MessagesDialogsNotModified:
        _offsets = TgObject();
        break;
    case TLType::MessagesDialogsSlice:
        _offsets = TgClient::getDialogsOffsets(data);
        break;
    }

    TgList dialogsList = data["dialogs"].toList();
    TgList messagesList = data["messages"].toList();
    TgList usersList = data["users"].toList();
    TgList chatsList = data["chats"].toList();

    peerCache().put(usersList, chatsList);

    if (dialogsList.isEmpty()) {
        _offsets = TgObject();
        return;
    }

    QList<TgObject> dialogsRows = buildRows(dialogsList, messagesList, usersList, chatsList);

    // The answer supersedes the cached list rather than being merged into it.
    // Order, unread counts and titles may all have moved while the app was
    // closed, and merging would show both versions of each.
    //
    // A reset rather than a removal followed by an insertion: removing every
    // row leaves the view holding delegates for indices that no longer exist,
    // and each one re-evaluates its bindings against them before it is torn
    // down. A reset tells the view the whole thing changed and it rebuilds.
    if (_fromCache) {
        _fromCache = false;
        _lastPinnedIndex = -1;

        beginResetModel();
        _dialogs = dialogsRows;
        endResetModel();
    } else {
        beginInsertRows(QModelIndex(), _dialogs.size(), _dialogs.size() + dialogsRows.size() - 1);
        _dialogs.append(dialogsRows);
        endInsertRows();
    }

    if (_avatarDownloader) {
        for (qint32 i = 0; i < usersList.size(); ++i) {
            _avatarDownloader->downloadAvatar(usersList[i].toMap());
        }
        for (qint32 i = 0; i < chatsList.size(); ++i) {
            _avatarDownloader->downloadAvatar(chatsList[i].toMap());
        }
    }

    if (canFetchMoreDownwards())
        fetchMoreDownwards();
}

QList<TgObject> DialogsModel::buildRows(TgList dialogsList, TgList messagesList,
                                       TgList usersList, TgList chatsList)
{
    QList<TgObject> dialogsRows;
    dialogsRows.reserve(dialogsList.size());

    QList<TgObject> folders;
    if (_folders)
        folders = _folders->folders();

    for (qint32 i = 0; i < dialogsList.size(); ++i) {
        TgObject lastDialog = dialogsList[i].toMap();

        if (lastDialog["pinned"].toBool()) {
            _lastPinnedIndex = qMax(_lastPinnedIndex, _dialogs.size() + i);
        }

        TgObject lastDialogPeer = lastDialog["peer"].toMap();
        TgInt lastMessageId = lastDialog["top_message"].toInt();

        TgObject lastMessage;
        TgObject lastPeer;

        for (qint32 j = messagesList.size() - 1; j >= 0; --j) {
            TgObject message = messagesList[j].toMap();
            if (TgClient::peersEqual(message["peer_id"].toMap(), lastDialogPeer)
                    && message["id"].toInt() == lastMessageId) {
                lastMessage = message;
                break;
            }
        }

        if (TgClient::isUser(lastDialogPeer)) for (qint32 j = 0; j < usersList.size(); ++j) {
            TgObject peer = usersList[j].toMap();
            if (TgClient::peersEqual(peer, lastDialogPeer)) {
                lastPeer = peer;
                break;
            }
        }
        if (TgClient::isChat(lastDialogPeer)) for (qint32 j = 0; j < chatsList.size(); ++j) {
            TgObject peer = chatsList[j].toMap();
            if (TgClient::peersEqual(peer, lastDialogPeer)) {
                lastPeer = peer;
                break;
            }
        }

        TgObject fromId = lastMessage["from_id"].toMap();
        TgObject messageSender;

        if (TgClient::isUser(fromId)) for (qint32 j = 0; j < usersList.size(); ++j) {
            TgObject peer = usersList[j].toMap();
            if (TgClient::peersEqual(peer, fromId)) {
                messageSender = peer;
                break;
            }
        }
        if (TgClient::isChat(fromId)) for (qint32 j = 0; j < chatsList.size(); ++j) {
            TgObject peer = chatsList[j].toMap();
            if (TgClient::peersEqual(peer, fromId)) {
                messageSender = peer;
                break;
            }
        }

        dialogsRows.append(createRow(lastDialog, lastPeer, lastMessage, messageSender, folders, usersList, chatsList));
    }

    return dialogsRows;
}

void DialogsModel::seedFromCache()
{
    if (!_client) {
        return;
    }

    TgStore *store = _client->store();
    if (!store->isOpen()) {
        return;
    }

    const TgList cachedDialogs = store->dialogs(0, 40);
    if (cachedDialogs.isEmpty()) {
        return;
    }

    // Assembled into the same four lists a response arrives in, so buildRows
    // needs no idea where any of it came from.
    TgList messagesList;
    TgList usersList;
    TgList chatsList;

    for (qint32 i = 0; i < cachedDialogs.size(); ++i) {
        const TgObject peer = cachedDialogs[i].toMap()["peer"].toMap();

        qint64 peerId = 0;
        qint32 kind = 0;
        switch (ID(peer)) {
        case TLType::PeerUser:    peerId = peer["user_id"].toLongLong();    kind = TLType::User; break;
        case TLType::PeerChat:    peerId = peer["chat_id"].toLongLong();    kind = TLType::Chat; break;
        case TLType::PeerChannel: peerId = peer["channel_id"].toLongLong(); kind = TLType::Chat; break;
        default: continue;
        }

        const TgObject cachedPeer = store->peer(peerId, kind);
        if (!EMPTY(cachedPeer)) {
            if (kind == TLType::User) {
                usersList.append(cachedPeer);
            } else {
                chatsList.append(cachedPeer);
            }
        }

        const TgList top = store->messages(peerId, kind, 0, 1);
        if (top.isEmpty()) {
            continue;
        }

        const TgObject message = top[0].toMap();
        messagesList.append(message);

        const qint64 senderId = TgClient::getPeerId(message["from_id"].toMap()).toLongLong();
        if (senderId == 0) {
            continue;
        }

        const TgObject sender = peerCache().byId(senderId);
        if (EMPTY(sender)) {
            continue;
        }
        if (TgClient::isUser(sender)) {
            usersList.append(sender);
        } else {
            chatsList.append(sender);
        }
    }

    const QList<TgObject> rows = buildRows(cachedDialogs, messagesList, usersList, chatsList);
    if (rows.isEmpty()) {
        return;
    }

    // Marked, so the first page from the network replaces this rather than
    // being appended after it.
    _fromCache = true;

    beginInsertRows(QModelIndex(), _dialogs.size(), _dialogs.size() + rows.size() - 1);
    _dialogs.append(rows);
    endInsertRows();

    kgInfo() << "Showed" << rows.size() << "dialogs from the cache";
}

void DialogsModel::handleDialogMessage(TgObject &row, TgObject message, TgObject messageSender, TgList users, TgList chats)
{
    //TODO 12-hour format
    row["messageTime"] = QDateTime::fromTime_t(qMax(message["date"].toInt(), message["edit_date"].toInt())).toString("hh:mm");

    QString messageSenderName;

    row["messageOut"] = message["out"].toBool();

    if (message["out"].toBool()) {
        if (ID(message["action"].toMap()) != 0) {
            messageSenderName = "You";
        }
    } else if (TgClient::isUser(messageSender)) {
        messageSenderName = messageSender["first_name"].toString();
    } else {
        messageSenderName = messageSender["title"].toString();
    }

    if (!messageSenderName.isEmpty()) {
        if (ID(message["action"].toMap()) == 0) {
            messageSenderName += ": ";
        } else {
            messageSenderName += " ";
        }
    }

    row["messageSenderName"] = messageSenderName;
    row["messageSenderColor"] = AvatarDownloader::userColor(messageSender["id"]);

    QString messageText = prepareDialogItemMessage(message["message"].toString(), message["entities"].toList());
    QString afterMessageText;
    if (GETID(message["media"].toMap()) != 0) {
        if (!messageText.isEmpty()) {
            afterMessageText += ", ";
        }

        //TODO attachment type
        afterMessageText += "Attachment";
    }
    messageText += afterMessageText;
    row["messageText"] = messageText;

    handleMessageAction(row, message, messageSender, users, chats);
}

TgObject DialogsModel::createRow(TgObject dialog, TgObject peer, TgObject message, TgObject messageSender, QList<TgObject> folders, TgList users, TgList chats)
{
    TgObject row;

    row["peer"] = peer;
    row["pinned"] = dialog["pinned"].toBool();
    row["silent"] = dialog["notify_settings"].toMap()["silent"].toBool();

    TgObject inputPeer = peer;
    inputPeer.unite(dialog);
    ID_PROPERTY(inputPeer) = ID_PROPERTY(peer);
    row["peerBytes"] = qSerialize(inputPeer);

    TgList dialogFolders;

    for (qint32 j = 0; j < folders.size(); ++j) {
        if (FoldersModel::matchesFilter(folders[j], inputPeer)) {
            dialogFolders << j;
        }
    }

    row["folders"] = dialogFolders;

    //TODO typing status
    if (TgClient::isUser(peer)) {
        row["title"] = QString(peer["first_name"].toString() + " " + peer["last_name"].toString());
        row["tooltip"] = "user"; //TODO last seen and online
    } else {
        row["title"] = peer["title"].toString();

        QString tooltip = TgClient::isChannel(peer) ? "channel" : "chat";
        if (!peer["participants_count"].isNull()) {
            tooltip = peer["participants_count"].toString();
            //TODO localization support
            tooltip += TgClient::isChannel(peer) ? " subscribers" : " members";
        }

        row["tooltip"] = tooltip;
    }

    row["thumbnailColor"] = AvatarDownloader::userColor(peer["id"].toLongLong());
    row["thumbnailText"] = AvatarDownloader::getAvatarText(row["title"].toString());
    row["avatar"] = "";
    row["photoId"] = peer["photo"].toMap()["photo_id"];

    handleDialogMessage(row, message, messageSender, users, chats);

    return row;
}

void DialogsModel::avatarDownloaded(TgLongVariant photoId, QString filePath)
{
    QMutexLocker lock(&_mutex);

    for (qint32 i = 0; i < _dialogs.size(); ++i) {
        TgObject dialog = _dialogs[i];

        if (dialog["photoId"] != photoId) {
            continue;
        }

        dialog["avatar"] = filePath;
        _dialogs[i] = dialog;

        emit dataChanged(index(i), index(i));
        break;
    }
}

void DialogsModel::refresh()
{
    resetState();
    // Before the request, not after: the point is to have something on screen
    // while the network is still being waited on.
    seedFromCache();
    fetchMoreDownwards();
}

void DialogsModel::updatesReset()
{
    // updates.differenceTooLong: the backlog was past the point where the
    // server will enumerate it, so the pipeline skipped ahead rather than
    // replaying. Everything in this list is from before that jump and nothing
    // will arrive to correct it one row at a time.
    kgInfo() << "Update sequence was reset, reloading the dialog list";

    refresh();
}

bool DialogsModel::inFolder(qint32 index, qint32 folderIndex)
{
    QMutexLocker lock(&_mutex);

    if (!_folders || index < 0 || folderIndex < 0)
        return true;

    return _dialogs[index]["folders"].toList().contains(folderIndex);
}

void DialogsModel::gotMessageUpdate(TgObject update, TgLongVariant messageId)
{
    QMutexLocker lock(&_mutex);

    if (!_offsets.isEmpty()) {
        return;
    }

    TgObject peerId;
    TgObject fromId;
    qint64 fromIdNumeric;

    //TODO this should be handled by singleton object with data from DB
    if (ID(update) == TLType::UpdateShortSentMessage) { //TODO what if we don't know about message?
        if (ID(update["peer_id"].toMap()) == 0) {
            return;
        }

        peerId = update["peer_id"].toMap();

        fromIdNumeric = _client->getUserId().toLongLong();
    } else if (update["user_id"].toLongLong()) {
        ID_PROPERTY(peerId) = TLType::PeerUser;
        peerId["user_id"] = update["user_id"];

        fromIdNumeric = update["out"].toBool() ? _client->getUserId().toLongLong() : update["user_id"].toLongLong();
    } else if (update["chat_id"].toLongLong()) {
        ID_PROPERTY(peerId) = TLType::PeerChat;
        peerId["chat_id"] = update["chat_id"];

        fromIdNumeric = update["from_id"].toLongLong();
    } else {
        return;
    }

    qint32 rowIndex = -1;
    for (qint32 i = 0; i < _dialogs.size(); ++i) {
        if (TgClient::peersEqual(_dialogs[i]["peer"].toMap(), peerId)) {
            rowIndex = i;
            break;
        }
    }

    if (rowIndex == -1) {
        return;
    }

    TgObject sender = peerCache().byId(fromIdNumeric);

    if (TgClient::isChannel(sender)) {
        ID_PROPERTY(fromId) = TLType::PeerChannel;
        fromId["channel_id"] = TgClient::getPeerId(sender);
    } else if (TgClient::isChat(sender)) {
        ID_PROPERTY(fromId) = TLType::PeerChat;
        fromId["chat_id"] = TgClient::getPeerId(sender);
    } else if (TgClient::isUser(sender)) {
        ID_PROPERTY(fromId) = TLType::PeerUser;
        fromId["user_id"] = TgClient::getPeerId(sender);
    }

    update["peer_id"] = peerId;
    update["from_id"] = fromId;

    // Empty lists: this update carried none, and resolvePeer inside falls
    // through to the cache for anything it needs to name.
    handleDialogMessage(_dialogs[rowIndex], update, sender, TgList(), TgList());
    emit dataChanged(index(rowIndex), index(rowIndex));

    prepareNotification(_dialogs[rowIndex]);

    if (_dialogs[rowIndex]["pinned"].toBool()) {
        return;
    }

    if (beginMoveRows(QModelIndex(), rowIndex, rowIndex, QModelIndex(), _lastPinnedIndex + 1)) {
        _dialogs.insert(_lastPinnedIndex + 1, _dialogs.takeAt(rowIndex));
        endMoveRows();
    }
}

void DialogsModel::gotUpdate(TgObject update, TgLongVariant messageId, TgList users, TgList chats, qint32 date, qint32 seq, qint32 seqStart)
{
    QMutexLocker lock(&_mutex);

    // Keyed by id, so a peer that arrives with every update is stored once
    // rather than appended again each time.
    peerCache().put(users, chats);

    switch (ID(update)) {
    case TLType::UpdateNewMessage:
    case TLType::UpdateNewChannelMessage:
    {
        if (!_offsets.isEmpty()) {
            return;
        }

        TgObject message = update["message"].toMap();

        qint32 rowIndex = -1;
        for (qint32 i = 0; i < _dialogs.size(); ++i) {
            if (TgClient::peersEqual(_dialogs[i]["peer"].toMap(), message["peer_id"].toMap())) {
                rowIndex = i;
                break;
            }
        }

        if (rowIndex == -1) {
            return;
        }

        TgObject fromId = message["from_id"].toMap();
        TgObject sender;
        if (TgClient::isUser(fromId)) for (qint32 j = 0; j < users.size(); ++j) {
            TgObject peer = users[j].toMap();
            if (TgClient::peersEqual(peer, fromId)) {
                sender = peer;
                break;
            }
        }
        if (TgClient::isChat(fromId)) for (qint32 j = 0; j < chats.size(); ++j) {
            TgObject peer = chats[j].toMap();
            if (TgClient::peersEqual(peer, fromId)) {
                sender = peer;
                break;
            }
        }
        if (TgClient::commonPeerType(fromId) == 0) {
            //This means that it is a channel feed or personal messages.
            //Authorized user is returned by API, so we don't need to put it manually.
            sender = _dialogs[rowIndex]["peer"].toMap();
        }

        message["out"] = TgClient::getPeerId(sender) == _client->getUserId();

        handleDialogMessage(_dialogs[rowIndex], message, sender, users, chats);
        emit dataChanged(index(rowIndex), index(rowIndex));

        prepareNotification(_dialogs[rowIndex]);

        if (_dialogs[rowIndex]["pinned"].toBool()) {
            return;
        }

        if (beginMoveRows(QModelIndex(), rowIndex, rowIndex, QModelIndex(), _lastPinnedIndex + 1)) {
            _dialogs.insert(_lastPinnedIndex + 1, _dialogs.takeAt(rowIndex));
            endMoveRows();
        }

        break;
    }
    }
}

void DialogsModel::prepareNotification(TgObject row)
{
    if (row["messageOut"].toBool())
        return;

    emit sendNotification(TgClient::getPeerId(row["peer"].toMap()).toLongLong(),
                          row["title"].toString(),
                          row["messageSenderName"].toString(),
                          row["messageText"].toString(),
                          row["silent"].toBool());
}
