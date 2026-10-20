# Offline integration tests: all git/GitHub calls are intercepted in this process.
#Requires -Version 7
Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'GitHubRelease.ps1')

$testRoot = New-ReleaseTempDirectory
$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$config = Get-WinmuxReleaseConfig
$version = Get-WinmuxSourceVersion $repoRoot
$packageName = "winmux_$($version)_windows_x64.zip"
$manifest = Join-Path $testRoot 'release.json'
$package = Join-Path $testRoot $packageName
$publishScript = Join-Path $PSScriptRoot 'publish-github-release.ps1'
$assertions = 0

function Assert-ReleaseTest([bool]$Condition, [string]$Message)
{
    if (-not $Condition) { throw $Message }
    $script:assertions++
}

function Expect-ReleaseFailure([scriptblock]$Action, [string]$Pattern)
{
    $failed = $false
    try { $null = & $Action }
    catch
    {
        if ("$_" -notmatch $Pattern) { throw "Unexpected failure: $_ (expected $Pattern)" }
        $failed = $true
    }
    Assert-ReleaseTest $failed "Expected failure matching: $Pattern"
}

function Save-ReleaseFixture
{
    $script:fixture.package.sha256 = Get-ReleaseSha256 $package
    $script:fixture.package.size = (Get-Item -LiteralPath $package).Length
    Write-ReleaseUtf8 $manifest ($script:fixture | ConvertTo-Json -Depth 8)
    Write-ReleaseChecksums $testRoot $packageName
}

function Reset-ReleaseMock
{
    $cloud = Join-Path $testRoot 'cloud'
    Remove-ReleaseDirectory $cloud $testRoot
    $null = New-Item -ItemType Directory -Path $cloud
    $global:WinmuxReleaseTest = @{
        Commit = '1111111111111111111111111111111111111111'
        LocalTag = $null
        RemoteTag = $null
        Dirty = $false
        Release = $null
        Cloud = $cloud
        FailUpload = $null
        Mutations = [Collections.Generic.List[string]]::new()
        Downloads = 0
        Latest = $false
    }
}

