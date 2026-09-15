/**
 * @file Poly.h
 * @brief The polyphonic engine of the lead and the arp: eight voices of up to seven unison oscillators.
 *
 * **Supersaw** (the default of the lead). Seven sawtooth oscillators after Szabo's measurement of the
 * Roland JP-8000 ("How to emulate the super saw", thesis, Stockholm 2010), all four of its findings
 * adopted and each number taken from the thesis itself:
 *  - *Detune.* The side oscillators sit at 1 + a_i y(x) times the centre frequency with
 *    a = (-0.11002313, -0.06288439, -0.01952356, 0, 0.01991221, 0.06216538, 0.10745242) (table 1),
 *    where the detune knob x goes through the eleventh-degree polynomial y(x) fitted to the
 *    instrument (table 2): almost flat up to the middle, steep after 0.9 -- fine control of the narrow
 *    detunings that make the pad and string sounds.
 *  - *Mix.* The centre oscillator's gain falls linearly, -0.55366 x + 0.99785, while the six side
 *    oscillators rise along -0.73764 x^2 + 1.2841 x + 0.044372 (section 3.2). Here the two curves set
 *    the balance; the sum is normalised by its incoherent power so that the mix knob does not change
 *    the loudness (a deviation from the instrument, which gets 4 dB louder).
 *  - *Phase.* Every note starts every oscillator at a random phase (section 3.4): with fixed phases
 *    the seven saws add up coherently at the attack and every note sounds the same.
 *  - *High pass.* A high pass that follows the pitch removes what lies below the fundamental (section
 *    3.3). Phosphene makes it the depth rule for everything that is not kick or bass: 24 dB/octave at
 *    max(HP Floor, HP Track x f0), so nothing of the lead reaches the band under 140 Hz where the
 *    phase lock of kick and bass lives.
 *
 * **Dynamic detune.** A short note of seven beating saws smears its attack, a long one blooms. The
 * detune a note plays with therefore scales with its length: at a sixteenth (a quarter of a beat) or
 * shorter it is (1 - Dynamic Detune) of the knob, from one beat on the full knob, in between along
 * log2 of the length.
 *
 * **VA** plays the centre and the inner pair of oscillators as saw-to-pulse blends; **FM** plays
 * two-operator phase modulation on the same three oscillators (PolyKernel.h). **Wavetable** (the pad
 * instance's default) plays all seven oscillators with Szabo's detune and mix, each reading a built-in
 * table (WaveTable.h) at its own random phase; the table position moves with an envelope and a slow LFO
 * whose period is given in beats. A table read is a gather, so it runs on the scalar side and hands the
 * kernel a row of samples -- the same code for every vector path, so the lanes stay bit-identical.
 *
 * The voice filter is a resonant 12 dB state-variable low pass with its own envelope and key
 * tracking; the amplitude envelope is the ADSR of Dsp.h. After the voices a tempo delay (TempoDelay.h).
 */
#pragma once
#include "phos/Dsp.h"
#include "phos/PolyKernel.h"
#include "phos/TempoDelay.h"
#include "phos/WaveTable.h"
#include <cmath>
#include <vector>

namespace phos {

/** @brief Szabo's detune offsets of the seven supersaw oscillators (thesis, table 1). */
inline constexpr double kSupersawOffsets[kPolyUnison] = { -0.11002313, -0.06288439, -0.01952356, 0.0, 0.01991221, 0.06216538, 0.10745242 };

/** @brief Polyphonic engine. */
class Poly {
public:
    /** @brief Prepares for a sample rate. */
    void prepare(double sampleRate);
    /** @brief Silences every voice and clears all state. */
    void reset();
    /** @brief Reads the effective parameters (indexed by poly::) and the tempo (for the delay). */
    void update(const float* v, double bpm);
    /**
     * @brief Starts a note.
     * @param pitch       MIDI note
     * @param velocity    0..1
     * @param lengthBeats written length of the note (for the dynamic detune)
     * @param gateSamples samples until release
     * @param late        how many samples ago the note ideally started (0 <= late < 1)
     */
    void noteOn(int pitch, float velocity, double lengthBeats, int gateSamples, double late);

