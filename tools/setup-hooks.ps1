<#
.SYNOPSIS
    Arm this clone's local leak-prevention gates. Safe to re-run.

.DESCRIPTION
    core.hooksPath is local config and is NOT carried by git clone, so this has
    to run once per clone. tools/build-desktop.cmd and tools/build-symbian.cmd
    set it too, so a clone that only ever builds still ends up hooked.

.EXAMPLE
    pwsh -File tools/setup-hooks.ps1
#>
[CmdletBinding()]
param([switch] $Quiet)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

Push-Location $RepoRoot
try {
    $problems = New-Object System.Collections.Generic.List[string]

    Write-Step "git hooks"
    & git config core.hooksPath .githooks
    # Git for Windows ignores the filesystem mode bit, but the index one still
    # has to be right or the hooks are not executable on a Linux checkout.
    & git update-index --chmod=+x .githooks/pre-commit .githooks/pre-push 2>$null | Out-Null
    Write-Ok "core.hooksPath = $(& git config --get core.hooksPath)"

    $pwshPath = (Get-Command pwsh -ErrorAction SilentlyContinue).Source
    if ($pwshPath) { Write-Ok "pwsh found: $pwshPath" }
    else { Write-Bad "pwsh not on PATH - the hooks will refuse every commit"; [void]$problems.Add("pwsh") }

    Write-Step "commit identity"
    $email = (& git config user.email)
    if ($email -like "*@users.noreply.github.com") {
        Write-Ok "user.email is a GitHub no-reply address"
    } else {
        Write-Bad "user.email is '$email' - a personal address in public commit metadata"
        Write-Host "        git config user.email '<id>+<login>@users.noreply.github.com'" -ForegroundColor Red
        [void]$problems.Add("email")
    }

    Write-Step "secret stores are ignored"
    # The point of asking git rather than reading .gitignore: an ignore rule can
    # be shadowed by a later negation, and only git knows the final answer.
    $mustBeIgnored = @("secrets/telegram.yaml", "secrets/symbogram.key", "libkg/apisecrets.h")
    if ($SessionDir.StartsWith($RepoRoot)) {
        $rel = $SessionDir.Substring($RepoRoot.Length).TrimStart("\", "/").Replace("\", "/")
        if ($rel) { $mustBeIgnored += $rel }
    }
    foreach ($p in $mustBeIgnored) {
        & git check-ignore -q $p 2>$null
        if ($LASTEXITCODE -eq 0) { Write-Ok "$p is gitignored" }
        else { Write-Bad "$p is NOT gitignored"; [void]$problems.Add("ignore:$p") }
    }

    $shadowed = @(& git ls-files --cached --ignored --exclude-standard)
    if ($shadowed.Count -eq 0) { Write-Ok "no ignore rule shadows a tracked file" }
    else {
        Write-Bad "$($shadowed.Count) tracked file(s) are shadowed by an ignore rule"
        $shadowed | ForEach-Object { Write-Host "        $_" -ForegroundColor Red }
        [void]$problems.Add("shadowed")
    }

    Write-Step "local credentials"
    $yaml = Join-Path $SecretsDir "telegram.yaml"
    if (Test-Path -LiteralPath $yaml) {
        $vals = @(Get-LocalSecretValues)
        $hash = $vals | Where-Object { $_.Name -eq "telegram.yaml api_hash" } | Select-Object -First 1
        # A digest proves it loaded without disclosing it. This output lands in
        # build logs and terminal scrollback.
        Write-Ok "api_hash $(Format-SecretDigest $(if ($hash) { $hash.Value } else { $null }))"
        Write-Ok "$($vals.Count) local secret value(s) available to the differential audit"
    } else {
        Write-Warn2 "no secrets/telegram.yaml - copy config/telegram.yaml.example and fill it in"
        Write-Warn2 "the audit's differential check does nothing without it"
    }

    Write-Step "agent guardrails"
    $seed = Join-Path $PSScriptRoot "claude-settings.seed.json"
    $dest = Join-Path $RepoRoot ".claude/settings.json"
    if (Test-Path -LiteralPath $seed) {
        if (Test-Path -LiteralPath $dest) {
            if ((Get-FileHash -LiteralPath $seed).Hash -eq (Get-FileHash -LiteralPath $dest).Hash) {
                Write-Ok ".claude/settings.json matches the tracked seed"
            } else {
                Write-Warn2 ".claude/settings.json differs from tools/claude-settings.seed.json"
                Write-Warn2 "review it, then copy the seed over it to restore the guardrails"
            }
        } else {
            New-Item -ItemType Directory -Force (Split-Path -Parent $dest) | Out-Null
            Copy-Item -LiteralPath $seed -Destination $dest
            Write-Ok "installed .claude/settings.json from the tracked seed"
        }
    }

    Write-Step "audit"
    & (Get-Command pwsh).Source -NoProfile -ExecutionPolicy Bypass `
        -File (Join-Path $PSScriptRoot "audit-public.ps1")
    if ($LASTEXITCODE -ne 0) { [void]$problems.Add("audit") }

    Write-Host ""
    if ($problems.Count -gt 0) {
        Write-Bad "setup incomplete: $($problems -join ', ')"
        exit 1
    }
    Write-Ok "setup complete - commits and pushes are audited from here on"
    Write-Host "        Hooks are a local gate, not a guarantee: --no-verify skips them" -ForegroundColor DarkGray
    Write-Host "        and core.hooksPath does not survive a clone. See docs/security.md." -ForegroundColor DarkGray
    exit 0
} finally {
    Pop-Location
}
