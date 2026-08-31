[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string]$Platform = 'x64',

    [switch]$SkipBuild,

    [switch]$NoLaunch,

    [switch]$BuildOnly,

    [switch]$IncludeWorkspaceTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$configuration = 'Release'
$packageBinRoot = Join-Path $repoRoot 'src\cascadia\CascadiaPackage\bin'
$packageLayout = Join-Path $packageBinRoot "$Platform\$configuration"
$deploymentLayout = Join-Path $packageLayout 'AppX'
$appRecipe = Join-Path $packageLayout 'CascadiaPackage.build.appxrecipe'
$terminalExe = Join-Path $deploymentLayout 'WindowsTerminal.exe'
$openConsoleModule = Join-Path $repoRoot 'tools\OpenConsole.psm1'
$solution = Join-Path $repoRoot 'OpenConsole.slnx'
$nuget = Join-Path $repoRoot 'dep\nuget\nuget.exe'
$nugetPackages = Join-Path $repoRoot 'dep\nuget\packages.config'
$oldClMpCount = $env:CL_MPCount

function Invoke-CheckedCommand
{
    param(
        [Parameter(Mandatory)]
        [string]$Executable,

        [Parameter(ValueFromRemainingArguments)]
        [string[]]$Arguments
    )

    & $Executable @Arguments
    if ($LASTEXITCODE -ne 0)
    {
        throw "Command failed with exit code $LASTEXITCODE`: $Executable"
    }
}

Push-Location $repoRoot
try
{
    Import-Module $openConsoleModule -Force
    Set-MsbuildDevEnvironment -Prerelease

    if (-not $SkipBuild)
    {
        Write-Host 'Restoring dependencies...' -ForegroundColor Cyan
        Invoke-CheckedCommand $nuget restore $solution
        Invoke-CheckedCommand $nuget restore $nugetPackages

        $buildTargets = [System.Collections.Generic.List[string]]::new()
        $buildTargets.Add('Terminal\CascadiaPackage')
        if ($IncludeWorkspaceTests)
        {
            $buildTargets.Add('Terminal\Tests\TaskCore_LocalTests')
            $buildTargets.Add('Terminal\Tests\TaskExecution_LocalTests')
            $buildTargets.Add('Terminal\Tests\TaskExecutionLockChild')
            $buildTargets.Add('Terminal\Tests\GitTestChild')
            $buildTargets.Add('Terminal\Tests\TaskRuntimeProtocol_LocalTests')
            $buildTargets.Add('Terminal\Tests\TaskRuntimeActivation_LocalTests')
            $buildTargets.Add('Terminal\Tests\TaskRuntimeClient_LocalTests')
            $buildTargets.Add('Terminal\Tests\TaskRuntimeActivationParent')
            $buildTargets.Add('Terminal\Tests\TaskRuntime_LocalTests')
            $buildTargets.Add('Terminal\Task\TaskRuntime')
            $buildTargets.Add('Terminal\Task\TaskExecution')
            $buildTargets.Add('Terminal\Tests\TerminalApp_LocalTests')
            $buildTargets.Add('Terminal\Tests\TestHostApp')
            $buildTargets.Add('Terminal\Tests\SettingsModel_UnitTests')
        }

        $buildDescription = if ($IncludeWorkspaceTests) { 'package and Workspace native tests' } else { 'package' }
        Write-Host "Building the optimized local Release $buildDescription..." -ForegroundColor Cyan
        $env:CL_MPCount = '1'
        $msbuild = (Get-Command msbuild.exe -ErrorAction Stop).Source
        $buildArguments = @(
            $solution
            "/t:$($buildTargets -join ';')"
            "/p:Configuration=$configuration"
            "/p:Platform=$Platform"
            '/p:WindowsTerminalBranding=Dev'
            '/p:AppxPackageSigningEnabled=false'
            '/p:AppxBundle=Never'
            '/p:AppxSymbolPackageEnabled=false'
            '/m:1'
            '/nologo'
            '/verbosity:minimal'
        )
        Invoke-CheckedCommand $msbuild @buildArguments
    }

    if (-not (Test-Path -LiteralPath $appRecipe -PathType Leaf))
    {
        throw "Release app recipe was not found: $appRecipe. Run again without -SkipBuild."
    }

    if ($BuildOnly)
    {
        Write-Host 'Local Release build completed.' -ForegroundColor Green
        return
    }

    $resolvedPackageBinRoot = [System.IO.Path]::GetFullPath($packageBinRoot).TrimEnd('\') + '\'
    Get-Process WindowsTerminal -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Path -and
            [System.IO.Path]::GetFullPath($_.Path).StartsWith($resolvedPackageBinRoot, [System.StringComparison]::OrdinalIgnoreCase)
        } |
        Stop-Process -Force

    Write-Host 'Registering the local Release package...' -ForegroundColor Cyan
    $deployAppRecipe = (Get-Command DeployAppRecipe.exe -ErrorAction Stop).Source
    Invoke-CheckedCommand $deployAppRecipe $appRecipe

    if ($NoLaunch)
    {
        Write-Host 'Local Release deployment completed.' -ForegroundColor Green
        return
    }

    $launcher = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\wtd.exe'
    if (-not (Test-Path -LiteralPath $launcher -PathType Leaf))
    {
        throw "The wtd.exe execution alias was not registered: $launcher"
    }

    Write-Host 'Launching the local Release build...' -ForegroundColor Cyan
    Start-Process -FilePath $launcher -ArgumentList @('-w', 'new')

    $started = $false
    for ($attempt = 0; $attempt -lt 20; $attempt++)
    {
        Start-Sleep -Milliseconds 250
        $started = [bool](Get-Process WindowsTerminal -ErrorAction SilentlyContinue |
            Where-Object { $_.Path -and [System.IO.Path]::GetFullPath($_.Path).Equals($terminalExe, [System.StringComparison]::OrdinalIgnoreCase) } |
            Select-Object -First 1)
        if ($started)
        {
            break
        }
    }

    if (-not $started)
    {
        throw "The local Release process did not start from the expected path: $terminalExe"
    }

    Write-Host 'Local Release is running. Use wtd.exe for future launches.' -ForegroundColor Green
}
finally
{
    if ($null -eq $oldClMpCount)
    {
        Remove-Item Env:\CL_MPCount -ErrorAction SilentlyContinue
    }
    else
    {
        $env:CL_MPCount = $oldClMpCount
    }
    Pop-Location
}
