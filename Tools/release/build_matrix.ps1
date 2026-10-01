# Phosphene -- build every target this repository claims to have, and say which ones came out.
#
#   powershell -File Tools\release\build_matrix.ps1 [-Only desktop-plugin,android] [-Jobs 2] [-Clean]
#
# WHY THIS EXISTS, AND WHY IT IS NOT A CI WORKFLOW
#
# A repository has exactly as many working configurations as somebody builds. Phosphene has six that
# matter and an everyday `cmake --build build` touches one of them, which is how Quest/src/main.cpp
# spent a phase and a half not compiling and how the Android configure broke without a sound.
#
# The obvious answer is GitHub Actions. It was considered and rejected for this repository, for
# reasons that are about this repository and not about CI:
#
#   * it is private, so every minute is billed, and the standing rule here is that CI which costs
#     minutes stays off unless it is asked for;
#   * the Android half needs NDK r27 and the Quest half needs the OpenXR loader and Oboe, which on
#     this machine are junctions into a sibling checkout (Quest/fetch_thirdparty.ps1) and on a
#     runner would be a 130 MB download per job;
#   * the JUCE plugin needs a 500 MB checkout of JUCE, which is local here and would be fetched
#     there;
#   * and the one thing a hosted runner is genuinely good at -- catching what only breaks on a clean
#     machine -- is covered by the package check's runtime-dependency test instead.
#
# So: a local script, run before a release (Deploy\build_release.ps1 is the caller that matters) or
# whenever something in Core/ changes shape. The cheap half of it is already in ctest as the test
# "questguard" (Tools\release\quest_guard.cmake, about five seconds); this is the expensive half,
# which actually links everything.
#
# WHAT IS IN THE MATRIX AND WHY
#
#   desktop-plugin   the everyday build. Plugin on, AVX2 on, tests on.
#   desktop-tools    -DPHOS_BUILD_PLUGIN=OFF. The core and the tools have to build without JUCE
#                    anywhere near them -- that separation is the point of Core/ being framework
#                    free, and it is only a claim until something builds it that way.
#   desktop-noavx2   -DPHOS_AVX2=OFF. The scalar-lane build. Nothing else ever compiles Vec.h
#                    without AVX2 on x86, so an intrinsic that crept outside its #if lives here.
#   desktop-static   -DPHOS_STATIC_RUNTIME=ON, which is what ships. A static runtime is fine until
#                    it is not, and finding out during a release is finding out late.
#   android          the ROOT project through the NDK toolchain, with default options: Core,
#                    phos_render and the tests cross-compiled for arm64. Defect (2) of 16.09.2026.
#   quest            the Quest app itself (Quest/CMakeLists.txt): libphosquest.so, linked. Defect
#                    (1) of 16.09.2026 -- and unlike the ctest guard, this one also catches a
#                    missing symbol rather than only a syntax error.
#
# After the desktop entries the three vector builds are run (phos_vectest, _neon, _scalar), because
# a build that produces them and a build whose lanes actually equal the scalar reference bit for bit
# are two different claims.

param(
    # Which entries to run; everything by default. Names as in the table above.
    [string[]]$Only = @(),
    # Parallel compiler jobs. Two by default: three other things may be running on this machine, and
    # a matrix build that makes it unusable is a matrix build nobody starts.
    [int]$Jobs = 2,
    # Throw the build directories away first. Off by default -- an incremental matrix is the one
    # people will actually run twice.
    [switch]$Clean,
    [string]$Ndk = "",
    [string]$Generator = "Visual Studio 18 2026"
)
$ErrorActionPreference = "Continue"    # one entry failing must not stop the others: the table is the point
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
$work = Join-Path $root "build\matrix"

# The NDK, the same way Tools\release\quest_guard.cmake finds it.
if (-not $Ndk) {
    foreach ($v in @($env:ANDROID_NDK_HOME, $env:ANDROID_NDK_ROOT, $env:ANDROID_NDK)) {
        if ($v -and (Test-Path (Join-Path $v "build\cmake\android.toolchain.cmake"))) { $Ndk = $v; break }
    }
}
if (-not $Ndk) {
    $Ndk = Get-ChildItem "C:\Android-Buildtools\sdk\ndk\*" -Directory -ErrorAction SilentlyContinue |
           Where-Object { Test-Path (Join-Path $_.FullName "build\cmake\android.toolchain.cmake") } |
           Sort-Object Name -Descending | Select-Object -First 1 -ExpandProperty FullName
}
# No ninja ships with this SDK; the NDK carries its own make.
$ndkMake = if ($Ndk) { Join-Path $Ndk "prebuilt\windows-x86_64\bin\make.exe" } else { "" }
$juce = Join-Path $root "ThirdParty\JUCE"

