param(
    [string] $CodexHome,

    # Deterministic race-test seam; normal installs leave this at zero.
    [ValidateRange(0, 10000)]
    [int] $BeforeCommitDelayMilliseconds = 0
)

$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'
$utf8NoBom = [System.Text.UTF8Encoding]::new($false)
$utf8Strict = [System.Text.UTF8Encoding]::new($false, $true)
$installIdPrefix = 'windows-terminal-workspace-history-v1-'
$events = @('UserPromptSubmit', 'Stop', 'SessionStart', 'SessionEnd')
$transactionMutexPrefix = 'Local\OpenAI.Codex.WorkspaceHistoryInstall.v1.'

function Resolve-CodexHome {
    param([AllowNull()][string] $Value)

    if ([string]::IsNullOrWhiteSpace($Value)) {
        $profile = [Environment]::GetFolderPath([Environment+SpecialFolder]::UserProfile)
        if ([string]::IsNullOrWhiteSpace($profile)) {
            throw 'Unable to resolve the current user profile.'
        }
        $Value = [System.IO.Path]::Combine($profile, '.codex')
    }
    return [System.IO.Path]::GetFullPath($Value)
}

function Get-HooksFileState {
    param([Parameter(Mandatory)][string] $Path)

    if (-not [System.IO.File]::Exists($Path)) {
        return [pscustomobject]@{
            Exists = $false
            Hash = ''
            Bytes = [byte[]]::new(0)
        }
    }

    $bytes = [System.IO.File]::ReadAllBytes($Path)
    return [pscustomobject]@{
        Exists = $true
        Hash = [Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
        Bytes = $bytes
    }
}

function Read-HooksConfiguration {
    param([Parameter(Mandatory)][string] $Path)

    $state = Get-HooksFileState $Path
    if (-not $state.Exists) {
        return [pscustomobject]@{
            Configuration = [ordered]@{}
            State = $state
        }
    }

    $text = $utf8Strict.GetString($state.Bytes)
    if ([string]::IsNullOrWhiteSpace($text)) {
        throw "Existing hooks configuration is empty: $Path"
    }
    try {
        $configuration = $text | ConvertFrom-Json -AsHashtable
    }
    catch {
        throw "Existing hooks configuration is not valid JSON; it was left unchanged: $Path"
    }
    if ($configuration -isnot [System.Collections.IDictionary]) {
        throw "Existing hooks configuration must be a JSON object; it was left unchanged: $Path"
    }
    return [pscustomobject]@{
        Configuration = $configuration
        State = $state
    }
}

function Assert-HooksFileUnchanged {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][object] $Baseline
    )

    $current = Get-HooksFileState $Path
    if ($current.Exists -ne $Baseline.Exists -or
        -not [string]::Equals([string] $current.Hash, [string] $Baseline.Hash, [System.StringComparison]::Ordinal)) {
        throw "Concurrent edit detected; hooks configuration was left unchanged by this installer: $Path"
    }
}

function Get-TransactionMutexName {
    param([Parameter(Mandatory)][string] $ResolvedCodexHome)

    $identity = $ResolvedCodexHome.TrimEnd([char[]] '\/').ToUpperInvariant()
    $hash = [Convert]::ToHexString(
        [System.Security.Cryptography.SHA256]::HashData($utf8NoBom.GetBytes($identity))).ToLowerInvariant()
    return $transactionMutexPrefix + $hash.Substring(0, 32)
}

function Enter-TransactionLock {
    param([Parameter(Mandatory)][string] $ResolvedCodexHome)

    $mutex = [System.Threading.Mutex]::new($false, (Get-TransactionMutexName $ResolvedCodexHome))
    $taken = $false
    try {
        try {
            $taken = $mutex.WaitOne(15000)
        }
        catch [System.Threading.AbandonedMutexException] {
            $taken = $true
        }
        if (-not $taken) {
            throw 'Timed out waiting for another workspace-history install/uninstall transaction.'
        }
        return $mutex
    }
    catch {
        $mutex.Dispose()
        throw
    }
}

