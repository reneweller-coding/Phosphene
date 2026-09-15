/**
 * @file Acid.h
 * @brief The acid voice: a monophonic saw through a resonant diode ladder, with accent, slide and the
 *        psytrance squelch.
 *
 * Signal path:
 * @code
 *   glide -> PolyBLEP saw/pulse (2 fs) -> diode ladder (2 fs) -> half-band -> [tuned comb] -> VCA
 *         -> ADAA tanh drive -> 24 dB low cut (>= 150 Hz) -> level -> centre + tempo delay
 * @endcode
 *
 * The behaviour follows the TB-303 as described in the circuit analyses collected around Open303
 * (R. Schmidt) and the service notes, not a copy of any code:
 *  - **Filter envelope.** Every retriggered note restarts an exponential decay that opens the cutoff
 *    by Env Amount octaves.
 *  - **Accent.** An accented note is louder, its filter envelope is deeper and shorter (at most
 *    200 ms), and it charges the "accent sweep" capacitor: a smoothed pulse that adds its own upward
 *    sweep of the cutoff and decays over about 300 ms. Consecutive accents charge it before it has
 *    discharged, so their sweeps climb -- the "wow" of an accented run. Its depth grows with the
 *    resonance, as on the instrument, where the accent circuit feeds the resonance-dependent part of
 *    the filter.
 *  - **Slide.** A note with the slide flag glides into the next one: the pitch approaches the new note
 *    exponentially with the slide time, and neither envelope restarts (legato). The composer writes a
 *    sliding note long enough to overlap the next.
 *  - **Squelch** (the "liquid" acid of Hallucinogen and Cosmosis). The cutoff starts Squelch Start
 *    times higher at every retriggered note and falls back with Squelch Time, while a feedback comb
 *    tuned to the note's period adds a pitched, vowel-like ring. A comb tuned to the period has its
 *    peaks on the harmonics, so the output is scaled by (1 - feedback) to keep the harmonics at their
 *    level. The numbers are design values: a measurement on the reference tracks found no detectable
 *    sweeps in the finished mixes (Tools/ref_sweeps.py, negative result of 15.09.2026).
 *
 * **Depth rule.** The low cut never goes below 150 Hz, and the composer keeps acid lines at or above
 * D3 (147 Hz): under 140 Hz only kick and bass may play.
 */
#pragma once
#include "phos/Adaa.h"
#include "phos/DiodeLadder.h"
#include "phos/Dsp.h"
#include "phos/Halfband.h"
#include "phos/Oscillator.h"
#include "phos/TempoDelay.h"
#include <vector>

namespace phos {

/** @brief Monophonic acid synthesizer. */
class Acid {
public:
    /** @brief Prepares for a sample rate. */
    void prepare(double sampleRate);
    /**
     * @brief Chooses the rate the oscillator and the diode ladder run at (Quality.h).
     *
     * 2 is the default: oscillator and ladder run at twice the sample rate, a half-band decimator
     * brings the result back. 1 skips both and costs about half; the cutoff ceiling then comes from
     * stability (0.45 fs) instead of the decimator's passband, which at 48 kHz is the same 18 kHz
     * limit, so the sound keeps its range. Call it after prepare() and before the first update().
     * @param factor 1 or 2; anything else is clamped into that range
     */
    void setOversampling(int factor);
    /** @brief The factor set by setOversampling(). */
    int oversampling() const { return os_; }
    /** @brief Silences and clears all state. */
    void reset();
    /** @brief Reads the effective parameters (indexed by acid::) and the tempo (for the delay). */
    void update(const float* v, double bpm);
    /**
     * @brief Starts a note.
     * @param pitch       MIDI note
     * @param velocity    0..1
     * @param accent      accented step
     * @param slide       this note glides into the next one
     * @param gateSamples samples until the gate closes
     * @param late        how many samples ago the note ideally started (0 <= late < 1)
     */
    void noteOn(int pitch, float velocity, bool accent, bool slide, int gateSamples, double late);
    /** @brief Renders @p n stereo samples, replacing @p L and @p R. */
    void process(float* L, float* R, int n);
    /** @brief Whether the amplitude envelope is open. */
    bool active() const { return amp_.isActive(); }
    /** @brief Pitch the oscillator plays now, in MIDI note numbers (fractional while gliding). */
    double currentPitch() const { return pitchNow_; }
    /** @brief Charge of the accent sweep capacitor, 0..1. */
    float accentSweep() const { return sweep_; }
    /** @brief Whether the last note started legato (slid into). */
    bool lastLegato() const { return legato_; }

private:
    double sr_ = 48000.0;
    int    os_ = 2;                  ///< oscillator/ladder rate as a multiple of sr_ (1 or 2)
    double osRate_ = 96000.0;        ///< sr_ * os_, the rate the voice runs at
    double nyqFactor_ = 0.2;         ///< cutoff ceiling as a fraction of osRate_: decimator passband at 2x, stability at 1x
    VaOscillator osc_;
    DiodeLadderT<float> ladder_;
    HalfbandDown<float> down_;
    Envelope amp_;
    TanhAdaa shaper_;
    Svf lc1_, lc2_;
    TempoDelay delay_;
    std::vector<float> comb_;
    size_t combPos_ = 0;
    std::vector<float> mono_, send_;

    // Note state.
    double pitchNow_ = 57.0, pitchTarget_ = 57.0;
    float  velocity_ = 1.0f;
    bool   accent_ = false, slidePending_ = false, legato_ = false;
    int    gate_ = 0;
    float  fenv_ = 0.0f, fDecayNote_ = 0.999f, sq_ = 0.0f;
    float  pulse_ = 0.0f, sweep_ = 0.0f;
    float  accentGain_ = 1.0f, accentGainTarget_ = 1.0f;

    // Settings from update().
    float wave_ = 0.0f, cutoff_ = 420.0f, k_ = 10.0f, envOct_ = 2.6f, accentAmt_ = 0.6f, keyTrack_ = 0.5f;
    float fDecay_ = 0.999f, fDecayAccent_ = 0.999f, glide_ = 0.001f, driveIn_ = 1.0f, driveOut_ = 1.0f;
    bool  squelch_ = false;
    float sqOct_ = 3.5f, sqDecay_ = 0.999f, combMix_ = 0.5f, combFb_ = 0.8f;
    float pulseDecay_ = 0.999f, sweepCharge_ = 0.001f, level_ = 0.3f, sendAmt_ = 0.25f, resonance_ = 0.7f;
    float ampDecay_ = 0.9f, hz_ = 220.0f, accentSmooth_ = 0.01f;
};

} // namespace phos