    /**
     * @brief Sets the quality limits of this instance (Quality.h).
     * @param unison oscillators per voice, 1 .. kPolyUnison
     * @param voices voices that may sound at once, 1 .. kPolyVoices
     *
     * The defaults (kPolyUnison, kPolyVoices) are "no limit" and are checked for first, so a
     * default engine runs exactly the code it ran before this existed.
     */
    void setQuality(int unison, int voices)
    {
        unisonLimit_ = unison < 1 ? 1 : (unison > kPolyUnison ? kPolyUnison : unison);
        voiceLimit_ = voices < 1 ? 1 : (voices > kPolyVoices ? kPolyVoices : voices);
    }
    /** @brief Oscillators per voice this instance plays. */
    int unisonLimit() const { return unisonLimit_; }
    /** @brief Voices this instance may sound at once. */
    int voiceLimit() const { return voiceLimit_; }

    /**
     * @brief noteOn() with the quality limits applied around it.
     *
     * Both limits are enforced from here rather than inside noteOn() so that the allocator and the
     * render loop in Poly.cpp stay exactly as they are (the vector tests compare them bit for bit).
     *
     *  - *Voices.* noteOn() takes the first inactive voice and only steals the oldest one when none
     *    is free, so voices fill up from index 0. Keeping the top voices permanently silent is
     *    therefore enough: when every voice below the limit is sounding, the oldest of them is killed
     *    here, which makes it the first free voice noteOn() finds. A silent voice costs nothing --
     *    the slot and channel kernels skip a group of eight lanes when none of its voices sounds
     *    (Poly.cpp, renderSegment) -- so four of eight pad voices really do halve the pad's filters
     *    and leave 28 of 56 oscillator slots unrendered.
     *  - *Unison.* The oscillator gains of the outer unison pairs are set to zero and the remaining
     *    ones are scaled so that the incoherent power of the voice is unchanged; the centre and the
     *    innermost detuned pair survive, which is the narrow beating the supersaw's body comes from.
     *    This does not save the kernel any arithmetic (the slots are computed either way, see the
     *    note in the Phase 7 report), it makes the sound of the level.
     */
    void noteOnLimited(int pitch, float velocity, double lengthBeats, int gateSamples, double late)
    {
        if (voiceLimit_ < kPolyVoices) freeVoiceWithinLimit();
        noteOn(pitch, velocity, lengthBeats, gateSamples, late);
        if (unisonLimit_ < kPolyUnison) applyUnisonLimit();
    }

    /** @brief Renders @p n stereo samples, replacing @p L and @p R. */
    void process(float* L, float* R, int n);
    /** @brief Renders with a chosen lane type (float = scalar reference), for the vector tests. */
    template <class V> void processWith(float* L, float* R, int n);
    /** @brief Voices whose envelope is open. */
    int activeVoices() const;
    /** @brief Seeds the random start phases (deterministic renders). */
    void seedPhases(uint64_t seed) { phaseRng_.seed(seed); }

    /** @brief Szabo's detune curve y(x), x in [0, 1]. */
    static double detuneCurve(double x);
    /** @brief Szabo's centre and side gains for the mix knob x in [0, 1]. */
    static void mixGains(double x, double& center, double& side);
    /** @brief The detune knob value a note of @p lengthBeats plays with. */
    static double dynamicDetune(double knob, double amount, double lengthBeats);

private:
    /**
     * @brief Makes sure a voice below voiceLimit_ is free before noteOn() allocates.
     *
     * When one is already free, nothing happens and the note is allocated as always. Otherwise the
     * oldest voice below the limit is killed, which is the steal noteOn() would have done anyway --
     * it reuses the voice that started longest ago -- only restricted to the voices this quality
     * level allows.
     */
    void freeVoiceWithinLimit()
    {
        int oldest = 0;
        for (int i = 0; i < voiceLimit_; ++i) {
            if (!amp_[i].isActive()) return;
            if (age_[i] < age_[oldest]) oldest = i;
        }
        amp_[oldest].kill();
        gate_[oldest] = 0;
    }

