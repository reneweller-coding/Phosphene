/**
 * @file Kick.h
 * @brief The kick drum: two engines, closed-form phase, sub-sample onsets, phase lock and tail limit.
 *
 * **Sweep engine.** A sine chirp y(t) = A(t) sin(2 pi phi(t)). The frequency falls from a start to an
 * end pitch along two exponential segments -- a fast punch segment (tau_1, a few ms) that makes the
 * attack and a slower body segment (tau_2, tens of ms) that settles onto the fundamental:
 *
 *   f(t)   = f_e + (f_s - f_e) ((1 - p) e^(-t/tau_2) + p e^(-t/tau_1))
 *   phi(t) = f_e t + (f_s - f_e) ((1 - p) tau_2 (1 - e^(-t/tau_2)) + p tau_1 (1 - e^(-t/tau_1)))
 *
 * phi is the exact integral of f, evaluated in closed form at every sample: no integration error
 * accumulates, and a kick that should have started 0.37 samples before the sample grid simply begins
 * at t = 0.37 / fs. Amplitude: linear attack, hold, exponential decay (linear in dB).
 *
 * **Resonant engine.** A damped rotating phasor z <- r e^(i 2 pi f(t)/fs) z, output Im(z) (Mathews and
 * Smith, "Methods for synthesizing very high Q parametrically well behaved two pole filters", SMAC
 * 2003). Its amplitude does not depend on its tuning, so the same pitch envelope can sweep it. A
 * trigger adds energy to the ring instead of restarting it, which is how the TR-808's bridged-T bass
 * drum behaves (Werner, Abel and Smith, DAFx 2014); the circuit is not modelled part by part.
 *
 * **Latching.** Every trigger freezes the pitch and amplitude settings for that kick. A parameter
 * ramp or a phase trim therefore acts on the next kick, never inside one, where a changed tau in the
 * closed-form phase would be a phase jump.
 *
 * **Output phase.** outputPhaseAt() returns the phase of the kick as it leaves the module at time t:
 * phi(t) plus the phase responses of the chain at the instantaneous frequency -- half a sample of
 * delay for the first-order ADAA saturator, the trapezoidal low pass at Tone, the DC blocker. In the
 * linear range a memoryless odd saturator adds no phase, and the kick's tail, which is what the
 * phase lock cares about, is in that range.
 *
 * **Phase lock ("kick follows bass").** setPhaseTarget() asks that at time T after the trigger the
 * kick's output phase be a whole number of cycles away from a target. The kick trims its body time
 * constant tau_2 to the nearest solution: d phi/d tau_2 = (f_s - f_e)(1 - p)(1 - (1 + T/tau_2) e^(-T/tau_2))
 * is positive, so phi(T) is monotonic in tau_2 and a bisection finds the root. At the default sound
 * one cycle costs about 7 ms of tau_2, so the trim stays within +-3.5 ms.
 *
 * **Tail limit.** constrainTail() shortens hold and decay so that at the first bass slot the kick's
 * output is at least Tail Limit below its peak. The saturation lifts a small tail by its small-signal
 * gain relative to full scale -- g / tanh(g) for tanh, max(g, 1) for the hard clip -- and the
 * constraint accounts for that lift (without it the default kick measured 9.4 dB louder in its tail
 * than its envelope said).
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
     * @brief Reads the effective parameter values.
     * @param v       values indexed by kick:: (after offsets, overrides and constraints)
     * @param keyRoot pitch class of the key (0 = C), used when Tune = Key
     */
    void update(const float* v, int keyRoot);
    /**
     * @brief Starts a kick.
     * @param velocity 0..1
     * @param late     how many samples ago the kick ideally started (0 <= late < 1)
     */
    void trigger(float velocity, double late = 0.0);
    /** @brief Renders @p n samples, replacing @p out. */
    void process(float* out, int n);

    /** @brief The end pitch actually used, after tuning. */
    float tunedEndHz() const { return endHz_; }
    /** @brief The body time constant the next kick uses, after the phase trim, in seconds. */
    double bodyTau() const { return tau2Trimmed_; }
    /** @brief Whether anything is sounding. */
    bool active() const { return voice_.active || fade_.active; }

    /**
     * @brief Output phase of a kick triggered with the current settings, @p t seconds after its start.
     * @return phase in cycles (not wrapped)
     */
    double outputPhaseAt(double t) const;
    /** @brief Instantaneous frequency of the current settings at @p t seconds. */
    double frequencyAt(double t) const;
    /**
     * @brief Trims tau_2 so the output phase at @p t is @p targetCycles modulo whole cycles.
     * @param t            seconds after the trigger (the first bass slot); <= 0 disables the trim
     * @param targetCycles wanted phase in cycles
     */
    void setPhaseTarget(double t, double targetCycles);

    /**
     * @brief Applies both amplitude constraints: the body floor, then the tail limit (which wins).
     *
     * Body floor: at least two periods of the end pitch lie above -20 dB, so a short recipe cannot
     * turn the kick into a click. The envelope falls 20 dB in a third of its decay, which gives
     * hold + decay/3 >= 2/f_end for the sweep engine and decay/3 >= 2/f_end for the resonator.
     * @param v           kick values indexed by kick:: (modified in place)
     * @param slotSeconds time from the kick to the first bass note
     * @param keyRoot     pitch class of the key, for the tuned end pitch
     */
    static void constrain(float* v, double slotSeconds, int keyRoot);
    /**
     * @brief Shortens hold and decay of a parameter set so the tail at @p slotSeconds stays under the limit.
     * @param v           kick values indexed by kick:: (modified in place)
     * @param slotSeconds time from the kick to the first bass note
     */
    static void constrainTail(float* v, double slotSeconds);

    /** @brief The octave-equivalent of the root or fifth nearest to @p targetHz. */
    static float tuneToKey(int keyRoot, float targetHz);

