<#
.SYNOPSIS
    Shared paths, output helpers and secret handling for the SymboGram tools.

.DESCRIPTION
    Dot-sourced by tools/audit-public.ps1, tools/setup-hooks.ps1 and the build
    scripts. Nothing here ever prints a secret value: what must be shown goes
    through Format-SecretDigest.

    Ported from TelegramJ2ME/tools/_env.ps1, with the key harvesting policy
    narrowed for SymboGram - see Get-LocalSecretValues.
#>

$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path

# The Telegram session (auth key, server salt, session id) and the SIS signing
# key live under secrets/, which is gitignored. SYMBOGRAM_SESSION_DIR overrides
# where the running app puts the session; the audit follows it wherever it goes
# so its coverage does not depend on the default staying put.
$SecretsDir = Join-Path $RepoRoot "secrets"
$SessionDir = if ($env:SYMBOGRAM_SESSION_DIR) {
    $env:SYMBOGRAM_SESSION_DIR
} else {
    Join-Path $SecretsDir "session"
}

# Upstream sources kept verbatim; provenance recorded in docs/VENDORED.md.
# Format-shaped checks skip these because mbedtls ships literal PEM headers in
# library/pkparse.c and library/pkwrite.c. The differential value check does
# NOT skip them: a real secret pasted into a vendored file must still be caught.
$VendoredRoots = @(
    "libkg/mbedtls/",
    "libkg/zlib/",
    "libkg/qt-json/",
    "pigler/",
    "android/gradle/"
)

function Write-Ok    ($m) { Write-Host "  ok    $m" -ForegroundColor Green }
function Write-Bad   ($m) { Write-Host "  FAIL  $m" -ForegroundColor Red }
function Write-Warn2 ($m) { Write-Host "  warn  $m" -ForegroundColor Yellow }
function Write-Step  ($m) { Write-Host ""; Write-Host "== $m" -ForegroundColor Cyan }

<#
.SYNOPSIS
    Safe to print: proves a value was loaded without disclosing it.
#>
function Format-SecretDigest ($value) {
    if (-not $value) { return "not set" }
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($value)
    $sha   = [System.Security.Cryptography.SHA256]::Create().ComputeHash($bytes)
    $hex   = ($sha | ForEach-Object { $_.ToString("x2") }) -join ""
    return "set (sha256 " + $hex.Substring(0, 8) + "..., $($value.Length) chars)"
}

<#
.SYNOPSIS
    True if the file looks binary (a NUL byte in the first 8 KB).
#>
function Test-BinaryFile ([string] $Path) {
    try {
        $fs = [IO.File]::OpenRead($Path)
        try {
            $buf   = New-Object byte[] 8192
            $count = $fs.Read($buf, 0, $buf.Length)
            for ($i = 0; $i -lt $count; $i++) { if ($buf[$i] -eq 0) { return $true } }
            return $false
        } finally { $fs.Dispose() }
    } catch { return $true }
}

$script:PlaceholderPattern = '^(0|REPLACE_ME|CHANGE_ME|PENDING|example|localhost|127\.0\.0\.1)$'

<#
.SYNOPSIS
    Harvest the literal secret values that exist on this machine.

.DESCRIPTION
    The strongest check available: rather than guessing at credential shapes
    with regexes, read the real values and look for those exact strings in
    whatever is about to be published. Catches a secret in any encoding or
    context a regex would miss.

    Values are returned, never printed. Callers must report only Name.

    SymboGram-specific policy, and the reason it is not a copy of J2MEgram's:
    J2MEgram harvests any key matching host|address|endpoint|url|server. Here
    the session file's CurrentHost is a PUBLIC Telegram data-centre IP that
    appears verbatim in libkg/tgtransport.cpp, so harvesting it would make the
    audit fail against the project's own source on the very first run - and the
    natural reaction to that is to switch the check off. The allow-list below is
    therefore explicit rather than pattern-based.
