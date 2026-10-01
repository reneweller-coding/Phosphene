/**
 * @file Poly.h
 * @brief The polyphonic engine of the six melodic voices -- lead, counter-lead, arp, stab, pad and drone
 *        (Params.h, PolyInstance) -- eight voices of up to seven unison oscillators each.
 *
 * **Supersaw** (the default of the lead). Seven sawtooth oscillators after Szabo's measurement of the
 * Roland JP-8000 ("How to emulate the super saw", thesis, Stockholm 2010), all four of its findings
 * adopted and each number taken from the thesis itself. Each of the seven reads the *mipmapped* saw
 * of the Classic table (WaveTable.h) rather than generating a PolyBLEP ramp: measured on 16.09.2026,
 * the table saw leaves 30 dB less aliasing at C5 and C6 and 20 dB less at A6 (docs/rounds/2026-09.md). PolyBLEP
 * remains the VA oscillator, where one saw per voice is blended into a pulse and a table cannot give
 * the pulse width. The table frame is normalised to a different RMS than the ramp, so the slot gains
 * carry kSawTableGain and the lead keeps its calibrated level.
 *  - *Detune.* The side oscillators sit at 1 + a_i y(x) times the centre frequency with
 *    a = (-0.11002313, -0.06288439, -0.01952356, 0, 0.01991221, 0.06216538, 0.10745242) (table 1),
 *    where the detune knob x goes through the eleventh-degree polynomial y(x) fitted to the
 *    instrument (table 2): almost flat up to the middle, steep after 0.9 -- fine control of the narrow
 *    detunings that make the pad and string sounds.
 *  - *Mix.* The centre oscillator's gain falls linearly, -0.55366 x + 0.99785, while the six side
 *    oscillators rise along -0.73764 x^2 + 1.2841 x + 0.044372 (section 3.2). Here the two curves set
 *    the balance; the sum is normalised by its incoherent power so that the mix knob does not change
 *    the loudness (a deviation from the instrument, which gets 4 dB louder).
 *  - *Phase.* Every note starts every oscillator at a random phase (section 3.4): with fixed phases
 *    the seven saws add up coherently at the attack and every note sounds the same.
 *  - *High pass.* A high pass that follows the pitch removes what lies below the fundamental (section
 *    3.3). Phosphene makes it the depth rule for everything that is not kick or bass: 24 dB/octave at
 *    max(HP Floor, HP Track x f0), so nothing of the lead reaches the band under 140 Hz where the
 *    phase lock of kick and bass lives.
 *
 * **Dynamic detune.** A short note of seven beating saws smears its attack, a long one blooms. The
 * detune a note plays with therefore scales with its length: at a sixteenth (a quarter of a beat) or
 * shorter it is (1 - Dynamic Detune) of the knob, from one beat on the full knob, in between along
 * log2 of the length.
 *
 * **VA** plays the centre and the inner pair of oscillators as saw-to-pulse blends; **FM** plays
 * two-operator phase modulation on the same three oscillators (PolyKernel.h), with the index limited
 * per note so that the outer sidebands Carson's rule predicts still fit under Nyquist -- an FM voice
 * is otherwise the one oscillator that can alias louder than its own carrier (Poly.cpp). **Wavetable** (the pad
 * instance's default) plays all seven oscillators with Szabo's detune and mix, each reading a built-in
 * table (WaveTable.h) at its own random phase; the table position moves with an envelope and a slow LFO
 * whose period is given in beats. A table read is a gather, so it runs on the scalar side and hands the
 * kernel a row of samples -- the same code for every vector path, so the lanes stay bit-identical.
 *
 * The voice filter is a resonant 12 dB state-variable low pass with its own envelope and key
 * tracking; the amplitude envelope is the ADSR of Dsp.h. After the voices a tempo delay (TempoDelay.h)
 * and, when it is switched on, the all-pass disperser (Disperser.h).
 *
 * **Thermal drift.** A real analogue voice is never twice in the same place: its VCOs wander by a few
 * cents, its filter by a fraction of a semitone, its envelope by a few per cent, all on a time scale
 * of seconds -- the effect the analogue literature attributes to the temperature drift of the
 * exponential converter and which Pirkle ("Designing Software Synthesizer Plug-Ins in C++", 2nd ed.
 * 2019, chapter on analogue modelling) models exactly this way: a very low-frequency noise source per
 * oscillator. Phosphene runs one first-order low-pass filtered noise source per unison slot and per
 * voice (kDriftHz, so a correlation time of about 0.8 s), normalised so that its standing deviation
 * is poly.drift cents, and:
 *
 *  - the walks advance on the **absolute sample grid** -- once per kPolyBlock samples, in
 *    renderSegment(), for every slot and voice whether it sounds or not. They are therefore a
 *    function of the sample index and of the seed alone, and nothing about them changes when the host
 *    hands over its samples in different blocks;
 *  - they are **read once, at note on**, and held for the note. At 0.2 Hz a walk moves by about half
 *    its standing deviation within a sixteenth at 145 BPM, so holding it is not an approximation of
 *    the physics but a statement of it -- and it keeps the pitch of a sounding oscillator constant,
 *    which matters: a pitch that wanders *within* a note smears every harmonic, and the supersaw's
 *    aliasing figures are measured in narrow bands around the lines the oscillators should produce
 *    (the measurement is in docs/rounds/2026-09.md, 16.09.2026);
 *  - they are computed **on the scalar side**, like the wavetable rows, and reach the kernels only as
 *    coefficients, so the AVX2, NEON and scalar lane paths stay bit-identical;
 *  - the **bass does not drift**, and cannot: it is not a Poly instance. The phase lock of kick and
 *    bass sits on the bass's first note, and a drifting bass would break it.
 *
 * **Glide (portamento), 20.09.2026, round "dialogue".** The user's articulation rule separates the two
 * leads: "die Lead fliesst mit Portamento ueber die Halbtonschritte, die Counter-Lead peitscht trocken".
 * Without it no polyphonic voice can bend at all -- only the acid slides -- and the rule has no
 * mechanism. `poly.glide` is the time constant of a one-pole slew on the voice's pitch, in
 * milliseconds:
 *
 *  - the slew runs **on the same absolute kPolyBlock grid the drift walks on**, so where the host cuts
 *    its blocks never decides when a pitch changes, and the scalar side alone computes it: the kernels
 *    see the same slot phase steps they always saw and the AVX2, NEON and scalar paths stay
 *    bit-identical;
 *  - a note starts from the voice's **last sounding pitch**, held per instance rather than per voice,
 *    so two overlapping notes chain into one continuous bend instead of each restarting from its own
 *    predecessor -- the "retrigger-free where two notes overlap" of the rule. The amplitude and filter
 *    envelopes are untouched: a new note is still a new note, it simply arrives from where the voice
 *    already was;
 *  - the first note after a silence does not glide (there is nothing to glide from), and neither does
 *    a note whose pitch equals the one before it;
 *  - at glide 0 -- every voice's default except the lead's -- nothing is computed and the slot
 *    coefficients are exactly the ones noteOn() wrote, so a render is bit for bit the render without glide.
 *
 * The slew is on the *pitch*, not on the frequency: a semitone takes the same time wherever it lies,
 * which is what a portamento is. A pitch that moves inside a note necessarily smears the harmonics --
 * that is the effect -- so the glide is the one thing that is deliberately *not* held for the note the
 * way the drift is (Poly.h above).
 *
 * **Pan (same round).** `poly.pan` places the voice's dry signal, constant power. The rule makes it a
 * property of the role -- the lead slightly left, the counter slightly right -- so that the two leads
 * are separated in the image as well as in register and articulation. The delay send is taken from the
 * unpanned sum, so the voice's own left/right delay times keep their image whatever the pan does.
 *
 * Three quantities drift per note: the pitch of each unison slot (its own walk, so the seven saws of
 * a supersaw move against each other, which is what a unison of real oscillators does), the voice's
 * filter cutoff (a quarter of the pitch deviation in cents, a design ratio), and the amplitude
 * envelope's attack (one per cent per cent of drift, likewise a design ratio -- the walk is a pitch
 * measure and the other two are scaled from it rather than given their own depths, so one knob moves
 * the whole ageing of the instrument).
 */
