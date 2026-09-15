/**
 * @file Composer.h
 * @brief The composer (Phase 1: kick and rolling bass) and the conductor that feeds the engine.
 *
 * **Determinism by bar.** Every bar is composed from the set seed and the bar's own index -- phrase
 * decisions from a seed mixed with the phrase index -- never from a random stream carried along. So
 * bar 37 is bar 37 whether the set was played from the start, rendered offline, or recomposed after
 * a parameter change. That is also what later lets a single section be re-rolled while everything
 * around it stays bit-identical.
 *
 * **Kick.** Four on the floor. "Four + Fills" leaves out the last beat of every eighth bar, the
 * classic breath before a new phrase, while the bass keeps rolling through it.
 *
 * **Bass patterns** place notes in the gaps between the kicks, per beat:
 *  - Rolling  K-B-B-B  (sixteenths 2, 3, 4; full-on)
 *  - Gallop   K-.-B-B  (sixteenths 3, 4)
 *  - Skip     K-B-.-B  (sixteenths 2, 4)
 *  - Offbeat  K-.-B-.  (the eighth offbeat; progressive)
 *  - Triplet  K-B-B    (the two later triplet eighths; Goa)
 * A note lasts Gate times the distance to the next slot.
 *
 * **Variation.** Per four-bar phrase, with probability Variation, a figure replaces the last beat of
 * the second and/or fourth bar: octave jumps, the fifth, the scale's second degree (the Phrygian
 * flat second), the seventh below. Figures are written in scale degrees, so they stay in the mode.
 */
#pragma once
#include "phos/Score.h"
#include <cstdint>
#include <vector>

namespace phos {

class Engine;
class ParamStore;

/** @brief Composes bars of the score from a seed and a parameter snapshot. */
class Composer {
public:
    /** @brief @p seed identifies the set. */
    explicit Composer(uint64_t seed = 1) : seed_(seed) {}
    /** @brief Changes the set seed. */
    void setSeed(uint64_t seed) { seed_ = seed; }
    /** @brief The set seed. */
    uint64_t seed() const { return seed_; }

    /**
     * @brief Composes whole bars and appends their events, sorted.
     * @param params   parameter values to compose with (compose.* are read)
     * @param firstBar index of the first bar (bar 0 starts at beat 0)
     * @param count    number of bars
     * @param out      receives the events
     */
    void composeBars(const ParamStore& params, int firstBar, int count, std::vector<NoteEvent>& out) const;

private:
    uint64_t seed_;
};

/**
 * @brief Keeps the engine's event ring filled a few bars ahead of the play position.
 *
 * In the plugin this runs on the composer thread; the offline renderer calls pump() before every
 * block. Bars are pushed whole and in order; what does not fit into the ring waits for the next call.
 */
class Conductor {
public:
    /** @brief Binds an engine and a composer. */
    Conductor(Engine& engine, const Composer& composer);
    /** @brief Starts again from bar 0. */
    void rewind();
    /**
     * @brief Composes and pushes bars until @p horizonBeats beyond the engine's position are covered.
     * @param params parameter values to compose with
     * @param horizonBeats how far ahead to keep the ring filled
     * @param record if not null, every pushed event is appended here as well (for MIDI export)
     */
    void pump(const ParamStore& params, double horizonBeats, std::vector<NoteEvent>* record = nullptr);
    /** @brief Index of the next bar to be composed. */
    int nextBar() const { return nextBar_; }

private:
    Engine& engine_;
    const Composer& composer_;
    int nextBar_ = 0;
    std::vector<NoteEvent> pending_;
    size_t pendingPos_ = 0;
};

} // namespace phos