#>
function Get-LocalSecretValues {
    $values = New-Object System.Collections.Generic.List[object]

    function Add-Value ([string] $name, $raw, [string] $severity = "fail") {
        if ($null -eq $raw) { return }
        $v = ([string]$raw).Trim()
        $v = $v.Trim([char]34).Trim([char]39)
        if ($v.Length -lt 6) { return }
        if ($v -match $script:PlaceholderPattern) { return }
        foreach ($e in $values) { if ($e.Value -ceq $v) { return } }
        $values.Add([pscustomobject]@{ Name = $name; Value = $v; Severity = $severity })
    }

    # --- secrets/telegram.yaml: api_id / api_hash only. app_title is public. ---
    $yaml = Join-Path $SecretsDir "telegram.yaml"
    if (Test-Path -LiteralPath $yaml) {
        foreach ($line in Get-Content -LiteralPath $yaml) {
            $t = $line.Trim()
            if ($t -eq "" -or $t.StartsWith("#")) { continue }
            $i = $t.IndexOf(":")
            if ($i -lt 1) { continue }
            $k = $t.Substring(0, $i).Trim()
            $v = $t.Substring($i + 1).Trim()
            if ($k -match '^(?i)(api_id|api_hash)$') { Add-Value "telegram.yaml $k" $v }
        }
    }

    # --- the live session: an auth key is full, un-rotatable account takeover ---
    # Explicit allow-list. Everything else in the file (CurrentHost, CurrentPort,
    # MainDc, TimeOffset, Sequence, LastMessageId, PingId) is public protocol
    # state and must NOT be harvested.
    $sessionKeys = @("AuthKey", "AuthKeyId", "ServerSalt", "SessionId", "UserId")
    if (Test-Path -LiteralPath $SessionDir) {
        $inis = Get-ChildItem -LiteralPath $SessionDir -Recurse -File -Filter "*_user_session.ini" -ErrorAction SilentlyContinue
        foreach ($ini in $inis) {
            foreach ($line in Get-Content -LiteralPath $ini.FullName) {
                $t = $line.Trim()
                if ($t -eq "" -or $t.StartsWith("#") -or $t.StartsWith("[")) { continue }
                $i = $t.IndexOf("=")
                if ($i -lt 1) { continue }
                $k = $t.Substring(0, $i).Trim()
                $v = $t.Substring($i + 1).Trim()
                if ($sessionKeys -notcontains $k) { continue }
                Add-Value "session $k" $v
                # QSettings stores a byte array as "@ByteArray(...)". The escaped
                # on-disk text is not the in-memory payload, so harvest both: a
                # leak could carry either form.
                if ($v -match '^"?@ByteArray\((.*)\)"?$') {
                    Add-Value "session $k payload" $Matches[1]
                }
            }
        }
    }

    # --- SIS signing key: whoever holds it can sign a package as SymboGram ---
    $pemFiles = @(
        @{ File = "symbogram.key"; Severity = "fail" },
        @{ File = "symbogram.cer"; Severity = "warn" }   # public half, still a fingerprint
    )
    foreach ($pem in $pemFiles) {
        $p = Join-Path $SecretsDir $pem.File
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $body = New-Object System.Text.StringBuilder
        foreach ($line in Get-Content -LiteralPath $p) {
            $t = $line.Trim()
            if ($t -eq "" -or $t.StartsWith("-----")) { continue }
            if ($t.Length -ge 40) { Add-Value "$($pem.File) line" $t $pem.Severity }
            [void]$body.Append($t)
        }
        if ($body.Length -ge 40) {
            Add-Value "$($pem.File) body" $body.ToString() $pem.Severity
        }
    }

    return $values
}

<#
.SYNOPSIS
    Prove a built binary carries nothing it must not carry.

.DESCRIPTION
    Reads the file as Latin-1 so the byte stream can be substring-searched
    without decode loss. Constant strings survive into the binary verbatim, so
    a hit is real.

    IMPORTANT - point this at the linked executable, NEVER at dist/*.sis.
    A SIS package deflate-compresses its payload: scanning dist/*.sis for the
    api_hash returns CLEAN on a binary that provably contains it. That is worse
    than no check at all, because it reports green. For the Symbian target the
    file to scan is
        Symbian1Qt473\epoc32\release\gcce\urel\SymboGram.exe
    immediately after `abld build` and before `make sis`.
#>
function Assert-CleanArtifact {
    param(
        [Parameter(Mandatory)] [string] $Path,
        # $true for a normal credentialled build: api_hash is SUPPOSED to be in
        # there (it is baked in by design, which is exactly why no locally built
        # binary is ever published). $false asserts a credential-free build.
        [bool] $ExpectApiHash = $true
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        Write-Warn2 "artifact scan skipped: $Path does not exist"
        return $true
    }

    $bytes = [IO.File]::ReadAllBytes($Path)
    $text  = [Text.Encoding]::GetEncoding(28591).GetString($bytes)
    $name  = Split-Path -Leaf $Path
    $found = New-Object System.Collections.Generic.List[string]

    $secrets = Get-LocalSecretValues
    $apiHash = $secrets | Where-Object { $_.Name -eq "telegram.yaml api_hash" } | Select-Object -First 1

    foreach ($s in $secrets) {
        # api_id / api_hash are handled separately below: they are baked in by
        # design and their presence is expected, not a defect.
        if ($s.Name -eq "telegram.yaml api_hash") { continue }
        if ($s.Name -eq "telegram.yaml api_id")   { continue }
        if ($text.Contains($s.Value)) { $found.Add($s.Name) }
    }

    # A debug build embeds the compiler's source paths. Verified: today's
    # build-desktop-debug\debug\SymboGram.exe contains the developer home path.
    if ($text -match '[A-Za-z]:\\Users\\[^\\/\s"]+') { $found.Add("developer home path") }
    if ($text -match '(?i)[A-Za-z]:\\[^\s"]*\\secrets\\') { $found.Add("secrets directory path") }

    if ($found.Count -gt 0) {
        Write-Bad "$name carries material that must not leave this machine:"
        $found | Sort-Object -Unique | ForEach-Object { Write-Host "          $_" -ForegroundColor Red }
        Write-Host "        Do not publish this artifact. See docs/security.md." -ForegroundColor Red
        return $false
    }

    if ($apiHash) {
        $has = $text.Contains($apiHash.Value)
        if ($ExpectApiHash -and -not $has) {
            Write-Warn2 "$name does not contain the api_hash - apisecrets.h may not have reached the link"
        } elseif (-not $ExpectApiHash -and $has) {
            Write-Bad "$name contains the api_hash but was built as credential-free"
            return $false
        }
    }

    Write-Ok "$name carries no signing key, session value or developer path"
    Write-Ok "this is still a LOCAL build and must not be published (docs/security.md)"
    return $true
}
