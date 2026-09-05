#ifndef E2E_SCENARIOS_H
#define E2E_SCENARIOS_H

#include "scenario.h"

// Scenarios are C++, not a data file.
//
// A data-driven format would have to express "take phone_code_hash out of the
// response to step 2 and put it into the request in step 3". That is variable
// binding, and its C++03 interpreter would be larger than every scenario put
// together. Scenarios change when TgClient's API changes, so they should
// compile against it and break loudly when it moves.
//
// What IS data: the phone numbers, peers and message text, which arrive from
// argv and secrets/e2e.ini. A test-DC number in a source file is harmless; a
// production one is not, and the difference is one typo wide.

// Handshake, initConnection, help.getConfig. The only scenario that needs no
// account at all -- if this fails, nothing else is worth reading.
Scenario* makeConnectScenario();

// Full login on the test environment: auth.sendCode -> auth.signIn with the
// predictable code. Test DCs only; production cannot do this (see docs).
Scenario* makeLoginScenario();

// Read paths against whatever account the session already holds.
Scenario* makeReadScenario();

// Send a message to Saved Messages and read it back through getHistory.
// Deliberately self-directed: never write to another person's chat from a test.
Scenario* makeSendScenario();

// auth.sendCode with a malformed number. Asserts the client surfaces
// PHONE_NUMBER_INVALID rather than hanging -- the failure mode that matters.
Scenario* makeNegativeScenario();

#endif // E2E_SCENARIOS_H
