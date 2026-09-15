/**
 * @file Params.cpp
 * @brief Module descriptor tables and the parameter store.
 */
#include "phos/Params.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace phos {

const char* const kKeyNames[12] = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };
const char* const kScaleNames[] = { "Aeolian", "Phrygian", "Harmonic Minor", "Phrygian Dominant", "Double Harmonic", "Dorian" };
const char* const kKickPatternNames[] = { "Four", "Four + Fills", "Off" };
const char* const kBassPatternNames[] = { "Rolling", "Gallop", "Skip", "Offbeat", "Triplet" };
const char* const kPercRoleNames[kNumPercRoles] = { "Closed Hat", "Open Hat", "Ride", "Crash", "Clap", "Snare", "Rim",
                                                    "Shaker", "Tom", "Conga", "Zap", "Blip" };

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
    { "tempo_range",     "Tempo Range",     "BPM",   0.0f,  10.0f,   3.0f, Curve::Linear },
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
};

/**
 * @brief The default kit: what each of the twelve lanes starts as.
 *
 * Levels and positions follow the reference measurement of 15.09.2026 (docs/PLAN.md, 6.6): the
 * offbeat hat is the loudest top-end event, the sixteenth layer (shaker, closed-hat ghosts) sits
 * around half of it. Low cuts never go under 150 Hz: below that only kick and bass may play.
 */
const char* const kDefaultKit =
    "perc1.role=Closed Hat;perc1.engine=Metal;perc1.decay=45;perc1.noise=0.35;perc1.noise_decay=35;perc1.filter=High Pass;"
    "perc1.cutoff=7500;perc1.resonance=0.25;perc1.level=5;perc1.pan=0.15;perc1.choke=1;perc1.density=0.6\n"
    "perc2.role=Open Hat;perc2.engine=Metal;perc2.decay=260;perc2.noise=0.35;perc2.noise_decay=220;perc2.filter=High Pass;"
    "perc2.cutoff=6500;perc2.level=2;perc2.pan=-0.1;perc2.choke=1\n"
    "perc3.role=Ride;perc3.engine=Metal;perc3.metal_scale=0.72;perc3.decay=700;perc3.noise=0.2;perc3.noise_decay=400;"
    "perc3.filter=Band Pass;perc3.cutoff=5200;perc3.resonance=0.35;perc3.level=-3;perc3.pan=0.35\n"
    "perc4.role=Crash;perc4.engine=Metal;perc4.metal_scale=0.5;perc4.decay=1600;perc4.noise=0.6;perc4.noise_decay=1400;"
    "perc4.filter=High Pass;perc4.cutoff=3000;perc4.level=-5;perc4.pan=-0.3\n"
    "perc5.role=Clap;perc5.engine=Noise;perc5.noise=1;perc5.bursts=4;perc5.burst_spacing=9;perc5.noise_decay=180;"
    "perc5.filter=Band Pass;perc5.cutoff=1400;perc5.resonance=0.35;perc5.level=-4;perc5.low_cut=300\n"
    "perc6.role=Snare;perc6.engine=Tone;perc6.pitch=190;perc6.pitch_amount=1.6;perc6.pitch_decay=25;perc6.decay=90;"
    "perc6.noise=0.8;perc6.noise_decay=140;perc6.filter=High Pass;perc6.cutoff=250;perc6.level=-6;perc6.low_cut=160\n"
    "perc7.role=Rim;perc7.engine=FM;perc7.pitch=1700;perc7.fm_ratio=2.61;perc7.fm_index=2.2;perc7.decay=28;perc7.pitch_decay=6;"
    "perc7.filter=Band Pass;perc7.cutoff=2200;perc7.resonance=0.3;perc7.level=-10;perc7.pan=-0.25\n"
    "perc8.role=Shaker;perc8.engine=Noise;perc8.noise=1;perc8.noise_decay=45;perc8.filter=High Pass;perc8.cutoff=6000;"
    "perc8.resonance=0.1;perc8.level=-1;perc8.pan=0.25\n"
    "perc9.role=Tom;perc9.engine=Modal;perc9.pitch=220;perc9.low_cut=190;perc9.mode_set=Membrane;perc9.mode_damp=0.6;perc9.decay=280;"
    "perc9.noise=0.08;perc9.noise_decay=15;perc9.filter=Low Pass;perc9.cutoff=6000;perc9.level=-8;perc9.pan=-0.2;perc9.tune=1\n"
    "perc10.role=Conga;perc10.engine=Modal;perc10.pitch=330;perc10.mode_set=Harmonic;perc10.mode_damp=0.4;perc10.decay=180;"
    "perc10.noise=0.05;perc10.noise_decay=8;perc10.filter=Low Pass;perc10.cutoff=8000;perc10.low_cut=220;perc10.level=-10;perc10.pan=0.3;perc10.tune=1\n"
    "perc11.role=Zap;perc11.engine=FM;perc11.pitch=420;perc11.pitch_amount=8;perc11.pitch_decay=35;perc11.fm_ratio=1.5;"
    "perc11.fm_index=3;perc11.decay=110;perc11.filter=Low Pass;perc11.cutoff=9000;perc11.resonance=0.4;perc11.drive=0.3;"
    "perc11.level=-12;perc11.pan=0.4\n"
    "perc12.role=Blip;perc12.engine=Tone;perc12.pitch=1100;perc12.pitch_amount=1.3;perc12.pitch_decay=4;perc12.decay=45;"
    "perc12.filter=Band Pass;perc12.cutoff=1800;perc12.resonance=0.2;perc12.level=-12;perc12.pan=-0.4;perc12.tune=1\n";

