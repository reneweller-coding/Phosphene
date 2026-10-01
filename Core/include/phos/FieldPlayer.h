/**
 * @file FieldPlayer.h
 * @brief The Field track's sampler (27.09.2026): field recordings, looped with a crossfade, through envelopes and a filter.
 *
 * The user asked for a track of its own for field recordings -- "insbesondere für Forest" -- and for "einen ordentlichen
 * Sampler ..., mit ADSR-Hüllkurven, eventuell Filtern, Möglichkeiten die Samples überblendbar zu loopen, wenn sie keine
 * Loops sind ... Also ähnlich wie Kontakt." What that asks for, piece by piece:
 *
 *  - **Two layers per event, A and B.** Each plays a recording of the library (FieldLibrary.h) chosen by category and
 *    variation, with its own level, pitch (semitones and cents, by resampling), start point (and how far a start may
 *    wander, drawn per event, so the same recording does not always begin with the same bird) and direction. A layer's
 *    level and B's share (layer_mix) mix them -- rain over a forest, a river under insects.
 *  - **A loop that joins any stretch.** The loop runs over [loop_start, loop_end] of the recording and is closed by an
 *    equal-power crossfade of loop_xfade: over the last X before the loop's end a second read head runs from the loop's
 *    start, the two are mixed with cos / sin, and at the end the playhead jumps to where the second head is (start + X).
 *    What the ear hears at the seam is always a crossfade between two stretches of the same recording, never a cut, so a
 *    recording that was not made as a loop plays as one; equal power because two stretches of a field recording are
 *    uncorrelated noise, whose sum keeps its level only when the gains' squares add up to one. The crossfade is at most
 *    half the loop. Reverse runs the same loop backwards. With the loop off a layer plays to the recording's end.
 *  - **Envelopes.** An amp ADSR (long by default: a recording enters under an intro over seconds and leaves a
 *    breakdown the same way) and a filter ADSR whose depth is env_amount octaves.
 *  - **A filter.** The state-variable filter (low, band, high pass, notch) or one of the nine circuit models the synths
 *    have (Filters.h), with the same level trims as the melodic voices; a 24 dB/octave low cut after it, because the depth
 *    rule leaves the range under 100 Hz to kick and bass, and a wind or a river has most of its power there.
 *  - **Modulation.** The synths' block (Modulation.h): a modulation envelope, four LFOs, eight matrix slots onto pitch,
 *    cutoff, resonance, the filter's mode, level and pan (kFieldModDestNames).
 *  - **Width and pan** on the voice's output.
 *
 * **Determinism.** The event's own seed (Engine.cpp, from its position and lane) draws the start and the random source,
 * the LFOs run on the engine's absolute sample count and beat. One thing is not in the score: whether the recording is
 * in memory. A layer whose recording is not loaded yet asks the library for it and starts when it arrives; an offline
 * render loads what the track will play before it renders (the composer's preload), so a render is the same every time.
 *
 * **SpaceShot.** The NASA recordings up to 25 s are effect shots: FieldShot plays one whole, once, at the effect's
 * velocity, on the SFX strip.
 */
#pragma once
#include "phos/Dsp.h"
#include "phos/FieldLibrary.h"
#include "phos/Filters.h"
#include "phos/Modulation.h"
#include "phos/Params.h"
#include <cstdint>

namespace phos {

/** @brief The Field track's sampler (part Field). */
class FieldPlayer {
public:
    static constexpr int kVoices = 3;     ///< events sounding at once (one entering while one releases, and one spare)
    static constexpr int kControl = 16;   ///< samples between two evaluations of modulation and filter

