Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PortableDeployment.ps1')

$tempRoot = [IO.Path]::GetFullPath([IO.Path]::GetTempPath()).TrimEnd('\') + '\'
$testRoot = [IO.Path]::GetFullPath((Join-Path $tempRoot ('winmux-deployment-tests-' + [guid]::NewGuid().ToString('N'))))
if (-not $testRoot.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Invalid test directory.' }
$null = New-Item -ItemType Directory -Path $testRoot
$target = Join-Path $testRoot 'winmux-2.0.0.0'

function Add-SettingsFixture([string]$Name)
{
    $directory = Join-Path $testRoot $Name
    $settings = Join-Path $directory 'settings'
    $null = New-Item -ItemType Directory -Path $settings -Force
    [IO.File]::WriteAllText((Join-Path $settings 'settings.json'), '{}')
    return $directory
}

try
{
    if (Get-PortableSettingsSource $testRoot $target) { throw 'An empty deployment should not have a settings source.' }
    $firstLegacy = Add-SettingsFixture 'terminal-0.0.1.0'
    if ((Get-PortableSettingsSource $testRoot $target) -ne $firstLegacy) { throw 'Legacy portable settings were not selected.' }
    $latestLegacy = Add-SettingsFixture 'terminal-1.0.0.0'
    if ((Get-PortableSettingsSource $testRoot $target) -ne $latestLegacy) { throw 'The newest legacy settings were not selected.' }
    $null = Add-SettingsFixture '.winmux-2.0.0.0.backup-test'
    $null = Add-SettingsFixture 'unrelated'
    $null = Add-SettingsFixture 'validation\winmux-9.0.0.0'
    if ((Get-PortableSettingsSource $testRoot $target) -ne $latestLegacy) { throw 'Backup, test, or nested settings were selected.' }
    $priorWinmux = Add-SettingsFixture 'winmux-0.1.0.0'
    if ((Get-PortableSettingsSource $testRoot $target) -ne $priorWinmux) { throw 'Existing Winmux preferences must take precedence over legacy settings.' }
    $null = Add-SettingsFixture 'winmux-2.0.0.0'
    $null = Add-SettingsFixture 'winmux-3.0.0.0'
    if ((Get-PortableSettingsSource $testRoot $target) -ne $target) { throw 'Updating an installation must preserve its own settings.' }
    Write-Host 'PASS: 6 portable deployment migration assertions'
}
finally
{
    $resolved = [IO.Path]::GetFullPath($testRoot)
    if (-not $resolved.StartsWith($tempRoot, [StringComparison]::OrdinalIgnoreCase)) { throw 'Refusing to clean an unexpected test path.' }
    Remove-Item -LiteralPath $resolved -Recurse -Force
}