# Each entry: a name, the source directory, the arguments, and what makes it worth building.
$matrix = @(
    @{ Name = "desktop-plugin"; Src = $root; Gen = $Generator; Arch = "x64"; Cfg = "Release"
       Args = @("-DPHOS_BUILD_PLUGIN=ON", "-DPHOS_BUILD_TOOLS=ON") }
    @{ Name = "desktop-tools";  Src = $root; Gen = $Generator; Arch = "x64"; Cfg = "Release"
       Args = @("-DPHOS_BUILD_PLUGIN=OFF", "-DPHOS_BUILD_TOOLS=ON") }
    @{ Name = "desktop-noavx2"; Src = $root; Gen = $Generator; Arch = "x64"; Cfg = "Release"
       Args = @("-DPHOS_BUILD_PLUGIN=OFF", "-DPHOS_AVX2=OFF") }
    @{ Name = "desktop-static"; Src = $root; Gen = $Generator; Arch = "x64"; Cfg = "Release"
       Args = @("-DPHOS_BUILD_PLUGIN=OFF", "-DPHOS_STATIC_RUNTIME=ON") }
    @{ Name = "android";        Src = $root; Gen = "Unix Makefiles"; Cfg = "Release"; NeedsNdk = $true
       # No -DPHOS_BUILD_PLUGIN here on purpose: the DEFAULT is what regressed, so the default is
       # what is built.
       Args = @() }
    @{ Name = "quest";          Src = (Join-Path $root "Quest"); Gen = "Unix Makefiles"; Cfg = "Release"; NeedsNdk = $true
       Args = @() }
)
if ($Only.Count -gt 0) { $matrix = $matrix | Where-Object { $Only -contains $_.Name } }
if (-not $matrix) { throw "no matrix entries selected" }

Write-Host "Phosphene build matrix -- $($matrix.Count) entries, -j $Jobs" -ForegroundColor Cyan
if ($Ndk) { Write-Host "  NDK: $Ndk" } else { Write-Host "  NDK: none found; the Android entries will be skipped" -ForegroundColor Yellow }
if (Test-Path (Join-Path $juce "CMakeLists.txt")) { Write-Host "  JUCE: $juce" } else { Write-Host "  JUCE: will be fetched from GitHub" -ForegroundColor Yellow }

