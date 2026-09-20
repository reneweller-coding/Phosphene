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
 *
 * With `--time` the wall time of the planning goes to stderr (never to stdout: the dump stays comparable).
 */
#include "phos/Composer.h"
#include "phos/Engine.h"
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
} // namespace

int main(int argc, char** argv)
{
    uint64_t seed = 1;
    int tracks = 2;
    bool timeIt = false, dataId = false;
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
        else { std::fprintf(stderr, "usage: phos_plandump [--seed N] [--tracks N] [--set module.param=value]... [--time]\n"); return 2; }
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
    auto engine = std::make_unique<Engine>();   // only for its ParamStore, as phos_render has it
    ParamStore& params = engine->params();
    for (const std::string& s : sets) {
        std::string err;
        if (!params.parseText(s, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    }
    Composer composer(seed);
    const auto t0 = std::chrono::steady_clock::now();
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
