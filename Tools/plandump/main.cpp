/**
 * @file main.cpp
 * @brief phos_plandump: every number the composer's probe renders write into a TrackPlan, as bit patterns.
 *
 * Round "speed" (20.09.2026) changes *when* the probes run (in parallel, or not at all on a cache hit) and
 * must not change *what* they return. `phos_render --tracks` prints the plans rounded to a tenth of a dB,
 * which would hide exactly the kind of error a wrong schedule makes (a mix probe that ran before the
 * presence gain it depends on is off by hundredths of a LU). This tool prints the raw IEEE-754 patterns of
 * all fields the probes write, one line per track, so two builds or two settings can be compared with a
 * plain text diff or an MD5:
 *
 *     phos_plandump --seed 864566672 --tracks 20 [--set module.param=value ...] [--time]
 *     phos_plandump --data-id     (the core's build id, the shared data's hash and what hashing it costs)
 *     phos_plandump --decisions --seed 7 --tracks 3 [--set ...]
 *
 * With `--time` the wall time of the planning goes to stderr (never to stdout: the dump stays comparable).
 *
 * **`--decisions` (23.09.2026, round "Schnappschuss")** prints the other half of a plan: every decision the
 * composer makes -- walk, recipes, voices, percussion, harmony, lead design, form, effects -- as readable
 * lines, and after them one checksum per sixteen bars over every note and every control event the composer
 * sends for them. Tests/golden holds these dumps for a handful of seeds, and ctest compares against them
 * (Tests/plan_snapshot.cmake): any change to what the program composes shows up as a text diff of the
 * decisions that moved, which is the review question "was this meant?" asked of every commit. Continuous
 * values are printed at the resolution a decision has (energies to two places, recipe directions to three),
 * so the dump says *what* was decided and not how the floating point unit rounded it.
 */
#include "phos/Composer.h"
#include "phos/Engine.h"
#include "phos/Form.h"
#include "phos/Melody.h"
#include "phos/Sfx.h"
#include "phos/SoundPresets.h"
#include <algorithm>
#include <cmath>
#if __has_include("phos/Probe.h")
#include "phos/Probe.h"
#define PHOS_PLANDUMP_HAS_PROBE 1
#endif

#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

using namespace phos;

