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
 * @author Rene Weller
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
    /**
     * @brief Prepares for a sample rate.
     * @param[in] sampleRate  the sample rate
     */
    void prepare(double sampleRate);
    /**
     * @brief Chooses the rate the oscillator and the ladder run at (Quality.h).
     *
     * 2 is the default: the voice runs at twice the sample rate and a half-band decimator brings it
     * back. 1 skips both -- one oscillator and one ladder step per output sample, no decimator -- and
     * costs about half. Call it after prepare() and before the first update(); the update() that
     * follows recomputes every coefficient that depends on the rate.
     * @param[in] factor  1 or 2; anything else is clamped into that range
     */
    void setOversampling(int factor);
    /**
     * @brief The factor set by setOversampling().
     * @return the oversampling factor
     */
    int oversampling() const { return m_os; }
    /** @brief Silences and clears all state. */
    void reset();
    /**
     * @brief Reads the effective parameter values, indexed by bass::.
     * @param[in] v  the effective parameter values
     */
    void update(const float* v);
    /**
     * @brief The set's beat at the next sample and the beats per sample (Engine.cpp, per chunk): the synced LFOs'
     *        clock.
     * @param[in] beat            the beat at the next sample
     * @param[in] beatsPerSample  the beats per sample
     */
    void setClock(double beat, double beatsPerSample) { m_beat = beat; m_beatsPerSample = beatsPerSample; }
    /**
     * @brief Starts a note.
     * @param[in] pitch             MIDI note number
     * @param[in] velocity          0..1
     * @param[in] gateSamples       samples until the note is released
     * @param[in] late              how many samples ago the note ideally started (0 <= late < 1)
     * @param[in] fundamentalPhase  sine phase of the fundamental at the ideal start, in cycles
     */
    void noteOn(int pitch, float velocity, int gateSamples, double late, double fundamentalPhase);
    /** @brief Ends the note's gate at the next sample (a played key's release, 01.10.2026; Engine::liveNoteOff). */
    void release() { if (m_gate > 1) m_gate = 1; }
    /**
     * @brief Starts a sidechain duck (called on every kick); @p late as for noteOn.
     * @param[in] late  samples after the ideal start
     */
    void duck(double late = 0.0) { m_ducker.trigger(late); }
    /**
     * @brief Renders @p n samples, replacing @p out.
     * @param[out] out  the samples
     * @param[in]  n    the block's samples
     */
    void process(float* out, int n);
    /**
     * @brief Whether the amplitude envelope is open.
     * @return true while the envelope is open
     */
    bool active() const { return m_amp.isActive(); }
    /**
     * @brief The release actually used for the current note, in seconds (after the floor).
     * @return the release in seconds
     */
    float effectiveRelease() const { return m_releaseUsed; }
    /**
     * @brief Fundamental phase the Start Phase knob stands for.
     * @return the phase in turns
     */
    double knobPhase() const { return static_cast<double>(m_startPhase) - 0.5; }

    /**
     * @brief The modulation to show on the knobs (26.09.2026, the live ring): the sums per destination (ModDest) of the
     *        modulator, or null where nothing moves. Rendering thread.
     * @return the sums per destination, or null
     */
    const float* displayModulation() const { return m_modOn && m_mod.active() ? m_modSum : nullptr; }
