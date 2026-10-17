function Get-PortableSettingsSource
{
    param(
        [Parameter(Mandatory)][string]$OutputDirectory,
        [Parameter(Mandatory)][string]$DeploymentDirectory
    )

    if (Test-Path -LiteralPath (Join-Path $DeploymentDirectory 'settings\settings.json') -PathType Leaf)
    {
        return [System.IO.Path]::GetFullPath($DeploymentDirectory)
    }

    # Prefer an existing Winmux installation, then migrate the old portable
    # Terminal layout. Only direct, versioned children of this output root count.
    $candidates = foreach ($directory in Get-ChildItem -LiteralPath $OutputDirectory -Directory)
    {
        if (($directory.Attributes -band [System.IO.FileAttributes]::ReparsePoint) -ne 0)
        {
            continue
        }
        if ($directory.Name -match '^(winmux|terminal)-(\d+\.\d+\.\d+\.\d+)$' -and
            (Test-Path -LiteralPath (Join-Path $directory.FullName 'settings\settings.json') -PathType Leaf))
        {
            [pscustomobject]@{
                Path = $directory.FullName
                IsWinmux = $Matches[1] -eq 'winmux'
                Version = [version]$Matches[2]
            }
        }
    }
    $source = $candidates | Sort-Object -Property @(
        @{ Expression = 'IsWinmux'; Descending = $true },
        @{ Expression = 'Version'; Descending = $true }
    ) | Select-Object -First 1
    if ($source)
    {
        return $source.Path
    }
}
