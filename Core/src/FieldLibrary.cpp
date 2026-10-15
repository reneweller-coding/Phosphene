/**
 * @file FieldLibrary.cpp
 * @brief The Field track's recordings: the scan, the loads, the loader thread and the cache (FieldLibrary.h).
 * @author Rene Weller
 *
 * @par Compile Flags:
 *   @arg @c PHOS_SOURCE_DATA_DIR - the source tree's data directory (Core/CMakeLists.txt sets it), searched after the
 *                                  search path
 */
#include "phos/FieldLibrary.h"
#include "phos/Params.h"
#include "phos/WaveTableFile.h"

#define DR_FLAC_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4267 4244)   // the library's own size_t narrowings
#elif defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wconversion"
#endif
#include "dr_flac.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#elif defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

namespace phos {

namespace {

namespace fs = std::filesystem;

/** @brief One file. Entries live as long as the program, so an index into any catalogue stays valid. */
struct Entry {
    std::string m_path;   ///< the file
    std::string m_name;   ///< its name (the file name without extension)
    int m_category = -1;   ///< its category (field::), the hidden shot category, or -1
    std::atomic<FieldClip*> m_clip{ nullptr };   ///< the recording once loaded, else null
    std::atomic<bool> m_wanted{ false };   ///< the loader is asked to load it
    std::atomic<int64_t> m_lastUse{ 0 };   ///< when it was last asked for, ms
    bool m_pinned = false;   ///< made in memory (addFieldClip): no file behind it, never evicted
};

/** @brief One scan's result: immutable once published. */
struct Catalogue {
    std::vector<Entry*> m_entries;   ///< the files, in the order they were found
    std::vector<std::vector<int>> m_byCategory;   ///< per category: indices into entries
    std::vector<std::string> m_folders;   ///< the folders scanned
};

/** @brief An evicted recording, freed when its grace has run out. */
struct Retired {
    FieldClip* m_clip;   ///< the recording
    int64_t m_at;   ///< when it was evicted, ms
};

constexpr int64_t GraceMs = 5000;   ///< an evicted recording is freed this long after the eviction
constexpr int64_t IdleMs = 10000;   ///< and only a recording not asked for this long is evicted
std::atomic<size_t> gCacheBytes{ FieldCacheBytes };   ///< setFieldCacheBytes

/** @brief The library: the files ever seen, the catalogues, the cache, the loader thread. */
struct Library {
    std::mutex m_mutex;   ///< scans, loads, eviction, the loader's start and stop
    std::string m_searchPath;   ///< the search path (setFieldSearchPath)
    std::string m_userFolder;   ///< the user's folder
    bool m_stale = true;   ///< the next request scans again
    bool m_shipped = true;   ///< setFieldShippedFolder
    std::map<std::string, std::unique_ptr<Entry>> m_pool;   ///< every file ever seen, by path ("mem:" + name for addFieldClip)
    std::vector<Entry*> m_memory;   ///< the recordings made in memory, in every catalogue
    std::vector<std::unique_ptr<Catalogue>> m_catalogues;   ///< every catalogue ever published (a rescan is rare)
    std::atomic<Catalogue*> m_current{ nullptr };   ///< the catalogue in force
    std::vector<Retired> m_graveyard;   ///< evicted recordings waiting for their grace to run out
    size_t m_bytes = 0;   ///< bytes the loaded recordings take
    /// The loader thread.
    std::thread m_loader;
    int m_holders = 0;   ///< engines that hold the loader thread
    std::condition_variable m_wake;   ///< wakes the loader
    std::mutex m_wakeMutex;   ///< ... with this mutex
    std::atomic<bool> m_pending{ false };   ///< a load is asked for
    std::atomic<bool> m_stop{ false };   ///< the loader is to end
    ~Library()
    {
        // An engine that was never destroyed still holds the thread: let it go rather than join it here (a DLL's
        // static destructors run under the loader lock, where a join can deadlock).
        m_stop.store(true);
        m_wake.notify_all();
        if (m_loader.joinable()) m_loader.detach();
    }
};

/**
 * @brief The library, created on first use.
 * @return the library
 */
Library& lib()
{
    static Library l;
    return l;
}

/**
 * @brief The steady clock, ms.
 * @return the milliseconds
 */
int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** @brief The hidden category of the NASA shots, after the visible ones. */
constexpr int ShotCategory = field::Categories;

/**
 * @brief The category of @p file by its name ("fr-\<slug\>-..."), the NASA ones by their folder; -1 none.
 * @param[in] file     the file
 * @param[in] inNasa   true below the NASA folder
 * @param[in] inShots  true below its shots
 * @return the category, -1 for none
 */
int categoryOf(const fs::path& file, bool inNasa, bool inShots)
{
    if (inNasa) return inShots ? ShotCategory : field::Categories - 1;   // the last visible category is "nasa"
    const std::string stem = file.stem().string();
    if (stem.rfind("fr-", 0) != 0) return -1;
    for (int c = 0; c < field::Categories; ++c) {
        const std::string pre = std::string("fr-") + FieldCategorySlugs[c] + "-";
        if (stem.rfind(pre, 0) == 0) return c;
    }
    return -1;
}

/**
 * @brief Adds the .flac files under @p root to @p cat (new ones to the pool of @p l).
 * @param[in,out] l     the library
 * @param[in,out] cat   the catalogue, added to
 * @param[in]     root  the folder
 */
void scanFolder(Library& l, Catalogue& cat, const fs::path& root)
{
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    cat.m_folders.push_back(root.string());
    for (auto it = fs::recursive_directory_iterator(root, fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path p = it->path();
        std::string ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
        if (ext != ".flac") continue;
        bool inNasa = false, inShots = false;
        for (fs::path q = p.parent_path(); !q.empty() && q != root && q != q.parent_path(); q = q.parent_path()) {
            if (q.filename().string() == "nasa") inNasa = true;
            if (q.filename().string() == "shots") inShots = true;
        }
        const int c = categoryOf(p, inNasa, inShots);
        if (c < 0) continue;
        std::unique_ptr<Entry>& slot = l.m_pool[p.string()];
        if (!slot) {
            slot = std::make_unique<Entry>();
            slot->m_path = p.string();
            slot->m_name = p.stem().string();
            slot->m_category = c;
        }
        cat.m_entries.push_back(slot.get());
    }
}

/**
 * @brief Scans every root and publishes a new catalogue (the mutex held).
 * @param[in,out] l  the library
 */
void scanLocked(Library& l)
{
    auto cat = std::make_unique<Catalogue>();
    std::vector<fs::path> roots;
    roots.emplace_back("field");
    if (!l.m_searchPath.empty()) roots.push_back(fs::path(l.m_searchPath) / "field");
    if (!waveTableSearchPath().empty()) roots.push_back(fs::path(waveTableSearchPath()) / "field");
    // Where the Windows setup installs them (Deploy/Phosphene.iss, {autoappdata}\Phosphene\field): once for the
    // standalone and the plug-in, not a second 2 GB in the VST3 bundle -- the machine-wide place, or the user's.
    for (const char* var : { "PROGRAMDATA", "APPDATA" })
        if (const char* v = std::getenv(var)) if (*v != 0) roots.push_back(fs::path(v) / "Phosphene" / "field");
#if !defined(_WIN32)
    // Linux and macOS (02.10.2026): the user's data folder, where the README of their archives says to unpack them --
    // $XDG_DATA_HOME (else ~/.local/share) on Linux, ~/Library/Application Support on a Mac.
    {
        const char* home = std::getenv("HOME");
  #if defined(__APPLE__)
        if (home != nullptr && *home != 0) roots.push_back(fs::path(home) / "Library" / "Application Support" / "Phosphene" / "field");
  #else
        const char* xdg = std::getenv("XDG_DATA_HOME");
        if (xdg != nullptr && *xdg != 0) roots.push_back(fs::path(xdg) / "Phosphene" / "field");
        else if (home != nullptr && *home != 0) roots.push_back(fs::path(home) / ".local" / "share" / "Phosphene" / "field");
  #endif
    }
#endif
#if defined(PHOS_SOURCE_DATA_DIR)
    roots.push_back(fs::path(PHOS_SOURCE_DATA_DIR) / "field");
#endif
    // The shipped folder: the first of these that exists.
    if (!l.m_shipped) roots.clear();
    for (const fs::path& r : roots) {
        std::error_code ec;
        if (fs::is_directory(r, ec)) { scanFolder(l, *cat, r); break; }
    }
    if (!l.m_userFolder.empty()) scanFolder(l, *cat, fs::path(l.m_userFolder));
    cat->m_entries.insert(cat->m_entries.end(), l.m_memory.begin(), l.m_memory.end());
    // By category, then by name; a name twice (the same recording in both folders) once.
    std::stable_sort(cat->m_entries.begin(), cat->m_entries.end(), [](const Entry* a, const Entry* b) {
        return a->m_category != b->m_category ? a->m_category < b->m_category : a->m_name < b->m_name;
    });
    cat->m_entries.erase(std::unique(cat->m_entries.begin(), cat->m_entries.end(), [](const Entry* a, const Entry* b) {
        return a->m_category == b->m_category && a->m_name == b->m_name;
    }), cat->m_entries.end());
    cat->m_byCategory.assign(field::Categories + 1, {});
    for (int i = 0; i < static_cast<int>(cat->m_entries.size()); ++i)
        cat->m_byCategory[static_cast<size_t>(cat->m_entries[static_cast<size_t>(i)]->m_category)].push_back(i);
    l.m_current.store(cat.get(), std::memory_order_release);
    l.m_catalogues.push_back(std::move(cat));
    l.m_stale = false;
}

/**
 * @brief Entry @p index of the current catalogue, null where there is none.
 * @param[in] index  the index
 * @return the entry, or null
 */
Entry* entryAt(int index)
{
    const Catalogue* c = lib().m_current.load(std::memory_order_acquire);
    if (c == nullptr || index < 0 || index >= static_cast<int>(c->m_entries.size())) return nullptr;
    return c->m_entries[static_cast<size_t>(index)];
}

void normalise(FieldClip& clip);

/**
 * @brief Decodes one file (outside every lock: a long recording takes a second).
 * @param[in] e  the entry
 * @return the recording, or null
 */
FieldClip* decodeFile(const Entry& e)
{
    unsigned int channels = 0, rate = 0;
    drflac_uint64 frames = 0;
    float* pcm = drflac_open_file_and_read_pcm_frames_f32(e.m_path.c_str(), &channels, &rate, &frames, nullptr);
    if (pcm == nullptr) return nullptr;
    if (channels == 0 || frames == 0) { drflac_free(pcm, nullptr); return nullptr; }
    auto* c = new FieldClip();
    c->m_name = e.m_name;
    c->m_category = e.m_category;
    c->m_sampleRate = static_cast<int>(rate);
    c->m_left.resize(static_cast<size_t>(frames));
    if (channels > 1) c->m_right.resize(static_cast<size_t>(frames));
    const size_t ch = channels;
    for (size_t i = 0; i < static_cast<size_t>(frames); ++i) {
        c->m_left[i] = pcm[i * ch];
        if (channels > 1) c->m_right[i] = pcm[i * ch + 1];
    }
    drflac_free(pcm, nullptr);
    normalise(*c);
    return c;
}

/**
 * @brief Sets the clip's norm (FieldClip::norm): RMS to -20 dBFS, the peak held at -1 dBFS.
 * @param[in,out] clip  the recording
 */
void normalise(FieldClip& clip)
{
    FieldClip* c = &clip;
    double sum = 0.0;
    float peak = 0.0f;
    const float* l = c->dataL();
    const float* r = c->dataR();
    for (size_t i = 0; i < c->m_left.size(); ++i) {
        sum += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
        peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
    }
    const double rms = std::sqrt(sum / (2.0 * static_cast<double>(c->m_left.size()))) + 1e-9;
    c->m_norm = static_cast<float>(std::min(0.1 / rms, 0.891 / (static_cast<double>(peak) + 1e-9)));
}

/**
 * @brief The bytes recording @p c takes.
 * @param[in] c  the recording
 * @return the bytes
 */
size_t clipBytes(const FieldClip* c) { return (c->m_left.size() + c->m_right.size()) * sizeof(float); }

/**
 * @brief Frees what has served its grace, then evicts idle recordings until @p need more bytes fit. Under the lock.
 * @param[in,out] l     the library
 * @param[in]     need  the bytes wanted
 */
void makeRoom(Library& l, size_t need)
{
    const int64_t now = nowMs();
    l.m_graveyard.erase(std::remove_if(l.m_graveyard.begin(), l.m_graveyard.end(), [&](const Retired& r) {
        if (now - r.m_at < GraceMs || r.m_clip->m_users.load() != 0) return false;
        delete r.m_clip;
        return true;
    }), l.m_graveyard.end());
    while (l.m_bytes + need > gCacheBytes.load()) {
        Entry* victim = nullptr;
        for (auto& [path, e] : l.m_pool) {
            FieldClip* c = e->m_clip.load();
            if (c == nullptr || e->m_pinned || c->m_users.load() != 0 || now - e->m_lastUse.load() < IdleMs) continue;
            if (victim == nullptr || e->m_lastUse.load() < victim->m_lastUse.load()) victim = e.get();
        }
        if (victim == nullptr) return;   // everything in memory is playing or just asked for: go over the budget
        FieldClip* c = victim->m_clip.exchange(nullptr);
        l.m_bytes -= clipBytes(c);
        l.m_graveyard.push_back({ c, now });
    }
}

/**
 * @brief Loads @p e unless it is loaded (making room in the cache); false if it cannot be read.
 * @param[in,out] l  the library
 * @param[in,out] e  the entry
 * @return false if it cannot be read
 */
bool loadEntry(Library& l, Entry* e)
{
    if (e == nullptr) return false;
    e->m_lastUse.store(nowMs());
    if (e->m_clip.load() != nullptr) return true;
    FieldClip* c = decodeFile(*e);
    if (c == nullptr) return false;
    std::lock_guard<std::mutex> g(l.m_mutex);
    if (e->m_clip.load() != nullptr) { delete c; return true; }   // another thread was quicker
    makeRoom(l, clipBytes(c));
    l.m_bytes += clipBytes(c);
    e->m_clip.store(c, std::memory_order_release);
    return true;
}

/** @brief The loader thread: loads what is wanted, until stop. */
void loaderMain()
{
    Library& l = lib();
    while (!l.m_stop.load()) {
        {
            std::unique_lock<std::mutex> lk(l.m_wakeMutex);
            l.m_wake.wait_for(lk, std::chrono::milliseconds(100), [&] { return l.m_pending.load() || l.m_stop.load(); });
        }
        if (l.m_stop.load()) break;
        if (!l.m_pending.exchange(false)) continue;
        const Catalogue* c = l.m_current.load(std::memory_order_acquire);
        if (c == nullptr) continue;
        for (Entry* e : c->m_entries) {
            if (l.m_stop.load()) break;
            if (e->m_wanted.exchange(false)) loadEntry(l, e);
        }
    }
}

} // namespace

void setFieldSearchPath(const std::string& directory)
{
    Library& l = lib();
    std::lock_guard<std::mutex> g(l.m_mutex);
    if (l.m_searchPath != directory) { l.m_searchPath = directory; l.m_stale = true; }
}

void setFieldCacheBytes(size_t bytes)
{
    gCacheBytes.store(bytes);
}

void setFieldShippedFolder(bool enabled)
{
    Library& l = lib();
    std::lock_guard<std::mutex> g(l.m_mutex);
    if (l.m_shipped != enabled) { l.m_shipped = enabled; l.m_stale = true; }
}

void setFieldUserFolder(const std::string& directory)
{
    Library& l = lib();
    std::lock_guard<std::mutex> g(l.m_mutex);
    if (l.m_userFolder != directory) { l.m_userFolder = directory; l.m_stale = true; }
}

int scanFieldLibrary()
{
    Library& l = lib();
    std::lock_guard<std::mutex> g(l.m_mutex);
    if (l.m_stale) scanLocked(l);
    return static_cast<int>(l.m_current.load()->m_entries.size());
}

std::vector<std::string> fieldLibraryFolders()
{
    scanFieldLibrary();
    return lib().m_current.load(std::memory_order_acquire)->m_folders;
}

int fieldClipCount(int category)
{
    const Catalogue* c = lib().m_current.load(std::memory_order_acquire);
    if (c == nullptr || category < 0 || category >= field::Categories) return 0;
    return static_cast<int>(c->m_byCategory[static_cast<size_t>(category)].size());
}

namespace {
/**
 * @brief The index of variation @p variation (wrapped) of category @p category, -1 none.
 * @param[in] category   the category
 * @param[in] variation  the variation
 * @return the index, -1 for none
 */
int indexIn(int category, int variation)
{
    const Catalogue* c = lib().m_current.load(std::memory_order_acquire);
    if (c == nullptr || category < 0 || category >= static_cast<int>(c->m_byCategory.size())) return -1;
    const std::vector<int>& v = c->m_byCategory[static_cast<size_t>(category)];
    const int n = static_cast<int>(v.size());
    return n == 0 ? -1 : v[static_cast<size_t>(((variation % n) + n) % n)];
}
} // namespace

int fieldClipIndex(int category, int variation)
{
    return category >= 0 && category < field::Categories ? indexIn(category, variation) : -1;
}

int fieldShotCount()
{
    const Catalogue* c = lib().m_current.load(std::memory_order_acquire);
    return c == nullptr || c->m_byCategory.size() <= static_cast<size_t>(ShotCategory) ? 0 : static_cast<int>(c->m_byCategory[ShotCategory].size());
}

int fieldShotIndex(int i)
{
    return indexIn(ShotCategory, i);
}

std::string fieldClipName(int index)
{
    const Entry* e = entryAt(index);
    return e != nullptr ? e->m_name : std::string();
}

FieldClip* acquireFieldClip(int index)
{
    Entry* e = entryAt(index);
    if (e == nullptr) return nullptr;
    FieldClip* c = e->m_clip.load(std::memory_order_acquire);
    if (c == nullptr) return nullptr;
    c->m_users.fetch_add(1);
    e->m_lastUse.store(nowMs());
    return c;
}

void releaseFieldClip(FieldClip* clip)
{
    if (clip != nullptr) clip->m_users.fetch_sub(1);
}

bool fieldClipLoaded(int index)
{
    const Entry* e = entryAt(index);
    return e != nullptr && e->m_clip.load(std::memory_order_acquire) != nullptr;
}

void requestFieldClip(int index)
{
    Library& l = lib();
    Entry* e = entryAt(index);
    if (e == nullptr || e->m_clip.load() != nullptr || e->m_wanted.load()) return;
    e->m_lastUse.store(nowMs());
    e->m_wanted.store(true);
    l.m_pending.store(true);
    l.m_wake.notify_one();
}

bool loadFieldClipNow(int index)
{
    scanFieldLibrary();
    return loadEntry(lib(), entryAt(index));
}

void addFieldClip(int category, const std::string& name, std::vector<float> left, std::vector<float> right, int sampleRate)
{
    Library& l = lib();
    auto* c = new FieldClip();
    c->m_name = name;
    c->m_category = category;
    c->m_sampleRate = sampleRate;
    c->m_left = std::move(left);
    if (right.size() == c->m_left.size()) c->m_right = std::move(right);
    normalise(*c);
    std::lock_guard<std::mutex> g(l.m_mutex);
    std::unique_ptr<Entry>& slot = l.m_pool["mem:" + name];
    if (!slot) {
        slot = std::make_unique<Entry>();
        slot->m_name = name;
        slot->m_category = category;
        slot->m_pinned = true;
        l.m_memory.push_back(slot.get());
    } else if (FieldClip* old = slot->m_clip.exchange(nullptr)) {
        l.m_graveyard.push_back({ old, nowMs() });
    }
    slot->m_clip.store(c, std::memory_order_release);
    l.m_stale = true;
}

namespace {
std::atomic<bool> gPreloadBlocking{ false };   ///< preloads load at once on the calling thread (the renders), not on the loader
} // namespace

void setFieldPreloadBlocking(bool blocking)
{
    gPreloadBlocking.store(blocking);
}

void preloadFieldClip(int index)
{
    if (index < 0) return;
    if (gPreloadBlocking.load()) loadFieldClipNow(index);
    else requestFieldClip(index);
}

void retainFieldLoader()
{
    Library& l = lib();
    std::lock_guard<std::mutex> g(l.m_mutex);
    if (l.m_holders++ == 0) {
        l.m_stop.store(false);
        l.m_loader = std::thread(loaderMain);
    }
}

void releaseFieldLoader()
{
    Library& l = lib();
    std::thread t;
    {
        std::lock_guard<std::mutex> g(l.m_mutex);
        if (l.m_holders == 0 || --l.m_holders != 0) return;
        l.m_stop.store(true);
        t = std::move(l.m_loader);
    }
    l.m_wake.notify_all();
    if (t.joinable()) t.join();
}

} // namespace phos
