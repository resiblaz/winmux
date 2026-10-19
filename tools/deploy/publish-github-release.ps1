#Requires -Version 7
[CmdletBinding(SupportsShouldProcess)]
param(
    [Parameter(Mandatory)][string]$Manifest,
    [switch]$Publish
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'GitHubRelease.ps1')

$repoRoot = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '../..'))
$bundle = Get-WinmuxReleaseBundle $Manifest
$data = $bundle.Data
$null = Assert-WinmuxReleaseSource $repoRoot $data.sourceCommit
if ((Get-WinmuxSourceVersion $repoRoot) -cne $data.version) { throw 'The committed source version differs from the release.' }
$operation = if ($Publish) { 'Verify downloaded assets and publish the existing draft' } else { 'Create or resume the GitHub release draft' }
if (-not $PSCmdlet.ShouldProcess("github.com/$($data.repository) $($data.tag) ($($data.sourceCommit))", $operation))
{
    return
}

if (-not (Get-Command gh -ErrorAction SilentlyContinue))
{
    throw 'GitHub CLI (gh) is required. Install it and run gh auth login --hostname github.com.'
}
$repository = Invoke-WinmuxGitHubApi "repos/$($data.repository)"
if ($repository.full_name -cne $data.repository -or -not $repository.permissions.push)
{
    throw "The authenticated GitHub account needs write access to $($data.repository)."
}
$comparison = Invoke-WinmuxGitHubApi "repos/$($data.repository)/compare/$($data.sourceCommit)...develop"
if ($comparison.status -cnotin @('ahead', 'identical'))
{
    throw 'The release commit must already be pushed to the GitHub develop branch.'
}

$localTag = Invoke-ReleaseCommand git @('-C', $repoRoot, 'show-ref', '--verify', '--quiet', "refs/tags/$($data.tag)") -AllowFailure
if ($localTag.ExitCode -eq 0)
{
    $target = (Invoke-ReleaseCommand git @('-C', $repoRoot, 'rev-parse', "refs/tags/$($data.tag)^{commit}")).Trim()
    if ($target -cne $data.sourceCommit) { throw 'The existing local release tag points to a different commit.' }
}
elseif ($localTag.ExitCode -ne 1) { throw "Could not inspect the local tag: $($localTag.Output)" }
$remoteTagExists = Assert-WinmuxRemoteTag $repoRoot $data.tag $data.sourceCommit
$release = Get-WinmuxGitHubRelease $data.repository $data.tag

if ($Publish -and ($null -eq $release -or -not $remoteTagExists))
{
    throw 'Create and finish the release draft and its remote tag before using -Publish.'
}
if ($null -ne $release)
{
    Assert-WinmuxRemoteRelease $release $bundle
    if (-not $release.draft)
    {
        if (-not $remoteTagExists) { throw 'The published release is missing its remote tag.' }
        if (@($release.assets).Count -ne $bundle.Assets.Count) { throw 'Published release attachments are incomplete.' }
        Test-WinmuxRemoteAssets $release $bundle
        Write-Host "The identical release is already published: $($release.html_url)"
        return [pscustomobject]@{ Url = $release.html_url; Draft = $false; Tag = $data.tag }
    }
    if (@($release.assets).Count -lt $bundle.Assets.Count)
    {
        Test-WinmuxRemoteAssets $release $bundle
    }
}

if (-not $remoteTagExists)
{
    if ($localTag.ExitCode -ne 0)
    {
        $null = Invoke-ReleaseCommand git @('-C', $repoRoot, 'tag', '-a', $data.tag, $data.sourceCommit,
            '-m', "Winmux $($data.version) ($($data.sourceCommit))")
    }
    $null = Invoke-ReleaseCommand git @('-C', $repoRoot, 'push', 'origin', "refs/tags/$($data.tag):refs/tags/$($data.tag)")
    if (-not (Assert-WinmuxRemoteTag $repoRoot $data.tag $data.sourceCommit)) { throw 'The pushed release tag is missing.' }
}

if ($null -eq $release)
{
    $arguments = @('release', 'create', $data.tag, '--repo', "github.com/$($data.repository)",
        '--draft', '--verify-tag', '--latest=false', '--title', "Winmux $($data.version)", '--notes-file', $bundle.Notes)
    if ($data.prerelease) { $arguments += '--prerelease' }
    $null = Invoke-ReleaseCommand gh $arguments
    $release = Get-WinmuxGitHubRelease $data.repository $data.tag
    if ($null -eq $release) { throw 'GitHub did not return the newly created release draft.' }
    Assert-WinmuxRemoteRelease $release $bundle
}

if ($Publish -and @($release.assets).Count -ne $bundle.Assets.Count)
{
    throw 'Release attachments are incomplete. Resume the draft upload before using -Publish.'
}
if (-not $Publish)
{
    foreach ($asset in $bundle.Assets)
    {
        $name = [IO.Path]::GetFileName($asset)
        if (@($release.assets | Where-Object { $_.name -ceq $name }).Count -eq 0)
        {
            # No --clobber: a conflicting attachment always stops the release.
            $null = Invoke-ReleaseCommand gh @('release', 'upload', $data.tag, $asset, '--repo', "github.com/$($data.repository)")
        }
    }
}

$release = Invoke-WinmuxGitHubApi "repos/$($data.repository)/releases/$($release.id)"
if (@($release.assets).Count -ne $bundle.Assets.Count) { throw 'Release attachments are incomplete.' }
Test-WinmuxRemoteAssets $release $bundle -Smoke
$null = Get-WinmuxReleaseBundle $Manifest
$null = Assert-WinmuxReleaseSource $repoRoot $data.sourceCommit
if (-not (Assert-WinmuxRemoteTag $repoRoot $data.tag $data.sourceCommit)) { throw 'The remote release tag disappeared.' }
if ($Publish)
{
    $latest = if ($data.prerelease) { '--latest=false' } else { '--latest' }
    $null = Invoke-ReleaseCommand gh @('release', 'edit', $data.tag, '--repo', "github.com/$($data.repository)", '--draft=false', $latest)
    $release = Invoke-WinmuxGitHubApi "repos/$($data.repository)/releases/$($release.id)"
    if ($release.draft) { throw 'GitHub still reports the release as a draft.' }
    Assert-WinmuxRemoteRelease $release $bundle
}
Write-Host "$(if ($release.draft) { 'Draft verified' } else { 'Release published' }): $($release.html_url)"
[pscustomobject]@{ Url = $release.html_url; Draft = [bool]$release.draft; Tag = $data.tag }
