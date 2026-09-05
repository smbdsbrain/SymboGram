<#
.SYNOPSIS
    Deliberate-failure tests for tools/audit-public.ps1.

.DESCRIPTION
    An audit that never fails is indistinguishable from one that checks nothing.
    That is not a slogan here: docs/security.md records three checks in this
    very file that shipped reporting green while testing nothing, and only
    deliberate-failure cases found them.

    So every check gets a control that proves it can go red. This script
    automates the ones that can be automated -- in particular the two added
    for recorded TL vectors, where the failure mode is a leak rather than a
    build break:

      (k) hex-encoded content is decoded and re-scanned
      (l) every file under tests/vectors/ declares where its bytes came from

    (k)'s control needs a real local secret to hex-encode. It takes one from
    Get-LocalSecretValues, encodes it and writes it to a scratch file WITHOUT
    ever printing it -- the value stays inside this process, exactly as the
    audit's own differential check does. With no secrets/ present (CI) that one
    control is reported as skipped rather than silently passing.

    Every control writes to tests/vectors/_control_*.tlv and removes it again,
    including on failure.

.EXAMPLE
    pwsh -File tools\test-audit.ps1
#>
[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

. "$PSScriptRoot\_env.ps1"

$audit      = Join-Path $RepoRoot 'tools\audit-public.ps1'
$vectorDir  = Join-Path $RepoRoot 'tests\vectors'
$passed = 0; $failed = 0; $skipped = 0; $total = 0

function Invoke-Audit {
    # Publication mode reads tracked + untracked-not-ignored, so a scratch file
    # under tests/vectors/ is picked up without staging anything.
    $out = & pwsh -NoProfile -ExecutionPolicy Bypass -File $audit 2>&1 | Out-String
    return [pscustomobject]@{ Code = $LASTEXITCODE; Text = $out }
}

function Test-Control ([string] $name, [string] $content, [string] $expect) {
    $script:total++
    $path = Join-Path $vectorDir ("_control_" + [guid]::NewGuid().ToString('N').Substring(0, 8) + ".tlv")
    New-Item -ItemType Directory -Force -Path $vectorDir | Out-Null
    try {
        [System.IO.File]::WriteAllText($path, $content)
        $r = Invoke-Audit
        if ($r.Code -eq 0) {
            Write-Host ("  FAIL  {0}" -f $name) -ForegroundColor Red
            Write-Host ("        audit passed; the check under test is not working") -ForegroundColor Red
            $script:failed++
        } elseif ($r.Text -notmatch [regex]::Escape($expect)) {
            Write-Host ("  FAIL  {0}" -f $name) -ForegroundColor Red
            Write-Host ("        failed, but not for '{0}'" -f $expect) -ForegroundColor Red
            $script:failed++
        } else {
            Write-Host ("  ok    {0}" -f $name) -ForegroundColor Green
            $script:passed++
        }
    } finally {
        if (Test-Path -LiteralPath $path) { Remove-Item -Force -LiteralPath $path }
    }
}

Write-Host "Deliberate-failure controls for audit-public.ps1"
Write-Host ""

# --- baseline: the tree must be clean, or every result below is meaningless --
$base = Invoke-Audit
if ($base.Code -ne 0) {
    Write-Host "  FAIL  baseline: audit is already failing on a clean tree" -ForegroundColor Red
    Write-Host "        Fix that first; the controls below cannot be interpreted." -ForegroundColor Red
    exit 1
}
Write-Host "  ok    baseline: clean tree passes" -ForegroundColor Green
$passed++

# --- (l) provenance ---------------------------------------------------------
Test-Control "(l) vector with no '# source:' line is rejected" `
    "b5757299`n" `
    "no '# source:' provenance line"

Test-Control "(l) vector claiming production provenance is rejected" `
    "# source: production`nb5757299`n" `
    "provenance is neither test-dc nor synthetic"

# A well-formed vector must NOT trip anything -- otherwise (l) is just banning
# the directory, which would pass the two controls above while being useless.
$script:total++
$okPath = Join-Path $vectorDir "_control_wellformed.tlv"
New-Item -ItemType Directory -Force -Path $vectorDir | Out-Null
try {
    [System.IO.File]::WriteAllText($okPath, "# source: synthetic`n# what: boolTrue`nb5757299`n")
    $r = Invoke-Audit
    if ($r.Code -eq 0) {
        Write-Host "  ok    (l) a well-formed synthetic vector is accepted" -ForegroundColor Green
        $passed++
    } else {
        Write-Host "  FAIL  (l) a well-formed synthetic vector was rejected" -ForegroundColor Red
        Write-Host "        (l) would be banning the directory rather than checking it" -ForegroundColor Red
        $failed++
    }
} finally {
    if (Test-Path -LiteralPath $okPath) { Remove-Item -Force -LiteralPath $okPath }
}

# --- (k) hex-decoded differential -------------------------------------------
# Every harvested value, not just one. The first attempt tested $secrets[0],
# which happened to be the 8-digit api_id; its hex expansion is 16 characters
# and the check's floor was 32, so the control went red and exposed a real hole
# rather than a scripting slip. Testing all of them is what stops a future
# short secret from silently falling under the floor.
$secrets = @(Get-LocalSecretValues | Where-Object { $_.Severity -ne "warn" })
if ($secrets.Count -eq 0) {
    Write-Host "  SKIP  (k) hex-encoded secrets are caught - no local secrets present" -ForegroundColor Yellow
    Write-Host "        Expected on CI. On a dev machine this means secrets/ is missing." -ForegroundColor Yellow
    $skipped++
} else {
    $latin1c = [Text.Encoding]::GetEncoding(28591)
    foreach ($s in $secrets) {
        # The value is never printed, never written in the clear, and the
        # scratch file is removed in Test-Control's finally block. Only its
        # name and length ever reach the console.
        $bytes = $latin1c.GetBytes($s.Value)
        $hex = ($bytes | ForEach-Object { $_.ToString("x2") }) -join ""
        Test-Control ("(k) hex-encoded '{0}' ({1} chars -> {2} hex) is caught" -f $s.Name, $s.Value.Length, $hex.Length) `
            ("# source: synthetic`n" + $hex + "`n") `
            "hex-decoded"
    }
}

Write-Host ""
if ($failed -gt 0) {
    Write-Host "$failed control(s) failed. A check that cannot go red is not a check." -ForegroundColor Red
    exit 1
}
Write-Host "$passed control(s) passed, $skipped skipped." -ForegroundColor Green
Write-Host "Each of these turns red only because the audit noticed. That is the point."
exit 0