private:
    /** @brief Everything a kick needs, frozen at its trigger. */
    struct Shape {
        int    engine = 0;
        double fe = 50.0, fs = 330.0, tau1 = 0.004, tau2 = 0.022, punch = 0.5;
        double attack = 10.0, hold = 576.0;     ///< samples
        double decayRate = -1e-4;               ///< ln(amplitude) per sample after the hold
        double damping = 0.9999;                ///< resonant engine radius per sample
    };
    struct Voice {
        bool   active = false;
        Shape  s;
        double late = 0.0;                      ///< sub-sample start offset
        int    n = 0;                           ///< samples since the trigger
        double e1 = 1.0, e2 = 1.0;              ///< e^(-t/tau_1), e^(-t/tau_2)
        double d1 = 1.0, d2 = 1.0;              ///< their per-sample factors
        float  click = 0.0f;
        float  velocity = 1.0f;
        double zRe = 0.0, zIm = 0.0;            ///< resonant phasor
    };

    float voiceSample(Voice& v);
    Shape currentShape() const;
    double phaseWith(double t, double tau2) const;
    double chainPhase(double hz) const;

    double sr_ = 48000.0;
    Voice  voice_, fade_;
    float  fadeGain_ = 0.0f, fadeStep_ = 0.0f;

    // Settings from update().
    int    engine_ = 0;
    float  endHz_ = 50.0f, startHz_ = 330.0f, punch_ = 0.5f;
    double tau1_ = 0.004, tau2_ = 0.022, tau2Trimmed_ = 0.022;
    double attackSamples_ = 10.0, holdSamples_ = 576.0, decaySeconds_ = 0.15;
    float  drive_ = 2.0f, driveNorm_ = 1.0f;
    int    clip_ = 0;
    float  clickLevel_ = 0.2f, clickDecay_ = 0.99f;
    float  toneHz_ = 9000.0f;
    float  level_ = 1.0f;
    double lockT_ = 0.0, lockTarget_ = 0.0;
    double trimKey_[10] = { -1.0 };           ///< inputs of the last trim solve

    Rng          noise_;
    Svf          clickFilter_, toneFilter_;
    TanhAdaa     tanh_;
    HardClipAdaa hard_;
    DcBlocker    dc_;
};

} // namespace phos
