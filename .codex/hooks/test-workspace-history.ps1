$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
Set-StrictMode -Version Latest

$hookScript = [System.IO.Path]::Combine($PSScriptRoot, 'workspace-history.ps1')
$installScript = [System.IO.Path]::Combine($PSScriptRoot, 'install-workspace-history.ps1')
$uninstallScript = [System.IO.Path]::Combine($PSScriptRoot, 'uninstall-workspace-history.ps1')
$repositoryHooks = [System.IO.Path]::GetFullPath([System.IO.Path]::Combine($PSScriptRoot, '..', 'hooks.json'))
$pwsh = (Get-Command pwsh -ErrorAction Stop).Source
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$temporaryBase = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$testRoot = [System.IO.Path]::Combine($temporaryBase, 'wt-workspace-history-test-' + [guid]::NewGuid().ToString('N'))
$null = [System.IO.Directory]::CreateDirectory($testRoot)
$jobs = [System.Collections.Generic.List[object]]::new()
$assertions = 0

function Assert-True {
    param(
        [bool] $Condition,
        [Parameter(Mandatory)][string] $Message
    )

    if (-not $Condition) {
        throw "ASSERTION FAILED: $Message"
    }
    $script:assertions++
}

function Assert-Equal {
    param(
        [AllowNull()][object] $Actual,
        [AllowNull()][object] $Expected,
        [Parameter(Mandatory)][string] $Message
    )

    if (-not [object]::Equals($Actual, $Expected)) {
        throw "ASSERTION FAILED: $Message (expected '$Expected', got '$Actual')"
    }
    $script:assertions++
}

function Assert-SetEqual {
    param(
        [string[]] $Actual,
        [string[]] $Expected,
        [Parameter(Mandatory)][string] $Message
    )

    $actualSorted = @($Actual | Sort-Object -Unique)
    $expectedSorted = @($Expected | Sort-Object -Unique)
    $difference = @(Compare-Object $actualSorted $expectedSorted)
    Assert-True ($difference.Count -eq 0) $Message
}

function New-TestPipeName {
    return 'LOCAL\cmux-terminal-hooktest-' + [guid]::NewGuid().ToString('N')
}