#pragma once
#include "phos/Disperser.h"
#include "phos/Dsp.h"
#include "phos/Modulation.h"
#include "phos/Params.h"
#include "phos/PolyKernel.h"
#include "phos/TempoDelay.h"
#include "phos/WaveTable.h"
#include <cmath>
#include <vector>

namespace phos {

/** @brief Szabo's detune offsets of the seven supersaw oscillators (thesis, table 1). */
inline constexpr double kSupersawOffsets[kPolyUnison] = { -0.11002313, -0.06288439, -0.01952356, 0.0, 0.01991221, 0.06216538, 0.10745242 };

/** @brief Corner frequency of the thermal drift walks, Hz (a correlation time of about 0.8 s). */
inline constexpr double kDriftHz = 0.2;
/** @brief Filter drift in cents per cent of pitch drift (design ratio, see Poly.h). */
inline constexpr double kDriftCutoffRatio = 0.25;
/** @brief Attack drift, relative, per cent of pitch drift (design ratio, see Poly.h). */
inline constexpr double kDriftAttackRatio = 0.01;

/** @brief Polyphonic engine. */
class Poly {
public:
    /** @brief Prepares for a sample rate. */
    void prepare(double sampleRate);
    /** @brief Silences every voice and clears all state. */
    void reset();
    /** @brief Reads the effective parameters (indexed by poly::) and the tempo (for the delay). */
    void update(const float* v, double bpm);
    /**
     * @brief Whether the table update() chose is no longer the one its index resolves to -- a library table that
     *        was still being expanded then (a built-in stood in) and is published now. Engine::applyParams skips an
     *        update whose inputs did not move, and this is the one input that can move without a parameter.
     */
    bool tableStale() const;
    /**
     * @brief What update() does to the voices' envelopes and nothing else: every voice back to the written times.
     *
     * Engine::applyParams skips update() for a voice whose inputs did not move (23.09.2026), and has to call this
     * instead, because update() is not free of effect even then: noteOn() gives a voice with drift its own attack
     * time, "held for the note" by the comment there, and the next update() -- the next 32-sample chunk -- puts
     * every voice back to the written times. So the drifted attack has only ever lasted up to 32 samples. Skipping
     * update() made it last the whole note, which changed the render; this keeps the render as it was, and the
     * question whether the drift should really be held is left to a listening decision.
     */
    void refreshEnvelopeTimes() { for (Envelope& e : amp_) e.copyTimes(ampTimes_); }
    /**
     * @brief Starts a note.
     * @param pitch       MIDI note
     * @param velocity    0..1
     * @param lengthBeats written length of the note (for the dynamic detune)
     * @param gateSamples samples until release
     * @param late        how many samples ago the note ideally started (0 <= late < 1)
     * @param accent      22.09.2026: the note is accented -- its filter envelope opens 1.5 times as far
     *                    (the 303's accent logic on a poly voice; velocity carries the level as before)
     * @param slide       22.09.2026: the note may glide in from the pitch before it. True lets every
     *                    note glide; the lead passes its kNoteSlide flag (Score.h)
     */
    void noteOn(int pitch, float velocity, double lengthBeats, int gateSamples, double late, bool accent = false, bool slide = true);
    /**
     * @brief The free slow movement's two sines at this moment (Engine.cpp, 25.09.2026): the first moves the cutoff by
     *        up to an octave, the second the wavetable position by up to 0.15, both scaled by poly.slow_mod.
     */
    void setSlow(float cutoffSine, float colourSine) { slowCut_ = cutoffSine; slowColour_ = colourSine; }
    /**
     * @brief The set's beat at the next sample and the beats per sample (Engine.cpp, per chunk; 26.09.2026): the
     *        clock the tempo-synced LFOs of the modulation (Modulation.h) read.
     */
    void setClock(double beat, double beatsPerSample) { beat_ = beat; beatsPerSample_ = beatsPerSample; }
    /** @brief A voice's modulation sums per destination, as the last grid step evaluated them (tests, the live ring). */
    const float* modulation(int voice) const { return modSum_[voice]; }
    /**
     * @brief Closes the gate of every voice holding @p pitch (23.09.2026, live keyboard): a played note starts with a
     *        gate that never runs out on its own, and the key's release ends it -- the voice then releases as any.
     */
    void noteOff(int pitch)
    {
        for (int v = 0; v < kPolyVoices; ++v) if (gate_[v] > 0 && pitch_[v] == pitch) gate_[v] = 1;
    }

