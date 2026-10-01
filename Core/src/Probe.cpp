/**
 * @file Probe.cpp
 * @brief The probe scheduler's thread pool and the cross-process probe cache (Probe.h; round "speed").
 */
#include "phos/Probe.h"

#include "phos/Dsp.h"
#include "phos/Vocal.h"
#include "phos/WaveTable.h"
#include "phos/WaveTableFile.h"

#if __has_include("phos_build_id.h")
#include "phos_build_id.h"   // generated at build time: Core/cmake/build_id.cmake
#endif
#if !defined(PHOS_BUILD_ID)
// A build that did not go through Core/CMakeLists.txt has no id. The cache must then never serve anything:
// cacheEnabled() refuses while the id is empty.
/** @brief The build id the cache keys carry (Core/CMakeLists.txt sets it; empty: no cache). */
#define PHOS_BUILD_ID ""
#endif

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

namespace phos::probe {

namespace {

/** @brief Settings of the process. One mutex: they are written at start-up or by a test, read per plan. */
struct Settings {
    std::mutex lock;   ///< guards the settings
    int threads = 1;               ///< the library's default: serial
    std::string cacheDir;          ///< empty = no cache
    std::string buildIdOverride;   ///< setBuildIdForTest()
    bool buildIdOverridden = false;   ///< setBuildIdForTest() was called
};
/** @brief The process's settings. */
Settings& settings() { static Settings s; return s; }

std::atomic<uint64_t> gHits{ 0 };   ///< cache hits
std::atomic<uint64_t> gMisses{ 0 };   ///< cache misses
std::atomic<uint64_t> gStores{ 0 };   ///< entries stored
std::atomic<uint64_t> gTempCounter{ 0 };   ///< counts the cache's temporary files (unique names)

constexpr size_t kWorkerStack = 16u * 1024u * 1024u;   ///< as the JUCE-linked programs' main threads (/STACK)
constexpr char kMagic[8] = { 'P', 'H', 'O', 'S', 'P', 'R', 'B', '1' };   ///< a cache entry's magic
constexpr size_t kEntryBytes = 8 + 16 + 24 + 8;        ///< magic, key, three doubles, check

/** @brief The machine's hardware threads, at least one. */
int hardwareThreads()
{
    const unsigned n = std::thread::hardware_concurrency();
    return n == 0 ? 1 : static_cast<int>(n);
}

/** @brief The file of a key: 32 hex digits. */
std::filesystem::path entryPath(const std::string& dir, const Key& key)
{
    char name[64];
    std::snprintf(name, sizeof(name), "%016llx%016llx.probe", static_cast<unsigned long long>(key.a),
                  static_cast<unsigned long long>(key.b));
    return std::filesystem::path(dir) / name;
}

/** @brief The check word over an entry's first bytes: a torn or foreign file does not pass for an entry. */
uint64_t checkOf(const unsigned char* data, size_t n)
{
    Hasher h;
    h.bytes(data, n);
    return h.finish().b;
}

/** @brief This process's id: the probe cache's temporary files carry it, so two processes never write one name. */
unsigned long long processId()
{
#if defined(_WIN32)
    return static_cast<unsigned long long>(GetCurrentProcessId());
#else
    return static_cast<unsigned long long>(getpid());
#endif
}

/** @brief What a worker thread runs: tasks by a shared counter until none is left. */
struct Job {
    const std::vector<std::function<void()>>* tasks = nullptr;   ///< the tasks
    std::atomic<size_t> next{ 0 };   ///< the next task to take
    /** @brief Takes tasks until none is left. */
    void work()
    {
        for (;;) {
            const size_t i = next.fetch_add(1, std::memory_order_relaxed);
            if (i >= tasks->size()) return;
            (*tasks)[i]();
        }
    }
};

#if defined(_WIN32)
unsigned __stdcall workerEntry(void* arg) { static_cast<Job*>(arg)->work(); return 0; }
#else
/** @brief A worker thread: runs the job @p arg points to. */
void* workerEntry(void* arg) { static_cast<Job*>(arg)->work(); return nullptr; }
#endif

} // namespace

void setThreads(int n)
{
#if defined(__ANDROID__)
    (void)n;   // the Quest's composer has one small core (Quality.h); see the file comment of Probe.h
#else
    std::lock_guard<std::mutex> g(settings().lock);
    settings().threads = std::clamp(n <= 0 ? hardwareThreads() : n, 1, kMaxThreads);
#endif
}

int threads()
{
    std::lock_guard<std::mutex> g(settings().lock);
    return settings().threads;
}

void setCacheDir(const std::string& directory)
{
#if defined(__ANDROID__)
    (void)directory;
#else
    std::lock_guard<std::mutex> g(settings().lock);
    settings().cacheDir = directory;
#endif
}

std::string cacheDir()
{
    std::lock_guard<std::mutex> g(settings().lock);
    return settings().cacheDir;
}

void configureFromEnvironment()
{
    const char* t = std::getenv("PHOS_PROBE_THREADS");
    setThreads(t != nullptr && t[0] != 0 ? std::max(1, std::atoi(t)) : 0);
    const char* c = std::getenv("PHOS_PROBE_CACHE");
    setCacheDir(c != nullptr ? std::string(c) : std::string());
}

CacheStats cacheStats()
{
    CacheStats s;
    s.hits = gHits.load();
    s.misses = gMisses.load();
    s.stores = gStores.load();
    return s;
}

void resetCacheStats() { gHits = 0; gMisses = 0; gStores = 0; }

std::string buildId()
{
    std::lock_guard<std::mutex> g(settings().lock);
    return settings().buildIdOverridden ? settings().buildIdOverride : std::string(PHOS_BUILD_ID);
}

void setBuildIdForTest(const char* id)
{
    std::lock_guard<std::mutex> g(settings().lock);
    settings().buildIdOverridden = id != nullptr;
    settings().buildIdOverride = id != nullptr ? id : "";
}

void Hasher::bytes(const void* data, size_t n)
{
    const unsigned char* p = static_cast<const unsigned char*>(data);
    uint64_t a = a_, b = b_;
    for (size_t i = 0; i < n; ++i) {
        a = (a ^ p[i]) * 0x100000001b3ull;
        b = (b + p[i] + 0x632be59bd9b4e019ull) * 0xff51afd7ed558ccdull;
        b ^= b >> 29;
    }
    a_ = a;
    b_ = b;
    n_ += n;
}

Key Hasher::finish() const
{
    // A last avalanche with the length folded in, so that a prefix is not the key of the whole.
    auto mix = [](uint64_t x) {
        x ^= x >> 33; x *= 0xff51afd7ed558ccdull;
        x ^= x >> 33; x *= 0xc4ceb9fe1a85ec53ull;
        x ^= x >> 33;
        return x;
    };
    Key k;
    k.a = mix(a_ ^ n_);
    k.b = mix(b_ + 0x9e3779b97f4a7c15ull * (n_ + 1));
    return k;
}

void warmSharedData()
{
    for (int i = 0; i < kNumBuiltinWaveTables; ++i) builtinWaveTable(i);
    loadWaveTableLibrary();
    loadVoicePack();
    (void)sineTable();
}

uint64_t sharedDataId()
{
    // Word-wise, not through Hasher's byte loop: the expanded library is 23 MB (Quality.h) and this runs per
    // probe -- measured 7 ms (phos_plandump --data-id), against the 0.3 to 4 s of the render it may save. Per probe and not once per
    // process, because a test may reset and reload the library between two plans.
    uint64_t h = 0x243f6a8885a308d3ull;
    auto word = [&h](uint64_t w) { h = (h ^ w) * 0x9fb21c651e98df25ull; h ^= h >> 32; };
    auto floats = [&](const float* f, size_t n) {
        word(n);
        size_t i = 0;
        for (; i + 2 <= n; i += 2) { uint64_t w; std::memcpy(&w, f + i, 8); word(w); }
        if (i < n) { uint32_t w; std::memcpy(&w, f + i, 4); word(w); }
    };
    for (int i = 0; i < kNumWaveTables; ++i) {
        const WaveTable& t = waveTable(i);
        word(static_cast<uint64_t>(waveTableLoaded(i) ? 1 : 0));
        floats(t.data.data(), t.data.size());
    }
    const int phrases = voicePhraseCount();
    word(static_cast<uint64_t>(phrases));
    for (int i = 0; i < phrases; ++i) {
        const VoicePhrase& p = voicePhrase(i);
        word(static_cast<uint64_t>(p.sampleRate));
        word((static_cast<uint64_t>(p.throwAt) << 32) | p.chopStart);
        word(p.chopLength);
        floats(p.samples.data(), p.samples.size());
    }
    return h;
}

bool cacheEnabled()
{
#if defined(__ANDROID__)
    return false;
#else
    std::lock_guard<std::mutex> g(settings().lock);
    if (settings().cacheDir.empty()) return false;
    // No build id, no cache: without it a changed core would be served the old core's numbers.
    return settings().buildIdOverridden ? !settings().buildIdOverride.empty() : PHOS_BUILD_ID[0] != 0;
#endif
}

bool cacheLookup(const Key& key, double values[3])
{
    if (!cacheEnabled()) return false;
    unsigned char buf[kEntryBytes + 1];
    size_t got = 0;
    {
        std::ifstream f(entryPath(cacheDir(), key), std::ios::binary);
        if (f) {
            f.read(reinterpret_cast<char*>(buf), sizeof(buf));
            got = static_cast<size_t>(f.gcount());
        }
    }
    bool ok = got == kEntryBytes && std::memcmp(buf, kMagic, 8) == 0;
    if (ok) {
        uint64_t a, b, check;
        std::memcpy(&a, buf + 8, 8);
        std::memcpy(&b, buf + 16, 8);
        std::memcpy(&check, buf + 48, 8);
        ok = a == key.a && b == key.b && check == checkOf(buf, 48);
    }
    if (!ok) { ++gMisses; return false; }
    std::memcpy(values, buf + 24, 24);
    ++gHits;
    return true;
}

void cacheStore(const Key& key, const double values[3])
{
    if (!cacheEnabled()) return;
    const std::string dir = cacheDir();
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    unsigned char buf[kEntryBytes];
    std::memcpy(buf, kMagic, 8);
    std::memcpy(buf + 8, &key.a, 8);
    std::memcpy(buf + 16, &key.b, 8);
    std::memcpy(buf + 24, values, 24);
    const uint64_t check = checkOf(buf, 48);
    std::memcpy(buf + 48, &check, 8);
    const std::filesystem::path target = entryPath(dir, key);
    // A name no other writer can have: process id and a counter of this process.
    char suffix[64];
    std::snprintf(suffix, sizeof(suffix), ".%llu.%llu.tmp", processId(), static_cast<unsigned long long>(gTempCounter.fetch_add(1)));
    std::filesystem::path temp = target;
    temp += suffix;
    {
        std::ofstream f(temp, std::ios::binary | std::ios::trunc);
        if (!f) return;
        f.write(reinterpret_cast<const char*>(buf), sizeof(buf));
        f.close();
        if (!f) { std::filesystem::remove(temp, ec); return; }
    }
    // rename() replaces an existing target (MoveFileEx with REPLACE_EXISTING on Windows, rename(2)
    // elsewhere). When it fails -- another process holds the target open for reading at this instant --
    // the entry that is already there says the same thing, and the temporary file goes away.
    std::filesystem::rename(temp, target, ec);
    if (ec) { std::filesystem::remove(temp, ec); return; }
    ++gStores;
}

void runAll(const std::vector<std::function<void()>>& tasks)
{
    if (tasks.empty()) return;
    Job job;
    job.tasks = &tasks;
    const int extra = std::min(threads(), static_cast<int>(tasks.size())) - 1;
#if defined(_WIN32)
    std::vector<HANDLE> workers;
    for (int i = 0; i < extra; ++i) {
        const uintptr_t h = _beginthreadex(nullptr, static_cast<unsigned>(kWorkerStack), workerEntry, &job, 0, nullptr);
        if (h != 0) workers.push_back(reinterpret_cast<HANDLE>(h));   // no thread: the others take its tasks
    }
    job.work();
    for (HANDLE h : workers) { WaitForSingleObject(h, INFINITE); CloseHandle(h); }
#else
    std::vector<pthread_t> workers;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, kWorkerStack);
    for (int i = 0; i < extra; ++i) {
        pthread_t t;
        if (pthread_create(&t, &attr, workerEntry, &job) == 0) workers.push_back(t);
    }
    pthread_attr_destroy(&attr);
    job.work();
    for (pthread_t t : workers) pthread_join(t, nullptr);
#endif
}

} // namespace phos::probe
