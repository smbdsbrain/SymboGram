#!/usr/bin/env python3
"""Verify the vendored trees still match their pinned upstream commits.

docs/VENDORED.md has documented this script since the trees were absorbed, but
the file itself never existed -- the command in the "Verification" section
could not run at all. A verification step that cannot run is worse than none,
because the documentation asserts a guarantee nobody is checking.

Two modes:

    python tools/verify-vendored.py            offline
    python tools/verify-vendored.py --online   re-clone each pin and diff

Offline mode recomputes a manifest (path -> sha256 of every tracked file in
each vendored tree) and compares it against tools/vendored-manifest.txt. It
catches the case that actually happens: someone edits a vendored file while
fixing something nearby. It cannot tell you the pin is what upstream says it
is -- only that the tree has not drifted since the manifest was recorded.

Online mode is the real check. It clones each repository at its pinned commit,
deletes the paths in tools/vendored-prune.txt, and diffs against what is in the
tree. That is the claim docs/VENDORED.md actually makes: "clone each repository
at the commit above and delete exactly the paths in that file".

Reads the pin table out of docs/VENDORED.md rather than repeating it, so the
documentation and the check cannot disagree.

Exit codes: 0 clean, 1 drift found, 2 could not run the check.
"""

import argparse
import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
VENDORED_MD = os.path.join(REPO, "docs", "VENDORED.md")
PRUNE_TXT = os.path.join(REPO, "tools", "vendored-prune.txt")
MANIFEST = os.path.join(REPO, "tools", "vendored-manifest.txt")
EDITED_TXT = os.path.join(REPO, "tools", "vendored-edited.txt")

# Rows look like: | `libkg/zlib/` | https://... | `de1ba36...` | 2023-08-22 | zlib |
ROW = re.compile(
    r"^\|\s*`([^`]+)`\s*\|\s*(https://\S+)\s*\|\s*`([0-9a-f]{40})`\s*\|", re.M
)


def fail(msg):
    print("FAILED: " + msg, file=sys.stderr)
    sys.exit(2)


def read_pins():
    if not os.path.exists(VENDORED_MD):
        fail("no docs/VENDORED.md")
    with open(VENDORED_MD, encoding="utf-8") as fh:
        pins = [(p.rstrip("/"), u, c) for p, u, c in ROW.findall(fh.read())]
    if not pins:
        fail("no pin rows parsed out of docs/VENDORED.md -- has the table changed shape?")
    return pins


def read_list(path):
    paths = []
    if not os.path.exists(path):
        return paths
    with open(path, encoding="utf-8") as fh:
        for line in fh:
            line = line.strip()
            if line and not line.startswith("#"):
                paths.append(line.rstrip("/"))
    return paths


def read_prune():
    return read_list(PRUNE_TXT)


def read_edited():
    """Paths SymboGram has deliberately changed. See tools/vendored-edited.txt."""
    return set(read_list(EDITED_TXT))


def owned_files(prefix, all_prefixes):
    """Files belonging to THIS pin, excluding any nested pin's files.

    The trees nest: libkg/zlib, libkg/mbedtls and libkg/qt-json all live inside
    libkg, and tools/tl-generator/qt-json inside tools/tl-generator. Upstream
    had them as submodules, so a clone of libkg does not contain zlib's files
    at all. Without this exclusion every nested file reports as "not upstream",
    several hundred times over."""
    nested = [p for p in all_prefixes if p != prefix and p.startswith(prefix + "/")]
    out = []
    for rel in tracked_files(prefix):
        if not any(rel.startswith(n + "/") for n in nested):
            out.append(rel)
    return out


def tracked_files(prefix):
    """Files git would publish under prefix.

    Deliberately `--cached --others --exclude-standard`, the same set
    tools/audit-public.ps1 calls the publication set: tracked files plus
    untracked-and-not-ignored ones. Using --cached alone would report a
    freshly vendored tree as empty until someone staged it, which is exactly
    when you most want the check to work. Ignored build artefacts stay out
    either way."""
    out = subprocess.run(
        ["git", "-C", REPO, "ls-files", "-z", "--cached", "--others",
         "--exclude-standard", "--", prefix],
        capture_output=True, check=False,
    )
    if out.returncode:
        fail("git ls-files failed -- not a git checkout?")
    return sorted(p for p in out.stdout.decode("utf-8").split("\0") if p)


def norm(path):
    """sha256 of the file with CRLF folded to LF, so line-ending churn does not
    masquerade as a content change."""
    with open(path, "rb") as fh:
        return hashlib.sha256(fh.read().replace(b"\r\n", b"\n")).hexdigest()


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def build_manifest(pins):
    lines = []
    seen = set()
    prefixes = [p for p, _u, _c in pins]
    for prefix, _url, _commit in pins:
        for rel in owned_files(prefix, prefixes):
            if rel in seen:
                continue
            seen.add(rel)
            abspath = os.path.join(REPO, rel)
            if os.path.exists(abspath):
                lines.append("%s  %s" % (sha256(abspath), rel))
    return sorted(lines, key=lambda l: l.split("  ", 1)[1])


