/**
 * @file Params.h
 * @brief The parameter system: descriptor tables per module, instantiated in blocks.
 *
 * Noctuary kept one flat enum of parameters. Phosphene has modules that exist several times -- twelve
 * percussion lanes, several polyphonic engines -- so parameters are declared once per module as a
 * table of descriptors, and a module may be instantiated more than once. A parameter's id is the
 * base index of its module instance plus its index in the table; its text key is
 * "\<prefix\>.\<key\>" or "\<prefix\>\<instance\>.\<key\>" ("kick.pitch_start", "perc3.decay").
 *
 * From the same tables come the host parameters, OSC addresses, the preset text form, the manual and
 * the `--list` output of phos_render. The composer never writes parameters; it reads a snapshot.
 *
 * Values are stored as std::atomic\<float\> in their real range (Hz, ms, dB), so the audio thread can
 * read what another thread wrote without a lock.
 * @author Rene Weller
 */
#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace phos
{

/** @brief How a parameter maps between its real value and the normalised 0..1 of a knob. */
enum class CurveE : uint8_t
{
    LINEAR,   ///< proportional
    LOG,      ///< logarithmic; minimum must be > 0
    INT,      ///< integer steps, linear
    CHOICE,   ///< integer index into a list of names
    TOGGLE,   ///< 0 or 1
};

/** @brief Static description of one parameter. */
struct ParamDesc
{
    const char        *m_key;                 ///< identifier inside the module, snake_case
    const char        *m_name;                ///< display name (English)
    const char        *m_unit;                ///< unit for display, may be empty
    float              m_minValue;            ///< lowest real value
    float              m_maxValue;            ///< highest real value
    float              m_defValue;            ///< default real value
    CurveE             m_curve;               ///< mapping to the knob
    const char *const *m_choices = nullptr;   ///< names for Curve::Choice (maxValue + 1 entries)
};

/** @brief The modules that own parameters. */
enum class ModuleE : int
{
    COMPOSE = 0,
    KICK,
    BASS,
    PERC,
    ACID,
    POLY,
    SFX,
    FX,
    MIX,
    MASTER,
    /** The cue bridge of PLAN 8.3; appended, so no existing id moved. */
    CUE,
    /** 19.09.2026, round "fx-psychedelia": the shamanic bed (Texture.h), the voices
                           *  (Vocal.h) and the modulation effects of the SFX bus (PsyFx.h). Appended. */
    TEXTURE,
    VOCAL,
    PSY_FX,
    /** 27.09.2026: the Field track's sampler (FieldPlayer.h). Appended. */
    FIELD,
    COUNT
};

constexpr int PercLanes = 12;   ///< instances of the percussion lane module
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
enum class PolyInstanceE : int
{
    LEAD = 0,
    COUNTER,
    ARP,
    STAB,
    PAD,
    DRONE,
    COUNT
};
constexpr int PolyInstances = static_cast<int>( PolyInstanceE::COUNT );   ///< instances of the polyphonic engine module
/**
 * @brief The index of an instance, for arrays kept per instance.
 * @param[in] i  the instance
 * @return its index
 */
constexpr int polyIndex( PolyInstanceE i )
{
    return static_cast<int>( i );
}
extern const char *const PolyInstanceNames[PolyInstances];   ///< "lead", "counter", "arp", "stab", "pad", "drone"

/** @brief Parameters of the composer (read as a snapshot when bars are composed). */
namespace compose
{
enum : int
{
    BPM,
    KEY,
    SCALE,
    KICK_PATTERN,
    BASS_PATTERN,
    BASS_GATE,
    BASS_VARIATION,
    BASS_REGISTER,
    TRACK_BARS,
    TRACK_VARIATION,
    SOUND_VARIATION,
    TEMPO_RANGE,
    LEVEL_MATCH,
    PERC_DENSITY,
    PERC_VARIATION,
    SWING,
    ACID_AMOUNT,
    LEAD_AMOUNT,
    ARP_AMOUNT,
    MELODY_VARIATION,
    MELODY_TEMPERATURE,
    SQUELCH_CHANCE,
    BASS_FOLLOWS_CHORDS,
    PAD_AMOUNT,
    SFX_AMOUNT,
    GATE_CHANCE,
    STYLE,
    ARC,
    STYLE_TEMPO,
    SET_MINUTES,
    // Phase 8: which predictive model the melodic lines are drawn from (Model.h).
    MELODY_MODEL,
    // Phase 8, bass round: where the pitch of each bass note comes from -- the pattern
    // families of Patterns.h with their phrase figures, or the learned fourth role (Model.h).
    BASS_MODEL,
    // 16.09.2026: modal interchange over the tonic pedal (Form.h). Off reproduces every
    // note the program played before that date, which is what the self test measures against.
    MODAL_INTERCHANGE,
    // 16.09.2026, bass rhythm round: where the *onset pattern* of a bar comes from -- the
    // five pattern families of Patterns.h, or a bar drawn from the corpus onset model
    // (Corpus.h, BassRhythm). Pattern is the default and reproduces every note bit for bit.
    BASS_RHYTHM,
    // 19.09.2026, round "voices": how often a track has each of the three new voices, and the
    // density of the voices (speech, chants) and of the shamanic bed (FormSfx.cpp, placePsychedelia).
    // Appended: the compose block is not part of the reordering, which concerns the voices only.
    COUNTER_AMOUNT,
    STAB_AMOUNT,
    DRONE_AMOUNT,
    VOICE_DENSITY,
    BED_DENSITY,
    // 19.09.2026, round "polish": the presence match -- each track's lines (lead, counter, arp, stab)
    // brought to a band around the reference median's presence (ComposerLevels.cpp, matchPresence).
    // 22.09.2026: the styles walk through the night instead of one style for all of it
    // (Composer.cpp, walkAt). On by default; off, every track is compose.style as before.
    STYLE_MIX,
    PRESENCE_MATCH,
    // 22.09.2026, round "Lead": the lead's density and the pitch entropy of the lines, each a
    // choice whose first entry is "Auto (Style)" -- the style profile's vector decides (Form.h,
    // LeadStyle) unless the user takes the knob. Appended, never reordered.
    LEAD_DENSITY,
    PITCH_ENTROPY,
    // 23.09.2026, round "Counter": how the counter-lead answers (Form.h, CounterMode); "Auto (Style)" first.
    COUNTER_MODE,
    // 23.09.2026, round "Hoerbarkeit": the quiet lines lifted to a share of the lead's partial loudness
    // (ComposerLevels.cpp, matchAudibility). Appended, in the same place in the table.
    AUDIBILITY_MATCH,
    // 26.09.2026: the track's sounds come from the preset bank (on) or from the older recipes (off; Composer::presetOf).
    SOUND_PRESETS,
    // 27.09.2026: how often the Field track plays (FormSfx.cpp, placeField); 1 the style's own density.
    FIELD_DENSITY,
    COUNT
};
}
/** @brief Parameters of one percussion lane (module Perc, twelve instances "perc1" .. "perc12"). */
namespace perc
{
enum : int
{
    ACTIVE,
    ROLE,
    ENGINE,
    PITCH,
    PITCH_AMOUNT,
    PITCH_DECAY,
    FM_RATIO,
    FM_INDEX,
    MODE_SET,
    MODE_DAMP,
    METAL_SCALE,
    NOISE,
    NOISE_DECAY,
    BURSTS,
    BURST_SPACING,
    DECAY,
    FILTER,
    CUTOFF,
    RESONANCE,
    LOW_CUT,
    DRIVE,
    LEVEL,
    PAN,
    CHOKE,
    SHIFT,
    DENSITY,
    TUNE,
    // 16.09.2026, arrangement dynamics. Appended, never reordered: the indices above sit in
    // saved presets, in .phosset files and in the plugin's state.
    PAN_DEPTH,   ///< 0..1: how far the tempo-synchronous auto-pan swings the lane (Perc.h)
    PAN_BARS,    ///< the period of that swing in bars (PercKit::setTempo; 145 BPM until it is called)
    CUT_TRACK,   ///< 0..2: the exponent with which the low cut follows a hit's pitch shift (Perc.h)
    COUNT
};
}
/** @brief What a percussion lane plays in the groove; decides its patterns and its MIDI note. */
enum class PercRoleE : int
{
    CLOSED_HAT = 0,
    OPEN_HAT,
    RIDE,
    CRASH,
    CLAP,
    SNARE,
    RIM,
    SHAKER,
    TOM,
    CONGA,
    ZAP,
    BLIP,
    COUNT
};
constexpr int NumPercRoles = static_cast<int>( PercRoleE::COUNT );   ///< number of roles
/** @brief Sound sources of a percussion lane. */
enum class PercEngineE : int
{
    NOISE = 0,
    METAL,
    MODAL,
    TONE,
    FM,
    COUNT
};
extern const char *const PercRoleNames[NumPercRoles];   ///< display names of perc.role
/** @brief Parameters of the kick drum. */
namespace kick
{
enum : int
{
    ENGINE,
    TUNE,
    PITCH_END,
    PITCH_START,
    PITCH_DECAY,
    PUNCH_DECAY,
    PUNCH,
    AMP_ATTACK,
    AMP_HOLD,
    AMP_DECAY,
    DRIVE,
    CLIP,
    CLICK_LEVEL,
    CLICK_TONE,
    CLICK_DECAY,
    TONE,
    LEVEL,
    TAIL_LIMIT,
    COUNT
};
}
/** @brief Parameters of the bass. */
namespace bass
{
enum : int
{
    WAVE,
    PULSE_WIDTH,
    SUB,
    SUB_MODE,
    SPLIT_RATIO,
    KICK_LOCK,
    RETRIGGER,
    START_PHASE,
    CUTOFF,
    RESONANCE,
    ENV_AMOUNT,
    FILTER_DECAY,
    KEY_TRACK,
    VEL_TO_CUTOFF,
    DRIVE,
    AMP_ATTACK,
    AMP_DECAY,
    AMP_SUSTAIN,
    AMP_RELEASE,
    DUCK_DEPTH,
    DUCK_HOLD,
    DUCK_RELEASE,
    LEVEL,
    // Appended 19.09.2026 (round "lowend-acid"): the bite layer (Bass.h). New entries go at
    // the end: stored sets, presets and plugin state refer to a parameter by its index.
    BITE,
    BITE_CUTOFF,
    BITE_ENV,
    BITE_DECAY,
    BITE_DRIVE,
    BITE_RESONANCE,
    SUB_OCTAVE,
    // 26.09.2026: the filter models (Filters.h); 0 is the bass's own ladder (Ladder.h), as before.
    FILTER_MODEL,
    FILTER_MODE,
    // 26.09.2026: the modulation block (Modulation.h, ModBlockSize knobs), with the bass's own targets
    // (BassModDestNames: no pitch -- the kick lock sits on it -- and no pan).
    FILT_ATTACK,
    FILT_SUSTAIN,
    FILT_RELEASE,
    MENV_ATTACK,
    MENV_DECAY,
    MENV_SUSTAIN,
    MENV_RELEASE,
    LFO1_RATE,
    LFO1_SHAPE,
    LFO1_SYNC,
    LFO1_RETRIG,
    LFO1_FADE,
    LFO2_RATE,
    LFO2_SHAPE,
    LFO2_SYNC,
    LFO2_RETRIG,
    LFO2_FADE,
    LFO3_RATE,
    LFO3_SHAPE,
    LFO3_SYNC,
    LFO3_RETRIG,
    LFO3_FADE,
    LFO4_RATE,
    LFO4_SHAPE,
    LFO4_SYNC,
    LFO4_RETRIG,
    LFO4_FADE,
    MX1_SRC,
    MX1_DST,
    MX1_AMOUNT,
    MX2_SRC,
    MX2_DST,
    MX2_AMOUNT,
    MX3_SRC,
    MX3_DST,
    MX3_AMOUNT,
    MX4_SRC,
    MX4_DST,
    MX4_AMOUNT,
    MX5_SRC,
    MX5_DST,
    MX5_AMOUNT,
    MX6_SRC,
    MX6_DST,
    MX6_AMOUNT,
    MX7_SRC,
    MX7_DST,
    MX7_AMOUNT,
    MX8_SRC,
    MX8_DST,
    MX8_AMOUNT,
    COUNT
};
}
/** @brief Parameters of the acid voice (module Acid, prefix "acid"). */
namespace acid
{
enum : int
{
    WAVE,
    CUTOFF,
    RESONANCE,
    ENV_AMOUNT,
    DECAY,
    ACCENT,
    SLIDE_TIME,
    AMP_DECAY,
    KEY_TRACK,
    DRIVE,
    SQUELCH,
    SQUELCH_START,
    SQUELCH_TIME,
    COMB_MIX,
    COMB_FEEDBACK,
    LOW_CUT,
    DELAY_SEND,
    DELAY_LEFT,
    DELAY_RIGHT,
    DELAY_FEEDBACK,
    DELAY_HIGH_PASS,
    DELAY_LOW_PASS,
    ROOM_SEND,
    HALL_SEND,
    DUCK,
    LEVEL,
    // Appended 16.09.2026 (acid colour round). New entries go at the end: the index of a
    // parameter is its position in this list and every stored set, preset and automation
    // slot refers to it by that index.
    DISPERSE,
    DISPERSE_FREQ,
    // Appended 20.09.2026 (round "reverb"): routes hall_send into the gated hall (Reverb.h,
    // Engine.cpp) instead of the plain one -- a big hall that ducks while this voice plays and
    // is cut hard on the absolute bar line. Off by default, so older sets render unchanged.
    HALL_GATE,
    PLATE_SEND,   ///< 25.09.2026, the mix guide: the send into the plate, the middle plane of depth (Engine.h)
    // 26.09.2026: the modulation block (Modulation.h) without the filter envelope -- the acid keeps its 303
    // envelope -- and with the acid's own targets (AcidModDestNames).
    MENV_ATTACK,
    MENV_DECAY,
    MENV_SUSTAIN,
    MENV_RELEASE,
    LFO1_RATE,
    LFO1_SHAPE,
    LFO1_SYNC,
    LFO1_RETRIG,
    LFO1_FADE,
    LFO2_RATE,
    LFO2_SHAPE,
    LFO2_SYNC,
    LFO2_RETRIG,
    LFO2_FADE,
    LFO3_RATE,
    LFO3_SHAPE,
    LFO3_SYNC,
    LFO3_RETRIG,
    LFO3_FADE,
    LFO4_RATE,
    LFO4_SHAPE,
    LFO4_SYNC,
    LFO4_RETRIG,
    LFO4_FADE,
    MX1_SRC,
    MX1_DST,
    MX1_AMOUNT,
    MX2_SRC,
    MX2_DST,
    MX2_AMOUNT,
    MX3_SRC,
    MX3_DST,
    MX3_AMOUNT,
    MX4_SRC,
    MX4_DST,
    MX4_AMOUNT,
    MX5_SRC,
    MX5_DST,
    MX5_AMOUNT,
    MX6_SRC,
    MX6_DST,
    MX6_AMOUNT,
    MX7_SRC,
    MX7_DST,
    MX7_AMOUNT,
    MX8_SRC,
    MX8_DST,
    MX8_AMOUNT,
    COUNT
};
}
/** @brief Parameters of a polyphonic engine (module Poly, the six instances of PolyInstance). */
namespace poly
{
enum : int
{
    OSC,
    DETUNE,
    MIX,
    DYNAMIC_DETUNE,
    WAVE,
    PULSE_WIDTH,
    FM_RATIO,
    FM_INDEX,
    FM_DECAY,
    TABLE,
    POSITION,
    POS_ENV,
    POS_DECAY,
    POS_LFO_DEPTH,
    POS_LFO_BEATS,
    CUTOFF,
    RESONANCE,
    ENV_AMOUNT,
    FILTER_DECAY,
    KEY_TRACK,
    HP_FLOOR,
    HP_TRACK,
    AMP_ATTACK,
    AMP_DECAY,
    AMP_SUSTAIN,
    AMP_RELEASE,
    WIDTH,
    VEL_SENS,
    DELAY_SEND,
    DELAY_LEFT,
    DELAY_RIGHT,
    DELAY_FEEDBACK,
    DELAY_HIGH_PASS,
    DELAY_LOW_PASS,
    ROOM_SEND,
    HALL_SEND,
    DUCK,
    GATE,
    GATE_PATTERN,
    GATE_DEPTH,
    GATE_DUTY,
    GATE_ATTACK,
    GATE_RELEASE,
    GATE_TONE,
    LEVEL,
    // Appended 16.09.2026 (acid colour round), at the end for the same reason as above.
    DISPERSE,
    DISPERSE_FREQ,
    DRIFT,
    // Appended 19.09.2026 (round "voices"): the voice filter's response (PolyFilter) -- the
    // per-track recipes of every voice may choose a band pass or a notch instead of the low pass.
    FILTER_TYPE,
    // Appended 20.09.2026 (round "dialogue"), for the user's articulation and stereo rules:
    GLIDE,   ///< ms: the time constant of the portamento between two notes of this voice (Poly.h)
    PAN,     ///< -1 .. 1: where the voice's dry signal stands (its delay keeps its own image)
    /** @name The voice's own modulation insert (Engine.h; PsyFx.h supplies the two effects)
              *  The user's effect list asks for "Kammfilter / Flanger / Phaser als Klangfarbe auf Lead,
              *  Counter und Arp -- nicht nur auf dem FX-Bus": the alien, hollow character, tempo-synced.
              *  On the effects strip alone they would colour every voice at once.
              *  @{ */
    MOD,            ///< PolyMod: off, flanger, phaser or a static comb
    MOD_BEATS,      ///< the sweep's period in beats (tempo-synchronised, so it never drifts)
    MOD_DEPTH,      ///< 0 .. 1: how far the comb's teeth or the phaser's notches travel
    MOD_FEEDBACK,   ///< -0.9 .. 0.9: how sharp they are
    MOD_MIX,        ///< 0 .. 1: how much of the voice goes through it
    /** @} */
    // Appended 20.09.2026 (round "reverb"): routes hall_send into the gated hall (Reverb.h,
    // Engine.cpp) instead of the plain one -- a big hall that ducks while this voice plays and
    // is cut hard on the absolute bar line. Off by default, so older sets render unchanged.
    HALL_GATE,
    /** @name The second oscillator (22.09.2026, round "Klangfarben")
              *  The user: "Eventuell sollte man bei Pads auch mindestens zwei Oszillatoren nehmen, um
              *  mehr Varianz zuzulassen." It costs no render pass: a voice already spreads over seven
              *  unison slots, each with its own frequency and its own source weights (Poly.h), so the
              *  second oscillator is the outermost pair of those slots given another source and another
              *  pitch. Off by default, so older sets render unchanged.
              *  @{ */
    OSC2,            ///< PolyOsc2: off, or one of the four oscillators, on the outer unison pair
    OSC2_MIX,        ///< 0 .. 1: the share of the voice's power the second oscillator carries
    OSC2_INTERVAL,   ///< PolyOsc2Interval: its interval against the note, a *choice* so a recipe can override it
    OSC2_DETUNE,     ///< -50 .. +50 cents on top, for the slow beating of two nearly equal pitches
    /** @} */
    /** @name The voice LFO (22.09.2026, round "Klangfarben")
              *  A second, tempo-synced LFO beside the position one, free-running for the whole instance
              *  rather than retriggered per note, so a held chord moves as one. The user: "Baue auch
              *  LFOs ein, das erhoeht die Variabilitaet zusaetzlich und kostet praktisch nichts" -- and
              *  it does not: the cutoff destination rides in lowPassCoefs(), which already runs once per
              *  16-sample grid, and the amplitude one is a multiply in a loop that exists.
              *  @{ */
    LFO_BEATS,    ///< its period in beats (tempo-synchronised, so it never drifts against the bar)
    LFO_CUTOFF,   ///< +- octaves on the filter cutoff
    LFO_PITCH,    ///< +- cents on every oscillator of the voice (vibrato, or a slow warp)
    LFO_AMP,      ///< 0 .. 1: tremolo depth
    /** @} */
    PLATE_SEND,   ///< 25.09.2026, the mix guide: the send into the plate, the middle plane of depth (Engine.h)
    DISTANCE,     ///< 25.09.2026, the addon: 0 near .. 1 far -- level, low pass, wet and width together (Engine.h)
    SLOW_MOD,     ///< 25.09.2026, the addon: depth of the free slow movement (13 .. 34 s) of cutoff, colour, width
    // 26.09.2026, the filter models of the BerlinSchoolGenerator (Filters.h): which circuit the voice filter is
    // (0 = the state-variable filter it always had, so older sets render unchanged) and its mode -- the SEM's
    // morph from low pass through notch to high pass, the Xpander's eight pole mixes, the Polivoks' band pass.
    FILTER_MODEL,
    FILTER_MODE,
    // 26.09.2026, the voice's own modulation (Modulation.h): the filter envelope's attack, sustain and release (its
    // decay is FilterDecay; attack at its minimum and sustain 0 keep the exponential decay the voice always had),
    // the modulation envelope, four LFOs and the eight slots of the matrix.
    FILT_ATTACK,
    FILT_SUSTAIN,
    FILT_RELEASE,
    MENV_ATTACK,
    MENV_DECAY,
    MENV_SUSTAIN,
    MENV_RELEASE,
    LFO1_RATE,
    LFO1_SHAPE,
    LFO1_SYNC,
    LFO1_RETRIG,
    LFO1_FADE,
    LFO2_RATE,
    LFO2_SHAPE,
    LFO2_SYNC,
    LFO2_RETRIG,
    LFO2_FADE,
    LFO3_RATE,
    LFO3_SHAPE,
    LFO3_SYNC,
    LFO3_RETRIG,
    LFO3_FADE,
    LFO4_RATE,
    LFO4_SHAPE,
    LFO4_SYNC,
    LFO4_RETRIG,
    LFO4_FADE,
    MX1_SRC,
    MX1_DST,
    MX1_AMOUNT,
    MX2_SRC,
    MX2_DST,
    MX2_AMOUNT,
    MX3_SRC,
    MX3_DST,
    MX3_AMOUNT,
    MX4_SRC,
    MX4_DST,
    MX4_AMOUNT,
    MX5_SRC,
    MX5_DST,
    MX5_AMOUNT,
    MX6_SRC,
    MX6_DST,
    MX6_AMOUNT,
    MX7_SRC,
    MX7_DST,
    MX7_AMOUNT,
    MX8_SRC,
    MX8_DST,
    MX8_AMOUNT,
    COUNT
};
}
/** @brief Values of poly.mod: the voice's modulation insert (20.09.2026, round "dialogue"). */
enum class PolyModE : int
{
    OFF = 0,   ///< no insert: the plain voice, sample for sample
    FLANGER,   ///< a comb filter whose teeth slide (PsyFx.h, Flanger)
    PHASER,    ///< notches that slide (PsyFx.h, Phaser)
    COMB,      ///< the flanger with its sweep stopped: a *static* comb, the hollow metallic colour
    COUNT
};
/** @brief Values of poly.filter_type: the outputs of the voice's state-variable filter (PolyKernel.h). */
enum class PolyFilterE : int
{
    LOW_PASS = 0,
    BAND_PASS,
    HIGH_PASS,
    NOTCH,
    COUNT
};
// Names of the modulation block's choices (Modulation.h; Params.cpp), 26.09.2026.
extern const char *const LfoShapeNames[];                ///< LfoShape
extern const char *const LfoSyncNames[];                 ///< lfoCyclesPerBeat's divisions
extern const char *const ModSourceNames[];               ///< ModSource
extern const char *const ModDestNames[];                 ///< ModDest
extern const char *const BassModDestNames[];             ///< the bass's targets (BassModDests maps them to ModDest)
constexpr int            BassModDests = 6;               ///< entries of BassModDestNames
extern const int         BassModDestMap[BassModDests];   ///< BassModDestNames -> ModDest
extern const char *const AcidModDestNames[];             ///< the acid's targets
constexpr int            AcidModDests = 6;               ///< entries of AcidModDestNames
extern const int         AcidModDestMap[AcidModDests];   ///< AcidModDestNames -> ModDest
/**
 * @brief Values of poly.filter_model and bass.filter_model (26.09.2026): entry 0 is the synth's own filter (the
 *        state-variable filter of a voice, the ladder of the bass), entries 1 .. 9 are Filters.h's FilterModel from
 *        Moog to Wasp (the comb is left out: the voices have one in their modulation insert).
 */
constexpr int VoiceFilterModels =
    10;   ///< entries of the two lists (Params.cpp PolyFilterModelNames, BassFilterModelNames)
/** @brief Parameters of the effect generator (module Sfx, prefix "sfx"). */
namespace sfx
{
enum : int
{
    LEVEL,
    NOISE,
    RESONANCE,
    BRIGHTNESS,
    IMPACT_DECAY,
    VOWEL,
    SWELL_DECAY,
    WIDTH,
    ROOM_SEND,
    HALL_SEND,
    DUCK,
    // 19.09.2026, round "fx-psychedelia". Appended.
    SUB_LEVEL,   ///< dB: the sub drop against sfx.level (Sfx.h; it plays mono and ducks under the kick)
    SUB_DUCK,    ///< 0..1: how deep the kick ducks the sub drop
    // 20.09.2026, round "wandering-fx" (Sfx.h): a directed pan trajectory plus a reverb-send
    // trajectory over an event's own length, drawn from its own seed. Off by default, so older
    // sets render unchanged.
    WANDER,        ///< toggle: an event's pan sweeps from one side to the other and its content
                   ///< crosses from dry to the hall's send over its length, instead of the
                   ///< oscillating auto-pan and the constant hall_send fraction
    WANDER_SEND,   ///< 0..1: how far the crossfade reaches by the event's own tail (x = 1)
    // 24.09.2026, the user: "Im SFX-Fenster ist nach wie vor keine Auswahl fuer das Preset". One per
    // family of the effect bank (Sfx.h, SfxPreset): 0 = Auto, the composer's draw per event as before;
    // n = every event of that family plays bank preset n (Sfx::trigger). Appended; Auto everywhere is
    // the composer's own draw, sample for sample.
    PRESET_RISER,
    PRESET_DOWNLIFTER,
    PRESET_IMPACT,
    PRESET_SWEEP,
    PRESET_FORMANT_SHOT,
    PRESET_REVERSE_SWELL,
    PRESET_ZAP,
    PRESET_SQUELCH,
    PRESET_BUBBLE,
    PRESET_REVERSE_CRASH,
    PRESET_ATMOSPHERE,
    PLATE_SEND,   ///< 25.09.2026: the effects' send into the plate (the throw rides it; the hall is the far room now)
    COUNT
};
/** @brief The first of the per-family preset choices, and how many there are. */
constexpr int FirstPreset = PRESET_RISER;
constexpr int NumPresetChoices =
    PRESET_ATMOSPHERE - PRESET_RISER + 1;   ///< how many per-family preset choices there are
}   // namespace sfx
/** @brief Parameters of the shamanic bed (module Texture, prefix "texture"; Texture.h). */
namespace texture
{
enum : int
{
    WIDTH,
    BOWL_DECAY,
    BOWL_BRIGHT,
    DIDGE_FORMANT,
    DIDGE_BREATH,
    JAW_SWEEP,
    ROOM_SEND,
    HALL_SEND,
    FX_SEND,
    DUCK,
    COUNT
};
}
/** @brief Parameters of the voices (module Vocal, prefix "vocal"; Vocal.h). */
namespace vocal
{
enum : int
{
    PITCH,
    DRIVE,
    THROW_SEND,
    THROW_BEATS,
    THROW_FEEDBACK,
    FX_SEND,
    HALL_SEND,
    DUCK,
    WIDTH,
    COUNT
};
}
/**
 * @brief Parameters of the Field track's sampler (module Field, prefix "field"; FieldPlayer.h, 27.09.2026).
 *
 * Two layers, A and B, each a recording of the field library (a category and a variation within it, "Auto" for the
 * composer's choice), with its own level, pitch, start and direction; a loop over any stretch of the recording with a
 * crossfade of its own length, so a recording that is not a loop plays as one; amp and filter envelopes, the voices'
 * filters (the state-variable filter or the nine circuit models) and their modulation block -- the same controls as a
 * synth's, from filt_attack to mx8_amount.
 */
namespace field
{
enum : int
{
    A_CATEGORY,
    A_VARIATION,
    A_LEVEL,
    A_PITCH,
    A_FINE,
    A_START,
    A_START_RANDOM,
    A_REVERSE,
    B_ON,
    B_CATEGORY,
    B_VARIATION,
    B_LEVEL,
    B_PITCH,
    B_FINE,
    B_START,
    B_START_RANDOM,
    B_REVERSE,
    LAYER_MIX,
    LOOP,
    LOOP_START,
    LOOP_END,
    LOOP_XFADE,
    AMP_ATTACK,
    AMP_DECAY,
    AMP_SUSTAIN,
    AMP_RELEASE,
    FILTER_MODEL,
    FILTER_TYPE,
    CUTOFF,
    RESONANCE,
    FILTER_MODE,
    ENV_AMOUNT,
    FILTER_DECAY,
    LOW_CUT,
    FILT_ATTACK,
    FILT_SUSTAIN,
    FILT_RELEASE,
    MENV_ATTACK,
    MENV_DECAY,
    MENV_SUSTAIN,
    MENV_RELEASE,
    LFO1_RATE,
    LFO1_SHAPE,
    LFO1_SYNC,
    LFO1_RETRIG,
    LFO1_FADE,
    LFO2_RATE,
    LFO2_SHAPE,
    LFO2_SYNC,
    LFO2_RETRIG,
    LFO2_FADE,
    LFO3_RATE,
    LFO3_SHAPE,
    LFO3_SYNC,
    LFO3_RETRIG,
    LFO3_FADE,
    LFO4_RATE,
    LFO4_SHAPE,
    LFO4_SYNC,
    LFO4_RETRIG,
    LFO4_FADE,
    MX1_SRC,
    MX1_DST,
    MX1_AMOUNT,
    MX2_SRC,
    MX2_DST,
    MX2_AMOUNT,
    MX3_SRC,
    MX3_DST,
    MX3_AMOUNT,
    MX4_SRC,
    MX4_DST,
    MX4_AMOUNT,
    MX5_SRC,
    MX5_DST,
    MX5_AMOUNT,
    MX6_SRC,
    MX6_DST,
    MX6_AMOUNT,
    MX7_SRC,
    MX7_DST,
    MX7_AMOUNT,
    MX8_SRC,
    MX8_DST,
    MX8_AMOUNT,
    WIDTH,
    PAN,
    ROOM_SEND,
    PLATE_SEND,
    HALL_SEND,
    DUCK,
    COUNT
};
/** @brief The categories of the field library, in the order of the category choice (index 0 is "Auto"). */
constexpr int Categories = 31;
}   // namespace field
extern const char *const FieldCategoryNames[field::Categories + 1];   ///< "Auto", then the categories' names
extern const char *const
    FieldCategorySlugs[field::Categories];       ///< the file names' category tokens ("rainforest", ...)
extern const char *const FieldModDestNames[7];   ///< the matrix's targets on the Field track
extern const int         FieldModDestMap[7];     ///< and the ModDest each one is

/** @brief Parameters of the modulation effects (module PsyFx, prefix "psyfx"; PsyFx.h). */
namespace psyfx
{
enum : int
{
    FLANGER_BEATS,
    FLANGER_DEPTH,
    FLANGER_FEEDBACK,
    FLANGER_MIX,
    PHASER_BEATS,
    PHASER_DEPTH,
    PHASER_FEEDBACK,
    PHASER_MIX,
    SHIFT_HZ,
    SHIFT_MIX,
    RETURN,
    MOTION,
    COUNT
};
}
/** @brief Parameters of the send effects (module Fx, prefix "fx"): a short room and a long hall. */
namespace fx
{
enum : int
{
    ROOM_SIZE,
    ROOM_DECAY,
    ROOM_DAMPING,
    HALL_SIZE,
    HALL_DECAY,
    HALL_DAMPING,
    HALL_PRE_DELAY,
    LOW_CUT,
    HIGH_CUT,
    ROOM_RETURN,
    HALL_RETURN,
    RETURN_DUCK,
    /** @name Appended 25.09.2026 (the mix guide): the returns' own duck release, and the plate -- the middle
              *  plane between the dry front (kick, bass, lead) and the hall at the back (Engine.h)
              *  @{ */
    RETURN_DUCK_RELEASE,
    PLATE_SIZE,
    PLATE_DECAY,
    PLATE_DAMPING,
    PLATE_PRE_DELAY,
    PLATE_RETURN,
    /** @} */
    /** @name Appended 25.09.2026 (the Dark-Ambient addon): the near room's pre-delay, a send filter per room,
              *  and the far room fed from the plate (Engine.h)
              *  @{ */
    ROOM_PRE_DELAY,
    ROOM_LOW_CUT,
    ROOM_HIGH_CUT,
    PLATE_LOW_CUT,
    PLATE_HIGH_CUT,
    PLATE_TO_HALL,
    /** @} */
    COUNT
};
}
/** @brief Values of poly.osc. */
enum class PolyOscE : int
{
    SUPERSAW = 0,
    VA,
    FM,
    WAVETABLE,
    COUNT
};
/** @brief Values of poly.osc2: the second oscillator, or none (22.09.2026, round "Klangfarben"). */
enum class PolyOsc2E : int
{
    OFF = 0,
    SUPERSAW,
    VA,
    FM,
    WAVETABLE,
    COUNT
};
/**
 * @brief Values of poly.osc2_interval: where the second oscillator sits against the note.
 *
 * A list and not a number of semitones, because only a discrete parameter can carry a
 * per-track override (Engine.cpp, applyParams: an Override is read for discrete curves only)
 * -- and because these six are the intervals that make a second oscillator worth having. A
 * unison is not a waste of one: with poly.osc2_detune it is the slow beat of two analogue
 * oscillators that never quite agree.
 */
enum class PolyOsc2IntervalE : int
{
    TWO_OCTAVES_DOWN = 0,
    OCTAVE_DOWN,
    FIFTH_DOWN,
    UNISON,
    FIFTH_UP,
    OCTAVE_UP,
    COUNT
};
/** @brief The semitones each PolyOsc2Interval stands for. */
constexpr int Osc2IntervalSemis[] = { -24, -12, -7, 0, 7, 12 };
/** @brief Delay times offered by the delay-time choices, in beats. */
inline constexpr float DelayBeats[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
constexpr int          NumDelayTimes = 6;   ///< entries of DelayBeats

/** @brief Values of bass.sub_mode. */
enum class SubModeE : int
{
    MIXED = 0,
    SPLIT
};
/** @brief Values of bass.kick_lock. */
enum class KickLockE : int
{
    OFF = 0,
    BASS_FOLLOWS_KICK,
    KICK_FOLLOWS_BASS
};
/** @brief Parameters of the mixer. */
namespace mix
{
// 19.09.2026, round "voices": the strips follow the voices' groups (PolyInstance), so the counter-lead's
// strip stands beside the lead's, the stab's beside the arp's and the drone's beside the pad's. The keys
// ("mix.lead_level") did not change, only the indices -- see PolyInstance.
enum : int
{
    KICK_MUTE,
    BASS_MUTE,
    TRACK_GAIN,
    PERC_MUTE,
    PERC_LEVEL,
    ACID_MUTE,
    ACID_LEVEL,
    LEAD_MUTE,
    LEAD_LEVEL,
    COUNTER_MUTE,
    COUNTER_LEVEL,
    ARP_MUTE,
    ARP_LEVEL,
    STAB_MUTE,
    STAB_LEVEL,
    PAD_MUTE,
    PAD_LEVEL,
    DRONE_MUTE,
    DRONE_LEVEL,
    SFX_MUTE,
    SFX_LEVEL,
    PERC_ROOM,
    PERC_HALL,
    DUCK_ATTACK,
    DUCK_HOLD,
    DUCK_RELEASE,
    // 19.09.2026, round "fx-psychedelia": the strips of the two new parts.
    TEXTURE_MUTE,
    TEXTURE_LEVEL,
    VOCAL_MUTE,
    VOCAL_LEVEL,
    // 23.09.2026, round "Presets": a synth whose sound is the user's -- a preset picked on its page, or knobs
    // set by hand -- and which the composer's per-track recipes leave alone (Engine.cpp, dispatchControl).
    KICK_OWN,
    BASS_OWN,
    ACID_OWN,
    LEAD_OWN,
    COUNTER_OWN,
    ARP_OWN,
    STAB_OWN,
    PAD_OWN,
    DRONE_OWN,
    // 23.09.2026, round "Keyboard": which voice a MIDI keyboard plays, and whether it replaces that voice's
    // generated notes or plays over them (Engine.h, liveNoteOn).
    KEYBOARD_PART,
    KEYBOARD_MODE,
    // 01.10.2026: off, only what the keyboard plays sounds -- every generated note is left out (live play only).
    COMPOSER,
    /** @name Appended 25.09.2026 (the mix guide): the lines' duck release (the pads and the bed keep
              *  duck_release), the counter's duck under the lead, and the pad's presence band under the lead
              *  @{ */
    DUCK_RELEASE_LINES,
    COUNTER_DUCK,
    PAD_LEAD_DUCK,
    /** @} */
    /** @brief Appended 27.09.2026: the Field track's strip (FieldPlayer.h). */
    FIELD_MUTE,
    FIELD_LEVEL,
    /** @name Appended 02.10.2026: the keyboard's options, all off by default -- the voice under a split
              *  (KeyboardLowerNames; Off: no split), the split key C1 .. C5, Scale Lock (a played key to the nearest
              *  note of the track's key and scale; not the kit), the velocity curve (frame::shapeVelocity)
              *  @{ */
    KEYBOARD_LOWER,
    KEYBOARD_SPLIT,
    KEYBOARD_SCALE,
    KEYBOARD_VELOCITY,
    /** @} */
    COUNT
};
/**
 * @brief The "own sound" switch of a polyphonic instance.
 * @param[in] i  the instance
 * @return the switch's parameter id
 */
constexpr int polyOwn( PolyInstanceE i )
{
    return LEAD_OWN + static_cast<int>( i );
}
static_assert( polyOwn( PolyInstanceE::DRONE ) == DRONE_OWN, "the own-sound switches follow PolyInstance" );
/**
 * @brief The mute of a polyphonic instance's strip.
 * @param[in] i  the instance
 * @return the mute's parameter id
 */
constexpr int polyMute( PolyInstanceE i )
{
    return LEAD_MUTE + 2 * static_cast<int>( i );
}
/**
 * @brief The level of a polyphonic instance's strip.
 * @param[in] i  the instance
 * @return the level's parameter id
 */
constexpr int polyLevel( PolyInstanceE i )
{
    return LEAD_LEVEL + 2 * static_cast<int>( i );
}
static_assert( polyMute( PolyInstanceE::DRONE ) == DRONE_MUTE && polyLevel( PolyInstanceE::STAB ) == STAB_LEVEL,
               "the mix strips of the polyphonic instances must follow PolyInstance pair by pair" );
}   // namespace mix
/**
 * @brief Parameters of the cue bridge (module Cue, prefix "cue"; PLAN 8.3).
 *
 * Off by default: a generator that starts talking to the network because it was installed would be
 * a surprise, and a visualiser that is not there is the normal case. The destination *address* is
 * not here because a parameter is a float and an address is not; it lives in the plugin's state and
 * in the Quest's `phos.cfg`, next to the port.
 */
namespace cue
{
enum : int
{
    SEND,
    PORT,
    BEATS,
    LEAD_MS,
    COUNT
};
}
/** @brief Parameters of the master section. */
namespace master
{
enum : int
{
    GAIN,
    CEILING,
    CLIP,
    COMP_THRESHOLD,
    COMP_RATIO,
    COMP_KNEE,
    COMP_ATTACK,
    COMP_RELEASE,
    MONO_BASS,
    LIMITER,
    LIMITER_RELEASE,
    TARGET_LUFS,
    AUTO_GAIN,
    CLIPPER,
    CLIPPER_THRESHOLD,
    MONITOR,   ///< 25.09.2026, the addon's monitoring: Normal, Mono, Sub (80 Hz low pass) or Side, after the meter
    COUNT
};
}

extern const char *const KeyNames[12];          ///< C, C#, ... B
extern const char *const ScaleNames[];          ///< names of compose.scale
extern const char *const StyleNames[];          ///< names of compose.style (Form.h)
extern const char *const ArcNames[];            ///< names of compose.arc (Form.h)
extern const char *const KickPatternNames[];    ///< names of compose.kick_pattern
extern const char *const BassPatternNames[];    ///< names of compose.bass_pattern
extern const char *const MelodyModelNames[];    ///< names of compose.melody_model (Model.h)
extern const char *const BassModelNames[];      ///< names of compose.bass_model (Model.h)
extern const char *const BassRhythmNames[];     ///< names of compose.bass_rhythm (Corpus.h)
extern const char *const LeadDensityNames[];    ///< names of compose.lead_density (22.09.2026)
extern const char *const PitchEntropyNames[];   ///< names of compose.pitch_entropy (22.09.2026)
extern const char *const CounterModeNames[];    ///< names of compose.counter_mode (23.09.2026)

/**
 * @brief All parameter values of one engine, lock-free readable from the audio thread.
 *
 * Construction builds the registry from the module tables. Not copyable (atomics); use
 * copyValuesFrom() for snapshots.
 */
class ParamStore
{
public:
    /** @brief Builds the registry from the module tables, every value at its default. */
    ParamStore();
    ParamStore( const ParamStore & ) = delete;
    ParamStore &operator=( const ParamStore & ) = delete;

    /**
     * @brief Number of parameters.
     * @return the number of parameters
     */
    int count() const { return static_cast<int>( m_entries.size() ); }
    /**
     * @brief First id of a module instance; -1 if it does not exist.
     * @param[in] m         the module
     * @param[in] instance  its instance
     * @return the first id of the module instance, -1 if it does not exist
     */
    int base( ModuleE m, int instance = 0 ) const;
    /**
     * @brief First id of a polyphonic instance: the one way to address a voice's parameters by name.
     * @param[in] i  the instance
     * @return its first id
     */
    int base( PolyInstanceE i ) const { return base( ModuleE::POLY, polyIndex( i ) ); }
    /**
     * @brief Descriptor of @p id.
     * @param[in] id  the parameter
     * @return its descriptor
     */
    const ParamDesc &desc( int id ) const { return *m_entries[static_cast<size_t>( id )].m_desc; }
    /**
     * @brief Full text key of @p id ("kick.pitch_start").
     * @param[in] id  the parameter
     * @return its full text key
     */
    const std::string &key( int id ) const { return m_entries[static_cast<size_t>( id )].m_key; }
    /**
     * @brief Id for a text key, or -1.
     * @param[in] key  the text key
     * @return its id, -1 when there is none
     */
    int find( std::string_view key ) const;

    /**
     * @brief Current real value.
     * @param[in] id  the parameter
     * @return its current real value
     */
    float get( int id ) const { return m_values[static_cast<size_t>( id )].load( std::memory_order_relaxed ); }
    /**
     * @brief Current value rounded to an integer (Int, Choice, Toggle).
     * @param[in] id  the parameter
     * @return its current value rounded to an integer
     */
    int getInt( int id ) const;
    /**
     * @brief Current value as a switch.
     * @param[in] id  the parameter
     * @return its current value as a switch
     */
    bool getBool( int id ) const { return get( id ) >= 0.5f; }
    /**
     * @brief Sets a real value, clamped to the range (and rounded for discrete curves).
     * @param[in] id     the parameter
     * @param[in] value  the real value
     */
    void set( int id, float value );
    /**
     * @brief Sets from a normalised 0..1 position.
     * @param[in] id    the parameter
     * @param[in] norm  the position, 0..1
     */
    void setNormalised( int id, float norm ) { set( id, fromNormalised( id, norm ) ); }

    /**
     * @brief Real value to normalised 0..1.
     * @param[in] id     the parameter
     * @param[in] value  a real value
     * @return its normalised position, 0..1
     */
    float toNormalised( int id, float value ) const;
    /**
     * @brief Normalised 0..1 to real value.
     * @param[in] id    the parameter
     * @param[in] norm  a normalised position, 0..1
     * @return the real value there
     */
    float fromNormalised( int id, float norm ) const;

    /** @brief All parameters back to their defaults. */
    void resetDefaults();
    /**
     * @brief Default of @p id: the descriptor's default, or the instance's own where a module's
     *        instances start from different values (the twelve lanes of the percussion kit).
     * @param[in] id  the parameter
     * @return its default value
     */
    float defaultValue( int id ) const { return m_defaults[static_cast<size_t>( id )]; }
    /**
     * @brief Copies every value from another store (for snapshots on another thread).
     * @param[in] other  the store whose values are copied
     */
    void copyValuesFrom( const ParamStore &other );
    /**
     * @brief Copies the current values of one module instance into @p out, indexed like its table.
     * @param[in]     m         module
     * @param[in]     instance  instance index
     * @param[in,out] out       at least as many floats as the module has parameters
     */
    void readModule( ModuleE m, int instance, float *out ) const;
    /**
     * @brief Number of parameters of a module.
     * @param[in] m  the module
     * @return how many parameters an instance of it has
     */
    static int moduleCount( ModuleE m );

    /**
     * @brief Applies "key=value" assignments separated by whitespace, newlines or ';'.
     *
     * Choice parameters accept their name ("compose.key=F#") or index. Lines starting with '#' are
     * comments.
     * @param[in]  text   the assignments
     * @param[out] error  receives a message for the first bad assignment, may be null
     * @return false if any assignment failed (the good ones are still applied)
     */
    bool parseText( std::string_view text, std::string *error = nullptr );
    /**
     * @brief The text form, one "key=value" per line.
     * @param[in] onlyChanged  leave out parameters at their default
     * @return the text form
     */
    std::string toText( bool onlyChanged ) const;
    /**
     * @brief A value formatted for display ("330 Hz", "F#").
     * @param[in] id  the parameter
     * @return its value formatted for display
     */
    std::string format( int id ) const;

private:
    /** @brief One registered parameter: its descriptor, key, module and instance. */
    struct Entry
    {
        const ParamDesc *m_desc;       ///< the descriptor in the module table
        std::string      m_key;        ///< "module.name"
        ModuleE          m_module;     ///< which module
        int              m_instance;   ///< which instance of it
    };
    std::vector<Entry>                    m_entries;           ///< every parameter, by global id
    std::unique_ptr<std::atomic<float>[]> m_values;            ///< the values, lock-free for the audio thread
    std::vector<float>                    m_defaults;          ///< the defaults, by global id
    std::unordered_map<std::string, int>  m_index;             ///< key -> global id
    static constexpr int                  MaxInstances = 16;   ///< instances a module may have (base() table)
    int m_bases[static_cast<int>( ModuleE::COUNT )]
               [MaxInstances] = {};   ///< per module and instance: the id of its first parameter, -1 none
};

}   // namespace phos
