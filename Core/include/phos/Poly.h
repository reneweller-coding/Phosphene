/**
 * @file Poly.h
 * @brief The polyphonic engine of the lead and the arp: eight voices of up to seven unison oscillators.
 *
 * **Supersaw** (the default of the lead). Seven sawtooth oscillators after Szabo's measurement of the
 * Roland JP-8000 ("How to emulate the super saw", thesis, Stockholm 2010), all four of its findings
 * adopted and each number taken from the thesis itself. Each of the seven reads the *mipmapped* saw
 * of the Classic table (WaveTable.h) rather than generating a PolyBLEP ramp: measured on 16.09.2026,
 * the table saw leaves 30 dB less aliasing at C5 and C6 and 20 dB less at A6 (docs/PLAN.md). PolyBLEP
 * remains the VA oscillator, where one saw per voice is blended into a pulse and a table cannot give
 * the pulse width. The table frame is normalised to a different RMS than the ramp, so the slot gains
 * carry kSawTableGain and the lead keeps its calibrated level.
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
 * two-operator phase modulation on the same three oscillators (PolyKernel.h), with the index limited
 * per note so that the outer sidebands Carson's rule predicts still fit under Nyquist -- an FM voice
 * is otherwise the one oscillator that can alias louder than its own carrier (Poly.cpp). **Wavetable** (the pad
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
     * The defaults (kPolyUnison, kPolyVoices) are "no limit", so a default engine runs exactly the
     * code it ran before this existed. Set once before the first note (Engine::prepare): the unison
     * limit decides which slots a note sets up, so changing it while notes sound would leave the
     * running notes with slots the render loop no longer sums.
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
     * @brief noteOn() with the voice limit applied around it.
     *
     *  - *Voices.* noteOn() takes the first inactive voice and only steals the oldest one when none
     *    is free, so voices fill up from index 0. Keeping the top voices permanently silent is
     *    therefore enough: when every voice below the limit is sounding, the oldest of them is killed
     *    here, which makes it the first free voice noteOn() finds. A silent voice costs nothing --
     *    the slot and channel kernels skip a group of eight lanes when none of its voices sounds
     *    (Poly.cpp, renderSegment) -- so four of eight pad voices really do halve the pad's filters
     *    and leave 28 of 56 oscillator slots unrendered. The limit is enforced from here rather than
     *    inside noteOn() so that the allocator stays exactly as it is.
     *  - *Unison.* noteOn() itself sets up only the middle unisonLimit_ oscillators of a voice and
     *    leaves the rest at gain 0 with no phase step and no source weight, and renderSegment() then
     *    reads no wavetable for them, keeps no slot group alive for them and leaves them out of the
     *    voice's sum (Poly.cpp). The centre and the innermost detuned pair survive, which is the
     *    narrow beating the supersaw's body comes from, and the kept gains are normalised by their
     *    own incoherent power, so the level does not jump.
     */
    void noteOnLimited(int pitch, float velocity, double lengthBeats, int gateSamples, double late)
    {
        if (voiceLimit_ < kPolyVoices) freeVoiceWithinLimit();
        noteOn(pitch, velocity, lengthBeats, gateSamples, late);
    }

    /** @brief Renders @p n stereo samples, replacing @p L and @p R. */
    void process(float* L, float* R, int n);
    /** @brief Renders with a chosen lane type (float = scalar reference), for the vector tests. */
    template <class V> void processWith(float* L, float* R, int n);
    /** @brief Voices whose envelope is open. */
    int activeVoices() const;
    /** @brief Seeds the random start phases (deterministic renders). */
    void seedPhases(uint64_t seed) { phaseRng_.seed(seed); }
    /**
     * @brief Wavetable reads the scalar pre-pass has done since prepare(), for the tests.
     *
     * The pre-pass reads one table sample per sounding wavetable slot per sample, and each read is a
     * mipmap choice plus a Catmull-Rom interpolation -- the most expensive scalar work of the engine.
     * The counter is raised once per voice and segment by the number of reads the segment did, never
     * inside the sample loop, so it costs nothing measurable. It is the only property that tells a
     * real unison limit from one that merely zeroes the outer gains: the sound is the same, the work
     * is not.
     */
    uint64_t tableReads() const { return tableReads_; }

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
    bool  sawVoice_[kPolyVoices] = {};        ///< voice plays the supersaw: the Classic saw frame, no position
    const WaveTable* table_ = nullptr;
    const WaveTable* sawTable_ = nullptr;     ///< the Classic table, whose frame kClassicSawFrame is the saw
    float posDecay_ = 0.999f, lfoInc_ = 0.0f;
    double bpm_ = 145.0;
    uint64_t counter_ = 0;
    uint64_t pos_ = 0;          ///< samples rendered since reset (the coefficient grid)
    uint64_t tableReads_ = 0;   ///< wavetable reads of the scalar pre-pass (tests, see tableReads())
    Rng phaseRng_;
    TempoDelay delay_;
    float send_ = 0.0f, level_ = 1.0f;
    std::vector<float> slotL_, slotR_, chanIn_, chanAmp_, chanOut_, sendBuf_, wtRow_;
};

} // namespace phos