namespace {
/** @brief The bit pattern of a double. */
uint64_t bits(double v) { uint64_t u; std::memcpy(&u, &v, sizeof(u)); return u; }
/** @brief The bit pattern of a float. */
uint32_t bits(float v) { uint32_t u; std::memcpy(&u, &v, sizeof(u)); return u; }

/** @brief FNV-1a over 64-bit words. */
struct Fnv {
    uint64_t h = 1469598103934665603ull;   ///< the hash
    /** @brief Adds the eight bytes of @p v. */
    void add(int64_t v) { for (int i = 0; i < 8; ++i) { h ^= static_cast<uint64_t>(v >> (8 * i)) & 0xFFu; h *= 1099511628211ull; } }
    /** @brief Adds the bytes of @p s and its length. */
    void add(const std::string& s) { for (unsigned char c : s) { h ^= c; h *= 1099511628211ull; } add(static_cast<int64_t>(s.size())); }
};

/** @brief A beat position or length on a grid far finer than any the composer uses (1/3840 beat). */
int64_t tick(double beats) { return static_cast<int64_t>(std::llround(beats * 3840.0)); }

/** @brief Everything the composer decides for tracks 0 .. tracks-1, and a checksum per 16 bars of what it sends. */
void dumpDecisions(Composer& composer, const ParamStore& params, uint64_t seed, int tracks)
{
    static const char* const kRoman[7] = { "i", "ii", "iii", "iv", "v", "vi", "vii" };
    int lastBar = 0;
    for (int t = 0; t < tracks; ++t) {
        const TrackPlan p = composer.track(params, t);
        lastBar = std::max(lastBar, p.firstBar + p.bars);
        std::printf("seed %llu track %d: bars %d..%d, %.1f BPM, %s %s, %s, bass %s/%s gate %.2f, kick engine %d clip %d\n",
                    static_cast<unsigned long long>(seed), t + 1, p.firstBar, p.firstBar + p.bars - 1, p.bpm, kKeyNames[p.key], kScaleNames[p.scale],
                    kStyleNames[std::clamp(p.style, 0, kNumStyles - 1)], kBassPatternNames[p.primaryPattern], kBassPatternNames[p.secondaryPattern],
                    static_cast<double>(p.gate), p.kickEngine, p.kickClip);
        std::printf("  recipes: kick");
        for (int m = 0; m < kNumKickMacros; ++m) std::printf(" %+.3f", static_cast<double>(p.kickMacro[m]));
        std::printf("; bass");
        for (int m = 0; m < kNumBassMacros; ++m) std::printf(" %+.3f", static_cast<double>(p.bassMacro[m]));
        std::printf("; acid voicing %.3f %.3f %.3f\n", static_cast<double>(p.acidVoicing[0]), static_cast<double>(p.acidVoicing[1]), static_cast<double>(p.acidVoicing[2]));
        for (int v = 0; v < kPolyInstances; ++v) {
            const VoiceRecipe& rc = p.voice[v];
            std::printf("  voice %-8s osc %d table %d filter %d osc2 %d/%d (%s)", kPolyInstanceNames[v], rc.osc, rc.table, rc.filter, rc.osc2, rc.osc2Semis,
                        rc.hasOsc2 ? "drawn" : "knob");
            for (int k = 0; k < kNumVoiceMacros; ++k) std::printf(" %+.3f", static_cast<double>(rc.macro[k]));
            std::printf("\n");
        }
        std::printf("  perc: layers %d (hats %d), hat mode %d, clap %d, order", p.perc.layers, p.perc.hatLayers, p.perc.hatMode, p.perc.clapBackbeat ? 1 : 0);
        for (int i = 0; i < p.perc.layers; ++i) std::printf(" %d", p.perc.layerOrder[i]);
        std::printf("\n");
        const MelodyPlan& m = p.melody;
        std::printf("  harmony: %s, %d bars each:", m.progression == 1 ? "loop" : "pendulum", m.chordBars);
        for (int c = 0; c < 4; ++c) std::printf(" %s%s", kRoman[m.chordDegree[c]], kChordTypeNames[std::clamp(m.chordType[c], 0, kNumChordTypes - 1)]);
        if (m.secondHalf) {
            std::printf("; second half %s:", m.progression2 == 1 ? "loop" : "pendulum");
            for (int c = 0; c < 4; ++c) std::printf(" %s%s", kRoman[m.chordDegree2[c]], kChordTypeNames[std::clamp(m.chordType2[c], 0, kNumChordTypes - 1)]);
        }
        std::printf("; breakdown %s %d bars each:", m.breakHolds ? "holds" : "moves", m.breakChordBars);
        for (int c = 0; c < (m.breakHolds ? 1 : 4); ++c) std::printf(" %s%s", kRoman[m.breakDegree[c]], kChordTypeNames[std::clamp(m.breakType[c], 0, kNumChordTypes - 1)]);
        std::printf("\n  parts:");
        for (int k = 0; k < kMelodyParts; ++k) std::printf(" %d", m.present[k] ? 1 : 0);
        std::printf("; acid %d steps squelch %d; arp style %d; lead window %d, counter mode %d, density band %d\n", m.acidSteps, m.acidSquelch, m.arpStyle,
                    m.leadWindowLo, m.counterMode, m.leadDensityBand);
        for (int w = 0; w < 2; ++w) {
            std::printf("  lead %d: archetype %d cell %04x%s%s ops", w + 1, m.leadArchetype[w], m.leadCell[w], m.leadCellFromCorpus[w] ? " corpus" : "",
                        m.leadQuotesSet[w] ? " set-motif" : "");
            for (int b = 0; b < 8; ++b) std::printf(" %d%+d", m.leadOps[w][b], m.leadShift[w][b]);
            std::printf("\n");
        }
        std::printf("  form: template %d, hand-over %d, overlap tail %d, arc %.2f..%.2f\n", p.form.body, p.form.handover, p.form.overlapTail,
                    static_cast<double>(p.arcIn), static_cast<double>(p.arcOut));
        for (int s = 0; s < p.form.count; ++s) {
            const Section& sec = p.form.section[s];
            std::printf("    %-6s @%3d x%2d energy %.2f..%.2f scale %d%s roll %d pdb %d/%d cut %.0f%s%s%s\n", kSectionNames[static_cast<int>(sec.type)], sec.startBar, sec.bars,
                        static_cast<double>(sec.energy), static_cast<double>(sec.energyTo), sec.scale, sec.climax ? " climax" : "", sec.rollBars, sec.pdbBars,
                        sec.pdbVariant, static_cast<double>(sec.cutBeats), sec.spiral ? " spiral" : "", sec.dry ? " dry" : "", sec.breakPerc > 0 ? " perc" : "");
        }
        std::printf("  effects (%zu):\n", p.form.sfx.size());
        for (const SfxEvent& e : p.form.sfx)
            std::printf("    %-14s beat %7g length %5g preset %d\n", kSfxTypeNames[std::clamp(e.type, 0, kNumSfxTypes - 1)], e.beat, static_cast<double>(e.length), e.variant);
        // The Field track's places (27.09.2026, placeField): start, length, velocity.
        std::printf("  field (%zu):\n", p.form.field.size());
        for (const SfxEvent& e : p.form.field)
            std::printf("    beat %7g length %5g velocity %d\n", e.beat, static_cast<double>(e.length), e.variant);
    }
    // What the composer sends, sixteen bars at a time: a checksum over every note and every control event.
    std::vector<NoteEvent> notes;
    std::vector<ControlEvent> controls;
    composer.composeBars(params, 0, lastBar, notes, &controls);
    for (int w = 0; w * 16 < lastBar; ++w) {
        const double from = static_cast<double>(w) * 16.0 * kBeatsPerBar, to = from + 16.0 * kBeatsPerBar;
        Fnv hn, hc;
        int nn = 0, nc = 0;
        for (const NoteEvent& e : notes) {
            if (e.beat < from || e.beat >= to) continue;
            ++nn;
            hn.add(tick(e.beat)); hn.add(tick(e.length)); hn.add(static_cast<int>(e.part)); hn.add(e.lane); hn.add(e.pitch); hn.add(e.velocity); hn.add(e.flags);
        }
        for (const ControlEvent& c : controls) {
            if (c.beat < from || c.beat >= to) continue;
            ++nc;
            // The parameter by its key, not its global id (23.09.2026): a parameter added to the store shifts every id
            // after it, and a snapshot has to say what the composer does, not where the table's rows ended up.
            hc.add(tick(c.beat)); hc.add(tick(c.length)); hc.add(std::llround(static_cast<double>(c.value) * 1000.0));
            hc.add(c.param >= 0 && c.param < params.count() ? params.key(c.param) : std::to_string(c.param));
            hc.add(static_cast<int>(c.kind));
        }
        std::printf("bars %4d..%4d: %5d notes %016llx, %5d controls %016llx\n", w * 16, w * 16 + 15, nn, static_cast<unsigned long long>(hn.h), nc,
                    static_cast<unsigned long long>(hc.h));
    }
}
} // namespace

