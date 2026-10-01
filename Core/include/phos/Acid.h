/**
 * @file Acid.h
 * @brief The acid voice: a monophonic saw through a resonant diode ladder, with accent, slide and the
 *        psytrance squelch.
 *
 * Signal path:
 * @code
 *   glide -> PolyBLEP saw/pulse (2 fs) -> diode ladder (2 fs) -> half-band -> [tuned comb] -> VCA
 *         -> ADAA tanh drive -> 24 dB low cut (>= 150 Hz) -> [disperser] -> level
 *         -> centre + tempo delay
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
 *    sweeps in the finished mixes (Tools/ref_sweeps.py, negative result of 15.09.2026). The delay
 *    line is read with **third-order Lagrange interpolation**, not linear; combTaps() carries the
 *    measurement that chose it.
 *  - **Disperser** (Disperser.h). A chain of second-order all-passes turns the attack into a short
 *    downward chirp -- the "pew" of a modern psytrance stab -- without touching the magnitude of any
 *    band. Off by default (acid.disperse = 0 sections), because no measurement asks for it to be on.
 *
 * **Depth rule.** The low cut never goes below 150 Hz, and the composer keeps acid lines at or above
 * D3 (147 Hz): under 140 Hz only kick and bass may play.
 */
#pragma once
#include "phos/Modulation.h"
#include "phos/Adaa.h"
#include "phos/DiodeLadder.h"
#include "phos/Disperser.h"
#include "phos/Dsp.h"
#include "phos/Halfband.h"
#include "phos/Oscillator.h"
#include "phos/TempoDelay.h"
#include <vector>

namespace phos {

/**
 * @brief The four weights of a third-order Lagrange fractional delay, for the fraction @p x in [0, 1).
 *
 * The taps belong to the samples at offsets -1, 0, +1, +2 from the integer part, and the weights are
 * the Lagrange basis polynomials on the nodes (-1, 0, 1, 2) evaluated at @p x. At x = 0 the weights
 * are (0, 1, 0, 0) and at x = 1 they are (0, 0, 1, 0), so the interpolator reproduces the integer
 * delays exactly and never introduces a gain step as the tuning crosses a sample.
 *
 * **Why this and not linear interpolation, and why not an all-pass.** Linear interpolation is a
 * two-tap low pass whose damping depends on the fraction, so a comb tuned to the note period loses
 * its top by an amount that changes with the note -- exactly where the resonance is supposed to be
 * sharpest. Measured at 48 kHz for a fraction of 0.5: linear loses 0.47 dB at 5 kHz and 2.01 dB at
 * 10 kHz, and with the default feedback of 0.82 that pulls the comb's resonance peak from 14.89 dB
 * down to 13.01 dB and 9.13 dB. The third-order Lagrange interpolator (Laakso, Valimaki, Karjalainen
 * and Laine, "Splitting the unit delay -- tools for fractional delay filter design", IEEE Signal
 * Processing Magazine 13(1), 1996, section on Lagrange interpolation, where it is the maximally flat
 * FIR fractional-delay filter) loses 0.04 dB and 0.53 dB instead, for peaks of 14.73 dB and 12.81 dB
 * against an ideal 14.89 dB.
 *
 * The literature's own preference in that same paper is the first-order all-pass, whose magnitude is
 * exactly 1. It is not used here, and the reason is a trade rather than a defect:
 *  - What it would buy is small where it matters. Against Lagrange it is 0.00 dB better at 2 kHz,
 *    0.13 dB at 5 kHz and 2.31 dB at 10 kHz -- and at 10 kHz an acid note has been through a
 *    four-pole ladder whose cutoff is a few hundred hertz.
 *  - What it costs is state, in a comb that is retuned at *every note*. Its coefficient
 *    a = (1 - frac)/(1 + frac) approaches 1 as the fraction approaches zero, which puts its pole on
 *    the unit circle at z = -1, and the filter then stops forgetting. Measured in the self test
 *    (section "acid colour"): 100 ms after a retune, with the input taken away and the comb itself
 *    136 dB down, the all-pass version still stands at -108 dB of the steady state where the Lagrange
 *    version is at -184 dB. That residue is far too quiet to hear; what it shows is that the state
 *    would have to be reset or crossfaded at every note, and a stateless interpolator that already
 *    removes 91 % of the linear error at 5 kHz makes that machinery pointless.
 */
inline void combTaps(float x, float& wm1, float& w0, float& w1, float& w2)
{
    const float xm1 = x - 1.0f, xm2 = x - 2.0f, xp1 = x + 1.0f;
    wm1 = -x * xm1 * xm2 * (1.0f / 6.0f);
    w0 = xp1 * xm1 * xm2 * 0.5f;
    w1 = -xp1 * x * xm2 * 0.5f;
    w2 = xp1 * x * xm1 * (1.0f / 6.0f);
}

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
    /** @brief The set's beat at the next sample and the beats per sample (Engine.cpp, per chunk): the synced LFOs' clock. */
    void setClock(double beat, double beatsPerSample) { beat_ = beat; beatsPerSample_ = beatsPerSample; }
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
    /**
     * @brief Closes the gate of the note at @p pitch, if it is the one sounding (23.09.2026, live keyboard): a
     *        played note starts with a gate that never runs out on its own, and the key's release ends it.
     */
    void noteOff(int pitch) { if (gate_ > 0 && static_cast<int>(pitchTarget_ + 0.5) == pitch) gate_ = 1; }
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

