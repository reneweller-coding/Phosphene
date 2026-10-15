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
 * one cycle costs about 7 ms of tau_2, so the trim stays within +-3.5 ms. (Still true for the defaults
 * of 18.09.2026: 284 Hz of sweep at half punch and tau_2 = 13 ms give 142 cycles per second of tau_2,
 * 7.1 ms per cycle, inside the bisection's range of 6.5 .. 26 ms.)
 *
 * **Click (18.09.2026).** The click layer -- band-passed noise with a few milliseconds of decay --
 * joins the body *after* the saturator. Inside it, it rode on whatever the body was doing and was
 * flattened with it; outside it, it is the transient the ear reads as "punch". It does not touch the
 * phase lock: the lock is solved on the body's phase at the first bass slot, 100 ms after the click
 * has decayed by more than 100 dB, and the tail limit concerns the same late window.
 *
 * **The default sound against the references (18.09.2026).** `Tools/ref_kick.py` measured the kicks
 * of 24 of the 40 reference recordings where kick and bass play nearly alone, over the window from the
 * onset to the first bass slot: power under 60 Hz against 60 .. 120 Hz median -4.7 dB, the click band
 * 2 .. 5 kHz against 40 .. 120 Hz -27.7 dB, crest 7.1 dB. The kick before 19.09.2026 measured -7.0,
 * -38.7 and 6.0 there (little sub, no click). The defaults now sweep faster onto the fundamental
 * (Body Decay 22 -> 13 ms) from higher up (Pitch Start 220 -> 330 Hz), drive the body a little less
 * (0.35 -> 0.30) and carry the click at 0.5 after the saturator: -3.4, -27.9 and 8.2 dB in the self
 * test (testKickReference). The end pitch stays at 50 Hz, tuned to the key. The references' kicks
 * read 62 Hz in that window, but they are still falling there, so the number is not an end pitch and
 * gives no reason to move ours in either direction.
 *
 * **Tail limit.** constrainTail() shortens hold and decay so that at the first bass slot the kick's
 * output is at least Tail Limit below its peak. The saturation lifts a small tail by its small-signal
 * gain relative to full scale -- g / tanh(g) for tanh, max(g, 1) for the hard clip -- and the
 * constraint accounts for that lift (without it the default kick measured 9.4 dB louder in its tail
 * than its envelope said).
 *
 * **Kick body and the limit (19.09.2026).** The reference kicks stay within 20 dB of their peak up to
 * the first bass slot (Tools/ref_kick.py, median 104 ms, the end of its window); ours fell 20 dB by
 * 76 ms. The limit of -24 dB was not what cut it -- at the default decay of 150 ms it did not bind --
 * but it would have cut any decay long enough (at most 170 ms at 145 BPM). The default limit is now
 * -15 dB and the decay 240 ms, which the limit trims to about 235 ms: the body reads 113 ms by the
 * tool's definition. What the limit exists for still holds: in the first bass sixteenth the kick's tail
 * stays 15.8 dB under the bass in 30 .. 150 Hz (median over the first drop of the listening seed), and
 * with the lock the two meet in phase there. Neither the lock nor the trim range is touched: tau_2 is
 * the same, so one cycle still costs 7.1 ms of it.
 * @author Rene Weller
 */
#pragma once
#include "phos/Adaa.h"
#include "phos/Dsp.h"

namespace phos
{

class ParamStore;

/** @brief The kick drum synthesizer (one voice plus a fade slot). */
class Kick
{
public:
    /**
     * @brief Prepares for a sample rate.
     * @param[in] sampleRate  the sample rate in Hz
     */
    void prepare( double sampleRate );
    /** @brief Silences and clears all state. */
    void reset();
    /**
     * @brief Reads the effective parameter values.
     * @param[in] v        values indexed by kick:: (after offsets, overrides and constraints)
     * @param[in] keyRoot  pitch class of the key (0 = C), used when Tune = Key
     */
    void update( const float *v, int keyRoot );
    /**
     * @brief Starts a kick.
     * @param[in] velocity  0..1
     * @param[in] late      how many samples ago the kick ideally started (0 <= late < 1)
     */
    void trigger( float velocity, double late = 0.0 );
    /**
     * @brief Renders @p n samples, replacing @p out.
     * @param[out] out  the samples
     * @param[in]  n    the block's samples
     */
    void process( float *out, int n );

    /**
     * @brief The end pitch actually used, after tuning.
     * @return the end pitch in Hz after tuning
     */
    float tunedEndHz() const { return m_endHz; }
    /**
     * @brief The body time constant the next kick uses, after the phase trim, in seconds.
     * @return the body time constant in seconds
     */
    double bodyTau() const { return m_tau2Trimmed; }
    /**
     * @brief Whether anything is sounding.
     * @return true while anything sounds
     */
    bool active() const { return m_voice.m_active || m_fade.m_active; }

    /**
     * @brief Output phase of a kick triggered with the current settings, @p t seconds after its start.
     * @param[in] t  seconds after the kick's ideal start
     * @return phase in cycles (not wrapped)
     */
    double outputPhaseAt( double t ) const;
    /**
     * @brief Instantaneous frequency of the current settings at @p t seconds.
     * @param[in] t  seconds after the kick's start
     * @return the frequency in Hz
     */
    double frequencyAt( double t ) const;
    /**
     * @brief Trims tau_2 so the output phase at @p t is @p targetCycles modulo whole cycles.
     * @param[in] t             seconds after the trigger (the first bass slot); <= 0 disables the trim
     * @param[in] targetCycles  wanted phase in cycles
     */
    void setPhaseTarget( double t, double targetCycles );

    /**
     * @brief Applies both amplitude constraints: the body floor, then the tail limit (which wins).
     *
     * Body floor: at least two periods of the end pitch lie above -20 dB, so a short recipe cannot
     * turn the kick into a click. The envelope falls 20 dB in a third of its decay, which gives
     * hold + decay/3 >= 2/f_end for the sweep engine and decay/3 >= 2/f_end for the resonator.
     * @param[in,out] v            kick values indexed by kick:: (modified in place)
     * @param[in]     slotSeconds  time from the kick to the first bass note
     * @param[in]     keyRoot      pitch class of the key, for the tuned end pitch
     */
    static void constrain( float *v, double slotSeconds, int keyRoot );
    /**
     * @brief Shortens hold and decay of a parameter set so the tail at @p slotSeconds stays under the limit.
     * @param[in,out] v            kick values indexed by kick:: (modified in place)
     * @param[in]     slotSeconds  time from the kick to the first bass note
     */
    static void constrainTail( float *v, double slotSeconds );

    /**
     * @brief The octave-equivalent of the root or fifth nearest to @p targetHz.
     * @param[in] keyRoot   the key's pitch class
     * @param[in] targetHz  the frequency aimed at
     * @return the frequency in Hz
     */
    static float tuneToKey( int keyRoot, float targetHz );

private:
    /** @brief Everything a kick needs, frozen at its trigger. */
    struct Shape
    {
        int    m_engine = 0;          ///< kick.engine
        double m_fe = 50.0;           ///< end pitch, Hz
        double m_fs = 330.0;          ///< start pitch, Hz
        double m_tau1 = 0.004;        ///< the punch's sweep time constant, s
        double m_tau2 = 0.022;        ///< the body's sweep time constant, s
        double m_punch = 0.5;         ///< the punch's share
        double m_attack = 10.0;       ///< the attack, samples
        double m_hold = 576.0;        ///< the hold, samples
        double m_decayRate = -1e-4;   ///< ln(amplitude) per sample after the hold
        double m_damping = 0.9999;    ///< resonant engine radius per sample
    };
    /** @brief One sounding kick: its shape, its envelopes and where it stands. */
    struct Voice
    {
        bool   m_active = false;    ///< sounding
        Shape  m_s;                 ///< the shape it was triggered with
        double m_late = 0.0;        ///< sub-sample start offset
        int    m_n = 0;             ///< samples since the trigger
        double m_e1 = 1.0;          ///< e^(-t/tau_1)
        double m_e2 = 1.0;          ///< e^(-t/tau_2)
        double m_d1 = 1.0;          ///< e1's per-sample factor
        double m_d2 = 1.0;          ///< e2's per-sample factor
        float  m_click = 0.0f;      ///< the click layer's envelope
        float  m_velocity = 1.0f;   ///< 0..1
        double m_zRe = 0.0;         ///< the resonant phasor's real part
        double m_zIm = 0.0;         ///< the resonant phasor's imaginary part
    };

    /**
     * @brief The body of one voice for one sample; the click layer's sample goes to @p click.
     * @param[in,out] v      the voice
     * @param[out]    click  receives the click layer's sample
     * @return the body's sample
     */
    float voiceSample( Voice &v, float &click );
    /**
     * @brief The shape a kick triggered now would get, from the current parameters.
     * @return the shape a kick triggered now would get
     */
    Shape currentShape() const;
    /**
     * @brief The sweep's phase at @p t seconds with body time constant @p tau2, in cycles.
     * @param[in] t     seconds after the kick's start
     * @param[in] tau2  the body time constant in seconds
     * @return the sweep's phase in cycles
     */
    double phaseWith( double t, double tau2 ) const;
    /**
     * @brief The phase the output chain (ADAA, tone filter) adds at @p hz, in cycles.
     * @param[in] hz  the frequency in Hz
     * @return the phase the chain adds there, in cycles
     */
    double chainPhase( double hz ) const;

    double m_sr = 48000.0;      ///< sample rate
    Voice  m_voice;             ///< the sounding kick
    Voice  m_fade;              ///< the kick fading out under it
    float  m_fadeGain = 0.0f;   ///< the fading kick's gain
    float  m_fadeStep = 0.0f;   ///< its per-sample step

    // Settings from update().
    int    m_engine = 0;               ///< kick.engine
    float  m_endHz = 50.0f;            ///< end pitch (tuned), Hz
    float  m_startHz = 330.0f;         ///< start pitch, Hz
    float  m_punch = 0.5f;             ///< the punch's share
    double m_tau1 = 0.004;             ///< the punch's time constant, s
    double m_tau2 = 0.022;             ///< the body's time constant, s
    double m_tau2Trimmed = 0.022;      ///< the body's time constant after the phase trim, s
    double m_attackSamples = 10.0;     ///< the amplitude envelope's attack, samples
    double m_holdSamples = 576.0;      ///< its hold, samples
    double m_decaySeconds = 0.15;      ///< its decay, seconds
    float  m_drive = 2.0f;             ///< drive into the clip
    float  m_driveNorm = 1.0f;         ///< the gain that keeps the level after the clip
    int    m_clip = 0;                 ///< kick.clip: soft (tanh) or hard
    float  m_clickLevel = 0.2f;        ///< the click layer's level
    float  m_clickDecay = 0.99f;       ///< the click layer's per-sample decay
    float  m_toneHz = 9000.0f;         ///< the tone low pass
    float  m_level = 1.0f;             ///< kick.level, linear
    double m_lockT = 0.0;              ///< the phase lock: seconds from the trigger to the slot
    double m_lockTarget = 0.0;         ///< the phase lock: the phase wanted at the slot, cycles
    double m_trimKey[10] = { -1.0 };   ///< inputs of the last trim solve

    Rng          m_noise;         ///< the click's noise
    Svf          m_clickFilter;   ///< the click's band
    Svf          m_toneFilter;    ///< the tone low pass
    TanhAdaa     m_tanh;          ///< the soft clip
    HardClipAdaa m_hard;          ///< the hard clip
    DcBlocker    m_dc;            ///< after the clip
};

}   // namespace phos
