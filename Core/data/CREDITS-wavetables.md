# Wavetables shipped with Phosphene

`Core/data/library.phoswt` holds 35 tables, chosen by measurement from the 2191-table wavetable
library of the sibling project Noctuary (`G:\Tools\VRAudio\AmbientSynth\Library\Wavetables`)
by `Tools/wt_select.py`. Frames of 2048 samples; the pack carries each frame's Fourier
coefficients rather than its samples (`Core/include/phos/WaveTableFile.h`).

## AKWF -- Adventure Kid Waveforms, Kristoffer Ekstrand

<https://github.com/KristofferKarlAxelEkstrand/AKWF-FREE> (8de90bf, 2025-12-04)
CC0 1.0 Universal. Single cycles of 600 samples, ordered into morph tables and aligned by
Noctuary's `Tools/WavetableLib/build_classic.py`.

## WaveEdit Online -- the banks of the WaveEdit users (Synthesis Technology E352/E370)

<https://github.com/smpldsnds/wavedit-online> (1a8d80f, 2023-10-07)
CC0 1.0 Universal. Order and phases as in the original, cycles brought from 256 to 2048
samples by the same script.

CC0 asks for no attribution; it stands here all the same, as it does in Noctuary's own
`Library/Wavetables/Classic/CREDITS-classic.md`.

## Procedurally generated -- Noctuary `HarmonicGen` / `AmbientGen`

`Tools/WavetableGen` writes these from a recipe and a seed: partial envelopes, chord and
formant families, optimal-transport morphs between spectra. No external material of any
kind enters them; the sidecar of such a table names a generator and a seed and no source.

## Measured families -- `ambient_sampled` and its relatives

These are spectral analyses of the user's own material: single notes generated with Stable
Audio 3 medium by the user's own pipeline (`G:\Tools\VRAudio\StableAudio3`) under the
Stability Community License, which assigns the outputs to the user. What ships here is not
audio but the harmonic envelopes measured from it -- the same relation a wavetable has to
the instrument it was drawn from. The Stability Community License is revenue-capped:
commercial use is free below one million US dollars of annual revenue.

## The tables

| # | Table | Lane | Library id | Provenance |
|---|---|---|---|---|
| 6 | WaveEdit Hyperbol | pad | `Classic/wavedit_hyperbol` | CC0: WaveEdit Online |
| 7 | Sampled 210 | pad | `Ambient/ambient_sampled_210` | measured from the user's own SA3 material |
| 8 | WaveEdit Sohler52 | pad | `Classic/wavedit_sohler52` | CC0: WaveEdit Online |
| 9 | Organ 034 | pad | `Harmonic/harmonic_organ_034` | generated: harmonicgen |
| 10 | Otmorph 069 | pad | `Ambient/ambient_otmorph_069` | generated: ambientgen |
| 11 | WaveEdit Vocal_fo | pad | `Classic/wavedit_vocal_fo` | CC0: WaveEdit Online |
| 12 | WaveEdit Ppg_wa04 | pad | `Classic/wavedit_ppg_wa04` | CC0: WaveEdit Online |
| 13 | Sampled 214 | pad | `Ambient/ambient_sampled_214` | measured from the user's own SA3 material |
| 14 | Glass 003 | pad | `Harmonic/harmonic_glass_003` | generated: harmonicgen |
| 15 | WaveEdit Organ_di | pad | `Classic/wavedit_organ_di` | CC0: WaveEdit Online |
| 16 | AKWF hdrawn-01 | pad | `Classic/akwf_hdrawn_01` | CC0: AKWF |
| 17 | WaveEdit Qux_fmy | pad | `Classic/wavedit_qux_fmy` | CC0: WaveEdit Online |
| 18 | WaveEdit Hienharm | lead | `Classic/wavedit_hienharm` | CC0: WaveEdit Online |
| 19 | WaveEdit Junox_ho | lead | `Classic/wavedit_junox_ho` | CC0: WaveEdit Online |
| 20 | WaveEdit Euclidea | lead | `Classic/wavedit_euclidea` | CC0: WaveEdit Online |
| 21 | WaveEdit Sohler49 | lead | `Classic/wavedit_sohler49` | CC0: WaveEdit Online |
| 22 | WaveEdit Pwn_saw | lead | `Classic/wavedit_pwn_saw` | CC0: WaveEdit Online |
| 23 | WaveEdit Tidyb030 | lead | `Classic/wavedit_tidyb030` | CC0: WaveEdit Online |
| 24 | WaveEdit Tezzalog | lead | `Classic/wavedit_tezzalog` | CC0: WaveEdit Online |
| 25 | WaveEdit Sine_n | lead | `Classic/wavedit_sine_n` | CC0: WaveEdit Online |
| 26 | Consonant 129 | arp | `Ambient/ambient_consonant_129` | generated: ambientgen |
| 27 | AKWF 0004-hollow-01 | arp | `Classic/akwf_0004_hollow_01` | CC0: AKWF |
| 28 | WaveEdit Pd104 | arp | `Classic/wavedit_pd104` | CC0: WaveEdit Online |
| 29 | WaveEdit Crush_ad | arp | `Classic/wavedit_crush_ad` | CC0: WaveEdit Online |
| 30 | WaveEdit Micro_q | arp | `Classic/wavedit_micro_q` | CC0: WaveEdit Online |
| 31 | AKWF oscchip-04 | arp | `Classic/akwf_oscchip_04` | CC0: AKWF |
| 32 | AKWF 0014-hollow-01 | arp | `Classic/akwf_0014_hollow_01` | CC0: AKWF |
| 33 | Vowel Bass 026 | arp | `Ambient/ambient_vowel_bass_026` | generated: ambientgen |
| 34 | Sub 003 | drone | `Harmonic/harmonic_sub_003` | generated: harmonicgen |
| 35 | WaveEdit Sohler79 | drone | `Classic/wavedit_sohler79` | CC0: WaveEdit Online |
| 36 | Tube 002 | drone | `Ambient/ambient_tube_002` | generated: ambientgen |
| 37 | Consonant 008 | drone | `Ambient/ambient_consonant_008` | generated: ambientgen |
| 38 | WaveEdit Ppg_wa03 | drone | `Classic/wavedit_ppg_wa03` | CC0: WaveEdit Online |
| 39 | Pluck 028 | drone | `Harmonic/harmonic_pluck_028` | generated: harmonicgen |
| 40 | Vowel Alto 012 | drone | `Ambient/ambient_vowel_alto_012` | generated: ambientgen |