function Exit-TransactionLock {
    param([AllowNull()][System.Threading.Mutex] $Mutex)

    if ($null -eq $Mutex) {
        return
    }
    try { $Mutex.ReleaseMutex() } finally { $Mutex.Dispose() }
}

function Test-OwnedHandler {
    param([AllowNull()][object] $Handler)

    if ($Handler -isnot [System.Collections.IDictionary] -or -not $Handler.Contains('command')) {
        return $false
    }
    $command = [string] $Handler['command']
    return $command -match '(?i)workspace-history-[0-9a-f]{16}\.ps1' -and
        $command.Contains($installIdPrefix, [System.StringComparison]::OrdinalIgnoreCase)
}

function Remove-OwnedHandlers {
    param(
        [Parameter(Mandatory)][System.Collections.IDictionary] $Hooks,
        [Parameter(Mandatory)][string] $Event
    )

    if (-not $Hooks.Contains($Event)) {
        return
    }

    $remainingGroups = [System.Collections.Generic.List[object]]::new()
    foreach ($group in @($Hooks[$Event])) {
        if ($group -isnot [System.Collections.IDictionary] -or -not $group.Contains('hooks')) {
            $remainingGroups.Add($group)
            continue
        }

        $remainingHandlers = [System.Collections.Generic.List[object]]::new()
        $removed = $false
        foreach ($handler in @($group['hooks'])) {
            if (Test-OwnedHandler $handler) {
                $removed = $true
            }
            else {
                $remainingHandlers.Add($handler)
            }
        }

        if ($remainingHandlers.Count -gt 0) {
            $group['hooks'] = @($remainingHandlers)
            $remainingGroups.Add($group)
        }
        elseif (-not $removed) {
            # Preserve pre-existing empty/odd groups verbatim.
            $remainingGroups.Add($group)
        }
    }

    if ($remainingGroups.Count -gt 0) {
        $Hooks[$Event] = @($remainingGroups)
    }
    else {
        $Hooks.Remove($Event)
    }
}

function Quote-PowerShellLiteral {
    param([Parameter(Mandatory)][string] $Value)

    return "'" + $Value.Replace("'", "''") + "'"
}

function New-HookCommand {
    param(
        [Parameter(Mandatory)][string] $ScriptPath,
        [Parameter(Mandatory)][string] $Event,
        [Parameter(Mandatory)][string] $InstallId
    )

    $scriptLiteral = Quote-PowerShellLiteral $ScriptPath
    $eventLiteral = Quote-PowerShellLiteral $Event
    $idLiteral = Quote-PowerShellLiteral $InstallId
    if ($Event -eq 'SessionEnd') {
        # Keep the marker/path in the exact trusted command so uninstall can
        # identify ownership, but do not start the hook implementation at all.
        return 'pwsh -NoProfile -NonInteractive -Command "$hookScript = {0}; $installId = {1}; exit 0"' -f
            $scriptLiteral, $idLiteral
    }
    $fallback = if ($Event -eq 'Stop') {
        " else { [Console]::Out.WriteLine('{}') }"
    }
    else {
        ''
    }

    return 'pwsh -NoProfile -NonInteractive -Command "$hookScript = {0}; if (Test-Path -LiteralPath $hookScript) {{ & $hookScript -EventName {1} -InstallId {2} }}{3}"' -f
        $scriptLiteral, $eventLiteral, $idLiteral, $fallback
}

function New-MatcherGroup {
    param(
        [Parameter(Mandatory)][string] $Event,
        [Parameter(Mandatory)][string] $Command
    )

    $timeout = if ($Event -eq 'SessionEnd') { 3 } else { 5 }
    $handler = [ordered]@{
        type = 'command'
        command = $Command
        timeout = $timeout
    }
    $group = [ordered]@{
        hooks = @($handler)
    }
    if ($Event -eq 'SessionStart') {
        $group = [ordered]@{
            matcher = 'startup|resume|clear|compact'
            hooks = @($handler)
        }
    }
    elseif ($Event -eq 'SessionEnd') {
        $group = [ordered]@{
            matcher = 'other'
            hooks = @($handler)
        }
    }
    return $group
}

