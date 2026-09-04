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

Removed paths are listed in `tools/vendored-prune.txt`. No file that remains
has been edited. To reproduce this tree from scratch, clone each repository at
the commit above and delete exactly the paths in that file.

## Verification

    python tools/verify-vendored.py            # offline: recompute tree manifest
    python tools/verify-vendored.py --online   # re-clone each pin and diff
