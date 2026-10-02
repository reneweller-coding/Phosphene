/**
 * @file FieldLibrary.cpp
 * @brief The Field track's recordings: the scan, the loads, the loader thread and the cache (FieldLibrary.h).
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
    std::string path;   ///< the file
    std::string name;   ///< its name (the file name without extension)
    int category = -1;   ///< its category (field::), the hidden shot category, or -1
    std::atomic<FieldClip*> clip{ nullptr };   ///< the recording once loaded, else null
    std::atomic<bool> wanted{ false };   ///< the loader is asked to load it
    std::atomic<int64_t> lastUse{ 0 };   ///< when it was last asked for, ms
    bool pinned = false;   ///< made in memory (addFieldClip): no file behind it, never evicted
};

/** @brief One scan's result: immutable once published. */
struct Catalogue {
    std::vector<Entry*> entries;   ///< the files, in the order they were found
    std::vector<std::vector<int>> byCategory;   ///< per category: indices into entries
    std::vector<std::string> folders;   ///< the folders scanned
};

/** @brief An evicted recording, freed when its grace has run out. */
struct Retired {
    FieldClip* clip;   ///< the recording
    int64_t at;   ///< when it was evicted, ms
};

constexpr int64_t kGraceMs = 5000;   ///< an evicted recording is freed this long after the eviction
constexpr int64_t kIdleMs = 10000;   ///< and only a recording not asked for this long is evicted
std::atomic<size_t> gCacheBytes{ kFieldCacheBytes };   ///< setFieldCacheBytes

/** @brief The library: the files ever seen, the catalogues, the cache, the loader thread. */
struct Library {
    std::mutex mutex;   ///< scans, loads, eviction, the loader's start and stop
    std::string searchPath;   ///< the search path (setFieldSearchPath)
    std::string userFolder;   ///< the user's folder
    bool stale = true;   ///< the next request scans again
    bool shipped = true;   ///< setFieldShippedFolder
    std::map<std::string, std::unique_ptr<Entry>> pool;   ///< every file ever seen, by path ("mem:" + name for addFieldClip)
    std::vector<Entry*> memory;   ///< the recordings made in memory, in every catalogue
    std::vector<std::unique_ptr<Catalogue>> catalogues;   ///< every catalogue ever published (a rescan is rare)
    std::atomic<Catalogue*> current{ nullptr };   ///< the catalogue in force
    std::vector<Retired> graveyard;   ///< evicted recordings waiting for their grace to run out
    size_t bytes = 0;   ///< bytes the loaded recordings take
    /// The loader thread.
    std::thread loader;
    int holders = 0;   ///< engines that hold the loader thread
    std::condition_variable wake;   ///< wakes the loader
    std::mutex wakeMutex;   ///< ... with this mutex
    std::atomic<bool> pending{ false };   ///< a load is asked for
    std::atomic<bool> stop{ false };   ///< the loader is to end
    ~Library()
    {
        // An engine that was never destroyed still holds the thread: let it go rather than join it here (a DLL's
        // static destructors run under the loader lock, where a join can deadlock).
        stop.store(true);
        wake.notify_all();
        if (loader.joinable()) loader.detach();
    }
};

/** @brief The library, created on first use. */
Library& lib()
{
    static Library l;
    return l;
}

/** @brief The steady clock, ms. */
int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

/** @brief The hidden category of the NASA shots, after the visible ones. */
constexpr int kShotCategory = field::kCategories;

/** @brief The category of @p file by its name ("fr-<slug>-..."), the NASA ones by their folder; -1 none. */
int categoryOf(const fs::path& file, bool inNasa, bool inShots)
{
    if (inNasa) return inShots ? kShotCategory : field::kCategories - 1;   // the last visible category is "nasa"
    const std::string stem = file.stem().string();
    if (stem.rfind("fr-", 0) != 0) return -1;
    for (int c = 0; c < field::kCategories; ++c) {
        const std::string pre = std::string("fr-") + kFieldCategorySlugs[c] + "-";
        if (stem.rfind(pre, 0) == 0) return c;
    }
    return -1;
}

