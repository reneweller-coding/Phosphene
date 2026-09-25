/**
 * @file WaveTableFile.h
 * @brief The shipped wavetable library: the `.phoswt` file, its reader, and the table lookup that
 *        sits in front of the six built-in tables.
 *
 * ## Why a file at all
 *
 * Until 16.09.2026 Phosphene's six tables were written as spectra in code (WaveTable.cpp). That was
 * never a judgement about tables, only about *samples*: the plan's "no samples" rule is about sample
 * playback. The wavetable library of the sibling project Noctuary is the user's own work, the code
 * that reads it is already Phosphene's (WaveTable.h is adapted from Noctuary
 * `Core/include/ambient/CycleTable.h` at b60a2fe), and PLAN 5.7 says in as many words that the
 * generated tables of Noctuary's `Tools/WavetableLib` "kommen mit". This is that.
 *
 * ## Why a pack and not the `.wav` files
 *
 * The library's files are not one layout but two: `Classic/` is 16-bit PCM with the `clm ` chunk
 * Serum writes and Vital copies, `Harmonic/` and `Ambient/` are 32-bit float with no `clm` at all --
 * 512 KB for a table of 64 frames. Shipping them as they lie would mean carrying that, opening one
 * file per table, and running a 2048-point analysis per frame at load before the mip levels can even
 * begin. A `.phoswt` carries instead exactly what `WaveTable::buildFromHarmonics()` consumes -- the
 * Fourier coefficients of each frame -- with the harmonics that are below the frame's own noise
 * floor left out, which is most of them for a table built from 32 or 128 partials. One file, one
 * open, no analysis, and the numbers in docs/rounds/2026-09.md (block of 16.09.2026) for what that saves.
 *
 * ## Byte layout
 *
 * Little endian throughout; `float` is IEEE-754 binary32. The shape follows `docs/MODEL_FORMAT.md`
 * (magic, version, a text header of `key=value` lines, records, an end marker), so a reader written
 * for one is not surprised by the other.
 *
 * ```
 * char   magic[8]    = "PHOSWT1\0"
 * uint32 version     = 1
 * uint32 tableCount
 * uint32 headerLen                    // bytes of the text header
 * char   header[headerLen]            // "key=value\n", ASCII, provenance and build info
 * uint8  pad[...]                     // zeros until the offset from the file start is 32-aligned
 * then tableCount table records:
 *   char   name[32]                   // display name, zero padded ("Vowel Sweep 004")
 *   char   id[64]                     // the library id it came from, zero padded
 *   uint16 frames
 *   uint16 harmonicsMax               // the largest per-frame harmonic count in this table
 *   uint8  dtype                      // 0 = float32 coefficients, 1 = int16 with one scale a frame
 *   uint8  reserved[3] = 0
 *   then `frames` frame records:
 *     uint16 harmonics                // stored harmonics of this frame, 0 .. 512
 *     uint16 bands                    // dtype 1: octave bands, each with a scale. dtype 0: 0
 *     float  scale[bands]             // dtype 1 only
 *     data                            // harmonics pairs: float32 re, im -- or int16 re, im
 * char   magic2[8]   = "PHOSWTE1"
 * ```
 *
 * Element `h-1` of a frame is harmonic `h` as the complex amplitude of a cosine,
 * `|c| cos(2 pi h t + arg c)` -- the same convention `buildFromHarmonics()` documents. Harmonics
 * past a frame's `harmonics` count are zero, which is how a table of 32 partials costs 128 bytes a
 * frame instead of 4096.
 *
 * **int16 quantisation** (`dtype == 1`), one scale per octave band of harmonics -- band `b` holds
 * harmonics `2^b .. 2^(b+1) - 1`, so `bands` is `floor(log2(harmonics)) + 1`:
 * ```
 * scale[b] = max over band b of max(|Re c_h|, |Im c_h|) / 32767     (an empty band gets 0)
 * q        = clamp(round(c_h / scale[band(h)]), -32767, 32767)
 * c_h     ~= q * scale[band(h)]
 * ```
 * One scale for the whole frame was tried first and is 10 dB worse: the quantisation error of every
 * harmonic is then the same absolute size, and 512 of them sum to a floor far above any single
 * one's. Measured over the shipped selection, one scale a frame gives a round trip of 82.7 dB and
 * octave bands 92.2 dB -- within three dB of the 16-bit PCM the `Classic/` files are stored in to
 * begin with. The packer's `--check` prints both, and every alternative, before anything is written
 * (docs/rounds/2026-09.md, block of 16.09.2026).
 *
 * ## What the reader guarantees
 *
 * A table that comes out of a `.phoswt` and a table written as spectra in code are the same object:
 * both end in `WaveTable::buildFromHarmonics()`, so the ten mip levels, the guards, the target RMS
 * and the Catmull-Rom read are the same code. Nothing downstream can tell them apart, which is why
 * `waveTable()` can hand either one to Poly.
 *
 * Loading happens once, in `Poly::prepare()`, never on the audio thread: the mip build is ten
 * inverse FFTs per frame.
 *
 * @note The WAV side (`readWaveTableWav`) is adapted from Noctuary `Core/src/WavFile.cpp` and
 *       `Core/src/CycleTable.cpp` at b60a2fe (15.09.2026): the `clm `/`srge` chunk reading and the
 *       per-cycle analysis are unchanged in substance; the Surge `.wt` container, FLAC, the
 *       file-name convention and the WaveEdit ambiguity of `detectCycleLength` are left behind
 *       because no file of this library needs them. It exists so the self test can prove the pack
 *       against the source files it was made from, and so a user can drop a table in.
 */
