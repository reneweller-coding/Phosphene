# Phosphene

A generator for complete psytrance sets: it composes and synthesizes kick, bass, percussion, acid,
leads, arpeggios, pads and effects from a seed, a style profile and an energy arc, and exports every
note line as MIDI. Standalone and VST3 on Windows, native on Meta Quest 2. Everything is synthesized;
there are no samples.

The design and the literature behind each building block are in [docs/PLAN.md](docs/PLAN.md) (German).

**Status:** Phase 0 (framework) and Phase 1 (kick, rolling bass, clock) are done. The offline
renderer plays a composed kick-and-bass loop in five bass patterns and exports it as MIDI. No plugin
or headset build yet.

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

`--list` prints every parameter. `python Tools/inspect_wav.py out/loop.wav --bpm 145` draws the
render (waveform, one beat, spectrogram) and prints where in the beat the sub band is occupied.

## Tests

```bash
ctest --test-dir build -C Release
```

`phos_selftest` measures every building block against independently derived values: the ladder's
analytic response, the tempo integral, the half-band stopband, oscillator aliasing, the kick's pitch
sweep, sample-accurate event timing, bit-identical output across block sizes, the MIDI round trip and
BS.1770 loudness. `phos_vectest` runs in three builds (AVX2, NEON through an x86 shim, scalar) and
requires every lane of the vectorised DSP to equal the scalar computation bit for bit.

## Layout

| Path | Contents |
|---|---|
| `Core/` | framework-free engine, `phos::` namespace |
| `Tools/render/` | `phos_render`: offline render, MIDI export, benchmark |
| `Tools/inspect_wav.py` | pictures and measurements of a render |
| `Tests/` | self test, vector-path tests, NEON shim |
| `docs/` | plan, Doxygen configuration |

Some modules are copied from [Noctuary](https://github.com/reneweller-coding/Noctuary) and name their
origin and commit in their file header.

## Licence

AGPL-3.0, as Noctuary.