    /** @brief Prepares for @p sampleRate; @p seed seeds the LFOs' random shapes. */
    void prepare(double sampleRate, uint64_t seed = 0x46494Cull);
    /** @brief Silences every voice and gives its recordings back. */
    void reset();
    /** @brief Reads the effective parameters (indexed by field::). */
    void update(const float* v);
    /**
     * @brief Starts an event.
     * @param samples  its length: the amp envelope releases after it
     * @param velocity 0..1
     * @param late     sub-sample onset (0 <= late < 1)
     * @param semis    transposition in semitones (the note's pitch against middle C)
     * @param beat     the set's beat at the onset
     * @param pick     the event's own seed
     */
    void trigger(int samples, float velocity, double late, int semis, double beat, uint64_t pick);
    /** @brief Renders @p n samples whose first is at beat @p beat, replacing @p L and @p R. */
    void process(float* L, float* R, int n, double beat, double beatsPerSample);
    /** @brief Voices sounding. */
    int active() const;
    /** @brief The recording layer @p layer (0 A, 1 B) plays for category value @p categoryParam and @p variation (-1 none). */
    static int clipFor(int categoryParam, int variation, uint64_t pick);
    /** @brief The library indices the current settings name for layers A and B (-1: none), for a preload. */
    void wantedClips(int& a, int& b) const;
    /** @brief The latest voice's modulation sum per ModDest (the editor's live ring), zero when nothing sounds. */
    float displayModulation(ModDest d) const;

private:
    /** @brief One layer of a voice: a recording and a playhead on it. */
    struct Layer {
        int index = -1;              ///< library index, -1 none
        FieldClip* clip = nullptr;   ///< held while it plays (acquireFieldClip)
        bool on = false;             ///< the layer takes part in this event
        bool done = false;           ///< a layer without a loop has run off its recording's end
        double p = 0.0;              ///< playhead, in the recording's frames (negative: not placed yet)
        double start = 0.0;          ///< where it starts once the recording is there (0..1 of the length)
        double ls = 0.0;   ///< the loop's start, frames (control step)
        double le = 0.0;   ///< the loop's end, frames
        double x = 0.0;   ///< the loop's crossfade, frames
        double ratio = 1.0;          ///< frames per output sample (control step)
        bool loop = false;           ///< the loop is on (control step)
        float gain = 1.0f;           ///< its level and share
        float semis = 0.0f;          ///< its transposition
        bool reverse = false;        ///< reads backwards
    };
    /** @brief One event. */
    struct Voice {
        bool on = false;                 ///< sounding
        int64_t pos = 0;   ///< samples since the onset
        int64_t length = 1;   ///< the event's length, samples
        uint64_t age = 0;                ///< trigger order (stealing)
        float velocity = 1.0f;   ///< the event's velocity
        float random = 0.0f;   ///< the event's random source
        float keySemis = 0.0f;   ///< the event's transposition, semitones
        Layer layer[2];                  ///< A and B
        Envelope amp;   ///< the amp envelope
        Envelope filt;   ///< the filter envelope
        Modulator mod;                   ///< the modulation block
        Svf svfL;   ///< the state-variable filter, left
        Svf svfR;   ///< ... right
        Svf lowL1;   ///< the low cut's first section, left
        Svf lowR1;   ///< ... right
        Svf lowL2;   ///< the low cut's second section, left
        Svf lowR2;   ///< ... right
        FilterLane laneL;   ///< the circuit model, left
        FilterLane laneR;   ///< ... right
        float sums[kModDests] = {};      ///< the modulation's latest sums
        // The control step's results.
        float gainNow = 1.0f;   ///< the level now
        float panL = 1.0f;   ///< the pan's gain, left
        float panR = 1.0f;   ///< ... right
        float pitchMod = 0.0f;   ///< the pitch offset
        float g = 0.0f;   ///< the circuit model's tan(pi fc / fs)
        float k = 1.0f;   ///< ... its feedback
        float mode = 0.0f;   ///< ... its mode
        float trim = 1.0f;   ///< ... its makeup
    };
    /** @brief Sets a layer up for an event (its recording held or asked for). */
    void startLayer(Layer& l, int index, float gain, float semis, bool reverse, double start);
    /** @brief Silences a voice and gives its recordings back. */
    void freeVoice(Voice& v);
    /** @brief A layer's next stereo frame (interpolated, looped with the crossfade), advancing it by its ratio. */
    void readLayer(Layer& l, float& outL, float& outR);
    /** @brief A voice's control step: the modulation, the filter's coefficients, the layers' loops and ratios. */
    void control(Voice& v, int64_t at, double beat);

    double sr_ = 48000.0;            ///< sample rate
    Voice voice_[kVoices];           ///< the events
    uint64_t counter_ = 0;           ///< triggers so far (the voices' age)
    int64_t at_ = 0;                 ///< samples rendered since the last reset (the LFOs' clock)
    float v_[field::Count] = {};     ///< the effective parameters as update() read them
    int latest_ = -1;                ///< the latest voice (displayModulation)
    bool filterOff_ = true;          ///< the filter is transparent (LP at the top, no envelope, no modulation on it)
};

/** @brief The NASA shots (SfxType::SpaceShot): a recording of the category "nasa" up to 25 s, once, whole. */
class FieldShot {
public:
    static constexpr int kVoices = 2;   ///< shots at once
    static constexpr double kMaxSeconds = 25.0;   ///< longer NASA recordings are atmospheres, for the Field track
    /** @brief Prepares for @p sampleRate. */
    void prepare(double sampleRate) { sr_ = sampleRate; reset(); }
    /** @brief Silences every shot and gives its recordings back. */
    void reset();
    /** @brief Starts a shot drawn by @p pick at @p velocity. */
    void trigger(float velocity, double late, uint64_t pick);
    /** @brief Adds @p n samples into @p L and @p R. */
    void process(float* L, float* R, int n);
private:
    /** @brief One shot: its recording (held while it plays, or asked for until it is there) and its playhead. */
    struct Voice {
        FieldClip* clip = nullptr;   ///< the recording, null while it is asked for
        int index = -1;              ///< its index in the library
        double p = 0.0;              ///< the playhead, frames
        float gain = 0.0f;           ///< its level
        bool on = false;             ///< it sounds
        float pan = 0.0f;            ///< its place, -1 .. 1
    };
    double sr_ = 48000.0;       ///< sample rate
    Voice voice_[kVoices];      ///< the shots
    int next_ = 0;              ///< the voice the next shot takes
};

} // namespace phos
