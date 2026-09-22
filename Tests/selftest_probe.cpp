/**
 * @file selftest_probe.cpp
 * @brief The self test's sections of round "speed" (20.09.2026): the parallel probe schedule and the probe
 *        cache must not change a single bit of a plan (phos/Probe.h, Composer::measureTrack).
 *
 * A file of its own, linked into phos_selftest and registered in selftest.cpp's main() like every section:
 * the round ran beside another one that owned selftest.cpp's section bodies.
 *
 * Both sections switch the process-wide probe settings and put back what they found. They always turn the
 * cache *off* for their reference plans: under ctest every self-test process has PHOS_PROBE_CACHE set, and a
 * reference that came out of the cache would prove nothing about the cache.
 */
#include "phos/Composer.h"
#include "phos/Params.h"
#include "phos/Probe.h"
#include "phos/Vocal.h"
#include "phos/WaveTableFile.h"
#include "TestSupport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

using namespace phos;
using namespace phostest;

namespace {

/** @brief The seed of the listening excerpts (briefs/listen.py): its first two tracks both lie outside the
 *         presence band, so the presence gain the first mix probe depends on is not zero in either. */
constexpr uint64_t kListeningSeed = 864566672ull;

/**
 * @brief Whether two plans agree, bit for bit, in every field a probe render writes or decides.
 *
 * memcmp and not ==: -0.0 and 0.0, or two NaNs, must not pass for each other. The fields are those of
 * Composer::measureTrack, matchPresence and matchMaster; everything else in a plan is made before the first
 * probe and cannot depend on the schedule.
 */
bool sameNumbers(const TrackPlan& a, const TrackPlan& b, std::string& where)
{
    auto same = [&](const char* name, const void* x, const void* y, size_t n) {
        if (std::memcmp(x, y, n) == 0) return true;
        if (where.empty()) where = name;
        return false;
    };
    bool ok = true;
    ok &= same("loudness", &a.loudness, &b.loudness, sizeof(a.loudness));
    ok &= same("gainDb", &a.gainDb, &b.gainDb, sizeof(a.gainDb));
    ok &= same("partLoudness", a.partLoudness, b.partLoudness, sizeof(a.partLoudness));
    ok &= same("partGainDb", a.partGainDb, b.partGainDb, sizeof(a.partGainDb));
    ok &= same("mixLoudness", &a.mixLoudness, &b.mixLoudness, sizeof(a.mixLoudness));
    ok &= same("masterGainDb", &a.masterGainDb, &b.masterGainDb, sizeof(a.masterGainDb));
    ok &= same("presenceDb", &a.presenceDb, &b.presenceDb, sizeof(a.presenceDb));
    ok &= same("presenceAfterDb", &a.presenceAfterDb, &b.presenceAfterDb, sizeof(a.presenceAfterDb));
    ok &= same("presenceGainDb", &a.presenceGainDb, &b.presenceGainDb, sizeof(a.presenceGainDb));
    return ok;
}

/** @brief The first @p count plans of @p seed with the knobs of @p params, from a fresh composer. */
std::vector<TrackPlan> planTracks(const ParamStore& params, uint64_t seed, int count)
{
    Composer composer(seed);
    std::vector<TrackPlan> out;
    for (int i = 0; i < count; ++i) out.push_back(composer.track(params, i));
    return out;
}

/** @brief Compares two runs plan by plan and records one check. */
void checkSame(const std::vector<TrackPlan>& got, const std::vector<TrackPlan>& want, const char* what)
{
    std::string where;
    bool ok = got.size() == want.size();
    for (size_t i = 0; ok && i < got.size(); ++i) {
        std::string field;
        if (!sameNumbers(got[i], want[i], field)) { ok = false; where = fmt("track %d, %s", static_cast<int>(i) + 1, field.c_str()); }
    }
    check(ok, what, ok ? std::string() : "first difference: " + where);
}

/** @brief Puts the process's probe settings back when a section ends, however it ends. */
struct RestoreProbeSettings {
    int threads = probe::threads();
    std::string dir = probe::cacheDir();
    ~RestoreProbeSettings()
    {
        probe::setThreads(threads);
        probe::setCacheDir(dir);
        probe::setBuildIdForTest(nullptr);
    }
};

/** @brief How many probes one plan of the first track renders: foundation, seven parts, lines, rest, mix twice. */
constexpr uint64_t kProbesFirstTrack = 1 + kMelodyParts + 2 + 2;

} // namespace