function Get-BytesHash {
    param([Parameter(Mandatory)][byte[]] $Bytes)

    return [Convert]::ToHexString(
        [System.Security.Cryptography.SHA256]::HashData($Bytes)).ToLowerInvariant()
}

function New-StagedFile {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][byte[]] $Bytes
    )

    $directory = [System.IO.Path]::GetDirectoryName($Path)
    $null = [System.IO.Directory]::CreateDirectory($directory)
    $temporaryPath = [System.IO.Path]::Combine(
        $directory,
        ([guid]::NewGuid().ToString('N') + '.tmp'))
    $stream = $null
    try {
        $stream = [System.IO.FileStream]::new(
            $temporaryPath,
            [System.IO.FileMode]::CreateNew,
            [System.IO.FileAccess]::Write,
            [System.IO.FileShare]::None,
            4096,
            [System.IO.FileOptions]::WriteThrough)
        $stream.Write($Bytes, 0, $Bytes.Length)
        $stream.Flush($true)
        $stream.Dispose()
        $stream = $null
        return $temporaryPath
    }
    catch {
        if ($null -ne $stream) {
            $stream.Dispose()
        }
        if ([System.IO.File]::Exists($temporaryPath)) {
            try { [System.IO.File]::Delete($temporaryPath) } catch {}
        }
        throw
    }
}

function Undo-FailedHooksCas {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][string] $CapturedPath,
        [Parameter(Mandatory)][string] $CapturedHash,
        [Parameter(Mandatory)][string] $OursHash
    )

    # File.Replace is an atomic exchange, but it has no conditional form. Use
    # the file it displaces as the next exchange candidate. With a finite number
    # of external writes, this restores the newest displaced edit without ever
    # discarding it. Keep a separate copy of the first edit until recovery is
    # complete so even a continuously racing editor cannot destroy user bytes.
    $directory = [System.IO.Path]::GetDirectoryName($Path)
    $name = [System.IO.Path]::GetFileName($Path)
    $preservedPath = [System.IO.Path]::Combine(
        $directory,
        ($name + '.' + [guid]::NewGuid().ToString('N') + '.conflict.bak'))
    try {
        [System.IO.File]::Copy($CapturedPath, $preservedPath, $false)
    }
    catch {
        return [pscustomobject]@{
            Restored = $false
            CapturedConsumed = $false
            RetainedPaths = @($CapturedPath)
        }
    }

    $candidatePath = $CapturedPath
    $candidateHash = $CapturedHash
    $expectedHash = $OursHash
    for ($attempt = 0; $attempt -lt 32; $attempt++) {
        $displacedPath = [System.IO.Path]::Combine(
            $directory,
            ($name + '.' + [guid]::NewGuid().ToString('N') + '.cas-displaced'))
        try {
            [System.IO.File]::Replace($candidatePath, $Path, $displacedPath)
            $candidatePath = $null
        }
        catch {
            if ($attempt -eq 0) {
                try { [System.IO.File]::Delete($preservedPath) } catch {}
                return [pscustomobject]@{
                    Restored = $false
                    CapturedConsumed = $false
                    RetainedPaths = @($CapturedPath)
                }
            }
            return [pscustomobject]@{
                Restored = $false
                CapturedConsumed = $true
                RetainedPaths = @($preservedPath, $candidatePath) | Where-Object { -not [string]::IsNullOrEmpty($_) }
            }
        }

        $displaced = Get-HooksFileState $displacedPath
        if ($displaced.Exists -and
            [string]::Equals([string] $displaced.Hash, $expectedHash, [System.StringComparison]::Ordinal)) {
            try { [System.IO.File]::Delete($displacedPath) } catch {}
            if ($attempt -eq 0) {
                # The destination was still ours: the exact external file that
                # the failed CAS captured is back in place.
                try { [System.IO.File]::Delete($preservedPath) } catch {}
                return [pscustomobject]@{
                    Restored = $true
                    CapturedConsumed = $true
                    RetainedPaths = @()
                }
            }

            # A post-replace external edit raced recovery. It is current again;
            # retain the originally captured edit as an additional safety copy.
            return [pscustomobject]@{
                Restored = $false
                CapturedConsumed = $true
                RetainedPaths = @($preservedPath)
            }
        }

        # Another external replace won between exchanges. The displaced file is
        # newer than our candidate, so make it the next candidate and verify the
        # content that this iteration put in the destination.
        $expectedHash = $candidateHash
        $candidateHash = [string] $displaced.Hash
        $candidatePath = $displacedPath
    }

    return [pscustomobject]@{
        Restored = $false
        CapturedConsumed = $true
        RetainedPaths = @($preservedPath, $candidatePath) | Where-Object { -not [string]::IsNullOrEmpty($_) }
    }
}

