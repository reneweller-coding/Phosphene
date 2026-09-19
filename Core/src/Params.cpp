/**
 * @file Params.cpp
 * @brief Module descriptor tables and the parameter store.
 */
#include "phos/Params.h"
#include "phos/Disperser.h"        // kDisperseStages: the upper end of the disperse parameters
#include "phos/WaveTableFile.h"   // kNumWaveTables and the names of the shipped library tables
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace phos {

const char* const kKeyNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
const char* const kScaleNames[] = { "Aeolian", "Phrygian", "Harmonic Minor", "Phrygian Dominant", "Double Harmonic", "Dorian" };
const char* const kKickPatternNames[] = { "Four", "Four + Fills", "Off" };
// The names of the style profiles and the dramaturgy presets (Form.h). They live here, with the other
// choice tables, because the parameter registry is built from them.
const char* const kStyleNames[] = { "Goa", "Full-On", "Progressive", "Dark Forest", "Hi-Tech" };
const char* const kArcNames[] = { "Warm-up", "Peak-Time", "Morning", "Closing", "Flat" };
const char* const kBassPatternNames[] = { "Rolling", "Gallop", "Skip", "Offbeat", "Triplet" };
// Phase 8: the predictive model behind the melodic lines. "Markov" is the measured order-2 model of
// the corpus (Corpus.h), "Neural" the trained transformer (Model.h). Markov stays the default until
// the trained model has been measured to be better -- and Neural falls back to it when no weight
// file is there.
const char* const kMelodyModelNames[] = { "Markov", "Neural" };
// Phase 8, bass round: where a bass note's pitch comes from. "Pattern" is the root plus the phrase
// figures of Composer.cpp, which is what the program has always played; "Neural" is the learned
// fourth role of Model.h, drawn under the same register and scale constraints. Pattern stays the
// default until a listening comparison exists -- the held-out measurement says the learned bass
// predicts real basslines far better, which is not the same as saying it sounds better -- and
// Neural falls back to Pattern when no weight file is there.
const char* const kBassModelNames[] = { "Pattern", "Neural" };
// 16.09.2026, bass rhythm round: where the onset pattern of a bar comes from. "Pattern" is the five
// hard-wired families of Patterns.h, which is what the program has always played and what every
// earlier render stays bit-identical to; "Corpus" draws the bar from the onset model of Corpus.h --
// a bar-pattern lookup mixed with a parametric chain, both counted on the local bass corpus --
// around the family as the track's home figure, and how far it strays is compose.bass_variation
// times the style profile's hatDensity. Two consequences worth knowing before turning it on: a bar
// drawn this way never puts a note on a kick step, and the Triplet family has no image on the
// sixteenth grid, so a Triplet track plays the Skip mask under "Corpus".
// Pattern stays the default for the reason bass_model does: the held-out measurement says the
// corpus model predicts real bass bars far better, which is not the same as saying it sounds better.
const char* const kBassRhythmNames[] = { "Pattern", "Corpus" };
const char* const kPercRoleNames[kNumPercRoles] = { "Closed Hat", "Open Hat", "Ride", "Crash", "Clap", "Snare", "Rim",
                                                    "Shaker", "Tom", "Conga", "Zap", "Blip" };
// The prefixes of the polyphonic instances, in the order of PolyInstance (Params.h): the voices' groups.
const char* const kPolyInstanceNames[kPolyInstances] = { "lead", "counter", "arp", "stab", "pad", "drone" };

namespace {

const char* const kKickEngineNames[] = { "Sweep", "Resonant" };
const char* const kKickTuneNames[] = { "Free", "Key" };
const char* const kKickClipNames[] = { "Tanh", "Hard" };
const char* const kSubModeNames[] = { "Mixed", "Split" };
const char* const kKickLockNames[] = { "Off", "Bass follows kick", "Kick follows bass" };

const ParamDesc kComposeParams[compose::Count] = {
    { "bpm",             "Tempo",           "BPM", 100.0f, 190.0f, 145.0f, Curve::Linear },
    { "key",             "Key",             "",      0.0f,  11.0f,   6.0f, Curve::Choice, kKeyNames },
    { "scale",           "Scale",           "",      0.0f,   5.0f,   1.0f, Curve::Choice, kScaleNames },
    { "kick_pattern",    "Kick Pattern",    "",      0.0f,   2.0f,   1.0f, Curve::Choice, kKickPatternNames },
    { "bass_pattern",    "Bass Pattern",    "",      0.0f,   4.0f,   0.0f, Curve::Choice, kBassPatternNames },
    { "bass_gate",       "Bass Gate",       "",      0.2f,   1.0f,   0.7f, Curve::Linear },
    { "bass_variation",  "Bass Variation",  "",      0.0f,   1.0f,   0.4f, Curve::Linear },
    { "bass_register",   "Bass Register",   "st",  -12.0f,  12.0f,   0.0f, Curve::Int },
    { "track_bars",      "Track Length",    "bars", 32.0f, 512.0f, 256.0f, Curve::Int },
    { "track_variation", "Track Variation", "",      0.0f,   1.0f,   0.5f, Curve::Linear },
    { "sound_variation", "Sound Variation", "",      0.0f,   1.0f,   0.5f, Curve::Linear },
    // 19.09.2026, round "arrangement": 1 instead of 3, so that the default set (Full-On, 145 BPM) wanders
    // inside the user's Full-On window of 142 .. 146 BPM (144 .. 146) rather than up to 148.
    { "tempo_range",     "Tempo Range",     "BPM",   0.0f,  10.0f,   1.0f, Curve::Linear },
    { "level_match",     "Level Match",     "",      0.0f,   1.0f,   1.0f, Curve::Toggle },
    { "perc_density",    "Perc Density",    "",      0.0f,   1.0f,   0.6f, Curve::Linear },
    { "perc_variation",  "Perc Variation",  "",      0.0f,   1.0f,   0.5f, Curve::Linear },
    { "swing",           "Swing",           "",      0.0f,   0.3f,   0.0f, Curve::Linear },
    { "acid_amount",     "Acid Amount",     "",      0.0f,   1.0f,   0.6f, Curve::Linear },
    { "lead_amount",     "Lead Amount",     "",      0.0f,   1.0f,   0.5f, Curve::Linear },
    { "arp_amount",      "Arp Amount",      "",      0.0f,   1.0f,   0.5f, Curve::Linear },
    { "melody_variation","Melody Variation","",      0.0f,   1.0f,   0.5f, Curve::Linear },
    { "melody_temperature","Melody Temperature","",  0.5f,   2.0f,   1.0f, Curve::Log },
    { "squelch_chance",  "Squelch Chance",  "",      0.0f,   1.0f,   0.3f, Curve::Linear },
    { "bass_follows_chords","Bass Follows Chords","", 0.0f,  1.0f,   0.0f, Curve::Toggle },
    { "pad_amount",      "Pad Amount",      "",      0.0f,   1.0f,   0.7f, Curve::Linear },
    { "sfx_amount",      "SFX Amount",      "",      0.0f,   1.0f,   0.7f, Curve::Linear },
    { "gate_chance",     "Gate Chance",     "",      0.0f,   1.0f,  0.35f, Curve::Linear },
    // Phase 5: the style profile, the dramaturgy of the set and the time base of its energy arc.
    { "style",           "Style",           "",      0.0f,   4.0f,   1.0f, Curve::Choice, kStyleNames },
    { "arc",             "Energy Arc",      "",      0.0f,   4.0f,   4.0f, Curve::Choice, kArcNames },
    { "style_tempo",     "Style Tempo",     "",      0.0f,   1.0f,   0.0f, Curve::Toggle },
    { "set_minutes",     "Set Length",      "min",  10.0f, 300.0f,  60.0f, Curve::Int },
    // Phase 8: stage A or stage B behind the melodic lines.
    { "melody_model",    "Melody Model",    "",      0.0f,   1.0f,   0.0f, Curve::Choice, kMelodyModelNames },
    // Phase 8, bass round: the pattern families or the learned fourth role behind the bass pitches.
    { "bass_model",      "Bass Model",      "",      0.0f,   1.0f,   0.0f, Curve::Choice, kBassModelNames },
    // 16.09.2026: modal interchange. A section may borrow another mode over the track's tonic pedal
    // -- Dorian in the groove, Phrygian in the drive, Phrygian dominant at the peak (Form.h). On by
    // default because that is what the genre does; the bass does not move with it either way.
    { "modal_interchange","Modal Interchange","",     0.0f,   1.0f,   1.0f, Curve::Toggle },
    // 16.09.2026, bass rhythm round: the pattern families or the corpus onset model behind the bass
    // *rhythm*. Appended, so no parameter above it moves; Pattern is the default.
    { "bass_rhythm",     "Bass Rhythm",     "",      0.0f,   1.0f,   0.0f, Curve::Choice, kBassRhythmNames },
    // 19.09.2026, round "voices": the three new voices' share of the tracks (like lead_amount), and two
    // density knobs of the psychedelic layer the round of the same day could not add (it did not own the
    // composer): 1 is the density that round calibrated, 0 removes the voices or the bed, 2 doubles them.
    { "counter_amount",  "Counter Amount",  "",      0.0f,   1.0f,   0.6f, Curve::Linear },
    { "stab_amount",     "Stab Amount",     "",      0.0f,   1.0f,   0.5f, Curve::Linear },
    { "drone_amount",    "Drone Amount",    "",      0.0f,   1.0f,   0.6f, Curve::Linear },
    { "voice_density",   "Voice Density",   "x",     0.0f,   2.0f,   1.0f, Curve::Linear },
    { "bed_density",     "Bed Density",     "x",     0.0f,   2.0f,   1.0f, Curve::Linear },
};

const char* const kPercEngineNames[] = { "Noise", "Metal", "Modal", "Tone", "FM" };
const char* const kModeSetNames[] = { "Membrane", "Bar", "Harmonic" };
const char* const kPercFilterNames[] = { "Low Pass", "Band Pass", "High Pass" };

const ParamDesc kPercParams[perc::Count] = {
    { "active",        "Active",        "",      0.0f,     1.0f,    1.0f, Curve::Toggle },
    { "role",          "Role",          "",      0.0f,    11.0f,    0.0f, Curve::Choice, kPercRoleNames },
    { "engine",        "Engine",        "",      0.0f,     4.0f,    0.0f, Curve::Choice, kPercEngineNames },
    { "pitch",         "Pitch",         "Hz",   40.0f, 12000.0f,  400.0f, Curve::Log },
    { "pitch_amount",  "Pitch Amount",  "x",     1.0f,    16.0f,    1.0f, Curve::Log },
    { "pitch_decay",   "Pitch Decay",   "ms",    0.5f,   300.0f,   10.0f, Curve::Log },
    { "fm_ratio",      "FM Ratio",      "",     0.25f,     8.0f,   1.41f, Curve::Linear },
    { "fm_index",      "FM Index",      "",      0.0f,     8.0f,    0.0f, Curve::Linear },
    { "mode_set",      "Modes",         "",      0.0f,     2.0f,    0.0f, Curve::Choice, kModeSetNames },
    { "mode_damp",     "Mode Damping",  "",      0.0f,     1.0f,    0.5f, Curve::Linear },
    { "metal_scale",   "Metal Scale",   "x",    0.25f,     4.0f,    1.0f, Curve::Log },
    { "noise",         "Noise",         "",      0.0f,     1.0f,    0.0f, Curve::Linear },
    { "noise_decay",   "Noise Decay",   "ms",    2.0f,  3000.0f,   60.0f, Curve::Log },
    { "bursts",        "Bursts",        "",      1.0f,     6.0f,    1.0f, Curve::Int },
    { "burst_spacing", "Burst Spacing", "ms",    2.0f,    40.0f,   10.0f, Curve::Linear },
    { "decay",         "Decay",         "ms",    2.0f,  3000.0f,  120.0f, Curve::Log },
    { "filter",        "Filter",        "",      0.0f,     2.0f,    2.0f, Curve::Choice, kPercFilterNames },
    { "cutoff",        "Cutoff",        "Hz",  100.0f, 18000.0f, 8000.0f, Curve::Log },
    { "resonance",     "Resonance",     "",      0.0f,     1.0f,    0.2f, Curve::Linear },
    { "low_cut",       "Low Cut",       "Hz",  150.0f,  4000.0f,  150.0f, Curve::Log },
    { "drive",         "Drive",         "",      0.0f,     1.0f,    0.0f, Curve::Linear },
    { "level",         "Level",         "dB",  -36.0f,     6.0f,  -12.0f, Curve::Linear },
    { "pan",           "Pan",           "",     -1.0f,     1.0f,    0.0f, Curve::Linear },
    { "choke",         "Choke Group",   "",      0.0f,     4.0f,    0.0f, Curve::Int },
    { "shift",         "Shift",         "ms",  -10.0f,    10.0f,    0.0f, Curve::Linear },
    { "density",       "Density",       "",      0.0f,     1.0f,    0.5f, Curve::Linear },
    { "tune",          "Tune to Key",   "",      0.0f,     1.0f,    0.0f, Curve::Toggle },
    // 16.09.2026, arrangement dynamics. Appended in the order of the perc:: enum, nothing reordered:
    // the indices above sit in saved presets and in the plugin's state.
    // pan_depth defaults to 0 in the table and is set per lane in kDefaultKit below, so that a lane
    // a user builds from scratch stands still until it is told to move. The period is three
    // sixteenths of a bar, and the reason is arithmetic rather than taste (Perc.h): a lane on the
    // sixteenth grid then samples its own swing at three phases 120 degrees apart, which carry the
    // first and the second moment of the sinusoid exactly, so the kit's measured width does not move.
    { "pan_depth",     "Pan Depth",     "",      0.0f,     1.0f,    0.0f, Curve::Linear },
    { "pan_bars",      "Pan Period",    "bars",  0.0625f, 16.0f,  0.1875f, Curve::Log },
    { "cut_track",     "Cut Tracks Pitch","",    0.0f,     2.0f,    0.0f, Curve::Linear },
};

/**
 * @brief The default kit: what each of the twelve lanes starts as.
 *
 * Levels and positions follow the reference measurement of 15.09.2026 (docs/PLAN.md, 6.6): the
 * offbeat hat is the loudest top-end event, the sixteenth layer (shaker, closed-hat ghosts) sits
 * around half of it. Low cuts never go under 150 Hz: below that only kick and bass may play.
 *
 * **Band limits on the top-end lanes (16.09.2026, Phase 9).** Hat, open hat and shaker are a
 * band pass built from the lane's own two filters -- the 24 dB/octave low cut below, the main
 * filter as a low pass above -- not a plain high pass. A high pass on a spectrally flat source
 * (white noise, inharmonic metal partials) rises all the way to Nyquist, which put the kit's energy
 * above the air band instead of inside it: measured against the 39 reference recordings the mix was
 * 3.8 dB short at 8 kHz and 5.1 dB *over* at 16 kHz. Real cymbals are band limited (Fletcher and
 * Rossing, "The Physics of Musical Instruments", ch. 19: the modes of a thin plate crowd towards a
 * finite upper region and are damped hardest there), and so is every reference track measured here.
 *
 * **The positions across the stereo field (16.09.2026).** The kit owns the air band -- in the solo
 * table of Phase 9 it is 60 % of 6 .. 16 kHz and 95 % of it in a track without a lead -- so the width
 * of the mix up there is the width of the kit, and the kit was 9 dB narrower than the recordings:
 * side over mid -17.6 dB against a reference median of -8.5 (`Tools/ref_width.py`, forty recordings,
 * four windows each). The cause was in these lines: the two loudest lanes of the top end, closed hat
 * at +5 dB and open hat at +2, sat at 0.15 and -0.10, close enough to the middle that the rest of the
 * kit could not widen the sum. The positions of the ten lanes that are not the backbeat are now
 * 0.35 .. 0.60 of full deflection, alternating side by side as they did before, and the kit measures
 * -8.7 dB with an inter-channel correlation of +0.79 against the recordings' +0.75.
 *
 * Three things this does not disturb, two of them by construction. Panning here is constant power
 * (`Perc.cpp`), so cos^2 + sin^2 = 1 and the two channels' powers **sum** to the same figure at any
 * position: the band balance of Phase 9 cannot move, and measured over eight seeds it does not.
 * Summing to mono cannot cancel a panned source either, only a phase-inverted one, and the measured
 * cost of a mono downmix stays at 0.8 dB of presence against the recordings' 1.2. And the depth rule
 * is untouched: no lane's low cut goes under 150 Hz, so nothing below 140 Hz is being placed anywhere.
 * Clap and snare stay in the middle -- the backbeat is the one thing a psytrance mix anchors there,
 * and the presence band they live in already measured inside the recordings' quartiles.
 *
 * **Levels per role (18.09.2026, round "mix-foundation").** The user heard only hats and kick. The
 * kit was composed -- 770 congas and 288 toms in track 1 of the listening seed -- but not heard:
 * rendered lane by lane in the first drop (all lanes active so the composition does not move, the
 * others at -36 dB, the floor subtracted), tom and conga stood at -0.7 and -1.3 dB against the rest of
 * the mix in their own octave (250 .. 500 Hz), where arp and acid live. Tom +6 dB, conga +7, clap +3,
 * snare +2, ride +3, rim, zap and blip +4; the arp gives 2 dB back (mix.arp_level). The acid, whose
 * driven default also lives in that octave, takes back part of it. Two more dB on tom and conga
 * would have bought more, and were tried: they push the level match of testVariety (spread of four
 * tracks, perc, kick and bass only) from 0.75 to 0.85 LU, over its 0.8 bound -- the composer's
 * loudness probe does not see how much of a track the toms and congas play. The measurements
 * before and after are in docs/PLAN.md.
 */
const char* const kDefaultKit =
    "perc1.role=Closed Hat;perc1.engine=Metal;perc1.decay=45;perc1.noise=0.35;perc1.noise_decay=35;perc1.filter=Low Pass;"
    "perc1.cutoff=12000;perc1.low_cut=3500;perc1.resonance=0.25;perc1.level=5;perc1.pan=0.45;perc1.pan_depth=1;perc1.choke=1;perc1.density=0.6\n"
    "perc2.role=Open Hat;perc2.engine=Metal;perc2.decay=260;perc2.noise=0.35;perc2.noise_decay=220;perc2.filter=Low Pass;"
    "perc2.cutoff=12000;perc2.low_cut=3000;perc2.level=2;perc2.pan=-0.4;perc2.pan_depth=1;perc2.choke=1\n"
    "perc3.role=Ride;perc3.engine=Metal;perc3.metal_scale=0.72;perc3.decay=700;perc3.noise=0.2;perc3.noise_decay=400;"
    "perc3.filter=Band Pass;perc3.cutoff=5200;perc3.resonance=0.35;perc3.level=0;perc3.pan=0.6;perc3.pan_depth=1\n"
    "perc4.role=Crash;perc4.engine=Metal;perc4.metal_scale=0.5;perc4.decay=1600;perc4.noise=0.6;perc4.noise_decay=1400;"
    "perc4.filter=High Pass;perc4.cutoff=3000;perc4.level=-5;perc4.pan=-0.55;perc4.pan_depth=1\n"
    "perc5.role=Clap;perc5.engine=Noise;perc5.noise=1;perc5.bursts=4;perc5.burst_spacing=9;perc5.noise_decay=180;"
    "perc5.filter=Band Pass;perc5.cutoff=1400;perc5.resonance=0.35;perc5.level=-1;perc5.low_cut=300\n"
    "perc6.role=Snare;perc6.engine=Tone;perc6.pitch=190;perc6.pitch_amount=1.6;perc6.pitch_decay=25;perc6.decay=90;"
    "perc6.noise=0.8;perc6.noise_decay=140;perc6.filter=High Pass;perc6.cutoff=250;perc6.level=-4;perc6.low_cut=160;perc6.cut_track=2\n"
    "perc7.role=Rim;perc7.engine=FM;perc7.pitch=1700;perc7.fm_ratio=2.61;perc7.fm_index=2.2;perc7.decay=28;perc7.pitch_decay=6;"
    "perc7.filter=Band Pass;perc7.cutoff=2200;perc7.resonance=0.3;perc7.level=-6;perc7.pan=-0.45;perc7.pan_depth=1\n"
    "perc8.role=Shaker;perc8.engine=Noise;perc8.noise=1;perc8.noise_decay=45;perc8.filter=Low Pass;perc8.cutoff=11000;"
    "perc8.low_cut=3000;perc8.resonance=0.1;perc8.level=-1;perc8.pan=0.55;perc8.pan_depth=1\n"
    "perc9.role=Tom;perc9.engine=Modal;perc9.pitch=220;perc9.low_cut=190;perc9.mode_set=Membrane;perc9.mode_damp=0.6;perc9.decay=280;"
    "perc9.noise=0.08;perc9.noise_decay=15;perc9.filter=Low Pass;perc9.cutoff=6000;perc9.level=-2;perc9.pan=-0.35;perc9.pan_depth=1;perc9.tune=1\n"
    "perc10.role=Conga;perc10.engine=Modal;perc10.pitch=330;perc10.mode_set=Harmonic;perc10.mode_damp=0.4;perc10.decay=180;"
    "perc10.noise=0.05;perc10.noise_decay=8;perc10.filter=Low Pass;perc10.cutoff=8000;perc10.low_cut=220;perc10.level=-3;perc10.pan=0.5;perc10.pan_depth=1;perc10.tune=1\n"
    "perc11.role=Zap;perc11.engine=FM;perc11.pitch=420;perc11.pitch_amount=8;perc11.pitch_decay=35;perc11.fm_ratio=1.5;"
    "perc11.fm_index=3;perc11.decay=110;perc11.filter=Low Pass;perc11.cutoff=9000;perc11.resonance=0.4;perc11.drive=0.3;"
    "perc11.level=-8;perc11.pan=0.6;perc11.pan_depth=1\n"
    "perc12.role=Blip;perc12.engine=Tone;perc12.pitch=1100;perc12.pitch_amount=1.3;perc12.pitch_decay=4;perc12.decay=45;"
    "perc12.filter=Band Pass;perc12.cutoff=1800;perc12.resonance=0.2;perc12.level=-8;perc12.pan=-0.6;perc12.pan_depth=1;perc12.tune=1\n";

// 19.09.2026 (round "lowend-acid"): Decay 150 -> 240 ms and Tail Limit -24 -> -15 dB give the kick the
// reference kicks' body (Kick.h, "Kick body and the limit"); Level -2 -> -6 dB, because the louder,
// heavier bass now shares the 40 .. 140 Hz band the mix is calibrated against (testMixBalance), and the
// bass-to-kick ratio lands on the references' (Tools/ref_bass.py, "b/k": -4.2 against a median -4.7).
const ParamDesc kKickParams[kick::Count] = {
    { "engine",      "Engine",       "",     0.0f,     1.0f,    0.0f, Curve::Choice, kKickEngineNames },
    { "tune",        "Tune",         "",     0.0f,     1.0f,    1.0f, Curve::Choice, kKickTuneNames },
    { "pitch_end",   "Pitch End",    "Hz",  30.0f,   120.0f,   50.0f, Curve::Log },
    { "pitch_start", "Pitch Start",  "Hz",  60.0f,  1500.0f,  330.0f, Curve::Log },
    { "pitch_decay", "Body Decay",   "ms",   5.0f,   150.0f,   13.0f, Curve::Log },
    { "punch_decay", "Punch Decay",  "ms",   0.5f,    20.0f,    4.0f, Curve::Log },
    { "punch",       "Punch",        "",     0.0f,     1.0f,    0.5f, Curve::Linear },
    { "amp_attack",  "Attack",       "ms",   0.0f,    10.0f,    0.2f, Curve::Linear },
    { "amp_hold",    "Hold",         "ms",   0.0f,   150.0f,   12.0f, Curve::Linear },
    { "amp_decay",   "Decay",        "ms",  20.0f,  1500.0f,  240.0f, Curve::Log },
    { "drive",       "Drive",        "",     0.0f,     1.0f,   0.30f, Curve::Linear },
    { "clip",        "Clip",         "",     0.0f,     1.0f,    0.0f, Curve::Choice, kKickClipNames },
    { "click_level", "Click",        "",     0.0f,     1.0f,    0.5f, Curve::Linear },
    { "click_tone",  "Click Tone",   "Hz", 500.0f, 12000.0f, 4000.0f, Curve::Log },
    { "click_decay", "Click Decay",  "ms",   0.5f,    30.0f,    3.0f, Curve::Log },
    { "tone",        "Tone",         "Hz", 200.0f, 20000.0f, 9000.0f, Curve::Log },
    { "level",       "Level",        "dB", -36.0f,     6.0f,   -6.0f, Curve::Linear },
    { "tail_limit",  "Tail Limit",   "dB", -60.0f,     0.0f,  -15.0f, Curve::Linear },
};

// 19.09.2026 (round "lowend-acid"): calibrated against Tools/ref_bass.py (24 reference recordings, the
// sixteenths between the kicks, each band against 20 .. 120 Hz; self test testBassBite). Sub 0.6 -> 0.3
// and the new Sub Octave 0.5: under 60 Hz -5.7 dB, 60 .. 120 Hz -1.4 dB (references -7.4 and -0.9).
// Cutoff 140 -> 240 Hz for 120 .. 300 Hz. The new bite layer: 300 Hz .. 2 kHz -10.3 dB (references
// -9.8; the bass before read -21.6). Level -5 -> -3 dB: bass against kick, see the kick table. Split
// stays 2 x f0 with a steeper filter (Bass.h).
const ParamDesc kBassParams[bass::Count] = {
    { "wave",          "Wave",          "",      0.0f,     1.0f,  0.15f, Curve::Linear },
    { "pulse_width",   "Pulse Width",   "",     0.05f,    0.95f,   0.5f, Curve::Linear },
    { "sub",           "Sub",           "",      0.0f,     1.0f,   0.3f, Curve::Linear },
    { "sub_mode",      "Sub Mode",      "",      0.0f,     1.0f,   1.0f, Curve::Choice, kSubModeNames },
    { "split_ratio",   "Split",         "x f0",  1.2f,     3.0f,   2.0f, Curve::Linear },
    { "kick_lock",     "Kick Lock",     "",      0.0f,     2.0f,   2.0f, Curve::Choice, kKickLockNames },
    { "retrigger",     "Retrigger",     "",      0.0f,     1.0f,   1.0f, Curve::Toggle },
    { "start_phase",   "Start Phase",   "",      0.0f,     1.0f,   0.5f, Curve::Linear },
    { "cutoff",        "Cutoff",        "Hz",   20.0f, 10000.0f, 240.0f, Curve::Log },
    { "resonance",     "Resonance",     "",      0.0f,     1.0f,   0.3f, Curve::Linear },
    { "env_amount",    "Env Amount",    "oct",   0.0f,     8.0f,   4.0f, Curve::Linear },
    { "filter_decay",  "Filter Decay",  "ms",    3.0f,  1000.0f,  75.0f, Curve::Log },
    { "key_track",     "Key Track",     "",      0.0f,     1.0f,   0.6f, Curve::Linear },
    { "vel_to_cutoff", "Vel > Cutoff",  "",      0.0f,     1.0f,  0.25f, Curve::Linear },
    { "drive",         "Drive",         "",      0.0f,     1.0f,   0.3f, Curve::Linear },
    { "amp_attack",    "Attack",        "ms",    0.1f,    50.0f,   0.8f, Curve::Log },
    { "amp_decay",     "Decay",         "ms",    5.0f,  2000.0f, 180.0f, Curve::Log },
    { "amp_sustain",   "Sustain",       "",      0.0f,     1.0f,  0.55f, Curve::Linear },
    { "amp_release",   "Release",       "ms",    1.0f,   500.0f,  10.0f, Curve::Log },
    { "duck_depth",    "Duck Depth",    "",      0.0f,     1.0f,   0.5f, Curve::Linear },
    { "duck_hold",     "Duck Hold",     "ms",    0.0f,   200.0f,  25.0f, Curve::Linear },
    { "duck_release",  "Duck Release",  "ms",    5.0f,   500.0f,  60.0f, Curve::Log },
    { "level",         "Level",         "dB",  -36.0f,     6.0f,  -3.0f, Curve::Linear },
    // Appended 19.09.2026 (round "lowend-acid"): the bite layer, a saturated copy of the oscillator
    // low-passed around 400 .. 800 Hz with its own envelope (Bass.h, "Bite"). The envelope opens the
    // cutoff three octaves at the onset and closes it within 90 ms: the pluck of the roll.
    { "bite",           "Bite",          "",      0.0f,     1.0f,   0.5f, Curve::Linear },
    { "bite_cutoff",    "Bite Cutoff",   "Hz",  100.0f,  4000.0f, 500.0f, Curve::Log },
    { "bite_env",       "Bite Env",      "oct",   0.0f,     5.0f,   3.0f, Curve::Linear },
    { "bite_decay",     "Bite Decay",    "ms",    5.0f,  1000.0f,  90.0f, Curve::Log },
    { "bite_drive",     "Bite Drive",    "",      0.0f,     1.0f,   0.5f, Curve::Linear },
    { "bite_resonance", "Bite Reso",     "",      0.0f,     1.0f,   0.2f, Curve::Linear },
    // The octave of the sub: a sine at twice the fundamental, phase-locked to it, in the sub's level
    // units. It carries the 60 .. 120 Hz weight the references have without a filter near the
    // fundamental (Bass.h, "Sub octave").
    { "sub_octave",     "Sub Octave",    "",      0.0f,     1.0f,   0.5f, Curve::Linear },
};

const char* const kDelayTimeNames[] = { "1/16", "1/8", "3/16", "1/4", "3/8", "1/2" };
const char* const kPolyOscNames[] = { "Supersaw", "VA", "FM", "Wavetable" };
// The `table` choice: the six tables written as spectra in code, then whatever library tables this
// build ships (Core/include/phos/WaveTableList.inl, generated by Tools/wt_pack.py). The six come
// first and in this order for good: a `.phosset` and a plugin state store the index as a number, so
// "lead.table=1" must go on meaning Vocal whatever is added behind it.
#define PHOS_WT(index, name, id, lane, fallback) name,
const char* const kWaveTableNames[] = { "Classic", "Vocal", "Glass", "PWM", "Sync", "Formant Saw",
#include "phos/WaveTableList.inl"
};
#undef PHOS_WT
static_assert(sizeof(kWaveTableNames) / sizeof(kWaveTableNames[0]) == kNumWaveTables,
              "the table choice list and kNumWaveTables have come apart");
const char* const kGatePatternNames[] = { "Sixteenths", "Eighths", "Rolling", "Gallop", "3-3-2", "Triplets" };
const char* const kPolyFilterNames[] = { "Low Pass", "Band Pass", "High Pass", "Notch" };
static_assert(sizeof(kPolyFilterNames) / sizeof(kPolyFilterNames[0]) == static_cast<int>(PolyFilter::Count),
              "one name per PolyFilter");

// 18.09.2026 (round "mix-foundation"): the default voicing is the "driven" one of three candidates
// rendered for the user -- a 303 into a distortion pedal. Cutoff 650 -> 900 Hz, env 4 -> 3.5 oct,
// decay 350 -> 220 ms, accent 0.6 -> 0.7, slide 55 -> 70 ms (the 60 .. 80 ms of the rule text), drive
// 0.45 -> 0.85 on the new, stronger drive curve (Acid.cpp, kDriveMaxDb), low cut 150 -> 250 Hz so the
// distortion does not thicken the low mids, delay feedback 0.45 -> 0.5, hall 0.05 -> 0.08, and the
// level -9 -> -2.3 dB, which puts the acid alone at -20 LUFS in the first drop, level with the arp.
const ParamDesc kAcidParams[acid::Count] = {
    { "wave",           "Wave",           "",      0.0f,     1.0f,   0.0f, Curve::Linear },
    { "cutoff",         "Cutoff",         "Hz",   80.0f,  8000.0f, 900.0f, Curve::Log },
    { "resonance",      "Resonance",      "",      0.0f,     1.0f,  0.72f, Curve::Linear },
    { "env_amount",     "Env Amount",     "oct",   0.0f,     6.0f,   3.5f, Curve::Linear },
    { "decay",          "Decay",          "ms",   30.0f,  2000.0f, 220.0f, Curve::Log },
    { "accent",         "Accent",         "",      0.0f,     1.0f,   0.7f, Curve::Linear },
    { "slide_time",     "Slide Time",     "ms",    5.0f,   200.0f,  70.0f, Curve::Log },
    { "amp_decay",      "Amp Decay",      "ms",   50.0f,  4000.0f, 900.0f, Curve::Log },
    { "key_track",      "Key Track",      "",      0.0f,     1.0f,   0.5f, Curve::Linear },
    { "drive",          "Drive",          "",      0.0f,     1.0f,  0.85f, Curve::Linear },
    { "squelch",        "Squelch",        "",      0.0f,     1.0f,   0.0f, Curve::Toggle },
    { "squelch_start",  "Squelch Start",  "x",     2.0f,    32.0f,  12.0f, Curve::Log },
    { "squelch_time",   "Squelch Time",   "ms",    5.0f,   150.0f,  28.0f, Curve::Log },
    { "comb_mix",       "Comb Mix",       "",      0.0f,     1.0f,  0.55f, Curve::Linear },
    { "comb_feedback",  "Comb Feedback",  "",      0.0f,    0.97f,  0.82f, Curve::Linear },
    { "low_cut",        "Low Cut",        "Hz",  150.0f,  1000.0f, 250.0f, Curve::Log },
    { "delay_send",     "Delay Send",     "",      0.0f,     1.0f,  0.25f, Curve::Linear },
    { "delay_left",     "Delay Left",     "",      0.0f,     5.0f,   2.0f, Curve::Choice, kDelayTimeNames },
    { "delay_right",    "Delay Right",    "",      0.0f,     5.0f,   3.0f, Curve::Choice, kDelayTimeNames },
    { "delay_feedback", "Delay Feedback", "",      0.0f,     0.9f,  0.50f, Curve::Linear },
    { "delay_high_pass","Delay High Pass","Hz",  150.0f,  2000.0f, 400.0f, Curve::Log },
    { "delay_low_pass", "Delay Low Pass", "Hz",  800.0f, 16000.0f,4500.0f, Curve::Log },
    { "room_send",      "Room Send",      "",      0.0f,     1.0f,   0.1f, Curve::Linear },
    { "hall_send",      "Hall Send",      "",      0.0f,     1.0f,  0.08f, Curve::Linear },
    { "duck",           "Duck",           "",      0.0f,     1.0f,  0.15f, Curve::Linear },
    { "level",          "Level",          "dB",  -36.0f,     6.0f,  -2.3f, Curve::Linear },
    // Appended 16.09.2026 (acid colour round). Off by default: an all-pass chain changes no band's
    // power, but it does smear the attack, and no measurement asks for that to be the default sound.
    { "disperse",       "Disperse",       "x",     0.0f, static_cast<float>(kDisperseStages), 0.0f, Curve::Int },
    { "disperse_freq",  "Disperse Freq",  "Hz",  200.0f,  8000.0f, 1250.0f, Curve::Log },
};

const ParamDesc kPolyParams[poly::Count] = {
    { "osc",            "Oscillator",     "",      0.0f,     3.0f,   0.0f, Curve::Choice, kPolyOscNames },
    { "detune",         "Detune",         "",      0.0f,     1.0f,  0.55f, Curve::Linear },
    { "mix",            "Mix",            "",      0.0f,     1.0f,  0.75f, Curve::Linear },
    { "dynamic_detune", "Dynamic Detune", "",      0.0f,     1.0f,   0.6f, Curve::Linear },
    { "wave",           "Wave",           "",      0.0f,     1.0f,   0.0f, Curve::Linear },
    { "pulse_width",    "Pulse Width",    "",     0.05f,    0.95f,   0.5f, Curve::Linear },
    { "fm_ratio",       "FM Ratio",       "",      0.5f,     8.0f,   2.0f, Curve::Linear },
    { "fm_index",       "FM Index",       "",      0.0f,    10.0f,   2.5f, Curve::Linear },
    { "fm_decay",       "FM Decay",       "ms",    5.0f,  2000.0f, 250.0f, Curve::Log },
    { "table",          "Table",          "",      0.0f, static_cast<float>(kNumWaveTables - 1), 1.0f, Curve::Choice, kWaveTableNames },
    { "position",       "Position",       "",      0.0f,     1.0f,   0.3f, Curve::Linear },
    { "pos_env",        "Position Env",   "",     -1.0f,     1.0f,   0.0f, Curve::Linear },
    { "pos_decay",      "Position Decay", "ms",   10.0f,  8000.0f, 1500.0f, Curve::Log },
    { "pos_lfo_depth",  "Position LFO",   "",      0.0f,     0.5f,  0.15f, Curve::Linear },
    { "pos_lfo_beats",  "LFO Period",     "beats", 0.25f,   64.0f,  16.0f, Curve::Log },
    { "cutoff",         "Cutoff",         "Hz",  200.0f, 18000.0f,10000.0f, Curve::Log },
    { "resonance",      "Resonance",      "",      0.0f,     1.0f,  0.15f, Curve::Linear },
    { "env_amount",     "Env Amount",     "oct",   0.0f,     6.0f,   2.0f, Curve::Linear },
    { "filter_decay",   "Filter Decay",   "ms",    5.0f,  3000.0f, 400.0f, Curve::Log },
    { "key_track",      "Key Track",      "",      0.0f,     1.0f,   0.5f, Curve::Linear },
    // The floor reaches down to 40 Hz since 18.09.2026: the pad's sub foundation (Melody.h, rule 20)
    // needs its high pass under 73 Hz where kick and bass rest, and a range that stopped at 150 Hz
    // could not be reached by a control event. The defaults of lead and arp (220 Hz) keep the depth
    // rule where they always did; a text or a state that stores 150..400 Hz reads back unchanged.
    { "hp_floor",       "HP Floor",       "Hz",   40.0f,   400.0f, 220.0f, Curve::Log },
    { "hp_track",       "HP Track",       "x f0",  0.0f,     1.0f,   1.0f, Curve::Linear },
    { "amp_attack",     "Attack",         "ms",    0.3f,  2000.0f,   4.0f, Curve::Log },
    { "amp_decay",      "Decay",          "ms",    5.0f,  4000.0f, 500.0f, Curve::Log },
    { "amp_sustain",    "Sustain",        "",      0.0f,     1.0f,  0.75f, Curve::Linear },
    { "amp_release",    "Release",        "ms",    5.0f,  4000.0f, 180.0f, Curve::Log },
    { "width",          "Width",          "",      0.0f,     1.0f,   0.8f, Curve::Linear },
    { "vel_sens",       "Velocity",       "",      0.0f,     1.0f,   0.4f, Curve::Linear },
    { "delay_send",     "Delay Send",     "",      0.0f,     1.0f,   0.3f, Curve::Linear },
    { "delay_left",     "Delay Left",     "",      0.0f,     5.0f,   2.0f, Curve::Choice, kDelayTimeNames },
    { "delay_right",    "Delay Right",    "",      0.0f,     5.0f,   3.0f, Curve::Choice, kDelayTimeNames },
    { "delay_feedback", "Delay Feedback", "",      0.0f,     0.9f,   0.4f, Curve::Linear },
    { "delay_high_pass","Delay High Pass","Hz",  150.0f,  2000.0f, 350.0f, Curve::Log },
    { "delay_low_pass", "Delay Low Pass", "Hz",  800.0f, 16000.0f,6000.0f, Curve::Log },
    { "room_send",      "Room Send",      "",      0.0f,     1.0f,   0.0f, Curve::Linear },
    { "hall_send",      "Hall Send",      "",      0.0f,     1.0f,  0.25f, Curve::Linear },
    { "duck",           "Duck",           "",      0.0f,     1.0f,   0.2f, Curve::Linear },
    { "gate",           "Trance Gate",    "",      0.0f,     1.0f,   0.0f, Curve::Toggle },
    { "gate_pattern",   "Gate Pattern",   "",      0.0f,     5.0f,   0.0f, Curve::Choice, kGatePatternNames },
    { "gate_depth",     "Gate Depth",     "",      0.0f,     1.0f,  0.85f, Curve::Linear },
    { "gate_duty",      "Gate Duty",      "",     0.05f,     1.0f,   0.5f, Curve::Linear },
    { "gate_attack",    "Gate Attack",    "ms",    0.5f,    60.0f,   3.0f, Curve::Log },
    { "gate_release",   "Gate Release",   "ms",    0.5f,   120.0f,  12.0f, Curve::Log },
    { "gate_tone",      "Gate Tone",      "",      0.0f,     1.0f,   0.4f, Curve::Linear },
    { "level",          "Level",          "dB",  -36.0f,     6.0f,  -4.0f, Curve::Linear },
    // Appended 16.09.2026 (acid colour round).
    { "disperse",       "Disperse",       "x",     0.0f, static_cast<float>(kDisperseStages), 0.0f, Curve::Int },
    { "disperse_freq",  "Disperse Freq",  "Hz",  200.0f,  8000.0f, 1250.0f, Curve::Log },
    // Thermal drift: the standard deviation of the slow random walk, in cents (Poly.h). 1 cent is
    // +-2 cents at two sigma, the range the analogue literature gives for a warmed-up VCO.
    { "drift",          "Drift",          "ct",    0.0f,     8.0f,   1.0f, Curve::Linear },
    // 19.09.2026 (round "voices"): which output of the voice's state-variable filter is heard (PolyKernel.h).
    // Low pass is what every voice played before; the per-track recipes (Composer.cpp) may pick the others.
    { "filter_type",    "Filter Type",    "",      0.0f,     3.0f,   0.0f, Curve::Choice, kPolyFilterNames },
};

/**
 * @brief Where the two polyphonic instances start: the lead a held supersaw, the arp a plucked one
 *        with less detune (a short sixteenth smears when its seven saws beat against each other).
 *
 * 18.09.2026 (Melody.h, genre rules): the arp's release is 12 ms instead of 90 ms. The arp plays
 * every sixteenth now, and rule 15 asks for a gate of 15 to 35 % of a sixteenth; with the note at a
 * fifth of a sixteenth and 90 ms of release it sounded for 3.5 sixteenths (self test, section "genre
 * rules", measured before). The pad's high-pass floor is 140 Hz instead of 200 Hz: its voicings now
 * start at D3 (147 Hz) in root position (rule 19), and at 200 Hz the root lost 11 dB; with the floor
 * under it the tracking high pass at f0 treats the root like every other voice.
 */
const char* const kDefaultPoly =
    "arp.detune=0.3;arp.mix=0.6;arp.cutoff=3500;arp.env_amount=2.8;arp.filter_decay=140;arp.resonance=0.3;"
    "arp.amp_attack=0.8;arp.amp_decay=220;arp.amp_sustain=0;arp.amp_release=12;arp.delay_send=0.35;"
    "arp.delay_left=2;arp.delay_right=1;arp.level=-5;arp.width=0.6;arp.hall_send=0.15;arp.duck=0.25\n"
    "pad.osc=Wavetable;pad.table=Vocal;pad.detune=0.35;pad.mix=0.7;pad.dynamic_detune=0;pad.cutoff=5000;pad.env_amount=0;pad.resonance=0.1;"
    "pad.amp_attack=700;pad.amp_decay=2000;pad.amp_sustain=1;pad.amp_release=1800;pad.hp_floor=140;pad.hp_track=1;pad.width=1;"
    "pad.delay_send=0;pad.hall_send=0.45;pad.duck=0.5;pad.pos_env=0.3;pad.pos_decay=3000;pad.gate_pattern=Sixteenths;pad.level=-16\n"
    // 19.09.2026, round "voices". The counter-lead answers the lead in another timbre -- the user's
    // inventory names "wavetable / vocal character" -- so it is a formant saw read by the wavetable
    // oscillator with a slow vowel movement of the position and a quarter-note echo, and no supersaw.
    "counter.osc=Wavetable;counter.table=Formant Saw;counter.detune=0.25;counter.mix=0.55;counter.dynamic_detune=0.3;"
    "counter.cutoff=5000;counter.env_amount=1.5;counter.filter_decay=300;counter.resonance=0.2;counter.position=0.4;"
    "counter.pos_lfo_depth=0.25;counter.pos_lfo_beats=4;counter.amp_attack=3;counter.amp_decay=400;counter.amp_sustain=0.6;"
    "counter.amp_release=110;counter.delay_send=0.4;counter.delay_left=3;counter.delay_right=2;counter.hall_send=0.3;"
    "counter.width=0.7;counter.duck=0.25;counter.level=-7\n"
    // The stab: a short, bright chord -- a narrow supersaw through a filter envelope that closes within
    // 90 ms, no sustain, and throws into the delay and the hall so the hit leaves an echo behind it.
    "stab.detune=0.35;stab.mix=0.6;stab.dynamic_detune=0;stab.cutoff=1800;stab.env_amount=3.5;stab.filter_decay=90;"
    "stab.resonance=0.35;stab.amp_attack=0.5;stab.amp_decay=180;stab.amp_sustain=0;stab.amp_release=60;stab.delay_send=0.45;"
    "stab.delay_left=2;stab.delay_right=3;stab.delay_feedback=0.45;stab.hall_send=0.35;stab.width=0.9;stab.duck=0.3;stab.level=-7\n"
    // The tonic drone: a warm organ table held across a section, dark, slow in and slow out (the
    // cross-fade at section boundaries), with more thermal drift than the other voices. Its high pass
    // tracks an octave under each voice with a floor at 140 Hz; where kick and bass rest the composer
    // opens the floor to 40 Hz for its low octave, as it does for the pad's sub foundation.
    "drone.osc=Wavetable;drone.table=Organ 034;drone.detune=0.2;drone.mix=0.5;drone.dynamic_detune=0;drone.cutoff=900;"
    "drone.env_amount=0;drone.resonance=0.1;drone.amp_attack=1500;drone.amp_decay=2000;drone.amp_sustain=1;"
    "drone.amp_release=2500;drone.hp_floor=140;drone.hp_track=0.5;drone.width=1;drone.delay_send=0;drone.hall_send=0.35;"
    "drone.duck=0.35;drone.position=0.3;drone.pos_lfo_depth=0.1;drone.pos_lfo_beats=32;drone.drift=2;drone.level=-14\n";

// sfx.level -12 -> -3 dB (18.09.2026): the SFX strip measured -27.6 LUFS over the first drop and its
// events 5 to 15 dB under the mix at their loudest; the per-type balance is in Sfx.cpp (kTypeGainDb).
const ParamDesc kSfxParams[sfx::Count] = {
    { "level",        "Level",         "dB", -36.0f,   6.0f,  -3.0f, Curve::Linear },
    { "noise",        "Noise",         "",     0.0f,   1.0f,   0.6f, Curve::Linear },
    { "resonance",    "Resonance",     "",     0.0f,   1.0f,   0.5f, Curve::Linear },
    { "brightness",   "Brightness",    "",     0.0f,   1.0f,   0.5f, Curve::Linear },
    { "impact_decay", "Impact Decay",  "ms",  200.0f, 4000.0f, 1400.0f, Curve::Log },
    { "vowel",        "Vowel",         "",     0.0f,   1.0f,   0.3f, Curve::Linear },
    { "swell_decay",  "Swell Decay",   "ms",  200.0f, 6000.0f, 1500.0f, Curve::Log },
    { "width",        "Width",         "",     0.0f,   1.0f,   0.7f, Curve::Linear },
    { "room_send",    "Room Send",     "",     0.0f,   1.0f,   0.0f, Curve::Linear },
    { "hall_send",    "Hall Send",     "",     0.0f,   1.0f,   0.5f, Curve::Linear },
    { "duck",         "Duck",          "",     0.0f,   1.0f,   0.0f, Curve::Linear },
    // 19.09.2026, round "fx-psychedelia" (Sfx.h): the sub drop, mono and under the kick's sidechain.
    { "sub_level",    "Sub Drop",      "dB", -36.0f,   6.0f, -14.0f, Curve::Linear },
    { "sub_duck",     "Sub Duck",      "",     0.0f,   1.0f,   1.0f, Curve::Linear },
};

// The shamanic bed (Texture.h), the voices (Vocal.h) and the modulation effects (PsyFx.h), 19.09.2026.
// Their levels are the calibration of that round (docs/PLAN.md): a bed that is felt rather than heard,
// voices that stand in a breakdown without covering the pad.
const ParamDesc kTextureParams[texture::Count] = {
    { "width",         "Width",          "",     0.0f,   1.0f,   0.8f, Curve::Linear },
    { "bowl_decay",    "Bowl Decay",     "s",    1.0f,  20.0f,   7.0f, Curve::Log },
    { "bowl_bright",   "Bowl Brightness","",     0.0f,   1.0f,   0.4f, Curve::Linear },
    { "didge_formant", "Didge Formant",  "",     0.0f,   1.0f,   0.5f, Curve::Linear },
    { "didge_breath",  "Didge Breath",   "",     0.0f,   1.0f,   0.5f, Curve::Linear },
    { "jaw_sweep",     "Jaw Harp Sweep", "",     0.0f,   1.0f,   0.6f, Curve::Linear },
    { "room_send",     "Room Send",      "",     0.0f,   1.0f,   0.1f, Curve::Linear },
    { "hall_send",     "Hall Send",      "",     0.0f,   1.0f,   0.35f, Curve::Linear },
    { "fx_send",       "FX Send",        "",     0.0f,   1.0f,   0.3f, Curve::Linear },
    { "duck",          "Duck",           "",     0.0f,   1.0f,   0.3f, Curve::Linear },
};

const ParamDesc kVocalParams[vocal::Count] = {
    { "pitch",          "Pitch Spread",   "st",   0.0f,  12.0f,   3.0f, Curve::Linear },
    { "drive",          "Drive",          "",     0.0f,   1.0f,   0.35f, Curve::Linear },
    { "throw_send",     "Delay Throw",    "",     0.0f,   1.0f,   0.6f, Curve::Linear },
    { "throw_beats",    "Throw Time",     "",     0.0f,   5.0f,   4.0f, Curve::Choice, kDelayTimeNames },
    { "throw_feedback", "Throw Feedback", "",     0.0f,   0.9f,   0.55f, Curve::Linear },
    { "fx_send",        "FX Send",        "",     0.0f,   1.0f,   0.45f, Curve::Linear },
    { "hall_send",      "Hall Send",      "",     0.0f,   1.0f,   0.3f, Curve::Linear },
    { "duck",           "Duck",           "",     0.0f,   1.0f,   0.3f, Curve::Linear },
    { "width",          "Width",          "",     0.0f,   1.0f,   0.5f, Curve::Linear },
};

const ParamDesc kPsyFxParams[psyfx::Count] = {
    { "flanger_beats",    "Flanger Period",   "beats", 0.5f, 32.0f,  8.0f, Curve::Log },
    { "flanger_depth",    "Flanger Depth",    "",      0.0f,  1.0f,  0.7f, Curve::Linear },
    { "flanger_feedback", "Flanger Feedback", "",     -0.9f,  0.9f,  0.6f, Curve::Linear },
    { "flanger_mix",      "Flanger Mix",      "",      0.0f,  1.0f,  0.35f, Curve::Linear },
    { "phaser_beats",     "Phaser Period",    "beats", 0.5f, 32.0f, 16.0f, Curve::Log },
    { "phaser_depth",     "Phaser Depth",     "",      0.0f,  1.0f,  0.8f, Curve::Linear },
    { "phaser_feedback",  "Phaser Feedback",  "",      0.0f,  0.9f,  0.5f, Curve::Linear },
    { "phaser_mix",       "Phaser Mix",       "",      0.0f,  1.0f,  0.3f, Curve::Linear },
    { "shift_hz",         "Frequency Shift",  "Hz", -500.0f, 500.0f, 0.0f, Curve::Linear },
    { "shift_mix",        "Shifter Mix",      "",      0.0f,  1.0f,  0.5f, Curve::Linear },
    { "return",           "Send Return",      "dB",  -36.0f,  6.0f, -3.0f, Curve::Linear },
    { "motion",           "Motion",           "",      0.0f,  1.0f,  0.7f, Curve::Linear },
};

const ParamDesc kFxParams[fx::Count] = {
    { "room_size",      "Room Size",      "",      0.3f,    3.0f,   0.5f, Curve::Linear },
    { "room_decay",     "Room Decay",     "s",     0.1f,    4.0f,   0.7f, Curve::Log },
    { "room_damping",   "Room Damping",   "",      0.0f,    1.0f,   0.5f, Curve::Linear },
    { "hall_size",      "Hall Size",      "",      0.3f,    3.0f,   1.6f, Curve::Linear },
    { "hall_decay",     "Hall Decay",     "s",     0.3f,   20.0f,   4.5f, Curve::Log },
    { "hall_damping",   "Hall Damping",   "",      0.0f,    1.0f,  0.45f, Curve::Linear },
    { "hall_pre_delay", "Hall Pre-Delay", "beats", 0.0f,    0.5f,  0.25f, Curve::Linear },
    { "low_cut",        "Return Low Cut", "Hz",  150.0f, 1000.0f, 300.0f, Curve::Log },
    { "high_cut",       "Return High Cut","Hz",  1000.0f,20000.0f,9000.0f, Curve::Log },
    { "room_return",    "Room Return",    "dB",  -36.0f,    6.0f,  -6.0f, Curve::Linear },
    { "hall_return",    "Hall Return",    "dB",  -36.0f,    6.0f,  -6.0f, Curve::Linear },
    { "return_duck",    "Return Duck",    "",      0.0f,    1.0f,   0.5f, Curve::Linear },
};

const ParamDesc kMixParams[mix::Count] = {
    { "kick_mute", "Kick Mute", "", 0.0f, 1.0f, 0.0f, Curve::Toggle },
    { "bass_mute", "Bass Mute", "", 0.0f, 1.0f, 0.0f, Curve::Toggle },
    { "track_gain", "Track Gain", "dB", -12.0f, 12.0f, 0.0f, Curve::Linear },
    { "perc_mute",  "Perc Mute",  "",     0.0f,  1.0f, 0.0f, Curve::Toggle },
    // +3 dB instead of +1 (16.09.2026, Phase 9): with the band-limited hats the kit no longer spends
    // its level above 12 kHz, and the presence and air bands land on the reference median.
    { "perc_level", "Perc Level", "dB", -24.0f, 12.0f, 2.0f, Curve::Linear },
    { "acid_mute",  "Acid Mute",  "",     0.0f,  1.0f, 0.0f, Curve::Toggle },
    { "acid_level", "Acid Level", "dB", -24.0f, 12.0f, 0.0f, Curve::Linear },
    // 19.09.2026, round "voices": the strips in the order of the voices' groups (Params.h, mix::).
    { "lead_mute",  "Lead Mute",  "",     0.0f,  1.0f, 0.0f, Curve::Toggle },
    // -3 dB (19.09.2026, round "voices"): excerpt B of the listening seed (track 2's drop) sat +9.8 dB over
    // the reference median in presence once its foundation was no longer over-matched (Composer.cpp,
    // probeLoudness); with the lead muted it read +1.8, so the lead carried most of it. -3 dB: +7.6.
    // -6 dB (19.09.2026, round "arrangement"): the user after listening to the voices round, "insgesamt ist
    // der Lead und auch der Arp zu laut"; the arp -2 -> -5 with it, the counter and the stab 3 dB down as
    // well so that they stay under the lead. 4 dB each took the presence band of testMixBalance 2.2 dB
    // under the reference median; docs/PLAN.md has the band balance before and after.
    { "lead_level", "Lead Level", "dB", -24.0f, 12.0f, -6.0f, Curve::Linear },
    { "counter_mute",  "Counter Mute",  "",     0.0f,  1.0f, 0.0f, Curve::Toggle },
    { "counter_level", "Counter Level", "dB", -24.0f, 12.0f, -3.0f, Curve::Linear },
    { "arp_mute",   "Arp Mute",   "",     0.0f,  1.0f, 0.0f, Curve::Toggle },
    // -2 dB (18.09.2026): the arp was the loudest melodic part and owned 300 Hz .. 2 kHz, where the
    // congas, toms and the clap have to be heard (kDefaultKit).
    { "arp_level",  "Arp Level",  "dB", -24.0f, 12.0f, -5.0f, Curve::Linear },
    { "stab_mute",  "Stab Mute",  "",     0.0f,  1.0f, 0.0f, Curve::Toggle },
    { "stab_level", "Stab Level", "dB", -24.0f, 12.0f, -3.0f, Curve::Linear },
    { "pad_mute",   "Pad Mute",   "",     0.0f,  1.0f, 0.0f, Curve::Toggle },
    { "pad_level",  "Pad Level",  "dB", -24.0f, 12.0f, 0.0f, Curve::Linear },
    { "drone_mute", "Drone Mute", "",     0.0f,  1.0f, 0.0f, Curve::Toggle },
    { "drone_level","Drone Level","dB", -24.0f, 12.0f, 0.0f, Curve::Linear },
    { "sfx_mute",   "SFX Mute",   "",     0.0f,  1.0f, 0.0f, Curve::Toggle },
    { "sfx_level",  "SFX Level",  "dB", -24.0f, 12.0f, 0.0f, Curve::Linear },
    { "perc_room",  "Perc Room",  "",     0.0f,  1.0f, 0.12f, Curve::Linear },
    { "perc_hall",  "Perc Hall",  "",     0.0f,  1.0f, 0.0f, Curve::Linear },
    { "duck_attack","Duck Attack","ms",   0.5f, 30.0f, 2.0f, Curve::Log },
    { "duck_hold",  "Duck Hold",  "ms",   0.0f, 200.0f, 20.0f, Curve::Linear },
    { "duck_release","Duck Release","ms", 10.0f, 800.0f, 180.0f, Curve::Log },
    // 19.09.2026, round "fx-psychedelia": the shamanic bed and the voices get their own strips.
    { "texture_mute",  "Texture Mute",  "",     0.0f,  1.0f, 0.0f, Curve::Toggle },
    { "texture_level", "Texture Level", "dB", -24.0f, 12.0f, 0.0f, Curve::Linear },
    { "vocal_mute",    "Vocal Mute",    "",     0.0f,  1.0f, 0.0f, Curve::Toggle },
    { "vocal_level",   "Vocal Level",   "dB", -24.0f, 12.0f, 0.0f, Curve::Linear },
};

const ParamDesc kMasterParams[master::Count] = {
    { "gain",    "Gain",    "dB", -24.0f, 24.0f,  0.0f, Curve::Linear },
    { "ceiling", "Ceiling", "dBTP", -12.0f,  0.0f, -1.0f, Curve::Linear },
    { "clip",    "Clip",    "",     0.0f,  1.0f,  1.0f, Curve::Toggle },
    { "comp_threshold", "Comp Threshold", "dB", -40.0f, 0.0f, -8.0f, Curve::Linear },
    { "comp_ratio",     "Comp Ratio",     ":1",   1.0f, 10.0f,  1.5f, Curve::Log },
    { "comp_knee",      "Comp Knee",      "dB",   0.0f, 24.0f,  6.0f, Curve::Linear },
    { "comp_attack",    "Comp Attack",    "ms",   0.1f, 100.0f, 30.0f, Curve::Log },
    { "comp_release",   "Comp Release",   "ms",   10.0f, 2000.0f, 200.0f, Curve::Log },
    { "mono_bass",      "Mono Bass",      "Hz",   40.0f, 300.0f, 120.0f, Curve::Log },
    { "limiter",        "Limiter",        "",     0.0f,  1.0f,  1.0f, Curve::Toggle },
    { "limiter_release","Limiter Release","ms",   2.0f, 1000.0f, 20.0f, Curve::Log },
    { "target_lufs",    "Target Loudness","LUFS", -20.0f, -4.0f, -9.0f, Curve::Linear },
    { "auto_gain",      "Auto Gain",      "",     0.0f,  1.0f,  1.0f, Curve::Toggle },
    { "clipper",        "Clipper",        "",     0.0f,  1.0f,  1.0f, Curve::Toggle },
    { "clipper_threshold","Clipper Threshold","dB", -6.0f, 6.0f, 0.0f, Curve::Linear },
};

// The cue bridge of PLAN 8.3. Appended after the master block, so no parameter that existed before
// changed its id, its default or its place in the host's list.
const ParamDesc kCueParams[cue::Count] = {
    { "send",    "Send Cues", "",      0.0f,     1.0f,    0.0f, Curve::Toggle },
    { "port",    "Port",      "",   1024.0f, 65535.0f, 9000.0f, Curve::Int },
    { "beats",   "Beat Cues", "",      0.0f,     1.0f,    1.0f, Curve::Toggle },
    // A trim on the lead the sender already computes from the block size and the limiter's lookahead
    // (Cue.h). Positive moves the cue later. It exists because the one part of the chain the plugin
    // cannot ask about is how deep the driver's own buffering is; zero is right for the common case
    // of a single buffer in flight.
    { "lead_ms", "Lead Trim", "ms",  -50.0f,    50.0f,    0.0f, Curve::Linear },
};

/** @brief One module: its prefix, table, how many instances exist, and optionally their names. */
struct ModuleSpec {
    const char* prefix;
    const ParamDesc* descs;
    int count;
    int instances;
    const char* const* instanceNames = nullptr;   ///< prefixes of the instances instead of prefix + number
};

const ModuleSpec kModules[static_cast<int>(Module::Count)] = {
    { "compose", kComposeParams, compose::Count, 1 },
    { "kick",    kKickParams,    kick::Count,    1 },
    { "bass",    kBassParams,    bass::Count,    1 },
    { "perc",    kPercParams,    perc::Count,    kPercLanes },
    { "acid",    kAcidParams,    acid::Count,    1 },
    { "poly",    kPolyParams,    poly::Count,    kPolyInstances, kPolyInstanceNames },
    { "sfx",     kSfxParams,     sfx::Count,     1 },
    { "fx",      kFxParams,      fx::Count,      1 },
    { "mix",     kMixParams,     mix::Count,     1 },
    { "master",  kMasterParams,  master::Count,  1 },
    { "cue",     kCueParams,     cue::Count,     1 },
    { "texture", kTextureParams, texture::Count, 1 },
    { "vocal",   kVocalParams,   vocal::Count,   1 },
    { "psyfx",   kPsyFxParams,   psyfx::Count,   1 },
};

bool isDiscrete(Curve c) { return c == Curve::Int || c == Curve::Choice || c == Curve::Toggle; }

std::string_view trim(std::string_view s)
{
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

} // namespace

ParamStore::ParamStore()
{
    for (auto& row : bases_) for (int& b : row) b = -1;
    int total = 0;
    for (const ModuleSpec& m : kModules) total += m.count * m.instances;
    entries_.reserve(static_cast<size_t>(total));
    for (int mi = 0; mi < static_cast<int>(Module::Count); ++mi) {
        const ModuleSpec& m = kModules[mi];
        for (int inst = 0; inst < m.instances && inst < kMaxInstances; ++inst) {
            bases_[mi][inst] = static_cast<int>(entries_.size());
            std::string prefix = m.instanceNames != nullptr ? std::string(m.instanceNames[inst]) : std::string(m.prefix);
            if (m.instances > 1 && m.instanceNames == nullptr) prefix += std::to_string(inst + 1);
            for (int p = 0; p < m.count; ++p) {
                Entry e{ &m.descs[p], prefix + "." + m.descs[p].key, static_cast<Module>(mi), inst };
                index_.emplace(e.key, static_cast<int>(entries_.size()));
                entries_.push_back(std::move(e));
            }
        }
    }
    values_ = std::make_unique<std::atomic<float>[]>(entries_.size());
    // Defaults: the descriptors', then the instance-specific ones of the default kit on top.
    defaults_.resize(entries_.size());
    for (int i = 0; i < count(); ++i) {
        defaults_[static_cast<size_t>(i)] = desc(i).defValue;
        values_[static_cast<size_t>(i)].store(desc(i).defValue, std::memory_order_relaxed);
    }
    parseText(kDefaultKit);
    parseText(kDefaultPoly);
    for (int i = 0; i < count(); ++i) defaults_[static_cast<size_t>(i)] = get(i);
}

int ParamStore::base(Module m, int instance) const
{
    const int mi = static_cast<int>(m);
    if (mi < 0 || mi >= static_cast<int>(Module::Count) || instance < 0 || instance >= kMaxInstances) return -1;
    return bases_[mi][instance];
}

int ParamStore::find(std::string_view key) const
{
    const auto it = index_.find(std::string(key));
    return it == index_.end() ? -1 : it->second;
}

int ParamStore::getInt(int id) const
{
    return static_cast<int>(std::lround(get(id)));
}

void ParamStore::set(int id, float value)
{
    if (id < 0 || id >= count()) return;
    const ParamDesc& d = desc(id);
    if (!(value == value)) value = defaults_.empty() ? d.defValue : defaults_[static_cast<size_t>(id)];   // NaN
    float v = value < d.minValue ? d.minValue : (value > d.maxValue ? d.maxValue : value);
    if (isDiscrete(d.curve)) v = std::round(v);
    values_[static_cast<size_t>(id)].store(v, std::memory_order_relaxed);
}

float ParamStore::toNormalised(int id, float value) const
{
    const ParamDesc& d = desc(id);
    if (d.maxValue <= d.minValue) return 0.0f;
    float n;
    if (d.curve == Curve::Log) n = std::log(value / d.minValue) / std::log(d.maxValue / d.minValue);
    else n = (value - d.minValue) / (d.maxValue - d.minValue);
    return n < 0.0f ? 0.0f : (n > 1.0f ? 1.0f : n);
}

float ParamStore::fromNormalised(int id, float norm) const
{
    const ParamDesc& d = desc(id);
    const float n = norm < 0.0f ? 0.0f : (norm > 1.0f ? 1.0f : norm);
    float v;
    if (d.curve == Curve::Log) v = d.minValue * std::pow(d.maxValue / d.minValue, n);
    else v = d.minValue + n * (d.maxValue - d.minValue);
    if (isDiscrete(d.curve)) v = std::round(v);
    return v;
}

void ParamStore::resetDefaults()
{
    for (int i = 0; i < count(); ++i) values_[static_cast<size_t>(i)].store(defaults_[static_cast<size_t>(i)], std::memory_order_relaxed);
}

int ParamStore::moduleCount(Module m)
{
    const int mi = static_cast<int>(m);
    return mi >= 0 && mi < static_cast<int>(Module::Count) ? kModules[mi].count : 0;
}

void ParamStore::readModule(Module m, int instance, float* out) const
{
    const int b = base(m, instance);
    if (b < 0) return;
    const int n = moduleCount(m);
    for (int i = 0; i < n; ++i) out[i] = get(b + i);
}

void ParamStore::copyValuesFrom(const ParamStore& other)
{
    const int n = count() < other.count() ? count() : other.count();
    for (int i = 0; i < n; ++i) values_[static_cast<size_t>(i)].store(other.get(i), std::memory_order_relaxed);
}

namespace {
/** @brief Lower case without spaces, for matching choice names ("Double Harmonic" = "doubleharmonic"). */
std::string foldName(std::string_view s)
{
    std::string out;
    for (char ch : s) {
        if (ch == ' ' || ch == '_' || ch == '-') continue;
        out += (ch >= 'A' && ch <= 'Z') ? static_cast<char>(ch - 'A' + 'a') : ch;
    }
    return out;
}
} // namespace

bool ParamStore::parseText(std::string_view text, std::string* error)
{
    // Split into assignments. Newlines and ';' always separate; whitespace separates only where the
    // next word contains '=' -- so a choice name with a space ("compose.scale=Double Harmonic") stays
    // one value while "a=1 b=2" is still two assignments. '#' starts a comment to the end of the line.
    std::vector<std::string> items;
    size_t pos = 0;
    bool newItem = true;
    while (pos < text.size()) {
        const char ch = text[pos];
        if (ch == '\n' || ch == ';' || ch == '\r') { newItem = true; ++pos; continue; }
        if (ch == ' ' || ch == '\t') { ++pos; continue; }
        if (ch == '#') { while (pos < text.size() && text[pos] != '\n') ++pos; continue; }
        size_t end = pos;
        while (end < text.size() && text[end] != '\n' && text[end] != ';' && text[end] != '\r' && text[end] != ' ' && text[end] != '\t') ++end;
        const std::string_view word = text.substr(pos, end - pos);
        pos = end;
        if (newItem || word.find('=') != std::string_view::npos || items.empty()) items.emplace_back(word);
        else { items.back() += ' '; items.back() += word; }
        newItem = false;
    }

    bool ok = true;
    for (const std::string& item : items) {
        const std::string_view tok = trim(item);
        const size_t eq = tok.find('=');
        if (eq == std::string_view::npos) {
            if (error && ok) *error = "missing '=' in \"" + std::string(tok) + "\"";
            ok = false;
            continue;
        }
        const std::string_view k = trim(tok.substr(0, eq)), v = trim(tok.substr(eq + 1));
        const int id = find(k);
        if (id < 0) {
            if (error && ok) *error = "unknown parameter \"" + std::string(k) + "\"";
            ok = false;
            continue;
        }
        const ParamDesc& d = desc(id);
        bool matched = false;
        if ((d.curve == Curve::Choice && d.choices != nullptr) || d.curve == Curve::Toggle) {
            const std::string fv = foldName(v);
            if (d.curve == Curve::Toggle && (fv == "on" || fv == "off")) { set(id, fv == "on" ? 1.0f : 0.0f); matched = true; }
            for (int c = 0; !matched && d.choices != nullptr && c <= static_cast<int>(d.maxValue); ++c) {
                if (fv == foldName(d.choices[c])) { set(id, static_cast<float>(c)); matched = true; }
            }
        }
        if (!matched) {
            const std::string vs(v);
            char* stop = nullptr;
            const double x = std::strtod(vs.c_str(), &stop);
            if (vs.empty() || stop == nullptr || *stop != 0) {
                if (error && ok) *error = "bad value \"" + vs + "\" for " + std::string(k);
                ok = false;
                continue;
            }
            set(id, static_cast<float>(x));
        }
    }
    return ok;
}

std::string ParamStore::toText(bool onlyChanged) const
{
    std::string out;
    char buf[64];
    for (int i = 0; i < count(); ++i) {
        const float v = get(i);
        if (onlyChanged && v == defaults_[static_cast<size_t>(i)]) continue;
        // %.9g round-trips every float exactly.
        std::snprintf(buf, sizeof(buf), "%.9g", static_cast<double>(v));
        out += key(i);
        out += '=';
        out += buf;
        out += '\n';
    }
    return out;
}

std::string ParamStore::format(int id) const
{
    const ParamDesc& d = desc(id);
    const float v = get(id);
    if (d.curve == Curve::Choice && d.choices != nullptr) return d.choices[getInt(id)];
    if (d.curve == Curve::Toggle) return v >= 0.5f ? "On" : "Off";
    char buf[64];
    if (d.curve == Curve::Int) std::snprintf(buf, sizeof(buf), "%d", getInt(id));
    else if (std::fabs(v) >= 100.0f) std::snprintf(buf, sizeof(buf), "%.0f", static_cast<double>(v));
    else if (std::fabs(v) >= 10.0f) std::snprintf(buf, sizeof(buf), "%.1f", static_cast<double>(v));
    else std::snprintf(buf, sizeof(buf), "%.2f", static_cast<double>(v));
    std::string s = buf;
    if (d.unit != nullptr && d.unit[0] != 0) { s += ' '; s += d.unit; }
    return s;
}

} // namespace phos