/** @brief Adds the .flac files under @p root to @p cat (new ones to the pool of @p l). */
void scanFolder(Library& l, Catalogue& cat, const fs::path& root)
{
    std::error_code ec;
    if (!fs::is_directory(root, ec)) return;
    cat.folders.push_back(root.string());
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
        std::unique_ptr<Entry>& slot = l.pool[p.string()];
        if (!slot) {
            slot = std::make_unique<Entry>();
            slot->path = p.string();
            slot->name = p.stem().string();
            slot->category = c;
        }
        cat.entries.push_back(slot.get());
    }
}

/** @brief Scans every root and publishes a new catalogue (the mutex held). */
void scanLocked(Library& l)
{
    auto cat = std::make_unique<Catalogue>();
    std::vector<fs::path> roots;
    roots.emplace_back("field");
    if (!l.searchPath.empty()) roots.push_back(fs::path(l.searchPath) / "field");
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
    if (!l.shipped) roots.clear();
    for (const fs::path& r : roots) {
        std::error_code ec;
        if (fs::is_directory(r, ec)) { scanFolder(l, *cat, r); break; }
    }
    if (!l.userFolder.empty()) scanFolder(l, *cat, fs::path(l.userFolder));
    cat->entries.insert(cat->entries.end(), l.memory.begin(), l.memory.end());
    // By category, then by name; a name twice (the same recording in both folders) once.
    std::stable_sort(cat->entries.begin(), cat->entries.end(), [](const Entry* a, const Entry* b) {
        return a->category != b->category ? a->category < b->category : a->name < b->name;
    });
    cat->entries.erase(std::unique(cat->entries.begin(), cat->entries.end(), [](const Entry* a, const Entry* b) {
        return a->category == b->category && a->name == b->name;
    }), cat->entries.end());
    cat->byCategory.assign(field::kCategories + 1, {});
    for (int i = 0; i < static_cast<int>(cat->entries.size()); ++i)
        cat->byCategory[static_cast<size_t>(cat->entries[static_cast<size_t>(i)]->category)].push_back(i);
    l.current.store(cat.get(), std::memory_order_release);
    l.catalogues.push_back(std::move(cat));
    l.stale = false;
}

/** @brief Entry @p index of the current catalogue, null where there is none. */
Entry* entryAt(int index)
{
    const Catalogue* c = lib().current.load(std::memory_order_acquire);
    if (c == nullptr || index < 0 || index >= static_cast<int>(c->entries.size())) return nullptr;
    return c->entries[static_cast<size_t>(index)];
}

void normalise(FieldClip& clip);

/** @brief Decodes one file (outside every lock: a long recording takes a second). */
FieldClip* decodeFile(const Entry& e)
{
    unsigned int channels = 0, rate = 0;
    drflac_uint64 frames = 0;
    float* pcm = drflac_open_file_and_read_pcm_frames_f32(e.path.c_str(), &channels, &rate, &frames, nullptr);
    if (pcm == nullptr) return nullptr;
    if (channels == 0 || frames == 0) { drflac_free(pcm, nullptr); return nullptr; }
    auto* c = new FieldClip();
    c->name = e.name;
    c->category = e.category;
    c->sampleRate = static_cast<int>(rate);
    c->left.resize(static_cast<size_t>(frames));
    if (channels > 1) c->right.resize(static_cast<size_t>(frames));
    const size_t ch = channels;
    for (size_t i = 0; i < static_cast<size_t>(frames); ++i) {
        c->left[i] = pcm[i * ch];
        if (channels > 1) c->right[i] = pcm[i * ch + 1];
    }
    drflac_free(pcm, nullptr);
    normalise(*c);
    return c;
}

