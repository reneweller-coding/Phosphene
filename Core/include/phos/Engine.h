/**
 * @file Engine.h
 * @brief The engine: plays score events sample-accurately through the generators, mixer and master.
 *
 * **Time.** Musical time advances on a fixed grid of 32-sample chunks counted from the start. At the
 * start of every chunk the tempo is read (from the tempo map, or from compose.bpm when no map is set),
 * ramps advance and the effective parameters are applied; inside the chunk the beat of sample i is
 * chunkBeat + i * beatsPerSample. Because the grid is absolute, the output does not depend on how a
 * host cuts the stream into blocks (checked bit for bit in the self test).
 *
 * **Events.** Notes and control events arrive in two lock-free rings. An event fires at the first
 * sample whose beat has reached it, and the generators are told how far past the ideal instant that
 * sample lies (0 <= late < 1 sample), so onsets are sub-sample exact. Control events fire before
 * notes at the same sample, so a sound change on a downbeat applies to the kick on that downbeat.
 *
 * **Effective parameters.** Every generator plays knob + offset (continuous parameters, in the
 * normalised domain) or the override (discrete parameters); see ControlEvent. Then two constraints
 * are applied that depend on tempo and pattern: the kick's tail limit at the first bass slot, and
 * the kick lock between the kick's phase and the bass's start phase.
 *
 * **Signal flow.** Every generator renders into its own buffer. Its channel strip applies the level, the
 * kick's sidechain duck (event-driven, Ducker.h), the trance gate for lead, arp and pad (TranceGate.h),
 * and sends to a short room and a long hall (Reverb.h), whose returns are ducked as well. A voice whose
 * own `hall_gate` is on (poly.hall_gate, acid.hall_gate; 20.09.2026, round "reverb") sends into a second,
 * dedicated hall instead: a big space that ducks by its own send's envelope and is cut hard on the
 * absolute bar line (Reverb::processDucked, Reverb::barGate) -- off by default, so it changes nothing
 * for a voice that never turns it on. The master sums everything, applies the gain (knob, the track's
 * level match and the composer's loudness offset),
 * the bus compressor, mono bass (the side signal high-passed), the soft clipper, the band limit that
 * gives the programme an upper end (Dsp.h, BandLimit), the lookahead true-peak limiter and a final
 * safety clip at the ceiling, and meters the result to BS.1770 (Dynamics.h, Loudness.h). With the
 * limiter on, the output is delayed by latencySamples().
 *
 * **Why the band limit sits between the clipper and the limiter.** After the clipper, because the
 * clipper is the last stage that makes new harmonics; before the limiter, because a true-peak ceiling
 * is a claim about the analogue waveform and a true-peak estimate is a band-limited reconstruction --
 * a limiter handed a programme that runs past its estimator's band cannot hold the ceiling it reports.
 * Measured on eight minutes of seed 7 before this was there: the meter read -0.98 dBTP, the exact peak
 * was -0.075, and the same render cut at the estimator's 0.45 fs read -0.182 exact against -0.218
 * estimated -- the whole error lived above the band.
 *
 * **The psychedelic layer (19.09.2026, round "fx-psychedelia").** Two more strips after the SFX strip:
 * the shamanic bed (Texture.h) and the voices (Vocal.h). Effect notes are routed by their type
 * (Sfx.h, sfxTypePart), so the composer still writes every effect as a Part::Sfx note. Three more
 * things happen around the strips:
 *  - the SFX strip passes its insert chain (PsyFx.h: flanger, phaser, frequency shifter) before its
 *    gain, and the texture and vocal strips *send* into a second chain whose return joins the mix --
 *    both move per section (control events on psyfx.*, Form.cpp) and per event (a riser drags the
 *    shifter up, a downlifter down, a sweep opens the flanger, a phrase detunes the send);
 *  - the vocal strip throws its last word into a tempo delay (Vocal.h, the throw weight);
 *  - a Stutter event repeats a slice of the melodic bus (acid, lead, arp) in place of the live signal;
 *    the sends keep the live signal, so the reverb tails run on underneath the glitch;
 *  - the sub drop leaves the SFX generator on its own mono output and joins kick and bass in the
 *    centre, under a ducker the kick triggers (sfx.sub_duck), so it never sits on a kick transient.
 *
 * **Wandering effects (20.09.2026, round "wandering-fx").** With `sfx.wander` on, an effect voice's pan
 * sweeps across the field instead of oscillating and its content crosses from dry to the hall's send over
 * its own length (Sfx.h); the growing wet share (`sfxWetL_`/`sfxWetR_`) is added straight into the plain
 * hall's send in renderSegment(), alongside the (correspondingly shrinking) dry mix's own constant
 * sfx.hall_send fraction -- not into Engine::hallGate_, whose self-duck and bar-line cut are built for a
 * continuous voice, not a one-shot event's own growing trajectory into a tail that is meant to run past
 * the bar. Off by default, so no existing render changes.
 *
 * **Threads.** process() runs on the audio thread and never allocates. The push functions may be
 * called from one other thread. Parameters may be written from any thread.
 */
