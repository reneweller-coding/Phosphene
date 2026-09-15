/**
 * @file Kick.h
 * @brief The kick drum: two synthesis engines, saturation, click, tuning to the key.
 *
 * **Sweep engine.** A sine whose frequency falls exponentially from a start to an end pitch,
 * f(t) = f_end + (f_start - f_end) ((1 - p) e^(-t/tau) + p e^(-t/(0.3 tau))), where the punch p
 * weights a second, faster segment that sharpens the attack. Amplitude: linear attack, hold, then
 * exponential decay (linear in dB). This is the canonical construction of the psytrance kick and
 * the one its parameters are usually described in.
 *
 * **Resonant engine.** A decaying resonator instead of an oscillator. A trigger adds energy to the
 * resonator's state rather than restarting it, so a kick that arrives while the previous one still
 * rings sums with it the way the TR-808's bridged-T bass drum does. The behaviour follows the
 * analysis of Werner, Abel and Smith, "A physically-informed, circuit-bendable, digital model of the
 * Roland TR-808 bass drum circuit", DAFx 2014; the circuit itself is not modelled component by
 * component.
 *
 * The resonator is a damped rotating phasor, z <- r e^(i w) z, output Im(z): the "phasor filter" of
 * Mathews and Smith, "Methods for synthesizing very high Q parametrically well behaved two pole
 * filters", SMAC 2003. Its amplitude is independent of its tuning, so the pitch envelope can sweep
 * it from several hundred hertz down to the fundamental without the level collapsing -- which is what
 * a state-variable filter does under the same sweep (measured with a 330 -> 50 Hz sweep and a 1.5 s
 * decay: the SVF lost 8.2 dB by 100..200 ms where the decay alone accounts for 4; the phasor loses
 * exactly the 4). The damping r gives -60 dB over the decay time.
 *
 * **Common tail.** Click (a filtered noise burst plus an impulse, for the beater), saturation with
 * first-order ADAA (tanh or hard clip, Adaa.h), a tone low pass and a DC blocker.
 *
 * **Tuning.** With Tune = Key, the end pitch is moved to the root or the fifth of the key --
 * whichever octave-equivalent lies closest to the Pitch End setting -- so the kick's fundamental
 * never beats against the bass.
 *
 * **Retrigger.** A new trigger hands the sounding voice to a fade slot that ramps it out over 3 ms
 * while the new voice starts at phase zero: no click, no gap.
 */
#pragma once
#include "phos/Adaa.h"
#include "phos/Dsp.h"

namespace phos {

class ParamStore;

/** @brief The kick drum synthesizer (one voice plus a fade slot). */
class Kick {
public:
    /** @brief Prepares for a sample rate. */
    void prepare(double sampleRate);
    /** @brief Silences and clears all state. */
    void reset();
    /**
     * @brief Reads the parameters.
     * @param p       parameter store
     * @param base    base id of the kick module
     * @param keyRoot pitch class of the key (0 = C), used when Tune = Key
     * @param scaleHasFifth whether the scale's fifth is perfect (it is in every scale Phosphene knows)
     */
    void update(const ParamStore& p, int base, int keyRoot, bool scaleHasFifth = true);
    /** @brief Starts a kick now. @param velocity 0..1 */
    void trigger(float velocity);
    /** @brief Renders @p n samples, replacing @p out. */
    void process(float* out, int n);
    /** @brief The end pitch actually used, after tuning. */
    float tunedEndHz() const { return endHz_; }
    /** @brief Whether anything is sounding. */
    bool active() const { return voice_.active || fade_.active; }

    /**
     * @brief The octave-equivalent of the root or fifth nearest to @p targetHz.
     * @param keyRoot pitch class of the root
     * @param targetHz where the end pitch should be near
     */
    static float tuneToKey(int keyRoot, float targetHz);

private:
    struct Voice {
        bool   active = false;
        double phase = 0.0;
        double e1 = 0.0, e2 = 0.0;        ///< the two pitch-envelope segments, 1 -> 0
        int    t = 0;                     ///< samples since the trigger
        float  amp = 0.0f;                ///< amplitude envelope
        float  click = 0.0f;              ///< click envelope
        float  velocity = 1.0f;
        double zRe = 0.0, zIm = 0.0;      ///< the resonant engine's phasor
    };

    float voiceSample(Voice& v);

    double sr_ = 48000.0;
    Voice  voice_, fade_;
    float  fadeGain_ = 0.0f, fadeStep_ = 0.0f;

    // Settings from update().
    int    engine_ = 0;
    float  endHz_ = 50.0f, startHz_ = 330.0f, punch_ = 0.5f;
    double d1_ = 0.999, d2_ = 0.99;       ///< per-sample decay of the pitch segments
    int    attackSamples_ = 10, holdSamples_ = 1680;
    float  ampDecay_ = 0.9999f;           ///< per-sample factor, -60 dB over the decay time
    double resDamping_ = 0.9999;          ///< phasor radius per sample, -60 dB over the decay time
    float  drive_ = 2.0f, driveNorm_ = 1.0f;
    int    clip_ = 0;
    float  clickLevel_ = 0.2f, clickDecay_ = 0.99f;
    float  level_ = 1.0f;

    Rng          noise_;
    Svf          clickFilter_, toneFilter_;
    TanhAdaa     tanh_;
    HardClipAdaa hard_;
    DcBlocker    dc_;
};

} // namespace phos
