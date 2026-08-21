[CmdletBinding()]
param(
    [ValidateSet('x64')]
    [string]$Platform = 'x64',

    [switch]$SkipBuild,

    [switch]$NoLaunch
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$deploymentScript = Join-Path $PSScriptRoot 'tools\Deploy-LocalRelease.ps1'
& $deploymentScript @PSBoundParameters
