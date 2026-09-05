#include "tgclient.h"

#include "tlschema.h"

TgLongVariant TgClient::updatesGetState()
{
    TGOBJECT(TLType::UpdatesGetStateMethod, method);

    return sendObject<&writeTLMethodUpdatesGetState>(method);
}

TgLongVariant TgClient::updatesGetDifference(qint32 pts, qint32 date, qint32 qts)
{
    TGOBJECT(TLType::UpdatesGetDifferenceMethod, method);

    method["pts"] = pts;
    // A zero date or qts is rejected. 1 means "from the beginning", which the
    // server answers with differenceTooLong -- an outcome the pipeline already
    // knows how to handle, unlike an error it does not expect.
    method["date"] = date ? date : 1;
    method["qts"] = qts ? qts : 1;
    // pts_limit, pts_total_limit and qts_limit are flag fields. Left unset:
    // the generated writer builds the flags word from which keys are present,
    // so assigning 0 would set the bit and ask for a limit of nothing.

    return sendObject<&writeTLMethodUpdatesGetDifference>(method);
}

TgLongVariant TgClient::updatesGetChannelDifference(TgObject inputChannel, qint32 pts,
                                                    qint32 limit, bool force)
{
    TGOBJECT(TLType::UpdatesGetChannelDifferenceMethod, method);

    if (force) method["force"] = true;
    method["channel"] = inputChannel;

    TGOBJECT(TLType::ChannelMessagesFilterEmpty, filter);
    method["filter"] = filter;

    method["pts"] = pts;
    method["limit"] = limit;

    return sendObject<&writeTLMethodUpdatesGetChannelDifference>(method);
}
