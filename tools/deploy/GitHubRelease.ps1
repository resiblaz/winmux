# Shared validation for the Windows x64 GitHub release workflow.
#Requires -Version 7

function Invoke-ReleaseCommand
{
    param(
        [Parameter(Mandatory)][string]$Executable,
        [Parameter(Mandatory)][string[]]$Arguments,
        [switch]$AllowFailure
    )

    $PSNativeCommandUseErrorActionPreference = $false
    $output = @(& $Executable @Arguments 2>&1)
    $code = $LASTEXITCODE
    $result = [pscustomobject]@{
        ExitCode = $code
        Output = ($output | ForEach-Object { "$_" }) -join [Environment]::NewLine
    }
    if (-not $AllowFailure -and $code -ne 0)
    {
        throw "$Executable failed ($code): $($result.Output)"
    }
    if ($AllowFailure) { return $result }
    return $result.Output
}

function Assert-ReleaseVersion
{
    param([Parameter(Mandatory)][string]$Version)
    if ($Version -cnotmatch '^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)$' -or
        @($Version.Split('.') | Where-Object { [long]$_ -gt 65535 }).Count -ne 0)
    {
        throw "Release version must be four numbers between 0 and 65535: $Version"
    }
}

function Get-WinmuxReleaseConfig
{
    $config = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'github-release.json') -Raw | ConvertFrom-Json
    if ($config.schemaVersion -ne 1 -or $config.repository -cne 'resiblaz/winmux' -or
        $config.platform -cne 'x64' -or $config.baselineCommit -cnotmatch '^[0-9a-f]{40}$')
    {
        throw 'Invalid GitHub release configuration. Only resiblaz/winmux Windows x64 releases are supported.'
    }
    Assert-ReleaseVersion $config.baselineVersion
    return $config
}

function Get-WinmuxSourceVersion
{
    param([Parameter(Mandatory)][string]$RepoRoot)
    $manifest = [xml](Get-Content -LiteralPath (Join-Path $RepoRoot 'src/cascadia/CascadiaPackage/Package-Dev.appxmanifest') -Raw)
    $version = [string]$manifest.Package.Identity.Version
    Assert-ReleaseVersion $version
    return $version
}

function Assert-WinmuxReleaseSource
{
    param(
        [Parameter(Mandatory)][string]$RepoRoot,
        [string]$ExpectedCommit
    )
    $config = Get-WinmuxReleaseConfig
    $status = Invoke-ReleaseCommand git @('-C', $RepoRoot, 'status', '--porcelain', '--untracked-files=normal')
    if (-not [string]::IsNullOrWhiteSpace($status))
    {
        throw 'Release preparation and publishing require a clean committed worktree, including untracked source files.'
    }
    foreach ($arguments in @(
        @('-C', $RepoRoot, 'remote', 'get-url', 'origin'),
        @('-C', $RepoRoot, 'remote', 'get-url', '--push', 'origin')))
    {
        $url = (Invoke-ReleaseCommand git $arguments).Trim()
        if ($url -cnotmatch '^(https://github\.com/resiblaz/winmux(?:\.git)?|git@github\.com:resiblaz/winmux(?:\.git)?|ssh://git@github\.com/resiblaz/winmux(?:\.git)?)$')
        {
            throw 'Both origin fetch and push URLs must point to github.com/resiblaz/winmux.'
        }
    }
    $commit = (Invoke-ReleaseCommand git @('-C', $RepoRoot, 'rev-parse', 'HEAD')).Trim()
    if ($commit -cnotmatch '^[0-9a-f]{40}$' -or ($ExpectedCommit -and $commit -cne $ExpectedCommit))
    {
        throw "Source commit changed. Expected $ExpectedCommit; found $commit."
    }
    $null = Invoke-ReleaseCommand git @('-C', $RepoRoot, 'merge-base', '--is-ancestor', $config.baselineCommit, $commit)
    return $commit
}

function Write-ReleaseUtf8
{
    param([string]$Path, [string]$Content)
    [IO.File]::WriteAllText($Path, $Content, [Text.UTF8Encoding]::new($false))
}

