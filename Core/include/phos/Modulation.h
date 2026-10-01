/**
 * @file Modulation.h
 * @brief A voice's own modulation (26.09.2026): a modulation envelope, four LFOs and an eight-slot modulation matrix,
 *        taken over from the BerlinSchoolGenerator (eph/synth/Modulation.h) on the user's request: "je eine
 *        vollständige ADSR-Hüllkurve für AMP und Filter sollte schon drin sein (vielleicht auch noch ein dritter, für
 *        weitere Modulationen?). Und drei, vier LFOs doch eigentlich auch, z.B. um durch das Wavetable zu
 *        interpolieren." The six melodic voices carry it per voice, the bass and the acid once.
 *
 * **Sources.** The four LFOs (bipolar, -1..1), the modulation envelope and the synth's filter envelope (0..1), the
 * note's velocity (0..1), its key position (-1..1 over four octaves around middle C) and a random value drawn per
 * note (-1..1). **Destinations.** Pitch, the second oscillator's pitch, pulse width, the place in a wavetable, the FM
 * index, cutoff, resonance, the filter's mode, the level and the pan. A slot's amount is -1..1 of the destination's
 * span (modDestSpan): 12 semitones for the pitch (the amount squared, so a vibrato of a few cents sits on the first
 * tenth of the knob), 24 for the second oscillator, 0.45 of the pulse width, the whole table, five octaves of cutoff,
 * the whole of the resonance and the mode, the FM index doubled or shut, the level doubled or shut, the pan from one
 * side to the other.
 *
 * **Determinism.** Everything runs on the synth's absolute sample count and the set's beat, never on the host's
 * blocks: the envelope per sample, the LFOs evaluated at the synth's control steps (the 16-sample grid of Poly). A
 * free LFO's phase advances by the samples between two evaluations; a synced one reads its phase off the beat, so a
 * one-bar sweep opens on every downbeat unless it restarts with each note (retrig). The random shapes draw from a
 * stream of their own.
 *
 * **Fade.** An LFO can come in over a few seconds after each note, as a player reaches for the wheel.
 */
#pragma once
#include "phos/Dsp.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>

