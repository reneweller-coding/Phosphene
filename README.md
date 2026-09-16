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
bass note. Phase 6 adds the plugin: a VST3 and a standalone with ten tabs of controls generated from
the parameter tables, the composer on a thread of its own, host transport and tempo, MIDI output of
the score, and a recorder -- the standalone renders exactly what the offline renderer renders, sample
for sample. No song form grammar or headset build yet.

## Build

Visual Studio 2026 and CMake 3.22 or newer:

```bash
cmake -S . -B build -G "Visual Studio 18 2026" -A x64
cmake --build build --config Release
```

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
[docs/screenshots](docs/screenshots)); `PHOS_PLAY=<seconds>` with `PHOS_RECORD=<file.wav>` plays for a
while, records, and exits; `PHOS_TRACE=1` makes the plugin report its transport and its composer on
stderr, which is the way to see inside it when a host has loaded it and it is silent.

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

## Tests

```bash
ctest --test-dir build -C Release
```

`phos_hosttest` measures the plugin around the engine: rates and block sizes no one develops at,
blocks that change size in the middle of a set, parameters written from another thread, a transport
that starts, jumps and stops, a state that comes back exactly as it went out, and every tab laid out
and painted. Its oracle is the offline renderer: with its own clock the plugin has to produce the
same samples `phos_render` produces, bit for bit.

`phos_vst3test` goes one step further and loads the **built VST3 off the disk**, the way a DAW loads
it: the module, the factory, an instance, the parameter list, a transport, the MIDI it produces, the
state, the editor, and the teardown. That is the part of pluginval that can live in the repository.

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
| `Tools/render/` | `phos_render`: offline render, MIDI export, benchmark |
| `Tools/inspect_wav.py` | pictures and measurements of a render |
| `Tools/ref_*.py` | measurements of reference recordings: bass slots, percussion grid, band balance, sweeps |
| `Tools/corpus/` | `build_corpus.py`: melodic statistics from a local MIDI corpus (the MIDI files stay local), memorisation check |
| `Plugin/` | JUCE 9 VST3 and standalone: processor, editor, layout engine |
| `Tests/` | self test, host test, vector-path tests, NEON shim |
| `docs/screenshots/` | one picture per tab of the editor |
| `docs/` | plan, Doxygen configuration |

Some modules are copied from [Noctuary](https://github.com/reneweller-coding/Noctuary) and name their
origin and commit in their file header.

## Licence

AGPL-3.0, as Noctuary.
