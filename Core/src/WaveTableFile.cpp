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
#include "phos/Fft.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>

namespace phos {

namespace {

using Coeffs = std::vector<std::complex<double>>;

constexpr size_t kNameBytes = 32;
constexpr size_t kIdBytes = 64;
constexpr size_t kAlign = 32;
constexpr int kTopHarmonics = WaveTable::kStoreLen / 8;   ///< levelHarmonics(0)

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

/** @brief Reads the whole file; false when it cannot be opened or read. */
bool readFile(const char* path, std::vector<uint8_t>& out)
{
    std::FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    std::fseek(f, 0, SEEK_END);
    const long n = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    if (n <= 0) { std::fclose(f); return false; }
    out.resize(static_cast<size_t>(n));
    const size_t got = std::fread(out.data(), 1, out.size(), f);
    std::fclose(f);
    return got == out.size();
}

/** @brief Little-endian reads that do not care how the host aligns. */
uint32_t readU32(const uint8_t* p) { uint32_t v = 0; std::memcpy(&v, p, 4); return v; }
uint16_t readU16(const uint8_t* p) { uint16_t v = 0; std::memcpy(&v, p, 2); return v; }
float readF32(const uint8_t* p) { float v = 0.0f; std::memcpy(&v, p, 4); return v; }
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
    const int keep = (limit > 0 && limit < total) ? limit : std::min(total, WaveTable::kMaxFrames);
    std::vector<int> idx(static_cast<size_t>(keep));
    for (int k = 0; k < keep; ++k)
        idx[static_cast<size_t>(k)] = (keep == total)
            ? k : static_cast<int>(static_cast<long long>(k) * (total - 1) / std::max(keep - 1, 1));
    return idx;
}

/** @brief One entry of the loaded library. */
struct LibraryEntry {
    WaveTable table;
    bool loaded = false;
};

/** @brief The loaded library: built once, read from the audio thread only through waveTable(). */
struct Library {
    LibraryEntry entry[kNumLibraryWaveTables > 0 ? kNumLibraryWaveTables : 1];
    bool attempted = false;
    int count = 0;
    size_t bytes = 0;
};

Library& library()
{
    static Library lib;
    return lib;
}

/** @brief Records a message that names the field and where in the file it sits. */
bool fail(std::string* error, const char* what, size_t at)
{
    if (error != nullptr) {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s (at byte %llu)", what, static_cast<unsigned long long>(at));
        *error = buf;
    }
    return false;
}

/**
 * @brief Parses the pack into @p out, indexed by the position of an id in kLibraryTables.
 *
 * A table the file holds but this build does not ship is skipped, and a table this build ships but
 * the file does not hold stays unloaded -- both so that a newer pack and an older binary, or the
 * other way round, degrade to the built-in fallback instead of misaddressing a table.
 */
bool parsePack(const std::vector<uint8_t>& file, Library& out, std::string* error)
{
    const size_t n = file.size();
    if (n < 24 || std::memcmp(file.data(), "PHOSWT1\0", 8) != 0) return fail(error, "magic is not PHOSWT1", 0);
    if (readU32(file.data() + 8) != 1u) return fail(error, "version is not 1", 8);
    const uint32_t tables = readU32(file.data() + 12);
    const uint32_t headerLen = readU32(file.data() + 16);
    size_t at = 20 + headerLen;
    if (at > n) return fail(error, "headerLen runs past the end of the file", 16);
    at = (at + kAlign - 1) / kAlign * kAlign;

    std::vector<Coeffs> frames;
    for (uint32_t t = 0; t < tables; ++t) {
        if (at + kNameBytes + kIdBytes + 8 > n) return fail(error, "a table record runs past the end", at);
        const char* id = reinterpret_cast<const char*>(file.data() + at + kNameBytes);
        at += kNameBytes + kIdBytes;
        const int frameCount = readU16(file.data() + at);
        const int dtype = file[at + 4];
        at += 8;
        // Which of the shipped tables this is, by its library id -- never by its position.
        int slot = -1;
        for (int i = 0; i < kNumLibraryWaveTables; ++i)
            if (std::strncmp(id, kLibraryTables[i].id, kIdBytes) == 0) slot = i;

        const std::vector<int> keep = frameIndices(frameCount, frameLimit());
        frames.assign(keep.size(), Coeffs());
        size_t next = 0;
        for (int k = 0; k < frameCount; ++k) {
            if (at + 4 > n) return fail(error, "a frame record runs past the end", at);
            const int harmonics = readU16(file.data() + at);
            const int bands = readU16(file.data() + at + 2);
            at += 4;
            if (harmonics < 0 || harmonics > kTopHarmonics) return fail(error, "harmonics out of range", at - 4);
            const size_t scaleBytes = static_cast<size_t>(bands) * 4;
            const size_t dataBytes = static_cast<size_t>(harmonics) * (dtype == 0 ? 8u : 4u);
            if (at + scaleBytes + dataBytes > n) return fail(error, "frame data runs past the end", at);
            const uint8_t* scales = file.data() + at;
            const uint8_t* data = scales + scaleBytes;
            // Only the frames this table keeps are decoded; the rest are walked past.
            const bool wanted = slot >= 0 && next < keep.size() && keep[next] == k;
            if (wanted) {
                Coeffs c(static_cast<size_t>(harmonics));
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
                frames[next++] = std::move(c);
            }
            at += scaleBytes + dataBytes;
        }
        if (slot >= 0 && next == keep.size() && !frames.empty()) {
            LibraryEntry& e = out.entry[slot];
            e.loaded = e.table.buildFromHarmonics(frames);
            if (e.loaded) {
                ++out.count;
                out.bytes += e.table.data.size() * sizeof(float);
            }
        }
    }
    if (at + 8 > n || std::memcmp(file.data() + at, "PHOSWTE1", 8) != 0)
        return fail(error, "the end marker PHOSWTE1 is missing", at);
    return true;
}

} // namespace