#pragma once
#include "phos/Acid.h"
#include "phos/Bass.h"
#include "phos/Dynamics.h"
#include "phos/FieldPlayer.h"
#include "phos/Loudness.h"
#include "phos/Reverb.h"
#include "phos/Sfx.h"
#include "phos/TranceGate.h"
#include "phos/Clock.h"
#include "phos/Kick.h"
#include "phos/Params.h"
#include "phos/Perc.h"
#include "phos/Poly.h"
#include "phos/PsyFx.h"
#include "phos/Quality.h"
#include "phos/Score.h"
#include "phos/TempoDelay.h"
#include "phos/Texture.h"
#include "phos/Vocal.h"
#include <limits>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <vector>

namespace phos {

/**
 * @name Stems (23.09.2026, round "Stems")
 * One stem per part (Part order: kick, bass, percussion, acid, the six polyphonic voices, effects, bed,
 * voices) and one for the returns -- room, hall, gated hall, the modulation send and the voices' delay
 * throw. Each part's stem is what that part puts into the mix: after its strip gain, its trance gate and
 * its duck, times the master gain, *before* the master (bus compressor, mono bass, clipper, band limit,
 * limiter). The sub drop belongs to the effects stem. A stutter is not in the stems: they carry the live
 * melodic bus the stutter replaces. So the stems sum to the mix as it enters the master, and a stem is what
 * a mixing engineer would get from a bounce of that channel.
 * @{ */
constexpr int kNumStems = kNumParts + 1;           ///< the parts and the returns
extern const char* const kStemNames[kNumStems];    ///< "Kick" ... "Vocal", "Returns"
/**
 * @brief Where the engine writes the stems while it renders (Engine::setStemTap).
 *
 * Each pointer is a buffer of at least as many samples as the largest process() call; the engine writes
 * sample i of a call at index i, exactly as it writes the mix.
 */
struct StemTap {
    float* L[kNumStems] = {};   ///< left channel of each stem
    float* R[kNumStems] = {};   ///< right channel of each stem
};
/** @} */

/** @brief The Phosphene engine. */
class Engine {
public:
    static constexpr int kChunk = 32;   ///< samples per time/parameter chunk

    /**
     * @brief Writes the stems into @p tap on every following process() call; nullptr stops it (StemTap).
     *
     * Costs a store per part and sample while it is set and nothing when it is not; the mix is the same
     * bit for bit either way (the self test measures that). Not for the audio thread of a host: set it
     * before rendering, from the thread that renders.
     */
    void setStemTap(StemTap* tap) { tap_ = tap; }

    /**
     * @name A MIDI keyboard on one voice (23.09.2026, round "Keyboard")
     * mix.keyboard_part says which voice the keyboard plays -- the acid or one of the six polyphonic voices, or
     * "By channel" (channel 1 the acid, 2 the lead ... 7 the drone) -- with that voice's sound as its page has it.
     * mix.keyboard_mode Replace silences the voice's generated notes (in "By channel" from a voice's first played
     * note on), Layer plays over them. A played note starts with a gate that never runs out; its key's release
     * ends it. Called on the rendering thread between process() calls -- the plugin splits a block at every MIDI
     * event, so a note starts on its sample.
     * @{ */
    /** @brief Starts a note played live.
     *  @param pitch MIDI note; @param velocity 1..127; @param channel 0..15 */
    void liveNoteOn(int pitch, int velocity, int channel);
    /** @brief Releases the note @p pitch on whichever voice its note-on went to. */
    void liveNoteOff(int pitch, int channel);
    /** @brief Releases every played note (transport stop, a changed keyboard part). */
    void liveAllOff();
    /** @brief Whether the composer's notes of @p part are replaced by the keyboard (the plugin's MIDI out skips them too). */
    bool generatedSilenced(Part part) const;
    /** @} */

