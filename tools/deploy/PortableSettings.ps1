function Get-JsonContainerRange
{
    param([string]$Content, [AllowEmptyString()][string]$JsonPath)

    $lineStarts = [System.Collections.Generic.List[int]]::new()
    $lineStarts.Add(0)
    foreach ($newline in [regex]::Matches($Content, '\r\n|\n|\r'))
    {
        $lineStarts.Add($newline.Index + $newline.Length)
    }
    $reader = [Newtonsoft.Json.JsonTextReader]::new([System.IO.StringReader]::new($Content))
    $reader.CloseInput = $true
    try
    {
        while ($reader.Read())
        {
            if ($reader.Path -ceq $JsonPath -and $reader.TokenType -in @('StartObject', 'StartArray'))
            {
                $start = $lineStarts[$reader.LineNumber - 1] + $reader.LinePosition - 1
                $depth = $reader.Depth
                $endToken = if ($reader.TokenType -eq 'StartObject') { 'EndObject' } else { 'EndArray' }
                while ($reader.Read())
                {
                    if ($reader.Depth -eq $depth -and $reader.TokenType -eq $endToken)
                    {
                        return [pscustomobject]@{
                            Start = $start
                            End = $lineStarts[$reader.LineNumber - 1] + $reader.LinePosition
                            Depth = $depth
                        }
                    }
                }
                break
            }
        }
        throw "JSON container not found: $JsonPath"
    }
    finally
    {
        $reader.Close()
    }
}

function Add-JsonObjectProperty
{
    param([string]$Content, [AllowEmptyString()][string]$JsonPath, [string]$Name, [string]$ValueJson, [int]$ExistingProperties)

    $range = Get-JsonContainerRange -Content $Content -JsonPath $JsonPath
    $newline = if ($Content.Contains("`r`n")) { "`r`n" } else { "`n" }
    $indent = ' ' * (4 * ($range.Depth + 1))
    $comma = if ($ExistingProperties -gt 0) { ',' } else { '' }
    $propertyName = ConvertTo-Json -InputObject $Name -Compress
    return $Content.Insert($range.Start + 1, "$newline$indent${propertyName}: $ValueJson$comma")
}

function Enable-PortableVimModeDefault
{
    param([Parameter(Mandatory)][string]$Content)

    $settings = $Content | ConvertFrom-Json -AsHashtable -ErrorAction Stop
    if ($settings -isnot [System.Collections.IDictionary])
    {
        throw 'Portable settings must be a JSON object.'
    }
    if (-not $settings.Contains('profiles'))
    {
        return Add-JsonObjectProperty $Content '' 'profiles' '{"defaults":{"experimental.vimMode":true}}' $settings.Count
    }
    $profiles = $settings['profiles']
    if ($profiles -is [System.Collections.IList])
    {
        # Preserve the complete legacy array, including comments and formatting.
        $range = Get-JsonContainerRange $Content 'profiles'
        $array = $Content.Substring($range.Start, $range.End - $range.Start)
        return $Content.Substring(0, $range.Start) + '{"defaults":{"experimental.vimMode":true},"list":' + $array + '}' + $Content.Substring($range.End)
    }
    if ($profiles -isnot [System.Collections.IDictionary])
    {
        throw 'Portable profiles must be an object or a legacy array.'
    }
    if (-not $profiles.Contains('defaults'))
    {
        return Add-JsonObjectProperty $Content 'profiles' 'defaults' '{"experimental.vimMode":true}' $profiles.Count
    }
    $defaults = $profiles['defaults']
    if ($defaults -isnot [System.Collections.IDictionary])
    {
        throw 'Portable profile defaults must be a JSON object.'
    }
    if ($defaults.Contains('experimental.vimMode'))
    {
        return $Content
    }
    return Add-JsonObjectProperty $Content 'profiles.defaults' 'experimental.vimMode' 'true' $defaults.Count
}
