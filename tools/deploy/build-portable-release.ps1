[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string]$Platform = 'x64',

    [switch]$SkipBuild,

    [switch]$NoLaunch,

    [string]$OutputDirectory
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PortableSettings.ps1')
. (Join-Path $PSScriptRoot 'PortableDeployment.ps1')

$repoRoot = [System.IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..\..'))
$configuration = 'Release'
$packageRoot = Join-Path $repoRoot 'src\cascadia\CascadiaPackage\AppPackages'
$localBuildScript = Join-Path $repoRoot 'tools\Deploy-LocalRelease.ps1'
$portableBuildScript = Join-Path $repoRoot 'build\scripts\New-UnpackagedTerminalDistribution.ps1'
$Verbose = $PSBoundParameters.ContainsKey('Verbose')

if ([string]::IsNullOrWhiteSpace($OutputDirectory))
{
    $outputRoot = Join-Path $repoRoot 'dist'
}
elseif ([System.IO.Path]::IsPathRooted($OutputDirectory))
{
    $outputRoot = [System.IO.Path]::GetFullPath($OutputDirectory)
}
else
{
    $outputRoot = [System.IO.Path]::GetFullPath((Join-Path $repoRoot $OutputDirectory))
}

function Assert-ChildPath
{
    param(
        [Parameter(Mandatory)]
        [string]$Path,

        [Parameter(Mandatory)]
        [string]$Parent
    )

    $fullPath = [System.IO.Path]::GetFullPath($Path)
    $fullParent = [System.IO.Path]::GetFullPath($Parent).TrimEnd('\') + '\'
    if (-not $fullPath.StartsWith($fullParent, [System.StringComparison]::OrdinalIgnoreCase))
    {
        throw "Path is outside the expected parent directory: $fullPath"
    }

    return $fullPath
}

function Set-WorkspaceSidebarSettings
{
    param(
        [Parameter(Mandatory)]
        [string]$SettingsPath
    )

    $content = [System.IO.File]::ReadAllText($SettingsPath)
    $tabLayoutPattern = '("tabLayout"\s*:\s*)"[^"]*"'
    $tabLayoutRegex = [regex]::new($tabLayoutPattern)
    if ($tabLayoutRegex.IsMatch($content))
    {
        $content = $tabLayoutRegex.Replace($content, '${1}"workspaceSidebar"', 1)
    }
    else
    {
        $openingBrace = $content.IndexOf('{')
        if ($openingBrace -lt 0)
        {
            throw "Settings file is not a JSON object: $SettingsPath"
        }

        $content = $content.Insert($openingBrace + 1, "`r`n    `"tabLayout`": `"workspaceSidebar`",")
    }

    # Enable restoration when upgrading older portable settings, while keeping
    # an explicit startup preference (including an opt-out) and JSON comments.
    $settings = $content | ConvertFrom-Json -AsHashtable
    if (-not $settings.ContainsKey('firstWindowPreference'))
    {
        $openingBrace = $content.IndexOf('{')
        $content = $content.Insert($openingBrace + 1, "`r`n    `"firstWindowPreference`": `"persistedLayout`",")
    }

    $content = Enable-PortableVimModeDefault -Content $content
    $content = Update-PortableProjectLinks -Content $content
    [System.IO.File]::WriteAllText($SettingsPath, $content, [System.Text.UTF8Encoding]::new($false))
}

function Stop-PortableTerminal
{
    param(
        [Parameter(Mandatory)]
        [string]$ExecutablePath
    )

    $normalizedExecutable = [System.IO.Path]::GetFullPath($ExecutablePath)
    $processes = @(Get-Process winmux, WindowsTerminal -ErrorAction SilentlyContinue | Where-Object {
        $_.Path -and
        [System.IO.Path]::GetFullPath($_.Path).Equals($normalizedExecutable, [System.StringComparison]::OrdinalIgnoreCase)
    })

    foreach ($process in $processes)
    {
        $null = $process.CloseMainWindow()
    }

    # Session state is flushed during normal shutdown. Never force termination
    # while a close confirmation or the final persistence write is pending.
    $deadline = [DateTime]::UtcNow.AddSeconds(20)
    foreach ($process in $processes)
    {
        $remainingMilliseconds = [Math]::Max(0, [int]($deadline - [DateTime]::UtcNow).TotalMilliseconds)
        if (-not $process.WaitForExit($remainingMilliseconds))
        {
            throw 'Portable Terminal is still running. Close it normally so its session can be saved, then retry the deployment.'
        }
    }
}

if (-not $SkipBuild)
{
    Write-Host "Building Winmux $Platform $configuration..." -ForegroundColor Cyan
    & $localBuildScript -Platform $Platform -BuildOnly
}

$terminalAppx = Get-ChildItem -LiteralPath $packageRoot -Recurse -File -Filter '*.msix' |
    Where-Object {
        $_.Directory.Name -like "*_${Platform}_Test" -and
        $_.Name -notlike '*_Debug.msix'
    } |
    Sort-Object LastWriteTime -Descending |
    Select-Object -First 1
if (-not $terminalAppx)
{
    throw "No $Platform $configuration Terminal MSIX was found under $packageRoot. Run without -SkipBuild."
}

$xamlDependencyDirectory = Join-Path $terminalAppx.Directory.FullName "Dependencies\$Platform"
$xamlAppx = Get-ChildItem -LiteralPath $xamlDependencyDirectory -File -Filter 'Microsoft.UI.Xaml*.appx' |
    Select-Object -First 1
if (-not $xamlAppx)
{
    throw "Microsoft.UI.Xaml dependency was not found under $xamlDependencyDirectory."
}

New-Item -ItemType Directory -Path $outputRoot -Force | Out-Null

$systemTempRoot = [System.IO.Path]::GetFullPath([System.IO.Path]::GetTempPath())
$workRoot = Join-Path $systemTempRoot ("windows-terminal-portable-{0}" -f [guid]::NewGuid().ToString('N'))
$workRoot = Assert-ChildPath -Path $workRoot -Parent $systemTempRoot
$officialOutput = Join-Path $workRoot 'official'
$extractedOutput = Join-Path $workRoot 'extracted'
$preservedSettings = Join-Path $workRoot 'preserved-settings'
$newDeployment = $null
$backupDeployment = $null
$temporaryZip = $null

try
{
    New-Item -ItemType Directory -Path $officialOutput, $extractedOutput -Force | Out-Null

    Write-Host 'Creating the official unpackaged portable layout...' -ForegroundColor Cyan
    $null = & $portableBuildScript `
        -TerminalAppX $terminalAppx.FullName `
        -XamlAppX $xamlAppx.FullName `
        -Destination $officialOutput `
        -PortableMode `
        -Verbose:$Verbose *>&1

    $officialZip = Get-ChildItem -LiteralPath $officialOutput -File -Filter '*.zip' |
        Sort-Object LastWriteTime -Descending |
        Select-Object -First 1
    if (-not $officialZip)
    {
        throw 'The official portable build did not produce a ZIP archive.'
    }

    Expand-Archive -LiteralPath $officialZip.FullName -DestinationPath $extractedOutput
    $terminalRoot = Get-ChildItem -LiteralPath $extractedOutput -Directory |
        Where-Object { Test-Path -LiteralPath (Join-Path $_.FullName 'winmux.exe') -PathType Leaf } |
        Select-Object -First 1
    if (-not $terminalRoot)
    {
        throw 'winmux.exe was not found in the extracted portable layout. Rebuild the Winmux package before using -SkipBuild.'
    }

    if ($terminalRoot.Name -notmatch '^terminal-(\d+\.\d+\.\d+\.\d+)$')
    {
        throw "Unexpected upstream portable directory name: $($terminalRoot.Name)"
    }
    $version = $Matches[1]
    $winmuxDirectoryName = "winmux-$version"
    $originalExtractedRoot = Assert-ChildPath -Path $terminalRoot.FullName -Parent $extractedOutput
    $winmuxExtractedRoot = Assert-ChildPath -Path (Join-Path $extractedOutput $winmuxDirectoryName) -Parent $extractedOutput
    Move-Item -LiteralPath $originalExtractedRoot -Destination $winmuxExtractedRoot
    $terminalRoot = Get-Item -LiteralPath $winmuxExtractedRoot

    $settingsDirectory = Join-Path $terminalRoot.FullName 'settings'
    $settingsPath = Join-Path $settingsDirectory 'settings.json'
    New-Item -ItemType Directory -Path $settingsDirectory -Force | Out-Null
    $defaultSettings = [ordered]@{
        '$schema' = 'https://raw.githubusercontent.com/resiblaz/winmux/develop/doc/cascadia/profiles.schema.json'
        '$help' = 'https://github.com/resiblaz/winmux/blob/develop/doc/user-docs/WorkspaceSidebar.md'
        tabLayout = 'workspaceSidebar'
        firstWindowPreference = 'persistedLayout'
        profiles = [ordered]@{
            defaults = [ordered]@{
                'experimental.vimMode' = $true
                padding = '3'
                scrollbarState = 'hidden'
            }
        }
    } | ConvertTo-Json -Depth 4
    [System.IO.File]::WriteAllText($settingsPath, $defaultSettings + "`r`n", [System.Text.UTF8Encoding]::new($false))

    $configuredZipName = "winmux_${version}_windows_${Platform}.zip"
    $configuredZip = Join-Path $outputRoot $configuredZipName
    $temporaryZip = Join-Path $outputRoot ('.{0}.{1}.tmp.zip' -f [System.IO.Path]::GetFileNameWithoutExtension($configuredZipName), [guid]::NewGuid().ToString('N'))
    $temporaryZip = Assert-ChildPath -Path $temporaryZip -Parent $outputRoot

    Write-Host 'Creating the Winmux portable ZIP...' -ForegroundColor Cyan
    & tar.exe -c --format=zip -f $temporaryZip -C $extractedOutput $terminalRoot.Name
    if ($LASTEXITCODE -ne 0)
    {
        throw "Creating the configured ZIP failed with exit code $LASTEXITCODE."
    }

    $archiveEntries = @(& tar.exe -tf $temporaryZip)
    foreach ($requiredEntry in @(
        "$($terminalRoot.Name)/.portable",
        "$($terminalRoot.Name)/settings/settings.json",
        "$($terminalRoot.Name)/winmux.exe",
        "$($terminalRoot.Name)/wmx.exe"
    ))
    {
        if ($archiveEntries -notcontains $requiredEntry)
        {
            throw "Configured ZIP validation failed; missing $requiredEntry."
        }
    }

    Move-Item -LiteralPath $temporaryZip -Destination $configuredZip -Force

    $deploymentPath = Join-Path $outputRoot $terminalRoot.Name
    $deploymentPath = Assert-ChildPath -Path $deploymentPath -Parent $outputRoot
    $deploymentExecutable = Join-Path $deploymentPath 'winmux.exe'
    $settingsSource = Get-PortableSettingsSource -OutputDirectory $outputRoot -DeploymentDirectory $deploymentPath

    if (Test-Path -LiteralPath $deploymentExecutable -PathType Leaf)
    {
        Stop-PortableTerminal -ExecutablePath $deploymentExecutable
    }

    if ($settingsSource)
    {
        foreach ($name in @('winmux.exe', 'WindowsTerminal.exe'))
        {
            $sourceExecutable = Join-Path $settingsSource $name
            if (Test-Path -LiteralPath $sourceExecutable -PathType Leaf)
            {
                Stop-PortableTerminal -ExecutablePath $sourceExecutable
            }
        }
        $existingSettingsDirectory = Join-Path $settingsSource 'settings'
        Write-Host "Preserving portable settings and session data from $existingSettingsDirectory" -ForegroundColor Cyan
        Copy-Item -LiteralPath $existingSettingsDirectory -Destination $preservedSettings -Recurse
    }

    $newDeployment = Assert-ChildPath `
        -Path (Join-Path $outputRoot ('.{0}.new-{1}' -f $terminalRoot.Name, [guid]::NewGuid().ToString('N'))) `
        -Parent $outputRoot
    Copy-Item -LiteralPath $terminalRoot.FullName -Destination $newDeployment -Recurse

    if (Test-Path -LiteralPath $preservedSettings -PathType Container)
    {
        $newSettingsDirectory = Join-Path $newDeployment 'settings'
        Get-ChildItem -LiteralPath $preservedSettings -Force |
            Copy-Item -Destination $newSettingsDirectory -Recurse -Force
        Set-WorkspaceSidebarSettings -SettingsPath (Join-Path $newSettingsDirectory 'settings.json')
    }

    if (Test-Path -LiteralPath $deploymentPath)
    {
        $backupDeployment = Assert-ChildPath `
            -Path (Join-Path $outputRoot ('.{0}.backup-{1}' -f $terminalRoot.Name, [guid]::NewGuid().ToString('N'))) `
            -Parent $outputRoot
        Move-Item -LiteralPath $deploymentPath -Destination $backupDeployment
    }

    try
    {
        Move-Item -LiteralPath $newDeployment -Destination $deploymentPath
        $newDeployment = $null
    }
    catch
    {
        if ($backupDeployment -and (Test-Path -LiteralPath $backupDeployment) -and -not (Test-Path -LiteralPath $deploymentPath))
        {
            Move-Item -LiteralPath $backupDeployment -Destination $deploymentPath
            $backupDeployment = $null
        }
        throw
    }

    if ($backupDeployment -and (Test-Path -LiteralPath $backupDeployment))
    {
        $backupDeployment = Assert-ChildPath -Path $backupDeployment -Parent $outputRoot
        Remove-Item -LiteralPath $backupDeployment -Recurse -Force
        $backupDeployment = $null
    }

    $deployedSettings = Get-Content -LiteralPath (Join-Path $deploymentPath 'settings\settings.json') -Raw
    if ($deployedSettings -notmatch '"tabLayout"\s*:\s*"workspaceSidebar"')
    {
        throw 'The deployed settings do not enable Workspace Sidebar.'
    }

    if (-not $NoLaunch)
    {
        Write-Host 'Launching Winmux...' -ForegroundColor Cyan
        # Explicit window arguments would create an extra window after restore.
        Start-Process -FilePath $deploymentExecutable -WorkingDirectory $deploymentPath

        $started = $false
        for ($attempt = 0; $attempt -lt 20; $attempt++)
        {
            Start-Sleep -Milliseconds 250
            $started = [bool](Get-Process winmux -ErrorAction SilentlyContinue |
                Where-Object {
                    $_.Path -and
                    [System.IO.Path]::GetFullPath($_.Path).Equals($deploymentExecutable, [System.StringComparison]::OrdinalIgnoreCase)
                } |
                Select-Object -First 1)
            if ($started)
            {
                break
            }
        }

        if (-not $started)
        {
            throw "The portable Terminal did not start from $deploymentExecutable."
        }
    }

    $zipHash = Get-FileHash -LiteralPath $configuredZip -Algorithm SHA256
    Write-Host 'Winmux portable Release completed.' -ForegroundColor Green
    [pscustomobject]@{
        Package = $configuredZip
        Deployment = $deploymentPath
        Launcher = $deploymentExecutable
        CommandLine = Join-Path $deploymentPath 'wmx.exe'
        SettingsMigratedFrom = $settingsSource
        SHA256 = $zipHash.Hash
    }
}
finally
{
    if ($temporaryZip -and (Test-Path -LiteralPath $temporaryZip))
    {
        $temporaryZip = Assert-ChildPath -Path $temporaryZip -Parent $outputRoot
        Remove-Item -LiteralPath $temporaryZip -Force
    }

    if ($newDeployment -and (Test-Path -LiteralPath $newDeployment))
    {
        $newDeployment = Assert-ChildPath -Path $newDeployment -Parent $outputRoot
        Remove-Item -LiteralPath $newDeployment -Recurse -Force
    }

    if (Test-Path -LiteralPath $workRoot)
    {
        $workRoot = Assert-ChildPath -Path $workRoot -Parent $systemTempRoot
        Remove-Item -LiteralPath $workRoot -Recurse -Force
    }
}
