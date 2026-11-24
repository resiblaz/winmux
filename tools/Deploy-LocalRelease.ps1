[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string]$Platform = 'x64',

    [switch]$SkipBuild,

    [switch]$NoLaunch,

    [switch]$BuildOnly,

    [switch]$IncludeWorkspaceTests,

    [switch]$IncludeUiTests,

    [ValidateRange(0, 64)]
    [int]$MaxCpuCount = 0,

    [switch]$NormalPriority,

    [switch]$ForceRestore
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$configuration = 'Release'
$packageBinRoot = Join-Path $repoRoot 'src\cascadia\CascadiaPackage\bin'
$packageLayout = Join-Path $packageBinRoot "$Platform\$configuration"
$deploymentLayout = Join-Path $packageLayout 'AppX'
$appRecipe = Join-Path $packageLayout 'CascadiaPackage.build.appxrecipe'
$terminalExe = Join-Path $deploymentLayout 'winmux.exe'
$openConsoleModule = Join-Path $repoRoot 'tools\OpenConsole.psm1'
$solution = Join-Path $repoRoot 'OpenConsole.slnx'
$nuget = Join-Path $repoRoot 'dep\nuget\nuget.exe'
$nugetPackages = Join-Path $repoRoot 'dep\nuget\packages.config'
$packagesDirectory = Join-Path $repoRoot 'packages'
$packageAssets = Join-Path $repoRoot 'src\cascadia\CascadiaPackage\obj\project.assets.json'
$restoreStamp = Join-Path $repoRoot "obj\$Platform\$configuration\.nuget-restore.stamp"
$oldClMpCount = $env:CL_MPCount

function Resolve-BuildCpuCount
{
    param([int]$Requested)

    if ($Requested -gt 0)
    {
        return $Requested
    }

    # Keep CPU headroom by default (e.g. 4 of 16 logical threads) so interactive apps and games stay smooth.
    $defaultCount = [int][Math]::Floor([Environment]::ProcessorCount / 4)
    return [Math]::Clamp($defaultCount, 2, 6)
}

function Test-NuGetRestoreUpToDate
{
    if (-not (Test-Path -LiteralPath $packagesDirectory -PathType Container))
    {
        return $false
    }
    if (-not (Get-ChildItem -LiteralPath $packagesDirectory -Directory -ErrorAction SilentlyContinue | Select-Object -First 1))
    {
        return $false
    }
    if (-not (Test-Path -LiteralPath $packageAssets -PathType Leaf))
    {
        return $false
    }

    $referenceTime = if (Test-Path -LiteralPath $restoreStamp -PathType Leaf)
    {
        (Get-Item -LiteralPath $restoreStamp).LastWriteTimeUtc
    }
    else
    {
        (Get-Item -LiteralPath $packageAssets).LastWriteTimeUtc
    }

    $restoreInputs = @(
        $solution,
        (Join-Path $repoRoot 'NuGet.Config'),
        $nugetPackages,
        (Join-Path $repoRoot '.nuget\packages.config'),
        (Join-Path $repoRoot 'build\packages.config'),
        (Join-Path $repoRoot 'src\common.nugetversions.props'),
        (Join-Path $repoRoot 'src\cascadia\CascadiaPackage\CascadiaPackage.wapproj')
    )

    foreach ($inputPath in $restoreInputs)
    {
        if ((Test-Path -LiteralPath $inputPath -PathType Leaf) -and
            (Get-Item -LiteralPath $inputPath).LastWriteTimeUtc -gt $referenceTime)
        {
            return $false
        }
    }

    return $true
}

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
        Invoke-CheckedCommand $Executable @Arguments
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
        Invoke-CheckedCommand $Executable @Arguments
    }
    finally
    {
        if ($priorityChanged)
        {
            $currentProcess.PriorityClass = $previousPriority
        }
    }
}

