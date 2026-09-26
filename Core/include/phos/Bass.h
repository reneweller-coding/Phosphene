/**
 * @file Bass.h
 * @brief The rolling psytrance bass: a phase-locked sine sub under a retriggered, filtered VA voice.
 *
 * Signal path per note:
 * @code
 *   PolyBLEP saw/pulse (2 fs) -> drive -> nonlinear ZDF ladder + filter envelope --+
 *                             -> bite: tanh -> 2-pole HP -> 4-pole LP + envelope --+-> half-band -> [LR8 high pass] --+
 *                                                                                                                  +-> amp ADSR -> duck -> level
 *   sines at f0 and 2 f0 (fs, same phase course as the saw's fundamental) -> sub and sub octave levels -----------------+
 * @endcode
 *
 * **Why the sub is separate.** A resonant low pass whose cutoff is swept by a fast envelope shifts
 * the phase of everything it passes, and the fundamental most of all while the cutoff falls towards
 * it: measured on the default sound, the fundamental's phase moved by -34 degrees during the first
 * 22 ms of every note. That is exactly the window in which the kick's tail and the bass overlap.
 * The fundamental is therefore generated as a pure sine that no filter touches, and in Split mode
 * the filtered voice is high-passed so that it carries the overtones only. Since 19.09.2026 that high
 * pass is a fourth-order Butterworth squared at Split x f0 -- an 8th-order Linkwitz-Riley, -6 dB at
 * the corner, 48 dB/octave -- which at the default ratio of 2 leaves the filtered path's fundamental
 * 48.2 dB down; the fourth-order pair before it left 24.6 dB. The difference became necessary when
 * the sub came down to make room for the octave and the bite: with the old pair the saw's leaked
 * fundamental would have stood about 11 dB under the new, quieter sub. Mixed mode adds the sub
 * without splitting.
 *
 * **Sub octave (19.09.2026).** A second sine at twice the fundamental, on the sub's own phase
 * course, so it is phase-locked by construction and carries no filter's phase. The reference
 * basses have more power in 60 .. 120 Hz than under 60 Hz between their kicks (Tools/ref_bass.py:
 * -0.9 against -7.4 dB, each against 20 .. 120 Hz); the saw path's second harmonic sits at the
 * Split corner and loses 6 dB there, so the weight comes from this sine instead of from moving the
 * corner towards the fundamental.
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
 * speakers and gives the roll its attack. Without it the voice is almost pure sub: in the first
 * drop of the listening seed its power in 300 Hz .. 2 kHz lay 21 dB under its power below 60 Hz, and
 * `Tools/ref_bass.py`, which measures the reference recordings between their kicks where kick and bass
 * play alone, reads the bite band there roughly 10 dB under the fundamental region (docs/rounds/2026-09.md,
 * 19.09.2026). The bite takes the oscillator's own samples, so it is phase-coherent with the saw path
 * and the sub by construction:
 * @code
 *   oscillator (2 fs) -> tanh drive -> 2-pole high pass at 0.4 x Bite Cutoff
 *                     -> 4-pole low pass (two SVFs, Butterworth, resonance on the second),
 *                        cutoff = Bite Cutoff x 2^(Bite Env x its own exponential envelope + key track)
 *   ... summed with the ladder's output *before* the half-band decimator and the Split high pass
 * @endcode
 * Summing it before the Split high pass is what keeps the low end clean: in Split mode the bite loses
 * its fundamental exactly as the saw path does (48 dB down at the default ratio, on top of its own
 * floor, a 2-pole high pass at 0.4 x Bite Cutoff), so everything at the fundamental is still the one
 * sine no filter touches, and the kick lock, which is solved on that sine, is untouched. The bite's
 * own envelope lets it be plucky (a bright onset that falls 13 dB above 700 Hz from a note's first
 * period to its third at the defaults) while the saw path stays round, or long and "rubbery" with
 * resonance. The pulse part of the wave has its edge at the fundamental's zero crossing; the bite's
 * saturator lifts that edge into a band-limited attack under the bite's cutoff, which is part of the
 * point, not a step.
 */
#pragma once
#include "phos/Ducker.h"
#include "phos/Dsp.h"
#include "phos/Halfband.h"
#include "phos/Filters.h"
#include "phos/Modulation.h"
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
    /** @brief The set's beat at the next sample and the beats per sample (Engine.cpp, per chunk): the synced LFOs' clock. */
    void setClock(double beat, double beatsPerSample) { beat_ = beat; beatsPerSample_ = beatsPerSample; }
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

    /**
     * @brief The modulation to show on the knobs (26.09.2026, the live ring): the sums per destination (ModDest) of the
     *        modulator, or null where nothing moves. Rendering thread.
     */
    const float* displayModulation() const { return modOn_ && mod_.active() ? modSum_ : nullptr; }
