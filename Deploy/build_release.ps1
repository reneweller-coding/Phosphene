# Phosphene -- from a checkout to the things other people get, in one run.
#
#   powershell -File Deploy\build_release.ps1 [-Version 1.0.0] [-Toolchain intel|msvc]
#                    [-SkipBuild] [-SkipTests] [-SkipManual] [-SkipQuest] [-NoSetup]
#                    [-UpdateDocs] [-SignWith <thumbprint|file.pfx>] [-SignPassword <pw>]
#
# It configures, builds, tests, prints the manual, builds the APK, stages, checks the staging
# directory, compiles the setup, zips a portable archive and prints a manifest with every size and
# hash. Any step that fails stops the run: there is no path through this script that produces an
# installer out of a build that did not pass its tests, and no -Force to make one.
#
# The -Skip switches exist for working on a later step without repeating an earlier one. They are
# not shortcuts for a real release: each one prints a line saying what has not been verified, and
# the manifest at the end says which of them was on.
#
# FIVE THINGS MAKE THIS BUILD DIFFERENT FROM AN EVERYDAY ONE
#
#   * PHOS_SHIP=ON takes this checkout's Core\data path out of the binaries (Core\CMakeLists.txt).
#     Everyday builds keep it as the last step of the core's lookup, which is convenient here and a
#     hazard everywhere else: it puts this machine's directory layout inside every downloaded binary,
#     and on this machine it makes a missing data file invisible -- a staged binary reads the source
#     tree whatever is staged beside it, so a package with a model deleted renders exactly like a
#     correct one. Set here and checked afterwards (check_package.ps1, check F2, which now fails
#     rather than warns). ctest below runs in this configuration, not in a different one.
#   * PHOS_STATIC_RUNTIME=ON links the MSVC runtime in, so nothing has to be installed first -- no
#     redistributable, no DLL beside the executable. Checked afterwards rather than assumed
#     (Tools\release\check_package.ps1, check E).
#   * PHOS_AVX2=ON. The setup asks the processor about AVX2 before installing, because a binary
#     built for it does not degrade on a machine without it -- it takes an illegal instruction and
#     dies with nothing on screen.
#   * It builds in a tree of its own (build-release-intel, or build-release with -Toolchain msvc), so
#     the everyday build directory keeps its timestamps and nobody has to rebuild it afterwards.
#   * The data (the wavetable pack, the two models, the voices) is packed into an archive of its own
#     that the setup downloads when it is not already installed (Deploy\Phosphene.iss). The portable
#     archive still carries everything.
#
# THE INTEL COMPILER (23.09.2026, at the user's request; until then this said "MSVC is what ships").
# The old objection was the determinism contract: the offline render is the oracle, the plugin must
# equal phos_render sample for sample, and the vector paths must equal the scalar path bit for bit.
# None of that is broken by icx as such -- every artefact of a release is built by the same compiler,
# so the plugin and phos_render still agree -- provided icx computes what the source says. Two things
# had to be made true first, and both are measured, not assumed:
#   - icx's default floating-point model is "fast", and even under /fp:precise it contracts a*b+c into
#     an FMA. The root CMakeLists gives it /fp:precise /Qfma- (checked on the generated code).
#   - Its /O2 vectoriser computed the singing bowl wrongly (1/260 of the energy, wrong frequencies; right
#     at /O1 and under MSVC). The loop is written so that it cannot trip over it (Core/src/Texture.cpp).
# The whole suite runs in the icx configuration below, and a release is only made when it passes there.
# -Toolchain msvc builds the MSVC way, for comparing the two.

param(
    [string]$Version = "",
    # Which compiler builds what ships: Intel oneAPI's icx (the default since 23.09.2026) or MSVC.
    [ValidateSet("intel", "msvc")]
    [string]$Toolchain = "intel",
    [switch]$SkipBuild,      # reuse whatever is in build-release already
    [switch]$SkipTests,      # do not run ctest (NOT for a release; see the warning it prints)
    [switch]$SkipManual,     # reuse the manual already in docs\manual
    [switch]$SkipQuest,      # do not rebuild the APK (reuse build-quest\PhospheneQuest.apk)
    [switch]$NoSetup,        # stage, check and zip, but do not call the Inno compiler
    # Copy the freshly made manual and its screenshots back into docs\ as well. Off by default: a
    # release build should not quietly rewrite a hundred committed PNGs, and docs\manual\*.pdf may
    # be open in a reader, which makes the copy fail. Turn it on when the pictures in the repository
    # really are meant to be the ones from this build.
    [switch]$UpdateDocs,
    # Code signing. Without it Windows shows "Unknown publisher" on the first run of the setup:
    # SmartScreen has nothing to go on but the file's reputation, and a fresh file has none.
    #   -SignWith "<thumbprint>"   a certificate in the current user's store (signtool /sha1)
    #   -SignWith "<file.pfx>"     a PFX on disk; -SignPassword goes with it
    # There is no certificate on this machine, so every release built here is unsigned. That is not
    # dangerous, it is unattested: the manifest's SHA-256 lines are what anybody can check instead.
    [string]$SignWith = "",
    [string]$SignPassword = "",
    [string]$TimestampUrl = "http://timestamp.digicert.com"
)
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
Set-Location $root

