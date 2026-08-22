$ErrorActionPreference = 'Stop'

# Codex writes hook payloads as UTF-8. Set the redirected-input decoder before
# PowerShell lazily enumerates `$input`, otherwise non-ASCII text follows the
# active Windows console code page and arrives mojibaked.
[Console]::InputEncoding = [System.Text.UTF8Encoding]::new($false)

function Invoke-CmuxRequest {
    param(
        [Parameter(Mandatory)]
        [string] $PipeName,

        [Parameter(Mandatory)]
        [hashtable] $Request
    )

    $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
        '.',
        $PipeName,
        [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::None)

    try {
        $pipe.Connect(350)
        $encoding = [System.Text.UTF8Encoding]::new($false)
        $writer = [System.IO.StreamWriter]::new($pipe, $encoding, 1024, $true)
        $reader = [System.IO.StreamReader]::new($pipe, $encoding, $false, 1024, $true)
        try {
            $writer.AutoFlush = $true
            $writer.WriteLine(($Request | ConvertTo-Json -Depth 8 -Compress))
            $line = $reader.ReadLine()
            if ([string]::IsNullOrWhiteSpace($line)) {
                return $null
            }
            return $line | ConvertFrom-Json
        }
        finally {
            $reader.Dispose()
            $writer.Dispose()
        }
    }
    finally {
        $pipe.Dispose()
    }
}

function Get-CmuxPipeNames {
    $prefix = '\\.\pipe\'
    foreach ($path in [System.IO.Directory]::EnumerateFiles($prefix)) {
        $name = $path.Substring($prefix.Length)
        if ($name -like 'LOCAL\cmux-terminal-*') {
            $name
        }
    }
}

function Normalize-Directory {
    param([string] $Path)

    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ''
    }

    try {
        return [System.IO.Path]::GetFullPath($Path).TrimEnd([char[]]'\/')
    }
    catch {
        return $Path.TrimEnd([char[]]'\/')
    }
}

function Repair-HookTextEncoding {
    param([string] $Text)

    if ([string]::IsNullOrEmpty($Text)) {
        return $Text
    }

    try {
        # On Windows, `pwsh -File` can materialize redirected UTF-8 stdin through
        # the active ANSI code page before the script starts. Reverse that
        # transformation only when both conversions are lossless. Correctly
        # decoded non-ASCII text fails strict UTF-8 decoding and is left intact.
        $codePage = [System.Globalization.CultureInfo]::CurrentCulture.TextInfo.ANSICodePage
        $legacyEncoding = [System.Text.Encoding]::GetEncoding(
            $codePage,
            [System.Text.EncoderFallback]::ExceptionFallback,
            [System.Text.DecoderFallback]::ExceptionFallback)
        $strictUtf8 = [System.Text.UTF8Encoding]::new($false, $true)
        $repaired = $strictUtf8.GetString($legacyEncoding.GetBytes($Text))
        if ($repaired -ne $Text) {
            return $repaired
        }
    }
    catch {
        # The text was not a lossless ANSI mis-decoding of UTF-8.
    }

    return $Text
}

function Get-ConclusionTitle {
    param([string] $Message)

    $lines = $Message -split "`r?`n"
    $headingPattern = '^\s*(?:#{1,6}\s*)?(?:结论|本轮结论|总结|最终方案|实现结果|结果|建议|Conclusion|Summary|Result)\s*[:：]?\s*$'
    for ($index = 0; $index -lt $lines.Count; $index++) {
        if ($lines[$index] -match $headingPattern) {
            for ($next = $index + 1; $next -lt $lines.Count; $next++) {
                if (-not [string]::IsNullOrWhiteSpace($lines[$next])) {
                    $candidate = $lines[$next]
                    break
                }
            }
            break
        }
    }

    if ([string]::IsNullOrWhiteSpace($candidate)) {
        $candidate = $lines |
            Where-Object { -not [string]::IsNullOrWhiteSpace($_) } |
            Select-Object -First 1
    }

    $candidate = ($candidate -replace '^\s*(?:[-*+]\s+|\d+[.)]\s+|#{1,6}\s*)', '').Trim(' ', '`', '*', '_', '#')
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        $candidate = 'Codex 结论'
    }
    if ($candidate.Length -gt 72) {
        $candidate = $candidate.Substring(0, 71) + '…'
    }
    return $candidate
}

