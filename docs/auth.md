# Signing in

How SymboGram gets from a phone number to an authorization, and what the
two-step-verification path does on the way.

This page exists for one reason: the SRP proof is a sequence of hashes whose
**order** is the whole correctness argument, and the code is one function full
of `mbedtls_mpi` calls. Reading it back tells you what it computes, not whether
that is what Telegram expects. The construction is written out below and pinned
by the known-answer vector in `tests/crypto/main.cpp`.

## The flow

```
auth.sendCode      -> auth.sentCode, carrying phone_code_hash
auth.signIn        -> auth.Authorization              (no cloud password)
                   -> 401 SESSION_PASSWORD_NEEDED     (cloud password set)
account.getPassword -> account.password
auth.checkPassword -> auth.Authorization
```

`TgTransport::handleRpcError` intercepts `SESSION_PASSWORD_NEEDED` above its
own `errorCode == 401` branch. That ordering matters: the error *is* a 401, and
the branch below it calls `resetSession()`, which erases the auth key the
handshake just established.

`auth.checkPassword` returns `auth.Authorization`, the same constructor
`auth.signIn` returns on an account without a password, so the success path
needs nothing of its own — `TgTransport::handleAuthorization` already stores the
user id and saves the session.

## Where the flow is driven from

`TgClient`, not QML. The interface calls `authCheckPasswordSRP(password)` and
supplies nothing else; the client fetches the parameters, runs the proof and
sends the request.

That is not an arbitrary split. `srp_id` expires server-side, and a proof built
against a stale one is refused with `SRP_ID_INVALID` — which is indistinguishable
from a wrong password to anyone reading the screen. Driving the flow from one
place makes "every attempt proves against freshly fetched parameters" a property
of the code rather than a discipline the interface has to remember, and reduces
recovery from `SRP_ID_INVALID` to fetching again.

The cost is that the password is held across one network round trip. That is
unavoidable: SRP needs the password *after* the parameters arrive.

The password page therefore issues two `account.getPassword` calls, and they are
not redundant. The first, on arrival, is for the hint. The second is the proof's
own, at submit time.

## The construction

`H` is SHA-256. `|` is concatenation. Every integer that enters a hash is
written **big-endian, zero-left-padded to exactly 256 bytes** — including `g`,
so `pad(g)` for `g = 3` is 255 zero bytes followed by `0x03`, and `H(pad(g))` is
not `H("\x03")`.

```
SH(data, salt) = H(salt | data | salt)          -- salt on BOTH sides

x  = SH(PBKDF2-HMAC-SHA512(SH(SH(password, salt1), salt2), salt1, 100000, 64), salt2)
k  = H(pad(p) | pad(g))
v  = g^x mod p
A  = g^a mod p                                  -- a is 256 random bytes
u  = H(pad(A) | pad(B))                         -- A first, then B
t  = B - k*v ;  if t < 0 then t += p
S  = t^(a + u*x) mod p
M1 = H( (H(pad(p)) XOR H(pad(g))) | H(salt1) | H(salt2) | pad(A) | pad(B) | H(pad(S)) )
```

`inputCheckPasswordSRP` carries `srp_id`, `A` and `M1`.

The details that are easy to get wrong, and that produce a perfectly
well-formed proof when wrong:

- **`salt1` is the PBKDF2 salt.** `salt2` appears only in the two `SH`
  wrappers. The shape invites swapping them.
- **`H(pad(p)) XOR H(pad(g))` is a 32-byte XOR of two digests**, not
  `H(p XOR g)`.
- **The exponent `a + u*x` is not reduced** mod `p-1`. Reducing it would be
  mathematically equivalent and is not what the proof has to match.
- **The correction on `t` is explicit.** `mbedtls_mpi_sub_mpi` is signed, and
  relying on `mbedtls_mpi_exp_mod`'s treatment of a negative base would be
  relying on behaviour its contract does not state.
- **`A` goes on the wire padded to 256 bytes.** A shorter encoding is a
  different message and a different `M1`.

## What is checked before the proof

A server that supplies a bad prime, a generator that does not generate the large
prime-order subgroup, or a degenerate `B` can put the shared secret in a set
small enough to enumerate. Accepting whatever arrives makes the whole exchange
theatre in a client whose premise is that it is not the official one.

`srpCheckParams` requires `p` to be exactly 2048 bits, `g` to satisfy the
residue condition for its value, and `B` to lie within `[2^1984, p - 2^1984]`.
Our own `A` is held to the same range rule, and a draw outside it is redrawn
rather than reported: the parameters are fine, that particular secret is not.

`p` is compared against the prime Telegram serves, compiled in. A byte-exact
match is what licenses skipping the primality test, because proving a 2048-bit
number prime — twice, for `p` and `(p-1)/2` — is tens of modular
exponentiations, and each one is seconds on the ARM11 this targets.

When the prime is not the known one, the Miller-Rabin path runs anyway. It is
slow, and it is kept because both alternatives are worse: trusting an unverified
prime, or refusing to sign in the day Telegram rotates it.

## Why the computation is on a thread

100000 PBKDF2-HMAC-SHA512 iterations plus three 2048-bit modular
exponentiations. On the target hardware that is seconds, and a UI thread that
stops for that long is painted as unresponsive by the window server.

Chunking across timer ticks does not work here: the exponentiations are atomic
inside mbedtls, so the interface would stay responsive through the derivation
and then freeze for the part the user can least interpret.

Nothing else in this codebase is threaded, and `tgtransport.cpp` carries
`//TODO: lock` markers where message-id generation would need protection if
anything moved. So the boundary is deliberately the narrowest one that helps,
and **it should not be widened**:

- The worker reads no `TgClient` state, holds no socket, touches no store and no
  model, and never generates a message id.
- Its inputs are value types, assigned before `start()`. `QThread::start()` is
  the happens-before edge, which is why there is no mutex — a mutex would imply
  the fields may be written while the thread runs, and they must not be.
- Its outputs are `qint64` and `QByteArray` through queued signals. **The GUI
  thread sends the request.**
- It polls a cancel flag between PBKDF2 blocks, so backing out of the page stops
  the work rather than waiting it out.

## Errors worth handling by name

| Error | Meaning | Response |
|---|---|---|
| `PASSWORD_HASH_INVALID` | wrong password | say so, stay on the page |
| `SRP_ID_INVALID` | parameters went stale between fetch and proof | refetch and prove again, once, without telling the user their password is wrong |
| `SRP_PASSWORD_CHANGED` | the password changed under the attempt | refresh the hint; do not retry, the typed password is now known to be wrong |
| `FLOOD_WAIT_x` | too many attempts | report the delay as a sentence |

`auth.checkPassword` is excluded from the transport's `FLOOD_WAIT` replay.
Replay suits a request whose meaning does not expire; a proof is bound to one
`srp_id`, so resending it after the wait fails and spends another attempt
against the same limit.

## What is not here

No QR sign-in (`auth.exportLoginToken`), no future auth tokens, and no password
recovery (`auth.requestPasswordRecovery`) — an account that has lost its
password cannot be recovered from this client.

Signing up is not implemented either. Note that SMS delivery has been
unavailable to third-party `api_id`s since 2023-02-18, which leaves
`sentCodeTypeApp` — a code delivered to another already-logged-in session — as
the practical route to a first sign-in.