namespace phos {

constexpr int kLfos = 4;        ///< LFOs per voice
constexpr int kModSlots = 8;    ///< slots of the modulation matrix

/** @brief The LFOs' shapes (kLfoShapeNames). */
enum class LfoShape : int { Sine, Triangle, SawUp, SawDown, Square, SampleHold, SmoothRandom, Count };
/** @brief The matrix's sources (kModSourceNames). */
enum class ModSource : int { Off, Lfo1, Lfo2, Lfo3, Lfo4, ModEnv, FilterEnv, Velocity, Key, Random, Count };
/** @brief The matrix's destinations (kModDestNames). */
enum class ModDest : int { Off, Pitch, Osc2Pitch, PulseWidth, TablePos, FmIndex, Cutoff, Resonance, FilterMode, Level, Pan, Count };
constexpr int kModSources = static_cast<int>(ModSource::Count);   ///< sources including Off
constexpr int kModDests = static_cast<int>(ModDest::Count);       ///< destinations including Off
constexpr int kLfoSyncs = 10;                                     ///< entries of kLfoSyncNames

/** @brief Cycles per beat of an LFO's sync division (kLfoSyncNames: free, 4, 2, 1 bars, 1/2 .. 1/16, 1/4T, 1/8T); 0: free. */
inline double lfoCyclesPerBeat(int sync)
{
    static const double k[kLfoSyncs] = { 0.0, 1.0 / 16.0, 1.0 / 8.0, 1.0 / 4.0, 1.0 / 2.0, 1.0, 2.0, 4.0, 1.5, 3.0 };
    return k[std::clamp(sync, 0, kLfoSyncs - 1)];
}

/** @brief One LFO's settings. */
struct LfoSettings {
    float rateHz = 1.0f;   ///< the free rate
    int shape = 0;         ///< LfoShape
    int sync = 0;          ///< tempo division (lfoCyclesPerBeat), 0 free
    int retrig = 0;        ///< 1: every note restarts it
    float fadeS = 0.0f;    ///< it comes in over this long after each note
};
/** @brief One slot of the matrix. */
struct ModSlot {
    int src = 0;           ///< ModSource
    int dst = 0;           ///< ModDest
    float amount = 0.0f;   ///< -1..1 of the destination's span
};
/** @brief A voice's modulation settings: the modulation envelope, the LFOs, the matrix. */
struct ModSettings {
    float attackMs = 10.0f;   ///< the modulation envelope's attack, ms
    float decayMs = 500.0f;   ///< its decay, ms
    float sustain = 0.0f;   ///< its sustain, 0..1
    float releaseMs = 300.0f;   ///< its release, ms
    LfoSettings lfo[kLfos];   ///< the four LFOs
    ModSlot slot[kModSlots];  ///< the matrix
};

/**
 * @brief The modulation block's layout in a module's parameter table (Params.h; 26.09.2026): the filter envelope's
 *        attack, sustain and release, the modulation envelope's four times, four LFOs of five knobs (rate, shape,
 *        sync, retrig, fade) and eight slots of three (source, target, amount) -- 51 knobs from the block's first.
 */
constexpr int kModBlockMenv = 3;   ///< the modulation envelope's first knob in the block
constexpr int kModBlockLfo = 7;   ///< the first LFO's first knob
constexpr int kModBlockSlots = 27;   ///< the first slot's first knob
constexpr int kModBlockSize = 51;   ///< knobs in the block

/**
 * @brief The ModSettings a module's knobs ask for; @p b points at the block's first knob (its FiltAttack).
 * @param b the block's first knob in the module's effective values
 * @param dstMap where a synth offers only some destinations (the bass): its target list's index -> ModDest, or null
 * @param dstCount entries of @p dstMap
 */
inline ModSettings readModBlock(const float* b, const int* dstMap = nullptr, int dstCount = 0)
{
    ModSettings ms;
    ms.attackMs = b[kModBlockMenv];
    ms.decayMs = b[kModBlockMenv + 1];
    ms.sustain = b[kModBlockMenv + 2];
    ms.releaseMs = b[kModBlockMenv + 3];
    for (int l = 0; l < kLfos; ++l) {
        const float* f = b + kModBlockLfo + 5 * l;
        ms.lfo[l].rateHz = f[0];
        ms.lfo[l].shape = static_cast<int>(std::lround(f[1]));
        ms.lfo[l].sync = static_cast<int>(std::lround(f[2]));
        ms.lfo[l].retrig = f[3] >= 0.5f ? 1 : 0;
        ms.lfo[l].fadeS = f[4];
    }
    for (int k = 0; k < kModSlots; ++k) {
        const float* f = b + kModBlockSlots + 3 * k;
        ms.slot[k].src = static_cast<int>(std::lround(f[0]));
        const int d = static_cast<int>(std::lround(f[1]));
        ms.slot[k].dst = dstMap == nullptr ? d : dstMap[std::clamp(d, 0, dstCount - 1)];
        ms.slot[k].amount = f[2];
    }
    return ms;
}

/** @brief The span of each destination for an amount of 1 (see the file comment); the pitch's amount is squared. */
inline float modDestSpan(ModDest d)
{
    switch (d) {
    case ModDest::Pitch: return 12.0f;
    case ModDest::Osc2Pitch: return 24.0f;
    case ModDest::PulseWidth: return 0.45f;
    case ModDest::Cutoff: return 5.0f;
    case ModDest::Off: return 0.0f;
    default: return 1.0f;
    }
}

/**
 * @brief One voice's modulation: the envelope, the LFOs, the matrix (see the file comment).
 *
 * Use: set() when the settings change; noteOn() / noteOff() with the voice's notes; tick() every sample (the
 * envelope); evaluate() at the voice's control steps, which returns the sums per destination.
 */
class Modulator {
public:
    /** @brief The sample rate and the seed of the random shapes' stream. */
    void prepare(double sampleRate, uint64_t seed)
    {
        sr_ = sampleRate > 0.0 ? sampleRate : 48000.0;
        env_.setSampleRate(sr_);
        env_.kill();
        for (int l = 0; l < kLfos; ++l) {
            lfo_[l] = LfoState{};
            lfo_[l].rng.seed(mixSeed(seed, 0x4C464F00ull + static_cast<uint64_t>(l)));
        }
        lastAt_ = 0;
        noteAt_ = std::numeric_limits<int64_t>::min() / 2;
        set(m_);
    }
    /** @brief Takes new settings; the LFOs run on. */
    void set(const ModSettings& m)
    {
        m_ = m;
        env_.setTimes(m.attackMs * 0.001f, m.decayMs * 0.001f, std::clamp(m.sustain, 0.0f, 1.0f), m.releaseMs * 0.001f);
        lfoUsed_ = 0u;
        dstMask_ = 0u;
        envUsed_ = false;
        live_ = 0;
        for (const ModSlot& s : m.slot) {
            const int src = std::clamp(s.src, 0, kModSources - 1), dst = std::clamp(s.dst, 0, kModDests - 1);
            if (src == 0 || dst == 0 || s.amount == 0.0f) continue;
            const ModDest d = static_cast<ModDest>(dst);
            const float a = std::clamp(s.amount, -1.0f, 1.0f);
            Live& l = liveSlots_[live_++];
            l.src = src;
            l.dst = dst;
            l.amount = (d == ModDest::Pitch ? a * std::fabs(a) : a) * modDestSpan(d);
            dstMask_ |= 1u << dst;
            if (src >= static_cast<int>(ModSource::Lfo1) && src <= static_cast<int>(ModSource::Lfo4))
                lfoUsed_ |= 1u << (src - static_cast<int>(ModSource::Lfo1));
            envUsed_ = envUsed_ || src == static_cast<int>(ModSource::ModEnv);
        }
        for (int l = 0; l < kLfos; ++l) {
            lfo_[l].inc = std::clamp(static_cast<double>(m.lfo[l].rateHz), 0.0, 100.0) / sr_;
            lfo_[l].cpb = lfoCyclesPerBeat(m.lfo[l].sync);
        }
    }
    /** @brief Whether any slot is live. */
    bool active() const { return live_ > 0; }
    /** @brief Whether a live slot reaches @p d. */
    bool targets(ModDest d) const { return ((dstMask_ >> static_cast<int>(d)) & 1u) != 0u; }
    /** @brief A new note at sample @p at, beat @p beat: the envelope's attack, the retriggered LFOs from their start, the fades from 0. */
    void noteOn(int64_t at, double beat)
    {
        env_.noteOn();
        noteAt_ = at;
        advance(at);
        for (int l = 0; l < kLfos; ++l) {
            if (m_.lfo[l].retrig == 0) continue;
            LfoState& s = lfo_[l];
            // The next whole cycle begins here (a new cycle also draws a new random value).
            if (s.cpb > 0.0) s.offset = beat * s.cpb - (std::floor(beat * s.cpb - s.offset) + 1.0);
            else s.cyc = std::floor(s.cyc) + 1.0;
        }
    }
    /** @brief The note's end: the envelope's release. */
    void noteOff() { env_.noteOff(); }
    /** @brief Silences the envelope. */
    void kill() { env_.kill(); }
    /** @brief One sample of the envelope (only when a slot reads it). */
    void tick() { if (envUsed_) env_.process(); }
    /** @brief @p n samples of the envelope at once (the voice's 16-sample grid). */
    void tick(int n) { if (envUsed_) for (int i = 0; i < n; ++i) env_.process(); }
    /**
     * @brief The sums per destination at sample @p at, beat @p beat.
     * @param at the synth's absolute sample count; @param beat the set's beat there
     * @param ext the voice's own sources, indexed by ModSource (FilterEnv, Velocity, Key, Random are read)
     * @param out kModDests sums, overwritten
     */
    void evaluate(int64_t at, double beat, const float* ext, float* out)
    {
        float src[kModSources] = {};
        for (int i = 0; i < kModSources; ++i) src[i] = ext[i];
        src[0] = 0.0f;
        src[static_cast<int>(ModSource::ModEnv)] = env_.level();
        advance(at);
        for (int l = 0; l < kLfos; ++l)
            if (((lfoUsed_ >> l) & 1u) != 0u) src[static_cast<int>(ModSource::Lfo1) + l] = value(l, at, beat);
        for (int d = 0; d < kModDests; ++d) out[d] = 0.0f;
        for (int i = 0; i < live_; ++i) out[liveSlots_[i].dst] += src[liveSlots_[i].src] * liveSlots_[i].amount;
    }
    /** @brief The envelope's level (for tests). */
    float envLevel() const { return env_.level(); }
    /** @brief @p sum of destination @p d where a slot reaches it, else nothing (for the synths' per-destination reads). */
    float offset(const float* sums, ModDest d) const { return active() && targets(d) ? sums[static_cast<int>(d)] : 0.0f; }

private:
    /** @brief One LFO's running state. */
    struct LfoState {
        double cyc = 0.0;       ///< a free LFO's cycles
        double offset = 0.0;    ///< a synced one's shift against the beat (retrig)
        double inc = 0.0;       ///< cycles per sample, free
        double cpb = 0.0;       ///< cycles per beat, synced (0: free)
        int64_t cycle = std::numeric_limits<int64_t>::min();   ///< the whole cycle last read (the random shapes draw on a new one)
        float from = 0.0f;   ///< the random shapes' last value
        float to = 0.0f;   ///< ... and the next
        Rng rng;                ///< the random shapes' stream
    };
    /** @brief A slot that reaches something, with its amount already scaled by the span. */
    struct Live {
        int src = 0;           ///< its source (ModSource)
        int dst = 0;           ///< its destination (ModDest)
        float amount = 0.0f;   ///< its amount, scaled by the destination's span
    };