# The version comes out of the one place that holds it. Deliberately a regular expression against
# the project() line and not a parameter with a default: a default here would be a second place.
if (-not $Version) {
    $m = Select-String -Path (Join-Path $root "CMakeLists.txt") -Pattern 'project\(Phosphene VERSION ([0-9.]+)'
    if (-not $m) { throw "no version in CMakeLists.txt and none given" }
    $Version = $m.Matches[0].Groups[1].Value
}

$intel     = $Toolchain -eq "intel"
# A tree of its own per compiler: the two make different objects from the same sources, and sharing a
# build directory between them is a rebuild that looks incremental and is not.
$buildDir  = Join-Path $root ($(if ($intel) { "build-release-intel" } else { "build-release" }))
$stage     = Join-Path $root "Deploy\stage"
$out       = Join-Path $root "Deploy\out"
$manualDir = Join-Path $root "docs\manual"
$shots     = Join-Path $root "docs\screenshots"
$art       = Join-Path $buildDir "Plugin\Phosphene_artefacts\Release"
$exe       = Join-Path $art "Standalone\Phosphene.exe"
$vst       = Join-Path $art "VST3\Phosphene.vst3"
# Ninja (the Intel build) is single-configuration and puts no "Release" folder under the tools; JUCE's
# artefacts are under Release either way.
$render    = Join-Path $buildDir ($(if ($intel) { "Tools\render\phos_render.exe" } else { "Tools\render\Release\phos_render.exe" }))
$apk       = Join-Path $root "build-quest\PhospheneQuest.apk"
$refWav    = Join-Path $root "Deploy\reference.wav"

Write-Host "Phosphene $Version -- release build ($Toolchain)" -ForegroundColor Cyan
Write-Host "  tree:  $buildDir"