function Set-HooksConfigurationCas {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][byte[]] $Bytes,
        [Parameter(Mandatory)][object] $Baseline,
        [Parameter(Mandatory)][string] $Actor
    )

    $oursHash = Get-BytesHash $Bytes
    $temporaryPath = New-StagedFile -Path $Path -Bytes $Bytes
    $backupPath = $null
    $retainBackup = $false
    try {
        if (-not $Baseline.Exists) {
            try {
                # The two-argument overload is deliberately no-overwrite. If an
                # editor creates hooks.json after our absent baseline, it wins.
                [System.IO.File]::Move($temporaryPath, $Path)
                $temporaryPath = $null
            }
            catch [System.IO.IOException] {
                throw "Concurrent edit detected; hooks configuration was left unchanged by this ${Actor}: $Path"
            }

            $current = Get-HooksFileState $Path
            if (-not $current.Exists -or
                -not [string]::Equals([string] $current.Hash, $oursHash, [System.StringComparison]::Ordinal)) {
                throw "Concurrent edit detected after the ${Actor} commit; the newer hooks configuration was preserved: $Path"
            }
            return $current
        }

        $directory = [System.IO.Path]::GetDirectoryName($Path)
        $backupPath = [System.IO.Path]::Combine(
            $directory,
            ([System.IO.Path]::GetFileName($Path) + '.' + [guid]::NewGuid().ToString('N') + '.cas-backup'))
        try {
            # This is the compare-and-swap point. The backup is the exact file
            # that existed at the same instant ours became the destination.
            [System.IO.File]::Replace($temporaryPath, $Path, $backupPath)
            $temporaryPath = $null
            $retainBackup = $true
        }
        catch {
            throw "Concurrent edit detected; hooks configuration was left unchanged by this ${Actor}: $Path"
        }

        $captured = Get-HooksFileState $backupPath
        if (-not $captured.Exists -or
            -not [string]::Equals([string] $captured.Hash, [string] $Baseline.Hash, [System.StringComparison]::Ordinal)) {
            $currentBeforeUndo = Get-HooksFileState $Path
            if (-not $currentBeforeUndo.Exists -or
                -not [string]::Equals([string] $currentBeforeUndo.Hash, $oursHash, [System.StringComparison]::Ordinal)) {
                # The editor wrote again after our replace. Its newest file is
                # already current, so do not roll it back; retain the atomically
                # captured earlier edit as a recovery copy.
                throw "Concurrent edit detected after the installer replace; the newer hooks configuration was preserved and the captured edit was retained at: $backupPath"
            }
            $capturedHash = if ($captured.Exists) { [string] $captured.Hash } else { '' }
            $undo = Undo-FailedHooksCas -Path $Path -CapturedPath $backupPath -CapturedHash $capturedHash -OursHash $oursHash
            if ($undo.CapturedConsumed) {
                $backupPath = $null
            }
            if ($undo.Restored) {
                throw "Concurrent edit detected; the exact external hooks configuration was restored by this ${Actor}: $Path"
            }
            $retained = @($undo.RetainedPaths) -join ', '
            throw "Concurrent edit detected; a newer hooks configuration was preserved and captured edits were retained at: $retained"
        }

        # The CAS succeeded against the expected baseline. If an editor replaced
        # ours afterwards, it is newer and must remain the destination.
        $retainBackup = $false
        try { [System.IO.File]::Delete($backupPath) } catch {}
        $backupPath = $null
        $current = Get-HooksFileState $Path
        if (-not $current.Exists -or
            -not [string]::Equals([string] $current.Hash, $oursHash, [System.StringComparison]::Ordinal)) {
            throw "Concurrent edit detected after the ${Actor} commit; the newer hooks configuration was preserved: $Path"
        }
        return $current
    }
    finally {
        if (-not [string]::IsNullOrEmpty($temporaryPath) -and [System.IO.File]::Exists($temporaryPath)) {
            try { [System.IO.File]::Delete($temporaryPath) } catch {}
        }
        if (-not $retainBackup -and -not [string]::IsNullOrEmpty($backupPath) -and
            [System.IO.File]::Exists($backupPath)) {
            try { [System.IO.File]::Delete($backupPath) } catch {}
        }
    }
}

