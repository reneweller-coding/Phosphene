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
 * frequencies are recomputed for it at the trigger. Since 16.09.2026 the lane's low cut may follow
 * that shift (@c perc.cut_track): a snare pitched up twelve semitones through a buildup roll gets its
 * high pass an octave higher with it, which is the "thin it while it rises" half of the roll. At
 * @c cut_track = 0 -- every lane but the snare -- nothing moves.
 *
 * **The auto-pan (16.09.2026).** The width round measured that the reference recordings are wide at
 * *equal* channel level (inter-channel level difference 1.5 to 2.2 dB rms over 85 ms windows, in
 * every band) while this kit leans on static placement: the kit's own figure is 5.3 dB. Each lane
 * therefore carries a tempo-synchronous swing of its position,
 *
 *     p(t) = p0 * [ sqrt(1 - D*D) + sqrt(2) * D * cos(2 pi t / T + phi_l) ],   phi_l in {0, pi}
 *
 * with p0 the lane's own @c perc.pan, D its @c perc.pan_depth and T its @c perc.pan_bars in bars of
 * the current tempo. Three decisions, each of them measured rather than chosen (docs/rounds/2026-09.md,
 * 16.09.2026, and @c Tools/ref_arrange.py --pan-bound):
 *
 * **1. The law keeps the width the width round calibrated.** Both figures of a constant-power panner
 * are readings of the same position: over a long window side/mid is E[1 - cos(p pi/2)] over
 * E[1 + cos(p pi/2)], which to second order is (pi/4)^2 E[p^2]. The factors sqrt(1 - D^2) and
 * sqrt(2) D are exactly the pair for which E[p^2] = p0^2 at *every* depth -- the standing part
 * shrinks as the swinging part grows -- so the kit's side/mid does not move with the depth knob. A
 * law with (1 - D) in place of sqrt(1 - D*D) holds only at D = 0 and D = 1 and narrows the kit by up
 * to 2.6 dB in between; that was measured on the model before this was written.
 *
 * **2. The period is three sixteenths, and that number is not decoration.** A lane hit on the
 * sixteenth grid samples its own LFO at the phases 0, 2pi/3, 4pi/3 and nothing else. Three points
 * 120 degrees apart reproduce the first *and* the second moment of a sinusoid exactly (sum of
 * cos = 0, sum of cos^2 = 3/2), so a sixteenth-grid lane realises E[p] and E[p^2] of the continuous
 * swing exactly, not on average -- the side/mid of the kit is preserved for the material that
 * actually plays. Two sixteenths would sample at 0 and pi only, giving E[p^2] = 2 p0^2 and a kit
 * 1.4 dB too wide; the review asked for "a 3/16 polymeter that precesses against the bar" and the
 * arithmetic says why that is the right one. The period realigns with the bar every three bars.
 *
 * **3. The phases are two, and they are balanced, not spread.** What the 85 ms measurement reads is
 * the power-weighted *mean* position of the lanes sounding in that window; it falls when the loud
 * lanes move in opposite directions and not when they merely move. The lanes are therefore split
 * into two groups of opposite phase by a greedy descending-weight partition (Graham, "Bounds on
 * multiprocessing timing anomalies", SIAM J. Appl. Math. 17, 1969: the classical greedy bound for
 * number partitioning), weight = lane power times |p0| times depth, so that sum over lanes of
 * w_l p_l(t) stays near zero at all times. Measured on the model against a golden-ratio (Weyl)
 * spread of twelve phases, which is the obvious alternative: balanced 3.59 dB, Weyl 4.46 dB, static
 * 4.42 -- spreading the phases *raises* the level difference, because it leaves the weighted sum a
 * random walk instead of cancelling it.
 *
 * Panning stays constant power throughout (the kernel rotates the gain pair, PercKernel.h), so no
 * band balance and no loudness figure of the mix round can move; that is algebra, not a measurement.
 * The phasor turns once per sample, so the movement is bit-identical whatever block size the host
 * renders in, and at D = 0 the rotation is the exact identity in every bit.
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
     * @brief Tells the kit the tempo, which is the time base of the auto-pan.
     *
     * The swing of a lane is given in bars (@c perc.pan_bars), so the kit needs the bar length to
     * turn it into a rotation per sample. Nothing else in the kit depends on the tempo. Until this is
     * called the kit assumes @c kDefaultBpm, the default of @c compose.bpm.
     *
     * **The caller** is @c Engine::applyParams (since 18.09.2026, round "mix-foundation"): it hands
     * the kit the tempo the current chunk is timed with, so the period follows the tempo map and a
     * ramp between tracks, and never changes inside a chunk. Before that the kit ran at kDefaultBpm
     * whatever the tempo; at +-4 BPM around 145 that was a 2.8 % error, and for a set driven far from
     * 145 BPM the "three sixteenths" of the swing were not three sixteenths any more.
     * @param bpm beats per minute (4/4, so a bar is four beats)
     */
    void setTempo(double bpm);
    /** @brief The tempo the auto-pan assumes until setTempo() says otherwise. */
    static constexpr double kDefaultBpm = 145.0;
    /** @brief The tempo the auto-pan currently runs at, in BPM. */
    double tempo() const { return bpm_; }
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
    /** @brief The pan angle a lane is rotated by right now, in radians (for the tests). */
    double panAngle(int lane) const;
    /** @brief The position a lane's panner stands at right now, in -1..1 (for the tests). */
    double panPosition(int lane) const;
    /** @brief Sign of a lane's swing: +1 and -1 are the two phase groups (for the tests). */
    int panGroup(int lane) const;
    /** @brief Signed swing of a lane's auto-pan, in radians of rotation (for the tests). */
    double panSwing(int lane) const;

