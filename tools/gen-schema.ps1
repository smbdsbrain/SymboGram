<#
.SYNOPSIS
    Regenerates libkg/tlschema.* and libkg/mtschema.* from a pinned TL schema.

.DESCRIPTION
    Builds tools/tlgen (our argv front end over the vendored Kutegram
    generator), runs it, and installs the four generated files into libkg/.

    The layer and the schema move together, always. generator.cpp emits
    `#define API_LAYER <n>` into tlschema.h from the same invocation that reads
    the schema, so there is no way to regenerate at one layer and announce
    another. That matters more here than it looks: every generated reader is a
    `switch` on the constructor id with no `default:` case, and TL carries no
    length prefixes. A reader built for layer B fed layer A's bytes does not
    fail -- it returns an empty object, leaves the stream parked mid-object,
    and every subsequent field in that packet is garbage. Silent, not loud.

.PARAMETER Api
    Path to the TL schema. Defaults to schema/api.tl once that exists,
    otherwise the one pinned inside the vendored generator (layer 166).

.PARAMETER Layer
    API layer number to stamp into tlschema.h.

.PARAMETER Check
    Generate into a temporary directory and report whether the result matches
    what is committed, without touching libkg/. This is the reproduction gate:
    regenerating layer 166 must reproduce the committed files exactly.

.EXAMPLE
    pwsh -File tools\gen-schema.ps1 -Check -Layer 166
    pwsh -File tools\gen-schema.ps1 -Api schema\api.tl -Layer 229
#>
[CmdletBinding()]
param(
    [string] $Api,
    [Parameter(Mandatory = $true)]
    [int]    $Layer,
    [switch] $Check
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\_env.ps1"

$QtDir    = if ($env:QTDIR_47) { $env:QTDIR_47 } else { 'C:\Qt\4.8.7' }
$MinGWDir = if ($env:MINGW)    { $env:MINGW }    else { 'C:\mingw482\mingw32' }

$genDir  = Join-Path $RepoRoot 'tools\tl-generator'
$proFile = Join-Path $RepoRoot 'tools\tlgen\tlgen.pro'
$buildIn = Join-Path $RepoRoot 'build-desktop\tlgen'
$libkg   = Join-Path $RepoRoot 'libkg'

if (-not $Api) {
    $pinned = Join-Path $RepoRoot 'schema\api.tl'
    $Api = if (Test-Path $pinned) { $pinned } else { Join-Path $genDir 'api.tl' }
}
if (-not (Test-Path $Api)) { throw "No such schema: $Api" }
# Resolve now, not later: generation runs from a temp staging directory (the
# generator always writes to the CWD), so a relative -Api would be looked up
# there and reported as missing.
$Api = (Resolve-Path $Api).Path

$mtproto = Join-Path $genDir 'mtproto.json'

foreach ($p in @((Join-Path $QtDir 'bin\qmake.exe'), (Join-Path $MinGWDir 'bin\g++.exe'))) {
    if (-not (Test-Path $p)) { throw "FAILED: missing toolchain component: $p" }
}
$env:PATH = "$QtDir\bin;$MinGWDir\bin;$env:PATH"

Write-Host "[1/3] build tlgen"
New-Item -ItemType Directory -Force -Path $buildIn | Out-Null
Push-Location $buildIn
try {
    & qmake.exe $proFile -spec win32-g++ 'CONFIG+=release' | Out-Null
    if ($LASTEXITCODE) { throw "qmake failed ($LASTEXITCODE)" }
    # -j8 is safe here: eight translation units, no generated-source ordering.
    $log = & mingw32-make.exe -j8 2>&1
    if ($LASTEXITCODE) { $log | Write-Host; throw "make failed ($LASTEXITCODE)" }
} finally { Pop-Location }

$exe = Join-Path $buildIn 'release\tlgen.exe'
if (-not (Test-Path $exe)) { throw "FAILED: tlgen.exe was not produced" }

Write-Host "[2/3] generate (layer $Layer)"
$stage = Join-Path ([System.IO.Path]::GetTempPath()) ("symbogram-schema-" + [guid]::NewGuid().ToString('N'))
New-Item -ItemType Directory -Force -Path $stage | Out-Null
Push-Location $stage
try {
    & $exe --api $Api --mtproto $mtproto --layer $Layer
    if ($LASTEXITCODE) { throw "tlgen failed ($LASTEXITCODE)" }
} finally { Pop-Location }

$names = @('tlschema.h', 'tlschema.cpp', 'mtschema.h', 'mtschema.cpp')
foreach ($n in $names) {
    if (-not (Test-Path (Join-Path $stage $n))) { throw "FAILED: generator produced no $n" }
}

# The generator opens its output with QFile::WriteOnly and no QIODevice::Text,
# so it writes LF. The files committed in libkg/ are CRLF, and .gitattributes
# deliberately does not normalise them (a blanket `text=auto eol=lf` would
# rewrite the byte-pinned vendored trees). Converting here is what makes
# `git diff` after a regeneration show the schema change and nothing else --
# without it every one of tlschema.cpp's 35,681 lines reports as modified and
# the real diff is unreadable. Do not "simplify" this away.
Write-Host "[3/3] $(if ($Check) { 'compare' } else { 'install' })"
$differs = @()
foreach ($n in $names) {
    $lf   = [System.IO.File]::ReadAllText((Join-Path $stage $n))
    $crlf = $lf -replace "(?<!`r)`n", "`r`n"
    $bytes = [System.Text.Encoding]::UTF8.GetBytes($crlf)

    $dest = Join-Path $libkg $n
    if ($Check) {
        $current = if (Test-Path $dest) { [System.IO.File]::ReadAllBytes($dest) } else { @() }
        if (@(Compare-Object $bytes $current -SyncWindow 0).Count -ne 0) { $differs += $n }
    } else {
        [System.IO.File]::WriteAllBytes($dest, $bytes)
    }
}
Remove-Item -Recurse -Force $stage

if ($Check) {
    if ($differs.Count) {
        Write-Host ""
        Write-Host "DIFFERS from committed: $($differs -join ', ')" -ForegroundColor Red
        Write-Host "If this was a layer-166 run, the generator pipeline is NOT faithful." -ForegroundColor Red
        Write-Host "Understand why before regenerating at a new layer -- a pipeline that"
        Write-Host "cannot reproduce the current schema cannot be trusted to produce the next."
        exit 1
    }
    Write-Host "Reproduces the committed schema exactly." -ForegroundColor Green
    exit 0
}

Write-Host ""
Write-Host "Installed into libkg/ at layer $Layer from $Api"
Write-Host "API_LAYER and the schema moved together. Commit them together too."
