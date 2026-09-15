/**
 * @file Bass.h
 * @brief The rolling psytrance bass: retriggered VA oscillator into a driven ladder, 2x oversampled.
 *
 * Signal path per note: PolyBLEP saw/pulse plus sine sub (Oscillator.h) at twice the sample rate,
 * drive, the nonlinear zero-delay ladder (Ladder.h) with a fast exponential filter envelope,
 * half-band decimation (Halfband.h), amplitude ADSR, event-driven ducking against the kick
 * (Ducker.h), level.
 *
 * What makes the bass "roll" is that every sixteenth is the same note: the oscillator phase is reset
 * on every note (Retrigger), the filter envelope restarts from its peak, and the gate is short
 * enough that the note has died before the next one begins. The composer places the notes between
 * the kicks; the ducker keeps the sub band clear even when a pattern overlaps the kick's tail.
 *
 * The oscillator and the ladder run at the doubled rate because the ladder's saturation creates
 * harmonics of an already bright waveform; the cutoff and its envelope are computed once per base
 * sample, which at a 3 ms minimum filter decay is ample.
 */
#pragma once
#include "phos/Ducker.h"
#include "phos/Dsp.h"
#include "phos/Halfband.h"
#include "phos/Ladder.h"
#include "phos/Oscillator.h"

namespace phos {

class ParamStore;

/** @brief Monophonic bass synthesizer. */
class Bass {
public:
    /** @brief Prepares for a sample rate. */
    void prepare(double sampleRate);
    /** @brief Silences and clears all state. */
    void reset();
    /** @brief Reads the parameters. */
    void update(const ParamStore& p, int base);
    /**
     * @brief Starts a note now.
     * @param pitch       MIDI note number
     * @param velocity    0..1
     * @param gateSamples samples until the note is released
     */
    void noteOn(int pitch, float velocity, int gateSamples);
    /** @brief Starts a sidechain duck (called on every kick). */
    void duck() { ducker_.trigger(); }
    /** @brief Renders @p n samples, replacing @p out. */
    void process(float* out, int n);
    /** @brief Whether the amplitude envelope is open. */
    bool active() const { return amp_.isActive(); }

private:
    double sr_ = 48000.0;
    VaOscillator      osc_;
    LadderT<float>    ladder_;
    HalfbandDown<float> down_;
    Envelope          amp_;
    Ducker            ducker_;

    int    pitch_ = 36;
    float  velocity_ = 1.0f;
    int    gate_ = 0;
    float  fenv_ = 0.0f;              ///< filter envelope, 1 at the note start

    // Settings from update().
    float  wave_ = 0.0f, pw_ = 0.5f, sub_ = 0.25f;
    bool   retrigger_ = true;
    float  startPhase_ = 0.5f;        ///< 0.5 starts the saw at its zero crossing: no DC step per note
    float  cutoff_ = 140.0f, k_ = 1.0f, envOct_ = 4.0f, keyTrack_ = 0.6f, velCut_ = 0.25f;
    float  fDecay_ = 0.999f;
    float  driveIn_ = 1.0f, driveOut_ = 1.0f;
    float  level_ = 0.5f;
};

} // namespace phos
