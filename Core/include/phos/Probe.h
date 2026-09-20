/**
 * @file Probe.h
 * @brief How the composer's probe renders are *scheduled*: in parallel inside one plan, and not at all when
 *        a development cache already holds the numbers (round "speed", 20.09.2026).
 *
 * **Why.** Planning one track renders up to twelve probes (Composer::probeLoudness): the foundation and seven
 * melodic parts for the level match, the lines and the rest of the drops for the presence match, the whole
 * mix twice for Auto Gain -- about 64 bars of audio, measured at 12 s per track on one core. Every test
 * section and every tool that plans a track paid that, again in every process. Nothing here touches what a
 * probe renders or what is computed from it; it only decides *when* a probe runs and whether its render can
 * be replaced by the identical numbers of an earlier, identical render.
 *
 * **Two switches, both off unless a program asks.** The library's defaults are what it did before this
 * round: one thread, no cache. The plugin and the Quest app never call anything in this file, so they stay
 * exactly there: on the Quest the composer has one small core, and in the plugin the plan is made next to a
 * running audio thread, where eight busy worker threads for several seconds are a risk this round could not
 * show to be harmless (it has no way to measure a host's audio thread under that load). Development
 * programs -- `phos_render`, `phos_plandump`, `phos_selftest`, `phos_hosttest` -- opt in with one call,
 * configureFromEnvironment(), at the top of their `main()`:
 *
 * | Variable | Meaning | Default after configureFromEnvironment() |
 * |---|---|---|
 * | `PHOS_PROBE_THREADS` | probes of one stage that may render at once; 1 = the old serial order | hardware threads, at most 8 |
 * | `PHOS_PROBE_CACHE`   | directory of the cross-process probe cache; unset or empty = no cache | no cache |
 *
 * On Android both calls are ignored (threads stay 1, the cache stays off) whatever a program asks.
 *
 * **Bit identity.** A probe is a pure function of its inputs: every probe owns its Engine, and what the
 * engines share (the wavetable library, the voice pack, the built-in tables, the sine table) is immutable
 * once loaded. The loads themselves are *not* thread-safe (a flag, no lock: WaveTableFile.cpp, Vocal.cpp),
 * so warmSharedData() runs them on the calling thread before the first worker starts. The order in which
 * results are *used* is unchanged: the stages of Composer::measureTrack wait for everything a later probe
 * reads. `testProbeSchedule` holds the parallel schedule against the serial order of before, bit for bit.
 *
 * **The cache key** covers everything a probe's render depends on: the core's build id (a hash of
 * `Core/src`, `Core/include` and the compile flags, generated at build time -- Core/cmake/build_id.cmake),
 * the contents of the loaded wavetable library and voice pack, the probe engine's every parameter value
 * after the probe's setup, the tempo, the sample rate, block size and length, and every control and note
 * event pushed, in order. The value is the three numbers a probe returns. `testProbeCache` checks that a hit
 * returns exactly what the render returns, and that a changed parameter, event or build id misses.
 */
#pragma once
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <type_traits>
#include <vector>

