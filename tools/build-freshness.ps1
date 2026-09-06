<#
.SYNOPSIS
    Discard a test build tree when the headers behind it have changed.

.DESCRIPTION
    The test runners build into build-desktop/<target>/ and reuse whatever is
    already there, which is what makes them quick enough to run on every
    change. That reuse is only safe while the class layouts have not moved.

    Adding, removing or reordering a member in a libkg header changes the size
    and field offsets of a class. Object files compiled against the previous
    layout still link against the new ones, and the result is a binary whose
    parts disagree about where fields live. It does not fail to build and it
    does not fail an assertion: every TAP case prints ok and the process dies
    at teardown with 0xC0000005, which reads as a crash in the code under test
    rather than as a stale object file.

    So the build directory carries a digest of the headers it was produced
    from, and is discarded whenever that digest changes. A full rebuild of one
    of these targets costs under a minute; a misattributed access violation
    costs considerably more.

.NOTES
    Dot-source this and call Assert-FreshBuild before qmake.
#>

function Get-HeaderDigest {
    param(
        [Parameter(Mandatory = $true)] [string[]] $SourceDirs
    )

    $sha = [System.Security.Cryptography.SHA256]::Create()
    try {
        $accumulated = New-Object System.Text.StringBuilder

        foreach ($dir in ($SourceDirs | Sort-Object)) {
            if (-not (Test-Path $dir)) { continue }

            # Headers and project includes only: a .cpp change cannot move a
            # field offset, and make already handles those correctly.
            $files = Get-ChildItem -Path $dir -Recurse -File -Include '*.h', '*.pri' `
                        -ErrorAction SilentlyContinue |
                     Sort-Object -Property FullName

            foreach ($file in $files) {
                $bytes = [System.IO.File]::ReadAllBytes($file.FullName)
                $hash  = [System.BitConverter]::ToString($sha.ComputeHash($bytes))
                [void] $accumulated.AppendLine("$($file.FullName)=$hash")
            }
        }

        $final = $sha.ComputeHash([System.Text.Encoding]::UTF8.GetBytes($accumulated.ToString()))
        return [System.BitConverter]::ToString($final).Replace('-', '').ToLower()
    } finally {
        $sha.Dispose()
    }
}

function Assert-FreshBuild {
    param(
        [Parameter(Mandatory = $true)] [string]   $BuildDir,
        [Parameter(Mandatory = $true)] [string[]] $SourceDirs
    )

    $digest = Get-HeaderDigest -SourceDirs $SourceDirs
    $stamp  = Join-Path $BuildDir '.headers.sha256'

    if (Test-Path $BuildDir) {
        $previous = if (Test-Path $stamp) { (Get-Content $stamp -Raw).Trim() } else { '' }

        if ($previous -ne $digest) {
            Write-Host "# headers changed since this build tree was made; rebuilding it" `
                       -ForegroundColor DarkGray
            Remove-Item -Recurse -Force $BuildDir
        }
    }

    New-Item -ItemType Directory -Force -Path $BuildDir | Out-Null
    Set-Content -Path $stamp -Value $digest -Encoding ascii
}
