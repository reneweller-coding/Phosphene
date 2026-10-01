/**
 * @file TranceGate.h
 * @brief Tempo-synchronised amplitude gate for a channel (pad, lead, arp), with a tone duck.
 *
 * The gate's opening is a function of the beat position alone: which step of the pattern the beat
 * lies in, and where inside that step. Each open step rises with a raised-cosine attack, holds for the
 * duty cycle and falls with a raised-cosine release -- continuous in value and slope, so the gate never
 * clicks, however short the edges. Because nothing is carried from sample to sample except the tone
 * filter, the gate is the same whatever the host's blocks, and a gate switched on in the middle of a
 * bar is exactly in phase with the grid.
 *
 * **Tone duck.** A gate that only turns the level down sounds like a volume knob; the classic hardware
 * gates also closed a filter. Here the closed part of each step crossfades towards a one-pole low pass
 * at 700 Hz by Tone, so the pad goes dark as well as quiet between the pulses.
 */
#pragma once
#include "phos/Dsp.h"

namespace phos {

constexpr int kNumGatePatterns = 6;   ///< entries of kGatePatternNames (Params.cpp)

/** @brief Gate state of one stereo channel. */
class TranceGate {
public:
    /** @brief Sets the sample rate (for the tone filter). */
    void prepare(double sampleRate)
    {
        lpCoef_ = static_cast<float>(1.0 - std::exp(-2.0 * 3.141592653589793 * 700.0 / sampleRate));
        reset();
    }
    /** @brief Clears the tone filter. */
    void reset() { lpL_ = lpR_ = 0.0f; }

    /**
     * @brief How open the gate is at a beat position, 0..1.
     * @param beat         beats from the start of the set
     * @param pattern      0 sixteenths, 1 eighths, 2 rolling (.xxx), 3 gallop (x.xx), 4 3-3-2, 5 eighth triplets
     * @param duty         open share of a step, 0.05..1
     * @param attackBeats  rise time
     * @param releaseBeats fall time
     */
    static float open(double beat, int pattern, float duty, double attackBeats, double releaseBeats)
    {
        // Steps per beat and the pattern over one bar of steps.
        static constexpr int kSteps[kNumGatePatterns] = { 4, 2, 4, 4, 4, 3 };
        static constexpr unsigned kMask[kNumGatePatterns] = {
            0xFFFFu,                     // x x x x x x x x x x x x x x x x
            0x00FFu,                     // eighths: every step of two per beat
            0xEEEEu,                     // . x x x per beat (bit 0 = first step)
            0xDDDDu,                     // x . x x per beat
            0x4949u | 0x0000u,           // x . . x . . x . (3-3-2 of sixteenths), twice per bar
            0x0FFFu,                     // twelve triplet eighths
        };
        const int p = pattern < 0 ? 0 : (pattern >= kNumGatePatterns ? kNumGatePatterns - 1 : pattern);
        const int perBeat = kSteps[p];
        const int perBar = perBeat * 4;
        const double pos = beat * perBeat;
        const double fl = std::floor(pos);
        long long step = static_cast<long long>(fl) % perBar;
        if (step < 0) step += perBar;
        if (((kMask[p] >> step) & 1u) == 0u) return 0.0f;
        const double stepBeats = 1.0 / perBeat;
        const double t = (pos - fl) * stepBeats;                  // beats into the step
        const double d = clampv(static_cast<double>(duty), 0.05, 1.0) * stepBeats;
        const double a = std::min(attackBeats, 0.5 * d);
        const double r = std::min(releaseBeats, stepBeats - d);
        auto rc = [](double x) { return 0.5 - 0.5 * std::cos(3.141592653589793 * x); };
        if (t < a) return static_cast<float>(rc(t / a));
        if (t < d) return 1.0f;
        if (r > 0.0 && t < d + r) return static_cast<float>(rc(1.0 - (t - d) / r));
        return 0.0f;
    }

    /**
     * @brief Applies the gate to one stereo sample.
     * @param o     opening from open()
     * @param depth 0..1, how far a closed gate lowers the level
     * @param tone  0..1, how far a closed gate darkens
     * @param l,r   the stereo sample, gated in place
     */
    inline void apply(float& l, float& r, float o, float depth, float tone)
    {
        lpL_ += lpCoef_ * (l - lpL_);
        lpR_ += lpCoef_ * (r - lpR_);
        const float closed = 1.0f - o;
        const float dark = tone * closed;
        const float gain = 1.0f - depth * closed;
        l = gain * (l + dark * (lpL_ - l));
        r = gain * (r + dark * (lpR_ - r));
    }

private:
    float lpCoef_ = 0.09f;   ///< the gate tone's low pass: coefficient
    float lpL_ = 0.0f;   ///< ... state, left
    float lpR_ = 0.0f;   ///< ... state, right
};

} // namespace phos
