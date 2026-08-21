[CmdletBinding(SupportsShouldProcess, ConfirmImpact = 'High')]
param(
    [ValidateSet('Debug', 'Root', 'Deep')]
    [string]$Scope = 'Root',

    [switch]$IncludePackages
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..')).TrimEnd('\')
$candidateTargets = [System.Collections.Generic.List[string]]::new()

function Assert-RepositoryChildPath
{
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $repoPrefix = $repoRoot + '\'
    if (-not $fullPath.StartsWith($repoPrefix, [System.StringComparison]::OrdinalIgnoreCase))
    {
        throw "Cleanup target is outside the repository: $fullPath"
    }

    return $fullPath
}

function Add-CleanupTarget
{
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    $fullPath = Assert-RepositoryChildPath -Path $Path
    if (Test-Path -LiteralPath $fullPath)
    {
        $candidateTargets.Add($fullPath)
    }
}

function Get-TargetStatistics
{
    param(
        [Parameter(Mandatory)]
        [string]$Path
    )

    $item = Get-Item -LiteralPath $Path -Force
    if ($item.PSIsContainer)
    {
        $measure = Get-ChildItem -LiteralPath $Path -Force -Recurse -File -ErrorAction SilentlyContinue |
            Measure-Object -Property Length -Sum
        $bytes = if ($null -eq $measure.Sum) { 0L } else { [long]$measure.Sum }
        $fileCount = $measure.Count
    }
    else
    {
        $bytes = [long]$item.Length
        $fileCount = 1
    }

    [pscustomobject]@{
        Path = $Path
        RelativePath = [System.IO.Path]::GetRelativePath($repoRoot, $Path)
        Bytes = $bytes
        SizeGB = [math]::Round($bytes / 1GB, 2)
        Files = $fileCount
    }
}

switch ($Scope)
{
    'Debug'
    {
        Add-CleanupTarget -Path (Join-Path $repoRoot 'obj\x64\Debug')
        Add-CleanupTarget -Path (Join-Path $repoRoot 'bin\x64\Debug')
    }
    'Root'
    {
        Add-CleanupTarget -Path (Join-Path $repoRoot 'obj')
        Add-CleanupTarget -Path (Join-Path $repoRoot 'bin')
    }
    'Deep'
    {
        Add-CleanupTarget -Path (Join-Path $repoRoot 'obj')
        Add-CleanupTarget -Path (Join-Path $repoRoot 'bin')

        $ignoredSourceItems = @(& git.exe -C $repoRoot ls-files --others --ignored --exclude-standard --directory -- src/)
        if ($LASTEXITCODE -ne 0)
        {
            throw "Unable to enumerate ignored source outputs; git exited with $LASTEXITCODE."
        }

        foreach ($relativePath in $ignoredSourceItems)
        {
            if (-not [string]::IsNullOrWhiteSpace($relativePath))
            {
                Add-CleanupTarget -Path (Join-Path $repoRoot $relativePath.TrimEnd('/', '\'))
            }
        }
    }
}

if ($IncludePackages)
{
    Add-CleanupTarget -Path (Join-Path $repoRoot 'packages')
}

$protectedRelativePaths = @('dist', '.git', '.PowershellModules')
if (-not $IncludePackages)
{
    $protectedRelativePaths += 'packages'
}

$protectedPaths = @($protectedRelativePaths | ForEach-Object {
    [System.IO.Path]::GetFullPath((Join-Path $repoRoot $_)).TrimEnd('\')
})

$uniqueTargets = [System.Collections.Generic.HashSet[string]]::new([System.StringComparer]::OrdinalIgnoreCase)
foreach ($candidate in $candidateTargets)
{
    $null = $uniqueTargets.Add($candidate.TrimEnd('\'))
}

$targets = [System.Collections.Generic.List[string]]::new()
foreach ($candidate in @($uniqueTargets | Sort-Object Length))
{
    $candidatePrefix = $candidate + '\'
    $coveredByParent = [bool]($targets | Where-Object {
        $candidate.StartsWith($_ + '\', [System.StringComparison]::OrdinalIgnoreCase)
    } | Select-Object -First 1)
    if (-not $coveredByParent)
    {
        $targets.Add($candidate)
    }

    foreach ($protectedPath in $protectedPaths)
    {
        if ($candidate.Equals($protectedPath, [System.StringComparison]::OrdinalIgnoreCase) -or
            $protectedPath.StartsWith($candidatePrefix, [System.StringComparison]::OrdinalIgnoreCase))
        {
            throw "Cleanup target overlaps a protected path: $candidate"
        }
    }
}

if ($targets.Count -eq 0)
{
    Write-Host "No build artifacts matched scope '$Scope'." -ForegroundColor Green
    return
}

Write-Host "Cleanup scope: $Scope" -ForegroundColor Cyan
Write-Host 'The dist, .git, and .PowershellModules directories are always preserved.'
if (-not $IncludePackages)
{
    Write-Host 'The NuGet packages directory is preserved. Use -IncludePackages to remove it.'
}

$statistics = @($targets | ForEach-Object { Get-TargetStatistics -Path $_ })
$statistics |
    Sort-Object Bytes -Descending |
    Select-Object RelativePath, SizeGB, Files |
    Format-Table -AutoSize |
    Out-Host

$totalBytes = [long](($statistics | Measure-Object -Property Bytes -Sum).Sum)
$totalFiles = [int](($statistics | Measure-Object -Property Files -Sum).Sum)
$summary = '{0} target(s), {1:N2} GB, {2:N0} file(s)' -f $targets.Count, ($totalBytes / 1GB), $totalFiles

$activeBuildProcesses = @(Get-Process msbuild, cl, link -ErrorAction SilentlyContinue)
if ($activeBuildProcesses.Count -gt 0)
{
    $processSummary = ($activeBuildProcesses | ForEach-Object { "$($_.ProcessName) ($($_.Id))" }) -join ', '
    throw "Build processes are still running: $processSummary"
}

if (-not $PSCmdlet.ShouldProcess($summary, 'Recursively delete the listed build artifacts'))
{
    return
}

foreach ($target in $targets)
{
    $target = Assert-RepositoryChildPath -Path $target
    Write-Host "Removing $([System.IO.Path]::GetRelativePath($repoRoot, $target))..." -ForegroundColor Cyan
    Remove-Item -LiteralPath $target -Recurse -Force -Confirm:$false
}

Write-Host ("Cleanup completed. Approximately {0:N2} GB was removed." -f ($totalBytes / 1GB)) -ForegroundColor Green
