#include "runner.h"

#include <QRegExp>
#include <QTimerEvent>
#include <QTextStream>

#include "tgclient.h"

// A FLOOD_WAIT_n is not a defect and must not fail a step.
//
// TgTransport::handleRpcError parks the message in floodMessages and re-sends
// it from timerEvent -- which fires on the 60 s ping tick, not immediately. So
// a step that hits one needs n seconds plus a tick, and the +75 s below is that
// tick, not slack. Treating FLOOD_WAIT as a failure is the fastest way to get
// an e2e suite switched off, and treating it as success would hide real
// throttling.
static const int kFloodBudgetSeconds = 120;
static const int kFloodTickAllowance = 75000;

const char* tgEventName(TgEvent::Kind k)
{
    switch (k) {
    case TgEvent::None:           return "none";
    case TgEvent::Connected:      return "connected";
    case TgEvent::Disconnected:   return "disconnected";
    case TgEvent::Initialized:    return "initialized";
    case TgEvent::Authorized:     return "authorized";
    case TgEvent::TfaRequired:    return "tfaRequired";
    case TgEvent::RpcError:       return "rpcError";
    case TgEvent::SentCode:       return "auth.sentCode";
    case TgEvent::Authorization:  return "auth.authorization";
    case TgEvent::Dialogs:        return "messages.dialogs";
    case TgEvent::Messages:       return "messages.messages";
    case TgEvent::VectorUser:     return "Vector<User>";
    case TgEvent::VectorFilter:   return "Vector<DialogFilter>";
    case TgEvent::CountriesList:  return "help.countriesList";
    case TgEvent::BoolResp:       return "Bool";
    case TgEvent::Unknown:        return "unknownResponse";
    case TgEvent::Update:         return "update";
    case TgEvent::MessageUpdate:  return "messageUpdate";
    case TgEvent::FileUploaded:   return "fileUploaded";
    case TgEvent::FileDownloaded: return "fileDownloaded";
    case TgEvent::Timeout:        return "timeout";
    }
    return "?";
}

ScenarioRunner::ScenarioRunner(TgClient *client, Scenario *scenario, QObject *parent)
    : QObject(parent)
    , _client(client)
    , _scenario(scenario)
    , _ctx()
    , _index(0)
    , _failed(0)
    , _finished(false)
    , _skipped(false)
    , _inDeliver(false)
    , _timer()
    , _timeoutMs(0)
    , _tap()
    , _failure()
    , _floodExtraMs(0)
    , _skippedSteps(0)
{
    connect(_client, SIGNAL(connected(bool)),                   this, SLOT(onConnected(bool)));
    connect(_client, SIGNAL(disconnected(bool)),                this, SLOT(onDisconnected(bool)));
    connect(_client, SIGNAL(initialized(bool)),                 this, SLOT(onInitialized(bool)));
    connect(_client, SIGNAL(authorized(TgLongVariant)),         this, SLOT(onAuthorized(TgLongVariant)));
    connect(_client, SIGNAL(tfaRequired()),                     this, SLOT(onTfaRequired()));
    connect(_client, SIGNAL(rpcError(qint32,QString,TgLongVariant)),
            this, SLOT(onRpcError(qint32,QString,TgLongVariant)));
    connect(_client, SIGNAL(authSentCodeResponse(TgObject,TgLongVariant)),
            this, SLOT(onSentCode(TgObject,TgLongVariant)));
    connect(_client, SIGNAL(authAuthorizationResponse(TgObject,TgLongVariant)),
            this, SLOT(onAuthorization(TgObject,TgLongVariant)));
    connect(_client, SIGNAL(messagesDialogsResponse(TgObject,TgLongVariant)),
            this, SLOT(onDialogs(TgObject,TgLongVariant)));
    connect(_client, SIGNAL(messagesMessagesResponse(TgObject,TgLongVariant)),
            this, SLOT(onMessages(TgObject,TgLongVariant)));
    connect(_client, SIGNAL(vectorUserResponse(TgVector,TgLongVariant)),
            this, SLOT(onVectorUser(TgVector,TgLongVariant)));
    connect(_client, SIGNAL(vectorDialogFilterResponse(TgVector,TgLongVariant)),
            this, SLOT(onVectorFilter(TgVector,TgLongVariant)));
    connect(_client, SIGNAL(helpCountriesListResponse(TgObject,TgLongVariant)),
            this, SLOT(onCountriesList(TgObject,TgLongVariant)));
    connect(_client, SIGNAL(boolResponse(bool,TgLongVariant)),
            this, SLOT(onBoolResponse(bool,TgLongVariant)));
    connect(_client, SIGNAL(unknownResponse(qint32,QByteArray,TgLongVariant)),
            this, SLOT(onUnknown(qint32,QByteArray,TgLongVariant)));
    connect(_client, SIGNAL(gotUpdate(TgObject,TgLongVariant,TgList,TgList,qint32,qint32,qint32)),
            this, SLOT(onUpdate(TgObject,TgLongVariant,TgList,TgList,qint32,qint32,qint32)));
    connect(_client, SIGNAL(gotMessageUpdate(TgObject,TgLongVariant)),
            this, SLOT(onMessageUpdate(TgObject,TgLongVariant)));
    connect(_client, SIGNAL(fileUploaded(TgLongVariant,TgObject)),
            this, SLOT(onFileUploaded(TgLongVariant,TgObject)));
    connect(_client, SIGNAL(fileDownloaded(TgLongVariant,QString)),
            this, SLOT(onFileDownloaded(TgLongVariant,QString)));
}