const ParamDesc kKickParams[kick::Count] = {
    { "engine",      "Engine",       "",     0.0f,     1.0f,    0.0f, Curve::Choice, kKickEngineNames },
    { "tune",        "Tune",         "",     0.0f,     1.0f,    1.0f, Curve::Choice, kKickTuneNames },
    { "pitch_end",   "Pitch End",    "Hz",  30.0f,   120.0f,   50.0f, Curve::Log },
    { "pitch_start", "Pitch Start",  "Hz",  60.0f,  1500.0f,  330.0f, Curve::Log },
    { "pitch_decay", "Body Decay",   "ms",   5.0f,   150.0f,   22.0f, Curve::Log },
    { "punch_decay", "Punch Decay",  "ms",   0.5f,    20.0f,    4.0f, Curve::Log },
    { "punch",       "Punch",        "",     0.0f,     1.0f,    0.5f, Curve::Linear },
    { "amp_attack",  "Attack",       "ms",   0.0f,    10.0f,    0.2f, Curve::Linear },
    { "amp_hold",    "Hold",         "ms",   0.0f,   150.0f,   12.0f, Curve::Linear },
    { "amp_decay",   "Decay",        "ms",  20.0f,  1500.0f,  150.0f, Curve::Log },
    { "drive",       "Drive",        "",     0.0f,     1.0f,   0.35f, Curve::Linear },
    { "clip",        "Clip",         "",     0.0f,     1.0f,    0.0f, Curve::Choice, kKickClipNames },
    { "click_level", "Click",        "",     0.0f,     1.0f,    0.2f, Curve::Linear },
    { "click_tone",  "Click Tone",   "Hz", 500.0f, 12000.0f, 4000.0f, Curve::Log },
    { "click_decay", "Click Decay",  "ms",   0.5f,    30.0f,    3.0f, Curve::Log },
    { "tone",        "Tone",         "Hz", 200.0f, 20000.0f, 9000.0f, Curve::Log },
    { "level",       "Level",        "dB", -36.0f,     6.0f,   -2.0f, Curve::Linear },
    { "tail_limit",  "Tail Limit",   "dB", -60.0f,     0.0f,  -24.0f, Curve::Linear },
};