/** @brief Sets the clip's norm (FieldClip::norm): RMS to -20 dBFS, the peak held at -1 dBFS. */
void normalise(FieldClip& clip)
{
    FieldClip* c = &clip;
    double sum = 0.0;
    float peak = 0.0f;
    const float* l = c->dataL();
    const float* r = c->dataR();
    for (size_t i = 0; i < c->left.size(); ++i) {
        sum += static_cast<double>(l[i]) * l[i] + static_cast<double>(r[i]) * r[i];
        peak = std::max(peak, std::max(std::fabs(l[i]), std::fabs(r[i])));
    }
    const double rms = std::sqrt(sum / (2.0 * static_cast<double>(c->left.size()))) + 1e-9;
    c->norm = static_cast<float>(std::min(0.1 / rms, 0.891 / (static_cast<double>(peak) + 1e-9)));
}

/** @brief The bytes recording @p c takes. */
size_t clipBytes(const FieldClip* c) { return (c->left.size() + c->right.size()) * sizeof(float); }

/** @brief Frees what has served its grace, then evicts idle recordings until @p need more bytes fit. Under the lock. */
void makeRoom(Library& l, size_t need)
{
    const int64_t now = nowMs();
    l.graveyard.erase(std::remove_if(l.graveyard.begin(), l.graveyard.end(), [&](const Retired& r) {
        if (now - r.at < kGraceMs || r.clip->users.load() != 0) return false;
        delete r.clip;
        return true;
    }), l.graveyard.end());
    while (l.bytes + need > gCacheBytes.load()) {
        Entry* victim = nullptr;
        for (auto& [path, e] : l.pool) {
            FieldClip* c = e->clip.load();
            if (c == nullptr || e->pinned || c->users.load() != 0 || now - e->lastUse.load() < kIdleMs) continue;
            if (victim == nullptr || e->lastUse.load() < victim->lastUse.load()) victim = e.get();
        }
        if (victim == nullptr) return;   // everything in memory is playing or just asked for: go over the budget
        FieldClip* c = victim->clip.exchange(nullptr);
        l.bytes -= clipBytes(c);
        l.graveyard.push_back({ c, now });
    }
}

/** @brief Loads @p e unless it is loaded (making room in the cache); false if it cannot be read. */
bool loadEntry(Library& l, Entry* e)
{
    if (e == nullptr) return false;
    e->lastUse.store(nowMs());
    if (e->clip.load() != nullptr) return true;
    FieldClip* c = decodeFile(*e);
    if (c == nullptr) return false;
    std::lock_guard<std::mutex> g(l.mutex);
    if (e->clip.load() != nullptr) { delete c; return true; }   // another thread was quicker
    makeRoom(l, clipBytes(c));
    l.bytes += clipBytes(c);
    e->clip.store(c, std::memory_order_release);
    return true;
}

/** @brief The loader thread: loads what is wanted, until stop. */
void loaderMain()
{
    Library& l = lib();
    while (!l.stop.load()) {
        {
            std::unique_lock<std::mutex> lk(l.wakeMutex);
            l.wake.wait_for(lk, std::chrono::milliseconds(100), [&] { return l.pending.load() || l.stop.load(); });
        }
        if (l.stop.load()) break;
        if (!l.pending.exchange(false)) continue;
        const Catalogue* c = l.current.load(std::memory_order_acquire);
        if (c == nullptr) continue;
        for (Entry* e : c->entries) {
            if (l.stop.load()) break;
            if (e->wanted.exchange(false)) loadEntry(l, e);
        }
    }
}

} // namespace

void setFieldSearchPath(const std::string& directory)
{
    Library& l = lib();
    std::lock_guard<std::mutex> g(l.mutex);
    if (l.searchPath != directory) { l.searchPath = directory; l.stale = true; }
}

void setFieldCacheBytes(size_t bytes)
{
    gCacheBytes.store(bytes);
}

void setFieldShippedFolder(bool enabled)
{
    Library& l = lib();
    std::lock_guard<std::mutex> g(l.mutex);
    if (l.shipped != enabled) { l.shipped = enabled; l.stale = true; }
}

void setFieldUserFolder(const std::string& directory)
{
    Library& l = lib();
    std::lock_guard<std::mutex> g(l.mutex);
    if (l.userFolder != directory) { l.userFolder = directory; l.stale = true; }
}

