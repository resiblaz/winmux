[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string]$Platform = 'x64',

    [switch]$SkipPackageBuild,

    [switch]$SkipTestBuild,

    [switch]$SkipNativeTests,

    [switch]$SkipHookTests,

    [switch]$IncludeHookTests,

    [switch]$SkipCliSmoke,

    [switch]$IncludeUiTests,

    [ValidateRange(0, 64)]
    [int]$MaxCpuCount = 0,

    [switch]$NormalPriority,

    [switch]$ForceRestore
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$configuration = 'Release'
$openConsoleModule = Join-Path $repoRoot 'tools\OpenConsole.psm1'
$packageBuild = Join-Path $repoRoot 'tools\Deploy-LocalRelease.ps1'
$hookTests = Join-Path $repoRoot '.codex\hooks\test-workspace-history.ps1'
$localTestsDirectory = Join-Path $repoRoot "bin\$Platform\$configuration\LocalTests_TerminalApp"
$testHostDirectory = Join-Path $repoRoot "bin\$Platform\$configuration\TestHostApp"
$settingsTestDirectory = Join-Path $repoRoot "bin\$Platform\$configuration\UnitTests_SettingsModel"
$controlTestDirectory = Join-Path $repoRoot "bin\$Platform\$configuration\UnitTests_Control"
$coreTestDirectory = Join-Path $repoRoot "bin\$Platform\$configuration\UnitTests_TerminalCore"
$appUnitTestDirectory = Join-Path $repoRoot "bin\$Platform\$configuration\UnitTests_TerminalApp"
$wt = Join-Path $repoRoot "src\cascadia\CascadiaPackage\bin\$Platform\$configuration\wmx.exe"
$workspaceTestTargets = [System.Collections.Generic.List[string]]@(
    'Terminal\Tests\TerminalApp_LocalTests',
    'Terminal\Tests\SettingsModel_UnitTests',
    'Terminal\Tests\Control_UnitTests',
    'Terminal\Tests\UnitTests',
    'Terminal\Tests\TerminalApp_UnitTests')
if ($IncludeUiTests)
{
    $workspaceTestTargets.Insert(1, 'Terminal\Tests\TestHostApp')
}
$oldClMpCount = $env:CL_MPCount

function Resolve-BuildCpuCount
{
    param([int]$Requested)

    if ($Requested -gt 0)
    {
        return $Requested
    }

    $defaultCount = [int][Math]::Floor([Environment]::ProcessorCount / 4)
    return [Math]::Clamp($defaultCount, 2, 6)
}

function Invoke-CheckedCommand
{
    param(
        [Parameter(Mandatory)]
        [string]$Executable,

        [Parameter(Mandatory)]
        [string[]]$Arguments
    )

    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0)
    {
        throw "Command failed with exit code $($LASTEXITCODE): $Executable $($Arguments -join ' ')"
    }
}

function Invoke-BuildCommand
{
    param(
        [Parameter(Mandatory)]
        [string]$Executable,

        [Parameter(Mandatory)]
        [string[]]$Arguments,

        [switch]$UseBelowNormalPriority
    )

    if (-not $UseBelowNormalPriority)
    {
        Invoke-CheckedCommand -Executable $Executable -Arguments $Arguments
        return
    }

    $currentProcess = [System.Diagnostics.Process]::GetCurrentProcess()
    $previousPriority = $currentProcess.PriorityClass
    $priorityChanged = $false
    try
    {
        if ($previousPriority -ne [System.Diagnostics.ProcessPriorityClass]::BelowNormal)
        {
            $currentProcess.PriorityClass = [System.Diagnostics.ProcessPriorityClass]::BelowNormal
            $priorityChanged = $true
        }
        Invoke-CheckedCommand -Executable $Executable -Arguments $Arguments
    }
    finally
    {
        if ($priorityChanged)
        {
            $currentProcess.PriorityClass = $previousPriority
        }
    }
}