# Runs a batch file for its environment and keeps what it set (Visual Studio's vcvars64.bat): a batch file
# cannot change the environment of the PowerShell that called it, so it runs in a cmd of its own and what
# it left behind is read back. The same helper as Noctuary's release script.
function Import-CmdEnvironment([string]$batch) {
    if (-not (Test-Path $batch)) { throw "not found: $batch" }
    $tmp = [System.IO.Path]::GetTempFileName()
    cmd /c " `"$batch`" > nul 2>&1 && set > `"$tmp`" "
    foreach ($line in Get-Content $tmp) {
        if ($line -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($Matches[1])" -Value $Matches[2] }
    }
    Remove-Item $tmp -Force
}
# What oneAPI's setvars.bat would do, done by hand (on this machine setvars cannot find its per-component
# scripts): the compiler, lld-link one level down (JUCE's link-time optimisation makes icx write LLVM bitcode
# that link.exe cannot read), its libraries and its headers. Returns icx.exe.
function Enable-IntelToolchain {
    $vs = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Auxiliary\Build\vcvars64.bat" -ErrorAction SilentlyContinue |
          Select-Object -First 1 -ExpandProperty FullName
    if (-not $vs) { throw "vcvars64.bat not found -- the Intel build still needs the Windows SDK and the MSVC libraries" }
    Import-CmdEnvironment $vs
    $icx = Get-ChildItem "C:\Program Files (x86)\Intel\oneAPI\compiler\*\bin\icx.exe" -ErrorAction SilentlyContinue |
           Sort-Object FullName -Descending | Select-Object -First 1
    if (-not $icx) { throw "icx.exe not found -- is the oneAPI C++ compiler installed? (or run with -Toolchain msvc)" }
    $iroot = Split-Path -Parent (Split-Path -Parent $icx.FullName)
    $env:PATH = "$iroot\bin;$iroot\bin\compiler;$env:PATH"
    $env:LIB = "$iroot\lib;$env:LIB"
    $env:INCLUDE = "$iroot\include;$env:INCLUDE"
    Write-Host "  Intel oneAPI: $iroot" -ForegroundColor DarkGray
    return $icx.FullName
}
$skipped = @()
foreach ($s in @(@{n="-SkipBuild";  v=$SkipBuild;  w="the binaries are whatever was in build-release"},
                 @{n="-SkipTests";  v=$SkipTests;  w="NOTHING proves this build passes its own tests"},
                 @{n="-SkipManual"; v=$SkipManual; w="the manual may describe an older build"},
                 @{n="-SkipQuest";  v=$SkipQuest;  w="the APK may be from an older Core"},
                 @{n="-NoSetup";    v=$NoSetup;    w="no installer is produced"})) {
    if ($s.v) { $skipped += $s.n; Write-Warning ("{0}: {1}" -f $s.n, $s.w) }
}

# Wall-clock per step, printed as a table at the end -- so the next person knows what this costs
# before they start it rather than after.
$timings = New-Object System.Collections.Generic.List[object]
function Step([string]$name, [scriptblock]$body) {
    Write-Host ""
    Write-Host "== $name" -ForegroundColor Cyan
    $sw = [Diagnostics.Stopwatch]::StartNew()
    & $body
    $sw.Stop()
    $script:timings.Add([PSCustomObject]@{ Step = $name; Seconds = [math]::Round($sw.Elapsed.TotalSeconds, 1) })
}

# ---------------------------------------------------------------- 0. the build guard
# Five seconds, first, because it is the only thing here that looks at the two artefacts the rest of
# this script cannot check: the Quest sources (which nothing in ctest compiles) and the Android
# configure of the root project. Running it before a twenty-minute build rather than after is the
# whole point of it being cheap.
Step "guard (Quest sources + Android configure)" {
    & cmake "-DPHOS_ROOT=$root" "-DPHOS_SCRATCH=$root\build-guard" -P (Join-Path $root "Tools\release\quest_guard.cmake")
    if ($LASTEXITCODE -eq 77) { Write-Warning "the guard could not run (no NDK or no ThirdParty) -- the Quest side of this release is unverified" }
    elseif ($LASTEXITCODE -ne 0) { throw "the build guard failed" }
}

# ---------------------------------------------------------------- 1. build
if (-not $SkipBuild) {
    Step "configure + build Release (static runtime, AVX2)" {
        # JUCE writes the Windows version resource once and never notices afterwards that the
        # project's version changed. Noctuary shipped a 1.1.0 with 1.0.0 inside the executable that
        # way: the setup was named right, the file properties were wrong, and nothing said so.
        # Deleting the generated .rc forces it to be written again. check_package.ps1's check D
        # reads the resource back, so this is belt and braces.
        $rc = Join-Path $buildDir "Plugin\Phosphene_artefacts\JuceLibraryCode\Phosphene_resources.rc"
        if (Test-Path $rc) { Remove-Item $rc -Force }

        # JUCE from the checkout that is already on this machine rather than a fresh clone from
        # GitHub. The root CMakeLists points FETCHCONTENT_SOURCE_DIR_JUCE at ThirdParty/JUCE by
        # itself when that exists; this says it again for a tree where it does not.
        $juce = Join-Path $root "ThirdParty\JUCE"
        $common = @("-DPHOS_SHIP=ON", "-DPHOS_STATIC_RUNTIME=ON", "-DPHOS_AVX2=ON", "-DPHOS_BUILD_TOOLS=ON", "-DPHOS_BUILD_PLUGIN=ON")
        if (Test-Path (Join-Path $juce "CMakeLists.txt")) { $common += "-DFETCHCONTENT_SOURCE_DIR_JUCE=$juce" }

        if ($intel) {
            $icx = Enable-IntelToolchain
            # Ninja, which comes with Visual Studio: there is no Visual Studio generator for icx without the
            # IDE integration, and NMake (Noctuary's choice) builds one file at a time.
            $ninja = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe" -ErrorAction SilentlyContinue |
                     Select-Object -First 1 -ExpandProperty FullName
            if (-not $ninja) { throw "ninja.exe not found (it ships with Visual Studio's CMake component)" }
            $lld = "-fuse-ld=lld"
            & cmake -S $root -B $buildDir -G Ninja "-DCMAKE_MAKE_PROGRAM=$ninja" -DCMAKE_BUILD_TYPE=Release `
                "-DCMAKE_C_COMPILER=$icx" "-DCMAKE_CXX_COMPILER=$icx" `
                "-DCMAKE_EXE_LINKER_FLAGS=$lld" "-DCMAKE_SHARED_LINKER_FLAGS=$lld" "-DCMAKE_MODULE_LINKER_FLAGS=$lld" @common
            if ($LASTEXITCODE -ne 0) { throw "configure failed" }
            # Four jobs: this machine usually has other work on it.
            & cmake --build $buildDir -- -j 4
            if ($LASTEXITCODE -ne 0) { throw "build failed" }
        } else {
            & cmake -S $root -B $buildDir -G "Visual Studio 18 2026" -A x64 @common
            if ($LASTEXITCODE -ne 0) { throw "configure failed" }
            # A build for other people is not worth having in a hurry, and this machine has other work
            # on it: --parallel 2 on top of MSVC's own /MP leaves it usable.
            & cmake --build $buildDir --config Release --parallel 2
            if ($LASTEXITCODE -ne 0) { throw "build failed" }
        }
    }
}
foreach ($p in @($exe, $vst, $render)) { if (-not (Test-Path $p)) { throw "missing build output: $p" } }

# ---------------------------------------------------------------- 2. the tests, in this configuration
if (-not $SkipTests) {
    Step "ctest (the quick suite and the VST3 test, in the configuration that ships)" {
        # PHOS_MUTE has to be OFF. It is set in the shell for every standalone started by hand, and
        # it makes the plugin deliberately silent for ever -- the host test then measures -inf dBFS
        # everywhere and fails for a reason that has nothing to do with the build. Noctuary lost a
        # release build to exactly this.
        # (Tests/CMakeLists.txt now also unsets it for the host and VST3 test themselves; this
        # stays as the second belt.)
        Remove-Item env:PHOS_MUTE -ErrorAction SilentlyContinue
        # Every self-test section is its own test (selftest.<name>, Tests/selftest_tests.cmake), so
        # the suite runs six at a time; the host and VST3 tests still run alone (RUN_SERIAL). The old
        # all-in-one `selftest` is disabled unless PHOS_SELFTEST_FULL is set -- it would only run
        # every section a second time. -j 6 matches the recommendation in the README: a release
        # build is often made while somebody works on this machine.
        Remove-Item env:PHOS_SELFTEST_FULL -ErrorAction SilentlyContinue
        # The quick suite (26.09.2026, the user: "Wobei wir auch vor Releases nicht immer diesen kompletten Wahnsinn
        # ablaufen müssen"), plus the one long test that is about what ships: the built VST3 loaded as a DAW loads it.
        # The long tests are registered only when PHOS_TESTS names them (Tests/selftest_tests.cmake).
        $env:PHOS_TESTS = "vst3test"
        & ctest --test-dir $buildDir -C Release -j 12 --output-on-failure
        Remove-Item env:PHOS_TESTS -ErrorAction SilentlyContinue
        $code = $LASTEXITCODE
        # Keep the evidence. ctest writes Testing\Temporary\LastTest.log and overwrites it on the
        # next run, so the log of a failure is gone the moment anyone re-runs the suite to see
        # whether it was transient -- which is the first thing anyone does. On 16.09.2026 the self
        # test failed once in a run of eight and was green in the seventeen runs that followed; by
        # then the only record of which check had failed had been overwritten, and the cause is
        # still unknown. A timestamped copy costs nothing and makes the next one answerable.
        $last = Join-Path $buildDir "Testing\Temporary\LastTest.log"
        if ($code -ne 0 -and (Test-Path $last)) {
            $kept = Join-Path $root ("build-release-failure-" + (Get-Date -Format "yyyyMMdd-HHmmss") + ".log")
            Copy-Item $last $kept -Force
            Write-Warning "the failing run's log is kept at $kept"
        }
        if ($code -ne 0) { throw "the test suite failed in the release configuration -- nothing is packaged" }
    }
} else {
    Write-Warning "ctest skipped"
}

# ---------------------------------------------------------------- 3. the reference render
# Made by the binary that is about to be staged, reading a copy of Core\data that is not the staging
# directory. The package check then renders the same thing with the *staged* copy of the same binary,
# from an unrelated directory with nothing in it, and requires the two to be identical; anything else
# means the staged data files are not found, are stale, or are truncated.
#
# A working directory of its own, filled here, and not $buildDir as before. Until 16.09.2026 this
# render was made in the build tree with no data file beside it at all, and it worked only because
# every binary carried PHOS_SOURCE_DATA_DIR and quietly read the source tree -- the very thing
# PHOS_SHIP now removes. With that gone, a reference render from an empty directory would be a
# fallback render, and comparing two fallback renders proves nothing about the models.
#
# Both neural models are switched on because they are off by default, and a model file that nothing
# opens proves nothing by being present.
Step "reference render (64 bars, both neural models)" {
    if (Test-Path $refWav) { Remove-Item $refWav -Force }
    $refDir = Join-Path $buildDir "refdata"
    if (Test-Path $refDir) { Remove-Item $refDir -Recurse -Force }
    New-Item -ItemType Directory -Force $refDir | Out-Null
    foreach ($f in @("library.phoswt", "melody.phosmdl", "bass.phosmdl", "voices.phosvx")) {
        Copy-Item (Join-Path $root "Core\data\$f") $refDir -Force
    }
    Push-Location $refDir
    try {
        & $render --bars 64 --set "compose.melody_model=Neural compose.bass_model=Neural" --out $refWav
        if ($LASTEXITCODE -ne 0) { throw "the reference render failed" }
    } finally { Pop-Location }
    Write-Host ("  {0:N0} bytes, sha256 {1}" -f (Get-Item $refWav).Length, (Get-FileHash $refWav -Algorithm SHA256).Hash.ToLower().Substring(0, 16))
}

# ---------------------------------------------------------------- 4. the manual
# Made from the running plugin: the parameter tables and the groups each tab really built, read back
# out of the editor, plus one picture per tab at design size. That is why it cannot go stale the way
# a written description would -- and why it has to run the binary that is being released rather than
# any older one. PHOS_SHOT_WAIT is how long the composer gets to plan before the pictures are taken;
# 26 s plans a whole sixty-minute set, which the arrange timeline needs in order to show anything.
$manualWork = Join-Path $root "Deploy\manual-work"
$manualPdf = Join-Path $manualWork "Phosphene-Manual.pdf"
if (-not $SkipManual) {
    Step "manual (screenshots from the plugin, then HTML and PDF)" {
        # Into a work folder of its own, not into docs\screenshots and docs\manual. Two reasons.
        # docs\manual is where people open the PDF, and a PDF open in a reader can be neither
        # deleted nor overwritten -- which cost Noctuary two whole release builds. And the work
        # folder is generated and thrown away, so a release build never leaves the repository dirty
        # with a hundred changed pixels in a screenshot. -UpdateDocs below copies it in on purpose.
        if (Test-Path $manualWork) { Remove-Item $manualWork -Recurse -Force }
        New-Item -ItemType Directory -Force $manualWork | Out-Null

        $env:PHOS_MANUAL = $manualWork
        $env:PHOS_SHOT_WAIT = "26"    # how long the composer gets to plan; 26 s plans a whole set
        try {
            $mp = Start-Process $exe -PassThru
            if (-not $mp.WaitForExit(180000)) { $mp.Kill(); throw "the manual export did not finish in three minutes" }
        } finally {
            Remove-Item env:PHOS_MANUAL, env:PHOS_SHOT_WAIT -ErrorAction SilentlyContinue
        }
        $json = Join-Path $manualWork "manual.json"
        if (-not (Test-Path $json)) { throw "the plugin wrote no manual.json into $manualWork" }
        # The version the running plugin put in there is its own JucePlugin_VersionString, which
        # comes from the project() line by way of juce_add_plugin. If it disagrees with the version
        # being packaged, something is being shipped that was not built for this release.
        $man = Get-Content $json -Raw | ConvertFrom-Json
        if ($man.version -ne $Version) { throw "the plugin reports version $($man.version), but this release is $Version" }
        Write-Host ("  manual.json: version {0}, {1} parameters, {2} tabs" -f $man.version, $man.params.Count, $man.tabs.Count)

        # --dir and --out the same folder, so the HTML points at the pictures beside it rather than
        # at ..\screenshots: the work folder is then self-contained and can be read without the
        # repository. The PDF carries its pictures inside itself either way, and the PDF is what
        # ships (Deploy\Phosphene.iss).
        #
        # The generator refuses to print at all when a parameter exists in the engine but appears on
        # no tab -- an unreachable control is a real bug and a manual that omits it looks finished.
        # Its exit code is that refusal; --allow-holes is never passed here.
        & python (Join-Path $root "Tools\manual\make_manual.py") --dir $manualWork --out $manualWork
        if ($LASTEXITCODE -ne 0) { throw "the manual did not print (a parameter appears on no tab, or the generator failed)" }
        if (-not (Test-Path $manualPdf)) { throw "no PDF came out of the manual generator" }

        if ($UpdateDocs) {
            # Best effort, on purpose: if the PDF in docs\manual is open in a reader this fails, and
            # that must not cost the release. The installer takes its copy from the work folder.
            New-Item -ItemType Directory -Force -Path $manualDir, $shots | Out-Null
            $failed = $false
            Get-ChildItem $manualWork -File | ForEach-Object {
                $dst = $(if ($_.Extension -eq ".png" -or $_.Name -eq "manual.json") { Join-Path $shots $_.Name } else { Join-Path $manualDir $_.Name })
                try { Copy-Item $_.FullName $dst -Force -ErrorAction Stop } catch { $failed = $true }
            }
            if ($failed) { Write-Warning "docs could not be fully updated (a file is open there); the installer takes the manual from $manualWork" }
        }
    }
} else {
    Write-Warning "manual skipped"
    # Fall back to the copy in the repository when the work folder is not there.
    if (-not (Test-Path $manualPdf)) { $manualPdf = Join-Path $manualDir "Phosphene-Manual.pdf" }
}

# ---------------------------------------------------------------- 5. the Quest APK
if (-not $SkipQuest) {
    Step "Quest APK (NDK, aapt2, zipalign, apksigner)" {
        if (-not (Test-Path (Join-Path $root "ThirdParty\openxr-loader\prefab"))) {
            & powershell -NoProfile -File (Join-Path $root "Quest\fetch_thirdparty.ps1")
            if ($LASTEXITCODE -ne 0) { throw "Quest\fetch_thirdparty.ps1 failed" }
        }
        & powershell -NoProfile -File (Join-Path $root "Quest\build_apk.ps1")
        if ($LASTEXITCODE -ne 0) { throw "the APK build failed" }
    }
} else {
    Write-Warning "APK build skipped"
}

# ---------------------------------------------------------------- 6. stage
# Everything the installer will pack, in one directory that can be looked at. Built from scratch
# every time: a staging folder that is only ever added to is how a file that was dropped from the
# product three versions ago keeps being shipped.
Step "stage" {
    if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
    New-Item -ItemType Directory -Force -Path $stage, $out | Out-Null

    Copy-Item $exe $stage
    Copy-Item $render $stage
    Copy-Item $vst (Join-Path $stage "Phosphene.vst3") -Recurse
    Copy-Item (Join-Path $root "Deploy\phosphene.ico") $stage
    Copy-Item (Join-Path $root "LICENSE") (Join-Path $stage "LICENSE.txt")
    if (Test-Path $manualPdf) { Copy-Item $manualPdf $stage } else { Write-Warning "no manual PDF to ship" }
    if (Test-Path $apk) { Copy-Item $apk $stage } else { Write-Warning "no APK to ship" }

    # The three files the engine opens by bare name, into both places a binary looks: beside the
    # executables, and inside the VST3 bundle's Contents\Resources. The plugin's own post-build step
    # puts all three in both places already; they are copied again from Core\data here so that the
    # staging directory is built from the repository rather than from whatever is in the build tree,
    # and check A/B then hashes each copy against Core\data.
    $dataDir = Join-Path $root "Core\data"
    $res = Join-Path $stage "Phosphene.vst3\Contents\Resources"
    New-Item -ItemType Directory -Force $res | Out-Null
    # 19.09.2026: the voice pack (Vocal.h) and its credits travel the same way.
    foreach ($f in @("library.phoswt", "melody.phosmdl", "bass.phosmdl", "voices.phosvx", "CREDITS-wavetables.md", "CREDITS-voices.md")) {
        Copy-Item (Join-Path $dataDir $f) $stage -Force
        if ($f -notlike "CREDITS-*") { Copy-Item (Join-Path $dataDir $f) $res -Force }
    }

    # The read-me for the portable archive: what each file is and where it has to go for anybody who
    # would rather not run an installer.
    $readme = @"
Phosphene $Version
==================

A generator for complete psytrance sets: kick, bass, percussion, acid, leads, arpeggios, pads and
effects, composed and synthesised from a seed, a style profile and an energy arc. Everything is
synthesised except the spoken phrases of voices.phosvx (public-domain and free-to-reuse recordings,
credited in CREDITS-voices.md).

WHAT IS HERE

  Phosphene.exe        the standalone. Nothing else needs to be installed.
  Phosphene.vst3       the plug-in. Copy the whole FOLDER to
                       C:\Program Files\Common Files\VST3\ and rescan in your DAW.
  phos_render.exe      the offline renderer:
                         phos_render --minutes 60 --seed 2026 --out night.wav --midi night.mid
                       --list prints every parameter, --version says which build this is.
  library.phoswt       the wavetable pack, and
  melody.phosmdl       the two learned models the composer can use.
  bass.phosmdl         KEEP THESE BESIDE Phosphene.exe AND phos_render.exe (and voices.phosvx, the
                       spoken phrases -- without it the voices speak only in synthetic formants). Without them the
                       engine falls back to six built-in wavetables and to the Markov composer. The
                       Set tab says which pitch model each part is really using and the choosers mark
                       an entry whose file is missing, so you can see it -- phos_render only says it
                       on stderr. The copies inside Phosphene.vst3\Contents\Resources are the
                       plug-in's own; leave them there.
  Phosphene-Manual.pdf the manual. Every picture in it is the plugin drawing itself.
  PhospheneQuest.apk   the Meta Quest build. Developer mode, a cable, then
                         adb install -r PhospheneQuest.apk
  CREDITS-wavetables.md  where the wavetables come from.

THE FIELD RECORDINGS (optional, 2 GB)

  The Field track plays field recordings -- forest, night, water, weather, machines, NASA's sounds from
  space -- and they are not in this archive. Download Phosphene-field-1a.zip and Phosphene-field-1b.zip
  from https://github.com/reneweller-coding/Phosphene/releases/tag/field-data-1 and unpack both here,
  so that a folder "field" lies beside Phosphene.exe. Without it everything plays; only the Field
  track and the NASA shots stay silent. On the Quest:
    adb push field /sdcard/Android/data/com.reneweller.phosphene.quest/files/

The setup does all of that for you; this archive is for anyone who would rather it did not.

FIRST RUN

Press play. A track opens with a sparse intro -- the kick enters between bars five and nine -- and
the set plans itself ahead of what you hear. Every track and every section can be locked or rerolled
in the Arrange tab and the whole thing saved as a .phosset.

$(Get-Content (Join-Path $root "LICENSE") -TotalCount 1)
"@
    [IO.File]::WriteAllText((Join-Path $stage "README.txt"), ($readme -replace "`r`n", "`n" -replace "`n", "`r`n"))
    Write-Host ("  staged {0} files" -f (Get-ChildItem $stage -Recurse -File).Count)
}

# ---------------------------------------------------------------- 6b. the data archive
# The setup is slim and fetches the data (Deploy\Phosphene.iss) -- unless the same data is already installed,
# which it finds out by the hashes written here. One archive, attached to the release it belongs to; its name
# carries the version, so an old setup never unpacks a newer pack by accident. The two generated includes
# name the file, its size and its SHA-256; Inno checks the download against that hash before unpacking.
$dataFiles = @("library.phoswt", "melody.phosmdl", "bass.phosmdl", "voices.phosvx", "CREDITS-wavetables.md", "CREDITS-voices.md")
# The field recordings (27.09.2026) are not in it: two archives of a release of their own (Tools\field_archives.py),
# whose names and hashes Deploy\field-files.iss carries into the setup, their credits inside them (field\CREDITS-field.md).
# That file is committed; without it no setup.
if (-not (Test-Path (Join-Path $root "Deploy\field-files.iss"))) { throw "Deploy\field-files.iss is missing -- run Tools\field_archives.py" }
$dataZipName = "Phosphene-data-$Version.zip"
Step "data archive (downloaded by the setup)" {
    New-Item -ItemType Directory -Force -Path $out | Out-Null
    $dataZip = Join-Path $out $dataZipName
    if (Test-Path $dataZip) { Remove-Item $dataZip -Force }
    Compress-Archive -Path ($dataFiles | ForEach-Object { Join-Path $root "Core\data\$_" }) -DestinationPath $dataZip -CompressionLevel Optimal
    $zipHash = (Get-FileHash $dataZip -Algorithm SHA256).Hash.ToLower()
    $lines = @("// Generated by Deploy\build_release.ps1 -- do not edit.",
               "#define DataZip `"$dataZipName`"",
               "#define DataZipSha256 `"$zipHash`"")
    # The installed files' own hashes: the setup compares them with what is already in the folder and does not
    # download at all when every one of them matches (an update that changes no data, a reinstall).
    $i = 0
    foreach ($f in $dataFiles) {
        $h = (Get-FileHash (Join-Path $root "Core\data\$f") -Algorithm SHA256).Hash.ToLower()
        $lines += "#define DataFile$i `"$f`""
        $lines += "#define DataHash$i `"$h`""
        $i++
    }
    $lines += "#define DataFileCount $i"
    [IO.File]::WriteAllLines((Join-Path $root "Deploy\data-files.iss"), $lines)
    Write-Host ("  {0}: {1:N1} MB, sha256 {2}" -f $dataZipName, ((Get-Item $dataZip).Length / 1MB), $zipHash.Substring(0, 16))
}

# ---------------------------------------------------------------- 7. the package check
# The gate. Everything above can succeed and still leave a payload that installs a product which
# quietly plays the wrong sound; this is what refuses to let that become an installer.
Step "package check" {
    & powershell -NoProfile -File (Join-Path $root "Tools\release\check_package.ps1") `
        -Stage $stage -Version $Version -Reference $refWav -Manifest (Join-Path $out "MANIFEST-$Version.txt")
    if ($LASTEXITCODE -ne 0) { throw "the package check failed -- no installer was made" }
}

# ---------------------------------------------------------------- 8. signing
function Invoke-Sign([string]$path) {
    if (-not $SignWith) { return }
    $signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin\*\x64\signtool.exe" -ErrorAction SilentlyContinue |
                Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
    if (-not $signtool) { throw "signtool.exe not found (Windows SDK). Install it, or build without -SignWith." }
    $a = @("sign", "/fd", "SHA256", "/tr", $TimestampUrl, "/td", "SHA256")
    if (Test-Path $SignWith) {
        $a += @("/f", $SignWith)
        if ($SignPassword) { $a += @("/p", $SignPassword) }
    } else {
        $a += @("/sha1", $SignWith)
    }
    & $signtool @a $path
    if ($LASTEXITCODE -ne 0) { throw "signing failed for $path" }
    Write-Host "  signed $(Split-Path $path -Leaf)" -ForegroundColor Green
}
# The executable is signed before it goes into the installer and the installer after it is built:
# Windows checks both, and a signed setup that unpacks an unsigned exe warns about the exe instead.
if ($SignWith) { Invoke-Sign (Join-Path $stage "Phosphene.exe"); Invoke-Sign (Join-Path $stage "phos_render.exe") }
else { Write-Warning "no -SignWith: the setup will be unsigned and Windows will call the publisher unknown. The manifest's hashes are what can be checked instead." }

# ---------------------------------------------------------------- 9. the portable archive
Step "portable archive" {
    $zip = Join-Path $out "Phosphene-$Version-portable.zip"
    if (Test-Path $zip) {
        Write-Warning ("replacing {0}, built {1}" -f (Split-Path $zip -Leaf), (Get-Item $zip).LastWriteTime)
        Remove-Item $zip -Force
    }
    Compress-Archive -Path (Join-Path $stage "*") -DestinationPath $zip -CompressionLevel Optimal
    Write-Host ("  {0:N1} MB" -f ((Get-Item $zip).Length / 1MB))
}

# ---------------------------------------------------------------- 10. the setup
if (-not $NoSetup) {
    Step "Inno Setup" {
        $iscc = Get-ChildItem "C:\Program Files\Inno Setup *\ISCC.exe", "C:\Program Files (x86)\Inno Setup *\ISCC.exe" -ErrorAction SilentlyContinue |
                Sort-Object FullName | Select-Object -Last 1 -ExpandProperty FullName
        if (-not $iscc) { throw "Inno Setup not found. winget install JRSoftware.InnoSetup, or run with -NoSetup." }
        # The data comes from the release this setup belongs to (Deploy\Phosphene.iss, DataBaseUrl).
        & $iscc "/DVersion=$Version" "/DDataBaseUrl=https://github.com/reneweller-coding/Phosphene/releases/download/v$Version" (Join-Path $root "Deploy\Phosphene.iss")
        if ($LASTEXITCODE -ne 0) { throw "the installer failed to build" }
        $setup = Join-Path $out "Phosphene-$Version-Setup.exe"
        if (-not (Test-Path $setup)) { throw "the compiler reported success but there is no $setup" }
        Invoke-Sign $setup
        Write-Host ("  {0:N1} MB" -f ((Get-Item $setup).Length / 1MB)) -ForegroundColor Green
    }
}

# ---------------------------------------------------------------- 11. what came out
# The hashes are what an unsigned build can offer instead of a signature: printed here, meant for
# the release notes, so that anybody can check the file they downloaded is the file that was built.
$sums = Join-Path $out "SHA256SUMS.txt"
$lines = Get-ChildItem $out -File | Where-Object { $_.Name -ne "SHA256SUMS.txt" } | ForEach-Object {
    "{0}  {1}" -f (Get-FileHash $_.FullName -Algorithm SHA256).Hash.ToLower(), $_.Name
}
[IO.File]::WriteAllLines($sums, $lines)

Write-Host ""
Write-Host "Phosphene $Version" -ForegroundColor Green
if ($skipped.Count -gt 0) { Write-Warning ("built with {0} -- not a complete release" -f ($skipped -join ", ")) }
$timings | Format-Table Step, Seconds -AutoSize
Get-ChildItem $out -File | Sort-Object Name | Format-Table Name, @{ n = "MB"; e = { "{0:N1}" -f ($_.Length / 1MB) } }, LastWriteTime -AutoSize
Write-Host "checksums: $sums"
Get-Content $sums | ForEach-Object { Write-Host "  $_" }