int scanFieldLibrary()
{
    Library& l = lib();
    std::lock_guard<std::mutex> g(l.mutex);
    if (l.stale) scanLocked(l);
    return static_cast<int>(l.current.load()->entries.size());
}

std::vector<std::string> fieldLibraryFolders()
{
    scanFieldLibrary();
    return lib().current.load(std::memory_order_acquire)->folders;
}

int fieldClipCount(int category)
{
    const Catalogue* c = lib().current.load(std::memory_order_acquire);
    if (c == nullptr || category < 0 || category >= field::kCategories) return 0;
    return static_cast<int>(c->byCategory[static_cast<size_t>(category)].size());
}

namespace {
/** @brief The index of variation @p variation (wrapped) of category @p category, -1 none. */
int indexIn(int category, int variation)
{
    const Catalogue* c = lib().current.load(std::memory_order_acquire);
    if (c == nullptr || category < 0 || category >= static_cast<int>(c->byCategory.size())) return -1;
    const std::vector<int>& v = c->byCategory[static_cast<size_t>(category)];
    const int n = static_cast<int>(v.size());
    return n == 0 ? -1 : v[static_cast<size_t>(((variation % n) + n) % n)];
}
} // namespace

int fieldClipIndex(int category, int variation)
{
    return category >= 0 && category < field::kCategories ? indexIn(category, variation) : -1;
}

int fieldShotCount()
{
    const Catalogue* c = lib().current.load(std::memory_order_acquire);
    return c == nullptr || c->byCategory.size() <= static_cast<size_t>(kShotCategory) ? 0 : static_cast<int>(c->byCategory[kShotCategory].size());
}

int fieldShotIndex(int i)
{
    return indexIn(kShotCategory, i);
}

std::string fieldClipName(int index)
{
    const Entry* e = entryAt(index);
    return e != nullptr ? e->name : std::string();
}

FieldClip* acquireFieldClip(int index)
{
    Entry* e = entryAt(index);
    if (e == nullptr) return nullptr;
    FieldClip* c = e->clip.load(std::memory_order_acquire);
    if (c == nullptr) return nullptr;
    c->users.fetch_add(1);
    e->lastUse.store(nowMs());
    return c;
}

void releaseFieldClip(FieldClip* clip)
{
    if (clip != nullptr) clip->users.fetch_sub(1);
}

bool fieldClipLoaded(int index)
{
    const Entry* e = entryAt(index);
    return e != nullptr && e->clip.load(std::memory_order_acquire) != nullptr;
}

void requestFieldClip(int index)
{
    Library& l = lib();
    Entry* e = entryAt(index);
    if (e == nullptr || e->clip.load() != nullptr || e->wanted.load()) return;
    e->lastUse.store(nowMs());
    e->wanted.store(true);
    l.pending.store(true);
    l.wake.notify_one();
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
    c->name = name;
    c->category = category;
    c->sampleRate = sampleRate;
    c->left = std::move(left);
    if (right.size() == c->left.size()) c->right = std::move(right);
    normalise(*c);
    std::lock_guard<std::mutex> g(l.mutex);
    std::unique_ptr<Entry>& slot = l.pool["mem:" + name];
    if (!slot) {
        slot = std::make_unique<Entry>();
        slot->name = name;
        slot->category = category;
        slot->pinned = true;
        l.memory.push_back(slot.get());
    } else if (FieldClip* old = slot->clip.exchange(nullptr)) {
        l.graveyard.push_back({ old, nowMs() });
    }
    slot->clip.store(c, std::memory_order_release);
    l.stale = true;
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
    std::lock_guard<std::mutex> g(l.mutex);
    if (l.holders++ == 0) {
        l.stop.store(false);
        l.loader = std::thread(loaderMain);
    }
}

void releaseFieldLoader()
{
    Library& l = lib();
    std::thread t;
    {
        std::lock_guard<std::mutex> g(l.mutex);
        if (l.holders == 0 || --l.holders != 0) return;
        l.stop.store(true);
        t = std::move(l.loader);
    }
    l.wake.notify_all();
    if (t.joinable()) t.join();
}

} // namespace phos
