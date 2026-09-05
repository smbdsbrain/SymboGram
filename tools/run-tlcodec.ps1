<#
.SYNOPSIS
    Builds and runs the offline TL codec tests (test tier 0).

.DESCRIPTION
    No network, no credentials, no phone. Builds tests/tlcodec against
    libkg/tgstream.cpp and the generated schema only -- deliberately not the
    whole of libkg, which would drag in apisecrets.h -- and runs it.

    Output is TAP 13 on stdout; the exit code is the number of failures.

    What this tier is for: the generated readers are a switch on the
    constructor id with no default: case, and TL carries no length prefixes.
    A reader fed bytes from a different layer does not fail, it silently
    consumes the wrong number of bytes and garbles the rest of the packet.
    These tests make that condition observable.
#>
[CmdletBinding()]
param(
    [switch] $Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\_env.ps1"

$QtDir    = if ($env:QTDIR_487) { $env:QTDIR_487 } else { 'C:\Qt\4.8.7' }
$MinGWDir = if ($env:MINGW)     { $env:MINGW }     else { 'C:\mingw482\mingw32' }

foreach ($p in @((Join-Path $QtDir 'bin\qmake.exe'), (Join-Path $MinGWDir 'bin\g++.exe'))) {
    if (-not (Test-Path $p)) { throw "FAILED: missing toolchain component: $p" }
}
$env:PATH = "$QtDir\bin;$MinGWDir\bin;$env:PATH"

$pro   = Join-Path $RepoRoot 'tests\tlcodec\tlcodec.pro'
# Under build-desktop/, which .gitignore already covers -- so adding a test
# target needs no ignore-rule change, and an ignore rule is not something to
# add casually in this repository.
$build = Join-Path $RepoRoot 'build-desktop\tlcodec'

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
New-Item -ItemType Directory -Force -Path $build | Out-Null

Push-Location $build
try {
    & qmake.exe $pro -spec win32-g++ 'CONFIG+=release' | Out-Null
    if ($LASTEXITCODE) { throw "qmake failed ($LASTEXITCODE)" }
    $log = & mingw32-make.exe -j8 2>&1
    if ($LASTEXITCODE) { $log | Write-Host; throw "make failed ($LASTEXITCODE)" }
} finally { Pop-Location }

$exe = Join-Path $build 'release\tlcodec.exe'
if (-not (Test-Path $exe)) { throw "FAILED: tlcodec.exe was not produced" }

$output = & $exe 2>&1
$failures = $LASTEXITCODE
$output | Write-Host

# The exit code alone is not evidence that the suite ran. When the executable
# cannot start -- blocked by policy, a missing runtime DLL, a half-written link
# -- $LASTEXITCODE can still hold the value left by the previous command, and a
# runner that trusts it reports success for a suite that produced nothing. The
# plan line is the assertion that results actually exist.
$plan = $output | Select-String -Pattern '^1\.\.(\d+)\s*$' | Select-Object -First 1
if (-not $plan) {
    throw "FAILED: no TAP plan line -- the suite produced no output, so its exit code means nothing"
}
if ([int] $plan.Matches[0].Groups[1].Value -eq 0) {
    throw "FAILED: TAP plan is 1..0 -- no cases ran"
}

Write-Host ""
if ($failures -eq 0) {
    Write-Host "tier 0 green." -ForegroundColor Green
    Write-Host "Covers reader/writer symmetry and the unknown-constructor desync."
    Write-Host "Does NOT cover: MTProto, the network, the models, QML, or the device."
} else {
    Write-Host "$failures failure(s)." -ForegroundColor Red
}
exit $failures