void ScenarioRunner::start()
{
    if (_scenario->count() == 0) {
        _skipped = true;
        finishAll();
        return;
    }
    enterCurrent();
}

void ScenarioRunner::enterCurrent()
{
    Step *s = _scenario->at(_index);
    _floodExtraMs = 0;
    _timeoutMs = s->timeoutMs();
    _timer.start(_timeoutMs, this);
    s->enter(_client, _ctx);
}

void ScenarioRunner::finishStep(Step::Result r, const QString &reason)
{
    Step *s = _scenario->at(_index);
    const int n = _index + 1;
    const QString label = QString::fromLatin1(s->name());

    if (r == Step::Pass) {
        _tap.append(QString("ok %1 - %2").arg(n).arg(label));
    } else if (r == Step::Skip) {
        // TAP's own encoding for "did not run, and here is why".
        ++_skippedSteps;
        _tap.append(QString("ok %1 - %2 # SKIP %3").arg(n).arg(label).arg(reason));
    } else {
        ++_failed;
        _tap.append(QString("not ok %1 - %2 # %3").arg(n).arg(label).arg(reason));
        if (_failure.isEmpty()) _failure = reason;
    }

    _timer.stop();
    ++_index;

    // A failing or skipped step stops the scenario: later steps depend on it
    // and would report a cascade that names the wrong cause.
    if (r != Step::Pass || _index >= _scenario->count()) {
        finishAll();
        return;
    }
    enterCurrent();
}

void ScenarioRunner::finishAll()
{
    if (_finished) return;
    _finished = true;
    _timer.stop();
    emit done();
}

void ScenarioRunner::deliver(const TgEvent &e)
{
    if (_finished || _index >= _scenario->count()) return;

    // enterCurrent() can issue a request that completes synchronously enough to
    // re-enter here through the event loop. Guard rather than recurse.
    if (_inDeliver) return;
    _inDeliver = true;

    Step *s = _scenario->at(_index);
    const Step::Result r = s->on(e, _ctx);

    _inDeliver = false;

    if (r == Step::Pass || r == Step::Fail || r == Step::Skip)
        finishStep(r, s->reason);
}

void ScenarioRunner::timerEvent(QTimerEvent *event)
{
    if (event->timerId() != _timer.timerId()) {
        QObject::timerEvent(event);
        return;
    }
    _timer.stop();

    TgEvent e;
    e.kind = TgEvent::Timeout;
    deliver(e);

    if (!_finished && _index < _scenario->count()) {
        finishStep(Step::Fail,
                   QString("timed out after %1 ms").arg(_timeoutMs + _floodExtraMs));
    }
}

// --- adapters ---------------------------------------------------------------

void ScenarioRunner::onConnected(bool hasUserId)
{
    TgEvent e; e.kind = TgEvent::Connected; e.flag = hasUserId; deliver(e);
}

void ScenarioRunner::onDisconnected(bool hasUserId)
{
    TgEvent e; e.kind = TgEvent::Disconnected; e.flag = hasUserId; deliver(e);
}

void ScenarioRunner::onInitialized(bool hasUserId)
{
    TgEvent e; e.kind = TgEvent::Initialized; e.flag = hasUserId; deliver(e);
}

void ScenarioRunner::onAuthorized(TgLongVariant userId)
{
    TgEvent e; e.kind = TgEvent::Authorized; e.msgId = userId.toLongLong(); deliver(e);
}

void ScenarioRunner::onTfaRequired()
{
    TgEvent e; e.kind = TgEvent::TfaRequired; deliver(e);
}

