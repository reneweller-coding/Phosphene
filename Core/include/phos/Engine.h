/**
 * @file Engine.h
 * @brief The engine: plays score events sample-accurately through the generators, mixer and master.
 *
 * **Time.** Musical time advances on a fixed grid of 32-sample chunks counted from the start. At the
 * start of every chunk the tempo is read (from the tempo map, or from compose.bpm when no map is set)
 * and the parameters are applied; inside the chunk the beat of sample i is chunkBeat + i * beatsPerSample,
 * and the next chunk starts at chunkBeat + 32 * beatsPerSample. Because the grid is absolute, the
 * output does not depend on how a host cuts the stream into blocks: rendering with blocks of 1, 64 or
 * 4096 samples gives the same samples, bit for bit (checked in the self test).
 *
 * **Events.** The composer pushes note events into a lock-free ring (Score.h). An event fires at the
 * first sample whose beat has reached the event's beat. Events that arrive late fire immediately.
 *
 * **Threads.** process() runs on the audio thread and never allocates. pushEvent() may be called from
 * one other thread. Parameters may be written from any thread.
 */
#pragma once
#include "phos/Bass.h"
#include "phos/Clock.h"
#include "phos/Kick.h"
#include "phos/Params.h"
#include "phos/Score.h"
#include <atomic>
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
     * @param maxBlockSize largest block process() will be called with (larger blocks are split)
     */
    void prepare(double sampleRate, int maxBlockSize);
    /** @brief Back to beat 0; clears events and all sound. */
    void reset();

    /** @brief The parameters. */
    ParamStore& params() { return params_; }
    const ParamStore& params() const { return params_; }

    /** @brief Uses a tempo map instead of compose.bpm (call while stopped). */
    void setTempoMap(const TempoMap& map) { tempo_ = map; useTempoMap_ = true; }
    /** @brief Goes back to compose.bpm. */
    void clearTempoMap() { useTempoMap_ = false; }

    /** @brief Queues an event (producer thread); false if the ring is full. */
    bool pushEvent(const NoteEvent& e) { return ring_.push(e); }
    /** @brief Beat position of the next sample to be rendered (any thread). */
    double beatPosition() const { return beatNow_.load(std::memory_order_relaxed); }
    /** @brief Samples rendered since reset. */
    uint64_t samplePosition() const { return samples_; }

    /** @brief Renders @p n stereo samples. */
    void process(float* L, float* R, int n);

    /** @brief The kick, for tests and the editor's display. */
    const Kick& kick() const { return kick_; }

private:
    void applyParams();
    void renderSegment(float* L, float* R, int offset, int count);
    void dispatch(const NoteEvent& e);

    ParamStore params_;
    double sr_ = 48000.0;
    int maxBlock_ = 0;

    TempoMap tempo_;
    bool useTempoMap_ = false;

    EventRing<NoteEvent> ring_;
    uint64_t samples_ = 0;        ///< samples rendered since reset
    double chunkBeat_ = 0.0;      ///< beat at the start of the current chunk
    double beatsPerSample_ = 0.0; ///< for the current chunk
    int chunkPos_ = 0;            ///< samples of the current chunk already rendered
    std::atomic<double> beatNow_{ 0.0 };

    Kick kick_;
    Bass bass_;
    int keyRoot_ = 6;
    bool kickMute_ = false, bassMute_ = false;
    float masterGain_ = 1.0f, ceiling_ = 1.0f;
    bool clip_ = true;
    TanhAdaa clipL_, clipR_;

    std::vector<float> kickBuf_, bassBuf_;
};

} // namespace phos
