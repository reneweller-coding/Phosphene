/**
 * @file FieldLibrary.h
 * @brief The Field track's recordings (27.09.2026): original FLAC files in a folder, loaded into memory on demand.
 *
 * The user: "Warum willst du da irgendwas umwandeln? Nimm doch die originalen Aufnahmen?" -- so the recordings are the
 * library's own files, 24-bit, 44.1 kHz, stereo, whole, decoded with dr_flac (public domain) when they are needed and
 * kept as float. Nothing reads the disk on the audio thread: the composer loads what a track will play when it plans the
 * track (loadFieldClipNow), the engine asks for a recording it does not have yet (requestFieldClip) and the loader
 * thread fetches it; until then that layer waits and starts when the recording is there.
 *
 * **Where the recordings are.** A folder `field` beside the data files (the working directory, setFieldSearchPath()'s,
 * waveTableSearchPath()'s, then where the Windows setup puts it -- `%PROGRAMDATA%\Phosphene\field` or
 * `%APPDATA%\Phosphene\field` --, then PHOS_SOURCE_DATA_DIR's), the first that exists, and optionally one more folder the user
 * names (setFieldUserFolder(): Noctuary's whole library, say). Files named `fr-<category>-*.flac` belong to that
 * category of kFieldCategorySlugs; files below a subfolder named `nasa` to the category "nasa", except those below
 * `nasa/shots`, which are the effect shots (SfxType::SpaceShot, fieldShotCount); others are ignored.
 * Within a category the files are sorted by name, so a variation number names the same file on every machine that has
 * the same files.
 *
 * **Threads.** A scan publishes an immutable catalogue; a rescan publishes a new one and the old one stays readable, so
 * the audio thread never sees an index vanish under it. A loaded recording stays until the cache is full
 * (kFieldCacheBytes); then the least recently used one that no voice holds (acquireFieldClip / releaseFieldClip) is
 * evicted, and freed only seconds later, so a voice that took it an instant before the eviction still reads valid memory.
 * The loader thread runs while an engine holds it (retainFieldLoader / releaseFieldLoader), so it is started and joined
 * by a plugin's own objects and never by a DLL's static destructors.
 */
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace phos {

/** @brief One recording, decoded. */
struct FieldClip {
    std::string name;              ///< the file's stem
    int category = -1;             ///< index into kFieldCategorySlugs
    int sampleRate = 44100;        ///< the file's own rate
    std::vector<float> left;   ///< the left channel, -1..1
    std::vector<float> right;   ///< the right one; empty for a mono file (right() reads left)
    std::atomic<int> users{ 0 };   ///< voices playing it now (eviction skips it)
    /**
     * @brief The gain that brings it to the library's level: RMS -20 dBFS, the peak held at -1 dBFS.
     *
     * The files stay as they are; the sampler plays them at this gain, so that a quiet night recording and a loud river
     * sit at the same place under the same knob.
     */
    float norm = 1.0f;
    /** @brief Frames. */
    int frames() const { return static_cast<int>(left.size()); }
    /** @brief The left channel's samples. */
    const float* dataL() const { return left.data(); }
    /** @brief The right channel's samples (the left one's for a mono file). */
    const float* dataR() const { return right.empty() ? left.data() : right.data(); }
};

/** @brief How many bytes of decoded recordings the cache holds by default before it evicts unused ones (1.5 GB). */
constexpr size_t kFieldCacheBytes = size_t(1536) * 1024 * 1024;
/** @brief Sets that budget (the Quest app: 512 MB). */
void setFieldCacheBytes(size_t bytes);

/** @brief A folder to look for `field` in before waveTableSearchPath() (a host that unpacks its data elsewhere). */
void setFieldSearchPath(const std::string& directory);
/**
 * @brief Whether the scan looks for the shipped `field` folder at all (default on). Off, the library holds only the
 *        user folder and the recordings made in memory -- a machine where the recordings were not downloaded, which
 *        the tests stand in for this way (the user: "achte darauf, dass das Programm auch dann funktioniert, wenn die
 *        Samples nicht heruntergeladen wurden"). The next scan picks it up.
 */
void setFieldShippedFolder(bool enabled);
/** @brief One more folder of recordings, scanned beside the shipped one (empty: none). The next scan picks it up. */
void setFieldUserFolder(const std::string& directory);
/** @brief The folders the current catalogue was scanned from (the editor, the tests). */
std::vector<std::string> fieldLibraryFolders();
/** @brief Scans the folders if the catalogue is stale. Not on the audio thread. Returns the number of recordings. */
int scanFieldLibrary();
/** @brief Recordings of category @p category (index into kFieldCategorySlugs) in the current catalogue. Lock-free. */
int fieldClipCount(int category);
/** @brief The index of variation @p variation of @p category (wrapped), -1 if the category is empty. Lock-free. */
int fieldClipIndex(int category, int variation);
/** @brief The NASA shots in the current catalogue (the files below `nasa/shots`). Lock-free. */
int fieldShotCount();
/** @brief The library index of shot @p i (wrapped), -1 if there are none. Lock-free. */
int fieldShotIndex(int i);
/** @brief The file behind an index (the editor shows it). */
std::string fieldClipName(int index);
/** @brief The recording behind @p index if it is loaded, held for the caller (its users count raised); else null. Lock-free. */
FieldClip* acquireFieldClip(int index);
/** @brief Gives back a recording acquireFieldClip() returned. */
void releaseFieldClip(FieldClip* clip);
/** @brief Whether the recording behind @p index is in memory. Lock-free. */
bool fieldClipLoaded(int index);
/**
 * @brief Adds a recording made in memory under @p category (a kFieldCategorySlugs index; the tests' material), always
 *        loaded and never evicted; the next scan sorts it in by @p name. Not on the audio thread.
 */
void addFieldClip(int category, const std::string& name, std::vector<float> left, std::vector<float> right, int sampleRate);
/** @brief Asks the loader thread for a recording. Lock-free apart from a wake-up; the audio thread may call it. */
void requestFieldClip(int index);
/** @brief Loads a recording now, on the calling thread (the composer, an offline render). True when it is loaded. */
bool loadFieldClipNow(int index);
/**
 * @brief Whether preloadFieldClip() loads on the calling thread (an offline render, the tests: the same notes give the
 *        same audio, whatever the disk) or only asks the loader thread (a plugin: the composer never waits). Off by default.
 */
void setFieldPreloadBlocking(bool blocking);
/** @brief The composer's preload of a recording a track will play (-1: nothing): loaded now or asked for, see above. */
void preloadFieldClip(int index);
/** @brief Starts the loader thread for one more holder (an engine's constructor). */
void retainFieldLoader();
/** @brief Lets go of the loader thread; the last holder joins it (an engine's destructor). */
void releaseFieldLoader();

} // namespace phos
