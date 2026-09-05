# Security policy

SymboGram is an unofficial, experimental Telegram client for Symbian^3 and
Symbian Anna/Belle. It is not affiliated with or endorsed by Telegram.

## Reporting a vulnerability

Do not open a public issue for a vulnerability, and do not open one for a
suspected credential leak.

Use GitHub's private
[security advisory form](https://github.com/smbdsbrain/SymboGram/security/advisories/new).

Useful to include, where you have it:

- the commit or release you were running;
- the handset and firmware version, or the desktop build;
- what you did and what happened;
- the impact you expect.

Strip phone numbers, `api_id`, `api_hash`, authorization keys, session files
and message content from anything you attach. A partial report is fine; a
report you had to redact into uselessness is not.

There is no response-time commitment. This is a hobby project.

## Never post these in a public issue

`api_id` · `api_hash` · a phone number · an `auth_key` · `SymboGram_user_session.ini`
· `SymboGram_cache.ini` · contact avatars · a raw `symbogram.log`

The devlog records user ids and chat metadata. It is written for debugging, not
for publishing. Read [docs/security.md](docs/security.md) before pasting one.

If you have already posted a credential: rotate it. Deleting the comment does
not retract it, and the same is true of a pushed commit — see the runbook in
[docs/security.md](docs/security.md).

## No prebuilt binaries are published

SymboGram ships source only. This is a security decision, not an oversight.

Every build embeds its builder's `api_id` and `api_hash` as plain string
literals — unavoidable for any third-party MTProto client — and a debug build
additionally embeds the builder's filesystem paths. A binary built on a
developer's machine therefore carries traces of that machine.

The honest fix would be to build releases in CI, where there is nothing
personal to embed. That is not currently possible: the Symbian target needs the
proprietary Symbian^1 / Qt 4.7.3 SDK and the desktop target needs Qt 4.8.7 with
MinGW 4.8.2, neither of which runs on a GitHub-hosted runner. Until that is
solved, nothing is published rather than publishing something unverifiable.

Build it yourself with `tools\build-symbian.cmd`. You will need your own
credentials from [my.telegram.org](https://my.telegram.org); see the README.

Anyone offering a prebuilt SymboGram `.sis` is offering a binary built on their
machine with their credentials. Treat it accordingly.

## Security boundaries

SymboGram has had no security audit.

- MTProto cryptography runs on the handset via a vendored mbedtls (see
  [docs/VENDORED.md](docs/VENDORED.md)).
- The session file, which contains the authorization key, is stored
  **unencrypted**. Anyone with read access to it is logged into the account.
- Secret chats are not implemented.
- The platform is end-of-life and receives no vendor security updates.

Do not use it for conversations where any of that matters.