function Invoke-ExpectedExitCode
{
    param(
        [Parameter(Mandatory)]
        [string]$Executable,

        [Parameter(Mandatory)]
        [string[]]$Arguments,

        [Parameter(Mandatory)]
        [int]$ExpectedExitCode,

        [int]$Attempts = 1,

        [int[]]$RetryExitCodes = @()
    )

    for ($attempt = 1; $attempt -le $Attempts; $attempt++)
    {
        & $Executable @Arguments
        $actualExitCode = $LASTEXITCODE
        if ($actualExitCode -eq $ExpectedExitCode)
        {
            return
        }
        if ($attempt -lt $Attempts -and $RetryExitCodes -contains $actualExitCode)
        {
            Write-Host "Retrying transient exit code $actualExitCode ($attempt/$Attempts): $Executable $($Arguments -join ' ')" -ForegroundColor Yellow
            Start-Sleep -Milliseconds 250
            continue
        }
        throw "Expected exit code $ExpectedExitCode but got ${actualExitCode}: $Executable $($Arguments -join ' ')"
    }
}

function Assert-PowerShellSyntax
{
    param([Parameter(Mandatory)][string[]]$Paths)

    foreach ($path in $Paths)
    {
        $tokens = $null
        $errors = $null
        [void][System.Management.Automation.Language.Parser]::ParseFile(
            $path,
            [ref]$tokens,
            [ref]$errors)
        if ($errors.Count -ne 0)
        {
            $messages = $errors | ForEach-Object { $_.Message }
            throw "PowerShell syntax validation failed for $($path): $($messages -join '; ')"
        }
    }
}

function Invoke-TaefSelection
{
    param(
        [Parameter(Mandatory)]
        [string]$Taef,

        [Parameter(Mandatory)]
        [string]$TestDll,

        [Parameter(Mandatory)]
        [string[]]$Patterns,

        [string]$Label
    )

    if ($Patterns.Count -eq 1 -and $Patterns[0] -eq '*')
    {
        $display = if ($Label) { $Label } else { '*' }
        Write-Host "TAEF: $display" -ForegroundColor Cyan
        Invoke-CheckedCommand -Executable $Taef -Arguments @($TestDll, '/name:*')
        return
    }

    foreach ($pattern in $Patterns)
    {
        if ($pattern -match "[\s']")
        {
            throw "TAEF pattern contains unsupported whitespace or quotes: $pattern"
        }
    }

    # Combine patterns into a single whitespace-free /select query so PowerShell passes it
    # verbatim to TE.exe without outer quotes and each test host only cold-starts once.
    $query = ($Patterns | ForEach-Object { "@Name='$_'" }) -join 'or'
    $display = if ($Label) { "$Label ($($Patterns.Count) patterns)" } else { $Patterns -join ', ' }
    Write-Host "TAEF: $display" -ForegroundColor Cyan
    Invoke-CheckedCommand -Executable $Taef -Arguments @($TestDll, "/select:$query")
}

