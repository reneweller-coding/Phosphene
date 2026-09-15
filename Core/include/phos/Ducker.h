/**
 * @file Ducker.h
 * @brief Event-driven sidechain ducking.
 *
 * The kick's triggers are known from the score, so ducking does not need to listen to the kick's
 * audio: the gain curve starts on the trigger, sample-accurate and deterministic, with no detector
 * lag and no dependence on the kick's level. The curve is attack (linear, from wherever it is),
 * hold, and a raised-cosine release back to unity -- continuous in value everywhere, and in slope
 * at both ends of the release.
 */
#pragma once
#include "phos/Dsp.h"

namespace phos {

/** @brief Gain envelope triggered by events. */
class Ducker {
public:
    /** @brief Sets the sample rate. */
    void prepare(double sr) { sr_ = sr; reset(); }
    /** @brief Back to unity, idle. */
    void reset() { amount_ = 0.0f; stage_ = Stage::Idle; }
    /**
     * @brief Shape of the duck.
     * @param depth     0..1, how far the gain falls (1 = silence)
     * @param attackMs  time to full depth
     * @param holdMs    time held at full depth
     * @param releaseMs time back to unity
     */
    void set(float depth, float attackMs, float holdMs, float releaseMs)
    {
        depth_ = clampv(depth, 0.0f, 1.0f);
        attack_ = std::max(1, static_cast<int>(attackMs * 0.001 * sr_));
        hold_ = std::max(0, static_cast<int>(holdMs * 0.001 * sr_));
        release_ = std::max(1, static_cast<int>(releaseMs * 0.001 * sr_));
    }
    /** @brief Starts a duck now. */
    void trigger() { stage_ = Stage::Attack; pos_ = 0; from_ = amount_; }
    /** @brief Next gain factor. */
    inline float next()
    {
        switch (stage_) {
        case Stage::Attack:
            amount_ = from_ + (1.0f - from_) * static_cast<float>(pos_ + 1) / static_cast<float>(attack_);
            if (++pos_ >= attack_) { stage_ = Stage::Hold; pos_ = 0; amount_ = 1.0f; }
            break;
        case Stage::Hold:
            if (++pos_ >= hold_) { stage_ = Stage::Release; pos_ = 0; }
            break;
        case Stage::Release:
            amount_ = 0.5f + 0.5f * std::cos(kPi * static_cast<float>(pos_ + 1) / static_cast<float>(release_));
            if (++pos_ >= release_) { stage_ = Stage::Idle; amount_ = 0.0f; }
            break;
        default: break;
        }
        return 1.0f - depth_ * amount_;
    }
private:
    enum class Stage { Idle, Attack, Hold, Release };
    double sr_ = 48000.0;
    float depth_ = 0.5f, amount_ = 0.0f, from_ = 0.0f;
    int attack_ = 48, hold_ = 0, release_ = 2880, pos_ = 0;
    Stage stage_ = Stage::Idle;
};

} // namespace phos
