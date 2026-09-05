#ifndef E2E_SCENARIO_H
#define E2E_SCENARIO_H

#include <QList>
#include <QString>

#include "event.h"

class TgClient;

// Values carried between steps.
//
// This is what replaces closure capture. Step 2 puts phone_code_hash here and
// step 3 reads it; without somewhere shared to put it, every Step needs its own
// members and the design collapses back into one class per scenario.
typedef TgObject ScenarioCtx;

class Step
{
public:
    // Skip is not a pass and not a failure. It exists because some
    // preconditions are external: Telegram's test data centres currently
    // reject the documented fixed login code, which is a fact about Telegram,
    // not a defect in this client -- verified by an independent Telethon
    // client getting the identical error. Reporting that as a failure trains
    // people to ignore red runs; reporting it as a pass hides it.
    enum Result { Pending, Pass, Fail, Skip };

    Step() : reason() {}
    virtual ~Step() {}

    virtual const char* name() const = 0;

    // Issue whatever this step does. Called once, when the step becomes
    // current. A step that only waits leaves this empty.
    virtual void enter(TgClient *client, ScenarioCtx &ctx) { (void) client; (void) ctx; }

    // Called for every event until it returns Pass or Fail.
    virtual Result on(const TgEvent &e, ScenarioCtx &ctx) = 0;

    virtual int timeoutMs() const { return 30000; }

    QString reason;
};

class Scenario
{
public:
    Scenario(const char *name) : _name(name), _steps() {}
    ~Scenario();

    // Takes ownership.
    void add(Step *s) { _steps.append(s); }

    const char* name() const { return _name; }
    int count() const { return _steps.size(); }
    Step* at(int i) const { return _steps.at(i); }

private:
    Scenario(const Scenario &);
    Scenario& operator=(const Scenario &);

    const char   *_name;
    QList<Step*>  _steps;
};

#endif // E2E_SCENARIO_H
