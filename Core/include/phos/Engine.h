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
#include <atomic>
#include <memory>
#include <vector>

namespace phos {

/** @brief The Phosphene engine. */
class Engine {
public:
    static constexpr int kChunk = 32;   ///< samples per time/parameter chunk

    Engine();

    /**
     * @brief Prepares for playback.
     * @param sampleRate   output rate
     * @param maxBlockSize largest block the host will use (any size works; larger ones are split)
     * @param quality      what the engine may spend per part (Quality.h); the default is the desktop
     *                     level, which is what the engine did before quality levels existed
     */
    void prepare(double sampleRate, int maxBlockSize, const Quality& quality = Quality::desktop());
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

private:
    void advanceRamps();
    void applyParams();
    void renderSegment(float* L, float* R, int offset, int count);
    void dispatch(const NoteEvent& e, double late);
    void dispatchControl(const ControlEvent& e);
    double firstSlotSeconds() const;

    ParamStore params_;
    double sr_ = 48000.0;
    Quality quality_;

    TempoMap tempo_;
    bool useTempoMap_ = false;

    EventRing<NoteEvent> notes_;
    EventRing<ControlEvent> controls_;
    uint64_t samples_ = 0;
    double chunkBeat_ = 0.0;
    double beatsPerSample_ = 0.0;
    int chunkPos_ = 0;
    std::atomic<double> beatNow_{ 0.0 };

    /** @brief State of a parameter's variation. */
    struct Variation {
        float offset = 0.0f;               ///< current normalised offset
        float from = 0.0f, to = 0.0f;      ///< ramp endpoints
        double start = 0.0, length = 0.0;  ///< ramp in beats; length 0 = not ramping
        float override = -1.0f;            ///< discrete override, < 0 = none
    };
    std::unique_ptr<Variation[]> var_;
    std::vector<float> eff_;               ///< effective values, indexed by global id

    Kick kick_;
    Bass bass_;
    PercKit perc_;
    int keyRoot_ = 6;
    int scale_ = 1;
    bool percMute_ = false;
    float percGain_ = 1.0f;
    std::vector<float> percL_, percR_;
    int pattern_ = 0;
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
    int lockMode_ = 2;
    double bassPhase_ = 0.0;               ///< fundamental phase for the next bass note
    bool kickMute_ = false, bassMute_ = false;
    float masterGain_ = 1.0f, ceiling_ = 1.0f;
    bool clip_ = true;
    TanhAdaa clipL_, clipR_;

    std::vector<float> kickBuf_, bassBuf_;

    Acid acid_;
    Poly poly_[kPolyInstances];
    Sfx sfx_;
    std::vector<float> acidL_, acidR_, polyL_[kPolyInstances], polyR_[kPolyInstances], sfxL_, sfxR_;
    /// The wandering trajectory's reverb-send share (20.09.2026, round "wandering-fx"; Sfx.h,
    /// sfx.wander): energy the dry sfxL_/sfxR_ above lose as an event nears its tail, added straight into
    /// the plain hall's send in renderSegment() -- silent whenever sfx.wander is off.
    std::vector<float> sfxWetL_, sfxWetR_;

    Texture texture_;
    Vocal vocal_;
    std::vector<float> texL_, texR_, vocL_, vocR_, vocThrow_, subBuf_, throwIn_, sendL_, sendR_;

    /**
     * @brief Channel strips of the parts after kick and bass, in this order. The six polyphonic strips
     *        run from StripLead in the order of PolyInstance (19.09.2026), so StripLead + k is instance k.
     *        StripAcid .. StripStab is the melodic bus a stutter repeats: acid, the two leads, arp and stab;
     *        pad and drone keep holding underneath the glitch.
     */
    enum Strip : int { StripPerc = 0, StripAcid, StripLead, StripCounter, StripArp, StripStab, StripPad, StripDrone,
                       StripSfx, StripTexture, StripVocal, StripCount };
    static_assert(StripDrone - StripLead + 1 == kPolyInstances && StripStab - StripLead == static_cast<int>(PolyInstance::Stab),
                  "the polyphonic strips must follow PolyInstance");
    float stripGain_[StripCount] = {};
    float stripRoom_[StripCount] = {}, stripHall_[StripCount] = {};
    float stripFx_[StripCount] = {};       ///< send into the modulation chain (texture and vocal only)

    PsyFxChain sfxFx_, sendFx_;            ///< the SFX strip's insert, and the send chain
    /** @name The per-voice modulation insert (20.09.2026, round "dialogue"; Params.h, PolyMod)
     *  One flanger and one phaser per polyphonic instance, behind the voice and before its strip gain,
     *  so that the comb / flanger / phaser colour the user's effect list asks for is a property of the
     *  *voice* and not of the effects bus alone. At poly.mod = Off neither is ticked and the strip is
     *  sample for sample the strip of before this round.
     *  @{ */
    Flanger polyFlanger_[kPolyInstances];
    Phaser  polyPhaser_[kPolyInstances];
    int     polyMod_[kPolyInstances] = {};
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
    void applyMotion();

    TempoDelay throw_;                     ///< the vocal's delay throw
    float throwSend_ = 0.6f;
    Stutter stutter_;                      ///< buffer repeat of the melodic bus
    Ducker subDuck_;                       ///< the kick's hold on the sub drop
    Ducker duck_[StripCount];
    Ducker returnDuck_;
    TranceGate gate_[kPolyInstances];
    bool  gateOn_[kPolyInstances] = {};
    int   gatePattern_[kPolyInstances] = {};
    float gateDepth_[kPolyInstances] = {}, gateDuty_[kPolyInstances] = {}, gateTone_[kPolyInstances] = {};
    double gateAttack_[kPolyInstances] = {}, gateRelease_[kPolyInstances] = {};

    Reverb room_, hall_;
    float roomReturn_ = 0.5f, hallReturn_ = 0.5f;
    std::vector<float> roomInL_, roomInR_, hallInL_, hallInR_, roomOutL_, roomOutR_, hallOutL_, hallOutR_;
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
    std::vector<float> hallGateInL_, hallGateInR_, hallGateOutL_, hallGateOutR_;
    bool hallGateOn_[StripCount] = {};
    /// closeBeats_/holdBeats_/openBeats_ converted each chunk from fixed ms constants at the current
    /// tempo (Reverb::barGate() takes beats, like TranceGate::open() -- the same reason: tempo ramps
    /// between chunks, beats do not drift with it).
    double hallGateCloseBeats_ = 0.001, hallGateHoldBeats_ = 0.0, hallGateOpenBeats_ = 0.001;
    /** @} */

    BusCompressor comp_;
    Svf sideHp1_, sideHp2_;
    HalfbandUp<float> clipUpL_, clipUpR_;
    HalfbandDown<float> clipDownL_, clipDownR_;
    bool clipperOn_ = true;
    float clipperT_ = 1.0f;
    /// The upper end of the programme (Dsp.h, BandLimit). It sits after the clipper and before the
    /// limiter on purpose: the limiter's ceiling is a true-peak claim, and a true-peak estimate is a
    /// band-limited reconstruction, so the limiter has to be handed a signal that lives inside its
    /// own band -- otherwise it reads 0.9 dB low and lets the ceiling through (measured 16.09.2026).
    BandLimit bandLimitL_, bandLimitR_;
    TruePeakLimiter limiter_;
    bool limiterOn_ = true;
    LoudnessMeter meter_;
};

} // namespace phos