private:
    /** @brief Sets what depends on the note's pitch: the release floor, the envelope times and the Split high pass. */
    void applyNoteSettings();

    double m_sr = 48000.0;   ///< output sample rate
    int    m_os = 2;                 ///< oscillator/ladder rate as a multiple of m_sr (1 or 2)
    double m_osRate = 96000.0;       ///< m_sr * m_os, the rate the voice runs at
    VaOscillator        m_osc;   ///< saw-to-pulse oscillator
    LadderT<float>      m_ladder;   ///< the four-pole ladder
    FilterLane          m_model;    ///< bass.filter_model 1 .. 9 (26.09.2026, Filters.h), in the ladder's place
    /** @name The bass's own modulation (26.09.2026, Modulation.h), evaluated every 16 samples on the absolute count
     *  @{ */
    Modulator m_mod;                       ///< envelope, LFOs, matrix
    float m_modSum[ModDests] = {};        ///< the sums per destination
    bool  m_modOn = false;                 ///< some slot reaches something
    Envelope m_fAdsr;                      ///< the filter envelope as an ADSR
    bool  m_fAdsrOn = false;               ///< the ADSR runs (attack at its minimum and sustain 0: the exponential decay)
    float m_noteRand = 0.0f;               ///< the Random source, per note
    Rng   m_modRng;                        ///< its stream
    uint64_t m_modPos = 0;                 ///< samples since reset (the evaluation grid)
    double m_beat = 0.0, m_beatsPerSample = 0.0;   ///< setClock()
    float m_kMod = 0.0f, m_modelKMod = 0.0f, m_modeMod = 0.0f;   ///< the feedbacks and the mode as modulated
    float m_cutMod = 0.0f, m_levelMod = 1.0f;                   ///< the cutoff's octaves and the level's factor
    float m_resBase = 0.0f;                                     ///< bass.resonance, 0..1 (the resonance destination adds to it)
    /** @} */
    int   m_modelIndex = 0;         ///< bass.filter_model: 0 the ladder above
    float m_modelK = 0.0f;   ///< the model's feedback (FilterVoicing::feedback)
    float m_modelMode = 0.0f;   ///< bass.filter_mode
    float m_modelTrim = 1.0f;                   ///< the model's level against the bass ladder, made good (Bass.cpp)
    HalfbandDown<float> m_down;   ///< back from the oversampled rate
    Svf                 m_hp1;   ///< the Split high pass (Linkwitz-Riley, 8th order): first section
    Svf                 m_hp2;   ///< ... second
    Svf                 m_hp3;   ///< ... third
    Svf                 m_hp4;   ///< ... fourth
    Svf                 m_bite1;   ///< the bite's 4-pole low pass: first Butterworth section
    Svf                 m_bite2;   ///< ... second
    Svf                 m_biteHp;            ///< the bite's floor, a 2-pole high pass under the band
    Envelope            m_amp;   ///< amplitude envelope
    Ducker              m_ducker;   ///< the sidechain duck under the kick

    int    m_pitch = 30;   ///< the note's MIDI pitch
    float  m_velocity = 1.0f;   ///< 0..1
    int    m_gate = 0;   ///< samples until the note is released
    float  m_fenv = 0.0f;   ///< filter envelope
    double m_subPhase = 0.0;   ///< the sub sine's phase, in cycles

    // Settings from update().
    float  m_wave = 0.0f;   ///< the waveform
    float  m_pw = 0.5f;   ///< the pulse width
    float  m_subLevel = 0.42f;   ///< the sub's level
    float  m_splitRatio = 2.0f;   ///< the Split crossover as a multiple of f0
    float  m_octLevel = 0.0f;        ///< amplitude of the sub's octave (sin at 2 f0, same phase course)
    int    m_subMode = 1;   ///< bass.sub_mode (SubMode)
    bool   m_retrigger = true;   ///< bass.retrigger: every note restarts the envelopes
    float  m_startPhase = 0.5f;   ///< bass.start_phase
    float  m_cutoff = 140.0f;   ///< the cutoff, Hz
    float  m_k = 1.0f;   ///< the ladder's feedback
    float  m_envOct = 4.0f;   ///< the envelope's depth, octaves
    float  m_keyTrack = 0.6f;   ///< the key tracking
    float  m_velCut = 0.25f;   ///< velocity to cutoff
    float  m_fDecay = 0.999f;   ///< the filter envelope's per-sample decay
    float  m_driveIn = 1.0f;   ///< the drive into the ladder
    float  m_driveOut = 1.0f;   ///< ... and the gain back out
    float  m_attack = 0.0008f;   ///< the amplitude envelope's attack, s
    float  m_decay = 0.18f;   ///< ... decay, s
    float  m_sustain = 0.55f;   ///< ... sustain
    float  m_release = 0.01f;   ///< ... release, s
    float  m_releaseUsed = 0.01f;   ///< the release used after its floor, s
    float  m_level = 0.5f;   ///< bass.level, linear
    /** @name The bite layer (see the file comment)
     *  @{ */
    float  m_biteLevel = 0.0f;       ///< output gain of the layer (knob x BiteScale)
    float  m_biteCut = 600.0f;       ///< resting cutoff, Hz
    float  m_biteEnvOct = 1.5f;      ///< envelope depth, octaves
    float  m_biteDecay = 0.999f;     ///< per-sample factor of the bite envelope (-60 dB over Bite Decay)
    float  m_biteEnv = 0.0f;         ///< the bite envelope's current value, 1 at the onset
    float  m_biteGain = 1.0f;        ///< saturator input gain
    float  m_biteNorm = 1.0f;        ///< 1 / tanh(m_biteGain): a full-scale saw stays full scale
    float  m_biteK2 = 0.765f;        ///< damping of the second section (resonance)
    /** @} */
};

} // namespace phos
