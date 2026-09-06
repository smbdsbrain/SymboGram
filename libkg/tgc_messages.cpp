#include "tgclient.h"

#include "tlschema.h"

TgLongVariant TgClient::messagesGetDialogs(qint32 offsetDate, qint32 offsetId, TgObject offsetPeer, qint32 limit, qint32 folderId, bool excludePinned, TgLongVariant hash)
{
    TGOBJECT(TLType::MessagesGetDialogsMethod, method);

    if (excludePinned) method["exclude_pinned"] = true;
    method["folder_id"] = folderId == 1 ? 1 : 0;
    method["offset_date"] = offsetDate;
    method["offset_id"] = offsetId;
    method["offset_peer"] = toInputPeer(offsetPeer);
    method["limit"] = limit;
    method["hash"] = hash;

    return sendObject<&writeTLMethodMessagesGetDialogs>(method);
}

TgLongVariant TgClient::messagesGetDialogsWithOffsets(TgObject offsets, qint32 limit, qint32 folderId, bool excludePinned, TgLongVariant hash)
{
    return messagesGetDialogs(offsets["offset_date"].toInt(), offsets["offset_id"].toInt(), offsets["offset_peer"].toMap(), limit, folderId, excludePinned, hash);
}

TgLongVariant TgClient::messagesGetHistory(TgObject inputPeer, qint32 offsetId, qint32 offsetDate, qint32 addOffset, qint32 limit, qint32 maxId, qint32 minId, TgLongVariant hash)
{
    TGOBJECT(TLType::MessagesGetHistoryMethod, method);

    method["peer"] = toInputPeer(inputPeer);
    method["offset_id"] = offsetId;
    method["offset_date"] = offsetDate;
    method["add_offset"] = addOffset;
    method["limit"] = limit;
    method["max_id"] = maxId;
    method["min_id"] = minId;
    method["hash"] = hash;

    return sendObject<&writeTLMethodMessagesGetHistory>(method);
}

TgLongVariant TgClient::messagesSendMessage(TgObject inputPeer, QString message, TgObject media, TgLongVariant randomId, qint32 replyToMsgId)
{
    TGOBJECT(GETID(media) != 0 ? TLType::MessagesSendMediaMethod : TLType::MessagesSendMessageMethod, method);

    method["peer"] = toInputPeer(inputPeer);
    method["message"] = message;
    method["media"] = media;
    method["random_id"] = randomId;

    // Inserted only when there is one. The generated writer recomputes flags
    // from which keys are present, and an empty map counts as present: it
    // would set the reply_to bit and then write nothing for it, because the
    // inner switch matches no constructor. That is a truncated request, and
    // the server reads the rest of the packet as something else.
    if (replyToMsgId != 0) {
        TGOBJECT(TLType::InputReplyToMessage, replyTo);
        replyTo["reply_to_msg_id"] = replyToMsgId;
        method["reply_to"] = replyTo;
    }

    if (GETID(media) != 0) {
        return sendObject<&writeTLMethodMessagesSendMedia>(method);
    } else {
        return sendObject<&writeTLMethodMessagesSendMessage>(method);
    }
}

TgLongVariant TgClient::messagesSendMedia(TgObject inputPeer, TgObject media, QString message, TgLongVariant randomId)
{
    return messagesSendMessage(inputPeer, message, media, randomId);
}

TgLongVariant TgClient::messagesGetDialogFilters()
{
    TGOBJECT(TLType::MessagesGetDialogFiltersMethod, method);

    return sendObject<&writeTLMethodMessagesGetDialogFilters>(method);
}

TgLongVariant TgClient::messagesDeleteMessages(TgVector ids, bool revoke)
{
    TGOBJECT(TLType::MessagesDeleteMessagesMethod, method);

    // No peer: this method is account-wide and addresses messages by id alone,
    // which is why channels need one of their own.
    if (revoke) method["revoke"] = true;
    method["id"] = ids;

    return sendObject<&writeTLMethodMessagesDeleteMessages>(method);
}

TgLongVariant TgClient::messagesReadHistory(TgObject inputPeer, qint32 maxId)
{
    TGOBJECT(TLType::MessagesReadHistoryMethod, method);

    method["peer"] = toInputPeer(inputPeer);
    method["max_id"] = maxId;

    return sendObject<&writeTLMethodMessagesReadHistory>(method);
}

TgLongVariant TgClient::messagesEditMessage(TgObject inputPeer, qint32 messageId, QString message)
{
    TGOBJECT(TLType::MessagesEditMessageMethod, method);

    method["peer"] = toInputPeer(inputPeer);
    method["id"] = messageId;
    method["message"] = message;

    return sendObject<&writeTLMethodMessagesEditMessage>(method);
}