function ConvertTo-ReleaseNotes
{
    param([AllowEmptyString()][string]$Content)
    return $Content.Replace(([string][char]13 + [char]10), [string][char]10).TrimEnd() + [char]10
}

function Get-ReleaseSha256
{
    param([Parameter(Mandatory)][string]$Path)
    return (Get-FileHash -LiteralPath $Path -Algorithm SHA256).Hash.ToLowerInvariant()
}

function New-ReleaseTempDirectory
{
    $parent = [IO.Path]::GetFullPath([IO.Path]::GetTempPath())
    $path = Join-Path $parent ('winmux-release-' + [guid]::NewGuid().ToString('N'))
    $null = New-Item -ItemType Directory -Path $path
    return $path
}

function Remove-ReleaseDirectory
{
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Parent)
    $resolved = [IO.Path]::GetFullPath($Path)
    $prefix = [IO.Path]::GetFullPath($Parent).TrimEnd('\', '/') + [IO.Path]::DirectorySeparatorChar
    if (-not $resolved.StartsWith($prefix, [StringComparison]::OrdinalIgnoreCase))
    {
        throw "Refusing to remove a directory outside $Parent."
    }
    if (Test-Path -LiteralPath $resolved) { Remove-Item -LiteralPath $resolved -Recurse -Force }
}

function Assert-X64PortableArchive
{
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Version)
    Assert-ReleaseVersion $Version
    $root = "winmux-$Version/"
    $zip = [IO.Compression.ZipFile]::OpenRead($Path)
    try
    {
        $names = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
        foreach ($entry in $zip.Entries)
        {
            $name = $entry.FullName
            if (-not $name.StartsWith($root, [StringComparison]::Ordinal) -or
                $name.Contains('\') -or $name.Contains(':') -or $name -match '(^|/)\.\.?(/|$)' -or
                (($entry.ExternalAttributes -shr 16) -band 0xF000) -eq 0xA000 -or -not $names.Add($name))
            {
                throw "Unsafe, duplicate, or unexpected archive entry: $name"
            }
            if ($name.StartsWith($root + 'settings/', [StringComparison]::OrdinalIgnoreCase) -and
                -not $name.EndsWith('/') -and $name -cne ($root + 'settings/settings.json'))
            {
                throw 'A release ZIP must contain only default settings, without session data or history.'
            }
        }
        foreach ($required in @('.portable', 'settings/settings.json', 'winmux.exe', 'wmx.exe',
                'OpenConsole.exe', 'Microsoft.UI.Xaml.dll', 'resources.pri'))
        {
            if ($null -eq $zip.GetEntry($root + $required)) { throw "Portable ZIP is missing $required." }
        }
        foreach ($executable in @('winmux.exe', 'wmx.exe', 'OpenConsole.exe', 'Microsoft.UI.Xaml.dll'))
        {
            $stream = $zip.GetEntry($root + $executable).Open()
            $memory = [IO.MemoryStream]::new()
            try
            {
                $stream.CopyTo($memory)
                $bytes = $memory.ToArray()
                if ($bytes.Length -lt 64 -or [BitConverter]::ToUInt16($bytes, 0) -ne 0x5A4D)
                {
                    throw "$executable is not a Windows PE image."
                }
                $offset = [BitConverter]::ToInt32($bytes, 0x3C)
                if ($offset -lt 64 -or $offset -gt $bytes.Length - 6 -or
                    [BitConverter]::ToUInt32($bytes, $offset) -ne 0x4550 -or
                    [BitConverter]::ToUInt16($bytes, $offset + 4) -ne 0x8664)
                {
                    throw "$executable is not built for Windows x64."
                }
            }
            finally { $stream.Dispose(); $memory.Dispose() }
        }
        $reader = [IO.StreamReader]::new($zip.GetEntry($root + 'settings/settings.json').Open())
        try { $settings = $reader.ReadToEnd() | ConvertFrom-Json }
        finally { $reader.Dispose() }
        if ($settings.tabLayout -cne 'workspaceSidebar') { throw 'Portable release settings must enable Workspace Sidebar.' }
    }
    finally { $zip.Dispose() }
}

