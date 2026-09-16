# Builds the Phosphene Quest APK without Gradle:
#   CMake/NDK -> libphosquest.so, aapt2 link -> base.apk, add the native libs, zipalign, apksigner (debug key).
# Windows PowerShell 5.1.  powershell -File Quest\build_apk.ps1
param(
    [string]$Sdk = "C:\Android-Buildtools\sdk",
    [string]$NdkVersion = "27.2.12479018",
    [string]$BuildTools = "34.0.0",
    [string]$Platform = "android-34",
    [string]$Jdk = "C:\Android-Buildtools\jdk17",
    [string]$Config = "Release"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$quest = Join-Path $root "Quest"
$ndk = Join-Path $Sdk "ndk\$NdkVersion"
$bt = Join-Path $Sdk "build-tools\$BuildTools"
$androidJar = Join-Path $Sdk "platforms\$Platform\android.jar"
$build = Join-Path $root "build-quest"
$out = Join-Path $build "apk"

if (-not (Test-Path (Join-Path $root "ThirdParty\openxr-loader\prefab"))) { throw "ThirdParty missing: run Quest\fetch_thirdparty.ps1 first" }
$env:JAVA_HOME = $Jdk                                  # apksigner.bat looks for java here
$env:Path = (Join-Path $Jdk "bin") + ";" + $env:Path

# 1. the native library. No ninja is installed with this SDK, so the NDK's own make is used.
& cmake -S $quest -B $build -G "Unix Makefiles" `
    -DCMAKE_MAKE_PROGRAM="$ndk\prebuilt\windows-x86_64\bin\make.exe" `
    -DCMAKE_TOOLCHAIN_FILE="$ndk\build\cmake\android.toolchain.cmake" `
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-29 -DCMAKE_BUILD_TYPE=$Config
if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
& cmake --build $build -j 8
if ($LASTEXITCODE -ne 0) { throw "native build failed" }

# 2. staging: the native libraries
$libDir = Join-Path $out "lib\arm64-v8a"
New-Item -ItemType Directory -Force $libDir | Out-Null
Copy-Item (Join-Path $build "libphosquest.so") $libDir -Force
Copy-Item (Join-Path $root "ThirdParty\openxr-loader\prefab\modules\openxr_loader\libs\android.arm64-v8a\libopenxr_loader.so") $libDir -Force

# 3. assets: the three files the core opens by bare name -- the wavetable library (750 KB) and the
#    two learned models of Phase 8 (1.5 MB each). They are staged rather than kept under Quest\ so
#    that the APK, the plugin and the self test all ship the one copy in Core\data. The app unpacks
#    them into its private directory on the first start, because the core opens its resources by
#    name and an asset in the APK has none (Quest\src\main.cpp, prepareAsset / prepareModels).
#
#    The two models cost 3 MB of APK and 3 MB of the headset's internal storage. Without them the
#    Quest build loses the whole of Phase 8 -- and it is the one surface with no stderr for anybody
#    to read, so nothing at all would say so.
$assetFiles = @("library.phoswt", "melody.phosmdl", "bass.phosmdl")
$assets = Join-Path $out "assets"
if (Test-Path $assets) { Remove-Item -Recurse -Force $assets }
New-Item -ItemType Directory -Force $assets | Out-Null
foreach ($f in $assetFiles) { Copy-Item (Join-Path $root "Core\data\$f") $assets -Force }

# 4. resources (the launcher icon at five densities) -> compiled, then the manifest -> base.apk (no code)
$resZip = Join-Path $out "res.zip"
if (Test-Path $resZip) { Remove-Item $resZip -Force }
& (Join-Path $bt "aapt2.exe") compile --dir (Join-Path $quest "res") -o $resZip
if ($LASTEXITCODE -ne 0) { throw "aapt2 compile failed" }
$base = Join-Path $out "base.apk"
if (Test-Path $base) { Remove-Item $base -Force }
& (Join-Path $bt "aapt2.exe") link -o $base --manifest (Join-Path $quest "AndroidManifest.xml") -R $resZip -A $assets -I $androidJar --min-sdk-version 29 --target-sdk-version 32
if ($LASTEXITCODE -ne 0) { throw "aapt2 link failed" }

# 5. add the libraries (jar keeps the zip valid; extractNativeLibs=true allows compressed .so)
Push-Location $out
& (Join-Path $Jdk "bin\jar.exe") uf $base lib\arm64-v8a\libphosquest.so lib\arm64-v8a\libopenxr_loader.so
Pop-Location
if ($LASTEXITCODE -ne 0) { throw "jar failed" }

# 6. align and sign with a debug key (created once)
$aligned = Join-Path $out "aligned.apk"
& (Join-Path $bt "zipalign.exe") -f 4 $base $aligned
if ($LASTEXITCODE -ne 0) { throw "zipalign failed" }
$keystore = Join-Path $root "ThirdParty\debug.keystore"
if (-not (Test-Path $keystore)) {
    & (Join-Path $Jdk "bin\keytool.exe") -genkeypair -keystore $keystore -alias androiddebugkey -keyalg RSA -keysize 2048 -validity 10000 `
        -storepass android -keypass android -dname "CN=Android Debug,O=Android,C=US"
    if ($LASTEXITCODE -ne 0) { throw "keytool failed" }
}
$final = Join-Path $build "PhospheneQuest.apk"
& (Join-Path $bt "apksigner.bat") sign --ks $keystore --ks-pass pass:android --key-pass pass:android --out $final $aligned
if ($LASTEXITCODE -ne 0) { throw "apksigner failed" }

# 7. what the finished APK carries. All three files have to be *in* it, not only in the staging
#    folder: without the pack the app finds no library on the device and falls back to the six
#    built-in tables, and without the two models the composer falls back to the Markov model and the
#    pattern families -- audible, not fatal, and therefore exactly the kind of failure that ships
#    unnoticed. Nothing else can check it here, because no headset is attached to this machine.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$zip = [IO.Compression.ZipFile]::OpenRead($final)
$carried = @{}
foreach ($e in $zip.Entries) { if ($e.FullName.StartsWith("assets/")) { $carried[$e.FullName] = @($e.Length, $e.CompressedLength) } }
$zip.Dispose()
foreach ($f in $assetFiles) {
    $key = "assets/$f"
    $wantBytes = (Get-Item (Join-Path $root "Core\data\$f")).Length
    if (-not $carried.ContainsKey($key)) { throw "the APK carries no $key, so the device would fall back without saying so" }
    $packedBytes = $carried[$key][0]
    if ($packedBytes -ne $wantBytes) { throw "the APK carries $packedBytes bytes of $f, not the $wantBytes of Core\data" }
    Write-Host ("assets:   {0} {1:N0} bytes, {2:N0} in the APK" -f $f, $packedBytes, $carried[$key][1])
}
Write-Host ("APK size: {0:N0} bytes" -f (Get-Item $final).Length)
Write-Host "APK: $final"
Write-Host "install:  adb install -r `"$final`""
Write-Host "config:   adb push phos.cfg /sdcard/Android/data/com.reneweller.phosphene.quest/files/phos.cfg"
