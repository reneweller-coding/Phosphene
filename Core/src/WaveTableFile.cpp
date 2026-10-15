/**
 * @file WaveTableFile.cpp
 * @brief Reading `.phoswt`, reading a wavetable `.wav`, and the table lookup in front of the six
 *        built-in tables.
 *
 * The contract of the file format is in WaveTableFile.h and is normative; where this file and that
 * text disagree, the text is the bug report.
 *
 * @note The WAV side is adapted from Noctuary `Core/src/WavFile.cpp` and `Core/src/CycleTable.cpp`
 *       at b60a2fe (15.09.2026): the chunk walk, the `clm `/`srge` cycle length and the per-cycle
 *       analysis are the same in substance. What is left behind: FLAC, Surge's `.wt` container, the
 *       `-WT512` file-name convention and the WaveEdit 64x256 ambiguity of `detectCycleLength()`,
 *       none of which any file of this library needs.
 */
#include "phos/WaveTableFile.h"
#include "phos/Util.h"
#include "phos/Fft.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>

namespace phos {

namespace {

using CoeffsT = std::vector<std::complex<double>>;   ///< A table frame's harmonics as complex coefficients.

constexpr size_t NameBytes = 32;   ///< a table's name in the pack, bytes
constexpr size_t IdBytes = 64;   ///< a table's id in the pack, bytes
constexpr size_t Align = 32;   ///< the alignment of the pack's records, bytes
constexpr int TopHarmonics = WaveTable::StoreLen / 8;   ///< levelHarmonics(0)

/** @brief Where a bare library file name is looked for, after the working directory. */
std::string& searchPath()
{
    static std::string p;
    return p;
}

/** @brief Frames a library table is built with; 0 means every frame the file holds. */
int& frameLimit()
{
    static int n = 0;
    return n;
}

/** @brief Little-endian reads that do not care how the host aligns. */
uint32_t readU32(const uint8_t* p) { uint32_t v = 0; std::memcpy(&v, p, 4); return v; }
/** @brief A little-endian 16-bit word at @p p. */
uint16_t readU16(const uint8_t* p) { uint16_t v = 0; std::memcpy(&v, p, 2); return v; }
/** @brief A little-endian float at @p p. */
float readF32(const uint8_t* p) { float v = 0.0f; std::memcpy(&v, p, 4); return v; }
/** @brief A little-endian signed 16-bit word at @p p. */
int16_t readI16(const uint8_t* p) { int16_t v = 0; std::memcpy(&v, p, 2); return v; }

/** @brief The octave band a 1-based harmonic belongs to: 1 | 2,3 | 4..7 | 8..15 | ... */
int bandOf(int h)
{
    int b = 0;
    while ((h >> (b + 1)) != 0) ++b;
    return b;
}

/**
 * @brief Which frames of @p total a table keeps when the limit is smaller.
 *
 * Thinned evenly, `src = k * (total - 1) / (keep - 1)`, which is what Noctuary's
 * `CycleTable::build` does with a file of more frames than it stores. The first and the last frame
 * are always kept, so the ends of the position knob stay where they were.
 */
std::vector<int> frameIndices(int total, int limit)
{
    const int keep = (limit > 0 && limit < total) ? limit : std::min(total, WaveTable::MaxFrames);
    std::vector<int> idx(static_cast<size_t>(keep));
    for (int k = 0; k < keep; ++k)
        idx[static_cast<size_t>(k)] = (keep == total)
            ? k : static_cast<int>(static_cast<long long>(k) * (total - 1) / std::max(keep - 1, 1));
    return idx;
}

/**
 * @brief One entry of the library: where its frames sit in the pack, and the table once it is built.
 *
 * 21.09.2026: the pack used to be expanded in full at load. The ten mip levels make that expansion
 * 32 times the packed size -- 61 KB on disk becomes 1.94 MB in memory -- so 35 tables cost 68 MB and
 * a few hundred would cost gigabytes. Now the load only *indexes* the pack (`at`, `frameCount`,
 * `dtype`) and keeps its bytes; a table is expanded when a track that uses it is planned
 * (ensureWaveTables, on the composer thread) and published with a release store. The audio thread
 * reads `ready` with an acquire load and never builds anything itself: a table that is not ready
 * yet falls back to its built-in, exactly as a missing library file already did.
 */
struct LibraryEntry {
    WaveTable m_table;   ///< the table, its mip levels once built
    size_t m_at = 0;            ///< byte offset of this table's first frame record in the pack
    int m_frameCount = 0;       ///< frames the pack holds for it
    int m_dtype = 0;            ///< 0 = float32 coefficients, 1 = int16 with per-band scales
    bool m_indexed = false;     ///< the pack has this table and the three fields above are valid
    std::atomic<bool> m_ready{ false };   ///< the mip levels are built; only ever false -> true
};

/**
 * @brief The loaded library: built once, read from the audio thread only through waveTable().
 *
 * `attempted` is the call-once gate for loadWaveTableLibrary() below: a thread that finds it already
 * true (acquire) knows every write the loading thread made -- entry[], count, bytes -- is visible to
 * it too (release), without taking `mutex`. `mutex` serialises the small number of threads that *do*
 * find it false, so only one of them runs parsePack() while the rest block instead of racing it (the
 * bug this struct replaces: a plain `bool attempted` let a second thread see "already attempted" and
 * return `count` -- still 0 -- while the first thread's parse was still in flight).
 */
struct Library {
    LibraryEntry m_entry[NumLibraryWaveTables > 0 ? NumLibraryWaveTables : 1];   ///< the tables of the pack, by index
    std::vector<uint8_t> m_file;   ///< the pack itself, kept so a table can be expanded later
    std::atomic<bool> m_attempted{ false };   ///< loadWaveTableLibrary() ran (or runs): the call-once gate
    std::mutex m_mutex;   ///< serialises the threads that find it false
    int m_count = 0;               ///< tables the pack offers (indexed), not tables built
    size_t m_bytes = 0;            ///< memory the built tables occupy; written under `mutex`
};

/** @brief The process's one wavetable library. */
Library& library()
{
    static Library lib;
    return lib;
}

/**
 * @brief Walks one table's frame records; decodes the kept frames when @p out is given.
 *
 * One function for both passes, so that indexing and expanding can never disagree about where a
 * record ends: the load walks with @p out null to find the next table, ensureWaveTables() walks the
 * same bytes again with a buffer to fill.
 */
bool walkTableFrames(const std::vector<uint8_t>& file, size_t& at, int frameCount, int dtype,
                     const std::vector<int>& keep, std::vector<CoeffsT>* out, std::string* error)
{
    const size_t n = file.size();
    size_t next = 0;
    for (int k = 0; k < frameCount; ++k) {
        if (at + 4 > n) return fileError(error, "a frame record runs past the end", at);
        const int harmonics = readU16(file.data() + at);
        const int bands = readU16(file.data() + at + 2);
        at += 4;
        if (harmonics < 0 || harmonics > TopHarmonics) return fileError(error, "harmonics out of range", at - 4);
        const size_t scaleBytes = static_cast<size_t>(bands) * 4;
        const size_t dataBytes = static_cast<size_t>(harmonics) * (dtype == 0 ? 8u : 4u);
        if (at + scaleBytes + dataBytes > n) return fileError(error, "frame data runs past the end", at);
        const uint8_t* scales = file.data() + at;
        const uint8_t* data = scales + scaleBytes;
        if (out != nullptr && next < keep.size() && keep[next] == k) {
            CoeffsT c(static_cast<size_t>(harmonics));
            for (int h = 1; h <= harmonics; ++h) {
                const size_t i = static_cast<size_t>(h - 1);
                if (dtype == 0) {
                    c[i] = std::complex<double>(readF32(data + i * 8), readF32(data + i * 8 + 4));
                } else {
                    const int b = bandOf(h);
                    const double s = b < bands ? static_cast<double>(readF32(scales + static_cast<size_t>(b) * 4)) : 0.0;
                    c[i] = std::complex<double>(readI16(data + i * 4) * s, readI16(data + i * 4 + 2) * s);
                }
            }
            (*out)[next++] = std::move(c);
        }
        at += scaleBytes + dataBytes;
    }
    return out == nullptr || next == keep.size();
}

/** @brief The memory ceiling on expanded tables (setWaveTableBudgetBytes); 0 = none. */
std::atomic<size_t>& budgetBytes()
{
    static std::atomic<size_t> b{ 192u * 1024u * 1024u };
    return b;
}

/** @brief Empties an entry. Not an assignment: LibraryEntry holds an atomic and is not copyable. */
void clearEntry(LibraryEntry& e)
{
    e.m_table = WaveTable{};
    e.m_at = 0;
    e.m_frameCount = 0;
    e.m_dtype = 0;
    e.m_indexed = false;
    e.m_ready.store(false, std::memory_order_release);
}

/** @brief Expands one indexed table into its mip levels and publishes it. Caller holds the mutex. */
bool buildSlot(Library& lib, int slot)
{
    LibraryEntry& e = lib.m_entry[slot];
    if (e.m_ready.load(std::memory_order_relaxed)) return true;
    if (!e.m_indexed) return false;
    // The ceiling (WaveTableFile.h, setWaveTableBudgetBytes). Checked before the work, not after:
    // the point is not to allocate the table at all once the library is full, and a voice whose
    // table is refused here sounds its built-in fallback, which is audible but never a fault.
    const size_t budget = budgetBytes().load(std::memory_order_relaxed);
    if (budget != 0 && lib.m_bytes >= budget) return false;
    const std::vector<int> keep = frameIndices(e.m_frameCount, frameLimit());
    std::vector<CoeffsT> frames(keep.size());
    size_t at = e.m_at;
    if (!walkTableFrames(lib.m_file, at, e.m_frameCount, e.m_dtype, keep, &frames, nullptr)) return false;
    if (frames.empty() || !e.m_table.buildFromHarmonics(frames)) return false;
    lib.m_bytes += e.m_table.m_data.size() * sizeof(float);
    // Release: everything buildFromHarmonics() wrote is visible to any thread that sees `ready`.
    e.m_ready.store(true, std::memory_order_release);
    return true;
}


/**
 * @brief Parses the pack into @p out, indexed by the position of an id in LibraryTables.
 *
 * A table the file holds but this build does not ship is skipped, and a table this build ships but
 * the file does not hold stays unloaded -- both so that a newer pack and an older binary, or the
 * other way round, degrade to the built-in fallback instead of misaddressing a table.
 */
bool parsePack(const std::vector<uint8_t>& file, Library& out, std::string* error)
{
    const size_t n = file.size();
    if (n < 24 || std::memcmp(file.data(), "PHOSWT1\0", 8) != 0) return fileError(error, "magic is not PHOSWT1", 0);
    if (readU32(file.data() + 8) != 1u) return fileError(error, "version is not 1", 8);
    const uint32_t tables = readU32(file.data() + 12);
    const uint32_t headerLen = readU32(file.data() + 16);
    size_t at = 20 + headerLen;
    if (at > n) return fileError(error, "headerLen runs past the end of the file", 16);
    at = (at + Align - 1) / Align * Align;

    for (uint32_t t = 0; t < tables; ++t) {
        if (at + NameBytes + IdBytes + 8 > n) return fileError(error, "a table record runs past the end", at);
        const char* id = reinterpret_cast<const char*>(file.data() + at + NameBytes);
        at += NameBytes + IdBytes;
        const int frameCount = readU16(file.data() + at);
        const int dtype = file[at + 4];
        at += 8;
        // Which of the shipped tables this is, by its library id -- never by its position.
        int slot = -1;
        for (int i = 0; i < NumLibraryWaveTables; ++i)
            if (std::strncmp(id, LibraryTables[i].m_id, IdBytes) == 0) slot = i;

        // Indexing only: the frames are walked to find the end of the record, not decoded. What the
        // entry keeps is where they begin, so ensureWaveTables() can come back for them.
        const size_t framesAt = at;
        if (!walkTableFrames(file, at, frameCount, dtype, {}, nullptr, error)) return false;
        if (slot >= 0) {
            LibraryEntry& e = out.m_entry[slot];
            e.m_at = framesAt;
            e.m_frameCount = frameCount;
            e.m_dtype = dtype;
            e.m_indexed = true;
            ++out.m_count;
        }
    }
    if (at + 8 > n || std::memcmp(file.data() + at, "PHOSWTE1", 8) != 0)
        return fileError(error, "the end marker PHOSWTE1 is missing", at);
    return true;
}

} // namespace

/// The generated list, once as data.
#define PHOS_WT(index, name, id, lane, fallback) { name, id, lane, fallback },
const LibraryTableDesc LibraryTables[NumLibraryWaveTables] = {
#include "phos/WaveTableList.inl"
};
#undef PHOS_WT

void setWaveTableSearchPath(const std::string& directory) { searchPath() = directory; }
const std::string& waveTableSearchPath() { return searchPath(); }

void setWaveTableFrameLimit(int frames)
{
    if (!library().m_attempted.load(std::memory_order_acquire)) frameLimit() = frames;
}

int loadWaveTableLibrary(const char* path, std::string* error)
{
    Library& lib = library();
    // Hot path: once loaded (by this call or another thread), no lock -- an acquire load that pairs
    // with the release store at the bottom of the slow path below.
    if (lib.m_attempted.load(std::memory_order_acquire)) return lib.m_count;
    std::lock_guard<std::mutex> guard(lib.m_mutex);
    // Lost the race for the lock: whoever got there first already finished (attempted is set only
    // after the load is complete), so there is nothing left to do.
    if (lib.m_attempted.load(std::memory_order_relaxed)) return lib.m_count;
    if (error != nullptr) error->clear();
    const char* name = (path != nullptr && path[0] != 0) ? path : "library.phoswt";
    std::vector<uint8_t> file;
    std::string tried = name;
    if (!readFile(name, file)) {
        const bool bare = std::strchr(name, '/') == nullptr && std::strchr(name, '\\') == nullptr;
        bool got = false;
        if (bare && !searchPath().empty()) {
            const std::string p = searchPath() + "/" + name;
            tried += ", " + p;
            got = readFile(p.c_str(), file);
        }
#if defined(PHOS_SOURCE_DATA_DIR)
        if (!got && bare) {
            const std::string p = std::string(PHOS_SOURCE_DATA_DIR) + "/" + name;
            tried += ", " + p;
            got = readFile(p.c_str(), file);
        }
#endif
        if (!got) {
            if (error != nullptr) *error = "cannot open the wavetable library (" + tried + ")";
            // The one attempt is made either way -- a missing file does not retry on the next call --
            // so the gate opens here too, after the release store every other thread's acquire load pairs with.
            lib.m_attempted.store(true, std::memory_order_release);
            return 0;
        }
    }
    lib.m_file = std::move(file);
    std::string why;
    if (!parsePack(lib.m_file, lib, &why)) {
        // A broken file must leave nothing half-built: every table falls back to its built-in.
        for (int i = 0; i < NumLibraryWaveTables; ++i) clearEntry(lib.m_entry[i]);
        lib.m_count = 0;
        lib.m_bytes = 0;
        if (error != nullptr) *error = why;
        lib.m_attempted.store(true, std::memory_order_release);
        return 0;
    }
    lib.m_attempted.store(true, std::memory_order_release);
    return lib.m_count;
}

void resetWaveTableLibrary()
{
    Library& lib = library();
    // Guarded by the same mutex as the load: cheap (a test-only call, never during a load), and it
    // rules out a reset landing between a concurrent loader's release store and a reader still using
    // the entries it is about to clear.
    std::lock_guard<std::mutex> guard(lib.m_mutex);
    for (int i = 0; i < NumLibraryWaveTables; ++i) clearEntry(lib.m_entry[i]);
    lib.m_file.clear();
    lib.m_file.shrink_to_fit();
    lib.m_attempted.store(false, std::memory_order_release);
    lib.m_count = 0;
    lib.m_bytes = 0;
}

bool waveTableIsLibrary(int index)
{
    return index >= NumBuiltinWaveTables && index < NumWaveTables;
}

bool waveTableLoaded(int index)
{
    return waveTableIsLibrary(index) && library().m_entry[index - NumBuiltinWaveTables].m_ready.load(std::memory_order_acquire);
}

void ensureWaveTables(const int* indices, int count)
{
    if (count <= 0 || indices == nullptr) return;
    // The composer plans the first track before Engine::prepare() has loaded the pack, so asking for
    // tables has to be able to open it. This runs on the planning thread, never on the audio thread,
    // where loadWaveTableLibrary()'s own gate makes the call free once it has been done.
    loadWaveTableLibrary();
    Library& lib = library();
    if (!lib.m_attempted.load(std::memory_order_acquire)) return;
    // Cheap pass first: the common case is a track whose tables are all built already, and that must
    // not take the lock -- the composer calls this for every track it plans.
    bool missing = false;
    for (int i = 0; i < count && !missing; ++i) {
        const int idx = indices[i];
        if (idx >= NumBuiltinWaveTables && idx < NumWaveTables)
            missing = !lib.m_entry[idx - NumBuiltinWaveTables].m_ready.load(std::memory_order_acquire);
    }
    if (!missing) return;
    std::lock_guard<std::mutex> guard(lib.m_mutex);
    for (int i = 0; i < count; ++i) {
        const int idx = indices[i];
        if (idx >= NumBuiltinWaveTables && idx < NumWaveTables) buildSlot(lib, idx - NumBuiltinWaveTables);
    }
}

void setWaveTableBudgetBytes(size_t bytes) { budgetBytes().store(bytes, std::memory_order_relaxed); }

const std::vector<int>& waveTableLaneTables(WaveTableLaneE lane)
{
    // Built once on first use; the function-local static makes that thread safe without a lock of
    // our own, and nothing here can change afterwards -- LibraryTables is compiled in.
    static const std::vector<std::vector<int>> byLane = [] {
        std::vector<std::vector<int>> v(static_cast<size_t>(NumWaveTableLanes));
        for (int i = 0; i < NumLibraryWaveTables; ++i) {
            const int l = static_cast<int>(LibraryTables[i].m_lane);
            if (l >= 0 && l < NumWaveTableLanes) v[static_cast<size_t>(l)].push_back(NumBuiltinWaveTables + i);
        }
        return v;
    }();
    const int l = static_cast<int>(lane);
    static const std::vector<int> none;
    return l >= 0 && l < NumWaveTableLanes ? byLane[static_cast<size_t>(l)] : none;
}

int waveTablesIndexed() { return library().m_count; }

int waveTablesBuilt()
{
    const Library& lib = library();
    int n = 0;
    for (int i = 0; i < NumLibraryWaveTables; ++i) n += lib.m_entry[i].m_ready.load(std::memory_order_acquire) ? 1 : 0;
    return n;
}

size_t waveTableLibraryBytes() { return library().m_bytes; }

const WaveTable& waveTable(int index)
{
    if (index < NumBuiltinWaveTables) return builtinWaveTable(index);
    const int i = index < NumWaveTables ? index - NumBuiltinWaveTables : NumLibraryWaveTables - 1;
    if (NumLibraryWaveTables <= 0) return builtinWaveTable(0);
    const LibraryEntry& e = library().m_entry[i];
    // Acquire pairs with buildSlot()'s release store. Never builds here: this runs on the audio
    // thread, where an allocation or a file read would be a fault, so a table that has not been
    // asked for yet sounds as its built-in fallback rather than stalling the mix.
    if (e.m_ready.load(std::memory_order_acquire)) return e.m_table;
    return builtinWaveTable(LibraryTables[i].m_fallback);
}

/**
 * @brief Reads a single-cycle wavetable WAV at @p path into @p out (a frame's harmonics each, at most @p maxFrames) and its
 *        cycle length into @p cycleLen; false if it cannot.
 */
bool readWaveTableWav(const char* path, std::vector<CoeffsT>& out, int& cycleLen, int maxFrames)
{
    out.clear();
    cycleLen = 0;
    std::vector<uint8_t> file;
    if (!readFile(path, file)) return false;
    const size_t n = file.size();
    if (n < 12 || std::memcmp(file.data(), "RIFF", 4) != 0 || std::memcmp(file.data() + 8, "WAVE", 4) != 0)
        return false;
    // The chunk walk of Noctuary's WavFile.cpp: fmt for the sample layout, "clm " for the cycle
    // length Serum states and Vital copies, "srge" for Surge's, data for the samples themselves.
    size_t at = 12;
    int format = 0, channels = 1, bits = 0, stated = 0;
    const uint8_t* data = nullptr;
    size_t dataBytes = 0;
    while (at + 8 <= n) {
        const char* tag = reinterpret_cast<const char*>(file.data() + at);
        const uint32_t size = readU32(file.data() + at + 4);
        at += 8;
        if (at + size > n) break;
        if (std::memcmp(tag, "fmt ", 4) == 0 && size >= 16) {
            format = readU16(file.data() + at);
            channels = readU16(file.data() + at + 2);
            bits = readU16(file.data() + at + 14);
        } else if (std::memcmp(tag, "clm ", 4) == 0 && size >= 4) {
            char text[64] = {};
            std::memcpy(text, file.data() + at, std::min<size_t>(size, sizeof(text) - 1));
            if (std::strncmp(text, "<!>", 3) == 0) stated = std::atoi(text + 3);
        } else if (std::memcmp(tag, "srge", 4) == 0 && size >= 8) {
            stated = static_cast<int>(readU32(file.data() + at + 4));
        } else if (std::memcmp(tag, "data", 4) == 0) {
            data = file.data() + at;
            dataBytes = size;
        }
        at += size + (size & 1u);
    }
    if (data == nullptr || channels < 1) return false;
    const int bytesPerSample = bits / 8;
    if (bytesPerSample <= 0) return false;
    const size_t total = dataBytes / static_cast<size_t>(bytesPerSample);
    std::vector<float> mono(total / static_cast<size_t>(channels), 0.0f);
    for (size_t i = 0; i < mono.size(); ++i) {
        double sum = 0.0;
        for (int c = 0; c < channels; ++c) {
            const uint8_t* p = data + (i * static_cast<size_t>(channels) + static_cast<size_t>(c))
                                        * static_cast<size_t>(bytesPerSample);
            if (format == 3 && bits == 32) sum += readF32(p);
            else if (format == 1 && bits == 16) sum += readI16(p) / 32768.0;
            else if (format == 1 && bits == 24) {
                const int32_t v = static_cast<int32_t>(p[0] | (p[1] << 8) | (p[2] << 16))
                                - ((p[2] & 0x80u) ? (1 << 24) : 0);
                sum += v / 8388608.0;
            } else if (format == 1 && bits == 32) sum += static_cast<int32_t>(readU32(p)) / 2147483648.0;
            else return false;
        }
        mono[i] = static_cast<float>(sum / channels);
    }
    // 2048 is the layout of Serum, Vital and of every file of this library; a file that states
    // something else is believed.
    cycleLen = (stated >= 8 && stated <= 65536) ? stated : 2048;
    const int frameCount = static_cast<int>(mono.size()) / cycleLen;
    if (frameCount < 1) return false;
    const std::vector<int> keep = frameIndices(frameCount, maxFrames);
    const int top = std::min(TopHarmonics, (cycleLen - 1) / 2);
    const bool pow2 = (cycleLen & (cycleLen - 1)) == 0;
    std::unique_ptr<Fft> fft;
    std::vector<std::complex<double>> buf;
    if (pow2) {
        fft = std::make_unique<Fft>(cycleLen);
        buf.assign(static_cast<size_t>(cycleLen), std::complex<double>(0.0, 0.0));
    }
    out.assign(keep.size(), CoeffsT());
    for (size_t k = 0; k < keep.size(); ++k) {
        const float* x = mono.data() + static_cast<size_t>(keep[k]) * static_cast<size_t>(cycleLen);
        CoeffsT c(static_cast<size_t>(std::max(top, 0)));
        const double scale = 2.0 / static_cast<double>(cycleLen);
        if (fft != nullptr) {
            for (int i = 0; i < cycleLen; ++i) buf[static_cast<size_t>(i)] = std::complex<double>(x[i], 0.0);
            fft->transform(buf, false);
            for (int h = 1; h <= top; ++h) c[static_cast<size_t>(h - 1)] = buf[static_cast<size_t>(h)] * scale;
        } else {
            constexpr double PiD = 3.14159265358979323846;
            for (int h = 1; h <= top; ++h) {
                const double w = 2.0 * PiD * h / static_cast<double>(cycleLen);
                double sr = 0.0, si = 0.0;
                for (int i = 0; i < cycleLen; ++i) { sr += x[i] * std::cos(w * i); si -= x[i] * std::sin(w * i); }
                c[static_cast<size_t>(h - 1)] = std::complex<double>(sr, si) * scale;
            }
        }
        out[k] = std::move(c);
    }
    return true;
}

} // namespace phos
