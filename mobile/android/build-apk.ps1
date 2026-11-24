[CmdletBinding()]
param(
    [switch]$Release,
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

if ($Release)
{
    if (-not $env:WINMUX_KEYSTORE_PATH)
    {
        $signingDir = Join-Path $env:LOCALAPPDATA 'Winmux\android-signing'
        New-Item -ItemType Directory -Path $signingDir -Force | Out-Null
        $keystorePath = Join-Path $signingDir 'winmux-release.jks'
        $passwordFile = Join-Path $signingDir 'keystore-password.txt'
        if (-not (Test-Path -LiteralPath $keystorePath -PathType Leaf))
        {
            $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
            $bytes = New-Object byte[] 24
            $rng.GetBytes($bytes)
            $password = [Convert]::ToBase64String($bytes).Replace('+', 'A').Replace('/', 'B').Replace('=', '')
            [System.IO.File]::WriteAllText($passwordFile, $password, [System.Text.UTF8Encoding]::new($false))

            $keytool = Join-Path $javaHome 'bin\keytool.exe'
            Write-Host "Generating local Release signing keystore at $keystorePath..." -ForegroundColor Cyan
            & $keytool -genkeypair -v -storetype PKCS12 -keystore $keystorePath -alias winmux `
                -keyalg RSA -keysize 2048 -validity 10000 `
                -storepass $password -keypass $password `
                -dname 'CN=Winmux Mobile, OU=LocalBuild, O=Winmux, L=Local, S=Local, C=CN' | Out-Null
            if ($LASTEXITCODE -ne 0)
            {
                throw "keytool failed with exit code $LASTEXITCODE"
            }
        }
        $env:WINMUX_KEYSTORE_PATH = $keystorePath
        $env:WINMUX_KEY_ALIAS = 'winmux'
        $env:WINMUX_KEYSTORE_PASSWORD = (Get-Content -LiteralPath $passwordFile -Raw).Trim()
        $env:WINMUX_KEY_PASSWORD = $env:WINMUX_KEYSTORE_PASSWORD
    }
}

$gradleTask = if ($Release) { ':app:assembleRelease' } else { ':app:assembleDebug' }
$variantName = if ($Release) { 'Release' } else { 'Debug' }
$expectedApk = if ($Release) {
    Join-Path $projectRoot 'app\build\outputs\apk\release\app-release.apk'
} else {
    Join-Path $projectRoot 'app\build\outputs\apk\debug\app-debug.apk'
}

Push-Location $projectRoot
try
{
    Write-Host "Building Winmux Mobile $variantName APK (JAVA_HOME=$javaHome, ANDROID_HOME=$androidSdk)..." -ForegroundColor Cyan
    & $gradleBin $gradleTask --no-daemon --max-workers=4
    if ($LASTEXITCODE -ne 0)
    {
        throw "Gradle build failed with exit code $LASTEXITCODE"
    }

    if (-not (Test-Path -LiteralPath $expectedApk -PathType Leaf))
    {
        throw "Expected APK was not produced: $expectedApk"
    }
    Write-Host "Built $variantName APK: $expectedApk" -ForegroundColor Green

    if ($Install)
    {
        $adb = Join-Path $androidSdk 'platform-tools\adb.exe'
        & $adb install -r $expectedApk
        if ($LASTEXITCODE -ne 0)
        {
            throw "adb install failed with exit code $LASTEXITCODE"
        }
        Write-Host "Installed Winmux Mobile $variantName APK onto connected Android device." -ForegroundColor Green
    }
}
finally
{
    Pop-Location
}
