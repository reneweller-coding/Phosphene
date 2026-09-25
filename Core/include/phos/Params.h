/**
 * @file Params.h
 * @brief The parameter system: descriptor tables per module, instantiated in blocks.
 *
 * Noctuary kept one flat enum of parameters. Phosphene has modules that exist several times -- twelve
 * percussion lanes, several polyphonic engines -- so parameters are declared once per module as a
 * table of descriptors, and a module may be instantiated more than once. A parameter's id is the
 * base index of its module instance plus its index in the table; its text key is
 * "<prefix>.<key>" or "<prefix><instance>.<key>" ("kick.pitch_start", "perc3.decay").
 *
 * From the same tables come the host parameters, OSC addresses, the preset text form, the manual and
 * the `--list` output of phos_render. The composer never writes parameters; it reads a snapshot.
 *
 * Values are stored as std::atomic<float> in their real range (Hz, ms, dB), so the audio thread can
 * read what another thread wrote without a lock.
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace phos {

/** @brief How a parameter maps between its real value and the normalised 0..1 of a knob. */
enum class Curve : uint8_t {
    Linear,   ///< proportional
    Log,      ///< logarithmic; minimum must be > 0
    Int,      ///< integer steps, linear
    Choice,   ///< integer index into a list of names
    Toggle,   ///< 0 or 1
};

/** @brief Static description of one parameter. */
struct ParamDesc {
    const char* key;                    ///< identifier inside the module, snake_case
    const char* name;                   ///< display name (English)
    const char* unit;                   ///< unit for display, may be empty
    float minValue;                     ///< lowest real value
    float maxValue;                     ///< highest real value
    float defValue;                     ///< default real value
    Curve curve;                        ///< mapping to the knob
    const char* const* choices = nullptr;   ///< names for Curve::Choice (maxValue + 1 entries)
};

/** @brief The modules that own parameters. */
enum class Module : int { Compose = 0, Kick, Bass, Perc, Acid, Poly, Sfx, Fx, Mix, Master,
                          /** The cue bridge of PLAN 8.3; appended, so no existing id moved. */
                          Cue,
                          /** 19.09.2026, round "fx-psychedelia": the shamanic bed (Texture.h), the voices
                           *  (Vocal.h) and the modulation effects of the SFX bus (PsyFx.h). Appended. */
                          Texture, Vocal, PsyFx,
                          Count };

constexpr int kPercLanes = 12;   ///< instances of the percussion lane module
/**
 * @brief The instances of Module::Poly, in the order the user sees them.
 *
 * 19.09.2026, round "voices": the user asked for the voices to stand in related groups ("der
 * Counter-Lead neben Lead ..., Drone bei Pad") and accepted that the parameter indices move for it.
 * So the three new instances are *not* appended: the leads (lead, counter-lead), the rhythmic chord
 * voices (arp, stab) and the carpets (pad, drone) stand side by side, and every table that follows
 * this order -- the parts (Score.h), the mix strips, the MIDI tracks, the plugin's pages -- follows it
 * too. What this does to a state saved before that date is written down in docs/rounds/2026-09.md (19.09.2026,
 * "Stimmen"): every stored text key ("lead.cutoff", "mix.pad_level") still names the same knob, so
 * such a state loads the old voices unchanged and the new ones at their defaults.
 *
 * Nothing may address an instance by a bare number: use polyBase() or `static_cast<int>(PolyInstance::X)`.
 */
enum class PolyInstance : int { Lead = 0, Counter, Arp, Stab, Pad, Drone, Count };
constexpr int kPolyInstances = static_cast<int>(PolyInstance::Count); ///< instances of the polyphonic engine module
/** @brief The index of an instance, for arrays kept per instance. */
constexpr int polyIndex(PolyInstance i) { return static_cast<int>(i); }
extern const char* const kPolyInstanceNames[kPolyInstances];   ///< "lead", "counter", "arp", "stab", "pad", "drone"