function Ensure-VersionedHookScript {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][byte[]] $Bytes,
        [Parameter(Mandatory)][string] $ExpectedHash
    )

    if ([System.IO.File]::Exists($Path)) {
        $existingHash = Get-BytesHash ([System.IO.File]::ReadAllBytes($Path))
        if (-not [string]::Equals($existingHash, $ExpectedHash, [System.StringComparison]::Ordinal)) {
            throw "Refusing to overwrite an unexpected versioned hook script: $Path"
        }
        return $false
    }

    $temporaryPath = New-StagedFile -Path $Path -Bytes $Bytes
    try {
        try {
            # A content-addressed script is immutable. Never overwrite a file
            # that appeared after the existence check.
            [System.IO.File]::Move($temporaryPath, $Path)
            $temporaryPath = $null
            return $true
        }
        catch [System.IO.IOException] {
            if (-not [System.IO.File]::Exists($Path)) {
                throw
            }
            $existingHash = Get-BytesHash ([System.IO.File]::ReadAllBytes($Path))
            if (-not [string]::Equals($existingHash, $ExpectedHash, [System.StringComparison]::Ordinal)) {
                throw "Refusing to overwrite an unexpected versioned hook script: $Path"
            }
            return $false
        }
    }
    finally {
        if (-not [string]::IsNullOrEmpty($temporaryPath) -and [System.IO.File]::Exists($temporaryPath)) {
            try { [System.IO.File]::Delete($temporaryPath) } catch {}
        }
    }
}

function Remove-NewHookScriptIfUnreferenced {
    param(
        [Parameter(Mandatory)][string] $ScriptPath,
        [Parameter(Mandatory)][string] $ExpectedHash,
        [Parameter(Mandatory)][string] $ConfigurationPath
    )

    if (-not [System.IO.File]::Exists($ScriptPath)) {
        return
    }

    $configurationGuard = $null
    try {
        $namesScript = $false
        if ([System.IO.File]::Exists($ConfigurationPath)) {
            # Deny writes/deletes while deciding whether cleanup is safe. An
            # external atomic replace either happened before this guard (and is
            # observed) or must wait/fail until after the script decision.
            $configurationGuard = [System.IO.FileStream]::new(
                $ConfigurationPath,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Read,
                [System.IO.FileShare]::Read)
            if ($configurationGuard.Length -gt [int]::MaxValue) {
                return
            }
            $configurationBytes = [byte[]]::new([int] $configurationGuard.Length)
            $offset = 0
            while ($offset -lt $configurationBytes.Length) {
                $read = $configurationGuard.Read($configurationBytes, $offset, $configurationBytes.Length - $offset)
                if ($read -le 0) { throw 'Unexpected end of hooks configuration during rollback.' }
                $offset += $read
            }
            $namesScript = $utf8Strict.GetString($configurationBytes).IndexOf(
                [System.IO.Path]::GetFileName($ScriptPath),
                [System.StringComparison]::OrdinalIgnoreCase) -ge 0
        }

        if (-not $namesScript) {
            $scriptHash = Get-BytesHash ([System.IO.File]::ReadAllBytes($ScriptPath))
            if ([string]::Equals($scriptHash, $ExpectedHash, [System.StringComparison]::Ordinal)) {
                [System.IO.File]::Delete($ScriptPath)
            }
        }
    }
    finally {
        if ($null -ne $configurationGuard) {
            $configurationGuard.Dispose()
        }
    }
}

