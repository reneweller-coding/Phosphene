# Wavetables shipped with Phosphene

`Core/data/library.phoswt` holds 464 tables, chosen by measurement from the 2191-table wavetable
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
Audio 3 medium by the user's own pipeline (`G:\Tools\VRAudio\_Sources\StableAudio3`) under the
Stability Community License, which assigns the outputs to the user. What ships here is not
audio but the harmonic envelopes measured from it -- the same relation a wavetable has to
the instrument it was drawn from. The Stability Community License is revenue-capped:
commercial use is free below one million US dollars of annual revenue.

## The tables

| # | Table | Lane | Library id | Provenance |
|---|---|---|---|---|
| 6 | WaveEdit Hienharm | lead | `Classic/wavedit_hienharm` | CC0: WaveEdit Online |
| 7 | WaveEdit Junox_ho | lead | `Classic/wavedit_junox_ho` | CC0: WaveEdit Online |
| 8 | WaveEdit Euclidea | lead | `Classic/wavedit_euclidea` | CC0: WaveEdit Online |
| 9 | WaveEdit Sohler49 | lead | `Classic/wavedit_sohler49` | CC0: WaveEdit Online |
| 10 | WaveEdit Vocal_fo | lead | `Classic/wavedit_vocal_fo` | CC0: WaveEdit Online |
| 11 | WaveEdit Pwn_saw | lead | `Classic/wavedit_pwn_saw` | CC0: WaveEdit Online |
| 12 | WaveEdit Tidyb030 | lead | `Classic/wavedit_tidyb030` | CC0: WaveEdit Online |
| 13 | WaveEdit I_heart | lead | `Classic/wavedit_i_heart` | CC0: WaveEdit Online |
| 14 | WaveEdit Sine_n | lead | `Classic/wavedit_sine_n` | CC0: WaveEdit Online |
| 15 | WaveEdit Vocal_00 | lead | `Classic/wavedit_vocal_00` | CC0: WaveEdit Online |
| 16 | WaveEdit Supersaw | lead | `Classic/wavedit_supersaw` | CC0: WaveEdit Online |
| 17 | WaveEdit Tidyb060 | lead | `Classic/wavedit_tidyb060` | CC0: WaveEdit Online |
| 18 | Consonant 024 | lead | `Ambient/ambient_consonant_024` | generated: ambientgen |
| 19 | WaveEdit Synlp92 | lead | `Classic/wavedit_synlp92` | CC0: WaveEdit Online |
| 20 | WaveEdit Distorte | lead | `Classic/wavedit_distorte` | CC0: WaveEdit Online |
| 21 | WaveEdit Fourier2 | lead | `Classic/wavedit_fourier2` | CC0: WaveEdit Online |
| 22 | WaveEdit Grav_b8 | lead | `Classic/wavedit_grav_b8` | CC0: WaveEdit Online |
| 23 | WaveEdit Spectr01 | lead | `Classic/wavedit_spectr01` | CC0: WaveEdit Online |
| 24 | WaveEdit Discorda | lead | `Classic/wavedit_discorda` | CC0: WaveEdit Online |
| 25 | WaveEdit Sohler47 | lead | `Classic/wavedit_sohler47` | CC0: WaveEdit Online |
| 26 | WaveEdit Ppg_wa27 | lead | `Classic/wavedit_ppg_wa27` | CC0: WaveEdit Online |
| 27 | WaveEdit Micro_q | lead | `Classic/wavedit_micro_q` | CC0: WaveEdit Online |
| 28 | WaveEdit Noise_wa | lead | `Classic/wavedit_noise_wa` | CC0: WaveEdit Online |
| 29 | WaveEdit Kuato | lead | `Classic/wavedit_kuato` | CC0: WaveEdit Online |
| 30 | WaveEdit Synlp67 | lead | `Classic/wavedit_synlp67` | CC0: WaveEdit Online |
| 31 | WaveEdit Komple01 | lead | `Classic/wavedit_komple01` | CC0: WaveEdit Online |
| 32 | Consonant 066 | lead | `Ambient/ambient_consonant_066` | generated: ambientgen |
| 33 | WaveEdit Iterat00 | lead | `Classic/wavedit_iterat00` | CC0: WaveEdit Online |
| 34 | WaveEdit Retro_sp | lead | `Classic/wavedit_retro_sp` | CC0: WaveEdit Online |
| 35 | WaveEdit Acid_rin | lead | `Classic/wavedit_acid_rin` | CC0: WaveEdit Online |
| 36 | WaveEdit Meravigl | lead | `Classic/wavedit_meravigl` | CC0: WaveEdit Online |
| 37 | WaveEdit Tidyb066 | lead | `Classic/wavedit_tidyb066` | CC0: WaveEdit Online |
| 38 | WaveEdit Tidyb049 | lead | `Classic/wavedit_tidyb049` | CC0: WaveEdit Online |
| 39 | WaveEdit Fracta03 | lead | `Classic/wavedit_fracta03` | CC0: WaveEdit Online |
| 40 | WaveEdit Chebyshe | lead | `Classic/wavedit_chebyshe` | CC0: WaveEdit Online |
| 41 | Sampled 167 | lead | `Ambient/ambient_sampled_167` | measured from the user's own SA3 material |
| 42 | WaveEdit Wavetabl | lead | `Classic/wavedit_wavetabl` | CC0: WaveEdit Online |
| 43 | Chord 004 | lead | `Harmonic/harmonic_chord_004` | generated: harmonicgen |
| 44 | WaveEdit Grav_b6 | lead | `Classic/wavedit_grav_b6` | CC0: WaveEdit Online |
| 45 | WaveEdit Sohler33 | lead | `Classic/wavedit_sohler33` | CC0: WaveEdit Online |
| 46 | WaveEdit Ppg_wa13 | lead | `Classic/wavedit_ppg_wa13` | CC0: WaveEdit Online |
| 47 | WaveEdit Pd104 | lead | `Classic/wavedit_pd104` | CC0: WaveEdit Online |
| 48 | WaveEdit Sohler48 | lead | `Classic/wavedit_sohler48` | CC0: WaveEdit Online |
| 49 | WaveEdit Ppg_wa28 | lead | `Classic/wavedit_ppg_wa28` | CC0: WaveEdit Online |
| 50 | WaveEdit Wavetrip | lead | `Classic/wavedit_wavetrip` | CC0: WaveEdit Online |
| 51 | WaveEdit Cz_ish | lead | `Classic/wavedit_cz_ish` | CC0: WaveEdit Online |
| 52 | WaveEdit Fx_bitno | lead | `Classic/wavedit_fx_bitno` | CC0: WaveEdit Online |
| 53 | WaveEdit Tidyb046 | lead | `Classic/wavedit_tidyb046` | CC0: WaveEdit Online |
| 54 | WaveEdit Sohler74 | lead | `Classic/wavedit_sohler74` | CC0: WaveEdit Online |
| 55 | WaveEdit Sohler29 | lead | `Classic/wavedit_sohler29` | CC0: WaveEdit Online |
| 56 | WaveEdit Enshtu02 | lead | `Classic/wavedit_enshtu02` | CC0: WaveEdit Online |
| 57 | WaveEdit Tidyb072 | lead | `Classic/wavedit_tidyb072` | CC0: WaveEdit Online |
| 58 | WaveEdit Tidyb050 | lead | `Classic/wavedit_tidyb050` | CC0: WaveEdit Online |
| 59 | WaveEdit Voice_a | lead | `Classic/wavedit_voice_a` | CC0: WaveEdit Online |
| 60 | Resonator 018 | lead | `Harmonic/harmonic_resonator_018` | generated: harmonicgen |
| 61 | WaveEdit Tidyb035 | lead | `Classic/wavedit_tidyb035` | CC0: WaveEdit Online |
| 62 | WaveEdit Tezzalog | lead | `Classic/wavedit_tezzalog` | CC0: WaveEdit Online |
| 63 | AKWF 0004-hollow-01 | lead | `Classic/akwf_0004_hollow_01` | CC0: AKWF |
| 64 | WaveEdit Lom_a | lead | `Classic/wavedit_lom_a` | CC0: WaveEdit Online |
| 65 | WaveEdit Tidyb064 | lead | `Classic/wavedit_tidyb064` | CC0: WaveEdit Online |
| 66 | WaveEdit Moddrop | lead | `Classic/wavedit_moddrop` | CC0: WaveEdit Online |
| 67 | WaveEdit Harmomet | lead | `Classic/wavedit_harmomet` | CC0: WaveEdit Online |
| 68 | Hollow 013 | lead | `Harmonic/harmonic_hollow_013` | generated: harmonicgen |
| 69 | WaveEdit Tidyb065 | lead | `Classic/wavedit_tidyb065` | CC0: WaveEdit Online |
| 70 | WaveEdit Ultra_ch | lead | `Classic/wavedit_ultra_ch` | CC0: WaveEdit Online |
| 71 | WaveEdit Laser_cr | lead | `Classic/wavedit_laser_cr` | CC0: WaveEdit Online |
| 72 | WaveEdit Ppg_wa05 | lead | `Classic/wavedit_ppg_wa05` | CC0: WaveEdit Online |
| 73 | WaveEdit Drone | lead | `Classic/wavedit_drone` | CC0: WaveEdit Online |
| 74 | WaveEdit Gentle_m | lead | `Classic/wavedit_gentle_m` | CC0: WaveEdit Online |
| 75 | WaveEdit Tidyb067 | lead | `Classic/wavedit_tidyb067` | CC0: WaveEdit Online |
| 76 | WaveEdit Tidyb005 | lead | `Classic/wavedit_tidyb005` | CC0: WaveEdit Online |
| 77 | Resonator 008 | lead | `Harmonic/harmonic_resonator_008` | generated: harmonicgen |
| 78 | WaveEdit Synlp71 | lead | `Classic/wavedit_synlp71` | CC0: WaveEdit Online |
| 79 | WaveEdit Sohler46 | lead | `Classic/wavedit_sohler46` | CC0: WaveEdit Online |
| 80 | WaveEdit Tidyb077 | lead | `Classic/wavedit_tidyb077` | CC0: WaveEdit Online |
| 81 | WaveEdit Tidyb027 | lead | `Classic/wavedit_tidyb027` | CC0: WaveEdit Online |
| 82 | WaveEdit Sand_eye | lead | `Classic/wavedit_sand_eye` | CC0: WaveEdit Online |
| 83 | WaveEdit Feedback | lead | `Classic/wavedit_feedback` | CC0: WaveEdit Online |
| 84 | WaveEdit Tidyb071 | lead | `Classic/wavedit_tidyb071` | CC0: WaveEdit Online |
| 85 | WaveEdit Snake_ey | lead | `Classic/wavedit_snake_ey` | CC0: WaveEdit Online |
| 86 | WaveEdit Hyperbol | arp | `Classic/wavedit_hyperbol` | CC0: WaveEdit Online |
| 87 | AKWF 0018-hollow-01 | arp | `Classic/akwf_0018_hollow_01` | CC0: AKWF |
| 88 | Overtone 064 | arp | `Ambient/ambient_overtone_064` | generated: ambientgen |
| 89 | WaveEdit Light_00 | arp | `Classic/wavedit_light_00` | CC0: WaveEdit Online |
| 90 | WaveEdit Crush_ad | arp | `Classic/wavedit_crush_ad` | CC0: WaveEdit Online |
| 91 | Organ 034 | arp | `Harmonic/harmonic_organ_034` | generated: harmonicgen |
| 92 | Sampled 131 | arp | `Ambient/ambient_sampled_131` | measured from the user's own SA3 material |
| 93 | Glass 012 | arp | `Harmonic/harmonic_glass_012` | generated: harmonicgen |
| 94 | WaveEdit Sohler67 | arp | `Classic/wavedit_sohler67` | CC0: WaveEdit Online |
| 95 | Organ 006 | arp | `Harmonic/harmonic_organ_006` | generated: harmonicgen |
| 96 | WaveEdit Ppg_wa14 | arp | `Classic/wavedit_ppg_wa14` | CC0: WaveEdit Online |
| 97 | WaveEdit Sq8_sh | arp | `Classic/wavedit_sq8_sh` | CC0: WaveEdit Online |
| 98 | AKWF clavinet-hollow-01 | arp | `Classic/akwf_clavinet_hollow_01` | CC0: AKWF |
| 99 | Glass 009 | arp | `Harmonic/harmonic_glass_009` | generated: harmonicgen |
| 100 | WaveEdit Grav_c7 | arp | `Classic/wavedit_grav_c7` | CC0: WaveEdit Online |
| 101 | Consonant 100 | arp | `Ambient/ambient_consonant_100` | generated: ambientgen |
| 102 | WaveEdit Sohler51 | arp | `Classic/wavedit_sohler51` | CC0: WaveEdit Online |
| 103 | Vowel Bass 015 | arp | `Ambient/ambient_vowel_bass_015` | generated: ambientgen |
| 104 | Sampled 227 | arp | `Ambient/ambient_sampled_227` | measured from the user's own SA3 material |
| 105 | Hollow 005 | arp | `Harmonic/harmonic_hollow_005` | generated: harmonicgen |
| 106 | Sampled 065 | arp | `Ambient/ambient_sampled_065` | measured from the user's own SA3 material |
| 107 | AKWF oscchip-04 | arp | `Classic/akwf_oscchip_04` | CC0: AKWF |
| 108 | AKWF raw-01 | arp | `Classic/akwf_raw_01` | CC0: AKWF |
| 109 | Hollow 015 | arp | `Harmonic/harmonic_hollow_015` | generated: harmonicgen |
| 110 | AKWF snippets-01 | arp | `Classic/akwf_snippets_01` | CC0: AKWF |
| 111 | Chord 027 | arp | `Harmonic/harmonic_chord_027` | generated: harmonicgen |
| 112 | Consonant 071 | arp | `Ambient/ambient_consonant_071` | generated: ambientgen |
| 113 | Chord 020 | arp | `Harmonic/harmonic_chord_020` | generated: harmonicgen |
| 114 | Consonant 051 | arp | `Ambient/ambient_consonant_051` | generated: ambientgen |
| 115 | WaveEdit Cyborg | arp | `Classic/wavedit_cyborg` | CC0: WaveEdit Online |
| 116 | Sampled 238 | arp | `Ambient/ambient_sampled_238` | measured from the user's own SA3 material |
| 117 | AKWF 0014-hollow-01 | arp | `Classic/akwf_0014_hollow_01` | CC0: AKWF |
| 118 | Overtone 000 | arp | `Ambient/ambient_overtone_000` | generated: ambientgen |
| 119 | WaveEdit Ppg_wa15 | arp | `Classic/wavedit_ppg_wa15` | CC0: WaveEdit Online |
| 120 | WaveEdit Grav_b4 | arp | `Classic/wavedit_grav_b4` | CC0: WaveEdit Online |
| 121 | Consonant 009 | arp | `Ambient/ambient_consonant_009` | generated: ambientgen |
| 122 | WaveEdit Grav_c5 | arp | `Classic/wavedit_grav_c5` | CC0: WaveEdit Online |
| 123 | WaveEdit Twist6 | arp | `Classic/wavedit_twist6` | CC0: WaveEdit Online |
| 124 | Glass 027 | arp | `Harmonic/harmonic_glass_027` | generated: harmonicgen |
| 125 | Sampled 078 | arp | `Ambient/ambient_sampled_078` | measured from the user's own SA3 material |
| 126 | AKWF oscchip-05 | arp | `Classic/akwf_oscchip_05` | CC0: AKWF |
| 127 | Consonant 120 | arp | `Ambient/ambient_consonant_120` | generated: ambientgen |
| 128 | WaveEdit Ppg_wa04 | arp | `Classic/wavedit_ppg_wa04` | CC0: WaveEdit Online |
| 129 | WaveEdit Sohler59 | arp | `Classic/wavedit_sohler59` | CC0: WaveEdit Online |
| 130 | Sampled 081 | arp | `Ambient/ambient_sampled_081` | measured from the user's own SA3 material |
| 131 | Otmorph 053 | arp | `Ambient/ambient_otmorph_053` | generated: ambientgen |
| 132 | Chord 030 | arp | `Harmonic/harmonic_chord_030` | generated: harmonicgen |
| 133 | Chord 032 | arp | `Harmonic/harmonic_chord_032` | generated: harmonicgen |
| 134 | Organ 029 | arp | `Harmonic/harmonic_organ_029` | generated: harmonicgen |
| 135 | Overtone 108 | arp | `Ambient/ambient_overtone_108` | generated: ambientgen |
| 136 | Hollow 001 | arp | `Harmonic/harmonic_hollow_001` | generated: harmonicgen |
| 137 | WaveEdit Ppg_wa10 | arp | `Classic/wavedit_ppg_wa10` | CC0: WaveEdit Online |
| 138 | WaveEdit Organ_di | arp | `Classic/wavedit_organ_di` | CC0: WaveEdit Online |
| 139 | AKWF 0009-hollow-01 | arp | `Classic/akwf_0009_hollow_01` | CC0: AKWF |
| 140 | Tube 042 | arp | `Ambient/ambient_tube_042` | generated: ambientgen |
| 141 | Vowel Bass 008 | arp | `Ambient/ambient_vowel_bass_008` | generated: ambientgen |
| 142 | Organ 035 | arp | `Harmonic/harmonic_organ_035` | generated: harmonicgen |
| 143 | Metal 001 | arp | `Harmonic/harmonic_metal_001` | generated: harmonicgen |
| 144 | Consonant 114 | arp | `Ambient/ambient_consonant_114` | generated: ambientgen |
| 145 | AKWF vgame-03 | arp | `Classic/akwf_vgame_03` | CC0: AKWF |
| 146 | Sampled 224 | arp | `Ambient/ambient_sampled_224` | measured from the user's own SA3 material |
| 147 | Resonator 001 | arp | `Harmonic/harmonic_resonator_001` | generated: harmonicgen |
| 148 | Vowel Bass 038 | arp | `Ambient/ambient_vowel_bass_038` | generated: ambientgen |
| 149 | Sampled 058 | arp | `Ambient/ambient_sampled_058` | measured from the user's own SA3 material |
| 150 | Vowel Tenor 038 | arp | `Ambient/ambient_vowel_tenor_038` | generated: ambientgen |
| 151 | Glass 026 | arp | `Harmonic/harmonic_glass_026` | generated: harmonicgen |
| 152 | Chord 019 | arp | `Harmonic/harmonic_chord_019` | generated: harmonicgen |
| 153 | Otmorph 115 | arp | `Ambient/ambient_otmorph_115` | generated: ambientgen |
| 154 | AKWF stereo-04 | arp | `Classic/akwf_stereo_04` | CC0: AKWF |
| 155 | Consonant 105 | arp | `Ambient/ambient_consonant_105` | generated: ambientgen |
| 156 | Vowel Bass 026 | arp | `Ambient/ambient_vowel_bass_026` | generated: ambientgen |
| 157 | Chord 017 | arp | `Harmonic/harmonic_chord_017` | generated: harmonicgen |
| 158 | WaveEdit Ppg_wa11 | arp | `Classic/wavedit_ppg_wa11` | CC0: WaveEdit Online |
| 159 | Consonant 109 | arp | `Ambient/ambient_consonant_109` | generated: ambientgen |
| 160 | Sampled 125 | arp | `Ambient/ambient_sampled_125` | measured from the user's own SA3 material |
| 161 | Consonant 012 | arp | `Ambient/ambient_consonant_012` | generated: ambientgen |
| 162 | Overtone 046 | arp | `Ambient/ambient_overtone_046` | generated: ambientgen |
| 163 | Glass 016 | arp | `Harmonic/harmonic_glass_016` | generated: harmonicgen |
| 164 | Otmorph 067 | arp | `Ambient/ambient_otmorph_067` | generated: ambientgen |
| 165 | Organ 026 | arp | `Harmonic/harmonic_organ_026` | generated: harmonicgen |
| 166 | Otmorph 005 | arp | `Ambient/ambient_otmorph_005` | generated: ambientgen |
| 167 | Glass 018 | arp | `Harmonic/harmonic_glass_018` | generated: harmonicgen |
| 168 | Consonant 018 | arp | `Ambient/ambient_consonant_018` | generated: ambientgen |
| 169 | Hollow 023 | arp | `Harmonic/harmonic_hollow_023` | generated: harmonicgen |
| 170 | Overtone 019 | arp | `Ambient/ambient_overtone_019` | generated: ambientgen |
| 171 | Otmorph 070 | arp | `Ambient/ambient_otmorph_070` | generated: ambientgen |
| 172 | Consonant 127 | arp | `Ambient/ambient_consonant_127` | generated: ambientgen |
| 173 | Vowel Bass 029 | arp | `Ambient/ambient_vowel_bass_029` | generated: ambientgen |
| 174 | Organ 016 | arp | `Harmonic/harmonic_organ_016` | generated: harmonicgen |
| 175 | Vowel Bass 017 | arp | `Ambient/ambient_vowel_bass_017` | generated: ambientgen |
| 176 | Sampled 209 | arp | `Ambient/ambient_sampled_209` | measured from the user's own SA3 material |
| 177 | Sampled 130 | arp | `Ambient/ambient_sampled_130` | measured from the user's own SA3 material |
| 178 | Overtone 038 | arp | `Ambient/ambient_overtone_038` | generated: ambientgen |
| 179 | WaveEdit Reso_ana | arp | `Classic/wavedit_reso_ana` | CC0: WaveEdit Online |
| 180 | Consonant 142 | arp | `Ambient/ambient_consonant_142` | generated: ambientgen |
| 181 | WaveEdit Reso_p00 | arp | `Classic/wavedit_reso_p00` | CC0: WaveEdit Online |
| 182 | Chord 009 | pad | `Harmonic/harmonic_chord_009` | generated: harmonicgen |
| 183 | AKWF distorted-01 | pad | `Classic/akwf_distorted_01` | CC0: AKWF |
| 184 | Sampled 162 | pad | `Ambient/ambient_sampled_162` | measured from the user's own SA3 material |
| 185 | Glass 006 | pad | `Harmonic/harmonic_glass_006` | generated: harmonicgen |
| 186 | Metal 003 | pad | `Harmonic/harmonic_metal_003` | generated: harmonicgen |
| 187 | WaveEdit Square_p | pad | `Classic/wavedit_square_p` | CC0: WaveEdit Online |
| 188 | Sampled 059 | pad | `Ambient/ambient_sampled_059` | measured from the user's own SA3 material |
| 189 | Organ 030 | pad | `Harmonic/harmonic_organ_030` | generated: harmonicgen |
| 190 | Sampled 003 | pad | `Ambient/ambient_sampled_003` | measured from the user's own SA3 material |
| 191 | Consonant 036 | pad | `Ambient/ambient_consonant_036` | generated: ambientgen |
| 192 | Chord 034 | pad | `Harmonic/harmonic_chord_034` | generated: harmonicgen |
| 193 | Sampled 193 | pad | `Ambient/ambient_sampled_193` | measured from the user's own SA3 material |
| 194 | WaveEdit Qux_fmy | pad | `Classic/wavedit_qux_fmy` | CC0: WaveEdit Online |
| 195 | Vowel Bass 023 | pad | `Ambient/ambient_vowel_bass_023` | generated: ambientgen |
| 196 | Otmorph 133 | pad | `Ambient/ambient_otmorph_133` | generated: ambientgen |
| 197 | WaveEdit Sohler52 | pad | `Classic/wavedit_sohler52` | CC0: WaveEdit Online |
| 198 | Vowel Bass 007 | pad | `Ambient/ambient_vowel_bass_007` | generated: ambientgen |
| 199 | WaveEdit Grav_b3 | pad | `Classic/wavedit_grav_b3` | CC0: WaveEdit Online |
| 200 | Consonant 025 | pad | `Ambient/ambient_consonant_025` | generated: ambientgen |
| 201 | Consonant 146 | pad | `Ambient/ambient_consonant_146` | generated: ambientgen |
| 202 | Chord 018 | pad | `Harmonic/harmonic_chord_018` | generated: harmonicgen |
| 203 | Overtone 047 | pad | `Ambient/ambient_overtone_047` | generated: ambientgen |
| 204 | Chord 038 | pad | `Harmonic/harmonic_chord_038` | generated: harmonicgen |
| 205 | Sampled 127 | pad | `Ambient/ambient_sampled_127` | measured from the user's own SA3 material |
| 206 | Metal 009 | pad | `Harmonic/harmonic_metal_009` | generated: harmonicgen |
| 207 | Organ 014 | pad | `Harmonic/harmonic_organ_014` | generated: harmonicgen |
| 208 | Consonant 033 | pad | `Ambient/ambient_consonant_033` | generated: ambientgen |
| 209 | Vowel Bass 035 | pad | `Ambient/ambient_vowel_bass_035` | generated: ambientgen |
| 210 | Consonant 001 | pad | `Ambient/ambient_consonant_001` | generated: ambientgen |
| 211 | WaveEdit Iterativ | pad | `Classic/wavedit_iterativ` | CC0: WaveEdit Online |
| 212 | Consonant 129 | pad | `Ambient/ambient_consonant_129` | generated: ambientgen |
| 213 | Tube 066 | pad | `Ambient/ambient_tube_066` | generated: ambientgen |
| 214 | Organ 025 | pad | `Harmonic/harmonic_organ_025` | generated: harmonicgen |
| 215 | Vowel Tenor 012 | pad | `Ambient/ambient_vowel_tenor_012` | generated: ambientgen |
| 216 | Chord 039 | pad | `Harmonic/harmonic_chord_039` | generated: harmonicgen |
| 217 | Sampled 106 | pad | `Ambient/ambient_sampled_106` | measured from the user's own SA3 material |
| 218 | Sampled 207 | pad | `Ambient/ambient_sampled_207` | measured from the user's own SA3 material |
| 219 | Breath 013 | pad | `Harmonic/harmonic_breath_013` | generated: harmonicgen |
| 220 | WaveEdit Grav_c2 | pad | `Classic/wavedit_grav_c2` | CC0: WaveEdit Online |
| 221 | Consonant 079 | pad | `Ambient/ambient_consonant_079` | generated: ambientgen |
| 222 | Otmorph 119 | pad | `Ambient/ambient_otmorph_119` | generated: ambientgen |
| 223 | Consonant 064 | pad | `Ambient/ambient_consonant_064` | generated: ambientgen |
| 224 | WaveEdit Ppg_wa18 | pad | `Classic/wavedit_ppg_wa18` | CC0: WaveEdit Online |
| 225 | WaveEdit Pd103 | pad | `Classic/wavedit_pd103` | CC0: WaveEdit Online |
| 226 | Overtone 033 | pad | `Ambient/ambient_overtone_033` | generated: ambientgen |
| 227 | Chord 013 | pad | `Harmonic/harmonic_chord_013` | generated: harmonicgen |
| 228 | Vowel Bass 014 | pad | `Ambient/ambient_vowel_bass_014` | generated: ambientgen |
| 229 | Sampled 226 | pad | `Ambient/ambient_sampled_226` | measured from the user's own SA3 material |
| 230 | Chord 011 | pad | `Harmonic/harmonic_chord_011` | generated: harmonicgen |
| 231 | Sampled 048 | pad | `Ambient/ambient_sampled_048` | measured from the user's own SA3 material |
| 232 | Vowel Tenor 037 | pad | `Ambient/ambient_vowel_tenor_037` | generated: ambientgen |
| 233 | Otmorph 095 | pad | `Ambient/ambient_otmorph_095` | generated: ambientgen |
| 234 | Organ 012 | pad | `Harmonic/harmonic_organ_012` | generated: harmonicgen |
| 235 | Vowel Tenor 019 | pad | `Ambient/ambient_vowel_tenor_019` | generated: ambientgen |
| 236 | Overtone 015 | pad | `Ambient/ambient_overtone_015` | generated: ambientgen |
| 237 | Overtone 054 | pad | `Ambient/ambient_overtone_054` | generated: ambientgen |
| 238 | Consonant 088 | pad | `Ambient/ambient_consonant_088` | generated: ambientgen |
| 239 | Consonant 108 | pad | `Ambient/ambient_consonant_108` | generated: ambientgen |
| 240 | Sampled 197 | pad | `Ambient/ambient_sampled_197` | measured from the user's own SA3 material |
| 241 | Organ 037 | pad | `Harmonic/harmonic_organ_037` | generated: harmonicgen |
| 242 | Choir 004 | pad | `Harmonic/harmonic_choir_004` | generated: harmonicgen |
| 243 | Vowel Bass 030 | pad | `Ambient/ambient_vowel_bass_030` | generated: ambientgen |
| 244 | Resonator 019 | pad | `Harmonic/harmonic_resonator_019` | generated: harmonicgen |
| 245 | Otmorph 086 | pad | `Ambient/ambient_otmorph_086` | generated: ambientgen |
| 246 | Tube 074 | pad | `Ambient/ambient_tube_074` | generated: ambientgen |
| 247 | Tube 089 | pad | `Ambient/ambient_tube_089` | generated: ambientgen |
| 248 | Sampled 182 | pad | `Ambient/ambient_sampled_182` | measured from the user's own SA3 material |
| 249 | Organ 038 | pad | `Harmonic/harmonic_organ_038` | generated: harmonicgen |
| 250 | Sampled 173 | pad | `Ambient/ambient_sampled_173` | measured from the user's own SA3 material |
| 251 | Consonant 060 | pad | `Ambient/ambient_consonant_060` | generated: ambientgen |
| 252 | Otmorph 030 | pad | `Ambient/ambient_otmorph_030` | generated: ambientgen |
| 253 | Sampled 138 | pad | `Ambient/ambient_sampled_138` | measured from the user's own SA3 material |
| 254 | Consonant 056 | pad | `Ambient/ambient_consonant_056` | generated: ambientgen |
| 255 | Otmorph 132 | pad | `Ambient/ambient_otmorph_132` | generated: ambientgen |
| 256 | Otmorph 138 | pad | `Ambient/ambient_otmorph_138` | generated: ambientgen |
| 257 | Glass 003 | pad | `Harmonic/harmonic_glass_003` | generated: harmonicgen |
| 258 | Vowel Bass 021 | pad | `Ambient/ambient_vowel_bass_021` | generated: ambientgen |
| 259 | Vowel Tenor 036 | pad | `Ambient/ambient_vowel_tenor_036` | generated: ambientgen |
| 260 | Organ 033 | pad | `Harmonic/harmonic_organ_033` | generated: harmonicgen |
| 261 | Sampled 214 | pad | `Ambient/ambient_sampled_214` | measured from the user's own SA3 material |
| 262 | Consonant 081 | pad | `Ambient/ambient_consonant_081` | generated: ambientgen |
| 263 | Consonant 094 | pad | `Ambient/ambient_consonant_094` | generated: ambientgen |
| 264 | Consonant 028 | pad | `Ambient/ambient_consonant_028` | generated: ambientgen |
| 265 | Consonant 003 | pad | `Ambient/ambient_consonant_003` | generated: ambientgen |
| 266 | Vowel Soprano 035 | pad | `Ambient/ambient_vowel_soprano_035` | generated: ambientgen |
| 267 | Consonant 126 | pad | `Ambient/ambient_consonant_126` | generated: ambientgen |
| 268 | Vowel Bass 027 | pad | `Ambient/ambient_vowel_bass_027` | generated: ambientgen |
| 269 | Chord 000 | pad | `Harmonic/harmonic_chord_000` | generated: harmonicgen |
| 270 | Resonator 010 | pad | `Harmonic/harmonic_resonator_010` | generated: harmonicgen |
| 271 | Choir 026 | pad | `Harmonic/harmonic_choir_026` | generated: harmonicgen |
| 272 | Metal 015 | pad | `Harmonic/harmonic_metal_015` | generated: harmonicgen |
| 273 | AKWF 0018-02 | pad | `Classic/akwf_0018_02` | CC0: AKWF |
| 274 | Consonant 046 | pad | `Ambient/ambient_consonant_046` | generated: ambientgen |
| 275 | Otmorph 079 | pad | `Ambient/ambient_otmorph_079` | generated: ambientgen |
| 276 | Vowel Tenor 024 | pad | `Ambient/ambient_vowel_tenor_024` | generated: ambientgen |
| 277 | Consonant 117 | pad | `Ambient/ambient_consonant_117` | generated: ambientgen |
| 278 | Choir 025 | pad | `Harmonic/harmonic_choir_025` | generated: harmonicgen |
| 279 | Overtone 067 | pad | `Ambient/ambient_overtone_067` | generated: ambientgen |
| 280 | Chord 016 | pad | `Harmonic/harmonic_chord_016` | generated: harmonicgen |
| 281 | Otmorph 109 | pad | `Ambient/ambient_otmorph_109` | generated: ambientgen |
| 282 | Vowel Alto 001 | pad | `Ambient/ambient_vowel_alto_001` | generated: ambientgen |
| 283 | Otmorph 089 | pad | `Ambient/ambient_otmorph_089` | generated: ambientgen |
| 284 | Consonant 077 | pad | `Ambient/ambient_consonant_077` | generated: ambientgen |
| 285 | Shimmer 033 | pad | `Harmonic/harmonic_shimmer_033` | generated: harmonicgen |
| 286 | Otmorph 022 | pad | `Ambient/ambient_otmorph_022` | generated: ambientgen |
| 287 | Consonant 144 | pad | `Ambient/ambient_consonant_144` | generated: ambientgen |
| 288 | Organ 000 | pad | `Harmonic/harmonic_organ_000` | generated: harmonicgen |
| 289 | Consonant 139 | pad | `Ambient/ambient_consonant_139` | generated: ambientgen |
| 290 | Consonant 106 | pad | `Ambient/ambient_consonant_106` | generated: ambientgen |
| 291 | Vowel Bass 039 | pad | `Ambient/ambient_vowel_bass_039` | generated: ambientgen |
| 292 | Otmorph 043 | pad | `Ambient/ambient_otmorph_043` | generated: ambientgen |
| 293 | Vowel Tenor 033 | pad | `Ambient/ambient_vowel_tenor_033` | generated: ambientgen |
| 294 | Resonator 012 | pad | `Harmonic/harmonic_resonator_012` | generated: harmonicgen |
| 295 | Otmorph 059 | pad | `Ambient/ambient_otmorph_059` | generated: ambientgen |
| 296 | Otmorph 073 | pad | `Ambient/ambient_otmorph_073` | generated: ambientgen |
| 297 | Sampled 093 | pad | `Ambient/ambient_sampled_093` | measured from the user's own SA3 material |
| 298 | Overtone 055 | pad | `Ambient/ambient_overtone_055` | generated: ambientgen |
| 299 | Sampled 189 | pad | `Ambient/ambient_sampled_189` | measured from the user's own SA3 material |
| 300 | Otmorph 068 | pad | `Ambient/ambient_otmorph_068` | generated: ambientgen |
| 301 | Otmorph 064 | pad | `Ambient/ambient_otmorph_064` | generated: ambientgen |
| 302 | Breath 029 | pad | `Harmonic/harmonic_breath_029` | generated: harmonicgen |
| 303 | Chord 033 | pad | `Harmonic/harmonic_chord_033` | generated: harmonicgen |
| 304 | Sampled 108 | pad | `Ambient/ambient_sampled_108` | measured from the user's own SA3 material |
| 305 | Consonant 030 | pad | `Ambient/ambient_consonant_030` | generated: ambientgen |
| 306 | Bowed 075 | pad | `Ambient/ambient_bowed_075` | generated: ambientgen |
| 307 | Organ 011 | pad | `Harmonic/harmonic_organ_011` | generated: harmonicgen |
| 308 | Vowel Tenor 006 | pad | `Ambient/ambient_vowel_tenor_006` | generated: ambientgen |
| 309 | Otmorph 056 | pad | `Ambient/ambient_otmorph_056` | generated: ambientgen |
| 310 | AKWF 0006-02 | counter | `Classic/akwf_0006_02` | CC0: AKWF |
| 311 | Glass 037 | counter | `Harmonic/harmonic_glass_037` | generated: harmonicgen |
| 312 | Vowel Bass 001 | counter | `Ambient/ambient_vowel_bass_001` | generated: ambientgen |
| 313 | WaveEdit Tidyb026 | counter | `Classic/wavedit_tidyb026` | CC0: WaveEdit Online |
| 314 | WaveEdit Sohler39 | counter | `Classic/wavedit_sohler39` | CC0: WaveEdit Online |
| 315 | Sampled 076 | counter | `Ambient/ambient_sampled_076` | measured from the user's own SA3 material |
| 316 | Organ 021 | counter | `Harmonic/harmonic_organ_021` | generated: harmonicgen |
| 317 | WaveEdit Harmonio | counter | `Classic/wavedit_harmonio` | CC0: WaveEdit Online |
| 318 | WaveEdit Grav_c9 | counter | `Classic/wavedit_grav_c9` | CC0: WaveEdit Online |
| 319 | Consonant 037 | counter | `Ambient/ambient_consonant_037` | generated: ambientgen |
| 320 | WaveEdit Sine_a02 | counter | `Classic/wavedit_sine_a02` | CC0: WaveEdit Online |
| 321 | WaveEdit High_fre | counter | `Classic/wavedit_high_fre` | CC0: WaveEdit Online |
| 322 | WaveEdit Tidyb024 | counter | `Classic/wavedit_tidyb024` | CC0: WaveEdit Online |
| 323 | AKWF hvoice-01 | counter | `Classic/akwf_hvoice_01` | CC0: AKWF |
| 324 | WaveEdit Sohler58 | counter | `Classic/wavedit_sohler58` | CC0: WaveEdit Online |
| 325 | WaveEdit Ppg_wa29 | counter | `Classic/wavedit_ppg_wa29` | CC0: WaveEdit Online |
| 326 | Vowel Bass 036 | counter | `Ambient/ambient_vowel_bass_036` | generated: ambientgen |
| 327 | WaveEdit Sohler19 | counter | `Classic/wavedit_sohler19` | CC0: WaveEdit Online |
| 328 | WaveEdit Sohler53 | counter | `Classic/wavedit_sohler53` | CC0: WaveEdit Online |
| 329 | WaveEdit Sohler94 | counter | `Classic/wavedit_sohler94` | CC0: WaveEdit Online |
| 330 | Glass 023 | counter | `Harmonic/harmonic_glass_023` | generated: harmonicgen |
| 331 | Chord 031 | counter | `Harmonic/harmonic_chord_031` | generated: harmonicgen |
| 332 | WaveEdit Access_v | counter | `Classic/wavedit_access_v` | CC0: WaveEdit Online |
| 333 | Bowed 079 | counter | `Ambient/ambient_bowed_079` | generated: ambientgen |
| 334 | AKWF c604-01 | counter | `Classic/akwf_c604_01` | CC0: AKWF |
| 335 | WaveEdit Sohler56 | counter | `Classic/wavedit_sohler56` | CC0: WaveEdit Online |
| 336 | WaveEdit Tidyb017 | counter | `Classic/wavedit_tidyb017` | CC0: WaveEdit Online |
| 337 | WaveEdit Sohler11 | counter | `Classic/wavedit_sohler11` | CC0: WaveEdit Online |
| 338 | Overtone 018 | counter | `Ambient/ambient_overtone_018` | generated: ambientgen |
| 339 | Consonant 032 | counter | `Ambient/ambient_consonant_032` | generated: ambientgen |
| 340 | Reed 034 | counter | `Harmonic/harmonic_reed_034` | generated: harmonicgen |
| 341 | WaveEdit Mk_dwg_h | counter | `Classic/wavedit_mk_dwg_h` | CC0: WaveEdit Online |
| 342 | WaveEdit Grav_c3 | counter | `Classic/wavedit_grav_c3` | CC0: WaveEdit Online |
| 343 | WaveEdit Fracta01 | counter | `Classic/wavedit_fracta01` | CC0: WaveEdit Online |
| 344 | WaveEdit Synlp54 | counter | `Classic/wavedit_synlp54` | CC0: WaveEdit Online |
| 345 | WaveEdit Tidyb062 | counter | `Classic/wavedit_tidyb062` | CC0: WaveEdit Online |
| 346 | WaveEdit Envelo01 | counter | `Classic/wavedit_envelo01` | CC0: WaveEdit Online |
| 347 | Vowel Tenor 005 | counter | `Ambient/ambient_vowel_tenor_005` | generated: ambientgen |
| 348 | WaveEdit Tidyb004 | counter | `Classic/wavedit_tidyb004` | CC0: WaveEdit Online |
| 349 | Chord 008 | counter | `Harmonic/harmonic_chord_008` | generated: harmonicgen |
| 350 | AKWF bitreduced-01 | counter | `Classic/akwf_bitreduced_01` | CC0: AKWF |
| 351 | AKWF bw-saw-01 | counter | `Classic/akwf_bw_saw_01` | CC0: AKWF |
| 352 | Overtone 053 | counter | `Ambient/ambient_overtone_053` | generated: ambientgen |
| 353 | WaveEdit Grav_a6 | counter | `Classic/wavedit_grav_a6` | CC0: WaveEdit Online |
| 354 | WaveEdit Sohler26 | counter | `Classic/wavedit_sohler26` | CC0: WaveEdit Online |
| 355 | Sampled 066 | counter | `Ambient/ambient_sampled_066` | measured from the user's own SA3 material |
| 356 | WaveEdit Sohler86 | counter | `Classic/wavedit_sohler86` | CC0: WaveEdit Online |
| 357 | Tube 036 | counter | `Ambient/ambient_tube_036` | generated: ambientgen |
| 358 | Otmorph 014 | counter | `Ambient/ambient_otmorph_014` | generated: ambientgen |
| 359 | WaveEdit Sohler76 | counter | `Classic/wavedit_sohler76` | CC0: WaveEdit Online |
| 360 | WaveEdit Sohler27 | counter | `Classic/wavedit_sohler27` | CC0: WaveEdit Online |
| 361 | Choir 030 | counter | `Harmonic/harmonic_choir_030` | generated: harmonicgen |
| 362 | WaveEdit Bank_410 | counter | `Classic/wavedit_bank_410` | CC0: WaveEdit Online |
| 363 | WaveEdit Tidyb018 | counter | `Classic/wavedit_tidyb018` | CC0: WaveEdit Online |
| 364 | WaveEdit Alpha_2 | counter | `Classic/wavedit_alpha_2` | CC0: WaveEdit Online |
| 365 | WaveEdit Tidyb003 | counter | `Classic/wavedit_tidyb003` | CC0: WaveEdit Online |
| 366 | WaveEdit Sohler35 | counter | `Classic/wavedit_sohler35` | CC0: WaveEdit Online |
| 367 | WaveEdit Lfo_play | counter | `Classic/wavedit_lfo_play` | CC0: WaveEdit Online |
| 368 | WaveEdit Ppg_wa01 | counter | `Classic/wavedit_ppg_wa01` | CC0: WaveEdit Online |
| 369 | WaveEdit Ppg_wa02 | counter | `Classic/wavedit_ppg_wa02` | CC0: WaveEdit Online |
| 370 | WaveEdit Prophe00 | counter | `Classic/wavedit_prophe00` | CC0: WaveEdit Online |
| 371 | Vowel Tenor 011 | counter | `Ambient/ambient_vowel_tenor_011` | generated: ambientgen |
| 372 | Tube 072 | counter | `Ambient/ambient_tube_072` | generated: ambientgen |
| 373 | WaveEdit Tidyb058 | counter | `Classic/wavedit_tidyb058` | CC0: WaveEdit Online |
| 374 | Consonant 137 | counter | `Ambient/ambient_consonant_137` | generated: ambientgen |
| 375 | WaveEdit Sohler31 | counter | `Classic/wavedit_sohler31` | CC0: WaveEdit Online |
| 376 | Consonant 076 | counter | `Ambient/ambient_consonant_076` | generated: ambientgen |
| 377 | WaveEdit Just_ran | counter | `Classic/wavedit_just_ran` | CC0: WaveEdit Online |
| 378 | Consonant 080 | counter | `Ambient/ambient_consonant_080` | generated: ambientgen |
| 379 | WaveEdit Grav_a5 | counter | `Classic/wavedit_grav_a5` | CC0: WaveEdit Online |
| 380 | WaveEdit Sohler14 | counter | `Classic/wavedit_sohler14` | CC0: WaveEdit Online |
| 381 | Resonator 020 | counter | `Harmonic/harmonic_resonator_020` | generated: harmonicgen |
| 382 | WaveEdit Grav_a1 | counter | `Classic/wavedit_grav_a1` | CC0: WaveEdit Online |
| 383 | Glass 020 | counter | `Harmonic/harmonic_glass_020` | generated: harmonicgen |
| 384 | WaveEdit Sohler97 | counter | `Classic/wavedit_sohler97` | CC0: WaveEdit Online |
| 385 | Chord 028 | counter | `Harmonic/harmonic_chord_028` | generated: harmonicgen |
| 386 | WaveEdit Grav_a2 | counter | `Classic/wavedit_grav_a2` | CC0: WaveEdit Online |
| 387 | Vowel Bass 034 | counter | `Ambient/ambient_vowel_bass_034` | generated: ambientgen |
| 388 | WaveEdit Sohler70 | counter | `Classic/wavedit_sohler70` | CC0: WaveEdit Online |
| 389 | Breath 031 | counter | `Harmonic/harmonic_breath_031` | generated: harmonicgen |
| 390 | AKWF oscchip-03 | counter | `Classic/akwf_oscchip_03` | CC0: AKWF |
| 391 | WaveEdit Sohler84 | counter | `Classic/wavedit_sohler84` | CC0: WaveEdit Online |
| 392 | Consonant 149 | counter | `Ambient/ambient_consonant_149` | generated: ambientgen |
| 393 | WaveEdit Tidyb041 | counter | `Classic/wavedit_tidyb041` | CC0: WaveEdit Online |
| 394 | Consonant 023 | counter | `Ambient/ambient_consonant_023` | generated: ambientgen |
| 395 | Otmorph 126 | counter | `Ambient/ambient_otmorph_126` | generated: ambientgen |
| 396 | Bowed 054 | counter | `Ambient/ambient_bowed_054` | generated: ambientgen |
| 397 | WaveEdit Mixed_as | counter | `Classic/wavedit_mixed_as` | CC0: WaveEdit Online |
| 398 | Otmorph 084 | counter | `Ambient/ambient_otmorph_084` | generated: ambientgen |
| 399 | WaveEdit Twist31 | counter | `Classic/wavedit_twist31` | CC0: WaveEdit Online |
| 400 | WaveEdit Spectr05 | counter | `Classic/wavedit_spectr05` | CC0: WaveEdit Online |
| 401 | WaveEdit Sohler92 | counter | `Classic/wavedit_sohler92` | CC0: WaveEdit Online |
| 402 | WaveEdit Isobelle | counter | `Classic/wavedit_isobelle` | CC0: WaveEdit Online |
| 403 | Chord 021 | counter | `Harmonic/harmonic_chord_021` | generated: harmonicgen |
| 404 | Consonant 067 | counter | `Ambient/ambient_consonant_067` | generated: ambientgen |
| 405 | WaveEdit Envelope | counter | `Classic/wavedit_envelope` | CC0: WaveEdit Online |
| 406 | Sub 003 | drone | `Harmonic/harmonic_sub_003` | generated: harmonicgen |
| 407 | WaveEdit Sohler79 | drone | `Classic/wavedit_sohler79` | CC0: WaveEdit Online |
| 408 | Tube 002 | drone | `Ambient/ambient_tube_002` | generated: ambientgen |
| 409 | Consonant 008 | drone | `Ambient/ambient_consonant_008` | generated: ambientgen |
| 410 | WaveEdit Ppg_wa03 | drone | `Classic/wavedit_ppg_wa03` | CC0: WaveEdit Online |
| 411 | Pluck 028 | drone | `Harmonic/harmonic_pluck_028` | generated: harmonicgen |
| 412 | Vowel Alto 012 | drone | `Ambient/ambient_vowel_alto_012` | generated: ambientgen |
| 413 | AKWF epiano-01 | drone | `Classic/akwf_epiano_01` | CC0: AKWF |
| 414 | AKWF bw-squ-02 | drone | `Classic/akwf_bw_squ_02` | CC0: AKWF |
| 415 | Organ 005 | drone | `Harmonic/harmonic_organ_005` | generated: harmonicgen |
| 416 | WaveEdit Sine2saw | drone | `Classic/wavedit_sine2saw` | CC0: WaveEdit Online |
| 417 | Vowel Alto 039 | drone | `Ambient/ambient_vowel_alto_039` | generated: ambientgen |
| 418 | Glass 004 | drone | `Harmonic/harmonic_glass_004` | generated: harmonicgen |
| 419 | WaveEdit Sohler43 | drone | `Classic/wavedit_sohler43` | CC0: WaveEdit Online |
| 420 | Sampled 109 | drone | `Ambient/ambient_sampled_109` | measured from the user's own SA3 material |
| 421 | Vowel Alto 003 | drone | `Ambient/ambient_vowel_alto_003` | generated: ambientgen |
| 422 | Glass 010 | drone | `Harmonic/harmonic_glass_010` | generated: harmonicgen |
| 423 | AKWF clarinett-01 | drone | `Classic/akwf_clarinett_01` | CC0: AKWF |
| 424 | WaveEdit Sohler18 | drone | `Classic/wavedit_sohler18` | CC0: WaveEdit Online |
| 425 | Sampled 170 | drone | `Ambient/ambient_sampled_170` | measured from the user's own SA3 material |
| 426 | Tube 046 | drone | `Ambient/ambient_tube_046` | generated: ambientgen |
| 427 | Vowel Tenor 017 | drone | `Ambient/ambient_vowel_tenor_017` | generated: ambientgen |
| 428 | WaveEdit Foldfeed | drone | `Classic/wavedit_foldfeed` | CC0: WaveEdit Online |
| 429 | Tube 008 | drone | `Ambient/ambient_tube_008` | generated: ambientgen |
| 430 | AKWF 0015-01 | drone | `Classic/akwf_0015_01` | CC0: AKWF |
| 431 | Otmorph 106 | drone | `Ambient/ambient_otmorph_106` | generated: ambientgen |
| 432 | Overtone 037 | drone | `Ambient/ambient_overtone_037` | generated: ambientgen |
| 433 | Consonant 022 | drone | `Ambient/ambient_consonant_022` | generated: ambientgen |
| 434 | Breath 019 | drone | `Harmonic/harmonic_breath_019` | generated: harmonicgen |
| 435 | WaveEdit Ppg_wa07 | drone | `Classic/wavedit_ppg_wa07` | CC0: WaveEdit Online |
| 436 | Sampled 164 | drone | `Ambient/ambient_sampled_164` | measured from the user's own SA3 material |
| 437 | WaveEdit Sine_mut | drone | `Classic/wavedit_sine_mut` | CC0: WaveEdit Online |
| 438 | Vowel Alto 007 | drone | `Ambient/ambient_vowel_alto_007` | generated: ambientgen |
| 439 | Sampled 211 | drone | `Ambient/ambient_sampled_211` | measured from the user's own SA3 material |
| 440 | Metal 025 | drone | `Harmonic/harmonic_metal_025` | generated: harmonicgen |
| 441 | Glass 007 | drone | `Harmonic/harmonic_glass_007` | generated: harmonicgen |
| 442 | Vowel Soprano 011 | drone | `Ambient/ambient_vowel_soprano_011` | generated: ambientgen |
| 443 | Glass 024 | drone | `Harmonic/harmonic_glass_024` | generated: harmonicgen |
| 444 | AKWF 0008-01 | drone | `Classic/akwf_0008_01` | CC0: AKWF |
| 445 | Vowel Soprano 033 | drone | `Ambient/ambient_vowel_soprano_033` | generated: ambientgen |
| 446 | Shimmer 002 | drone | `Harmonic/harmonic_shimmer_002` | generated: harmonicgen |
| 447 | Pure 015 | drone | `Harmonic/harmonic_pure_015` | generated: harmonicgen |
| 448 | WaveEdit Geometri | drone | `Classic/wavedit_geometri` | CC0: WaveEdit Online |
| 449 | Sampled 203 | drone | `Ambient/ambient_sampled_203` | measured from the user's own SA3 material |
| 450 | Chord 007 | drone | `Harmonic/harmonic_chord_007` | generated: harmonicgen |
| 451 | Vowel Soprano 005 | drone | `Ambient/ambient_vowel_soprano_005` | generated: ambientgen |
| 452 | AKWF altosax-01 | drone | `Classic/akwf_altosax_01` | CC0: AKWF |
| 453 | Vowel Soprano 015 | drone | `Ambient/ambient_vowel_soprano_015` | generated: ambientgen |
| 454 | WaveEdit Ppg_wa17 | drone | `Classic/wavedit_ppg_wa17` | CC0: WaveEdit Online |
| 455 | Overtone 010 | drone | `Ambient/ambient_overtone_010` | generated: ambientgen |
| 456 | WaveEdit Synlp159 | drone | `Classic/wavedit_synlp159` | CC0: WaveEdit Online |
| 457 | Sampled 115 | drone | `Ambient/ambient_sampled_115` | measured from the user's own SA3 material |
| 458 | WaveEdit Ppg_wa22 | drone | `Classic/wavedit_ppg_wa22` | CC0: WaveEdit Online |
| 459 | Otmorph 112 | drone | `Ambient/ambient_otmorph_112` | generated: ambientgen |
| 460 | Vowel Alto 016 | drone | `Ambient/ambient_vowel_alto_016` | generated: ambientgen |
| 461 | Breath 033 | drone | `Harmonic/harmonic_breath_033` | generated: harmonicgen |
| 462 | Tube 009 | drone | `Ambient/ambient_tube_009` | generated: ambientgen |
| 463 | Tube 079 | drone | `Ambient/ambient_tube_079` | generated: ambientgen |
| 464 | Sampled 097 | drone | `Ambient/ambient_sampled_097` | measured from the user's own SA3 material |
| 465 | Chord 023 | drone | `Harmonic/harmonic_chord_023` | generated: harmonicgen |
| 466 | Stack 008 | drone | `Harmonic/harmonic_stack_008` | generated: harmonicgen |
| 467 | AKWF 0003-01 | drone | `Classic/akwf_0003_01` | CC0: AKWF |
| 468 | Glass 038 | drone | `Harmonic/harmonic_glass_038` | generated: harmonicgen |
| 469 | Metal 016 | drone | `Harmonic/harmonic_metal_016` | generated: harmonicgen |
