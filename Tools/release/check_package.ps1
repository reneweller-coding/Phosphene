# Phosphene -- the package check: does the thing we are about to ship actually contain a product?
#
#   powershell -File Tools\release\check_package.ps1 -Stage Deploy\stage -Version 1.0.0
#                    [-Reference <wav>] [-Manifest Deploy\out\MANIFEST.txt]
#
# WHY THIS EXISTS
#
# The manual generator has a rule worth copying: it refuses to print a manual in which a parameter
# exists in the engine but appears on no page, because such a manual looks finished and is not. A
# package has the same failure mode, and worse -- a missing data file is not an error at runtime,
# it is a quiet fallback. Without library.phoswt the engine plays its six built-in wavetables;
# without melody.phosmdl and bass.phosmdl the composer drops back to the Markov model and the
# pattern families. Both print one line on stderr, which a plug-in inside a DAW never shows anyone.
# The product would install, start, and sound wrong, and nothing would say so.
#
# So this script opens the staging directory -- the exact payload the installer will pack -- and
# proves, rather than assumes:
#
#   A  every file the runtime needs is there and is not empty;
#   B  every data file is byte-for-byte the one in Core/data (a truncated or stale copy is caught);
#   C  the APK really carries all three data files as assets, at the right length;
#   D  the version is the same in the renderer, in the standalone's version resource and in the
#      VST3's -- one number, four places, which is where Noctuary once shipped 1.1.0 with 1.0.0
#      inside the binary;
#   E  no binary still wants a Visual C++ or Intel runtime DLL that a music machine will not have;
#   F  the staged renderer runs from an unrelated directory and renders the reference bit for bit,
#      with both neural models switched on. Read what this does and does not prove -- the comment at
#      check F below is there because the first version of it proved nothing at all;
#   G  the manual is there, carries this version, and has no unreachable-parameter hole in it;
#   H  the two learned models of Phase 8 are in every place the three runtimes look for them, which
#      since 16.09.2026 is the only way they are found at all.
#
# It then writes the manifest -- every staged file with its size and SHA-256 -- which is what goes
# into the release notes and what anybody can check a download against.
#
# Exit code 0 only when every check passed. Every failure is collected and all of them are printed,
# because finding out about the second missing file after another twenty-minute build is a waste.

param(
    [Parameter(Mandatory = $true)][string]$Stage,
    [Parameter(Mandatory = $true)][string]$Version,
    [string]$Root = "",
    # A render made by the same binary from the build tree, where no data file lies beside it.
    # Without one, check F is skipped and said to be skipped.
    [string]$Reference = "",
    [string]$Manifest = ""
)
$ErrorActionPreference = "Stop"
if (-not $Root) { $Root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)) }
$Stage = (Resolve-Path $Stage).Path
$data = Join-Path $Root "Core\data"

$failures = New-Object System.Collections.Generic.List[string]
function Fail([string]$m) { $script:failures.Add($m); Write-Host "  FAIL  $m" -ForegroundColor Red }
function Ok([string]$m)   { Write-Host "  ok    $m" -ForegroundColor DarkGray }