namespace phos::probe {

/** @brief The most probe renders that ever run at once, whatever is asked for. */
constexpr int kMaxThreads = 8;

/**
 * @brief How many probes of one stage may render at once.
 * @param n 1 = serial, in the order of before this round (the library's default); 0 = the hardware's
 *          threads, at most kMaxThreads; anything else is clamped to 1..kMaxThreads. Ignored on Android.
 */
void setThreads(int n);

/** @brief The resolved number of threads (always >= 1). */
int threads();

/**
 * @brief Where the cross-process probe cache lives; empty turns it off (the library's default).
 *
 * The directory is created when the first entry is stored. Ignored on Android.
 */
void setCacheDir(const std::string& directory);

/** @brief The cache directory, empty when the cache is off. */
std::string cacheDir();

/**
 * @brief The opt-in of a development program: parallel probes, and the cache if `PHOS_PROBE_CACHE` names
 *        a directory (the table in the file comment). Never called by the plugin or the Quest app.
 */
void configureFromEnvironment();

/** @brief What the cache did in this process since the last resetCacheStats(). */
struct CacheStats {
    uint64_t hits = 0;     ///< probes answered from a file
    uint64_t misses = 0;   ///< probes rendered because no valid file was there
    uint64_t stores = 0;   ///< files written
};
/** @brief The counters (any thread). */
CacheStats cacheStats();
/** @brief Sets the counters to zero. */
void resetCacheStats();

/** @brief The build id compiled into this core (Core/cmake/build_id.cmake), or the override below. */
std::string buildId();
/**
 * @brief Replaces the build id in the cache key; null restores the compiled one.
 *
 * For `testProbeCache` only: it is how the test shows that a changed core misses instead of being served a
 * stale value, without rebuilding the core in the middle of a test.
 */
void setBuildIdForTest(const char* id);

/** @brief A 128-bit cache key. */
struct Key {
    uint64_t a = 0, b = 0;
    bool operator==(const Key& o) const { return a == o.a && b == o.b; }
};

/**
 * @brief Hashes the inputs of a probe into a Key: two independent 64-bit streams over the same bytes.
 *
 * Not cryptographic -- the cache defends against accidents (a stale file after a code change), not against
 * an attacker, and a development directory is not a trust boundary. 128 bits keep an accidental collision
 * out of reach: a test suite stores some thousands of entries. Values are added field by field, never as
 * whole structs, so padding bytes cannot make equal inputs hash differently.
 */
class Hasher {
public:
    /** @brief Adds raw bytes. */
    void bytes(const void* data, size_t n);
    /** @brief Adds one trivially copyable value (an arithmetic type or an enum). */
    template <typename T> void add(const T& v)
    {
        static_assert(std::is_arithmetic_v<T> || std::is_enum_v<T>, "field by field: no structs, no padding");
        bytes(&v, sizeof(T));
    }
    /** @brief Adds a string with its length, so that "ab","c" and "a","bc" differ. */
    void text(const std::string& s) { add(static_cast<uint64_t>(s.size())); bytes(s.data(), s.size()); }
    /** @brief The key of everything added so far. */
    Key finish() const;

private:
    uint64_t a_ = 0xcbf29ce484222325ull;   ///< FNV-1a, 64 bit
    uint64_t b_ = 0x9e3779b97f4a7c15ull;   ///< a multiply-rotate stream with other constants
    uint64_t n_ = 0;                       ///< bytes added (part of the key)
};

/**
 * @brief Loads, on the calling thread, everything the probe engines share: the built-in wavetables, the
 *        wavetable library, the voice pack and the sine table.
 *
 * Called before every parallel stage. Each load is idempotent and costs a flag test once it has happened;
 * deliberately not a std::call_once, because the tests reset the library (resetWaveTableLibrary()) and the
 * next stage then has to load it again before its workers start.
 */
void warmSharedData();

/**
 * @brief A hash of the *contents* of the shared data as it is loaded right now (every wavetable the engine
 *        can address, every phrase of the voice pack). Part of every cache key: a probe rendered with
 *        another library, or with the built-in fallback because the file was missing, is another probe.
 */
uint64_t sharedDataId();

/** @brief Whether lookups and stores do anything (a directory is set, and this is not Android). */
bool cacheEnabled();

/**
 * @brief Reads the values stored under @p key.
 * @param values receives the three numbers of a probe (loudness, presence-band power, low-band power)
 * @return false when there is no file or it is not a complete, intact entry for this key -- a truncated or
 *         foreign file is a miss, never an error
 */
bool cacheLookup(const Key& key, double values[3]);

/**
 * @brief Stores @p values under @p key: written to a temporary name beside the target and renamed, so that
 *        a concurrent reader sees either no file or a whole one, and two writers of the same key (who by
 *        construction write the same bytes) cannot interleave.
 */
void cacheStore(const Key& key, const double values[3]);

/**
 * @brief Runs every task, at most threads() at a time, and returns when all have finished.
 *
 * The calling thread works too, so `threads() == 1` runs the tasks in order on the caller without creating
 * a thread. Workers get a 16 MB stack like the JUCE-linked programs' main threads (a probe composes bars
 * and builds an Engine on its way). Tasks must not throw.
 */
void runAll(const std::vector<std::function<void()>>& tasks);

} // namespace phos::probe