    /**
     * @brief Plays bank preset @p preset of the family behind preset choice @p choice now, for the effects page's
     *        audition (24.09.2026; Sfx.h, kPresetChoiceType). Rendering thread, between process() calls. The
     *        family's sfx.preset_* choice still wins inside Sfx::trigger, so the page passes the preset it shows.
     */
    void previewSfx(int choice, int preset);

    /**
     * @name Channel meters (24.09.2026, the mixer's strips)
     * What each part puts into the mix -- the stem tap's signal (StemTap): after its strip gain, trance gate and
     * duck, times the master gain, before the master -- as a peak and a sum of squares (the mean of the two
     * channels) per part since the last takeMeters(). Off unless switched on, and reading only: the mix is the
     * same to the bit either way.
     * @{ */
    /** @brief Switches the gathering on or off (rendering thread). */
    void setMetering(bool on) { metering_ = on; }
    /**
     * @brief Hands over what was gathered since the last call and starts again (rendering thread).
     * @param peak  kNumParts peaks (absolute sample values)
     * @param sumSq kNumParts sums of squares
     * @return the number of samples they cover
     */
    int takeMeters(float* peak, double* sumSq);
    /** @} */

    Engine();
    /** @brief Lets go of the field library's loader thread (FieldLibrary.h). */
    ~Engine();
    Engine(const Engine&) = delete;              ///< one engine holds one share of the loader thread
    Engine& operator=(const Engine&) = delete;   ///< likewise

    /**
     * @brief Prepares for playback.
     * @param sampleRate   output rate
     * @param maxBlockSize largest block the host will use (any size works; larger ones are split)
     * @param quality      what the engine may spend per part (Quality.h); the default is the desktop
     *                     level, which is what the engine did before quality levels existed
     */
    void prepare(double sampleRate, int maxBlockSize, const Quality& quality = Quality::desktop());

    /**
     * @brief Expands the wavetables the parameters currently name, so the voices really sound them.
     *
     * Since 22.09.2026 the library is only indexed at load and a table is expanded when somebody
     * asks for it (WaveTableFile.h, ensureWaveTables) -- 464 tables cannot all live in memory. The
     * composer asks for a track's tables while it plans it, which covers everything the generator
     * itself decides. This covers the other way a table gets chosen: a human turning the Table knob,
     * a `--set pad.table=...`, a loaded `.phosset`. Without it those play the built-in fallback and
     * nothing says why.
     *
     * Cheap when nothing changed (six atomic loads, no lock), so the plugin can call it off its
     * 30 Hz timer. **Never from the audio thread:** it decodes and allocates. `prepare()` calls it
     * once for whatever the parameters hold at that point.
     */
    void ensureVoiceTables();
    /** @brief The level prepare() was called with. */
    const Quality& quality() const { return quality_; }
    /** @brief Back to beat 0; clears events, offsets, overrides and all sound. */
    void reset();

    /** @brief The parameters (the knobs). */
    ParamStore& params() { return params_; }
    const ParamStore& params() const { return params_; }

    /** @brief Uses a tempo map instead of compose.bpm (call while stopped). */
    void setTempoMap(const TempoMap& map) { tempo_ = map; useTempoMap_ = true; }
    /** @brief Goes back to compose.bpm. */
    void clearTempoMap() { useTempoMap_ = false; }

    /** @brief Queues a note (producer thread); false if the ring is full. */
    bool pushEvent(const NoteEvent& e) { return notes_.push(e); }
    /** @brief Queues a control event (producer thread); false if the ring is full. */
    bool pushControl(const ControlEvent& e) { return controls_.push(e); }
    /**
     * @brief Queues a control event that takes effect at the next chunk, ahead of everything queued (producer
     *        thread, the same one that pushes the others); false if its ring is full.
     *
     * The control ring is first in, first out, and the composer keeps it filled eight bars ahead, so an event
     * pushed "now" into it waits behind every event of those eight bars -- some thirteen seconds at 145 BPM
     * (23.09.2026: the measured line levels and Auto Gain's offset arrived that late, and the host test's ramp
     * was still short of its target when it looked). This ring is read at every chunk start before anything
     * else; an event whose beat has passed starts there, so a ramp runs its full length from now.
     */
    bool pushImmediate(const ControlEvent& e) { return immediate_.push(e); }
    /** @brief Beat position of the next sample to be rendered (any thread). */
    double beatPosition() const { return beatNow_.load(std::memory_order_relaxed); }
    /** @brief Samples rendered since reset. */
    uint64_t samplePosition() const { return samples_; }