try {
    # When PowerShell starts a script with `-File`, it may materialize redirected
    # stdin as pipeline input before the script can read Console.In. Support both
    # invocation modes because Codex launches this hook through `pwsh -File`.
    $pipelineInput = @($input)
    $rawInput = if ($pipelineInput.Count -gt 0) {
        $pipelineInput -join [Environment]::NewLine
    }
    else {
        [Console]::In.ReadToEnd()
    }
    if ([string]::IsNullOrWhiteSpace($rawInput)) {
        exit 0
    }

    $event = $rawInput | ConvertFrom-Json
    $message = Repair-HookTextEncoding ([string] $event.last_assistant_message)
    if ([string]::IsNullOrWhiteSpace($message) -or
        [string]::IsNullOrWhiteSpace([string] $event.session_id) -or
        [string]::IsNullOrWhiteSpace([string] $event.turn_id)) {
        exit 0
    }

    $message = ($message -replace '[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F-\u009F]', '').Trim()
    if ($message.Length -gt 8000) {
        $message = $message.Substring(0, 7999) + '…'
    }

    $eventDirectory = Normalize-Directory (Repair-HookTextEncoding ([string] $event.cwd))
    $eventTerminalSessionId = ([string] $env:WT_SESSION).Trim(' ', '{', '}')
    $bestMatch = $null
    $bestScore = -1

    $pipeNames = @(Get-CmuxPipeNames)
    foreach ($pipeName in $pipeNames) {
        try {
            $listResponse = Invoke-CmuxRequest -PipeName $pipeName -Request @{
                version = 1
                id = "codex-list-$([guid]::NewGuid().ToString('N'))"
                method = 'workspace.list'
                params = @{}
            }

            foreach ($workspace in @($listResponse.result.workspaces)) {
                $workspaceDirectory = Normalize-Directory ([string] $workspace.workingDirectory)
                if ([string]::IsNullOrWhiteSpace($workspaceDirectory)) {
                    continue
                }

                $score = -1
                $terminalSessionMatches = $false
                if (-not [string]::IsNullOrWhiteSpace($eventTerminalSessionId)) {
                    foreach ($workspaceTerminalSessionId in @($workspace.terminalSessionIds)) {
                        if ([string]::Equals(
                                ([string] $workspaceTerminalSessionId).Trim(' ', '{', '}'),
                                $eventTerminalSessionId,
                                [System.StringComparison]::OrdinalIgnoreCase)) {
                            $terminalSessionMatches = $true
                            break
                        }
                    }
                }
                if ($terminalSessionMatches) {
                    $score = 1000
                }
                elseif ([string]::Equals($workspaceDirectory, $eventDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
                    $score = 100
                }
                elseif ($eventDirectory.StartsWith($workspaceDirectory + '\', [System.StringComparison]::OrdinalIgnoreCase)) {
                    $score = 60
                }
                if ($score -ge 0 -and [bool] $workspace.selected) {
                    $score += 20
                }

                if ($score -gt $bestScore) {
                    $bestScore = $score
                    $bestMatch = [pscustomobject]@{
                        PipeName = $pipeName
                        WorkspaceId = [string] $workspace.id
                    }
                }
            }
        }
        catch {
            continue
        }
    }

    if ($null -eq $bestMatch) {
        exit 0
    }

    $request = @{
        version = 1
        id = "codex-conclusion-$([guid]::NewGuid().ToString('N'))"
        method = 'conclusion.create'
        params = @{
            workspaceId = $bestMatch.WorkspaceId
            sessionId = [string] $event.session_id
            turnId = [string] $event.turn_id
            title = Get-ConclusionTitle $message
            body = $message
            model = [string] $event.model
        }
    }
    $null = Invoke-CmuxRequest -PipeName $bestMatch.PipeName -Request $request
}
catch {
    # Hooks must never prevent Codex from completing a turn. The MVP intentionally
    # has no persistent fallback, so delivery failures are silently dropped.
}

exit 0
