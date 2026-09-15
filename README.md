# Phosphene

A generator for complete psytrance sets: it composes and synthesizes kick, bass, percussion, acid,
leads, arpeggios, pads and effects from a seed, a style profile and an energy arc, and exports every
note line as MIDI. Standalone and VST3 on Windows, native on Meta Quest 2. Everything is synthesized;
there are no samples.

The design and the literature behind each building block are in [docs/PLAN.md](docs/PLAN.md) (German).

**Status:** Phases 0 to 4 are done: framework, kick and rolling bass, a twelve-lane percussion kit,
the melodic layer (an acid voice on a diode ladder with accent, slide and squelch; a polyphonic
supersaw/VA/FM/wavetable engine for lead, arp and pads; chords, riffs, phrases and arps drawn from
statistics of a local MIDI corpus under musical constraints), and the space and the master: wavetable
pads with voice-led chords and a trance gate, synthesised effects (risers, impacts, formant shots,
reverse swells), kick sidechain on every channel, a room and a hall, and a master of bus compressor,
mono bass, soft clipper and true-peak limiter that meets a loudness target. The offline renderer
composes sets of any length; each track has its own key, tempo, patterns, groove, chords, melodies and
sound, levels are matched between tracks and parts, and kick and bass are phase-locked at the first
bass note. The headset build is there (below); no song form grammar and no plugin yet.

## Build

Visual Studio 2026 and CMake 3.22 or newer:

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

## Try it

```bash
build/Tools/render/Release/phos_render.exe --bars 32 --out out/loop.wav --midi out/loop.mid --report
```

```bash
build/Tools/render/Release/phos_render.exe --bars 32 --set "compose.bass_pattern=Triplet compose.key=A compose.scale=Double Harmonic kick.engine=Resonant" --out out/goa.wav
```

```bash
build/Tools/render/Release/phos_render.exe --minutes 30 --seed 2026 --tracks --report --out out/night.wav --midi out/night.mid
```

`--tracks` prints each track's key, tempo, patterns, sound recipe, chords, melodic parts per 16-bar
block, the effects and level corrections. `--solo acid` (or kick, bass, perc, lead, arp, pad, sfx) mutes
everything else. `PHOS_ONLY=testDynamics` runs only the named self-test sections. `--list`
prints every parameter. `python Tools/inspect_wav.py out/loop.wav --bpm 145` draws the
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

`phos_selftest` measures every building block against independently derived values: the ladders'
analytic responses (the diode ladder against Zavalishin's transfer function and its self-oscillation
at k = 17), the supersaw against Szabo's JP-8000 tables, FM sidebands against Bessel functions, the
constrained Markov sampler against brute-force enumeration, pad voicings against exhaustive search, the
compressor against Giannoulis et al., the limiter's output against an exact band-limited true peak, the
hall's decay time, an eight-minute track against the loudness target, the tempo integral, the half-band stopband, oscillator aliasing, the kick's pitch
sweep, sample-accurate event timing, bit-identical output across block sizes, the MIDI round trip and
BS.1770 loudness. `phos_vectest` runs in three builds (AVX2, NEON through an x86 shim, scalar) and
requires every lane of the vectorised DSP to equal the scalar computation bit for bit.

## Layout

| Path | Contents |
|---|---|
| `Core/` | framework-free engine, `phos::` namespace |
| `Quest/` | the Meta Quest app: OpenXR, GLES 3, Oboe, no Gradle |
| `Tools/render/` | `phos_render`: offline render, MIDI export, benchmark |
| `Tools/inspect_wav.py` | pictures and measurements of a render |
| `Tools/ref_*.py` | measurements of reference recordings: bass slots, percussion grid, band balance, sweeps |
| `Tools/corpus/` | `build_corpus.py`: melodic statistics from a local MIDI corpus (the MIDI files stay local), memorisation check |
| `Tests/` | self test, vector-path tests, NEON shim |
| `docs/` | plan, Doxygen configuration |

Some modules are copied from [Noctuary](https://github.com/reneweller-coding/Noctuary) and name their
origin and commit in their file header.

## Licence

AGPL-3.0, as Noctuary.