    /** @brief Renders @p n stereo samples. */
    void process(float* L, float* R, int n);

    /** @brief The kick, for tests and displays. */
    const Kick& kick() const { return kick_; }
    /** @brief The bass, for tests and displays. */
    const Bass& bass() const { return bass_; }
    /** @brief The percussion kit, for tests and displays. */
    const PercKit& percKit() const { return perc_; }
    /** @brief The acid voice, for tests and displays. */
    const Acid& acid() const { return acid_; }
    /** @brief A polyphonic engine (lead or arp), for tests and displays. */
    const Poly& poly(PolyInstance i) const { return poly_[static_cast<int>(i)]; }
    /** @brief The effect generator, for tests and displays. */
    const Sfx& sfx() const { return sfx_; }
    /** @brief The shamanic bed, for tests and displays. */
    const Texture& texture() const { return texture_; }
    /** @brief The voices, for tests and displays. */
    const Vocal& vocal() const { return vocal_; }
    /** @brief True while a stutter replaces the melodic bus (tests). */
    bool stuttering() const { return stutter_.active(); }
    /** @brief The SFX strip's modulation chain (tests: the event motion in force). */
    const PsyFxChain& sfxChain() const { return sfxFx_; }
    /** @brief Samples by which the output lags the events (the limiter's lookahead; 0 when it is off). */
    int latencySamples() const { return limiterOn_ ? limiter_.latency() : 0; }
    /** @brief The meter on the master output (audio thread; read it when not processing). */
    LoudnessReading meter() const { return meter_.read(); }
    /** @brief Restarts the meter. */
    void resetMeter() { meter_.reset(); }
    /** @brief Gain reduction of the bus compressor and the limiter at the end of the last block, dB. */
    float compReduction() const { return comp_.reduction(); }
    float limiterReduction() const { return limiter_.reduction(); }   ///< @copydoc compReduction
    /** @brief Effective value of a parameter as last applied (audio thread view). */
    float effective(int id) const;
    /**
     * @brief Every parameter's effective value, with the modulation of the voice a player hears on the knobs it moves
     *        (26.09.2026, the live ring): cutoff, resonance, filter mode, pulse width, FM index, table position, level
     *        and pan of the six voices, the bass and the acid, the way each synth applies its sums. Rendering thread.
     * @param out params().count() values
     */
    void playedValues(float* out) const;
    /** @brief The key's pitch class the engine plays in (0 = C), as last applied (audio thread view; the scopes). */
    int keyRoot() const { return keyRoot_; }

private:
    /** @brief Moves every running offset ramp to the current chunk's beat. */
    void advanceRamps();
    /** @brief Recomputes the effective values that moved and hands them to every generator (Engine.h, raw_). */
    void applyParams();
    /** @brief Renders @p count samples at @p offset: every generator, the strips, the sends and the master. */
    void renderSegment(float* L, float* R, int offset, int count);
    /** @brief Starts note @p e on its generator, @p late samples after its ideal onset. */
    void dispatch(const NoteEvent& e, double late);
    /** @brief Applies control event @p e: a parameter's offset ramp or override, or the bass slot of the beat. */
    void dispatchControl(const ControlEvent& e);
    /** @brief Seconds from the kick to the bass's first slot of the beat (the phase lock's target). */
    double firstSlotSeconds() const;

    ParamStore params_;   ///< the knobs (the host's and the command line's)
    double sr_ = 48000.0;   ///< sample rate
    Quality quality_;   ///< what each part may spend (Quality.h)

    TempoMap tempo_;   ///< the tempo ramps, when a render has them
    bool useTempoMap_ = false;   ///< true: beats per sample follow tempo_, not compose.bpm

    EventRing<NoteEvent> notes_;   ///< notes from the composer, in beat order
    EventRing<ControlEvent> controls_;   ///< control events from the composer, in beat order
    EventRing<ControlEvent> immediate_;    ///< pushImmediate()
    uint64_t samples_ = 0;   ///< samples rendered since the last reset
    double chunkBeat_ = 0.0;   ///< the beat at the start of the current 32-sample chunk
    double beatsPerSample_ = 0.0;   ///< the tempo of the current chunk
    int chunkPos_ = 0;   ///< samples of the current chunk already rendered
    std::atomic<double> beatNow_{ 0.0 };   ///< the beat reached, for other threads (beatPosition)