/** @brief Renders what the command line asks for; the exit code is 0 on success. */
int main(int argc, char** argv)
{
    uint64_t seed = 1;
    int tracks = 2;
    bool timeIt = false, dataId = false, decisions = false, presets = false;
    std::vector<std::string> sets;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--seed") seed = std::strtoull(next(), nullptr, 10);
        else if (a == "--tracks") tracks = std::atoi(next());
        else if (a == "--set") sets.push_back(next());
        else if (a == "--time") timeIt = true;
        else if (a == "--data-id") dataId = true;
        else if (a == "--decisions") decisions = true;
        else if (a == "--presets") presets = true;
        else { std::fprintf(stderr, "usage: phos_plandump [--seed N] [--tracks N] [--set module.param=value]... [--time] [--decisions]\n"); return 2; }
    }
#if defined(PHOS_PLANDUMP_HAS_PROBE)
    // A development tool: parallel probes and the probe cache as the environment says (Probe.h).
    probe::configureFromEnvironment();
#endif
#if defined(PHOS_PLANDUMP_HAS_PROBE)
    if (dataId) {
        // What the cache key's data hash costs per probe (Probe.cpp, sharedDataId), and what the core calls itself.
        probe::warmSharedData();
        const auto d0 = std::chrono::steady_clock::now();
        uint64_t id = 0;
        for (int i = 0; i < 20; ++i) id = probe::sharedDataId();
        const double ms = std::chrono::duration<double>(std::chrono::steady_clock::now() - d0).count() * 1000.0 / 20.0;
        std::printf("build id %s, shared data id %016" PRIx64 " (%.2f ms per call)\n", probe::buildId().c_str(), id, ms);
        return 0;
    }
