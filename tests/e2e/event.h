#ifndef E2E_EVENT_H
#define E2E_EVENT_H

// Every TgClient signal, collapsed into one value type.
//
// TgClient exposes ~18 signals with six different argument lists. A scenario
// that connected to each of them directly would need a slot per signal per
// scenario -- and C++03 has no lambdas, so "per scenario" means a QObject
// subclass with eighteen two-line methods, written again for every scenario.
//
// Funnelling them into a single queue moves sequencing out of the type system
// and into data, which is what makes a scenario a readable table instead of a
// state machine. The adapter slots that do the funnelling are written once, in
// ScenarioRunner.

#include "tgstream.h"

struct TgEvent {
    enum Kind {
        None = 0,
        Connected,
        Disconnected,
        Initialized,
        Authorized,
        TfaRequired,
        RpcError,
        SentCode,
        Authorization,
        Dialogs,
        Messages,
        VectorUser,
        VectorFilter,
        CountriesList,
        BoolResp,
        Unknown,
        Update,
        MessageUpdate,
        UpdatesState,
        UpdatesReset,
        FileUploaded,
        FileDownloaded,
        Timeout
    };

    TgEvent()
        : kind(None), obj(), list(), msgId(0), errCode(0), errText(), flag(false) {}

    Kind      kind;
    TgObject  obj;
    TgVector  list;
    qint64    msgId;
    qint32    errCode;
    QString   errText;
    bool      flag;    // the bool carried by connected/disconnected/initialized
};

const char* tgEventName(TgEvent::Kind k);

#endif // E2E_EVENT_H
