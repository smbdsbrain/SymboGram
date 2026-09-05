<#
.SYNOPSIS
    Builds and runs the offline MTProto crypto tests (test tier 0).

.DESCRIPTION
    No network, no credentials, no phone. Builds tests/crypto against
    libkg/crypto.cpp and the vendored mbedtls only -- deliberately not the
    whole of libkg, which would drag in apisecrets.h -- and runs it.

    Output is TAP 13 on stdout; the exit code is the number of failures.

    What this tier is for: key derivation reads a different 32-byte fragment of
    the auth key per direction, and a wrong fragment produces a key that is
    perfectly well-formed and completely wrong. The expected values are
    computed independently from the specification rather than by the code under
    test, because a self-consistent implementation agrees with itself whether or
    not it agrees with Telegram.
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

$pro   = Join-Path $RepoRoot 'tests\crypto\crypto.pro'
# Under build-desktop/, which .gitignore already covers.
$build = Join-Path $RepoRoot 'build-desktop\crypto'

if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
New-Item -ItemType Directory -Force -Path $build | Out-Null

Push-Location $build
try {
    & qmake.exe $pro -spec win32-g++ 'CONFIG+=release' | Out-Null
    if ($LASTEXITCODE) { throw "qmake failed ($LASTEXITCODE)" }
    $log = & mingw32-make.exe -j8 2>&1
    if ($LASTEXITCODE) { $log | Write-Host; throw "make failed ($LASTEXITCODE)" }
} finally { Pop-Location }

$exe = Join-Path $build 'release\crypto.exe'
if (-not (Test-Path $exe)) { throw "FAILED: crypto.exe was not produced" }

& $exe
$failures = $LASTEXITCODE

Write-Host ""
if ($failures -eq 0) {
    Write-Host "tier 0 green." -ForegroundColor Green
    Write-Host "Covers key derivation per direction and the IGE block-length check."
    Write-Host "Does NOT cover: the handshake, the transport, the network, or the device."
} else {
    Write-Host "$failures failure(s)." -ForegroundColor Red
}
exit $failures