/**
 * @brief The parallel schedule against the serial order of before 20.09.2026, bit for bit.
 *
 * The reference is the serial branch of Composer::measureTrack, which is the old code's order of calls. What
 * the comparison can see, each seen to fail first (docs/PLAN.md, the round's mutation table):
 *  - a probe started before a number it reads exists (the first mix probe beside the presence probes while
 *    the presence match is on: the listening seed's tracks both carry a presence gain);
 *  - a shared load racing the workers: the section *starts* with a parallel plan in a process that has not
 *    loaded the wavetable library or the voice pack yet, and repeats it after resetting both. Without
 *    probe::warmSharedData() the first worker sets the library's `attempted` flag and parses for 0.1 s while
 *    the others, told "already attempted", render with the built-in fallback tables.
 */
void testProbeSchedule()
{
    section("testProbeSchedule: parallel probes leave every plan bit-identical");
    RestoreProbeSettings restore;
    probe::setCacheDir("");
    ParamStore params;

    // First of all, before anything in this process has loaded the shared data.
    probe::setThreads(probe::kMaxThreads);
    const auto t0 = std::chrono::steady_clock::now();
    const std::vector<TrackPlan> coldParallel = planTracks(params, kListeningSeed, 1);
    const double parallelSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();

    probe::setThreads(1);
    const auto t1 = std::chrono::steady_clock::now();
    const std::vector<TrackPlan> serial = planTracks(params, kListeningSeed, 2);
    const double serialSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - t1).count();
    // 22.09.2026: **at least one**, not both. A presence gain of exactly zero is the correct answer
    // for a track whose lines already sit inside the band (Composer.cpp, matchPresence returns early
    // there), so demanding it of both reference tracks is a statement about which seed was picked and
    // not about the code -- and the sound changes of this round put the second one inside the band.
    // What this precondition is for is the dependency: the first mix probe reads a number the presence
    // probes write, and one track with a live correction exercises that ordering exactly as two do.
    check(serial[0].presenceGainDb != 0.0f || serial[1].presenceGainDb != 0.0f,
          "at least one reference track carries a presence gain (or the first mix probe's dependency would go untested)",
          fmt("%+.2f and %+.2f dB", static_cast<double>(serial[0].presenceGainDb), static_cast<double>(serial[1].presenceGainDb)));
    check(serial[0].loudness < -5.0 && serial[0].loudness > -40.0 && serial[0].mixLoudness < -5.0 && serial[0].mixLoudness > -40.0,
          "and the probes measured something", fmt("foundation %.2f LUFS, mix %.2f LUFS", serial[0].loudness, serial[0].mixLoudness));
    checkSame(coldParallel, std::vector<TrackPlan>(serial.begin(), serial.begin() + 1),
              "a parallel plan as the process's first use of the shared data equals the serial plan");

    probe::setThreads(probe::kMaxThreads);
    checkSame(planTracks(params, kListeningSeed, 2), serial, "two tracks planned with 8 probe threads equal the serial plans");
    resetWaveTableLibrary();
    resetVoicePack();
    checkSame(planTracks(params, kListeningSeed, 1), std::vector<TrackPlan>(serial.begin(), serial.begin() + 1),
              "and again after the wavetable library and the voice pack were reset");
    probe::setThreads(3);
    checkSame(planTracks(params, kListeningSeed, 1), std::vector<TrackPlan>(serial.begin(), serial.begin() + 1),
              "three threads, fewer than a stage has probes");

    // The one case where the first mix probe may run beside the presence probes: the match is off, so the
    // gain it would wait for is zero by construction.
    std::string err;
    params.parseText("compose.presence_match=Off", &err);
    probe::setThreads(1);
    const std::vector<TrackPlan> serialOff = planTracks(params, 3, 1);
    probe::setThreads(probe::kMaxThreads);
    checkSame(planTracks(params, 3, 1), serialOff, "presence match off (the first mix probe runs early): equal to serial");
    check(serialOff[0].presenceGainDb == 0.0f && serialOff[0].presenceDb != 0.0, "with the match off the presence is still measured and nothing is moved");
    std::printf("         first track: %.1f s with %d probe threads (shared data cold), two tracks serial %.1f s\n", parallelSeconds,
                probe::kMaxThreads, serialSeconds);
}

