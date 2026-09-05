#ifndef E2E_RUNNER_H
#define E2E_RUNNER_H

#include <QObject>
#include <QBasicTimer>
#include <QList>
#include <QStringList>

#include "scenario.h"

class TgClient;

// Drives one scenario to completion and reports TAP.
//
// The block of adapter slots below is unavoidable boilerplate in Qt 4 without
// lambdas -- but it is written ONCE here, rather than once per scenario, which
// is the whole reason the event funnel exists.
class ScenarioRunner : public QObject
{
    Q_OBJECT
public:
    ScenarioRunner(TgClient *client, Scenario *scenario, QObject *parent = 0);

    void start();

    // Seed a value the scenario's steps will read. This is how phone numbers
    // and message text reach a scenario without being compiled into it.
    void seed(const QString &key, const QVariant &value) { _ctx[key] = value; }

    bool passed() const { return _failed == 0 && _finished; }
    int  skippedSteps() const { return _skippedSteps; }
    int  stepsRun() const { return _index; }
    const QStringList& tap() const { return _tap; }
    const QString& failure() const { return _failure; }
    bool skipped() const { return _skipped; }

signals:
    void done();

private slots:
    void onConnected(bool hasUserId);
    void onDisconnected(bool hasUserId);
    void onInitialized(bool hasUserId);
    void onAuthorized(TgLongVariant userId);
    void onTfaRequired();
    void onRpcError(qint32 code, QString message, TgLongVariant messageId);
    void onSentCode(TgObject data, TgLongVariant messageId);
    void onAuthorization(TgObject data, TgLongVariant messageId);
    void onDialogs(TgObject data, TgLongVariant messageId);
    void onMessages(TgObject data, TgLongVariant messageId);
    void onVectorUser(TgVector data, TgLongVariant messageId);
    void onVectorFilter(TgVector data, TgLongVariant messageId);
    void onCountriesList(TgObject data, TgLongVariant messageId);
    void onBoolResponse(bool response, TgLongVariant messageId);
    void onUnknown(qint32 conId, QByteArray data, TgLongVariant messageId);
    void onUpdate(TgObject update, TgLongVariant messageId, TgList users,
                  TgList chats, qint32 date, qint32 seq, qint32 seqStart);
    void onMessageUpdate(TgObject update, TgLongVariant messageId);
    void onFileUploaded(TgLongVariant fileId, TgObject inputFile);
    void onFileDownloaded(TgLongVariant fileId, QString filePath);

protected:
    void timerEvent(QTimerEvent *event);

private:
    void deliver(const TgEvent &e);
    void enterCurrent();
    void finishStep(Step::Result r, const QString &reason);
    void finishAll();

    TgClient    *_client;
    Scenario    *_scenario;
    ScenarioCtx  _ctx;
    int          _index;
    int          _failed;
    bool         _finished;
    bool         _skipped;
    bool         _inDeliver;
    QBasicTimer  _timer;
    int          _timeoutMs;
    QStringList  _tap;
    QString      _failure;
    // Set when a FLOOD_WAIT parks the current request. TgTransport re-sends it
    // on its next ping tick, so the step has to be given room rather than
    // failed.
    int          _floodExtraMs;
    int          _skippedSteps;
};

#endif // E2E_RUNNER_H