/** @brief Parameters of the composer (read as a snapshot when bars are composed). */
namespace compose {
enum : int { Bpm, Key, Scale, KickPattern, BassPattern, BassGate, BassVariation, BassRegister,
             TrackBars, TrackVariation, SoundVariation, TempoRange, LevelMatch,
             PercDensity, PercVariation, Swing,
             AcidAmount, LeadAmount, ArpAmount, MelodyVariation, MelodyTemperature, SquelchChance,
             BassFollowsChords, PadAmount, SfxAmount, GateChance,
             Style, Arc, StyleTempo, SetMinutes,
             // Phase 8: which predictive model the melodic lines are drawn from (Model.h).
             MelodyModel,
             // Phase 8, bass round: where the pitch of each bass note comes from -- the pattern
             // families of Patterns.h with their phrase figures, or the learned fourth role (Model.h).
             BassModel,
             // 16.09.2026: modal interchange over the tonic pedal (Form.h). Off reproduces every
             // note the program played before that date, which is what the self test measures against.
             ModalInterchange,
             // 16.09.2026, bass rhythm round: where the *onset pattern* of a bar comes from -- the
             // five pattern families of Patterns.h, or a bar drawn from the corpus onset model
             // (Corpus.h, BassRhythm). Pattern is the default and reproduces every note bit for bit.
             BassRhythm,
             // 19.09.2026, round "voices": how often a track has each of the three new voices, and the
             // density of the voices (speech, chants) and of the shamanic bed (FormSfx.cpp, placePsychedelia).
             // Appended: the compose block is not part of the reordering, which concerns the voices only.
             CounterAmount, StabAmount, DroneAmount, VoiceDensity, BedDensity,
             // 19.09.2026, round "polish": the presence match -- each track's lines (lead, counter, arp, stab)
             // brought to a band around the reference median's presence (ComposerLevels.cpp, matchPresence).
             // 22.09.2026: the styles walk through the night instead of one style for all of it
             // (Composer.cpp, walkAt). On by default; off, every track is compose.style as before.
             StyleMix,
             PresenceMatch,
             // 22.09.2026, round "Lead": the lead's density and the pitch entropy of the lines, each a
             // choice whose first entry is "Auto (Style)" -- the style profile's vector decides (Form.h,
             // LeadStyle) unless the user takes the knob. Appended, never reordered.
             LeadDensity, PitchEntropy,
             // 23.09.2026, round "Counter": how the counter-lead answers (Form.h, CounterMode); "Auto (Style)" first.
             CounterMode,
             // 23.09.2026, round "Hoerbarkeit": the quiet lines lifted to a share of the lead's partial loudness
             // (ComposerLevels.cpp, matchAudibility). Appended, in the same place in the table.
             AudibilityMatch, Count };
}
/** @brief Parameters of one percussion lane (module Perc, twelve instances "perc1" .. "perc12"). */
namespace perc {
enum : int { Active, Role, Engine, Pitch, PitchAmount, PitchDecay, FmRatio, FmIndex, ModeSet, ModeDamp,
             MetalScale, Noise, NoiseDecay, Bursts, BurstSpacing, Decay, Filter, Cutoff, Resonance, LowCut,
             Drive, Level, Pan, Choke, Shift, Density, Tune,
             // 16.09.2026, arrangement dynamics. Appended, never reordered: the indices above sit in
             // saved presets, in .phosset files and in the plugin's state.
             PanDepth,   ///< 0..1: how far the tempo-synchronous auto-pan swings the lane (Perc.h)
             PanBars,    ///< the period of that swing in bars (PercKit::setTempo; 145 BPM until it is called)
             CutTrack,   ///< 0..2: the exponent with which the low cut follows a hit's pitch shift (Perc.h)
             Count };
}
/** @brief What a percussion lane plays in the groove; decides its patterns and its MIDI note. */
enum class PercRole : int { ClosedHat = 0, OpenHat, Ride, Crash, Clap, Snare, Rim, Shaker, Tom, Conga, Zap, Blip, Count };
constexpr int kNumPercRoles = static_cast<int>(PercRole::Count);   ///< number of roles
/** @brief Sound sources of a percussion lane. */
enum class PercEngine : int { Noise = 0, Metal, Modal, Tone, Fm, Count };
extern const char* const kPercRoleNames[kNumPercRoles];   ///< display names of perc.role
/** @brief Parameters of the kick drum. */
namespace kick {
enum : int { Engine, Tune, PitchEnd, PitchStart, PitchDecay, PunchDecay, Punch, AmpAttack, AmpHold, AmpDecay,
             Drive, Clip, ClickLevel, ClickTone, ClickDecay, Tone, Level, TailLimit, Count };
}
/** @brief Parameters of the bass. */
namespace bass {
enum : int { Wave, PulseWidth, Sub, SubMode, SplitRatio, KickLock, Retrigger, StartPhase, Cutoff, Resonance,
             EnvAmount, FilterDecay, KeyTrack, VelToCutoff, Drive, AmpAttack, AmpDecay, AmpSustain, AmpRelease,
             DuckDepth, DuckHold, DuckRelease, Level,
             // Appended 19.09.2026 (round "lowend-acid"): the bite layer (Bass.h). New entries go at
             // the end: stored sets, presets and plugin state refer to a parameter by its index.
             Bite, BiteCutoff, BiteEnv, BiteDecay, BiteDrive, BiteResonance, SubOctave, Count };
}
/** @brief Parameters of the acid voice (module Acid, prefix "acid"). */
namespace acid {
enum : int { Wave, Cutoff, Resonance, EnvAmount, Decay, Accent, SlideTime, AmpDecay, KeyTrack, Drive,
             Squelch, SquelchStart, SquelchTime, CombMix, CombFeedback, LowCut,
             DelaySend, DelayLeft, DelayRight, DelayFeedback, DelayHighPass, DelayLowPass,
             RoomSend, HallSend, Duck, Level,
             // Appended 16.09.2026 (acid colour round). New entries go at the end: the index of a
             // parameter is its position in this list and every stored set, preset and automation
             // slot refers to it by that index.
             Disperse, DisperseFreq,
             // Appended 20.09.2026 (round "reverb"): routes hall_send into the gated hall (Reverb.h,
             // Engine.cpp) instead of the plain one -- a big hall that ducks while this voice plays and
             // is cut hard on the absolute bar line. Off by default, so older sets render unchanged.
             HallGate,
             PlateSend,   ///< 25.09.2026, the mix guide: the send into the plate, the middle plane of depth (Engine.h)
             Count };
}
/** @brief Parameters of a polyphonic engine (module Poly, the six instances of PolyInstance). */
namespace poly {
enum : int { Osc, Detune, Mix, DynamicDetune, Wave, PulseWidth, FmRatio, FmIndex, FmDecay,
             Table, Position, PosEnv, PosDecay, PosLfoDepth, PosLfoBeats,
             Cutoff, Resonance, EnvAmount, FilterDecay, KeyTrack, HpFloor, HpTrack,
             AmpAttack, AmpDecay, AmpSustain, AmpRelease, Width, VelSens,
             DelaySend, DelayLeft, DelayRight, DelayFeedback, DelayHighPass, DelayLowPass,
             RoomSend, HallSend, Duck, Gate, GatePattern, GateDepth, GateDuty, GateAttack, GateRelease, GateTone,
             Level,
             // Appended 16.09.2026 (acid colour round), at the end for the same reason as above.
             Disperse, DisperseFreq, Drift,
             // Appended 19.09.2026 (round "voices"): the voice filter's response (PolyFilter) -- the
             // per-track recipes of every voice may choose a band pass or a notch instead of the low pass.
             FilterType,
             // Appended 20.09.2026 (round "dialogue"), for the user's articulation and stereo rules:
             Glide,   ///< ms: the time constant of the portamento between two notes of this voice (Poly.h)
             Pan,     ///< -1 .. 1: where the voice's dry signal stands (its delay keeps its own image)
             /** @name The voice's own modulation insert (Engine.h; PsyFx.h supplies the two effects)
              *  The user's effect list asks for "Kammfilter / Flanger / Phaser als Klangfarbe auf Lead,
              *  Counter und Arp -- nicht nur auf dem FX-Bus": the alien, hollow character, tempo-synced.
              *  On the effects strip alone they would colour every voice at once.
              *  @{ */
             Mod,          ///< PolyMod: off, flanger, phaser or a static comb
             ModBeats,     ///< the sweep's period in beats (tempo-synchronised, so it never drifts)
             ModDepth,     ///< 0 .. 1: how far the comb's teeth or the phaser's notches travel
             ModFeedback,  ///< -0.9 .. 0.9: how sharp they are
             ModMix,       ///< 0 .. 1: how much of the voice goes through it
             /** @} */
             // Appended 20.09.2026 (round "reverb"): routes hall_send into the gated hall (Reverb.h,
             // Engine.cpp) instead of the plain one -- a big hall that ducks while this voice plays and
             // is cut hard on the absolute bar line. Off by default, so older sets render unchanged.
             HallGate,
             /** @name The second oscillator (22.09.2026, round "Klangfarben")
              *  The user: "Eventuell sollte man bei Pads auch mindestens zwei Oszillatoren nehmen, um
              *  mehr Varianz zuzulassen." It costs no render pass: a voice already spreads over seven
              *  unison slots, each with its own frequency and its own source weights (Poly.h), so the
              *  second oscillator is the outermost pair of those slots given another source and another
              *  pitch. Off by default, so older sets render unchanged.
              *  @{ */
             Osc2,        ///< PolyOsc2: off, or one of the four oscillators, on the outer unison pair
             Osc2Mix,     ///< 0 .. 1: the share of the voice's power the second oscillator carries
             Osc2Interval,///< PolyOsc2Interval: its interval against the note, a *choice* so a recipe can override it
             Osc2Detune,  ///< -50 .. +50 cents on top, for the slow beating of two nearly equal pitches
             /** @} */
             /** @name The voice LFO (22.09.2026, round "Klangfarben")
              *  A second, tempo-synced LFO beside the position one, free-running for the whole instance
              *  rather than retriggered per note, so a held chord moves as one. The user: "Baue auch
              *  LFOs ein, das erhoeht die Variabilitaet zusaetzlich und kostet praktisch nichts" -- and
              *  it does not: the cutoff destination rides in lowPassCoefs(), which already runs once per
              *  16-sample grid, and the amplitude one is a multiply in a loop that exists.
              *  @{ */
             LfoBeats,    ///< its period in beats (tempo-synchronised, so it never drifts against the bar)
             LfoCutoff,   ///< +- octaves on the filter cutoff
             LfoPitch,    ///< +- cents on every oscillator of the voice (vibrato, or a slow warp)
             LfoAmp,      ///< 0 .. 1: tremolo depth
             /** @} */
             PlateSend,   ///< 25.09.2026, the mix guide: the send into the plate, the middle plane of depth (Engine.h)
             Distance,    ///< 25.09.2026, the addon: 0 near .. 1 far -- level, low pass, wet and width together (Engine.h)
             SlowMod,     ///< 25.09.2026, the addon: depth of the free slow movement (13 .. 34 s) of cutoff, colour, width
             Count };
}
/** @brief Values of poly.mod: the voice's modulation insert (20.09.2026, round "dialogue"). */
enum class PolyMod : int {
    Off = 0,   ///< no insert: the plain voice, sample for sample
    Flanger,   ///< a comb filter whose teeth slide (PsyFx.h, Flanger)
    Phaser,    ///< notches that slide (PsyFx.h, Phaser)
    Comb,      ///< the flanger with its sweep stopped: a *static* comb, the hollow metallic colour
    Count
};
/** @brief Values of poly.filter_type: the outputs of the voice's state-variable filter (PolyKernel.h). */
enum class PolyFilter : int { LowPass = 0, BandPass, HighPass, Notch, Count };
/** @brief Parameters of the effect generator (module Sfx, prefix "sfx"). */
namespace sfx {
enum : int { Level, Noise, Resonance, Brightness, ImpactDecay, Vowel, SwellDecay, Width, RoomSend, HallSend, Duck,
             // 19.09.2026, round "fx-psychedelia". Appended.
             SubLevel,     ///< dB: the sub drop against sfx.level (Sfx.h; it plays mono and ducks under the kick)
             SubDuck,      ///< 0..1: how deep the kick ducks the sub drop
             // 20.09.2026, round "wandering-fx" (Sfx.h): a directed pan trajectory plus a reverb-send
             // trajectory over an event's own length, drawn from its own seed. Off by default, so older
             // sets render unchanged.
             Wander,       ///< toggle: an event's pan sweeps from one side to the other and its content
                           ///< crosses from dry to the hall's send over its length, instead of the
                           ///< oscillating auto-pan and the constant hall_send fraction
             WanderSend,   ///< 0..1: how far the crossfade reaches by the event's own tail (x = 1)
             // 24.09.2026, the user: "Im SFX-Fenster ist nach wie vor keine Auswahl fuer das Preset". One per
             // family of the effect bank (Sfx.h, SfxPreset): 0 = Auto, the composer's draw per event as before;
             // n = every event of that family plays bank preset n (Sfx::trigger). Appended; Auto everywhere is
             // the composer's own draw, sample for sample.
             PresetRiser, PresetDownlifter, PresetImpact, PresetSweep, PresetFormantShot, PresetReverseSwell,
             PresetZap, PresetSquelch, PresetBubble, PresetReverseCrash, PresetAtmosphere,
             PlateSend,   ///< 25.09.2026: the effects' send into the plate (the throw rides it; the hall is the far room now)
             Count };
/** @brief The first of the per-family preset choices, and how many there are. */
constexpr int kFirstPreset = PresetRiser;
constexpr int kNumPresetChoices = PresetAtmosphere - PresetRiser + 1;
}
/** @brief Parameters of the shamanic bed (module Texture, prefix "texture"; Texture.h). */
namespace texture {
enum : int { Width, BowlDecay, BowlBright, DidgeFormant, DidgeBreath, JawSweep, RoomSend, HallSend, FxSend, Duck, Count };
}
/** @brief Parameters of the voices (module Vocal, prefix "vocal"; Vocal.h). */
namespace vocal {
enum : int { Pitch, Drive, ThrowSend, ThrowBeats, ThrowFeedback, FxSend, HallSend, Duck, Width, Count };
}
/** @brief Parameters of the modulation effects (module PsyFx, prefix "psyfx"; PsyFx.h). */
namespace psyfx {
enum : int { FlangerBeats, FlangerDepth, FlangerFeedback, FlangerMix, PhaserBeats, PhaserDepth, PhaserFeedback,
             PhaserMix, ShiftHz, ShiftMix, Return, Motion, Count };
}
/** @brief Parameters of the send effects (module Fx, prefix "fx"): a short room and a long hall. */
namespace fx {
enum : int { RoomSize, RoomDecay, RoomDamping, HallSize, HallDecay, HallDamping, HallPreDelay, LowCut, HighCut,
             RoomReturn, HallReturn, ReturnDuck,
             /** @name Appended 25.09.2026 (the mix guide): the returns' own duck release, and the plate -- the middle
              *  plane between the dry front (kick, bass, lead) and the hall at the back (Engine.h)
              *  @{ */
             ReturnDuckRelease, PlateSize, PlateDecay, PlateDamping, PlatePreDelay, PlateReturn,
             /** @} */
             /** @name Appended 25.09.2026 (the Dark-Ambient addon): the near room's pre-delay, a send filter per room,
              *  and the far room fed from the plate (Engine.h)
              *  @{ */
             RoomPreDelay, RoomLowCut, RoomHighCut, PlateLowCut, PlateHighCut, PlateToHall,
             /** @} */
             Count };
}
/** @brief Values of poly.osc. */
enum class PolyOsc : int { Supersaw = 0, Va, Fm, Wavetable, Count };
/** @brief Values of poly.osc2: the second oscillator, or none (22.09.2026, round "Klangfarben"). */
enum class PolyOsc2 : int { Off = 0, Supersaw, Va, Fm, Wavetable, Count };
/**
 * @brief Values of poly.osc2_interval: where the second oscillator sits against the note.
 *
 * A list and not a number of semitones, because only a discrete parameter can carry a
 * per-track override (Engine.cpp, applyParams: an Override is read for discrete curves only)
 * -- and because these six are the intervals that make a second oscillator worth having. A
 * unison is not a waste of one: with poly.osc2_detune it is the slow beat of two analogue
 * oscillators that never quite agree.
 */
