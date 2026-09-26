/**
 * @file main.cpp
 * @brief phos_render: renders a set offline, exports its MIDI, measures loudness and speed.
 *
 * Usage:
 * @code
 *   phos_render [options]
 *     --bars N            bars to render (default 16)
 *     --seconds S         render S seconds instead of a bar count
 *     --minutes M         render M minutes
 *     --tracks            print the plan of every track in the render (key, tempo, patterns, recipes)
 *     --sr RATE           sample rate (default 48000)
 *     --block N           block size handed to the engine (default 256)
 *     --quality LEVEL     desktop (default) or quest: what the engine may spend per part (Quality.h)
 *     --seed N            set seed (default 1)
 *     --set key=value     set a parameter; repeatable; "key=a;key2=b" also works
 *     --preset FILE       read key=value assignments from a file
 *     --set-file FILE     read a .phosset (seed, style, arc, knobs, locks, rerolls)
 *     --save-set FILE     write the set as a .phosset
 *     --lock UNIT:INDEX   lock a unit (set, track, section, lane); repeatable
 *     --reroll UNIT:INDEX reroll a unit; repeatable
 *     --sections          print the sections of every track in the render
 *     --tempo-ramp B:BPM  ramp the tempo from compose.bpm at beat 0 to BPM at beat B
 *     --solo PART         mute everything else: kick, bass, perc, acid, lead, counter, arp, stab, pad, drone,
 *                         sfx, texture or vocal
 *     --out FILE.wav      write the audio (32-bit float unless --pcm24)
 *     --pcm24             write 24-bit PCM
 *     --midi FILE.mid     write the score as a Standard MIDI File
 *     --report            print loudness, peak and timing
 *     --bar-log FILE.tsv  write one line per bar: owner track, section, output power and what the score
 *                         plays there (kicks, bass, percussion lanes, parts, the snare roll's spacing, what
 *                         sounds on beat 4) -- the arrangement round's measurements (19.09.2026)
 *     --score-only        with --bar-log: compose the score without rendering audio (the power column is -200)
 *     --mix-log FILE.tsv  what Tools/mix_audit.py needs to read a render against the mix guide (25.09.2026):
 *                         "bar <n> <first sample> <track> <section>" per bar, "kick <sample>" and "bass <sample>"
 *                         per onset, "gr <sample> <limiter dB> <compressor dB>" per block (the reduction at the
 *                         block's end, so --block 32 gives it every 0.7 ms). Samples are positions in the
 *                         mix WAV, the master's lookahead included.
 *     --excerpts DIR      the listening bench (23.09.2026): while the set renders, cut one WAV per section of
 *                         every track into DIR -- the section's first --excerpt-bars bars, a buildup's last
 *                         bars together with the drop it lands in -- named by seed, track, style, section and
 *                         bar, with an index.tsv beside them (Tools/listen_bench.py drives it per style)
 *     --excerpt-bars N    length of those excerpts in bars (default 16)
 *     --stems DIR         also write one WAV per part and one for the returns into DIR (23.09.2026): each part as it
 *                         enters the master (Engine.h, StemTap), delayed by the limiter's lookahead so that the
 *                         stems line up with --out sample for sample; same format as --out (--pcm24 applies)
 *     --audibility F.tsv  how much of each part is heard in the mix (23.09.2026, phos/Audibility.h): per track and
 *                         section, every sounding part's loudness alone, its partial loudness against all other
 *                         parts, and the ratio of the two; printed as one line per section, written as a table
 *     --dj-export DIR     the set for DJ software (23.09.2026): every track the render covers as a WAV of its own,
 *                         rendered alone (no blend into its neighbours: Composer::setSoloTrack) at its own constant
 *                         tempo with the first downbeat on sample 0, a cue marker per section and tags inside, plus
 *                         rekordbox.xml (beat grid, key, the eight sections as hot cues and memory cues), set.m3u8
 *                         and cues.tsv; the set itself is not rendered
 *     --preferences FILE  plan with the listener's learned preferences (phos/Preferences.h): the form template, lead
 *                         archetype, arp style, counter mode and loop-or-pendulum draws are reweighted by them
 *     --learn IN.tsv OUT  fit preferences from a ratings file (the plugin's ratings.tsv, or Tools/ratings.py --out) and
 *                         write them to OUT, then exit; the verdicts must carry their features (the eleventh column)
 *     --bench             render without writing and report the realtime factor
 *     --list              print every parameter with range and default
 *     --version           print the version, the vector path and the wavetable pack, then exit
 * @endcode
 */
#include "phos/SoundPresets.h"
#include "phos/Audibility.h"
#include "phos/Composer.h"
#include "phos/Engine.h"
#include "phos/Loudness.h"
#include "phos/Midi.h"
#include "phos/Model.h"
#include "phos/Preferences.h"
#include "phos/Rating.h"
#include "phos/Quality.h"
#include "phos/SetFile.h"
#include "phos/Sfx.h"
#include "phos/Vec.h"
#include "phos/WaveTableFile.h"
#include "phos/WavWriter.h"
#if defined(_WIN32)
// Only for GetModuleFileNameA (executableDirectory below). NOMINMAX is not optional: without it
// windows.h defines min and max as macros and every std::max in this file stops compiling.
#  define WIN32_LEAN_AND_MEAN
#  define NOMINMAX
#  include <windows.h>
#else
#  include <unistd.h>
#endif
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace phos;

