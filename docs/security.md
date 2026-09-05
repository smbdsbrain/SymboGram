# Keeping secrets out of a public repository

SymboGram is developed in the open, largely with AI coding agents, against a
public GitHub repository. Everything below exists because those three facts
together mean a mistake is published the instant it is made.

Nothing sensitive has ever been committed here. `git log --all --diff-filter=A`
over the full history turns up only placeholder templates. The point of this
document is to keep that true.

## What must never reach the repository

| | Why it matters |
|---|---|
| `secrets/telegram.yaml` — `api_id`, `api_hash` | Not cryptographic secrets: every third-party client necessarily ships them and they can be read out of any binary. Still tied to one Telegram account, and a leaked pair invites the abuse that gets that account's API access revoked. |
| `secrets/session/**/SymboGram_user_session.ini` | `AuthKey`, `AuthKeyId`, `ServerSalt`, `SessionId`. Full account takeover, stored unencrypted. The highest-value file on the machine. |
| `secrets/session/**/SymboGram_avatars/` | Avatars of real contacts. Other people's data, not the developer's to risk. |
| `secrets/session/**/SymboGram_cache.ini`, `symbogram.log` | Dialog metadata, user ids, chat titles. |
| `secrets/symbogram.key` | Unencrypted RSA private key. Whoever holds it can sign a package as SymboGram. |
| `libkg/apisecrets.h` | Generated from the above. |
| Anything built locally | See "Build artifacts" below. |
| `CLAUDE.md`, `AGENTS.md`, `.claude/`, `local/` | Working notes and agent instructions. Not secret, but not the project's public face either. |

## The four layers

Each catches what the one before it misses. None is sufficient alone, and the
document is explicit about where each one stops.

### 1. `.gitignore`

Covers everything in the table above. Two properties worth keeping:

- **Belt and braces.** The session file is matched three separate times
  (`/secrets/`, `/secrets/session/`, `**/SymboGram_user_session.ini`), and the
  signing key twice (`/secrets/`, `*.key`). A single edit cannot expose it.
- **Nothing tracked is shadowed.** An ignore rule that matches a file already in
  the repository is a trap: the file stays, but future changes to it silently
  vanish. Checked by the audit, not by eye — adding `*.map` here immediately
  shadowed the tracked upstream `libkg/zlib/zlib.map`.

`git add -f` walks straight past all of it. That is what the next layer is for.

### 2. `tools/audit-public.ps1`

Audits **the whole would-be commit set** — tracked files *plus* untracked files
that are not ignored — so it sees what a bulk `git add` would sweep in, not
just what is already staged.

It reports `category :: path`. **It never prints what it matched**, so the audit
output is safe to paste anywhere.

```powershell
pwsh -File tools/audit-public.ps1                       # what would be published
pwsh -File tools/audit-public.ps1 -Mode Staged          # what is staged
pwsh -File tools/audit-public.ps1 -Mode Range -Base A -Tip B
```

Checks, and what each is really for:

| | Check | Notes |
|---|---|---|
| a | Path under `secrets/`, `local/`, `private/`, `dist/`, `build-desktop*/`, `Symbian1Qt473/`, `obj/`, `moc/`, `ui/`, `rcc/`, `docs/local/`, `notes/` | |
| b | Any binary artifact extension, **anywhere** | How "no locally built binary is published" is enforced. Two allow-list entries, both upstream files whose extension lies. |
| c | Contact media by filename | |
| d | Contact media by **content hash** | Catches an avatar copied and renamed into `img/`. Avatars are hashed, never text-scanned. |
| e | Credential-shaped patterns | Skips vendored trees: mbedtls ships literal PEM headers in `library/pkparse.c`, and an audit that fails against upstream source on its first run gets switched off. |
| f | Machine-specific home paths | `C:\Qt\4.8.7` and `C:\mingw482` in the build scripts are intentional and are not matched. |
| g | **Exact match against the real local secret values** | The strongest check. |
| h | `secrets/` is genuinely ignored, asked of git rather than read off `.gitignore` | |
| i | No ignore rule shadows a tracked file | |
| j | Force-push orphans in the remote-tracking reflog | Warning only. |
| k | **Hex-encoded content, decoded and re-scanned** through (e) and (g) | Hex defeats a substring search completely. `tests/vectors/` holds recorded MTProto packets, so this closes a hole that directory opens. |
| l | Every file under `tests/vectors/` declares `# source:`, and only `test-dc` or `synthetic` is accepted | No pattern can tell a real chat title from a fake one. Provenance is the only enforceable rule. |