    /** @brief State of a parameter's variation. */
    struct Variation {
        float offset = 0.0f;               ///< current normalised offset
        float from = 0.0f, to = 0.0f;      ///< ramp endpoints
        double start = 0.0, length = 0.0;  ///< ramp in beats; length 0 = not ramping
        float override = -1.0f;            ///< discrete override, < 0 = none
        float base = std::numeric_limits<float>::quiet_NaN();   ///< ControlEvent::Kind::Base: the value in place of the knob, NaN = none
    };
    std::unique_ptr<Variation[]> var_;   ///< every parameter's offset and override
    std::vector<float> eff_;               ///< effective values, indexed by global id
    /**
     * @name What applyParams() can skip (23.09.2026, round "Planung + applyParams")
     * VTune put applyParams at 6 % of a render: every 32-sample chunk and every control event recomputed all ~3000
     * effective values, each continuous one with an offset through fromNormalised(toNormalised()) -- powf and log
     * for every logarithmic knob -- and a track start dispatches hundreds of events at one instant, each of them a
     * full pass. Now a value is recomputed only when its knob, its offset or override, or its own-sound switch
     * moved (raw_ keeps the result before Kick::constrain, which edits eff_ in place), and events at one sample
     * are applied once, before anything reads them. The output is the same to the bit (checked on three sets).
     * @{ */
    std::vector<float> raw_;               ///< effective values before the kick's constraints, per id
    std::vector<float> rawKnob_;           ///< the knob value raw_ was computed from
    std::vector<uint8_t> rawDirty_;        ///< 1: the offset or override moved since raw_ was computed
    bool ownSeen_[9] = {};                 ///< the own-sound switches as the last pass saw them
    bool rawValid_ = false;                ///< false: recompute everything (construction, reset)
    bool paramsPending_ = false;           ///< control events dispatched, applyParams() not yet run for them
    double polyBpm_[kPolyInstances] = {};  ///< the tempo each voice's last Poly::update() was given
    /** @brief Runs applyParams() if control events are waiting for it (before a note, a render or the next chunk). */
    void flushParams() { if (paramsPending_) applyParams(); }
    /** @} */

    Kick kick_;   ///< the kick
    Bass bass_;   ///< the bass
    PercKit perc_;   ///< the twelve percussion lanes
    int keyRoot_ = 6;   ///< the key's pitch class (0 = C)
    int scale_ = 1;   ///< the scale (index into kScaleSteps)
    bool percMute_ = false;   ///< mix.perc_mute
    float percGain_ = 1.0f;   ///< the percussion strip's gain (mute and level)
    std::vector<float> percL_, percR_;   ///< the percussion's output of a segment
    int pattern_ = 0;   ///< compose.bass_pattern, for the bass's first slot (firstSlotSeconds)
    /**
     * @brief The first sounding bass slot of the current beat in beats, or <= 0 for "derive it".
     *
     * Set by a `ControlEvent::Kind::BassSlot` and read by firstSlotSeconds(). While it is <= 0 --
     * which it is on every bar the composer plays from a pattern family, because such a bar sends a
     * clearing event of -1 -- the engine derives the slot from `firstBassSlot(pattern_)` exactly as
     * it did before 16.09.2026, so a Pattern render is bit-identical to every earlier one. That
     * clearing event is what lets `compose.bass_rhythm` be switched back *during* playback: without
     * it the engine could not tell "nothing sent yet" from "nothing sent any more".
     */
    double slotBeats_ = -1.0;
    int lockMode_ = 2;   ///< bass.kick_lock (KickLock): who follows whom in the phase lock
    double bassPhase_ = 0.0;               ///< fundamental phase for the next bass note
    bool kickMute_ = false, bassMute_ = false;   ///< mix.kick_mute and mix.bass_mute
    float masterGain_ = 1.0f, ceiling_ = 1.0f;   ///< master.gain (with Auto Gain's offset) and master.ceiling, linear
    StemTap* tap_ = nullptr;               ///< where the stems go (setStemTap), null = nowhere
    bool metering_ = false;                ///< setMetering()
    float meterPeak_[kNumParts] = {};      ///< takeMeters(): peak per part
    double meterSum_[kNumParts] = {};      ///< takeMeters(): sum of squares per part
    int meterCount_ = 0;                   ///< takeMeters(): samples gathered
    /** @brief Adds one stereo sample of part @p part to the meters. */
    void meterAdd(int part, float l, float r)
    {
        const float a = std::max(std::fabs(l), std::fabs(r));
        if (a > meterPeak_[part]) meterPeak_[part] = a;
        meterSum_[part] += 0.5 * (static_cast<double>(l) * l + static_cast<double>(r) * r);
    }
    std::vector<int8_t> ownOf_;            ///< per parameter: which own-sound switch governs it (0 kick .. 8 drone), -1 none
    int liveTarget_[128];                  ///< per pitch: the voice a played note went to (0 acid, 1.. poly), -1 none
    unsigned livePlayed_ = 0;              ///< voices the keyboard has played ("By channel" replaces from the first note)
    /** @brief The voice the keyboard plays on @p channel (0 acid, 1 .. 6 poly), -1 none. */
    int keyboardTarget(int channel) const;
    bool clip_ = true;   ///< master.clip: the safety clip after the limiter
    TanhAdaa clipL_, clipR_;   ///< the safety clip, antiderivative anti-aliased

