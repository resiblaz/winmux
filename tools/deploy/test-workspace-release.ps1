[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string]$Platform = 'x64',

    [switch]$SkipPackageBuild,

    [switch]$SkipTestBuild,

    [switch]$SkipNativeTests,

    [switch]$SkipHookTests,

    [switch]$SkipCliSmoke
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$configuration = 'Release'
$openConsoleModule = Join-Path $repoRoot 'tools\OpenConsole.psm1'
$packageBuild = Join-Path $repoRoot 'tools\Deploy-LocalRelease.ps1'
$hookTests = Join-Path $repoRoot '.codex\hooks\test-workspace-history.ps1'
$taskCoreTestDirectory = Join-Path $repoRoot "bin\$Platform\$configuration"
$taskExecutionTestDirectory = Join-Path $repoRoot "bin\$Platform\$configuration"
$taskGitTestDirectory = Join-Path $repoRoot "bin\$Platform\$configuration"
$taskProtocolTestDirectory = Join-Path $repoRoot "bin\$Platform\$configuration"
$taskActivationTestDirectory = Join-Path $repoRoot "bin\$Platform\$configuration"
$taskClientTestDirectory = Join-Path $repoRoot "bin\$Platform\$configuration"
$taskRuntimeTestDirectory = Join-Path $repoRoot "bin\$Platform\$configuration"
$testHostDirectory = Join-Path $repoRoot "bin\$Platform\$configuration\TestHostApp"
$settingsTestDirectory = Join-Path $repoRoot "bin\$Platform\$configuration\UnitTests_SettingsModel"
$wt = Join-Path $repoRoot "src\cascadia\CascadiaPackage\bin\$Platform\$configuration\wtd.exe"
$workspaceTestTargets = @(
    'Terminal\Tests\TaskCore_LocalTests',
    'Terminal\Tests\TaskExecution_LocalTests',
    'Terminal\Tests\TaskExecutionLockChild',
    'Terminal\Tests\GitTestChild',
    'Terminal\Tests\TaskGit_LocalTests',
    'Terminal\Tests\TaskRuntimeProtocol_LocalTests',
    'Terminal\Tests\TaskRuntimeActivation_LocalTests',
    'Terminal\Tests\TaskRuntimeClient_LocalTests',
    'Terminal\Tests\TaskRuntimeActivationParent',
    'Terminal\Tests\TaskRuntime_LocalTests',
    'Terminal\Task\TaskRuntime',
    'Terminal\Task\TaskExecution',
    'Terminal\Task\TaskGit',
    'Terminal\Task\Rtask',
    'Terminal\Tests\TerminalApp_LocalTests',
    'Terminal\Tests\TestHostApp',
    'Terminal\Tests\SettingsModel_UnitTests')
$oldClMpCount = $env:CL_MPCount

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
        [string]$Pattern
    )

    Write-Host "TAEF: $Pattern" -ForegroundColor Cyan
    Invoke-CheckedCommand -Executable $Taef -Arguments @($TestDll, "/name:$Pattern")
}