// The generated list, once as data.
#define PHOS_WT(index, name, id, lane, fallback) { name, id, lane, fallback },
const LibraryTableDesc kLibraryTables[kNumLibraryWaveTables] = {
#include "phos/WaveTableList.inl"
};
#undef PHOS_WT

void setWaveTableSearchPath(const std::string& directory) { searchPath() = directory; }

void setWaveTableFrameLimit(int frames)
{
    if (!library().attempted) frameLimit() = frames;
}

int loadWaveTableLibrary(const char* path, std::string* error)
{
    Library& lib = library();
    if (lib.attempted) return lib.count;
    lib.attempted = true;
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
            return 0;
        }
    }
    std::string why;
    if (!parsePack(file, lib, &why)) {
        // A broken file must leave nothing half-built: every table falls back to its built-in.
        for (int i = 0; i < kNumLibraryWaveTables; ++i) { lib.entry[i] = LibraryEntry{}; }
        lib.count = 0;
        lib.bytes = 0;
        if (error != nullptr) *error = why;
        return 0;
    }
    return lib.count;
}

void resetWaveTableLibrary()
{
    Library& lib = library();
    for (int i = 0; i < kNumLibraryWaveTables; ++i) lib.entry[i] = LibraryEntry{};
    lib.attempted = false;
    lib.count = 0;
    lib.bytes = 0;
}

bool waveTableIsLibrary(int index)
{
    return index >= kNumBuiltinWaveTables && index < kNumWaveTables;
}

bool waveTableLoaded(int index)
{
    return waveTableIsLibrary(index) && library().entry[index - kNumBuiltinWaveTables].loaded;
}

size_t waveTableLibraryBytes() { return library().bytes; }

const WaveTable& waveTable(int index)
{
    if (index < kNumBuiltinWaveTables) return builtinWaveTable(index);
    const int i = index < kNumWaveTables ? index - kNumBuiltinWaveTables : kNumLibraryWaveTables - 1;
    if (kNumLibraryWaveTables <= 0) return builtinWaveTable(0);
    const LibraryEntry& e = library().entry[i];
    if (e.loaded) return e.table;
    return builtinWaveTable(kLibraryTables[i].fallback);
}

bool readWaveTableWav(const char* path, std::vector<Coeffs>& out, int& cycleLen, int maxFrames)
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
    const int top = std::min(kTopHarmonics, (cycleLen - 1) / 2);
    const bool pow2 = (cycleLen & (cycleLen - 1)) == 0;
    std::unique_ptr<Fft> fft;
    std::vector<std::complex<double>> buf;
    if (pow2) {
        fft = std::make_unique<Fft>(cycleLen);
        buf.assign(static_cast<size_t>(cycleLen), std::complex<double>(0.0, 0.0));
    }
    out.assign(keep.size(), Coeffs());
    for (size_t k = 0; k < keep.size(); ++k) {
        const float* x = mono.data() + static_cast<size_t>(keep[k]) * static_cast<size_t>(cycleLen);
        Coeffs c(static_cast<size_t>(std::max(top, 0)));
        const double scale = 2.0 / static_cast<double>(cycleLen);
        if (fft != nullptr) {
            for (int i = 0; i < cycleLen; ++i) buf[static_cast<size_t>(i)] = std::complex<double>(x[i], 0.0);
            fft->transform(buf, false);
            for (int h = 1; h <= top; ++h) c[static_cast<size_t>(h - 1)] = buf[static_cast<size_t>(h)] * scale;
        } else {
            constexpr double kPiD = 3.14159265358979323846;
            for (int h = 1; h <= top; ++h) {
                const double w = 2.0 * kPiD * h / static_cast<double>(cycleLen);
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
