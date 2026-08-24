param(
    [Parameter(ValueFromPipeline = $true)]
    [AllowEmptyString()]
    [string] $PipelineHookInput,

    [Parameter(Mandatory)]
    [ValidateSet('UserPromptSubmit', 'Stop', 'SessionStart', 'SessionEnd')]
    [string] $EventName,

    [string] $InboxDirectory,

    [ValidateRange(50, 5000)]
    [int] $ConnectTimeoutMilliseconds = 350,

    [ValidateRange(50, 5000)]
    [int] $ResponseTimeoutMilliseconds = 750,

    # Total live-event limit across the mutex-protected UUID queue and the
    # fixed emergency slots reserved from it.
    [ValidateRange(1, 4096)]
    [int] $MaxInboxFiles = 256,

    # Total live-event bytes across those same two pools.
    [ValidateRange(65536, 1073741824)]
    [long] $MaxInboxBytes = 8388608,

    [ValidateRange(1, 64)]
    [int] $MaxReplayItems = 4,

    [ValidateRange(1, 32)]
    [int] $MaxPipeCandidates = 8,

    [switch] $SuppressStopResponse,

    # This marker makes user-level installs safely identifiable during uninstall.
    [string] $InstallId = '',

    # Explicit self-test seam. Installed hook commands never pass this value;
    # unlike an environment switch, it cannot be inherited accidentally.
    [ValidateSet('', 'fail-inbox-write-aba')]
    [string] $TestDiagnostics = ''
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$script:Utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$script:Utf8Strict = [System.Text.UTF8Encoding]::new($false, $true)
$script:MaximumTextCharacters = 8000
$script:MaximumRequestBytes = 60 * 1024
$script:MaximumEmergencySlots = 32
$script:EmergencySlotFileDivisor = 8
$script:InboxMutexName = 'Local\Microsoft.WindowsTerminal.WorkspaceHistoryInbox.v1'
# Hard wall-clock budgets: discovery spans every candidate, current delivery is
# exactly one request, and replay spans every item. The 3.4-second outer cap
# leaves process/JSON/filesystem headroom below the host's five-second timeout.
$script:DiscoveryBudgetMilliseconds = 900
$script:DeliveryBudgetMilliseconds = 1000
$script:ReplayBudgetMilliseconds = 1200
$script:HookWorkBudgetMilliseconds = 3400
$script:InboxLockWaitMilliseconds = 25
$script:MaximumPipeScanEntries = 512
$script:MaximumInboxScanEntries = 512
$script:MaximumCapacityScanEntries = 4096 + $script:MaximumEmergencySlots + 1
$script:TestDiagnostics = $TestDiagnostics

# Codex writes hook input as UTF-8. Set both encodings before stdin is consumed,
# and keep stdout deterministic: only Stop emits the required empty JSON object.
[Console]::InputEncoding = $script:Utf8NoBom
[Console]::OutputEncoding = $script:Utf8NoBom

# Stop has a five-second host timeout. Emit its required response before any
# discovery, pipe I/O, or inbox I/O so even an outer timeout cannot suppress it.
$isStop = $EventName -eq 'Stop'
if ($isStop -and -not $SuppressStopResponse) {
    [Console]::Out.WriteLine('{}')
    [Console]::Out.Flush()
}

function New-OperationDeadline {
    param(
        [Parameter(Mandatory)][int] $BudgetMilliseconds,
        [Parameter(Mandatory)][datetime] $OuterDeadlineUtc
    )

    $deadline = [datetime]::UtcNow.AddMilliseconds($BudgetMilliseconds)
    if ($OuterDeadlineUtc -lt $deadline) {
        return $OuterDeadlineUtc
    }
    return $deadline
}

function Get-RemainingMilliseconds {
    param([Parameter(Mandatory)][datetime] $DeadlineUtc)

    $remaining = ($DeadlineUtc - [datetime]::UtcNow).TotalMilliseconds
    if ($remaining -le 0) {
        return 0
    }
    if ($remaining -ge [int]::MaxValue) {
        return [int]::MaxValue
    }
    return [int] [Math]::Floor($remaining)
}

function Get-PropertyValue {
    param(
        [AllowNull()]
        [object] $Object,

        [Parameter(Mandatory)]
        [string] $Name
    )

    if ($null -eq $Object) {
        return $null
    }

    if ($Object -is [System.Collections.IDictionary]) {
        if ($Object.Contains($Name)) {
            return $Object[$Name]
        }
        return $null
    }

    $property = $Object.PSObject.Properties[$Name]
    if ($null -eq $property) {
        return $null
    }
    return $property.Value
}

function Test-HasProperty {
    param(
        [AllowNull()][object] $Object,
        [Parameter(Mandatory)][string] $Name
    )

    if ($null -eq $Object) {
        return $false
    }
    if ($Object -is [System.Collections.IDictionary]) {
        return $Object.Contains($Name)
    }
    return $null -ne $Object.PSObject.Properties[$Name]
}

function Test-AllowedProperties {
    param(
        [AllowNull()]
        [object] $Object,

        [Parameter(Mandatory)]
        [string[]] $Allowed
    )

    if ($null -eq $Object -or $Object -is [string] -or $Object -is [System.Array]) {
        return $false
    }

    $propertyNames = if ($Object -is [System.Collections.IDictionary]) {
        @($Object.Keys)
    }
    else {
        @($Object.PSObject.Properties.Name)
    }
    foreach ($propertyName in $propertyNames) {
        if ($Allowed -notcontains [string] $propertyName) {
            return $false
        }
    }
    return $true
}

function Repair-HookTextEncoding {
    param([AllowNull()][string] $Text)

    if ([string]::IsNullOrEmpty($Text)) {
        return $Text
    }

    try {
        # Some Windows pwsh -File launch paths materialize redirected UTF-8 via
        # the active ANSI code page. Reverse that conversion only if both sides
        # are lossless; correctly decoded Unicode is left untouched.
        $codePage = [System.Globalization.CultureInfo]::CurrentCulture.TextInfo.ANSICodePage
        $legacy = [System.Text.Encoding]::GetEncoding(
            $codePage,
            [System.Text.EncoderFallback]::ExceptionFallback,
            [System.Text.DecoderFallback]::ExceptionFallback)
        $candidate = $script:Utf8Strict.GetString($legacy.GetBytes($Text))
        if ($candidate -ne $Text) {
            return $candidate
        }
    }
    catch {
        # The input was not a lossless ANSI mis-decoding of UTF-8.
    }

    return $Text
}

function Limit-HookText {
    param(
        [AllowNull()][string] $Text,
        [int] $MaximumCharacters = $script:MaximumTextCharacters
    )

    $value = Repair-HookTextEncoding $Text
    if ([string]::IsNullOrWhiteSpace($value)) {
        return ''
    }

    # Preserve markdown line breaks and tabs, but never forward other control
    # characters to the pipe or durable inbox.
    $value = ($value -replace '[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F-\u009F]', '').Trim()
    if ($value.Length -le $MaximumCharacters) {
        return $value
    }

    $length = $MaximumCharacters - 1
    if ($length -gt 0 -and
        [char]::IsHighSurrogate($value[$length - 1]) -and
        $length -lt $value.Length -and
        [char]::IsLowSurrogate($value[$length])) {
        $length--
    }
    return $value.Substring(0, $length) + [char]0x2026
}

function Limit-KeyText {
    param(
        [AllowNull()][string] $Text,
        [int] $MaximumCharacters
    )

    $value = (Repair-HookTextEncoding $Text).Trim()
    if ([string]::IsNullOrWhiteSpace($value) -or
        $value.Length -gt $MaximumCharacters -or
        $value -match '[\u0000-\u001F\u007F-\u009F]') {
        return ''
    }
    return $value
}

function Normalize-GuidString {
    param([AllowNull()][string] $Value)

    $parsed = [guid]::Empty
    if (-not [guid]::TryParse(([string] $Value).Trim(), [ref] $parsed) -or $parsed -eq [guid]::Empty) {
        return ''
    }
    return $parsed.ToString('D')
}

function Normalize-PipeName {
    param([AllowNull()][string] $Value)

    $name = ([string] $Value).Trim()
    $prefix = '\\.\pipe\'
    if ($name.StartsWith($prefix, [System.StringComparison]::OrdinalIgnoreCase)) {
        $name = $name.Substring($prefix.Length)
    }

    if ($name.Length -gt 256 -or
        $name -match '[\u0000-\u001F\u007F-\u009F]' -or
        -not $name.StartsWith('LOCAL\cmux-terminal-', [System.StringComparison]::OrdinalIgnoreCase)) {
        return ''
    }
    return $name
}

function Normalize-Directory {
    param([AllowNull()][string] $Path)

    $value = Repair-HookTextEncoding $Path
    if ([string]::IsNullOrWhiteSpace($value)) {
        return ''
    }

    try {
        $fullPath = [System.IO.Path]::GetFullPath($value)
        $root = [System.IO.Path]::GetPathRoot($fullPath)
        if ([string]::Equals($fullPath, $root, [System.StringComparison]::OrdinalIgnoreCase)) {
            return $fullPath
        }
        return $fullPath.TrimEnd([char[]] '\/')
    }
    catch {
        return $value.Trim().TrimEnd([char[]] '\/')
    }
}

function Test-DirectoryDescendant {
    param(
        [string] $Path,
        [string] $Parent
    )

    if ([string]::IsNullOrWhiteSpace($Path) -or [string]::IsNullOrWhiteSpace($Parent)) {
        return $false
    }
    if ([string]::Equals($Path, $Parent, [System.StringComparison]::OrdinalIgnoreCase)) {
        return $false
    }

    $withSeparator = if ($Parent.EndsWith('\') -or $Parent.EndsWith('/')) {
        $Parent
    }
    else {
        $Parent + [System.IO.Path]::DirectorySeparatorChar
    }
    return $Path.StartsWith($withSeparator, [System.StringComparison]::OrdinalIgnoreCase)
}

function Get-CmuxPipeNames {
    param(
        [Parameter(Mandatory)][int] $MaximumCount,
        [Parameter(Mandatory)][datetime] $DeadlineUtc
    )

    if ($MaximumCount -le 0) {
        return
    }

    $pipePrefix = '\\.\pipe\'
    $emitted = 0
    $scanned = 0
    try {
        foreach ($path in [System.IO.Directory]::EnumerateFiles($pipePrefix)) {
            if ($emitted -ge $MaximumCount -or
                $scanned -ge $script:MaximumPipeScanEntries -or
                (Get-RemainingMilliseconds $DeadlineUtc) -le 0) {
                break
            }
            $scanned++
            $name = Normalize-PipeName $path
            if (-not [string]::IsNullOrWhiteSpace($name)) {
                $name
                $emitted++
            }
        }
    }
    catch {
        # No running Terminal automation server is a normal offline condition.
    }
}

function ConvertTo-CompactJson {
    param([Parameter(Mandatory)][object] $Value)

    return $Value | ConvertTo-Json -Depth 8 -Compress
}

function Invoke-CmuxRequest {
    param(
        [Parameter(Mandatory)][string] $PipeName,
        [Parameter(Mandatory)][object] $Request,
        [Parameter(Mandatory)][datetime] $DeadlineUtc
    )

    $json = ConvertTo-CompactJson $Request
    if ($script:Utf8NoBom.GetByteCount($json) -gt $script:MaximumRequestBytes) {
        throw [System.IO.InvalidDataException]::new('Workspace history request is too large.')
    }

    $pipe = [System.IO.Pipes.NamedPipeClientStream]::new(
        '.',
        $PipeName,
        [System.IO.Pipes.PipeDirection]::InOut,
        [System.IO.Pipes.PipeOptions]::Asynchronous)

    try {
        $remaining = Get-RemainingMilliseconds $DeadlineUtc
        if ($remaining -le 0) {
            throw [System.TimeoutException]::new('Workspace history pipe budget expired before connect.')
        }
        $pipe.Connect([Math]::Max(1, [Math]::Min($ConnectTimeoutMilliseconds, $remaining)))
        $writer = [System.IO.StreamWriter]::new($pipe, $script:Utf8NoBom, 1024, $true)
        $reader = [System.IO.StreamReader]::new($pipe, $script:Utf8NoBom, $false, 1024, $true)
        try {
            $writer.AutoFlush = $true
            $remaining = Get-RemainingMilliseconds $DeadlineUtc
            if ($remaining -le 0) {
                throw [System.TimeoutException]::new('Workspace history pipe budget expired before write.')
            }
            $writeTask = $writer.WriteLineAsync($json)
            if (-not $writeTask.Wait($remaining)) {
                throw [System.TimeoutException]::new('Workspace history request write timed out.')
            }
            $null = $writeTask.GetAwaiter().GetResult()

            $readTask = $reader.ReadLineAsync()
            $remaining = Get-RemainingMilliseconds $DeadlineUtc
            if ($remaining -le 0 -or
                -not $readTask.Wait([Math]::Max(1, [Math]::Min($ResponseTimeoutMilliseconds, $remaining)))) {
                throw [System.TimeoutException]::new('Workspace history ACK timed out.')
            }
            $line = $readTask.GetAwaiter().GetResult()
            if ([string]::IsNullOrWhiteSpace($line) -or
                $script:Utf8NoBom.GetByteCount($line) -gt $script:MaximumRequestBytes) {
                throw [System.IO.InvalidDataException]::new('Workspace history ACK is empty or too large.')
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

function Test-ResponseId {
    param(
        [AllowNull()][object] $Response,
        [Parameter(Mandatory)][object] $Request
    )

    return $null -ne $Response -and
        [string]::Equals(
            [string] (Get-PropertyValue $Response 'id'),
            [string] (Get-PropertyValue $Request 'id'),
            [System.StringComparison]::Ordinal)
}

function Test-AcceptedAck {
    param(
        [AllowNull()][object] $Response,
        [Parameter(Mandatory)][object] $Request
    )

    if (-not (Test-ResponseId -Response $Response -Request $Request)) {
        return $false
    }
    $result = Get-PropertyValue $Response 'result'
    $accepted = Get-PropertyValue $result 'accepted'
    return $accepted -is [bool] -and $accepted
}

function Get-WorkspaceListResponse {
    param(
        [Parameter(Mandatory)][string] $PipeName,
        [Parameter(Mandatory)][datetime] $DeadlineUtc
    )

    $request = [ordered]@{
        version = 1
        id = 'codex-workspace-list-' + [guid]::NewGuid().ToString('N')
        method = 'workspace.list'
        params = [ordered]@{}
    }
    try {
        $response = Invoke-CmuxRequest -PipeName $PipeName -Request $request -DeadlineUtc $DeadlineUtc
        if (Test-ResponseId -Response $response -Request $request) {
            return $response
        }
    }
    catch {
        # Discovery is best effort; durable delivery handles an offline server.
    }
    return $null
}

function Resolve-WorkspaceTarget {
    param(
        [AllowNull()][string] $EventDirectory,
        [Parameter(Mandatory)][datetime] $OuterDeadlineUtc
    )

    # These two explicit routing values are injected by Windows Terminal. Never
    # serialize the environment itself; only the validated workspace id enters
    # the protocol envelope.
    $explicitWorkspaceId = Normalize-GuidString ([Environment]::GetEnvironmentVariable('WT_WORKSPACE_ID', 'Process'))
    $explicitPipeName = Normalize-PipeName ([Environment]::GetEnvironmentVariable('WT_WORKSPACE_PIPE', 'Process'))
    $eventCwd = Normalize-Directory $EventDirectory
    $terminalSessionId = Normalize-GuidString ([Environment]::GetEnvironmentVariable('WT_SESSION', 'Process'))

    # A pane can be moved after its process environment was created. When a
    # WT_SESSION is available, inspect every live window (explicit pipe first)
    # before trusting potentially stale WT_WORKSPACE_* values.
    $discoveryDeadline = New-OperationDeadline -BudgetMilliseconds $script:DiscoveryBudgetMilliseconds -OuterDeadlineUtc $OuterDeadlineUtc
    $pipeNames = [System.Collections.Generic.List[string]]::new()
    $knownPipeNames = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
    if (-not [string]::IsNullOrWhiteSpace($explicitPipeName)) {
        $pipeNames.Add($explicitPipeName)
        $null = $knownPipeNames.Add($explicitPipeName)
    }
    $remainingCandidates = $MaxPipeCandidates - $pipeNames.Count
    foreach ($pipeName in @(Get-CmuxPipeNames -MaximumCount $remainingCandidates -DeadlineUtc $discoveryDeadline)) {
        if ($pipeNames.Count -ge $MaxPipeCandidates) {
            break
        }
        if ($knownPipeNames.Add($pipeName)) {
            $pipeNames.Add($pipeName)
        }
    }

    if ([string]::IsNullOrWhiteSpace($terminalSessionId) -and
        -not [string]::IsNullOrWhiteSpace($explicitWorkspaceId) -and
        -not [string]::IsNullOrWhiteSpace($explicitPipeName)) {
        return [pscustomobject]@{
            WorkspaceId = $explicitWorkspaceId
            PipeName = $explicitPipeName
        }
    }

    $snapshots = [System.Collections.Generic.List[object]]::new()
    foreach ($pipeName in $pipeNames) {
        if ((Get-RemainingMilliseconds $discoveryDeadline) -le 0) {
            break
        }
        $response = Get-WorkspaceListResponse -PipeName $pipeName -DeadlineUtc $discoveryDeadline
        if ($null -ne $response) {
            $workspaces = @((Get-PropertyValue (Get-PropertyValue $response 'result') 'workspaces'))
            $snapshots.Add([pscustomobject]@{
                PipeName = $pipeName
                Workspaces = $workspaces
            })

            # The explicit pipe is queried first. Once the live terminal session
            # is found, no lower-priority cwd scan or additional window query is
            # needed.
            if (-not [string]::IsNullOrWhiteSpace($terminalSessionId)) {
                foreach ($workspace in $workspaces) {
                    foreach ($candidateSession in @((Get-PropertyValue $workspace 'terminalSessionIds'))) {
                        $normalizedSession = Normalize-GuidString ([string] $candidateSession)
                        if ([string]::Equals($normalizedSession, $terminalSessionId, [System.StringComparison]::OrdinalIgnoreCase)) {
                            $actualWorkspaceId = Normalize-GuidString ([string] (Get-PropertyValue $workspace 'id'))
                            if (-not [string]::IsNullOrWhiteSpace($actualWorkspaceId)) {
                                return [pscustomobject]@{
                                    WorkspaceId = $actualWorkspaceId
                                    PipeName = $pipeName
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    if (-not [string]::IsNullOrWhiteSpace($terminalSessionId)) {
        # A live WT_SESSION is the authoritative pane identity. If none of the
        # available live snapshots contains it, routing by cwd or a stale pipe
        # could write history into another workspace, so fail closed. With no
        # live snapshot at all, retain only the injected id as an offline durable
        # key; the stale/unverified pipe is deliberately discarded.
        if ($snapshots.Count -eq 0 -and -not [string]::IsNullOrWhiteSpace($explicitWorkspaceId)) {
            return [pscustomobject]@{
                WorkspaceId = $explicitWorkspaceId
                PipeName = ''
            }
        }
        return $null
    }

    if (-not [string]::IsNullOrWhiteSpace($explicitWorkspaceId)) {
        foreach ($snapshot in $snapshots) {
            foreach ($workspace in @($snapshot.Workspaces)) {
                $candidateId = Normalize-GuidString ([string] (Get-PropertyValue $workspace 'id'))
                if ([string]::Equals($candidateId, $explicitWorkspaceId, [System.StringComparison]::OrdinalIgnoreCase)) {
                    return [pscustomobject]@{
                        WorkspaceId = $explicitWorkspaceId
                        PipeName = [string] $snapshot.PipeName
                    }
                }
            }
        }
    }

    $bestMatch = $null
    $bestScore = -1

    foreach ($snapshot in $snapshots) {
        foreach ($workspace in @($snapshot.Workspaces)) {
            $workspaceId = Normalize-GuidString ([string] (Get-PropertyValue $workspace 'id'))
            if ([string]::IsNullOrWhiteSpace($workspaceId)) {
                continue
            }

            $score = -1
            $workspaceDirectory = Normalize-Directory ([string] (Get-PropertyValue $workspace 'workingDirectory'))
            if (-not [string]::IsNullOrWhiteSpace($eventCwd) -and
                [string]::Equals($workspaceDirectory, $eventCwd, [System.StringComparison]::OrdinalIgnoreCase)) {
                $score = 100
            }
            elseif ($score -lt 0 -and (Test-DirectoryDescendant -Path $eventCwd -Parent $workspaceDirectory)) {
                $score = 60
            }

            if ($score -ge 0 -and (Get-PropertyValue $workspace 'selected') -is [bool] -and
                (Get-PropertyValue $workspace 'selected')) {
                $score += 20
            }
            if ($score -gt $bestScore) {
                $bestScore = $score
                $bestMatch = [pscustomobject]@{
                    WorkspaceId = $workspaceId
                    PipeName = [string] $snapshot.PipeName
                }
            }
        }
    }

    if ($null -ne $bestMatch) {
        return $bestMatch
    }

    if ($snapshots.Count -eq 0 -and -not [string]::IsNullOrWhiteSpace($explicitWorkspaceId)) {
        # Completely offline: retain the explicit id only as a durable routing
        # key. Never send immediately through an unverified, possibly stale pipe.
        return [pscustomobject]@{
            WorkspaceId = $explicitWorkspaceId
            PipeName = ''
        }
    }

    return $bestMatch
}

function Get-ConclusionTitle {
    param([AllowNull()][string] $Message)

    $lines = ([string] $Message) -split "`r?`n"
    $candidate = ''
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
    $candidate = ($candidate -replace '[\u0000-\u001F\u007F-\u009F]', ' ').Trim()
    if ([string]::IsNullOrWhiteSpace($candidate)) {
        $candidate = 'Codex 结论'
    }
    if ($candidate.Length -gt 72) {
        $length = 71
        if ([char]::IsHighSurrogate($candidate[$length - 1]) -and
            $length -lt $candidate.Length -and
            [char]::IsLowSurrogate($candidate[$length])) {
            $length--
        }
        $candidate = $candidate.Substring(0, $length) + [char]0x2026
    }
    return $candidate
}

function New-HistoryRequest {
    param(
        [Parameter(Mandatory)][object] $HookEvent,
        [Parameter(Mandatory)][string] $WorkspaceId
    )

    $sessionId = Limit-KeyText ([string] (Get-PropertyValue $HookEvent 'session_id')) 128
    $turnId = Limit-KeyText ([string] (Get-PropertyValue $HookEvent 'turn_id')) 128
    if ([string]::IsNullOrWhiteSpace($sessionId) -or [string]::IsNullOrWhiteSpace($turnId)) {
        return $null
    }

    $params = [ordered]@{
        eventId = [guid]::NewGuid().ToString('D')
        eventAt = [DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds()
        workspaceId = $WorkspaceId
        sessionId = $sessionId
        turnId = $turnId
    }

    if ($EventName -eq 'UserPromptSubmit') {
        $prompt = Limit-HookText ([string] (Get-PropertyValue $HookEvent 'prompt'))
        if ([string]::IsNullOrWhiteSpace($prompt)) {
            return $null
        }
        $params.event = 'prompt'
        $params.prompt = $prompt
    }
    elseif ($EventName -eq 'Stop') {
        $final = Limit-HookText ([string] (Get-PropertyValue $HookEvent 'last_assistant_message'))
        if ([string]::IsNullOrWhiteSpace($final)) {
            return $null
        }
        $params.event = 'final'
        $params.finalMarkdown = $final
        $params.title = Get-ConclusionTitle $final
    }
    else {
        return $null
    }

    $cwd = Normalize-Directory ([string] (Get-PropertyValue $HookEvent 'cwd'))
    if (-not [string]::IsNullOrWhiteSpace($cwd) -and $cwd.Length -le 4096 -and
        $cwd -notmatch '[\u0000-\u001F\u007F-\u009F]') {
        $params.cwd = $cwd
    }
    $model = Limit-KeyText ([string] (Get-PropertyValue $HookEvent 'model')) 128
    if (-not [string]::IsNullOrWhiteSpace($model)) {
        $params.model = $model
    }

    $request = [ordered]@{
        version = 1
        id = 'codex-history-' + [guid]::NewGuid().ToString('N')
        method = 'history.ingest'
        params = $params
    }

    $json = ConvertTo-CompactJson $request
    if ($script:Utf8NoBom.GetByteCount($json) -gt $script:MaximumRequestBytes) {
        return $null
    }
    return $request
}

function Test-HistoryRequest {
    param([AllowNull()][object] $Request)

    $version = Get-PropertyValue $Request 'version'
    if (-not (Test-AllowedProperties $Request @('version', 'id', 'method', 'params')) -or
        ($version -isnot [int] -and $version -isnot [long]) -or $version -ne 1 -or
        (Get-PropertyValue $Request 'method') -ne 'history.ingest') {
        return $false
    }

    $rawRequestId = Get-PropertyValue $Request 'id'
    if ($rawRequestId -isnot [string]) {
        return $false
    }
    $requestId = Limit-KeyText $rawRequestId 128
    if ([string]::IsNullOrWhiteSpace($requestId)) {
        return $false
    }

    $params = Get-PropertyValue $Request 'params'
    $allowedParams = @('eventId', 'eventAt', 'workspaceId', 'sessionId', 'turnId', 'event', 'cwd', 'prompt', 'finalMarkdown', 'title', 'model')
    foreach ($requiredString in @('eventId', 'workspaceId', 'sessionId', 'turnId', 'event')) {
        if ((Get-PropertyValue $params $requiredString) -isnot [string]) {
            return $false
        }
    }
    $eventIdText = [string] (Get-PropertyValue $params 'eventId')
    $eventAt = Get-PropertyValue $params 'eventAt'
    $workspaceIdText = [string] (Get-PropertyValue $params 'workspaceId')
    if (-not (Test-AllowedProperties $params $allowedParams) -or
        -not [string]::Equals($eventIdText, (Normalize-GuidString $eventIdText), [System.StringComparison]::Ordinal) -or
        ($eventAt -isnot [int] -and $eventAt -isnot [long]) -or
        [long] $eventAt -le 0 -or [long] $eventAt -gt 253402300799999 -or
        -not [string]::Equals($workspaceIdText, (Normalize-GuidString $workspaceIdText), [System.StringComparison]::Ordinal) -or
        [string]::IsNullOrWhiteSpace((Limit-KeyText ([string] (Get-PropertyValue $params 'sessionId')) 128)) -or
        [string]::IsNullOrWhiteSpace((Limit-KeyText ([string] (Get-PropertyValue $params 'turnId')) 128))) {
        return $false
    }

    if (Test-HasProperty $params 'cwd') {
        $cwdValue = Get-PropertyValue $params 'cwd'
        if ($cwdValue -isnot [string]) {
            return $false
        }
        $cwd = [string] $cwdValue
        if ([string]::IsNullOrWhiteSpace($cwd) -or $cwd.Length -gt 4096 -or
            $cwd -match '[\u0000-\u001F\u007F-\u009F]') {
            return $false
        }
    }
    if (Test-HasProperty $params 'model') {
        $modelValue = Get-PropertyValue $params 'model'
        if ($modelValue -isnot [string]) {
            return $false
        }
        $model = [string] $modelValue
        if ([string]::IsNullOrWhiteSpace($model) -or $model.Length -gt 128 -or
            $model -match '[\u0000-\u001F\u007F-\u009F]') {
            return $false
        }
    }

    $event = [string] (Get-PropertyValue $params 'event')
    $prompt = [string] (Get-PropertyValue $params 'prompt')
    $final = [string] (Get-PropertyValue $params 'finalMarkdown')
    $title = [string] (Get-PropertyValue $params 'title')
    if ($event -eq 'prompt') {
        if ((Get-PropertyValue $params 'prompt') -isnot [string] -or
            [string]::IsNullOrWhiteSpace($prompt) -or
            $prompt.Length -gt $script:MaximumTextCharacters -or
            $prompt -match '[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F-\u009F]' -or
            (Test-HasProperty $params 'finalMarkdown') -or
            (Test-HasProperty $params 'title')) {
            return $false
        }
    }
    elseif ($event -eq 'final') {
        if ((Get-PropertyValue $params 'finalMarkdown') -isnot [string] -or
            (Get-PropertyValue $params 'title') -isnot [string] -or
            [string]::IsNullOrWhiteSpace($final) -or
            $final.Length -gt $script:MaximumTextCharacters -or
            $final -match '[\u0000-\u0008\u000B\u000C\u000E-\u001F\u007F-\u009F]' -or
            [string]::IsNullOrWhiteSpace($title) -or
            $title.Length -gt 72 -or
            $title -match '[\u0000-\u001F\u007F-\u009F]' -or
            (Test-HasProperty $params 'prompt')) {
            return $false
        }
    }
    else {
        return $false
    }

    try {
        $json = ConvertTo-CompactJson $Request
        return $script:Utf8NoBom.GetByteCount($json) -le $script:MaximumRequestBytes
    }
    catch {
        return $false
    }
}

function Get-DefaultInboxDirectory {
    $localAppData = [Environment]::GetFolderPath([Environment+SpecialFolder]::LocalApplicationData)
    if ([string]::IsNullOrWhiteSpace($localAppData)) {
        return ''
    }
    return [System.IO.Path]::Combine($localAppData, 'Microsoft', 'Windows Terminal', 'workspace-history-inbox')
}

function Get-InboxCapacity {
    # MaxInboxFiles/MaxInboxBytes are total live-event limits. Emergency slots
    # are a reserved part of those limits, rather than capacity added on top.
    # The byte-derived cap keeps every reserved slot large enough for one
    # maximum-size request for every valid parameter combination.
    $slotFileLimit = [Math]::Min(
        $script:MaximumEmergencySlots,
        [Math]::Max(1, [int] [Math]::Floor($MaxInboxFiles / $script:EmergencySlotFileDivisor)))
    $slotByteLimit = [Math]::Max(1, [int] [Math]::Floor($MaxInboxBytes / $script:MaximumRequestBytes))
    $emergencySlots = [int] [Math]::Min($slotFileLimit, $slotByteLimit)
    $emergencyBytes = [long] $emergencySlots * [long] $script:MaximumRequestBytes

    return [pscustomobject]@{
        EmergencySlots = $emergencySlots
        EmergencyBytes = $emergencyBytes
        MainFiles = [int] ($MaxInboxFiles - $emergencySlots)
        MainBytes = [long] ($MaxInboxBytes - $emergencyBytes)
    }
}

function Get-InboxFileKind {
    param([Parameter(Mandatory)][string] $Name)

    if ($Name -cmatch '^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}\.json$') {
        return 'Main'
    }
    if ($Name -cmatch '^emergency-(0[0-9]|[12][0-9]|3[01])\.json$') {
        return 'Emergency'
    }
    return ''
}

function Get-EmergencySlotIndex {
    param([Parameter(Mandatory)][string] $Name)

    if ($Name -cmatch '^emergency-(0[0-9]|[12][0-9]|3[01])\.json$') {
        return [int] $Matches[1]
    }
    return -1
}

function Get-OwnedInboxFiles {
    param(
        [Parameter(Mandatory)][string] $Directory,
        [datetime] $DeadlineUtc = [datetime]::MaxValue
    )

    if (-not [System.IO.Directory]::Exists($Directory)) {
        return @()
    }

    $scanLimit = [Math]::Min($script:MaximumInboxScanEntries, $MaxInboxFiles + 64)
    $files = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
    $scanned = 0
    foreach ($path in [System.IO.Directory]::EnumerateFiles($Directory, '*', [System.IO.SearchOption]::TopDirectoryOnly)) {
        if ($scanned -ge $scanLimit -or (Get-RemainingMilliseconds $DeadlineUtc) -le 0) {
            break
        }
        $scanned++
        if (-not [string]::IsNullOrEmpty((Get-InboxFileKind ([System.IO.Path]::GetFileName($path))))) {
            $files.Add([System.IO.FileInfo]::new($path))
        }
    }
    return @($files | Sort-Object LastWriteTimeUtc, Name)
}

function Get-InboxCapacityState {
    param(
        [Parameter(Mandatory)][string] $Directory,
        [Parameter(Mandatory)][datetime] $DeadlineUtc
    )

    $mainFiles = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
    $emergencyFiles = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
    $complete = $false
    try {
        # The native *.json filter avoids paying for unrelated .tmp/.txt noise.
        # If enumeration cannot finish inside the deadline/count cap, Save fails
        # capacity rather than making any unverified main or emergency write.
        $scanned = 0
        foreach ($path in [System.IO.Directory]::EnumerateFiles($Directory, '*.json', [System.IO.SearchOption]::TopDirectoryOnly)) {
            if ($scanned -ge $script:MaximumCapacityScanEntries) {
                return [pscustomobject]@{
                    Complete = $false
                    MainFiles = @($mainFiles)
                    EmergencyFiles = @($emergencyFiles)
                }
            }
            $scanned++
            if ((Get-RemainingMilliseconds $DeadlineUtc) -le 0) {
                return [pscustomobject]@{
                    Complete = $false
                    MainFiles = @($mainFiles)
                    EmergencyFiles = @($emergencyFiles)
                }
            }
            $kind = Get-InboxFileKind ([System.IO.Path]::GetFileName($path))
            if ($kind -eq 'Main') {
                $mainFiles.Add([System.IO.FileInfo]::new($path))
            }
            elseif ($kind -eq 'Emergency') {
                $emergencyFiles.Add([System.IO.FileInfo]::new($path))
            }
        }
        $complete = $true
    }
    catch {
        $complete = $false
    }

    return [pscustomobject]@{
        Complete = $complete
        MainFiles = @($mainFiles)
        EmergencyFiles = @($emergencyFiles)
    }
}

function Invoke-WithInboxLock {
    param(
        [Parameter(Mandatory)][scriptblock] $Action,
        [ValidateRange(0, 1000)][int] $WaitMilliseconds = $script:InboxLockWaitMilliseconds
    )

    $mutex = $null
    $lockTaken = $false
    try {
        $mutex = [System.Threading.Mutex]::new($false, $script:InboxMutexName)
        try {
            $lockTaken = $mutex.WaitOne($WaitMilliseconds)
        }
        catch [System.Threading.AbandonedMutexException] {
            $lockTaken = $true
        }

        if ($lockTaken) {
            $null = & $Action
        }
    }
    finally {
        if ($lockTaken -and $null -ne $mutex) {
            try { $mutex.ReleaseMutex() } catch {}
        }
        if ($null -ne $mutex) {
            $mutex.Dispose()
        }
    }

    return $lockTaken
}

function Remove-CapacityInboxFile {
    param([Parameter(Mandatory)][System.IO.FileInfo] $File)

    $stream = $null
    try {
        # A fixed-slot producer uses FileShare.None. Opening for read therefore
        # proves this is a completed event, not an active writer that may grow.
        $stream = [System.IO.FileStream]::new(
            $File.FullName,
            [System.IO.FileMode]::Open,
            [System.IO.FileAccess]::Read,
            [System.IO.FileShare]::Read)
        $null = $stream.Length
        $stream.Dispose()
        $stream = $null

        # The caller owns the mutation mutex. The occupied path cannot be
        # claimed by another emergency writer between this check and deletion.
        [System.IO.File]::Delete($File.FullName)
        return (-not [System.IO.File]::Exists($File.FullName))
    }
    catch {
        return $false
    }
    finally {
        if ($null -ne $stream) {
            $stream.Dispose()
        }
    }
}

function Repair-EmergencyInboxCapacity {
    param(
        [Parameter(Mandatory)][string] $Directory,
        [Parameter(Mandatory)][datetime] $DeadlineUtc,
        [ValidateRange(0, 61440)][int] $ReserveBytes = 0
    )

    $capacity = Get-InboxCapacity
    if ($ReserveBytes -gt 0 -and ($capacity.EmergencySlots -le 0 -or $ReserveBytes -gt $script:MaximumRequestBytes)) {
        return $false
    }

    $state = Get-InboxCapacityState -Directory $Directory -DeadlineUtc $DeadlineUtc
    if (-not $state.Complete) {
        return $false
    }

    $remaining = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
    $outOfRange = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
    foreach ($file in @($state.EmergencyFiles)) {
        $slot = Get-EmergencySlotIndex $file.Name
        if ($slot -lt 0 -or $slot -ge $capacity.EmergencySlots) {
            $outOfRange.Add($file)
        }
        else {
            $remaining.Add($file)
        }
    }

    # Configuration may be lowered after higher-numbered slots were filled.
    # Retire those completed slots deterministically, oldest first. A sharing
    # violation means the old producer is still active, so this save must fail
    # rather than deleting it or claiming more capacity.
    foreach ($file in @($outOfRange | Sort-Object LastWriteTimeUtc, Name)) {
        if ((Get-RemainingMilliseconds $DeadlineUtc) -le 0 -or -not (Remove-CapacityInboxFile -File $file)) {
            return $false
        }
    }

    $targetFiles = $capacity.EmergencySlots
    $targetBytes = $capacity.EmergencyBytes
    if ($ReserveBytes -gt 0) {
        $targetFiles--
        $targetBytes -= $ReserveBytes
    }

    $files = @($remaining | Sort-Object LastWriteTimeUtc, Name)
    $totalBytes = [long] 0
    foreach ($file in $files) {
        try {
            $totalBytes += $file.Length
        }
        catch {
            return $false
        }
    }

    $index = 0
    while (($files.Count - $index) -gt $targetFiles -or $totalBytes -gt $targetBytes) {
        if ($index -ge $files.Count -or (Get-RemainingMilliseconds $DeadlineUtc) -le 0) {
            return $false
        }
        $file = $files[$index++]
        if (-not (Remove-CapacityInboxFile -File $file)) {
            return $false
        }
        $totalBytes -= $file.Length
    }

    return (($files.Count - $index) -le $targetFiles -and $totalBytes -le $targetBytes)
}

function Test-EmergencyFallbackCapacity {
    param(
        [Parameter(Mandatory)][string] $Directory,
        [Parameter(Mandatory)][datetime] $DeadlineUtc
    )

    $capacity = Get-InboxCapacity
    $state = Get-InboxCapacityState -Directory $Directory -DeadlineUtc $DeadlineUtc
    if (-not $state.Complete) {
        return $false
    }

    $mainFiles = @($state.MainFiles)
    if ($mainFiles.Count -gt $capacity.MainFiles) {
        return $false
    }
    $mainBytes = [long] 0
    foreach ($file in $mainFiles) {
        try { $mainBytes += $file.Length } catch { return $false }
    }
    if ($mainBytes -gt $capacity.MainBytes) {
        return $false
    }

    $emergencyFiles = @($state.EmergencyFiles)
    if ($emergencyFiles.Count -ge $capacity.EmergencySlots) {
        return $false
    }
    foreach ($file in $emergencyFiles) {
        $slot = Get-EmergencySlotIndex $file.Name
        if ($slot -lt 0 -or $slot -ge $capacity.EmergencySlots) {
            return $false
        }
    }

    # Each occupied/active slot is charged its full 60-KiB reservation. This is
    # safe even if a producer grows after the snapshot and makes the byte proof
    # equivalent to the fixed file-count proof.
    $worstCaseEmergencyBytes = [long] ($emergencyFiles.Count + 1) * [long] $script:MaximumRequestBytes
    return $worstCaseEmergencyBytes -le $capacity.EmergencyBytes
}

function Trim-MainInbox {
    param(
        [Parameter(Mandatory)][string] $Directory,
        [datetime] $DeadlineUtc = [datetime]::MaxValue,
        [ValidateRange(0, 61440)][int] $IncomingBytes = 0
    )

    $capacity = Get-InboxCapacity
    if ($IncomingBytes -gt $capacity.MainBytes -or ($IncomingBytes -gt 0 -and $capacity.MainFiles -le 0)) {
        return $false
    }

    $state = Get-InboxCapacityState -Directory $Directory -DeadlineUtc $DeadlineUtc
    if (-not $state.Complete) {
        return $false
    }

    $files = @($state.MainFiles | Sort-Object LastWriteTimeUtc, Name)
    $totalBytes = [long] 0
    foreach ($file in $files) {
        $totalBytes += $file.Length
    }

    $targetFiles = $capacity.MainFiles
    if ($IncomingBytes -gt 0) {
        $targetFiles--
    }
    $targetBytes = $capacity.MainBytes - $IncomingBytes
    $index = 0
    while (($files.Count - $index) -gt $targetFiles -or $totalBytes -gt $targetBytes) {
        if ($index -ge $files.Count -or (Get-RemainingMilliseconds $DeadlineUtc) -le 0) {
            return $false
        }
        $file = $files[$index++]
        $removed = $false
        try {
            [System.IO.File]::Delete($file.FullName)
            $removed = -not [System.IO.File]::Exists($file.FullName)
        }
        catch {}
        if (-not $removed) {
            return $false
        }
        $totalBytes -= $file.Length
    }

    return (($files.Count - $index) -le $targetFiles -and $totalBytes -le $targetBytes)
}

function Trim-Inbox {
    param(
        [Parameter(Mandatory)][string] $Directory,
        [datetime] $DeadlineUtc = [datetime]::MaxValue
    )

    if (-not (Repair-EmergencyInboxCapacity -Directory $Directory -DeadlineUtc $DeadlineUtc)) {
        return $false
    }
    return (Trim-MainInbox -Directory $Directory -DeadlineUtc $DeadlineUtc)
}

function Move-InvalidInboxFile {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][datetime] $DeadlineUtc,
        [AllowNull()][string] $ExpectedContents = $null
    )

    try {
        $remaining = Get-RemainingMilliseconds $DeadlineUtc
        if ($remaining -le 0) {
            return
        }
        $null = Invoke-WithInboxLock -WaitMilliseconds ([Math]::Min($script:InboxLockWaitMilliseconds, $remaining)) -Action {
            if (-not [System.IO.File]::Exists($Path)) {
                return
            }
            if ($null -ne $ExpectedContents) {
                try {
                    $currentContents = [System.IO.File]::ReadAllText($Path, $script:Utf8Strict)
                    if (-not [string]::Equals($currentContents, $ExpectedContents, [System.StringComparison]::Ordinal)) {
                        return
                    }
                }
                catch {
                    # An exclusive producer or a replacement race is transient.
                    return
                }
            }

            $invalidDirectory = [System.IO.Path]::Combine($InboxDirectory, '.invalid')
            $null = [System.IO.Directory]::CreateDirectory($invalidDirectory)
            $destination = [System.IO.Path]::Combine($invalidDirectory, [System.IO.Path]::GetFileName($Path))
            if ([System.IO.File]::Exists($destination)) {
                $destination = [System.IO.Path]::Combine(
                    $invalidDirectory,
                    ([System.IO.Path]::GetFileNameWithoutExtension($Path) + '-' + [guid]::NewGuid().ToString('N') + '.json'))
            }
            [System.IO.File]::Move($Path, $destination)

            # Keep the audit quarantine bounded as well: at most 32 entries and
            # at most 1 MiB, with oldest evidence evicted first.
            $invalidList = [System.Collections.Generic.List[System.IO.FileInfo]]::new()
            $invalidScanned = 0
            foreach ($invalidPath in [System.IO.Directory]::EnumerateFiles($invalidDirectory, '*', [System.IO.SearchOption]::TopDirectoryOnly)) {
                if ($invalidScanned -ge 96 -or (Get-RemainingMilliseconds $DeadlineUtc) -le 0) {
                    break
                }
                $invalidScanned++
                if ([System.IO.Path]::GetExtension($invalidPath) -eq '.json') {
                    $invalidList.Add([System.IO.FileInfo]::new($invalidPath))
                }
            }
            $invalidFiles = @($invalidList | Sort-Object LastWriteTimeUtc, Name)
            $invalidBytes = [long] 0
            foreach ($file in $invalidFiles) {
                $invalidBytes += $file.Length
            }
            $invalidIndex = 0
            while (($invalidFiles.Count - $invalidIndex) -gt 32 -or $invalidBytes -gt 1048576) {
                if ($invalidIndex -ge $invalidFiles.Count -or (Get-RemainingMilliseconds $DeadlineUtc) -le 0) {
                    break
                }
                $file = $invalidFiles[$invalidIndex++]
                $invalidBytes -= $file.Length
                try { [System.IO.File]::Delete($file.FullName) } catch {}
            }
        }
    }
    catch {
        # Quarantine is diagnostic only and must not affect hook completion.
    }
}

function Remove-FailedExclusiveInboxWrite {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][byte[]] $ExpectedBytes,
        [Parameter(Mandatory)][System.IO.FileStream] $Stream
    )

    $state = [pscustomobject]@{ Disposed = $false; Removed = $false }
    try {
        $null = Invoke-WithInboxLock -WaitMilliseconds $script:InboxLockWaitMilliseconds -Action {
            try {
                # Keep the exclusive handle open until the mutation mutex is
                # owned. This closes the dispose/delete ABA window; acquiring
                # the same mutex is reentrant for a main-queue writer.
                $Stream.Dispose()
                $state.Disposed = $true

                if ($script:TestDiagnostics -eq 'fail-inbox-write-aba') {
                    Start-Sleep -Milliseconds 300
                }

                if (-not [System.IO.File]::Exists($Path)) {
                    return
                }
                $length = [System.IO.FileInfo]::new($Path).Length
                if ($length -gt $ExpectedBytes.Length) {
                    return
                }
                $actual = [System.IO.File]::ReadAllBytes($Path)
                if ($actual.Length -ne $length) {
                    return
                }
                for ($index = 0; $index -lt $actual.Length; $index++) {
                    if ($actual[$index] -ne $ExpectedBytes[$index]) {
                        return
                    }
                }

                # A failed sequential write can only leave a prefix of this
                # event. Exact prefix verification makes the cleanup identity-
                # safe even if a nonconforming consumer touched the path.
                [System.IO.File]::Delete($Path)
                $state.Removed = -not [System.IO.File]::Exists($Path)
            }
            catch {
                # Retaining a bounded partial file is safer than deleting an
                # identity that could not be proven.
            }
        }
    }
    catch {}
    finally {
        if (-not $state.Disposed) {
            try { $Stream.Dispose() } catch {}
        }
    }

    return $state.Removed
}

function Write-ExclusiveInboxFile {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][byte[]] $Bytes
    )

    $stream = $null
    $created = $false
    $succeeded = $false
    try {
        # CreateNew is the atomic capacity claim. FileShare.None prevents replay
        # and the native importer from observing a partially written payload.
        $stream = [System.IO.FileStream]::new(
            $Path,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::None,
            4096,
            [System.IO.FileOptions]::WriteThrough)
        $created = $true
        if ($script:TestDiagnostics -eq 'fail-inbox-write-aba') {
            throw 'Injected inbox write failure after fixed-path claim.'
        }
        $stream.Write($Bytes, 0, $Bytes.Length)
        $stream.Flush($true)
        $succeeded = $true
    }
    catch {
        $succeeded = $false
    }
    finally {
        if ($created -and -not $succeeded -and $null -ne $stream) {
            $null = Remove-FailedExclusiveInboxWrite -Path $Path -ExpectedBytes $Bytes -Stream $stream
            $stream = $null
        }
        elseif ($null -ne $stream) {
            $stream.Dispose()
        }
    }

    return $succeeded
}

function Save-EmergencyInboxRequest {
    param(
        [Parameter(Mandatory)][byte[]] $Bytes,
        [Parameter(Mandatory)][string] $EventId
    )

    if ($Bytes.Length -gt $script:MaximumRequestBytes) {
        return $false
    }

    $capacity = Get-InboxCapacity
    if ($capacity.EmergencySlots -le 0) {
        return $false
    }

    # Rotate the first probe by eventId to reduce collisions between concurrent
    # producers. Every possible destination is still one of the fixed slots.
    $hexPrefix = $EventId.Replace('-', '').Substring(0, 8)
    $start = [int] ([Convert]::ToUInt32($hexPrefix, 16) % [uint32] $capacity.EmergencySlots)
    for ($offset = 0; $offset -lt $capacity.EmergencySlots; $offset++) {
        $slot = ($start + $offset) % $capacity.EmergencySlots
        $path = [System.IO.Path]::Combine($InboxDirectory, ('emergency-{0:D2}.json' -f $slot))
        if (Write-ExclusiveInboxFile -Path $path -Bytes $Bytes) {
            return $true
        }
    }

    # Every reserved slot contains (or is being filled by) another event. This
    # is explicit capacity exhaustion, not loss caused merely by mutex latency.
    return $false
}

function Save-InboxRequest {
    param(
        [Parameter(Mandatory)][object] $Request,
        [Parameter(Mandatory)][datetime] $OuterDeadlineUtc
    )

    if (-not (Test-HistoryRequest $Request) -or [string]::IsNullOrWhiteSpace($InboxDirectory)) {
        return
    }

    $json = ConvertTo-CompactJson $Request
    $bytes = $script:Utf8NoBom.GetBytes($json)
    if ($bytes.Length -gt $script:MaximumRequestBytes -or $bytes.Length -gt $MaxInboxBytes) {
        return
    }

    $null = [System.IO.Directory]::CreateDirectory($InboxDirectory)

    # eventId is generated locally by New-HistoryRequest, then canonicalized
    # again here. Main UUID publication happens only while holding the mutex so
    # its reserved quota remains hard under arbitrarily many concurrent hooks.
    $eventId = Normalize-GuidString ([string] (Get-PropertyValue (Get-PropertyValue $Request 'params') 'eventId'))
    if ([string]::IsNullOrWhiteSpace($eventId)) {
        return $false
    }

    $finalPath = [System.IO.Path]::Combine($InboxDirectory, ($eventId + '.json'))
    $capacity = Get-InboxCapacity
    $saveState = [pscustomobject]@{ Saved = $false; Governed = $false }
    $remaining = Get-RemainingMilliseconds $OuterDeadlineUtc
    if ($remaining -gt 0) {
        $lockWait = [Math]::Min($script:InboxLockWaitMilliseconds, $remaining)
        $lockTaken = Invoke-WithInboxLock -WaitMilliseconds $lockWait -Action {
            if (-not (Repair-EmergencyInboxCapacity -Directory $InboxDirectory -DeadlineUtc $OuterDeadlineUtc)) {
                return
            }
            if (-not (Trim-MainInbox -Directory $InboxDirectory -DeadlineUtc $OuterDeadlineUtc)) {
                return
            }
            $saveState.Governed = $true

            if ([System.IO.File]::Exists($finalPath)) {
                $saveState.Saved = $true
                return
            }

            $requiresEmergency = $capacity.MainFiles -le 0 -or $bytes.Length -gt $capacity.MainBytes
            if (-not $requiresEmergency -and (Trim-MainInbox -Directory $InboxDirectory -DeadlineUtc $OuterDeadlineUtc -IncomingBytes $bytes.Length)) {
                $saveState.Saved = Write-ExclusiveInboxFile -Path $finalPath -Bytes $bytes
            }
            elseif ($requiresEmergency) {
                # Make room for this exact fallback while the mutation lock is
                # held. Oldest completed emergency evidence is evicted first;
                # an active writer makes governance fail closed.
                $saveState.Governed = Repair-EmergencyInboxCapacity -Directory $InboxDirectory -DeadlineUtc $OuterDeadlineUtc -ReserveBytes $bytes.Length
            }
        }
        if ($lockTaken -and $saveState.Saved) {
            return $true
        }
        if ($lockTaken -and -not $saveState.Governed) {
            return $false
        }
    }

    # A bounded set of lock-free slots absorbs ordinary short mutex contention.
    # Their count and worst-case bytes were subtracted from the main quota.
    if (-not (Test-EmergencyFallbackCapacity -Directory $InboxDirectory -DeadlineUtc $OuterDeadlineUtc)) {
        return $false
    }
    return (Save-EmergencyInboxRequest -Bytes $bytes -EventId $eventId)
}

function Test-InboxFileMatchesRequest {
    param(
        [Parameter(Mandatory)][System.IO.FileInfo] $File,
        [Parameter(Mandatory)][object] $Request
    )

    $kind = Get-InboxFileKind $File.Name
    if ([string]::IsNullOrEmpty($kind)) {
        return $false
    }

    $params = Get-PropertyValue $Request 'params'
    $eventId = Normalize-GuidString ([string] (Get-PropertyValue $params 'eventId'))
    if ([string]::IsNullOrEmpty($eventId)) {
        return $false
    }
    if ($kind -eq 'Emergency') {
        # Slots carry no identity in their fixed filename; the canonical payload
        # eventId remains the durable receipt key.
        return $true
    }

    return [string]::Equals($File.BaseName, $eventId, [System.StringComparison]::Ordinal)
}

function Remove-AcknowledgedInboxFile {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][string] $ExpectedContents,
        [Parameter(Mandatory)][datetime] $DeadlineUtc
    )

    $remaining = Get-RemainingMilliseconds $DeadlineUtc
    if ($remaining -le 0) {
        return $false
    }

    $state = [pscustomobject]@{ Removed = $false }
    $lockTaken = Invoke-WithInboxLock -WaitMilliseconds ([Math]::Min($script:InboxLockWaitMilliseconds, $remaining)) -Action {
        if (-not [System.IO.File]::Exists($Path)) {
            $state.Removed = $true
            return
        }
        try {
            # Fixed slot names are reusable. Revalidate the exact bytes while
            # holding the mutation mutex so an ACK for A can never delete B.
            $currentContents = [System.IO.File]::ReadAllText($Path, $script:Utf8Strict)
            if ([string]::Equals($currentContents, $ExpectedContents, [System.StringComparison]::Ordinal)) {
                [System.IO.File]::Delete($Path)
                $state.Removed = -not [System.IO.File]::Exists($Path)
            }
        }
        catch {
            $state.Removed = $false
        }
    }
    return ($lockTaken -and $state.Removed)
}

function Replay-Inbox {
    param(
        [AllowNull()][object] $Target,
        [Parameter(Mandatory)][datetime] $OuterDeadlineUtc
    )

    if ($null -eq $Target -or
        [string]::IsNullOrWhiteSpace([string] $Target.WorkspaceId) -or
        [string]::IsNullOrWhiteSpace([string] $Target.PipeName) -or
        [string]::IsNullOrWhiteSpace($InboxDirectory) -or
        -not [System.IO.Directory]::Exists($InboxDirectory)) {
        return
    }

    $replayDeadline = New-OperationDeadline -BudgetMilliseconds $script:ReplayBudgetMilliseconds -OuterDeadlineUtc $OuterDeadlineUtc
    $files = @(Get-OwnedInboxFiles -Directory $InboxDirectory -DeadlineUtc $replayDeadline)
    $replayed = 0
    foreach ($file in $files) {
        if ($replayed -ge $MaxReplayItems -or (Get-RemainingMilliseconds $replayDeadline) -le 0) {
            break
        }

        $request = $null
        $json = $null
        try {
            $json = [System.IO.File]::ReadAllText($file.FullName, $script:Utf8Strict)
        }
        catch {
            # An emergency producer may still own this fixed slot exclusively.
            # A transient read failure must be retried on a later SessionStart.
            continue
        }

        try {
            $request = $json | ConvertFrom-Json
        }
        catch {
            Move-InvalidInboxFile -Path $file.FullName -DeadlineUtc $replayDeadline -ExpectedContents $json
            continue
        }

        if (-not (Test-HistoryRequest $request) -or -not (Test-InboxFileMatchesRequest -File $file -Request $request)) {
            Move-InvalidInboxFile -Path $file.FullName -DeadlineUtc $replayDeadline -ExpectedContents $json
            continue
        }

        $params = Get-PropertyValue $request 'params'
        $workspaceId = Normalize-GuidString ([string] (Get-PropertyValue $params 'workspaceId'))
        if (-not [string]::Equals($workspaceId, [string] $Target.WorkspaceId, [System.StringComparison]::OrdinalIgnoreCase)) {
            continue
        }

        $replayed++
        try {
            $response = Invoke-CmuxRequest -PipeName ([string] $Target.PipeName) -Request $request -DeadlineUtc $replayDeadline
            if (Test-AcceptedAck -Response $response -Request $request) {
                # Delete only the exact bytes explicitly ACKed by the server.
                $null = Remove-AcknowledgedInboxFile -Path $file.FullName -ExpectedContents $json -DeadlineUtc $replayDeadline
            }
            else {
                break
            }
        }
        catch {
            break
        }
    }

    try {
        $remaining = Get-RemainingMilliseconds $replayDeadline
        if ($remaining -gt 0) {
            $null = Invoke-WithInboxLock -WaitMilliseconds ([Math]::Min($script:InboxLockWaitMilliseconds, $remaining)) -Action {
                $null = Trim-Inbox -Directory $InboxDirectory -DeadlineUtc $replayDeadline
            }
        }
    }
    catch {
        # Inbox maintenance must never affect the Codex turn.
    }
}

try {
    # SessionEnd is intentionally a no-op. In particular, it never starts a
    # replay under the host's three-second lifecycle deadline.
    if ($EventName -ne 'SessionEnd') {
        $outerDeadline = [datetime]::UtcNow.AddMilliseconds($script:HookWorkBudgetMilliseconds)
        if ([string]::IsNullOrWhiteSpace($InboxDirectory)) {
            $InboxDirectory = Get-DefaultInboxDirectory
        }

        # Support both pwsh -File pipeline binding and direct Console.In.
        $rawInput = if (-not [string]::IsNullOrWhiteSpace($PipelineHookInput)) {
            $PipelineHookInput
        }
        else {
            [Console]::In.ReadToEnd()
        }

        if (-not [string]::IsNullOrWhiteSpace($rawInput)) {
            $hookEvent = $rawInput | ConvertFrom-Json
            $actualEventName = [string] (Get-PropertyValue $hookEvent 'hook_event_name')
            if ([string]::Equals($actualEventName, $EventName, [System.StringComparison]::Ordinal)) {
                $eventDirectory = [string] (Get-PropertyValue $hookEvent 'cwd')
                $target = Resolve-WorkspaceTarget -EventDirectory $eventDirectory -OuterDeadlineUtc $outerDeadline

                if ($EventName -eq 'UserPromptSubmit' -or $EventName -eq 'Stop') {
                    if ($null -ne $target -and -not [string]::IsNullOrWhiteSpace([string] $target.WorkspaceId)) {
                        $request = New-HistoryRequest -HookEvent $hookEvent -WorkspaceId ([string] $target.WorkspaceId)
                        if ($null -ne $request) {
                            $delivered = $false
                            if (-not [string]::IsNullOrWhiteSpace([string] $target.PipeName)) {
                                try {
                                    $deliveryDeadline = New-OperationDeadline -BudgetMilliseconds $script:DeliveryBudgetMilliseconds -OuterDeadlineUtc $outerDeadline
                                    $response = Invoke-CmuxRequest -PipeName ([string] $target.PipeName) -Request $request -DeadlineUtc $deliveryDeadline
                                    $delivered = Test-AcceptedAck -Response $response -Request $request
                                }
                                catch {
                                    $delivered = $false
                                }
                            }

                            if (-not $delivered) {
                                $null = Save-InboxRequest -Request $request -OuterDeadlineUtc $outerDeadline
                            }
                        }
                    }
                }
                elseif ($EventName -eq 'SessionStart') {
                    # Replay belongs only to SessionStart. A current prompt/final
                    # never inherits old requests and therefore has one pipe cap.
                    Replay-Inbox -Target $target -OuterDeadlineUtc $outerDeadline
                }
            }
        }
    }
}
catch {
    # Telemetry/history delivery must never block or alter a Codex turn.
}
finally {}

exit 0