const ParamDesc kBassParams[bass::Count] = {
    { "wave",          "Wave",          "",      0.0f,     1.0f,  0.15f, Curve::Linear },
    { "pulse_width",   "Pulse Width",   "",     0.05f,    0.95f,   0.5f, Curve::Linear },
    { "sub",           "Sub",           "",      0.0f,     1.0f,   0.6f, Curve::Linear },
    { "sub_mode",      "Sub Mode",      "",      0.0f,     1.0f,   1.0f, Curve::Choice, kSubModeNames },
    { "split_ratio",   "Split",         "x f0",  1.2f,     3.0f,   2.0f, Curve::Linear },
    { "kick_lock",     "Kick Lock",     "",      0.0f,     2.0f,   2.0f, Curve::Choice, kKickLockNames },
    { "retrigger",     "Retrigger",     "",      0.0f,     1.0f,   1.0f, Curve::Toggle },
    { "start_phase",   "Start Phase",   "",      0.0f,     1.0f,   0.5f, Curve::Linear },
    { "cutoff",        "Cutoff",        "Hz",   20.0f, 10000.0f, 140.0f, Curve::Log },
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
    { "level",         "Level",         "dB",  -36.0f,     6.0f,  -5.0f, Curve::Linear },
};

const char* const kDelayTimeNames[] = { "1/16", "1/8", "3/16", "1/4", "3/8", "1/2" };
const char* const kPolyOscNames[] = { "Supersaw", "VA", "FM" };

const ParamDesc kAcidParams[acid::Count] = {
    { "wave",           "Wave",           "",      0.0f,     1.0f,   0.0f, Curve::Linear },
    { "cutoff",         "Cutoff",         "Hz",   80.0f,  8000.0f, 650.0f, Curve::Log },
    { "resonance",      "Resonance",      "",      0.0f,     1.0f,  0.72f, Curve::Linear },
    { "env_amount",     "Env Amount",     "oct",   0.0f,     6.0f,   4.0f, Curve::Linear },
    { "decay",          "Decay",          "ms",   30.0f,  2000.0f, 350.0f, Curve::Log },
    { "accent",         "Accent",         "",      0.0f,     1.0f,   0.6f, Curve::Linear },
    { "slide_time",     "Slide Time",     "ms",    5.0f,   200.0f,  55.0f, Curve::Log },
    { "amp_decay",      "Amp Decay",      "ms",   50.0f,  4000.0f, 900.0f, Curve::Log },
    { "key_track",      "Key Track",      "",      0.0f,     1.0f,   0.5f, Curve::Linear },
    { "drive",          "Drive",          "",      0.0f,     1.0f,  0.45f, Curve::Linear },
    { "squelch",        "Squelch",        "",      0.0f,     1.0f,   0.0f, Curve::Toggle },
    { "squelch_start",  "Squelch Start",  "x",     2.0f,    32.0f,  12.0f, Curve::Log },
    { "squelch_time",   "Squelch Time",   "ms",    5.0f,   150.0f,  28.0f, Curve::Log },
    { "comb_mix",       "Comb Mix",       "",      0.0f,     1.0f,  0.55f, Curve::Linear },
    { "comb_feedback",  "Comb Feedback",  "",      0.0f,    0.97f,  0.82f, Curve::Linear },
    { "low_cut",        "Low Cut",        "Hz",  150.0f,  1000.0f, 150.0f, Curve::Log },
    { "delay_send",     "Delay Send",     "",      0.0f,     1.0f,  0.25f, Curve::Linear },
    { "delay_left",     "Delay Left",     "",      0.0f,     5.0f,   2.0f, Curve::Choice, kDelayTimeNames },
    { "delay_right",    "Delay Right",    "",      0.0f,     5.0f,   3.0f, Curve::Choice, kDelayTimeNames },
    { "delay_feedback", "Delay Feedback", "",      0.0f,     0.9f,  0.45f, Curve::Linear },
    { "delay_high_pass","Delay High Pass","Hz",  150.0f,  2000.0f, 400.0f, Curve::Log },
    { "delay_low_pass", "Delay Low Pass", "Hz",  800.0f, 16000.0f,4500.0f, Curve::Log },
    { "level",          "Level",          "dB",  -36.0f,     6.0f,  -8.0f, Curve::Linear },
};

