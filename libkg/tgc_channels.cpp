#include "tgclient.h"

#include "tlschema.h"

// channels.* takes an InputChannel rather than an InputPeer, and the two
// methods here exist because their messages.* counterparts do not reach a
// channel: messages.deleteMessages addresses messages by id across the
// account, and messages.readHistory takes a peer that a channel is not.

TgLongVariant TgClient::channelsDeleteMessages(TgObject channel, TgVector ids)
{
    TGOBJECT(TLType::ChannelsDeleteMessagesMethod, method);

    method["channel"] = toInputChannel(channel);
    method["id"] = ids;

    return sendObject<&writeTLMethodChannelsDeleteMessages>(method);
}

TgLongVariant TgClient::channelsReadHistory(TgObject channel, qint32 maxId)
{
    TGOBJECT(TLType::ChannelsReadHistoryMethod, method);

    method["channel"] = toInputChannel(channel);
    method["max_id"] = maxId;

    return sendObject<&writeTLMethodChannelsReadHistory>(method);
}