    std::vector<float> kickBuf_, bassBuf_;   ///< the kick's and the bass's output of a segment

    Acid acid_;   ///< the acid line
    Poly poly_[kPolyInstances];   ///< the six polyphonic voices, PolyInstance order
    Sfx sfx_;   ///< the effect generator
    std::vector<float> acidL_, acidR_, polyL_[kPolyInstances], polyR_[kPolyInstances], sfxL_, sfxR_;   ///< the generators' outputs of a segment
    /// The wandering trajectory's reverb-send share (20.09.2026, round "wandering-fx"; Sfx.h,
    /// sfx.wander): energy the dry sfxL_/sfxR_ above lose as an event nears its tail, added straight into
    /// the plain hall's send in renderSegment() -- silent whenever sfx.wander is off.
    std::vector<float> sfxWetL_, sfxWetR_;

    Texture texture_;   ///< the shamanic bed
    Vocal vocal_;   ///< the voices
    std::vector<float> texL_, texR_, vocL_, vocR_, vocThrow_, subBuf_, throwIn_, sendL_, sendR_;   ///< bed and voice outputs, the voices' throw weight, the sub drop, and the send buses of a segment
    std::vector<float> bedSendL_, bedSendR_;   ///< the bed's share of the modulation send (bedFx_)
    /** @name The Field track (27.09.2026; FieldPlayer.h): the sampler on its own strip, and the NASA shots on the SFX strip's
     *  @{ */
    FieldPlayer field_;                        ///< the field recordings' sampler
    FieldShot shot_;                           ///< the NASA shots (SfxType::SpaceShot)
    std::vector<float> fieldL_, fieldR_;       ///< the sampler's output of a segment
    /** @} */

    /**
     * @brief Channel strips of the parts after kick and bass, in this order. The six polyphonic strips
     *        run from StripLead in the order of PolyInstance (19.09.2026), so StripLead + k is instance k.
     *        StripAcid .. StripStab is the melodic bus a stutter repeats: acid, the two leads, arp and stab;
     *        pad and drone keep holding underneath the glitch.
     */
    enum Strip : int { StripPerc = 0, StripAcid, StripLead, StripCounter, StripArp, StripStab, StripPad, StripDrone,
                       StripSfx, StripTexture, StripVocal, StripField, StripCount };
    static_assert(StripDrone - StripLead + 1 == kPolyInstances && StripStab - StripLead == static_cast<int>(PolyInstance::Stab),
                  "the polyphonic strips must follow PolyInstance");
    float stripGain_[StripCount] = {};   ///< each strip's gain (mute and level)
    float stripRoom_[StripCount] = {}, stripHall_[StripCount] = {};   ///< each strip's room and hall sends
    float stripPlate_[StripCount] = {};   ///< each strip's plate send (the middle plane, 25.09.2026)
    float stripFx_[StripCount] = {};       ///< send into the modulation chain (texture and vocal only)