function Invoke-RtaskExpectedExitCode
{
    param(
        [Parameter(Mandatory)]
        [string]$Executable,

        [Parameter(Mandatory)]
        [string[]]$Arguments,

        [Parameter(Mandatory)]
        [int]$ExpectedExitCode,

        [hashtable]$Environment = @{},

        [byte[]]$StandardInput = @()
    )

    $startInfo = [System.Diagnostics.ProcessStartInfo]::new()
    $startInfo.FileName = $Executable
    $startInfo.UseShellExecute = $false
    $startInfo.CreateNoWindow = $true
    $startInfo.RedirectStandardInput = $true
    $startInfo.RedirectStandardOutput = $true
    $startInfo.RedirectStandardError = $true
    foreach ($argument in $Arguments)
    {
        $null = $startInfo.ArgumentList.Add($argument)
    }
    foreach ($name in @('RICHER_TASK_ENDPOINT', 'RICHER_TASK_RUN_ID', 'RICHER_TASK_CAPABILITY'))
    {
        $null = $startInfo.Environment.Remove($name)
    }
    foreach ($entry in $Environment.GetEnumerator())
    {
        $startInfo.Environment[$entry.Key] = [string]$entry.Value
    }

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $startInfo
    try
    {
        if (-not $process.Start())
        {
            throw 'rtask process did not start'
        }
        if ($StandardInput.Length -ne 0)
        {
            $process.StandardInput.BaseStream.Write($StandardInput, 0, $StandardInput.Length)
            $process.StandardInput.BaseStream.Flush()
        }
        $process.StandardInput.Close()
        if (-not $process.WaitForExit(10000))
        {
            $process.Kill($true)
            throw 'rtask process did not exit before the smoke timeout'
        }
        $standardOutput = $process.StandardOutput.ReadToEnd()
        $standardError = $process.StandardError.ReadToEnd()
        if ($process.ExitCode -ne $ExpectedExitCode)
        {
            throw "Expected rtask exit code $ExpectedExitCode but got $($process.ExitCode); stdout=$standardOutput; stderr=$standardError"
        }
    }
    finally
    {
        $process.Dispose()
    }
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
            'src\cascadia\TaskCore\TaskCore.vcxproj',
            'src\cascadia\LocalTests_TaskCore\TaskCore.LocalTests.vcxproj',
            'src\cascadia\TaskExecution\TaskExecution.vcxproj',
            'src\cascadia\LocalTests_TaskExecution\TaskExecution.LocalTests.vcxproj',
            'src\cascadia\LocalTests_TaskExecution\TaskExecutionLockChild\TaskExecutionLockChild.vcxproj',
            'src\cascadia\LocalTests_TaskExecution\GitTestChild\GitTestChild.vcxproj',
            'src\cascadia\TaskGit\TaskGit.vcxproj',
            'src\cascadia\LocalTests_TaskGit\TaskGit.LocalTests.vcxproj',
            'src\cascadia\TaskRuntimeProtocol\TaskRuntimeProtocol.vcxproj',
            'src\cascadia\LocalTests_TaskRuntimeProtocol\TaskRuntimeProtocol.LocalTests.vcxproj',
            'src\cascadia\TaskRuntimeActivation\TaskRuntimeActivation.vcxproj',
            'src\cascadia\LocalTests_TaskRuntimeActivation\TaskRuntimeActivation.LocalTests.vcxproj',
            'src\cascadia\TaskRuntimeClient\TaskRuntimeClient.vcxproj',
            'src\cascadia\LocalTests_TaskRuntimeClient\TaskRuntimeClient.LocalTests.vcxproj',
            'src\cascadia\LocalTests_TaskRuntime\TaskRuntimeActivationParent\TaskRuntimeActivationParent.vcxproj',
            'src\cascadia\TaskRuntimeTransport\TaskRuntimeTransport.vcxproj',
            'src\cascadia\TaskRuntime\TaskRuntimeHost.vcxproj',
            'src\cascadia\TaskRuntime\TaskRuntime.vcxproj',
            'src\cascadia\Rtask\RtaskLib.vcxproj',
            'src\cascadia\Rtask\Rtask.vcxproj',
            'src\cascadia\LocalTests_TaskRuntime\TaskRuntime.LocalTests.vcxproj',
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
        $PSCommandPath)

    if (-not $SkipHookTests)
    {
        Write-Host 'Running durable Hook tests...' -ForegroundColor Cyan
        Invoke-CheckedCommand -Executable (Get-Command pwsh.exe -ErrorAction Stop).Source -Arguments @(
            '-NoLogo',
            '-NoProfile',
            '-File',
            $hookTests)
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
        if (-not $SkipTestBuild)
        {
            Write-Host 'Building the unified Release/MSIX and native-test graph...' -ForegroundColor Cyan
            $buildArguments.Add('-IncludeWorkspaceTests')
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
        Write-Host 'Building Workspace/History native tests...' -ForegroundColor Cyan
        $env:CL_MPCount = '1'
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
            '/m:1',
            '/nologo',
            '/verbosity:minimal')
        Invoke-CheckedCommand -Executable $msbuild -Arguments $buildArguments
    }

    if (-not $SkipNativeTests)
    {
        $taskCoreTests = Join-Path $taskCoreTestDirectory 'TaskCore.LocalTests.dll'
        $taskExecutionTests = Join-Path $taskExecutionTestDirectory 'TaskExecution.LocalTests.dll'
        $taskExecutionLockChild = Join-Path $taskExecutionTestDirectory 'TaskExecutionLockChild.exe'
        $taskExecutionGitTestChild = Join-Path $taskExecutionTestDirectory 'TaskExecutionGitTestChild.exe'
        $taskGitTests = Join-Path $taskGitTestDirectory 'TaskGit.LocalTests.dll'
        $taskProtocolTests = Join-Path $taskProtocolTestDirectory 'TaskRuntimeProtocol.LocalTests.dll'
        $taskActivationTests = Join-Path $taskActivationTestDirectory 'TaskRuntimeActivation.LocalTests.dll'
        $taskClientTests = Join-Path $taskClientTestDirectory 'TaskRuntimeClient.LocalTests.dll'
        $taskRuntimeTests = Join-Path $taskRuntimeTestDirectory 'TaskRuntime.LocalTests.dll'
        $taskRuntimeExecutable = Join-Path $taskRuntimeTestDirectory 'TaskRuntime.exe'
        $rtaskExecutable = Join-Path $taskRuntimeTestDirectory 'rtask.exe'
        $terminalAppTests = Join-Path $testHostDirectory 'TerminalApp.LocalTests.dll'
        $settingsTests = Join-Path $settingsTestDirectory 'SettingsModel.Unit.Tests.dll'
        $taef = @(
            (Join-Path $testHostDirectory 'TE.exe'),
            (Join-Path $settingsTestDirectory 'TE.exe'),
            (Get-ChildItem -LiteralPath (Join-Path $repoRoot 'packages') -Recurse -Filter 'TE.exe' -File |
                Where-Object { $_.FullName -match '\\Binaries\\x64\\TE\.exe$' } |
                Select-Object -First 1 -ExpandProperty FullName)
        ) | Where-Object { $_ -and (Test-Path -LiteralPath $_ -PathType Leaf) } | Select-Object -First 1

        if (-not $taef -or
            -not (Test-Path -LiteralPath $taskCoreTests -PathType Leaf) -or
            -not (Test-Path -LiteralPath $taskExecutionTests -PathType Leaf) -or
            -not (Test-Path -LiteralPath $taskExecutionLockChild -PathType Leaf) -or
            -not (Test-Path -LiteralPath $taskExecutionGitTestChild -PathType Leaf) -or
            -not (Test-Path -LiteralPath $taskGitTests -PathType Leaf) -or
            -not (Test-Path -LiteralPath $taskProtocolTests -PathType Leaf) -or
            -not (Test-Path -LiteralPath $taskActivationTests -PathType Leaf) -or
            -not (Test-Path -LiteralPath $taskClientTests -PathType Leaf) -or
            -not (Test-Path -LiteralPath $taskRuntimeTests -PathType Leaf) -or
            -not (Test-Path -LiteralPath $taskRuntimeExecutable -PathType Leaf) -or
            -not (Test-Path -LiteralPath $rtaskExecutable -PathType Leaf) -or
            -not (Test-Path -LiteralPath $terminalAppTests -PathType Leaf) -or
            -not (Test-Path -LiteralPath $settingsTests -PathType Leaf))
        {
            throw 'Native test outputs are missing. Run without -SkipTestBuild.'
        }

        Push-Location $taskCoreTestDirectory
        try
        {
            Invoke-TaefSelection -Taef $taef -TestDll $taskCoreTests -Pattern '*TaskCore*'
        }
        finally
        {
            Pop-Location
        }

        Push-Location $taskExecutionTestDirectory
        try
        {
            Invoke-TaefSelection -Taef $taef -TestDll $taskExecutionTests -Pattern '*TaskExecution*'
        }
        finally
        {
            Pop-Location
        }

        Push-Location $taskGitTestDirectory
        try
        {
            Invoke-TaefSelection -Taef $taef -TestDll $taskGitTests -Pattern '*TaskGit*'
        }
        finally
        {
            Pop-Location
        }

        Push-Location $taskProtocolTestDirectory
        try
        {
            Invoke-TaefSelection -Taef $taef -TestDll $taskProtocolTests -Pattern '*TaskRuntimeProtocol*'
        }
        finally
        {
            Pop-Location
        }

        Push-Location $taskActivationTestDirectory
        try
        {
            Invoke-TaefSelection -Taef $taef -TestDll $taskActivationTests -Pattern '*TaskRuntimeActivationLocalTests*'
        }
        finally
        {
            Pop-Location
        }

        Push-Location $taskClientTestDirectory
        try
        {
            Invoke-TaefSelection -Taef $taef -TestDll $taskClientTests -Pattern '*TaskRuntimeClientLocalTests*'
        }
        finally
        {
            Pop-Location
        }

        Push-Location $taskRuntimeTestDirectory
        try
        {
            Invoke-TaefSelection -Taef $taef -TestDll $taskRuntimeTests -Pattern '*TaskRuntimeLocalTests*'
            Invoke-ExpectedExitCode -Executable $taskRuntimeExecutable -Arguments @(
                '--bootstrap-check',
                '--channel',
                "gate-$PID",
                '--settings-root',
                $taskRuntimeTestDirectory) -ExpectedExitCode 0
            Invoke-ExpectedExitCode -Executable $taskRuntimeExecutable -Arguments @('--bad') -ExpectedExitCode 2
            Invoke-ExpectedExitCode -Executable $rtaskExecutable -Arguments @('--help') -ExpectedExitCode 0
            Invoke-ExpectedExitCode -Executable $rtaskExecutable -Arguments @('--bad') -ExpectedExitCode 2
            Invoke-RtaskExpectedExitCode -Executable $rtaskExecutable -Arguments @('status', 'in-progress') -ExpectedExitCode 3
            $rtaskEnvironment = @{
                RICHER_TASK_ENDPOINT = '\\.\pipe\WT.TaskRuntime.dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd.control'
                RICHER_TASK_RUN_ID = 'dd000000-0000-4000-8000-000000000001'
                RICHER_TASK_CAPABILITY = 'eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee'
            }
            Invoke-RtaskExpectedExitCode -Executable $rtaskExecutable -Arguments @('result', '--stdin') -ExpectedExitCode 4 -Environment $rtaskEnvironment
            Invoke-RtaskExpectedExitCode -Executable $rtaskExecutable -Arguments @('result', '--stdin') -ExpectedExitCode 4 -Environment $rtaskEnvironment -StandardInput ([byte[]](0xc0, 0x80))
            Invoke-RtaskExpectedExitCode -Executable $rtaskExecutable -Arguments @('result', '--stdin') -ExpectedExitCode 5 -Environment $rtaskEnvironment -StandardInput ([byte[]](0x78))
        }
        finally
        {
            Pop-Location
        }

        Push-Location $testHostDirectory
        try
        {
            foreach ($pattern in @(
                    '*AgentHistoryStoreTests*',
                    '*CodexAppServerTests*',
                    '*WorkspaceGitMetadataTests*',
                    '*TaskPageViewModelTests*',
                    '*WorkspaceSidebarAdaptsToWindowModes*',
                    '*WorkspaceSurfaceShortcutActions*',
                    '*TabTests*WorkspaceSnapshot*',
                    '*WorkspaceLayoutPersistsMetadata*'))
            {
                Invoke-TaefSelection -Taef $taef -TestDll $terminalAppTests -Pattern $pattern
            }
        }
        finally
        {
            Pop-Location
        }

        Push-Location $settingsTestDirectory
        try
        {
            foreach ($pattern in @(
                    '*ApplicationStateTests*',
                    '*WorkspaceSnapshotStoreTests*',
                    '*WorkspaceSnapshotExchangeTests*',
                    '*WorkspaceCommandlineTests*',
                    '*WorkspaceActionsCanBeCustomized*',
                    '*DefaultWorkspaceSurfaceBindings*',
                    '*AgentHistoryRetentionSettings*'))
            {
                Invoke-TaefSelection -Taef $taef -TestDll $settingsTests -Pattern $pattern
            }
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

        Write-Host 'Running wt workspace CLI smoke tests...' -ForegroundColor Cyan
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

    Write-Host 'Workspace release gate passed. Manual Narrator/DPI acceptance remains separate.' -ForegroundColor Green
}
finally
{
    $env:CL_MPCount = $oldClMpCount
    Pop-Location
}
