[CmdletBinding()]
param(
    [ValidateSet('Check', 'Prepare', 'Continue', 'Abort', 'SelfTest')]
    [string]$Mode = 'Check',

    [string]$IntegrationBranch = 'main',

    [string]$RemoteName = 'upstream',

    [string]$UpstreamUrl = 'https://github.com/microsoft/terminal.git',

    [string]$UpstreamRef,

    [string]$SyncBranch,

    [string]$RepositoryRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')),

    [switch]$NoFetch,

    [switch]$SkipValidation,

    [switch]$SkipPortable,

    [switch]$AllowUnsafeSkip
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$script:Git = $null
$script:Pwsh = $null
$script:RepoRoot = $null
$script:GitDirectory = $null
$script:ReportDirectory = $null
$script:ReportPath = $null
$script:StatePath = $null

function Invoke-GitCapture
{
    param(
        [Parameter(Mandatory)]
        [string[]]$Arguments,

        [switch]$AllowFailure
    )

    $output = @(& $script:Git -C $script:RepoRoot @Arguments 2>&1 | ForEach-Object { $_.ToString() })
    $exitCode = $LASTEXITCODE
    if (-not $AllowFailure -and $exitCode -ne 0)
    {
        throw "git $($Arguments -join ' ') failed with exit code ${exitCode}: $($output -join [Environment]::NewLine)"
    }

    [pscustomobject]@{
        ExitCode = $exitCode
        Output = $output
    }
}

function Invoke-GitCommand
{
    param([Parameter(Mandatory)][string[]]$Arguments)

    & $script:Git -C $script:RepoRoot @Arguments
    if ($LASTEXITCODE -ne 0)
    {
        throw "git $($Arguments -join ' ') failed with exit code $LASTEXITCODE."
    }
}

function Get-GitScalar
{
    param([Parameter(Mandatory)][string[]]$Arguments)

    $result = Invoke-GitCapture -Arguments $Arguments
    return ($result.Output -join [Environment]::NewLine).Trim()
}

function Initialize-Repository
{
    $script:Git = (Get-Command git.exe -ErrorAction Stop).Source
    $script:Pwsh = (Get-Command pwsh.exe -ErrorAction Stop).Source

    $candidate = [System.IO.Path]::GetFullPath($RepositoryRoot)
    $rootResult = @(& $script:Git -C $candidate rev-parse --show-toplevel 2>&1 | ForEach-Object { $_.ToString() })
    if ($LASTEXITCODE -ne 0)
    {
        throw "Not a Git worktree: $candidate"
    }

    $script:RepoRoot = [System.IO.Path]::GetFullPath(($rootResult -join [Environment]::NewLine).Trim())
    $script:GitDirectory = [System.IO.Path]::GetFullPath((Get-GitScalar -Arguments @('rev-parse', '--absolute-git-dir')))
    $script:ReportDirectory = Join-Path $script:GitDirectory 'upstream-sync'
    $script:ReportPath = Join-Path $script:ReportDirectory 'report.md'
    $script:StatePath = Join-Path $script:ReportDirectory 'state.json'
    New-Item -ItemType Directory -Path $script:ReportDirectory -Force | Out-Null
}

function Normalize-RemoteUrl
{
    param([Parameter(Mandatory)][string]$Url)

    $value = $Url.Trim().Replace('\', '/')
    if ($value.StartsWith('git@github.com:', [System.StringComparison]::OrdinalIgnoreCase))
    {
        $value = 'https://github.com/' + $value.Substring('git@github.com:'.Length)
    }
    elseif ($value.StartsWith('ssh://git@github.com/', [System.StringComparison]::OrdinalIgnoreCase))
    {
        $value = 'https://github.com/' + $value.Substring('ssh://git@github.com/'.Length)
    }
    elseif (-not [System.Uri]::IsWellFormedUriString($value, [System.UriKind]::Absolute))
    {
        $value = [System.IO.Path]::GetFullPath($value).Replace('\', '/')
    }

    $value = $value.TrimEnd('/')
    if ($value.EndsWith('.git', [System.StringComparison]::OrdinalIgnoreCase))
    {
        $value = $value.Substring(0, $value.Length - 4)
    }
    return $value.ToLowerInvariant()
}

function Ensure-UpstreamRemote
{
    $remote = Invoke-GitCapture -Arguments @('remote', 'get-url', $RemoteName) -AllowFailure
    if ($remote.ExitCode -ne 0)
    {
        Write-Host "Adding $RemoteName remote: $UpstreamUrl" -ForegroundColor Cyan
        Invoke-GitCommand -Arguments @('remote', 'add', '-t', 'main', $RemoteName, $UpstreamUrl)
    }
    else
    {
        $actualUrl = ($remote.Output -join [Environment]::NewLine).Trim()
        if ((Normalize-RemoteUrl -Url $actualUrl) -ne (Normalize-RemoteUrl -Url $UpstreamUrl))
        {
            throw "Remote '$RemoteName' points to '$actualUrl', not the expected '$UpstreamUrl'. Refusing to rewrite it automatically."
        }
    }

    $pushUrls = Invoke-GitCapture -Arguments @('remote', 'get-url', '--push', '--all', $RemoteName) -AllowFailure
    if ($pushUrls.ExitCode -ne 0 -or @($pushUrls.Output).Count -ne 1 -or @($pushUrls.Output)[0] -ne 'DISABLED')
    {
        Invoke-GitCommand -Arguments @('remote', 'set-url', '--push', $RemoteName, 'DISABLED')
    }

    if (-not $NoFetch)
    {
        Write-Host "Fetching Microsoft Terminal main and tags from $RemoteName..." -ForegroundColor Cyan
        Invoke-GitCommand -Arguments @(
            'fetch',
            '--prune',
            '--tags',
            $RemoteName,
            "+refs/heads/main:refs/remotes/$RemoteName/main")
    }
}

function Test-GitMarker
{
    param([Parameter(Mandatory)][string]$Name)
    return Test-Path -LiteralPath (Join-Path $script:GitDirectory $Name)
}

function Assert-NoGitOperation
{
    $markers = @(
        'MERGE_HEAD',
        'CHERRY_PICK_HEAD',
        'REVERT_HEAD',
        'rebase-apply',
        'rebase-merge') | Where-Object { Test-GitMarker -Name $_ }
    if (@($markers).Count -ne 0)
    {
        throw "Another Git operation is active: $($markers -join ', '). Finish or abort it before preparing an upstream sync."
    }
}

function Assert-CleanWorktree
{
    $status = Invoke-GitCapture -Arguments @('status', '--porcelain=v1', '--untracked-files=normal')
    if (@($status.Output).Count -ne 0)
    {
        throw "The worktree must be clean before preparing an upstream sync:`n$($status.Output -join [Environment]::NewLine)"
    }
}

function Get-CurrentBranch
{
    $branch = Get-GitScalar -Arguments @('branch', '--show-current')
    if ([string]::IsNullOrWhiteSpace($branch))
    {
        throw 'Detached HEAD is not supported for upstream synchronization.'
    }
    return $branch
}

function Resolve-Commit
{
    param([Parameter(Mandatory)][string]$Ref)

    $result = Invoke-GitCapture -Arguments @('rev-parse', '--verify', "$Ref^{commit}") -AllowFailure
    if ($result.ExitCode -ne 0)
    {
        throw "Cannot resolve Git commit '$Ref'. Fetch the remote or choose another ref."
    }
    return ($result.Output -join [Environment]::NewLine).Trim()
}

function Get-SyncMetadata
{
    $integrationSha = Resolve-Commit -Ref "refs/heads/$IntegrationBranch"
    $targetRef = if ([string]::IsNullOrWhiteSpace($UpstreamRef)) { "refs/remotes/$RemoteName/main" } else { $UpstreamRef }
    $upstreamSha = Resolve-Commit -Ref $targetRef
    $mergeBaseResult = Invoke-GitCapture -Arguments @('merge-base', $integrationSha, $upstreamSha) -AllowFailure
    if ($mergeBaseResult.ExitCode -ne 0 -or @($mergeBaseResult.Output).Count -eq 0)
    {
        throw "'$IntegrationBranch' and '$targetRef' do not have a common ancestor. Refusing to merge unrelated histories."
    }

    $counts = Get-GitScalar -Arguments @('rev-list', '--left-right', '--count', "$integrationSha...$upstreamSha")
    $parts = @($counts -split '\s+' | Where-Object { $_ })
    if (@($parts).Count -ne 2)
    {
        throw "Unexpected rev-list count output: $counts"
    }

    $currentUrl = Get-GitScalar -Arguments @('remote', 'get-url', $RemoteName)
    $currentBranch = Get-CurrentBranch
    $currentHeadOnlyCount = [int64](Get-GitScalar -Arguments @('rev-list', '--count', "$integrationSha..HEAD"))
    return [pscustomobject][ordered]@{
        SchemaVersion = 1
        Phase = 'check'
        CreatedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
        UpdatedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
        InitialBranch = $currentBranch
        CurrentBranch = $currentBranch
        CurrentHeadOnlyCount = $currentHeadOnlyCount
        IntegrationBranch = $IntegrationBranch
        RemoteName = $RemoteName
        UpstreamUrl = $currentUrl
        UpstreamRef = $targetRef
        LocalSha = $integrationSha
        UpstreamSha = $upstreamSha
        MergeBase = ($mergeBaseResult.Output -join [Environment]::NewLine).Trim()
        LocalOnlyCount = [int64]$parts[0]
        UpstreamOnlyCount = [int64]$parts[1]
        SyncBranch = ''
        ConflictFiles = @()
        Validation = 'not-run'
        PortableDeployment = 'not-run'
        ResultCommit = ''
    }
}

function Get-UnmergedPaths
{
    $result = Invoke-GitCapture -Arguments @('diff', '--name-only', '--diff-filter=U')
    return @($result.Output | Where-Object { -not [string]::IsNullOrWhiteSpace($_) } | Sort-Object -Unique)
}

function Get-ConflictCategory
{
    param([Parameter(Mandatory)][string]$Path)

    if ($Path -match '^src/cascadia/(TerminalApp/(TerminalPage|TabManagement|AppActionHandlers|TabRowControl|Workspace)|TerminalSettingsModel/(AllShortcutActions|ActionAndArgs|ActionMap|defaults)|CascadiaPackage/)')
    {
        return 'fork hotspot — manually preserve both upstream core behavior and Workspace semantics'
    }
    if ($Path -match '^src/(renderer|terminal|buffer|host|interactivity)/')
    {
        return 'upstream core — prefer Microsoft changes unless the fork intentionally modified this code'
    }
    if ($Path -match '(Generated Files|\.vcxproj|\.sln|packages\.config|Directory\.)')
    {
        return 'build/generated boundary — reconcile inputs and regenerate through the normal build'
    }
    return 'general three-way merge'
}

function Write-SyncArtifacts
{
    param([Parameter(Mandatory)]$State)

    $State.UpdatedAtUtc = [DateTimeOffset]::UtcNow.ToString('o')
    $State.CurrentBranch = Get-CurrentBranch
    $conflicts = @(Get-UnmergedPaths)
    $State.ConflictFiles = $conflicts

    $upstreamResult = Invoke-GitCapture -Arguments @(
        'log',
        '--oneline',
        '--no-decorate',
        '--max-count=50',
        "$($State.LocalSha)..$($State.UpstreamSha)")
    $upstreamCommits = @($upstreamResult.Output)
    $changedResult = Invoke-GitCapture -Arguments @('diff', '--cached', '--name-only')
    $changed = @($changedResult.Output | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })

    $conflictText = if (@($conflicts).Count -eq 0)
    {
        '_None._'
    }
    else
    {
        ($conflicts | ForEach-Object { "- '$($_)' — $(Get-ConflictCategory -Path $_)" }) -join "`n"
    }
    $commitText = if (@($upstreamCommits).Count -eq 0)
    {
        '_No upstream-only commits._'
    }
    else
    {
        ($upstreamCommits | ForEach-Object { "- '$($_)'" }) -join "`n"
    }
    $changedPreview = if (@($changed).Count -eq 0)
    {
        '_No staged paths._'
    }
    else
    {
        (($changed | Select-Object -First 200) | ForEach-Object { "- '$($_)'" }) -join "`n"
    }

    $report = @"
# Microsoft Terminal upstream sync report

- Phase: **$($State.Phase)**
- Integration branch: '$($State.IntegrationBranch)' at '$($State.LocalSha)'
- Upstream: '$($State.UpstreamRef)' at '$($State.UpstreamSha)'
- Merge base: '$($State.MergeBase)'
- Local-only commits: $($State.LocalOnlyCount)
- Upstream-only commits: $($State.UpstreamOnlyCount)
- Current HEAD commits not in integration branch: $($State.CurrentHeadOnlyCount)
- Sync branch: '$($State.SyncBranch)'
- Validation: $($State.Validation)
- Portable deployment: $($State.PortableDeployment)
- Result commit: '$($State.ResultCommit)'

## Conflicts

$conflictText

## Staged path preview

$changedPreview

## Upstream commit preview (maximum 50)

$commitText

## AI handoff

Give the AI this instruction:

> Read 'AGENT.md', 'UPSTREAM.md', and '$($script:ReportPath)' completely. Continue the existing Microsoft Terminal upstream merge. Inspect base, upstream, and fork versions of every conflict; do not resolve an entire hotspot file with blanket ours/theirs. Preserve Microsoft renderer/core bug fixes and the fork's Workspace/Surface, persistence, automation, security, and test contracts. Stage every resolved path, leave unrelated files untouched, then run 'pwsh -NoLogo -NoProfile -File .\tools\upstream\sync-microsoft-terminal.ps1 -Mode Continue'.

## Commands

~~~powershell
# After conflicts are resolved and staged:
pwsh -NoLogo -NoProfile -File .\tools\upstream\sync-microsoft-terminal.ps1 -Mode Continue

# To abandon the merge while keeping the sync branch:
pwsh -NoLogo -NoProfile -File .\tools\upstream\sync-microsoft-terminal.ps1 -Mode Abort
~~~
"@

    [System.IO.File]::WriteAllText($script:ReportPath, $report, [System.Text.UTF8Encoding]::new($false))
    [System.IO.File]::WriteAllText(
        $script:StatePath,
        ($State | ConvertTo-Json -Depth 8),
        [System.Text.UTF8Encoding]::new($false))
    return $script:ReportPath
}

function Read-SyncState
{
    if (-not (Test-Path -LiteralPath $script:StatePath -PathType Leaf))
    {
        throw "Upstream sync state is missing: $($script:StatePath). Run -Mode Prepare first."
    }
    return Get-Content -LiteralPath $script:StatePath -Raw | ConvertFrom-Json
}

function Invoke-RepositoryScript
{
    param(
        [Parameter(Mandatory)][string]$Path,
        [string[]]$Arguments = @()
    )

    Push-Location $script:RepoRoot
    try
    {
        & $script:Pwsh -NoLogo -NoProfile -File $Path @Arguments
        if ($LASTEXITCODE -ne 0)
        {
            throw "Script failed with exit code ${LASTEXITCODE}: $Path"
        }
    }
    finally
    {
        Pop-Location
    }
}

function Invoke-Check
{
    Ensure-UpstreamRemote
    $state = Get-SyncMetadata
    $state.Phase = 'check'
    $report = Write-SyncArtifacts -State $state

    Write-Host "Integration: $($state.IntegrationBranch) $($state.LocalSha)" -ForegroundColor Cyan
    Write-Host "Upstream:    $($state.UpstreamRef) $($state.UpstreamSha)" -ForegroundColor Cyan
    Write-Host "Drift:       local-only=$($state.LocalOnlyCount), upstream-only=$($state.UpstreamOnlyCount)" -ForegroundColor Cyan
    if ($state.CurrentHeadOnlyCount -ne 0)
    {
        Write-Host "Warning: current HEAD has $($state.CurrentHeadOnlyCount) commit(s) not present in '$($state.IntegrationBranch)'. Promote the product branch before Prepare." -ForegroundColor Yellow
    }
    Write-Host "Report:      $report" -ForegroundColor Green
}

function Invoke-Prepare
{
    Assert-NoGitOperation
    Assert-CleanWorktree
    Ensure-UpstreamRemote

    $state = Get-SyncMetadata
    if ($state.UpstreamOnlyCount -eq 0)
    {
        $state.Phase = 'already-current'
        $report = Write-SyncArtifacts -State $state
        Write-Host "'$IntegrationBranch' already contains '$($state.UpstreamRef)'." -ForegroundColor Green
        Write-Host "Report: $report"
        return
    }

    $currentBranch = Get-CurrentBranch
    if ($currentBranch -ne $IntegrationBranch)
    {
        if ($state.CurrentHeadOnlyCount -ne 0)
        {
            throw "Current branch '$currentBranch' has $($state.CurrentHeadOnlyCount) commit(s) not integrated into '$IntegrationBranch'. Merge or fast-forward the product branch first."
        }
    }

    if ([string]::IsNullOrWhiteSpace($SyncBranch))
    {
        $shortSha = $state.UpstreamSha.Substring(0, 10)
        $SyncBranch = "sync/microsoft-terminal/$([DateTimeOffset]::UtcNow.ToString('yyyyMMdd-HHmmss'))-$shortSha"
    }
    $validBranch = Invoke-GitCapture -Arguments @('check-ref-format', '--branch', $SyncBranch) -AllowFailure
    if ($validBranch.ExitCode -ne 0)
    {
        throw "Invalid sync branch name: $SyncBranch"
    }
    $existingBranch = Invoke-GitCapture -Arguments @('show-ref', '--verify', '--quiet', "refs/heads/$SyncBranch") -AllowFailure
    if ($existingBranch.ExitCode -eq 0)
    {
        throw "Sync branch already exists: $SyncBranch"
    }

    $state.Phase = 'preparing'
    $state.InitialBranch = $currentBranch
    $state.SyncBranch = $SyncBranch
    [void](Write-SyncArtifacts -State $state)

    Invoke-GitCommand -Arguments @('switch', '-c', $SyncBranch, $IntegrationBranch)
    Write-Host "Merging Microsoft Terminal $($state.UpstreamSha) without committing..." -ForegroundColor Cyan
    $mergeOutput = @(& $script:Git -C $script:RepoRoot merge --no-ff --no-commit $state.UpstreamSha 2>&1 | ForEach-Object { $_.ToString() })
    $mergeExitCode = $LASTEXITCODE
    $mergeOutput | ForEach-Object { Write-Host $_ }

    $conflicts = @(Get-UnmergedPaths)
    if ($mergeExitCode -ne 0)
    {
        $state.Phase = if (@($conflicts).Count -ne 0) { 'conflicts' } else { 'merge-failed' }
        $report = Write-SyncArtifacts -State $state
        if (@($conflicts).Count -ne 0)
        {
            Write-Host "Merge stopped with $(@($conflicts).Count) conflict(s)." -ForegroundColor Yellow
            Write-Host "AI handoff report: $report" -ForegroundColor Yellow
            exit 2
        }
        throw "Upstream merge failed without producing resolvable conflicts. See $report"
    }

    $state.Phase = 'ready-for-validation'
    $report = Write-SyncArtifacts -State $state
    Write-Host 'Merge applied cleanly and intentionally left uncommitted.' -ForegroundColor Green
    Write-Host "Run -Mode Continue to validate, package, and create the merge commit." -ForegroundColor Green
    Write-Host "Report: $report"
}

function Invoke-Continue
{
    if (($SkipValidation -or $SkipPortable) -and -not $AllowUnsafeSkip)
    {
        throw 'Skipping validation or portable deployment requires -AllowUnsafeSkip. The normal upstream path must run both gates.'
    }

    $state = Read-SyncState
    if (-not (Test-GitMarker -Name 'MERGE_HEAD'))
    {
        throw 'No merge is in progress. Run -Mode Prepare first.'
    }
    if ((Get-CurrentBranch) -ne $state.SyncBranch)
    {
        throw "The merge belongs to '$($state.SyncBranch)', but the current branch is '$(Get-CurrentBranch)'."
    }

    $mergeHead = (Get-Content -LiteralPath (Join-Path $script:GitDirectory 'MERGE_HEAD') -Raw).Trim()
    if ($mergeHead -ne $state.UpstreamSha)
    {
        throw "MERGE_HEAD '$mergeHead' does not match the recorded upstream '$($state.UpstreamSha)'."
    }

    $conflicts = @(Get-UnmergedPaths)
    if (@($conflicts).Count -ne 0)
    {
        $state.Phase = 'conflicts'
        $report = Write-SyncArtifacts -State $state
        throw "Resolve and stage all conflicts before continuing. See $report"
    }

    $unstaged = Invoke-GitCapture -Arguments @('diff', '--quiet', '--ignore-submodules') -AllowFailure
    if ($unstaged.ExitCode -ne 0)
    {
        throw 'Unstaged changes remain. Stage resolved merge files and remove unrelated edits before continuing.'
    }
    $untrackedResult = Invoke-GitCapture -Arguments @('ls-files', '--others', '--exclude-standard')
    $untracked = @($untrackedResult.Output)
    if (@($untracked).Count -ne 0)
    {
        throw "Untracked files appeared during the merge. Move or remove them before continuing:`n$($untracked -join [Environment]::NewLine)"
    }

    Invoke-GitCommand -Arguments @('-c', 'core.whitespace=cr-at-eol', 'diff', '--check')
    Invoke-GitCommand -Arguments @('-c', 'core.whitespace=cr-at-eol', 'diff', '--cached', '--check')

    if (-not $SkipValidation)
    {
        $state.Phase = 'validating'
        $state.Validation = 'running'
        [void](Write-SyncArtifacts -State $state)
        try
        {
            Invoke-RepositoryScript -Path (Join-Path $script:RepoRoot 'tools\deploy\test-workspace-release.ps1')
            $state.Validation = 'passed'
        }
        catch
        {
            $state.Phase = 'validation-failed'
            $state.Validation = 'failed'
            $report = Write-SyncArtifacts -State $state
            throw "Upstream merge validation failed. Fix the issue, stage the change, and rerun -Mode Continue. Report: $report`n$_"
        }
    }
    else
    {
        $state.Validation = 'skipped'
    }

    if (-not $SkipPortable)
    {
        $state.Phase = 'packaging'
        $state.PortableDeployment = 'running'
        [void](Write-SyncArtifacts -State $state)
        try
        {
            Invoke-RepositoryScript -Path (Join-Path $script:RepoRoot 'tools\deploy\build-portable-release.ps1') -Arguments @('-SkipBuild')
            $state.PortableDeployment = 'passed'
        }
        catch
        {
            $state.Phase = 'packaging-failed'
            $state.PortableDeployment = 'failed'
            $report = Write-SyncArtifacts -State $state
            throw "Portable packaging failed. Fix the issue and rerun -Mode Continue. Report: $report`n$_"
        }
    }
    else
    {
        $state.PortableDeployment = 'skipped'
    }

    $postValidationUnstaged = Invoke-GitCapture -Arguments @('diff', '--quiet', '--ignore-submodules') -AllowFailure
    $postValidationUntrackedResult = Invoke-GitCapture -Arguments @('ls-files', '--others', '--exclude-standard')
    $postValidationUntracked = @($postValidationUntrackedResult.Output)
    if ($postValidationUnstaged.ExitCode -ne 0 -or @($postValidationUntracked).Count -ne 0)
    {
        $state.Phase = 'worktree-drift-after-validation'
        $report = Write-SyncArtifacts -State $state
        throw "Validation or packaging changed tracked/untracked worktree files. Review them before committing. Report: $report"
    }

    Invoke-GitCommand -Arguments @('commit', '--no-edit')
    $state.ResultCommit = Resolve-Commit -Ref 'HEAD'
    $state.Phase = 'complete'
    $report = Write-SyncArtifacts -State $state
    Write-Host "Upstream merge committed on '$($state.SyncBranch)': $($state.ResultCommit)" -ForegroundColor Green
    Write-Host "Review the branch, then fast-forward '$($state.IntegrationBranch)' or open a PR." -ForegroundColor Green
    Write-Host "Report: $report"
}

function Invoke-Abort
{
    $state = if (Test-Path -LiteralPath $script:StatePath -PathType Leaf) { Read-SyncState } else { $null }
    if (-not (Test-GitMarker -Name 'MERGE_HEAD'))
    {
        throw 'No merge is in progress.'
    }

    Invoke-GitCommand -Arguments @('merge', '--abort')
    if ($state)
    {
        $state.Phase = 'aborted'
        $state.Validation = 'not-run'
        $state.PortableDeployment = 'not-run'
        $report = Write-SyncArtifacts -State $state
        Write-Host "Merge aborted. Sync branch '$($state.SyncBranch)' was kept for inspection." -ForegroundColor Yellow
        Write-Host "Report: $report"
    }
}

function Invoke-SelfTest
{
    $git = (Get-Command git.exe -ErrorAction Stop).Source
    $pwsh = (Get-Command pwsh.exe -ErrorAction Stop).Source
    $temporaryBase = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
    $temporaryRoot = Join-Path $temporaryBase "terminal-upstream-sync-tests-$([guid]::NewGuid().ToString('N'))"
    $assertions = 0

    function Invoke-TestGit([string]$Root, [string[]]$Arguments)
    {
        & $git -C $Root @Arguments | Out-Null
        if ($LASTEXITCODE -ne 0)
        {
            throw "Self-test git command failed: git -C $Root $($Arguments -join ' ')"
        }
    }

    function Invoke-TestScript([string[]]$Arguments, [int]$ExpectedExitCode)
    {
        $output = @(& $pwsh -NoLogo -NoProfile -File $PSCommandPath @Arguments 2>&1 | ForEach-Object { $_.ToString() })
        $exitCode = $LASTEXITCODE
        if ($exitCode -ne $ExpectedExitCode)
        {
            throw "Self-test script expected exit $ExpectedExitCode but got ${exitCode}:`n$($output -join [Environment]::NewLine)"
        }
        return $output
    }

    function Assert-Test([bool]$Condition, [string]$Message)
    {
        if (-not $Condition)
        {
            throw "Self-test assertion failed: $Message"
        }
        $script:SelfTestAssertionCount++
    }

    $script:SelfTestAssertionCount = 0
    try
    {
        $seed = Join-Path $temporaryRoot 'seed'
        $remote = Join-Path $temporaryRoot 'upstream.git'
        $product = Join-Path $temporaryRoot 'product'
        New-Item -ItemType Directory -Path $seed -Force | Out-Null

        Invoke-TestGit -Root $seed -Arguments @('init', '-b', 'main')
        Invoke-TestGit -Root $seed -Arguments @('config', 'user.name', 'Upstream Sync Test')
        Invoke-TestGit -Root $seed -Arguments @('config', 'user.email', 'upstream-sync-test@example.invalid')
        Invoke-TestGit -Root $seed -Arguments @('config', 'core.autocrlf', 'false')
        [System.IO.File]::WriteAllText((Join-Path $seed 'shared.txt'), "base`n", [System.Text.UTF8Encoding]::new($false))
        Invoke-TestGit -Root $seed -Arguments @('add', 'shared.txt')
        Invoke-TestGit -Root $seed -Arguments @('commit', '-m', 'base')
        & $git clone --bare $seed $remote | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Self-test bare clone failed.' }
        & $git -c core.autocrlf=false clone $remote $product | Out-Null
        if ($LASTEXITCODE -ne 0) { throw 'Self-test product clone failed.' }
        Invoke-TestGit -Root $product -Arguments @('config', 'user.name', 'Fork Sync Test')
        Invoke-TestGit -Root $product -Arguments @('config', 'user.email', 'fork-sync-test@example.invalid')
        Invoke-TestGit -Root $product -Arguments @('config', 'core.autocrlf', 'false')
        [System.IO.File]::WriteAllText((Join-Path $product 'fork.txt'), "fork`n", [System.Text.UTF8Encoding]::new($false))
        Invoke-TestGit -Root $product -Arguments @('add', 'fork.txt')
        Invoke-TestGit -Root $product -Arguments @('commit', '-m', 'fork change')

        Invoke-TestGit -Root $seed -Arguments @('remote', 'add', 'origin', $remote)
        [System.IO.File]::WriteAllText((Join-Path $seed 'upstream.txt'), "upstream`n", [System.Text.UTF8Encoding]::new($false))
        Invoke-TestGit -Root $seed -Arguments @('add', 'upstream.txt')
        Invoke-TestGit -Root $seed -Arguments @('commit', '-m', 'upstream change')
        Invoke-TestGit -Root $seed -Arguments @('push', 'origin', 'main')

        [void](Invoke-TestScript -ExpectedExitCode 0 -Arguments @(
                '-Mode', 'Prepare',
                '-RepositoryRoot', $product,
                '-IntegrationBranch', 'main',
                '-RemoteName', 'upstream',
                '-UpstreamUrl', $remote,
                '-SyncBranch', 'sync/test-clean'))
        Assert-Test -Condition (Test-Path -LiteralPath (Join-Path $product '.git\MERGE_HEAD')) -Message 'Prepare should leave a clean merge uncommitted.'

        [void](Invoke-TestScript -ExpectedExitCode 0 -Arguments @(
                '-Mode', 'Continue',
                '-RepositoryRoot', $product,
                '-SkipValidation',
                '-SkipPortable',
                '-AllowUnsafeSkip'))
        Assert-Test -Condition (Test-Path -LiteralPath (Join-Path $product 'fork.txt')) -Message 'Fork file should survive clean merge.'
        Assert-Test -Condition (Test-Path -LiteralPath (Join-Path $product 'upstream.txt')) -Message 'Upstream file should be merged.'
        $parents = (Get-GitScalarForTest -Root $product -Arguments @('rev-list', '--parents', '-n', '1', 'HEAD')) -split '\s+'
        Assert-Test -Condition (@($parents).Count -eq 3) -Message 'Continue should create a two-parent merge commit.'

        Invoke-TestGit -Root $product -Arguments @('switch', 'main')
        Invoke-TestGit -Root $product -Arguments @('merge', '--ff-only', 'sync/test-clean')
        [System.IO.File]::WriteAllText((Join-Path $seed 'shared.txt'), "upstream conflict`n", [System.Text.UTF8Encoding]::new($false))
        Invoke-TestGit -Root $seed -Arguments @('add', 'shared.txt')
        Invoke-TestGit -Root $seed -Arguments @('commit', '-m', 'upstream conflict')
        Invoke-TestGit -Root $seed -Arguments @('push', 'origin', 'main')
        [System.IO.File]::WriteAllText((Join-Path $product 'shared.txt'), "fork conflict`n", [System.Text.UTF8Encoding]::new($false))
        Invoke-TestGit -Root $product -Arguments @('add', 'shared.txt')
        Invoke-TestGit -Root $product -Arguments @('commit', '-m', 'fork conflict')

        [void](Invoke-TestScript -ExpectedExitCode 2 -Arguments @(
                '-Mode', 'Prepare',
                '-RepositoryRoot', $product,
                '-IntegrationBranch', 'main',
                '-RemoteName', 'upstream',
                '-UpstreamUrl', $remote,
                '-SyncBranch', 'sync/test-conflict'))
        $reportPath = Join-Path $product '.git\upstream-sync\report.md'
        Assert-Test -Condition (Test-Path -LiteralPath $reportPath) -Message 'Conflict report should exist.'
        Assert-Test -Condition ((Get-Content -LiteralPath $reportPath -Raw) -match 'shared\.txt') -Message 'Conflict report should name the conflicted path.'
        Assert-Test -Condition (Test-Path -LiteralPath (Join-Path $product '.git\MERGE_HEAD')) -Message 'Conflict merge should remain active for AI repair.'

        [void](Invoke-TestScript -ExpectedExitCode 0 -Arguments @(
                '-Mode', 'Abort',
                '-RepositoryRoot', $product))
        Assert-Test -Condition (-not (Test-Path -LiteralPath (Join-Path $product '.git\MERGE_HEAD'))) -Message 'Abort should clear MERGE_HEAD.'

        $dirtyPath = Join-Path $product 'dirty.txt'
        [System.IO.File]::WriteAllText($dirtyPath, "dirty`n", [System.Text.UTF8Encoding]::new($false))
        [void](Invoke-TestScript -ExpectedExitCode 1 -Arguments @(
                '-Mode', 'Prepare',
                '-RepositoryRoot', $product,
                '-IntegrationBranch', 'main',
                '-RemoteName', 'upstream',
                '-UpstreamUrl', $remote,
                '-SyncBranch', 'sync/test-dirty'))
        Assert-Test -Condition (-not (Test-Path -LiteralPath (Join-Path $product '.git\refs\heads\sync\test-dirty'))) -Message 'Dirty worktree must not create a sync branch.'
        [System.IO.File]::Delete($dirtyPath)

        Invoke-TestGit -Root $product -Arguments @('switch', '-c', 'feature/unintegrated')
        [System.IO.File]::WriteAllText((Join-Path $product 'feature.txt'), "feature`n", [System.Text.UTF8Encoding]::new($false))
        Invoke-TestGit -Root $product -Arguments @('add', 'feature.txt')
        Invoke-TestGit -Root $product -Arguments @('commit', '-m', 'unintegrated feature')
        [void](Invoke-TestScript -ExpectedExitCode 1 -Arguments @(
                '-Mode', 'Prepare',
                '-RepositoryRoot', $product,
                '-IntegrationBranch', 'main',
                '-RemoteName', 'upstream',
                '-UpstreamUrl', $remote,
                '-SyncBranch', 'sync/test-stale'))
        $staleBranch = @(& $git -C $product show-ref --verify --quiet refs/heads/sync/test-stale 2>&1)
        Assert-Test -Condition ($LASTEXITCODE -ne 0) -Message 'An unintegrated feature branch must not create an upstream sync branch.'

        $assertions = $script:SelfTestAssertionCount
        Write-Host "PASS: $assertions upstream sync assertions" -ForegroundColor Green
    }
    finally
    {
        $resolvedRoot = [System.IO.Path]::GetFullPath($temporaryRoot)
        if (-not $resolvedRoot.StartsWith($temporaryBase, [System.StringComparison]::OrdinalIgnoreCase) -or
            -not ([System.IO.Path]::GetFileName($resolvedRoot)).StartsWith('terminal-upstream-sync-tests-', [System.StringComparison]::Ordinal))
        {
            throw "Refusing to remove unexpected self-test path: $resolvedRoot"
        }
        if (Test-Path -LiteralPath $resolvedRoot)
        {
            Remove-Item -LiteralPath $resolvedRoot -Recurse -Force
        }
        Remove-Variable -Name SelfTestAssertionCount -Scope Script -ErrorAction SilentlyContinue
    }
}

function Get-GitScalarForTest
{
    param(
        [Parameter(Mandatory)][string]$Root,
        [Parameter(Mandatory)][string[]]$Arguments
    )
    $output = @(& (Get-Command git.exe -ErrorAction Stop).Source -C $Root @Arguments 2>&1 | ForEach-Object { $_.ToString() })
    if ($LASTEXITCODE -ne 0)
    {
        throw "Self-test git query failed: $($Arguments -join ' ')"
    }
    return ($output -join [Environment]::NewLine).Trim()
}

if ($Mode -eq 'SelfTest')
{
    Invoke-SelfTest
    exit 0
}

Initialize-Repository

switch ($Mode)
{
    'Check' { Invoke-Check }
    'Prepare' { Invoke-Prepare }
    'Continue' { Invoke-Continue }
    'Abort' { Invoke-Abort }
}
