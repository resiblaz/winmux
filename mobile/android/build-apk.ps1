[CmdletBinding()]
param(
    [switch]$Install
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

$projectRoot = $PSScriptRoot
$jdkCandidates = @(
    $env:JAVA_HOME,
    'F:\Environment\Android\jdk-21\jdk-21.0.12.1+1',
    'F:\Environment\Android\jdk-21\jdk-21.0.12+8'
) | Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'bin\javac.exe') -PathType Leaf) }
$javaHome = $jdkCandidates | Select-Object -First 1
if (-not $javaHome)
{
    throw 'JDK 17+ was not found. Set JAVA_HOME to a valid JDK 17/21 installation.'
}

$sdkCandidates = @(
    $env:ANDROID_HOME,
    $env:ANDROID_SDK_ROOT,
    (Join-Path $env:LOCALAPPDATA 'Android\Sdk'),
    'F:\Environment\Android\SDK'
) | Where-Object { $_ -and (Test-Path -LiteralPath (Join-Path $_ 'platforms\android-36') -PathType Container) }
$androidSdk = $sdkCandidates | Select-Object -First 1
if (-not $androidSdk)
{
    throw 'Android SDK with android-36 platform was not found. Set ANDROID_HOME.'
}

$env:JAVA_HOME = $javaHome
$env:ANDROID_HOME = $androidSdk
$env:ANDROID_SDK_ROOT = $androidSdk
$env:GRADLE_USER_HOME = if (Test-Path -LiteralPath 'F:\Environment\Android\.gradle') { 'F:\Environment\Android\.gradle' } else { Join-Path $env:USERPROFILE '.gradle' }

$gradleVersion = '8.11.1'
$gradleCacheRoot = Join-Path $env:GRADLE_USER_HOME "winmux-gradle-$gradleVersion"
$gradleBin = Join-Path $gradleCacheRoot "gradle-$gradleVersion\bin\gradle.bat"
if (-not (Test-Path -LiteralPath $gradleBin -PathType Leaf))
{
    New-Item -ItemType Directory -Path $gradleCacheRoot -Force | Out-Null
    $zipPath = Join-Path $gradleCacheRoot "gradle-$gradleVersion-bin.zip"
    Write-Host "Downloading Gradle $gradleVersion..." -ForegroundColor Cyan
    Invoke-WebRequest -Uri "https://services.gradle.org/distributions/gradle-$gradleVersion-bin.zip" -OutFile $zipPath
    Expand-Archive -LiteralPath $zipPath -DestinationPath $gradleCacheRoot -Force
    Remove-Item -LiteralPath $zipPath -Force
}

$escapedSdk = $androidSdk.Replace('\', '\\').Replace(':', '\:')
[System.IO.File]::WriteAllText((Join-Path $projectRoot 'local.properties'), "sdk.dir=$escapedSdk`n", [System.Text.UTF8Encoding]::new($false))

Push-Location $projectRoot
try
{
    Write-Host "Building Winmux Mobile APK (JAVA_HOME=$javaHome, ANDROID_HOME=$androidSdk)..." -ForegroundColor Cyan
    & $gradleBin :app:assembleDebug --no-daemon --max-workers=4
    if ($LASTEXITCODE -ne 0)
    {
        throw "Gradle build failed with exit code $LASTEXITCODE"
    }

    $apk = Join-Path $projectRoot 'app\build\outputs\apk\debug\app-debug.apk'
    if (-not (Test-Path -LiteralPath $apk -PathType Leaf))
    {
        throw "Expected APK was not produced: $apk"
    }
    Write-Host "Built APK: $apk" -ForegroundColor Green

    if ($Install)
    {
        $adb = Join-Path $androidSdk 'platform-tools\adb.exe'
        & $adb install -r $apk
        if ($LASTEXITCODE -ne 0)
        {
            throw "adb install failed with exit code $LASTEXITCODE"
        }
        Write-Host 'Installed Winmux Mobile APK onto connected Android device.' -ForegroundColor Green
    }
}
finally
{
    Pop-Location
}
