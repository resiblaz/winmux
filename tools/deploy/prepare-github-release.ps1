#Requires -Version 7
[CmdletBinding(SupportsShouldProcess)]
param(
    [string]$Version,
    [switch]$Prerelease
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'GitHubRelease.ps1')

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$config = Get-WinmuxReleaseConfig
$sourceVersion = Get-WinmuxSourceVersion $repoRoot
if (-not $Version) { $Version = $sourceVersion }
Assert-ReleaseVersion $Version
if ($Version -cne $sourceVersion -or [version]$Version -lt [version]$config.baselineVersion)
{
    throw "Version must match Package-Dev.appxmanifest ($sourceVersion) and be at least $($config.baselineVersion)."
}
$commit = Assert-WinmuxReleaseSource $repoRoot
$null = Invoke-ReleaseCommand git @('-C', $repoRoot, 'ls-files', '--error-unmatch', '--', "doc/releases/$Version.md")
$notesSource = Join-Path $repoRoot "doc/releases/$Version.md"
$notes = ConvertTo-ReleaseNotes ([IO.File]::ReadAllText($notesSource))
if ([string]::IsNullOrWhiteSpace($notes)) { throw 'Release notes must not be empty.' }
$parent = Join-Path $repoRoot 'dist/releases'
$destination = Join-Path $parent $Version

if (Test-Path -LiteralPath $destination)
{
    $existing = Get-WinmuxReleaseBundle (Join-Path $destination 'release.json')
    if ($existing.Data.sourceCommit -cne $commit -or $existing.Data.prerelease -ne [bool]$Prerelease -or
        [IO.File]::ReadAllText($existing.Notes) -cne $notes)
    {
        throw 'This release version is already frozen with different source or metadata. Use a new version.'
    }
    Write-Host 'Reusing the validated, frozen release bundle.'
    return [pscustomobject]@{ Manifest = Join-Path $destination 'release.json'; Tag = $existing.Data.tag }
}

if (-not $PSCmdlet.ShouldProcess($destination, "Run the full x64 gate and prepare Winmux $Version from $commit"))
{
    return
}

$null = New-Item -ItemType Directory -Path $parent -Force
$staging = Join-Path $parent (".$Version.prepare-" + [guid]::NewGuid().ToString('N'))
$null = New-Item -ItemType Directory -Path $staging
$work = New-ReleaseTempDirectory
try
{
    # The gate owns the single package/native-test build graph and all tests.
    & (Get-Command pwsh -ErrorAction Stop).Source -NoLogo -NoProfile -File (Join-Path $PSScriptRoot 'test-workspace-release.ps1')
    if ($LASTEXITCODE -ne 0) { throw "The full release gate failed ($LASTEXITCODE)." }
    $gateCompleted = [DateTime]::UtcNow.ToString('o')
    $null = Assert-WinmuxReleaseSource $repoRoot $commit

    $packagePath = Join-Path $repoRoot "src/cascadia/CascadiaPackage/AppPackages/CascadiaPackage_$($Version)_x64_Test/CascadiaPackage_$($Version)_x64.msix"
    $packagePath = (Get-Item -LiteralPath $packagePath -ErrorAction Stop).FullName
    $msix = [IO.Compression.ZipFile]::OpenRead($packagePath)
    try
    {
        $reader = [IO.StreamReader]::new($msix.GetEntry('AppxManifest.xml').Open())
        try { $identity = ([xml]$reader.ReadToEnd()).Package.Identity }
        finally { $reader.Dispose() }
        if ($identity.Version -cne $Version -or $identity.ProcessorArchitecture -cne 'x64' -or
            $identity.Name -cne 'WindowsTerminalDev')
        {
            throw 'The gated MSIX does not match the release version, x64 architecture, or Dev package identity.'
        }
    }
    finally { $msix.Dispose() }

    $sourcePackageHash = Get-ReleaseSha256 $packagePath
    $portable = & (Join-Path $PSScriptRoot 'build-portable-release.ps1') -SkipBuild -NoLaunch -PackagePath $packagePath -OutputDirectory $work
    if ($portable.Version -cne $Version -or $portable.SourcePackage -cne $packagePath -or
        (Get-ReleaseSha256 $packagePath) -cne $sourcePackageHash)
    {
        throw 'The source package changed during release preparation.'
    }
    $packageName = "winmux_$($Version)_windows_x64.zip"
    $package = Join-Path $staging $packageName
    Copy-Item -LiteralPath $portable.Package -Destination $package
    Invoke-ReleasePortableSmoke $package $Version
    $null = Assert-WinmuxReleaseSource $repoRoot $commit
    Write-ReleaseUtf8 (Join-Path $staging 'release-notes.md') $notes
    $data = [ordered]@{
        schemaVersion = 1
        repository = $config.repository
        version = $Version
        tag = "v$Version"
        platform = 'x64'
        configuration = 'Release'
        sourceCommit = $commit
        baseline = [ordered]@{ version = $config.baselineVersion; commit = $config.baselineCommit }
        prerelease = [bool]$Prerelease
        preparedUtc = [DateTime]::UtcNow.ToString('o')
        gate = [ordered]@{ passed = $true; sourceCommit = $commit; completedUtc = $gateCompleted }
        package = [ordered]@{
            name = $packageName
            sha256 = Get-ReleaseSha256 $package
            size = (Get-Item -LiteralPath $package).Length
            sourceMsixSha256 = $sourcePackageHash
            binaryProductVersion = (Get-Item -LiteralPath $portable.Launcher).VersionInfo.ProductVersion
        }
        notesSha256 = Get-ReleaseSha256 (Join-Path $staging 'release-notes.md')
    }
    Write-ReleaseUtf8 (Join-Path $staging 'release.json') (($data | ConvertTo-Json -Depth 6) + [char]10)
    Write-ReleaseChecksums $staging $packageName
    $null = Get-WinmuxReleaseBundle (Join-Path $staging 'release.json')
    $null = Assert-WinmuxReleaseSource $repoRoot $commit
    [IO.Directory]::Move($staging, $destination)
    Write-Host "Prepared Winmux $Version (Windows x64) from $commit."
    [pscustomobject]@{ Manifest = Join-Path $destination 'release.json'; Tag = $data.tag }
}
finally
{
    Remove-ReleaseDirectory $work ([IO.Path]::GetTempPath())
    Remove-ReleaseDirectory $staging $parent
}