# ---------------------------------------------------------------- the declaration
# What a Phosphene package is. `Min` is a floor, not a size: it only has to be large enough that a
# zero-byte or half-written file is caught, and small enough not to need editing when a binary
# grows. `From` names the file in the repository a copy must equal byte for byte.
$payload = @(
    @{ Path = "Phosphene.exe";                       Min = 2MB;   What = "the standalone" }
    @{ Path = "phos_render.exe";                     Min = 200KB; What = "the offline renderer" }
    @{ Path = "library.phoswt";                      Min = 100KB; What = "wavetable pack (beside the binaries)";  From = "library.phoswt" }
    @{ Path = "melody.phosmdl";                      Min = 100KB; What = "melody model (beside the binaries)";     From = "melody.phosmdl" }
    @{ Path = "bass.phosmdl";                        Min = 100KB; What = "bass model (beside the binaries)";       From = "bass.phosmdl" }
    @{ Path = "CREDITS-wavetables.md";               Min = 200;   What = "where the wavetables come from";         From = "CREDITS-wavetables.md" }
    @{ Path = "LICENSE.txt";                         Min = 10KB;  What = "the licence the setup shows" }
    @{ Path = "README.txt";                          Min = 500;   What = "the portable archive's read-me" }
    @{ Path = "phosphene.ico";                       Min = 1KB;   What = "the setup and shortcut icon" }
    @{ Path = "Phosphene.vst3\Contents\x86_64-win\Phosphene.vst3";      Min = 2MB;   What = "the VST3 module" }
    @{ Path = "Phosphene.vst3\Contents\Resources\library.phoswt";       Min = 100KB; What = "wavetable pack (inside the bundle)"; From = "library.phoswt" }
    @{ Path = "Phosphene.vst3\Contents\Resources\melody.phosmdl";       Min = 100KB; What = "melody model (inside the bundle)";   From = "melody.phosmdl" }
    @{ Path = "Phosphene.vst3\Contents\Resources\bass.phosmdl";         Min = 100KB; What = "bass model (inside the bundle)";     From = "bass.phosmdl" }
    # The PDF only, not the HTML: the HTML points at the screenshot PNGs by relative path
    # (Tools/manual/make_manual.py writes them as ../screenshots/...), so shipping it would mean
    # shipping two more megabytes of pictures to say what the PDF already carries inside itself.
    @{ Path = "Phosphene-Manual.pdf";                Min = 200KB; What = "the manual" }
    @{ Path = "PhospheneQuest.apk";                  Min = 1MB;   What = "the headset build" }
)

Write-Host "package check: $Stage (version $Version)" -ForegroundColor Cyan

# ---------------------------------------------------------------- A + B: presence, size, identity
Write-Host "A/B  files and their contents"
foreach ($e in $payload) {
    $p = Join-Path $Stage $e.Path
    if (-not (Test-Path -LiteralPath $p)) { Fail ("missing: {0}  ({1})" -f $e.Path, $e.What); continue }
    $len = (Get-Item -LiteralPath $p).Length
    if ($len -lt $e.Min) { Fail ("too small: {0} is {1:N0} bytes, under the {2:N0} a real {3} has" -f $e.Path, $len, $e.Min, $e.What); continue }
    if ($e.ContainsKey("From")) {
        $src = Join-Path $data $e.From
        if (-not (Test-Path -LiteralPath $src)) { Fail ("no source to compare against: $src"); continue }
        $h1 = (Get-FileHash -LiteralPath $p -Algorithm SHA256).Hash
        $h2 = (Get-FileHash -LiteralPath $src -Algorithm SHA256).Hash
        if ($h1 -ne $h2) { Fail ("{0} is not the file in Core/data ({1} against {2})" -f $e.Path, $h1.Substring(0,12), $h2.Substring(0,12)); continue }
        Ok ("{0}  {1:N0} bytes, same as Core/data/{2}" -f $e.Path, $len, $e.From)
    } else {
        Ok ("{0}  {1:N0} bytes" -f $e.Path, $len)
    }
}