    PsyFxChain sfxFx_, sendFx_;            ///< the SFX strip's insert, and the send chain (the voices')
    /**
     * @brief The bed's modulation send (24.09.2026): the same chain and knobs as sendFx_, without the spoken
     *        phrases' frequency shift.
     *
     * A frequency shifter moves every partial by the same number of hertz, so it turns any pitched sound
     * inharmonic. Every spoken phrase shifts the send chain by 8 to 25 Hz (dispatch, sendMotion_) -- meant for
     * the voice, which is speech and has no pitch to lose -- and the bed shared that chain: under the intro's
     * phrases the didgeridoo's tonic drone got a copy of itself a few hertz off every harmonic. The user,
     * asked to have every voice checked for such detunings after the pad's: "Bitte untersuche, ob es zu solchen
     * Verstimmungen auch in den anderen Stimmen kommen kann". Flanger and phaser, which only colour, stay.
     */
    PsyFxChain bedFx_;
    /** @name The per-voice modulation insert (20.09.2026, round "dialogue"; Params.h, PolyMod)
     *  One flanger and one phaser per polyphonic instance, behind the voice and before its strip gain,
     *  so that the comb / flanger / phaser colour the user's effect list asks for is a property of the
     *  *voice* and not of the effects bus alone. At poly.mod = Off neither is ticked and the strip is
     *  sample for sample the strip without inserts.
     *  @{ */
    Flanger polyFlanger_[kPolyInstances];   ///< each voice's flanger (and comb)
    Phaser  polyPhaser_[kPolyInstances];   ///< each voice's phaser
    int     polyMod_[kPolyInstances] = {};   ///< each voice's poly.mod (PolyMod)
    /** @} */
    float sendReturn_ = 0.7f;              ///< psyfx.return
    float motion_ = 0.7f;                  ///< psyfx.motion: how far events move the chains
    /**
     * @brief The event motion of one chain: which event last moved it, from when and for how long
     *        (in beats), and how far. Evaluated at every chunk start from the chunk's beat, so the motion
     *        is a function of the score alone.
     */
    struct Motion { double start = 0.0, length = 0.0; float shiftHz = 0.0f, flange = 0.0f; };
    Motion sfxShift_, sfxFlange_, sendMotion_;   ///< the SFX chain's shift and flanger strands, the send's
    /** @brief Evaluates the effect events' motion (shift and flanger) for the current chunk and hands it to the chains. */
    void applyMotion();

    TempoDelay throw_;                     ///< the vocal's delay throw
    float throwSend_ = 0.6f;   ///< vocal.throw_send
    Stutter stutter_;                      ///< buffer repeat of the melodic bus
    Ducker subDuck_;                       ///< the kick's hold on the sub drop
    Ducker duck_[StripCount];   ///< each strip's duck under the kick
    Ducker returnDuck_;   ///< the returns' duck under the kick
    /** @name The mix guide's two ducks that are not the kick's (25.09.2026)
     *  The counter gives way to the lead (mix.counter_duck, 1 .. 3 dB, back in 50 ms), and the pad's presence band
     *  -- a bell at 1.2 kHz, an octave and a half wide, over 500 Hz .. 3 kHz -- drops by mix.pad_lead_duck while a
     *  lead note sounds (its hold is the note's length, its release 80 ms): the guide's dynamic EQ, the pad
     *  stepping back exactly where and while the lead speaks. Both are driven by the lead's notes, as the kick's
     *  ducks are by the kick's: deterministic, no detector.
     *  @{ */
    Ducker leadDuck_;             ///< the counter's gain under the lead
    Ducker padBandDuck_;          ///< the pad's presence band under the lead
    Svf padBandL_, padBandR_;     ///< the band's filters (a bandpass whose share is subtracted)
    float counterDuck_ = 0.0f;    ///< mix.counter_duck
    float padBandCut_ = 1.0f;     ///< linear gain of the band at full depth (mix.pad_lead_duck)
    /** @} */
    TranceGate gate_[kPolyInstances];   ///< each voice's trance gate
    bool  gateOn_[kPolyInstances] = {};   ///< poly.gate per voice
    int   gatePattern_[kPolyInstances] = {};   ///< poly.gate_pattern per voice
    float gateDepth_[kPolyInstances] = {}, gateDuty_[kPolyInstances] = {}, gateTone_[kPolyInstances] = {};   ///< the gate's depth, duty and tone per voice
    double gateAttack_[kPolyInstances] = {}, gateRelease_[kPolyInstances] = {};   ///< the gate's attack and release per voice, in beats