    /**
     * @brief Keeps only the innermost unisonLimit_ oscillators of the voice that was just started.
     *
     * The voice is the one noteOn() gave the newest age to. The kept oscillators are the middle
     * block of the seven, so 3 keeps Szabo's centre and the pair at +-0.0195 (his narrowest
     * detuning); the gains are rescaled by the square root of the ratio of the incoherent powers, so
     * the voice keeps the loudness the full unison would have had. Voices whose oscillator type only
     * uses the middle three anyway (VA, FM) come out unchanged.
     */
    void applyUnisonLimit()
    {
        int voice = 0;
        for (int i = 1; i < kPolyVoices; ++i) if (age_[i] > age_[voice]) voice = i;
        const int first = (kPolyUnison - unisonLimit_) / 2;
        const int last = first + unisonLimit_;
        double all = 0.0, kept = 0.0;
        for (int u = 0; u < kPolyUnison; ++u) {
            const int s = voice * kPolyUnison + u;
            const double p = static_cast<double>(slots_.gL[s]) * slots_.gL[s] + static_cast<double>(slots_.gR[s]) * slots_.gR[s];
            all += p;
            if (u >= first && u < last) kept += p;
        }
        const float scale = kept > 0.0 ? static_cast<float>(std::sqrt(all / kept)) : 0.0f;
        for (int u = 0; u < kPolyUnison; ++u) {
            const int s = voice * kPolyUnison + u;
            const bool keep = u >= first && u < last;
            slots_.gL[s] = keep ? slots_.gL[s] * scale : 0.0f;
            slots_.gR[s] = keep ? slots_.gR[s] * scale : 0.0f;
        }
    }

    void voiceCoefs(int voice);
    void lowPassCoefs(int voice, double damping);
    template <class V> void renderSegment(float* L, float* R, int n);

    double sr_ = 48000.0;
    int unisonLimit_ = kPolyUnison;   ///< oscillators per voice (Quality.h)
    int voiceLimit_ = kPolyVoices;    ///< voices that may sound at once (Quality.h)
    float values_[64] = {};
    PolySlots slots_;
    PolyChannels ch_;
    Envelope amp_[kPolyVoices];
    float fenv_[kPolyVoices] = {}, fDecay_ = 0.999f;
    int   pitch_[kPolyVoices] = {};
    float vel_[kPolyVoices] = {};
    int   gate_[kPolyVoices] = {};
    uint64_t age_[kPolyVoices] = {};
    float hpHz_[kPolyVoices] = {};
    float posEnv_[kPolyVoices] = {};          ///< table-position envelope per voice
    double lfoPh_[kPolyVoices] = {};          ///< table-position LFO phase per voice
    double wtPh_[kPolySlots] = {};            ///< wavetable phase per slot (double: long pads)
    double wtDt_[kPolySlots] = {};            ///< wavetable phase step per slot
    int wtLevel_[kPolySlots] = {};            ///< table level per slot
    const WaveTable* table_ = nullptr;
    float posDecay_ = 0.999f, lfoInc_ = 0.0f;
    double bpm_ = 145.0;
    uint64_t counter_ = 0;
    uint64_t pos_ = 0;          ///< samples rendered since reset (the coefficient grid)
    Rng phaseRng_;
    TempoDelay delay_;
    float send_ = 0.0f, level_ = 1.0f;
    std::vector<float> slotL_, slotR_, chanIn_, chanAmp_, chanOut_, sendBuf_, wtRow_;
};

} // namespace phos
