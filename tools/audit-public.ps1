<#
.SYNOPSIS
    Audit what is about to become public, before it becomes public.

.DESCRIPTION
    Reports file names and a category only: matched values and lines are
    deliberately never printed, so the audit output itself cannot leak.

    Modes:
      Publication  everything git would publish - tracked files PLUS untracked
                   files that are not ignored. Catches what a `git add .` would
                   sweep in, not just what is already staged. Default.
      Staged       only what is staged. Used by .githooks/pre-commit.
      Range        the paths introduced between -Base and -Tip. Used by
                   .githooks/pre-push, and the reason a pre-push hook exists at
                   all: a secret committed five commits ago is invisible to a
                   Publication audit of a clean working tree.

.EXAMPLE
    pwsh -File tools/audit-public.ps1
#>
[CmdletBinding()]
param(
    [ValidateSet("Publication", "Staged", "Range")]
    [string] $Mode = "Publication",
    [string] $Base,
    [string] $Tip,
    [switch] $Quiet
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

Push-Location $RepoRoot
try {

# ---------------------------------------------------------------------------
# Candidate set
# ---------------------------------------------------------------------------
switch ($Mode) {
    "Publication" { $raw = & git ls-files --cached --others --exclude-standard }
    "Staged"      { $raw = & git diff --cached --name-only --diff-filter=ACMR }
    "Range" {
        if (-not $Base -or -not $Tip) { throw "-Mode Range requires -Base and -Tip" }
        $raw = & git diff --name-only "$Base..$Tip"
    }
}
if ($LASTEXITCODE -ne 0) {
    Write-Bad "cannot enumerate the $Mode set"
    exit 1
}

$candidates = @($raw | Where-Object { $_ } | Sort-Object -Unique)

$failures = New-Object System.Collections.Generic.List[string]
$warnings = New-Object System.Collections.Generic.List[string]

function Add-Failure ([string] $category, [string] $path) {
    $e = "$category :: $path"
    if (-not $failures.Contains($e)) { $failures.Add($e) }
}
function Add-Warning ([string] $category, [string] $path) {
    $e = "$category :: $path"
    if (-not $warnings.Contains($e)) { $warnings.Add($e) }
}
function Test-Vendored ([string] $normalized) {
    foreach ($root in $VendoredRoots) { if ($normalized.StartsWith($root)) { return $true } }
    return $false
}

# In Range mode a path may name a file that a later commit deleted; skip those
# rather than reporting them as unreadable.
$onDisk = @($candidates | Where-Object { Test-Path -LiteralPath (Join-Path $RepoRoot $_) })

# ---------------------------------------------------------------------------
# (a) private path
# ---------------------------------------------------------------------------
$privateRoots = @(
    "secrets/", "local/", "private/", "dist/",
    "build-desktop/", "build-desktop-debug/", "Symbian1Qt473/",
    "obj/", "moc/", "ui/", "rcc/", "docs/local/", "notes/", "scratch/"
)
foreach ($path in $candidates) {
    $n = $path.Replace("\", "/")
    foreach ($root in $privateRoots) {
        if ($n.StartsWith($root) -and -not $n.EndsWith("/.gitkeep")) {
            Add-Failure "private or generated path" $path
        }
    }
}

# ---------------------------------------------------------------------------
# (b) binary artifact, ANYWHERE - this is how "only CI builds are published"
#     is actually enforced. A local build embeds api_hash as a plain literal,
#     and a debug build also embeds the developer's home path, so no locally
#     built binary may ever reach the publication set regardless of its path.
# ---------------------------------------------------------------------------
$artifactExt = @(
    ".exe", ".sis", ".sisx", ".dll", ".so", ".o", ".obj", ".a", ".lib",
    ".dso", ".sym", ".map", ".apk", ".deb", ".zip", ".7z", ".gz", ".jar"
)
# Tracked upstream files whose extension looks like an artifact but is not.
# gradle-wrapper.jar is Android tooling; zlib.map is a linker version script
# (a source file), not a linker map. Keep this list exact and tiny: it is the
# only hole in check (b), and check (b) is how "no locally built binary is ever
# published" is actually enforced.
$artifactAllowList = @(
    "android/gradle/wrapper/gradle-wrapper.jar",
    "libkg/zlib/zlib.map"
)
foreach ($path in $candidates) {
    $n = $path.Replace("\", "/")
    if ($artifactAllowList -contains $n) { continue }
    if ($artifactExt -contains [IO.Path]::GetExtension($n).ToLowerInvariant()) {
        Add-Failure "locally built artifact" $path
    }
}

# ---------------------------------------------------------------------------
# (c) contact media by name
# ---------------------------------------------------------------------------
foreach ($path in $candidates) {
    $n    = $path.Replace("\", "/")
    $leaf = [IO.Path]::GetFileName($n)
    if ($n -match "_avatars/" -or $n -match "_photos/" -or $leaf -match '^\d{10,}\.(jpg|png)$') {
        Add-Failure "contact media" $path
    }
}

# ---------------------------------------------------------------------------
# (d) avatar content hash - catches an avatar copied and renamed into img/.
#     The ~118 avatar files are binaries: hash-compare only, never text-scan.
# ---------------------------------------------------------------------------
$avatarHashes = @{}
if (Test-Path -LiteralPath $SessionDir) {
    $media = Get-ChildItem -LiteralPath $SessionDir -Recurse -File -ErrorAction SilentlyContinue |
             Where-Object { $_.DirectoryName -match "_avatars|_photos" }
    foreach ($m in $media) {
        $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $m.FullName).Hash
        $avatarHashes[$h] = $true
    }
}
if ($avatarHashes.Count -gt 0) {
    # Size is a free pre-filter: hashing all ~600 candidates costs seconds and
    # this runs in a pre-commit hook. A renamed avatar keeps its byte length.
    $avatarSizes = @{}
    foreach ($m in $media) { $avatarSizes[$m.Length] = $true }
    foreach ($path in $onDisk) {
        $full = Join-Path $RepoRoot $path
        $info = Get-Item -LiteralPath $full
        if ($info.Length -eq 0 -or -not $avatarSizes.ContainsKey($info.Length)) { continue }
        $h = (Get-FileHash -Algorithm SHA256 -LiteralPath $full).Hash
        if ($avatarHashes.ContainsKey($h)) { Add-Failure "contact media by content" $path }
    }
}

# ---------------------------------------------------------------------------
# Read the text candidates once. Skip binaries, logs and anything large: a log
# yields junk key/value pairs and floods the comparison set.
# ---------------------------------------------------------------------------
$candidateText = @{}
foreach ($path in $onDisk) {
    $full = Join-Path $RepoRoot $path
    $info = Get-Item -LiteralPath $full
    if ($info.Length -gt 4MB) { continue }
    if ([IO.Path]::GetExtension($path).ToLowerInvariant() -eq ".log") { continue }
    if (Test-BinaryFile $full) { continue }
    try { $candidateText[$path] = [IO.File]::ReadAllText($full) }
    catch { Add-Failure "unreadable publication file" $path }
}

# ---------------------------------------------------------------------------
# (e) credential formats. Vendored trees are skipped: mbedtls ships literal
#     PEM headers in library/pkparse.c and library/pkwrite.c, and an audit that
#     fails against upstream source on its first run gets switched off.
# ---------------------------------------------------------------------------
$credentialPatterns = @(
    "-----BEGIN (RSA |EC |OPENSSH |DSA |PGP )?PRIVATE KEY-----",
    "\bgh[pousr]_[A-Za-z0-9]{20,}\b",
    "\bgithub_pat_[A-Za-z0-9_]{20,}\b",
    "\bAKIA[0-9A-Z]{16}\b",
    "\b[0-9]{6,12}:[A-Za-z0-9_-]{30,}\b",
    "(?i)\b(api[_-]?hash|auth[_-]?key|password|access[_-]?token)\s*[:=]\s*(['`"][A-Za-z0-9_+/\-]{16,}['`"]|[0-9a-f]{32,})",
    'SYMBOGRAM_API_HASH\s+"[0-9a-fA-F]{32}"'
)
# Machine-specific paths. C:\Qt\4.8.7 and C:\mingw482 in tools/build-desktop.cmd
# are intentional documentation and are not matched by the home-directory forms.
#
# Assembled from fragments rather than written literally, because a home-path
# pattern written out in full IS an example of the thing it looks for, so the
# audit rejects its own source. The alternative - exempting this file - would
# put a blind spot in the one file most worth auditing. For the same reason
# nothing below, and nothing in docs/security.md, spells one of these patterns
# out; describe them, do not quote them. (J2MEgram had to rewrite its build
# documentation for exactly this reason.)
#
# Each element is parenthesised deliberately. PowerShell binds "," TIGHTER than
# "+", so without the parentheses @('a' + $u + 'b', 'c' + $h + 'd') builds ONE
# space-joined string instead of a list, and the whole check silently matches
# nothing while still reporting green. That is not hypothetical: it shipped here
# and was caught only by the negative test in docs/security.md.
$u = "Users"
$h = "home"
$personalPatterns = @(
    ('[A-Za-z]:\\' + $u + '\\[^\\/\s"]+'),
    ('(?<![\w.])/' + $h + '/[^/\s"]+'),
    ('(?<![\w.])/' + $u + '/[^/\s"]+'),
    '(?m)^S:\\'
)
if ($personalPatterns.Count -ne 4) { throw "personalPatterns collapsed: $($personalPatterns.Count)" }
foreach ($entry in $candidateText.GetEnumerator()) {
    $n = $entry.Key.Replace("\", "/")
    if (Test-Vendored $n) { continue }
    foreach ($p in $credentialPatterns) {
        if ($entry.Value -match $p) { Add-Failure "possible credential" $entry.Key; break }
    }
    foreach ($p in $personalPatterns) {
        if ($entry.Value -match $p) { Add-Failure "machine-specific path" $entry.Key; break }
    }
}

# ---------------------------------------------------------------------------
# (g) differential exact-value match - the strongest check here. Applies to
#     ALL candidates including vendored trees: a real secret pasted into a
#     vendored file must still be caught.
# ---------------------------------------------------------------------------
$localValues = @(Get-LocalSecretValues)
$differentialRan = $localValues.Count -gt 0
foreach ($entry in $candidateText.GetEnumerator()) {
    foreach ($s in $localValues) {
        if ($entry.Value.Contains($s.Value)) {
            if ($s.Severity -eq "warn") { Add-Warning "local value ($($s.Name))" $entry.Key }
            else                        { Add-Failure "exact local secret value ($($s.Name))" $entry.Key }
        }
    }
}

# ---------------------------------------------------------------------------
# (h) the secret stores are genuinely ignored. Cheap, and it fails loudly the
#     moment an edit to .gitignore breaks a rule - rather than after a push.
# ---------------------------------------------------------------------------
$mustBeIgnored = @("secrets/telegram.yaml", "secrets/symbogram.key", "libkg/apisecrets.h")
if ($SessionDir.StartsWith($RepoRoot)) {
    $rel = $SessionDir.Substring($RepoRoot.Length).TrimStart("\", "/").Replace("\", "/")
    if ($rel) { $mustBeIgnored += $rel }
}
foreach ($p in $mustBeIgnored) {
    & git check-ignore -q $p 2>$null
    if ($LASTEXITCODE -ne 0) { Add-Failure "secret store is NOT gitignored" $p }
}

# ---------------------------------------------------------------------------
# (i) no ignore rule shadows a tracked file. Not a one-off validation: the
#     vendored trees will be re-synced from upstream, and a future sync could
#     introduce a tracked .exe/.map/.a that the new rules silently start hiding.
# ---------------------------------------------------------------------------
#
#     `git ls-files --cached --ignored` asks exactly this question in one call.
#     Two implementations were rejected first, and both LOOKED like they worked:
#     a per-file `git check-ignore` loop reports nothing for tracked paths
#     unless --no-index is passed, and `--stdin` receives no input when piped
#     from PowerShell. Each produced a clean result while checking nothing, so
#     the positive control in docs/security.md is not optional.
$shadowed = @(& git ls-files --cached --ignored --exclude-standard)
foreach ($path in $shadowed) {
    if ($path) { Add-Failure "ignore rule shadows a tracked file" $path }
}

# ---------------------------------------------------------------------------
# (j) force-push orphans. A force-push does not delete the commit it replaced:
#     GitHub keeps serving it at /commit/<sha> indefinitely and mirrors the push
#     into the public events archive. Rewriting history does not unpublish.
#     A warning, not a failure - a fresh clone has no reflog at all.
# ---------------------------------------------------------------------------
$orphans = New-Object System.Collections.Generic.List[string]
$branch = (& git rev-parse --abbrev-ref HEAD 2>$null)
if ($LASTEXITCODE -eq 0 -and $branch -and $branch -ne "HEAD") {
    $remoteRef = "origin/$branch"
    $reflog = @(& git reflog show $remoteRef 2>$null)
    if ($LASTEXITCODE -eq 0 -and $reflog.Count -gt 1) {
        $tipSha = (& git rev-parse $remoteRef 2>$null)
        foreach ($line in $reflog) {
            if ($line -match '^([0-9a-f]{7,40})\s') {
                $sha = (& git rev-parse $Matches[1] 2>$null)
                if ($LASTEXITCODE -ne 0 -or -not $sha -or $sha -eq $tipSha) { continue }
                & git merge-base --is-ancestor $sha $tipSha 2>$null | Out-Null
                if ($LASTEXITCODE -ne 0 -and -not $orphans.Contains($sha)) { $orphans.Add($sha) }
            }
        }
    }
}

# ---------------------------------------------------------------------------
# Report
# ---------------------------------------------------------------------------
if ($failures.Count -gt 0) {
    Write-Host ""
    Write-Bad "$Mode audit found $($failures.Count) issue(s); contents are hidden:"
    $failures | Sort-Object | ForEach-Object { Write-Host "        $_" -ForegroundColor Red }
    if ($warnings.Count -gt 0) {
        $warnings | Sort-Object | ForEach-Object { Write-Host "        (warn) $_" -ForegroundColor Yellow }
    }
    Write-Host ""
    Write-Host "        Nothing above is printed with its value. See docs/security.md" -ForegroundColor DarkGray
    Write-Host "        for what each category means and how to clear it." -ForegroundColor DarkGray
    exit 1
}

if (-not $Quiet) {
    Write-Ok "$Mode audit passed: $($candidates.Count) path(s), $($candidateText.Count) scanned as text"
    Write-Ok "no private path, artifact, contact media, credential format or machine path"
}

if ($differentialRan) {
    if (-not $Quiet) {
        Write-Ok "differential check ran against $($localValues.Count) local secret value(s)"
    }
} else {
    # Must be loud. On a CI runner there is no secrets/ and no session, so this
    # check silently does nothing - and a green badge would then be read as
    # "the differential check passed", which is the exact false confidence this
    # whole audit exists to prevent.
    Write-Warn2 "differential check SKIPPED - no local secrets present (expected in CI, not on a dev machine)"
}

if ($warnings.Count -gt 0) {
    $warnings | Sort-Object | ForEach-Object { Write-Warn2 $_ }
}

if ($orphans.Count -gt 0) {
    Write-Host ""
    Write-Warn2 "$($orphans.Count) commit(s) were pushed and then force-pushed away."
    Write-Warn2 "A clean working tree does not unpublish them - GitHub still serves each one:"
    foreach ($sha in $orphans) {
        Write-Host "         https://github.com/smbdsbrain/SymboGram/commit/$sha" -ForegroundColor DarkGray
    }
    Write-Warn2 "Inspect with 'git show --stat <sha>'. If one carries a credential, rotate it:"
    Write-Warn2 "deleting the branch does not remove the object. See docs/security.md."
}

exit 0

} finally {
    Pop-Location
}