enum class PolyOsc2Interval : int { TwoOctavesDown = 0, OctaveDown, FifthDown, Unison, FifthUp, OctaveUp, Count };
/** @brief The semitones each PolyOsc2Interval stands for. */
constexpr int kOsc2IntervalSemis[] = { -24, -12, -7, 0, 7, 12 };
/** @brief Delay times offered by the delay-time choices, in beats. */
inline constexpr float kDelayBeats[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
constexpr int kNumDelayTimes = 6;   ///< entries of kDelayBeats

/** @brief Values of bass.sub_mode. */
enum class SubMode : int { Mixed = 0, Split };
/** @brief Values of bass.kick_lock. */
enum class KickLock : int { Off = 0, BassFollowsKick, KickFollowsBass };
/** @brief Parameters of the mixer. */
namespace mix {
// 19.09.2026, round "voices": the strips follow the voices' groups (PolyInstance), so the counter-lead's
// strip stands beside the lead's, the stab's beside the arp's and the drone's beside the pad's. The keys
// ("mix.lead_level") did not change, only the indices -- see PolyInstance.
enum : int { KickMute, BassMute, TrackGain, PercMute, PercLevel, AcidMute, AcidLevel,
             LeadMute, LeadLevel, CounterMute, CounterLevel, ArpMute, ArpLevel, StabMute, StabLevel,
             PadMute, PadLevel, DroneMute, DroneLevel,
             SfxMute, SfxLevel, PercRoom, PercHall,
             DuckAttack, DuckHold, DuckRelease,
             // 19.09.2026, round "fx-psychedelia": the strips of the two new parts.
             TextureMute, TextureLevel, VocalMute, VocalLevel,
             // 23.09.2026, round "Presets": a synth whose sound is the user's -- a preset picked on its page, or knobs
             // set by hand -- and which the composer's per-track recipes leave alone (Engine.cpp, dispatchControl).
             KickOwn, BassOwn, AcidOwn, LeadOwn, CounterOwn, ArpOwn, StabOwn, PadOwn, DroneOwn,
             // 23.09.2026, round "Keyboard": which voice a MIDI keyboard plays, and whether it replaces that voice's
             // generated notes or plays over them (Engine.h, liveNoteOn).
             KeyboardPart, KeyboardMode,
             /** @name Appended 25.09.2026 (the mix guide): the lines' duck release (the pads and the bed keep
              *  duck_release), the counter's duck under the lead, and the pad's presence band under the lead
              *  @{ */
             DuckReleaseLines, CounterDuck, PadLeadDuck,
             /** @} */
             Count };
/** @brief The "own sound" switch of a polyphonic instance. */
constexpr int polyOwn(PolyInstance i) { return LeadOwn + static_cast<int>(i); }
static_assert(polyOwn(PolyInstance::Drone) == DroneOwn, "the own-sound switches follow PolyInstance");
/** @brief The mute of a polyphonic instance's strip. */
constexpr int polyMute(PolyInstance i) { return LeadMute + 2 * static_cast<int>(i); }
/** @brief The level of a polyphonic instance's strip. */
constexpr int polyLevel(PolyInstance i) { return LeadLevel + 2 * static_cast<int>(i); }
static_assert(polyMute(PolyInstance::Drone) == DroneMute && polyLevel(PolyInstance::Stab) == StabLevel,
              "the mix strips of the polyphonic instances must follow PolyInstance pair by pair");
}
/**
 * @brief Parameters of the cue bridge (module Cue, prefix "cue"; PLAN 8.3).
 *
 * Off by default: a generator that starts talking to the network because it was installed would be
 * a surprise, and a visualiser that is not there is the normal case. The destination *address* is
 * not here because a parameter is a float and an address is not; it lives in the plugin's state and
 * in the Quest's `phos.cfg`, next to the port.
 */
namespace cue {
enum : int { Send, Port, Beats, LeadMs, Count };
}
/** @brief Parameters of the master section. */
namespace master {
enum : int { Gain, Ceiling, Clip, CompThreshold, CompRatio, CompKnee, CompAttack, CompRelease, MonoBass,
             Limiter, LimiterRelease, TargetLufs, AutoGain, Clipper, ClipperThreshold,
             Monitor,   ///< 25.09.2026, the addon's monitoring: Normal, Mono, Sub (80 Hz low pass) or Side, after the meter
             Count };
}

extern const char* const kKeyNames[12];         ///< C, C#, ... B
extern const char* const kScaleNames[];         ///< names of compose.scale
extern const char* const kStyleNames[];         ///< names of compose.style (Form.h)
extern const char* const kArcNames[];           ///< names of compose.arc (Form.h)
extern const char* const kKickPatternNames[];   ///< names of compose.kick_pattern
extern const char* const kBassPatternNames[];   ///< names of compose.bass_pattern
extern const char* const kMelodyModelNames[];   ///< names of compose.melody_model (Model.h)
extern const char* const kBassModelNames[];     ///< names of compose.bass_model (Model.h)
extern const char* const kBassRhythmNames[];    ///< names of compose.bass_rhythm (Corpus.h)
extern const char* const kLeadDensityNames[];   ///< names of compose.lead_density (22.09.2026)
extern const char* const kPitchEntropyNames[];  ///< names of compose.pitch_entropy (22.09.2026)
extern const char* const kCounterModeNames[];   ///< names of compose.counter_mode (23.09.2026)

/**
 * @brief All parameter values of one engine, lock-free readable from the audio thread.
 *
 * Construction builds the registry from the module tables. Not copyable (atomics); use
 * copyValuesFrom() for snapshots.
 */
class ParamStore {
public:
    /** @brief Builds the registry from the module tables, every value at its default. */
    ParamStore();
    ParamStore(const ParamStore&) = delete;
    ParamStore& operator=(const ParamStore&) = delete;

    /** @brief Number of parameters. */
    int count() const { return static_cast<int>(entries_.size()); }
    /** @brief First id of a module instance; -1 if it does not exist. */
    int base(Module m, int instance = 0) const;
    /** @brief First id of a polyphonic instance: the one way to address a voice's parameters by name. */
    int base(PolyInstance i) const { return base(Module::Poly, polyIndex(i)); }
    /** @brief Descriptor of @p id. */
    const ParamDesc& desc(int id) const { return *entries_[static_cast<size_t>(id)].desc; }
    /** @brief Full text key of @p id ("kick.pitch_start"). */
    const std::string& key(int id) const { return entries_[static_cast<size_t>(id)].key; }
    /** @brief Id for a text key, or -1. */
    int find(std::string_view key) const;

    /** @brief Current real value. */
    float get(int id) const { return values_[static_cast<size_t>(id)].load(std::memory_order_relaxed); }
    /** @brief Current value rounded to an integer (Int, Choice, Toggle). */
    int getInt(int id) const;
    /** @brief Current value as a switch. */
    bool getBool(int id) const { return get(id) >= 0.5f; }
    /** @brief Sets a real value, clamped to the range (and rounded for discrete curves). */
    void set(int id, float value);
    /** @brief Sets from a normalised 0..1 position. */
    void setNormalised(int id, float norm) { set(id, fromNormalised(id, norm)); }

    /** @brief Real value to normalised 0..1. */
    float toNormalised(int id, float value) const;
    /** @brief Normalised 0..1 to real value. */
    float fromNormalised(int id, float norm) const;

    /** @brief All parameters back to their defaults. */
    void resetDefaults();
    /**
     * @brief Default of @p id: the descriptor's default, or the instance's own where a module's
     *        instances start from different values (the twelve lanes of the percussion kit).
     */
    float defaultValue(int id) const { return defaults_[static_cast<size_t>(id)]; }
    /** @brief Copies every value from another store (for snapshots on another thread). */
    void copyValuesFrom(const ParamStore& other);
    /**
     * @brief Copies the current values of one module instance into @p out, indexed like its table.
     * @param m        module
     * @param instance instance index
     * @param out      at least as many floats as the module has parameters
     */
    void readModule(Module m, int instance, float* out) const;
    /** @brief Number of parameters of a module. */
    static int moduleCount(Module m);

    /**
     * @brief Applies "key=value" assignments separated by whitespace, newlines or ';'.
     *
     * Choice parameters accept their name ("compose.key=F#") or index. Lines starting with '#' are
     * comments.
     * @param text  the assignments
     * @param error receives a message for the first bad assignment, may be null
     * @return false if any assignment failed (the good ones are still applied)
     */
    bool parseText(std::string_view text, std::string* error = nullptr);
    /**
     * @brief The text form, one "key=value" per line.
     * @param onlyChanged leave out parameters at their default
     */
    std::string toText(bool onlyChanged) const;
    /** @brief A value formatted for display ("330 Hz", "F#"). */
    std::string format(int id) const;

private:
    /** @brief One registered parameter: its descriptor, key, module and instance. */
    struct Entry {
        const ParamDesc* desc;   ///< the descriptor in the module table
        std::string key;   ///< "module.name"
        Module module;   ///< which module
        int instance;   ///< which instance of it
    };
    std::vector<Entry> entries_;   ///< every parameter, by global id
    std::unique_ptr<std::atomic<float>[]> values_;   ///< the values, lock-free for the audio thread
    std::vector<float> defaults_;   ///< the defaults, by global id
    std::unordered_map<std::string, int> index_;   ///< key -> global id
    static constexpr int kMaxInstances = 16;   ///< instances a module may have (base() table)
    int bases_[static_cast<int>(Module::Count)][kMaxInstances] = {};
};

} // namespace phos
