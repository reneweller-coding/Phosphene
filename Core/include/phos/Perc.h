/**
 * @file Perc.h
 * @brief The percussion kit: twelve lanes of the universal percussion voice (PercKernel.h).
 *
 * The kit turns lane parameters into kernel coefficients, starts hits (with sub-sample onsets,
 * pitch shifts, chokes and clap bursts) and renders the lanes in registers: two AVX2 registers or
 * three NEON registers for the twelve lanes. Everything that is not a per-sample operation on all
 * lanes -- noise generation, burst schedules, triggers -- is scalar and shared by every vector path.
 *
 * **Tuning.** A lane with Tune to Key moves its pitch to the nearest note of the current key and scale,
 * so toms, congas and blips play in the track's mode.
 *
 * **Pitch shift.** A hit can carry a shift in semitones (the tom run of a fill); the lane's
 * frequencies are recomputed for it at the trigger.
 */
#pragma once
#include "phos/Dsp.h"
#include "phos/Params.h"
#include "phos/PercKernel.h"
#include <vector>

namespace phos {

/** @brief General MIDI drum note of each role (closed hat 42, open hat 46, ride 51, ...). */
inline constexpr int kPercRoleNote[kNumPercRoles] = { 42, 46, 51, 49, 39, 38, 37, 70, 45, 63, 76, 75 };

/** @brief The percussion kit. */
class PercKit {
public:
    static constexpr int kMaxBlock = 64;   ///< longest block process() renders at once (longer ones are split)

    /** @brief Prepares for a sample rate. */
    void prepare(double sampleRate);
    /** @brief Silences every lane and clears all state. */
    void reset();
    /**
     * @brief Reads one lane's effective parameters.
     * @param lane    0..11
     * @param v       values indexed by perc::
     * @param keyRoot pitch class of the key, for Tune to Key
     * @param scale   index into kScaleNames, for Tune to Key
     */
    void update(int lane, const float* v, int keyRoot, int scale);
    /**
     * @brief Starts a hit.
     * @param lane     0..11
     * @param velocity 0..1
     * @param shift    pitch shift in semitones for tonal and modal engines
     * @param late     how many samples ago the hit ideally started (0 <= late < 1)
     */
    void trigger(int lane, float velocity, int shift, double late);
    /** @brief Renders @p n stereo samples, replacing @p L and @p R. */
    void process(float* L, float* R, int n);
    /** @brief Renders with a chosen lane type (float = scalar reference), for the vector tests. */
    template <class V> void processWith(float* L, float* R, int n);

    /** @brief Role of a lane as last updated. */
    PercRole role(int lane) const { return static_cast<PercRole>(role_[lane]); }
    /** @brief Frequency a lane's tone and modes are tuned to (after Tune to Key, before a hit's shift). */
    double laneHz(int lane) const { return tunedHz_[lane]; }
    /** @brief Nearest note of the key and scale to @p hz, as a frequency. */
    static double tuneToScale(double hz, int keyRoot, int scale);

private:
    void computeCoefs(int lane);

    double sr_ = 48000.0;
    PercState s_;
    PercCoefs c_;
    float values_[kPercLanes][perc::Count] = {};
    bool  valid_[kPercLanes] = {};
    int   keyRoot_[kPercLanes] = {}, scale_[kPercLanes] = {};
    int   role_[kPercLanes] = {};
    int   engine_[kPercLanes] = {};
    int   choke_[kPercLanes] = {};
    double tunedHz_[kPercLanes] = {};
    double shiftMul_[kPercLanes] = {};
    float modeAmp_[kPercModes][kPercLanes] = {};
    double modeW_[kPercModes][kPercLanes] = {};
    double modeR_[kPercModes][kPercLanes] = {};
    // Noise and bursts.
    Rng   noiseRng_[kPercLanes];
    float noiseTail_[kPercLanes] = {}, noiseFast_[kPercLanes] = {};
    int   burstsLeft_[kPercLanes] = {};
    double burstTimer_[kPercLanes] = {}, burstSpacing_[kPercLanes] = {};
    float burstVel_[kPercLanes] = {};
    float chokeFactor_ = 0.999f;
    std::vector<float> noise_, reset_, dNoise_, outL_, outR_;
};

} // namespace phos