private:
    /** @brief Recomputes lane @p lane's synthesis coefficients from its parameters. */
    void computeCoefs(int lane);
    /** @brief The auto-pan phasor's step for @p lane from its period and the tempo (all setTempo() changes). */
    void updatePanRate(int lane);
    /** @brief Gives each lane its auto-pan direction from its role's pan group. */
    void assignPanGroups();

    double sr_ = 48000.0;   ///< sample rate
    PercState s_;   ///< every lane's synthesis state (PercKernel.h)
    PercCoefs c_;   ///< every lane's coefficients (PercKernel.h)
    float values_[kPercLanes][perc::Count] = {};   ///< each lane's parameters as update() read them
    bool  valid_[kPercLanes] = {};   ///< the lane has been updated at least once
    int   keyRoot_[kPercLanes] = {}, scale_[kPercLanes] = {};   ///< the key and scale each lane was tuned to
    int   role_[kPercLanes] = {};   ///< perc.role per lane (PercRole)
    int   engine_[kPercLanes] = {};   ///< perc.engine per lane
    int   choke_[kPercLanes] = {};   ///< perc.choke group per lane (0 = none)
    double tunedHz_[kPercLanes] = {};   ///< each lane's pitch after tuning to the key
    double shiftMul_[kPercLanes] = {};   ///< the pitch factor of the last hit's semitone shift
    double bpm_ = kDefaultBpm;   ///< time base of the auto-pan
    bool   panning_ = false;     ///< any lane moves: one decision for the whole kit (PercKernel.h)
    float modeAmp_[kPercModes][kPercLanes] = {};   ///< the modal engine's mode amplitudes
    double modeW_[kPercModes][kPercLanes] = {};   ///< their angular frequencies per sample
    double modeR_[kPercModes][kPercLanes] = {};   ///< their per-sample radii (the decays)
    // Noise and bursts.
    Rng   noiseRng_[kPercLanes];   ///< each lane's noise source
    float noiseTail_[kPercLanes] = {}, noiseFast_[kPercLanes] = {};   ///< the noise's per-sample decay after the hit and between the bursts of a clap
    int   burstsLeft_[kPercLanes] = {};   ///< a clap's bursts still to come
    double burstTimer_[kPercLanes] = {}, burstSpacing_[kPercLanes] = {};   ///< samples to the next burst, and between bursts
    float burstVel_[kPercLanes] = {};   ///< the bursts' level
    float chokeFactor_ = 0.999f;   ///< the per-sample decay of a choked lane (8 ms)
    std::vector<float> noise_, reset_, dNoise_, outL_, outR_;   ///< a block's noise, burst restarts and noise decays per lane, and the kit's output
};

} // namespace phos