private:
    /** @brief Sets what depends on the note's pitch: the release floor, the envelope times and the Split high pass. */
    void applyNoteSettings();

    double sr_ = 48000.0;   ///< output sample rate
    int    os_ = 2;                 ///< oscillator/ladder rate as a multiple of sr_ (1 or 2)
    double osRate_ = 96000.0;       ///< sr_ * os_, the rate the voice runs at
    VaOscillator        osc_;   ///< saw-to-pulse oscillator
    LadderT<float>      ladder_;   ///< the four-pole ladder
    FilterLane          model_;    ///< bass.filter_model 1 .. 9 (26.09.2026, Filters.h), in the ladder's place
    /** @name The bass's own modulation (26.09.2026, Modulation.h), evaluated every 16 samples on the absolute count
     *  @{ */
    Modulator mod_;                       ///< envelope, LFOs, matrix
    float modSum_[kModDests] = {};        ///< the sums per destination
    bool  modOn_ = false;                 ///< some slot reaches something
    Envelope fAdsr_;                      ///< the filter envelope as an ADSR
    bool  fAdsrOn_ = false;               ///< the ADSR runs (attack at its minimum and sustain 0: the exponential decay)
    float noteRand_ = 0.0f;               ///< the Random source, per note
    Rng   modRng_;                        ///< its stream
    uint64_t modPos_ = 0;                 ///< samples since reset (the evaluation grid)
    double beat_ = 0.0, beatsPerSample_ = 0.0;   ///< setClock()
    float kMod_ = 0.0f, modelKMod_ = 0.0f, modeMod_ = 0.0f;   ///< the feedbacks and the mode as modulated
    float cutMod_ = 0.0f, levelMod_ = 1.0f;                   ///< the cutoff's octaves and the level's factor
    float resBase_ = 0.0f;                                     ///< bass.resonance, 0..1 (the resonance destination adds to it)
    /** @} */
    int   modelIndex_ = 0;         ///< bass.filter_model: 0 the ladder above
    float modelK_ = 0.0f, modelMode_ = 0.0f;   ///< the model's feedback (FilterVoicing::feedback) and bass.filter_mode
    float modelTrim_ = 1.0f;                   ///< the model's level against the bass ladder, made good (Bass.cpp)
    HalfbandDown<float> down_;   ///< back from the oversampled rate
    Svf                 hp1_, hp2_, hp3_, hp4_;   ///< the Split high pass, Linkwitz-Riley 8th order
    Svf                 bite1_, bite2_;     ///< the bite's 4-pole low pass (two Butterworth sections)
    Svf                 biteHp_;            ///< the bite's floor, a 2-pole high pass under the band
    Envelope            amp_;   ///< amplitude envelope
    Ducker              ducker_;   ///< the sidechain duck under the kick

    int    pitch_ = 30;   ///< the note's MIDI pitch
    float  velocity_ = 1.0f;   ///< 0..1
    int    gate_ = 0;   ///< samples until the note is released
    float  fenv_ = 0.0f;   ///< filter envelope
    double subPhase_ = 0.0;   ///< the sub sine's phase, in cycles

    // Settings from update().
    float  wave_ = 0.0f, pw_ = 0.5f, subLevel_ = 0.42f, splitRatio_ = 2.0f;   ///< waveform, pulse width, sub level, Split crossover as a multiple of f0
    float  octLevel_ = 0.0f;        ///< amplitude of the sub's octave (sin at 2 f0, same phase course)
    int    subMode_ = 1;   ///< bass.sub_mode (SubMode)
    bool   retrigger_ = true;   ///< bass.retrigger: every note restarts the envelopes
    float  startPhase_ = 0.5f;   ///< bass.start_phase
    float  cutoff_ = 140.0f, k_ = 1.0f, envOct_ = 4.0f, keyTrack_ = 0.6f, velCut_ = 0.25f;   ///< cutoff, ladder feedback, envelope depth (octaves), key tracking, velocity to cutoff
    float  fDecay_ = 0.999f;   ///< the filter envelope's per-sample decay
    float  driveIn_ = 1.0f, driveOut_ = 1.0f;   ///< drive into the ladder and the gain back out
    float  attack_ = 0.0008f, decay_ = 0.18f, sustain_ = 0.55f, release_ = 0.01f, releaseUsed_ = 0.01f;   ///< the amplitude envelope (s), and the release used after its floor
    float  level_ = 0.5f;   ///< bass.level, linear
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
