/**
 * @file PsyFx.h
 * @brief The modulation effects of the psychedelic layer: a tempo-synchronised flanger, a phaser, a
 *        frequency shifter, the chain that holds the three, and the stutter (buffer repeat) of the
 *        melodic bus (19.09.2026, round "fx-psychedelia").
 *
 * **Why these three.** The user's inventory of a psytrance track asks for vocal samples and effects
 * "heavily treated with delay, flanger and frequency shifters". A flanger and a phaser are the two
 * classic ways to make a sound *move* without changing what it plays: a flanger is a comb filter whose
 * teeth slide (a short delay added to the dry signal, the delay swept), a phaser a set of notches that
 * slide (first-order all-pass stages added to the dry signal). A frequency shifter moves every partial
 * by the same number of hertz, which breaks the harmonic relations a pitch shifter keeps -- the
 * "alien", detuned-radio colour -- and at a few hertz it is a slow barber-pole phasing.
 *
 * **Time comes from the beat, not from a counter.** Every LFO here reads its phase from the absolute
 * beat of the sample (the engine hands in the beat of the first sample and the beats per sample). Two
 * consequences, both required by the house rules: a render started at bar 37 moves exactly like the
 * same bar inside a whole render (bar-alone equals bar-in-sequence), and the output does not depend on
 * how the host cuts its blocks. The periods are in beats, so the motion is tempo-synchronised.
 *
 * **The frequency shifter's 90-degree network** is a pair of all-pass chains of four second-order
 * sections each, y[n] = a^2 (x[n] + y[n-2]) - x[n-2], with the coefficients below and a one-sample
 * delay on the first chain. Measured (self test, "psychedelic effects"): the phase difference of the
 * two outputs stays within about one degree of 90 from 50 Hz to 20 kHz at 48 kHz, which is what makes
 * the unwanted sideband fall some 40 dB under the wanted one.
 *
 * **The stutter** records the melodic bus from the moment an event starts and plays the first slice
 * over and over for the event's length -- the "buffer repeat" of a glitch. The slice halves once over
 * the last quarter of the event (a sixteenth rolls into a thirty-second), and every slice boundary is
 * a 1 ms raised-cosine crossfade, so the repeat clicks only where the material itself does.
 */
#pragma once
#include "phos/Dsp.h"
#include <vector>