    /** @brief The free LFOs run on to sample @p at (read or not). */
    void advance(int64_t at)
    {
        const double n = static_cast<double>(at - lastAt_);
        for (LfoState& s : lfo_) if (s.cpb <= 0.0) s.cyc += n * s.inc;
        lastAt_ = at;
    }
    /** @brief LFO @p l's value at sample @p at, beat @p beat. */
    float value(int l, int64_t at, double beat)
    {
        LfoState& s = lfo_[l];
        const double c = s.cpb > 0.0 ? beat * s.cpb - s.offset : s.cyc;
        const double whole = std::floor(c);
        const float p = static_cast<float>(c - whole);
        const int64_t w = static_cast<int64_t>(whole);
        if (w != s.cycle) {
            s.cycle = w;
            s.from = s.to;
            s.to = s.rng.bipolar();
        }
        float v = 0.0f;
        switch (static_cast<LfoShape>(std::clamp(m_.lfo[l].shape, 0, static_cast<int>(LfoShape::Count) - 1))) {
        case LfoShape::Sine: v = sin01(p); break;
        case LfoShape::Triangle: v = p < 0.25f ? 4.0f * p : (p < 0.75f ? 2.0f - 4.0f * p : 4.0f * p - 4.0f); break;
        case LfoShape::SawUp: v = 2.0f * p - 1.0f; break;
        case LfoShape::SawDown: v = 1.0f - 2.0f * p; break;
        case LfoShape::Square: v = p < 0.5f ? 1.0f : -1.0f; break;
        case LfoShape::SampleHold: v = s.to; break;
        case LfoShape::SmoothRandom: v = s.from + (s.to - s.from) * p * p * (3.0f - 2.0f * p); break;
        default: break;
        }
        const float fade = m_.lfo[l].fadeS;
        if (fade > 0.0f) {
            const double t = static_cast<double>(at - noteAt_) / (static_cast<double>(fade) * sr_);
            if (t < 1.0) v *= static_cast<float>(std::max(0.0, t));
        }
        return v;
    }

    double sr_ = 48000.0;          ///< sample rate
    ModSettings m_;                ///< the settings
    Envelope env_;                 ///< the modulation envelope
    LfoState lfo_[kLfos];          ///< the LFOs
    Live liveSlots_[kModSlots];    ///< the slots that reach something
    int live_ = 0;                 ///< how many
    unsigned lfoUsed_ = 0u;   ///< which LFOs a slot reads (bit l)
    unsigned dstMask_ = 0u;   ///< which destinations a slot reaches (bit d)
    bool envUsed_ = false;         ///< a slot reads the envelope
    int64_t lastAt_ = 0;           ///< the sample the free LFOs ran to
    int64_t noteAt_ = 0;           ///< the last note's sample (the fades)
};

} // namespace phos