$resolvedCodexHome = Resolve-CodexHome $CodexHome
$hooksDirectory = [System.IO.Path]::Combine($resolvedCodexHome, 'hooks')
$configurationPath = [System.IO.Path]::Combine($resolvedCodexHome, 'hooks.json')
$sourceScript = [System.IO.Path]::Combine($PSScriptRoot, 'workspace-history.ps1')
if (-not [System.IO.File]::Exists($sourceScript)) {
    throw "Hook implementation is missing: $sourceScript"
}

$sourceBytes = [System.IO.File]::ReadAllBytes($sourceScript)
$fullHash = [Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($sourceBytes)).ToLowerInvariant()
$shortHash = $fullHash.Substring(0, 16)
$installId = $installIdPrefix + $shortHash
$installedScript = [System.IO.Path]::Combine($hooksDirectory, "workspace-history-$shortHash.ps1")
$transactionMutex = $null
try {
    # One named mutex is shared with the uninstaller and covers the complete
    # read-modify-write plus versioned-script transaction.
    $transactionMutex = Enter-TransactionLock $resolvedCodexHome

    # Parse before writing anything. Invalid user JSON is never replaced, and
    # the exact bytes read become the optimistic-concurrency baseline.
    $readResult = Read-HooksConfiguration $configurationPath
    $configuration = $readResult.Configuration
    $baseline = $readResult.State

    if (-not $configuration.Contains('hooks')) {
        $configuration['hooks'] = [ordered]@{}
    }
    if ($configuration['hooks'] -isnot [System.Collections.IDictionary]) {
        throw "The 'hooks' member in $configurationPath must be a JSON object; no changes were made."
    }
    $hooks = $configuration['hooks']

    foreach ($event in $events) {
        Remove-OwnedHandlers -Hooks $hooks -Event $event
        $command = New-HookCommand -ScriptPath $installedScript -Event $event -InstallId $installId
        $group = New-MatcherGroup -Event $event -Command $command
        $groups = if ($hooks.Contains($event)) { @($hooks[$event]) } else { @() }
        $hooks[$event] = @($groups) + @($group)
    }

    Assert-HooksFileUnchanged -Path $configurationPath -Baseline $baseline
    # Script first prevents a committed command from ever pointing at a missing
    # file. Its content-addressed name is created with no-overwrite semantics.
    $scriptCreated = Ensure-VersionedHookScript -Path $installedScript -Bytes $sourceBytes -ExpectedHash $fullHash
    try {
        # This fast check gives a clear early conflict. Set-HooksConfigurationCas
        # is still authoritative and closes the gap after this hash comparison.
        Assert-HooksFileUnchanged -Path $configurationPath -Baseline $baseline
        if ($BeforeCommitDelayMilliseconds -gt 0) {
            Start-Sleep -Milliseconds $BeforeCommitDelayMilliseconds
        }
        $configurationJson = $configuration | ConvertTo-Json -Depth 64
        $configurationBytes = $utf8NoBom.GetBytes($configurationJson + [Environment]::NewLine)
        $null = Set-HooksConfigurationCas -Path $configurationPath -Bytes $configurationBytes -Baseline $baseline -Actor 'installer'
    }
    catch {
        if ($scriptCreated) {
            try {
                Remove-NewHookScriptIfUnreferenced -ScriptPath $installedScript -ExpectedHash $fullHash -ConfigurationPath $configurationPath
            } catch {
                # Never replace the primary concurrent-edit failure with cleanup.
            }
        }
        throw
    }
}
finally {
    Exit-TransactionLock $transactionMutex
}

Write-Host "Installed Windows Terminal workspace-history hooks in $configurationPath"
Write-Host 'Codex will skip these non-managed hooks until you open /hooks, review the exact commands, and trust them.'
Write-Host 'Do not bypass hook trust for routine use. Script updates use a new hashed command path and require review again.'
