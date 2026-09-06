#!/usr/bin/env python3
"""Check that the layer, the schema and the provenance record still agree.

Four places name the API layer, and nothing at build time makes them move
together:

  * ``schema/api.tl`` carries tdesktop's own trailing ``// LAYER N`` marker.
    The generator's parser strips line comments, so it never reaches the
    output -- which is exactly why it can drift unnoticed.
  * ``libkg/tlschema.h`` announces ``#define API_LAYER N``, and that is the
    number sent in ``invokeWithLayer``.
  * ``schema/UPSTREAM.md`` records the pinned layer and the SHA-256 of the
    file the schema was generated from.

The consequence of disagreement is not a build error. Generated readers are a
``switch`` on the constructor id with no ``default:`` case, and TL carries no
length prefixes, so readers built for one layer fed another layer's bytes
consume the wrong number of bytes and every later field in the packet is
decoded from the wrong offset. Nothing throws. It presents as a message that
renders oddly or a chat list that comes back short.

``gen-schema.ps1 -Check`` answers a different question -- whether the committed
generated code is what the pinned schema produces at the layer passed on the
command line. It takes that layer as an argument, so it cannot notice that the
argument itself is wrong. This script is what notices.

Deliberately offline. It reaches nothing over the network and reads only files
already in the repository, so it can run in CI on a clone with no credentials.

Exit codes:

    0   ok         every recorded layer and digest agrees
    1   drift      the repository disagrees with itself
    2   malformed  a value could not be found or parsed
"""

import hashlib
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

API_TL = os.path.join(ROOT, 'schema', 'api.tl')
UPSTREAM = os.path.join(ROOT, 'schema', 'UPSTREAM.md')
TLSCHEMA_H = os.path.join(ROOT, 'libkg', 'tlschema.h')


class Malformed(Exception):
    pass


def read_text(path):
    if not os.path.exists(path):
        raise Malformed('missing file: %s' % os.path.relpath(path, ROOT))
    with open(path, 'r', encoding='utf-8', errors='replace') as handle:
        return handle.read()


def layer_from_api_tl():
    """tdesktop's trailing marker, e.g. '// LAYER 229'."""
    found = re.findall(r'^//\s*LAYER\s+(\d+)\s*$', read_text(API_TL), re.M)
    if not found:
        raise Malformed('no "// LAYER n" marker in schema/api.tl')
    # The marker belongs at the end of the file; take the last if a comment
    # elsewhere ever matches too.
    return int(found[-1])


def layer_from_tlschema_h():
    found = re.search(r'^#define\s+API_LAYER\s+(\d+)\s*$', read_text(TLSCHEMA_H), re.M)
    if not found:
        raise Malformed('no "#define API_LAYER n" in libkg/tlschema.h')
    return int(found.group(1))


def pin_from_upstream():
    """The layer and SHA-256 recorded in the provenance table."""
    text = read_text(UPSTREAM)

    layer = re.search(r'^\|\s*Layer\s*\|\s*(\d+)\s*\|', text, re.M)
    if not layer:
        raise Malformed('no "| Layer | n |" row in schema/UPSTREAM.md')

    digest = re.search(r'^\|\s*SHA-256\s*\|\s*`?([0-9a-fA-F]{64})`?\s*\|', text, re.M)
    if not digest:
        raise Malformed('no "| SHA-256 | ... |" row in schema/UPSTREAM.md')

    return int(layer.group(1)), digest.group(1).lower()


def sha256_of_api_tl():
    # Byte for byte, without normalising line endings: UPSTREAM.md states the
    # file is stored exactly as upstream has it so the digest can be checked
    # against the raw download.
    with open(API_TL, 'rb') as handle:
        return hashlib.sha256(handle.read()).hexdigest()


def main():
    try:
        marker = layer_from_api_tl()
        announced = layer_from_tlschema_h()
        pinned_layer, pinned_digest = pin_from_upstream()
        actual_digest = sha256_of_api_tl()
    except Malformed as problem:
        print('malformed: %s' % problem)
        return 2

    problems = []

    if marker != announced:
        problems.append(
            'schema/api.tl is layer %d but libkg/tlschema.h announces %d.\n'
            '    The generated readers and the layer sent in invokeWithLayer '
            'have come apart.' % (marker, announced))

    if pinned_layer != marker:
        problems.append(
            'schema/UPSTREAM.md pins layer %d but schema/api.tl is layer %d.'
            % (pinned_layer, marker))

    if pinned_digest != actual_digest:
        problems.append(
            'schema/api.tl does not match the digest in schema/UPSTREAM.md.\n'
            '    recorded %s\n'
            '    actual   %s\n'
            '    The schema has been edited, or the pin was not updated with it.'
            % (pinned_digest, actual_digest))

    if problems:
        print('drift:')
        for problem in problems:
            print('  - %s' % problem)
        return 1

    print('layer %d: api.tl, tlschema.h and UPSTREAM.md agree, '
          'and api.tl matches its recorded digest.' % marker)
    return 0


if __name__ == '__main__':
    sys.exit(main())
