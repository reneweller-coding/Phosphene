/**
 * @file Params.cpp
 * @brief Module descriptor tables and the parameter store.
 * @author Rene Weller
 */
#include "phos/Params.h"
#include "phos/Util.h"
#include "phos/Disperser.h"       // DisperseStages: the upper end of the disperse parameters
#include "phos/WaveTableFile.h"   // NumWaveTables and the names of the shipped library tables
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace phos
{

const char *const KeyNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
const char *const ScaleNames[] = { "Aeolian",           "Phrygian",        "Harmonic Minor",
                                   "Phrygian Dominant", "Double Harmonic", "Dorian" };
const char *const KickPatternNames[] = { "Four", "Four + Fills", "Off" };
// The names of the style profiles and the dramaturgy presets (Form.h). They live here, with the other
// choice tables, because the parameter registry is built from them.
const char *const StyleNames[] = { "Goa", "Full-On", "Progressive", "Dark Forest", "Hi-Tech" };
const char *const ArcNames[] = { "Warm-up", "Peak-Time", "Morning", "Closing", "Flat" };
const char *const BassPatternNames[] = { "Rolling", "Gallop", "Skip", "Offbeat", "Triplet" };
// Phase 8: the predictive model behind the melodic lines. "Markov" is the measured order-2 model of
// the corpus (Corpus.h), "Neural" the trained transformer (Model.h). Markov stays the default until
// the trained model has been measured to be better -- and Neural falls back to it when no weight
// file is there.
const char *const MelodyModelNames[] = { "Markov", "Neural" };
// Phase 8, bass round: where a bass note's pitch comes from. "Pattern" is the root plus the phrase
// figures of Composer.cpp, which is what the program has always played; "Neural" is the learned
// fourth role of Model.h, drawn under the same register and scale constraints. Pattern stays the
// default until a listening comparison exists -- the held-out measurement says the learned bass
// predicts real basslines far better, which is not the same as saying it sounds better -- and
// Neural falls back to Pattern when no weight file is there.
const char *const BassModelNames[] = { "Pattern", "Neural" };
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
const char *const BassRhythmNames[] = { "Pattern", "Corpus" };
// 22.09.2026, round "Lead": "Auto (Style)" hands the choice to the style profile's lead vector (Form.h).
const char *const LeadDensityNames[] = { "Auto (Style)", "Sparse", "Medium", "Dense" };
const char *const PitchEntropyNames[] = { "Auto (Style)", "Low", "Medium", "High" };
const char *const CounterModeNames[] = { "Auto (Style)", "Echo", "Answer", "Timbral", "Hocket" };
const char *const PercRoleNames[NumPercRoles] = { "Closed Hat", "Open Hat", "Ride", "Crash", "Clap", "Snare",
                                                  "Rim",        "Shaker",   "Tom",  "Conga", "Zap",  "Blip" };
// The prefixes of the polyphonic instances, in the order of PolyInstance (Params.h): the voices' groups.
const char *const PolyInstanceNames[PolyInstances] = { "lead", "counter", "arp", "stab", "pad", "drone" };

namespace
{

const char *const KickEngineNames[] = { "Sweep", "Resonant" };   ///< kick.engine
const char *const KickTuneNames[] = { "Free", "Key" };           ///< kick.tune
const char *const KickClipNames[] = { "Tanh", "Hard" };          ///< kick.clip
const char *const SubModeNames[] = { "Mixed",
                                     "Split" };   ///< bass.sub_mode: the sub mixed into the bass or split from it
const char *const KickLockNames[] = { "Off", "Bass follows kick",
                                      "Kick follows bass" };   ///< bass.kick_lock (KickLock)

/** @brief The compose module: what the composer is asked for. */
const ParamDesc ComposeParams[compose::COUNT] = {
    { "bpm", "Tempo", "BPM", 100.0f, 190.0f, 145.0f, CurveE::LINEAR },
    { "key", "Start Key", "", 0.0f, 11.0f, 6.0f, CurveE::CHOICE, KeyNames },
    { "scale", "Start Scale", "", 0.0f, 5.0f, 1.0f, CurveE::CHOICE, ScaleNames },
    { "kick_pattern", "Kick Pattern", "", 0.0f, 2.0f, 1.0f, CurveE::CHOICE, KickPatternNames },
    { "bass_pattern", "Bass Pattern", "", 0.0f, 4.0f, 0.0f, CurveE::CHOICE, BassPatternNames },
    { "bass_gate", "Bass Gate", "", 0.2f, 1.0f, 0.7f, CurveE::LINEAR },
    { "bass_variation", "Bass Variation", "", 0.0f, 1.0f, 0.4f, CurveE::LINEAR },
    // The bass's octave window in semitones (Harmony.h, bassRootNote): it moves where the root sits, never which note it is.
    { "bass_register", "Bass Register", "st", -12.0f, 12.0f, 0.0f, CurveE::INT },
    { "track_bars", "Track Length", "bars", 32.0f, 512.0f, 256.0f, CurveE::INT },
    { "track_variation", "Track Variation", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "sound_variation", "Sound Variation", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    // 19.09.2026, round "arrangement": 1 instead of 3, so that the default set (Full-On, 145 BPM) wanders
    // inside the user's Full-On window of 142 .. 146 BPM (144 .. 146) rather than up to 148.
    { "tempo_range", "Tempo Range", "BPM", 0.0f, 10.0f, 1.0f, CurveE::LINEAR },
    { "level_match", "Level Match", "", 0.0f, 1.0f, 1.0f, CurveE::TOGGLE },
    { "perc_density", "Perc Density", "", 0.0f, 1.0f, 0.6f, CurveE::LINEAR },
    { "perc_variation", "Perc Variation", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "swing", "Swing", "", 0.0f, 0.3f, 0.0f, CurveE::LINEAR },
    { "acid_amount", "Acid Amount", "", 0.0f, 1.0f, 0.6f, CurveE::LINEAR },
    { "lead_amount", "Lead Amount", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "arp_amount", "Arp Amount", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "melody_variation", "Melody Variation", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "melody_temperature", "Melody Temperature", "", 0.5f, 2.0f, 1.0f, CurveE::LOG },
    { "squelch_chance", "Squelch Chance", "", 0.0f, 1.0f, 0.3f, CurveE::LINEAR },
    { "bass_follows_chords", "Bass Follows Chords", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "pad_amount", "Pad Amount", "", 0.0f, 1.0f, 0.7f, CurveE::LINEAR },
    { "sfx_amount", "SFX Amount", "", 0.0f, 1.0f, 0.7f, CurveE::LINEAR },
    { "gate_chance", "Gate Chance", "", 0.0f, 1.0f, 0.35f, CurveE::LINEAR },
    // Phase 5: the style profile, the dramaturgy of the set and the time base of its energy arc.
    { "style", "Start Style", "", 0.0f, 4.0f, 1.0f, CurveE::CHOICE, StyleNames },
    { "arc", "Energy Arc", "", 0.0f, 4.0f, 4.0f, CurveE::CHOICE, ArcNames },
    { "style_tempo", "Style Tempo", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "set_minutes", "Set Length", "min", 10.0f, 300.0f, 60.0f, CurveE::INT },
    // Phase 8: stage A or stage B behind the melodic lines.
    { "melody_model", "Melody Model", "", 0.0f, 1.0f, 0.0f, CurveE::CHOICE, MelodyModelNames },
    // Phase 8, bass round: the pattern families or the learned fourth role behind the bass pitches.
    { "bass_model", "Bass Model", "", 0.0f, 1.0f, 0.0f, CurveE::CHOICE, BassModelNames },
    // 16.09.2026: modal interchange. A section may borrow another mode over the track's tonic pedal
    // -- Dorian in the groove, Phrygian in the drive, Phrygian dominant at the peak (Form.h). On by
    // default because that is what the genre does; the bass does not move with it either way.
    { "modal_interchange", "Modal Interchange", "", 0.0f, 1.0f, 1.0f, CurveE::TOGGLE },
    // 16.09.2026, bass rhythm round: the pattern families or the corpus onset model behind the bass
    // *rhythm*. Appended, so no parameter above it moves; Pattern is the default.
    { "bass_rhythm", "Bass Rhythm", "", 0.0f, 1.0f, 0.0f, CurveE::CHOICE, BassRhythmNames },
    // 19.09.2026, round "voices": the three new voices' share of the tracks (like lead_amount), and two
    // density knobs of the psychedelic layer the round of the same day could not add (it did not own the
    // composer): 1 is the density that round calibrated, 0 removes the voices or the bed, 2 doubles them.
    // 1.0 since 19.09.2026 (round "arrangement"): the user's drop 2 has "lead 1 and lead 2 in call and
    // response", so every track that has a lead has its counter; it plays in drop 2 only (Form.cpp). At 0.6
    // four tracks in seventeen of the listening seed had both.
    { "counter_amount", "Counter Amount", "", 0.0f, 1.0f, 1.0f, CurveE::LINEAR },
    { "stab_amount", "Stab Amount", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "drone_amount", "Drone Amount", "", 0.0f, 1.0f, 0.6f, CurveE::LINEAR },
    { "voice_density", "Voice Density", "x", 0.0f, 2.0f, 1.0f, CurveE::LINEAR },
    { "bed_density", "Bed Density", "x", 0.0f, 2.0f, 1.0f, CurveE::LINEAR },
    // The rows below stand in the order of the compose enum (Params.h), which is what indexes them. On
    // 22.09.2026 style_mix was appended *behind* presence_match while the enum put StyleMix before it, so
    // compose::PresenceMatch read the style_mix row (two toggles at 1: unnoticed) and, when two choice rows
    // followed on 23.09.2026, the lead_density row -- default 0, presence match off for every track
    // (testPresence: 8 of 8 tracks outside the band left alone). testParams now checks key against index.
    // 22.09.2026: one style for a whole night was the user's first complaint about the sets ("langweilig,
    // wenn immer alles im selben Stil ist"). On, the style walks from track to track (Composer.cpp).
    { "style_mix", "Style Mix", "", 0.0f, 1.0f, 1.0f, CurveE::TOGGLE },
    // 19.09.2026, round "polish" (ComposerLevels.cpp, matchPresence): On brings each track's presence band into
    // a band around the reference recordings' median by the level of its lines; Off plays them as matched.
    { "presence_match", "Presence Match", "", 0.0f, 1.0f, 1.0f, CurveE::TOGGLE },
    // 22.09.2026, round "Lead" (the user: lead_density and pitch_entropy as knobs, "Auto" = the style):
    // how many onsets a lead bar carries, and how far the pitch draws may stray from the model's
    // and the rules' favourites (MelodyLead.cpp, makeLead).
    { "lead_density", "Lead Density", "", 0.0f, 3.0f, 0.0f, CurveE::CHOICE, LeadDensityNames },
    { "pitch_entropy", "Pitch Entropy", "", 0.0f, 3.0f, 0.0f, CurveE::CHOICE, PitchEntropyNames },
    // 23.09.2026, round "Counter": the counter-lead's mode (Form.h, CounterMode; MelodyLead.cpp, makeCounter). At the end,
    // in the enum's order (see the note above the style_mix row).
    { "counter_mode", "Counter Mode", "", 0.0f, 4.0f, 0.0f, CurveE::CHOICE, CounterModeNames },
    // 23.09.2026, round "Hoerbarkeit" (ComposerLevels.cpp, matchAudibility): at the end, in the enum's order.
    { "audibility_match", "Audibility Match", "", 0.0f, 1.0f, 1.0f, CurveE::TOGGLE },
    { "sound_presets", "Sound Presets", "", 0.0f, 1.0f, 1.0f, CurveE::TOGGLE },
    // 27.09.2026 (FormSfx.cpp, placeField): at the end, in the enum's order.
    { "field_density", "Field Density", "x", 0.0f, 2.0f, 1.0f, CurveE::LINEAR },
};

const char *const PercEngineNames[] = { "Noise", "Metal", "Modal", "Tone", "FM" };   ///< perc.engine (PercEngine)
const char *const ModeSetNames[] = { "Membrane", "Bar", "Harmonic" };   ///< perc.modes: the modal engine's mode sets
const char *const PercFilterNames[] = { "Low Pass", "Band Pass",
                                        "High Pass" };   ///< perc.filter: the lane filter's output

/** @brief A lane of the kit. */
const ParamDesc PercParams[perc::COUNT] = {
    { "active", "Active", "", 0.0f, 1.0f, 1.0f, CurveE::TOGGLE },
    { "role", "Role", "", 0.0f, 11.0f, 0.0f, CurveE::CHOICE, PercRoleNames },
    { "engine", "Engine", "", 0.0f, 4.0f, 0.0f, CurveE::CHOICE, PercEngineNames },
    { "pitch", "Pitch", "Hz", 40.0f, 12000.0f, 400.0f, CurveE::LOG },
    { "pitch_amount", "Pitch Amount", "x", 1.0f, 16.0f, 1.0f, CurveE::LOG },
    { "pitch_decay", "Pitch Decay", "ms", 0.5f, 300.0f, 10.0f, CurveE::LOG },
    { "fm_ratio", "FM Ratio", "", 0.25f, 8.0f, 1.41f, CurveE::LINEAR },
    { "fm_index", "FM Index", "", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "mode_set", "Modes", "", 0.0f, 2.0f, 0.0f, CurveE::CHOICE, ModeSetNames },
    { "mode_damp", "Mode Damping", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "metal_scale", "Metal Scale", "x", 0.25f, 4.0f, 1.0f, CurveE::LOG },
    { "noise", "Noise", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "noise_decay", "Noise Decay", "ms", 2.0f, 3000.0f, 60.0f, CurveE::LOG },
    { "bursts", "Bursts", "", 1.0f, 6.0f, 1.0f, CurveE::INT },
    { "burst_spacing", "Burst Spacing", "ms", 2.0f, 40.0f, 10.0f, CurveE::LINEAR },
    { "decay", "Decay", "ms", 2.0f, 3000.0f, 120.0f, CurveE::LOG },
    { "filter", "Filter", "", 0.0f, 2.0f, 2.0f, CurveE::CHOICE, PercFilterNames },
    { "cutoff", "Cutoff", "Hz", 100.0f, 18000.0f, 8000.0f, CurveE::LOG },
    { "resonance", "Resonance", "", 0.0f, 1.0f, 0.2f, CurveE::LINEAR },
    // Up to 8 kHz since 25.09.2026: a closed hat's low cut belongs at 5 kHz (the kit above), and the range stopped at 4.
    { "low_cut", "Low Cut", "Hz", 150.0f, 8000.0f, 150.0f, CurveE::LOG },
    { "drive", "Drive", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "level", "Level", "dB", -36.0f, 6.0f, -12.0f, CurveE::LINEAR },
    { "pan", "Pan", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "choke", "Choke Group", "", 0.0f, 4.0f, 0.0f, CurveE::INT },
    { "shift", "Shift", "ms", -10.0f, 10.0f, 0.0f, CurveE::LINEAR },
    { "density", "Density", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "tune", "Tune to Key", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    // 16.09.2026, arrangement dynamics. Appended in the order of the perc:: enum, nothing reordered:
    // the indices above sit in saved presets and in the plugin's state.
    // pan_depth defaults to 0 in the table and is set per lane in DefaultKit below, so that a lane
    // a user builds from scratch stands still until it is told to move. The period is three
    // sixteenths of a bar, and the reason is arithmetic rather than taste (Perc.h): a lane on the
    // sixteenth grid then samples its own swing at three phases 120 degrees apart, which carry the
    // first and the second moment of the sinusoid exactly, so the kit's measured width does not move.
    { "pan_depth", "Pan Depth", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "pan_bars", "Pan Period", "bars", 0.0625f, 16.0f, 0.1875f, CurveE::LOG },
    { "cut_track", "Cut Tracks Pitch", "", 0.0f, 2.0f, 0.0f, CurveE::LINEAR },
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
 * before and after are in docs/rounds/2026-09.md.
 */
// 25.09.2026, the user after listening: "die Percussions, insbesondere die Hi Hats, VIEL zu laut und sie erzeugen
// auch noch Feedback". Measured lane by lane (seed 42, Full-On): the ride alone carried about 70 % of the kit's
// energy, and the metal engine's square partials rang 12 .. 28 dB over their neighbourhood at 1.7 .. 2.4 kHz -- under
// the cymbals' low cuts of 3 .. 3.5 kHz, which a gentle slope does not reach: a standing tone, heard as feedback.
// Noise carries the cymbals now: a metal lane's noise trades against its squares (Perc.cpp, MetalNoiseTrade), and
// at noise 0.9 .. 1 the partials stand 1 .. 6 dB over their neighbourhood instead of 12 .. 28, the resonances are
// low, and the levels came down with the brighter noise -- to where the air band sits near the recordings' median
// (testMixBalance; lowering the hats without the noise had taken it under their lower quartile). The hats' low cuts
// stay at 3 kHz: moved up to 4.5 .. 5 kHz they took the presence band with them, and the listening seed's darkest
// track fell 2.7 dB under the recordings' median (testPresence) with its lines already at the match's +3 dB cap.
// 26.09.2026, the user: "Jetzt höre ich praktisch gar keine Hi Hats mehr". The ride's noise, 700 ms long, had taken 41 %
// of the kit's air band (26 % before) and hissed over the short hats: the ride is 4 dB lower with a 220 ms noise tail,
// the closed hat +2 dB, the open hat +2 dB. Two consequences, each met where it arises: the clap, the kit's presence
// voice, is 1.5 dB up (with the ride's presence gone the listening seed's darkest track sat 2.57 dB under the
// recordings' median, testPresence, 2.5), and the shaker 2 dB -- the auto-pan swings the hats against shaker, ride
// and rim (RolePanGroup, Perc.cpp), and with the ride down that side had lost the weight to hold them once the
// shaker was corrected down (testArrangeDynamics: 0.78 dB of reduction left, 1 wanted; 1.07 now).
const char *const DefaultKit =
    "perc1.role=Closed Hat;perc1.engine=Metal;perc1.decay=45;perc1.noise=1;perc1.noise_decay=35;perc1.filter=Low Pass;"
    "perc1.cutoff=12000;perc1.low_cut=3000;perc1.resonance=0.05;perc1.level=1.5;perc1.pan=0.45;perc1.pan_depth=1;perc1.choke=1;perc1.density=0.6\n"
    "perc2.role=Open Hat;perc2.engine=Metal;perc2.decay=260;perc2.noise=0.9;perc2.noise_decay=220;perc2.filter=Low Pass;"
    "perc2.cutoff=12000;perc2.low_cut=3000;perc2.level=-1.5;perc2.pan=-0.4;perc2.pan_depth=1;perc2.choke=1\n"
    "perc3.role=Ride;perc3.engine=Metal;perc3.metal_scale=0.72;perc3.decay=700;perc3.noise=1;perc3.noise_decay=220;"
    "perc3.filter=Band Pass;perc3.cutoff=5200;perc3.resonance=0.1;perc3.low_cut=3500;perc3.level=-14.5;perc3.pan=0.6;perc3.pan_depth=1\n"
    "perc4.role=Crash;perc4.engine=Metal;perc4.metal_scale=0.5;perc4.decay=1600;perc4.noise=0.9;perc4.noise_decay=1400;"
    "perc4.filter=High Pass;perc4.cutoff=3000;perc4.low_cut=4000;perc4.level=-5;perc4.pan=-0.55;perc4.pan_depth=1\n"
    "perc5.role=Clap;perc5.engine=Noise;perc5.noise=1;perc5.bursts=4;perc5.burst_spacing=9;perc5.noise_decay=180;"
    "perc5.filter=Band Pass;perc5.cutoff=1400;perc5.resonance=0.35;perc5.level=0.5;perc5.low_cut=300\n"
    "perc6.role=Snare;perc6.engine=Tone;perc6.pitch=190;perc6.pitch_amount=1.6;perc6.pitch_decay=25;perc6.decay=90;"
    "perc6.noise=0.8;perc6.noise_decay=140;perc6.filter=High Pass;perc6.cutoff=250;perc6.level=-4;perc6.low_cut=160;perc6.cut_track=2\n"
    "perc7.role=Rim;perc7.engine=FM;perc7.pitch=1700;perc7.fm_ratio=2.61;perc7.fm_index=2.2;perc7.decay=28;perc7.pitch_decay=6;"
    "perc7.filter=Band Pass;perc7.cutoff=2200;perc7.resonance=0.3;perc7.level=-6;perc7.pan=-0.45;perc7.pan_depth=1\n"
    "perc8.role=Shaker;perc8.engine=Noise;perc8.noise=1;perc8.noise_decay=45;perc8.filter=Low Pass;perc8.cutoff=11000;"
    "perc8.low_cut=3000;perc8.resonance=0.1;perc8.level=1;perc8.pan=0.55;perc8.pan_depth=1\n"
    "perc9.role=Tom;perc9.engine=Modal;perc9.pitch=220;perc9.low_cut=190;perc9.mode_set=Membrane;perc9.mode_damp=0.6;perc9.decay=280;"
    "perc9.noise=0.08;perc9.noise_decay=15;perc9.filter=Low Pass;perc9.cutoff=6000;perc9.level=-2;perc9.pan=-0.35;perc9.pan_depth=1;perc9.tune=1\n"
    "perc10.role=Conga;perc10.engine=Modal;perc10.pitch=330;perc10.mode_set=Harmonic;perc10.mode_damp=0.4;perc10.decay=180;"
    "perc10.noise=0.05;perc10.noise_decay=8;perc10.filter=Low Pass;perc10.cutoff=8000;perc10.low_cut=220;perc10.level=-3;perc10.pan=0.5;perc10.pan_depth=1;perc10.tune=1\n"
    "perc11.role=Zap;perc11.engine=FM;perc11.pitch=420;perc11.pitch_amount=8;perc11.pitch_decay=35;perc11.fm_ratio=1.5;"
    "perc11.fm_index=3;perc11.decay=110;perc11.filter=Low Pass;perc11.cutoff=9000;perc11.resonance=0.2;perc11.drive=0.3;"
    "perc11.level=-8;perc11.pan=0.6;perc11.pan_depth=1\n"
    "perc12.role=Blip;perc12.engine=Tone;perc12.pitch=1100;perc12.pitch_amount=1.3;perc12.pitch_decay=4;perc12.decay=45;"
    "perc12.filter=Band Pass;perc12.cutoff=1800;perc12.resonance=0.2;perc12.level=-8;perc12.pan=-0.6;perc12.pan_depth=1;perc12.tune=1\n";

/// 19.09.2026 (round "lowend-acid"): Decay 150 -> 240 ms and Tail Limit -24 -> -15 dB give the kick the
/// reference kicks' body (Kick.h, "Kick body and the limit"); Level -2 -> -6 dB, because the louder,
/// heavier bass now shares the 40 .. 140 Hz band the mix is calibrated against (testMixBalance), and the
/// bass-to-kick ratio lands on the references' (Tools/ref_bass.py, "b/k": -4.2 against a median -4.7).
const ParamDesc KickParams[kick::COUNT] = {
    { "engine", "Engine", "", 0.0f, 1.0f, 0.0f, CurveE::CHOICE, KickEngineNames },
    { "tune", "Tune", "", 0.0f, 1.0f, 1.0f, CurveE::CHOICE, KickTuneNames },
    { "pitch_end", "Pitch End", "Hz", 30.0f, 120.0f, 50.0f, CurveE::LOG },
    { "pitch_start", "Pitch Start", "Hz", 60.0f, 1500.0f, 330.0f, CurveE::LOG },
    { "pitch_decay", "Body Decay", "ms", 5.0f, 150.0f, 13.0f, CurveE::LOG },
    { "punch_decay", "Punch Decay", "ms", 0.5f, 20.0f, 4.0f, CurveE::LOG },
    { "punch", "Punch", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "amp_attack", "Attack", "ms", 0.0f, 10.0f, 0.2f, CurveE::LINEAR },
    { "amp_hold", "Hold", "ms", 0.0f, 150.0f, 12.0f, CurveE::LINEAR },
    { "amp_decay", "Decay", "ms", 20.0f, 1500.0f, 240.0f, CurveE::LOG },
    { "drive", "Drive", "", 0.0f, 1.0f, 0.30f, CurveE::LINEAR },
    { "clip", "Clip", "", 0.0f, 1.0f, 0.0f, CurveE::CHOICE, KickClipNames },
    { "click_level", "Click", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "click_tone", "Click Tone", "Hz", 500.0f, 12000.0f, 4000.0f, CurveE::LOG },
    { "click_decay", "Click Decay", "ms", 0.5f, 30.0f, 3.0f, CurveE::LOG },
    { "tone", "Tone", "Hz", 200.0f, 20000.0f, 9000.0f, CurveE::LOG },
    { "level", "Level", "dB", -36.0f, 6.0f, -6.0f, CurveE::LINEAR },
    { "tail_limit", "Tail Limit", "dB", -60.0f, 0.0f, -15.0f, CurveE::LINEAR },
};

/// 19.09.2026 (round "lowend-acid"): calibrated against Tools/ref_bass.py (24 reference recordings, the
/// sixteenths between the kicks, each band against 20 .. 120 Hz; self test testBassBite). Sub 0.6 -> 0.3
/// and the new Sub Octave 0.5: under 60 Hz -5.7 dB, 60 .. 120 Hz -1.4 dB (references -7.4 and -0.9).
/// Cutoff 140 -> 240 Hz for 120 .. 300 Hz. The new bite layer: 300 Hz .. 2 kHz -10.3 dB (references
/// -9.8; the bass before read -21.6). Level -5 -> -3 dB: bass against kick, see the kick table. Split
/// stays 2 x f0 with a steeper filter (Bass.h).
/// 26.09.2026: the filter models' names (Params.h, VoiceFilterModels), before the first table that uses them.
const char *const PolyFilterModelNames[VoiceFilterModels] = {
    "State Variable", "Moog Ladder",  "Prophet (SSM2040)", "Juno (IR3109)", "Oberheim SEM",
    "Xpander",        "Diode Ladder", "Korg35 (MS-20)",    "Polivoks",      "Wasp"
};
/** @brief bass.filter_model: the bass ladder, then the filter models */
const char *const BassFilterModelNames[VoiceFilterModels] = { "Bass Ladder",   "Moog Ladder",    "Prophet (SSM2040)",
                                                              "Juno (IR3109)", "Oberheim SEM",   "Xpander",
                                                              "Diode Ladder",  "Korg35 (MS-20)", "Polivoks",
                                                              "Wasp" };
// 26.09.2026: the modulation block's choices (Modulation.h), defined outside the anonymous namespace: Params.h names them.
}   // namespace
const char *const LfoShapeNames[] = { "Sine",   "Triangle",      "Saw Up",       "Saw Down",
                                      "Square", "Sample & Hold", "Smooth Random" };
const char *const LfoSyncNames[] = {
    "Free", "4 Bars", "2 Bars", "1 Bar", "1/2", "1/4", "1/8", "1/16", "1/4 T", "1/8 T"
};
const char *const ModSourceNames[] = { "Off",     "LFO 1",      "LFO 2",    "LFO 3", "LFO 4",
                                       "Mod Env", "Filter Env", "Velocity", "Key",   "Random" };
const char *const ModDestNames[] = { "Off",      "Pitch",  "Osc 2 Pitch", "Pulse Width", "Table Position",
                                     "FM Index", "Cutoff", "Resonance",   "Filter Mode", "Level",
                                     "Pan" };
const char *const BassModDestNames[] = { "Off", "Pulse Width", "Cutoff", "Resonance", "Filter Mode", "Level" };
const int         BassModDestMap[BassModDests] = {
    0, 3, 6, 7, 8, 9
};   // ModDest: Off, PulseWidth, Cutoff, Resonance, FilterMode, Level
const char *const AcidModDestNames[] = { "Off", "Pitch", "Cutoff", "Resonance", "Level", "Pan" };
const int AcidModDestMap[AcidModDests] = { 0, 1, 6, 7, 9, 10 };   // ModDest: Off, Pitch, Cutoff, Resonance, Level, Pan
namespace
{
/** @brief The bass. */
const ParamDesc BassParams[bass::COUNT] = {
    { "wave", "Wave", "", 0.0f, 1.0f, 0.15f, CurveE::LINEAR },
    { "pulse_width", "Pulse Width", "", 0.05f, 0.95f, 0.5f, CurveE::LINEAR },
    { "sub", "Sub", "", 0.0f, 1.0f, 0.3f, CurveE::LINEAR },
    { "sub_mode", "Sub Mode", "", 0.0f, 1.0f, 1.0f, CurveE::CHOICE, SubModeNames },
    { "split_ratio", "Split", "x f0", 1.2f, 3.0f, 2.0f, CurveE::LINEAR },
    { "kick_lock", "Kick Lock", "", 0.0f, 2.0f, 2.0f, CurveE::CHOICE, KickLockNames },
    { "retrigger", "Retrigger", "", 0.0f, 1.0f, 1.0f, CurveE::TOGGLE },
    { "start_phase", "Start Phase", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "cutoff", "Cutoff", "Hz", 20.0f, 10000.0f, 240.0f, CurveE::LOG },
    { "resonance", "Resonance", "", 0.0f, 1.0f, 0.3f, CurveE::LINEAR },
    { "env_amount", "Env Amount", "oct", 0.0f, 8.0f, 4.0f, CurveE::LINEAR },
    { "filter_decay", "Filter Decay", "ms", 3.0f, 1000.0f, 75.0f, CurveE::LOG },
    { "key_track", "Key Track", "", 0.0f, 1.0f, 0.6f, CurveE::LINEAR },
    { "vel_to_cutoff", "Vel > Cutoff", "", 0.0f, 1.0f, 0.25f, CurveE::LINEAR },
    { "drive", "Drive", "", 0.0f, 1.0f, 0.3f, CurveE::LINEAR },
    { "amp_attack", "Attack", "ms", 0.1f, 50.0f, 0.8f, CurveE::LOG },
    { "amp_decay", "Decay", "ms", 5.0f, 2000.0f, 180.0f, CurveE::LOG },
    { "amp_sustain", "Sustain", "", 0.0f, 1.0f, 0.55f, CurveE::LINEAR },
    { "amp_release", "Release", "ms", 1.0f, 500.0f, 10.0f, CurveE::LOG },
    // 25.09.2026, the mix guide: the bass gives way to the kick by 2 .. 4 dB (0.37 is -4 dB; it was -6).
    { "duck_depth", "Duck Depth", "", 0.0f, 1.0f, 0.37f, CurveE::LINEAR },
    { "duck_hold", "Duck Hold", "ms", 0.0f, 200.0f, 25.0f, CurveE::LINEAR },
    { "duck_release", "Duck Release", "ms", 5.0f, 500.0f, 60.0f, CurveE::LOG },
    { "level", "Level", "dB", -36.0f, 6.0f, -3.0f, CurveE::LINEAR },
    // Appended 19.09.2026 (round "lowend-acid"): the bite layer, a saturated copy of the oscillator
    // low-passed around 400 .. 800 Hz with its own envelope (Bass.h, "Bite"). The envelope opens the
    // cutoff three octaves at the onset and closes it within 90 ms: the pluck of the roll.
    { "bite", "Bite", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "bite_cutoff", "Bite Cutoff", "Hz", 100.0f, 4000.0f, 500.0f, CurveE::LOG },
    { "bite_env", "Bite Env", "oct", 0.0f, 5.0f, 3.0f, CurveE::LINEAR },
    { "bite_decay", "Bite Decay", "ms", 5.0f, 1000.0f, 90.0f, CurveE::LOG },
    { "bite_drive", "Bite Drive", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "bite_resonance", "Bite Reso", "", 0.0f, 1.0f, 0.2f, CurveE::LINEAR },
    // The octave of the sub: a sine at twice the fundamental, phase-locked to it, in the sub's level
    // units. It carries the 60 .. 120 Hz weight the references have without a filter near the
    // fundamental (Bass.h, "Sub octave").
    { "sub_octave", "Sub Octave", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "filter_model", "Filter Model", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, BassFilterModelNames },
    { "filter_mode", "Filter Mode", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    // 26.09.2026, the modulation block (Params.h, Modulation.h).
    { "filt_attack", "Filter Attack", "ms", 0.1f, 4000.0f, 0.1f, CurveE::LOG },
    { "filt_sustain", "Filter Sustain", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "filt_release", "Filter Release", "ms", 5.0f, 8000.0f, 300.0f, CurveE::LOG },
    { "menv_attack", "Mod Attack", "ms", 0.1f, 8000.0f, 10.0f, CurveE::LOG },
    { "menv_decay", "Mod Decay", "ms", 5.0f, 12000.0f, 800.0f, CurveE::LOG },
    { "menv_sustain", "Mod Sustain", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "menv_release", "Mod Release", "ms", 5.0f, 12000.0f, 400.0f, CurveE::LOG },
    { "lfo1_rate", "LFO 1 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo1_shape", "LFO 1 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo1_sync", "LFO 1 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo1_retrig", "LFO 1 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo1_fade", "LFO 1 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "lfo2_rate", "LFO 2 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo2_shape", "LFO 2 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo2_sync", "LFO 2 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo2_retrig", "LFO 2 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo2_fade", "LFO 2 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "lfo3_rate", "LFO 3 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo3_shape", "LFO 3 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo3_sync", "LFO 3 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo3_retrig", "LFO 3 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo3_fade", "LFO 3 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "lfo4_rate", "LFO 4 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo4_shape", "LFO 4 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo4_sync", "LFO 4 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo4_retrig", "LFO 4 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo4_fade", "LFO 4 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "mx1_src", "Mod 1 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx1_dst", "Mod 1 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx1_amount", "Mod 1 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx2_src", "Mod 2 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx2_dst", "Mod 2 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx2_amount", "Mod 2 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx3_src", "Mod 3 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx3_dst", "Mod 3 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx3_amount", "Mod 3 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx4_src", "Mod 4 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx4_dst", "Mod 4 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx4_amount", "Mod 4 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx5_src", "Mod 5 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx5_dst", "Mod 5 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx5_amount", "Mod 5 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx6_src", "Mod 6 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx6_dst", "Mod 6 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx6_amount", "Mod 6 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx7_src", "Mod 7 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx7_dst", "Mod 7 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx7_amount", "Mod 7 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx8_src", "Mod 8 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx8_dst", "Mod 8 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx8_amount", "Mod 8 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
};

const char *const DelayTimeNames[] = {
    "1/16", "1/8", "3/16", "1/4", "3/8", "1/2"
};   ///< the tempo delay's times, in notes
const char *const PolyOscNames[] = { "Supersaw", "VA", "FM",
                                     "Wavetable" };   ///< poly.osc: the first oscillator's engine
const char *const PolyOsc2Names[] = { "Off", "Supersaw", "VA", "FM",
                                      "Wavetable" };   ///< poly.osc2: the second oscillator's engine, or none
/** @brief poly.osc2_interval: the second oscillator's interval */
const char *const PolyOsc2IntervalNames[] = { "-2 Oct", "-1 Oct", "-5th", "Unison", "+5th", "+1 Oct" };
/**
 * The `table` choice: the six tables written as spectra in code, then whatever library tables this
 * build ships (Core/include/phos/WaveTableList.inl, generated by Tools/wt_pack.py). The six come
 * first and in this order for good: a `.phosset` and a plugin state store the index as a number, so
 * "lead.table=1" must go on meaning Vocal whatever is added behind it.
 * @param[in] index     the table's index
 * @param[in] name      its name
 * @param[in] id        its library id
 * @param[in] lane      the lane it suits
 * @param[in] fallback  the built-in table standing in
 */
#define PHOS_WT( index, name, id, lane, fallback ) name,
/** @brief poly.table: the wavetables. */
const char *const WaveTableNames[] = {
    "Classic", "Vocal", "Glass", "PWM", "Sync", "Formant Saw",
#include "phos/WaveTableList.inl"
};
#undef PHOS_WT
static_assert( sizeof( WaveTableNames ) / sizeof( WaveTableNames[0] ) == NumWaveTables,
               "the table choice list and NumWaveTables have come apart" );
/** @brief poly.gate_pattern: the trance gate's patterns */
const char *const GatePatternNames[] = { "Sixteenths", "Eighths", "Rolling", "Gallop", "3-3-2", "Triplets" };
const char *const PolyModNames[] = { "Off", "Flanger", "Phaser",
                                     "Comb" };   ///< poly.mod: the voice's modulation insert
static_assert( sizeof( PolyModNames ) / sizeof( PolyModNames[0] ) == static_cast<int>( PolyModE::COUNT ),
               "one name per PolyMod" );
const char *const PolyFilterNames[] = { "Low Pass", "Band Pass", "High Pass",
                                        "Notch" };   ///< poly.filter_type: the filter's response
static_assert( sizeof( PolyFilterNames ) / sizeof( PolyFilterNames[0] ) == static_cast<int>( PolyFilterE::COUNT ),
               "one name per PolyFilter" );

/// 18.09.2026 (round "mix-foundation"): the default voicing is the "driven" one of three candidates
/// rendered for the user -- a 303 into a distortion pedal. Cutoff 650 -> 900 Hz, env 4 -> 3.5 oct,
/// decay 350 -> 220 ms, accent 0.6 -> 0.7, slide 55 -> 70 ms (the 60 .. 80 ms of the rule text), drive
/// 0.45 -> 0.85 on the new, stronger drive curve (Acid.cpp, DriveMaxDb), low cut 150 -> 250 Hz so the
/// distortion does not thicken the low mids, delay feedback 0.45 -> 0.5, hall 0.05 -> 0.08, and the
/// level -9 -> -2.3 dB, which puts the acid alone at -20 LUFS in the first drop, level with the arp.
const ParamDesc AcidParams[acid::COUNT] = {
    { "wave", "Wave", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "cutoff", "Cutoff", "Hz", 80.0f, 8000.0f, 900.0f, CurveE::LOG },
    { "resonance", "Resonance", "", 0.0f, 1.0f, 0.72f, CurveE::LINEAR },
    { "env_amount", "Env Amount", "oct", 0.0f, 6.0f, 3.5f, CurveE::LINEAR },
    { "decay", "Decay", "ms", 30.0f, 2000.0f, 220.0f, CurveE::LOG },
    { "accent", "Accent", "", 0.0f, 1.0f, 0.7f, CurveE::LINEAR },
    { "slide_time", "Slide Time", "ms", 5.0f, 200.0f, 70.0f, CurveE::LOG },
    { "amp_decay", "Amp Decay", "ms", 50.0f, 4000.0f, 900.0f, CurveE::LOG },
    { "key_track", "Key Track", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "drive", "Drive", "", 0.0f, 1.0f, 0.85f, CurveE::LINEAR },
    { "squelch", "Squelch", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "squelch_start", "Squelch Start", "x", 2.0f, 32.0f, 12.0f, CurveE::LOG },
    { "squelch_time", "Squelch Time", "ms", 5.0f, 150.0f, 28.0f, CurveE::LOG },
    { "comb_mix", "Comb Mix", "", 0.0f, 1.0f, 0.55f, CurveE::LINEAR },
    { "comb_feedback", "Comb Feedback", "", 0.0f, 0.97f, 0.82f, CurveE::LINEAR },
    { "low_cut", "Low Cut", "Hz", 150.0f, 1000.0f, 250.0f, CurveE::LOG },
    { "delay_send", "Delay Send", "", 0.0f, 1.0f, 0.25f, CurveE::LINEAR },
    { "delay_left", "Delay Left", "", 0.0f, 5.0f, 2.0f, CurveE::CHOICE, DelayTimeNames },
    { "delay_right", "Delay Right", "", 0.0f, 5.0f, 3.0f, CurveE::CHOICE, DelayTimeNames },
    { "delay_feedback", "Delay Feedback", "", 0.0f, 0.9f, 0.50f, CurveE::LINEAR },
    { "delay_high_pass", "Delay High Pass", "Hz", 150.0f, 2000.0f, 400.0f, CurveE::LOG },
    { "delay_low_pass", "Delay Low Pass", "Hz", 800.0f, 16000.0f, 4500.0f, CurveE::LOG },
    { "room_send", "Room Send", "", 0.0f, 1.0f, 0.1f, CurveE::LINEAR },
    { "hall_send", "Hall Send", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },   // 25.09.2026: the plate, not the far room
    { "duck", "Duck", "", 0.0f, 1.0f, 0.15f, CurveE::LINEAR },
    { "level", "Level", "dB", -36.0f, 6.0f, -2.3f, CurveE::LINEAR },
    // Appended 16.09.2026 (acid colour round). Off by default: an all-pass chain changes no band's
    // power, but it does smear the attack, and no measurement asks for that to be the default sound.
    { "disperse", "Disperse", "x", 0.0f, static_cast<float>( DisperseStages ), 0.0f, CurveE::INT },
    { "disperse_freq", "Disperse Freq", "Hz", 200.0f, 8000.0f, 1250.0f, CurveE::LOG },
    // Appended 20.09.2026 (round "reverb"). Off by default: hall_send keeps feeding the plain hall
    // exactly as before.
    { "hall_gate", "Hall Gate", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    // 25.09.2026, the mix guide: the acid is the middle plane -- 15 to 25 % into a short plate (Engine.h).
    { "plate_send", "Plate Send", "", 0.0f, 1.0f, 0.15f, CurveE::LINEAR },
    // 26.09.2026, the modulation block without the filter envelope (Params.h).
    { "menv_attack", "Mod Attack", "ms", 0.1f, 8000.0f, 10.0f, CurveE::LOG },
    { "menv_decay", "Mod Decay", "ms", 5.0f, 12000.0f, 800.0f, CurveE::LOG },
    { "menv_sustain", "Mod Sustain", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "menv_release", "Mod Release", "ms", 5.0f, 12000.0f, 400.0f, CurveE::LOG },
    { "lfo1_rate", "LFO 1 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo1_shape", "LFO 1 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo1_sync", "LFO 1 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo1_retrig", "LFO 1 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo1_fade", "LFO 1 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "lfo2_rate", "LFO 2 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo2_shape", "LFO 2 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo2_sync", "LFO 2 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo2_retrig", "LFO 2 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo2_fade", "LFO 2 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "lfo3_rate", "LFO 3 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo3_shape", "LFO 3 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo3_sync", "LFO 3 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo3_retrig", "LFO 3 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo3_fade", "LFO 3 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "lfo4_rate", "LFO 4 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo4_shape", "LFO 4 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo4_sync", "LFO 4 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo4_retrig", "LFO 4 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo4_fade", "LFO 4 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "mx1_src", "Mod 1 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx1_dst", "Mod 1 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, AcidModDestNames },
    { "mx1_amount", "Mod 1 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx2_src", "Mod 2 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx2_dst", "Mod 2 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, AcidModDestNames },
    { "mx2_amount", "Mod 2 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx3_src", "Mod 3 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx3_dst", "Mod 3 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, AcidModDestNames },
    { "mx3_amount", "Mod 3 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx4_src", "Mod 4 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx4_dst", "Mod 4 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, AcidModDestNames },
    { "mx4_amount", "Mod 4 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx5_src", "Mod 5 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx5_dst", "Mod 5 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, AcidModDestNames },
    { "mx5_amount", "Mod 5 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx6_src", "Mod 6 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx6_dst", "Mod 6 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, AcidModDestNames },
    { "mx6_amount", "Mod 6 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx7_src", "Mod 7 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx7_dst", "Mod 7 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, AcidModDestNames },
    { "mx7_amount", "Mod 7 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx8_src", "Mod 8 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx8_dst", "Mod 8 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, AcidModDestNames },
    { "mx8_amount", "Mod 8 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
};

/** @brief A polyphonic voice (lead, counter, arp, stab, pad, drone). */
const ParamDesc PolyParams[poly::COUNT] = {
    { "osc", "Oscillator", "", 0.0f, 3.0f, 0.0f, CurveE::CHOICE, PolyOscNames },
    { "detune", "Detune", "", 0.0f, 1.0f, 0.55f, CurveE::LINEAR },
    { "mix", "Mix", "", 0.0f, 1.0f, 0.75f, CurveE::LINEAR },
    { "dynamic_detune", "Dynamic Detune", "", 0.0f, 1.0f, 0.6f, CurveE::LINEAR },
    { "wave", "Wave", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "pulse_width", "Pulse Width", "", 0.05f, 0.95f, 0.5f, CurveE::LINEAR },
    { "fm_ratio", "FM Ratio", "", 0.5f, 8.0f, 2.0f, CurveE::LINEAR },
    { "fm_index", "FM Index", "", 0.0f, 10.0f, 2.5f, CurveE::LINEAR },
    { "fm_decay", "FM Decay", "ms", 5.0f, 2000.0f, 250.0f, CurveE::LOG },
    { "table", "Table", "", 0.0f, static_cast<float>( NumWaveTables - 1 ), 1.0f, CurveE::CHOICE, WaveTableNames },
    { "position", "Position", "", 0.0f, 1.0f, 0.3f, CurveE::LINEAR },
    { "pos_env", "Position Env", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "pos_decay", "Position Decay", "ms", 10.0f, 8000.0f, 1500.0f, CurveE::LOG },
    { "pos_lfo_depth", "Position LFO", "", 0.0f, 0.5f, 0.15f, CurveE::LINEAR },
    { "pos_lfo_beats", "LFO Period", "beats", 0.25f, 64.0f, 16.0f, CurveE::LOG },
    { "cutoff", "Cutoff", "Hz", 200.0f, 18000.0f, 10000.0f, CurveE::LOG },
    { "resonance", "Resonance", "", 0.0f, 1.0f, 0.15f, CurveE::LINEAR },
    { "env_amount", "Env Amount", "oct", 0.0f, 6.0f, 2.0f, CurveE::LINEAR },
    { "filter_decay", "Filter Decay", "ms", 5.0f, 3000.0f, 400.0f, CurveE::LOG },
    { "key_track", "Key Track", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    // The floor reaches down to 40 Hz since 18.09.2026: the pad's sub foundation (Melody.h, rule 20)
    // needs its high pass under 73 Hz where kick and bass rest, and a range that stopped at 150 Hz
    // could not be reached by a control event. The defaults of lead and arp (220 Hz) keep the depth
    // rule where they always did; a text or a state that stores 150..400 Hz reads back unchanged.
    { "hp_floor", "HP Floor", "Hz", 40.0f, 400.0f, 220.0f, CurveE::LOG },
    { "hp_track", "HP Track", "x f0", 0.0f, 1.0f, 1.0f, CurveE::LINEAR },
    { "amp_attack", "Attack", "ms", 0.3f, 2000.0f, 4.0f, CurveE::LOG },
    { "amp_decay", "Decay", "ms", 5.0f, 4000.0f, 500.0f, CurveE::LOG },
    { "amp_sustain", "Sustain", "", 0.0f, 1.0f, 0.75f, CurveE::LINEAR },
    { "amp_release", "Release", "ms", 5.0f, 4000.0f, 180.0f, CurveE::LOG },
    { "width", "Width", "", 0.0f, 1.0f, 0.8f, CurveE::LINEAR },
    { "vel_sens", "Velocity", "", 0.0f, 1.0f, 0.4f, CurveE::LINEAR },
    { "delay_send", "Delay Send", "", 0.0f, 1.0f, 0.3f, CurveE::LINEAR },
    { "delay_left", "Delay Left", "", 0.0f, 5.0f, 2.0f, CurveE::CHOICE, DelayTimeNames },
    { "delay_right", "Delay Right", "", 0.0f, 5.0f, 3.0f, CurveE::CHOICE, DelayTimeNames },
    { "delay_feedback", "Delay Feedback", "", 0.0f, 0.9f, 0.4f, CurveE::LINEAR },
    { "delay_high_pass", "Delay High Pass", "Hz", 150.0f, 2000.0f, 350.0f, CurveE::LOG },
    { "delay_low_pass", "Delay Low Pass", "Hz", 800.0f, 16000.0f, 6000.0f, CurveE::LOG },
    { "room_send", "Room Send", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "hall_send", "Hall Send", "", 0.0f, 1.0f, 0.25f, CurveE::LINEAR },
    { "duck", "Duck", "", 0.0f, 1.0f, 0.2f, CurveE::LINEAR },
    { "gate", "Trance Gate", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "gate_pattern", "Gate Pattern", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, GatePatternNames },
    { "gate_depth", "Gate Depth", "", 0.0f, 1.0f, 0.85f, CurveE::LINEAR },
    { "gate_duty", "Gate Duty", "", 0.05f, 1.0f, 0.5f, CurveE::LINEAR },
    { "gate_attack", "Gate Attack", "ms", 0.5f, 60.0f, 3.0f, CurveE::LOG },
    { "gate_release", "Gate Release", "ms", 0.5f, 120.0f, 12.0f, CurveE::LOG },
    { "gate_tone", "Gate Tone", "", 0.0f, 1.0f, 0.4f, CurveE::LINEAR },
    { "level", "Level", "dB", -36.0f, 6.0f, -4.0f, CurveE::LINEAR },
    // Appended 16.09.2026 (acid colour round).
    { "disperse", "Disperse", "x", 0.0f, static_cast<float>( DisperseStages ), 0.0f, CurveE::INT },
    { "disperse_freq", "Disperse Freq", "Hz", 200.0f, 8000.0f, 1250.0f, CurveE::LOG },
    // Thermal drift: the standard deviation of the slow random walk, in cents (Poly.h). 1 cent is
    // +-2 cents at two sigma, the range the analogue literature gives for a warmed-up VCO.
    { "drift", "Drift", "ct", 0.0f, 8.0f, 1.0f, CurveE::LINEAR },
    // 19.09.2026 (round "voices"): which output of the voice's state-variable filter is heard (PolyKernel.h).
    // Low pass is what every voice played before; the per-track recipes (Composer.cpp) may pick the others.
    { "filter_type", "Filter Type", "", 0.0f, 3.0f, 0.0f, CurveE::CHOICE, PolyFilterNames },
    // 20.09.2026 (round "dialogue"). Glide: the time constant of the pitch slew between two notes of
    // the voice (Poly.h); 0 is the instantaneous jump, so a voice
    // that does not set it renders bit for bit as it did. Pan: the dry signal's place in the image, the
    // role property the user's rule asks for ("Lead leicht links, Counter leicht rechts"); the delay
    // behind the voice keeps its own left/right times and is not panned with it.
    { "glide", "Glide", "ms", 0.0f, 400.0f, 0.0f, CurveE::LINEAR },
    { "pan", "Pan", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    // The voice's own modulation insert (Engine.cpp; PsyFx.h). Off is the plain voice,
    // sample for sample -- the insert is not even ticked.
    { "mod", "Modulation", "", 0.0f, 3.0f, 0.0f, CurveE::CHOICE, PolyModNames },
    { "mod_beats", "Mod Period", "beats", 0.5f, 64.0f, 16.0f, CurveE::LOG },
    { "mod_depth", "Mod Depth", "", 0.0f, 1.0f, 0.7f, CurveE::LINEAR },
    { "mod_feedback", "Mod Feedback", "", -0.9f, 0.9f, 0.5f, CurveE::LINEAR },
    { "mod_mix", "Mod Mix", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    // Appended 20.09.2026 (round "reverb"). Off by default: hall_send keeps feeding the plain hall
    // exactly as before.
    { "hall_gate", "Hall Gate", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    // Appended 22.09.2026 (round "Klangfarben"). Both groups are off at their defaults -- osc2 "Off"
    // and every LFO depth 0 -- so an older set renders the same samples it did.
    { "osc2", "Oscillator 2", "", 0.0f, 4.0f, 0.0f, CurveE::CHOICE, PolyOsc2Names },
    { "osc2_mix", "Osc 2 Mix", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "osc2_interval", "Osc 2 Interval", "", 0.0f, 5.0f, 1.0f, CurveE::CHOICE, PolyOsc2IntervalNames },
    { "osc2_detune", "Osc 2 Detune", "ct", -50.0f, 50.0f, 0.0f, CurveE::LINEAR },
    { "lfo_beats", "Voice LFO", "beats", 0.25f, 64.0f, 8.0f, CurveE::LOG },
    { "lfo_cutoff", "LFO to Cutoff", "oct", 0.0f, 4.0f, 0.0f, CurveE::LINEAR },
    { "lfo_pitch", "LFO to Pitch", "ct", 0.0f, 50.0f, 0.0f, CurveE::LINEAR },
    { "lfo_amp", "LFO to Level", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    // 25.09.2026, the mix guide: the plate, the middle plane. Off by default; the counter, the arp and the stab
    // take it per instance (DefaultPoly).
    { "plate_send", "Plate Send", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    // 25.09.2026, the Dark-Ambient addon. Distance: one cue for all five of distance (Engine.h, distanceCue) --
    // 0 the front plane, 0.5 the middle, 1 the back; each instance's default is its own plane (DefaultPoly), so
    // the knob at its default changes nothing and a ride moves the voice with every cue at once. Slow movement:
    // free periods of 13, 21 and 34 s (ratios of the golden mean), so no pattern is heard to come round.
    { "distance", "Distance", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "slow_mod", "Slow Movement", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "filter_model", "Filter Model", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, PolyFilterModelNames },
    { "filter_mode", "Filter Mode", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    // 26.09.2026, the modulation block (Params.h, Modulation.h).
    { "filt_attack", "Filter Attack", "ms", 0.1f, 4000.0f, 0.1f, CurveE::LOG },
    { "filt_sustain", "Filter Sustain", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "filt_release", "Filter Release", "ms", 5.0f, 8000.0f, 300.0f, CurveE::LOG },
    { "menv_attack", "Mod Attack", "ms", 0.1f, 8000.0f, 10.0f, CurveE::LOG },
    { "menv_decay", "Mod Decay", "ms", 5.0f, 12000.0f, 800.0f, CurveE::LOG },
    { "menv_sustain", "Mod Sustain", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "menv_release", "Mod Release", "ms", 5.0f, 12000.0f, 400.0f, CurveE::LOG },
    { "lfo1_rate", "LFO 1 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo1_shape", "LFO 1 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo1_sync", "LFO 1 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo1_retrig", "LFO 1 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo1_fade", "LFO 1 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "lfo2_rate", "LFO 2 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo2_shape", "LFO 2 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo2_sync", "LFO 2 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo2_retrig", "LFO 2 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo2_fade", "LFO 2 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "lfo3_rate", "LFO 3 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo3_shape", "LFO 3 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo3_sync", "LFO 3 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo3_retrig", "LFO 3 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo3_fade", "LFO 3 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "lfo4_rate", "LFO 4 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo4_shape", "LFO 4 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo4_sync", "LFO 4 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo4_retrig", "LFO 4 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo4_fade", "LFO 4 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "mx1_src", "Mod 1 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx1_dst", "Mod 1 Target", "", 0.0f, 10.0f, 0.0f, CurveE::CHOICE, ModDestNames },
    { "mx1_amount", "Mod 1 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx2_src", "Mod 2 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx2_dst", "Mod 2 Target", "", 0.0f, 10.0f, 0.0f, CurveE::CHOICE, ModDestNames },
    { "mx2_amount", "Mod 2 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx3_src", "Mod 3 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx3_dst", "Mod 3 Target", "", 0.0f, 10.0f, 0.0f, CurveE::CHOICE, ModDestNames },
    { "mx3_amount", "Mod 3 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx4_src", "Mod 4 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx4_dst", "Mod 4 Target", "", 0.0f, 10.0f, 0.0f, CurveE::CHOICE, ModDestNames },
    { "mx4_amount", "Mod 4 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx5_src", "Mod 5 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx5_dst", "Mod 5 Target", "", 0.0f, 10.0f, 0.0f, CurveE::CHOICE, ModDestNames },
    { "mx5_amount", "Mod 5 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx6_src", "Mod 6 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx6_dst", "Mod 6 Target", "", 0.0f, 10.0f, 0.0f, CurveE::CHOICE, ModDestNames },
    { "mx6_amount", "Mod 6 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx7_src", "Mod 7 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx7_dst", "Mod 7 Target", "", 0.0f, 10.0f, 0.0f, CurveE::CHOICE, ModDestNames },
    { "mx7_amount", "Mod 7 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx8_src", "Mod 8 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx8_dst", "Mod 8 Target", "", 0.0f, 10.0f, 0.0f, CurveE::CHOICE, ModDestNames },
    { "mx8_amount", "Mod 8 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
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
const char *const DefaultPoly =
    // 20.09.2026, round "dialogue". The user's four separations between the two leads, as far as they
    // are knob values: the lead flows (a 45 ms portamento -- 0.44 of a sixteenth at 145 BPM, so a step
    // has arrived within one note and a listener hears the bend, not a slur), stands slightly left and
    // echoes on a dotted eighth (0.75 beats) against a dotted quarter (1.5); the counter whips (no
    // glide, no sustain, a filter that shuts in 60 ms), stands slightly right and pings 1/16 against
    // 1/8. The pan of +-0.20 is the middle of the rule's "15 to 25 %".
    // The modulation insert of 20.09.2026 (poly.mod): the rule asks for the comb/flanger/phaser colour
    // on the three line voices, tempo-synced. The lead takes the slow phaser (16 beats -- four bars, so
    // the sweep is a phrase long and never a wobble), the counter the faster flanger (4 beats, one bar,
    // which suits a voice that answers in single whips), the arp a *static* comb: its notes are a fifth
    // of a sixteenth long, far too short for any sweep to be heard inside one, so what a moving filter
    // would give it is a colour that changes between notes -- a fixed comb gives it the hollow metallic
    // timbre instead. The mixes are small on purpose: this is a colour, not an effect the voice
    // disappears into, and the listening excerpts are where it gets judged.
    "lead.glide=45;lead.pan=-0.20;lead.room_send=0.1;lead.hall_send=0;lead.delay_left=2;lead.delay_right=4;"
    "lead.mod=Phaser;lead.mod_beats=16;lead.mod_depth=0.8;lead.mod_feedback=0.45;lead.mod_mix=0.22;"
    "lead.osc2_mix=0.28;lead.osc2_detune=7;lead.lfo_beats=8;lead.lfo_cutoff=0.30;lead.lfo_amp=0.06\n"
    "arp.detune=0.3;arp.mix=0.6;arp.cutoff=3500;arp.env_amount=2.8;arp.filter_decay=140;arp.resonance=0.3;"
    "arp.amp_attack=0.8;arp.amp_decay=220;arp.amp_sustain=0;arp.amp_release=12;arp.delay_send=0.35;"
    "arp.delay_left=2;arp.delay_right=1;arp.level=-5;arp.width=0.6;arp.hall_send=0;arp.duck=0.2;arp.plate_send=0.2;arp.distance=0.5;"
    "arp.mod=Comb;arp.mod_depth=0;arp.mod_feedback=0.6;arp.mod_mix=0.20;"
    "arp.osc2_mix=0.26;arp.osc2_detune=4;arp.lfo_beats=8;arp.lfo_cutoff=0.28;arp.lfo_amp=0.08\n"
    "pad.osc=Wavetable;pad.table=Vocal;pad.detune=0.35;pad.mix=0.7;pad.dynamic_detune=0;pad.cutoff=5000;pad.env_amount=0;pad.resonance=0.1;"
    "pad.amp_attack=700;pad.amp_decay=2000;pad.amp_sustain=1;pad.amp_release=1800;pad.hp_floor=140;pad.hp_track=1;pad.width=1;"
    "pad.delay_send=0;pad.hall_send=0;pad.plate_send=0.35;pad.distance=1;pad.slow_mod=0.5;pad.duck=0.5;pad.pos_env=0.3;pad.pos_decay=3000;pad.gate_pattern=Sixteenths;pad.level=-16;"
    "pad.osc2_mix=0.38;pad.osc2_detune=9;pad.lfo_beats=16;pad.lfo_cutoff=0.50;pad.lfo_amp=0.10\n"
    // 19.09.2026, round "voices". The counter-lead answers the lead in another timbre -- the user's
    // inventory names "wavetable / vocal character" -- so it is a formant saw read by the wavetable
    // oscillator with a slow vowel movement of the position and a quarter-note echo, and no supersaw.
    "counter.osc=Wavetable;counter.table=Formant Saw;counter.detune=0.25;counter.mix=0.55;counter.dynamic_detune=0.3;"
    "counter.cutoff=5000;counter.env_amount=3.2;counter.filter_decay=60;counter.resonance=0.2;counter.position=0.4;"
    // 20.09.2026: its envelope is the staccato of the rule, not the sustained answer of the voices round
    // (amp_decay 400 -> 70 ms, sustain 0.6 -> 0, release 110 -> 35 ms, filter_decay 300 -> 60 ms with a
    // deeper envelope so the whip still opens). Pan and delay are its role's (see the lead above).
    "counter.pos_lfo_depth=0.25;counter.pos_lfo_beats=4;counter.amp_attack=1;counter.amp_decay=70;counter.amp_sustain=0;"
    "counter.amp_release=35;counter.delay_send=0.4;counter.delay_left=0;counter.delay_right=1;counter.hall_send=0;counter.distance=0.5;"
    "counter.width=0.7;counter.duck=0.2;counter.plate_send=0.2;counter.level=-3;counter.pan=0.20;counter.glide=0;"   // level -7 -> -3 dB (23.09.2026: "kaum hoerbar")
    "counter.mod=Flanger;counter.mod_beats=4;counter.mod_depth=0.75;counter.mod_feedback=0.55;counter.mod_mix=0.28;"
    "counter.osc2_mix=0.30;counter.osc2_detune=6;counter.lfo_beats=4;counter.lfo_cutoff=0.35\n"
    // The stab: a short, bright chord -- a narrow supersaw through a filter envelope that closes within
    // 90 ms, no sustain, and throws into the delay and the hall so the hit leaves an echo behind it.
    "stab.detune=0.35;stab.mix=0.6;stab.dynamic_detune=0;stab.cutoff=1800;stab.env_amount=3.5;stab.filter_decay=90;"
    "stab.resonance=0.35;stab.amp_attack=0.5;stab.amp_decay=180;stab.amp_sustain=0;stab.amp_release=60;stab.delay_send=0.45;"
    "stab.delay_left=2;stab.delay_right=3;stab.delay_feedback=0.45;stab.hall_send=0;stab.distance=0.5;stab.width=0.9;stab.duck=0.2;stab.plate_send=0.15;stab.level=-7;"
    "stab.osc2_mix=0.26;stab.osc2_detune=5;stab.lfo_beats=8;stab.lfo_cutoff=0.22\n"
    // The tonic drone: a warm organ table held across a section, dark, slow in and slow out (the
    // cross-fade at section boundaries), with more thermal drift than the other voices. Its high pass
    // tracks an octave under each voice with a floor at 140 Hz; where kick and bass rest the composer
    // opens the floor to 40 Hz for its low octave, as it does for the pad's sub foundation.
    "drone.osc=Wavetable;drone.table=Organ 034;drone.detune=0.2;drone.mix=0.5;drone.dynamic_detune=0;drone.cutoff=900;"
    "drone.env_amount=0;drone.resonance=0.1;drone.amp_attack=1500;drone.amp_decay=2000;drone.amp_sustain=1;"
    "drone.amp_release=2500;drone.hp_floor=140;drone.hp_track=0.5;drone.width=1;drone.delay_send=0;drone.hall_send=0;drone.plate_send=0.5;drone.distance=1;drone.slow_mod=0.25;"
    "drone.duck=0.45;drone.position=0.3;drone.pos_lfo_depth=0.1;drone.pos_lfo_beats=32;drone.drift=2;drone.level=-17;"
    "drone.osc2_mix=0.36;drone.osc2_detune=5;drone.lfo_beats=32;drone.lfo_cutoff=0.32;drone.lfo_amp=0.08\n";

/// sfx.level -12 -> -3 dB (18.09.2026): the SFX strip measured -27.6 LUFS over the first drop and its
/// events 5 to 15 dB under the mix at their loudest; the per-type balance is in Sfx.cpp (TypeGainDb).
const ParamDesc SfxParams[sfx::COUNT] = {
    { "level", "Level", "dB", -36.0f, 6.0f, -3.0f, CurveE::LINEAR },
    { "noise", "Noise", "", 0.0f, 1.0f, 0.6f, CurveE::LINEAR },
    { "resonance", "Resonance", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "brightness", "Brightness", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "impact_decay", "Impact Decay", "ms", 200.0f, 4000.0f, 1400.0f, CurveE::LOG },
    { "vowel", "Vowel", "", 0.0f, 1.0f, 0.3f, CurveE::LINEAR },
    { "swell_decay", "Swell Decay", "ms", 200.0f, 6000.0f, 1500.0f, CurveE::LOG },
    { "width", "Width", "", 0.0f, 1.0f, 0.7f, CurveE::LINEAR },
    { "room_send", "Room Send", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "hall_send", "Hall Send", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "duck", "Duck", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    // 19.09.2026, round "fx-psychedelia" (Sfx.h): the sub drop, mono and under the kick's sidechain.
    { "sub_level", "Sub Drop", "dB", -36.0f, 6.0f, -14.0f, CurveE::LINEAR },
    { "sub_duck", "Sub Duck", "", 0.0f, 1.0f, 1.0f, CurveE::LINEAR },
    // 20.09.2026, round "wandering-fx" (Sfx.h): a directed pan trajectory and a growing reverb-send
    // trajectory over an event's own length. Off by default, so older sets render unchanged.
    { "wander", "Wander", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "wander_send", "Wander Send", "", 0.0f, 1.0f, 0.85f, CurveE::LINEAR },
    // 24.09.2026: the bank preset each family plays, 0 = Auto (Params.h, sfx::PresetRiser). The ranges are the
    // families' sizes (SfxBankTables.cpp, SfxBankCount); the self test holds the two together.
    { "preset_riser", "Riser", "", 0.0f, 256.0f, 0.0f, CurveE::INT },
    { "preset_downlifter", "Downlifter", "", 0.0f, 128.0f, 0.0f, CurveE::INT },
    { "preset_impact", "Impact", "", 0.0f, 128.0f, 0.0f, CurveE::INT },
    { "preset_sweep", "Sweep", "", 0.0f, 256.0f, 0.0f, CurveE::INT },
    { "preset_formant_shot", "Formant Shot", "", 0.0f, 96.0f, 0.0f, CurveE::INT },
    { "preset_reverse_swell", "Reverse Swell", "", 0.0f, 256.0f, 0.0f, CurveE::INT },
    { "preset_zap", "Zap", "", 0.0f, 96.0f, 0.0f, CurveE::INT },
    { "preset_squelch", "Squelch", "", 0.0f, 128.0f, 0.0f, CurveE::INT },
    { "preset_bubble", "Bubble", "", 0.0f, 64.0f, 0.0f, CurveE::INT },
    { "preset_reverse_crash", "Reverse Crash", "", 0.0f, 128.0f, 0.0f, CurveE::INT },
    { "preset_atmosphere", "Atmosphere", "", 0.0f, 512.0f, 0.0f, CurveE::INT },
    // 25.09.2026: the effects in the plate (the main room); the throw before an eight-bar change rides it.
    { "plate_send", "Plate Send", "", 0.0f, 1.0f, 0.1f, CurveE::LINEAR },
};

/// The shamanic bed (Texture.h), the voices (Vocal.h) and the modulation effects (PsyFx.h), 19.09.2026.
/// Their levels are the calibration of that round (docs/rounds/2026-09.md): a bed that is felt rather than heard,
/// voices that stand in a breakdown without covering the pad.
const ParamDesc TextureParams[texture::COUNT] = {
    { "width", "Width", "", 0.0f, 1.0f, 0.8f, CurveE::LINEAR },
    { "bowl_decay", "Bowl Decay", "s", 1.0f, 20.0f, 7.0f, CurveE::LOG },
    { "bowl_bright", "Bowl Brightness", "", 0.0f, 1.0f, 0.4f, CurveE::LINEAR },
    { "didge_formant", "Didge Formant", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "didge_breath", "Didge Breath", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "jaw_sweep", "Jaw Harp Sweep", "", 0.0f, 1.0f, 0.6f, CurveE::LINEAR },
    { "room_send", "Room Send", "", 0.0f, 1.0f, 0.1f, CurveE::LINEAR },
    { "hall_send", "Hall Send", "", 0.0f, 1.0f, 0.35f, CurveE::LINEAR },
    { "fx_send", "FX Send", "", 0.0f, 1.0f, 0.3f, CurveE::LINEAR },
    { "duck", "Duck", "", 0.0f, 1.0f, 0.45f, CurveE::LINEAR },   // 25.09.2026: the back plane, 4 .. 8 dB
};

/** @brief The voices. */
const ParamDesc VocalParams[vocal::COUNT] = {
    { "pitch", "Pitch Spread", "st", 0.0f, 12.0f, 3.0f, CurveE::LINEAR },
    { "drive", "Drive", "", 0.0f, 1.0f, 0.35f, CurveE::LINEAR },
    { "throw_send", "Delay Throw", "", 0.0f, 1.0f, 0.6f, CurveE::LINEAR },
    { "throw_beats", "Throw Time", "", 0.0f, 5.0f, 4.0f, CurveE::CHOICE, DelayTimeNames },
    { "throw_feedback", "Throw Feedback", "", 0.0f, 0.9f, 0.55f, CurveE::LINEAR },
    { "fx_send", "FX Send", "", 0.0f, 1.0f, 0.45f, CurveE::LINEAR },
    { "hall_send", "Hall Send", "", 0.0f, 1.0f, 0.3f, CurveE::LINEAR },
    { "duck", "Duck", "", 0.0f, 1.0f, 0.4f, CurveE::LINEAR },   // 25.09.2026: the back plane
    { "width", "Width", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
};

/** @brief The modulation effects: the SFX insert and the send chain. */
const ParamDesc PsyFxParams[psyfx::COUNT] = {
    { "flanger_beats", "Flanger Period", "beats", 0.5f, 32.0f, 8.0f, CurveE::LOG },
    { "flanger_depth", "Flanger Depth", "", 0.0f, 1.0f, 0.7f, CurveE::LINEAR },
    { "flanger_feedback", "Flanger Feedback", "", -0.9f, 0.9f, 0.6f, CurveE::LINEAR },
    { "flanger_mix", "Flanger Mix", "", 0.0f, 1.0f, 0.35f, CurveE::LINEAR },
    { "phaser_beats", "Phaser Period", "beats", 0.5f, 32.0f, 16.0f, CurveE::LOG },
    { "phaser_depth", "Phaser Depth", "", 0.0f, 1.0f, 0.8f, CurveE::LINEAR },
    { "phaser_feedback", "Phaser Feedback", "", 0.0f, 0.9f, 0.5f, CurveE::LINEAR },
    { "phaser_mix", "Phaser Mix", "", 0.0f, 1.0f, 0.3f, CurveE::LINEAR },
    { "shift_hz", "Frequency Shift", "Hz", -500.0f, 500.0f, 0.0f, CurveE::LINEAR },
    { "shift_mix", "Shifter Mix", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "return", "Send Return", "dB", -36.0f, 6.0f, -3.0f, CurveE::LINEAR },
    { "motion", "Motion", "", 0.0f, 1.0f, 0.7f, CurveE::LINEAR },
};

/** @brief The rooms: the room, the hall, the plate, the delay. */
const ParamDesc FxParams[fx::COUNT] = {
    { "room_size", "Room Size", "", 0.3f, 3.0f, 0.5f, CurveE::LINEAR },
    { "room_decay", "Room Decay", "s", 0.1f, 4.0f, 0.7f, CurveE::LOG },
    { "room_damping", "Room Damping", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    { "hall_size", "Hall Size", "", 0.3f, 3.0f, 1.6f, CurveE::LINEAR },
    // 25.09.2026, the Dark-Ambient addon: the hall is the far room C, 8 .. 20 s, heard only in intros and breakdowns
    // (ComposerControls.cpp rides its return), its pre-delay almost none (the back plane's 0 .. 10 ms, 5 ms at 145 BPM).
    { "hall_decay", "Hall Decay", "s", 0.3f, 20.0f, 10.0f, CurveE::LOG },
    { "hall_damping", "Hall Damping", "", 0.0f, 1.0f, 0.45f, CurveE::LINEAR },
    { "hall_pre_delay", "Hall Pre-Delay", "beats", 0.0f, 0.5f, 0.0125f, CurveE::LINEAR },
    // The hall's send filter since 25.09.2026 (room and plate have their own): the far room's 400 Hz .. 3 kHz.
    { "low_cut", "Hall Low Cut", "Hz", 150.0f, 1000.0f, 400.0f, CurveE::LOG },
    { "high_cut", "Hall High Cut", "Hz", 1000.0f, 20000.0f, 3000.0f, CurveE::LOG },
    { "room_return", "Room Return", "dB", -36.0f, 6.0f, -6.0f, CurveE::LINEAR },
    { "hall_return", "Hall Return", "dB", -36.0f, 6.0f, -6.0f, CurveE::LINEAR },
    // 25.09.2026, the mix guide: returns 3 .. 6 dB under the kick (0.45 is -5.2 dB), back within 80 .. 120 ms.
    { "return_duck", "Return Duck", "", 0.0f, 1.0f, 0.45f, CurveE::LINEAR },
    { "return_duck_release", "Return Duck Release", "ms", 10.0f, 800.0f, 100.0f, CurveE::LOG },
    // The plate: the main room B (the addon) -- the middle plane in a drop (1 .. 2 s), pads and lead in a
    // breakdown (3 .. 6 s, a ride). Its pre-delay is a distance cue, not a tempo value (the addon's correction:
    // near sources long, far ones short -- front 40 .. 60, middle 20 .. 30, back 0 .. 10 ms).
    { "plate_size", "Plate Size", "", 0.3f, 3.0f, 0.8f, CurveE::LINEAR },
    { "plate_decay", "Plate Decay", "s", 0.2f, 8.0f, 1.5f, CurveE::LOG },
    { "plate_damping", "Plate Damping", "", 0.0f, 1.0f, 0.35f, CurveE::LINEAR },
    { "plate_pre_delay", "Plate Pre-Delay", "ms", 0.0f, 100.0f, 25.0f, CurveE::LINEAR },
    { "plate_return", "Plate Return", "dB", -36.0f, 6.0f, -6.0f, CurveE::LINEAR },
    // 25.09.2026, the Dark-Ambient addon: the near room A (lead, snare, zaps) with the front's long pre-delay and
    // its own send filter; the plate's filter; and the plate's share that feeds the far room C (the hall) -- the
    // serial feed that makes a breakdown's room endless without a second, foreign place.
    { "room_pre_delay", "Room Pre-Delay", "ms", 0.0f, 100.0f, 50.0f, CurveE::LINEAR },
    { "room_low_cut", "Room Low Cut", "Hz", 100.0f, 1000.0f, 200.0f, CurveE::LOG },
    { "room_high_cut", "Room High Cut", "Hz", 1000.0f, 20000.0f, 10000.0f, CurveE::LOG },
    { "plate_low_cut", "Plate Low Cut", "Hz", 100.0f, 1000.0f, 300.0f, CurveE::LOG },
    { "plate_high_cut", "Plate High Cut", "Hz", 1000.0f, 20000.0f, 6000.0f, CurveE::LOG },
    { "plate_to_hall", "Plate to Hall", "", 0.0f, 1.0f, 0.15f, CurveE::LINEAR },
};

/// 23.09.2026, round "Keyboard": "By channel" plays the acid on channel 1, the lead on 2 ... the drone on 7.
/// 01.10.2026: the bass and the kit appended (the values before them stay what they were in a saved set).
const char *const KeyboardPartNames[] = { "Off", "Acid",  "Lead",       "Counter", "Arp", "Stab",
                                          "Pad", "Drone", "By channel", "Bass",    "Kit" };
const char *const KeyboardModeNames[] = { "Replace", "Layer" };   ///< perform.keyboard_mode
/// mix.keyboard_lower (02.10.2026): the voices of KeyboardPartNames without "By channel" (the plugin maps them back).
const char *const KeyboardLowerNames[] = { "Off",  "Acid", "Lead",  "Counter", "Arp",
                                           "Stab", "Pad",  "Drone", "Bass",    "Kit" };
const char *const KeyboardSplitNames[] = { "C1", "C2", "C3", "C4", "C5" };   ///< mix.keyboard_split: 36 .. 84
const char *const KeyboardVelocityNames[] = { "As Played", "Soft", "Hard", "Fixed" };   ///< mix.keyboard_velocity
const char *const MonitorNames[] = { "Normal", "Mono", "Sub", "Side" };   ///< master.monitor: what the output plays
/** @brief The mix: the strips' levels, mutes and sends, the ducks. */
const ParamDesc MixParams[mix::COUNT] = {
    { "kick_mute", "Kick Mute", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "bass_mute", "Bass Mute", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "track_gain", "Track Gain", "dB", -12.0f, 12.0f, 0.0f, CurveE::LINEAR },
    { "perc_mute", "Perc Mute", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    // +3 dB instead of +1 (16.09.2026, Phase 9): with the band-limited hats the kit no longer spends
    // its level above 12 kHz, and the presence and air bands land on the reference median.
    // +1.2 since 26.09.2026: the hats came up by 2.5 .. 3 dB after the user heard them no more ("Jetzt höre ich
    // praktisch gar keine Hi Hats mehr"), and a kit that is mostly its hats while it sounds then stood 4.4 dB over
    // the effects (testDialogue.levels, 4 dB) and the air band at the top of its window; 0.8 dB off the whole kit
    // puts both back, the air band on the reference median, and leaves the hats 1.2 .. 2.2 dB louder than before.
    { "perc_level", "Perc Level", "dB", -24.0f, 12.0f, 1.2f, CurveE::LINEAR },
    { "acid_mute", "Acid Mute", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "acid_level", "Acid Level", "dB", -24.0f, 12.0f, 0.0f, CurveE::LINEAR },
    // 19.09.2026, round "voices": the strips in the order of the voices' groups (Params.h, mix::).
    { "lead_mute", "Lead Mute", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    // -3 dB (19.09.2026, round "voices"): excerpt B of the listening seed (track 2's drop) sat +9.8 dB over
    // the reference median in presence once its foundation was no longer over-matched (Composer.cpp,
    // probeLoudness); with the lead muted it read +1.8, so the lead carried most of it. -3 dB: +7.6.
    // -6 dB (19.09.2026, round "arrangement"): the user after listening to the voices round, "insgesamt ist
    // der Lead und auch der Arp zu laut"; the arp -2 -> -5 with it, the counter and the stab 3 dB down as
    // well so that they stay under the lead. 4 dB each took the presence band of testMixBalance 2.2 dB
    // under the reference median; docs/rounds/2026-09.md has the band balance before and after.
    { "lead_level", "Lead Level", "dB", -24.0f, 12.0f, -6.0f, CurveE::LINEAR },
    { "counter_mute", "Counter Mute", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    // 21.09.2026: measured at -3 dB the counter-lead sounded 11.6 dB under the lead it answers
    // (seed 1, 173 notes against 1080, median MIDI 78 against 66, same velocity, same note length,
    // identical synth parameters) -- a dialogue partner nobody can hear. Why an octave up costs
    // that much through the same voice is not explained and wants its own round; this is the
    // mixer doing what a mixer is for, set to the number the measurement asks for.
    { "counter_level", "Counter Level", "dB", -24.0f, 12.0f, 5.0f, CurveE::LINEAR },
    { "arp_mute", "Arp Mute", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    // -2 dB (18.09.2026): the arp was the loudest melodic part and owned 300 Hz .. 2 kHz, where the
    // congas, toms and the clap have to be heard (DefaultKit).
    { "arp_level", "Arp Level", "dB", -24.0f, 12.0f, -5.0f, CurveE::LINEAR },
    { "stab_mute", "Stab Mute", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "stab_level", "Stab Level", "dB", -24.0f, 12.0f, -3.0f, CurveE::LINEAR },
    { "pad_mute", "Pad Mute", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "pad_level", "Pad Level", "dB", -24.0f, 12.0f, 0.0f, CurveE::LINEAR },
    { "drone_mute", "Drone Mute", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "drone_level", "Drone Level", "dB", -24.0f, 12.0f, 0.0f, CurveE::LINEAR },
    { "sfx_mute", "SFX Mute", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    // 20.09.2026, round "dialogue". The user hears the effects and the voices as too quiet, and their
    // rule is that in psytrance the effects *are* the composition. Measured on the listening seed's first
    // drop (32 bars), solo renders against the full mix, counting only the 40 ms frames in which the part
    // actually sounds (scratch/dialogue/events.py -- averaging over the silence, as the brief's -15.8 and
    // -21.1 dB do, measures sparsity as much as level): the effects sounded in 12.8 % of the frames and
    // stood 11.5 dB under the mix in them, the voices in 4.8 % and 13.0 dB under, while the percussion
    // stood 9.3 dB under and the acid 7.4 dB. So an effect event was *quieter than a hi-hat* and a spoken
    // phrase quieter than either. The targets: an effect event level with the percussion, a voice between
    // the percussion and the acid -- which is where a phrase has to sit to be a voice and not a texture.
    // Measured after: effects -8.5 dB (level with the percussion's -9.3), voices -8.4 dB. The master's
    // compressor and limiter give back about two thirds of a dB per dB on the strip, which is why the
    // strips move by 3 and 7 dB for 3.0 and 3.9 dB in the mix. docs/rounds/2026-09.md has the rest.
    // +4.5 since 26.09.2026: the kit came up for the hats the user no longer heard (clap +1.5, shaker +2 dB), and a
    // percussion hit then stood 4.56 dB over an effect event (testDialogue.levels, 4). The rule is that the two are
    // alike, so the effects follow; 1.5 dB on the strip is about 1 dB in the mix (the two thirds above).
    { "sfx_level", "SFX Level", "dB", -24.0f, 12.0f, 4.5f, CurveE::LINEAR },
    { "perc_room", "Perc Room", "", 0.0f, 1.0f, 0.12f, CurveE::LINEAR },
    { "perc_hall", "Perc Hall", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "duck_attack", "Duck Attack", "ms", 0.5f, 30.0f, 2.0f, CurveE::LOG },
    { "duck_hold", "Duck Hold", "ms", 0.0f, 200.0f, 20.0f, CurveE::LINEAR },
    // 25.09.2026, the mix guide: the pads and the bed come back within 100 .. 150 ms (it was 180); the lines have
    // their own release (duck_release_lines), the returns theirs (fx.return_duck_release).
    { "duck_release", "Duck Release", "ms", 10.0f, 800.0f, 125.0f, CurveE::LOG },
    // 19.09.2026, round "fx-psychedelia": the shamanic bed and the voices get their own strips.
    { "texture_mute", "Texture Mute", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    // 22.09.2026, the user: "das Didgeridoo und die Klangschalen hab ich ueberhaupt noch nie
    // gehoert". They were not missing -- measured over 128 bars of seed 7, the bed sounds in 72 of
    // 211 seconds (a difference render against mix.texture_mute) -- they were 25.5 dB under the mix
    // at the median and 39 dB under full scale, which in a psytrance mix is not a quiet layer but
    // no layer at all. The placement was tested (FormSfx.cpp, placePsychedelia; the self test checks
    // the bed never lands in a drop or a buildup); the *audibility* was not, which is the gap this
    // number and testBedAudible close together. +9 dB puts the median at about -16 dB under the
    // mix -- still a bed, under the voices, but there.
    { "texture_level", "Texture Level", "dB", -24.0f, 12.0f, 9.0f, CurveE::LINEAR },
    { "vocal_mute", "Vocal Mute", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "vocal_level", "Vocal Level", "dB", -24.0f, 12.0f, 7.0f, CurveE::LINEAR },   // 20.09.2026, see sfx_level
    // 23.09.2026, round "Presets": the synth keeps the sound on its page; the composer's recipes leave it alone.
    { "kick_own", "Kick Own Sound", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "bass_own", "Bass Own Sound", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "acid_own", "Acid Own Sound", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lead_own", "Lead Own Sound", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "counter_own", "Counter Own Sound", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "arp_own", "Arp Own Sound", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "stab_own", "Stab Own Sound", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "pad_own", "Pad Own Sound", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "drone_own", "Drone Own Sound", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    // 23.09.2026, round "Keyboard": what a MIDI keyboard plays (Engine.h, liveNoteOn).
    { "keyboard_part", "Keyboard Plays", "", 0.0f, 10.0f, 0.0f, CurveE::CHOICE, KeyboardPartNames },
    { "keyboard_mode", "Keyboard Mode", "", 0.0f, 1.0f, 0.0f, CurveE::CHOICE, KeyboardModeNames },
    { "composer", "Composer", "", 0.0f, 1.0f, 1.0f, CurveE::TOGGLE },   // 01.10.2026, live play only
    // 25.09.2026, the mix guide's ducking matrix. The lines (acid, lead, counter, arp, stab) come back within
    // 50 .. 80 ms, the pads and the bed within 100 .. 150 (duck_release, 125 now); the counter gives way to the
    // lead by 1 .. 3 dB, the pad's band of 500 Hz .. 3 kHz by 2 .. 4 dB while the lead plays (Engine.h).
    { "duck_release_lines", "Duck Release (Lines)", "ms", 10.0f, 800.0f, 65.0f, CurveE::LOG },
    { "counter_duck", "Counter under Lead", "", 0.0f, 1.0f, 0.2f, CurveE::LINEAR },
    { "pad_lead_duck", "Pad Band under Lead", "dB", 0.0f, 12.0f, 3.0f, CurveE::LINEAR },
    // 27.09.2026: the Field track's strip (FieldPlayer.h).
    { "field_mute", "Field Mute", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    // -14 dB (27.09.2026): measured on a Dark Forest intro (seed 7) at 0 dB the place had the highest partial loudness
    // of all parts (69.7 against the voices' 57.7), at -10 still 48; at -14 it sits by the shamanic bed (33), a
    // background, as a place should.
    { "field_level", "Field Level", "dB", -24.0f, 12.0f, -14.0f, CurveE::LINEAR },
    // 02.10.2026: the keyboard's options, all off by default.
    { "keyboard_lower", "Lower Keys Play", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, KeyboardLowerNames },
    { "keyboard_split", "Split At", "", 0.0f, 4.0f, 2.0f, CurveE::CHOICE, KeyboardSplitNames },
    { "keyboard_scale", "Scale Lock", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "keyboard_velocity", "Velocity Curve", "", 0.0f, 3.0f, 0.0f, CurveE::CHOICE, KeyboardVelocityNames },
};

/** @brief The master: the bus compressor, the clipper, the limiter, the loudness. */
const ParamDesc MasterParams[master::COUNT] = {
    { "gain", "Gain", "dB", -24.0f, 24.0f, 0.0f, CurveE::LINEAR },
    { "ceiling", "Ceiling", "dBTP", -12.0f, 0.0f, -1.0f, CurveE::LINEAR },
    { "clip", "Clip", "", 0.0f, 1.0f, 1.0f, CurveE::TOGGLE },
    { "comp_threshold", "Comp Threshold", "dB", -40.0f, 0.0f, -8.0f, CurveE::LINEAR },
    { "comp_ratio", "Comp Ratio", ":1", 1.0f, 10.0f, 1.5f, CurveE::LOG },
    { "comp_knee", "Comp Knee", "dB", 0.0f, 24.0f, 6.0f, CurveE::LINEAR },
    { "comp_attack", "Comp Attack", "ms", 0.1f, 100.0f, 30.0f, CurveE::LOG },
    { "comp_release", "Comp Release", "ms", 10.0f, 2000.0f, 200.0f, CurveE::LOG },
    { "mono_bass", "Mono Bass", "Hz", 40.0f, 300.0f, 120.0f, CurveE::LOG },
    { "limiter", "Limiter", "", 0.0f, 1.0f, 1.0f, CurveE::TOGGLE },
    { "limiter_release", "Limiter Release", "ms", 2.0f, 1000.0f, 20.0f, CurveE::LOG },
    { "target_lufs", "Target Loudness", "LUFS", -20.0f, -4.0f, -9.0f, CurveE::LINEAR },
    { "auto_gain", "Auto Gain", "", 0.0f, 1.0f, 1.0f, CurveE::TOGGLE },
    { "clipper", "Clipper", "", 0.0f, 1.0f, 1.0f, CurveE::TOGGLE },
    { "clipper_threshold", "Clipper Threshold", "dB", -6.0f, 6.0f, 0.0f, CurveE::LINEAR },
    // 25.09.2026, the addon's monitoring: listen in mono, to the sub alone (the kick-bass gap and the tuning are
    // judged there), or to the side signal. After the meter, so Auto Gain and the loudness readings are the mix's.
    { "monitor", "Monitor", "", 0.0f, 3.0f, 0.0f, CurveE::CHOICE, MonitorNames },
};

/// The cue bridge of PLAN 8.3. Appended after the master block, so no parameter that existed before
/// changed its id, its default or its place in the host's list.
const ParamDesc CueParams[cue::COUNT] = {
    { "send", "Send Cues", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "port", "Port", "", 1024.0f, 65535.0f, 9000.0f, CurveE::INT },
    { "beats", "Beat Cues", "", 0.0f, 1.0f, 1.0f, CurveE::TOGGLE },
    // A trim on the lead the sender already computes from the block size and the limiter's lookahead
    // (Cue.h). Positive moves the cue later. It exists because the one part of the chain the plugin
    // cannot ask about is how deep the driver's own buffering is; zero is right for the common case
    // of a single buffer in flight.
    { "lead_ms", "Lead Trim", "ms", -50.0f, 50.0f, 0.0f, CurveE::LINEAR },
};

/** @brief One module: its prefix, table, how many instances exist, and optionally their names. */
struct ModuleSpec
{
    const char        *m_prefix;                    ///< the key prefix ("kick", "perc" ...)
    const ParamDesc   *m_descs;                     ///< its parameters
    int                m_count;                     ///< how many
    int                m_instances;                 ///< how many instances
    const char *const *m_instanceNames = nullptr;   ///< prefixes of the instances instead of prefix + number
};

}   // namespace

// 27.09.2026: the Field track's sampler (Params.h, namespace field; FieldPlayer.h).
const char *const FieldCategoryNames[field::Categories + 1] = {
    "Auto",         "Rainforest",  "Night Country", "Insects",      "Foliage",      "Wetland",        "Mud Bubbles",
    "Cave",         "River",       "Sea",           "Rain on Land", "Rain on Roof", "Wind",           "Underwater",
    "Ice and Snow", "Desert",      "Fire",          "Geothermal",   "Seismic",      "Ritual Objects", "Abandoned",
    "Tunnel",       "Metal Creak", "Dark Drone",    "Electric",     "Ventilation",  "Factory",        "Polar Station",
    "Radio Space",  "Empty Space", "Grain Texture", "NASA"
};
const char *const FieldCategorySlugs[field::Categories] = { "rainforest",
                                                            "night-country",
                                                            "insects",
                                                            "foliage",
                                                            "wetland",
                                                            "mud-bubbles",
                                                            "cave",
                                                            "river",
                                                            "sea",
                                                            "rain-land",
                                                            "rain-roof",
                                                            "wind",
                                                            "underwater",
                                                            "ice-snow",
                                                            "desert",
                                                            "fire",
                                                            "geothermal",
                                                            "seismic",
                                                            "ritual-objects",
                                                            "abandoned",
                                                            "tunnel",
                                                            "metal-creak",
                                                            "dark-drone-noise",
                                                            "electric",
                                                            "ventilation",
                                                            "factory",
                                                            "polar-station",
                                                            "radio-space",
                                                            "empty-space",
                                                            "grain-texture",
                                                            "nasa" };
const char *const FieldModDestNames[7] = { "Off", "Pitch", "Cutoff", "Resonance", "Filter Mode", "Level", "Pan" };
const int         FieldModDestMap[7] = {
    0, 1, 6, 7, 8, 9, 10
};   // ModDest: Off, Pitch, Cutoff, Resonance, FilterMode, Level, Pan
/** @brief field.filter_type: the state-variable filter's response */
const char *const FieldFilterTypeNames[4] = { "Low Pass", "Band Pass", "High Pass", "Notch" };
namespace
{
/** @brief The Field track: the field recordings' sampler. */
const ParamDesc FieldParams[field::COUNT] = {
    // Layer A: a recording of the library -- the category, a variation within it (Auto: the composer's choice), its level,
    // its pitch (resampled), where it starts (and how far that start may wander), and its direction.
    { "a_category", "A Category", "", 0.0f, 31.0f, 0.0f, CurveE::CHOICE, FieldCategoryNames },
    { "a_variation", "A Variation", "", 0.0f, 63.0f, 0.0f, CurveE::INT },
    { "a_level", "A Level", "dB", -36.0f, 6.0f, 0.0f, CurveE::LINEAR },
    { "a_pitch", "A Pitch", "st", -24.0f, 24.0f, 0.0f, CurveE::INT },
    { "a_fine", "A Fine", "ct", -100.0f, 100.0f, 0.0f, CurveE::LINEAR },
    { "a_start", "A Start", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "a_start_random", "A Start Random", "", 0.0f, 1.0f, 1.0f, CurveE::LINEAR },
    { "a_reverse", "A Reverse", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    // Layer B: the same, off by default.
    { "b_on", "B On", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "b_category", "B Category", "", 0.0f, 31.0f, 0.0f, CurveE::CHOICE, FieldCategoryNames },
    { "b_variation", "B Variation", "", 0.0f, 63.0f, 1.0f, CurveE::INT },
    { "b_level", "B Level", "dB", -36.0f, 6.0f, -6.0f, CurveE::LINEAR },
    { "b_pitch", "B Pitch", "st", -24.0f, 24.0f, 0.0f, CurveE::INT },
    { "b_fine", "B Fine", "ct", -100.0f, 100.0f, 0.0f, CurveE::LINEAR },
    { "b_start", "B Start", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "b_start_random", "B Start Random", "", 0.0f, 1.0f, 1.0f, CurveE::LINEAR },
    { "b_reverse", "B Reverse", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "layer_mix", "Layer Mix", "", 0.0f, 1.0f, 0.5f, CurveE::LINEAR },
    // The loop: over [loop_start, loop_end] of the recording, joined by an equal-power crossfade of loop_xfade.
    { "loop", "Loop", "", 0.0f, 1.0f, 1.0f, CurveE::TOGGLE },
    { "loop_start", "Loop Start", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "loop_end", "Loop End", "", 0.0f, 1.0f, 1.0f, CurveE::LINEAR },
    { "loop_xfade", "Loop Crossfade", "ms", 10.0f, 10000.0f, 1500.0f, CurveE::LOG },
    // The amp envelope: long by default, the way a recording enters under an intro and leaves a breakdown.
    { "amp_attack", "Attack", "ms", 1.0f, 30000.0f, 2000.0f, CurveE::LOG },
    { "amp_decay", "Decay", "ms", 5.0f, 30000.0f, 1000.0f, CurveE::LOG },
    { "amp_sustain", "Sustain", "", 0.0f, 1.0f, 1.0f, CurveE::LINEAR },
    { "amp_release", "Release", "ms", 5.0f, 30000.0f, 4000.0f, CurveE::LOG },
    // The filter: the state-variable filter (its type) or one of the nine circuit models (Filters.h), the filter
    // envelope's depth in octaves and its decay (attack, sustain and release are in the block below), and the low cut
    // the depth rule wants under everything but kick and bass.
    { "filter_model", "Filter Model", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, PolyFilterModelNames },
    { "filter_type", "Filter Type", "", 0.0f, 3.0f, 0.0f, CurveE::CHOICE, FieldFilterTypeNames },
    { "cutoff", "Cutoff", "Hz", 40.0f, 20000.0f, 20000.0f, CurveE::LOG },
    { "resonance", "Resonance", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "filter_mode", "Filter Mode", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "env_amount", "Env Amount", "oct", -8.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "filter_decay", "Filter Decay", "ms", 5.0f, 30000.0f, 2000.0f, CurveE::LOG },
    { "low_cut", "Low Cut", "Hz", 20.0f, 1000.0f, 150.0f, CurveE::LOG },
    { "filt_attack", "Filter Attack", "ms", 0.1f, 4000.0f, 0.1f, CurveE::LOG },
    { "filt_sustain", "Filter Sustain", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "filt_release", "Filter Release", "ms", 5.0f, 8000.0f, 300.0f, CurveE::LOG },
    { "menv_attack", "Mod Attack", "ms", 0.1f, 8000.0f, 10.0f, CurveE::LOG },
    { "menv_decay", "Mod Decay", "ms", 5.0f, 12000.0f, 800.0f, CurveE::LOG },
    { "menv_sustain", "Mod Sustain", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "menv_release", "Mod Release", "ms", 5.0f, 12000.0f, 400.0f, CurveE::LOG },
    { "lfo1_rate", "LFO 1 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo1_shape", "LFO 1 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo1_sync", "LFO 1 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo1_retrig", "LFO 1 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo1_fade", "LFO 1 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "lfo2_rate", "LFO 2 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo2_shape", "LFO 2 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo2_sync", "LFO 2 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo2_retrig", "LFO 2 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo2_fade", "LFO 2 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "lfo3_rate", "LFO 3 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo3_shape", "LFO 3 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo3_sync", "LFO 3 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo3_retrig", "LFO 3 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo3_fade", "LFO 3 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "lfo4_rate", "LFO 4 Rate", "Hz", 0.01f, 40.0f, 1.0f, CurveE::LOG },
    { "lfo4_shape", "LFO 4 Shape", "", 0.0f, 6.0f, 0.0f, CurveE::CHOICE, LfoShapeNames },
    { "lfo4_sync", "LFO 4 Sync", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, LfoSyncNames },
    { "lfo4_retrig", "LFO 4 Retrig", "", 0.0f, 1.0f, 0.0f, CurveE::TOGGLE },
    { "lfo4_fade", "LFO 4 Fade", "s", 0.0f, 8.0f, 0.0f, CurveE::LINEAR },
    { "mx1_src", "Mod 1 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx1_dst", "Mod 1 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx1_amount", "Mod 1 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx2_src", "Mod 2 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx2_dst", "Mod 2 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx2_amount", "Mod 2 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx3_src", "Mod 3 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx3_dst", "Mod 3 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx3_amount", "Mod 3 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx4_src", "Mod 4 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx4_dst", "Mod 4 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx4_amount", "Mod 4 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx5_src", "Mod 5 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx5_dst", "Mod 5 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx5_amount", "Mod 5 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx6_src", "Mod 6 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx6_dst", "Mod 6 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx6_amount", "Mod 6 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx7_src", "Mod 7 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx7_dst", "Mod 7 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx7_amount", "Mod 7 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "mx8_src", "Mod 8 Source", "", 0.0f, 9.0f, 0.0f, CurveE::CHOICE, ModSourceNames },
    { "mx8_dst", "Mod 8 Target", "", 0.0f, 5.0f, 0.0f, CurveE::CHOICE, BassModDestNames },
    { "mx8_amount", "Mod 8 Amount", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    // Output: width of the stereo image, pan, the sends and the duck under the kick.
    { "width", "Width", "", 0.0f, 2.0f, 1.0f, CurveE::LINEAR },
    { "pan", "Pan", "", -1.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "room_send", "Room Send", "", 0.0f, 1.0f, 0.1f, CurveE::LINEAR },
    { "plate_send", "Plate Send", "", 0.0f, 1.0f, 0.0f, CurveE::LINEAR },
    { "hall_send", "Hall Send", "", 0.0f, 1.0f, 0.2f, CurveE::LINEAR },
    { "duck", "Duck", "", 0.0f, 1.0f, 0.4f, CurveE::LINEAR },
};

/** @brief The modules, in the order of Module. */
const ModuleSpec Modules[static_cast<int>( ModuleE::COUNT )] = {
    { "compose", ComposeParams, compose::COUNT, 1 },
    { "kick", KickParams, kick::COUNT, 1 },
    { "bass", BassParams, bass::COUNT, 1 },
    { "perc", PercParams, perc::COUNT, PercLanes },
    { "acid", AcidParams, acid::COUNT, 1 },
    { "poly", PolyParams, poly::COUNT, PolyInstances, PolyInstanceNames },
    { "sfx", SfxParams, sfx::COUNT, 1 },
    { "fx", FxParams, fx::COUNT, 1 },
    { "mix", MixParams, mix::COUNT, 1 },
    { "master", MasterParams, master::COUNT, 1 },
    { "cue", CueParams, cue::COUNT, 1 },
    { "texture", TextureParams, texture::COUNT, 1 },
    { "vocal", VocalParams, vocal::COUNT, 1 },
    { "psyfx", PsyFxParams, psyfx::COUNT, 1 },
    { "field", FieldParams, field::COUNT, 1 },
};

/**
 * @brief Whether a curve takes whole steps (Int, Choice, Toggle).
 * @param[in] c  the curve
 * @return true when it takes whole steps
 */
bool isDiscrete( CurveE c )
{
    return c == CurveE::INT || c == CurveE::CHOICE || c == CurveE::TOGGLE;
}

}   // namespace

ParamStore::ParamStore()
{
    for ( auto &row : m_bases )
        for ( int &b : row ) b = -1;
    int total = 0;
    for ( const ModuleSpec &m : Modules ) total += m.m_count * m.m_instances;
    m_entries.reserve( static_cast<size_t>( total ) );
    for ( int mi = 0; mi < static_cast<int>( ModuleE::COUNT ); ++mi )
    {
        const ModuleSpec &m = Modules[mi];
        for ( int inst = 0; inst < m.m_instances && inst < MaxInstances; ++inst )
        {
            m_bases[mi][inst] = static_cast<int>( m_entries.size() );
            std::string prefix =
                m.m_instanceNames != nullptr ? std::string( m.m_instanceNames[inst] ) : std::string( m.m_prefix );
            if ( m.m_instances > 1 && m.m_instanceNames == nullptr ) prefix += std::to_string( inst + 1 );
            for ( int p = 0; p < m.m_count; ++p )
            {
                Entry e{ &m.m_descs[p], prefix + "." + m.m_descs[p].m_key, static_cast<ModuleE>( mi ), inst };
                m_index.emplace( e.m_key, static_cast<int>( m_entries.size() ) );
                m_entries.push_back( std::move( e ) );
            }
        }
    }
    m_values = std::make_unique<std::atomic<float>[]>( m_entries.size() );
    // Defaults: the descriptors', then the instance-specific ones of the default kit on top.
    m_defaults.resize( m_entries.size() );
    for ( int i = 0; i < count(); ++i )
    {
        m_defaults[static_cast<size_t>( i )] = desc( i ).m_defValue;
        m_values[static_cast<size_t>( i )].store( desc( i ).m_defValue, std::memory_order_relaxed );
    }
    parseText( DefaultKit );
    parseText( DefaultPoly );
    for ( int i = 0; i < count(); ++i ) m_defaults[static_cast<size_t>( i )] = get( i );
}

int ParamStore::base( ModuleE m, int instance ) const
{
    const int mi = static_cast<int>( m );
    if ( mi < 0 || mi >= static_cast<int>( ModuleE::COUNT ) || instance < 0 || instance >= MaxInstances ) return -1;
    return m_bases[mi][instance];
}

int ParamStore::find( std::string_view key ) const
{
    const auto it = m_index.find( std::string( key ) );
    return it == m_index.end() ? -1 : it->second;
}

int ParamStore::getInt( int id ) const
{
    return static_cast<int>( std::lround( get( id ) ) );
}

void ParamStore::set( int id, float value )
{
    if ( id < 0 || id >= count() ) return;
    const ParamDesc &d = desc( id );
    if ( !( value == value ) )
        value = m_defaults.empty() ? d.m_defValue : m_defaults[static_cast<size_t>( id )];   // NaN
    float v = value < d.m_minValue ? d.m_minValue : ( value > d.m_maxValue ? d.m_maxValue : value );
    if ( isDiscrete( d.m_curve ) ) v = std::round( v );
    m_values[static_cast<size_t>( id )].store( v, std::memory_order_relaxed );
}

float ParamStore::toNormalised( int id, float value ) const
{
    const ParamDesc &d = desc( id );
    if ( d.m_maxValue <= d.m_minValue ) return 0.0f;
    float n;
    if ( d.m_curve == CurveE::LOG )
        n = std::log( value / d.m_minValue ) / std::log( d.m_maxValue / d.m_minValue );
    else
        n = ( value - d.m_minValue ) / ( d.m_maxValue - d.m_minValue );
    return n < 0.0f ? 0.0f : ( n > 1.0f ? 1.0f : n );
}

float ParamStore::fromNormalised( int id, float norm ) const
{
    const ParamDesc &d = desc( id );
    const float      n = norm < 0.0f ? 0.0f : ( norm > 1.0f ? 1.0f : norm );
    float            v;
    if ( d.m_curve == CurveE::LOG )
        v = d.m_minValue * std::pow( d.m_maxValue / d.m_minValue, n );
    else
        v = d.m_minValue + n * ( d.m_maxValue - d.m_minValue );
    if ( isDiscrete( d.m_curve ) ) v = std::round( v );
    return v;
}

void ParamStore::resetDefaults()
{
    for ( int i = 0; i < count(); ++i )
        m_values[static_cast<size_t>( i )].store( m_defaults[static_cast<size_t>( i )], std::memory_order_relaxed );
}

int ParamStore::moduleCount( ModuleE m )
{
    const int mi = static_cast<int>( m );
    return mi >= 0 && mi < static_cast<int>( ModuleE::COUNT ) ? Modules[mi].m_count : 0;
}

void ParamStore::readModule( ModuleE m, int instance, float *out ) const
{
    const int b = base( m, instance );
    if ( b < 0 ) return;
    const int n = moduleCount( m );
    for ( int i = 0; i < n; ++i ) out[i] = get( b + i );
}

void ParamStore::copyValuesFrom( const ParamStore &other )
{
    const int n = count() < other.count() ? count() : other.count();
    for ( int i = 0; i < n; ++i ) m_values[static_cast<size_t>( i )].store( other.get( i ), std::memory_order_relaxed );
}

namespace
{
/**
 * @brief Lower case without spaces, for matching choice names ("Double Harmonic" = "doubleharmonic").
 * @param[in] s  the name
 * @return the name folded
 */
std::string foldName( std::string_view s )
{
    std::string out;
    for ( char ch : s )
    {
        if ( ch == ' ' || ch == '_' || ch == '-' ) continue;
        out += ( ch >= 'A' && ch <= 'Z' ) ? static_cast<char>( ch - 'A' + 'a' ) : ch;
    }
    return out;
}
}   // namespace

bool ParamStore::parseText( std::string_view text, std::string *error )
{
    // Split into assignments. Newlines and ';' always separate; whitespace separates only where the
    // next word contains '=' -- so a choice name with a space ("compose.scale=Double Harmonic") stays
    // one value while "a=1 b=2" is still two assignments. '#' starts a comment to the end of the line.
    std::vector<std::string> items;
    size_t                   pos = 0;
    bool                     newItem = true;
    while ( pos < text.size() )
    {
        const char ch = text[pos];
        if ( ch == '\n' || ch == ';' || ch == '\r' )
        {
            newItem = true;
            ++pos;
            continue;
        }
        if ( ch == ' ' || ch == '\t' )
        {
            ++pos;
            continue;
        }
        if ( ch == '#' )
        {
            while ( pos < text.size() && text[pos] != '\n' ) ++pos;
            continue;
        }
        size_t end = pos;
        while ( end < text.size() && text[end] != '\n' && text[end] != ';' && text[end] != '\r' && text[end] != ' ' &&
                text[end] != '\t' )
            ++end;
        const std::string_view word = text.substr( pos, end - pos );
        pos = end;
        if ( newItem || word.find( '=' ) != std::string_view::npos || items.empty() )
            items.emplace_back( word );
        else
        {
            items.back() += ' ';
            items.back() += word;
        }
        newItem = false;
    }

    bool ok = true;
    for ( const std::string &item : items )
    {
        const std::string_view tok = trimView( item );
        const size_t           eq = tok.find( '=' );
        if ( eq == std::string_view::npos )
        {
            if ( error && ok ) *error = "missing '=' in \"" + std::string( tok ) + "\"";
            ok = false;
            continue;
        }
        const std::string_view k = trimView( tok.substr( 0, eq ) ), v = trimView( tok.substr( eq + 1 ) );
        const int              id = find( k );
        if ( id < 0 )
        {
            if ( error && ok ) *error = "unknown parameter \"" + std::string( k ) + "\"";
            ok = false;
            continue;
        }
        const ParamDesc &d = desc( id );
        bool             matched = false;
        if ( ( d.m_curve == CurveE::CHOICE && d.m_choices != nullptr ) || d.m_curve == CurveE::TOGGLE )
        {
            const std::string fv = foldName( v );
            if ( d.m_curve == CurveE::TOGGLE && ( fv == "on" || fv == "off" ) )
            {
                set( id, fv == "on" ? 1.0f : 0.0f );
                matched = true;
            }
            for ( int c = 0; !matched && d.m_choices != nullptr && c <= static_cast<int>( d.m_maxValue ); ++c )
            {
                if ( fv == foldName( d.m_choices[c] ) )
                {
                    set( id, static_cast<float>( c ) );
                    matched = true;
                }
            }
        }
        if ( !matched )
        {
            const std::string vs( v );
            char             *stop = nullptr;
            const double      x = std::strtod( vs.c_str(), &stop );
            if ( vs.empty() || stop == nullptr || *stop != 0 )
            {
                if ( error && ok ) *error = "bad value \"" + vs + "\" for " + std::string( k );
                ok = false;
                continue;
            }
            set( id, static_cast<float>( x ) );
        }
    }
    return ok;
}

std::string ParamStore::toText( bool onlyChanged ) const
{
    std::string out;
    char        buf[64];
    for ( int i = 0; i < count(); ++i )
    {
        const float v = get( i );
        if ( onlyChanged && v == m_defaults[static_cast<size_t>( i )] ) continue;
        // %.9g round-trips every float exactly.
        std::snprintf( buf, sizeof( buf ), "%.9g", static_cast<double>( v ) );
        out += key( i );
        out += '=';
        out += buf;
        out += '\n';
    }
    return out;
}

std::string ParamStore::format( int id ) const
{
    const ParamDesc &d = desc( id );
    const float      v = get( id );
    if ( d.m_curve == CurveE::CHOICE && d.m_choices != nullptr ) return d.m_choices[getInt( id )];
    if ( d.m_curve == CurveE::TOGGLE ) return v >= 0.5f ? "On" : "Off";
    char buf[64];
    if ( d.m_curve == CurveE::INT )
        std::snprintf( buf, sizeof( buf ), "%d", getInt( id ) );
    else if ( std::fabs( v ) >= 100.0f )
        std::snprintf( buf, sizeof( buf ), "%.0f", static_cast<double>( v ) );
    else if ( std::fabs( v ) >= 10.0f )
        std::snprintf( buf, sizeof( buf ), "%.1f", static_cast<double>( v ) );
    else
        std::snprintf( buf, sizeof( buf ), "%.2f", static_cast<double>( v ) );
    std::string s = buf;
    if ( d.m_unit != nullptr && d.m_unit[0] != 0 )
    {
        s += ' ';
        s += d.m_unit;
    }
    return s;
}

}   // namespace phos