namespace {

/** @brief --list: every parameter with its range and default. */
void printList(const ParamStore& p)
{
    for (int i = 0; i < p.count(); ++i) {
        const ParamDesc& d = p.desc(i);
        std::printf("%-26s %-16s ", p.key(i).c_str(), d.name);
        if (d.curve == Curve::Choice && d.choices != nullptr) {
            for (int c = 0; c <= static_cast<int>(d.maxValue); ++c) std::printf("%s%s", c ? "|" : "", d.choices[c]);
            std::printf("  (default %s)\n", d.choices[static_cast<int>(d.defValue)]);
        } else {
            std::printf("%g..%g %s  (default %g)\n", static_cast<double>(d.minValue), static_cast<double>(d.maxValue), d.unit, static_cast<double>(d.defValue));
        }
    }
}

/**
 * @brief The directory this executable lies in, or an empty string when it cannot be determined.
 *
 * Not `argv[0]`: that is whatever the caller typed, and on a shell that resolved the name through
 * PATH it is a bare name with no directory in it at all.
 */
std::string executableDirectory()
{
#if defined(_WIN32)
    char buf[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buf, static_cast<DWORD>(sizeof(buf)));
    if (n == 0 || n >= sizeof(buf)) return {};
    std::string path(buf, n);
#else
    char buf[4096];
    const ssize_t n = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return {};
    std::string path(buf, static_cast<size_t>(n));
#endif
    const size_t cut = path.find_last_of("/\\");
    return cut == std::string::npos ? std::string{} : path.substr(0, cut);
}

/**
 * @brief Points the core at the data files installed beside this executable.
 *
 * The core opens `library.phoswt`, `melody.phosmdl` and `bass.phosmdl` by bare name. Its lookup is
 * (1) the name as given, relative to the working directory, (2) the directory a host declared with
 * setWaveTableSearchPath()/setModelSearchPath(), (3) `PHOS_SOURCE_DATA_DIR`, which only ever names
 * a source tree. An installed `phos_render.exe` has neither (1) -- the working directory is
 * wherever the user's shell happens to be -- nor (3), so without this it would fall back to the six
 * built-in wavetables and to the Markov model and say so only on stderr, which is the kind of
 * failure that ships unnoticed. The plugin does the same thing for itself
 * (Plugin/PluginProcessor.cpp, installSearchPaths).
 *
 * A development build is unaffected: nothing is installed beside the built binary, the search path
 * finds nothing, and the lookup falls through to `PHOS_SOURCE_DATA_DIR` exactly as before. That
 * matters -- the default render is the determinism oracle and has to stay byte-identical. A
 * shipping build has no step (3) at all (PHOS_SHIP, Core/CMakeLists.txt), which is what makes a
 * missing data file observable instead of silently repaired by the build machine's source tree.
 */
void installDataSearchPath()
{
    const std::string home = executableDirectory();
    if (home.empty()) return;
    setWaveTableSearchPath(home);
    setModelSearchPath(home);
}

/**
 * @brief What this binary is, in two lines meant to be read by a person and by a script.
 *
 * The version comes from `PHOS_VERSION`, which the root CMakeLists.txt defines out of its
 * `project(... VERSION ...)` line -- the one place that spells it out. Without the definition the
 * build is not one of ours (a hand-made compile of this file, say), and it says so rather than
 * pretending to a number.
 *
 * The second line is the vector path the core was compiled for. It is not decoration: the release
 * build turns AVX2 on, an AVX2 binary dies with an illegal instruction on a processor without it,
 * and Tools/release/check_package.ps1 reads this line back to prove the staged binary is the one
 * that was meant to be staged.
 */
void printVersion()
{
#if defined(PHOS_VERSION)
    std::printf("phos_render %s\n", PHOS_VERSION);
#else
    std::printf("phos_render unversioned\n");
#endif
    std::printf("vector path: %s\n", kVecPathName);
}

/**
 * @brief One excerpt of the listening bench (--excerpts, 23.09.2026): a sample window of the render that is
 *        copied into a file of its own while the set plays through.
 *
 * The bench exists so that a listener can hear "every drop in Full-On" or "every breakdown of seed 7" in a
 * row instead of sitting through the sets around them, and so that a remark about one of them names a bar.
 * Nothing is rendered twice: the excerpts are cut from the one render, sample-exact at the bar lines the
 * tempo map gives, with a ten-millisecond fade at both ends so that a cut in the middle of a kick's tail
 * does not click. Two excerpts may overlap (the outgoing track's outro and the incoming track's intro over
 * the DJ blend), which is why each one carries its own writer.
 */
struct Excerpt {
    std::string path;              ///< the file
    uint64_t start = 0, end = 0;   ///< sample range in the render, end exclusive
    WavWriter wav;                 ///< opened at the first sample, closed after the last
    bool opened = false, closed = false;
};

/** @brief A style name as a file name part: "Dark Forest" -> "Dark-Forest". */
std::string fileToken(const char* name)
{
    std::string s(name);
    for (char& c : s) if (c == ' ' || c == '/' || c == '\\') c = '-';
    return s;
}

/** @brief A path as a rekordbox Location: file://localhost/ and the path with forward slashes, percent-encoded. */
std::string fileUrl(const std::filesystem::path& path)
{
    const std::u8string u8 = std::filesystem::absolute(path).generic_u8string();   // UTF-8 bytes, percent-encoded below
    const std::string raw(u8.begin(), u8.end());
    std::string out = "file://localhost/";
    for (unsigned char c : raw) {
        if (std::isalnum(c) || c == '/' || c == '-' || c == '_' || c == '.' || c == '~' || c == ':') out += static_cast<char>(c);
        else { char h[4]; std::snprintf(h, sizeof(h), "%%%02X", c); out += h; }
    }
    return out;
}

/** @brief Text for an XML attribute. */
std::string xmlEscape(const std::string& in)
{
    std::string out;
    for (char c : in) {
        if (c == '&') out += "&amp;";
        else if (c == '<') out += "&lt;";
        else if (c == '>') out += "&gt;";
        else if (c == '"') out += "&quot;";
        else out += c;
    }
    return out;
}

/**
 * @brief The DJ export (--dj-export, 23.09.2026): every track of the set as a WAV of its own, rendered alone.
 *
 * A DJ wants tracks, not a mix: each file starts with its own intro and ends with its own outro, and the DJ does
 * the blends. So each track is composed alone (Composer::setSoloTrack: no guest over its outro, no outgoing track
 * under its intro) and rendered in an engine of its own at the track's constant tempo -- inside the set the tempo
 * ramps between tracks, which a beat grid cannot show. The limiter's lookahead is cut from the front, so the first
 * downbeat is sample 0 and the grid starts at 0.000 s; ten milliseconds of fade close the last bar.
 *
 * Every section start is a cue marker in the WAV (`cue ` + `labl`, WavWriter::addCue) and, in rekordbox.xml, both
 * a memory cue and -- the form has eight sections -- a hot cue A to H. The key goes to rekordbox as its tonic with
 * "m" for the modes with a minor third over it (all but Phrygian Dominant and Double Harmonic). set.m3u8 lists the
 * files in set order, cues.tsv the markers as a table.
 */
int djExport(const std::string& dir, Composer& composer, const ParamStore& params, int totalBars, int sr, int block,
             const Quality& quality, bool pcm24, uint64_t seed)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(dir, ec);
    struct Done { std::string file, name, style, tonality; double bpm = 0.0, seconds = 0.0; uint64_t bytes = 0; std::vector<std::pair<std::string, double>> cues; };
    std::vector<Done> done;
    FILE* tsv = std::fopen((fs::path(dir) / "cues.tsv").string().c_str(), "w");
    if (tsv != nullptr) std::fprintf(tsv, "file\ttrack\tcue\tbar\tseconds\n");
    for (int t = 0; composer.track(params, t).firstBar < totalBars; ++t) {
        const TrackPlan plan = composer.track(params, t);
        const std::string style = kStyleNames[std::clamp(plan.style, 0, kNumStyles - 1)];
        const bool major = plan.scale == 3 || plan.scale == 4;   // Phrygian Dominant, Double Harmonic: a major third over the tonic
        const std::string tonality = std::string(kKeyNames[plan.key]) + (major ? "" : "m");
        char base[160];
        std::snprintf(base, sizeof(base), "%02d - %s - %s %s - %.1f BPM", t + 1, style.c_str(), kKeyNames[plan.key], kScaleNames[plan.scale], plan.bpm);
        Done d;
        d.file = std::string(base) + ".wav";
        d.name = "Phosphene " + std::to_string(seed) + " - " + std::to_string(t + 1);
        d.style = style;
        d.tonality = tonality;
        d.bpm = plan.bpm;

        auto eng = std::make_unique<Engine>();
        eng->params().copyValuesFrom(params);
        TempoMap tm;
        tm.setConstant(plan.bpm);
        eng->prepare(sr, block, quality);
        eng->setTempoMap(tm);
        composer.setSoloTrack(t);
        const double offset = static_cast<double>(plan.firstBar) * kBeatsPerBar;
        const int endBar = plan.firstBar + plan.bars;
        const uint64_t latency = static_cast<uint64_t>(std::max(0, eng->latencySamples()));
        const double seconds = static_cast<double>(plan.bars) * kBeatsPerBar * 60.0 / plan.bpm;
        const uint64_t frames = static_cast<uint64_t>(std::llround(seconds * sr));
        d.seconds = seconds;

        WavWriter wav;
        const std::string path = (fs::path(dir) / d.file).string();
        // The markers: every section's first bar, named by its place in the form.
        int drops = 0, builds = 0;
        for (int si = 0; si < plan.form.count; ++si) {
            const Section& sec = plan.form.section[si];
            std::string label = kSectionNames[static_cast<int>(sec.type)];
            if (sec.type == SectionType::Drop) label = sec.climax ? "Drop 2 (climax)" : "Drop " + std::to_string(++drops);
            else if (sec.type == SectionType::Build) label = "Build " + std::to_string(++builds);
            else if (sec.type == SectionType::Break) label = "Breakdown";
            const double at = static_cast<double>(sec.startBar) * kBeatsPerBar * 60.0 / plan.bpm;
            wav.addCue(static_cast<uint64_t>(std::llround(at * sr)), label);
            d.cues.emplace_back(label, at);
            if (tsv != nullptr) std::fprintf(tsv, "%s\t%d\t%s\t%d\t%.3f\n", d.file.c_str(), t + 1, label.c_str(), sec.startBar + 1, at);
        }
        wav.setInfo("INAM", d.name);
        wav.setInfo("IART", "Phosphene");
        wav.setInfo("IGNR", "Psytrance / " + style);
        char comment[160];
        std::snprintf(comment, sizeof(comment), "%s %s, %.1f BPM, seed %llu, track %d", kKeyNames[plan.key], kScaleNames[plan.scale], plan.bpm,
                      static_cast<unsigned long long>(seed), t + 1);
        wav.setInfo("ICMT", comment);
#if defined(PHOS_VERSION)
        wav.setInfo("ISFT", std::string("Phosphene phos_render ") + PHOS_VERSION);
#endif
        if (!wav.open(path.c_str(), sr, 2, pcm24 ? WavFormat::Pcm24 : WavFormat::Float32)) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }

