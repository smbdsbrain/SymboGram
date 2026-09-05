#ifndef E2E_STEPS_H
#define E2E_STEPS_H

#include "scenario.h"

// Three generic steps, so a scenario is a table rather than a class hierarchy.
//
// The behaviour that would live in a lambda lives in a free function pointer
// instead -- C++03 has no lambdas, and this is the cheapest substitute that
// keeps a scenario readable end to end.

typedef void (*InvokeFn)(TgClient *client, ScenarioCtx &ctx);
// Return value:
//   ""          accept the event; the step passes
//   "~reason"   skip the step -- an external precondition is not met
//   "!reason"   fatal for a WaitStep (an ordinary reason keeps it waiting)
//   "reason"    fail the step
typedef QString (*VerifyFn)(const TgEvent &e, ScenarioCtx &ctx);

// Issue a request, then wait for one kind of event and check it.
//
// RpcError always fails the step unless the scenario's verify handles it --
// which is how a negative-path scenario asserts that a request fails, and fails
// with the right error, rather than merely not succeeding.
class CallStep : public Step
{
public:
    CallStep(const char *name, InvokeFn invoke, TgEvent::Kind expect,
             VerifyFn verify = 0, int timeoutMs = 30000);

    const char* name() const { return _name; }
    void enter(TgClient *client, ScenarioCtx &ctx);
    Result on(const TgEvent &e, ScenarioCtx &ctx);
    int timeoutMs() const { return _timeout; }

private:
    const char    *_name;
    InvokeFn       _invoke;
    TgEvent::Kind  _expect;
    VerifyFn       _verify;
    int            _timeout;
};

// Wait for an event that some other actor causes -- a connection completing, or
// a message arriving because the Telethon oracle sent one.
class WaitStep : public Step
{
public:
    WaitStep(const char *name, TgEvent::Kind expect, VerifyFn verify = 0,
             int timeoutMs = 30000);

    const char* name() const { return _name; }
    Result on(const TgEvent &e, ScenarioCtx &ctx);
    int timeoutMs() const { return _timeout; }

private:
    const char    *_name;
    TgEvent::Kind  _expect;
    VerifyFn       _verify;
    int            _timeout;
};

#endif // E2E_STEPS_H
