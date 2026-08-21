$ErrorActionPreference = 'Stop'

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
    $rawInput = [Console]::In.ReadToEnd()
    if ([string]::IsNullOrWhiteSpace($rawInput)) {
        exit 0
    }

    $event = $rawInput | ConvertFrom-Json
    $message = [string] $event.last_assistant_message
    if ([string]::IsNullOrWhiteSpace($message) -or
        [string]::IsNullOrWhiteSpace([string] $event.session_id) -or
        [string]::IsNullOrWhiteSpace([string] $event.turn_id)) {
        exit 0
    }

    $message = ($message -replace '[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F-\u009F]', '').Trim()
    if ($message.Length -gt 8000) {
        $message = $message.Substring(0, 7999) + '…'
    }

    $eventDirectory = Normalize-Directory ([string] $event.cwd)
    $bestMatch = $null
    $bestScore = -1

    foreach ($pipeName in Get-CmuxPipeNames) {
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
                if ([string]::Equals($workspaceDirectory, $eventDirectory, [System.StringComparison]::OrdinalIgnoreCase)) {
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
