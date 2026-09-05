## What this changes

<!-- One or two sentences. Why, not just what. -->

## How it was verified

<!-- Tick what you actually ran. "Compiles" is not verification. -->

- [ ] `tools\build-symbian.cmd` — SIS builds
- [ ] Installed and exercised on a handset (model and firmware: )
- [ ] `tools\build-desktop.cmd` — desktop build runs
- [ ] `pwsh -File tools\audit-public.ps1` passes

## Checklist

- [ ] I did not add credentials, `api_id`/`api_hash`, phone numbers, auth keys,
      session files, contact avatars or locally built binaries.
- [ ] I did not paste a raw `symbogram.log` — it contains user ids and chat
      metadata.
- [ ] Changes to vendored trees (`libkg/mbedtls`, `libkg/zlib`,
      `libkg/qt-json`, `pigler`) are recorded in `docs/VENDORED.md`.

<!--
The audit runs in CI, but its strongest check does not: comparing what you are
publishing against the REAL values in your local secrets/ only works on your
machine. Run tools\setup-hooks.ps1 once per clone so the hooks do it for you.
-->
