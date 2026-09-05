<#
.SYNOPSIS
    PreToolUse guard: block agent actions that would disarm the leak gates.

.DESCRIPTION
    Wired up by tools/claude-settings.seed.json. Reads the tool-input JSON on
    stdin and exits 2 with a reason to block, 0 to allow.

    This is the strongest control available against tool-driven mistakes,
    because unlike a permission glob it sees the literal command string. It is
    still not a sandbox: it guesses at intent from text. Treat it as a guard
    rail on a road, not a wall around a vault.

    Blocking rather than warning is deliberate. Every pattern here describes an
    action whose damage is not undoable by the next command: a force-push cannot
    be taken back, and a secret that reaches GitHub is disclosed the moment it
    lands, not when someone notices.

    Two lessons are baked into the matching itself:

    * Here-document bodies are stripped before matching. Without that, writing a
      commit message that MENTIONS one of these commands is blocked - which
      happened while writing the commit that introduced this file, and would
      have made every honest changelog entry about the guard unwritable.
    * Patterns are anchored to a command position, not matched anywhere in the
      string, so a filename or a quoted argument cannot trip them.
#>
[CmdletBinding()]
param(
    [ValidateSet("Bash", "Edit")]
    [string] $Kind = "Bash"
)

$ErrorActionPreference = "Stop"

function Deny ([string] $why) {
    # Exit code 2 is how a PreToolUse hook blocks the call and feeds the reason
    # back to the model; stderr carries the text.
    [Console]::Error.WriteLine("BLOCKED by tools/agent-guard.ps1: $why")
    [Console]::Error.WriteLine("If this is genuinely intended, ask the user to run it themselves.")
    exit 2
}

<#
.SYNOPSIS
    Drop here-document bodies, so prose that quotes a command is not the command.
#>
function Remove-HeredocBodies ([string] $text) {
    $lines  = $text -split "`r?`n"
    $out    = New-Object System.Collections.Generic.List[string]
    $marker = $null
    foreach ($line in $lines) {
        if ($marker) {
            if ($line.Trim() -eq $marker) { $marker = $null }
            continue
        }
        $out.Add($line)
        # <<EOF, <<'EOF', <<"EOF", <<-EOF
        if ($line -match '<<-?\s*[''"]?([A-Za-z_][A-Za-z0-9_]*)[''"]?\s*$') {
            $marker = $Matches[1]
        }
    }
    return ($out -join "`n")
}

$raw = [Console]::In.ReadToEnd()
if (-not $raw) { exit 0 }

try { $payload = $raw | ConvertFrom-Json } catch { exit 0 }
$toolInput = $payload.tool_input
if (-not $toolInput) { exit 0 }