def offline(pins, write):
    current = build_manifest(pins)
    if not current:
        fail("no vendored files found -- check the paths in docs/VENDORED.md")

    if write or not os.path.exists(MANIFEST):
        with open(MANIFEST, "w", encoding="utf-8", newline="\n") as fh:
            fh.write("# sha256 of every tracked file in the vendored trees.\n")
            fh.write("# Regenerate with: python tools/verify-vendored.py --write\n")
            fh.write("\n".join(current) + "\n")
        print("Wrote %s (%d files)." % (os.path.relpath(MANIFEST, REPO), len(current)))
        return 0

    with open(MANIFEST, encoding="utf-8") as fh:
        recorded = [l.rstrip("\n") for l in fh if l.strip() and not l.startswith("#")]

    have = dict(l.split("  ", 1)[::-1] for l in current)
    want = dict(l.split("  ", 1)[::-1] for l in recorded)

    changed = sorted(p for p in have if p in want and have[p] != want[p])
    added = sorted(set(have) - set(want))
    removed = sorted(set(want) - set(have))

    for label, paths in (("modified", changed), ("added", added), ("removed", removed)):
        for p in paths:
            print("%-9s :: %s" % (label, p))

    if changed or added or removed:
        print("\n%d file(s) drifted from the recorded manifest." % (
            len(changed) + len(added) + len(removed)))
        print("If the change is intentional, record it in docs/VENDORED.md and")
        print("re-run with --write. Do not re-write the manifest to silence this.")
        return 1

    print("%d vendored files match the recorded manifest." % len(current))
    return 0


def online(pins):
    if not shutil.which("git"):
        fail("git not on PATH")
    prune = read_prune()
    declared = read_edited()
    prefixes = [p for p, _u, _c in pins]
    bad = 0
    accounted = set()

    for prefix, url, commit in pins:
        local = os.path.join(REPO, prefix)
        if not os.path.isdir(local):
            print("missing   :: %s" % prefix)
            bad += 1
            continue

        tmp = tempfile.mkdtemp(prefix="vendored-")
        try:
            # Full clone: a pinned commit is often not the tip, and --depth 1
            # cannot reach it.
            r = subprocess.run(["git", "clone", "--quiet", url, tmp],
                               capture_output=True)
            if r.returncode:
                print("clone-fail:: %s (%s)" % (prefix, url))
                bad += 1
                continue
            r = subprocess.run(["git", "-C", tmp, "checkout", "--quiet", commit],
                               capture_output=True)
            if r.returncode:
                print("no-commit :: %s @ %s" % (prefix, commit[:12]))
                bad += 1
                continue

            for rel in prune:
                if rel.startswith(prefix + "/"):
                    victim = os.path.join(tmp, rel[len(prefix) + 1:])
                    if os.path.isdir(victim):
                        shutil.rmtree(victim, ignore_errors=True)
                    elif os.path.exists(victim):
                        os.remove(victim)

            for rel in owned_files(prefix, prefixes):
                ours = os.path.join(REPO, rel)
                theirs = os.path.join(tmp, rel[len(prefix) + 1:])
                if not os.path.exists(theirs):
                    print("not-upstr :: %s" % rel)
                    bad += 1
                elif sha256(ours) != sha256(theirs):
                    # libkg/ was renormalised to CRLF during the rename while
                    # upstream is LF, so a byte difference is usually nothing
                    # but line endings. Reporting that as an edit buries the
                    # handful of real ones.
                    if norm(ours) == norm(theirs):
                        print("eol-only  :: %s" % rel)
                    elif rel in declared:
                        print("declared  :: %s" % rel)
                        accounted.add(rel)
                    else:
                        print("EDITED    :: %s" % rel)
                        bad += 1
        finally:
            shutil.rmtree(tmp, ignore_errors=True)

        print("checked   :: %s @ %s" % (prefix, commit[:12]))

    stale = sorted(declared - accounted)
    for rel in stale:
        print("no-longer :: %s" % rel)

    if bad:
        print("\n%d undeclared edit(s) against upstream." % bad)
        print("Either revert them, or record each one with its reason in")
        print("tools/vendored-edited.txt. Do not add a path there without the why:")
        print("that file is how the GPL-3.0 'state what you changed' obligation")
        print("stays checkable rather than aspirational.")
        return 1
    if stale:
        print("\n%d path(s) in tools/vendored-edited.txt no longer differ from" % len(stale))
        print("upstream. Remove them, so the record does not overstate what we changed.")
        return 1
    print("\nEvery vendored file matches upstream, or is a declared edit.")
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--online", action="store_true",
                    help="re-clone each pin and diff (needs network)")
    ap.add_argument("--write", action="store_true",
                    help="record the current tree as the manifest")
    args = ap.parse_args()

    pins = read_pins()
    return online(pins) if args.online else offline(pins, args.write)


if __name__ == "__main__":
    sys.exit(main())
