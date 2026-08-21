[CmdletBinding()]
param()

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$launcher = Join-Path ([Environment]::GetFolderPath('LocalApplicationData')) 'Microsoft\WindowsApps\wtd.exe'
if (-not (Test-Path -LiteralPath $launcher -PathType Leaf))
{
    throw 'Local Release is not deployed. Run .\tools\deploy\deploy-local-release.ps1 first.'
}

Start-Process -FilePath $launcher -ArgumentList @('-w', 'new')