#pragma once
#include "phos/WaveTable.h"
#include <string>
#include <vector>

namespace phos {

/** @brief How many library tables this build ships (generated by `Tools/wt_pack.py`). */
#include "phos/WaveTableList.inl"
constexpr int kNumLibraryWaveTables = PHOS_WT_LIBRARY_COUNT;
/** @brief Values the `table` parameter takes: the six built-in tables, then the library. */
constexpr int kNumWaveTables = kNumBuiltinWaveTables + kNumLibraryWaveTables;

/**
 * @brief The lane a library table was chosen for (`Tools/wt_select.py`), for displays.
 * @note `Drone` appended 20.09.2026 (round "wavetable-selection"): the tonic drone used to draw pad
 *       lane tables by index; it now has a lane measured for its own character (organ-like, a clear
 *       fundamental, dark). `Counter` appended 22.09.2026: the counter-lead had no lane and drew
 *       from three built-ins, which is why it was the one voice that sounded the same in every
 *       track. Never stored (no Params/.phosset concern), but no longer display-only: the recipe
 *       draw asks for a voice's candidates *by lane* now (ComposerInternal.h, kVoicePalette), so the
 *       palettes grow with the pack instead of naming table indices by hand.
 */
enum class WaveTableLane : int { Pad = 0, Lead, Arp, Drone, Counter, Count };
/** @brief How many lanes the selection knows (for per-lane indexes). */
constexpr int kNumWaveTableLanes = static_cast<int>(WaveTableLane::Count);

/** @brief Static description of one shipped library table. */
struct LibraryTableDesc {
    const char* name;      ///< display name, as it stands in the parameter's choice list
    const char* id;        ///< the library file it came from ("Ambient/ambient_bowed_000")
    WaveTableLane lane;    ///< which lane the selection chose it for
    int fallback;          ///< built-in table used when the file is missing (0 .. kNumBuiltinWaveTables-1)
};

/** @brief The shipped library tables, in the order the parameter's indices 6 .. follow. */
extern const LibraryTableDesc kLibraryTables[kNumLibraryWaveTables];

/**
 * @brief Where a bare library file name is looked for, after the working directory.
 *
 * The same idea as `setModelSearchPath()` (Model.h): a host tells the core where it put its
 * resources. A build that never calls it still finds the file beside the sources through
 * `PHOS_SOURCE_DATA_DIR`, and a build that finds nothing at all falls back to the built-in tables.
 */
void setWaveTableSearchPath(const std::string& directory);
/**
 * @brief The directory setWaveTableSearchPath() was given (empty when none).
 *
 * Added 19.09.2026 so the voice pack (Vocal.h), which ships beside the wavetable pack, is found
 * wherever a host told the core its resources are, without every host learning a second call.
 */
const std::string& waveTableSearchPath();

/**
 * @brief Caps the frames a library table is built with; the Quest lever.
 *
 * A table of 64 frames costs 2.1 MB once its ten mip levels are expanded, so the library is the
 * largest block of memory in the engine. Halving the frames halves that and coarsens the morph the
 * position knob walks through -- the same kind of trade as the unison limit of Quality.h, and like
 * that one it changes the sound, so it is off by default. Frames are thinned evenly, exactly as
 * Noctuary's `CycleTable::build` thins a file with more than its maximum.
 *
 * Must be called before `loadWaveTableLibrary()`; afterwards it does nothing.
 * @param frames 1 .. WaveTable::kMaxFrames; 0 or more than kMaxFrames means "every frame".
 */
void setWaveTableFrameLimit(int frames);

/**
 * @brief Loads the library once. Idempotent, and genuinely safe to call from several threads at once
 *        (20.09.2026 round "threadsafe-loaders": a call-once gate -- an atomic flag checked without a
 *        lock once it is set, a mutex around the one real load for whichever calls arrive first --
 *        replaces a bare flag that let a second thread see "already attempted" while the first thread's
 *        parse was still in flight and come away thinking the library was empty).
 * @param path  the `.phoswt` file; nullptr means the shipped name "library.phoswt"
 * @param error receives why nothing was loaded, may be null (only the thread that ends up doing the
 *               load writes into it; a thread that finds the load already done does not touch it)
 * @return how many tables are loaded (0 when the file is missing -- not an error for the caller,
 *         the built-in tables are the fallback)
 * @warning Never on the audio thread: it builds ten mip levels per frame per table.
 */
int loadWaveTableLibrary(const char* path = nullptr, std::string* error = nullptr);

/**
 * @brief Forgets the loaded library so another file, or another frame limit, can be loaded.
 *
 * For the self test, which has to see what a missing file, a broken file and a frame limit do, and
 * cannot fork a process to do it. **Never while an engine is prepared:** a `Poly` holds a raw
 * pointer into the library's tables between `update()` calls, and this empties them.
 */
void resetWaveTableLibrary();

/**
 * @brief A table by parameter index: 0 .. kNumBuiltinWaveTables-1 built in, then the library.
 *
 * A library table whose file is missing falls back to the built-in its descriptor names, so the
 * parameter always addresses *something* and a set saved on a machine with the library still plays
 * on one without it.
 */
const WaveTable& waveTable(int index);

/** @brief True when @p index addresses the library rather than a built-in table. */
bool waveTableIsLibrary(int index);

/** @brief True when @p index addresses a library table whose data is actually loaded. */
bool waveTableLoaded(int index);

/**
 * @brief Expands the given tables so that waveTable() returns them, and publishes them.
 *
 * Call it from the thread that plans a track, never from the audio thread: it decodes and allocates.
 * Tables already built are skipped without taking a lock. A table that is never asked for costs
 * nothing but its packed bytes -- the pack itself is 32 times smaller than the expansion.
 */
void ensureWaveTables(const int* indices, int count);

/** @brief How many library tables are expanded in memory right now. */
int waveTablesBuilt();

/**
 * @brief How many library tables the pack offers: found in the file and indexed.
 *
 * Since the library is only indexed at load (22.09.2026) this is the number that answers
 * "is the pack there and complete"; waveTablesBuilt() answers "how much of it is in memory",
 * which is a property of what has been played, not of the installation.
 */
int waveTablesIndexed();

/** @brief Bytes the expanded tables occupy (the packed file itself is not counted). */
size_t waveTableLibraryBytes();

/**
 * @brief Ceiling on the memory the expanded tables may take; the on-demand library's safety belt.
 *
 * On-demand expansion (21.09.2026) turned the library's cost from a constant into a growing one: a
 * track asks for six tables, and a set of twenty tracks can therefore ask for a hundred and twenty
 * of the 464 the pack offers -- at 1.94 MB of mip levels each, far past what the old "expand all 35
 * at load" ever took. Nothing is ever freed, because a prepared `Poly` holds a raw pointer into a
 * table between `update()` calls and the audio thread must never find that pointer dangling. So the
 * bound is at the other end: once the built tables reach the budget, a further one is not built and
 * the voice that asked sounds its built-in fallback instead -- the same graceful path a machine
 * without the pack at all takes.
 *
 * The default (192 MB) is about a hundred tables, which is a set of sixteen or so tracks with every
 * voice drawing a different table every time. The Quest sets its own, smaller one, and pairs it with
 * setWaveTableFrameLimit(), which makes each table cheaper rather than rarer.
 * @param bytes the ceiling; 0 means no ceiling
 */
void setWaveTableBudgetBytes(size_t bytes);

/**
 * @brief The parameter indices (6 ..) of every library table the selection chose for @p lane.
 *
 * The voice palettes (ComposerInternal.h, kVoicePalette) name lanes, not tables, so that widening the
 * pack widens what a voice can sound like without a line of C++ changing. Built once, from
 * kLibraryTables, and safe to call from several threads.
 */
const std::vector<int>& waveTableLaneTables(WaveTableLane lane);

/**
 * @brief Reads a wavetable `.wav` into one frame's-worth-per-row coefficients (tests, user files).
 * @param path      a 16-bit PCM or 32-bit float WAV, mono or folded to mono
 * @param out       receives one coefficient vector per frame, ready for buildFromHarmonics()
 * @param cycleLen  receives the frame length that was used
 * @param maxFrames frames to keep; more are thinned evenly
 * @return false when the file cannot be read or holds no whole frame
 */
bool readWaveTableWav(const char* path, std::vector<std::vector<std::complex<double>>>& out,
                      int& cycleLen, int maxFrames = WaveTable::kMaxFrames);

} // namespace phos
