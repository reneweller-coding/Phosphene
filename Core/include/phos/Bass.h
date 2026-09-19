/**
 * @file Bass.h
 * @brief The rolling psytrance bass: a phase-locked sine sub under a retriggered, filtered VA voice.
 *
 * Signal path per note:
 * @code
 *   PolyBLEP saw/pulse (2 fs) -> drive -> nonlinear ZDF ladder + filter envelope --+
 *                             -> bite: tanh -> 4-pole low pass + its envelope -----+-> half-band -> [high pass] --+
 *                                                                                                              +-> amp ADSR -> duck -> level
 *   sine at the fundamental (fs, same phase as the saw's fundamental) -> sub level ----------------------------------+
 * @endcode
 *
 * **Why the sub is separate.** A resonant low pass whose cutoff is swept by a fast envelope shifts
 * the phase of everything it passes, and the fundamental most of all while the cutoff falls towards
 * it: measured on the default sound, the fundamental's phase moved by -34 degrees during the first
 * 22 ms of every note. That is exactly the window in which the kick's tail and the bass overlap.
 * The fundamental is therefore generated as a pure sine that no filter touches, and in Split mode
 * the filtered voice is high-passed so that it carries the overtones only: two cascaded Butterworth
 * sections at Split x f0 form a fourth-order Linkwitz-Riley high pass (-6 dB at the corner,
 * 24 dB/octave), which at the default ratio of 2 leaves the filtered path's fundamental 24.6 dB
 * down. Mixed mode adds the sub without splitting.
 *
 * **One phase for the note.** The saw and the sub start with the same fundamental phase, and the
 * engine chooses it: the Start Phase knob (0.5 is the saw's zero crossing, fundamental phase 0), or
 * the kick's phase at the first bass slot when the kick lock says the bass follows the kick. Onsets
 * are sub-sample exact: a note that should have started 0.4 samples before the grid starts 0.4
 * samples into its waveform.
 *
 * **Release floor.** The release is never shorter than half a period of the note's fundamental. A
 * release that closes within a fraction of a period is a window shorter than the waveform it cuts,
 * and its spectrum spreads the note's end into a broadband thump.
 *
 * **Bite (19.09.2026).** A psytrance bass is two layers: the clean sub and a mid-bass "bite" -- the
 * same line as a saturated saw, low-passed a few hundred hertz up -- that makes it audible on small
 * speakers and gives the roll its attack. Before this round the voice was almost pure sub: in the first
 * drop of the listening seed its power in 300 Hz .. 2 kHz lay 21 dB under its power below 60 Hz, and
 * `Tools/ref_bass.py`, which measures the reference recordings between their kicks where kick and bass
 * play alone, reads the bite band there roughly 10 dB under the fundamental region (docs/PLAN.md,
 * 19.09.2026). The bite takes the oscillator's own samples, so it is phase-coherent with the saw path
 * and the sub by construction:
 * @code
 *   oscillator (2 fs) -> tanh drive -> 4-pole low pass (two SVFs, Butterworth, resonance on the second),
 *                        cutoff = Bite Cutoff x 2^(Bite Env x its own exponential envelope + key track)
 *   ... summed with the ladder's output *before* the half-band decimator and the Split high pass
 * @endcode
 * Summing it before the Split high pass is what keeps the low end clean: in Split mode the bite loses
 * its fundamental exactly as the saw path does (24.6 dB down at the default ratio), so everything at
 * the fundamental is still the one sine no filter touches, and the kick lock, which is solved on that
 * sine, is untouched. The bite's own envelope lets it be plucky (short, bright onset) while the saw
 * path stays dark and round, or long and "rubbery" with resonance.
 */
#pragma once
#include "phos/Ducker.h"
#include "phos/Dsp.h"
#include "phos/Halfband.h"
#include "phos/Ladder.h"
#include "phos/Oscillator.h"