        std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
        std::vector<NoteEvent> notes;
        std::vector<ControlEvent> controls;
        int nextBar = plan.firstBar;
        uint64_t rendered = 0, written = 0;
        const uint64_t fade = static_cast<uint64_t>(sr / 100);
        while (written < frames) {
            // Keep eight bars composed ahead of the engine, the conductor's horizon; beats moved to the file's own 0.
            while (nextBar < endBar && static_cast<double>(nextBar) * kBeatsPerBar - offset < eng->beatPosition() + 8.0 * kBeatsPerBar) {
                notes.clear();
                controls.clear();
                composer.composeBars(params, nextBar, 1, notes, &controls);
                for (ControlEvent c : controls) { c.beat -= offset; eng->pushControl(c); }
                for (NoteEvent n : notes) { n.beat -= offset; eng->pushEvent(n); }
                ++nextBar;
            }
            const int n = block;
            eng->process(L.data(), R.data(), n);
            // Drop the limiter's lookahead at the front, so the first downbeat is the file's first sample.
            int from = 0;
            if (rendered < latency) from = static_cast<int>(std::min<uint64_t>(latency - rendered, static_cast<uint64_t>(n)));
            rendered += static_cast<uint64_t>(n);
            const int take = static_cast<int>(std::min<uint64_t>(static_cast<uint64_t>(n - from), frames - written));
            for (int i = 0; i < take; ++i) {
                const uint64_t k = written + static_cast<uint64_t>(i);
                if (k + fade > frames) {
                    const float g = static_cast<float>(frames - k) / static_cast<float>(fade);
                    L[static_cast<size_t>(from + i)] *= g;
                    R[static_cast<size_t>(from + i)] *= g;
                }
            }
            if (take > 0) wav.write(L.data() + from, R.data() + from, take);
            written += static_cast<uint64_t>(std::max(0, take));
        }
        wav.close();
        composer.setSoloTrack(-1);
        d.bytes = static_cast<uint64_t>(fs::file_size(path, ec));
        std::printf("dj export: %s  (%.1f min, %zu cues)\n", d.file.c_str(), seconds / 60.0, d.cues.size());
        std::fflush(stdout);
        done.push_back(std::move(d));
    }
    if (tsv != nullptr) std::fclose(tsv);

    // set.m3u8: the files in set order.
    if (FILE* m3u = std::fopen((fs::path(dir) / "set.m3u8").string().c_str(), "w")) {
        std::fprintf(m3u, "#EXTM3U\n");
        for (const Done& d : done) std::fprintf(m3u, "#EXTINF:%d,Phosphene - %s\n%s\n", static_cast<int>(std::lround(d.seconds)), d.name.c_str(), d.file.c_str());
        std::fclose(m3u);
    }
    // rekordbox.xml: File > Import > rekordbox xml. The beat grid starts at 0, the eight sections are hot cues A-H
    // and memory cues.
    if (FILE* x = std::fopen((fs::path(dir) / "rekordbox.xml").string().c_str(), "w")) {
        std::fprintf(x, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<DJ_PLAYLISTS Version=\"1.0.0\">\n");
#if defined(PHOS_VERSION)
        std::fprintf(x, "  <PRODUCT Name=\"Phosphene\" Version=\"%s\" Company=\"Rene Weller\"/>\n", PHOS_VERSION);
#endif
        std::fprintf(x, "  <COLLECTION Entries=\"%zu\">\n", done.size());
        for (size_t i = 0; i < done.size(); ++i) {
            const Done& d = done[i];
            std::fprintf(x, "    <TRACK TrackID=\"%zu\" Name=\"%s\" Artist=\"Phosphene\" Album=\"Set %llu\" Genre=\"%s\" Kind=\"WAV File\" Size=\"%llu\" TotalTime=\"%d\""
                            " TrackNumber=\"%zu\" AverageBpm=\"%.2f\" SampleRate=\"%d\" Tonality=\"%s\" Location=\"%s\">\n",
                         i + 1, xmlEscape(d.name).c_str(), static_cast<unsigned long long>(seed), xmlEscape("Psytrance / " + d.style).c_str(),
                         static_cast<unsigned long long>(d.bytes), static_cast<int>(std::lround(d.seconds)), i + 1, d.bpm, sr, d.tonality.c_str(),
                         xmlEscape(fileUrl(fs::path(dir) / d.file)).c_str());
            std::fprintf(x, "      <TEMPO Inizio=\"0.000\" Bpm=\"%.2f\" Metro=\"4/4\" Battito=\"1\"/>\n", d.bpm);
            for (size_t c = 0; c < d.cues.size(); ++c) {
                std::fprintf(x, "      <POSITION_MARK Name=\"%s\" Type=\"0\" Start=\"%.3f\" Num=\"-1\"/>\n", xmlEscape(d.cues[c].first).c_str(), d.cues[c].second);
                if (c < 8) std::fprintf(x, "      <POSITION_MARK Name=\"%s\" Type=\"0\" Start=\"%.3f\" Num=\"%zu\"/>\n", xmlEscape(d.cues[c].first).c_str(), d.cues[c].second, c);
            }
            std::fprintf(x, "    </TRACK>\n");
        }
        std::fprintf(x, "  </COLLECTION>\n  <PLAYLISTS>\n    <NODE Type=\"0\" Name=\"ROOT\" Count=\"1\">\n");
        std::fprintf(x, "      <NODE Name=\"Phosphene set %llu\" Type=\"1\" KeyType=\"0\" Entries=\"%zu\">\n", static_cast<unsigned long long>(seed), done.size());
        for (size_t i = 0; i < done.size(); ++i) std::fprintf(x, "        <TRACK Key=\"%zu\"/>\n", i + 1);
        std::fprintf(x, "      </NODE>\n    </NODE>\n  </PLAYLISTS>\n</DJ_PLAYLISTS>\n");
        std::fclose(x);
    }
    std::printf("dj export: %zu tracks, rekordbox.xml, set.m3u8 and cues.tsv in %s\n", done.size(), dir.c_str());
    return 0;
}

/** @brief Reads a whole text file into @p out; false if it cannot be opened. */
bool readFile(const char* path, std::string& out)
{
    FILE* f = std::fopen(path, "rb");
    if (f == nullptr) return false;
    char buf[4096];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
    std::fclose(f);
    return true;
}

} // namespace

