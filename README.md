# Phosphene

A generator for complete psytrance sets: it composes and synthesizes kick, bass, percussion, acid,
leads, arpeggios, pads and effects from a seed, a style profile and an energy arc, and exports every
note line as MIDI. Standalone and VST3 on Windows, native on Meta Quest 2. Everything is synthesized;
there are no samples.

The design and the literature behind each building block are in [docs/PLAN.md](docs/PLAN.md) (German).

**Status:** Phases 0 to 7 are done: framework, kick and rolling bass, a twelve-lane percussion kit,
the melodic layer (an acid voice on a diode ladder with accent, slide and squelch; a polyphonic
supersaw/VA/FM/wavetable engine for lead, arp and pads; chords, riffs, phrases and arps drawn from
statistics of a local MIDI corpus under musical constraints), the space and the master (wavetable pads
with voice-led chords and a trance gate, synthesised effects, kick sidechain on every channel, a room
and a hall, bus compressor, mono bass, soft clipper and true-peak limiter on a loudness target), and
the composer: every track is built from a weighted grammar over intro, groove, buildup, pre-drop break,
drop, breakdown, cut and outro, an energy arc over the whole set moves loudness, density, register and
dissonance, five style profiles weight everything from tempo to chord moves, transitions between tracks
are written rather than mixed, and any unit of a set can be locked or rerolled and saved as a
`.phosset`. A sixty-minute set comes out of one seed as audio and as a Standard MIDI File. Phase 6 adds the plugin: a
VST3 and a standalone with twelve tabs of controls generated from the parameter tables, an arrange
timeline of the whole set with a lock and a reroll on every track and every section, four perform
macros, the composer on a thread of its own, host transport and tempo, MIDI output of the score, and a
recorder; the standalone renders exactly what the offline renderer renders, sample for sample. The
headset build is there (below). Phase 9 adds the release path: one script from a checkout to an
installer, a package check that refuses an incomplete payload, and a build guard in `ctest` for the
two artefacts nothing else builds (see [Install](#install) and [Tests](#tests)).

## Build

Visual Studio 2026 and CMake 3.22 or newer:

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

That builds one of six configurations this repository has. Before a release, or after anything in
`Core/` changes shape, build them all:

```powershell
powershell -File Tools\release\build_matrix.ps1
```

It configures and builds the desktop with and without the plugin, without AVX2, with the static
runtime, the root project through the Android NDK, and the Quest app itself, runs the three vector
builds, and prints a table of what came out and what each entry cost. Five minutes from empty build
directories on a 12900K, most of it the one entry that has JUCE in it. Nothing else builds the Quest
app or the Android configure, which is how both of them broke unnoticed once; the cheap half of that
check runs in every `ctest` as `questguard` (below).

### Plugin

The VST3 and the standalone are built with the rest and need JUCE 9.0.1, which CMake fetches from
GitHub on the first configure. To use a checkout you already have, copy it to `ThirdParty/JUCE`
(ignored by git) and it is taken from there. `-DPHOS_BUILD_PLUGIN=OFF` builds the tools alone.

```bash
cmake --build build --config Release --target Phosphene_Standalone Phosphene_VST3
build/Plugin/Phosphene_artefacts/Release/Standalone/Phosphene.exe
```

The VST3 is `build/Plugin/Phosphene_artefacts/Release/VST3/Phosphene.vst3`; copy it to
`C:\Program Files\Common Files\VST3`. In a host the playhead is the clock -- tempo and position
come from the transport, a jump is followed to the bar -- and the score's notes leave the plugin as
MIDI, one channel per part. The standalone has its own clock, a play and stop button, a loudness
meter, the plan of the set, a recorder and the exports.

Environment variables, for tests and documentation: `PHOS_MUTE=1` starts the standalone silent and it
never unmutes itself; `PHOS_SHOT=<file.png>` renders the editor at design size into a PNG and exits
(`PHOS_TAB=<index>` picks the tab, `PHOS_SHOT_ALL=<folder>` writes one picture per tab, see
[docs/screenshots](docs/screenshots)); `PHOS_SHOT_WAIT=<seconds>` is how long the composer is given to
plan before the pictures are taken, which the arrange timeline needs (26 seconds plans a whole
sixty-minute set); `PHOS_MANUAL=<folder>` writes those pictures plus a `manual.json` of the parameter
tables; `PHOS_PLAY=<seconds>` with `PHOS_RECORD=<file.wav>` plays for a while, records, and exits;
`PHOS_TRACE=1` makes the plugin report its transport and its composer on stderr, which is the way to
see inside it when a host has loaded it and it is silent.

### Manual

The manual is generated from the plugin itself -- the parameter tables and the groups each tab really
built, read back out of the running editor, plus one picture per tab -- with the prose in
`Tools/manual/chapters.txt`:

```bash
PHOS_MANUAL=docs/screenshots PHOS_SHOT_WAIT=26 build/.../Phosphene.exe
python Tools/manual/make_manual.py
```

That writes [docs/manual/Phosphene-Manual.html](docs/manual/Phosphene-Manual.html) and, through Edge
in headless mode, the PDF beside it. The generator refuses to call a manual complete when a parameter
exists in the engine but appears on no tab.

## Install

Releases are made by one script, from a clean checkout to the artefacts:

```powershell
powershell -File Deploy\build_release.ps1
```

It runs the build guard, configures and builds Release with the static MSVC runtime and AVX2 in a
tree of its own, runs the whole `ctest` suite in that configuration, renders a reference, prints the
manual out of the freshly built plugin, builds the Quest APK, stages everything into `Deploy\stage`,
checks the staging directory, and only then compiles the installer and the portable archive into
`Deploy\out`. Every step that fails stops the run; there is no switch that makes an installer out of
a build that did not pass its tests. It prints a manifest of every file with its size and SHA-256.

The package check ([`Tools/release/check_package.ps1`](Tools/release/check_package.ps1)) is what
stands between a green build and a bad release. It proves that every file the runtime needs is
there, that each data file is byte for byte the one in `Core/data`, that the APK really carries the
wavetable pack, that the version is the same in the renderer and in both Windows version resources,
that no binary still wants a Visual C++ runtime DLL, and — the one that covers the whole data path
at once — that the *staged* renderer, started from an unrelated directory and with both neural
models switched on, renders the reference bit for bit.

The installer puts the standalone, `phos_render.exe`, the manual, the APK and the data files into
`%ProgramFiles%\Phosphene`, and the VST3 bundle into the common VST3 folder. `library.phoswt`,
`melody.phosmdl` and `bass.phosmdl` are installed twice, beside the executables and inside the
bundle's `Contents\Resources`, because those are the two directories the artefacts declare as their
search path; without them the engine falls back to six built-in wavetables and to the Markov
composer and says so only on stderr. An update deletes the previous copies before installing rather
than writing over them, so a file that was renamed or dropped cannot linger.

Nothing built here is code-signed — there is no certificate on this machine — so Windows calls the
publisher unknown on first run. The manifest's hashes are what can be checked instead.

## Try it

```bash
build/Tools/render/Release/phos_render.exe --bars 32 --out out/loop.wav --midi out/loop.mid --report
```

```bash
build/Tools/render/Release/phos_render.exe --bars 32 --set "compose.bass_pattern=Triplet compose.key=A compose.scale=Double Harmonic kick.engine=Resonant" --out out/goa.wav
```

```bash
build/Tools/render/Release/phos_render.exe --minutes 60 --seed 2026 --tracks --sections --report \
    --set "compose.style=Goa compose.style_tempo=On compose.arc=Peak-Time compose.set_minutes=60" \
    --out out/night.wav --midi out/night.mid --save-set out/night.phosset
```

`--tracks` prints each track's key, tempo, patterns, sound recipe, chords, form, effects, level
corrections and, after the render, the loudness it really played; `--sections` lists every section of
the set. `--set-file night.phosset` plays a saved set again, `--lock track:3` and `--reroll track:5`
curate it (the units are set, track, section and lane). `--solo acid` (or kick, bass, perc, lead, arp,
pad, sfx) mutes everything else. `PHOS_ONLY=testDynamics` runs only the named self-test sections.
`--list` prints every parameter. `python Tools/inspect_wav.py out/loop.wav --bpm 145` draws the
render (waveform, one beat, spectrogram) and prints where in the beat the sub band is occupied.

## Meta Quest

The whole generator runs on the headset: the composer plans the set on a small core, the engine
synthesizes it on a big one, and the hands play it — pinch left for play/stop, pinch right for the
next track, left hand height is the track gain and right hand height the acid cutoff. Native
OpenXR, no game engine. Build and on-device checks: [Quest/README.md](Quest/README.md).

```powershell
powershell -File Quest\fetch_thirdparty.ps1
powershell -File Quest\build_apk.ps1
adb install -r build-quest\PhospheneQuest.apk
```

`Quality::Quest` (`Core/include/phos/Quality.h`) is what the engine may spend there: the acid's
ladder at 1× instead of 2× (the bass keeps its oversampling), three unison oscillators instead of
seven, four pad voices instead of eight. `Engine::prepare` takes the level and defaults to Desktop,
so nothing else changes; `phos_render --quality quest` renders and benchmarks at it. On the desktop
the Quest level costs 16.9 % less for an eight-minute set with every part (7.2 % of a core against
6.0 %); **the number on the device is still open — no headset has been attached yet.**

## Tests

```bash
ctest --test-dir build -C Release
```

`questguard` is the cheapest test in the suite and the only one that looks outside the desktop build:
it configures the root project for Android with its **default** options, asserts that the JUCE plugin
is off there (JUCE cannot cross-compile the host-side helper it needs, and its error names the
compiler rather than the cause), and compiles every `Quest/src/*.cpp` with the NDK's own clang in
syntax-only mode. Nothing else in `ctest` builds those; `Quest/src/main.cpp` had not compiled since
Phase 5 before this existed. Five seconds on this machine, and it reports itself as skipped, with the
reason, on a machine without the NDK or without `ThirdParty` rather than failing.

`phos_hosttest` measures the plugin around the engine: rates and block sizes no one develops at,
blocks that change size in the middle of a set, parameters written from another thread, a transport
that starts, jumps and stops, a state that comes back exactly as it went out, and every tab laid out
and painted. Its oracle is the offline renderer: with its own clock the plugin has to produce the
same samples `phos_render` produces, bit for bit.

`phos_vst3test` goes one step further and loads the **built VST3 off the disk**, the way a DAW loads
it: the module, the factory, an instance, the parameter list, a transport, the MIDI it produces, the
state, the editor, and the teardown. That is the part of pluginval that can live in the repository.
pluginval itself is not in the repository (it is a binary); fetched from Tracktion's releases and run
as `pluginval --strictness-level 10 --timeout-ms 900000 --validate <the .vst3>`, version 1.0.4 passes
all 25 of its test sections.

`phos_selftest` measures every building block against independently derived values: the ladders'
analytic responses (the diode ladder against Zavalishin's transfer function and its self-oscillation
at k = 17), the supersaw against Szabo's JP-8000 tables, FM sidebands against Bessel functions, the
constrained Markov sampler against brute-force enumeration, pad voicings against exhaustive search, the
compressor against Giannoulis et al., the limiter's output against an exact band-limited true peak, the
hall's decay time, an eight-minute track against the loudness target, the tempo integral, the half-band
stopband, oscillator aliasing, the kick's pitch sweep, sample-accurate event timing, bit-identical
output across block sizes, the MIDI round trip and BS.1770 loudness. The form is measured on a rendered
track against the findings of Solberg and Dibben (2019): the breakdown at least 6 dB under the core
before it, the buildup rising over every four-bar window, the kick-and-bass band 20 dB down in the
breakdown, beat 4 of the pre-drop break 30 dB under a core beat, and the drop at least as loud and as
bright as what stood before the break. `phos_vectest` runs in three builds (AVX2, NEON through an x86 shim, scalar) and
requires every lane of the vectorised DSP to equal the scalar computation bit for bit.

## Layout

| Path | Contents |
|---|---|
| `Core/` | framework-free engine, `phos::` namespace |
| `Quest/` | the Meta Quest app: OpenXR, GLES 3, Oboe, no Gradle |
| `Tools/render/` | `phos_render`: offline render, MIDI export, benchmark |
| `Tools/inspect_wav.py` | pictures and measurements of a render |
| `Tools/ref_*.py` | measurements of reference recordings: bass slots, percussion grid, band balance, sweeps, tempo per style and the shape of a break |
| `Tools/corpus/` | `build_corpus.py`: melodic statistics from a local MIDI corpus (the MIDI files stay local), memorisation check |
| `Plugin/` | JUCE 9 VST3 and standalone: processor, editor, layout engine, arrange timeline, perform macros |
| `Tools/manual/` | `make_manual.py` and the manual's prose: HTML and PDF out of the plugin's own tables |
| `Tools/release/` | the build guard (`quest_guard.cmake`), the target matrix and the package check |
| `Deploy/` | `build_release.ps1`, the Inno Setup script, the icon; `stage/` and `out/` are made by the script |
| `Tests/` | self test, host test, VST3 test, vector-path tests, NEON shim, the build guard |
| `docs/screenshots/` | one picture per tab of the editor |
| `docs/` | plan, Doxygen configuration |

Some modules are copied from [Noctuary](https://github.com/reneweller-coding/Noctuary) and name their
origin and commit in their file header.

## Licence

AGPL-3.0, as Noctuary.
