# Building SymboGram

Windows only. The Symbian toolchain is Windows-hosted and there is no
substitute for it.

## Once per clone

```
pwsh -File tools\setup-hooks.ps1
```

Sets `core.hooksPath`, checks that `secrets/` is genuinely ignored, installs the
agent guardrails and runs the audit. `core.hooksPath` is local config that
`git clone` does not carry, which is why this is a per-clone step. Both build
scripts set it too, so building at least once also arms it.

## Credentials

Register an application at [my.telegram.org](https://my.telegram.org), then:

```
copy config\telegram.yaml.example secrets\telegram.yaml
```

and fill in `api_id` and `api_hash`.

`tools\write-apisecrets.ps1` turns that into `libkg\apisecrets.h`, which
`libkg/libkg.pri` lists in `HEADERS` — the build cannot link without it. Both
build scripts run it, so you do not normally invoke it yourself.

Precedence, highest first:

1. `TG_API_ID` / `TG_API_HASH` in the environment
2. `secrets\telegram.yaml`

The environment comes first so a build can be given credentials without writing
them to disk, where they could end up in a cache, an uploaded artifact or a
`git status`. The generated header is gitignored; the generator prints a
SHA-256 digest rather than the values, because its output lands in build logs.

`app_title` should say `SymboGram`. If the credentials are registered to some
other application the generator warns: a shared `api_id` risks
`API_ID_PUBLISHED_FLOOD` taking down both projects at once.

## Signing certificate

```
openssl req -x509 -newkey rsa:2048 -sha256 -days 7300 -nodes ^
  -keyout secrets/symbogram.key -out secrets/symbogram.cer ^
  -subj "/CN=SymboGram/O=SymboGram/C=UA"
openssl rsa -in secrets/symbogram.key -out secrets/symbogram.key -traditional
```

The second command is not optional: `signsis` predates PKCS#8 and reads only
PKCS#1. The SDK's bundled `selfsigned.cer` expired years ago and Symbian
validates against the device clock, so an expired certificate presents as a
mysterious install failure rather than a signing error.

The key is unencrypted on disk. It is gitignored twice (`/secrets/`, `*.key`)
and the audit greps the publication set for its base64 body.

## Symbian target

```
tools\build-symbian.cmd [clean]
```

**Must run from `cmd.exe`.** Not Git Bash, not PowerShell: the SDK's Perl
scripts shell out to `find`, `sort` and `make`, and the Git-for-Windows POSIX
versions shadow the SDK's and fail in ways that look unrelated to the real
cause.

What it does:

1. `subst`s a short drive — `abld` builds under a path derived from the full
   source path and blows past `MAX_PATH`; the 2011 binaries are not long-path
   aware. `qtenvS1.bat` also derives `EPOCROOT` by stripping the drive letter,
   so the SDK and the project must sit on the same logical drive.
2. Generates `libkg\apisecrets.h`.
3. Clones the pinned toolchain into `Symbian1Qt473\` if absent (gitignored).
4. `qmake` → `bldmake bldfiles` → `abld build gcce urel`.
5. **Scans the uncompressed linker output** for signing key material, session
   values and machine paths — see below for why not the E32 image.
6. `make sis` with an explicit certificate, and copies the result to `dist\`.

`bldmake` is run explicitly rather than left to the generated Makefile: make
runs each recipe line in its own shell and `bldmake` is itself a `.bat`, so the
`ABLD.BAT` it produces is not visible to the next line.

### The image has to fit under 4 MB of code

`.data` is loaded at a fixed `0x400000`, so everything before it — `.text`,
`.rodata` and the ARM exception tables — has to fit in 4 MB. The generated TL
schema dominates that budget and only grows: `tlschema.o` was 3.8 MB at API
layer 166 and 5.8 MB at 229. At 229 it stopped linking:

```
arm-none-symbianelf-ld: section .data loaded at [00400000,00400003]
  overlaps section .ARM.extab loaded at [0035be3c,004187a3]
```

about 100 KB over. `.ARM.extab` holds exception unwind tables, and GCCE emits
them for every one of the schema's ~2470 generated functions.

`symbogram.pro` therefore compiles the Symbian target with
`OPTION GCCE -Os -fno-asynchronous-unwind-tables`, which brought `tlschema.o`
back to 4.2 MB and the SIS to slightly *smaller* than the layer-166 build. If a
future layer overflows again, the next lever is splitting the generator's output
across translation units — not because that reduces the total, it does not, but
because it makes it possible to compile the schema with stricter flags than the
rest of the app.

This failure appears **only on the Symbian target**. The desktop build links a
1.9 MB `tlschema.cpp` without complaint, so "the desktop build is green" says
nothing about it.

### `abld` does not report link failures

`abld` is a Perl wrapper around `make` and does not propagate its exit code:
the link can fail and `abld` still returns 0, so a `||` guard after it never
fires. Left alone, the build would continue with whatever `.exe` happened to be
on disk from an earlier run.

`tools\build-symbian.cmd` therefore deletes the target before calling `abld` and
checks the file exists afterwards, so whether the link succeeded is a question
for the filesystem rather than for `abld`. If you see

```
FAILED: abld reported success but produced no SymboGram.exe.
```

scroll up for the real error; `abld` swallowed it.

### Which file the artifact scan reads

Two layers of compression sit between the linker and the installer, and a
substring search sees through neither.

`make sis` deflate-compresses its payload, so searching `dist\*.sis` for a
known-embedded string finds nothing on a package that carries it. Less
obviously, `abld` runs the linker output through `elftran`, and the E32 image
in `epoc32\release\gcce\urel\` is **byte-pair compressed** as well --
compression UID `0x102822AA` at offset `0x1C` of the `E32ImageHeader`, 1.9 MB
from a 4.0 MB ELF. A substring search over either reads compressed bytes.

So the scan runs against the raw ELF, before `elftran`:

```
epoc32\BUILD\SymboGram\SYMBOGRAM_EXE\GCCE\urel\SymboGram.exe
```

Same code, uncompressed, and the only form a substring search can read.
`build-symbian.cmd` refuses to package if that file is absent rather than
falling back to a target it cannot scan.

`scan-artifact.ps1` expects the `api_hash` to be present in the binary and
warns when it is not. Treat that warning as a signal that the scan is pointed
at the wrong file, or that `apisecrets.h` never reached the link -- not as
noise to silence.

To confirm the scan is live on a given file, append this machine's own home
directory to a **copy** and check it exits 1. The literal form is not written
out here on purpose: a document containing an example of what the audit looks
for is rejected by the audit.

```powershell
Copy-Item <the ELF> $env:TEMP\ctrl.bin
Add-Content -Value $HOME -Path $env:TEMP\ctrl.bin
pwsh -File tools\scan-artifact.ps1 -Path $env:TEMP\ctrl.bin   # must exit 1
Remove-Item $env:TEMP\ctrl.bin
```

## Desktop target

```
tools\build-desktop.cmd [clean]
```

This is an **autonomous test target**, not a shipping build. It exists so the
app can be run and driven on a PC instead of requiring a SIS install and manual
tapping on the phone for every change.

Expected setup:

| | Path | Installer |
|---|---|---|
| Qt | `C:\Qt\4.8.7` | `qt-opensource-windows-x86-mingw482-4.8.7.exe` |
| MinGW | `C:\mingw482\mingw32` | `i686-4.8.2-release-posix-dwarf-rt_v3-rev3.7z` |

Those two paths are the only machine-specific paths written down anywhere in
the tracked tree, and they are deliberate: they are toolchain locations, not
anybody's home directory, which is why the audit's machine-path check does not
match them.

Qt **4.x specifically, not 5 or 6**: all 23 QML files use `import QtQuick 1.0`,
and QtQuick 1 was removed in Qt 5.6. Qt 4.8.7 still ships it, so the QML runs
unmodified and stays identical to what the Symbian build uses.

If Qt 4.8.7 was installed silently (`/S`), its installer skips the edition step
and leaves `src\corelib\global\qconfig.h` containing an unfinished
`#define QT_EDITION QT_EDITION_`. Every compile then dies inside Qt's own
headers with "QtValidLicenseForCoreModule does not name a type", which looks
like a toolchain mismatch and is not. Complete it to `QT_EDITION_OPENSOURCE`.

What the desktop target does **not** cover, and still needs a handset:

- the `Q_OS_SYMBIAN` branch of `src/platformutils.cpp`
- Pigler notifications, SIS packaging, capabilities
- real memory behaviour under `EPOCHEAPSIZE`

Device behaviour is authoritative wherever the two disagree.

### Running against a real account

`SYMBOGRAM_SESSION_DIR` relocates the session store, and
`SYMBOGRAM_LOG_FILE` the devlog. The default keeps both under `secrets/`, which
is gitignored, rather than `%APPDATA%` — a real session can then be reused
across runs without sitting somewhere easy to commit by accident.

That session file contains a usable authorization key. It is ignored three
times over and the audit hunts for its values, but it is still a live account
credential inside the working tree: do not archive the project directory, sync
it to cloud storage, or share your screen with the file tree open.

An account with a cloud password signs in normally; see [auth.md](auth.md) for
what happens between the code and the authorization.

### A second account, for the gap scenario

`run-e2e.ps1 -Tier prod -Scenario gap` needs one, because two sessions of a
single account are the same authorization and Telegram simply pushes the
update to whichever connection appears next -- no difference is involved and
the test asserts nothing.

Sign the second account in with the session store pointed at `secrets/session-b`:

```powershell
$env:SYMBOGRAM_SESSION_DIR = "$PWD\secrets\session-b"
.\build-desktop\release\SymboGram.exe
```

Then have the two accounts exchange at least one message, so the sender holds
an `access_hash` for the client, and run the scenario. The runner copies both
sessions into `build-desktop/` before using them.

## Before pushing

The hooks do this automatically. To run it by hand:

```powershell
pwsh -File tools\audit-public.ps1
```

See [security.md](security.md) for what it checks, what it deliberately does
not, and what to do if something leaks.
