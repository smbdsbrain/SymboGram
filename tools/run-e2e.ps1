<#
.SYNOPSIS
    Builds and runs the end-to-end harness (test tiers 1 and 2).

.DESCRIPTION
    Tier 1 (-Tier test) talks to Telegram's test data centres. Accounts there
    are disposable and public, so this tier is safe to run at will.

    Tier 2 (-Tier prod) talks to production against a session that already
    exists. It cannot log in: SMS has been unavailable to third-party api_ids
    since 2023-02-18, leaving sentCodeTypeApp, which needs another logged-in
    session.

    THE SESSION IS COPIED, NEVER USED IN PLACE. TgTransport::handleRpcError
    calls resetSession() on any 401, which erases the stored auth key. Point
    the harness at secrets/session directly and the first expired-session run
    destroys the only login on this machine. The copy lives under
    build-desktop/, which is gitignored, and is removed by -CleanSession.

.PARAMETER Tier
    test (default) or prod.

.PARAMETER Scenario
    connect, login, read, send, negative, or all.

.PARAMETER Phone
    Test-DC number, 99966XYYYY. Generated if omitted.

.PARAMETER Code
    Override the derived login code. Telegram's documented fixed code for test
    accounts is currently rejected on all three test DCs -- verified against an
    independent Telethon client -- so unattended login is not available today.

.EXAMPLE
    pwsh -File tools\run-e2e.ps1
    pwsh -File tools\run-e2e.ps1 -Tier prod -Scenario read
#>
[CmdletBinding()]
param(
    [ValidateSet('test', 'prod')]
    [string] $Tier = 'test',
    [ValidateSet('connect', 'login', 'read', 'send', 'negative', 'updates', 'all')]
    [string] $Scenario = 'all',
    [string] $Phone,
    [string] $Code,
    [int]    $DeadlineSeconds = 300,
    [switch] $CleanSession,
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

# The harness links the whole of libkg, so it needs apisecrets.h -- exactly as
# both build scripts do.
& pwsh -NoProfile -ExecutionPolicy Bypass -File (Join-Path $RepoRoot 'tools\write-apisecrets.ps1')
if ($LASTEXITCODE) { throw "write-apisecrets failed" }

$pro   = Join-Path $RepoRoot 'tests\e2e\e2e.pro'
$build = Join-Path $RepoRoot 'build-desktop\e2e'
if ($Clean -and (Test-Path $build)) { Remove-Item -Recurse -Force $build }
New-Item -ItemType Directory -Force -Path $build | Out-Null

Push-Location $build
try {
    & qmake.exe $pro -spec win32-g++ 'CONFIG+=release' | Out-Null
    if ($LASTEXITCODE) { throw "qmake failed ($LASTEXITCODE)" }
    $log = & mingw32-make.exe -j8 2>&1
    if ($LASTEXITCODE) { $log | Write-Host; throw "make failed ($LASTEXITCODE)" }
} finally { Pop-Location }

$exe = Join-Path $build 'release\e2e.exe'
if (-not (Test-Path $exe)) { throw "FAILED: e2e.exe was not produced" }

$runDir = Join-Path $RepoRoot ("build-desktop\e2e-session-" + $Tier)
if ($CleanSession -and (Test-Path $runDir)) { Remove-Item -Recurse -Force $runDir }
New-Item -ItemType Directory -Force -Path (Join-Path $runDir 'SymboGram') | Out-Null

if ($Tier -eq 'prod') {
    $dest = Join-Path $runDir 'SymboGram\SymboGram_e2e.ini'
    if (-not (Test-Path $dest)) {
        # Take a COPY of whatever real session exists. Never the original: a 401
        # makes the client erase the file it is using.
        $src = Get-ChildItem -Path $SessionDir -Recurse -Filter '*_user_session.ini' `
                             -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($src) {
            Copy-Item -LiteralPath $src.FullName -Destination $dest
            Write-Host "Copied a session into $runDir (the original is untouched)."
            Write-Host "If this run hits a 401 the COPY is erased, not your login." -ForegroundColor DarkGray
        } else {
            Write-Host "No session found under $SessionDir." -ForegroundColor Yellow
            Write-Host "The prod tier will report SKIP, which is the correct result" -ForegroundColor Yellow
            Write-Host "for a missing credential rather than a defect." -ForegroundColor Yellow
        }
    }
}

$scenarios = if ($Scenario -eq 'all') {
    if ($Tier -eq 'test') { @('connect', 'negative', 'login') } else { @('connect', 'read', 'updates') }
} else { @($Scenario) }

if (-not $Phone) {
    # 99966XYYYY. A fresh YYYY each run keeps per-number flood limits away.
    $Phone = "999662" + (Get-Random -Minimum 1000 -Maximum 9999)
}

$fail = 0; $skip = 0
foreach ($s in $scenarios) {
    Write-Host ""
    Write-Host "--- $s ($Tier) ---" -ForegroundColor Cyan
    $argv = @("--tier=$Tier", "--only=$s", "--session-dir=$runDir",
              "--phone=$Phone", "--deadline=$DeadlineSeconds")
    if ($Code) { $argv += "--code=$Code" }

    # Capture as well as display: a step SKIP is reported inside a scenario that
    # still exits 0, so counting exit codes alone would call a skipped assertion
    # a pass.
    $captured = & $exe @argv 2>&1
    # $exitCode, never $code. PowerShell variable names are case-insensitive, so
    # $code would be the same variable as the -Code parameter, and assigning
    # here would silently rewrite the caller's login-code override.
    $exitCode = $LASTEXITCODE
    $captured | ForEach-Object { Write-Host $_ }
    $stepSkips = @($captured | Where-Object { "$_" -match '# SKIP' }).Count
    switch ($exitCode) {
        0  { $skip += $stepSkips }
        77 { $skip++ }
        default { $fail++ }
    }
}

Write-Host ""
if ($fail -gt 0) {
    Write-Host "$fail scenario(s) failed." -ForegroundColor Red
    exit 1
}
if ($skip -gt 0) {
    Write-Host "No failures, but $skip step(s)/scenario(s) were SKIPPED." -ForegroundColor Yellow
    Write-Host "A skip is an assertion that did not run. Read the reasons above" -ForegroundColor Yellow
    Write-Host "before treating this as a green run." -ForegroundColor Yellow
} else {
    Write-Host "All scenarios passed." -ForegroundColor Green
}
Write-Host "This tier does not cover QML, src/ models, or the Symbian device."
exit 0