Push-Location $repoRoot
try
{
    Write-Host 'Running static release checks...' -ForegroundColor Cyan
    $git = (Get-Command git.exe -ErrorAction Stop).Source
    Invoke-CheckedCommand -Executable $git -Arguments @('-c', 'core.whitespace=cr-at-eol', 'diff', '--check')
    Invoke-CheckedCommand -Executable $git -Arguments @('-c', 'core.whitespace=cr-at-eol', 'diff', '--cached', '--check')

    $normalNumstat = (& $git diff --numstat -- .) -join [Environment]::NewLine
    $eolIgnoredNumstat = (& $git diff --ignore-space-at-eol --numstat -- .) -join [Environment]::NewLine
    if ($LASTEXITCODE -ne 0 -or $normalNumstat -ne $eolIgnoredNumstat)
    {
        throw 'The worktree contains EOL-only changes.'
    }

    Get-Content -LiteralPath (Join-Path $repoRoot '.codex\hooks.json') -Raw |
        ConvertFrom-Json -AsHashtable |
        Out-Null
    Get-Content -LiteralPath (Join-Path $repoRoot 'doc\cascadia\profiles.schema.json') -Raw |
        ConvertFrom-Json -AsHashtable |
        Out-Null

    foreach ($project in @(
            'src\cascadia\TerminalApp\TerminalAppLib.vcxproj',
            'src\cascadia\LocalTests_TerminalApp\TerminalApp.LocalTests.vcxproj',
            'src\cascadia\LocalTests_TerminalApp\TestHostApp\TestHostApp.vcxproj',
            'src\cascadia\UnitTests_SettingsModel\SettingsModel.UnitTests.vcxproj'))
    {
        [xml](Get-Content -LiteralPath (Join-Path $repoRoot $project) -Raw) | Out-Null
    }

    Assert-PowerShellSyntax -Paths @(
        (Join-Path $repoRoot '.codex\hooks\workspace-history.ps1'),
        (Join-Path $repoRoot '.codex\hooks\install-workspace-history.ps1'),
        (Join-Path $repoRoot '.codex\hooks\uninstall-workspace-history.ps1'),
        $hookTests,
        $packageBuild,
        (Join-Path $repoRoot 'tools\deploy\build-portable-release.ps1'),
        (Join-Path $repoRoot 'tools\deploy\PortableSettings.ps1'),
        (Join-Path $repoRoot 'tools\deploy\PortableDeployment.ps1'),
        (Join-Path $repoRoot 'tools\deploy\test-portable-deployment.ps1'),
        (Join-Path $repoRoot 'tools\deploy\test-portable-settings.ps1'),
        (Join-Path $repoRoot 'tools\deploy\GitHubRelease.ps1'),
        (Join-Path $repoRoot 'tools\deploy\prepare-github-release.ps1'),
        (Join-Path $repoRoot 'tools\deploy\publish-github-release.ps1'),
        (Join-Path $repoRoot 'tools\deploy\test-github-release.ps1'),
        $PSCommandPath)

    Invoke-CheckedCommand -Executable (Get-Command pwsh.exe -ErrorAction Stop).Source -Arguments @(
        '-NoLogo', '-NoProfile', '-File', (Join-Path $repoRoot 'tools\deploy\test-portable-settings.ps1'))
    Invoke-CheckedCommand -Executable (Get-Command pwsh.exe -ErrorAction Stop).Source -Arguments @(
        '-NoLogo', '-NoProfile', '-File', (Join-Path $repoRoot 'tools\deploy\test-portable-deployment.ps1'))
    Invoke-CheckedCommand -Executable (Get-Command pwsh.exe -ErrorAction Stop).Source -Arguments @(
        '-NoLogo', '-NoProfile', '-File', (Join-Path $repoRoot 'tools\deploy\test-github-release.ps1'))

    if (-not $SkipHookTests)
    {
        $hookChanges = @(& $git status --porcelain -- '.codex/hooks' '.codex/hooks.json')
        if ($LASTEXITCODE -ne 0)
        {
            throw 'Failed to inspect .codex/hooks git status.'
        }

        if ($IncludeHookTests -or $hookChanges.Count -gt 0)
        {
            Write-Host 'Running durable Hook tests...' -ForegroundColor Cyan
            Invoke-CheckedCommand -Executable (Get-Command pwsh.exe -ErrorAction Stop).Source -Arguments @(
                '-NoLogo',
                '-NoProfile',
                '-File',
                $hookTests)
        }
        else
        {
            Write-Host 'Skipping durable Hook tests (no .codex/hooks changes; pass -IncludeHookTests to run).' -ForegroundColor DarkCyan
        }
    }

    if (-not $SkipPackageBuild)
    {
        $buildArguments = [System.Collections.Generic.List[string]]@(
            '-NoLogo',
            '-NoProfile',
            '-File',
            $packageBuild,
            '-Platform',
            $Platform,
            '-BuildOnly')
        if ($MaxCpuCount -gt 0)
        {
            $buildArguments.Add('-MaxCpuCount')
            $buildArguments.Add("$MaxCpuCount")
        }
        if ($NormalPriority)
        {
            $buildArguments.Add('-NormalPriority')
        }
        if ($ForceRestore)
        {
            $buildArguments.Add('-ForceRestore')
        }
        if (-not $SkipTestBuild)
        {
            Write-Host 'Building the unified Release/MSIX and native-test graph...' -ForegroundColor Cyan
            $buildArguments.Add('-IncludeWorkspaceTests')
            if ($IncludeUiTests)
            {
                $buildArguments.Add('-IncludeUiTests')
            }
        }
        else
        {
            Write-Host 'Building the Release/MSIX package...' -ForegroundColor Cyan
        }

        Invoke-CheckedCommand -Executable (Get-Command pwsh.exe -ErrorAction Stop).Source -Arguments $buildArguments
    }

    if (-not $SkipTestBuild -or -not $SkipNativeTests)
    {
        Import-Module $openConsoleModule -Force
        Set-MsbuildDevEnvironment -Prerelease
    }

    if ($SkipPackageBuild -and -not $SkipTestBuild)
    {
        $resolvedMaxCpuCount = Resolve-BuildCpuCount -Requested $MaxCpuCount
        $priorityLabel = if ($NormalPriority) { 'Normal' } else { 'BelowNormal' }
        Write-Host "Building Workspace/History native tests (jobs=$resolvedMaxCpuCount, priority=$priorityLabel)..." -ForegroundColor Cyan
        $env:CL_MPCount = "$resolvedMaxCpuCount"
        $msbuild = (Get-Command msbuild.exe -ErrorAction Stop).Source
        $buildArguments = @(
            (Join-Path $repoRoot 'OpenConsole.slnx'),
            "/t:$($workspaceTestTargets -join ';')",
            "/p:Configuration=$configuration",
            "/p:Platform=$Platform",
            '/p:WindowsTerminalBranding=Dev',
            '/p:AppxPackageSigningEnabled=false',
            '/p:AppxBundle=Never',
            '/p:AppxSymbolPackageEnabled=false',
            '/p:UseMultiToolTask=true',
            '/p:EnforceProcessCountAcrossBuilds=true',
            "/m:$resolvedMaxCpuCount",
            '/nologo',
            '/verbosity:minimal')
        Invoke-BuildCommand -Executable $msbuild -Arguments $buildArguments -UseBelowNormalPriority:(-not $NormalPriority)
    }

    if (-not $SkipNativeTests)
    {
        $headlessLocalTests = Join-Path $localTestsDirectory 'TerminalApp.LocalTests.dll'
        $terminalAppTests = Join-Path $testHostDirectory 'TerminalApp.LocalTests.dll'
        $settingsTests = Join-Path $settingsTestDirectory 'SettingsModel.Unit.Tests.dll'
        $controlTests = Join-Path $controlTestDirectory 'Control.Unit.Tests.dll'
        $coreTests = Join-Path $coreTestDirectory 'Terminal.Core.Unit.Tests.dll'
        $appUnitTests = Join-Path $appUnitTestDirectory 'Terminal.App.Unit.Tests.dll'
        $taef = @(
            (Join-Path $settingsTestDirectory 'TE.exe'),
            (Join-Path $testHostDirectory 'TE.exe'),
            (Get-ChildItem -LiteralPath (Join-Path $repoRoot 'packages') -Recurse -Filter 'TE.exe' -File |
                Where-Object { $_.FullName -match '\\Binaries\\x64\\TE\.exe$' } |
                Select-Object -First 1 -ExpandProperty FullName)
        ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1

        if (-not $taef -or
            -not (Test-Path -LiteralPath $headlessLocalTests -PathType Leaf) -or
            ($IncludeUiTests -and -not (Test-Path -LiteralPath $terminalAppTests -PathType Leaf)) -or
            -not (Test-Path -LiteralPath $settingsTests -PathType Leaf) -or
            -not (Test-Path -LiteralPath $controlTests -PathType Leaf) -or
            -not (Test-Path -LiteralPath $coreTests -PathType Leaf) -or
            -not (Test-Path -LiteralPath $appUnitTests -PathType Leaf))
        {
            throw 'Native test outputs are missing. Run without -SkipTestBuild.'
        }

        Push-Location $localTestsDirectory
        try
        {
            # WorkspaceGitMetadataTests runs in TE.ProcessHost.exe (no RunAs=UAP window or TestHostApp needed).
            Invoke-TaefSelection -Taef $taef -TestDll $headlessLocalTests -Label 'TerminalApp.LocalTests (headless)' -Patterns @(
                '*WorkspaceGitMetadataTests*')
        }
        finally
        {
            Pop-Location
        }

        if ($IncludeUiTests)
        {
            Push-Location $testHostDirectory
            try
            {
                Invoke-TaefSelection -Taef $taef -TestDll $terminalAppTests -Label 'TerminalApp.LocalTests (UWP TestHostApp)' -Patterns @(
                    '*WorkspaceSidebar*',
                    '*WorkspaceSurfaceShortcutActions*',
                    '*WorkspaceSwitchShortcutActions*',
                    '*WorkspaceSwitchStartupActions*',
                    '*WorkspaceNumber*',
                    '*TabTests*WorkspaceAutomation*',
                    '*TabTests*WorkspaceVimMode*',
                    '*TabTests*WorkspaceNames*',
                    '*WorkspaceWorkingDirectory*',
                    '*WorkspaceClosingTerminals*',
                    '*TabTests*WorkspaceSnapshot*',
                    '*WorkspaceNotification*',
                    '*PaneAttentionState*',
                    '*WorkspaceLayoutPersistsMetadata*')
            }
            finally
            {
                Pop-Location
            }
        }
        else
        {
            Write-Host 'Skipping UWP TestHostApp UI tests in default headless mode (pass -IncludeUiTests to run them).' -ForegroundColor Yellow
        }

        Push-Location $settingsTestDirectory
        try
        {
            Invoke-TaefSelection -Taef $taef -TestDll $settingsTests -Label 'SettingsModel.Unit.Tests' -Patterns @(
                '*ApplicationStateTests*',
                '*WorkspaceSnapshotStoreTests*',
                '*WorkspaceSnapshotExchangeTests*',
                '*WorkspaceCommandlineTests*',
                '*WorkspaceActionsCanBeCustomized*',
                '*DefaultWorkspaceSurfaceBindings*',
                '*PortableInitialSettings*',
                '*VimModeSettings*',
                '*VimModeEntryBindings*',
                '*AgentHistoryRetentionSettings*')
        }
        finally
        {
            Pop-Location
        }
    }

    if (-not $SkipNativeTests)
    {
        Push-Location $controlTestDirectory
        try
        {
            Invoke-TaefSelection -Taef $taef -TestDll $controlTests -Label 'Control.Unit.Tests (Winmux Vim)' -Patterns @(
                '*VimModeTests*')
        }
        finally
        {
            Pop-Location
        }
        Push-Location $coreTestDirectory
        try
        {
            Invoke-TaefSelection -Taef $taef -TestDll $coreTests -Label 'Terminal.Core.Unit.Tests' -Patterns @(
                '*SelectionTest*',
                '*TerminalApiTest*')
        }
        finally
        {
            Pop-Location
        }
        Push-Location $appUnitTestDirectory
        try
        {
            # Run Winmux-owned unit suites (skip upstream FzfTests/JsonUtilsTests).
            Invoke-TaefSelection -Taef $taef -TestDll $appUnitTests -Label 'Terminal.App.Unit.Tests (Winmux)' -Patterns @(
                '*DesktopNotificationTests*',
                '*Workspace*')
        }
        finally
        {
            Pop-Location
        }
    }

    if (-not $SkipCliSmoke)
    {
        if (-not (Test-Path -LiteralPath $wt -PathType Leaf))
        {
            throw "Release workspace CLI is missing: $wt. Run without -SkipPackageBuild."
        }

        Write-Host 'Running wmx workspace CLI smoke tests...' -ForegroundColor Cyan
        $missingSnapshot = '11111111-1111-1111-1111-111111111111'
        $exportPath = Join-Path ([System.IO.Path]::GetTempPath()) "wt-workspace-release-gate-$PID.wtworkspace.json"
        if (Test-Path -LiteralPath $exportPath)
        {
            Remove-Item -LiteralPath $exportPath -Force
        }
        try
        {
            Invoke-ExpectedExitCode -Executable $wt -Arguments @('workspace', 'invalid') -ExpectedExitCode 2
            Invoke-ExpectedExitCode -Executable $wt -Arguments @('workspace', 'list') -ExpectedExitCode 0 -Attempts 20 -RetryExitCodes @(8)
            Invoke-ExpectedExitCode -Executable $wt -Arguments @('workspace', 'open', $missingSnapshot) -ExpectedExitCode 3
            Invoke-ExpectedExitCode -Executable $wt -Arguments @('workspace', 'export', $missingSnapshot, $exportPath) -ExpectedExitCode 3
            if (Test-Path -LiteralPath $exportPath)
            {
                throw 'A failed export unexpectedly created an output file.'
            }
        }
        finally
        {
            if (Test-Path -LiteralPath $exportPath)
            {
                Remove-Item -LiteralPath $exportPath -Force
            }
        }
    }

    if ($IncludeUiTests)
    {
        Write-Host 'Workspace release gate passed. Manual Narrator/DPI acceptance remains separate.' -ForegroundColor Green
    }
    else
    {
        Write-Host 'Workspace headless gate passed. Pass -IncludeUiTests to also run UWP TestHostApp UI tests.' -ForegroundColor Green
    }
}
finally
{
    $env:CL_MPCount = $oldClMpCount
    Pop-Location
}