try
{
    # A tiny real x64 console executable lets the download/publish path execute
    # its smoke check, without starting Winmux or touching any user's settings.
    $compiler = Join-Path $env:WINDIR 'Microsoft.NET/Framework64/v4.0.30319/csc.exe'
    $source = Join-Path $testRoot 'Smoke.cs'
    $executable = Join-Path $testRoot 'Smoke.exe'
    Write-ReleaseUtf8 $source 'class Smoke { static int Main(string[] a) { return a.Length == 2 && a[0] == "workspace" && a[1] == "invalid" ? 2 : 0; } }'
    $null = Invoke-ReleaseCommand $compiler @('/nologo', '/target:exe', '/platform:x64', "/out:$executable", $source)
    $payload = Join-Path $testRoot 'payload'
    $app = Join-Path $payload "winmux-$version"
    $null = New-Item -ItemType Directory -Path (Join-Path $app 'settings') -Force
    foreach ($name in @('winmux.exe', 'wmx.exe', 'OpenConsole.exe', 'Microsoft.UI.Xaml.dll'))
    {
        Copy-Item -LiteralPath $executable -Destination (Join-Path $app $name)
    }
    Write-ReleaseUtf8 (Join-Path $app '.portable') ''
    Write-ReleaseUtf8 (Join-Path $app 'resources.pri') 'fixture'
    Write-ReleaseUtf8 (Join-Path $app 'settings/settings.json') '{"tabLayout":"workspaceSidebar"}'
    [IO.Compression.ZipFile]::CreateFromDirectory($payload, $package)
    $originalZip = [IO.File]::ReadAllBytes($package)
    Write-ReleaseUtf8 (Join-Path $testRoot 'release-notes.md') (ConvertTo-ReleaseNotes 'Release fixture.')
    $script:fixture = [ordered]@{
        schemaVersion = 1
        repository = $config.repository
        version = $version
        tag = "v$version"
        platform = 'x64'
        configuration = 'Release'
        sourceCommit = '1111111111111111111111111111111111111111'
        baseline = @{ version = $config.baselineVersion; commit = $config.baselineCommit }
        prerelease = $false
        gate = @{ passed = $true; sourceCommit = '1111111111111111111111111111111111111111' }
        package = @{ name = $packageName; sha256 = ''; size = 0 }
        notesSha256 = Get-ReleaseSha256 (Join-Path $testRoot 'release-notes.md')
    }
    Save-ReleaseFixture
    $bundle = Get-WinmuxReleaseBundle $manifest
    Assert-ReleaseTest ($bundle.Assets.Count -eq 3) 'Only the ZIP, manifest, and checksums should be uploaded.'
    Invoke-ReleasePortableSmoke $package $version
    Assert-ReleaseTest $true 'The extracted fixture CLI should pass its smoke check.'

    [IO.File]::AppendAllText($package, 'tampered')
    Expect-ReleaseFailure { Get-WinmuxReleaseBundle $manifest } 'differ from the frozen manifest'
    [IO.File]::WriteAllBytes($package, $originalZip)
    $script:fixture.tag = "winmux-v$version"
    Save-ReleaseFixture
    Expect-ReleaseFailure { Get-WinmuxReleaseBundle $manifest } 'identity'
    $script:fixture.tag = "v$version"
    $script:fixture.platform = 'x86'
    Save-ReleaseFixture
    Expect-ReleaseFailure { Get-WinmuxReleaseBundle $manifest } 'architecture'
    $script:fixture.platform = 'x64'
    Save-ReleaseFixture
    $zip = [IO.Compression.ZipFile]::Open($package, [IO.Compression.ZipArchiveMode]::Update)
    $entry = $zip.CreateEntry("winmux-$version/../../escape.txt")
    $writer = [IO.StreamWriter]::new($entry.Open())
    $writer.Write('escape')
    $writer.Dispose()
    $zip.Dispose()
    Save-ReleaseFixture
    Expect-ReleaseFailure { Get-WinmuxReleaseBundle $manifest } 'Unsafe'
    [IO.File]::WriteAllBytes($package, $originalZip)
    $zip = [IO.Compression.ZipFile]::Open($package, [IO.Compression.ZipArchiveMode]::Update)
    $zip.GetEntry("winmux-$version/wmx.exe").Delete()
    $entry = $zip.CreateEntry("winmux-$version/wmx.exe")
    $bytes = [IO.File]::ReadAllBytes($executable)
    $peOffset = [BitConverter]::ToInt32($bytes, 0x3C)
    $bytes[$peOffset + 4] = 0x4C
    $bytes[$peOffset + 5] = 0x01
    $stream = $entry.Open()
    $stream.Write($bytes, 0, $bytes.Length)
    $stream.Dispose()
    $zip.Dispose()
    Save-ReleaseFixture
    Expect-ReleaseFailure { Get-WinmuxReleaseBundle $manifest } 'not built for Windows x64'
    [IO.File]::WriteAllBytes($package, $originalZip)
    Save-ReleaseFixture

    # These functions shadow applications only inside this test process.
    function global:git
    {
        $global:LASTEXITCODE = 0
        $a = @($args)
        if ($a[0] -eq '-C') { $a = @($a | Select-Object -Skip 2) }
        $state = $global:WinmuxReleaseTest
        switch ($a[0])
        {
            'status' { if ($state.Dirty) { ' M tracked.ps1' }; return }
            'remote' { 'https://github.com/resiblaz/winmux.git'; return }
            'merge-base' { return }
            'ls-files' { $a[-1]; return }
            'rev-parse' { if ($a[1] -eq 'HEAD') { $state.Commit } else { $state.LocalTag }; return }
            'show-ref' { if (-not $state.LocalTag) { $global:LASTEXITCODE = 1 }; return }
            'ls-remote' { if ($state.RemoteTag) { $state.RemoteTag + [char]9 + "refs/tags/v$version^{}" }; return }
            'tag' {
                if ($a[2] -cne "v$version") { throw 'Release tags must use v<version>.' }
                $state.LocalTag = $a[3]; $state.Mutations.Add('tag'); return
            }
            'push' {
                if ($a[1] -cne 'origin' -or $a[2] -cne "refs/tags/v$($version):refs/tags/v$version")
                {
                    throw 'Only the exact v<version> release tag may be pushed.'
                }
                $state.RemoteTag = $state.LocalTag; $state.Mutations.Add('push'); return
            }
            default { throw "Unexpected test git command: $($a -join ' ')" }
        }
    }
    function global:gh
    {
        $global:LASTEXITCODE = 0
        $a = @($args)
        $state = $global:WinmuxReleaseTest
        if ($a[0] -eq 'api')
        {
            $endpoint = @($a | Where-Object { $_ -like 'repos/*' })[0]
            if ($endpoint -eq 'repos/resiblaz/winmux')
            {
                '{"full_name":"resiblaz/winmux","permissions":{"push":true}}'
            }
            elseif ($endpoint -like '*/compare/*') { '{"status":"ahead"}' }
            elseif ($endpoint -like '*/releases?per_page=*')
            {
                $releases = if ($null -eq $state.Release) { @() } else { @($state.Release) }
                '[' + (ConvertTo-Json -InputObject $releases -Depth 12 -Compress) + ']'
            }
            elseif ($endpoint -like '*/releases/1') { ConvertTo-Json -InputObject $state.Release -Depth 12 -Compress }
            else { throw "Unexpected test endpoint: $endpoint" }
            return
        }
        if ($a[0] -ne 'release') { throw 'Unexpected test gh command.' }
        switch ($a[1])
        {
            'create' {
                $notesPath = $a[[Array]::IndexOf($a, '--notes-file') + 1]
                $title = $a[[Array]::IndexOf($a, '--title') + 1]
                $state.Release = @{
                    id = 1; tag_name = $a[2]; name = $title
                    body = [IO.File]::ReadAllText($notesPath)
                    draft = $true; prerelease = ($a -contains '--prerelease')
                    assets = @(); html_url = 'https://github.com/resiblaz/winmux/releases/tag/' + $a[2]
                }
                $state.Mutations.Add('create')
                return
            }
            'upload' {
                $path = $a[3]
                $name = [IO.Path]::GetFileName($path)
                if ($state.FailUpload -eq $name)
                {
                    $state.FailUpload = $null
                    $global:LASTEXITCODE = 1
                    'simulated upload interruption'
                    return
                }
                if ($a -contains '--clobber') { throw 'Release assets must never be overwritten.' }
                Copy-Item -LiteralPath $path -Destination (Join-Path $state.Cloud $name)
                $state.Release.assets += @{ name = $name; size = (Get-Item -LiteralPath $path).Length; state = 'uploaded' }
                $state.Mutations.Add("upload:$name")
                return
            }
            'download' {
                $directory = $a[[Array]::IndexOf($a, '--dir') + 1]
                foreach ($asset in $state.Release.assets)
                {
                    Copy-Item -LiteralPath (Join-Path $state.Cloud $asset.name) -Destination (Join-Path $directory $asset.name)
                }
                $state.Downloads++
                return
            }
            'edit' {
                if ($state.Release.assets.Count -ne 3 -or $state.Downloads -eq 0) { throw 'Publishing before verification.' }
                $state.Release.draft = $false
                $state.Latest = $a -contains '--latest'
                $state.Mutations.Add('publish')
                return
            }
            default { throw "Unexpected test gh command: $($a -join ' ')" }
        }
    }

    Reset-ReleaseMock
    $null = & $publishScript -Manifest $manifest -WhatIf
    Assert-ReleaseTest ($global:WinmuxReleaseTest.Mutations.Count -eq 0) 'WhatIf must not modify tags or GitHub.'
    $global:WinmuxReleaseTest.Dirty = $true
    Expect-ReleaseFailure { & (Join-Path $PSScriptRoot 'prepare-github-release.ps1') -WhatIf } 'clean committed worktree'

    # Exercise the real preparation script in an isolated miniature checkout.
    # Its gate is a child-process fixture; packaging returns the real test ZIP.
    Reset-ReleaseMock
    $miniRoot = Join-Path $testRoot 'checkout'
    $miniDeploy = Join-Path $miniRoot 'tools/deploy'
    $miniPackage = Join-Path $miniRoot 'src/cascadia/CascadiaPackage'
    $null = New-Item -ItemType Directory -Path $miniDeploy, $miniPackage, (Join-Path $miniRoot 'doc/releases') -Force
    foreach ($name in @('prepare-github-release.ps1', 'GitHubRelease.ps1', 'github-release.json'))
    {
        Copy-Item -LiteralPath (Join-Path $PSScriptRoot $name) -Destination (Join-Path $miniDeploy $name)
    }
    Write-ReleaseUtf8 (Join-Path $miniPackage 'Package-Dev.appxmanifest') "<Package><Identity Version='$version'/></Package>"
    Write-ReleaseUtf8 (Join-Path $miniRoot "doc/releases/$version.md") 'Release fixture.'
    Copy-Item -LiteralPath $package -Destination (Join-Path $miniDeploy 'fixture.zip')
    Copy-Item -LiteralPath $executable -Destination (Join-Path $miniDeploy 'fixture.exe')
    Write-ReleaseUtf8 (Join-Path $miniDeploy 'build-portable-release.ps1') @'
param([switch]$SkipBuild, [switch]$NoLaunch, [string]$PackagePath, [string]$OutputDirectory)
if (-not $SkipBuild -or -not $NoLaunch -or -not $PackagePath) { throw 'Preparation must reuse its exact gated package.' }
$root = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$version = ([xml](Get-Content -LiteralPath (Join-Path $root 'src/cascadia/CascadiaPackage/Package-Dev.appxmanifest') -Raw)).Package.Identity.Version
[pscustomobject]@{
    Package = Join-Path $PSScriptRoot 'fixture.zip'
    SourcePackage = $PackagePath
    Version = $version
    Launcher = Join-Path $PSScriptRoot 'fixture.exe'
}
'@
    $miniPrepare = Join-Path $miniDeploy 'prepare-github-release.ps1'
    $miniGate = Join-Path $miniDeploy 'test-workspace-release.ps1'
    $miniManifest = Join-Path $miniRoot "dist/releases/$version/release.json"
    $miniMsixDirectory = Join-Path $miniPackage "AppPackages/CascadiaPackage_$($version)_x64_Test"
    $null = New-Item -ItemType Directory -Path $miniMsixDirectory -Force
    $miniMsix = Join-Path $miniMsixDirectory "CascadiaPackage_$($version)_x64.msix"
    function Write-ReleaseMsixFixture([string]$IdentityVersion)
    {
        if (Test-Path -LiteralPath $miniMsix) { Remove-Item -LiteralPath $miniMsix -Force }
        $zip = [IO.Compression.ZipFile]::Open($miniMsix, [IO.Compression.ZipArchiveMode]::Create)
        try
        {
            $writer = [IO.StreamWriter]::new($zip.CreateEntry('AppxManifest.xml').Open())
            try { $writer.Write("<Package><Identity Name='WindowsTerminalDev' Version='$IdentityVersion' ProcessorArchitecture='x64'/></Package>") }
            finally { $writer.Dispose() }
        }
        finally { $zip.Dispose() }
    }
    $null = & $miniPrepare -WhatIf
    Assert-ReleaseTest (-not (Test-Path -LiteralPath (Join-Path $miniRoot 'dist'))) 'Preparation WhatIf must not build or create a staging directory.'
    Write-ReleaseUtf8 $miniGate 'exit 1'
    Expect-ReleaseFailure { & $miniPrepare } 'full release gate failed'
    Assert-ReleaseTest (-not (Test-Path -LiteralPath $miniManifest)) 'A failing gate must not produce release evidence.'
    Write-ReleaseUtf8 $miniGate @'
[IO.File]::AppendAllText((Join-Path $PSScriptRoot 'gate-runs.txt'), 'gate')
exit 0
'@
    Write-ReleaseMsixFixture '65535.0.0.0'
    Expect-ReleaseFailure { & $miniPrepare } 'gated MSIX does not match'
    Write-ReleaseMsixFixture $version
    $null = & $miniPrepare
    $prepared = Get-WinmuxReleaseBundle $miniManifest
    Assert-ReleaseTest ($prepared.Data.gate.passed -and $prepared.Data.sourceCommit -eq $global:WinmuxReleaseTest.Commit) 'Successful preparation must bind a full-gate result to its source commit.'
    Assert-ReleaseTest ($prepared.Data.tag -ceq "v$version") 'Preparation must freeze the v<version> release tag.'
    $gateRuns = [IO.File]::ReadAllText((Join-Path $miniDeploy 'gate-runs.txt'))
    $null = & $miniPrepare
    Assert-ReleaseTest ([IO.File]::ReadAllText((Join-Path $miniDeploy 'gate-runs.txt')) -ceq $gateRuns) 'Reusing a frozen release must not rebuild or repackage.'
    $global:WinmuxReleaseTest.Commit = '2222222222222222222222222222222222222222'
    Expect-ReleaseFailure { & $miniPrepare } 'already frozen with different source'

    Reset-ReleaseMock
    Expect-ReleaseFailure { & $publishScript -Manifest $manifest -Publish } 'finish the release draft'
    Assert-ReleaseTest ($global:WinmuxReleaseTest.Mutations.Count -eq 0) 'Publish must not create a missing draft.'

    $null = & $publishScript -Manifest $manifest
    Assert-ReleaseTest ($global:WinmuxReleaseTest.Release.draft -and
        $global:WinmuxReleaseTest.Release.tag_name -ceq "v$version" -and
        $global:WinmuxReleaseTest.Release.assets.Count -eq 3 -and
        $global:WinmuxReleaseTest.RemoteTag -eq $script:fixture.sourceCommit) 'A draft must contain the exact source and three assets.'
    $mutations = $global:WinmuxReleaseTest.Mutations.Count
    $null = & $publishScript -Manifest $manifest
    Assert-ReleaseTest ($global:WinmuxReleaseTest.Mutations.Count -eq $mutations) 'Retrying a complete draft must not upload or tag again.'
    $global:WinmuxReleaseTest.RemoteTag = '2222222222222222222222222222222222222222'
    Expect-ReleaseFailure { & $publishScript -Manifest $manifest } 'Remote tag.*different commit'
    $global:WinmuxReleaseTest.RemoteTag = $script:fixture.sourceCommit
    $cloudZip = Join-Path $global:WinmuxReleaseTest.Cloud $packageName
    $remoteBytes = [IO.File]::ReadAllBytes($cloudZip)
    $remoteBytes[$remoteBytes.Length - 1] = $remoteBytes[$remoteBytes.Length - 1] -bxor 1
    [IO.File]::WriteAllBytes($cloudZip, $remoteBytes)
    Expect-ReleaseFailure { & $publishScript -Manifest $manifest -Publish } 'conflicting SHA256'
    Assert-ReleaseTest ($global:WinmuxReleaseTest.Release.draft) 'A corrupt remote attachment must keep the release as a draft.'
    Copy-Item -LiteralPath $package -Destination $cloudZip -Force
    $null = & $publishScript -Manifest $manifest -Publish
    Assert-ReleaseTest (-not $global:WinmuxReleaseTest.Release.draft -and $global:WinmuxReleaseTest.Latest) 'A verified stable release should be published as Latest.'
    $mutations = $global:WinmuxReleaseTest.Mutations.Count
    $null = & $publishScript -Manifest $manifest -Publish
    Assert-ReleaseTest ($global:WinmuxReleaseTest.Mutations.Count -eq $mutations) 'Retrying a published release must not change it.'

    Reset-ReleaseMock
    $global:WinmuxReleaseTest.FailUpload = 'release.json'
    Expect-ReleaseFailure { & $publishScript -Manifest $manifest } 'simulated upload interruption'
    Assert-ReleaseTest ($global:WinmuxReleaseTest.Release.assets.Count -eq 1) 'The interrupted upload should leave a resumable partial draft.'
    $null = & $publishScript -Manifest $manifest
    Assert-ReleaseTest (@($global:WinmuxReleaseTest.Mutations | Where-Object { $_ -eq "upload:$packageName" }).Count -eq 1) 'Resuming must retain the already verified ZIP.'
    $script:fixture.prerelease = $true
    Save-ReleaseFixture
    Reset-ReleaseMock
    $null = & $publishScript -Manifest $manifest
    $null = & $publishScript -Manifest $manifest -Publish
    Assert-ReleaseTest ($global:WinmuxReleaseTest.Release.prerelease -and -not $global:WinmuxReleaseTest.Latest) 'A prerelease must not become Latest.'
    Write-Host "PASS: $assertions offline GitHub release assertions"
}
finally
{
    Remove-Item Function:\git, Function:\gh -ErrorAction SilentlyContinue
    Remove-Variable -Name WinmuxReleaseTest -Scope Global -ErrorAction SilentlyContinue
    Remove-ReleaseDirectory $testRoot ([IO.Path]::GetTempPath())
}