**Checks (k) and (l) exist because of recorded TL packets.** A captured packet
is stored as hex text, and an `api_hash` inside one shares no substring with the
value (g) is looking for — so (g) sails straight past it. Check (k) decodes
every hex run of 16 characters or more and re-runs (e) and (g) over the result.
It is not restricted to `tests/vectors/`: a packet pasted into a Markdown file
or a C++ string literal leaks just as well.

The 16-character floor is 8 bytes, and it is load-bearing. Every value the
differential check harvests must expand above it or (k) cannot see the value at
all; the shortest today is an 8-digit `api_id`, expanding to exactly 16. The
first version of this used a 32-character floor and silently missed both the
`api_id` and the `UserId` — caught only because `tools/test-audit.ps1` tests
**every** harvested value rather than one of them.

What (k) cannot do is recognise somebody's chat title, because a real one looks
like any other text. That is what (l) is for: a vector must say where its bytes
came from, and only Telegram's test environment counts. Test-DC accounts are
handed out to anyone (`99966XYYYY`, code = the DC id five times) and wiped
periodically, so a packet from one is genuinely publishable. A packet from
production never is, and a vector with no header is exactly what an accidental
production capture looks like — so a missing header is a failure, not a warning.

**Check (g) is the one that matters most.** Rather than guessing at credential
shapes, it reads the actual values out of `secrets/` and looks for those exact
strings in what is about to be published. A secret in any encoding, any file
type, any context is caught; a regex would need to anticipate the shape.

It harvests `api_id` and `api_hash`; the session's `AuthKey` (both the stored
`@ByteArray(...)` form and the payload inside it), `AuthKeyId`, `ServerSalt`,
`SessionId`, `UserId`; and the base64 body of the signing key. It deliberately
does **not** harvest the session's `CurrentHost`, `CurrentPort`, `MainDc` and
similar: those are public Telegram protocol state, and `CurrentHost` appears
verbatim in `libkg/tgtransport.cpp` — harvesting it makes the audit fail
against the project's own source, and the natural reaction to that is to turn
the check off.

### 3. Git hooks

```powershell
pwsh -File tools/setup-hooks.ps1     # once per clone
```

`pre-commit` audits the staged set and refuses a commit whose author address is
not a GitHub no-reply address.

`pre-push` is the important one:

1. audits the working tree;
2. audits **every blob introduced by the commits being pushed**, read out of
   git's object store;
3. refuses a non-fast-forward push unless `SYMBOGRAM_ALLOW_FORCE=1`.

Step 2 reads objects rather than files on purpose. A secret added in one commit
and deleted in the next is in neither the net diff nor the working tree, so a
working-tree audit passes while the secret sits in the history about to be
pushed. That is the gap that makes a CI-only audit — which runs *after* the
push — too late to help.

### 4. CI (`.github/workflows/audit.yml`)

Runs the audit, parse-checks every PowerShell script, and runs gitleaks over
the full history.

**CI runs a weaker audit than your machine does.** A runner has no `secrets/`,
so checks (d) and (g) do nothing there. The audit says so in yellow and the
workflow prints it again at the end. A green badge does not mean the
differential check passed; it means it did not run.

## What this does not cover

Stated plainly, because a control you believe in wrongly is worse than one you
know the limits of.

- **`git commit --no-verify` and `git push --no-verify` skip the hooks.** Git
  offers no way to prevent that, and a second hook under a different name is
  skipped too. For agent-driven work `tools/agent-guard.ps1` blocks it. For a
  human at the terminal it is a deliberate act by the person who owns the risk.
- **`core.hooksPath` is local config and does not survive `git clone`.** The
  build scripts re-arm it, so a fresh clone that builds anything gets covered,
  but a clone that only reads is not.
- **The agent guard matches command text.** It stops accidents, not an
  adversary: flag reordering, an alias, or the same command inside a `.cmd` file
  all get past it.
- **CI runs after the push.** By the time it is red, the commit is public.
- **The audit reads the tip, gitleaks reads history.** Between them the coverage
  is good, but neither knows about a secret in a *branch you never push*, which
  is fine, or in a *commit you push from another clone with no hooks*, which is
  not.

## Build artifacts

**SymboGram publishes no prebuilt binaries.** `dist/`, `build-desktop*/` and
`Symbian1Qt473/` are ignored, and audit check (b) rejects a binary anywhere in
the publication set.

