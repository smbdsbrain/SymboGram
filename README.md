# SymboGram

An unofficial Telegram client for Symbian, written in Qt Quick and C++.

SymboGram uses the Telegram API and is part of the Telegram ecosystem. It is
not affiliated with, endorsed by, or produced by Telegram.

## Relationship to Kutegram

SymboGram is a fork of [Kutegram Quick](https://github.com/kutegram/quick) by
**curoviyxru**, modified beginning September 2026. Upstream is dormant (last
commit February 2024) and its issue tracker is closed.

Substantive changes so far:

- **Monorepo.** Upstream's five repositories — `libkg`, `pigler`, `qt-json`,
  `mbedtls` and `zlib` — are vendored directly at the commits upstream pinned,
  so `git clone` alone yields a buildable tree. Provenance and the prune list
  are in [docs/VENDORED.md](docs/VENDORED.md).
- **Renamed** throughout, with its own application UID (`0xE4A51BF7`), so
  SymboGram installs alongside Kutegram rather than replacing it.
- **Drops the `SwEvent` capability.** SwEvent is a system capability and cannot
  be self-signed, so upstream's package will not install on a stock phone.
  SymboGram requests five user-grantable capabilities and reaches the browser
  via `StartDocument`. Build with `CONFIG+=swevent` to restore the
  original behaviour on a device that permits it.
- **Fixes four defects that prevented the Symbian build from compiling at all** —
  see the commit history for details.
- Raises the heap ceiling from qmlapplicationviewer's 32 MB default to 64 MB.

## Building

Windows only; the Symbian toolchain is Windows-hosted.

```
tools\build-symbian.cmd
```

The script fetches the pinned Symbian^1 / Qt 4.7.3 toolchain, generates
`libkg/apisecrets.h` from your credentials, builds, and produces a signed SIS
in `dist\`. It must be run from `cmd.exe` — the SDK's Perl scripts shell out to
`find` and `sort`, and Git-for-Windows' POSIX versions shadow them.

You need your own Telegram credentials. Register an application at
[my.telegram.org](https://my.telegram.org), then:

```
copy config\telegram.yaml.example secrets\telegram.yaml
```

and fill in `api_id` and `api_hash`. `secrets/` is gitignored. An `api_id` is
bound to one registered application; do not share one across clients.

You also need a signing certificate:

```
openssl req -x509 -newkey rsa:2048 -sha256 -days 7300 -nodes ^
  -keyout secrets/symbogram.key -out secrets/symbogram.cer ^
  -subj "/CN=SymboGram/O=SymboGram/C=UA"
openssl rsa -in secrets/symbogram.key -out secrets/symbogram.key -traditional
```

The second command is required: `signsis` predates PKCS#8 and reads only
PKCS#1. Do not rely on the SDK's bundled `selfsigned.cer` — it expired years
ago, and Symbian validates against the device clock.

Once per clone, arm the local leak gates:

```
pwsh -File tools\setup-hooks.ps1
```

This installs a pre-commit and pre-push audit. `core.hooksPath` is local git
config and is not carried by `git clone`, so it has to be set per clone — the
build scripts also set it, so building at least once covers you.

## Security and credentials

`secrets/` holds the `api_id`/`api_hash`, the SIS signing key and a live
Telegram session. It is gitignored several times over, and
`tools\audit-public.ps1` independently checks the whole would-be commit set
against the real values in it, reporting file names and never values.

**No prebuilt binaries are published, deliberately.** Every build embeds its
builder's `api_hash` as a plain string literal, and a debug build embeds the
builder's filesystem paths too. Building releases in CI would fix that, but the
Symbian^1 SDK does not run on a hosted runner — so SymboGram ships source only.
Build it yourself.

Never attach `SymboGram_user_session.ini` or a raw `symbogram.log` to an issue:
the session file is a full account takeover and the log records user ids and
chat metadata.

Details, limits and the leak runbook: [docs/security.md](docs/security.md).
Reporting a vulnerability: [SECURITY.md](SECURITY.md).

## Status

Early. The build works and produces an installable package; the feature set is
still upstream's, on API layer 166.

## Licence

GPL-3.0, inherited from Kutegram. See [COPYING](COPYING). Vendored dependencies
keep their own licences — Apache-2.0 for mbedtls, the zlib licence for zlib —
recorded in [docs/VENDORED.md](docs/VENDORED.md).
