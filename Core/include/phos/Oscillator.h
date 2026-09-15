/**
 * @file Oscillator.h
 * @brief Band-limited virtual-analogue oscillator: PolyBLEP saw/pulse blend with a sine sub.
 *
 * A naive sawtooth has a discontinuity every period whose partials fold back above Nyquist as
 * inharmonic tones. PolyBLEP subtracts a two-sample polynomial approximation of the band-limited
 * step residual at every discontinuity (Valimaki and Huovilainen, "Antialiasing oscillators in
 * subtractive synthesis", IEEE Signal Processing Magazine 24(2), 2007). Run at twice the sample
 * rate and decimated through a half-band filter, as the bass does, the remaining aliasing lies far
 * below the ladder's own distortion.
 *
 * The phase can be reset at every note -- the "hard retrigger" that makes a rolling psytrance bass
 * identical from note to note -- and the reset is itself a discontinuity, corrected with a BLEP of
 * the jump's height.
 */
#pragma once
#include "phos/Dsp.h"

namespace phos {

/** @brief Two-point PolyBLEP residual for a unit step at phase 0; @p t phase in [0,1), @p dt increment. */
inline float polyBlep(float t, float dt)
{
    if (t < dt) { const float x = t / dt; return x + x - x * x - 1.0f; }
    if (t > 1.0f - dt) { const float x = (t - 1.0f) / dt; return x * x + x + x + 1.0f; }
    return 0.0f;
}

/** @brief PolyBLEP saw-to-pulse oscillator with a sine sub-oscillator an octave down. */
class VaOscillator {
public:
    /**
     * @brief Sets the shape for the next samples.
     * @param hz         frequency
     * @param sampleRate rate the oscillator runs at
     * @param wave       0 = saw, 1 = pulse, blended in between
     * @param pulseWidth duty cycle of the pulse, 0.05..0.95
     */
    void set(double hz, double sampleRate, float wave, float pulseWidth)
    {
        dt_ = static_cast<float>(clampv(hz / sampleRate, 1e-7, 0.45));
        wave_ = clampv(wave, 0.0f, 1.0f);
        pw_ = clampv(pulseWidth, 0.05f, 0.95f);
    }
    /**
     * @brief Restarts the phase; the jump from the current value is band-limited over two samples.
     * @param phase new phase in [0,1)
     */
    void resetPhase(float phase)
    {
        pendingJump_ = shapeAt(phase) - lastValue_;
        phase_ = phase;
        subPhase_ = 0.5 * phase;
        hasJump_ = true;
    }
    /** @brief Next main sample. */
    inline float next()
    {
        const float t = phase_;
        const float saw = 2.0f * t - 1.0f - polyBlep(t, dt_);
        float t2 = t + 1.0f - pw_;
        if (t2 >= 1.0f) t2 -= 1.0f;
        float pulse = (t < pw_ ? 1.0f : -1.0f) + polyBlep(t, dt_) - polyBlep(t2, dt_);
        float v = saw + wave_ * (pulse - saw);
        if (hasJump_) {
            // The first sample after a reset carries half of the missing step, the one before it
            // already went out; spreading the other half keeps the edge band-limited.
            v -= 0.5f * pendingJump_;
            hasJump_ = false;
        }
        lastValue_ = v;
        phase_ += dt_;
        if (phase_ >= 1.0f) phase_ -= 1.0f;
        subPhase_ += 0.5 * static_cast<double>(dt_);
        if (subPhase_ >= 1.0) subPhase_ -= 1.0;
        return v;
    }
    /** @brief Current sub-oscillator sample (sine an octave below); read after next(). */
    inline float sub() const { return sin01(subPhase_); }

private:
    float shapeAt(float t) const
    {
        const float saw = 2.0f * t - 1.0f;
        const float pulse = t < pw_ ? 1.0f : -1.0f;
        return saw + wave_ * (pulse - saw);
    }
    float  phase_ = 0.0f, dt_ = 0.001f, wave_ = 0.0f, pw_ = 0.5f;
    double subPhase_ = 0.0;
    float  lastValue_ = 0.0f, pendingJump_ = 0.0f;
    bool   hasJump_ = false;
};

} // namespace phos
