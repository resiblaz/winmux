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
$testHostDirectory = Join-Path $repoRoot "bin\$Platform\$configuration\TestHostApp"
$settingsTestDirectory = Join-Path $repoRoot "bin\$Platform\$configuration\UnitTests_SettingsModel"
$wt = Join-Path $repoRoot "src\cascadia\CascadiaPackage\bin\$Platform\$configuration\wtd.exe"
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
        [int]$ExpectedExitCode
    )

    & $Executable @Arguments
    if ($LASTEXITCODE -ne $ExpectedExitCode)
    {
        throw "Expected exit code $ExpectedExitCode but got $($LASTEXITCODE): $Executable $($Arguments -join ' ')"
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

Push-Location $repoRoot
try
{
    Write-Host 'Running static release checks...' -ForegroundColor Cyan
    $git = (Get-Command git.exe -ErrorAction Stop).Source
    Invoke-CheckedCommand -Executable $git -Arguments @('diff', '--check')
    Invoke-CheckedCommand -Executable $git -Arguments @('diff', '--cached', '--check')

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
        Write-Host 'Building the Release/MSIX package...' -ForegroundColor Cyan
        Invoke-CheckedCommand -Executable (Get-Command pwsh.exe -ErrorAction Stop).Source -Arguments @(
            '-NoLogo',
            '-NoProfile',
            '-File',
            $packageBuild,
            '-Platform',
            $Platform,
            '-BuildOnly')
    }

    if (-not $SkipTestBuild -or -not $SkipNativeTests)
    {
        Import-Module $openConsoleModule -Force
        Set-MsbuildDevEnvironment -Prerelease
    }

    if (-not $SkipTestBuild)
    {
        Write-Host 'Building Workspace/History native tests...' -ForegroundColor Cyan
        $env:CL_MPCount = '1'
        $msbuild = (Get-Command msbuild.exe -ErrorAction Stop).Source
        $solutionDirectory = $repoRoot.TrimEnd('\\') + '\\'
        $commonArguments = @(
            "/p:Configuration=$configuration",
            "/p:Platform=$Platform",
            "/p:SolutionDir=$solutionDirectory",
            '/p:WindowsTerminalBranding=Dev',
            '/m:1',
            '/nologo',
            '/verbosity:minimal')
        foreach ($project in @(
                'src\cascadia\LocalTests_TerminalApp\TerminalApp.LocalTests.vcxproj',
                'src\cascadia\LocalTests_TerminalApp\TestHostApp\TestHostApp.vcxproj',
                'src\cascadia\UnitTests_SettingsModel\SettingsModel.UnitTests.vcxproj'))
        {
            Invoke-CheckedCommand -Executable $msbuild -Arguments (@(
                    (Join-Path $repoRoot $project)) + $commonArguments)
        }
    }

    if (-not $SkipNativeTests)
    {
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
            -not (Test-Path -LiteralPath $terminalAppTests -PathType Leaf) -or
            -not (Test-Path -LiteralPath $settingsTests -PathType Leaf))
        {
            throw 'Native test outputs are missing. Run without -SkipTestBuild.'
        }

        Push-Location $testHostDirectory
        try
        {
            foreach ($pattern in @(
                    '*AgentHistoryStoreTests*',
                    '*CodexAppServerTests*',
                    '*WorkspaceGitMetadataTests*',
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
            Invoke-ExpectedExitCode -Executable $wt -Arguments @('workspace', 'list') -ExpectedExitCode 0
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