/**
 * @brief The probe cache: a hit returns exactly what the render returns, and nothing stale is ever served.
 *
 * Expected counts are derived from the plan's structure, not read off the implementation: the first track
 * renders 1 foundation + 7 parts + 2 presence + 2 mix = 12 probes, all with different inputs.
 */
void testProbeCache()
{
    section("testProbeCache: a cache hit is the render's numbers, and a changed input misses");
    RestoreProbeSettings restore;
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / "phos_probe_cache_test";
    std::error_code ec;
    fs::remove_all(dir, ec);
    probe::setThreads(probe::kMaxThreads);
    ParamStore params;

    probe::setCacheDir("");
    const std::vector<TrackPlan> reference = planTracks(params, kListeningSeed, 1);
    check(!fs::exists(dir), "with the cache off nothing is written");

    probe::setCacheDir(dir.string());
    probe::resetCacheStats();
    const std::vector<TrackPlan> cold = planTracks(params, kListeningSeed, 1);
    probe::CacheStats s = probe::cacheStats();
    check(s.hits == 0 && s.misses == kProbesFirstTrack && s.stores == kProbesFirstTrack, "cold: every probe of the first track misses and is stored",
          fmt("%llu hits, %llu misses, %llu stores (12 probes)", static_cast<unsigned long long>(s.hits), static_cast<unsigned long long>(s.misses), static_cast<unsigned long long>(s.stores)));
    checkSame(cold, reference, "cold: the plan equals the uncached plan");
    size_t files = 0, strays = 0;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        if (e.path().extension() == ".probe" && e.file_size() == 56) ++files; else ++strays;
    }
    check(files == kProbesFirstTrack && strays == 0, "twelve entries of 56 bytes, no temporary file left behind",
          fmt("%d entries, %d other files", static_cast<int>(files), static_cast<int>(strays)));

    probe::resetCacheStats();
    const auto w0 = std::chrono::steady_clock::now();
    const std::vector<TrackPlan> warm = planTracks(params, kListeningSeed, 1);
    const double warmSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - w0).count();
    s = probe::cacheStats();
    check(s.hits == kProbesFirstTrack && s.misses == 0 && s.stores == 0, "warm: every probe hits",
          fmt("%llu hits, %llu misses, %llu stores in %.2f s", static_cast<unsigned long long>(s.hits), static_cast<unsigned long long>(s.misses),
              static_cast<unsigned long long>(s.stores), warmSeconds));
    checkSame(warm, reference, "warm: the plan equals the uncached plan, bit for bit");

    // Another core: nothing of this one's may be served, and what it stores must not disturb this one's.
    probe::setBuildIdForTest("another core");
    probe::resetCacheStats();
    const std::vector<TrackPlan> other = planTracks(params, kListeningSeed, 1);
    s = probe::cacheStats();
    check(s.hits == 0 && s.misses == kProbesFirstTrack, "a changed build id misses every entry",
          fmt("%llu hits, %llu misses", static_cast<unsigned long long>(s.hits), static_cast<unsigned long long>(s.misses)));
    checkSame(other, reference, "and renders the same plan");
    probe::setBuildIdForTest("");
    check(!probe::cacheEnabled(), "a core without a build id does not use the cache at all");
    probe::setBuildIdForTest(nullptr);
    check(!probe::buildId().empty(), "this core has a build id", probe::buildId());

    // A changed parameter. The percussion level reaches the foundation, the rest and both mix probes as sound;
    // the part probes mute the percussion, but they are still other probes: the key holds every parameter, because
    // knowing which parameter cannot reach which probe is exactly the knowledge a cache must not need.
    ParamStore louder;
    louder.copyValuesFrom(params);
    const int percLevel = louder.base(Module::Mix) + mix::PercLevel;
    louder.set(percLevel, louder.get(percLevel) - 1.5f);
    probe::setCacheDir("");
    const std::vector<TrackPlan> louderReference = planTracks(louder, kListeningSeed, 1);
    probe::setCacheDir(dir.string());
    probe::resetCacheStats();
    const std::vector<TrackPlan> louderCached = planTracks(louder, kListeningSeed, 1);
    s = probe::cacheStats();
    check(s.hits == 0 && s.misses == kProbesFirstTrack, "a changed parameter (percussion level -1.5 dB) misses every entry",
          fmt("%llu hits, %llu misses", static_cast<unsigned long long>(s.hits), static_cast<unsigned long long>(s.misses)));
    checkSame(louderCached, louderReference, "and the plan is the changed knob's plan");
    std::string differs;
    const bool otherPlan = !sameNumbers(louderReference[0], reference[0], differs);
    check(otherPlan, "which is another plan than the default's", "differs in " + differs);

    // Damaged entries: every file of the directory gets one flipped bit inside its three numbers. The check
    // word must turn each into a miss -- a wrong number must never come out of the cache.
    size_t damaged = 0;
    for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) {
        std::fstream f(e.path(), std::ios::in | std::ios::out | std::ios::binary);
        char c = 0;
        f.seekg(30);
        f.read(&c, 1);
        c = static_cast<char>(c ^ 0x10);
        f.seekp(30);
        f.write(&c, 1);
        ++damaged;
    }
    probe::resetCacheStats();
    const std::vector<TrackPlan> repaired = planTracks(params, kListeningSeed, 1);
    s = probe::cacheStats();
    check(damaged >= 3 * kProbesFirstTrack && s.hits == 0 && s.stores == kProbesFirstTrack, "damaged entries are misses and are written again",
          fmt("%d files damaged; %llu hits, %llu stores", static_cast<int>(damaged), static_cast<unsigned long long>(s.hits), static_cast<unsigned long long>(s.stores)));
    checkSame(repaired, reference, "and the plan is still the uncached plan");
    {
        // A truncated file, as a crashed writer without the rename would leave it.
        probe::Hasher h;
        h.text("testProbeCache truncated");
        const probe::Key key = h.finish();
        const double v[3] = { -14.25, 0.5, 0.25 };
        double r[3] = {};
        probe::cacheStore(key, v);
        const bool whole = probe::cacheLookup(key, r) && std::memcmp(r, v, sizeof(v)) == 0;
        char name[64];
        std::snprintf(name, sizeof(name), "%016llx%016llx.probe", static_cast<unsigned long long>(key.a), static_cast<unsigned long long>(key.b));
        const bool there = fs::exists(dir / name);
        fs::resize_file(dir / name, 20, ec);
        const bool truncatedMisses = !probe::cacheLookup(key, r);
        probe::cacheStore(key, v);
        const bool again = probe::cacheLookup(key, r) && std::memcmp(r, v, sizeof(v)) == 0;
        check(whole && there && truncatedMisses && again, "a truncated entry is a miss, not an error, and the next store replaces it",
              fmt("stored %d, file %d, truncated misses %d, stored again %d", whole, there, truncatedMisses, again));
    }

    // Writers at once: eight threads store and read the same 64 keys, as the processes of a parallel ctest
    // run do with the plans they share. Every read is either a miss or the whole, right entry.
    {
        std::atomic<int> wrong{ 0 }, found{ 0 };
        auto valueOf = [](int k, int i) { return 1000.0 * k + i + 0.125; };
        auto keyOf = [](int k) { probe::Hasher h; h.text("testProbeCache"); h.add(k); return h.finish(); };
        std::vector<std::thread> threads;
        for (int t = 0; t < 8; ++t)
            threads.emplace_back([&] {
                for (int round = 0; round < 4; ++round)
                    for (int k = 0; k < 64; ++k) {
                        const double v[3] = { valueOf(k, 0), valueOf(k, 1), valueOf(k, 2) };
                        probe::cacheStore(keyOf(k), v);
                        double r[3] = {};
                        if (probe::cacheLookup(keyOf(k), r)) {
                            ++found;
                            if (std::memcmp(r, v, sizeof(v)) != 0) ++wrong;
                        }
                    }
            });
        for (std::thread& t : threads) t.join();
        size_t temps = 0;
        for (const fs::directory_entry& e : fs::directory_iterator(dir, ec)) if (e.path().extension() == ".tmp") ++temps;
        check(wrong.load() == 0 && found.load() > 0 && temps == 0, "eight concurrent writers of the same keys: no torn entry, no temporary file left",
              fmt("%d reads found, %d wrong, %d temporary files", found.load(), wrong.load(), static_cast<int>(temps)));
        probe::Hasher a, b;
        a.text("ab"); a.text("c");
        b.text("a"); b.text("bc");
        check(!(a.finish() == b.finish()), "the key separates its fields (\"ab\",\"c\" is not \"a\",\"bc\")");
    }
    fs::remove_all(dir, ec);
}
