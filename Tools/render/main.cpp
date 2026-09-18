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
 *     --solo PART         mute everything else: kick, bass, perc, acid, lead, arp, pad or sfx
 *     --out FILE.wav      write the audio (32-bit float unless --pcm24)
 *     --pcm24             write 24-bit PCM
 *     --midi FILE.mid     write the score as a Standard MIDI File
 *     --report            print loudness, peak and timing
 *     --bench             render without writing and report the realtime factor
 *     --list              print every parameter with range and default
 *     --version           print the version, the vector path and the wavetable pack, then exit
 * @endcode
 */
#include "phos/Composer.h"
#include "phos/Engine.h"
#include "phos/Loudness.h"
#include "phos/Midi.h"
#include "phos/Model.h"
#include "phos/Quality.h"
#include "phos/SetFile.h"
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
#include <chrono>
#include <cmath>
#include <cstdio>
#include <memory>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

using namespace phos;

namespace {

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
    std::string out, midi, solo, setFile, saveSet;
    bool report = false, bench = false, pcm24 = false, listTracks = false, listSections = false;
    Quality quality = Quality::desktop();
    double rampBeat = -1.0, rampBpm = 0.0;
    std::vector<std::string> sets;
    std::vector<std::pair<bool, std::string>> unitOps;   // (lock?, "unit:index")

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
    if (!solo.empty()) {
        static const char* const kSoloNames[] = { "kick", "bass", "perc", "acid", "lead", "arp", "pad", "sfx" };
        static const int kSoloMutes[] = { mix::KickMute, mix::BassMute, mix::PercMute, mix::AcidMute, mix::LeadMute, mix::ArpMute, mix::PadMute, mix::SfxMute };
        int which = -1;
        for (int k = 0; k < 8; ++k) if (solo == kSoloNames[k]) which = k;
        if (which < 0) { std::fprintf(stderr, "--solo wants kick, bass, perc, acid, lead, arp, pad or sfx\n"); return 2; }
        for (int k = 0; k < 8; ++k) params.set(mb + kSoloMutes[k], k == which ? 0.0f : 1.0f);
    }

    const int cb = params.base(Module::Compose);
    Composer composer(seed);
    // A set file carries the seed, the style, the arc, the knobs and the curation (locks, rerolls).
    // It is read before the command line's own --set assignments so that those still win.
    if (!setFile.empty()) {
        std::string err;
        if (!readSetFile(setFile.c_str(), composer, params, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
        for (const std::string& s : sets) if (!params.parseText(s, &err)) { std::fprintf(stderr, "%s\n", err.c_str()); return 2; }
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
    if (listTracks) {
        for (int t = 0; composer.track(params, t).firstBar < totalBars; ++t) {
            const TrackPlan p = composer.track(params, t);
            std::printf("track %2d  bar %5d  %3d bars  %5.1f BPM  %-2s %-17s  bass %-7s/%-7s gate %.2f  kick %s%s  gain %+.1f dB\n",
                        t + 1, p.firstBar, p.bars, p.bpm, kKeyNames[p.key], kScaleNames[p.scale],
                        kBassPatternNames[p.primaryPattern], kBassPatternNames[p.secondaryPattern], static_cast<double>(p.gate),
                        (p.kickEngine < 0 ? params.getInt(params.base(Module::Kick) + kick::Engine) : p.kickEngine) == 1 ? "resonant" : "sweep",
                        p.kickClip == 1 ? " hard" : "", static_cast<double>(p.gainDb));
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
            for (int c = 0; c < 4; ++c) std::printf(" %s", kRoman[m.chordDegree[c]]);
            std::printf(" (%d bars each); acid %s (%d steps%s), lead %s (osc %d), arp %s (%s)\n", m.chordBars,
                        m.present[0] ? "yes" : "no", m.acidSteps, m.acidSquelch == 1 ? ", squelch" : "", m.present[1] ? "yes" : "no", m.leadOsc,
                        m.present[2] ? "yes" : "no", kArpStyles[m.arpStyle]);
            std::printf("          form (%s body, arc %.2f..%.2f):", p.form.body == 0 ? "Full-On" : (p.form.body == 1 ? "Progressive" : "Goa"),
                        static_cast<double>(p.arcIn), static_cast<double>(p.arcOut));
            for (int s = 0; s < p.form.count; ++s)
                std::printf(" %s%d@%d(E%.2f)", kSectionNames[static_cast<int>(p.form.section[s].type)], p.form.section[s].bars,
                            p.form.section[s].startBar, static_cast<double>(p.form.section[s].energy));
            std::printf("\n          effects:");
            for (const SfxEvent& s : p.form.sfx) std::printf(" %s@%g", kSfxTypeNames[s.type], s.beat / kBeatsPerBar);
            std::printf("\n          part gains %+.1f / %+.1f / %+.1f / %+.1f dB, mix %.1f LUFS before the master offset %+.1f dB\n",
                        static_cast<double>(p.partGainDb[0]), static_cast<double>(p.partGainDb[1]), static_cast<double>(p.partGainDb[2]),
                        static_cast<double>(p.partGainDb[3]), p.mixLoudness, static_cast<double>(p.masterGainDb));
        }
    }
    if (listSections) {
        for (const SectionMark& s : composer.sections(params, totalBars))
            std::printf("bar %5d  track %2d  %-6s  energy %.2f\n", static_cast<int>(s.beat / kBeatsPerBar) + 1, s.track + 1,
                        kSectionNames[static_cast<int>(s.type)], static_cast<double>(s.energy));
    }
    const uint64_t totalSamples = static_cast<uint64_t>(std::llround(tempo.secondsAt(totalBeats) * sr));

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
    const auto t0 = std::chrono::steady_clock::now();
    uint64_t renderedSamples = 0;
    double peak = 0.0;
    // The loudness of each track on its own, so that a set can be checked for jumps between tracks.
    LoudnessMeter trackMeter;
    trackMeter.prepare(sr);
    int meteredTrack = 0;
    while (renderedSamples < totalSamples) {
        const int n = static_cast<int>(std::min<uint64_t>(static_cast<uint64_t>(block), totalSamples - renderedSamples));
        conductor.pump(params, 8.0 * kBeatsPerBar, midi.empty() ? nullptr : &recorded);
        engine->process(L.data(), R.data(), n);
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
        }
        renderedSamples += static_cast<uint64_t>(n);
    }
    if (listTracks && !bench) std::printf("track %2d played: %.1f LUFS integrated\n", meteredTrack + 1, static_cast<double>(trackMeter.read().integrated));
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    wav.close();

    if (!midi.empty()) {
        Score score;
        score.tempo = tempo;
        score.keyRoot = composer.track(params, 0).key;
        score.scale = composer.track(params, 0).scale;
        for (int t = 0; composer.track(params, t).firstBar < totalBars; ++t) {
            const TrackPlan p = composer.track(params, t);
            const double beat = static_cast<double>(p.firstBar) * kBeatsPerBar;
            if (t > 0 && p.key != composer.track(params, t - 1).key) score.keyChanges.push_back(KeyChange{ beat, p.key });
        }
        score.sections = composer.sections(params, totalBars);
        for (const NoteEvent& e : recorded) if (e.beat < totalBeats) score.notes.push_back(e);
        score.sort();
        if (!writeMidiFile(score, midi.c_str())) { std::fprintf(stderr, "cannot write %s\n", midi.c_str()); return 1; }
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