if ($Kind -eq "Bash") {
    $cmd = [string]$toolInput.command
    if (-not $cmd) { exit 0 }

    $c = Remove-HeredocBodies $cmd
    # Collapse whitespace so "git   add    -f" reads the same as "git add -f".
    $c = ($c -replace '\s+', ' ')

    # A command starts at the beginning, or after a separator, or inside $( ).
    $start = '(?:^|[|;&(]\s*|\bthen\s+|\bdo\s+|\belse\s+)'
    # "git", optionally with -c/-C/--flag prefixes. -c is spelled out because
    # `git -c core.hooksPath=x commit` bypasses a rule that only knows `git config`.
    $git   = 'git\s+(?:-[cC]\s+\S+\s+|--\S+\s+)*'

    $rules = @(
        @{ P = "(?i)$start$git" + 'commit\b[^|;&]*\s(?:--no-verify|-n)\b'
           W = 'git commit with the verify step skipped bypasses the pre-commit audit' },
        @{ P = "(?i)$start$git" + 'push\b[^|;&]*\s(?:--no-verify|--force|--force-with-lease|-f)\b'
           W = 'a forced or unverified push cannot be taken back - GitHub keeps serving the replaced commits' },
        @{ P = "(?i)$start$git" + 'add\b[^|;&]*\s(?:--force|-f)\b'
           W = 'forcing a path into the index overrides .gitignore, which is what keeps secrets/ out of the repository' },
        @{ P = "(?i)$start$git" + 'config\b[^|;&]*core\.hooksPath'
           W = 'changing core.hooksPath disarms the pre-commit and pre-push audits' },
        # `git -c core.hooksPath=... <anything>` runs with the hooks redirected.
        @{ P = '(?i)\bgit\s+-c\s+core\.hooksPath'
           W = 'running git with core.hooksPath overridden disarms the pre-commit and pre-push audits' },
        @{ P = "(?i)$start$git" + '(?:filter-branch|filter-repo)\b'
           W = 'rewriting history does not unpublish anything; rotate the credential instead' },
        @{ P = "(?i)$start$git" + 'update-index\b[^|;&]*(?:--skip-worktree|--assume-unchanged)'
           W = 'hiding a tracked file from git status hides it from review too' },
        @{ P = "(?i)$start$git" + 'push\b[^|;&]*\bupstream\b'
           W = 'upstream is kutegram/quick - pushes there are not ours to make' },
        # Reading a secret into the transcript is itself the leak: the value then
        # lives in conversation history, which is stored and may be summarised.
        @{ P = "(?i)$start" + '(?:cat|type|more|less|head|tail|strings|xxd|od|base64)\b[^|;&]*\bsecrets[\\/]'
           W = 'that would read a secret into the transcript' },
        @{ P = "(?i)$start" + '(?:cat|type|more|less|head|tail|strings)\b[^|;&]*apisecrets\.h\b'
           W = 'apisecrets.h contains the api_hash' },
        @{ P = "(?i)$start" + '(?:cp|copy|mv|move|rsync|tar|zip|7z)\b[^|;&]*\bsecrets[\\/]'
           W = 'copying anything out of secrets/ moves it outside the ignored tree' },
        # The guard files themselves. The Edit branch below covers the Write and
        # Edit tools; this covers writing to them from a shell, which would
        # otherwise be an open door straight through the Edit rules.
        @{ P = '(?i)(?:>|>>)\s*\S*(?:\.gitignore|\.gitattributes|agent-guard\.ps1|audit-public\.ps1|setup-hooks\.ps1|_env\.ps1)\b'
           W = 'redirecting into a leak-prevention gate would rewrite it without review' },
        @{ P = "(?i)$start" + '(?:rm|del|mv|move)\b[^|;&]*(?:\.gitignore|\.githooks)\b'
           W = 'removing or moving a leak-prevention gate disarms it' }
    )

    foreach ($r in $rules) {
        if ($c -match $r.P) { Deny $r.W }
    }
    exit 0
}

if ($Kind -eq "Edit") {
    $path = [string]$toolInput.file_path
    if (-not $path) { exit 0 }
    $n = $path.Replace("\", "/")

    # An agent must not be able to edit its own guardrails. Not because the edit
    # would be malicious, but because a plausible-looking "fix" to a check that
    # is inconveniently failing is exactly how a gate gets quietly removed.
    if ($env:SYMBOGRAM_ALLOW_GUARD_EDIT -eq "1") { exit 0 }

    $protected = @(
        "/.gitignore", "/.gitattributes",
        "/.githooks/", "/tools/agent-guard.ps1", "/tools/audit-public.ps1",
        "/tools/_env.ps1", "/tools/setup-hooks.ps1", "/tools/claude-settings.seed.json",
        "/.claude/settings.json"
    )
    foreach ($p in $protected) {
        if ($n -like "*$p*") {
            # Parenthesised: `Deny "a" + "b"` passes three positional arguments
            # to Deny, not one concatenated string.
            Deny (("$([IO.Path]::GetFileName($path)) is a leak-prevention gate. ") +
                  "Changing it needs the user's explicit say-so (SYMBOGRAM_ALLOW_GUARD_EDIT=1).")
        }
    }

    # Writing into secrets/ is how a secret gets duplicated somewhere less
    # protected; writing into local/ is fine and expected.
    if ($n -match "/secrets/") { Deny "writes into secrets/ must be made by the user, not an agent" }
    exit 0
}

exit 0