The reason is concrete rather than precautionary. Every build embeds `api_hash`
as a plain string literal — unavoidable for an MTProto client. A debug build
also embeds the builder's filesystem paths: the current
`build-desktop-debug\debug\SymboGram.exe` contains this machine's home
directory, which is exactly the check that catches it.

Both build scripts run `tools/scan-artifact.ps1` on the linked executable,
which looks for the signing key, session values and home paths, and reports
through a digest rather than a value.

> **Scan the uncompressed ELF, not the E32 image and not the `.sis`.**
> Two layers of compression sit between the linker and the installer and a
> substring search sees through neither. A SIS deflate-compresses its payload,
> so searching `dist/*.sis` for the `api_hash` finds nothing on a package that
> provably contains it. Less obviously, `abld` runs the linker output through
> `elftran` and the E32 image in `epoc32\release\` is **byte-pair compressed**
> as well (compression UID `0x102822AA`, 1.9 MB from a 4.0 MB ELF) -- so the
> scan that ran there was reading compressed bytes and reporting clean on
> everything. That is the third check in this repository to have shipped green
> while testing nothing.
>
> The Symbian scan therefore runs on the raw ELF at
> `epoc32\BUILD\SymboGram\SYMBOGRAM_EXE\GCCE\urel\SymboGram.exe`, after
> `abld build` and before `make sis`, and `build-symbian.cmd` refuses to
> package if that file is absent rather than scanning something it cannot read.
>
> It was caught by the scanner's own canary: it *expects* the `api_hash` to be
> present and warns when it is not. Do not silence that warning.

The honest way to publish binaries is to build them where there is nothing
personal to embed. That needs a CI runner with the Symbian^1 / Qt 4.7.3 SDK,
which GitHub-hosted runners cannot provide. Until then, source only.

## Verifying the audit still works

An audit that never fails is indistinguishable from one that checks nothing.
Three defects during this setup each reported green while checking nothing, and
only these tests found them. A fourth was found the same way afterwards: check
(k) shipped with a hex-run floor that its own control immediately proved too
high. Re-run this after touching `.gitignore` or the audit.

The controls for (k) and (l) are automated, including one that hex-encodes each
real local secret without ever printing it:

```powershell
pwsh -File tools/test-audit.ps1
```

It also asserts that a *well-formed* vector is accepted — otherwise (l) could
pass its rejection tests by simply banning the directory.

The rest are still by hand:

```powershell
# 1. clean tree passes, and the audit does not reject its own source
pwsh -File tools/audit-public.ps1

# 2. a shadowed tracked file is caught
Add-Content .gitignore "README.md"
pwsh -File tools/audit-public.ps1        # expect: ignore rule shadows a tracked file
git checkout .gitignore

# 3. a copied secret is caught (never printed)
Copy-Item secrets/telegram.yaml docs/leak.yaml
pwsh -File tools/audit-public.ps1        # expect: exact local secret value
Remove-Item docs/leak.yaml

# 4. a renamed avatar is caught by content
Copy-Item (Get-ChildItem secrets/session/*/SymboGram_avatars/* | Select -First 1) img/x.png
pwsh -File tools/audit-public.ps1        # expect: contact media by content
Remove-Item img/x.png

# 5. a machine-specific path is caught
#    Write a line containing this machine's home directory into a new
#    docs/x.md. It is not spelled out here on purpose: a document that
#    contains an example of what the audit looks for is rejected by it.
pwsh -File tools/audit-public.ps1        # expect: machine-specific path
```

Each must fail with a `category :: path` line and **no value**.

## If something leaks anyway

A force-push does not help. GitHub keeps serving the replaced commit at
`/commit/<sha>` indefinitely and mirrors the push into the public events
archive. Rewriting history makes a leak harder to find, not gone. Treat
anything pushed as disclosed.

1. **Rotate first, investigate second.**
   - `api_id` / `api_hash` — revoke and re-register at
     [my.telegram.org](https://my.telegram.org).
   - Session / `AuthKey` — Telegram → Settings → Devices → terminate that
     session. This invalidates the key immediately.
   - `secrets/symbogram.key` — generate a new key and certificate (the command
     is in the README) and treat SIS packages signed with the old one as
     untrusted.
2. Find every affected path and commit: `git log --all --diff-filter=A --name-only`.
3. Ask GitHub Support to expire the unreachable objects, or delete and recreate
   the repository. Nothing you can do from a client removes them.
4. Record what happened in `local/project-notes/security/incident-log.md`.
5. Re-run `pwsh -File tools/setup-hooks.ps1` and work out which layer should
   have caught it. If none would have, that is the finding — add the check.
