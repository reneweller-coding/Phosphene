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
 * @author Rene Weller
 */
#pragma once
#include "phos/Dsp.h"
#include "phos/Params.h"
#include <vector>

namespace phos {

/** @brief Tempo-synchronised stereo flanger (the right channel's LFO a quarter period ahead). */
class Flanger {
public:
    /**
     * @brief Prepares for @p sampleRate (allocates the delay lines, where there are any).
     * @param[in] sampleRate  the sample rate
     */
    void prepare(double sampleRate);
    /** @brief Clears every state: silence in, silence out. */
    void reset();
    /**
     * @param[in] periodBeats  LFO period in beats
     * @param[in] depth        0..1: sweep range as a share of 0.3 .. 6 ms
     * @param[in] feedback     -0.9 .. 0.9
     * @param[in] mix          0..1: dry to the comb (0.5 dry + 0.5 delayed)
     */
    void set(float periodBeats, float depth, float feedback, float mix);
    /**
     * @brief One stereo sample in place; @p beat is the absolute beat of the sample.
     * @param[in,out] l     the left sample, in place
     * @param[in,out] r     the right sample, in place
     * @param[in]     beat  the sample's beat
     */
    void tick(float& l, float& r, double beat);
private:
    /**
     * @brief Reads buffer @p b @p delay samples back, linearly interpolated.
     * @param[in] b      the buffer
     * @param[in] delay  the delay in samples
     * @return the sample read
     */
    float read(const std::vector<float>& b, double delay) const;
    double m_sr = 48000.0;   ///< sample rate
    std::vector<float> m_bufL;   ///< the left delay line, a power of two long
    std::vector<float> m_bufR;   ///< ... the right one
    size_t m_mask = 0;   ///< the index mask
    size_t m_write = 0;   ///< the write position
    float m_period = 8.0f;   ///< set(): the period, beats
    float m_depth = 0.7f;   ///< set(): the depth
    float m_fb = 0.6f;   ///< set(): the feedback
    float m_mix = 0.0f;   ///< set(): the mix
};

/** @brief Six-stage stereo phaser with feedback, tempo-synchronised. */
class Phaser {
public:
    static constexpr int Stages = 6;   ///< all-pass stages per channel
    /**
     * @brief Prepares for @p sampleRate (allocates the delay lines, where there are any).
     * @param[in] sampleRate  the sample rate
     */
    void prepare(double sampleRate);
    /** @brief Clears every state: silence in, silence out. */
    void reset();
    /**
     * @param[in] periodBeats  LFO period;
     * @param[in] depth        0..1 sweep over 200 Hz .. 200 * 2^(5 depth) Hz;
     * @param[in] feedback     0..1;
     * @param[in] mix          dry/wet, 0..1
     */
    void set(float periodBeats, float depth, float feedback, float mix);
    /**
     * @brief One stereo sample in place; @p beat is the absolute beat of the sample.
     * @param[in,out] l     the left sample, in place
     * @param[in,out] r     the right sample, in place
     * @param[in]     beat  the sample's beat
     */
    void tick(float& l, float& r, double beat);
private:
    /**
     * @brief The six all-pass stages with feedback for one channel and one sample.
     * @param[in]     x        the input sample
     * @param[in]     a        the all-pass coefficient
     * @param[in,out] z        the stages' states
     * @param[in,out] fbState  the feedback's state
     * @return the output sample
     */
    float stage(float x, float a, float* z, float& fbState);
    double m_sr = 48000.0;   ///< sample rate
    float m_zL[Stages] = {};   ///< the all-passes' states, left
    float m_zR[Stages] = {};   ///< ... right
    float m_fbL = 0.0f;   ///< the last stage's output, fed back, left
    float m_fbR = 0.0f;   ///< ... right
    float m_period = 16.0f;   ///< set(): the period, beats
    float m_depth = 0.8f;   ///< set(): the depth
    float m_fb = 0.5f;   ///< set(): the feedback
    float m_mix = 0.0f;   ///< set(): the mix
};

/**
 * @brief The 90-degree phase-difference network: two all-pass chains whose outputs are in quadrature.
 *
 * Public so that the self test can measure it on its own.
 */
struct HilbertPair {
    double m_xa[4][2] = {};   ///< chain a's second-order sections: inputs, two samples back
    double m_ya[4][2] = {};   ///< ... outputs
    double m_xb[4][2] = {};   ///< chain b's: inputs
    double m_yb[4][2] = {};   ///< ... outputs
    double m_delayA = 0.0;   ///< chain A's output one sample late (the 90 degrees against chain B)
    /**
     * @brief One sample in; @p i and @p q receive the in-phase and the quadrature output.
     * @param[in]  x  the input sample
     * @param[out] i  the in-phase output
     * @param[out] q  the quadrature output
     */
    void tick(double x, double& i, double& q);
    /** @brief Clears every state: silence in, silence out. */
    void reset();
};

/** @brief Single-sideband frequency shifter, stereo (the right channel's carrier 90 degrees ahead). */
class FreqShifter {
public:
    /**
     * @brief Prepares for @p sampleRate (allocates the delay lines, where there are any).
     * @param[in] sampleRate  the sample rate
     */
    void prepare(double sampleRate);
    /** @brief Clears every state: silence in, silence out. */
    void reset();
    /**
     * @param[in] hz   shift in Hz (negative = down);
     * @param[in] mix  0..1
     */
    void set(float hz, float mix);
    /**
     * @brief One stereo sample in place.
     * @param[in,out] l  the left sample, in place
     * @param[in,out] r  the right sample, in place
     */
    void tick(float& l, float& r);
private:
    double m_sr = 48000.0;   ///< sample rate
    HilbertPair m_hl;   ///< the quadrature network, left
    HilbertPair m_hr;   ///< ... right
    double m_phase = 0.0;   ///< the carrier's phase, in cycles
    float m_hz = 0.0f;   ///< set(): the shift, Hz
    float m_mix = 0.0f;   ///< set(): the mix
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
    /**
     * @brief Prepares for @p sampleRate (allocates the delay lines, where there are any).
     * @param[in] sampleRate  the sample rate
     */
    void prepare(double sampleRate);
    /** @brief Clears every state: silence in, silence out. */
    void reset();
    /**
     * @brief Reads the effective psyfx parameters (indexed by psyfx::).
     * @param[in] v  the effective parameter values
     */
    void update(const float* v);
    /**
     * @brief Extra shift in Hz and extra flanger mix added on top of the parameters (per event).
     * @param[in] shiftHz     the extra shift in Hz
     * @param[in] flangerMix  the extra flanger mix
     */
    void setMotion(float shiftHz, float flangerMix);
    /**
     * @brief The event motion's shift in force (tests).
     * @return the shift in Hz
     */
    float motionHz() const { return m_motionHz; }
    /**
     * @brief The event motion's extra flanger mix in force (tests).
     * @return the extra mix
     */
    float motionFlange() const { return m_motionFlange; }
    /**
     * @brief Processes @p n stereo samples in place.
     * @param[in,out] L      the stereo signal, processed in place
     * @param[in,out] R      the stereo signal, processed in place
     * @param[in]     n      samples
     * @param[in]     beat0  absolute beat of the first sample
     * @param[in]     bps    beats per sample
     */
    void process(float* L, float* R, int n, double beat0, double bps);
private:
    /** @brief Hands the parameters and the event motion to the three effects. */
    void apply();
    Flanger m_flanger;   ///< first in the chain
    Phaser m_phaser;   ///< second
    FreqShifter m_shifter;   ///< third
    float m_v[psyfx::COUNT] = {};   ///< the psyfx parameters as update() read them (sized by the table, as Poly's since 25.09.2026)
    float m_motionHz = 0.0f;   ///< setMotion(): the shift's motion, Hz
    float m_motionFlange = 0.0f;   ///< setMotion(): the flanger's motion
};

/** @brief Buffer repeat of a stereo bus for the length of an event (the glitch of the psychedelic layer). */
class Stutter {
public:
    /**
     * @brief Prepares for @p sampleRate (allocates the delay lines, where there are any).
     * @param[in] sampleRate  the sample rate
     */
    void prepare(double sampleRate);
    /** @brief Clears every state: silence in, silence out. */
    void reset();
    /**
     * @brief Starts a repeat.
     * @param[in] lengthSamples  how long the repeat replaces the live signal
     * @param[in] sliceSamples   the slice that is repeated (it halves over the last quarter)
     */
    void trigger(int lengthSamples, int sliceSamples);
    /**
     * @brief True while a repeat is running.
     * @return true while a repeat runs
     */
    bool active() const { return m_on; }
    /**
     * @brief One stereo sample: records it and returns the live sample or the repeat in its place.
     * @param[in,out] l  the left sample, in place
     * @param[in,out] r  the right sample, in place
     */
    void tick(float& l, float& r);
private:
    std::vector<float> m_recL;   ///< the recorded live signal, left
    std::vector<float> m_recR;   ///< ... right
    bool m_on = false;   ///< a repeat is running
    long long m_pos = 0;   ///< samples into the repeat
    long long m_length = 0;   ///< its length, samples
    long long m_slice = 1;   ///< the slice repeated, samples
    int m_fade = 48;   ///< crossfade at the slice's seams, samples (1 ms)
};

} // namespace phos