    /**
     * @brief Sets the quality limits of this instance (Quality.h).
     * @param unison oscillators per voice, 1 .. kPolyUnison
     * @param voices voices that may sound at once, 1 .. kPolyVoices
     *
     * The defaults (kPolyUnison, kPolyVoices) are "no limit", so a default engine runs exactly the
     * code it ran before this existed. Set once before the first note (Engine::prepare): the unison
     * limit decides which slots a note sets up, so changing it while notes sound would leave the
     * running notes with slots the render loop no longer sums.
     */
    void setQuality(int unison, int voices)
    {
        unisonLimit_ = unison < 1 ? 1 : (unison > kPolyUnison ? kPolyUnison : unison);
        voiceLimit_ = voices < 1 ? 1 : (voices > kPolyVoices ? kPolyVoices : voices);
    }
    /** @brief Oscillators per voice this instance plays. */
    int unisonLimit() const { return unisonLimit_; }
    /** @brief Voices this instance may sound at once. */
    int voiceLimit() const { return voiceLimit_; }

    /**
     * @brief noteOn() with the voice limit applied around it.
     *
     *  - *Voices.* noteOn() takes the first inactive voice and only steals the oldest one when none
     *    is free, so voices fill up from index 0. Keeping the top voices permanently silent is
     *    therefore enough: when every voice below the limit is sounding, the oldest of them is killed
     *    here, which makes it the first free voice noteOn() finds. A silent voice costs nothing --
     *    the slot and channel kernels skip a group of eight lanes when none of its voices sounds
     *    (Poly.cpp, renderSegment) -- so four of eight pad voices really do halve the pad's filters
     *    and leave 28 of 56 oscillator slots unrendered. The limit is enforced from here rather than
     *    inside noteOn() so that the allocator stays exactly as it is.
     *  - *Unison.* noteOn() itself sets up only the middle unisonLimit_ oscillators of a voice and
     *    leaves the rest at gain 0 with no phase step and no source weight, and renderSegment() then
     *    reads no wavetable for them, keeps no slot group alive for them and leaves them out of the
     *    voice's sum (Poly.cpp). The centre and the innermost detuned pair survive, which is the
     *    narrow beating the supersaw's body comes from, and the kept gains are normalised by their
     *    own incoherent power, so the level does not jump.
     */
    void noteOnLimited(int pitch, float velocity, double lengthBeats, int gateSamples, double late, bool accent = false, bool slide = true)
    {
        if (voiceLimit_ < kPolyVoices) freeVoiceWithinLimit();
        noteOn(pitch, velocity, lengthBeats, gateSamples, late, accent, slide);
    }

