# The update pipeline

How SymboGram stays in step with Telegram, and what it does when it falls out.

Telegram does not resend what it pushed while a client was away. Everything
below exists because of that one fact: a client that only reacts to what
arrives is correct exactly as long as nothing goes wrong with the connection,
and silently wrong afterwards, with nothing in the interface to say so.

## The sequences

Telegram numbers its updates so a client can tell whether it has seen
everything. There are several counters, and they are independent:

| | |
|---|---|
| `pts` | The common message sequence: private chats and small groups. |
| `pts` per channel | Each channel counts separately, from its own base. |
| `qts` | Secret chats. Tracked because `getDifference` requires it; nothing here reads the updates it covers. |
| `seq` | Orders whole `updates` containers, not the updates inside them. |
| `date` | The server's clock, needed to ask for a difference. |

An update that belongs to a sequence carries the `pts` the sequence reaches
**after** it is applied, and `pts_count`, how much it advances by. So the
question for every update is whether `local + pts_count == pts`.

Three answers, in `TgUpdatesState::check`:

- **equal** — this is the next one. Apply it and advance.
- **`pts` at or below the local value** — already applied. Drop it.
- **anything else** — something in front of it is missing.

The order of the first two tests matters more than it looks. An update with
`pts_count` 0 does not advance the sequence, so `local + 0 == pts` is a
legitimate apply; testing "already applied" first swallows every read receipt
and every edit.

A channel never polled has no local `pts`, so there is nothing for it to be
behind: it adopts what the server sends. Requesting a difference instead would
mean a round trip per channel the account has ever been in before a single
message could be shown.

## Closing a gap

A missing update is not an error and is not rare — it is what a dropped
connection looks like from the inside. The update that revealed the gap is
parked, and `updates.getDifference` (or `updates.getChannelDifference` for a
channel) asks for everything between the local position and the server's.

The parked queue is bounded and expires. Both limits exist for the same
reason: a gap that never closes must not be able to pin memory or wedge the
pipeline. Past the size limit the queue is dropped whole rather than trimmed —
what it holds is a run waiting on one missing update, and half a run is worth
nothing — and a difference refetches all of it. Past the timeout the same
thing happens.

Messages recovered from a difference are wrapped in `updateNewMessage` and
`updateNewChannelMessage` and emitted on the same signal as a pushed update.
Nothing about a recovered message differs once it reaches a model, and a
separate path would mean a second branch in every consumer.

## When the server gives up

Three constructors mean the backlog is past the point where the server will
enumerate it:

- `updatesTooLong` — fetch the difference.
- `updates.differenceTooLong` — the common sequence jumps to a new `pts`.
  Nothing arrives to correct what is on screen, so `updatesReset()` tells the
  models to reload.
- `updates.channelDifferenceTooLong` — the same for one channel, via
  `channelReset(channelId)`. The messages it carries are a fresh window, not a
  sequence of events, so they are not replayed as new-message updates: that
  would duplicate whatever the model already holds.

## Where the state lives

Beside the session, in the same ini as the auth key, under `UpdateState` and
`ChannelPts`. Not in the SQLite cache, deliberately: the pipeline has to work
on a device whose Qt has no SQLite driver, and a few kilobytes of per-channel
`pts` fit in an ini.

It is written at most every 30 seconds, and on disconnect. A write per applied
update would be a flash write per message; losing up to 30 seconds of position
costs exactly one extra `getDifference` at the next start, which is the thing
the pipeline is for.

## Replies from a previous run

The session id outlives the process, and MTProto re-delivers any reply the
client never acknowledged. So a difference answered minutes ago by a previous
run of the app arrives seconds after the next connect, carrying a state from
before everything that has happened since.

These are recognised and ignored. Applying one would rewind `pts` to where the
sequence was then, and everything after it would be refetched or replayed.

## What is not here

- **Secret chats.** `qts` is tracked because `getDifference` needs it. Nothing
  reads `updateNewEncryptedMessage`, and secret chats are out of scope.
- **A limit on how far behind is too far.** `getDifference` is asked for
  everything since the stored position, however old. The server answers a very
  stale request with `differenceTooLong`, which is handled, so this costs a
  round trip rather than correctness.
- **Ordering between channels.** Each channel's sequence is independent and no
  attempt is made to interleave them by date.
