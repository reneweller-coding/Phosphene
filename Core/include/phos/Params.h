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
enum class Module : int { Compose = 0, Kick, Bass, Perc, Acid, Poly, Sfx, Fx, Mix, Master, Count };

constexpr int kPercLanes = 12;   ///< instances of the percussion lane module
constexpr int kPolyInstances = 3; ///< instances of the polyphonic engine module: "lead", "arp" and "pad"
/** @brief The instances of Module::Poly. */
enum class PolyInstance : int { Lead = 0, Arp, Pad };

/** @brief Parameters of the composer (read as a snapshot when bars are composed). */
namespace compose {
enum : int { Bpm, Key, Scale, KickPattern, BassPattern, BassGate, BassVariation, BassRegister,
             TrackBars, TrackVariation, SoundVariation, TempoRange, LevelMatch,
             PercDensity, PercVariation, Swing,
             AcidAmount, LeadAmount, ArpAmount, MelodyVariation, MelodyTemperature, SquelchChance,
             BassFollowsChords, PadAmount, SfxAmount, GateChance, Count };
}
/** @brief Parameters of one percussion lane (module Perc, twelve instances "perc1" .. "perc12"). */
namespace perc {
enum : int { Active, Role, Engine, Pitch, PitchAmount, PitchDecay, FmRatio, FmIndex, ModeSet, ModeDamp,
             MetalScale, Noise, NoiseDecay, Bursts, BurstSpacing, Decay, Filter, Cutoff, Resonance, LowCut,
             Drive, Level, Pan, Choke, Shift, Density, Tune, Count };
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
             DuckDepth, DuckHold, DuckRelease, Level, Count };
}
/** @brief Parameters of the acid voice (module Acid, prefix "acid"). */
namespace acid {
enum : int { Wave, Cutoff, Resonance, EnvAmount, Decay, Accent, SlideTime, AmpDecay, KeyTrack, Drive,
             Squelch, SquelchStart, SquelchTime, CombMix, CombFeedback, LowCut,
             DelaySend, DelayLeft, DelayRight, DelayFeedback, DelayHighPass, DelayLowPass,
             RoomSend, HallSend, Duck, Level, Count };
}
/** @brief Parameters of a polyphonic engine (module Poly, instances "lead" and "arp"). */
namespace poly {
enum : int { Osc, Detune, Mix, DynamicDetune, Wave, PulseWidth, FmRatio, FmIndex, FmDecay,
             Table, Position, PosEnv, PosDecay, PosLfoDepth, PosLfoBeats,
             Cutoff, Resonance, EnvAmount, FilterDecay, KeyTrack, HpFloor, HpTrack,
             AmpAttack, AmpDecay, AmpSustain, AmpRelease, Width, VelSens,
             DelaySend, DelayLeft, DelayRight, DelayFeedback, DelayHighPass, DelayLowPass,
             RoomSend, HallSend, Duck, Gate, GatePattern, GateDepth, GateDuty, GateAttack, GateRelease, GateTone,
             Level, Count };
}
/** @brief Parameters of the effect generator (module Sfx, prefix "sfx"). */
namespace sfx {
enum : int { Level, Noise, Resonance, Brightness, ImpactDecay, Vowel, SwellDecay, Width, RoomSend, HallSend, Duck, Count };
}
/** @brief Parameters of the send effects (module Fx, prefix "fx"): a short room and a long hall. */
namespace fx {
enum : int { RoomSize, RoomDecay, RoomDamping, HallSize, HallDecay, HallDamping, HallPreDelay, LowCut, HighCut,
             RoomReturn, HallReturn, ReturnDuck, Count };
}
/** @brief Values of poly.osc. */
enum class PolyOsc : int { Supersaw = 0, Va, Fm, Wavetable, Count };
/** @brief Delay times offered by the delay-time choices, in beats. */
inline constexpr float kDelayBeats[] = { 0.25f, 0.5f, 0.75f, 1.0f, 1.5f, 2.0f };
constexpr int kNumDelayTimes = 6;   ///< entries of kDelayBeats

/** @brief Values of bass.sub_mode. */
enum class SubMode : int { Mixed = 0, Split };
/** @brief Values of bass.kick_lock. */
enum class KickLock : int { Off = 0, BassFollowsKick, KickFollowsBass };
/** @brief Parameters of the mixer. */
namespace mix {
enum : int { KickMute, BassMute, TrackGain, PercMute, PercLevel, AcidMute, AcidLevel, LeadMute, LeadLevel,
             ArpMute, ArpLevel, PadMute, PadLevel, SfxMute, SfxLevel, PercRoom, PercHall,
             DuckAttack, DuckHold, DuckRelease, Count };
}
/** @brief Parameters of the master section. */
namespace master {
enum : int { Gain, Ceiling, Clip, CompThreshold, CompRatio, CompKnee, CompAttack, CompRelease, MonoBass,
             Limiter, LimiterRelease, TargetLufs, AutoGain, Clipper, ClipperThreshold, Count };
}

extern const char* const kKeyNames[12];         ///< C, C#, ... B
extern const char* const kScaleNames[];         ///< names of compose.scale
extern const char* const kKickPatternNames[];   ///< names of compose.kick_pattern
extern const char* const kBassPatternNames[];   ///< names of compose.bass_pattern

/**
 * @brief All parameter values of one engine, lock-free readable from the audio thread.
 *
 * Construction builds the registry from the module tables. Not copyable (atomics); use
 * copyValuesFrom() for snapshots.
 */
class ParamStore {
public:
    ParamStore();
    ParamStore(const ParamStore&) = delete;
    ParamStore& operator=(const ParamStore&) = delete;

    /** @brief Number of parameters. */
    int count() const { return static_cast<int>(entries_.size()); }
    /** @brief First id of a module instance; -1 if it does not exist. */
    int base(Module m, int instance = 0) const;
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
    struct Entry {
        const ParamDesc* desc;
        std::string key;
        Module module;
        int instance;
    };
    std::vector<Entry> entries_;
    std::unique_ptr<std::atomic<float>[]> values_;
    std::vector<float> defaults_;
    std::unordered_map<std::string, int> index_;
    static constexpr int kMaxInstances = 16;
    int bases_[static_cast<int>(Module::Count)][kMaxInstances] = {};
};

} // namespace phos