int main(int argc, char** argv)
{
    int bars = 16;
    double seconds = -1.0;
    int sr = 48000, block = 256;
    uint64_t seed = 1;
    std::string out, midi, solo, setFile, saveSet, barLog, excerpts, stemsDir, audibilityPath, djDir, mixLog;
    int excerptBars = 16;
    bool report = false, bench = false, pcm24 = false, listTracks = false, listSections = false, scoreOnly = false;
    Quality quality = Quality::desktop();
    double rampBeat = -1.0, rampBpm = 0.0;
    std::vector<std::string> sets;
    std::vector<std::pair<bool, std::string>> unitOps;   // (lock?, "unit:index")

    // A development tool (round "speed", 20.09.2026): the composer's probes run in parallel, and from the
    // probe cache when PHOS_PROBE_CACHE names a directory (phos/Probe.h). The plans, and with them the
    // render, are bit for bit what the serial order makes -- PHOS_PROBE_THREADS=1 is that order.
    probe::configureFromEnvironment();

    // Before the engine: the first Engine::prepare() anywhere in the process is the one that loads
    // the wavetable library, so the search path has to be in place before there is an engine at all.
    installDataSearchPath();

    auto engine = std::make_unique<Engine>();
    ParamStore& params = engine->params();

    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc) { std::fprintf(stderr, "%s needs a value\n", a.c_str()); std::exit(2); }
            return argv[++i];
        };
        if (a == "--bars") bars = std::atoi(next());
        else if (a == "--seconds") seconds = std::atof(next());
        else if (a == "--minutes") seconds = 60.0 * std::atof(next());
        else if (a == "--tracks") listTracks = true;
        else if (a == "--sr") sr = std::atoi(next());
        else if (a == "--block") block = std::atoi(next());
        else if (a == "--quality") {
            const char* level = next();
            if (!Quality::fromName(level, quality)) { std::fprintf(stderr, "--quality wants desktop or quest\n"); return 2; }
        }
        else if (a == "--seed") seed = std::strtoull(next(), nullptr, 10);
        else if (a == "--set") sets.push_back(next());
        else if (a == "--preset") {
            std::string text;
            const char* path = next();
            if (!readFile(path, text)) { std::fprintf(stderr, "cannot read %s\n", path); return 2; }
            sets.push_back(text);
        }
        else if (a == "--tempo-ramp") {
            const std::string v = next();
            const size_t c = v.find(':');
            if (c == std::string::npos) { std::fprintf(stderr, "--tempo-ramp wants BEAT:BPM\n"); return 2; }
            rampBeat = std::atof(v.substr(0, c).c_str());
            rampBpm = std::atof(v.substr(c + 1).c_str());
        }
        else if (a == "--set-file") setFile = next();
        else if (a == "--save-set") saveSet = next();
        else if (a == "--lock") unitOps.emplace_back(true, next());
        else if (a == "--reroll") unitOps.emplace_back(false, next());
        else if (a == "--sections") listSections = true;
        else if (a == "--solo") solo = next();
        else if (a == "--out") out = next();
        else if (a == "--pcm24") pcm24 = true;
        else if (a == "--midi") midi = next();
        else if (a == "--report") report = true;
        else if (a == "--bar-log") barLog = next();
        else if (a == "--mix-log") mixLog = next();
        else if (a == "--score-only") scoreOnly = true;
        else if (a == "--excerpts") excerpts = next();
        else if (a == "--excerpt-bars") excerptBars = std::max(1, std::atoi(next()));
        else if (a == "--stems") stemsDir = next();
        else if (a == "--audibility") audibilityPath = next();
        else if (a == "--dj-export") djDir = next();
        else if (a == "--preferences") {
            std::string text;
            const char* path = next();
            auto prefs = std::make_shared<Preferences>();
            std::string err;
            if (!readFile(path, text) || !prefs->parse(text, &err)) { std::fprintf(stderr, "cannot read preferences %s %s\n", path, err.c_str()); return 2; }
            setPreferences(prefs);
            std::fprintf(stderr, "preferences: %zu weights from %s\n", prefs->size(), path);
        }
        else if (a == "--learn") {
            const char* in = next();
            const char* outPath = next();
            std::string text;
            if (!readFile(in, text)) { std::fprintf(stderr, "cannot read %s\n", in); return 2; }
            std::vector<RatingEntry> ratings;
            size_t start = 0;
            while (start < text.size()) {
                size_t end = text.find('\n', start);
                if (end == std::string::npos) end = text.size();
                RatingEntry r;
                if (parseRating(text.substr(start, end - start), r)) ratings.push_back(r);
                start = end + 1;
            }
            const Preferences prefs = fitPreferences(ratings);
            FILE* f = std::fopen(outPath, "w");
            if (f == nullptr) { std::fprintf(stderr, "cannot write %s\n", outPath); return 1; }
            std::fputs(prefs.toText().c_str(), f);
            std::fclose(f);
            std::printf("%zu verdicts, %zu weights written to %s\n%s", ratings.size(), prefs.size(), outPath, prefs.toText().c_str());
            return 0;
        }
        else if (a == "--bench") bench = true;
        else if (a == "--list") { printList(params); return 0; }
        else if (a == "--version") { printVersion(); return 0; }
        else { std::fprintf(stderr, "unknown option %s\n", a.c_str()); return 2; }
    }

    for (const std::string& s : sets) {
        std::string err;
        if (!params.parseText(s, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
    }
    const int mb = params.base(Module::Mix);
    std::vector<int> soloMutes;   // the mutes a --solo sets, to set again after a set file (below)
    int soloWhich = -1;
    if (!solo.empty()) {
        // One entry per part, in the order of the Part enum (19.09.2026: the polyphonic voices in their groups).
        static const char* const kSoloNames[] = { "kick", "bass", "perc", "acid", "lead", "counter", "arp", "stab", "pad", "drone",
                                                  "sfx", "texture", "vocal" };
        static const int kSoloMutes[] = { mix::KickMute, mix::BassMute, mix::PercMute, mix::AcidMute,
                                          mix::LeadMute, mix::CounterMute, mix::ArpMute, mix::StabMute, mix::PadMute, mix::DroneMute,
                                          mix::SfxMute, mix::TextureMute, mix::VocalMute };
        static_assert(sizeof(kSoloNames) / sizeof(kSoloNames[0]) == kNumParts, "one solo name per part");
        static_assert(sizeof(kSoloMutes) / sizeof(kSoloMutes[0]) == kNumParts, "one mute per part");
        int which = -1;
        for (int k = 0; k < kNumParts; ++k) if (solo == kSoloNames[k]) which = k;
        if (which < 0) { std::fprintf(stderr, "--solo wants kick, bass, perc, acid, lead, counter, arp, stab, pad, drone, sfx, texture or vocal\n"); return 2; }
        for (int k = 0; k < kNumParts; ++k) params.set(mb + kSoloMutes[k], k == which ? 0.0f : 1.0f);
        soloMutes.assign(std::begin(kSoloMutes), std::end(kSoloMutes));
        soloWhich = which;
    }

    const int cb = params.base(Module::Compose);
    Composer composer(seed);
    // A set file carries the seed, the style, the arc, the knobs and the curation (locks, rerolls).
    // It is read before the command line's own --set assignments so that those still win. Reading it
    // puts every knob it does not name back to its default (SetFile.h, since 19.09.2026), so the solo
    // is applied again after it.
    if (!setFile.empty()) {
        std::string err;
        if (!readSetFile(setFile.c_str(), composer, params, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
        for (const std::string& s : sets) if (!params.parseText(s, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
        for (size_t k = 0; k < soloMutes.size(); ++k) params.set(mb + soloMutes[k], static_cast<int>(k) == soloWhich ? 0.0f : 1.0f);
    }
    for (const auto& op : unitOps) {
        const size_t c = op.second.find(':');
        if (c == std::string::npos) { std::fprintf(stderr, "--lock/--reroll want UNIT:INDEX\n"); return 2; }
        int unit = -1;
        for (int u = 0; u < kNumLockUnits; ++u) if (op.second.compare(0, c, kLockUnitNames[u]) == 0) unit = u;
        if (unit < 0) { std::fprintf(stderr, "unknown unit %s (set, track, section, lane)\n", op.second.substr(0, c).c_str()); return 2; }
        const int index = std::atoi(op.second.c_str() + c + 1);
        if (op.first) composer.setLock(static_cast<LockUnit>(unit), index, true);
        else composer.reroll(static_cast<LockUnit>(unit), index);
    }
    if (!saveSet.empty() && !writeSetFile(saveSet.c_str(), composer, params)) {
        std::fprintf(stderr, "cannot write %s\n", saveSet.c_str());
        return 1;
    }
    // The composer's tempo map covers the render: per track, ramped between tracks. For a length in
    // seconds, plan generously (at the fastest tempo the range allows) and cut by time afterwards.
    // With Style Tempo on, the tracks run at the style profile's centre, not at compose.bpm; planning
    // at the slower of the two would leave the end of a long set without its tempo points.
    const StyleProfile& style = styleProfile(styleOf(params));
    const bool styleTempo = params.getBool(cb + compose::StyleTempo);
    const double planBpm = (styleTempo ? style.bpmCentre + style.bpmRange : params.get(cb + compose::Bpm) + params.get(cb + compose::TempoRange));
    const int planBars = seconds > 0.0 ? static_cast<int>(seconds * planBpm / 240.0) + 64 : bars;
    TempoMap tempo = composer.tempoMap(params, planBars);
    if (rampBeat > 0.0) {
        tempo.setConstant(params.get(cb + compose::Bpm));
        tempo.add(0.0, params.get(cb + compose::Bpm), true);
        tempo.add(rampBeat, rampBpm, false);
    }

    engine->prepare(sr, block, quality);
    engine->setTempoMap(tempo);
    Conductor conductor(*engine, composer);

    const double totalBeats = seconds > 0.0 ? tempo.beatAt(seconds) : static_cast<double>(bars) * kBeatsPerBar;
    const int totalBars = static_cast<int>(std::ceil(totalBeats / kBeatsPerBar));
    if (!djDir.empty()) return djExport(djDir, composer, params, totalBars, sr, block, quality, pcm24, seed);
    if (listTracks) {
        for (int t = 0; composer.track(params, t).firstBar < totalBars; ++t) {
            const TrackPlan p = composer.track(params, t);
            std::printf("track %2d  bar %5d  %3d bars  %5.1f BPM  %-2s %-17s %-11s  bass %-7s/%-7s gate %.2f  kick %s%s  gain %+.1f dB\n",
                        t + 1, p.firstBar, p.bars, p.bpm, kKeyNames[p.key], kScaleNames[p.scale], kStyleNames[std::clamp(p.style, 0, kNumStyles - 1)],
                        kBassPatternNames[p.primaryPattern], kBassPatternNames[p.secondaryPattern], static_cast<double>(p.gate),
                        (p.kickEngine < 0 ? params.getInt(params.base(Module::Kick) + kick::Engine) : p.kickEngine) == 1 ? "resonant" : "sweep",
                        p.kickClip == 1 ? " hard" : "", static_cast<double>(p.gainDb));
            // The presence match (19.09.2026, round "polish"): the drops' estimate against the reference median,
            // the gain it put on the lines, and its own prediction of where that lands (presenceAfterDb; printed
            // since 20.09.2026, round "climax-polish", to see a capped correction without a full render).
            std::printf("          presence %+.2f dB against the reference median, lines %+.2f dB, predicted after %+.2f dB\n",
                        p.presenceDb, static_cast<double>(p.presenceGainDb), p.presenceAfterDb);
            // 23.09.2026: the audibility match -- each line's partial loudness in the drops and what was lifted.
            {
                static const char* const kLine[] = { "acid", "lead", "counter", "arp", "stab", "pad", "drone" };
                static_assert(sizeof(kLine) / sizeof(kLine[0]) == kMelodyParts, "one name per melodic part");
                std::printf("          heard in the drops (partial loudness):");
                for (int k = 0; k < kMelodyParts; ++k) {
                    if (p.audibleInMix[k] <= 0.0) continue;
                    std::printf(" %s %.1f", kLine[k], p.audibleInMix[k]);
                    if (p.audibilityLiftDb[k] > 0.0f) std::printf(" (+%.1f dB)", static_cast<double>(p.audibilityLiftDb[k]));
                }
                std::printf("\n");
            }
            // 26.09.2026: the sound presets the composer chose, one per synth (SoundPresets.h, Composer::presetOf).
            {
                static const char* const kSynth[kSoundSynths] = { "kick", "bass", "acid", "lead", "counter", "arp", "stab", "pad", "drone" };
                std::printf("          presets:");
                for (int k = 0; k < kSoundSynths; ++k) {
                    const SoundPreset* sp = Composer::presetOf(params, p, k);
                    std::printf(" %s \"%s\"%s", kSynth[k], sp != nullptr ? (sp->group + ": " + sp->name).c_str() : "knobs", k + 1 < kSoundSynths ? ";" : "");
                }
                std::printf("\n");
            }
            std::printf("          kick recipe");
            for (int m = 0; m < kNumKickMacros; ++m) std::printf(" %s %+.2f", kKickMacroNames[m], static_cast<double>(p.kickMacro[m]));
            std::printf("\n          bass recipe");
            for (int m = 0; m < kNumBassMacros; ++m) std::printf(" %s %+.2f", kBassMacroNames[m], static_cast<double>(p.bassMacro[m]));
            static const char* const kHatModes[3] = { "closed offbeat", "open offbeat + closed 16ths", "closed offbeat + shaker" };
            std::printf("\n          perc: %s, clap backbeat %s, up to %d layers:", kHatModes[p.perc.hatMode], p.perc.clapBackbeat ? "on" : "off", p.perc.layers);
            for (int i = 0; i < p.perc.layers; ++i)
                std::printf(" %s", kPercRoleNames[params.getInt(params.base(Module::Perc, p.perc.layerOrder[i]) + perc::Role)]);
            const MelodyPlan& m = p.melody;
            static const char* const kRoman[7] = { "i", "ii", "iii", "iv", "v", "vi", "vii" };
            static const char* const kArpStyles[] = { "corpus", "up", "down", "up-down", "Euclid", "polymeter" };
            static_assert(sizeof(kArpStyles) / sizeof(kArpStyles[0]) == kNumArpStyles, "one name per ArpStyle");
            std::printf("\n          melody: chords");
            for (int c = 0; c < 4; ++c) std::printf(" %s%s", kRoman[m.chordDegree[c]], kChordTypeNames[std::clamp(m.chordType[c], 0, kNumChordTypes - 1)]);
            std::printf(" (%d bars each, %s)", m.chordBars, m.progression == 1 ? "loop" : "pendulum");
            if (m.secondHalf) {
                std::printf("; after the main breakdown");
                for (int c = 0; c < 4; ++c) std::printf(" %s%s", kRoman[m.chordDegree2[c]], kChordTypeNames[std::clamp(m.chordType2[c], 0, kNumChordTypes - 1)]);
                std::printf(" (%s)", m.progression2 == 1 ? "loop" : "pendulum");
            }
            std::printf("; main breakdown");
            if (m.breakHolds) std::printf(" holds %s%s", kRoman[m.breakDegree[0]], kChordTypeNames[std::clamp(m.breakType[0], 0, kNumChordTypes - 1)]);
            else for (int c = 0; c < 4; ++c) std::printf(" %s%s", kRoman[m.breakDegree[c]], kChordTypeNames[std::clamp(m.breakType[c], 0, kNumChordTypes - 1)]);
            auto on = [&](MelodyPart part) { return m.present[mpIndex(part)] ? "yes" : "no"; };
            std::printf(" (%d bars each); acid %s (%d steps%s), lead %s, counter %s, arp %s (%s), stab %s, pad %s, drone %s\n", m.breakChordBars,
                        on(MelodyPart::Acid), m.acidSteps, m.acidSquelch == 1 ? ", squelch" : "", on(MelodyPart::Lead), on(MelodyPart::Counter),
                        on(MelodyPart::Arp), kArpStyles[m.arpStyle], on(MelodyPart::Stab), on(MelodyPart::Pad), on(MelodyPart::Drone));
            // 23.09.2026, round "Counter": the register the lead window stands in and how the counter answers.
            static const char* const kCounterModes[] = { "echo", "answer", "timbral", "hocket" };
            static_assert(sizeof(kCounterModes) / sizeof(kCounterModes[0]) == kNumCounterModes, "one name per CounterMode");
            std::printf("          register: lead %s..%s, counter %s..%s; counter mode %s\n",
                        kKeyNames[m.leadWindowLo % 12], kKeyNames[leadWindowHi(m) % 12], kKeyNames[counterWindowLo(m) % 12], kKeyNames[counterWindowHi(m) % 12],
                        kCounterModes[std::clamp(m.counterMode, 0, kNumCounterModes - 1)]);
            // 22.09.2026, round "Lead": the two phrases' design -- archetype, cell, operator and shift per bar.
            for (int w = 0; w < 2 && m.present[mpIndex(MelodyPart::Lead)]; ++w) {
                std::printf("          lead %d: %s, cell ", w + 1, kLeadArchetypeNames[std::clamp(m.leadArchetype[w], 0, kNumLeadArchetypes - 1)]);
                for (int s = 0; s < 16; ++s) std::printf("%c", ((m.leadCell[w] >> s) & 1u) ? 'x' : '.');
                std::printf(" (%s, band %d), bars", m.leadQuotesSet[w] ? (t == 0 ? "the set's motif" : "recalls the set's motif") : (m.leadCellFromCorpus[w] ? "corpus" : "family"),
                            m.leadDensityBand);
                for (int b = 0; b < 8; ++b) std::printf(" %s%+d", kCellOpNames[std::clamp<int>(m.leadOps[w][b], 0, kNumCellOps - 1)], static_cast<int>(m.leadShift[w][b]));
                std::printf("\n");
            }
            // 19.09.2026: each polyphonic voice's sound in this track (Composer.h, VoiceRecipe); -1 is the knob.
            static const char* const kOscNames[] = { "supersaw", "va", "fm", "wavetable" };
            static_assert(sizeof(kOscNames) / sizeof(kOscNames[0]) == static_cast<int>(PolyOsc::Count), "one name per PolyOsc");
            for (int v = 0; v < kPolyInstances; ++v) {
                const VoiceRecipe& rc = p.voice[v];
                const int tableId = params.base(static_cast<PolyInstance>(v)) + poly::Table;
                // 22.09.2026: the second oscillator and its interval (Composer.h, VoiceRecipe).
                static const char* const kOsc2Names[] = { "off", "supersaw", "va", "fm", "wavetable" };
                static const char* const kIntervalNames[] = { "-2oct", "-1oct", "-5th", "unison", "+5th", "+1oct" };
                std::printf("          %-8s osc %-9s table %-20s filter %d  osc2 %-9s %-6s", kPolyInstanceNames[v],
                            rc.osc < 0 ? "knob" : kOscNames[rc.osc],
                            rc.table < 0 ? "knob" : params.desc(tableId).choices[rc.table], rc.filter,
                            !rc.hasOsc2 || rc.osc2 < 0 ? "knob" : kOsc2Names[rc.osc2],
                            !rc.hasOsc2 || rc.osc2 == 0 ? "" : kIntervalNames[rc.osc2Semis]);
                for (int k = 0; k < kNumVoiceMacros; ++k) std::printf(" %s %+.2f", kVoiceMacroNames[k], static_cast<double>(rc.macro[k]));
                std::printf("\n");
            }
            static const char* const kTemplateNames[] = { "Full-On", "Progressive", "Goa", "Dark Forest" };
            static_assert(sizeof(kTemplateNames) / sizeof(kTemplateNames[0]) == kNumBodies, "one name per form template");
            std::printf("          form (%s template, hand-over at bar %d, arc %.2f..%.2f):", kTemplateNames[std::clamp(p.form.body, 0, kNumBodies - 1)],
                        handoverBar(p), static_cast<double>(p.arcIn), static_cast<double>(p.arcOut));
            for (int s = 0; s < p.form.count; ++s)
                std::printf(" %s%d@%d(E%.2f)", kSectionNames[static_cast<int>(p.form.section[s].type)], p.form.section[s].bars,
                            p.form.section[s].startBar, static_cast<double>(p.form.section[s].energy));
            std::printf("\n          effects:");
            for (const SfxEvent& s : p.form.sfx) std::printf(" %s@%g", kSfxTypeNames[s.type], s.beat / kBeatsPerBar);
            std::printf("\n          part gains (acid, lead, counter, arp, stab, pad, drone)");
            for (int k = 0; k < kMelodyParts; ++k) std::printf(" %+.1f", static_cast<double>(p.partGainDb[k]));
            std::printf(" dB, mix %.1f LUFS before the master offset %+.1f dB\n", p.mixLoudness, static_cast<double>(p.masterGainDb));
        }
    }
    if (listSections) {
        for (const SectionMark& s : composer.sections(params, totalBars))
            std::printf("bar %5d  track %2d  %-6s  energy %.2f\n", static_cast<int>(s.beat / kBeatsPerBar) + 1, s.track + 1,
                        kSectionNames[static_cast<int>(s.type)], static_cast<double>(s.energy));
    }
    const uint64_t totalSamples = static_cast<uint64_t>(std::llround(tempo.secondsAt(totalBeats) * sr));

    // The listening bench (Excerpt above): one window per section of every track the render reaches. A
    // section's excerpt is its first excerptBars bars -- the drop's hit, the breakdown's cut, the groove's
    // entry are all at the start. A buildup is the exception: what a listener judges there is its *end*,
    // the roll, the pre-drop break and the drop it lands in, so its excerpt is its last bars plus the first
    // four of the section after it. The file name says everything the index says, so a folder listing is
    // already the bench: s<seed>_t<track>_<style>_<section index>-<section>_b<first bar>.wav.
    std::vector<std::unique_ptr<Excerpt>> excerptList;
    if (!excerpts.empty() && !bench && !scoreOnly) {
        std::error_code ec;
        std::filesystem::create_directories(excerpts, ec);
        const std::string indexPath = excerpts + "/index.tsv";
        FILE* idx = std::fopen(indexPath.c_str(), "w");
        if (idx == nullptr) { std::fprintf(stderr, "cannot write %s\n", indexPath.c_str()); return 1; }
        // The last two columns are the listener's (23.09.2026, phos/Rating.h): "good", "bad" or empty, and a
        // note. Tools/ratings.py reads them back together with the plugin's ratings.tsv.
        std::fprintf(idx, "file\tseed\ttrack\tstyle\tkey\tscale\tbpm\tsection\tindex\tfirst_bar\tbars\tenergy\tclimax\tfeatures\trating\tnote\n");
        for (int t = 0; composer.track(params, t).firstBar < totalBars; ++t) {
            const TrackPlan p = composer.track(params, t);
            const std::string styleName = fileToken(kStyleNames[std::clamp(p.style, 0, kNumStyles - 1)]);
            for (int s = 0; s < p.form.count; ++s) {
                const Section& sec = p.form.section[s];
                int from = p.firstBar + sec.startBar;
                int len = std::min(excerptBars, sec.bars);
                if (sec.type == SectionType::Build) {
                    const int tail = std::min(sec.bars, std::max(4, excerptBars - 4));
                    from = p.firstBar + sec.startBar + sec.bars - tail;
                    len = tail + (s + 1 < p.form.count ? std::min(4, p.form.section[s + 1].bars) : 0);
                }
                const int to = std::min(from + len, totalBars);
                if (to <= from) continue;
                char name[256];
                std::snprintf(name, sizeof(name), "s%llu_t%02d_%s_%d-%s_b%05d.wav", static_cast<unsigned long long>(seed), t + 1,
                              styleName.c_str(), s + 1, kSectionNames[static_cast<int>(sec.type)], from + 1);
                auto e = std::make_unique<Excerpt>();
                e->path = excerpts + "/" + name;
                e->start = static_cast<uint64_t>(std::llround(tempo.secondsAt(static_cast<double>(from) * kBeatsPerBar) * sr));
                e->end = static_cast<uint64_t>(std::llround(tempo.secondsAt(static_cast<double>(to) * kBeatsPerBar) * sr));
                std::fprintf(idx, "%s\t%llu\t%d\t%s\t%s\t%s\t%.1f\t%s\t%d\t%d\t%d\t%.2f\t%d\t%s\t\t\n", name, static_cast<unsigned long long>(seed), t + 1,
                             kStyleNames[std::clamp(p.style, 0, kNumStyles - 1)], kKeyNames[p.key], kScaleNames[p.scale], p.bpm,
                             kSectionNames[static_cast<int>(sec.type)], s + 1, from + 1, to - from, static_cast<double>(sec.energy), sec.climax ? 1 : 0,
                             decisionFeatures(p, from - p.firstBar).c_str());   // what a verdict on it teaches (Preferences.h)
                excerptList.push_back(std::move(e));
            }
        }
        std::fclose(idx);
    }
    const uint64_t excerptFade = static_cast<uint64_t>(sr / 100);   // 10 ms
    std::vector<float> exL(static_cast<size_t>(block)), exR(static_cast<size_t>(block));

    // The stems (Engine.h, StemTap): one writer per part and one for the returns. The master's limiter delays
    // the mix by its lookahead, the stems are taken before it, so each stem file starts with that many zeros and
    // stops at the mix's length -- then --out and the stems line up in any editor. What remains is the clipper's
    // oversampling: its half-band filters are IIR allpass sections with a group delay that depends on frequency
    // (Halfband.h), a few samples at the bottom of the spectrum -- measured 3 samples, 0.06 ms at 48 kHz, on seed 1
    // by cross-correlating the stems' sum with the mix. No constant describes it, so none is added.
    std::vector<std::vector<float>> stemBufL, stemBufR;
    std::vector<std::unique_ptr<WavWriter>> stemWav;
    StemTap tap;
    uint64_t stemLatency = 0, stemWritten = 0;
    const bool wantTap = (!stemsDir.empty() || !audibilityPath.empty()) && !bench && !scoreOnly;
    if (wantTap) {
        for (int s = 0; s < kNumStems; ++s) {
            stemBufL.emplace_back(static_cast<size_t>(block), 0.0f);
            stemBufR.emplace_back(static_cast<size_t>(block), 0.0f);
            tap.L[s] = stemBufL.back().data();
            tap.R[s] = stemBufR.back().data();
        }
        engine->setStemTap(&tap);
    }
    // The audibility report (Audibility.h, 23.09.2026): the stems of every block into a partial-loudness meter,
    // one reading per track and section -- the owner track's section, since over a DJ blend two tracks sound.
    std::unique_ptr<AudibilityMeter> audMeter;
    FILE* audFile = nullptr;
    int audTrack = -1, audSection = -1, audFirstBar = 0;
    auto flushAudibility = [&](int lastBar) {
        if (audMeter == nullptr || audTrack < 0) return;
        const TrackPlan p = composer.track(params, audTrack);
        const Section& sec = p.form.section[std::clamp(audSection, 0, p.form.count - 1)];
        std::printf("audibility track %2d %-6s bars %5d..%5d:", audTrack + 1, kSectionNames[static_cast<int>(sec.type)], audFirstBar + 1, lastBar + 1);
        for (int s = 0; s < kNumParts; ++s) {
            const AudibilityReading a = audMeter->read(s);
            if (a.frames == 0 || a.alone < 0.05) continue;
            std::printf(" %s %.2f/%.1f", kStemNames[s], a.ratio, a.inMix);   // share heard / partial loudness in the mix
            std::fprintf(audFile, "%d\t%s\t%d\t%s\t%d\t%d\t%s\t%.3f\t%.3f\t%.3f\t%d\n", audTrack + 1, kStyleNames[std::clamp(p.style, 0, kNumStyles - 1)], audSection + 1,
                         kSectionNames[static_cast<int>(sec.type)], audFirstBar + 1, lastBar + 1, kStemNames[s], a.alone, a.inMix, a.ratio, a.frames);
        }
        std::printf("\n");
        audMeter->reset();
    };
    if (!audibilityPath.empty() && wantTap) {
        audFile = std::fopen(audibilityPath.c_str(), "w");
        if (audFile == nullptr) { std::fprintf(stderr, "cannot write %s\n", audibilityPath.c_str()); return 1; }
        std::fprintf(audFile, "track\tstyle\tsection_index\tsection\tfirst_bar\tlast_bar\tpart\talone\tin_mix\tratio\tframes\n");
        audMeter = std::make_unique<AudibilityMeter>(kNumStems, static_cast<double>(sr));
    }
    if (!stemsDir.empty() && wantTap) {
        std::error_code ec;
        std::filesystem::create_directories(stemsDir, ec);
        stemLatency = static_cast<uint64_t>(std::max(0, engine->latencySamples()));
        std::vector<float> zeros(4096, 0.0f);
        for (int s = 0; s < kNumStems; ++s) {
            char name[64];
            std::snprintf(name, sizeof(name), "%02d_%s.wav", s + 1, kStemNames[s]);
            auto w = std::make_unique<WavWriter>();
            const std::string path = stemsDir + "/" + name;
            if (!w->open(path.c_str(), sr, 2, pcm24 ? WavFormat::Pcm24 : WavFormat::Float32)) { std::fprintf(stderr, "cannot write %s\n", path.c_str()); return 1; }
            for (uint64_t z = 0; z < std::min(stemLatency, totalSamples); z += zeros.size())
                w->write(zeros.data(), zeros.data(), static_cast<int>(std::min<uint64_t>(zeros.size(), std::min(stemLatency, totalSamples) - z)));
            stemWav.push_back(std::move(w));
        }
        stemWritten = std::min(stemLatency, totalSamples);
    }

    WavWriter wav;
    if (!out.empty() && !bench) {
        if (!wav.open(out.c_str(), sr, 2, pcm24 ? WavFormat::Pcm24 : WavFormat::Float32)) {
            std::fprintf(stderr, "cannot write %s\n", out.c_str());
            return 1;
        }
    }
    LoudnessMeter meter;
    meter.prepare(sr);

    std::vector<float> L(static_cast<size_t>(block)), R(static_cast<size_t>(block));
    std::vector<NoteEvent> recorded;
    // Output power per bar for --bar-log: the sum of squares over every sample whose block starts in it.
    std::vector<double> barPower(static_cast<size_t>(totalBars) + 1, 0.0);
    std::vector<uint64_t> barSamples(static_cast<size_t>(totalBars) + 1, 0);
    const bool record = !midi.empty() || !barLog.empty() || !mixLog.empty();
    // --mix-log: the master's gain reduction at the end of every block.
    struct GrPoint { uint64_t at; float limiter, comp; };
    std::vector<GrPoint> grLog;
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t renderedSamples = 0;
    double peak = 0.0;
    // The loudness of each track on its own, so that a set can be checked for jumps between tracks.
    LoudnessMeter trackMeter;
    trackMeter.prepare(sr);
    int meteredTrack = 0;
    if (scoreOnly) {
        composer.composeBars(params, 0, totalBars, recorded);
        renderedSamples = totalSamples;
    }
    while (renderedSamples < totalSamples) {
        const int n = static_cast<int>(std::min<uint64_t>(static_cast<uint64_t>(block), totalSamples - renderedSamples));
        conductor.pump(params, 8.0 * kBeatsPerBar, record ? &recorded : nullptr);
        const int blockBar = std::clamp(static_cast<int>(engine->beatPosition() / kBeatsPerBar), 0, totalBars);
        engine->process(L.data(), R.data(), n);
        if (!barLog.empty()) {
            double sq = 0.0;
            for (int i = 0; i < n; ++i) sq += 0.5 * (static_cast<double>(L[static_cast<size_t>(i)]) * L[static_cast<size_t>(i)] + static_cast<double>(R[static_cast<size_t>(i)]) * R[static_cast<size_t>(i)]);
            barPower[static_cast<size_t>(blockBar)] += sq;
            barSamples[static_cast<size_t>(blockBar)] += static_cast<uint64_t>(n);
        }
        if (!bench) {
            meter.process(L.data(), R.data(), n);
            const int at = composer.trackOfBar(params, static_cast<int>(engine->beatPosition() / kBeatsPerBar));
            if (at != meteredTrack) {
                if (listTracks) std::printf("track %2d played: %.1f LUFS integrated\n", meteredTrack + 1, static_cast<double>(trackMeter.read().integrated));
                trackMeter.reset();
                meteredTrack = at;
            }
            trackMeter.process(L.data(), R.data(), n);
            for (int i = 0; i < n; ++i) peak = std::max(peak, static_cast<double>(std::max(std::fabs(L[static_cast<size_t>(i)]), std::fabs(R[static_cast<size_t>(i)]))));
            if (!out.empty()) wav.write(L.data(), R.data(), n);
            if (audMeter != nullptr) {
                const int ti = composer.trackOfBar(params, std::max(0, blockBar));
                const TrackPlan tp = composer.track(params, ti);
                const int si = sectionOfBar(tp.form, std::max(0, blockBar - tp.firstBar));
                if (ti != audTrack || si != audSection) { flushAudibility(blockBar - 1); audTrack = ti; audSection = si; audFirstBar = blockBar; }
                const float* sl[kNumStems];
                const float* srr[kNumStems];
                for (int s = 0; s < kNumStems; ++s) { sl[s] = stemBufL[static_cast<size_t>(s)].data(); srr[s] = stemBufR[static_cast<size_t>(s)].data(); }
                audMeter->add(sl, srr, n);
            }
            if (!stemWav.empty() && stemWritten < totalSamples) {
                const int m = static_cast<int>(std::min<uint64_t>(static_cast<uint64_t>(n), totalSamples - stemWritten));
                for (int s = 0; s < kNumStems; ++s) stemWav[static_cast<size_t>(s)]->write(stemBufL[static_cast<size_t>(s)].data(), stemBufR[static_cast<size_t>(s)].data(), m);
                stemWritten += static_cast<uint64_t>(m);
            }
            // The bench's excerpts: whatever part of this block falls into an open window, faded at the ends.
            const uint64_t blockEnd = renderedSamples + static_cast<uint64_t>(n);
            for (auto& e : excerptList) {
                if (e->closed || e->start >= blockEnd || e->end <= renderedSamples) continue;
                if (!e->opened) {
                    if (!e->wav.open(e->path.c_str(), sr, 2, pcm24 ? WavFormat::Pcm24 : WavFormat::Float32)) {
                        std::fprintf(stderr, "cannot write %s\n", e->path.c_str());
                        return 1;
                    }
                    e->opened = true;
                }
                const uint64_t a = std::max(e->start, renderedSamples), b = std::min(e->end, blockEnd);
                for (uint64_t i = a; i < b; ++i) {
                    const uint64_t in = i - e->start, left = e->end - i;
                    const float g = static_cast<float>(std::min<double>({ 1.0, static_cast<double>(in) / static_cast<double>(excerptFade),
                                                                           static_cast<double>(left) / static_cast<double>(excerptFade) }));
                    exL[static_cast<size_t>(i - a)] = g * L[static_cast<size_t>(i - renderedSamples)];
                    exR[static_cast<size_t>(i - a)] = g * R[static_cast<size_t>(i - renderedSamples)];
                }
                e->wav.write(exL.data(), exR.data(), static_cast<int>(b - a));
                if (b == e->end) { e->wav.close(); e->closed = true; }
            }
        }
        renderedSamples += static_cast<uint64_t>(n);
        if (!mixLog.empty()) grLog.push_back({ renderedSamples, engine->limiterReduction(), engine->compReduction() });
    }
    for (auto& e : excerptList) if (e->opened && !e->closed) { e->wav.close(); e->closed = true; }
    if (!mixLog.empty()) {
        FILE* f = std::fopen(mixLog.c_str(), "w");
        if (f == nullptr) { std::fprintf(stderr, "cannot write %s\n", mixLog.c_str()); return 1; }
        const double lat = static_cast<double>(std::max(0, engine->latencySamples()));
        const std::vector<SectionMark> marks = composer.sections(params, totalBars);
        size_t mk = 0;
        const char* sec = "Intro";
        int trk = 0;
        for (int b = 0; b < totalBars; ++b) {
            while (mk < marks.size() && marks[mk].beat < (b + 1) * kBeatsPerBar - 1e-9 && static_cast<int>(marks[mk].beat / kBeatsPerBar) <= b) {
                sec = kSectionNames[static_cast<int>(marks[mk].type)];
                trk = marks[mk].track;
                ++mk;
            }
            std::fprintf(f, "bar\t%d\t%.0f\t%d\t%s\n", b, tempo.secondsAt(b * kBeatsPerBar) * sr + lat, trk, sec);
        }
        for (const NoteEvent& e : recorded) {
            if (e.beat >= totalBeats) continue;
            const Part part = routedPart(e);
            if (part != Part::Kick && part != Part::Bass) continue;
            std::fprintf(f, "%s\t%.0f\n", part == Part::Kick ? "kick" : "bass", tempo.secondsAt(e.beat) * sr + lat);
        }
        for (const GrPoint& g : grLog) std::fprintf(f, "gr\t%llu\t%.3f\t%.3f\n", static_cast<unsigned long long>(g.at), static_cast<double>(g.limiter), static_cast<double>(g.comp));
        std::fclose(f);
    }
    engine->setStemTap(nullptr);
    for (auto& w : stemWav) w->close();
    if (audMeter != nullptr) { flushAudibility(totalBars - 1); std::fclose(audFile); }
    if (listTracks && !bench) std::printf("track %2d played: %.1f LUFS integrated\n", meteredTrack + 1, static_cast<double>(trackMeter.read().integrated));
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    wav.close();

    if (!midi.empty()) {
        Score score;
        score.tempo = tempo;
        score.keyRoot = composer.track(params, 0).key;
        score.scale = composer.track(params, 0).scale;
        // The key signature changes where the bass does: at the hand-over of the DJ overlap (19.09.2026).
        for (int t = 0; composer.track(params, t).firstBar < totalBars; ++t) {
            const TrackPlan p = composer.track(params, t);
            const double beat = static_cast<double>(handoverBar(p)) * kBeatsPerBar;
            if (t > 0 && p.key != composer.track(params, t - 1).key && beat < totalBeats) score.keyChanges.push_back(KeyChange{ beat, p.key });
        }
        score.sections = composer.sections(params, totalBars);
        for (const NoteEvent& e : recorded) if (e.beat < totalBeats) score.notes.push_back(e);
        score.sort();
        if (!writeMidiFile(score, midi.c_str())) { std::fprintf(stderr, "cannot write %s\n", midi.c_str()); return 1; }
    }

    if (!barLog.empty()) {
        FILE* f = std::fopen(barLog.c_str(), "w");
        if (f == nullptr) { std::fprintf(stderr, "cannot write %s\n", barLog.c_str()); return 1; }
        int snareLane = -1;
        for (int l = 0; l < kPercLanes; ++l)
            if (params.getBool(params.base(Module::Perc, l) + perc::Active) && params.getInt(params.base(Module::Perc, l) + perc::Role) == static_cast<int>(PercRole::Snare)) snareLane = l;
        struct BarNotes { int kicks = 0, bass = 0; unsigned lanes = 0, parts = 0, beat4Parts = 0, beat4Sfx = 0; int snare = 0; double snareGap = 0.0, lastSnare = -1.0; };
        std::vector<BarNotes> bn(static_cast<size_t>(totalBars) + 1);
        std::vector<NoteEvent> sorted = recorded;
        std::stable_sort(sorted.begin(), sorted.end(), [](const NoteEvent& a, const NoteEvent& b) { return a.beat < b.beat; });
        for (const NoteEvent& e : sorted) {
            const int b = static_cast<int>(e.beat / kBeatsPerBar);
            if (b < 0 || b >= totalBars) continue;
            BarNotes& x = bn[static_cast<size_t>(b)];
            const double inBar = e.beat - static_cast<double>(b) * kBeatsPerBar;
            const Part part = routedPart(e);
            x.parts |= 1u << static_cast<int>(part);
            if (part == Part::Kick) ++x.kicks;
            if (part == Part::Bass) ++x.bass;
            if (part == Part::Perc) x.lanes |= 1u << e.lane;
            if (part == Part::Perc && static_cast<int>(e.lane) == snareLane) {
                ++x.snare;
                if (x.lastSnare >= 0.0) x.snareGap = x.snareGap > 0.0 ? std::min(x.snareGap, e.beat - x.lastSnare) : e.beat - x.lastSnare;
                x.lastSnare = e.beat;
            }
            if (inBar >= 3.0 - 1e-6) {
                x.beat4Parts |= 1u << static_cast<int>(part);
                if (e.part == Part::Sfx || e.part == Part::Vocal || e.part == Part::Texture) x.beat4Sfx |= 1u << std::clamp(static_cast<int>(e.pitch) - kSfxBaseNote, 0, 31);
            }
        }
        // One line per bar and sounding track: over the DJ overlap a bar has two lines, the outgoing track's
        // (owner 1) and the incoming one's (owner 0). The score columns are the bar's, whoever wrote them.
        std::fprintf(f, "bar\ttrack\tintrack\towner\tsection\tsecindex\tbarinsec\tclimax\tbody\tpower_db\tkicks\tbass\tlanes\tparts\tsnare\tsnaregap\tbeat4parts\tbeat4sfx\tlayers\tplanparts\tflags\n");
        for (int ti = 0; composer.track(params, ti).firstBar < totalBars; ++ti)
        for (int b = composer.track(params, ti).firstBar; b < std::min(totalBars, composer.track(params, ti).firstBar + composer.track(params, ti).bars); ++b) {
            const TrackPlan t = composer.track(params, ti);
            const int inTrack = b - t.firstBar;
            const int si = sectionOfBar(t.form, inTrack);
            const Section& sec = t.form.section[si];
            const int owner = composer.trackOfBar(params, b) == ti ? 1 : 0;
            const double ms = barSamples[static_cast<size_t>(b)] > 0 ? barPower[static_cast<size_t>(b)] / static_cast<double>(barSamples[static_cast<size_t>(b)]) : 0.0;
            const BarNotes& x = bn[static_cast<size_t>(b)];
            PartAvailability av;
            for (int k = 0; k < kMelodyParts; ++k) av.part[k] = t.melody.present[k];
            av.percLayers = t.perc.layers;
            av.hatLayers = t.perc.hatLayers;
            const BarPlan bp = planBar(t.form, av, t.sectionSeed, inTrack);
            // Planned flags: 1 quiet hats, 2 shaker, 4 off-beat hat, 8 open hats, 16 ride, 32 crash, 64 floor silent.
            const unsigned flags = (bp.quietHats ? 1u : 0u) | (bp.shaker ? 2u : 0u) | (bp.offbeatHat ? 4u : 0u) | (bp.openHats ? 8u : 0u)
                                 | (bp.ride ? 16u : 0u) | (bp.crash ? 32u : 0u) | (bp.floorSilent ? 64u : 0u);
            std::fprintf(f, "%d\t%d\t%d\t%d\t%s\t%d\t%d\t%d\t%d\t%.3f\t%d\t%d\t%u\t%u\t%d\t%.4f\t%u\t%u\t%d\t%u\t%u\n", b, ti, inTrack, owner,
                         kSectionNames[static_cast<int>(sec.type)], si, inTrack - sec.startBar, sec.climax ? 1 : 0, t.form.body,
                         ms > 0.0 ? 10.0 * std::log10(ms) : -200.0, x.kicks, x.bass, x.lanes, x.parts, x.snare, x.snareGap,
                         x.beat4Parts, x.beat4Sfx, bp.percLayers, static_cast<unsigned>(bp.parts), flags);
        }
        std::fclose(f);
    }

    const double audioSeconds = static_cast<double>(totalSamples) / sr;
    if (bench || report) {
        std::printf("path %s, quality %s, %.1f s of audio in %.3f s: %.1fx realtime (%.2f %% of a core)\n",
                    kVecPathName, quality.name(), audioSeconds, elapsed, audioSeconds / elapsed, 100.0 * elapsed / audioSeconds);
    }
    if (report && !bench) {
        const LoudnessReading r = meter.read();
        std::printf("integrated %.1f LUFS, short-term %.1f LUFS, range %.1f LU, true peak %.2f dBTP, sample peak %.2f dBFS\n",
                    static_cast<double>(r.integrated), static_cast<double>(r.shortTerm), static_cast<double>(r.range),
                    static_cast<double>(r.truePeak), 20.0 * std::log10(std::max(peak, 1e-12)));
        std::printf("kick end pitch %.2f Hz, %.2f bars\n", static_cast<double>(engine->kick().tunedEndHz()), totalBeats / kBeatsPerBar);
        if (!midi.empty()) std::printf("MIDI: %zu events written to %s\n", recorded.size(), midi.c_str());
    }
    return 0;
}
