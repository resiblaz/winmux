Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'PortableSettings.ps1')

$fixtures = @(
    @{} | ConvertTo-Json -Compress
    '{"profiles":{}}'
    '{"profiles":{"defaults":{}}}'
    '{"profiles":{"defaults":{"padding":"3"},"list":[{"name":"raw","experimental.vimMode":false}]}}'
    "// Keep { this comment }`r`n{`r`n  `"profiles`": {`r`n    `"defaults`": { /* keep defaults */ `"font`": {`"size`": 15}, },`r`n  },`r`n}"
    '{"profiles":[/* legacy profile */ {"name":"legacy","experimental.vimMode":false}]}'
)
$assertions = 0
foreach ($fixture in $fixtures)
{
    $updated = Enable-PortableVimModeDefault -Content $fixture
    $settings = $updated | ConvertFrom-Json -AsHashtable
    if ($settings['profiles']['defaults']['experimental.vimMode'] -ne $true)
    {
        throw 'Missing default Vim mode after migration.'
    }
    $assertions++
    if ((Enable-PortableVimModeDefault -Content $updated) -cne $updated)
    {
        throw 'Portable migration is not idempotent.'
    }
    $assertions++
    if ($fixture.Contains('keep defaults') -and (-not $updated.Contains('keep defaults') -or $settings.profiles.defaults.font.size -ne 15))
    {
        throw 'Portable migration changed comments or existing profile values.'
    }
    if ($fixture.Contains('"experimental.vimMode":false') -and $settings.profiles.list[0]['experimental.vimMode'] -ne $false)
    {
        throw 'A profile opt-out was overwritten.'
    }
    $assertions++
}
foreach ($value in @('false', 'true'))
{
    $fixture = '{"profiles":{"defaults":{"experimental.vimMode":' + $value + '}}}'
    if ((Enable-PortableVimModeDefault -Content $fixture) -cne $fixture)
    {
        throw 'An explicit default preference was rewritten.'
    }
    $assertions++
}
Write-Host "PASS: $assertions portable settings assertions"
