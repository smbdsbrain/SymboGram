<#
.SYNOPSIS
    Builds and runs the offline local-cache tests (test tier 0).

.DESCRIPTION
    No network, no credentials, no phone. Builds tests/updates against
    libkg/tgstore.cpp and the generated schema only -- deliberately not
    the whole of libkg, which would drag in apisecrets.h -- and runs it.

    Output is TAP 13 on stdout; the exit code is the number of failures.

    What this tier is for: the cache is optional by design, because Qt for
    Symbian may not offer a SQLite driver on every device. That makes the
    degradation path -- a store that will not open and must stay silent rather
    than take the client down with it -- as important as the working one, and
    it is the path no other tier ever exercises.
#>
[CmdletBinding()]
param(
    [switch] $Clean
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\_env.ps1"
. "$PSScriptRoot\build-freshness.ps1"

$QtDir    = if ($env:QTDIR_487) { $env:QTDIR_487 } else { 'C:\Qt\4.8.7' }
$MinGWDir = if ($env:MINGW)     { $env:MINGW }     else { 'C:\mingw482\mingw32' }

foreach ($p in @((Join-Path $QtDir 'bin\qmake.exe'), (Join-Path $MinGWDir 'bin\g++.exe'))) {
    if (-not (Test-Path $p)) { throw "FAILED: missing toolchain component: $p" }
}
$env:PATH = "$QtDir\bin;$MinGWDir\bin;$env:PATH"

$pro   = Join-Path $RepoRoot 'tests\store\store.pro'
# Under build-desktop/, which .gitignore already covers.
$build = Join-Path $RepoRoot 'build-desktop\store'

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
# Also discards the tree when a header layout has moved under it.
Assert-FreshBuild -BuildDir $build -SourceDirs @((Join-Path $RepoRoot 'libkg'), (Join-Path $RepoRoot 'src'), (Join-Path $RepoRoot 'tests/store'), (Join-Path $RepoRoot 'tests/tlcodec'))

Push-Location $build
try {
    & qmake.exe $pro -spec win32-g++ 'CONFIG+=release' | Out-Null
    if ($LASTEXITCODE) { throw "qmake failed ($LASTEXITCODE)" }
    $log = & mingw32-make.exe -j8 2>&1
    if ($LASTEXITCODE) { $log | Write-Host; throw "make failed ($LASTEXITCODE)" }
} finally { Pop-Location }

$exe = Join-Path $build 'release\store.exe'
if (-not (Test-Path $exe)) { throw "FAILED: store.exe was not produced" }

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
    Write-Host "Covers the round trip through a blob, dialog ordering, trimming and degradation,"
    Write-Host "plus the predicates that decide whether an edit or a delete is offered."
    Write-Host "Does NOT cover: whether the device's Qt has the driver at all -- smoke that on the phone."
} else {
    Write-Host "$failures failure(s)." -ForegroundColor Red
}
exit $failures
