# TL schema provenance

`api.tl` is the Telegram API schema `libkg/tlschema.{h,cpp}` is generated from.
Regenerate with:

    pwsh -File tools\gen-schema.ps1 -Api schema\api.tl -Layer 229

## Pin

| | |
|---|---|
| Source | https://github.com/telegramdesktop/tdesktop |
| Path | `Telegram/SourceFiles/mtproto/scheme/api.tl` |
| Commit | `11d18d829b25d583f2e3058663b522a528fde51a` |
| Commit message | Update API scheme on layer 229. |
| Date | 2026-08-22 |
| Layer | 229 |
| SHA-256 | `7655504c25a5d3a368e7729d3e9e64afffcacbcb54ec85508c60f0472064b617` |

Stored byte-for-byte as upstream has it, LF line endings included, so the
digest above can be checked against the raw file without normalising anything:

    curl -sL https://raw.githubusercontent.com/telegramdesktop/tdesktop/11d18d829b25d583f2e3058663b522a528fde51a/Telegram/SourceFiles/mtproto/scheme/api.tl | sha256sum

## Why tdesktop rather than core.telegram.org

`core.telegram.org/schema` publishes the current layer, but it is a live page
with no version in its URL: it changes under you, and a schema that changes
under you cannot be pinned. tdesktop commits the schema as a file, one commit
per layer, with the layer in the commit message — which is exactly a pin.

The trailing `// LAYER 229` comment is tdesktop's own marker. The generator's
parser strips line comments, so it does not reach the output; the layer reaches
`#define API_LAYER` from `gen-schema.ps1 -Layer` instead, and the two are kept
in step by hand. `tools/check-schema-drift.py` is what verifies they agree, along with the
layer and digest recorded below:

    python tools\check-schema-drift.py

It reads only files already in the repository, so it needs no network and
no credentials. `gen-schema.ps1 -Check` cannot answer this: it takes the
layer as an argument and so cannot notice that the argument is wrong.

## The MTProto schema is separate, and unpinned here

`mtschema.{h,cpp}` comes from `tools/tl-generator/mtproto.json`, inside the
vendored generator. The MTProto service schema (`req_pq_multi`, `rpc_result`,
`msg_container`, `gzip_packed`, salts and acks) is versionless and has not
changed in years, so it moves with the generator's pin rather than with the API
layer. `generate()` is called with layer 0 for it, which is what suppresses a
second `#define API_LAYER`.

## Previous layer

Layer 166, from `tools/tl-generator/api.tl` (SHA-256
`4056d76f6bec7a43b31a625278ee24c0a29cbd462dfc4769be1361c0221b96c7`), the copy
baked into the vendored generator's `.qrc` by upstream Kutegram. Regenerating
at 166 from that file reproduces the pre-bump `libkg/tlschema.*` and
`libkg/mtschema.*` exactly — that reproduction is what established the
generator pipeline is faithful, and it is worth re-running before trusting any
future bump:

    pwsh -File tools\gen-schema.ps1 -Check -Layer 166 -Api tools\tl-generator\api.tl
