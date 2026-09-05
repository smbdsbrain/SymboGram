#include "steps.h"

#include "tgclient.h"

CallStep::CallStep(const char *name, InvokeFn invoke, TgEvent::Kind expect,
                   VerifyFn verify, int timeoutMs)
    : _name(name), _invoke(invoke), _expect(expect), _verify(verify), _timeout(timeoutMs)
{
}

void CallStep::enter(TgClient *client, ScenarioCtx &ctx)
{
    if (_invoke) (*_invoke)(client, ctx);
}

Step::Result CallStep::on(const TgEvent &e, ScenarioCtx &ctx)
{
    if (e.kind == TgEvent::RpcError && _expect != TgEvent::RpcError) {
        // Let a verify function claim it: that is how a negative-path scenario
        // asserts a specific error rather than merely "did not succeed", and
        // how an external precondition becomes a SKIP rather than a failure.
        if (_verify) {
            const QString why = (*_verify)(e, ctx);
            if (why.isEmpty()) return Pass;
            if (why.startsWith("~")) { reason = why.mid(1); return Skip; }
        }
        reason = QString("RPC error %1 %2").arg(e.errCode).arg(e.errText);
        return Fail;
    }

    if (e.kind != _expect) return Pending;

    if (_verify) {
        const QString why = (*_verify)(e, ctx);
        if (why.startsWith("~")) { reason = why.mid(1); return Skip; }
        if (!why.isEmpty()) { reason = why; return Fail; }
    }
    return Pass;
}

WaitStep::WaitStep(const char *name, TgEvent::Kind expect, VerifyFn verify,
                   int timeoutMs)
    : _name(name), _expect(expect), _verify(verify), _timeout(timeoutMs)
{
}

Step::Result WaitStep::on(const TgEvent &e, ScenarioCtx &ctx)
{
    if (e.kind != _expect) return Pending;

    if (_verify) {
        const QString why = (*_verify)(e, ctx);
        // A wait step's verify may legitimately reject an event and keep
        // waiting -- several updates arrive before the interesting one. Only a
        // reason beginning with "!" is fatal.
        if (!why.isEmpty()) {
            if (why.startsWith("!")) { reason = why.mid(1); return Fail; }
            return Pending;
        }
    }
    return Pass;
}