    /** @brief Renders @p n stereo samples, replacing @p L and @p R. */
    void process(float* L, float* R, int n);
    /** @brief Renders with a chosen lane type (float = scalar reference), for the vector tests. */
    template <class V> void processWith(float* L, float* R, int n);
    /** @brief Voices whose envelope is open. */
    int activeVoices() const;
    /**
     * @brief Seeds the random start phases and the thermal drift walks (deterministic renders).
     *
     * The drift gets its own generator, salted against the phase one, so that switching the drift on
     * or off does not move the start phases -- a render with drift 0 is bit-identical to a render
     * from before the drift existed.
     */
    void seedPhases(uint64_t seed)
    {
        phaseRng_.seed(seed);
        driftRng_.seed(seed ^ 0x44524946'54574C4Bull);   // "DRIFTWLK"
        // The modulation's streams (26.09.2026), salted against both, so nothing above moves.
        modRng_.seed(seed ^ 0x4D4F4452'414E4430ull);      // "MODRAND0"
        for (int v = 0; v < kPolyVoices; ++v) mod_[v].prepare(sr_, mixSeed(seed, 0x4D4F4456'4F494345ull + static_cast<uint64_t>(v)));
    }
    /** @brief The drift walk of one unison slot, in cents (tests). */
    float slotDrift(int slot) const { return driftSlot_[slot] * driftNorm_; }
    /** @brief The drift walk of one voice, in cents (tests). */
    float voiceDrift(int voice) const { return driftVoice_[voice] * driftNorm_; }
    /**
     * @brief The pitch a voice's oscillators are sounding at right now, in MIDI notes (tests).
     *
     * Equal to the note's own pitch except while a glide is running (Poly.h), which is what makes the
     * portamento measurable without a spectrum: the self test reads this on the kPolyBlock grid and
     * checks the trajectory against the time constant the parameter asks for.
     */
    double soundingPitch(int voice) const { return glidePitch_[voice]; }
    /**
     * @brief Wavetable reads the scalar pre-pass has done since prepare(), for the tests.
     *
     * The pre-pass reads one table sample per sounding wavetable slot per sample, and each read is a
     * mipmap choice plus a Catmull-Rom interpolation -- the most expensive scalar work of the engine.
     * The counter is raised once per voice and segment by the number of reads the segment did, never
     * inside the sample loop, so it costs nothing measurable. It is the only property that tells a
     * real unison limit from one that merely zeroes the outer gains: the sound is the same, the work
     * is not.
     */
    uint64_t tableReads() const { return tableReads_; }

    /** @brief Szabo's detune curve y(x), x in [0, 1]. */
    static double detuneCurve(double x);
    /** @brief Szabo's centre and side gains for the mix knob x in [0, 1]. */
    static void mixGains(double x, double& center, double& side);
    /** @brief The detune knob value a note of @p lengthBeats plays with. */
    static double dynamicDetune(double knob, double amount, double lengthBeats);

    /**
     * @brief The modulation to show on the knobs (26.09.2026, the live ring): the sums per destination (ModDest) of the
     *        latest note's voice (after it ends, the last it played), or null where nothing moves. Rendering thread.
     */
    const float* displayModulation() const
    {
        return modOn_ && lastVoice_ >= 0 && mod_[lastVoice_].active() ? modSum_[lastVoice_] : nullptr;
    }
private:
    float slowCut_ = 0.0f;   ///< setSlow()'s sine on the cutoff
    float slowColour_ = 0.0f;   ///< ... and on the colour
    /** @name The voice's own modulation (26.09.2026, Modulation.h)
     *  @{ */
    int   lastVoice_ = -1;                        ///< the voice of the latest note (displayModulation)
    Modulator mod_[kPolyVoices];                  ///< envelope, LFOs and matrix, per voice
    float modSum_[kPolyVoices][kModDests] = {};   ///< the sums per destination, per voice, on the 16-sample grid
    bool  modOn_ = false;                         ///< some slot of the matrix reaches something
    Envelope fAdsr_[kPolyVoices];                 ///< the filter envelope as an ADSR (filt_attack, filt_sustain, filt_release)
    bool  fAdsrOn_ = false;                       ///< the ADSR runs; off (attack at its minimum, sustain 0) the exponential decay does
    float noteRand_[kPolyVoices] = {};            ///< the Random source, drawn per note
    Rng   modRng_;                                ///< its stream, salted against the phases and the drift
    bool  slotOsc2_[kPolySlots] = {};             ///< the slot is the second oscillator (its own pitch destination)
    float idxFloor0_[kPolySlots] = {};            ///< the FM index floor the note set, before the modulation
    float modGainL_[kPolyVoices] = {}, modGainR_[kPolyVoices] = {};   ///< the level and pan destinations as channel gains
    double beat_ = 0.0, beatsPerSample_ = 0.0;    ///< setClock()
    /** @brief Evaluates voice @p voice's modulation at this grid step and writes what it moves (renderSegment). */
    void applyModulation(int voice, double beat);
    /** @} */
    /**
     * @brief Makes sure a voice below voiceLimit_ is free before noteOn() allocates.
     *
     * When one is already free, nothing happens and the note is allocated as always. Otherwise the
     * oldest voice below the limit is killed, which is the steal noteOn() would have done anyway --
     * it reuses the voice that started longest ago -- only restricted to the voices this quality
     * level allows.
     */
    void freeVoiceWithinLimit()
    {
        int oldest = 0;
        for (int i = 0; i < voiceLimit_; ++i) {
            if (!amp_[i].isActive()) return;
            if (age_[i] < age_[oldest]) oldest = i;
        }
        amp_[oldest].kill();
        gate_[oldest] = 0;
    }

    /** @brief Sets a voice's high-pass coefficients for its note. */
    void voiceCoefs(int voice);
    /** @brief Sets a voice's low-pass coefficients from its envelope, key tracking, drift and LFO. */
    void lowPassCoefs(int voice, double damping);
    /**
     * @brief Writes a voice's slot frequencies for the pitch it is sounding at (Poly.h, glide).
     *
     * Exactly the arithmetic noteOn() does for dt, inv, mdt and the wavetable read, with the note's own
     * detune curve, detune scale and FM ratio held from note on, so that a voice that never glides gets
     * the same numbers twice and one that does moves continuously between them.
     */
    void writeSlotPitch(int voice);
    /** @brief Advances every gliding voice by one kPolyBlock grid step (Poly.h). */
    void advanceGlide();
    /**
     * @brief Advances every drift walk by one kPolyBlock grid step.
     *
     * One first-order low pass per walk, x += (w - x) * kDriftAlpha with w uniform in [-1, 1]. The
     * standing variance of that process is alpha / (2 - alpha) times the variance of w, which is
     * 1/3; driftNorm_ divides it out and multiplies the drift depth back in, so the walk's standing
     * deviation is exactly the drift parameter in cents whatever the sample rate.
     */
    void advanceDrift();
    /** @brief Renders @p n samples with the lane type @p V (scalar, AVX2 or NEON). */
    template <class V> void renderSegment(float* L, float* R, int n);

    double sr_ = 48000.0;   ///< sample rate
    int unisonLimit_ = kPolyUnison;   ///< oscillators per voice (Quality.h)
    int voiceLimit_ = kPolyVoices;    ///< voices that may sound at once (Quality.h)
    /** @brief The instance's parameters as update() read them. Sized by the table itself: a fixed 64 held until the
     *         mix guide's appends of 25.09.2026 took poly::Count to 68, and update()'s copy ran past its end. */
    float values_[poly::Count] = {};
    PolySlots slots_;   ///< every unison slot's oscillator state (PolyKernel.h)
    PolyChannels ch_;   ///< every voice's filter and output state (PolyKernel.h)
    Envelope amp_[kPolyVoices];   ///< amplitude envelope per voice
    Envelope ampTimes_;   ///< the written times, computed once per update() and copied into every voice
    float fenv_[kPolyVoices] = {};   ///< per voice: its filter envelope
    float fDecay_ = 0.999f;   ///< the filter envelope's per-sample decay
    float accent_[kPolyVoices] = {};          ///< the note's factor on the filter envelope amount: 1, or 1.5 for an accent (22.09.2026)
    int   pitch_[kPolyVoices] = {};   ///< each voice's MIDI pitch
    float vel_[kPolyVoices] = {};   ///< each voice's velocity, 0..1
    int   gate_[kPolyVoices] = {};   ///< samples until each voice is released
    uint64_t age_[kPolyVoices] = {};   ///< note-on order, for voice stealing
    float hpHz_[kPolyVoices] = {};   ///< each voice's high-pass frequency (the depth rule)
    float posEnv_[kPolyVoices] = {};          ///< table-position envelope per voice
    double lfoPh_[kPolyVoices] = {};          ///< table-position LFO phase per voice
    double wtPh_[kPolySlots] = {};            ///< wavetable phase per slot (double: long pads)
    double wtDt_[kPolySlots] = {};            ///< wavetable phase step per slot
    int wtLevel_[kPolySlots] = {};            ///< table level per slot
    bool  sawVoice_[kPolyVoices] = {};        ///< every slot of the voice is the supersaw: the fast path
    bool  slotSaw_[kPolySlots] = {};          ///< this slot reads the Classic saw frame rather than the table
    double slotHzMul_[kPolySlots] = {};       ///< the slot's pitch against the note (the second oscillator's interval)
    double slotSpread_[kPolySlots] = {};      ///< the share of the unison detune the slot plays (Poly.cpp, kOsc2Spread)
    double lfo2Ph_ = 0.0;                     ///< the voice LFO's phase: one per instance, free-running
    float lfo2Inc_ = 0.0f;                    ///< its step per sample, from poly::LfoBeats and the tempo
    float lfo2Cut_ = 0.0f;   ///< the voice LFO's depth on the cutoff
    float lfo2Pitch_ = 0.0f;   ///< ... on the pitch
    float lfo2Amp_ = 0.0f;   ///< ... on the level
    float lfo2Value_ = 0.0f;                  ///< its last value, read by lowPassCoefs on the 16-sample grid
    const WaveTable* table_ = nullptr;   ///< the table poly.table names
    const WaveTable* sawTable_ = nullptr;     ///< the Classic table, whose frame kClassicSawFrame is the saw
    float posDecay_ = 0.999f;   ///< the position envelope's per-sample decay
    float lfoInc_ = 0.0f;   ///< the position LFO's step
    double bpm_ = 145.0;   ///< the tempo the LFO periods were computed for
    uint64_t counter_ = 0;   ///< notes so far (the voices' age)
    uint64_t pos_ = 0;          ///< samples rendered since reset (the coefficient grid)
    uint64_t tableReads_ = 0;   ///< wavetable reads of the scalar pre-pass (tests, see tableReads())
    Rng phaseRng_;   ///< the oscillators' random start phases
    Rng driftRng_;                            ///< the thermal drift walks, salted against phaseRng_
    float driftSlot_[kPolySlots] = {};        ///< drift walk per unison slot, before driftNorm_
    float driftVoice_[kPolyVoices] = {};      ///< drift walk per voice, before driftNorm_
    float driftOct_[kPolyVoices] = {};         ///< the voice's cutoff drift in octaves, held for the note
    float driftAlpha_ = 0.0f;                 ///< one-pole coefficient of a walk, per grid step
    float driftNorm_ = 0.0f;                  ///< walk -> cents, so the standing deviation is drift_
    float drift_ = 0.0f;                      ///< poly.drift in cents (standing deviation)
    /** @name Portamento (20.09.2026, round "dialogue"; Poly.h)
     *  @{ */
    double glidePitch_[kPolyVoices] = {};     ///< the pitch each voice sounds at now, in MIDI notes
    double glideTarget_[kPolyVoices] = {};    ///< the note's own pitch: where the slew is going
    double glideY_[kPolyVoices] = {};         ///< the note's detune curve value, held for the note
    double glideScale_[kPolyVoices] = {};     ///< the note's detune scale (FM halves it), held
    double glideFmRatio_[kPolyVoices] = {};   ///< the note's FM ratio, held
    /**
     * @brief Each slot's thermal drift factor, read at note on and held for the note.
     *
     * The drift walks keep moving while a note sounds (Poly.h), and a glide has to keep reading the
     * value the note started with -- otherwise the portamento would drag the drift into the note and
     * break the statement that a sounding oscillator's pitch is constant apart from the bend.
     */
    double driftFactorHeld_[kPolySlots] = {};
    double lastPitch_ = -1.0;                 ///< the instance's last sounding pitch, < 0 before the first note
    int    newest_ = 0;                       ///< the voice that started last: the one lastPitch_ follows
    float  glideAlpha_ = 0.0f;                ///< one-pole coefficient of the slew, per kPolyBlock step
    float  glideMs_ = 0.0f;                   ///< poly.glide: 0 switches the whole mechanism off
    float  panL_ = 1.0f, panR_ = 1.0f;        ///< constant-power gains of poly.pan (both exactly 1 at centre)
    /** @} */
    Disperser disperse_;                      ///< all-pass chain coefficients (Disperser.h)
    DisperserChannel dispL_;   ///< the disperser's state, left
    DisperserChannel dispR_;   ///< ... right
    TempoDelay delay_;   ///< the voice's tempo delay
    float send_ = 0.0f;   ///< poly.delay_send, linear
    float level_ = 1.0f;   ///< poly.level, linear
    std::vector<float> slotL_;   ///< a block's slot outputs, left
    std::vector<float> slotR_;   ///< ... right
    std::vector<float> chanIn_;   ///< the channels' input
    std::vector<float> chanAmp_;   ///< the channels' amplitude
    std::vector<float> chanOut_;   ///< the channels' output
    std::vector<float> sendBuf_;   ///< the delay send
    std::vector<float> wtRow_;   ///< the wavetable rows
};

} // namespace phos
