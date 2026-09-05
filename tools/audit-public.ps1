<#
.SYNOPSIS
    Audit what is about to become public, before it becomes public.

.DESCRIPTION
    Reports file names and a category only: matched values and lines are
    deliberately never printed, so the audit output itself cannot leak.

    Modes:
      Publication  everything git would publish - tracked files PLUS untracked
                   files that are not ignored. Catches what a bulk stage would
                   sweep in, not just what is already staged. Content is read
                   from the working tree, which is what "publish" means here.
                   Default.
      Staged       what is staged, read from the index rather than the working
                   tree - those differ, and the index is what gets committed.
                   Used by .githooks/pre-commit.
      Range        every blob introduced between -Base and -Tip, read out of
                   git's object store. Used by .githooks/pre-push.

    Range mode reads objects, not files, and that distinction is the whole
    point of it. An earlier version diffed the range for names and then read
    those names off disk. A secret added in one commit and deleted in the next
    appears in NEITHER: not in the net diff, and not in the working tree. It
    passed clean while the secret sat in the history about to be pushed.

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

$EmptyTree = "4b825dc642cb6eb9a060e54bf8d69288fbee4904"

Push-Location $RepoRoot
try {

# ---------------------------------------------------------------------------
# Candidate set. Each candidate is a path plus, where the content lives in the
# object store rather than on disk, the blob it came from.
# ---------------------------------------------------------------------------
$candidates = New-Object System.Collections.Generic.List[object]
$seen = @{}

function Add-Candidate ([string] $path, [string] $blob) {
    if (-not $path) { return }
    $key = "$path|$blob"
    if ($seen.ContainsKey($key)) { return }
    $seen[$key] = $true
    $candidates.Add([pscustomobject]@{ Path = $path; Blob = $blob })
}

# ":100644 100644 <src> <dst> M\tpath" - and for a rename, two tab-separated
# paths, of which the destination is the one being introduced.
function Add-RawDiff ($lines) {
    foreach ($line in $lines) {
        if (-not $line -or $line[0] -ne ':') { continue }
        $parts = $line -split "`t"
        if ($parts.Count -lt 2) { continue }
        $meta = $parts[0] -split '\s+'
        if ($meta.Count -lt 5) { continue }
        $dst  = $meta[3]
        $path = $parts[$parts.Count - 1]
        if ($dst -match '^0+$') { continue }   # deletion
        Add-Candidate $path $dst
    }
}

switch ($Mode) {
    "Publication" {
        $raw = & git ls-files --cached --others --exclude-standard
        if ($LASTEXITCODE -ne 0) { Write-Bad "cannot enumerate the publication set"; exit 1 }
        foreach ($p in $raw) { Add-Candidate $p $null }
    }
    "Staged" {
        Add-RawDiff (& git diff --cached --raw --diff-filter=ACMR)
        if ($LASTEXITCODE -ne 0) { Write-Bad "cannot enumerate the staged set"; exit 1 }
    }
    "Range" {
        if (-not $Tip) { throw "-Mode Range requires -Tip" }
        if (-not $Base -or $Base -eq $EmptyTree) {
            # A brand-new branch: everything on it that is not already published.
            $commits = @(& git rev-list $Tip --not --remotes)
        } else {
            $commits = @(& git rev-list "$Base..$Tip")
        }
        foreach ($c in $commits) {
            if (-not $c) { continue }
            # -m so a merge commit is compared against each parent; without it a
            # merge introduces nothing and its content is never examined.
            Add-RawDiff (& git diff-tree -r -m --no-commit-id --diff-filter=ACMR $c)
        }
    }
}

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

<#
.SYNOPSIS
    Exact bytes of a candidate, from the object store or from disk.

.DESCRIPTION
    Read through the raw stdout stream rather than PowerShell's line pipeline:
    the pipeline decodes to text and normalises line endings, which corrupts a
    binary and can split a value being searched for.
#>
function Get-CandidateBytes ($cand) {
    if ($cand.Blob) {
        $psi = [Diagnostics.ProcessStartInfo]::new()
        $psi.FileName               = "git"
        $psi.RedirectStandardOutput = $true
        $psi.RedirectStandardError  = $true
        $psi.UseShellExecute        = $false
        $psi.WorkingDirectory       = $RepoRoot
        foreach ($a in @("cat-file", "blob", $cand.Blob)) { [void]$psi.ArgumentList.Add($a) }
        $proc = [Diagnostics.Process]::Start($psi)
        $ms   = New-Object IO.MemoryStream
        $proc.StandardOutput.BaseStream.CopyTo($ms)
        [void]$proc.StandardError.ReadToEnd()
        $proc.WaitForExit()
        if ($proc.ExitCode -ne 0) { return $null }
        return $ms.ToArray()
    }
    $full = Join-Path $RepoRoot $cand.Path
    if (-not (Test-Path -LiteralPath $full)) { return $null }
    try { return [IO.File]::ReadAllBytes($full) } catch { return $null }
}

function Test-BinaryBytes ($bytes) {
    $n = [Math]::Min($bytes.Length, 8192)
    for ($i = 0; $i -lt $n; $i++) { if ($bytes[$i] -eq 0) { return $true } }
    return $false
}

# ---------------------------------------------------------------------------
# (a) private path
# ---------------------------------------------------------------------------
$privateRoots = @(
    "secrets/", "local/", "private/", "dist/",
    "build-desktop/", "build-desktop-debug/", "Symbian1Qt473/",
    "obj/", "moc/", "ui/", "rcc/", "docs/local/", "notes/", "scratch/"
)
foreach ($c in $candidates) {
    $n = $c.Path.Replace("\", "/")
    foreach ($root in $privateRoots) {
        if ($n.StartsWith($root) -and -not $n.EndsWith("/.gitkeep")) {
            Add-Failure "private or generated path" $c.Path
        }
    }
}

# ---------------------------------------------------------------------------
# (b) binary artifact, ANYWHERE - this is how "only CI builds are published"
#     is actually enforced. A local build embeds api_hash as a plain literal,
#     and a debug build also embeds the developer's home path, so no locally
#     built binary may reach the publication set regardless of its path.
# ---------------------------------------------------------------------------
$artifactExt = @(
    ".exe", ".sis", ".sisx", ".dll", ".so", ".o", ".obj", ".a", ".lib",
    ".dso", ".sym", ".map", ".apk", ".deb", ".zip", ".7z", ".gz", ".jar"
)
# Tracked upstream files whose extension looks like an artifact but is not.
# gradle-wrapper.jar is Android tooling; zlib.map is a linker version script
# (a source file), not a linker map. Keep this list exact and tiny: it is the
# only hole in check (b).
$artifactAllowList = @(
    "android/gradle/wrapper/gradle-wrapper.jar",
    "libkg/zlib/zlib.map"
)
foreach ($c in $candidates) {
    $n = $c.Path.Replace("\", "/")
    if ($artifactAllowList -contains $n) { continue }
    if ($artifactExt -contains [IO.Path]::GetExtension($n).ToLowerInvariant()) {
        Add-Failure "locally built artifact" $c.Path
    }
}

# ---------------------------------------------------------------------------
# (c) contact media by name
# ---------------------------------------------------------------------------
foreach ($c in $candidates) {
    $n    = $c.Path.Replace("\", "/")
    $leaf = [IO.Path]::GetFileName($n)
    if ($n -match "_avatars/" -or $n -match "_photos/" -or $leaf -match '^\d{10,}\.(jpg|png)$') {
        Add-Failure "contact media" $c.Path
    }
}

# ---------------------------------------------------------------------------
# Read every candidate once. Hash it for check (d); keep the text for (e)-(g).
# Skip logs and anything large: a log yields junk key/value pairs and floods
# the comparison set. Binaries are hashed but never text-scanned.
# ---------------------------------------------------------------------------
$avatarHashes = @{}
$avatarSizes  = @{}
if (Test-Path -LiteralPath $SessionDir) {
    $media = Get-ChildItem -LiteralPath $SessionDir -Recurse -File -ErrorAction SilentlyContinue |
             Where-Object { $_.DirectoryName -match "_avatars|_photos" }
    foreach ($m in $media) {
        $avatarHashes[(Get-FileHash -Algorithm SHA256 -LiteralPath $m.FullName).Hash] = $true
        $avatarSizes[$m.Length] = $true
    }
}

$sha256 = [Security.Cryptography.SHA256]::Create()
$latin1 = [Text.Encoding]::GetEncoding(28591)
$candidateText = @{}
$scanned = 0

foreach ($c in $candidates) {
    $bytes = Get-CandidateBytes $c
    if ($null -eq $bytes) { continue }

    # (d) avatar content. Size is a free pre-filter - a renamed avatar keeps its
    #     byte length - and this runs in a pre-commit hook.
    if ($avatarHashes.Count -gt 0 -and $bytes.Length -gt 0 -and $avatarSizes.ContainsKey([int64]$bytes.Length)) {
        $h = ($sha256.ComputeHash($bytes) | ForEach-Object { $_.ToString("x2") }) -join ""
        if ($avatarHashes.ContainsKey($h.ToUpperInvariant())) {
            Add-Failure "contact media by content" $c.Path
        }
    }

    if ($bytes.Length -gt 4MB) { continue }
    if ([IO.Path]::GetExtension($c.Path).ToLowerInvariant() -eq ".log") { continue }
    if (Test-BinaryBytes $bytes) { continue }
    # Latin-1 is byte-transparent, so a substring search over this string is a
    # search over the original bytes.
    $candidateText[$c.Path] = $latin1.GetString($bytes)
    $scanned++
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
# "+", so without the parentheses this builds ONE space-joined string instead of
# a list, and the whole check silently matches nothing while still reporting
# green. That is not hypothetical: it shipped here and was caught only by the
# negative test in docs/security.md.
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
# (k) hex-encoded content, decoded and re-scanned.
#
#     Checks (e) and (g) above search a file's bytes for a literal value. Hex
#     text defeats both completely: an api_hash sitting inside a recorded
#     MTProto packet is present in the file as its hex expansion, which shares
#     no substring with the value being searched for. The same is true of a real
#     chat title, username or message body inside a captured packet.
#
#     tests/vectors/ exists precisely to hold recorded packets, so this is not a
#     theoretical gap - it is a hole opened by the thing that directory adds.
#     Decode every long hex run and run (e) and (g) again over the result.
#
#     Deliberately not restricted to tests/vectors/: hex is hex, and a packet
#     pasted into a Markdown file or a C++ string literal leaks exactly as well.
#     tests/tlcodec/main.cpp is full of hex literals and is scanned for that
#     reason rather than in spite of it.
#
#     What this does NOT catch, stated plainly so nobody over-trusts it: values
#     that are not byte-identical inside the packet. A session id or server salt
#     is an int64 and appears as eight little-endian bytes, not as the decimal
#     string Get-LocalSecretValues harvests. Provenance - check (l) - is what
#     covers that, not this.
# ---------------------------------------------------------------------------
#     The 16-character floor is 8 bytes, and it is a real limit, not a round
#     number: every value Get-LocalSecretValues harvests must survive hex
#     expansion above it or this check cannot see it at all. The shortest today
#     is an 8-digit api_id, which expands to exactly 16. tools/test-audit.ps1
#     asserts that for EVERY harvested value, so adding a shorter secret fails
#     the controls rather than quietly going unprotected.
#
#     Going lower is not free: 8 characters would decode every 32-bit
#     constructor id in tests/tlcodec/main.cpp and every short hash in the tree.
$hexRun = [regex] '(?i)(?<![0-9a-f])[0-9a-f]{16,}(?![0-9a-f])'
$decodedText = @{}
foreach ($entry in $candidateText.GetEnumerator()) {
    $runs = $hexRun.Matches($entry.Value)
    if ($runs.Count -eq 0) { continue }

    $sb = New-Object System.Text.StringBuilder
    foreach ($m in $runs) {
        $s = $m.Value
        # An odd-length run cannot be whole bytes. Drop the last nibble rather
        # than the whole run, so a hex blob abutting a word character still gets
        # looked at.
        if ($s.Length % 2) { $s = $s.Substring(0, $s.Length - 1) }
        $n = [int]($s.Length / 2)
        $buf = New-Object byte[] $n
        for ($i = 0; $i -lt $n; $i++) {
            $buf[$i] = [Convert]::ToByte($s.Substring($i * 2, 2), 16)
        }
        [void] $sb.Append($latin1.GetString($buf))
        # Separator, so a value cannot be manufactured across two unrelated runs.
        [void] $sb.Append("`n")
    }
    $decodedText[$entry.Key] = $sb.ToString()
}

foreach ($entry in $decodedText.GetEnumerator()) {
    $dn = $entry.Key.Replace("\", "/")
    if (-not (Test-Vendored $dn)) {
        foreach ($p in $credentialPatterns) {
            if ($entry.Value -match $p) { Add-Failure "possible credential (hex-decoded)" $entry.Key; break }
        }
    }
    # As with (g), this applies to vendored trees too.
    foreach ($s in $localValues) {
        if ($entry.Value.Contains($s.Value)) {
            if ($s.Severity -eq "warn") { Add-Warning "local value, hex-decoded ($($s.Name))" $entry.Key }
            else                        { Add-Failure "exact local secret value, hex-decoded ($($s.Name))" $entry.Key }
        }
    }
}

# ---------------------------------------------------------------------------
# (l) recorded-packet provenance.
#
#     A TL vector captured from a production session is somebody's chat list,
#     and no pattern check can recognise that - a real chat title is just text.
#     The only enforceable rule is where the bytes came from, so every file
#     under tests/vectors/ must say, and only the test environment is allowed.
#
#     Telegram's test DCs hand out accounts to anyone who asks (99966XYYYY,
#     code = the DC id five times) and wipe them periodically, so a packet from
#     one is genuinely publishable. A packet from production never is.
#
#     Missing or unreadable provenance is a failure, not a warning: a vector
#     with no header is exactly what an accidental prod capture looks like.
# ---------------------------------------------------------------------------
$vectorRoot = "tests/vectors/"
foreach ($c in $candidates) {
    $vn = $c.Path.Replace("\", "/")
    if (-not $vn.StartsWith($vectorRoot)) { continue }
    if ($vn.EndsWith(".md")) { continue }

    $vtext = $candidateText[$c.Path]
    if ($null -eq $vtext) {
        Add-Failure "vector unreadable as text (must be hex, not binary)" $c.Path
        continue
    }
    if ($vtext -notmatch '(?m)^#\s*source:\s*(\S+)') {
        Add-Failure "vector has no '# source:' provenance line" $c.Path
        continue
    }
    if ($Matches[1] -ne "test-dc" -and $Matches[1] -ne "synthetic") {
        Add-Failure "vector provenance is neither test-dc nor synthetic" $c.Path
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
#     introduce a tracked .exe/.map/.a that the rules silently start hiding.
#
#     "git ls-files --cached --ignored" asks exactly this question in one call.
#     Two implementations were rejected first, and both LOOKED like they worked:
#     a per-file "git check-ignore" loop reports nothing for tracked paths
#     unless --no-index is passed, and --stdin receives no input when piped
#     from PowerShell. Each produced a clean result while checking nothing, so
#     the positive control in docs/security.md is not optional.
# ---------------------------------------------------------------------------
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
    Write-Ok "$Mode audit passed: $($candidates.Count) path(s), $scanned scanned as text"
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
