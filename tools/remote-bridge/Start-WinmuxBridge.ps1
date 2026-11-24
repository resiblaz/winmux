[CmdletBinding()]
param(
    [int]$Port = 47710,
    [string]$RemoteUrl = ''
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$node = (Get-Command node.exe -ErrorAction Stop).Source
$entry = Join-Path $PSScriptRoot 'bin\winmux-bridge.mjs'
$env:WINMUX_BRIDGE_PORT = "$Port"
if (-not [string]::IsNullOrWhiteSpace($RemoteUrl))
{
    $env:WINMUX_BRIDGE_REMOTE_URL = $RemoteUrl
}

& $node $entry
exit $LASTEXITCODE
