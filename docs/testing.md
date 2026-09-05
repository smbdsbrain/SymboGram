# Testing SymboGram

Three tiers, cheapest first. Each says what it covers and — more usefully —
what it does not, because the gaps here are large and easy to mistake for
coverage.

None of these tiers touch the Symbian device. Read [the last section](#what-no-tier-covers)
before treating a green run as evidence the app works.

## Why this exists at all

The generated TL readers in `libkg/tlschema.cpp` are a `switch` on the
constructor id with **no `default:` case**, and TL carries no length prefixes.
So an unrecognised constructor id does not raise an error and does not stop the
parse. It consumes its four bytes, returns an empty object, and leaves the
stream positioned mid-object — after which every remaining field in that packet
is decoded from the wrong offset.

Nothing about that is visible at runtime. It looks like a message that renders
oddly, or a chat list that comes back short. That is the failure mode a schema
change introduces, and it is why the codec tier exists.

## Tier 0 — local acceptance (no network, seconds)

```
pwsh -File tools\run-tlcodec.ps1
pwsh -File tools\gen-schema.ps1 -Check -Layer 229 -Api schema\api.tl
python tools\verify-vendored.py
pwsh -File tools\audit-public.ps1
```

`tools\run-tlcodec.ps1` builds `tests/tlcodec` and runs it. Output is TAP 13;
the exit code is the number of failures. It decodes each vector and asserts the
reader consumed **exactly** the buffer — `QDataStream::status() == Ok` *and*
`atEnd()` — then re-encodes and compares.

Two assertion strengths, and the difference matters:

- **`TlExact`** — the re-encoded bytes equal the input. The strong claim, and it
  is correct only for constructors with no flags word.
- **`TlStructural`** (default) — decode, re-encode, decode again, compare with
  `flags`/`flags2` stripped. Necessary because generated writers **recompute**
  the flags word from which keys are present, so a bit the server set for a
  field this layer does not know is silently dropped on the way back out. That
  is a genuine property of the codec, not a test artefact, and pretending
  otherwise would mean weakening the exact cases that can hold.

Four of the nine cases are **deliberate-failure controls** — truncated buffer,
trailing bytes, an unknown top-level id, and an unknown id nested inside a
`Vector`. They assert the checks report the *right* failure. A suite that
asserts nothing prints green lines too, and the controls are the only thing
that tells the two apart. Add one with every new assertion.

`gen-schema.ps1 -Check` is the reproduction gate: the committed
`libkg/tlschema.*` and `libkg/mtschema.*` must be byte for byte what the pinned
`schema/api.tl` generates at the layer `libkg/tlschema.h` announces. A dirty
result means someone hand-edited generated code, or the layer and the schema
have come apart — which is the one thing that must never happen, because a
reader built for one layer fed another layer's bytes misparses silently.

Pass the layer the tree is actually on. `-Check -Layer 166` was the one-time
provenance proof that the vendored generator faithfully reproduces upstream's
checked-in output; it is expected to fail now that the tree is at 229, and
re-running it means checking out the pre-bump commit first.

This tier needs no credentials — `tests/tlcodec` deliberately compiles only
`tgstream.cpp` plus the generated schema, not `libkg.pri`, which would pull in
`apisecrets.h`. It is therefore the one target that could run on a hosted CI
runner.

**Covers:** reader/writer symmetry, flag handling, `Vector<T>`, and the
unknown-constructor desync.
**Does not cover:** MTProto itself (auth key generation, DH, salts, containers,
encryption), any server behaviour, and any constructor no vector exercises. It
cannot tell you the layer is stale — only that what you recorded still parses.

## Tier 1 — test environment (Telegram test DCs)

```
pwsh -File tools\run-e2e.ps1 -Tier test
```

Telegram runs a separate test environment. Accounts there are disposable and
public: the phone number is `99966XYYYY` where `X` is the DC id and `YYYY` is
anything, and the login code is the DC id repeated five times (`22222` on DC 2).
Data is wiped periodically. Nothing private belongs there and nothing there is
private.

Test DC addresses: DC 1 `149.154.175.10:443`, DC 2 `149.154.167.40:443`,
DC 3 `149.154.175.117:443`. These are public and DC 2 is already the bootstrap
address in `libkg/tgtransport.cpp`.

`TgTransport` has carried a `testMode` flag since upstream. It switches the
bootstrap DC, namespaces the `QSettings` session group (`DcSession10002` rather
than `DcSession2`, so a test session cannot clobber a production one), and sets
`pq_inner_data.dc`. The Telegram **test** RSA public key is already in the
keychain at `tgtransport.cpp:602-638` alongside the production one, so the
handshake needs nothing added.

### Unattended login does not currently work

Telegram's documented rule — the login code for a test account is the DC id
repeated — **is rejected on all three test DCs today**. `auth.sendCode`
succeeds; `auth.signIn` returns `PHONE_CODE_INVALID` for both the five- and
six-digit forms.

This is not a SymboGram defect. An independent Telethon client, at a current
layer and with an entirely separate TL implementation, gets the identical error
against DCs 1, 2 and 3 — which is what `tools/e2e-oracle/` is for. Absent that
second opinion the natural reading would be "our `auth.signIn` serialisation
broke at 229"; it did not, and `auth.signIn` is byte-identical between layers
166 and 229.

So the harness reports that step as **`# SKIP`**, not as a failure. A skip is an
assertion that did not run; the runner counts them separately and says so,
because a skip quietly rendered as a pass is how a suite stops meaning anything.
Pass `-Code` to override if the rule starts working again.

The practical consequence: the test tier covers everything up to and including
`initConnection`, plus the negative paths. Anything needing an authenticated
session needs one supplied, exactly as the production tier does.

**Covers:** the real handshake (including the test RSA key), layer acceptance by
a real server, and error surfacing.
**Does not cover:** production backend behaviour — the test DCs run different
code with different limits — real media and CDN redirects, real `access_hash`
lifetimes, or the shape of real data (large dialog lists, channels, forums).

## Tier 2 — production

A dedicated account, never the developer's own, and never the session in
`secrets/session/`.

**Login is not automatable.** SMS has been unavailable to third-party `api_id`s
since 2023-02-18, which leaves `sentCodeTypeApp` — a code delivered to another
already-logged-in session. So this tier runs against a session file that already
exists and must report "no session, tier skipped" rather than failing when one
is absent.

**Copy the session file into a scratch directory and point `SYMBOGRAM_SESSION_DIR`
at the copy.** `TgTransport::handleRpcError` calls `resetSession()` on any 401,
which erases the stored auth key. Running this tier directly against a session
you care about will, on the first expired-session run, destroy it.

```
pwsh -File tools\run-e2e.ps1 -Tier prod -Scenario read
```

`run-e2e.ps1` **copies** the session into `build-desktop/` before running. Do not
work around that: `TgTransport::handleRpcError` calls `resetSession()` on any
401, so a run against the original file destroys the login on the first
expired-session run.

**Covers:** the real schema as the real server emits it — the only tier that can
discover that a layer no longer parses what production sends. A method's return
type is not part of any constructor definition, so a schema diff cannot show it
changing — `messages.getDialogFilters` went from `Vector<DialogFilter>` to a
boxed `messages.DialogFilters` at 229, and only a live server reveals that class
of change.
**Does not cover:** login, signup, 2FA, or anything needing a second real
account.

## What no tier covers

Worth reading as a list of things a green run says nothing about:

- **The device.** GCCE codegen, the `Q_OS_SYMBIAN` branch of
  `src/platformutils.cpp`, Pigler notifications, SIS packaging and
  capabilities, `EPOCHEAPSIZE` behaviour in a long chat, keypad navigation, the
  phone's TCP stack and `QNetworkSession` roaming. Device behaviour is
  authoritative wherever it and the desktop build disagree.
- **QML.** All 23 files, untouched by any tier.
- **`src/`.** `DialogsModel`, `MessagesModel`, `FoldersModel`,
  `AvatarDownloader`, `MessageUtil` — where most of SymboGram's own logic lives,
  as opposed to libkg's. The tiers test the library, not the application. This
  is the largest gap and the easiest to misread as coverage.
- **Long-running behaviour.** Ping and disconnect handling, salt rotation,
  `bad_server_salt` recovery, reconnection over hours.
- **The update pipeline**, because there isn't one: no `pts`/`qts`/`seq`
  tracking, no `getDifference`. Messages are missed when the connection blips,
  and no tier here will tell you so.

## Device smoke

After a layer change, before believing it:

1. `tools\build-symbian.cmd clean`, install the SIS from `dist\`.
2. Log in. Open the drawer — folder tabs must show **names**, not blanks
   (`dialogFilter.title` became `TextWithEntities` at layer 229; a blank tab is
   that regression).
3. Open a chat with history, scroll up to trigger a second `getHistory`.
4. Send a message; confirm it appears on another client.
5. Open a photo.

Anything that reads as garbled text or a short list is the desync described at
the top of this file, not a rendering bug.