$results = New-Object System.Collections.Generic.List[object]
foreach ($e in $matrix) {
    $dir = Join-Path $work $e.Name
    if ($Clean -and (Test-Path $dir)) { Remove-Item -Recurse -Force $dir }
    Write-Host ""
    Write-Host "== $($e.Name)" -ForegroundColor Cyan

    if ($e.NeedsNdk -and -not $Ndk) {
        $results.Add([PSCustomObject]@{ Entry = $e.Name; Configure = "-"; Build = "-"; Result = "SKIPPED (no NDK)" })
        Write-Host "  skipped: no Android NDK" -ForegroundColor Yellow
        continue
    }
    # The Quest app cannot even be configured without the OpenXR loader and Oboe.
    if ($e.Name -eq "quest" -and -not (Test-Path (Join-Path $root "ThirdParty\openxr-loader\prefab"))) {
        $results.Add([PSCustomObject]@{ Entry = $e.Name; Configure = "-"; Build = "-"; Result = "SKIPPED (run Quest\fetch_thirdparty.ps1)" })
        Write-Host "  skipped: ThirdParty missing" -ForegroundColor Yellow
        continue
    }

    $cfgArgs = @("-S", $e.Src, "-B", $dir, "-G", $e.Gen)
    if ($e.Arch) { $cfgArgs += @("-A", $e.Arch) }
    if ($e.NeedsNdk) {
        $cfgArgs += @("-DCMAKE_MAKE_PROGRAM=$ndkMake",
                      "-DCMAKE_TOOLCHAIN_FILE=$Ndk\build\cmake\android.toolchain.cmake",
                      "-DANDROID_ABI=arm64-v8a", "-DANDROID_PLATFORM=android-29",
                      "-DCMAKE_BUILD_TYPE=$($e.Cfg)", "-Wno-deprecated")
    } elseif (Test-Path (Join-Path $juce "CMakeLists.txt")) {
        $cfgArgs += "-DFETCHCONTENT_SOURCE_DIR_JUCE=$juce"
    }
    $cfgArgs += $e.Args

    $sw = [Diagnostics.Stopwatch]::StartNew()
    & cmake @cfgArgs 2>&1 | Out-Host
    $cfgRc = $LASTEXITCODE
    $sw.Stop(); $cfgSec = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    if ($cfgRc -ne 0) {
        $results.Add([PSCustomObject]@{ Entry = $e.Name; Configure = $cfgSec; Build = "-"; Result = "CONFIGURE FAILED" })
        Write-Host "  configure failed" -ForegroundColor Red
        continue
    }

    $buildArgs = @("--build", $dir, "--parallel", $Jobs)
    # Only a multi-configuration generator takes --config; the makefile ones got CMAKE_BUILD_TYPE
    # at configure time and reject it.
    if ($e.Arch) { $buildArgs += @("--config", $e.Cfg) }
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & cmake @buildArgs 2>&1 | Out-Host
    $buildRc = $LASTEXITCODE
    $sw.Stop(); $buildSec = [math]::Round($sw.Elapsed.TotalSeconds, 1)
    $results.Add([PSCustomObject]@{
        Entry = $e.Name; Configure = $cfgSec; Build = $buildSec
        Result = $(if ($buildRc -eq 0) { "ok" } else { "BUILD FAILED" })
    })
    if ($buildRc -eq 0) { Write-Host "  ok" -ForegroundColor Green } else { Write-Host "  build failed" -ForegroundColor Red }
}

# ---------------------------------------------------------------- the three vector paths
# Building them is one claim; that every lane equals the scalar computation bit for bit is another,
# and the second is the one the Quest depends on. Run from the desktop-plugin tree when it is there.
$vec = Join-Path $work "desktop-plugin\Tests\Release"
if (-not (Test-Path $vec)) { $vec = Join-Path $work "desktop-tools\Tests\Release" }
$vecRows = New-Object System.Collections.Generic.List[object]
foreach ($v in @("phos_vectest", "phos_vectest_neon", "phos_vectest_scalar")) {
    $p = Join-Path $vec "$v.exe"
    if (-not (Test-Path $p)) {
        $vecRows.Add([PSCustomObject]@{ Variant = $v; Result = "not built" })
        continue
    }
    $out = & $p 2>&1
    $rc = $LASTEXITCODE
    # The binary names the path it was compiled for (PHOS_EXPECT_PATH in Tests/CMakeLists.txt); the
    # line is repeated here so the table shows three different paths and not three copies of one.
    $path = ($out | Select-String -Pattern "(avx2|neon-shim|neon|scalar)" | Select-Object -First 1)
    $vecRows.Add([PSCustomObject]@{
        Variant = $v
        Path    = $(if ($path) { $path.Matches[0].Value } else { "?" })
        Result  = $(if ($rc -eq 0) { "bit-identical to scalar" } else { "FAILED" })
    })
}

Write-Host ""
Write-Host "matrix" -ForegroundColor Cyan
$results | Format-Table Entry, @{ n = "cfg s"; e = { $_.Configure } }, @{ n = "build s"; e = { $_.Build } }, Result -AutoSize
Write-Host "vector paths" -ForegroundColor Cyan
$vecRows | Format-Table -AutoSize

$bad = @($results | Where-Object { $_.Result -like "*FAILED*" }).Count + @($vecRows | Where-Object { $_.Result -like "*FAILED*" }).Count
$skip = @($results | Where-Object { $_.Result -like "SKIPPED*" }).Count
if ($bad -gt 0) { Write-Host "$bad entr(y/ies) failed" -ForegroundColor Red; exit 1 }
if ($skip -gt 0) { Write-Host "$skip entr(y/ies) skipped -- those configurations are unverified" -ForegroundColor Yellow }
Write-Host "every built entry succeeded" -ForegroundColor Green
exit 0
