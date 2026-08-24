param(
    [string] $CodexHome,

    # Deterministic race-test seam; normal uninstalls leave this at zero.
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

function Assert-HooksFileUnchanged {
    param(
        [Parameter(Mandatory)][string] $Path,
        [Parameter(Mandatory)][object] $Baseline
    )

    $current = Get-HooksFileState $Path
    if ($current.Exists -ne $Baseline.Exists -or
        -not [string]::Equals([string] $current.Hash, [string] $Baseline.Hash, [System.StringComparison]::Ordinal)) {
        throw "Concurrent edit detected; hooks configuration was left unchanged by this uninstaller: $Path"
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
        return $false
    }

    $changed = $false
    $remainingGroups = [System.Collections.Generic.List[object]]::new()
    foreach ($group in @($Hooks[$Event])) {
        if ($group -isnot [System.Collections.IDictionary] -or -not $group.Contains('hooks')) {
            $remainingGroups.Add($group)
            continue
        }

        $remainingHandlers = [System.Collections.Generic.List[object]]::new()
        $removedFromGroup = $false
        foreach ($handler in @($group['hooks'])) {
            if (Test-OwnedHandler $handler) {
                $changed = $true
                $removedFromGroup = $true
            }
            else {
                $remainingHandlers.Add($handler)
            }
        }

        if ($remainingHandlers.Count -gt 0) {
            $group['hooks'] = @($remainingHandlers)
            $remainingGroups.Add($group)
        }
        elseif (-not $removedFromGroup) {
            $remainingGroups.Add($group)
        }
    }

    if ($remainingGroups.Count -gt 0) {
        $Hooks[$Event] = @($remainingGroups)
    }
    else {
        $Hooks.Remove($Event)
    }
    return $changed
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
                throw "Concurrent edit detected after the uninstaller replace; the newer hooks configuration was preserved and the captured edit was retained at: $backupPath"
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

function Remove-VerifiedInstalledScripts {
    param(
        [Parameter(Mandatory)][string] $HooksDirectory,
        [Parameter(Mandatory)][AllowEmptyString()][string] $ConfigurationText
    )

    if (-not [System.IO.Directory]::Exists($HooksDirectory)) {
        return 0
    }

    $removed = 0
    foreach ($path in [System.IO.Directory]::EnumerateFiles($HooksDirectory, 'workspace-history-*.ps1')) {
        $name = [System.IO.Path]::GetFileName($path)
        if ($name -notmatch '^workspace-history-([0-9a-f]{16})\.ps1$') {
            continue
        }
        if ($ConfigurationText.IndexOf($name, [System.StringComparison]::OrdinalIgnoreCase) -ge 0) {
            # Conservative: never remove a script still named anywhere in the
            # committed configuration, even in an unfamiliar custom hook group.
            continue
        }

        try {
            $bytes = [System.IO.File]::ReadAllBytes($path)
            $hash = [Convert]::ToHexString([System.Security.Cryptography.SHA256]::HashData($bytes)).ToLowerInvariant()
            if ($hash.StartsWith($Matches[1], [System.StringComparison]::Ordinal)) {
                [System.IO.File]::Delete($path)
                $removed++
            }
        }
        catch {
            # Keep any file that cannot be positively verified as installer-owned.
        }
    }
    return $removed
}

$resolvedCodexHome = Resolve-CodexHome $CodexHome
$hooksDirectory = [System.IO.Path]::Combine($resolvedCodexHome, 'hooks')
$configurationPath = [System.IO.Path]::Combine($resolvedCodexHome, 'hooks.json')
$configurationChanged = $false
$removedScripts = 0
$transactionMutex = $null
try {
    # The installer uses this exact mutex identity. Keep configuration changes
    # and verified script cleanup in one cross-process transaction.
    $transactionMutex = Enter-TransactionLock $resolvedCodexHome
    $baseline = Get-HooksFileState $configurationPath
    $configurationText = ''

    if ($baseline.Exists) {
        $text = $utf8Strict.GetString($baseline.Bytes)
        if ([string]::IsNullOrWhiteSpace($text)) {
            throw "Existing hooks configuration is empty; it was left unchanged: $configurationPath"
        }
        try {
            $configuration = $text | ConvertFrom-Json -AsHashtable
        }
        catch {
            throw "Existing hooks configuration is not valid JSON; it was left unchanged: $configurationPath"
        }
        if ($configuration -isnot [System.Collections.IDictionary]) {
            throw "Existing hooks configuration must be a JSON object; it was left unchanged: $configurationPath"
        }

        if ($configuration.Contains('hooks')) {
            if ($configuration['hooks'] -isnot [System.Collections.IDictionary]) {
                throw "The 'hooks' member in $configurationPath must be a JSON object; it was left unchanged."
            }
            foreach ($event in $events) {
                if (Remove-OwnedHandlers -Hooks $configuration['hooks'] -Event $event) {
                    $configurationChanged = $true
                }
            }
        }

        if ($configurationChanged) {
            # Commands are removed before scripts. A failed write therefore
            # leaves the old command and its script together, never dangling.
            Assert-HooksFileUnchanged -Path $configurationPath -Baseline $baseline
            if ($BeforeCommitDelayMilliseconds -gt 0) {
                Start-Sleep -Milliseconds $BeforeCommitDelayMilliseconds
            }
            $configurationText = ($configuration | ConvertTo-Json -Depth 64) + [Environment]::NewLine
            $configurationBytes = $utf8NoBom.GetBytes($configurationText)
            $baseline = Set-HooksConfigurationCas -Path $configurationPath -Bytes $configurationBytes -Baseline $baseline -Actor 'uninstaller'
        }
        else {
            if ($BeforeCommitDelayMilliseconds -gt 0) {
                Start-Sleep -Milliseconds $BeforeCommitDelayMilliseconds
            }
            $configurationText = $text
        }
    }

    # User editors do not take our mutex. Recheck immediately before destructive
    # script cleanup and fail safely if the committed/read config changed.
    Assert-HooksFileUnchanged -Path $configurationPath -Baseline $baseline
    if ($baseline.Exists) {
        # Hold a non-write/non-delete share on the verified config while scripts
        # are removed, closing the last external-editor window for dangling
        # commands. Absence cannot be locked, so orphan cleanup is conservative.
        $configurationGuard = $null
        try {
            $configurationGuard = [System.IO.FileStream]::new(
                $configurationPath,
                [System.IO.FileMode]::Open,
                [System.IO.FileAccess]::Read,
                [System.IO.FileShare]::Read)
            $guardHash = [Convert]::ToHexString(
                [System.Security.Cryptography.SHA256]::HashData($configurationGuard)).ToLowerInvariant()
            if (-not [string]::Equals($guardHash, [string] $baseline.Hash, [System.StringComparison]::Ordinal)) {
                throw "Concurrent edit detected before script cleanup; installed scripts were retained: $configurationPath"
            }
            $removedScripts = Remove-VerifiedInstalledScripts -HooksDirectory $hooksDirectory -ConfigurationText $configurationText
        }
        finally {
            if ($null -ne $configurationGuard) {
                $configurationGuard.Dispose()
            }
        }
    }
}
finally {
    Exit-TransactionLock $transactionMutex
}

Write-Host "Removed $removedScripts verified workspace-history script(s)."
if ($configurationChanged) {
    Write-Host "Removed only Windows Terminal workspace-history handlers from $configurationPath; all unrelated hooks were preserved."
}
else {
    Write-Host 'No installed Windows Terminal workspace-history handlers were present.'
}
Write-Host 'The durable inbox was retained. Use /hooks in Codex to verify the remaining hook configuration.'
