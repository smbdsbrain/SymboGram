<#
.SYNOPSIS
    Scan a freshly built binary for anything that must not leave this machine.

.DESCRIPTION
    A thin entry point so the .cmd build scripts can call Assert-CleanArtifact.
    See tools/_env.ps1 for what it checks and, importantly, for why it must be
    pointed at the linked executable and never at a .sis package.

.EXAMPLE
    pwsh -File tools/scan-artifact.ps1 -Path build-desktop\release\SymboGram.exe
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory)] [string] $Path,
    # Pass -CredentialFree for a build that was deliberately made without
    # api_id/api_hash; then finding the api_hash inside is a failure, not the
    # expected result.
    [switch] $CredentialFree
)

$ErrorActionPreference = "Stop"
. (Join-Path $PSScriptRoot "_env.ps1")

$ok = Assert-CleanArtifact -Path $Path -ExpectApiHash (-not $CredentialFree)
if (-not $ok) { exit 1 }
exit 0
