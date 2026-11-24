#Requires -Version 7
[CmdletBinding()]
param(
    [switch]$SkipWinmuxBuild,
    [switch]$IncludeUiTests,
    [switch]$SkipMobileApk
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$distRoot = Join-Path $repoRoot 'dist'
$bridgeRoot = Join-Path $repoRoot 'tools\remote-bridge'

Write-Host '=== [1/4] Testing Remote Bridge & building standalone winmux-bridge.exe ===' -ForegroundColor Cyan
& node --test (Join-Path $bridgeRoot 'test\bridge.test.mjs')
if ($LASTEXITCODE -ne 0) { throw "Remote Bridge unit tests failed ($LASTEXITCODE)." }

$bridgeExe = Join-Path $bridgeRoot 'dist\winmux-bridge.exe'
& node (Join-Path $bridgeRoot 'build-sea.mjs') --out $bridgeExe
if ($LASTEXITCODE -ne 0) { throw "SEA build failed ($LASTEXITCODE)." }

if (-not $SkipWinmuxBuild)
{
    Write-Host '=== [2/4] Building Winmux x64 Release & running release gate ===' -ForegroundColor Cyan
    $gateArgs = @('-NoLogo', '-NoProfile', '-File', (Join-Path $PSScriptRoot 'test-workspace-release.ps1'))
    if ($IncludeUiTests) { $gateArgs += '-IncludeUiTests' }
    & (Get-Command pwsh -ErrorAction Stop).Source @gateArgs
    if ($LASTEXITCODE -ne 0) { throw "Winmux release gate failed ($LASTEXITCODE)." }
}
else
{
    Write-Host '=== [2/4] Skipping Winmux C++ rebuild (-SkipWinmuxBuild); running static release checks ===' -ForegroundColor Yellow
    & (Get-Command pwsh -ErrorAction Stop).Source -NoLogo -NoProfile -File (Join-Path $PSScriptRoot 'test-github-release.ps1')
    if ($LASTEXITCODE -ne 0) { throw "Static GitHub release checks failed ($LASTEXITCODE)." }
}

Write-Host '=== [3/4] Packaging zero-dependency Winmux x64 Portable ZIP ===' -ForegroundColor Cyan
$portable = & (Join-Path $PSScriptRoot 'build-portable-release.ps1') -SkipBuild -NoLaunch -IncludeBridgeExe
$artifacts = [System.Collections.Generic.List[object]]::new()
$artifacts.Add([pscustomobject]@{
    Name = [System.IO.Path]::GetFileName($portable.Package)
    Path = $portable.Package
    SHA256 = $portable.SHA256
    SizeMB = [Math]::Round(((Get-Item -LiteralPath $portable.Package).Length / 1MB), 1)
})

if (-not $SkipMobileApk)
{
    Write-Host '=== [4/4] Building signed Android Release APK ===' -ForegroundColor Cyan
    & (Join-Path $repoRoot 'mobile\android\build-apk.ps1') -Release
    if ($LASTEXITCODE -ne 0) { throw "Android Release APK build failed ($LASTEXITCODE)." }

    $apkSource = Join-Path $repoRoot 'mobile\android\app\build\outputs\apk\release\app-release.apk'
    $apkDestName = "winmux_mobile_$($portable.Version)_android.apk"
    $apkDest = Join-Path $distRoot $apkDestName
    Copy-Item -LiteralPath $apkSource -Destination $apkDest -Force
    $apkHash = (Get-FileHash -LiteralPath $apkDest -Algorithm SHA256).Hash
    $artifacts.Add([pscustomobject]@{
        Name = $apkDestName
        Path = $apkDest
        SHA256 = $apkHash
        SizeMB = [Math]::Round(((Get-Item -LiteralPath $apkDest).Length / 1MB), 1)
    })
}

$checksumsPath = Join-Path $distRoot 'SHA256SUMS.txt'
$checksumLines = $artifacts | ForEach-Object { "$($_.SHA256.ToLowerInvariant())  $($_.Name)" }
[System.IO.File]::WriteAllText($checksumsPath, ($checksumLines -join "`n") + "`n", [System.Text.UTF8Encoding]::new($false))

Write-Host ''
Write-Host '=== Winmux Unified Release Artifacts ===' -ForegroundColor Green
$artifacts | Format-Table Name, SizeMB, SHA256 -AutoSize
Write-Host "Checksums : $checksumsPath" -ForegroundColor Green
Write-Host "Deployment: $($portable.Deployment)" -ForegroundColor Green
