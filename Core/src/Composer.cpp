/**
 * @file Composer.cpp
 * @brief Phase-1 composer: kick and bass patterns with phrase variations.
 */
#include "phos/Composer.h"
#include "phos/Dsp.h"
#include "phos/Engine.h"
#include "phos/Harmony.h"
#include "phos/Params.h"
#include <algorithm>

namespace phos {

namespace {

/** @brief Slots of a bass pattern within one beat, in beats. */
struct BassPatternDef {
    int count;
    double pos[3];
};

const BassPatternDef kBassPatterns[] = {
    { 3, { 0.25, 0.5, 0.75 } },               // Rolling
    { 2, { 0.5, 0.75, 0.0 } },                // Gallop
    { 2, { 0.25, 0.75, 0.0 } },               // Skip
    { 1, { 0.5, 0.0, 0.0 } },                 // Offbeat
    { 2, { 1.0 / 3.0, 2.0 / 3.0, 0.0 } },     // Triplet
};

/** @brief A figure for the last beat of a bar: pitch per slot, as scale degree plus octave. */
struct Figure {
    int degree[3];   ///< scale degree per slot (the last `count` entries are used)
    int octave[3];   ///< octave per slot
};

const Figure kFigures[] = {
    { { 0, 0, 0 }, { 0, 0, 1 } },     // root, root, octave
    { { 0, 0, 4 }, { 0, 1, 0 } },     // root, octave, fifth
    { { 0, 4, 0 }, { 1, 0, 0 } },     // octave, fifth, root
    { { 0, 1, 1 }, { 0, 0, 0 } },     // root, second, second (the Phrygian flat two)
    { { 6, 0, 0 }, { -1, 0, 0 } },    // seventh below, root, root
    { { 0, 2, 0 }, { 0, 0, 0 } },     // root, third, root
    { { 4, 0, 0 }, { 0, 1, 0 } },     // fifth, octave, root
};
constexpr int kNumFigures = static_cast<int>(sizeof(kFigures) / sizeof(kFigures[0]));

constexpr uint64_t kSaltPhrase = 0x5048524153450001ull;   ///< "PHRASE"

} // namespace

void Composer::composeBars(const ParamStore& p, int firstBar, int count, std::vector<NoteEvent>& out) const
{
    const int cb = p.base(Module::Compose);
    const int key = p.getInt(cb + compose::Key);
    const int scale = p.getInt(cb + compose::Scale);
    const int kickPattern = p.getInt(cb + compose::KickPattern);
    const int bassPattern = std::clamp(p.getInt(cb + compose::BassPattern), 0, 4);
    const float gate = p.get(cb + compose::BassGate);
    const float variation = p.get(cb + compose::BassVariation);
    const int root = bassRootNote(key, p.getInt(cb + compose::BassRegister));
    const BassPatternDef& pat = kBassPatterns[bassPattern];

    const size_t start = out.size();
    for (int bar = firstBar; bar < firstBar + count; ++bar) {
        const double barBeat = static_cast<double>(bar) * kBeatsPerBar;

        // Phrase decisions: which figures, if any, end bars 2 and 4 of this four-bar phrase.
        const int phrase = bar / 4;
        Rng rng;
        rng.seed(mixSeed(seed_ ^ kSaltPhrase, static_cast<uint64_t>(phrase)));
        const bool varyBar2 = rng.uniform() < variation * 0.6f;
        const bool varyBar4 = rng.uniform() < variation;
        const int figure2 = rng.below(kNumFigures);
        const int figure4 = rng.below(kNumFigures);
        const int barInPhrase = bar % 4;

        for (int beat = 0; beat < kBeatsPerBar; ++beat) {
            const double b = barBeat + beat;
            const bool fillGap = kickPattern == 1 && bar % 8 == 7 && beat == 3;
            if (kickPattern != 2 && !fillGap) {
                NoteEvent k;
                k.beat = b;
                k.length = 0.25f;
                k.part = Part::Kick;
                k.pitch = 36;
                k.velocity = 127;
                out.push_back(k);
            }

            int figure = -1;
            if (beat == 3 && barInPhrase == 1 && varyBar2) figure = figure2;
            if (beat == 3 && barInPhrase == 3 && varyBar4) figure = figure4;

            for (int s = 0; s < pat.count; ++s) {
                const double pos = pat.pos[s];
                const double next = s + 1 < pat.count ? pat.pos[s + 1] : 1.0;
                int pitch = root;
                if (figure >= 0) {
                    const int idx = 3 - pat.count + s;   // align figures to the end of the beat
                    const Figure& f = kFigures[figure];
                    pitch = root + scaleDegree(scale, f.degree[idx]) + 12 * f.octave[idx];
                }
                NoteEvent n;
                n.beat = b + pos;
                n.length = static_cast<float>((next - pos) * gate);
                n.part = Part::Bass;
                n.pitch = static_cast<uint8_t>(std::clamp(pitch, 0, 127));
                n.velocity = 110;
                out.push_back(n);
            }
        }
    }
    std::stable_sort(out.begin() + static_cast<long>(start), out.end(), noteLess);
}

Conductor::Conductor(Engine& engine, const Composer& composer) : engine_(engine), composer_(composer) {}

void Conductor::rewind()
{
    nextBar_ = 0;
    pending_.clear();
    pendingPos_ = 0;
}

void Conductor::pump(const ParamStore& params, double horizonBeats, std::vector<NoteEvent>* record)
{
    const double target = engine_.beatPosition() + horizonBeats;
    for (;;) {
        // Finish handing over what is already composed.
        while (pendingPos_ < pending_.size()) {
            if (!engine_.pushEvent(pending_[pendingPos_])) return;
            if (record != nullptr) record->push_back(pending_[pendingPos_]);
            ++pendingPos_;
        }
        if (static_cast<double>(nextBar_) * kBeatsPerBar >= target) return;
        pending_.clear();
        pendingPos_ = 0;
        composer_.composeBars(params, nextBar_, 1, pending_);
        ++nextBar_;
    }
}

} // namespace phos
