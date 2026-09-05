# Vendored upstream trees

SymboGram is a monorepo. Where upstream Kutegram used git submodules, we track
the dependency source directly, at the exact commit upstream had pinned.

This file is the provenance record: it says where every vendored tree came
from, which commit it is, and what was removed. It is also how we satisfy the
GPL-3.0 obligation to state what we changed.

## Why not submodules

Upstream splits the project across five repositories. A contributor cloning
SymboGram should get a buildable tree from `git clone` alone, with no
`--recursive` and no chance of a half-initialised checkout. The trees below
are placed at **the same paths** upstream used, so `libkg.pri`, `mbedtls.pri`,
`zlib.pri`, `qt-json.pri`, `headers.pri` and `pigler.pri` are byte-for-byte
unmodified and future upstream merges stay tractable.

## Pinned commits

Captured from `git submodule status --recursive` against upstream
`kutegram/quick` at `1214003` (2024-02-21) before the gitlinks were removed.

| Path | Upstream | Commit | Date | Licence |
|---|---|---|---|---|
| `libkg/` | https://github.com/kutegram/libkg | `b41733ca4e29a330541d76078aaa3852c21cc944` | 2024-02-21 | GPL-3.0 |
| `libkg/mbedtls/` | https://github.com/kutegram/mbedtls | `6e27871576d05727e292cfb0cd64d6712db8b4cd` | 2024-02-21 | Apache-2.0 |
| `libkg/qt-json/` | https://github.com/kutegram/qt-json | `800a381d8f12f056e51f303384c79805b2292811` | 2022-10-13 | GPL-3.0 |
| `libkg/zlib/` | https://github.com/kutegram/zlib | `de1ba36ed2ed05db06ae55dd85a14d8eb3ed4675` | 2023-08-22 | zlib |
| `pigler/` | https://github.com/piglerorg/pigler | `a350e04cdafa4b66f07ab4e3947afe03671c3bcd` | 2023-08-16 | GPL-3.0 |
| `tools/tl-generator/` | https://github.com/kutegram/generator | `3fd0d338b834aa5c5593fc644d50de7eba5f7b9b` | 2023-10-29 | GPL-3.0 |
| `tools/tl-generator/qt-json/` | https://github.com/kutegram/qt-json | `1d795de012c1570a2620c3877e4582113dea7d6d` | — | GPL-3.0 |

`mbedtls` is `v3.4.1-21-g6e2787157`; `zlib` is `v1.3-1-gde1ba36`; `pigler` is
`1.0.0-50-ga350e04`.

Note on `pigler`: upstream `piglerorg/pigler` has since moved on to `23b5f39`
(v1.4.1, 2025-09-26). We deliberately vendor the **pin**, not the newer tip.
Pigler only compiles when `SYMBIAN_VERSION == Symbian3`, which SymboGram does
not yet target, so adopting a newer version is a separate decision to be made
when the Symbian^3 build lands rather than an incidental side effect of the
fork.

Note on `qt-json`: the licence is **GPL-3.0**, not MIT. Kutegram's fork
relicensed it relative to the original `lawand/qt-json`.

Note on `tl-generator`: this is the tool that produces `libkg/tlschema.*` and
`libkg/mtschema.*`, which upstream ships as checked-in generated code with no
record of what generated it. It is vendored at its own pin, and it carries its
own `qt-json` at a **different** commit (`1d795de0`) from the one `libkg/`
uses (`800a381d`) — upstream pinned them separately, and reproducing the
generator faithfully means reproducing its pin, not ours.

Not edited, including `main.cpp`, which hardcodes the layer and reads the
schema out of its own `.qrc`. `tools/tlgen/` is our own small front end that
compiles the vendored sources against an argv-driven `main` instead, so the
layer and the schema can be chosen at run time while the pinned tree stays
byte-verifiable. `tools/gen-schema.ps1` drives it.

Verified faithful before the layer was raised: regenerating layer 166 from
`tools/tl-generator/api.tl` reproduced all four then-committed files exactly,
byte for byte after the LF→CRLF conversion `gen-schema.ps1` performs. That is
what established the pipeline can be trusted, and it is the reason the 229 bump
was a readable diff rather than a rewrite of every line.

To repeat that proof, check out the last commit before the bump and run

    pwsh -File tools\gen-schema.ps1 -Check -Layer 166 -Api tools\tl-generator\api.tl

Against the current tree the standing check is the schema it is actually on;
see [docs/testing.md](testing.md).

## Licence composition

The combined work is **GPL-3.0**. Apache-2.0 (mbedtls) is one-way compatible
with GPLv3, and the zlib licence is permissive; both may be combined into a
GPLv3 work. `COPYING` at the repository root, and `libkg/COPYING`, are
retained unmodified, as are each dependency's own licence file.

## Removed content

The vendored trees are pruned to what the build actually references.
`mbedtls.pri` enumerates every source file it compiles and `headers.pri` every
header; nothing outside `library/`, `include/`, `configs/` and `group/` is
referenced. Unpruned the five trees are ~47 MB, of which `mbedtls/tests/`
alone is ~27 MB.

Removed paths are listed in `tools/vendored-prune.txt`. To reproduce this tree
from scratch, clone each repository at the commit above and delete exactly the
paths in that file.

## Edited content

`mbedtls`, `zlib`, `qt-json`, `pigler` and `tl-generator` are unedited: every
remaining file is byte-identical to its pinned commit.

**`libkg/` is not.** It is a fork rather than a pinned dependency — the rename
to SymboGram lands in it by definition — and an earlier version of this file
claimed otherwise for all six trees. Five files differ:

| File | Change |
|---|---|
| `apisecrets.h.empty` | `KUTEGRAM_API_*` → `SYMBOGRAM_API_*` |
| `tgc_auth.cpp` | the same two macros at the call site |
| `tgtransport.cpp` | `SYMBOGRAM_API_ID` in `initConnection` |
| `tgclient.cpp` | QML module name registered for `TgClient`/`TgStream`/`TgPacket` |
| `crypto.cpp` | CTR-DRBG personalisation string |

`libkg/` also differs from upstream in line endings throughout — it was
renormalised to CRLF during the rename. That is not an edit and the verifier
reports it separately.

The five are listed with their reasons in `tools/vendored-edited.txt`, which
`verify-vendored.py --online` reads: an edit that is not recorded there fails
the check, and a recorded path that no longer differs fails it too. That is
what turns the GPL-3.0 "state what you changed" obligation from prose into
something that cannot quietly go stale.

## Verification

    python tools/verify-vendored.py            # offline: tree manifest vs recorded
    python tools/verify-vendored.py --write    # re-record the manifest
    python tools/verify-vendored.py --online   # re-clone each pin and diff

Offline compares against `tools/vendored-manifest.txt` and catches a vendored
file edited in passing. Online is the real check and is the one that backs the
claims above; it needs network and takes a few minutes, because a pinned commit
is usually not a tip and cannot be reached with `--depth 1`.

## Regenerating the schema

`libkg/tlschema.*` is generated, not written. See
[schema/UPSTREAM.md](../schema/UPSTREAM.md) for the pinned input and
`tools/gen-schema.ps1` for the command. The layer and the schema move together
in one invocation by design.