Push-Location $repoRoot
try
{
    Import-Module $openConsoleModule -Force
    Set-MsbuildDevEnvironment -Prerelease

    if (-not $SkipBuild)
    {
        if ($ForceRestore -or -not (Test-NuGetRestoreUpToDate))
        {
            Write-Host 'Restoring dependencies...' -ForegroundColor Cyan
            Invoke-CheckedCommand $nuget restore $solution
            Invoke-CheckedCommand $nuget restore $nugetPackages

            $stampDirectory = Split-Path -Path $restoreStamp -Parent
            New-Item -ItemType Directory -Path $stampDirectory -Force | Out-Null
            Set-Content -LiteralPath $restoreStamp -Value ([DateTime]::UtcNow.ToString('o')) -Encoding Ascii
        }
        else
        {
            Write-Host 'NuGet packages are up to date; skipping restore (pass -ForceRestore to force).' -ForegroundColor DarkCyan
        }

        $buildTargets = [System.Collections.Generic.List[string]]::new()
        $buildTargets.Add('Terminal\CascadiaPackage')
        if ($IncludeWorkspaceTests)
        {
            $buildTargets.Add('Terminal\Tests\TerminalApp_LocalTests')
            if ($IncludeUiTests)
            {
                $buildTargets.Add('Terminal\Tests\TestHostApp')
            }
            $buildTargets.Add('Terminal\Tests\SettingsModel_UnitTests')
            $buildTargets.Add('Terminal\Tests\Control_UnitTests')
            $buildTargets.Add('Terminal\Tests\UnitTests')
            $buildTargets.Add('Terminal\Tests\TerminalApp_UnitTests')
        }

        $resolvedMaxCpuCount = Resolve-BuildCpuCount -Requested $MaxCpuCount
        $priorityLabel = if ($NormalPriority) { 'Normal' } else { 'BelowNormal' }
        $buildDescription = if ($IncludeWorkspaceTests) { 'package and Workspace native tests' } else { 'package' }
        Write-Host "Building the optimized local Release $buildDescription (jobs=$resolvedMaxCpuCount, priority=$priorityLabel)..." -ForegroundColor Cyan
        $env:CL_MPCount = "$resolvedMaxCpuCount"
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
            '/p:UseMultiToolTask=true'
            '/p:EnforceProcessCountAcrossBuilds=true'
            "/m:$resolvedMaxCpuCount"
            '/nologo'
            '/verbosity:minimal'
        )
        Invoke-BuildCommand -Executable $msbuild -Arguments $buildArguments -UseBelowNormalPriority:(-not $NormalPriority)
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
    $runningPackages = @(Get-Process winmux, WindowsTerminal -ErrorAction SilentlyContinue |
        Where-Object {
            $_.Path -and
            [System.IO.Path]::GetFullPath($_.Path).StartsWith($resolvedPackageBinRoot, [System.StringComparison]::OrdinalIgnoreCase)
        })
    foreach ($process in $runningPackages)
    {
        $null = $process.CloseMainWindow()
    }
    foreach ($process in $runningPackages)
    {
        if (-not $process.WaitForExit(20000))
        {
            throw 'The local terminal is still running. Close it normally so its session can be saved, then retry deployment.'
        }
    }

    Write-Host 'Registering the local Release package...' -ForegroundColor Cyan
    $deployAppRecipe = (Get-Command DeployAppRecipe.exe -ErrorAction Stop).Source
    Invoke-CheckedCommand $deployAppRecipe $appRecipe

    if ($NoLaunch)
    {
        Write-Host 'Local Release deployment completed.' -ForegroundColor Green
        return
    }

    $launcher = Join-Path $env:LOCALAPPDATA 'Microsoft\WindowsApps\wmx.exe'
    if (-not (Test-Path -LiteralPath $launcher -PathType Leaf))
    {
        throw "The wmx.exe execution alias was not registered: $launcher"
    }

    Write-Host 'Launching the local Release build...' -ForegroundColor Cyan
    Start-Process -FilePath $launcher -ArgumentList @('-w', 'new')

    $started = $false
    for ($attempt = 0; $attempt -lt 20; $attempt++)
    {
        Start-Sleep -Milliseconds 250
        $started = [bool](Get-Process winmux -ErrorAction SilentlyContinue |
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

    Write-Host 'Winmux is running. Use wmx.exe for future launches.' -ForegroundColor Green
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