#else
    (void)dataId;
#endif
    // --presets (24.09.2026): every factory preset as "module instance | group | name | key=value ...", so what a
    // preset sets can be read and counted -- the user asked why presets shift oscillators so far.
    if (presets) {
        static const char* const kVoice[] = { "lead", "counter", "arp", "stab", "pad", "drone" };
        auto dump = [](const char* who, Module m, int instance) {
            for (const SoundPreset& sp : factoryPresets(m, instance)) {
                std::string text = sp.text;
                std::replace(text.begin(), text.end(), '\n', ' ');
                std::printf("%s | %s | %s | %s\n", who, sp.group.c_str(), sp.name.c_str(), text.c_str());
            }
        };
        dump("kick", Module::Kick, 0);
        dump("bass", Module::Bass, 0);
        dump("acid", Module::Acid, 0);
        for (int v = 0; v < kPolyInstances; ++v) dump(kVoice[v], Module::Poly, v);
        return 0;
    }
    auto engine = std::make_unique<Engine>();   // only for its ParamStore, as phos_render has it
    ParamStore& params = engine->params();
    for (const std::string& s : sets) {
        std::string err;
        if (!params.parseText(s, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    }
    Composer composer(seed);
    const auto t0 = std::chrono::steady_clock::now();
    if (decisions) {
        dumpDecisions(composer, params, seed, tracks);
        if (timeIt) std::fprintf(stderr, "decisions of %d tracks in %.2f s\n", tracks, std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count());
        return 0;
    }
    for (int t = 0; t < tracks; ++t) {
        const TrackPlan p = composer.track(params, t);
        std::printf("seed %" PRIu64 " track %d loud %016" PRIx64 " gain %08x mix %016" PRIx64 " master %08x pres %016" PRIx64 " %016" PRIx64 " %08x parts",
                    seed, t, bits(p.loudness), bits(p.gainDb), bits(p.mixLoudness), bits(p.masterGainDb),
                    bits(p.presenceDb), bits(p.presenceAfterDb), bits(p.presenceGainDb));
        for (int k = 0; k < kMelodyParts; ++k) std::printf(" %016" PRIx64 ":%08x", bits(p.partLoudness[k]), bits(p.partGainDb[k]));
        std::printf("  | %.3f LUFS, gain %+.3f, mix %.3f, master %+.3f, presence %+.3f -> %+.3f (%+.3f dB)\n", p.loudness,
                    static_cast<double>(p.gainDb), p.mixLoudness, static_cast<double>(p.masterGainDb), p.presenceDb, p.presenceAfterDb,
                    static_cast<double>(p.presenceGainDb));
        std::fflush(stdout);
    }
    if (timeIt) {
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::fprintf(stderr, "planned %d tracks of seed %" PRIu64 " in %.2f s (%.2f s per track)\n", tracks, seed, s, s / std::max(1, tracks));
    }
    return 0;
}