const ParamDesc kPolyParams[poly::Count] = {
    { "osc",            "Oscillator",     "",      0.0f,     2.0f,   0.0f, Curve::Choice, kPolyOscNames },
    { "detune",         "Detune",         "",      0.0f,     1.0f,  0.55f, Curve::Linear },
    { "mix",            "Mix",            "",      0.0f,     1.0f,  0.75f, Curve::Linear },
    { "dynamic_detune", "Dynamic Detune", "",      0.0f,     1.0f,   0.6f, Curve::Linear },
    { "wave",           "Wave",           "",      0.0f,     1.0f,   0.0f, Curve::Linear },
    { "pulse_width",    "Pulse Width",    "",     0.05f,    0.95f,   0.5f, Curve::Linear },
    { "fm_ratio",       "FM Ratio",       "",      0.5f,     8.0f,   2.0f, Curve::Linear },
    { "fm_index",       "FM Index",       "",      0.0f,    10.0f,   2.5f, Curve::Linear },
    { "fm_decay",       "FM Decay",       "ms",    5.0f,  2000.0f, 250.0f, Curve::Log },
    { "cutoff",         "Cutoff",         "Hz",  200.0f, 18000.0f,7500.0f, Curve::Log },
    { "resonance",      "Resonance",      "",      0.0f,     1.0f,  0.15f, Curve::Linear },
    { "env_amount",     "Env Amount",     "oct",   0.0f,     6.0f,   2.0f, Curve::Linear },
    { "filter_decay",   "Filter Decay",   "ms",    5.0f,  3000.0f, 400.0f, Curve::Log },
    { "key_track",      "Key Track",      "",      0.0f,     1.0f,   0.5f, Curve::Linear },
    { "hp_floor",       "HP Floor",       "Hz",  150.0f,   400.0f, 220.0f, Curve::Log },
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
    { "level",          "Level",          "dB",  -36.0f,     6.0f,  -8.0f, Curve::Linear },
};

/**
 * @brief Where the two polyphonic instances start: the lead a held supersaw, the arp a plucked one
 *        with less detune (a short sixteenth smears when its seven saws beat against each other).
 */
const char* const kDefaultPoly =
    "arp.detune=0.3;arp.mix=0.6;arp.cutoff=2200;arp.env_amount=2.8;arp.filter_decay=140;arp.resonance=0.3;"
    "arp.amp_attack=0.8;arp.amp_decay=220;arp.amp_sustain=0;arp.amp_release=90;arp.delay_send=0.35;"
    "arp.delay_left=2;arp.delay_right=1;arp.level=-10;arp.width=0.6\n";

const char* const kPolyInstanceNames[kPolyInstances] = { "lead", "arp" };

const ParamDesc kMixParams[mix::Count] = {
    { "kick_mute", "Kick Mute", "", 0.0f, 1.0f, 0.0f, Curve::Toggle },
    { "bass_mute", "Bass Mute", "", 0.0f, 1.0f, 0.0f, Curve::Toggle },
    { "track_gain", "Track Gain", "dB", -12.0f, 12.0f, 0.0f, Curve::Linear },
    { "perc_mute",  "Perc Mute",  "",     0.0f,  1.0f, 0.0f, Curve::Toggle },
    { "perc_level", "Perc Level", "dB", -24.0f, 12.0f, 0.0f, Curve::Linear },
    { "acid_mute",  "Acid Mute",  "",     0.0f,  1.0f, 0.0f, Curve::Toggle },
    { "acid_level", "Acid Level", "dB", -24.0f, 12.0f, 0.0f, Curve::Linear },
    { "lead_mute",  "Lead Mute",  "",     0.0f,  1.0f, 0.0f, Curve::Toggle },
    { "lead_level", "Lead Level", "dB", -24.0f, 12.0f, 0.0f, Curve::Linear },
    { "arp_mute",   "Arp Mute",   "",     0.0f,  1.0f, 0.0f, Curve::Toggle },
    { "arp_level",  "Arp Level",  "dB", -24.0f, 12.0f, 0.0f, Curve::Linear },
};

const ParamDesc kMasterParams[master::Count] = {
    { "gain",    "Gain",    "dB", -24.0f, 12.0f,  0.0f, Curve::Linear },
    { "ceiling", "Ceiling", "dB", -12.0f,  0.0f, -0.3f, Curve::Linear },
    { "clip",    "Clip",    "",     0.0f,  1.0f,  1.0f, Curve::Toggle },
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
    { "mix",     kMixParams,     mix::Count,     1 },
    { "master",  kMasterParams,  master::Count,  1 },
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