function Start-FakeTerminalServer {
    param(
        [Parameter(Mandatory)][string] $PipeName,
        [object[]] $Workspaces = @(),
        [ValidateRange(1, 8)][int] $ExpectedConnections = 1,
        [ValidateRange(0, 10000)][int] $ResponseDelayMilliseconds = 0
    )

    $workspaceJson = if (@($Workspaces).Count -eq 0) {
        '[]'
    }
    else {
        @($Workspaces) | ConvertTo-Json -Depth 8 -Compress
    }
    $job = Start-Job -ArgumentList $PipeName, $workspaceJson, $ExpectedConnections, $ResponseDelayMilliseconds -ScriptBlock {
        param($Name, $WorkspaceJson, $ConnectionCount, $ResponseDelay)

        $ErrorActionPreference = 'Stop'
        $encoding = [System.Text.UTF8Encoding]::new($false)
        $workspaces = @($WorkspaceJson | ConvertFrom-Json)
        $requests = [System.Collections.Generic.List[string]]::new()
        $server = [System.IO.Pipes.NamedPipeServerStream]::new(
            $Name,
            [System.IO.Pipes.PipeDirection]::InOut,
            1,
            [System.IO.Pipes.PipeTransmissionMode]::Byte,
            [System.IO.Pipes.PipeOptions]::Asynchronous)
        try {
            for ($index = 0; $index -lt $ConnectionCount; $index++) {
                $server.WaitForConnection()
                $reader = [System.IO.StreamReader]::new($server, $encoding, $false, 1024, $true)
                $writer = [System.IO.StreamWriter]::new($server, $encoding, 1024, $true)
                try {
                    $writer.AutoFlush = $true
                    $line = $reader.ReadLine()
                    $requests.Add($line)
                    $request = $line | ConvertFrom-Json
                    if ($request.method -eq 'workspace.list') {
                        $response = [ordered]@{
                            version = 1
                            id = $request.id
                            result = [ordered]@{ workspaces = $workspaces }
                        }
                    }
                    elseif ($request.method -eq 'history.ingest') {
                        $response = [ordered]@{
                            version = 1
                            id = $request.id
                            result = [ordered]@{ accepted = $true }
                        }
                    }
                    else {
                        $response = [ordered]@{
                            version = 1
                            id = $request.id
                            error = [ordered]@{ code = 'unsupported_method' }
                        }
                    }
                    if ($ResponseDelay -gt 0) {
                        Start-Sleep -Milliseconds $ResponseDelay
                    }
                    $writer.WriteLine(($response | ConvertTo-Json -Depth 8 -Compress))
                }
                finally {
                    $writer.Dispose()
                    $reader.Dispose()
                }

                if ($index + 1 -lt $ConnectionCount) {
                    $server.Disconnect()
                }
            }
        }
        finally {
            $server.Dispose()
        }
        return $requests.ToArray()
    }
    $script:jobs.Add($job)

    $fullPipePath = '\\.\pipe\' + $PipeName
    $visible = $false
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        try {
            $visible = [bool] (@([System.IO.Directory]::EnumerateFiles('\\.\pipe\')) -contains $fullPipePath)
        }
        catch {
            $visible = $false
        }
        if ($visible) {
            break
        }
        Start-Sleep -Milliseconds 25
    }
    Assert-True $visible "Fake pipe did not become visible: $PipeName"
    return $job
}

function Receive-FakeTerminalRequests {
    param([Parameter(Mandatory)][object] $Job)

    $completed = Wait-Job -Job $Job -Timeout 15
    Assert-True ($null -ne $completed) "Fake pipe server timed out: $($Job.Id)"
    $lines = @(Receive-Job -Job $Job -ErrorAction Stop)
    Remove-Job -Job $Job -Force
    $null = $script:jobs.Remove($Job)
    return @($lines | ForEach-Object { [string] $_ })
}

function Wait-TestJobSuccess {
    param(
        [Parameter(Mandatory)][object] $Job,
        [Parameter(Mandatory)][string] $Description,
        [int] $TimeoutSeconds = 20
    )

    $completed = Wait-Job -Job $Job -Timeout $TimeoutSeconds
    Assert-True ($null -ne $completed) "$Description timed out."
    $reason = if ($null -ne $Job.ChildJobs[0].JobStateInfo.Reason) {
        $Job.ChildJobs[0].JobStateInfo.Reason.ToString()
    }
    else {
        ''
    }
    Assert-Equal ([string] $Job.State) 'Completed' "$Description failed: $reason"
    $null = Receive-Job -Job $Job -ErrorAction SilentlyContinue -InformationAction SilentlyContinue 6>$null
    Remove-Job -Job $Job -Force
    $null = $script:jobs.Remove($Job)
}

function Wait-TestTransactionJob {
    param(
        [Parameter(Mandatory)][object] $Job,
        [Parameter(Mandatory)][string] $Description,
        [int] $TimeoutSeconds = 30
    )

    $completed = Wait-Job -Job $Job -Timeout $TimeoutSeconds
    Assert-True ($null -ne $completed) "$Description timed out."
    Assert-Equal ([string] $Job.State) 'Completed' "$Description job did not complete."
    $result = @(Receive-Job -Job $Job -ErrorAction SilentlyContinue -InformationAction SilentlyContinue 6>$null)
    Remove-Job -Job $Job -Force
    $null = $script:jobs.Remove($Job)
    Assert-True ($result.Count -gt 0) "$Description did not return a transaction result."
    return $result[-1]
}

function Invoke-HookProcess {
    param(
        [Parameter(Mandatory)][string] $EventName,
        [Parameter(Mandatory)][object] $Payload,
        [Parameter(Mandatory)][string] $InboxDirectory,
        [AllowNull()][string] $WorkspaceId,
        [AllowNull()][string] $PipeName,
        [AllowNull()][string] $TerminalSessionId,
        [int] $MaxInboxFiles = 256,
        [long] $MaxInboxBytes = 8388608,
        [int] $MaxReplayItems = 4,
        [int] $MaxPipeCandidates = 8,
        [int] $ConnectTimeoutMilliseconds = 100,
        [int] $ResponseTimeoutMilliseconds = 750,
        [string] $TestDiagnostics = ''
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $pwsh
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    $startInfo.StandardInputEncoding = $utf8NoBom
    $startInfo.StandardOutputEncoding = $utf8NoBom
    $startInfo.StandardErrorEncoding = $utf8NoBom
    foreach ($argument in @(
        '-NoProfile', '-NonInteractive', '-File', $hookScript,
        '-EventName', $EventName,
        '-InboxDirectory', $InboxDirectory,
        '-ConnectTimeoutMilliseconds', [string] $ConnectTimeoutMilliseconds,
        '-ResponseTimeoutMilliseconds', [string] $ResponseTimeoutMilliseconds,
        '-MaxInboxFiles', [string] $MaxInboxFiles,
        '-MaxInboxBytes', [string] $MaxInboxBytes,
        '-MaxReplayItems', [string] $MaxReplayItems,
        '-MaxPipeCandidates', [string] $MaxPipeCandidates)) {
        $startInfo.ArgumentList.Add($argument)
    }
    if (-not [string]::IsNullOrEmpty($TestDiagnostics)) {
        $startInfo.ArgumentList.Add('-TestDiagnostics')
        $startInfo.ArgumentList.Add($TestDiagnostics)
    }

    foreach ($name in @('WT_WORKSPACE_ID', 'WT_WORKSPACE_PIPE', 'WT_SESSION')) {
        $null = $startInfo.Environment.Remove($name)
    }
    $null = $startInfo.Environment.Remove('WT_WORKSPACE_HISTORY_TEST_DIAGNOSTICS')
    if ($null -ne $WorkspaceId) { $startInfo.Environment['WT_WORKSPACE_ID'] = $WorkspaceId }
    if ($null -ne $PipeName) { $startInfo.Environment['WT_WORKSPACE_PIPE'] = $PipeName }
    if ($null -ne $TerminalSessionId) { $startInfo.Environment['WT_SESSION'] = $TerminalSessionId }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    $stopwatch = [System.Diagnostics.Stopwatch]::new()
    try {
        $stopwatch.Start()
        Assert-True $process.Start() 'Failed to start hook process.'
        $stdoutTask = $process.StandardOutput.ReadToEndAsync()
        $stderrTask = $process.StandardError.ReadToEndAsync()
        $json = if ($Payload -is [string]) { $Payload } else { $Payload | ConvertTo-Json -Depth 10 -Compress }
        $process.StandardInput.WriteLine($json)
        $process.StandardInput.Close()

        if (-not $process.WaitForExit(7000)) {
            $process.Kill($true)
            throw "Hook process timed out for $EventName"
        }
        $stopwatch.Stop()
        $stdout = $stdoutTask.GetAwaiter().GetResult()
        $stderr = $stderrTask.GetAwaiter().GetResult()
        return [pscustomobject]@{
            ExitCode = $process.ExitCode
            Stdout = $stdout
            Stderr = $stderr
            ElapsedMilliseconds = $stopwatch.ElapsedMilliseconds
        }
    }
    finally {
        $process.Dispose()
    }
}

function Get-JsonFiles {
    param([Parameter(Mandatory)][string] $Directory)

    if (-not [System.IO.Directory]::Exists($Directory)) {
        return @()
    }
    return @(Get-ChildItem -LiteralPath $Directory -File -Filter '*.json')
}

function Write-TestAtomicUtf8 {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][string] $Text
    )

    $directory = [System.IO.Path]::GetDirectoryName($Path)
    $temporaryPath = [System.IO.Path]::Combine($directory, [guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [System.IO.File]::WriteAllText($temporaryPath, $Text, $utf8NoBom)
        [System.IO.File]::Move($temporaryPath, $Path, $true)
    }
    finally {
        if ([System.IO.File]::Exists($temporaryPath)) {
            [System.IO.File]::Delete($temporaryPath)
        }
    }
}

try {
    foreach ($requiredScript in @($hookScript, $installScript, $uninstallScript, $repositoryHooks)) {
        Assert-True ([System.IO.File]::Exists($requiredScript)) "Missing required file: $requiredScript"
    }

    $configuration = [System.IO.File]::ReadAllText($repositoryHooks, $utf8NoBom) | ConvertFrom-Json
    Assert-SetEqual @($configuration.hooks.PSObject.Properties.Name) @('UserPromptSubmit', 'Stop', 'SessionStart', 'SessionEnd') 'Repository hooks.json must register all four events.'
    Assert-True ([int] $configuration.hooks.SessionEnd[0].hooks[0].timeout -le 3) 'SessionEnd timeout must not exceed the documented 3-second maximum.'
    Assert-True ([string] $configuration.hooks.SessionEnd[0].hooks[0].command -match 'exit 0' -and [string] $configuration.hooks.SessionEnd[0].hooks[0].command -notmatch 'workspace-history|git') 'Repository SessionEnd must be a true no-op.'
    Assert-True ([string] $configuration.hooks.Stop[0].hooks[0].command -match "WriteLine\('\{\}'\)") 'Stop wrapper must have a JSON fallback when the script is unavailable.'
    Assert-True ([string] $configuration.hooks.Stop[0].hooks[0].command -match 'SuppressStopResponse') 'Stop wrapper must emit before work without duplicating the script response.'

    # Invalid input must still leave Stop with exactly one valid empty object.
    $invalidInbox = [System.IO.Path]::Combine($testRoot, 'invalid-input-inbox')
    $invalidResult = Invoke-HookProcess -EventName Stop -Payload '{not-json' -InboxDirectory $invalidInbox -WorkspaceId $null -PipeName $null -TerminalSessionId $null
    Assert-Equal $invalidResult.ExitCode 0 'Stop must be fail-open.'
    Assert-Equal $invalidResult.Stdout.Trim() '{}' 'Stop must always emit valid empty JSON.'
    Assert-True ([string]::IsNullOrEmpty($invalidResult.Stderr)) 'Stop must not leak diagnostics to stderr.'

    # Explicit routing sends only the privacy allowlist and validates ACK ids.
    $promptPipe = New-TestPipeName
    $promptWorkspace = [guid]::NewGuid().ToString('D')
    $promptServer = Start-FakeTerminalServer -PipeName $promptPipe
    $promptInbox = [System.IO.Path]::Combine($testRoot, 'prompt-inbox')
    $promptPayload = [ordered]@{
        session_id = 'session-prompt'
        turn_id = 'turn-prompt'
        cwd = $testRoot
        hook_event_name = 'UserPromptSubmit'
        model = 'gpt-test'
        prompt = "你好, privacy test"
        transcript_path = 'C:\secret\transcript.jsonl'
        token_count = 987654
        env = @{ SECRET = 'do-not-forward' }
        tool_output = 'do-not-forward'
    }
    $promptResult = Invoke-HookProcess -EventName UserPromptSubmit -Payload $promptPayload -InboxDirectory $promptInbox -WorkspaceId $promptWorkspace -PipeName $promptPipe -TerminalSessionId $null
    Assert-Equal $promptResult.ExitCode 0 'UserPromptSubmit hook failed.'
    Assert-True ([string]::IsNullOrEmpty($promptResult.Stdout)) 'UserPromptSubmit must not inject model-visible stdout.'
    Assert-True ([string]::IsNullOrEmpty($promptResult.Stderr)) 'UserPromptSubmit emitted stderr.'
    $promptLines = @(Receive-FakeTerminalRequests $promptServer)
    Assert-Equal $promptLines.Count 1 'Explicit prompt route should make one request.'
    $promptRequest = $promptLines[0] | ConvertFrom-Json
    Assert-Equal $promptRequest.method 'history.ingest' 'Prompt used the wrong method.'
    Assert-SetEqual @($promptRequest.PSObject.Properties.Name) @('version', 'id', 'method', 'params') 'Envelope contains unexpected fields.'
    Assert-SetEqual @($promptRequest.params.PSObject.Properties.Name) @('eventId', 'eventAt', 'workspaceId', 'sessionId', 'turnId', 'event', 'cwd', 'prompt', 'model') 'Prompt params violate the privacy allowlist.'
    Assert-Equal $promptRequest.params.event 'prompt' 'Prompt event discriminator is wrong.'
    Assert-Equal $promptRequest.params.prompt '你好, privacy test' 'Unicode prompt was corrupted.'
    Assert-True ($promptRequest.params.eventId -cmatch '^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$') 'eventId must be canonical lowercase UUID.'
    Assert-True ([long] $promptRequest.params.eventAt -gt 0 -and [long] $promptRequest.params.eventAt -le 253402300799999) 'eventAt must be a bounded Unix-millisecond timestamp.'

    # Stop sends finalMarkdown plus a mandatory title, truncating safely at 8000.
    $finalPipe = New-TestPipeName
    $finalWorkspace = [guid]::NewGuid().ToString('D')
    $finalServer = Start-FakeTerminalServer -PipeName $finalPipe
    $longFinal = ('界' * 8100)
    $finalPayload = [ordered]@{
        session_id = 'session-final'
        turn_id = 'turn-final'
        cwd = $testRoot
        hook_event_name = 'Stop'
        model = ('m' * 140)
        last_assistant_message = $longFinal
    }
    $finalResult = Invoke-HookProcess -EventName Stop -Payload $finalPayload -InboxDirectory ([System.IO.Path]::Combine($testRoot, 'final-inbox')) -WorkspaceId $finalWorkspace -PipeName $finalPipe -TerminalSessionId $null
    Assert-Equal $finalResult.Stdout.Trim() '{}' 'Successful Stop must emit empty JSON.'
    $finalLines = @(Receive-FakeTerminalRequests $finalServer)
    $finalRequest = $finalLines[0] | ConvertFrom-Json
    Assert-SetEqual @($finalRequest.params.PSObject.Properties.Name) @('eventId', 'eventAt', 'workspaceId', 'sessionId', 'turnId', 'event', 'cwd', 'finalMarkdown', 'title') 'Final params violate the privacy allowlist.'
    Assert-Equal $finalRequest.params.event 'final' 'Final event discriminator is wrong.'
    Assert-Equal $finalRequest.params.finalMarkdown.Length 8000 'Final markdown must be capped at 8000 UTF-16 characters.'
    Assert-True $finalRequest.params.finalMarkdown.EndsWith([char]0x2026) 'Truncated final markdown needs an ellipsis.'
    Assert-True (-not [string]::IsNullOrWhiteSpace($finalRequest.params.title)) 'Final title is mandatory.'
    Assert-True ($finalRequest.params.title.Length -le 72) 'Final title exceeds its cap.'
    Assert-True ($null -eq $finalRequest.params.PSObject.Properties['model']) 'Oversized model must be omitted, not truncated ambiguously.'

    # A server that accepts the pipe but withholds its ACK cannot consume the
    # host's full five seconds. Stop emits {} first, caps the request, and spools.
    $slowStopPipe = New-TestPipeName
    $slowStopWorkspace = [guid]::NewGuid().ToString('D')
    $null = Start-FakeTerminalServer -PipeName $slowStopPipe -ResponseDelayMilliseconds 5000
    $slowStopInbox = [System.IO.Path]::Combine($testRoot, 'slow-stop-inbox')
    $slowStopPayload = [ordered]@{
        session_id = 'session-slow-stop'
        turn_id = 'turn-slow-stop'
        cwd = $testRoot
        hook_event_name = 'Stop'
        last_assistant_message = 'bounded stop delivery'
    }
    $slowStopResult = Invoke-HookProcess -EventName Stop -Payload $slowStopPayload -InboxDirectory $slowStopInbox -WorkspaceId $slowStopWorkspace -PipeName $slowStopPipe -TerminalSessionId $null -ConnectTimeoutMilliseconds 5000 -ResponseTimeoutMilliseconds 5000
    Assert-Equal $slowStopResult.Stdout.Trim() '{}' 'Timed-out Stop lost its required JSON response.'
    Assert-True ($slowStopResult.ElapsedMilliseconds -lt 5000) 'Stop exceeded the five-second host deadline under a stalled ACK.'
    Assert-Equal @(Get-JsonFiles $slowStopInbox).Count 1 'Timed-out Stop was not durably queued.'

    # Discovery has its own total budget independent of the configured per-pipe
    # timeouts; an offline authoritative pane still queues by injected id.
    $slowDiscoveryPipe = New-TestPipeName
    $slowDiscoveryWorkspace = [guid]::NewGuid().ToString('D')
    $null = Start-FakeTerminalServer -PipeName $slowDiscoveryPipe -ResponseDelayMilliseconds 5000
    $slowDiscoveryInbox = [System.IO.Path]::Combine($testRoot, 'slow-discovery-inbox')
    $slowDiscoveryPayload = [ordered]@{
        session_id = 'session-slow-discovery'
        turn_id = 'turn-slow-discovery'
        cwd = $testRoot
        hook_event_name = 'Stop'
        last_assistant_message = 'bounded discovery'
    }
    $slowDiscoveryResult = Invoke-HookProcess -EventName Stop -Payload $slowDiscoveryPayload -InboxDirectory $slowDiscoveryInbox -WorkspaceId $slowDiscoveryWorkspace -PipeName $slowDiscoveryPipe -TerminalSessionId ([guid]::NewGuid().ToString('D')) -MaxPipeCandidates 1 -ConnectTimeoutMilliseconds 5000 -ResponseTimeoutMilliseconds 5000
    Assert-Equal $slowDiscoveryResult.Stdout.Trim() '{}' 'Discovery-timeout Stop lost its required JSON response.'
    Assert-True ($slowDiscoveryResult.ElapsedMilliseconds -lt 5000) 'Total discovery budget exceeded the Stop deadline.'
    Assert-Equal @(Get-JsonFiles $slowDiscoveryInbox).Count 1 'Fully offline discovery did not retain the injected durable key.'

    # Offline delivery lands atomically in the bounded shared format, then a
    # SessionStart replay removes it only after accepted=true for the same id.
    $replayPipe = New-TestPipeName
    $replayWorkspace = [guid]::NewGuid().ToString('D')
    $replayInbox = [System.IO.Path]::Combine($testRoot, 'replay-inbox')
    $offlinePayload = [ordered]@{
        session_id = 'session-replay'
        turn_id = 'turn-replay'
        cwd = $testRoot
        hook_event_name = 'UserPromptSubmit'
        model = 'gpt-test'
        prompt = 'persist me'
    }
    $offlineResult = Invoke-HookProcess -EventName UserPromptSubmit -Payload $offlinePayload -InboxDirectory $replayInbox -WorkspaceId $replayWorkspace -PipeName $replayPipe -TerminalSessionId $null
    Assert-Equal $offlineResult.ExitCode 0 'Offline prompt hook failed open incorrectly.'
    $queuedFiles = @(Get-JsonFiles $replayInbox)
    Assert-Equal $queuedFiles.Count 1 'Offline event was not durably queued.'
    Assert-Equal @(Get-ChildItem -LiteralPath $replayInbox -File -Filter '*.tmp').Count 0 'Atomic inbox left a temporary file behind.'
    $queuedRequest = [System.IO.File]::ReadAllText($queuedFiles[0].FullName, $utf8NoBom) | ConvertFrom-Json
    Assert-Equal $queuedFiles[0].BaseName $queuedRequest.params.eventId 'Inbox stem must equal the locally generated eventId.'

    # A successful current event must not opportunistically replay older rows.
    $currentOnlyServer = Start-FakeTerminalServer -PipeName $replayPipe
    $currentOnlyPayload = [ordered]@{
        session_id = 'session-replay'
        turn_id = 'turn-current-only'
        cwd = $testRoot
        hook_event_name = 'UserPromptSubmit'
        prompt = 'deliver only this request'
    }
    $currentOnlyResult = Invoke-HookProcess -EventName UserPromptSubmit -Payload $currentOnlyPayload -InboxDirectory $replayInbox -WorkspaceId $replayWorkspace -PipeName $replayPipe -TerminalSessionId $null
    Assert-Equal $currentOnlyResult.ExitCode 0 'Current-only prompt failed.'
    $currentOnlyLines = @(Receive-FakeTerminalRequests $currentOnlyServer)
    Assert-Equal $currentOnlyLines.Count 1 'A current event unexpectedly replayed an inbox item.'
    Assert-Equal (($currentOnlyLines[0] | ConvertFrom-Json).params.turnId) 'turn-current-only' 'The current request was not the sole delivery.'
    $currentQueueFiles = @(Get-JsonFiles $replayInbox)
    $currentQueueTurns = @($currentQueueFiles | ForEach-Object {
        ([System.IO.File]::ReadAllText($_.FullName, $utf8NoBom) | ConvertFrom-Json).params.turnId
    }) -join ','
    Assert-Equal $currentQueueFiles.Count 1 "A current event consumed the replay queue. elapsed=$($currentOnlyResult.ElapsedMilliseconds) turns=$currentQueueTurns"

    $replayServer = Start-FakeTerminalServer -PipeName $replayPipe
    $sessionStartPayload = [ordered]@{
        session_id = 'session-replay'
        cwd = $testRoot
        hook_event_name = 'SessionStart'
        model = 'gpt-test'
        source = 'resume'
    }
    $replayResult = Invoke-HookProcess -EventName SessionStart -Payload $sessionStartPayload -InboxDirectory $replayInbox -WorkspaceId $replayWorkspace -PipeName $replayPipe -TerminalSessionId $null
    Assert-True ([string]::IsNullOrEmpty($replayResult.Stdout)) 'SessionStart must not inject stdout.'
    $replayLines = @(Receive-FakeTerminalRequests $replayServer)
    $replayedRequest = $replayLines[0] | ConvertFrom-Json
    Assert-Equal $replayedRequest.id $queuedRequest.id 'Replay must retain the original request id.'
    Assert-Equal ([long] $replayedRequest.params.eventAt) ([long] $queuedRequest.params.eventAt) 'Replay must retain the original stable eventAt.'
    Assert-Equal @(Get-JsonFiles $replayInbox).Count 0 'ACKed inbox entry was not cleaned up.'

    # Replay has a separate total cap, so even MaxReplayItems cannot multiply a
    # stalled ACK into a hook timeout.
    $slowReplayPipe = New-TestPipeName
    $slowReplayWorkspace = [guid]::NewGuid().ToString('D')
    $slowReplayInbox = [System.IO.Path]::Combine($testRoot, 'slow-replay-inbox')
    $slowReplayOfflinePayload = [ordered]@{
        session_id = 'session-slow-replay'
        turn_id = 'turn-slow-replay'
        cwd = $testRoot
        hook_event_name = 'UserPromptSubmit'
        prompt = 'retain after replay timeout'
    }
    $null = Invoke-HookProcess -EventName UserPromptSubmit -Payload $slowReplayOfflinePayload -InboxDirectory $slowReplayInbox -WorkspaceId $slowReplayWorkspace -PipeName $slowReplayPipe -TerminalSessionId $null
    Assert-Equal @(Get-JsonFiles $slowReplayInbox).Count 1 'Slow-replay fixture was not queued.'
    $null = Start-FakeTerminalServer -PipeName $slowReplayPipe -ResponseDelayMilliseconds 5000
    $slowReplayStartPayload = [ordered]@{
        session_id = 'session-slow-replay'
        cwd = $testRoot
        hook_event_name = 'SessionStart'
        source = 'resume'
    }
    $slowReplayResult = Invoke-HookProcess -EventName SessionStart -Payload $slowReplayStartPayload -InboxDirectory $slowReplayInbox -WorkspaceId $slowReplayWorkspace -PipeName $slowReplayPipe -TerminalSessionId $null -MaxReplayItems 64 -ConnectTimeoutMilliseconds 5000 -ResponseTimeoutMilliseconds 5000
    Assert-True ($slowReplayResult.ElapsedMilliseconds -lt 5000) 'Replay budget exceeded the five-second SessionStart deadline.'
    Assert-Equal @(Get-JsonFiles $slowReplayInbox).Count 1 'Unacknowledged replay entry was removed.'

    # SessionEnd is a strict sub-three-second no-op. Only SessionStart may scan
    # and quarantine malformed replay entries for bounded audit.
    $quarantineInbox = [System.IO.Path]::Combine($testRoot, 'quarantine-inbox')
    $null = [System.IO.Directory]::CreateDirectory($quarantineInbox)
    $malformedId = [guid]::NewGuid().ToString('D')
    [System.IO.File]::WriteAllText([System.IO.Path]::Combine($quarantineInbox, "$malformedId.json"), '{broken', $utf8NoBom)
    $quarantinePayload = [ordered]@{
        session_id = 'session-quarantine'
        cwd = $testRoot
        hook_event_name = 'SessionEnd'
        reason = 'other'
    }
    $quarantineResult = Invoke-HookProcess -EventName SessionEnd -Payload $quarantinePayload -InboxDirectory $quarantineInbox -WorkspaceId ([guid]::NewGuid().ToString('D')) -PipeName (New-TestPipeName) -TerminalSessionId $null
    Assert-Equal $quarantineResult.ExitCode 0 'SessionEnd no-op failed.'
    Assert-True ($quarantineResult.ElapsedMilliseconds -lt 3000) 'SessionEnd exceeded its three-second deadline.'
    Assert-Equal @(Get-JsonFiles $quarantineInbox).Count 1 'SessionEnd must not scan or mutate the inbox.'
    Assert-Equal @(Get-JsonFiles ([System.IO.Path]::Combine($quarantineInbox, '.invalid'))).Count 0 'SessionEnd unexpectedly quarantined an entry.'

    $quarantineStartPayload = [ordered]@{
        session_id = 'session-quarantine'
        cwd = $testRoot
        hook_event_name = 'SessionStart'
        source = 'resume'
    }
    $quarantineStartResult = Invoke-HookProcess -EventName SessionStart -Payload $quarantineStartPayload -InboxDirectory $quarantineInbox -WorkspaceId ([guid]::NewGuid().ToString('D')) -PipeName (New-TestPipeName) -TerminalSessionId $null
    Assert-Equal $quarantineStartResult.ExitCode 0 'SessionStart quarantine failed open incorrectly.'
    Assert-Equal @(Get-JsonFiles $quarantineInbox).Count 0 'Malformed entry remained in the live inbox after SessionStart.'
    Assert-Equal @(Get-JsonFiles ([System.IO.Path]::Combine($quarantineInbox, '.invalid'))).Count 1 'Malformed entry was not quarantined.'

    $invalidTimestampRequest = (($queuedRequest | ConvertTo-Json -Depth 8 -Compress) | ConvertFrom-Json)
    $invalidTimestampEventId = [guid]::NewGuid().ToString('D')
    $invalidTimestampRequest.id = 'codex-history-' + [guid]::NewGuid().ToString('N')
    $invalidTimestampRequest.params.eventId = $invalidTimestampEventId
    $invalidTimestampRequest.params.eventAt = 253402300800000
    [System.IO.File]::WriteAllText(
        [System.IO.Path]::Combine($quarantineInbox, "$invalidTimestampEventId.json"),
        ($invalidTimestampRequest | ConvertTo-Json -Depth 8 -Compress),
        $utf8NoBom)
    $null = Invoke-HookProcess -EventName SessionStart -Payload $quarantineStartPayload -InboxDirectory $quarantineInbox -WorkspaceId ([guid]::NewGuid().ToString('D')) -PipeName (New-TestPipeName) -TerminalSessionId $null
    Assert-Equal @(Get-JsonFiles $quarantineInbox).Count 0 'Out-of-range eventAt remained replayable.'
    Assert-Equal @(Get-JsonFiles ([System.IO.Path]::Combine($quarantineInbox, '.invalid'))).Count 2 'Out-of-range eventAt was not quarantined.'

    # File-count bounding evicts oldest owned entries.
    $boundedInbox = [System.IO.Path]::Combine($testRoot, 'bounded-inbox')
    $boundedWorkspace = [guid]::NewGuid().ToString('D')
    $boundedPipe = New-TestPipeName
    for ($index = 0; $index -lt 6; $index++) {
        $payload = [ordered]@{
            session_id = 'session-bounded'
            turn_id = "turn-$index"
            cwd = $testRoot
            hook_event_name = 'UserPromptSubmit'
            prompt = "queued-$index"
        }
        $null = Invoke-HookProcess -EventName UserPromptSubmit -Payload $payload -InboxDirectory $boundedInbox -WorkspaceId $boundedWorkspace -PipeName $boundedPipe -TerminalSessionId $null -MaxInboxFiles 3 -MaxInboxBytes 65536
    }
    Assert-True (@(Get-JsonFiles $boundedInbox).Count -le 3) 'Inbox file-count bound was exceeded.'
    $boundedBytes = (@(Get-JsonFiles $boundedInbox) | Measure-Object Length -Sum).Sum
    Assert-True ([long] $boundedBytes -le 65536) 'Inbox byte bound was exceeded.'

    # MaxInboxFiles/Bytes bound main UUID files plus reserved emergency slots.
    # While the mutex is held, a normal event still claims a fixed slot; once
    # that reserved capacity is full, later events fail only for capacity.
    $mutexReadyPath = [System.IO.Path]::Combine($testRoot, 'inbox-mutex-ready')
    $mutexReleasePath = [System.IO.Path]::Combine($testRoot, 'inbox-mutex-release')
    $mutexJob = Start-Job -ArgumentList $mutexReadyPath, $mutexReleasePath -ScriptBlock {
        param($ReadyPath, $ReleasePath)
        $mutex = [System.Threading.Mutex]::new($false, 'Local\Microsoft.WindowsTerminal.WorkspaceHistoryInbox.v1')
        $taken = $false
        try {
            try { $taken = $mutex.WaitOne(5000) } catch [System.Threading.AbandonedMutexException] { $taken = $true }
            if (-not $taken) { throw 'Unable to acquire inbox mutex for contention test.' }
            [System.IO.File]::WriteAllText($ReadyPath, 'locked', [System.Text.UTF8Encoding]::new($false))
            $deadline = [datetime]::UtcNow.AddSeconds(20)
            while (-not [System.IO.File]::Exists($ReleasePath) -and [datetime]::UtcNow -lt $deadline) {
                Start-Sleep -Milliseconds 25
            }
        }
        finally {
            if ($taken) { try { $mutex.ReleaseMutex() } catch {} }
            $mutex.Dispose()
        }
    }
    $jobs.Add($mutexJob)
    $mutexReady = $false
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        if ([System.IO.File]::Exists($mutexReadyPath)) {
            $mutexReady = $true
            break
        }
        Start-Sleep -Milliseconds 25
    }
    Assert-True $mutexReady 'Inbox mutex holder did not become ready.'
    $contendedInbox = [System.IO.Path]::Combine($testRoot, 'contended-inbox')
    $contendedWorkspace = [guid]::NewGuid().ToString('D')
    for ($index = 0; $index -lt 7; $index++) {
        $contendedPayload = [ordered]@{
            session_id = 'session-contended'
            turn_id = "turn-contended-$index"
            cwd = $testRoot
            hook_event_name = 'UserPromptSubmit'
            prompt = "save through fixed emergency capacity $index"
        }
        $contendedResult = Invoke-HookProcess -EventName UserPromptSubmit -Payload $contendedPayload -InboxDirectory $contendedInbox -WorkspaceId $contendedWorkspace -PipeName (New-TestPipeName) -TerminalSessionId $null -MaxInboxFiles 3 -MaxInboxBytes 65536
        Assert-Equal $contendedResult.ExitCode 0 "Contended inbox save $index failed open incorrectly."
        Assert-True ($contendedResult.ElapsedMilliseconds -lt 3000) "Contended inbox save $index waited for the mutex holder."
    }
    [System.IO.File]::WriteAllText($mutexReleasePath, 'release', $utf8NoBom)
    $null = Wait-Job -Job $mutexJob -Timeout 6
    $null = Receive-Job -Job $mutexJob -ErrorAction SilentlyContinue
    Remove-Job -Job $mutexJob -Force
    $null = $jobs.Remove($mutexJob)

    $contendedFiles = @(Get-JsonFiles $contendedInbox)
    Assert-True ($contendedFiles.Count -ge 1) 'Mutex contention silently dropped the first ordinary event despite reserved capacity.'
    Assert-True ($contendedFiles.Count -le 3) 'Main plus emergency files exceeded MaxInboxFiles under a held mutex.'
    $contendedBytes = ($contendedFiles | Measure-Object Length -Sum).Sum
    Assert-True ([long] $contendedBytes -le 65536) 'Main plus emergency bytes exceeded MaxInboxBytes under a held mutex.'
    Assert-True (@($contendedFiles | Where-Object { $_.Name -notmatch '^emergency-(?:0[0-9]|[12][0-9]|3[01])\.json$' }).Count -eq 0) 'Contended Save used an unbounded filename instead of a fixed emergency slot.'
    $contendedRequest = [System.IO.File]::ReadAllText($contendedFiles[0].FullName, $utf8NoBom) | ConvertFrom-Json
    Assert-True ($contendedRequest.params.eventId -cmatch '^[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}$') 'Emergency slot did not retain a canonical payload eventId receipt.'

    $contendedReplayPipe = New-TestPipeName
    $contendedReplayServer = Start-FakeTerminalServer -PipeName $contendedReplayPipe
    $contendedStartPayload = [ordered]@{
        session_id = 'session-contended'
        cwd = $testRoot
        hook_event_name = 'SessionStart'
        source = 'resume'
    }
    $null = Invoke-HookProcess -EventName SessionStart -Payload $contendedStartPayload -InboxDirectory $contendedInbox -WorkspaceId $contendedWorkspace -PipeName $contendedReplayPipe -TerminalSessionId $null -MaxInboxFiles 3 -MaxInboxBytes 65536
    $contendedReplayLines = @(Receive-FakeTerminalRequests $contendedReplayServer)
    Assert-Equal $contendedReplayLines.Count 1 'SessionStart did not recognize the emergency slot.'
    Assert-Equal (($contendedReplayLines[0] | ConvertFrom-Json).params.eventId) $contendedRequest.params.eventId 'Emergency replay changed the payload eventId receipt.'
    Assert-Equal @(Get-JsonFiles $contendedInbox).Count 0 'ACKed emergency slot was not released for reuse.'

    # A failed fixed-path writer must not delete a replacement that appears
    # after its exclusive handle closes. The injected pause is inside the mutex;
    # exact-byte cleanup must retain the nonmatching replacement.
    $abaInbox = [System.IO.Path]::Combine($testRoot, 'failed-write-aba-inbox')
    $null = [System.IO.Directory]::CreateDirectory($abaInbox)
    $abaSlot = [System.IO.Path]::Combine($abaInbox, 'emergency-00.json')
    $abaReplacement = 'replacement-B-must-survive'
    $abaJob = Start-Job -ArgumentList $abaSlot, $abaReplacement -ScriptBlock {
        param($Slot, $Replacement)
        $encoding = [System.Text.UTF8Encoding]::new($false)
        $deadline = [datetime]::UtcNow.AddSeconds(10)
        while ([datetime]::UtcNow -lt $deadline) {
            if ([System.IO.File]::Exists($Slot)) {
                $probe = $null
                try {
                    $probe = [System.IO.FileStream]::new(
                        $Slot,
                        [System.IO.FileMode]::Open,
                        [System.IO.FileAccess]::ReadWrite,
                        [System.IO.FileShare]::None)
                    $probe.Dispose()
                    $probe = $null
                    [System.IO.File]::Delete($Slot)
                    [System.IO.File]::WriteAllText($Slot, $Replacement, $encoding)
                    return
                }
                catch {}
                finally {
                    if ($null -ne $probe) { $probe.Dispose() }
                }
            }
            Start-Sleep -Milliseconds 5
        }
        throw 'Timed out replacing the failed writer slot.'
    }
    $jobs.Add($abaJob)
    $abaPayload = [ordered]@{
        session_id = 'session-failed-write-aba'
        turn_id = 'turn-failed-write-aba'
        cwd = $testRoot
        hook_event_name = 'UserPromptSubmit'
        prompt = 'inject a failed emergency write'
    }
    $abaResult = Invoke-HookProcess -EventName UserPromptSubmit -Payload $abaPayload -InboxDirectory $abaInbox -WorkspaceId ([guid]::NewGuid().ToString('D')) -PipeName (New-TestPipeName) -TerminalSessionId $null -MaxInboxFiles 1 -MaxInboxBytes 65536 -TestDiagnostics 'fail-inbox-write-aba'
    Assert-Equal $abaResult.ExitCode 0 'Injected failed writer did not fail open.'
    Wait-TestJobSuccess -Job $abaJob -Description 'Failed-write ABA replacement'
    Assert-True ([System.IO.File]::Exists($abaSlot)) 'Failed writer cleanup deleted the replacement slot.'
    Assert-Equal ([System.IO.File]::ReadAllText($abaSlot, $utf8NoBom)) $abaReplacement 'Failed writer cleanup did not preserve the replacement identity.'
    [System.IO.File]::Delete($abaSlot)

    # A later lower configuration retires completed high-numbered slots before
    # publishing. Active old slots are never deleted; that save fails capacity,
    # and a later attempt succeeds after the old writer closes.
    $downshiftInbox = [System.IO.Path]::Combine($testRoot, 'downshift-inbox')
    $downshiftReady = [System.IO.Path]::Combine($testRoot, 'downshift-mutex-ready')
    $downshiftRelease = [System.IO.Path]::Combine($testRoot, 'downshift-mutex-release')
    $downshiftMutexJob = Start-Job -ArgumentList $downshiftReady, $downshiftRelease -ScriptBlock {
        param($ReadyPath, $ReleasePath)
        $mutex = [System.Threading.Mutex]::new($false, 'Local\Microsoft.WindowsTerminal.WorkspaceHistoryInbox.v1')
        $taken = $false
        try {
            try { $taken = $mutex.WaitOne(5000) } catch [System.Threading.AbandonedMutexException] { $taken = $true }
            if (-not $taken) { throw 'Unable to acquire mutex for downshift fixture.' }
            [System.IO.File]::WriteAllText($ReadyPath, 'locked', [System.Text.UTF8Encoding]::new($false))
            $deadline = [datetime]::UtcNow.AddSeconds(20)
            while (-not [System.IO.File]::Exists($ReleasePath) -and [datetime]::UtcNow -lt $deadline) {
                Start-Sleep -Milliseconds 25
            }
        }
        finally {
            if ($taken) { try { $mutex.ReleaseMutex() } catch {} }
            $mutex.Dispose()
        }
    }
    $jobs.Add($downshiftMutexJob)
    for ($attempt = 0; $attempt -lt 100 -and -not [System.IO.File]::Exists($downshiftReady); $attempt++) {
        Start-Sleep -Milliseconds 25
    }
    Assert-True ([System.IO.File]::Exists($downshiftReady)) 'Downshift mutex holder did not become ready.'

    $downshiftWorkspace = [guid]::NewGuid().ToString('D')
    for ($index = 0; $index -lt 4; $index++) {
        $largePayload = [ordered]@{
            session_id = 'session-downshift'
            turn_id = "turn-large-slot-$index"
            cwd = $testRoot
            hook_event_name = 'UserPromptSubmit'
            prompt = "large-configuration slot $index"
        }
        $null = Invoke-HookProcess -EventName UserPromptSubmit -Payload $largePayload -InboxDirectory $downshiftInbox -WorkspaceId $downshiftWorkspace -PipeName (New-TestPipeName) -TerminalSessionId $null -MaxInboxFiles 32 -MaxInboxBytes 262144
    }
    $lockedDownshiftPayload = [ordered]@{
        session_id = 'session-downshift'
        turn_id = 'turn-small-while-locked'
        cwd = $testRoot
        hook_event_name = 'UserPromptSubmit'
        prompt = 'do not add while legacy slots cannot be governed'
    }
    $null = Invoke-HookProcess -EventName UserPromptSubmit -Payload $lockedDownshiftPayload -InboxDirectory $downshiftInbox -WorkspaceId $downshiftWorkspace -PipeName (New-TestPipeName) -TerminalSessionId $null -MaxInboxFiles 3 -MaxInboxBytes 65536
    Assert-Equal @(Get-JsonFiles $downshiftInbox).Count 4 'Lock-free downshift fallback added to an already unsafe legacy layout.'
    [System.IO.File]::WriteAllText($downshiftRelease, 'release', $utf8NoBom)
    Wait-TestJobSuccess -Job $downshiftMutexJob -Description 'Downshift mutex holder'
    $largeSlots = @(Get-JsonFiles $downshiftInbox)
    Assert-Equal $largeSlots.Count 4 'Large configuration did not fill all four reserved slots.'
    Assert-True (@($largeSlots | Where-Object { $_.Name -notmatch '^emergency-0[0-3]\.json$' }).Count -eq 0) 'Large configuration escaped its fixed four-slot reservation.'

    $smallSuccessPayload = [ordered]@{
        session_id = 'session-downshift'
        turn_id = 'turn-small-success'
        cwd = $testRoot
        hook_event_name = 'UserPromptSubmit'
        prompt = 'save after lowering capacity'
    }
    $null = Invoke-HookProcess -EventName UserPromptSubmit -Payload $smallSuccessPayload -InboxDirectory $downshiftInbox -WorkspaceId $downshiftWorkspace -PipeName (New-TestPipeName) -TerminalSessionId $null -MaxInboxFiles 3 -MaxInboxBytes 65536
    $smallFiles = @(Get-JsonFiles $downshiftInbox)
    Assert-True ($smallFiles.Count -le 3) 'Downshifted save left live files above the new total limit.'
    Assert-True ([long] (($smallFiles | Measure-Object Length -Sum).Sum) -le 65536) 'Downshifted save left live bytes above the new total limit.'
    Assert-True (@($smallFiles | Where-Object { $_.Name -match '^emergency-(?!00)' }).Count -eq 0) 'Downshifted save retained an out-of-range emergency slot.'
    $smallTurns = @($smallFiles | ForEach-Object { ([System.IO.File]::ReadAllText($_.FullName, $utf8NoBom) | ConvertFrom-Json).params.turnId })
    Assert-True ($smallTurns -contains 'turn-small-success') 'Downshift governance dropped the current event instead of preserving its main/fallback path.'

    $activeHighSlot = [System.IO.Path]::Combine($downshiftInbox, 'emergency-03.json')
    $activeReady = [System.IO.Path]::Combine($testRoot, 'downshift-active-ready')
    $activeRelease = [System.IO.Path]::Combine($testRoot, 'downshift-active-release')
    $activeSlotJob = Start-Job -ArgumentList $activeHighSlot, $activeReady, $activeRelease -ScriptBlock {
        param($Slot, $ReadyPath, $ReleasePath)
        $stream = [System.IO.FileStream]::new($Slot, [System.IO.FileMode]::CreateNew, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)
        try {
            [System.IO.File]::WriteAllText($ReadyPath, 'active', [System.Text.UTF8Encoding]::new($false))
            $deadline = [datetime]::UtcNow.AddSeconds(20)
            while (-not [System.IO.File]::Exists($ReleasePath) -and [datetime]::UtcNow -lt $deadline) {
                Start-Sleep -Milliseconds 25
            }
        }
        finally {
            $stream.Dispose()
        }
    }
    $jobs.Add($activeSlotJob)
    for ($attempt = 0; $attempt -lt 100 -and -not [System.IO.File]::Exists($activeReady); $attempt++) {
        Start-Sleep -Milliseconds 25
    }
    Assert-True ([System.IO.File]::Exists($activeReady)) 'Active out-of-range slot fixture did not become ready.'
    $mainBeforeFailure = @($smallFiles | Where-Object { $_.BaseName -match '^[0-9a-f]{8}-' }).Count
    $smallFailurePayload = [ordered]@{
        session_id = 'session-downshift'
        turn_id = 'turn-small-must-fail'
        cwd = $testRoot
        hook_event_name = 'UserPromptSubmit'
        prompt = 'do not delete an active old slot'
    }
    $null = Invoke-HookProcess -EventName UserPromptSubmit -Payload $smallFailurePayload -InboxDirectory $downshiftInbox -WorkspaceId $downshiftWorkspace -PipeName (New-TestPipeName) -TerminalSessionId $null -MaxInboxFiles 3 -MaxInboxBytes 65536
    Assert-True ([System.IO.File]::Exists($activeHighSlot)) 'Downshift governance deleted an active out-of-range writer.'
    $mainAfterFailure = @(Get-JsonFiles $downshiftInbox | Where-Object { $_.BaseName -match '^[0-9a-f]{8}-' }).Count
    Assert-Equal $mainAfterFailure $mainBeforeFailure 'A save succeeded even though active legacy capacity could not be governed.'
    [System.IO.File]::WriteAllText($activeRelease, 'release', $utf8NoBom)
    Wait-TestJobSuccess -Job $activeSlotJob -Description 'Active downshift slot holder'

    $smallRetryPayload = [ordered]@{
        session_id = 'session-downshift'
        turn_id = 'turn-small-retry'
        cwd = $testRoot
        hook_event_name = 'UserPromptSubmit'
        prompt = 'retry after old writer closes'
    }
    $null = Invoke-HookProcess -EventName UserPromptSubmit -Payload $smallRetryPayload -InboxDirectory $downshiftInbox -WorkspaceId $downshiftWorkspace -PipeName (New-TestPipeName) -TerminalSessionId $null -MaxInboxFiles 3 -MaxInboxBytes 65536
    $retryFiles = @(Get-JsonFiles $downshiftInbox)
    Assert-True ($retryFiles.Count -le 3) 'Retry after downshift exceeded the new total file limit.'
    Assert-True ([long] (($retryFiles | Measure-Object Length -Sum).Sum) -le 65536) 'Retry after downshift exceeded the new total byte limit.'
    Assert-True (-not [System.IO.File]::Exists($activeHighSlot)) 'Completed out-of-range slot was not retired on retry.'
    $retryTurns = @($retryFiles | ForEach-Object { ([System.IO.File]::ReadAllText($_.FullName, $utf8NoBom) | ConvertFrom-Json).params.turnId })
    Assert-True ($retryTurns -contains 'turn-small-retry') 'Retry did not save after legacy slot governance became safe.'

    # Directory scans are hard-capped by total entries, including attacker-made
    # non-JSON files, so replay and trim retain their outer deadlines.
    $crowdedInbox = [System.IO.Path]::Combine($testRoot, 'crowded-inbox')
    $null = [System.IO.Directory]::CreateDirectory($crowdedInbox)
    for ($index = 0; $index -lt 700; $index++) {
        [System.IO.File]::WriteAllText(
            [System.IO.Path]::Combine($crowdedInbox, ('junk-{0:D4}.tmp' -f $index)),
            'x',
            $utf8NoBom)
    }
    $crowdedWorkspace = [guid]::NewGuid().ToString('D')
    $crowdedStartPayload = [ordered]@{
        session_id = 'session-crowded'
        cwd = $testRoot
        hook_event_name = 'SessionStart'
        source = 'resume'
    }
    $crowdedStartResult = Invoke-HookProcess -EventName SessionStart -Payload $crowdedStartPayload -InboxDirectory $crowdedInbox -WorkspaceId $crowdedWorkspace -PipeName (New-TestPipeName) -TerminalSessionId $null
    Assert-True ($crowdedStartResult.ElapsedMilliseconds -lt 5000) 'Replay scanned an unbounded crowded directory.'
    $crowdedStopPayload = [ordered]@{
        session_id = 'session-crowded'
        turn_id = 'turn-crowded'
        cwd = $testRoot
        hook_event_name = 'Stop'
        last_assistant_message = 'bounded trim'
    }
    $crowdedStopResult = Invoke-HookProcess -EventName Stop -Payload $crowdedStopPayload -InboxDirectory $crowdedInbox -WorkspaceId $crowdedWorkspace -PipeName (New-TestPipeName) -TerminalSessionId $null
    Assert-Equal $crowdedStopResult.Stdout.Trim() '{}' 'Crowded-directory Stop lost its JSON response.'
    Assert-True ($crowdedStopResult.ElapsedMilliseconds -lt 5000) 'Trim scanned an unbounded crowded directory.'
    Assert-Equal @(Get-JsonFiles $crowdedInbox).Count 1 'Crowded-directory Save was not committed.'

    # A moved pane overrides stale explicit workspace/pipe values with the live
    # WT_SESSION location, even when cwd still resembles the old workspace.
    $oldPipe = New-TestPipeName
    $actualPipe = New-TestPipeName
    $oldWorkspace = [guid]::NewGuid().ToString('D')
    $actualWorkspace = [guid]::NewGuid().ToString('D')
    $actualTerminalSession = [guid]::NewGuid().ToString('D')
    $oldServer = Start-FakeTerminalServer -PipeName $oldPipe -ExpectedConnections 1 -Workspaces @(
        [ordered]@{
            id = $oldWorkspace
            selected = $true
            workingDirectory = $testRoot
            terminalSessionIds = @([guid]::NewGuid().ToString('D'))
        })
    $actualServer = Start-FakeTerminalServer -PipeName $actualPipe -ExpectedConnections 2 -Workspaces @(
        [ordered]@{
            id = $actualWorkspace
            selected = $true
            workingDirectory = [System.IO.Path]::Combine($testRoot, 'moved')
            terminalSessionIds = @($actualTerminalSession)
        })
    $stalePayload = [ordered]@{
        session_id = 'session-moved'
        turn_id = 'turn-moved'
        cwd = $testRoot
        hook_event_name = 'UserPromptSubmit'
        prompt = 'route to actual pane location'
    }
    $staleResult = Invoke-HookProcess -EventName UserPromptSubmit -Payload $stalePayload -InboxDirectory ([System.IO.Path]::Combine($testRoot, 'stale-inbox')) -WorkspaceId $oldWorkspace -PipeName $oldPipe -TerminalSessionId $actualTerminalSession
    Assert-Equal $staleResult.ExitCode 0 'Stale explicit route test hook failed.'
    $oldLines = @(Receive-FakeTerminalRequests $oldServer)
    $actualLines = @(Receive-FakeTerminalRequests $actualServer)
    Assert-Equal (($oldLines | ForEach-Object { ($_ | ConvertFrom-Json).method }) -join ',') 'workspace.list' 'Stale pipe must not receive history.'
    Assert-Equal (($actualLines | ForEach-Object { ($_ | ConvertFrom-Json).method }) -join ',') 'workspace.list,history.ingest' 'Actual pipe did not receive discovery plus history.'
    $actualHistory = ($actualLines[1] | ConvertFrom-Json)
    Assert-Equal $actualHistory.params.workspaceId $actualWorkspace 'WT_SESSION did not override stale explicit workspace id.'

    # If at least one live snapshot exists but no pane matches WT_SESSION, the
    # route must fail closed even when cwd is an exact workspace match.
    $unmatchedPipe = New-TestPipeName
    $unmatchedWorkspace = [guid]::NewGuid().ToString('D')
    $unmatchedServer = Start-FakeTerminalServer -PipeName $unmatchedPipe -Workspaces @(
        [ordered]@{
            id = $unmatchedWorkspace
            selected = $true
            workingDirectory = $testRoot
            terminalSessionIds = @([guid]::NewGuid().ToString('D'))
        })
    $unmatchedInbox = [System.IO.Path]::Combine($testRoot, 'unmatched-live-inbox')
    $unmatchedPayload = [ordered]@{
        session_id = 'session-unmatched'
        turn_id = 'turn-unmatched'
        cwd = $testRoot
        hook_event_name = 'UserPromptSubmit'
        prompt = 'must not route by cwd'
    }
    $unmatchedResult = Invoke-HookProcess -EventName UserPromptSubmit -Payload $unmatchedPayload -InboxDirectory $unmatchedInbox -WorkspaceId $unmatchedWorkspace -PipeName $unmatchedPipe -TerminalSessionId ([guid]::NewGuid().ToString('D')) -MaxPipeCandidates 1
    Assert-Equal $unmatchedResult.ExitCode 0 'Unmatched live WT_SESSION hook failed.'
    $unmatchedLines = @(Receive-FakeTerminalRequests $unmatchedServer)
    Assert-Equal (($unmatchedLines | ForEach-Object { ($_ | ConvertFrom-Json).method }) -join ',') 'workspace.list' 'Unmatched live WT_SESSION must never send history by cwd.'
    Assert-Equal @(Get-JsonFiles $unmatchedInbox).Count 0 'Unmatched live WT_SESSION must fail closed, not spool to a possibly wrong workspace.'

    # With no live snapshot at all, the injected workspace id remains a safe
    # durable key, but its unverified pipe is never used for immediate delivery.
    $offlineSessionWorkspace = [guid]::NewGuid().ToString('D')
    $offlineSessionInbox = [System.IO.Path]::Combine($testRoot, 'offline-session-inbox')
    $offlineSessionPayload = [ordered]@{
        session_id = 'session-offline-pane'
        turn_id = 'turn-offline-pane'
        cwd = $testRoot
        hook_event_name = 'UserPromptSubmit'
        prompt = 'queue with trusted injected id'
    }
    $offlineSessionResult = Invoke-HookProcess -EventName UserPromptSubmit -Payload $offlineSessionPayload -InboxDirectory $offlineSessionInbox -WorkspaceId $offlineSessionWorkspace -PipeName (New-TestPipeName) -TerminalSessionId ([guid]::NewGuid().ToString('D')) -MaxPipeCandidates 1
    Assert-Equal $offlineSessionResult.ExitCode 0 'Offline WT_SESSION hook failed.'
    $offlineSessionFiles = @(Get-JsonFiles $offlineSessionInbox)
    Assert-Equal $offlineSessionFiles.Count 1 'Offline WT_SESSION event did not retain its trusted durable route.'
    $offlineSessionRequest = [System.IO.File]::ReadAllText($offlineSessionFiles[0].FullName, $utf8NoBom) | ConvertFrom-Json
    Assert-Equal $offlineSessionRequest.params.workspaceId $offlineSessionWorkspace 'Offline WT_SESSION durable key changed.'

    # Installer merge is idempotent and uninstall removes only owned handlers.
    $testCodexHome = [System.IO.Path]::Combine($testRoot, 'codex-home')
    $null = [System.IO.Directory]::CreateDirectory($testCodexHome)
    $deepExtension = [ordered]@{ leaf = 'preserve-deep-value' }
    for ($depth = 29; $depth -ge 0; $depth--) {
        $deepExtension = [ordered]@{ ("level-$depth") = $deepExtension }
    }
    $existingConfiguration = [ordered]@{
        description = 'preserve me'
        customTopLevel = [ordered]@{ value = 42 }
        customDeepExtension = $deepExtension
        hooks = [ordered]@{
            Stop = @([ordered]@{
                matcher = ''
                hooks = @([ordered]@{
                    type = 'command'
                    command = 'pwsh -File C:\other\stop.ps1'
                    timeout = 2
                })
            })
            PreToolUse = @([ordered]@{
                matcher = 'Bash'
                hooks = @([ordered]@{
                    type = 'command'
                    command = 'pwsh -File C:\other\policy.ps1'
                })
            })
        }
    }
    $testHooksPath = [System.IO.Path]::Combine($testCodexHome, 'hooks.json')
    [System.IO.File]::WriteAllText($testHooksPath, ($existingConfiguration | ConvertTo-Json -Depth 64), $utf8NoBom)
    $installOutput = (& $installScript -CodexHome $testCodexHome 6>&1 | Out-String)
    Assert-True ($installOutput -match '/hooks' -and $installOutput -match 'trust') 'Installer must surface the official hook trust review.'
    $null = & $installScript -CodexHome $testCodexHome 6>&1
    $installedConfiguration = [System.IO.File]::ReadAllText($testHooksPath, $utf8NoBom) | ConvertFrom-Json
    Assert-Equal $installedConfiguration.description 'preserve me' 'Installer overwrote a top-level field.'
    Assert-Equal ([int] $installedConfiguration.customTopLevel.value) 42 'Installer lost a custom top-level object.'
    $installedDeepCursor = $installedConfiguration.customDeepExtension
    for ($depth = 0; $depth -lt 30; $depth++) {
        $installedDeepCursor = $installedDeepCursor.PSObject.Properties["level-$depth"].Value
    }
    Assert-Equal $installedDeepCursor.leaf 'preserve-deep-value' 'Installer truncated a deeply nested extension field.'
    Assert-Equal $installedConfiguration.hooks.PreToolUse[0].hooks[0].command 'pwsh -File C:\other\policy.ps1' 'Installer overwrote an unrelated event.'
    Assert-True (@($installedConfiguration.hooks.Stop | ForEach-Object { $_.hooks } | Where-Object { $_.command -eq 'pwsh -File C:\other\stop.ps1' }).Count -eq 1) 'Installer removed an unrelated Stop handler.'
    foreach ($event in @('UserPromptSubmit', 'Stop', 'SessionStart', 'SessionEnd')) {
        $owned = @($installedConfiguration.hooks.$event | ForEach-Object { $_.hooks } | Where-Object { $_.command -match 'windows-terminal-workspace-history-v1-' })
        Assert-Equal $owned.Count 1 "Installer is not idempotent for $event."
    }
    Assert-True ([int] $installedConfiguration.hooks.SessionEnd[-1].hooks[0].timeout -le 3) 'Installed SessionEnd timeout exceeds 3 seconds.'
    Assert-True ([string] $installedConfiguration.hooks.SessionEnd[-1].hooks[0].command -match 'exit 0' -and [string] $installedConfiguration.hooks.SessionEnd[-1].hooks[0].command -notmatch '& \$hookScript') 'Installed SessionEnd must not execute the implementation.'
    Assert-Equal @(Get-ChildItem -LiteralPath ([System.IO.Path]::Combine($testCodexHome, 'hooks')) -File -Filter 'workspace-history-*.ps1').Count 1 'Installer did not create exactly one versioned script.'

    $uninstallOutput = (& $uninstallScript -CodexHome $testCodexHome 6>&1 | Out-String)
    $uninstalledConfiguration = [System.IO.File]::ReadAllText($testHooksPath, $utf8NoBom) | ConvertFrom-Json
    Assert-True ($uninstallOutput -match 'unrelated hooks were preserved') 'Uninstaller did not report safe merge behavior.'
    Assert-Equal $uninstalledConfiguration.hooks.PreToolUse[0].hooks[0].command 'pwsh -File C:\other\policy.ps1' 'Uninstaller removed an unrelated event.'
    $uninstalledDeepCursor = $uninstalledConfiguration.customDeepExtension
    for ($depth = 0; $depth -lt 30; $depth++) {
        $uninstalledDeepCursor = $uninstalledDeepCursor.PSObject.Properties["level-$depth"].Value
    }
    Assert-Equal $uninstalledDeepCursor.leaf 'preserve-deep-value' 'Uninstaller truncated a deeply nested extension field.'
    Assert-True (@($uninstalledConfiguration.hooks.Stop | ForEach-Object { $_.hooks } | Where-Object { $_.command -eq 'pwsh -File C:\other\stop.ps1' }).Count -eq 1) 'Uninstaller removed an unrelated Stop handler.'
    foreach ($event in @('UserPromptSubmit', 'Stop', 'SessionStart', 'SessionEnd')) {
        $eventProperty = $uninstalledConfiguration.hooks.PSObject.Properties[$event]
        if ($null -ne $eventProperty) {
            $owned = @($eventProperty.Value | ForEach-Object { $_.hooks } | Where-Object { $_.command -match 'windows-terminal-workspace-history-v1-' })
            Assert-Equal $owned.Count 0 "Uninstaller left an owned $event handler."
        }
    }
    Assert-Equal @(Get-ChildItem -LiteralPath ([System.IO.Path]::Combine($testCodexHome, 'hooks')) -File -Filter 'workspace-history-*.ps1').Count 0 'Uninstaller left a verified installed script.'

    # Concurrent installers serialize their full read/config/script transaction.
    $serializedHome = [System.IO.Path]::Combine($testRoot, 'serialized-codex-home')
    $null = [System.IO.Directory]::CreateDirectory($serializedHome)
    $serializedHooksPath = [System.IO.Path]::Combine($serializedHome, 'hooks.json')
    [System.IO.File]::WriteAllText($serializedHooksPath, '{"marker":"serialized"}', $utf8NoBom)
    $serializedTimer = [System.Diagnostics.Stopwatch]::StartNew()
    $installJobOne = Start-Job -ArgumentList $installScript, $serializedHome -ScriptBlock {
        param($ScriptPath, $HomePath)
        $null = & $ScriptPath -CodexHome $HomePath -BeforeCommitDelayMilliseconds 500 6>&1
    }
    $installJobTwo = Start-Job -ArgumentList $installScript, $serializedHome -ScriptBlock {
        param($ScriptPath, $HomePath)
        $null = & $ScriptPath -CodexHome $HomePath -BeforeCommitDelayMilliseconds 500 6>&1
    }
    $jobs.Add($installJobOne)
    $jobs.Add($installJobTwo)
    $null = Wait-TestJobSuccess -Job $installJobOne -Description 'First concurrent installer'
    $null = Wait-TestJobSuccess -Job $installJobTwo -Description 'Second concurrent installer'
    $serializedTimer.Stop()
    Assert-True ($serializedTimer.ElapsedMilliseconds -ge 850) 'Concurrent installers did not appear to serialize on one cross-process lock.'
    $serializedConfiguration = [System.IO.File]::ReadAllText($serializedHooksPath, $utf8NoBom) | ConvertFrom-Json
    Assert-Equal $serializedConfiguration.marker 'serialized' 'Concurrent installers lost an unrelated field.'
    foreach ($event in @('UserPromptSubmit', 'Stop', 'SessionStart', 'SessionEnd')) {
        $owned = @($serializedConfiguration.hooks.$event | ForEach-Object { $_.hooks } | Where-Object { $_.command -match 'windows-terminal-workspace-history-v1-' })
        Assert-Equal $owned.Count 1 "Concurrent installers duplicated or lost $event."
    }
    Assert-Equal @(Get-ChildItem -LiteralPath ([System.IO.Path]::Combine($serializedHome, 'hooks')) -File -Filter 'workspace-history-*.ps1').Count 1 'Concurrent installers produced inconsistent scripts.'

    # Installer and uninstaller use the same lock. Whichever commits last, the
    # resulting config/script pair must be entirely installed or uninstalled.
    $mixedTimer = [System.Diagnostics.Stopwatch]::StartNew()
    $mixedInstallJob = Start-Job -ArgumentList $installScript, $serializedHome -ScriptBlock {
        param($ScriptPath, $HomePath)
        $null = & $ScriptPath -CodexHome $HomePath -BeforeCommitDelayMilliseconds 700 6>&1
    }
    $mixedUninstallJob = Start-Job -ArgumentList $uninstallScript, $serializedHome -ScriptBlock {
        param($ScriptPath, $HomePath)
        $null = & $ScriptPath -CodexHome $HomePath -BeforeCommitDelayMilliseconds 700 6>&1
    }
    $jobs.Add($mixedInstallJob)
    $jobs.Add($mixedUninstallJob)
    $null = Wait-TestJobSuccess -Job $mixedInstallJob -Description 'Concurrent mixed installer'
    $null = Wait-TestJobSuccess -Job $mixedUninstallJob -Description 'Concurrent mixed uninstaller'
    $mixedTimer.Stop()
    Assert-True ($mixedTimer.ElapsedMilliseconds -ge 1200) 'Installer and uninstaller did not share one transaction lock.'
    $mixedConfiguration = [System.IO.File]::ReadAllText($serializedHooksPath, $utf8NoBom) | ConvertFrom-Json
    $mixedOwnedCount = 0
    foreach ($event in @('UserPromptSubmit', 'Stop', 'SessionStart', 'SessionEnd')) {
        $property = $mixedConfiguration.hooks.PSObject.Properties[$event]
        if ($null -ne $property) {
            $mixedOwnedCount += @($property.Value | ForEach-Object { $_.hooks } | Where-Object { $_.command -match 'windows-terminal-workspace-history-v1-' }).Count
        }
    }
    $mixedScriptCount = @(Get-ChildItem -LiteralPath ([System.IO.Path]::Combine($serializedHome, 'hooks')) -File -Filter 'workspace-history-*.ps1').Count
    Assert-True (($mixedOwnedCount -eq 4 -and $mixedScriptCount -eq 1) -or ($mixedOwnedCount -eq 0 -and $mixedScriptCount -eq 0)) 'Concurrent install/uninstall left dangling commands or orphan transaction state.'

    # An external editor does not take the transaction mutex, so the installer
    # must compare the exact original hash immediately before commit.
    $editRaceHome = [System.IO.Path]::Combine($testRoot, 'edit-race-codex-home')
    $null = [System.IO.Directory]::CreateDirectory($editRaceHome)
    $editRaceHooksPath = [System.IO.Path]::Combine($editRaceHome, 'hooks.json')
    [System.IO.File]::WriteAllText($editRaceHooksPath, '{"marker":"before-install-race"}', $utf8NoBom)
    $editInstallJob = Start-Job -ArgumentList $installScript, $editRaceHome -ScriptBlock {
        param($ScriptPath, $HomePath)
        $null = & $ScriptPath -CodexHome $HomePath -BeforeCommitDelayMilliseconds 1200 6>&1
    }
    $jobs.Add($editInstallJob)
    Start-Sleep -Milliseconds 650
    Write-TestAtomicUtf8 -Path $editRaceHooksPath -Text '{"marker":"user-install-edit"}'
    $editInstallCompleted = Wait-Job -Job $editInstallJob -Timeout 20
    Assert-True ($null -ne $editInstallCompleted) 'Concurrent-edit installer timed out.'
    $null = Receive-Job -Job $editInstallJob -ErrorAction SilentlyContinue -InformationAction SilentlyContinue 6>$null
    Remove-Job -Job $editInstallJob -Force
    $null = $jobs.Remove($editInstallJob)
    $editInstallConfiguration = [System.IO.File]::ReadAllText($editRaceHooksPath, $utf8NoBom) | ConvertFrom-Json
    Assert-Equal $editInstallConfiguration.marker 'user-install-edit' 'Installer lost a concurrent user edit.'
    $editInstallOwned = 0
    $editInstallHooksProperty = $editInstallConfiguration.PSObject.Properties['hooks']
    if ($null -ne $editInstallHooksProperty) {
        foreach ($event in @('UserPromptSubmit', 'Stop', 'SessionStart', 'SessionEnd')) {
            $property = $editInstallHooksProperty.Value.PSObject.Properties[$event]
            if ($null -ne $property) {
                $editInstallOwned += @($property.Value | ForEach-Object { $_.hooks } | Where-Object { $_.command -match 'windows-terminal-workspace-history-v1-' }).Count
            }
        }
    }
    $editInstallScriptCount = if ([System.IO.Directory]::Exists([System.IO.Path]::Combine($editRaceHome, 'hooks'))) {
        @(Get-ChildItem -LiteralPath ([System.IO.Path]::Combine($editRaceHome, 'hooks')) -File -Filter 'workspace-history-*.ps1').Count
    }
    else { 0 }
    Assert-True (($editInstallOwned -eq 4 -and $editInstallScriptCount -eq 1) -or ($editInstallOwned -eq 0 -and $editInstallScriptCount -eq 0)) 'Installer edit race left a dangling config/script pair.'

    # Ensure an installed fixture, then race the uninstaller with another atomic
    # user edit. Either serialized outcome must retain the edit and stay paired.
    if ($editInstallOwned -eq 0) {
        $null = & $installScript -CodexHome $editRaceHome 6>&1
    }
    $userUninstallEdit = [System.IO.File]::ReadAllText($editRaceHooksPath, $utf8NoBom) | ConvertFrom-Json -AsHashtable
    $userUninstallEdit['marker'] = 'user-uninstall-edit'
    $userUninstallEditText = $userUninstallEdit | ConvertTo-Json -Depth 64
    $editUninstallJob = Start-Job -ArgumentList $uninstallScript, $editRaceHome -ScriptBlock {
        param($ScriptPath, $HomePath)
        $null = & $ScriptPath -CodexHome $HomePath -BeforeCommitDelayMilliseconds 1200 6>&1
    }
    $jobs.Add($editUninstallJob)
    Start-Sleep -Milliseconds 650
    Write-TestAtomicUtf8 -Path $editRaceHooksPath -Text $userUninstallEditText
    $editUninstallCompleted = Wait-Job -Job $editUninstallJob -Timeout 20
    Assert-True ($null -ne $editUninstallCompleted) 'Concurrent-edit uninstaller timed out.'
    $null = Receive-Job -Job $editUninstallJob -ErrorAction SilentlyContinue -InformationAction SilentlyContinue 6>$null
    Remove-Job -Job $editUninstallJob -Force
    $null = $jobs.Remove($editUninstallJob)
    $editUninstallConfiguration = [System.IO.File]::ReadAllText($editRaceHooksPath, $utf8NoBom) | ConvertFrom-Json
    Assert-Equal $editUninstallConfiguration.marker 'user-uninstall-edit' 'Uninstaller lost a concurrent user edit.'
    $editUninstallOwned = 0
    foreach ($event in @('UserPromptSubmit', 'Stop', 'SessionStart', 'SessionEnd')) {
        $property = $editUninstallConfiguration.hooks.PSObject.Properties[$event]
        if ($null -ne $property) {
            $editUninstallOwned += @($property.Value | ForEach-Object { $_.hooks } | Where-Object { $_.command -match 'windows-terminal-workspace-history-v1-' }).Count
        }
    }
    $editUninstallScriptCount = @(Get-ChildItem -LiteralPath ([System.IO.Path]::Combine($editRaceHome, 'hooks')) -File -Filter 'workspace-history-*.ps1').Count
    Assert-True (($editUninstallOwned -eq 4 -and $editUninstallScriptCount -eq 1) -or ($editUninstallOwned -eq 0 -and $editUninstallScriptCount -eq 0)) 'Uninstaller edit race left dangling commands or removed a referenced script.'

    # Reproduce the former hash-check/overwrite gap with a large valid config.
    # The versioned script appears only after the installer's baseline checks;
    # replace hooks.json atomically while it is still in the commit delay. The
    # CAS must report a conflict, restore the marker, and roll back its script.
    $largeRaceHome = [System.IO.Path]::Combine($testRoot, 'large-cas-race-codex-home')
    $null = [System.IO.Directory]::CreateDirectory($largeRaceHome)
    $largeRaceHooksPath = [System.IO.Path]::Combine($largeRaceHome, 'hooks.json')
    $largePadding = 'x' * (20 * 1024 * 1024)
    $largeBaselineText = '{"marker":"large-baseline","padding":"' + $largePadding + '"}'
    $largeInstallEditText = '{"marker":"external-after-hash-install","padding":"' + $largePadding + '"}'
    [System.IO.File]::WriteAllText($largeRaceHooksPath, $largeBaselineText, $utf8NoBom)
    $largeInstallJob = Start-Job -ArgumentList $installScript, $largeRaceHome -ScriptBlock {
        param($ScriptPath, $HomePath)
        try {
            $message = (& $ScriptPath -CodexHome $HomePath -BeforeCommitDelayMilliseconds 1200 6>&1 | Out-String)
            [pscustomobject]@{ Succeeded = $true; Message = $message }
        }
        catch {
            [pscustomobject]@{ Succeeded = $false; Message = $_.Exception.Message }
        }
    }
    $jobs.Add($largeInstallJob)
    $largeScriptObserved = $false
    for ($attempt = 0; $attempt -lt 500; $attempt++) {
        $largeHooksDirectory = [System.IO.Path]::Combine($largeRaceHome, 'hooks')
        if ([System.IO.Directory]::Exists($largeHooksDirectory) -and
            @([System.IO.Directory]::EnumerateFiles($largeHooksDirectory, 'workspace-history-*.ps1')).Count -gt 0) {
            $largeScriptObserved = $true
            break
        }
        if ($largeInstallJob.State -ne 'Running') { break }
        Start-Sleep -Milliseconds 20
    }
    Assert-True $largeScriptObserved 'Large-config installer never reached its post-hash script phase.'
    Start-Sleep -Milliseconds 200
    Write-TestAtomicUtf8 -Path $largeRaceHooksPath -Text $largeInstallEditText
    $largeInstallResult = Wait-TestTransactionJob -Job $largeInstallJob -Description 'Large-config CAS installer' -TimeoutSeconds 45
    Assert-True (-not $largeInstallResult.Succeeded) 'Large-config installer reported success after a post-hash external replace.'
    Assert-True ([string] $largeInstallResult.Message -match 'Concurrent edit detected') 'Large-config installer did not report its CAS conflict.'
    $largeInstallConfiguration = [System.IO.File]::ReadAllText($largeRaceHooksPath, $utf8NoBom) | ConvertFrom-Json
    Assert-Equal $largeInstallConfiguration.marker 'external-after-hash-install' 'Installer CAS lost the post-hash external marker.'
    Assert-Equal @([System.IO.Directory]::EnumerateFiles($largeHooksDirectory, 'workspace-history-*.ps1')).Count 0 'Installer CAS conflict left an orphan versioned script.'

    # Establish a large installed fixture. The uninstaller uses the same CAS;
    # racing it with an atomic copy of the installed config must leave both the
    # marker and every referenced versioned script intact.
    $null = & $installScript -CodexHome $largeRaceHome 6>&1
    $largeInstalledText = [System.IO.File]::ReadAllText($largeRaceHooksPath, $utf8NoBom)
    $largeUninstallEditText = $largeInstalledText -replace '"marker"\s*:\s*"external-after-hash-install"', '"marker": "external-after-hash-uninstall"'
    Assert-True (-not [string]::Equals($largeInstalledText, $largeUninstallEditText, [System.StringComparison]::Ordinal)) 'Failed to prepare the large uninstaller marker edit.'
    $largeUninstallJob = Start-Job -ArgumentList $uninstallScript, $largeRaceHome -ScriptBlock {
        param($ScriptPath, $HomePath)
        try {
            $message = (& $ScriptPath -CodexHome $HomePath -BeforeCommitDelayMilliseconds 2500 6>&1 | Out-String)
            [pscustomobject]@{ Succeeded = $true; Message = $message }
        }
        catch {
            [pscustomobject]@{ Succeeded = $false; Message = $_.Exception.Message }
        }
    }
    $jobs.Add($largeUninstallJob)
    # Parsing and hashing 20 MiB completes before this delay on the supported
    # Windows PowerShell 7 test hosts; the explicit commit delay then keeps the
    # post-check window open deterministically.
    Start-Sleep -Milliseconds 2000
    Assert-Equal ([string] $largeUninstallJob.State) 'Running' 'Large-config uninstaller left no post-hash race window.'
    Write-TestAtomicUtf8 -Path $largeRaceHooksPath -Text $largeUninstallEditText
    $largeUninstallResult = Wait-TestTransactionJob -Job $largeUninstallJob -Description 'Large-config CAS uninstaller' -TimeoutSeconds 45
    Assert-True (-not $largeUninstallResult.Succeeded) 'Large-config uninstaller reported success after a post-hash external replace.'
    Assert-True ([string] $largeUninstallResult.Message -match 'Concurrent edit detected') 'Large-config uninstaller did not report its CAS conflict.'
    $largeUninstallConfiguration = [System.IO.File]::ReadAllText($largeRaceHooksPath, $utf8NoBom) | ConvertFrom-Json
    Assert-Equal $largeUninstallConfiguration.marker 'external-after-hash-uninstall' 'Uninstaller CAS lost the post-hash external marker.'
    $largeUninstallOwned = 0
    foreach ($event in @('UserPromptSubmit', 'Stop', 'SessionStart', 'SessionEnd')) {
        $property = $largeUninstallConfiguration.hooks.PSObject.Properties[$event]
        if ($null -ne $property) {
            $largeUninstallOwned += @($property.Value | ForEach-Object { $_.hooks } | Where-Object { $_.command -match 'windows-terminal-workspace-history-v1-' }).Count
        }
    }
    Assert-Equal $largeUninstallOwned 4 'Uninstaller CAS conflict did not restore the externally replaced installed config.'
    Assert-Equal @([System.IO.Directory]::EnumerateFiles($largeHooksDirectory, 'workspace-history-*.ps1')).Count 1 'Uninstaller CAS conflict removed a still-referenced versioned script.'
    $largePadding = $null
    $largeBaselineText = $null
    $largeInstallEditText = $null
    $largeInstalledText = $null
    $largeUninstallEditText = $null

    # Invalid existing JSON must be preserved byte-for-byte.
    $invalidCodexHome = [System.IO.Path]::Combine($testRoot, 'invalid-codex-home')
    $null = [System.IO.Directory]::CreateDirectory($invalidCodexHome)
    $invalidHooksPath = [System.IO.Path]::Combine($invalidCodexHome, 'hooks.json')
    [System.IO.File]::WriteAllText($invalidHooksPath, '{invalid-json', $utf8NoBom)
    $installThrew = $false
    try { $null = & $installScript -CodexHome $invalidCodexHome 6>&1 } catch { $installThrew = $true }
    Assert-True $installThrew 'Installer must reject invalid existing JSON.'
    Assert-Equal ([System.IO.File]::ReadAllText($invalidHooksPath, $utf8NoBom)) '{invalid-json' 'Installer overwrote invalid existing JSON.'

    Write-Host "PASS: $assertions assertions"
}
finally {
    foreach ($job in @($jobs)) {
        try { Stop-Job -Job $job -ErrorAction SilentlyContinue } catch {}
        try { Remove-Job -Job $job -Force -ErrorAction SilentlyContinue } catch {}
    }

    # Recursive cleanup is limited to the exact random directory created under
    # the OS temp root by this test run.
    $resolvedTestRoot = [System.IO.Path]::GetFullPath($testRoot)
    $expectedPrefix = $temporaryBase.TrimEnd([char[]] '\/') + [System.IO.Path]::DirectorySeparatorChar
    if ($resolvedTestRoot.StartsWith($expectedPrefix, [System.StringComparison]::OrdinalIgnoreCase) -and
        [System.IO.Path]::GetFileName($resolvedTestRoot).StartsWith('wt-workspace-history-test-', [System.StringComparison]::Ordinal)) {
        Remove-Item -LiteralPath $resolvedTestRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