function Invoke-ReleasePortableSmoke
{
    param([Parameter(Mandatory)][string]$Path, [Parameter(Mandatory)][string]$Version)
    Assert-X64PortableArchive $Path $Version
    $temporary = New-ReleaseTempDirectory
    try
    {
        Expand-Archive -LiteralPath $Path -DestinationPath $temporary
        $launcher = Join-Path $temporary "winmux-$Version/wmx.exe"
        $result = Invoke-ReleaseCommand $launcher @('workspace', 'invalid') -AllowFailure
        if ($result.ExitCode -ne 2)
        {
            throw "Extracted portable CLI smoke failed: expected exit 2, got $($result.ExitCode). $($result.Output)"
        }
    }
    finally { Remove-ReleaseDirectory $temporary ([IO.Path]::GetTempPath()) }
}

function Write-ReleaseChecksums
{
    param([Parameter(Mandatory)][string]$Directory, [Parameter(Mandatory)][string]$PackageName)
    $lines = foreach ($name in @($PackageName, 'release.json'))
    {
        "$(Get-ReleaseSha256 (Join-Path $Directory $name))  $name"
    }
    Write-ReleaseUtf8 (Join-Path $Directory 'SHA256SUMS.txt') (($lines -join [char]10) + [char]10)
}

function Get-WinmuxReleaseBundle
{
    param([Parameter(Mandatory)][string]$Manifest)
    $config = Get-WinmuxReleaseConfig
    $manifestPath = (Get-Item -LiteralPath $Manifest -ErrorAction Stop).FullName
    if ([IO.Path]::GetFileName($manifestPath) -cne 'release.json') { throw 'The manifest must be named release.json.' }
    $directory = [IO.Path]::GetDirectoryName($manifestPath)
    $data = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
    Assert-ReleaseVersion $data.version
    $packageName = "winmux_$($data.version)_windows_x64.zip"
    if ($data.schemaVersion -ne 1 -or $data.repository -cne $config.repository -or
        $data.platform -cne 'x64' -or $data.configuration -cne 'Release' -or
        $data.tag -cne "v$($data.version)" -or $data.sourceCommit -cnotmatch '^[0-9a-f]{40}$' -or
        $data.baseline.version -cne $config.baselineVersion -or $data.baseline.commit -cne $config.baselineCommit -or
        [version]$data.version -lt [version]$config.baselineVersion -or
        $data.gate.passed -isnot [bool] -or -not $data.gate.passed -or
        $data.gate.sourceCommit -cne $data.sourceCommit -or $data.prerelease -isnot [bool] -or
        $data.package.name -cne $packageName -or $data.package.sha256 -cnotmatch '^[0-9a-f]{64}$' -or
        $data.notesSha256 -cnotmatch '^[0-9a-f]{64}$')
    {
        throw 'Release manifest identity, architecture, baseline, or full-gate evidence is invalid.'
    }
    $package = Join-Path $directory $packageName
    $notes = Join-Path $directory 'release-notes.md'
    if ((Get-ReleaseSha256 $package) -cne $data.package.sha256 -or
        (Get-Item -LiteralPath $package).Length -ne $data.package.size -or
        (Get-ReleaseSha256 $notes) -cne $data.notesSha256)
    {
        throw 'Release package or notes differ from the frozen manifest.'
    }
    $manifestHash = Get-ReleaseSha256 $manifestPath
    $expectedSums = @(
        "$($data.package.sha256)  $packageName",
        "$manifestHash  release.json")
    $actualSums = @(Get-Content -LiteralPath (Join-Path $directory 'SHA256SUMS.txt'))
    if (($actualSums -join [char]10) -cne ($expectedSums -join [char]10))
    {
        throw 'SHA256SUMS.txt does not match the frozen ZIP and manifest.'
    }
    Assert-X64PortableArchive $package $data.version
    return [pscustomobject]@{
        Data = $data
        Directory = $directory
        Notes = $notes
        Assets = @($package, $manifestPath, (Join-Path $directory 'SHA256SUMS.txt'))
        AssetHashes = @{
            $packageName = $data.package.sha256
            'release.json' = $manifestHash
            'SHA256SUMS.txt' = Get-ReleaseSha256 (Join-Path $directory 'SHA256SUMS.txt')
        }
    }
}