    /**
     * @brief The modulation to show on the knobs (26.09.2026, the live ring): the sums per destination (ModDest) of the
     *        modulator, or null where nothing moves. Rendering thread.
     */
    const float* displayModulation() const { return modOn_ && mod_.active() ? modSum_ : nullptr; }
private:
    double sr_ = 48000.0;   ///< output sample rate
    int    os_ = 2;                  ///< oscillator/ladder rate as a multiple of sr_ (1 or 2)
    double osRate_ = 96000.0;        ///< sr_ * os_, the rate the voice runs at
    double nyqFactor_ = 0.2;         ///< cutoff ceiling as a fraction of osRate_: decimator passband at 2x, stability at 1x
    VaOscillator osc_;   ///< saw-to-square oscillator
    DiodeLadderT<float> ladder_;   ///< the 303's diode ladder
    HalfbandDown<float> down_;   ///< back from the oversampled rate
    Envelope amp_;   ///< amplitude envelope
    TanhAdaa shaper_;   ///< the drive stage
    Svf lc1_;   ///< the low cut (acid.low_cut): its first section
    Svf lc2_;   ///< ... its second
    Disperser disperse_;             ///< all-pass chain coefficients (Disperser.h)
    DisperserChannel dispState_;     ///< its state; the acid is mono until the delay
    TempoDelay delay_;   ///< the acid's own tempo delay
    std::vector<float> comb_;   ///< the squelch's feedback comb, tuned to the note
    size_t combPos_ = 0;   ///< its write position
    std::vector<float> mono_;   ///< a block's dry signal
    std::vector<float> send_;   ///< a block's delay send

    /// Note state.
    double pitchNow_ = 57.0, pitchTarget_ = 57.0;   ///< MIDI pitch now and where a slide goes
    float  velocity_ = 1.0f;   ///< the note's velocity, 0..1
    bool   accent_ = false;   ///< the note's accent
    bool   slidePending_ = false;   ///< the note slides into the next
    bool   legato_ = false;   ///< a slid (legato) note
    int    gate_ = 0;   ///< samples until the note is released
    float  fenv_ = 0.0f;   ///< the filter envelope
    float  fDecayNote_ = 0.999f;   ///< its per-sample decay for this note
    float  sq_ = 0.0f;   ///< the squelch envelope
    float  pulse_ = 0.0f;   ///< the accent pulse
    float  sweep_ = 0.0f;   ///< the accent sweep capacitor it charges
    float  accentGain_ = 1.0f;   ///< the accent's level
    float  accentGainTarget_ = 1.0f;   ///< ... its target

    // Settings from update().
    float wave_ = 0.0f;   ///< the waveform
    float cutoff_ = 420.0f;   ///< the cutoff, Hz
    float k_ = 10.0f;   ///< the ladder's feedback
    float envOct_ = 2.6f;   ///< the envelope's depth, octaves
    float accentAmt_ = 0.6f;   ///< the accent's amount
    float keyTrack_ = 0.5f;   ///< the key tracking
    float fDecay_ = 0.999f;   ///< the filter's per-sample decay
    float fDecayAccent_ = 0.999f;   ///< ... on an accented note
    float glide_ = 0.001f;   ///< the slide's coefficient
    float driveIn_ = 1.0f;   ///< the drive into the filter
    float driveOut_ = 1.0f;   ///< ... and the gain after it
    bool  squelch_ = false;   ///< acid.squelch
    float sqOct_ = 3.5f;   ///< the squelch's start, octaves
    float sqDecay_ = 0.999f;   ///< the squelch's per-sample decay
    float combMix_ = 0.5f;   ///< the comb's mix
    float combFb_ = 0.8f;   ///< the comb's feedback
    float pulseDecay_ = 0.999f;   ///< the accent pulse's decay
    float sweepCharge_ = 0.001f;   ///< the sweep's charge rate
    float level_ = 0.3f;   ///< the level
    float sendAmt_ = 0.25f;   ///< the delay send
    float resonance_ = 0.7f;   ///< the resonance
    float ampDecay_ = 0.9f;   ///< the amplitude's decay
    float hz_ = 220.0f;   ///< the frequency sounding
    float accentSmooth_ = 0.01f;   ///< the accent gain's smoothing
    /** @name The acid's own modulation (26.09.2026, Modulation.h), every 16 samples on the absolute count
     *  @{ */
    Modulator mod_;                       ///< envelope, LFOs, matrix
    float modSum_[kModDests] = {};        ///< the sums per destination
    bool  modOn_ = false;                 ///< some slot reaches something
    float noteRand_ = 0.0f;               ///< the Random source, per note
    Rng   modRng_;                        ///< its stream
    uint64_t modPos_ = 0;                 ///< samples since reset (the evaluation grid)
    double beat_ = 0.0, beatsPerSample_ = 0.0;   ///< setClock()
    float cutMod_ = 0.0f, pitchMod_ = 0.0f, kMod_ = 0.0f;   ///< cutoff octaves, pitch semitones, feedback as modulated
    float gainL_ = 1.0f, gainR_ = 1.0f;   ///< the level and pan destinations
    /** @} */
};

} // namespace phos