namespace phos {

/** @brief Monophonic bass synthesizer. */
class Bass {
public:
    /** @brief Prepares for a sample rate. */
    void prepare(double sampleRate);
    /**
     * @brief Chooses the rate the oscillator and the ladder run at (Quality.h).
     *
     * 2 is the default: the voice runs at twice the sample rate and a half-band decimator brings it
     * back. 1 skips both -- one oscillator and one ladder step per output sample, no decimator -- and
     * costs about half. Call it after prepare() and before the first update(); the update() that
     * follows recomputes every coefficient that depends on the rate.
     * @param factor 1 or 2; anything else is clamped into that range
     */
    void setOversampling(int factor);
    /** @brief The factor set by setOversampling(). */
    int oversampling() const { return os_; }
    /** @brief Silences and clears all state. */
    void reset();
    /** @brief Reads the effective parameter values, indexed by bass::. */
    void update(const float* v);
    /**
     * @brief Starts a note.
     * @param pitch            MIDI note number
     * @param velocity         0..1
     * @param gateSamples      samples until the note is released
     * @param late             how many samples ago the note ideally started (0 <= late < 1)
     * @param fundamentalPhase sine phase of the fundamental at the ideal start, in cycles
     */
    void noteOn(int pitch, float velocity, int gateSamples, double late, double fundamentalPhase);
    /** @brief Starts a sidechain duck (called on every kick); @p late as for noteOn. */
    void duck(double late = 0.0) { ducker_.trigger(late); }
    /** @brief Renders @p n samples, replacing @p out. */
    void process(float* out, int n);
    /** @brief Whether the amplitude envelope is open. */
    bool active() const { return amp_.isActive(); }
    /** @brief The release actually used for the current note, in seconds (after the floor). */
    float effectiveRelease() const { return releaseUsed_; }
    /** @brief Fundamental phase the Start Phase knob stands for. */
    double knobPhase() const { return static_cast<double>(startPhase_) - 0.5; }

private:
    void applyNoteSettings();

    double sr_ = 48000.0;
    int    os_ = 2;                 ///< oscillator/ladder rate as a multiple of sr_ (1 or 2)
    double osRate_ = 96000.0;       ///< sr_ * os_, the rate the voice runs at
    VaOscillator        osc_;
    LadderT<float>      ladder_;
    HalfbandDown<float> down_;
    Svf                 hp1_, hp2_, hp3_, hp4_;   ///< the Split high pass, Linkwitz-Riley 8th order
    Svf                 bite1_, bite2_;     ///< the bite's 4-pole low pass (two Butterworth sections)
    Svf                 biteHp_;            ///< the bite's floor, a 2-pole high pass under the band
    Envelope            amp_;
    Ducker              ducker_;

    int    pitch_ = 30;
    float  velocity_ = 1.0f;
    int    gate_ = 0;
    float  fenv_ = 0.0f;
    double subPhase_ = 0.0;

    // Settings from update().
    float  wave_ = 0.0f, pw_ = 0.5f, subLevel_ = 0.42f, splitRatio_ = 2.0f;
    float  octLevel_ = 0.0f;        ///< amplitude of the sub's octave (sin at 2 f0, same phase course)
    int    subMode_ = 1;
    bool   retrigger_ = true;
    float  startPhase_ = 0.5f;
    float  cutoff_ = 140.0f, k_ = 1.0f, envOct_ = 4.0f, keyTrack_ = 0.6f, velCut_ = 0.25f;
    float  fDecay_ = 0.999f;
    float  driveIn_ = 1.0f, driveOut_ = 1.0f;
    float  attack_ = 0.0008f, decay_ = 0.18f, sustain_ = 0.55f, release_ = 0.01f, releaseUsed_ = 0.01f;
    float  level_ = 0.5f;
    /** @name The bite layer (see the file comment)
     *  @{ */
    float  biteLevel_ = 0.0f;       ///< output gain of the layer (knob x kBiteScale)
    float  biteCut_ = 600.0f;       ///< resting cutoff, Hz
    float  biteEnvOct_ = 1.5f;      ///< envelope depth, octaves
    float  biteDecay_ = 0.999f;     ///< per-sample factor of the bite envelope (-60 dB over Bite Decay)
    float  biteEnv_ = 0.0f;         ///< the bite envelope's current value, 1 at the onset
    float  biteGain_ = 1.0f;        ///< saturator input gain
    float  biteNorm_ = 1.0f;        ///< 1 / tanh(biteGain_): a full-scale saw stays full scale
    float  biteK2_ = 0.765f;        ///< damping of the second section (resonance)
    /** @} */
};

} // namespace phos