function Invoke-WinmuxGitHubApi
{
    param([Parameter(Mandatory)][string]$Endpoint, [switch]$Paginate)
    $arguments = @('api', '--hostname', 'github.com', $Endpoint)
    if ($Paginate) { $arguments += @('--paginate', '--slurp') }
    return (Invoke-ReleaseCommand gh $arguments | ConvertFrom-Json -NoEnumerate)
}

function Get-WinmuxGitHubRelease
{
    param([Parameter(Mandatory)][string]$Repository, [Parameter(Mandatory)][string]$Tag)
    $pages = Invoke-WinmuxGitHubApi "repos/$Repository/releases?per_page=100" -Paginate
    foreach ($page in $pages)
    {
        foreach ($release in $page)
        {
            if ($release.tag_name -ceq $Tag) { return $release }
        }
    }
    return $null
}

function Assert-WinmuxRemoteTag
{
    param([string]$RepoRoot, [string]$Tag, [string]$Commit)
    $output = Invoke-ReleaseCommand git @('-C', $RepoRoot, 'ls-remote', 'origin', "refs/tags/$Tag", "refs/tags/$Tag^{}")
    if ([string]::IsNullOrWhiteSpace($output)) { return $false }
    $lines = @($output -split '\r?\n')
    $peeled = @($lines | Where-Object { $_.EndsWith('^{}') })
    $target = if ($peeled.Count) { ($peeled[0] -split '\s+')[0] } else { ($lines[0] -split '\s+')[0] }
    if ($target -cne $Commit) { throw "Remote tag $Tag points to a different commit." }
    return $true
}

function Assert-WinmuxRemoteRelease
{
    param($Release, $Bundle)
    if ($Release.tag_name -cne $Bundle.Data.tag -or
        $Release.prerelease -ne $Bundle.Data.prerelease -or
        $Release.name -cne "Winmux $($Bundle.Data.version)" -or
        (ConvertTo-ReleaseNotes $Release.body) -cne (ConvertTo-ReleaseNotes ([IO.File]::ReadAllText($Bundle.Notes))))
    {
        throw 'Existing GitHub release metadata differs from the frozen release.'
    }
    $allowed = @($Bundle.Assets | ForEach-Object { [IO.Path]::GetFileName($_) })
    $seen = [Collections.Generic.HashSet[string]]::new([StringComparer]::OrdinalIgnoreCase)
    foreach ($asset in $Release.assets)
    {
        if ($allowed -cnotcontains $asset.name -or -not $seen.Add($asset.name) -or $asset.state -cne 'uploaded')
        {
            throw "Unexpected, duplicate, or incomplete GitHub release asset: $($asset.name)"
        }
        $local = Join-Path $Bundle.Directory $asset.name
        if ($asset.size -ne (Get-Item -LiteralPath $local).Length)
        {
            throw "GitHub release asset has a conflicting size: $($asset.name)"
        }
    }
}

function Test-WinmuxRemoteAssets
{
    param($Release, $Bundle, [switch]$Smoke)
    Assert-WinmuxRemoteRelease $Release $Bundle
    if (@($Release.assets).Count -eq 0) { return }
    $temporary = New-ReleaseTempDirectory
    try
    {
        $arguments = @('release', 'download', $Bundle.Data.tag, '--repo',
            "github.com/$($Bundle.Data.repository)", '--dir', $temporary)
        foreach ($asset in $Release.assets) { $arguments += @('--pattern', $asset.name) }
        $null = Invoke-ReleaseCommand gh $arguments
        foreach ($asset in $Release.assets)
        {
            if ((Get-ReleaseSha256 (Join-Path $temporary $asset.name)) -cne
                $Bundle.AssetHashes[$asset.name])
            {
                throw "Downloaded GitHub release asset has a conflicting SHA256: $($asset.name)"
            }
        }
        if ($Smoke)
        {
            if (@($Release.assets).Count -ne $Bundle.Assets.Count) { throw 'Release attachments are incomplete.' }
            Invoke-ReleasePortableSmoke (Join-Path $temporary $Bundle.Data.package.name) $Bundle.Data.version
        }
    }
    finally { Remove-ReleaseDirectory $temporary ([IO.Path]::GetTempPath()) }
}
