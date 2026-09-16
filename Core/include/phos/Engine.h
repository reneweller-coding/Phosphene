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
 * and sends to a short room and a long hall (Reverb.h), whose returns are ducked as well. The master
 * sums everything, applies the gain (knob, the track's level match and the composer's loudness offset),
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
#include "phos/Quality.h"
#include "phos/Score.h"
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

    /** @brief Channel strips of the parts after kick and bass, in this order. */
    enum Strip : int { StripPerc = 0, StripAcid, StripLead, StripArp, StripPad, StripSfx, StripCount };
    float stripGain_[StripCount] = {};
    float stripRoom_[StripCount] = {}, stripHall_[StripCount] = {};
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
