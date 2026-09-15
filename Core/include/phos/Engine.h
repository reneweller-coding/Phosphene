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
 * **Threads.** process() runs on the audio thread and never allocates. The push functions may be
 * called from one other thread. Parameters may be written from any thread.
 */
#pragma once
#include "phos/Bass.h"
#include "phos/Clock.h"
#include "phos/Kick.h"
#include "phos/Params.h"
#include "phos/Perc.h"
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
     */
    void prepare(double sampleRate, int maxBlockSize);
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
};

} // namespace phos