namespace phos {

/** @brief Tempo-synchronised stereo flanger (the right channel's LFO a quarter period ahead). */
class Flanger {
public:
    /** @brief Prepares for @p sampleRate (allocates the delay lines, where there are any). */
    void prepare(double sampleRate);
    /** @brief Clears every state: silence in, silence out. */
    void reset();
    /**
     * @param periodBeats LFO period in beats
     * @param depth       0..1: sweep range as a share of 0.3 .. 6 ms
     * @param feedback    -0.9 .. 0.9
     * @param mix         0..1: dry to the comb (0.5 dry + 0.5 delayed)
     */
    void set(float periodBeats, float depth, float feedback, float mix);
    /** @brief One stereo sample in place; @p beat is the absolute beat of the sample. */
    void tick(float& l, float& r, double beat);
private:
    /** @brief Reads buffer @p b @p delay samples back, linearly interpolated. */
    float read(const std::vector<float>& b, double delay) const;
    double sr_ = 48000.0;   ///< sample rate
    std::vector<float> bufL_, bufR_;   ///< the delay lines, a power of two long
    size_t mask_ = 0, write_ = 0;   ///< index mask and write position
    float period_ = 8.0f, depth_ = 0.7f, fb_ = 0.6f, mix_ = 0.0f;   ///< set(): period in beats, depth, feedback, mix
};

/** @brief Six-stage stereo phaser with feedback, tempo-synchronised. */
class Phaser {
public:
    static constexpr int kStages = 6;   ///< all-pass stages per channel
    /** @brief Prepares for @p sampleRate (allocates the delay lines, where there are any). */
    void prepare(double sampleRate);
    /** @brief Clears every state: silence in, silence out. */
    void reset();
    /** @param periodBeats LFO period; @param depth 0..1 sweep over 200 Hz .. 200 * 2^(5 depth) Hz;
     *  @param feedback 0..1; @param mix dry/wet, 0..1 */
    void set(float periodBeats, float depth, float feedback, float mix);
    /** @brief One stereo sample in place; @p beat is the absolute beat of the sample. */
    void tick(float& l, float& r, double beat);
private:
    /** @brief The six all-pass stages with feedback for one channel and one sample. */
    float stage(float x, float a, float* z, float& fbState);
    double sr_ = 48000.0;   ///< sample rate
    float zL_[kStages] = {}, zR_[kStages] = {};   ///< the all-passes' states
    float fbL_ = 0.0f, fbR_ = 0.0f;   ///< the last stage's output, fed back
    float period_ = 16.0f, depth_ = 0.8f, fb_ = 0.5f, mix_ = 0.0f;   ///< set(): period in beats, depth, feedback, mix
};

/**
 * @brief The 90-degree phase-difference network: two all-pass chains whose outputs are in quadrature.
 *
 * Public so that the self test can measure it on its own.
 */
struct HilbertPair {
    double xa[4][2] = {}, ya[4][2] = {}, xb[4][2] = {}, yb[4][2] = {};   ///< the two chains' second-order sections: inputs and outputs, two samples back
    double delayA = 0.0;   ///< chain A's output one sample late (the 90 degrees against chain B)
    /** @brief One sample in; @p i and @p q receive the in-phase and the quadrature output. */
    void tick(double x, double& i, double& q);
    /** @brief Clears every state: silence in, silence out. */
    void reset();
};

/** @brief Single-sideband frequency shifter, stereo (the right channel's carrier 90 degrees ahead). */
class FreqShifter {
public:
    /** @brief Prepares for @p sampleRate (allocates the delay lines, where there are any). */
    void prepare(double sampleRate);
    /** @brief Clears every state: silence in, silence out. */
    void reset();
    /** @param hz shift in Hz (negative = down); @param mix 0..1 */
    void set(float hz, float mix);
    /** @brief One stereo sample in place. */
    void tick(float& l, float& r);
private:
    double sr_ = 48000.0;   ///< sample rate
    HilbertPair hl_, hr_;   ///< the quadrature networks of the two channels
    double phase_ = 0.0;   ///< the carrier's phase, in cycles
    float hz_ = 0.0f, mix_ = 0.0f;   ///< set(): shift in Hz and mix
};

/**
 * @brief Flanger, phaser and frequency shifter in series, with the event motion on top.
 *
 * The chain carries one extra input besides its parameters: the **event motion** (setMotion), a shift
 * in hertz and a flanger mix that the engine sets per effect event -- a riser drags the shifter up with
 * it, a downlifter down, a sweep opens the flanger. That is the "per event" half of the automation;
 * the "per section" half arrives as ordinary control events on the psyfx parameters (Form.cpp,
 * sectionAutomation).
 */
class PsyFxChain {
public:
    /** @brief Prepares for @p sampleRate (allocates the delay lines, where there are any). */
    void prepare(double sampleRate);
    /** @brief Clears every state: silence in, silence out. */
    void reset();
    /** @brief Reads the effective psyfx parameters (indexed by psyfx::). */
    void update(const float* v);
    /** @brief Extra shift in Hz and extra flanger mix added on top of the parameters (per event). */
    void setMotion(float shiftHz, float flangerMix);
    /** @brief The event motion's shift in force (tests). */
    float motionHz() const { return motionHz_; }
    /** @brief The event motion's extra flanger mix in force (tests). */
    float motionFlange() const { return motionFlange_; }
    /**
     * @brief Processes @p n stereo samples in place.
     * @param beat0 absolute beat of the first sample
     * @param bps   beats per sample
     * @param L,R the stereo signal, processed in place
     * @param n   samples
     */
    void process(float* L, float* R, int n, double beat0, double bps);
private:
    /** @brief Hands the parameters and the event motion to the three effects. */
    void apply();
    Flanger flanger_;   ///< first in the chain
    Phaser phaser_;   ///< second
    FreqShifter shifter_;   ///< third
    float v_[16] = {};   ///< the psyfx parameters as update() read them
    float motionHz_ = 0.0f, motionFlange_ = 0.0f;   ///< setMotion()
};

/** @brief Buffer repeat of a stereo bus for the length of an event (the glitch of the psychedelic layer). */
class Stutter {
public:
    /** @brief Prepares for @p sampleRate (allocates the delay lines, where there are any). */
    void prepare(double sampleRate);
    /** @brief Clears every state: silence in, silence out. */
    void reset();
    /**
     * @brief Starts a repeat.
     * @param lengthSamples how long the repeat replaces the live signal
     * @param sliceSamples  the slice that is repeated (it halves over the last quarter)
     */
    void trigger(int lengthSamples, int sliceSamples);
    /** @brief True while a repeat is running. */
    bool active() const { return on_; }
    /** @brief One stereo sample: records it and returns the live sample or the repeat in its place. */
    void tick(float& l, float& r);
private:
    std::vector<float> recL_, recR_;   ///< the recorded live signal
    bool on_ = false;   ///< a repeat is running
    long long pos_ = 0, length_ = 0, slice_ = 1;   ///< samples into the repeat, its length, the slice repeated
    int fade_ = 48;   ///< crossfade at the slice's seams, samples (1 ms)
};

} // namespace phos