    Reverb room_, hall_;   ///< the two send reverbs
    /** @brief The plate (25.09.2026): the middle plane of depth -- acid, counter, arp and stab -- between the dry front
     *         and the hall; short, bright, its pre-delay on the tempo grid (fx.plate_*). */
    Reverb plate_;
    float plateReturn_ = 0.0f;   ///< fx.plate_return, linear
    float plateToHall_ = 0.0f;   ///< fx.plate_to_hall: the plate's output share that feeds the far room (the addon)
    /** @name Distance (25.09.2026, the Dark-Ambient addon)
     *  One cue for the five of distance, per polyphonic voice: poly.distance against the voice's own plane
     *  (kDistancePlane) moves its level, a low pass, its plate send and its width together (distanceCue), so a
     *  voice is never near by one cue and far by another -- the addon's "half assignment" that sticks to the
     *  speakers. At the default the deltas are nil and nothing is processed.
     *  @{ */
    float distGain_[kPolyInstances] = {};       ///< linear gain against the plane's
    float distWet_[kPolyInstances] = {};        ///< plate send added (may take away)
    float distWidth_[kPolyInstances] = {};      ///< side gain against the plane's
    bool distLpOn_[kPolyInstances] = {};        ///< the low pass is engaged (the voice is further than its plane)
    Svf distLpL_[kPolyInstances], distLpR_[kPolyInstances];   ///< the low passes
    /** @} */
    float slowWidth_[kPolyInstances] = {};      ///< poly.slow_mod, for the width's free movement (Engine.cpp)
    int monitor_ = 0;                           ///< master.monitor
    float slowSide_ = 0.0f;                     ///< the free width movement's sine, renewed every 32 samples
    Svf monLp1_, monLp2_;                       ///< the sub monitor's fourth-order low pass at 80 Hz
    std::vector<float> plateInL_, plateInR_, plateOutL_, plateOutR_;   ///< the plate's send and return buffers
    float roomReturn_ = 0.5f, hallReturn_ = 0.5f;   ///< their returns, linear
    std::vector<float> roomInL_, roomInR_, hallInL_, hallInR_, roomOutL_, roomOutR_, hallOutL_, hallOutR_;   ///< the reverbs' inputs and outputs of a segment
    /**
     * @name The gated hall (20.09.2026, round "reverb"; A2-gated-reverb.md)
     * A second, dedicated hall a voice's hall_send is routed into instead of the plain one when its own
     * `hall_gate` is on (poly.hall_gate, acid.hall_gate) -- so gating is an independent per-voice choice
     * and never touches the plain hall's tail for the voices that leave it alone. Recipe is fixed
     * (Engine.cpp, applyParams()): the brief names a result ("a big hall ... ducked ... cut hard"), not
     * a set of knobs. hallGateOn_ is indexed like Strip (Reverb.h `Strip` below), so an off entry for a
     * strip that never sets it (perc, sfx, texture, vocal) reads false, its default.
     * @{
     */
    Reverb hallGate_;
    std::vector<float> hallGateInL_, hallGateInR_, hallGateOutL_, hallGateOutR_;   ///< the gated hall's input and output of a segment
    bool hallGateOn_[StripCount] = {};   ///< per strip: its hall send goes to the gated hall
    /// closeBeats_/holdBeats_/openBeats_ converted each chunk from fixed ms constants at the current
    /// tempo (Reverb::barGate() takes beats, like TranceGate::open() -- the same reason: tempo ramps
    /// between chunks, beats do not drift with it).
    double hallGateCloseBeats_ = 0.001, hallGateHoldBeats_ = 0.0, hallGateOpenBeats_ = 0.001;
    /** @} */

    BusCompressor comp_;   ///< the master's bus compressor
    Svf sideHp1_, sideHp2_;   ///< the mono bass: a fourth-order high pass on the side signal at master.mono_bass
    HalfbandUp<float> clipUpL_, clipUpR_;   ///< the clipper's 2x upsampling
    HalfbandDown<float> clipDownL_, clipDownR_;   ///< and its way back down
    bool clipperOn_ = true;   ///< master.clipper
    float clipperT_ = 1.0f;   ///< the clipper's threshold, linear
    /// The upper end of the programme (Dsp.h, BandLimit). It sits after the clipper and before the
    /// limiter on purpose: the limiter's ceiling is a true-peak claim, and a true-peak estimate is a
    /// band-limited reconstruction, so the limiter has to be handed a signal that lives inside its
    /// own band -- otherwise it reads 0.9 dB low and lets the ceiling through (measured 16.09.2026).
    BandLimit bandLimitL_, bandLimitR_;
    TruePeakLimiter limiter_;   ///< the lookahead true-peak limiter
    bool limiterOn_ = true;   ///< master.limiter
    LoudnessMeter meter_;   ///< the output meter (BS.1770)
};

} // namespace phos