# Nothing unexpected either: a file in the staging directory that the declaration does not know
# about either belongs in the list or is a leftover from an earlier run that is about to be shipped.
$declared = @{}
foreach ($e in $payload) { $declared[$e.Path.ToLower()] = $true }
Get-ChildItem -LiteralPath $Stage -Recurse -File | ForEach-Object {
    $rel = $_.FullName.Substring($Stage.Length).TrimStart('\')
    # Everything inside the VST3 bundle except the declared entries is JUCE's own (moduleinfo.json,
    # desktop.ini, the PlugIn.ico): the bundle is staged whole and is not this list's business.
    if ($rel.ToLower().StartsWith("phosphene.vst3\")) { return }
    if (-not $declared.ContainsKey($rel.ToLower())) { Fail ("not declared, but staged: $rel") }
}

# ---------------------------------------------------------------- C: the APK's assets
# All three, because the headset unpacks all three (Quest/src/main.cpp, prepareAsset). A missing pack
# is the six built-in tables; a missing model is the Markov composer and the pattern families. Both
# are audible and neither is fatal, and the Quest is the one surface with no stderr for anybody to
# read -- so nothing at all would say so.
Write-Host "C    the APK's assets"
$apk = Join-Path $Stage "PhospheneQuest.apk"
$apkAssets = @{}
if (Test-Path -LiteralPath $apk) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($apk)
    try {
        foreach ($e in $zip.Entries) { if ($e.FullName.StartsWith("assets/")) { $apkAssets[$e.FullName] = @($e.Length, $e.CompressedLength) } }
    } finally { $zip.Dispose() }
    foreach ($f in @("library.phoswt", "melody.phosmdl", "bass.phosmdl")) {
        $key = "assets/$f"
        $want = (Get-Item -LiteralPath (Join-Path $data $f)).Length
        if (-not $apkAssets.ContainsKey($key)) {
            Fail "the APK carries no $key -- on the headset that is a silent fallback, which is audible and not fatal, so it would ship unnoticed"
        } elseif ($apkAssets[$key][0] -ne $want) {
            Fail ("the APK's {0} is {1:N0} bytes, not the {2:N0} of Core/data" -f $key, $apkAssets[$key][0], $want)
        } else {
            Ok ("{0}  {1:N0} bytes, {2:N0} compressed" -f $key, $apkAssets[$key][0], $apkAssets[$key][1])
        }
    }
}

# ---------------------------------------------------------------- D: one version, everywhere
Write-Host "D    the version in every artefact"
$render = Join-Path $Stage "phos_render.exe"
if (Test-Path -LiteralPath $render) {
    $lines = & $render --version 2>&1
    $first = ($lines | Select-Object -First 1)
    if ($first -ne "phos_render $Version") { Fail ("phos_render --version says '$first', not 'phos_render $Version'") }
    else { Ok ("phos_render --version: $first; $($lines | Select-Object -Skip 1 -First 1)") }
}
# The Windows version resource, which is what a DAW and Explorer show. JUCE writes it once from the
# project's version and does not notice later that the version changed -- so it is read back rather
# than trusted (Deploy/build_release.ps1 deletes the generated .rc before a release build for the
# same reason).
foreach ($rel in @("Phosphene.exe", "Phosphene.vst3\Contents\x86_64-win\Phosphene.vst3")) {
    $p = Join-Path $Stage $rel
    if (-not (Test-Path -LiteralPath $p)) { continue }
    $vi = (Get-Item -LiteralPath $p).VersionInfo
    $got = "{0}.{1}.{2}" -f $vi.FileMajorPart, $vi.FileMinorPart, $vi.FileBuildPart
    if ($got -ne $Version) { Fail ("$rel has version $got in its resource, not $Version") }
    else { Ok ("$rel  version resource $got") }
}

# ---------------------------------------------------------------- E: runtime dependencies
Write-Host "E    what the binaries still need from the machine"
$dumpbin = Get-ChildItem "C:\Program Files\Microsoft Visual Studio\*\*\VC\Tools\MSVC\*\bin\Hostx64\x64\dumpbin.exe" -ErrorAction SilentlyContinue |
           Select-Object -First 1 -ExpandProperty FullName
if (-not $dumpbin) {
    Write-Warning "  dumpbin not found -- the runtime check did not run. A binary that still wants the Visual C++ runtime fails on a clean machine as 'it just does not start'."
} else {
    foreach ($rel in @("Phosphene.exe", "phos_render.exe", "Phosphene.vst3\Contents\x86_64-win\Phosphene.vst3")) {
        $p = Join-Path $Stage $rel
        if (-not (Test-Path -LiteralPath $p)) { continue }
        $deps = & $dumpbin /dependents $p | Select-String -Pattern '^\s+\S+\.dll' | ForEach-Object { $_.Line.Trim() }
        $bad = $deps | Where-Object { $_ -match '^(VCRUNTIME|MSVCP|CONCRT|api-ms-win-crt|libmmd|svml|libiomp|libirng)' }
        if ($bad) { Fail ("$rel still needs a runtime nobody has: $($bad -join ', ')") }
        else { Ok ("$rel  $($deps.Count) system DLLs, none of them a redistributable") }
    }
}

# ---------------------------------------------------------------- F: the data path, end to end
# WHAT THIS PROVES -- AND WHAT IT DID NOT, UNTIL THE PATH INTO THE SOURCE TREE WAS TAKEN OUT.
#
# It was written to prove the whole data path at once: staged renderer, started somewhere else,
# renders the reference, therefore it found the staged data files. On 16.09.2026 it did not, and the
# way that was found out is worth keeping: with the staged bass.phosmdl deleted outright, and again
# with one byte of the staged melody.phosmdl flipped, this check still passed. The core's lookup had
# a third step, PHOS_SOURCE_DATA_DIR, which Core/CMakeLists.txt baked into every binary as an
# absolute path into the developer's own source tree -- so on the machine that built it, a Phosphene
# binary found Core/data no matter what was or was not staged beside it, and no run-time test on the
# build machine could tell "opened the staged copy" from "opened the source copy".
#
# A shipping build no longer defines it (PHOS_SHIP, which Deploy/build_release.ps1 sets), so the
# staged binary now has exactly two places to look: beside itself and its working directory. This
# check gives it a working directory with nothing in it, which leaves only the staged files -- and a
# deleted or stale model therefore changes the render and is caught here. F2 below is what keeps it
# that way: it *fails* the package if the string is in a staged binary after all.
#
# What it proves besides: the staged binary is the build ctest passed and not a stale or half-copied
# one, it starts with no working directory of its own, the static-runtime build renders exactly what
# the tested build renders, and both learned models load and are used -- they are off by default,
# hence the --set, because a model file that nothing opens proves nothing by being present.
Write-Host "F    the staged renderer is the build that was tested, reading the staged data"
if (-not $Reference) {
    Write-Warning "  no -Reference render given -- NOT checked: that the staged binary renders what the tested one renders."
} elseif (-not (Test-Path -LiteralPath $Reference)) {
    Fail "the reference render $Reference is not there"
} elseif (Test-Path -LiteralPath $render) {
    # A directory that has nothing of ours in it, so the working-directory branch of the core's
    # lookup cannot accidentally succeed. The render turns both models on: they are off by default,
    # so without the --set neither .phosmdl would be opened at all and the run would say nothing
    # about them.
    $tmp = Join-Path ([IO.Path]::GetTempPath()) ("phos-pkg-" + [Guid]::NewGuid().ToString("N").Substring(0, 8))
    New-Item -ItemType Directory -Force $tmp | Out-Null
    $got = Join-Path $tmp "staged.wav"
    Push-Location $tmp
    try {
        & $render --bars 64 --set "compose.melody_model=Neural compose.bass_model=Neural" --out $got | Out-Null
        $rc = $LASTEXITCODE
    } finally { Pop-Location }
    if ($rc -ne 0) {
        Fail "the staged phos_render.exe returned $rc"
    } else {
        $h1 = (Get-FileHash -LiteralPath $got -Algorithm SHA256).Hash
        $h2 = (Get-FileHash -LiteralPath $Reference -Algorithm SHA256).Hash
        if ($h1 -ne $h2) {
            Fail ("the staged renderer does not render the reference: $($h1.Substring(0,16)) against $($h2.Substring(0,16)). " +
                  "A shipping build has no source tree to fall back on, so the first thing to look at is the staged " +
                  "data itself -- a missing, stale or truncated library.phoswt or .phosmdl beside the binary, which " +
                  "checks A, B and H name. After that: a staged binary from an older build, a half-finished copy, " +
                  "or a Release configuration that does not render what the tested one renders.")
        } else {
            Ok ("64 bars with both neural models, rendered from ${tmp}: identical to the reference ($($h1.Substring(0,16)))")
        }
    }
    Remove-Item -Recurse -Force $tmp -ErrorAction SilentlyContinue
}

# ---------------------------------------------------------------- F2: the path into somebody's disk
# A shipping binary must not carry an absolute path into the machine that built it. Here it is not
# only untidy, it is a live code path: PHOS_SOURCE_DATA_DIR is the last step of the core's lookup for
# library.phoswt and the two models (Core/CMakeLists.txt), so on the build machine a binary with a
# missing data file quietly reads the source tree instead and looks perfectly healthy -- which is
# why check F above could not see a missing data file at all.
#
# **A failure, not a warning, since 16.09.2026.** It was reported and tolerated while the fix was
# somebody else's to make; the fix now exists (configure with -DPHOS_SHIP=ON, which
# Deploy/build_release.ps1 does), so a package that still embeds the path is a package built the
# wrong way, and check F is measuring nothing. A warning here would be a check that cannot fail,
# which this file has already learned once is worse than no check at all -- see the note about the
# two separators below.
Write-Host "F2   whether the binaries still carry the build machine's source path"
# Both separators. CMake writes the define with forward slashes -- "G:/.../Core/data" -- and the
# first version of this check looked for the backslash spelling only and cheerfully reported that
# nothing was embedded. A check that cannot fail is worse than no check, so it is spelled out here.
$needles = @((Join-Path $Root "Core\data"), ((Join-Path $Root "Core\data") -replace '\\', '/'))
foreach ($rel in @("Phosphene.exe", "phos_render.exe", "Phosphene.vst3\Contents\x86_64-win\Phosphene.vst3")) {
    $p = Join-Path $Stage $rel
    if (-not (Test-Path -LiteralPath $p)) { continue }
    $bytes = [IO.File]::ReadAllBytes($p)
    # Both encodings too: a narrow string literal, and a UTF-16 one should anything widen it.
    $ascii = [Text.Encoding]::ASCII.GetString($bytes)
    $wide = [Text.Encoding]::Unicode.GetString($bytes)
    $hit = $needles | Where-Object { $ascii.Contains($_) -or $wide.Contains($_) } | Select-Object -First 1
    if ($hit) { Fail ("$rel embeds $hit -- built without -DPHOS_SHIP=ON, so check F above proves nothing: this binary reads the source tree whatever is staged beside it") }
    else { Ok "$rel carries no path into the source tree" }
}

# ---------------------------------------------------------------- G: the manual
Write-Host "G    the manual"
# Whether the manual is *complete* is settled before this: the generator refuses to print at all
# when a parameter appears on no tab (Tools/manual/make_manual.py), and Deploy/build_release.ps1
# checks the version the running plugin wrote into manual.json. What is left to prove here is that
# what got staged is a PDF and not, say, a zero-byte file left behind by a browser that never
# finished printing -- which is a real failure of that step and looks like success in a log.
$pdf = Join-Path $Stage "Phosphene-Manual.pdf"
if (Test-Path -LiteralPath $pdf) {
    $head = [byte[]]::new(5)
    $fs = [IO.File]::OpenRead($pdf)
    try { [void]$fs.Read($head, 0, 5) } finally { $fs.Dispose() }
    if ([Text.Encoding]::ASCII.GetString($head) -ne "%PDF-") { Fail "Phosphene-Manual.pdf does not start with %PDF- -- the browser did not print it" }
    else { Ok ("Phosphene-Manual.pdf  {0:N1} MB, a real PDF" -f ((Get-Item -LiteralPath $pdf).Length / 1MB)) }
}

# ---------------------------------------------------------------- H: every place the runtime looks
# The two learned models of Phase 8 are opened by bare name, and since 16.09.2026 the only places
# they are found are the ones a host points the core at -- the plugin and the renderer at their own
# directory (Plugin/PluginProcessor.cpp, resolveResourceDirectory; Tools/render/main.cpp,
# installDataSearchPath) and the Quest app at its unpacked assets (Quest/src/main.cpp,
# prepareModels). PHOS_SOURCE_DATA_DIR used to be a fourth, and is no longer one; F2 above is what
# keeps it that way.
#
# So the list below is the runtime's lookup written out as a payload requirement. A and B already
# hash two of these entries and C the third; this section exists so that the *set* is stated in one
# place: if a fourth surface ever learns to find the models, it belongs here, and if one of the three
# quietly stops being filled, this says which runtime lost Phase 8 rather than which file is missing.
Write-Host "H    both learned models in every place a runtime looks"
$modelPlaces = @(
    @{ Runtime = "Phosphene.exe, phos_render.exe (beside the binary)"; Path = ""; Kind = "stage" }
    @{ Runtime = "Phosphene.vst3 (the bundle's Contents\Resources)";   Path = "Phosphene.vst3\Contents\Resources"; Kind = "stage" }
    @{ Runtime = "PhospheneQuest.apk (unpacked on the first start)";   Path = "assets/"; Kind = "apk" }
)
foreach ($place in $modelPlaces) {
    foreach ($f in @("melody.phosmdl", "bass.phosmdl")) {
        $want = (Get-Item -LiteralPath (Join-Path $data $f)).Length
        if ($place.Kind -eq "apk") {
            if (-not (Test-Path -LiteralPath $apk)) { continue }
            $key = $place.Path + $f
            if (-not $apkAssets.ContainsKey($key)) { Fail ("{0}: no {1} -- that runtime loses Phase 8 and says so to nobody" -f $place.Runtime, $f) }
            elseif ($apkAssets[$key][0] -ne $want) { Fail ("{0}: {1} is {2:N0} bytes, not the {3:N0} of Core/data" -f $place.Runtime, $f, $apkAssets[$key][0], $want) }
            else { Ok ("{0}: {1}" -f $place.Runtime, $f) }
        } else {
            $p = if ($place.Path) { Join-Path (Join-Path $Stage $place.Path) $f } else { Join-Path $Stage $f }
            if (-not (Test-Path -LiteralPath $p)) { Fail ("{0}: no {1} -- that runtime loses Phase 8 and says so only on stderr" -f $place.Runtime, $f) }
            elseif ((Get-Item -LiteralPath $p).Length -ne $want) { Fail ("{0}: {1} is not the {2:N0} bytes of Core/data" -f $place.Runtime, $f, $want) }
            else { Ok ("{0}: {1}" -f $place.Runtime, $f) }
        }
    }
}

# ---------------------------------------------------------------- the manifest
$rows = Get-ChildItem -LiteralPath $Stage -Recurse -File | Sort-Object FullName | ForEach-Object {
    [PSCustomObject]@{
        Path   = $_.FullName.Substring($Stage.Length).TrimStart('\')
        Bytes  = $_.Length
        SHA256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLower()
    }
}
$total = ($rows | Measure-Object -Property Bytes -Sum).Sum
Write-Host ""
Write-Host ("manifest: {0} files, {1:N1} MB" -f $rows.Count, ($total / 1MB)) -ForegroundColor Cyan
$rows | ForEach-Object { Write-Host ("  {0}  {1,12:N0}  {2}" -f $_.SHA256.Substring(0, 16), $_.Bytes, $_.Path) }
if ($Manifest) {
    New-Item -ItemType Directory -Force (Split-Path -Parent $Manifest) | Out-Null
    $out = New-Object System.Collections.Generic.List[string]
    $out.Add("Phosphene $Version -- package manifest")
    $out.Add("built  $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss')")
    $out.Add("files  $($rows.Count), $('{0:N0}' -f $total) bytes")
    $out.Add("")
    foreach ($r in $rows) { $out.Add(("{0}  {1,12}  {2}" -f $r.SHA256, $r.Bytes, $r.Path)) }
    [IO.File]::WriteAllLines($Manifest, $out)
    Write-Host "manifest written: $Manifest"
}

Write-Host ""
if ($failures.Count -gt 0) {
    Write-Host ("package check FAILED: {0} problem(s)" -f $failures.Count) -ForegroundColor Red
    foreach ($f in $failures) { Write-Host "  - $f" -ForegroundColor Red }
    exit 1
}
Write-Host "package check passed" -ForegroundColor Green
exit 0