void ScenarioRunner::onRpcError(qint32 code, QString message, TgLongVariant messageId)
{
    if (message.contains("FLOOD_WAIT_")) {
        QRegExp rx("FLOOD_WAIT_(\\d+)");
        const int seconds = rx.indexIn(message) >= 0 ? rx.cap(1).toInt() : 0;
        if (seconds > 0 && seconds <= kFloodBudgetSeconds) {
            // Give the step room for the wait plus the ping tick that re-sends
            // the parked message, and do NOT deliver the error to the step.
            _floodExtraMs += seconds * 1000 + kFloodTickAllowance;
            _timer.start(_timeoutMs + _floodExtraMs, this);
            return;
        }
        // Too long to wait out. Fall through: the step fails and says why, so
        // the run is legibly throttled rather than mysteriously slow.
    }

    TgEvent e;
    e.kind = TgEvent::RpcError;
    e.errCode = code;
    e.errText = message;
    e.msgId = messageId.toLongLong();
    deliver(e);
}

void ScenarioRunner::onSentCode(TgObject data, TgLongVariant messageId)
{
    TgEvent e; e.kind = TgEvent::SentCode; e.obj = data; e.msgId = messageId.toLongLong(); deliver(e);
}

void ScenarioRunner::onAuthorization(TgObject data, TgLongVariant messageId)
{
    TgEvent e; e.kind = TgEvent::Authorization; e.obj = data; e.msgId = messageId.toLongLong(); deliver(e);
}

void ScenarioRunner::onDialogs(TgObject data, TgLongVariant messageId)
{
    TgEvent e; e.kind = TgEvent::Dialogs; e.obj = data; e.msgId = messageId.toLongLong(); deliver(e);
}

void ScenarioRunner::onMessages(TgObject data, TgLongVariant messageId)
{
    TgEvent e; e.kind = TgEvent::Messages; e.obj = data; e.msgId = messageId.toLongLong(); deliver(e);
}

void ScenarioRunner::onVectorUser(TgVector data, TgLongVariant messageId)
{
    TgEvent e; e.kind = TgEvent::VectorUser; e.list = data; e.msgId = messageId.toLongLong(); deliver(e);
}

void ScenarioRunner::onVectorFilter(TgVector data, TgLongVariant messageId)
{
    TgEvent e; e.kind = TgEvent::VectorFilter; e.list = data; e.msgId = messageId.toLongLong(); deliver(e);
}

void ScenarioRunner::onCountriesList(TgObject data, TgLongVariant messageId)
{
    TgEvent e; e.kind = TgEvent::CountriesList; e.obj = data; e.msgId = messageId.toLongLong(); deliver(e);
}

void ScenarioRunner::onBoolResponse(bool response, TgLongVariant messageId)
{
    TgEvent e; e.kind = TgEvent::BoolResp; e.flag = response; e.msgId = messageId.toLongLong(); deliver(e);
}

void ScenarioRunner::onUnknown(qint32 conId, QByteArray data, TgLongVariant messageId)
{
    // Every one of these is a constructor the app received and could not route.
    // At a fresh layer it is the single most useful diagnostic there is, so it
    // goes into the TAP stream as a diagnostic line even when nothing fails.
    _tap.append(QString("# unknownResponse conId=0x%1 (%2 bytes) -- candidate vector")
                .arg(quint32(conId), 8, 16, QChar('0')).arg(data.size()));
    TgEvent e; e.kind = TgEvent::Unknown; e.errCode = conId; e.msgId = messageId.toLongLong(); deliver(e);
}

void ScenarioRunner::onUpdate(TgObject update, TgLongVariant messageId, TgList users,
                              TgList chats, qint32 date, qint32 seq, qint32 seqStart)
{
    (void) users; (void) chats; (void) date; (void) seq; (void) seqStart;
    TgEvent e; e.kind = TgEvent::Update; e.obj = update; e.msgId = messageId.toLongLong(); deliver(e);
}

void ScenarioRunner::onMessageUpdate(TgObject update, TgLongVariant messageId)
{
    TgEvent e; e.kind = TgEvent::MessageUpdate; e.obj = update; e.msgId = messageId.toLongLong(); deliver(e);
}

void ScenarioRunner::onFileUploaded(TgLongVariant fileId, TgObject inputFile)
{
    TgEvent e; e.kind = TgEvent::FileUploaded; e.obj = inputFile; e.msgId = fileId.toLongLong(); deliver(e);
}

void ScenarioRunner::onFileDownloaded(TgLongVariant fileId, QString filePath)
{
    TgEvent e; e.kind = TgEvent::FileDownloaded; e.errText = filePath; e.msgId = fileId.toLongLong(); deliver(e);
}

Scenario::~Scenario()
{
    for (int i = 0; i < _steps.size(); ++i) delete _steps.at(i);
}
